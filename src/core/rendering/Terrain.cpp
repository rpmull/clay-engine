// CRITICAL: Include Jolt/Jolt.h FIRST to set up Jolt configuration before any standard library headers
// This prevents macro/namespace conflicts between Jolt and standard library headers
#include <Jolt/Jolt.h>

#include "Terrain.h"
#include "core/rendering/TerrainGrass.h"
#include "core/rendering/TerrainClipmaps.h"
#include "core/rendering/TerrainStreaming.h"

#include "core/physics/Physics.h"  // Already includes HeightFieldShape.h
#include "core/physics/PhysicsLayerManager.h"
#include "core/ecs/Components.h"  // Need full definition of TerrainComponent
#include "core/rendering/MaterialCache.h"
#include "core/rendering/TextureLoader.h"
#include "core/rendering/VertexTypes.h"
#include "core/vfs/FileSystem.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <system_error>
#include <unordered_map>
#include <glm/gtc/matrix_inverse.hpp>

namespace fs = std::filesystem;

namespace
{
   // V2 header (for backward compatibility)
   struct TerrainAssetHeaderV2
   {
      char Magic[4];
      uint32_t Version;
      uint32_t GridResolution;
      uint32_t ChunkResolution;
      float WorldSizeX;
      float WorldSizeY;
      float MaxHeight;
      uint32_t HeightCount;
      uint32_t SplatCount;
   };
   
   // V3 header (adds SplatCount2 for 8-layer support)
   struct TerrainAssetHeader : TerrainAssetHeaderV2
   {
      uint32_t SplatCount2 = 0;  // v3: second splatmap for layers 4-7
      uint32_t HoleCount = 0;    // v4: visibility mask, 0=solid, 255=hole
   };

   constexpr char kTerrainAssetMagic[4] = { 'C', 'T', 'R', 'N' };
   constexpr uint32_t kTerrainAssetVersion = 5;
   constexpr uint64_t kTerrainDataTextureFlags =
      BGFX_TEXTURE_COMPUTE_WRITE |
      BGFX_SAMPLER_MIN_POINT |
      BGFX_SAMPLER_MAG_POINT |
      BGFX_SAMPLER_MIP_POINT |
      BGFX_SAMPLER_U_CLAMP |
      BGFX_SAMPLER_V_CLAMP;

   bool UsesSplatInstancerMask(TerrainInstancerMaskSource source)
   {
      switch (source)
      {
      case TerrainInstancerMaskSource::SplatR:
      case TerrainInstancerMaskSource::SplatG:
      case TerrainInstancerMaskSource::SplatB:
      case TerrainInstancerMaskSource::SplatA:
         return true;
      default:
         return false;
      }
   }

   void MarkInstancerRegionsDirty(TerrainComponent& terrain, const glm::ivec2& minCell, const glm::ivec2& maxCell, bool splatOnly)
   {
      if (terrain.GridResolution < 2)
      {
         terrain.MarkInstancersDirty();
         return;
      }

      for (size_t i = 0; i < terrain.InstancerLayers.size(); ++i)
      {
         if (splatOnly && !UsesSplatInstancerMask(terrain.InstancerLayers[i].Mask))
            continue;

         terrain.MarkInstancerLayerRegionDirty(i, minCell, maxCell);
      }
   }

   // Build LOD index buffers for terrain rendering
   // Each LOD level skips vertices to reduce triangle count
   void BuildLODIndexBuffers(TerrainComponent& terrain)
   {
      const uint32_t res = std::max(terrain.GridResolution, 2u);
      const uint32_t vertexCount = res * res;
      const bool use32BitIndices = vertexCount > std::numeric_limits<uint16_t>::max();
      
      // Determine how many LOD levels we can actually support
      // based on the terrain resolution
      terrain.LODLevelCount = 0;
      for (uint32_t lod = 0; lod < TerrainLODConfig::kMaxLODLevels; ++lod)
      {
         const uint32_t step = terrain.LODConfig.LODSteps[lod];
         // We need at least 2 quads in each direction
         if ((res - 1) / step < 2)
            break;
         terrain.LODLevelCount++;
      }
      
      if (terrain.LODLevelCount == 0)
         terrain.LODLevelCount = 1; // Always have at least LOD 0
      
      // Build index buffer for each LOD level
      for (uint32_t lod = 0; lod < terrain.LODLevelCount; ++lod)
      {
         const uint32_t step = terrain.LODConfig.LODSteps[lod];
         const uint32_t quadsX = (res - 1) / step;
         const uint32_t quadsZ = (res - 1) / step;
         const uint32_t indexCount = quadsX * quadsZ * 6;
         
         // Destroy existing buffer if any
         if (bgfx::isValid(terrain.LODIndexBuffers[lod]))
         {
            bgfx::destroy(terrain.LODIndexBuffers[lod]);
            terrain.LODIndexBuffers[lod] = BGFX_INVALID_HANDLE;
         }
         
         terrain.LODIndexCounts[lod] = indexCount;
         
         if (use32BitIndices)
         {
            std::vector<uint32_t> indices(indexCount);
            uint32_t idx = 0;
            for (uint32_t z = 0; z < quadsZ; ++z)
            {
               for (uint32_t x = 0; x < quadsX; ++x)
               {
                  // Calculate actual vertex indices based on step
                  const uint32_t x0 = x * step;
                  const uint32_t z0 = z * step;
                  const uint32_t x1 = x0 + step;
                  const uint32_t z1 = z0 + step;
                  
                  const uint32_t i00 = z0 * res + x0;
                  const uint32_t i10 = z0 * res + x1;
                  const uint32_t i01 = z1 * res + x0;
                  const uint32_t i11 = z1 * res + x1;
                  
                  // Two triangles per quad
                  indices[idx++] = i00;
                  indices[idx++] = i01;
                  indices[idx++] = i10;
                  indices[idx++] = i10;
                  indices[idx++] = i01;
                  indices[idx++] = i11;
               }
            }
            const bgfx::Memory* iMem = bgfx::copy(indices.data(), static_cast<uint32_t>(indices.size() * sizeof(uint32_t)));
            terrain.LODIndexBuffers[lod] = bgfx::createIndexBuffer(iMem, BGFX_BUFFER_INDEX32);
         }
         else
         {
            std::vector<uint16_t> indices(indexCount);
            uint32_t idx = 0;
            for (uint32_t z = 0; z < quadsZ; ++z)
            {
               for (uint32_t x = 0; x < quadsX; ++x)
               {
                  const uint32_t x0 = x * step;
                  const uint32_t z0 = z * step;
                  const uint32_t x1 = x0 + step;
                  const uint32_t z1 = z0 + step;
                  
                  const uint32_t i00 = z0 * res + x0;
                  const uint32_t i10 = z0 * res + x1;
                  const uint32_t i01 = z1 * res + x0;
                  const uint32_t i11 = z1 * res + x1;
                  
                  indices[idx++] = static_cast<uint16_t>(i00);
                  indices[idx++] = static_cast<uint16_t>(i01);
                  indices[idx++] = static_cast<uint16_t>(i10);
                  indices[idx++] = static_cast<uint16_t>(i10);
                  indices[idx++] = static_cast<uint16_t>(i01);
                  indices[idx++] = static_cast<uint16_t>(i11);
               }
            }
            const bgfx::Memory* iMem = bgfx::copy(indices.data(), static_cast<uint32_t>(indices.size() * sizeof(uint16_t)));
            terrain.LODIndexBuffers[lod] = bgfx::createIndexBuffer(iMem);
         }
      }
      
      terrain.LODBuffersDirty = false;
   }
   
