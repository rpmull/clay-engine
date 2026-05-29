#include "TerrainChunks.h"
#include "Terrain.h"
#include "VertexTypes.h"
#include "core/ecs/Components.h"
#include "core/jobs/JobSystem.h"
#include "core/jobs/ParallelFor.h"

#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace terrain {

namespace {
   void ComputeChunkWorldAabb(const TerrainChunk& chunk,
                               const glm::mat4& worldMatrix,
                               glm::vec3& outMin,
                               glm::vec3& outMax)
   {
      const glm::vec3 corners[8] = {
         glm::vec3(chunk.WorldMin.x, chunk.WorldMin.y, chunk.WorldMin.z),
         glm::vec3(chunk.WorldMax.x, chunk.WorldMin.y, chunk.WorldMin.z),
         glm::vec3(chunk.WorldMin.x, chunk.WorldMax.y, chunk.WorldMin.z),
         glm::vec3(chunk.WorldMax.x, chunk.WorldMax.y, chunk.WorldMin.z),
         glm::vec3(chunk.WorldMin.x, chunk.WorldMin.y, chunk.WorldMax.z),
         glm::vec3(chunk.WorldMax.x, chunk.WorldMin.y, chunk.WorldMax.z),
         glm::vec3(chunk.WorldMin.x, chunk.WorldMax.y, chunk.WorldMax.z),
         glm::vec3(chunk.WorldMax.x, chunk.WorldMax.y, chunk.WorldMax.z)
      };

      outMin = glm::vec3(std::numeric_limits<float>::max());
      outMax = glm::vec3(std::numeric_limits<float>::lowest());

      for (int i = 0; i < 8; ++i)
      {
         const glm::vec4 wc = worldMatrix * glm::vec4(corners[i], 1.0f);
         const glm::vec3 w = glm::vec3(wc);
         outMin = glm::min(outMin, w);
         outMax = glm::max(outMax, w);
      }
   }

   float DistanceToAabbXZ(const glm::vec3& point,
                          const glm::vec3& min,
                          const glm::vec3& max)
   {
      const glm::vec2 p(point.x, point.z);
      const glm::vec2 min2(min.x, min.z);
      const glm::vec2 max2(max.x, max.z);
      const glm::vec2 clamped = glm::clamp(p, min2, max2);
      return glm::length(p - clamped);
   }
}

// ============================================================================
// ChunkFrustum Implementation
// ============================================================================

ChunkFrustum ChunkFrustum::FromViewProj(const glm::mat4& vp)
{
   ChunkFrustum f;
   
   // Extract frustum planes using Gribb-Hartmann method
   // Left
   f.Planes[0] = glm::vec4(
      vp[0][3] + vp[0][0],
      vp[1][3] + vp[1][0],
      vp[2][3] + vp[2][0],
      vp[3][3] + vp[3][0]);
   // Right
   f.Planes[1] = glm::vec4(
      vp[0][3] - vp[0][0],
      vp[1][3] - vp[1][0],
      vp[2][3] - vp[2][0],
      vp[3][3] - vp[3][0]);
   // Bottom
   f.Planes[2] = glm::vec4(
      vp[0][3] + vp[0][1],
      vp[1][3] + vp[1][1],
      vp[2][3] + vp[2][1],
      vp[3][3] + vp[3][1]);
   // Top
   f.Planes[3] = glm::vec4(
      vp[0][3] - vp[0][1],
      vp[1][3] - vp[1][1],
      vp[2][3] - vp[2][1],
      vp[3][3] - vp[3][1]);
   // Near
   f.Planes[4] = glm::vec4(
      vp[0][3] + vp[0][2],
      vp[1][3] + vp[1][2],
      vp[2][3] + vp[2][2],
      vp[3][3] + vp[3][2]);
   // Far
   f.Planes[5] = glm::vec4(
      vp[0][3] - vp[0][2],
      vp[1][3] - vp[1][2],
      vp[2][3] - vp[2][2],
      vp[3][3] - vp[3][2]);
   
   // Normalize planes
   for (int i = 0; i < 6; ++i)
   {
      float len = glm::length(glm::vec3(f.Planes[i]));
      if (len > 0.0001f)
         f.Planes[i] /= len;
   }
   
   return f;
}

