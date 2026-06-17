#include "EntityBinaryWriter.h"
#include "core/ecs/Scene.h"
#include "core/ecs/Entity.h"
#include "core/ecs/EntityData.h"
#include "core/ecs/Components.h"
#include "core/assets/BinaryFormats.h"
#include "core/animation/AnimationPlayerComponent.h"
#include "core/animation/AvatarDefinition.h"
#include "core/physics/area/AreaComponent.h"
#include "core/ecs/InstancerComponent.h"
#include "core/resourcelayer/ResourceLayerTypes.h"
#include "core/navigation/NavMesh.h"
#include <nlohmann/json.hpp>
#include "core/navigation/NavAgent.h"
#include "core/rendering/PBRMaterial.h"
#include "core/rendering/MaterialPropertyBlock.h"
#include "core/serialization/Serializer.h"
#include "editor/nodegraph/ShaderGraphMaterial.h"
#include "editor/nodegraph/ShaderGraphSerializer.h"
#include "core/model/ModelDelta.h"
#include "core/model/ModelDeltaExtractor.h"
#include <algorithm>
#include <cctype>
#include <functional>
#include "managed/interop/ScriptComponent.h"
#include "editor/pipeline/AssetLibrary.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include <variant>
#include <glm/glm.hpp>
#include <bgfx/bgfx.h>

