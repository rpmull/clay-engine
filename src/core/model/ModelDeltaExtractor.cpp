#include "ModelDeltaExtractor.h"
#include "core/ecs/Scene.h"
#include "core/ecs/EntityData.h"
#include <sstream>
#include <iostream>
#include <chrono>

namespace cm::model {

ModelDeltaExtractor::ModelDeltaExtractor(Scene& scene)
    : m_Scene(scene)
{
}

bool ModelDeltaExtractor::IsModelRoot(const EntityData* data) {
    if (!data) return false;
    return data->ModelAssetGuid.high != 0 || data->ModelAssetGuid.low != 0;
}

std::vector<EntityID> ModelDeltaExtractor::GetModelRoots() const {
    std::vector<EntityID> roots;
    for (const auto& entity : m_Scene.GetEntities()) {
        auto* data = m_Scene.GetEntityData(entity.GetID());
        if (IsModelRoot(data)) {
            roots.push_back(entity.GetID());
        }
    }
    return roots;
}

std::string ModelDeltaExtractor::ComputeRelativePath(EntityID modelRootId, EntityID entityId) {
    if (entityId == modelRootId) {
        return "";  // Model root has empty relative path
    }
    
    std::vector<std::string> parts;
    EntityID current = entityId;
    
    while (current != INVALID_ENTITY_ID && current != modelRootId) {
        auto* data = m_Scene.GetEntityData(current);
        if (!data) break;
        parts.push_back(data->Name);
        current = data->Parent;
    }
    
    // Reverse to get root-to-leaf order, then skip the root name
    std::reverse(parts.begin(), parts.end());
    
    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result += "/";
        result += parts[i];
    }
    
    return result;
}

StableNodeId ModelDeltaExtractor::BuildNodeId(EntityID entityId, EntityID modelRootId, const std::string& relativePath, int siblingIndex) {
    auto* data = m_Scene.GetEntityData(entityId);
    if (!data) return StableNodeId{};
    
    StableNodeId id;
    id.path = relativePath;
    id.normalizedPath = StableNodeId::ComputeNormalizedPath(relativePath);
    id.nodeName = data->Name;
    id.normalizedName = StableNodeId::ComputeNormalizedName(data->Name);
    id.entityGuid = data->EntityGuid;
    
    // Determine node type
    if (data->Mesh) {
        id.type = StableNodeId::NodeType::MeshNode;
        id.meshFileId = data->Mesh->meshReference.fileID;
        
        // Compute content hash for mesh nodes
        int vertHint = data->Mesh->mesh ? static_cast<int>(data->Mesh->mesh->Vertices.size()) : 0;
        id.contentHash = StableNodeId::ComputeContentHash(
            id.meshFileId,
            data->Transform.LocalMatrix,
            vertHint);
    }
    else if (data->Skeleton) {
        id.type = StableNodeId::NodeType::SkeletonRoot;
    }
    else if (IsModelRoot(data) && entityId != modelRootId) {
        // Nested model root - this shouldn't happen in normal extraction
        // but handle it gracefully
        id.type = StableNodeId::NodeType::MeshNode;
    }
    else {
        // Empty node or bone - determine by checking if parent is skeleton
        auto* parentData = m_Scene.GetEntityData(data->Parent);
        bool parentIsSkeleton = parentData && parentData->Skeleton;
        bool parentNameLooksBone = parentData && (
            parentData->Name.find("mixamorig") != std::string::npos ||
            parentData->Name.find("Bone") != std::string::npos ||
            parentData->Name.find("bone") != std::string::npos ||
            parentData->Name.find("Armature") != std::string::npos);
        
        if (parentIsSkeleton || parentNameLooksBone) {
            id.type = StableNodeId::NodeType::Bone;
        } else {
            id.type = StableNodeId::NodeType::Empty;
        }
    }
    
    // Compute structural hash
    std::string parentNormPath;
    if (!relativePath.empty()) {
        size_t lastSlash = relativePath.find_last_of('/');
        if (lastSlash != std::string::npos) {
            parentNormPath = StableNodeId::ComputeNormalizedPath(relativePath.substr(0, lastSlash));
        }
    }
    id.structuralHash = StableNodeId::ComputeStructuralHash(parentNormPath, siblingIndex, id.type);
    
    // Calculate depth
    id.depth = 0;
    for (char c : relativePath) {
        if (c == '/') id.depth++;
    }
    if (!relativePath.empty()) id.depth++;  // Add 1 for the node itself
    
    return id;
}

