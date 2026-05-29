#pragma once

#include "ClimateTypes.h"
#include "EligibilityFilter.h"
#include "core/assets/AssetReference.h"
#include "core/ecs/Entity.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <bgfx/bgfx.h>
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <random>

namespace cm {
namespace resourcelayer {

// Forward declaration (defined in ImposterManager.h)
struct ImposterCache;

//------------------------------------------------------------------------------
// ResourceState - Lifecycle state of a resource instance
//------------------------------------------------------------------------------
enum class ResourceState : uint8_t {
    Pristine = 0,   // Untouched - can swap freely between imposter and prefab
    Active,         // Currently a full prefab in scene
    Modified,       // User modified the prefab - keep as prefab, never swap back
    Destroyed       // User destroyed - won't respawn
};

//------------------------------------------------------------------------------
// ResourceInstance - A single placed resource
//------------------------------------------------------------------------------
struct ResourceInstance {
    uint32_t InstanceID = 0;       // Stable ID for persistence
    uint32_t BiomeIndex = 0;       // Reserved (was biome, now layer group)
    uint32_t LayerIndex = 0;       // Which layer spawned this
    
    glm::vec3 Position{0.0f};
    glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
    float Scale = 1.0f;
    
    ResourceState State = ResourceState::Pristine;
    
    // Runtime (not serialized)
    EntityID ActiveEntity = INVALID_ENTITY_ID;  // When swapped to prefab
    float DistanceToCamera = 0.0f;
    bool Visible = true;
};

//------------------------------------------------------------------------------
// PersistentState - Saved instance modifications
//------------------------------------------------------------------------------
struct PersistentState {
    std::vector<uint32_t> DestroyedIDs;
    std::unordered_map<uint32_t, ResourceState> StateOverrides;
};

//------------------------------------------------------------------------------
// RuntimeData - Runtime-only state for resource layer system
//------------------------------------------------------------------------------
struct RuntimeData {
    std::vector<ResourceInstance> Instances;
    
    // Spatial acceleration for proximity queries
    std::unordered_map<int64_t, std::vector<uint32_t>> SpatialHash;
    float CellSize = 50.0f;
    
    // Tracking sets
    std::unordered_set<uint32_t> DestroyedInstances;
    std::unordered_map<uint32_t, ResourceState> ModifiedInstances;
    std::unordered_set<uint32_t> ActivePrefabIndices;
    
    // Stats
    uint32_t TotalInstances = 0;
    uint32_t VisibleCount = 0;
    
    void Clear() {
        Instances.clear();
        SpatialHash.clear();
        DestroyedInstances.clear();
        ModifiedInstances.clear();
        ActivePrefabIndices.clear();
        TotalInstances = 0;
        VisibleCount = 0;
    }
    
    void UpdateSpatialHash() {
        SpatialHash.clear();
        for (uint32_t i = 0; i < Instances.size(); ++i) {
            const auto& inst = Instances[i];
            int64_t cellX = static_cast<int64_t>(std::floor(inst.Position.x / CellSize));
            int64_t cellZ = static_cast<int64_t>(std::floor(inst.Position.z / CellSize));
            int64_t key = (cellX << 32) | (cellZ & 0xFFFFFFFF);
            SpatialHash[key].push_back(i);
        }
    }
    
    std::vector<uint32_t> QueryNearby(const glm::vec3& pos, float radius) const {
        std::vector<uint32_t> result;
        int cellRadius = static_cast<int>(std::ceil(radius / CellSize));
        int64_t centerX = static_cast<int64_t>(std::floor(pos.x / CellSize));
        int64_t centerZ = static_cast<int64_t>(std::floor(pos.z / CellSize));
        
        for (int dx = -cellRadius; dx <= cellRadius; ++dx) {
            for (int dz = -cellRadius; dz <= cellRadius; ++dz) {
                int64_t key = ((centerX + dx) << 32) | ((centerZ + dz) & 0xFFFFFFFF);
                auto it = SpatialHash.find(key);
                if (it != SpatialHash.end()) {
                    for (uint32_t idx : it->second) {
                        float dist = glm::length(pos - Instances[idx].Position);
                        if (dist <= radius) {
                            result.push_back(idx);
                        }
                    }
                }
            }
        }
        return result;
    }
};

// Type aliases for compatibility
using BiomePersistentState = PersistentState;
using BiomeRuntimeData = RuntimeData;

//------------------------------------------------------------------------------
// ProceduralResourceLayer - Single resource type with eligibility-based distribution
//------------------------------------------------------------------------------
struct ProceduralResourceLayer {
    ClaymoreGUID Guid = ClaymoreGUID::Generate();
    std::string Name = "Resource Layer";
    bool Enabled = true;
    
