#include "AssetLibrary.h"
#include "AssetRegistry.h"
#include "core/rendering/StandardMeshManager.h"
#include "core/rendering/MaterialManager.h"
#include "core/rendering/MaterialCache.h"
#include "editor/import/ModelLoader.h"
#include "core/rendering/TextureLoader.h"
#include "core/serialization/Serializer.h"
#include "editor/nodegraph/GraphSerializer.h"
#include "editor/ui/utility/TextureSlotPicker.h"
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <unordered_set>
#include "editor/Project.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include "editor/pipeline/MeshBin.h"
#include "core/particles/SpriteLoader.h"
#include "core/vfs/FileSystem.h"

namespace fs = std::filesystem;

// File-scope mesh cache for model loading - allows external invalidation on reimport
namespace {
    struct MeshCache {
        std::mutex m;
        std::unordered_map<std::string, std::vector<std::shared_ptr<Mesh>>> map;
        
        void Invalidate(const std::string& path) {
            std::lock_guard<std::mutex> lk(m);
            // Normalize path for lookup
            std::string normalized = path;
            std::replace(normalized.begin(), normalized.end(), '\\', '/');
            map.erase(normalized);
            // Also try without normalization in case it was stored differently
            map.erase(path);
        }
        
        void Clear() {
            std::lock_guard<std::mutex> lk(m);
            map.clear();
        }
    };
    static MeshCache s_MeshCache;
}

void AssetLibrary::RegisterAsset(const AssetReference& ref, AssetType type, const std::string& path, const std::string& name) {
    std::lock_guard<std::mutex> lk(m_Mutex);
    // Normalize forward slashes
    std::string normPath = path;
    std::replace(normPath.begin(), normPath.end(), '\\', '/');

    // If already registered with same mapping, skip noisy log and return
    auto it = m_Assets.find(ref.guid);
    if (it != m_Assets.end()) {
        bool samePath = (it->second.path == normPath);
        bool sameType = (it->second.type == type);
        if (samePath && sameType) {
            // Ensure reverse maps are consistent then exit quietly
            m_PathToGUID[normPath] = ref.guid;
            m_GUIDToPath[ref.guid] = normPath;
            return;
        }
        // If GUID exists but path changed (rename/move), update and keep entry data (like cached meshes)
        // Note: Only log once per unique path change to avoid spam
        static std::unordered_set<std::string> loggedPathChanges;
        std::string changeKey = ref.guid.ToString() + ":" + normPath;
        
        it->second.path = normPath;
        it->second.type = type;
        it->second.name = name;
        m_PathToGUID[normPath] = ref.guid;
        m_GUIDToPath[ref.guid] = normPath;
        
        if (loggedPathChanges.find(changeKey) == loggedPathChanges.end()) {
            std::cout << "[AssetLibrary] Updated asset path: " << name << " (GUID: " << ref.guid.ToString() << ") -> " << normPath << std::endl;
            loggedPathChanges.insert(changeKey);
            // Warn about potential GUID collision
            if (loggedPathChanges.size() > 10000) {
                loggedPathChanges.clear(); // Prevent memory growth
            }
        }
        return;
    }

    AssetEntry entry(ref, type, normPath, name);
    m_Assets[ref.guid] = entry;
    m_PathToGUID[normPath] = ref.guid;
    m_GUIDToPath[ref.guid] = normPath;

    std::cout << "[AssetLibrary] Registered asset: " << name << " (GUID: " << ref.guid.ToString() << ")" << std::endl;
}

void AssetLibrary::RegisterPathAlias(const ClaymoreGUID& guid, const std::string& altPath) {
    std::lock_guard<std::mutex> lk(m_Mutex);
    if (guid.high == 0 && guid.low == 0) return;
    std::string norm = altPath;
    std::replace(norm.begin(), norm.end(), '\\', '/');
    m_PathToGUID[norm] = guid;
    
    // Also update the reverse mapping if not already set
    // This ensures GetPathForGUID can find the path
    if (m_GUIDToPath.find(guid) == m_GUIDToPath.end()) {
        m_GUIDToPath[guid] = norm;
    }
}