bool ModelDeltaExtractor::IsUserAddedChild(EntityID childId, EntityID modelRootId) {
    auto* childData = m_Scene.GetEntityData(childId);
    auto* rootData = m_Scene.GetEntityData(modelRootId);
    
    if (!childData || !rootData) return false;
    
    // If child has a different ModelAssetGuid (nested model), it's not a user-added child
    // of this model - it's a separate model
    if (IsModelRoot(childData)) {
        return false;  // This is a nested model, handled separately
    }
    
    // If child has mesh from the same model asset, it's a model node
    if (childData->Mesh && 
        childData->Mesh->meshReference.guid == rootData->ModelAssetGuid &&
        (rootData->ModelAssetGuid.high != 0 || rootData->ModelAssetGuid.low != 0)) {
        return false;
    }
    
    // If child has skeleton, skinning, or blend shapes, it's likely a model node
    if (childData->Skeleton || childData->Skinning || childData->BlendShapes) {
        return false;
    }
    
    // Check for user-facing components that indicate user-added entity
    bool hasUserComponents = childData->Camera || childData->Light || childData->Collider ||
                            childData->RigidBody || childData->StaticBody || 
                            childData->CharacterController || childData->Emitter ||
                            childData->Canvas || childData->Panel || childData->Button ||
                            childData->Text || childData->Navigation || childData->NavAgent ||
                            childData->Area || !childData->Scripts.empty() ||
                            childData->Terrain || childData->BoneAttachment ||
                            (childData->Extra.is_object() && !childData->Extra.empty());
    
    if (hasUserComponents) {
        return true;
    }
    
    // If entity has mesh from a DIFFERENT source (not this model's asset)
    if (childData->Mesh) {
        // Has mesh but not from this model - user-added mesh
        return true;
    }
    
    // Empty entity with no model GUID - check if it's a bone
    // First, check skeleton's BoneEntities list (most reliable)
    bool isBone = false;
    EntityID ancestor = childData->Parent;
    while (ancestor != INVALID_ENTITY_ID) {
        auto* ancestorData = m_Scene.GetEntityData(ancestor);
        if (!ancestorData) break;
        if (ancestorData->Skeleton && !ancestorData->Skeleton->BoneEntities.empty()) {
            // Found a skeleton - check if this entity is in its bone list
            for (EntityID boneId : ancestorData->Skeleton->BoneEntities) {
                if (boneId == childId) {
                    isBone = true;
                    break;
                }
            }
            // Stop after finding the first skeleton (closest ancestor)
            break;
        }
        ancestor = ancestorData->Parent;
    }
    
    // If no skeleton with BoneEntities found, fall back to name-based heuristics
    if (!isBone) {
        const std::string& name = childData->Name;
        bool nameLooksLikeBone = name.find("mixamorig") != std::string::npos ||
                                name.find("Bone") != std::string::npos ||
                                name.find("bone") != std::string::npos ||
                                name == "Armature" ||
                                name == "SkeletonRoot" ||
                                name.find("Bip001") != std::string::npos ||  // 3ds Max Biped
                                name.find("_End") != std::string::npos;      // Common bone end marker
        isBone = nameLooksLikeBone;
    }
    
    if (isBone) {
        return false;  // It's a bone, not user-added
    }
    
    // Default: if it doesn't look like a model node, treat as user-added
    return true;
}

bool ModelDeltaExtractor::IsNestedModelRoot(EntityID entityId, EntityID parentModelRootId) {
    auto* data = m_Scene.GetEntityData(entityId);
    if (!data) return false;
    
    // Must have its own ModelAssetGuid
    if (!IsModelRoot(data)) return false;
    
    // Must be different from parent model
    auto* parentRootData = m_Scene.GetEntityData(parentModelRootId);
    if (parentRootData && data->ModelAssetGuid == parentRootData->ModelAssetGuid) {
        return false;  // Same model asset, not a nested model
    }
    
    return true;
}

StableEntityRef ModelDeltaExtractor::BuildEntityRef(EntityID targetId, EntityID modelRootId) {
    StableEntityRef ref;
    
    if (targetId == INVALID_ENTITY_ID) {
        return ref;
    }
    
    auto* targetData = m_Scene.GetEntityData(targetId);
    if (!targetData) {
        return ref;
    }
    
    ref.targetGuid = targetData->EntityGuid;
    
    // Check if target is within the same model
    EntityID cur = targetId;
    bool inSameModel = false;
    while (cur != INVALID_ENTITY_ID) {
        if (cur == modelRootId) {
            inSameModel = true;
            break;
        }
        auto* d = m_Scene.GetEntityData(cur);
        if (!d) break;
        cur = d->Parent;
    }
    
    if (inSameModel) {
        ref.modelNodePath = ComputeRelativePath(modelRootId, targetId);
        auto* rootData = m_Scene.GetEntityData(modelRootId);
        if (rootData) {
            ref.modelGuid = rootData->ModelAssetGuid;
        }
    } else {
        // Target is outside this model - compute scene path
        std::vector<std::string> pathParts;
        cur = targetId;
        while (cur != INVALID_ENTITY_ID) {
            auto* d = m_Scene.GetEntityData(cur);
            if (!d) break;
            pathParts.push_back(d->Name);
            cur = d->Parent;
        }
        std::reverse(pathParts.begin(), pathParts.end());
        
        for (size_t i = 0; i < pathParts.size(); ++i) {
            if (i > 0) ref.scenePath += "/";
            ref.scenePath += pathParts[i];
        }
        
        // Check if target is in a different model
        cur = targetId;
        while (cur != INVALID_ENTITY_ID) {
            auto* d = m_Scene.GetEntityData(cur);
            if (!d) break;
            if (IsModelRoot(d)) {
                ref.modelGuid = d->ModelAssetGuid;
                ref.modelNodePath = ComputeRelativePath(cur, targetId);
                break;
            }
            cur = d->Parent;
        }
    }
    
    return ref;
}