   // Compute chunk AABB bounds (for frustum culling)
   void ComputeChunkBounds(const TerrainComponent& terrain, TerrainChunk& chunk)
   {
      const glm::vec2 cellSize = Terrain::GetCellSize(terrain);
      
      // World-space XZ bounds (local coordinates before transform)
      const float minX = static_cast<float>(chunk.Start.x) * cellSize.x;
      const float minZ = static_cast<float>(chunk.Start.y) * cellSize.y;
      const float maxX = minX + static_cast<float>(chunk.VertexCountX - 1) * cellSize.x;
      const float maxZ = minZ + static_cast<float>(chunk.VertexCountZ - 1) * cellSize.y;
      
      // Sample height map to get min/max Y within this chunk
      float minY = std::numeric_limits<float>::max();
      float maxY = std::numeric_limits<float>::lowest();
      
      for (uint32_t z = chunk.Start.y; z < chunk.Start.y + chunk.VertexCountZ; ++z)
      {
         for (uint32_t x = chunk.Start.x; x < chunk.Start.x + chunk.VertexCountX; ++x)
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
      
      if (minY > maxY)
      {
         minY = 0.0f;
         maxY = terrain.MaxHeight;
      }
      
      chunk.WorldMin = glm::vec3(minX, minY, minZ);
      chunk.WorldMax = glm::vec3(maxX, maxY, maxZ);
   }

   float SampleHeightAtIndex(const TerrainComponent& terrain, uint32_t x, uint32_t z)
   {
      if (terrain.HeightMap.empty() || terrain.GridResolution == 0)
         return 0.0f;
      const uint32_t clampedX = std::min(x, terrain.GridResolution - 1);
      const uint32_t clampedZ = std::min(z, terrain.GridResolution - 1);
      const size_t idx = static_cast<size_t>(clampedZ) * terrain.GridResolution + clampedX;
      const float normalized = terrain.HeightMap[idx] / 65535.0f;
      return normalized * terrain.MaxHeight;
   }

   void BuildTerrainMeshBuffers(TerrainComponent& terrain)
   {
      const uint32_t res = std::max(terrain.GridResolution, 2u);
      const glm::vec2 cell = Terrain::GetCellSize(terrain);
      std::vector<TerrainVertex> vertices(res * res);
      for (uint32_t z = 0; z < res; ++z)
      {
         for (uint32_t x = 0; x < res; ++x)
         {
            TerrainVertex& vert = vertices[z * res + x];
            vert.x = static_cast<float>(x) * cell.x;
            vert.y = 0.0f;
            vert.z = static_cast<float>(z) * cell.y;
            vert.nx = 0.0f;
            vert.ny = 1.0f;
            vert.nz = 0.0f;
            const float denom = (res > 1) ? static_cast<float>(res - 1) : 1.0f;
            vert.u = static_cast<float>(x) / denom;
            vert.v = static_cast<float>(z) / denom;
         }
      }

      const uint32_t indexCount = (res - 1) * (res - 1) * 6;
      const uint32_t vertexCount = res * res;
      const bool use32BitIndices = vertexCount > std::numeric_limits<uint16_t>::max();

      auto buildIndices16 = [&](std::vector<uint16_t>& indices)
      {
         uint32_t idx = 0;
         for (uint32_t z = 0; z < res - 1; ++z)
         {
            for (uint32_t x = 0; x < res - 1; ++x)
            {
               const uint32_t base = z * res + x;
               indices[idx++] = static_cast<uint16_t>(base);
               indices[idx++] = static_cast<uint16_t>(base + res);
               indices[idx++] = static_cast<uint16_t>(base + 1);
               indices[idx++] = static_cast<uint16_t>(base + 1);
               indices[idx++] = static_cast<uint16_t>(base + res);
               indices[idx++] = static_cast<uint16_t>(base + res + 1);
            }
         }
      };

      auto buildIndices32 = [&](std::vector<uint32_t>& indices)
      {
         uint32_t idx = 0;
         for (uint32_t z = 0; z < res - 1; ++z)
         {
            for (uint32_t x = 0; x < res - 1; ++x)
            {
               const uint32_t base = z * res + x;
               indices[idx++] = base;
               indices[idx++] = base + res;
               indices[idx++] = base + 1;
               indices[idx++] = base + 1;
               indices[idx++] = base + res;
               indices[idx++] = base + res + 1;
            }
         }
      };

      if (bgfx::isValid(terrain.ChunkVB))
         bgfx::destroy(terrain.ChunkVB);
      if (bgfx::isValid(terrain.ChunkIB))
         bgfx::destroy(terrain.ChunkIB);

      const bgfx::Memory* vMem = bgfx::copy(vertices.data(), static_cast<uint32_t>(vertices.size() * sizeof(TerrainVertex)));
      terrain.ChunkVB = bgfx::createVertexBuffer(vMem, TerrainVertex::layout);

      if (use32BitIndices)
      {
         std::vector<uint32_t> indices(indexCount);
         buildIndices32(indices);
         const bgfx::Memory* iMem = bgfx::copy(indices.data(), static_cast<uint32_t>(indices.size() * sizeof(uint32_t)));
         terrain.ChunkIB = bgfx::createIndexBuffer(iMem, BGFX_BUFFER_INDEX32);
      }
      else
      {
         std::vector<uint16_t> indices(indexCount);
         buildIndices16(indices);
         const bgfx::Memory* iMem = bgfx::copy(indices.data(), static_cast<uint32_t>(indices.size() * sizeof(uint16_t)));
         terrain.ChunkIB = bgfx::createIndexBuffer(iMem);
      }
      terrain.MeshDirty = false;
      
      // Update chunk bounds since cell size may have changed (e.g. WorldSize change)
      for (TerrainChunk& chunk : terrain.Chunks)
      {
         ComputeChunkBounds(terrain, chunk);
      }
   }

   void UploadHeightTexture(const TerrainComponent& terrain, TerrainChunk& chunk)
   {
      // For chunked terrain, use full terrain resolution (unified heightmap)
      // For legacy single-chunk, use chunk's vertex counts
      const uint32_t width = terrain.UseChunkedTerrain 
         ? terrain.GridResolution 
         : std::max(1u, chunk.VertexCountX);
      const uint32_t height = terrain.UseChunkedTerrain 
         ? terrain.GridResolution 
         : std::max(1u, chunk.VertexCountZ);

      std::vector<float> buffer(width * height);
      const float invMax = 1.0f / 65535.0f;
      bool anyNonZero = false;
      
      // For chunked terrain, copy entire heightmap
      // For legacy, copy only this chunk's region
      const uint32_t startX = terrain.UseChunkedTerrain ? 0 : chunk.Start.x;
      const uint32_t startZ = terrain.UseChunkedTerrain ? 0 : chunk.Start.y;
      
      for (uint32_t z = 0; z < height; ++z)
      {
         const uint32_t srcZ = startZ + z;
         for (uint32_t x = 0; x < width; ++x)
         {
            const uint32_t srcX = startX + x;
            const size_t srcIdx = static_cast<size_t>(srcZ) * terrain.GridResolution + srcX;
            const uint16_t h = terrain.HeightMap[srcIdx];
            if (h != 0)
            {
               anyNonZero = true;
            }
            buffer[z * width + x] = static_cast<float>(h) * invMax;
         }
      }

      const bgfx::Memory* mem = bgfx::copy(buffer.data(), static_cast<uint32_t>(buffer.size() * sizeof(float)));
      
      // Always recreate the texture to ensure updates are applied immediately.
      // bgfx::updateTexture2D can sometimes fail to update textures properly.
      if (bgfx::isValid(chunk.HeightTexture))
      {
         bgfx::destroy(chunk.HeightTexture);
      }
      
      // Store as a single-channel 32-bit float texture; this matches the CPU
      // buffer element size (float) and avoids format/data-size mismatches.
      chunk.HeightTexture = bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::R32F, kTerrainDataTextureFlags, mem);
      chunk.HeightDirty = false;
   }

   void UploadSplatTexture(const TerrainComponent& terrain, TerrainChunk& chunk)
   {
      // For chunked terrain, use full terrain resolution (unified splatmap)
      // For legacy single-chunk, use chunk's vertex counts
      const uint32_t width = terrain.UseChunkedTerrain 
         ? terrain.GridResolution 
         : std::max(1u, chunk.VertexCountX);
      const uint32_t height = terrain.UseChunkedTerrain 
         ? terrain.GridResolution 
         : std::max(1u, chunk.VertexCountZ);
      
      // For chunked terrain, copy entire splatmap; for legacy, copy only this chunk's region
      const uint32_t startX = terrain.UseChunkedTerrain ? 0 : chunk.Start.x;
      const uint32_t startZ = terrain.UseChunkedTerrain ? 0 : chunk.Start.y;
      
      std::vector<glm::u8vec4> buffer(width * height);
      
      for (uint32_t z = 0; z < height; ++z)
      {
         const uint32_t srcZ = startZ + z;
         for (uint32_t x = 0; x < width; ++x)
         {
            const uint32_t srcX = startX + x;
            const size_t srcIdx = static_cast<size_t>(srcZ) * terrain.GridResolution + srcX;
            buffer[z * width + x] = terrain.SplatMap[srcIdx];
         }
      }
      
      const bgfx::Memory* mem = bgfx::copy(buffer.data(), static_cast<uint32_t>(buffer.size() * sizeof(glm::u8vec4)));
      
      // Always recreate the texture to ensure updates are applied immediately.
      if (bgfx::isValid(chunk.SplatTexture))
      {
         bgfx::destroy(chunk.SplatTexture);
      }
      
      const uint64_t splatFlags =
         BGFX_TEXTURE_COMPUTE_WRITE |
         BGFX_SAMPLER_U_CLAMP |
         BGFX_SAMPLER_V_CLAMP;
      chunk.SplatTexture = bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::RGBA8, splatFlags, mem);
      
