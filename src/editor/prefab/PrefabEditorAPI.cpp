#include "editor/prefab/PrefabEditorAPI.h"
#include "core/prefab/PrefabDelta.h"
#include "core/assets/AssetMetadata.h"
#include "core/serialization/Serializer.h"
#include "core/managed/ScriptReflection.h"
#include "core/prefab/RuntimePrefabInstantiator.h"
#include "editor/pipeline/AssetPipeline.h"
#include "editor/pipeline/AssetRegistry.h"
#include "editor/pipeline/AssetLibrary.h"
#include "editor/pipeline/BinaryAssetCache.h"
#include "editor/Project.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <functional>
#include <memory>
#include <unordered_set>
#include <unordered_map>
#include <filesystem>
#include <cmath>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace prefab_editor {

namespace {

static void RemapGuidIfPresent(const std::unordered_map<ClaymoreGUID, ClaymoreGUID>& guidMap,
                               ClaymoreGUID& guid) {
    auto it = guidMap.find(guid);
    if (it != guidMap.end()) {
        guid = it->second;
    }
}

static void RemapGuidIfPresent(const std::unordered_map<ClaymoreGUID, ClaymoreGUID>& guidMap,
                               uint64_t& high,
                               uint64_t& low) {
    ClaymoreGUID guid{high, low};
    auto it = guidMap.find(guid);
    if (it != guidMap.end()) {
        high = it->second.high;
        low = it->second.low;
    }
}

static void RemapPropertyValueEntityMetadata(
    PropertyValue& value,
    const std::unordered_map<ClaymoreGUID, ClaymoreGUID>& guidMap) {
    if (std::holds_alternative<std::shared_ptr<ListPropertyValue>>(value)) {
        auto listPtr = std::get<std::shared_ptr<ListPropertyValue>>(value);
        if (!listPtr) return;
        for (auto& meta : listPtr->entityRefs) {
            RemapGuidIfPresent(guidMap, meta.guid);
            RemapGuidIfPresent(guidMap, meta.modelRootGuid);
        }
        for (auto& elem : listPtr->elements) {
            RemapPropertyValueEntityMetadata(elem, guidMap);
        }
        return;
    }

    if (std::holds_alternative<std::shared_ptr<StructPropertyValue>>(value)) {
        auto structPtr = std::get<std::shared_ptr<StructPropertyValue>>(value);
        if (!structPtr) return;
        for (auto& field : structPtr->fields) {
            RemapPropertyValueEntityMetadata(field.second, guidMap);
        }
        return;
    }

    if (std::holds_alternative<std::shared_ptr<DictionaryPropertyValue>>(value)) {
        auto dictPtr = std::get<std::shared_ptr<DictionaryPropertyValue>>(value);
        if (!dictPtr) return;
        for (auto& entry : dictPtr->entries) {
            RemapPropertyValueEntityMetadata(entry.first, guidMap);
            RemapPropertyValueEntityMetadata(entry.second, guidMap);
        }
    }
}

static void RemapSubtreeAuthoringIdentity(
    Scene& scene,
    EntityID root,
    const std::unordered_map<ClaymoreGUID, ClaymoreGUID>& guidMap) {
    std::unordered_set<EntityID> visited;
    std::function<void(EntityID)> remap = [&](EntityID id) {
        if (id == INVALID_ENTITY_ID || !visited.insert(id).second) {
            return;
        }

        EntityData* data = scene.GetEntityData(id);
        if (!data) {
            return;
        }

        RemapGuidIfPresent(guidMap, data->EntityGuid);

        for (auto& lookAt : data->LookAtConstraints) {
            RemapGuidIfPresent(guidMap, lookAt.TargetEntityGuidHigh, lookAt.TargetEntityGuidLow);
        }

        for (auto& ik : data->IKs) {
            RemapGuidIfPresent(guidMap, ik.TargetEntityGuidHigh, ik.TargetEntityGuidLow);
            RemapGuidIfPresent(guidMap, ik.PoleEntityGuidHigh, ik.PoleEntityGuidLow);
        }

        for (auto& script : data->Scripts) {
            for (auto& [propertyName, meta] : script.EntityRefMetadata) {
                (void)propertyName;
                RemapGuidIfPresent(guidMap, meta.guid);
                RemapGuidIfPresent(guidMap, meta.modelRootGuid);
            }

            for (auto& [propertyName, propertyValue] : script.Values) {
                (void)propertyName;
                RemapPropertyValueEntityMetadata(propertyValue, guidMap);
            }
        }

        for (EntityID child : data->Children) {
            remap(child);
        }
    };

    remap(root);
}

class ScopedPrefabAuthoringSerialization {
public:
    ScopedPrefabAuthoringSerialization(Scene& scene, EntityID root)
        : m_Scene(scene), m_Root(root) {
        m_RootData = m_Scene.GetEntityData(m_Root);
        if (!m_RootData || !m_RootData->PrefabInstance) {
            return;
        }

        m_RootPrefabSource = m_RootData->PrefabSource;
        m_RootPrefabInstance = std::move(m_RootData->PrefabInstance);

        // Serialize prefab instances back into authored prefab identity, not scene-instance identity.
        if (!m_RootPrefabInstance->InstanceToPrefabGuid.empty()) {
            RemapSubtreeAuthoringIdentity(m_Scene, m_Root, m_RootPrefabInstance->InstanceToPrefabGuid);
        }
        m_RootData->PrefabSource.clear();
        m_Active = true;
    }

    ~ScopedPrefabAuthoringSerialization() {
        if (!m_Active) {
            return;
        }

        // Restore the isolated editor scene back to its instance GUID space.
        if (!m_RootPrefabInstance->PrefabToInstanceGuid.empty()) {
            RemapSubtreeAuthoringIdentity(m_Scene, m_Root, m_RootPrefabInstance->PrefabToInstanceGuid);
        }

        if (m_RootData) {
            m_RootData->PrefabSource = m_RootPrefabSource;
            m_RootData->PrefabInstance = std::move(m_RootPrefabInstance);
        }
    }