namespace {
constexpr uint32_t kScriptBlockTypedFlag = 0x80000000u;

using binary::EntityBinaryWriter;

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

static void WritePropertyBlock(EntityBinaryWriter::WriteContext& ctx, const MaterialPropertyBlock& pb) {
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

static void WriteTexturePathMap(EntityBinaryWriter::WriteContext& ctx,
                                const std::unordered_map<std::string, std::string>& paths) {
    uint32_t texCount = static_cast<uint32_t>(paths.size());
    ctx.Write(texCount);
    for (const auto& kv : paths) {
        uint32_t samplerIdx = ctx.AddString(kv.first);
        uint32_t pathIdx = ctx.AddString(kv.second);
        ctx.Write(samplerIdx);
        ctx.Write(pathIdx);
    }
}

static EntityID SanitizePrefabEntityRef(const EntityBinaryWriter::WriteContext& ctx, EntityID ref) {
    if (!ctx.sanitizeDerivedPrefabRefs) {
        return ref;
    }
    if (ref == 0 || ref == INVALID_ENTITY_ID || ref == static_cast<EntityID>(-1)) {
        return INVALID_ENTITY_ID;
    }
    if (ctx.prefabEntityIndexMap && ctx.prefabEntityIndexMap->find(ref) != ctx.prefabEntityIndexMap->end()) {
        return ref;
    }
    return INVALID_ENTITY_ID;
}

static bool IsEntityLike(PropertyType t) {
    return t == PropertyType::Entity || t == PropertyType::ComponentRef || t == PropertyType::ScriptRef;
}

static bool HasEntityRefHints(const ScriptEntityRefMetadata* meta) {
    if (!meta) return false;
    return meta->entityId > 0 ||
           meta->guid.high != 0 || meta->guid.low != 0 ||
           meta->modelGuid.high != 0 || meta->modelGuid.low != 0 ||
           meta->modelRootGuid.high != 0 || meta->modelRootGuid.low != 0 ||
           !meta->modelNodePath.empty();
}

static PropertyType ResolvePropertyType(const ScriptInstance& script, const std::string& name, const PropertyValue& value) {
    if (ScriptReflection::HasProperties(script.ClassName)) {
        auto& props = ScriptReflection::GetScriptProperties(script.ClassName);
        auto it = std::find_if(props.begin(), props.end(), [&](const PropertyInfo& p){ return p.name == name; });
        if (it != props.end()) {
            return it->type;
        }
    }
    // Reflection may be unavailable during prefabization (hot reload, compile race).
    // Preserve entity references by honoring stored metadata hints.
    auto metaIt = script.EntityRefMetadata.find(name);
    if (metaIt != script.EntityRefMetadata.end() && HasEntityRefHints(&metaIt->second)) {
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
        if (!outRelPath.empty()) outRelPath += "/";
        outRelPath += d->Name;
    }

    outModelGuid = modelGuid;
    if (auto* rootData = scene->GetEntityData(modelRoot)) {
        outModelRootGuid = rootData->EntityGuid;
    }
    return true;
}

struct SerializedEntityRef {
    int32_t entityId = -1;
    ClaymoreGUID guid{};
    ClaymoreGUID modelGuid{};
    ClaymoreGUID modelRootGuid{};
    std::string modelNodePath;
};

static SerializedEntityRef BuildEntityRef(const EntityBinaryWriter::WriteContext& ctx, const PropertyValue& value, const ScriptEntityRefMetadata* metaOverride = nullptr) {
    SerializedEntityRef ref;
    if (metaOverride) {
        if (metaOverride->entityId > 0) {
            ref.entityId = metaOverride->entityId;
        }
        ref.guid = metaOverride->guid;
        ref.modelGuid = metaOverride->modelGuid;
        ref.modelRootGuid = metaOverride->modelRootGuid;
        ref.modelNodePath = metaOverride->modelNodePath;
    }
    if (std::holds_alternative<int>(value)) {
        ref.entityId = std::get<int>(value);
    } else if (!metaOverride) {
        return ref;
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

static void WriteTypedScriptValue(EntityBinaryWriter::WriteContext& ctx, const PropertyValue& value, PropertyType declaredType, const ScriptEntityRefMetadata* entityMetaOverride = nullptr) {
    ScriptValueTag tag = ToValueTag(declaredType, value);
    if (tag == ScriptValueTag::Int && std::holds_alternative<int>(value) && HasEntityRefHints(entityMetaOverride)) {
        tag = ScriptValueTag::Entity;
    }
    ctx.Write(static_cast<uint8_t>(tag));
    
    switch (tag) {
        case ScriptValueTag::Int: {
            int v = std::holds_alternative<int>(value) ? std::get<int>(value) : 0;
            ctx.Write(static_cast<int32_t>(v));
            break;
        }
        case ScriptValueTag::Float: {
            float v = std::holds_alternative<float>(value) ? std::get<float>(value) : 0.0f;
            ctx.Write(v);
            break;
        }
        case ScriptValueTag::Bool: {
            uint8_t v = (std::holds_alternative<bool>(value) && std::get<bool>(value)) ? 1 : 0;
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
            SerializedEntityRef ref = BuildEntityRef(ctx, value, entityMetaOverride);
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
                    if (!IsEntityLike(elemType) && !listPtr->entityRefs.empty()) {
                        bool anyHints = false;
                        for (const auto& meta : listPtr->entityRefs) {
                            if (HasEntityRefHints(&meta)) {
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
} // namespace

namespace binary {

void EntityBinaryWriter::WriteContext::Write(const void* src, size_t count) {
    size_t oldSize = data.size();
    data.resize(oldSize + count);
    std::memcpy(data.data() + oldSize, src, count);
}

void EntityBinaryWriter::WriteContext::WriteAt(size_t offset, const void* src, size_t count) {
    if (offset + count <= data.size()) {
        std::memcpy(data.data() + offset, src, count);
    }
}

uint32_t EntityBinaryWriter::WriteContext::AddString(const std::string& str) {
    auto it = stringLookup.find(str);
    if (it != stringLookup.end()) {
        return it->second;
    }
    uint32_t index = static_cast<uint32_t>(strings.size());
    strings.push_back(str);
    stringLookup[str] = index;
    return index;
}

void EntityBinaryWriter::WriteContext::AddAssetRef(const ClaymoreGUID& guid, const std::string& path, uint32_t typeHint) {
    AssetRefEntry ref;
    ref.guidHigh = guid.high;
    ref.guidLow = guid.low;
    ref.pathOffset = AddString(path);
    ref.typeHint = typeHint;
    assetRefs.push_back(ref);
}

bool EntityBinaryWriter::Write(const Scene& scene, const std::string& outputPath) {
    std::vector<uint8_t> data;
    if (!WriteToMemory(scene, data)) {
        return false;
    }
    
    std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        std::cerr << "[EntityBinaryWriter] Failed to open output file: " << outputPath << std::endl;
        return false;
    }
    
    out.write(reinterpret_cast<const char*>(data.data()), data.size());
    std::cout << "[EntityBinaryWriter] Wrote " << data.size() << " bytes to " << outputPath << std::endl;
    return true;
}

bool EntityBinaryWriter::WriteToMemory(const Scene& scene, std::vector<uint8_t>& outData) {
    WriteContext ctx;
    ctx.scene = const_cast<Scene*>(&scene);
    
    // Reserve space for header
    SceneBinaryHeader header{};
    header.base.magic = SCENE_MAGIC;
    header.base.version = SCENE_VERSION;
    header.base.flags = 0;
    header.base.reserved = 0;
    
    // Write placeholder header
    ctx.data.resize(sizeof(SceneBinaryHeader));
    
    // Write entities (const_cast needed because GetEntityData isn't const)
    WriteEntities(ctx, scene, header);
    
    // Write string table
    WriteStringTable(ctx, header);
    
    // Write asset ref table
    WriteAssetRefTable(ctx, header);
    
    // Write environment data
    WriteEnvironment(ctx, scene, header);
    
    // Write model deltas (v3+)
    WriteModelDeltas(ctx, scene, header);
    
    // Update header with final offsets
    ctx.WriteAt(0, &header, sizeof(header));
    
    outData = std::move(ctx.data);
    return true;
}

// Helper: Check if entity is a model root (has ModelAssetGuid set)
static bool IsModelRoot(const EntityData* data) {
    return data && (data->ModelAssetGuid.high != 0 || data->ModelAssetGuid.low != 0);
}

void EntityBinaryWriter::WriteEntities(WriteContext& ctx, const Scene& scene, SceneBinaryHeader& header) {
    const auto& entities = scene.GetEntities();
    header.entityCount = static_cast<uint32_t>(entities.size());
    header.componentTableOffset = static_cast<uint32_t>(ctx.Position());
    
    // Need non-const access to get entity data
    Scene& mutableScene = const_cast<Scene&>(scene);
    
    // NOTE: We serialize ALL entities including model children.
    // The scene binary is self-contained - no dependency on .meta files at runtime.
    // ModelAssetGuid is written for model roots to support delta application,
    // but all hierarchy data is baked directly into the scene binary.
    
    for (const auto& entity : entities) {
        EntityID id = entity.GetID();
        EntityData* data = mutableScene.GetEntityData(id);
        if (!data) continue;
        
        // Write entity header
        uint32_t entityId = id;
        uint32_t parentId = static_cast<uint32_t>(data->Parent);
        uint32_t nameIndex = ctx.AddString(data->Name);
        uint64_t guidHigh = data->EntityGuid.high;
        uint64_t guidLow = data->EntityGuid.low;
        
        // v4+: Write ModelAssetGuid for model roots (used for delta application)
        uint64_t modelGuidHigh = data->ModelAssetGuid.high;
        uint64_t modelGuidLow = data->ModelAssetGuid.low;
        
        uint8_t flags = 0;
        if (data->Active) flags |= 0x01;
        if (data->Visible) flags |= 0x02;
        if (modelGuidHigh != 0 || modelGuidLow != 0) flags |= 0x04;  // Has ModelAssetGuid
        
        // Count components - MUST match JSON serializer for full parity
        uint32_t componentCount = 1; // Transform always present
        if (data->Mesh) componentCount++;
        if (data->MeshProxy) componentCount++;
        if (data->Light) componentCount++;
        if (data->Camera) componentCount++;
        if (data->Skeleton) componentCount++;
        if (data->Skinning) componentCount++;
        if (data->BoneAttachment) componentCount++;
        if (data->BlendShapes && !data->BlendShapes->Shapes.empty()) componentCount++;
        if (data->UnifiedMorph && !data->UnifiedMorph->Names.empty()) componentCount++;
        if (data->Collider) componentCount++;
        if (data->RigidBody) componentCount++;
        if (data->StaticBody) componentCount++;
        if (data->Softbody) componentCount++;
        if (data->CharacterController) componentCount++;
        if (data->GrassDeformer) componentCount++;
        if (data->Terrain) componentCount++;
        if (data->River) componentCount++;
        if (data->Spline) componentCount++;
        if (data->Emitter) componentCount++;
        if (data->Area) componentCount++;
        if (data->AnimationPlayer) componentCount++;
        if (data->TintController) componentCount++;
        if (data->RenderOverrides) componentCount++;
        // UI components
        if (data->Canvas) componentCount++;
        if (data->Panel) componentCount++;
        if (data->Button) componentCount++;
        if (data->Slider) componentCount++;
        if (data->ProgressBar) componentCount++;
        if (data->Toggle) componentCount++;
        if (data->ScrollView) componentCount++;
        if (data->LayoutGroup) componentCount++;
        if (data->InputField) componentCount++;
        if (data->Dropdown) componentCount++;
        if (data->Text) componentCount++;
        if (data->UIRect) componentCount++;
        if (data->FitToContent) componentCount++;
        // Navigation
        if (data->Navigation) componentCount++;
        if (data->NavAgent) componentCount++;
        if (data->NavLink) componentCount++;
        if (data->Portal) componentCount++;
        // Animation constraints
        if (!data->IKs.empty()) componentCount++;
        if (!data->LookAtConstraints.empty()) componentCount++;
        // Instancer and ResourceLayers (serialized as JSON blobs for full parity)
        if (data->Instancer) componentCount++;
        if (data->ResourceLayers) componentCount++;
        // Scripts
        if (!data->Scripts.empty()) componentCount++;
        
        ctx.Write(entityId);
        ctx.Write(parentId);
        ctx.Write(nameIndex);
        ctx.Write(guidHigh);
        ctx.Write(guidLow);
        ctx.Write(flags);
        
        // v31+: Write layer and interned tag for parity with prefab entity metadata.
        int32_t layer = data->Layer;
        ctx.Write(layer);
        uint32_t tagIndex = ctx.AddString(data->Tag);
        ctx.Write(tagIndex);
        
        // v4+: Write ModelAssetGuid if present (flag 0x04)
        if (flags & 0x04) {
            ctx.Write(modelGuidHigh);
            ctx.Write(modelGuidLow);
        }
        
        ctx.Write(componentCount);
        
        ctx.currentEntityId = id;
        // Write components - MUST match count above and JSON serializer
        WriteComponent(ctx, data, ComponentTypeId::Transform);
        if (data->Mesh) WriteComponent(ctx, data, ComponentTypeId::Mesh);
        if (data->MeshProxy) WriteComponent(ctx, data, ComponentTypeId::MeshProxy);
        if (data->Light) WriteComponent(ctx, data, ComponentTypeId::Light);
        if (data->Camera) WriteComponent(ctx, data, ComponentTypeId::Camera);
        if (data->Skeleton) WriteComponent(ctx, data, ComponentTypeId::Skeleton);
        if (data->Skinning) WriteComponent(ctx, data, ComponentTypeId::Skinning);
        if (data->BoneAttachment) WriteComponent(ctx, data, ComponentTypeId::BoneAttachment);
        if (data->BlendShapes && !data->BlendShapes->Shapes.empty()) WriteComponent(ctx, data, ComponentTypeId::BlendShape);
        if (data->UnifiedMorph && !data->UnifiedMorph->Names.empty()) WriteComponent(ctx, data, ComponentTypeId::UnifiedMorph);
        if (data->Collider) WriteComponent(ctx, data, ComponentTypeId::Collider);
        if (data->RigidBody) WriteComponent(ctx, data, ComponentTypeId::RigidBody);
        if (data->StaticBody) WriteComponent(ctx, data, ComponentTypeId::StaticBody);
        if (data->Softbody) WriteComponent(ctx, data, ComponentTypeId::Softbody);
        if (data->CharacterController) WriteComponent(ctx, data, ComponentTypeId::CharacterController);
        if (data->GrassDeformer) WriteComponent(ctx, data, ComponentTypeId::GrassDeformer);
        if (data->Terrain) WriteComponent(ctx, data, ComponentTypeId::Terrain);
        if (data->River) WriteComponent(ctx, data, ComponentTypeId::River);
        if (data->Spline) WriteComponent(ctx, data, ComponentTypeId::Spline);
        if (data->Emitter) WriteComponent(ctx, data, ComponentTypeId::ParticleEmitter);
        if (data->Area) WriteComponent(ctx, data, ComponentTypeId::Area);
        if (data->AnimationPlayer) WriteComponent(ctx, data, ComponentTypeId::AnimationPlayer);
        if (data->TintController) WriteComponent(ctx, data, ComponentTypeId::TintController);
        if (data->RenderOverrides) WriteComponent(ctx, data, ComponentTypeId::RenderOverrides);
        // UI components
        if (data->Canvas) WriteComponent(ctx, data, ComponentTypeId::Canvas);
        if (data->Panel) WriteComponent(ctx, data, ComponentTypeId::Panel);
        if (data->Button) WriteComponent(ctx, data, ComponentTypeId::Button);
        if (data->Slider) WriteComponent(ctx, data, ComponentTypeId::Slider);
        if (data->ProgressBar) WriteComponent(ctx, data, ComponentTypeId::ProgressBar);
        if (data->Toggle) WriteComponent(ctx, data, ComponentTypeId::Toggle);
        if (data->ScrollView) WriteComponent(ctx, data, ComponentTypeId::ScrollView);
        if (data->LayoutGroup) WriteComponent(ctx, data, ComponentTypeId::LayoutGroup);
        if (data->InputField) WriteComponent(ctx, data, ComponentTypeId::InputField);
        if (data->Dropdown) WriteComponent(ctx, data, ComponentTypeId::Dropdown);
        if (data->Text) WriteComponent(ctx, data, ComponentTypeId::Text);
        if (data->UIRect) WriteComponent(ctx, data, ComponentTypeId::UIRect);
        if (data->FitToContent) WriteComponent(ctx, data, ComponentTypeId::FitToContent);
        // Navigation
        if (data->Navigation) WriteComponent(ctx, data, ComponentTypeId::NavMesh);
        if (data->NavAgent) WriteComponent(ctx, data, ComponentTypeId::NavAgent);
        if (data->NavLink) WriteComponent(ctx, data, ComponentTypeId::NavLink);
        if (data->Portal) WriteComponent(ctx, data, ComponentTypeId::Portal);
        // Animation constraints
        if (!data->IKs.empty()) WriteComponent(ctx, data, ComponentTypeId::IKConstraint);
        if (!data->LookAtConstraints.empty()) WriteComponent(ctx, data, ComponentTypeId::LookAtConstraint);
        // Instancer and ResourceLayers (serialized as JSON blobs for full parity)
        if (data->Instancer) WriteComponent(ctx, data, ComponentTypeId::Instancer);
        if (data->ResourceLayers) WriteComponent(ctx, data, ComponentTypeId::ResourceLayers);
        // Scripts
        if (!data->Scripts.empty()) WriteComponent(ctx, data, ComponentTypeId::Script);
    }
}

void EntityBinaryWriter::WriteComponent(WriteContext& ctx, const EntityData* data, ComponentTypeId typeId) {
    ComponentEntry entry;
    entry.typeId = typeId;
    entry.flags = 0;
    
    // Remember position to write entry header later
    size_t entryPos = ctx.Position();
    ctx.data.resize(ctx.data.size() + sizeof(ComponentEntry)); // Reserve space
    
    size_t dataStart = ctx.Position();
    entry.dataOffset = static_cast<uint32_t>(dataStart);
    
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
            
            // Write inline materials - supports PBR, PSX, and ShaderGraph materials
            // Collect materials from all sources
            std::vector<binary::InlineMaterialData> pbrMaterials;
            std::vector<binary::ShaderGraphMaterialData> shaderGraphMaterials;
            
            // Process all material slots from the materials vector
            for (size_t i = 0; i < m.materials.size(); ++i) {
                auto mat = m.materials[i];
                if (!mat) continue;
                
                // Check for ShaderGraphMaterial first
                auto sgMat = std::dynamic_pointer_cast<shadergraph::ShaderGraphMaterial>(mat);
                if (sgMat && !sgMat->GetShaderGraphPath().empty()) {
                    binary::ShaderGraphMaterialData sgData;
                    sgData.slotIndex = static_cast<uint32_t>(i);
                    sgData.shaderGraphPath = sgMat->GetShaderGraphPath();
                    sgData.name = sgMat->GetName();
                    sgData.uvScale = sgMat->GetUVScale();
                    sgData.uvOffset = sgMat->GetUVOffset();
                    sgData.stateFlags = sgMat->GetStateFlags();
                    
                    // Try to get compiled shader names from the shader graph file
                    shadergraph::ShaderGraph graph;
                    if (shadergraph::ShaderGraphSerializer::LoadFromFile(sgData.shaderGraphPath, graph)) {
                        if (graph.isCompiled && !graph.compiledVSPath.empty() && !graph.compiledFSPath.empty()) {
                            // Extract shader names from paths (e.g., "shaders/compiled/windows/vs_shgraph_MyShader.bin" -> "vs_shgraph_MyShader")
                            std::filesystem::path vsPath(graph.compiledVSPath);
                            std::filesystem::path fsPath(graph.compiledFSPath);
                            sgData.compiledVSName = vsPath.stem().string();
                            sgData.compiledFSName = fsPath.stem().string();
                        } else {
                            // Generate default shader names from graph path
                            std::string baseName = std::filesystem::path(sgData.shaderGraphPath).stem().string();
                            std::replace(baseName.begin(), baseName.end(), ' ', '_');
                            // Remove non-alphanumeric chars
                            baseName.erase(std::remove_if(baseName.begin(), baseName.end(), 
                                [](char c) { return !std::isalnum(c) && c != '_'; }), baseName.end());
                            sgData.compiledVSName = "vs_shgraph_" + baseName;
                            sgData.compiledFSName = "fs_shgraph_" + baseName;
                        }
                    }
                    
                    // Copy parameters
                    for (const auto& param : sgMat->GetParameters()) {
                        binary::ShaderGraphParamData pd;
                        pd.name = param.name;
                        pd.displayName = param.displayName;
                        pd.type = static_cast<uint32_t>(param.type);
                        pd.value = param.value;
                        pd.texturePath = param.texturePath;
                        pd.textureSlot = param.textureSlot;
                        sgData.parameters.push_back(pd);
                    }
                    
                    shaderGraphMaterials.push_back(sgData);
                    std::cout << "[EntityBinaryWriter]   Slot " << i << " ShaderGraph: " << sgData.shaderGraphPath << std::endl;
                    continue;
                }
                
                // Check for PBR material (includes PSX materials which are PBR with different shaders)
                auto pbr = std::dynamic_pointer_cast<PBRMaterial>(mat);
                if (pbr) {
                    binary::InlineMaterialData inl;
                    inl.slotIndex = static_cast<uint32_t>(i);
                    
                    // Detect PSX material by checking for PSX uniforms
                    glm::vec4 psxParams(0.0f), psxWorld(0.0f), toonParams(0.0f);
                    bool hasPsxParams = pbr->TryGetUniform("u_psxParams", psxParams);
                    bool hasPsxWorld = pbr->TryGetUniform("u_psxWorld", psxWorld);
                    bool hasToonParams = pbr->TryGetUniform("u_toonParams", toonParams);
                    
                    if (hasPsxParams || hasPsxWorld || hasToonParams) {
                        inl.materialType = binary::InlineMaterialType::PSX;
                        inl.psxParams = psxParams;
                        inl.psxWorld = psxWorld;
                        inl.toonParams = toonParams;
                        std::cout << "[EntityBinaryWriter]   Slot " << i << " PSX material" << std::endl;
                    } else {
                        inl.materialType = binary::InlineMaterialType::PBR;
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
                    
                    // Only add if there's some actual data
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
                        inl.materialType == binary::InlineMaterialType::PSX;
                    if (hasPbrState) {
                        pbrMaterials.push_back(inl);
                        if (inl.materialType == binary::InlineMaterialType::PBR) {
                            std::cout << "[EntityBinaryWriter]   Slot " << i << " PBR albedo: " << inl.albedoPath << std::endl;
                        }
                    }
                }
            }
            
            // Fallback: Check InlineMaterials and SlotPropertyBlockTexturePaths for PBR data
            if (pbrMaterials.empty() && shaderGraphMaterials.empty()) {
                // Try InlineMaterials first
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
                        inl.materialType == binary::InlineMaterialType::PSX) {
                        pbrMaterials.push_back(inl);
                    }
                }
                
                // Then try SlotPropertyBlockTexturePaths
                if (pbrMaterials.empty()) {
                    for (size_t i = 0; i < m.SlotPropertyBlockTexturePaths.size(); ++i) {
                        const auto& texPaths = m.SlotPropertyBlockTexturePaths[i];
                        if (texPaths.empty()) continue;
                        
                        binary::InlineMaterialData inl;
                        inl.slotIndex = static_cast<uint32_t>(i);
                        inl.materialType = binary::InlineMaterialType::PBR;
                        
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
            
            // Write PBR/PSX material count and data
            uint32_t pbrMatCount = static_cast<uint32_t>(pbrMaterials.size());
            ctx.Write(pbrMatCount);
            
            for (const auto& inl : pbrMaterials) {
                ctx.Write(inl.slotIndex);
                ctx.Write(static_cast<uint8_t>(inl.materialType));
                
                // Write texture paths
                ctx.Write(ctx.AddString(inl.albedoPath));
                ctx.Write(ctx.AddString(inl.metallicRoughnessPath));
                ctx.Write(ctx.AddString(inl.normalPath));
                ctx.Write(ctx.AddString(inl.aoPath));
                ctx.Write(ctx.AddString(inl.emissionPath));
                ctx.Write(ctx.AddString(inl.displacementPath));
                ctx.Write(ctx.AddString(inl.tintMaskPath));
                
                // Write PBR properties
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
                
                // Write PSX properties if this is a PSX material
                if (inl.materialType == binary::InlineMaterialType::PSX) {
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
            
            // Write ShaderGraph material count and data
            uint32_t sgMatCount = static_cast<uint32_t>(shaderGraphMaterials.size());
            ctx.Write(sgMatCount);
            
            for (const auto& sg : shaderGraphMaterials) {
                ctx.Write(sg.slotIndex);
                ctx.Write(ctx.AddString(sg.shaderGraphPath));
                ctx.Write(ctx.AddString(sg.name));
                ctx.Write(ctx.AddString(sg.compiledVSName));  // Compiled vertex shader name
                ctx.Write(ctx.AddString(sg.compiledFSName));  // Compiled fragment shader name
                ctx.Write(sg.uvScale.x);
                ctx.Write(sg.uvScale.y);
                ctx.Write(sg.uvOffset.x);
                ctx.Write(sg.uvOffset.y);
                ctx.Write(sg.stateFlags);
                ctx.Write(static_cast<uint8_t>(sg.twoSided ? 1 : 0));
                ctx.Write(static_cast<uint8_t>(sg.alphaClip ? 1 : 0));
                ctx.Write(sg.alphaClipThreshold);
                
                // Write parameters
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
        
        case ComponentTypeId::Skeleton: {
            // Write skeleton data
            const auto& s = *data->Skeleton;
            uint32_t boneCount = static_cast<uint32_t>(s.InverseBindPoses.size());
            ctx.Write(boneCount);
            
            // Write inverse bind poses
            for (const auto& mat : s.InverseBindPoses) {
                ctx.Write(&mat[0][0], 64);  // 4x4 float matrix
            }
            
            // Write bone parents
            for (int parent : s.BoneParents) {
                ctx.Write(static_cast<int32_t>(parent));
            }
            
            // Write bone names (for remapping)
            for (const auto& name : s.BoneNames) {
                uint32_t nameIdx = ctx.AddString(name);
                ctx.Write(nameIdx);
            }
            
            // Write bone entity IDs (for pose sampling from hierarchy)
            for (EntityID boneId : s.BoneEntities) {
                boneId = SanitizePrefabEntityRef(ctx, boneId);
                ctx.Write(static_cast<uint32_t>(boneId));
            }
            
            // Write Avatar data (critical for humanoid animation retargeting)
            uint8_t hasAvatar = (s.Avatar != nullptr) ? 1 : 0;
            ctx.Write(hasAvatar);
            
            if (s.Avatar) {
                const auto& avatar = *s.Avatar;
                uint32_t humanoidCountDbg   = static_cast<uint32_t>(avatar.Map.size());
                uint32_t bindModelCountDbg  = static_cast<uint32_t>(avatar.BindModel.size());
                uint32_t bindLocalCountDbg  = static_cast<uint32_t>(avatar.BindLocal.size());
                uint32_t presentCountDbg    = static_cast<uint32_t>(avatar.Present.size());
                uint32_t restOffsetCountDbg = static_cast<uint32_t>(avatar.RestOffsetRot.size());
                uint32_t retargetCountDbg   = static_cast<uint32_t>(avatar.RetargetModel.size());
                std::cout << "[EntityBinaryWriter][Skeleton] Avatar counts "
                          << "humanoid=" << humanoidCountDbg
                          << " bindModel=" << bindModelCountDbg
                          << " bindLocal=" << bindLocalCountDbg
                          << " present=" << presentCountDbg
                          << " restOffset=" << restOffsetCountDbg
                          << " retarget=" << retargetCountDbg
                          << std::endl;
                
                // Write rig name
                uint32_t rigNameIdx = ctx.AddString(avatar.RigName);
                ctx.Write(rigNameIdx);
                
                // Write axes configuration
                ctx.Write(static_cast<uint8_t>(avatar.Axes.Up));
                ctx.Write(static_cast<uint8_t>(avatar.Axes.Forward));
                ctx.Write(static_cast<uint8_t>(avatar.Axes.RightHanded ? 1 : 0));
                ctx.Write(avatar.UnitsPerMeter);
                
                // Write humanoid bone count (should be HumanoidBoneCount)
                uint32_t humanoidCount = static_cast<uint32_t>(avatar.Map.size());
                ctx.Write(humanoidCount);
                
                // Write bone mapping
                for (const auto& entry : avatar.Map) {
                    ctx.Write(static_cast<uint16_t>(entry.Bone));
                    ctx.Write(entry.BoneIndex);
                    uint32_t boneNameIdx = ctx.AddString(entry.BoneName);
                    ctx.Write(boneNameIdx);
                }
                
                // Write BindModel matrices
                uint32_t bindModelCount = static_cast<uint32_t>(avatar.BindModel.size());
                ctx.Write(bindModelCount);
                for (const auto& mat : avatar.BindModel) {
                    ctx.Write(&mat[0][0], 64);
                }
                
                // Write BindLocal matrices
                uint32_t bindLocalCount = static_cast<uint32_t>(avatar.BindLocal.size());
                ctx.Write(bindLocalCount);
                for (const auto& mat : avatar.BindLocal) {
                    ctx.Write(&mat[0][0], 64);
                }
                
                // Write Present flags
                uint32_t presentCount = static_cast<uint32_t>(avatar.Present.size());
                ctx.Write(presentCount);
                for (bool present : avatar.Present) {
                    ctx.Write(static_cast<uint8_t>(present ? 1 : 0));
                }
                
                // Write RestOffsetRot quaternions
                uint32_t restOffsetCount = static_cast<uint32_t>(avatar.RestOffsetRot.size());
                ctx.Write(restOffsetCount);
                for (const auto& q : avatar.RestOffsetRot) {
                    ctx.Write(q.w);
                    ctx.Write(q.x);
                    ctx.Write(q.y);
                    ctx.Write(q.z);
                }
                
                // Write RetargetModel matrices (precomputed)
                uint32_t retargetCount = static_cast<uint32_t>(avatar.RetargetModel.size());
                ctx.Write(retargetCount);
                for (const auto& mat : avatar.RetargetModel) {
                    ctx.Write(&mat[0][0], 64);
                }
                
                std::cout << "[EntityBinaryWriter] Wrote avatar for skeleton with " 
                          << humanoidCount << " humanoid bones mapped" << std::endl;
            }
            break;
        }
        
        case ComponentTypeId::Skinning: {
            // Write skinning data
            const auto& sk = *data->Skinning;
            EntityID skeletonRoot = SanitizePrefabEntityRef(ctx, sk.SkeletonRoot);
            ctx.Write(static_cast<uint32_t>(skeletonRoot));
            uint8_t useParentSkel = sk.UseParentSkeleton ? 1 : 0;
            ctx.Write(useParentSkel);
            
            // Write original bone names (for remapping to different skeletons)
            uint32_t boneNameCount = static_cast<uint32_t>(sk.OriginalBoneNames.size());
            ctx.Write(boneNameCount);
            for (const auto& name : sk.OriginalBoneNames) {
                uint32_t nameIdx = ctx.AddString(name);
                ctx.Write(nameIdx);
            }
            
            // Write original inverse bind poses
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
            // v5+: Write physics layer name (for proper collision layer preservation)
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
            
            // Clipmap settings
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
            
            // Write terrain layers (for splatmap materials)
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
            
            // Basic settings
            ctx.Write(pe.MaxParticles);
            ctx.Write(pe.Enabled ? uint8_t(1) : uint8_t(0));
            uint32_t spritePathIdx = ctx.AddString(pe.SpritePath);
            ctx.Write(spritePathIdx);
            
            // Duration & Looping
            ctx.Write(pe.Duration);
            ctx.Write(pe.Looping ? uint8_t(1) : uint8_t(0));
            ctx.Write(pe.Prewarm ? uint8_t(1) : uint8_t(0));
            ctx.Write(pe.PlayOnAwake ? uint8_t(1) : uint8_t(0));
            ctx.Write(pe.DestroyOnComplete ? uint8_t(1) : uint8_t(0));
            
            // Emission
            ctx.Write(pe.EmissionRate);
            ctx.Write(pe.BurstEnabled ? uint8_t(1) : uint8_t(0));
            ctx.Write(static_cast<int32_t>(pe.BurstCount));
            ctx.Write(pe.BurstTime);
            ctx.Write(static_cast<int32_t>(pe.BurstCycles));
            ctx.Write(pe.BurstInterval);
            
            // Shape
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
            
            // Lifetime
            ctx.Write(pe.Lifetime.Min);
            ctx.Write(pe.Lifetime.Max);
            
            // Start values
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
            
            // Physics
            ctx.Write(pe.GravityModifier);
            ctx.Write(static_cast<int32_t>(pe.SimulationSpace));
            ctx.Write(pe.InheritVelocity);
            ctx.Write(pe.DragCoefficient);
            
            // Size over lifetime
            ctx.Write(pe.SizeOverLifetimeEnabled ? uint8_t(1) : uint8_t(0));
            ctx.Write(static_cast<int32_t>(pe.SizeOverLifetime.CurveType));
            ctx.Write(pe.SizeOverLifetime.StartValue);
            ctx.Write(pe.SizeOverLifetime.EndValue);
            
            // Rendering
            ctx.Write(static_cast<int32_t>(pe.BlendMode));
            ctx.Write(static_cast<int32_t>(pe.RenderOrder));
            ctx.Write(pe.FaceCamera ? uint8_t(1) : uint8_t(0));
            ctx.Write(pe.AlignWithTrajectory ? uint8_t(1) : uint8_t(0));
            
            // Color gradient (simplified - just write count and key data)
            uint32_t gradientCount = static_cast<uint32_t>(pe.ColorGradient.size());
            ctx.Write(gradientCount);
            for (const auto& key : pe.ColorGradient)
            {
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
            
            // Write mode placeholder (0 = controller mode, kept for backwards compatibility)
            ctx.Write(static_cast<uint32_t>(0));
            
            // Write controller path
            uint32_t controllerPathIdx = ctx.AddString(ap.ControllerPath);
            ctx.Write(controllerPathIdx);
            
            // Write single clip path (for simple animation player mode)
            uint32_t clipPathIdx = ctx.AddString(ap.SingleClipPath);
            ctx.Write(clipPathIdx);

            // Write animator override path
            uint32_t overridePathIdx = ctx.AddString(ap.ControllerOverridePath);
            ctx.Write(overridePathIdx);
            
            // Write flags
            uint8_t playOnStart = ap.PlayOnStart ? 1 : 0;
            uint8_t loop = !ap.ActiveStates.empty() && ap.ActiveStates.front().Loop ? 1 : 0;
            ctx.Write(playOnStart);
            ctx.Write(loop);
            
            // Write playback speed
            ctx.Write(ap.PlaybackSpeed);
            
            // v5+: Root motion target configuration
            ctx.Write(static_cast<uint32_t>(ap.MotionTarget));
            ctx.Write(static_cast<uint32_t>(ap.ExplicitTargetEntityId));
            break;
        }
        
        case ComponentTypeId::Script: {
            // Write script count
            uint32_t scriptCount = static_cast<uint32_t>(data->Scripts.size());
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
                    const ScriptEntityRefMetadata* metaOverride = nullptr;
                    auto metaIt = script.EntityRefMetadata.find(kv.first);
                    if (metaIt != script.EntityRefMetadata.end()) {
                        metaOverride = &metaIt->second;
                    }
                    WriteTypedScriptValue(ctx, kv.second, declaredType, metaOverride);
                }
            }
            break;
        }
        
        // ============================================================================
        // Additional components for full parity with JSON serializer
        // ============================================================================
        
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
            EntityID skeletonEntity = SanitizePrefabEntityRef(ctx, ba.SkeletonEntity);
            ctx.Write(static_cast<uint32_t>(skeletonEntity));
            uint32_t boneNameIdx = ctx.AddString(ba.TargetBoneName);
            ctx.Write(boneNameIdx);
            ctx.Write(ba.LocalPosition.x);
            ctx.Write(ba.LocalPosition.y);
            ctx.Write(ba.LocalPosition.z);
            ctx.Write(ba.LocalRotation.x);
            ctx.Write(ba.LocalRotation.y);
            ctx.Write(ba.LocalRotation.z);
            ctx.Write(ba.InheritRotation ? uint8_t(1) : uint8_t(0));
            ctx.Write(ba.InheritScale ? uint8_t(1) : uint8_t(0));
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
            // Write explicit targets (v5+: with MaterialSlot and UseTargetColor)
            uint32_t targetCount = static_cast<uint32_t>(tc.Targets.size());
            ctx.Write(targetCount);
            for (const auto& target : tc.Targets) {
                ctx.Write(static_cast<uint32_t>(target.TargetEntity));
                ctx.Write(static_cast<uint32_t>(target.BlendMode));
                ctx.Write(target.Color.r);
                ctx.Write(target.Color.g);
                ctx.Write(target.Color.b);
                ctx.Write(target.Color.a);
                // v5+: Additional target properties
                ctx.Write(static_cast<int32_t>(target.MaterialSlot));
                ctx.Write(target.UseTargetColor ? uint8_t(1) : uint8_t(0));
            }
            break;
        }
        
        case ComponentTypeId::GrassDeformer: {
            const auto& gd = *data->GrassDeformer;
            ctx.Write(gd.Enabled ? uint8_t(1) : uint8_t(0));
            ctx.Write(gd.Radius);
            ctx.Write(gd.Strength);
            // v5+: Additional params for full parity
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
            // v5+: Additional params for full parity
            ctx.Write(area.Offset.x);
            ctx.Write(area.Offset.y);
            ctx.Write(area.Offset.z);
            ctx.Write(static_cast<uint8_t>(area.Effects));
            ctx.Write(area.GravityOverride);
            ctx.Write(area.LinearDamp);
            ctx.Write(area.AngularDamp);
            ctx.Write(area.Priority);
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
            ctx.Write(p.Texture.guid.high);
            ctx.Write(p.Texture.guid.low);
            ctx.Write(p.Texture.fileID);
            ctx.Write(p.Texture.type);
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
            // v17+: preserve all panel interaction/anchor flags
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
            // v17+: additional layout group fields for parity
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
            // Must match ComponentBinarySerializer.cpp for prefab/scene parity
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
            // Write navmesh data GUID and asset path
            ctx.Write(nav.NavMeshDataGuid.high);
            ctx.Write(nav.NavMeshDataGuid.low);
            ctx.Write(ctx.AddString(nav.AssetPath));
            // Write legacy baked asset GUID for backwards compat
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
            // v5+: Additional params for full parity
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
                // Write chain bone IDs
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
                // Write target entity GUID for robust resolution after load
                ctx.Write(lac.TargetEntityGuidHigh);
                ctx.Write(lac.TargetEntityGuidLow);
                ctx.Write(lac.SmoothingSpeed);
                ctx.Write(static_cast<uint32_t>(lac.Axes));
                // LookAtConstraintComponent uses symmetric limits (MaxYawDeg, MaxPitchDeg)
                // Write max as both max and min for backward compatibility
                ctx.Write(lac.MaxYawDeg);
                ctx.Write(-lac.MaxYawDeg);  // MinYaw = -MaxYaw for symmetric
                ctx.Write(lac.MaxPitchDeg);
                ctx.Write(-lac.MaxPitchDeg);  // MinPitch = -MaxPitch for symmetric
                // Write bone chain
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
        
        case ComponentTypeId::Instancer: {
            // Full binary serialization - matches EntityBinaryLoader::LoadComponent exactly
            const auto& inst = *data->Instancer;
            
            // Asset references
            ctx.Write(inst.MeshAsset.guid.high);
            ctx.Write(inst.MeshAsset.guid.low);
            uint32_t meshPathLen = static_cast<uint32_t>(inst.MeshPath.size());
            ctx.Write(meshPathLen);
            if (meshPathLen > 0) {
                ctx.Write(inst.MeshPath.data(), meshPathLen);
            }
            
            ctx.Write(inst.PrefabAsset.guid.high);
            ctx.Write(inst.PrefabAsset.guid.low);
            uint32_t prefabPathLen = static_cast<uint32_t>(inst.PrefabPath.size());
            ctx.Write(prefabPathLen);
            if (prefabPathLen > 0) {
                ctx.Write(inst.PrefabPath.data(), prefabPathLen);
            }
            
            int32_t surfaceEntity = static_cast<int32_t>(inst.SurfaceEntity);
            ctx.Write(surfaceEntity);
            
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
        
        case ComponentTypeId::ResourceLayers: {
            // Full binary serialization for ResourceLayers
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
                // Layer identity
                ctx.Write(layer.Guid.high);
                ctx.Write(layer.Guid.low);
                uint32_t nameLen = static_cast<uint32_t>(layer.Name.size());
                ctx.Write(nameLen);
                if (nameLen > 0) {
                    ctx.Write(layer.Name.data(), nameLen);
                }
                ctx.Write(layer.Enabled ? uint8_t(1) : uint8_t(0));
                
                // Prefab reference
                ctx.Write(layer.PrefabAsset.guid.high);
                ctx.Write(layer.PrefabAsset.guid.low);
                uint32_t prefabPathLen = static_cast<uint32_t>(layer.PrefabPath.size());
                ctx.Write(prefabPathLen);
                if (prefabPathLen > 0) {
                    ctx.Write(layer.PrefabPath.data(), prefabPathLen);
                }
                
                // Distribution
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
                ctx.Write(layer.ClusterMinCount);
                ctx.Write(layer.ClusterMaxCount);
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
                if (tagLen > 0) {
                    ctx.Write(layer.InteractionTag.data(), tagLen);
                }
                
                // Preview
                ctx.Write(layer.PreviewColor.r);
                ctx.Write(layer.PreviewColor.g);
                ctx.Write(layer.PreviewColor.b);
                
                // Eligibility filters - serialize as JSON blob since it has its own Serialize method
                nlohmann::json eligJson;
                layer.Eligibility.Serialize(eligJson);
                std::string eligStr = eligJson.dump();
                uint32_t eligLen = static_cast<uint32_t>(eligStr.size());
                ctx.Write(eligLen);
                if (eligLen > 0) {
                    ctx.Write(eligStr.data(), eligLen);
                }
            }
            break;
        }
        
        default:
            break;
    }
    
    entry.dataSize = static_cast<uint32_t>(ctx.Position() - dataStart);
    
    // Write entry header
    ctx.WriteAt(entryPos, &entry, sizeof(entry));
}

void EntityBinaryWriter::WriteStringTable(WriteContext& ctx, SceneBinaryHeader& header) {
    header.stringTableOffset = static_cast<uint32_t>(ctx.Position());
    
    uint32_t count = static_cast<uint32_t>(ctx.strings.size());
    ctx.Write(count);
    
    for (const auto& str : ctx.strings) {
        uint32_t len = static_cast<uint32_t>(str.length());
        ctx.Write(len);
        if (len > 0) {
            ctx.Write(str.data(), len);
        }
    }
}

void EntityBinaryWriter::WriteAssetRefTable(WriteContext& ctx, SceneBinaryHeader& header) {
    header.assetRefTableOffset = static_cast<uint32_t>(ctx.Position());
    
    uint32_t count = static_cast<uint32_t>(ctx.assetRefs.size());
    ctx.Write(count);
    
    for (const auto& ref : ctx.assetRefs) {
        ctx.Write(ref);
    }
}

void EntityBinaryWriter::WriteEnvironment(WriteContext& ctx, const Scene& scene, SceneBinaryHeader& header) {
    header.environmentOffset = static_cast<uint32_t>(ctx.Position());
    size_t startPos = ctx.Position();
    
    const Environment& env = scene.GetEnvironment();
    
    // Write environment data in a versioned, extensible format
    // Basic settings
    ctx.Write(static_cast<uint32_t>(env.Ambient));
    ctx.Write(env.AmbientColor.x);
    ctx.Write(env.AmbientColor.y);
    ctx.Write(env.AmbientColor.z);
    ctx.Write(env.AmbientIntensity);
    ctx.Write(env.Exposure);
    
    // Fog
    ctx.Write(env.EnableFog ? uint8_t(1) : uint8_t(0));
    ctx.Write(env.FogColor.x);
    ctx.Write(env.FogColor.y);
    ctx.Write(env.FogColor.z);
    ctx.Write(env.FogDensity);
    
    // Procedural sky
    ctx.Write(env.ProceduralSky ? uint8_t(1) : uint8_t(0));
    ctx.Write(env.SkyTopColor.x);
    ctx.Write(env.SkyTopColor.y);
    ctx.Write(env.SkyTopColor.z);
    ctx.Write(env.SkyHorizonColor.x);
    ctx.Write(env.SkyHorizonColor.y);
    ctx.Write(env.SkyHorizonColor.z);
    ctx.Write(env.SkyGroundColor.x);
    ctx.Write(env.SkyGroundColor.y);
    ctx.Write(env.SkyGroundColor.z);
    ctx.Write(env.SunSize);
    ctx.Write(env.SunSizeConvergence);
    ctx.Write(env.SunIntensity);
    ctx.Write(env.AtmosphereThickness);
    ctx.Write(env.HorizonFade);
    ctx.Write(env.SkyExposure);
    
    // Shadows
    ctx.Write(env.ShadowsEnabled ? uint8_t(1) : uint8_t(0));
    ctx.Write(static_cast<int32_t>(env.ShadowMapResolution));
    ctx.Write(env.ShadowDistance);
    ctx.Write(env.ShadowBias);
    ctx.Write(env.ShadowNormalBias);
    ctx.Write(env.ShadowSoftness);
    ctx.Write(static_cast<int32_t>(env.ShadowSamples));
    ctx.Write(env.ShadowStrength);
    
    // Outline (cosmetic)
    ctx.Write(env.OutlineEnabled ? uint8_t(1) : uint8_t(0));
    ctx.Write(env.OutlineColor.x);
    ctx.Write(env.OutlineColor.y);
    ctx.Write(env.OutlineColor.z);
    ctx.Write(env.OutlineThickness);
    ctx.Write(static_cast<uint32_t>(env.TextureFilter));
    ctx.Write(static_cast<uint32_t>(env.TextureMaxDimension));
    ctx.Write(static_cast<uint32_t>(env.RenderResolutionWidth));
    ctx.Write(static_cast<uint32_t>(env.RenderResolutionHeight));
    ctx.Write(env.UseSkybox ? uint8_t(1) : uint8_t(0));
    ctx.Write(env.UseSkyboxEquirectangular ? uint8_t(1) : uint8_t(0));
    ctx.Write(ctx.AddString(env.SkyboxEquirectangularPath));
    ctx.Write(static_cast<uint32_t>(env.SkyboxEquirectangularResolution));
    for (const std::string& facePath : env.SkyboxFacePaths) {
        ctx.Write(ctx.AddString(facePath));
    }

    header.environmentSize = static_cast<uint32_t>(ctx.Position() - startPos);
    
    std::cout << "[EntityBinaryWriter] Wrote environment data: " << header.environmentSize << " bytes" << std::endl;
}

void EntityBinaryWriter::WriteModelDeltas(WriteContext& ctx, const Scene& scene, SceneBinaryHeader& header) {
    using namespace cm::model;
    
    header.modelDeltaTableOffset = static_cast<uint32_t>(ctx.Position());
    
    // Cast away const for delta extraction (extractor doesn't modify scene)
    Scene& mutableScene = const_cast<Scene&>(scene);
    
    // Extract deltas from all model roots
    ModelDeltaExtractor extractor(mutableScene);
    DeltaExtractionConfig extractConfig;
    // Scene/prefab binaries already serialize full entity state, including scripts.
    // Keep model deltas focused on model-structural overrides to avoid replaying
    // stale entity IDs and visibility snapshots at load time.
    extractConfig.extractScripts = false;
    extractConfig.extractEntityOverrides = false;
    extractConfig.verbose = false;
    
    auto allDeltas = extractor.ExtractAll(extractConfig);
    
    // Filter to only non-empty deltas
    std::vector<std::pair<EntityID, ModelDelta>> nonEmptyDeltas;
    for (auto& [rootId, delta] : allDeltas) {
        if (!delta.IsEmpty()) {
            nonEmptyDeltas.emplace_back(rootId, std::move(delta));
        }
    }
    
    header.modelDeltaCount = static_cast<uint32_t>(nonEmptyDeltas.size());
    
    if (nonEmptyDeltas.empty()) {
        std::cout << "[EntityBinaryWriter] No model deltas to write" << std::endl;
        return;
    }
    
    // First, write the delta entry table (fixed-size entries for direct access)
    size_t entryTableStart = ctx.Position();
    
    // Reserve space for entries - we'll fill in offsets after writing delta data
    std::vector<size_t> entryOffsets;
    entryOffsets.reserve(nonEmptyDeltas.size());
    
    for (size_t i = 0; i < nonEmptyDeltas.size(); ++i) {
        entryOffsets.push_back(ctx.Position());
        ModelDeltaEntry placeholder{};
        ctx.Write(placeholder);
    }
    
    // Write delta data blobs and update entries
    for (size_t i = 0; i < nonEmptyDeltas.size(); ++i) {
        const auto& [rootId, delta] = nonEmptyDeltas[i];
        
        // Serialize delta to JSON and store as blob
        std::string jsonStr = delta.ToJson().dump();
        
        // Build entry
        ModelDeltaEntry entry;
        entry.modelGuidHigh = delta.modelAssetGuid.high;
        entry.modelGuidLow = delta.modelAssetGuid.low;
        entry.rootEntityId = static_cast<uint32_t>(rootId);
        entry.dataOffset = static_cast<uint32_t>(ctx.Position());
        entry.dataSize = static_cast<uint32_t>(jsonStr.size());
        
        // Write delta data
        ctx.Write(jsonStr.data(), jsonStr.size());
        
        // Update entry in table
        ctx.WriteAt(entryOffsets[i], &entry, sizeof(entry));
    }
    
    std::cout << "[EntityBinaryWriter] Wrote " << nonEmptyDeltas.size() << " model deltas" << std::endl;
}

//==============================================================================
// Prefab Writing
//==============================================================================
// Unified prefab writing - uses the same component serialization as scenes
// but with GUID-based parent references for stable prefab linking.

bool EntityBinaryWriter::WritePrefab(Scene& scene, EntityID rootId, const std::string& outputPath) {
    std::vector<uint8_t> data;
    if (!WritePrefabToMemory(scene, rootId, data)) {
        return false;
    }
    
    std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        std::cerr << "[EntityBinaryWriter] Failed to open output file: " << outputPath << std::endl;
        return false;
    }
    
    out.write(reinterpret_cast<const char*>(data.data()), data.size());
    std::cout << "[EntityBinaryWriter] Wrote prefab " << data.size() << " bytes to " << outputPath << std::endl;
    return true;
}

bool EntityBinaryWriter::WritePrefabToMemory(Scene& scene, EntityID rootId, std::vector<uint8_t>& outData) {
    outData.clear();
    
    // Collect all entities under root (including root)
    std::vector<EntityID> entityIds;
    std::unordered_map<EntityID, uint32_t> entityIndexMap;
    
    // Recursively collect entities
    std::function<void(EntityID)> collectEntities = [&](EntityID id) {
        EntityData* data = scene.GetEntityData(id);
        if (!data) return;
        
        entityIndexMap[id] = static_cast<uint32_t>(entityIds.size());
        entityIds.push_back(id);
        
        for (EntityID childId : data->Children) {
            collectEntities(childId);
        }
    };
    
    collectEntities(rootId);
    
    if (entityIds.empty()) {
        std::cerr << "[EntityBinaryWriter] No entities to write for prefab\n";
        return false;
    }
    
    // Prepare write context
    WriteContext ctx;
    ctx.scene = &scene;
    ctx.sanitizeDerivedPrefabRefs = true;
    ctx.prefabEntityIndexMap = &entityIndexMap;
    ctx.strings.push_back(""); // Index 0 = empty string
    ctx.stringLookup[""] = 0;
    
    std::vector<uint8_t> componentData;
    
    // Prefab entity header structure (v4 format - unified with scene format)
    // Must include all entity-level properties for full state preservation
    struct PrefabEntityHeader {
        uint64_t guidHigh;
        uint64_t guidLow;
        uint64_t parentGuidHigh;
        uint64_t parentGuidLow;
        uint32_t entityId;          // Original entity ID for reference remapping
        uint32_t nameIndex;
        uint8_t  flags;             // 0x01=Active, 0x02=Visible, 0x04=HasModelGuid
        uint8_t  padding[3];        // Alignment padding
        int32_t  layer;
        uint32_t tagIndex;          // String table index for tag
        uint64_t modelGuidHigh;     // ModelAssetGuid (only if flags & 0x04)
        uint64_t modelGuidLow;
        uint32_t componentCount;
        uint32_t componentOffset;   // Offset into component data section
    };
    
    std::vector<PrefabEntityHeader> entityHeaders;
    
    // Write component data for each entity (using shared WriteComponent)
    for (EntityID id : entityIds) {
        EntityData* data = scene.GetEntityData(id);
        if (!data) continue;
        
        PrefabEntityHeader header{};
        header.guidHigh = data->EntityGuid.high;
        header.guidLow = data->EntityGuid.low;
        header.entityId = static_cast<uint32_t>(id);
        
        // Get parent GUID
        if (data->Parent != static_cast<EntityID>(-1) && data->Parent != 0) {
            EntityData* parentData = scene.GetEntityData(data->Parent);
            if (parentData) {
                header.parentGuidHigh = parentData->EntityGuid.high;
                header.parentGuidLow = parentData->EntityGuid.low;
            }
        }
        
        // Entity-level properties (unified with scene format)
        header.flags = 0;
        header.padding[0] = header.padding[1] = header.padding[2] = 0;
        if (data->Active) header.flags |= 0x01;
        if (data->Visible) header.flags |= 0x02;
        if (data->ModelAssetGuid.high != 0 || data->ModelAssetGuid.low != 0) header.flags |= 0x04;
        
        header.layer = data->Layer;
        header.tagIndex = ctx.AddString(data->Tag);  // Use string table for tag
        header.modelGuidHigh = data->ModelAssetGuid.high;
        header.modelGuidLow = data->ModelAssetGuid.low;
        
        // Add name to string table
        header.nameIndex = ctx.AddString(data->Name);
        header.componentOffset = static_cast<uint32_t>(componentData.size());
        
        // Build component list - same logic as WriteEntities for scenes
        std::vector<ComponentTypeId> components;
        components.push_back(ComponentTypeId::Transform); // Always present
        if (data->Mesh) components.push_back(ComponentTypeId::Mesh);
        if (data->MeshProxy) components.push_back(ComponentTypeId::MeshProxy);
        if (data->Light) components.push_back(ComponentTypeId::Light);
        if (data->Camera) components.push_back(ComponentTypeId::Camera);
        if (data->Skeleton) components.push_back(ComponentTypeId::Skeleton);
        if (data->Skinning) components.push_back(ComponentTypeId::Skinning);
        if (data->BoneAttachment) components.push_back(ComponentTypeId::BoneAttachment);
        if (data->BlendShapes && !data->BlendShapes->Shapes.empty()) components.push_back(ComponentTypeId::BlendShape);
        if (data->UnifiedMorph && !data->UnifiedMorph->Names.empty()) components.push_back(ComponentTypeId::UnifiedMorph);
        if (data->Collider) components.push_back(ComponentTypeId::Collider);
        if (data->RigidBody) components.push_back(ComponentTypeId::RigidBody);
        if (data->StaticBody) components.push_back(ComponentTypeId::StaticBody);
        if (data->Softbody) components.push_back(ComponentTypeId::Softbody);
        if (data->CharacterController) components.push_back(ComponentTypeId::CharacterController);
        if (data->GrassDeformer) components.push_back(ComponentTypeId::GrassDeformer);
        if (data->Terrain) components.push_back(ComponentTypeId::Terrain);
        if (data->River) components.push_back(ComponentTypeId::River);
        if (data->Spline) components.push_back(ComponentTypeId::Spline);
        if (data->Emitter) components.push_back(ComponentTypeId::ParticleEmitter);
        if (data->Area) components.push_back(ComponentTypeId::Area);
        if (data->AnimationPlayer) components.push_back(ComponentTypeId::AnimationPlayer);
        if (data->TintController) components.push_back(ComponentTypeId::TintController);
        if (data->RenderOverrides) components.push_back(ComponentTypeId::RenderOverrides);
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
        if (data->UIRect) components.push_back(ComponentTypeId::UIRect);
        if (data->FitToContent) components.push_back(ComponentTypeId::FitToContent);
        if (data->Navigation) components.push_back(ComponentTypeId::NavMesh);
        if (data->NavAgent) components.push_back(ComponentTypeId::NavAgent);
        if (data->NavLink) components.push_back(ComponentTypeId::NavLink);
        if (!data->IKs.empty()) components.push_back(ComponentTypeId::IKConstraint);
        if (!data->LookAtConstraints.empty()) components.push_back(ComponentTypeId::LookAtConstraint);
        if (data->Instancer) components.push_back(ComponentTypeId::Instancer);
        if (data->ResourceLayers) components.push_back(ComponentTypeId::ResourceLayers);
        if (!data->Scripts.empty()) components.push_back(ComponentTypeId::Script);
        
        header.componentCount = static_cast<uint32_t>(components.size());
        
        // Write each component using EntityBinaryWriter::WriteComponent (UNIFIED WITH SCENE WRITING)
        // Use a shared WriteContext that accumulates into componentData
        ctx.currentEntityId = id;
        for (ComponentTypeId typeId : components) {
            size_t entryPos = componentData.size();
            componentData.resize(entryPos + sizeof(ComponentEntry));  // Reserve space for entry header
            
            uint32_t dataOffset = static_cast<uint32_t>(componentData.size());
            
            // Reuse the unified component writer without cloning the string table for
            // every component. ctx.data is only scratch storage in prefab writing.
            ctx.data.clear();
            WriteComponent(ctx, data, typeId);

            // WriteComponent writes: ComponentEntry (12 bytes) + component data.
            // Extract just the component data (skip the ComponentEntry it wrote)
            if (ctx.data.size() > sizeof(ComponentEntry)) {
                ComponentEntry entry;
                std::memcpy(&entry, ctx.data.data(), sizeof(ComponentEntry));
                
                // Update entry with correct offset into componentData
                entry.dataOffset = dataOffset;
                
                // Copy just the component data (after ComponentEntry)
                // Use entry.dataSize from WriteComponent (already calculated correctly)
                // Don't recalculate - WriteComponent already set it correctly
                size_t extractedDataSize = ctx.data.size() - sizeof(ComponentEntry);
                
                // Copy exactly entry.dataSize bytes to ensure we don't copy extra data
                // This is critical - we must copy exactly what WriteComponent calculated
                if (extractedDataSize != entry.dataSize) {
                    std::cerr << "[EntityBinaryWriter] WARNING: Size mismatch! entry.dataSize=" << entry.dataSize 
                              << " but extractedDataSize=" << extractedDataSize << std::endl;
                    // Use the actual extracted size to ensure consistency
                    entry.dataSize = static_cast<uint32_t>(extractedDataSize);
                }
                
                // Copy exactly entry.dataSize bytes (not extractedDataSize, in case they differ)
                size_t dataStartOffset = sizeof(ComponentEntry);
                size_t dataEndOffset = dataStartOffset + entry.dataSize;
                if (dataEndOffset > ctx.data.size()) {
                    std::cerr << "[EntityBinaryWriter] ERROR: Component data truncated! entry.dataSize=" << entry.dataSize
                              << " but scratch size=" << ctx.data.size()
                              << ", available=" << (ctx.data.size() - dataStartOffset) << std::endl;
                    return false;
                }
                
                componentData.insert(componentData.end(), 
                    ctx.data.begin() + dataStartOffset,
                    ctx.data.begin() + dataEndOffset);
                
                // Write the entry header at reserved position
                std::memcpy(componentData.data() + entryPos, &entry, sizeof(ComponentEntry));
            }
        }
        
        entityHeaders.push_back(header);
    }
    
    // Extract model deltas for any model roots in the prefab hierarchy
    std::vector<std::pair<EntityID, std::vector<uint8_t>>> modelDeltaBlobs;
    
    for (EntityID id : entityIds) {
        EntityData* data = scene.GetEntityData(id);
        if (!data) continue;
        
        // Check if this entity is a model root
        if (data->ModelAssetGuid.high != 0 || data->ModelAssetGuid.low != 0) {
            cm::model::ModelDeltaExtractor extractor(scene);
            cm::model::DeltaExtractionConfig deltaConfig{};
            // Prefab binaries already serialize full entity hierarchies, including user-added
            // children. Keep model deltas focused on model-node overrides to avoid re-adding
            // duplicate child entities at runtime.
            deltaConfig.extractAddedChildren = false;
            deltaConfig.extractScripts = false;
            deltaConfig.extractEntityOverrides = false;
            cm::model::ModelDelta delta = extractor.Extract(id, deltaConfig);
            
            if (!delta.IsEmpty()) {
                nlohmann::json deltaJson = delta.ToJson();
                std::string deltaStr = deltaJson.dump();
                std::vector<uint8_t> deltaData(deltaStr.begin(), deltaStr.end());
                modelDeltaBlobs.emplace_back(id, std::move(deltaData));
            }
        }
    }
    
    // Calculate layout
    size_t headerSize = sizeof(PrefabBinaryHeader);
    size_t prefabGuidSize = 16;  // GUID high + low
    size_t prefabNameSize = 4;   // Name index
    size_t entityTableOffset = headerSize + prefabGuidSize + prefabNameSize;
    size_t entityTableSize = entityHeaders.size() * sizeof(PrefabEntityHeader);
    size_t componentTableOffset = entityTableOffset + entityTableSize;
    size_t stringTableOffset = componentTableOffset + componentData.size();
    
    // Adjust componentOffset in entity headers to be absolute file offsets
    for (auto& eh : entityHeaders) {
        eh.componentOffset += static_cast<uint32_t>(componentTableOffset);
    }
    
    // Build string table binary
    std::vector<uint8_t> stringTableData;
    std::vector<StringEntry> stringEntries;
    for (const auto& str : ctx.strings) {
        StringEntry entry;
        entry.offset = static_cast<uint32_t>(stringTableData.size());
        entry.length = static_cast<uint32_t>(str.size());
        stringTableData.insert(stringTableData.end(), str.begin(), str.end());
        stringTableData.push_back(0);  // null terminator
        stringEntries.push_back(entry);
    }
    
    // Calculate model delta table offset
    size_t modelDeltaTableOffset = stringTableOffset + 
                                   stringEntries.size() * sizeof(StringEntry) + 
                                   stringTableData.size();
    
    // Reserve output buffer
    size_t deltaTableSize = modelDeltaBlobs.size() * sizeof(ModelDeltaEntry);
    size_t deltaBlobsSize = 0;
    for (const auto& [id, blob] : modelDeltaBlobs) {
        deltaBlobsSize += blob.size();
    }
    size_t totalSize = modelDeltaTableOffset + deltaTableSize + deltaBlobsSize;
    outData.reserve(totalSize);
    
    // Helper to write values
    auto writeValue = [&outData](const auto& value) {
        size_t offset = outData.size();
        outData.resize(offset + sizeof(value));
        std::memcpy(outData.data() + offset, &value, sizeof(value));
    };
    
    // Write header
    PrefabBinaryHeader header;
    header.base.magic = PREFAB_MAGIC;
    header.base.version = PREFAB_VERSION;
    header.base.flags = 0;
    header.base.reserved = 0;
    header.entityCount = static_cast<uint32_t>(entityHeaders.size());
    header.rootEntityIndex = 0;  // Root is always first
    header.componentTableOffset = static_cast<uint32_t>(componentTableOffset);
    header.stringTableOffset = static_cast<uint32_t>(stringTableOffset + stringEntries.size() * sizeof(StringEntry));
    header.assetRefTableOffset = 0;
    header.modelDeltaTableOffset = static_cast<uint32_t>(modelDeltaTableOffset);
    header.modelDeltaCount = static_cast<uint32_t>(modelDeltaBlobs.size());
    
    writeValue(header);
    
    // Write prefab asset GUID. This must be the prefab asset identity (PrefabGuid),
    // not the root entity's instance GUID, so runtime can resolve source JSON reliably.
    EntityData* rootData = scene.GetEntityData(rootId);
    ClaymoreGUID prefabGuid = rootData ? rootData->PrefabGuid : ClaymoreGUID{};
    writeValue(prefabGuid.high);
    writeValue(prefabGuid.low);
    
    // Write prefab name index
    uint32_t prefabNameIdx = ctx.AddString(rootData ? rootData->Name : "Prefab");
    writeValue(prefabNameIdx);
    
    // Write entity headers
    for (const auto& eh : entityHeaders) {
        writeValue(eh);
    }
    
    // Write component data
    outData.insert(outData.end(), componentData.begin(), componentData.end());
    
    // Write string entries
    for (const auto& se : stringEntries) {
        writeValue(se);
    }
    
    // Write string table data
    outData.insert(outData.end(), stringTableData.begin(), stringTableData.end());
    
    // Write model delta table and data
    if (!modelDeltaBlobs.empty()) {
        size_t deltaBlobStart = outData.size() + modelDeltaBlobs.size() * sizeof(ModelDeltaEntry);
        size_t currentBlobOffset = deltaBlobStart;
        
        for (const auto& [modelEntityId, blob] : modelDeltaBlobs) {
            EntityData* modelData = scene.GetEntityData(modelEntityId);
            
            ModelDeltaEntry entry;
            entry.modelGuidHigh = modelData ? modelData->ModelAssetGuid.high : 0;
            entry.modelGuidLow = modelData ? modelData->ModelAssetGuid.low : 0;
            entry.rootEntityId = static_cast<uint32_t>(modelEntityId);
            entry.dataOffset = static_cast<uint32_t>(currentBlobOffset);
            entry.dataSize = static_cast<uint32_t>(blob.size());
            
            writeValue(entry);
            currentBlobOffset += blob.size();
        }
        
        for (const auto& [id, blob] : modelDeltaBlobs) {
            outData.insert(outData.end(), blob.begin(), blob.end());
        }
        
        std::cout << "[EntityBinaryWriter] Wrote " << modelDeltaBlobs.size() << " model deltas for prefab\n";
    }
    
    std::cout << "[EntityBinaryWriter] Wrote prefab with " << entityHeaders.size() 
              << " entities, " << outData.size() << " bytes total\n";
    return true;
}

} // namespace binary