      // Upload second splatmap for 8-layer support
      if (!terrain.SplatMap2.empty())
      {
         std::vector<glm::u8vec4> buffer2(width * height);
         for (uint32_t z = 0; z < height; ++z)
         {
            const uint32_t srcZ = startZ + z;
            for (uint32_t x = 0; x < width; ++x)
            {
               const uint32_t srcX = startX + x;
               const size_t srcIdx = static_cast<size_t>(srcZ) * terrain.GridResolution + srcX;
               buffer2[z * width + x] = terrain.SplatMap2[srcIdx];
            }
         }
         
         const bgfx::Memory* mem2 = bgfx::copy(buffer2.data(), static_cast<uint32_t>(buffer2.size() * sizeof(glm::u8vec4)));
         if (bgfx::isValid(chunk.SplatTexture2))
         {
            bgfx::destroy(chunk.SplatTexture2);
         }
         chunk.SplatTexture2 = bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::RGBA8, splatFlags, mem2);
      }
      
      chunk.SplatDirty = false;
   }

   void UploadHoleTexture(const TerrainComponent& terrain, TerrainChunk& chunk)
   {
      const uint32_t width = terrain.UseChunkedTerrain
         ? terrain.GridResolution
         : std::max(1u, chunk.VertexCountX);
      const uint32_t height = terrain.UseChunkedTerrain
         ? terrain.GridResolution
         : std::max(1u, chunk.VertexCountZ);

      const uint32_t startX = terrain.UseChunkedTerrain ? 0 : chunk.Start.x;
      const uint32_t startZ = terrain.UseChunkedTerrain ? 0 : chunk.Start.y;

      std::vector<uint8_t> buffer(width * height, 0);
      for (uint32_t z = 0; z < height; ++z)
      {
         const uint32_t srcZ = startZ + z;
         for (uint32_t x = 0; x < width; ++x)
         {
            const uint32_t srcX = startX + x;
            const size_t srcIdx = static_cast<size_t>(srcZ) * terrain.GridResolution + srcX;
            if (srcIdx < terrain.HoleMask.size())
            {
               buffer[z * width + x] = terrain.HoleMask[srcIdx];
            }
         }
      }

      const bgfx::Memory* mem = bgfx::copy(buffer.data(), static_cast<uint32_t>(buffer.size() * sizeof(uint8_t)));
      if (bgfx::isValid(chunk.HoleTexture))
      {
         bgfx::destroy(chunk.HoleTexture);
      }

      chunk.HoleTexture = bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::R8, kTerrainDataTextureFlags, mem);
      chunk.HoleDirty = false;
   }

   void SyncLayerHandles(TerrainLayerDesc& layer)
   {
      if (layer.AlbedoPath.empty())
      {
         layer.AlbedoHandle = BGFX_INVALID_HANDLE;
      }
      else
      {
         TextureSpecifier spec;
         spec.Path = layer.AlbedoPath;
         layer.AlbedoHandle = AcquireTextureHandle(spec, TextureColorSpace::Linear);
      }

      if (layer.NormalPath.empty())
      {
         layer.NormalHandle = BGFX_INVALID_HANDLE;
      }
      else
      {
         TextureSpecifier spec;
         spec.Path = layer.NormalPath;
         layer.NormalHandle = AcquireTextureHandle(spec, TextureColorSpace::Linear);
      }
   }

   bool RayAabb(const glm::vec3& origin, const glm::vec3& dir, const glm::vec3& minB, const glm::vec3& maxB, float& tEnter, float& tExit)
   {
      float t0 = 0.0f;
      float t1 = std::numeric_limits<float>::max();
      for (int i = 0; i < 3; ++i)
      {
         float o = origin[i];
         float d = dir[i];
         float minPlane = minB[i];
         float maxPlane = maxB[i];
         if (std::abs(d) < 1e-6f)
         {
            if (o < minPlane || o > maxPlane)
               return false;
            continue;
         }
         float inv = 1.0f / d;
         float tNear = (minPlane - o) * inv;
         float tFar = (maxPlane - o) * inv;
         if (tNear > tFar) std::swap(tNear, tFar);
         t0 = glm::max(t0, tNear);
         t1 = glm::min(t1, tFar);
         if (t0 > t1) return false;
      }
      tEnter = t0;
      tExit = t1;
      return true;
   }
}

void Terrain::EnsureChunkLayout(TerrainComponent& terrain)
{
   terrain.GridResolution = std::max(terrain.GridResolution, 2u);
   terrain.EnsureMapSize();

   // When using chunked terrain, the TerrainChunkSystem manages chunk layout.
   // Don't interfere with its multi-chunk setup.
   if (terrain.UseChunkedTerrain)
   {
      // Chunked terrain: chunk layout is managed by TerrainChunkSystem in Renderer.
      // Just ensure we have at least one chunk for the unified texture storage.
      if (terrain.Chunks.empty())
      {
         TerrainChunk chunk;
         chunk.Start = glm::ivec2(0);
         chunk.VertexCountX = terrain.GridResolution;
         chunk.VertexCountZ = terrain.GridResolution;
         chunk.DirtyMin = glm::ivec2(0);
         chunk.DirtyMax = glm::ivec2(static_cast<int>(terrain.GridResolution) - 1);
         chunk.HeightDirty = true;
         chunk.SplatDirty = true;
         chunk.HoleDirty = true;
         chunk.CurrentLOD = 0;
         chunk.Visible = true;
         ComputeChunkBounds(terrain, chunk);
         terrain.Chunks.push_back(chunk);
         terrain.HeightDataDirty = true;
         terrain.SplatDataDirty = true;
         terrain.HoleDataDirty = true;
      }
      return;
   }

   // Legacy single-chunk mode: use one chunk covering entire terrain
   const bool needsRebuild = terrain.Chunks.empty() ||
      terrain.Chunks[0].VertexCountX != terrain.GridResolution ||
      terrain.Chunks[0].VertexCountZ != terrain.GridResolution;

   if (needsRebuild)
   {
      terrain.DestroyGpuResources();
      terrain.ChunksX = 1;
      terrain.ChunksZ = 1;
      
      TerrainChunk chunk;
      chunk.Start = glm::ivec2(0);
      chunk.VertexCountX = terrain.GridResolution;
      chunk.VertexCountZ = terrain.GridResolution;
      chunk.DirtyMin = glm::ivec2(0);
      chunk.DirtyMax = glm::ivec2(static_cast<int>(terrain.GridResolution) - 1);
      chunk.HeightDirty = true;
      chunk.SplatDirty = true;
      chunk.HoleDirty = true;
      chunk.CurrentLOD = 0;
      chunk.Visible = true;
      
      // Compute bounds for the entire terrain
      ComputeChunkBounds(terrain, chunk);
      
      terrain.Chunks.clear();
      terrain.Chunks.push_back(chunk);
      terrain.MeshDirty = true;
      terrain.HeightDataDirty = true;
      terrain.SplatDataDirty = true;
      terrain.HoleDataDirty = true;
      terrain.LODBuffersDirty = true;
   }
}

glm::vec2 Terrain::GetCellSize(const TerrainComponent& terrain)
{
   const uint32_t denom = std::max(1u, terrain.GridResolution - 1u);
   if (denom == 0)
      return glm::vec2(0.0f);
   return glm::vec2(
      denom ? terrain.WorldSize.x / static_cast<float>(denom) : 0.0f,
      denom ? terrain.WorldSize.y / static_cast<float>(denom) : 0.0f);
}

float Terrain::SampleHeightWorld(const TerrainComponent& terrain, float localX, float localZ)
{
   if (terrain.HeightMap.empty() || terrain.GridResolution < 2)
      return 0.0f;

   const glm::vec2 cell = GetCellSize(terrain);
   if (cell.x <= 0.0f || cell.y <= 0.0f)
      return 0.0f;

   const float maxCoord = static_cast<float>(terrain.GridResolution - 1);
   const float xf = glm::clamp(localX / cell.x, 0.0f, maxCoord);
   const float zf = glm::clamp(localZ / cell.y, 0.0f, maxCoord);

   const uint32_t x0 = static_cast<uint32_t>(std::floor(xf));
   const uint32_t z0 = static_cast<uint32_t>(std::floor(zf));
   const uint32_t x1 = std::min(x0 + 1, terrain.GridResolution - 1);
   const uint32_t z1 = std::min(z0 + 1, terrain.GridResolution - 1);
   const float tx = xf - static_cast<float>(x0);
   const float tz = zf - static_cast<float>(z0);

   const float h00 = SampleHeightAtIndex(terrain, x0, z0);
   const float h10 = SampleHeightAtIndex(terrain, x1, z0);
   const float h01 = SampleHeightAtIndex(terrain, x0, z1);
   const float h11 = SampleHeightAtIndex(terrain, x1, z1);

   const float hx0 = glm::mix(h00, h10, tx);
   const float hx1 = glm::mix(h01, h11, tx);
   return glm::mix(hx0, hx1, tz);
}

glm::vec3 Terrain::SampleNormal(const TerrainComponent& terrain, float localX, float localZ)
{
   if (terrain.HeightMap.empty() || terrain.GridResolution < 2)
      return glm::vec3(0.0f, 1.0f, 0.0f);

   const glm::vec2 cell = GetCellSize(terrain);
   if (cell.x <= 0.0f || cell.y <= 0.0f)
      return glm::vec3(0.0f, 1.0f, 0.0f);

   const float maxCoord = static_cast<float>(terrain.GridResolution - 1);
   const float xf = glm::clamp(localX / cell.x, 0.0f, maxCoord);
   const float zf = glm::clamp(localZ / cell.y, 0.0f, maxCoord);
   const uint32_t xi = static_cast<uint32_t>(glm::clamp(std::round(xf), 0.0f, maxCoord));
   const uint32_t zi = static_cast<uint32_t>(glm::clamp(std::round(zf), 0.0f, maxCoord));

   const uint32_t left = xi > 0 ? xi - 1 : xi;
   const uint32_t right = std::min(xi + 1, terrain.GridResolution - 1);
   const uint32_t down = zi > 0 ? zi - 1 : zi;
   const uint32_t up = std::min(zi + 1, terrain.GridResolution - 1);

   const float hL = SampleHeightAtIndex(terrain, left, zi);
   const float hR = SampleHeightAtIndex(terrain, right, zi);
   const float hD = SampleHeightAtIndex(terrain, xi, down);
   const float hU = SampleHeightAtIndex(terrain, xi, up);

   glm::vec3 normal(
      (hL - hR) / (2.0f * cell.x),
      1.0f,
      (hD - hU) / (2.0f * cell.y));
   return glm::normalize(normal);
}

