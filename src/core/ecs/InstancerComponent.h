#pragma once

#include "Entity.h"
#include "core/assets/AssetReference.h"
#include "core/rendering/Material.h"
#include "core/rendering/Mesh.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <bgfx/bgfx.h>
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <random>

// Forward declarations
struct TerrainComponent;
class Scene;

namespace cm {
namespace instancer {

//------------------------------------------------------------------------------
// InstanceState - Lifecycle state of a single instance
//------------------------------------------------------------------------------
enum class InstanceState : uint8_t {
    Pristine = 0,   // Untouched - render as instanced mesh, can swap to prefab
    Active,         // Currently a full prefab in scene (close to camera)
    Modified,       // User modified the prefab - keep as prefab forever
    Destroyed       // User destroyed - won't render or respawn
};

//------------------------------------------------------------------------------
// InstanceData - A single placed instance
//------------------------------------------------------------------------------
struct InstanceData {
    uint32_t InstanceID = 0;       // Stable ID for persistence (hash of position + seed)
    
    glm::vec3 Position{0.0f};
    glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 Scale{1.0f};
    
    InstanceState State = InstanceState::Pristine;
    
    // Runtime (not serialized)
    EntityID ActivePrefabEntity = INVALID_ENTITY_ID;  // When swapped to prefab
    float DistanceToCamera = 0.0f;
    bool Visible = true;  // Frustum/distance culling result
};

//------------------------------------------------------------------------------
// MeshInstanceBatch - GPU data for rendering instances of a single submesh
//------------------------------------------------------------------------------
struct MeshInstanceBatch {
    bgfx::VertexBufferHandle VBH = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle IBH = BGFX_INVALID_HANDLE;
    uint32_t IndexStart = 0;
    uint32_t IndexCount = 0;
    uint32_t MaterialSlot = 0;
    std::shared_ptr<Material> BatchMaterial;
    std::shared_ptr<Mesh> MeshRef;  // Keep mesh alive for buffer handles
    glm::mat4 LocalTransform{1.0f};  // Per-mesh local transform from model
    bool UseAlphaCutout = false;
    float AlphaCutoutThreshold = 0.5f;
    
    // Instance transforms collected for this batch each frame
    std::vector<glm::mat4> InstanceTransforms;
    
    bool IsValid() const {
        return bgfx::isValid(VBH) && bgfx::isValid(IBH) && IndexCount > 0;
    }
};

//------------------------------------------------------------------------------
// CachedMeshData - Cached mesh/material data from the source model
//------------------------------------------------------------------------------
struct CachedMeshData {
    std::shared_ptr<Mesh> MeshPtr;
    std::vector<MeshInstanceBatch> Batches;  // One per submesh
    glm::vec3 BoundsMin{0.0f};
    glm::vec3 BoundsMax{0.0f};
    bool Valid = false;
    
    void Release();
};

//------------------------------------------------------------------------------
// DistributionSettings - Controls procedural placement
//------------------------------------------------------------------------------
struct DistributionSettings {
    uint32_t Seed = 12345;
    float DensityPerSquareMeter = 0.05f;  // How many instances per m²
    float MinSpacing = 2.0f;               // Minimum distance between instances
    
    // Scale variation
    float MinScale = 0.8f;
    float MaxScale = 1.2f;
    bool NonUniformScale = false;
    glm::vec3 MinScaleVec{0.8f};
    glm::vec3 MaxScaleVec{1.2f};
    
    // Rotation variation
    float YawVarianceDegrees = 360.0f;     // Random Y rotation
    float PitchVarianceDegrees = 0.0f;     // Random X rotation
    float RollVarianceDegrees = 0.0f;      // Random Z rotation
    bool AlignToSlope = false;             // Align to terrain normal
    float SlopeAlignmentFactor = 0.5f;     // 0 = ignore slope, 1 = fully align
    
    // Slope filtering
    float MinSlopeDegrees = 0.0f;          // Minimum slope to place on
    float MaxSlopeDegrees = 45.0f;         // Maximum slope to place on
    
    // Height offset
    float HeightOffset = 0.0f;             // Offset above surface
    float HeightOffsetVariance = 0.0f;     // Random variance
};

//------------------------------------------------------------------------------
// SwapSettings - Controls LOD/prefab hot-swap behavior
//------------------------------------------------------------------------------
struct SwapSettings {
    float SwapDistance = 30.0f;            // Distance to swap imposter → prefab
    float SwapHysteresis = 5.0f;           // Prevents rapid swap oscillation
    float CullDistance = 500.0f;           // Distance to cull entirely
    uint32_t MaxActivePrefabs = 64;        // Budget for full prefabs in scene
};

//------------------------------------------------------------------------------
// RuntimeData - Runtime-only state for the instancer
//------------------------------------------------------------------------------
struct RuntimeData {
    std::vector<InstanceData> Instances;
    
    // Spatial hash for fast proximity queries
    std::unordered_map<int64_t, std::vector<uint32_t>> SpatialHash;
    float CellSize = 50.0f;
    std::vector<uint32_t> VisibleInstanceIndices;
    
    // Tracking sets
    std::unordered_set<uint32_t> DestroyedInstances;
    std::unordered_map<uint32_t, InstanceState> ModifiedInstances;
    std::unordered_set<uint32_t> ActivePrefabIndices;
    
    // Stats
    uint32_t TotalInstances = 0;
    uint32_t VisibleInstances = 0;
    uint32_t ActivePrefabs = 0;
    uint32_t CulledInstances = 0;
    
