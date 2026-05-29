#include "InstancerSystem.h"
#include "InstancerComponent.h"
#include "core/spline/SplineUtils.h"
#include "Scene.h"
#include "EntityData.h"
#include "Components.h"
#include "core/rendering/Terrain.h"
#include "core/rendering/ShaderManager.h"
#include "core/rendering/Renderer.h"
#include "core/rendering/Material.h"
#include "core/rendering/MaterialManager.h"
#include "core/rendering/PBRMaterial.h"
#include "core/rendering/TextureLoader.h"
#include "core/rendering/GlobalShaderProperties.h"
#include "core/utils/Profiler.h"
#include "core/prefab/PrefabAPI.h"
#include "core/assets/AssetReference.h"
#include "core/physics/Physics.h"
#include "core/physics/PhysicsLayerManager.h"
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>

#ifdef CLAYMORE_RUNTIME
#include "core/serialization/MeshBinaryLoader.h"
#include "core/assets/IAssetResolver.h"
#else
#include "editor/pipeline/AssetLibrary.h"
#include "editor/pipeline/MeshBin.h"
#include "editor/pipeline/MaterialSourceSerialization.h"
#include "editor/pipeline/ModelImportCache.h"
#include "editor/Project.h"
#include "core/rendering/MaterialSource.h"
#include "core/rendering/MaterialCache.h"
#endif

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

namespace cm {
namespace instancer {


namespace {

constexpr float kInstancerMinShadowScreenSize = 0.005f;
// Dense terrain-backed instancers can easily have thousands of visible
// instances. Submitting only 256 per draw balloons draw-call counts on weaker
// GPUs, so use a larger batch and let bgfx clamp to available buffer space.
constexpr uint32_t kMaxInstancedSubmitCount = 1024u;
constexpr uint32_t kMaxInstancedShadowSubmitCount = kMaxInstancedSubmitCount;
constexpr uint32_t kMaxTerrainInstances = 200000u;
constexpr uint32_t kMaxTerrainInstancerGenerationCellsPerUpdate = 50000u;
constexpr float kMaxTerrainInstancerGenerationMsPerUpdate = 2.0f;

bool InstancerDiagnosticsEnabled()
{
    static const bool enabled = []() {
        const char* value = std::getenv("CLAYMORE_INSTANCER_DIAGNOSTICS");
        return value != nullptr && value[0] != '\0' && std::string(value) != "0";
    }();
    return enabled;
}

bool NearlyEqualVec3(const glm::vec3& a, const glm::vec3& b, float epsilon = 1e-3f)
{
    return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec3(epsilon)));
}

bool NearlyEqualMat4(const glm::mat4& a, const glm::mat4& b, float epsilon = 1e-4f)
{
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (std::abs(a[column][row] - b[column][row]) > epsilon) {
                return false;
            }
        }
    }
    return true;
}

bool ShouldRefreshInstancerVisibility(const InstancerComponent& comp,
                                      const glm::vec3& cameraPos,
                                      const glm::mat4& viewProj)
{
    if (!comp.VisibilityCacheValid) {
        return true;
    }

    return !NearlyEqualVec3(comp.LastVisibilityCameraPos, cameraPos) ||
           !NearlyEqualMat4(comp.LastVisibilityViewProj, viewProj);
}

void RecordInstancerVisibilityState(InstancerComponent& comp,
                                    const glm::vec3& cameraPos,
                                    const glm::mat4& viewProj)
{
    comp.LastVisibilityCameraPos = cameraPos;
    comp.LastVisibilityViewProj = viewProj;
    comp.VisibilityCacheValid = true;
}

class ScopedInstancerTimer {
public:
    explicit ScopedInstancerTimer(const char* label)
        : m_Label(label)
        , m_Enabled(InstancerDiagnosticsEnabled())
        , m_Start(std::chrono::high_resolution_clock::now())
    {
    }

    ~ScopedInstancerTimer()
    {
        if (!m_Enabled)
            return;

        const auto end = std::chrono::high_resolution_clock::now();
        const float elapsedMs = std::chrono::duration<float, std::milli>(end - m_Start).count();
        std::cout << "[InstancerDiag] " << m_Label << " " << elapsedMs << "ms\n";
    }

private:
    const char* m_Label = "";
    bool m_Enabled = false;
    std::chrono::high_resolution_clock::time_point m_Start;
};

struct SharedMeshCacheEntry {
    CachedMeshData Data;
    struct Dependency {
        std::string Path;
        std::filesystem::file_time_type WriteTime{};
        bool HasWriteTime = false;
    };
    std::vector<Dependency> Dependencies;
};

std::unordered_map<std::string, SharedMeshCacheEntry> s_SharedMeshCache;

std::string NormalizeCachePath(std::string path)
{
    for (char& c : path) {
        if (c == '\\') c = '/';
    }
    return path;
}

std::string MakeSharedMeshCacheKey(const ClaymoreGUID& guid, const std::string& meshPath, const std::string& metaPath)
{
    return guid.ToString() + "|" + NormalizeCachePath(meshPath) + "|" + NormalizeCachePath(metaPath);
}

bool TryGetWriteTime(const std::string& path, std::filesystem::file_time_type& outTime)
{
    if (path.empty()) {
        return false;
    }

    std::error_code ec;
    std::filesystem::path candidate(path);
    if (std::filesystem::exists(candidate, ec)) {
        outTime = std::filesystem::last_write_time(candidate, ec);
        return !ec;
    }

#ifndef CLAYMORE_RUNTIME
    const std::filesystem::path projectPath = Project::GetProjectDirectory() / candidate;
    ec.clear();
    if (std::filesystem::exists(projectPath, ec)) {
        outTime = std::filesystem::last_write_time(projectPath, ec);
        return !ec;
    }
#endif

    return false;
}

bool IsSharedMeshCacheEntryCurrent(const SharedMeshCacheEntry& entry)
{
    std::filesystem::file_time_type writeTime{};
    for (const SharedMeshCacheEntry::Dependency& dependency : entry.Dependencies) {
        if (dependency.Path.empty()) {
            continue;
        }
        if (dependency.HasWriteTime && (!TryGetWriteTime(dependency.Path, writeTime) || writeTime != dependency.WriteTime)) {
            return false;
        }
        if (!dependency.HasWriteTime && TryGetWriteTime(dependency.Path, writeTime)) {
            return false;
        }
    }
    return true;
}

bool TryUseSharedMeshCache(const std::string& key, InstancerComponent& comp)
{
    auto it = s_SharedMeshCache.find(key);
    if (it == s_SharedMeshCache.end()) {
        return false;
    }
    if (!IsSharedMeshCacheEntryCurrent(it->second)) {
        s_SharedMeshCache.erase(it);
        return false;
    }

    comp.CachedMesh = it->second.Data;
    comp.NeedsMeshReload = false;
    return comp.CachedMesh.Valid;
}

void AddSharedMeshCacheDependency(SharedMeshCacheEntry& entry, const std::string& path)
{
    if (path.empty()) {
        return;
    }

    const std::string normalized = NormalizeCachePath(path);
    auto duplicate = std::find_if(entry.Dependencies.begin(), entry.Dependencies.end(),
        [&normalized](const SharedMeshCacheEntry::Dependency& dependency) {
            return NormalizeCachePath(dependency.Path) == normalized;
        });
    if (duplicate != entry.Dependencies.end()) {
        return;
    }

    SharedMeshCacheEntry::Dependency dependency;
    dependency.Path = path;
    dependency.HasWriteTime = TryGetWriteTime(path, dependency.WriteTime);
    entry.Dependencies.push_back(std::move(dependency));
}

#ifndef CLAYMORE_RUNTIME
void AddMaterialSourceDependencies(std::vector<std::string>& dependencies, const MaterialSource& source)
{
    auto add = [&dependencies](const TextureSpecifier& specifier) {
        if (specifier.Path.empty()) {
            return;
        }
        const std::string normalized = NormalizeCachePath(specifier.Path);
        auto it = std::find_if(dependencies.begin(), dependencies.end(),
            [&normalized](const std::string& existing) {
                return NormalizeCachePath(existing) == normalized;
            });
        if (it == dependencies.end()) {
            dependencies.push_back(specifier.Path);
        }
    };

    add(source.Albedo);
    add(source.MetallicRoughness);
    add(source.Normal);
    add(source.AO);
    add(source.Emission);
    add(source.Displacement);
}
#endif

void StoreSharedMeshCache(
    const std::string& key,
    const std::string& sourcePath,
    const std::string& meshPath,
    const std::string& metaPath,
    const CachedMeshData& data,
    const std::vector<std::string>& extraDependencies = {})
{
    if (!data.Valid) {
        return;
    }

    SharedMeshCacheEntry entry;
    entry.Data = data;
    AddSharedMeshCacheDependency(entry, sourcePath);
    AddSharedMeshCacheDependency(entry, meshPath);
    AddSharedMeshCacheDependency(entry, metaPath);
    for (const std::string& dependency : extraDependencies) {
        AddSharedMeshCacheDependency(entry, dependency);
    }
    s_SharedMeshCache[key] = std::move(entry);
}

glm::mat4 ComposeInstanceTransform(const InstanceData& inst)
{
    glm::mat4 transform = glm::translate(glm::mat4(1.0f), inst.Position);
    transform = transform * glm::mat4_cast(inst.Rotation);
    transform = glm::scale(transform, inst.Scale);
    return transform;
}

float MaxAbsScaleComponent(const glm::vec3& scale)
{
    return glm::max(glm::max(std::abs(scale.x), std::abs(scale.y)), std::abs(scale.z));
}

float EstimateMaxInstanceScale(const InstancerComponent& comp)
{
    const DistributionSettings& settings = comp.Distribution;
    if (settings.NonUniformScale) {
        return glm::max(0.001f, MaxAbsScaleComponent(glm::max(glm::abs(settings.MinScaleVec), glm::abs(settings.MaxScaleVec))));
    }
    return glm::max(0.001f, glm::max(std::abs(settings.MinScale), std::abs(settings.MaxScale)));
}

uint32_t HashCell(uint32_t seed, uint32_t x, uint32_t z)
{
    uint32_t h = seed ^ 0x9e3779b9u;
    h ^= x + 0x85ebca6bu + (h << 6) + (h >> 2);
    h ^= z + 0xc2b2ae35u + (h << 6) + (h >> 2);
    return h;
}

float SampleU8MaskBilinear(const std::vector<uint8_t>& mask, uint32_t resolution, float localX, float localZ, const glm::vec2& worldSize)
{
    if (mask.size() != static_cast<size_t>(resolution) * resolution || resolution < 2 ||
        worldSize.x <= 0.0f || worldSize.y <= 0.0f) {
        return 0.0f;
    }

    const float maxCoord = static_cast<float>(resolution - 1);
    const float xf = glm::clamp((localX / worldSize.x) * maxCoord, 0.0f, maxCoord);
    const float zf = glm::clamp((localZ / worldSize.y) * maxCoord, 0.0f, maxCoord);
    const uint32_t x0 = static_cast<uint32_t>(std::floor(xf));
    const uint32_t z0 = static_cast<uint32_t>(std::floor(zf));
    const uint32_t x1 = std::min(x0 + 1, resolution - 1);
    const uint32_t z1 = std::min(z0 + 1, resolution - 1);
    const float tx = xf - static_cast<float>(x0);
    const float tz = zf - static_cast<float>(z0);

    auto sample = [&](uint32_t x, uint32_t z) {
        const size_t idx = static_cast<size_t>(z) * resolution + x;
        return idx < mask.size() ? static_cast<float>(mask[idx]) / 255.0f : 0.0f;
    };

    const float v00 = sample(x0, z0);
    const float v10 = sample(x1, z0);
    const float v01 = sample(x0, z1);
    const float v11 = sample(x1, z1);
    return glm::mix(glm::mix(v00, v10, tx), glm::mix(v01, v11, tx), tz);
}

float SampleSplatChannelBilinear(const TerrainComponent& terrain, int channel, float localX, float localZ)
{
    if (terrain.SplatMap.empty() || terrain.GridResolution < 2 ||
        terrain.WorldSize.x <= 0.0f || terrain.WorldSize.y <= 0.0f) {
        return 0.0f;
    }

    const uint32_t resolution = terrain.GridResolution;
    const float maxCoord = static_cast<float>(resolution - 1);
    const float xf = glm::clamp((localX / terrain.WorldSize.x) * maxCoord, 0.0f, maxCoord);
    const float zf = glm::clamp((localZ / terrain.WorldSize.y) * maxCoord, 0.0f, maxCoord);
    const uint32_t x0 = static_cast<uint32_t>(std::floor(xf));
    const uint32_t z0 = static_cast<uint32_t>(std::floor(zf));
    const uint32_t x1 = std::min(x0 + 1, resolution - 1);
    const uint32_t z1 = std::min(z0 + 1, resolution - 1);
    const float tx = xf - static_cast<float>(x0);
    const float tz = zf - static_cast<float>(z0);

    auto sample = [&](uint32_t x, uint32_t z) {
        const size_t idx = static_cast<size_t>(z) * resolution + x;
        if (idx >= terrain.SplatMap.size()) {
            return 0.0f;
        }
        const glm::u8vec4& px = terrain.SplatMap[idx];
        switch (channel) {
        case 0: return static_cast<float>(px.r) / 255.0f;
        case 1: return static_cast<float>(px.g) / 255.0f;
        case 2: return static_cast<float>(px.b) / 255.0f;
        case 3: return static_cast<float>(px.a) / 255.0f;
        default: return 0.0f;
        }
    };

    const float v00 = sample(x0, z0);
    const float v10 = sample(x1, z0);
    const float v01 = sample(x0, z1);
    const float v11 = sample(x1, z1);
    return glm::mix(glm::mix(v00, v10, tx), glm::mix(v01, v11, tx), tz);
}

float SampleTerrainInstancerMask(const TerrainComponent& terrain, TerrainInstancerLayerDesc& layer, float localX, float localZ)
{
    if (layer.Mask == TerrainInstancerMaskSource::Painted) {
        return SampleU8MaskBilinear(layer.PaintedMask, terrain.GridResolution, localX, localZ, terrain.WorldSize);
    }

    int channel = -1;
    switch (layer.Mask) {
    case TerrainInstancerMaskSource::SplatR: channel = 0; break;
    case TerrainInstancerMaskSource::SplatG: channel = 1; break;
    case TerrainInstancerMaskSource::SplatB: channel = 2; break;
    case TerrainInstancerMaskSource::SplatA: channel = 3; break;
    default: break;
    }

    const float raw = SampleSplatChannelBilinear(terrain, channel, localX, localZ);
    const float threshold = glm::clamp(layer.SplatThreshold, 0.0f, 0.99f);
    return glm::clamp((raw - threshold) / glm::max(0.001f, 1.0f - threshold), 0.0f, 1.0f);
}

void ApplyTerrainInstancerVariation(
    InstanceData& inst,
    const DistributionSettings& settings,
    const glm::vec3& terrainNormal,
    std::mt19937& rng)
{
    if (settings.NonUniformScale) {
        const glm::vec3 minScale = glm::min(settings.MinScaleVec, settings.MaxScaleVec);
        const glm::vec3 maxScale = glm::max(settings.MinScaleVec, settings.MaxScaleVec);
        std::uniform_real_distribution<float> scaleX(minScale.x, maxScale.x);
        std::uniform_real_distribution<float> scaleY(minScale.y, maxScale.y);
        std::uniform_real_distribution<float> scaleZ(minScale.z, maxScale.z);
        inst.Scale = glm::vec3(scaleX(rng), scaleY(rng), scaleZ(rng));
    } else {
        std::uniform_real_distribution<float> scale(glm::min(settings.MinScale, settings.MaxScale), glm::max(settings.MinScale, settings.MaxScale));
        inst.Scale = glm::vec3(glm::max(0.001f, scale(rng)));
    }

    std::uniform_real_distribution<float> yaw(-settings.YawVarianceDegrees * 0.5f, settings.YawVarianceDegrees * 0.5f);
    std::uniform_real_distribution<float> pitch(-settings.PitchVarianceDegrees * 0.5f, settings.PitchVarianceDegrees * 0.5f);
    std::uniform_real_distribution<float> roll(-settings.RollVarianceDegrees * 0.5f, settings.RollVarianceDegrees * 0.5f);
    inst.Rotation = glm::quat(glm::vec3(glm::radians(pitch(rng)), glm::radians(yaw(rng)), glm::radians(roll(rng))));

    if (settings.AlignToSlope && glm::length(terrainNormal - glm::vec3(0, 1, 0)) > 0.001f) {
        glm::quat slopeRot = glm::rotation(glm::vec3(0, 1, 0), terrainNormal);
        inst.Rotation = glm::normalize(glm::slerp(inst.Rotation, slopeRot * inst.Rotation, glm::clamp(settings.SlopeAlignmentFactor, 0.0f, 1.0f)));
    }

    std::uniform_real_distribution<float> heightOffset(
        settings.HeightOffset - settings.HeightOffsetVariance,
        settings.HeightOffset + settings.HeightOffsetVariance);
    inst.Position.y += heightOffset(rng);
}

struct TerrainInstancerGenerationContext {
    float Density = 0.0f;
    float Spacing = 1.0f;
    float BaseProbability = 1.0f;
    uint32_t TotalCellsX = 0;
    uint32_t TotalCellsZ = 0;
    uint32_t ScanStartX = 0;
    uint32_t ScanStartZ = 0;
    uint32_t ScanEndX = 0;
    uint32_t ScanEndZ = 0;
    uint32_t Seed = 0;
    glm::mat3 NormalMatrix{1.0f};
};