float Terrain::SampleHoleMaskNormalized(const TerrainComponent& terrain, float localX, float localZ)
{
   if (terrain.HoleMask.empty() || terrain.GridResolution < 2)
      return 0.0f;

   const glm::vec2 cell = GetCellSize(terrain);
   if (cell.x <= 0.0f || cell.y <= 0.0f)
      return 0.0f;

   const float maxCoord = static_cast<float>(terrain.GridResolution - 1);
   const float xf = glm::clamp(localX / cell.x, 0.0f, maxCoord);
   const float zf = glm::clamp(localZ / cell.y, 0.0f, maxCoord);

   const uint32_t x0 = static_cast<uint32_t>(std::floor(xf));
   const uint32_t z0 = static_cast<uint32_t>(std::floor(zf));
   const uint32_t x1 = std::min(x0 + 1, terrain.GridResolution - 1);
   const uint32_t z1 = std::min(z0 + 1, terrain.GridResolution - 1);
   const float tx = xf - static_cast<float>(x0);
   const float tz = zf - static_cast<float>(z0);

   const size_t idx00 = static_cast<size_t>(z0) * terrain.GridResolution + x0;
   const size_t idx10 = static_cast<size_t>(z0) * terrain.GridResolution + x1;
   const size_t idx01 = static_cast<size_t>(z1) * terrain.GridResolution + x0;
   const size_t idx11 = static_cast<size_t>(z1) * terrain.GridResolution + x1;

   const float m00 = terrain.HoleMask[idx00] / 255.0f;
   const float m10 = terrain.HoleMask[idx10] / 255.0f;
   const float m01 = terrain.HoleMask[idx01] / 255.0f;
   const float m11 = terrain.HoleMask[idx11] / 255.0f;

   const float mx0 = glm::mix(m00, m10, tx);
   const float mx1 = glm::mix(m01, m11, tx);
   return glm::mix(mx0, mx1, tz);
}

bool Terrain::IsHoleAtLocal(const TerrainComponent& terrain, float localX, float localZ, float threshold)
{
   return SampleHoleMaskNormalized(terrain, localX, localZ) >= threshold;
}

bool Terrain::Raycast(const TransformComponent& transform,
                      TerrainComponent& terrain,
                      const glm::vec3& rayOriginWorld,
                      const glm::vec3& rayDirWorld,
                      glm::vec3* outWorldPos,
                      glm::vec3* outWorldNormal,
                      glm::vec3* outLocalPos,
                      glm::vec3* outLocalNormal,
                      bool ignoreHoles)
{
   if (terrain.HeightMap.empty() || terrain.GridResolution < 2)
      return false;

   glm::mat4 invTransform = glm::inverse(transform.WorldMatrix);
   glm::vec3 originLocal = glm::vec3(invTransform * glm::vec4(rayOriginWorld, 1.0f));
   glm::vec3 dirLocal = glm::normalize(glm::vec3(invTransform * glm::vec4(rayDirWorld, 0.0f)));

   glm::vec3 aabbMin(0.0f, 0.0f, 0.0f);
   glm::vec3 aabbMax(terrain.WorldSize.x, terrain.MaxHeight, terrain.WorldSize.y);

   float tEnter = 0.0f;
   float tExit = 0.0f;
   if (!RayAabb(originLocal, dirLocal, aabbMin, aabbMax, tEnter, tExit))
      return false;

   glm::vec2 cellSize = Terrain::GetCellSize(terrain);
   float step = glm::max(0.1f, glm::min(cellSize.x, cellSize.y) * 0.5f);
   float t = glm::max(0.0f, tEnter);

   auto trySampleSignedDistance = [&](float sampleT, glm::vec3& outPos,
                                      float& outSignedDistance) -> bool
   {
      outPos = originLocal + dirLocal * sampleT;
      if (outPos.x < 0.0f || outPos.x > terrain.WorldSize.x ||
          outPos.z < 0.0f || outPos.z > terrain.WorldSize.y)
      {
         return false;
      }
      if (!ignoreHoles && Terrain::IsHoleAtLocal(terrain, outPos.x, outPos.z))
      {
         return false;
      }

      const float height = Terrain::SampleHeightWorld(terrain, outPos.x,
         outPos.z);
      outSignedDistance = outPos.y - height;
      return true;
   };

   auto writeHit = [&](float hitT) -> bool
   {
      glm::vec3 pos = originLocal + dirLocal * hitT;
      pos.x = glm::clamp(pos.x, 0.0f, terrain.WorldSize.x);
      pos.z = glm::clamp(pos.z, 0.0f, terrain.WorldSize.y);
      if (!ignoreHoles && Terrain::IsHoleAtLocal(terrain, pos.x, pos.z))
      {
         return false;
      }

      const float height = Terrain::SampleHeightWorld(terrain, pos.x, pos.z);
      glm::vec3 localPos = glm::vec3(pos.x, height, pos.z);
      glm::vec3 localNormal = Terrain::SampleNormal(terrain, pos.x, pos.z);
      if (outLocalPos) *outLocalPos = localPos;
      if (outLocalNormal) *outLocalNormal = localNormal;
      if (outWorldPos || outWorldNormal)
      {
         glm::vec4 worldPos4 = transform.WorldMatrix * glm::vec4(localPos, 1.0f);
         if (outWorldPos) *outWorldPos = glm::vec3(worldPos4);
         if (outWorldNormal)
         {
            glm::mat3 normalMatrix = glm::mat3(glm::transpose(glm::inverse(glm::mat3(transform.WorldMatrix))));
            *outWorldNormal = glm::normalize(normalMatrix * localNormal);
         }
      }
      return true;
   };

   bool hasPreviousSample = false;
   float previousT = t;
   float previousSignedDistance = 0.0f;

   for (int i = 0; i < 4096 && t <= tExit + step; ++i)
   {
      glm::vec3 pos(0.0f);
      float signedDistance = 0.0f;
      if (!trySampleSignedDistance(t, pos, signedDistance))
      {
         t += step;
         continue;
      }

      if (signedDistance <= 0.0f)
      {
         float hitT = t;
         if (hasPreviousSample && previousSignedDistance > 0.0f)
         {
            float lo = previousT;
            float hi = t;
            for (int refine = 0; refine < 12; ++refine)
            {
               const float mid = (lo + hi) * 0.5f;
               glm::vec3 midPos(0.0f);
               float midSignedDistance = 0.0f;
               if (!trySampleSignedDistance(mid, midPos, midSignedDistance))
               {
                  lo = mid;
                  continue;
               }

               if (midSignedDistance > 0.0f)
               {
                  lo = mid;
               }
               else
               {
                  hi = mid;
               }
            }
            hitT = hi;
         }

         if (writeHit(hitT))
         {
            return true;
         }
      }

      hasPreviousSample = true;
      previousT = t;
      previousSignedDistance = signedDistance;
      t += step;
   }
   return false;
}

void Terrain::MarkHeightRegionDirty(TerrainComponent& terrain, const glm::ivec2& minCell, const glm::ivec2& maxCell)
{
   MarkInstancerRegionsDirty(terrain, minCell, maxCell, false);
   if (terrain.Chunks.empty())
      return;

   const int maxIndex = static_cast<int>(terrain.GridResolution) - 1;
   const glm::ivec2 clampMin(0);
   const glm::ivec2 clampMax(maxIndex);
   glm::ivec2 minClamped = glm::clamp(minCell, clampMin, clampMax);
   glm::ivec2 maxClamped = glm::clamp(maxCell, clampMin, clampMax);

   TerrainChunk& chunk = terrain.Chunks[0];
   if (!chunk.HeightDirty)
   {
       chunk.DirtyMin = minClamped;
       chunk.DirtyMax = maxClamped;
   }
   else
   {
       chunk.DirtyMin = glm::min(chunk.DirtyMin, minClamped);
       chunk.DirtyMax = glm::max(chunk.DirtyMax, maxClamped);
   }
   chunk.HeightDirty = true;
   
   // Update chunk bounds for LOD
   ComputeChunkBounds(terrain, chunk);
   
   terrain.HeightDataDirty = true;
   terrain.AssetDirty = true;
   terrain.GrassStructureDirty = true;
}