void AssetLibrary::UnregisterAsset(const AssetReference& ref) {
    std::lock_guard<std::mutex> lk(m_Mutex);
    auto it = m_Assets.find(ref.guid);
    if (it != m_Assets.end()) {
        m_PathToGUID.erase(it->second.path);
        m_GUIDToPath.erase(ref.guid);
        m_Assets.erase(it);
    }
}

AssetEntry* AssetLibrary::GetAsset(const AssetReference& ref) {
    std::lock_guard<std::mutex> lk(m_Mutex);
    auto it = m_Assets.find(ref.guid);
    return it != m_Assets.end() ? &it->second : nullptr;
}

AssetEntry* AssetLibrary::GetAsset(const ClaymoreGUID& guid) {
    std::lock_guard<std::mutex> lk(m_Mutex);
    auto it = m_Assets.find(guid);
    return it != m_Assets.end() ? &it->second : nullptr;
}

AssetEntry* AssetLibrary::GetAsset(const std::string& path) {
    // Normalize slashes first (no lock needed)
    std::string norm = path;
    std::replace(norm.begin(), norm.end(), '\\', '/');

    // Use helper which handles locking internally
    ClaymoreGUID guid = GetGUIDForPath(norm);
    if (guid.high != 0 || guid.low != 0) {
        // This overload also locks internally
        return GetAsset(guid);
    }

    // Fallback: check direct map under lock
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        auto it = m_PathToGUID.find(norm);
        if (it != m_PathToGUID.end()) {
            auto it2 = m_Assets.find(it->second);
            return it2 != m_Assets.end() ? &it2->second : nullptr;
        }
    }
    return nullptr;
}

bool AssetLibrary::LoadPrefabIntoEntity(const AssetReference& ref, EntityData& outEntity, Scene& scene) {
    AssetEntry* entry = this->GetAsset(ref);
    if (!entry) return false;
    if (entry->type != AssetType::Prefab) return false;
    // Prefer new authoring prefab JSON: load minimal and inject via PrefabAPI when available.
    // For now, keep legacy fallback for compatibility.
    try {
        std::ifstream in(entry->path);
        if (in) {
            nlohmann::json j; in >> j; in.close();
            if (j.is_object() && j.contains("guid") && j.contains("entities")) {
                // Create a placeholder entity with the prefab name
                outEntity.Name = j.value("name", std::string("Prefab"));
                return true;
            }
        }
    } catch(...) {}
    return Serializer::LoadPrefabFromFile(entry->path, outEntity, scene);
}

// Optional helper to load node graph assets into memory (authoring or runtime usage)
static bool LoadNodeGraphInternal(const AssetEntry& entry, nodegraph::GraphAsset& out)
{
    try {
        return nodegraph::GraphSerializer::LoadFromFile(entry.path, out);
    } catch(...) { return false; }
}