    bool IsActive() const { return m_Active; }

    const PrefabInstanceComponent* PrefabInstance() const {
        return m_RootPrefabInstance.get();
    }

private:
    Scene& m_Scene;
    EntityID m_Root = INVALID_ENTITY_ID;
    EntityData* m_RootData = nullptr;
    std::string m_RootPrefabSource;
    std::unique_ptr<PrefabInstanceComponent> m_RootPrefabInstance;
    bool m_Active = false;
};

static bool IsValidGuid(const ClaymoreGUID& guid) {
    return guid.high != 0 || guid.low != 0;
}

static std::string NormalizeAssetVirtualPath(const fs::path& assetPath) {
    std::error_code ec;
    fs::path rel = fs::relative(assetPath, Project::GetProjectDirectory(), ec);
    std::string vpath = (ec ? assetPath.string() : rel.string());
    std::replace(vpath.begin(), vpath.end(), '\\', '/');
    size_t assetsPos = vpath.find("assets/");
    if (assetsPos != std::string::npos) {
        vpath = vpath.substr(assetsPos);
    }
    return vpath;
}

static bool TryLoadAssetMetadata(const fs::path& metaPath, AssetMetadata& metadata) {
    if (!fs::exists(metaPath)) {
        return false;
    }

    try {
        std::ifstream in(metaPath.string());
        if (!in) {
            return false;
        }

        json j;
        in >> j;
        metadata = j.get<AssetMetadata>();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[PrefabEditor] WARNING: Failed to read prefab metadata '"
                  << metaPath.string() << "': " << e.what() << "\n";
    } catch (...) {
        std::cerr << "[PrefabEditor] WARNING: Failed to read prefab metadata '"
                  << metaPath.string() << "'\n";
    }

    return false;
}

static void CollectSubtreeEntities(Scene& scene,
                                   EntityID root,
                                   std::vector<EntityID>& outEntities,
                                   std::unordered_set<EntityID>& visited) {
    if (root == INVALID_ENTITY_ID || !visited.insert(root).second) {
        return;
    }

    EntityData* data = scene.GetEntityData(root);
    if (!data) {
        return;
    }

    outEntities.push_back(root);
    for (EntityID child : data->Children) {
        CollectSubtreeEntities(scene, child, outEntities, visited);
    }
}

static EntityID TryResolveLiveEntityForPrefabJson(
    const json& entityJson,
    const std::unordered_set<EntityID>& liveEntityIds,
    const std::unordered_map<ClaymoreGUID, EntityID>& liveGuidToEntity,
    const std::unordered_map<ClaymoreGUID, ClaymoreGUID>& previousPrefabToInstance) {

    ClaymoreGUID prefabGuid{};
    if (entityJson.contains("guid")) {
        try {
            entityJson.at("guid").get_to(prefabGuid);
        } catch (...) {
            prefabGuid = {};
        }
    }
    if (!IsValidGuid(prefabGuid) && entityJson.contains("_prefabEntityGuid")) {
        try {
            entityJson.at("_prefabEntityGuid").get_to(prefabGuid);
        } catch (...) {
            prefabGuid = {};
        }
    }

    if (IsValidGuid(prefabGuid)) {
        auto liveGuidIt = liveGuidToEntity.find(prefabGuid);
        if (liveGuidIt != liveGuidToEntity.end()) {
            return liveGuidIt->second;
        }

        auto previousIt = previousPrefabToInstance.find(prefabGuid);
        if (previousIt != previousPrefabToInstance.end()) {
            liveGuidIt = liveGuidToEntity.find(previousIt->second);
            if (liveGuidIt != liveGuidToEntity.end()) {
                return liveGuidIt->second;
            }
        }
    }

    if (entityJson.contains("id")) {
        try {
            const EntityID serializedId = entityJson.at("id").get<EntityID>();
            if (liveEntityIds.find(serializedId) != liveEntityIds.end()) {
                return serializedId;
            }
        } catch (...) {
        }
    }

    return INVALID_ENTITY_ID;
}

static bool TryReadPrefabJsonEntityGuid(const json& entityJson, ClaymoreGUID& outGuid) {
    outGuid = {};
    if (entityJson.contains("guid")) {
        try {
            entityJson.at("guid").get_to(outGuid);
        } catch (...) {
            outGuid = {};
        }
    }
    if (!IsValidGuid(outGuid) && entityJson.contains("_prefabEntityGuid")) {
        try {
            entityJson.at("_prefabEntityGuid").get_to(outGuid);
        } catch (...) {
            outGuid = {};
        }
    }
    return IsValidGuid(outGuid);
}

} // namespace

//==============================================================================
// Model Root Detection
//==============================================================================

bool IsImportedModelRoot(Scene& scene, EntityID id, std::string& outModelPath, ClaymoreGUID& outGuid) {
    auto* ed = scene.GetEntityData(id);
    if (!ed) return false;
    
    // Model roots typically don't have a mesh component themselves
    if (ed->Mesh) return false;
    
    // Strategy 1: Check ModelAssetGuid
    if (!(ed->ModelAssetGuid.high == 0 && ed->ModelAssetGuid.low == 0)) {
        std::string p = AssetLibrary::Instance().GetPathForGUID(ed->ModelAssetGuid);
        if (!p.empty()) {
            outModelPath = p;
            outGuid = ed->ModelAssetGuid;
            return true;
        }
    }
    
    // Strategy 2: DFS through children to find mesh with valid asset reference
    std::function<bool(EntityID)> findMeshAsset = [&](EntityID e) -> bool {
        auto* cd = scene.GetEntityData(e);
        if (!cd) return false;
        
        if (cd->Mesh && cd->Mesh->meshReference.IsValid()) {
            ClaymoreGUID meshGuid = cd->Mesh->meshReference.guid;
            std::string p = AssetLibrary::Instance().GetPathForGUID(meshGuid);
            if (!p.empty()) {
                outModelPath = p;
                outGuid = meshGuid;
                ed->ModelAssetGuid = meshGuid; // Cache for future lookups
                return true;
            }
        }
        
        for (EntityID c : cd->Children) {
            if (findMeshAsset(c)) return true;
        }
        return false;
    };
    
    for (EntityID c : ed->Children) {
        if (findMeshAsset(c)) return true;
    }
    
    return false;
}