    //--------------------------------------------------------------------------
    // Prefab Reference - What to spawn
    //--------------------------------------------------------------------------
    AssetReference PrefabAsset;
    std::string PrefabPath;  // Virtual path for display
    
    //--------------------------------------------------------------------------
    // Eligibility - Where to spawn (rejection sampling)
    //--------------------------------------------------------------------------
    CompositeEligibilityMap Eligibility;
    
    //--------------------------------------------------------------------------
    // Distribution Settings
    //--------------------------------------------------------------------------
    float DensityPerSquareMeter = 0.1f;  // Base density before rejection
    float MinSpacing = 1.0f;              // Minimum distance between instances
    
    // Scale variation
    float MinScale = 0.8f;
    float MaxScale = 1.2f;
    bool NonUniformScale = false;
    glm::vec3 MinScaleVec = glm::vec3(0.8f);
    glm::vec3 MaxScaleVec = glm::vec3(1.2f);
    
    // Rotation variation
    float YawVarianceDegrees = 360.0f;   // Random Y rotation
    float PitchVarianceDegrees = 0.0f;   // Random X rotation (for grass/plants)
    float RollVarianceDegrees = 0.0f;    // Random Z rotation
    bool AlignToSlope = false;
    float SlopeAlignmentFactor = 0.5f;   // 0 = ignore slope, 1 = fully align to normal
    
    // Height offset
    float HeightOffset = 0.0f;           // Offset above terrain surface
    float HeightOffsetVariance = 0.0f;   // Random variance
    
    //--------------------------------------------------------------------------
    // Clustering (optional grouped spawning)
    //--------------------------------------------------------------------------
    bool EnableClustering = false;
    float ClusterRadius = 10.0f;         // Radius of each cluster
    int ClusterMinCount = 3;             // Minimum instances per cluster
    int ClusterMaxCount = 8;             // Maximum instances per cluster
    float ClusterFalloff = 0.5f;         // Density falloff from cluster center
    float ClusterSpacing = 50.0f;        // Distance between cluster centers
    
    //--------------------------------------------------------------------------
    // LOD / Imposter Settings
    //--------------------------------------------------------------------------
    bool UseImposter = true;
    float ImposterDistance = 50.0f;      // Distance to switch to imposter
    float CullDistance = 500.0f;         // Distance to cull entirely
    float CrossfadeRange = 5.0f;         // Blend range for LOD transitions
    
    //--------------------------------------------------------------------------
    // Interaction Settings
    //--------------------------------------------------------------------------
    bool Interactable = false;           // Can player interact with these resources?
    bool PreservePhysics = false;        // Keep physics components when spawned (for forageable items)
    float InteractionRadius = 2.0f;      // How close player must be to interact
    std::string InteractionTag;          // Tag for gameplay systems (e.g., "forageable", "mineable")
    
    //--------------------------------------------------------------------------
    // Preview Settings
    //--------------------------------------------------------------------------
    glm::vec3 PreviewColor = glm::vec3(0.4f, 0.8f, 0.4f);  // Debug visualization color
    bool ShowPreviewMarkers = true;      // Show debug markers for instances
    
    //--------------------------------------------------------------------------
    // Runtime Cache (not serialized)
    //--------------------------------------------------------------------------
    mutable struct {
        std::vector<ResourceInstance> Instances;
        bool Valid = false;
        uint64_t GenerationHash = 0;  // Hash of settings for change detection
    } Cache;
    