uint64_t GetTerrainInstancerScanCellCount(const TerrainInstancerGenerationContext& ctx)
{
    if (ctx.TotalCellsX == 0 || ctx.TotalCellsZ == 0 ||
        ctx.ScanEndX < ctx.ScanStartX || ctx.ScanEndZ < ctx.ScanStartZ) {
        return 0;
    }
    const uint64_t scanCellsX = static_cast<uint64_t>(ctx.ScanEndX - ctx.ScanStartX + 1u);
    const uint64_t scanCellsZ = static_cast<uint64_t>(ctx.ScanEndZ - ctx.ScanStartZ + 1u);
    return scanCellsX * scanCellsZ;
}

bool BuildTerrainInstancerGenerationContext(
    const TerrainComponent& terrain,
    TerrainInstancerLayerDesc& layer,
    const glm::mat4& terrainTransform,
    TerrainInstancerGenerationContext& out)
{
    const InstancerComponent& comp = layer.Instancer;
    if (!layer.Enabled || !comp.Enabled || !comp.HasValidMesh() ||
        terrain.GridResolution < 2 || terrain.WorldSize.x <= 0.0f || terrain.WorldSize.y <= 0.0f ||
        comp.Distribution.DensityPerSquareMeter <= 0.0f) {
        return false;
    }

    const DistributionSettings& settings = comp.Distribution;
    out.Density = glm::max(0.0001f, settings.DensityPerSquareMeter);
    const float candidateSpacing = glm::clamp(std::sqrt(1.0f / out.Density), 0.25f, 128.0f);
    out.Spacing = glm::max(candidateSpacing, glm::max(0.05f, settings.MinSpacing));
    const float cellArea = out.Spacing * out.Spacing;
    out.BaseProbability = glm::clamp(out.Density * cellArea, 0.0f, 1.0f);
    out.TotalCellsX = static_cast<uint32_t>(glm::ceil(terrain.WorldSize.x / out.Spacing));
    out.TotalCellsZ = static_cast<uint32_t>(glm::ceil(terrain.WorldSize.y / out.Spacing));
    if (out.TotalCellsX == 0 || out.TotalCellsZ == 0) {
        return false;
    }

    out.ScanStartX = 0;
    out.ScanStartZ = 0;
    out.ScanEndX = out.TotalCellsX - 1u;
    out.ScanEndZ = out.TotalCellsZ - 1u;

    if (layer.Mask == TerrainInstancerMaskSource::Painted) {
        layer.RebuildPaintedMaskBounds(terrain.GridResolution);
        if (!layer.PaintedMaskHasCoverage) {
            out.ScanStartX = 1;
            out.ScanEndX = 0;
            out.ScanStartZ = 1;
            out.ScanEndZ = 0;
        } else {
            const glm::vec2 terrainCellSize = glm::max(Terrain::GetCellSize(terrain), glm::vec2(0.0001f));
            const glm::ivec2 paddedMin = glm::max(layer.PaintedMaskMin - glm::ivec2(1), glm::ivec2(0));
            const glm::ivec2 paddedMax = glm::min(
                layer.PaintedMaskMax + glm::ivec2(1),
                glm::ivec2(static_cast<int>(terrain.GridResolution) - 1));
            const float minLocalX = static_cast<float>(paddedMin.x) * terrainCellSize.x;
            const float minLocalZ = static_cast<float>(paddedMin.y) * terrainCellSize.y;
            const float maxLocalX = static_cast<float>(paddedMax.x) * terrainCellSize.x;
            const float maxLocalZ = static_cast<float>(paddedMax.y) * terrainCellSize.y;

            out.ScanStartX = static_cast<uint32_t>(glm::clamp(
                static_cast<int>(std::floor(minLocalX / out.Spacing)) - 1,
                0,
                static_cast<int>(out.TotalCellsX) - 1));
            out.ScanEndX = static_cast<uint32_t>(glm::clamp(
                static_cast<int>(std::ceil(maxLocalX / out.Spacing)) + 1,
                0,
                static_cast<int>(out.TotalCellsX) - 1));
            out.ScanStartZ = static_cast<uint32_t>(glm::clamp(
                static_cast<int>(std::floor(minLocalZ / out.Spacing)) - 1,
                0,
                static_cast<int>(out.TotalCellsZ) - 1));
            out.ScanEndZ = static_cast<uint32_t>(glm::clamp(
                static_cast<int>(std::ceil(maxLocalZ / out.Spacing)) + 1,
                0,
                static_cast<int>(out.TotalCellsZ) - 1));
        }
    }

    out.Seed = settings.Seed ^ static_cast<uint32_t>(layer.Guid.low) ^ static_cast<uint32_t>(layer.Guid.high >> 32);
    out.NormalMatrix = glm::transpose(glm::inverse(glm::mat3(terrainTransform)));
    return true;
}

bool AppendTerrainInstancerCell(
    TerrainComponent& terrain,
    TerrainInstancerLayerDesc& layer,
    const glm::mat4& terrainTransform,
    InstancerDistributor& distributor,
    const TerrainInstancerGenerationContext& ctx,
    uint32_t x,
    uint32_t z,
    bool rejectExistingInstanceId,
    std::unordered_set<uint32_t>* existingInstanceIds = nullptr)
{
    InstancerComponent& comp = layer.Instancer;
    if (comp.Runtime.Instances.size() >= kMaxTerrainInstances) {
        return false;
    }

    std::mt19937 rng(HashCell(ctx.Seed, x, z));
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);

    const float localX = glm::min((static_cast<float>(x) + unit(rng)) * ctx.Spacing, terrain.WorldSize.x);
    const float localZ = glm::min((static_cast<float>(z) + unit(rng)) * ctx.Spacing, terrain.WorldSize.y);
    if (Terrain::IsHoleAtLocal(terrain, localX, localZ)) {
        return false;
    }

    const float maskDensity = SampleTerrainInstancerMask(terrain, layer, localX, localZ);
    if (maskDensity <= 0.001f || unit(rng) > maskDensity * ctx.BaseProbability) {
        return false;
    }

    const float localHeight = Terrain::SampleHeightWorld(terrain, localX, localZ);
    const glm::vec3 localNormal = Terrain::SampleNormal(terrain, localX, localZ);
    glm::vec3 worldNormal = glm::normalize(ctx.NormalMatrix * localNormal);
    if (!std::isfinite(worldNormal.x) || !std::isfinite(worldNormal.y) || !std::isfinite(worldNormal.z) ||
        glm::length(worldNormal) < 0.001f) {
        worldNormal = glm::vec3(0, 1, 0);
    }

    const DistributionSettings& settings = comp.Distribution;
    const float slopeDegrees = glm::degrees(std::acos(glm::clamp(glm::dot(worldNormal, glm::vec3(0, 1, 0)), -1.0f, 1.0f)));
    if (slopeDegrees < settings.MinSlopeDegrees || slopeDegrees > settings.MaxSlopeDegrees) {
        return false;
    }

    InstanceData inst;
    const glm::vec3 localPos(localX, localHeight, localZ);
    inst.Position = glm::vec3(terrainTransform * glm::vec4(localPos, 1.0f));
    inst.InstanceID = distributor.GenerateInstanceID(inst.Position, ctx.Seed);

    if (comp.Runtime.DestroyedInstances.count(inst.InstanceID) > 0) {
        return false;
    }

    auto stateIt = comp.Runtime.ModifiedInstances.find(inst.InstanceID);
    if (stateIt != comp.Runtime.ModifiedInstances.end()) {
        if (!rejectExistingInstanceId && stateIt->second == InstanceState::Modified) {
            for (const InstanceData& existing : comp.Runtime.Instances) {
                if (existing.InstanceID == inst.InstanceID &&
                    existing.State == InstanceState::Modified &&
                    existing.ActivePrefabEntity != INVALID_ENTITY_ID) {
                    return false;
                }
            }
        }
        inst.State = stateIt->second;
        if (inst.State == InstanceState::Destroyed) {
            return false;
        }
    }

    if (rejectExistingInstanceId) {
        if (existingInstanceIds) {
            if (existingInstanceIds->count(inst.InstanceID) != 0) {
                return false;
            }
        } else {
            for (const InstanceData& existing : comp.Runtime.Instances) {
                if (existing.InstanceID == inst.InstanceID) {
                    return false;
                }
            }
        }
    }

    ApplyTerrainInstancerVariation(inst, settings, worldNormal, rng);
    comp.Runtime.Instances.push_back(inst);
    comp.Runtime.AddToSpatialHash(static_cast<uint32_t>(comp.Runtime.Instances.size() - 1u));
    if (existingInstanceIds) {
        existingInstanceIds->insert(inst.InstanceID);
    }
    return true;
}

void SyncTerrainInstancerPersistentState(InstancerComponent& comp)
{
    comp.Runtime.DestroyedInstances.clear();
    for (uint32_t id : comp.Persistent.DestroyedIDs) {
        comp.Runtime.DestroyedInstances.insert(id);
    }
    comp.Runtime.ModifiedInstances = comp.Persistent.StateOverrides;
}

void RebuildTerrainInstancerActiveIndexSet(InstancerComponent& comp)
{
    comp.Runtime.ActivePrefabIndices.clear();
    comp.Runtime.ActivePrefabs = 0;
    for (uint32_t i = 0; i < comp.Runtime.Instances.size(); ++i) {
        if ((comp.Runtime.Instances[i].State == InstanceState::Active ||
             comp.Runtime.Instances[i].State == InstanceState::Modified) &&
            comp.Runtime.Instances[i].ActivePrefabEntity != INVALID_ENTITY_ID) {
            comp.Runtime.ActivePrefabIndices.insert(i);
            ++comp.Runtime.ActivePrefabs;
        }
    }
    comp.Stats.ActivePrefabs = comp.Runtime.ActivePrefabs;
}