bool ChunkFrustum::ContainsAABB(const glm::vec3& min, const glm::vec3& max) const
{
   for (int i = 0; i < 6; ++i)
   {
      const glm::vec4& p = Planes[i];
      
      // Find positive vertex (furthest along plane normal)
      glm::vec3 pVertex(
         p.x >= 0.0f ? max.x : min.x,
         p.y >= 0.0f ? max.y : min.y,
         p.z >= 0.0f ? max.z : min.z);
      
      // If positive vertex is behind plane, AABB is outside frustum
      if (glm::dot(glm::vec3(p), pVertex) + p.w < 0.0f)
         return false;
   }
   return true;
}

// ============================================================================
// Utility Functions
// ============================================================================

bool ChunkVisibleInFrustum(const TerrainChunk& chunk,
                            const glm::mat4& worldMatrix,
                            const ChunkFrustum& frustum)
{
   glm::vec3 worldMin;
   glm::vec3 worldMax;
   ComputeChunkWorldAabb(chunk, worldMatrix, worldMin, worldMax);
   return frustum.ContainsAABB(worldMin, worldMax);
}

// ============================================================================
// TerrainChunkSystem Implementation
// ============================================================================

void TerrainChunkSystem::Init(TerrainComponent& terrain, const ChunkSystemConfig& config)
{
   if (m_Initialized)
      Shutdown();
   
   m_Config = config;
   
   // Ensure valid chunk vertex size (power of 2 + 1)
   uint32_t size = std::max(9u, config.ChunkVertexSize);
   // Round to nearest power of 2 + 1
   uint32_t pot = 1;
   while (pot + 1 < size) pot *= 2;
   m_Config.ChunkVertexSize = pot + 1;
   
   RebuildChunkLayout(terrain);
   BuildChunkMeshBuffers(terrain);
   
   m_Initialized = true;
}

void TerrainChunkSystem::Shutdown()
{
   // Note: GPU resources are owned by TerrainComponent, not this system
   m_ChunksX = 0;
   m_ChunksZ = 0;
   m_Initialized = false;
}