std::optional<TransformDelta> ModelDeltaExtractor::ExtractTransformDelta(const EntityData* data, bool alwaysExtract) {
    if (!data) return std::nullopt;
    
    TransformDelta delta;
    delta.position = data->Transform.Position;
    delta.rotation = data->Transform.RotationQ;
    delta.scale = data->Transform.Scale;
    
    if (alwaysExtract) {
        delta.positionModified = true;
        delta.rotationModified = true;
        delta.scaleModified = true;
        return delta;
    }
    
    // TODO: Compare against original model transform to detect modifications
    // For now, always extract transforms to be safe
    delta.positionModified = true;
    delta.rotationModified = true;
    delta.scaleModified = true;
    
    return delta;
}

std::optional<MeshDelta> ModelDeltaExtractor::ExtractMeshDelta(const EntityData* data) {
    if (!data || !data->Mesh) return std::nullopt;
    
    const auto& mesh = *data->Mesh;
    MeshDelta delta;
    
    // Extract material overrides for each slot
    for (size_t i = 0; i < mesh.materials.size(); ++i) {
        MeshMaterialDelta matDelta;
        matDelta.slotIndex = static_cast<uint32_t>(i);
        
        // Material asset path
        if (i < mesh.MaterialAssetPaths.size()) {
            matDelta.materialAssetPath = mesh.MaterialAssetPaths[i];
        }
        
        // Texture paths from property blocks
        if (i < mesh.SlotPropertyBlockTexturePaths.size()) {
            matDelta.texturePaths = mesh.SlotPropertyBlockTexturePaths[i];
        }
        
        // Property block
        if (i < mesh.SlotPropertyBlocks.size()) {
            matDelta.propertyBlock = mesh.SlotPropertyBlocks[i];
        }
        
        // Only add if there's actual override data
        if (!matDelta.materialAssetPath.empty() || 
            !matDelta.texturePaths.empty() ||
            matDelta.alphaBlendEnabled) {
            delta.materialOverrides.push_back(std::move(matDelta));
        }
    }
    
    delta.showBackfaces = mesh.ShowBackfaces;
    delta.renderOnTop = mesh.RenderOnTop;
    delta.renderOrder = mesh.RenderOrder;
    delta.boundsPadding = mesh.BoundsPadding;
    delta.uniqueMaterial = mesh.UniqueMaterial;
    
    if (delta.HasModifications()) {
        return delta;
    }
    
    return std::nullopt;
}

std::optional<RenderOverridesDelta> ModelDeltaExtractor::ExtractRenderOverridesDelta(const EntityData* data) {
    if (!data || !data->RenderOverrides) return std::nullopt;
    
    const auto& ro = *data->RenderOverrides;
    RenderOverridesDelta delta;
    delta.alphaBlendEnabled = ro.AlphaBlendEnabled;
    delta.useAlphaCutout = ro.UseAlphaCutout;
    delta.alphaCutoutThreshold = ro.AlphaCutoutThreshold;
    delta.depthWriteEnabled = ro.DepthWriteEnabled;
    delta.castShadows = ro.CastShadows;
    delta.receiveShadows = ro.ReceiveShadows;
    delta.visible = ro.Visible;
    delta.sortingOrder = ro.SortingOrder;
    
    return delta;
}

std::optional<BlendShapeDelta> ModelDeltaExtractor::ExtractBlendShapeDelta(const EntityData* data) {
    if (!data || !data->BlendShapes) return std::nullopt;
    
    BlendShapeDelta delta;
    for (const auto& shape : data->BlendShapes->Shapes) {
        if (std::abs(shape.Weight) > 0.0001f) {  // Only store non-zero weights
            delta.weights[shape.Name] = shape.Weight;
        }
    }
    
    if (delta.weights.empty()) return std::nullopt;
    return delta;
}

std::optional<BoneAttachmentDelta> ModelDeltaExtractor::ExtractBoneAttachmentDelta(const EntityData* data, EntityID modelRootId) {
    if (!data || !data->BoneAttachment) return std::nullopt;
    
    const auto& ba = *data->BoneAttachment;
    BoneAttachmentDelta delta;
    delta.targetBoneName = ba.TargetBoneName;
    delta.localPosition = ba.LocalPosition;
    delta.localRotation = ba.LocalRotation;
    delta.localScale = ba.LocalScale;
    delta.inheritRotation = ba.InheritRotation;
    delta.inheritScale = ba.InheritScale;
    delta.enabled = ba.Enabled;
    
    // Build stable reference to skeleton
    if (ba.SkeletonEntity != INVALID_ENTITY_ID) {
        delta.skeletonRef = BuildEntityRef(ba.SkeletonEntity, modelRootId);
    }
    
    return delta;
}

