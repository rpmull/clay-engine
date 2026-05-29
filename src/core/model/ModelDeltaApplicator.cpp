#include "ModelDeltaApplicator.h"
#include "core/ecs/Scene.h"
#include "core/ecs/EntityData.h"
#include "core/ecs/AnimationComponents.h"  // TintMaskController, TintTarget, TintBlendMode
#include "core/assets/RuntimeModelInstantiator.h"  // For nested model instantiation
#include <iostream>
#include <algorithm>
#include <queue>
#include <sstream>

namespace cm::model {

ModelDeltaApplicator::ModelDeltaApplicator(Scene& scene)
    : m_Scene(scene)
    , m_RefResolver(DefaultRefResolver)
{
}

EntityID ModelDeltaApplicator::DefaultRefResolver(const StableEntityRef& ref, Scene& scene) {
    // First try GUID lookup
    if (!(ref.targetGuid.high == 0 && ref.targetGuid.low == 0)) {
        EntityID found = scene.FindEntityByGUID(ref.targetGuid);
        if (found != INVALID_ENTITY_ID) {
            return found;
        }
    }
    
    // Then try scene path lookup
    if (!ref.scenePath.empty()) {
        EntityID found = scene.FindEntityByPath(ref.scenePath);
        if (found != INVALID_ENTITY_ID) {
            return found;
        }
    }
    
    return INVALID_ENTITY_ID;
}

void ModelDeltaApplicator::ClearLookupMaps() {
    m_CurrentMaps = ModelLookupMaps{};
}

void ModelDeltaApplicator::BuildMapsRecursive(EntityID entityId, EntityID modelRootId, const std::string& parentPath) {
    auto* data = m_Scene.GetEntityData(entityId);
    if (!data) return;
    
    // Compute path
    std::string path = parentPath.empty() 
        ? (entityId == modelRootId ? "" : data->Name)
        : (parentPath + "/" + data->Name);
    
    // Build stable node ID for this entity
    StableNodeId nodeId;
    nodeId.path = path;
    nodeId.normalizedPath = StableNodeId::ComputeNormalizedPath(path);
    nodeId.nodeName = data->Name;
    nodeId.normalizedName = StableNodeId::ComputeNormalizedName(data->Name);
    nodeId.entityGuid = data->EntityGuid;
    
    // Determine type and compute hashes
    if (data->Mesh) {
        nodeId.type = StableNodeId::NodeType::MeshNode;
        nodeId.meshFileId = data->Mesh->meshReference.fileID;
        int vertHint = data->Mesh->mesh ? static_cast<int>(data->Mesh->mesh->Vertices.size()) : 0;
        nodeId.contentHash = StableNodeId::ComputeContentHash(nodeId.meshFileId, data->Transform.LocalMatrix, vertHint);
    } else if (data->Skeleton) {
        nodeId.type = StableNodeId::NodeType::SkeletonRoot;
    } else {
        nodeId.type = StableNodeId::NodeType::Empty;
    }
    
    // Add to maps
    if (!path.empty()) {
        m_CurrentMaps.byPath[path] = entityId;
    }
    if (!nodeId.normalizedPath.empty()) {
        m_CurrentMaps.byNormalizedPath[nodeId.normalizedPath] = entityId;
    }
    m_CurrentMaps.byNormalizedName[nodeId.normalizedName].push_back(entityId);
    
    if (nodeId.meshFileId >= 0) {
        m_CurrentMaps.byMeshFileId[nodeId.meshFileId].push_back(entityId);
    }
    if (nodeId.contentHash != 0) {
        m_CurrentMaps.byContentHash[nodeId.contentHash].push_back(entityId);
    }
    
    // GUID mapping
    if (!(data->EntityGuid.high == 0 && data->EntityGuid.low == 0)) {
        m_CurrentMaps.guidHigh[data->EntityGuid.high] = data->EntityGuid.low;
        m_CurrentMaps.byGuidLow[data->EntityGuid.low] = entityId;
    }
    
    // Stop at nested models
    bool isNestedModel = entityId != modelRootId && 
                         (data->ModelAssetGuid.high != 0 || data->ModelAssetGuid.low != 0);
    if (isNestedModel) {
        return;
    }
    
    // Recurse into children
    for (EntityID childId : data->Children) {
        BuildMapsRecursive(childId, modelRootId, entityId == modelRootId ? "" : path);
    }
}

void ModelDeltaApplicator::BuildModelLookupMaps(EntityID modelRootId) {
    ClearLookupMaps();
    m_CurrentMaps.rootId = modelRootId;
    
    // Add root to maps with empty path
    auto* rootData = m_Scene.GetEntityData(modelRootId);
    if (rootData) {
        m_CurrentMaps.byPath[""] = modelRootId;
        if (!(rootData->EntityGuid.high == 0 && rootData->EntityGuid.low == 0)) {
            m_CurrentMaps.guidHigh[rootData->EntityGuid.high] = rootData->EntityGuid.low;
            m_CurrentMaps.byGuidLow[rootData->EntityGuid.low] = modelRootId;
        }
    }
    
    BuildMapsRecursive(modelRootId, modelRootId, "");
}

std::pair<EntityID, StableNodeId::MatchConfidence> ModelDeltaApplicator::FindMatchingEntity(
    const StableNodeId& nodeId,
    const ModelLookupMaps& maps) const
{
    // Priority 1: GUID match for user-added entities
    if (nodeId.type == StableNodeId::NodeType::UserAdded && 
        !(nodeId.entityGuid.high == 0 && nodeId.entityGuid.low == 0)) {
        auto highIt = maps.guidHigh.find(nodeId.entityGuid.high);
        if (highIt != maps.guidHigh.end() && highIt->second == nodeId.entityGuid.low) {
            auto lowIt = maps.byGuidLow.find(nodeId.entityGuid.low);
            if (lowIt != maps.byGuidLow.end()) {
                return {lowIt->second, StableNodeId::MatchConfidence::ExactGuid};
            }
        }
    }
    
    // Priority 2: Exact path match
    if (!nodeId.path.empty()) {
        auto it = maps.byPath.find(nodeId.path);
        if (it != maps.byPath.end()) {
            return {it->second, StableNodeId::MatchConfidence::ExactPath};
        }
    }
    
    // Priority 3: Normalized path match
    if (!nodeId.normalizedPath.empty()) {
        auto it = maps.byNormalizedPath.find(nodeId.normalizedPath);
        if (it != maps.byNormalizedPath.end()) {
            return {it->second, StableNodeId::MatchConfidence::NormalizedPath};
        }
    }
    
    // Priority 4: Content hash match
    if (nodeId.contentHash != 0) {
        auto it = maps.byContentHash.find(nodeId.contentHash);
        if (it != maps.byContentHash.end() && !it->second.empty()) {
            // If multiple matches, prefer one with matching name
            for (EntityID id : it->second) {
                auto* data = m_Scene.GetEntityData(id);
                if (data && StableNodeId::ComputeNormalizedName(data->Name) == nodeId.normalizedName) {
                    return {id, StableNodeId::MatchConfidence::ContentHash};
                }
            }
            // Otherwise just use first match
            return {it->second[0], StableNodeId::MatchConfidence::ContentHash};
        }
    }
    
    // Priority 5: Mesh file ID match (with name validation)
    if (nodeId.meshFileId >= 0) {
        auto it = maps.byMeshFileId.find(nodeId.meshFileId);
        if (it != maps.byMeshFileId.end() && !it->second.empty()) {
            // Prefer match with same normalized name
            for (EntityID id : it->second) {
                auto* data = m_Scene.GetEntityData(id);
                if (data && StableNodeId::ComputeNormalizedName(data->Name) == nodeId.normalizedName) {
                    return {id, StableNodeId::MatchConfidence::ContentHash};
                }
            }
        }
    }
    
    // Priority 6: Fuzzy name match (last resort)
    if (!nodeId.normalizedName.empty()) {
        auto it = maps.byNormalizedName.find(nodeId.normalizedName);
        if (it != maps.byNormalizedName.end() && !it->second.empty()) {
            // If multiple entities with same name, try to match by depth
            if (it->second.size() > 1) {
                for (EntityID id : it->second) {
                    std::string entityPath = ComputeEntityPath(maps.rootId, id);
                    int depth = 0;
                    for (char c : entityPath) if (c == '/') depth++;
                    if (!entityPath.empty()) depth++;
                    
                    if (depth == nodeId.depth) {
                        return {id, StableNodeId::MatchConfidence::FuzzyName};
                    }
                }
            }
            return {it->second[0], StableNodeId::MatchConfidence::FuzzyName};
        }
    }
    
    return {INVALID_ENTITY_ID, StableNodeId::MatchConfidence::None};
}

EntityID ModelDeltaApplicator::GetEntityByPath(EntityID modelRootId, const std::string& path) const {
    if (path.empty()) return modelRootId;
    
    EntityID current = modelRootId;
    std::stringstream ss(path);
    std::string segment;
    
    while (std::getline(ss, segment, '/')) {
        auto* data = m_Scene.GetEntityData(current);
        if (!data) return INVALID_ENTITY_ID;
        
        EntityID found = INVALID_ENTITY_ID;
        for (EntityID childId : data->Children) {
            auto* childData = m_Scene.GetEntityData(childId);
            if (childData && childData->Name == segment) {
                found = childId;
                break;
            }
        }
        
        if (found == INVALID_ENTITY_ID) return INVALID_ENTITY_ID;
        current = found;
    }
    
    return current;
}

std::string ModelDeltaApplicator::ComputeEntityPath(EntityID modelRootId, EntityID entityId) const {
    if (entityId == modelRootId) return "";
    
    std::vector<std::string> parts;
    EntityID current = entityId;
    
    while (current != INVALID_ENTITY_ID && current != modelRootId) {
        auto* data = m_Scene.GetEntityData(current);
        if (!data) break;
        parts.push_back(data->Name);
        current = data->Parent;
    }
    
    std::reverse(parts.begin(), parts.end());
    
    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result += "/";
        result += parts[i];
    }
    