bool IsModelDescendant(Scene& scene, EntityID id, EntityID& outModelRoot) {
    EntityID current = id;
    while (current != INVALID_ENTITY_ID) {
        auto* data = scene.GetEntityData(current);
        if (!data) break;
        
        std::string modelPath;
        ClaymoreGUID modelGuid;
        if (IsImportedModelRoot(scene, current, modelPath, modelGuid)) {
            outModelRoot = current;
            return id != current; // true if id is a descendant, not the root itself
        }
        
        current = data->Parent;
    }
    outModelRoot = INVALID_ENTITY_ID;
    return false;
}

//==============================================================================
// Prefab Building - Uses SerializeScene with root filter (SINGLE SOURCE OF TRUTH)
// This ensures prefabs and scenes use IDENTICAL serialization logic
//==============================================================================

bool BuildPrefabAssetFromScene(Scene& scene, EntityID root, PrefabAsset& out) {
    auto* rd = scene.GetEntityData(root);
    if (!rd) return false;

    ScopedPrefabAuthoringSerialization authoringScope(scene, root);
    
    // Use SerializeScene with root filter - THE SINGLE SOURCE OF TRUTH
    // This is the EXACT same code path as scene serialization, just filtered to subtree
    json prefabJson = Serializer::SerializeScene(scene, root);

    if (authoringScope.IsActive()) {
        const PrefabInstanceComponent* prefabInstance = authoringScope.PrefabInstance();
        if (prefabInstance &&
            (prefabInstance->PrefabAssetGuid.high != 0 || prefabInstance->PrefabAssetGuid.low != 0)) {
            // Preserve the authored prefab asset GUID when re-saving through the prefab editor.
            prefabJson["guid"] = prefabInstance->PrefabAssetGuid;
        }
    }
    
    // STORE THE FULL JSON - this includes assetMap, entities, everything
    // This is TRUE PARITY - prefab file IS the serializer output
    out.Raw = prefabJson;
    
    // Extract commonly-needed fields for convenience access
    out.Clear();
    
    // Get GUID from json if present, otherwise use entity GUID
    if (prefabJson.contains("guid")) {
        try { prefabJson.at("guid").get_to(out.Guid); } 
        catch (...) { out.Guid = rd->EntityGuid; }
    } else {
        out.Guid = rd->EntityGuid;
    }
    
    out.Name = prefabJson.value("name", rd->Name);
    
    // Get rootGuid from json if present
    if (prefabJson.contains("rootGuid")) {
        try { prefabJson.at("rootGuid").get_to(out.RootGuid); }
        catch (...) { out.RootGuid = rd->EntityGuid; }
    } else {
        out.RootGuid = rd->EntityGuid;
    }
    
    // Copy entities array from serializer output
    if (prefabJson.contains("entities") && prefabJson["entities"].is_array()) {
        out.Entities = prefabJson["entities"];
    } else {
        out.Entities = json::array();
    }
    
    std::cout << "[PrefabEditor] Built prefab via SerializeScene: " << out.EntityCount() << " entities\n";
    return true;
}

//==============================================================================
// Prefab Saving
//==============================================================================

bool SavePrefab(const std::string& path, const PrefabAsset& prefab) {
    return PrefabIO::SavePrefab(path, prefab);
}

bool SavePrefabByGuid(const ClaymoreGUID& prefabGuid, const PrefabAsset& prefab) {
    std::string path = "assets/prefabs/" + prefabGuid.ToString() + ".prefab";
    fs::path fullPath = Project::GetProjectDirectory() / path;
    return PrefabIO::SavePrefab(fullPath.string(), prefab);
}

bool AdoptExistingPrefabAssetGuid(const std::string& prefabPath, PrefabAsset& prefab) {
    ClaymoreGUID existingGuid{};

    fs::path metaPath = fs::path(prefabPath);
    metaPath += ".meta";
    AssetMetadata meta;
    if (TryLoadAssetMetadata(metaPath, meta) && IsValidGuid(meta.guid)) {
        existingGuid = meta.guid;
    }

    if (!IsValidGuid(existingGuid) && fs::exists(prefabPath)) {
        try {
            PrefabAsset existingPrefab;
            if (PrefabIO::LoadPrefabSource(prefabPath, existingPrefab) && IsValidGuid(existingPrefab.Guid)) {
                existingGuid = existingPrefab.Guid;
            }
        } catch (...) {
        }
    }

    if (!IsValidGuid(existingGuid) || existingGuid == prefab.Guid) {
        return false;
    }

    prefab.Guid = existingGuid;
    if (prefab.Raw.is_object()) {
        prefab.Raw["guid"] = existingGuid;
    }
    std::cout << "[PrefabEditor] Preserving existing prefab asset GUID "
              << existingGuid.ToString() << " for overwrite: "
              << prefabPath << "\n";
    return true;
}

