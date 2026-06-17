#include "RuntimePrefabInstantiator.h"
#include "PrefabAsset.h"
#include "PrefabBinaryLoader.h"
#include "PrefabDelta.h"
#include "PrefabInstanceComponent.h"
#include "core/ecs/Scene.h"
#include "core/ecs/EntityData.h"
#include "core/ecs/Components.h"
#include "core/ecs/ScenePostProcessing.h"
#include "core/model/ModelDelta.h"
#include "core/model/ModelDeltaApplicator.h"
#include "core/assets/IAssetResolver.h"
#include "core/assets/BinaryFormats.h"
#include "core/assets/AssetReference.h"
#include "core/vfs/FileSystem.h"
#include "core/animation/AnimationPlayerComponent.h"
#include "core/animation/AnimatorController.h"
#include "core/animation/AnimatorControllerIO.h"
#include "core/animation/AnimatorControllerOverrideIO.h"
#include "core/animation/AvatarDefinition.h"
#include "core/animation/ik/IKComponent.h"
#include "core/animation/lookat/LookAtConstraintComponent.h"
#include "core/physics/Physics.h"
#include "core/physics/area/AreaComponent.h"
#include "core/navigation/NavMesh.h"
#include "core/navigation/NavAgent.h"
#include "core/rendering/PBRMaterial.h"
#include "core/rendering/SkinnedPBRMaterial.h"
#include "core/rendering/RuntimeShaderGraphMaterial.h"
#include "core/rendering/ShaderManager.h"
#include "core/rendering/TextureLoader.h"
#include "core/rendering/MaterialCache.h"
#include "core/rendering/MaterialManager.h"
#include "core/rendering/VertexTypes.h"
#include "core/rendering/Terrain.h"
#include "core/serialization/MeshBinaryLoader.h"
#include "core/serialization/MeshCache.h"
#include "core/serialization/MaterialCache.h"  // Material caching for performance
#include "core/rendering/DeferredGPUBuffers.h"  // Deferred GPU buffer creation
#include "core/serialization/MaterialBinaryLoader.h"  // For loading materials from paths
#include "core/serialization/EntityBinaryLoader.h"  // For shared LoadComponentBinary
#include "core/debug/PrefabLog.h"
#include "core/utils/PrefabPerfDiagnostics.h"
#include "core/utils/Profiler.h"
#include "core/jobs/ParallelFor.h"
#include "core/jobs/Jobs.h"
#include "managed/interop/ScriptComponent.h"
#include "managed/interop/DotNetHost.h"  // For CallOnValidateForSubtree
#include "core/managed/ScriptReflection.h"
#include "core/managed/ScriptSystem.h"  // For creating script instances
#include "core/managed/ScriptOrder.h"   // For OnCreate ordering by [Priority]
#include "core/managed/DeferredScriptInit.h"  // For optional deferred OnCreate
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <functional>
#include <fstream>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <list>
#include <deque>
#include <sstream>
#include <chrono>
#include <atomic>
#include <mutex>
#include <cstring>
#include <cfloat>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include "core/vfs/VirtualFS.h"
#include <cmath>

// Collision-resistant GUID packing using FNV-1a hash
static uint64_t PackGuidHash(const ClaymoreGUID& g) { 
    constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
    constexpr uint64_t FNV_PRIME = 1099511628211ULL;
    
    uint64_t hash = FNV_OFFSET;
    
    for (int i = 0; i < 8; ++i) {
        hash ^= (g.high >> (i * 8)) & 0xFF;
        hash *= FNV_PRIME;
    }
    
    for (int i = 0; i < 8; ++i) {
        hash ^= (g.low >> (i * 8)) & 0xFF;
        hash *= FNV_PRIME;
    }
    
    return hash;
}

namespace {
#ifndef NDEBUG
class PrefabScopedTimer {
public:
    explicit PrefabScopedTimer(const char* label)
        : m_Label(label),
          m_Start(std::chrono::high_resolution_clock::now()) {}

    ~PrefabScopedTimer() {
        if (!cm::debug::PrefabPerfDetailedTimingsEnabled()) {
            return;
        }
        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - m_Start).count();
        PREFAB_LOG(m_Label << " took " << ms << " ms");
    }

private:
    const char* m_Label;
    std::chrono::high_resolution_clock::time_point m_Start;
};
#else
class PrefabScopedTimer {
public:
    explicit PrefabScopedTimer(const char*) {}
};
#endif

constexpr double kPrefabPerfConsoleLogThresholdMs = 0.25;