    return result;
}

bool ModelDeltaApplicator::IsDescendantOf(EntityID entityId, EntityID ancestorId) const {
    EntityID current = entityId;
    while (current != INVALID_ENTITY_ID) {
        if (current == ancestorId) return true;
        auto* data = m_Scene.GetEntityData(current);
        if (!data) break;
        current = data->Parent;
    }
    return false;
}

EntityID ModelDeltaApplicator::ResolveEntityRef(const StableEntityRef& ref, EntityID modelRootId) {
    // First try cached resolved ID
    if (ref.resolvedId != INVALID_ENTITY_ID) {
        // Verify it's still valid
        if (m_Scene.GetEntityData(ref.resolvedId)) {
            return ref.resolvedId;
        }
        ref.resolvedId = INVALID_ENTITY_ID;  // Invalidate cache
    }
    
    // Try GUID
    if (!(ref.targetGuid.high == 0 && ref.targetGuid.low == 0)) {
        EntityID found = m_Scene.FindEntityByGUID(ref.targetGuid);
        if (found != INVALID_ENTITY_ID) {
            ref.resolvedId = found;
            return found;
        }
    }
    
    // Try model node path (within same model)
    if (!ref.modelNodePath.empty() && (ref.modelGuid.high == 0 && ref.modelGuid.low == 0)) {
        // Target is in same model as reference source
        EntityID found = GetEntityByPath(modelRootId, ref.modelNodePath);
        if (found != INVALID_ENTITY_ID) {
            ref.resolvedId = found;
            return found;
        }
    }
    
    // Try model node path with specific model GUID
    if (!ref.modelNodePath.empty() && !(ref.modelGuid.high == 0 && ref.modelGuid.low == 0)) {
        // Find the model root with this GUID
        for (const auto& entity : m_Scene.GetEntities()) {
            auto* data = m_Scene.GetEntityData(entity.GetID());
            if (data && data->ModelAssetGuid == ref.modelGuid) {
                EntityID found = GetEntityByPath(entity.GetID(), ref.modelNodePath);
                if (found != INVALID_ENTITY_ID) {
                    ref.resolvedId = found;
                    return found;
                }
            }
        }
    }
    
    // Try scene path
    if (!ref.scenePath.empty()) {
        EntityID found = m_Scene.FindEntityByPath(ref.scenePath);
        if (found != INVALID_ENTITY_ID) {
            ref.resolvedId = found;
            return found;
        }
    }
    
    // Fall back to custom resolver
    if (m_RefResolver) {
        ref.resolvedId = m_RefResolver(ref, m_Scene);
        return ref.resolvedId;
    }
    
    return INVALID_ENTITY_ID;
}

