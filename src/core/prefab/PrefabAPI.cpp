#include "core/prefab/PrefabAPI.h"
#include "core/prefab/PrefabDelta.h"
#include "core/prefab/PrefabBinaryLoader.h"
#include "core/prefab/RuntimePrefabInstantiator.h"
#include "core/serialization/Serializer.h"
#include "core/ecs/ScenePostProcessing.h"
#include "core/animation/SkeletonBinding.h"
#include "core/animation/AvatarDefinition.h"
#include "core/animation/AnimationPlayerComponent.h"
#include "core/animation/ik/IKComponent.h"
#include "core/animation/lookat/LookAtConstraintComponent.h"
#include "core/assets/IAssetResolver.h"
#include "core/vfs/FileSystem.h"
#include "core/ecs/EntityData.h"
#include "core/ecs/UIComponents.h"
#include "core/ecs/RenderOverridesComponent.h"
#include "core/rendering/StandardMeshManager.h"
#include "core/physics/area/AreaComponent.h"
#include "core/managed/ScriptSystem.h"
#include "managed/interop/DotNetHost.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <mutex>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

#ifdef CLAYMORE_EDITOR
#include "editor/pipeline/AssetLibrary.h"
#include "editor/import/ModelBuild.h"
#include "editor/rendering/RendererFactory.h"
#include "editor/Project.h"
#include "editor/pipeline/BinaryAssetCache.h"
#endif

namespace {
#if defined(CLAYMORE_EDITOR)
bool TryEnsureBinaryForPlayMode(const std::string& sourcePath) {
    if (sourcePath.empty()) return false;
    if (Assets::GetLoadMode() != AssetLoadMode::PlayMode) return false;
    if (FileSystem::Instance().IsPakMounted()) return false;
    return BinaryAssetCache::Instance().EnsureBinary(sourcePath);
}
#endif
}

using json = nlohmann::json;
namespace fs = std::filesystem;

//==============================================================================
// GUID Packing (collision-resistant)
//==============================================================================

// Collision-resistant GUID packing using FNV-1a hash
// XOR-based packing can collide (e.g., {1,2} and {5,0} both map to 5)
// FNV-1a provides better distribution with minimal collisions
static uint64_t PackGuidHash(const ClaymoreGUID& g) { 
    constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
    constexpr uint64_t FNV_PRIME = 1099511628211ULL;
    
    uint64_t hash = FNV_OFFSET;
    
    // Hash high bytes
    for (int i = 0; i < 8; ++i) {
        hash ^= (g.high >> (i * 8)) & 0xFF;
        hash *= FNV_PRIME;
    }
    
    // Hash low bytes
    for (int i = 0; i < 8; ++i) {
        hash ^= (g.low >> (i * 8)) & 0xFF;
        hash *= FNV_PRIME;
    }
    
    return hash;
}

//==============================================================================
// Spam Protection
//==============================================================================

namespace {
    std::unordered_set<std::string> s_FailedPrefabPaths;
    std::unordered_set<uint64_t> s_FailedPrefabGuids;

    struct PrefabAssetCacheEntry {
        PrefabAsset Asset;
        fs::file_time_type WriteTime{};
        bool HasWriteTime = false;
    };

    std::mutex s_PrefabAssetCacheMutex;
    std::unordered_map<std::string, PrefabAssetCacheEntry> s_PrefabAssetCache;
    
    uint64_t HashGUID(const ClaymoreGUID& g) {
        return g.high ^ (g.low << 1);
    }
    
    bool HasFailedPath(const std::string& path) {
        bool failed = s_FailedPrefabPaths.count(path) > 0;
        if (failed) {
            static std::unordered_set<std::string> s_LoggedFailedPaths;
            if (s_LoggedFailedPaths.count(path) == 0) {
                s_LoggedFailedPaths.insert(path);
                std::cerr << "[Prefab] Skipping previously failed path: " << path << std::endl;
            }
        }
        return failed;
    }
    
    void MarkPathFailed(const std::string& path) {
        s_FailedPrefabPaths.insert(path);
    }
    
    bool HasFailedGuid(const ClaymoreGUID& guid) {
        bool failed = s_FailedPrefabGuids.count(HashGUID(guid)) > 0;
        if (failed) {
            static std::unordered_set<uint64_t> s_LoggedFailedGuids;
            if (s_LoggedFailedGuids.count(HashGUID(guid)) == 0) {
                s_LoggedFailedGuids.insert(HashGUID(guid));
                std::cerr << "[Prefab] Skipping previously failed GUID: " << guid.ToString() << std::endl;
            }
        }
        return failed;
    }
    
    void MarkGuidFailed(const ClaymoreGUID& guid) {
        std::cerr << "[Prefab] Marking GUID as failed: " << guid.ToString() << std::endl;
        s_FailedPrefabGuids.insert(HashGUID(guid));
    }

    void CreatePrefabScriptInstances(Scene& scene, const std::vector<EntityID>& entityIds) {
        int createdCount = 0;
        for (EntityID id : entityIds) {
            EntityData* data = scene.GetEntityData(id);
            if (!data) continue;

            for (auto& script : data->Scripts) {
                if (script.ClassName.empty()) continue;
                if (script.Instance) continue;

                auto created = ScriptSystem::Instance().Create(script.ClassName);
                if (created) {
                    script.Instance = created;
                    createdCount++;
                } else {
                    std::cerr << "[Prefab] Failed to create script instance: " << script.ClassName
                              << " for entity '" << data->Name << "'" << std::endl;
                }
            }
        }

        if (createdCount > 0) {
            std::cout << "[Prefab] Created " << createdCount << " script instances" << std::endl;
        }
    }
}

// Clear spam protection caches (call this when entering play mode, for example)
void ClearPrefabFailedCache() {
    s_FailedPrefabPaths.clear();
    s_FailedPrefabGuids.clear();
    {
        std::lock_guard<std::mutex> lock(s_PrefabAssetCacheMutex);
        s_PrefabAssetCache.clear();
    }
    std::cout << "[Prefab] Cleared failed prefab cache" << std::endl;
}

//==============================================================================
// Path Resolution
//==============================================================================

static fs::path GetProjectDirectory() {
#ifdef CLAYMORE_EDITOR
    return Project::GetProjectDirectory();
#else
    return FileSystem::Instance().GetProjectRoot();
#endif
}

static std::string ResolvePrefabDiskPath(const std::string& path) {
    fs::path rawPath(path);
    if (rawPath.is_absolute()) {
        return rawPath.lexically_normal().string();
    }

    fs::path proj = GetProjectDirectory();
    if (!proj.empty()) {
        fs::path projectPath = (proj / rawPath).lexically_normal();
        if (fs::exists(projectPath)) {
            return projectPath.string();
        }
    }

    if (fs::exists(rawPath)) {
        return rawPath.lexically_normal().string();
    }

    return path;
}

static std::string NormalizePrefabCacheKey(std::string path) {
    fs::path normalized(path);
    path = normalized.lexically_normal().string();
    for (char& c : path) {
        if (c == '\\') c = '/';
        else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return path;
}

static bool TryGetPrefabWriteTime(const std::string& path, fs::file_time_type& outTime) {
    std::error_code ec;
    const fs::path resolved = ResolvePrefabDiskPath(path);
    if (!fs::exists(resolved, ec)) {
        return false;
    }
    outTime = fs::last_write_time(resolved, ec);
    return !ec;
}

static bool LoadPrefabAssetCached(const std::string& prefabPath, bool sourceOnly, PrefabAsset& out) {
    fs::file_time_type writeTime{};
    const bool hasWriteTime = TryGetPrefabWriteTime(prefabPath, writeTime);
    const std::string cacheKey =
        std::string(sourceOnly ? "source:" : "normal:") + NormalizePrefabCacheKey(ResolvePrefabDiskPath(prefabPath));

    if (hasWriteTime) {
        std::lock_guard<std::mutex> lock(s_PrefabAssetCacheMutex);
        auto it = s_PrefabAssetCache.find(cacheKey);
        if (it != s_PrefabAssetCache.end() &&
            it->second.HasWriteTime &&
            it->second.WriteTime == writeTime) {
            out = it->second.Asset;
            return true;
        }
    }

    PrefabAsset loaded;
    const bool loadedOk = sourceOnly
        ? PrefabIO::LoadPrefabSource(prefabPath, loaded)
        : PrefabIO::LoadPrefab(prefabPath, loaded);
    if (!loadedOk) {
        return false;
    }

    out = loaded;
    if (hasWriteTime) {
        PrefabAssetCacheEntry entry;
        entry.Asset = std::move(loaded);
        entry.WriteTime = writeTime;
        entry.HasWriteTime = true;
        std::lock_guard<std::mutex> lock(s_PrefabAssetCacheMutex);
        s_PrefabAssetCache[cacheKey] = std::move(entry);
    }
    return true;
}

static void ClearPrefabAssetCacheForPath(const std::string& prefabPath) {
    const std::string normalized = NormalizePrefabCacheKey(ResolvePrefabDiskPath(prefabPath));
    std::lock_guard<std::mutex> lock(s_PrefabAssetCacheMutex);
    s_PrefabAssetCache.erase("source:" + normalized);
    s_PrefabAssetCache.erase("normal:" + normalized);
}

static std::string GetPrefabPathFromGuid(const ClaymoreGUID& guid) {
    // Use the global asset resolver (works in editor, play mode, and runtime)
    if (auto* resolver = Assets::GetResolver()) {
        std::string sourcePath = resolver->GetPathForGUID(guid);
        if (!sourcePath.empty()) {
            // In play mode or runtime, get the binary path if it exists
            if (resolver->ShouldLoadBinary()) {
                std::string binaryPath = resolver->GetBinaryPath(sourcePath);
                if (!binaryPath.empty()) {
#ifdef CLAYMORE_RUNTIME
                    return binaryPath;
#else
                    if (FileSystem::Instance().Exists(binaryPath)) {
                        return binaryPath;
                    }
                    if (!resolver->AllowSourceFallback()) {
                        return binaryPath;
                    }
#endif
                }
            }
            return sourcePath;
        } else {
            std::cout << "[Prefab] GetPathForGUID returned empty for: " << guid.ToString() << std::endl;
        }
    } else {
        std::cout << "[Prefab] No resolver available!" << std::endl;
    }
    // Fallback to canonical location
    std::string fallback = "assets/prefabs/" + guid.ToString() + ".prefab";
    std::cout << "[Prefab] Using fallback path: " << fallback << std::endl;
    return fallback;
}

//==============================================================================
// PrefabIO Implementation
//==============================================================================

namespace PrefabIO {

static std::string ResolvePrefabAuthoringPath(const std::string& path) {
    return ResolvePrefabDiskPath(path);
}

static bool PopulatePrefabAssetFromJson(const json& j, const std::string& path, PrefabAsset& out) {
    try {
        out.Clear();

        // Store raw JSON for assetMap and other metadata
        out.Raw = j;

        // Read prefab metadata
        if (j.contains("guid")) j.at("guid").get_to(out.Guid);
        out.Name = j.value("name", "");

        // Handle both "root" and "rootGuid" keys
        if (j.contains("rootGuid")) {
            j.at("rootGuid").get_to(out.RootGuid);
        } else if (j.contains("root")) {
            j.at("root").get_to(out.RootGuid);
        }

        // Load entities (already in Serializer format)
        if (j.contains("entities") && j["entities"].is_array()) {
            out.Entities = j["entities"];
        } else {
            out.Entities = json::array();
        }

        std::cout << "[PrefabIO] Loaded prefab: " << path
                  << " (" << out.EntityCount() << " entities)\n";
        return true;

    } catch (const std::exception& e) {
        std::cerr << "[PrefabIO] Error loading prefab: " << e.what() << std::endl;
        return false;
    }
}

bool LoadPrefabSource(const std::string& path, PrefabAsset& out) {
    const std::string resolvedPath = ResolvePrefabAuthoringPath(path);

    std::ifstream file(resolvedPath);
    if (!file.is_open()) {
        std::cerr << "[PrefabIO] Cannot read prefab source: " << path << std::endl;
        return false;
    }

    json j;
    try {
        file >> j;
    } catch (const std::exception& e) {
        std::cerr << "[PrefabIO] JSON parse error in " << path << ": " << e.what() << std::endl;
        return false;
    }

    return PopulatePrefabAssetFromJson(j, path, out);
}

bool LoadPrefab(const std::string& path, PrefabAsset& out) {
    // Resolve path
    std::string resolvedPath = ResolvePrefabAuthoringPath(path);

    // Check if this is a binary prefab (.prefabb)
    std::string ext = fs::path(resolvedPath).extension().string();
    if (ext == ".prefabb") {
        if (binary::PrefabBinaryLoader::Load(resolvedPath, out)) {
            std::cout << "[PrefabIO] Loaded binary prefab: " << path 
                      << " (" << out.EntityCount() << " entities)\n";
            return true;
        }
#if defined(CLAYMORE_EDITOR)
        std::filesystem::path sourcePath(resolvedPath);
        sourcePath.replace_extension(".prefab");
        if (TryEnsureBinaryForPlayMode(sourcePath.string())) {
            std::string binaryPath = Assets::GetBinaryPath(sourcePath.string());
            if (!binaryPath.empty() && binary::PrefabBinaryLoader::Load(binaryPath, out)) {
                std::cout << "[PrefabIO] Loaded compiled binary prefab: " << binaryPath
                          << " (" << out.EntityCount() << " entities)\n";
                return true;
            }
        }
#endif
        std::cerr << "[PrefabIO] Failed to load binary prefab: " << path << std::endl;
        return false;
    }

    if (Assets::ShouldLoadBinary()) {
        std::string binaryPath = Assets::GetBinaryPath(resolvedPath);
        if (!binaryPath.empty() && binary::PrefabBinaryLoader::Load(binaryPath, out)) {
            std::cout << "[PrefabIO] Loaded binary prefab: " << binaryPath 
                      << " (" << out.EntityCount() << " entities)\n";
            return true;
        }
#if defined(CLAYMORE_EDITOR)
        if (TryEnsureBinaryForPlayMode(resolvedPath)) {
            binaryPath = Assets::GetBinaryPath(resolvedPath);
            if (!binaryPath.empty() && binary::PrefabBinaryLoader::Load(binaryPath, out)) {
                std::cout << "[PrefabIO] Loaded compiled binary prefab: " << binaryPath
                          << " (" << out.EntityCount() << " entities)\n";
                return true;
            }
        }
#endif
    }
    
    if (Assets::ShouldLoadBinary() && !Assets::AllowSourceFallback()) {
        std::cerr << "[PrefabIO] Missing binary prefab (binary-only mode): " << path << std::endl;
        return false;
    }

    // Load from JSON source
    std::string jsonText;
    if (!FileSystem::Instance().ReadTextFile(resolvedPath, jsonText)) {
        std::cerr << "[PrefabIO] Cannot read prefab from VFS: " << path << std::endl;
        return false;
    }
    
    json j;
    try {
        j = json::parse(jsonText);
    } catch (const std::exception& e) {
        std::cerr << "[PrefabIO] JSON parse error in " << path << ": " << e.what() << std::endl;
        return false;
    }
    
    try {
        return PopulatePrefabAssetFromJson(j, path, out);
    } catch (const std::exception& e) {
        std::cerr << "[PrefabIO] Error loading prefab: " << e.what() << std::endl;
        return false;
    }
}

bool SavePrefab(const std::string& path, const PrefabAsset& in) {
    // TRUE PARITY: If we have Raw JSON from SerializeScene, use it directly
    // This ensures prefabs have EXACTLY the same format as scenes
    json j;
    if (!in.Raw.is_null() && in.Raw.is_object()) {
        // Use the raw serializer output - includes assetMap, entities, everything
        j = in.Raw;
    } else {
        // Fallback for legacy code paths
        j["version"] = "2.0";
        j["guid"] = in.Guid;
        j["name"] = in.Name;
        j["rootGuid"] = in.RootGuid;
        j["entities"] = in.Entities;
    }
    
    // Ensure directory exists
    fs::path p(path);
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path());
    }
    
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        std::cerr << "[PrefabIO] Cannot write prefab: " << path << std::endl;
        return false;
    }
    
    out << j.dump(4);
    out.close();
    std::cout << "[PrefabIO] Saved prefab: " << path 
              << " (" << in.EntityCount() << " entities)\n";
    