void TerrainChunkSystem::RebuildChunkLayout(TerrainComponent& terrain)
{
   const uint32_t terrainRes = std::max(2u, terrain.GridResolution);
   const uint32_t chunkVerts = m_Config.ChunkVertexSize;
   
   // Calculate chunk grid dimensions
   // Chunks overlap by 1 vertex for seamless stitching
   const uint32_t effectiveChunkSize = chunkVerts - 1;
   m_ChunksX = (terrainRes + effectiveChunkSize - 2) / effectiveChunkSize;
   m_ChunksZ = m_ChunksX;
   
   // Ensure at least 1 chunk
   m_ChunksX = std::max(1u, m_ChunksX);
   m_ChunksZ = std::max(1u, m_ChunksZ);
   
   terrain.ChunksX = m_ChunksX;
   terrain.ChunksZ = m_ChunksZ;
   
   // Preserve unified texture handles from chunk 0 (if they exist)
   // All chunks share these textures via UV offsets
   bgfx::TextureHandle savedHeightTex = BGFX_INVALID_HANDLE;
   bgfx::TextureHandle savedSplatTex = BGFX_INVALID_HANDLE;
   bgfx::TextureHandle savedSplatTex2 = BGFX_INVALID_HANDLE;
   bgfx::TextureHandle savedHoleTex = BGFX_INVALID_HANDLE;
   if (!terrain.Chunks.empty())
   {
      savedHeightTex = terrain.Chunks[0].HeightTexture;
      savedSplatTex = terrain.Chunks[0].SplatTexture;
      savedSplatTex2 = terrain.Chunks[0].SplatTexture2;
      savedHoleTex = terrain.Chunks[0].HoleTexture;
      // Clear handles so they don't get destroyed when chunks are cleared
      terrain.Chunks[0].HeightTexture = BGFX_INVALID_HANDLE;
      terrain.Chunks[0].SplatTexture = BGFX_INVALID_HANDLE;
      terrain.Chunks[0].SplatTexture2 = BGFX_INVALID_HANDLE;
      terrain.Chunks[0].HoleTexture = BGFX_INVALID_HANDLE;
   }
   
   // Clear and rebuild chunks
   terrain.Chunks.clear();
   terrain.Chunks.reserve(m_ChunksX * m_ChunksZ);
   
   const glm::vec2 cellSize = Terrain::GetCellSize(terrain);
   const float uvPerChunk = 1.0f / static_cast<float>(std::max(1u, m_ChunksX));
   
   for (uint32_t cz = 0; cz < m_ChunksZ; ++cz)
   {
      for (uint32_t cx = 0; cx < m_ChunksX; ++cx)
      {
         TerrainChunk chunk;
         
         // Grid position
         chunk.GridX = cx;
         chunk.GridZ = cz;
         
         // Vertex start position (overlapping by 1)
         chunk.Start = glm::ivec2(
            cx * effectiveChunkSize,
            cz * effectiveChunkSize);
         
         // Clamp vertex counts to terrain bounds
         chunk.VertexCountX = std::min(chunkVerts, terrainRes - static_cast<uint32_t>(chunk.Start.x));
         chunk.VertexCountZ = std::min(chunkVerts, terrainRes - static_cast<uint32_t>(chunk.Start.y));
         
         // UV mapping into unified heightmap/splatmap
         chunk.UVOffset = glm::vec2(
            static_cast<float>(chunk.Start.x) / static_cast<float>(terrainRes - 1),
            static_cast<float>(chunk.Start.y) / static_cast<float>(terrainRes - 1));
         chunk.UVScale = glm::vec2(
            static_cast<float>(chunk.VertexCountX - 1) / static_cast<float>(terrainRes - 1),
            static_cast<float>(chunk.VertexCountZ - 1) / static_cast<float>(terrainRes - 1));
         
         // World position (local to terrain)
         chunk.WorldOffset = glm::vec2(
            static_cast<float>(chunk.Start.x) * cellSize.x,
            static_cast<float>(chunk.Start.y) * cellSize.y);
         chunk.WorldExtent = glm::vec2(
            static_cast<float>(chunk.VertexCountX - 1) * cellSize.x,
            static_cast<float>(chunk.VertexCountZ - 1) * cellSize.y);
         
         // Initialize state
         // Only chunk 0 should have dirty flags - all chunks share unified textures from chunk 0
         chunk.HeightDirty = (cx == 0 && cz == 0);
         chunk.SplatDirty = (cx == 0 && cz == 0);
         chunk.HoleDirty = (cx == 0 && cz == 0);
         chunk.CurrentLOD = 0;
         chunk.DesiredLOD = 0;
         chunk.Visible = true;
         chunk.MorphFactor = 0.0f;
         chunk.StreamState = ChunkStreamState::Resident;
         
         // Initialize neighbor LODs to -1 (will be updated)
         for (int i = 0; i < 4; ++i)
            chunk.NeighborLODs[i] = -1;
         
         // Compute initial bounds
         ComputeChunkBounds(terrain, chunk);
         
         terrain.Chunks.push_back(chunk);
      }
   }
   
   // Restore preserved unified texture handles to chunk 0
   if (!terrain.Chunks.empty())
   {
      terrain.Chunks[0].HeightTexture = savedHeightTex;
      terrain.Chunks[0].SplatTexture = savedSplatTex;
      terrain.Chunks[0].SplatTexture2 = savedSplatTex2;
      terrain.Chunks[0].HoleTexture = savedHoleTex;
      // Only mark dirty if textures weren't preserved
      terrain.Chunks[0].HeightDirty = !bgfx::isValid(savedHeightTex);
      terrain.Chunks[0].SplatDirty = !bgfx::isValid(savedSplatTex);
      terrain.Chunks[0].HoleDirty = !bgfx::isValid(savedHoleTex);
   }
   
   terrain.ChunkMeshDirty = true;
}