void RegenerateTerrainInstancerLayer(
    Scene& scene,
    TerrainComponent& terrain,
    TerrainInstancerLayerDesc& layer,
    const glm::mat4& terrainTransform,
    InstancerDistributor& distributor)
{
    auto startTime = std::chrono::high_resolution_clock::now();
    auto& comp = layer.Instancer;
    if (layer.Mask == TerrainInstancerMaskSource::Painted) {
        layer.EnsureMaskSize(terrain.GridResolution);
    }

    TerrainInstancerGenerationContext ctx;
    if (!BuildTerrainInstancerGenerationContext(terrain, layer, terrainTransform, ctx)) {
        std::vector<InstanceData> preservedModifiedPrefabs;
        for (const InstanceData& inst : comp.Runtime.Instances) {
            if (inst.State == InstanceState::Modified && inst.ActivePrefabEntity != INVALID_ENTITY_ID) {
                preservedModifiedPrefabs.push_back(inst);
            } else if (inst.ActivePrefabEntity != INVALID_ENTITY_ID) {
                scene.DestroyEntity(inst.ActivePrefabEntity);
            }
        }
        layer.ReleaseCollisionBodies();
        comp.Runtime.Clear();
        SyncTerrainInstancerPersistentState(comp);
        for (const InstanceData& inst : preservedModifiedPrefabs) {
            const uint32_t index = static_cast<uint32_t>(comp.Runtime.Instances.size());
            comp.Runtime.Instances.push_back(inst);
            comp.Runtime.AddToSpatialHash(index);
            comp.Runtime.ActivePrefabIndices.insert(index);
        }
        comp.Runtime.TotalInstances = static_cast<uint32_t>(comp.Runtime.Instances.size());
        comp.Runtime.ActivePrefabs = static_cast<uint32_t>(comp.Runtime.ActivePrefabIndices.size());
        comp.Stats.TotalInstances = comp.Runtime.TotalInstances;
        comp.Stats.ActivePrefabs = comp.Runtime.ActivePrefabs;
        comp.NeedsRegeneration = false;
        layer.RuntimeDirty = false;
        layer.RuntimeRegionDirty = false;
        layer.RuntimeRebuildInProgress = false;
        layer.RuntimeRebuildNextCell = 0;
        return;
    }

    if (!layer.RuntimeRebuildInProgress) {
        std::vector<InstanceData> preservedModifiedPrefabs;
        for (const InstanceData& inst : comp.Runtime.Instances) {
            if (inst.State == InstanceState::Modified && inst.ActivePrefabEntity != INVALID_ENTITY_ID) {
                preservedModifiedPrefabs.push_back(inst);
            } else if (inst.ActivePrefabEntity != INVALID_ENTITY_ID) {
                scene.DestroyEntity(inst.ActivePrefabEntity);
            }
        }
        layer.ReleaseCollisionBodies();
        comp.Runtime.Clear();
        SyncTerrainInstancerPersistentState(comp);
        for (const InstanceData& inst : preservedModifiedPrefabs) {
            const uint32_t index = static_cast<uint32_t>(comp.Runtime.Instances.size());
            comp.Runtime.Instances.push_back(inst);
            comp.Runtime.AddToSpatialHash(index);
            comp.Runtime.ActivePrefabIndices.insert(index);
        }
        comp.Runtime.ActivePrefabs = static_cast<uint32_t>(comp.Runtime.ActivePrefabIndices.size());
        comp.Stats.ActivePrefabs = comp.Runtime.ActivePrefabs;

        const size_t candidateCellCount = static_cast<size_t>(GetTerrainInstancerScanCellCount(ctx));
        comp.Runtime.Instances.reserve(std::min<size_t>(kMaxTerrainInstances, candidateCellCount));
        layer.RuntimeRebuildInProgress = true;
        layer.RuntimeRebuildNextCell = 0;
        layer.RuntimeDirty = false;
        layer.RuntimeRegionDirty = false;
        comp.NeedsRegeneration = false;
    }

    const uint64_t totalCells = GetTerrainInstancerScanCellCount(ctx);
    const uint32_t scanCellsX = (totalCells > 0)
        ? (ctx.ScanEndX - ctx.ScanStartX + 1u)
        : 0u;
    uint32_t processedCells = 0;
    while (layer.RuntimeRebuildNextCell < totalCells &&
           processedCells < kMaxTerrainInstancerGenerationCellsPerUpdate &&
           comp.Runtime.Instances.size() < kMaxTerrainInstances) {
        const uint64_t cell = layer.RuntimeRebuildNextCell++;
        const uint32_t x = ctx.ScanStartX + static_cast<uint32_t>(cell % scanCellsX);
        const uint32_t z = ctx.ScanStartZ + static_cast<uint32_t>(cell / scanCellsX);
        AppendTerrainInstancerCell(terrain, layer, terrainTransform, distributor, ctx, x, z, false);
        ++processedCells;

        if ((processedCells & 1023u) == 0u) {
            const auto now = std::chrono::high_resolution_clock::now();
            const float elapsedMs = std::chrono::duration<float, std::milli>(now - startTime).count();
            if (elapsedMs >= kMaxTerrainInstancerGenerationMsPerUpdate) {
                break;
            }
        }
    }

    comp.Runtime.TotalInstances = static_cast<uint32_t>(comp.Runtime.Instances.size());
    comp.Stats.TotalInstances = comp.Runtime.TotalInstances;
    if (layer.RuntimeRebuildNextCell >= totalCells || comp.Runtime.Instances.size() >= kMaxTerrainInstances) {
        comp.Runtime.UpdateSpatialHash();
        layer.RuntimeRebuildInProgress = false;
        layer.RuntimeRebuildNextCell = 0;
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    comp.Stats.LastGenerationTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();
    if (InstancerDiagnosticsEnabled()) {
        std::cout << "[InstancerDiag] terrain full chunk layer=\"" << layer.Name
                  << "\" cells=" << processedCells
                  << " next=" << layer.RuntimeRebuildNextCell << "/" << totalCells
                  << " instances=" << comp.Runtime.TotalInstances
                  << " ms=" << comp.Stats.LastGenerationTimeMs << "\n";
    }
}

void RegenerateTerrainInstancerLayerRegion(
    Scene& scene,
    TerrainComponent& terrain,
    TerrainInstancerLayerDesc& layer,
    const glm::mat4& terrainTransform,
    InstancerDistributor& distributor)
{
    if (!layer.RuntimeRegionDirty) {
        return;
    }

    auto startTime = std::chrono::high_resolution_clock::now();
    InstancerComponent& comp = layer.Instancer;
    if (layer.Mask == TerrainInstancerMaskSource::Painted) {
        layer.EnsureMaskSize(terrain.GridResolution);
    }

    TerrainInstancerGenerationContext ctx;
    if (!BuildTerrainInstancerGenerationContext(terrain, layer, terrainTransform, ctx)) {
        layer.RuntimeRegionDirty = false;
        return;
    }

    SyncTerrainInstancerPersistentState(comp);

    const glm::vec2 cellSize = glm::max(Terrain::GetCellSize(terrain), glm::vec2(0.0001f));
    const glm::ivec2 minCell = glm::max(layer.RuntimeDirtyMin - glm::ivec2(1), glm::ivec2(0));
    const glm::ivec2 maxCell = glm::min(layer.RuntimeDirtyMax + glm::ivec2(1), glm::ivec2(static_cast<int>(terrain.GridResolution) - 1));
    const float minLocalX = static_cast<float>(minCell.x) * cellSize.x;
    const float minLocalZ = static_cast<float>(minCell.y) * cellSize.y;
    const float maxLocalX = static_cast<float>(maxCell.x) * cellSize.x;
    const float maxLocalZ = static_cast<float>(maxCell.y) * cellSize.y;

    const glm::mat4 invTerrain = glm::inverse(terrainTransform);
    auto isInsideDirtyRegion = [&](const InstanceData& inst) {
        const glm::vec3 local = glm::vec3(invTerrain * glm::vec4(inst.Position, 1.0f));
        return local.x >= minLocalX && local.x <= maxLocalX &&
               local.z >= minLocalZ && local.z <= maxLocalZ;
    };

    size_t writeIndex = 0;
    for (size_t readIndex = 0; readIndex < comp.Runtime.Instances.size(); ++readIndex) {
        InstanceData& inst = comp.Runtime.Instances[readIndex];
        const bool preserveModified = inst.State == InstanceState::Modified;
        const bool removeInstance = !preserveModified && isInsideDirtyRegion(inst);
        if (!removeInstance) {
            if (writeIndex != readIndex) {
                comp.Runtime.Instances[writeIndex] = std::move(inst);
            }
            ++writeIndex;
            continue;
        }

        if (inst.ActivePrefabEntity != INVALID_ENTITY_ID) {
            scene.DestroyEntity(inst.ActivePrefabEntity);
        }
        auto bodyIt = layer.ActiveCollisionBodies.find(inst.InstanceID);
        if (bodyIt != layer.ActiveCollisionBodies.end()) {
            if (!bodyIt->second.IsInvalid()) {
                Physics::DestroyBody(bodyIt->second);
            }
            layer.ActiveCollisionBodies.erase(bodyIt);
        }
    }
    comp.Runtime.Instances.resize(writeIndex);
    RebuildTerrainInstancerActiveIndexSet(comp);

    std::unordered_set<uint32_t> existingInstanceIds;
    existingInstanceIds.reserve(comp.Runtime.Instances.size());
    for (const InstanceData& inst : comp.Runtime.Instances) {
        existingInstanceIds.insert(inst.InstanceID);
    }

    const uint32_t startX = static_cast<uint32_t>(glm::clamp(
        static_cast<int>(std::floor(minLocalX / ctx.Spacing)) - 1, 0, static_cast<int>(ctx.TotalCellsX) - 1));
    const uint32_t endX = static_cast<uint32_t>(glm::clamp(
        static_cast<int>(std::ceil(maxLocalX / ctx.Spacing)) + 1, 0, static_cast<int>(ctx.TotalCellsX) - 1));
    const uint32_t startZ = static_cast<uint32_t>(glm::clamp(
        static_cast<int>(std::floor(minLocalZ / ctx.Spacing)) - 1, 0, static_cast<int>(ctx.TotalCellsZ) - 1));
    const uint32_t endZ = static_cast<uint32_t>(glm::clamp(
        static_cast<int>(std::ceil(maxLocalZ / ctx.Spacing)) + 1, 0, static_cast<int>(ctx.TotalCellsZ) - 1));

    comp.Runtime.Instances.reserve(std::min<size_t>(kMaxTerrainInstances, comp.Runtime.Instances.size() + 256u));
    if (GetTerrainInstancerScanCellCount(ctx) > 0) {
        for (uint32_t z = startZ; z <= endZ && comp.Runtime.Instances.size() < kMaxTerrainInstances; ++z) {
            for (uint32_t x = startX; x <= endX && comp.Runtime.Instances.size() < kMaxTerrainInstances; ++x) {
                AppendTerrainInstancerCell(terrain, layer, terrainTransform, distributor, ctx, x, z, true, &existingInstanceIds);
            }
        }
    }

    comp.Runtime.TotalInstances = static_cast<uint32_t>(comp.Runtime.Instances.size());
    comp.Runtime.UpdateSpatialHash();
    comp.Stats.TotalInstances = comp.Runtime.TotalInstances;
    layer.RuntimeRegionDirty = false;

    auto endTime = std::chrono::high_resolution_clock::now();
    comp.Stats.LastGenerationTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();
    if (InstancerDiagnosticsEnabled()) {
        std::cout << "[InstancerDiag] terrain region rebuild layer=\"" << layer.Name
                  << "\" region=(" << minCell.x << "," << minCell.y << ")-(" << maxCell.x << "," << maxCell.y << ")"
                  << " instances=" << comp.Runtime.TotalInstances
                  << " ms=" << comp.Stats.LastGenerationTimeMs << "\n";
    }
}

bool EnsureTerrainInstancerCollisionShape(TerrainInstancerLayerDesc& layer)
{
    if (layer.SharedCollisionShape) {
        return true;
    }

    InstancerComponent& comp = layer.Instancer;
    if (comp.NeedsMeshReload && !InstancerRenderer::Instance().LoadMeshData(comp)) {
        return false;
    }
    if (!comp.CachedMesh.Valid) {
        return false;
    }

    JPH::VertexList vertices;
    JPH::IndexedTriangleList triangles;
    std::unordered_set<const Mesh*> emittedMeshes;

    for (const MeshInstanceBatch& batch : comp.CachedMesh.Batches) {
        const Mesh* mesh = batch.MeshRef.get();
        if (!mesh || mesh->Vertices.empty() || emittedMeshes.count(mesh) != 0) {
            continue;
        }
        emittedMeshes.insert(mesh);

        const uint32_t baseVertex = static_cast<uint32_t>(vertices.size());
        vertices.reserve(vertices.size() + mesh->Vertices.size());
        for (const glm::vec3& v : mesh->Vertices) {
            const glm::vec3 tv = glm::vec3(batch.LocalTransform * glm::vec4(v, 1.0f));
            vertices.emplace_back(tv.x, tv.y, tv.z);
        }

        if (!mesh->Indices.empty()) {
            for (size_t i = 0; i + 2 < mesh->Indices.size(); i += 3) {
                const uint32_t i0 = mesh->Indices[i + 0];
                const uint32_t i1 = mesh->Indices[i + 1];
                const uint32_t i2 = mesh->Indices[i + 2];
                if (i0 >= mesh->Vertices.size() || i1 >= mesh->Vertices.size() || i2 >= mesh->Vertices.size()) {
                    continue;
                }
                triangles.emplace_back(baseVertex + i0, baseVertex + i1, baseVertex + i2, 0);
            }
        } else {
            for (uint32_t i = 0; i + 2 < mesh->Vertices.size(); i += 3) {
                triangles.emplace_back(baseVertex + i, baseVertex + i + 1, baseVertex + i + 2, 0);
            }
        }
    }

    if (vertices.empty() || triangles.empty()) {
        return false;
    }

    JPH::MeshShapeSettings settings(std::move(vertices), std::move(triangles));
    JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (result.HasError()) {
        std::cerr << "[TerrainInstancer] Failed to create shared collision mesh: " << result.GetError() << "\n";
        return false;
    }

    layer.SharedCollisionShape = result.Get();
    return layer.SharedCollisionShape != nullptr;
}

JPH::RefConst<JPH::Shape> BuildTerrainInstancerCollisionShapeForScale(
    TerrainInstancerLayerDesc& layer,
    const glm::vec3& scale)
{
    if (!EnsureTerrainInstancerCollisionShape(layer)) {
        return nullptr;
    }

    const glm::vec3 absScale = glm::max(glm::abs(scale), glm::vec3(0.001f));
    if (glm::length(absScale - glm::vec3(1.0f)) <= 0.0001f) {
        return layer.SharedCollisionShape;
    }

    JPH::ScaledShapeSettings scaledSettings(
        layer.SharedCollisionShape,
        JPH::Vec3(absScale.x, absScale.y, absScale.z));
    JPH::ShapeSettings::ShapeResult result = scaledSettings.Create();
    if (result.HasError()) {
        return layer.SharedCollisionShape;
    }
    return result.Get();
}

void UpdateTerrainInstancerCollision(
    TerrainInstancerLayerDesc& layer,
    const glm::vec3& cameraPos,
    EntityID ownerEntity)
{
    const auto startTime = std::chrono::high_resolution_clock::now();
    InstancerComponent& comp = layer.Instancer;
    if (!layer.Enabled || !comp.Enabled || !layer.Collision.Enabled || Physics::GetSystem() == nullptr) {
        layer.ReleaseCollisionBodies();
        return;
    }

    const float activationDistance = glm::max(0.0f, layer.Collision.ActivationDistance);
    const uint32_t maxBodies = glm::max(1u, layer.Collision.MaxActiveBodies);
    if (activationDistance <= 0.0f || comp.Runtime.Instances.empty()) {
        layer.ReleaseCollisionBodies();
        return;
    }

    struct Candidate {
        uint32_t Index = 0;
        float Distance = 0.0f;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(std::min<size_t>(static_cast<size_t>(maxBodies) * 2u, comp.Runtime.Instances.size()));

    std::vector<uint32_t> nearby;
    comp.Runtime.QueryNearby(cameraPos, activationDistance, nearby);
    for (uint32_t i : nearby) {
        if (i >= comp.Runtime.Instances.size()) {
            continue;
        }
        const InstanceData& inst = comp.Runtime.Instances[i];
        if (inst.State == InstanceState::Destroyed ||
            inst.State == InstanceState::Active ||
            inst.State == InstanceState::Modified) {
            continue;
        }
        const float distance = glm::length(inst.Position - cameraPos);
        if (distance <= activationDistance) {
            candidates.push_back({ i, distance });
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.Distance < b.Distance;
    });
    if (candidates.size() > maxBodies) {
        candidates.resize(maxBodies);
    }

    std::unordered_set<uint32_t> desired;
    desired.reserve(candidates.size());
    for (const Candidate& c : candidates) {
        desired.insert(comp.Runtime.Instances[c.Index].InstanceID);
    }

    for (auto it = layer.ActiveCollisionBodies.begin(); it != layer.ActiveCollisionBodies.end(); ) {
        if (desired.count(it->first) == 0) {
            if (!it->second.IsInvalid()) {
                Physics::DestroyBody(it->second);
            }
            it = layer.ActiveCollisionBodies.erase(it);
        } else {
            ++it;
        }
    }

    int32_t layerIdx = -1;
    if (!layer.Collision.PhysicsLayerName.empty()) {
        layerIdx = PhysicsLayers::PhysicsLayerManager::Get().GetLayerIndex(layer.Collision.PhysicsLayerName);
    }
    const uint32_t physicsLayer = (layerIdx >= 0)
        ? static_cast<uint32_t>(layerIdx)
        : layer.Collision.PhysicsLayer;

    for (const Candidate& c : candidates) {
        const InstanceData& inst = comp.Runtime.Instances[c.Index];
        if (layer.ActiveCollisionBodies.find(inst.InstanceID) != layer.ActiveCollisionBodies.end()) {
            continue;
        }

        JPH::RefConst<JPH::Shape> shape = BuildTerrainInstancerCollisionShapeForScale(layer, inst.Scale);
        if (!shape) {
            break;
        }

        glm::mat4 bodyTransform = glm::translate(glm::mat4(1.0f), inst.Position) * glm::mat4_cast(inst.Rotation);
        JPH::BodyID bodyId = Physics::CreateBody(bodyTransform, shape, true, physicsLayer);
        if (!bodyId.IsInvalid()) {
            if (ownerEntity != INVALID_ENTITY_ID) {
                Physics::GetBodyInterface().SetUserData(bodyId, static_cast<uint64_t>(ownerEntity));
            }
            layer.ActiveCollisionBodies.emplace(inst.InstanceID, bodyId);
        }
    }

    if (InstancerDiagnosticsEnabled()) {
        const auto endTime = std::chrono::high_resolution_clock::now();
        const float elapsedMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();
        std::cout << "[InstancerDiag] collision nearby=" << nearby.size()
                  << " candidates=" << candidates.size()
                  << " bodies=" << layer.ActiveCollisionBodies.size()
                  << " ms=" << elapsedMs << "\n";
    }
}

} // namespace


//------------------------------------------------------------------------------
// InstancerComponent Implementation
//------------------------------------------------------------------------------

void RuntimeData::Clear() {
    Instances.clear();
    SpatialHash.clear();
    VisibleInstanceIndices.clear();
    DestroyedInstances.clear();
    ModifiedInstances.clear();
    ActivePrefabIndices.clear();
    TotalInstances = 0;
    VisibleInstances = 0;
    ActivePrefabs = 0;
    CulledInstances = 0;
}

void RuntimeData::AddToSpatialHash(uint32_t instanceIndex) {
    if (instanceIndex >= Instances.size()) {
        return;
    }
    const auto& inst = Instances[instanceIndex];
    const float cellSize = glm::max(CellSize, 0.001f);
    int64_t cellX = static_cast<int64_t>(std::floor(inst.Position.x / cellSize));
    int64_t cellZ = static_cast<int64_t>(std::floor(inst.Position.z / cellSize));
    int64_t key = (cellX << 32) | (cellZ & 0xFFFFFFFF);
    SpatialHash[key].push_back(instanceIndex);
}

void RuntimeData::UpdateSpatialHash() {
    SpatialHash.clear();
    for (uint32_t i = 0; i < Instances.size(); ++i) {
        AddToSpatialHash(i);
    }
}

void RuntimeData::QueryNearby(const glm::vec3& pos, float radius, std::vector<uint32_t>& outIndices) const {
    outIndices.clear();
    if (radius < 0.0f || Instances.empty()) {
        return;
    }

    const float radiusSq = radius * radius;
    if (SpatialHash.empty()) {
        outIndices.reserve(Instances.size());
        for (uint32_t i = 0; i < Instances.size(); ++i) {
            const glm::vec3 delta = pos - Instances[i].Position;
            if (glm::dot(delta, delta) <= radiusSq) {
                outIndices.push_back(i);
            }
        }
        return;
    }

    const float cellSize = glm::max(CellSize, 0.001f);
    int cellRadius = static_cast<int>(std::ceil(radius / cellSize));
    int64_t centerX = static_cast<int64_t>(std::floor(pos.x / cellSize));
    int64_t centerZ = static_cast<int64_t>(std::floor(pos.z / cellSize));
    
    for (int dx = -cellRadius; dx <= cellRadius; ++dx) {
        for (int dz = -cellRadius; dz <= cellRadius; ++dz) {
            int64_t key = ((centerX + dx) << 32) | ((centerZ + dz) & 0xFFFFFFFF);
            auto it = SpatialHash.find(key);
            if (it != SpatialHash.end()) {
                for (uint32_t idx : it->second) {
                    if (idx >= Instances.size()) {
                        continue;
                    }
                    const glm::vec3 delta = pos - Instances[idx].Position;
                    if (glm::dot(delta, delta) <= radiusSq) {
                        outIndices.push_back(idx);
                    }
                }
            }
        }
    }
}

std::vector<uint32_t> RuntimeData::QueryNearby(const glm::vec3& pos, float radius) const {
    std::vector<uint32_t> result;
    QueryNearby(pos, radius, result);
    return result;
}

void CachedMeshData::Release() {
    // Note: Don't destroy VBH/IBH handles - they're owned by the mesh system
    Batches.clear();
    MeshPtr.reset();
    Valid = false;
}

InstancerComponent::InstancerComponent() = default;
InstancerComponent::~InstancerComponent() {
    CachedMesh.Release();
}

InstancerComponent::InstancerComponent(const InstancerComponent& other) {
    MeshAsset = other.MeshAsset;
    MeshPath = other.MeshPath;
    PrefabAsset = other.PrefabAsset;
    PrefabPath = other.PrefabPath;
    SurfaceEntity = other.SurfaceEntity;
    Distribution = other.Distribution;
    DistributionRadius = other.DistributionRadius;
    DistributionAreaMin = other.DistributionAreaMin;
    DistributionAreaMax = other.DistributionAreaMax;
    UseRadiusMode = other.UseRadiusMode;
    ManualPoints = other.ManualPoints;
    UseManualPoints = other.UseManualPoints;
    Swap = other.Swap;
    Persistent = other.Persistent;
    Enabled = other.Enabled;
    NeedsRegeneration = true;
    NeedsMeshReload = true;
    PreviewColor = other.PreviewColor;
    ShowDebugMarkers = other.ShowDebugMarkers;
    ShowBounds = other.ShowBounds;
    // Runtime and CachedMesh are not copied - regenerated on demand
}

InstancerComponent& InstancerComponent::operator=(const InstancerComponent& other) {
    if (this != &other) {
        CachedMesh.Release();
        Runtime.Clear();
        
        MeshAsset = other.MeshAsset;
        MeshPath = other.MeshPath;
        PrefabAsset = other.PrefabAsset;
        PrefabPath = other.PrefabPath;
        SurfaceEntity = other.SurfaceEntity;
        Distribution = other.Distribution;
        DistributionRadius = other.DistributionRadius;
        DistributionAreaMin = other.DistributionAreaMin;
        DistributionAreaMax = other.DistributionAreaMax;
        UseRadiusMode = other.UseRadiusMode;
        ManualPoints = other.ManualPoints;
        UseManualPoints = other.UseManualPoints;
        Swap = other.Swap;
        Persistent = other.Persistent;
        Enabled = other.Enabled;
        NeedsRegeneration = true;
        NeedsMeshReload = true;
        PreviewColor = other.PreviewColor;
        ShowDebugMarkers = other.ShowDebugMarkers;
        ShowBounds = other.ShowBounds;
    }
    return *this;
}

InstancerComponent::InstancerComponent(InstancerComponent&& other) noexcept = default;
InstancerComponent& InstancerComponent::operator=(InstancerComponent&& other) noexcept = default;

void InstancerComponent::Regenerate(const glm::mat4& instancerTransform, const TerrainComponent* terrain, const glm::mat4& terrainTransform) {
    auto startTime = std::chrono::high_resolution_clock::now();
    
    Runtime.Clear();
    
    // Preserve destroyed/modified state from persistent storage
    for (uint32_t id : Persistent.DestroyedIDs) {
        Runtime.DestroyedInstances.insert(id);
    }
    Runtime.ModifiedInstances = Persistent.StateOverrides;
    
    InstancerDistributor distributor;
    InstancerDistributor::DistributionParams params;
    params.Seed = Distribution.Seed;
    params.Terrain = terrain;
    params.TerrainTransform = terrainTransform;
    params.InstancerTransform = instancerTransform;
    
    glm::vec3 instancerPos = glm::vec3(instancerTransform[3]);
    params.CenterPosition = instancerPos;
    
    if (UseRadiusMode) {
        params.UseRadius = true;
        params.Radius = DistributionRadius;
        params.AreaMin = glm::vec2(instancerPos.x - DistributionRadius, instancerPos.z - DistributionRadius);
        params.AreaMax = glm::vec2(instancerPos.x + DistributionRadius, instancerPos.z + DistributionRadius);
    } else {
        params.UseRadius = false;
        params.AreaMin = DistributionAreaMin + glm::vec2(instancerPos.x, instancerPos.z);
        params.AreaMax = DistributionAreaMax + glm::vec2(instancerPos.x, instancerPos.z);
    }
    
    if (UseManualPoints && !ManualPoints.empty()) {
        // Use manual points instead of procedural distribution
        for (const auto& pos : ManualPoints) {
            InstanceData inst;
            inst.Position = pos;
            inst.Rotation = glm::quat(1, 0, 0, 0);
            inst.Scale = glm::vec3(1.0f);
            inst.InstanceID = distributor.GenerateInstanceID(pos, Distribution.Seed);
            
            // Check if this instance was previously destroyed
            if (Runtime.DestroyedInstances.count(inst.InstanceID) > 0) {
                inst.State = InstanceState::Destroyed;
            } else if (Runtime.ModifiedInstances.count(inst.InstanceID) > 0) {
                inst.State = Runtime.ModifiedInstances[inst.InstanceID];
            }
            
            Runtime.Instances.push_back(inst);
        }
    } else {
        // Procedural distribution
        distributor.GenerateInstances(*this, params, Runtime.Instances);
    }
    
    Runtime.TotalInstances = static_cast<uint32_t>(Runtime.Instances.size());
    Runtime.UpdateSpatialHash();
    
    NeedsRegeneration = false;
    
    auto endTime = std::chrono::high_resolution_clock::now();
    Stats.LastGenerationTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();
    Stats.TotalInstances = Runtime.TotalInstances;
    
    std::cout << "[Instancer] Generated " << Runtime.TotalInstances << " instances in " 
              << Stats.LastGenerationTimeMs << "ms" << std::endl;
}

void InstancerComponent::UpdateVisibility(const glm::vec3& cameraPos, const glm::mat4& viewProj, const glm::mat4& proj) {
    const auto startTime = std::chrono::high_resolution_clock::now();
    uint32_t visible = 0;
    for (uint32_t index : Runtime.VisibleInstanceIndices) {
        if (index < Runtime.Instances.size()) {
            Runtime.Instances[index].Visible = false;
        }
    }
    Runtime.VisibleInstanceIndices.clear();

    const float projectionScale = glm::max(std::abs(proj[0][0]), std::abs(proj[1][1]));
    const bool hasBounds = CachedMesh.Valid;
    const glm::vec3 boundsCenter = hasBounds ? (CachedMesh.BoundsMin + CachedMesh.BoundsMax) * 0.5f : glm::vec3(0.0f);
    const glm::vec3 boundsExtents = hasBounds ? (CachedMesh.BoundsMax - CachedMesh.BoundsMin) * 0.5f : glm::vec3(0.0f);
    const float baseRadius = (hasBounds && glm::dot(boundsExtents, boundsExtents) > 1e-6f)
        ? glm::length(boundsExtents)
        : 10.0f;

    if (Runtime.Instances.empty() || Swap.CullDistance <= 0.0f) {
        Runtime.VisibleInstances = 0;
        Runtime.CulledInstances = static_cast<uint32_t>(Runtime.Instances.size());
        Stats.VisibleInstances = 0;
        Stats.CulledInstances = Runtime.CulledInstances;
        return;
    }

    const float maxConfiguredScale = EstimateMaxInstanceScale(*this);
    const float queryRadius = Swap.CullDistance + (baseRadius + glm::length(boundsCenter)) * maxConfiguredScale;
    std::vector<uint32_t> candidateIndices;
    Runtime.QueryNearby(cameraPos, queryRadius, candidateIndices);
    
    for (uint32_t index : candidateIndices) {
        if (index >= Runtime.Instances.size()) {
            continue;
        }
        auto& inst = Runtime.Instances[index];
        inst.DistanceToCamera = glm::length(cameraPos - inst.Position);
        
        // Distance culling
        if (inst.DistanceToCamera > Swap.CullDistance) {
            inst.Visible = false;
            continue;
        }
        
        // Skip destroyed instances
        if (inst.State == InstanceState::Destroyed) {
            inst.Visible = false;
            continue;
        }
        
        const float maxScale = glm::max(std::abs(inst.Scale.x), glm::max(std::abs(inst.Scale.y), std::abs(inst.Scale.z)));
        const float worldRadius = baseRadius * maxScale;
        glm::vec3 worldCenter = inst.Position;
        if (hasBounds) {
            const glm::vec3 scaledCenter = boundsCenter * inst.Scale;
            worldCenter += inst.Rotation * scaledCenter;
        }
        
        // Simple frustum culling using clip space (sphere-expanded)
        glm::vec4 clip = viewProj * glm::vec4(worldCenter, 1.0f);
        if (clip.w <= 0.0f) {
            inst.Visible = false;
            continue;
        }
        
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        
        // Convert world-space radius to NDC margin (account for projection scale)
        float margin = (worldRadius * projectionScale) / clip.w;
        // Add minimum margin to handle edge cases
        margin = glm::max(margin, 0.15f);
        
        inst.Visible = ndc.x >= -1.0f - margin && ndc.x <= 1.0f + margin &&
                       ndc.y >= -1.0f - margin && ndc.y <= 1.0f + margin &&
                       ndc.z >= 0.0f && ndc.z <= 1.0f;
        
        if (inst.Visible) {
            Runtime.VisibleInstanceIndices.push_back(index);
            ++visible;
        }
    }
    
    const uint32_t culled = Runtime.TotalInstances >= visible
        ? Runtime.TotalInstances - visible
        : 0u;
    Runtime.VisibleInstances = visible;
    Runtime.CulledInstances = culled;
    Stats.VisibleInstances = visible;
    Stats.CulledInstances = culled;

    if (InstancerDiagnosticsEnabled()) {
        const auto endTime = std::chrono::high_resolution_clock::now();
        const float elapsedMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();
        std::cout << "[InstancerDiag] visibility instances=" << Runtime.TotalInstances
                  << " candidates=" << candidateIndices.size()
                  << " visible=" << visible
                  << " ms=" << elapsedMs << "\n";
    }
}

void InstancerComponent::MarkInstanceDestroyed(uint32_t instanceId) {
    VisibilityCacheValid = false;
    Persistent.DestroyedIDs.push_back(instanceId);
    Persistent.StateOverrides[instanceId] = InstanceState::Destroyed;
    Runtime.DestroyedInstances.insert(instanceId);
    
    // Update runtime instance
    for (uint32_t i = 0; i < Runtime.Instances.size(); ++i) {
        auto& inst = Runtime.Instances[i];
        if (inst.InstanceID == instanceId) {
            inst.State = InstanceState::Destroyed;
            inst.Visible = false;
            Runtime.ActivePrefabIndices.erase(i);
            break;
        }
    }
    Runtime.ActivePrefabs = static_cast<uint32_t>(Runtime.ActivePrefabIndices.size());
    Stats.ActivePrefabs = Runtime.ActivePrefabs;
}

void InstancerComponent::MarkInstanceModified(uint32_t instanceId) {
    VisibilityCacheValid = false;
    Persistent.StateOverrides[instanceId] = InstanceState::Modified;
    Runtime.ModifiedInstances[instanceId] = InstanceState::Modified;
    
    // Update runtime instance
    for (uint32_t i = 0; i < Runtime.Instances.size(); ++i) {
        auto& inst = Runtime.Instances[i];
        if (inst.InstanceID == instanceId) {
            inst.State = InstanceState::Modified;
            if (inst.ActivePrefabEntity != INVALID_ENTITY_ID) {
                Runtime.ActivePrefabIndices.insert(i);
            }
            break;
        }
    }
    Runtime.ActivePrefabs = static_cast<uint32_t>(Runtime.ActivePrefabIndices.size());
    Stats.ActivePrefabs = Runtime.ActivePrefabs;
}

void InstancerComponent::ReloadMesh() {
    VisibilityCacheValid = false;
    CachedMesh.Release();
    NeedsMeshReload = true;
}

void InstancerComponent::ClearCache() {
    VisibilityCacheValid = false;
    CachedMesh.Release();
    Runtime.Clear();
    NeedsRegeneration = true;
    NeedsMeshReload = true;
}

bool InstancerComponent::HasValidMesh() const {
    return MeshAsset.guid.high != 0 || MeshAsset.guid.low != 0;
}

bool InstancerComponent::HasValidPrefab() const {
    return PrefabAsset.guid.high != 0 || PrefabAsset.guid.low != 0;
}

//------------------------------------------------------------------------------
// InstancerDistributor Implementation
//------------------------------------------------------------------------------

void InstancerDistributor::GenerateInstances(
    InstancerComponent& comp,
    const DistributionParams& params,
    std::vector<InstanceData>& outInstances)
{
    const auto& settings = comp.Distribution;
    
    // Generate candidate positions using Poisson disk sampling
    std::vector<glm::vec2> positions;
    PoissonDiskSample(
        params.AreaMin, params.AreaMax,
        settings.MinSpacing,
        settings.DensityPerSquareMeter,
        params.Seed,
        positions);
    
    // Random number generator for variations
    std::mt19937 rng(params.Seed);
    
    for (const auto& pos2d : positions) {
        InstanceData inst;
        inst.Position.x = pos2d.x;
        inst.Position.z = pos2d.y;
        inst.Position.y = params.CenterPosition.y;  // Default height
        
        // Check radius constraint
        if (params.UseRadius) {
            float distSq = glm::dot(
                glm::vec2(inst.Position.x, inst.Position.z) - glm::vec2(params.CenterPosition.x, params.CenterPosition.z),
                glm::vec2(inst.Position.x, inst.Position.z) - glm::vec2(params.CenterPosition.x, params.CenterPosition.z));
            if (distSq > params.Radius * params.Radius) {
                continue;
            }
        }
        
        // Sample terrain if available
        glm::vec3 terrainNormal(0, 1, 0);
        if (params.Terrain) {
            float height;
            if (SampleTerrainHeight(params.Terrain, params.TerrainTransform, pos2d, height)) {
                inst.Position.y = height;
            } else {
                continue;  // Outside terrain bounds
            }
            
            // Check slope constraints
            float slopeDegrees;
            if (SampleTerrainSlope(params.Terrain, params.TerrainTransform, pos2d, slopeDegrees, terrainNormal)) {
                if (slopeDegrees < settings.MinSlopeDegrees || slopeDegrees > settings.MaxSlopeDegrees) {
                    continue;  // Slope out of range
                }
            }
        }
        
        // Generate stable instance ID
        inst.InstanceID = GenerateInstanceID(inst.Position, params.Seed);
        
        // Check if previously destroyed
        if (comp.Runtime.DestroyedInstances.count(inst.InstanceID) > 0) {
            continue;
        }
        
        // Check for state override
        auto stateIt = comp.Runtime.ModifiedInstances.find(inst.InstanceID);
        if (stateIt != comp.Runtime.ModifiedInstances.end()) {
            inst.State = stateIt->second;
            if (inst.State == InstanceState::Destroyed) {
                continue;
            }
        }
        
        // Apply random variations
        ApplyRandomVariation(inst, settings, terrainNormal, rng);
        
        // Apply height offset
        std::uniform_real_distribution<float> heightOffsetDist(
            settings.HeightOffset - settings.HeightOffsetVariance,
            settings.HeightOffset + settings.HeightOffsetVariance);
        inst.Position.y += heightOffsetDist(rng);
        
        outInstances.push_back(inst);
    }
}

void InstancerDistributor::PoissonDiskSample(
    const glm::vec2& areaMin,
    const glm::vec2& areaMax,
    float minDistance,
    float density,
    uint32_t seed,
    std::vector<glm::vec2>& outPositions)
{
    // Bridson's algorithm for Poisson disk sampling
    const float cellSize = minDistance / std::sqrt(2.0f);
    const glm::vec2 areaSize = areaMax - areaMin;
    const int gridWidth = static_cast<int>(std::ceil(areaSize.x / cellSize));
    const int gridHeight = static_cast<int>(std::ceil(areaSize.y / cellSize));
    
    if (gridWidth <= 0 || gridHeight <= 0) return;
    
    std::vector<int> grid(gridWidth * gridHeight, -1);
    std::vector<glm::vec2> points;
    std::vector<int> activeList;
    
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> uniformDist(0.0f, 1.0f);
    
    // Calculate approximate number of points based on density
    float area = areaSize.x * areaSize.y;
    int targetPoints = static_cast<int>(area * density);
    targetPoints = std::min(targetPoints, 100000);  // Cap to prevent excessive computation
    
    // Start with a random point
    glm::vec2 firstPoint(
        areaMin.x + uniformDist(rng) * areaSize.x,
        areaMin.y + uniformDist(rng) * areaSize.y);
    
    points.push_back(firstPoint);
    activeList.push_back(0);
    
    int gridX = static_cast<int>((firstPoint.x - areaMin.x) / cellSize);
    int gridY = static_cast<int>((firstPoint.y - areaMin.y) / cellSize);
    if (gridX >= 0 && gridX < gridWidth && gridY >= 0 && gridY < gridHeight) {
        grid[gridY * gridWidth + gridX] = 0;
    }
    
    const int maxAttempts = 30;
    
    while (!activeList.empty() && static_cast<int>(points.size()) < targetPoints) {
        // Pick random point from active list
        int activeIdx = static_cast<int>(uniformDist(rng) * activeList.size());
        int pointIdx = activeList[activeIdx];
        const glm::vec2& point = points[pointIdx];
        
        bool foundValid = false;
        
        for (int attempt = 0; attempt < maxAttempts; ++attempt) {
            // Generate random point in annulus [r, 2r]
            float angle = uniformDist(rng) * 6.28318530718f;
            float dist = minDistance * (1.0f + uniformDist(rng));
            
            glm::vec2 newPoint = point + glm::vec2(std::cos(angle), std::sin(angle)) * dist;
            
            // Check bounds
            if (newPoint.x < areaMin.x || newPoint.x >= areaMax.x ||
                newPoint.y < areaMin.y || newPoint.y >= areaMax.y) {
                continue;
            }
            
            // Check grid cell
            int newGridX = static_cast<int>((newPoint.x - areaMin.x) / cellSize);
            int newGridY = static_cast<int>((newPoint.y - areaMin.y) / cellSize);
            
            if (newGridX < 0 || newGridX >= gridWidth || newGridY < 0 || newGridY >= gridHeight) {
                continue;
            }
            
            // Check neighboring cells for conflicts
            bool valid = true;
            for (int dy = -2; dy <= 2 && valid; ++dy) {
                for (int dx = -2; dx <= 2 && valid; ++dx) {
                    int checkX = newGridX + dx;
                    int checkY = newGridY + dy;
                    
                    if (checkX < 0 || checkX >= gridWidth || checkY < 0 || checkY >= gridHeight) {
                        continue;
                    }
                    
                    int neighborIdx = grid[checkY * gridWidth + checkX];
                    if (neighborIdx >= 0) {
                        float distSq = glm::dot(newPoint - points[neighborIdx], newPoint - points[neighborIdx]);
                        if (distSq < minDistance * minDistance) {
                            valid = false;
                        }
                    }
                }
            }
            
            if (valid) {
                int newIdx = static_cast<int>(points.size());
                points.push_back(newPoint);
                activeList.push_back(newIdx);
                grid[newGridY * gridWidth + newGridX] = newIdx;
                foundValid = true;
                break;
            }
        }
        
        if (!foundValid) {
            // Remove from active list
            activeList[activeIdx] = activeList.back();
            activeList.pop_back();
        }
    }
    
    outPositions = std::move(points);
}

bool InstancerDistributor::SampleTerrainHeight(
    const TerrainComponent* terrain,
    const glm::mat4& terrainTransform,
    const glm::vec2& xz,
    float& outHeight)
{
    if (!terrain) return false;
    
    // Transform world position to terrain local space
    glm::mat4 invTransform = glm::inverse(terrainTransform);
    glm::vec4 localPosH = invTransform * glm::vec4(xz.x, 0, xz.y, 1.0f);
    glm::vec3 localPos = glm::vec3(localPosH) / localPosH.w;
    
    // Check bounds
    if (localPos.x < 0 || localPos.x > terrain->WorldSize.x ||
        localPos.z < 0 || localPos.z > terrain->WorldSize.y) {
        return false;
    }
    if (Terrain::IsHoleAtLocal(*terrain, localPos.x, localPos.z)) {
        return false;
    }
    
    // Sample height
    float localHeight = Terrain::SampleHeightWorld(*terrain, localPos.x, localPos.z);
    
    // Transform back to world space
    glm::vec4 worldPosH = terrainTransform * glm::vec4(localPos.x, localHeight, localPos.z, 1.0f);
    outHeight = worldPosH.y / worldPosH.w;
    
    return true;
}

bool InstancerDistributor::SampleTerrainSlope(
    const TerrainComponent* terrain,
    const glm::mat4& terrainTransform,
    const glm::vec2& xz,
    float& outSlopeDegrees,
    glm::vec3& outNormal)
{
    if (!terrain) return false;
    
    // Transform world position to terrain local space
    glm::mat4 invTransform = glm::inverse(terrainTransform);
    glm::vec4 localPosH = invTransform * glm::vec4(xz.x, 0, xz.y, 1.0f);
    glm::vec3 localPos = glm::vec3(localPosH) / localPosH.w;
    
    // Check bounds
    if (localPos.x < 0 || localPos.x > terrain->WorldSize.x ||
        localPos.z < 0 || localPos.z > terrain->WorldSize.y) {
        return false;
    }
    if (Terrain::IsHoleAtLocal(*terrain, localPos.x, localPos.z)) {
        return false;
    }
    
    // Sample normal
    glm::vec3 localNormal = Terrain::SampleNormal(*terrain, localPos.x, localPos.z);
    
    // Transform normal to world space
    glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(terrainTransform)));
    outNormal = glm::normalize(normalMatrix * localNormal);
    
    // Calculate slope angle
    float dotUp = glm::dot(outNormal, glm::vec3(0, 1, 0));
    outSlopeDegrees = glm::degrees(std::acos(glm::clamp(dotUp, -1.0f, 1.0f)));
    
    return true;
}