std::vector<LookAtConstraintDelta> ModelDeltaExtractor::ExtractLookAtConstraints(const EntityData* data, EntityID modelRootId) {
    std::vector<LookAtConstraintDelta> deltas;
    
    if (!data) return deltas;
    
    for (const auto& lac : data->LookAtConstraints) {
        LookAtConstraintDelta delta;
        delta.enabled = lac.Enabled;
        delta.weight = lac.Weight;
        delta.mode = static_cast<uint8_t>(lac.Mode);
        delta.axes = static_cast<uint8_t>(lac.Axes);
        delta.space = static_cast<uint8_t>(lac.Space);
        delta.smoothingSpeed = lac.SmoothingSpeed;
        delta.maxYawDeg = lac.MaxYawDeg;
        delta.maxPitchDeg = lac.MaxPitchDeg;
        
        // Convert bone chain
        for (auto boneId : lac.BoneChain) {
            delta.boneChain.push_back(static_cast<int32_t>(boneId));
        }
        
        // Build target reference
        if (lac.TargetEntity != 0 && lac.TargetEntity != INVALID_ENTITY_ID) {
            delta.target = BuildEntityRef(lac.TargetEntity, modelRootId);
        }
        
        deltas.push_back(std::move(delta));
    }
    
    return deltas;
}

std::vector<IKConstraintDelta> ModelDeltaExtractor::ExtractIKConstraints(const EntityData* data, EntityID modelRootId) {
    std::vector<IKConstraintDelta> deltas;
    
    if (!data) return deltas;
    
    for (const auto& ik : data->IKs) {
        IKConstraintDelta delta;
        delta.enabled = ik.Enabled;
        delta.weight = ik.Weight;
        delta.useTwoBone = ik.UseTwoBone;
        delta.maxIterations = ik.MaxIterations;
        delta.tolerance = ik.Tolerance;
        delta.damping = ik.Damping;
        delta.lockAxisX = ik.LockAxisX;
        delta.lockAxisY = ik.LockAxisY;
        delta.lockAxisZ = ik.LockAxisZ;
        delta.chainRootHint = ik.ChainRootHint;
        delta.chainEffectorHint = ik.ChainEffectorHint;
        
        // Convert chain
        for (auto boneId : ik.Chain) {
            delta.chain.push_back(static_cast<int32_t>(boneId));
        }
        
        // Build target reference
        if (ik.TargetEntity != 0 && ik.TargetEntity != INVALID_ENTITY_ID) {
            delta.target = BuildEntityRef(ik.TargetEntity, modelRootId);
        }
        
        // Build pole reference
        if (ik.PoleEntity != 0 && ik.PoleEntity != INVALID_ENTITY_ID) {
            delta.pole = BuildEntityRef(ik.PoleEntity, modelRootId);
        }
        
        deltas.push_back(std::move(delta));
    }
    
    return deltas;
}

static bool IsScriptEntityLike(PropertyType type) {
    return type == PropertyType::Entity ||
           type == PropertyType::ComponentRef ||
           type == PropertyType::ScriptRef;
}