void ModelDeltaApplicator::ApplyTransformDelta(EntityData* data, const TransformDelta& delta) {
    if (!data) return;
    
    if (delta.positionModified) {
        data->Transform.Position = delta.position;
    }
    if (delta.rotationModified) {
        data->Transform.RotationQ = delta.rotation;
        // Update Euler angles from quaternion
        data->Transform.Rotation = glm::degrees(glm::eulerAngles(delta.rotation));
    }
    if (delta.scaleModified) {
        data->Transform.Scale = delta.scale;
    }
    
    // Rebuild local matrix
    data->Transform.LocalMatrix = glm::translate(glm::mat4(1.0f), data->Transform.Position) *
                                  glm::mat4_cast(data->Transform.RotationQ) *
                                  glm::scale(glm::mat4(1.0f), data->Transform.Scale);
}

void ModelDeltaApplicator::ApplyMeshDelta(EntityData* data, const MeshDelta& delta) {
    if (!data || !data->Mesh) return;
    
    auto& mesh = *data->Mesh;
    
    // Apply material overrides
    for (const auto& matDelta : delta.materialOverrides) {
        uint32_t slot = matDelta.slotIndex;
        
        // Expand material paths array if needed
        if (mesh.MaterialAssetPaths.size() <= slot) {
            mesh.MaterialAssetPaths.resize(slot + 1);
        }
        mesh.MaterialAssetPaths[slot] = matDelta.materialAssetPath;
        
        // Expand texture paths array if needed  
        if (mesh.SlotPropertyBlockTexturePaths.size() <= slot) {
            mesh.SlotPropertyBlockTexturePaths.resize(slot + 1);
        }
        mesh.SlotPropertyBlockTexturePaths[slot] = matDelta.texturePaths;
        
        // Expand property blocks array if needed
        if (mesh.SlotPropertyBlocks.size() <= slot) {
            mesh.SlotPropertyBlocks.resize(slot + 1);
        }
        mesh.SlotPropertyBlocks[slot] = matDelta.propertyBlock;
    }
    
    mesh.ShowBackfaces = delta.showBackfaces;
    mesh.RenderOnTop = delta.renderOnTop;
    mesh.RenderOrder = delta.renderOrder;
    mesh.BoundsPadding = delta.boundsPadding;
    mesh.UniqueMaterial = delta.uniqueMaterial;
}

void ModelDeltaApplicator::ApplyRenderOverridesDelta(EntityData* data, const RenderOverridesDelta& delta) {
    if (!data) return;
    
    if (!data->RenderOverrides) {
        data->RenderOverrides = std::make_unique<RenderOverridesComponent>();
    }
    
    auto& ro = *data->RenderOverrides;
    ro.AlphaBlendEnabled = delta.alphaBlendEnabled;
    ro.UseAlphaCutout = delta.useAlphaCutout;
    ro.AlphaCutoutThreshold = delta.alphaCutoutThreshold;
    ro.DepthWriteEnabled = delta.depthWriteEnabled;
    ro.CastShadows = delta.castShadows;
    ro.ReceiveShadows = delta.receiveShadows;
    ro.Visible = delta.visible;
    ro.SortingOrder = delta.sortingOrder;
}

void ModelDeltaApplicator::ApplyBlendShapeDelta(EntityData* data, const BlendShapeDelta& delta) {
    if (!data || !data->BlendShapes) return;
    
    for (auto& shape : data->BlendShapes->Shapes) {
        auto it = delta.weights.find(shape.Name);
        if (it != delta.weights.end()) {
            shape.Weight = it->second;
        }
    }
    
    // Also store pending weights for deferred application
    for (const auto& [name, weight] : delta.weights) {
        data->PendingBlendShapeWeights[name] = weight;
    }
}

void ModelDeltaApplicator::ApplyBoneAttachmentDelta(EntityData* data, const BoneAttachmentDelta& delta, EntityID modelRootId) {
    if (!data) return;
    
    if (!data->BoneAttachment) {
        data->BoneAttachment = std::make_unique<BoneAttachmentComponent>();
    }
    
    auto& ba = *data->BoneAttachment;
    ba.TargetBoneName = delta.targetBoneName;
    ba.LocalPosition = delta.localPosition;
    ba.LocalRotation = delta.localRotation;
    ba.LocalScale = delta.localScale;
    ba.InheritRotation = delta.inheritRotation;
    ba.InheritScale = delta.inheritScale;
    ba.Enabled = delta.enabled;
    
    // Resolve skeleton reference
    if (delta.skeletonRef.IsValid()) {
        ba.SkeletonEntity = ResolveEntityRef(delta.skeletonRef, modelRootId);
    }
}

void ModelDeltaApplicator::ApplyLookAtConstraints(EntityData* data, const std::vector<LookAtConstraintDelta>& deltas, EntityID modelRootId) {
    if (!data) return;
    
    // Clear existing and rebuild
    data->LookAtConstraints.clear();
    
    for (const auto& delta : deltas) {
        cm::animation::lookat::LookAtConstraintComponent lac;
        lac.Enabled = delta.enabled;
        lac.Weight = delta.weight;
        lac.Mode = static_cast<cm::animation::lookat::LookAtMode>(delta.mode);
        lac.Axes = static_cast<cm::animation::lookat::AxisMask>(delta.axes);
        lac.Space = static_cast<cm::animation::lookat::LookAtSpace>(delta.space);
        lac.SmoothingSpeed = delta.smoothingSpeed;
        lac.MaxYawDeg = delta.maxYawDeg;
        lac.MaxPitchDeg = delta.maxPitchDeg;
        
        // Resolve target
        if (delta.target.IsValid()) {
            lac.TargetEntity = ResolveEntityRef(delta.target, modelRootId);
            // Store GUID for re-resolution
            lac.TargetEntityGuidHigh = delta.target.targetGuid.high;
            lac.TargetEntityGuidLow = delta.target.targetGuid.low;
        }
        
        // Convert bone chain
        for (int32_t boneId : delta.boneChain) {
            lac.BoneChain.push_back(static_cast<uint32_t>(boneId));
        }
        
        data->LookAtConstraints.push_back(std::move(lac));
    }
}