uint32_t InstancerDistributor::GenerateInstanceID(const glm::vec3& position, uint32_t seed) {
    // Create stable ID from position (quantized) and seed
    int32_t qx = static_cast<int32_t>(std::round(position.x * 100.0f));
    int32_t qy = static_cast<int32_t>(std::round(position.y * 100.0f));
    int32_t qz = static_cast<int32_t>(std::round(position.z * 100.0f));
    
    uint32_t hash = seed;
    hash ^= static_cast<uint32_t>(qx) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= static_cast<uint32_t>(qy) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= static_cast<uint32_t>(qz) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    
    return hash;
}

void InstancerDistributor::ApplyRandomVariation(
    InstanceData& inst,
    const DistributionSettings& settings,
    const glm::vec3& terrainNormal,
    std::mt19937& rng)
{
    // Scale variation
    if (settings.NonUniformScale) {
        std::uniform_real_distribution<float> scaleDistX(settings.MinScaleVec.x, settings.MaxScaleVec.x);
        std::uniform_real_distribution<float> scaleDistY(settings.MinScaleVec.y, settings.MaxScaleVec.y);
        std::uniform_real_distribution<float> scaleDistZ(settings.MinScaleVec.z, settings.MaxScaleVec.z);
        inst.Scale = glm::vec3(scaleDistX(rng), scaleDistY(rng), scaleDistZ(rng));
    } else {
        std::uniform_real_distribution<float> scaleDist(settings.MinScale, settings.MaxScale);
        float s = scaleDist(rng);
        inst.Scale = glm::vec3(s);
    }
    
    // Rotation variation
    std::uniform_real_distribution<float> yawDist(-settings.YawVarianceDegrees * 0.5f, settings.YawVarianceDegrees * 0.5f);
    std::uniform_real_distribution<float> pitchDist(-settings.PitchVarianceDegrees * 0.5f, settings.PitchVarianceDegrees * 0.5f);
    std::uniform_real_distribution<float> rollDist(-settings.RollVarianceDegrees * 0.5f, settings.RollVarianceDegrees * 0.5f);
    
    glm::vec3 euler(
        glm::radians(pitchDist(rng)),
        glm::radians(yawDist(rng)),
        glm::radians(rollDist(rng)));
    
    inst.Rotation = glm::quat(euler);
    
    // Slope alignment
    if (settings.AlignToSlope && glm::length(terrainNormal - glm::vec3(0, 1, 0)) > 0.001f) {
        glm::vec3 up(0, 1, 0);
        glm::quat slopeRot = glm::rotation(up, terrainNormal);
        inst.Rotation = glm::slerp(inst.Rotation, slopeRot * inst.Rotation, settings.SlopeAlignmentFactor);
    }
}