    std::unique_ptr<ImposterCache> BakedImposter;
    
    //--------------------------------------------------------------------------
    // Methods
    //--------------------------------------------------------------------------
    uint64_t ComputeSettingsHash() const;
    bool NeedsRegeneration() const;
    void InvalidateCache() { Cache.Valid = false; }
    
    // Copy/move (due to unique_ptr<ImposterCache> - all defined in .cpp where ImposterCache is complete)
    ProceduralResourceLayer();
    ~ProceduralResourceLayer();
    ProceduralResourceLayer(const ProceduralResourceLayer& other);
    ProceduralResourceLayer& operator=(const ProceduralResourceLayer& other);
    ProceduralResourceLayer(ProceduralResourceLayer&& other) noexcept;
    ProceduralResourceLayer& operator=(ProceduralResourceLayer&& other) noexcept;
};

//------------------------------------------------------------------------------
// ResourceLayerComponent - Main component attached to terrain entity
//------------------------------------------------------------------------------
struct ResourceLayerComponent {
    //--------------------------------------------------------------------------
    // Climate System
    //--------------------------------------------------------------------------
    ClimateConfig Climate;
    bool UseClimateGradients = true;
    
    //--------------------------------------------------------------------------
    // Resource Layers
    //--------------------------------------------------------------------------
    std::vector<ProceduralResourceLayer> Layers;
    
    //--------------------------------------------------------------------------
    // Global Settings
    //--------------------------------------------------------------------------
    uint32_t GlobalSeed = 12345;
    float GlobalDensityMultiplier = 1.0f;
    float GlobalSwapDistance = 30.0f;     // Default imposter→prefab swap distance
    float SwapHysteresis = 5.0f;          // Prevents rapid swap oscillation
    uint32_t MaxActivePrefabs = 256;      // Budget for full prefabs
    
    //--------------------------------------------------------------------------
    // Optional Region Definitions (for RegionMaskFilter)
    //--------------------------------------------------------------------------
    std::vector<RegionPolygon> Regions;
    
    //--------------------------------------------------------------------------
    // Optional Road/Path Splines (for RoadDistanceFilter)
    //--------------------------------------------------------------------------
    std::vector<std::vector<glm::vec3>> Roads;
    
    //--------------------------------------------------------------------------
    // Runtime State (not serialized)
    //--------------------------------------------------------------------------
    BiomeRuntimeData Runtime;
    
    //--------------------------------------------------------------------------
    // Persistent State (serialized with scene)
    //--------------------------------------------------------------------------
    BiomePersistentState Persistent;
    
    //--------------------------------------------------------------------------
    // Generation State
    //--------------------------------------------------------------------------
    bool NeedsRegeneration = true;
    bool NeedsFullRegeneration = true;  // Regenerate all layers
    float LastGenerationTime = 0.0f;    // For async generation
    
    //--------------------------------------------------------------------------
    // Statistics
    //--------------------------------------------------------------------------
    struct Statistics {
        uint32_t TotalInstances = 0;
        uint32_t VisibleImposters = 0;
        uint32_t ActivePrefabs = 0;
        uint32_t CulledInstances = 0;
        float GenerationTimeMs = 0.0f;
        float LastFrameTimeMs = 0.0f;
    } Stats;
    
    //--------------------------------------------------------------------------
    // Methods
    //--------------------------------------------------------------------------
    
    // Full regeneration of all layers
    void Regenerate(const struct TerrainComponent& terrain, const glm::mat4& terrainTransform = glm::mat4(1.0f));
    
    // Regenerate specific layer only
    void RegenerateLayer(uint32_t layerIndex, const struct TerrainComponent& terrain, const glm::mat4& terrainTransform = glm::mat4(1.0f));
    
    // Mark instance as destroyed (won't respawn)
    void MarkInstanceDestroyed(uint32_t instanceId);
    
    // Mark instance as modified (keeps as prefab, won't swap to imposter)
    void MarkInstanceModified(uint32_t instanceId);
    
    // Update visibility and LOD for current camera position
    void UpdateVisibility(const glm::vec3& cameraPos, const glm::mat4& viewProj);
    
