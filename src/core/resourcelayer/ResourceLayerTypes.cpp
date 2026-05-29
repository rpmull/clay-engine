#include "ResourceLayerTypes.h"
#include "ImposterManager.h"  // For ImposterCache destructor
#include "core/ecs/Components.h"
#include "core/ecs/Scene.h"
#include "core/ecs/EntityData.h"
#include "core/rendering/Terrain.h"
#ifndef CLAYMORE_RUNTIME
#include "core/prefab/PrefabAPI.h"
#endif

#include <algorithm>
#include <iostream>
#include <random>
#include <cmath>
#include <cstring>
#include <chrono>

namespace cm {
namespace resourcelayer {

//------------------------------------------------------------------------------
// ProceduralResourceLayer Implementation
//------------------------------------------------------------------------------

ProceduralResourceLayer::ProceduralResourceLayer() = default;
ProceduralResourceLayer::~ProceduralResourceLayer() = default;
ProceduralResourceLayer::ProceduralResourceLayer(ProceduralResourceLayer&& other) noexcept = default;
ProceduralResourceLayer& ProceduralResourceLayer::operator=(ProceduralResourceLayer&& other) noexcept = default;

ProceduralResourceLayer::ProceduralResourceLayer(const ProceduralResourceLayer& other)
    : Guid(ClaymoreGUID::Generate())  // New GUID for copy
    , Name(other.Name)
    , Enabled(other.Enabled)
    , PrefabAsset(other.PrefabAsset)
    , PrefabPath(other.PrefabPath)
    , DensityPerSquareMeter(other.DensityPerSquareMeter)
    , MinSpacing(other.MinSpacing)
    , MinScale(other.MinScale)
    , MaxScale(other.MaxScale)
    , NonUniformScale(other.NonUniformScale)
    , MinScaleVec(other.MinScaleVec)
    , MaxScaleVec(other.MaxScaleVec)
    , YawVarianceDegrees(other.YawVarianceDegrees)
    , PitchVarianceDegrees(other.PitchVarianceDegrees)
    , RollVarianceDegrees(other.RollVarianceDegrees)
    , AlignToSlope(other.AlignToSlope)
    , SlopeAlignmentFactor(other.SlopeAlignmentFactor)
    , HeightOffset(other.HeightOffset)
    , HeightOffsetVariance(other.HeightOffsetVariance)
    , EnableClustering(other.EnableClustering)
    , ClusterRadius(other.ClusterRadius)
    , ClusterMinCount(other.ClusterMinCount)
    , ClusterMaxCount(other.ClusterMaxCount)
    , ClusterFalloff(other.ClusterFalloff)
    , ClusterSpacing(other.ClusterSpacing)
    , UseImposter(other.UseImposter)
    , ImposterDistance(other.ImposterDistance)
    , CullDistance(other.CullDistance)
    , CrossfadeRange(other.CrossfadeRange)
    , PreviewColor(other.PreviewColor)
    , ShowPreviewMarkers(other.ShowPreviewMarkers)
{
    // Copy eligibility filters via serialization
    nlohmann::json ej;
    other.Eligibility.Serialize(ej);
    Eligibility.Deserialize(ej);
    
    // Don't copy cache or imposter
    Cache.Valid = false;
}

ProceduralResourceLayer& ProceduralResourceLayer::operator=(const ProceduralResourceLayer& other) {
    if (this != &other) {
        Guid = ClaymoreGUID::Generate();
        Name = other.Name;
        Enabled = other.Enabled;
        PrefabAsset = other.PrefabAsset;
        PrefabPath = other.PrefabPath;
        DensityPerSquareMeter = other.DensityPerSquareMeter;
        MinSpacing = other.MinSpacing;
        MinScale = other.MinScale;
        MaxScale = other.MaxScale;
        NonUniformScale = other.NonUniformScale;
        MinScaleVec = other.MinScaleVec;
        MaxScaleVec = other.MaxScaleVec;
        YawVarianceDegrees = other.YawVarianceDegrees;
        PitchVarianceDegrees = other.PitchVarianceDegrees;
        RollVarianceDegrees = other.RollVarianceDegrees;
        AlignToSlope = other.AlignToSlope;
        SlopeAlignmentFactor = other.SlopeAlignmentFactor;
        HeightOffset = other.HeightOffset;
        HeightOffsetVariance = other.HeightOffsetVariance;
        EnableClustering = other.EnableClustering;
        ClusterRadius = other.ClusterRadius;
        ClusterMinCount = other.ClusterMinCount;
        ClusterMaxCount = other.ClusterMaxCount;
        ClusterFalloff = other.ClusterFalloff;
        ClusterSpacing = other.ClusterSpacing;
        UseImposter = other.UseImposter;
        ImposterDistance = other.ImposterDistance;
        CullDistance = other.CullDistance;
        CrossfadeRange = other.CrossfadeRange;
        PreviewColor = other.PreviewColor;
        ShowPreviewMarkers = other.ShowPreviewMarkers;
        
        nlohmann::json ej;
        other.Eligibility.Serialize(ej);
        Eligibility.Deserialize(ej);
        
        Cache.Valid = false;
        BakedImposter.reset();
    }
    return *this;
}

uint64_t ProceduralResourceLayer::ComputeSettingsHash() const {
    // Simple hash combining key settings
    uint64_t hash = 0;
    auto hashFloat = [&hash](float f) {
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof(bits));
        hash ^= bits;
        hash = hash * 31 + bits;
    };
    