void TerrainChunkSystem::ComputeChunkBounds(const TerrainComponent& terrain, TerrainChunk& chunk)
{
   const glm::vec2 cellSize = Terrain::GetCellSize(terrain);
   
   // XZ bounds (local space)
   const float minX = chunk.WorldOffset.x;
   const float minZ = chunk.WorldOffset.y;
   const float maxX = minX + chunk.WorldExtent.x;
   const float maxZ = minZ + chunk.WorldExtent.y;
   
   // Sample height map to find Y bounds
   float minY = std::numeric_limits<float>::max();
   float maxY = std::numeric_limits<float>::lowest();
   
   const uint32_t startX = static_cast<uint32_t>(chunk.Start.x);
   const uint32_t startZ = static_cast<uint32_t>(chunk.Start.y);
   const uint32_t endX = startX + chunk.VertexCountX;
   const uint32_t endZ = startZ + chunk.VertexCountZ;
   
   for (uint32_t z = startZ; z < endZ && z < terrain.GridResolution; ++z)
   {
      for (uint32_t x = startX; x < endX && x < terrain.GridResolution; ++x)
      {
         const size_t idx = static_cast<size_t>(z) * terrain.GridResolution + x;
         if (idx < terrain.HeightMap.size())
         {
            const float h = (terrain.HeightMap[idx] / 65535.0f) * terrain.MaxHeight;
            minY = std::min(minY, h);
            maxY = std::max(maxY, h);
         }
      }
   }
   
   // Fallback if no valid samples
   if (minY > maxY)
   {
      minY = 0.0f;
      maxY = terrain.MaxHeight;
   }
   
   // Add small padding to avoid edge cases
   minY -= 0.1f;
   maxY += 0.1f;
   
   chunk.WorldMin = glm::vec3(minX, minY, minZ);
   chunk.WorldMax = glm::vec3(maxX, maxY, maxZ);
}

// Static vertex layout for chunk vertices
namespace {
   bgfx::VertexLayout s_ChunkVertexLayout;
   bool s_ChunkLayoutInit = false;
   
   void EnsureChunkVertexLayout()
   {
      if (!s_ChunkLayoutInit)
      {
         s_ChunkVertexLayout.begin()
            .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
            .end();
         s_ChunkLayoutInit = true;
      }
   }
}

void TerrainChunkSystem::BuildChunkMeshBuffers(TerrainComponent& terrain)
{
   const uint32_t chunkSize = m_Config.ChunkVertexSize;
   
   // Destroy old buffers if they exist
   if (bgfx::isValid(terrain.SharedChunkVB))
   {
      bgfx::destroy(terrain.SharedChunkVB);
      terrain.SharedChunkVB = BGFX_INVALID_HANDLE;
   }
   for (auto& ib : terrain.ChunkLODIndexBuffers)
   {
      if (bgfx::isValid(ib))
      {
         bgfx::destroy(ib);
         ib = BGFX_INVALID_HANDLE;
      }
   }
   
   EnsureChunkVertexLayout();
   
   // Build shared vertex buffer (local 0-1 coordinates)
   // Vertex format: localX, localZ (position derived in shader)
   struct ChunkVertex
   {
      float localX, localZ;  // 0-1 within chunk
   };
   
   std::vector<ChunkVertex> vertices(chunkSize * chunkSize);
   for (uint32_t z = 0; z < chunkSize; ++z)
   {
      for (uint32_t x = 0; x < chunkSize; ++x)
      {
         ChunkVertex& v = vertices[z * chunkSize + x];
         v.localX = static_cast<float>(x) / static_cast<float>(chunkSize - 1);
         v.localZ = static_cast<float>(z) / static_cast<float>(chunkSize - 1);
      }
   }
   
   const bgfx::Memory* vMem = bgfx::copy(vertices.data(),
      static_cast<uint32_t>(vertices.size() * sizeof(ChunkVertex)));
   terrain.SharedChunkVB = bgfx::createVertexBuffer(vMem, s_ChunkVertexLayout);
   
   // Build per-LOD index buffers
   for (uint32_t lod = 0; lod < ChunkSystemConfig::kMaxLODLevels; ++lod)
   {
      const uint32_t step = m_Config.LODSteps[lod];
      const uint32_t lodSize = (chunkSize - 1) / step + 1;
      
      if (lodSize < 2)
      {
         terrain.ChunkLODIndexCounts[lod] = 0;
         continue;
      }
      
      std::vector<uint16_t> indices;
      indices.reserve((lodSize - 1) * (lodSize - 1) * 6);
      
      for (uint32_t z = 0; z < lodSize - 1; ++z)
      {
         for (uint32_t x = 0; x < lodSize - 1; ++x)
         {
            // Map LOD grid position to full-resolution vertex indices
            uint32_t v00 = (z * step) * chunkSize + (x * step);
            uint32_t v10 = (z * step) * chunkSize + ((x + 1) * step);
            uint32_t v01 = ((z + 1) * step) * chunkSize + (x * step);
            uint32_t v11 = ((z + 1) * step) * chunkSize + ((x + 1) * step);
            
            // Triangle 1
            indices.push_back(static_cast<uint16_t>(v00));
            indices.push_back(static_cast<uint16_t>(v01));
            indices.push_back(static_cast<uint16_t>(v10));
            
            // Triangle 2
            indices.push_back(static_cast<uint16_t>(v10));
            indices.push_back(static_cast<uint16_t>(v01));
            indices.push_back(static_cast<uint16_t>(v11));
         }
      }
      
      terrain.ChunkLODIndexCounts[lod] = static_cast<uint32_t>(indices.size());
      
      if (!indices.empty())
      {
         const bgfx::Memory* iMem = bgfx::copy(indices.data(),
            static_cast<uint32_t>(indices.size() * sizeof(uint16_t)));
         terrain.ChunkLODIndexBuffers[lod] = bgfx::createIndexBuffer(iMem);
      }
   }
   
   terrain.ChunkMeshDirty = false;
}