bool FinalizeSavedPrefabFromScene(
    Scene& scene,
    EntityID root,
    const std::string& prefabPath,
    const PrefabAsset& prefab,
    std::string* outVirtualPath) {

    if (!IsValidGuid(prefab.Guid)) {
        std::cerr << "[PrefabEditor] Failed to register prefab '" << prefabPath
                  << "': prefab asset has no GUID\n";
        return false;
    }

    EntityData* rootData = scene.GetEntityData(root);
    if (!rootData) {
        std::cerr << "[PrefabEditor] Failed to link saved prefab '" << prefabPath
                  << "': source entity was not found\n";
        return false;
    }

    const fs::path prefabFsPath(prefabPath);
    fs::path metaPath = prefabFsPath;
    metaPath += ".meta";
    const std::string vpath = NormalizeAssetVirtualPath(prefabFsPath);

    AssetMetadata meta;
    TryLoadAssetMetadata(metaPath, meta);
    meta.guid = prefab.Guid;
    meta.type = "prefab";
    meta.sourcePath = prefabPath;
    meta.processedPath = prefabPath;
    try {
        meta.hash = AssetPipeline::Instance().ComputeFileHash(prefabPath);
    } catch (...) {
    }
    meta.reference = AssetReference(prefab.Guid, 0, static_cast<int>(AssetType::Prefab));

    try {
        std::ofstream out(metaPath.string());
        if (!out) {
            std::cerr << "[PrefabEditor] Failed to write prefab metadata: "
                      << metaPath.string() << "\n";
            return false;
        }
        json j = meta;
        out << j.dump(4);
    } catch (const std::exception& e) {
        std::cerr << "[PrefabEditor] Failed to write prefab metadata '"
                  << metaPath.string() << "': " << e.what() << "\n";
        return false;
    }

    AssetRegistry::Instance().SetMetadata(prefabPath, meta);
    AssetLibrary::Instance().RegisterAsset(
        meta.reference,
        AssetType::Prefab,
        vpath,
        prefabFsPath.filename().string());
    AssetLibrary::Instance().RegisterPathAlias(prefab.Guid, prefabPath);
    if (!BinaryAssetCache::Instance().EnsureBinary(prefabPath)) {
        std::cerr << "[PrefabEditor] Failed to rebuild prefab binary for: "
                  << prefabPath << "\n";
    }
    runtime::RuntimePrefabInstantiator::InvalidateCache(prefabPath);
    runtime::RuntimePrefabInstantiator::InvalidateCache(BinaryAssetCache::Instance().GetBinaryPath(prefabPath));

    std::vector<EntityID> subtree;
    std::unordered_set<EntityID> visited;
    CollectSubtreeEntities(scene, root, subtree, visited);

    std::unordered_map<ClaymoreGUID, EntityID> liveGuidToEntity;
    liveGuidToEntity.reserve(subtree.size());
    for (EntityID entityId : subtree) {
        EntityData* data = scene.GetEntityData(entityId);
        if (data && IsValidGuid(data->EntityGuid)) {
            liveGuidToEntity[data->EntityGuid] = entityId;
        }
    }

    std::unordered_map<ClaymoreGUID, ClaymoreGUID> previousPrefabToInstance;
    if (rootData->PrefabInstance) {
        previousPrefabToInstance = rootData->PrefabInstance->PrefabToInstanceGuid;
    }

    auto instance = std::make_unique<PrefabInstanceComponent>();
    instance->PrefabAssetGuid = prefab.Guid;
    instance->PrefabPath = vpath;
    instance->OwnedEntityGuids.reserve(subtree.size());

    auto addPrefabEntityMapping = [&](const json& entityJson) {
        ClaymoreGUID prefabEntityGuid{};
        if (!TryReadPrefabJsonEntityGuid(entityJson, prefabEntityGuid)) {
            return;
        }

        if (instance->PrefabToInstanceGuid.find(prefabEntityGuid) == instance->PrefabToInstanceGuid.end()) {
            EntityID liveEntity = TryResolveLiveEntityForPrefabJson(
                entityJson,
                visited,
                liveGuidToEntity,
                previousPrefabToInstance);
            EntityData* liveData = scene.GetEntityData(liveEntity);
            if (!liveData || !IsValidGuid(liveData->EntityGuid)) {
                return;
            }

            instance->PrefabToInstanceGuid[prefabEntityGuid] = liveData->EntityGuid;
            instance->InstanceToPrefabGuid[liveData->EntityGuid] = prefabEntityGuid;
        }
    };

    std::function<void(const json&)> visitPrefabEntityJson = [&](const json& entityJson) {
        if (!entityJson.is_object()) {
            return;
        }

        addPrefabEntityMapping(entityJson);

        if (entityJson.contains("children") && entityJson["children"].is_array()) {
            for (const json& childJson : entityJson["children"]) {
                visitPrefabEntityJson(childJson);
            }
        }
    };

    if (prefab.Entities.is_array()) {
        for (const json& entityJson : prefab.Entities) {
            visitPrefabEntityJson(entityJson);
        }
    }

    for (EntityID entityId : subtree) {
        EntityData* data = scene.GetEntityData(entityId);
        if (!data) {
            continue;
        }

        data->PrefabGuid = prefab.Guid;
        if (IsValidGuid(data->EntityGuid)) {
            instance->OwnedEntityGuids.push_back(data->EntityGuid);
        }
    }

    rootData->PrefabGuid = prefab.Guid;
    rootData->PrefabSource = vpath;
    rootData->PrefabInstance = std::move(instance);

    if (outVirtualPath) {
        *outVirtualPath = vpath;
    }

    std::cout << "[PrefabEditor] Registered prefab asset " << vpath
              << " and linked source entity to GUID "
              << prefab.Guid.ToString() << "\n";
    return true;
}

//==============================================================================
// Override Computation
//==============================================================================