std::vector<ScriptDelta> ModelDeltaExtractor::ExtractScripts(const EntityData* data, EntityID modelRootId) {
    std::vector<ScriptDelta> deltas;
    
    if (!data) return deltas;
    
    for (const auto& script : data->Scripts) {
        ScriptDelta delta;
        delta.className = script.ClassName;

        const std::vector<PropertyInfo>* reflectedProps = nullptr;
        if (ScriptReflection::HasProperties(script.ClassName)) {
            reflectedProps = &ScriptReflection::GetScriptProperties(script.ClassName);
        }

        auto findReflectedProp = [&](const std::string& key) -> const PropertyInfo* {
            if (!reflectedProps) return nullptr;
            for (const auto& p : *reflectedProps) {
                if (p.name == key) return &p;
            }
            return nullptr;
        };

        auto appendRefMeta = [](nlohmann::json& j, const ScriptEntityRefMetadata& meta) {
            if (meta.entityId >= 0) j["entityId"] = meta.entityId;
            if (meta.guid.high != 0 || meta.guid.low != 0) j["guid"] = meta.guid.ToString();
            if (meta.modelGuid.high != 0 || meta.modelGuid.low != 0) j["modelGuid"] = meta.modelGuid.ToString();
            if (!meta.modelNodePath.empty()) j["modelNodePath"] = meta.modelNodePath;
        };

        // Serialize property values to JSON. Entity refs are encoded symbolically so
        // model-delta replay does not depend on unstable runtime IDs.
        for (const auto& [key, value] : script.Values) {
            const PropertyInfo* reflected = findReflectedProp(key);
            const bool reflectedEntityLike =
                reflected && IsScriptEntityLike(reflected->type);
            const bool reflectedEntityList =
                reflected && reflected->type == PropertyType::List &&
                IsScriptEntityLike(reflected->listElementType);

            auto metaIt = script.EntityRefMetadata.find(key);
            const bool hasScalarMeta = (metaIt != script.EntityRefMetadata.end());

            std::visit([&](auto&& arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, int>) {
                    if (reflectedEntityLike || hasScalarMeta) {
                        nlohmann::json refJson;
                        refJson["__entityRef"] = true;
                        refJson["value"] = arg;

                        ScriptEntityRefMetadata meta{};
                        bool haveMeta = false;
                        if (hasScalarMeta) {
                            meta = metaIt->second;
                            haveMeta = true;
                        } else if (arg > 0 && static_cast<EntityID>(arg) != INVALID_ENTITY_ID) {
                            StableEntityRef stable = BuildEntityRef(static_cast<EntityID>(arg), modelRootId);
                            meta.entityId = arg;
                            meta.guid = stable.targetGuid;
                            meta.modelGuid = stable.modelGuid;
                            meta.modelNodePath = stable.modelNodePath;
                            haveMeta = stable.IsValid();
                            if (!stable.scenePath.empty()) refJson["scenePath"] = stable.scenePath;
                        }
                        if (haveMeta) {
                            appendRefMeta(refJson, meta);
                        }
                        delta.properties[key] = std::move(refJson);
                    } else {
                        delta.properties[key] = arg;
                    }
                } else if constexpr (std::is_same_v<T, float>) {
                    delta.properties[key] = arg;
                } else if constexpr (std::is_same_v<T, bool>) {
                    delta.properties[key] = arg;
                } else if constexpr (std::is_same_v<T, std::string>) {
                    delta.properties[key] = arg;
                } else if constexpr (std::is_same_v<T, glm::vec3>) {
                    delta.properties[key] = nlohmann::json::array({arg.x, arg.y, arg.z});
                } else if constexpr (std::is_same_v<T, std::shared_ptr<ListPropertyValue>>) {
                    if (arg && (reflectedEntityList || !arg->entityRefs.empty())) {
                        nlohmann::json listJson;
                        listJson["__entityRefList"] = true;
                        listJson["items"] = nlohmann::json::array();
                        for (size_t i = 0; i < arg->elements.size(); ++i) {
                            nlohmann::json item;
                            int rawValue = -1;
                            if (const int* v = std::get_if<int>(&arg->elements[i])) {
                                rawValue = *v;
                            }
                            item["value"] = rawValue;

                            ScriptEntityRefMetadata meta{};
                            bool haveMeta = false;
                            if (i < arg->entityRefs.size()) {
                                meta = arg->entityRefs[i];
                                haveMeta = true;
                            } else if (rawValue > 0 && static_cast<EntityID>(rawValue) != INVALID_ENTITY_ID) {
                                StableEntityRef stable = BuildEntityRef(static_cast<EntityID>(rawValue), modelRootId);
                                meta.entityId = rawValue;
                                meta.guid = stable.targetGuid;
                                meta.modelGuid = stable.modelGuid;
                                meta.modelNodePath = stable.modelNodePath;
                                haveMeta = stable.IsValid();
                                if (!stable.scenePath.empty()) item["scenePath"] = stable.scenePath;
                            }

                            if (haveMeta) {
                                appendRefMeta(item, meta);
                            }
                            listJson["items"].push_back(std::move(item));
                        }
                        delta.properties[key] = std::move(listJson);
                    }
                }
                // List and Struct types would need more complex handling
            }, value);
        }
        
        deltas.push_back(std::move(delta));
    }
    
    return deltas;
}

std::optional<AddedLightDelta> ModelDeltaExtractor::ExtractAddedLight(const EntityData* data) {
    if (!data || !data->Light) return std::nullopt;
    
    const auto& l = *data->Light;
    AddedLightDelta delta;
    delta.type = static_cast<int>(l.Type);
    delta.color = l.Color;
    delta.intensity = l.Intensity;
    
    return delta;
}

std::optional<AddedColliderDelta> ModelDeltaExtractor::ExtractAddedCollider(const EntityData* data) {
    if (!data || !data->Collider) return std::nullopt;
    
    const auto& c = *data->Collider;
    AddedColliderDelta delta;
    delta.shapeType = static_cast<int>(c.ShapeType);
    delta.offset = c.Offset;
    delta.size = c.Size;
    delta.radius = c.Radius;
    delta.height = c.Height;
    delta.isTrigger = c.IsTrigger;
    delta.physicsLayerName = c.PhysicsLayerName;
    
    return delta;
}

