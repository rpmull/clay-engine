#include "EntityBinaryLoader.h"
#include "Serializer.h"
#include "MeshBinaryLoader.h"
#include "MeshCache.h"
#include "MaterialBinaryLoader.h"
#include "core/ecs/Scene.h"
#include "core/ecs/Entity.h"
#include "core/ecs/EntityData.h"
#include "core/ecs/Components.h"
#include "core/ecs/ModuleComponent.h"
#include "core/ecs/ComponentRegistry.h"
#include "core/ecs/InstancerComponent.h"
#include "core/resourcelayer/ResourceLayerTypes.h"
#include "core/model/ModelDelta.h"
#include "core/model/ModelDeltaApplicator.h"
#include <nlohmann/json.hpp>
#include "core/vfs/VirtualFS.h"
#include "core/vfs/FileSystem.h"
#include "core/assets/IAssetResolver.h"
#include "core/assets/AssetReference.h"
#include "core/physics/PhysicsLayerManager.h"
#include "core/physics/area/AreaComponent.h"
#include "core/animation/AnimationPlayerComponent.h"
#include "core/animation/AnimationPreloader.h"
#include "core/animation/AvatarDefinition.h"
#include "core/rendering/Terrain.h"
#include "core/rendering/PBRMaterial.h"
#include "core/rendering/SkinnedPBRMaterial.h"
#include "core/rendering/RuntimeShaderGraphMaterial.h"
#include "core/rendering/MaterialManager.h"
#include "core/rendering/ShaderManager.h"
#include "core/rendering/TextureLoader.h"
#include "core/rendering/MaterialCache.h"
#if defined(CLAYMORE_EDITOR)
#include "editor/pipeline/AssetLibrary.h"
#endif
#include "core/navigation/NavMesh.h"
#include "core/navigation/NavAgent.h"
#include "core/rendering/VertexTypes.h"
#include "core/managed/RuntimeHost.h"
#include "managed/interop/ScriptComponent.h"
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
bool HasFreshSlotPropertyBlocks(const MeshComponent& mesh) {
    if (mesh.SlotPropertyBlocks.empty() && mesh.SlotPropertyBlockTexturePaths.empty()) {
        return false;
    }

    const size_t slotCount = std::max(mesh.SlotPropertyBlocks.size(), mesh.SlotPropertyBlockTexturePaths.size());
    for (size_t i = 0; i < slotCount; ++i) {
        const bool hasVectors = i < mesh.SlotPropertyBlocks.size() && !mesh.SlotPropertyBlocks[i].Vec4Uniforms.empty();
        const bool hasTextures = i < mesh.SlotPropertyBlockTexturePaths.size() && !mesh.SlotPropertyBlockTexturePaths[i].empty();
        if (hasVectors || hasTextures) {
            return true;
        }
    }

    return false;
}

void RebuildPropertyBlockTexturesFromPathsLocal(MaterialPropertyBlock& block,
                                                const std::unordered_map<std::string, std::string>& texturePaths) {
    block.Textures.clear();
    block.TexturesByID.clear();

    for (const auto& kv : texturePaths) {
        if (kv.second.empty()) {
            continue;
        }

        std::string path = kv.second;
        try {
            path = IVirtualFS::NormalizePath(path);
        } catch (...) {
        }

        TextureSpecifier spec;
        spec.Path = path;
        bgfx::TextureHandle handle = AcquireTextureHandle(spec, TextureColorSpace::Linear);
        if (!bgfx::isValid(handle)) {
            continue;
        }

        block.SetTexture(kv.first, handle);
    }
}

uint32_t FloatBits(float value) {
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float size mismatch");
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::string BuildInlineMaterialCacheKey(const binary::InlineMaterialData& inl, bool skinned) {
    std::ostringstream key;
    key << (skinned ? '1' : '0')
        << '|' << static_cast<int>(inl.materialType)
        << '|' << inl.albedoPath
        << '|' << inl.metallicRoughnessPath
        << '|' << inl.normalPath
        << '|' << inl.aoPath
        << '|' << inl.emissionPath
        << '|' << inl.displacementPath
        << '|' << inl.tintMaskPath
        << '|' << FloatBits(inl.metallic)
        << '|' << FloatBits(inl.roughness)
        << '|' << FloatBits(inl.normalScale)
        << '|' << FloatBits(inl.aoStrength)
        << '|' << FloatBits(inl.emissionStrength)
        << '|' << FloatBits(inl.displacementScale)
        << '|' << FloatBits(inl.emissionColor.x)
        << '|' << FloatBits(inl.emissionColor.y)
        << '|' << FloatBits(inl.emissionColor.z)
        << '|' << FloatBits(inl.uvScale.x)
        << '|' << FloatBits(inl.uvScale.y)
        << '|' << FloatBits(inl.uvOffset.x)
        << '|' << FloatBits(inl.uvOffset.y)
        << '|' << FloatBits(inl.tint.x)
        << '|' << FloatBits(inl.tint.y)
        << '|' << FloatBits(inl.tint.z)
        << '|' << FloatBits(inl.tint.w)
        << '|' << (inl.hasAlpha ? '1' : '0')
        << '|' << (inl.receiveShadowsOverride ? '1' : '0')
        << '|' << (inl.receiveShadows ? '1' : '0')
        << '|' << FloatBits(inl.psxParams.x)
        << '|' << FloatBits(inl.psxParams.y)
        << '|' << FloatBits(inl.psxParams.z)
        << '|' << FloatBits(inl.psxParams.w)
        << '|' << FloatBits(inl.psxWorld.x)
        << '|' << FloatBits(inl.psxWorld.y)
        << '|' << FloatBits(inl.psxWorld.z)
        << '|' << FloatBits(inl.psxWorld.w)
        << '|' << FloatBits(inl.toonParams.x)
        << '|' << FloatBits(inl.toonParams.y)
        << '|' << FloatBits(inl.toonParams.z)
        << '|' << FloatBits(inl.toonParams.w);
    return key.str();
}

std::shared_ptr<Material> AcquireSharedDefaultMaterial(Scene& scene, bool skinned) {
    MaterialSource source;
    source.Skinned = skinned;
    return AcquireMaterialFromSource(source, scene);
}

std::mutex s_inlineMaterialCacheMutex;
std::unordered_map<std::string, std::weak_ptr<Material>> s_inlineMaterialCache;

std::shared_ptr<Material> ShareEquivalentMaterial(const std::shared_ptr<Material>& material) {
    return AcquireEquivalentMaterial(material);
}

bool IsEquivalentMaterialShared(const std::shared_ptr<Material>& material) {
    return material &&
        GetMaterialEquivalenceKey(material.get()).EquivalentSafe;
}
}

namespace {
constexpr uint32_t kScriptBlockTypedFlag = 0x80000000u;
constexpr uint32_t kEntityRefModelRootGuidVersion = 16;

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

static PropertyValue ReadTypedScriptValue(binary::ComponentLoadContext& ctx, PropertyType declaredType, ScriptEntityRefMetadata* outMeta = nullptr) {
    std::function<PropertyValue(PropertyType, ScriptEntityRefMetadata*)> readValue;
    readValue = [&](PropertyType type, ScriptEntityRefMetadata* metaOut) -> PropertyValue {
        uint8_t tagByte = 0;
        if (!ctx.Read(tagByte)) return PropertyValue{};
        ScriptValueTag tag = static_cast<ScriptValueTag>(tagByte);
        
        switch (tag) {
            case ScriptValueTag::Int: {
                int32_t v = 0;
                ctx.Read(v);
                return static_cast<int>(v);
            }
            case ScriptValueTag::Float: {
                float v = 0.0f;
                ctx.Read(v);
                return v;
            }
            case ScriptValueTag::Bool: {
                uint8_t v = 0;
                ctx.Read(v);
                return v != 0;
            }
            case ScriptValueTag::String: {
                uint32_t idx = 0;
                ctx.Read(idx);
                return ctx.ReadString(idx);
            }
            case ScriptValueTag::Vec3: {
                float x = 0.f, y = 0.f, z = 0.f;
                ctx.Read(x); ctx.Read(y); ctx.Read(z);
                return glm::vec3(x, y, z);
            }
            case ScriptValueTag::Entity: {
                int32_t id = -1;
                ctx.Read(id);
                ScriptEntityRefMetadata meta;
                meta.entityId = id;
                uint8_t hasGuid = 0;
                ctx.Read(hasGuid);
                if (hasGuid) {
                    ctx.Read(meta.guid.high);
                    ctx.Read(meta.guid.low);
                }
                uint8_t hasModel = 0;
                ctx.Read(hasModel);
                if (hasModel) {
                    ctx.Read(meta.modelGuid.high);
                    ctx.Read(meta.modelGuid.low);
                    if (ctx.version >= kEntityRefModelRootGuidVersion) {
                        ctx.Read(meta.modelRootGuid.high);
                        ctx.Read(meta.modelRootGuid.low);
                    }
                    uint32_t pathIdx = 0;
                    ctx.Read(pathIdx);
                    meta.modelNodePath = ctx.ReadString(pathIdx);
                }
                if (metaOut) {
                    *metaOut = meta;
                }
                return static_cast<int>(id);
            }
            case ScriptValueTag::List: {
                uint8_t elemTypeByte = 0;
                ctx.Read(elemTypeByte);
                PropertyType elemType = static_cast<PropertyType>(elemTypeByte);
                uint32_t count = 0;
                ctx.Read(count);
                auto listPtr = std::make_shared<ListPropertyValue>();
                listPtr->elementType = elemType;
                listPtr->elements.reserve(count);
                if (IsEntityLike(elemType)) {
                    listPtr->entityRefs.resize(count);
                }
                for (uint32_t i = 0; i < count; ++i) {
                    ScriptEntityRefMetadata elemMeta;
                    PropertyValue elemVal = readValue(elemType, IsEntityLike(elemType) ? &elemMeta : nullptr);
                    listPtr->elements.push_back(elemVal);
                    if (IsEntityLike(elemType) && i < listPtr->entityRefs.size()) {
                        listPtr->entityRefs[i] = elemMeta;
                    }
                }
                return listPtr;
            }
            case ScriptValueTag::Struct: {
                uint32_t fieldCount = 0;
                ctx.Read(fieldCount);
                auto structPtr = std::make_shared<StructPropertyValue>();
                structPtr->fields.reserve(fieldCount);
                for (uint32_t i = 0; i < fieldCount; ++i) {
                    uint32_t keyIdx = 0;
                    ctx.Read(keyIdx);
                    ScriptEntityRefMetadata fieldMeta;
                    PropertyValue fieldVal = readValue(PropertyType::Int, &fieldMeta);
                    structPtr->fields.emplace_back(ctx.ReadString(keyIdx), fieldVal);
                }
                return structPtr;
            }
            case ScriptValueTag::Dictionary: {
                uint8_t keyTypeByte = 0, valueTypeByte = 0;
                ctx.Read(keyTypeByte);
                ctx.Read(valueTypeByte);
                PropertyType keyType = static_cast<PropertyType>(keyTypeByte);
                PropertyType valueType = static_cast<PropertyType>(valueTypeByte);
                uint32_t entryCount = 0;
                ctx.Read(entryCount);
                auto dictPtr = std::make_shared<DictionaryPropertyValue>();
                dictPtr->keyType = keyType;
                dictPtr->valueType = valueType;
                dictPtr->entries.reserve(entryCount);
                for (uint32_t i = 0; i < entryCount; ++i) {
                    PropertyValue keyVal = readValue(keyType, nullptr);
                    ScriptEntityRefMetadata valMeta;
                    PropertyValue valVal = readValue(valueType, IsEntityLike(valueType) ? &valMeta : nullptr);
                    dictPtr->entries.emplace_back(keyVal, valVal);
                }
                return dictPtr;
            }
            default: {
                uint32_t idx = 0;
                if (ctx.Read(idx)) {
                    return ctx.ReadString(idx);
                }
                return PropertyValue{};
            }
        }
    };
    
    return readValue(declaredType, outMeta);
}

// Prefab debug logging toggle (chat-driven)
static bool kPrefabDebugLog = true;
}  // namespace