#ifdef CLAYMORE_EDITOR
    // Automatically rebuild binary cache after saving prefab
    BinaryAssetCache::Instance().RebuildBinary(path);
#endif
    ClearPrefabAssetCacheForPath(path);
    
    return true;
}

} // namespace PrefabIO

//==============================================================================
// Skeleton Helpers
//==============================================================================

static void GenerateBoneEntitiesForSkeleton(Scene& scene, EntityID skeletonEntity) {
    auto* skelData = scene.GetEntityData(skeletonEntity);
    if (!skelData || !skelData->Skeleton) return;
    
    SkeletonComponent& sk = *skelData->Skeleton;
    const size_t n = sk.InverseBindPoses.size();
    if (n == 0) return;
    
    // Ensure BindPoseGlobals computed
    if (sk.BindPoseGlobals.size() != n) {
        sk.BindPoseGlobals.resize(n);
        for (size_t i = 0; i < n; ++i) {
            sk.BindPoseGlobals[i] = glm::inverse(sk.InverseBindPoses[i]);
        }
    }
    
    // Build parent index map
    std::vector<int> parentBone(n, -1);
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < n; ++j) {
            if (i == j) continue;
            // Check if j is parent of i
            glm::mat4 localI = glm::inverse(sk.BindPoseGlobals[j]) * sk.BindPoseGlobals[i];
            if (glm::length(glm::vec3(localI[3])) < 0.0001f) continue;
            // Simple heuristic: closer parent in hierarchy
            if (parentBone[i] < 0) parentBone[i] = (int)j;
        }
    }
    
    // Resize BoneEntities
    if (sk.BoneEntities.size() != n) {
        sk.BoneEntities.assign(n, INVALID_ENTITY_ID);
    }
    
    // Map existing children by name (with fuzzy matching for numeric suffixes)
    std::unordered_map<std::string, EntityID> nameToChild;
    std::function<void(EntityID)> collectChildren = [&](EntityID eid) {
        auto* ed = scene.GetEntityData(eid);
        if (!ed) return;
        // Add full name
        nameToChild[ed->Name] = eid;
        // Also add without numeric suffix for fuzzy matching
        // (handles cases like "mixamorig:Head_113" -> "mixamorig:Head")
        size_t underscore = ed->Name.find_last_of('_');
        if (underscore != std::string::npos) {
            bool allDigits = true;
            for (size_t i = underscore + 1; i < ed->Name.size(); ++i) {
                if (!std::isdigit(static_cast<unsigned char>(ed->Name[i]))) {
                    allDigits = false;
                    break;
                }
            }
            if (allDigits) {
                nameToChild[ed->Name.substr(0, underscore)] = eid;
            }
        }
        for (EntityID c : ed->Children) collectChildren(c);
    };
    collectChildren(skeletonEntity);
    
    // Create missing bone entities
    for (size_t b = 0; b < n; ++b) {
        if (b < sk.BoneNames.size()) {
            auto it = nameToChild.find(sk.BoneNames[b]);
            if (it != nameToChild.end()) {
                sk.BoneEntities[b] = it->second;
                continue;
            }
        }
        
        // Create new bone entity
        std::string boneName = (b < sk.BoneNames.size()) ? sk.BoneNames[b] : ("Bone_" + std::to_string(b));
        Entity boneEnt = scene.CreateEntityExact(boneName);
        EntityID boneId = boneEnt.GetID();
        sk.BoneEntities[b] = boneId;
        
        // Determine parent
        EntityID parentId = skeletonEntity;
        if (parentBone[b] >= 0 && parentBone[b] < (int)n) {
            EntityID pb = sk.BoneEntities[parentBone[b]];
            if (pb != INVALID_ENTITY_ID) parentId = pb;
        }
        scene.SetParent(boneId, parentId);
        
        // Set transform from bind pose
        auto* bd = scene.GetEntityData(boneId);
        if (bd) {
            glm::mat4 local = sk.BindPoseGlobals[b];
            if (parentBone[b] >= 0 && parentBone[b] < (int)n) {
                local = glm::inverse(sk.BindPoseGlobals[parentBone[b]]) * sk.BindPoseGlobals[b];
            }
            
            glm::vec3 scale, translation, skew;
            glm::vec4 perspective;
            glm::quat rotation;
            glm::decompose(local, scale, rotation, translation, skew, perspective);
            
            bd->Transform.Position = translation;
            bd->Transform.RotationQ = rotation;
            bd->Transform.Scale = scale;
            bd->Transform.UseQuatRotation = true;
            scene.MarkTransformDirty(boneId);
        }
    }
}

//==============================================================================
// Prefab Instantiation
//==============================================================================

EntityID InstantiatePrefab(const ClaymoreGUID& prefabGuid, Scene& dst, EntityID existingRoot, bool useExistingRoot) {
    if (HasFailedGuid(prefabGuid)) {
        return INVALID_ENTITY_ID;
    }
    
    std::string prefabPath = GetPrefabPathFromGuid(prefabGuid);
    EntityID rootId = InstantiatePrefabFromPath(prefabPath, dst, existingRoot, useExistingRoot);
    if (rootId == INVALID_ENTITY_ID) {
        MarkGuidFailed(prefabGuid);
    }
    return rootId;
}

EntityID InstantiatePrefabBlocking(const ClaymoreGUID& prefabGuid, Scene& dst, EntityID existingRoot, bool useExistingRoot) {
    if (HasFailedGuid(prefabGuid)) {
        return INVALID_ENTITY_ID;
    }
    
    std::string prefabPath = GetPrefabPathFromGuid(prefabGuid);
    EntityID rootId = InstantiatePrefabFromPathBlocking(prefabPath, dst, existingRoot, useExistingRoot);
    if (rootId == INVALID_ENTITY_ID) {
        MarkGuidFailed(prefabGuid);
    }
    return rootId;
}