std::shared_ptr<Mesh> AssetLibrary::LoadMesh(const AssetReference& ref) {
    if (AssetReference::IsPrimitiveGuid(ref.guid)) {
        AssetReference::PrimitiveType type = AssetReference::PrimitiveTypeFromGuid(ref.guid);
        const char* primName = AssetReference::PrimitiveTypeToString(type);
        std::string name = primName ? primName : std::string("Cube");
        return CreatePrimitiveMesh(name);
    }
    AssetEntry* entry = GetAsset(ref);
    if (!entry) {
        std::cout << "[AssetLibrary] Warning: Asset not found for GUID: " << ref.guid.ToString() << std::endl;
        return nullptr;
    }

    // For imported models, support submesh selection via fileID
    if (!entry->path.empty()) {
        // Fast-path: .meshbin or .meta
        std::string ext = std::filesystem::path(entry->path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".meshbin") {
            return LoadMeshBin(entry->path, ref.fileID);
        }
        if (ext == ".meta") {
            // Prefer fast path via cached .meshbin
            std::string meshBin = ResolveMeshBinFromMeta(entry->path, ref.fileID);
            if (!meshBin.empty()) {
                if (auto fast = LoadMeshBin(meshBin, ref.fileID)) return fast;
                try { std::cout << "[AssetLibrary] LoadMeshBin failed for path=" << meshBin << " fileID=" << ref.fileID << std::endl; } catch(...) {}
            } else {
                try { std::cout << "[AssetLibrary] ResolveMeshBinFromMeta returned empty for " << entry->path << std::endl; } catch(...) {}
            }
            // No meshbin available: do not fall back to Assimp during scene load.
            return nullptr;
        }
        // Guard against re-entrant loads and races: use file-scope cache that can be invalidated
        std::vector<std::shared_ptr<Mesh>> meshListLocal;
        {
            std::lock_guard<std::mutex> lk(s_MeshCache.m);
            auto& meshList = s_MeshCache.map[entry->path];
            if (meshList.empty()) {
                // Load outside lock
            } else {
                meshListLocal = meshList;
            }
        }
        if (meshListLocal.empty()) {
            Model model = ModelLoader::LoadModel(entry->path);
            meshListLocal = model.Meshes;
            std::lock_guard<std::mutex> lk2(s_MeshCache.m);
            s_MeshCache.map[entry->path] = meshListLocal;
        }
        int idx = std::max(0, ref.fileID);
        if (idx < (int)meshListLocal.size()) {
            return meshListLocal[idx];
        }
        std::cout << "[AssetLibrary] Warning: fileID " << ref.fileID << " out of range for model: " << entry->path << std::endl;
        return meshListLocal.empty() ? nullptr : meshListLocal[0];
    }

    return nullptr;
}

std::shared_ptr<Material> AssetLibrary::LoadMaterial(const AssetReference& ref) {
    // Double-checked locking: avoid holding the mutex during heavy work
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        auto it = m_Assets.find(ref.guid);
        if (it == m_Assets.end()) return nullptr;
        if (it->second.material) return it->second.material;
    }
    auto created = MaterialManager::Instance().CreateSceneDefaultMaterial(&Scene::Get());
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        auto it = m_Assets.find(ref.guid);
        if (it == m_Assets.end()) return created;
        if (!it->second.material) it->second.material = created;
        return it->second.material;
    }
}

std::shared_ptr<bgfx::TextureHandle> AssetLibrary::LoadTexture(const AssetReference& ref) {
    std::string path;
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        auto it = m_Assets.find(ref.guid);
        if (it == m_Assets.end()) return nullptr;
        if (it->second.texture) return it->second.texture;
        path = it->second.path;
    }
    if (!path.empty()) {
        TextureSpecifier spec;
        spec.Path = path;
        bgfx::TextureHandle handle = AcquireTextureHandle(spec, TextureColorSpace::Linear);
        auto texPtr = std::make_shared<bgfx::TextureHandle>(handle);
        std::lock_guard<std::mutex> lk2(m_Mutex);
        auto it = m_Assets.find(ref.guid);
        if (it == m_Assets.end()) return texPtr;
        if (!it->second.texture) it->second.texture = texPtr;
        return it->second.texture;
    }
    return nullptr;
}

std::shared_ptr<Mesh> AssetLibrary::CreatePrimitiveMesh(const std::string& primitiveType) {
    std::lock_guard<std::mutex> lk(m_Mutex);
    // Check if we already have this primitive cached
    auto it = m_PrimitiveMeshes.find(primitiveType);
    if (it != m_PrimitiveMeshes.end()) {
        return it->second;
    }
    
    // Create the primitive mesh
    std::shared_ptr<Mesh> mesh;
    if (primitiveType == "Cube") {
        mesh = StandardMeshManager::Instance().GetCubeMesh();
    } else if (primitiveType == "Sphere") {
        mesh = StandardMeshManager::Instance().GetSphereMesh();
    } else if (primitiveType == "Plane") {
        mesh = StandardMeshManager::Instance().GetPlaneMesh();
    } else if (primitiveType == "Capsule") {
        mesh = StandardMeshManager::Instance().GetCapsuleMesh();
    } else {
        std::cout << "[AssetLibrary] Warning: Unknown primitive type: " << primitiveType << std::endl;
        mesh = StandardMeshManager::Instance().GetCubeMesh(); // Fallback to cube
    }
    
    // Cache the primitive mesh
    m_PrimitiveMeshes[primitiveType] = mesh;
    return mesh;
}