int TerrainChunkSystem::SelectLOD(float distance) const
{
   for (uint32_t i = 0; i < ChunkSystemConfig::kMaxLODLevels; ++i)
   {
      if (distance <= m_Config.LODDistances[i])
         return static_cast<int>(i);
   }
   return static_cast<int>(ChunkSystemConfig::kMaxLODLevels - 1);
}

float TerrainChunkSystem::CalculateMorphFactor(float distance, int lodLevel) const
{
   if (!m_Config.EnableMorphing || lodLevel < 0)
      return 0.0f;
   
   const float lodDistance = m_Config.LODDistances[lodLevel];
   const float prevDistance = (lodLevel > 0) ? m_Config.LODDistances[lodLevel - 1] : 0.0f;
   const float lodBand = lodDistance - prevDistance;
   
   if (lodBand <= 0.0f)
      return 0.0f;
   
   // Calculate morph region (last N% of LOD band)
   const float morphStart = lodDistance - (lodBand * m_Config.MorphRegion);
   
   if (distance <= morphStart)
      return 0.0f;
   
   // Linear interpolation within morph region
   return glm::clamp((distance - morphStart) / (lodDistance - morphStart), 0.0f, 1.0f);
}

int TerrainChunkSystem::GetNeighborIndex(uint32_t chunkIndex, ChunkDirection dir) const
{
   const uint32_t x = chunkIndex % m_ChunksX;
   const uint32_t z = chunkIndex / m_ChunksX;
   
   switch (dir)
   {
      case DIR_NORTH: // +Z
         return (z < m_ChunksZ - 1) ? static_cast<int>((z + 1) * m_ChunksX + x) : -1;
      case DIR_EAST:  // +X
         return (x < m_ChunksX - 1) ? static_cast<int>(z * m_ChunksX + x + 1) : -1;
      case DIR_SOUTH: // -Z
         return (z > 0) ? static_cast<int>((z - 1) * m_ChunksX + x) : -1;
      case DIR_WEST:  // -X
         return (x > 0) ? static_cast<int>(z * m_ChunksX + x - 1) : -1;
      default:
         return -1;
   }
}

