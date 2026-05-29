#include "core/prefab/PrefabDelta.h"
#include "core/serialization/Serializer.h"
#include "core/ecs/ScenePostProcessing.h"
#include "core/rendering/StandardMeshManager.h"
#include "core/rendering/MaterialManager.h"
#include "core/animation/ik/IKComponent.h"
#include "core/animation/lookat/LookAtConstraintComponent.h"
#include <glm/glm.hpp>
#include <sstream>
#include <algorithm>
#include <functional>
#include <iostream>
#include <cctype>
#include <cmath>
#include <set>
#include <filesystem>
#include <chrono>
#include <unordered_set>

#ifdef CLAYMORE_EDITOR
#include "editor/pipeline/AssetLibrary.h"
#include "editor/import/ModelBuild.h"
#include "editor/Project.h"
#endif

namespace prefab {

//------------------------------------------------------------------------------
// Path Utilities Implementation
//------------------------------------------------------------------------------

std::string ComputeNodePath(Scene& scene, EntityID root, EntityID descendant) {
    if (root == descendant) return "";
    
    std::vector<std::string> parts;
    EntityID cur = descendant;
    
    while (cur != INVALID_ENTITY_ID && cur != root) {
        auto* d = scene.GetEntityData(cur);
        if (!d) break;
        parts.push_back(d->Name);
        cur = d->Parent;
    }
    
    if (cur != root) return ""; // Not a descendant
    
    // Reverse to get root-to-descendant order
    std::reverse(parts.begin(), parts.end());
    
    std::string path;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) path += "/";
        path += parts[i];
    }
    return path;
}

std::string NormalizeName(const std::string& name) {
    // Strip trailing _<digits> suffix (common in imported models)
    size_t underscore = name.find_last_of('_');
    if (underscore == std::string::npos) return name;
    
    bool allDigits = true;
    for (size_t i = underscore + 1; i < name.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
            allDigits = false;
            break;
        }
    }
    
    return allDigits ? name.substr(0, underscore) : name;
}

std::string NormalizePath(const std::string& path) {
    if (path.empty()) return path;
    
    std::string result;
    std::istringstream ss(path);
    std::string part;
    bool first = true;
    
    while (std::getline(ss, part, '/')) {
        if (!first) result += "/";
        result += NormalizeName(part);
        first = false;
    }
    
    return result;
}