ClaymoreGUID AssetLibrary::GetGUIDForPath(const std::string& path) {
    std::lock_guard<std::mutex> lk(m_Mutex);
    // Normalize slashes
    std::string key = path;
    std::replace(key.begin(), key.end(), '\\', '/');
    // Direct lookup
    auto it = m_PathToGUID.find(key);
    if (it != m_PathToGUID.end()) return it->second;
    // If absolute under project, convert to project-relative
    try {
        std::filesystem::path proj = Project::GetProjectDirectory();
        if (!proj.empty()) {
            std::error_code ec;
            std::filesystem::path rel = std::filesystem::relative(key, proj, ec);
            if (!ec) {
                std::string v = rel.string();
                std::replace(v.begin(), v.end(), '\\', '/');
                auto it2 = m_PathToGUID.find(v);
                if (it2 != m_PathToGUID.end()) return it2->second;
            }
        }
    } catch(...) {}
    // If the string contains assets/, use substring from there
    size_t pos = key.find("assets/");
    if (pos != std::string::npos) {
        std::string v = key.substr(pos);
        auto it3 = m_PathToGUID.find(v);
        if (it3 != m_PathToGUID.end()) return it3->second;
    }
    return ClaymoreGUID();
}

std::string AssetLibrary::GetPathForGUID(const ClaymoreGUID& guid) {
    std::lock_guard<std::mutex> lk(m_Mutex);
    // First check the direct GUID->path mapping
    auto it = m_GUIDToPath.find(guid);
    if (it != m_GUIDToPath.end()) {
        return it->second;
    }
    
    // Fallback: check the asset entries (may have path even if m_GUIDToPath wasn't populated)
    auto ita = m_Assets.find(guid);
    if (ita != m_Assets.end() && !ita->second.path.empty()) {
        // Populate the reverse mapping for future lookups
        m_GUIDToPath[guid] = ita->second.path;
        return ita->second.path;
    }
    
    return "";
}

std::vector<std::tuple<std::string, ClaymoreGUID, AssetType>> AssetLibrary::GetAllAssets() const {
    std::lock_guard<std::mutex> lk(m_Mutex);
    std::vector<std::tuple<std::string, ClaymoreGUID, AssetType>> out;
    out.reserve(m_Assets.size());
    for (const auto& [guid, entry] : m_Assets) {
        out.emplace_back(entry.path, guid, entry.type);
    }
    return out;
}

void AssetLibrary::Clear() {
    std::lock_guard<std::mutex> lk(m_Mutex);
    m_Assets.clear();
    m_PathToGUID.clear();
    m_GUIDToPath.clear();
    m_PrimitiveMeshes.clear();
    // Also clear the model mesh cache
    s_MeshCache.Clear();
}

void AssetLibrary::InvalidateMesh(const std::string& path) {
    s_MeshCache.Invalidate(path);
}

void AssetLibrary::InvalidateMesh(const ClaymoreGUID& guid) {
    std::string path = GetPathForGUID(guid);
    if (!path.empty()) {
        s_MeshCache.Invalidate(path);
    }
}

void AssetLibrary::InvalidateAllMeshes() {
    s_MeshCache.Clear();
}

void AssetLibrary::PrintAllAssets() const {
    std::cout << "[AssetLibrary] Registered Assets:" << std::endl;
    std::lock_guard<std::mutex> lk(m_Mutex);
    for (const auto& pair : m_Assets) {
        const AssetEntry& entry = pair.second;
        std::cout << "  - " << entry.name << " (GUID: " << entry.reference.guid.ToString() 
                  << ", Path: " << entry.path << ")" << std::endl;
    }
} 

