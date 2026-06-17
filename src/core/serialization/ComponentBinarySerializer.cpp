#include "ComponentBinarySerializer.h"
#include "core/assets/BinaryFormats.h"
#include "core/serialization/EntityBinaryLoader.h"
#include "core/ecs/EntityData.h"
#include "core/ecs/Components.h"
#include "core/ecs/InstancerComponent.h"
#include "core/animation/AnimationPlayerComponent.h"
#include "core/animation/AvatarDefinition.h"
#include "core/physics/area/AreaComponent.h"
#include "core/navigation/NavMesh.h"
#include "core/navigation/NavAgent.h"
#include "core/resourcelayer/ResourceLayerTypes.h"
#include "core/rendering/PBRMaterial.h"
#include "core/rendering/MaterialPropertyBlock.h"
#ifndef CLAYMORE_RUNTIME
#include "editor/nodegraph/ShaderGraphMaterial.h"
#include "editor/nodegraph/ShaderGraphSerializer.h"
#endif
#include "managed/interop/ScriptComponent.h"
#include "Serializer.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <iostream>
#include <variant>
#include <filesystem>
#include <unordered_map>
#include <bgfx/bgfx.h>
#include <cstring>
#include <cassert>

// Ensure prefab schema parity stays aligned with the emitted fields.
static_assert(binary::PREFAB_VERSION >= 5,
              "ComponentBinarySerializer assumes PREFAB_VERSION >= 5 for v5 schema fields. "
              "Update the serializer if the prefab version changes.");