std::optional<AddedCameraDelta> ModelDeltaExtractor::ExtractAddedCamera(const EntityData* data) {
    if (!data || !data->Camera) return std::nullopt;
    
    const auto& c = *data->Camera;
    AddedCameraDelta delta;
    delta.fieldOfView = c.FieldOfView;
    delta.nearClip = c.NearClip;
    delta.farClip = c.FarClip;
    delta.isPerspective = c.IsPerspective;
    delta.active = c.Active;
    delta.priority = c.priority;
    
    return delta;
}

std::optional<AddedEmitterDelta> ModelDeltaExtractor::ExtractAddedEmitter(const EntityData* data) {
    if (!data || !data->Emitter) return std::nullopt;
    
    const auto& e = *data->Emitter;
    AddedEmitterDelta delta;
    delta.spritePath = e.SpritePath;
    delta.maxParticles = e.MaxParticles;
    delta.emissionRate = e.EmissionRate;
    delta.enabled = e.Enabled;
    
    // Store full emitter config as JSON for complete restoration
    // This is a simplified version - full serialization would be more complex
    delta.fullData["duration"] = e.Duration;
    delta.fullData["looping"] = e.Looping;
    delta.fullData["shape"] = static_cast<int>(e.Shape);
    delta.fullData["startColor"] = nlohmann::json::array({e.StartColor.r, e.StartColor.g, e.StartColor.b, e.StartColor.a});
    delta.fullData["gravityModifier"] = e.GravityModifier;
    
    return delta;
}

std::optional<nlohmann::json> ModelDeltaExtractor::ExtractTintController(const EntityData* data, EntityID modelRootId) {
    if (!data || !data->TintController) return std::nullopt;
    
    const auto& tint = *data->TintController;
    nlohmann::json j;
    
    // Name pattern
    j["namePattern"] = tint.NamePattern;
    
    // Targets with entity paths for stable resolution
    nlohmann::json targetsArr = nlohmann::json::array();
    for (const auto& target : tint.Targets) {
        nlohmann::json t;
        t["entity"] = static_cast<int64_t>(target.TargetEntity);  // Keep for backward compat
        t["slot"] = target.MaterialSlot;
        t["blendMode"] = static_cast<int>(target.BlendMode);
        t["useColor"] = target.UseTargetColor;
        t["color"] = {target.Color.x, target.Color.y, target.Color.z, target.Color.w};
        
        // Build entity path for stable reference
        if (target.TargetEntity != INVALID_ENTITY_ID) {
            std::string entityPath = ComputeRelativePath(modelRootId, target.TargetEntity);
            t["entityPath"] = entityPath;
        }
        
        targetsArr.push_back(t);
    }
    j["targets"] = targetsArr;
    
    // Global settings
    j["globalBlendMode"] = static_cast<int>(tint.GlobalBlendMode);
    j["autoIncludeParentedSkinnedMeshes"] = tint.AutoIncludeParentedSkinnedMeshes;
    j["useTintMask"] = tint.UseTintMask;
    j["tintColor0"] = {tint.TintColor0.x, tint.TintColor0.y, tint.TintColor0.z, tint.TintColor0.w};
    j["tintColor1"] = {tint.TintColor1.x, tint.TintColor1.y, tint.TintColor1.z, tint.TintColor1.w};
    j["tintColor2"] = {tint.TintColor2.x, tint.TintColor2.y, tint.TintColor2.z, tint.TintColor2.w};
    j["tintColor3"] = {tint.TintColor3.x, tint.TintColor3.y, tint.TintColor3.z, tint.TintColor3.w};
    j["baseTint"] = {tint.BaseTint.x, tint.BaseTint.y, tint.BaseTint.z, tint.BaseTint.w};
    j["usePbrOverrides"] = tint.UsePbrOverrides;
    j["pbrMetallic"] = tint.OverrideMetallic;
    j["pbrRoughness"] = tint.OverrideRoughness;
    j["pbrEmissionStrength"] = tint.OverrideEmissionStrength;
    j["pbrEmissionColor"] = {tint.OverrideEmissionColor.x, tint.OverrideEmissionColor.y, tint.OverrideEmissionColor.z};
    
    return j;
}