uint32_t TerrainChunkSystem::UpdateChunkLOD(TerrainComponent& terrain,
                                             const glm::vec3& cameraPos,
                                             const glm::mat4& worldMatrix,
                                             const ChunkFrustum& frustum)
{
   uint32_t visibleCount = 0;
   
   for (size_t i = 0; i < terrain.Chunks.size(); ++i)
   {
      TerrainChunk& chunk = terrain.Chunks[i];
      
      // Frustum cull
      glm::vec3 worldMin;
      glm::vec3 worldMax;
      ComputeChunkWorldAabb(chunk, worldMatrix, worldMin, worldMax);
      chunk.Visible = frustum.ContainsAABB(worldMin, worldMax);
      
      if (!chunk.Visible)
      {
         chunk.CurrentLOD = -1;
         continue;
      }
      
      // Calculate horizontal distance to chunk bounds (keeps LOD0 under camera)
      chunk.DistanceToCamera = DistanceToAabbXZ(cameraPos, worldMin, worldMax);
      
      // Select LOD
      chunk.DesiredLOD = SelectLOD(chunk.DistanceToCamera);
      chunk.CurrentLOD = chunk.DesiredLOD;
      
      ++visibleCount;
   }
   
   return visibleCount;
}

uint32_t TerrainChunkSystem::UpdateChunkLODParallel(TerrainComponent& terrain,
                                                     const glm::vec3& cameraPos,
                                                     const glm::mat4& worldMatrix,
                                                     const ChunkFrustum& frustum,
                                                     JobSystem& jobs)
{
   const size_t chunkCount = terrain.Chunks.size();
   if (chunkCount == 0)
      return 0;
   
   // Atomic counter for visible chunks
   std::atomic<uint32_t> visibleCount{0};
   
   // Process chunks in parallel
   parallel_for_auto(jobs, size_t(0), chunkCount,
      [&terrain, &cameraPos, &worldMatrix, &frustum, &visibleCount, this]
      (size_t start, size_t count)
      {
         uint32_t localVisible = 0;
         
         for (size_t i = start; i < start + count; ++i)
         {
            TerrainChunk& chunk = terrain.Chunks[i];
            
            // Frustum cull
            glm::vec3 worldMin;
            glm::vec3 worldMax;
            ComputeChunkWorldAabb(chunk, worldMatrix, worldMin, worldMax);
            chunk.Visible = frustum.ContainsAABB(worldMin, worldMax);
            
            if (!chunk.Visible)
            {
               chunk.CurrentLOD = -1;
               continue;
            }
            
            // Calculate horizontal distance to chunk bounds (keeps LOD0 under camera)
            chunk.DistanceToCamera = DistanceToAabbXZ(cameraPos, worldMin, worldMax);
            
            // Select LOD
            chunk.DesiredLOD = SelectLOD(chunk.DistanceToCamera);
            chunk.CurrentLOD = chunk.DesiredLOD;
            
            ++localVisible;
         }
         
         visibleCount.fetch_add(localVisible, std::memory_order_relaxed);
      },
      32,  // Min granularity
      JobSystem::Priority::High);
   
   return visibleCount.load(std::memory_order_relaxed);
}

void TerrainChunkSystem::EnforceRestrictedLOD(TerrainComponent& terrain)
{
   // Iteratively enforce max LOD difference of 1 between neighbors
   bool changed = true;
   int iterations = 0;
   const int maxIterations = 10;  // Safety limit
   
   while (changed && iterations < maxIterations)
   {
      changed = false;
      ++iterations;
      
      for (size_t i = 0; i < terrain.Chunks.size(); ++i)
      {
         TerrainChunk& chunk = terrain.Chunks[i];
         
         if (!chunk.Visible)
            continue;
         
         // Check all 4 neighbors
         for (int dir = 0; dir < 4; ++dir)
         {
            int neighborIdx = GetNeighborIndex(static_cast<uint32_t>(i), static_cast<ChunkDirection>(dir));
            if (neighborIdx < 0)
               continue;
            
            TerrainChunk& neighbor = terrain.Chunks[neighborIdx];
            if (!neighbor.Visible)
               continue;
            
            // Enforce max LOD difference of 1
            if (chunk.CurrentLOD > neighbor.CurrentLOD + 1)
            {
               chunk.CurrentLOD = neighbor.CurrentLOD + 1;
               changed = true;
            }
         }
      }
   }
}