//------------------------------------------------------------------------------
// InstancerSwapSystem Implementation
//------------------------------------------------------------------------------
// NOTE: If no prefab is assigned to the instancer, this system does nothing.
// All instances remain in Pristine state and are rendered via GPU instancing.
// Hot-swap to prefabs only occurs when a prefab is assigned.
//------------------------------------------------------------------------------

void InstancerSwapSystem::Update(
    InstancerComponent& comp,
    Scene& scene,
    const glm::vec3& cameraPos,
    float deltaTime)
{
    const auto startTime = std::chrono::high_resolution_clock::now();
    (void)deltaTime;
    
    // No prefab = instancing only mode - all instances stay Pristine and get rendered
    if (!comp.Enabled || !comp.HasValidPrefab()) return;
    
    const float swapDistance = comp.Swap.SwapDistance;
    const float hysteresis = comp.Swap.SwapHysteresis;
    const float outerDistance = swapDistance + hysteresis;
    const float innerDistance = swapDistance - hysteresis;
    
    m_CurrentActivePrefabs = 0;

    std::vector<uint32_t> activeIndices;
    activeIndices.reserve(comp.Runtime.ActivePrefabIndices.size());
    for (uint32_t index : comp.Runtime.ActivePrefabIndices) {
        activeIndices.push_back(index);
    }

    for (uint32_t i : activeIndices) {
        if (i >= comp.Runtime.Instances.size()) {
            comp.Runtime.ActivePrefabIndices.erase(i);
            continue;
        }

        InstanceData& inst = comp.Runtime.Instances[i];
        if (inst.State == InstanceState::Destroyed || inst.ActivePrefabEntity == INVALID_ENTITY_ID) {
            comp.Runtime.ActivePrefabIndices.erase(i);
            continue;
        }

        if (IsPrefabDestroyed(scene, inst.ActivePrefabEntity)) {
            comp.MarkInstanceDestroyed(inst.InstanceID);
            inst.ActivePrefabEntity = INVALID_ENTITY_ID;
            continue;
        }

        if (inst.State == InstanceState::Active && IsPrefabModified(scene, inst.ActivePrefabEntity)) {
            comp.MarkInstanceModified(inst.InstanceID);
        }

        const float distance = glm::length(inst.Position - cameraPos);
        inst.DistanceToCamera = distance;

        if (inst.State == InstanceState::Active && distance > outerDistance) {
            SwapToInstance(comp, scene, i);
            continue;
        }

        if (inst.State == InstanceState::Active || inst.State == InstanceState::Modified) {
            ++m_CurrentActivePrefabs;
        }
    }

    if (m_CurrentActivePrefabs < comp.Swap.MaxActivePrefabs && innerDistance > 0.0f) {
        std::vector<uint32_t> nearby;
        comp.Runtime.QueryNearby(cameraPos, innerDistance, nearby);
        for (uint32_t i : nearby) {
            if (m_CurrentActivePrefabs >= comp.Swap.MaxActivePrefabs || i >= comp.Runtime.Instances.size()) {
                break;
            }

            InstanceData& inst = comp.Runtime.Instances[i];
            if (inst.State != InstanceState::Pristine) {
                continue;
            }

            const float distance = glm::length(inst.Position - cameraPos);
            inst.DistanceToCamera = distance;
            if (distance >= innerDistance) {
                continue;
            }

            EntityID prefabId = SwapToPrefab(comp, scene, i);
            if (prefabId != INVALID_ENTITY_ID) {
                ++m_CurrentActivePrefabs;
            }
        }
    }
    
    comp.Runtime.ActivePrefabs = m_CurrentActivePrefabs;
    comp.Stats.ActivePrefabs = m_CurrentActivePrefabs;

    if (InstancerDiagnosticsEnabled()) {
        const auto endTime = std::chrono::high_resolution_clock::now();
        const float elapsedMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();
        std::cout << "[InstancerDiag] swap active=" << m_CurrentActivePrefabs
                  << " total=" << comp.Runtime.TotalInstances
                  << " ms=" << elapsedMs << "\n";
    }
}

EntityID InstancerSwapSystem::SwapToPrefab(
    InstancerComponent& comp,
    Scene& scene,
    uint32_t instanceIndex)
{
    ScopedInstancerTimer timer("prefab instantiate");
    if (instanceIndex >= comp.Runtime.Instances.size()) return INVALID_ENTITY_ID;
    
    InstanceData& inst = comp.Runtime.Instances[instanceIndex];
    
    // Instantiate the prefab
    EntityID prefabId = InstantiatePrefab(comp.PrefabAsset.guid, scene);
    if (prefabId == INVALID_ENTITY_ID) return INVALID_ENTITY_ID;
    
    // Set transform
    if (auto* data = scene.GetEntityData(prefabId)) {
        data->Transform.Position = inst.Position;
        data->Transform.RotationQ = inst.Rotation;
        data->Transform.Scale = inst.Scale;
        data->Transform.UseQuatRotation = true;
        scene.MarkTransformDirty(prefabId);
    }
    
    inst.State = InstanceState::Active;
    inst.ActivePrefabEntity = prefabId;
    comp.Runtime.ActivePrefabIndices.insert(instanceIndex);
    
    return prefabId;
}

void InstancerSwapSystem::SwapToInstance(
    InstancerComponent& comp,
    Scene& scene,
    uint32_t instanceIndex)
{
    if (instanceIndex >= comp.Runtime.Instances.size()) return;
    
    InstanceData& inst = comp.Runtime.Instances[instanceIndex];
    
    if (inst.ActivePrefabEntity != INVALID_ENTITY_ID) {
        scene.DestroyEntity(inst.ActivePrefabEntity);
        inst.ActivePrefabEntity = INVALID_ENTITY_ID;
    }
    
    inst.State = InstanceState::Pristine;
    comp.Runtime.ActivePrefabIndices.erase(instanceIndex);
}

bool InstancerSwapSystem::IsPrefabModified(Scene& scene, EntityID prefabRoot) {
    auto* data = scene.GetEntityData(prefabRoot);
    if (!data) return false;
    
    // Check if entity has PrefabInstanceComponent with overrides
    if (data->PrefabInstance && !data->PrefabInstance->Overrides.empty()) {
        return true;
    }
    
    return false;
}

bool InstancerSwapSystem::IsPrefabDestroyed(Scene& scene, EntityID prefabRoot) {
    return scene.GetEntityData(prefabRoot) == nullptr;
}

//------------------------------------------------------------------------------
// InstancerRenderer Implementation
//------------------------------------------------------------------------------

InstancerRenderer& InstancerRenderer::Instance() {
    static InstancerRenderer instance;
    return instance;
}

InstancerRenderer::InstancerRenderer() = default;

void InstancerRenderer::EnsureInitialized() {
    if (m_Initialized) return;
    
    std::cout << "[InstancerRenderer] Initializing..." << std::endl;
    
    // Load instanced PBR shader
    m_InstancedProgram = ShaderManager::Instance().LoadProgram("vs_pbr_instanced", "fs_pbr");
    if (!bgfx::isValid(m_InstancedProgram)) {
        std::cerr << "[InstancerRenderer] FAILED to load vs_pbr_instanced shader!" << std::endl;
        return;
    }

    m_ShadowDepthProgram = ShaderManager::Instance().LoadProgram("vs_depth_instanced", "fs_depth");
    m_ShadowDepthCutoutProgram = ShaderManager::Instance().LoadProgram("vs_depth_instanced_cutout", "fs_depth_alpha_cutout");
    m_PointShadowDepthProgram = ShaderManager::Instance().LoadProgram("vs_depth_instanced", "fs_point_shadow_depth");
    m_PointShadowDepthCutoutProgram = ShaderManager::Instance().LoadProgram("vs_depth_instanced_cutout", "fs_point_shadow_depth_alpha_cutout");
    
    // Instance layout: 4 vec4s = model matrix columns
    // Must match varying.def.sc: i_data0=TEXCOORD7, i_data1=TEXCOORD6, i_data2=TEXCOORD5, i_data3=TEXCOORD4
    m_InstanceLayout.begin()
        .add(bgfx::Attrib::TexCoord7, 4, bgfx::AttribType::Float)  // Column 0
        .add(bgfx::Attrib::TexCoord6, 4, bgfx::AttribType::Float)  // Column 1
        .add(bgfx::Attrib::TexCoord5, 4, bgfx::AttribType::Float)  // Column 2
        .add(bgfx::Attrib::TexCoord4, 4, bgfx::AttribType::Float)  // Column 3
        .end();
    
    // Create uniforms
    m_u_cameraPos = bgfx::createUniform("u_cameraPos", bgfx::UniformType::Vec4);
    m_u_lightViewProj = bgfx::createUniform("u_lightViewProj", bgfx::UniformType::Mat4);
    m_u_PBRScalar1 = bgfx::createUniform("u_PBRScalar1", bgfx::UniformType::Vec4);
    m_u_pointShadowLightPosRangeDepth = bgfx::createUniform("u_pointShadowLightPosRangeDepth", bgfx::UniformType::Vec4);
    m_u_UVTransform = bgfx::createUniform("u_UVTransform", bgfx::UniformType::Vec4);
    m_s_albedo = bgfx::createUniform("s_albedo", bgfx::UniformType::Sampler);
    m_s_normal = bgfx::createUniform("s_normal", bgfx::UniformType::Sampler);
    m_s_metalRoughness = bgfx::createUniform("s_metalRoughness", bgfx::UniformType::Sampler);
    
    // Create default textures
    const uint32_t white = 0xFFFFFFFF;
    m_WhiteTexture = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8,
        BGFX_TEXTURE_NONE | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
        bgfx::copy(&white, sizeof(white)));
    
    const uint32_t flatNormal = 0xFFFF8080;  // R=128, G=128, B=255, A=255
    m_DefaultNormal = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8,
        BGFX_TEXTURE_NONE | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
        bgfx::copy(&flatNormal, sizeof(flatNormal)));
    
    m_Initialized = true;
    std::cout << "[InstancerRenderer] Initialized successfully" << std::endl;
}