void ModelDeltaExtractor::ExtractNodeDeltasRecursive(
    EntityID entityId,
    EntityID modelRootId,
    const std::string& parentPath,
    int siblingIndex,
    const DeltaExtractionConfig& config,
    ModelDelta& outDelta,
    std::vector<EntityID>& visitedNestedModels)
{
    auto* data = m_Scene.GetEntityData(entityId);
    if (!data) return;
    
    // Compute path for this node relative to model root.
    std::string nodePath;
    if (entityId != modelRootId) {
        nodePath = parentPath.empty() ? data->Name : (parentPath + "/" + data->Name);
    }
    
    // Check for nested model - don't recurse into nested models
    if (entityId != modelRootId && IsNestedModelRoot(entityId, modelRootId)) {
        // Record as added child (nested model)
        visitedNestedModels.push_back(entityId);
        return;
    }
    
    // Build node delta
    NodeDelta nodeDelta;
    nodeDelta.nodeId = BuildNodeId(entityId, modelRootId, nodePath, siblingIndex);
    
    // Entity-level overrides are optional for compatibility with runtime pipelines
    // that already deserialize full entity state from binary headers/components.
    if (config.extractEntityOverrides) {
        nodeDelta.name = data->Name;
        nodeDelta.layer = data->Layer;
        nodeDelta.tag = data->Tag;
        nodeDelta.visible = data->Visible;
        nodeDelta.active = data->Active;
    }
    
    // Extract component deltas
    if (config.extractTransforms) {
        nodeDelta.transform = ExtractTransformDelta(data, config.alwaysExtractTransforms);
    }
    
    if (config.extractMeshOverrides) {
        nodeDelta.mesh = ExtractMeshDelta(data);
        nodeDelta.renderOverrides = ExtractRenderOverridesDelta(data);
    }
    
    if (config.extractBlendShapes) {
        nodeDelta.blendShapes = ExtractBlendShapeDelta(data);
    }
    
    if (config.extractAddedComponents) {
        if (data->BoneAttachment) {
            nodeDelta.boneAttachment = ExtractBoneAttachmentDelta(data, modelRootId);
        }
        nodeDelta.addedLight = ExtractAddedLight(data);
        nodeDelta.addedCollider = ExtractAddedCollider(data);
        nodeDelta.addedCamera = ExtractAddedCamera(data);
        nodeDelta.addedEmitter = ExtractAddedEmitter(data);
        nodeDelta.tintController = ExtractTintController(data, modelRootId);
    }
    
    if (config.extractConstraints) {
        nodeDelta.lookAtConstraints = ExtractLookAtConstraints(data, modelRootId);
        nodeDelta.ikConstraints = ExtractIKConstraints(data, modelRootId);
    }
    
    if (config.extractScripts) {
        nodeDelta.scripts = ExtractScripts(data, modelRootId);
    }
    
    // Store extra data
    if (data->Extra.is_object() && !data->Extra.empty()) {
        nodeDelta.extra = data->Extra;
    }
    
    // Add to delta if there are modifications
    // For model root, always add to rootDelta
    if (entityId == modelRootId) {
        outDelta.rootDelta = std::move(nodeDelta);
    } else if (nodeDelta.HasModifications()) {
        outDelta.nodeDeltas.push_back(std::move(nodeDelta));
    }
    
    // Process children
    int childIndex = 0;
    bool extractedAddedChildrenForParent = false;
    for (EntityID childId : data->Children) {
        auto* childData = m_Scene.GetEntityData(childId);
        if (!childData) continue;
        
        // Check if this is a user-added child
        if (config.extractAddedChildren && IsUserAddedChild(childId, modelRootId)) {
            if (!extractedAddedChildrenForParent) {
                ExtractAddedChildren(entityId, modelRootId, nodePath, config, outDelta);
                extractedAddedChildrenForParent = true;
            }
            continue;  // ExtractAddedChildren handles this child
        }
        
        // Recurse into model children
        ExtractNodeDeltasRecursive(
            childId, 
            modelRootId, 
            nodePath,
            childIndex++,
            config,
            outDelta,
            visitedNestedModels);
    }
}

void ModelDeltaExtractor::ExtractAddedChildren(
    EntityID parentId,
    EntityID modelRootId,
    const std::string& parentPath,
    const DeltaExtractionConfig& config,
    ModelDelta& outDelta)
{
    auto* parentData = m_Scene.GetEntityData(parentId);
    if (!parentData) return;
    
    for (EntityID childId : parentData->Children) {
        auto* childData = m_Scene.GetEntityData(childId);
        if (!childData) continue;
        
        if (!IsUserAddedChild(childId, modelRootId)) continue;
        
        AddedChildDelta addedChild;
        
        // Build child identity
        std::string childPath = parentPath.empty() ? childData->Name : (parentPath + "/" + childData->Name);
        addedChild.childId = BuildNodeId(childId, modelRootId, childPath, 0);
        addedChild.childId.type = StableNodeId::NodeType::UserAdded;
        
        // Build parent identity
        addedChild.parentNodeId = BuildNodeId(parentId, modelRootId,
                                              parentId == modelRootId ? "" : parentPath, 0);
        
        // Check if this is a nested model
        if (IsModelRoot(childData)) {
            addedChild.isNestedModel = true;
            AssetReference ref;
            ref.guid = childData->ModelAssetGuid;
            addedChild.nestedModelAsset = ref;
        }
        
        // Serialize complete entity data
        // This is a simplified version - full entity serialization would be more complex
        addedChild.entityData["name"] = childData->Name;
        addedChild.entityData["guid"] = childData->EntityGuid.ToString();
        addedChild.entityData["layer"] = childData->Layer;
        addedChild.entityData["tag"] = childData->Tag;
        addedChild.entityData["visible"] = childData->Visible;
        addedChild.entityData["active"] = childData->Active;
        
        // Serialize transform
        addedChild.entityData["transform"]["position"] = {
            childData->Transform.Position.x,
            childData->Transform.Position.y,
            childData->Transform.Position.z
        };
        addedChild.entityData["transform"]["rotation"] = {
            childData->Transform.RotationQ.w,
            childData->Transform.RotationQ.x,
            childData->Transform.RotationQ.y,
            childData->Transform.RotationQ.z
        };
        addedChild.entityData["transform"]["scale"] = {
            childData->Transform.Scale.x,
            childData->Transform.Scale.y,
            childData->Transform.Scale.z
        };
        
        // Add component flags for restoration
        if (childData->Camera) addedChild.entityData["hasCamera"] = true;
        if (childData->Light) addedChild.entityData["hasLight"] = true;
        if (childData->Collider) addedChild.entityData["hasCollider"] = true;
        if (!childData->Scripts.empty()) addedChild.entityData["hasScripts"] = true;
        
        outDelta.addedChildren.push_back(std::move(addedChild));
        
        if (config.verbose) {
            std::cout << "[DeltaExtractor] Found added child: " << childData->Name 
                      << " under " << parentPath << std::endl;
        }
    }
}