void TerrainChunkSystem::UpdateMorphFactors(TerrainComponent& terrain, const glm::vec3& cameraPos)
{
   for (TerrainChunk& chunk : terrain.Chunks)
   {
      if (!chunk.Visible || chunk.CurrentLOD < 0)
      {
         chunk.MorphFactor = 0.0f;
         continue;
      }
      
      chunk.MorphFactor = CalculateMorphFactor(chunk.DistanceToCamera, chunk.CurrentLOD);
   }
}

void TerrainChunkSystem::UpdateNeighborLODs(TerrainComponent& terrain)
{
   for (size_t i = 0; i < terrain.Chunks.size(); ++i)
   {
      TerrainChunk& chunk = terrain.Chunks[i];
      
      for (int dir = 0; dir < 4; ++dir)
      {
         int neighborIdx = GetNeighborIndex(static_cast<uint32_t>(i), static_cast<ChunkDirection>(dir));
         
         if (neighborIdx < 0)
         {
            // Edge of terrain - use own LOD
            chunk.NeighborLODs[dir] = chunk.CurrentLOD;
         }
         else
         {
            const TerrainChunk& neighbor = terrain.Chunks[neighborIdx];
            chunk.NeighborLODs[dir] = neighbor.Visible ? neighbor.CurrentLOD : chunk.CurrentLOD;
         }
      }
   }
}

void TerrainChunkSystem::PrepareRenderData(const TerrainComponent& terrain,
                                            std::vector<ChunkRenderData>& outRenderData)
{
   outRenderData.clear();
   outRenderData.reserve(terrain.Chunks.size());
   
   for (size_t i = 0; i < terrain.Chunks.size(); ++i)
   {
      const TerrainChunk& chunk = terrain.Chunks[i];
      
      if (!chunk.Visible || chunk.CurrentLOD < 0)
         continue;
      
      ChunkRenderData rd;
      rd.ChunkParams = glm::vec4(chunk.UVOffset.x, chunk.UVOffset.y,
                                  chunk.UVScale.x, chunk.UVScale.y);
      rd.ChunkWorld = glm::vec4(chunk.WorldOffset.x, chunk.WorldOffset.y,
                                 chunk.WorldExtent.x, chunk.WorldExtent.y);
      rd.MorphParams = glm::vec4(chunk.MorphFactor, 
                                  static_cast<float>(chunk.CurrentLOD),
                                  static_cast<float>(m_Config.ChunkVertexSize),
                                  0.0f);
      rd.NeighborLODs = glm::vec4(
         static_cast<float>(chunk.NeighborLODs[DIR_NORTH]),
         static_cast<float>(chunk.NeighborLODs[DIR_EAST]),
         static_cast<float>(chunk.NeighborLODs[DIR_SOUTH]),
         static_cast<float>(chunk.NeighborLODs[DIR_WEST]));
      rd.LODLevel = chunk.CurrentLOD;
      rd.Visible = true;
      rd.ChunkIndex = static_cast<uint32_t>(i);
      
      outRenderData.push_back(rd);
   }
}

uint32_t TerrainChunkSystem::PrepareInstancedBatches(const TerrainComponent& terrain,
                                                      std::array<ChunkLODBatch, ChunkSystemConfig::kMaxLODLevels>& outBatches)
{
   // Clear all batches
   for (auto& batch : outBatches)
      batch.Clear();
   
   // Reserve space (estimate ~25% of chunks per LOD as a heuristic)
   const size_t reservePerLOD = (terrain.Chunks.size() + 3) / 4;
   for (auto& batch : outBatches)
      batch.Reserve(reservePerLOD);
   
   uint32_t totalVisible = 0;
   
   for (size_t i = 0; i < terrain.Chunks.size(); ++i)
   {
      const TerrainChunk& chunk = terrain.Chunks[i];
      
      // Skip invisible or invalid chunks
      if (!chunk.Visible || chunk.CurrentLOD < 0)
         continue;
      
      // Skip unloaded chunks (streaming)
      if (chunk.StreamState != ChunkStreamState::Resident)
         continue;
      
      const uint32_t lodLevel = static_cast<uint32_t>(chunk.CurrentLOD);
      if (lodLevel >= ChunkSystemConfig::kMaxLODLevels)
         continue;
      
      // Build instance data
      ChunkInstanceData inst;
      inst.ChunkParams = glm::vec4(chunk.UVOffset.x, chunk.UVOffset.y,
                                    chunk.UVScale.x, chunk.UVScale.y);
      inst.ChunkWorld = glm::vec4(chunk.WorldOffset.x, chunk.WorldOffset.y,
                                   chunk.WorldExtent.x, chunk.WorldExtent.y);
      inst.MorphParams = glm::vec4(chunk.MorphFactor,
                                    static_cast<float>(chunk.CurrentLOD),
                                    static_cast<float>(m_Config.ChunkVertexSize),
                                    0.0f);
      inst.NeighborLODs = glm::vec4(
         static_cast<float>(chunk.NeighborLODs[DIR_NORTH]),
         static_cast<float>(chunk.NeighborLODs[DIR_EAST]),
         static_cast<float>(chunk.NeighborLODs[DIR_SOUTH]),
         static_cast<float>(chunk.NeighborLODs[DIR_WEST]));
      
      outBatches[lodLevel].Instances.push_back(inst);
      outBatches[lodLevel].LODLevel = lodLevel;
      ++totalVisible;
   }
   
   return totalVisible;
}