bool InstancerRenderer::LoadMeshData(InstancerComponent& comp) {
    ScopedInstancerTimer loadTimer("mesh load");
#ifdef CLAYMORE_RUNTIME
    // Runtime mesh loading via MeshBinaryLoader
    if (!comp.HasValidMesh()) return false;
    if (comp.CachedMesh.Valid) return true;
    
    comp.CachedMesh.Release();
    
    // Resolve GUID to meshbin path using RuntimeAssetResolver
    IAssetResolver* resolver = Assets::GetResolver();
    if (!resolver) {
        std::cerr << "[InstancerRenderer] No asset resolver available" << std::endl;
        return false;
    }
    
    std::string meshPath = resolver->GetPathForGUID(comp.MeshAsset.guid);
    if (meshPath.empty()) {
        std::cerr << "[InstancerRenderer] Cannot resolve GUID: " << comp.MeshAsset.guid.ToString() << std::endl;
        return false;
    }

    const std::string cacheKey = MakeSharedMeshCacheKey(comp.MeshAsset.guid, meshPath, std::string());
    if (TryUseSharedMeshCache(cacheKey, comp)) {
        return true;
    }
    
    // Get submesh count
    uint32_t submeshCount = MeshBinaryLoader::GetSubmeshCount(meshPath);
    if (submeshCount == 0) {
        std::cerr << "[InstancerRenderer] No submeshes found in: " << meshPath << std::endl;
        return false;
    }
    
    // Load each submesh and create batches
    glm::vec3 boundsMin(std::numeric_limits<float>::max());
    glm::vec3 boundsMax(std::numeric_limits<float>::lowest());
    
    for (uint32_t meshIdx = 0; meshIdx < submeshCount; ++meshIdx) {
        bool isSkinned = false;
        auto meshPtr = MeshBinaryLoader::LoadMesh(meshPath, meshIdx, &isSkinned);
        if (!meshPtr) continue;
        
        // Store reference to first mesh for bounds
        if (meshIdx == 0) {
            comp.CachedMesh.MeshPtr = meshPtr;
        }
        
        // Expand bounds
        boundsMin = glm::min(boundsMin, meshPtr->BoundsMin);
        boundsMax = glm::max(boundsMax, meshPtr->BoundsMax);
        
        // Create batch for each submesh within this mesh
        if (!meshPtr->Submeshes.empty()) {
            for (size_t subIdx = 0; subIdx < meshPtr->Submeshes.size(); ++subIdx) {
                const auto& sm = meshPtr->Submeshes[subIdx];
                
                MeshInstanceBatch batch;
                batch.VBH = meshPtr->vbh;
                batch.IBH = meshPtr->ibh;
                batch.IndexStart = sm.indexStart;
                batch.IndexCount = sm.indexCount;
                batch.MaterialSlot = sm.materialSlot;
                batch.MeshRef = meshPtr;
                batch.LocalTransform = glm::mat4(1.0f);
                
                // Create default PBR material
                batch.BatchMaterial = MaterialManager::Instance().CreateDefaultPBRMaterial();
                
                comp.CachedMesh.Batches.push_back(std::move(batch));
            }
        } else {
            // No submeshes defined - draw entire mesh as single batch
            MeshInstanceBatch batch;
            batch.VBH = meshPtr->vbh;
            batch.IBH = meshPtr->ibh;
            batch.IndexStart = 0;
            batch.IndexCount = meshPtr->numIndices;
            batch.MaterialSlot = 0;
            batch.MeshRef = meshPtr;
            batch.LocalTransform = glm::mat4(1.0f);
            
            batch.BatchMaterial = MaterialManager::Instance().CreateDefaultPBRMaterial();
            
            comp.CachedMesh.Batches.push_back(std::move(batch));
        }
    }
    
    comp.CachedMesh.BoundsMin = boundsMin;
    comp.CachedMesh.BoundsMax = boundsMax;
    comp.CachedMesh.Valid = !comp.CachedMesh.Batches.empty();
    comp.NeedsMeshReload = false;
    
    if (comp.CachedMesh.Valid) {
        std::cout << "[InstancerRenderer] Runtime loaded mesh with " 
                  << comp.CachedMesh.Batches.size() << " batches" << std::endl;
        StoreSharedMeshCache(cacheKey, std::string(), meshPath, std::string(), comp.CachedMesh);
    }
    
    return comp.CachedMesh.Valid;
#else
    if (!comp.HasValidMesh()) return false;
    if (comp.CachedMesh.Valid) return true;
    
    comp.CachedMesh.Release();
    
    // Get asset entry
    auto* assetEntry = AssetLibrary::Instance().GetAsset(comp.MeshAsset.guid);
    if (!assetEntry) {
        std::cerr << "[InstancerRenderer] Asset not found: " << comp.MeshAsset.guid.ToString() << std::endl;
        return false;
    }
    
    std::string assetPath = assetEntry->path;
    std::string meshPath;
    std::string metaPath;
    std::vector<std::string> cacheDependencies;
    cacheDependencies.push_back(assetPath);
    
    // Check extension to determine how to resolve meshbin path
    std::filesystem::path srcPath(assetPath);
    std::string ext = srcPath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    if (ext == ".meshbin") {
        // Already a meshbin file
        meshPath = assetPath;
        // Derive meta path from meshbin
        metaPath = (srcPath.parent_path() / (srcPath.stem().string() + ".meta")).string();
    } else {
        // Use model cache paths only when already available; do not import on slot/render.
        BuiltModelPaths built;
        if (HasModelCache(assetPath, built)) {
            meshPath = built.meshPath;
            metaPath = built.metaPath;
        } else {
            // Fallback: construct paths manually
            meshPath = (srcPath.parent_path() / (srcPath.stem().string() + ".meshbin")).string();
            metaPath = (srcPath.parent_path() / (srcPath.stem().string() + ".meta")).string();
        }
        
        // Normalize to forward slashes for VFS consistency
        for (char& c : meshPath) {
            if (c == '\\') c = '/';
        }
        for (char& c : metaPath) {
            if (c == '\\') c = '/';
        }
    }

    const std::string cacheKey = MakeSharedMeshCacheKey(comp.MeshAsset.guid, meshPath, metaPath);
    if (TryUseSharedMeshCache(cacheKey, comp)) {
        return true;
    }

    cacheDependencies.push_back(meshPath);
    cacheDependencies.push_back(metaPath);
    
    // Load meta file to get material sources and transforms
    nlohmann::json meta;
    std::vector<std::vector<MaterialSource>> meshMaterials;  // materials[meshIdx][slotIdx]
    std::vector<glm::mat4> meshTransforms;  // transforms[meshIdx] - local transform per mesh entry
    {
        // Try to open meta file from disk
        std::ifstream metaIn(metaPath);
        if (!metaIn.is_open()) {
            // Try with project directory prefix
            std::string fullMetaPath = (Project::GetProjectDirectory() / metaPath).string();
            metaIn.open(fullMetaPath);
        }
        if (metaIn.is_open()) {
            try {
                metaIn >> meta;
                // Parse materials and transforms from entries
                if (meta.contains("entries") && meta["entries"].is_array()) {
                    for (const auto& entry : meta["entries"]) {
                        // Parse materials
                        std::vector<MaterialSource> entryMats;
                        if (entry.contains("materials") && entry["materials"].is_array()) {
                            for (const auto& matJson : entry["materials"]) {
                                MaterialSource source = material_serialization::FromJson(matJson);
                                AddMaterialSourceDependencies(cacheDependencies, source);
                                entryMats.push_back(std::move(source));
                            }
                        }
                        meshMaterials.push_back(std::move(entryMats));
                        
                        // Parse transform from meta (the "m" array is column-major 4x4 matrix)
                        glm::mat4 localXform(1.0f);
                        if (entry.contains("transform") && entry["transform"].contains("m") && 
                            entry["transform"]["m"].is_array() && entry["transform"]["m"].size() == 16) {
                            const auto& m = entry["transform"]["m"];
                            for (int col = 0; col < 4; ++col) {
                                for (int row = 0; row < 4; ++row) {
                                    localXform[col][row] = m[col * 4 + row].get<float>();
                                }
                            }
                        }
                        meshTransforms.push_back(localXform);
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "[InstancerRenderer] Failed to parse meta file: " << e.what() << std::endl;
            }
        } else {
            std::cerr << "[InstancerRenderer] Could not open meta file: " << metaPath << std::endl;
        }
    }
    
    // Let meshbin functions handle VFS resolution via EnsureFileOnDisk
    uint32_t submeshCount = meshbin::GetSubmeshCount(meshPath);
    if (submeshCount == 0) {
        std::cerr << "[InstancerRenderer] No submeshes found in: " << meshPath << std::endl;
        return false;
    }
    
    // Load each mesh entry from the meshbin and create batches for each submesh
    bool isSkinned = false;
    glm::vec3 boundsMin(std::numeric_limits<float>::max());
    glm::vec3 boundsMax(std::numeric_limits<float>::lowest());
    
    for (uint32_t meshIdx = 0; meshIdx < submeshCount; ++meshIdx) {
        auto meshPtr = meshbin::ReadMeshFromBin(meshPath, meshIdx, isSkinned, nullptr);
        if (!meshPtr) continue;
        
        // Store reference to first mesh for bounds
        if (meshIdx == 0) {
            comp.CachedMesh.MeshPtr = meshPtr;
        }
        
        // Get local transform for this mesh entry from meta file (preferred) or meshbin (fallback)
        glm::mat4 localTransform(1.0f);
        bool hasLocalTransform = false;
        if (meshIdx < meshTransforms.size()) {
            localTransform = meshTransforms[meshIdx];
            hasLocalTransform = (localTransform != glm::mat4(1.0f));
        }
        if (!hasLocalTransform) {
            // Fallback to meshbin transform
            meshbin::TransformInfo xformInfo = meshbin::GetSubmeshLocalTransform(meshPath, meshIdx);
            if (xformInfo.has) {
                localTransform = xformInfo.matrix;
                hasLocalTransform = true;
            }
        }
        
        // Expand bounds (transform bounds by local transform)
        if (hasLocalTransform) {
            glm::vec3 corners[8] = {
                glm::vec3(meshPtr->BoundsMin.x, meshPtr->BoundsMin.y, meshPtr->BoundsMin.z),
                glm::vec3(meshPtr->BoundsMax.x, meshPtr->BoundsMin.y, meshPtr->BoundsMin.z),
                glm::vec3(meshPtr->BoundsMin.x, meshPtr->BoundsMax.y, meshPtr->BoundsMin.z),
                glm::vec3(meshPtr->BoundsMax.x, meshPtr->BoundsMax.y, meshPtr->BoundsMin.z),
                glm::vec3(meshPtr->BoundsMin.x, meshPtr->BoundsMin.y, meshPtr->BoundsMax.z),
                glm::vec3(meshPtr->BoundsMax.x, meshPtr->BoundsMin.y, meshPtr->BoundsMax.z),
                glm::vec3(meshPtr->BoundsMin.x, meshPtr->BoundsMax.y, meshPtr->BoundsMax.z),
                glm::vec3(meshPtr->BoundsMax.x, meshPtr->BoundsMax.y, meshPtr->BoundsMax.z)
            };
            for (int c = 0; c < 8; ++c) {
                glm::vec3 tc = glm::vec3(localTransform * glm::vec4(corners[c], 1.0f));
                boundsMin = glm::min(boundsMin, tc);
                boundsMax = glm::max(boundsMax, tc);
            }
        } else {
            boundsMin = glm::min(boundsMin, meshPtr->BoundsMin);
            boundsMax = glm::max(boundsMax, meshPtr->BoundsMax);
        }
        
        // Get materials for this mesh entry from meta file
        const std::vector<MaterialSource>* entryMats = nullptr;
        if (meshIdx < meshMaterials.size()) {
            entryMats = &meshMaterials[meshIdx];
        }
        
        // Each mesh entry can have multiple submeshes (material slots)
        // Iterate over the Submeshes array to create proper batches
        if (!meshPtr->Submeshes.empty()) {
            for (size_t subIdx = 0; subIdx < meshPtr->Submeshes.size(); ++subIdx) {
                const auto& sm = meshPtr->Submeshes[subIdx];
                
                MeshInstanceBatch batch;
                batch.VBH = meshPtr->vbh;
                batch.IBH = meshPtr->ibh;
                batch.IndexStart = sm.indexStart;
                batch.IndexCount = sm.indexCount;
                batch.MaterialSlot = sm.materialSlot;
                batch.MeshRef = meshPtr;  // Keep mesh alive
                batch.LocalTransform = localTransform;  // Apply mesh's local transform
                
                // Create material from MaterialSource if available (from meta file)
                auto pbr = MaterialManager::Instance().CreateDefaultPBRMaterial();
                if (pbr) {
                    // Use MaterialSource from meta file if available
                    size_t matSlot = sm.materialSlot < (entryMats ? entryMats->size() : 0) ? sm.materialSlot : 0;
                    if (entryMats && matSlot < entryMats->size()) {
                        const MaterialSource& ms = (*entryMats)[matSlot];
                        if (!ms.Albedo.Path.empty()) {
                            pbr->SetAlbedoTextureFromPath(ms.Albedo.Path);
                        }
                        if (!ms.Normal.Path.empty()) {
                            pbr->SetNormalTextureFromPath(ms.Normal.Path);
                        }
                        if (!ms.MetallicRoughness.Path.empty()) {
                            pbr->SetMetallicRoughnessTextureFromPath(ms.MetallicRoughness.Path);
                        }
                        if (!ms.AO.Path.empty()) {
                            pbr->SetAmbientOcclusionTextureFromPath(ms.AO.Path);
                        }
                        if (!ms.Emission.Path.empty()) {
                            pbr->SetEmissionTextureFromPath(ms.Emission.Path);
                        }
                        if (ms.TwoSided) {
                            pbr->m_StateFlags &= ~(BGFX_STATE_CULL_CW | BGFX_STATE_CULL_CCW);
                        }
                        if (ms.AlphaBlend) {
                            pbr->m_StateFlags |= BGFX_STATE_BLEND_ALPHA;
                        }
                        if (ms.AlphaCutout) {
                            batch.UseAlphaCutout = true;
                            batch.AlphaCutoutThreshold = ms.AlphaCutoutThreshold;
                            pbr->m_StateFlags &= ~BGFX_STATE_BLEND_ALPHA;
                            pbr->SetUniform("u_PBRScalar1", glm::vec4(pbr->GetEmissionStrength(), ms.AlphaCutoutThreshold, 0.0f, 0.0f));
                        }
                    } else {
                        // Fallback to texture hints from meshbin
                        meshbin::TextureHints hints = meshbin::GetSubmeshTextureHints(meshPath, meshIdx);
                        if (!hints.albedo.empty()) {
                            cacheDependencies.push_back(hints.albedo);
                            pbr->SetAlbedoTextureFromPath(hints.albedo);
                        }
                        if (!hints.normal.empty()) {
                            cacheDependencies.push_back(hints.normal);
                            pbr->SetNormalTextureFromPath(hints.normal);
                        }
                        if (!hints.metallicRoughness.empty()) {
                            cacheDependencies.push_back(hints.metallicRoughness);
                            pbr->SetMetallicRoughnessTextureFromPath(hints.metallicRoughness);
                        }
                    }
                    batch.BatchMaterial = pbr;
                } else {
                    batch.BatchMaterial = MaterialManager::Instance().CreateDefaultPBRMaterial();
                }
                
                comp.CachedMesh.Batches.push_back(std::move(batch));
            }
        } else {
            // No submeshes defined - draw entire mesh as single batch
            MeshInstanceBatch batch;
            batch.VBH = meshPtr->vbh;
            batch.IBH = meshPtr->ibh;
            batch.IndexStart = 0;
            batch.IndexCount = meshPtr->numIndices;
            batch.MaterialSlot = 0;
            batch.MeshRef = meshPtr;
            batch.LocalTransform = localTransform;
            
            auto pbr = MaterialManager::Instance().CreateDefaultPBRMaterial();
            if (pbr) {
                // Use first MaterialSource if available
                if (entryMats && !entryMats->empty()) {
                    const MaterialSource& ms = (*entryMats)[0];
                    if (!ms.Albedo.Path.empty()) {
                        pbr->SetAlbedoTextureFromPath(ms.Albedo.Path);
                    }
                    if (!ms.Normal.Path.empty()) {
                        pbr->SetNormalTextureFromPath(ms.Normal.Path);
                    }
                    if (!ms.MetallicRoughness.Path.empty()) {
                        pbr->SetMetallicRoughnessTextureFromPath(ms.MetallicRoughness.Path);
                    }
                    if (!ms.AO.Path.empty()) {
                        pbr->SetAmbientOcclusionTextureFromPath(ms.AO.Path);
                    }
                    if (!ms.Emission.Path.empty()) {
                        pbr->SetEmissionTextureFromPath(ms.Emission.Path);
                    }
                    if (ms.TwoSided) {
                        pbr->m_StateFlags &= ~(BGFX_STATE_CULL_CW | BGFX_STATE_CULL_CCW);
                    }
                    if (ms.AlphaBlend) {
                        pbr->m_StateFlags |= BGFX_STATE_BLEND_ALPHA;
                    }
                    if (ms.AlphaCutout) {
                        batch.UseAlphaCutout = true;
                        batch.AlphaCutoutThreshold = ms.AlphaCutoutThreshold;
                        pbr->m_StateFlags &= ~BGFX_STATE_BLEND_ALPHA;
                        pbr->SetUniform("u_PBRScalar1", glm::vec4(pbr->GetEmissionStrength(), ms.AlphaCutoutThreshold, 0.0f, 0.0f));
                    }
                } else {
                    // Fallback to texture hints
                    meshbin::TextureHints hints = meshbin::GetSubmeshTextureHints(meshPath, meshIdx);
                    if (!hints.albedo.empty()) {
                        cacheDependencies.push_back(hints.albedo);
                        pbr->SetAlbedoTextureFromPath(hints.albedo);
                    }
                    if (!hints.normal.empty()) {
                        cacheDependencies.push_back(hints.normal);
                        pbr->SetNormalTextureFromPath(hints.normal);
                    }
                    if (!hints.metallicRoughness.empty()) {
                        cacheDependencies.push_back(hints.metallicRoughness);
                        pbr->SetMetallicRoughnessTextureFromPath(hints.metallicRoughness);
                    }
                }
                batch.BatchMaterial = pbr;
            } else {
                batch.BatchMaterial = MaterialManager::Instance().CreateDefaultPBRMaterial();
            }
            
            comp.CachedMesh.Batches.push_back(std::move(batch));
        }
    }
    
    comp.CachedMesh.BoundsMin = boundsMin;
    comp.CachedMesh.BoundsMax = boundsMax;
    comp.CachedMesh.Valid = !comp.CachedMesh.Batches.empty();
    comp.NeedsMeshReload = false;
    
    std::cout << "[InstancerRenderer] Loaded mesh with " << comp.CachedMesh.Batches.size() 
              << " batches from " << meshPath << std::endl;
    StoreSharedMeshCache(cacheKey, assetPath, meshPath, metaPath, comp.CachedMesh, cacheDependencies);
    
    return comp.CachedMesh.Valid;
#endif
}

void InstancerRenderer::Render(
    uint16_t viewId,
    InstancerComponent& comp,
    const glm::mat4& view,
    const glm::mat4& proj,
    const glm::vec3& cameraPos,
    uint64_t stateFlags)
{
    (void)view;
    (void)proj;
    
    if (!comp.Enabled) return;
    
    EnsureInitialized();
    if (!m_Initialized || !bgfx::isValid(m_InstancedProgram)) return;
    
    // Collect visible instances for GPU instanced rendering
    // - Pristine: Always rendered via instancing (or swapped to prefab if close + prefab assigned)
    // - Active: Rendered as prefab entity (skip instancing)
    // - Modified: Permanently prefab (skip instancing)  
    // - Destroyed: Never rendered
    // If no prefab is assigned, all instances stay Pristine and are always instanced.
    std::vector<glm::mat4> transforms;
    transforms.reserve(comp.Runtime.VisibleInstances);
    
    for (uint32_t index : comp.Runtime.VisibleInstanceIndices) {
        if (index >= comp.Runtime.Instances.size()) {
            continue;
        }
        const auto& inst = comp.Runtime.Instances[index];
        if (!inst.Visible) continue;
        if (inst.State == InstanceState::Destroyed) continue;
        if (inst.State == InstanceState::Active) continue;    // Rendered as prefab entity
        if (inst.State == InstanceState::Modified) continue;  // Permanently prefab
        
        // Build transform matrix
        glm::mat4 transform = glm::translate(glm::mat4(1.0f), inst.Position);
        transform = transform * glm::mat4_cast(inst.Rotation);
        transform = glm::scale(transform, inst.Scale);
        
        transforms.push_back(transform);
    }
    
    if (transforms.empty()) return;

    // Loading a dropped mesh can be expensive, so defer it until there is
    // actually something visible to draw with it.
    if (comp.NeedsMeshReload && !LoadMeshData(comp)) {
        return;
    }

    if (!comp.CachedMesh.Valid) return;
    
    // Separate opaque and transparent batches for correct depth ordering
    // (unless using alpha cutout, which makes transparent batches behave as opaque)
    std::vector<MeshInstanceBatch*> opaqueBatches;
    std::vector<MeshInstanceBatch*> transparentBatches;
    
    for (auto& batch : comp.CachedMesh.Batches) {
        if (!batch.IsValid()) continue;
        
        bool isTransparent = batch.BatchMaterial && 
            (batch.BatchMaterial->GetStateFlags() & BGFX_STATE_BLEND_ALPHA);
        
        // When using alpha cutout, treat all batches as opaque
        if (isTransparent && !comp.UseAlphaCutout && !batch.UseAlphaCutout) {
            transparentBatches.push_back(&batch);
        } else {
            opaqueBatches.push_back(&batch);
        }
    }
    
    // Render opaque batches first (includes cutout batches)
    for (auto* batch : opaqueBatches) {
        batch->InstanceTransforms.clear();
        batch->InstanceTransforms.reserve(transforms.size());
        for (const auto& t : transforms) {
            batch->InstanceTransforms.push_back(t * batch->LocalTransform);
        }
        const bool useBatchCutout = batch->UseAlphaCutout || comp.UseAlphaCutout;
        const float batchCutoutThreshold = batch->UseAlphaCutout
            ? batch->AlphaCutoutThreshold
            : comp.AlphaCutoutThreshold;
        SubmitInstancedBatch(viewId, *batch, batch->InstanceTransforms, cameraPos, stateFlags,
                            useBatchCutout, batchCutoutThreshold);
    }
    
    // Render transparent batches after opaque (only when not using alpha cutout)
    for (auto* batch : transparentBatches) {
        batch->InstanceTransforms.clear();
        batch->InstanceTransforms.reserve(transforms.size());
        for (const auto& t : transforms) {
            batch->InstanceTransforms.push_back(t * batch->LocalTransform);
        }
        SubmitInstancedBatch(viewId, *batch, batch->InstanceTransforms, cameraPos, stateFlags,
                            false, 0.0f);  // No cutout for actual transparent batches
    }
    
    comp.Stats.InstancedDraws = static_cast<uint32_t>(comp.CachedMesh.Batches.size());
}

void InstancerRenderer::SubmitInstancedBatch(
    uint16_t viewId,
    const MeshInstanceBatch& batch,
    const std::vector<glm::mat4>& transforms,
    const glm::vec3& cameraPos,
    uint64_t stateFlags,
    bool useAlphaCutout,
    float alphaCutoutThreshold)
{
    if (transforms.empty()) return;

    const bgfx::Caps* caps = bgfx::getCaps();
    const bool supportsAlphaToCoverage =
        caps != nullptr && 0 != (caps->supported & BGFX_CAPS_ALPHA_TO_COVERAGE);
    
    // Instance data: 4 vec4s = 64 bytes per instance
    struct InstanceDataGPU {
        glm::vec4 Data[4];
    };
    static_assert(sizeof(InstanceDataGPU) == 64, "InstanceDataGPU must be 64 bytes");
    
    const uint16_t stride = m_InstanceLayout.getStride();
    uint32_t instanceCount = static_cast<uint32_t>(transforms.size());
    
    // Process in reasonably large chunks so dense foliage layers do not turn
    // into hundreds of tiny instanced submits.
    uint32_t offset = 0;
    
    while (offset < instanceCount) {
        uint32_t count = std::min(instanceCount - offset, kMaxInstancedSubmitCount);
        
        // Check available buffer space
        uint32_t available = bgfx::getAvailInstanceDataBuffer(count, stride);
        if (available == 0) break;
        count = std::min(count, available);
        
        // Allocate and fill instance buffer
        bgfx::InstanceDataBuffer idb{};
        bgfx::allocInstanceDataBuffer(&idb, count, stride);
        
        InstanceDataGPU* dst = reinterpret_cast<InstanceDataGPU*>(idb.data);
        for (uint32_t i = 0; i < count; ++i) {
            const glm::mat4& m = transforms[offset + i];
            dst[i].Data[0] = m[0];  // Column 0
            dst[i].Data[1] = m[1];  // Column 1
            dst[i].Data[2] = m[2];  // Column 2
            dst[i].Data[3] = m[3];  // Column 3
        }
        
        // Set camera uniform
        glm::vec4 camPosVec4(cameraPos, 1.0f);
        bgfx::setUniform(m_u_cameraPos, &camPosVec4);
        
        // Bind material uniforms (handles textures, UV, scalar params, etc.)
        if (batch.BatchMaterial) {
            batch.BatchMaterial->BindUniforms();
        } else {
            // Fallback: bind default textures
            glm::vec4 uvTransform(1.0f, 1.0f, 0.0f, 0.0f);
            bgfx::setUniform(m_u_UVTransform, &uvTransform);
            bgfx::setTexture(0, m_s_albedo, m_WhiteTexture);
            bgfx::setTexture(1, m_s_normal, m_DefaultNormal);
            bgfx::setTexture(2, m_s_metalRoughness, m_WhiteTexture);
        }
        
        // Set u_PBRScalar1: x=emissionStrength, y=alphaCutoffThreshold
        // Must always set this to ensure correct alpha cutoff state per draw
        {
            float emissionStrength = 0.0f;
            if (batch.BatchMaterial) {
                if (auto* pbrMat = dynamic_cast<PBRMaterial*>(batch.BatchMaterial.get())) {
                    emissionStrength = pbrMat->GetEmissionStrength();
                }
            }
            // y=alphaCutoffThreshold: 0 disables cutout, >0 enables it
            float cutoff = useAlphaCutout ? alphaCutoutThreshold : 0.0f;
            glm::vec4 scalar1(emissionStrength, cutoff, 0.0f, 0.0f);
            bgfx::setUniform(m_u_PBRScalar1, &scalar1);
        }
        
        // Bind lighting, shadow, and global shader properties (required by fs_pbr)
        // Each draw call needs these uniforms set since bgfx clears uniform state after submit()
        Renderer::Get().BindLightingUniforms();
        Renderer::Get().BindShadowUniforms();
        {
            float receive = 1.0f;
            if (batch.BatchMaterial) {
                if (auto* pbrMat = dynamic_cast<PBRMaterial*>(batch.BatchMaterial.get())) {
                    if (pbrMat->GetReceiveShadowsOverride()) {
                        receive = pbrMat->GetReceiveShadows() ? 1.0f : 0.0f;
                    }
                }
            }
            Renderer::Get().SetShadowReceive(receive);
        }
        GlobalShaderProperties::Instance().Apply();
        
        // Set mesh buffers
        bgfx::setVertexBuffer(0, batch.VBH);
        bgfx::setIndexBuffer(batch.IBH, batch.IndexStart, batch.IndexCount);
        
        // Set instance data
        bgfx::setInstanceDataBuffer(&idb);
        
        // Use material's state flags (includes alpha blend, two-sided, etc.) 
        uint64_t batchState = batch.BatchMaterial ? batch.BatchMaterial->GetStateFlags() : stateFlags;
        
        // Ensure MSAA is enabled for quality matching
        batchState |= BGFX_STATE_MSAA;
        
        // Treat alpha-cutout as opaque geometry so mixed trunk/leaf meshes keep
        // self-occlusion, write scene depth, and participate correctly in fog
        // and particle depth testing.
        if (useAlphaCutout) {
            batchState &= ~BGFX_STATE_BLEND_MASK;
            batchState &= ~BGFX_STATE_BLEND_ALPHA_TO_COVERAGE;
            if (supportsAlphaToCoverage) {
                batchState |= BGFX_STATE_BLEND_ALPHA_TO_COVERAGE;
            }
            batchState |= BGFX_STATE_WRITE_Z;
            batchState &= ~BGFX_STATE_DEPTH_TEST_MASK;
            batchState |= BGFX_STATE_DEPTH_TEST_LEQUAL;
        }
        // For regular alpha-blended materials, disable depth write
        else if ((batchState & BGFX_STATE_BLEND_MASK) != 0u) {
            batchState &= ~BGFX_STATE_BLEND_ALPHA_TO_COVERAGE;
            batchState &= ~BGFX_STATE_WRITE_Z;
        } else {
            batchState &= ~BGFX_STATE_BLEND_ALPHA_TO_COVERAGE;
        }
        
        bgfx::setState(batchState);
        bgfx::submit(viewId, m_InstancedProgram);
        
        offset += count;
    }
}

bool InstancerRenderer::PrepareShadowCandidates(
    InstancerComponent& comp,
    const glm::vec3& cameraPos,
    float shadowDistance,
    float cameraNear,
    const std::array<float, kShadowCascadeCount>& cascadeSplits,
    int cascadeCount)
{
    m_ShadowCandidates.clear();
    m_PointShadowCandidates.clear();
    for (auto& candidates : m_ShadowCascadeCandidates) {
        candidates.clear();
    }

    if (!comp.Enabled) {
        return false;
    }

    EnsureInitialized();
    if (!m_Initialized) {
        return false;
    }

    if (comp.NeedsMeshReload && !LoadMeshData(comp)) {
        return false;
    }

    if (!comp.CachedMesh.Valid) {
        return false;
    }

    const glm::vec3 localCenter = 0.5f * (comp.CachedMesh.BoundsMin + comp.CachedMesh.BoundsMax);
    const float localRadius = 0.5f * glm::length(comp.CachedMesh.BoundsMax - comp.CachedMesh.BoundsMin);
    const float maxShadowDistance = glm::max(0.0f, shadowDistance) * 1.2f;

    const float maxConfiguredScale = EstimateMaxInstanceScale(comp);
    std::vector<uint32_t> nearby;
    if (maxShadowDistance > 0.0f) {
        const float queryRadius = maxShadowDistance + (localRadius + glm::length(localCenter)) * maxConfiguredScale;
        comp.Runtime.QueryNearby(cameraPos, queryRadius, nearby);
    } else {
        nearby.reserve(comp.Runtime.Instances.size());
        for (uint32_t i = 0; i < comp.Runtime.Instances.size(); ++i) {
            nearby.push_back(i);
        }
    }

    m_ShadowCandidates.reserve(nearby.size());
    for (auto& candidates : m_ShadowCascadeCandidates) {
        candidates.reserve(nearby.size());
    }

    for (uint32_t index : nearby) {
        if (index >= comp.Runtime.Instances.size()) {
            continue;
        }
        const auto& inst = comp.Runtime.Instances[index];
        if (inst.State == InstanceState::Destroyed ||
            inst.State == InstanceState::Active ||
            inst.State == InstanceState::Modified) {
            continue;
        }

        ShadowCandidate candidate;
        candidate.Transform = ComposeInstanceTransform(inst);
        candidate.CenterWS = glm::vec3(candidate.Transform * glm::vec4(localCenter, 1.0f));
        candidate.RadiusWS = localRadius * glm::max(MaxAbsScaleComponent(inst.Scale), 0.001f);

        const float distToCamera = glm::length(candidate.CenterWS - cameraPos);
        if (maxShadowDistance > 0.0f && (distToCamera - candidate.RadiusWS) > maxShadowDistance) {
            continue;
        }

        if (candidate.RadiusWS > 0.001f) {
            const float screenSize = candidate.RadiusWS / glm::max(1.0f, distToCamera);
            if (screenSize < kInstancerMinShadowScreenSize) {
                continue;
            }
        }

        if (cascadeCount > 0) {
            const float cascadeMargin = glm::clamp(candidate.RadiusWS, 1.0f, 8.0f);
            for (int ci = 0; ci < cascadeCount && ci < kShadowCascadeCount; ++ci) {
                const float cascadeNear = (ci == 0) ? cameraNear : cascadeSplits[ci - 1];
                const float cascadeFar = cascadeSplits[ci];
                if ((distToCamera + cascadeMargin) < cascadeNear ||
                    (distToCamera - cascadeMargin) > cascadeFar) {
                    continue;
                }
                candidate.CascadeMask |= static_cast<uint8_t>(1u << ci);
            }
        }

        const uint32_t candidateIndex = static_cast<uint32_t>(m_ShadowCandidates.size());
        m_ShadowCandidates.push_back(candidate);
        for (int ci = 0; ci < cascadeCount && ci < kShadowCascadeCount; ++ci) {
            if ((candidate.CascadeMask & static_cast<uint8_t>(1u << ci)) != 0u) {
                m_ShadowCascadeCandidates[ci].push_back(candidateIndex);
            }
        }
    }

    return !m_ShadowCandidates.empty();
}

bool InstancerRenderer::HasPointShadowCasterInRange(
    InstancerComponent& comp,
    const glm::vec3& lightPos,
    float lightRange,
    const glm::vec3& cameraPos,
    float shadowDistance)
{
    if (!comp.Enabled) {
        return false;
    }

    EnsureInitialized();
    if (!m_Initialized) {
        return false;
    }

    if (comp.NeedsMeshReload && !LoadMeshData(comp)) {
        return false;
    }

    if (!comp.CachedMesh.Valid) {
        return false;
    }

    const glm::vec3 localCenter = 0.5f * (comp.CachedMesh.BoundsMin + comp.CachedMesh.BoundsMax);
    const float localRadius = 0.5f * glm::length(comp.CachedMesh.BoundsMax - comp.CachedMesh.BoundsMin);

    const float maxConfiguredScale = EstimateMaxInstanceScale(comp);
    const float queryRadius = shadowDistance + lightRange + (localRadius + glm::length(localCenter)) * maxConfiguredScale;
    std::vector<uint32_t> nearby;
    comp.Runtime.QueryNearby(cameraPos, queryRadius, nearby);

    for (uint32_t index : nearby) {
        if (index >= comp.Runtime.Instances.size()) {
            continue;
        }
        const auto& inst = comp.Runtime.Instances[index];
        if (inst.State == InstanceState::Destroyed ||
            inst.State == InstanceState::Active ||
            inst.State == InstanceState::Modified) {
            continue;
        }

        const glm::mat4 transform = ComposeInstanceTransform(inst);
        const glm::vec3 centerWS = glm::vec3(transform * glm::vec4(localCenter, 1.0f));
        const float radiusWS = localRadius * glm::max(MaxAbsScaleComponent(inst.Scale), 0.001f);
        const float distToCamera = glm::length(centerWS - cameraPos);

        const float maxCameraDistance = shadowDistance + lightRange + radiusWS;
        const glm::vec3 cameraDelta = centerWS - cameraPos;
        if (glm::dot(cameraDelta, cameraDelta) > (maxCameraDistance * maxCameraDistance)) {
            continue;
        }

        if (radiusWS > 0.001f) {
            const float screenSize = radiusWS / glm::max(1.0f, distToCamera);
            if (screenSize < kInstancerMinShadowScreenSize) {
                continue;
            }
        }

        const float maxReach = lightRange + radiusWS;
        const glm::vec3 lightDelta = centerWS - lightPos;
        if (glm::dot(lightDelta, lightDelta) <= (maxReach * maxReach)) {
            return true;
        }
    }

    return false;
}

bool InstancerRenderer::ResolveShadowBatchState(
    const InstancerComponent& comp,
    const MeshInstanceBatch& batch,
    bgfx::ProgramHandle defaultProgram,
    bgfx::ProgramHandle cutoutProgram,
    bgfx::ProgramHandle& outProgram,
    bool& outUseAlphaCutout,
    float& outAlphaCutoutThreshold,
    uint64_t& inOutStateFlags) const
{
    outUseAlphaCutout = batch.UseAlphaCutout || comp.UseAlphaCutout;
    outAlphaCutoutThreshold = batch.UseAlphaCutout
        ? batch.AlphaCutoutThreshold
        : comp.AlphaCutoutThreshold;

    const bool isTransparent = batch.BatchMaterial &&
        ((batch.BatchMaterial->GetStateFlags() & BGFX_STATE_BLEND_ALPHA) != 0);
    if (isTransparent && !outUseAlphaCutout) {
        return false;
    }

    const uint64_t cullMask = BGFX_STATE_CULL_CW | BGFX_STATE_CULL_CCW;
    if (batch.BatchMaterial) {
        const uint64_t materialState = batch.BatchMaterial->GetStateFlags();
        inOutStateFlags &= ~cullMask;
        inOutStateFlags |= (materialState & cullMask);
    }

    outProgram = outUseAlphaCutout ? cutoutProgram : defaultProgram;
    return bgfx::isValid(outProgram);
}

uint32_t InstancerRenderer::SubmitShadowBatch(
    uint16_t viewId,
    const MeshInstanceBatch& batch,
    const std::vector<uint32_t>& candidateIndices,
    bgfx::ProgramHandle program,
    uint64_t stateFlags,
    const glm::mat4* lightViewProj,
    const glm::vec4* pointLightPosRangeDepth,
    bool useAlphaCutout,
    float alphaCutoutThreshold)
{
    if (candidateIndices.empty() || !bgfx::isValid(program)) {
        return 0;
    }

    const uint16_t stride = m_InstanceLayout.getStride();
    const glm::mat4 localTransform = batch.LocalTransform;
    const uint32_t candidateCount = static_cast<uint32_t>(candidateIndices.size());
    uint32_t submitCount = 0;
    uint32_t offset = 0;

    struct InstanceDataGPU {
        glm::vec4 Data[4];
    };
    static_assert(sizeof(InstanceDataGPU) == 64, "InstanceDataGPU must be 64 bytes");

    while (offset < candidateCount) {
        uint32_t count = std::min(candidateCount - offset, kMaxInstancedShadowSubmitCount);
        uint32_t available = bgfx::getAvailInstanceDataBuffer(count, stride);
        if (available == 0) {
            break;
        }
        count = std::min(count, available);

        bgfx::InstanceDataBuffer idb{};
        bgfx::allocInstanceDataBuffer(&idb, count, stride);

        InstanceDataGPU* dst = reinterpret_cast<InstanceDataGPU*>(idb.data);
        for (uint32_t i = 0; i < count; ++i) {
            const ShadowCandidate& candidate = m_ShadowCandidates[candidateIndices[offset + i]];
            const glm::mat4 model = candidate.Transform * localTransform;
            dst[i].Data[0] = model[0];
            dst[i].Data[1] = model[1];
            dst[i].Data[2] = model[2];
            dst[i].Data[3] = model[3];
        }

        if (lightViewProj) {
            bgfx::setUniform(m_u_lightViewProj, glm::value_ptr(*lightViewProj));
        }
        if (pointLightPosRangeDepth) {
            bgfx::setUniform(m_u_pointShadowLightPosRangeDepth, pointLightPosRangeDepth);
        }

        if (useAlphaCutout) {
            if (batch.BatchMaterial) {
                batch.BatchMaterial->BindUniforms();
            } else {
                const glm::vec4 uvTransform(1.0f, 1.0f, 0.0f, 0.0f);
                bgfx::setUniform(m_u_UVTransform, &uvTransform);
                bgfx::setTexture(0, m_s_albedo, m_WhiteTexture);
            }

            const glm::vec4 scalar1(0.0f, alphaCutoutThreshold, 0.0f, 0.0f);
            bgfx::setUniform(m_u_PBRScalar1, &scalar1);
        }

        bgfx::setVertexBuffer(0, batch.VBH);
        bgfx::setIndexBuffer(batch.IBH, batch.IndexStart, batch.IndexCount);
        bgfx::setInstanceDataBuffer(&idb);
        bgfx::setState(stateFlags);
        bgfx::submit(viewId, program);

        ++submitCount;
        offset += count;
    }

    return submitCount;
}

uint32_t InstancerRenderer::RenderDirectionalShadows(
    InstancerComponent& comp,
    const ShadowRenderParams& params,
    ShadowRenderStats* stats)
{
    if (m_ShadowCandidates.empty()) {
        return 0;
    }

    uint32_t submitCount = 0;
    for (const auto& batch : comp.CachedMesh.Batches) {
        if (!batch.IsValid()) {
            continue;
        }

        uint64_t batchState = params.DirectionalStateFlags;
        bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
        bool useAlphaCutout = false;
        float alphaCutoutThreshold = 0.0f;
        if (!ResolveShadowBatchState(
                comp,
                batch,
                m_ShadowDepthProgram,
                m_ShadowDepthCutoutProgram,
                program,
                useAlphaCutout,
                alphaCutoutThreshold,
                batchState)) {
            continue;
        }

        for (int ci = 0; ci < params.CascadeCount && ci < kShadowCascadeCount; ++ci) {
            const auto& cascadeCandidates = m_ShadowCascadeCandidates[ci];
            if (cascadeCandidates.empty()) {
                continue;
            }

            const uint16_t viewId = static_cast<uint16_t>(params.FirstCascadeViewId + ci);
            const uint32_t batchSubmits = SubmitShadowBatch(
                viewId,
                batch,
                cascadeCandidates,
                program,
                batchState,
                &params.CascadeMatrices[ci],
                nullptr,
                useAlphaCutout,
                alphaCutoutThreshold);
            submitCount += batchSubmits;

            if (stats && ci >= 0 && ci < kShadowCascadeCount) {
                stats->DirectionalSubmits += batchSubmits;
                stats->CascadeSubmits[ci] += batchSubmits;
            }
        }
    }

    return submitCount;
}

uint32_t InstancerRenderer::RenderPointShadows(
    InstancerComponent& comp,
    const PointShadowLightRender& light,
    uint64_t stateFlags)
{
    if (m_ShadowCandidates.empty()) {
        return 0;
    }

    m_PointShadowCandidates.clear();
    m_PointShadowCandidates.reserve(m_ShadowCandidates.size());
    for (uint32_t candidateIndex = 0; candidateIndex < static_cast<uint32_t>(m_ShadowCandidates.size()); ++candidateIndex) {
        const ShadowCandidate& candidate = m_ShadowCandidates[candidateIndex];
        const float maxReach = light.Range + candidate.RadiusWS;
        const glm::vec3 lightDelta = candidate.CenterWS - light.Position;
        if (glm::dot(lightDelta, lightDelta) <= (maxReach * maxReach)) {
            m_PointShadowCandidates.push_back(candidateIndex);
        }
    }

    if (m_PointShadowCandidates.empty()) {
        return 0;
    }

    const glm::vec4 pointLightPosRangeDepth(light.Position, glm::max(light.Range, 0.11f));
    uint32_t submitCount = 0;

    for (const auto& batch : comp.CachedMesh.Batches) {
        if (!batch.IsValid()) {
            continue;
        }

        uint64_t batchState = stateFlags;
        bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
        bool useAlphaCutout = false;
        float alphaCutoutThreshold = 0.0f;
        if (!ResolveShadowBatchState(
                comp,
                batch,
                m_PointShadowDepthProgram,
                m_PointShadowDepthCutoutProgram,
                program,
                useAlphaCutout,
                alphaCutoutThreshold,
                batchState)) {
            continue;
        }

        for (const auto& face : light.Faces) {
            submitCount += SubmitShadowBatch(
                face.ViewId,
                batch,
                m_PointShadowCandidates,
                program,
                batchState,
                &face.LightViewProj,
                &pointLightPosRangeDepth,
                useAlphaCutout,
                alphaCutoutThreshold);
        }
    }

    return submitCount;
}

void InstancerRenderer::RenderDebug(
    uint16_t viewId,
    InstancerComponent& comp,
    const glm::mat4& view,
    const glm::mat4& proj)
{
    (void)viewId;
    (void)view;
    (void)proj;
    
    if (!comp.ShowDebugMarkers) return;
    
    Renderer& renderer = Renderer::Get();
    
    // Visible instances: use preview color
    uint32_t visibleColor = (255u << 24) |
                     (static_cast<uint32_t>(comp.PreviewColor.b * 255.0f) << 16) |
                     (static_cast<uint32_t>(comp.PreviewColor.g * 255.0f) << 8) |
                     static_cast<uint32_t>(comp.PreviewColor.r * 255.0f);
    
    // Culled instances: dim gray
    uint32_t culledColor = 0xFF404040;
    
    constexpr size_t kMaxMarkers = 1000;
    size_t count = std::min(static_cast<size_t>(comp.Runtime.Instances.size()), kMaxMarkers);
    
    for (size_t i = 0; i < count; ++i) {
        const auto& inst = comp.Runtime.Instances[i];
        if (inst.State == InstanceState::Destroyed) continue;
        
        uint32_t color = inst.Visible ? visibleColor : culledColor;
        const float size = 0.3f;
        renderer.DrawDebugLineColored(
            inst.Position - glm::vec3(0, size * 0.5f, 0),
            inst.Position + glm::vec3(0, size * 1.5f, 0), color);
        renderer.DrawDebugLineColored(
            inst.Position - glm::vec3(size, 0, 0),
            inst.Position + glm::vec3(size, 0, 0), color);
        renderer.DrawDebugLineColored(
            inst.Position - glm::vec3(0, 0, size),
            inst.Position + glm::vec3(0, 0, size), color);
    }
}

//------------------------------------------------------------------------------
// InstancerSystem Implementation
//------------------------------------------------------------------------------

InstancerSystem& InstancerSystem::Instance() {
    static InstancerSystem instance;
    return instance;
}

void InstancerSystem::Update(Scene& scene, float deltaTime) {
    // Use same camera selection as Renderer: scene camera when playing, editor camera otherwise
    Camera* camera = Renderer::Get().GetCamera();
    if (!camera) return;
    
    glm::vec3 cameraPos = camera->GetPosition();
    glm::mat4 viewProj = camera->GetProjectionMatrix() * camera->GetViewMatrix();
    const bool isPlaying = scene.m_IsPlaying;
    uint32_t visibilityRefreshes = 0;
    uint32_t visibilitySkips = 0;
    uint32_t swapUpdates = 0;
    
    // Iterate all entities with InstancerComponent
    const auto& entityList = scene.GetEntities();
    for (const auto& entity : entityList) {
        EntityID id = entity.GetID();
        auto* data = scene.GetEntityData(id);
        if (!data || !data->Instancer) continue;
        if (!data->Active) continue;
        
        auto& comp = *data->Instancer;
        
        // Regenerate if needed
        const bool needsRegeneration = comp.NeedsRegeneration;
        if (comp.NeedsRegeneration) {
            const TerrainComponent* terrain = nullptr;
            glm::mat4 terrainTransform(1.0f);

            // If both Spline and Instancer exist on same entity, use spline for distribution
            if (data->Spline && !data->Spline->ControlPoints.empty()) {
                auto sampled = cm::spline::SampleSpline(
                    data->Spline->ControlPoints,
                    data->Spline->SplineSubdivision,
                    data->Spline->Closed);
                comp.ManualPoints.clear();
                comp.ManualPoints.reserve(sampled.size());
                const glm::mat4& worldMat = data->Transform.WorldMatrix;
                for (const auto& p : sampled)
                    comp.ManualPoints.push_back(glm::vec3(worldMat * glm::vec4(p, 1.0f)));
                comp.UseManualPoints = true;
            } else {
                comp.UseManualPoints = false;
            }
            
            // Get surface entity for distribution (when not using spline)
            if (comp.SurfaceEntity != INVALID_ENTITY_ID && !comp.UseManualPoints) {
                if (auto* surfaceData = scene.GetEntityData(comp.SurfaceEntity)) {
                    terrain = surfaceData->Terrain.get();
                    terrainTransform = surfaceData->Transform.WorldMatrix;
                }
            }
            
            comp.Regenerate(data->Transform.WorldMatrix, terrain, terrainTransform);
        }

        const bool refreshVisibility =
            isPlaying ||
            needsRegeneration ||
            comp.NeedsMeshReload ||
            ShouldRefreshInstancerVisibility(comp, cameraPos, viewProj);
        if (refreshVisibility) {
            comp.UpdateVisibility(cameraPos, viewProj, camera->GetProjectionMatrix());
            RecordInstancerVisibilityState(comp, cameraPos, viewProj);
            ++visibilityRefreshes;
        } else {
            ++visibilitySkips;
        }

        if (isPlaying || refreshVisibility) {
            m_SwapSystem.Update(comp, scene, cameraPos, deltaTime);
            ++swapUpdates;
        }
    }

    for (const auto& entity : entityList) {
        EntityID id = entity.GetID();
        auto* data = scene.GetEntityData(id);
        if (!data || !data->Terrain || !data->Active) {
            continue;
        }

        TerrainComponent& terrain = *data->Terrain;
        const bool rebuildAllLayers = terrain.InstancerLayersDirty;
        for (TerrainInstancerLayerDesc& layer : terrain.InstancerLayers) {
            InstancerComponent& comp = layer.Instancer;
            comp.Enabled = layer.Enabled;

            if (!layer.Enabled) {
                comp.VisibilityCacheValid = false;
                layer.ReleaseCollisionBodies();
                for (InstanceData& inst : comp.Runtime.Instances) {
                    if (inst.ActivePrefabEntity != INVALID_ENTITY_ID) {
                        scene.DestroyEntity(inst.ActivePrefabEntity);
                        inst.ActivePrefabEntity = INVALID_ENTITY_ID;
                    }
                    if (inst.State == InstanceState::Active) {
                        inst.State = InstanceState::Pristine;
                    }
                }
                comp.Runtime.ActivePrefabIndices.clear();
                comp.Runtime.ActivePrefabs = 0;
                comp.Stats.ActivePrefabs = 0;
                continue;
            }

            const bool needsFullRebuild =
                rebuildAllLayers || layer.RuntimeDirty || comp.NeedsRegeneration || layer.RuntimeRebuildInProgress;
            const bool hadRegionRebuild = layer.RuntimeRegionDirty;
            if (needsFullRebuild) {
                RegenerateTerrainInstancerLayer(scene, terrain, layer, data->Transform.WorldMatrix, m_Distributor);
            } else if (layer.RuntimeRegionDirty) {
                RegenerateTerrainInstancerLayerRegion(scene, terrain, layer, data->Transform.WorldMatrix, m_Distributor);
            }

            const bool refreshVisibility =
                isPlaying ||
                needsFullRebuild ||
                hadRegionRebuild ||
                ShouldRefreshInstancerVisibility(comp, cameraPos, viewProj);
            if (refreshVisibility) {
                comp.UpdateVisibility(cameraPos, viewProj, camera->GetProjectionMatrix());
                RecordInstancerVisibilityState(comp, cameraPos, viewProj);
                ++visibilityRefreshes;
            } else {
                ++visibilitySkips;
            }

            if (isPlaying || refreshVisibility) {
                m_SwapSystem.Update(comp, scene, cameraPos, deltaTime);
                UpdateTerrainInstancerCollision(layer, cameraPos, id);
                ++swapUpdates;
            }
        }
        if (rebuildAllLayers) {
            terrain.InstancerLayersDirty = false;
        }
    }

    Profiler::Get().SetCounter("Instancers/VisibilityRefreshes", visibilityRefreshes);
    Profiler::Get().SetCounter("Instancers/VisibilitySkipped", visibilitySkips);
    Profiler::Get().SetCounter("Instancers/SwapUpdates", swapUpdates);
}

void InstancerSystem::Render(
    uint16_t viewId,
    Scene& scene,
    const float* view,
    const glm::mat4& proj,
    const glm::vec3& cameraPos,
    uint64_t stateFlags)
{
    auto& renderer = InstancerRenderer::Instance();
    
    // Convert float[16] to glm::mat4 for internal use
    glm::mat4 viewMat = glm::make_mat4(view);
    
    const auto& entityList = scene.GetEntities();
    for (const auto& entity : entityList) {
        EntityID id = entity.GetID();
        (void)id;
        auto* data = scene.GetEntityData(id);
        if (!data || !data->Instancer) continue;
        if (!data->Active) continue;
        
        renderer.Render(viewId, *data->Instancer, viewMat, proj, cameraPos, stateFlags);
        renderer.RenderDebug(viewId, *data->Instancer, viewMat, proj);
    }

    for (const auto& entity : entityList) {
        auto* data = scene.GetEntityData(entity.GetID());
        if (!data || !data->Terrain || !data->Active) {
            continue;
        }
        for (TerrainInstancerLayerDesc& layer : data->Terrain->InstancerLayers) {
            if (!layer.Enabled) {
                continue;
            }
            renderer.Render(viewId, layer.Instancer, viewMat, proj, cameraPos, stateFlags);
            renderer.RenderDebug(viewId, layer.Instancer, viewMat, proj);
        }
    }
    
    // IMPORTANT: Reset u_PBRScalar1 to default after instancer rendering
    // In bgfx, uniforms persist across frames until explicitly overwritten.
    // Without this reset, the alpha cutout threshold set by the instancer could
    // leak into the next frame and affect other render passes that use fs_pbr.
    {
        static bgfx::UniformHandle u_PBRScalar1 = bgfx::createUniform("u_PBRScalar1", bgfx::UniformType::Vec4);
        glm::vec4 defaultScalar1(0.0f, 0.0f, 0.0f, 0.0f);  // y=0 disables alpha cutout
        bgfx::setUniform(u_PBRScalar1, &defaultScalar1);
    }
}

bool InstancerSystem::HasPointShadowCaster(
    Scene& scene,
    const glm::vec3& lightPos,
    float lightRange,
    const glm::vec3& cameraPos,
    float shadowDistance)
{
    auto& renderer = InstancerRenderer::Instance();
    const auto& entityList = scene.GetEntities();
    for (const auto& entity : entityList) {
        auto* data = scene.GetEntityData(entity.GetID());
        if (!data || !data->Instancer || !data->Active) {
            continue;
        }
        if (data->RenderOverrides && !data->RenderOverrides->CastShadows) {
            continue;
        }
        if (renderer.HasPointShadowCasterInRange(
                *data->Instancer,
                lightPos,
                lightRange,
                cameraPos,
                shadowDistance)) {
            return true;
        }
    }

    for (const auto& entity : entityList) {
        auto* data = scene.GetEntityData(entity.GetID());
        if (!data || !data->Terrain || !data->Active) {
            continue;
        }
        if (data->RenderOverrides && !data->RenderOverrides->CastShadows) {
            continue;
        }
        for (TerrainInstancerLayerDesc& layer : data->Terrain->InstancerLayers) {
            if (!layer.Enabled) {
                continue;
            }
            if (renderer.HasPointShadowCasterInRange(
                    layer.Instancer,
                    lightPos,
                    lightRange,
                    cameraPos,
                    shadowDistance)) {
                return true;
            }
        }
    }

    return false;
}

void InstancerSystem::RenderShadows(
    Scene& scene,
    const ShadowRenderParams& params,
    ShadowRenderStats* outStats)
{
    ShadowRenderStats localStats{};
    auto& renderer = InstancerRenderer::Instance();
    const auto& entityList = scene.GetEntities();

    for (const auto& entity : entityList) {
        auto* data = scene.GetEntityData(entity.GetID());
        if (!data || !data->Instancer || !data->Active) {
            continue;
        }
        if (data->RenderOverrides && !data->RenderOverrides->CastShadows) {
            continue;
        }

        InstancerComponent& comp = *data->Instancer;
        float shadowCandidateDistance = params.ShadowDistance;
        for (int pointLightIndex = 0; pointLightIndex < params.PointLightCount && pointLightIndex < kPointShadowLightCount; ++pointLightIndex) {
            shadowCandidateDistance = glm::max(
                shadowCandidateDistance,
                params.ShadowDistance + params.PointLights[pointLightIndex].Range);
        }
        if (!renderer.PrepareShadowCandidates(
                comp,
                params.CameraPosition,
                shadowCandidateDistance,
                params.CameraNear,
                params.CascadeSplits,
                params.CascadeCount)) {
            continue;
        }

        renderer.RenderDirectionalShadows(comp, params, &localStats);
        for (int pointLightIndex = 0; pointLightIndex < params.PointLightCount && pointLightIndex < kPointShadowLightCount; ++pointLightIndex) {
            localStats.PointSubmits += renderer.RenderPointShadows(
                comp,
                params.PointLights[pointLightIndex],
                params.PointStateFlags);
        }
    }

    for (const auto& entity : entityList) {
        auto* data = scene.GetEntityData(entity.GetID());
        if (!data || !data->Terrain || !data->Active) {
            continue;
        }
        if (data->RenderOverrides && !data->RenderOverrides->CastShadows) {
            continue;
        }

        for (TerrainInstancerLayerDesc& layer : data->Terrain->InstancerLayers) {
            if (!layer.Enabled) {
                continue;
            }

            InstancerComponent& comp = layer.Instancer;
            float shadowCandidateDistance = params.ShadowDistance;
            for (int pointLightIndex = 0; pointLightIndex < params.PointLightCount && pointLightIndex < kPointShadowLightCount; ++pointLightIndex) {
                shadowCandidateDistance = glm::max(
                    shadowCandidateDistance,
                    params.ShadowDistance + params.PointLights[pointLightIndex].Range);
            }
            if (!renderer.PrepareShadowCandidates(
                    comp,
                    params.CameraPosition,
                    shadowCandidateDistance,
                    params.CameraNear,
                    params.CascadeSplits,
                    params.CascadeCount)) {
                continue;
            }

            renderer.RenderDirectionalShadows(comp, params, &localStats);
            for (int pointLightIndex = 0; pointLightIndex < params.PointLightCount && pointLightIndex < kPointShadowLightCount; ++pointLightIndex) {
                localStats.PointSubmits += renderer.RenderPointShadows(
                    comp,
                    params.PointLights[pointLightIndex],
                    params.PointStateFlags);
            }
        }
    }

    if (outStats) {
        *outStats = localStats;
    }
}

void InstancerSystem::RegenerateInstancer(EntityID entityId, Scene& scene) {
    auto* data = scene.GetEntityData(entityId);
    if (!data || !data->Instancer) return;
    
    data->Instancer->NeedsRegeneration = true;
}

} // namespace instancer
} // namespace cm