// Collision-resistant GUID packing using FNV-1a hash
// XOR-based packing can collide (e.g., {1,2} and {5,0} both map to 5)
// FNV-1a provides better distribution with minimal collisions
static uint64_t PackGuid(const ClaymoreGUID& g) { 
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

struct PrefabEntityRecord {
    const json* Entity = nullptr;
    ClaymoreGUID Guid{};
    ClaymoreGUID ParentGuid{};
    std::string NodePath;
    std::string NormalizedPath;
    std::string NormalizedName;
    std::string StableMeshName;
    std::string ParentNormalizedName;
    uint64_t ContentHash = 0;
    int MeshFileId = -1;
};

static constexpr EntityID kInvalidSerializedEntityId = static_cast<EntityID>(-1);

static ClaymoreGUID RemapInstanceGuidToPrefabGuid(
    const std::unordered_map<ClaymoreGUID, ClaymoreGUID>* instanceToPrefabGuid,
    const ClaymoreGUID& guid) {
    if (!instanceToPrefabGuid) {
        return guid;
    }
    auto it = instanceToPrefabGuid->find(guid);
    if (it != instanceToPrefabGuid->end()) {
        return it->second;
    }
    return guid;
}

static std::string ExtractStableMeshName(const json& entity) {
    if (entity.contains("mesh") && entity["mesh"].is_object()) {
        const auto& mesh = entity["mesh"];
        std::string meshName = mesh.value("meshName", std::string());
        if (!meshName.empty()) {
            return meshName;
        }
    }
    return entity.value("name", std::string());
}

static int ExtractMeshFileId(const json& entity) {
    if (!entity.contains("mesh") || !entity["mesh"].is_object()) {
        return -1;
    }
    const auto& mesh = entity["mesh"];
    if (mesh.contains("meshReference") && mesh["meshReference"].is_object()) {
        return mesh["meshReference"].value("fileID", -1);
    }
    return mesh.value("fileID", -1);
}

static uint64_t ComputeSerializedEntityContentHash(const json& entity) {
    uint64_t hash = 0;

    if (entity.contains("mesh") && entity["mesh"].is_object()) {
        const auto& mesh = entity["mesh"];
        hash ^= static_cast<uint64_t>(ExtractMeshFileId(entity)) * 2654435761ULL;
        if (mesh.contains("meshReference") && mesh["meshReference"].is_object()) {
            ClaymoreGUID meshGuid{};
            try { mesh["meshReference"].at("guid").get_to(meshGuid); } catch (...) { meshGuid = {}; }
            hash ^= meshGuid.high;
            hash ^= meshGuid.low * 31ULL;
        }
    }

    auto roundF = [](float v) -> int64_t {
        return static_cast<int64_t>(std::llround(static_cast<double>(v) * 1000.0));
    };
    auto accumulateVec3 = [&](const json& vec, uint64_t xMul, uint64_t yMul, uint64_t zMul) {
        if (!vec.is_array() || vec.size() != 3) return;
        hash ^= static_cast<uint64_t>(roundF(vec[0].get<float>())) * xMul;
        hash ^= static_cast<uint64_t>(roundF(vec[1].get<float>())) * yMul;
        hash ^= static_cast<uint64_t>(roundF(vec[2].get<float>())) * zMul;
    };

    if (entity.contains("transform") && entity["transform"].is_object()) {
        const auto& transform = entity["transform"];
        if (transform.contains("position")) {
            accumulateVec3(transform["position"], 73856093ULL, 19349663ULL, 83492791ULL);
        }
        if (transform.contains("scale")) {
            accumulateVec3(transform["scale"], 1610612741ULL, 805306457ULL, 402653189ULL);
        }
    }

    return hash;
}

static prefab::PropertyOverride::ResolutionHints BuildResolutionHints(
    Scene& scene,
    EntityID instanceRoot,
    EntityID id) {
    prefab::PropertyOverride::ResolutionHints hints;
    auto* data = scene.GetEntityData(id);
    if (!data) {
        return hints;
    }

    hints.NodePath = prefab::ComputeNodePath(scene, instanceRoot, id);
    hints.NormalizedPath = prefab::NormalizePath(hints.NodePath);
    hints.NormalizedName = prefab::NormalizeName(data->Name);
    hints.ContentHash = prefab::ComputeNodeContentHash(scene, id);
    hints.MeshFileId = (data->Mesh && data->Mesh->meshReference.fileID >= 0)
        ? data->Mesh->meshReference.fileID
        : -1;
    hints.StableMeshName = data->Mesh
        ? (data->Mesh->MeshName.empty() ? data->Name : data->Mesh->MeshName)
        : data->Name;
    if (data->Parent != INVALID_ENTITY_ID) {
        if (auto* parentData = scene.GetEntityData(data->Parent)) {
            hints.ParentNormalizedName = prefab::NormalizeName(parentData->Name);
        }
    }
    return hints;
}

static json SerializeResolutionHints(const prefab::PropertyOverride::ResolutionHints& hints) {
    json value = json::object();
    if (!hints.NodePath.empty()) value["nodePath"] = hints.NodePath;
    if (!hints.NormalizedPath.empty()) value["normalizedPath"] = hints.NormalizedPath;
    if (!hints.NormalizedName.empty()) value["normalizedName"] = hints.NormalizedName;
    if (!hints.StableMeshName.empty()) value["stableMeshName"] = hints.StableMeshName;
    if (!hints.ParentNormalizedName.empty()) value["parentName"] = hints.ParentNormalizedName;
    if (hints.ContentHash != 0) value["contentHash"] = hints.ContentHash;
    if (hints.MeshFileId >= 0) value["meshFileId"] = hints.MeshFileId;
    return value;
}

static json NormalizeComparableValue(const std::string& key, const json& value) {
    if (value.is_null()) {
        return value;
    }

    if (key == "transform" && value.is_object()) {
        json normalized = json::object();
        if (value.contains("position")) normalized["position"] = value["position"];
        if (value.contains("rotation")) normalized["rotation"] = value["rotation"];
        if (value.contains("scale")) normalized["scale"] = value["scale"];
        if (value.contains("useQuatRotation")) normalized["useQuatRotation"] = value["useQuatRotation"];
        if (value.contains("rotationQ")) normalized["rotationQ"] = value["rotationQ"];
        return normalized;
    }

    if (key == "scripts" && value.is_array()) {
        auto scripts = value.get<std::vector<json>>();
        std::sort(scripts.begin(), scripts.end(), [](const json& a, const json& b) {
            const std::string classA = a.value("className", std::string());
            const std::string classB = b.value("className", std::string());
            if (classA == classB) {
                return a.dump() < b.dump();
            }
            return classA < classB;
        });
        return scripts;
    }

    if (key == "skeleton" && value.is_object()) {
        json normalized = json::object();
        if (value.contains("skeletonGuid")) normalized["skeletonGuid"] = value["skeletonGuid"];
        if (value.contains("boneNames")) normalized["boneNames"] = value["boneNames"];
        if (value.contains("boneParents")) normalized["boneParents"] = value["boneParents"];
        if (value.contains("jointGuids")) normalized["jointGuids"] = value["jointGuids"];
        return normalized;
    }

    if (key == "skinning" && value.is_object()) {
        json normalized = json::object();
        if (value.contains("meshGuid")) normalized["meshGuid"] = value["meshGuid"];
        if (value.contains("useParentSkeleton")) normalized["useParentSkeleton"] = value["useParentSkeleton"];
        if (value.contains("originalBoneNames")) normalized["originalBoneNames"] = value["originalBoneNames"];
        if (value.contains("originalInverseBindPoses")) normalized["originalInverseBindPoses"] = value["originalInverseBindPoses"];
        return normalized;
    }

    return value;
}

static std::unordered_map<uint64_t, PrefabEntityRecord> BuildPrefabEntityRecords(const PrefabAsset& prefab) {
    std::unordered_map<uint64_t, PrefabEntityRecord> records;
    if (!prefab.Entities.is_array()) {
        return records;
    }

    std::unordered_map<EntityID, ClaymoreGUID> idToGuid;
    std::unordered_map<uint64_t, std::string> nameByGuid;
    std::unordered_map<uint64_t, ClaymoreGUID> parentByGuid;

    for (const auto& entity : prefab.Entities) {
        if (!entity.is_object() || !entity.contains("guid")) continue;
        ClaymoreGUID guid{};
        try { entity.at("guid").get_to(guid); } catch (...) { continue; }
        if (entity.contains("id")) {
            try {
                idToGuid[entity.at("id").get<EntityID>()] = guid;
            } catch (...) {}
        }
        nameByGuid[PackGuid(guid)] = entity.value("name", std::string());
    }

    for (const auto& entity : prefab.Entities) {
        if (!entity.is_object() || !entity.contains("guid")) continue;
        ClaymoreGUID guid{};
        try { entity.at("guid").get_to(guid); } catch (...) { continue; }

        ClaymoreGUID parentGuid{};
        if (entity.contains("parent")) {
            try {
                EntityID parentId = entity.at("parent").get<EntityID>();
                if (parentId != kInvalidSerializedEntityId) {
                    auto it = idToGuid.find(parentId);
                    if (it != idToGuid.end()) {
                        parentGuid = it->second;
                    }
                }
            } catch (...) {}
        }
        parentByGuid[PackGuid(guid)] = parentGuid;
    }

    std::unordered_map<uint64_t, std::string> pathCache;
    std::function<std::string(const ClaymoreGUID&)> buildPath = [&](const ClaymoreGUID& guid) -> std::string {
        const uint64_t key = PackGuid(guid);
        auto cached = pathCache.find(key);
        if (cached != pathCache.end()) {
            return cached->second;
        }

        const std::string name = nameByGuid.count(key) > 0 ? nameByGuid.at(key) : std::string();
        const ClaymoreGUID parentGuid = parentByGuid.count(key) > 0 ? parentByGuid.at(key) : ClaymoreGUID{};
        std::string path;
        if (guid == prefab.RootGuid) {
            path.clear();
        } else if (parentGuid.high == 0 && parentGuid.low == 0) {
            path = name;
        } else {
            const std::string parentPath = buildPath(parentGuid);
            path = parentPath.empty() ? name : (parentPath + "/" + name);
        }
        pathCache[key] = path;
        return path;
    };

    for (const auto& entity : prefab.Entities) {
        if (!entity.is_object() || !entity.contains("guid")) continue;
        ClaymoreGUID guid{};
        try { entity.at("guid").get_to(guid); } catch (...) { continue; }

        PrefabEntityRecord record;
        record.Entity = &entity;
        record.Guid = guid;
        record.ParentGuid = parentByGuid.count(PackGuid(guid)) > 0 ? parentByGuid.at(PackGuid(guid)) : ClaymoreGUID{};
        record.NodePath = buildPath(guid);
        record.NormalizedPath = prefab::NormalizePath(record.NodePath);
        record.NormalizedName = prefab::NormalizeName(entity.value("name", std::string()));
        record.StableMeshName = ExtractStableMeshName(entity);
        record.ContentHash = ComputeSerializedEntityContentHash(entity);
        record.MeshFileId = ExtractMeshFileId(entity);
        if (!(record.ParentGuid.high == 0 && record.ParentGuid.low == 0)) {
            const uint64_t parentKey = PackGuid(record.ParentGuid);
            auto itName = nameByGuid.find(parentKey);
            if (itName != nameByGuid.end()) {
                record.ParentNormalizedName = prefab::NormalizeName(itName->second);
            }
        }

        records[PackGuid(guid)] = std::move(record);
    }

    return records;
}

std::vector<prefab::PropertyOverride> ComputeOverrides(
    const PrefabAsset& basePrefab, 
    Scene& instanceScene, 
    EntityID instanceRoot
) {
    std::vector<prefab::PropertyOverride> overrides;
    if (instanceRoot == INVALID_ENTITY_ID) return overrides;

    const auto baseRecords = BuildPrefabEntityRecords(basePrefab);

    const EntityData* instanceRootData = instanceScene.GetEntityData(instanceRoot);
    const std::unordered_map<ClaymoreGUID, ClaymoreGUID>* instanceToPrefabGuid = nullptr;
    if (instanceRootData && instanceRootData->PrefabInstance) {
        instanceToPrefabGuid = &instanceRootData->PrefabInstance->InstanceToPrefabGuid;
    }
    
    // Build live instance GUID -> EntityID map
    std::unordered_map<uint64_t, EntityID> liveByGuid;
    std::function<void(EntityID)> collectLive = [&](EntityID id) {
        auto* d = instanceScene.GetEntityData(id);
        if (!d) return;
        ClaymoreGUID compareGuid = RemapInstanceGuidToPrefabGuid(instanceToPrefabGuid, d->EntityGuid);
        liveByGuid[PackGuid(compareGuid)] = id;
        for (EntityID c : d->Children) collectLive(c);
    };
    collectLive(instanceRoot);

    static const std::unordered_set<std::string> kIgnoredOverrideKeys = {
        "id", "guid", "name", "parent", "children",
        "prefabInstance", "prefabGuid", "prefabSource", "asset"
    };

    // Compare each base entity with its live counterpart
    for (const auto& [packedGuid, record] : baseRecords) {
        auto itLive = liveByGuid.find(packedGuid);
        if (itLive == liveByGuid.end()) continue; // Entity removed (handled elsewhere)

        EntityID liveId = itLive->second;
        auto* liveData = instanceScene.GetEntityData(liveId);
        if (!liveData) continue;

        json liveJson = Serializer::SerializeEntity(liveId, instanceScene);
        const ClaymoreGUID entityGuid = RemapInstanceGuidToPrefabGuid(instanceToPrefabGuid, liveData->EntityGuid);
        const auto hints = BuildResolutionHints(instanceScene, instanceRoot, liveId);

        auto appendOverride = [&](const std::string& key, const json& value) {
            prefab::PropertyOverride ov;
            ov.TargetEntityGuid = entityGuid;
            ov.ComponentKey = key;
            ov.Value = value;
            ov.Hints = hints;
            overrides.push_back(std::move(ov));
        };

        // Compare name
        std::string baseName = record.Entity->value("name", "");
        std::string liveName = liveJson.value("name", "");
        if (baseName != liveName) {
            appendOverride("name", liveName);
        }

        ClaymoreGUID liveParentGuid{};
        if (liveData->Parent != INVALID_ENTITY_ID) {
            if (auto* liveParentData = instanceScene.GetEntityData(liveData->Parent)) {
                liveParentGuid = RemapInstanceGuidToPrefabGuid(instanceToPrefabGuid, liveParentData->EntityGuid);
            }
        }
        if (liveParentGuid != record.ParentGuid) {
            if (liveParentGuid.high == 0 && liveParentGuid.low == 0) {
                appendOverride("parent", json());
            } else {
                json parentValue = json::object();
                parentValue["guid"] = liveParentGuid;
                if (liveData->Parent != INVALID_ENTITY_ID) {
                    parentValue["hints"] = SerializeResolutionHints(BuildResolutionHints(instanceScene, instanceRoot, liveData->Parent));
                }
                appendOverride("parent", parentValue);
            }
        }

        std::unordered_set<std::string> keys;
        keys.reserve(record.Entity->size() + liveJson.size());
        for (auto it = record.Entity->begin(); it != record.Entity->end(); ++it) {
            if (kIgnoredOverrideKeys.count(it.key()) == 0) {
                keys.insert(it.key());
            }
        }
        for (auto it = liveJson.begin(); it != liveJson.end(); ++it) {
            if (kIgnoredOverrideKeys.count(it.key()) == 0) {
                keys.insert(it.key());
            }
        }

        for (const auto& key : keys) {
            const bool baseHas = record.Entity->contains(key);
            const bool liveHas = liveJson.contains(key);
            const json baseComparable = baseHas ? NormalizeComparableValue(key, (*record.Entity)[key]) : json();
            const json liveComparable = liveHas ? NormalizeComparableValue(key, liveJson[key]) : json();

            if (baseHas == liveHas && baseComparable == liveComparable) {
                continue;
            }

            appendOverride(key, liveHas ? liveJson[key] : json());
        }
    }

    return overrides;
}

std::vector<ClaymoreGUID> GetModifiedEntityGuids(
    const PrefabAsset& basePrefab,
    Scene& instanceScene,
    EntityID instanceRoot
) {
    auto overrides = ComputeOverrides(basePrefab, instanceScene, instanceRoot);
    
    std::unordered_set<uint64_t> seen;
    std::vector<ClaymoreGUID> result;
    
    for (const auto& ov : overrides) {
        uint64_t packed = PackGuid(ov.TargetEntityGuid);
        if (seen.insert(packed).second) {
            result.push_back(ov.TargetEntityGuid);
        }
    }
    
    return result;
}

std::vector<prefab::AddedEntity> GetAddedEntities(
    const PrefabAsset& basePrefab,
    Scene& instanceScene,
    EntityID instanceRoot
) {
    std::vector<prefab::AddedEntity> added;

    // Build base GUID set
    std::unordered_set<uint64_t> baseGuids;
    for (const auto& e : basePrefab.Entities) {
        if (e.contains("guid")) {
            ClaymoreGUID g;
            try { e.at("guid").get_to(g); } catch (...) { continue; }
            baseGuids.insert(PackGuid(g));
        }
    }

    ScopedPrefabAuthoringSerialization authoringScope(instanceScene, instanceRoot);

    std::function<prefab::AddedEntity(EntityID, const ClaymoreGUID&, const prefab::PropertyOverride::ResolutionHints&)> captureAddedSubtree =
        [&](EntityID id, const ClaymoreGUID& parentGuid, const prefab::PropertyOverride::ResolutionHints& parentHints) -> prefab::AddedEntity {
            auto* d = instanceScene.GetEntityData(id);
            prefab::AddedEntity addedEntity;
            if (!d) {
                return addedEntity;
            }

            addedEntity.Guid = d->EntityGuid;
            addedEntity.Name = d->Name;
            addedEntity.ParentGuid = parentGuid;
            addedEntity.ParentNodePath = parentHints.NodePath;
            addedEntity.ParentNormalizedPath = parentHints.NormalizedPath;
            addedEntity.ParentNormalizedName = parentHints.NormalizedName;
            addedEntity.ParentStableMeshName = parentHints.StableMeshName;
            addedEntity.ParentContentHash = parentHints.ContentHash;
            addedEntity.ParentMeshFileId = parentHints.MeshFileId;
            addedEntity.Components = Serializer::SerializeEntity(id, instanceScene);

            const auto selfHints = BuildResolutionHints(instanceScene, instanceRoot, id);
            for (EntityID child : d->Children) {
                auto* childData = instanceScene.GetEntityData(child);
                if (!childData) continue;
                if (baseGuids.count(PackGuid(childData->EntityGuid)) == 0) {
                    addedEntity.Children.push_back(captureAddedSubtree(child, d->EntityGuid, selfHints));
                }
            }

            return addedEntity;
        };

    // Find entities in instance that aren't in base
    std::function<void(EntityID, const ClaymoreGUID&, const prefab::PropertyOverride::ResolutionHints&)> findAdded =
        [&](EntityID id, const ClaymoreGUID& parentGuid, const prefab::PropertyOverride::ResolutionHints& parentHints) {
        auto* d = instanceScene.GetEntityData(id);
        if (!d) return;

        if (baseGuids.count(PackGuid(d->EntityGuid)) == 0) {
            added.push_back(captureAddedSubtree(id, parentGuid, parentHints));
            return;
        }

        const auto selfHints = BuildResolutionHints(instanceScene, instanceRoot, id);
        for (EntityID c : d->Children) {
            findAdded(c, d->EntityGuid, selfHints);
        }
    };

    findAdded(instanceRoot, ClaymoreGUID{}, prefab::PropertyOverride::ResolutionHints{});
    return added;
}

std::vector<prefab::RemovedEntity> GetRemovedEntities(
    const PrefabAsset& basePrefab,
    Scene& instanceScene,
    EntityID instanceRoot
) {
    std::vector<prefab::RemovedEntity> removed;

    const auto baseRecords = BuildPrefabEntityRecords(basePrefab);
    ScopedPrefabAuthoringSerialization authoringScope(instanceScene, instanceRoot);

    // Build live GUID set
    std::unordered_set<uint64_t> liveGuids;
    std::function<void(EntityID)> collectLive = [&](EntityID id) {
        auto* d = instanceScene.GetEntityData(id);
        if (!d) return;
        liveGuids.insert(PackGuid(d->EntityGuid));
        for (EntityID c : d->Children) collectLive(c);
    };
    collectLive(instanceRoot);

    // Find base entities not in instance
    for (const auto& [packedGuid, record] : baseRecords) {
        if (record.Guid == basePrefab.RootGuid) {
            continue;
        }

        if (liveGuids.count(packedGuid) == 0) {
            prefab::RemovedEntity re;
            re.Guid = record.Guid;
            re.NodePath = record.NodePath;
            re.NormalizedPath = record.NormalizedPath;
            re.NormalizedName = record.NormalizedName;
            re.StableMeshName = record.StableMeshName;
            re.ParentNormalizedName = record.ParentNormalizedName;
            re.ContentHash = record.ContentHash;
            re.MeshFileId = record.MeshFileId;
            removed.push_back(std::move(re));
        }
    }

    return removed;
}

//==============================================================================
// Validation
//==============================================================================

PrefabDiagnostics ValidatePrefab(const PrefabAsset& prefab) {
    PrefabDiagnostics diag;
    
    if (prefab.Guid.high == 0 && prefab.Guid.low == 0) {
        diag.Errors.push_back("Prefab has no GUID");
    }
    
    if (prefab.Name.empty()) {
        diag.Warnings.push_back("Prefab has no name");
    }
    
    if (!prefab.IsValid()) {
        diag.Errors.push_back("Prefab has no entities");
    }
    
    // Check for root entity
    if (prefab.RootGuid.high != 0 || prefab.RootGuid.low != 0) {
        if (!prefab.FindRootEntity()) {
            diag.Errors.push_back("Root entity GUID not found in entities");
        }
    } else {
        diag.Warnings.push_back("Prefab has no root GUID set");
    }
    
    // Check for duplicate GUIDs
    std::unordered_set<uint64_t> seenGuids;
    for (const auto& e : prefab.Entities) {
        if (e.contains("guid")) {
            ClaymoreGUID g;
            try { e.at("guid").get_to(g); } catch (...) { continue; }
            uint64_t packed = PackGuid(g);
            if (!seenGuids.insert(packed).second) {
                diag.Errors.push_back("Duplicate entity GUID: " + g.ToString());
            }
        }
    }
    
    return diag;
}

PrefabDiagnostics ValidatePrefabFile(const std::string& path) {
    PrefabAsset asset;
    if (!PrefabIO::LoadPrefabSource(path, asset)) {
        PrefabDiagnostics diag;
        diag.Errors.push_back("Failed to load prefab file: " + path);
        return diag;
    }
    return ValidatePrefab(asset);
}

//==============================================================================
// Apply Changes to Prefab Asset
//==============================================================================

bool ApplyInstanceToPrefab(
    Scene& instanceScene,
    EntityID instanceRoot,
    PrefabAsset& prefabAsset
) {
    // Rebuild the prefab from the instance scene
    return BuildPrefabAssetFromScene(instanceScene, instanceRoot, prefabAsset);
}

bool ApplySelectedOverridesToPrefab(
    Scene& instanceScene,
    EntityID instanceRoot,
    const std::vector<prefab::PropertyOverride>& overridesToApply,
    PrefabAsset& prefabAsset
) {
    // Apply specific overrides to the prefab asset
    for (const auto& ov : overridesToApply) {
        json* entityJson = prefabAsset.FindEntityByGuid(ov.TargetEntityGuid);
        if (!entityJson) continue;
        
        if (ov.ComponentKey == "name") {
            (*entityJson)["name"] = ov.Value;
        } else {
            (*entityJson)[ov.ComponentKey] = ov.Value;
        }
    }
    
    return true;
}

} // namespace prefab_editor