EntityID InstantiatePrefabFromPathBlocking(const std::string& prefabPath, Scene& dst, EntityID existingRoot, bool useExistingRoot) {
    if (HasFailedPath(prefabPath)) {
        return INVALID_ENTITY_ID;
    }
    
    std::string resolvedPath = ResolvePrefabDiskPath(prefabPath);
    std::string ext = fs::path(resolvedPath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    // Check if this is a binary prefab - use RuntimePrefabInstantiator for v2 binary format.
    if (ext == ".prefabb") {
        EntityID rootId = runtime::RuntimePrefabInstantiator::InstantiateBlocking(resolvedPath, dst, existingRoot, useExistingRoot);
        if (rootId != INVALID_ENTITY_ID) {
            return rootId;
        }
        std::cerr << "[Prefab] Binary instantiation failed for: " << prefabPath << std::endl;
        MarkPathFailed(prefabPath);
        return INVALID_ENTITY_ID;
    }

    if ((ext == ".prefab" || ext == ".json") && Assets::ShouldLoadBinary()) {
#if defined(CLAYMORE_EDITOR)
        (void)TryEnsureBinaryForPlayMode(prefabPath);
        if (resolvedPath != prefabPath) {
            (void)TryEnsureBinaryForPlayMode(resolvedPath);
        }
#endif
        std::string binaryPath = Assets::GetBinaryPath(prefabPath);
        if (binaryPath.empty() && resolvedPath != prefabPath) {
            binaryPath = Assets::GetBinaryPath(resolvedPath);
        }
        if (!binaryPath.empty()) {
            EntityID rootId = runtime::RuntimePrefabInstantiator::InstantiateBlocking(binaryPath, dst, existingRoot, useExistingRoot);
            if (rootId != INVALID_ENTITY_ID) {
                return rootId;
            }
        }

        if (!Assets::AllowSourceFallback()) {
            std::cerr << "[Prefab] Binary prefab load failed and source fallback disabled: "
                      << prefabPath << std::endl;
            MarkPathFailed(prefabPath);
            return INVALID_ENTITY_ID;
        }
    }

    PrefabAsset asset;
    const bool loadedSource =
        Assets::ShouldLoadBinary()
            ? LoadPrefabAssetCached(prefabPath, true, asset)
            : LoadPrefabAssetCached(prefabPath, false, asset);
    if (!loadedSource) {
        MarkPathFailed(prefabPath);
        return INVALID_ENTITY_ID;
    }
    
    if (!asset.IsValid()) {
        std::cerr << "[Prefab] Invalid prefab (no entities): " << prefabPath << std::endl;
        MarkPathFailed(prefabPath);
        return INVALID_ENTITY_ID;
    }
    
    return InstantiatePrefabAsset(asset, dst, existingRoot, useExistingRoot, prefabPath.c_str());
}

EntityID InstantiatePrefabFromPath(const std::string& prefabPath, Scene& dst, EntityID existingRoot, bool useExistingRoot) {
    if (HasFailedPath(prefabPath)) {
        return INVALID_ENTITY_ID;
    }
    
    // Resolve path (same as PrefabIO::LoadPrefab) so binary resolution uses correct key
    std::string resolvedPath = prefabPath;
    if (!FileSystem::Instance().Exists(resolvedPath)) {
        fs::path proj = GetProjectDirectory();
        if (!proj.empty()) {
            resolvedPath = (proj / prefabPath).string();
        }
    }
    std::string ext = fs::path(resolvedPath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    // Explicit .prefabb path: use RuntimePrefabInstantiator (reads v2+ binary format)
    if (ext == ".prefabb") {
        EntityID rootId = dst.m_IsPlaying
            ? runtime::RuntimePrefabInstantiator::Instantiate(resolvedPath, dst, existingRoot, useExistingRoot)
            : runtime::RuntimePrefabInstantiator::InstantiateBlocking(resolvedPath, dst, existingRoot, useExistingRoot);
        if (rootId != INVALID_ENTITY_ID) {
            return rootId;
        }
        std::cerr << "[Prefab] Binary instantiation failed for: " << prefabPath << std::endl;
        MarkPathFailed(prefabPath);
        return INVALID_ENTITY_ID;
    }
    
    // Play mode / live runtime: .prefab and .json must use binary path with RuntimePrefabInstantiator.
    // PrefabBinaryLoader expects legacy layout; current binaries are v18 (EntityBinaryWriter).
    // Loading them via LoadPrefab + InstantiatePrefabAsset produces wrong hierarchy/transform/animation.
    if ((ext == ".prefab" || ext == ".json") && Assets::ShouldLoadBinary()) {
        // Prefer scene-stored path (often relative) so VFS/PAK can resolve in live builds
        std::string binaryPath = Assets::GetBinaryPath(prefabPath);
        if (binaryPath.empty() && resolvedPath != prefabPath) {
            binaryPath = Assets::GetBinaryPath(resolvedPath);
        }
        if (!binaryPath.empty()) {
            EntityID rootId = dst.m_IsPlaying
                ? runtime::RuntimePrefabInstantiator::Instantiate(binaryPath, dst, existingRoot, useExistingRoot)
                : runtime::RuntimePrefabInstantiator::InstantiateBlocking(binaryPath, dst, existingRoot, useExistingRoot);
            if (rootId != INVALID_ENTITY_ID) {
                return rootId;
            }
        }
        if (!Assets::AllowSourceFallback()) {
            std::cerr << "[Prefab] Binary prefab load failed and source fallback disabled: " << prefabPath << std::endl;
            MarkPathFailed(prefabPath);
            return INVALID_ENTITY_ID;
        }
    }
    
    // Authoring or fallback: load from JSON and instantiate (single code path for JSON -> scene)
    PrefabAsset asset;
    if (!LoadPrefabAssetCached(prefabPath, Assets::ShouldLoadBinary(), asset)) {
        MarkPathFailed(prefabPath);
        return INVALID_ENTITY_ID;
    }
    
    if (!asset.IsValid()) {
        std::cerr << "[Prefab] Invalid prefab (no entities): " << prefabPath << std::endl;
        MarkPathFailed(prefabPath);
        return INVALID_ENTITY_ID;
    }
    
    return InstantiatePrefabAsset(asset, dst, existingRoot, useExistingRoot, prefabPath.c_str());
}

EntityID InstantiatePrefabAsset(const PrefabAsset& asset, Scene& dst, EntityID existingRoot, bool useExistingRoot, const char* prefabPathForInstance) {
    std::string prefabPath = prefabPathForInstance ? std::string(prefabPathForInstance) : std::string();
    auto pack = [](const ClaymoreGUID& g) -> uint64_t { return PackGuidHash(g); };
    
    // Map from prefab entity GUID (packed) to created scene EntityID
    std::unordered_map<uint64_t, EntityID> guidToId;
    guidToId.reserve(asset.EntityCount() * 2);
    
    // UNIFIED WITH SCENE LOADING: Map from old EntityID to new EntityID
    std::unordered_map<EntityID, EntityID> idMapping;
    idMapping.reserve(asset.EntityCount());
    
    std::unordered_set<EntityID> opaqueRoots;
    std::unordered_map<ClaymoreGUID, ClaymoreGUID> prefabToInstanceGuid;
    std::unordered_map<ClaymoreGUID, ClaymoreGUID> instanceToPrefabGuid;
    prefabToInstanceGuid.reserve(asset.EntityCount());
    instanceToPrefabGuid.reserve(asset.EntityCount());
    
    std::cout << "[Prefab] Instantiating: " << asset.Name 
              << " (" << asset.EntityCount() << " entities)\n";
    
#ifdef CLAYMORE_EDITOR
    if (asset.Raw.contains("assetMap") && asset.Raw["assetMap"].is_array()) {
        for (const auto& rec : asset.Raw["assetMap"]) {
            std::string gstr = rec.value("guid", "");
            std::string vpath = rec.value("path", "");
            if (gstr.empty() || vpath.empty()) continue;
            std::string ext = std::filesystem::path(vpath).extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            bool looksLikeMesh = (ext == ".fbx" || ext == ".gltf" || ext == ".glb" || ext == ".meshbin" || ext == ".meta");
            if (!looksLikeMesh) continue;
            ClaymoreGUID g = ClaymoreGUID::FromString(gstr);
            AssetLibrary::Instance().RegisterAsset(AssetReference(g, 0, (int)AssetType::Mesh), AssetType::Mesh, vpath, vpath);
        }
    }
#endif
    
    std::vector<EntityID> indexToEntityId(asset.EntityCount(), INVALID_ENTITY_ID);
    std::unordered_set<size_t> modelRootIndices;
    std::unordered_set<size_t> modelDescendantIndices;
    
    for (size_t i = 0; i < asset.Entities.size(); ++i) {
        const auto& entityJson = asset.Entities[i];
        if (!entityJson.is_object()) continue;
        if (entityJson.contains("asset") && entityJson["asset"].is_object()) {
            if (entityJson["asset"].value("type", "") == "model") {
                modelRootIndices.insert(i);
            }
        }
    }
    
    // Second pass: find all descendants of model roots using parentIndex or parent
    if (!modelRootIndices.empty()) {
        // Build index -> parent index map
        std::vector<int> parentIndexMap(asset.Entities.size(), -1);
        std::unordered_map<EntityID, size_t> oldIdToIndex;
        
        for (size_t i = 0; i < asset.Entities.size(); ++i) {
            const auto& entityJson = asset.Entities[i];
            if (!entityJson.is_object()) continue;
            
            // Track old EntityID -> index mapping for parent resolution
            if (entityJson.contains("id")) {
                EntityID oldId = entityJson["id"].get<EntityID>();
                oldIdToIndex[oldId] = i;
            }
            
            // Get parent index (from parentIndex or resolve from parent)
            if (entityJson.contains("parentIndex")) {
                parentIndexMap[i] = entityJson["parentIndex"].get<int>();
            }
        }
        
        // Resolve parent field to parent index (for new format prefabs)
        for (size_t i = 0; i < asset.Entities.size(); ++i) {
            if (parentIndexMap[i] >= 0) continue; // Already have parentIndex
            
            const auto& entityJson = asset.Entities[i];
            if (!entityJson.is_object()) continue;
            
            if (entityJson.contains("parent")) {
                EntityID oldParent = entityJson["parent"].get<EntityID>();
                if (oldParent != INVALID_ENTITY_ID && oldParent != (EntityID)-1) {
                    auto it = oldIdToIndex.find(oldParent);
                    if (it != oldIdToIndex.end()) {
                        parentIndexMap[i] = static_cast<int>(it->second);
                    }
                }
            }
        }
        
        // Mark all descendants of model roots
        std::function<bool(size_t)> isDescendantOfModelRoot = [&](size_t idx) -> bool {
            if (idx >= asset.Entities.size()) return false;
            if (modelRootIndices.count(idx)) return false; // Model root itself is NOT a descendant
            if (modelDescendantIndices.count(idx)) return true; // Already marked
            
            int parentIdx = parentIndexMap[idx];
            if (parentIdx < 0) return false;
            
            if (modelRootIndices.count(parentIdx)) {
                // Direct child of model root
                modelDescendantIndices.insert(idx);
                return true;
            }
            
            if (isDescendantOfModelRoot(parentIdx)) {
                modelDescendantIndices.insert(idx);
                return true;
            }
            
            return false;
        };
        
        for (size_t i = 0; i < asset.Entities.size(); ++i) {
            isDescendantOfModelRoot(i);
        }
        
        if (!modelDescendantIndices.empty()) {
            std::cout << "[Prefab LOAD] Pre-scan: Found " << modelRootIndices.size() << " model roots, "
                      << modelDescendantIndices.size() << " model descendants to skip\n";
            // DEBUG: Log all model descendants that will be skipped
            for (size_t idx : modelDescendantIndices) {
                const auto& entJson = asset.Entities[idx];
                std::string name = entJson.value("name", "???");
                int pIdx = parentIndexMap[idx];
                std::cout << "[Prefab LOAD] Will skip descendant idx=" << idx << " name='" << name 
                          << "' parentIdx=" << pIdx << "\n";
            }
        }
    }
    
    //--------------------------------------------------------------------------
    // Pass 1: Create all entities
    //--------------------------------------------------------------------------
    const bool reuseRootRequested = useExistingRoot && existingRoot != INVALID_ENTITY_ID && dst.GetEntityData(existingRoot) != nullptr;
    auto isPrefabRootEntity = [&](const ClaymoreGUID& guid, size_t index) {
        if (asset.RootGuid.high != 0 || asset.RootGuid.low != 0) {
            return guid.high == asset.RootGuid.high && guid.low == asset.RootGuid.low;
        }
        return index == 0;
    };
    std::cout << "[Prefab LOAD] Starting Pass 1 - " << asset.Entities.size() << " entities in prefab JSON\n";
    for (size_t entityIndex = 0; entityIndex < asset.Entities.size(); ++entityIndex) {
        const auto& entityJson = asset.Entities[entityIndex];
        if (!entityJson.is_object()) continue;
        
        std::string name = entityJson.value("name", "Entity");
        
        // Skip model descendants - they're created by InstantiateModel()
        if (modelDescendantIndices.count(entityIndex)) {
            std::cout << "[Prefab LOAD] Skipping model descendant idx=" << entityIndex 
                      << " name='" << name << "'\n";
            continue;
        }
        ClaymoreGUID guid{};
        try { if (entityJson.contains("guid")) entityJson.at("guid").get_to(guid); } catch (...) {}
        
        bool reuseRoot = reuseRootRequested && isPrefabRootEntity(guid, entityIndex);
        bool handledAsModelAsset = false;
        
        // Handle model asset blocks - both old and new format prefabs use this
        // Model roots have an "asset" block that triggers InstantiateModel()
        if (entityJson.contains("asset") && entityJson["asset"].is_object()) {
            const auto& assetBlock = entityJson["asset"];
            if (assetBlock.value("type", "") == "model") {
                std::string modelPath = assetBlock.value("path", "");
                std::string resolved = modelPath;
                if (!resolved.empty() && !fs::exists(resolved)) {
                    resolved = (GetProjectDirectory() / modelPath).string();
                }
                for (char& c : resolved) if (c == '\\') c = '/';
                
#ifdef CLAYMORE_EDITOR
                // Register in asset library
                try {
                    std::string guidStr = assetBlock.value("guid", "");
                    if (!guidStr.empty()) {
                        ClaymoreGUID modelGuid = ClaymoreGUID::FromString(guidStr);
                        if (!(modelGuid.high == 0 && modelGuid.low == 0)) {
                            std::string vpath = modelPath;
                            for (char& ch : vpath) if (ch == '\\') ch = '/';
                            AssetLibrary::Instance().RegisterAsset(
                                AssetReference(modelGuid, 0, (int)AssetType::Mesh),
                                AssetType::Mesh, vpath, vpath);
                        }
                    }
                } catch (...) {}
#endif
                
                // Try .meta fast path
                std::string metaPath = resolved;
                fs::path rp(resolved);
                fs::path mp = rp.parent_path() / (rp.stem().string() + ".meta");
                if (fs::exists(mp)) metaPath = mp.string();
                
                EntityID modelId = INVALID_ENTITY_ID;
                if (fs::path(metaPath).extension() == ".meta") {
                    modelId = reuseRoot
                        ? dst.InstantiateModelFast(metaPath, glm::vec3(0.0f), true, existingRoot)
                        : dst.InstantiateModelFast(metaPath, glm::vec3(0.0f), true);
                }
                if (modelId == INVALID_ENTITY_ID) {
                    modelId = reuseRoot
                        ? dst.InstantiateModel(resolved, glm::vec3(0.0f), existingRoot)
                        : dst.InstantiateModel(resolved, glm::vec3(0.0f));
                }
                
                if (modelId != INVALID_ENTITY_ID) {
                    std::cout << "[Prefab LOAD] Instantiated MODEL ROOT idx=" << entityIndex 
                              << " name='" << name << "' -> newId=" << modelId << "\n";
                    auto* md = dst.GetEntityData(modelId);
                    if (md) {
                        dst.SetEntityName(modelId, name);
                        
                        // Generate new unique GUID for this instance to avoid collisions
                        ClaymoreGUID instanceGuid = ClaymoreGUID::Generate();
                        md->EntityGuid = instanceGuid;
                        md->PrefabGuid = asset.Guid;
                        
                        // Track GUID remapping
                        if (guid.high != 0 || guid.low != 0) {
                            prefabToInstanceGuid[guid] = instanceGuid;
                            instanceToPrefabGuid[instanceGuid] = guid;
                        }
                        
                        // Apply saved components - these are user-added components
                        // that don't conflict with model structure (mesh, skeleton, skinning)
                        if (entityJson.contains("transform")) {
                            Serializer::DeserializeTransform(entityJson["transform"], md->Transform);
                            dst.MarkTransformDirty(modelId);
                        }
                        if (entityJson.contains("scripts")) {
                            Serializer::DeserializeScripts(entityJson["scripts"], md->Scripts, dst, false);
                        }
                        if (entityJson.contains("animator") && !md->AnimationPlayer) {
                            md->AnimationPlayer = std::make_unique<cm::animation::AnimationPlayerComponent>();
                            Serializer::DeserializeAnimator(entityJson["animator"], *md->AnimationPlayer);
                        }
                        if (entityJson.contains("active")) dst.SetEntityActive(modelId, entityJson["active"].get<bool>());
                        if (entityJson.contains("visible")) dst.SetEntityVisibleDirect(modelId, entityJson["visible"].get<bool>());
                        if (entityJson.contains("layer")) dst.SetEntityLayer(modelId, entityJson["layer"].get<int>());
                        if (entityJson.contains("tag") && entityJson["tag"].is_string()) {
                            dst.SetEntityTag(modelId, entityJson["tag"].get<std::string>());
                        }
                        
                        // Physics components
                        if (entityJson.contains("collider")) {
                            if (!md->Collider) md->Collider = std::make_unique<ColliderComponent>();
                            Serializer::DeserializeCollider(entityJson["collider"], *md->Collider);
                        }
                        if (entityJson.contains("rigidbody")) {
                            if (!md->RigidBody) md->RigidBody = std::make_unique<RigidBodyComponent>();
                            Serializer::DeserializeRigidBody(entityJson["rigidbody"], *md->RigidBody);
                        }
                        if (entityJson.contains("staticbody")) {
                            if (!md->StaticBody) md->StaticBody = std::make_unique<StaticBodyComponent>();
                            Serializer::DeserializeStaticBody(entityJson["staticbody"], *md->StaticBody);
                        }
                        if (entityJson.contains("characterController")) {
                            if (!md->CharacterController) md->CharacterController = std::make_unique<CharacterControllerComponent>();
                            Serializer::DeserializeCharacterController(entityJson["characterController"], *md->CharacterController);
                        }
                        
                        // Navigation components
                        if (entityJson.contains("navagent")) {
                            if (!md->NavAgent) md->NavAgent = std::make_unique<nav::NavAgentComponent>();
                            Serializer::DeserializeNavAgent(entityJson["navagent"], *md->NavAgent);
                        }
                        if (entityJson.contains("navlink")) {
                            if (!md->NavLink) md->NavLink = std::make_unique<nav::NavLinkComponent>();
                            Serializer::DeserializeNavLink(entityJson["navlink"], *md->NavLink);
                        }
                        if (entityJson.contains("navmesh")) {
                            if (!md->Navigation) md->Navigation = std::make_unique<nav::NavMeshComponent>();
                            Serializer::DeserializeNavMesh(entityJson["navmesh"], *md->Navigation);
                        }
                        
                        // Visual/rendering components
                        if (entityJson.contains("tintController")) {
                            if (!md->TintController) md->TintController = std::make_unique<TintMaskController>();
                            Serializer::DeserializeTintController(entityJson["tintController"], *md->TintController);
                            md->TintController->NeedsRefresh = true;
                        }
                        if (entityJson.contains("renderOverrides")) {
                            if (!md->RenderOverrides) md->RenderOverrides = std::make_unique<RenderOverridesComponent>();
                            Serializer::DeserializeRenderOverrides(entityJson["renderOverrides"], *md->RenderOverrides);
                        }
                        if (entityJson.contains("light")) {
                            if (!md->Light) md->Light = std::make_unique<LightComponent>();
                            Serializer::DeserializeLight(entityJson["light"], *md->Light);
                        }
                        if (entityJson.contains("audioSource")) {
                            if (!md->AudioSource) md->AudioSource = std::make_unique<AudioSourceComponent>();
                            Serializer::DeserializeAudioSource(entityJson["audioSource"], *md->AudioSource);
                        }
                        if (entityJson.contains("audioListener")) {
                            if (!md->AudioListener) md->AudioListener = std::make_unique<AudioListenerComponent>();
                            Serializer::DeserializeAudioListener(entityJson["audioListener"], *md->AudioListener);
                        }
                        if (entityJson.contains("camera")) {
                            if (!md->Camera) md->Camera = std::make_unique<CameraComponent>();
                            Serializer::DeserializeCamera(entityJson["camera"], *md->Camera);
                        }
                        
                        // Particle/effects
                        if (entityJson.contains("emitter")) {
                            if (!md->Emitter) md->Emitter = std::make_unique<ParticleEmitterComponent>();
                            Serializer::DeserializeParticleEmitter(entityJson["emitter"], *md->Emitter);
                        }
                        
                        // Model-specific data
                        if (entityJson.contains("deletedModelNodes")) {
                            md->DeletedModelNodes.clear();
                            for (const auto& path : entityJson["deletedModelNodes"]) {
                                if (path.is_string()) {
                                    std::string p = path.get<std::string>();
                                    if (!p.empty()) md->DeletedModelNodes.push_back(std::move(p));
                                }
                            }
                        }
                        if (entityJson.contains("modelAssetGuid")) {
                            try { entityJson.at("modelAssetGuid").get_to(md->ModelAssetGuid); } catch (...) {}
                        }
                        
                        // Apply model delta if present (handles added children with skinning/bone attachments)
                        // Note: children should be an array of delta objects, not integer entity IDs
                        // Legacy broken prefabs may have integer children - skip those
                        if (entityJson.contains("children") && entityJson["children"].is_array()) {
                            const auto& childrenArr = entityJson["children"];
                            // Validate that children contains delta objects, not integer IDs
                            bool hasValidDelta = !childrenArr.empty() && childrenArr[0].is_object();
                            if (hasValidDelta) {
                                prefab::ApplyModelDeltaFromJson(dst, modelId, childrenArr);
                            } else if (!childrenArr.empty()) {
                                std::cerr << "[Prefab] WARNING: Model root '" << name 
                                          << "' has integer children instead of delta objects. "
                                          << "Re-save the prefab to fix.\n";
                            }
                        }
                    }
                    // Map from PREFAB GUID to EntityID for internal reference resolution
                    guidToId[pack(guid)] = modelId;
                    // Index-based mapping for parentIndex hierarchy (legacy support)
                    indexToEntityId[entityIndex] = modelId;
                    // UNIFIED: Map old EntityID to new EntityID (like scene loading)
                    if (entityJson.contains("id")) {
                        EntityID oldId = entityJson["id"].get<EntityID>();
                        idMapping[oldId] = modelId;
                    }
                    // Track as opaque root for nested model parent resolution
                    opaqueRoots.insert(modelId);
                    handledAsModelAsset = true;
                }
            }
        }
        
        if (!handledAsModelAsset) {
            // Create regular entity
            EntityID id = INVALID_ENTITY_ID;
            EntityData* data = nullptr;
            bool reuseEntity = reuseRoot;
            if (reuseEntity) {
                id = existingRoot;
                data = dst.GetEntityData(id);
                if (!data) {
                    reuseEntity = false;
                } else {
                    dst.SetEntityName(id, name);
                }
            }
            if (!reuseEntity) {
                Entity ent = dst.CreateEntityExact(name);
                id = ent.GetID();
                data = dst.GetEntityData(id);
            }
            if (!data) continue;
            
            // DEBUG: Log regular entity creation
            EntityID jsonParent = INVALID_ENTITY_ID;
            int jsonParentIdx = -1;
            if (entityJson.contains("parent")) jsonParent = entityJson["parent"].get<EntityID>();
            if (entityJson.contains("parentIndex")) jsonParentIdx = entityJson["parentIndex"].get<int>();
            std::cout << "[Prefab LOAD] Created REGULAR entity idx=" << entityIndex 
                      << " name='" << name << "' -> newId=" << id 
                      << " (jsonParent=" << jsonParent << ", jsonParentIdx=" << jsonParentIdx << ")\n";
            
            // Generate new unique GUID for this instance to avoid collisions
            // with other instances of the same prefab in the scene
            ClaymoreGUID instanceGuid = ClaymoreGUID::Generate();
            data->EntityGuid = instanceGuid;
            data->PrefabGuid = asset.Guid;
            
            // Track GUID remapping
            if (guid.high != 0 || guid.low != 0) {
                prefabToInstanceGuid[guid] = instanceGuid;
                instanceToPrefabGuid[instanceGuid] = guid;
            }
            
            // Map from PREFAB GUID to EntityID for internal reference resolution
            guidToId[pack(guid)] = id;
            // Index-based mapping for parentIndex hierarchy (legacy support)
            indexToEntityId[entityIndex] = id;
            // UNIFIED: Map old EntityID to new EntityID (like scene loading)
            if (entityJson.contains("id")) {
                EntityID oldId = entityJson["id"].get<EntityID>();
                idMapping[oldId] = id;
            }
        }
    }
    
    //--------------------------------------------------------------------------
    // Pass 2: Set up hierarchy - UNIFIED with scene loading
    // Uses id/parent fields (same as scene serialization) with idMapping
    // Falls back to parentIndex for legacy prefabs
    //--------------------------------------------------------------------------
    for (size_t entityIndex = 0; entityIndex < asset.Entities.size(); ++entityIndex) {
        const auto& entityJson = asset.Entities[entityIndex];
        if (!entityJson.is_object()) continue;
        
        EntityID id = indexToEntityId[entityIndex];
        if (id == INVALID_ENTITY_ID) continue;
        
        // Skip opaque roots (model roots) - their hierarchy is already set up
        if (opaqueRoots.find(id) != opaqueRoots.end()) continue;
        
        std::string entityName = entityJson.value("name", "???");
        
        // UNIFIED: Use parent field (EntityID) and idMapping - SAME AS SCENE LOADING
        if (entityJson.contains("parent")) {
            EntityID oldParent = entityJson["parent"].get<EntityID>();
            if (oldParent != INVALID_ENTITY_ID && oldParent != (EntityID)-1) {
                auto itP = idMapping.find(oldParent);
                if (itP != idMapping.end()) {
                    std::cout << "[Prefab LOAD] Pass2: Setting parent via 'parent' field: entity=" << id 
                              << " ('" << entityName << "') -> parent=" << itP->second 
                              << " (oldParent=" << oldParent << ")\n";
                    dst.SetParent(id, itP->second);
                } else {
                    std::cout << "[Prefab LOAD] Pass2: Parent mapping MISSING for entity=" << id 
                              << " ('" << entityName << "') oldParent=" << oldParent << "\n";
                }
            }
        }
        // Fallback: support legacy parentIndex for older prefabs
        else if (entityJson.contains("parentIndex")) {
            int parentIndex = entityJson["parentIndex"].get<int>();
            if (parentIndex >= 0 && parentIndex < static_cast<int>(indexToEntityId.size())) {
                EntityID parentId = indexToEntityId[parentIndex];
                if (parentId != INVALID_ENTITY_ID) {
                    std::cout << "[Prefab LOAD] Pass2: Setting parent via 'parentIndex': entity=" << id 
                              << " ('" << entityName << "') -> parent=" << parentId 
                              << " (parentIndex=" << parentIndex << ")\n";
                    dst.SetParent(id, parentId);
                } else {
                    std::cout << "[Prefab LOAD] Pass2: parentIndex=" << parentIndex 
                              << " resolved to INVALID for entity=" << id << " ('" << entityName << "')\n";
                }
            }
        }
        // Fallback: support legacy _parentGuid for older prefabs
        else if (entityJson.contains("_parentGuid")) {
            ClaymoreGUID parentGuid{};
            try { entityJson.at("_parentGuid").get_to(parentGuid); } catch (...) {}
            if (parentGuid.high != 0 || parentGuid.low != 0) {
                auto itP = guidToId.find(pack(parentGuid));
                if (itP != guidToId.end()) dst.SetParent(id, itP->second);
            }
        }
        
        // Skip legacy model assets for component deserialization (already handled in Pass 1)
        if (entityJson.contains("asset") && entityJson["asset"].is_object()) {
            if (entityJson["asset"].value("type", "") == "model") {
                continue;
            }
        }
        
        auto* data = dst.GetEntityData(id);
        if (!data) continue;
        
        // USE THE SAME DESERIALIZATION AS SCENE LOADING - TRUE PARITY
        // This single call replaces ALL the duplicate component deserialization code
        Serializer::DeserializeEntityData(entityJson, data, dst, false);
        dst.MarkTransformDirty(id);
    }
    
    // REMOVED: 200+ lines of duplicate component deserialization code
    // DeserializeEntityData handles ALL components with the SAME logic as scene loading
    
    //--------------------------------------------------------------------------
    // Pass 3: Post-processing (skeletons, skinning)
    //--------------------------------------------------------------------------
    for (const auto& entityJson : asset.Entities) {
        ClaymoreGUID guid{};
        try { if (entityJson.contains("guid")) entityJson.at("guid").get_to(guid); } catch (...) {}
        
        auto itId = guidToId.find(pack(guid));
        if (itId == guidToId.end()) continue;
        EntityID id = itId->second;
        auto* data = dst.GetEntityData(id);
        if (!data) continue;
        
        // Generate bone entities for skeletons
        if (data->Skeleton) {
            GenerateBoneEntitiesForSkeleton(dst, id);
            if (!data->Skeleton->Avatar) {
                data->Skeleton->Avatar = std::make_unique<cm::animation::AvatarDefinition>();
                cm::animation::avatar_builders::BuildFromSkeleton(*data->Skeleton, *data->Skeleton->Avatar, true);
            }
        }
        
        // Resolve skinning root
        if (data->Skinning) {
            data->Skinning->SkeletonRoot = INVALID_ENTITY_ID;
            EntityID p = data->Parent;
            while (p != INVALID_ENTITY_ID) {
                auto* pd = dst.GetEntityData(p);
                if (!pd) break;
                if (pd->Skeleton) {
                    data->Skinning->SkeletonRoot = p;
                    break;
                }
                p = pd->Parent;
            }
        }
    }
    
    //--------------------------------------------------------------------------
    // Pass 4: Fix up nested models with path-based parent references
    // UNIFIED WITH SCENE LOADING - handles models parented inside other model hierarchies
    // (e.g., armor parented under skeleton root)
    //--------------------------------------------------------------------------
    for (const auto& entityJson : asset.Entities) {
        if (!entityJson.contains("_parentModelGuid") || !entityJson.contains("_parentPath")) continue;
        if (!entityJson.contains("id")) continue;
        
        EntityID oldId = entityJson["id"].get<EntityID>();
        auto itChild = idMapping.find(oldId);
        if (itChild == idMapping.end()) continue;
        EntityID childNew = itChild->second;
        if (childNew == INVALID_ENTITY_ID || childNew == (EntityID)-1) continue;
        
        std::string parentModelGuid = entityJson["_parentModelGuid"].get<std::string>();
        std::string parentPath = entityJson["_parentPath"].get<std::string>();
        
        // Get old parent model ID for disambiguation (when multiple models have same GUID)
        EntityID oldParentModelId = (EntityID)-1;
        if (entityJson.contains("_parentModelId")) {
            oldParentModelId = entityJson["_parentModelId"].get<EntityID>();
        }
        
        // Find the model root with matching GUID in our newly instantiated models
        EntityID parentModelRoot = (EntityID)-1;
        EntityID guidOnlyMatch = (EntityID)-1;  // Fallback if no ID match
        
        for (EntityID opaqueRoot : opaqueRoots) {
            auto* rootData = dst.GetEntityData(opaqueRoot);
            if (!rootData) continue;
            if (rootData->ModelAssetGuid.ToString() == parentModelGuid) {
                // GUID matches - check if old ID also matches
                if (oldParentModelId != (EntityID)-1) {
                    auto it = idMapping.find(oldParentModelId);
                    if (it != idMapping.end() && it->second == opaqueRoot) {
                        parentModelRoot = opaqueRoot;
                        break;
                    }
                }
                // Store as fallback if no perfect match found yet
                if (guidOnlyMatch == (EntityID)-1) {
                    guidOnlyMatch = opaqueRoot;
                }
            }
        }
        
        // Use GUID-only match as fallback
        if (parentModelRoot == (EntityID)-1) {
            parentModelRoot = guidOnlyMatch;
        }
        
        if (parentModelRoot == (EntityID)-1) {
            std::cout << "[Prefab] Could not find model root with GUID=" << parentModelGuid << std::endl;
            continue;
        }
        
        // Helper to strip trailing numeric suffix (e.g., "SkeletonRoot_97" -> "SkeletonRoot")
        auto stripNumericSuffix = [](const std::string& name) -> std::string {
            size_t us = name.find_last_of('_');
            if (us == std::string::npos || us == 0) return name;
            bool allDigits = true;
            for (size_t i = us + 1; i < name.size(); ++i) {
                if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
                    allDigits = false;
                    break;
                }
            }
            return allDigits ? name.substr(0, us) : name;
        };
        
        // Resolve the path inside the model hierarchy
        auto resolvePathInModel = [&](EntityID modelRoot, const std::string& path) -> EntityID {
            if (path.empty()) return modelRoot;
            
            std::vector<std::string> parts;
            std::string current;
            for (char ch : path) {
                if (ch == '/') {
                    if (!current.empty()) parts.push_back(current);
                    current.clear();
                } else {
                    current += ch;
                }
            }
            if (!current.empty()) parts.push_back(current);
            
            EntityID cur = modelRoot;
            auto* curData = dst.GetEntityData(cur);
            if (!curData) return (EntityID)-1;
            
            // Start searching from model root's children
            for (const std::string& part : parts) {
                bool found = false;
                std::string partBase = stripNumericSuffix(part);
                
                // First try exact match
                for (EntityID child : curData->Children) {
                    auto* childData = dst.GetEntityData(child);
                    if (childData && childData->Name == part) {
                        cur = child;
                        curData = childData;
                        found = true;
                        break;
                    }
                }
                
                // If not found, try matching with stripped suffixes
                if (!found) {
                    for (EntityID child : curData->Children) {
                        auto* childData = dst.GetEntityData(child);
                        if (childData) {
                            std::string childBase = stripNumericSuffix(childData->Name);
                            if (childBase == partBase) {
                                cur = child;
                                curData = childData;
                                found = true;
                                std::cout << "[Prefab] Fuzzy matched '" << part << "' to '" << childData->Name << "'" << std::endl;
                                break;
                            }
                        }
                    }
                }
                
                if (!found) {
                    std::cout << "[Prefab] Could not find path part '" << part << "' in model hierarchy" << std::endl;
                    return (EntityID)-1;
                }
            }
            return cur;
        };
        
        EntityID resolvedParent = resolvePathInModel(parentModelRoot, parentPath);
        if (resolvedParent != (EntityID)-1) {
            dst.SetParent(childNew, resolvedParent);
            try {
                auto* childData = dst.GetEntityData(childNew);
                auto* parentData = dst.GetEntityData(resolvedParent);
                std::cout << "[Prefab] Parented '" << (childData ? childData->Name : "?") 
                          << "' under '" << (parentData ? parentData->Name : "?") 
                          << "' via path='" << parentPath << "'" << std::endl;
            } catch(...) {}
        }
    }
    
    //--------------------------------------------------------------------------
    // Determine root and attach PrefabInstanceComponent
    //--------------------------------------------------------------------------
    EntityID root = INVALID_ENTITY_ID;
    auto itRoot = guidToId.find(pack(asset.RootGuid));
    if (itRoot != guidToId.end()) {
        root = itRoot->second;
    } else {
        // Fall back deterministically to the first created top-level entity.
        // Never pick an arbitrary hash-map element here; that can silently
        // turn a root-guid mismatch into a broken partial prefab instance.
        for (EntityID candidate : indexToEntityId) {
            if (candidate == INVALID_ENTITY_ID) continue;
            auto* candidateData = dst.GetEntityData(candidate);
            if (!candidateData) continue;
            if (candidateData->Parent == INVALID_ENTITY_ID || candidateData->Parent == (EntityID)-1) {
                root = candidate;
                break;
            }
        }

        if (root == INVALID_ENTITY_ID) {
            for (EntityID candidate : indexToEntityId) {
                if (candidate != INVALID_ENTITY_ID) {
                    root = candidate;
                    break;
                }
            }
        }

        if (root != INVALID_ENTITY_ID) {
            std::cerr << "[Prefab] WARNING: Root GUID " << asset.RootGuid.ToString()
                      << " was not found in prefab entities. Falling back to entity "
                      << root << " instead.\n";
        }
    }
    
    if (root != INVALID_ENTITY_ID) {
        auto* rootData = dst.GetEntityData(root);
        
        // Make copies of GUID mappings before moving for script entity resolution
        auto prefabToInstanceGuidCopy = prefabToInstanceGuid;
        
        // Build instanceGuidToId map for resolution
        std::unordered_map<ClaymoreGUID, EntityID> instanceGuidToId;
        for (const auto& [packedGuid, id] : guidToId) {
            // The new instance GUID is stored in the entity data
            if (auto* d = dst.GetEntityData(id)) {
                instanceGuidToId[d->EntityGuid] = id;
            }
        }
        
        if (rootData) {
            // Create PrefabInstanceComponent
            rootData->PrefabInstance = std::make_unique<PrefabInstanceComponent>();
            rootData->PrefabInstance->PrefabAssetGuid = asset.Guid;
            rootData->PrefabInstance->PrefabPath = prefabPath;
            
            // Store GUID remapping for override resolution and serialization
            // This allows overrides to reference entities by original prefab GUIDs
            // while the scene uses unique instance GUIDs
            rootData->PrefabInstance->PrefabToInstanceGuid = std::move(prefabToInstanceGuid);
            rootData->PrefabInstance->InstanceToPrefabGuid = std::move(instanceToPrefabGuid);
            
            // Legacy field for compatibility
            rootData->PrefabSource = prefabPath;
        }
        
        // Collect the full instantiated subtree so model descendants are included.
        // This prevents internal refs to model nodes from being misclassified as external.
        std::vector<EntityID> createdEntities;
        std::vector<ClaymoreGUID> subtreeOwnedGuids;
        std::unordered_set<EntityID> visitedEntities;
        std::function<void(EntityID)> collectSubtree = [&](EntityID id) {
            if (id == INVALID_ENTITY_ID || visitedEntities.count(id)) return;
            visitedEntities.insert(id);
            createdEntities.push_back(id);
            if (auto* d = dst.GetEntityData(id)) {
                d->PrefabGuid = asset.Guid;
                subtreeOwnedGuids.push_back(d->EntityGuid);
                for (EntityID child : d->Children) {
                    collectSubtree(child);
                }
            }
        };
        if (root != INVALID_ENTITY_ID) {
            collectSubtree(root);
        }
        if (rootData && rootData->PrefabInstance) {
            rootData->PrefabInstance->OwnedEntityGuids = std::move(subtreeOwnedGuids);
        }
        
        if (!createdEntities.empty()) {
            std::cout << "[Prefab] Running post-processing on " << createdEntities.size() << " entities\n";
            cm::PostProcessEntities(dst, createdEntities);
        }
        
        // Remap entity references in all components including script fields
        // Uses idMapping to remap internal references, clears external references
        // CRITICAL: Pass GUID mappings to enable GUID-based script reference resolution
        if (!idMapping.empty() && !createdEntities.empty()) {
            RemapPrefabEntityReferences(dst, createdEntities, idMapping, &prefabToInstanceGuidCopy, &instanceGuidToId);
        }
        
        // Resolve script entity references using GUID remapping
        // This handles references that couldn't be resolved during deserialization
        // because entity GUIDs are different in the instantiated prefab
        // CRITICAL FIX: Always attempt resolution, especially for root entity script references
        if (!prefabToInstanceGuidCopy.empty() && asset.Entities.is_array()) {
            ResolvePrefabScriptEntityReferences(dst, root, asset.Entities, 
                                                 prefabToInstanceGuidCopy, instanceGuidToId, &createdEntities);
        }

        if (!createdEntities.empty()) {
            std::cout << "[Prefab] Re-running post-processing after prefab reference remap on "
                      << createdEntities.size() << " entities\n";
            cm::PostProcessEntities(dst, createdEntities);
        }
        
        dst.MarkTransformDirty(root);
        dst.UpdateTransforms();

        // Only propagate explicit entity-level hidden state. Component-level UI visibility
        // (e.g. Panel.Visible) should not collapse an entire entity subtree.
        auto isEntityHidden = [](EntityData* d) -> bool {
            return d && !d->Visible;
        };
        // Propagate visibility for each hidden entity to ensure its children are also hidden
        for (EntityID id : createdEntities) {
            auto* data = dst.GetEntityData(id);
            if (isEntityHidden(data)) {
                dst.SetEntityVisible(id, false);
            }
        }
        
        // Call OnValidate for all scripts in the prefab instance
        // This ensures scripts receive their serialized values and can initialize properly
        if (prefabPathForInstance != nullptr && IsDotnetRuntimeReady()) {
            CreatePrefabScriptInstances(dst, createdEntities);
            CallOnValidateForSubtree(dst, root);
        }
    }
    
    std::cout << "[Prefab] Instantiated: " << asset.Name << " (root=" << root << ")\n";
    return root;
}

//==============================================================================
// Instance Queries
//==============================================================================

bool IsPrefabInstanceRoot(EntityID entity, Scene& scene) {
    auto* data = scene.GetEntityData(entity);
    return data && data->PrefabInstance != nullptr;
}

bool IsPartOfPrefabInstance(EntityID entity, Scene& scene) {
    return GetPrefabInstanceRoot(entity, scene) != INVALID_ENTITY_ID;
}

EntityID GetPrefabInstanceRoot(EntityID entity, Scene& scene) {
    EntityID current = entity;
    while (current != INVALID_ENTITY_ID) {
        auto* data = scene.GetEntityData(current);
        if (!data) break;
        
        if (data->PrefabInstance) {
            // Check if the original entity is owned by this instance
            for (const auto& guid : data->PrefabInstance->OwnedEntityGuids) {
                auto* originalData = scene.GetEntityData(entity);
                if (originalData && originalData->EntityGuid == guid) {
                    return current;
                }
            }
        }
        
        current = data->Parent;
    }
    return INVALID_ENTITY_ID;
}

PrefabInstanceComponent* GetPrefabInstance(EntityID entity, Scene& scene) {
    auto* data = scene.GetEntityData(entity);
    return data ? data->PrefabInstance.get() : nullptr;
}

//==============================================================================
// Override Management
//==============================================================================

void ApplyPrefabOverrides(EntityID instanceRoot, Scene& scene) {
    auto* data = scene.GetEntityData(instanceRoot);
    if (!data) return;
    
    // Build instance GUID -> EntityID map for the instance subtree
    std::unordered_map<uint64_t, EntityID> instanceGuidToId;
    auto pack = [](const ClaymoreGUID& g)->uint64_t { return PackGuidHash(g); };
    
    std::function<void(EntityID)> buildMap = [&](EntityID id) {
        auto* d = scene.GetEntityData(id);
        if (!d) return;
        instanceGuidToId[pack(d->EntityGuid)] = id;
        for (EntityID c : d->Children) buildMap(c);
    };
    buildMap(instanceRoot);
    
    // Get overrides - either from PrefabInstanceComponent or passed in
    const std::vector<prefab::PropertyOverride>* overrides = nullptr;
    const std::unordered_map<ClaymoreGUID, ClaymoreGUID>* prefabToInstance = nullptr;
    if (data->PrefabInstance) {
        overrides = &data->PrefabInstance->Overrides;
        prefabToInstance = &data->PrefabInstance->PrefabToInstanceGuid;
    }
    if (!overrides || overrides->empty()) return;
    
    std::cout << "[Prefab] Applying " << overrides->size() << " overrides to instance\n";
    
    for (const auto& ov : *overrides) {
        // Overrides reference original PREFAB GUIDs, but entities now have INSTANCE GUIDs
        // Use the GUID remapping to find the correct target
        ClaymoreGUID lookupGuid = ov.TargetEntityGuid;
        if (prefabToInstance && !prefabToInstance->empty()) {
            auto remapIt = prefabToInstance->find(ov.TargetEntityGuid);
            if (remapIt != prefabToInstance->end()) {
                lookupGuid = remapIt->second; // Use remapped instance GUID
            }
        }
        
        auto it = instanceGuidToId.find(pack(lookupGuid));
        if (it == instanceGuidToId.end()) {
            std::cerr << "[Prefab] Override target not found: " << ov.TargetEntityGuid.ToString() << "\n";
            continue;
        }
        
        EntityID targetId = it->second;
        auto* targetData = scene.GetEntityData(targetId);
        if (!targetData) continue;
        
        // Apply the override based on component key
        const std::string& key = ov.ComponentKey;
        const json& val = ov.Value;
        
        try {
            if (val.is_null()) {
                if (key == "mesh") targetData->Mesh.reset();
                else if (key == "meshProxy") targetData->MeshProxy.reset();
                else if (key == "light") targetData->Light.reset();
                else if (key == "camera") targetData->Camera.reset();
                else if (key == "collider") targetData->Collider.reset();
                else if (key == "rigidbody") targetData->RigidBody.reset();
                else if (key == "staticbody") targetData->StaticBody.reset();
                else if (key == "softbody") targetData->Softbody.reset();
                else if (key == "characterController") targetData->CharacterController.reset();
                else if (key == "area") targetData->Area.reset();
                else if (key == "emitter") targetData->Emitter.reset();
                else if (key == "animator") targetData->AnimationPlayer.reset();
                else if (key == "scripts") targetData->Scripts.clear();
                else if (key == "canvas") targetData->Canvas.reset();
                else if (key == "panel") targetData->Panel.reset();
                else if (key == "button") targetData->Button.reset();
                else if (key == "slider") targetData->Slider.reset();
                else if (key == "progressBar") targetData->ProgressBar.reset();
                else if (key == "toggle") targetData->Toggle.reset();
                else if (key == "scrollView") targetData->ScrollView.reset();
                else if (key == "layoutGroup") targetData->LayoutGroup.reset();
                else if (key == "inputField") targetData->InputField.reset();
                else if (key == "dropdown") targetData->Dropdown.reset();
                else if (key == "uiRect") targetData->UIRect.reset();
                else if (key == "fitToContent") targetData->FitToContent.reset();
                else if (key == "uiSceneCapture") targetData->UISceneCapture.reset();
                else if (key == "text") targetData->Text.reset();
                else if (key == "renderOverrides") targetData->RenderOverrides.reset();
                else if (key == "navmesh") targetData->Navigation.reset();
                else if (key == "navagent") targetData->NavAgent.reset();
                else if (key == "navlink") targetData->NavLink.reset();
                else if (key == "portal") targetData->Portal.reset();
                else if (key == "terrain") targetData->Terrain.reset();
                else if (key == "resourceLayers") targetData->ResourceLayers.reset();
                else if (key == "instancer") targetData->Instancer.reset();
                else if (key == "river") targetData->River.reset();
                else if (key == "spline") targetData->Spline.reset();
                else if (key == "grassDeformer") targetData->GrassDeformer.reset();
                else if (key == "skeleton") targetData->Skeleton.reset();
                else if (key == "skinning") targetData->Skinning.reset();
                else if (key == "boneAttachment") targetData->BoneAttachment.reset();
                else if (key == "blendShapeWeights") {
                    targetData->PendingBlendShapeWeights.clear();
                    if (targetData->BlendShapes) {
                        for (auto& shape : targetData->BlendShapes->Shapes) {
                            shape.Weight = 0.0f;
                        }
                        targetData->BlendShapes->Dirty = true;
                    }
                }
                else if (key == "unifiedMorphWeights") {
                    targetData->PendingUnifiedMorphWeights.clear();
                    if (targetData->UnifiedMorph) {
                        targetData->UnifiedMorph->Weights.assign(targetData->UnifiedMorph->Weights.size(), 0.0f);
                    }
                }
                else if (key == "tintController") targetData->TintController.reset();
                else if (key == "ik") targetData->IKs.clear();
                else if (key == "lookat") targetData->LookAtConstraints.clear();
                else if (key == "modelAssetGuid") targetData->ModelAssetGuid = {};
                else if (key == "deletedModelNodes") targetData->DeletedModelNodes.clear();
                else if (key == "Dynamic") targetData->Dynamic.clear();
                else {
                    std::cerr << "[Prefab] Unknown null override key: " << key << "\n";
                }
                continue;
            }

            if (key == "name") {
                scene.SetEntityName(targetId, val.get<std::string>());
            }
            else if (key == "active") {
                scene.SetEntityActive(targetId, val.get<bool>());
            }
            else if (key == "visible") {
                scene.SetEntityVisibleDirect(targetId, val.get<bool>());
            }
            else if (key == "layer") {
                scene.SetEntityLayer(targetId, val.get<int>());
            }
            else if (key == "tag") {
                scene.SetEntityTag(targetId, val.get<std::string>());
            }
            else if (key == "transform") {
                Serializer::DeserializeTransform(val, targetData->Transform);
                scene.MarkTransformDirty(targetId);
            }
            else if (key == "mesh") {
                if (!targetData->Mesh) targetData->Mesh = std::make_unique<MeshComponent>();
                json meshOverride = val;
                if (meshOverride.is_object() && targetData->Mesh->mesh) {
                    // Preserve the mesh chosen by fresh model instantiation. Prefab overrides can
                    // carry stale meshReference/fileID values after the source model is re-exported.
                    meshOverride.erase("meshReference");
                    meshOverride.erase("fileID");
                }
                Serializer::DeserializeMesh(meshOverride, *targetData->Mesh);
            }
            else if (key == "light") {
                if (!targetData->Light) targetData->Light = std::make_unique<LightComponent>();
                Serializer::DeserializeLight(val, *targetData->Light);
            }
            else if (key == "camera") {
                if (!targetData->Camera) targetData->Camera = std::make_unique<CameraComponent>();
                Serializer::DeserializeCamera(val, *targetData->Camera);
            }
            else if (key == "collider") {
                if (!targetData->Collider) targetData->Collider = std::make_unique<ColliderComponent>();
                Serializer::DeserializeCollider(val, *targetData->Collider);
            }
            else if (key == "rigidbody") {
                if (!targetData->RigidBody) targetData->RigidBody = std::make_unique<RigidBodyComponent>();
                Serializer::DeserializeRigidBody(val, *targetData->RigidBody);
            }
            else if (key == "staticbody") {
                if (!targetData->StaticBody) targetData->StaticBody = std::make_unique<StaticBodyComponent>();
                Serializer::DeserializeStaticBody(val, *targetData->StaticBody);
            }
            else if (key == "softbody") {
                if (!targetData->Softbody) targetData->Softbody = std::make_unique<SoftbodyComponent>();
                Serializer::DeserializeSoftbody(val, *targetData->Softbody);
            }
            else if (key == "characterController") {
                if (!targetData->CharacterController) targetData->CharacterController = std::make_unique<CharacterControllerComponent>();
                Serializer::DeserializeCharacterController(val, *targetData->CharacterController);
            }
            else if (key == "area") {
                if (!targetData->Area) targetData->Area = std::make_unique<cm::physics::AreaComponent>();
                Serializer::DeserializeArea(val, *targetData->Area);
            }
            else if (key == "emitter") {
                if (!targetData->Emitter) targetData->Emitter = std::make_unique<ParticleEmitterComponent>();
                Serializer::DeserializeParticleEmitter(val, *targetData->Emitter);
            }
            else if (key == "animator") {
                if (!targetData->AnimationPlayer) targetData->AnimationPlayer = std::make_unique<cm::animation::AnimationPlayerComponent>();
                Serializer::DeserializeAnimator(val, *targetData->AnimationPlayer);
            }
            else if (key == "scripts") {
                Serializer::DeserializeScripts(val, targetData->Scripts, scene);
            }
            else if (key == "canvas") {
                if (!targetData->Canvas) targetData->Canvas = std::make_unique<CanvasComponent>();
                Serializer::DeserializeCanvas(val, *targetData->Canvas);
            }
            else if (key == "panel") {
                if (!targetData->Panel) targetData->Panel = std::make_unique<PanelComponent>();
                Serializer::DeserializePanel(val, *targetData->Panel);
            }
            else if (key == "button") {
                if (!targetData->Button) targetData->Button = std::make_unique<ButtonComponent>();
                Serializer::DeserializeButton(val, *targetData->Button);
            }
            else if (key == "text") {
                if (!targetData->Text) targetData->Text = std::make_unique<TextRendererComponent>();
                Serializer::DeserializeText(val, *targetData->Text);
            }
            else if (key == "renderOverrides") {
                if (!targetData->RenderOverrides) targetData->RenderOverrides = std::make_unique<RenderOverridesComponent>();
                Serializer::DeserializeRenderOverrides(val, *targetData->RenderOverrides);
            }
            else if (key == "navmesh") {
                if (!targetData->Navigation) targetData->Navigation = std::make_unique<nav::NavMeshComponent>();
                Serializer::DeserializeNavMesh(val, *targetData->Navigation);
            }
            else if (key == "navagent") {
                if (!targetData->NavAgent) targetData->NavAgent = std::make_unique<nav::NavAgentComponent>();
                Serializer::DeserializeNavAgent(val, *targetData->NavAgent);
            }
            else if (key == "navlink") {
                if (!targetData->NavLink) targetData->NavLink = std::make_unique<nav::NavLinkComponent>();
                Serializer::DeserializeNavLink(val, *targetData->NavLink);
            }
            else if (key == "terrain") {
                if (!targetData->Terrain) targetData->Terrain = std::make_unique<TerrainComponent>();
                Serializer::DeserializeTerrain(val, *targetData->Terrain);
            }
            else if (key == "resourceLayers") {
                if (!targetData->ResourceLayers) targetData->ResourceLayers = std::make_unique<cm::resourcelayer::ResourceLayerComponent>();
                Serializer::DeserializeResourceLayers(val, *targetData->ResourceLayers);
            }
            else if (key == "instancer") {
                if (!targetData->Instancer) targetData->Instancer = std::make_unique<cm::instancer::InstancerComponent>();
                Serializer::DeserializeInstancer(val, *targetData->Instancer);
            }
            else if (key == "skeleton") {
                if (!targetData->Skeleton) targetData->Skeleton = std::make_unique<SkeletonComponent>();
                Serializer::DeserializeSkeleton(val, *targetData->Skeleton);
            }
            else if (key == "skinning") {
                if (!targetData->Skinning) targetData->Skinning = std::make_unique<SkinningComponent>();
                Serializer::DeserializeSkinning(val, *targetData->Skinning);
            }
            // UI components (missing from original implementation)
            else if (key == "slider") {
                if (!targetData->Slider) targetData->Slider = std::make_unique<SliderComponent>();
                Serializer::DeserializeSlider(val, *targetData->Slider);
            }
            else if (key == "progressBar") {
                if (!targetData->ProgressBar) targetData->ProgressBar = std::make_unique<ProgressBarComponent>();
                Serializer::DeserializeProgressBar(val, *targetData->ProgressBar);
            }
            else if (key == "toggle") {
                if (!targetData->Toggle) targetData->Toggle = std::make_unique<ToggleComponent>();
                Serializer::DeserializeToggle(val, *targetData->Toggle);
            }
            else if (key == "scrollView") {
                if (!targetData->ScrollView) targetData->ScrollView = std::make_unique<ScrollViewComponent>();
                Serializer::DeserializeScrollView(val, *targetData->ScrollView);
            }
            else if (key == "layoutGroup") {
                if (!targetData->LayoutGroup) targetData->LayoutGroup = std::make_unique<LayoutGroupComponent>();
                Serializer::DeserializeLayoutGroup(val, *targetData->LayoutGroup);
            }
            else if (key == "inputField") {
                if (!targetData->InputField) targetData->InputField = std::make_unique<InputFieldComponent>();
                Serializer::DeserializeInputField(val, *targetData->InputField);
            }
            else if (key == "dropdown") {
                if (!targetData->Dropdown) targetData->Dropdown = std::make_unique<DropdownComponent>();
                Serializer::DeserializeDropdown(val, *targetData->Dropdown);
            }
            else if (key == "uiRect") {
                if (!targetData->UIRect) targetData->UIRect = std::make_unique<UIRectComponent>();
                Serializer::DeserializeUIRect(val, *targetData->UIRect);
            }
            else if (key == "fitToContent") {
                if (!targetData->FitToContent) targetData->FitToContent = std::make_unique<FitToContentComponent>();
                Serializer::DeserializeFitToContent(val, *targetData->FitToContent);
            }
            else if (key == "uiSceneCapture") {
                if (!targetData->UISceneCapture) targetData->UISceneCapture = std::make_unique<UISceneCaptureComponent>();
                Serializer::DeserializeUISceneCapture(val, *targetData->UISceneCapture);
            }
            // Animation/mesh components (missing from original implementation)
            else if (key == "meshProxy") {
                if (!targetData->MeshProxy) targetData->MeshProxy = std::make_unique<MeshProxyComponent>();
                Serializer::DeserializeMeshProxy(val, *targetData->MeshProxy, scene);
            }
            else if (key == "blendShapeWeights") {
                if (targetData->BlendShapes) {
                    Serializer::DeserializeBlendShapeWeights(val, *targetData->BlendShapes);
                }
            }
            else if (key == "unifiedMorphWeights") {
                if (targetData->UnifiedMorph) {
                    Serializer::DeserializeUnifiedMorphWeights(val, *targetData->UnifiedMorph);
                } else if (val.is_array()) {
                    for (const auto& entry : val) {
                        if (!entry.is_object() || !entry.contains("name") || !entry.contains("weight")) continue;
                        targetData->PendingUnifiedMorphWeights[entry["name"].get<std::string>()] = entry["weight"].get<float>();
                    }
                }
            }
            else if (key == "boneAttachment") {
                if (!targetData->BoneAttachment) targetData->BoneAttachment = std::make_unique<BoneAttachmentComponent>();
                Serializer::DeserializeBoneAttachment(val, *targetData->BoneAttachment);
            }
            // Environment components (missing from original implementation)
            else if (key == "river") {
                if (!targetData->River) targetData->River = std::make_unique<RiverComponent>();
                Serializer::DeserializeRiver(val, *targetData->River);
            }
            else if (key == "spline") {
                if (!targetData->Spline) targetData->Spline = std::make_unique<SplineComponent>();
                Serializer::DeserializeSpline(val, *targetData->Spline);
            }
            else if (key == "grassDeformer") {
                if (!targetData->GrassDeformer) targetData->GrassDeformer = std::make_unique<GrassDeformerComponent>();
                Serializer::DeserializeGrassDeformer(val, *targetData->GrassDeformer);
            }
            // Rendering components (missing from original implementation)
            else if (key == "tintController") {
                if (!targetData->TintController) targetData->TintController = std::make_unique<TintMaskController>();
                Serializer::DeserializeTintController(val, *targetData->TintController);
            }
            else if (key == "ik") {
                if (val.is_array()) {
                    targetData->IKs.clear();
                    for (const auto& j : val) {
                        if (!j.is_object()) continue;
                        cm::animation::ik::IKComponent c;
                        c.Enabled = j.value("enabled", true);
                        c.TargetEntity = j.value("target", (EntityID)0);
                        c.PoleEntity = j.value("pole", (EntityID)0);
                        c.TargetEntityGuidHigh = j.value("targetGuidHigh", (uint64_t)0);
                        c.TargetEntityGuidLow = j.value("targetGuidLow", (uint64_t)0);
                        c.PoleEntityGuidHigh = j.value("poleGuidHigh", (uint64_t)0);
                        c.PoleEntityGuidLow = j.value("poleGuidLow", (uint64_t)0);
                        c.Weight = j.value("weight", 1.0f);
                        c.MaxIterations = j.value("maxIterations", 12.0f);
                        c.Tolerance = j.value("tolerance", 0.001f);
                        c.Damping = j.value("damping", 0.2f);
                        c.UseTwoBone = j.value("useTwoBone", true);
                        c.LockAxisX = j.value("lockAxisX", false);
                        c.LockAxisY = j.value("lockAxisY", false);
                        c.LockAxisZ = j.value("lockAxisZ", false);
                        c.Visualize = j.value("visualize", false);
                        std::vector<cm::animation::ik::BoneId> chainBones;
                        if (j.contains("chain") && j["chain"].is_array()) {
                            for (auto& b : j["chain"]) chainBones.push_back((cm::animation::ik::BoneId)b.get<int>());
                        }
                        c.SetChain(chainBones);
                        c.ChainRootHint = j.value("rootBone", c.ChainRootHint);
                        c.ChainEffectorHint = j.value("tipBone", c.ChainEffectorHint);
                        if (j.contains("constraints") && j["constraints"].is_array()) {
                            for (auto& cj : j["constraints"]) {
                                if (!cj.is_object()) continue;
                                cm::animation::ik::IKComponent::Constraint cc;
                                cc.useHinge=cj.value("useHinge",false); cc.useTwist=cj.value("useTwist",false);
                                cc.hingeMinDeg=cj.value("hingeMinDeg",0.0f); cc.hingeMaxDeg=cj.value("hingeMaxDeg",0.0f);
                                cc.twistMinDeg=cj.value("twistMinDeg",0.0f); cc.twistMaxDeg=cj.value("twistMaxDeg",0.0f);
                                c.Constraints.push_back(cc);
                            }
                        }
                        targetData->IKs.push_back(std::move(c));
                    }
                }
            }
            else if (key == "lookat") {
                if (val.is_array()) {
                    targetData->LookAtConstraints.clear();
                    for (const auto& j : val) {
                        if (!j.is_object()) continue;
                        cm::animation::lookat::LookAtConstraintComponent lac;
                        lac.Enabled = j.value("enabled", true);
                        lac.Mode = static_cast<cm::animation::lookat::LookAtMode>(j.value("mode", (uint8_t)cm::animation::lookat::LookAtMode::LookAtPosition));
                        lac.TargetEntity = j.value("target", (EntityID)0);
                        lac.TargetEntityGuidHigh = j.value("targetGuidHigh", (uint64_t)0);
                        lac.TargetEntityGuidLow = j.value("targetGuidLow", (uint64_t)0);
                        lac.Weight = j.value("weight", 1.0f);
                        lac.Axes = static_cast<cm::animation::lookat::AxisMask>(j.value("axes", (uint8_t)cm::animation::lookat::AxisMask::YawPitch));
                        lac.Space = static_cast<cm::animation::lookat::LookAtSpace>(j.value("space", (uint8_t)cm::animation::lookat::LookAtSpace::Component));
                        lac.Distribution = static_cast<cm::animation::lookat::DistributionMode>(j.value("distribution", (uint8_t)cm::animation::lookat::DistributionMode::Linear));
                        lac.MaxYawDeg = j.value("maxYawDeg", 70.0f);
                        lac.MaxPitchDeg = j.value("maxPitchDeg", 45.0f);
                        lac.MaxRollDeg = j.value("maxRollDeg", 15.0f);
                        lac.SmoothingSpeed = j.value("smoothingSpeed", 10.0f);
                        lac.Visualize = j.value("visualize", false);
                        if (j.contains("boneChain") && j["boneChain"].is_array()) {
                            for (auto& b : j["boneChain"]) lac.BoneChain.push_back((cm::animation::lookat::BoneId)b.get<int>());
                        }
                        if (j.contains("boneWeights") && j["boneWeights"].is_array()) {
                            for (auto& w : j["boneWeights"]) lac.BoneWeights.push_back(w.get<float>());
                        }
                        targetData->LookAtConstraints.push_back(std::move(lac));
                    }
                }
            }
            else if (key == "modelAssetGuid") {
                try { val.get_to(targetData->ModelAssetGuid); } catch (...) {}
            }
            else if (key == "deletedModelNodes") {
                targetData->DeletedModelNodes.clear();
                if (val.is_array()) {
                    for (const auto& path : val) {
                        if (path.is_string()) {
                            std::string p = path.get<std::string>();
                            if (!p.empty()) targetData->DeletedModelNodes.push_back(std::move(p));
                        }
                    }
                }
                if (!targetData->DeletedModelNodes.empty()) {
                    std::unordered_map<std::string, EntityID> pathToEntity;
                    std::function<void(EntityID, const std::string&)> buildPaths = [&](EntityID id, const std::string& parentPath) {
                        auto* d = scene.GetEntityData(id);
                        if (!d) return;
                        std::string path = parentPath.empty() ? d->Name : (parentPath + "/" + d->Name);
                        pathToEntity[path] = id;
                        for (EntityID c : d->Children) buildPaths(c, path);
                    };
                    buildPaths(targetId, std::string());
                    for (const auto& relPath : targetData->DeletedModelNodes) {
                        if (relPath.empty()) continue;
                        auto it = pathToEntity.find(relPath);
                        if (it != pathToEntity.end()) {
                            scene.RemoveEntity(it->second);
                        }
                    }
                }
            }
            else if (key == "Dynamic") {
                try {
                    if (val.is_array()) {
                        targetData->Dynamic.clear();
                        Serializer::DeserializeDynamic(val, targetData->Dynamic);
                    }
                } catch (...) {}
            }
            else {
                std::cerr << "[Prefab] Unknown component key for override: " << key << "\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "[Prefab] Failed to apply override for " << key << ": " << e.what() << "\n";
        }
    }
    
    scene.UpdateTransforms();
}

void ApplyPrefabOverrides(EntityID instanceRoot, Scene& scene, const std::vector<prefab::PropertyOverride>& overrides) {
    if (overrides.empty()) return;
    
    auto* rootData = scene.GetEntityData(instanceRoot);
    
    // Build instance GUID -> EntityID map for the instance subtree
    std::unordered_map<uint64_t, EntityID> instanceGuidToId;
    auto pack = [](const ClaymoreGUID& g)->uint64_t { return PackGuidHash(g); };
    
    std::function<void(EntityID)> buildMap = [&](EntityID id) {
        auto* d = scene.GetEntityData(id);
        if (!d) return;
        instanceGuidToId[pack(d->EntityGuid)] = id;
        for (EntityID c : d->Children) buildMap(c);
    };
    buildMap(instanceRoot);
    
    // Get GUID remapping if available
    const std::unordered_map<ClaymoreGUID, ClaymoreGUID>* prefabToInstance = nullptr;
    if (rootData && rootData->PrefabInstance) {
        prefabToInstance = &rootData->PrefabInstance->PrefabToInstanceGuid;
    }
    
    std::cout << "[Prefab] Applying " << overrides.size() << " overrides to instance (external)\n";
    
    for (const auto& ov : overrides) {
        // Overrides reference original PREFAB GUIDs, but entities now have INSTANCE GUIDs
        // Use the GUID remapping to find the correct target
        ClaymoreGUID lookupGuid = ov.TargetEntityGuid;
        if (prefabToInstance && !prefabToInstance->empty()) {
            auto remapIt = prefabToInstance->find(ov.TargetEntityGuid);
            if (remapIt != prefabToInstance->end()) {
                lookupGuid = remapIt->second; // Use remapped instance GUID
            }
        }
        
        auto it = instanceGuidToId.find(pack(lookupGuid));
        if (it == instanceGuidToId.end()) {
            std::cerr << "[Prefab] Override target not found: " << ov.TargetEntityGuid.ToString() << "\n";
            continue;
        }
        
        EntityID targetId = it->second;
        auto* targetData = scene.GetEntityData(targetId);
        if (!targetData) continue;
        
        // Apply the override based on component key
        const std::string& key = ov.ComponentKey;
        const json& val = ov.Value;
        
        try {
            if (key == "name") {
                scene.SetEntityName(targetId, val.get<std::string>());
            }
            else if (key == "active") {
                scene.SetEntityActive(targetId, val.get<bool>());
            }
            else if (key == "visible") {
                scene.SetEntityVisibleDirect(targetId, val.get<bool>());
            }
            else if (key == "layer") {
                scene.SetEntityLayer(targetId, val.get<int>());
            }
            else if (key == "tag") {
                scene.SetEntityTag(targetId, val.get<std::string>());
            }
            else if (key == "transform") {
                Serializer::DeserializeTransform(val, targetData->Transform);
                scene.MarkTransformDirty(targetId);
            }
            else if (key == "mesh") {
                if (!targetData->Mesh) targetData->Mesh = std::make_unique<MeshComponent>();
                json meshOverride = val;
                if (meshOverride.is_object() && targetData->Mesh->mesh) {
                    // Preserve the mesh chosen by fresh model instantiation. Prefab overrides can
                    // carry stale meshReference/fileID values after the source model is re-exported.
                    meshOverride.erase("meshReference");
                    meshOverride.erase("fileID");
                }
                Serializer::DeserializeMesh(meshOverride, *targetData->Mesh);
            }
            else if (key == "light") {
                if (!targetData->Light) targetData->Light = std::make_unique<LightComponent>();
                Serializer::DeserializeLight(val, *targetData->Light);
            }
            else if (key == "camera") {
                if (!targetData->Camera) targetData->Camera = std::make_unique<CameraComponent>();
                Serializer::DeserializeCamera(val, *targetData->Camera);
            }
            else if (key == "collider") {
                if (!targetData->Collider) targetData->Collider = std::make_unique<ColliderComponent>();
                Serializer::DeserializeCollider(val, *targetData->Collider);
            }
            else if (key == "rigidbody") {
                if (!targetData->RigidBody) targetData->RigidBody = std::make_unique<RigidBodyComponent>();
                Serializer::DeserializeRigidBody(val, *targetData->RigidBody);
            }
            else if (key == "staticbody") {
                if (!targetData->StaticBody) targetData->StaticBody = std::make_unique<StaticBodyComponent>();
                Serializer::DeserializeStaticBody(val, *targetData->StaticBody);
            }
            else if (key == "characterController") {
                if (!targetData->CharacterController) targetData->CharacterController = std::make_unique<CharacterControllerComponent>();
                Serializer::DeserializeCharacterController(val, *targetData->CharacterController);
            }
            else if (key == "area") {
                if (!targetData->Area) targetData->Area = std::make_unique<cm::physics::AreaComponent>();
                Serializer::DeserializeArea(val, *targetData->Area);
            }
            else if (key == "emitter") {
                if (!targetData->Emitter) targetData->Emitter = std::make_unique<ParticleEmitterComponent>();
                Serializer::DeserializeParticleEmitter(val, *targetData->Emitter);
            }
            else if (key == "animator") {
                if (!targetData->AnimationPlayer) targetData->AnimationPlayer = std::make_unique<cm::animation::AnimationPlayerComponent>();
                Serializer::DeserializeAnimator(val, *targetData->AnimationPlayer);
            }
            else if (key == "scripts") {
                Serializer::DeserializeScripts(val, targetData->Scripts, scene);
            }
            else if (key == "canvas") {
                if (!targetData->Canvas) targetData->Canvas = std::make_unique<CanvasComponent>();
                Serializer::DeserializeCanvas(val, *targetData->Canvas);
            }
            else if (key == "panel") {
                if (!targetData->Panel) targetData->Panel = std::make_unique<PanelComponent>();
                Serializer::DeserializePanel(val, *targetData->Panel);
            }
            else if (key == "button") {
                if (!targetData->Button) targetData->Button = std::make_unique<ButtonComponent>();
                Serializer::DeserializeButton(val, *targetData->Button);
            }
            else if (key == "text") {
                if (!targetData->Text) targetData->Text = std::make_unique<TextRendererComponent>();
                Serializer::DeserializeText(val, *targetData->Text);
            }
            else if (key == "renderOverrides") {
                if (!targetData->RenderOverrides) targetData->RenderOverrides = std::make_unique<RenderOverridesComponent>();
                Serializer::DeserializeRenderOverrides(val, *targetData->RenderOverrides);
            }
            else if (key == "navmesh") {
                if (!targetData->Navigation) targetData->Navigation = std::make_unique<nav::NavMeshComponent>();
                Serializer::DeserializeNavMesh(val, *targetData->Navigation);
            }
            else if (key == "navagent") {
                if (!targetData->NavAgent) targetData->NavAgent = std::make_unique<nav::NavAgentComponent>();
                Serializer::DeserializeNavAgent(val, *targetData->NavAgent);
            }
            else if (key == "navlink") {
                if (!targetData->NavLink) targetData->NavLink = std::make_unique<nav::NavLinkComponent>();
                Serializer::DeserializeNavLink(val, *targetData->NavLink);
            }
            else if (key == "portal") {
                if (!targetData->Portal) targetData->Portal = std::make_unique<PortalComponent>();
                Serializer::DeserializePortal(val, *targetData->Portal);
            }
            else if (key == "terrain") {
                if (!targetData->Terrain) targetData->Terrain = std::make_unique<TerrainComponent>();
                Serializer::DeserializeTerrain(val, *targetData->Terrain);
            }
            else if (key == "resourceLayers") {
                if (!targetData->ResourceLayers) targetData->ResourceLayers = std::make_unique<cm::resourcelayer::ResourceLayerComponent>();
                Serializer::DeserializeResourceLayers(val, *targetData->ResourceLayers);
            }
            else if (key == "instancer") {
                if (!targetData->Instancer) targetData->Instancer = std::make_unique<cm::instancer::InstancerComponent>();
                Serializer::DeserializeInstancer(val, *targetData->Instancer);
            }
            else if (key == "skeleton") {
                if (!targetData->Skeleton) targetData->Skeleton = std::make_unique<SkeletonComponent>();
                Serializer::DeserializeSkeleton(val, *targetData->Skeleton);
            }
            else if (key == "skinning") {
                if (!targetData->Skinning) targetData->Skinning = std::make_unique<SkinningComponent>();
                Serializer::DeserializeSkinning(val, *targetData->Skinning);
            }
            // UI components (missing from original implementation)
            else if (key == "slider") {
                if (!targetData->Slider) targetData->Slider = std::make_unique<SliderComponent>();
                Serializer::DeserializeSlider(val, *targetData->Slider);
            }
            else if (key == "progressBar") {
                if (!targetData->ProgressBar) targetData->ProgressBar = std::make_unique<ProgressBarComponent>();
                Serializer::DeserializeProgressBar(val, *targetData->ProgressBar);
            }
            else if (key == "toggle") {
                if (!targetData->Toggle) targetData->Toggle = std::make_unique<ToggleComponent>();
                Serializer::DeserializeToggle(val, *targetData->Toggle);
            }
            else if (key == "scrollView") {
                if (!targetData->ScrollView) targetData->ScrollView = std::make_unique<ScrollViewComponent>();
                Serializer::DeserializeScrollView(val, *targetData->ScrollView);
            }
            else if (key == "layoutGroup") {
                if (!targetData->LayoutGroup) targetData->LayoutGroup = std::make_unique<LayoutGroupComponent>();
                Serializer::DeserializeLayoutGroup(val, *targetData->LayoutGroup);
            }
            else if (key == "inputField") {
                if (!targetData->InputField) targetData->InputField = std::make_unique<InputFieldComponent>();
                Serializer::DeserializeInputField(val, *targetData->InputField);
            }
            else if (key == "dropdown") {
                if (!targetData->Dropdown) targetData->Dropdown = std::make_unique<DropdownComponent>();
                Serializer::DeserializeDropdown(val, *targetData->Dropdown);
            }
            else if (key == "uiRect") {
                if (!targetData->UIRect) targetData->UIRect = std::make_unique<UIRectComponent>();
                Serializer::DeserializeUIRect(val, *targetData->UIRect);
            }
            else if (key == "fitToContent") {
                if (!targetData->FitToContent) targetData->FitToContent = std::make_unique<FitToContentComponent>();
                Serializer::DeserializeFitToContent(val, *targetData->FitToContent);
            }
            // Animation/mesh components (missing from original implementation)
            else if (key == "meshProxy") {
                if (!targetData->MeshProxy) targetData->MeshProxy = std::make_unique<MeshProxyComponent>();
                Serializer::DeserializeMeshProxy(val, *targetData->MeshProxy, scene);
            }
            else if (key == "blendShapeWeights") {
                if (targetData->BlendShapes) {
                    Serializer::DeserializeBlendShapeWeights(val, *targetData->BlendShapes);
                }
            }
            else if (key == "unifiedMorphWeights") {
                if (targetData->UnifiedMorph) {
                    Serializer::DeserializeUnifiedMorphWeights(val, *targetData->UnifiedMorph);
                } else if (val.is_array()) {
                    for (const auto& entry : val) {
                        if (!entry.is_object() || !entry.contains("name") || !entry.contains("weight")) continue;
                        targetData->PendingUnifiedMorphWeights[entry["name"].get<std::string>()] = entry["weight"].get<float>();
                    }
                }
            }
            else if (key == "boneAttachment") {
                if (!targetData->BoneAttachment) targetData->BoneAttachment = std::make_unique<BoneAttachmentComponent>();
                Serializer::DeserializeBoneAttachment(val, *targetData->BoneAttachment);
            }
            // Environment components (missing from original implementation)
            else if (key == "river") {
                if (!targetData->River) targetData->River = std::make_unique<RiverComponent>();
                Serializer::DeserializeRiver(val, *targetData->River);
            }
            else if (key == "spline") {
                if (!targetData->Spline) targetData->Spline = std::make_unique<SplineComponent>();
                Serializer::DeserializeSpline(val, *targetData->Spline);
            }
            else if (key == "grassDeformer") {
                if (!targetData->GrassDeformer) targetData->GrassDeformer = std::make_unique<GrassDeformerComponent>();
                Serializer::DeserializeGrassDeformer(val, *targetData->GrassDeformer);
            }
            // Rendering components (missing from original implementation)
            else if (key == "tintController") {
                if (!targetData->TintController) targetData->TintController = std::make_unique<TintMaskController>();
                Serializer::DeserializeTintController(val, *targetData->TintController);
            }
            else if (key == "ik") {
                if (val.is_array()) {
                    targetData->IKs.clear();
                    for (const auto& j : val) {
                        if (!j.is_object()) continue;
                        cm::animation::ik::IKComponent c;
                        c.Enabled = j.value("enabled", true);
                        c.TargetEntity = j.value("target", (EntityID)0);
                        c.PoleEntity = j.value("pole", (EntityID)0);
                        c.TargetEntityGuidHigh = j.value("targetGuidHigh", (uint64_t)0);
                        c.TargetEntityGuidLow = j.value("targetGuidLow", (uint64_t)0);
                        c.PoleEntityGuidHigh = j.value("poleGuidHigh", (uint64_t)0);
                        c.PoleEntityGuidLow = j.value("poleGuidLow", (uint64_t)0);
                        c.Weight = j.value("weight", 1.0f);
                        c.MaxIterations = j.value("maxIterations", 12.0f);
                        c.Tolerance = j.value("tolerance", 0.001f);
                        c.Damping = j.value("damping", 0.2f);
                        c.UseTwoBone = j.value("useTwoBone", true);
                        c.LockAxisX = j.value("lockAxisX", false);
                        c.LockAxisY = j.value("lockAxisY", false);
                        c.LockAxisZ = j.value("lockAxisZ", false);
                        c.Visualize = j.value("visualize", false);
                        std::vector<cm::animation::ik::BoneId> chainBones;
                        if (j.contains("chain") && j["chain"].is_array()) {
                            for (auto& b : j["chain"]) chainBones.push_back((cm::animation::ik::BoneId)b.get<int>());
                        }
                        c.SetChain(chainBones);
                        c.ChainRootHint = j.value("rootBone", c.ChainRootHint);
                        c.ChainEffectorHint = j.value("tipBone", c.ChainEffectorHint);
                        if (j.contains("constraints") && j["constraints"].is_array()) {
                            for (auto& cj : j["constraints"]) {
                                if (!cj.is_object()) continue;
                                cm::animation::ik::IKComponent::Constraint cc;
                                cc.useHinge=cj.value("useHinge",false); cc.useTwist=cj.value("useTwist",false);
                                cc.hingeMinDeg=cj.value("hingeMinDeg",0.0f); cc.hingeMaxDeg=cj.value("hingeMaxDeg",0.0f);
                                cc.twistMinDeg=cj.value("twistMinDeg",0.0f); cc.twistMaxDeg=cj.value("twistMaxDeg",0.0f);
                                c.Constraints.push_back(cc);
                            }
                        }
                        targetData->IKs.push_back(std::move(c));
                    }
                }
            }
            else if (key == "lookat") {
                if (val.is_array()) {
                    targetData->LookAtConstraints.clear();
                    for (const auto& j : val) {
                        if (!j.is_object()) continue;
                        cm::animation::lookat::LookAtConstraintComponent lac;
                        lac.Enabled = j.value("enabled", true);
                        lac.Mode = static_cast<cm::animation::lookat::LookAtMode>(j.value("mode", (uint8_t)cm::animation::lookat::LookAtMode::LookAtPosition));
                        lac.TargetEntity = j.value("target", (EntityID)0);
                        lac.TargetEntityGuidHigh = j.value("targetGuidHigh", (uint64_t)0);
                        lac.TargetEntityGuidLow = j.value("targetGuidLow", (uint64_t)0);
                        lac.Weight = j.value("weight", 1.0f);
                        lac.Axes = static_cast<cm::animation::lookat::AxisMask>(j.value("axes", (uint8_t)cm::animation::lookat::AxisMask::YawPitch));
                        lac.Space = static_cast<cm::animation::lookat::LookAtSpace>(j.value("space", (uint8_t)cm::animation::lookat::LookAtSpace::Component));
                        lac.Distribution = static_cast<cm::animation::lookat::DistributionMode>(j.value("distribution", (uint8_t)cm::animation::lookat::DistributionMode::Linear));
                        lac.MaxYawDeg = j.value("maxYawDeg", 70.0f);
                        lac.MaxPitchDeg = j.value("maxPitchDeg", 45.0f);
                        lac.MaxRollDeg = j.value("maxRollDeg", 15.0f);
                        lac.SmoothingSpeed = j.value("smoothingSpeed", 10.0f);
                        lac.Visualize = j.value("visualize", false);
                        if (j.contains("boneChain") && j["boneChain"].is_array()) {
                            for (auto& b : j["boneChain"]) lac.BoneChain.push_back((cm::animation::lookat::BoneId)b.get<int>());
                        }
                        if (j.contains("boneWeights") && j["boneWeights"].is_array()) {
                            for (auto& w : j["boneWeights"]) lac.BoneWeights.push_back(w.get<float>());
                        }
                        targetData->LookAtConstraints.push_back(std::move(lac));
                    }
                }
            }
            else if (key == "modelAssetGuid") {
                try { val.get_to(targetData->ModelAssetGuid); } catch (...) {}
            }
            else if (key == "deletedModelNodes") {
                targetData->DeletedModelNodes.clear();
                if (val.is_array()) {
                    for (const auto& path : val) {
                        if (path.is_string()) {
                            std::string p = path.get<std::string>();
                            if (!p.empty()) targetData->DeletedModelNodes.push_back(std::move(p));
                        }
                    }
                }
                if (!targetData->DeletedModelNodes.empty()) {
                    std::unordered_map<std::string, EntityID> pathToEntity;
                    std::function<void(EntityID, const std::string&)> buildPaths = [&](EntityID id, const std::string& parentPath) {
                        auto* d = scene.GetEntityData(id);
                        if (!d) return;
                        std::string path = parentPath.empty() ? d->Name : (parentPath + "/" + d->Name);
                        pathToEntity[path] = id;
                        for (EntityID c : d->Children) buildPaths(c, path);
                    };
                    buildPaths(targetId, std::string());
                    for (const auto& relPath : targetData->DeletedModelNodes) {
                        if (relPath.empty()) continue;
                        auto it = pathToEntity.find(relPath);
                        if (it != pathToEntity.end()) {
                            scene.RemoveEntity(it->second);
                        }
                    }
                }
            }
            else if (key == "Dynamic") {
                try {
                    if (val.is_array()) {
                        targetData->Dynamic.clear();
                        Serializer::DeserializeDynamic(val, targetData->Dynamic);
                    }
                } catch (...) {}
            }
            else {
                std::cerr << "[Prefab] Unknown component key for override: " << key << "\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "[Prefab] Failed to apply override for " << key << ": " << e.what() << "\n";
        }
    }

    scene.ResolveScriptEntityReferencesFromMetadata();
    scene.UpdateTransforms();
}

void RevertEntityToPrefab(EntityID entity, Scene& scene) {
    EntityID root = GetPrefabInstanceRoot(entity, scene);
    if (root == INVALID_ENTITY_ID) return;
    
    auto* rootData = scene.GetEntityData(root);
    if (!rootData || !rootData->PrefabInstance) return;
    
    auto* entityData = scene.GetEntityData(entity);
    if (!entityData) return;
    
    // Remove overrides for this entity from the instance component
    rootData->PrefabInstance->RevertEntity(entityData->EntityGuid);
    
    // Re-apply prefab defaults by reloading the prefab and finding matching entity
    PrefabAsset prefab;
    std::string prefabPath = rootData->PrefabInstance->PrefabPath;
    if (prefabPath.empty()) {
        prefabPath = GetPrefabPathFromGuid(rootData->PrefabInstance->PrefabAssetGuid);
    }
    
    if (PrefabIO::LoadPrefab(prefabPath, prefab)) {
        // Find the base entity data in the prefab by GUID
        for (const auto& baseEntity : prefab.Entities) {
            if (!baseEntity.contains("guid")) continue;
            ClaymoreGUID baseGuid;
            try { baseEntity.at("guid").get_to(baseGuid); } catch (...) { continue; }
            
            if (baseGuid == entityData->EntityGuid) {
                // Apply base prefab data to this entity
                Serializer::DeserializeEntityData(baseEntity, entityData, scene);
                scene.MarkTransformDirty(entity);
                break;
            }
        }
    }
}

void RevertComponentToPrefab(EntityID entity, const std::string& componentKey, Scene& scene) {
    EntityID root = GetPrefabInstanceRoot(entity, scene);
    if (root == INVALID_ENTITY_ID) return;
    
    auto* rootData = scene.GetEntityData(root);
    if (!rootData || !rootData->PrefabInstance) return;
    
    auto* entityData = scene.GetEntityData(entity);
    if (!entityData) return;
    
    // Remove component override from the instance
    rootData->PrefabInstance->RevertComponent(entityData->EntityGuid, componentKey);
    
    // Re-apply prefab default for this component by reloading
    PrefabAsset prefab;
    std::string prefabPath = rootData->PrefabInstance->PrefabPath;
    if (prefabPath.empty()) {
        prefabPath = GetPrefabPathFromGuid(rootData->PrefabInstance->PrefabAssetGuid);
    }
    
    if (PrefabIO::LoadPrefab(prefabPath, prefab)) {
        // Find the base entity in the prefab
        for (const auto& baseEntity : prefab.Entities) {
            if (!baseEntity.contains("guid")) continue;
            ClaymoreGUID baseGuid;
            try { baseEntity.at("guid").get_to(baseGuid); } catch (...) { continue; }
            
            if (baseGuid == entityData->EntityGuid && baseEntity.contains(componentKey)) {
                // Apply just this component from base prefab
                // Create a minimal override to apply just this component
                prefab::PropertyOverride ov;
                ov.TargetEntityGuid = entityData->EntityGuid;
                ov.ComponentKey = componentKey;
                ov.Value = baseEntity[componentKey];
                
                std::vector<prefab::PropertyOverride> singleOverride = { ov };
                ApplyPrefabOverrides(root, scene, singleOverride);
                break;
            }
        }
    }
}

void RevertInstanceToPrefab(EntityID instanceRoot, Scene& scene) {
    auto* data = scene.GetEntityData(instanceRoot);
    if (!data || !data->PrefabInstance) return;
    
    // Store current transform and parent before reverting
    TransformComponent savedTransform = data->Transform;
    EntityID savedParent = data->Parent;
    ClaymoreGUID prefabGuid = data->PrefabInstance->PrefabAssetGuid;
    std::string prefabPath = data->PrefabInstance->PrefabPath;
    
    // Clear all overrides
    data->PrefabInstance->RevertAll();
    
    // Reload and re-instantiate the prefab
    PrefabAsset prefab;
    if (prefabPath.empty()) {
        prefabPath = GetPrefabPathFromGuid(prefabGuid);
    }
    
    if (PrefabIO::LoadPrefab(prefabPath, prefab)) {
        // Delete the current instance subtree (except root)
        std::vector<EntityID> childrenToRemove;
        for (EntityID child : data->Children) {
            childrenToRemove.push_back(child);
        }
        for (EntityID child : childrenToRemove) {
            scene.RemoveEntity(child);
        }
        data->Children.clear();
        
        // Re-instantiate from prefab (this will rebuild the subtree)
        // For now, we re-apply the base prefab data to the root
        for (const auto& baseEntity : prefab.Entities) {
            if (baseEntity.contains("guid")) {
                ClaymoreGUID baseGuid;
                try { baseEntity.at("guid").get_to(baseGuid); } catch (...) { continue; }
                if (baseGuid == data->EntityGuid) {
                    Serializer::DeserializeEntityData(baseEntity, data, scene);
                    scene.MarkTransformDirty(instanceRoot);
                    break;
                }
            }
        }
        
        // Restore transform and parent relationship
        data->Transform = savedTransform;
        if (savedParent != INVALID_ENTITY_ID) {
            scene.SetParent(instanceRoot, savedParent);
        }
        
    }
}

//==============================================================================
// Prefab Updates
//==============================================================================

void RefreshPrefabInstances(const ClaymoreGUID& prefabGuid, Scene& scene) {
    // Find all entities with this prefab GUID
    std::vector<EntityID> instances;
    for (const auto& ent : scene.GetEntities()) {
        auto* data = scene.GetEntityData(ent.GetID());
        if (data && data->PrefabInstance && data->PrefabInstance->PrefabAssetGuid == prefabGuid) {
            instances.push_back(ent.GetID());
        }
#ifdef CLAYMORE_EDITOR
        // Also check legacy PrefabSource (editor only - requires AssetLibrary)
        if (data && !data->PrefabSource.empty()) {
            // Try to resolve PrefabSource to GUID
            ClaymoreGUID sourceGuid = AssetLibrary::Instance().GetGuidByPath(data->PrefabSource);
            if (sourceGuid == prefabGuid) {
                // Avoid duplicates
                if (std::find(instances.begin(), instances.end(), ent.GetID()) == instances.end()) {
                    instances.push_back(ent.GetID());
                }
            }
        }
#endif
    }
    
    if (instances.empty()) return;
    
    // Reload the prefab asset
    std::string prefabPath = GetPrefabPathFromGuid(prefabGuid);
    PrefabAsset newPrefab;
    if (!PrefabIO::LoadPrefab(prefabPath, newPrefab)) {
        return;
    }
    
    // For each instance, re-apply the updated prefab while preserving overrides
    for (EntityID instRoot : instances) {
        auto* rootData = scene.GetEntityData(instRoot);
        if (!rootData) continue;
        
        // === NEW: Extract current overrides using scene serialization pattern ===
        // This collects overrides in the 'children' array format (same as model roots)
        nlohmann::json childrenOverrides = nlohmann::json::array();
        TransformComponent savedTransform = rootData->Transform;
        EntityID savedParent = rootData->Parent;
        std::string savedName = rootData->Name;
        
        // Helper to compute relative path from root
        auto computeNodePath = [&](EntityID root, EntityID node) -> std::string {
            std::vector<std::string> parts;
            EntityID cur = node;
            while (cur != INVALID_ENTITY_ID) {
                auto* d = scene.GetEntityData(cur);
                if (!d) break;
                if (cur == root) break;
                parts.push_back(d->Name);
                cur = d->Parent;
            }
            std::reverse(parts.begin(), parts.end());
            std::string s;
            for (size_t i = 0; i < parts.size(); ++i) {
                s += parts[i];
                if (i + 1 < parts.size()) s += "/";
            }
            return s;
        };
        
        // Collect overrides for all descendants (same pattern as SerializeScene)
        std::function<void(EntityID)> collectOverrides = [&](EntityID id) {
            auto* d = scene.GetEntityData(id);
            if (!d) return;
            
            for (EntityID c : d->Children) {
                nlohmann::json childJ = Serializer::SerializeEntity(c, scene);
                std::string relPath = computeNodePath(instRoot, c);
                childJ["_prefabNodePath"] = relPath;
                
                // Store entity GUID for fallback resolution
                auto* cd = scene.GetEntityData(c);
                if (cd && (cd->EntityGuid.high != 0 || cd->EntityGuid.low != 0)) {
                    childJ["_prefabEntityGuid"] = cd->EntityGuid;
                }
                
                // Strip relational fields
                childJ.erase("id");
                childJ.erase("parent");
                childJ.erase("children");
                childJ.erase("asset");
                
                if (!childJ.empty()) {
                    childrenOverrides.push_back(std::move(childJ));
                }
                
                collectOverrides(c);
            }
        };
        collectOverrides(instRoot);
        
        // === Delete old instance hierarchy ===
        std::function<void(EntityID)> deleteDescendants = [&](EntityID id) {
            auto* d = scene.GetEntityData(id);
            if (!d) return;
            std::vector<EntityID> children = d->Children; // Copy to avoid iteration issues
            for (EntityID c : children) {
                deleteDescendants(c);
                scene.RemoveEntity(c);
            }
        };
        deleteDescendants(instRoot);
        
        // Remove the root itself
        scene.RemoveEntity(instRoot);
        
        // === Re-instantiate fresh prefab ===
        EntityID newInstRoot = InstantiatePrefab(prefabGuid, scene);
        if (newInstRoot == INVALID_ENTITY_ID)
            continue;
        
        
        // Restore root properties
        auto* newRootData = scene.GetEntityData(newInstRoot);
        if (newRootData) {
            newRootData->Transform = savedTransform;
            scene.MarkTransformDirty(newInstRoot);
            scene.SetEntityName(newInstRoot, savedName);
            if (savedParent != INVALID_ENTITY_ID) {
                scene.SetParent(newInstRoot, savedParent);
            }
        }
        
        // === Re-apply overrides from children array ===
        // Build path resolution helper for new instance
        auto resolveByPath = [&](const std::string& path) -> EntityID {
            EntityID target = newInstRoot;
            if (path.empty()) return target;
            std::stringstream ss(path);
            std::string part;
            while (std::getline(ss, part, '/')) {
                auto* d = scene.GetEntityData(target);
                if (!d) return INVALID_ENTITY_ID;
                EntityID next = INVALID_ENTITY_ID;
                for (EntityID c : d->Children) {
                    auto* cd = scene.GetEntityData(c);
                    if (cd && cd->Name == part) {
                        next = c;
                        break;
                    }
                }
                if (next == INVALID_ENTITY_ID) return INVALID_ENTITY_ID;
                target = next;
            }
            return target;
        };
        
        // Apply each override
        for (const auto& childOverride : childrenOverrides) {
            std::string relPath;
            if (childOverride.contains("_prefabNodePath")) {
                relPath = childOverride["_prefabNodePath"].get<std::string>();
            } else {
                continue;
            }
            
            EntityID target = resolveByPath(relPath);
            if (target == INVALID_ENTITY_ID) continue;
            
            auto* td = scene.GetEntityData(target);
            if (!td) continue;
            
            // Apply component overrides
            if (childOverride.contains("transform")) {
                Serializer::DeserializeTransform(childOverride["transform"], td->Transform);
                scene.MarkTransformDirty(target);
            }
            if (childOverride.contains("scripts")) {
                Serializer::DeserializeScripts(childOverride["scripts"], td->Scripts, scene);
            }
            // Add other components as needed (same pattern as DeserializeScene prefab children)
        }
        
    }
    
    scene.UpdateTransforms();
}