namespace binary {

namespace {
static void DebugRunPrefabRoundTripSmokeTest();

// Debug-time guard to surface unexpected prefab version mismatches.
static void DebugAssertPrefabWriterVersion() {
#if !defined(NDEBUG)
    static bool logged = false;
    if (!logged) {
        logged = true;
        if (binary::PREFAB_VERSION < 5) {
            std::cerr << "[ComponentBinarySerializer] PREFAB_VERSION=" << binary::PREFAB_VERSION
                      << " is older than the schema emitted by this writer (v5). "
                      << "Prefab binaries may lose required fields." << std::endl;
        }
        DebugRunPrefabRoundTripSmokeTest();
    }
#endif
}
}  // namespace

// Serialize a material property block (Vec4 uniforms only; textures are persisted via path maps)
static void WritePropertyBlock(ComponentWriteContext& ctx, const MaterialPropertyBlock& pb) {
    uint32_t vecCount = static_cast<uint32_t>(pb.Vec4Uniforms.size());
    ctx.Write(vecCount);
    for (const auto& kv : pb.Vec4Uniforms) {
        uint32_t nameIdx = ctx.AddString(kv.first);
        ctx.Write(nameIdx);
        ctx.Write(kv.second.x);
        ctx.Write(kv.second.y);
        ctx.Write(kv.second.z);
        ctx.Write(kv.second.w);
    }
}

static void WriteTexturePathMap(ComponentWriteContext& ctx, const std::unordered_map<std::string, std::string>& paths) {
    uint32_t texCount = static_cast<uint32_t>(paths.size());
    ctx.Write(texCount);
    for (const auto& kv : paths) {
        uint32_t samplerIdx = ctx.AddString(kv.first);
        uint32_t pathIdx = ctx.AddString(kv.second);
        ctx.Write(samplerIdx);
        ctx.Write(pathIdx);
    }
}

namespace {
constexpr uint32_t kScriptBlockTypedFlag = 0x80000000u;
static bool gRoundTripInProgress = false;

enum class ScriptValueTag : uint8_t {
    Int = 0,
    Float = 1,
    Bool = 2,
    String = 3,
    Vec3 = 4,
    Entity = 5,
    List = 6,
    Struct = 7,
    Dictionary = 8
};

static bool IsEntityLike(PropertyType t) {
    return t == PropertyType::Entity || t == PropertyType::ComponentRef || t == PropertyType::ScriptRef;
}

static bool HasEntityRefHints(const ScriptEntityRefMetadata& meta) {
    return meta.entityId > 0 ||
           meta.guid.high != 0 || meta.guid.low != 0 ||
           meta.modelGuid.high != 0 || meta.modelGuid.low != 0 ||
           meta.modelRootGuid.high != 0 || meta.modelRootGuid.low != 0 ||
           !meta.modelNodePath.empty();
}

static PropertyType ResolvePropertyType(const ScriptInstance& script, const std::string& name, const PropertyValue& value) {
    if (ScriptReflection::HasProperties(script.ClassName)) {
        auto& props = ScriptReflection::GetScriptProperties(script.ClassName);
        auto it = std::find_if(props.begin(), props.end(), [&](const PropertyInfo& p) { return p.name == name; });
        if (it != props.end()) {
            return it->type;
        }
    }

    auto metaIt = script.EntityRefMetadata.find(name);
    if (metaIt != script.EntityRefMetadata.end() && HasEntityRefHints(metaIt->second)) {
        return PropertyType::Entity;
    }
    
    if (std::holds_alternative<int>(value)) return PropertyType::Int;
    if (std::holds_alternative<float>(value)) return PropertyType::Float;
    if (std::holds_alternative<bool>(value)) return PropertyType::Bool;
    if (std::holds_alternative<std::string>(value)) return PropertyType::String;
    if (std::holds_alternative<glm::vec3>(value)) return PropertyType::Vector3;
    if (std::holds_alternative<std::shared_ptr<ListPropertyValue>>(value)) return PropertyType::List;
    if (std::holds_alternative<std::shared_ptr<StructPropertyValue>>(value)) return PropertyType::Struct;
    if (std::holds_alternative<std::shared_ptr<DictionaryPropertyValue>>(value)) return PropertyType::Dictionary;
    return PropertyType::String;
}

static ScriptValueTag ToValueTag(PropertyType declaredType, const PropertyValue& value) {
    switch (declaredType) {
        case PropertyType::Int: return ScriptValueTag::Int;
        case PropertyType::Float: return ScriptValueTag::Float;
        case PropertyType::Bool: return ScriptValueTag::Bool;
        case PropertyType::String: return ScriptValueTag::String;
        case PropertyType::Vector3: return ScriptValueTag::Vec3;
        case PropertyType::Entity:
        case PropertyType::ComponentRef:
        case PropertyType::ScriptRef: return ScriptValueTag::Entity;
        case PropertyType::List: return ScriptValueTag::List;
        case PropertyType::Struct: return ScriptValueTag::Struct;
        case PropertyType::Dictionary: return ScriptValueTag::Dictionary;
        default: break;
    }
    
    // Fallback to variant introspection
    if (std::holds_alternative<int>(value)) return ScriptValueTag::Int;
    if (std::holds_alternative<float>(value)) return ScriptValueTag::Float;
    if (std::holds_alternative<bool>(value)) return ScriptValueTag::Bool;
    if (std::holds_alternative<std::string>(value)) return ScriptValueTag::String;
    if (std::holds_alternative<glm::vec3>(value)) return ScriptValueTag::Vec3;
    if (std::holds_alternative<std::shared_ptr<ListPropertyValue>>(value)) return ScriptValueTag::List;
    if (std::holds_alternative<std::shared_ptr<StructPropertyValue>>(value)) return ScriptValueTag::Struct;
    if (std::holds_alternative<std::shared_ptr<DictionaryPropertyValue>>(value)) return ScriptValueTag::Dictionary;
    return ScriptValueTag::String;
}

static bool ComputeModelNodePathInfo(Scene* scene,
                                     EntityID id,
                                     ClaymoreGUID& outModelGuid,
                                     ClaymoreGUID& outModelRootGuid,
                                     std::string& outRelPath) {
    outModelGuid = {};
    outModelRootGuid = {};
    outRelPath.clear();
    if (!scene || id == INVALID_ENTITY_ID) return false;
    
    std::vector<EntityID> chain;
    EntityID cur = id;
    size_t guard = 0;
    while (cur != INVALID_ENTITY_ID && guard++ < 100000) {
        chain.push_back(cur);
        auto* d = scene->GetEntityData(cur);
        if (!d || d->Parent == INVALID_ENTITY_ID || d->Parent == cur) break;
        cur = d->Parent;
    }
    if (chain.empty()) return false;
    std::reverse(chain.begin(), chain.end());
    
    EntityID modelRoot = INVALID_ENTITY_ID;
    ClaymoreGUID modelGuid{};
    for (EntityID nid : chain) {
        auto* d = scene->GetEntityData(nid);
        if (!d) continue;
        if (d->ModelAssetGuid.high != 0 || d->ModelAssetGuid.low != 0) {
            modelRoot = nid;
            modelGuid = d->ModelAssetGuid;
            break;
        }
    }
    if (modelRoot == INVALID_ENTITY_ID) return false;
    
    std::string relPath;
    bool started = false;
    for (EntityID nid : chain) {
        if (!started) {
            if (nid == modelRoot) {
                started = true;
            }
            continue;
        }
        auto* d = scene->GetEntityData(nid);
        if (!d) continue;
        if (!relPath.empty()) relPath += "/";
        relPath += d->Name;
    }
    
    outModelGuid = modelGuid;
    if (auto* rootData = scene->GetEntityData(modelRoot)) {
        outModelRootGuid = rootData->EntityGuid;
    }
    outRelPath = relPath;
    return true;
}

struct SerializedEntityRef {
    int32_t entityId = -1;
    ClaymoreGUID guid{};
    ClaymoreGUID modelGuid{};
    ClaymoreGUID modelRootGuid{};
    std::string modelNodePath;
};

static SerializedEntityRef BuildEntityRef(ComponentWriteContext& ctx, int entityId, const ScriptEntityRefMetadata* metaOverride = nullptr) {
    SerializedEntityRef ref;
    ref.entityId = entityId;
    if (metaOverride) {
        if (metaOverride->entityId > 0 && ref.entityId <= 0) {
            ref.entityId = metaOverride->entityId;
        }
        ref.guid = metaOverride->guid;
        ref.modelGuid = metaOverride->modelGuid;
        ref.modelRootGuid = metaOverride->modelRootGuid;
        ref.modelNodePath = metaOverride->modelNodePath;
    }
    if (!ctx.scene || ref.entityId < 0) return ref;
    
    if (auto* d = ctx.scene->GetEntityData(ref.entityId)) {
        ref.guid = d->EntityGuid;
        ClaymoreGUID modelGuid{};
        ClaymoreGUID modelRootGuid{};
        std::string modelNodePath;
        if (ComputeModelNodePathInfo(ctx.scene, ref.entityId, modelGuid, modelRootGuid, modelNodePath)) {
            ref.modelGuid = modelGuid;
            ref.modelRootGuid = modelRootGuid;
            ref.modelNodePath = modelNodePath;
        }
    }
    return ref;
}

static void WriteTypedScriptValue(ComponentWriteContext& ctx, const PropertyValue& value, PropertyType declaredType, const ScriptEntityRefMetadata* entityMetaOverride = nullptr) {
    ScriptValueTag tag = ToValueTag(declaredType, value);
    if (tag == ScriptValueTag::Int && std::holds_alternative<int>(value) && entityMetaOverride &&
        HasEntityRefHints(*entityMetaOverride)) {
        tag = ScriptValueTag::Entity;
    }
    ctx.Write(static_cast<uint8_t>(tag));
    
    switch (tag) {
        case ScriptValueTag::Int: {
            int v = 0;
            if (std::holds_alternative<int>(value)) v = std::get<int>(value);
            ctx.Write(static_cast<int32_t>(v));
            break;
        }
        case ScriptValueTag::Float: {
            float v = 0.0f;
            if (std::holds_alternative<float>(value)) v = std::get<float>(value);
            ctx.Write(v);
            break;
        }
        case ScriptValueTag::Bool: {
            uint8_t v = 0;
            if (std::holds_alternative<bool>(value)) v = std::get<bool>(value) ? 1 : 0;
            ctx.Write(v);
            break;
        }
        case ScriptValueTag::String: {
            std::string s = ScriptReflection::PropertyValueToString(value);
            uint32_t idx = ctx.AddString(s);
            ctx.Write(idx);
            break;
        }
        case ScriptValueTag::Vec3: {
            glm::vec3 v{0.0f};
            if (std::holds_alternative<glm::vec3>(value)) v = std::get<glm::vec3>(value);
            ctx.Write(v.x); ctx.Write(v.y); ctx.Write(v.z);
            break;
        }
        case ScriptValueTag::Entity: {
            int entityId = -1;
            if (std::holds_alternative<int>(value)) entityId = std::get<int>(value);
            SerializedEntityRef ref = BuildEntityRef(ctx, entityId, entityMetaOverride);
            ctx.Write(static_cast<int32_t>(ref.entityId));
            uint8_t hasGuid = (ref.guid.high != 0 || ref.guid.low != 0) ? 1 : 0;
            ctx.Write(hasGuid);
            if (hasGuid) {
                ctx.Write(ref.guid.high);
                ctx.Write(ref.guid.low);
            }
            uint8_t hasModel =
                (ref.modelGuid.high != 0 || ref.modelGuid.low != 0 ||
                 ref.modelRootGuid.high != 0 || ref.modelRootGuid.low != 0 ||
                 !ref.modelNodePath.empty()) ? 1 : 0;
            ctx.Write(hasModel);
            if (hasModel) {
                ctx.Write(ref.modelGuid.high);
                ctx.Write(ref.modelGuid.low);
                ctx.Write(ref.modelRootGuid.high);
                ctx.Write(ref.modelRootGuid.low);
                uint32_t pathIdx = ctx.AddString(ref.modelNodePath);
                ctx.Write(pathIdx);
            }
            break;
        }
        case ScriptValueTag::List: {
            PropertyType elemType = PropertyType::Int;
            std::shared_ptr<ListPropertyValue> listPtr;
            if (std::holds_alternative<std::shared_ptr<ListPropertyValue>>(value)) {
                listPtr = std::get<std::shared_ptr<ListPropertyValue>>(value);
                if (listPtr) {
                    elemType = listPtr->elementType;
                    // If list metadata carries entity references but element type degraded to Int
                    // (e.g. reflection not loaded), force entity-like encoding.
                    if (!IsEntityLike(elemType) && !listPtr->entityRefs.empty()) {
                        bool anyHints = false;
                        for (const auto& refMeta : listPtr->entityRefs) {
                            if (HasEntityRefHints(refMeta)) {
                                anyHints = true;
                                break;
                            }
                        }
                        if (anyHints) {
                            elemType = PropertyType::Entity;
                        }
                    }
                }
            }
            ctx.Write(static_cast<uint8_t>(elemType));
            uint32_t count = listPtr ? static_cast<uint32_t>(listPtr->elements.size()) : 0;
            ctx.Write(count);
            if (listPtr) {
                for (size_t i = 0; i < listPtr->elements.size(); ++i) {
                    const ScriptEntityRefMetadata* elemMeta = nullptr;
                    if (IsEntityLike(elemType) && i < listPtr->entityRefs.size()) {
                        elemMeta = &listPtr->entityRefs[i];
                    }
                    WriteTypedScriptValue(ctx, listPtr->elements[i], elemType, elemMeta);
                }
            }
            break;
        }
        case ScriptValueTag::Struct: {
            std::shared_ptr<StructPropertyValue> structPtr;
            if (std::holds_alternative<std::shared_ptr<StructPropertyValue>>(value)) {
                structPtr = std::get<std::shared_ptr<StructPropertyValue>>(value);
            }
            uint32_t fieldCount = structPtr ? static_cast<uint32_t>(structPtr->fields.size()) : 0;
            ctx.Write(fieldCount);
            if (structPtr) {
                for (const auto& field : structPtr->fields) {
                    uint32_t keyIdx = ctx.AddString(field.first);
                    ctx.Write(keyIdx);
                    PropertyType fieldType = ResolvePropertyType({}, field.first, field.second);
                    WriteTypedScriptValue(ctx, field.second, fieldType, nullptr);
                }
            }
            break;
        }
        case ScriptValueTag::Dictionary: {
            std::shared_ptr<DictionaryPropertyValue> dictPtr;
            if (std::holds_alternative<std::shared_ptr<DictionaryPropertyValue>>(value)) {
                dictPtr = std::get<std::shared_ptr<DictionaryPropertyValue>>(value);
            }
            PropertyType keyType = dictPtr ? dictPtr->keyType : PropertyType::String;
            PropertyType valueType = dictPtr ? dictPtr->valueType : PropertyType::Int;
            ctx.Write(static_cast<uint8_t>(keyType));
            ctx.Write(static_cast<uint8_t>(valueType));
            uint32_t entryCount = dictPtr ? static_cast<uint32_t>(dictPtr->entries.size()) : 0;
            ctx.Write(entryCount);
            if (dictPtr) {
                for (const auto& entry : dictPtr->entries) {
                    WriteTypedScriptValue(ctx, entry.first, keyType, nullptr);
                    WriteTypedScriptValue(ctx, entry.second, valueType, nullptr);
                }
            }
            break;
        }
    }
}

#if !defined(NDEBUG)
struct DebugComponentSlice {
    ComponentTypeId type;
    size_t start = 0;
    size_t size = 0;
};

static void DebugRunPrefabRoundTripSmokeTest() {
    static bool ran = false;
    if (ran || gRoundTripInProgress) return;
    ran = true;
    gRoundTripInProgress = true;

    EntityData src;
    src.AnimationPlayer = std::make_unique<cm::animation::AnimationPlayerComponent>();
    src.AnimationPlayer->PlaybackSpeed = 1.25f;
    src.AnimationPlayer->MotionTarget = cm::animation::RootMotionTarget::FindCharacterController;
    src.AnimationPlayer->ExplicitTargetEntityId = 7;
    src.AnimationPlayer->ControllerPath = "assets/animations/base.animctrl";
    src.AnimationPlayer->ControllerOverridePath = "assets/animations/base.animoverride";

    src.NavAgent = std::make_unique<nav::NavAgentComponent>();
    src.NavAgent->Enabled = true;
    src.NavAgent->NavMeshEntity = 2;
    src.NavAgent->Params.radius = 0.5f;
    src.NavAgent->Params.height = 2.0f;
    src.NavAgent->Params.maxSpeed = 3.0f;
    src.NavAgent->Params.maxAccel = 6.0f;
    src.NavAgent->ArriveThreshold = 0.2f;
    src.NavAgent->AutoRepath = true;
    src.NavAgent->RepathInterval = 0.5f;
    src.NavAgent->AvoidanceRadiusMul = 1.1f;
    src.NavAgent->SteeringSmoothness = 0.35f;
    src.NavAgent->ArrivalSlowdownDist = 0.8f;
    src.NavAgent->Params.maxSlopeDeg = 40.0f;
    src.NavAgent->Params.maxStep = 0.3f;

    src.TintController = std::make_unique<TintMaskController>();
    src.TintController->UseTintMask = true;
    src.TintController->BaseTint = glm::vec4(1.0f, 0.9f, 0.8f, 1.0f);
    TintTarget tt{};
    tt.TargetEntity = 5;
    tt.BlendMode = TintBlendMode::Multiply;
    tt.MaterialSlot = 1;
    tt.UseTargetColor = true;
    tt.Color = glm::vec4(0.5f, 0.6f, 0.7f, 1.0f);
    src.TintController->Targets.push_back(tt);

    src.GrassDeformer = std::make_unique<GrassDeformerComponent>();
    src.GrassDeformer->Enabled = true;
    src.GrassDeformer->Radius = 2.0f;
    src.GrassDeformer->Strength = 0.5f;
    src.GrassDeformer->HeightOffset = 0.25f;
    src.GrassDeformer->UseVelocity = true;

    src.Emitter = std::make_unique<ParticleEmitterComponent>();
    src.Emitter->FaceCamera = false;
    src.Emitter->RenderOrder = 42;
    src.Emitter->Prewarm = true;
    src.Emitter->StopEmittingOnComplete = false;
    src.Emitter->StartColorRandom = true;
    src.Emitter->StartColorMin = glm::vec4(0.1f, 0.2f, 0.3f, 0.4f);
    src.Emitter->StartColorMax = glm::vec4(0.9f, 0.8f, 0.7f, 0.6f);
    src.Emitter->VelocityOverLifetimeEnabled = true;
    src.Emitter->LinearVelocity = glm::vec3(1.0f, 2.0f, 3.0f);
    src.Emitter->OrbitalVelocity = 4.0f;
    src.Emitter->RadialVelocity = -5.0f;
    src.Emitter->ColorOverLifetimeEnabled = false;
    src.Emitter->RotationOverLifetimeEnabled = true;
    src.Emitter->AngularVelocity = 90.0f;

    ScriptInstance script;
    script.ClassName = "DebugScript";
    script.Values["entityRef"] = 3;
    ScriptEntityRefMetadata entityRefMeta;
    entityRefMeta.entityId = 3;
    entityRefMeta.guid = ClaymoreGUID{11, 12};
    entityRefMeta.modelGuid = ClaymoreGUID{21, 22};
    entityRefMeta.modelRootGuid = ClaymoreGUID{31, 32};
    entityRefMeta.modelNodePath = "Rig/Hips";
    script.EntityRefMetadata["entityRef"] = entityRefMeta;
    src.Scripts.push_back(script);

    std::vector<uint8_t> buf;
    std::vector<std::string> strings;
    strings.push_back("");
    auto addString = [&](const std::string& s) -> uint32_t {
        strings.push_back(s);
        return static_cast<uint32_t>(strings.size() - 1);
    };

    ComponentWriteContext wctx{buf, strings, addString, nullptr, 0};
    std::vector<DebugComponentSlice> slices;
    auto writeSlice = [&](ComponentTypeId type) {
        size_t start = buf.size();
        binary::WriteComponentBinary(wctx, &src, type);
        size_t size = buf.size() - start;
        slices.push_back({type, start, size});
    };

    writeSlice(ComponentTypeId::AnimationPlayer);
    writeSlice(ComponentTypeId::NavAgent);
    writeSlice(ComponentTypeId::TintController);
    writeSlice(ComponentTypeId::GrassDeformer);
    writeSlice(ComponentTypeId::ParticleEmitter);
    writeSlice(ComponentTypeId::Script);

    EntityData dst;
    for (const auto& slice : slices) {
        binary::ComponentLoadContext lctx;
        lctx.data = buf.data();
        lctx.size = slice.start + slice.size;
        lctx.pos = slice.start;
        lctx.version = binary::PREFAB_VERSION;
        lctx.readString = [&](uint32_t idx) -> std::string {
            return idx < strings.size() ? strings[idx] : std::string();
        };
        binary::LoadComponentBinary(lctx, &dst, slice.type, static_cast<uint32_t>(slice.size));
    }

    assert(dst.AnimationPlayer && dst.NavAgent && dst.TintController && dst.GrassDeformer && !dst.Scripts.empty());
    assert(dst.AnimationPlayer->ExplicitTargetEntityId == src.AnimationPlayer->ExplicitTargetEntityId);
    assert(dst.NavAgent->RepathInterval == src.NavAgent->RepathInterval);
    assert(dst.TintController->Targets.size() == src.TintController->Targets.size());
    assert(dst.GrassDeformer->UseVelocity == src.GrassDeformer->UseVelocity);
    assert(dst.Emitter && dst.Emitter->FaceCamera == src.Emitter->FaceCamera);
    assert(dst.Emitter->RenderOrder == src.Emitter->RenderOrder);
    assert(dst.Emitter->Prewarm == src.Emitter->Prewarm);
    assert(dst.Emitter->StopEmittingOnComplete == src.Emitter->StopEmittingOnComplete);
    assert(dst.Emitter->StartColorRandom == src.Emitter->StartColorRandom);
    assert(dst.Emitter->StartColorMin.x == src.Emitter->StartColorMin.x);
    assert(dst.Emitter->StartColorMin.y == src.Emitter->StartColorMin.y);
    assert(dst.Emitter->StartColorMin.z == src.Emitter->StartColorMin.z);
    assert(dst.Emitter->StartColorMin.w == src.Emitter->StartColorMin.w);
    assert(dst.Emitter->StartColorMax.x == src.Emitter->StartColorMax.x);
    assert(dst.Emitter->StartColorMax.y == src.Emitter->StartColorMax.y);
    assert(dst.Emitter->StartColorMax.z == src.Emitter->StartColorMax.z);
    assert(dst.Emitter->StartColorMax.w == src.Emitter->StartColorMax.w);
    assert(dst.Emitter->VelocityOverLifetimeEnabled == src.Emitter->VelocityOverLifetimeEnabled);
    assert(dst.Emitter->LinearVelocity.x == src.Emitter->LinearVelocity.x);
    assert(dst.Emitter->LinearVelocity.y == src.Emitter->LinearVelocity.y);
    assert(dst.Emitter->LinearVelocity.z == src.Emitter->LinearVelocity.z);
    assert(dst.Emitter->OrbitalVelocity == src.Emitter->OrbitalVelocity);
    assert(dst.Emitter->RadialVelocity == src.Emitter->RadialVelocity);
    assert(dst.Emitter->ColorOverLifetimeEnabled == src.Emitter->ColorOverLifetimeEnabled);
    assert(dst.Emitter->RotationOverLifetimeEnabled == src.Emitter->RotationOverLifetimeEnabled);
    assert(dst.Emitter->AngularVelocity == src.Emitter->AngularVelocity);
    assert(dst.Scripts[0].EntityRefMetadata["entityRef"].guid == entityRefMeta.guid);
    assert(dst.Scripts[0].EntityRefMetadata["entityRef"].modelGuid == entityRefMeta.modelGuid);
    assert(dst.Scripts[0].EntityRefMetadata["entityRef"].modelRootGuid == entityRefMeta.modelRootGuid);
    assert(dst.Scripts[0].EntityRefMetadata["entityRef"].modelNodePath == entityRefMeta.modelNodePath);
    gRoundTripInProgress = false;
}
#endif
} // namespace

std::vector<ComponentTypeId> GetEntityComponents(const EntityData* data) {
    std::vector<ComponentTypeId> components;
    
    // Transform is always present (implicitly)
    components.push_back(ComponentTypeId::Transform);
    
    // Check all component types
    if (data->Mesh) components.push_back(ComponentTypeId::Mesh);
    if (data->MeshProxy) components.push_back(ComponentTypeId::MeshProxy);
    if (data->Light) components.push_back(ComponentTypeId::Light);
    if (data->Camera) components.push_back(ComponentTypeId::Camera);
    if (data->Skeleton) components.push_back(ComponentTypeId::Skeleton);
    if (data->Skinning) components.push_back(ComponentTypeId::Skinning);
    if (data->BlendShapes) components.push_back(ComponentTypeId::BlendShape);
    if (data->UnifiedMorph) components.push_back(ComponentTypeId::UnifiedMorph);
    if (data->Collider) components.push_back(ComponentTypeId::Collider);
    if (data->RigidBody) components.push_back(ComponentTypeId::RigidBody);
    if (data->StaticBody) components.push_back(ComponentTypeId::StaticBody);
    if (data->Softbody) components.push_back(ComponentTypeId::Softbody);
    if (data->CharacterController) components.push_back(ComponentTypeId::CharacterController);
    if (data->Terrain) components.push_back(ComponentTypeId::Terrain);
    if (data->Emitter) components.push_back(ComponentTypeId::ParticleEmitter);
    if (data->AnimationPlayer) components.push_back(ComponentTypeId::AnimationPlayer);
    if (data->BoneAttachment) components.push_back(ComponentTypeId::BoneAttachment);
    if (data->TintController) components.push_back(ComponentTypeId::TintController);
    if (data->GrassDeformer) components.push_back(ComponentTypeId::GrassDeformer);
    if (data->River) components.push_back(ComponentTypeId::River);
    if (data->Spline) components.push_back(ComponentTypeId::Spline);
    if (data->Area) components.push_back(ComponentTypeId::Area);
    if (data->RenderOverrides) components.push_back(ComponentTypeId::RenderOverrides);
    // UI Components
    if (data->Canvas) components.push_back(ComponentTypeId::Canvas);
    if (data->Panel) components.push_back(ComponentTypeId::Panel);
    if (data->Button) components.push_back(ComponentTypeId::Button);
    if (data->Slider) components.push_back(ComponentTypeId::Slider);
    if (data->ProgressBar) components.push_back(ComponentTypeId::ProgressBar);
    if (data->Toggle) components.push_back(ComponentTypeId::Toggle);
    if (data->ScrollView) components.push_back(ComponentTypeId::ScrollView);
    if (data->LayoutGroup) components.push_back(ComponentTypeId::LayoutGroup);
    if (data->InputField) components.push_back(ComponentTypeId::InputField);
    if (data->Dropdown) components.push_back(ComponentTypeId::Dropdown);
    if (data->Text) components.push_back(ComponentTypeId::Text);
    // Navigation
    if (data->Navigation) components.push_back(ComponentTypeId::NavMesh);
    if (data->NavAgent) components.push_back(ComponentTypeId::NavAgent);
    if (data->NavLink) components.push_back(ComponentTypeId::NavLink);
    // Animation constraints
    if (!data->IKs.empty()) components.push_back(ComponentTypeId::IKConstraint);
    if (!data->LookAtConstraints.empty()) components.push_back(ComponentTypeId::LookAtConstraint);
    // Instancer
    if (data->Instancer) components.push_back(ComponentTypeId::Instancer);
    // Scripts
    if (!data->Scripts.empty()) components.push_back(ComponentTypeId::Script);
    // Module components (Dynamic map)
    if (!data->Dynamic.empty()) components.push_back(ComponentTypeId::Module);
    // UI Rect, FitToContent, ResourceLayers
    if (data->UIRect) components.push_back(ComponentTypeId::UIRect);
    if (data->FitToContent) components.push_back(ComponentTypeId::FitToContent);
    if (data->UISceneCapture) components.push_back(ComponentTypeId::UISceneCapture);
    if (data->ResourceLayers) components.push_back(ComponentTypeId::ResourceLayers);
    if (data->AudioSource) components.push_back(ComponentTypeId::AudioSource);
    if (data->AudioListener) components.push_back(ComponentTypeId::AudioListener);
    
    return components;
}

size_t WriteComponentBinary(ComponentWriteContext& ctx, const EntityData* data, ComponentTypeId typeId) {
    DebugAssertPrefabWriterVersion();
    size_t startPos = ctx.Position();
    
    switch (typeId) {
        case ComponentTypeId::Transform: {
            const auto& t = data->Transform;
            float pos[3] = {t.Position.x, t.Position.y, t.Position.z};
            // Use quaternion (w,x,y,z order)
            float rot[4] = {t.RotationQ.w, t.RotationQ.x, t.RotationQ.y, t.RotationQ.z};
            float scale[3] = {t.Scale.x, t.Scale.y, t.Scale.z};
            ctx.Write(pos, 12);
            ctx.Write(rot, 16);
            ctx.Write(scale, 12);
            break;
        }
        
        case ComponentTypeId::Mesh: {
            const auto& m = *data->Mesh;
            // Write mesh reference (GUID + fileID + type)
            ctx.Write(m.meshReference.guid.high);
            ctx.Write(m.meshReference.guid.low);
            ctx.Write(m.meshReference.fileID);
            ctx.Write(m.meshReference.type);
            
            // Write material paths count + paths
            uint32_t matCount = static_cast<uint32_t>(m.MaterialAssetPaths.size());
            ctx.Write(matCount);
            
            for (const auto& matPath : m.MaterialAssetPaths) {
                uint32_t pathIdx = ctx.AddString(matPath);
                ctx.Write(pathIdx);
            }
            
            // Collect inline materials
            std::vector<InlineMaterialData> pbrMaterials;
            std::vector<ShaderGraphMaterialData> shaderGraphMaterials;
            
#ifndef CLAYMORE_RUNTIME
            // Process all material slots from the materials vector
            for (size_t i = 0; i < m.materials.size(); ++i) {
                auto mat = m.materials[i];
                if (!mat) continue;
                
                // Check for ShaderGraphMaterial first
                auto sgMat = std::dynamic_pointer_cast<shadergraph::ShaderGraphMaterial>(mat);
                if (sgMat && !sgMat->GetShaderGraphPath().empty()) {
                    ShaderGraphMaterialData sgData;
                    sgData.slotIndex = static_cast<uint32_t>(i);
                    sgData.shaderGraphPath = sgMat->GetShaderGraphPath();
                    sgData.name = sgMat->GetName();
                    sgData.uvScale = sgMat->GetUVScale();
                    sgData.uvOffset = sgMat->GetUVOffset();
                    sgData.stateFlags = sgMat->GetStateFlags();
                    
                    // Get compiled shader names
                    shadergraph::ShaderGraph graph;
                    if (shadergraph::ShaderGraphSerializer::LoadFromFile(sgData.shaderGraphPath, graph)) {
                        if (graph.isCompiled && !graph.compiledVSPath.empty() && !graph.compiledFSPath.empty()) {
                            std::filesystem::path vsPath(graph.compiledVSPath);
                            std::filesystem::path fsPath(graph.compiledFSPath);
                            sgData.compiledVSName = vsPath.stem().string();
                            sgData.compiledFSName = fsPath.stem().string();
                        } else {
                            std::string baseName = std::filesystem::path(sgData.shaderGraphPath).stem().string();
                            std::replace(baseName.begin(), baseName.end(), ' ', '_');
                            baseName.erase(std::remove_if(baseName.begin(), baseName.end(), 
                                [](char c) { return !std::isalnum(c) && c != '_'; }), baseName.end());
                            sgData.compiledVSName = "vs_shgraph_" + baseName;
                            sgData.compiledFSName = "fs_shgraph_" + baseName;
                        }
                    }
                    
                    // Copy parameters
                    for (const auto& param : sgMat->GetParameters()) {
                        ShaderGraphParamData pd;
                        pd.name = param.name;
                        pd.displayName = param.displayName;
                        pd.type = static_cast<uint32_t>(param.type);
                        pd.value = param.value;
                        pd.texturePath = param.texturePath;
                        pd.textureSlot = param.textureSlot;
                        sgData.parameters.push_back(pd);
                    }
                    
                    shaderGraphMaterials.push_back(sgData);
                    continue;
                }
                
                // Check for PBR material
                auto pbr = std::dynamic_pointer_cast<PBRMaterial>(mat);
                if (pbr) {
                    InlineMaterialData inl;
                    inl.slotIndex = static_cast<uint32_t>(i);
                    
                    // Detect PSX material
                    glm::vec4 psxParams(0.0f), psxWorld(0.0f), toonParams(0.0f);
                    bool hasPsxParams = pbr->TryGetUniform("u_psxParams", psxParams);
                    bool hasPsxWorld = pbr->TryGetUniform("u_psxWorld", psxWorld);
                    bool hasToonParams = pbr->TryGetUniform("u_toonParams", toonParams);
                    
                    if (hasPsxParams || hasPsxWorld || hasToonParams) {
                        inl.materialType = InlineMaterialType::PSX;
                        inl.psxParams = psxParams;
                        inl.psxWorld = psxWorld;
                        inl.toonParams = toonParams;
                    } else {
                        inl.materialType = InlineMaterialType::PBR;
                    }
                    
                    // Common PBR properties
                    inl.albedoPath = pbr->GetAlbedoPath();
                    inl.metallicRoughnessPath = pbr->GetMetallicRoughnessPath();
                    inl.normalPath = pbr->GetNormalPath();
                    inl.aoPath = pbr->GetAOPath();
                    inl.emissionPath = pbr->GetEmissionPath();
                    inl.displacementPath = pbr->GetDisplacementPath();
                    inl.tintMaskPath = pbr->GetTintMaskPath();
                    inl.metallic = pbr->GetMetallic();
                    inl.roughness = pbr->GetRoughness();
                    inl.normalScale = pbr->GetNormalScale();
                    inl.aoStrength = pbr->GetAmbientOcclusion();
                    inl.emissionStrength = pbr->GetEmissionStrength();
                    inl.emissionColor = pbr->GetEmissionColor();
                    inl.displacementScale = pbr->GetDisplacementScale();
                    inl.uvScale = pbr->GetUVScale();
                    inl.uvOffset = pbr->GetUVOffset();
                    pbr->TryGetUniform("u_ColorTint", inl.tint);
                    inl.hasAlpha = (pbr->GetStateFlags() & BGFX_STATE_BLEND_ALPHA) != 0;
                    inl.receiveShadowsOverride = pbr->GetReceiveShadowsOverride();
                    inl.receiveShadows = pbr->GetReceiveShadows();
                    
                    const bool hasPbrState =
                        !inl.albedoPath.empty() ||
                        !inl.metallicRoughnessPath.empty() ||
                        !inl.normalPath.empty() ||
                        !inl.aoPath.empty() ||
                        !inl.emissionPath.empty() ||
                        !inl.displacementPath.empty() ||
                        !inl.tintMaskPath.empty() ||
                        inl.tint != glm::vec4(1.0f) ||
                        std::abs(inl.metallic - PBRMaterial::kDefaultMetallic) > 1e-4f ||
                        std::abs(inl.roughness - PBRMaterial::kDefaultRoughness) > 1e-4f ||
                        std::abs(inl.normalScale - PBRMaterial::kDefaultNormalScale) > 1e-4f ||
                        std::abs(inl.aoStrength - PBRMaterial::kDefaultAO) > 1e-4f ||
                        std::abs(inl.emissionStrength - PBRMaterial::kDefaultEmissionStrength) > 1e-4f ||
                        std::abs(inl.displacementScale - PBRMaterial::kDefaultDisplacementScale) > 1e-4f ||
                        inl.emissionColor != glm::vec3(1.0f) ||
                        inl.uvScale != glm::vec2(1.0f) ||
                        inl.uvOffset != glm::vec2(0.0f) ||
                        inl.hasAlpha ||
                        inl.receiveShadowsOverride ||
                        inl.materialType == InlineMaterialType::PSX;
                    if (hasPbrState) {
                        pbrMaterials.push_back(inl);
                    }
                }
            }
            
            // Fallback: Check InlineMaterials and SlotPropertyBlockTexturePaths for PBR data
            if (pbrMaterials.empty() && shaderGraphMaterials.empty()) {
                for (const auto& inl : m.InlineMaterials) {
                    if (!inl.albedoPath.empty() || !inl.metallicRoughnessPath.empty() || 
                        !inl.normalPath.empty() || !inl.aoPath.empty() || !inl.emissionPath.empty() ||
                        !inl.displacementPath.empty() || !inl.tintMaskPath.empty() ||
                        inl.tint != glm::vec4(1.0f) ||
                        std::abs(inl.metallic - PBRMaterial::kDefaultMetallic) > 1e-4f ||
                        std::abs(inl.roughness - PBRMaterial::kDefaultRoughness) > 1e-4f ||
                        std::abs(inl.normalScale - PBRMaterial::kDefaultNormalScale) > 1e-4f ||
                        std::abs(inl.aoStrength - PBRMaterial::kDefaultAO) > 1e-4f ||
                        std::abs(inl.emissionStrength - PBRMaterial::kDefaultEmissionStrength) > 1e-4f ||
                        std::abs(inl.displacementScale - PBRMaterial::kDefaultDisplacementScale) > 1e-4f ||
                        inl.emissionColor != glm::vec3(1.0f) ||
                        inl.uvScale != glm::vec2(1.0f) ||
                        inl.uvOffset != glm::vec2(0.0f) ||
                        inl.hasAlpha || inl.receiveShadowsOverride ||
                        inl.materialType == InlineMaterialType::PSX) {
                        pbrMaterials.push_back(inl);
                    }
                }

                if (pbrMaterials.empty()) {
                    for (size_t i = 0; i < m.SlotPropertyBlockTexturePaths.size(); ++i) {
                        const auto& texPaths = m.SlotPropertyBlockTexturePaths[i];
                        if (texPaths.empty()) continue;

                        InlineMaterialData inl;
                        inl.slotIndex = static_cast<uint32_t>(i);
                        inl.materialType = InlineMaterialType::PBR;

                        auto it = texPaths.find("s_albedo");
                        if (it != texPaths.end()) inl.albedoPath = it->second;
                        it = texPaths.find("s_metallicRoughness");
                        if (it != texPaths.end()) inl.metallicRoughnessPath = it->second;
                        it = texPaths.find("s_normalMap");
                        if (it != texPaths.end()) inl.normalPath = it->second;

                        if (i < m.SlotPropertyBlocks.size()) {
                            glm::vec4 tint(1.0f);
                            if (m.SlotPropertyBlocks[i].TryGetVector(PropertyID::Get("u_ColorTint"), tint)) {
                                inl.tint = tint;
                            }
                        }

                        if (!inl.albedoPath.empty() || !inl.metallicRoughnessPath.empty() || !inl.normalPath.empty()) {
                            pbrMaterials.push_back(inl);
                        }
                    }
                }
            }
#endif
            
            // Write PBR/PSX materials
            uint32_t pbrMatCount = static_cast<uint32_t>(pbrMaterials.size());
            ctx.Write(pbrMatCount);
            
            for (const auto& inl : pbrMaterials) {
                ctx.Write(inl.slotIndex);
                ctx.Write(static_cast<uint8_t>(inl.materialType));
                ctx.Write(ctx.AddString(inl.albedoPath));
                ctx.Write(ctx.AddString(inl.metallicRoughnessPath));
                ctx.Write(ctx.AddString(inl.normalPath));
                ctx.Write(ctx.AddString(inl.aoPath));
                ctx.Write(ctx.AddString(inl.emissionPath));
                ctx.Write(ctx.AddString(inl.displacementPath));
                ctx.Write(ctx.AddString(inl.tintMaskPath));
                ctx.Write(inl.metallic);
                ctx.Write(inl.roughness);
                ctx.Write(inl.normalScale);
                ctx.Write(inl.aoStrength);
                ctx.Write(inl.emissionStrength);
                ctx.Write(inl.displacementScale);
                ctx.Write(inl.emissionColor.x);
                ctx.Write(inl.emissionColor.y);
                ctx.Write(inl.emissionColor.z);
                ctx.Write(inl.uvScale.x);
                ctx.Write(inl.uvScale.y);
                ctx.Write(inl.uvOffset.x);
                ctx.Write(inl.uvOffset.y);
                ctx.Write(inl.tint.x);
                ctx.Write(inl.tint.y);
                ctx.Write(inl.tint.z);
                ctx.Write(inl.tint.w);
                ctx.Write(static_cast<uint8_t>(inl.hasAlpha ? 1 : 0));
                ctx.Write(static_cast<uint8_t>(inl.receiveShadowsOverride ? 1 : 0));
                ctx.Write(static_cast<uint8_t>(inl.receiveShadows ? 1 : 0));
                
                if (inl.materialType == InlineMaterialType::PSX) {
                    ctx.Write(inl.psxParams.x);
                    ctx.Write(inl.psxParams.y);
                    ctx.Write(inl.psxParams.z);
                    ctx.Write(inl.psxParams.w);
                    ctx.Write(inl.psxWorld.x);
                    ctx.Write(inl.psxWorld.y);
                    ctx.Write(inl.psxWorld.z);
                    ctx.Write(inl.psxWorld.w);
                    ctx.Write(inl.toonParams.x);
                    ctx.Write(inl.toonParams.y);
                    ctx.Write(inl.toonParams.z);
                    ctx.Write(inl.toonParams.w);
                }
            }
            
            // Write ShaderGraph materials
            uint32_t sgMatCount = static_cast<uint32_t>(shaderGraphMaterials.size());
            ctx.Write(sgMatCount);
            
            for (const auto& sg : shaderGraphMaterials) {
                ctx.Write(sg.slotIndex);
                ctx.Write(ctx.AddString(sg.shaderGraphPath));
                ctx.Write(ctx.AddString(sg.name));
                ctx.Write(ctx.AddString(sg.compiledVSName));
                ctx.Write(ctx.AddString(sg.compiledFSName));
                ctx.Write(sg.uvScale.x);
                ctx.Write(sg.uvScale.y);
                ctx.Write(sg.uvOffset.x);
                ctx.Write(sg.uvOffset.y);
                ctx.Write(sg.stateFlags);
                ctx.Write(static_cast<uint8_t>(sg.twoSided ? 1 : 0));
                ctx.Write(static_cast<uint8_t>(sg.alphaClip ? 1 : 0));
                ctx.Write(sg.alphaClipThreshold);
                
                uint32_t paramCount = static_cast<uint32_t>(sg.parameters.size());
                ctx.Write(paramCount);
                
                for (const auto& p : sg.parameters) {
                    ctx.Write(ctx.AddString(p.name));
                    ctx.Write(ctx.AddString(p.displayName));
                    ctx.Write(p.type);
                    ctx.Write(p.value.x);
                    ctx.Write(p.value.y);
                    ctx.Write(p.value.z);
                    ctx.Write(p.value.w);
                    ctx.Write(ctx.AddString(p.texturePath));
                    ctx.Write(p.textureSlot);
                }
            }

            uint32_t slotBlockCount = static_cast<uint32_t>(std::max(
                m.SlotPropertyBlocks.size(),
                m.SlotPropertyBlockTexturePaths.size()));
            ctx.Write(slotBlockCount);
            for (size_t i = 0; i < slotBlockCount; ++i) {
                if (i < m.SlotPropertyBlocks.size()) {
                    WritePropertyBlock(ctx, m.SlotPropertyBlocks[i]);
                } else {
                    WritePropertyBlock(ctx, MaterialPropertyBlock{});
                }

                if (i < m.SlotPropertyBlockTexturePaths.size()) {
                    WriteTexturePathMap(ctx, m.SlotPropertyBlockTexturePaths[i]);
                } else {
                    uint32_t zero = 0;
                    ctx.Write(zero);
                }
            }

            WritePropertyBlock(ctx, m.PropertyBlock);
            WriteTexturePathMap(ctx, m.PropertyBlockTexturePaths);
            break;
        }
        
        case ComponentTypeId::Light: {
            const auto& l = *data->Light;
            uint32_t type = static_cast<uint32_t>(l.Type);
            float color[3] = {l.Color.r, l.Color.g, l.Color.b};
            ctx.Write(type);
            ctx.Write(color, 12);
            ctx.Write(l.Intensity);
            ctx.Write(l.Range);
            ctx.Write(l.SpotInnerAngleDegrees);
            ctx.Write(l.SpotOuterAngleDegrees);
            ctx.Write(static_cast<uint8_t>(l.PointShadowsEnabled ? 1 : 0));
            break;
        }
        
        case ComponentTypeId::Camera: {
            const auto& c = *data->Camera;
            ctx.Write(c.FieldOfView);
            ctx.Write(c.NearClip);
            ctx.Write(c.FarClip);
            uint8_t isActive = c.Active ? 1 : 0;
            ctx.Write(isActive);
            ctx.Write(static_cast<int32_t>(c.priority));
            ctx.Write(c.LayerMask);
            ctx.Write(static_cast<uint8_t>(c.IsPerspective ? 1 : 0));
            break;
        }

        case ComponentTypeId::AudioSource: {
            const auto& a = *data->AudioSource;
            ctx.Write(a.AudioClip.guid.high);
            ctx.Write(a.AudioClip.guid.low);
            ctx.Write(ctx.AddString(a.AudioPath));
            ctx.Write(a.Volume);
            ctx.Write(a.Pitch);
            ctx.Write(static_cast<uint8_t>(a.Loop ? 1 : 0));
            ctx.Write(static_cast<uint8_t>(a.PlayOnAwake ? 1 : 0));
            ctx.Write(static_cast<uint8_t>(a.Mute ? 1 : 0));
            ctx.Write(static_cast<uint8_t>(a.Spatial ? 1 : 0));
            ctx.Write(a.MinDistance);
            ctx.Write(a.MaxDistance);
            ctx.Write(a.DopplerFactor);
            ctx.Write(a.Rolloff);
            break;
        }

        case ComponentTypeId::AudioListener: {
            const auto& l = *data->AudioListener;
            ctx.Write(static_cast<uint8_t>(l.Active ? 1 : 0));
            ctx.Write(static_cast<int32_t>(l.Priority));
            ctx.Write(l.VolumeMultiplier);
            break;
        }
        
        case ComponentTypeId::Skeleton: {
            const auto& s = *data->Skeleton;
            uint32_t boneCount = static_cast<uint32_t>(s.InverseBindPoses.size());
            ctx.Write(boneCount);
            
            for (const auto& mat : s.InverseBindPoses) {
                ctx.Write(&mat[0][0], 64);
            }
            
            for (int parent : s.BoneParents) {
                ctx.Write(static_cast<int32_t>(parent));
            }
            
            for (const auto& name : s.BoneNames) {
                uint32_t nameIdx = ctx.AddString(name);
                ctx.Write(nameIdx);
            }
            
            for (EntityID boneId : s.BoneEntities) {
                ctx.Write(static_cast<uint32_t>(boneId));
            }
            
            // Avatar data
            uint8_t hasAvatar = (s.Avatar != nullptr) ? 1 : 0;
            ctx.Write(hasAvatar);
            
            if (s.Avatar) {
                const auto& avatar = *s.Avatar;
                
                uint32_t rigNameIdx = ctx.AddString(avatar.RigName);
                ctx.Write(rigNameIdx);
                
                ctx.Write(static_cast<uint8_t>(avatar.Axes.Up));
                ctx.Write(static_cast<uint8_t>(avatar.Axes.Forward));
                ctx.Write(static_cast<uint8_t>(avatar.Axes.RightHanded ? 1 : 0));
                ctx.Write(avatar.UnitsPerMeter);
                
                uint32_t humanoidCount = static_cast<uint32_t>(avatar.Map.size());
                ctx.Write(humanoidCount);
                
                for (const auto& entry : avatar.Map) {
                    ctx.Write(static_cast<uint16_t>(entry.Bone));
                    ctx.Write(entry.BoneIndex);
                    uint32_t boneNameIdx = ctx.AddString(entry.BoneName);
                    ctx.Write(boneNameIdx);
                }
                
                uint32_t bindModelCount = static_cast<uint32_t>(avatar.BindModel.size());
                ctx.Write(bindModelCount);
                for (const auto& mat : avatar.BindModel) {
                    ctx.Write(&mat[0][0], 64);
                }
                
                uint32_t bindLocalCount = static_cast<uint32_t>(avatar.BindLocal.size());
                ctx.Write(bindLocalCount);
                for (const auto& mat : avatar.BindLocal) {
                    ctx.Write(&mat[0][0], 64);
                }
                
                uint32_t presentCount = static_cast<uint32_t>(avatar.Present.size());
                ctx.Write(presentCount);
                for (bool present : avatar.Present) {
                    ctx.Write(static_cast<uint8_t>(present ? 1 : 0));
                }
                
                uint32_t restOffsetCount = static_cast<uint32_t>(avatar.RestOffsetRot.size());
                ctx.Write(restOffsetCount);
                for (const auto& q : avatar.RestOffsetRot) {
                    ctx.Write(q.w);
                    ctx.Write(q.x);
                    ctx.Write(q.y);
                    ctx.Write(q.z);
                }
                
                uint32_t retargetCount = static_cast<uint32_t>(avatar.RetargetModel.size());
                ctx.Write(retargetCount);
                for (const auto& mat : avatar.RetargetModel) {
                    ctx.Write(&mat[0][0], 64);
                }
            }
            break;
        }
        
        case ComponentTypeId::Skinning: {
            const auto& sk = *data->Skinning;
            ctx.Write(static_cast<uint32_t>(sk.SkeletonRoot));
            uint8_t useParentSkel = sk.UseParentSkeleton ? 1 : 0;
            ctx.Write(useParentSkel);
            
            uint32_t boneNameCount = static_cast<uint32_t>(sk.OriginalBoneNames.size());
            ctx.Write(boneNameCount);
            for (const auto& name : sk.OriginalBoneNames) {
                uint32_t nameIdx = ctx.AddString(name);
                ctx.Write(nameIdx);
            }
            
            uint32_t ibpCount = static_cast<uint32_t>(sk.OriginalInverseBindPoses.size());
            ctx.Write(ibpCount);
            for (const auto& mat : sk.OriginalInverseBindPoses) {
                ctx.Write(&mat[0][0], 64);
            }
            break;
        }
        
        case ComponentTypeId::Collider: {
            const auto& c = *data->Collider;
            uint32_t shapeType = static_cast<uint32_t>(c.ShapeType);
            float offset[3] = {c.Offset.x, c.Offset.y, c.Offset.z};
            float size[3] = {c.Size.x, c.Size.y, c.Size.z};
            ctx.Write(shapeType);
            ctx.Write(offset, 12);
            ctx.Write(size, 12);
            ctx.Write(c.Radius);
            ctx.Write(c.Height);
            uint8_t isTrigger = c.IsTrigger ? 1 : 0;
            ctx.Write(isTrigger);
            // v5+: meshPath and physicsLayer for full parity
            uint32_t meshPathIdx = ctx.AddString(c.MeshPath);
            ctx.Write(meshPathIdx);
            uint32_t layerNameIdx = ctx.AddString(c.PhysicsLayerName);
            ctx.Write(layerNameIdx);
            break;
        }
        
        case ComponentTypeId::RigidBody: {
            const auto& rb = *data->RigidBody;
            ctx.Write(rb.Mass);
            ctx.Write(rb.Friction);
            ctx.Write(rb.Restitution);
            uint8_t useGravity = rb.UseGravity ? 1 : 0;
            uint8_t isKinematic = rb.IsKinematic ? 1 : 0;
            ctx.Write(useGravity);
            ctx.Write(isKinematic);
            // v5+: Write physics layer name
            uint32_t layerNameIdx = ctx.AddString(rb.PhysicsLayerName);
            ctx.Write(layerNameIdx);
            // v5+: Initial velocities
            ctx.Write(rb.LinearVelocity.x);
            ctx.Write(rb.LinearVelocity.y);
            ctx.Write(rb.LinearVelocity.z);
            ctx.Write(rb.AngularVelocity.x);
            ctx.Write(rb.AngularVelocity.y);
            ctx.Write(rb.AngularVelocity.z);
            ctx.Write(rb.CollisionMask);
            break;
        }
        
        case ComponentTypeId::StaticBody: {
            const auto& sb = *data->StaticBody;
            ctx.Write(sb.Friction);
            ctx.Write(sb.Restitution);
            uint32_t layerNameIdx = ctx.AddString(sb.PhysicsLayerName);
            ctx.Write(layerNameIdx);
            break;
        }

        case ComponentTypeId::Softbody: {
            const auto& sb = *data->Softbody;
            ctx.Write(sb.Enabled ? uint8_t(1) : uint8_t(0));
            ctx.Write(sb.SolverIterations);
            ctx.Write(sb.LinearDamping);
            ctx.Write(sb.Friction);
            ctx.Write(sb.Restitution);
            ctx.Write(sb.Pressure);
            ctx.Write(sb.GravityFactor);
            ctx.Write(sb.VertexRadius);
            ctx.Write(sb.MaxLinearVelocity);
            ctx.Write(sb.EdgeCompliance);
            ctx.Write(sb.ShearCompliance);
            ctx.Write(sb.BendCompliance);
            ctx.Write(sb.EnableLongRangeAttachments ? uint8_t(1) : uint8_t(0));
            ctx.Write(sb.LRAMaxDistanceMultiplier);
            ctx.Write(static_cast<uint32_t>(sb.BendMode));
            ctx.Write(sb.FacesDoubleSided ? uint8_t(1) : uint8_t(0));
            ctx.Write(sb.WeightFloor);
            uint32_t layerNameIdx = ctx.AddString(sb.PhysicsLayerName);
            ctx.Write(layerNameIdx);
            ctx.Write(sb.SourceVertexCount);
            ctx.Write(sb.SourceIndexCount);
            uint32_t weightCount = static_cast<uint32_t>(sb.VertexWeights.size());
            ctx.Write(weightCount);
            for (float weight : sb.VertexWeights) {
                ctx.Write(weight);
            }
            uint32_t anchorCount = static_cast<uint32_t>(sb.AnchorVertices.size());
            ctx.Write(anchorCount);
            if (anchorCount > 0) {
                ctx.Write(sb.AnchorVertices.data(), anchorCount);
            }
            break;
        }
        
        case ComponentTypeId::CharacterController: {
            const auto& cc = *data->CharacterController;
            ctx.Write(cc.Radius);
            ctx.Write(cc.Height);
            ctx.Write(cc.Up.x);
            ctx.Write(cc.Up.y);
            ctx.Write(cc.Up.z);
            ctx.Write(cc.Offset.x);
            ctx.Write(cc.Offset.y);
            ctx.Write(cc.Offset.z);
            ctx.Write(cc.MaxSlopeDegrees);
            ctx.Write(cc.JumpSpeed);
            ctx.Write(cc.StickToFloor ? uint8_t(1) : uint8_t(0));
            ctx.Write(cc.EnableWalkStairs ? uint8_t(1) : uint8_t(0));
            uint32_t layerNameIdx = ctx.AddString(cc.PhysicsLayerName);
            ctx.Write(layerNameIdx);
            ctx.Write(cc.CollisionMask);
            break;
        }

        case ComponentTypeId::Terrain: {
            const auto& t = *data->Terrain;
            uint32_t assetPathIdx = ctx.AddString(t.AssetPath);
            ctx.Write(assetPathIdx);
            ctx.Write(t.GridResolution);
            ctx.Write(t.WorldSize.x);
            ctx.Write(t.WorldSize.y);
            ctx.Write(t.MaxHeight);
            
            ctx.Write(t.UseClipmaps ? uint8_t(1) : uint8_t(0));
            ctx.Write(t.ClipmapLevels);
            ctx.Write(t.ClipmapGridSize);
            ctx.Write(t.ClipmapMorphing ? uint8_t(1) : uint8_t(0));
            
            // Chunked terrain settings (Skyrim-style cells with unified textures)
            ctx.Write(t.UseChunkedTerrain ? uint8_t(1) : uint8_t(0));
            ctx.Write(t.ChunkVertexSize);
            ctx.Write(t.ChunkMorphing ? uint8_t(1) : uint8_t(0));
            ctx.Write(t.ChunkMorphRegion);
            ctx.Write(t.ChunkStreaming ? uint8_t(1) : uint8_t(0));
            ctx.Write(t.StreamingLoadRadius);
            ctx.Write(t.StreamingUnloadRadius);
            
            uint32_t layerCount = static_cast<uint32_t>(t.Layers.size());
            ctx.Write(layerCount);
            for (const auto& layer : t.Layers) {
                uint32_t nameIdx = ctx.AddString(layer.Name);
                uint32_t albedoIdx = ctx.AddString(layer.AlbedoPath);
                uint32_t normalIdx = ctx.AddString(layer.NormalPath);
                ctx.Write(nameIdx);
                ctx.Write(albedoIdx);
                ctx.Write(normalIdx);
                ctx.Write(layer.Tiling);
                ctx.Write(layer.PlaceholderColor.x);
                ctx.Write(layer.PlaceholderColor.y);
                ctx.Write(layer.PlaceholderColor.z);
                ctx.Write(layer.NavCost);
            }

            // Grass layers (metadata only; masks are stored in the terrain asset)
            uint32_t grassLayerCount = static_cast<uint32_t>(t.GrassLayers.size());
            ctx.Write(grassLayerCount);
            for (const auto& grass : t.GrassLayers) {
                ctx.Write(grass.Guid.high);
                ctx.Write(grass.Guid.low);

                uint32_t nameIdx = ctx.AddString(grass.Name);
                ctx.Write(nameIdx);

                uint8_t enabled = grass.Enabled ? uint8_t(1) : uint8_t(0);
                uint8_t useGpu = grass.UseGPU ? uint8_t(1) : uint8_t(0);
                uint8_t renderMode = static_cast<uint8_t>(grass.RenderMode);
                uint8_t mask = static_cast<uint8_t>(grass.Mask);
                ctx.Write(enabled);
                ctx.Write(useGpu);
                ctx.Write(renderMode);
                ctx.Write(mask);

                ctx.Write(grass.SplatSeed);
                ctx.Write(grass.SplatNoiseScale);
                ctx.Write(grass.SplatNoiseStrength);
                ctx.Write(grass.SplatThreshold);
                ctx.Write(grass.DensityPerSquareMeter);
                ctx.Write(grass.ScaleRange.x);
                ctx.Write(grass.ScaleRange.y);
                ctx.Write(grass.RandomYawDegrees);
                ctx.Write(grass.HeightRange.x);
                ctx.Write(grass.HeightRange.y);
                ctx.Write(grass.MaxSlopeDegrees);
                ctx.Write(grass.MinDistance);
                ctx.Write(grass.MaxDistance);
                ctx.Write(grass.WindStrength);
                ctx.Write(grass.WindDirectionDegrees);
                ctx.Write(grass.BaseColor.x);
                ctx.Write(grass.BaseColor.y);
                ctx.Write(grass.BaseColor.z);
                ctx.Write(grass.ColorVariance.x);
                ctx.Write(grass.ColorVariance.y);
                ctx.Write(grass.ColorVariance.z);

                uint32_t textureIdx = ctx.AddString(grass.BillboardTexturePath);
                uint32_t meshPathIdx = ctx.AddString(grass.MeshPath);
                ctx.Write(textureIdx);
                ctx.Write(meshPathIdx);
                ctx.Write(grass.MeshAsset.guid.high);
                ctx.Write(grass.MeshAsset.guid.low);
            }

            ctx.Write(t.GrassChunkResolution);
            ctx.Write(t.GrassSamplingMultiplier);
            
            // v9+: Persist terrain asset GUID + texture array settings for parity
            ctx.Write(t.TerrainDataGuid.high);
            ctx.Write(t.TerrainDataGuid.low);
            ctx.Write(t.LayerTextureResolution);
            ctx.Write(static_cast<uint32_t>(t.LayerResizeFilter));

            // v28+: Terrain instancer layer metadata. Painted masks live in the terrain asset.
            uint32_t instancerLayerCount = static_cast<uint32_t>(t.InstancerLayers.size());
            ctx.Write(instancerLayerCount);
            for (const auto& layer : t.InstancerLayers) {
                ctx.Write(layer.Guid.high);
                ctx.Write(layer.Guid.low);
                uint32_t nameIdx = ctx.AddString(layer.Name);
                uint32_t physicsLayerNameIdx = ctx.AddString(layer.Collision.PhysicsLayerName);
                uint32_t instancerJsonIdx = ctx.AddString(::Serializer::SerializeInstancer(layer.Instancer).dump());
                ctx.Write(nameIdx);
                ctx.Write(layer.Enabled ? uint8_t(1) : uint8_t(0));
                ctx.Write(static_cast<uint8_t>(layer.Mask));
                ctx.Write(layer.SplatThreshold);
                ctx.Write(layer.Collision.Enabled ? uint8_t(1) : uint8_t(0));
                ctx.Write(layer.Collision.ActivationDistance);
                ctx.Write(layer.Collision.MaxActiveBodies);
                ctx.Write(layer.Collision.PhysicsLayer);
                ctx.Write(physicsLayerNameIdx);
                ctx.Write(layer.Collision.UseSharedMeshShape ? uint8_t(1) : uint8_t(0));
                ctx.Write(instancerJsonIdx);
            }
            break;
        }
        
        case ComponentTypeId::ParticleEmitter: {
            const auto& pe = *data->Emitter;
            
            ctx.Write(pe.MaxParticles);
            ctx.Write(pe.Enabled ? uint8_t(1) : uint8_t(0));
            uint32_t spritePathIdx = ctx.AddString(pe.SpritePath);
            ctx.Write(spritePathIdx);
            
            ctx.Write(pe.Duration);
            ctx.Write(pe.Looping ? uint8_t(1) : uint8_t(0));
            ctx.Write(pe.Prewarm ? uint8_t(1) : uint8_t(0));
            ctx.Write(pe.PlayOnAwake ? uint8_t(1) : uint8_t(0));
            ctx.Write(pe.DestroyOnComplete ? uint8_t(1) : uint8_t(0));
            
            ctx.Write(pe.EmissionRate);
            ctx.Write(pe.BurstEnabled ? uint8_t(1) : uint8_t(0));
            ctx.Write(static_cast<int32_t>(pe.BurstCount));
            ctx.Write(pe.BurstTime);
            ctx.Write(static_cast<int32_t>(pe.BurstCycles));
            ctx.Write(pe.BurstInterval);
            
            ctx.Write(static_cast<int32_t>(pe.Shape));
            ctx.Write(pe.ShapeRadius);
            ctx.Write(pe.ShapeRadiusThickness);
            ctx.Write(pe.ShapeAngle);
            ctx.Write(pe.ShapeArc);
            ctx.Write(pe.ShapeScale.x);
            ctx.Write(pe.ShapeScale.y);
            ctx.Write(pe.ShapeScale.z);
            ctx.Write(pe.ShapeLength);
            ctx.Write(pe.ShapeEmitFromEdge ? uint8_t(1) : uint8_t(0));
            ctx.Write(pe.ShapeRandomizeDirection ? uint8_t(1) : uint8_t(0));
            
            ctx.Write(pe.Lifetime.Min);
            ctx.Write(pe.Lifetime.Max);
            
            ctx.Write(pe.StartSpeed.Min);
            ctx.Write(pe.StartSpeed.Max);
            ctx.Write(pe.StartSize.Min);
            ctx.Write(pe.StartSize.Max);
            ctx.Write(pe.StartRotation.Min);
            ctx.Write(pe.StartRotation.Max);
            ctx.Write(pe.StartColor.r);
            ctx.Write(pe.StartColor.g);
            ctx.Write(pe.StartColor.b);
            ctx.Write(pe.StartColor.a);
            
            ctx.Write(pe.GravityModifier);
            ctx.Write(static_cast<int32_t>(pe.SimulationSpace));
            ctx.Write(pe.InheritVelocity);
            ctx.Write(pe.DragCoefficient);
            
            ctx.Write(pe.SizeOverLifetimeEnabled ? uint8_t(1) : uint8_t(0));
            ctx.Write(static_cast<int32_t>(pe.SizeOverLifetime.CurveType));
            ctx.Write(pe.SizeOverLifetime.StartValue);
            ctx.Write(pe.SizeOverLifetime.EndValue);
            
            ctx.Write(static_cast<int32_t>(pe.BlendMode));
            ctx.Write(static_cast<int32_t>(pe.RenderOrder));
            ctx.Write(pe.FaceCamera ? uint8_t(1) : uint8_t(0));
            ctx.Write(pe.AlignWithTrajectory ? uint8_t(1) : uint8_t(0));
            
            ctx.Write(static_cast<uint32_t>(pe.ColorGradient.size()));
            for (const auto& key : pe.ColorGradient) {
                ctx.Write(key.Time);
                ctx.Write(key.Color.r);
                ctx.Write(key.Color.g);
                ctx.Write(key.Color.b);
                ctx.Write(key.Color.a);
            }

            // v25+: Full inspector parity tail.
            ctx.Write(pe.StopEmittingOnComplete ? uint8_t(1) : uint8_t(0));
            ctx.Write(pe.RateOverDistance ? uint8_t(1) : uint8_t(0));
            ctx.Write(pe.StartColorRandom ? uint8_t(1) : uint8_t(0));
            ctx.Write(pe.StartColorMin.r);
            ctx.Write(pe.StartColorMin.g);
            ctx.Write(pe.StartColorMin.b);
            ctx.Write(pe.StartColorMin.a);
            ctx.Write(pe.StartColorMax.r);
            ctx.Write(pe.StartColorMax.g);
            ctx.Write(pe.StartColorMax.b);
            ctx.Write(pe.StartColorMax.a);
            ctx.Write(pe.VelocityOverLifetimeEnabled ? uint8_t(1) : uint8_t(0));
            ctx.Write(pe.LinearVelocity.x);
            ctx.Write(pe.LinearVelocity.y);
            ctx.Write(pe.LinearVelocity.z);
            ctx.Write(pe.OrbitalVelocity);
            ctx.Write(pe.RadialVelocity);
            ctx.Write(pe.ColorOverLifetimeEnabled ? uint8_t(1) : uint8_t(0));
            ctx.Write(pe.RotationOverLifetimeEnabled ? uint8_t(1) : uint8_t(0));
            ctx.Write(pe.AngularVelocity);
            ctx.Write(pe.TextureSheetEnabled ? uint8_t(1) : uint8_t(0));
            ctx.Write(static_cast<int32_t>(pe.TextureSheetTilesX));
            ctx.Write(static_cast<int32_t>(pe.TextureSheetTilesY));
            ctx.Write(pe.TextureSheetFrameRate);
            ctx.Write(pe.TextureSheetRandomStart ? uint8_t(1) : uint8_t(0));
            break;
        }
        
        case ComponentTypeId::AnimationPlayer: {
            const auto& ap = *data->AnimationPlayer;
            
            ctx.Write(static_cast<uint32_t>(0));  // mode placeholder
            
            uint32_t controllerPathIdx = ctx.AddString(ap.ControllerPath);
            ctx.Write(controllerPathIdx);
            
            uint32_t clipPathIdx = ctx.AddString(ap.SingleClipPath);
            ctx.Write(clipPathIdx);

            uint32_t overridePathIdx = ctx.AddString(ap.ControllerOverridePath);
            ctx.Write(overridePathIdx);
            
            uint8_t playOnStart = ap.PlayOnStart ? 1 : 0;
            uint8_t loop = !ap.ActiveStates.empty() && ap.ActiveStates.front().Loop ? 1 : 0;
            ctx.Write(playOnStart);
            ctx.Write(loop);
            
            ctx.Write(ap.PlaybackSpeed);
            ctx.Write(static_cast<uint32_t>(ap.MotionTarget));
            ctx.Write(static_cast<uint32_t>(ap.ExplicitTargetEntityId));
            ctx.Write(static_cast<uint8_t>(ap.CrowdThrottleEnabled ? 1 : 0));
            ctx.Write(static_cast<uint8_t>(ap.LODEnabled ? 1 : 0));
            ctx.Write(static_cast<uint8_t>(ap.OffscreenDormancyEnabled ? 1 : 0));
            ctx.Write(ap.LODNearDistance);
            ctx.Write(ap.LODMediumDistance);
            ctx.Write(ap.LODFarDistance);
            ctx.Write(ap.LODMediumInterval);
            ctx.Write(ap.LODFarInterval);
            ctx.Write(ap.LODVeryFarInterval);
            ctx.Write(ap.OffscreenNearInterval);
            ctx.Write(ap.OffscreenMediumInterval);
            ctx.Write(ap.OffscreenFarInterval);
            ctx.Write(ap.OffscreenVeryFarInterval);
            break;
        }
        
        case ComponentTypeId::Script: {
            uint32_t scriptCount = static_cast<uint32_t>(data->Scripts.size());
            // High-bit flag denotes typed script payloads
            ctx.Write(scriptCount | kScriptBlockTypedFlag);
            
            for (const auto& script : data->Scripts) {
                uint32_t classNameIdx = ctx.AddString(script.ClassName);
                ctx.Write(classNameIdx);
                
                uint32_t propCount = static_cast<uint32_t>(script.Values.size());
                ctx.Write(propCount);
                
                for (const auto& kv : script.Values) {
                    uint32_t keyIdx = ctx.AddString(kv.first);
                    ctx.Write(keyIdx);
                    
                    PropertyType declaredType = ResolvePropertyType(script, kv.first, kv.second);
                    const auto metaIt = script.EntityRefMetadata.find(kv.first);
                    const ScriptEntityRefMetadata* metaPtr =
                        (metaIt != script.EntityRefMetadata.end()) ? &metaIt->second : nullptr;
                    
                    // If metadata says this is an entity reference, preserve entity encoding even
                    // when reflection is stale and resolved type appears as Int.
                    if (!IsEntityLike(declaredType) && metaPtr && HasEntityRefHints(*metaPtr)) {
                        declaredType = PropertyType::Entity;
                    }
                    
                    PropertyValue valueToWrite = kv.second;
                    // If list metadata carries entity refs but list element type degraded, coerce
                    // element type for deterministic entity-ref encoding.
                    if (declaredType == PropertyType::List &&
                        std::holds_alternative<std::shared_ptr<ListPropertyValue>>(kv.second)) {
                        auto listPtr = std::get<std::shared_ptr<ListPropertyValue>>(kv.second);
                        if (listPtr && !IsEntityLike(listPtr->elementType) && !listPtr->entityRefs.empty()) {
                            bool anyHints = false;
                            for (const auto& refMeta : listPtr->entityRefs) {
                                if (HasEntityRefHints(refMeta)) {
                                    anyHints = true;
                                    break;
                                }
                            }
                            if (anyHints) {
                                auto coerced = std::make_shared<ListPropertyValue>(*listPtr);
                                coerced->elementType = PropertyType::Entity;
                                valueToWrite = coerced;
                            }
                        }
                    }
                    
                    WriteTypedScriptValue(ctx, valueToWrite, declaredType, metaPtr);
                }
            }
            break;
        }
        
        case ComponentTypeId::MeshProxy: {
            const auto& mp = *data->MeshProxy;
            ctx.Write(static_cast<uint32_t>(mp.SerializedTarget));
            uint32_t slotCount = static_cast<uint32_t>(mp.SubmeshSlots.size());
            ctx.Write(slotCount);
            for (uint32_t slot : mp.SubmeshSlots) {
                ctx.Write(slot);
            }
            break;
        }
        
        case ComponentTypeId::BoneAttachment: {
            const auto& ba = *data->BoneAttachment;
            ctx.Write(static_cast<uint32_t>(ba.SkeletonEntity));
            uint32_t boneNameIdx = ctx.AddString(ba.TargetBoneName);
            ctx.Write(boneNameIdx);
            ctx.Write(ba.LocalPosition.x);
            ctx.Write(ba.LocalPosition.y);
            ctx.Write(ba.LocalPosition.z);
            ctx.Write(ba.LocalRotation.x);
            ctx.Write(ba.LocalRotation.y);
            ctx.Write(ba.LocalRotation.z);
            ctx.Write(ba.LocalScale.x);
            ctx.Write(ba.LocalScale.y);
            ctx.Write(ba.LocalScale.z);
            ctx.Write(ba.InheritRotation ? uint8_t(1) : uint8_t(0));
            ctx.Write(ba.InheritScale ? uint8_t(1) : uint8_t(0));
            ctx.Write(ba.Enabled ? uint8_t(1) : uint8_t(0));
            break;
        }
        
        case ComponentTypeId::BlendShape: {
            const auto& bs = *data->BlendShapes;
            uint32_t shapeCount = static_cast<uint32_t>(bs.Shapes.size());
            ctx.Write(shapeCount);
            for (const auto& shape : bs.Shapes) {
                uint32_t nameIdx = ctx.AddString(shape.Name);
                ctx.Write(nameIdx);
                ctx.Write(shape.Weight);
            }
            break;
        }
        
        case ComponentTypeId::UnifiedMorph: {
            const auto& um = *data->UnifiedMorph;
            uint32_t count = static_cast<uint32_t>(um.Names.size());
            ctx.Write(count);
            for (size_t i = 0; i < um.Names.size(); ++i) {
                uint32_t nameIdx = ctx.AddString(um.Names[i]);
                ctx.Write(nameIdx);
                float weight = (i < um.Weights.size()) ? um.Weights[i] : 0.0f;
                ctx.Write(weight);
            }
            break;
        }
        
        case ComponentTypeId::TintController: {
            const auto& tc = *data->TintController;
            ctx.Write(tc.UseTintMask ? uint8_t(1) : uint8_t(0));
            ctx.Write(tc.BaseTint.r);
            ctx.Write(tc.BaseTint.g);
            ctx.Write(tc.BaseTint.b);
            ctx.Write(tc.BaseTint.a);
            // Write all 4 tint colors for mask channels
            ctx.Write(tc.TintColor0.r); ctx.Write(tc.TintColor0.g); ctx.Write(tc.TintColor0.b); ctx.Write(tc.TintColor0.a);
            ctx.Write(tc.TintColor1.r); ctx.Write(tc.TintColor1.g); ctx.Write(tc.TintColor1.b); ctx.Write(tc.TintColor1.a);
            ctx.Write(tc.TintColor2.r); ctx.Write(tc.TintColor2.g); ctx.Write(tc.TintColor2.b); ctx.Write(tc.TintColor2.a);
            ctx.Write(tc.TintColor3.r); ctx.Write(tc.TintColor3.g); ctx.Write(tc.TintColor3.b); ctx.Write(tc.TintColor3.a);
            ctx.Write(static_cast<uint32_t>(tc.GlobalBlendMode));
            ctx.Write(tc.AutoIncludeParentedSkinnedMeshes ? uint8_t(1) : uint8_t(0));
            // Write name pattern for legacy matching
            uint32_t patternLen = static_cast<uint32_t>(tc.NamePattern.size());
            ctx.Write(patternLen);
            if (patternLen > 0) {
                ctx.Write(tc.NamePattern.data(), patternLen);
            }
            // Write explicit targets
            uint32_t targetCount = static_cast<uint32_t>(tc.Targets.size());
            ctx.Write(targetCount);
            for (const auto& target : tc.Targets) {
                ctx.Write(static_cast<uint32_t>(target.TargetEntity));
                ctx.Write(static_cast<uint32_t>(target.BlendMode));
                ctx.Write(target.Color.r);
                ctx.Write(target.Color.g);
                ctx.Write(target.Color.b);
                ctx.Write(target.Color.a);
                ctx.Write(static_cast<int32_t>(target.MaterialSlot));
                ctx.Write(target.UseTargetColor ? uint8_t(1) : uint8_t(0));
            }
            // v6+: PBR scalar overrides
            ctx.Write(tc.UsePbrOverrides ? uint8_t(1) : uint8_t(0));
            ctx.Write(tc.OverrideMetallic);
            ctx.Write(tc.OverrideRoughness);
            ctx.Write(tc.OverrideEmissionStrength);
            ctx.Write(tc.OverrideEmissionColor.x);
            ctx.Write(tc.OverrideEmissionColor.y);
            ctx.Write(tc.OverrideEmissionColor.z);
            break;
        }
        
        case ComponentTypeId::GrassDeformer: {
            const auto& gd = *data->GrassDeformer;
            ctx.Write(gd.Enabled ? uint8_t(1) : uint8_t(0));
            ctx.Write(gd.Radius);
            ctx.Write(gd.Strength);
            ctx.Write(gd.HeightOffset);
            ctx.Write(gd.UseVelocity ? uint8_t(1) : uint8_t(0));
            break;
        }
        
        case ComponentTypeId::River: {
            const auto& r = *data->River;
            ctx.Write(r.Width);
            ctx.Write(r.Depth);
            ctx.Write(r.FlowSpeed);
            uint32_t pointCount = static_cast<uint32_t>(r.PathPoints.size());
            ctx.Write(pointCount);
            for (const auto& pt : r.PathPoints) {
                ctx.Write(pt.Position.x);
                ctx.Write(pt.Position.y);
                ctx.Write(pt.Position.z);
            }
            break;
        }
        
        case ComponentTypeId::Spline: {
            const auto& s = *data->Spline;
            ctx.Write(s.SplineSubdivision);
            ctx.Write(s.Closed ? uint8_t(1) : uint8_t(0));
            uint32_t pointCount = static_cast<uint32_t>(s.ControlPoints.size());
            ctx.Write(pointCount);
            for (const auto& pt : s.ControlPoints) {
                ctx.Write(pt.Position.x);
                ctx.Write(pt.Position.y);
                ctx.Write(pt.Position.z);
                ctx.Write(pt.Normal.x);
                ctx.Write(pt.Normal.y);
                ctx.Write(pt.Normal.z);
            }
            break;
        }
        
        case ComponentTypeId::Area: {
            const auto& area = *data->Area;
            ctx.Write(static_cast<uint32_t>(area.ShapeType));
            ctx.Write(area.Size.x);
            ctx.Write(area.Size.y);
            ctx.Write(area.Size.z);
            ctx.Write(area.Radius);
            ctx.Write(area.Height);
            ctx.Write(area.Enabled ? uint8_t(1) : uint8_t(0));
            ctx.Write(area.MonitorBodies ? uint8_t(1) : uint8_t(0));
            ctx.Write(area.MonitorAreas ? uint8_t(1) : uint8_t(0));
            ctx.Write(area.CollisionLayer);
            ctx.Write(area.CollisionMask);
            // v5 parity fields
            ctx.Write(area.Offset.x);
            ctx.Write(area.Offset.y);
            ctx.Write(area.Offset.z);
            ctx.Write(static_cast<uint8_t>(area.Effects));
            ctx.Write(area.GravityOverride);
            ctx.Write(area.LinearDamp);
            ctx.Write(area.AngularDamp);
            ctx.Write(static_cast<int32_t>(area.Priority));
            break;
        }
        
        case ComponentTypeId::RenderOverrides: {
            const auto& ro = *data->RenderOverrides;
            ctx.Write(ro.AlphaBlendEnabled ? uint8_t(1) : uint8_t(0));
            ctx.Write(ro.UseAlphaCutout ? uint8_t(1) : uint8_t(0));
            ctx.Write(ro.AlphaCutoutThreshold);
            ctx.Write(ro.DepthWriteEnabled ? uint8_t(1) : uint8_t(0));
            ctx.Write(ro.CastShadows ? uint8_t(1) : uint8_t(0));
            ctx.Write(ro.ReceiveShadows ? uint8_t(1) : uint8_t(0));
            break;
        }
        
        case ComponentTypeId::Canvas: {
            const auto& c = *data->Canvas;
            ctx.Write(static_cast<uint32_t>(c.Space));
            ctx.Write(static_cast<int32_t>(c.Width));
            ctx.Write(static_cast<int32_t>(c.Height));
            ctx.Write(c.DPIScale);
            ctx.Write(c.SortOrder);
            ctx.Write(c.Opacity);
            ctx.Write(c.BlockSceneInput ? uint8_t(1) : uint8_t(0));
            // v17+: reference-resolution scaling settings for UI parity
            ctx.Write(static_cast<int32_t>(c.ReferenceWidth));
            ctx.Write(static_cast<int32_t>(c.ReferenceHeight));
            ctx.Write(static_cast<uint32_t>(c.ReferenceScaleMode));
            // v18+: world-space billboarding toggle
            ctx.Write(c.Billboard ? uint8_t(1) : uint8_t(0));
            break;
        }
        
        case ComponentTypeId::Panel: {
            const auto& p = *data->Panel;
            
            ctx.Write(p.Position.x);
            ctx.Write(p.Position.y);
            ctx.Write(p.Size.x);
            ctx.Write(p.Size.y);
            ctx.Write(p.Scale.x);
            ctx.Write(p.Scale.y);
            ctx.Write(p.Pivot.x);
            ctx.Write(p.Pivot.y);
            ctx.Write(p.Rotation);
            ctx.Write(p.TintColor.r);
            ctx.Write(p.TintColor.g);
            ctx.Write(p.TintColor.b);
            ctx.Write(p.TintColor.a);
            ctx.Write(p.Opacity);
            ctx.Write(p.AnchorEnabled ? uint8_t(1) : uint8_t(0));
            ctx.Write(static_cast<uint32_t>(p.Anchor));
            ctx.Write(p.AnchorOffset.x);
            ctx.Write(p.AnchorOffset.y);
            ctx.Write(p.Visible ? uint8_t(1) : uint8_t(0));
            ctx.Write(p.ZOrder);
            // Texture asset reference
            ctx.Write(p.Texture.guid.high);
            ctx.Write(p.Texture.guid.low);
            ctx.Write(p.Texture.fileID);
            ctx.Write(p.Texture.type);
            // UV and fill mode
            ctx.Write(p.UVRect.x);
            ctx.Write(p.UVRect.y);
            ctx.Write(p.UVRect.z);
            ctx.Write(p.UVRect.w);
            ctx.Write(static_cast<uint32_t>(p.Mode));
            ctx.Write(p.TileRepeat.x);
            ctx.Write(p.TileRepeat.y);
            ctx.Write(p.SliceUV.x);
            ctx.Write(p.SliceUV.y);
            ctx.Write(p.SliceUV.z);
            ctx.Write(p.SliceUV.w);
            ctx.Write(p.SliceBorder.x);
            ctx.Write(p.SliceBorder.y);
            ctx.Write(p.SliceBorder.z);
            ctx.Write(p.SliceBorder.w);
            ctx.Write(p.AllowDrag ? uint8_t(1) : uint8_t(0));
            ctx.Write(p.AllowDrop ? uint8_t(1) : uint8_t(0));
            ctx.Write(p.AnchorToParentUI ? uint8_t(1) : uint8_t(0));
            // v26+: optional descendant opacity multiplier
            ctx.Write(p.DriveChildrenOpacity ? uint8_t(1) : uint8_t(0));
            
            break;
        }
        
        case ComponentTypeId::Button: {
            const auto& b = *data->Button;
            ctx.Write(b.NormalTint.r);
            ctx.Write(b.NormalTint.g);
            ctx.Write(b.NormalTint.b);
            ctx.Write(b.NormalTint.a);
            ctx.Write(b.HoverTint.r);
            ctx.Write(b.HoverTint.g);
            ctx.Write(b.HoverTint.b);
            ctx.Write(b.HoverTint.a);
            ctx.Write(b.PressedTint.r);
            ctx.Write(b.PressedTint.g);
            ctx.Write(b.PressedTint.b);
            ctx.Write(b.PressedTint.a);
            ctx.Write(b.DisabledTint.r);
            ctx.Write(b.DisabledTint.g);
            ctx.Write(b.DisabledTint.b);
            ctx.Write(b.DisabledTint.a);
            ctx.Write(b.Interactable ? uint8_t(1) : uint8_t(0));
            ctx.Write(b.Toggle ? uint8_t(1) : uint8_t(0));
            break;
        }
        
        case ComponentTypeId::Slider: {
            const auto& s = *data->Slider;
            ctx.Write(s.MinValue);
            ctx.Write(s.MaxValue);
            ctx.Write(s.Value);
            ctx.Write(s.WholeNumbers ? uint8_t(1) : uint8_t(0));
            ctx.Write(static_cast<uint32_t>(s.SliderDirection));
            ctx.Write(s.Interactable ? uint8_t(1) : uint8_t(0));
            // v17+: full slider payload parity
            ctx.Write(s.Step);
            ctx.Write(s.HandleSize.x);
            ctx.Write(s.HandleSize.y);
            ctx.Write(s.HandleNormalTint.r);
            ctx.Write(s.HandleNormalTint.g);
            ctx.Write(s.HandleNormalTint.b);
            ctx.Write(s.HandleNormalTint.a);
            ctx.Write(s.HandleHoverTint.r);
            ctx.Write(s.HandleHoverTint.g);
            ctx.Write(s.HandleHoverTint.b);
            ctx.Write(s.HandleHoverTint.a);
            ctx.Write(s.HandlePressedTint.r);
            ctx.Write(s.HandlePressedTint.g);
            ctx.Write(s.HandlePressedTint.b);
            ctx.Write(s.HandlePressedTint.a);
            ctx.Write(s.HandleDisabledTint.r);
            ctx.Write(s.HandleDisabledTint.g);
            ctx.Write(s.HandleDisabledTint.b);
            ctx.Write(s.HandleDisabledTint.a);
            ctx.Write(s.ShowFill ? uint8_t(1) : uint8_t(0));
            ctx.Write(s.FillColor.r);
            ctx.Write(s.FillColor.g);
            ctx.Write(s.FillColor.b);
            ctx.Write(s.FillColor.a);
            ctx.Write(s.HandleTexture.guid.high);
            ctx.Write(s.HandleTexture.guid.low);
            ctx.Write(s.HandleTexture.fileID);
            ctx.Write(s.HandleTexture.type);
            ctx.Write(s.FillTexture.guid.high);
            ctx.Write(s.FillTexture.guid.low);
            ctx.Write(s.FillTexture.fileID);
            ctx.Write(s.FillTexture.type);
            ctx.Write(s.Opacity);
            ctx.Write(s.Visible ? uint8_t(1) : uint8_t(0));
            break;
        }
        
        case ComponentTypeId::ProgressBar: {
            const auto& pb = *data->ProgressBar;
            ctx.Write(pb.MinValue);
            ctx.Write(pb.MaxValue);
            ctx.Write(pb.Value);
            ctx.Write(pb.FillColor.r);
            ctx.Write(pb.FillColor.g);
            ctx.Write(pb.FillColor.b);
            ctx.Write(pb.FillColor.a);
            ctx.Write(static_cast<uint32_t>(pb.Direction));
            ctx.Write(pb.UseGradient ? uint8_t(1) : uint8_t(0));
            // v17+: full progress bar payload parity
            ctx.Write(pb.GradientLowColor.r);
            ctx.Write(pb.GradientLowColor.g);
            ctx.Write(pb.GradientLowColor.b);
            ctx.Write(pb.GradientLowColor.a);
            ctx.Write(pb.GradientHighColor.r);
            ctx.Write(pb.GradientHighColor.g);
            ctx.Write(pb.GradientHighColor.b);
            ctx.Write(pb.GradientHighColor.a);
            ctx.Write(pb.Padding.x);
            ctx.Write(pb.Padding.y);
            ctx.Write(pb.Padding.z);
            ctx.Write(pb.Padding.w);
            ctx.Write(pb.UsePanelBorderAsPadding ? uint8_t(1) : uint8_t(0));
            ctx.Write(pb.Animate ? uint8_t(1) : uint8_t(0));
            ctx.Write(pb.AnimationSpeed);
            ctx.Write(pb.FillTexture.guid.high);
            ctx.Write(pb.FillTexture.guid.low);
            ctx.Write(pb.FillTexture.fileID);
            ctx.Write(pb.FillTexture.type);
            ctx.Write(pb.Opacity);
            ctx.Write(pb.Visible ? uint8_t(1) : uint8_t(0));
            break;
        }
        
        case ComponentTypeId::Toggle: {
            const auto& t = *data->Toggle;
            ctx.Write(t.IsOn ? uint8_t(1) : uint8_t(0));
            ctx.Write(t.Interactable ? uint8_t(1) : uint8_t(0));
            // v17+: full toggle payload parity
            ctx.Write(t.CheckmarkSize.x);
            ctx.Write(t.CheckmarkSize.y);
            ctx.Write(t.CheckmarkOffset.x);
            ctx.Write(t.CheckmarkOffset.y);
            ctx.Write(t.CheckmarkTint.r);
            ctx.Write(t.CheckmarkTint.g);
            ctx.Write(t.CheckmarkTint.b);
            ctx.Write(t.CheckmarkTint.a);
            ctx.Write(t.OffTint.r);
            ctx.Write(t.OffTint.g);
            ctx.Write(t.OffTint.b);
            ctx.Write(t.OffTint.a);
            ctx.Write(t.OnTint.r);
            ctx.Write(t.OnTint.g);
            ctx.Write(t.OnTint.b);
            ctx.Write(t.OnTint.a);
            ctx.Write(t.HoverTint.r);
            ctx.Write(t.HoverTint.g);
            ctx.Write(t.HoverTint.b);
            ctx.Write(t.HoverTint.a);
            ctx.Write(t.DisabledTint.r);
            ctx.Write(t.DisabledTint.g);
            ctx.Write(t.DisabledTint.b);
            ctx.Write(t.DisabledTint.a);
            ctx.Write(static_cast<int32_t>(t.GroupID));
            ctx.Write(t.CheckmarkTexture.guid.high);
            ctx.Write(t.CheckmarkTexture.guid.low);
            ctx.Write(t.CheckmarkTexture.fileID);
            ctx.Write(t.CheckmarkTexture.type);
            ctx.Write(t.Opacity);
            ctx.Write(t.Visible ? uint8_t(1) : uint8_t(0));
            break;
        }
        
        case ComponentTypeId::ScrollView: {
            const auto& sv = *data->ScrollView;
            ctx.Write(sv.HorizontalScroll ? uint8_t(1) : uint8_t(0));
            ctx.Write(sv.VerticalScroll ? uint8_t(1) : uint8_t(0));
            ctx.Write(sv.ScrollSensitivity);
            ctx.Write(sv.ContentSize.x);
            ctx.Write(sv.ContentSize.y);
            ctx.Write(sv.ShowScrollbars ? uint8_t(1) : uint8_t(0));
            // v17+: full scroll view payload parity
            ctx.Write(sv.ScrollbarWidth);
            ctx.Write(sv.ScrollbarTrackColor.r);
            ctx.Write(sv.ScrollbarTrackColor.g);
            ctx.Write(sv.ScrollbarTrackColor.b);
            ctx.Write(sv.ScrollbarTrackColor.a);
            ctx.Write(sv.ScrollbarThumbColor.r);
            ctx.Write(sv.ScrollbarThumbColor.g);
            ctx.Write(sv.ScrollbarThumbColor.b);
            ctx.Write(sv.ScrollbarThumbColor.a);
            ctx.Write(sv.ScrollbarThumbHoverColor.r);
            ctx.Write(sv.ScrollbarThumbHoverColor.g);
            ctx.Write(sv.ScrollbarThumbHoverColor.b);
            ctx.Write(sv.ScrollbarThumbHoverColor.a);
            ctx.Write(sv.UseInertia ? uint8_t(1) : uint8_t(0));
            ctx.Write(sv.InertiaDeceleration);
            ctx.Write(sv.Elastic ? uint8_t(1) : uint8_t(0));
            ctx.Write(sv.ElasticAmount);
            ctx.Write(sv.ScrollbarTrackTexture.guid.high);
            ctx.Write(sv.ScrollbarTrackTexture.guid.low);
            ctx.Write(sv.ScrollbarTrackTexture.fileID);
            ctx.Write(sv.ScrollbarTrackTexture.type);
            ctx.Write(sv.ScrollbarThumbTexture.guid.high);
            ctx.Write(sv.ScrollbarThumbTexture.guid.low);
            ctx.Write(sv.ScrollbarThumbTexture.fileID);
            ctx.Write(sv.ScrollbarThumbTexture.type);
            ctx.Write(sv.Opacity);
            ctx.Write(sv.Visible ? uint8_t(1) : uint8_t(0));
            break;
        }
        
        case ComponentTypeId::LayoutGroup: {
            const auto& lg = *data->LayoutGroup;
            ctx.Write(static_cast<uint32_t>(lg.Direction));
            ctx.Write(lg.Spacing);
            ctx.Write(lg.Padding.x);
            ctx.Write(lg.Padding.y);
            ctx.Write(lg.Padding.z);
            ctx.Write(lg.Padding.w);
            ctx.Write(lg.ChildForceExpandWidth ? uint8_t(1) : uint8_t(0));
            ctx.Write(lg.ChildForceExpandHeight ? uint8_t(1) : uint8_t(0));
            // v8+: additional layout group fields (appended for backward compatibility)
            ctx.Write(static_cast<uint32_t>(lg.ChildAlignment));
            ctx.Write(static_cast<uint32_t>(lg.CrossAlignment));
            ctx.Write(lg.ControlChildWidth ? uint8_t(1) : uint8_t(0));
            ctx.Write(lg.ControlChildHeight ? uint8_t(1) : uint8_t(0));
            ctx.Write(lg.ReverseOrder ? uint8_t(1) : uint8_t(0));
            ctx.Write(static_cast<int32_t>(lg.Columns));
            ctx.Write(static_cast<int32_t>(lg.Rows));
            ctx.Write(lg.CellSize.x);
            ctx.Write(lg.CellSize.y);
            break;
        }
        
        case ComponentTypeId::InputField: {
            const auto& inf = *data->InputField;
            uint32_t textIdx = ctx.AddString(inf.Text);
            ctx.Write(textIdx);
            uint32_t placeholderIdx = ctx.AddString(inf.PlaceholderText);
            ctx.Write(placeholderIdx);
            ctx.Write(static_cast<int32_t>(inf.MaxLength));
            ctx.Write(static_cast<uint32_t>(inf.Type));
            ctx.Write(inf.Interactable ? uint8_t(1) : uint8_t(0));
            ctx.Write(inf.Multiline ? uint8_t(1) : uint8_t(0));
            // v17+: full input field payload parity
            ctx.Write(inf.ReadOnly ? uint8_t(1) : uint8_t(0));
            ctx.Write(static_cast<uint8_t>(inf.PasswordChar));
            ctx.Write(inf.TextColor.r);
            ctx.Write(inf.TextColor.g);
            ctx.Write(inf.TextColor.b);
            ctx.Write(inf.TextColor.a);
            ctx.Write(inf.PlaceholderColor.r);
            ctx.Write(inf.PlaceholderColor.g);
            ctx.Write(inf.PlaceholderColor.b);
            ctx.Write(inf.PlaceholderColor.a);
            ctx.Write(inf.SelectionColor.r);
            ctx.Write(inf.SelectionColor.g);
            ctx.Write(inf.SelectionColor.b);
            ctx.Write(inf.SelectionColor.a);
            ctx.Write(inf.CursorColor.r);
            ctx.Write(inf.CursorColor.g);
            ctx.Write(inf.CursorColor.b);
            ctx.Write(inf.CursorColor.a);
            ctx.Write(inf.CursorWidth);
            ctx.Write(inf.Opacity);
            ctx.Write(inf.Visible ? uint8_t(1) : uint8_t(0));
            break;
        }
        
        case ComponentTypeId::Dropdown: {
            const auto& dd = *data->Dropdown;
            ctx.Write(dd.SelectedIndex);
            uint32_t optionCount = static_cast<uint32_t>(dd.Options.size());
            ctx.Write(optionCount);
            for (const auto& opt : dd.Options) {
                uint32_t optIdx = ctx.AddString(opt);
                ctx.Write(optIdx);
            }
            ctx.Write(dd.Interactable ? uint8_t(1) : uint8_t(0));
            // v17+: full dropdown payload parity
            ctx.Write(dd.OptionHeight);
            ctx.Write(static_cast<int32_t>(dd.MaxVisibleOptions));
            ctx.Write(dd.OptionNormalColor.r);
            ctx.Write(dd.OptionNormalColor.g);
            ctx.Write(dd.OptionNormalColor.b);
            ctx.Write(dd.OptionNormalColor.a);
            ctx.Write(dd.OptionHoverColor.r);
            ctx.Write(dd.OptionHoverColor.g);
            ctx.Write(dd.OptionHoverColor.b);
            ctx.Write(dd.OptionHoverColor.a);
            ctx.Write(dd.OptionSelectedColor.r);
            ctx.Write(dd.OptionSelectedColor.g);
            ctx.Write(dd.OptionSelectedColor.b);
            ctx.Write(dd.OptionSelectedColor.a);
            ctx.Write(dd.ShowArrow ? uint8_t(1) : uint8_t(0));
            ctx.Write(dd.ArrowSize.x);
            ctx.Write(dd.ArrowSize.y);
            ctx.Write(dd.ArrowTint.r);
            ctx.Write(dd.ArrowTint.g);
            ctx.Write(dd.ArrowTint.b);
            ctx.Write(dd.ArrowTint.a);
            uint32_t captionIdx = ctx.AddString(dd.Caption);
            ctx.Write(captionIdx);
            ctx.Write(dd.ArrowTexture.guid.high);
            ctx.Write(dd.ArrowTexture.guid.low);
            ctx.Write(dd.ArrowTexture.fileID);
            ctx.Write(dd.ArrowTexture.type);
            ctx.Write(dd.Opacity);
            ctx.Write(dd.Visible ? uint8_t(1) : uint8_t(0));
            break;
        }
        
        case ComponentTypeId::Text: {
            const auto& t = *data->Text;
            uint32_t textIdx = ctx.AddString(t.Text);
            ctx.Write(textIdx);
            uint32_t fontPathIdx = ctx.AddString(t.FontPath);
            ctx.Write(fontPathIdx);
            ctx.Write(t.PixelSize);
            ctx.Write(t.ColorAbgr);
            ctx.Write(t.WorldSpace ? uint8_t(1) : uint8_t(0));
            ctx.Write(t.Visible ? uint8_t(1) : uint8_t(0));
            ctx.Write(t.ZOrder);
            ctx.Write(t.Opacity);
            // v5+: Text anchor-to-parent UI offset and flag (tail of payload)
            ctx.Write(t.AnchorOffset.x);
            ctx.Write(t.AnchorOffset.y);
            ctx.Write(t.AnchorToParentUI ? uint8_t(1) : uint8_t(0));
            // v17+: additional text layout parity fields
            ctx.Write(t.AnchorEnabled ? uint8_t(1) : uint8_t(0));
            ctx.Write(static_cast<uint32_t>(t.Anchor));
            ctx.Write(t.RectSize.x);
            ctx.Write(t.RectSize.y);
            ctx.Write(t.WordWrap ? uint8_t(1) : uint8_t(0));
            // Optional visual effect parity fields
            ctx.Write(t.OutlineEnabled ? uint8_t(1) : uint8_t(0));
            ctx.Write(t.OutlineColorAbgr);
            ctx.Write(t.OutlineThickness);
            ctx.Write(t.ShadowEnabled ? uint8_t(1) : uint8_t(0));
            ctx.Write(t.ShadowColorAbgr);
            ctx.Write(t.ShadowOffset.x);
            ctx.Write(t.ShadowOffset.y);
            // v26+: horizontal text alignment
            ctx.Write(static_cast<uint32_t>(t.TextAlignment));
            // v27+: standalone world text billboarding
            ctx.Write(t.Billboard ? uint8_t(1) : uint8_t(0));
            break;
        }
        
        case ComponentTypeId::NavMesh: {
            const auto& nav = *data->Navigation;
            ctx.Write(nav.Enabled ? uint8_t(1) : uint8_t(0));
            ctx.Write(nav.Bake.agentRadius);
            ctx.Write(nav.Bake.agentHeight);
            ctx.Write(nav.Bake.agentMaxSlopeDeg);
            ctx.Write(nav.Bake.agentMaxClimb);
            ctx.Write(nav.Bake.cellSize);
            ctx.Write(nav.Bake.cellHeight);
            ctx.Write(nav.NavMeshDataGuid.high);
            ctx.Write(nav.NavMeshDataGuid.low);
            ctx.Write(ctx.AddString(nav.AssetPath));
            ctx.Write(uint64_t{0});
            ctx.Write(uint64_t{0});
            // v11+: source selection + terrain + stitching settings
            ctx.Write(uint8_t{1});
            ctx.Write(uint8_t{0});
            ctx.Write(nav.TerrainSampleStep);
            ctx.Write(nav.Bake.agentMaxSlopeDeg);
            ctx.Write(uint8_t{1});
            ctx.Write(nav.DebugDrawOffset);
            ctx.Write(nav.CostAwareSmoothing ? uint8_t(1) : uint8_t(0));
            ctx.Write(nav.EnableStitching ? uint8_t(1) : uint8_t(0));
            ctx.Write(nav.StitchEpsilon);
            ctx.Write(nav.StitchMaxNormalAngleDeg);
            ctx.Write(nav.StitchMaxHeight);
            ctx.Write(nav.StitchMaxXZ);
            ctx.Write(uint8_t{1});
            ctx.Write(uint8_t{1});
            ctx.Write(uint8_t{1});
            ctx.Write(int32_t{0});
            ctx.Write(ctx.AddString(std::string()));
            uint32_t overrideCount = 0;
            ctx.Write(overrideCount);
            // v12+: child regex filter + model nav overrides
            ctx.Write(nav.GeometryIncludeRegexEnabled ? uint8_t(1) : uint8_t(0));
            ctx.Write(ctx.AddString(nav.GeometryIncludeRegexPattern));
            uint32_t modelOverrideCount = 0;
            ctx.Write(modelOverrideCount);
            // v13+: navmesh domains + auto portals
            ctx.Write(static_cast<int32_t>(nav.DomainId));
            ctx.Write(static_cast<int32_t>(nav.DomainPriority));
            ctx.Write(nav.AutoPortalEnabled ? uint8_t(1) : uint8_t(0));
            ctx.Write(nav.AutoPortalMaxXZ);
            ctx.Write(nav.AutoPortalMaxHeight);
            // v15+: visible chunk bake options
            ctx.Write(nav.BakeVisibleChunksOnly ? uint8_t(1) : uint8_t(0));
            ctx.Write(static_cast<uint32_t>(nav.BakeVisibleChunkPadding));
            // v16+: chunked nav streaming options
            ctx.Write(nav.ChunkedNavEnabled ? uint8_t(1) : uint8_t(0));
            ctx.Write(static_cast<uint8_t>(nav.ChunkingMode));
            ctx.Write(nav.ChunkWorldSize);
            ctx.Write(static_cast<uint32_t>(nav.ChunkBakePadding));
            ctx.Write(nav.ChunkStreamRadius);
            ctx.Write(ctx.AddString(nav.NavPackPath));
            // v23+: per-navmesh agent placement height offset
            ctx.Write(nav.AgentPlacementOffset);
            break;
        }
        
        case ComponentTypeId::NavAgent: {
            const auto& agent = *data->NavAgent;
            ctx.Write(agent.Enabled ? uint8_t(1) : uint8_t(0));
            ctx.Write(static_cast<uint32_t>(agent.NavMeshEntity));
            ctx.Write(agent.Params.radius);
            ctx.Write(agent.Params.height);
            ctx.Write(agent.Params.maxSpeed);
            ctx.Write(agent.Params.maxAccel);
            ctx.Write(agent.ArriveThreshold);
            ctx.Write(agent.AutoRepath ? uint8_t(1) : uint8_t(0));
            ctx.Write(agent.RepathInterval);
            ctx.Write(agent.AvoidanceRadiusMul);
            ctx.Write(agent.SteeringSmoothness);
            ctx.Write(agent.ArrivalSlowdownDist);
            ctx.Write(agent.Params.maxSlopeDeg);
            ctx.Write(agent.Params.maxStep);
            // v13+: preferred navmesh domain
            ctx.Write(static_cast<int32_t>(agent.Params.preferredDomainId));
            break;
        }

        case ComponentTypeId::NavLink: {
            const auto& link = *data->NavLink;
            ctx.Write(link.Enabled ? uint8_t(1) : uint8_t(0));
            ctx.Write(link.Start.x);
            ctx.Write(link.Start.y);
            ctx.Write(link.Start.z);
            ctx.Write(link.End.x);
            ctx.Write(link.End.y);
            ctx.Write(link.End.z);
            ctx.Write(link.Radius);
            ctx.Write(link.Cost);
            ctx.Write(link.Flags);
            ctx.Write(link.Bidirectional ? uint8_t(1) : uint8_t(0));
            ctx.Write(link.UseWorldSpace ? uint8_t(1) : uint8_t(0));
            break;
        }
        
        case ComponentTypeId::Portal: {
            const auto& portal = *data->Portal;
            ctx.Write(portal.Enabled ? uint8_t(1) : uint8_t(0));
            ctx.Write(ctx.AddString(portal.TargetScenePath));
            ctx.Write(portal.TargetPortalGuid.high);
            ctx.Write(portal.TargetPortalGuid.low);
            ctx.Write(ctx.AddString(portal.TargetPortalPath));
            ctx.Write(portal.EntryOffset.x);
            ctx.Write(portal.EntryOffset.y);
            ctx.Write(portal.EntryOffset.z);
            ctx.Write(portal.ExitOffset.x);
            ctx.Write(portal.ExitOffset.y);
            ctx.Write(portal.ExitOffset.z);
            ctx.Write(portal.AutoDetect ? uint8_t(1) : uint8_t(0));
            ctx.Write(portal.TriggerRadius);
            ctx.Write(portal.FireExitEvents ? uint8_t(1) : uint8_t(0));
            break;
        }
        
        case ComponentTypeId::IKConstraint: {
            uint32_t ikCount = static_cast<uint32_t>(data->IKs.size());
            ctx.Write(ikCount);
            for (const auto& ik : data->IKs) {
                ctx.Write(ik.Enabled ? uint8_t(1) : uint8_t(0));
                ctx.Write(ik.Weight);
                ctx.Write(static_cast<uint32_t>(ik.TargetEntity));
                ctx.Write(static_cast<uint32_t>(ik.PoleEntity));
                ctx.Write(ik.UseTwoBone ? uint8_t(1) : uint8_t(0));
                ctx.Write(ik.MaxIterations);
                ctx.Write(ik.Tolerance);
                uint32_t chainLen = static_cast<uint32_t>(ik.Chain.size());
                ctx.Write(chainLen);
                for (auto boneId : ik.Chain) {
                    ctx.Write(static_cast<int32_t>(boneId));
                }
            }
            break;
        }
        
        case ComponentTypeId::LookAtConstraint: {
            uint32_t lacCount = static_cast<uint32_t>(data->LookAtConstraints.size());
            ctx.Write(lacCount);
            for (const auto& lac : data->LookAtConstraints) {
                ctx.Write(lac.Enabled ? uint8_t(1) : uint8_t(0));
                ctx.Write(lac.Weight);
                ctx.Write(static_cast<uint32_t>(lac.Mode));
                ctx.Write(lac.TargetEntityGuidHigh);
                ctx.Write(lac.TargetEntityGuidLow);
                ctx.Write(lac.SmoothingSpeed);
                ctx.Write(static_cast<uint32_t>(lac.Axes));
                ctx.Write(lac.MaxYawDeg);
                ctx.Write(-lac.MaxYawDeg);
                ctx.Write(lac.MaxPitchDeg);
                ctx.Write(-lac.MaxPitchDeg);
                uint32_t chainLen = static_cast<uint32_t>(lac.BoneChain.size());
                ctx.Write(chainLen);
                for (auto boneId : lac.BoneChain) {
                    ctx.Write(static_cast<int32_t>(boneId));
                }
            }
            break;
        }
        
        case ComponentTypeId::UIRect: {
            const auto& rect = *data->UIRect;
            ctx.Write(rect.AnchorToParent ? uint8_t(1) : uint8_t(0));
            ctx.Write(rect.HorizontalAnchor);
            ctx.Write(rect.VerticalAnchor);
            ctx.Write(rect.Pivot.x);
            ctx.Write(rect.Pivot.y);
            ctx.Write(rect.Offset.x);
            ctx.Write(rect.Offset.y);
            ctx.Write(rect.Size.x);
            ctx.Write(rect.Size.y);
            break;
        }
        
        case ComponentTypeId::FitToContent: {
            const auto& ftc = *data->FitToContent;
            ctx.Write(ftc.Enabled ? uint8_t(1) : uint8_t(0));
            ctx.Write(ftc.FitWidth ? uint8_t(1) : uint8_t(0));
            ctx.Write(ftc.FitHeight ? uint8_t(1) : uint8_t(0));
            ctx.Write(ftc.Padding.x);
            ctx.Write(ftc.Padding.y);
            ctx.Write(ftc.Padding.z);
            ctx.Write(ftc.Padding.w);
            ctx.Write(ftc.MinSize.x);
            ctx.Write(ftc.MinSize.y);
            ctx.Write(ftc.MaxSize.x);
            ctx.Write(ftc.MaxSize.y);
            ctx.Write(ftc.DirectChildrenOnly ? uint8_t(1) : uint8_t(0));
            break;
        }

        case ComponentTypeId::UISceneCapture: {
            const auto& cap = *data->UISceneCapture;
            ctx.Write(cap.Enabled ? uint8_t(1) : uint8_t(0));
            ctx.Write(cap.AutoFrame ? uint8_t(1) : uint8_t(0));
            ctx.Write(cap.IncludeChildren ? uint8_t(1) : uint8_t(0));
            ctx.Write(cap.BoundsPadding);
            ctx.Write(cap.FieldOfView);
            ctx.Write(cap.NearClip);
            ctx.Write(cap.FarClip);
            ctx.Write(cap.ViewDirection.x);
            ctx.Write(cap.ViewDirection.y);
            ctx.Write(cap.ViewDirection.z);
            ctx.Write(cap.UpDirection.x);
            ctx.Write(cap.UpDirection.y);
            ctx.Write(cap.UpDirection.z);
            ctx.Write(cap.FocusOffset.x);
            ctx.Write(cap.FocusOffset.y);
            ctx.Write(cap.FocusOffset.z);
            ctx.Write(static_cast<int32_t>(cap.TargetEntity));
            ctx.Write(cap.TargetGuidHigh);
            ctx.Write(cap.TargetGuidLow);
            ctx.Write(static_cast<int32_t>(cap.RenderWidth));
            ctx.Write(static_cast<int32_t>(cap.RenderHeight));
            ctx.Write(cap.ClearColor);
            ctx.Write(cap.ShowGrid ? uint8_t(1) : uint8_t(0));
            ctx.Write(cap.LockViewToTarget ? uint8_t(1) : uint8_t(0));
            break;
        }
        
        case ComponentTypeId::ResourceLayers: {
            const auto& layers = *data->ResourceLayers;
            // Global settings
            ctx.Write(layers.GlobalSeed);
            ctx.Write(layers.GlobalDensityMultiplier);
            ctx.Write(layers.GlobalSwapDistance);
            ctx.Write(layers.SwapHysteresis);
            ctx.Write(layers.MaxActivePrefabs);
            ctx.Write(layers.UseClimateGradients ? uint8_t(1) : uint8_t(0));
            
            // Climate configuration
            ctx.Write(layers.Climate.MinAltitude);
            ctx.Write(layers.Climate.MaxAltitude);
            ctx.Write(layers.Climate.MinLongitude);
            ctx.Write(layers.Climate.MaxLongitude);
            
            // Vertical gradient
            uint32_t vertGradCount = static_cast<uint32_t>(layers.Climate.VerticalGradient.Points.size());
            ctx.Write(vertGradCount);
            for (const auto& pt : layers.Climate.VerticalGradient.Points) {
                ctx.Write(pt.Position);
                ctx.Write(pt.Temperature);
                ctx.Write(pt.Moisture);
                ctx.Write(pt.WindExposure);
            }
            
            // Longitudinal gradient
            uint32_t longGradCount = static_cast<uint32_t>(layers.Climate.LongitudinalGradient.Points.size());
            ctx.Write(longGradCount);
            for (const auto& pt : layers.Climate.LongitudinalGradient.Points) {
                ctx.Write(pt.Position);
                ctx.Write(pt.Temperature);
                ctx.Write(pt.Moisture);
                ctx.Write(pt.WindExposure);
            }
            
            // Layers array
            uint32_t layerCount = static_cast<uint32_t>(layers.Layers.size());
            ctx.Write(layerCount);
            for (const auto& layer : layers.Layers) {
                // Layer identity (must match loader order)
                ctx.Write(layer.Guid.high);
                ctx.Write(layer.Guid.low);
                uint32_t nameLen = static_cast<uint32_t>(layer.Name.size());
                ctx.Write(nameLen);
                if (nameLen > 0) ctx.Write(layer.Name.data(), nameLen);
                ctx.Write(layer.Enabled ? uint8_t(1) : uint8_t(0));
                
                // Prefab reference
                ctx.Write(layer.PrefabAsset.guid.high);
                ctx.Write(layer.PrefabAsset.guid.low);
                uint32_t prefabPathLen = static_cast<uint32_t>(layer.PrefabPath.size());
                ctx.Write(prefabPathLen);
                if (prefabPathLen > 0) ctx.Write(layer.PrefabPath.data(), prefabPathLen);
                
                // Distribution settings
                ctx.Write(layer.DensityPerSquareMeter);
                ctx.Write(layer.MinSpacing);
                ctx.Write(layer.MinScale);
                ctx.Write(layer.MaxScale);
                ctx.Write(layer.NonUniformScale ? uint8_t(1) : uint8_t(0));
                ctx.Write(layer.MinScaleVec.x);
                ctx.Write(layer.MinScaleVec.y);
                ctx.Write(layer.MinScaleVec.z);
                ctx.Write(layer.MaxScaleVec.x);
                ctx.Write(layer.MaxScaleVec.y);
                ctx.Write(layer.MaxScaleVec.z);
                ctx.Write(layer.YawVarianceDegrees);
                ctx.Write(layer.PitchVarianceDegrees);
                ctx.Write(layer.RollVarianceDegrees);
                ctx.Write(layer.AlignToSlope ? uint8_t(1) : uint8_t(0));
                ctx.Write(layer.SlopeAlignmentFactor);
                ctx.Write(layer.HeightOffset);
                ctx.Write(layer.HeightOffsetVariance);
                
                // Clustering
                ctx.Write(layer.EnableClustering ? uint8_t(1) : uint8_t(0));
                ctx.Write(layer.ClusterRadius);
                ctx.Write(static_cast<int32_t>(layer.ClusterMinCount));
                ctx.Write(static_cast<int32_t>(layer.ClusterMaxCount));
                ctx.Write(layer.ClusterFalloff);
                ctx.Write(layer.ClusterSpacing);
                
                // LOD
                ctx.Write(layer.UseImposter ? uint8_t(1) : uint8_t(0));
                ctx.Write(layer.ImposterDistance);
                ctx.Write(layer.CullDistance);
                ctx.Write(layer.CrossfadeRange);
                
                // Interaction
                ctx.Write(layer.Interactable ? uint8_t(1) : uint8_t(0));
                ctx.Write(layer.PreservePhysics ? uint8_t(1) : uint8_t(0));
                ctx.Write(layer.InteractionRadius);
                uint32_t tagLen = static_cast<uint32_t>(layer.InteractionTag.size());
                ctx.Write(tagLen);
                if (tagLen > 0) ctx.Write(layer.InteractionTag.data(), tagLen);
                
                // Preview
                ctx.Write(layer.PreviewColor.r);
                ctx.Write(layer.PreviewColor.g);
                ctx.Write(layer.PreviewColor.b);
                
                // Eligibility (serialize as JSON blob to match loader)
                nlohmann::json eligJson;
                layer.Eligibility.Serialize(eligJson);
                std::string eligStr = eligJson.dump();
                uint32_t eligLen = static_cast<uint32_t>(eligStr.size());
                ctx.Write(eligLen);
                if (eligLen > 0) ctx.Write(eligStr.data(), eligLen);
            }
            break;
        }
        
        case ComponentTypeId::Instancer: {
            const auto& inst = *data->Instancer;
            // Asset references
            ctx.Write(inst.MeshAsset.guid.high);
            ctx.Write(inst.MeshAsset.guid.low);
            uint32_t meshPathLen = static_cast<uint32_t>(inst.MeshPath.size());
            ctx.Write(meshPathLen);
            if (meshPathLen > 0) ctx.Write(inst.MeshPath.data(), meshPathLen);
            ctx.Write(inst.PrefabAsset.guid.high);
            ctx.Write(inst.PrefabAsset.guid.low);
            uint32_t prefabPathLen = static_cast<uint32_t>(inst.PrefabPath.size());
            ctx.Write(prefabPathLen);
            if (prefabPathLen > 0) ctx.Write(inst.PrefabPath.data(), prefabPathLen);
            ctx.Write(static_cast<int32_t>(inst.SurfaceEntity));
            // Distribution settings
            ctx.Write(inst.Distribution.Seed);
            ctx.Write(inst.Distribution.DensityPerSquareMeter);
            ctx.Write(inst.Distribution.MinSpacing);
            ctx.Write(inst.Distribution.MinScale);
            ctx.Write(inst.Distribution.MaxScale);
            ctx.Write(inst.Distribution.NonUniformScale ? uint8_t(1) : uint8_t(0));
            ctx.Write(inst.Distribution.MinScaleVec.x);
            ctx.Write(inst.Distribution.MinScaleVec.y);
            ctx.Write(inst.Distribution.MinScaleVec.z);
            ctx.Write(inst.Distribution.MaxScaleVec.x);
            ctx.Write(inst.Distribution.MaxScaleVec.y);
            ctx.Write(inst.Distribution.MaxScaleVec.z);
            ctx.Write(inst.Distribution.YawVarianceDegrees);
            ctx.Write(inst.Distribution.PitchVarianceDegrees);
            ctx.Write(inst.Distribution.RollVarianceDegrees);
            ctx.Write(inst.Distribution.AlignToSlope ? uint8_t(1) : uint8_t(0));
            ctx.Write(inst.Distribution.SlopeAlignmentFactor);
            ctx.Write(inst.Distribution.MinSlopeDegrees);
            ctx.Write(inst.Distribution.MaxSlopeDegrees);
            ctx.Write(inst.Distribution.HeightOffset);
            ctx.Write(inst.Distribution.HeightOffsetVariance);
            // Distribution area
            ctx.Write(inst.DistributionRadius);
            ctx.Write(inst.DistributionAreaMin.x);
            ctx.Write(inst.DistributionAreaMin.y);
            ctx.Write(inst.DistributionAreaMax.x);
            ctx.Write(inst.DistributionAreaMax.y);
            ctx.Write(inst.UseRadiusMode ? uint8_t(1) : uint8_t(0));
            // Manual points
            ctx.Write(inst.UseManualPoints ? uint8_t(1) : uint8_t(0));
            uint32_t numManualPoints = static_cast<uint32_t>(inst.ManualPoints.size());
            ctx.Write(numManualPoints);
            for (const auto& pt : inst.ManualPoints) {
                ctx.Write(pt.x);
                ctx.Write(pt.y);
                ctx.Write(pt.z);
            }
            // Swap settings
            ctx.Write(inst.Swap.SwapDistance);
            ctx.Write(inst.Swap.SwapHysteresis);
            ctx.Write(inst.Swap.CullDistance);
            ctx.Write(inst.Swap.MaxActivePrefabs);
            // Flags
            ctx.Write(inst.Enabled ? uint8_t(1) : uint8_t(0));
            ctx.Write(inst.PreviewColor.r);
            ctx.Write(inst.PreviewColor.g);
            ctx.Write(inst.PreviewColor.b);
            ctx.Write(inst.ShowDebugMarkers ? uint8_t(1) : uint8_t(0));
            ctx.Write(inst.ShowBounds ? uint8_t(1) : uint8_t(0));
            // Rendering options
            ctx.Write(inst.UseAlphaCutout ? uint8_t(1) : uint8_t(0));
            ctx.Write(inst.AlphaCutoutThreshold);
            // Persistent state
            uint32_t numDestroyed = static_cast<uint32_t>(inst.Persistent.DestroyedIDs.size());
            ctx.Write(numDestroyed);
            for (uint32_t id : inst.Persistent.DestroyedIDs) {
                ctx.Write(id);
            }
            uint32_t numOverrides = static_cast<uint32_t>(inst.Persistent.StateOverrides.size());
            ctx.Write(numOverrides);
            for (const auto& [id, state] : inst.Persistent.StateOverrides) {
                ctx.Write(id);
                ctx.Write(static_cast<uint32_t>(state));
            }
            break;
        }
        
        case ComponentTypeId::Module: {
            // Serialize all Module components from Dynamic map
            uint32_t moduleCount = static_cast<uint32_t>(data->Dynamic.size());
            ctx.Write(moduleCount);
            for (const auto& [typeId, module] : data->Dynamic) {
                // Write typeId (hi, lo)
                uint64_t typeIdHi = typeId.hi;
                uint64_t typeIdLo = typeId.lo;
                ctx.Write(typeIdHi);
                ctx.Write(typeIdLo);
                // Write version
                ctx.Write(module.GetVersion());
                // Write field count
                uint32_t fieldCount = static_cast<uint32_t>(module.Fields().size());
                ctx.Write(fieldCount);
                // Write each field: name (string index), type (uint8), then value based on type
                for (const auto& field : module.Fields()) {
                    uint32_t nameIdx = ctx.AddString(field.name);
                    ctx.Write(nameIdx);
                    ctx.Write(static_cast<uint8_t>(field.data.type));
                    // Write value based on type - use Variant directly
                    switch (field.data.type) {
                        case cm::ValueType::Bool:
                            ctx.Write(std::get<bool>(field.data.value) ? uint8_t(1) : uint8_t(0));
                            break;
                        case cm::ValueType::Int:
                            ctx.Write(std::get<int32_t>(field.data.value));
                            break;
                        case cm::ValueType::Int64:
                            ctx.Write(std::get<int64_t>(field.data.value));
                            break;
                        case cm::ValueType::Float:
                            ctx.Write(std::get<float>(field.data.value));
                            break;
                        case cm::ValueType::Double:
                            ctx.Write(std::get<double>(field.data.value));
                            break;
                        case cm::ValueType::String: {
                            std::string str = std::get<std::string>(field.data.value);
                            uint32_t strIdx = ctx.AddString(str);
                            ctx.Write(strIdx);
                            break;
                        }
                        case cm::ValueType::Vec2: {
                            auto v = std::get<glm::vec2>(field.data.value);
                            ctx.Write(v.x);
                            ctx.Write(v.y);
                            break;
                        }
                        case cm::ValueType::Vec3: {
                            auto v = std::get<glm::vec3>(field.data.value);
                            ctx.Write(v.x);
                            ctx.Write(v.y);
                            ctx.Write(v.z);
                            break;
                        }
                        case cm::ValueType::Vec4: {
                            auto v = std::get<glm::vec4>(field.data.value);
                            ctx.Write(v.x);
                            ctx.Write(v.y);
                            ctx.Write(v.z);
                            ctx.Write(v.w);
                            break;
                        }
                        case cm::ValueType::Quat: {
                            auto q = std::get<glm::quat>(field.data.value);
                            ctx.Write(q.w);
                            ctx.Write(q.x);
                            ctx.Write(q.y);
                            ctx.Write(q.z);
                            break;
                        }
                        case cm::ValueType::Guid: {
                            std::string guid = std::get<std::string>(field.data.value);
                            uint32_t guidIdx = ctx.AddString(guid);
                            ctx.Write(guidIdx);
                            break;
                        }
                        case cm::ValueType::Enum: {
                            int32_t enumVal = std::get<cm::EnumValue>(field.data.value).value;
                            ctx.Write(enumVal);
                            break;
                        }
                        default:
                            // Unsupported type - skip
                            break;
                    }
                }
            }
            break;
        }
        
        default:
            break;
    }
    
    return ctx.Position() - startPos;
}

} // namespace binary
