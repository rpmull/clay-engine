#pragma once

#include <glm/glm.hpp>
#include <bgfx/bgfx.h>
#include <vector>
#include <array>
#include <functional>

// Forward declarations
struct TerrainComponent;
struct TerrainChunk;
class JobSystem;

namespace terrain {

// Direction indices for neighbor lookups
enum ChunkDirection : uint8_t
{
   DIR_NORTH = 0,  // +Z
   DIR_EAST = 1,   // +X
   DIR_SOUTH = 2,  // -Z
   DIR_WEST = 3    // -X
};

// Frustum for chunk culling
struct ChunkFrustum
{
   glm::vec4 Planes[6];  // Left, Right, Bottom, Top, Near, Far
   
   static ChunkFrustum FromViewProj(const glm::mat4& viewProj);
   bool ContainsAABB(const glm::vec3& min, const glm::vec3& max) const;
};

// Per-chunk render data prepared for GPU submission
struct ChunkRenderData
{
   glm::vec4 ChunkParams;    // xy=UV offset, zw=UV scale
   glm::vec4 ChunkWorld;     // xy=world offset, zw=world extent
   glm::vec4 MorphParams;    // x=morph factor, y=LOD level, z=grid size, w=unused
   glm::vec4 NeighborLODs;   // x=N, y=E, z=S, w=W neighbor LOD levels
   int LODLevel;
   bool Visible;
   uint32_t ChunkIndex;
};

// Instance data for GPU instanced rendering of terrain chunks
// Layout: 4 vec4s (64 bytes) matching bgfx instance convention (i_data0..i_data3)
// This allows all visible chunks to be rendered in a single draw call per LOD level
struct ChunkInstanceData
{
   glm::vec4 ChunkParams;    // i_data0: xy=UV offset into heightmap, zw=UV scale
   glm::vec4 ChunkWorld;     // i_data1: xy=world XZ offset, zw=world XZ extent  
   glm::vec4 MorphParams;    // i_data2: x=morph factor, y=LOD level, z=grid size, w=unused
   glm::vec4 NeighborLODs;   // i_data3: x=North LOD, y=East LOD, z=South LOD, w=West LOD
   
   void SetFromChunk(const ChunkRenderData& rd)
   {
      ChunkParams = rd.ChunkParams;
      ChunkWorld = rd.ChunkWorld;
      MorphParams = rd.MorphParams;
      NeighborLODs = rd.NeighborLODs;
   }
};
static_assert(sizeof(ChunkInstanceData) == 64, "ChunkInstanceData must be 64 bytes for bgfx instancing");

// Per-LOD batch for instanced chunk rendering
struct ChunkLODBatch
{
   std::vector<ChunkInstanceData> Instances;
   uint32_t LODLevel = 0;
   
   void Clear() { Instances.clear(); }
   void Reserve(size_t count) { Instances.reserve(count); }
   bool Empty() const { return Instances.empty(); }
   size_t Size() const { return Instances.size(); }
};

// Chunk system configuration
struct ChunkSystemConfig
{
   uint32_t ChunkVertexSize = 33;     // Vertices per edge (power of 2 + 1)
   bool EnableMorphing = true;
   float MorphRegion = 0.3f;          // Fraction of LOD band for morph transition
   float LODDistances[4] = { 50.0f, 150.0f, 400.0f, 1000.0f };
   uint32_t LODSteps[4] = { 1, 2, 4, 8 };
   static constexpr uint32_t kMaxLODLevels = 4;
};

// Main terrain chunk management class
class TerrainChunkSystem
{
public:
   TerrainChunkSystem() = default;
   ~TerrainChunkSystem() { Shutdown(); }
   
   // Initialize chunk layout for terrain
   // Creates NxN chunks based on terrain resolution and chunk vertex size
   void Init(TerrainComponent& terrain, const ChunkSystemConfig& config = ChunkSystemConfig{});
   
   // Clean up GPU resources
   void Shutdown();
   
   // Rebuild chunk layout (call when terrain resolution or chunk size changes)
   void RebuildChunkLayout(TerrainComponent& terrain);
   
   // Build shared vertex buffer and per-LOD index buffers
   void BuildChunkMeshBuffers(TerrainComponent& terrain);
   
   // Update per-chunk LOD selection and culling (can be called from parallel jobs)
   // Returns number of visible chunks
   uint32_t UpdateChunkLOD(TerrainComponent& terrain,
                           const glm::vec3& cameraPos,
                           const glm::mat4& worldMatrix,
                           const ChunkFrustum& frustum);
   
   // Parallel version using job system
   uint32_t UpdateChunkLODParallel(TerrainComponent& terrain,
                                    const glm::vec3& cameraPos,
                                    const glm::mat4& worldMatrix,
                                    const ChunkFrustum& frustum,
                                    JobSystem& jobs);
   
   // Enforce restricted LOD (max difference of 1 between neighbors)
   // Call after UpdateChunkLOD to fix LOD discontinuities
   void EnforceRestrictedLOD(TerrainComponent& terrain);
   
   // Calculate morph factors for all chunks
   void UpdateMorphFactors(TerrainComponent& terrain, const glm::vec3& cameraPos);
   
   // Update neighbor LOD info for edge morphing
   void UpdateNeighborLODs(TerrainComponent& terrain);
   
   // Prepare render data for all visible chunks
   void PrepareRenderData(const TerrainComponent& terrain,
                          std::vector<ChunkRenderData>& outRenderData);
   
   // Prepare instanced batches grouped by LOD level for efficient rendering
   // Returns total number of visible chunks across all batches
   uint32_t PrepareInstancedBatches(const TerrainComponent& terrain,
                                     std::array<ChunkLODBatch, ChunkSystemConfig::kMaxLODLevels>& outBatches);

   // Prepare instanced batches for visible chunks near the active camera.
   // Used by shadow cascades so large terrains do not submit distant camera-visible
   // chunks into every shadow map split.
   uint32_t PrepareInstancedBatchesNearCamera(const TerrainComponent& terrain,
                                               float maxCameraDistance,
                                               std::array<ChunkLODBatch, ChunkSystemConfig::kMaxLODLevels>& outBatches);
   
   // Getters
   const ChunkSystemConfig& GetConfig() const { return m_Config; }
   uint32_t GetChunksX() const { return m_ChunksX; }
   uint32_t GetChunksZ() const { return m_ChunksZ; }
   uint32_t GetTotalChunks() const { return m_ChunksX * m_ChunksZ; }
   
   // Get index buffer for LOD level
   bgfx::IndexBufferHandle GetLODIndexBuffer(const TerrainComponent& terrain, uint32_t lod) const;
   uint32_t GetLODIndexCount(const TerrainComponent& terrain, uint32_t lod) const;

private:
   // Compute bounds for a single chunk from height data
   void ComputeChunkBounds(const TerrainComponent& terrain, TerrainChunk& chunk);
   
   // Select LOD level based on distance
   int SelectLOD(float distance) const;
   
   // Calculate morph factor for smooth transitions
   float CalculateMorphFactor(float distance, int lodLevel) const;
   
   // Get neighbor chunk index (-1 if out of bounds)
   int GetNeighborIndex(uint32_t chunkIndex, ChunkDirection dir) const;
   
   ChunkSystemConfig m_Config;
   uint32_t m_ChunksX = 0;
   uint32_t m_ChunksZ = 0;
   bool m_Initialized = false;
};

// Utility: Transform local AABB to world space and test against frustum
bool ChunkVisibleInFrustum(const TerrainChunk& chunk,
                            const glm::mat4& worldMatrix,
                            const ChunkFrustum& frustum);

} // namespace terrain