    hashFloat(DensityPerSquareMeter);
    hashFloat(MinSpacing);
    hashFloat(MinScale);
    hashFloat(MaxScale);
    hash ^= Enabled ? 1 : 0;
    hash ^= EnableClustering ? 2 : 0;
    hash ^= Eligibility.Filters.size() * 1000;
    
    return hash;
}

bool ProceduralResourceLayer::NeedsRegeneration() const {
    return !Cache.Valid || Cache.GenerationHash != ComputeSettingsHash();
}

//------------------------------------------------------------------------------
// ResourceLayerComponent Implementation
//------------------------------------------------------------------------------

void ResourceLayerComponent::Regenerate(const TerrainComponent& terrain, const glm::mat4& terrainTransform) {
    auto startTime = std::chrono::high_resolution_clock::now();
    
    Runtime.Clear();
    
    EligibilityContext ctx = BuildContext(terrain);
    ResourceDistributor distributor;
    
    // Extract terrain world position from transform
    glm::vec3 terrainPos = glm::vec3(terrainTransform[3]);
    
    // Determine terrain bounds in world space
    // WorldSize is (width, depth) in terrain local space
    glm::vec2 areaMin(terrainPos.x, terrainPos.z);
    glm::vec2 areaMax(terrainPos.x + terrain.WorldSize.x, terrainPos.z + terrain.WorldSize.y);
    
    ResourceDistributor::DistributionParams params;
    params.Seed = GlobalSeed;
    params.DensityMultiplier = GlobalDensityMultiplier;
    params.Terrain = &terrain;
    params.TerrainTransform = terrainTransform;
    params.Context = &ctx;
    params.AreaMin = areaMin;
    params.AreaMax = areaMax;
    
    for (uint32_t i = 0; i < Layers.size(); ++i) {
        auto& layer = Layers[i];
        if (!layer.Enabled) continue;
        
        std::vector<ResourceInstance> layerInstances;
        
        if (layer.EnableClustering) {
            distributor.GenerateClusteredInstances(layer, params, layerInstances);
        } else {
            distributor.GenerateInstances(layer, params, layerInstances);
        }
        
        // Set layer index on instances
        for (auto& inst : layerInstances) {
            inst.LayerIndex = i;
        }
        
        Runtime.Instances.insert(Runtime.Instances.end(), 
            layerInstances.begin(), layerInstances.end());
        
        layer.Cache.Instances = std::move(layerInstances);
        layer.Cache.Valid = true;
        layer.Cache.GenerationHash = layer.ComputeSettingsHash();
    }
    
    // Update Stats for UI
    Stats.TotalInstances = static_cast<uint32_t>(Runtime.Instances.size());
    
    // Apply persistent state (destroyed/modified instances)
    for (auto it = Runtime.Instances.begin(); it != Runtime.Instances.end(); ) {
        if (Runtime.DestroyedInstances.count(it->InstanceID) > 0) {
            it = Runtime.Instances.erase(it);
        } else {
            auto modIt = Runtime.ModifiedInstances.find(it->InstanceID);
            if (modIt != Runtime.ModifiedInstances.end()) {
                it->State = modIt->second;
            }
            ++it;
        }
    }
    
    Runtime.UpdateSpatialHash();
    Runtime.TotalInstances = static_cast<uint32_t>(Runtime.Instances.size());
    
    NeedsRegeneration = false;
    NeedsFullRegeneration = false;
    
    auto endTime = std::chrono::high_resolution_clock::now();
    Stats.GenerationTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();
}

void ResourceLayerComponent::RegenerateLayer(uint32_t layerIndex, const TerrainComponent& terrain, const glm::mat4& terrainTransform) {
    if (layerIndex >= Layers.size()) return;
    
    auto& layer = Layers[layerIndex];
    
    // Remove existing instances for this layer
    Runtime.Instances.erase(
        std::remove_if(Runtime.Instances.begin(), Runtime.Instances.end(),
            [layerIndex](const ResourceInstance& inst) {
                return inst.LayerIndex == layerIndex;
            }),
        Runtime.Instances.end()
    );
    
    if (!layer.Enabled) {
        layer.Cache.Valid = false;
        Runtime.UpdateSpatialHash();
        return;
    }
    
    EligibilityContext ctx = BuildContext(terrain);
    ResourceDistributor distributor;
    
    // Extract terrain world position from transform
    glm::vec3 terrainPos = glm::vec3(terrainTransform[3]);
    
    // Determine terrain bounds in world space
    glm::vec2 areaMin(terrainPos.x, terrainPos.z);
    glm::vec2 areaMax(terrainPos.x + terrain.WorldSize.x, terrainPos.z + terrain.WorldSize.y);
    
    ResourceDistributor::DistributionParams params;
    params.Seed = GlobalSeed ^ layerIndex;
    params.DensityMultiplier = GlobalDensityMultiplier;
    params.Terrain = &terrain;
    params.TerrainTransform = terrainTransform;
    params.Context = &ctx;
    params.AreaMin = areaMin;
    params.AreaMax = areaMax;
    
    std::vector<ResourceInstance> layerInstances;
    
    if (layer.EnableClustering) {
        distributor.GenerateClusteredInstances(layer, params, layerInstances);
    } else {
        distributor.GenerateInstances(layer, params, layerInstances);
    }
    
    for (auto& inst : layerInstances) {
        inst.LayerIndex = layerIndex;
    }
    
    Runtime.Instances.insert(Runtime.Instances.end(), 
        layerInstances.begin(), layerInstances.end());
    
    layer.Cache.Instances = layerInstances;
    layer.Cache.Valid = true;
    layer.Cache.GenerationHash = layer.ComputeSettingsHash();
    
    Runtime.UpdateSpatialHash();
    Runtime.TotalInstances = static_cast<uint32_t>(Runtime.Instances.size());
    Stats.TotalInstances = Runtime.TotalInstances;
}

void ResourceLayerComponent::MarkInstanceDestroyed(uint32_t instanceId) {
    if (std::find(Persistent.DestroyedIDs.begin(), Persistent.DestroyedIDs.end(), instanceId)
        == Persistent.DestroyedIDs.end()) {
        Persistent.DestroyedIDs.push_back(instanceId);
    }
    Runtime.DestroyedInstances.insert(instanceId);
    
    for (auto& inst : Runtime.Instances) {
        if (inst.InstanceID == instanceId) {
            inst.State = ResourceState::Destroyed;
            break;
        }
    }
}

void ResourceLayerComponent::MarkInstanceModified(uint32_t instanceId) {
    Persistent.StateOverrides[instanceId] = ResourceState::Modified;
    Runtime.ModifiedInstances[instanceId] = ResourceState::Modified;
    
    for (auto& inst : Runtime.Instances) {
        if (inst.InstanceID == instanceId) {
            inst.State = ResourceState::Modified;
            break;
        }
    }
}

void ResourceLayerComponent::UpdateVisibility(const glm::vec3& cameraPos, const glm::mat4& viewProj) {
    uint32_t visible = 0;
    uint32_t culled = 0;
    
    for (auto& inst : Runtime.Instances) {
        inst.DistanceToCamera = glm::length(cameraPos - inst.Position);
        
        // Get cull distance from layer
        float cullDist = 500.0f;
        if (inst.LayerIndex < Layers.size()) {
            cullDist = Layers[inst.LayerIndex].CullDistance;
        }
        
        // Distance culling
        if (inst.DistanceToCamera > cullDist) {
            inst.Visible = false;
            ++culled;
            continue;
        }
        
        // Simple frustum culling
        glm::vec4 clip = viewProj * glm::vec4(inst.Position, 1.0f);
        if (clip.w <= 0.0f) {
            inst.Visible = false;
            ++culled;
            continue;
        }
        
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        float margin = inst.Scale * 2.0f / clip.w;
        
        inst.Visible = ndc.x >= -1.0f - margin && ndc.x <= 1.0f + margin &&
                       ndc.y >= -1.0f - margin && ndc.y <= 1.0f + margin &&
                       ndc.z >= 0.0f && ndc.z <= 1.0f;
        
        if (inst.Visible) {
            ++visible;
        } else {
            ++culled;
        }
    }
    
    Stats.VisibleImposters = visible;
    Stats.CulledInstances = culled;
}

EligibilityContext ResourceLayerComponent::BuildContext(const TerrainComponent& terrain) const {
    EligibilityContext ctx;
    ctx.Terrain = &terrain;
    ctx.TerrainTransform = glm::mat4(1.0f);
    ctx.Climate = UseClimateGradients ? &Climate : nullptr;
    ctx.Roads = Roads.empty() ? nullptr : &Roads;
    ctx.Regions = Regions.empty() ? nullptr : &Regions;
    return ctx;
}

ProceduralResourceLayer* ResourceLayerComponent::GetLayer(const ClaymoreGUID& guid) {
    for (auto& layer : Layers) {
        if (layer.Guid == guid) return &layer;
    }
    return nullptr;
}

const ProceduralResourceLayer* ResourceLayerComponent::GetLayer(const ClaymoreGUID& guid) const {
    for (const auto& layer : Layers) {
        if (layer.Guid == guid) return &layer;
    }
    return nullptr;
}

void ResourceLayerComponent::AddLayer(const ProceduralResourceLayer& layer) {
    Layers.push_back(layer);
    NeedsRegeneration = true;
}

void ResourceLayerComponent::RemoveLayer(const ClaymoreGUID& guid) {
    Layers.erase(
        std::remove_if(Layers.begin(), Layers.end(),
            [&guid](const ProceduralResourceLayer& l) { return l.Guid == guid; }),
        Layers.end()
    );
    NeedsFullRegeneration = true;
}

void ResourceLayerComponent::RemoveLayerAt(size_t index) {
    if (index < Layers.size()) {
        Layers.erase(Layers.begin() + index);
        NeedsFullRegeneration = true;
    }
}

void ResourceLayerComponent::AddRegion(const RegionPolygon& region) {
    Regions.push_back(region);
}

void ResourceLayerComponent::RemoveRegion(const ClaymoreGUID& guid) {
    Regions.erase(
        std::remove_if(Regions.begin(), Regions.end(),
            [&guid](const RegionPolygon& r) { return r.Guid == guid; }),
        Regions.end()
    );
}

void ResourceLayerComponent::AddRoad(const std::vector<glm::vec3>& roadPoints) {
    Roads.push_back(roadPoints);
}

void ResourceLayerComponent::RemoveRoad(size_t index) {
    if (index < Roads.size()) {
        Roads.erase(Roads.begin() + index);
    }
}

void ResourceLayerComponent::ClearRoads() {
    Roads.clear();
}

//------------------------------------------------------------------------------
// Interaction Queries Implementation
//------------------------------------------------------------------------------

std::vector<ResourceLayerComponent::NearbyResource> 
ResourceLayerComponent::QueryInteractableNearby(const glm::vec3& position, float radius) const {
    std::vector<NearbyResource> results;
    
    for (uint32_t i = 0; i < Runtime.Instances.size(); ++i) {
        const auto& inst = Runtime.Instances[i];
        
        // Skip destroyed instances
        if (inst.State == ResourceState::Destroyed) continue;
        
        // Check if layer is interactable
        if (inst.LayerIndex >= Layers.size()) continue;
        const auto& layer = Layers[inst.LayerIndex];
        if (!layer.Interactable) continue;
        
        // Distance check
        float dist = glm::length(position - inst.Position);
        if (dist > radius) continue;
        
        NearbyResource res;
        res.InstanceIndex = i;
        res.LayerIndex = inst.LayerIndex;
        res.Position = inst.Position;
        res.Distance = dist;
        res.InteractionTag = layer.InteractionTag;
        res.IsActive = (inst.State == ResourceState::Active);
        results.push_back(res);
    }
    
    // Sort by distance
    std::sort(results.begin(), results.end(), 
        [](const NearbyResource& a, const NearbyResource& b) {
            return a.Distance < b.Distance;
        });
    
    return results;
}

bool ResourceLayerComponent::GetClosestInteractable(
    const glm::vec3& position, 
    float maxRadius, 
    NearbyResource& outResult) const 
{
    float closestDist = maxRadius;
    bool found = false;
    
    for (uint32_t i = 0; i < Runtime.Instances.size(); ++i) {
        const auto& inst = Runtime.Instances[i];
        
        // Skip destroyed instances
        if (inst.State == ResourceState::Destroyed) continue;
        
        // Check if layer is interactable
        if (inst.LayerIndex >= Layers.size()) continue;
        const auto& layer = Layers[inst.LayerIndex];
        if (!layer.Interactable) continue;
        
        // Distance check
        float dist = glm::length(position - inst.Position);
        if (dist < closestDist) {
            closestDist = dist;
            outResult.InstanceIndex = i;
            outResult.LayerIndex = inst.LayerIndex;
            outResult.Position = inst.Position;
            outResult.Distance = dist;
            outResult.InteractionTag = layer.InteractionTag;
            outResult.IsActive = (inst.State == ResourceState::Active);
            found = true;
        }
    }
    
    return found;
}

EntityID ResourceLayerComponent::SpawnForInteraction(uint32_t instanceIndex, Scene& scene) {
    if (instanceIndex >= Runtime.Instances.size()) {
        return INVALID_ENTITY_ID;
    }
    
    auto& inst = Runtime.Instances[instanceIndex];
    
    // Already active? Return existing entity
    if (inst.State == ResourceState::Active && inst.ActiveEntity != INVALID_ENTITY_ID) {
        return inst.ActiveEntity;
    }
    
    if (inst.LayerIndex >= Layers.size()) {
        return INVALID_ENTITY_ID;
    }
    
    const auto& layer = Layers[inst.LayerIndex];
    
    // Instantiate prefab
#ifndef CLAYMORE_RUNTIME
    EntityID prefabRoot = INVALID_ENTITY_ID;
    if (!layer.PrefabPath.empty()) {
        prefabRoot = ::InstantiatePrefabFromPath(layer.PrefabPath, scene);
    }
    if (prefabRoot == INVALID_ENTITY_ID && 
        !(layer.PrefabAsset.guid.high == 0 && layer.PrefabAsset.guid.low == 0)) {
        prefabRoot = ::InstantiatePrefab(layer.PrefabAsset.guid, scene);
    }
#else
    EntityID prefabRoot = INVALID_ENTITY_ID;
#endif
    
    if (prefabRoot == INVALID_ENTITY_ID) {
        return INVALID_ENTITY_ID;
    }
    
    // Position the prefab
    if (EntityData* data = scene.GetEntityData(prefabRoot)) {
        data->Transform.Position = inst.Position;
        data->Transform.Rotation = glm::eulerAngles(inst.Rotation);
        data->Transform.Scale = glm::vec3(inst.Scale);
        scene.MarkTransformDirty(prefabRoot);
    }
    
    // Mark as active (physics is preserved for interactable layers by default)
    inst.State = ResourceState::Active;
    inst.ActiveEntity = prefabRoot;
    Runtime.ActivePrefabIndices.insert(instanceIndex);
    
    std::cout << "[ResourceLayer] Spawned interactable resource at (" 
              << inst.Position.x << ", " << inst.Position.y << ", " << inst.Position.z 
              << ") for interaction" << std::endl;
    
    return prefabRoot;
}

//------------------------------------------------------------------------------
// ResourceDistributor Implementation
//------------------------------------------------------------------------------

void ResourceDistributor::GenerateInstances(
    const ProceduralResourceLayer& layer,
    const DistributionParams& params,
    std::vector<ResourceInstance>& outInstances)
{
    outInstances.clear();
    
    if (layer.DensityPerSquareMeter <= 0.0f) return;
    
    std::mt19937 rng(params.Seed);
    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
    
    // Generate candidate positions using Poisson disk sampling
    std::vector<glm::vec2> positions;
    float effectiveDensity = layer.DensityPerSquareMeter * params.DensityMultiplier;
    
    PoissonDiskSample(params.AreaMin, params.AreaMax, layer.MinSpacing, 
                      effectiveDensity, params.Seed, positions);
    
    // Evaluate eligibility and create instances
    for (const glm::vec2& pos : positions) {
        // Sample terrain
        float height = 0.0f;
        float slopeDeg = 0.0f;
        glm::vec3 normal(0, 1, 0);
        
        if (params.Terrain) {
            if (!SampleTerrainHeight(params.Terrain, params.TerrainTransform, pos, height)) {
                continue;
            }
            if (!SampleTerrainSlope(params.Terrain, params.TerrainTransform, pos, slopeDeg, normal)) {
                continue;
            }
        }
        
        glm::vec3 worldPos(pos.x, height, pos.y);
        
        // Evaluate eligibility via context
        if (params.Context) {
            params.Context->InvalidateCache();
            float eligibility = layer.Eligibility.Evaluate(worldPos, *params.Context);
            
            // Rejection sampling
            if (dist01(rng) > eligibility) {
                continue;
            }
        }
        
        // Create instance
        ResourceInstance inst;
        inst.BiomeIndex = 0; // Not using biomes
        inst.LayerIndex = 0; // Will be set by caller
        inst.Position = worldPos;
        inst.Position.y += layer.HeightOffset + (dist01(rng) - 0.5f) * 2.0f * layer.HeightOffsetVariance;
        
        // Apply random variation
        ApplyRandomVariation(inst, layer, normal, rng);
        
        // Generate stable ID
        inst.InstanceID = GenerateInstanceID(inst.Position, params.Seed);
        inst.State = ResourceState::Pristine;
        
        outInstances.push_back(inst);
    }
}

void ResourceDistributor::GenerateClusteredInstances(
    const ProceduralResourceLayer& layer,
    const DistributionParams& params,
    std::vector<ResourceInstance>& outInstances)
{
    outInstances.clear();
    
    std::mt19937 rng(params.Seed);
    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
    std::uniform_int_distribution<int> countDist(layer.ClusterMinCount, layer.ClusterMaxCount);
    
    // Generate cluster centers using Poisson disk
    std::vector<glm::vec2> clusterCenters;
    float clusterDensity = layer.DensityPerSquareMeter * params.DensityMultiplier / 
                          (layer.ClusterMinCount + layer.ClusterMaxCount) * 2.0f;
    
    PoissonDiskSample(params.AreaMin, params.AreaMax, layer.ClusterSpacing,
                      clusterDensity, params.Seed, clusterCenters);
    
    // Generate instances around each cluster center
    for (const glm::vec2& center : clusterCenters) {
        int count = countDist(rng);
        
        for (int i = 0; i < count; ++i) {
            // Random position within cluster radius with falloff
            float angle = dist01(rng) * glm::two_pi<float>();
            float radius = std::sqrt(dist01(rng)) * layer.ClusterRadius; // sqrt for uniform distribution
            
            glm::vec2 pos = center + glm::vec2(std::cos(angle), std::sin(angle)) * radius;
            
            // Check bounds
            if (pos.x < params.AreaMin.x || pos.x > params.AreaMax.x ||
                pos.y < params.AreaMin.y || pos.y > params.AreaMax.y) {
                continue;
            }
            
            // Sample terrain
            float height = 0.0f;
            float slopeDeg = 0.0f;
            glm::vec3 normal(0, 1, 0);
            
            if (params.Terrain) {
                if (!SampleTerrainHeight(params.Terrain, params.TerrainTransform, pos, height)) {
                    continue;
                }
                if (!SampleTerrainSlope(params.Terrain, params.TerrainTransform, pos, slopeDeg, normal)) {
                    continue;
                }
            }
            
            glm::vec3 worldPos(pos.x, height, pos.y);
            
            // Evaluate eligibility with cluster falloff
            float clusterFactor = 1.0f - (radius / layer.ClusterRadius) * layer.ClusterFalloff;
            
            if (params.Context) {
                params.Context->InvalidateCache();
                float eligibility = layer.Eligibility.Evaluate(worldPos, *params.Context);
                eligibility *= clusterFactor;
                
                if (dist01(rng) > eligibility) {
                    continue;
                }
            }
            
            ResourceInstance inst;
            inst.BiomeIndex = 0;
            inst.LayerIndex = 0;
            inst.Position = worldPos;
            inst.Position.y += layer.HeightOffset;
            
            ApplyRandomVariation(inst, layer, normal, rng);
            inst.InstanceID = GenerateInstanceID(inst.Position, params.Seed);
            inst.State = ResourceState::Pristine;
            
            outInstances.push_back(inst);
        }
    }
}

void ResourceDistributor::PoissonDiskSample(
    const glm::vec2& areaMin,
    const glm::vec2& areaMax,
    float minDistance,
    float density,
    uint32_t seed,
    std::vector<glm::vec2>& outPositions)
{
    outPositions.clear();
    
    const float cellSize = minDistance / std::sqrt(2.0f);
    const glm::vec2 boundsSize = areaMax - areaMin;
    
    if (boundsSize.x <= 0 || boundsSize.y <= 0) return;
    
    const int gridW = static_cast<int>(std::ceil(boundsSize.x / cellSize));
    const int gridH = static_cast<int>(std::ceil(boundsSize.y / cellSize));
    
    std::vector<int> grid(gridW * gridH, -1);
    
    auto gridIndex = [&](const glm::vec2& p) -> int {
        int gx = static_cast<int>((p.x - areaMin.x) / cellSize);
        int gy = static_cast<int>((p.y - areaMin.y) / cellSize);
        gx = std::clamp(gx, 0, gridW - 1);
        gy = std::clamp(gy, 0, gridH - 1);
        return gy * gridW + gx;
    };
    
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> distAngle(0.0f, glm::two_pi<float>());
    std::uniform_real_distribution<float> distRadius(minDistance, minDistance * 2.0f);
    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
    
    // First point
    glm::vec2 firstPoint;
    firstPoint.x = areaMin.x + dist01(rng) * boundsSize.x;
    firstPoint.y = areaMin.y + dist01(rng) * boundsSize.y;
    
    outPositions.push_back(firstPoint);
    grid[gridIndex(firstPoint)] = 0;
    
    std::vector<int> activeList;
    activeList.push_back(0);
    
    // Target based on density and area
    const float area = boundsSize.x * boundsSize.y;
    const int targetCount = std::max(1, static_cast<int>(density * area));
    const int maxAttempts = 30;
    
    while (!activeList.empty() && static_cast<int>(outPositions.size()) < targetCount) {
        int activeIdx = static_cast<int>(dist01(rng) * activeList.size());
        glm::vec2 center = outPositions[activeList[activeIdx]];
        
        bool found = false;
        for (int k = 0; k < maxAttempts; ++k) {
            float angle = distAngle(rng);
            float radius = distRadius(rng);
            glm::vec2 candidate = center + glm::vec2(std::cos(angle), std::sin(angle)) * radius;
            
            if (candidate.x < areaMin.x || candidate.x > areaMax.x ||
                candidate.y < areaMin.y || candidate.y > areaMax.y) {
                continue;
            }
            
            int cx = static_cast<int>((candidate.x - areaMin.x) / cellSize);
            int cy = static_cast<int>((candidate.y - areaMin.y) / cellSize);
            
            bool tooClose = false;
            for (int dy = -2; dy <= 2 && !tooClose; ++dy) {
                for (int dx = -2; dx <= 2 && !tooClose; ++dx) {
                    int nx = cx + dx;
                    int ny = cy + dy;
                    if (nx >= 0 && nx < gridW && ny >= 0 && ny < gridH) {
                        int idx = grid[ny * gridW + nx];
                        if (idx >= 0) {
                            float distSq = glm::distance2(candidate, outPositions[idx]);
                            if (distSq < minDistance * minDistance) {
                                tooClose = true;
                            }
                        }
                    }
                }
            }
            
            if (!tooClose) {
                int newIdx = static_cast<int>(outPositions.size());
                outPositions.push_back(candidate);
                grid[gridIndex(candidate)] = newIdx;
                activeList.push_back(newIdx);
                found = true;
                break;
            }
        }
        
        if (!found) {
            activeList[activeIdx] = activeList.back();
            activeList.pop_back();
        }
    }
}

bool ResourceDistributor::SampleTerrainHeight(
    const TerrainComponent* terrain,
    const glm::mat4& transform,
    const glm::vec2& xz,
    float& outHeight)
{
    if (!terrain) return false;
    
    glm::mat4 invTransform = glm::inverse(transform);
    glm::vec4 localPos = invTransform * glm::vec4(xz.x, 0.0f, xz.y, 1.0f);
    if (localPos.x < 0.0f || localPos.x > terrain->WorldSize.x ||
        localPos.z < 0.0f || localPos.z > terrain->WorldSize.y) {
        return false;
    }
    if (Terrain::IsHoleAtLocal(*terrain, localPos.x, localPos.z)) {
        return false;
    }
    
    outHeight = Terrain::SampleHeightWorld(*terrain, localPos.x, localPos.z);
    
    glm::vec4 worldPos = transform * glm::vec4(localPos.x, outHeight, localPos.z, 1.0f);
    outHeight = worldPos.y;
    
    return true;
}

bool ResourceDistributor::SampleTerrainSlope(
    const TerrainComponent* terrain,
    const glm::mat4& transform,
    const glm::vec2& xz,
    float& outSlopeDegrees,
    glm::vec3& outNormal)
{
    if (!terrain) return false;
    
    glm::mat4 invTransform = glm::inverse(transform);
    glm::vec4 localPos = invTransform * glm::vec4(xz.x, 0.0f, xz.y, 1.0f);
    if (localPos.x < 0.0f || localPos.x > terrain->WorldSize.x ||
        localPos.z < 0.0f || localPos.z > terrain->WorldSize.y) {
        return false;
    }
    if (Terrain::IsHoleAtLocal(*terrain, localPos.x, localPos.z)) {
        return false;
    }
    
    outNormal = Terrain::SampleNormal(*terrain, localPos.x, localPos.z);
    outSlopeDegrees = glm::degrees(std::acos(glm::clamp(outNormal.y, 0.0f, 1.0f)));
    
    return true;
}

uint32_t ResourceDistributor::GenerateInstanceID(const glm::vec3& position, uint32_t seed) {
    uint32_t hash = seed ^ 2166136261u;
    
    auto hashFloat = [&hash](float f) {
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof(bits));
        hash ^= bits;
        hash *= 16777619u;
    };
    