void Terrain::MarkSplatRegionDirty(TerrainComponent& terrain, const glm::ivec2& minCell, const glm::ivec2& maxCell)
{
   MarkInstancerRegionsDirty(terrain, minCell, maxCell, true);
   if (terrain.Chunks.empty())
      return;

   const int maxIndex = static_cast<int>(terrain.GridResolution) - 1;
   const glm::ivec2 clampMin(0);
   const glm::ivec2 clampMax(maxIndex);
   glm::ivec2 minClamped = glm::clamp(minCell, clampMin, clampMax);
   glm::ivec2 maxClamped = glm::clamp(maxCell, clampMin, clampMax);

   TerrainChunk& chunk = terrain.Chunks[0];
   if (!chunk.SplatDirty)
   {
       chunk.DirtyMin = minClamped;
       chunk.DirtyMax = maxClamped;
   }
   else
   {
       chunk.DirtyMin = glm::min(chunk.DirtyMin, minClamped);
       chunk.DirtyMax = glm::max(chunk.DirtyMax, maxClamped);
   }
   chunk.SplatDirty = true;
   terrain.SplatDataDirty = true;
   terrain.AssetDirty = true;
   terrain.GrassMasksDirty = true;
}

void Terrain::MarkHoleRegionDirty(TerrainComponent& terrain, const glm::ivec2& minCell, const glm::ivec2& maxCell)
{
   MarkInstancerRegionsDirty(terrain, minCell, maxCell, false);
   if (terrain.Chunks.empty())
      return;

   const int maxIndex = static_cast<int>(terrain.GridResolution) - 1;
   const glm::ivec2 clampMin(0);
   const glm::ivec2 clampMax(maxIndex);
   glm::ivec2 minClamped = glm::clamp(minCell, clampMin, clampMax);
   glm::ivec2 maxClamped = glm::clamp(maxCell, clampMin, clampMax);

   TerrainChunk& chunk = terrain.Chunks[0];
   if (!chunk.HoleDirty)
   {
      chunk.DirtyMin = minClamped;
      chunk.DirtyMax = maxClamped;
   }
   else
   {
      chunk.DirtyMin = glm::min(chunk.DirtyMin, minClamped);
      chunk.DirtyMax = glm::max(chunk.DirtyMax, maxClamped);
   }

   chunk.HoleDirty = true;
   terrain.HoleDataDirty = true;
   terrain.PhysicsDirty = true;
   terrain.AssetDirty = true;
   terrain.GrassStructureDirty = true;
   terrain.GrassMasksDirty = true;
}

void Terrain::PrepareForRendering(TerrainComponent& terrain)
{
   EnsureChunkLayout(terrain);

   if (terrain.MeshDirty || !bgfx::isValid(terrain.ChunkVB) || !bgfx::isValid(terrain.ChunkIB))
   {
      BuildTerrainMeshBuffers(terrain);
   }

   for (TerrainLayerDesc& layer : terrain.Layers)
   {
      SyncLayerHandles(layer);
   }

   // When using chunked terrain, all chunks share a SINGLE unified heightmap/splatmap
   // stored in chunk 0. Only upload textures for chunk 0.
   // Non-chunked terrain (legacy path) may have multiple chunks with individual textures.
   if (terrain.UseChunkedTerrain && terrain.Chunks.size() > 0)
   {
      // Chunked terrain: only chunk 0 has the unified textures
      TerrainChunk& chunk0 = terrain.Chunks[0];
      
      if (!bgfx::isValid(chunk0.HeightTexture))
      {
         chunk0.HeightDirty = true;
      }
      if (!bgfx::isValid(chunk0.SplatTexture))
      {
         chunk0.SplatDirty = true;
      }
      if (!bgfx::isValid(chunk0.HoleTexture))
      {
         chunk0.HoleDirty = true;
      }
      
      if (terrain.HeightDataDirty || chunk0.HeightDirty)
      {
         // Upload unified heightmap covering entire terrain
         UploadHeightTexture(terrain, chunk0);
      }
      if (terrain.SplatDataDirty || chunk0.SplatDirty)
      {
         // Upload unified splatmap covering entire terrain
         UploadSplatTexture(terrain, chunk0);
      }
      if (terrain.HoleDataDirty || chunk0.HoleDirty)
      {
         UploadHoleTexture(terrain, chunk0);
      }
   }
   else
   {
      // Legacy path: each chunk has its own textures
      for (TerrainChunk& chunk : terrain.Chunks)
      {
         if (!bgfx::isValid(chunk.HeightTexture))
         {
            chunk.HeightDirty = true;
         }
         if (!bgfx::isValid(chunk.SplatTexture))
         {
            chunk.SplatDirty = true;
         }
         if (!bgfx::isValid(chunk.HoleTexture))
         {
            chunk.HoleDirty = true;
         }

         if (terrain.HeightDataDirty || chunk.HeightDirty)
         {
            UploadHeightTexture(terrain, chunk);
         }
         if (terrain.SplatDataDirty || chunk.SplatDirty)
         {
            UploadSplatTexture(terrain, chunk);
         }
         if (terrain.HoleDataDirty || chunk.HoleDirty)
         {
            UploadHoleTexture(terrain, chunk);
         }
      }
   }

   terrain.HeightDataDirty = false;
   terrain.SplatDataDirty = false;
   terrain.HoleDataDirty = false;
   
   // Build LOD index buffers if needed
   if (terrain.LODConfig.Enabled && terrain.LODBuffersDirty)
   {
      BuildLODIndexBuffers(terrain);
   }
   
   TerrainGrass::EnsureChunksUpToDate(terrain);
}

// Update chunk bounds after height changes (call from MarkHeightRegionDirty)
void Terrain::UpdateChunkBoundsFromHeightChange(TerrainComponent& terrain)
{
   for (TerrainChunk& chunk : terrain.Chunks)
   {
      ComputeChunkBounds(terrain, chunk);
   }
}

// Update per-frame LOD selection and frustum culling for all terrain chunks
void Terrain::UpdateLOD(TerrainComponent& terrain, const glm::vec3& cameraPos, const glm::mat4& worldMatrix)
{
   if (!terrain.LODConfig.Enabled || terrain.Chunks.empty())
      return;
   
   for (TerrainChunk& chunk : terrain.Chunks)
   {
      // Transform chunk center to world space for distance calculation
      const glm::vec3 localCenter = (chunk.WorldMin + chunk.WorldMax) * 0.5f;
      const glm::vec4 worldCenterH = worldMatrix * glm::vec4(localCenter, 1.0f);
      const glm::vec3 worldCenter = glm::vec3(worldCenterH);
      
      // Calculate distance to camera
      chunk.DistanceToCamera = glm::length(cameraPos - worldCenter);
      
      // Select LOD based on distance
      chunk.CurrentLOD = 0;
      for (uint32_t lod = 0; lod < terrain.LODLevelCount; ++lod)
      {
         if (chunk.DistanceToCamera < terrain.LODConfig.LODDistances[lod])
         {
            chunk.CurrentLOD = static_cast<int>(lod);
            break;
         }
         chunk.CurrentLOD = static_cast<int>(lod);
      }
      
      // Note: Frustum culling is handled by the Renderer using its own infrastructure
      chunk.Visible = true;
   }
}