void ModelDeltaApplicator::ApplyIKConstraints(EntityData* data, const std::vector<IKConstraintDelta>& deltas, EntityID modelRootId) {
    if (!data) return;
    
    // Clear existing and rebuild
    data->IKs.clear();
    
    for (const auto& delta : deltas) {
        cm::animation::ik::IKComponent ik;
        ik.Enabled = delta.enabled;
        ik.Weight = delta.weight;
        ik.UseTwoBone = delta.useTwoBone;
        ik.MaxIterations = delta.maxIterations;
        ik.Tolerance = delta.tolerance;
        ik.Damping = delta.damping;
        ik.LockAxisX = delta.lockAxisX;
        ik.LockAxisY = delta.lockAxisY;
        ik.LockAxisZ = delta.lockAxisZ;
        ik.ChainRootHint = delta.chainRootHint;
        ik.ChainEffectorHint = delta.chainEffectorHint;
        
        // Resolve target
        if (delta.target.IsValid()) {
            ik.TargetEntity = ResolveEntityRef(delta.target, modelRootId);
        }
        
        // Resolve pole
        if (delta.pole.IsValid()) {
            ik.PoleEntity = ResolveEntityRef(delta.pole, modelRootId);
        }
        
        // Convert chain - BoneId is int32_t
        for (int32_t boneId : delta.chain) {
            ik.Chain.push_back(boneId);
        }
        
        data->IKs.push_back(std::move(ik));
    }
}

void ModelDeltaApplicator::ApplyScripts(EntityData* data, const std::vector<ScriptDelta>& deltas) {
    if (!data) return;

    auto parseGuidString = [](const nlohmann::json& node) -> ClaymoreGUID {
        if (!node.is_string()) return {};
        try {
            return ClaymoreGUID::FromString(node.get<std::string>());
        } catch (...) {
            return {};
        }
    };

    auto resolveEntityRefValue = [&](const nlohmann::json& node, ScriptEntityRefMetadata* outMeta) -> int {
        int fallback = node.value("value", -1);
        StableEntityRef ref;
        if (node.contains("guid")) ref.targetGuid = parseGuidString(node["guid"]);
        if (node.contains("modelGuid")) ref.modelGuid = parseGuidString(node["modelGuid"]);
        if (node.contains("modelNodePath") && node["modelNodePath"].is_string()) {
            ref.modelNodePath = node["modelNodePath"].get<std::string>();
        }
        if (node.contains("scenePath") && node["scenePath"].is_string()) {
            ref.scenePath = node["scenePath"].get<std::string>();
        }

        int resolvedValue = fallback;
        if (ref.IsValid()) {
            EntityID resolved = ResolveEntityRef(ref, m_CurrentMaps.rootId);
            if (resolved != INVALID_ENTITY_ID) {
                resolvedValue = static_cast<int>(resolved);
            }
        }

        if (outMeta) {
            outMeta->entityId = resolvedValue;
            outMeta->guid = ref.targetGuid;
            outMeta->modelGuid = ref.modelGuid;
            outMeta->modelNodePath = ref.modelNodePath;
            outMeta->unresolved = ref.IsValid() &&
                                  (resolvedValue <= 0 || static_cast<EntityID>(resolvedValue) == INVALID_ENTITY_ID);
        }
        return resolvedValue;
    };

    auto applyScriptProperty = [&](ScriptInstance& script, const std::string& key, const nlohmann::json& value) {
        if (value.is_object() && value.value("__entityRef", false)) {
            ScriptEntityRefMetadata meta;
            int resolved = resolveEntityRefValue(value, &meta);
            script.Values[key] = resolved;
            script.EntityRefMetadata[key] = meta;
            return;
        }
        if (value.is_object() && value.value("__entityRefList", false) &&
            value.contains("items") && value["items"].is_array()) {
            auto listPtr = std::make_shared<ListPropertyValue>();
            listPtr->elementType = PropertyType::Entity;
            listPtr->entityRefs.reserve(value["items"].size());
            listPtr->elements.reserve(value["items"].size());
            for (const auto& item : value["items"]) {
                ScriptEntityRefMetadata elemMeta{};
                int resolved = item.is_object() ? resolveEntityRefValue(item, &elemMeta)
                                                : (item.is_number_integer() ? item.get<int>() : -1);
                listPtr->elements.emplace_back(resolved);
                listPtr->entityRefs.push_back(elemMeta);
            }
            script.Values[key] = std::move(listPtr);
            return;
        }
        // Backward-compat with older raw scalar encoding.
        if (value.is_number_integer()) {
            script.Values[key] = value.get<int>();
        } else if (value.is_number_float()) {
            script.Values[key] = value.get<float>();
        } else if (value.is_boolean()) {
            script.Values[key] = value.get<bool>();
        } else if (value.is_string()) {
            script.Values[key] = value.get<std::string>();
        } else if (value.is_array() && value.size() == 3) {
            script.Values[key] = glm::vec3(value[0], value[1], value[2]);
        }
    };

    for (const auto& delta : deltas) {
        // Check if script already exists
        bool found = false;
        for (auto& script : data->Scripts) {
            if (script.ClassName == delta.className) {
                // Update existing script's properties
                for (const auto& [key, value] : delta.properties) {
                    applyScriptProperty(script, key, value);
                }
                found = true;
                break;
            }
        }
        
        if (!found) {
            // Add new script
            ScriptInstance script;
            script.ClassName = delta.className;
            for (const auto& [key, value] : delta.properties) {
                applyScriptProperty(script, key, value);
            }
            data->Scripts.push_back(std::move(script));
        }
    }
}

void ModelDeltaApplicator::ApplyAddedLight(EntityData* data, const AddedLightDelta& delta) {
    if (!data) return;
    
    if (!data->Light) {
        data->Light = std::make_unique<LightComponent>();
    }
    
    data->Light->Type = static_cast<LightType>(delta.type);
    data->Light->Color = delta.color;
    data->Light->Intensity = delta.intensity;
}

void ModelDeltaApplicator::ApplyAddedCollider(EntityData* data, const AddedColliderDelta& delta) {
    if (!data) return;
    
    if (!data->Collider) {
        data->Collider = std::make_unique<ColliderComponent>();
    }
    
    data->Collider->ShapeType = static_cast<ColliderShape>(delta.shapeType);
    data->Collider->Offset = delta.offset;
    data->Collider->Size = delta.size;
    data->Collider->Radius = delta.radius;
    data->Collider->Height = delta.height;
    data->Collider->IsTrigger = delta.isTrigger;
    data->Collider->PhysicsLayerName = delta.physicsLayerName;
}