namespace binary {

namespace {
constexpr uint32_t kSceneEntityTagStringTableVersion = 31;

struct PreparedSceneLayout {
    SceneBinaryHeader header{};
    size_t dataSize = 0;
    uint64_t timestampToken = 0;
    std::vector<std::string> strings;
    std::vector<AssetRefEntry> assetRefs;
    std::vector<EntityBinaryLoader::PreparedEntityRecord> entities;
};

std::mutex s_preparedSceneLayoutCacheMutex;
std::unordered_map<std::string, std::shared_ptr<PreparedSceneLayout>> s_preparedSceneLayoutCache;

uint64_t GetFileTimestampToken(const std::string& path) {
    if (path.empty()) {
        return 0;
    }
    std::error_code ec;
    const std::filesystem::path fsPath(path);
    if (!std::filesystem::exists(fsPath, ec) || ec) {
        return 0;
    }
    const auto writeTime = std::filesystem::last_write_time(fsPath, ec);
    if (ec) {
        return 0;
    }
    using Rep = decltype(writeTime.time_since_epoch())::rep;
    const Rep ticks = writeTime.time_since_epoch().count();
    return static_cast<uint64_t>(ticks < 0 ? -ticks : ticks);
}

std::string MakeSceneLayoutCacheKey(const std::string& path) {
    if (path.empty()) {
        return {};
    }
    try {
        return std::filesystem::weakly_canonical(std::filesystem::path(path)).string();
    } catch (...) {
        return path;
    }
}

bool IsPreparedSceneLayoutCompatible(const PreparedSceneLayout& layout,
                                     const SceneBinaryHeader& header,
                                     size_t dataSize,
                                     uint64_t timestampToken) {
    return layout.dataSize == dataSize &&
           layout.timestampToken == timestampToken &&
           layout.header.base.magic == header.base.magic &&
           layout.header.base.version == header.base.version &&
           layout.header.base.flags == header.base.flags &&
           layout.header.entityCount == header.entityCount &&
           layout.header.stringTableOffset == header.stringTableOffset &&
           layout.header.assetRefTableOffset == header.assetRefTableOffset &&
           layout.header.environmentOffset == header.environmentOffset &&
           layout.header.environmentSize == header.environmentSize &&
           layout.header.modelDeltaTableOffset == header.modelDeltaTableOffset &&
           layout.header.modelDeltaCount == header.modelDeltaCount;
}

std::shared_ptr<PreparedSceneLayout> TryGetPreparedSceneLayoutCache(const std::string& key,
                                                                    const SceneBinaryHeader& header,
                                                                    size_t dataSize,
                                                                    uint64_t timestampToken) {
    if (key.empty() || timestampToken == 0) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(s_preparedSceneLayoutCacheMutex);
    auto it = s_preparedSceneLayoutCache.find(key);
    if (it == s_preparedSceneLayoutCache.end() || !it->second) {
        return nullptr;
    }
    if (!IsPreparedSceneLayoutCompatible(*it->second, header, dataSize, timestampToken)) {
        s_preparedSceneLayoutCache.erase(it);
        return nullptr;
    }
    return it->second;
}

void StorePreparedSceneLayoutCache(const std::string& key, std::shared_ptr<PreparedSceneLayout> layout) {
    if (key.empty() || !layout || layout->timestampToken == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(s_preparedSceneLayoutCacheMutex);
    s_preparedSceneLayoutCache[key] = std::move(layout);
}

const std::vector<AssetRefEntry>& AssetRefsForContext(const EntityBinaryLoader::LoadContext& ctx) {
    return ctx.assetRefView ? *ctx.assetRefView : ctx.assetRefs;
}

bool ReadStringTableInto(EntityBinaryLoader::LoadContext& ctx,
                         const SceneBinaryHeader& header,
                         std::vector<std::string>& outStrings) {
    outStrings.clear();
    if (header.stringTableOffset == 0) {
        return true;
    }

    ctx.pos = header.stringTableOffset;

    uint32_t stringCount = 0;
    if (!ctx.Read(stringCount)) {
        return false;
    }

    outStrings.reserve(stringCount);

    for (uint32_t i = 0; i < stringCount; ++i) {
        uint32_t len = 0;
        if (!ctx.Read(len)) {
            return false;
        }
        if (len > ctx.size - ctx.pos) {
            return false;
        }

        std::string str;
        str.resize(len);
        if (len > 0 && !ctx.Read(str.data(), len)) {
            return false;
        }

        outStrings.push_back(std::move(str));
    }

    return true;
}

bool ReadAssetRefsInto(EntityBinaryLoader::LoadContext& ctx,
                       const SceneBinaryHeader& header,
                       std::vector<AssetRefEntry>& outAssetRefs) {
    outAssetRefs.clear();
    if (header.assetRefTableOffset == 0) {
        return true;
    }

    ctx.pos = header.assetRefTableOffset;

    uint32_t refCount = 0;
    if (!ctx.Read(refCount)) {
        return false;
    }
    if (refCount > (ctx.size - ctx.pos) / sizeof(AssetRefEntry)) {
        return false;
    }

    outAssetRefs.resize(refCount);
    for (uint32_t i = 0; i < refCount; ++i) {
        if (!ctx.Read(outAssetRefs[i])) {
            return false;
        }
    }

    return true;
}

bool BuildPreparedSceneLayout(const uint8_t* data,
                              size_t size,
                              const SceneBinaryHeader& header,
                              std::shared_ptr<PreparedSceneLayout>& outLayout,
                              std::string* error) {
    auto fail = [&](const char* message) {
        if (error) {
            *error = message;
        }
        return false;
    };

    auto layout = std::make_shared<PreparedSceneLayout>();
    layout->header = header;
    layout->dataSize = size;

    EntityBinaryLoader::LoadContext ctx;
    ctx.data = data;
    ctx.size = size;
    ctx.pos = 0;
    ctx.version = header.base.version;

    if (!ReadStringTableInto(ctx, header, layout->strings)) {
        return fail("Failed to read scene string table");
    }
    ctx.stringView = &layout->strings;

    if (!ReadAssetRefsInto(ctx, header, layout->assetRefs)) {
        return fail("Failed to read scene asset reference table");
    }

    ctx.pos = sizeof(SceneBinaryHeader);
    layout->entities.reserve(header.entityCount);

    for (uint32_t i = 0; i < header.entityCount; ++i) {
        EntityBinaryLoader::PreparedEntityRecord record;
        if (!ctx.Read(record.entityId)) return fail("Failed to read entity id");
        if (!ctx.Read(record.parentId)) return fail("Failed to read entity parent");
        if (!ctx.Read(record.nameIndex)) return fail("Failed to read entity name");
        if (!ctx.Read(record.guidHigh)) return fail("Failed to read entity guid high");
        if (!ctx.Read(record.guidLow)) return fail("Failed to read entity guid low");
        if (!ctx.Read(record.flags)) return fail("Failed to read entity flags");

        if (header.base.version >= 5) {
            if (!ctx.Read(record.layer)) return fail("Failed to read entity layer");
            if (header.base.version >= kSceneEntityTagStringTableVersion) {
                if (!ctx.Read(record.tagIndex)) return fail("Failed to read entity tag index");
            } else {
                uint32_t tagLen = 0;
                if (!ctx.Read(tagLen)) return fail("Failed to read entity tag length");
                if (tagLen > ctx.size - ctx.pos) return fail("Entity tag out of bounds");
                record.legacyTag.resize(tagLen);
                if (tagLen > 0 && !ctx.Read(record.legacyTag.data(), tagLen)) {
                    return fail("Failed to read entity tag");
                }
            }
        }

        if (header.base.version >= 4 && (record.flags & 0x04)) {
            if (!ctx.Read(record.modelGuidHigh)) return fail("Failed to read entity model guid high");
            if (!ctx.Read(record.modelGuidLow)) return fail("Failed to read entity model guid low");
        }

        if (!ctx.Read(record.componentCount)) return fail("Failed to read entity component count");

        record.componentOffset = ctx.pos;
        for (uint32_t c = 0; c < record.componentCount; ++c) {
            ComponentEntry entry;
            if (!ctx.Read(entry)) {
                return fail("Failed to read component entry");
            }
            const size_t dataOffset = static_cast<size_t>(entry.dataOffset);
            if (dataOffset > ctx.size || entry.dataSize > ctx.size - dataOffset) {
                return fail("Component data out of bounds");
            }
            if (entry.dataSize > ctx.size - ctx.pos) {
                return fail("Component entry payload out of bounds");
            }
            ctx.pos += entry.dataSize;
        }

        layout->entities.push_back(std::move(record));
    }

    outLayout = std::move(layout);
    return true;
}
} // namespace

// =============================================================================
// Inline primitive mesh creation for runtime
// Avoids cross-library dependency on StandardMeshManager
// =============================================================================

static std::shared_ptr<Mesh> CreatePrimitiveCube() {
    static PBRVertex cubeVertices[] = {
        // Front
        {-1,  1,  1,  0, 0, 1,  0, 0}, { 1,  1,  1,  0, 0, 1,  1, 0},
        {-1, -1,  1,  0, 0, 1,  0, 1}, { 1, -1,  1,  0, 0, 1,  1, 1},
        // Back
        {-1,  1, -1,  0, 0, -1, 0, 0}, { 1,  1, -1,  0, 0, -1, 1, 0},
        {-1, -1, -1,  0, 0, -1, 0, 1}, { 1, -1, -1,  0, 0, -1, 1, 1},
        // Left
        {-1,  1, -1, -1, 0, 0,  0, 0}, {-1,  1,  1, -1, 0, 0,  1, 0},
        {-1, -1, -1, -1, 0, 0,  0, 1}, {-1, -1,  1, -1, 0, 0,  1, 1},
        // Right
        { 1,  1,  1,  1, 0, 0,  0, 0}, { 1,  1, -1,  1, 0, 0,  1, 0},
        { 1, -1,  1,  1, 0, 0,  0, 1}, { 1, -1, -1,  1, 0, 0,  1, 1},
        // Top
        {-1,  1, -1,  0, 1, 0,  0, 0}, { 1,  1, -1,  0, 1, 0,  1, 0},
        {-1,  1,  1,  0, 1, 0,  0, 1}, { 1,  1,  1,  0, 1, 0,  1, 1},
        // Bottom
        {-1, -1,  1,  0, -1, 0, 0, 0}, { 1, -1,  1,  0, -1, 0, 1, 0},
        {-1, -1, -1,  0, -1, 0, 0, 1}, { 1, -1, -1,  0, -1, 0, 1, 1}
    };
    static const uint16_t cubeIndices[] = {
        0, 2, 1, 1, 2, 3,  4, 5, 6, 5, 7, 6,  8,10, 9, 9,10,11,
        12,14,13,13,14,15, 16,18,17,17,18,19, 20,22,21,21,22,23
    };
    
    auto mesh = std::make_shared<Mesh>();
    mesh->vbh = bgfx::createVertexBuffer(bgfx::makeRef(cubeVertices, sizeof(cubeVertices)), PBRVertex::layout);
    mesh->ibh = bgfx::createIndexBuffer(bgfx::makeRef(cubeIndices, sizeof(cubeIndices)));
    mesh->numIndices = sizeof(cubeIndices) / sizeof(uint16_t);
    for (auto& v : cubeVertices) mesh->Vertices.push_back(glm::vec3(v.x, v.y, v.z));
    mesh->Indices.assign(cubeIndices, cubeIndices + mesh->numIndices);
    mesh->ComputeBounds();
    return mesh;
}

static std::shared_ptr<Mesh> CreatePrimitivePlane() {
    static PBRVertex planeVertices[] = {
        {-1,  1,  0,  0, 0, 1,  0, 0}, { 1,  1,  0,  0, 0, 1,  1, 0},
        {-1, -1,  0,  0, 0, 1,  0, 1}, { 1, -1,  0,  0, 0, 1,  1, 1}
    };
    static const uint16_t planeIndices[] = { 0, 2, 1, 1, 2, 3 };
    
    auto mesh = std::make_shared<Mesh>();
    mesh->vbh = bgfx::createVertexBuffer(bgfx::makeRef(planeVertices, sizeof(planeVertices)), PBRVertex::layout);
    mesh->ibh = bgfx::createIndexBuffer(bgfx::makeRef(planeIndices, sizeof(planeIndices)));
    mesh->numIndices = sizeof(planeIndices) / sizeof(uint16_t);
    for (auto& v : planeVertices) mesh->Vertices.push_back(glm::vec3(v.x, v.y, v.z));
    mesh->Indices.assign(planeIndices, planeIndices + mesh->numIndices);
    mesh->ComputeBounds();
    return mesh;
}

static std::shared_ptr<Mesh> CreatePrimitiveSphere() {
    const int segments = 32, rings = 16;
    const float radius = 1.0f;
    std::vector<PBRVertex> vertices;
    std::vector<uint16_t> indices;
    
    for (int ring = 0; ring <= rings; ++ring) {
        float phi = (float)ring / rings * glm::pi<float>();
        float y = radius * cos(phi), ringRadius = radius * sin(phi);
        for (int seg = 0; seg <= segments; ++seg) {
            float theta = (float)seg / segments * 2.0f * glm::pi<float>();
            float x = ringRadius * cos(theta), z = ringRadius * sin(theta);
            vertices.push_back({x, y, z, x/radius, y/radius, z/radius, (float)seg/segments, (float)ring/rings});
        }
    }
    for (int ring = 0; ring < rings; ++ring) {
        for (int seg = 0; seg < segments; ++seg) {
            uint16_t curr = (uint16_t)(ring * (segments + 1) + seg);
            uint16_t next = (uint16_t)(curr + segments + 1);
            indices.push_back(curr); indices.push_back((uint16_t)(curr + 1)); indices.push_back(next);
            indices.push_back(next); indices.push_back((uint16_t)(curr + 1)); indices.push_back((uint16_t)(next + 1));
        }
    }
    
    auto mesh = std::make_shared<Mesh>();
    mesh->vbh = bgfx::createVertexBuffer(bgfx::copy(vertices.data(), (uint32_t)(vertices.size() * sizeof(PBRVertex))), PBRVertex::layout);
    mesh->ibh = bgfx::createIndexBuffer(bgfx::copy(indices.data(), (uint32_t)(indices.size() * sizeof(uint16_t))));
    mesh->numIndices = (uint32_t)indices.size();
    for (auto& v : vertices) mesh->Vertices.push_back(glm::vec3(v.x, v.y, v.z));
    mesh->Indices.assign(indices.begin(), indices.end());
    mesh->ComputeBounds();
    return mesh;
}

static std::shared_ptr<Mesh> CreatePrimitiveCapsule() {
    const int segments = 32, ringsCap = 16;
    const float radius = 0.5f, halfHeight = 0.5f;
    std::vector<PBRVertex> vertices;
    std::vector<uint16_t> indices;
    
    // Cylinder body
    for (int yStep = 0; yStep <= 1; ++yStep) {
        float y = (yStep == 0) ? -halfHeight : +halfHeight;
        for (int s = 0; s <= segments; ++s) {
            float u = (float)s / segments, theta = u * glm::two_pi<float>();
            float cx = radius * cosf(theta), cz = radius * sinf(theta);
            vertices.push_back({cx, y, cz, cosf(theta), 0.0f, sinf(theta), u, yStep * 0.5f});
        }
    }
    for (int s = 0; s < segments; ++s) {
        uint16_t i0 = (uint16_t)s, i1 = (uint16_t)(s + 1);
        uint16_t i2 = (uint16_t)(segments + 1 + s), i3 = (uint16_t)(segments + 1 + s + 1);
        indices.push_back(i0); indices.push_back(i2); indices.push_back(i1);
        indices.push_back(i2); indices.push_back(i3); indices.push_back(i1);
    }
    
    // Top hemisphere
    int baseTop = (int)vertices.size();
    for (int r = 0; r <= ringsCap; ++r) {
        float t = (float)r / ringsCap, phi = t * glm::half_pi<float>();
        float yLocal = radius * cosf(phi), ringR = radius * sinf(phi), y = halfHeight + yLocal;
        for (int s = 0; s <= segments; ++s) {
            float u = (float)s / segments, theta = u * glm::two_pi<float>();
            float x = ringR * cosf(theta), z = ringR * sinf(theta);
            glm::vec3 n = glm::normalize(glm::vec3(cosf(theta) * sinf(phi), cosf(phi), sinf(theta) * sinf(phi)));
            vertices.push_back({x, y, z, n.x, n.y, n.z, u, 0.5f + 0.5f * (1.0f - t)});
        }
    }
    for (int r = 0; r < ringsCap; ++r) {
        for (int s = 0; s < segments; ++s) {
            uint16_t curr = (uint16_t)(baseTop + r * (segments + 1) + s);
            uint16_t next = (uint16_t)(curr + segments + 1);
            indices.push_back(curr); indices.push_back((uint16_t)(curr + 1)); indices.push_back(next);
            indices.push_back(next); indices.push_back((uint16_t)(curr + 1)); indices.push_back((uint16_t)(next + 1));
        }
    }
    
    // Bottom hemisphere
    int baseBottom = (int)vertices.size();
    for (int r = 0; r <= ringsCap; ++r) {
        float t = (float)r / ringsCap, phi = t * glm::half_pi<float>();
        float yLocal = radius * cosf(phi), ringR = radius * sinf(phi), y = -halfHeight - yLocal;
        for (int s = 0; s <= segments; ++s) {
            float u = (float)s / segments, theta = u * glm::two_pi<float>();
            float x = ringR * cosf(theta), z = ringR * sinf(theta);
            glm::vec3 n = glm::normalize(glm::vec3(cosf(theta) * sinf(phi), -cosf(phi), sinf(theta) * sinf(phi)));
            vertices.push_back({x, y, z, n.x, n.y, n.z, u, 0.5f * (1.0f - (1.0f - t))});
        }
    }
    for (int r = 0; r < ringsCap; ++r) {
        for (int s = 0; s < segments; ++s) {
            uint16_t curr = (uint16_t)(baseBottom + r * (segments + 1) + s);
            uint16_t next = (uint16_t)(curr + segments + 1);
            indices.push_back(curr); indices.push_back(next); indices.push_back((uint16_t)(curr + 1));
            indices.push_back(next); indices.push_back((uint16_t)(next + 1)); indices.push_back((uint16_t)(curr + 1));
        }
    }
    
    auto mesh = std::make_shared<Mesh>();
    mesh->vbh = bgfx::createVertexBuffer(bgfx::copy(vertices.data(), (uint32_t)(vertices.size() * sizeof(PBRVertex))), PBRVertex::layout);
    mesh->ibh = bgfx::createIndexBuffer(bgfx::copy(indices.data(), (uint32_t)(indices.size() * sizeof(uint16_t))));
    mesh->numIndices = (uint32_t)indices.size();
    for (auto& v : vertices) mesh->Vertices.push_back(glm::vec3(v.x, v.y, v.z));
    mesh->Indices.assign(indices.begin(), indices.end());
    mesh->ComputeBounds();
    return mesh;
}

// =============================================================================
// Shared Component Loading Context
// =============================================================================

bool ComponentLoadContext::Read(void* dst, size_t count) {
    if (pos + count > size) return false;
    std::memcpy(dst, data + pos, count);
    pos += count;
    return true;
}

// =============================================================================
// EntityBinaryLoader internal context
// =============================================================================

bool EntityBinaryLoader::LoadContext::Read(void* dst, size_t count) {
    if (pos + count > size) return false;
    std::memcpy(dst, data + pos, count);
    pos += count;
    return true;
}

std::string EntityBinaryLoader::LoadContext::ReadString(uint32_t index) const {
    const std::vector<std::string>* table = stringView ? stringView : &strings;
    if (table && index < table->size()) {
        return (*table)[index];
    }
    return "";
}

bool EntityBinaryLoader::Load(const std::string& path, Scene& scene) {
    std::vector<uint8_t> data;
    const std::string cacheKey = MakeSceneLayoutCacheKey(path);
    const uint64_t timestampToken = GetFileTimestampToken(path);
    
    // Try VFS first
    if (VFS::Get() && VFS::Get()->ReadFile(path, data)) {
        return LoadFromMemoryInternal(data.data(), data.size(), scene, cacheKey, timestampToken);
    }
    
    // Fallback to FileSystem
    if (FileSystem::Instance().ReadFile(path, data)) {
        return LoadFromMemoryInternal(data.data(), data.size(), scene, cacheKey, timestampToken);
    }
    
    std::cerr << "[EntityBinaryLoader] Failed to read file: " << path << std::endl;
    return false;
}

bool EntityBinaryLoader::LoadFromMemory(const uint8_t* data, size_t size, Scene& scene) {
    return LoadFromMemoryInternal(data, size, scene, {}, 0);
}

bool EntityBinaryLoader::LoadFromMemoryInternal(const uint8_t* data,
                                                size_t size,
                                                Scene& scene,
                                                const std::string& cacheKey,
                                                uint64_t timestampToken) {
    if (!data || size < sizeof(SceneBinaryHeader)) {
        std::cerr << "[EntityBinaryLoader] Invalid data or size" << std::endl;
        return false;
    }
    
    LoadContext ctx;
    ctx.data = data;
    ctx.size = size;
    ctx.pos = 0;
    
    // Load and validate header
    SceneBinaryHeader header;
    if (!LoadHeader(ctx, header)) {
        return false;
    }
    
    // Store version in context for version-specific component loading
    ctx.version = header.base.version;

    std::shared_ptr<PreparedSceneLayout> preparedLayout =
        TryGetPreparedSceneLayoutCache(cacheKey, header, size, timestampToken);
    if (!preparedLayout) {
        std::string parseError;
        if (!BuildPreparedSceneLayout(data, size, header, preparedLayout, &parseError)) {
            std::cerr << "[EntityBinaryLoader] " << parseError << std::endl;
            return false;
        }
        preparedLayout->timestampToken = timestampToken;
        StorePreparedSceneLayoutCache(cacheKey, preparedLayout);
    }

    ctx.stringView = &preparedLayout->strings;
    ctx.assetRefView = &preparedLayout->assetRefs;

    // Load entities from the prepared metadata state. This mirrors Godot's PackedScene
    // SceneState approach: metadata is decoded once, then instantiation walks arrays.
    if (!LoadPreparedEntities(ctx, header, preparedLayout->entities, scene)) {
        return false;
    }

    std::cout << "[EntityBinaryLoader] Successfully loaded " << header.entityCount << " entities from binary" << std::endl;
    
    // Load environment data (v2+)
    if (header.base.version >= 2 && header.environmentSize > 0) {
        LoadEnvironment(ctx, header, scene);
    }
    
    // Load and apply model deltas (v3+)
    if (header.base.version >= 3 && header.modelDeltaCount > 0) {
        LoadModelDeltas(ctx, header, scene);
    }
    
    // Resolve mesh references and load actual mesh data
    // This is critical for runtime mode where meshes need to be loaded from PAK
    ResolveMeshReferences(ctx, scene);
    
    // Resolve material references and load actual material data
    // Materials are loaded from .matbin files for runtime rendering
    ResolveMaterialReferences(ctx, scene);
    
    // Build avatars for all skeletons (required for humanoid animation retargeting)
    // This must happen before FixupAnimationComponents since animations need avatars
    BuildSkeletonAvatars(scene);
    
    // Fix up animation components - ensure AnimationPlayer is on same entity as Skeleton
    // This is needed because the binary format might store them on different entities
    FixupAnimationComponents(scene);
    
    return true;
}

bool EntityBinaryLoader::BeginStreaming(StreamingContext& stream, const uint8_t* data, size_t size) {
    if (!data || size < sizeof(SceneBinaryHeader)) {
        return false;
    }
    stream = StreamingContext{};
    stream.ctx.data = data;
    stream.ctx.size = size;
    stream.ctx.pos = 0;
    stream.initialized = true;
    return true;
}

float EntityBinaryLoader::GetStreamingProgress(const StreamingContext& stream) {
    if (!stream.initialized) return 0.0f;
    if (stream.complete) return 1.0f;
    
    const float wHeader = 0.05f;
    const float wStrings = 0.10f;
    const float wAssetRefs = 0.10f;
    const float wEntities = 0.45f;
    const float wParents = 0.05f;
    const float wEnvModel = 0.05f;
    const float wMeshes = 0.10f;
    const float wMaterials = 0.10f;
    
    float progress = 0.0f;
    if (stream.headerLoaded) progress += wHeader;
    if (stream.stringsLoaded) progress += wStrings;
    if (stream.assetRefsLoaded) progress += wAssetRefs;
    
    if (stream.header.entityCount > 0) {
        float entityRatio = static_cast<float>(stream.entityIndex) / static_cast<float>(stream.header.entityCount);
        progress += wEntities * std::min(1.0f, entityRatio);
    } else {
        progress += wEntities;
    }
    
    if (stream.parentsApplied) progress += wParents;
    if (stream.envLoaded && stream.modelDeltasLoaded) progress += wEnvModel;
    
    if (stream.entityOrder.empty()) {
        progress += wMeshes + wMaterials;
    } else {
        float meshRatio = static_cast<float>(stream.meshResolveIndex) / static_cast<float>(stream.entityOrder.size());
        float matRatio = static_cast<float>(stream.materialResolveIndex) / static_cast<float>(stream.entityOrder.size());
        progress += wMeshes * std::min(1.0f, meshRatio);
        progress += wMaterials * std::min(1.0f, matRatio);
    }
    
    return std::min(1.0f, progress);
}

bool EntityBinaryLoader::StepStreaming(StreamingContext& stream, Scene& scene,
                                       uint32_t maxEntities,
                                       uint32_t maxMeshes,
                                       uint32_t maxMaterials) {
    if (!stream.initialized || stream.failed || stream.complete) {
        return !stream.failed;
    }
    
    if (!stream.headerLoaded) {
        if (!LoadHeader(stream.ctx, stream.header)) {
            stream.failed = true;
            return false;
        }
        stream.ctx.version = stream.header.base.version;
        stream.headerLoaded = true;
    }
    
    if (!stream.stringsLoaded) {
        if (!LoadStringTable(stream.ctx, stream.header)) {
            stream.failed = true;
            return false;
        }
        stream.stringsLoaded = true;
    }
    
    if (!stream.assetRefsLoaded) {
        if (!LoadAssetRefTable(stream.ctx, stream.header)) {
            stream.failed = true;
            return false;
        }
        stream.assetRefsLoaded = true;
    }
    
    if (stream.entityIndex < stream.header.entityCount) {
        if (stream.entityIndex == 0) {
            stream.ctx.pos = sizeof(SceneBinaryHeader);
            stream.parentRelationships.clear();
            stream.oldToNewIdMap.clear();
            stream.entityOrder.clear();
            stream.entityOrder.reserve(stream.header.entityCount);
        }
        
        uint32_t remaining = stream.header.entityCount - stream.entityIndex;
        uint32_t batch = (maxEntities == 0) ? remaining : std::min(remaining, maxEntities);
        
        for (uint32_t i = 0; i < batch; ++i) {
            if (!LoadSingleEntity(stream.ctx, stream.header, scene,
                                  stream.parentRelationships, stream.oldToNewIdMap, stream.entityOrder)) {
                stream.failed = true;
                return false;
            }
            stream.entityIndex++;
        }
        return true;
    }
    
    if (!stream.parentsApplied) {
        for (const auto& [childId, oldParentId] : stream.parentRelationships) {
            auto it = stream.oldToNewIdMap.find(oldParentId);
            if (it != stream.oldToNewIdMap.end()) {
                scene.SetParent(childId, it->second);
            } else {
                std::cerr << "[EntityBinaryLoader] Could not find parent entity mapping for old ID: " 
                          << oldParentId << std::endl;
            }
        }
        RemapEntityReferences(scene, stream.oldToNewIdMap);
        stream.parentsApplied = true;
        stream.refsRemapped = true;
        if (stream.entityOrder.empty()) {
            for (const Entity& e : scene.GetEntities()) {
                stream.entityOrder.push_back(e.GetID());
            }
        }
        return true;
    }
    
    if (!stream.envLoaded) {
        if (stream.header.base.version >= 2 && stream.header.environmentSize > 0) {
            LoadEnvironment(stream.ctx, stream.header, scene);
        }
        stream.envLoaded = true;
    }
    
    if (!stream.modelDeltasLoaded) {
        if (stream.header.base.version >= 3 && stream.header.modelDeltaCount > 0) {
            LoadModelDeltas(stream.ctx, stream.header, scene);
        }
        stream.modelDeltasLoaded = true;
    }
    
    if (!stream.meshesResolved) {
        if (stream.entityOrder.empty()) {
            stream.meshesResolved = true;
        } else {
            size_t remaining = stream.entityOrder.size() - stream.meshResolveIndex;
            size_t batch = (maxMeshes == 0) ? remaining : std::min(remaining, static_cast<size_t>(maxMeshes));
            bool done = ResolveMeshReferencesRange(stream.ctx, scene, stream.entityOrder,
                                                   stream.meshResolveIndex, batch,
                                                   stream.meshesLoaded, stream.meshesFailed);
            stream.meshResolveIndex = std::min(stream.entityOrder.size(), stream.meshResolveIndex + batch);
            if (done || stream.meshResolveIndex >= stream.entityOrder.size()) {
                std::cout << "[EntityBinaryLoader] Mesh resolution complete: " << stream.meshesLoaded << " loaded, " 
                          << stream.meshesFailed << " failed" << std::endl;
                stream.meshesResolved = true;
            }
        }
        return true;
    }
    
    if (!stream.materialsResolved) {
        if (stream.entityOrder.empty()) {
            stream.materialsResolved = true;
        } else {
            size_t remaining = stream.entityOrder.size() - stream.materialResolveIndex;
            size_t batch = (maxMaterials == 0) ? remaining : std::min(remaining, static_cast<size_t>(maxMaterials));
            bool done = ResolveMaterialReferencesRange(stream.ctx, scene, stream.entityOrder,
                                                       stream.materialResolveIndex, batch,
                                                       stream.materialsLoaded, stream.materialsFailed,
                                                       stream.inlineMaterialsCreated,
                                                       stream.entitiesWithMesh,
                                                       stream.entitiesWithMaterialPaths);
            stream.materialResolveIndex = std::min(stream.entityOrder.size(), stream.materialResolveIndex + batch);
            if (done || stream.materialResolveIndex >= stream.entityOrder.size()) {
                std::cout << "[EntityBinaryLoader] Material resolution complete: " 
                          << stream.materialsLoaded << " loaded, "
                          << stream.materialsFailed << " failed, "
                          << stream.inlineMaterialsCreated << " inline materials, "
                          << stream.entitiesWithMesh << " mesh entities, "
                          << stream.entitiesWithMaterialPaths << " with paths" << std::endl;
                ApplyDefaultMaterials(scene);
                stream.materialsResolved = true;
            }
        }
        return true;
    }
    
    if (!stream.avatarsBuilt) {
        BuildSkeletonAvatars(scene);
        stream.avatarsBuilt = true;
    }
    
    if (!stream.animationsFixed) {
        FixupAnimationComponents(scene);
        stream.animationsFixed = true;
    }
    
    stream.complete = true;
    return true;
}

// Helper to resolve GUID to meshbin path
static std::string ResolveGuidToMeshPath(const ClaymoreGUID& guid) {
    IAssetResolver* resolver = Assets::GetResolver();
    if (resolver) {
        std::string path = resolver->GetPathForGUID(guid);
        if (!path.empty()) {
            std::filesystem::path p(path);
            std::string ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".meshbin") return path;
            if (ext == ".fbx" || ext == ".gltf" || ext == ".glb" || ext == ".obj") {
#if defined(CLAYMORE_EDITOR)
                // In editor/play mode (no pak), meshbin may be in .bin cache; try .meta resolution first
                if (!FileSystem::Instance().IsPakMounted()) {
                    std::filesystem::path metaPath = p;
                    metaPath.replace_extension(".meta");
                    std::string resolved = AssetLibrary::Instance().ResolveMeshBinFromMeta(metaPath.string(), 0);
                    if (!resolved.empty()) return resolved;
                }
#endif
                p.replace_extension(".meshbin");
                return p.string();
            }
            if (ext == ".meta") {
#if defined(CLAYMORE_EDITOR)
                std::string meshBin = AssetLibrary::Instance().ResolveMeshBinFromMeta(path, 0);
                if (!meshBin.empty()) return meshBin;
#endif
            }
            return p.string();
        }
    }
    return "";
}

static bool SceneMeshNeedsUniqueInstance(Scene& scene, const EntityData& data) {
    if (data.BlendShapes && !data.BlendShapes->Shapes.empty()) {
        return true;
    }

    EntityID parentId = data.Parent;
    while (parentId != INVALID_ENTITY_ID) {
        EntityData* parentData = scene.GetEntityData(parentId);
        if (!parentData) {
            break;
        }
        if (parentData->UnifiedMorph && !parentData->UnifiedMorph->Names.empty()) {
            return true;
        }
        parentId = parentData->Parent;
    }

    return false;
}

void EntityBinaryLoader::ResolveMeshReferences(LoadContext& ctx, Scene& scene) {
    int meshesLoaded = 0;
    int meshesFailed = 0;
    
    std::vector<EntityID> entities;
    entities.reserve(scene.GetEntities().size());
    for (const Entity& e : scene.GetEntities()) {
        entities.push_back(e.GetID());
    }
    
    ResolveMeshReferencesRange(ctx, scene, entities, 0, static_cast<size_t>(entities.size()),
                               meshesLoaded, meshesFailed);
    
    std::cout << "[EntityBinaryLoader] Mesh resolution complete: " << meshesLoaded << " loaded, " 
              << meshesFailed << " failed" << std::endl;
}

bool EntityBinaryLoader::ResolveMeshReferencesRange(LoadContext& ctx,
                                                    Scene& scene,
                                                    const std::vector<EntityID>& entities,
                                                    size_t startIndex,
                                                    size_t maxCount,
                                                    int& meshesLoaded,
                                                    int& meshesFailed) {
    if (entities.empty()) return true;
    size_t end = std::min(entities.size(), startIndex + maxCount);
    
    for (size_t i = startIndex; i < end; ++i) {
        EntityID id = entities[i];
        EntityData* data = scene.GetEntityData(id);
        if (!data || !data->Mesh) continue;
        
        // Skip if mesh already loaded
        if (data->Mesh->mesh) continue;
        
        MeshComponent& mc = *data->Mesh;
        const ClaymoreGUID& guid = mc.meshReference.guid;
        
        // Skip if no GUID
        if (guid.high == 0 && guid.low == 0) continue;
        
        // Check if this is a primitive mesh (Cube, Sphere, Plane, Capsule)
        // Create primitives inline to avoid cross-library dependency issues
        if (AssetReference::IsPrimitiveGuid(guid)) {
            AssetReference::PrimitiveType primType = AssetReference::PrimitiveTypeFromGuid(guid);
            std::shared_ptr<Mesh> primMesh = nullptr;
            
            switch (primType) {
                case AssetReference::PrimitiveType::Cube:
                    primMesh = CreatePrimitiveCube();
                    break;
                case AssetReference::PrimitiveType::Sphere:
                    primMesh = CreatePrimitiveSphere();
                    break;
                case AssetReference::PrimitiveType::Plane:
                    primMesh = CreatePrimitivePlane();
                    break;
                case AssetReference::PrimitiveType::Capsule:
                    primMesh = CreatePrimitiveCapsule();
                    break;
                default:
                    break;
            }
            
            if (primMesh) {
                mc.mesh = primMesh;
                meshesLoaded++;
                std::cout << "[EntityBinaryLoader] Loaded primitive mesh for entity: " << data->Name << std::endl;
            } else {
                std::cerr << "[EntityBinaryLoader] Unknown primitive type for entity: " << data->Name << std::endl;
                meshesFailed++;
            }
            continue;
        }
        
        // Try to resolve GUID to path
        std::string meshBinPath = ResolveGuidToMeshPath(guid);
        
        if (meshBinPath.empty()) {
            // Fallback: check asset ref table
            for (const auto& ref : AssetRefsForContext(ctx)) {
                if (ref.guidHigh == guid.high && ref.guidLow == guid.low) {
                    meshBinPath = ctx.ReadString(ref.pathOffset);
                    // Ensure .meshbin extension
                    std::filesystem::path p(meshBinPath);
                    if (p.extension() != ".meshbin") {
                        p.replace_extension(".meshbin");
                    }
                    meshBinPath = p.string();
                    break;
                }
            }
        }
        
        if (meshBinPath.empty()) {
            std::cerr << "[EntityBinaryLoader] Could not resolve mesh GUID: " 
                      << guid.ToString() << " for entity: " << data->Name << std::endl;
            meshesFailed++;
            continue;
        }
        
        // Load the mesh with error handling
        uint32_t submeshIndex = static_cast<uint32_t>(std::max(0, mc.meshReference.fileID));
        bool skinned = false;
        
        try {
            const bool canShareCachedMesh = !SceneMeshNeedsUniqueInstance(scene, *data);
            auto mesh = canShareCachedMesh
                ? MeshCache::GetOrLoadMesh(meshBinPath, submeshIndex, &skinned)
                : MeshBinaryLoader::LoadMesh(meshBinPath, submeshIndex, &skinned);
            if (mesh) {
                mc.mesh = mesh;
                meshesLoaded++;
                std::cout << "[EntityBinaryLoader] Loaded mesh for entity: " << data->Name << std::endl;
            } else {
                std::cerr << "[EntityBinaryLoader] Failed to load mesh: " << meshBinPath 
                          << " for entity: " << data->Name << std::endl;
                meshesFailed++;
            }
        } catch (const std::exception& ex) {
            std::cerr << "[EntityBinaryLoader] Exception loading mesh " << meshBinPath 
                      << ": " << ex.what() << std::endl;
            meshesFailed++;
        } catch (...) {
            std::cerr << "[EntityBinaryLoader] Unknown exception loading mesh " << meshBinPath << std::endl;
            meshesFailed++;
        }
    }
    
    return end >= entities.size();
}

bool EntityBinaryLoader::Validate(const std::string& path) {
    std::vector<uint8_t> data;
    
    if (VFS::Get() && VFS::Get()->ReadFile(path, data)) {
        return ValidateMemory(data.data(), data.size());
    }
    
    if (FileSystem::Instance().ReadFile(path, data)) {
        return ValidateMemory(data.data(), data.size());
    }
    
    return false;
}

bool EntityBinaryLoader::ValidateMemory(const uint8_t* data, size_t size) {
    if (!data || size < sizeof(SceneBinaryHeader)) {
        return false;
    }
    
    const SceneBinaryHeader* header = reinterpret_cast<const SceneBinaryHeader*>(data);
    return header->base.magic == SCENE_MAGIC && header->base.version <= SCENE_VERSION;
}

bool EntityBinaryLoader::LoadHeader(LoadContext& ctx, SceneBinaryHeader& header) {
    if (!ctx.Read(header)) {
        std::cerr << "[EntityBinaryLoader] Failed to read header" << std::endl;
        return false;
    }
    
    if (header.base.magic != SCENE_MAGIC) {
        std::cerr << "[EntityBinaryLoader] Invalid magic number" << std::endl;
        return false;
    }
    
    if (header.base.version > SCENE_VERSION) {
        std::cerr << "[EntityBinaryLoader] Unsupported version: " << header.base.version << std::endl;
        return false;
    }
    
    return true;
}

bool EntityBinaryLoader::LoadStringTable(LoadContext& ctx, const SceneBinaryHeader& header) {
    ctx.stringView = nullptr;
    return ReadStringTableInto(ctx, header, ctx.strings);
}

bool EntityBinaryLoader::LoadAssetRefTable(LoadContext& ctx, const SceneBinaryHeader& header) {
    ctx.assetRefView = nullptr;
    return ReadAssetRefsInto(ctx, header, ctx.assetRefs);
}

bool EntityBinaryLoader::LoadSingleEntity(LoadContext& ctx,
                                          const SceneBinaryHeader& header,
                                          Scene& scene,
                                          std::vector<std::pair<EntityID, EntityID>>& parentRelationships,
                                          std::unordered_map<EntityID, EntityID>& oldToNewIdMap,
                                          std::vector<EntityID>& outEntityOrder) {
    // Read entity header
    uint32_t entityId = 0;
    uint32_t parentId = 0;
    uint32_t nameIndex = 0;
    uint64_t guidHigh = 0, guidLow = 0;
    uint32_t componentCount = 0;
    uint8_t flags = 0;
    
    if (!ctx.Read(entityId)) return false;
    if (!ctx.Read(parentId)) return false;
    if (!ctx.Read(nameIndex)) return false;
    if (!ctx.Read(guidHigh)) return false;
    if (!ctx.Read(guidLow)) return false;
    if (!ctx.Read(flags)) return false;
    
    // v5+: Read layer and tag for full JSON parity
    int32_t layer = 0;
    std::string tag;
    if (header.base.version >= 5) {
        if (!ctx.Read(layer)) return false;
        if (header.base.version >= kSceneEntityTagStringTableVersion) {
            uint32_t tagIndex = 0;
            if (!ctx.Read(tagIndex)) return false;
            tag = ctx.ReadString(tagIndex);
        } else {
            uint32_t tagLen = 0;
            if (!ctx.Read(tagLen)) return false;
            if (tagLen > 0) {
                tag.resize(tagLen);
                if (!ctx.Read(tag.data(), tagLen)) return false;
            }
        }
    }
    
    // v4+: Read ModelAssetGuid if present (flag 0x04)
    uint64_t modelGuidHigh = 0, modelGuidLow = 0;
    if (header.base.version >= 4 && (flags & 0x04)) {
        if (!ctx.Read(modelGuidHigh)) return false;
        if (!ctx.Read(modelGuidLow)) return false;
    }
    
    if (!ctx.Read(componentCount)) return false;
    
    // Create entity with exact name (no suffix)
    std::string name = ctx.ReadString(nameIndex);
    Entity entity = scene.CreateEntityExact(name);
    EntityID newId = entity.GetID();
    outEntityOrder.push_back(newId);
    
    // Store the old->new ID mapping
    oldToNewIdMap[static_cast<EntityID>(entityId)] = newId;
    
    EntityData* data = scene.GetEntityData(newId);
    if (!data) return true;
    
    // Store parent relationship for later (after all entities created)
    // We store the OLD parent ID which will be remapped later
    if (parentId != 0 && parentId != 0xFFFFFFFFu) {
        parentRelationships.emplace_back(newId, static_cast<EntityID>(parentId));
    }
    
    // Set GUID
    data->EntityGuid.high = guidHigh;
    data->EntityGuid.low = guidLow;
    
    // Set ModelAssetGuid (v4+) - marks this as a model root for delta application
    if (modelGuidHigh != 0 || modelGuidLow != 0) {
        data->ModelAssetGuid.high = modelGuidHigh;
        data->ModelAssetGuid.low = modelGuidLow;
    }
    
    // Set flags
    data->Active = (flags & 0x01) != 0;
    data->Visible = (flags & 0x02) != 0;
    
    // Set layer and tag (v5+)
    data->Layer = layer;
    data->Tag = tag;
    
    // Load components
    bool anyComponentFailed = false;
    for (uint32_t c = 0; c < componentCount; ++c) {
        ComponentEntry entry;
        if (!ctx.Read(entry)) return false;
        
        if (!LoadComponent(ctx, entry, scene, newId)) {
            std::cerr << "[EntityBinaryLoader] Failed to load component type " 
                      << static_cast<int>(entry.typeId) << std::endl;
            anyComponentFailed = true;
        }
    }
    if (anyComponentFailed) {
        return false;
    }
    
    return true;
}

bool EntityBinaryLoader::LoadSinglePreparedEntity(LoadContext& ctx,
                                                  const PreparedEntityRecord& record,
                                                  Scene& scene,
                                                  std::vector<std::pair<EntityID, EntityID>>& parentRelationships,
                                                  std::unordered_map<EntityID, EntityID>& oldToNewIdMap,
                                                  std::vector<EntityID>& outEntityOrder) {
    std::string name = ctx.ReadString(record.nameIndex);
    Entity entity = scene.CreateEntityExact(name);
    EntityID newId = entity.GetID();
    outEntityOrder.push_back(newId);

    oldToNewIdMap[static_cast<EntityID>(record.entityId)] = newId;

    EntityData* data = scene.GetEntityData(newId);
    if (!data) {
        return true;
    }

    if (record.parentId != 0 && record.parentId != 0xFFFFFFFFu) {
        parentRelationships.emplace_back(newId, static_cast<EntityID>(record.parentId));
    }

    data->EntityGuid.high = record.guidHigh;
    data->EntityGuid.low = record.guidLow;

    if (record.modelGuidHigh != 0 || record.modelGuidLow != 0) {
        data->ModelAssetGuid.high = record.modelGuidHigh;
        data->ModelAssetGuid.low = record.modelGuidLow;
    }

    data->Active = (record.flags & 0x01) != 0;
    data->Visible = (record.flags & 0x02) != 0;
    data->Layer = record.layer;
    data->Tag = (record.tagIndex != 0xFFFFFFFFu) ? ctx.ReadString(record.tagIndex) : record.legacyTag;

    ctx.pos = record.componentOffset;

    bool anyComponentFailed = false;
    for (uint32_t c = 0; c < record.componentCount; ++c) {
        ComponentEntry entry;
        if (!ctx.Read(entry)) {
            return false;
        }

        if (!LoadComponent(ctx, entry, scene, newId)) {
            std::cerr << "[EntityBinaryLoader] Failed to load component type "
                      << static_cast<int>(entry.typeId) << std::endl;
            anyComponentFailed = true;
        }
    }

    return !anyComponentFailed;
}

bool EntityBinaryLoader::LoadPreparedEntities(LoadContext& ctx,
                                              const SceneBinaryHeader& header,
                                              const std::vector<PreparedEntityRecord>& records,
                                              Scene& scene) {
    if (records.size() != header.entityCount) {
        std::cerr << "[EntityBinaryLoader] Prepared entity count mismatch" << std::endl;
        return false;
    }

    std::vector<std::pair<EntityID, EntityID>> parentRelationships;
    parentRelationships.reserve(records.size());

    std::unordered_map<EntityID, EntityID> oldToNewIdMap;
    oldToNewIdMap.reserve(records.size());

    std::vector<EntityID> entityOrder;
    entityOrder.reserve(records.size());

    for (const PreparedEntityRecord& record : records) {
        if (!LoadSinglePreparedEntity(ctx, record, scene,
                                      parentRelationships, oldToNewIdMap, entityOrder)) {
            return false;
        }
    }

    for (const auto& [childId, oldParentId] : parentRelationships) {
        auto it = oldToNewIdMap.find(oldParentId);
        if (it != oldToNewIdMap.end()) {
            scene.SetParent(childId, it->second);
        } else {
            std::cerr << "[EntityBinaryLoader] Could not find parent entity mapping for old ID: "
                      << oldParentId << std::endl;
        }
    }

    RemapEntityReferences(scene, oldToNewIdMap);

    return true;
}

bool EntityBinaryLoader::LoadEntities(LoadContext& ctx, const SceneBinaryHeader& header, Scene& scene) {
    // Position after header for entity data
    ctx.pos = sizeof(SceneBinaryHeader);
    
    // Store parent relationships for later (child newId, parent oldId)
    std::vector<std::pair<EntityID, EntityID>> parentRelationships;
    
    // Map old entity IDs to new entity IDs (critical for entity references)
    std::unordered_map<EntityID, EntityID> oldToNewIdMap;
    std::vector<EntityID> entityOrder;
    entityOrder.reserve(header.entityCount);
    
    for (uint32_t i = 0; i < header.entityCount; ++i) {
        if (!LoadSingleEntity(ctx, header, scene, parentRelationships, oldToNewIdMap, entityOrder)) {
            return false;
        }
    }
    
    // Apply parent relationships (remap old parent IDs to new IDs)
    for (const auto& [childId, oldParentId] : parentRelationships) {
        auto it = oldToNewIdMap.find(oldParentId);
        if (it != oldToNewIdMap.end()) {
            scene.SetParent(childId, it->second);
        } else {
            std::cerr << "[EntityBinaryLoader] Could not find parent entity mapping for old ID: " 
                      << oldParentId << std::endl;
        }
    }
    
    // Remap entity references in components (Skeleton BoneEntities, Skinning SkeletonRoot, etc.)
    RemapEntityReferences(scene, oldToNewIdMap);
    
    return true;
}

bool EntityBinaryLoader::LoadComponent(LoadContext& ctx, const ComponentEntry& entry, Scene& scene, uint32_t entityId) {
    // Save position after reading ComponentEntry header
    size_t savedPos = ctx.pos;
    
    // Jump to absolute offset where component data is stored
    size_t dataOffset = static_cast<size_t>(entry.dataOffset);
    if (dataOffset > ctx.size || entry.dataSize > ctx.size - dataOffset) {
        std::cerr << "[EntityBinaryLoader] Component data out of bounds (offset=" << entry.dataOffset
                  << ", size=" << entry.dataSize << ", fileSize=" << ctx.size << ")" << std::endl;
        ctx.pos = savedPos;
        return false;
    }
    ctx.pos = dataOffset;
    
    EntityData* data = scene.GetEntityData(entityId);
    if (!data) {
        ctx.pos = savedPos + entry.dataSize;
        return false;
    }
    
    // Create shared context that delegates to our internal context
    ComponentLoadContext compCtx;
    compCtx.data = ctx.data;
    // Constrain reads to this component's payload
    compCtx.size = dataOffset + static_cast<size_t>(entry.dataSize);
    compCtx.pos = ctx.pos;
    compCtx.version = ctx.version;  // Pass version for version-specific loading
    compCtx.readString = [&ctx](uint32_t idx) { return ctx.ReadString(idx); };
    
    // Use shared component loader
    bool success = LoadComponentBinary(compCtx, data, entry.typeId, entry.dataSize);
    
    // Update position from shared context
    ctx.pos = compCtx.pos;
    
    // Advance past component data
    if (savedPos > ctx.size - entry.dataSize) {
        std::cerr << "[EntityBinaryLoader] Component entry size out of bounds (entrySize=" << entry.dataSize
                  << ", fileSize=" << ctx.size << ")" << std::endl;
        ctx.pos = savedPos;
        return false;
    }
    ctx.pos = savedPos + entry.dataSize;
    return success;
}

//==============================================================================
// SHARED COMPONENT LOADING - THE SINGLE SOURCE OF TRUTH
// Used by both EntityBinaryLoader (scenes) and RuntimePrefabInstantiator (prefabs)
//==============================================================================


bool LoadComponentBinary(ComponentLoadContext& ctx, EntityData* data, ComponentTypeId typeId, uint32_t dataSize) {
    bool success = true;
    
    switch (typeId) {
        case ComponentTypeId::Transform: {
            // Position (vec3), RotationQ (quat as vec4 wxyz), Scale (vec3)
            float pos[3], rot[4], scale[3];
            success = ctx.Read(pos, 12) && ctx.Read(rot, 16) && ctx.Read(scale, 12);
            if (success) {
                data->Transform.Position = glm::vec3(pos[0], pos[1], pos[2]);
                // Quaternion stored as w,x,y,z
                data->Transform.RotationQ = glm::quat(rot[0], rot[1], rot[2], rot[3]);
                data->Transform.UseQuatRotation = true;
                data->Transform.Scale = glm::vec3(scale[0], scale[1], scale[2]);
                data->Transform.TransformDirty = true;
            }
            break;
        }
        
        case ComponentTypeId::Mesh: {
            // meshReference GUID, MaterialAssetPaths count + paths
            uint64_t meshGuidHigh = 0, meshGuidLow = 0;
            int32_t meshFileId = 0, meshType = 0;
            uint32_t matCount = 0;
            
            success = ctx.Read(meshGuidHigh) && ctx.Read(meshGuidLow) 
                   && ctx.Read(meshFileId) && ctx.Read(meshType);
            if (success) {
                const bool hadExistingMesh = (data->Mesh != nullptr);
                const bool sameMeshAsset =
                    hadExistingMesh &&
                    data->Mesh->mesh &&
                    data->Mesh->meshReference.guid.high == meshGuidHigh &&
                    data->Mesh->meshReference.guid.low == meshGuidLow &&
                    data->Mesh->meshReference.type == meshType;

                if (!data->Mesh) {
                    data->Mesh = std::make_unique<MeshComponent>();
                } else if (!sameMeshAsset) {
                    *data->Mesh = MeshComponent{};
                }

                MeshComponent& mesh = *data->Mesh;
                if (!sameMeshAsset) {
                    mesh.meshReference.guid.high = meshGuidHigh;
                    mesh.meshReference.guid.low = meshGuidLow;
                    mesh.meshReference.fileID = meshFileId;
                    mesh.meshReference.type = meshType;
                }
                
                // Read material paths
                std::vector<std::string> serializedMaterialPaths;
                if (ctx.Read(matCount)) {
                    serializedMaterialPaths.resize(matCount);
                    for (uint32_t m = 0; m < matCount; ++m) {
                        uint32_t pathIdx = 0;
                        if (ctx.Read(pathIdx)) {
                            serializedMaterialPaths[m] = ctx.ReadString(pathIdx);
                        }
                    }
                }
                bool hasSerializedMaterialPaths = false;
                for (const auto& path : serializedMaterialPaths) {
                    if (!path.empty()) {
                        hasSerializedMaterialPaths = true;
                        break;
                    }
                }
                if (!sameMeshAsset || hasSerializedMaterialPaths) {
                    mesh.MaterialAssetPaths = std::move(serializedMaterialPaths);
                }
                
                // Read PBR/PSX material count
                uint32_t pbrMatCount = 0;
                std::vector<InlineMaterialData> serializedInlineMaterials;
                if (ctx.Read(pbrMatCount) && pbrMatCount > 0) {
                    serializedInlineMaterials.reserve(pbrMatCount);
                    for (uint32_t i = 0; i < pbrMatCount; ++i) {
                        InlineMaterialData matData;
                        uint32_t slotIdx;
                        uint8_t matType;
                        uint32_t albedoIdx, mrIdx, normalIdx, aoIdx, emissionIdx;
                        uint32_t displacementIdx = 0;
                        uint32_t tintMaskIdx = 0;
                        
                        if (!ctx.Read(slotIdx)) break;
                        if (!ctx.Read(matType)) break;
                        matData.slotIndex = slotIdx;
                        matData.materialType = static_cast<InlineMaterialType>(matType);
                        
                        if (!ctx.Read(albedoIdx) || !ctx.Read(mrIdx) || !ctx.Read(normalIdx) ||
                            !ctx.Read(aoIdx) || !ctx.Read(emissionIdx)) break;
                        if (ctx.version >= 29 && !ctx.Read(displacementIdx)) break;
                        if (ctx.version >= 30 && !ctx.Read(tintMaskIdx)) break;
                        
                        matData.albedoPath = ctx.ReadString(albedoIdx);
                        matData.metallicRoughnessPath = ctx.ReadString(mrIdx);
                        matData.normalPath = ctx.ReadString(normalIdx);
                        matData.aoPath = ctx.ReadString(aoIdx);
                        matData.emissionPath = ctx.ReadString(emissionIdx);
                        if (ctx.version >= 29) {
                            matData.displacementPath = ctx.ReadString(displacementIdx);
                        }
                        if (ctx.version >= 30) {
                            matData.tintMaskPath = ctx.ReadString(tintMaskIdx);
                        }
                        
                        if (!ctx.Read(matData.metallic) || !ctx.Read(matData.roughness) ||
                            !ctx.Read(matData.normalScale) || !ctx.Read(matData.aoStrength)) break;
                        if (ctx.version >= 29) {
                            if (!ctx.Read(matData.emissionStrength) ||
                                !ctx.Read(matData.displacementScale) ||
                                !ctx.Read(matData.emissionColor.x) ||
                                !ctx.Read(matData.emissionColor.y) ||
                                !ctx.Read(matData.emissionColor.z) ||
                                !ctx.Read(matData.uvScale.x) ||
                                !ctx.Read(matData.uvScale.y) ||
                                !ctx.Read(matData.uvOffset.x) ||
                                !ctx.Read(matData.uvOffset.y)) break;
                        }
                        
                        if (!ctx.Read(matData.tint.x) || !ctx.Read(matData.tint.y) ||
                            !ctx.Read(matData.tint.z) || !ctx.Read(matData.tint.w)) break;
                        
                        uint8_t hasAlpha;
                        if (!ctx.Read(hasAlpha)) break;
                        matData.hasAlpha = hasAlpha != 0;
                        if (ctx.version >= 8) {
                            uint8_t receiveOverride = 0;
                            uint8_t receive = 0;
                            if (!ctx.Read(receiveOverride) || !ctx.Read(receive)) break;
                            matData.receiveShadowsOverride = receiveOverride != 0;
                            matData.receiveShadows = receive != 0;
                        }
                        
                        // Read PSX-specific properties if this is a PSX material
                        if (matData.materialType == InlineMaterialType::PSX) {
                            ctx.Read(matData.psxParams.x);
                            ctx.Read(matData.psxParams.y);
                            ctx.Read(matData.psxParams.z);
                            ctx.Read(matData.psxParams.w);
                            ctx.Read(matData.psxWorld.x);
                            ctx.Read(matData.psxWorld.y);
                            ctx.Read(matData.psxWorld.z);
                            ctx.Read(matData.psxWorld.w);
                            ctx.Read(matData.toonParams.x);
                            ctx.Read(matData.toonParams.y);
                            ctx.Read(matData.toonParams.z);
                            ctx.Read(matData.toonParams.w);
                        }
                        
                        serializedInlineMaterials.push_back(matData);
                    }
                }
                if (!sameMeshAsset || !serializedInlineMaterials.empty()) {
                    mesh.InlineMaterials = std::move(serializedInlineMaterials);
                }
                
                // Read ShaderGraph material count
                uint32_t sgMatCount = 0;
                std::vector<ShaderGraphMaterialData> serializedShaderGraphMaterials;
                if (ctx.Read(sgMatCount) && sgMatCount > 0) {
                    serializedShaderGraphMaterials.reserve(sgMatCount);
                    for (uint32_t i = 0; i < sgMatCount; ++i) {
                        ShaderGraphMaterialData sgData;
                        uint32_t slotIdx;
                        uint32_t pathIdx, nameIdx, vsNameIdx, fsNameIdx;
                        
                        if (!ctx.Read(slotIdx)) break;
                        if (!ctx.Read(pathIdx) || !ctx.Read(nameIdx) || 
                            !ctx.Read(vsNameIdx) || !ctx.Read(fsNameIdx)) break;
                        
                        sgData.slotIndex = slotIdx;
                        sgData.shaderGraphPath = ctx.ReadString(pathIdx);
                        sgData.name = ctx.ReadString(nameIdx);
                        sgData.compiledVSName = ctx.ReadString(vsNameIdx);
                        sgData.compiledFSName = ctx.ReadString(fsNameIdx);
                        
                        ctx.Read(sgData.uvScale.x);
                        ctx.Read(sgData.uvScale.y);
                        ctx.Read(sgData.uvOffset.x);
                        ctx.Read(sgData.uvOffset.y);
                        ctx.Read(sgData.stateFlags);
                        
                        uint8_t twoSided, alphaClip;
                        ctx.Read(twoSided);
                        ctx.Read(alphaClip);
                        sgData.twoSided = twoSided != 0;
                        sgData.alphaClip = alphaClip != 0;
                        ctx.Read(sgData.alphaClipThreshold);
                        
                        // Read parameters
                        uint32_t paramCount = 0;
                        ctx.Read(paramCount);
                        for (uint32_t p = 0; p < paramCount; ++p) {
                            ShaderGraphParamData param;
                            uint32_t pNameIdx, pDisplayIdx, pTexPathIdx;
                            
                            ctx.Read(pNameIdx);
                            ctx.Read(pDisplayIdx);
                            ctx.Read(param.type);
                            ctx.Read(param.value.x);
                            ctx.Read(param.value.y);
                            ctx.Read(param.value.z);
                            ctx.Read(param.value.w);
                            ctx.Read(pTexPathIdx);
                            ctx.Read(param.textureSlot);
                            
                            param.name = ctx.ReadString(pNameIdx);
                            param.displayName = ctx.ReadString(pDisplayIdx);
                            param.texturePath = ctx.ReadString(pTexPathIdx);
                            
                            sgData.parameters.push_back(param);
                        }
                        
                        serializedShaderGraphMaterials.push_back(sgData);
                        std::cout << "[EntityBinaryLoader]   ShaderGraph[" << slotIdx << "] " << sgData.name 
                                  << " (shaders: " << sgData.compiledVSName << " + " << sgData.compiledFSName << ")" << std::endl;
                    }
                }
                if (!sameMeshAsset || !serializedShaderGraphMaterials.empty()) {
                    mesh.ShaderGraphMaterials = std::move(serializedShaderGraphMaterials);
                }

                // Read per-slot property blocks and texture path overrides (v5+)
                auto remainingBytes = [&](size_t start) -> size_t {
                    if (dataSize == 0) return SIZE_MAX; // unknown size, read optimistically
                    if (ctx.pos >= start + dataSize) return 0;
                    return (start + dataSize) - ctx.pos;
                };

                size_t meshStart = ctx.pos; // current position after materials

                if (remainingBytes(meshStart) >= sizeof(uint32_t)) {
                    uint32_t slotBlockCount = 0;
                    if (ctx.Read(slotBlockCount)) {
                        const bool preserveExistingDefaults = HasFreshSlotPropertyBlocks(*data->Mesh);
                        if (preserveExistingDefaults) {
                            if (data->Mesh->SlotPropertyBlocks.size() < slotBlockCount) {
                                data->Mesh->SlotPropertyBlocks.resize(slotBlockCount);
                            }
                            if (data->Mesh->SlotPropertyBlockTexturePaths.size() < slotBlockCount) {
                                data->Mesh->SlotPropertyBlockTexturePaths.resize(slotBlockCount);
                            }
                        } else {
                            data->Mesh->SlotPropertyBlocks.resize(slotBlockCount);
                            data->Mesh->SlotPropertyBlockTexturePaths.resize(slotBlockCount);
                        }

                        for (uint32_t i = 0; i < slotBlockCount; ++i) {
                            uint32_t vecCount = 0;
                            if (!ctx.Read(vecCount)) break;
                            std::vector<std::pair<std::string, glm::vec4>> serializedVectors;
                            serializedVectors.reserve(vecCount);
                            for (uint32_t v = 0; v < vecCount; ++v) {
                                uint32_t nameIdx = 0;
                                float x, y, z, w;
                                if (!ctx.Read(nameIdx) || !ctx.Read(x) || !ctx.Read(y) || !ctx.Read(z) || !ctx.Read(w)) break;
                                serializedVectors.emplace_back(ctx.ReadString(nameIdx), glm::vec4{x, y, z, w});
                            }

                            uint32_t texCount = 0;
                            if (!ctx.Read(texCount)) break;
                            std::vector<std::pair<std::string, std::string>> serializedTextures;
                            serializedTextures.reserve(texCount);
                            for (uint32_t t = 0; t < texCount; ++t) {
                                uint32_t samplerIdx = 0, pathIdx = 0;
                                if (!ctx.Read(samplerIdx) || !ctx.Read(pathIdx)) break;
                                serializedTextures.emplace_back(ctx.ReadString(samplerIdx), ctx.ReadString(pathIdx));
                            }

                            if (serializedVectors.empty() && serializedTextures.empty() && preserveExistingDefaults) {
                                continue;
                            }

                            data->Mesh->SlotPropertyBlocks[i] = MaterialPropertyBlock{};
                            auto& texPaths = data->Mesh->SlotPropertyBlockTexturePaths[i];
                            texPaths.clear();
                            for (const auto& [name, value] : serializedVectors) {
                                data->Mesh->SlotPropertyBlocks[i].SetVector(name, value);
                            }
                            for (const auto& [samplerName, path] : serializedTextures) {
                                texPaths[samplerName] = path;
                            }
                            RebuildPropertyBlockTexturesFromPathsLocal(data->Mesh->SlotPropertyBlocks[i], texPaths);
                        }
                    }
                }

                // Mesh-level property block + texture paths
                if (remainingBytes(meshStart) >= sizeof(uint32_t)) {
                    uint32_t meshVecCount = 0;
                    std::vector<std::pair<std::string, glm::vec4>> serializedMeshVectors;
                    if (ctx.Read(meshVecCount)) {
                        serializedMeshVectors.reserve(meshVecCount);
                        for (uint32_t v = 0; v < meshVecCount; ++v) {
                            uint32_t nameIdx = 0;
                            float x, y, z, w;
                            if (!ctx.Read(nameIdx) || !ctx.Read(x) || !ctx.Read(y) || !ctx.Read(z) || !ctx.Read(w)) break;
                            serializedMeshVectors.emplace_back(ctx.ReadString(nameIdx), glm::vec4{x, y, z, w});
                        }
                    }

                    uint32_t meshTexCount = 0;
                    std::vector<std::pair<std::string, std::string>> serializedMeshTextures;
                    if (ctx.Read(meshTexCount)) {
                        serializedMeshTextures.reserve(meshTexCount);
                        for (uint32_t t = 0; t < meshTexCount; ++t) {
                            uint32_t samplerIdx = 0, pathIdx = 0;
                            if (!ctx.Read(samplerIdx) || !ctx.Read(pathIdx)) break;
                            serializedMeshTextures.emplace_back(ctx.ReadString(samplerIdx), ctx.ReadString(pathIdx));
                        }
                    }

                    const bool preserveMeshPropertyDefaults =
                        sameMeshAsset &&
                        (!data->Mesh->PropertyBlock.Empty() || !data->Mesh->PropertyBlockTexturePaths.empty());
                    if (!(serializedMeshVectors.empty() && serializedMeshTextures.empty() && preserveMeshPropertyDefaults)) {
                        data->Mesh->PropertyBlock = MaterialPropertyBlock{};
                        data->Mesh->PropertyBlockTexturePaths.clear();
                        for (const auto& [name, value] : serializedMeshVectors) {
                            data->Mesh->PropertyBlock.SetVector(name, value);
                        }
                        for (const auto& [samplerName, path] : serializedMeshTextures) {
                            data->Mesh->PropertyBlockTexturePaths[samplerName] = path;
                        }
                        RebuildPropertyBlockTexturesFromPathsLocal(
                            data->Mesh->PropertyBlock,
                            data->Mesh->PropertyBlockTexturePaths);
                    }
                }
            }
            break;
        }
        
        case ComponentTypeId::Light: {
            const size_t componentStart = ctx.pos;
            uint32_t type = 0;
            float color[3], intensity;
            success = ctx.Read(type) && ctx.Read(color, 12) && ctx.Read(intensity);
            if (success) {
                data->Light = std::make_unique<LightComponent>();
                data->Light->Type = static_cast<LightType>(type);
                data->Light->Color = glm::vec3(color[0], color[1], color[2]);
                data->Light->Intensity = intensity;
                constexpr uint32_t kLegacyLightSize =
                    sizeof(uint32_t) + sizeof(float) * 4;
                constexpr uint32_t kExtendedLightSize =
                    kLegacyLightSize + sizeof(float) * 3;
                if (dataSize >= kExtendedLightSize) {
                    ctx.Read(data->Light->Range);
                    ctx.Read(data->Light->SpotInnerAngleDegrees);
                    ctx.Read(data->Light->SpotOuterAngleDegrees);
                    if (data->Light->SpotOuterAngleDegrees < data->Light->SpotInnerAngleDegrees) {
                        data->Light->SpotOuterAngleDegrees = data->Light->SpotInnerAngleDegrees;
                    }
                }
                // Optional in newer binary formats; defaults to false for older assets.
                if (dataSize == 0 || (ctx.pos - componentStart) < dataSize) {
                    uint8_t pointShadowsEnabled = 0;
                    if (ctx.Read(pointShadowsEnabled)) {
                        data->Light->PointShadowsEnabled = (pointShadowsEnabled != 0);
                    }
                }
            }
            break;
        }
        
        case ComponentTypeId::Camera: {
            float fov, nearClip, farClip;
            uint8_t isActive;
            success = ctx.Read(fov) && ctx.Read(nearClip) && ctx.Read(farClip) && ctx.Read(isActive);
            if (success) {
                data->Camera = std::make_unique<CameraComponent>();
                data->Camera->FieldOfView = fov;
                data->Camera->NearClip = nearClip;
                data->Camera->FarClip = farClip;
                data->Camera->Active = isActive != 0;
                constexpr uint32_t kLegacyCameraSize =
                    sizeof(float) * 3 + sizeof(uint8_t);
                constexpr uint32_t kCameraPrioritySize = sizeof(int32_t);
                constexpr uint32_t kCameraLayerMaskSize = sizeof(uint32_t);
                constexpr uint32_t kCameraPerspectiveSize = sizeof(uint8_t);

                if (dataSize >= kLegacyCameraSize + kCameraPrioritySize) {
                    int32_t priority = data->Camera->priority;
                    if (ctx.Read(priority)) {
                        data->Camera->priority = priority;
                    }
                }
                if (dataSize >= kLegacyCameraSize + kCameraPrioritySize + kCameraLayerMaskSize) {
                    uint32_t layerMask = data->Camera->LayerMask;
                    if (ctx.Read(layerMask)) {
                        data->Camera->LayerMask = layerMask;
                    }
                }
                if (dataSize >= kLegacyCameraSize + kCameraPrioritySize +
                                   kCameraLayerMaskSize + kCameraPerspectiveSize) {
                    uint8_t isPerspective = data->Camera->IsPerspective ? 1 : 0;
                    if (ctx.Read(isPerspective)) {
                        data->Camera->IsPerspective = (isPerspective != 0);
                    }
                }
                std::cout << "[EntityBinaryLoader] Loaded camera: FOV=" << fov 
                          << " Near=" << nearClip << " Far=" << farClip 
                          << " Active=" << (isActive ? "true" : "false")
                          << " Priority=" << data->Camera->priority
                          << " Perspective=" << (data->Camera->IsPerspective ? "true" : "false")
                          << std::endl;
            }
            break;
        }

        case ComponentTypeId::AudioSource: {
            constexpr int32_t kAudioAssetType = 100;
            uint64_t clipGuidHigh = 0;
            uint64_t clipGuidLow = 0;
            uint32_t pathIdx = 0;
            float volume = 1.0f, pitch = 1.0f;
            uint8_t loop = 0, playOnAwake = 1, mute = 0, spatial = 1;
            float minDistance = 1.0f, maxDistance = 50.0f, doppler = 1.0f, rolloff = 1.0f;

            success = ctx.Read(clipGuidHigh) && ctx.Read(clipGuidLow) && ctx.Read(pathIdx) &&
                      ctx.Read(volume) && ctx.Read(pitch) &&
                      ctx.Read(loop) && ctx.Read(playOnAwake) &&
                      ctx.Read(mute) && ctx.Read(spatial) &&
                      ctx.Read(minDistance) && ctx.Read(maxDistance) &&
                      ctx.Read(doppler) && ctx.Read(rolloff);
            if (success) {
                data->AudioSource = std::make_unique<AudioSourceComponent>();
                auto& source = *data->AudioSource;
                ClaymoreGUID clipGuid{clipGuidHigh, clipGuidLow};
                if (clipGuid != ClaymoreGUID()) {
                    source.AudioClip = AssetReference(clipGuid, 0, kAudioAssetType);
                }
                source.AudioPath = ctx.ReadString(pathIdx);
                source.Volume = volume;
                source.Pitch = pitch;
                source.Loop = (loop != 0);
                source.PlayOnAwake = (playOnAwake != 0);
                source.Mute = (mute != 0);
                source.Spatial = (spatial != 0);
                source.MinDistance = minDistance;
                source.MaxDistance = maxDistance;
                source.DopplerFactor = doppler;
                source.Rolloff = rolloff;
            }
            break;
        }

        case ComponentTypeId::AudioListener: {
            uint8_t active = 1;
            int32_t priority = 0;
            float volumeMultiplier = 1.0f;
            success = ctx.Read(active) && ctx.Read(priority) && ctx.Read(volumeMultiplier);
            if (success) {
                data->AudioListener = std::make_unique<AudioListenerComponent>();
                data->AudioListener->Active = (active != 0);
                data->AudioListener->Priority = priority;
                data->AudioListener->VolumeMultiplier = volumeMultiplier;
            }
            break;
        }
        
        case ComponentTypeId::Skeleton: {
            // Read skeleton data
            data->Skeleton = std::make_unique<SkeletonComponent>();
            auto& s = *data->Skeleton;
            
            uint32_t boneCount = 0;
            ctx.Read(boneCount);
            
            // Check if this is v1 (marker only) or v2+ (full data)
            if (boneCount <= 1 && dataSize <= 4) {
                // Old format - just a marker, no actual data
                // Skeleton data will need to be loaded elsewhere
                std::cout << "[EntityBinaryLoader] Skeleton component with legacy marker format" << std::endl;
                break;
            }
            
            // Read inverse bind poses
            s.InverseBindPoses.resize(boneCount);
            for (uint32_t i = 0; i < boneCount; i++) {
                ctx.Read(&s.InverseBindPoses[i][0][0], 64);
            }
            
            // Read bone parents
            s.BoneParents.resize(boneCount);
            for (uint32_t i = 0; i < boneCount; i++) {
                int32_t parent;
                ctx.Read(parent);
                s.BoneParents[i] = parent;
            }
            
            // Read bone names
            s.BoneNames.resize(boneCount);
            for (uint32_t i = 0; i < boneCount; i++) {
                uint32_t nameIdx;
                ctx.Read(nameIdx);
                s.BoneNames[i] = ctx.ReadString(nameIdx);
                s.BoneNameToIndex[s.BoneNames[i]] = static_cast<int>(i);
            }
            
            // Read bone entity IDs
            s.BoneEntities.resize(boneCount);
            for (uint32_t i = 0; i < boneCount; i++) {
                uint32_t boneId;
                ctx.Read(boneId);
                s.BoneEntities[i] = static_cast<EntityID>(boneId);
            }
            
            s.BoneCount = boneCount;
            
            // Read Avatar data (for humanoid animation retargeting)
            uint8_t hasAvatar = 0;
            if (ctx.Read(hasAvatar) && hasAvatar != 0) {
                s.Avatar = std::make_unique<cm::animation::AvatarDefinition>();
                auto& avatar = *s.Avatar;
                
                // Read rig name
                uint32_t rigNameIdx = 0;
                ctx.Read(rigNameIdx);
                avatar.RigName = ctx.ReadString(rigNameIdx);
                
                // Read axes configuration
                uint8_t up = 0, forward = 0, rightHanded = 0;
                ctx.Read(up);
                ctx.Read(forward);
                ctx.Read(rightHanded);
                avatar.Axes.Up = static_cast<cm::animation::AvatarAxes::Axis>(up);
                avatar.Axes.Forward = static_cast<cm::animation::AvatarAxes::Axis>(forward);
                avatar.Axes.RightHanded = (rightHanded != 0);
                ctx.Read(avatar.UnitsPerMeter);
                
                // Read humanoid bone mapping
                uint32_t humanoidCount = 0;
                ctx.Read(humanoidCount);
                avatar.Map.resize(humanoidCount);
                for (uint32_t i = 0; i < humanoidCount; i++) {
                    uint16_t boneEnum = 0;
                    ctx.Read(boneEnum);
                    avatar.Map[i].Bone = static_cast<cm::animation::HumanoidBone>(boneEnum);
                    ctx.Read(avatar.Map[i].BoneIndex);
                    uint32_t boneNameIdx = 0;
                    ctx.Read(boneNameIdx);
                    avatar.Map[i].BoneName = ctx.ReadString(boneNameIdx);
                }
                
                auto remainingBytes = [&]() -> size_t {
                    return (ctx.size > ctx.pos) ? (ctx.size - ctx.pos) : 0;
                };
                auto clampCount = [&](uint32_t count, size_t elemSize, const char* label) -> uint32_t {
                    size_t maxCount = elemSize ? (remainingBytes() / elemSize) : 0;
                    if (elemSize == 0 || count <= maxCount) return count;
                    std::cerr << "[LoadComponentBinary] Avatar " << label
                              << " count " << count << " exceeds remaining bytes "
                              << remainingBytes() << ", clamping to " << maxCount << std::endl;
                    return static_cast<uint32_t>(maxCount);
                };

                // Read BindModel matrices
                uint32_t bindModelCount = 0;
                ctx.Read(bindModelCount);
                bindModelCount = clampCount(bindModelCount, 64, "BindModel");
                avatar.BindModel.resize(bindModelCount);
                for (uint32_t i = 0; i < bindModelCount; i++) {
                    ctx.Read(&avatar.BindModel[i][0][0], 64);
                }
                
                // Read BindLocal matrices
                uint32_t bindLocalCount = 0;
                ctx.Read(bindLocalCount);
                bindLocalCount = clampCount(bindLocalCount, 64, "BindLocal");
                avatar.BindLocal.resize(bindLocalCount);
                for (uint32_t i = 0; i < bindLocalCount; i++) {
                    ctx.Read(&avatar.BindLocal[i][0][0], 64);
                }
                
                // Read Present flags
                uint32_t presentCount = 0;
                ctx.Read(presentCount);
                presentCount = clampCount(presentCount, 1, "Present");
                avatar.Present.resize(presentCount);
                for (uint32_t i = 0; i < presentCount; i++) {
                    uint8_t present = 0;
                    ctx.Read(present);
                    avatar.Present[i] = (present != 0);
                }
                
                // Read RestOffsetRot quaternions
                uint32_t restOffsetCount = 0;
                ctx.Read(restOffsetCount);
                restOffsetCount = clampCount(restOffsetCount, sizeof(glm::quat), "RestOffsetRot");
                avatar.RestOffsetRot.resize(restOffsetCount);
                for (uint32_t i = 0; i < restOffsetCount; i++) {
                    ctx.Read(avatar.RestOffsetRot[i].w);
                    ctx.Read(avatar.RestOffsetRot[i].x);
                    ctx.Read(avatar.RestOffsetRot[i].y);
                    ctx.Read(avatar.RestOffsetRot[i].z);
                }
                
                // Read RetargetModel matrices (precomputed)
                uint32_t retargetCount = 0;
                ctx.Read(retargetCount);
                retargetCount = clampCount(retargetCount, 64, "RetargetModel");
                avatar.RetargetModel.resize(retargetCount);
                for (uint32_t i = 0; i < retargetCount; i++) {
                    ctx.Read(&avatar.RetargetModel[i][0][0], 64);
                }
                
            }
            
            break;
        }
        
        case ComponentTypeId::Skinning: {
            // Read skinning data
            data->Skinning = std::make_unique<SkinningComponent>();
            auto& sk = *data->Skinning;
            
            uint32_t skelRoot;
            ctx.Read(skelRoot);
            sk.SkeletonRoot = static_cast<EntityID>(skelRoot);
            
            // Check if this is old marker format
            if (dataSize <= 5) {
                // Old format - just skeleton root, no additional data
                std::cout << "[EntityBinaryLoader] Skinning component with legacy format" << std::endl;
                break;
            }
            
            uint8_t useParent;
            ctx.Read(useParent);
            sk.UseParentSkeleton = useParent != 0;
            
            // Read original bone names
            uint32_t boneNameCount;
            ctx.Read(boneNameCount);
            sk.OriginalBoneNames.resize(boneNameCount);
            for (uint32_t i = 0; i < boneNameCount; i++) {
                uint32_t nameIdx;
                ctx.Read(nameIdx);
                sk.OriginalBoneNames[i] = ctx.ReadString(nameIdx);
            }
            
            // Read original inverse bind poses
            uint32_t ibpCount;
            ctx.Read(ibpCount);
            sk.OriginalInverseBindPoses.resize(ibpCount);
            for (uint32_t i = 0; i < ibpCount; i++) {
                ctx.Read(&sk.OriginalInverseBindPoses[i][0][0], 64);
            }
            
            std::cout << "[EntityBinaryLoader] Loaded skinning: skelRoot=" << skelRoot 
                      << ", bones=" << boneNameCount << std::endl;
            break;
        }
        
        case ComponentTypeId::RigidBody: {
            float mass, friction, restitution;
            uint8_t useGravity, isKinematic;
            success = ctx.Read(mass) && ctx.Read(friction) && ctx.Read(restitution)
                   && ctx.Read(useGravity) && ctx.Read(isKinematic);
            if (success) {
                data->RigidBody = std::make_unique<RigidBodyComponent>();
                data->RigidBody->Mass = mass;
                data->RigidBody->Friction = friction;
                data->RigidBody->Restitution = restitution;
                data->RigidBody->UseGravity = useGravity != 0;
                data->RigidBody->IsKinematic = isKinematic != 0;
                
                // v5+: Read physics layer and velocities
                if (ctx.version >= 5) {
                    uint32_t layerNameIdx;
                    if (ctx.Read(layerNameIdx)) {
                        data->RigidBody->PhysicsLayerName = ctx.ReadString(layerNameIdx);
                        int32_t idx = PhysicsLayers::PhysicsLayerManager::Get().GetLayerIndex(data->RigidBody->PhysicsLayerName);
                        data->RigidBody->PhysicsLayer = (idx >= 0) ? static_cast<uint32_t>(idx) : 0;
                    }
                    // Initial velocities
                    float lv[3], av[3];
                    if (ctx.Read(lv, 12) && ctx.Read(av, 12)) {
                        data->RigidBody->LinearVelocity = glm::vec3(lv[0], lv[1], lv[2]);
                        data->RigidBody->AngularVelocity = glm::vec3(av[0], av[1], av[2]);
                    }
                    constexpr uint32_t kRigidBodySizeWithCollisionMask =
                        sizeof(float) * 3u + sizeof(uint8_t) * 2u + sizeof(uint32_t) + sizeof(float) * 6u + sizeof(uint32_t);
                    if (dataSize >= kRigidBodySizeWithCollisionMask) {
                        uint32_t collisionMask = data->RigidBody->CollisionMask;
                        if (ctx.Read(collisionMask)) {
                            data->RigidBody->CollisionMask = collisionMask;
                        }
                    }
                }
            }
            break;
        }

        case ComponentTypeId::StaticBody: {
            float friction, restitution;
            uint32_t layerNameIdx;
            success = ctx.Read(friction) && ctx.Read(restitution) && ctx.Read(layerNameIdx);
            if (success) {
                data->StaticBody = std::make_unique<StaticBodyComponent>();
                data->StaticBody->Friction = friction;
                data->StaticBody->Restitution = restitution;
                data->StaticBody->PhysicsLayerName = ctx.ReadString(layerNameIdx);
                int32_t idx = PhysicsLayers::PhysicsLayerManager::Get().GetLayerIndex(data->StaticBody->PhysicsLayerName);
                data->StaticBody->PhysicsLayer = (idx >= 0) ? static_cast<uint32_t>(idx) : 0;
            }
            break;
        }

        case ComponentTypeId::Softbody: {
            uint8_t enabled = 1;
            uint32_t solverIterations = 0;
            float linearDamping = 0.0f, friction = 0.0f, restitution = 0.0f;
            float pressure = 0.0f, gravityFactor = 1.0f, vertexRadius = 0.0f, maxLinearVelocity = 0.0f;
            float edgeCompliance = 0.0f, shearCompliance = 0.0f, bendCompliance = 0.0f;
            uint8_t enableLRA = 0, facesDoubleSided = 0;
            float lraMaxDistance = 1.0f, weightFloor = 0.0f;
            uint32_t bendMode = 0, layerNameIdx = 0;
            uint32_t sourceVertexCount = 0, sourceIndexCount = 0;
            uint32_t weightCount = 0, anchorCount = 0;

            success = ctx.Read(enabled) && ctx.Read(solverIterations);
            success = success && ctx.Read(linearDamping) && ctx.Read(friction) && ctx.Read(restitution);
            success = success && ctx.Read(pressure) && ctx.Read(gravityFactor) && ctx.Read(vertexRadius) && ctx.Read(maxLinearVelocity);
            success = success && ctx.Read(edgeCompliance) && ctx.Read(shearCompliance) && ctx.Read(bendCompliance);
            success = success && ctx.Read(enableLRA) && ctx.Read(lraMaxDistance) && ctx.Read(bendMode);
            success = success && ctx.Read(facesDoubleSided) && ctx.Read(weightFloor) && ctx.Read(layerNameIdx);
            success = success && ctx.Read(sourceVertexCount) && ctx.Read(sourceIndexCount) && ctx.Read(weightCount);
            if (success) {
                data->Softbody = std::make_unique<SoftbodyComponent>();
                auto& sb = *data->Softbody;
                sb.Enabled = enabled != 0;
                sb.SolverIterations = solverIterations;
                sb.LinearDamping = linearDamping;
                sb.Friction = friction;
                sb.Restitution = restitution;
                sb.Pressure = pressure;
                sb.GravityFactor = gravityFactor;
                sb.VertexRadius = vertexRadius;
                sb.MaxLinearVelocity = maxLinearVelocity;
                sb.EdgeCompliance = edgeCompliance;
                sb.ShearCompliance = shearCompliance;
                sb.BendCompliance = bendCompliance;
                sb.EnableLongRangeAttachments = enableLRA != 0;
                sb.LRAMaxDistanceMultiplier = lraMaxDistance;
                sb.BendMode = static_cast<SoftbodyBendMode>(bendMode);
                sb.FacesDoubleSided = facesDoubleSided != 0;
                sb.WeightFloor = weightFloor;
                sb.PhysicsLayerName = ctx.ReadString(layerNameIdx);
                int32_t idx = PhysicsLayers::PhysicsLayerManager::Get().GetLayerIndex(sb.PhysicsLayerName);
                sb.PhysicsLayer = (idx >= 0) ? static_cast<uint32_t>(idx) : 0;
                sb.SourceVertexCount = sourceVertexCount;
                sb.SourceIndexCount = sourceIndexCount;
                sb.VertexWeights.resize(weightCount, 1.0f);
                for (uint32_t i = 0; i < weightCount && success; ++i) {
                    success = ctx.Read(sb.VertexWeights[i]);
                }
                success = success && ctx.Read(anchorCount);
                if (success) {
                    sb.AnchorVertices.resize(anchorCount, 0);
                    if (anchorCount > 0) {
                        success = ctx.Read(sb.AnchorVertices.data(), anchorCount);
                    }
                }
                for (float& weight : sb.VertexWeights) {
                    weight = glm::clamp(weight, 0.0f, 1.0f);
                }
                for (uint8_t& anchor : sb.AnchorVertices) {
                    anchor = anchor != 0 ? 1 : 0;
                }
            }
            break;
        }
        
        case ComponentTypeId::CharacterController: {
            float radius, height;
            float upX, upY, upZ;
            float offsetX, offsetY, offsetZ;
            float maxSlope, jumpSpeed;
            uint8_t stickToFloor, walkStairs;
            uint32_t layerNameIdx;
            
            success = ctx.Read(radius) && ctx.Read(height);
            success = success && ctx.Read(upX) && ctx.Read(upY) && ctx.Read(upZ);
            success = success && ctx.Read(offsetX) && ctx.Read(offsetY) && ctx.Read(offsetZ);
            success = success && ctx.Read(maxSlope) && ctx.Read(jumpSpeed);
            success = success && ctx.Read(stickToFloor) && ctx.Read(walkStairs);
            success = success && ctx.Read(layerNameIdx);
            
            if (success) {
                data->CharacterController = std::make_unique<CharacterControllerComponent>();
                data->CharacterController->Radius = radius;
                data->CharacterController->Height = height;
                data->CharacterController->Up = glm::vec3(upX, upY, upZ);
                data->CharacterController->Offset = glm::vec3(offsetX, offsetY, offsetZ);
                data->CharacterController->MaxSlopeDegrees = maxSlope;
                data->CharacterController->JumpSpeed = jumpSpeed;
                data->CharacterController->StickToFloor = stickToFloor != 0;
                data->CharacterController->EnableWalkStairs = walkStairs != 0;
                data->CharacterController->PhysicsLayerName = ctx.ReadString(layerNameIdx);
                int32_t idx = PhysicsLayers::PhysicsLayerManager::Get().GetLayerIndex(data->CharacterController->PhysicsLayerName);
                data->CharacterController->PhysicsLayer = (idx >= 0) ? static_cast<uint32_t>(idx) : 1; // Default Player layer
                // Optional trailing CollisionMask (added after initial format). Guard on component
                // size so older binaries without the field still load with the default mask.
                constexpr uint32_t kCharacterControllerSizeWithCollisionMask =
                    sizeof(float) * 10u + sizeof(uint8_t) * 2u + sizeof(uint32_t) + sizeof(uint32_t);
                if (dataSize >= kCharacterControllerSizeWithCollisionMask) {
                    uint32_t collisionMask = data->CharacterController->CollisionMask;
                    if (ctx.Read(collisionMask)) {
                        data->CharacterController->CollisionMask = collisionMask;
                    }
                }
                std::cout << "[EntityBinaryLoader] Loaded CharacterController: radius=" << radius
                          << ", height=" << height << std::endl;
            }
            break;
        }
        
        case ComponentTypeId::Collider: {
            uint32_t shapeType;
            float offset[3], size[3], radius, height;
            uint8_t isTrigger;
            success = ctx.Read(shapeType) && ctx.Read(offset, 12) && ctx.Read(size, 12)
                   && ctx.Read(radius) && ctx.Read(height) && ctx.Read(isTrigger);
            if (success) {
                data->Collider = std::make_unique<ColliderComponent>();
                data->Collider->ShapeType = static_cast<ColliderShape>(shapeType);
                data->Collider->Offset = glm::vec3(offset[0], offset[1], offset[2]);
                data->Collider->Size = glm::vec3(size[0], size[1], size[2]);
                data->Collider->Radius = radius;
                data->Collider->Height = height;
                data->Collider->IsTrigger = isTrigger != 0;
                
                // v5+: meshPath and physicsLayer
                if (ctx.version >= 5) {
                    uint32_t meshPathIdx, layerNameIdx;
                    if (ctx.Read(meshPathIdx) && ctx.Read(layerNameIdx)) {
                        data->Collider->MeshPath = ctx.ReadString(meshPathIdx);
                        data->Collider->PhysicsLayerName = ctx.ReadString(layerNameIdx);
                        int32_t idx = PhysicsLayers::PhysicsLayerManager::Get().GetLayerIndex(data->Collider->PhysicsLayerName);
                        data->Collider->PhysicsLayer = (idx >= 0) ? static_cast<uint32_t>(idx) : 0;
                    }
                }
            }
            break;
        }
        
        case ComponentTypeId::Terrain: {
            const size_t componentStart = ctx.pos;
            uint32_t assetPathIdx;
            uint32_t gridResolution;
            float worldSizeX, worldSizeY, maxHeight;
            
            success = ctx.Read(assetPathIdx) && ctx.Read(gridResolution) 
                   && ctx.Read(worldSizeX) && ctx.Read(worldSizeY) && ctx.Read(maxHeight);
            if (success) {
                data->Terrain = std::make_unique<TerrainComponent>();
                std::string assetPath = ctx.ReadString(assetPathIdx);
                data->Terrain->AssetPath = assetPath;
                data->Terrain->GridResolution = gridResolution;
                data->Terrain->WorldSize = glm::vec2(worldSizeX, worldSizeY);
                data->Terrain->MaxHeight = maxHeight;
                
                // Read clipmap settings
                uint8_t useClipmaps = 0;
                uint32_t clipmapLevels = 4;
                uint32_t clipmapGridSize = 64;
                uint8_t clipmapMorphing = 1;
                if (ctx.Read(useClipmaps) && ctx.Read(clipmapLevels) && 
                    ctx.Read(clipmapGridSize) && ctx.Read(clipmapMorphing)) {
                    data->Terrain->UseClipmaps = useClipmaps != 0;
                    data->Terrain->ClipmapLevels = clipmapLevels;
                    data->Terrain->ClipmapGridSize = clipmapGridSize;
                    data->Terrain->ClipmapMorphing = clipmapMorphing != 0;
                    std::cout << "[EntityBinaryLoader] Terrain clipmap settings: UseClipmaps=" 
                              << (data->Terrain->UseClipmaps ? "true" : "false") << std::endl;
                }
                
                // Read chunked terrain settings (Skyrim-style cells with unified textures)
                // Backward-compatible: older binaries omit this block.
                size_t bytesConsumed = ctx.pos - componentStart;
                size_t bytesRemaining = (dataSize > bytesConsumed) ? (dataSize - bytesConsumed) : 0;
                bool hasChunkSettings = false;
                const size_t probeStart = ctx.pos;

                auto remainderLooksValid = [&](size_t remainder, size_t afterLayersPos) -> bool {
                    if (remainder == 0 || remainder == sizeof(uint32_t) * 2) {
                        return true;
                    }
                    if (remainder < sizeof(uint32_t)) {
                        return false;
                    }
                    const size_t savedPos = ctx.pos;
                    ctx.pos = afterLayersPos;
                    uint32_t grassLayerCount = 0;
                    bool ok = ctx.Read(grassLayerCount);
                    ctx.pos = savedPos;
                    if (!ok || grassLayerCount > 512) {
                        return false;
                    }
                    constexpr size_t kGrassLayerBytes = 132;
                    const size_t grassBlockBytes = sizeof(uint32_t) + static_cast<size_t>(grassLayerCount) * kGrassLayerBytes;
                    if (remainder < grassBlockBytes) {
                        return false;
                    }
                    const size_t remainderAfterGrass = remainder - grassBlockBytes;
                    if (ctx.version >= 9) {
                        return remainderAfterGrass >= (sizeof(uint32_t) * 2 + sizeof(uint64_t) * 2 + sizeof(uint32_t) * 2);
                    }
                    return remainderAfterGrass == 0 || remainderAfterGrass == sizeof(uint32_t) * 2;
                };

                if (bytesRemaining >= 4) {
                    uint8_t probeUseChunked = 0;
                    uint32_t probeChunkVertexSize = 0;
                    uint8_t probeChunkMorphing = 0;
                    float probeChunkMorphRegion = 0.0f;
                    uint8_t probeChunkStreaming = 0;
                    float probeStreamingLoadRadius = 0.0f;
                    float probeStreamingUnloadRadius = 0.0f;
                    if (ctx.Read(probeUseChunked) && ctx.Read(probeChunkVertexSize) &&
                        ctx.Read(probeChunkMorphing) && ctx.Read(probeChunkMorphRegion) &&
                        ctx.Read(probeChunkStreaming) && ctx.Read(probeStreamingLoadRadius) &&
                        ctx.Read(probeStreamingUnloadRadius)) {
                        uint32_t probeLayerCount = 0;
                        if (ctx.Read(probeLayerCount)) {
                            const size_t layerBytes = static_cast<size_t>(probeLayerCount) * 28;
                            const size_t afterLayersPos = ctx.pos + layerBytes;
                            if (probeLayerCount <= 512 && afterLayersPos <= componentStart + dataSize) {
                                const size_t remainder = componentStart + dataSize - afterLayersPos;
                                if (remainderLooksValid(remainder, afterLayersPos)) {
                                    hasChunkSettings = true;
                                }
                            }
                        }
                    }
                }
                ctx.pos = probeStart;
                if (hasChunkSettings) {
                    uint8_t useChunkedTerrain = 1;
                    uint32_t chunkVertexSize = 33;
                    uint8_t chunkMorphing = 1;
                    float chunkMorphRegion = 0.3f;
                    uint8_t chunkStreaming = 0;
                    float streamingLoadRadius = 500.0f;
                    float streamingUnloadRadius = 600.0f;
                    if (ctx.Read(useChunkedTerrain) && ctx.Read(chunkVertexSize) &&
                        ctx.Read(chunkMorphing) && ctx.Read(chunkMorphRegion) &&
                        ctx.Read(chunkStreaming) && ctx.Read(streamingLoadRadius) &&
                        ctx.Read(streamingUnloadRadius)) {
                        data->Terrain->UseChunkedTerrain = useChunkedTerrain != 0;
                        data->Terrain->ChunkVertexSize = chunkVertexSize;
                        data->Terrain->ChunkMorphing = chunkMorphing != 0;
                        data->Terrain->ChunkMorphRegion = chunkMorphRegion;
                        data->Terrain->ChunkStreaming = chunkStreaming != 0;
                        data->Terrain->StreamingLoadRadius = streamingLoadRadius;
                        data->Terrain->StreamingUnloadRadius = streamingUnloadRadius;
                        std::cout << "[EntityBinaryLoader] Terrain chunked settings: UseChunkedTerrain=" 
                                  << (data->Terrain->UseChunkedTerrain ? "true" : "false")
                                  << ", ChunkSize=" << chunkVertexSize 
                                  << ", Streaming=" << (data->Terrain->ChunkStreaming ? "true" : "false") << std::endl;
                    }
                }
                
                // Read terrain layers (splatmap materials)
                uint32_t layerCount = 0;
                if (ctx.Read(layerCount)) {
                    const size_t perLayerBytes = (ctx.version >= 10) ? 32u : 28u;
                    size_t layerBytes = static_cast<size_t>(layerCount) * perLayerBytes;
                    if (layerCount > 512 || ctx.pos + layerBytes > componentStart + dataSize) {
                        std::cerr << "[EntityBinaryLoader] Invalid terrain layer count: " << layerCount << std::endl;
                        break;
                    }
                    data->Terrain->Layers.clear();
                    data->Terrain->Layers.reserve(layerCount);
                    for (uint32_t i = 0; i < layerCount; i++) {
                        uint32_t nameIdx, albedoIdx, normalIdx;
                        float tiling;
                        float colorR, colorG, colorB;
                        float navCost = 1.0f;
                        if (ctx.Read(nameIdx) && ctx.Read(albedoIdx) && ctx.Read(normalIdx) &&
                            ctx.Read(tiling) && ctx.Read(colorR) && ctx.Read(colorG) && ctx.Read(colorB) &&
                            (ctx.version < 10 || ctx.Read(navCost))) {
                            TerrainLayerDesc layer;
                            layer.Name = ctx.ReadString(nameIdx);
                            layer.AlbedoPath = ctx.ReadString(albedoIdx);
                            layer.NormalPath = ctx.ReadString(normalIdx);
                            layer.Tiling = tiling;
                            layer.PlaceholderColor = glm::vec3(colorR, colorG, colorB);
                            layer.NavCost = navCost;
                            data->Terrain->Layers.push_back(std::move(layer));
                        }
                    }
                    std::cout << "[EntityBinaryLoader] Loaded " << layerCount << " terrain layers" << std::endl;
                }

                // Optional grass layer metadata (v5+ appended block)
                size_t bytesConsumedAfterLayers = ctx.pos - componentStart;
                size_t bytesRemainingAfterLayers = (dataSize > bytesConsumedAfterLayers) ? (dataSize - bytesConsumedAfterLayers) : 0;
                if (bytesRemainingAfterLayers >= sizeof(uint32_t)) {
                    const size_t grassBlockStart = ctx.pos;
                    uint32_t grassLayerCount = 0;
                    if (ctx.Read(grassLayerCount)) {
                        constexpr size_t kGrassLayerBytes = 132;
                        const size_t requiredBytes = sizeof(uint32_t) + static_cast<size_t>(grassLayerCount) * kGrassLayerBytes;
                        if (grassLayerCount > 512 || bytesRemainingAfterLayers < requiredBytes) {
                            std::cerr << "[EntityBinaryLoader] Invalid grass layer block (count=" << grassLayerCount << ")\n";
                            ctx.pos = grassBlockStart;
                        } else {
                            data->Terrain->GrassLayers.clear();
                            data->Terrain->GrassLayers.reserve(grassLayerCount);
                            for (uint32_t i = 0; i < grassLayerCount; ++i) {
                                uint64_t guidHigh = 0, guidLow = 0;
                                uint32_t nameIdx = 0;
                                uint8_t enabled = 0, useGpu = 0, renderMode = 0, mask = 0;
                                uint32_t splatSeed = 0;
                                float splatNoiseScale = 0.0f, splatNoiseStrength = 0.0f, splatThreshold = 0.2f;
                                float density = 0.0f;
                                float scaleMin = 0.0f, scaleMax = 0.0f;
                                float randomYaw = 0.0f;
                                float heightMin = 0.0f, heightMax = 0.0f;
                                float maxSlope = 0.0f;
                                float minDistance = 0.0f, maxDistance = 0.0f;
                                float windStrength = 0.0f, windDirection = 0.0f;
                                float baseR = 0.0f, baseG = 0.0f, baseB = 0.0f;
                                float varR = 0.0f, varG = 0.0f, varB = 0.0f;
                                uint32_t textureIdx = 0, meshPathIdx = 0;
                                uint64_t meshGuidHigh = 0, meshGuidLow = 0;

                                bool ok = ctx.Read(guidHigh) && ctx.Read(guidLow) &&
                                          ctx.Read(nameIdx) &&
                                          ctx.Read(enabled) && ctx.Read(useGpu) &&
                                          ctx.Read(renderMode) && ctx.Read(mask) &&
                                          ctx.Read(splatSeed) && ctx.Read(splatNoiseScale) && ctx.Read(splatNoiseStrength) &&
                                          ctx.Read(splatThreshold) &&
                                          ctx.Read(density) &&
                                          ctx.Read(scaleMin) && ctx.Read(scaleMax) &&
                                          ctx.Read(randomYaw) &&
                                          ctx.Read(heightMin) && ctx.Read(heightMax) &&
                                          ctx.Read(maxSlope) &&
                                          ctx.Read(minDistance) && ctx.Read(maxDistance) &&
                                          ctx.Read(windStrength) && ctx.Read(windDirection) &&
                                          ctx.Read(baseR) && ctx.Read(baseG) && ctx.Read(baseB) &&
                                          ctx.Read(varR) && ctx.Read(varG) && ctx.Read(varB) &&
                                          ctx.Read(textureIdx) && ctx.Read(meshPathIdx) &&
                                          ctx.Read(meshGuidHigh) && ctx.Read(meshGuidLow);
                                if (!ok) {
                                    std::cerr << "[EntityBinaryLoader] Failed to read grass layer " << i << "\n";
                                    ctx.pos = grassBlockStart;
                                    data->Terrain->GrassLayers.clear();
                                    break;
                                }

                                TerrainGrassLayerDesc layer;
                                layer.Guid.high = guidHigh;
                                layer.Guid.low = guidLow;
                                layer.Name = ctx.ReadString(nameIdx);
                                layer.Enabled = enabled != 0;
                                layer.UseGPU = useGpu != 0;
                                layer.RenderMode = static_cast<GrassRenderMode>(renderMode);
                                layer.Mask = static_cast<GrassMaskSource>(mask);
                                layer.SplatSeed = splatSeed;
                                layer.SplatNoiseScale = splatNoiseScale;
                                layer.SplatNoiseStrength = splatNoiseStrength;
                                layer.SplatThreshold = splatThreshold;
                                layer.DensityPerSquareMeter = density;
                                layer.ScaleRange = glm::vec2(scaleMin, scaleMax);
                                layer.RandomYawDegrees = randomYaw;
                                layer.HeightRange = glm::vec2(heightMin, heightMax);
                                layer.MaxSlopeDegrees = maxSlope;
                                layer.MinDistance = minDistance;
                                layer.MaxDistance = maxDistance;
                                layer.WindStrength = windStrength;
                                layer.WindDirectionDegrees = windDirection;
                                layer.BaseColor = glm::vec3(baseR, baseG, baseB);
                                layer.ColorVariance = glm::vec3(varR, varG, varB);
                                layer.BillboardTexturePath = ctx.ReadString(textureIdx);
                                layer.MeshPath = ctx.ReadString(meshPathIdx);

                                if (meshGuidHigh != 0 || meshGuidLow != 0) {
                                    layer.MeshAsset.guid.high = meshGuidHigh;
                                    layer.MeshAsset.guid.low = meshGuidLow;
                                    // AssetReference uses numeric type codes; Mesh is 3.
                                    layer.MeshAsset.type = 3;
                                }

                                layer.EnsureMaskSize(data->Terrain->GridResolution);
                                data->Terrain->GrassLayers.push_back(std::move(layer));
                            }

                            bytesConsumedAfterLayers = ctx.pos - componentStart;
                            bytesRemainingAfterLayers = (dataSize > bytesConsumedAfterLayers) ? (dataSize - bytesConsumedAfterLayers) : 0;
                            if (bytesRemainingAfterLayers >= sizeof(uint32_t) * 2) {
                                uint32_t grassChunkRes = 0;
                                uint32_t grassSamplingMult = 0;
                                if (ctx.Read(grassChunkRes) && ctx.Read(grassSamplingMult)) {
                                    data->Terrain->GrassChunkResolution = std::max(8u, grassChunkRes);
                                    data->Terrain->GrassSamplingMultiplier = std::clamp(grassSamplingMult, 1u, 16u);
                                }
                            }
                        }
                    }
                }

                // v9+: terrain asset GUID + texture array settings
                size_t bytesConsumedFinal = ctx.pos - componentStart;
                size_t bytesRemainingFinal = (dataSize > bytesConsumedFinal) ? (dataSize - bytesConsumedFinal) : 0;
                if (bytesRemainingFinal >= (sizeof(uint64_t) * 2 + sizeof(uint32_t) * 2)) {
                    uint64_t guidHigh = 0;
                    uint64_t guidLow = 0;
                    uint32_t layerTexRes = 0;
                    uint32_t layerFilter = 0;
                    if (ctx.Read(guidHigh) && ctx.Read(guidLow) && ctx.Read(layerTexRes) && ctx.Read(layerFilter)) {
                        data->Terrain->TerrainDataGuid.high = guidHigh;
                        data->Terrain->TerrainDataGuid.low = guidLow;
                        if (layerTexRes > 0) {
                            data->Terrain->LayerTextureResolution = layerTexRes;
                        }
                        data->Terrain->LayerResizeFilter = static_cast<TerrainTextureFilter>(layerFilter);
                    }
                }

                // v28+: terrain instancer layer metadata. Painted masks are loaded from the terrain asset by GUID.
                bytesConsumedFinal = ctx.pos - componentStart;
                bytesRemainingFinal = (dataSize > bytesConsumedFinal) ? (dataSize - bytesConsumedFinal) : 0;
                if (bytesRemainingFinal >= sizeof(uint32_t)) {
                    const size_t instancerBlockStart = ctx.pos;
                    uint32_t instancerLayerCount = 0;
                    if (ctx.Read(instancerLayerCount)) {
                        constexpr size_t kInstancerLayerBytes = 48;
                        const size_t requiredBytes = sizeof(uint32_t) + static_cast<size_t>(instancerLayerCount) * kInstancerLayerBytes;
                        if (instancerLayerCount > 512 || bytesRemainingFinal < requiredBytes) {
                            ctx.pos = instancerBlockStart;
                        } else {
                            data->Terrain->InstancerLayers.clear();
                            data->Terrain->InstancerLayers.reserve(instancerLayerCount);
                            for (uint32_t i = 0; i < instancerLayerCount; ++i) {
                                uint64_t guidHigh = 0, guidLow = 0;
                                uint32_t nameIdx = 0, physicsLayerNameIdx = 0, instancerJsonIdx = 0;
                                uint8_t enabled = 0, mask = 0, collisionEnabled = 0, sharedMeshShape = 1;
                                float splatThreshold = 0.2f;
                                float activationDistance = 35.0f;
                                uint32_t maxActiveBodies = 128;
                                uint32_t physicsLayer = 0;

                                const bool ok =
                                    ctx.Read(guidHigh) && ctx.Read(guidLow) &&
                                    ctx.Read(nameIdx) &&
                                    ctx.Read(enabled) &&
                                    ctx.Read(mask) &&
                                    ctx.Read(splatThreshold) &&
                                    ctx.Read(collisionEnabled) &&
                                    ctx.Read(activationDistance) &&
                                    ctx.Read(maxActiveBodies) &&
                                    ctx.Read(physicsLayer) &&
                                    ctx.Read(physicsLayerNameIdx) &&
                                    ctx.Read(sharedMeshShape) &&
                                    ctx.Read(instancerJsonIdx);
                                if (!ok) {
                                    ctx.pos = instancerBlockStart;
                                    data->Terrain->InstancerLayers.clear();
                                    break;
                                }

                                TerrainInstancerLayerDesc layer;
                                layer.Guid.high = guidHigh;
                                layer.Guid.low = guidLow;
                                layer.Name = ctx.ReadString(nameIdx);
                                layer.Enabled = enabled != 0;
                                layer.Mask = static_cast<TerrainInstancerMaskSource>(std::clamp(static_cast<int>(mask), 0, 4));
                                layer.SplatThreshold = splatThreshold;
                                layer.Collision.Enabled = collisionEnabled != 0;
                                layer.Collision.ActivationDistance = activationDistance;
                                layer.Collision.MaxActiveBodies = maxActiveBodies;
                                layer.Collision.PhysicsLayer = physicsLayer;
                                layer.Collision.PhysicsLayerName = ctx.ReadString(physicsLayerNameIdx);
                                layer.Collision.UseSharedMeshShape = sharedMeshShape != 0;

                                const std::string instancerJson = ctx.ReadString(instancerJsonIdx);
                                if (!instancerJson.empty()) {
                                    try {
                                        nlohmann::json parsed = nlohmann::json::parse(instancerJson);
                                        Serializer::DeserializeInstancer(parsed, layer.Instancer);
                                    } catch (const std::exception& e) {
                                        std::cerr << "[EntityBinaryLoader] Failed to parse terrain instancer JSON: " << e.what() << "\n";
                                    }
                                }
                                layer.Instancer.SurfaceEntity = INVALID_ENTITY_ID;
                                layer.EnsureMaskSize(data->Terrain->GridResolution);
                                layer.MarkRuntimeDirty();
                                data->Terrain->InstancerLayers.push_back(std::move(layer));
                            }
                        }
                    }
                }

                if (data->Terrain->AssetPath.empty() &&
                    (data->Terrain->TerrainDataGuid.high != 0 || data->Terrain->TerrainDataGuid.low != 0)) {
                    data->Terrain->AssetPath = TerrainComponent::BuildDefaultAssetPath(data->Terrain->TerrainDataGuid);
                }
                
                // Load actual terrain data (heightmap, splatmap) from asset file
                const std::string& terrainAssetPath = data->Terrain->AssetPath;
                if (!ctx.prewarm && !terrainAssetPath.empty()) {
                    if (Terrain::LoadTerrainAsset(terrainAssetPath, *data->Terrain)) {
                        std::cout << "[EntityBinaryLoader] Loaded terrain asset: " << terrainAssetPath << std::endl;
                    } else {
                        std::cerr << "[EntityBinaryLoader] Failed to load terrain asset: " << terrainAssetPath << std::endl;
                    }
                }
            }
            break;
        }
        
        case ComponentTypeId::ParticleEmitter: {
            data->Emitter = std::make_unique<ParticleEmitterComponent>();
            auto& pe = *data->Emitter;
            
            // Basic settings
            uint32_t maxParticles;
            uint8_t enabled;
            uint32_t spritePathIdx;
            success = ctx.Read(maxParticles) && ctx.Read(enabled) && ctx.Read(spritePathIdx);
            if (success)
            {
                pe.MaxParticles = maxParticles;
                pe.Enabled = enabled != 0;
                pe.SpritePath = ctx.ReadString(spritePathIdx);
            }
            
            // Duration & Looping
            float duration;
            uint8_t looping, prewarm, playOnAwake, destroyOnComplete;
            if (ctx.Read(duration) && ctx.Read(looping) && ctx.Read(prewarm) 
                && ctx.Read(playOnAwake) && ctx.Read(destroyOnComplete))
            {
                pe.Duration = duration;
                pe.Looping = looping != 0;
                pe.Prewarm = prewarm != 0;
                pe.PlayOnAwake = playOnAwake != 0;
                pe.DestroyOnComplete = destroyOnComplete != 0;
            }
            
            // Emission
            float emissionRate, burstTime, burstInterval;
            uint8_t burstEnabled;
            int32_t burstCount, burstCycles;
            if (ctx.Read(emissionRate) && ctx.Read(burstEnabled) && ctx.Read(burstCount)
                && ctx.Read(burstTime) && ctx.Read(burstCycles) && ctx.Read(burstInterval))
            {
                pe.EmissionRate = emissionRate;
                pe.BurstEnabled = burstEnabled != 0;
                pe.BurstCount = burstCount;
                pe.BurstTime = burstTime;
                pe.BurstCycles = burstCycles;
                pe.BurstInterval = burstInterval;
            }
            
            // Shape
            int32_t shape;
            float shapeRadius, shapeRadiusThickness, shapeAngle, shapeArc;
            float shapeScaleX, shapeScaleY, shapeScaleZ, shapeLength;
            uint8_t shapeEmitFromEdge, shapeRandomizeDirection;
            if (ctx.Read(shape) && ctx.Read(shapeRadius) && ctx.Read(shapeRadiusThickness)
                && ctx.Read(shapeAngle) && ctx.Read(shapeArc)
                && ctx.Read(shapeScaleX) && ctx.Read(shapeScaleY) && ctx.Read(shapeScaleZ)
                && ctx.Read(shapeLength) && ctx.Read(shapeEmitFromEdge) && ctx.Read(shapeRandomizeDirection))
            {
                pe.Shape = static_cast<ParticleEmissionShape>(shape);
                pe.ShapeRadius = shapeRadius;
                pe.ShapeRadiusThickness = shapeRadiusThickness;
                pe.ShapeAngle = shapeAngle;
                pe.ShapeArc = shapeArc;
                pe.ShapeScale = glm::vec3(shapeScaleX, shapeScaleY, shapeScaleZ);
                pe.ShapeLength = shapeLength;
                pe.ShapeEmitFromEdge = shapeEmitFromEdge != 0;
                pe.ShapeRandomizeDirection = shapeRandomizeDirection != 0;
            }
            
            // Lifetime
            float lifetimeMin, lifetimeMax;
            if (ctx.Read(lifetimeMin) && ctx.Read(lifetimeMax))
            {
                pe.Lifetime.Min = lifetimeMin;
                pe.Lifetime.Max = lifetimeMax;
            }
            
            // Start values
            float startSpeedMin, startSpeedMax, startSizeMin, startSizeMax;
            float startRotationMin, startRotationMax;
            float startColorR, startColorG, startColorB, startColorA;
            if (ctx.Read(startSpeedMin) && ctx.Read(startSpeedMax)
                && ctx.Read(startSizeMin) && ctx.Read(startSizeMax)
                && ctx.Read(startRotationMin) && ctx.Read(startRotationMax)
                && ctx.Read(startColorR) && ctx.Read(startColorG)
                && ctx.Read(startColorB) && ctx.Read(startColorA))
            {
                pe.StartSpeed.Min = startSpeedMin;
                pe.StartSpeed.Max = startSpeedMax;
                pe.StartSize.Min = startSizeMin;
                pe.StartSize.Max = startSizeMax;
                pe.StartRotation.Min = startRotationMin;
                pe.StartRotation.Max = startRotationMax;
                pe.StartColor = glm::vec4(startColorR, startColorG, startColorB, startColorA);
            }
            
            // Physics
            float gravityModifier, inheritVelocity, dragCoefficient;
            int32_t simulationSpace;
            if (ctx.Read(gravityModifier) && ctx.Read(simulationSpace)
                && ctx.Read(inheritVelocity) && ctx.Read(dragCoefficient))
            {
                pe.GravityModifier = gravityModifier;
                pe.SimulationSpace = static_cast<ParticleSimulationSpace>(simulationSpace);
                pe.InheritVelocity = inheritVelocity;
                pe.DragCoefficient = dragCoefficient;
            }
            
            // Size over lifetime
            uint8_t sizeOverLifetimeEnabled;
            int32_t sizeOverLifetimeCurve;
            float sizeOverLifetimeStart, sizeOverLifetimeEnd;
            if (ctx.Read(sizeOverLifetimeEnabled) && ctx.Read(sizeOverLifetimeCurve)
                && ctx.Read(sizeOverLifetimeStart) && ctx.Read(sizeOverLifetimeEnd))
            {
                pe.SizeOverLifetimeEnabled = sizeOverLifetimeEnabled != 0;
                pe.SizeOverLifetime.CurveType = static_cast<ParticleCurveType>(sizeOverLifetimeCurve);
                pe.SizeOverLifetime.StartValue = sizeOverLifetimeStart;
                pe.SizeOverLifetime.EndValue = sizeOverLifetimeEnd;
            }
            
            // Rendering
            int32_t blendMode, renderOrder;
            uint8_t faceCamera;
            if (ctx.Read(blendMode) && ctx.Read(renderOrder) && ctx.Read(faceCamera))
            {
                pe.BlendMode = static_cast<ParticleBlendMode>(blendMode);
                pe.RenderOrder = renderOrder;
                pe.FaceCamera = faceCamera != 0;
            }
            // v7+: Read AlignWithTrajectory (always attempt to read if version >= 7)
            // Default to false if not present (for older versions)
            pe.AlignWithTrajectory = false;
            if (ctx.version >= 7) {
                uint8_t alignWithTrajectory = 0;
                if (ctx.Read(alignWithTrajectory)) {
                    pe.AlignWithTrajectory = alignWithTrajectory != 0;
                } else {
                    // Version >= 7 but failed to read - this shouldn't happen, but default to false
                    std::cerr << "[EntityBinaryLoader] Warning: Failed to read AlignWithTrajectory for version " << ctx.version << std::endl;
                }
            }
            
            // Color gradient
            uint32_t gradientCount;
            if (ctx.Read(gradientCount))
            {
                pe.ColorGradient.clear();
                pe.ColorGradient.reserve(gradientCount);
                for (uint32_t gi = 0; gi < gradientCount; ++gi)
                {
                    ParticleColorKey key;
                    float keyTime, cr, cg, cb, ca;
                    if (ctx.Read(keyTime) && ctx.Read(cr) && ctx.Read(cg) && ctx.Read(cb) && ctx.Read(ca))
                    {
                        key.Time = keyTime;
                        key.Color = glm::vec4(cr, cg, cb, ca);
                        pe.ColorGradient.push_back(key);
                    }
                }
            }

            // v25+: Full inspector parity tail. Read opportunistically from
            // component bounds so older binaries keep default values.
            auto hasBytes = [&](size_t bytes) { return (ctx.pos + bytes) <= ctx.size; };
            uint8_t flag = 0;
            if (hasBytes(sizeof(uint8_t)) && ctx.Read(flag)) {
                pe.StopEmittingOnComplete = flag != 0;
            }
            if (hasBytes(sizeof(uint8_t)) && ctx.Read(flag)) {
                pe.RateOverDistance = flag != 0;
            }
            if (hasBytes(sizeof(uint8_t)) && ctx.Read(flag)) {
                pe.StartColorRandom = flag != 0;
            }
            if (hasBytes(sizeof(float) * 8)) {
                float minR, minG, minB, minA, maxR, maxG, maxB, maxA;
                if (ctx.Read(minR) && ctx.Read(minG) && ctx.Read(minB) && ctx.Read(minA)
                    && ctx.Read(maxR) && ctx.Read(maxG) && ctx.Read(maxB) && ctx.Read(maxA)) {
                    pe.StartColorMin = glm::vec4(minR, minG, minB, minA);
                    pe.StartColorMax = glm::vec4(maxR, maxG, maxB, maxA);
                }
            }
            if (hasBytes(sizeof(uint8_t)) && ctx.Read(flag)) {
                pe.VelocityOverLifetimeEnabled = flag != 0;
            }
            if (hasBytes(sizeof(float) * 5)) {
                float linearX, linearY, linearZ, orbital, radial;
                if (ctx.Read(linearX) && ctx.Read(linearY) && ctx.Read(linearZ)
                    && ctx.Read(orbital) && ctx.Read(radial)) {
                    pe.LinearVelocity = glm::vec3(linearX, linearY, linearZ);
                    pe.OrbitalVelocity = orbital;
                    pe.RadialVelocity = radial;
                }
            }
            if (hasBytes(sizeof(uint8_t)) && ctx.Read(flag)) {
                pe.ColorOverLifetimeEnabled = flag != 0;
            }
            if (hasBytes(sizeof(uint8_t)) && ctx.Read(flag)) {
                pe.RotationOverLifetimeEnabled = flag != 0;
            }
            if (hasBytes(sizeof(float))) {
                float angularVelocity = 0.0f;
                if (ctx.Read(angularVelocity)) {
                    pe.AngularVelocity = angularVelocity;
                }
            }
            if (hasBytes(sizeof(uint8_t)) && ctx.Read(flag)) {
                pe.TextureSheetEnabled = flag != 0;
            }
            if (hasBytes(sizeof(int32_t) * 2 + sizeof(float) + sizeof(uint8_t))) {
                int32_t tilesX = 1;
                int32_t tilesY = 1;
                float frameRate = 30.0f;
                uint8_t randomStart = 0;
                if (ctx.Read(tilesX) && ctx.Read(tilesY) && ctx.Read(frameRate) && ctx.Read(randomStart)) {
                    pe.TextureSheetTilesX = tilesX;
                    pe.TextureSheetTilesY = tilesY;
                    pe.TextureSheetFrameRate = frameRate;
                    pe.TextureSheetRandomStart = randomStart != 0;
                }
            }
            break;
        }
        
        case ComponentTypeId::AnimationPlayer: {
            data->AnimationPlayer = std::make_unique<cm::animation::AnimationPlayerComponent>();
            auto& ap = *data->AnimationPlayer;
            
            // Read mode placeholder (ignored - all animations now go through controller)
            uint32_t mode = 0;
            if (!ctx.Read(mode)) break;
            // Mode field is ignored - kept for backwards compatibility with old binary files
            
            // Read controller path
            uint32_t controllerPathIdx = 0;
            if (!ctx.Read(controllerPathIdx)) break;
            ap.ControllerPath = ctx.ReadString(controllerPathIdx);
            
            // Read single clip path
            uint32_t clipPathIdx = 0;
            if (!ctx.Read(clipPathIdx)) break;
            ap.SingleClipPath = ctx.ReadString(clipPathIdx);

            // v18+/v21+: Read controller override path
            if (ctx.version >= 18) {
                uint32_t overridePathIdx = 0;
                if (!ctx.Read(overridePathIdx)) break;
                ap.ControllerOverridePath = ctx.ReadString(overridePathIdx);
            } else {
                ap.ControllerOverridePath.clear();
            }

            // Normalize VFS paths for portability (match JSON loader behavior)
            auto normalizeVFSPath = [](const std::string& p) -> std::string {
                if (p.empty()) return p;
                std::string normalized = IVirtualFS::NormalizePath(p);
                if (normalized.find("../") != std::string::npos) {
                    size_t assetsPos = normalized.find("/assets/");
                    if (assetsPos != std::string::npos) {
                        return normalized.substr(assetsPos + 1);
                    }
                    assetsPos = normalized.find("assets/");
                    if (assetsPos != std::string::npos && assetsPos > 0) {
                        return normalized.substr(assetsPos);
                    }
                }
                return normalized;
            };
            ap.ControllerPath = normalizeVFSPath(ap.ControllerPath);
            ap.ControllerOverridePath = normalizeVFSPath(ap.ControllerOverridePath);
            ap.SingleClipPath = normalizeVFSPath(ap.SingleClipPath);
            
            // Read flags
            uint8_t playOnStart = 0, loop = 0;
            ctx.Read(playOnStart);
            ctx.Read(loop);
            ap.PlayOnStart = playOnStart != 0;
            if (ap.ActiveStates.empty()) {
                ap.ActiveStates.push_back({});
            }
            ap.ActiveStates.front().Loop = loop != 0;
            
            // Read playback speed
            float speed = 1.0f;
            ctx.Read(speed);
            ap.PlaybackSpeed = speed;
            
            // v5+: Root motion target configuration
            if (ctx.version >= 5) {
                uint32_t motionTarget, explicitTargetEntity;
                if (ctx.Read(motionTarget) && ctx.Read(explicitTargetEntity)) {
                    ap.MotionTarget = static_cast<cm::animation::RootMotionTarget>(motionTarget);
                    ap.ExplicitTargetEntityId = static_cast<EntityID>(explicitTargetEntity);
                }
            }

            if (ctx.version >= 24) {
                uint8_t crowdThrottleEnabled = 1;
                uint8_t lodEnabled = 1;
                uint8_t offscreenDormancyEnabled = 1;
                if (!ctx.Read(crowdThrottleEnabled)) break;
                if (!ctx.Read(lodEnabled)) break;
                if (!ctx.Read(offscreenDormancyEnabled)) break;
                ap.CrowdThrottleEnabled = crowdThrottleEnabled != 0;
                ap.LODEnabled = lodEnabled != 0;
                ap.OffscreenDormancyEnabled = offscreenDormancyEnabled != 0;
                if (!ctx.Read(ap.LODNearDistance)) break;
                if (!ctx.Read(ap.LODMediumDistance)) break;
                if (!ctx.Read(ap.LODFarDistance)) break;
                if (!ctx.Read(ap.LODMediumInterval)) break;
                if (!ctx.Read(ap.LODFarInterval)) break;
                if (!ctx.Read(ap.LODVeryFarInterval)) break;
                if (!ctx.Read(ap.OffscreenNearInterval)) break;
                if (!ctx.Read(ap.OffscreenMediumInterval)) break;
                if (!ctx.Read(ap.OffscreenFarInterval)) break;
                if (!ctx.Read(ap.OffscreenVeryFarInterval)) break;
            }
            
            // Reset runtime state
            ap._InitApplied = false;
            ap.IsPlaying = false;
            ap.InvalidateTargetCache();
            if (!ctx.prewarm) {
                cm::animation::PreloadAnimatorComponent(ap);
            }
            
            break;
        }
        
        case ComponentTypeId::Script: {
            uint32_t scriptCountRaw = 0;
            if (!ctx.Read(scriptCountRaw)) break;
            bool typedFormat = (scriptCountRaw & kScriptBlockTypedFlag) != 0;
            uint32_t scriptCount = scriptCountRaw & ~kScriptBlockTypedFlag;
            
            // Typed format (v5+)
            if (typedFormat) {
                for (uint32_t s = 0; s < scriptCount; ++s) {
                    ScriptInstance instance;
                    
                    uint32_t classNameIdx = 0;
                    if (!ctx.Read(classNameIdx)) break;
                    instance.ClassName = ctx.ReadString(classNameIdx);
                    
                    uint32_t propCount = 0;
                    if (!ctx.Read(propCount)) break;
                    
                    for (uint32_t p = 0; p < propCount; ++p) {
                        uint32_t keyIdx = 0;
                        if (!ctx.Read(keyIdx)) break;
                        std::string key = ctx.ReadString(keyIdx);
                        
                        PropertyType declaredType = PropertyType::Int;
                        if (ScriptReflection::HasProperties(instance.ClassName)) {
                            auto& props = ScriptReflection::GetScriptProperties(instance.ClassName);
                            auto pit = std::find_if(props.begin(), props.end(), [&](const PropertyInfo& info){ return info.name == key; });
                            if (pit != props.end()) {
                                declaredType = pit->type;
                            }
                        }
                        
                        ScriptEntityRefMetadata meta;
                        // Always request entity-ref metadata from typed values.
                        // The binary stream carries value tags (including Entity/List Entity), so this
                        // preserves GUID/model-path hints even when reflection metadata is missing.
                        PropertyValue val = ReadTypedScriptValue(ctx, declaredType, &meta);
                        instance.Values[key] = val;
                        if (meta.entityId != -1 || meta.guid.high != 0 || meta.guid.low != 0 ||
                            meta.modelGuid.high != 0 || meta.modelGuid.low != 0 || !meta.modelNodePath.empty()) {
                            instance.EntityRefMetadata[key] = meta;
                        }
                    }
                    
                    data->Scripts.push_back(std::move(instance));
                }
                break;
            }
            
            // Legacy stringified format (pre-typed)
            for (uint32_t s = 0; s < scriptCount; ++s) {
                ScriptInstance instance;
                
                uint32_t classNameIdx = 0;
                if (!ctx.Read(classNameIdx)) break;
                instance.ClassName = ctx.ReadString(classNameIdx);
                
                uint32_t propCount = 0;
                if (!ctx.Read(propCount)) break;
                
                for (uint32_t p = 0; p < propCount; ++p) {
                    uint32_t keyIdx = 0, valueIdx = 0;
                    if (!ctx.Read(keyIdx) || !ctx.Read(valueIdx)) break;
                    
                    std::string key = ctx.ReadString(keyIdx);
                    std::string valueStr = ctx.ReadString(valueIdx);
                    
                    if (valueStr == "true") {
                        instance.Values[key] = true;
                    } else if (valueStr == "false") {
                        instance.Values[key] = false;
                    } else if (valueStr.find('.') != std::string::npos && 
                               valueStr.find(',') == std::string::npos) {
                        try {
                            instance.Values[key] = std::stof(valueStr);
                        } catch (...) {
                            instance.Values[key] = valueStr;
                        }
                    } else if (valueStr.find(',') != std::string::npos) {
                        std::istringstream iss(valueStr);
                        std::string token;
                        std::vector<float> components;
                        while (std::getline(iss, token, ',')) {
                            try {
                                components.push_back(std::stof(token));
                            } catch (...) {
                                break;
                            }
                        }
                        if (components.size() == 3) {
                            instance.Values[key] = glm::vec3(components[0], components[1], components[2]);
                        } else {
                            instance.Values[key] = valueStr;
                        }
                    } else {
                        try {
                            instance.Values[key] = std::stoi(valueStr);
                        } catch (...) {
                            instance.Values[key] = valueStr;
                        }
                    }
                }
                
                data->Scripts.push_back(std::move(instance));
            }
            break;
        }
        
        // ============================================================================
        // Additional components for full parity with JSON serializer
        // ============================================================================
        
        case ComponentTypeId::MeshProxy: {
            uint32_t targetId = 0;
            uint32_t slotCount = 0;
            success = ctx.Read(targetId) && ctx.Read(slotCount);
            if (success) {
                data->MeshProxy = std::make_unique<MeshProxyComponent>();
                data->MeshProxy->SerializedTarget = static_cast<EntityID>(targetId);
                data->MeshProxy->SubmeshSlots.resize(slotCount);
                for (uint32_t i = 0; i < slotCount; ++i) {
                    uint32_t slot;
                    ctx.Read(slot);
                    data->MeshProxy->SubmeshSlots[i] = slot;
                }
            }
            break;
        }
        
        case ComponentTypeId::BoneAttachment: {
            uint32_t skelEntity = 0;
            uint32_t boneNameIdx = 0;
            float posX, posY, posZ;
            float rotX, rotY, rotZ;
            uint8_t inheritRot, inheritScale;
            
            success = ctx.Read(skelEntity) && ctx.Read(boneNameIdx);
            success = success && ctx.Read(posX) && ctx.Read(posY) && ctx.Read(posZ);
            success = success && ctx.Read(rotX) && ctx.Read(rotY) && ctx.Read(rotZ);
            success = success && ctx.Read(inheritRot) && ctx.Read(inheritScale);
            
            if (success) {
                data->BoneAttachment = std::make_unique<BoneAttachmentComponent>();
                data->BoneAttachment->SkeletonEntity = static_cast<EntityID>(skelEntity);
                data->BoneAttachment->TargetBoneName = ctx.ReadString(boneNameIdx);
                data->BoneAttachment->LocalPosition = glm::vec3(posX, posY, posZ);
                data->BoneAttachment->LocalRotation = glm::vec3(rotX, rotY, rotZ);
                data->BoneAttachment->InheritRotation = inheritRot != 0;
                data->BoneAttachment->InheritScale = inheritScale != 0;
                
                // New fields (v6+): LocalScale and Enabled
                // Check if there's enough data remaining for new fields (12 bytes for scale + 1 byte for enabled)
                // Old format: 4+4+12+12+1+1 = 34 bytes
                // New format: 4+4+12+12+12+1+1+1 = 47 bytes
                float scaleX = 1.0f, scaleY = 1.0f, scaleZ = 1.0f;
                uint8_t enabled = 1;
                
                // Try to read new fields if dataSize indicates new format
                // We detect new format by successfully reading additional bytes
                if (dataSize >= 47 || dataSize == 0) {  // dataSize==0 means unknown/new format
                    if (ctx.Read(scaleX) && ctx.Read(scaleY) && ctx.Read(scaleZ) && ctx.Read(enabled)) {
                        data->BoneAttachment->LocalScale = glm::vec3(scaleX, scaleY, scaleZ);
                        data->BoneAttachment->Enabled = enabled != 0;
                    }
                }
                // If reading new fields failed or wasn't attempted, defaults are already set (scale=1, enabled=true)
            }
            break;
        }
        
        case ComponentTypeId::BlendShape: {
            uint32_t shapeCount = 0;
            if (ctx.Read(shapeCount)) {
                data->BlendShapes = std::make_unique<BlendShapeComponent>();
                data->BlendShapes->Shapes.resize(shapeCount);
                for (uint32_t i = 0; i < shapeCount; ++i) {
                    uint32_t nameIdx;
                    float weight;
                    if (ctx.Read(nameIdx) && ctx.Read(weight)) {
                        data->BlendShapes->Shapes[i].Name = ctx.ReadString(nameIdx);
                        data->BlendShapes->Shapes[i].Weight = weight;
                        data->BlendShapes->Shapes[i].UpdateNameHash();
                    }
                }
            }
            break;
        }
        
        case ComponentTypeId::UnifiedMorph: {
            uint32_t count = 0;
            if (ctx.Read(count)) {
                data->UnifiedMorph = std::make_unique<UnifiedMorphComponent>();
                data->UnifiedMorph->Names.resize(count);
                data->UnifiedMorph->Weights.resize(count);
                // Populate PendingUnifiedMorphWeights for name-based weight application
                // This matches authoring behavior where weights are applied by name matching
                // after MemberMeshes are rebuilt
                for (uint32_t i = 0; i < count; ++i) {
                    uint32_t nameIdx;
                    float weight;
                    if (ctx.Read(nameIdx) && ctx.Read(weight)) {
                        std::string name = ctx.ReadString(nameIdx);
                        data->UnifiedMorph->Names[i] = name;
                        data->UnifiedMorph->Weights[i] = weight;
                        // Store in PendingUnifiedMorphWeights for name-based application after MemberMeshes rebuild
                        data->PendingUnifiedMorphWeights[name] = weight;
                    }
                }
                data->UnifiedMorph->NameIndexDirty = true;
            }
            break;
        }
        
        case ComponentTypeId::TintController: {
            uint8_t useTintMask;
            float baseR, baseG, baseB, baseA;
            float tint0R, tint0G, tint0B, tint0A;
            float tint1R, tint1G, tint1B, tint1A;
            float tint2R, tint2G, tint2B, tint2A;
            float tint3R, tint3G, tint3B, tint3A;
            uint32_t blendMode;
            uint8_t autoIncludeParentedSkinnedMeshes = 1;
            
            success = ctx.Read(useTintMask);
            success = success && ctx.Read(baseR) && ctx.Read(baseG) && ctx.Read(baseB) && ctx.Read(baseA);
            success = success && ctx.Read(tint0R) && ctx.Read(tint0G) && ctx.Read(tint0B) && ctx.Read(tint0A);
            success = success && ctx.Read(tint1R) && ctx.Read(tint1G) && ctx.Read(tint1B) && ctx.Read(tint1A);
            success = success && ctx.Read(tint2R) && ctx.Read(tint2G) && ctx.Read(tint2B) && ctx.Read(tint2A);
            success = success && ctx.Read(tint3R) && ctx.Read(tint3G) && ctx.Read(tint3B) && ctx.Read(tint3A);
            success = success && ctx.Read(blendMode);
            if (success && ctx.version >= 15) {
                success = ctx.Read(autoIncludeParentedSkinnedMeshes);
            }
            
            if (success) {
                data->TintController = std::make_unique<TintMaskController>();
                data->TintController->UseTintMask = useTintMask != 0;
                data->TintController->BaseTint = glm::vec4(baseR, baseG, baseB, baseA);
                data->TintController->TintColor0 = glm::vec4(tint0R, tint0G, tint0B, tint0A);
                data->TintController->TintColor1 = glm::vec4(tint1R, tint1G, tint1B, tint1A);
                data->TintController->TintColor2 = glm::vec4(tint2R, tint2G, tint2B, tint2A);
                data->TintController->TintColor3 = glm::vec4(tint3R, tint3G, tint3B, tint3A);
                data->TintController->GlobalBlendMode = static_cast<TintBlendMode>(blendMode);
                data->TintController->AutoIncludeParentedSkinnedMeshes = autoIncludeParentedSkinnedMeshes != 0;
                
                // Read name pattern
                uint32_t patternLen = 0;
                if (ctx.Read(patternLen) && patternLen > 0) {
                    std::string pattern(patternLen, '\0');
                    if (ctx.Read(pattern.data(), patternLen)) {
                        data->TintController->NamePattern = pattern;
                    }
                }
                
                // Read explicit targets
                uint32_t targetCount = 0;
                if (ctx.Read(targetCount)) {
                    data->TintController->Targets.resize(targetCount);
                    for (uint32_t i = 0; i < targetCount; ++i) {
                        uint32_t targetEntity, targetBlendMode;
                        float multR, multG, multB, multA;
                        if (ctx.Read(targetEntity) && ctx.Read(targetBlendMode)
                            && ctx.Read(multR) && ctx.Read(multG) && ctx.Read(multB) && ctx.Read(multA)) {
                            data->TintController->Targets[i].TargetEntity = static_cast<EntityID>(targetEntity);
                            data->TintController->Targets[i].BlendMode = static_cast<TintBlendMode>(targetBlendMode);
                            data->TintController->Targets[i].Color = glm::vec4(multR, multG, multB, multA);
                            
                            // v5+: MaterialSlot and UseTargetColor
                            if (ctx.version >= 5) {
                                int32_t materialSlot;
                                uint8_t useTargetColor;
                                if (ctx.Read(materialSlot) && ctx.Read(useTargetColor)) {
                                    data->TintController->Targets[i].MaterialSlot = materialSlot;
                                    data->TintController->Targets[i].UseTargetColor = useTargetColor != 0;
                                }
                            }
                        }
                    }
                }

                // v6+: PBR scalar overrides
                if (ctx.version >= 6) {
                    uint8_t usePbrOverrides = 0;
                    float metallic = 0.0f;
                    float roughness = 0.5f;
                    float emissionStrength = 0.0f;
                    float emR = 1.0f, emG = 1.0f, emB = 1.0f;
                    if (ctx.Read(usePbrOverrides) &&
                        ctx.Read(metallic) &&
                        ctx.Read(roughness) &&
                        ctx.Read(emissionStrength) &&
                        ctx.Read(emR) && ctx.Read(emG) && ctx.Read(emB)) {
                        data->TintController->UsePbrOverrides = usePbrOverrides != 0;
                        data->TintController->OverrideMetallic = metallic;
                        data->TintController->OverrideRoughness = roughness;
                        data->TintController->OverrideEmissionStrength = emissionStrength;
                        data->TintController->OverrideEmissionColor = glm::vec3(emR, emG, emB);
                    }
                }
                
                data->TintController->NeedsRefresh = true;
                data->TintController->TintDirty = true;  // Force apply on first frame
            }
            break;
        }
        

        case ComponentTypeId::GrassDeformer: {
            uint8_t enabled;
            float radius, strength;
            if (ctx.Read(enabled) && ctx.Read(radius) && ctx.Read(strength)) {
                data->GrassDeformer = std::make_unique<GrassDeformerComponent>();
                data->GrassDeformer->Enabled = enabled != 0;
                data->GrassDeformer->Radius = radius;
                data->GrassDeformer->Strength = strength;
                
                // v5+: Additional params for full parity
                if (ctx.version >= 5) {
                    float heightOffset;
                    uint8_t useVelocity;
                    if (ctx.Read(heightOffset) && ctx.Read(useVelocity)) {
                        data->GrassDeformer->HeightOffset = heightOffset;
                        data->GrassDeformer->UseVelocity = useVelocity != 0;
                    }
                }
            }
            break;
        }
        
        case ComponentTypeId::River: {
            float width, depth, flowSpeed;
            uint32_t pointCount;
            if (ctx.Read(width) && ctx.Read(depth) && ctx.Read(flowSpeed) && ctx.Read(pointCount)) {
                data->River = std::make_unique<RiverComponent>();
                data->River->Width = width;
                data->River->Depth = depth;
                data->River->FlowSpeed = flowSpeed;
                data->River->PathPoints.resize(pointCount);
                for (uint32_t i = 0; i < pointCount; ++i) {
                    float x, y, z;
                    if (ctx.Read(x) && ctx.Read(y) && ctx.Read(z)) {
                        data->River->PathPoints[i].Position = glm::vec3(x, y, z);
                    }
                }
            }
            break;
        }
        
        case ComponentTypeId::Spline: {
            int32_t subdivision;
            uint8_t closed;
            uint32_t pointCount;
            if (ctx.Read(subdivision) && ctx.Read(closed) && ctx.Read(pointCount)) {
                data->Spline = std::make_unique<SplineComponent>();
                data->Spline->SplineSubdivision = subdivision;
                data->Spline->Closed = (closed != 0);
                data->Spline->ControlPoints.resize(pointCount);
                for (uint32_t i = 0; i < pointCount; ++i) {
                    float x, y, z, nx, ny, nz;
                    if (ctx.Read(x) && ctx.Read(y) && ctx.Read(z) && ctx.Read(nx) && ctx.Read(ny) && ctx.Read(nz)) {
                        data->Spline->ControlPoints[i].Position = glm::vec3(x, y, z);
                        data->Spline->ControlPoints[i].Normal = glm::vec3(nx, ny, nz);
                    }
                }
            }
            break;
        }
        
        case ComponentTypeId::Area: {
            uint32_t shapeType;
            float sizeX, sizeY, sizeZ, radius, height;
            uint8_t enabled, monitorBodies, monitorAreas;
            uint32_t collisionLayer, collisionMask;
            
            success = ctx.Read(shapeType) && ctx.Read(sizeX) && ctx.Read(sizeY) && ctx.Read(sizeZ);
            success = success && ctx.Read(radius) && ctx.Read(height);
            success = success && ctx.Read(enabled) && ctx.Read(monitorBodies) && ctx.Read(monitorAreas);
            success = success && ctx.Read(collisionLayer) && ctx.Read(collisionMask);
            
            if (success) {
                data->Area = std::make_unique<cm::physics::AreaComponent>();
                data->Area->ShapeType = static_cast<cm::physics::AreaShapeType>(shapeType);
                data->Area->Size = glm::vec3(sizeX, sizeY, sizeZ);
                data->Area->Radius = radius;
                data->Area->Height = height;
                data->Area->Enabled = enabled != 0;
                data->Area->MonitorBodies = monitorBodies != 0;
                data->Area->MonitorAreas = monitorAreas != 0;
                data->Area->CollisionLayer = collisionLayer;
                data->Area->CollisionMask = collisionMask;
                
                // v5+: Additional params for full parity
                if (ctx.version >= 5) {
                    float offsetX, offsetY, offsetZ;
                    uint8_t effects;
                    float gravityOverride, linearDamp, angularDamp;
                    int32_t priority;
                    if (ctx.Read(offsetX) && ctx.Read(offsetY) && ctx.Read(offsetZ) &&
                        ctx.Read(effects) && ctx.Read(gravityOverride) &&
                        ctx.Read(linearDamp) && ctx.Read(angularDamp) && ctx.Read(priority)) {
                        data->Area->Offset = glm::vec3(offsetX, offsetY, offsetZ);
                        data->Area->Effects = static_cast<cm::physics::AreaSpaceEffect>(effects);
                        data->Area->GravityOverride = gravityOverride;
                        data->Area->LinearDamp = linearDamp;
                        data->Area->AngularDamp = angularDamp;
                        data->Area->Priority = priority;
                    }
                }
            }
            break;
        }
        
        case ComponentTypeId::RenderOverrides: {
            uint8_t alpha, useAlphaCutout = 0, depthWrite, castShadows, receiveShadows;
            float alphaCutoutThreshold = 0.5f;
            // Old format: 4 bytes (alpha, depthWrite, castShadows, receiveShadows)
            // New format: 9 bytes (alpha, useAlphaCutout, alphaCutoutThreshold(float), depthWrite, castShadows, receiveShadows)
            bool success = false;
            if (dataSize >= 9) {
                // New format with alpha cutout
                success = ctx.Read(alpha) && ctx.Read(useAlphaCutout) && ctx.Read(alphaCutoutThreshold) &&
                          ctx.Read(depthWrite) && ctx.Read(castShadows) && ctx.Read(receiveShadows);
            } else if (dataSize == 4) {
                // Old format without alpha cutout
                success = ctx.Read(alpha) && ctx.Read(depthWrite) && ctx.Read(castShadows) && ctx.Read(receiveShadows);
                useAlphaCutout = 0;
                alphaCutoutThreshold = 0.5f;
            }
            if (success) {
                data->RenderOverrides = std::make_unique<RenderOverridesComponent>();
                data->RenderOverrides->AlphaBlendEnabled = alpha != 0;
                data->RenderOverrides->UseAlphaCutout = useAlphaCutout != 0;
                data->RenderOverrides->AlphaCutoutThreshold = alphaCutoutThreshold;
                data->RenderOverrides->DepthWriteEnabled = depthWrite != 0;
                data->RenderOverrides->CastShadows = castShadows != 0;
                data->RenderOverrides->ReceiveShadows = receiveShadows != 0;
            }
            break;
        }
        
        case ComponentTypeId::Canvas: {
            uint32_t space;
            int32_t width, height;
            float dpiScale;
            int32_t sortOrder;
            float opacity;
            uint8_t blockInput;
            
            success = ctx.Read(space) && ctx.Read(width) && ctx.Read(height);
            success = success && ctx.Read(dpiScale) && ctx.Read(sortOrder);
            success = success && ctx.Read(opacity) && ctx.Read(blockInput);
            
            if (success) {
                data->Canvas = std::make_unique<CanvasComponent>();
                data->Canvas->Space = static_cast<CanvasComponent::RenderSpace>(space);
                data->Canvas->Width = width;
                data->Canvas->Height = height;
                data->Canvas->DPIScale = dpiScale;
                data->Canvas->SortOrder = sortOrder;
                data->Canvas->Opacity = opacity;
                data->Canvas->BlockSceneInput = blockInput != 0;
                // v17+: optional reference-resolution settings
                auto hasBytes = [&](size_t bytes) { return (ctx.pos + bytes) <= ctx.size; };
                if (hasBytes(sizeof(int32_t) * 2 + sizeof(uint32_t))) {
                    int32_t refW = 0;
                    int32_t refH = 0;
                    uint32_t scaleMode = 0;
                    if (ctx.Read(refW) && ctx.Read(refH) && ctx.Read(scaleMode)) {
                        data->Canvas->ReferenceWidth = refW;
                        data->Canvas->ReferenceHeight = refH;
                        data->Canvas->ReferenceScaleMode = static_cast<CanvasComponent::ScaleMode>(scaleMode);
                    }
                }
                if (hasBytes(sizeof(uint8_t))) {
                    uint8_t billboard = 1;
                    if (ctx.Read(billboard)) {
                        data->Canvas->Billboard = billboard != 0;
                    }
                }
            }
            break;
        }
        
        case ComponentTypeId::Panel: {
            float posX, posY, sizeX, sizeY;
            float scaleX, scaleY;
            float pivotX, pivotY;
            float rotation;
            float tintR, tintG, tintB, tintA;
            float opacity;
            uint8_t anchorEnabled;
            uint32_t anchor;
            float anchorOffX, anchorOffY;
            uint8_t visible;
            int32_t zOrder;
            uint8_t allowDrag = 0;
            uint8_t allowDrop = 0;
            uint8_t anchorToParentUI = 0;
            uint8_t driveChildrenOpacity = 0;
            // Texture
            uint64_t texGuidHigh, texGuidLow;
            int64_t texFileID;
            uint32_t texType;
            // UV and fill mode
            float uvX, uvY, uvZ, uvW;
            uint32_t fillMode;
            float tileX, tileY;
            float sliceUvX, sliceUvY, sliceUvZ, sliceUvW;
            float sliceBorderX = 0.0f, sliceBorderY = 0.0f, sliceBorderZ = 0.0f, sliceBorderW = 0.0f;
            
            size_t readStartPos = ctx.pos;
            
            // Read fields in-order; keep logging quiet unless final load fails.
            #define READ_AND_LOG(field, name, expectedSize) \
                do { \
                    bool readOk = ctx.Read(field); \
                    success = success && readOk; \
                } while(0)
            
            READ_AND_LOG(posX, "posX", 4);
            READ_AND_LOG(posY, "posY", 4);
            READ_AND_LOG(sizeX, "sizeX", 4);
            READ_AND_LOG(sizeY, "sizeY", 4);
            READ_AND_LOG(scaleX, "scaleX", 4);
            READ_AND_LOG(scaleY, "scaleY", 4);
            READ_AND_LOG(pivotX, "pivotX", 4);
            READ_AND_LOG(pivotY, "pivotY", 4);
            READ_AND_LOG(rotation, "rotation", 4);
            READ_AND_LOG(tintR, "tintR", 4);
            READ_AND_LOG(tintG, "tintG", 4);
            READ_AND_LOG(tintB, "tintB", 4);
            READ_AND_LOG(tintA, "tintA", 4);
            READ_AND_LOG(opacity, "opacity", 4);
            READ_AND_LOG(anchorEnabled, "anchorEnabled", 1);
            READ_AND_LOG(anchor, "anchor", 4);
            READ_AND_LOG(anchorOffX, "anchorOffX", 4);
            READ_AND_LOG(anchorOffY, "anchorOffY", 4);
            READ_AND_LOG(visible, "visible", 1);
            READ_AND_LOG(zOrder, "zOrder", 4);
            // Read texture
            READ_AND_LOG(texGuidHigh, "texGuidHigh", 8);
            READ_AND_LOG(texGuidLow, "texGuidLow", 8);
            READ_AND_LOG(texFileID, "texFileID", 8);
            READ_AND_LOG(texType, "texType", 4);
            // Read UV and fill mode
            READ_AND_LOG(uvX, "uvX", 4);
            READ_AND_LOG(uvY, "uvY", 4);
            READ_AND_LOG(uvZ, "uvZ", 4);
            READ_AND_LOG(uvW, "uvW", 4);
            READ_AND_LOG(fillMode, "fillMode", 4);
            READ_AND_LOG(tileX, "tileX", 4);
            READ_AND_LOG(tileY, "tileY", 4);
            READ_AND_LOG(sliceUvX, "sliceUvX", 4);
            READ_AND_LOG(sliceUvY, "sliceUvY", 4);
            READ_AND_LOG(sliceUvZ, "sliceUvZ", 4);
            READ_AND_LOG(sliceUvW, "sliceUvW", 4);
            READ_AND_LOG(sliceBorderX, "sliceBorderX", 4);
            READ_AND_LOG(sliceBorderY, "sliceBorderY", 4);
            
            // Read sliceBorderZ and sliceBorderW only if enough bytes remain
            // This makes the reader dataSize-aware and backward-compatible
            // Rule: Optional fields are gated by remaining bytes
            size_t remainingBytes = ctx.size - ctx.pos;
            
            if (remainingBytes >= sizeof(float)) {
                READ_AND_LOG(sliceBorderZ, "sliceBorderZ", 4);
            } else {
                sliceBorderZ = 0.0f;  // Default value
            }
            
            remainingBytes = ctx.size - ctx.pos;
            
            if (remainingBytes >= sizeof(float)) {
                READ_AND_LOG(sliceBorderW, "sliceBorderW", 4);
            } else {
                sliceBorderW = 0.0f;  // Default value
            }

            remainingBytes = ctx.size - ctx.pos;
            if (remainingBytes >= sizeof(uint8_t)) {
                READ_AND_LOG(allowDrag, "allowDrag", 1);
            }

            remainingBytes = ctx.size - ctx.pos;
            if (remainingBytes >= sizeof(uint8_t)) {
                READ_AND_LOG(allowDrop, "allowDrop", 1);
            }

            remainingBytes = ctx.size - ctx.pos;
            if (remainingBytes >= sizeof(uint8_t)) {
                READ_AND_LOG(anchorToParentUI, "anchorToParentUI", 1);
            }

            remainingBytes = ctx.size - ctx.pos;
            if (remainingBytes >= sizeof(uint8_t)) {
                READ_AND_LOG(driveChildrenOpacity, "driveChildrenOpacity", 1);
            }
            
            #undef READ_AND_LOG
            
            // Check if we read past the end (this should never happen if dataSize is correct)
            if (ctx.pos > ctx.size) {
                success = false;
            }
            
            if (!success) {
                // Check if failure was due to missing optional fields (sliceBorderZ/W)
                // If so, continue with defaults rather than failing the entire component
                size_t bytesReadSoFar = ctx.pos - readStartPos;
                if (bytesReadSoFar >= 154) {  // At least got through sliceBorderY (154 bytes)
                    success = true;  // Allow component to load with defaults
                }
            }
            
            if (success) {
                data->Panel = std::make_unique<PanelComponent>();
                data->Panel->Position = glm::vec2(posX, posY);
                data->Panel->Size = glm::vec2(sizeX, sizeY);
                data->Panel->Scale = glm::vec2(scaleX, scaleY);
                data->Panel->Pivot = glm::vec2(pivotX, pivotY);
                data->Panel->Rotation = rotation;
                data->Panel->TintColor = glm::vec4(tintR, tintG, tintB, tintA);
                data->Panel->Opacity = opacity;
                data->Panel->AnchorEnabled = anchorEnabled != 0;
                data->Panel->AnchorToParentUI = anchorToParentUI != 0;
                data->Panel->Anchor = static_cast<UIAnchorPreset>(anchor);
                data->Panel->AnchorOffset = glm::vec2(anchorOffX, anchorOffY);
                data->Panel->Visible = visible != 0;
                data->Panel->AllowDrag = allowDrag != 0;
                data->Panel->AllowDrop = allowDrop != 0;
                data->Panel->DriveChildrenOpacity = driveChildrenOpacity != 0;
                data->Panel->ZOrder = zOrder;
                // Texture
                data->Panel->Texture.guid.high = texGuidHigh;
                data->Panel->Texture.guid.low = texGuidLow;
                data->Panel->Texture.fileID = texFileID;
                data->Panel->Texture.type = texType;
                // UV and fill mode
                data->Panel->UVRect = glm::vec4(uvX, uvY, uvZ, uvW);
                data->Panel->Mode = static_cast<PanelComponent::FillMode>(fillMode);
                data->Panel->TileRepeat = glm::vec2(tileX, tileY);
                data->Panel->SliceUV = glm::vec4(sliceUvX, sliceUvY, sliceUvZ, sliceUvW);
                data->Panel->SliceBorder = glm::vec4(sliceBorderX, sliceBorderY, sliceBorderZ, sliceBorderW);
            } else {
                std::cerr << "[LoadComponentBinary] Failed to read PanelComponent data "
                          << "(pos=" << ctx.pos << ", size=" << ctx.size
                          << ", available=" << (ctx.size - ctx.pos) << " bytes)" << std::endl;
            }
            break;
        }
        
        case ComponentTypeId::Button: {
            float normalR, normalG, normalB, normalA;
            float hoverR, hoverG, hoverB, hoverA;
            float pressedR, pressedG, pressedB, pressedA;
            float disabledR, disabledG, disabledB, disabledA;
            uint8_t interactable, toggle;
            
            success = ctx.Read(normalR) && ctx.Read(normalG) && ctx.Read(normalB) && ctx.Read(normalA);
            success = success && ctx.Read(hoverR) && ctx.Read(hoverG) && ctx.Read(hoverB) && ctx.Read(hoverA);
            success = success && ctx.Read(pressedR) && ctx.Read(pressedG) && ctx.Read(pressedB) && ctx.Read(pressedA);
            success = success && ctx.Read(disabledR) && ctx.Read(disabledG) && ctx.Read(disabledB) && ctx.Read(disabledA);
            success = success && ctx.Read(interactable) && ctx.Read(toggle);
            
            if (success) {
                data->Button = std::make_unique<ButtonComponent>();
                data->Button->NormalTint = glm::vec4(normalR, normalG, normalB, normalA);
                data->Button->HoverTint = glm::vec4(hoverR, hoverG, hoverB, hoverA);
                data->Button->PressedTint = glm::vec4(pressedR, pressedG, pressedB, pressedA);
                data->Button->DisabledTint = glm::vec4(disabledR, disabledG, disabledB, disabledA);
                data->Button->Interactable = interactable != 0;
                data->Button->Toggle = toggle != 0;
            }
            break;
        }
        
        case ComponentTypeId::Slider: {
            float minVal, maxVal, value;
            uint8_t wholeNumbers;
            uint32_t direction;
            uint8_t interactable;
            
            success = ctx.Read(minVal) && ctx.Read(maxVal) && ctx.Read(value);
            success = success && ctx.Read(wholeNumbers) && ctx.Read(direction) && ctx.Read(interactable);
            
            if (success) {
                data->Slider = std::make_unique<SliderComponent>();
                data->Slider->MinValue = minVal;
                data->Slider->MaxValue = maxVal;
                data->Slider->Value = value;
                data->Slider->WholeNumbers = wholeNumbers != 0;
                data->Slider->SliderDirection = static_cast<SliderComponent::Direction>(direction);
                data->Slider->Interactable = interactable != 0;

                // v17+: optional full slider payload
                auto hasBytes = [&](size_t bytes) { return (ctx.pos + bytes) <= ctx.size; };
                if (hasBytes(sizeof(float))) {
                    float step = 0.0f;
                    if (ctx.Read(step)) data->Slider->Step = step;
                }
                if (hasBytes(sizeof(float) * 2)) {
                    float handleX = 0.0f, handleY = 0.0f;
                    if (ctx.Read(handleX) && ctx.Read(handleY)) {
                        data->Slider->HandleSize = glm::vec2(handleX, handleY);
                    }
                }
                auto readVec4 = [&](glm::vec4& v) {
                    if (!hasBytes(sizeof(float) * 4)) return;
                    float x = 0.f, y = 0.f, z = 0.f, w = 0.f;
                    if (ctx.Read(x) && ctx.Read(y) && ctx.Read(z) && ctx.Read(w)) {
                        v = glm::vec4(x, y, z, w);
                    }
                };
                readVec4(data->Slider->HandleNormalTint);
                readVec4(data->Slider->HandleHoverTint);
                readVec4(data->Slider->HandlePressedTint);
                readVec4(data->Slider->HandleDisabledTint);
                if (hasBytes(sizeof(uint8_t))) {
                    uint8_t showFill = 0;
                    if (ctx.Read(showFill)) data->Slider->ShowFill = showFill != 0;
                }
                readVec4(data->Slider->FillColor);
                auto readAssetRef = [&](AssetReference& ref) {
                    if (!hasBytes(sizeof(uint64_t) + sizeof(uint64_t) + sizeof(int64_t) + sizeof(uint32_t))) return;
                    ctx.Read(ref.guid.high);
                    ctx.Read(ref.guid.low);
                    ctx.Read(ref.fileID);
                    ctx.Read(ref.type);
                };
                readAssetRef(data->Slider->HandleTexture);
                readAssetRef(data->Slider->FillTexture);
                if (hasBytes(sizeof(float))) {
                    float op = 1.0f;
                    if (ctx.Read(op)) data->Slider->Opacity = op;
                }
                if (hasBytes(sizeof(uint8_t))) {
                    uint8_t vis = 1;
                    if (ctx.Read(vis)) data->Slider->Visible = vis != 0;
                }
            }
            break;
        }
        
        case ComponentTypeId::ProgressBar: {
            float minVal, maxVal, value;
            float fillR, fillG, fillB, fillA;
            uint32_t direction;
            uint8_t useGradient;
            
            success = ctx.Read(minVal) && ctx.Read(maxVal) && ctx.Read(value);
            success = success && ctx.Read(fillR) && ctx.Read(fillG) && ctx.Read(fillB) && ctx.Read(fillA);
            success = success && ctx.Read(direction) && ctx.Read(useGradient);
            
            if (success) {
                data->ProgressBar = std::make_unique<ProgressBarComponent>();
                data->ProgressBar->MinValue = minVal;
                data->ProgressBar->MaxValue = maxVal;
                data->ProgressBar->Value = value;
                data->ProgressBar->FillColor = glm::vec4(fillR, fillG, fillB, fillA);
                data->ProgressBar->Direction = static_cast<ProgressBarComponent::FillDirection>(direction);
                data->ProgressBar->UseGradient = useGradient != 0;

                // v17+: optional full progress bar payload
                auto hasBytes = [&](size_t bytes) { return (ctx.pos + bytes) <= ctx.size; };
                auto readVec4 = [&](glm::vec4& v) {
                    if (!hasBytes(sizeof(float) * 4)) return;
                    float x = 0.f, y = 0.f, z = 0.f, w = 0.f;
                    if (ctx.Read(x) && ctx.Read(y) && ctx.Read(z) && ctx.Read(w)) {
                        v = glm::vec4(x, y, z, w);
                    }
                };
                readVec4(data->ProgressBar->GradientLowColor);
                readVec4(data->ProgressBar->GradientHighColor);
                readVec4(data->ProgressBar->Padding);
                if (hasBytes(sizeof(uint8_t))) {
                    uint8_t useBorder = 0;
                    if (ctx.Read(useBorder)) data->ProgressBar->UsePanelBorderAsPadding = useBorder != 0;
                }
                if (hasBytes(sizeof(uint8_t))) {
                    uint8_t animate = 0;
                    if (ctx.Read(animate)) data->ProgressBar->Animate = animate != 0;
                }
                if (hasBytes(sizeof(float))) {
                    float speed = data->ProgressBar->AnimationSpeed;
                    if (ctx.Read(speed)) data->ProgressBar->AnimationSpeed = speed;
                }
                if (hasBytes(sizeof(uint64_t) + sizeof(uint64_t) + sizeof(int64_t) + sizeof(uint32_t))) {
                    ctx.Read(data->ProgressBar->FillTexture.guid.high);
                    ctx.Read(data->ProgressBar->FillTexture.guid.low);
                    ctx.Read(data->ProgressBar->FillTexture.fileID);
                    ctx.Read(data->ProgressBar->FillTexture.type);
                }
                if (hasBytes(sizeof(float))) {
                    float op = 1.0f;
                    if (ctx.Read(op)) data->ProgressBar->Opacity = op;
                }
                if (hasBytes(sizeof(uint8_t))) {
                    uint8_t vis = 1;
                    if (ctx.Read(vis)) data->ProgressBar->Visible = vis != 0;
                }
            }
            break;
        }
        
        case ComponentTypeId::Toggle: {
            uint8_t isOn, interactable;
            if (ctx.Read(isOn) && ctx.Read(interactable)) {
                data->Toggle = std::make_unique<ToggleComponent>();
                data->Toggle->IsOn = isOn != 0;
                data->Toggle->Interactable = interactable != 0;

                // v17+: optional full toggle payload
                auto hasBytes = [&](size_t bytes) { return (ctx.pos + bytes) <= ctx.size; };
                if (hasBytes(sizeof(float) * 4)) {
                    float sx = 0.f, sy = 0.f, ox = 0.f, oy = 0.f;
                    if (ctx.Read(sx) && ctx.Read(sy) && ctx.Read(ox) && ctx.Read(oy)) {
                        data->Toggle->CheckmarkSize = glm::vec2(sx, sy);
                        data->Toggle->CheckmarkOffset = glm::vec2(ox, oy);
                    }
                }
                auto readVec4 = [&](glm::vec4& v) {
                    if (!hasBytes(sizeof(float) * 4)) return;
                    float x = 0.f, y = 0.f, z = 0.f, w = 0.f;
                    if (ctx.Read(x) && ctx.Read(y) && ctx.Read(z) && ctx.Read(w)) {
                        v = glm::vec4(x, y, z, w);
                    }
                };
                readVec4(data->Toggle->CheckmarkTint);
                readVec4(data->Toggle->OffTint);
                readVec4(data->Toggle->OnTint);
                readVec4(data->Toggle->HoverTint);
                readVec4(data->Toggle->DisabledTint);
                if (hasBytes(sizeof(int32_t))) {
                    int32_t gid = 0;
                    if (ctx.Read(gid)) data->Toggle->GroupID = gid;
                }
                if (hasBytes(sizeof(uint64_t) + sizeof(uint64_t) + sizeof(int64_t) + sizeof(uint32_t))) {
                    ctx.Read(data->Toggle->CheckmarkTexture.guid.high);
                    ctx.Read(data->Toggle->CheckmarkTexture.guid.low);
                    ctx.Read(data->Toggle->CheckmarkTexture.fileID);
                    ctx.Read(data->Toggle->CheckmarkTexture.type);
                }
                if (hasBytes(sizeof(float))) {
                    float op = 1.0f;
                    if (ctx.Read(op)) data->Toggle->Opacity = op;
                }
                if (hasBytes(sizeof(uint8_t))) {
                    uint8_t vis = 1;
                    if (ctx.Read(vis)) data->Toggle->Visible = vis != 0;
                }
            }
            break;
        }
        
        case ComponentTypeId::ScrollView: {
            uint8_t hScroll, vScroll;
            float sensitivity;
            float contentX, contentY;
            uint8_t showScrollbars;
            
            success = ctx.Read(hScroll) && ctx.Read(vScroll) && ctx.Read(sensitivity);
            success = success && ctx.Read(contentX) && ctx.Read(contentY) && ctx.Read(showScrollbars);
            
            if (success) {
                data->ScrollView = std::make_unique<ScrollViewComponent>();
                data->ScrollView->HorizontalScroll = hScroll != 0;
                data->ScrollView->VerticalScroll = vScroll != 0;
                data->ScrollView->ScrollSensitivity = sensitivity;
                data->ScrollView->ContentSize = glm::vec2(contentX, contentY);
                data->ScrollView->ShowScrollbars = showScrollbars != 0;

                // v17+: optional full scroll view payload
                auto hasBytes = [&](size_t bytes) { return (ctx.pos + bytes) <= ctx.size; };
                if (hasBytes(sizeof(float))) {
                    float w = data->ScrollView->ScrollbarWidth;
                    if (ctx.Read(w)) data->ScrollView->ScrollbarWidth = w;
                }
                auto readVec4 = [&](glm::vec4& v) {
                    if (!hasBytes(sizeof(float) * 4)) return;
                    float x = 0.f, y = 0.f, z = 0.f, w = 0.f;
                    if (ctx.Read(x) && ctx.Read(y) && ctx.Read(z) && ctx.Read(w)) {
                        v = glm::vec4(x, y, z, w);
                    }
                };
                readVec4(data->ScrollView->ScrollbarTrackColor);
                readVec4(data->ScrollView->ScrollbarThumbColor);
                readVec4(data->ScrollView->ScrollbarThumbHoverColor);
                if (hasBytes(sizeof(uint8_t))) {
                    uint8_t useInertia = 1;
                    if (ctx.Read(useInertia)) data->ScrollView->UseInertia = useInertia != 0;
                }
                if (hasBytes(sizeof(float))) {
                    float decel = data->ScrollView->InertiaDeceleration;
                    if (ctx.Read(decel)) data->ScrollView->InertiaDeceleration = decel;
                }
                if (hasBytes(sizeof(uint8_t))) {
                    uint8_t elastic = 1;
                    if (ctx.Read(elastic)) data->ScrollView->Elastic = elastic != 0;
                }
                if (hasBytes(sizeof(float))) {
                    float amt = data->ScrollView->ElasticAmount;
                    if (ctx.Read(amt)) data->ScrollView->ElasticAmount = amt;
                }
                auto readAssetRef = [&](AssetReference& ref) {
                    if (!hasBytes(sizeof(uint64_t) + sizeof(uint64_t) + sizeof(int64_t) + sizeof(uint32_t))) return;
                    ctx.Read(ref.guid.high);
                    ctx.Read(ref.guid.low);
                    ctx.Read(ref.fileID);
                    ctx.Read(ref.type);
                };
                readAssetRef(data->ScrollView->ScrollbarTrackTexture);
                readAssetRef(data->ScrollView->ScrollbarThumbTexture);
                if (hasBytes(sizeof(float))) {
                    float op = 1.0f;
                    if (ctx.Read(op)) data->ScrollView->Opacity = op;
                }
                if (hasBytes(sizeof(uint8_t))) {
                    uint8_t vis = 1;
                    if (ctx.Read(vis)) data->ScrollView->Visible = vis != 0;
                }
            }
            break;
        }
        
        case ComponentTypeId::LayoutGroup: {
            uint32_t direction;
            float spacing;
            float padX, padY, padZ, padW;
            uint8_t forceExpandW, forceExpandH;
            
            success = ctx.Read(direction) && ctx.Read(spacing);
            success = success && ctx.Read(padX) && ctx.Read(padY) && ctx.Read(padZ) && ctx.Read(padW);
            success = success && ctx.Read(forceExpandW) && ctx.Read(forceExpandH);
            
            if (success) {
                data->LayoutGroup = std::make_unique<LayoutGroupComponent>();
                data->LayoutGroup->Direction = static_cast<LayoutGroupComponent::LayoutDirection>(direction);
                data->LayoutGroup->Spacing = spacing;
                data->LayoutGroup->Padding = glm::vec4(padX, padY, padZ, padW);
                data->LayoutGroup->ChildForceExpandWidth = forceExpandW != 0;
                data->LayoutGroup->ChildForceExpandHeight = forceExpandH != 0;
                // Optional fields appended in newer prefab versions
                auto hasBytes = [&](size_t bytes) { return (ctx.pos + bytes) <= ctx.size; };
                if (hasBytes(sizeof(uint32_t))) {
                    uint32_t childAlign = 0;
                    if (ctx.Read(childAlign)) {
                        data->LayoutGroup->ChildAlignment = static_cast<LayoutGroupComponent::Alignment>(childAlign);
                    }
                }
                if (hasBytes(sizeof(uint32_t))) {
                    uint32_t crossAlign = 0;
                    if (ctx.Read(crossAlign)) {
                        data->LayoutGroup->CrossAlignment = static_cast<LayoutGroupComponent::Alignment>(crossAlign);
                    }
                }
                if (hasBytes(sizeof(uint8_t))) {
                    uint8_t controlW = 0;
                    if (ctx.Read(controlW)) data->LayoutGroup->ControlChildWidth = controlW != 0;
                }
                if (hasBytes(sizeof(uint8_t))) {
                    uint8_t controlH = 0;
                    if (ctx.Read(controlH)) data->LayoutGroup->ControlChildHeight = controlH != 0;
                }
                if (hasBytes(sizeof(uint8_t))) {
                    uint8_t reverseOrder = 0;
                    if (ctx.Read(reverseOrder)) data->LayoutGroup->ReverseOrder = reverseOrder != 0;
                }
                if (hasBytes(sizeof(int32_t))) {
                    int32_t columns = 0;
                    if (ctx.Read(columns)) data->LayoutGroup->Columns = columns;
                }
                if (hasBytes(sizeof(int32_t))) {
                    int32_t rows = 0;
                    if (ctx.Read(rows)) data->LayoutGroup->Rows = rows;
                }
                if (hasBytes(sizeof(float))) {
                    float cellW = 0.0f;
                    if (ctx.Read(cellW)) data->LayoutGroup->CellSize.x = cellW;
                }
                if (hasBytes(sizeof(float))) {
                    float cellH = 0.0f;
                    if (ctx.Read(cellH)) data->LayoutGroup->CellSize.y = cellH;
                }
            }
            break;
        }
        
        case ComponentTypeId::InputField: {
            uint32_t textIdx, placeholderIdx;
            int32_t maxLength;
            uint32_t contentType;
            uint8_t interactable, multiline;
            
            success = ctx.Read(textIdx) && ctx.Read(placeholderIdx) && ctx.Read(maxLength);
            success = success && ctx.Read(contentType) && ctx.Read(interactable) && ctx.Read(multiline);
            
            if (success) {
                data->InputField = std::make_unique<InputFieldComponent>();
                data->InputField->Text = ctx.ReadString(textIdx);
                data->InputField->PlaceholderText = ctx.ReadString(placeholderIdx);
                data->InputField->MaxLength = maxLength;
                data->InputField->Type = static_cast<InputFieldComponent::ContentType>(contentType);
                data->InputField->Interactable = interactable != 0;
                data->InputField->Multiline = multiline != 0;

                // v17+: optional full input field payload
                auto hasBytes = [&](size_t bytes) { return (ctx.pos + bytes) <= ctx.size; };
                if (hasBytes(sizeof(uint8_t))) {
                    uint8_t ro = 0;
                    if (ctx.Read(ro)) data->InputField->ReadOnly = ro != 0;
                }
                if (hasBytes(sizeof(uint8_t))) {
                    uint8_t pw = static_cast<uint8_t>(data->InputField->PasswordChar);
                    if (ctx.Read(pw)) data->InputField->PasswordChar = static_cast<char>(pw);
                }
                auto readVec4 = [&](glm::vec4& v) {
                    if (!hasBytes(sizeof(float) * 4)) return;
                    float x = 0.f, y = 0.f, z = 0.f, w = 0.f;
                    if (ctx.Read(x) && ctx.Read(y) && ctx.Read(z) && ctx.Read(w)) {
                        v = glm::vec4(x, y, z, w);
                    }
                };
                readVec4(data->InputField->TextColor);
                readVec4(data->InputField->PlaceholderColor);
                readVec4(data->InputField->SelectionColor);
                readVec4(data->InputField->CursorColor);
                if (hasBytes(sizeof(float))) {
                    float cursorWidth = data->InputField->CursorWidth;
                    if (ctx.Read(cursorWidth)) data->InputField->CursorWidth = cursorWidth;
                }
                if (hasBytes(sizeof(float))) {
                    float op = 1.0f;
                    if (ctx.Read(op)) data->InputField->Opacity = op;
                }
                if (hasBytes(sizeof(uint8_t))) {
                    uint8_t vis = 1;
                    if (ctx.Read(vis)) data->InputField->Visible = vis != 0;
                }
            }
            break;
        }
        
        case ComponentTypeId::Dropdown: {
            int32_t selectedIdx;
            uint32_t optionCount;
            uint8_t interactable;
            
            if (ctx.Read(selectedIdx) && ctx.Read(optionCount)) {
                data->Dropdown = std::make_unique<DropdownComponent>();
                data->Dropdown->SelectedIndex = selectedIdx;
                data->Dropdown->Options.resize(optionCount);
                for (uint32_t i = 0; i < optionCount; ++i) {
                    uint32_t optIdx;
                    if (ctx.Read(optIdx)) {
                        data->Dropdown->Options[i] = ctx.ReadString(optIdx);
                    }
                }
                if (ctx.Read(interactable)) {
                    data->Dropdown->Interactable = interactable != 0;
                }

                // v17+: optional full dropdown payload
                auto hasBytes = [&](size_t bytes) { return (ctx.pos + bytes) <= ctx.size; };
                if (hasBytes(sizeof(float))) {
                    float h = data->Dropdown->OptionHeight;
                    if (ctx.Read(h)) data->Dropdown->OptionHeight = h;
                }
                if (hasBytes(sizeof(int32_t))) {
                    int32_t maxVisible = data->Dropdown->MaxVisibleOptions;
                    if (ctx.Read(maxVisible)) data->Dropdown->MaxVisibleOptions = maxVisible;
                }
                auto readVec4 = [&](glm::vec4& v) {
                    if (!hasBytes(sizeof(float) * 4)) return;
                    float x = 0.f, y = 0.f, z = 0.f, w = 0.f;
                    if (ctx.Read(x) && ctx.Read(y) && ctx.Read(z) && ctx.Read(w)) {
                        v = glm::vec4(x, y, z, w);
                    }
                };
                readVec4(data->Dropdown->OptionNormalColor);
                readVec4(data->Dropdown->OptionHoverColor);
                readVec4(data->Dropdown->OptionSelectedColor);
                if (hasBytes(sizeof(uint8_t))) {
                    uint8_t showArrow = 1;
                    if (ctx.Read(showArrow)) data->Dropdown->ShowArrow = showArrow != 0;
                }
                if (hasBytes(sizeof(float) * 2)) {
                    float ax = data->Dropdown->ArrowSize.x;
                    float ay = data->Dropdown->ArrowSize.y;
                    if (ctx.Read(ax) && ctx.Read(ay)) data->Dropdown->ArrowSize = glm::vec2(ax, ay);
                }
                readVec4(data->Dropdown->ArrowTint);
                if (hasBytes(sizeof(uint32_t))) {
                    uint32_t captionIdx = 0;
                    if (ctx.Read(captionIdx)) data->Dropdown->Caption = ctx.ReadString(captionIdx);
                }
                if (hasBytes(sizeof(uint64_t) + sizeof(uint64_t) + sizeof(int64_t) + sizeof(uint32_t))) {
                    ctx.Read(data->Dropdown->ArrowTexture.guid.high);
                    ctx.Read(data->Dropdown->ArrowTexture.guid.low);
                    ctx.Read(data->Dropdown->ArrowTexture.fileID);
                    ctx.Read(data->Dropdown->ArrowTexture.type);
                }
                if (hasBytes(sizeof(float))) {
                    float op = 1.0f;
                    if (ctx.Read(op)) data->Dropdown->Opacity = op;
                }
                if (hasBytes(sizeof(uint8_t))) {
                    uint8_t vis = 1;
                    if (ctx.Read(vis)) data->Dropdown->Visible = vis != 0;
                }
            }
            break;
        }
        
        case ComponentTypeId::Text: {
            uint32_t textIdx, fontPathIdx;
            float pixelSize;
            uint32_t colorAbgr;
            uint8_t worldSpace, visible;
            int32_t zOrder;
            float opacity;
            float offsetX = 0.0f, offsetY = 0.0f;
            uint8_t anchorToParentUI = 0;
            
            success = ctx.Read(textIdx) && ctx.Read(fontPathIdx) && ctx.Read(pixelSize);
            success = success && ctx.Read(colorAbgr) && ctx.Read(worldSpace) && ctx.Read(visible);
            success = success && ctx.Read(zOrder) && ctx.Read(opacity);
            
            if (success) {
                data->Text = std::make_unique<TextRendererComponent>();
                data->Text->Text = ctx.ReadString(textIdx);
                data->Text->FontPath = ctx.ReadString(fontPathIdx);
                data->Text->PixelSize = pixelSize;
                data->Text->ColorAbgr = colorAbgr;
                data->Text->WorldSpace = worldSpace != 0;
                data->Text->Visible = visible != 0;
                data->Text->ZOrder = zOrder;
                data->Text->Opacity = opacity;

                // v5+: optional anchor-to-parent offset (2 floats) and flag (1 byte) at tail of payload.
                // Older binaries won't contain these; only read if enough bytes remain.
                size_t remaining = (ctx.size > ctx.pos) ? (ctx.size - ctx.pos) : 0;
                if (remaining >= sizeof(float) * 2 + sizeof(uint8_t)) {
                    if (ctx.Read(offsetX) && ctx.Read(offsetY) && ctx.Read(anchorToParentUI)) {
                        data->Text->AnchorOffset.x = offsetX;
                        data->Text->AnchorOffset.y = offsetY;
                        data->Text->AnchorToParentUI = (anchorToParentUI != 0);
                    }
                }

                // v17+: optional anchor preset + rect size + word wrap
                remaining = (ctx.size > ctx.pos) ? (ctx.size - ctx.pos) : 0;
                if (remaining >= sizeof(uint8_t) + sizeof(uint32_t) + sizeof(float) * 2 + sizeof(uint8_t)) {
                    uint8_t anchorEnabled = 0;
                    uint32_t anchorPreset = 0;
                    float rectW = 0.0f, rectH = 0.0f;
                    uint8_t wordWrap = 0;
                    if (ctx.Read(anchorEnabled) && ctx.Read(anchorPreset) &&
                        ctx.Read(rectW) && ctx.Read(rectH) && ctx.Read(wordWrap)) {
                        data->Text->AnchorEnabled = anchorEnabled != 0;
                        data->Text->Anchor = static_cast<UIAnchorPreset>(anchorPreset);
                        data->Text->RectSize = glm::vec2(rectW, rectH);
                        data->Text->WordWrap = wordWrap != 0;
                    }
                }

                // Optional visual effect parity fields.
                remaining = (ctx.size > ctx.pos) ? (ctx.size - ctx.pos) : 0;
                if (remaining >= sizeof(uint8_t) + sizeof(uint32_t) + sizeof(float) +
                                 sizeof(uint8_t) + sizeof(uint32_t) + sizeof(float) * 2) {
                    uint8_t outlineEnabled = 0;
                    uint32_t outlineColorAbgr = data->Text->OutlineColorAbgr;
                    float outlineThickness = data->Text->OutlineThickness;
                    uint8_t shadowEnabled = 0;
                    uint32_t shadowColorAbgr = data->Text->ShadowColorAbgr;
                    float shadowOffsetX = data->Text->ShadowOffset.x;
                    float shadowOffsetY = data->Text->ShadowOffset.y;
                    if (ctx.Read(outlineEnabled) &&
                        ctx.Read(outlineColorAbgr) &&
                        ctx.Read(outlineThickness) &&
                        ctx.Read(shadowEnabled) &&
                        ctx.Read(shadowColorAbgr) &&
                        ctx.Read(shadowOffsetX) &&
                        ctx.Read(shadowOffsetY)) {
                        data->Text->OutlineEnabled = outlineEnabled != 0;
                        data->Text->OutlineColorAbgr = outlineColorAbgr;
                        data->Text->OutlineThickness = outlineThickness;
                        data->Text->ShadowEnabled = shadowEnabled != 0;
                        data->Text->ShadowColorAbgr = shadowColorAbgr;
                        data->Text->ShadowOffset = glm::vec2(shadowOffsetX, shadowOffsetY);
                    }
                }

                // v26+: optional horizontal text alignment.
                remaining = (ctx.size > ctx.pos) ? (ctx.size - ctx.pos) : 0;
                if (remaining >= sizeof(uint32_t)) {
                    uint32_t textAlignment = 0;
                    if (ctx.Read(textAlignment)) {
                        data->Text->TextAlignment = static_cast<TextRendererComponent::Alignment>(textAlignment);
                    }
                }

                // v27+: optional standalone world text billboarding.
                remaining = (ctx.size > ctx.pos) ? (ctx.size - ctx.pos) : 0;
                if (remaining >= sizeof(uint8_t)) {
                    uint8_t billboard = 1;
                    if (ctx.Read(billboard)) {
                        data->Text->Billboard = billboard != 0;
                    }
                }
            }
            break;
        }
        
        case ComponentTypeId::NavMesh: {
            uint8_t enabled;
            float agentRadius, agentHeight, maxSlope, stepHeight, cellSize, cellHeight;
            uint64_t dataGuidHigh, dataGuidLow;
            uint32_t assetPathIdx;
            uint64_t legacyGuidHigh, legacyGuidLow;
            success = ctx.Read(enabled);
            success = success && ctx.Read(agentRadius) && ctx.Read(agentHeight) && ctx.Read(maxSlope);
            success = success && ctx.Read(stepHeight) && ctx.Read(cellSize) && ctx.Read(cellHeight);
            success = success && ctx.Read(dataGuidHigh) && ctx.Read(dataGuidLow);
            success = success && ctx.Read(assetPathIdx);
            success = success && ctx.Read(legacyGuidHigh) && ctx.Read(legacyGuidLow);
            if (success) {
                data->Navigation = std::make_unique<nav::NavMeshComponent>();
                data->Navigation->Enabled = enabled != 0;
                data->Navigation->Bake.agentRadius = agentRadius;
                data->Navigation->Bake.agentHeight = agentHeight;
                data->Navigation->Bake.agentMaxSlopeDeg = maxSlope;
                data->Navigation->Bake.agentMaxClimb = stepHeight;
                data->Navigation->Bake.cellSize = cellSize;
                data->Navigation->Bake.cellHeight = cellHeight;
                data->Navigation->NavMeshDataGuid = ClaymoreGUID(dataGuidHigh, dataGuidLow);
                data->Navigation->AssetPath = ctx.ReadString(assetPathIdx);
                (void)legacyGuidHigh;
                (void)legacyGuidLow;
                if (ctx.version >= 11) {
                    uint8_t includeTerrain, terrainOnly, keepLargestIsland;
                    uint8_t costAwareSmoothing, enableStitching;
                    uint8_t sourceMode, includeSiblings, sourceIncludeChildren;
                    uint32_t terrainSampleStep;
                    float terrainSlopeFilter, debugDrawOffset;
                    float stitchEpsilon, stitchMaxNormalAngle, stitchMaxHeight, stitchMaxXZ;
                    int32_t sourceLayer;
                    uint32_t sourceTagIdx;
                    uint32_t overrideCount = 0;
                    if (ctx.Read(includeTerrain) && ctx.Read(terrainOnly) &&
                        ctx.Read(terrainSampleStep) && ctx.Read(terrainSlopeFilter) &&
                        ctx.Read(keepLargestIsland) && ctx.Read(debugDrawOffset) &&
                        ctx.Read(costAwareSmoothing) && ctx.Read(enableStitching) &&
                        ctx.Read(stitchEpsilon) && ctx.Read(stitchMaxNormalAngle) &&
                        ctx.Read(stitchMaxHeight) && ctx.Read(stitchMaxXZ) &&
                        ctx.Read(sourceMode) && ctx.Read(includeSiblings) && ctx.Read(sourceIncludeChildren) &&
                        ctx.Read(sourceLayer) && ctx.Read(sourceTagIdx) &&
                        ctx.Read(overrideCount)) {
                        (void)includeTerrain;
                        (void)terrainOnly;
                        data->Navigation->TerrainSampleStep = terrainSampleStep;
                        (void)terrainSlopeFilter;
                        (void)keepLargestIsland;
                        data->Navigation->DebugDrawOffset = debugDrawOffset;
                        data->Navigation->CostAwareSmoothing = costAwareSmoothing != 0;
                        data->Navigation->EnableStitching = enableStitching != 0;
                        data->Navigation->StitchEpsilon = stitchEpsilon;
                        data->Navigation->StitchMaxNormalAngleDeg = stitchMaxNormalAngle;
                        data->Navigation->StitchMaxHeight = stitchMaxHeight;
                        data->Navigation->StitchMaxXZ = stitchMaxXZ;
                        (void)sourceMode;
                        (void)includeSiblings;
                        (void)sourceIncludeChildren;
                        (void)sourceLayer;
                        (void)ctx.ReadString(sourceTagIdx);
                        for (uint32_t i = 0; i < overrideCount; ++i) {
                            uint32_t entId;
                            float cost;
                            uint8_t includeChildren;
                            if (!ctx.Read(entId) || !ctx.Read(cost) || !ctx.Read(includeChildren)) break;
                            (void)entId;
                            (void)cost;
                            (void)includeChildren;
                        }
                        if (ctx.version >= 12) {
                            uint8_t regexEnabled = 0;
                            uint32_t regexIdx = 0;
                            uint32_t modelOverrideCount = 0;
                            if (ctx.Read(regexEnabled) && ctx.Read(regexIdx) && ctx.Read(modelOverrideCount)) {
                                data->Navigation->GeometryIncludeRegexEnabled = regexEnabled != 0;
                                data->Navigation->GeometryIncludeRegexPattern = ctx.ReadString(regexIdx);
                                for (uint32_t i = 0; i < modelOverrideCount; ++i) {
                                    uint32_t entId;
                                    uint8_t includeChildren;
                                    if (!ctx.Read(entId) || !ctx.Read(includeChildren)) break;
                                    (void)entId;
                                    (void)includeChildren;
                                }
                            }
                        }
                        if (ctx.version >= 13) {
                            int32_t domainId = 0;
                            int32_t domainPriority = 0;
                            uint8_t autoPortalEnabled = data->Navigation->AutoPortalEnabled ? 1 : 0;
                            float autoPortalMaxXZ = data->Navigation->AutoPortalMaxXZ;
                            float autoPortalMaxHeight = data->Navigation->AutoPortalMaxHeight;
                            if (ctx.Read(domainId) && ctx.Read(domainPriority) &&
                                ctx.Read(autoPortalEnabled) && ctx.Read(autoPortalMaxXZ) &&
                                ctx.Read(autoPortalMaxHeight)) {
                                data->Navigation->DomainId = domainId;
                                data->Navigation->DomainPriority = domainPriority;
                                data->Navigation->AutoPortalEnabled = autoPortalEnabled != 0;
                                data->Navigation->AutoPortalMaxXZ = autoPortalMaxXZ;
                                data->Navigation->AutoPortalMaxHeight = autoPortalMaxHeight;
                            }
                        }
                        if (ctx.version >= 15) {
                            uint8_t bakeVisible = 0;
                            uint32_t padding = 0;
                            if (ctx.Read(bakeVisible) && ctx.Read(padding)) {
                                data->Navigation->BakeVisibleChunksOnly = bakeVisible != 0;
                                data->Navigation->BakeVisibleChunkPadding = padding;
                            }
                        }
                        if (ctx.version >= 16) {
                            uint8_t chunked = 0;
                            uint8_t chunkMode = 0;
                            float chunkWorldSize = 0.0f;
                            uint32_t chunkPad = 0;
                            float streamRadius = 0.0f;
                            uint32_t packPathIdx = 0;
                            if (ctx.Read(chunked) && ctx.Read(chunkMode) && ctx.Read(chunkWorldSize) &&
                                ctx.Read(chunkPad) && ctx.Read(streamRadius) && ctx.Read(packPathIdx)) {
                                data->Navigation->ChunkedNavEnabled = chunked != 0;
                                data->Navigation->ChunkingMode = static_cast<nav::NavChunkingMode>(chunkMode);
                                data->Navigation->ChunkWorldSize = chunkWorldSize;
                                data->Navigation->ChunkBakePadding = chunkPad;
                                data->Navigation->ChunkStreamRadius = streamRadius;
                                data->Navigation->NavPackPath = ctx.ReadString(packPathIdx);
                            }
                        }
                        if (ctx.version >= 23) {
                            float agentPlacementOffset = data->Navigation->AgentPlacementOffset;
                            if (ctx.Read(agentPlacementOffset)) {
                                data->Navigation->AgentPlacementOffset = agentPlacementOffset;
                            }
                        }
                    }
                }
            }
            break;
        }
        
        case ComponentTypeId::NavAgent: {
            uint8_t enabled;
            uint32_t navMeshEntity;
            float radius, height, maxSpeed, maxAccel, arriveThreshold;
            uint8_t autoRepath;
            success = ctx.Read(enabled) && ctx.Read(navMeshEntity);
            success = success && ctx.Read(radius) && ctx.Read(height) && ctx.Read(maxSpeed);
            success = success && ctx.Read(maxAccel) && ctx.Read(arriveThreshold) && ctx.Read(autoRepath);
            if (success) {
                data->NavAgent = std::make_unique<nav::NavAgentComponent>();
                data->NavAgent->Enabled = enabled != 0;
                data->NavAgent->NavMeshEntity = static_cast<EntityID>(navMeshEntity);
                data->NavAgent->Params.radius = radius;
                data->NavAgent->Params.height = height;
                data->NavAgent->Params.maxSpeed = maxSpeed;
                data->NavAgent->Params.maxAccel = maxAccel;
                data->NavAgent->ArriveThreshold = arriveThreshold;
                data->NavAgent->AutoRepath = autoRepath != 0;
                
                // v5+: Additional params for full parity
                if (ctx.version >= 5) {
                    float repathInterval, avoidanceRadiusMul, steeringSmoothness, arrivalSlowdownDist;
                    float maxSlopeDeg, maxStep;
                    if (ctx.Read(repathInterval) && ctx.Read(avoidanceRadiusMul) &&
                        ctx.Read(steeringSmoothness) && ctx.Read(arrivalSlowdownDist) &&
                        ctx.Read(maxSlopeDeg) && ctx.Read(maxStep)) {
                        data->NavAgent->RepathInterval = repathInterval;
                        data->NavAgent->AvoidanceRadiusMul = avoidanceRadiusMul;
                        data->NavAgent->SteeringSmoothness = steeringSmoothness;
                        data->NavAgent->ArrivalSlowdownDist = arrivalSlowdownDist;
                        data->NavAgent->Params.maxSlopeDeg = maxSlopeDeg;
                        data->NavAgent->Params.maxStep = maxStep;
                    }
                }
                if (ctx.version >= 13) {
                    int32_t preferredDomainId = 0;
                    if (ctx.Read(preferredDomainId)) {
                        data->NavAgent->Params.preferredDomainId = preferredDomainId;
                    }
                }
            }
            break;
        }

        case ComponentTypeId::NavLink: {
            uint8_t enabled = 1;
            float startX, startY, startZ;
            float endX, endY, endZ;
            float radius = 0.5f;
            float cost = 1.0f;
            uint32_t flags = 0;
            uint8_t bidirectional = 1;
            uint8_t useWorldSpace = 0;
            success = ctx.Read(enabled) &&
                      ctx.Read(startX) && ctx.Read(startY) && ctx.Read(startZ) &&
                      ctx.Read(endX) && ctx.Read(endY) && ctx.Read(endZ) &&
                      ctx.Read(radius) && ctx.Read(cost) &&
                      ctx.Read(flags) && ctx.Read(bidirectional) && ctx.Read(useWorldSpace);
            if (success) {
                data->NavLink = std::make_unique<nav::NavLinkComponent>();
                data->NavLink->Enabled = enabled != 0;
                data->NavLink->Start = glm::vec3(startX, startY, startZ);
                data->NavLink->End = glm::vec3(endX, endY, endZ);
                data->NavLink->Radius = radius;
                data->NavLink->Cost = cost;
                data->NavLink->Flags = flags;
                data->NavLink->Bidirectional = bidirectional != 0;
                data->NavLink->UseWorldSpace = useWorldSpace != 0;
            }
            break;
        }

        case ComponentTypeId::Portal: {
            uint8_t enabled;
            uint32_t targetSceneIdx;
            uint64_t targetGuidHigh, targetGuidLow;
            uint32_t targetPortalPathIdx;
            float entryX, entryY, entryZ;
            float exitX, exitY, exitZ;
            uint8_t autoDetect;
            float triggerRadius;
            uint8_t fireExit;
            success = ctx.Read(enabled);
            success = success && ctx.Read(targetSceneIdx);
            success = success && ctx.Read(targetGuidHigh) && ctx.Read(targetGuidLow);
            success = success && ctx.Read(targetPortalPathIdx);
            success = success && ctx.Read(entryX) && ctx.Read(entryY) && ctx.Read(entryZ);
            success = success && ctx.Read(exitX) && ctx.Read(exitY) && ctx.Read(exitZ);
            success = success && ctx.Read(autoDetect);
            success = success && ctx.Read(triggerRadius);
            success = success && ctx.Read(fireExit);
            if (success) {
                data->Portal = std::make_unique<PortalComponent>();
                data->Portal->Enabled = enabled != 0;
                data->Portal->TargetScenePath = ctx.ReadString(targetSceneIdx);
                data->Portal->TargetPortalGuid = ClaymoreGUID(targetGuidHigh, targetGuidLow);
                data->Portal->TargetPortalPath = ctx.ReadString(targetPortalPathIdx);
                data->Portal->EntryOffset = glm::vec3(entryX, entryY, entryZ);
                data->Portal->ExitOffset = glm::vec3(exitX, exitY, exitZ);
                data->Portal->AutoDetect = autoDetect != 0;
                data->Portal->TriggerRadius = triggerRadius;
                data->Portal->FireExitEvents = fireExit != 0;
                data->Portal->ResetRuntime();
            }
            break;
        }
        
        case ComponentTypeId::IKConstraint: {
            uint32_t ikCount = 0;
            if (ctx.Read(ikCount)) {
                data->IKs.resize(ikCount);
                for (uint32_t i = 0; i < ikCount; ++i) {
                    uint8_t enabled, useTwoBone;
                    float weight, maxIter, tolerance;
                    uint32_t targetEnt, poleEnt;
                    uint32_t chainLen;
                    
                    success = ctx.Read(enabled) && ctx.Read(weight);
                    success = success && ctx.Read(targetEnt) && ctx.Read(poleEnt);
                    success = success && ctx.Read(useTwoBone) && ctx.Read(maxIter) && ctx.Read(tolerance);
                    success = success && ctx.Read(chainLen);
                    
                    if (success) {
                        data->IKs[i].Enabled = enabled != 0;
                        data->IKs[i].Weight = weight;
                        data->IKs[i].TargetEntity = static_cast<EntityID>(targetEnt);
                        data->IKs[i].PoleEntity = static_cast<EntityID>(poleEnt);
                        data->IKs[i].UseTwoBone = useTwoBone != 0;
                        data->IKs[i].MaxIterations = maxIter;
                        data->IKs[i].Tolerance = tolerance;
                        data->IKs[i].Chain.resize(chainLen);
                        for (uint32_t j = 0; j < chainLen; ++j) {
                            int32_t boneId;
                            if (ctx.Read(boneId)) {
                                data->IKs[i].Chain[j] = boneId;
                            }
                        }
                    }
                }
            }
            break;
        }
        
        case ComponentTypeId::LookAtConstraint: {
            uint32_t lacCount = 0;
            if (ctx.Read(lacCount)) {
                data->LookAtConstraints.resize(lacCount);
                for (uint32_t i = 0; i < lacCount; ++i) {
                    uint8_t enabled;
                    float weight, smoothingSpeed;
                    float maxYaw, minYaw, maxPitch, minPitch;
                    uint32_t mode, axes;
                    uint64_t targetGuidHigh, targetGuidLow;
                    uint32_t chainLen;
                    
                    success = ctx.Read(enabled) && ctx.Read(weight) && ctx.Read(mode);
                    success = success && ctx.Read(targetGuidHigh) && ctx.Read(targetGuidLow);
                    success = success && ctx.Read(smoothingSpeed) && ctx.Read(axes);
                    success = success && ctx.Read(maxYaw) && ctx.Read(minYaw);
                    success = success && ctx.Read(maxPitch) && ctx.Read(minPitch);
                    success = success && ctx.Read(chainLen);
                    
                    if (success) {
                        data->LookAtConstraints[i].Enabled = enabled != 0;
                        data->LookAtConstraints[i].Weight = weight;
                        data->LookAtConstraints[i].Mode = static_cast<cm::animation::lookat::LookAtMode>(mode);
                        data->LookAtConstraints[i].TargetEntityGuidHigh = targetGuidHigh;
                        data->LookAtConstraints[i].TargetEntityGuidLow = targetGuidLow;
                        data->LookAtConstraints[i].SmoothingSpeed = smoothingSpeed;
                        data->LookAtConstraints[i].Axes = static_cast<cm::animation::lookat::AxisMask>(axes);
                        // MaxYaw/MaxPitch are stored as symmetric limits (stored max value is used for both +/-)
                        data->LookAtConstraints[i].MaxYawDeg = maxYaw;
                        data->LookAtConstraints[i].MaxPitchDeg = maxPitch;
                        // MinYaw/MinPitch from binary are ignored - struct uses symmetric limits
                        (void)minYaw;
                        (void)minPitch;
                        data->LookAtConstraints[i].BoneChain.resize(chainLen);
                        for (uint32_t j = 0; j < chainLen; ++j) {
                            int32_t boneId;
                            if (ctx.Read(boneId)) {
                                data->LookAtConstraints[i].BoneChain[j] = boneId;
                            }
                        }
                    }
                }
            }
            break;
        }
        
        case ComponentTypeId::UIRect: {
            data->UIRect = std::make_unique<UIRectComponent>();
            auto& rect = *data->UIRect;
            uint8_t anchorToParent;
            success = ctx.Read(anchorToParent);
            if (success) rect.AnchorToParent = anchorToParent != 0;
            success = success && ctx.Read(rect.HorizontalAnchor);
            success = success && ctx.Read(rect.VerticalAnchor);
            success = success && ctx.Read(rect.Pivot.x);
            success = success && ctx.Read(rect.Pivot.y);
            success = success && ctx.Read(rect.Offset.x);
            success = success && ctx.Read(rect.Offset.y);
            success = success && ctx.Read(rect.Size.x);
            success = success && ctx.Read(rect.Size.y);
            break;
        }
        
        case ComponentTypeId::FitToContent: {
            data->FitToContent = std::make_unique<FitToContentComponent>();
            auto& ftc = *data->FitToContent;
            uint8_t enabled, fitWidth, fitHeight, directChildrenOnly;
            success = ctx.Read(enabled);
            if (success) ftc.Enabled = enabled != 0;
            success = success && ctx.Read(fitWidth);
            if (success) ftc.FitWidth = fitWidth != 0;
            success = success && ctx.Read(fitHeight);
            if (success) ftc.FitHeight = fitHeight != 0;
            success = success && ctx.Read(ftc.Padding.x);
            success = success && ctx.Read(ftc.Padding.y);
            success = success && ctx.Read(ftc.Padding.z);
            success = success && ctx.Read(ftc.Padding.w);
            success = success && ctx.Read(ftc.MinSize.x);
            success = success && ctx.Read(ftc.MinSize.y);
            success = success && ctx.Read(ftc.MaxSize.x);
            success = success && ctx.Read(ftc.MaxSize.y);
            success = success && ctx.Read(directChildrenOnly);
            if (success) ftc.DirectChildrenOnly = directChildrenOnly != 0;
            break;
        }

        case ComponentTypeId::UISceneCapture: {
            data->UISceneCapture = std::make_unique<UISceneCaptureComponent>();
            auto& cap = *data->UISceneCapture;
            uint8_t enabled = 0, autoFrame = 0, includeChildren = 0, showGrid = 0;
            auto hasBytes = [&](size_t bytes) { return (ctx.pos + bytes) <= ctx.size; };
            int32_t targetEntity = -1;
            int32_t renderW = 0, renderH = 0;
            success = ctx.Read(enabled) && ctx.Read(autoFrame) && ctx.Read(includeChildren);
            if (success) {
                cap.Enabled = enabled != 0;
                cap.AutoFrame = autoFrame != 0;
                cap.IncludeChildren = includeChildren != 0;
            }
            success = success && ctx.Read(cap.BoundsPadding);
            success = success && ctx.Read(cap.FieldOfView);
            success = success && ctx.Read(cap.NearClip);
            success = success && ctx.Read(cap.FarClip);
            success = success && ctx.Read(cap.ViewDirection.x);
            success = success && ctx.Read(cap.ViewDirection.y);
            success = success && ctx.Read(cap.ViewDirection.z);
            success = success && ctx.Read(cap.UpDirection.x);
            success = success && ctx.Read(cap.UpDirection.y);
            success = success && ctx.Read(cap.UpDirection.z);
            success = success && ctx.Read(cap.FocusOffset.x);
            success = success && ctx.Read(cap.FocusOffset.y);
            success = success && ctx.Read(cap.FocusOffset.z);
            success = success && ctx.Read(targetEntity);
            if (success) cap.TargetEntity = targetEntity;
            success = success && ctx.Read(cap.TargetGuidHigh);
            success = success && ctx.Read(cap.TargetGuidLow);
            success = success && ctx.Read(renderW);
            success = success && ctx.Read(renderH);
            if (success) {
                cap.RenderWidth = renderW;
                cap.RenderHeight = renderH;
            }
            success = success && ctx.Read(cap.ClearColor);
            success = success && ctx.Read(showGrid);
            if (success) cap.ShowGrid = showGrid != 0;
            if (success && hasBytes(sizeof(uint8_t))) {
                uint8_t lockView = 0;
                if (ctx.Read(lockView)) {
                    cap.LockViewToTarget = lockView != 0;
                }
            }
            break;
        }
        
        case ComponentTypeId::ResourceLayers: {
            data->ResourceLayers = std::make_unique<cm::resourcelayer::ResourceLayerComponent>();
            auto& layers = *data->ResourceLayers;
            
            // Global settings
            success = ctx.Read(layers.GlobalSeed);
            success = success && ctx.Read(layers.GlobalDensityMultiplier);
            success = success && ctx.Read(layers.GlobalSwapDistance);
            success = success && ctx.Read(layers.SwapHysteresis);
            success = success && ctx.Read(layers.MaxActivePrefabs);
            uint8_t useClimate;
            success = success && ctx.Read(useClimate);
            if (success) layers.UseClimateGradients = useClimate != 0;
            
            // Climate configuration
            success = success && ctx.Read(layers.Climate.MinAltitude);
            success = success && ctx.Read(layers.Climate.MaxAltitude);
            success = success && ctx.Read(layers.Climate.MinLongitude);
            success = success && ctx.Read(layers.Climate.MaxLongitude);
            
            // Vertical gradient
            uint32_t vertGradCount = 0;
            success = success && ctx.Read(vertGradCount);
            if (success) {
                layers.Climate.VerticalGradient.Points.resize(vertGradCount);
                for (uint32_t i = 0; i < vertGradCount && success; ++i) {
                    auto& pt = layers.Climate.VerticalGradient.Points[i];
                    success = ctx.Read(pt.Position);
                    success = success && ctx.Read(pt.Temperature);
                    success = success && ctx.Read(pt.Moisture);
                    success = success && ctx.Read(pt.WindExposure);
                }
            }
            
            // Longitudinal gradient
            uint32_t longGradCount = 0;
            success = success && ctx.Read(longGradCount);
            if (success) {
                layers.Climate.LongitudinalGradient.Points.resize(longGradCount);
                for (uint32_t i = 0; i < longGradCount && success; ++i) {
                    auto& pt = layers.Climate.LongitudinalGradient.Points[i];
                    success = ctx.Read(pt.Position);
                    success = success && ctx.Read(pt.Temperature);
                    success = success && ctx.Read(pt.Moisture);
                    success = success && ctx.Read(pt.WindExposure);
                }
            }
            
            // Layers array
            uint32_t layerCount = 0;
            success = success && ctx.Read(layerCount);
            if (success) {
                layers.Layers.resize(layerCount);
                for (uint32_t i = 0; i < layerCount && success; ++i) {
                    auto& layer = layers.Layers[i];
                    
                    // Layer identity
                    success = ctx.Read(layer.Guid.high);
                    success = success && ctx.Read(layer.Guid.low);
                    uint32_t nameLen = 0;
                    success = success && ctx.Read(nameLen);
                    if (success && nameLen > 0) {
                        layer.Name.resize(nameLen);
                        ctx.Read(layer.Name.data(), nameLen);
                    }
                    uint8_t enabled;
                    success = success && ctx.Read(enabled);
                    if (success) layer.Enabled = enabled != 0;
                    
                    // Prefab reference
                    success = success && ctx.Read(layer.PrefabAsset.guid.high);
                    success = success && ctx.Read(layer.PrefabAsset.guid.low);
                    uint32_t prefabPathLen = 0;
                    success = success && ctx.Read(prefabPathLen);
                    if (success && prefabPathLen > 0) {
                        layer.PrefabPath.resize(prefabPathLen);
                        ctx.Read(layer.PrefabPath.data(), prefabPathLen);
                    }
                    
                    // Distribution
                    success = success && ctx.Read(layer.DensityPerSquareMeter);
                    success = success && ctx.Read(layer.MinSpacing);
                    success = success && ctx.Read(layer.MinScale);
                    success = success && ctx.Read(layer.MaxScale);
                    uint8_t nonUniform;
                    success = success && ctx.Read(nonUniform);
                    if (success) layer.NonUniformScale = nonUniform != 0;
                    success = success && ctx.Read(layer.MinScaleVec.x);
                    success = success && ctx.Read(layer.MinScaleVec.y);
                    success = success && ctx.Read(layer.MinScaleVec.z);
                    success = success && ctx.Read(layer.MaxScaleVec.x);
                    success = success && ctx.Read(layer.MaxScaleVec.y);
                    success = success && ctx.Read(layer.MaxScaleVec.z);
                    success = success && ctx.Read(layer.YawVarianceDegrees);
                    success = success && ctx.Read(layer.PitchVarianceDegrees);
                    success = success && ctx.Read(layer.RollVarianceDegrees);
                    uint8_t alignSlope;
                    success = success && ctx.Read(alignSlope);
                    if (success) layer.AlignToSlope = alignSlope != 0;
                    success = success && ctx.Read(layer.SlopeAlignmentFactor);
                    success = success && ctx.Read(layer.HeightOffset);
                    success = success && ctx.Read(layer.HeightOffsetVariance);
                    
                    // Clustering
                    uint8_t enableCluster;
                    success = success && ctx.Read(enableCluster);
                    if (success) layer.EnableClustering = enableCluster != 0;
                    success = success && ctx.Read(layer.ClusterRadius);
                    success = success && ctx.Read(layer.ClusterMinCount);
                    success = success && ctx.Read(layer.ClusterMaxCount);
                    success = success && ctx.Read(layer.ClusterFalloff);
                    success = success && ctx.Read(layer.ClusterSpacing);
                    
                    // LOD
                    uint8_t useImposter;
                    success = success && ctx.Read(useImposter);
                    if (success) layer.UseImposter = useImposter != 0;
                    success = success && ctx.Read(layer.ImposterDistance);
                    success = success && ctx.Read(layer.CullDistance);
                    success = success && ctx.Read(layer.CrossfadeRange);
                    
                    // Interaction
                    uint8_t interactable, preservePhys;
                    success = success && ctx.Read(interactable);
                    if (success) layer.Interactable = interactable != 0;
                    success = success && ctx.Read(preservePhys);
                    if (success) layer.PreservePhysics = preservePhys != 0;
                    success = success && ctx.Read(layer.InteractionRadius);
                    uint32_t tagLen = 0;
                    success = success && ctx.Read(tagLen);
                    if (success && tagLen > 0) {
                        layer.InteractionTag.resize(tagLen);
                        ctx.Read(layer.InteractionTag.data(), tagLen);
                    }
                    
                    // Preview
                    success = success && ctx.Read(layer.PreviewColor.r);
                    success = success && ctx.Read(layer.PreviewColor.g);
                    success = success && ctx.Read(layer.PreviewColor.b);
                    
                    // Eligibility filters (JSON blob for polymorphic filters)
                    uint32_t eligLen = 0;
                    success = success && ctx.Read(eligLen);
                    if (success && eligLen > 0) {
                        std::string eligStr(eligLen, '\0');
                        ctx.Read(eligStr.data(), eligLen);
                        try {
                            nlohmann::json eligJson = nlohmann::json::parse(eligStr);
                            layer.Eligibility.Deserialize(eligJson);
                        } catch (const std::exception& e) {
                            std::cerr << "[EntityBinaryLoader] Failed to parse eligibility JSON: " << e.what() << std::endl;
                        }
                    }
                }
            }
            
            std::cout << "[EntityBinaryLoader] Loaded ResourceLayers with " << layers.Layers.size() << " layers" << std::endl;
            break;
        }
        
        case ComponentTypeId::Instancer: {
            data->Instancer = std::make_unique<cm::instancer::InstancerComponent>();
            auto& inst = *data->Instancer;
            
            // Asset references
            uint64_t meshGuidHigh, meshGuidLow;
            uint32_t meshPathLen;
            success = ctx.Read(meshGuidHigh) && ctx.Read(meshGuidLow);
            success = success && ctx.Read(meshPathLen);
            if (success) {
                inst.MeshAsset.guid.high = meshGuidHigh;
                inst.MeshAsset.guid.low = meshGuidLow;
                if (meshPathLen > 0) {
                    inst.MeshPath.resize(meshPathLen);
                    ctx.Read(inst.MeshPath.data(), meshPathLen);
                }
            }
            uint64_t prefabGuidHigh, prefabGuidLow;
            uint32_t prefabPathLen;
            success = success && ctx.Read(prefabGuidHigh) && ctx.Read(prefabGuidLow);
            success = success && ctx.Read(prefabPathLen);
            if (success) {
                inst.PrefabAsset.guid.high = prefabGuidHigh;
                inst.PrefabAsset.guid.low = prefabGuidLow;
                if (prefabPathLen > 0) {
                    inst.PrefabPath.resize(prefabPathLen);
                    ctx.Read(inst.PrefabPath.data(), prefabPathLen);
                }
            }
            int32_t surfaceEntity;
            success = success && ctx.Read(surfaceEntity);
            if (success) inst.SurfaceEntity = static_cast<EntityID>(surfaceEntity);
            
            // Distribution settings
            success = success && ctx.Read(inst.Distribution.Seed);
            success = success && ctx.Read(inst.Distribution.DensityPerSquareMeter);
            success = success && ctx.Read(inst.Distribution.MinSpacing);
            success = success && ctx.Read(inst.Distribution.MinScale);
            success = success && ctx.Read(inst.Distribution.MaxScale);
            uint8_t nonUniform;
            success = success && ctx.Read(nonUniform);
            if (success) inst.Distribution.NonUniformScale = nonUniform != 0;
            success = success && ctx.Read(inst.Distribution.MinScaleVec.x);
            success = success && ctx.Read(inst.Distribution.MinScaleVec.y);
            success = success && ctx.Read(inst.Distribution.MinScaleVec.z);
            success = success && ctx.Read(inst.Distribution.MaxScaleVec.x);
            success = success && ctx.Read(inst.Distribution.MaxScaleVec.y);
            success = success && ctx.Read(inst.Distribution.MaxScaleVec.z);
            success = success && ctx.Read(inst.Distribution.YawVarianceDegrees);
            success = success && ctx.Read(inst.Distribution.PitchVarianceDegrees);
            success = success && ctx.Read(inst.Distribution.RollVarianceDegrees);
            uint8_t alignSlope;
            success = success && ctx.Read(alignSlope);
            if (success) inst.Distribution.AlignToSlope = alignSlope != 0;
            success = success && ctx.Read(inst.Distribution.SlopeAlignmentFactor);
            success = success && ctx.Read(inst.Distribution.MinSlopeDegrees);
            success = success && ctx.Read(inst.Distribution.MaxSlopeDegrees);
            success = success && ctx.Read(inst.Distribution.HeightOffset);
            success = success && ctx.Read(inst.Distribution.HeightOffsetVariance);
            
            // Distribution area
            success = success && ctx.Read(inst.DistributionRadius);
            success = success && ctx.Read(inst.DistributionAreaMin.x);
            success = success && ctx.Read(inst.DistributionAreaMin.y);
            success = success && ctx.Read(inst.DistributionAreaMax.x);
            success = success && ctx.Read(inst.DistributionAreaMax.y);
            uint8_t useRadius;
            success = success && ctx.Read(useRadius);
            if (success) inst.UseRadiusMode = useRadius != 0;
            
            // Manual points
            uint8_t useManual;
            success = success && ctx.Read(useManual);
            if (success) inst.UseManualPoints = useManual != 0;
            uint32_t numManualPoints;
            success = success && ctx.Read(numManualPoints);
            if (success) {
                inst.ManualPoints.resize(numManualPoints);
                for (uint32_t i = 0; i < numManualPoints && success; ++i) {
                    success = ctx.Read(inst.ManualPoints[i].x);
                    success = success && ctx.Read(inst.ManualPoints[i].y);
                    success = success && ctx.Read(inst.ManualPoints[i].z);
                }
            }
            
            // Swap settings
            success = success && ctx.Read(inst.Swap.SwapDistance);
            success = success && ctx.Read(inst.Swap.SwapHysteresis);
            success = success && ctx.Read(inst.Swap.CullDistance);
            success = success && ctx.Read(inst.Swap.MaxActivePrefabs);
            
            // Flags
            uint8_t enabled, showMarkers, showBnds;
            success = success && ctx.Read(enabled);
            if (success) inst.Enabled = enabled != 0;
            success = success && ctx.Read(inst.PreviewColor.r);
            success = success && ctx.Read(inst.PreviewColor.g);
            success = success && ctx.Read(inst.PreviewColor.b);
            success = success && ctx.Read(showMarkers);
            success = success && ctx.Read(showBnds);
            if (success) {
                inst.ShowDebugMarkers = showMarkers != 0;
                inst.ShowBounds = showBnds != 0;
            }
            
            // Rendering options
            uint8_t useAlpha;
            success = success && ctx.Read(useAlpha);
            if (success) inst.UseAlphaCutout = useAlpha != 0;
            success = success && ctx.Read(inst.AlphaCutoutThreshold);
            
            // Persistent state
            uint32_t numDestroyed;
            success = success && ctx.Read(numDestroyed);
            if (success) {
                for (uint32_t i = 0; i < numDestroyed; ++i) {
                    uint32_t id;
                    if (ctx.Read(id)) {
                        inst.Persistent.DestroyedIDs.push_back(id);
                    }
                }
            }
            uint32_t numOverrides;
            success = success && ctx.Read(numOverrides);
            if (success) {
                for (uint32_t i = 0; i < numOverrides; ++i) {
                    uint32_t id;
                    uint32_t state;
                    if (ctx.Read(id) && ctx.Read(state)) {
                        inst.Persistent.StateOverrides[id] = static_cast<cm::instancer::InstanceState>(state);
                    }
                }
            }
            
            // Mark for regeneration after load
            inst.NeedsRegeneration = true;
            inst.NeedsMeshReload = true;
            
            std::cout << "[EntityBinaryLoader] Loaded instancer for entity '" << data->Name 
                      << "' MeshGUID=" << inst.MeshAsset.guid.ToString() << std::endl;
            break;
        }
        
        case ComponentTypeId::Module: {
            // Deserialize all Module components into Dynamic map
            uint32_t moduleCount = 0;
            success = ctx.Read(moduleCount);
            if (success) {
                for (uint32_t i = 0; i < moduleCount && success; ++i) {
                    // Read typeId
                    uint64_t typeIdHigh, typeIdLow;
                    success = success && ctx.Read(typeIdHigh) && ctx.Read(typeIdLow);
                    if (!success) break;
                    
                    cm::TypeId typeId;
                    typeId.hi = typeIdHigh;
                    typeId.lo = typeIdLow;
                    
                    // Read version
                    uint32_t version = 1;
                    success = success && ctx.Read(version);
                    if (!success) break;
                    
                    // Create ModuleComponent
                    cm::ModuleComponent module(typeId, version);
                    
                    // Look up schema from ComponentRegistry
                    const auto* desc = cm::ComponentRegistry::Instance().Find(typeId);
                    if (desc) {
                        module.DefineFields(desc->fields);
                    } else {
                        std::cerr << "[LoadComponentBinary] Unknown module schema for typeId=" 
                                  << typeId.ToHex() << ". Module may be disabled." << std::endl;
                    }
                    
                    // Read field count
                    uint32_t fieldCount = 0;
                    success = success && ctx.Read(fieldCount);
                    if (!success) break;
                    
                    // Read each field
                    for (uint32_t j = 0; j < fieldCount && success; ++j) {
                        // Read field name (string index)
                        uint32_t nameIdx = 0;
                        success = success && ctx.Read(nameIdx);
                        if (!success) break;
                        
                        std::string fieldName = ctx.ReadString(nameIdx);
                        if (fieldName.empty()) continue;
                        
                        // Read field type
                        uint8_t fieldTypeByte = 0;
                        success = success && ctx.Read(fieldTypeByte);
                        if (!success) break;
                        
                        cm::ValueType fieldType = static_cast<cm::ValueType>(fieldTypeByte);
                        
                        // Read value based on type
                        cm::Variant value;
                        value.type = fieldType;
                        
                        switch (fieldType) {
                            case cm::ValueType::Bool: {
                                uint8_t b = 0;
                                success = success && ctx.Read(b);
                                if (success) value.value = (b != 0);
                                break;
                            }
                            case cm::ValueType::Int: {
                                int32_t v = 0;
                                success = success && ctx.Read(v);
                                if (success) value.value = v;
                                break;
                            }
                            case cm::ValueType::Int64: {
                                int64_t v = 0;
                                success = success && ctx.Read(v);
                                if (success) value.value = v;
                                break;
                            }
                            case cm::ValueType::Float: {
                                float v = 0.0f;
                                success = success && ctx.Read(v);
                                if (success) value.value = v;
                                break;
                            }
                            case cm::ValueType::Double: {
                                double v = 0.0;
                                success = success && ctx.Read(v);
                                if (success) value.value = v;
                                break;
                            }
                            case cm::ValueType::String: {
                                uint32_t strIdx = 0;
                                success = success && ctx.Read(strIdx);
                                if (success) {
                                    std::string str = ctx.ReadString(strIdx);
                                    value.value = str;
                                }
                                break;
                            }
                            case cm::ValueType::Vec2: {
                                float x = 0.0f, y = 0.0f;
                                success = success && ctx.Read(x) && ctx.Read(y);
                                if (success) value.value = glm::vec2(x, y);
                                break;
                            }
                            case cm::ValueType::Vec3: {
                                float x = 0.0f, y = 0.0f, z = 0.0f;
                                success = success && ctx.Read(x) && ctx.Read(y) && ctx.Read(z);
                                if (success) value.value = glm::vec3(x, y, z);
                                break;
                            }
                            case cm::ValueType::Vec4: {
                                float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
                                success = success && ctx.Read(x) && ctx.Read(y) && ctx.Read(z) && ctx.Read(w);
                                if (success) value.value = glm::vec4(x, y, z, w);
                                break;
                            }
                            case cm::ValueType::Quat: {
                                float w = 0.0f, x = 0.0f, y = 0.0f, z = 0.0f;
                                success = success && ctx.Read(w) && ctx.Read(x) && ctx.Read(y) && ctx.Read(z);
                                if (success) value.value = glm::quat(w, x, y, z);
                                break;
                            }
                            case cm::ValueType::Guid: {
                                uint32_t guidIdx = 0;
                                success = success && ctx.Read(guidIdx);
                                if (success) {
                                        std::string guid = ctx.ReadString(guidIdx);
                                    value.value = guid;
                                }
                                break;
                            }
                            case cm::ValueType::Enum: {
                                int32_t v = 0;
                                success = success && ctx.Read(v);
                                if (success) value.value = cm::EnumValue{v};
                                break;
                            }
                            default:
                                // Unsupported type - skip this field
                                break;
                        }
                        
                        // Set the field value if we successfully read it
                        if (success && module.HasField(fieldName)) {
                            module.Set(fieldName, value);
                        }
                    }
                    
                    // Store module in Dynamic map
                    if (success) {
                        data->Dynamic[typeId] = std::move(module);
                    }
                }
            }
            break;
        }
        
        // Skip unknown component types gracefully
        default:
            std::cerr << "[LoadComponentBinary] Unknown component type: " << static_cast<int>(typeId) << std::endl;
            break;
    }
    
    return success;
}

void EntityBinaryLoader::LoadEnvironment(LoadContext& ctx, const SceneBinaryHeader& header, Scene& scene) {
    if (header.environmentOffset == 0 || header.environmentSize == 0) {
        return;
    }
    
    ctx.pos = header.environmentOffset;
    Environment& env = scene.GetEnvironment();
    
    // Read environment data matching the writer format
    uint32_t ambientMode = 0;
    ctx.Read(ambientMode);
    env.Ambient = static_cast<Environment::AmbientMode>(ambientMode);
    
    ctx.Read(env.AmbientColor.x);
    ctx.Read(env.AmbientColor.y);
    ctx.Read(env.AmbientColor.z);
    ctx.Read(env.AmbientIntensity);
    ctx.Read(env.Exposure);
    
    // Fog
    uint8_t enableFog = 0;
    ctx.Read(enableFog);
    env.EnableFog = enableFog != 0;
    ctx.Read(env.FogColor.x);
    ctx.Read(env.FogColor.y);
    ctx.Read(env.FogColor.z);
    ctx.Read(env.FogDensity);
    
    // Procedural sky
    uint8_t proceduralSky = 0;
    ctx.Read(proceduralSky);
    env.ProceduralSky = proceduralSky != 0;
    ctx.Read(env.SkyTopColor.x);
    ctx.Read(env.SkyTopColor.y);
    ctx.Read(env.SkyTopColor.z);
    ctx.Read(env.SkyHorizonColor.x);
    ctx.Read(env.SkyHorizonColor.y);
    ctx.Read(env.SkyHorizonColor.z);
    ctx.Read(env.SkyGroundColor.x);
    ctx.Read(env.SkyGroundColor.y);
    ctx.Read(env.SkyGroundColor.z);
    ctx.Read(env.SunSize);
    ctx.Read(env.SunSizeConvergence);
    ctx.Read(env.SunIntensity);
    ctx.Read(env.AtmosphereThickness);
    ctx.Read(env.HorizonFade);
    ctx.Read(env.SkyExposure);
    
    // Shadows
    uint8_t shadowsEnabled = 0;
    ctx.Read(shadowsEnabled);
    env.ShadowsEnabled = shadowsEnabled != 0;
    int32_t shadowRes = 0;
    ctx.Read(shadowRes);
    env.ShadowMapResolution = shadowRes;
    ctx.Read(env.ShadowDistance);
    ctx.Read(env.ShadowBias);
    ctx.Read(env.ShadowNormalBias);
    ctx.Read(env.ShadowSoftness);
    int32_t shadowSamples = 0;
    ctx.Read(shadowSamples);
    env.ShadowSamples = shadowSamples;
    ctx.Read(env.ShadowStrength);
    
    // Outline (cosmetic)
    uint8_t outlineEnabled = 0;
    ctx.Read(outlineEnabled);
    env.OutlineEnabled = outlineEnabled != 0;
    ctx.Read(env.OutlineColor.x);
    ctx.Read(env.OutlineColor.y);
    ctx.Read(env.OutlineColor.z);
    ctx.Read(env.OutlineThickness);
    if (header.base.version >= 32) {
        uint32_t textureFilter = 0;
        uint32_t textureMaxDimension = 0;
        uint32_t renderResolutionWidth = 0;
        uint32_t renderResolutionHeight = 0;
        ctx.Read(textureFilter);
        ctx.Read(textureMaxDimension);
        ctx.Read(renderResolutionWidth);
        ctx.Read(renderResolutionHeight);
        if (textureFilter == static_cast<uint32_t>(Environment::TextureFilterMode::Point)) {
            env.TextureFilter = Environment::TextureFilterMode::Point;
        } else if (textureFilter == static_cast<uint32_t>(Environment::TextureFilterMode::Anisotropic)) {
            env.TextureFilter = Environment::TextureFilterMode::Anisotropic;
        } else {
            env.TextureFilter = Environment::TextureFilterMode::Linear;
        }
        env.TextureMaxDimension = static_cast<uint16_t>(textureMaxDimension);
        env.RenderResolutionWidth = static_cast<uint16_t>(renderResolutionWidth);
        env.RenderResolutionHeight = static_cast<uint16_t>(renderResolutionHeight);
    }
    if (header.base.version >= 33) {
        uint8_t useSkybox = 0;
        uint8_t useSkyboxEquirectangular = 0;
        uint32_t equirectPathIdx = 0;
        uint32_t equirectResolution = 0;
        ctx.Read(useSkybox);
        ctx.Read(useSkyboxEquirectangular);
        ctx.Read(equirectPathIdx);
        ctx.Read(equirectResolution);
        env.UseSkybox = useSkybox != 0;
        env.UseSkyboxEquirectangular = useSkyboxEquirectangular != 0;
        env.SkyboxEquirectangularPath = ctx.ReadString(equirectPathIdx);
        env.SkyboxEquirectangularResolution = static_cast<uint16_t>(equirectResolution);
        for (std::string& facePath : env.SkyboxFacePaths) {
            uint32_t facePathIdx = 0;
            ctx.Read(facePathIdx);
            facePath = ctx.ReadString(facePathIdx);
        }
    }
    env.SkyboxTexture.reset();
    env.SkyboxRuntimeCacheKey.clear();

    std::cout << "[EntityBinaryLoader] Loaded environment data (" << header.environmentSize << " bytes)" << std::endl;
    std::cout << "[EntityBinaryLoader]   ProceduralSky: " << (env.ProceduralSky ? "true" : "false") << std::endl;
    std::cout << "[EntityBinaryLoader]   ShadowsEnabled: " << (env.ShadowsEnabled ? "true" : "false") << std::endl;
}

// Helper to create a PBR or PSX material from inline data
static std::shared_ptr<Material> CreateMaterialFromInlineData(const binary::InlineMaterialData& inl, bool skinned) {
    const std::string cacheKey = BuildInlineMaterialCacheKey(inl, skinned);
    {
        std::lock_guard<std::mutex> lock(s_inlineMaterialCacheMutex);
        auto it = s_inlineMaterialCache.find(cacheKey);
        if (it != s_inlineMaterialCache.end()) {
            if (auto existing = it->second.lock()) {
                return ShareEquivalentMaterial(existing);
            }
            s_inlineMaterialCache.erase(it);
        }
    }

    std::string shaderVS, shaderFS;
    
    // Determine shader based on material type
    if (inl.materialType == binary::InlineMaterialType::PSX) {
        shaderVS = skinned ? "vs_psx_skinned" : "vs_psx";
        shaderFS = "fs_psx";
    } else {
        shaderVS = skinned ? "vs_pbr_skinned" : "vs_pbr";
        shaderFS = skinned ? "fs_pbr_skinned" : "fs_pbr";
    }
    
    auto program = ShaderManager::Instance().LoadProgram(shaderVS, shaderFS);
    if (!bgfx::isValid(program)) {
        std::cerr << "[EntityBinaryLoader] Failed to load shader program: " << shaderVS << " + " << shaderFS << std::endl;
        return nullptr;
    }
    
    std::shared_ptr<PBRMaterial> mat;
    if (skinned) {
        mat = std::make_shared<SkinnedPBRMaterial>("InlineMaterial", program);
    } else {
        mat = std::make_shared<PBRMaterial>("InlineMaterial", program);
    }
    
    // Set common PBR properties
    mat->SetMetallic(inl.metallic);
    mat->SetRoughness(inl.roughness);
    mat->SetNormalScale(inl.normalScale);
    mat->SetAmbientOcclusion(inl.aoStrength);
    mat->SetEmissionStrength(inl.emissionStrength);
    mat->SetEmissionColor(inl.emissionColor);
    mat->SetDisplacementScale(inl.displacementScale);
    mat->SetUVTransform(inl.uvScale, inl.uvOffset);
    
    // Set color tint via uniform
    if (inl.tint != glm::vec4(1.0f)) {
        mat->SetUniform("u_ColorTint", inl.tint);
    }
    
    // Set PSX-specific uniforms. Seed the full PSX uniform set first so every
    // PSX uniform (posterize, shadow params, emission) exists on the material —
    // property-block overrides only bind for uniforms the material owns.
    if (inl.materialType == binary::InlineMaterialType::PSX) {
        MaterialManager::InitializePSXUniformDefaults(*mat, skinned);
        mat->SetUniform("u_psxParams", inl.psxParams);
        mat->SetUniform("u_psxWorld", inl.psxWorld);
        mat->SetUniform("u_toonParams", inl.toonParams);
        std::cout << "[EntityBinaryLoader]   Applied PSX uniforms" << std::endl;
    }
    
    // Load textures from paths
    if (!inl.albedoPath.empty()) {
        mat->SetAlbedoTextureFromPath(inl.albedoPath);
    }
    if (!inl.normalPath.empty()) {
        mat->SetNormalTextureFromPath(inl.normalPath);
    }
    if (!inl.metallicRoughnessPath.empty()) {
        mat->SetMetallicRoughnessTextureFromPath(inl.metallicRoughnessPath);
    }
    if (!inl.aoPath.empty()) {
        mat->SetAmbientOcclusionTextureFromPath(inl.aoPath);
    }
    if (!inl.emissionPath.empty()) {
        mat->SetEmissionTextureFromPath(inl.emissionPath);
    }
    if (!inl.displacementPath.empty()) {
        mat->SetDisplacementTextureFromPath(inl.displacementPath);
    }
    if (!inl.tintMaskPath.empty()) {
        mat->SetTintMaskTextureFromPath(inl.tintMaskPath);
    }
    
    // Set alpha blend state
    if (inl.hasAlpha) {
        mat->m_StateFlags = mat->GetStateFlags() | BGFX_STATE_BLEND_ALPHA;
    }

    if (inl.receiveShadowsOverride) {
        mat->SetReceiveShadowsOverride(true);
        mat->SetReceiveShadows(inl.receiveShadows);
    }
    
    std::shared_ptr<Material> shared = ShareEquivalentMaterial(mat);
    {
        std::lock_guard<std::mutex> lock(s_inlineMaterialCacheMutex);
        s_inlineMaterialCache[cacheKey] = shared;
    }
    return shared;
}

// Helper to create a RuntimeShaderGraphMaterial from shader graph data
static std::shared_ptr<Material> CreateShaderGraphMaterial(const ShaderGraphMaterialData& sg) {
    // Load compiled shader program
    if (sg.compiledVSName.empty() || sg.compiledFSName.empty()) {
        std::cerr << "[EntityBinaryLoader] ShaderGraph material missing compiled shader names" << std::endl;
        return nullptr;
    }
    
    auto program = ShaderManager::Instance().LoadProgram(sg.compiledVSName, sg.compiledFSName);
    if (!bgfx::isValid(program)) {
        std::cerr << "[EntityBinaryLoader] Failed to load shader graph program: " 
                  << sg.compiledVSName << " + " << sg.compiledFSName << std::endl;
        return nullptr;
    }
    
    auto mat = std::make_shared<cm::RuntimeShaderGraphMaterial>(sg.name, program);
    mat->SetShaderGraphPath(sg.shaderGraphPath);
    mat->SetUVScale(sg.uvScale);
    mat->SetUVOffset(sg.uvOffset);
    mat->m_StateFlags = sg.stateFlags;
    
    // Add parameters
    for (const auto& p : sg.parameters) {
        cm::RuntimeMaterialParameter param;
        param.name = p.name;
        param.displayName = p.displayName;
        param.type = static_cast<cm::RuntimeShaderValueType>(p.type);
        param.value = p.value;
        param.texturePath = p.texturePath;
        param.textureSlot = p.textureSlot;
        
        // Load texture if this is a texture parameter
        if (param.type == cm::RuntimeShaderValueType::Texture2D && !param.texturePath.empty()) {
            TextureSpecifier spec;
            spec.Path = param.texturePath;
            param.textureHandle = AcquireTextureHandle(spec, TextureColorSpace::Linear);
        }
        
        mat->AddParameter(param);
    }
    
    std::cout << "[EntityBinaryLoader]   Created ShaderGraph material: " << sg.name 
              << " with " << sg.parameters.size() << " parameters" << std::endl;
    
    return mat;
}

void EntityBinaryLoader::ResolveMaterialReferences(LoadContext& ctx, Scene& scene) {
    int materialsLoaded = 0;
    int materialsFailed = 0;
    int inlineMaterialsCreated = 0;
    int entitiesWithMesh = 0;
    int entitiesWithMaterialPaths = 0;
    
    std::vector<EntityID> entities;
    entities.reserve(scene.GetEntities().size());
    for (const Entity& e : scene.GetEntities()) {
        entities.push_back(e.GetID());
    }
    
    ResolveMaterialReferencesRange(ctx, scene, entities, 0, static_cast<size_t>(entities.size()),
                                   materialsLoaded, materialsFailed, inlineMaterialsCreated,
                                   entitiesWithMesh, entitiesWithMaterialPaths);
    
    std::cout << "[EntityBinaryLoader] Material resolution complete: " << materialsLoaded << " from paths, " 
              << inlineMaterialsCreated << " from inline, " << materialsFailed << " failed" << std::endl;
    
    ApplyDefaultMaterials(scene);
}

bool EntityBinaryLoader::ResolveMaterialReferencesRange(LoadContext& ctx,
                                                        Scene& scene,
                                                        const std::vector<EntityID>& entities,
                                                        size_t startIndex,
                                                        size_t maxCount,
                                                        int& materialsLoaded,
                                                        int& materialsFailed,
                                                        int& inlineMaterialsCreated,
                                                        int& entitiesWithMesh,
                                                        int& entitiesWithMaterialPaths) {
    if (entities.empty()) return true;
    size_t end = std::min(entities.size(), startIndex + maxCount);
    IAssetResolver* resolver = Assets::GetResolver();
    
    for (size_t idx = startIndex; idx < end; ++idx) {
        EntityID id = entities[idx];
        EntityData* data = scene.GetEntityData(id);
        if (!data || !data->Mesh) continue;
        
        entitiesWithMesh++;
        MeshComponent& mc = *data->Mesh;
        
        // Check if we need to use skinned material (has skinning component)
        bool needsSkinned = data->Skinning != nullptr;
        
        if (!mc.MaterialAssetPaths.empty()) {
            entitiesWithMaterialPaths++;
            std::cout << "[EntityBinaryLoader] Entity '" << data->Name 
                      << "' has " << mc.MaterialAssetPaths.size() << " material paths" << std::endl;
            
            // Resize materials vector to match paths
            if (mc.materials.size() < mc.MaterialAssetPaths.size()) {
                mc.materials.resize(mc.MaterialAssetPaths.size());
            }
            if (mc.OwnedMaterialSlots.size() < mc.MaterialAssetPaths.size()) {
                mc.OwnedMaterialSlots.resize(mc.MaterialAssetPaths.size(), false);
            }
            
            for (size_t i = 0; i < mc.MaterialAssetPaths.size(); ++i) {
                const std::string& matPath = mc.MaterialAssetPaths[i];
                
                if (matPath.empty()) {
                    continue;
                }
                
                std::string resolvedPath = matPath;
                if (resolver) {
                    std::string candidate = resolver->ResolvePath(matPath);
                    if (!candidate.empty()) {
                        resolvedPath = candidate;
                    }
                }
                
                // Convert .mat path to .matbin path for binary loading
                std::filesystem::path p(resolvedPath);
                std::string matBinPath = resolvedPath;
                if (p.extension() == ".mat") {
                    p.replace_extension(".matbin");
                    matBinPath = p.string();
                }
                
                std::cout << "[EntityBinaryLoader]   -> Trying to load: " << matBinPath << std::endl;
                
                // Load the material (passing skinned hint)
                auto material = MaterialBinaryLoader::Load(matBinPath, needsSkinned);
                if (material) {
                    mc.materials[i] = material;
                    mc.OwnedMaterialSlots[i] = false;
                    
                    // Set primary material reference too
                    if (i == 0) {
                        mc.material = mc.materials[i];
                    }
                    
                    materialsLoaded++;
                    std::cout << "[EntityBinaryLoader]   -> Loaded successfully" << std::endl;
                } else {
                    std::cerr << "[EntityBinaryLoader]   -> FAILED to load: " << matBinPath << std::endl;
                    materialsFailed++;
                }
            }
        }
        
        // Then try ShaderGraphMaterials
        if (!mc.ShaderGraphMaterials.empty()) {
            std::cout << "[EntityBinaryLoader] Entity '" << data->Name 
                      << "' has " << mc.ShaderGraphMaterials.size() << " shader graph materials" << std::endl;
            
            // Resize materials vector if needed
            size_t maxSlot = mc.materials.size();
            for (const auto& sg : mc.ShaderGraphMaterials) {
                maxSlot = std::max(maxSlot, (size_t)(sg.slotIndex + 1));
            }
            if (mc.materials.size() < maxSlot) {
                mc.materials.resize(maxSlot);
            }
            if (mc.OwnedMaterialSlots.size() < maxSlot) {
                mc.OwnedMaterialSlots.resize(maxSlot, false);
            }
            
            for (const auto& sg : mc.ShaderGraphMaterials) {
                auto mat = ShareEquivalentMaterial(CreateShaderGraphMaterial(sg));
                if (mat) {
                    if (sg.slotIndex >= mc.materials.size()) {
                        mc.materials.resize(sg.slotIndex + 1);
                    }
                    if (sg.slotIndex >= mc.OwnedMaterialSlots.size()) {
                        mc.OwnedMaterialSlots.resize(sg.slotIndex + 1, false);
                    }
                    mc.materials[sg.slotIndex] = mat;
                    mc.OwnedMaterialSlots[sg.slotIndex] = !IsEquivalentMaterialShared(mat);
                    
                    if (sg.slotIndex == 0 || !mc.material) {
                        mc.material = mat;
                    }
                    
                    inlineMaterialsCreated++;
                }
            }
        }
        
        // Finally try to create materials from InlineMaterials (from model imports)
        if (!mc.InlineMaterials.empty()) {
            std::cout << "[EntityBinaryLoader] Entity '" << data->Name 
                      << "' has " << mc.InlineMaterials.size() << " inline materials" << std::endl;
            
            // Resize materials vector if needed
            size_t maxSlot = mc.materials.size();
            for (const auto& inl : mc.InlineMaterials) {
                maxSlot = std::max(maxSlot, (size_t)(inl.slotIndex + 1));
            }
            if (mc.materials.size() < maxSlot) {
                mc.materials.resize(maxSlot);
            }
            if (mc.OwnedMaterialSlots.size() < maxSlot) {
                mc.OwnedMaterialSlots.resize(maxSlot, false);
            }
            
            for (const auto& inl : mc.InlineMaterials) {
                // Create PBR material from inline data
                auto mat = CreateMaterialFromInlineData(inl, needsSkinned);
                if (mat) {
                    if (inl.slotIndex >= mc.materials.size()) {
                        mc.materials.resize(inl.slotIndex + 1);
                    }
                    if (inl.slotIndex >= mc.OwnedMaterialSlots.size()) {
                        mc.OwnedMaterialSlots.resize(inl.slotIndex + 1, false);
                    }
                    mc.materials[inl.slotIndex] = mat;
                    mc.OwnedMaterialSlots[inl.slotIndex] = !IsEquivalentMaterialShared(mat);
                    
                    if (inl.slotIndex == 0 || !mc.material) {
                        mc.material = mat;
                    }
                    
                    inlineMaterialsCreated++;
                    std::cout << "[EntityBinaryLoader]   Created inline material for slot " << inl.slotIndex << std::endl;
                }
            }
        }

        mc.UniqueMaterial = std::any_of(
            mc.OwnedMaterialSlots.begin(),
            mc.OwnedMaterialSlots.end(),
            [](bool owned) { return owned; });
        
        // If we loaded any materials but primary isn't set, use first
        if (!mc.material && !mc.materials.empty() && mc.materials[0]) {
            mc.material = mc.materials[0];
        }
    }
    
    return end >= entities.size();
}

void EntityBinaryLoader::ApplyDefaultMaterials(Scene& scene) {
    // Apply default materials for meshes that have no materials loaded
    int defaultsApplied = 0;
    for (const Entity& e : scene.GetEntities()) {
        EntityData* data = scene.GetEntityData(e.GetID());
        if (!data || !data->Mesh) continue;
        
        MeshComponent& mc = *data->Mesh;
        
        // Skip if already has a primary material
        if (mc.material) continue;
        
        // Check if we have a mesh but no material
        if (mc.mesh) {
            bool needsSkinned = data->Skinning != nullptr;
            
            // Create default material based on whether mesh is skinned
            mc.material = AcquireSharedDefaultMaterial(scene, needsSkinned);
            
            // Also set in materials array if needed
            if (mc.materials.empty()) {
                mc.materials.push_back(mc.material);
            } else if (!mc.materials[0]) {
                mc.materials[0] = mc.material;
            }
            
            defaultsApplied++;
            std::cout << "[EntityBinaryLoader] Applied default " 
                      << (needsSkinned ? "skinned " : "") << "material to entity: " 
                      << data->Name << std::endl;
        }
    }
    
    if (defaultsApplied > 0) {
        std::cout << "[EntityBinaryLoader] Applied " << defaultsApplied << " default materials" << std::endl;
    }
}

void EntityBinaryLoader::BuildSkeletonAvatars(Scene& scene) {
    // This function is a FALLBACK for old binary files that don't have avatar data.
    // New exports should include avatar data serialized with the skeleton.
    int avatarsBuilt = 0;
    int avatarsLoaded = 0;
    
    for (const Entity& e : scene.GetEntities()) {
        EntityData* data = scene.GetEntityData(e.GetID());
        if (!data || !data->Skeleton) continue;
        
        SkeletonComponent& skel = *data->Skeleton;
        
        // If avatar was loaded from binary, count it
        if (skel.Avatar) {
            avatarsLoaded++;
            continue;
        }
        
        // Fallback: Build avatar from skeleton data using automatic bone name mapping
        // This is only for backwards compatibility with old binary files
        skel.Avatar = std::make_unique<cm::animation::AvatarDefinition>();
        cm::animation::avatar_builders::BuildFromSkeleton(skel, *skel.Avatar, true);
        avatarsBuilt++;
        
        std::cout << "[EntityBinaryLoader] WARNING: Built avatar at runtime for skeleton on entity '" 
                  << data->Name << "' - re-export scene for optimal results" << std::endl;
    }
    
    if (avatarsLoaded > 0) {
        std::cout << "[EntityBinaryLoader] Loaded " << avatarsLoaded << " pre-built avatars from binary" << std::endl;
    }
    if (avatarsBuilt > 0) {
        std::cout << "[EntityBinaryLoader] Built " << avatarsBuilt 
                  << " avatars at runtime (re-export scene to include avatar data)" << std::endl;
    }
}

void EntityBinaryLoader::FixupAnimationComponents(Scene& scene) {
    // The AnimationSystem requires both AnimationPlayer AND Skeleton on the same entity.
    // In some scene setups, the AnimationPlayer might be on a parent entity while
    // the Skeleton is on a child entity. This function migrates the AnimationPlayer
    // to the skeleton entity if needed.
    
    int fixedCount = 0;
    
    std::unordered_map<EntityID, std::vector<EntityID>> childrenByParent;
    childrenByParent.reserve(scene.GetEntities().size());

    // Debug: Count entities with AnimationPlayer and Skeleton before fixup
    int apCount = 0, skelCount = 0, bothCount = 0;
    for (const Entity& e : scene.GetEntities()) {
        EntityData* data = scene.GetEntityData(e.GetID());
        if (!data) continue;
        if (data->AnimationPlayer) apCount++;
        if (data->Skeleton) skelCount++;
        if (data->AnimationPlayer && data->Skeleton) bothCount++;
        if (data->Parent != INVALID_ENTITY_ID) {
            childrenByParent[data->Parent].push_back(e.GetID());
        }
    }
    std::cout << "[EntityBinaryLoader] FixupAnimation: " << apCount << " entities with AnimationPlayer, "
              << skelCount << " with Skeleton, " << bothCount << " with both" << std::endl;
    
    // Build GUID -> EntityID map for entity reference resolution
    // Key formula: high ^ (low << 1) - must match JSON serializer for consistency
    std::unordered_map<uint64_t, EntityID> guidHighToId;
    std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> entityGuidMap;
    for (const Entity& e : scene.GetEntities()) {
        EntityData* data = scene.GetEntityData(e.GetID());
        if (!data) continue;
        if (data->EntityGuid.high != 0 || data->EntityGuid.low != 0) {
            // Use high ^ (low << 1) as key to reduce collisions and match JSON serializer
            uint64_t key = data->EntityGuid.high ^ (data->EntityGuid.low << 1);
            guidHighToId[key] = e.GetID();
            entityGuidMap[e.GetID()] = {data->EntityGuid.high, data->EntityGuid.low};
        }
    }
    
    // Helper to compute consistent GUID key
    auto computeGuidKey = [](uint64_t high, uint64_t low) -> uint64_t {
        return high ^ (low << 1);
    };
    
    // Resolve LookAt target entity GUIDs
    int lookAtResolved = 0;
    for (const Entity& e : scene.GetEntities()) {
        EntityData* data = scene.GetEntityData(e.GetID());
        if (!data) continue;
        
        for (auto& lac : data->LookAtConstraints) {
            if (lac.TargetEntityGuidHigh != 0 || lac.TargetEntityGuidLow != 0) {
                uint64_t key = computeGuidKey(lac.TargetEntityGuidHigh, lac.TargetEntityGuidLow);
                auto it = guidHighToId.find(key);
                if (it != guidHighToId.end()) {
                    lac.TargetEntity = it->second;
                    lookAtResolved++;
                }
            }
        }
        
        // Also resolve IK target/pole entity GUIDs
        for (auto& ik : data->IKs) {
            if (ik.TargetEntityGuidHigh != 0 || ik.TargetEntityGuidLow != 0) {
                uint64_t key = computeGuidKey(ik.TargetEntityGuidHigh, ik.TargetEntityGuidLow);
                auto it = guidHighToId.find(key);
                if (it != guidHighToId.end()) {
                    ik.TargetEntity = it->second;
                }
            }
            if (ik.PoleEntityGuidHigh != 0 || ik.PoleEntityGuidLow != 0) {
                uint64_t key = computeGuidKey(ik.PoleEntityGuidHigh, ik.PoleEntityGuidLow);
                auto it = guidHighToId.find(key);
                if (it != guidHighToId.end()) {
                    ik.PoleEntity = it->second;
                }
            }
        }
    }
    
    if (lookAtResolved > 0) {
        std::cout << "[EntityBinaryLoader] Resolved " << lookAtResolved << " LookAt target entity references" << std::endl;
    }
    
    // First pass: find entities with AnimationPlayer but no Skeleton
    for (const Entity& e : scene.GetEntities()) {
        EntityData* data = scene.GetEntityData(e.GetID());
        if (!data) continue;
        
        // Skip if this entity already has both AnimationPlayer and Skeleton
        if (data->AnimationPlayer && data->Skeleton) {
            std::cout << "[EntityBinaryLoader] Entity '" << data->Name << "' already has both AnimationPlayer and Skeleton" << std::endl;
            continue;
        }
        
        // Skip if no AnimationPlayer
        if (!data->AnimationPlayer) continue;
        
        std::cout << "[EntityBinaryLoader] Entity '" << data->Name << "' has AnimationPlayer but no Skeleton, searching children..." << std::endl;
        
        // This entity has AnimationPlayer but no Skeleton
        // Find a child entity with Skeleton and move/copy AnimationPlayer there
        std::function<EntityID(EntityID)> findSkeletonChild = [&](EntityID parentId) -> EntityID {
            auto childrenIt = childrenByParent.find(parentId);
            if (childrenIt == childrenByParent.end()) {
                return 0;
            }

            for (EntityID childId : childrenIt->second) {
                EntityData* childData = scene.GetEntityData(childId);
                if (!childData) continue;
                
                if (childData->Skeleton) {
                    std::cout << "[EntityBinaryLoader]   Found skeleton on child '" << childData->Name << "'" << std::endl;
                    return childId;
                }
                
                // Recursively search children
                EntityID found = findSkeletonChild(childId);
                if (found != 0) return found;
            }
            return 0;
        };
        
        EntityID skeletonChildId = findSkeletonChild(e.GetID());
        if (skeletonChildId != 0) {
            EntityData* skeletonData = scene.GetEntityData(skeletonChildId);
            if (skeletonData) {
                // Move the AnimationPlayer to the skeleton entity
                if (!skeletonData->AnimationPlayer) {
                    std::cout << "[EntityBinaryLoader] Moving AnimationPlayer from '" << data->Name 
                              << "' to skeleton entity '" << skeletonData->Name << "'" << std::endl;
                    skeletonData->AnimationPlayer = std::move(data->AnimationPlayer);
                    std::cout << "[EntityBinaryLoader] Moved AnimationPlayer from '" << data->Name 
                              << "' to skeleton entity '" << skeletonData->Name << "'" << std::endl;
                    fixedCount++;
                } else {
                    // Skeleton already has AnimationPlayer - merge controller path if needed
                    if (skeletonData->AnimationPlayer->ControllerPath.empty() && 
                        !data->AnimationPlayer->ControllerPath.empty()) {
                        skeletonData->AnimationPlayer->ControllerPath = data->AnimationPlayer->ControllerPath;
                        std::cout << "[EntityBinaryLoader] Copied ControllerPath from '" << data->Name 
                                  << "' to skeleton entity '" << skeletonData->Name << "'" << std::endl;
                        fixedCount++;
                    }
                    data->AnimationPlayer.reset();
                }
            }
        } else {
            std::cout << "[EntityBinaryLoader] WARNING: No skeleton child found for entity '" << data->Name << "'" << std::endl;
        }
    }
    
    if (fixedCount > 0) {
        std::cout << "[EntityBinaryLoader] Fixed " << fixedCount << " animation component locations" << std::endl;
    }
}

void EntityBinaryLoader::RemapEntityReferences(Scene& scene, const std::unordered_map<EntityID, EntityID>& oldToNewIdMap) {
    int remappedCount = 0;
    int skeletonBonesMapped = 0;
    int skeletonBonesNotFound = 0;
    
    for (const Entity& e : scene.GetEntities()) {
        EntityData* data = scene.GetEntityData(e.GetID());
        if (!data) continue;
        
        // Remap Skeleton BoneEntities
        if (data->Skeleton) {
            std::cout << "[EntityBinaryLoader] Remapping " << data->Skeleton->BoneEntities.size() 
                      << " bone entities for skeleton on '" << data->Name << "'" << std::endl;
            
            for (size_t i = 0; i < data->Skeleton->BoneEntities.size(); ++i) {
                EntityID oldId = data->Skeleton->BoneEntities[i];
                if (oldId == static_cast<EntityID>(-1) || oldId == 0) continue;
                
                auto it = oldToNewIdMap.find(oldId);
                if (it != oldToNewIdMap.end()) {
                    data->Skeleton->BoneEntities[i] = it->second;
                    skeletonBonesMapped++;
                } else {
                    // Set to invalid if not found
                    data->Skeleton->BoneEntities[i] = static_cast<EntityID>(-1);
                    skeletonBonesNotFound++;
                }
            }
            
            // Debug: verify mapping
            int validBones = 0;
            for (const auto& be : data->Skeleton->BoneEntities) {
                if (be != static_cast<EntityID>(-1) && be != 0) {
                    EntityData* boneData = scene.GetEntityData(be);
                    if (boneData) validBones++;
                }
            }
            std::cout << "[EntityBinaryLoader]   Valid bone entities after remap: " << validBones 
                      << "/" << data->Skeleton->BoneEntities.size() << std::endl;
        }
        
        // Remap Skinning SkeletonRoot
        if (data->Skinning) {
            EntityID oldId = data->Skinning->SkeletonRoot;
            if (oldId != 0 && oldId != static_cast<EntityID>(-1)) {
                auto it = oldToNewIdMap.find(oldId);
                if (it != oldToNewIdMap.end()) {
                    data->Skinning->SkeletonRoot = it->second;
                    remappedCount++;
                }
            }
        }
        
        // Remap BoneAttachment SkeletonEntity
        if (data->BoneAttachment) {
            EntityID oldId = data->BoneAttachment->SkeletonEntity;
            if (oldId != 0 && oldId != static_cast<EntityID>(-1)) {
                auto it = oldToNewIdMap.find(oldId);
                if (it != oldToNewIdMap.end()) {
                    data->BoneAttachment->SkeletonEntity = it->second;
                    remappedCount++;
                }
            }
        }
        
        // Remap IK target and pole entities (if stored as entity IDs)
        for (auto& ik : data->IKs) {
            EntityID oldTargetId = ik.TargetEntity;
            if (oldTargetId != 0 && oldTargetId != static_cast<EntityID>(-1)) {
                auto it = oldToNewIdMap.find(oldTargetId);
                if (it != oldToNewIdMap.end()) {
                    ik.TargetEntity = it->second;
                    remappedCount++;
                }
            }
            
            EntityID oldPoleId = ik.PoleEntity;
            if (oldPoleId != 0 && oldPoleId != static_cast<EntityID>(-1)) {
                auto it = oldToNewIdMap.find(oldPoleId);
                if (it != oldToNewIdMap.end()) {
                    ik.PoleEntity = it->second;
                    remappedCount++;
                }
            }
        }
        
        // Remap LookAt target entities
        for (auto& lac : data->LookAtConstraints) {
            EntityID oldId = lac.TargetEntity;
            if (oldId != 0 && oldId != static_cast<EntityID>(-1)) {
                auto it = oldToNewIdMap.find(oldId);
                if (it != oldToNewIdMap.end()) {
                    lac.TargetEntity = it->second;
                    remappedCount++;
                }
            }
        }
        
        // Remap NavAgent NavMeshEntity
        if (data->NavAgent) {
            EntityID oldId = data->NavAgent->NavMeshEntity;
            if (oldId != 0 && oldId != static_cast<EntityID>(-1)) {
                auto it = oldToNewIdMap.find(oldId);
                if (it != oldToNewIdMap.end()) {
                    data->NavAgent->NavMeshEntity = it->second;
                    remappedCount++;
                }
            }
        }
    }
    
    if (remappedCount > 0 || skeletonBonesMapped > 0) {
        std::cout << "[EntityBinaryLoader] Remapped entity references: " << remappedCount 
                  << " component refs, " << skeletonBonesMapped << " skeleton bones" << std::endl;
    }
}

void EntityBinaryLoader::LoadModelDeltas(LoadContext& ctx, const SceneBinaryHeader& header, Scene& scene) {
    using namespace cm::model;
    
    if (header.modelDeltaCount == 0) {
        return;
    }
    
    std::cout << "[EntityBinaryLoader] Loading " << header.modelDeltaCount << " model deltas" << std::endl;
    
    // Seek to delta table
    ctx.pos = header.modelDeltaTableOffset;
    
    std::vector<ModelDeltaEntry> entries(header.modelDeltaCount);
    for (uint32_t i = 0; i < header.modelDeltaCount; ++i) {
        if (!ctx.Read(&entries[i], sizeof(ModelDeltaEntry))) {
            std::cerr << "[EntityBinaryLoader] Failed to read delta entry " << i << std::endl;
            return;
        }
    }
    
    // Create applicator
    ModelDeltaApplicator applicator(scene);
    DeltaApplicationConfig config;
    // Scene binaries already contain full entity/component/script state. Keep model
    // delta replay scoped to structural/model overrides to prevent stale ID/visibility
    // resurrection.
    config.applyScripts = false;
    config.applyEntityOverrides = false;
    config.verbose = false;
    config.allowUnmatchedDeltas = true;
    config.fastHierarchy = true;
    
    int appliedCount = 0;
    int failedCount = 0;
    
    for (const auto& entry : entries) {
        // Find the model root by entity ID
        EntityID rootId = static_cast<EntityID>(entry.rootEntityId);
        auto* rootData = scene.GetEntityData(rootId);
        
        if (!rootData) {
            std::cerr << "[EntityBinaryLoader] Model root entity not found: " << rootId << std::endl;
            failedCount++;
            continue;
        }
        
        // Read delta data
        if (entry.dataOffset + entry.dataSize > ctx.size) {
            std::cerr << "[EntityBinaryLoader] Delta data out of bounds for entity " << rootId << std::endl;
            failedCount++;
            continue;
        }
        
        std::string jsonStr(reinterpret_cast<const char*>(ctx.data + entry.dataOffset), entry.dataSize);
        
        try {
            nlohmann::json j = nlohmann::json::parse(jsonStr);
            ModelDelta delta = ModelDelta::FromJson(j);
            
            // Apply delta
            DeltaApplicationResult result = applicator.Apply(rootId, delta, config);
            
            if (result.HasErrors()) {
                std::cerr << "[EntityBinaryLoader] Delta application had errors for " << rootData->Name << ":";
                for (const auto& err : result.errors) {
                    std::cerr << " " << err;
                }
                std::cerr << std::endl;
                failedCount++;
            } else {
                appliedCount++;
                if (!result.warnings.empty()) {
                    std::cout << "[EntityBinaryLoader] Delta applied to " << rootData->Name 
                              << " with " << result.warnings.size() << " warnings" << std::endl;
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[EntityBinaryLoader] Failed to parse delta for " << rootData->Name 
                      << ": " << e.what() << std::endl;
            failedCount++;
        }
    }
    
    std::cout << "[EntityBinaryLoader] Applied " << appliedCount << " model deltas, " 
              << failedCount << " failed" << std::endl;
}

} // namespace binary