    hashFloat(position.x);
    hashFloat(position.y);
    hashFloat(position.z);
    
    return hash;
}

void ResourceDistributor::ApplyRandomVariation(
    ResourceInstance& inst,
    const ProceduralResourceLayer& layer,
    const glm::vec3& terrainNormal,
    std::mt19937& rng)
{
    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
    
    // Scale
    if (layer.NonUniformScale) {
        glm::vec3 scale;
        scale.x = layer.MinScaleVec.x + dist01(rng) * (layer.MaxScaleVec.x - layer.MinScaleVec.x);
        scale.y = layer.MinScaleVec.y + dist01(rng) * (layer.MaxScaleVec.y - layer.MinScaleVec.y);
        scale.z = layer.MinScaleVec.z + dist01(rng) * (layer.MaxScaleVec.z - layer.MinScaleVec.z);
        inst.Scale = (scale.x + scale.y + scale.z) / 3.0f; // Average for uniform storage
    } else {
        inst.Scale = layer.MinScale + dist01(rng) * (layer.MaxScale - layer.MinScale);
    }
    
    // Rotation
    float yaw = (dist01(rng) - 0.5f) * 2.0f * layer.YawVarianceDegrees;
    float pitch = (dist01(rng) - 0.5f) * 2.0f * layer.PitchVarianceDegrees;
    float roll = (dist01(rng) - 0.5f) * 2.0f * layer.RollVarianceDegrees;
    
    glm::quat rotation = glm::quat(glm::vec3(glm::radians(pitch), glm::radians(yaw), glm::radians(roll)));
    
    // Slope alignment
    if (layer.AlignToSlope && terrainNormal.y < 0.999f) {
        glm::vec3 up(0, 1, 0);
        glm::vec3 axis = glm::cross(up, terrainNormal);
        float angle = std::acos(glm::clamp(glm::dot(up, terrainNormal), -1.0f, 1.0f));
        angle *= layer.SlopeAlignmentFactor;
        
        if (glm::length2(axis) > 0.0001f) {
            glm::quat slopeRot = glm::angleAxis(angle, glm::normalize(axis));
            rotation = slopeRot * rotation;
        }
    }
    
    inst.Rotation = rotation;
}

} // namespace resourcelayer
} // namespace cm