bool Terrain::SaveTerrainAsset(TerrainComponent& terrain, bool forceWrite)
{
   if (terrain.HeightMap.empty() || terrain.SplatMap.empty())
      return false;

   if (!terrain.AssetDirty && !forceWrite)
      return true;

   if (terrain.AssetPath.empty())
   {
      terrain.AssetPath = TerrainComponent::BuildDefaultAssetPath(terrain.TerrainDataGuid);
   }

   std::string normalizedPath = terrain.AssetPath;
   try
   {
      normalizedPath = IVirtualFS::NormalizePath(terrain.AssetPath);
   }
   catch (...) {}
   terrain.AssetPath = normalizedPath;

   // Resolve path relative to project root
   fs::path diskPath(normalizedPath);
   if (diskPath.is_relative())
   {
      const fs::path& projectRoot = FileSystem::Instance().GetProjectRoot();
      if (!projectRoot.empty())
      {
         diskPath = projectRoot / diskPath;
      }
   }
   
   if (diskPath.has_parent_path())
   {
      std::error_code ec;
      fs::create_directories(diskPath.parent_path(), ec);
   }

   std::ofstream stream(diskPath, std::ios::binary | std::ios::trunc);
   if (!stream.is_open())
   {
      std::cerr << "[Terrain] Failed to open terrain asset for writing: " << diskPath.string() << std::endl;
      return false;
   }

   TerrainAssetHeader header{};
   std::memcpy(header.Magic, kTerrainAssetMagic, sizeof(header.Magic));
   header.Version = kTerrainAssetVersion;
   header.GridResolution = terrain.GridResolution;
   header.ChunkResolution = terrain.ChunkResolution;
   header.WorldSizeX = terrain.WorldSize.x;
   header.WorldSizeY = terrain.WorldSize.y;
   header.MaxHeight = terrain.MaxHeight;
   header.HeightCount = static_cast<uint32_t>(terrain.HeightMap.size());
   header.SplatCount = static_cast<uint32_t>(terrain.SplatMap.size());
   header.SplatCount2 = static_cast<uint32_t>(terrain.SplatMap2.size());
   header.HoleCount = static_cast<uint32_t>(terrain.HoleMask.size());

   stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
   stream.write(reinterpret_cast<const char*>(terrain.HeightMap.data()), header.HeightCount * sizeof(uint16_t));
   stream.write(reinterpret_cast<const char*>(terrain.SplatMap.data()), header.SplatCount * sizeof(glm::u8vec4));
   if (header.SplatCount2 > 0)
   {
      stream.write(reinterpret_cast<const char*>(terrain.SplatMap2.data()), header.SplatCount2 * sizeof(glm::u8vec4));
   }
   if (header.HoleCount > 0)
   {
      stream.write(reinterpret_cast<const char*>(terrain.HoleMask.data()), header.HoleCount * sizeof(uint8_t));
   }

   const uint32_t grassLayerCount = static_cast<uint32_t>(terrain.GrassLayers.size());
   stream.write(reinterpret_cast<const char*>(&grassLayerCount), sizeof(uint32_t));
   for (const TerrainGrassLayerDesc& layer : terrain.GrassLayers)
   {
      stream.write(reinterpret_cast<const char*>(&layer.Guid.high), sizeof(uint64_t));
      stream.write(reinterpret_cast<const char*>(&layer.Guid.low), sizeof(uint64_t));
      const uint32_t maskSize = static_cast<uint32_t>(layer.PaintedMask.size());
      stream.write(reinterpret_cast<const char*>(&maskSize), sizeof(uint32_t));
      if (maskSize > 0)
      {
         stream.write(reinterpret_cast<const char*>(layer.PaintedMask.data()), maskSize * sizeof(uint8_t));
      }
   }

   const uint32_t instancerLayerCount = static_cast<uint32_t>(terrain.InstancerLayers.size());
   stream.write(reinterpret_cast<const char*>(&instancerLayerCount), sizeof(uint32_t));
   for (const TerrainInstancerLayerDesc& layer : terrain.InstancerLayers)
   {
      stream.write(reinterpret_cast<const char*>(&layer.Guid.high), sizeof(uint64_t));
      stream.write(reinterpret_cast<const char*>(&layer.Guid.low), sizeof(uint64_t));
      const uint32_t maskSize = static_cast<uint32_t>(layer.PaintedMask.size());
      stream.write(reinterpret_cast<const char*>(&maskSize), sizeof(uint32_t));
      if (maskSize > 0)
      {
         stream.write(reinterpret_cast<const char*>(layer.PaintedMask.data()), maskSize * sizeof(uint8_t));
      }
   }

   if (!stream.good())
   {
      std::cerr << "[Terrain] Failed while writing terrain asset: " << diskPath.string() << std::endl;
      return false;
   }

   stream.close();
   terrain.AssetDirty = false;
   return true;
}

// Use stb_image_write for heightmap export (implementation defined elsewhere)
#ifndef CLAYMORE_RUNTIME
#include <stb_image_write.h>
#endif
#include <stb_image.h>

namespace
{
   bool LoadHeightmapTextureSamplesInternal(const std::string& filePath, std::vector<float>& outSamples, int& outWidth, int& outHeight)
   {
      outSamples.clear();
      outWidth = 0;
      outHeight = 0;
      if (filePath.empty())
         return false;

      // stb's vertical flip flag is global, so keep terrain-related sampling consistent.
      stbi_set_flip_vertically_on_load(false);

      int width = 0;
      int height = 0;
      int channels = 0;

      stbi_us* data16 = stbi_load_16(filePath.c_str(), &width, &height, &channels, 1);
      if (data16)
      {
         const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
         outSamples.resize(count, 0.0f);
         for (size_t i = 0; i < count; ++i)
         {
            outSamples[i] = static_cast<float>(data16[i]) / 65535.0f;
         }
         stbi_image_free(data16);
         outWidth = width;
         outHeight = height;
         return true;
      }

      stbi_uc* data8 = stbi_load(filePath.c_str(), &width, &height, &channels, 1);
      if (!data8)
         return false;

      const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
      outSamples.resize(count, 0.0f);
      for (size_t i = 0; i < count; ++i)
      {
         outSamples[i] = static_cast<float>(data8[i]) / 255.0f;
      }
      stbi_image_free(data8);
      outWidth = width;
      outHeight = height;
      return true;
   }
}

#ifndef CLAYMORE_RUNTIME
bool Terrain::ExportHeightmap(const TerrainComponent& terrain, const std::string& filePath)
{
   if (terrain.HeightMap.empty() || terrain.GridResolution < 2)
   {
      std::cerr << "[Terrain] Cannot export heightmap: no height data\n";
      return false;
   }

   const uint32_t res = terrain.GridResolution;
   
   // Create 16-bit grayscale image data (heightmap values are already 0-65535)
   std::vector<uint16_t> imageData(res * res);
   for (size_t i = 0; i < terrain.HeightMap.size(); ++i)
   {
      imageData[i] = terrain.HeightMap[i];
   }

   // stb_image_write's stbi_write_png expects bytes, so for 16-bit we need to handle endianness
   // Convert to big-endian for standard PNG 16-bit grayscale
   std::vector<uint8_t> rawData(res * res * 2);
   for (size_t i = 0; i < imageData.size(); ++i)
   {
      uint16_t val = imageData[i];
      rawData[i * 2 + 0] = static_cast<uint8_t>(val >> 8);    // High byte
      rawData[i * 2 + 1] = static_cast<uint8_t>(val & 0xFF);  // Low byte
   }

   // Note: stbi_write_png doesn't support 16-bit grayscale directly
   // Export as 8-bit grayscale instead (scaled from 16-bit)
   std::vector<uint8_t> data8bit(res * res);
   for (size_t i = 0; i < terrain.HeightMap.size(); ++i)
   {
      data8bit[i] = static_cast<uint8_t>(terrain.HeightMap[i] >> 8);  // Use high byte for 8-bit export
   }

   int result = stbi_write_png(filePath.c_str(), res, res, 1, data8bit.data(), res);
   if (result == 0)
   {
      std::cerr << "[Terrain] Failed to write heightmap to: " << filePath << "\n";
      return false;
   }

   std::cout << "[Terrain] Exported heightmap to: " << filePath << " (" << res << "x" << res << ")\n";
   return true;
}
#endif // CLAYMORE_RUNTIME - ExportHeightmap is editor-only

bool Terrain::ImportHeightmap(TerrainComponent& terrain, const std::string& filePath)
{
   if (filePath.empty())
      return false;

   // Ensure consistent texture orientation - stbi's flip flag is global
   stbi_set_flip_vertically_on_load(false);
   
   int width = 0, height = 0, channels = 0;
   
   // First try to load as 16-bit if possible
   stbi_us* data16 = stbi_load_16(filePath.c_str(), &width, &height, &channels, 1);
   if (data16)
   {
      // 16-bit grayscale import
      const uint32_t newRes = static_cast<uint32_t>(std::min(width, height));
      if (newRes != terrain.GridResolution)
      {
         terrain.Resize(newRes);
      }
      terrain.EnsureMapSize();

      const uint32_t res = terrain.GridResolution;
      for (uint32_t z = 0; z < res; ++z)
      {
         for (uint32_t x = 0; x < res; ++x)
         {
            const size_t srcIdx = static_cast<size_t>(z) * width + x;
            const size_t dstIdx = static_cast<size_t>(z) * res + x;
            if (srcIdx < static_cast<size_t>(width * height))
            {
               terrain.HeightMap[dstIdx] = data16[srcIdx];
            }
         }
      }

      stbi_image_free(data16);
      terrain.MarkDataDirty();
      std::cout << "[Terrain] Imported 16-bit heightmap from: " << filePath << " (" << res << "x" << res << ")\n";
      return true;
   }

   // Fallback to 8-bit
   stbi_uc* data8 = stbi_load(filePath.c_str(), &width, &height, &channels, 1);
   if (!data8)
   {
      std::cerr << "[Terrain] Failed to load heightmap image: " << filePath << "\n";
      return false;
   }

   // 8-bit grayscale import - scale to 16-bit
   const uint32_t newRes = static_cast<uint32_t>(std::min(width, height));
   if (newRes != terrain.GridResolution)
   {
      terrain.Resize(newRes);
   }
   terrain.EnsureMapSize();

   const uint32_t res = terrain.GridResolution;
   for (uint32_t z = 0; z < res; ++z)
   {
      for (uint32_t x = 0; x < res; ++x)
      {
         const size_t srcIdx = static_cast<size_t>(z) * width + x;
         const size_t dstIdx = static_cast<size_t>(z) * res + x;
         if (srcIdx < static_cast<size_t>(width * height))
         {
            // Scale 8-bit (0-255) to 16-bit (0-65535)
            terrain.HeightMap[dstIdx] = static_cast<uint16_t>(data8[srcIdx]) * 257;
         }
      }
   }

   stbi_image_free(data8);
   terrain.MarkDataDirty();
   std::cout << "[Terrain] Imported 8-bit heightmap from: " << filePath << " (" << res << "x" << res << ")\n";
   return true;
}

