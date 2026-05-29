#include "ImposterManager.h"
#include "core/ecs/Scene.h"
#include "core/ecs/EntityData.h"
#include "core/prefab/PrefabAsset.h"
#ifndef CLAYMORE_RUNTIME
#include "core/prefab/PrefabAPI.h"
#include "editor/pipeline/AssetLibrary.h"
#include "editor/Project.h"
#endif
#include "core/rendering/ShaderManager.h"
#include "core/rendering/Renderer.h"
#include "core/rendering/Material.h"
#include "core/rendering/Mesh.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <unordered_set>
#include <glm/gtc/type_ptr.hpp>

namespace cm {
namespace resourcelayer {

namespace {
    // Track GUIDs that have already failed to prevent log spam
    std::unordered_set<uint64_t> s_FailedBakeAttempts;
    
    uint64_t HashGUID(const ClaymoreGUID& g) {
        return g.high ^ (g.low << 1);
    }
    
#ifndef CLAYMORE_RUNTIME
    // Get cache path for imposter data
    std::filesystem::path GetImposterCachePath(const ClaymoreGUID& guid) {
        auto projectDir = Project::GetProjectDirectory();
        if (projectDir.empty()) return {};
        
        std::filesystem::path cacheDir = projectDir / ".bin" / "imposters";
        std::filesystem::create_directories(cacheDir);
        return cacheDir / (guid.ToString() + ".imposter");
    }
    
    // Imposter cache file format (binary):
    // - Magic: "IMPL" (4 bytes)
    // - Version: uint32_t
    // - BoundsMin: 3 floats
    // - BoundsMax: 3 floats
    // - VertexCount: uint32_t
    // - IndexCount: uint32_t
    // - Vertices: VertexCount * (3+3+2+4 floats = 48 bytes per vertex)
    // - Indices: IndexCount * uint32_t
    
    constexpr uint32_t IMPOSTER_MAGIC = 0x4C504D49; // "IMPL"
    constexpr uint32_t IMPOSTER_VERSION = 1;
    
    bool SaveImposterToCache(
        const ClaymoreGUID& guid,
        const std::vector<glm::vec3>& positions,
        const std::vector<glm::vec3>& normals,
        const std::vector<glm::vec2>& uvs,
        const std::vector<glm::vec4>& tangents,
        const std::vector<uint32_t>& indices,
        const glm::vec3& boundsMin,
        const glm::vec3& boundsMax)
    {
        auto path = GetImposterCachePath(guid);
        if (path.empty()) return false;
        
        std::ofstream file(path, std::ios::binary);
        if (!file) return false;
        
        // Header
        file.write(reinterpret_cast<const char*>(&IMPOSTER_MAGIC), sizeof(IMPOSTER_MAGIC));
        file.write(reinterpret_cast<const char*>(&IMPOSTER_VERSION), sizeof(IMPOSTER_VERSION));
        file.write(reinterpret_cast<const char*>(&boundsMin), sizeof(boundsMin));
        file.write(reinterpret_cast<const char*>(&boundsMax), sizeof(boundsMax));
        
        uint32_t vertexCount = static_cast<uint32_t>(positions.size());
        uint32_t indexCount = static_cast<uint32_t>(indices.size());
        file.write(reinterpret_cast<const char*>(&vertexCount), sizeof(vertexCount));
        file.write(reinterpret_cast<const char*>(&indexCount), sizeof(indexCount));
        
        // Vertex data interleaved
        for (size_t i = 0; i < vertexCount; ++i) {
            file.write(reinterpret_cast<const char*>(&positions[i]), sizeof(glm::vec3));
            file.write(reinterpret_cast<const char*>(&normals[i]), sizeof(glm::vec3));
            file.write(reinterpret_cast<const char*>(&uvs[i]), sizeof(glm::vec2));
            file.write(reinterpret_cast<const char*>(&tangents[i]), sizeof(glm::vec4));
        }
        
        // Indices
        file.write(reinterpret_cast<const char*>(indices.data()), indices.size() * sizeof(uint32_t));
        
        std::cout << "[ImposterManager] Saved imposter cache: " << path.string() << std::endl;
        return true;
    }
    