ModelDelta ModelDeltaExtractor::Extract(EntityID modelRootId, const DeltaExtractionConfig& config) {
    ModelDelta delta;
    
    auto* rootData = m_Scene.GetEntityData(modelRootId);
    if (!rootData || !IsModelRoot(rootData)) {
        if (config.verbose) {
            std::cerr << "[DeltaExtractor] Entity " << modelRootId << " is not a model root\n";
        }
        return delta;
    }
    
    // Fill header
    delta.modelAssetGuid = rootData->ModelAssetGuid;
    delta.createdTimestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count());
    
    // Extract deleted nodes
    if (config.extractDeletedNodes) {
        delta.deletedNodePaths.clear();
        delta.deletedNodePaths.reserve(rootData->DeletedModelNodes.size());
        for (const auto& path : rootData->DeletedModelNodes) {
            if (!path.empty()) {
                delta.deletedNodePaths.push_back(path);
            }
        }
    }
    
    // Recursively extract node deltas
    std::vector<EntityID> visitedNestedModels;
    ExtractNodeDeltasRecursive(
        modelRootId, 
        modelRootId, 
        "", 
        0, 
        config, 
        delta,
        visitedNestedModels);
    
    // Handle nested models (record as added children)
    if (config.extractAddedChildren) {
        for (EntityID nestedModelId : visitedNestedModels) {
            auto* nestedData = m_Scene.GetEntityData(nestedModelId);
            if (!nestedData) continue;
            
            AddedChildDelta addedChild;
            
            // Find parent path
            EntityID parentId = nestedData->Parent;
            std::string parentPath = parentId != INVALID_ENTITY_ID && parentId != modelRootId
                                   ? ComputeRelativePath(modelRootId, parentId)
                                   : "";
            
            std::string childPath = parentPath.empty() ? nestedData->Name : (parentPath + "/" + nestedData->Name);
            addedChild.childId.path = childPath;
            addedChild.childId.normalizedPath = StableNodeId::ComputeNormalizedPath(childPath);
            addedChild.childId.nodeName = nestedData->Name;
            addedChild.childId.entityGuid = nestedData->EntityGuid;
            addedChild.childId.type = StableNodeId::NodeType::MeshNode;
            
            addedChild.parentNodeId.path = parentPath;
            addedChild.parentNodeId.normalizedPath = StableNodeId::ComputeNormalizedPath(parentPath);
            
            addedChild.isNestedModel = true;
            AssetReference ref;
            ref.guid = nestedData->ModelAssetGuid;
            addedChild.nestedModelAsset = ref;
            
            addedChild.entityData["name"] = nestedData->Name;
            addedChild.entityData["modelAssetGuid"] = nestedData->ModelAssetGuid.ToString();
            
            delta.addedChildren.push_back(std::move(addedChild));
            
            if (config.verbose) {
                std::cout << "[DeltaExtractor] Found nested model: " << nestedData->Name << std::endl;
            }
        }
    }
    
    if (config.verbose) {
        std::cout << "[DeltaExtractor] Extracted delta with "
                  << delta.nodeDeltas.size() << " node deltas, "
                  << delta.addedChildren.size() << " added children, "
                  << delta.deletedNodePaths.size() << " deleted nodes\n";
    }
    
    return delta;
}

std::vector<std::pair<EntityID, ModelDelta>> ModelDeltaExtractor::ExtractAll(const DeltaExtractionConfig& config) {
    std::vector<std::pair<EntityID, ModelDelta>> results;
    
    auto roots = GetModelRoots();
    results.reserve(roots.size());
    
    for (EntityID rootId : roots) {
        ModelDelta delta = Extract(rootId, config);
        if (!delta.IsEmpty()) {
            results.emplace_back(rootId, std::move(delta));
        }
    }
    
    return results;
}

} // namespace cm::model