uint32_t FloatBits(float value)
{
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float size mismatch");
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::string BuildInlineMaterialCacheKey(const binary::InlineMaterialData& inl, bool skinned)
{
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

std::shared_ptr<Material> AcquireSharedDefaultMaterial(Scene& scene, bool skinned)
{
    MaterialSource source;
    source.Skinned = skinned;
    return AcquireMaterialFromSource(source, scene);
}

std::mutex s_inlineMaterialCacheMutex;
std::unordered_map<std::string, std::weak_ptr<Material>> s_inlineMaterialCache;

std::shared_ptr<Material> ShareEquivalentMaterial(const std::shared_ptr<Material>& material)
{
    return AcquireEquivalentMaterial(material);
}

bool IsEquivalentMaterialShared(const std::shared_ptr<Material>& material)
{
    return material &&
        GetMaterialEquivalenceKey(material.get()).EquivalentSafe;
}

cm::debug::PrefabPerfLabel BuildPrefabPerfLabel(Scene& scene,
                                                EntityID prefabRootId,
                                                const std::string& prefabPath,
                                                size_t ownedEntityCount) {
    auto label = cm::debug::DescribePrefabRoot(scene, prefabRootId);
    if (!label.IsValid() && prefabRootId != INVALID_ENTITY_ID) {
        label.RootId = prefabRootId;
        if (EntityData* data = scene.GetEntityData(prefabRootId)) {
            label.RootName = data->Name;
        }
    }

    if (label.PrefabPath.empty()) {
        label.PrefabPath = prefabPath;
    }
    if (label.PrefabName.empty()) {
        try {
            const std::filesystem::path fsPath(prefabPath);
            if (fsPath.has_filename()) {
                label.PrefabName = fsPath.filename().string();
            }
        } catch (...) {
        }
    }
    if (label.PrefabName.empty()) {
        label.PrefabName = label.RootName.empty() ? "prefab" : label.RootName;
    }
    if (label.OwnedEntityCount == 0 && ownedEntityCount > 0) {
        label.OwnedEntityCount = ownedEntityCount;
    }
    return label;
}

std::string BuildPrefabPerfDetails(size_t entityCount, const std::string& extraDetails) {
    if (entityCount == 0) {
        return extraDetails;
    }
    if (extraDetails.empty()) {
        return "entities=" + std::to_string(entityCount);
    }
    return "entities=" + std::to_string(entityCount) + " " + extraDetails;
}

class PrefabPerfStageTimer {
public:
    PrefabPerfStageTimer(Scene& scene,
                         EntityID prefabRootId,
                         const std::string& prefabPath,
                         size_t entityCount,
                         const char* profilerPrefix,
                         const char* category)
        : m_Label(BuildPrefabPerfLabel(scene, prefabRootId, prefabPath, entityCount)),
          m_EntityCount(entityCount),
          m_ProfilerPrefix(profilerPrefix),
          m_Category(category),
          m_Start(std::chrono::high_resolution_clock::now()) {}

    void SetExtraDetails(std::string extraDetails) {
        m_ExtraDetails = std::move(extraDetails);
    }

    ~PrefabPerfStageTimer() {
        const auto end = std::chrono::high_resolution_clock::now();
        const double durationMs = std::chrono::duration<double, std::milli>(end - m_Start).count();
        cm::debug::RecordPrefabProfilerSample(m_Label, m_ProfilerPrefix, durationMs);
        if (durationMs >= kPrefabPerfConsoleLogThresholdMs) {
            cm::debug::LogPrefabPerfEvent(
                m_Category,
                m_Label,
                durationMs,
                BuildPrefabPerfDetails(m_EntityCount, m_ExtraDetails));
        }
    }

private:
    cm::debug::PrefabPerfLabel m_Label;
    size_t m_EntityCount = 0;
    const char* m_ProfilerPrefix = nullptr;
    const char* m_Category = nullptr;
    std::string m_ExtraDetails;
    std::chrono::high_resolution_clock::time_point m_Start;
};

static std::mutex s_prefabJsonCacheMutex;
static std::unordered_map<ClaymoreGUID, nlohmann::json, ClaymoreGUIDHasher> s_prefabJsonCache;

struct PrefabBinaryCacheEntry {
    std::shared_ptr<std::vector<uint8_t>> data;
    size_t size = 0;
    std::list<std::string>::iterator lruIt;
};

struct CachedModelDelta {
    uint32_t rootEntityId = 0;
    cm::model::ModelDelta delta;
};

static std::mutex s_prefabBinaryCacheMutex;
static std::unordered_map<std::string, PrefabBinaryCacheEntry> s_prefabBinaryCache;
static std::list<std::string> s_prefabBinaryCacheLru;
static size_t s_prefabBinaryCacheBytes = 0;
static constexpr size_t kPrefabBinaryCacheMaxBytes = 128 * 1024 * 1024;
static std::unordered_map<std::string, std::shared_ptr<std::vector<CachedModelDelta>>> s_prefabModelDeltaCache;

static bool IsValidEntityRefValue(int id) {
    return id > 0 && id != static_cast<int>(INVALID_ENTITY_ID);
}

static std::string DescribeEntityRefTarget(Scene& scene, int id) {
    if (!IsValidEntityRefValue(id)) return "None";
    auto* d = scene.GetEntityData(static_cast<EntityID>(id));
    return d ? d->Name : "Missing";
}

static std::string DescribeEntityRefMeta(const ScriptEntityRefMetadata* meta) {
    if (!meta) return "meta:none";
    std::ostringstream oss;
    oss << "meta:{";
    bool first = true;
    auto add = [&](const std::string& part) {
        if (!first) oss << ", ";
        oss << part;
        first = false;
    };
    if (meta->guid.high != 0 || meta->guid.low != 0) {
        add(std::string("guid=") + meta->guid.ToString());
    }
    if (meta->modelGuid.high != 0 || meta->modelGuid.low != 0) {
        add(std::string("modelGuid=") + meta->modelGuid.ToString());
    }
    if (meta->modelRootGuid.high != 0 || meta->modelRootGuid.low != 0) {
        add(std::string("modelRootGuid=") + meta->modelRootGuid.ToString());
    }
    if (!meta->modelNodePath.empty()) {
        add(std::string("modelNodePath=") + meta->modelNodePath);
    }
    if (meta->entityId > 0) {
        add(std::string("metaEntityId=") + std::to_string(meta->entityId));
    }
    if (first) add("empty");
    oss << "}";
    return oss.str();
}

static bool HasEntityRefHints(const ScriptEntityRefMetadata* meta) {
    if (!meta) return false;
    return (meta->guid.high != 0 || meta->guid.low != 0 ||
            meta->modelGuid.high != 0 || meta->modelGuid.low != 0 ||
            meta->modelRootGuid.high != 0 || meta->modelRootGuid.low != 0 ||
            !meta->modelNodePath.empty() || meta->entityId > 0);
}

static bool ShouldUsePrefabBinaryCache() {
    // RuntimePrefabInstantiator only consumes compiled prefab payloads. Cache them
    // in editor too so drag-in, hot-swap, and previews instantiate from prepared
    // state instead of re-reading/parsing the same .prefabb repeatedly.
    return true;
}

static std::string NormalizePrefabCacheKey(const std::string& path) {
    if (path.empty()) return {};
    try {
        return IVirtualFS::NormalizePath(path);
    } catch (...) {
        std::string normalized = path;
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        return normalized;
    }
}

static bool IsPreparedBinaryReady(const std::string& sourcePath, const std::string& binaryPath) {
    if (binaryPath.empty() || !FileSystem::Instance().Exists(binaryPath)) {
        return false;
    }
    if (auto* resolver = Assets::GetResolver()) {
        return resolver->IsBinaryCurrent(sourcePath);
    }
    return true;
}

static std::shared_ptr<std::vector<uint8_t>> TryGetPrefabBinaryCache(const std::string& key) {
    if (key.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(s_prefabBinaryCacheMutex);
    auto it = s_prefabBinaryCache.find(key);
    if (it == s_prefabBinaryCache.end()) {
        return nullptr;
    }
    s_prefabBinaryCacheLru.splice(s_prefabBinaryCacheLru.begin(), s_prefabBinaryCacheLru, it->second.lruIt);
    return it->second.data;
}

static std::shared_ptr<std::vector<CachedModelDelta>> TryGetPrefabModelDeltaCache(const std::string& key) {
    if (key.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(s_prefabBinaryCacheMutex);
    auto it = s_prefabModelDeltaCache.find(key);
    if (it == s_prefabModelDeltaCache.end()) {
        return nullptr;
    }
    return it->second;
}

static void StorePrefabModelDeltaCache(const std::string& key, std::shared_ptr<std::vector<CachedModelDelta>> deltas) {
    if (key.empty() || !deltas) return;
    std::lock_guard<std::mutex> lock(s_prefabBinaryCacheMutex);
    s_prefabModelDeltaCache[key] = std::move(deltas);
}

static void StorePrefabBinaryCache(const std::string& key, std::shared_ptr<std::vector<uint8_t>> data) {
    if (key.empty() || !data) return;
    std::lock_guard<std::mutex> lock(s_prefabBinaryCacheMutex);
    auto it = s_prefabBinaryCache.find(key);
    if (it != s_prefabBinaryCache.end()) {
        s_prefabBinaryCacheBytes -= it->second.size;
        s_prefabBinaryCacheLru.erase(it->second.lruIt);
        s_prefabBinaryCache.erase(it);
        s_prefabModelDeltaCache.erase(key);
    }
    s_prefabBinaryCacheLru.push_front(key);
    PrefabBinaryCacheEntry entry;
    entry.data = std::move(data);
    entry.size = entry.data->size();
    entry.lruIt = s_prefabBinaryCacheLru.begin();
    s_prefabBinaryCacheBytes += entry.size;
    s_prefabBinaryCache.emplace(key, std::move(entry));
    while (s_prefabBinaryCacheBytes > kPrefabBinaryCacheMaxBytes && !s_prefabBinaryCacheLru.empty()) {
        const std::string& evictKey = s_prefabBinaryCacheLru.back();
        auto evictIt = s_prefabBinaryCache.find(evictKey);
        if (evictIt != s_prefabBinaryCache.end()) {
            s_prefabBinaryCacheBytes -= evictIt->second.size;
            s_prefabBinaryCache.erase(evictIt);
            s_prefabModelDeltaCache.erase(evictKey);
        }
        s_prefabBinaryCacheLru.pop_back();
    }
}

static void ClearPrefabRuntimeCachesLocked() {
    s_prefabBinaryCache.clear();
    s_prefabBinaryCacheLru.clear();
    s_prefabBinaryCacheBytes = 0;
    s_prefabModelDeltaCache.clear();
}

static void AppendUniqueKey(std::vector<std::string>& keys, const std::string& raw)
{
    if (raw.empty()) {
        return;
    }
    std::string key = NormalizePrefabCacheKey(raw);
    if (key.empty()) {
        return;
    }
    if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
        keys.push_back(std::move(key));
    }
}

static std::vector<std::string> BuildPrefabCacheInvalidationKeys(const std::string& prefabPath)
{
    std::vector<std::string> keys;
    AppendUniqueKey(keys, prefabPath);
    try {
        std::filesystem::path path(prefabPath);
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (ext == ".prefab" || ext == ".json") {
            path.replace_extension(".prefabb");
            AppendUniqueKey(keys, path.string());
        } else if (ext == ".prefabb") {
            path.replace_extension(".prefab");
            AppendUniqueKey(keys, path.string());
            path.replace_extension(".json");
            AppendUniqueKey(keys, path.string());
        }
    } catch (...) {
    }
    return keys;
}

static ClaymoreGUID TryReadPrefabSourceGuidForCacheKey(const std::string& key)
{
    ClaymoreGUID guid{};
    try {
        std::filesystem::path path(key);
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (ext != ".prefab" && ext != ".json") {
            return guid;
        }

        std::string jsonText;
        if (!FileSystem::Instance().ReadTextFile(key, jsonText)) {
            std::ifstream file(key);
            if (!file.is_open()) {
                return guid;
            }
            std::ostringstream buffer;
            buffer << file.rdbuf();
            jsonText = buffer.str();
        }

        nlohmann::json json = nlohmann::json::parse(jsonText, nullptr, false);
        if (json.is_discarded() || !json.contains("guid")) {
            return guid;
        }
        json.at("guid").get_to(guid);
    } catch (...) {
    }
    return guid;
}

static void ErasePrefabJsonCacheForGuid(const ClaymoreGUID& guid)
{
    if (guid.high == 0 && guid.low == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(s_prefabJsonCacheMutex);
    s_prefabJsonCache.erase(guid);
}

} // namespace

// Forward declaration of global remap function (defined at end of file)
// Note: Default arguments are specified in RuntimePrefabInstantiator.h, not here
void RemapPrefabEntityReferences(Scene& scene, const std::vector<EntityID>& createdEntityIds,
                                  const std::unordered_map<EntityID, EntityID>& oldToNewIdMap,
                                  const std::unordered_map<ClaymoreGUID, ClaymoreGUID>* prefabToInstanceGuid,
                                  const std::unordered_map<ClaymoreGUID, EntityID>* instanceGuidToId);

namespace runtime {

using json = nlohmann::json;
using namespace binary;

// Read context for binary components
struct ReadContext {
    const uint8_t* data;
    size_t dataSize;
    size_t offset;
    const uint8_t* stringTableData;
    size_t stringTableOffset;
    std::vector<std::string> strings;
    const std::vector<std::string>* stringView = nullptr;
    
    bool Read(void* dst, size_t count) {
        if (offset + count > dataSize) return false;
        std::memcpy(dst, data + offset, count);
        offset += count;
        return true;
    }
    
    template<typename T>
    bool Read(T& value) {
        return Read(&value, sizeof(T));
    }
    
    std::string ReadString(uint32_t index) {
        const std::vector<std::string>* table = stringView ? stringView : &strings;
        if (table && !table->empty() && index < table->size()) {
            return (*table)[index];
        }
        // Fallback: navigate to string by counting null terminators
        size_t pos = stringTableOffset;
        uint32_t currentIdx = 0;
        while (currentIdx < index && pos < dataSize) {
            while (pos < dataSize && data[pos] != 0) ++pos;
            ++pos;  // skip null
            ++currentIdx;
        }
        if (pos >= dataSize) return "";

        size_t end = pos;
        while (end < dataSize && data[end] != 0) ++end;
        return std::string(reinterpret_cast<const char*>(data + pos), end - pos);
    }

    void BuildStringTable(size_t endOffset) {
        stringView = nullptr;
        strings.clear();
        if (stringTableOffset >= dataSize) return;
        size_t end = std::min(endOffset, dataSize);
        if (stringTableOffset >= end) return;
        size_t pos = stringTableOffset;
        while (pos < end) {
            size_t start = pos;
            while (pos < end && data[pos] != 0) {
                ++pos;
            }
            strings.emplace_back(reinterpret_cast<const char*>(data + start), pos - start);
            if (pos < end) {
                ++pos; // skip null terminator
            }
        }
    }
    
    void Seek(size_t newOffset) { offset = newOffset; }
};

// Inline primitive mesh creation (mirrors EntityBinaryLoader behavior)
static std::shared_ptr<Mesh> CreatePrimitiveCube() {
    static PBRVertex cubeVertices[] = {
        {-1,  1,  1,  0, 0, 1,  0, 0}, { 1,  1,  1,  0, 0, 1,  1, 0},
        {-1, -1,  1,  0, 0, 1,  0, 1}, { 1, -1,  1,  0, 0, 1,  1, 1},
        {-1,  1, -1,  0, 0, -1, 0, 0}, { 1,  1, -1,  0, 0, -1, 1, 0},
        {-1, -1, -1,  0, 0, -1, 0, 1}, { 1, -1, -1,  0, 0, -1, 1, 1},
        {-1,  1, -1, -1, 0, 0,  0, 0}, {-1,  1,  1, -1, 0, 0,  1, 0},
        {-1, -1, -1, -1, 0, 0,  0, 1}, {-1, -1,  1, -1, 0, 0,  1, 1},
        { 1,  1,  1,  1, 0, 0,  0, 0}, { 1,  1, -1,  1, 0, 0,  1, 0},
        { 1, -1,  1,  1, 0, 0,  0, 1}, { 1, -1, -1,  1, 0, 0,  1, 1},
        {-1,  1, -1,  0, 1, 0,  0, 0}, { 1,  1, -1,  0, 1, 0,  1, 0},
        {-1,  1,  1,  0, 1, 0,  0, 1}, { 1,  1,  1,  0, 1, 0,  1, 1},
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
        float y = radius * cosf(phi), ringRadius = radius * sinf(phi);
        for (int seg = 0; seg <= segments; ++seg) {
            float theta = (float)seg / segments * 2.0f * glm::pi<float>();
            float x = ringRadius * cosf(theta), z = ringRadius * sinf(theta);
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

// Entity header for binary format v4 (unified with scene format)
// Includes all entity-level properties for full state preservation
struct EntityHeader {
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
    uint64_t modelGuidHigh;     // ModelAssetGuid
    uint64_t modelGuidLow;
    uint32_t componentCount;
    uint32_t componentOffset;
};

// v3 entity header (without flags/layer/tag/modelGuid)
struct EntityHeaderV3 {
    uint64_t guidHigh;
    uint64_t guidLow;
    uint64_t parentGuidHigh;
    uint64_t parentGuidLow;
    uint32_t entityId;
    uint32_t nameIndex;
    uint32_t componentCount;
    uint32_t componentOffset;
};

// v2 entity header (without entityId)
struct EntityHeaderV2 {
    uint64_t guidHigh;
    uint64_t guidLow;
    uint64_t parentGuidHigh;
    uint64_t parentGuidLow;
    uint32_t nameIndex;
    uint32_t componentCount;
    uint32_t componentOffset;
};

struct ParsedPrefabLayout {
    PrefabBinaryHeader header{};
    bool isV4 = false;
    bool isV3 = false;
    ClaymoreGUID prefabAssetGuid{};
    std::vector<std::string> strings;
    std::vector<EntityHeader> entityHeaders;
};

static std::mutex s_prefabParsedLayoutCacheMutex;
static std::unordered_map<std::string, std::shared_ptr<ParsedPrefabLayout>> s_prefabParsedLayoutCache;

static bool IsParsedPrefabLayoutCompatible(const ParsedPrefabLayout& layout, const PrefabBinaryHeader& header)
{
    return layout.header.base.magic == header.base.magic &&
           layout.header.base.version == header.base.version &&
           layout.header.base.flags == header.base.flags &&
           layout.header.entityCount == header.entityCount &&
           layout.header.stringTableOffset == header.stringTableOffset &&
           layout.header.modelDeltaTableOffset == header.modelDeltaTableOffset;
}

static std::shared_ptr<ParsedPrefabLayout> TryGetParsedPrefabLayoutCache(const std::string& key)
{
    if (key.empty()) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(s_prefabParsedLayoutCacheMutex);
    auto it = s_prefabParsedLayoutCache.find(key);
    return it != s_prefabParsedLayoutCache.end() ? it->second : nullptr;
}

static void StoreParsedPrefabLayoutCache(const std::string& key, std::shared_ptr<ParsedPrefabLayout> layout)
{
    if (key.empty() || !layout) {
        return;
    }

    std::lock_guard<std::mutex> lock(s_prefabParsedLayoutCacheMutex);
    s_prefabParsedLayoutCache[key] = std::move(layout);
}

static void ErasePrefabRuntimeCacheForKey(const std::string& key)
{
    ClaymoreGUID prefabGuid{};
    {
        std::lock_guard<std::mutex> lock(s_prefabParsedLayoutCacheMutex);
        auto it = s_prefabParsedLayoutCache.find(key);
        if (it != s_prefabParsedLayoutCache.end()) {
            if (it->second) {
                prefabGuid = it->second->prefabAssetGuid;
            }
            s_prefabParsedLayoutCache.erase(it);
        }
    }
    {
        std::lock_guard<std::mutex> lock(s_prefabBinaryCacheMutex);
        auto it = s_prefabBinaryCache.find(key);
        if (it != s_prefabBinaryCache.end()) {
            s_prefabBinaryCacheBytes -= it->second.size;
            s_prefabBinaryCacheLru.erase(it->second.lruIt);
            s_prefabBinaryCache.erase(it);
        }
        s_prefabModelDeltaCache.erase(key);
    }
    if (prefabGuid.high != 0 || prefabGuid.low != 0) {
        std::lock_guard<std::mutex> lock(s_prefabJsonCacheMutex);
        s_prefabJsonCache.erase(prefabGuid);
    }
}

static bool BuildParsedPrefabLayout(const std::vector<uint8_t>& data,
                                    const PrefabBinaryHeader& header,
                                    ParsedPrefabLayout& out,
                                    std::string* error)
{
    auto fail = [&](const char* message) {
        if (error) {
            *error = message;
        }
        return false;
    };

    ReadContext ctx{};
    ctx.data = data.data();
    ctx.dataSize = data.size();
    ctx.stringTableOffset = header.stringTableOffset;

    size_t stringTableEnd = data.size();
    if (header.modelDeltaTableOffset > 0 && header.modelDeltaTableOffset < stringTableEnd) {
        stringTableEnd = header.modelDeltaTableOffset;
    }
    ctx.BuildStringTable(stringTableEnd);

    out = {};
    out.header = header;
    out.isV4 = (header.base.version >= 4);
    out.isV3 = (header.base.version >= 3);
    out.strings = ctx.strings;

    size_t offset = sizeof(PrefabBinaryHeader) + 16 + 4;
    if (offset > data.size()) {
        return fail("Prefab binary truncated before entity table");
    }

    size_t entityHeaderSize = out.isV4 ? sizeof(EntityHeader) : (out.isV3 ? sizeof(EntityHeaderV3) : sizeof(EntityHeaderV2));
    if (entityHeaderSize == 0 || header.entityCount > (data.size() - offset) / entityHeaderSize) {
        return fail("Prefab entity table out of bounds");
    }

    out.entityHeaders.resize(header.entityCount);
    for (uint32_t i = 0; i < header.entityCount; ++i) {
        ctx.offset = offset;
        if (out.isV4) {
            if (!ctx.Read(out.entityHeaders[i])) {
                return fail("Failed to read entity header");
            }
        } else if (out.isV3) {
            EntityHeaderV3 v3Header;
            if (!ctx.Read(v3Header)) {
                return fail("Failed to read v3 entity header");
            }
            out.entityHeaders[i].guidHigh = v3Header.guidHigh;
            out.entityHeaders[i].guidLow = v3Header.guidLow;
            out.entityHeaders[i].parentGuidHigh = v3Header.parentGuidHigh;
            out.entityHeaders[i].parentGuidLow = v3Header.parentGuidLow;
            out.entityHeaders[i].entityId = v3Header.entityId;
            out.entityHeaders[i].nameIndex = v3Header.nameIndex;
            out.entityHeaders[i].flags = 0x03;
            out.entityHeaders[i].layer = 0;
            out.entityHeaders[i].tagIndex = 0;
            out.entityHeaders[i].modelGuidHigh = 0;
            out.entityHeaders[i].modelGuidLow = 0;
            out.entityHeaders[i].componentCount = v3Header.componentCount;
            out.entityHeaders[i].componentOffset = v3Header.componentOffset;
        } else {
            EntityHeaderV2 v2Header;
            if (!ctx.Read(v2Header)) {
                return fail("Failed to read v2 entity header");
            }
            out.entityHeaders[i].guidHigh = v2Header.guidHigh;
            out.entityHeaders[i].guidLow = v2Header.guidLow;
            out.entityHeaders[i].parentGuidHigh = v2Header.parentGuidHigh;
            out.entityHeaders[i].parentGuidLow = v2Header.parentGuidLow;
            out.entityHeaders[i].entityId = static_cast<uint32_t>(i);
            out.entityHeaders[i].nameIndex = v2Header.nameIndex;
            out.entityHeaders[i].flags = 0x03;
            out.entityHeaders[i].layer = 0;
            out.entityHeaders[i].tagIndex = 0;
            out.entityHeaders[i].modelGuidHigh = 0;
            out.entityHeaders[i].modelGuidLow = 0;
            out.entityHeaders[i].componentCount = v2Header.componentCount;
            out.entityHeaders[i].componentOffset = v2Header.componentOffset;
        }
        offset = ctx.offset;
    }

    size_t prefabGuidOffset = sizeof(PrefabBinaryHeader);
    if (prefabGuidOffset + 16 <= data.size()) {
        std::memcpy(&out.prefabAssetGuid.high, data.data() + prefabGuidOffset, 8);
        std::memcpy(&out.prefabAssetGuid.low, data.data() + prefabGuidOffset + 8, 8);
    }

    return true;
}

enum class AsyncPrefabPhase {
    WaitingForData = 0,
    ParseHeaders,
    CreateEntities,
    ResolveParents,
    RemapReferences,
    ApplyModelDeltas,
    CollectEntities,
    ResolveMeshes,
    ResolveMaterials,
    PostProcess,
    UpdateTransforms,
    CreatePhysics,
    ApplyPropertyBlocks,
    WarmAnimations,
    InitScripts,
    Done,
    Failed
};

struct AsyncPrefabRequest {
    Scene* scene = nullptr;
    std::string prefabPath;
    std::string cacheKey;
    bool allowCache = false;
    bool allowDeltaCache = false;
    std::shared_ptr<std::vector<uint8_t>> data;
    std::atomic<bool> dataReady{false};
    std::atomic<bool> loadFailed{false};
    std::string loadError;
    std::mutex loadMutex;

    PrefabBinaryHeader header{};
    bool headerParsed = false;
    bool isV4 = false;
    bool isV3 = false;
    bool isLegacy = false;
    size_t entityHeaderSize = 0;
    size_t entityHeaderOffset = 0;
    ReadContext ctx{};
    std::shared_ptr<ParsedPrefabLayout> parsedLayout;
    const std::vector<EntityHeader>* entityHeaders = nullptr;
    size_t entityIndex = 0;
    size_t parentIndex = 0;

    ClaymoreGUID prefabAssetGuid{};
    std::optional<nlohmann::json> prefabEntitiesJson;

    std::unordered_map<uint64_t, EntityID> guidToEntityId;
    std::unordered_map<ClaymoreGUID, ClaymoreGUID> prefabToInstanceGuid;
    std::unordered_map<ClaymoreGUID, ClaymoreGUID> instanceToPrefabGuid;
    std::unordered_map<EntityID, EntityID> oldToNewIdMap;
    std::vector<EntityID> createdEntityIds;
    std::unordered_map<ClaymoreGUID, EntityID> instanceGuidToId;
    std::vector<EntityID> allPrefabEntities;
    std::vector<EntityID> unifiedMorphEntities;

    EntityID placeholderRoot = INVALID_ENTITY_ID;
    std::string placeholderName;
    EntityID overrideRoot = INVALID_ENTITY_ID;
    bool useOverrideRoot = false;

    AsyncPrefabPhase phase = AsyncPrefabPhase::WaitingForData;
    std::atomic<bool> cancelled{false};
};

static std::mutex s_asyncPrefabMutex;
static std::deque<std::shared_ptr<AsyncPrefabRequest>> s_asyncPrefabQueue;

static std::string DerivePrefabPlaceholderName(const std::string& path) {
    if (path.empty()) return "Prefab";
    try {
        std::filesystem::path p(path);
        std::string stem = p.stem().string();
        return stem.empty() ? "Prefab" : stem;
    } catch (...) {
        return "Prefab";
    }
}

static bool IsDefaultTransform(const TransformComponent& t) {
    const float eps = 0.0001f;
    auto nearEq = [&](float a, float b) { return std::fabs(a - b) <= eps; };
    return nearEq(t.Position.x, 0.0f) && nearEq(t.Position.y, 0.0f) && nearEq(t.Position.z, 0.0f) &&
           nearEq(t.Scale.x, 1.0f) && nearEq(t.Scale.y, 1.0f) && nearEq(t.Scale.z, 1.0f) &&
           nearEq(t.RotationQ.x, 0.0f) && nearEq(t.RotationQ.y, 0.0f) && nearEq(t.RotationQ.z, 0.0f) &&
           nearEq(t.RotationQ.w, 1.0f);
}

struct PrefabNavMeshCandidate {
    EntityID Entity = INVALID_ENTITY_ID;
    glm::vec3 Center{0.0f};
};

static std::vector<PrefabNavMeshCandidate> BuildPrefabNavMeshCandidates(Scene& scene) {
    std::vector<PrefabNavMeshCandidate> candidates;
    candidates.reserve(scene.GetEntities().size());

    const auto& allSceneEntities = scene.GetEntities();
    for (const Entity& entity : allSceneEntities) {
        EntityData* data = scene.GetEntityData(entity.GetID());
        if (!data || !data->Navigation || !data->Navigation->Runtime) {
            continue;
        }

        const glm::vec3 center =
            (data->Navigation->AABB.min + data->Navigation->AABB.max) * 0.5f;
        candidates.push_back({entity.GetID(), center});
    }

    return candidates;
}

static EntityID FindNearestPrefabNavMeshCandidate(
    const std::vector<PrefabNavMeshCandidate>& candidates,
    const glm::vec3& position) {
    float bestDistSq = FLT_MAX;
    EntityID best = 0;

    for (const PrefabNavMeshCandidate& candidate : candidates) {
        const float distSq = glm::distance2(position, candidate.Center);
        if (distSq < bestDistSq) {
            bestDistSq = distSq;
            best = candidate.Entity;
        }
    }

    return best;
}

static void AutoBindPrefabNavAgents(
    Scene& scene,
    const std::vector<EntityID>& prefabEntities,
    bool logBindings) {
    std::vector<EntityID> agentsToBind;
    agentsToBind.reserve(prefabEntities.size());

    for (EntityID id : prefabEntities) {
        EntityData* data = scene.GetEntityData(id);
        if (!data || !data->NavAgent || !data->NavAgent->Enabled ||
            (data->NavAgent->NavMeshEntity != 0 &&
             data->NavAgent->NavMeshEntity != INVALID_ENTITY_ID)) {
            continue;
        }

        agentsToBind.push_back(id);
    }

    if (agentsToBind.empty()) {
        return;
    }

    const std::vector<PrefabNavMeshCandidate> navMeshes =
        BuildPrefabNavMeshCandidates(scene);

    uint64_t agentsVisited = static_cast<uint64_t>(agentsToBind.size());
    uint64_t agentsBound = 0;
    uint64_t agentsUnbound = 0;

    for (EntityID id : agentsToBind) {
        EntityData* data = scene.GetEntityData(id);
        if (!data || !data->NavAgent) {
            continue;
        }

        const glm::vec3 position = glm::vec3(data->Transform.WorldMatrix[3]);
        const EntityID best = FindNearestPrefabNavMeshCandidate(navMeshes, position);
        if (best != 0) {
            data->NavAgent->NavMeshEntity = best;
            ++agentsBound;
            if (logBindings) {
                PREFAB_LOG("Auto-bound NavAgent on '" << data->Name
                    << "' to NavMeshEntity=" << best);
            }
        } else {
            ++agentsUnbound;
            if (logBindings) {
                PREFAB_LOG("NavAgent on '" << data->Name
                    << "' has no navmesh binding after instantiation (no navmesh found in scene)");
            }
        }
    }

    auto& profiler = Profiler::Get();
    profiler.AddCounter("Prefab/AutoBindNavMeshSceneScans", 1);
    profiler.AddCounter("Prefab/AutoBindNavMeshCandidates", static_cast<uint64_t>(navMeshes.size()));
    profiler.AddCounter("Prefab/AutoBindNavAgentsVisited", agentsVisited);
    profiler.AddCounter("Prefab/AutoBindNavAgentsBound", agentsBound);
    profiler.AddCounter("Prefab/AutoBindNavAgentsUnbound", agentsUnbound);
}

// Keep binary-instantiated entities in parity with authored entities by moving the
// complete component set from load buffers into the actual scene entities.
// This helper is shared by sync and async prefab instantiation paths.
static void MoveLoadedEntityComponents(EntityData& dst, EntityData& src) {
    dst.Mesh = std::move(src.Mesh);
    dst.MeshProxy = std::move(src.MeshProxy);
    dst.Light = std::move(src.Light);
    dst.BlendShapes = std::move(src.BlendShapes);
    dst.PendingBlendShapeWeights = std::move(src.PendingBlendShapeWeights);
    dst.UnifiedMorph = std::move(src.UnifiedMorph);
    dst.PendingUnifiedMorphWeights = std::move(src.PendingUnifiedMorphWeights);
    dst.TintController = std::move(src.TintController);
    dst.Skeleton = std::move(src.Skeleton);
    dst.Skinning = std::move(src.Skinning);
    dst.BoneAttachment = std::move(src.BoneAttachment);
    dst.ArmorFit = std::move(src.ArmorFit);
    dst.Collider = std::move(src.Collider);
    dst.Camera = std::move(src.Camera);
    dst.RigidBody = std::move(src.RigidBody);
    dst.StaticBody = std::move(src.StaticBody);
    dst.CharacterController = std::move(src.CharacterController);
    dst.GrassDeformer = std::move(src.GrassDeformer);
    dst.Terrain = std::move(src.Terrain);
    dst.River = std::move(src.River);
    dst.Spline = std::move(src.Spline);
    dst.ResourceLayers = std::move(src.ResourceLayers);
    dst.Instancer = std::move(src.Instancer);
    dst.Emitter = std::move(src.Emitter);
    dst.Area = std::move(src.Area);
    dst.Navigation = std::move(src.Navigation);
    dst.NavAgent = std::move(src.NavAgent);
    dst.NavLink = std::move(src.NavLink);
    dst.Portal = std::move(src.Portal);
    dst.Text = std::move(src.Text);
    dst.Canvas = std::move(src.Canvas);
    dst.Panel = std::move(src.Panel);
    dst.Button = std::move(src.Button);
    dst.Slider = std::move(src.Slider);
    dst.ProgressBar = std::move(src.ProgressBar);
    dst.Toggle = std::move(src.Toggle);
    dst.ScrollView = std::move(src.ScrollView);
    dst.LayoutGroup = std::move(src.LayoutGroup);
    dst.InputField = std::move(src.InputField);
    dst.Dropdown = std::move(src.Dropdown);
    dst.UIRect = std::move(src.UIRect);
    dst.FitToContent = std::move(src.FitToContent);
    dst.UISceneCapture = std::move(src.UISceneCapture);
    dst.AudioSource = std::move(src.AudioSource);
    dst.AudioListener = std::move(src.AudioListener);
    dst.AnimationPlayer = std::move(src.AnimationPlayer);
    dst.RenderOverrides = std::move(src.RenderOverrides);
    dst.Scripts = std::move(src.Scripts);
    dst.IKs = std::move(src.IKs);
    dst.LookAtConstraints = std::move(src.LookAtConstraints);

    // Keep mesh->BlendShapes raw pointer in sync after ownership transfer.
    if (dst.Mesh && dst.BlendShapes) {
        dst.Mesh->BlendShapes = dst.BlendShapes.get();
    }
}

static bool TimeExceeded(const std::chrono::high_resolution_clock::time_point& start, double budgetMs) {
    if (budgetMs <= 0.0) return true;
    auto now = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double, std::milli>(now - start).count();
    return elapsed >= budgetMs;
}

// Helper to resolve mesh GUIDs to meshbin paths
static std::string ResolveGuidToMeshPath(const ClaymoreGUID& guid) {
    IAssetResolver* resolver = Assets::GetResolver();
    if (resolver) {
        std::string path = resolver->GetPathForGUID(guid);
        if (!path.empty()) {
            std::filesystem::path p(path);
            std::string ext = p.extension().string();
            if (ext != ".meshbin") {
                p.replace_extension(".meshbin");
            }
            return p.string();
        }
    }
    return "";
}

// Create physics bodies for prefab entities with colliders
// This is essential for runtime prefab instantiation - without it, colliders and rigidbodies
// won't interact with the physics simulation
static void CreatePrefabPhysicsBodies(Scene& scene, const std::vector<EntityID>& entityIds) {
    PrefabScopedTimer timer("Prefab::CreatePhysicsBodies");
    int bodiesCreated = 0;
    
    for (EntityID id : entityIds) {
        EntityData* data = scene.GetEntityData(id);
        if (!data) continue;
        
        // Skip if no collider or if it's a character controller (handled separately)
        if (!data->Collider || data->CharacterController) continue;
        
        // Skip if physics body already exists
        if (data->RigidBody && !data->RigidBody->BodyID.IsInvalid()) continue;
        if (data->StaticBody && !data->StaticBody->BodyID.IsInvalid()) continue;
        
        // Use WORLD scale for shape creation so parent scales are respected
        glm::vec3 wpos, wscale, wskew;
        glm::vec4 wpersp;
        glm::quat wrot;
        glm::decompose(data->Transform.WorldMatrix, wscale, wrot, wpos, wskew, wpersp);
        
        // Build the collider shape with world scale
        data->Collider->BuildShape(
            data->Mesh && data->Mesh->mesh ? data->Mesh->mesh.get() : nullptr,
            glm::abs(wscale)
        );
        
        // Create the physics body
        scene.CreatePhysicsBody(id, data->Transform, *data->Collider);
        bodiesCreated++;
    }
    
    if (bodiesCreated > 0) {
        PREFAB_LOG("Created " << bodiesCreated << " physics bodies");
    }
}

static bool PrefabMeshNeedsUniqueInstance(Scene& scene, const EntityData& data) {
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

// Resolve mesh references for prefab entities
static void ResolvePrefabMeshes(Scene& scene, const std::vector<EntityID>& entityIds) {
    PrefabScopedTimer timer("Prefab::ResolveMeshes");
    int meshesLoaded = 0;
    
    // PERFORMANCE OPTIMIZATION: Collect mesh load requests and pre-warm cache in parallel
    // This parallelizes file I/O across multiple threads while keeping component updates sequential
    struct MeshLoadRequest {
        EntityID entityId;
        std::string meshBinPath;
        uint32_t submeshIndex;
        bool isPrimitive;
        AssetReference::PrimitiveType primType;
        bool canShareCachedMesh = false;
    };
    struct MeshLoadResult {
        std::shared_ptr<Mesh> mesh;
        std::unique_ptr<BlendShapeComponent> blendShapes;
        bool skinned = false;
    };
    std::vector<MeshLoadRequest> loadRequests;
    loadRequests.reserve(entityIds.size());
    std::unordered_map<ClaymoreGUID, std::string, ClaymoreGUIDHasher> meshPathCache;
    meshPathCache.reserve(entityIds.size());
    
    // Phase 1: Collect load requests (sequential - accesses entity data)
    for (EntityID id : entityIds) {
        EntityData* data = scene.GetEntityData(id);
        if (!data || !data->Mesh) continue;
        if (data->Mesh->mesh) continue;  // Skip if mesh already loaded
        
        MeshComponent& mc = *data->Mesh;
        const ClaymoreGUID& guid = mc.meshReference.guid;
        if (guid.high == 0 && guid.low == 0) continue;
        
        MeshLoadRequest req;
        req.entityId = id;
        req.submeshIndex = static_cast<uint32_t>(std::max(0, mc.meshReference.fileID));
        
        if (AssetReference::IsPrimitiveGuid(guid)) {
            req.isPrimitive = true;
            req.primType = AssetReference::PrimitiveTypeFromGuid(guid);
        } else {
            req.isPrimitive = false;
            req.canShareCachedMesh = !PrefabMeshNeedsUniqueInstance(scene, *data);
            auto it = meshPathCache.find(guid);
            if (it == meshPathCache.end()) {
                std::string resolved = ResolveGuidToMeshPath(guid);
                it = meshPathCache.emplace(guid, std::move(resolved)).first;
            }
            req.meshBinPath = it->second;
            if (req.meshBinPath.empty()) continue;
        }
        
        loadRequests.push_back(std::move(req));
    }
    
    // Parallelize mesh loading (I/O + parsing) but keep component mutation sequential.
    // Immutable meshes can share the cache; blendshape/unified-morph cases keep unique instances.
    const bool useParallel = cm::g_JobSystem != nullptr && loadRequests.size() > 1;
    std::vector<MeshLoadResult> loadResults;
    if (useParallel) {
        loadResults.resize(loadRequests.size());
        const size_t chunk = ComputeOptimalChunkSize(loadRequests.size(), Jobs().GetWorkerCount(), 1);
        parallel_for(Jobs(), size_t{0}, loadRequests.size(), chunk,
            [&](size_t start, size_t count) {
                for (size_t i = start; i < start + count; ++i) {
                    const auto& req = loadRequests[i];
                    if (req.isPrimitive) {
                        continue;
                    }
                    if (req.canShareCachedMesh) {
                        auto& dst = loadResults[i];
                        dst.mesh = MeshCache::GetOrLoadMesh(req.meshBinPath, req.submeshIndex, &dst.skinned);
                        continue;
                    }
                    bool skinned = false;
                    auto mesh = MeshBinaryLoader::LoadMesh(req.meshBinPath, req.submeshIndex, &skinned);
                    std::unique_ptr<BlendShapeComponent> blendShapes;
                    if (mesh) {
                        blendShapes = MeshBinaryLoader::LoadBlendShapes(req.meshBinPath, req.submeshIndex);
                    }
                    auto& dst = loadResults[i];
                    dst.mesh = std::move(mesh);
                    dst.blendShapes = std::move(blendShapes);
                    dst.skinned = skinned;
                }
            });
    }
    
    // Phase 2: Load meshes and apply to components (sequential - component updates not thread-safe)
    for (size_t i = 0; i < loadRequests.size(); ++i) {
        const auto& req = loadRequests[i];
        EntityData* data = scene.GetEntityData(req.entityId);
        if (!data || !data->Mesh) continue;
        
        MeshComponent& mc = *data->Mesh;
        
        if (req.isPrimitive) {
            std::shared_ptr<Mesh> primMesh = nullptr;
            switch (req.primType) {
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
            }
            continue;
        }
        
        // Non-primitive mesh. Immutable meshes can use the shared cache; mutable morph targets
        // keep per-instance mesh objects so later dynamic upgrades do not leak across instances.
        bool skinned = false;
        std::shared_ptr<Mesh> mesh;
        std::unique_ptr<BlendShapeComponent> meshBlendShapes;
        
        try {
            if (useParallel) {
                auto& loaded = loadResults[i];
                mesh = loaded.mesh;
                meshBlendShapes = std::move(loaded.blendShapes);
                skinned = loaded.skinned;
            } else if (req.canShareCachedMesh) {
                mesh = MeshCache::GetOrLoadMesh(req.meshBinPath, req.submeshIndex, &skinned);
            } else {
                mesh = MeshBinaryLoader::LoadMesh(req.meshBinPath, req.submeshIndex, &skinned);
                if (mesh) {
                    meshBlendShapes = MeshBinaryLoader::LoadBlendShapes(req.meshBinPath, req.submeshIndex);
                }
            }
            if (mesh) {
                mc.mesh = mesh;
                meshesLoaded++;
                
                // CRITICAL FIX: Load blend shape geometry from mesh file and merge with prefab weights
                // Prefab binary only stores weights, but blend shapes need geometry data (DeltaPos/DeltaNormal)
                // from the mesh file to actually morph the mesh
                if (meshBlendShapes) {
                    // If entity already has blend shapes from prefab (with weights), merge them
                    if (data->BlendShapes && !data->BlendShapes->Shapes.empty()) {
                        // Create a map of prefab weights by name
                        std::unordered_map<std::string, float> prefabWeights;
                        for (const auto& prefabShape : data->BlendShapes->Shapes) {
                            prefabWeights[prefabShape.Name] = prefabShape.Weight;
                        }
                        
                        // Apply prefab weights to mesh-loaded blend shapes (which have geometry)
                        for (auto& meshShape : meshBlendShapes->Shapes) {
                            auto it = prefabWeights.find(meshShape.Name);
                            if (it != prefabWeights.end()) {
                                meshShape.Weight = it->second;
                            }
                        }
                        
                        PREFAB_LOG("Merged " << prefabWeights.size() << " prefab blend shape weights with mesh geometry for entity '" << data->Name << "'");
                    }
                    
                    // Replace prefab blend shapes (weights only) with mesh blend shapes (geometry + weights)
                    data->BlendShapes = std::move(meshBlendShapes);
                    data->BlendShapes->Dirty = true;  // Mark dirty so SkinningSystem processes them
                }
                
                // If mesh is skinned, ensure we have a skinned material
                if (skinned && mc.mesh->HasSkinning()) {
                    if (!data->Skinning) {
                        data->Skinning = std::make_unique<SkinningComponent>();
                    }
                    // Create skinned material if current material isn't skinned
                    if (MaterialNeedsSkinnedVariant(mc.material)) {
                        mc.material = AcquireSkinnedMaterialVariant(scene, mc.material);
                        if (mc.materials.empty()) {
                            mc.materials.push_back(mc.material);
                        } else {
                            mc.materials[0] = mc.material;
                        }
                        if (mc.OwnedMaterialSlots.empty()) {
                            mc.OwnedMaterialSlots.resize(mc.materials.size(), false);
                        } else if (mc.material) {
                            mc.OwnedMaterialSlots[0] =
                                !GetMaterialEquivalenceKey(mc.material.get()).EquivalentSafe;
                        }
                        mc.UniqueMaterial = std::any_of(
                            mc.OwnedMaterialSlots.begin(),
                            mc.OwnedMaterialSlots.end(),
                            [](bool owned) { return owned; });
                    }
                }
            }
        } catch (const std::exception& e) {
            PREFAB_LOG_ERROR("Failed to load mesh: " << e.what());
        }
    }
    
    if (meshesLoaded > 0) {
        PREFAB_LOG("Loaded " << meshesLoaded << " meshes");
    }
    
    // FIX: Ensure Mesh->BlendShapes pointer is linked to entity's BlendShapeComponent
    // This is critical for UnifiedMorph/BlendShape rendering - must be done after mesh loading
    // Also ensure blend shapes are marked dirty so SkinningSystem processes them
    int blendsLinked = 0;
    int meshesUpgraded = 0;
    for (EntityID id : entityIds) {
        EntityData* data = scene.GetEntityData(id);
        if (data && data->Mesh) {
            // Link BlendShapes component if it exists
            if (data->BlendShapes) {
                data->Mesh->BlendShapes = data->BlendShapes.get();
                // CRITICAL: Mark blend shapes dirty so SkinningSystem processes them on first frame
                // This ensures blend shapes with geometry data actually morph the mesh
                data->BlendShapes->Dirty = true;
                blendsLinked++;
            }
            
            // FIX: Upgrade static mesh to dynamic for blend shape support
            // The SkinningSystem requires mesh->Dynamic=true and valid dvbh for blend shapes
            // Check if mesh has BlendShapes OR if it will get BlendShapes from UnifiedMorph
            auto mesh = data->Mesh->mesh;
            bool hasBlendShapes = (data->BlendShapes != nullptr && !data->BlendShapes->Shapes.empty());
            bool needsDynamic = hasBlendShapes;
            
            // Also check if parent has UnifiedMorph (meshes will receive blendshape weights)
            if (!needsDynamic && data->Parent != INVALID_ENTITY_ID) {
                EntityID parentId = data->Parent;
                while (parentId != INVALID_ENTITY_ID) {
                    auto* parentData = scene.GetEntityData(parentId);
                    if (!parentData) break;
                    if (parentData->UnifiedMorph && !parentData->UnifiedMorph->Names.empty()) {
                        needsDynamic = true;
                        break;
                    }
                    parentId = parentData->Parent;
                }
            }
            
            if (mesh && needsDynamic && !mesh->Dynamic && !mesh->Vertices.empty()) {
                // Convert static VB to dynamic VB
                if (bgfx::isValid(mesh->vbh)) {
                    bgfx::destroy(mesh->vbh);
                    mesh->vbh = BGFX_INVALID_HANDLE;
                }
                
                // Create dynamic buffer from CPU vertex data
                uint32_t vbSize = 0;
                const bgfx::Memory* vbMem = nullptr;
                
                if (mesh->SkinnedLayout || mesh->HasSkinning()) {
                    // Rebuild as SkinnedPBRVertex
                    std::vector<SkinnedPBRVertex> vertices(mesh->numVertices);
                    for (uint32_t i = 0; i < mesh->numVertices; i++) {
                        SkinnedPBRVertex& v = vertices[i];
                        v.x = mesh->Vertices[i].x;
                        v.y = mesh->Vertices[i].y;
                        v.z = mesh->Vertices[i].z;
                        v.nx = i < mesh->Normals.size() ? mesh->Normals[i].x : 0.0f;
                        v.ny = i < mesh->Normals.size() ? mesh->Normals[i].y : 1.0f;
                        v.nz = i < mesh->Normals.size() ? mesh->Normals[i].z : 0.0f;
                        v.u = i < mesh->UVs.size() ? mesh->UVs[i].x : 0.0f;
                        v.v = i < mesh->UVs.size() ? mesh->UVs[i].y : 0.0f;
                        if (i < mesh->BoneIndices.size()) {
                            v.i0 = (uint8_t)mesh->BoneIndices[i].x;
                            v.i1 = (uint8_t)mesh->BoneIndices[i].y;
                            v.i2 = (uint8_t)mesh->BoneIndices[i].z;
                            v.i3 = (uint8_t)mesh->BoneIndices[i].w;
                        }
                        if (i < mesh->BoneWeights.size()) {
                            v.w0 = mesh->BoneWeights[i].x;
                            v.w1 = mesh->BoneWeights[i].y;
                            v.w2 = mesh->BoneWeights[i].z;
                            v.w3 = mesh->BoneWeights[i].w;
                        }
                    }
                    vbSize = (uint32_t)(vertices.size() * sizeof(SkinnedPBRVertex));
                    vbMem = bgfx::copy(vertices.data(), vbSize);
                    mesh->dvbh = bgfx::createDynamicVertexBuffer(vbMem, SkinnedPBRVertex::layout);
                } else {
                    // Rebuild as PBRVertex
                    std::vector<PBRVertex> vertices(mesh->numVertices);
                    for (uint32_t i = 0; i < mesh->numVertices; i++) {
                        PBRVertex& v = vertices[i];
                        v.x = mesh->Vertices[i].x;
                        v.y = mesh->Vertices[i].y;
                        v.z = mesh->Vertices[i].z;
                        v.nx = i < mesh->Normals.size() ? mesh->Normals[i].x : 0.0f;
                        v.ny = i < mesh->Normals.size() ? mesh->Normals[i].y : 1.0f;
                        v.nz = i < mesh->Normals.size() ? mesh->Normals[i].z : 0.0f;
                        v.u = i < mesh->UVs.size() ? mesh->UVs[i].x : 0.0f;
                        v.v = i < mesh->UVs.size() ? mesh->UVs[i].y : 0.0f;
                    }
                    vbSize = (uint32_t)(vertices.size() * sizeof(PBRVertex));
                    vbMem = bgfx::copy(vertices.data(), vbSize);
                    mesh->dvbh = bgfx::createDynamicVertexBuffer(vbMem, PBRVertex::layout);
                }
                
                mesh->Dynamic = true;
                meshesUpgraded++;
                
                // Mark blend shapes dirty so they get processed on first frame
                if (data->BlendShapes) {
                    data->BlendShapes->Dirty = true;
                }
                
                // FIX: Ensure material is SkinnedPBRMaterial for skinned meshes with blendshapes
                // Blendshapes require SkinnedPBRMaterial to work correctly
                if (mesh->HasSkinning() && data->Mesh) {
                    MeshComponent& mc = *data->Mesh;
                    if (MaterialNeedsSkinnedVariant(mc.material)) {
                        mc.material = AcquireSkinnedMaterialVariant(scene, mc.material);
                        if (mc.materials.empty()) {
                            mc.materials.push_back(mc.material);
                        } else {
                            mc.materials[0] = mc.material;
                        }
                        if (mc.OwnedMaterialSlots.empty()) {
                            mc.OwnedMaterialSlots.resize(mc.materials.size(), false);
                        } else if (mc.material) {
                            mc.OwnedMaterialSlots[0] =
                                !GetMaterialEquivalenceKey(mc.material.get()).EquivalentSafe;
                        }
                        mc.UniqueMaterial = std::any_of(
                            mc.OwnedMaterialSlots.begin(),
                            mc.OwnedMaterialSlots.end(),
                            [](bool owned) { return owned; });
                    }
                }
            }
        }
    }
    
    if (blendsLinked > 0) {
        PREFAB_LOG("Linked " << blendsLinked << " BlendShapeComponents, upgraded " << meshesUpgraded << " meshes to dynamic for blend shape support");
    }
}

static void ResolvePrefabGuidReferences(
    Scene& scene,
    const std::vector<EntityID>& createdEntityIds,
    const std::unordered_map<uint64_t, EntityID>& guidToEntityId)
{
    auto resolveGuid = [&](uint64_t high, uint64_t low) -> EntityID {
        if (high == 0 && low == 0) return INVALID_ENTITY_ID;
        ClaymoreGUID g{high, low};
        auto it = guidToEntityId.find(PackGuidHash(g));
        return (it != guidToEntityId.end()) ? it->second : INVALID_ENTITY_ID;
    };
    
    for (EntityID id : createdEntityIds) {
        EntityData* data = scene.GetEntityData(id);
        if (!data) continue;
        
        for (auto& lac : data->LookAtConstraints) {
            if (lac.TargetEntityGuidHigh != 0 || lac.TargetEntityGuidLow != 0) {
                EntityID resolved = resolveGuid(lac.TargetEntityGuidHigh, lac.TargetEntityGuidLow);
                if (resolved != INVALID_ENTITY_ID) {
                    lac.TargetEntity = resolved;
                }
            }
        }
        
        for (auto& ik : data->IKs) {
            if (ik.TargetEntityGuidHigh != 0 || ik.TargetEntityGuidLow != 0) {
                EntityID resolved = resolveGuid(ik.TargetEntityGuidHigh, ik.TargetEntityGuidLow);
                if (resolved != INVALID_ENTITY_ID) {
                    ik.TargetEntity = resolved;
                }
            }
            if (ik.PoleEntityGuidHigh != 0 || ik.PoleEntityGuidLow != 0) {
                EntityID resolved = resolveGuid(ik.PoleEntityGuidHigh, ik.PoleEntityGuidLow);
                if (resolved != INVALID_ENTITY_ID) {
                    ik.PoleEntity = resolved;
                }
            }
        }
    }
}

static std::shared_ptr<Material> CreateMaterialFromInlineData(const InlineMaterialData& inl, bool skinned) {
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

    std::string shaderVS;
    std::string shaderFS;
    
    if (inl.materialType == InlineMaterialType::PSX) {
        shaderVS = skinned ? "vs_psx_skinned" : "vs_psx";
        shaderFS = "fs_psx";
    } else {
        shaderVS = skinned ? "vs_pbr_skinned" : "vs_pbr";
        shaderFS = skinned ? "fs_pbr_skinned" : "fs_pbr";
    }
    
    auto program = ShaderManager::Instance().LoadProgram(shaderVS, shaderFS);
    if (!bgfx::isValid(program)) {
        PREFAB_LOG_ERROR("Failed to load shader program: " << shaderVS << " + " << shaderFS);
        return nullptr;
    }
    
    std::shared_ptr<PBRMaterial> mat;
    if (skinned) {
        mat = std::make_shared<SkinnedPBRMaterial>("InlineMaterial", program);
    } else {
        mat = std::make_shared<PBRMaterial>("InlineMaterial", program);
    }
    
    mat->SetMetallic(inl.metallic);
    mat->SetRoughness(inl.roughness);
    mat->SetNormalScale(inl.normalScale);
    mat->SetAmbientOcclusion(inl.aoStrength);
    mat->SetEmissionStrength(inl.emissionStrength);
    mat->SetEmissionColor(inl.emissionColor);
    mat->SetDisplacementScale(inl.displacementScale);
    mat->SetUVTransform(inl.uvScale, inl.uvOffset);
    
    if (inl.tint != glm::vec4(1.0f)) {
        mat->SetUniform("u_ColorTint", inl.tint);
    }
    
    if (inl.materialType == InlineMaterialType::PSX) {
        // Seed the full PSX uniform set first (posterize, shadow params,
        // emission) so per-entity property-block overrides can bind to them.
        MaterialManager::InitializePSXUniformDefaults(*mat, skinned);
        mat->SetUniform("u_psxParams", inl.psxParams);
        mat->SetUniform("u_psxWorld", inl.psxWorld);
        mat->SetUniform("u_toonParams", inl.toonParams);
    }
    
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

static std::shared_ptr<Material> CreateShaderGraphMaterial(const ShaderGraphMaterialData& sg) {
    if (sg.compiledVSName.empty() || sg.compiledFSName.empty()) {
        PREFAB_LOG_ERROR("ShaderGraph material missing compiled shader names");
        return nullptr;
    }
    
    auto program = ShaderManager::Instance().LoadProgram(sg.compiledVSName, sg.compiledFSName);
    if (!bgfx::isValid(program)) {
        PREFAB_LOG_ERROR("Failed to load shader graph program: " << sg.compiledVSName << " + " << sg.compiledFSName);
        return nullptr;
    }
    
    auto mat = std::make_shared<cm::RuntimeShaderGraphMaterial>(sg.name, program);
    mat->SetShaderGraphPath(sg.shaderGraphPath);
    mat->SetUVScale(sg.uvScale);
    mat->SetUVOffset(sg.uvOffset);
    mat->m_StateFlags = sg.stateFlags;
    
    for (const auto& p : sg.parameters) {
        cm::RuntimeMaterialParameter param;
        param.name = p.name;
        param.displayName = p.displayName;
        param.type = static_cast<cm::RuntimeShaderValueType>(p.type);
        param.value = p.value;
        param.texturePath = p.texturePath;
        param.textureSlot = p.textureSlot;
        
        if (param.type == cm::RuntimeShaderValueType::Texture2D && !param.texturePath.empty()) {
            TextureSpecifier spec;
            spec.Path = param.texturePath;
            param.textureHandle = AcquireTextureHandle(spec, TextureColorSpace::Linear);
        }
        
        mat->AddParameter(param);
    }
    
    return mat;
}

// Resolve and load materials from MaterialAssetPaths
// This is needed after model deltas are applied since they store paths but don't load materials
static void ResolvePrefabMaterials(Scene& scene, const std::vector<EntityID>& entityIds) {
    PrefabScopedTimer timer("Prefab::ResolveMaterials");
    int materialsLoaded = 0;
    int materialsFailed = 0;
    int inlineMaterialsCreated = 0;
    
    for (EntityID id : entityIds) {
        EntityData* data = scene.GetEntityData(id);
        if (!data || !data->Mesh) continue;
        
        MeshComponent& mc = *data->Mesh;
        // FIX: Check if mesh HAS skinning data, not just if Skinning component exists
        // Skinning component might be added later, but mesh already has bone data
        bool needsSkinned = data->Skinning != nullptr;
        if (!needsSkinned && mc.mesh && mc.mesh->HasSkinning()) {
            needsSkinned = true;
        }
        
        // Always load materials from MaterialAssetPaths to match authoring DeserializeMesh behavior
        // This ensures materials are properly set up even if model deltas have already set some materials
        if (!mc.MaterialAssetPaths.empty()) {
            if (mc.materials.size() < mc.MaterialAssetPaths.size()) {
                mc.materials.resize(mc.MaterialAssetPaths.size());
            }
            if (mc.OwnedMaterialSlots.size() < mc.MaterialAssetPaths.size()) {
                mc.OwnedMaterialSlots.resize(mc.MaterialAssetPaths.size(), false);
            }
            
            for (size_t i = 0; i < mc.MaterialAssetPaths.size(); ++i) {
                const std::string& matPath = mc.MaterialAssetPaths[i];
                if (matPath.empty()) continue;
                
                std::filesystem::path p(matPath);
                std::string matBinPath = matPath;
                if (p.extension() == ".mat") {
                    p.replace_extension(".matbin");
                    matBinPath = p.string();
                }
                
                // Always load from MaterialAssetPaths to match authoring behavior
                // This ensures materials are properly set up even if they were set from model deltas
                // Use cache for performance - avoids redundant file I/O and shader loading
                auto material = RuntimeMaterialCache::GetOrLoad(matBinPath, needsSkinned);
                if (material) {
                    mc.materials[i] = material;
                    mc.OwnedMaterialSlots[i] = false;
                    
                    if (i == 0) {
                        mc.material = material;
                    }
                    
                    materialsLoaded++;
                } else {
                    materialsFailed++;
                    PREFAB_LOG_ERROR("Failed to load material from path: " << matBinPath << " (entity: " << data->Name << ", slot: " << i << ")");
                }
            }
        }
        
        if (!mc.ShaderGraphMaterials.empty()) {
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
        
        if (!mc.InlineMaterials.empty()) {
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
                }
            }
        }

        mc.UniqueMaterial = std::any_of(
            mc.OwnedMaterialSlots.begin(),
            mc.OwnedMaterialSlots.end(),
            [](bool owned) { return owned; });
        
        if (!mc.material && !mc.materials.empty() && mc.materials[0]) {
            mc.material = mc.materials[0];
        }
    }
    
    if (materialsLoaded > 0 || inlineMaterialsCreated > 0 || materialsFailed > 0) {
        PREFAB_LOG("Loaded " << materialsLoaded << " materials from paths, " << inlineMaterialsCreated << " inline, " << materialsFailed << " failed");
    }
    
    int defaultsApplied = 0;
    for (EntityID id : entityIds) {
        EntityData* data = scene.GetEntityData(id);
        if (!data || !data->Mesh) continue;
        
        MeshComponent& mc = *data->Mesh;
        if (mc.material) continue;
        if (!mc.mesh) continue;
        
        bool needsSkinned = data->Skinning != nullptr;
        mc.material = AcquireSharedDefaultMaterial(scene, needsSkinned);
        
        if (mc.materials.empty()) {
            mc.materials.push_back(mc.material);
        } else if (!mc.materials[0]) {
            mc.materials[0] = mc.material;
        }
        
        defaultsApplied++;
    }
    
    if (defaultsApplied > 0) {
        PREFAB_LOG("Applied " << defaultsApplied << " default materials");
    }
}

// Apply material property block texture overrides from stored paths
// Model deltas and binary loading store texture paths in SlotPropertyBlockTexturePaths,
// but the actual textures need to be loaded and set on SlotPropertyBlocks for rendering
static void ApplyPrefabPropertyBlockOverrides(Scene& scene, const std::vector<EntityID>& entityIds) {
    PrefabScopedTimer timer("Prefab::ApplyPropertyBlocks");
    int texturesLoaded = 0;
    
    for (EntityID id : entityIds) {
        EntityData* data = scene.GetEntityData(id);
        if (!data || !data->Mesh) continue;
        
        MeshComponent& mc = *data->Mesh;
        
        // Ensure property block arrays are sized correctly
        size_t slotCount = std::max(mc.materials.size(), size_t(1));
        if (mc.SlotPropertyBlocks.size() < slotCount) {
            mc.SlotPropertyBlocks.resize(slotCount);
        }
        if (mc.SlotPropertyBlockTexturePaths.size() < slotCount) {
            mc.SlotPropertyBlockTexturePaths.resize(slotCount);
        }
        
        // Load textures from paths and set them on property blocks
        for (size_t slot = 0; slot < mc.SlotPropertyBlockTexturePaths.size(); ++slot) {
            const auto& texturePaths = mc.SlotPropertyBlockTexturePaths[slot];
            if (texturePaths.empty()) continue;
            
            // Ensure slot has a property block
            while (mc.SlotPropertyBlocks.size() <= slot) {
                mc.SlotPropertyBlocks.push_back(MaterialPropertyBlock{});
            }
            
            MaterialPropertyBlock& pb = mc.SlotPropertyBlocks[slot];
            
            for (const auto& [samplerName, texPath] : texturePaths) {
                if (texPath.empty()) continue;
                
                // Check if texture is already loaded in the property block
                // (avoid reloading if already present)
                auto existingIt = pb.Textures.find(samplerName);
                if (existingIt != pb.Textures.end() && bgfx::isValid(existingIt->second)) {
                    continue;
                }
                
                // Load the texture (use cache to avoid redundant I/O)
                TextureSpecifier spec;
                spec.Path = texPath;
                bgfx::TextureHandle tex = AcquireTextureHandle(spec, TextureColorSpace::Linear);
                if (bgfx::isValid(tex)) {
                    pb.SetTexture(samplerName, tex);
                    texturesLoaded++;
                }
            }
        }
        
        // Also handle the legacy single PropertyBlock if it has paths stored
        // (some older prefabs might use this)
        if (!mc.PropertyBlock.Empty()) {
            // The PropertyBlock doesn't store paths, so this is just for consistency
        }
    }
    
    if (texturesLoaded > 0) {
        PREFAB_LOG("Loaded " << texturesLoaded << " property block textures");
    }
}

static bool WarmPrefabAnimationPlayers(Scene& scene, const std::vector<EntityID>& entityIds) {
    bool allReady = true;

    for (EntityID id : entityIds) {
        EntityData* data = scene.GetEntityData(id);
        if (!data || !data->AnimationPlayer) continue;

        auto& player = *data->AnimationPlayer;

        if (!player.ControllerOverride && !player.ControllerOverridePath.empty()) {
            player.ControllerOverride = cm::animation::LoadAnimatorControllerOverrideFromFile(player.ControllerOverridePath);
        }

        if (!player.Controller && !player.ControllerPath.empty()) {
            player.Controller = cm::animation::LoadAnimatorControllerFromFile(player.ControllerPath);
            if (player.Controller) {
                player._AutoControllerGenerated = false;
                player.SyncRuntimeControllerState();
            }
        }

        if (!player.Controller &&
            player.ControllerPath.empty() &&
            !player.SingleClipPath.empty() &&
            !player._AutoControllerGenerated) {
            auto ctrl = std::make_shared<cm::animation::AnimatorController>();
            ctrl->Name = "AutoGenerated";

            cm::animation::AnimatorState state;
            state.Id = 0;
            state.Name = "Default";
            state.AnimationAssetPath = player.SingleClipPath;
            state.Loop = player.ActiveStates.empty() ? true : player.ActiveStates.front().Loop;
            state.Speed = 1.0f;

            ctrl->States.push_back(std::move(state));
            ctrl->DefaultState = 0;

            player.Controller = std::move(ctrl);
            player._AutoControllerGenerated = true;
            player.SyncRuntimeControllerState();
        }

        if (!player.Controller) {
            continue;
        }

        if (!player.PreloadAllControllerAssets(true)) {
            allReady = false;
        }
    }

    return allReady;
}

// Create managed script instances for all scripts in prefab entities
// This must be called before OnValidate so script instances exist to receive serialized values
static void CreatePrefabScriptInstances(Scene& scene, const std::vector<EntityID>& entityIds) {
    PrefabScopedTimer timer("Prefab::CreateScriptInstances");
    int instancesCreated = 0;
    
    for (EntityID id : entityIds) {
        EntityData* data = scene.GetEntityData(id);
        if (!data) continue;
        
        for (auto& script : data->Scripts) {
            if (script.ClassName.empty()) continue;
            
            // Skip if instance already exists
            if (script.Instance) continue;
            
            // Create the script instance
            auto created = ScriptSystem::Instance().Create(script.ClassName);
            if (created) {
                script.Instance = created;
                instancesCreated++;
            } else {
                PREFAB_LOG_ERROR("Failed to create script instance: " << script.ClassName << " for entity " << data->Name);
            }
        }
    }
    
    if (instancesCreated > 0) {
        PREFAB_LOG("Created " << instancesCreated << " script instances");
    }
}

static void LogPrefabScriptRefAuditSnapshot(
    const char* stage,
    Scene& scene,
    const std::vector<EntityID>& prefabEntityIds)
{
    std::unordered_set<EntityID> prefabSet(prefabEntityIds.begin(), prefabEntityIds.end());
    auto isPrefabEntityValue = [&](int value) -> bool {
        return IsValidEntityRefValue(value) &&
               prefabSet.find(static_cast<EntityID>(value)) != prefabSet.end();
    };
    auto isEntityLikeType = [](PropertyType t) -> bool {
        return t == PropertyType::Entity || t == PropertyType::ComponentRef || t == PropertyType::ScriptRef;
    };

    for (EntityID id : prefabEntityIds) {
        auto* data = scene.GetEntityData(id);
        if (!data) continue;
        for (const auto& script : data->Scripts) {
            std::unordered_map<std::string, PropertyType> reflectedTypes;
            std::unordered_map<std::string, PropertyType> reflectedListElemTypes;
            if (ScriptReflection::HasProperties(script.ClassName)) {
                const auto& props = ScriptReflection::GetScriptProperties(script.ClassName);
                reflectedTypes.reserve(props.size());
                reflectedListElemTypes.reserve(props.size());
                for (const auto& p : props) {
                    reflectedTypes[p.name] = p.type;
                    if (p.type == PropertyType::List) {
                        reflectedListElemTypes[p.name] = p.listElementType;
                    }
                }
            }

            for (const auto& [key, value] : script.Values) {
                const ScriptEntityRefMetadata* meta = nullptr;
                auto metaIt = script.EntityRefMetadata.find(key);
                if (metaIt != script.EntityRefMetadata.end()) meta = &metaIt->second;
                const bool hasMetaHints = HasEntityRefHints(meta);
                const auto reflectedTypeIt = reflectedTypes.find(key);
                const bool reflectedEntityLike =
                    (reflectedTypeIt != reflectedTypes.end()) &&
                    isEntityLikeType(reflectedTypeIt->second);

                if (std::holds_alternative<int>(value)) {
                    if (!reflectedEntityLike && !hasMetaHints) continue;
                    int v = std::get<int>(value);
                    if (!hasMetaHints && !IsValidEntityRefValue(v)) continue;
                    if (!hasMetaHints && isPrefabEntityValue(v)) continue; // reduce noise on stable prefab-local refs
                    PREFAB_LOG("[ScriptRefAudit] stage=" << stage
                        << " owner=" << id << "(" << data->Name << ")"
                        << " script=" << script.ClassName
                        << " prop=" << key
                        << " value=" << v << "(" << DescribeEntityRefTarget(scene, v) << ")"
                        << " inPrefab=" << (isPrefabEntityValue(v) ? "true" : "false")
                        << " " << DescribeEntityRefMeta(meta));
                } else if (std::holds_alternative<std::shared_ptr<ListPropertyValue>>(value)) {
                    auto listPtr = std::get<std::shared_ptr<ListPropertyValue>>(value);
                    if (!listPtr) continue;
                    bool reflectedListEntityLike = false;
                    auto reflectedListIt = reflectedListElemTypes.find(key);
                    if (reflectedListIt != reflectedListElemTypes.end()) {
                        reflectedListEntityLike = isEntityLikeType(reflectedListIt->second);
                    }
                    if (!reflectedListEntityLike && !isEntityLikeType(listPtr->elementType)) continue;
                    for (size_t i = 0; i < listPtr->elements.size(); ++i) {
                        if (!std::holds_alternative<int>(listPtr->elements[i])) continue;
                        const ScriptEntityRefMetadata* elemMeta = (i < listPtr->entityRefs.size()) ? &listPtr->entityRefs[i] : nullptr;
                        const bool elemHasMetaHints = HasEntityRefHints(elemMeta);
                        int v = std::get<int>(listPtr->elements[i]);
                        if (!elemHasMetaHints && !IsValidEntityRefValue(v)) continue;
                        if (!elemHasMetaHints && isPrefabEntityValue(v)) continue; // reduce noise on stable prefab-local refs
                        PREFAB_LOG("[ScriptRefAudit] stage=" << stage
                            << " owner=" << id << "(" << data->Name << ")"
                            << " script=" << script.ClassName
                            << " prop=" << key << "[" << i << "]"
                            << " value=" << v << "(" << DescribeEntityRefTarget(scene, v) << ")"
                            << " inPrefab=" << (isPrefabEntityValue(v) ? "true" : "false")
                            << " " << DescribeEntityRefMeta(elemMeta));
                    }
                }
            }
        }
    }
}

// Final safety pass before OnValidate: ensure script entity refs still point into this prefab subtree.
// This guards against late writes (e.g. model-delta script overrides) occurring after initial remap.
static void ResolvePrefabScriptRefsPreValidate(
    Scene& scene,
    EntityID rootId,
    std::vector<EntityID>& prefabEntityIds,
    const std::optional<nlohmann::json>& prefabEntitiesJson,
    const std::unordered_map<ClaymoreGUID, ClaymoreGUID>& prefabToInstanceGuid)
{
    if (rootId == INVALID_ENTITY_ID || prefabEntityIds.empty()) {
        return;
    }

    std::unordered_map<ClaymoreGUID, EntityID> instanceGuidToId;
    instanceGuidToId.reserve(prefabEntityIds.size());
    for (EntityID id : prefabEntityIds) {
        auto* data = scene.GetEntityData(id);
        if (!data) continue;
        if (data->EntityGuid.high != 0 || data->EntityGuid.low != 0) {
            instanceGuidToId[data->EntityGuid] = id;
        }
    }

    nlohmann::json emptyJson = nlohmann::json::array();
    ResolvePrefabScriptEntityReferences(
        scene,
        rootId,
        prefabEntitiesJson ? *prefabEntitiesJson : emptyJson,
        prefabToInstanceGuid,
        instanceGuidToId,
        &prefabEntityIds);

    LogPrefabScriptRefAuditSnapshot("prevalidate-final", scene, prefabEntityIds);
}

static bool HasRenderableBlendShapes(const EntityData* data) {
    return data && data->BlendShapes && !data->BlendShapes->Shapes.empty();
}

static bool IsDescendantOf(Scene& scene, EntityID childId, EntityID ancestorId) {
    if (childId == INVALID_ENTITY_ID || ancestorId == INVALID_ENTITY_ID) {
        return false;
    }

    EntityID cur = childId;
    while (cur != INVALID_ENTITY_ID) {
        if (cur == ancestorId) {
            return true;
        }
        auto* data = scene.GetEntityData(cur);
        if (!data) {
            break;
        }
        cur = data->Parent;
    }
    return false;
}

static bool SkinningReferencesMorphRoot(Scene& scene, const SkinningComponent* skinning, EntityID morphEntity) {
    if (!skinning || morphEntity == INVALID_ENTITY_ID) {
        return false;
    }

    const EntityID roots[] = {
        skinning->SkeletonRoot,
        skinning->ResolvedSkeletonRoot
    };
    for (EntityID root : roots) {
        if (root == INVALID_ENTITY_ID || root == (EntityID)-1) {
            continue;
        }
        if (root == morphEntity || IsDescendantOf(scene, root, morphEntity)) {
            return true;
        }
    }
    return false;
}

static void AppendUniqueUnifiedMorphMember(
    std::vector<EntityID>& members,
    std::unordered_set<EntityID>& seen,
    EntityID meshId)
{
    if (meshId == INVALID_ENTITY_ID || !seen.insert(meshId).second) {
        return;
    }
    members.push_back(meshId);
}

static bool ReconcileUnifiedMorphMembers(
    Scene& scene,
    const std::vector<EntityID>& entityIds,
    const std::unordered_set<EntityID>& prefabEntitySet,
    EntityID morphEntity,
    UnifiedMorphComponent& unifiedMorph)
{
    std::vector<EntityID> reconciled;
    std::unordered_set<EntityID> seen;
    reconciled.reserve(unifiedMorph.MemberMeshes.size());

    for (EntityID candidateId : entityIds) {
        auto* candidate = scene.GetEntityData(candidateId);
        if (!HasRenderableBlendShapes(candidate)) {
            continue;
        }

        const bool belongsToMorph =
            IsDescendantOf(scene, candidateId, morphEntity) ||
            SkinningReferencesMorphRoot(scene, candidate->Skinning.get(), morphEntity);
        if (belongsToMorph) {
            AppendUniqueUnifiedMorphMember(reconciled, seen, candidateId);
        }
    }

    for (EntityID existingId : unifiedMorph.MemberMeshes) {
        auto* existing = scene.GetEntityData(existingId);
        if (!HasRenderableBlendShapes(existing)) {
            continue;
        }

        // Preserve valid explicit members, including runtime-added armor outside
        // this prefab scope, while the scoped pass above fills in missing body meshes.
        if (!seen.count(existingId) || prefabEntitySet.find(existingId) == prefabEntitySet.end()) {
            AppendUniqueUnifiedMorphMember(reconciled, seen, existingId);
        }
    }

    if (reconciled == unifiedMorph.MemberMeshes) {
        return false;
    }

    unifiedMorph.MemberMeshes = std::move(reconciled);
    return true;
}

static bool ReconcileUnifiedMorphNames(Scene& scene, UnifiedMorphComponent& unifiedMorph) {
    bool changed = false;
    if (unifiedMorph.Weights.size() < unifiedMorph.Names.size()) {
        unifiedMorph.Weights.resize(unifiedMorph.Names.size(), 0.0f);
        changed = true;
    }

    std::unordered_set<std::string> knownNames(
        unifiedMorph.Names.begin(),
        unifiedMorph.Names.end());
    for (EntityID meshId : unifiedMorph.MemberMeshes) {
        auto* meshData = scene.GetEntityData(meshId);
        if (!HasRenderableBlendShapes(meshData)) {
            continue;
        }
        for (const auto& shape : meshData->BlendShapes->Shapes) {
            if (shape.Name.empty() || !knownNames.insert(shape.Name).second) {
                continue;
            }
            unifiedMorph.Names.push_back(shape.Name);
            unifiedMorph.Weights.push_back(0.0f);
            changed = true;
        }
    }

    if (changed) {
        unifiedMorph.NameIndexDirty = true;
    }
    return changed;
}

// Reconcile UnifiedMorph member meshes and propagate weights (binary prefabs don't store MemberMeshes).
// Returns list of entities that contain UnifiedMorph components for post-validation propagation.
static std::vector<EntityID> ResolvePrefabUnifiedMorphs(Scene& scene, const std::vector<EntityID>& entityIds) {
    PrefabScopedTimer timer("Prefab::ResolveUnifiedMorphs");
    int reconciled = 0;
    std::vector<EntityID> unifiedMorphEntities;
    const std::unordered_set<EntityID> prefabEntitySet(entityIds.begin(), entityIds.end());
    
    for (EntityID id : entityIds) {
        auto* data = scene.GetEntityData(id);
        if (!data || !data->UnifiedMorph) continue;

        const bool membersChanged =
            ReconcileUnifiedMorphMembers(
                scene,
                entityIds,
                prefabEntitySet,
                id,
                *data->UnifiedMorph);
        const bool namesChanged = ReconcileUnifiedMorphNames(scene, *data->UnifiedMorph);
        if (membersChanged || namesChanged) {
            ++reconciled;
        }
        
        if (!data->PendingUnifiedMorphWeights.empty()) {
            for (const auto& pendingWeight : data->PendingUnifiedMorphWeights) {
                const auto existingName = std::find(
                    data->UnifiedMorph->Names.begin(),
                    data->UnifiedMorph->Names.end(),
                    pendingWeight.first);
                if (existingName == data->UnifiedMorph->Names.end()) {
                    data->UnifiedMorph->Names.push_back(pendingWeight.first);
                    data->UnifiedMorph->Weights.push_back(0.0f);
                    data->UnifiedMorph->NameIndexDirty = true;
                }
            }
            for (size_t i = 0; i < data->UnifiedMorph->Names.size(); ++i) {
                auto it = data->PendingUnifiedMorphWeights.find(data->UnifiedMorph->Names[i]);
                if (it != data->PendingUnifiedMorphWeights.end()) {
                    if (i < data->UnifiedMorph->Weights.size()) {
                        data->UnifiedMorph->Weights[i] = it->second;
                    }
                }
            }
            data->PendingUnifiedMorphWeights.clear();
        }
        
        // FIX CASE 1: Always propagate - meshes are resolved before this function is called
        // ResolvePrefabMeshes() is called before ResolvePrefabUnifiedMorphs() in InstantiateBinaryV2
        scene.PropagateUnifiedMorphWeights(id);
        unifiedMorphEntities.push_back(id);
    }
    
    if (reconciled > 0) {
        PREFAB_LOG("Reconciled " << reconciled << " UnifiedMorph member lists");
    }
    
    return unifiedMorphEntities;
}

// Internal delegate to global RemapPrefabEntityReferences function
static void RemapEntityRefsInternal(Scene& scene, const std::vector<EntityID>& createdEntityIds,
                             const std::unordered_map<EntityID, EntityID>& oldToNewIdMap,
                             const std::unordered_map<ClaymoreGUID, ClaymoreGUID>* prefabToInstanceGuid = nullptr,
                             const std::unordered_map<ClaymoreGUID, EntityID>* instanceGuidToId = nullptr) {
    // Call the global function defined outside the namespace
    ::RemapPrefabEntityReferences(scene, createdEntityIds, oldToNewIdMap, prefabToInstanceGuid, instanceGuidToId);
}

static EntityID InstantiateBinaryV2(const std::vector<uint8_t>& data, const PrefabBinaryHeader& header, Scene& scene,
                                    const std::string& cacheKey, bool allowDeltaCache,
                                    EntityID existingRoot, bool useExistingRoot) {
    PrefabScopedTimer totalTimer("Prefab::InstantiateBinaryV2");
    ReadContext ctx;
    ctx.data = data.data();
    ctx.dataSize = data.size();
    ctx.stringTableOffset = header.stringTableOffset;
    bool isV4 = false;
    bool isV3 = false;
    std::shared_ptr<ParsedPrefabLayout> parsedLayout;
    ClaymoreGUID prefabAssetGuid{};
    {
        parsedLayout =
            (allowDeltaCache && !cacheKey.empty()) ? TryGetParsedPrefabLayoutCache(cacheKey) : nullptr;

        if (!parsedLayout || !IsParsedPrefabLayoutCompatible(*parsedLayout, header)) {
            PrefabScopedTimer timer("Prefab::ReadEntityHeaders");
            auto rebuiltLayout = std::make_shared<ParsedPrefabLayout>();
            std::string parseError;
            if (!BuildParsedPrefabLayout(data, header, *rebuiltLayout, &parseError)) {
                PREFAB_LOG_ERROR(parseError);
                return INVALID_ENTITY_ID;
            }

            if (allowDeltaCache && !cacheKey.empty()) {
                StoreParsedPrefabLayoutCache(cacheKey, rebuiltLayout);
            }
            parsedLayout = std::move(rebuiltLayout);
        }

        isV4 = parsedLayout->isV4;
        isV3 = parsedLayout->isV3;
        prefabAssetGuid = parsedLayout->prefabAssetGuid;
        ctx.stringView = &parsedLayout->strings;
    }
    const std::vector<EntityHeader>& entityHeaders = parsedLayout->entityHeaders;

    std::string prefabPath;
    if ((prefabAssetGuid.high != 0 || prefabAssetGuid.low != 0) && Assets::GetResolver()) {
        prefabPath = Assets::GetResolver()->GetPathForGUID(prefabAssetGuid);
    }
    if (prefabPath.empty()) {
        prefabPath = cacheKey;
    }
    
    std::optional<nlohmann::json> prefabEntitiesJson;
    const bool allowSourceFallback = Assets::AllowSourceFallback();
    if ((prefabAssetGuid.high != 0 || prefabAssetGuid.low != 0) && Assets::GetResolver() && allowSourceFallback) {
        std::string prefabSourcePath = Assets::GetResolver()->GetPathForGUID(prefabAssetGuid);
        if (!prefabSourcePath.empty()) {
            {
                std::lock_guard<std::mutex> lock(s_prefabJsonCacheMutex);
                auto it = s_prefabJsonCache.find(prefabAssetGuid);
                if (it != s_prefabJsonCache.end()) {
                    prefabEntitiesJson = it->second;
                }
            }
            if (!prefabEntitiesJson) {
                PrefabAsset prefabAsset;
                {
                    PrefabScopedTimer timer("Prefab::LoadPrefabJson");
                    if (PrefabIO::LoadPrefabSource(prefabSourcePath, prefabAsset)) {
                        prefabEntitiesJson = prefabAsset.Entities;
                        std::lock_guard<std::mutex> lock(s_prefabJsonCacheMutex);
                        s_prefabJsonCache[prefabAssetGuid] = prefabAsset.Entities;
                    } else {
                        PREFAB_LOG_ERROR("Failed to load prefab JSON: " << prefabSourcePath);
                    }
                }
            }
        } else {
            PREFAB_LOG_WARN("Prefab source path not found for binary header prefab GUID "
                            << prefabAssetGuid.ToString() << "; metadata-only script ref fallback will be used.");
        }
    }
    
    // Create entities and map GUIDs to entity IDs
    std::unordered_map<uint64_t, EntityID> guidToEntityId;
    std::unordered_map<ClaymoreGUID, ClaymoreGUID> prefabToInstanceGuid;
    std::unordered_map<ClaymoreGUID, ClaymoreGUID> instanceToPrefabGuid;
    std::unordered_map<EntityID, EntityID> oldToNewIdMap;   // For entity reference remapping (prefab-local ID -> instance runtime ID)
    // CRITICAL FIX: Map from authoring-time runtime entity IDs (stored in meta.entityId) to prefab-local entity IDs
    // This is needed because script entity references store authoring-time runtime IDs, but remapping uses prefab-local IDs
    std::unordered_map<EntityID, EntityID> authoringRuntimeToPrefabLocalIdMap;
    std::vector<EntityID> createdEntityIds;
    
    prefabToInstanceGuid.reserve(entityHeaders.size());
    instanceToPrefabGuid.reserve(entityHeaders.size());
    guidToEntityId.reserve(entityHeaders.size());
    createdEntityIds.reserve(entityHeaders.size());
    
    const bool fastHierarchy = true;
    {
        PrefabScopedTimer timer("Prefab::CreateEntities+Components");
        for (size_t i = 0; i < entityHeaders.size(); ++i) {
            const auto& eh = entityHeaders[i];
            
            if (eh.componentOffset > data.size()) {
                PREFAB_LOG_ERROR("Entity component offset out of bounds (offset=" << eh.componentOffset
                                 << ", fileSize=" << data.size() << ")");
                return INVALID_ENTITY_ID;
            }
            // Calculate componentTableOffset: where the component data buffer starts in the file
            size_t headerSize = sizeof(PrefabBinaryHeader);
            size_t prefabGuidSize = 16;  // GUID high + low
            size_t prefabNameSize = 4;   // Name index
            size_t entityTableOffset = headerSize + prefabGuidSize + prefabNameSize;
            size_t entityTableSize = header.entityCount * sizeof(EntityHeader);
            size_t componentTableOffset = entityTableOffset + entityTableSize;
            
            ctx.offset = eh.componentOffset;
            std::string name = ctx.ReadString(eh.nameIndex);
            
            // Create temporary EntityData to load components
            auto entityData = std::make_unique<EntityData>();
            entityData->Name = name;
            ClaymoreGUID prefabGuid{eh.guidHigh, eh.guidLow};
            ClaymoreGUID instanceGuid = ClaymoreGUID::Generate();
            entityData->EntityGuid = instanceGuid;
            entityData->PrefabGuid = prefabAssetGuid;
            
            // Apply entity-level properties from header (v4+)
            entityData->Active = (eh.flags & 0x01) != 0;
            entityData->Visible = (eh.flags & 0x02) != 0;
            entityData->Layer = eh.layer;
            entityData->Tag = ctx.ReadString(eh.tagIndex);
            entityData->ModelAssetGuid.high = eh.modelGuidHigh;
            entityData->ModelAssetGuid.low = eh.modelGuidLow;
            
            // Load components
            // CRITICAL UNDERSTANDING:
            // In the writer:
            //   1. header.componentOffset = componentData.size() BEFORE writing components (0 for entity 0)
            //   2. For each component:
            //      - entryPos = componentData.size() (where entry header will be written)
            //      - componentData.resize(entryPos + sizeof(ComponentEntry)) (reserve space)
            //      - entry.dataOffset = componentData.size() (AFTER reserving, so entryPos + 12)
            //      - Write component data (appends to componentData)
            //      - Write entry header at entryPos
            //   3. After all entities: eh.componentOffset += componentTableOffset (makes it absolute file offset)
            //
            // So the layout in componentData buffer is: [Entry0][Data0][Entry1][Data1]...
            // entry.dataOffset is the absolute offset within componentData buffer (12 for first component)
            // eh.componentOffset (after adjustment) = componentTableOffset + original_componentOffset
            // For entity 0: original_componentOffset = 0, so eh.componentOffset = componentTableOffset
            // For entity 1: original_componentOffset = size of entity 0's components
            //
            // When reading:
            // - componentTableOffset is calculated from header sizes
            // - entry.dataOffset is the absolute offset within componentData buffer
            // - componentDataOffset = componentTableOffset + entry.dataOffset (absolute file offset)
            //
            // However, we read entries sequentially, so ctx.offset advances past each entry header.
            // For sequential reading: componentDataOffset should be ctx.offset (after reading entry header)
            // But entry.dataOffset might not match if there's padding or non-sequential storage.
            // So we use entry.dataOffset as the authoritative source.
            
            for (uint32_t c = 0; c < eh.componentCount; ++c) {
                ComponentEntry entry;
                if (ctx.offset > data.size() || data.size() - ctx.offset < sizeof(ComponentEntry)) {
                    PREFAB_LOG_ERROR("Component entry out of bounds (entity=" << i
                                     << ", component=" << c << ", offset=" << ctx.offset << ", fileSize=" << data.size() << ")");
                    return INVALID_ENTITY_ID;
                }
                if (!ctx.Read(entry)) {
                    PREFAB_LOG_ERROR("Failed to read component entry (entity=" << i
                                     << ", component=" << c << ", offset=" << ctx.offset << ")");
                    return INVALID_ENTITY_ID;
                }
                
                // Calculate component data offset using entry.dataOffset (authoritative)
                // entry.dataOffset is absolute within componentData buffer
                // componentDataOffset = componentTableOffset + entry.dataOffset
                size_t componentDataOffset = componentTableOffset + entry.dataOffset;
                
                if (componentDataOffset + entry.dataSize > data.size()) {
                    PREFAB_LOG_ERROR("Component data out of bounds (entity=" << i
                                     << ", component=" << c << ", offset=" << componentDataOffset
                                     << ", size=" << entry.dataSize << ", fileSize=" << data.size() << ")");
                    return INVALID_ENTITY_ID;
                }
                
                // Use shared LoadComponentBinary (THE SINGLE SOURCE OF TRUTH)
                binary::ComponentLoadContext compCtx;
                // Point directly at the component payload using the stored dataOffset
                compCtx.data = ctx.data + componentDataOffset;
                compCtx.size = entry.dataSize;  // Size is the component's data size
                compCtx.pos = 0;
                compCtx.version = header.base.version;  // Pass version for version-specific loading
                compCtx.readString = [&ctx](uint32_t idx) { return ctx.ReadString(idx); };
                
                bool loadSuccess = binary::LoadComponentBinary(compCtx, entityData.get(), entry.typeId, entry.dataSize);
                if (!loadSuccess && entry.typeId == ComponentTypeId::Panel) {
                    PREFAB_LOG_ERROR("Failed to load PanelComponent for entity " << i << " (name=" << name 
                                     << ", dataOffset=" << componentDataOffset << ", dataSize=" << entry.dataSize 
                                     << ", eh.componentOffset=" << eh.componentOffset << ", entry.dataOffset=" << entry.dataOffset << ")");
                }
                
                // Advance to next component entry
                // Since components are stored sequentially [Entry][Data][Entry][Data]...
                // and we read the entry header (advancing ctx.offset past it),
                // then read the data (componentDataOffset to componentDataOffset + entry.dataSize),
                // the next entry header is right after the data
                ctx.offset = componentDataOffset + entry.dataSize;
            }
            
            // Create entity in scene - use CreateEntityExact to preserve exact prefab names
            // This prevents double suffixes like "SkeletonRoot_97_297"
            EntityID entityId = INVALID_ENTITY_ID;
            EntityData* sceneData = nullptr;
            bool reuseRoot = useExistingRoot && existingRoot != INVALID_ENTITY_ID && (i == header.rootEntityIndex);
            if (reuseRoot) {
                entityId = existingRoot;
                sceneData = scene.GetEntityData(entityId);
                if (!sceneData) {
                    reuseRoot = false;
                } else {
                    sceneData->Name = name;
                }
            }
            if (!reuseRoot) {
                Entity entity = fastHierarchy ? scene.CreateEntityExactFast(name) : scene.CreateEntityExact(name);
                entityId = entity.GetID();
                sceneData = scene.GetEntityData(entityId);
            }
            
            if (sceneData) {
                // Copy loaded data to scene entity
                sceneData->EntityGuid = instanceGuid;
                bool preserveTransform = reuseRoot && !IsDefaultTransform(sceneData->Transform);
                if (!preserveTransform) {
                    sceneData->Transform = entityData->Transform;
                }
                sceneData->PrefabGuid = prefabAssetGuid;
                
                // Copy entity-level properties (v4+)
                sceneData->Active = entityData->Active;
                sceneData->Visible = entityData->Visible;
                sceneData->Layer = entityData->Layer;
                sceneData->Tag = entityData->Tag;
                sceneData->ModelAssetGuid = entityData->ModelAssetGuid;
                
                MoveLoadedEntityComponents(*sceneData, *entityData);
              }
              
              if (prefabGuid.high != 0 || prefabGuid.low != 0) {
                  guidToEntityId[PackGuidHash(prefabGuid)] = entityId;
                  prefabToInstanceGuid[prefabGuid] = instanceGuid;
                  instanceToPrefabGuid[instanceGuid] = prefabGuid;
              }
              createdEntityIds.push_back(entityId);
              
              // Build old->new EntityID mapping for entity reference remapping
              EntityID oldEntityId = static_cast<EntityID>(eh.entityId);
              oldToNewIdMap[oldEntityId] = entityId;
              
              // CRITICAL FIX: Build map from authoring-time runtime entity IDs to prefab-local entity IDs
              // Script entity references store authoring-time runtime IDs in meta.entityId, but remapping
              // uses prefab-local IDs. We need to convert authoring-time runtime IDs to prefab-local IDs.
              // We can do this by matching GUIDs: if a script reference has meta.guid matching this entity's GUID,
              // then meta.entityId (authoring-time runtime ID) should map to eh.entityId (prefab-local ID).
              // However, we don't have the authoring-time runtime IDs readily available here.
              // Instead, we'll fix this during remapping by prioritizing GUID-based resolution.
        }
    }
    
    const EntityID prefabRootId =
        header.rootEntityIndex < createdEntityIds.size()
            ? createdEntityIds[header.rootEntityIndex]
            : INVALID_ENTITY_ID;

    // Resolve parent relationships
    {
        PrefabScopedTimer timer("Prefab::ResolveParents");
        for (size_t i = 0; i < entityHeaders.size(); ++i) {
            const auto& eh = entityHeaders[i];
            if (eh.parentGuidHigh == 0 && eh.parentGuidLow == 0) continue;
            
            ClaymoreGUID parentGuid{eh.parentGuidHigh, eh.parentGuidLow};
            auto it = guidToEntityId.find(PackGuidHash(parentGuid));
            if (it != guidToEntityId.end()) {
                EntityID childId = createdEntityIds[i];
                EntityID parentId = it->second;
                if (fastHierarchy) {
                    scene.SetParentFast(childId, parentId);
                } else {
                    scene.SetParent(childId, parentId);
                }
            }
        }
        if (fastHierarchy) {
            scene.InvalidateHierarchyCache();
        }
    }
    
    // Remap entity references in components (IK targets, skeleton roots, etc., plus script fields)
    
    // CRITICAL FIX: Build instanceGuidToId map BEFORE remapping (needed for GUID-based script reference resolution)
    std::unordered_map<ClaymoreGUID, EntityID> instanceGuidToId;
    instanceGuidToId.reserve(createdEntityIds.size());
    for (EntityID createdId : createdEntityIds) {
        if (auto* data = scene.GetEntityData(createdId)) {
            if (data->EntityGuid.high != 0 || data->EntityGuid.low != 0) {
                instanceGuidToId[data->EntityGuid] = createdId;
            }
        }
    }
    
    // CRITICAL FIX: Pass prefabToInstanceGuid and instanceGuidToId to RemapPrefabEntityReferences
    // This enables GUID-based resolution of script entity references (meta.guid contains prefab GUIDs)
    // The prefabGuidToEntityId map built inside RemapPrefabEntityReferences maps prefab GUIDs -> instance runtime IDs
    {
        PrefabScopedTimer timer("Prefab::RemapEntityReferences");
        PrefabPerfStageTimer perfStage(
            scene,
            prefabRootId,
            prefabPath,
            createdEntityIds.size(),
            "PrefabSetup/RemapEntityRefs",
            "RemapEntityRefs");
        ::RemapPrefabEntityReferences(scene, createdEntityIds, oldToNewIdMap, &prefabToInstanceGuid, &instanceGuidToId);
        ResolvePrefabGuidReferences(scene, createdEntityIds, guidToEntityId);
    }
    
    // FIX CASE 4: Always attempt script reference resolution
    // If JSON is available, use it; otherwise the function will use GUID-based metadata fallback
    if (!prefabToInstanceGuid.empty() && !instanceGuidToId.empty() && 
        header.rootEntityIndex < createdEntityIds.size()) {
        PrefabScopedTimer timer("Prefab::ResolveScriptRefs");
        PrefabPerfStageTimer perfStage(
            scene,
            prefabRootId,
            prefabPath,
            createdEntityIds.size(),
            "PrefabSetup/ResolveScriptRefs",
            "ResolveScriptRefs");
        EntityID rootId = createdEntityIds[header.rootEntityIndex];
        nlohmann::json emptyJson = nlohmann::json::array();
        ResolvePrefabScriptEntityReferences(scene, rootId, 
                                            prefabEntitiesJson ? *prefabEntitiesJson : emptyJson,
                                            prefabToInstanceGuid, instanceGuidToId, &createdEntityIds);
        LogPrefabScriptRefAuditSnapshot("post-remap-initial", scene, createdEntityIds);
    }
    
    // PERFORMANCE CRITICAL: Apply model deltas BEFORE any mesh/material work
    // This eliminates the two-phase stall by ensuring deltas are applied first,
    // then we do mesh/material resolution ONCE for all entities (including delta-added ones)
    // Load and apply model deltas (v3+)
    if (isV3 && header.modelDeltaCount > 0 && header.modelDeltaTableOffset > 0) {
        PrefabScopedTimer timer("Prefab::ApplyModelDeltas");
        PrefabPerfStageTimer perfStage(
            scene,
            prefabRootId,
            prefabPath,
            createdEntityIds.size(),
            "PrefabSetup/ApplyModelDeltas",
            "ApplyModelDeltas");
        perfStage.SetExtraDetails("deltas=" + std::to_string(header.modelDeltaCount));
        using namespace cm::model;
        
        auto resolveRoot = [&](uint32_t rootEntityId) {
            EntityID oldModelRoot = static_cast<EntityID>(rootEntityId);
            auto it = oldToNewIdMap.find(oldModelRoot);
            return (it != oldToNewIdMap.end()) ? it->second : oldModelRoot;
        };
        
        std::shared_ptr<std::vector<CachedModelDelta>> cachedDeltas;
        if (allowDeltaCache && !cacheKey.empty()) {
            cachedDeltas = TryGetPrefabModelDeltaCache(cacheKey);
            if (cachedDeltas && cachedDeltas->size() != header.modelDeltaCount) {
                cachedDeltas.reset();
            }
        }
        
        cm::model::ModelDeltaApplicator applicator(scene);
        cm::model::DeltaApplicationConfig config;
        // Prefab binaries already restore full entity/component/script state from the
        // entity stream. Avoid replaying model-delta script/entity overrides, which can
        // reintroduce authoring-time IDs and stale visibility snapshots.
        config.applyScripts = false;
        config.applyEntityOverrides = false;
        config.verbose = false;
        config.allowUnmatchedDeltas = true;
        config.fastHierarchy = true;
        
        if (cachedDeltas) {
            for (const auto& cached : *cachedDeltas) {
                EntityID newModelRoot = resolveRoot(cached.rootEntityId);
                if (!cached.delta.IsEmpty()) {
                    applicator.Apply(newModelRoot, cached.delta, config);
                }
            }
        } else {
            ctx.offset = header.modelDeltaTableOffset;
            auto parsedDeltas = std::make_shared<std::vector<CachedModelDelta>>();
            parsedDeltas->reserve(header.modelDeltaCount);
            bool cacheable = true;
            
            for (uint32_t i = 0; i < header.modelDeltaCount; ++i) {
                ModelDeltaEntry entry;
                if (!ctx.Read(entry)) {
                    cacheable = false;
                    break;
                }
                
                // Read delta JSON blob
                if (entry.dataOffset > 0 && entry.dataSize > 0 &&
                    entry.dataOffset + entry.dataSize <= data.size()) {
                    std::string deltaStr(
                        reinterpret_cast<const char*>(data.data() + entry.dataOffset),
                        entry.dataSize);
                    
                    try {
                        nlohmann::json deltaJson = nlohmann::json::parse(deltaStr);
                        cm::model::ModelDelta delta = cm::model::ModelDelta::FromJson(deltaJson);
                        parsedDeltas->push_back({entry.rootEntityId, delta});
                        
                        if (!delta.IsEmpty()) {
                            EntityID newModelRoot = resolveRoot(entry.rootEntityId);
                            applicator.Apply(newModelRoot, delta, config);
                        }
                    } catch (const std::exception& e) {
                        cacheable = false;
                        PREFAB_LOG_ERROR("Failed to parse model delta: " << e.what());
                    }
                } else {
                    cacheable = false;
                }
            }
            
            if (allowDeltaCache && cacheable && !cacheKey.empty()) {
                StorePrefabModelDeltaCache(cacheKey, parsedDeltas);
            }
        }
        
        // CRITICAL FIX: Re-remap entity references after model deltas are applied
        // Model deltas overwrite script Values with authoring-time entity IDs from delta JSON,
        // so we need to remap them again to runtime IDs
        if (!prefabToInstanceGuid.empty() && !instanceGuidToId.empty() && 
            header.rootEntityIndex < createdEntityIds.size()) {
            PrefabScopedTimer timerRemap("Prefab::ResolveScriptRefsAfterDeltas");
            PrefabPerfStageTimer perfStage(
                scene,
                prefabRootId,
                prefabPath,
                createdEntityIds.size(),
                "PrefabSetup/ResolveScriptRefsAfterDeltas",
                "ResolveScriptRefsAfterDeltas");
            ResolvePrefabGuidReferences(scene, createdEntityIds, guidToEntityId);
            EntityID rootId = createdEntityIds[header.rootEntityIndex];
            nlohmann::json emptyJson = nlohmann::json::array();
            ResolvePrefabScriptEntityReferences(scene, rootId, 
                                                prefabEntitiesJson ? *prefabEntitiesJson : emptyJson,
                                                prefabToInstanceGuid, instanceGuidToId, &createdEntityIds);
            LogPrefabScriptRefAuditSnapshot("post-remap-after-deltas", scene, createdEntityIds);
        }
    }
    
    // Collect all entities in the prefab subtree AFTER model deltas are applied
    // Model deltas can add new entities (added children) that need mesh/material resolution
    std::vector<EntityID> allPrefabEntities;
    if (!createdEntityIds.empty()) {
        std::function<void(EntityID)> collectDescendants = [&](EntityID id) {
            allPrefabEntities.push_back(id);
            EntityData* d = scene.GetEntityData(id);
            if (d) {
                for (EntityID child : d->Children) {
                    collectDescendants(child);
                }
            }
        };
        collectDescendants(createdEntityIds[header.rootEntityIndex]);
    }
    
    // PERFORMANCE CRITICAL: Single-pass mesh/material resolution for ALL entities (base + delta-added)
    // This eliminates duplicate work that was causing the two-phase stall
    {
        PrefabPerfStageTimer perfStage(
            scene,
            prefabRootId,
            prefabPath,
            allPrefabEntities.size(),
            "PrefabSetup/ResolveMeshes",
            "ResolveMeshes");
        ResolvePrefabMeshes(scene, allPrefabEntities);
    }
    
    // Resolve and load materials from MaterialAssetPaths
    // This must happen AFTER model deltas are applied (they may add material paths)
    {
        PrefabPerfStageTimer perfStage(
            scene,
            prefabRootId,
            prefabPath,
            allPrefabEntities.size(),
            "PrefabSetup/ResolveMaterials",
            "ResolveMaterials");
        ResolvePrefabMaterials(scene, allPrefabEntities);
    }
    
    // Run post-processing (skeleton fixup, bone attachments, skinning initialization)
    // Must happen AFTER mesh/material resolution so components are fully set up
    {
        PrefabScopedTimer timer("Prefab::PostProcessEntities");
        PrefabPerfStageTimer perfStage(
            scene,
            prefabRootId,
            prefabPath,
            allPrefabEntities.size(),
            "PrefabSetup/PostProcess",
            "PostProcess");
        cm::PostProcessEntities(scene, allPrefabEntities);
    }
    
    // Update transforms before creating physics bodies (they need world matrices)
    if (!allPrefabEntities.empty()) {
        PrefabScopedTimer timer("Prefab::UpdateTransforms");
        PrefabPerfStageTimer perfStage(
            scene,
            prefabRootId,
            prefabPath,
            allPrefabEntities.size(),
            "PrefabSetup/UpdateTransforms",
            "UpdateTransforms");
        scene.MarkTransformDirty(allPrefabEntities[0]);
        scene.UpdateTransforms();
    }
    
    // Create physics bodies for entities with colliders
    // This must happen AFTER transforms are updated so world matrices are correct
    {
        PrefabPerfStageTimer perfStage(
            scene,
            prefabRootId,
            prefabPath,
            allPrefabEntities.size(),
            "PrefabSetup/CreatePhysicsBodies",
            "CreatePhysicsBodies");
        CreatePrefabPhysicsBodies(scene, allPrefabEntities);
    }

    // FIX CASE 3: Verify materials are loaded before applying property block overrides
    // Log warnings for any entities with MaterialAssetPaths but no loaded materials
    for (EntityID id : allPrefabEntities) {
        EntityData* data = scene.GetEntityData(id);
        if (data && data->Mesh) {
            MeshComponent& mc = *data->Mesh;
            if (!mc.MaterialAssetPaths.empty()) {
                bool anyMaterialMissing = false;
                for (size_t i = 0; i < mc.MaterialAssetPaths.size(); ++i) {
                    if (!mc.MaterialAssetPaths[i].empty() && 
                        (i >= mc.materials.size() || !mc.materials[i])) {
                        anyMaterialMissing = true;
                        break;
                    }
                }
                if (anyMaterialMissing) {
                    PREFAB_LOG_WARN("Materials not fully loaded for entity '" << data->Name << "' before property block overrides");
                }
            }
        }
    }

    // Apply property block texture overrides from stored paths
    // This ensures material overrides (albedo, normal, etc.) are actually loaded and applied
    {
        PrefabPerfStageTimer perfStage(
            scene,
            prefabRootId,
            prefabPath,
            allPrefabEntities.size(),
            "PrefabSetup/ApplyPropertyBlocks",
            "ApplyPropertyBlocks");
        ApplyPrefabPropertyBlockOverrides(scene, allPrefabEntities);
    }

    // PERFORMANCE OPTIMIZATION: Flush all deferred GPU buffer creation
    // This batches all bgfx::createVertexBuffer/createIndexBuffer calls together,
    // avoiding synchronous GPU operations (1-10ms each) during mesh loading
    // Must happen BEFORE mesh upgrade logic which assumes GPU buffers exist
    size_t flushedCount = 0;
    {
        PrefabScopedTimer timer("Prefab::FlushDeferredGPU");
        PrefabPerfStageTimer perfStage(
            scene,
            prefabRootId,
            prefabPath,
            allPrefabEntities.size(),
            "PrefabSetup/FlushDeferredGPU",
            "FlushDeferredGPU");
        flushedCount = DeferredGPU::FlushPendingBuffers();
        perfStage.SetExtraDetails("flushed=" + std::to_string(flushedCount));
    }
    if (flushedCount > 0) {
        PREFAB_LOG("Flushed " << flushedCount << " deferred GPU buffers");
    }

    // Ensure navmesh runtime data is ready for navigation users
    {
        PrefabScopedTimer timer("Prefab::EnsureNavmeshRuntime");
        PrefabPerfStageTimer perfStage(
            scene,
            prefabRootId,
            prefabPath,
            allPrefabEntities.size(),
            "PrefabSetup/EnsureNavmeshRuntime",
            "EnsureNavmeshRuntime");
        for (EntityID id : allPrefabEntities) {
            EntityData* d = scene.GetEntityData(id);
            if (d && d->Navigation) {
                if (!d->Navigation->Runtime && !d->Navigation->EnsureRuntimeLoaded()) {
                    PREFAB_LOG_ERROR("Failed to load navmesh runtime for entity '" << d->Name << "'");
                }
            }
        }
    }
    
    // FIX CASE 2: Trigger auto-binding immediately for NavAgents with NavMeshEntity=0
    // This ensures agents are ready for navigation on the first update tick
    // Search the ENTIRE SCENE for navmeshes, not just prefab entities (navmesh may be external)
    {
        PrefabScopedTimer timer("Prefab::AutoBindNavAgents");
        PrefabPerfStageTimer perfStage(
            scene,
            prefabRootId,
            prefabPath,
            allPrefabEntities.size(),
            "PrefabSetup/AutoBindNavAgents",
            "AutoBindNavAgents");
        AutoBindPrefabNavAgents(scene, allPrefabEntities, true);
    }

    // FIX: Upgrade meshes to dynamic for blendshapes AFTER all components loaded
    // This ensures meshes that receive blendshape weights from UnifiedMorph get dynamic buffers
    // Also ensures materials are SkinnedPBRMaterial for skinned meshes with blendshapes
    {
        PrefabScopedTimer timer("Prefab::UpgradeMeshesForBlendshapes");
        PrefabPerfStageTimer perfStage(
            scene,
            prefabRootId,
            prefabPath,
            allPrefabEntities.size(),
            "PrefabSetup/UpgradeMeshesForBlendshapes",
            "UpgradeMeshesForBlendshapes");
        for (EntityID id : allPrefabEntities) {
            EntityData* data = scene.GetEntityData(id);
            if (!data || !data->Mesh || !data->Mesh->mesh) continue;
            
            auto mesh = data->Mesh->mesh;
            MeshComponent& mc = *data->Mesh;
            
            // Check if mesh needs dynamic buffer for blendshapes
            bool hasBlendShapes = (data->BlendShapes != nullptr && !data->BlendShapes->Shapes.empty());
            bool needsDynamic = hasBlendShapes;
            
            // Also check if parent has UnifiedMorph (meshes will receive blendshape weights)
            if (!needsDynamic && data->Parent != INVALID_ENTITY_ID) {
                EntityID parentId = data->Parent;
                while (parentId != INVALID_ENTITY_ID) {
                    auto* parentData = scene.GetEntityData(parentId);
                    if (!parentData) break;
                    if (parentData->UnifiedMorph && !parentData->UnifiedMorph->Names.empty()) {
                        needsDynamic = true;
                        break;
                    }
                    parentId = parentData->Parent;
                }
            }
            
            // Upgrade to dynamic if needed
            if (needsDynamic && !mesh->Dynamic && !mesh->Vertices.empty()) {
            // Convert static VB to dynamic VB (same logic as ResolvePrefabMeshes)
            if (bgfx::isValid(mesh->vbh)) {
                bgfx::destroy(mesh->vbh);
                mesh->vbh = BGFX_INVALID_HANDLE;
            }
            
            uint32_t vbSize = 0;
            const bgfx::Memory* vbMem = nullptr;
            
            if (mesh->SkinnedLayout || mesh->HasSkinning()) {
                std::vector<SkinnedPBRVertex> vertices(mesh->numVertices);
                for (uint32_t i = 0; i < mesh->numVertices; i++) {
                    SkinnedPBRVertex& v = vertices[i];
                    v.x = mesh->Vertices[i].x;
                    v.y = mesh->Vertices[i].y;
                    v.z = mesh->Vertices[i].z;
                    v.nx = i < mesh->Normals.size() ? mesh->Normals[i].x : 0.0f;
                    v.ny = i < mesh->Normals.size() ? mesh->Normals[i].y : 1.0f;
                    v.nz = i < mesh->Normals.size() ? mesh->Normals[i].z : 0.0f;
                    v.u = i < mesh->UVs.size() ? mesh->UVs[i].x : 0.0f;
                    v.v = i < mesh->UVs.size() ? mesh->UVs[i].y : 0.0f;
                    if (i < mesh->BoneIndices.size()) {
                        v.i0 = (uint8_t)mesh->BoneIndices[i].x;
                        v.i1 = (uint8_t)mesh->BoneIndices[i].y;
                        v.i2 = (uint8_t)mesh->BoneIndices[i].z;
                        v.i3 = (uint8_t)mesh->BoneIndices[i].w;
                    }
                    if (i < mesh->BoneWeights.size()) {
                        v.w0 = mesh->BoneWeights[i].x;
                        v.w1 = mesh->BoneWeights[i].y;
                        v.w2 = mesh->BoneWeights[i].z;
                        v.w3 = mesh->BoneWeights[i].w;
                    }
                }
                vbSize = (uint32_t)(vertices.size() * sizeof(SkinnedPBRVertex));
                vbMem = bgfx::copy(vertices.data(), vbSize);
                mesh->dvbh = bgfx::createDynamicVertexBuffer(vbMem, SkinnedPBRVertex::layout);
            } else {
                std::vector<PBRVertex> vertices(mesh->numVertices);
                for (uint32_t i = 0; i < mesh->numVertices; i++) {
                    PBRVertex& v = vertices[i];
                    v.x = mesh->Vertices[i].x;
                    v.y = mesh->Vertices[i].y;
                    v.z = mesh->Vertices[i].z;
                    v.nx = i < mesh->Normals.size() ? mesh->Normals[i].x : 0.0f;
                    v.ny = i < mesh->Normals.size() ? mesh->Normals[i].y : 1.0f;
                    v.nz = i < mesh->Normals.size() ? mesh->Normals[i].z : 0.0f;
                    v.u = i < mesh->UVs.size() ? mesh->UVs[i].x : 0.0f;
                    v.v = i < mesh->UVs.size() ? mesh->UVs[i].y : 0.0f;
                }
                vbSize = (uint32_t)(vertices.size() * sizeof(PBRVertex));
                vbMem = bgfx::copy(vertices.data(), vbSize);
                mesh->dvbh = bgfx::createDynamicVertexBuffer(vbMem, PBRVertex::layout);
            }
            
            mesh->Dynamic = true;
            
            PREFAB_LOG("Upgraded mesh to dynamic for blendshapes: entity '" << data->Name << "' (hasBlendShapes=" << hasBlendShapes << ", hasSkinning=" << mesh->HasSkinning() << ")");
            
            // Mark blend shapes dirty
            if (data->BlendShapes) {
                data->BlendShapes->Dirty = true;
                PREFAB_LOG("Marked BlendShapes dirty for entity '" << data->Name << "' (" << data->BlendShapes->Shapes.size() << " shapes)");
            }
            
            // CRITICAL: Ensure material is SkinnedPBRMaterial for skinned meshes with blendshapes
            // Blendshapes require SkinnedPBRMaterial to work correctly
            if (mesh->HasSkinning()) {
                if (!mc.material) {
                    // No material - create SkinnedPBRMaterial
                    mc.material = AcquireSharedDefaultMaterial(scene, true);
                    if (mc.materials.empty()) {
                        mc.materials.push_back(mc.material);
                    } else {
                        mc.materials[0] = mc.material;
                    }
                    if (mc.OwnedMaterialSlots.empty()) {
                        mc.OwnedMaterialSlots.resize(mc.materials.size(), false);
                    } else if (!mc.OwnedMaterialSlots.empty()) {
                        mc.OwnedMaterialSlots[0] = false;
                    }
                    mc.UniqueMaterial = std::any_of(
                        mc.OwnedMaterialSlots.begin(),
                        mc.OwnedMaterialSlots.end(),
                        [](bool owned) { return owned; });
                    PREFAB_LOG("Created SkinnedPBRMaterial for entity '" << data->Name << "' (skinned mesh with blendshapes)");
                } else if (MaterialNeedsSkinnedVariant(mc.material)) {
                    // Wrong material type - upgrade to SkinnedPBRMaterial
                    auto oldMat = mc.material;
                    mc.material = AcquireSkinnedMaterialVariant(scene, mc.material);
                    if (mc.materials.empty()) {
                        mc.materials.push_back(mc.material);
                    } else {
                        mc.materials[0] = mc.material;
                    }
                    if (mc.OwnedMaterialSlots.empty()) {
                        mc.OwnedMaterialSlots.resize(mc.materials.size(), false);
                    } else if (mc.material) {
                        mc.OwnedMaterialSlots[0] =
                            !GetMaterialEquivalenceKey(mc.material.get()).EquivalentSafe;
                    }
                    mc.UniqueMaterial = std::any_of(
                        mc.OwnedMaterialSlots.begin(),
                        mc.OwnedMaterialSlots.end(),
                        [](bool owned) { return owned; });
                    PREFAB_LOG("Upgraded material to SkinnedPBRMaterial for entity '" << data->Name << "' (was: " << oldMat->GetName() << ")");
                }
            }
         } else if (needsDynamic && mesh->Dynamic) {
            // Mesh already dynamic - just verify material is correct
            if (mesh->HasSkinning() && mc.material && 
                MaterialNeedsSkinnedVariant(mc.material)) {
                auto oldMat = mc.material;
                mc.material = AcquireSkinnedMaterialVariant(scene, mc.material);
                if (mc.materials.empty()) {
                    mc.materials.push_back(mc.material);
                } else {
                    mc.materials[0] = mc.material;
                }
                if (mc.OwnedMaterialSlots.empty()) {
                    mc.OwnedMaterialSlots.resize(mc.materials.size(), false);
                } else if (mc.material) {
                    mc.OwnedMaterialSlots[0] =
                        !GetMaterialEquivalenceKey(mc.material.get()).EquivalentSafe;
                }
                mc.UniqueMaterial = std::any_of(
                    mc.OwnedMaterialSlots.begin(),
                    mc.OwnedMaterialSlots.end(),
                    [](bool owned) { return owned; });
                PREFAB_LOG("Fixed material type for entity '" << data->Name << "' (dynamic skinned mesh needs SkinnedPBRMaterial)");
            }
         }
        }
    }
    
    // Rebuild UnifiedMorph member meshes and propagate weights for prefab instances
    std::vector<EntityID> unifiedMorphEntities;
    {
        PrefabPerfStageTimer perfStage(
            scene,
            prefabRootId,
            prefabPath,
            allPrefabEntities.size(),
            "PrefabSetup/ResolveUnifiedMorphs",
            "ResolveUnifiedMorphs");
        unifiedMorphEntities = ResolvePrefabUnifiedMorphs(scene, allPrefabEntities);
    }
    
    // Return root entity ID and attach PrefabInstanceComponent
    if (!createdEntityIds.empty()) {
        EntityID rootId = createdEntityIds[header.rootEntityIndex];
        
        // Attach PrefabInstanceComponent to root for prefab system integration
        EntityData* rootData = scene.GetEntityData(rootId);
        if (rootData) {
              rootData->PrefabInstance = std::make_unique<PrefabInstanceComponent>();
              rootData->PrefabInstance->PrefabAssetGuid = prefabAssetGuid;
              rootData->PrefabInstance->PrefabToInstanceGuid = std::move(prefabToInstanceGuid);
              rootData->PrefabInstance->InstanceToPrefabGuid = std::move(instanceToPrefabGuid);
              
              // Track all created entity GUIDs as owned by this instance
              // Use allPrefabEntities to include model delta-created entities
              for (EntityID id : allPrefabEntities) {
                  EntityData* d = scene.GetEntityData(id);
                  if (d) {
                      d->PrefabGuid = prefabAssetGuid;
                      rootData->PrefabInstance->OwnedEntityGuids.push_back(d->EntityGuid);
                  }
              }
            
            {
                PrefabScopedTimer timer("Prefab::UpdateTransformsAfterAttach");
                PrefabPerfStageTimer perfStage(
                    scene,
                    prefabRootId,
                    prefabPath,
                    allPrefabEntities.size(),
                    "PrefabSetup/UpdateTransformsAfterAttach",
                    "UpdateTransformsAfterAttach");
                scene.MarkTransformDirty(rootId);
                scene.UpdateTransforms();
            }

            // Only propagate explicit entity-level hidden state. Component-level UI visibility
            // (e.g. Panel.Visible) should not collapse an entire entity subtree.
            auto isEntityHidden = [](EntityData* d) -> bool {
                return d && !d->Visible;
            };
            // Propagate visibility for each hidden entity to ensure its children are also hidden
            for (EntityID id : allPrefabEntities) {
                auto* data = scene.GetEntityData(id);
                if (isEntityHidden(data)) {
                    scene.SetEntityVisible(id, false);
                }
            }
            
            // Create managed script instances for all scripts in the prefab
            // This must happen before OnValidate so there's an instance to apply values to
            // Use allPrefabEntities to include model delta-created entities
            if (IsDotnetRuntimeReady()) {
                {
                    PrefabPerfStageTimer perfStage(
                        scene,
                        prefabRootId,
                        prefabPath,
                        allPrefabEntities.size(),
                        "PrefabSetup/CreateScriptInstances",
                        "CreateScriptInstances");
                    CreatePrefabScriptInstances(scene, allPrefabEntities);
                }

                // CRITICAL FIX: Two-phase script initialization (matches Scene::RuntimeClone)
                // Phase 1: Call OnBind on all scripts to register them in ScriptRegistry
                // This enables GetScript<T>() to work during OnCreate for cross-script references
                {
                    PrefabScopedTimer timer("Prefab::ScriptsOnBind");
                    PrefabPerfStageTimer perfStage(
                        scene,
                        prefabRootId,
                        prefabPath,
                        allPrefabEntities.size(),
                        "PrefabSetup/ScriptsOnBind",
                        "ScriptsOnBind");
                    for (EntityID id : allPrefabEntities) {
                        auto* data = scene.GetEntityData(id);
                        if (!data) continue;
                        for (auto& script : data->Scripts) {
                            if (script.Instance) {
                                Entity entity(id, &scene);
                                script.Instance->OnBind(entity);
                            }
                        }
                    }
                }

                // Final re-resolve right before OnValidate to ensure refs remain prefab-local.
                {
                    PrefabScopedTimer timer("Prefab::ResolveScriptRefsPreValidate");
                    PrefabPerfStageTimer perfStage(
                        scene,
                        prefabRootId,
                        prefabPath,
                        allPrefabEntities.size(),
                        "PrefabSetup/ResolveScriptRefsPreValidate",
                        "ResolveScriptRefsPreValidate");
                    if (rootData && rootData->PrefabInstance) {
                        ResolvePrefabScriptRefsPreValidate(
                            scene,
                            rootId,
                            allPrefabEntities,
                            prefabEntitiesJson,
                            rootData->PrefabInstance->PrefabToInstanceGuid);
                    }
                }

                // Now call OnValidate to push serialized values to the managed side
                {
                    PrefabScopedTimer timer("Prefab::ScriptsOnValidate");
                    CallOnValidateForSubtree(scene, rootId);
                }

                // Phase 2: Call OnCreate on all scripts in sorted order ([Priority] then stable tie-breakers)
                // Collect flat list, sort globally, then queue so OnCreate order is deterministic
                {
                    PrefabScopedTimer timer("Prefab::ScriptsOnCreateQueue");
                    PrefabPerfStageTimer perfStage(
                        scene,
                        prefabRootId,
                        prefabPath,
                        allPrefabEntities.size(),
                        "PrefabSetup/ScriptsOnCreateQueue",
                        "ScriptsOnCreateQueue");
                    struct ScriptInitEntry { Scene* scene; EntityID entityId; size_t scriptIndex; int priority; uint64_t scriptTypeStableId; };
                    std::vector<ScriptInitEntry> toCreate;
                    for (EntityID id : allPrefabEntities) {
                        EntityData* data = scene.GetEntityData(id);
                        if (!data) continue;
                        for (size_t i = 0; i < data->Scripts.size(); ++i) {
                            if (data->Scripts[i].Instance) {
                                const std::string& className = data->Scripts[i].ClassName;
                                toCreate.push_back({ &scene, id, i, ScriptSystem::Instance().GetScriptPriority(className), ScriptOrder::StableHashScriptClassName(className) });
                            }
                        }
                    }
                    std::sort(toCreate.begin(), toCreate.end(), [](const ScriptInitEntry& a, const ScriptInitEntry& b) {
                        return ScriptOrder::OrderLess(a.priority, a.scriptTypeStableId, a.entityId, a.scriptIndex, b.priority, b.scriptTypeStableId, b.entityId, b.scriptIndex);
                    });
                    for (const auto& e : toCreate) {
                        DeferredScriptInit::QueueScriptOnCreate(e.scene, e.entityId, e.scriptIndex);
                    }
                }

                // Re-propagate unified morph weights after managed fields are applied
                {
                    PrefabScopedTimer timer("Prefab::UnifiedMorphPropagate");
                    PrefabPerfStageTimer perfStage(
                        scene,
                        prefabRootId,
                        prefabPath,
                        allPrefabEntities.size(),
                        "PrefabSetup/UnifiedMorphPropagate",
                        "UnifiedMorphPropagate");
                    for (EntityID unifiedId : unifiedMorphEntities) {
                        scene.PropagateUnifiedMorphWeights(unifiedId);
                    }
                }
                
                // CRITICAL FIX: Ensure all blend shapes are linked and marked dirty after UnifiedMorph propagation
                // This ensures blend shapes with geometry data actually morph the mesh at runtime
                {
                    PrefabScopedTimer timer("Prefab::FinalizeBlendShapes");
                    PrefabPerfStageTimer perfStage(
                        scene,
                        prefabRootId,
                        prefabPath,
                        allPrefabEntities.size(),
                        "PrefabSetup/FinalizeBlendShapes",
                        "FinalizeBlendShapes");
                    for (EntityID id : allPrefabEntities) {
                        EntityData* data = scene.GetEntityData(id);
                        if (data && data->Mesh && data->BlendShapes) {
                            // Ensure link is set
                            if (data->Mesh->BlendShapes != data->BlendShapes.get()) {
                                data->Mesh->BlendShapes = data->BlendShapes.get();
                            }
                            // Mark dirty so SkinningSystem processes them
                            data->BlendShapes->Dirty = true;
                        }
                    }
                }
            }
        }
        
        return rootId;
    }
    return static_cast<EntityID>(-1);
}

static void StartAsyncLoad(const std::shared_ptr<AsyncPrefabRequest>& req) {
    if (!req || req->prefabPath.empty()) {
        return;
    }
    if (req->cancelled.load(std::memory_order_acquire)) {
        return;
    }
    
    const bool useCache = req->allowCache;
    if (useCache && !req->cacheKey.empty()) {
        if (auto cached = TryGetPrefabBinaryCache(req->cacheKey)) {
            req->data = cached;
            req->dataReady.store(true, std::memory_order_release);
            return;
        }
    }
    
    auto loadJob = [req, useCache]() {
        if (req->cancelled.load(std::memory_order_acquire)) {
            return;
        }
        std::vector<uint8_t> localData;
        bool readOk = FileSystem::Instance().ReadFile(req->prefabPath, localData);
        if (!readOk) {
            {
                std::lock_guard<std::mutex> lock(req->loadMutex);
                req->loadError = "Failed to read prefab: " + req->prefabPath;
            }
            req->loadFailed.store(true, std::memory_order_release);
            return;
        }
        if (req->cancelled.load(std::memory_order_acquire)) {
            return;
        }
        if (useCache && !req->cacheKey.empty()) {
            auto sharedData = std::make_shared<std::vector<uint8_t>>(std::move(localData));
            StorePrefabBinaryCache(req->cacheKey, sharedData);
            req->data = std::move(sharedData);
        } else {
            req->data = std::make_shared<std::vector<uint8_t>>(std::move(localData));
        }
        req->dataReady.store(true, std::memory_order_release);
    };
    
    if (cm::g_JobSystem) {
        if (!Jobs().Enqueue(loadJob, JobSystem::Priority::Low)) {
            loadJob();
        }
    } else {
        loadJob();
    }
}

static void EnsurePrefabJsonLoaded(AsyncPrefabRequest& req) {
    if (req.prefabEntitiesJson.has_value()) return;
    const bool allowSourceFallback = Assets::AllowSourceFallback();
    if (!(req.prefabAssetGuid.high != 0 || req.prefabAssetGuid.low != 0) || !Assets::GetResolver() || !allowSourceFallback) {
        return;
    }
    
    std::string prefabSourcePath = Assets::GetResolver()->GetPathForGUID(req.prefabAssetGuid);
    if (prefabSourcePath.empty()) return;
    
    {
        std::lock_guard<std::mutex> lock(s_prefabJsonCacheMutex);
        auto it = s_prefabJsonCache.find(req.prefabAssetGuid);
        if (it != s_prefabJsonCache.end()) {
            req.prefabEntitiesJson = it->second;
            return;
        }
    }
    
    PrefabAsset prefabAsset;
    if (PrefabIO::LoadPrefabSource(prefabSourcePath, prefabAsset)) {
        req.prefabEntitiesJson = prefabAsset.Entities;
        std::lock_guard<std::mutex> lock(s_prefabJsonCacheMutex);
        s_prefabJsonCache[req.prefabAssetGuid] = prefabAsset.Entities;
    } else {
        PREFAB_LOG_ERROR("Failed to load prefab JSON: " << prefabSourcePath);
    }
}

static bool StepAsyncPrefab(AsyncPrefabRequest& req, double budgetMs) {
    if (req.phase == AsyncPrefabPhase::Done || req.phase == AsyncPrefabPhase::Failed) {
        return true;
    }
    if (req.cancelled.load(std::memory_order_acquire)) {
        req.loadError = "Async prefab request cancelled";
        req.phase = AsyncPrefabPhase::Failed;
        return true;
    }
    if (!req.scene) {
        req.loadError = "Async prefab request has no scene";
        req.phase = AsyncPrefabPhase::Failed;
        return true;
    }
    
    auto start = std::chrono::high_resolution_clock::now();
    auto asyncPrefabRootId = [&req]() -> EntityID {
        return req.header.rootEntityIndex < req.createdEntityIds.size()
            ? req.createdEntityIds[req.header.rootEntityIndex]
            : INVALID_ENTITY_ID;
    };
    
    if (req.phase == AsyncPrefabPhase::WaitingForData) {
        if (req.loadFailed.load(std::memory_order_acquire)) {
            std::string err;
            {
                std::lock_guard<std::mutex> lock(req.loadMutex);
                err = req.loadError;
            }
            PREFAB_LOG_ERROR((err.empty() ? "Failed to read prefab" : err));
            req.phase = AsyncPrefabPhase::Failed;
            return true;
        }
        if (!req.dataReady.load(std::memory_order_acquire)) {
            return false;
        }
        req.phase = AsyncPrefabPhase::ParseHeaders;
    }
    
    while (!TimeExceeded(start, budgetMs)) {
        if (req.cancelled.load(std::memory_order_acquire)) {
            req.loadError = "Async prefab request cancelled";
            req.phase = AsyncPrefabPhase::Failed;
            return true;
        }
        switch (req.phase) {
            case AsyncPrefabPhase::ParseHeaders: {
                if (!req.data || req.data->size() < sizeof(PrefabBinaryHeader)) {
                    req.loadError = "Prefab data too small";
                    req.phase = AsyncPrefabPhase::Failed;
                    return true;
                }
                const auto& data = *req.data;
                std::memcpy(&req.header, data.data(), sizeof(PrefabBinaryHeader));
                if (req.header.base.magic != PREFAB_MAGIC) {
                    req.loadError = "Invalid prefab magic";
                    req.phase = AsyncPrefabPhase::Failed;
                    return true;
                }
                
                req.isLegacy = (req.header.base.flags & 1) != 0;
                if (req.isLegacy) {
                    req.loadError = "Legacy prefab format not supported for async instantiate";
                    req.phase = AsyncPrefabPhase::Failed;
                    return true;
                }
                
                req.isV4 = (req.header.base.version >= 4);
                req.isV3 = (req.header.base.version >= 3);
                
                req.ctx.data = data.data();
                req.ctx.dataSize = data.size();
                req.ctx.stringTableOffset = req.header.stringTableOffset;
                std::shared_ptr<ParsedPrefabLayout> parsedLayout =
                    (req.allowCache && !req.cacheKey.empty()) ? TryGetParsedPrefabLayoutCache(req.cacheKey) : nullptr;

                if (!parsedLayout || !IsParsedPrefabLayoutCompatible(*parsedLayout, req.header)) {
                    auto rebuiltLayout = std::make_shared<ParsedPrefabLayout>();
                    if (!BuildParsedPrefabLayout(data, req.header, *rebuiltLayout, &req.loadError)) {
                        req.phase = AsyncPrefabPhase::Failed;
                        return true;
                    }

                    if (req.allowCache && !req.cacheKey.empty()) {
                        StoreParsedPrefabLayoutCache(req.cacheKey, rebuiltLayout);
                    }
                    parsedLayout = std::move(rebuiltLayout);
                }

                req.isV4 = parsedLayout->isV4;
                req.isV3 = parsedLayout->isV3;
                req.entityHeaderSize = req.isV4 ? sizeof(EntityHeader) : (req.isV3 ? sizeof(EntityHeaderV3) : sizeof(EntityHeaderV2));
                req.parsedLayout = std::move(parsedLayout);
                req.ctx.stringView = &req.parsedLayout->strings;
                req.entityHeaders = &req.parsedLayout->entityHeaders;
                req.prefabAssetGuid = req.parsedLayout->prefabAssetGuid;

                const size_t entityCount = req.entityHeaders ? req.entityHeaders->size() : 0;
                req.prefabToInstanceGuid.reserve(entityCount);
                req.instanceToPrefabGuid.reserve(entityCount);
                req.guidToEntityId.reserve(entityCount);
                req.createdEntityIds.reserve(entityCount);
                req.oldToNewIdMap.reserve(entityCount);
                
                req.phase = AsyncPrefabPhase::CreateEntities;
                break;
            }
            case AsyncPrefabPhase::CreateEntities: {
                const bool fastHierarchy = true;
                const auto& data = *req.data;
                if (!req.entityHeaders) {
                    req.loadError = "Async prefab request missing parsed entity headers";
                    req.phase = AsyncPrefabPhase::Failed;
                    return true;
                }
                for (; req.entityIndex < req.entityHeaders->size(); ++req.entityIndex) {
                    if (TimeExceeded(start, budgetMs)) {
                        return false;
                    }
                    const auto& eh = (*req.entityHeaders)[req.entityIndex];
                    if (eh.componentOffset > data.size()) {
                        req.loadError = "Entity component offset out of bounds";
                        req.phase = AsyncPrefabPhase::Failed;
                        return true;
                    }
                    req.ctx.offset = eh.componentOffset;
                    std::string name = req.ctx.ReadString(eh.nameIndex);
                    
                    auto entityData = std::make_unique<EntityData>();
                    entityData->Name = name;
                    ClaymoreGUID prefabGuid{eh.guidHigh, eh.guidLow};
                    ClaymoreGUID instanceGuid = ClaymoreGUID::Generate();
                    entityData->EntityGuid = instanceGuid;
                    entityData->PrefabGuid = req.prefabAssetGuid;
                    entityData->Active = (eh.flags & 0x01) != 0;
                    entityData->Visible = (eh.flags & 0x02) != 0;
                    entityData->Layer = eh.layer;
                    entityData->Tag = req.ctx.ReadString(eh.tagIndex);
                    entityData->ModelAssetGuid.high = eh.modelGuidHigh;
                    entityData->ModelAssetGuid.low = eh.modelGuidLow;
                    
                    for (uint32_t c = 0; c < eh.componentCount; ++c) {
                        ComponentEntry entry;
                        if (req.ctx.offset > data.size() || data.size() - req.ctx.offset < sizeof(ComponentEntry)) {
                            req.loadError = "Component entry out of bounds";
                            req.phase = AsyncPrefabPhase::Failed;
                            return true;
                        }
                        if (!req.ctx.Read(entry)) {
                            req.loadError = "Failed to read component entry";
                            req.phase = AsyncPrefabPhase::Failed;
                            return true;
                        }
                        if (entry.dataSize > data.size() - req.ctx.offset) {
                            req.loadError = "Component data out of bounds";
                            req.phase = AsyncPrefabPhase::Failed;
                            return true;
                        }
                        size_t nextCompOffset = req.ctx.offset + entry.dataSize;
                        binary::ComponentLoadContext compCtx;
                        compCtx.data = req.ctx.data + req.ctx.offset;
                        compCtx.size = entry.dataSize;
                        compCtx.pos = 0;
                        compCtx.version = req.header.base.version;
                        compCtx.prewarm = true;
                        compCtx.readString = [&req](uint32_t idx) { return req.ctx.ReadString(idx); };
                        binary::LoadComponentBinary(compCtx, entityData.get(), entry.typeId, entry.dataSize);
                        req.ctx.offset = nextCompOffset;
                    }
                    
                    EntityID entityId = INVALID_ENTITY_ID;
                    EntityData* sceneData = nullptr;
                    bool reuseRoot = (req.entityIndex == req.header.rootEntityIndex && req.placeholderRoot != INVALID_ENTITY_ID);
                    if (reuseRoot) {
                        entityId = req.placeholderRoot;
                        sceneData = req.scene ? req.scene->GetEntityData(entityId) : nullptr;
                        if (!sceneData) {
                            reuseRoot = false;
                        } else if (!req.placeholderName.empty() && sceneData->Name == req.placeholderName) {
                            sceneData->Name = name;
                        }
                    }
                    
                    if (!reuseRoot) {
                        Entity entity = fastHierarchy ? req.scene->CreateEntityExactFast(name) : req.scene->CreateEntityExact(name);
                        entityId = entity.GetID();
                        sceneData = req.scene->GetEntityData(entityId);
                    }
                    
                    if (sceneData) {
                        bool preserveTransform = reuseRoot && !IsDefaultTransform(sceneData->Transform);
                        bool forcePresentationHidden = false;
                        bool forceRootInactive = false;
                        if (req.scene && req.placeholderRoot != INVALID_ENTITY_ID) {
                            if (EntityData* placeholderData = req.scene->GetEntityData(req.placeholderRoot)) {
                                forcePresentationHidden = placeholderData->PresentationHidden;
                                forceRootInactive = reuseRoot && (entityId == req.placeholderRoot) && !placeholderData->Active;
                            }
                        }
                        sceneData->EntityGuid = instanceGuid;
                        if (!preserveTransform) {
                            sceneData->Transform = entityData->Transform;
                        }
                        sceneData->PrefabGuid = req.prefabAssetGuid;
                        sceneData->Active = forceRootInactive ? false : entityData->Active;
                        sceneData->Visible = entityData->Visible;
                        sceneData->PresentationHidden = forcePresentationHidden;
                        sceneData->Layer = entityData->Layer;
                        sceneData->Tag = entityData->Tag;
                        sceneData->ModelAssetGuid = entityData->ModelAssetGuid;
                        
                        MoveLoadedEntityComponents(*sceneData, *entityData);
                    }
                    
                    if (prefabGuid.high != 0 || prefabGuid.low != 0) {
                        req.guidToEntityId[PackGuidHash(prefabGuid)] = entityId;
                        req.prefabToInstanceGuid[prefabGuid] = instanceGuid;
                        req.instanceToPrefabGuid[instanceGuid] = prefabGuid;
                    }
                    req.createdEntityIds.push_back(entityId);
                    req.oldToNewIdMap[static_cast<EntityID>(eh.entityId)] = entityId;
                }
                req.phase = AsyncPrefabPhase::ResolveParents;
                break;
            }
            case AsyncPrefabPhase::ResolveParents: {
                const bool fastHierarchy = true;
                if (!req.entityHeaders) {
                    req.loadError = "Async prefab request missing parsed entity headers";
                    req.phase = AsyncPrefabPhase::Failed;
                    return true;
                }
                for (; req.parentIndex < req.entityHeaders->size(); ++req.parentIndex) {
                    if (TimeExceeded(start, budgetMs)) {
                        return false;
                    }
                    const auto& eh = (*req.entityHeaders)[req.parentIndex];
                    if (eh.parentGuidHigh == 0 && eh.parentGuidLow == 0) continue;
                    ClaymoreGUID parentGuid{eh.parentGuidHigh, eh.parentGuidLow};
                    auto it = req.guidToEntityId.find(PackGuidHash(parentGuid));
                    if (it != req.guidToEntityId.end()) {
                        EntityID childId = req.createdEntityIds[req.parentIndex];
                        EntityID parentId = it->second;
                        if (fastHierarchy) {
                            req.scene->SetParentFast(childId, parentId);
                        } else {
                            req.scene->SetParent(childId, parentId);
                        }
                    }
                }
                if (fastHierarchy) {
                    req.scene->InvalidateHierarchyCache();
                }
                req.phase = AsyncPrefabPhase::RemapReferences;
                break;
            }
            case AsyncPrefabPhase::RemapReferences: {
                if (TimeExceeded(start, budgetMs)) return false;
                PrefabPerfStageTimer perfStage(
                    *req.scene,
                    asyncPrefabRootId(),
                    req.prefabPath,
                    req.createdEntityIds.size(),
                    "PrefabSetup/RemapEntityRefs",
                    "RemapEntityRefs");
                req.instanceGuidToId.clear();
                req.instanceGuidToId.reserve(req.createdEntityIds.size());
                for (EntityID createdId : req.createdEntityIds) {
                    if (auto* data = req.scene->GetEntityData(createdId)) {
                        if (data->EntityGuid.high != 0 || data->EntityGuid.low != 0) {
                            req.instanceGuidToId[data->EntityGuid] = createdId;
                        }
                    }
                }
                ::RemapPrefabEntityReferences(*req.scene, req.createdEntityIds, req.oldToNewIdMap, &req.prefabToInstanceGuid, &req.instanceGuidToId);
                ResolvePrefabGuidReferences(*req.scene, req.createdEntityIds, req.guidToEntityId);
                
                if (!req.prefabToInstanceGuid.empty() && !req.instanceGuidToId.empty() &&
                    req.header.rootEntityIndex < req.createdEntityIds.size()) {
                    EnsurePrefabJsonLoaded(req);
                    nlohmann::json emptyJson = nlohmann::json::array();
                    EntityID rootId = req.createdEntityIds[req.header.rootEntityIndex];
                    ResolvePrefabScriptEntityReferences(*req.scene, rootId,
                                                        req.prefabEntitiesJson ? *req.prefabEntitiesJson : emptyJson,
                                                        req.prefabToInstanceGuid, req.instanceGuidToId, &req.createdEntityIds);
                    LogPrefabScriptRefAuditSnapshot("post-remap-initial", *req.scene, req.createdEntityIds);
                }
                
                req.phase = AsyncPrefabPhase::ApplyModelDeltas;
                break;
            }
            case AsyncPrefabPhase::ApplyModelDeltas: {
                if (TimeExceeded(start, budgetMs)) return false;
                PrefabPerfStageTimer perfStage(
                    *req.scene,
                    asyncPrefabRootId(),
                    req.prefabPath,
                    req.createdEntityIds.size(),
                    "PrefabSetup/ApplyModelDeltas",
                    "ApplyModelDeltas");
                perfStage.SetExtraDetails("deltas=" + std::to_string(req.header.modelDeltaCount));
                if (req.isV3 && req.header.modelDeltaCount > 0 && req.header.modelDeltaTableOffset > 0) {
                    auto resolveRoot = [&](uint32_t rootEntityId) {
                        EntityID oldModelRoot = static_cast<EntityID>(rootEntityId);
                        auto it = req.oldToNewIdMap.find(oldModelRoot);
                        return (it != req.oldToNewIdMap.end()) ? it->second : oldModelRoot;
                    };
                    
                    auto cachedDeltas = (req.allowDeltaCache && !req.cacheKey.empty())
                        ? TryGetPrefabModelDeltaCache(req.cacheKey)
                        : std::shared_ptr<std::vector<CachedModelDelta>>{};
                    if (cachedDeltas && cachedDeltas->size() != req.header.modelDeltaCount) {
                        cachedDeltas.reset();
                    }
                    
                    cm::model::ModelDeltaApplicator applicator(*req.scene);
                    cm::model::DeltaApplicationConfig config;
                    config.applyScripts = false;
                    config.applyEntityOverrides = false;
                    config.verbose = false;
                    config.allowUnmatchedDeltas = true;
                    config.fastHierarchy = true;
                    
                    if (cachedDeltas) {
                        for (const auto& cached : *cachedDeltas) {
                            EntityID newModelRoot = resolveRoot(cached.rootEntityId);
                            if (!cached.delta.IsEmpty()) {
                                applicator.Apply(newModelRoot, cached.delta, config);
                            }
                        }
                    } else {
                        req.ctx.offset = req.header.modelDeltaTableOffset;
                        auto parsedDeltas = std::make_shared<std::vector<CachedModelDelta>>();
                        parsedDeltas->reserve(req.header.modelDeltaCount);
                        bool cacheable = true;
                        
                        for (uint32_t i = 0; i < req.header.modelDeltaCount; ++i) {
                            ModelDeltaEntry entry;
                            if (!req.ctx.Read(entry)) {
                                cacheable = false;
                                break;
                            }
                            if (entry.dataOffset > 0 && entry.dataSize > 0 &&
                                entry.dataOffset + entry.dataSize <= req.data->size()) {
                                std::string deltaStr(
                                    reinterpret_cast<const char*>(req.data->data() + entry.dataOffset),
                                    entry.dataSize);
                                try {
                                    nlohmann::json deltaJson = nlohmann::json::parse(deltaStr);
                                    cm::model::ModelDelta delta = cm::model::ModelDelta::FromJson(deltaJson);
                                    parsedDeltas->push_back({entry.rootEntityId, delta});
                                    
                                    if (!delta.IsEmpty()) {
                                        EntityID newModelRoot = resolveRoot(entry.rootEntityId);
                                        applicator.Apply(newModelRoot, delta, config);
                                    }
                                } catch (const std::exception& e) {
                                    cacheable = false;
                                    PREFAB_LOG_ERROR("Failed to parse model delta: " << e.what());
                                }
                            } else {
                                cacheable = false;
                            }
                        }
                        
                        if (req.allowDeltaCache && cacheable && !req.cacheKey.empty()) {
                            StorePrefabModelDeltaCache(req.cacheKey, parsedDeltas);
                        }
                    }
                    
                    if (!req.prefabToInstanceGuid.empty() && !req.instanceGuidToId.empty() &&
                        req.header.rootEntityIndex < req.createdEntityIds.size()) {
                        ResolvePrefabGuidReferences(*req.scene, req.createdEntityIds, req.guidToEntityId);
                        nlohmann::json emptyJson = nlohmann::json::array();
                        EntityID rootId = req.createdEntityIds[req.header.rootEntityIndex];
                        ResolvePrefabScriptEntityReferences(*req.scene, rootId,
                                                            req.prefabEntitiesJson ? *req.prefabEntitiesJson : emptyJson,
                                                            req.prefabToInstanceGuid, req.instanceGuidToId, &req.createdEntityIds);
                        LogPrefabScriptRefAuditSnapshot("post-remap-after-deltas", *req.scene, req.createdEntityIds);
                    }
                }
                req.phase = AsyncPrefabPhase::CollectEntities;
                break;
            }
            case AsyncPrefabPhase::CollectEntities: {
                if (TimeExceeded(start, budgetMs)) return false;
                req.allPrefabEntities.clear();
                if (!req.createdEntityIds.empty() && req.header.rootEntityIndex < req.createdEntityIds.size()) {
                    std::function<void(EntityID)> collectDescendants = [&](EntityID id) {
                        req.allPrefabEntities.push_back(id);
                        EntityData* d = req.scene->GetEntityData(id);
                        if (d) {
                            for (EntityID child : d->Children) {
                                collectDescendants(child);
                            }
                        }
                    };
                    collectDescendants(req.createdEntityIds[req.header.rootEntityIndex]);
                }
                req.phase = AsyncPrefabPhase::ResolveMeshes;
                break;
            }
            case AsyncPrefabPhase::ResolveMeshes: {
                if (TimeExceeded(start, budgetMs)) return false;
                {
                    PrefabPerfStageTimer perfStage(
                        *req.scene,
                        asyncPrefabRootId(),
                        req.prefabPath,
                        req.allPrefabEntities.size(),
                        "PrefabSetup/ResolveMeshes",
                        "ResolveMeshes");
                    ResolvePrefabMeshes(*req.scene, req.allPrefabEntities);
                }
                req.phase = AsyncPrefabPhase::ResolveMaterials;
                break;
            }
            case AsyncPrefabPhase::ResolveMaterials: {
                if (TimeExceeded(start, budgetMs)) return false;
                {
                    PrefabPerfStageTimer perfStage(
                        *req.scene,
                        asyncPrefabRootId(),
                        req.prefabPath,
                        req.allPrefabEntities.size(),
                        "PrefabSetup/ResolveMaterials",
                        "ResolveMaterials");
                    ResolvePrefabMaterials(*req.scene, req.allPrefabEntities);
                }
                req.phase = AsyncPrefabPhase::PostProcess;
                break;
            }
            case AsyncPrefabPhase::PostProcess: {
                if (TimeExceeded(start, budgetMs)) return false;
                {
                    PrefabPerfStageTimer perfStage(
                        *req.scene,
                        asyncPrefabRootId(),
                        req.prefabPath,
                        req.allPrefabEntities.size(),
                        "PrefabSetup/PostProcess",
                        "PostProcess");
                    cm::PostProcessEntities(*req.scene, req.allPrefabEntities);
                }
                req.phase = AsyncPrefabPhase::UpdateTransforms;
                break;
            }
            case AsyncPrefabPhase::UpdateTransforms: {
                if (TimeExceeded(start, budgetMs)) return false;
                if (!req.allPrefabEntities.empty()) {
                    PrefabPerfStageTimer perfStage(
                        *req.scene,
                        asyncPrefabRootId(),
                        req.prefabPath,
                        req.allPrefabEntities.size(),
                        "PrefabSetup/UpdateTransforms",
                        "UpdateTransforms");
                    req.scene->MarkTransformDirty(req.allPrefabEntities[0]);
                    req.scene->UpdateTransforms();
                }
                req.phase = AsyncPrefabPhase::CreatePhysics;
                break;
            }
            case AsyncPrefabPhase::CreatePhysics: {
                if (TimeExceeded(start, budgetMs)) return false;
                {
                    PrefabPerfStageTimer perfStage(
                        *req.scene,
                        asyncPrefabRootId(),
                        req.prefabPath,
                        req.allPrefabEntities.size(),
                        "PrefabSetup/CreatePhysicsBodies",
                        "CreatePhysicsBodies");
                    CreatePrefabPhysicsBodies(*req.scene, req.allPrefabEntities);
                }
                req.phase = AsyncPrefabPhase::ApplyPropertyBlocks;
                break;
            }
            case AsyncPrefabPhase::ApplyPropertyBlocks: {
                if (TimeExceeded(start, budgetMs)) return false;
                for (EntityID id : req.allPrefabEntities) {
                    EntityData* data = req.scene->GetEntityData(id);
                    if (data && data->Mesh) {
                        MeshComponent& mc = *data->Mesh;
                        if (!mc.MaterialAssetPaths.empty()) {
                            bool anyMaterialMissing = false;
                            for (size_t i = 0; i < mc.MaterialAssetPaths.size(); ++i) {
                                if (!mc.MaterialAssetPaths[i].empty() &&
                                    (i >= mc.materials.size() || !mc.materials[i])) {
                                    anyMaterialMissing = true;
                                    break;
                                }
                            }
                            if (anyMaterialMissing) {
                                PREFAB_LOG_WARN("Materials not fully loaded for entity '" << data->Name << "' before property block overrides");
                            }
                        }
                    }
                }
                {
                    PrefabPerfStageTimer perfStage(
                        *req.scene,
                        asyncPrefabRootId(),
                        req.prefabPath,
                        req.allPrefabEntities.size(),
                        "PrefabSetup/ApplyPropertyBlocks",
                        "ApplyPropertyBlocks");
                    ApplyPrefabPropertyBlockOverrides(*req.scene, req.allPrefabEntities);
                }
                
                size_t flushedCount = 0;
                {
                    PrefabPerfStageTimer perfStage(
                        *req.scene,
                        asyncPrefabRootId(),
                        req.prefabPath,
                        req.allPrefabEntities.size(),
                        "PrefabSetup/FlushDeferredGPU",
                        "FlushDeferredGPU");
                    flushedCount = DeferredGPU::FlushPendingBuffers();
                    perfStage.SetExtraDetails("flushed=" + std::to_string(flushedCount));
                }
                if (flushedCount > 0) {
                    PREFAB_LOG("Flushed " << flushedCount << " deferred GPU buffers");
                }
                
                {
                    PrefabPerfStageTimer perfStage(
                        *req.scene,
                        asyncPrefabRootId(),
                        req.prefabPath,
                        req.allPrefabEntities.size(),
                        "PrefabSetup/EnsureNavmeshRuntime",
                        "EnsureNavmeshRuntime");
                    for (EntityID id : req.allPrefabEntities) {
                        EntityData* d = req.scene->GetEntityData(id);
                        if (d && d->Navigation) {
                            if (!d->Navigation->Runtime && !d->Navigation->EnsureRuntimeLoaded()) {
                                PREFAB_LOG_ERROR("Failed to load navmesh runtime for entity '" << d->Name << "'");
                            }
                        }
                    }
                }
                
                {
                    PrefabPerfStageTimer perfStage(
                        *req.scene,
                        asyncPrefabRootId(),
                        req.prefabPath,
                        req.allPrefabEntities.size(),
                        "PrefabSetup/AutoBindNavAgents",
                        "AutoBindNavAgents");
                    AutoBindPrefabNavAgents(*req.scene, req.allPrefabEntities, false);
                }
                
                {
                    PrefabPerfStageTimer perfStage(
                        *req.scene,
                        asyncPrefabRootId(),
                        req.prefabPath,
                        req.allPrefabEntities.size(),
                        "PrefabSetup/ResolveUnifiedMorphs",
                        "ResolveUnifiedMorphs");
                    req.unifiedMorphEntities = ResolvePrefabUnifiedMorphs(*req.scene, req.allPrefabEntities);
                }
                req.phase = AsyncPrefabPhase::WarmAnimations;
                break;
            }
            case AsyncPrefabPhase::WarmAnimations: {
                if (TimeExceeded(start, budgetMs)) return false;
                {
                    PrefabPerfStageTimer perfStage(
                        *req.scene,
                        asyncPrefabRootId(),
                        req.prefabPath,
                        req.allPrefabEntities.size(),
                        "PrefabSetup/WarmAnimations",
                        "WarmAnimations");
                    if (!WarmPrefabAnimationPlayers(*req.scene, req.allPrefabEntities)) {
                        return false;
                    }
                }
                req.phase = AsyncPrefabPhase::InitScripts;
                break;
            }
            case AsyncPrefabPhase::InitScripts: {
                if (TimeExceeded(start, budgetMs)) return false;
                if (!req.createdEntityIds.empty() && req.header.rootEntityIndex < req.createdEntityIds.size()) {
                    EntityID rootId = req.createdEntityIds[req.header.rootEntityIndex];
                    EntityData* rootData = req.scene->GetEntityData(rootId);
                    if (rootData) {
                        rootData->PrefabInstance = std::make_unique<PrefabInstanceComponent>();
                        rootData->PrefabInstance->PrefabAssetGuid = req.prefabAssetGuid;
                        rootData->PrefabInstance->PrefabToInstanceGuid = std::move(req.prefabToInstanceGuid);
                        rootData->PrefabInstance->InstanceToPrefabGuid = std::move(req.instanceToPrefabGuid);
                        for (EntityID id : req.allPrefabEntities) {
                            EntityData* d = req.scene->GetEntityData(id);
                            if (d) {
                                d->PrefabGuid = req.prefabAssetGuid;
                                rootData->PrefabInstance->OwnedEntityGuids.push_back(d->EntityGuid);
                            }
                        }
                        rootData->PrefabInstance->PrefabPath = req.prefabPath;
                        rootData->PrefabSource = req.prefabPath;
                        {
                            PrefabPerfStageTimer perfStage(
                                *req.scene,
                                rootId,
                                req.prefabPath,
                                req.allPrefabEntities.size(),
                                "PrefabSetup/UpdateTransformsAfterAttach",
                                "UpdateTransformsAfterAttach");
                            req.scene->MarkTransformDirty(rootId);
                            req.scene->UpdateTransforms();
                        }
                        
                        if (IsDotnetRuntimeReady()) {
                            {
                                PrefabPerfStageTimer perfStage(
                                    *req.scene,
                                    rootId,
                                    req.prefabPath,
                                    req.allPrefabEntities.size(),
                                    "PrefabSetup/CreateScriptInstances",
                                    "CreateScriptInstances");
                                CreatePrefabScriptInstances(*req.scene, req.allPrefabEntities);
                            }
                            
                            {
                                PrefabPerfStageTimer perfStage(
                                    *req.scene,
                                    rootId,
                                    req.prefabPath,
                                    req.allPrefabEntities.size(),
                                    "PrefabSetup/ScriptsOnBind",
                                    "ScriptsOnBind");
                                for (EntityID id : req.allPrefabEntities) {
                                    auto* data = req.scene->GetEntityData(id);
                                    if (!data) continue;
                                    for (auto& script : data->Scripts) {
                                        if (script.Instance) {
                                            Entity entity(id, req.scene);
                                            script.Instance->OnBind(entity);
                                        }
                                    }
                                }
                            }
                            
                            EnsurePrefabJsonLoaded(req);
                            if (rootData->PrefabInstance) {
                                PrefabPerfStageTimer perfStage(
                                    *req.scene,
                                    rootId,
                                    req.prefabPath,
                                    req.allPrefabEntities.size(),
                                    "PrefabSetup/ResolveScriptRefsPreValidate",
                                    "ResolveScriptRefsPreValidate");
                                ResolvePrefabScriptRefsPreValidate(
                                    *req.scene,
                                    rootId,
                                    req.allPrefabEntities,
                                    req.prefabEntitiesJson,
                                    rootData->PrefabInstance->PrefabToInstanceGuid);
                            }

                            CallOnValidateForSubtree(*req.scene, rootId);
                            
                            {
                                PrefabPerfStageTimer perfStage(
                                    *req.scene,
                                    rootId,
                                    req.prefabPath,
                                    req.allPrefabEntities.size(),
                                    "PrefabSetup/ScriptsOnCreateQueue",
                                    "ScriptsOnCreateQueue");
                                struct ScriptInitEntry { Scene* scene; EntityID entityId; size_t scriptIndex; int priority; uint64_t scriptTypeStableId; };
                                std::vector<ScriptInitEntry> toCreate;
                                for (EntityID id : req.allPrefabEntities) {
                                    EntityData* data = req.scene->GetEntityData(id);
                                    if (!data) continue;
                                    for (size_t i = 0; i < data->Scripts.size(); ++i) {
                                        if (data->Scripts[i].Instance) {
                                            const std::string& className = data->Scripts[i].ClassName;
                                            toCreate.push_back({ req.scene, id, i, ScriptSystem::Instance().GetScriptPriority(className), ScriptOrder::StableHashScriptClassName(className) });
                                        }
                                    }
                                }
                                std::sort(toCreate.begin(), toCreate.end(), [](const ScriptInitEntry& a, const ScriptInitEntry& b) {
                                    return ScriptOrder::OrderLess(a.priority, a.scriptTypeStableId, a.entityId, a.scriptIndex, b.priority, b.scriptTypeStableId, b.entityId, b.scriptIndex);
                                });
                                for (const auto& e : toCreate) {
                                    DeferredScriptInit::QueueScriptOnCreate(e.scene, e.entityId, e.scriptIndex);
                                }
                            }
                            
                            {
                                PrefabPerfStageTimer perfStage(
                                    *req.scene,
                                    rootId,
                                    req.prefabPath,
                                    req.allPrefabEntities.size(),
                                    "PrefabSetup/UnifiedMorphPropagate",
                                    "UnifiedMorphPropagate");
                                for (EntityID unifiedId : req.unifiedMorphEntities) {
                                    req.scene->PropagateUnifiedMorphWeights(unifiedId);
                                }
                            }
                            
                            {
                                PrefabPerfStageTimer perfStage(
                                    *req.scene,
                                    rootId,
                                    req.prefabPath,
                                    req.allPrefabEntities.size(),
                                    "PrefabSetup/FinalizeBlendShapes",
                                    "FinalizeBlendShapes");
                                for (EntityID id : req.allPrefabEntities) {
                                    EntityData* data = req.scene->GetEntityData(id);
                                    if (data && data->Mesh && data->BlendShapes) {
                                        if (data->Mesh->BlendShapes != data->BlendShapes.get()) {
                                            data->Mesh->BlendShapes = data->BlendShapes.get();
                                        }
                                        data->BlendShapes->Dirty = true;
                                    }
                                }
                            }
                        }
                    }
                }
                req.phase = AsyncPrefabPhase::Done;
                return true;
            }
            case AsyncPrefabPhase::Done:
            case AsyncPrefabPhase::Failed:
                return true;
        }
    }
    
    return (req.phase == AsyncPrefabPhase::Done || req.phase == AsyncPrefabPhase::Failed);
}

EntityID RuntimePrefabInstantiator::Instantiate(const std::string& prefabPath, Scene& scene, EntityID existingRoot, bool useExistingRoot) {
    return InstantiateAsync(prefabPath, scene, existingRoot, useExistingRoot);
}

EntityID RuntimePrefabInstantiator::InstantiateAsync(const std::string& prefabPath, Scene& scene, EntityID existingRoot, bool useExistingRoot) {
    PrefabScopedTimer timer("Prefab::InstantiateAsync");
    PREFAB_LOG("InstantiateAsync: " << prefabPath);
    
    auto req = std::make_shared<AsyncPrefabRequest>();
    req->scene = &scene;
    req->prefabPath = prefabPath;
    req->allowCache = ShouldUsePrefabBinaryCache();
    req->allowDeltaCache = req->allowCache;
    req->cacheKey = req->allowCache ? NormalizePrefabCacheKey(prefabPath) : std::string{};
    
    const bool canReuseRoot = useExistingRoot && existingRoot != INVALID_ENTITY_ID && scene.GetEntityData(existingRoot) != nullptr;
    if (canReuseRoot) {
        req->overrideRoot = existingRoot;
        req->useOverrideRoot = true;
        req->placeholderRoot = existingRoot;
        if (auto* existingData = scene.GetEntityData(existingRoot)) {
            req->placeholderName = existingData->Name;
        }
    } else {
        const std::string placeholderName = DerivePrefabPlaceholderName(prefabPath);
        req->placeholderName = placeholderName;
        const bool fastHierarchy = scene.m_IsPlaying;
        Entity placeholder = fastHierarchy ? scene.CreateEntityExactFast(placeholderName)
                                           : scene.CreateEntityExact(placeholderName);
        req->placeholderRoot = placeholder.GetID();
    }

    if (auto* placeholderData = scene.GetEntityData(req->placeholderRoot)) {
        placeholderData->PrefabAsyncPending = true;
        placeholderData->PrefabAsyncFailed = false;
    }
    
    {
        std::lock_guard<std::mutex> lock(s_asyncPrefabMutex);
        s_asyncPrefabQueue.push_back(req);
    }
    
    StartAsyncLoad(req);
    return req->placeholderRoot;
}

EntityID RuntimePrefabInstantiator::InstantiateBlocking(const std::string& prefabPath, Scene& scene, EntityID existingRoot, bool useExistingRoot) {
    PrefabScopedTimer timer("Prefab::Instantiate");
    PREFAB_LOG("Instantiate: " << prefabPath);
    
    const bool useCache = ShouldUsePrefabBinaryCache();
    std::string cacheKey = useCache ? NormalizePrefabCacheKey(prefabPath) : std::string{};
    std::shared_ptr<std::vector<uint8_t>> cachedData = useCache ? TryGetPrefabBinaryCache(cacheKey) : nullptr;
    std::vector<uint8_t> localData;
    
    bool readFromVfs = false;
    bool readOk = false;
    
    if (!cachedData) {
        // Use VFS for consistent runtime/editor behavior
        readFromVfs = FileSystem::Instance().ReadFile(prefabPath, localData);
        readOk = readFromVfs;
        if (!readOk) {
            PREFAB_LOG_ERROR("Failed to read file: " << prefabPath);
            return static_cast<EntityID>(-1);
        }
        if (readFromVfs) {
            PREFAB_LOG("Read " << localData.size() << " bytes via VFS");
        }
        if (useCache && readOk) {
            cachedData = std::make_shared<std::vector<uint8_t>>(std::move(localData));
            StorePrefabBinaryCache(cacheKey, cachedData);
        }
    } else {
        readOk = true;
        PREFAB_LOG("Using cached prefab bytes");
    }
    
    const std::vector<uint8_t>& data = cachedData ? *cachedData : localData;
    
    if (data.size() < sizeof(PrefabBinaryHeader)) {
        PREFAB_LOG_ERROR("File too small: " << prefabPath);
        return static_cast<EntityID>(-1);
    }
    
    // Read header
    PrefabBinaryHeader header;
    std::memcpy(&header, data.data(), sizeof(PrefabBinaryHeader));
    
    PREFAB_LOG("Header: magic=0x" << std::hex << header.base.magic << " version=" << std::dec << header.base.version << " flags=" << header.base.flags << " entities=" << header.entityCount);
    
    if (header.base.magic != PREFAB_MAGIC) {
        PREFAB_LOG_ERROR("Invalid magic: " << prefabPath << " (expected 0x" << std::hex << PREFAB_MAGIC << ", got 0x" << header.base.magic << ")" << std::dec);
        return static_cast<EntityID>(-1);
    }
    
    // Check format version
    EntityID rootId = static_cast<EntityID>(-1);
    if ((header.base.flags & 1) == 0) {
        // Binary v2/v3 format (pure binary components)
        PREFAB_LOG("Using binary v2/v3 format");
        rootId = InstantiateBinaryV2(data, header, scene, cacheKey, useCache, existingRoot, useExistingRoot);
    } else {
        // Legacy v1 format (JSON blobs) - use PrefabBinaryLoader
        PREFAB_LOG("Using legacy JSON blob format");
        PrefabAsset asset;
        if (!binary::PrefabBinaryLoader::Load(data, asset)) {
            PREFAB_LOG_ERROR("Failed to load legacy prefab: " << prefabPath);
            return static_cast<EntityID>(-1);
        }
        rootId = InstantiateFromAsset(asset, scene, existingRoot, useExistingRoot);
    }
    
    if (rootId != static_cast<EntityID>(-1)) {
        if (auto* rootData = scene.GetEntityData(rootId)) {
            rootData->PrefabSource = prefabPath;
            if (rootData->PrefabInstance) {
                rootData->PrefabInstance->PrefabPath = prefabPath;
            }
        }
    }
    
    return rootId;
}

bool RuntimePrefabInstantiator::PreloadFromMemory(const std::string& prefabPath,
                                                  std::shared_ptr<std::vector<uint8_t>> data) {
    if (prefabPath.empty() || !data || data->size() < sizeof(PrefabBinaryHeader)) {
        return false;
    }

    const bool useCache = ShouldUsePrefabBinaryCache();
    const std::string cacheKey = useCache ? NormalizePrefabCacheKey(prefabPath) : std::string{};
    if (useCache && !cacheKey.empty()) {
        if (auto cached = TryGetPrefabBinaryCache(cacheKey)) {
            data = std::move(cached);
        } else {
            StorePrefabBinaryCache(cacheKey, data);
        }
    }

    if (!data || data->size() < sizeof(PrefabBinaryHeader)) {
        return false;
    }

    PrefabBinaryHeader header{};
    std::memcpy(&header, data->data(), sizeof(PrefabBinaryHeader));
    if (header.base.magic != PREFAB_MAGIC) {
        return false;
    }

    if ((header.base.flags & 1) == 0 && !cacheKey.empty()) {
        std::shared_ptr<ParsedPrefabLayout> parsedLayout = TryGetParsedPrefabLayoutCache(cacheKey);
        if (!parsedLayout || !IsParsedPrefabLayoutCompatible(*parsedLayout, header)) {
            auto rebuiltLayout = std::make_shared<ParsedPrefabLayout>();
            std::string parseError;
            if (!BuildParsedPrefabLayout(*data, header, *rebuiltLayout, &parseError)) {
                PREFAB_LOG_ERROR(parseError);
                return false;
            }
            StoreParsedPrefabLayoutCache(cacheKey, std::move(rebuiltLayout));
        }
    }

    return true;
}

bool RuntimePrefabInstantiator::Preload(const std::string& prefabPath) {
    if (prefabPath.empty()) {
        return false;
    }

    const bool useCache = ShouldUsePrefabBinaryCache();
    const std::string cacheKey = useCache ? NormalizePrefabCacheKey(prefabPath) : std::string{};
    if (useCache && !cacheKey.empty()) {
        if (auto cached = TryGetPrefabBinaryCache(cacheKey)) {
            return PreloadFromMemory(prefabPath, cached);
        }
    }

    auto bytes = std::make_shared<std::vector<uint8_t>>();
    if (!FileSystem::Instance().ReadFile(prefabPath, *bytes)) {
        PREFAB_LOG_ERROR("Failed to preload prefab: " << prefabPath);
        return false;
    }

    return PreloadFromMemory(prefabPath, std::move(bytes));
}

bool RuntimePrefabInstantiator::PreloadByGuid(const ClaymoreGUID& prefabGuid) {
    if (prefabGuid.high == 0 && prefabGuid.low == 0) {
        return false;
    }

    IAssetResolver* resolver = Assets::GetResolver();
    if (!resolver) {
        return false;
    }

    std::string path = resolver->GetPathForGUID(prefabGuid);
    if (path.empty()) {
        return false;
    }

    if (Assets::ShouldLoadBinary()) {
        std::string binaryPath = resolver->GetBinaryPath(path);
        if (IsPreparedBinaryReady(path, binaryPath)) {
            path = std::move(binaryPath);
        } else if (!Assets::AllowSourceFallback()) {
            PREFAB_LOG_ERROR("Missing or stale compiled prefab for GUID: " << prefabGuid.ToString());
            return false;
        } else {
            return false;
        }
    }

    return Preload(path);
}

void RuntimePrefabInstantiator::UpdateAsync(double budgetMs) {
    auto frameStart = std::chrono::high_resolution_clock::now();
    auto elapsedMs = [&]() {
        const auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(now - frameStart).count();
    };

    auto finalizeRequest = [](const std::shared_ptr<AsyncPrefabRequest>& current) {
        if (!current) return;

        if (current->scene && current->placeholderRoot != INVALID_ENTITY_ID) {
            if (EntityData* rootData = current->scene->GetEntityData(current->placeholderRoot)) {
                rootData->PrefabAsyncPending = false;
                rootData->PrefabAsyncFailed = (current->phase == AsyncPrefabPhase::Failed);
            }
        }

        if (current->phase == AsyncPrefabPhase::Failed) {
            if (current->scene && current->placeholderRoot != INVALID_ENTITY_ID && !current->useOverrideRoot) {
                current->scene->QueueRemoveEntity(current->placeholderRoot);
            }
        }

        std::lock_guard<std::mutex> lock(s_asyncPrefabMutex);
        auto it = std::find(s_asyncPrefabQueue.begin(), s_asyncPrefabQueue.end(), current);
        if (it != s_asyncPrefabQueue.end()) {
            s_asyncPrefabQueue.erase(it);
        }
    };

    while (elapsedMs() < budgetMs) {
        std::shared_ptr<AsyncPrefabRequest> current;
        {
            std::lock_guard<std::mutex> lock(s_asyncPrefabMutex);
            for (const auto& req : s_asyncPrefabQueue) {
                if (!req) continue;
                const bool waitingForData =
                    req->phase == AsyncPrefabPhase::WaitingForData &&
                    !req->dataReady.load(std::memory_order_acquire) &&
                    !req->loadFailed.load(std::memory_order_acquire) &&
                    !req->cancelled.load(std::memory_order_acquire);
                if (!waitingForData) {
                    current = req;
                    break;
                }
            }
        }

        if (!current) {
            return;
        }

        const double remainingMs = std::max(0.0, budgetMs - elapsedMs());
        const bool done = StepAsyncPrefab(*current, remainingMs);
        if (done) {
            finalizeRequest(current);
            continue;
        }

        return;
    }
}

PrefabAsyncStatus RuntimePrefabInstantiator::GetAsyncStatus(EntityID rootEntity, Scene& scene) {
    if (rootEntity == INVALID_ENTITY_ID) {
        return PrefabAsyncStatus::NotFound;
    }

    if (auto* data = scene.GetEntityData(rootEntity)) {
        if (data->PrefabAsyncFailed) {
            return PrefabAsyncStatus::Failed;
        }
        if (data->PrefabAsyncPending) {
            return PrefabAsyncStatus::Pending;
        }
        return PrefabAsyncStatus::Ready;
    }

    return PrefabAsyncStatus::NotFound;
}

void RuntimePrefabInstantiator::ResetRuntimeCaches() {
    {
        std::lock_guard<std::mutex> lock(s_prefabBinaryCacheMutex);
        ClearPrefabRuntimeCachesLocked();
    }
    {
        std::lock_guard<std::mutex> lock(s_prefabJsonCacheMutex);
        s_prefabJsonCache.clear();
    }
    {
        std::lock_guard<std::mutex> lock(s_prefabParsedLayoutCacheMutex);
        s_prefabParsedLayoutCache.clear();
    }
    PREFAB_LOG("Runtime prefab caches reset");
}

void RuntimePrefabInstantiator::InvalidateCache(const std::string& prefabPath) {
    const std::vector<std::string> keys = BuildPrefabCacheInvalidationKeys(prefabPath);
    {
        std::lock_guard<std::mutex> lock(s_asyncPrefabMutex);
        for (const auto& req : s_asyncPrefabQueue) {
            if (!req || req->cacheKey.empty()) {
                continue;
            }
            if (std::find(keys.begin(), keys.end(), req->cacheKey) != keys.end()) {
                req->loadError = "Prefab cache invalidated while async instantiation was pending";
                req->cancelled.store(true, std::memory_order_release);
            }
        }
    }

    for (const std::string& key : keys) {
        ErasePrefabJsonCacheForGuid(TryReadPrefabSourceGuidForCacheKey(key));
        ErasePrefabRuntimeCacheForKey(key);
    }
}

void RuntimePrefabInstantiator::CancelAsyncForScene(Scene& scene, bool removePlaceholders) {
    std::vector<std::shared_ptr<AsyncPrefabRequest>> cancelledRequests;
    {
        std::lock_guard<std::mutex> lock(s_asyncPrefabMutex);
        auto it = s_asyncPrefabQueue.begin();
        while (it != s_asyncPrefabQueue.end()) {
            const auto& req = *it;
            if (req && req->scene == &scene) {
                req->cancelled.store(true, std::memory_order_release);
                cancelledRequests.push_back(req);
                it = s_asyncPrefabQueue.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (const auto& req : cancelledRequests) {
        if (!req) continue;
        Scene* reqScene = req->scene;
        req->scene = nullptr;
        if (removePlaceholders &&
            reqScene &&
            req->placeholderRoot != INVALID_ENTITY_ID &&
            !req->useOverrideRoot) {
            reqScene->QueueRemoveEntity(req->placeholderRoot);
        }
    }
}

EntityID RuntimePrefabInstantiator::InstantiateFromAsset(const PrefabAsset& asset, Scene& scene, EntityID existingRoot, bool useExistingRoot) {
    // Legacy path for PrefabAsset with JSON entities
    // This is the fallback for old-format prefabs
    
    if (asset.Entities.empty() || !asset.Entities.is_array()) {
        PREFAB_LOG_ERROR("Invalid prefab asset");
        return static_cast<EntityID>(-1);
    }
    
    std::unordered_map<uint64_t, EntityID> guidToEntityId;
    std::vector<EntityID> createdIds;
    EntityID rootId = static_cast<EntityID>(-1);
    const bool reuseRootRequested = useExistingRoot && existingRoot != INVALID_ENTITY_ID && scene.GetEntityData(existingRoot) != nullptr;
    
    // Create entities
    for (const auto& entityJson : asset.Entities) {
        if (!entityJson.is_object()) continue;
        
        std::string name = entityJson.value("name", "Entity");
        EntityID entityId = INVALID_ENTITY_ID;
        
        ClaymoreGUID guid{};
        if (entityJson.contains("guid")) {
            try { entityJson.at("guid").get_to(guid); } catch (...) {}
        }
        
        EntityData* data = nullptr;
        bool reuseEntity = reuseRootRequested &&
            ((guid.high == asset.RootGuid.high && guid.low == asset.RootGuid.low) || rootId == static_cast<EntityID>(-1));
        if (reuseEntity) {
            entityId = existingRoot;
            data = scene.GetEntityData(entityId);
            if (!data) {
                reuseEntity = false;
            } else {
                data->Name = name;
            }
        }
        if (!reuseEntity) {
            Entity entity = scene.CreateEntity(name);
            entityId = entity.GetID();
            data = scene.GetEntityData(entityId);
        }
        if (data) {
            data->EntityGuid = guid;
            
            // Load transform
            if (entityJson.contains("transform")) {
                const auto& t = entityJson["transform"];
                if (t.contains("position") && t["position"].is_array()) {
                    data->Transform.Position.x = t["position"][0].get<float>();
                    data->Transform.Position.y = t["position"][1].get<float>();
                    data->Transform.Position.z = t["position"][2].get<float>();
                }
                if (t.contains("rotation") && t["rotation"].is_array()) {
                    data->Transform.Rotation.x = t["rotation"][0].get<float>();
                    data->Transform.Rotation.y = t["rotation"][1].get<float>();
                    data->Transform.Rotation.z = t["rotation"][2].get<float>();
                }
                if (t.contains("scale") && t["scale"].is_array()) {
                    data->Transform.Scale.x = t["scale"][0].get<float>();
                    data->Transform.Scale.y = t["scale"][1].get<float>();
                    data->Transform.Scale.z = t["scale"][2].get<float>();
                }
            }
        }
        
        guidToEntityId[guid.high] = entityId;
        createdIds.push_back(entityId);
        
        if (guid.high == asset.RootGuid.high && guid.low == asset.RootGuid.low) {
            rootId = entityId;
        }
    }
    
    // Setup parent relationships
    for (size_t i = 0; i < asset.Entities.size(); ++i) {
        const auto& entityJson = asset.Entities[i];
        if (!entityJson.contains("_parentGuid")) continue;
        
        ClaymoreGUID parentGuid{};
        try { entityJson.at("_parentGuid").get_to(parentGuid); } catch (...) { continue; }
        
        auto it = guidToEntityId.find(parentGuid.high);
        if (it != guidToEntityId.end()) {
            scene.SetParent(createdIds[i], it->second);
        }
    }
    
    if (rootId == static_cast<EntityID>(-1) && !createdIds.empty()) {
        rootId = createdIds[0];
    }
    
    return rootId;
}

EntityID RuntimePrefabInstantiator::InstantiateByGuid(const ClaymoreGUID& prefabGuid, Scene& scene, EntityID existingRoot, bool useExistingRoot) {
    IAssetResolver* resolver = Assets::GetResolver();
    if (!resolver) {
        PREFAB_LOG_ERROR("No asset resolver available");
        return static_cast<EntityID>(-1);
    }
    
    std::string path = resolver->GetPathForGUID(prefabGuid);
    if (path.empty()) {
        PREFAB_LOG_ERROR("GUID not found: " << prefabGuid.ToString());
        return static_cast<EntityID>(-1);
    }
    
    if (Assets::ShouldLoadBinary()) {
        std::string binaryPath = resolver->GetBinaryPath(path);
        if (IsPreparedBinaryReady(path, binaryPath)) {
            path = std::move(binaryPath);
        } else {
            PREFAB_LOG_ERROR("Missing or stale compiled prefab for GUID: " << prefabGuid.ToString());
            return static_cast<EntityID>(-1);
        }
    }

    return Instantiate(path, scene, existingRoot, useExistingRoot);
}

} // namespace runtime

// Global function for remapping prefab entity references
// Callable from both PrefabAPI.cpp (JSON path) and RuntimePrefabInstantiator (binary path)
// Note: Default arguments are specified in RuntimePrefabInstantiator.h
void RemapPrefabEntityReferences(Scene& scene, const std::vector<EntityID>& createdEntityIds,
                                  const std::unordered_map<EntityID, EntityID>& oldToNewIdMap,
                                  const std::unordered_map<ClaymoreGUID, ClaymoreGUID>* prefabToInstanceGuid,
                                  const std::unordered_map<ClaymoreGUID, EntityID>* instanceGuidToId) {
    std::unordered_map<uint64_t, EntityID> guidToEntityId;
    guidToEntityId.reserve(createdEntityIds.size());
    
    // CRITICAL FIX: Build map from prefab GUIDs to instance-time runtime entity IDs
    // This is needed because meta.guid contains prefab GUIDs (authoring-time), not instance GUIDs
    std::unordered_map<uint64_t, EntityID> prefabGuidToEntityId;
    if (prefabToInstanceGuid && instanceGuidToId) {
        for (const auto& [prefabGuid, instanceGuid] : *prefabToInstanceGuid) {
            auto it = instanceGuidToId->find(instanceGuid);
            if (it != instanceGuidToId->end()) {
                prefabGuidToEntityId[PackGuidHash(prefabGuid)] = it->second;
            }
        }
    }
    
    // Also build map from instance GUIDs to entity IDs (for fallback)
    for (EntityID newId : createdEntityIds) {
        EntityData* d = scene.GetEntityData(newId);
        if (d) {
            guidToEntityId[PackGuidHash(d->EntityGuid)] = newId;
        }
    }

    std::unordered_set<EntityID> createdEntitySet(createdEntityIds.begin(), createdEntityIds.end());
    auto isPrefabEntityValue = [&](int value) -> bool {
        return IsValidEntityRefValue(value) &&
               createdEntitySet.find(static_cast<EntityID>(value)) != createdEntitySet.end();
    };
    auto logScriptRefMap = [&](const char* stage,
                               EntityID ownerId,
                               const std::string& scriptClass,
                               const std::string& propName,
                               int oldVal,
                               int newVal,
                               const ScriptEntityRefMetadata* meta,
                               const char* reason,
                               const char* source) {
        auto* ownerData = scene.GetEntityData(ownerId);
        const std::string ownerName = ownerData ? ownerData->Name : "Unknown";
        PREFAB_LOG("[ScriptRefMap] stage=" << stage
            << " owner=" << ownerId << "(" << ownerName << ")"
            << " script=" << scriptClass
            << " prop=" << propName
            << " old=" << oldVal << "(" << DescribeEntityRefTarget(scene, oldVal) << ")"
            << " new=" << newVal << "(" << DescribeEntityRefTarget(scene, newVal) << ")"
            << " oldInPrefab=" << (isPrefabEntityValue(oldVal) ? "true" : "false")
            << " newInPrefab=" << (isPrefabEntityValue(newVal) ? "true" : "false")
            << " reason=" << reason
            << " source=" << source
            << " " << DescribeEntityRefMeta(meta));
    };
    auto shouldLogScriptRef = [&](int oldVal, int newVal, const ScriptEntityRefMetadata* meta) -> bool {
        if (oldVal != newVal) return true;
        if (HasEntityRefHints(meta)) return true;
        if (IsValidEntityRefValue(oldVal) && !isPrefabEntityValue(oldVal)) return true;
        if (IsValidEntityRefValue(newVal) && !isPrefabEntityValue(newVal)) return true;
        return false;
    };
    auto remapRefOrInvalid = [&](EntityID oldId, EntityID invalidValue) -> EntityID {
        if (oldId == 0 || oldId == static_cast<EntityID>(-1) || oldId == INVALID_ENTITY_ID) {
            return oldId;
        }
        auto it = oldToNewIdMap.find(oldId);
        if (it == oldToNewIdMap.end()) {
            return invalidValue;
        }
        EntityID mapped = it->second;
        return scene.GetEntityData(mapped) ? mapped : invalidValue;
    };
    auto isDescendantOrSelf = [&](EntityID candidate, EntityID root) -> bool {
        if (candidate == INVALID_ENTITY_ID || candidate == static_cast<EntityID>(-1) || candidate == 0) return false;
        EntityID cur = candidate;
        size_t guard = 0;
        while (cur != INVALID_ENTITY_ID && cur != static_cast<EntityID>(-1) && guard++ < 100000) {
            if (cur == root) return true;
            auto* d = scene.GetEntityData(cur);
            if (!d) break;
            cur = d->Parent;
        }
        return false;
    };

    for (EntityID id : createdEntityIds) {
        EntityData* data = scene.GetEntityData(id);
        if (!data) continue;
        
        // Remap Skeleton BoneEntities
        if (data->Skeleton) {
            for (size_t i = 0; i < data->Skeleton->BoneEntities.size(); ++i) {
                EntityID oldId = data->Skeleton->BoneEntities[i];
                if (oldId == 0 || oldId == static_cast<EntityID>(-1) || oldId == INVALID_ENTITY_ID) {
                    continue;
                }
                EntityID mapped = remapRefOrInvalid(oldId, static_cast<EntityID>(-1));
                // Guard against wrong-but-existing remaps: skeleton bones must stay in the same skeleton subtree.
                if (mapped != static_cast<EntityID>(-1) && !isDescendantOrSelf(mapped, id)) {
                    mapped = static_cast<EntityID>(-1);
                }
                data->Skeleton->BoneEntities[i] = mapped;
            }
            // Stage 3 foundation: DeepCopy does not carry the per-bone back-reference
            // marker, so rebuild it from the freshly remapped BoneEntities. Without this,
            // cloned / prefab-spawned characters (the alkahest spawn path) resolve bone
            // world matrices from the stale AoS transform instead of the live pose
            // palette. Safe + idempotent (a bad marker only falls back to AoS).
            scene.RebindSkeletonBoneMarkers(id);
        }

        // Remap Skinning SkeletonRoot
        if (data->Skinning) {
            EntityID oldId = data->Skinning->SkeletonRoot;
            if (oldId != 0 && oldId != static_cast<EntityID>(-1) && oldId != INVALID_ENTITY_ID) {
                EntityID mapped = remapRefOrInvalid(oldId, static_cast<EntityID>(-1));
                if (mapped != static_cast<EntityID>(-1)) {
                    EntityData* skelData = scene.GetEntityData(mapped);
                    if (!skelData || !skelData->Skeleton) {
                        mapped = static_cast<EntityID>(-1);
                    }
                }
                data->Skinning->SkeletonRoot = mapped;
                if (mapped == static_cast<EntityID>(-1)) {
                    data->Skinning->ResolvedSkeletonRoot = INVALID_ENTITY_ID;
                    data->Skinning->InvalidateRemap();
                }
            }
        }
        
        // Remap BoneAttachment SkeletonEntity
        if (data->BoneAttachment) {
            EntityID oldId = data->BoneAttachment->SkeletonEntity;
            if (oldId != 0 && oldId != static_cast<EntityID>(-1) && oldId != INVALID_ENTITY_ID) {
                EntityID mapped = remapRefOrInvalid(oldId, static_cast<EntityID>(-1));
                if (mapped != static_cast<EntityID>(-1)) {
                    EntityData* skelData = scene.GetEntityData(mapped);
                    if (!skelData || !skelData->Skeleton) {
                        mapped = static_cast<EntityID>(-1);
                    }
                }
                data->BoneAttachment->SkeletonEntity = mapped;
                if (mapped == static_cast<EntityID>(-1)) {
                    data->BoneAttachment->InvalidateResolution();
                }
            }
        }
        
        // Remap IK TargetEntity and PoleEntity
        for (auto& ik : data->IKs) {
            if (ik.TargetEntity != 0 && ik.TargetEntity != static_cast<EntityID>(-1)) {
                auto it = oldToNewIdMap.find(ik.TargetEntity);
                if (it != oldToNewIdMap.end()) {
                    ik.TargetEntity = it->second;
                }
            }
            if (ik.PoleEntity != 0 && ik.PoleEntity != static_cast<EntityID>(-1)) {
                auto it = oldToNewIdMap.find(ik.PoleEntity);
                if (it != oldToNewIdMap.end()) {
                    ik.PoleEntity = it->second;
                }
            }
        }
        
        // Remap LookAtConstraint TargetEntity
        for (auto& lac : data->LookAtConstraints) {
            if (lac.TargetEntity != 0 && lac.TargetEntity != static_cast<EntityID>(-1)) {
                auto it = oldToNewIdMap.find(lac.TargetEntity);
                if (it != oldToNewIdMap.end()) {
                    lac.TargetEntity = it->second;
                }
            }
        }
        
        // Remap NavAgent NavMeshEntity
        // If the NavMeshEntity references an entity outside the prefab, clear it to 0
        // so the Navigation system's auto-binding can find the nearest navmesh in the scene
        if (data->NavAgent) {
            EntityID oldId = data->NavAgent->NavMeshEntity;
            if (oldId != 0 && oldId != static_cast<EntityID>(-1)) {
                auto it = oldToNewIdMap.find(oldId);
                if (it != oldToNewIdMap.end()) {
                    data->NavAgent->NavMeshEntity = it->second;
                } else {
                    // Reference is external to prefab - clear to trigger auto-binding
                    PREFAB_LOG("NavAgent on entity " << id << " has external NavMeshEntity ref (old ID " << oldId << "), clearing for auto-binding");
                    data->NavAgent->NavMeshEntity = 0;
                }
            }
        }
        
        // Remap MeshProxy SerializedTarget
        if (data->MeshProxy) {
            EntityID oldId = data->MeshProxy->SerializedTarget;
            if (oldId != 0 && oldId != static_cast<EntityID>(-1)) {
                auto it = oldToNewIdMap.find(oldId);
                if (it != oldToNewIdMap.end()) {
                    data->MeshProxy->SerializedTarget = it->second;
                }
            }
        }
        
        // Remap TintController target entity references
        // This ensures tint targets point to the correct entities in the instantiated prefab
        if (data->TintController) {
            for (auto& target : data->TintController->Targets) {
                EntityID oldId = target.TargetEntity;
                if (oldId != 0 && oldId != static_cast<EntityID>(-1)) {
                    auto it = oldToNewIdMap.find(oldId);
                    if (it != oldToNewIdMap.end()) {
                        target.TargetEntity = it->second;
                    } else {
                        // Target entity is external to prefab - mark as invalid
                        target.TargetEntity = static_cast<EntityID>(-1);
                    }
                }
            }
            // Force refresh of tint matching after remapping
            data->TintController->NeedsRefresh = true;
            data->TintController->TintDirty = true;
        }
        
        auto isEntityLikeType = [](PropertyType t) -> bool {
            return t == PropertyType::Entity || t == PropertyType::ComponentRef || t == PropertyType::ScriptRef;
        };

        auto remapEntityId = [&guidToEntityId, &prefabGuidToEntityId, &oldToNewIdMap, &createdEntitySet](
            int oldId, const ScriptEntityRefMetadata* meta, const char** outSource) -> int {
            auto mark = [&](const char* src, int value) -> int {
                if (outSource) *outSource = src;
                return value;
            };
            if (meta) {
                // CRITICAL FIX: Always prioritize GUID-based resolution for internal prefab references
                // meta.guid contains prefab GUIDs (authoring-time), not instance GUIDs
                // We need to look up prefab GUIDs in prefabGuidToEntityId, not guidToEntityId
                if (meta->guid.high != 0 || meta->guid.low != 0) {
                    // First try prefab GUID lookup (for internal prefab references)
                    // This is the correct way to resolve internal prefab entity references
                    auto prefabIt = prefabGuidToEntityId.find(PackGuidHash(meta->guid));
                    if (prefabIt != prefabGuidToEntityId.end()) {
                        // CRITICAL: Verify the resolved entity is actually in the prefab instance
                        EntityID resolvedId = prefabIt->second;
                        if (createdEntitySet.find(resolvedId) != createdEntitySet.end()) {
                            return mark("prefab-guid", static_cast<int>(resolvedId));
                        }
                        // GUID matched but entity not in prefab - might be external reference, return -1 for JSON resolution
                        return mark("prefab-guid-outside-prefab", -1);
                    }
                    // Fallback: try instance GUID lookup (for external references that were already remapped)
                    auto gIt = guidToEntityId.find(PackGuidHash(meta->guid));
                    if (gIt != guidToEntityId.end()) {
                        return mark("instance-guid-fallback", static_cast<int>(gIt->second));
                    }
                    // GUID not found - return -1 to trigger JSON-based resolution
                    return mark("guid-not-found", -1);
                }

                // If we have model path hints but no GUID, avoid numeric-ID remapping.
                // Authoring-time IDs can collide with prefab-local IDs and bind to wrong nodes
                // (e.g. camera refs becoming skeleton bones). Let JSON/metadata path resolution
                // choose the target instead.
                if ((meta->modelGuid.high != 0 || meta->modelGuid.low != 0 || !meta->modelNodePath.empty())) {
                    return mark("model-path-hints", -1);
                }
                
                // Fallback: Try entityId remapping (only works if meta.entityId is a prefab-local ID)
                // This handles v2 format where entityId might be the prefab-local index
                // CRITICAL: meta.entityId might contain authoring-time runtime IDs, not prefab-local IDs
                // So this lookup might fail, which is why GUID-based resolution is prioritized above
                if (meta->entityId > 0) {
                    auto it = oldToNewIdMap.find(static_cast<EntityID>(meta->entityId));
                    if (it != oldToNewIdMap.end()) {
                        // CRITICAL: Verify the resolved entity is actually in the prefab instance
                        EntityID resolvedId = it->second;
                        if (createdEntitySet.find(resolvedId) != createdEntitySet.end()) {
                            return mark("meta-entity-id", static_cast<int>(resolvedId));
                        }
                        // EntityID matched but entity not in prefab - return -1 for JSON resolution
                        return mark("meta-entity-id-outside-prefab", -1);
                    }
                }
            }
            // Final fallback: try oldId directly (might be prefab-local ID from v2 format)
            // CRITICAL FIX: Only use this if oldId actually maps to a prefab entity
            // Don't blindly preserve authoring-time IDs that might match scene entities
            if (oldId > 0 && oldId != -1) {
                auto it = oldToNewIdMap.find(static_cast<EntityID>(oldId));
                if (it != oldToNewIdMap.end()) {
                    // Verify the resolved entity is actually in the prefab instance
                    EntityID resolvedId = it->second;
                    if (createdEntitySet.find(resolvedId) != createdEntitySet.end()) {
                        return mark("old-id-map", static_cast<int>(resolvedId));
                    }
                }
                // oldId doesn't map to a prefab entity - return -1 for JSON resolution
                // This prevents authoring-time IDs (like entity ID 2) from being preserved incorrectly
                return mark("old-id-no-prefab-match", -1);
            }
            return mark("invalid-or-empty", -1);
        };

        // Remap Script entity references in Values
        for (auto& script : data->Scripts) {
            bool hasReflection = ScriptReflection::HasProperties(script.ClassName);

            if (hasReflection) {
                auto& properties = ScriptReflection::GetScriptProperties(script.ClassName);
                std::unordered_set<std::string> reflectedEntityLikeProps;
                reflectedEntityLikeProps.reserve(properties.size());
                for (const auto& prop : properties) {
                    if (isEntityLikeType(prop.type)) {
                        reflectedEntityLikeProps.insert(prop.name);
                        auto it = script.Values.find(prop.name);
                        if (it != script.Values.end() && std::holds_alternative<int>(it->second)) {
                            auto metaIt = script.EntityRefMetadata.find(prop.name);
                            int oldVal = std::get<int>(it->second);
                            const ScriptEntityRefMetadata* metaPtr =
                                (metaIt != script.EntityRefMetadata.end()) ? &metaIt->second : nullptr;
                            const char* source = "unknown";
                            int newVal = remapEntityId(oldVal, metaPtr, &source);
                            
                            // CRITICAL FIX: Always update to newVal, even if it's -1
                            // If remapping failed (newVal == -1), we need to clear the old value so
                            // ResolvePrefabScriptEntityReferences can fix it using JSON-based resolution.
                            // Preserving oldVal when it's an authoring-time ID that doesn't map correctly
                            // causes it to incorrectly match scene entities (like entity ID 2 = Directional Light)
                            it->second = newVal;

                            if (shouldLogScriptRef(oldVal, newVal, metaPtr)) {
                                logScriptRefMap(
                                    "remap-initial",
                                    id,
                                    script.ClassName,
                                    prop.name,
                                    oldVal,
                                    newVal,
                                    metaPtr,
                                    "reflection-entity-like",
                                    source);
                            }
                        }
                    }

                    if (prop.type == PropertyType::List && isEntityLikeType(prop.listElementType)) {
                        auto it = script.Values.find(prop.name);
                        if (it != script.Values.end() &&
                            std::holds_alternative<std::shared_ptr<ListPropertyValue>>(it->second)) {
                            auto listPtr = std::get<std::shared_ptr<ListPropertyValue>>(it->second);
                            if (listPtr) {
                                for (size_t i = 0; i < listPtr->elements.size(); ++i) {
                                    if (std::holds_alternative<int>(listPtr->elements[i])) {
                                        ScriptEntityRefMetadata* metaPtr = (i < listPtr->entityRefs.size()) ? &listPtr->entityRefs[i] : nullptr;
                                        int oldVal = std::get<int>(listPtr->elements[i]);
                                        const char* source = "unknown";
                                        int newVal = remapEntityId(oldVal, metaPtr, &source);
                                        listPtr->elements[i] = newVal;
                                        if (metaPtr) metaPtr->entityId = newVal;
                                        if (shouldLogScriptRef(oldVal, newVal, metaPtr)) {
                                            logScriptRefMap(
                                                "remap-initial",
                                                id,
                                                script.ClassName,
                                                prop.name + "[" + std::to_string(i) + "]",
                                                oldVal,
                                                newVal,
                                                metaPtr,
                                                "reflection-list",
                                                source);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // Metadata-driven fallback for properties missing/incorrect in reflection.
                // This preserves deterministic remapping when typed binary data encodes entity refs
                // but script reflection is stale or incomplete at load time.
                for (auto& [key, value] : script.Values) {
                    if (reflectedEntityLikeProps.find(key) != reflectedEntityLikeProps.end()) {
                        continue; // already handled above
                    }
                    auto metaIt = script.EntityRefMetadata.find(key);
                    if (metaIt == script.EntityRefMetadata.end()) {
                        continue;
                    }
                    ScriptEntityRefMetadata* metaPtr = &metaIt->second;

                    if (std::holds_alternative<int>(value)) {
                        const bool hasRefHints = (metaPtr->guid.high != 0 || metaPtr->guid.low != 0 ||
                                                  metaPtr->modelGuid.high != 0 || metaPtr->modelGuid.low != 0 ||
                                                  !metaPtr->modelNodePath.empty() || metaPtr->entityId > 0);
                        if (!hasRefHints) continue;

                        int oldVal = std::get<int>(value);
                        const char* source = "unknown";
                        int newVal = remapEntityId(oldVal, metaPtr, &source);
                        value = newVal;
                        metaPtr->entityId = newVal;
                        if (shouldLogScriptRef(oldVal, newVal, metaPtr)) {
                            logScriptRefMap(
                                "remap-initial",
                                id,
                                script.ClassName,
                                key,
                                oldVal,
                                newVal,
                                metaPtr,
                                "metadata-fallback-single",
                                source);
                        }
                    } else if (std::holds_alternative<std::shared_ptr<ListPropertyValue>>(value)) {
                        auto listPtr = std::get<std::shared_ptr<ListPropertyValue>>(value);
                        if (!listPtr) continue;
                        for (size_t i = 0; i < listPtr->elements.size(); ++i) {
                            if (!std::holds_alternative<int>(listPtr->elements[i])) continue;
                            ScriptEntityRefMetadata* elemMeta = (i < listPtr->entityRefs.size()) ? &listPtr->entityRefs[i] : nullptr;
                            int oldVal = std::get<int>(listPtr->elements[i]);
                            const char* source = "unknown";
                            int newVal = remapEntityId(oldVal, elemMeta, &source);
                            listPtr->elements[i] = newVal;
                            if (elemMeta) elemMeta->entityId = newVal;
                            if (shouldLogScriptRef(oldVal, newVal, elemMeta)) {
                                logScriptRefMap(
                                    "remap-initial",
                                    id,
                                    script.ClassName,
                                    key + "[" + std::to_string(i) + "]",
                                    oldVal,
                                    newVal,
                                    elemMeta,
                                    "metadata-fallback-list",
                                    source);
                            }
                        }
                    }
                }
            } else {
                // CRITICAL FIX: Process scripts without reflection metadata
                // These might have entity references that need remapping too
                for (auto& [key, value] : script.Values) {
                    auto metaIt = script.EntityRefMetadata.find(key);
                    ScriptEntityRefMetadata* metaPtr = (metaIt != script.EntityRefMetadata.end()) ? &metaIt->second : nullptr;

                    if (std::holds_alternative<int>(value)) {
                        int oldVal = std::get<int>(value);
                        // CRITICAL: Only remap if we have metadata indicating this is an entity reference
                        // Without metadata, we can't tell if an int is an entity ID or just a regular int
                        if (metaPtr && (metaPtr->guid.high != 0 || metaPtr->guid.low != 0 ||
                                        metaPtr->modelGuid.high != 0 || metaPtr->modelGuid.low != 0 ||
                                        !metaPtr->modelNodePath.empty() || metaPtr->entityId > 0)) {
                            const char* source = "unknown";
                            int newVal = remapEntityId(oldVal, metaPtr, &source);
                            // CRITICAL FIX: Always update to newVal, even if it's -1
                            // If remapping failed, clear the old value so JSON-based resolution can fix it
                            // Preserving oldVal when it doesn't map correctly causes incorrect matches to scene entities
                            value = newVal;
                            if (metaPtr) metaPtr->entityId = newVal;
                            if (shouldLogScriptRef(oldVal, newVal, metaPtr)) {
                                logScriptRefMap(
                                    "remap-initial",
                                    id,
                                    script.ClassName,
                                    key,
                                    oldVal,
                                    newVal,
                                    metaPtr,
                                    "no-reflection-single",
                                    source);
                            }
                        }
                    } else if (std::holds_alternative<std::shared_ptr<ListPropertyValue>>(value)) {
                        auto listPtr = std::get<std::shared_ptr<ListPropertyValue>>(value);
                        if (listPtr) {
                            for (size_t i = 0; i < listPtr->elements.size(); ++i) {
                                if (std::holds_alternative<int>(listPtr->elements[i])) {
                                    ScriptEntityRefMetadata* elemMeta = (i < listPtr->entityRefs.size()) ? &listPtr->entityRefs[i] : nullptr;
                                    int oldVal = std::get<int>(listPtr->elements[i]);
                                    const char* source = "unknown";
                                    int newVal = remapEntityId(oldVal, elemMeta, &source);
                                    listPtr->elements[i] = newVal;
                                    if (elemMeta) elemMeta->entityId = newVal;
                                    if (shouldLogScriptRef(oldVal, newVal, elemMeta)) {
                                        logScriptRefMap(
                                            "remap-initial",
                                            id,
                                            script.ClassName,
                                            key + "[" + std::to_string(i) + "]",
                                            oldVal,
                                            newVal,
                                            elemMeta,
                                            "no-reflection-list",
                                            source);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

// FIX CASE 4: Helper to resolve entity reference using ScriptEntityRefMetadata
// This provides GUID-based fallback when source JSON is not available
static EntityID ResolveEntityRefFromMetadata(
    Scene& scene,
    EntityID prefabRoot,
    const ScriptEntityRefMetadata& meta,
    const std::unordered_map<ClaymoreGUID, ClaymoreGUID>& prefabToInstanceGuid,
    const std::unordered_map<ClaymoreGUID, EntityID>& instanceGuidToId)
{
    // Try GUID resolution first (CRITICAL: This is the correct way to resolve internal prefab references)
    if (meta.guid.high != 0 || meta.guid.low != 0) {
        auto it = prefabToInstanceGuid.find(meta.guid);
        if (it != prefabToInstanceGuid.end()) {
            auto idIt = instanceGuidToId.find(it->second);
            if (idIt != instanceGuidToId.end()) {
                return idIt->second;
            }
        }
    }
    
    // Try modelGuid + modelNodePath resolution
    if (meta.modelGuid.high != 0 || meta.modelGuid.low != 0) {
        // Find model root within the prefab hierarchy that has matching model asset GUID
        EntityID modelRoot = INVALID_ENTITY_ID;
        int modelRootMatches = 0;

        // Prefer explicit model instance hint when present (disambiguates duplicate model GUIDs).
        if (meta.modelRootGuid.high != 0 || meta.modelRootGuid.low != 0) {
            ClaymoreGUID lookupGuid = meta.modelRootGuid;
            auto mapped = prefabToInstanceGuid.find(meta.modelRootGuid);
            if (mapped != prefabToInstanceGuid.end()) {
                lookupGuid = mapped->second;
            }
            auto hintedIt = instanceGuidToId.find(lookupGuid);
            if (hintedIt != instanceGuidToId.end()) {
                EntityID hintedRoot = hintedIt->second;
                auto* hintedData = scene.GetEntityData(hintedRoot);
                if (hintedData &&
                    hintedData->ModelAssetGuid.high == meta.modelGuid.high &&
                    hintedData->ModelAssetGuid.low == meta.modelGuid.low) {
                    modelRoot = hintedRoot;
                    modelRootMatches = 1;
                }
            }
        }

        if (modelRoot == INVALID_ENTITY_ID) {
            std::function<void(EntityID)> findModelRoot = [&](EntityID id) {
                auto* d = scene.GetEntityData(id);
                if (!d) return;

                if (d->ModelAssetGuid.high == meta.modelGuid.high &&
                    d->ModelAssetGuid.low == meta.modelGuid.low) {
                    if (modelRoot == INVALID_ENTITY_ID) {
                        modelRoot = id;
                    }
                    ++modelRootMatches;
                }

                for (EntityID c : d->Children) findModelRoot(c);
            };
            findModelRoot(prefabRoot);
        }
        
        // Ambiguous model roots are non-deterministic without an instance discriminator.
        if (modelRootMatches > 1) {
            return INVALID_ENTITY_ID;
        }
        
        if (modelRoot != INVALID_ENTITY_ID) {
            // Empty path means the model root itself.
            if (meta.modelNodePath.empty()) {
                return modelRoot;
            }
            
            // Walk the node path from model root
            EntityID cur = modelRoot;
            std::stringstream ss(meta.modelNodePath);
            std::string seg;
            while (std::getline(ss, seg, '/')) {
                if (seg.empty()) continue;
                auto* d = scene.GetEntityData(cur);
                if (!d) { cur = INVALID_ENTITY_ID; break; }
                EntityID next = INVALID_ENTITY_ID;
                std::vector<EntityID> fuzzyCandidates;
                for (EntityID c : d->Children) {
                    auto* cd = scene.GetEntityData(c);
                    if (cd) {
                        if (cd->Name == seg) {
                            next = c;
                            break;
                        }
                        // Fuzzy match - strip _NNN suffix
                        size_t underscorePos = cd->Name.rfind('_');
                        if (underscorePos != std::string::npos) {
                            std::string baseName = cd->Name.substr(0, underscorePos);
                            size_t segUnderscorePos = seg.rfind('_');
                            std::string segBaseName = (segUnderscorePos != std::string::npos) 
                                ? seg.substr(0, segUnderscorePos) : seg;
                            if (baseName == segBaseName || baseName == seg) {
                                fuzzyCandidates.push_back(c);
                            }
                        }
                    }
                }
                if (next == INVALID_ENTITY_ID && fuzzyCandidates.size() == 1) {
                    next = fuzzyCandidates.front();
                }
                if (next == INVALID_ENTITY_ID) { cur = INVALID_ENTITY_ID; break; }
                cur = next;
            }
            return cur;
        }
    }
    
    return INVALID_ENTITY_ID;
}

// Resolve script entity references using ScriptEntityRefMetadata.
// This can run as a fallback when JSON is unavailable and as a strict correction pass
// after JSON resolution to fix valid-but-wrong IDs introduced by late delta writes.
static void ResolvePrefabScriptEntityReferencesFromMetadata(
    Scene& scene,
    EntityID prefabRoot,
    const std::unordered_map<ClaymoreGUID, ClaymoreGUID>& prefabToInstanceGuid,
    const std::unordered_map<ClaymoreGUID, EntityID>& instanceGuidToId)
{
    auto isEntityLikeType = [](PropertyType t) -> bool {
        return t == PropertyType::Entity || t == PropertyType::ComponentRef || t == PropertyType::ScriptRef;
    };
    auto isInPrefabSubtree = [&](EntityID candidate) -> bool {
        if (candidate == INVALID_ENTITY_ID) return false;
        EntityID cur = candidate;
        size_t guard = 0;
        while (cur != INVALID_ENTITY_ID && guard++ < 100000) {
            if (cur == prefabRoot) return true;
            auto* d = scene.GetEntityData(cur);
            if (!d || d->Parent == cur) break;
            cur = d->Parent;
        }
        return false;
    };
    auto logScriptRefMap = [&](EntityID ownerId,
                               const std::string& scriptClass,
                               const std::string& propName,
                               int oldVal,
                               int newVal,
                               const ScriptEntityRefMetadata* meta,
                               const char* reason) {
        auto* ownerData = scene.GetEntityData(ownerId);
        const std::string ownerName = ownerData ? ownerData->Name : "Unknown";
        PREFAB_LOG("[ScriptRefMap] stage=resolve-metadata"
            << " owner=" << ownerId << "(" << ownerName << ")"
            << " script=" << scriptClass
            << " prop=" << propName
            << " old=" << oldVal << "(" << DescribeEntityRefTarget(scene, oldVal) << ")"
            << " new=" << newVal << "(" << DescribeEntityRefTarget(scene, newVal) << ")"
            << " newInPrefab=" << (isInPrefabSubtree(static_cast<EntityID>(newVal)) ? "true" : "false")
            << " reason=" << reason
            << " " << DescribeEntityRefMeta(meta));
    };
    
    std::function<void(EntityID)> processEntity = [&](EntityID id) {
        auto* data = scene.GetEntityData(id);
        if (!data) return;
        
        for (auto& script : data->Scripts) {
            // Process single entity references using metadata
            for (auto& [key, value] : script.Values) {
                auto metaIt = script.EntityRefMetadata.find(key);
                if (metaIt == script.EntityRefMetadata.end()) continue;
                
                ScriptEntityRefMetadata& meta = metaIt->second;
                if (std::holds_alternative<int>(value)) {
                    int curVal = std::get<int>(value);
                    EntityID resolved = ResolveEntityRefFromMetadata(
                        scene, prefabRoot, meta, prefabToInstanceGuid, instanceGuidToId);
                    const bool curValid = curVal > 0 && curVal != static_cast<int>(INVALID_ENTITY_ID);
                    const bool curInPrefab = curValid && isInPrefabSubtree(static_cast<EntityID>(curVal));
                    const bool resolvedInPrefab = resolved != INVALID_ENTITY_ID && isInPrefabSubtree(resolved);

                    if (resolvedInPrefab) {
                        int newVal = static_cast<int>(resolved);
                        if (!curInPrefab || curVal != newVal) {
                            script.Values[key] = newVal;
                            meta.entityId = newVal;
                            const char* reason = !curValid
                                ? "metadata-single-resolved"
                                : (curInPrefab ? "metadata-single-mismatch-fixed" : "metadata-single-outside-prefab-fixed");
                            logScriptRefMap(id, script.ClassName, key, curVal, newVal, &meta, reason);
                        }
                    } else {
                        if (!curInPrefab) {
                            logScriptRefMap(id, script.ClassName, key, curVal, curVal, &meta, "metadata-single-unresolved");
                        }
                    }
                }
            }
            
            // Process list entity references using metadata
            for (auto& [key, value] : script.Values) {
                if (std::holds_alternative<std::shared_ptr<ListPropertyValue>>(value)) {
                    auto listPtr = std::get<std::shared_ptr<ListPropertyValue>>(value);
                    if (!listPtr || !isEntityLikeType(listPtr->elementType)) continue;
                    
                    for (size_t i = 0; i < listPtr->elements.size(); ++i) {
                        if (!std::holds_alternative<int>(listPtr->elements[i])) continue;
                        
                        int curVal = std::get<int>(listPtr->elements[i]);
                        
                        if (i < listPtr->entityRefs.size()) {
                            EntityID resolved = ResolveEntityRefFromMetadata(
                                scene, prefabRoot, listPtr->entityRefs[i], prefabToInstanceGuid, instanceGuidToId);
                            const bool curValid = curVal > 0 && curVal != static_cast<int>(INVALID_ENTITY_ID);
                            const bool curInPrefab = curValid && isInPrefabSubtree(static_cast<EntityID>(curVal));
                            const bool resolvedInPrefab = resolved != INVALID_ENTITY_ID && isInPrefabSubtree(resolved);
                            if (resolvedInPrefab) {
                                int newVal = static_cast<int>(resolved);
                                if (!curInPrefab || curVal != newVal) {
                                    listPtr->elements[i] = newVal;
                                    listPtr->entityRefs[i].entityId = newVal;
                                    const char* reason = !curValid
                                        ? "metadata-list-resolved"
                                        : (curInPrefab ? "metadata-list-mismatch-fixed" : "metadata-list-outside-prefab-fixed");
                                    logScriptRefMap(id, script.ClassName, key + "[" + std::to_string(i) + "]", curVal, newVal, &listPtr->entityRefs[i], reason);
                                }
                            } else {
                                if (!curInPrefab) {
                                    logScriptRefMap(id, script.ClassName, key + "[" + std::to_string(i) + "]", curVal, curVal, &listPtr->entityRefs[i], "metadata-list-unresolved");
                                }
                            }
                        }
                    }
                }
            }
        }
        
        for (EntityID c : data->Children) {
            processEntity(c);
        }
    };
    
    processEntity(prefabRoot);
}

// Resolve script entity references within a prefab hierarchy using GUID remapping
void ResolvePrefabScriptEntityReferences(
    Scene& scene,
    EntityID prefabRoot,
    const nlohmann::json& prefabJson,
    const std::unordered_map<ClaymoreGUID, ClaymoreGUID>& prefabToInstanceGuid,
    const std::unordered_map<ClaymoreGUID, EntityID>& instanceGuidToId,
    const std::vector<EntityID>* createdEntityIds)
{
    // FIX CASE 4: If JSON not available, use metadata-based fallback resolution
    if (!prefabJson.is_array() || prefabJson.empty()) {
        PREFAB_LOG("[ScriptRefMap] stage=resolve-json root=" << prefabRoot
            << " mode=metadata-fallback reason=no-prefab-json");
        ResolvePrefabScriptEntityReferencesFromMetadata(scene, prefabRoot, prefabToInstanceGuid, instanceGuidToId);
        return;
    }
    
    // Helper to check if a type is entity-like
    auto isEntityLikeType = [](PropertyType t) -> bool {
        return t == PropertyType::Entity || t == PropertyType::ComponentRef || t == PropertyType::ScriptRef;
    };

    auto parsePathSegment = [](const std::string& token, std::string& outName, int& outIndex) {
        outName = token;
        outIndex = 0;
        const size_t lb = token.find_last_of('[');
        const size_t rb = token.find_last_of(']');
        if (lb == std::string::npos || rb == std::string::npos || rb <= lb + 1 || rb != token.size() - 1) {
            return;
        }
        const std::string idxStr = token.substr(lb + 1, rb - lb - 1);
        bool allDigits = !idxStr.empty();
        for (char c : idxStr) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                allDigits = false;
                break;
            }
        }
        if (!allDigits) return;
        try {
            outIndex = std::stoi(idxStr);
            outName = token.substr(0, lb);
        } catch (...) {
            outIndex = 0;
            outName = token;
        }
    };

    auto findChildByNameAndIndex = [&](EntityID parent, const std::string& name, int siblingIndex) -> EntityID {
        auto* pd = scene.GetEntityData(parent);
        if (!pd) return INVALID_ENTITY_ID;
        int matchCount = 0;
        for (EntityID c : pd->Children) {
            auto* cd = scene.GetEntityData(c);
            if (cd && cd->Name == name) {
                if (matchCount == siblingIndex) return c;
                ++matchCount;
            }
        }
        // Fallback to first match if specific sibling index is unavailable.
        for (EntityID c : pd->Children) {
            auto* cd = scene.GetEntityData(c);
            if (cd && cd->Name == name) return c;
        }
        return INVALID_ENTITY_ID;
    };

    auto isDescendantOf = [&](EntityID node, EntityID ancestor) -> bool {
        if (node == INVALID_ENTITY_ID || ancestor == INVALID_ENTITY_ID) return false;
        EntityID cur = node;
        size_t guard = 0;
        while (cur != INVALID_ENTITY_ID && guard++ < 100000) {
            if (cur == ancestor) return true;
            auto* d = scene.GetEntityData(cur);
            if (!d || d->Parent == cur) break;
            cur = d->Parent;
        }
        return false;
    };

    auto resolvePrefabNodePath = [&](const nlohmann::json& refJson) -> EntityID {
        if (!refJson.is_object() || !refJson.contains("prefabNodePath")) {
            return INVALID_ENTITY_ID;
        }

        EntityID localRoot = prefabRoot;
        if (refJson.contains("prefabRootGuid")) {
            try {
                ClaymoreGUID rootGuid = ClaymoreGUID::FromString(refJson["prefabRootGuid"].get<std::string>());
                if (rootGuid.high != 0 || rootGuid.low != 0) {
                    ClaymoreGUID lookupGuid = rootGuid;
                    auto mapped = prefabToInstanceGuid.find(rootGuid);
                    if (mapped != prefabToInstanceGuid.end()) {
                        lookupGuid = mapped->second;
                    }
                    auto idIt = instanceGuidToId.find(lookupGuid);
                    if (idIt != instanceGuidToId.end() && isDescendantOf(idIt->second, prefabRoot)) {
                        localRoot = idIt->second;
                    }
                }
            } catch (...) {}
        }

        std::string path;
        try {
            path = refJson["prefabNodePath"].get<std::string>();
        } catch (...) {
            return INVALID_ENTITY_ID;
        }

        if (path.empty()) {
            return localRoot;
        }

        EntityID cur = localRoot;
        std::stringstream ss(path);
        std::string token;
        while (std::getline(ss, token, '/')) {
            if (token.empty()) continue;
            std::string name;
            int idx = 0;
            parsePathSegment(token, name, idx);
            cur = findChildByNameAndIndex(cur, name, idx);
            if (cur == INVALID_ENTITY_ID) {
                return INVALID_ENTITY_ID;
            }
        }

        return isDescendantOf(cur, prefabRoot) ? cur : INVALID_ENTITY_ID;
    };
    
    // Helper to resolve an entity reference JSON object within the prefab hierarchy
    // NOTE: This lambda accesses scene data, so it must only be called when entities are fully created
    auto resolveWithinPrefab = [&](const nlohmann::json& refJson) -> EntityID {
        // Safety check: ensure scene and prefab root are valid
        if (prefabRoot == INVALID_ENTITY_ID || !scene.GetEntityData(prefabRoot)) {
            return INVALID_ENTITY_ID;
        }
        
        if (refJson.is_string()) {
            std::string str = refJson.get<std::string>();
            if (str.empty()) return INVALID_ENTITY_ID;
            // Try parsing as GUID
            try {
                ClaymoreGUID g = ClaymoreGUID::FromString(str);
                if (!(g.high == 0 && g.low == 0)) {
                    // Map through prefabToInstanceGuid
                    auto it = prefabToInstanceGuid.find(g);
                    if (it != prefabToInstanceGuid.end()) {
                        auto idIt = instanceGuidToId.find(it->second);
                        if (idIt != instanceGuidToId.end()) {
                            return idIt->second;
                        }
                    }
                }
            } catch (...) {}
            return INVALID_ENTITY_ID;
        }
        
        if (!refJson.is_object()) return INVALID_ENTITY_ID;
        
        // Try entity GUID first (mapped through prefabToInstanceGuid)
        if (refJson.contains("guid")) {
            try {
                std::string gstr = refJson["guid"].get<std::string>();
                ClaymoreGUID g = ClaymoreGUID::FromString(gstr);
                if (!(g.high == 0 && g.low == 0)) {
                    auto it = prefabToInstanceGuid.find(g);
                    if (it != prefabToInstanceGuid.end()) {
                        auto idIt = instanceGuidToId.find(it->second);
                        if (idIt != instanceGuidToId.end()) {
                            return idIt->second;
                        }
                    }
                }
            } catch (...) {}
        }

        // Prefer prefab-local authored path before model/scene fallbacks. This is
        // the self-contained identity emitted when saving a scene subtree as a prefab.
        if (refJson.contains("prefabNodePath")) {
            EntityID byPrefabPath = resolvePrefabNodePath(refJson);
            if (byPrefabPath != INVALID_ENTITY_ID) {
                return byPrefabPath;
            }
        }
        
        // Try modelGuid + modelNodePath (search within prefab hierarchy)
        if (refJson.contains("modelGuid") && refJson.contains("modelNodePath")) {
            try {
                std::string mgstr = refJson["modelGuid"].get<std::string>();
                std::string nodePath = refJson["modelNodePath"].get<std::string>();
                ClaymoreGUID modelAssetGuid = ClaymoreGUID::FromString(mgstr);
                
                if (!(modelAssetGuid.high == 0 && modelAssetGuid.low == 0)) {
                    EntityID modelRoot = INVALID_ENTITY_ID;
                    bool usedModelRootGuidHint = false;
                    int modelRootMatches = 0;

                    // Prefer the exact model instance when authoring metadata provides modelRootGuid.
                    if (refJson.contains("modelRootGuid")) {
                        try {
                            ClaymoreGUID modelRootGuid = ClaymoreGUID::FromString(refJson["modelRootGuid"].get<std::string>());
                            if (!(modelRootGuid.high == 0 && modelRootGuid.low == 0)) {
                                ClaymoreGUID lookupGuid = modelRootGuid;
                                auto mapped = prefabToInstanceGuid.find(modelRootGuid);
                                if (mapped != prefabToInstanceGuid.end()) {
                                    lookupGuid = mapped->second;
                                }
                                auto idIt = instanceGuidToId.find(lookupGuid);
                                if (idIt != instanceGuidToId.end() && isDescendantOf(idIt->second, prefabRoot)) {
                                    modelRoot = idIt->second;
                                    usedModelRootGuidHint = true;
                                }
                            }
                        } catch (...) {}
                    }

                    // Fallback: find first matching model root in prefab hierarchy.
                    // This is deterministic because traversal starts at prefabRoot.
                    if (modelRoot == INVALID_ENTITY_ID) {
                        std::function<void(EntityID)> findModelRoot = [&](EntityID id) {
                            if (id == INVALID_ENTITY_ID) return;
                            auto* d = scene.GetEntityData(id);
                            if (!d) return;
                            
                            if (d->ModelAssetGuid.high == modelAssetGuid.high && 
                                d->ModelAssetGuid.low == modelAssetGuid.low) {
                                if (modelRoot == INVALID_ENTITY_ID) {
                                    modelRoot = id;
                                }
                                ++modelRootMatches;
                            }
                            
                            for (EntityID c : d->Children) findModelRoot(c);
                        };
                        findModelRoot(prefabRoot);
                    }

                    if (!usedModelRootGuidHint && modelRootMatches > 1) {
                        return INVALID_ENTITY_ID;
                    }
                    
                    if (modelRoot != INVALID_ENTITY_ID) {
                        // Walk the node path from model root
                        EntityID cur = modelRoot;
                        if (!nodePath.empty()) {
                            std::stringstream ss(nodePath);
                            std::string seg;
                            while (std::getline(ss, seg, '/')) {
                                if (seg.empty()) continue;
                                auto* d = scene.GetEntityData(cur);
                                if (!d) { cur = INVALID_ENTITY_ID; break; }
                                EntityID next = INVALID_ENTITY_ID;
                                std::vector<EntityID> fuzzyCandidates;
                                for (EntityID c : d->Children) {
                                    auto* cd = scene.GetEntityData(c);
                                    // Match with or without numeric suffix
                                    if (cd) {
                                        if (cd->Name == seg) {
                                            next = c;
                                            break;
                                        }
                                        // Fuzzy match - strip _NNN suffix
                                        size_t underscorePos = cd->Name.rfind('_');
                                        if (underscorePos != std::string::npos) {
                                            std::string baseName = cd->Name.substr(0, underscorePos);
                                            size_t segUnderscorePos = seg.rfind('_');
                                            std::string segBaseName = (segUnderscorePos != std::string::npos) 
                                                ? seg.substr(0, segUnderscorePos) : seg;
                                            if (baseName == segBaseName || baseName == seg) {
                                                fuzzyCandidates.push_back(c);
                                            }
                                        }
                                    }
                                }
                                if (next == INVALID_ENTITY_ID && fuzzyCandidates.size() == 1) {
                                    next = fuzzyCandidates.front();
                                }
                                if (next == INVALID_ENTITY_ID) { cur = INVALID_ENTITY_ID; break; }
                                cur = next;
                            }
                        }
                        return cur;
                    }
                }
            } catch (...) {}
        }

        // Try scene path (supports sibling indices like Root/Child[1]/Leaf)
        // Resolve strictly within prefab subtree for deterministic internal refs.
        if (refJson.contains("scenePath")) {
            try {
                std::string sp = refJson["scenePath"].get<std::string>();
                if (!sp.empty()) {
                    std::vector<std::pair<std::string, int>> segments;
                    std::stringstream ss(sp);
                    std::string token;
                    while (std::getline(ss, token, '/')) {
                        if (token.empty()) continue;
                        std::string name;
                        int idx = 0;
                        parsePathSegment(token, name, idx);
                        segments.push_back({name, idx});
                    }

                    if (!segments.empty()) {
                        EntityID cur = prefabRoot;
                        auto* rootData = scene.GetEntityData(prefabRoot);
                        const std::string rootName = rootData ? rootData->Name : "";

                        size_t start = 0;
                        // Full scene path commonly starts with prefab root name.
                        if (!rootName.empty() && segments[0].first == rootName) {
                            start = 1;
                        }

                        bool ok = true;
                        for (size_t i = start; i < segments.size() && ok; ++i) {
                            EntityID next = findChildByNameAndIndex(cur, segments[i].first, segments[i].second);
                            if (next == INVALID_ENTITY_ID) {
                                ok = false;
                            } else {
                                cur = next;
                            }
                        }
                        if (ok && isDescendantOf(cur, prefabRoot)) {
                            return cur;
                        }
                    }
                }
            } catch (...) {}
        }
        
        return INVALID_ENTITY_ID;
    };
    
    // Build a map from prefab GUID -> entity JSON index
    std::unordered_map<std::string, size_t> guidToJsonIndex;
    for (size_t i = 0; i < prefabJson.size(); ++i) {
        const auto& entityJson = prefabJson[i];
        if (entityJson.contains("guid")) {
            try {
                std::string gstr = entityJson["guid"].get<std::string>();
                guidToJsonIndex[gstr] = i;
            } catch (...) {}
        }
    }
    
    // Build set of prefab entity IDs for validation
    std::unordered_set<EntityID> prefabEntityIds;
    if (createdEntityIds) {
        for (EntityID id : *createdEntityIds) {
            prefabEntityIds.insert(id);
        }
    }
    auto isPrefabEntityValue = [&](int value) -> bool {
        return IsValidEntityRefValue(value) &&
               prefabEntityIds.find(static_cast<EntityID>(value)) != prefabEntityIds.end();
    };
    auto logScriptRefMap = [&](const char* stage,
                               EntityID ownerId,
                               const std::string& scriptClass,
                               const std::string& propName,
                               int oldVal,
                               int newVal,
                               const ScriptEntityRefMetadata* meta,
                               const char* reason) {
        auto* ownerData = scene.GetEntityData(ownerId);
        const std::string ownerName = ownerData ? ownerData->Name : "Unknown";
        PREFAB_LOG("[ScriptRefMap] stage=" << stage
            << " owner=" << ownerId << "(" << ownerName << ")"
            << " script=" << scriptClass
            << " prop=" << propName
            << " old=" << oldVal << "(" << DescribeEntityRefTarget(scene, oldVal) << ")"
            << " new=" << newVal << "(" << DescribeEntityRefTarget(scene, newVal) << ")"
            << " oldInPrefab=" << (isPrefabEntityValue(oldVal) ? "true" : "false")
            << " newInPrefab=" << (isPrefabEntityValue(newVal) ? "true" : "false")
            << " reason=" << reason
            << " " << DescribeEntityRefMeta(meta));
    };
    
    // Process all entities in the prefab hierarchy
    std::function<void(EntityID)> processEntity = [&](EntityID id) {
        auto* data = scene.GetEntityData(id);
        if (!data) return;
        
        // Find this entity's original prefab JSON by matching instance GUID back to prefab GUID
        ClaymoreGUID instanceGuid = data->EntityGuid;
        const nlohmann::json* entityJson = nullptr;
        
        // Find prefab GUID that maps to this instance GUID
        bool foundGuidMapping = false;
        for (const auto& [prefabGuid, instGuid] : prefabToInstanceGuid) {
            if (instGuid.high == instanceGuid.high && instGuid.low == instanceGuid.low) {
                foundGuidMapping = true;
                std::string gstr = prefabGuid.ToString();
                auto it = guidToJsonIndex.find(gstr);
                if (it != guidToJsonIndex.end()) {
                    entityJson = &prefabJson[it->second];
                } else {
                    // Root entity might not have GUID in JSON if it was created differently.
                    // Try to find by matching first entry/name as a quiet fallback.
                    if (id == prefabRoot) {
                        if (!prefabJson.empty()) {
                            const auto& firstEntity = prefabJson[0];
                            if (firstEntity.contains("guid")) {
                                try {
                                    std::string firstGuidStr = firstEntity["guid"].get<std::string>();
                                    ClaymoreGUID firstGuid = ClaymoreGUID::FromString(firstGuidStr);
                                    if (firstGuid.high == prefabGuid.high && firstGuid.low == prefabGuid.low) {
                                        entityJson = &firstEntity;
                                    }
                                } catch (...) {}
                            }
                            if (!entityJson && firstEntity.contains("name")) {
                                std::string firstName = firstEntity.value("name", "");
                                if (firstName == data->Name || firstName.empty()) {
                                    entityJson = &firstEntity;
                                }
                            }
                        }
                    }
                }
                break;
            }
        }
        
        // If root entity GUID mapping wasn't found, mapping quality may degrade for JSON-only references.
        if (id == prefabRoot && !foundGuidMapping) {
            PREFAB_LOG_WARN("[ScriptRefMap] stage=resolve-json owner=" << id
                << " reason=missing-prefab-guid-mapping guid=" << instanceGuid.ToString());
        }
        
        // Determine if this is the root entity (needed for special handling)
        bool isRootEntity = (id == prefabRoot);
        
        if (entityJson && entityJson->contains("scripts") && (*entityJson)["scripts"].is_array()) {
            const auto& scriptsJson = (*entityJson)["scripts"];

            // Build runtime script index buckets by class name so remapping remains stable even when
            // scripts are removed/reordered. Index-based matching is fragile and can shift references.
            std::unordered_map<std::string, std::vector<size_t>> runtimeScriptsByClass;
            runtimeScriptsByClass.reserve(data->Scripts.size());
            for (size_t i = 0; i < data->Scripts.size(); ++i) {
                runtimeScriptsByClass[data->Scripts[i].ClassName].push_back(i);
            }
            std::unordered_map<std::string, size_t> runtimeClassCursor;
            runtimeClassCursor.reserve(runtimeScriptsByClass.size());

            for (size_t scriptIdx = 0; scriptIdx < scriptsJson.size(); ++scriptIdx) {
                const auto& scriptJson = scriptsJson[scriptIdx];
                if (!scriptJson.contains("className") || !scriptJson.contains("properties")) continue;
                std::string className = scriptJson["className"].get<std::string>();

                auto bucketIt = runtimeScriptsByClass.find(className);
                if (bucketIt == runtimeScriptsByClass.end() || bucketIt->second.empty()) {
                    if (isRootEntity) {
                        PREFAB_LOG_WARN("[ScriptRefMap] stage=resolve-json owner=" << id
                            << " script=" << className
                            << " reason=runtime-script-class-missing");
                    }
                    continue;
                }
                size_t& classCursor = runtimeClassCursor[className];
                if (classCursor >= bucketIt->second.size()) {
                    if (isRootEntity) {
                        PREFAB_LOG_WARN("[ScriptRefMap] stage=resolve-json owner=" << id
                            << " script=" << className
                            << " reason=runtime-script-class-occurrence-missing idx=" << classCursor);
                    }
                    continue;
                }

                const size_t runtimeScriptIndex = bucketIt->second[classCursor++];
                auto& script = data->Scripts[runtimeScriptIndex];
                
                const auto& propsJson = scriptJson["properties"];
                if (!propsJson.is_object()) continue;
                
                // Get reflection metadata
                if (!ScriptReflection::HasProperties(className)) {
                    if (isRootEntity) {
                        PREFAB_LOG_WARN("[ScriptRefMap] stage=resolve-json owner=" << id
                            << " script=" << className
                            << " reason=missing-reflection-metadata");
                    }
                    continue;
                }
                auto& properties = ScriptReflection::GetScriptProperties(className);
                
                // CRITICAL FIX: For root entity, process ALL entity reference properties, even if they point to scene entities
                // This catches cases where RemapPrefabEntityReferences incorrectly mapped to scene entities
                // NOTE: Only process if we have valid JSON and createdEntityIds list, and script data is ready
                if (isRootEntity && !propsJson.empty() && createdEntityIds && !createdEntityIds->empty() && 
                    prefabRoot != INVALID_ENTITY_ID && scene.GetEntityData(prefabRoot)) {
                    // First, check all properties from reflection to see if any point to scene entities
                    for (const auto& prop : properties) {
                        if (!isEntityLikeType(prop.type)) continue;
                        auto it = script.Values.find(prop.name);
                        if (it == script.Values.end() || !std::holds_alternative<int>(it->second)) continue;
                        
                        int val = std::get<int>(it->second);
                        if (IsValidEntityRefValue(val) && !isPrefabEntityValue(val) && propsJson.contains(prop.name)) {
                            ScriptEntityRefMetadata* metaPtr = nullptr;
                            auto metaIt = script.EntityRefMetadata.find(prop.name);
                            if (metaIt != script.EntityRefMetadata.end()) metaPtr = &metaIt->second;
                            int newVal = -1;
                            try {
                                const auto& propValue = propsJson[prop.name];
                                EntityID resolved = resolveWithinPrefab(propValue);
                                if (resolved != INVALID_ENTITY_ID && isPrefabEntityValue(static_cast<int>(resolved))) {
                                    newVal = static_cast<int>(resolved);
                                }
                            } catch (...) {
                                newVal = -1;
                            }
                            script.Values[prop.name] = newVal;
                            if (metaPtr) metaPtr->entityId = newVal;
                            logScriptRefMap(
                                "resolve-json",
                                id,
                                script.ClassName,
                                prop.name,
                                val,
                                newVal,
                                metaPtr,
                                (newVal == -1) ? "root-precheck-scene-bound-unresolved" : "root-precheck-scene-bound-fixed");
                        }
                    }
                }
                
                for (const auto& prop : properties) {
                    if (!propsJson.contains(prop.name)) continue;
                    const auto& propValue = propsJson[prop.name];
                    
                    // Handle single entity reference
                    if (isEntityLikeType(prop.type)) {
                        auto it = script.Values.find(prop.name);
                        if (it != script.Values.end() && std::holds_alternative<int>(it->second)) {
                            int curVal = std::get<int>(it->second);
                            ScriptEntityRefMetadata* metaPtr = nullptr;
                            auto metaIt = script.EntityRefMetadata.find(prop.name);
                            if (metaIt != script.EntityRefMetadata.end()) metaPtr = &metaIt->second;

                            bool needsResolution = false;
                            const char* resolutionReason = "unchanged";
                            
                            if (IsValidEntityRefValue(curVal)) {
                                // If value points outside prefab, it must be fixed.
                                if (!isPrefabEntityValue(curVal)) {
                                    needsResolution = true;
                                    resolutionReason = "current-outside-prefab";
                                } else {
                                    // Value is prefab-local, but still verify against JSON target to catch
                                    // wrong-but-valid remaps (e.g. old ID collision mapped to a bone).
                                    EntityID expected = resolveWithinPrefab(propValue);
                                    bool expectedInPrefab = (expected != INVALID_ENTITY_ID) &&
                                        (prefabEntityIds.find(expected) != prefabEntityIds.end());
                                    if (expectedInPrefab && expected != static_cast<EntityID>(curVal)) {
                                        needsResolution = true;
                                        resolutionReason = "prefab-local-mismatch-vs-json";
                                    }
                                }
                            } else {
                                // Invalid (-1) or unresolved - needs resolution
                                needsResolution = true;
                                resolutionReason = "current-unresolved";
                            }
                            
                            if (needsResolution) {
                                // Resolve from prefab JSON (has full GUID/modelGuid/modelNodePath info)
                                EntityID resolved = resolveWithinPrefab(propValue);
                                int newVal = -1;
                                if (resolved != INVALID_ENTITY_ID && isPrefabEntityValue(static_cast<int>(resolved))) {
                                    newVal = static_cast<int>(resolved);
                                }
                                script.Values[prop.name] = newVal;
                                if (metaPtr) metaPtr->entityId = newVal;
                                logScriptRefMap(
                                    "resolve-json",
                                    id,
                                    script.ClassName,
                                    prop.name,
                                    curVal,
                                    newVal,
                                    metaPtr,
                                    resolutionReason);
                            }

                            // Metadata-guid/path is the most stable identity for prefab refs.
                            // Re-apply it after JSON resolution to fix late delta writes that reintroduce authoring IDs.
                            if (metaPtr && HasEntityRefHints(metaPtr)) {
                                int curAfter = curVal;
                                auto curIt = script.Values.find(prop.name);
                                if (curIt != script.Values.end() && std::holds_alternative<int>(curIt->second)) {
                                    curAfter = std::get<int>(curIt->second);
                                }

                                EntityID metaResolved = ResolveEntityRefFromMetadata(
                                    scene,
                                    prefabRoot,
                                    *metaPtr,
                                    prefabToInstanceGuid,
                                    instanceGuidToId);

                                if (metaResolved != INVALID_ENTITY_ID &&
                                    isPrefabEntityValue(static_cast<int>(metaResolved)) &&
                                    curAfter != static_cast<int>(metaResolved)) {
                                    script.Values[prop.name] = static_cast<int>(metaResolved);
                                    metaPtr->entityId = static_cast<int>(metaResolved);
                                    logScriptRefMap(
                                        "resolve-json",
                                        id,
                                        script.ClassName,
                                        prop.name,
                                        curAfter,
                                        static_cast<int>(metaResolved),
                                        metaPtr,
                                        "metadata-hint-override");
                                }
                            }
                        }
                    }
                    
                    // Handle list of entity references
                    if (prop.type == PropertyType::List && isEntityLikeType(prop.listElementType)) {
                        auto it = script.Values.find(prop.name);
                        if (it != script.Values.end() && 
                            std::holds_alternative<std::shared_ptr<ListPropertyValue>>(it->second)) {
                            auto listPtr = std::get<std::shared_ptr<ListPropertyValue>>(it->second);
                            if (listPtr && propValue.contains("elements") && propValue["elements"].is_array()) {
                                const auto& elementsJson = propValue["elements"];
                                for (size_t i = 0; i < listPtr->elements.size() && i < elementsJson.size(); ++i) {
                                    if (std::holds_alternative<int>(listPtr->elements[i])) {
                                        int curVal = std::get<int>(listPtr->elements[i]);
                                        ScriptEntityRefMetadata* elemMeta =
                                            (i < listPtr->entityRefs.size()) ? &listPtr->entityRefs[i] : nullptr;

                                        const EntityID curEntity = static_cast<EntityID>(curVal);
                                        const bool curIsPrefabEntity =
                                            (curVal > 0 && curVal != INVALID_ENTITY_ID &&
                                             prefabEntityIds.find(curEntity) != prefabEntityIds.end());

                                        // Resolve expected target from prefab JSON.
                                        EntityID resolved = resolveWithinPrefab(elementsJson[i]);
                                        const bool resolvedInPrefab =
                                            (resolved != INVALID_ENTITY_ID &&
                                             prefabEntityIds.find(resolved) != prefabEntityIds.end());

                                        if (resolvedInPrefab) {
                                            // Fix unresolved, scene-bound, or mismatched prefab-local entries.
                                            if (!curIsPrefabEntity || curEntity != resolved) {
                                                listPtr->elements[i] = static_cast<int>(resolved);
                                                if (elemMeta) elemMeta->entityId = static_cast<int>(resolved);
                                                logScriptRefMap(
                                                    "resolve-json",
                                                    id,
                                                    script.ClassName,
                                                    prop.name + "[" + std::to_string(i) + "]",
                                                    curVal,
                                                    static_cast<int>(resolved),
                                                    elemMeta,
                                                    curIsPrefabEntity ? "list-prefab-mismatch-vs-json" : "list-outside-prefab-or-unresolved");
                                            }
                                        } else if (!curIsPrefabEntity) {
                                            // Avoid leaving list refs bound to scene entities when prefab-local resolution fails.
                                            listPtr->elements[i] = -1;
                                            if (elemMeta) elemMeta->entityId = -1;
                                            logScriptRefMap(
                                                "resolve-json",
                                                id,
                                                script.ClassName,
                                                prop.name + "[" + std::to_string(i) + "]",
                                                curVal,
                                                -1,
                                                elemMeta,
                                                "list-resolution-failed");
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // Process children
        for (EntityID c : data->Children) {
            processEntity(c);
        }
    };
    
    processEntity(prefabRoot);

    // Final strict metadata pass to correct valid-but-wrong IDs (e.g. delta reintroducing authoring IDs)
    // even when JSON payload still carries authoring-time numeric values.
    ResolvePrefabScriptEntityReferencesFromMetadata(
        scene,
        prefabRoot,
        prefabToInstanceGuid,
        instanceGuidToId);
}