uint32_t TerrainChunkSystem::PrepareInstancedBatchesNearCamera(
   const TerrainComponent& terrain,
   float maxCameraDistance,
   std::array<ChunkLODBatch, ChunkSystemConfig::kMaxLODLevels>& outBatches)
{
   for (auto& batch : outBatches)
      batch.Clear();

   const size_t reservePerLOD = (terrain.Chunks.size() + 3) / 4;
   for (auto& batch : outBatches)
      batch.Reserve(reservePerLOD);

   const float distanceLimit = glm::max(0.0f, maxCameraDistance);
   uint32_t totalVisible = 0;

   for (size_t i = 0; i < terrain.Chunks.size(); ++i)
   {
      const TerrainChunk& chunk = terrain.Chunks[i];

      if (!chunk.Visible || chunk.CurrentLOD < 0)
         continue;

      if (chunk.StreamState != ChunkStreamState::Resident)
         continue;

      if (chunk.DistanceToCamera > distanceLimit)
         continue;

      const uint32_t lodLevel = static_cast<uint32_t>(chunk.CurrentLOD);
      if (lodLevel >= ChunkSystemConfig::kMaxLODLevels)
         continue;

      ChunkInstanceData inst;
      inst.ChunkParams = glm::vec4(chunk.UVOffset.x, chunk.UVOffset.y,
                                    chunk.UVScale.x, chunk.UVScale.y);
      inst.ChunkWorld = glm::vec4(chunk.WorldOffset.x, chunk.WorldOffset.y,
                                   chunk.WorldExtent.x, chunk.WorldExtent.y);
      inst.MorphParams = glm::vec4(chunk.MorphFactor,
                                    static_cast<float>(chunk.CurrentLOD),
                                    static_cast<float>(m_Config.ChunkVertexSize),
                                    0.0f);
      inst.NeighborLODs = glm::vec4(
         static_cast<float>(chunk.NeighborLODs[DIR_NORTH]),
         static_cast<float>(chunk.NeighborLODs[DIR_EAST]),
         static_cast<float>(chunk.NeighborLODs[DIR_SOUTH]),
         static_cast<float>(chunk.NeighborLODs[DIR_WEST]));

      outBatches[lodLevel].Instances.push_back(inst);
      outBatches[lodLevel].LODLevel = lodLevel;
      ++totalVisible;
   }

   return totalVisible;
}

bgfx::IndexBufferHandle TerrainChunkSystem::GetLODIndexBuffer(const TerrainComponent& terrain, uint32_t lod) const
{
   if (lod >= ChunkSystemConfig::kMaxLODLevels)
      lod = ChunkSystemConfig::kMaxLODLevels - 1;
   return terrain.ChunkLODIndexBuffers[lod];
}

uint32_t TerrainChunkSystem::GetLODIndexCount(const TerrainComponent& terrain, uint32_t lod) const
{
   if (lod >= ChunkSystemConfig::kMaxLODLevels)
      lod = ChunkSystemConfig::kMaxLODLevels - 1;
   return terrain.ChunkLODIndexCounts[lod];
}

} // namespace terrain