bool Terrain::LoadHeightmapTextureSamples(const std::string& filePath, std::vector<float>& outSamples, int& outWidth, int& outHeight)
{
   const bool loaded = LoadHeightmapTextureSamplesInternal(filePath, outSamples, outWidth, outHeight);
   if (!loaded)
   {
      std::cerr << "[Terrain] Failed to load heightmap texture samples from: " << filePath << "\n";
   }
   return loaded;
}

#ifndef CLAYMORE_RUNTIME
bool Terrain::ExportSplatmap(const TerrainComponent& terrain, const std::string& filePath)
{
   if (terrain.SplatMap.empty() || terrain.GridResolution < 2)
   {
      std::cerr << "[Terrain] Cannot export splatmap: no splat data\n";
      return false;
   }

   const uint32_t res = terrain.GridResolution;
   
   // Splatmap is already stored as RGBA u8vec4, perfect for PNG export
   std::vector<uint8_t> imageData(res * res * 4);
   for (size_t i = 0; i < terrain.SplatMap.size(); ++i)
   {
      imageData[i * 4 + 0] = terrain.SplatMap[i].r;
      imageData[i * 4 + 1] = terrain.SplatMap[i].g;
      imageData[i * 4 + 2] = terrain.SplatMap[i].b;
      imageData[i * 4 + 3] = terrain.SplatMap[i].a;
   }

   int result = stbi_write_png(filePath.c_str(), res, res, 4, imageData.data(), res * 4);
   if (result == 0)
   {
      std::cerr << "[Terrain] Failed to write splatmap to: " << filePath << "\n";
      return false;
   }

   std::cout << "[Terrain] Exported splatmap to: " << filePath << " (" << res << "x" << res << ")\n";
   return true;
}
#endif // CLAYMORE_RUNTIME - ExportSplatmap is editor-only

bool Terrain::ImportSplatmap(TerrainComponent& terrain, const std::string& filePath)
{
   if (filePath.empty())
      return false;

   // Ensure consistent texture orientation - stbi's flip flag is global
   stbi_set_flip_vertically_on_load(false);
   
   int width = 0, height = 0, channels = 0;
   
   // Load as RGBA (force 4 channels)
   stbi_uc* data = stbi_load(filePath.c_str(), &width, &height, &channels, 4);
   if (!data)
   {
      std::cerr << "[Terrain] Failed to load splatmap image: " << filePath << "\n";
      return false;
   }

   // Use the smaller dimension and current terrain resolution
   const uint32_t srcRes = static_cast<uint32_t>(std::min(width, height));
   const uint32_t res = terrain.GridResolution;
   terrain.EnsureMapSize();

   // Import the splatmap - resize if needed
   for (uint32_t z = 0; z < res; ++z)
   {
      for (uint32_t x = 0; x < res; ++x)
      {
         // Sample from source image with simple nearest-neighbor if sizes differ
         uint32_t srcX = (srcRes == res) ? x : static_cast<uint32_t>((float)x / res * srcRes);
         uint32_t srcZ = (srcRes == res) ? z : static_cast<uint32_t>((float)z / res * srcRes);
         srcX = std::min(srcX, srcRes - 1);
         srcZ = std::min(srcZ, srcRes - 1);
         
         const size_t srcIdx = (static_cast<size_t>(srcZ) * width + srcX) * 4;
         const size_t dstIdx = static_cast<size_t>(z) * res + x;
         
         terrain.SplatMap[dstIdx] = glm::u8vec4(
            data[srcIdx + 0],
            data[srcIdx + 1],
            data[srcIdx + 2],
            data[srcIdx + 3]
         );
      }
   }

   stbi_image_free(data);
   
   // Mark all splat regions dirty
   terrain.SplatDataDirty = true;
   MarkSplatRegionDirty(terrain, glm::ivec2(0), glm::ivec2(res - 1));
   
   std::cout << "[Terrain] Imported splatmap from: " << filePath << " (source: " << width << "x" << height 
             << ", target: " << res << "x" << res << ")\n";
   return true;
}

bool Terrain::LoadTerrainAsset(const std::string& assetPath, TerrainComponent& terrain)
{
   if (assetPath.empty())
      return false;

   std::string normalizedPath = assetPath;
   try
   {
      normalizedPath = IVirtualFS::NormalizePath(assetPath);
   }
   catch (...) {}

   // Try virtual filesystem first (for pak-mounted files), then disk
   std::vector<uint8_t> fileData;
   bool loaded = FileSystem::Instance().ReadFile(normalizedPath, fileData);
   
   // Fallback to direct disk access with project root resolution
   if (!loaded) {
      fs::path diskPath(normalizedPath);
      if (diskPath.is_relative())
      {
         const fs::path& projectRoot = FileSystem::Instance().GetProjectRoot();
         if (!projectRoot.empty())
         {
            diskPath = projectRoot / diskPath;
         }
      }
      std::ifstream stream(diskPath, std::ios::binary);
      if (!stream.is_open())
      {
         std::cerr << "[Terrain] Failed to open terrain asset for reading: " << diskPath.string() << std::endl;
         return false;
      }
      stream.seekg(0, std::ios::end);
      size_t size = static_cast<size_t>(stream.tellg());
      stream.seekg(0, std::ios::beg);
      fileData.resize(size);
      stream.read(reinterpret_cast<char*>(fileData.data()), size);
      if (!stream.good()) {
         std::cerr << "[Terrain] Failed to read terrain asset from disk: " << diskPath.string() << std::endl;
         return false;
      }
   }
   
   // Create a memory stream for parsing
   size_t readPos = 0;
   auto readBytes = [&](void* dst, size_t count) -> bool {
      if (readPos + count > fileData.size()) return false;
      std::memcpy(dst, fileData.data() + readPos, count);
      readPos += count;
      return true;
   };

   // First read the v2 header (compatible with all versions)
   TerrainAssetHeaderV2 headerV2{};
   if (!readBytes(&headerV2, sizeof(headerV2)) || std::memcmp(headerV2.Magic, kTerrainAssetMagic, sizeof(headerV2.Magic)) != 0)
   {
      std::cerr << "[Terrain] Invalid terrain asset header: " << normalizedPath << std::endl;
      return false;
   }
   if (headerV2.Version < 1 || headerV2.Version > kTerrainAssetVersion)
   {
      std::cerr << "[Terrain] Unsupported terrain asset version (" << headerV2.Version << "): " << normalizedPath << std::endl;
      return false;
   }
   
   // Copy v2 header to full header
   TerrainAssetHeader header{};
   static_cast<TerrainAssetHeaderV2&>(header) = headerV2;
   header.SplatCount2 = 0;
   header.HoleCount = 0;
   
   // Read v3 extension if present
   if (headerV2.Version >= 3)
   {
      if (!readBytes(&header.SplatCount2, sizeof(header.SplatCount2)))
      {
         std::cerr << "[Terrain] Failed to read v3 header extension: " << normalizedPath << std::endl;
         return false;
      }
   }
   if (headerV2.Version >= 4)
   {
      if (!readBytes(&header.HoleCount, sizeof(header.HoleCount)))
      {
         std::cerr << "[Terrain] Failed to read v4 header extension: " << normalizedPath << std::endl;
         return false;
      }
   }

   terrain.GridResolution = header.GridResolution;
   terrain.ChunkResolution = header.ChunkResolution;
   terrain.WorldSize = glm::vec2(header.WorldSizeX, header.WorldSizeY);
   terrain.MaxHeight = header.MaxHeight;
   terrain.EnsureMapSize();

   if (terrain.HeightMap.size() != header.HeightCount)
   {
      terrain.HeightMap.resize(header.HeightCount);
   }
   if (terrain.SplatMap.size() != header.SplatCount)
   {
      terrain.SplatMap.resize(header.SplatCount);
   }
   terrain.HoleMask.assign(static_cast<size_t>(terrain.GridResolution) * terrain.GridResolution, 0);

   if (!readBytes(terrain.HeightMap.data(), header.HeightCount * sizeof(uint16_t)) ||
       !readBytes(terrain.SplatMap.data(), header.SplatCount * sizeof(glm::u8vec4)))
   {
      std::cerr << "[Terrain] Failed to read terrain asset payload: " << normalizedPath << std::endl;
      return false;
   }

   // Load SplatMap2 for 8-layer support (v3+)
   if (header.Version >= 3 && header.SplatCount2 > 0)
   {
      terrain.SplatMap2.resize(header.SplatCount2);
      if (!readBytes(terrain.SplatMap2.data(), header.SplatCount2 * sizeof(glm::u8vec4)))
      {
         std::cerr << "[Terrain] Failed to read terrain SplatMap2: " << normalizedPath << std::endl;
         return false;
      }
   }
   else
   {
      terrain.SplatMap2.clear();  // No second splatmap in older versions
   }
   if (header.Version >= 4 && header.HoleCount > 0)
   {
      terrain.HoleMask.resize(header.HoleCount);
      if (!readBytes(terrain.HoleMask.data(), header.HoleCount * sizeof(uint8_t)))
      {
         std::cerr << "[Terrain] Failed to read terrain hole mask: " << normalizedPath << std::endl;
         return false;
      }
   }
   const size_t expectedHoleCount = static_cast<size_t>(terrain.GridResolution) * terrain.GridResolution;
   if (terrain.HoleMask.size() != expectedHoleCount)
   {
      terrain.HoleMask.assign(expectedHoleCount, 0);
   }

   std::unordered_map<ClaymoreGUID, std::vector<uint8_t>> grassMasks;
   if (header.Version >= 2)
   {
      uint32_t storedLayers = 0;
      readBytes(&storedLayers, sizeof(uint32_t));
      for (uint32_t i = 0; i < storedLayers; ++i)
      {
         ClaymoreGUID guid;
         uint32_t maskCount = 0;
         readBytes(&guid.high, sizeof(uint64_t));
         readBytes(&guid.low, sizeof(uint64_t));
         readBytes(&maskCount, sizeof(uint32_t));
         std::vector<uint8_t> mask(maskCount);
         if (maskCount > 0)
         {
            readBytes(mask.data(), maskCount * sizeof(uint8_t));
         }
         grassMasks.emplace(guid, std::move(mask));
      }
   }

   std::unordered_map<ClaymoreGUID, std::vector<uint8_t>> instancerMasks;
   if (header.Version >= 5)
   {
      uint32_t storedLayers = 0;
      if (!readBytes(&storedLayers, sizeof(uint32_t)))
      {
         std::cerr << "[Terrain] Failed to read terrain instancer mask count: " << normalizedPath << std::endl;
         return false;
      }
      for (uint32_t i = 0; i < storedLayers; ++i)
      {
         ClaymoreGUID guid;
         uint32_t maskCount = 0;
         if (!readBytes(&guid.high, sizeof(uint64_t)) ||
             !readBytes(&guid.low, sizeof(uint64_t)) ||
             !readBytes(&maskCount, sizeof(uint32_t)))
         {
            std::cerr << "[Terrain] Failed to read terrain instancer mask header: " << normalizedPath << std::endl;
            return false;
         }
         std::vector<uint8_t> mask(maskCount);
         if (maskCount > 0 && !readBytes(mask.data(), maskCount * sizeof(uint8_t)))
         {
            std::cerr << "[Terrain] Failed to read terrain instancer mask payload: " << normalizedPath << std::endl;
            return false;
         }
         instancerMasks.emplace(guid, std::move(mask));
      }
   }

   terrain.AssetPath = normalizedPath;
   terrain.AssetDirty = false;
   terrain.HeightDataDirty = true;
   terrain.SplatDataDirty = true;
   terrain.HoleDataDirty = true;
   terrain.MeshDirty = true;
   terrain.PhysicsDirty = true;

   for (auto& layer : terrain.GrassLayers)
   {
      auto it = grassMasks.find(layer.Guid);
      if (it != grassMasks.end())
      {
         layer.PaintedMask = it->second;
      }
      layer.EnsureMaskSize(terrain.GridResolution);
   }

   for (auto& layer : terrain.InstancerLayers)
   {
      auto it = instancerMasks.find(layer.Guid);
      if (it != instancerMasks.end())
      {
         layer.PaintedMask = it->second;
      }
      layer.EnsureMaskSize(terrain.GridResolution);
      layer.MarkRuntimeDirty();
   }
   terrain.InstancerLayersDirty = true;

   return true;
}