    void Clear();
    void AddToSpatialHash(uint32_t instanceIndex);
    void UpdateSpatialHash();
    void QueryNearby(const glm::vec3& pos, float radius, std::vector<uint32_t>& outIndices) const;
    std::vector<uint32_t> QueryNearby(const glm::vec3& pos, float radius) const;
};

//------------------------------------------------------------------------------
// PersistentState - Saved instance modifications (serialized with scene)
//------------------------------------------------------------------------------
struct PersistentState {
    std::vector<uint32_t> DestroyedIDs;
    std::unordered_map<uint32_t, InstanceState> StateOverrides;
};

//------------------------------------------------------------------------------
// InstancerComponent - Main component for optimized mesh instancing
//------------------------------------------------------------------------------
struct InstancerComponent {
    //--------------------------------------------------------------------------
    // Asset References
    //--------------------------------------------------------------------------
    
    // Mesh model for instanced rendering (what to draw when far away)
    // Each submesh is rendered with its own material via GPU instancing
    AssetReference MeshAsset;
    std::string MeshPath;  // Display path
    
    // Prefab for hot-swap (what to instantiate when close)
    // This allows full interaction, physics, scripts, etc.
    AssetReference PrefabAsset;
    std::string PrefabPath;  // Display path
    
    // Optional surface entity for distribution (e.g., terrain)
    // If invalid, uses a manual point list or radius from instancer entity
    EntityID SurfaceEntity = INVALID_ENTITY_ID;
    
    //--------------------------------------------------------------------------
    // Distribution
    //--------------------------------------------------------------------------
    DistributionSettings Distribution;
    
    // Manual distribution area (when no surface entity)
    float DistributionRadius = 100.0f;     // Radius from instancer entity
    glm::vec2 DistributionAreaMin{-100.0f};
    glm::vec2 DistributionAreaMax{100.0f};
    bool UseRadiusMode = true;             // If true, use radius; if false, use area bounds
    
    // Manual point list (optional - overrides procedural distribution)
    std::vector<glm::vec3> ManualPoints;
    bool UseManualPoints = false;
    
    //--------------------------------------------------------------------------
    // Swap/LOD Settings
    //--------------------------------------------------------------------------
    SwapSettings Swap;
    
    //--------------------------------------------------------------------------
    // State
    //--------------------------------------------------------------------------
    RuntimeData Runtime;
    PersistentState Persistent;
    
    //--------------------------------------------------------------------------
    // Cached Mesh Data
    //--------------------------------------------------------------------------
    CachedMeshData CachedMesh;
    
    //--------------------------------------------------------------------------
    // Flags
    //--------------------------------------------------------------------------
    bool Enabled = true;
    bool NeedsRegeneration = true;
    bool NeedsMeshReload = true;
    
    //--------------------------------------------------------------------------
    // Rendering Options
    //--------------------------------------------------------------------------
    bool UseAlphaCutout = false;            // Convert alpha blend to cutout (discard)
    float AlphaCutoutThreshold = 0.5f;      // Alpha value below which pixels are discarded
    
    //--------------------------------------------------------------------------
    // Debug/Preview
    //--------------------------------------------------------------------------
    glm::vec3 PreviewColor{0.2f, 0.8f, 0.3f};
    bool ShowDebugMarkers = false;
    bool ShowBounds = false;
    
    //--------------------------------------------------------------------------
    // Statistics
    //--------------------------------------------------------------------------
    struct Statistics {
        uint32_t TotalInstances = 0;
        uint32_t VisibleInstances = 0;
        uint32_t InstancedDraws = 0;
        uint32_t ActivePrefabs = 0;
        uint32_t CulledInstances = 0;
        float LastFrameTimeMs = 0.0f;
        float LastGenerationTimeMs = 0.0f;
    } Stats;

    // Cached editor visibility state. These values are transient runtime data
    // used to skip repeated visibility/swap work while the camera is idle.
    glm::vec3 LastVisibilityCameraPos{0.0f};
    glm::mat4 LastVisibilityViewProj{1.0f};
    bool VisibilityCacheValid = false;
    
    //--------------------------------------------------------------------------
    // Methods
    //--------------------------------------------------------------------------
    
    // Regenerate instance positions based on distribution settings
    void Regenerate(const glm::mat4& instancerTransform, const TerrainComponent* terrain = nullptr, const glm::mat4& terrainTransform = glm::mat4(1.0f));
    
    // Update visibility for current camera
    void UpdateVisibility(const glm::vec3& cameraPos, const glm::mat4& viewProj, const glm::mat4& proj);
    
    // Mark an instance as destroyed (won't render or respawn)
    void MarkInstanceDestroyed(uint32_t instanceId);
    
    // Mark an instance as modified (keeps as prefab forever)
    void MarkInstanceModified(uint32_t instanceId);
    
    // Force reload mesh data from asset
    void ReloadMesh();
    
    // Clear all cached data
    void ClearCache();
    
    // Check if mesh asset is valid
    bool HasValidMesh() const;
    
    // Check if prefab asset is valid
    bool HasValidPrefab() const;
    
    // Constructor/Destructor
    InstancerComponent();
    ~InstancerComponent();
    InstancerComponent(const InstancerComponent& other);
    InstancerComponent& operator=(const InstancerComponent& other);
    InstancerComponent(InstancerComponent&& other) noexcept;
    InstancerComponent& operator=(InstancerComponent&& other) noexcept;
};

} // namespace instancer
} // namespace cm