// ----------------------------------------------
// Fast-path helpers for .meta/.meshbin
// ----------------------------------------------
std::string AssetLibrary::ResolveMeshBinFromMeta(const std::string& metaPath, int /*fileID*/) {
    auto debugLog = [&](const std::string& msg){ try { std::cout << "[AssetLibrary] ResolveMeshBinFromMeta: " << msg << " path=" << metaPath << std::endl; } catch(...) {} };
    try {
        nlohmann::json j;
        // Try direct open first
        {
            std::ifstream in(metaPath);
            if (in.is_open()) { in >> j; in.close(); }
        }
        // If direct open failed or j is null, try virtual FS
        if (j.is_null()) {
            std::vector<uint8_t> bytes;
            if (FileSystem::Instance().ReadFile(metaPath, bytes) && !bytes.empty()) {
                try { j = nlohmann::json::parse(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size())); }
                catch(...) { /* ignore */ }
            }
        }
        // If still empty, try project-relative path
        if (j.is_null()) {
            try {
                fs::path proj = Project::GetProjectDirectory();
                if (!proj.empty()) {
                    fs::path candidate = proj / metaPath;
                    std::ifstream in2(candidate.string());
                    if (in2.is_open()) { in2 >> j; in2.close(); }
                }
            } catch(...) {}
        }

        if (!j.is_object()) { debugLog("failed to open/parse meta json"); return {}; }

        // 1) Preferred: meshes[0].mesh string
        if (j.contains("meshes") && j["meshes"].is_array() && !j["meshes"].empty()) {
            std::string m = j["meshes"][0].value("mesh", std::string());
            if (!m.empty()) {
                auto pos = m.find('#');
                if (pos != std::string::npos) m = m.substr(0, pos);
                debugLog(std::string("resolved via meshes[0].mesh -> ") + m);
                return m;
            }
        }
        // 2) Alternate: top-level "mesh" or "meshbin"
        if (j.contains("mesh") && j["mesh"].is_string()) {
            std::string m = j["mesh"].get<std::string>();
            auto pos = m.find('#'); if (pos != std::string::npos) m = m.substr(0, pos);
            if (!m.empty()) { debugLog(std::string("resolved via top-level mesh -> ") + m); return m; }
        }
        if (j.contains("meshbin") && j["meshbin"].is_string()) {
            std::string m = j["meshbin"].get<std::string>();
            auto pos = m.find('#'); if (pos != std::string::npos) m = m.substr(0, pos);
            if (!m.empty()) { debugLog(std::string("resolved via top-level meshbin -> ") + m); return m; }
        }
        // 3) Heuristic: same directory, same stem, .meshbin
        {
            fs::path p(metaPath);
            // Try project-relative join if relative
            if (p.is_relative()) {
                try { fs::path proj = Project::GetProjectDirectory(); if (!proj.empty()) p = proj / p; } catch(...) {}
            }
            fs::path candidate = p; candidate.replace_extension(".meshbin");
            std::error_code ec; if (fs::exists(candidate, ec)) { debugLog(std::string("resolved via sibling .meshbin -> ") + candidate.string()); return candidate.string(); }
        }
        // 4) From "source" or "processedPath": try stem replacement
        {
            fs::path p(metaPath); if (p.is_relative()) { try { fs::path proj = Project::GetProjectDirectory(); if (!proj.empty()) p = proj / p; } catch(...) {}
            }
            fs::path dir = p.parent_path();
            std::string stem;
            if (j.contains("source") && j["source"].is_string()) { stem = fs::path(j["source"].get<std::string>()).stem().string(); }
            if (stem.empty() && j.contains("sourcePath") && j["sourcePath"].is_string()) { stem = fs::path(j["sourcePath"].get<std::string>()).stem().string(); }
            if (stem.empty() && j.contains("processedPath") && j["processedPath"].is_string()) { stem = fs::path(j["processedPath"].get<std::string>()).stem().string(); }
            if (!stem.empty()) {
                fs::path candidate = dir / (stem + ".meshbin");
                std::error_code ec; if (fs::exists(candidate, ec)) { debugLog(std::string("resolved via stem heuristic -> ") + candidate.string()); return candidate.string(); }
            }
        }
        debugLog("no meshbin resolution found");
    } catch(...) { debugLog("exception during resolution"); }
    return {};
}