JPH::RefConst<JPH::Shape> Terrain::BuildHeightFieldShape(const TerrainComponent& terrain)
{
   // Ensure we have valid height data
   if (terrain.HeightMap.empty() || terrain.GridResolution < 2)
   {
      std::cerr << "[Terrain] Cannot build heightfield: invalid height data\n";
      return nullptr;
   }

   const uint32_t res = terrain.GridResolution;
   const glm::vec2 cellSize = GetCellSize(terrain);
   
   // Jolt HeightFieldShape expects a 2D grid of height samples
   // Convert heightmap (uint16_t normalized to 0-65535) to float heights
   
   // Allocate height samples array (size: res * res)
   std::vector<float> heightSamples(res * res);
   
   // Convert heightmap to world-space heights
   const float invMax = 1.0f / 65535.0f;
   bool hasCollisionSamples = false;
   for (uint32_t z = 0; z < res; ++z)
   {
      for (uint32_t x = 0; x < res; ++x)
      {
         const size_t idx = static_cast<size_t>(z) * res + x;
         if (idx < terrain.HoleMask.size() && terrain.HoleMask[idx] >= 128u)
         {
            heightSamples[z * res + x] = JPH::HeightFieldShapeConstants::cNoCollisionValue;
            continue;
         }
         const uint16_t h = terrain.HeightMap[idx];
         const float normalized = static_cast<float>(h) * invMax;
         heightSamples[z * res + x] = normalized * terrain.MaxHeight;
         hasCollisionSamples = true;
      }
   }
   if (!hasCollisionSamples)
   {
      return nullptr;
   }
   
   // Create HeightFieldShapeSettings
   // Jolt's HeightFieldShape uses samples in row-major order (Z-major), which matches our layout
   // The terrain spans from (0, 0, 0) to (WorldSize.x, MaxHeight, WorldSize.y) in local space
   
   // Calculate sample spacing (world units per sample)
   const float sampleSpacingX = cellSize.x;
   const float sampleSpacingZ = cellSize.y;
   
   // Create heightfield settings using constructor
   // Jolt HeightFieldShapeSettings constructor signature (from Jolt header):
   // HeightFieldShapeSettings(const float *inSamples, Vec3Arg inOffset, Vec3Arg inScale, uint32 inSampleCount, ...)
   // Note: inSampleCount is a single uint32 for square grids (res x res)
   // The samples array must persist until Create() is called (heightSamples vector will persist)
   JPH::HeightFieldShapeSettings hfSettings(
      heightSamples.data(),  // Samples array (inSampleCount^2 floats)
      JPH::Vec3(0.0f, 0.0f, 0.0f),  // Offset
      JPH::Vec3(sampleSpacingX, 1.0f, sampleSpacingZ),  // Scale
      res  // Sample count (for square grid: res x res)
   );
   
   // Set optional parameters
   hfSettings.mBlockSize = 2; // Use small blocks for better performance with large terrains
   
   // Create the shape
   JPH::ShapeSettings::ShapeResult result = hfSettings.Create();
   
   if (result.HasError())
   {
      std::cerr << "[Terrain] Failed to create heightfield shape: " << result.GetError() << "\n";
      return nullptr;
   }
   
   return result.Get();
}

void Terrain::UpdatePhysicsBody(TerrainComponent& terrain, const glm::mat4& worldTransform, EntityID ownerEntity)
{
   // Destroy existing body if it exists
   if (!terrain.PhysicsBodyID.IsInvalid())
   {
      DestroyPhysicsBody(terrain);
   }
   
   // Only create physics body if we have valid height data
   if (terrain.HeightMap.empty() || terrain.GridResolution < 2)
   {
      terrain.PhysicsDirty = false;
      return;
   }
   
   // Build heightfield shape
   JPH::RefConst<JPH::Shape> heightFieldShape = BuildHeightFieldShape(terrain);
   if (!heightFieldShape)
   {
      terrain.PhysicsDirty = false;
      return;
   }
   
   // Create static physics body with the heightfield shape on Terrain layer
   int32_t terrainLayerIdx = PhysicsLayers::PhysicsLayerManager::Get().GetLayerIndex("Terrain");
   uint32_t terrainLayer = (terrainLayerIdx >= 0) ? static_cast<uint32_t>(terrainLayerIdx) : 0;
   terrain.PhysicsBodyID = Physics::CreateBody(worldTransform, heightFieldShape, true, terrainLayer);
   if (!terrain.PhysicsBodyID.IsInvalid() && ownerEntity != 0) {
      Physics::GetBodyInterface().SetUserData(terrain.PhysicsBodyID, (uint64_t)ownerEntity);
   }
   
   if (!terrain.PhysicsBodyID.IsInvalid())
   {
      terrain.PhysicsDirty = false;
   }
   else
   {
      std::cerr << "[Terrain] Failed to create physics body\n";
   }
}

void Terrain::DestroyPhysicsBody(TerrainComponent& terrain)
{
   if (!terrain.PhysicsBodyID.IsInvalid())
   {
      Physics::DestroyBody(terrain.PhysicsBodyID);
      terrain.PhysicsBodyID = JPH::BodyID();
      terrain.PhysicsDirty = false;
   }
}

void Terrain::DestroyClipmapSystem(TerrainComponent& terrain)
{
   if (terrain.ClipmapSystem != nullptr)
   {
      auto* clipSystem = static_cast<terrain::TerrainClipmapSystem*>(terrain.ClipmapSystem);
      delete clipSystem;
      terrain.ClipmapSystem = nullptr;
   }
}

void Terrain::DestroyStreamingSystem(TerrainComponent& terrain)
{
   if (terrain.ChunkStreamingSystem != nullptr)
   {
      auto* streaming = static_cast<terrain::TerrainStreamingSystem*>(terrain.ChunkStreamingSystem);
      delete streaming;
      terrain.ChunkStreamingSystem = nullptr;
   }
}