void ModelDeltaApplicator::ApplyAddedCamera(EntityData* data, const AddedCameraDelta& delta) {
    if (!data) return;
    
    if (!data->Camera) {
        data->Camera = std::make_unique<CameraComponent>();
    }
    
    data->Camera->FieldOfView = delta.fieldOfView;
    data->Camera->NearClip = delta.nearClip;
    data->Camera->FarClip = delta.farClip;
    data->Camera->IsPerspective = delta.isPerspective;
    data->Camera->Active = delta.active;
    data->Camera->priority = delta.priority;
}

void ModelDeltaApplicator::ApplyAddedEmitter(EntityData* data, const AddedEmitterDelta& delta) {
    if (!data) return;
    
    if (!data->Emitter) {
        data->Emitter = std::make_unique<ParticleEmitterComponent>();
    }
    
    data->Emitter->SpritePath = delta.spritePath;
    data->Emitter->MaxParticles = delta.maxParticles;
    data->Emitter->EmissionRate = delta.emissionRate;
    data->Emitter->Enabled = delta.enabled;
    
    // Apply full data if available
    if (!delta.fullData.empty()) {
        if (delta.fullData.contains("duration")) {
            data->Emitter->Duration = delta.fullData["duration"].get<float>();
        }
        if (delta.fullData.contains("looping")) {
            data->Emitter->Looping = delta.fullData["looping"].get<bool>();
        }
        if (delta.fullData.contains("shape")) {
            data->Emitter->Shape = static_cast<ParticleEmissionShape>(delta.fullData["shape"].get<int>());
        }
        if (delta.fullData.contains("gravityModifier")) {
            data->Emitter->GravityModifier = delta.fullData["gravityModifier"].get<float>();
        }
    }
}

void ModelDeltaApplicator::ApplyTintControllerDelta(EntityData* data, const nlohmann::json& tintJson, EntityID targetEntityId, EntityID modelRootId) {
    if (!data) return;
    
    // Create TintController if it doesn't exist
    if (!data->TintController) {
        data->TintController = std::make_unique<TintMaskController>();
    }
    
    auto& tc = *data->TintController;
    
    // Read name pattern
    tc.NamePattern = tintJson.value("namePattern", std::string());
    
    // Read global settings
    tc.GlobalBlendMode = static_cast<TintBlendMode>(tintJson.value("globalBlendMode", 0));
    tc.AutoIncludeParentedSkinnedMeshes = tintJson.value("autoIncludeParentedSkinnedMeshes", true);
    tc.UseTintMask = tintJson.value("useTintMask", false);
    tc.UsePbrOverrides = tintJson.value("usePbrOverrides", false);
    tc.OverrideMetallic = tintJson.value("pbrMetallic", tc.OverrideMetallic);
    tc.OverrideRoughness = tintJson.value("pbrRoughness", tc.OverrideRoughness);
    tc.OverrideEmissionStrength = tintJson.value("pbrEmissionStrength", tc.OverrideEmissionStrength);
    if (tintJson.contains("pbrEmissionColor") && tintJson["pbrEmissionColor"].is_array() && tintJson["pbrEmissionColor"].size() >= 3) {
        tc.OverrideEmissionColor = glm::vec3(tintJson["pbrEmissionColor"][0], tintJson["pbrEmissionColor"][1], tintJson["pbrEmissionColor"][2]);
    }
    
    // Read tint colors
    if (tintJson.contains("baseTint") && tintJson["baseTint"].is_array() && tintJson["baseTint"].size() >= 4) {
        tc.BaseTint = glm::vec4(tintJson["baseTint"][0], tintJson["baseTint"][1], 
                                 tintJson["baseTint"][2], tintJson["baseTint"][3]);
    }
    if (tintJson.contains("tintColor0") && tintJson["tintColor0"].is_array() && tintJson["tintColor0"].size() >= 4) {
        tc.TintColor0 = glm::vec4(tintJson["tintColor0"][0], tintJson["tintColor0"][1],
                                   tintJson["tintColor0"][2], tintJson["tintColor0"][3]);
    }
    if (tintJson.contains("tintColor1") && tintJson["tintColor1"].is_array() && tintJson["tintColor1"].size() >= 4) {
        tc.TintColor1 = glm::vec4(tintJson["tintColor1"][0], tintJson["tintColor1"][1],
                                   tintJson["tintColor1"][2], tintJson["tintColor1"][3]);
    }
    if (tintJson.contains("tintColor2") && tintJson["tintColor2"].is_array() && tintJson["tintColor2"].size() >= 4) {
        tc.TintColor2 = glm::vec4(tintJson["tintColor2"][0], tintJson["tintColor2"][1],
                                   tintJson["tintColor2"][2], tintJson["tintColor2"][3]);
    }
    if (tintJson.contains("tintColor3") && tintJson["tintColor3"].is_array() && tintJson["tintColor3"].size() >= 4) {
        tc.TintColor3 = glm::vec4(tintJson["tintColor3"][0], tintJson["tintColor3"][1],
                                   tintJson["tintColor3"][2], tintJson["tintColor3"][3]);
    }
    
    // Read explicit targets with entity path resolution
    tc.Targets.clear();
    if (tintJson.contains("targets") && tintJson["targets"].is_array()) {
        for (const auto& targetJ : tintJson["targets"]) {
            TintTarget target;
            target.MaterialSlot = targetJ.value("slot", -1);
            target.BlendMode = static_cast<TintBlendMode>(targetJ.value("blendMode", 0));
            target.UseTargetColor = targetJ.value("useColor", false);
            
            if (targetJ.contains("color") && targetJ["color"].is_array() && targetJ["color"].size() >= 4) {
                target.Color = glm::vec4(targetJ["color"][0], targetJ["color"][1],
                                         targetJ["color"][2], targetJ["color"][3]);
            }
            
            // Resolve target entity by path within model hierarchy
            target.TargetEntity = INVALID_ENTITY_ID;
            if (targetJ.contains("entityPath")) {
                std::string entityPath = targetJ["entityPath"].get<std::string>();
                if (entityPath.empty()) {
                    // Empty path = this entity (where TintController is applied)
                    target.TargetEntity = targetEntityId;
                } else {
                    // Try exact path first
                    auto itExact = m_CurrentMaps.byPath.find(entityPath);
                    if (itExact != m_CurrentMaps.byPath.end()) {
                        target.TargetEntity = itExact->second;
                    } else {
                        // Try normalized path
                        std::string normalizedPath = StableNodeId::ComputeNormalizedPath(entityPath);
                        auto itNorm = m_CurrentMaps.byNormalizedPath.find(normalizedPath);
                        if (itNorm != m_CurrentMaps.byNormalizedPath.end()) {
                            target.TargetEntity = itNorm->second;
                        }
                    }
                }
            } else if (targetJ.contains("entity")) {
                // Legacy fallback: use entity ID directly (won't work across instantiations)
                target.TargetEntity = static_cast<EntityID>(targetJ.value("entity", -1));
            }
            
            tc.Targets.push_back(target);
        }
    }
    
    // Force refresh of tint matching
    tc.NeedsRefresh = true;
    tc.TintDirty = true;
    
    if (!tc.Targets.empty()) {
        std::cout << "[DeltaApplicator] Applied TintController with " << tc.Targets.size() 
                  << " targets to entity '" << data->Name << "'" << std::endl;
    }
}