    // Build eligibility context for filter evaluation
    EligibilityContext BuildContext(const struct TerrainComponent& terrain) const;
    
    // Get layer by GUID
    ProceduralResourceLayer* GetLayer(const ClaymoreGUID& guid);
    const ProceduralResourceLayer* GetLayer(const ClaymoreGUID& guid) const;
    
    // Add/remove layers
    void AddLayer(const ProceduralResourceLayer& layer);
    void RemoveLayer(const ClaymoreGUID& guid);
    void RemoveLayerAt(size_t index);
    
    // Add/remove regions
    void AddRegion(const RegionPolygon& region);
    void RemoveRegion(const ClaymoreGUID& guid);
    
    // Add/remove roads
    void AddRoad(const std::vector<glm::vec3>& roadPoints);
    void RemoveRoad(size_t index);
    void ClearRoads();
    
    //--------------------------------------------------------------------------
    // Interaction Queries (for forageable/mineable resources)
    //--------------------------------------------------------------------------
    
    // Result of a nearby resource query
    struct NearbyResource {
        uint32_t InstanceIndex;
        uint32_t LayerIndex;
        glm::vec3 Position;
        float Distance;
        std::string InteractionTag;
        bool IsActive;  // Already spawned as full prefab
    };
    
    // Find all interactable resources within radius of a point
    std::vector<NearbyResource> QueryInteractableNearby(const glm::vec3& position, float radius) const;
    
    // Find the closest interactable resource to a point
    bool GetClosestInteractable(const glm::vec3& position, float maxRadius, NearbyResource& outResult) const;
    
    // Force-spawn a specific instance as full prefab (for interaction)
    // Returns the spawned entity ID, or INVALID_ENTITY_ID if failed
    EntityID SpawnForInteraction(uint32_t instanceIndex, class Scene& scene);
};

//------------------------------------------------------------------------------
// ResourceDistributor - Handles procedural placement with rejection sampling
//------------------------------------------------------------------------------
class ResourceDistributor {
public:
    struct DistributionParams {
        uint32_t Seed = 0;
        float DensityMultiplier = 1.0f;
        const struct TerrainComponent* Terrain = nullptr;
        glm::mat4 TerrainTransform = glm::mat4(1.0f);
        const EligibilityContext* Context = nullptr;
        
        // Area bounds (in world space)
        glm::vec2 AreaMin = glm::vec2(-1000.0f);
        glm::vec2 AreaMax = glm::vec2(1000.0f);
    };
    
    // Generate instances for a single layer using rejection sampling
    void GenerateInstances(
        const ProceduralResourceLayer& layer,
        const DistributionParams& params,
        std::vector<ResourceInstance>& outInstances);
    
    // Generate clustered instances
    void GenerateClusteredInstances(
        const ProceduralResourceLayer& layer,
        const DistributionParams& params,
        std::vector<ResourceInstance>& outInstances);
    
private:
    // Poisson disk sampling for minimum spacing
    void PoissonDiskSample(
        const glm::vec2& areaMin,
        const glm::vec2& areaMax,
        float minDistance,
        float density,
        uint32_t seed,
        std::vector<glm::vec2>& outPositions);
    
    // Sample terrain height at position
    bool SampleTerrainHeight(
        const struct TerrainComponent* terrain,
        const glm::mat4& transform,
        const glm::vec2& xz,
        float& outHeight);
    
    // Sample terrain slope at position
    bool SampleTerrainSlope(
        const struct TerrainComponent* terrain,
        const glm::mat4& transform,
        const glm::vec2& xz,
        float& outSlopeDegrees,
        glm::vec3& outNormal);
    
    // Generate stable instance ID from position
    uint32_t GenerateInstanceID(const glm::vec3& position, uint32_t seed);
    
    // Generate random transform variation
    void ApplyRandomVariation(
        ResourceInstance& inst,
        const ProceduralResourceLayer& layer,
        const glm::vec3& terrainNormal,
        std::mt19937& rng);
};

} // namespace resourcelayer
} // namespace cm