std::shared_ptr<Mesh> AssetLibrary::LoadMeshBin(const std::string& meshBinPath, int fileID) {
    try {
        bool skinned = false;
        uint32_t sub = (uint32_t)std::max(0, fileID);
        auto m = meshbin::ReadMeshFromBin(meshBinPath, sub, skinned);
        return m;
    } catch(...) {
        return nullptr;
    }
}

std::unique_ptr<BlendShapeComponent> AssetLibrary::LoadMeshBlendShapes(const AssetReference& ref)
{
    AssetEntry* entry = GetAsset(ref);
    if (!entry)
    {
        return nullptr;
    }

    auto resolveMeshbinPath = [&](const std::string& sourcePath) -> std::string
    {
        if (sourcePath.empty())
        {
            return {};
        }
        std::string ext = std::filesystem::path(sourcePath).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".meshbin")
        {
            return sourcePath;
        }
        if (ext == ".meta")
        {
            return ResolveMeshBinFromMeta(sourcePath, ref.fileID);
        }

        std::filesystem::path candidate = sourcePath;
        candidate.replace_extension(".meshbin");
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec))
        {
            return candidate.string();
        }
        return {};
    };

    std::string meshPath = resolveMeshbinPath(entry->path);
    if (meshPath.empty())
    {
        return nullptr;
    }

    uint32_t fileId = static_cast<uint32_t>(std::max(0, ref.fileID));
    return meshbin::ReadBlendShapesFromBin(meshPath, fileId);
}

//------------------------------------------------------------------------------
// Audio Path Resolution
//------------------------------------------------------------------------------

std::string AssetLibrary::GetAudioPath(const AssetReference& ref) {
    return GetAudioPath(ref.guid);
}

std::string AssetLibrary::GetAudioPath(const ClaymoreGUID& guid) {
    std::lock_guard<std::mutex> lk(m_Mutex);
    
    auto it = m_Assets.find(guid);
    if (it == m_Assets.end()) {
        return {};
    }
    
    const AssetEntry& entry = it->second;
    if (entry.type != AssetType::Audio) {
        std::cerr << "[AssetLibrary] GetAudioPath: Asset is not audio type: " 
                  << entry.path << std::endl;
        return {};
    }
    
    return entry.path;
}

//------------------------------------------------------------------------------
// Texture Cache Invalidation
//------------------------------------------------------------------------------

void AssetLibrary::InvalidateTexture(const std::string& path) {
    if (path.empty()) return;
    
    // Normalize path
    std::string normPath = path;
    std::replace(normPath.begin(), normPath.end(), '\\', '/');
    
    // Find GUID for this path
    ClaymoreGUID guid;
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        auto it = m_PathToGUID.find(normPath);
        if (it != m_PathToGUID.end()) {
            guid = it->second;
        }
    }
    
    // If found by GUID, invalidate that entry
    if (guid.high != 0 || guid.low != 0) {
        InvalidateTexture(guid);
    }
    
    // Also invalidate in MaterialCache (handles path-keyed cache)
    InvalidateTextureCache(normPath);
    InvalidateTextureCache(path); // Also try original path
    particles::InvalidateSpriteCache(normPath);
    particles::InvalidateSpriteCache(path);
    
    // Invalidate thumbnail cache in texture picker
    texturepicker::InvalidateThumbnail(normPath);
    texturepicker::InvalidateThumbnail(path);
    
    std::cout << "[AssetLibrary] Invalidated texture: " << normPath << std::endl;
}

void AssetLibrary::InvalidateTexture(const ClaymoreGUID& guid) {
    if (guid.high == 0 && guid.low == 0) return;
    
    std::string texturePath;
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        auto it = m_Assets.find(guid);
        if (it != m_Assets.end()) {
            // Clear the cached texture handle
            if (it->second.texture) {
                if (bgfx::isValid(*it->second.texture)) {
                    // Don't destroy here. Other live materials may still be using
                    // the same bgfx handle; dropping our shared_ptr is enough.
                }
                it->second.texture.reset();
            }
            texturePath = it->second.path;
        }
    }
    
    // Also invalidate in MaterialCache
    if (!texturePath.empty()) {
        InvalidateTextureCache(texturePath);
        particles::InvalidateSpriteCache(texturePath);
    }
}