bool ModelDeltaApplicator::ApplyNodeDelta(
    EntityID targetEntity,
    const NodeDelta& delta,
    const DeltaApplicationConfig& config,
    DeltaApplicationResult& result)
{
    auto* data = m_Scene.GetEntityData(targetEntity);
    if (!data) {
        result.warnings.push_back("Target entity " + std::to_string(targetEntity) + " not found");
        return false;
    }
    
    // Apply entity-level overrides
    if (config.applyEntityOverrides) {
        if (!delta.name.empty()) {
            m_Scene.SetEntityName(targetEntity, delta.name);
        }
        if (delta.layer.has_value()) {
            m_Scene.SetEntityLayer(targetEntity, *delta.layer);
        }
        if (delta.tag.has_value()) {
            m_Scene.SetEntityTag(targetEntity, *delta.tag);
        }
        if (delta.visible.has_value()) {
            m_Scene.SetEntityVisibleDirect(targetEntity, *delta.visible);
        }
        if (delta.active.has_value()) {
            m_Scene.SetEntityActive(targetEntity, *delta.active);
        }
    }
    
    // Apply transform
    if (config.applyTransforms && delta.transform.has_value()) {
        ApplyTransformDelta(data, *delta.transform);
        m_Scene.MarkTransformDirty(targetEntity);
    }
    
    // Apply mesh overrides
    if (config.applyMeshOverrides) {
        if (delta.mesh.has_value()) {
            ApplyMeshDelta(data, *delta.mesh);
        }
        if (delta.renderOverrides.has_value()) {
            ApplyRenderOverridesDelta(data, *delta.renderOverrides);
        }
    }
    
    // Apply blend shapes
    if (config.applyBlendShapes && delta.blendShapes.has_value()) {
        ApplyBlendShapeDelta(data, *delta.blendShapes);
    }
    
    // Apply bone attachment
    if (config.applyAddedComponents && delta.boneAttachment.has_value()) {
        ApplyBoneAttachmentDelta(data, *delta.boneAttachment, m_CurrentMaps.rootId);
    }
    
    // Apply constraints
    if (config.applyConstraints) {
        if (!delta.lookAtConstraints.empty()) {
            ApplyLookAtConstraints(data, delta.lookAtConstraints, m_CurrentMaps.rootId);
        }
        if (!delta.ikConstraints.empty()) {
            ApplyIKConstraints(data, delta.ikConstraints, m_CurrentMaps.rootId);
        }
    }
    
    // Apply scripts
    if (config.applyScripts && !delta.scripts.empty()) {
        ApplyScripts(data, delta.scripts);
    }
    
    // Apply added components
    if (config.applyAddedComponents) {
        if (delta.addedLight.has_value()) {
            ApplyAddedLight(data, *delta.addedLight);
        }
        if (delta.addedCollider.has_value()) {
            ApplyAddedCollider(data, *delta.addedCollider);
        }
        if (delta.addedCamera.has_value()) {
            ApplyAddedCamera(data, *delta.addedCamera);
        }
        if (delta.addedEmitter.has_value()) {
            ApplyAddedEmitter(data, *delta.addedEmitter);
        }
        // Apply TintController component (includes entity path resolution for targets)
        if (delta.tintController.has_value()) {
            ApplyTintControllerDelta(data, *delta.tintController, targetEntity, m_CurrentMaps.rootId);
        }
    }
    
    // Restore extra data
    if (!delta.extra.empty()) {
        data->Extra = delta.extra;
    }
    
    return true;
}

void ModelDeltaApplicator::ApplyDeletedNodes(
    EntityID modelRootId,
    const std::vector<std::string>& deletedPaths,
    DeltaApplicationResult& result)
{
    auto* rootData = m_Scene.GetEntityData(modelRootId);
    if (!rootData) return;
    
    // Store sanitized paths in root entity for later reference
    rootData->DeletedModelNodes.clear();
    rootData->DeletedModelNodes.reserve(deletedPaths.size());
    for (const auto& path : deletedPaths) {
        if (!path.empty()) {
            rootData->DeletedModelNodes.push_back(path);
        }
    }
    
    // Mark entities as invisible
    for (const auto& path : deletedPaths) {
        if (path.empty()) {
            continue;
        }
        EntityID entityId = GetEntityByPath(modelRootId, path);
        if (entityId != INVALID_ENTITY_ID) {
            auto* data = m_Scene.GetEntityData(entityId);
            if (data) {
                m_Scene.SetEntityVisibleDirect(entityId, false);
                result.deletedNodesCount++;
            }
        } else {
            result.warnings.push_back("Deleted node path not found: " + path);
        }
    }
}