EntityID ResolveByPath(Scene& scene, EntityID root, const std::string& path) {
    if (path.empty()) return root;
    
    EntityID target = root;
    std::istringstream ss(path);
    std::string part;
    
    auto normalize = [](const std::string& name) -> std::string {
        return NormalizeName(name);
    };
    
    auto lower = [](std::string s) -> std::string {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    
    while (std::getline(ss, part, '/')) {
        auto* d = scene.GetEntityData(target);
        if (!d) return INVALID_ENTITY_ID;
        
        const std::string partNorm = normalize(part);
        EntityID next = INVALID_ENTITY_ID;
        
        for (EntityID c : d->Children) {
            auto* cd = scene.GetEntityData(c);
            if (!cd) continue;
            
            const std::string& childName = cd->Name;
            if (childName == part || 
                normalize(childName) == partNorm ||
                lower(childName) == lower(part) ||
                lower(normalize(childName)) == lower(partNorm)) {
                next = c;
                break;
            }
        }
        
        if (next == INVALID_ENTITY_ID) return INVALID_ENTITY_ID;
        target = next;
    }
    
    return target;
}

uint64_t ComputeNodeContentHash(Scene& scene, EntityID id) {
    auto* d = scene.GetEntityData(id);
    if (!d) return 0;
    
    uint64_t hash = 0;
    
    // Include mesh reference in hash
    if (d->Mesh) {
        hash ^= static_cast<uint64_t>(d->Mesh->meshReference.fileID) * 2654435761ULL;
        hash ^= d->Mesh->meshReference.guid.high;
        hash ^= d->Mesh->meshReference.guid.low * 31;
    }
    
    // Include transform in hash (rounded to avoid float noise)
    auto roundF = [](float v) -> int64_t { return static_cast<int64_t>(std::round(v * 1000.0f)); };
    
    hash ^= static_cast<uint64_t>(roundF(d->Transform.Position.x)) * 73856093ULL;
    hash ^= static_cast<uint64_t>(roundF(d->Transform.Position.y)) * 19349663ULL;
    hash ^= static_cast<uint64_t>(roundF(d->Transform.Position.z)) * 83492791ULL;
    hash ^= static_cast<uint64_t>(roundF(d->Transform.Scale.x)) * 1610612741ULL;
    hash ^= static_cast<uint64_t>(roundF(d->Transform.Scale.y)) * 805306457ULL;
    hash ^= static_cast<uint64_t>(roundF(d->Transform.Scale.z)) * 402653189ULL;
    
    return hash;
}

//------------------------------------------------------------------------------
// Entity Classification Implementation
//------------------------------------------------------------------------------

bool IsUserAddedEntity(Scene& scene, EntityID id) {
    auto* d = scene.GetEntityData(id);
    if (!d) return false;
    
    // User-added entities have user-facing components that wouldn't come from model import
    bool hasUserComponent = d->Camera != nullptr ||
           d->Light != nullptr ||
           d->Collider != nullptr ||
           d->RigidBody != nullptr ||
           d->StaticBody != nullptr ||
           d->CharacterController != nullptr ||
           d->Emitter != nullptr ||
           d->Canvas != nullptr ||
           d->Panel != nullptr ||
           d->Button != nullptr ||
           d->Text != nullptr ||
           d->Navigation != nullptr ||
           d->NavAgent != nullptr ||
           d->Area != nullptr ||
           !d->Scripts.empty() ||
           d->Terrain != nullptr ||
           !d->IKs.empty() ||
           !d->LookAtConstraints.empty() ||
           (d->Extra.is_object() && !d->Extra.empty());
    
    if (hasUserComponent) return true;
    
    // Also treat as user-added if the entity has NO model-related components at all
    // (empty entities intentionally added by the user for tracking points, etc.)
    // BUT be careful not to flag bones as user-added - bones are empty entities under a skeleton
    bool hasModelComponent = d->Mesh != nullptr ||
           d->Skeleton != nullptr ||
           d->Skinning != nullptr ||
           d->MeshProxy != nullptr;
    
    if (hasModelComponent) return false;
    
    // Check if this entity is a bone (listed in a skeleton's BoneEntities)
    // If the entity is in BoneEntities, it's a model bone and not user-added
    // Otherwise, even if under a skeleton, it could be user-added (e.g., head tracker empty entity)
    EntityID cur = d->Parent;
    while (cur != INVALID_ENTITY_ID) {
        auto* pd = scene.GetEntityData(cur);
        if (!pd) break;
        if (pd->Skeleton && !pd->Skeleton->BoneEntities.empty()) {
            // Check if 'id' is in the skeleton's bone list
            for (EntityID boneId : pd->Skeleton->BoneEntities) {
                if (boneId == id) {
                    // This is a bone - not user-added
                    return false;
                }
            }
            // Entity is under a skeleton but NOT in its BoneEntities - likely user-added
            return true;
        }
        cur = pd->Parent;
    }
    
    // Empty entity that's not under a skeleton - likely user-added
    return true;
}

std::vector<EntityID> GetAllDescendants(Scene& scene, EntityID root) {
    std::vector<EntityID> result;
    
    std::function<void(EntityID)> collect = [&](EntityID id) {
        auto* d = scene.GetEntityData(id);
        if (!d) return;
        
        for (EntityID c : d->Children) {
            result.push_back(c);
            collect(c);
        }
    };
    
    collect(root);
    return result;
}

//------------------------------------------------------------------------------
// Delta Computation Implementation - DEPRECATED
//------------------------------------------------------------------------------
// The ComputeModelDelta function has been deprecated. Delta computation is now
// handled during scene serialization using the same 'children' array pattern
// as model roots. This eliminates the complex baseline comparison logic that
// was fragile and hard to maintain.
//
// Overrides are now:
// - Computed implicitly during scene serialization (SerializeScene)
// - Stored in scene file's 'children' array on prefab instance roots
// - Applied during scene deserialization (DeserializeScene)
//
// The kDeltaComponentKeys list below is kept for backward compatibility with
// existing delta serialization/deserialization code.
//------------------------------------------------------------------------------

// Keys for delta component serialization (used by SerializeModelDelta/DeserializeModelDelta)
static const std::vector<std::string> kDeltaComponentKeys = {
    "transform", "mesh", "meshProxy", "skeleton", "skinning",
    "blendShapeWeights", "unifiedMorphWeights",
    "animator", "scripts", "camera", "light", "collider", "rigidbody",
    "staticbody", "characterController", "terrain", "emitter",
    "canvas", "panel", "button", "text", "uiRect", "fitToContent",
    "navmesh", "navagent", "area", "renderOverrides", "ik", "lookat",
    "tintController", "boneAttachment", "slider", "progressBar", "toggle",
    "scrollView", "layoutGroup", "inputField", "dropdown",
    "active", "visible", "layer", "tag"
};

// DEPRECATED: This function is no longer used. Delta computation is now handled
// during scene serialization. Kept for backward compatibility.
ModelDelta ComputeModelDelta(
    Scene& scene,
    EntityID modelRoot,
    const std::string& modelPath,
    const ClaymoreGUID& modelGuid
) {
    // Return empty delta - actual override computation happens in SerializeScene
    (void)scene; (void)modelRoot; (void)modelPath; (void)modelGuid;
    std::cerr << "[PrefabDelta] WARNING: ComputeModelDelta is deprecated. "
              << "Overrides are now computed during scene serialization.\n";
    return ModelDelta{};
}

//------------------------------------------------------------------------------
// Delta Serialization Implementation
// (Kept for backward compatibility with existing scene files)
//------------------------------------------------------------------------------

// NOTE: The complex baseline comparison code (~600 lines) has been removed.
// Delta computation now happens implicitly during scene serialization using
// Serializer::SerializeEntity on prefab instance children, identical to how
// model children are handled.

// The following functions are kept for backward compatibility with existing delta
// serialization in scene files. They are used to serialize/deserialize the
// ModelDelta structure which stores overrides in a JSON format.

//------------------------------------------------------------------------------
// Delta Serialization Implementation
//------------------------------------------------------------------------------

static nlohmann::json SerializeModelAddedEntity(const ModelAddedEntity& added) {
    nlohmann::json j;
    j["_added"] = true;
    j["name"] = added.name;
    j["_parentPath"] = added.parentPath;
    
    if (!(added.guid.high == 0 && added.guid.low == 0)) {
        j["guid"] = added.guid;
    }
    
    // Merge components into the object
    for (auto it = added.components.begin(); it != added.components.end(); ++it) {
        j[it.key()] = it.value();
    }
    
    // Serialize children recursively
    if (!added.children.empty()) {
        nlohmann::json childrenArr = nlohmann::json::array();
        for (const auto& child : added.children) {
            childrenArr.push_back(SerializeModelAddedEntity(child));
        }
        j["_children"] = std::move(childrenArr);
    }
    
    return j;
}

nlohmann::json SerializeModelDelta(const ModelDelta& delta) {
    nlohmann::json arr = nlohmann::json::array();
    
    // Serialize overrides
    for (const auto& override : delta.overrides) {
        nlohmann::json j;
        j["_modelNodePath"] = override.modelNodePath;
        j["_normalizedPath"] = override.normalizedPath;
        j["_normalizedName"] = override.normalizedName;
        
        if (override.contentHash != 0) {
            j["_contentHash"] = override.contentHash;
        }
        if (override.meshFileId >= 0) {
            j["_meshFileId"] = override.meshFileId;
        }
        
        // Stable identity for matching across model re-exports
        if (!override.stableMeshName.empty()) {
            j["_stableMeshName"] = override.stableMeshName;
        }
        if (!override.parentNormalizedName.empty()) {
            j["_parentNormalizedName"] = override.parentNormalizedName;
        }
        
        // Merge components
        for (auto it = override.components.begin(); it != override.components.end(); ++it) {
            j[it.key()] = it.value();
        }
        
        // Include name if it differs from path leaf
        if (!override.name.empty()) {
            j["name"] = override.name;
        }
        
        arr.push_back(std::move(j));
    }
    
    // Serialize added entities
    for (const auto& added : delta.added) {
        arr.push_back(SerializeModelAddedEntity(added));
    }
    
    // Serialize deleted nodes
    for (const auto& deleted : delta.deleted) {
        nlohmann::json j;
        j["_modelNodePath"] = deleted.modelNodePath;
        j["_deleted"] = true;
        j["_normalizedPath"] = deleted.normalizedPath;
        j["_normalizedName"] = deleted.normalizedName;
        if (deleted.contentHash != 0) {
            j["_contentHash"] = deleted.contentHash;
        }
        if (deleted.meshFileId >= 0) {
            j["_meshFileId"] = deleted.meshFileId;
        }
        if (!deleted.stableMeshName.empty()) {
            j["_stableMeshName"] = deleted.stableMeshName;
        }
        if (!deleted.parentNormalizedName.empty()) {
            j["_parentNormalizedName"] = deleted.parentNormalizedName;
        }
        arr.push_back(std::move(j));
    }
    
    return arr;
}

ModelDelta DeserializeModelDelta(const nlohmann::json& j) {
    ModelDelta delta;
    
    if (!j.is_array()) return delta;
    
    std::function<ModelAddedEntity(const nlohmann::json&)> parseAdded = [&](const nlohmann::json& j) -> ModelAddedEntity {
        ModelAddedEntity added;
        const std::string modelNodePath = j.value("_modelNodePath", "");
        added.name = j.value("name", "");
        added.parentPath = j.value("_parentPath", "");

        // Compatibility path: scene-style added entries often carry only _modelNodePath.
        // Derive both name and parentPath so added entities are rebuilt with correct hierarchy.
        if (added.name.empty() && !modelNodePath.empty()) {
            const size_t slash = modelNodePath.find_last_of('/');
            added.name = (slash == std::string::npos) ? modelNodePath : modelNodePath.substr(slash + 1);
        }
        if (added.parentPath.empty() && !modelNodePath.empty()) {
            const size_t slash = modelNodePath.find_last_of('/');
            added.parentPath = (slash == std::string::npos) ? "" : modelNodePath.substr(0, slash);
        }
        
        try {
            if (j.contains("guid")) {
                j.at("guid").get_to(added.guid);
            }
        } catch (...) {}
        
        // Extract components
        for (const auto& key : kDeltaComponentKeys) {
            if (j.contains(key) && !j[key].is_null()) {
                added.components[key] = j[key];
            }
        }
        
        // Parse children recursively
        if (j.contains("_children") && j["_children"].is_array()) {
            for (const auto& child : j["_children"]) {
                added.children.push_back(parseAdded(child));
            }
        }
        // Compatibility for scene-style nested child payloads.
        else if (j.contains("children") && j["children"].is_array()) {
            for (const auto& child : j["children"]) {
                if (!child.is_object()) continue;
                if (child.contains("_added") && !child["_added"].get<bool>()) continue;
                added.children.push_back(parseAdded(child));
            }
        }
        
        return added;
    };
    
    for (const auto& item : j) {
        if (!item.is_object()) continue;
        
        if (item.contains("_deleted") && item["_deleted"].get<bool>()) {
            // Deleted node
            DeletedNode deleted;
            deleted.modelNodePath = item.value("_modelNodePath", "");
            deleted.normalizedPath = item.value("_normalizedPath", "");
            deleted.normalizedName = item.value("_normalizedName", "");
            deleted.contentHash = item.value("_contentHash", 0ULL);
            deleted.meshFileId = item.value("_meshFileId", -1);
            deleted.stableMeshName = item.value("_stableMeshName", "");
            deleted.parentNormalizedName = item.value("_parentNormalizedName", "");
            delta.deleted.push_back(std::move(deleted));
        }
        else if (item.contains("_added") && item["_added"].get<bool>()) {
            // Added entity
            delta.added.push_back(parseAdded(item));
        }
        else if (item.contains("_modelNodePath")) {
            // Override
            NodeOverride override;
            override.modelNodePath = item.value("_modelNodePath", "");
            override.normalizedPath = item.value("_normalizedPath", "");
            override.normalizedName = item.value("_normalizedName", "");
            override.contentHash = item.value("_contentHash", 0ULL);
            override.meshFileId = item.value("_meshFileId", -1);
            override.stableMeshName = item.value("_stableMeshName", "");
            override.parentNormalizedName = item.value("_parentNormalizedName", "");
            override.name = item.value("name", "");
            override.active = item.value("active", true);
            override.visible = item.value("visible", true);
            
            // Extract components
            for (const auto& key : kDeltaComponentKeys) {
                if (item.contains(key) && !item[key].is_null()) {
                    override.components[key] = item[key];
                }
            }
            
            delta.overrides.push_back(std::move(override));
        }
    }
    
    return delta;
}

//------------------------------------------------------------------------------
// Delta Application Implementation
//------------------------------------------------------------------------------

bool ApplyModelDelta(
    Scene& scene,
    EntityID modelRoot,
    const ModelDelta& delta
) {
    auto* rootData = scene.GetEntityData(modelRoot);
    if (!rootData) return false;
    
    std::cout << "[PrefabDelta] Applying delta to model root '" << rootData->Name << "'\n";
    
    // Build lookup maps for efficient resolution
    std::unordered_map<std::string, EntityID> pathToEntity;
    std::unordered_map<std::string, EntityID> normalizedPathToEntity;
    std::unordered_map<std::string, EntityID> normalizedNameToEntity;
    std::unordered_map<uint64_t, std::vector<EntityID>> hashToEntities;
    std::unordered_map<int, EntityID> meshFileIdToEntity;
    
    // Stable mesh name -> list of entities (for disambiguation by parent)
    // Key is normalized mesh name, value is list of (EntityID, parentNormalizedName)
    std::unordered_map<std::string, std::vector<std::pair<EntityID, std::string>>> stableMeshNameToEntities;
    
    std::function<void(EntityID, const std::string&, const std::string&)> buildMaps = [&](EntityID id, const std::string& parentPath, const std::string& parentNormName) {
        auto* d = scene.GetEntityData(id);
        if (!d) return;
        
        std::string path = parentPath.empty() ? d->Name : (parentPath + "/" + d->Name);
        std::string normPath = NormalizePath(path);
        std::string normName = NormalizeName(d->Name);
        
        pathToEntity[path] = id;
        normalizedPathToEntity[normPath] = id;
        normalizedNameToEntity[normName] = id;
        
        // Build stable mesh name map
        std::string stableName = d->Mesh ? (d->Mesh->MeshName.empty() ? d->Name : d->Mesh->MeshName) : d->Name;
        std::string stableNameNorm = NormalizeName(stableName);
        stableMeshNameToEntities[stableNameNorm].push_back({id, parentNormName});
        
        uint64_t hash = ComputeNodeContentHash(scene, id);
        if (hash != 0) {
            hashToEntities[hash].push_back(id);
        }
        
        if (d->Mesh && d->Mesh->meshReference.fileID >= 0) {
            meshFileIdToEntity[d->Mesh->meshReference.fileID] = id;
        }
        
        for (EntityID c : d->Children) {
            buildMaps(c, path, normName);
        }
    };
    
    for (EntityID c : rootData->Children) {
        buildMaps(c, "", "");
    }
    
    // Helper to resolve an entity using multiple strategies
    // Priority: 1. Exact path, 2. Stable mesh name (with parent disambiguation), 3. Normalized path, 4. Content hash, 5. Name only
    auto resolveEntity = [&](
        const std::string& path,
        const std::string& normPath,
        const std::string& normName,
        uint64_t contentHash,
        int meshFileId,
        const std::string& stableMeshName,
        const std::string& parentNormName
    ) -> EntityID {
        // Try exact path first (fast path for unchanged models)
        auto it = pathToEntity.find(path);
        if (it != pathToEntity.end()) return it->second;
        
        // Try stable mesh name - this is the key for cross-export stability
        if (!stableMeshName.empty()) {
            std::string stableNorm = NormalizeName(stableMeshName);
            auto itStable = stableMeshNameToEntities.find(stableNorm);
            if (itStable != stableMeshNameToEntities.end() && !itStable->second.empty()) {
                // If only one match, use it
                if (itStable->second.size() == 1) {
                    return itStable->second.front().first;
                }
                // Multiple matches - disambiguate by parent name
                for (const auto& [entityId, entityParentName] : itStable->second) {
                    if (entityParentName == parentNormName) {
                        return entityId;
                    }
                }
                // Fallback to first match if no parent disambiguation
                return itStable->second.front().first;
            }
        }
        
        // Try normalized path
        it = normalizedPathToEntity.find(normPath);
        if (it != normalizedPathToEntity.end()) return it->second;
        
        // Try mesh file ID (only useful if model hasn't changed mesh count)
        if (meshFileId >= 0) {
            auto itMesh = meshFileIdToEntity.find(meshFileId);
            if (itMesh != meshFileIdToEntity.end()) return itMesh->second;
        }
        
        // Try content hash
        if (contentHash != 0) {
            auto itHash = hashToEntities.find(contentHash);
            if (itHash != hashToEntities.end() && !itHash->second.empty()) {
                // If multiple matches, prefer one with matching normalized name
                for (EntityID candidate : itHash->second) {
                    auto* cd = scene.GetEntityData(candidate);
                    if (cd && NormalizeName(cd->Name) == normName) {
                        return candidate;
                    }
                }
                return itHash->second.front();
            }
        }
        
        // Try fuzzy name match as last resort
        it = normalizedNameToEntity.find(normName);
        if (it != normalizedNameToEntity.end()) return it->second;
        
        return INVALID_ENTITY_ID;
    };

    auto resolveGuid = [&](uint64_t high, uint64_t low) -> EntityID {
        if (high == 0 && low == 0) {
            return INVALID_ENTITY_ID;
        }
        ClaymoreGUID guid{};
        guid.high = high;
        guid.low = low;
        return scene.FindEntityByGUID(guid);
    };
    
    // Apply overrides
    for (const auto& override : delta.overrides) {
        EntityID target = resolveEntity(
            override.modelNodePath,
            override.normalizedPath,
            override.normalizedName,
            override.contentHash,
            override.meshFileId,
            override.stableMeshName,
            override.parentNormalizedName
        );
        
        if (target == INVALID_ENTITY_ID) {
            std::cerr << "[PrefabDelta] WARNING: Could not resolve override target: " 
                      << override.modelNodePath << " (stableMeshName=" << override.stableMeshName << ")\n";
            continue;
        }
        
        auto* d = scene.GetEntityData(target);
        if (!d) continue;
        
        // Entity-level active/visible are part of the prefab baseline when loading from prefab asset.
        // Apply them so that "toggle visibility in prefab editor, save, reload" is deterministic.
        scene.SetEntityActive(target, override.active);
        scene.SetEntityVisibleDirect(target, override.visible);
        
        // Apply name override
        if (!override.name.empty() && override.name != d->Name) {
            scene.SetEntityName(target, override.name);
        }
        
        // Apply transform
        if (override.components.contains("transform")) {
            Serializer::DeserializeTransform(override.components["transform"], d->Transform);
            scene.MarkTransformDirty(target);
        }
        
        // Apply mesh material overrides
        if (override.components.contains("mesh") && d->Mesh) {
            nlohmann::json meshOverride = override.components["mesh"];
            if (meshOverride.is_object() && d->Mesh->mesh) {
                // Keep the freshly instantiated model mesh identity. Delta snapshots may
                // carry stale meshReference/fileID values after model reimport.
                meshOverride.erase("meshReference");
                meshOverride.erase("fileID");
            }
            Serializer::DeserializeMesh(meshOverride, *d->Mesh, d->RenderOverrides);
        }
        
        // Apply render overrides
        if (override.components.contains("renderOverrides")) {
            if (!d->RenderOverrides) d->RenderOverrides = std::make_unique<RenderOverridesComponent>();
            Serializer::DeserializeRenderOverrides(override.components["renderOverrides"], *d->RenderOverrides);
        }
        
        // Apply user components
        if (override.components.contains("camera")) {
            if (!d->Camera) d->Camera = std::make_unique<CameraComponent>();
            Serializer::DeserializeCamera(override.components["camera"], *d->Camera);
        }
        if (override.components.contains("light")) {
            if (!d->Light) d->Light = std::make_unique<LightComponent>();
            Serializer::DeserializeLight(override.components["light"], *d->Light);
        }
        if (override.components.contains("collider")) {
            if (!d->Collider) d->Collider = std::make_unique<ColliderComponent>();
            Serializer::DeserializeCollider(override.components["collider"], *d->Collider);
        }
        if (override.components.contains("rigidbody")) {
            if (!d->RigidBody) d->RigidBody = std::make_unique<RigidBodyComponent>();
            Serializer::DeserializeRigidBody(override.components["rigidbody"], *d->RigidBody);
        }
        if (override.components.contains("staticbody")) {
            if (!d->StaticBody) d->StaticBody = std::make_unique<StaticBodyComponent>();
            Serializer::DeserializeStaticBody(override.components["staticbody"], *d->StaticBody);
        }
        if (override.components.contains("characterController")) {
            if (!d->CharacterController) d->CharacterController = std::make_unique<CharacterControllerComponent>();
            Serializer::DeserializeCharacterController(override.components["characterController"], *d->CharacterController);
        }
        if (override.components.contains("emitter")) {
            if (!d->Emitter) d->Emitter = std::make_unique<ParticleEmitterComponent>();
            Serializer::DeserializeParticleEmitter(override.components["emitter"], *d->Emitter);
        }
        if (override.components.contains("canvas")) {
            if (!d->Canvas) d->Canvas = std::make_unique<CanvasComponent>();
            Serializer::DeserializeCanvas(override.components["canvas"], *d->Canvas);
        }
        if (override.components.contains("panel")) {
            if (!d->Panel) d->Panel = std::make_unique<PanelComponent>();
            Serializer::DeserializePanel(override.components["panel"], *d->Panel);
        }
        if (override.components.contains("button")) {
            if (!d->Button) d->Button = std::make_unique<ButtonComponent>();
            Serializer::DeserializeButton(override.components["button"], *d->Button);
        }
        if (override.components.contains("text")) {
            if (!d->Text) d->Text = std::make_unique<TextRendererComponent>();
            Serializer::DeserializeText(override.components["text"], *d->Text);
        }
        if (override.components.contains("scripts")) {
            Serializer::DeserializeScripts(override.components["scripts"], d->Scripts, scene);
        }
        if (override.components.contains("animator")) {
            if (!d->AnimationPlayer) d->AnimationPlayer = std::make_unique<cm::animation::AnimationPlayerComponent>();
            Serializer::DeserializeAnimator(override.components["animator"], *d->AnimationPlayer);
        }
        if (override.components.contains("terrain")) {
            if (!d->Terrain) d->Terrain = std::make_unique<TerrainComponent>();
            Serializer::DeserializeTerrain(override.components["terrain"], *d->Terrain);
        }
        // Area component
        if (override.components.contains("area")) {
            if (!d->Area) d->Area = std::make_unique<cm::physics::AreaComponent>();
            Serializer::DeserializeArea(override.components["area"], *d->Area);
            std::cout << "[PrefabDelta] Applied area component to '" << d->Name << "'\n";
        }
        // IK constraints
        if (override.components.contains("ik") && override.components["ik"].is_array()) {
            d->IKs.clear();
            for (const auto& j : override.components["ik"]) {
                if (!j.is_object()) continue;
                cm::animation::ik::IKComponent c;
                c.Enabled = j.value("enabled", true);
                c.TargetEntity = j.value("target", (EntityID)0);
                c.PoleEntity = j.value("pole", (EntityID)0);
                c.TargetEntityGuidHigh = j.value("targetGuidHigh", (uint64_t)0);
                c.TargetEntityGuidLow = j.value("targetGuidLow", (uint64_t)0);
                c.PoleEntityGuidHigh = j.value("poleGuidHigh", (uint64_t)0);
                c.PoleEntityGuidLow = j.value("poleGuidLow", (uint64_t)0);
                if (EntityID resolvedTarget = resolveGuid(c.TargetEntityGuidHigh, c.TargetEntityGuidLow);
                    resolvedTarget != INVALID_ENTITY_ID) {
                    c.TargetEntity = resolvedTarget;
                }
                if (EntityID resolvedPole = resolveGuid(c.PoleEntityGuidHigh, c.PoleEntityGuidLow);
                    resolvedPole != INVALID_ENTITY_ID) {
                    c.PoleEntity = resolvedPole;
                }
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
                        cc.useHinge = cj.value("useHinge", false);
                        cc.useTwist = cj.value("useTwist", false);
                        cc.hingeMinDeg = cj.value("hingeMinDeg", 0.0f);
                        cc.hingeMaxDeg = cj.value("hingeMaxDeg", 0.0f);
                        cc.twistMinDeg = cj.value("twistMinDeg", 0.0f);
                        cc.twistMaxDeg = cj.value("twistMaxDeg", 0.0f);
                        c.Constraints.push_back(cc);
                    }
                }
                d->IKs.push_back(std::move(c));
            }
        }
        // LookAt constraints
        if (override.components.contains("lookat") && override.components["lookat"].is_array()) {
            d->LookAtConstraints.clear();
            for (const auto& j : override.components["lookat"]) {
                if (!j.is_object()) continue;
                cm::animation::lookat::LookAtConstraintComponent lac;
                lac.Enabled = j.value("enabled", true);
                lac.Mode = static_cast<cm::animation::lookat::LookAtMode>(j.value("mode", (uint8_t)cm::animation::lookat::LookAtMode::LookAtPosition));
                lac.TargetEntity = j.value("target", (EntityID)0);
                lac.TargetEntityGuidHigh = j.value("targetGuidHigh", (uint64_t)0);
                lac.TargetEntityGuidLow = j.value("targetGuidLow", (uint64_t)0);
                if (EntityID resolvedTarget = resolveGuid(lac.TargetEntityGuidHigh, lac.TargetEntityGuidLow);
                    resolvedTarget != INVALID_ENTITY_ID) {
                    lac.TargetEntity = resolvedTarget;
                }
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
                d->LookAtConstraints.push_back(std::move(lac));
            }
        }
        // TintController
        if (override.components.contains("tintController")) {
            if (!d->TintController) d->TintController = std::make_unique<TintMaskController>();
            Serializer::DeserializeTintController(override.components["tintController"], *d->TintController);
            
            // Resolve entityPath to EntityIDs within the model subtree
            const auto& tintJson = override.components["tintController"];
            if (tintJson.contains("targets") && tintJson["targets"].is_array()) {
                size_t idx = 0;
                for (const auto& targetJ : tintJson["targets"]) {
                    if (idx < d->TintController->Targets.size() && targetJ.contains("entityPath")) {
                        std::string entityPath = targetJ["entityPath"].get<std::string>();
                        EntityID resolvedTarget = INVALID_ENTITY_ID;
                        if (entityPath.empty()) {
                            resolvedTarget = target;  // Empty path = this entity
                        } else {
                            // Try normalized path lookup first
                            auto itPath = normalizedPathToEntity.find(NormalizePath(entityPath));
                            if (itPath != normalizedPathToEntity.end()) {
                                resolvedTarget = itPath->second;
                            } else {
                                // Fallback to exact path
                                auto itExact = pathToEntity.find(entityPath);
                                if (itExact != pathToEntity.end()) {
                                    resolvedTarget = itExact->second;
                                }
                            }
                        }
                        if (resolvedTarget != INVALID_ENTITY_ID) {
                            d->TintController->Targets[idx].TargetEntity = resolvedTarget;
                        }
                    }
                    idx++;
                }
            }
            d->TintController->NeedsRefresh = true;
            std::cout << "[PrefabDelta] Applied tintController to '" << d->Name << "'\n";
        }
        // BoneAttachment
        if (override.components.contains("boneAttachment")) {
            if (!d->BoneAttachment) d->BoneAttachment = std::make_unique<BoneAttachmentComponent>();
            Serializer::DeserializeBoneAttachment(override.components["boneAttachment"], *d->BoneAttachment);
            std::cout << "[PrefabDelta] Applied boneAttachment to '" << d->Name << "'\n";
        }
    }
    
    // Apply deleted nodes
    for (const auto& deleted : delta.deleted) {
        EntityID target = resolveEntity(
            deleted.modelNodePath,
            deleted.normalizedPath,
            deleted.normalizedName,
            deleted.contentHash,
            deleted.meshFileId,
            deleted.stableMeshName,
            deleted.parentNormalizedName
        );
        
        if (target != INVALID_ENTITY_ID) {
            std::cout << "[PrefabDelta] Deleting entity at path: " << deleted.modelNodePath << "\n";
            scene.RemoveEntity(target);
        }
    }
    
    // Apply added entities using Serializer::DeserializeEntityData as single source of truth
    std::function<EntityID(const ModelAddedEntity&, EntityID)> createAdded = [&](
        const ModelAddedEntity& added,
        EntityID parent
    ) -> EntityID {
        Entity ent = scene.CreateEntity(added.name);
        EntityID id = ent.GetID();
        auto* d = scene.GetEntityData(id);
        if (!d) return INVALID_ENTITY_ID;
        
        // Set GUID if provided
        if (!(added.guid.high == 0 && added.guid.low == 0)) {
            d->EntityGuid = added.guid;
        }
        
        // Parent it
        if (parent != INVALID_ENTITY_ID) {
            scene.SetParent(id, parent);
        }
        
        // Use the single source of truth for entity deserialization
        // This handles ALL components consistently with scene loading
        Serializer::DeserializeEntityData(added.components, d, scene);
        
        // Mark transform dirty after deserialization
        scene.MarkTransformDirty(id);
        
        // Create children recursively
        for (const auto& child : added.children) {
            createAdded(child, id);
        }
        
        return id;
    };
    
    // Collect all added entity IDs for post-processing
    std::vector<EntityID> addedEntityIds;
    
    // Modified createAdded to track created entities
    std::function<EntityID(const ModelAddedEntity&, EntityID)> createAddedTracked = [&](
        const ModelAddedEntity& added,
        EntityID parent
    ) -> EntityID {
        EntityID id = createAdded(added, parent);
        if (id != INVALID_ENTITY_ID) {
            addedEntityIds.push_back(id);
            // Also add children recursively (they were created by createAdded)
            auto* data = scene.GetEntityData(id);
            if (data) {
                std::function<void(EntityID)> collectChildren = [&](EntityID pid) {
                    auto* pd = scene.GetEntityData(pid);
                    if (!pd) return;
                    for (EntityID c : pd->Children) {
                        addedEntityIds.push_back(c);
                        collectChildren(c);
                    }
                };
                collectChildren(id);
            }
        }
        return id;
    };
    
    for (const auto& added : delta.added) {
        // Find parent
        EntityID parent = modelRoot;
        if (!added.parentPath.empty()) {
            parent = ResolveByPath(scene, modelRoot, added.parentPath);
            if (parent == INVALID_ENTITY_ID) {
                std::cerr << "[PrefabDelta] WARNING: Could not resolve parent path for added entity: "
                          << added.parentPath << "\n";
                parent = modelRoot;
            }
        }
        
        std::cout << "[PrefabDelta] Creating added entity: " << added.name 
                  << " under parent path: " << added.parentPath << "\n";
        
        createAddedTracked(added, parent);
    }
    
    // Use shared post-processing module (same as scene loading)
    // This ensures prefabs behave identically to scenes
    if (!addedEntityIds.empty()) {
        std::cout << "[PrefabDelta] Running post-processing on " << addedEntityIds.size() << " added entities\n";
        cm::PostProcessEntities(scene, addedEntityIds);
    }

    // Final strict GUID-based fixup for constraint targets. This guarantees component refs
    // (e.g. aim/look-at/IK) remain stable even when raw IDs in delta payloads are stale.
    std::function<void(EntityID)> fixConstraintRefs = [&](EntityID id) {
        EntityData* d = scene.GetEntityData(id);
        if (!d) return;

        for (auto& lac : d->LookAtConstraints) {
            if (EntityID resolved = resolveGuid(lac.TargetEntityGuidHigh, lac.TargetEntityGuidLow);
                resolved != INVALID_ENTITY_ID) {
                lac.TargetEntity = resolved;
            }
        }
        for (auto& ik : d->IKs) {
            if (EntityID resolvedTarget = resolveGuid(ik.TargetEntityGuidHigh, ik.TargetEntityGuidLow);
                resolvedTarget != INVALID_ENTITY_ID) {
                ik.TargetEntity = resolvedTarget;
            }
            if (EntityID resolvedPole = resolveGuid(ik.PoleEntityGuidHigh, ik.PoleEntityGuidLow);
                resolvedPole != INVALID_ENTITY_ID) {
                ik.PoleEntity = resolvedPole;
            }
        }

        for (EntityID child : d->Children) {
            fixConstraintRefs(child);
        }
    };
    fixConstraintRefs(modelRoot);
    
    // Update transforms
    scene.MarkTransformDirty(modelRoot);
    scene.UpdateTransforms();
    
    std::cout << "[PrefabDelta] Delta application complete\n";
    
    return true;
}

bool ApplyModelDeltaFromJson(
    Scene& scene,
    EntityID modelRoot,
    const nlohmann::json& childrenArray
) {
    if (!childrenArray.is_array()) return false;
    
    ModelDelta delta = DeserializeModelDelta(childrenArray);
    return ApplyModelDelta(scene, modelRoot, delta);
}

} // namespace prefab