    bool LoadImposterFromCache(
        const ClaymoreGUID& guid,
        std::vector<glm::vec3>& outPositions,
        std::vector<glm::vec3>& outNormals,
        std::vector<glm::vec2>& outUVs,
        std::vector<glm::vec4>& outTangents,
        std::vector<uint32_t>& outIndices,
        glm::vec3& outBoundsMin,
        glm::vec3& outBoundsMax)
    {
        auto path = GetImposterCachePath(guid);
        if (path.empty() || !std::filesystem::exists(path)) return false;
        
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        
        // Header
        uint32_t magic, version;
        file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        file.read(reinterpret_cast<char*>(&version), sizeof(version));
        
        if (magic != IMPOSTER_MAGIC || version != IMPOSTER_VERSION) {
            return false;
        }
        
        file.read(reinterpret_cast<char*>(&outBoundsMin), sizeof(outBoundsMin));
        file.read(reinterpret_cast<char*>(&outBoundsMax), sizeof(outBoundsMax));
        
        uint32_t vertexCount, indexCount;
        file.read(reinterpret_cast<char*>(&vertexCount), sizeof(vertexCount));
        file.read(reinterpret_cast<char*>(&indexCount), sizeof(indexCount));
        
        outPositions.resize(vertexCount);
        outNormals.resize(vertexCount);
        outUVs.resize(vertexCount);
        outTangents.resize(vertexCount);
        outIndices.resize(indexCount);
        
        // Vertex data interleaved
        for (size_t i = 0; i < vertexCount; ++i) {
            file.read(reinterpret_cast<char*>(&outPositions[i]), sizeof(glm::vec3));
            file.read(reinterpret_cast<char*>(&outNormals[i]), sizeof(glm::vec3));
            file.read(reinterpret_cast<char*>(&outUVs[i]), sizeof(glm::vec2));
            file.read(reinterpret_cast<char*>(&outTangents[i]), sizeof(glm::vec4));
        }
        
        // Indices
        file.read(reinterpret_cast<char*>(outIndices.data()), indexCount * sizeof(uint32_t));
        
        std::cout << "[ImposterManager] Loaded imposter from cache: " << path.string() 
                  << " (" << vertexCount << " verts)" << std::endl;
        return file.good();
    }
#endif
}

//------------------------------------------------------------------------------
// ImposterCache Implementation
//------------------------------------------------------------------------------
void ImposterCache::Release() {
    if (bgfx::isValid(VBH)) {
        bgfx::destroy(VBH);
        VBH = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(IBH)) {
        bgfx::destroy(IBH);
        IBH = BGFX_INVALID_HANDLE;
    }
    // Don't destroy textures - they're owned by the material system
    AlbedoTexture = BGFX_INVALID_HANDLE;
    NormalTexture = BGFX_INVALID_HANDLE;
    ImposterMaterial.reset();
    IndexCount = 0;
    Valid = false;
}

ImposterCache::ImposterCache(ImposterCache&& other) noexcept
    : VBH(other.VBH)
    , IBH(other.IBH)
    , AlbedoTexture(other.AlbedoTexture)
    , NormalTexture(other.NormalTexture)
    , ImposterMaterial(std::move(other.ImposterMaterial))
    , IndexCount(other.IndexCount)
    , BoundsMin(other.BoundsMin)
    , BoundsMax(other.BoundsMax)
    , Valid(other.Valid)
{
    other.VBH = BGFX_INVALID_HANDLE;
    other.IBH = BGFX_INVALID_HANDLE;
    other.AlbedoTexture = BGFX_INVALID_HANDLE;
    other.NormalTexture = BGFX_INVALID_HANDLE;
    other.Valid = false;
}

ImposterCache& ImposterCache::operator=(ImposterCache&& other) noexcept {
    if (this != &other) {
        Release();
        VBH = other.VBH;
        IBH = other.IBH;
        AlbedoTexture = other.AlbedoTexture;
        NormalTexture = other.NormalTexture;
        ImposterMaterial = std::move(other.ImposterMaterial);
        IndexCount = other.IndexCount;
        BoundsMin = other.BoundsMin;
        BoundsMax = other.BoundsMax;
        Valid = other.Valid;
        
        other.VBH = BGFX_INVALID_HANDLE;
        other.IBH = BGFX_INVALID_HANDLE;
        other.AlbedoTexture = BGFX_INVALID_HANDLE;
        other.NormalTexture = BGFX_INVALID_HANDLE;
        other.Valid = false;
    }
    return *this;
}

//------------------------------------------------------------------------------
// ImposterManager Implementation
//------------------------------------------------------------------------------
ImposterManager& ImposterManager::Instance() {
    static ImposterManager instance;
    return instance;
}

bool ImposterManager::BakeImposter(const ClaymoreGUID& prefabGuid, ImposterCache& outCache) {
    outCache.Release();
    
#ifdef CLAYMORE_RUNTIME
    // Runtime builds don't support dynamic prefab loading for baking
    return false;
#else
    // Check if we've already failed this GUID
    uint64_t guidHash = HashGUID(prefabGuid);
    if (s_FailedBakeAttempts.count(guidHash) > 0) {
        return false;
    }
    
    // Mesh data arrays
    std::vector<glm::vec3> positions, normals;
    std::vector<glm::vec2> uvs;
    std::vector<glm::vec4> tangents;
    std::vector<uint32_t> indices;
    glm::vec3 boundsMin(std::numeric_limits<float>::max());
    glm::vec3 boundsMax(std::numeric_limits<float>::lowest());
    
    // Try to load from disk cache first
    bool loadedFromCache = LoadImposterFromCache(prefabGuid, positions, normals, uvs, tangents, indices, boundsMin, boundsMax);
    std::string prefabPath;
    
    if (!loadedFromCache) {
        // Not in cache - need to bake from prefab
        auto* assetEntry = AssetLibrary::Instance().GetAsset(prefabGuid);
        if (!assetEntry || assetEntry->type != AssetType::Prefab) {
            s_FailedBakeAttempts.insert(guidHash);
            return false;
        }
        prefabPath = assetEntry->path;
        
        // Load prefab JSON
        PrefabAsset prefab;
        if (!PrefabIO::LoadPrefab(assetEntry->path, prefab)) {
            std::cerr << "[ImposterManager] Failed to load prefab: " << assetEntry->path << std::endl;
            s_FailedBakeAttempts.insert(guidHash);
            return false;
        }
        
        // Collect mesh data from all entities in prefab hierarchy
        std::cout << "[ImposterManager] Collecting meshes from prefab with " 
                  << prefab.EntityCount() << " entities..." << std::endl;
        
        if (!CollectPrefabMeshes(prefab, positions, normals, uvs, tangents, indices, boundsMin, boundsMax)) {
            // No mesh data found - use a simple placeholder
            std::cerr << "[ImposterManager] WARNING: No mesh data found in prefab, using placeholder: " 
                      << assetEntry->path << std::endl;
            std::cerr << "[ImposterManager] Prefab entity count: " << prefab.EntityCount() << std::endl;
            
            // Debug: print first entity structure
            if (prefab.Entities.is_array() && !prefab.Entities.empty()) {
                std::cerr << "[ImposterManager] First entity keys: ";
                for (auto it = prefab.Entities[0].begin(); it != prefab.Entities[0].end(); ++it) {
                    std::cerr << it.key() << " ";
                }
                std::cerr << std::endl;
            }
            
            // Create a simple billboard quad as placeholder
            float s = 1.0f;
            positions = {{-s, 0, 0}, {s, 0, 0}, {s, s*2, 0}, {-s, s*2, 0}};
            normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
            uvs = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
            tangents = {{1, 0, 0, 1}, {1, 0, 0, 1}, {1, 0, 0, 1}, {1, 0, 0, 1}};
            indices = {0, 1, 2, 0, 2, 3};
            boundsMin = glm::vec3(-s, 0, -s);
            boundsMax = glm::vec3(s, s*2, s);
        } else {
            std::cout << "[ImposterManager] Successfully collected " << positions.size() 
                      << " vertices, " << indices.size() << " indices from prefab" << std::endl;
        }
        
        // Save to disk cache for next time
        if (!positions.empty()) {
            SaveImposterToCache(prefabGuid, positions, normals, uvs, tangents, indices, boundsMin, boundsMax);
        }
    }
    
    if (positions.empty()) {
        s_FailedBakeAttempts.insert(guidHash);
        return false;
    }
    
    // Create GPU vertex buffer
    // Layout MUST match PBRVertex: Position (3), Normal (3), UV (2) = 8 floats = 32 bytes
    // (No tangent - that's for advanced PBR which we don't use here)
    bgfx::VertexLayout layout;
    layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();
    
    size_t vertexCount = positions.size();
    size_t vertexSize = 8 * sizeof(float); // 3+3+2 = 8 floats per vertex
    const bgfx::Memory* vbMem = bgfx::alloc(static_cast<uint32_t>(vertexCount * vertexSize));
    
    float* vbData = reinterpret_cast<float*>(vbMem->data);
    for (size_t i = 0; i < vertexCount; ++i) {
        // Position
        *vbData++ = positions[i].x;
        *vbData++ = positions[i].y;
        *vbData++ = positions[i].z;
        
        // Normal
        glm::vec3 n = (i < normals.size()) ? normals[i] : glm::vec3(0, 1, 0);
        *vbData++ = n.x;
        *vbData++ = n.y;
        *vbData++ = n.z;
        
        // UV
        glm::vec2 uv = (i < uvs.size()) ? uvs[i] : glm::vec2(0);
        *vbData++ = uv.x;
        *vbData++ = uv.y;
    }
    
    outCache.VBH = bgfx::createVertexBuffer(vbMem, layout);
    
    // Create GPU index buffer
    const bgfx::Memory* ibMem = bgfx::alloc(static_cast<uint32_t>(indices.size() * sizeof(uint32_t)));
    std::memcpy(ibMem->data, indices.data(), indices.size() * sizeof(uint32_t));
    outCache.IBH = bgfx::createIndexBuffer(ibMem, BGFX_BUFFER_INDEX32);
    
    outCache.IndexCount = static_cast<uint32_t>(indices.size());
    outCache.BoundsMin = boundsMin;
    outCache.BoundsMax = boundsMax;
    outCache.Valid = bgfx::isValid(outCache.VBH) && bgfx::isValid(outCache.IBH);
    
    if (outCache.Valid) {
        std::string source = loadedFromCache ? "cache" : prefabPath;
        std::cout << "[ImposterManager] Created imposter from " << source 
                  << " (" << vertexCount << " verts, " << indices.size() << " indices)" << std::endl;
    }
    
    return outCache.Valid;
#endif
}

bool ImposterManager::CollectPrefabMeshes(
    const PrefabAsset& prefab,
    std::vector<glm::vec3>& outPositions,
    std::vector<glm::vec3>& outNormals,
    std::vector<glm::vec2>& outUVs,
    std::vector<glm::vec4>& outTangents,
    std::vector<uint32_t>& outIndices,
    glm::vec3& outBoundsMin,
    glm::vec3& outBoundsMax)
{
#ifdef CLAYMORE_RUNTIME
    return false;
#else
    outPositions.clear();
    outNormals.clear();
    outUVs.clear();
    outTangents.clear();
    outIndices.clear();
    outBoundsMin = glm::vec3(std::numeric_limits<float>::max());
    outBoundsMax = glm::vec3(std::numeric_limits<float>::lowest());
    
    bool foundMesh = false;
    
    // Helper to load mesh from GUID
    auto loadMeshFromGuid = [&](const ClaymoreGUID& guid, int fileID = 0) -> std::shared_ptr<Mesh> {
        if (guid.high == 0 && guid.low == 0) return nullptr;
        
        std::cout << "[ImposterManager]   Loading mesh from GUID: " << guid.ToString() 
                  << " fileID: " << fileID << std::endl;
        
        // First try direct LoadMesh
        AssetReference assetRef;
        assetRef.guid = guid;
        assetRef.fileID = fileID;
        
        auto mesh = AssetLibrary::Instance().LoadMesh(assetRef);
        if (mesh) {
            std::cout << "[ImposterManager]   Success! Mesh has " << mesh->Vertices.size() << " vertices" << std::endl;
            return mesh;
        }
        
        // If direct load failed, get the asset entry and try alternative paths
        AssetEntry* entry = AssetLibrary::Instance().GetAsset(guid);
        if (!entry) {
            std::cout << "[ImposterManager]   No asset entry found for GUID" << std::endl;
            return nullptr;
        }
        
        std::cout << "[ImposterManager]   Asset path: " << entry->path << " type: " << static_cast<int>(entry->type) << std::endl;
        
        // Get the extension
        std::string ext = std::filesystem::path(entry->path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        
        // If it's a model file (.fbx, .glb, etc), try to find the associated .meta and .meshbin
        if (ext == ".fbx" || ext == ".glb" || ext == ".gltf" || ext == ".obj") {
            // Look for .meta file
            std::filesystem::path metaPath = std::filesystem::path(entry->path).replace_extension(".meta");
            if (std::filesystem::exists(metaPath)) {
                std::cout << "[ImposterManager]   Found meta file: " << metaPath.string() << std::endl;
                std::string meshBin = AssetLibrary::Instance().ResolveMeshBinFromMeta(metaPath.string(), fileID);
                if (!meshBin.empty()) {
                    mesh = AssetLibrary::Instance().LoadMeshBin(meshBin, fileID);
                    if (mesh) {
                        std::cout << "[ImposterManager]   Loaded from meshbin: " << mesh->Vertices.size() << " vertices" << std::endl;
                        return mesh;
                    }
                }
            }
            
            // Look for .meshbin file directly alongside the model
            std::filesystem::path meshBinPath = std::filesystem::path(entry->path).replace_extension(".meshbin");
            if (std::filesystem::exists(meshBinPath)) {
                std::cout << "[ImposterManager]   Found meshbin directly: " << meshBinPath.string() << std::endl;
                mesh = AssetLibrary::Instance().LoadMeshBin(meshBinPath.string(), fileID);
                if (mesh) {
                    std::cout << "[ImposterManager]   Loaded from meshbin: " << mesh->Vertices.size() << " vertices" << std::endl;
                    return mesh;
                }
            }
        }
        
        // If it's a .meta file, resolve to meshbin
        if (ext == ".meta") {
            std::string meshBin = AssetLibrary::Instance().ResolveMeshBinFromMeta(entry->path, fileID);
            if (!meshBin.empty()) {
                mesh = AssetLibrary::Instance().LoadMeshBin(meshBin, fileID);
                if (mesh) {
                    std::cout << "[ImposterManager]   Loaded from meshbin via meta: " << mesh->Vertices.size() << " vertices" << std::endl;
                    return mesh;
                }
            }
        }
        
        std::cout << "[ImposterManager]   Could not load mesh from asset" << std::endl;
        return nullptr;
    };
    
    // Process a single entity with its world transform
    auto processEntity = [&](const nlohmann::json& entity, const glm::mat4& worldTransform) {
        if (!entity.is_object()) return;
        
        std::string entityName = entity.value("name", "unnamed");
        std::shared_ptr<Mesh> loadedMesh;
        
        // Debug: print ALL relevant fields for mesh resolution
        std::cout << "[ImposterManager] ========================================" << std::endl;
        std::cout << "[ImposterManager] Processing entity '" << entityName << "'" << std::endl;
        std::cout << "[ImposterManager]   modelAssetGuid: " 
                  << (entity.contains("modelAssetGuid") ? entity["modelAssetGuid"].dump() : "NOT PRESENT") << std::endl;
        std::cout << "[ImposterManager]   asset: " 
                  << (entity.contains("asset") ? entity["asset"].dump() : "NOT PRESENT") << std::endl;
        std::cout << "[ImposterManager]   prefabSource: " 
                  << (entity.contains("prefabSource") ? entity["prefabSource"].dump() : "NOT PRESENT") << std::endl;
        std::cout << "[ImposterManager]   prefabGuid: " 
                  << (entity.contains("prefabGuid") ? entity["prefabGuid"].dump() : "NOT PRESENT") << std::endl;
        std::cout << "[ImposterManager] ========================================" << std::endl;
        std::cout.flush();
        
        // Method 1: modelAssetGuid (direct model reference on entity - can be object or string)
        if (entity.contains("modelAssetGuid")) {
            std::cout << "[ImposterManager] Entity '" << entityName << "' has modelAssetGuid" << std::endl;
            try {
                ClaymoreGUID guid;
                const auto& guidField = entity["modelAssetGuid"];
                
                if (guidField.is_object()) {
                    if (guidField.contains("high") && guidField.contains("low")) {
                        guid.high = guidField["high"].get<uint64_t>();
                        guid.low = guidField["low"].get<uint64_t>();
                    } else if (guidField.contains("guid")) {
                        guid = ClaymoreGUID::FromString(guidField["guid"].get<std::string>());
                    }
                } else if (guidField.is_string()) {
                    guid = ClaymoreGUID::FromString(guidField.get<std::string>());
                } else {
                    entity.at("modelAssetGuid").get_to(guid);
                }
                
                std::cout << "[ImposterManager]   modelAssetGuid parsed: " << guid.ToString() << std::endl;
                if (guid.high != 0 || guid.low != 0) {
                    loadedMesh = loadMeshFromGuid(guid, 0);
                }
            } catch (const std::exception& e) {
                std::cerr << "[ImposterManager]   Exception parsing modelAssetGuid: " << e.what() << std::endl;
            }
        }
        
        // Method 2: asset (model asset reference with path, guid, type)
        if (!loadedMesh && entity.contains("asset") && entity["asset"].is_object()) {
            const auto& assetField = entity["asset"];
            std::string assetType = assetField.value("type", "");
            std::string assetPath = assetField.value("path", "");
            
            std::cout << "[ImposterManager] Entity '" << entityName << "' has asset reference" << std::endl;
            std::cout << "[ImposterManager]   asset.type: " << assetType << std::endl;
            std::cout << "[ImposterManager]   asset.path: " << assetPath << std::endl;
            
            if (assetType == "model" && !assetPath.empty()) {
                // This is the correct path! Use the .meta file to resolve the mesh
                std::filesystem::path projectDir = Project::GetProjectDirectory();
                std::filesystem::path metaPath = projectDir / assetPath;
                
                std::cout << "[ImposterManager]   Full meta path: " << metaPath.string() << std::endl;
                
                if (std::filesystem::exists(metaPath)) {
                    // Try to read GUID from meta and load mesh
                    try {
                        std::ifstream metaFile(metaPath);
                        nlohmann::json metaJson;
                        metaFile >> metaJson;
                        metaFile.close();
                        
                        if (metaJson.contains("guid")) {
                            ClaymoreGUID modelGuid;
                            metaJson.at("guid").get_to(modelGuid);
                            std::cout << "[ImposterManager]   Model GUID from meta: " << modelGuid.ToString() << std::endl;
                            loadedMesh = loadMeshFromGuid(modelGuid, 0);
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "[ImposterManager]   Exception reading model meta: " << e.what() << std::endl;
                    }
                    
                    // Also try ResolveMeshBinFromMeta directly
                    if (!loadedMesh) {
                        std::string meshBin = AssetLibrary::Instance().ResolveMeshBinFromMeta(metaPath.string(), 0);
                        if (!meshBin.empty()) {
                            std::cout << "[ImposterManager]   Resolved meshbin: " << meshBin << std::endl;
                            loadedMesh = AssetLibrary::Instance().LoadMeshBin(meshBin, 0);
                            if (loadedMesh) {
                                std::cout << "[ImposterManager]   Loaded from meshbin: " << loadedMesh->Vertices.size() << " vertices" << std::endl;
                            }
                        }
                    }
                } else {
                    std::cout << "[ImposterManager]   Meta file does not exist: " << metaPath.string() << std::endl;
                }
            }
            
            // Fallback: try GUID directly
            if (!loadedMesh && assetField.contains("guid")) {
                try {
                    ClaymoreGUID guid = ClaymoreGUID::FromString(assetField["guid"].get<std::string>());
                    std::cout << "[ImposterManager]   Trying asset GUID directly: " << guid.ToString() << std::endl;
                    if (guid.high != 0 || guid.low != 0) {
                        loadedMesh = loadMeshFromGuid(guid, 0);
                    }
                } catch (const std::exception& e) {
                    std::cerr << "[ImposterManager]   Exception parsing asset GUID: " << e.what() << std::endl;
                }
            }
        }
        
        // Method 3: mesh component (new format)
        if (!loadedMesh && entity.contains("mesh") && entity["mesh"].is_object()) {
            const auto& meshJson = entity["mesh"];
            
            std::cout << "[ImposterManager] Entity '" << entityName << "' has mesh component" << std::endl;
            
            // Try meshReference (GUID + fileID)
            if (meshJson.contains("meshReference") && meshJson["meshReference"].is_object()) {
                try {
                    const auto& ref = meshJson["meshReference"];
                    ClaymoreGUID guid;
                    if (ref.contains("guid") && ref["guid"].is_string()) {
                        guid = ClaymoreGUID::FromString(ref["guid"].get<std::string>());
                    } else if (ref.contains("high") && ref.contains("low")) {
                        guid.high = ref["high"].get<uint64_t>();
                        guid.low = ref["low"].get<uint64_t>();
                    }
                    loadedMesh = loadMeshFromGuid(guid, ref.value("fileID", 0));
                } catch (const std::exception& e) {
                    std::cerr << "[ImposterManager]   Exception loading mesh: " << e.what() << std::endl;
                }
            }
            
            // Try meshPath (string path)
            if (!loadedMesh && meshJson.contains("meshPath")) {
                std::string meshPath = meshJson["meshPath"].get<std::string>();
                std::cout << "[ImposterManager]   Trying meshPath: " << meshPath << std::endl;
                auto* meshAsset = AssetLibrary::Instance().GetAsset(meshPath);
                if (meshAsset && meshAsset->mesh) {
                    loadedMesh = meshAsset->mesh;
                    std::cout << "[ImposterManager]   Loaded from meshPath: " << loadedMesh->Vertices.size() << " vertices" << std::endl;
                }
            }
            
            // Try meshName (legacy)
            if (!loadedMesh && meshJson.contains("meshName")) {
                std::string meshName = meshJson["meshName"].get<std::string>();
                if (!meshName.empty()) {
                    std::cout << "[ImposterManager]   Trying meshName: " << meshName << std::endl;
                    auto* meshAsset = AssetLibrary::Instance().GetAsset(meshName);
                    if (meshAsset && meshAsset->mesh) {
                        loadedMesh = meshAsset->mesh;
                    }
                }
            }
        }
        
        // Method 4: components.MeshComponent (component-based format)
        if (!loadedMesh && entity.contains("components") && entity["components"].is_object()) {
            const auto& comps = entity["components"];
            if (comps.contains("MeshComponent") && comps["MeshComponent"].is_object()) {
                const auto& meshJson = comps["MeshComponent"];
                std::cout << "[ImposterManager] Entity '" << entityName << "' has components.MeshComponent" << std::endl;
                
                if (meshJson.contains("meshReference") && meshJson["meshReference"].is_object()) {
                    try {
                        const auto& ref = meshJson["meshReference"];
                        ClaymoreGUID guid;
                        if (ref.contains("guid") && ref["guid"].is_string()) {
                            guid = ClaymoreGUID::FromString(ref["guid"].get<std::string>());
                        } else if (ref.contains("high") && ref.contains("low")) {
                            guid.high = ref["high"].get<uint64_t>();
                            guid.low = ref["low"].get<uint64_t>();
                        }
                        loadedMesh = loadMeshFromGuid(guid, ref.value("fileID", 0));
                    } catch (...) {}
                }
            }
        }
        
        // Method 5: prefabSource (path to original model file)
        if (!loadedMesh && entity.contains("prefabSource") && entity["prefabSource"].is_string()) {
            std::string prefabSource = entity["prefabSource"].get<std::string>();
            if (!prefabSource.empty()) {
                std::cout << "[ImposterManager] Entity '" << entityName << "' has prefabSource: " << prefabSource << std::endl;
                
                std::filesystem::path projectDir = Project::GetProjectDirectory();
                
                // Try to find by path first
                auto* asset = AssetLibrary::Instance().GetAsset(prefabSource);
                if (asset) {
                    std::cout << "[ImposterManager]   Found asset at path, type: " << static_cast<int>(asset->type) << std::endl;
                    loadedMesh = AssetLibrary::Instance().LoadMesh(asset->reference);
                    if (loadedMesh) {
                        std::cout << "[ImposterManager]   Loaded mesh from prefabSource: " << loadedMesh->Vertices.size() << " vertices" << std::endl;
                    }
                }
                
                // Try to read GUID from the .meta sidecar file
                if (!loadedMesh) {
                    std::filesystem::path metaPath = projectDir / (prefabSource + ".meta");
                    std::cout << "[ImposterManager]   Looking for meta file: " << metaPath.string() << std::endl;
                    
                    if (std::filesystem::exists(metaPath)) {
                        try {
                            std::ifstream metaFile(metaPath);
                            nlohmann::json metaJson;
                            metaFile >> metaJson;
                            metaFile.close();
                            
                            if (metaJson.contains("guid")) {
                                ClaymoreGUID metaGuid;
                                metaJson.at("guid").get_to(metaGuid);
                                std::cout << "[ImposterManager]   Found GUID in meta: " << metaGuid.ToString() << std::endl;
                                loadedMesh = loadMeshFromGuid(metaGuid, 0);
                            }
                        } catch (const std::exception& e) {
                            std::cerr << "[ImposterManager]   Exception reading meta: " << e.what() << std::endl;
                        }
                    }
                }
                
                // Try .meshbin directly
                if (!loadedMesh) {
                    std::filesystem::path meshBinPath = std::filesystem::path(prefabSource).replace_extension(".meshbin");
                    std::filesystem::path fullMeshBin = projectDir / meshBinPath;
                    std::cout << "[ImposterManager]   Trying meshbin: " << fullMeshBin.string() << std::endl;
                    
                    if (std::filesystem::exists(fullMeshBin)) {
                        loadedMesh = AssetLibrary::Instance().LoadMeshBin(fullMeshBin.string(), 0);
                        if (loadedMesh) {
                            std::cout << "[ImposterManager]   Loaded from meshbin: " << loadedMesh->Vertices.size() << " vertices" << std::endl;
                        }
                    } else if (std::filesystem::exists(meshBinPath)) {
                        loadedMesh = AssetLibrary::Instance().LoadMeshBin(meshBinPath.string(), 0);
                        if (loadedMesh) {
                            std::cout << "[ImposterManager]   Loaded from meshbin: " << loadedMesh->Vertices.size() << " vertices" << std::endl;
                        }
                    }
                }
                
                // Try .meta and resolve to meshbin
                if (!loadedMesh) {
                    std::filesystem::path metaPath = projectDir / (prefabSource + ".meta");
                    if (std::filesystem::exists(metaPath)) {
                        std::string meshBin = AssetLibrary::Instance().ResolveMeshBinFromMeta(metaPath.string(), 0);
                        if (!meshBin.empty()) {
                            std::cout << "[ImposterManager]   Resolved meshbin from meta: " << meshBin << std::endl;
                            loadedMesh = AssetLibrary::Instance().LoadMeshBin(meshBin, 0);
                            if (loadedMesh) {
                                std::cout << "[ImposterManager]   Loaded from resolved meshbin: " << loadedMesh->Vertices.size() << " vertices" << std::endl;
                            }
                        }
                    }
                }
            }
        }
        
        // Method 6: Search for model file with matching entity name (last resort fallback)
        if (!loadedMesh) {
            std::filesystem::path projectDir = Project::GetProjectDirectory();
            std::string searchName = entityName;
            
            // Common model extensions
            std::vector<std::string> modelExts = {".fbx", ".glb", ".gltf", ".obj"};
            // Common model directories
            std::vector<std::string> modelDirs = {"assets/models", "assets/meshes", "assets/prefabs", "assets"};
            
            std::cout << "[ImposterManager] Method 6: Searching for model files matching '" << searchName << "'" << std::endl;
            
            for (const auto& dir : modelDirs) {
                for (const auto& ext : modelExts) {
                    // Try exact name match
                    std::filesystem::path modelPath = projectDir / dir / (searchName + ext);
                    if (std::filesystem::exists(modelPath)) {
                        std::cout << "[ImposterManager]   Found model file: " << modelPath.string() << std::endl;
                        
                        // Try to load via .meta file
                        std::filesystem::path metaPath = modelPath.string() + ".meta";
                        if (std::filesystem::exists(metaPath)) {
                            try {
                                std::ifstream metaFile(metaPath);
                                nlohmann::json metaJson;
                                metaFile >> metaJson;
                                metaFile.close();
                                
                                if (metaJson.contains("guid")) {
                                    ClaymoreGUID modelGuid;
                                    metaJson.at("guid").get_to(modelGuid);
                                    std::cout << "[ImposterManager]   Model GUID from meta: " << modelGuid.ToString() << std::endl;
                                    loadedMesh = loadMeshFromGuid(modelGuid, 0);
                                    if (loadedMesh) break;
                                }
                            } catch (...) {}
                        }
                        
                        // Try direct meshbin
                        std::filesystem::path meshBinPath = modelPath;
                        meshBinPath.replace_extension(".meshbin");
                        if (std::filesystem::exists(meshBinPath)) {
                            std::cout << "[ImposterManager]   Trying direct meshbin: " << meshBinPath.string() << std::endl;
                            loadedMesh = AssetLibrary::Instance().LoadMeshBin(meshBinPath.string(), 0);
                            if (loadedMesh) {
                                std::cout << "[ImposterManager]   Loaded from model meshbin: " << loadedMesh->Vertices.size() << " vertices" << std::endl;
                                break;
                            }
                        }
                    }
                }
                if (loadedMesh) break;
            }
        }
        
        // If we found a mesh, combine it into the output
        if (loadedMesh) {
            Mesh* mesh = loadedMesh.get();
            uint32_t baseVertex = static_cast<uint32_t>(outPositions.size());
            
            // NOTE: For imposters, we want LOCAL-SPACE vertices, NOT world-space!
            // The instance transform will position the mesh at runtime.
            // Do NOT multiply by worldTransform - that bakes world position into the mesh.
            
            for (size_t i = 0; i < mesh->Vertices.size(); ++i) {
                // Use raw mesh vertices (local space)
                glm::vec3 localPos = mesh->Vertices[i];
                outPositions.push_back(localPos);
                
                glm::vec3 n = (i < mesh->Normals.size()) ? mesh->Normals[i] : glm::vec3(0, 1, 0);
                outNormals.push_back(n);
                
                glm::vec2 uv = (i < mesh->UVs.size()) ? mesh->UVs[i] : glm::vec2(0);
                outUVs.push_back(uv);
                
                // Generate default tangent (mesh doesn't store tangents)
                glm::vec4 t(1, 0, 0, 1);
                outTangents.push_back(t);
                
                outBoundsMin = glm::min(outBoundsMin, localPos);
                outBoundsMax = glm::max(outBoundsMax, localPos);
            }
            
            // Append indices with offset
            for (uint32_t idx : mesh->Indices) {
                outIndices.push_back(baseVertex + idx);
            }
            
            foundMesh = true;
            std::cout << "[ImposterManager] Found mesh in entity '" << entityName << "' with " 
                      << mesh->Vertices.size() << " vertices" << std::endl;
        } else {
            std::cout << "[ImposterManager] Entity '" << entityName << "' - no mesh found" << std::endl;
        }
        
        // Note: children are processed separately since prefab stores entities flat
    };
    
    // Prefab stores entities as a FLAT array with "parent" references, not nested children
    // We need to build parent transforms first, then process all entities
    
    // First pass: collect all transforms by GUID
    std::unordered_map<std::string, glm::mat4> entityTransforms;
    std::unordered_map<std::string, std::string> entityParents;
    
    for (const auto& entity : prefab.Entities) {
        if (!entity.is_object()) continue;
        
        std::string guid = "";
        if (entity.contains("guid")) {
            try { 
                ClaymoreGUID g;
                entity.at("guid").get_to(g);
                guid = g.ToString();
            } catch (...) {}
        }
        
        std::string parentGuid = "";
        if (entity.contains("parent")) {
            try {
                ClaymoreGUID g;
                entity.at("parent").get_to(g);
                parentGuid = g.ToString();
            } catch (...) {}
        }
        
        if (!guid.empty()) {
            entityParents[guid] = parentGuid;
            
            // Build local transform
            glm::mat4 localTransform(1.0f);
            if (entity.contains("transform")) {
                const auto& t = entity["transform"];
                glm::vec3 pos(0), scl(1);
                glm::quat rotQ(1, 0, 0, 0);
                
                if (t.contains("position")) {
                    const auto& p = t["position"];
                    if (p.is_array() && p.size() >= 3) {
                        pos.x = p[0]; pos.y = p[1]; pos.z = p[2];
                    } else if (p.is_object()) {
                        pos.x = p.value("x", 0.0f);
                        pos.y = p.value("y", 0.0f);
                        pos.z = p.value("z", 0.0f);
                    }
                }
                
                if (t.contains("rotationQ") && t["rotationQ"].is_array() && t["rotationQ"].size() >= 4) {
                    const auto& q = t["rotationQ"];
                    rotQ = glm::quat(q[3].get<float>(), q[0].get<float>(), q[1].get<float>(), q[2].get<float>());
                } else if (t.contains("rotation")) {
                    const auto& r = t["rotation"];
                    glm::vec3 euler(0);
                    if (r.is_array() && r.size() >= 3) {
                        euler.x = glm::radians(r[0].get<float>());
                        euler.y = glm::radians(r[1].get<float>());
                        euler.z = glm::radians(r[2].get<float>());
                    } else if (r.is_object()) {
                        euler.x = glm::radians(r.value("x", 0.0f));
                        euler.y = glm::radians(r.value("y", 0.0f));
                        euler.z = glm::radians(r.value("z", 0.0f));
                    }
                    rotQ = glm::quat(euler);
                }
                
                if (t.contains("scale")) {
                    const auto& s = t["scale"];
                    if (s.is_array() && s.size() >= 3) {
                        scl.x = s[0]; scl.y = s[1]; scl.z = s[2];
                    } else if (s.is_object()) {
                        scl.x = s.value("x", 1.0f);
                        scl.y = s.value("y", 1.0f);
                        scl.z = s.value("z", 1.0f);
                    }
                }
                
                localTransform = glm::translate(glm::mat4(1.0f), pos);
                localTransform *= glm::mat4_cast(rotQ);
                localTransform = glm::scale(localTransform, scl);
            }
            entityTransforms[guid] = localTransform;
        }
    }
    
    // Second pass: compute world transforms (resolve parent chain)
    std::function<glm::mat4(const std::string&)> getWorldTransform = [&](const std::string& guid) -> glm::mat4 {
        if (guid.empty()) return glm::mat4(1.0f);
        
        auto it = entityTransforms.find(guid);
        if (it == entityTransforms.end()) return glm::mat4(1.0f);
        
        auto parentIt = entityParents.find(guid);
        if (parentIt == entityParents.end() || parentIt->second.empty()) {
            return it->second;
        }
        
        return getWorldTransform(parentIt->second) * it->second;
    };
    
    // Third pass: process all entities with correct world transforms
    for (const auto& entity : prefab.Entities) {
        if (!entity.is_object()) continue;
        
        std::string guid = "";
        if (entity.contains("guid")) {
            try {
                ClaymoreGUID g;
                entity.at("guid").get_to(g);
                guid = g.ToString();
            } catch (...) {}
        }
        
        glm::mat4 worldTransform = getWorldTransform(guid);
        processEntity(entity, worldTransform);
    }
    
    return foundMesh;
#endif
}

ImposterCache* ImposterManager::GetImposter(const ClaymoreGUID& prefabGuid) {
    auto it = m_Cache.find(prefabGuid);
    if (it != m_Cache.end()) {
        return &it->second;
    }
    
    // Bake on demand
    ImposterCache cache;
    if (BakeImposter(prefabGuid, cache)) {
        auto result = m_Cache.emplace(prefabGuid, std::move(cache));
        return &result.first->second;
    }
    
    return nullptr;
}

bool ImposterManager::HasImposter(const ClaymoreGUID& prefabGuid) const {
    return m_Cache.find(prefabGuid) != m_Cache.end();
}

void ImposterManager::ClearCache() {
    m_Cache.clear();
    s_FailedBakeAttempts.clear();
}

void ImposterManager::ClearImposter(const ClaymoreGUID& prefabGuid) {
    m_Cache.erase(prefabGuid);
    s_FailedBakeAttempts.erase(HashGUID(prefabGuid));
    
#ifndef CLAYMORE_RUNTIME
    // Also delete disk cache
    auto cachePath = GetImposterCachePath(prefabGuid);
    if (!cachePath.empty() && std::filesystem::exists(cachePath)) {
        std::filesystem::remove(cachePath);
        std::cout << "[ImposterManager] Deleted disk cache: " << cachePath.string() << std::endl;
    }
#endif
}

void ImposterManager::RenderImposters(
    uint16_t viewId,
    ResourceLayerComponent& comp,
    const glm::mat4& view,
    const glm::mat4& proj,
    const glm::vec3& cameraPos,
    uint64_t stateFlags)
{
    // Instance data layout: 4 vec4s = 64 bytes per instance (model matrix columns)
    // Must match varying.def.sc: i_data0=TEXCOORD7, i_data1=TEXCOORD6, i_data2=TEXCOORD5, i_data3=TEXCOORD4
    struct PBRInstanceData {
        glm::vec4 Data[4]; // Column-major 4x4 matrix
    };
    static_assert(sizeof(PBRInstanceData) == 64, "PBRInstanceData must be 64 bytes");
    
    // Static resources - initialized once
    static bgfx::ProgramHandle s_PbrInstancedProgram = BGFX_INVALID_HANDLE;
    static bgfx::VertexLayout s_InstanceLayout;
    static bgfx::UniformHandle s_u_cameraPos = BGFX_INVALID_HANDLE;
    static bgfx::UniformHandle s_u_UVTransform = BGFX_INVALID_HANDLE;
    static bgfx::UniformHandle s_s_albedo = BGFX_INVALID_HANDLE;
    static bgfx::UniformHandle s_s_normal = BGFX_INVALID_HANDLE;
    static bgfx::UniformHandle s_s_metalRoughness = BGFX_INVALID_HANDLE;
    static bgfx::TextureHandle s_WhiteTexture = BGFX_INVALID_HANDLE;
    static bgfx::TextureHandle s_DefaultNormal = BGFX_INVALID_HANDLE;
    static bool s_Initialized = false;
    
    // One-time initialization (following grass pattern)
    if (!s_Initialized) {
        std::cerr << "[ImposterManager] Initializing instanced PBR rendering..." << std::endl;
        s_PbrInstancedProgram = ShaderManager::Instance().LoadProgram("vs_pbr_instanced", "fs_pbr");
        if (!bgfx::isValid(s_PbrInstancedProgram)) {
            std::cerr << "[ImposterManager] FAILED to load vs_pbr_instanced shader!" << std::endl;
            return;
        }
        std::cerr << "[ImposterManager] Shader loaded OK: program idx=" << s_PbrInstancedProgram.idx << std::endl;
        
        // Instance layout must match varying.def.sc (same order as grass)
        s_InstanceLayout.begin()
            .add(bgfx::Attrib::TexCoord7, 4, bgfx::AttribType::Float)  // i_data0 = matrix column 0
            .add(bgfx::Attrib::TexCoord6, 4, bgfx::AttribType::Float)  // i_data1 = matrix column 1
            .add(bgfx::Attrib::TexCoord5, 4, bgfx::AttribType::Float)  // i_data2 = matrix column 2
            .add(bgfx::Attrib::TexCoord4, 4, bgfx::AttribType::Float)  // i_data3 = matrix column 3
            .end();
        
        // Create uniforms
        s_u_cameraPos = bgfx::createUniform("u_cameraPos", bgfx::UniformType::Vec4);
        s_u_UVTransform = bgfx::createUniform("u_UVTransform", bgfx::UniformType::Vec4);
        s_s_albedo = bgfx::createUniform("s_albedo", bgfx::UniformType::Sampler);
        s_s_normal = bgfx::createUniform("s_normal", bgfx::UniformType::Sampler);
        s_s_metalRoughness = bgfx::createUniform("s_metalRoughness", bgfx::UniformType::Sampler);
        
        // Create default white texture (1x1 white pixel)
        const uint32_t white = 0xFFFFFFFF;
        s_WhiteTexture = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8,
            BGFX_TEXTURE_NONE | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
            bgfx::copy(&white, sizeof(white)));
        
        // Create default normal texture (1x1 flat normal: R=128, G=128, B=255)
        const uint32_t flatNormal = 0xFFFF8080;
        s_DefaultNormal = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8,
            BGFX_TEXTURE_NONE | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
            bgfx::copy(&flatNormal, sizeof(flatNormal)));
        
        s_Initialized = true;
    }
    
    if (!bgfx::isValid(s_PbrInstancedProgram)) {
        return;
    }
    
    // Track instances needing debug visualization
    std::vector<std::pair<glm::vec3, glm::vec3>> debugPoints;
    
    // Scratch buffer for collecting instance data per layer
    static thread_local std::vector<PBRInstanceData> s_InstanceScratch;
    
    // Process each layer
    for (size_t layerIdx = 0; layerIdx < comp.Layers.size(); ++layerIdx) {
        const auto& layer = comp.Layers[layerIdx];
        
        if (!layer.Enabled) continue;
        
        // Try to get imposter for this layer's prefab
        ImposterCache* imposter = nullptr;
        if (layer.UseImposter && (layer.PrefabAsset.guid.high != 0 || layer.PrefabAsset.guid.low != 0)) {
            imposter = GetImposter(layer.PrefabAsset.guid);
        }
        
        bool hasValidImposter = imposter && imposter->Valid &&
                               bgfx::isValid(imposter->VBH) &&
                               bgfx::isValid(imposter->IBH);
        
        s_InstanceScratch.clear();
        
        // Collect transforms for visible instances
        for (const auto& inst : comp.Runtime.Instances) {
            if (inst.LayerIndex != layerIdx) continue;
            if (!inst.Visible) continue;
            if (inst.State == ResourceState::Active || inst.State == ResourceState::Destroyed) continue;
            
            float distance = inst.DistanceToCamera;
            if (distance > layer.CullDistance) continue;
            
            if (hasValidImposter && layer.UseImposter) {
                // Build instance transform (column-major)
                glm::mat4 transform = glm::translate(glm::mat4(1.0f), inst.Position);
                transform = transform * glm::mat4_cast(inst.Rotation);
                transform = glm::scale(transform, glm::vec3(inst.Scale));
                
                PBRInstanceData instData;
                instData.Data[0] = transform[0]; // Column 0
                instData.Data[1] = transform[1]; // Column 1
                instData.Data[2] = transform[2]; // Column 2
                instData.Data[3] = transform[3]; // Column 3
                s_InstanceScratch.push_back(instData);
            } else {
                // Queue for debug visualization
                debugPoints.emplace_back(inst.Position, layer.PreviewColor);
            }
        }
        
        if (s_InstanceScratch.empty()) continue;
        
        // Submit instanced draw (following grass CPU pattern)
        uint32_t instanceCount = static_cast<uint32_t>(s_InstanceScratch.size());
        const uint16_t stride = s_InstanceLayout.getStride();
        
        // Debug: Log first render attempt per layer
        static std::unordered_set<size_t> s_LoggedLayers;
        if (s_LoggedLayers.find(layerIdx) == s_LoggedLayers.end()) {
            s_LoggedLayers.insert(layerIdx);
            std::cerr << "[ImposterManager] SUBMIT layer " << layerIdx << ":" << std::endl;
            std::cerr << "  instanceCount: " << instanceCount << std::endl;
            std::cerr << "  stride: " << stride << " (expected: " << sizeof(PBRInstanceData) << ")" << std::endl;
            std::cerr << "  imposter VBH: " << imposter->VBH.idx << " IBH: " << imposter->IBH.idx << std::endl;
            std::cerr << "  imposter indexCount: " << imposter->IndexCount << std::endl;
            std::cerr << "  shader program: " << s_PbrInstancedProgram.idx << std::endl;
            // Log first instance transform
            if (!s_InstanceScratch.empty()) {
                const auto& first = s_InstanceScratch[0];
                std::cerr << "  first instance pos: (" << first.Data[3].x << ", " 
                          << first.Data[3].y << ", " << first.Data[3].z << ")" << std::endl;
            }
        }
        
        if (stride != sizeof(PBRInstanceData)) {
            std::cerr << "[ImposterManager] Instance stride mismatch. Expected " 
                      << sizeof(PBRInstanceData) << " bytes but layout uses " << stride << " bytes." << std::endl;
            continue;
        }
        
        // Check available transient buffer space
        const uint32_t availableInstances = bgfx::getAvailInstanceDataBuffer(instanceCount, stride);
        if (availableInstances == 0) {
            continue;
        }
        if (instanceCount > availableInstances) {
            instanceCount = availableInstances;
        }
        
        // Allocate and fill instance buffer
        bgfx::InstanceDataBuffer idb{};
        bgfx::allocInstanceDataBuffer(&idb, instanceCount, stride);
        std::memcpy(idb.data, s_InstanceScratch.data(), instanceCount * sizeof(PBRInstanceData));
        
        // Set uniforms
        glm::vec4 camPosVec4(cameraPos, 1.0f);
        bgfx::setUniform(s_u_cameraPos, &camPosVec4);
        
        glm::vec4 uvTransform(1.0f, 1.0f, 0.0f, 0.0f);
        bgfx::setUniform(s_u_UVTransform, &uvTransform);
        
        // Bind textures
        bgfx::TextureHandle albedo = (imposter && bgfx::isValid(imposter->AlbedoTexture)) 
            ? imposter->AlbedoTexture : s_WhiteTexture;
        bgfx::TextureHandle normal = (imposter && bgfx::isValid(imposter->NormalTexture))
            ? imposter->NormalTexture : s_DefaultNormal;
        
        bgfx::setTexture(0, s_s_albedo, albedo);
        bgfx::setTexture(1, s_s_normal, normal);
        bgfx::setTexture(2, s_s_metalRoughness, s_WhiteTexture);
        
        // Set vertex/index buffers (match grass pattern - don't specify counts)
        bgfx::setVertexBuffer(0, imposter->VBH);
        bgfx::setIndexBuffer(imposter->IBH);
        
        // Set instance data buffer
        bgfx::setInstanceDataBuffer(&idb);
        
        // Set state and submit
        bgfx::setState(stateFlags);
        bgfx::submit(viewId, s_PbrInstancedProgram);
        
        // Log once that we submitted
        static bool s_LoggedSubmit = false;
        if (!s_LoggedSubmit) {
            s_LoggedSubmit = true;
            std::cerr << "[ImposterManager] bgfx::submit called! viewId=" << viewId 
                      << " program=" << s_PbrInstancedProgram.idx 
                      << " instances=" << instanceCount << std::endl;
        }
        
        // DEBUG: Also draw one mesh without instancing using regular PBR shader to verify mesh is valid
        static bool s_DebugDrawOnce = false;
        if (!s_DebugDrawOnce && !s_InstanceScratch.empty()) {
            s_DebugDrawOnce = true;
            // Use regular vs_pbr (non-instanced) to test if mesh renders at all
            static bgfx::ProgramHandle s_RegularPbr = BGFX_INVALID_HANDLE;
            if (!bgfx::isValid(s_RegularPbr)) {
                s_RegularPbr = ShaderManager::Instance().LoadProgram("vs_pbr", "fs_pbr");
            }
            if (bgfx::isValid(s_RegularPbr)) {
                // Set transform from first instance
                const auto& firstInst = s_InstanceScratch[0];
                glm::mat4 testTransform;
                testTransform[0] = firstInst.Data[0];
                testTransform[1] = firstInst.Data[1];
                testTransform[2] = firstInst.Data[2];
                testTransform[3] = firstInst.Data[3];
                bgfx::setTransform(glm::value_ptr(testTransform));
                
                bgfx::setVertexBuffer(0, imposter->VBH);
                bgfx::setIndexBuffer(imposter->IBH);
                bgfx::setTexture(0, s_s_albedo, s_WhiteTexture);
                // Disable culling to test if winding is wrong
                uint64_t noCullState = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | 
                                       BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_MSAA;
                bgfx::setState(noCullState);
                bgfx::submit(viewId, s_RegularPbr);
                std::cerr << "[ImposterManager] DEBUG: Drew one mesh with NO CULLING at pos ("
                          << firstInst.Data[3].x << ", " << firstInst.Data[3].y << ", " 
                          << firstInst.Data[3].z << ")" << std::endl;
            }
        }
    }
    
    // Render debug markers
    if (!debugPoints.empty()) {
        RenderDebugMarkers(debugPoints, viewId);
    }
}

void ImposterManager::RenderDebugMarkers(
    const std::vector<std::pair<glm::vec3, glm::vec3>>& points,
    uint16_t viewId)
{
    (void)viewId;
    
    constexpr size_t kMaxDebugMarkers = 500;
    if (points.empty()) return;
    
    Renderer& renderer = Renderer::Get();
    size_t count = std::min(points.size(), kMaxDebugMarkers);
    
    for (size_t i = 0; i < count; ++i) {
        const auto& [pos, color] = points[i];
        
        uint32_t abgr = (255u << 24) |
                        (static_cast<uint32_t>(color.b * 255.0f) << 16) |
                        (static_cast<uint32_t>(color.g * 255.0f) << 8) |
                        static_cast<uint32_t>(color.r * 255.0f);
        
        const float size = 0.3f;
        renderer.DrawDebugLineColored(pos - glm::vec3(0, size * 0.5f, 0),
                                      pos + glm::vec3(0, size * 1.5f, 0), abgr);
        renderer.DrawDebugLineColored(pos - glm::vec3(size, 0, 0),
                                      pos + glm::vec3(size, 0, 0), abgr);
        renderer.DrawDebugLineColored(pos - glm::vec3(0, 0, size),
                                      pos + glm::vec3(0, 0, size), abgr);
    }
}

//------------------------------------------------------------------------------
// ProximitySwapSystem Implementation
//------------------------------------------------------------------------------
void ProximitySwapSystem::Update(
    ResourceLayerComponent& comp,
    Scene& scene,
    const glm::vec3& cameraPos,
    float deltaTime)
{
    (void)deltaTime;
    
    const float swapDistance = comp.GlobalSwapDistance;
    const float hysteresis = comp.SwapHysteresis;
    
    m_CurrentActivePrefabs = 0;
    
    for (uint32_t i = 0; i < comp.Runtime.Instances.size(); ++i) {
        ResourceInstance& inst = comp.Runtime.Instances[i];
        
        if (inst.State == ResourceState::Destroyed) continue;
        
        // Get layer settings
        if (inst.LayerIndex >= comp.Layers.size()) continue;
        const auto& layer = comp.Layers[inst.LayerIndex];
        
        float effectiveSwapDist = layer.ImposterDistance > 0 ? layer.ImposterDistance : swapDistance;
        float effectiveOuter = effectiveSwapDist + hysteresis;
        float effectiveInner = effectiveSwapDist - hysteresis;
        
        float distance = glm::length(cameraPos - inst.Position);
        inst.DistanceToCamera = distance;
        
        if (inst.State == ResourceState::Active) {
            ++m_CurrentActivePrefabs;
            
            if (inst.ActiveEntity != INVALID_ENTITY_ID) {
                if (IsPrefabDestroyed(scene, inst.ActiveEntity)) {
                    comp.MarkInstanceDestroyed(inst.InstanceID);
                    inst.ActiveEntity = INVALID_ENTITY_ID;
                    continue;
                }
                
                if (IsPrefabModified(scene, inst.ActiveEntity)) {
                    comp.MarkInstanceModified(inst.InstanceID);
                    continue;
                }
            }
            
            if (distance > effectiveOuter) {
                SwapToImposter(comp, scene, i);
            }
        }
        else if (inst.State == ResourceState::Pristine) {
            if (distance < effectiveInner && m_CurrentActivePrefabs < m_MaxActivePrefabs) {
                SwapToActive(comp, scene, i);
            }
        }
    }
    
    comp.Stats.ActivePrefabs = m_CurrentActivePrefabs;
}

EntityID ProximitySwapSystem::SwapToActive(
    ResourceLayerComponent& comp,
    Scene& scene,
    uint32_t instanceIndex)
{
    if (instanceIndex >= comp.Runtime.Instances.size()) {
        return INVALID_ENTITY_ID;
    }
    
    ResourceInstance& inst = comp.Runtime.Instances[instanceIndex];
    
    if (inst.LayerIndex >= comp.Layers.size()) return INVALID_ENTITY_ID;
    const auto& layer = comp.Layers[inst.LayerIndex];
    
    // Check if prefab GUID is valid
    if (layer.PrefabAsset.guid.high == 0 && layer.PrefabAsset.guid.low == 0) {
        std::cerr << "[ProximitySwap] Cannot spawn prefab - no GUID set for layer: " 
                  << layer.Name << std::endl;
        return INVALID_ENTITY_ID;
    }
    
#ifndef CLAYMORE_RUNTIME
    // Log first attempt with full diagnostics
    static bool s_LoggedOnce = false;
    if (!s_LoggedOnce) {
        s_LoggedOnce = true;
        std::cout << "[ProximitySwap] First prefab instantiation attempt:" << std::endl;
        std::cout << "  Position: (" << inst.Position.x << ", " << inst.Position.y << ", " << inst.Position.z << ")" << std::endl;
        std::cout << "  Layer: " << layer.Name << std::endl;
        std::cout << "  GUID: " << layer.PrefabAsset.guid.ToString() << std::endl;
        std::cout << "  Path: " << layer.PrefabPath << std::endl;
        
        // Check if prefab file exists
        namespace fs = std::filesystem;
        if (!layer.PrefabPath.empty()) {
            fs::path p(layer.PrefabPath);
            if (fs::exists(p)) {
                std::cout << "  Prefab file exists at path: " << p << std::endl;
            } else {
                // Try with project directory
                fs::path projPath = Project::GetProjectDirectory() / layer.PrefabPath;
                if (fs::exists(projPath)) {
                    std::cout << "  Prefab file exists at project path: " << projPath << std::endl;
                } else {
                    std::cout << "  WARNING: Prefab file not found at: " << p << " or " << projPath << std::endl;
                }
            }
        }
    }
              
    // Try path-based instantiation first (more reliable)
    EntityID prefabRoot = INVALID_ENTITY_ID;
    if (!layer.PrefabPath.empty()) {
        prefabRoot = ::InstantiatePrefabFromPath(layer.PrefabPath, scene);
    }
    if (prefabRoot == INVALID_ENTITY_ID && !(layer.PrefabAsset.guid.high == 0 && layer.PrefabAsset.guid.low == 0)) {
        prefabRoot = ::InstantiatePrefab(layer.PrefabAsset.guid, scene);
    }
#else
    EntityID prefabRoot = INVALID_ENTITY_ID;
    std::cerr << "[ResourceLayer] Prefab instantiation not available in runtime" << std::endl;
#endif
    
    if (prefabRoot == INVALID_ENTITY_ID) {
        std::cerr << "[ProximitySwap] Failed to instantiate prefab for layer: " 
                  << layer.Name << std::endl;
        return INVALID_ENTITY_ID;
    }
    
    std::cout << "[ProximitySwap] Successfully instantiated prefab, root entity: " 
              << prefabRoot << std::endl;
    
    // Strip physics components unless layer explicitly preserves them (for forageable items)
    if (!layer.PreservePhysics) {
        // Helper to strip physics from entity and children
        std::function<void(EntityID)> stripPhysics = [&](EntityID eid) {
            if (EntityData* ed = scene.GetEntityData(eid)) {
                ed->Area.reset();       // Remove area trigger component
                ed->RigidBody.reset();  // Remove dynamic body
                ed->StaticBody.reset(); // Remove static body
                ed->Collider.reset();   // Remove collision shape
                ed->CharacterController.reset();
                
                for (EntityID child : ed->Children) {
                    stripPhysics(child);
                }
            }
        };
        stripPhysics(prefabRoot);
    } else {
        std::cout << "[ProximitySwap] Preserving physics for interactable layer: " 
                  << layer.Name << std::endl;
    }
    
    if (EntityData* data = scene.GetEntityData(prefabRoot)) {
        data->Transform.Position = inst.Position;
        data->Transform.Rotation = glm::eulerAngles(inst.Rotation);
        data->Transform.Scale = glm::vec3(inst.Scale);
        scene.MarkTransformDirty(prefabRoot);
    }
    
    inst.State = ResourceState::Active;
    inst.ActiveEntity = prefabRoot;
    comp.Runtime.ActivePrefabIndices.insert(instanceIndex);
    
    return prefabRoot;
}

void ProximitySwapSystem::SwapToImposter(
    ResourceLayerComponent& comp,
    Scene& scene,
    uint32_t instanceIndex)
{
    if (instanceIndex >= comp.Runtime.Instances.size()) return;
    
    ResourceInstance& inst = comp.Runtime.Instances[instanceIndex];
    
    // Don't revert if the instance was marked as modified by the user
    if (inst.State == ResourceState::Modified) {
        // User modified this instance - it stays as a full prefab permanently
        return;
    }
    
    if (inst.ActiveEntity != INVALID_ENTITY_ID) {
        std::cout << "[ProximitySwap] Reverting prefab entity " << inst.ActiveEntity 
                  << " back to imposter" << std::endl;
        scene.RemoveEntity(inst.ActiveEntity);
    }
    
    inst.State = ResourceState::Pristine;
    inst.ActiveEntity = INVALID_ENTITY_ID;
    comp.Runtime.ActivePrefabIndices.erase(instanceIndex);
}

bool ProximitySwapSystem::IsPrefabModified(Scene& scene, EntityID prefabRoot) {
    EntityData* data = scene.GetEntityData(prefabRoot);
    if (!data) return true;
    return false;
}

bool ProximitySwapSystem::IsPrefabDestroyed(Scene& scene, EntityID prefabRoot) {
    return scene.GetEntityData(prefabRoot) == nullptr;
}

//------------------------------------------------------------------------------
// ResourceLayerRenderer Implementation
//------------------------------------------------------------------------------
void ResourceLayerRenderer::Render(
    uint16_t viewId,
    ResourceLayerComponent& comp,
    const glm::mat4& view,
    const glm::mat4& proj,
    const glm::vec3& cameraPos,
    uint64_t stateFlags)
{
    ImposterManager::Instance().RenderImposters(viewId, comp, view, proj, cameraPos, stateFlags);
}

void ResourceLayerRenderer::UpdateVisibility(
    ResourceLayerComponent& comp,
    const glm::vec3& cameraPos,
    const glm::mat4& viewProj)
{
    comp.UpdateVisibility(cameraPos, viewProj);
}

bool ResourceLayerRenderer::IsVisible(const glm::vec3& position, float radius, const glm::mat4& viewProj) const {
    glm::vec4 clip = viewProj * glm::vec4(position, 1.0f);
    if (clip.w <= 0.0f) return false;
    
    glm::vec3 ndc = glm::vec3(clip) / clip.w;
    float ndcRadius = radius / clip.w;
    
    return ndc.x >= -1.0f - ndcRadius && ndc.x <= 1.0f + ndcRadius &&
           ndc.y >= -1.0f - ndcRadius && ndc.y <= 1.0f + ndcRadius &&
           ndc.z >= 0.0f - ndcRadius && ndc.z <= 1.0f + ndcRadius;
}

} // namespace resourcelayer
} // namespace cm