void ModelDeltaApplicator::ApplyAddedChildren(
    EntityID modelRootId,
    const std::vector<AddedChildDelta>& addedChildren,
    const DeltaApplicationConfig& config,
    DeltaApplicationResult& result)
{
    for (const auto& addedChild : addedChildren) {
        // Find parent entity
        auto [parentId, _] = FindMatchingEntity(addedChild.parentNodeId, m_CurrentMaps);
        if (parentId == INVALID_ENTITY_ID) {
            // Try exact path fallback
            parentId = GetEntityByPath(modelRootId, addedChild.parentNodeId.path);
        }
        
        if (parentId == INVALID_ENTITY_ID) {
            result.warnings.push_back("Could not find parent for added child: " + addedChild.childId.nodeName);
            continue;
        }
        
        if (addedChild.isNestedModel && addedChild.nestedModelAsset.has_value()) {
            // This is a nested model (e.g., armor, weapon attachment) - instantiate it using RuntimeModelInstantiator
            const ClaymoreGUID& modelGuid = addedChild.nestedModelAsset->guid;
            
            if (config.verbose) {
                std::cout << "[DeltaApplicator] Instantiating nested model: " 
                          << addedChild.childId.nodeName << " GUID=" << modelGuid.ToString() << std::endl;
            }
            
            // Instantiate the model hierarchy at origin (we'll parent and position it below)
            EntityID nestedRootId = cm::RuntimeModelInstantiator::InstantiateByGuid(modelGuid, m_Scene, glm::vec3(0.0f));
            
            if (nestedRootId != INVALID_ENTITY_ID) {
                EntityData* nestedData = m_Scene.GetEntityData(nestedRootId);
                if (nestedData) {
                    // Apply name from delta if available
                    const auto& ed = addedChild.entityData;
                    if (ed.contains("name")) {
                        nestedData->Name = ed["name"].get<std::string>();
                    }
                    
                    // Apply transform if available
                    if (ed.contains("transform")) {
                        const auto& t = ed["transform"];
                        if (t.contains("position") && t["position"].is_array() && t["position"].size() >= 3) {
                            nestedData->Transform.Position = glm::vec3(t["position"][0], t["position"][1], t["position"][2]);
                        }
                        if (t.contains("rotation") && t["rotation"].is_array() && t["rotation"].size() >= 4) {
                            nestedData->Transform.RotationQ = glm::quat(t["rotation"][0], t["rotation"][1], t["rotation"][2], t["rotation"][3]);
                            nestedData->Transform.Rotation = glm::degrees(glm::eulerAngles(nestedData->Transform.RotationQ));
                        }
                        if (t.contains("scale") && t["scale"].is_array() && t["scale"].size() >= 3) {
                            nestedData->Transform.Scale = glm::vec3(t["scale"][0], t["scale"][1], t["scale"][2]);
                        }
                        m_Scene.MarkTransformDirty(nestedRootId);
                    }
                    
                // Parent to the correct parent entity
                if (config.fastHierarchy) {
                    m_Scene.SetParentFast(nestedRootId, parentId);
                } else {
                    m_Scene.SetParent(nestedRootId, parentId);
                }
                    
                    result.addedChildrenCount++;
                    
                    if (config.verbose) {
                        std::cout << "[DeltaApplicator] Nested model instantiated successfully: " 
                                  << nestedData->Name << " (ID=" << nestedRootId << ")" << std::endl;
                    }
                }
            } else {
                result.warnings.push_back("Failed to instantiate nested model: " + addedChild.childId.nodeName + 
                                         " (GUID=" + modelGuid.ToString() + ")");
            }
        } else {
            // Create regular entity
            const auto& ed = addedChild.entityData;
            std::string newName = ed.value("name", addedChild.childId.nodeName);
            Entity newEntity = config.fastHierarchy
                ? m_Scene.CreateEntityExactFast(newName.empty() ? "Entity" : newName)
                : m_Scene.CreateEntity(newName.empty() ? "Entity" : newName);
            EntityID newEntityId = newEntity.GetID();
            auto* newData = m_Scene.GetEntityData(newEntityId);
            if (!newData) continue;
            
            // Apply entity data from JSON
            newData->Name = newName;
            newData->Layer = ed.value("layer", 0);
            newData->Tag = ed.value("tag", std::string());
            newData->Visible = ed.value("visible", true);
            newData->Active = ed.value("active", true);
            
            // Apply GUID if available
            if (ed.contains("guid") && ed["guid"].is_string()) {
                newData->EntityGuid = ClaymoreGUID::FromString(ed["guid"].get<std::string>());
            }
            
            // Apply transform
            if (ed.contains("transform")) {
                const auto& t = ed["transform"];
                if (t.contains("position") && t["position"].is_array() && t["position"].size() >= 3) {
                    newData->Transform.Position = glm::vec3(t["position"][0], t["position"][1], t["position"][2]);
                }
                if (t.contains("rotation") && t["rotation"].is_array() && t["rotation"].size() >= 4) {
                    newData->Transform.RotationQ = glm::quat(t["rotation"][0], t["rotation"][1], t["rotation"][2], t["rotation"][3]);
                    newData->Transform.Rotation = glm::degrees(glm::eulerAngles(newData->Transform.RotationQ));
                }
                if (t.contains("scale") && t["scale"].is_array() && t["scale"].size() >= 3) {
                    newData->Transform.Scale = glm::vec3(t["scale"][0], t["scale"][1], t["scale"][2]);
                }
                
                // Rebuild local matrix
                newData->Transform.LocalMatrix = glm::translate(glm::mat4(1.0f), newData->Transform.Position) *
                                                 glm::mat4_cast(newData->Transform.RotationQ) *
                                                 glm::scale(glm::mat4(1.0f), newData->Transform.Scale);
            }
            
            // Set parent
            if (config.fastHierarchy) {
                m_Scene.SetParentFast(newEntityId, parentId);
            } else {
                m_Scene.SetParent(newEntityId, parentId);
            }
            
            result.addedChildrenCount++;
            
            if (config.verbose) {
                std::cout << "[DeltaApplicator] Restored added child: " << newData->Name << std::endl;
            }
        }
    }
    
    if (config.fastHierarchy && result.addedChildrenCount > 0) {
        m_Scene.InvalidateHierarchyCache();
    }
}

DeltaApplicationResult ModelDeltaApplicator::Apply(
    EntityID modelRootId,
    const ModelDelta& delta,
    const DeltaApplicationConfig& config)
{
    DeltaApplicationResult result;
    
    auto* rootData = m_Scene.GetEntityData(modelRootId);
    if (!rootData) {
        result.errors.push_back("Model root entity not found: " + std::to_string(modelRootId));
        return result;
    }
    
    if (config.verbose) {
        std::cout << "[DeltaApplicator] Applying delta to model root " << modelRootId 
                  << " (" << rootData->Name << ") with "
                  << delta.nodeDeltas.size() << " node deltas, "
                  << delta.addedChildren.size() << " added children, "
                  << delta.deletedNodePaths.size() << " deleted nodes" << std::endl;
    }
    
    // Build lookup maps for the model
    BuildModelLookupMaps(modelRootId);
    
    // Apply root delta first
    if (delta.rootDelta.has_value()) {
        if (ApplyNodeDelta(modelRootId, *delta.rootDelta, config, result)) {
            result.appliedCount++;
        }
    }
    
    // Apply node deltas
    for (const auto& nodeDelta : delta.nodeDeltas) {
        // Find matching entity
        auto [targetId, confidence] = FindMatchingEntity(nodeDelta.nodeId, m_CurrentMaps);
        
        DeltaMatchResult matchResult;
        matchResult.delta = &nodeDelta;
        matchResult.matchedEntity = targetId;
        matchResult.confidence = confidence;
        
        if (targetId == INVALID_ENTITY_ID) {
            matchResult.warning = "Could not find matching entity for node: " + nodeDelta.nodeId.path;
            result.nodeResults.push_back(matchResult);
            result.unmatchedCount++;
            
            if (!config.allowUnmatchedDeltas) {
                result.errors.push_back(matchResult.warning);
            } else {
                result.warnings.push_back(matchResult.warning);
            }
            continue;
        }
        
        if (confidence < config.minMatchConfidence) {
            matchResult.warning = "Match confidence too low for node: " + nodeDelta.nodeId.path + 
                                 " (got " + std::to_string(static_cast<int>(confidence)) + ")";
            result.nodeResults.push_back(matchResult);
            result.unmatchedCount++;
            
            if (!config.allowUnmatchedDeltas) {
                result.errors.push_back(matchResult.warning);
            } else {
                result.warnings.push_back(matchResult.warning);
            }
            continue;
        }
        
        // Warn about fuzzy matches
        if (confidence < StableNodeId::MatchConfidence::NormalizedPath) {
            matchResult.warning = "Low-confidence match for " + nodeDelta.nodeId.path + 
                                 " -> " + ComputeEntityPath(modelRootId, targetId);
            result.warnings.push_back(matchResult.warning);
        }
        
        // Apply the delta
        if (ApplyNodeDelta(targetId, nodeDelta, config, result)) {
            result.appliedCount++;
        }
        
        result.nodeResults.push_back(matchResult);
    }
    
    // Apply deleted nodes
    if (config.applyDeletedNodes && !delta.deletedNodePaths.empty()) {
        ApplyDeletedNodes(modelRootId, delta.deletedNodePaths, result);
    }
    
    // Apply added children
    if (config.applyAddedChildren && !delta.addedChildren.empty()) {
        ApplyAddedChildren(modelRootId, delta.addedChildren, config, result);
    }
    
    ClearLookupMaps();
    
    if (config.verbose) {
        std::cout << "[DeltaApplicator] Application complete: "
                  << result.appliedCount << " applied, "
                  << result.unmatchedCount << " unmatched, "
                  << result.warnings.size() << " warnings, "
                  << result.errors.size() << " errors" << std::endl;
    }
    
    return result;
}

// =============================================================================
// Validation Function (standalone)
// =============================================================================

std::vector<std::string> ValidateModelDelta(
    const ModelDelta& delta,
    EntityID modelRootId,
    Scene& scene)
{
    std::vector<std::string> warnings;
    
    auto* rootData = scene.GetEntityData(modelRootId);
    if (!rootData) {
        warnings.push_back("Model root entity not found: " + std::to_string(modelRootId));
        return warnings;
    }
    
    // Check if model asset GUID matches
    if (!(delta.modelAssetGuid.high == 0 && delta.modelAssetGuid.low == 0) &&
        !(rootData->ModelAssetGuid.high == 0 && rootData->ModelAssetGuid.low == 0)) {
        if (delta.modelAssetGuid.high != rootData->ModelAssetGuid.high ||
            delta.modelAssetGuid.low != rootData->ModelAssetGuid.low) {
            warnings.push_back("Model asset GUID mismatch: delta=" + delta.modelAssetGuid.ToString() +
                             ", model=" + rootData->ModelAssetGuid.ToString());
        }
    }
    
    // Check version staleness
    if (delta.IsPotentiallyStale(rootData->ModelAssetGuid.low & 0xFFFF)) {  // Use low bits as version proxy
        warnings.push_back("Delta may be stale (model structure version changed)");
    }
    
    // Build lookup maps for the model
    ModelDeltaApplicator applicator(scene);
    applicator.BuildModelLookupMaps(modelRootId);
    
    // Validate each node delta can be matched
    int unmatchedCount = 0;
    int lowConfidenceCount = 0;
    
    for (const auto& nodeDelta : delta.nodeDeltas) {
        EntityID targetId = scene.FindEntityByPath(nodeDelta.nodeId.path);
        
        if (targetId == INVALID_ENTITY_ID) {
            // Try normalized path
            // For now, just count as unmatched
            unmatchedCount++;
            warnings.push_back("Node delta path not found: " + nodeDelta.nodeId.path);
        } else {
            // Check match confidence
            auto* targetData = scene.GetEntityData(targetId);
            if (targetData) {
                std::string normalizedName = StableNodeId::ComputeNormalizedName(targetData->Name);
                if (normalizedName != nodeDelta.nodeId.normalizedName) {
                    lowConfidenceCount++;
                    warnings.push_back("Node name mismatch at " + nodeDelta.nodeId.path + 
                                     ": expected " + nodeDelta.nodeId.normalizedName +
                                     ", found " + normalizedName);
                }
            }
        }
    }
    
    // Validate added children parent paths
    for (const auto& addedChild : delta.addedChildren) {
        EntityID parentId = scene.FindEntityByPath(addedChild.parentNodeId.path);
        if (parentId == INVALID_ENTITY_ID) {
            warnings.push_back("Added child parent path not found: " + addedChild.parentNodeId.path + 
                             " (child: " + addedChild.childId.nodeName + ")");
        }
    }
    
    // Validate deleted node paths
    for (const auto& deletedPath : delta.deletedNodePaths) {
        EntityID nodeId = scene.FindEntityByPath(deletedPath);
        if (nodeId == INVALID_ENTITY_ID) {
            // Not necessarily an error - node may already be gone
            // But worth noting
            warnings.push_back("Deleted node path not found (may be expected): " + deletedPath);
        }
    }
    
    applicator.ClearLookupMaps();
    
    // Summary warnings
    if (unmatchedCount > 0) {
        std::ostringstream ss;
        ss << "Total unmatched node deltas: " << unmatchedCount << " of " << delta.nodeDeltas.size();
        warnings.push_back(ss.str());
    }
    
    if (lowConfidenceCount > 0) {
        std::ostringstream ss;
        ss << "Total low-confidence matches: " << lowConfidenceCount;
        warnings.push_back(ss.str());
    }
    
    return warnings;
}

} // namespace cm::model

