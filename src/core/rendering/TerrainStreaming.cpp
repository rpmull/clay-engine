#include "TerrainStreaming.h"
#include "Terrain.h"
#include "core/ecs/Components.h"
#include "core/jobs/JobSystem.h"
#include "core/utils/Profiler.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cfloat>
#include <iostream>

namespace terrain {
namespace {
float DistanceToAabbXZ(const glm::vec3& p, const glm::vec3& bmin, const glm::vec3& bmax)
{
   float dx = 0.0f;
   if (p.x < bmin.x) dx = bmin.x - p.x;
   else if (p.x > bmax.x) dx = p.x - bmax.x;

   float dz = 0.0f;
   if (p.z < bmin.z) dz = bmin.z - p.z;
   else if (p.z > bmax.z) dz = p.z - bmax.z;

   return std::sqrt(dx * dx + dz * dz);
}
} // namespace

int64_t TerrainStreamingSystem::EncodeCoord(const ChunkCoord& coord)
{
   return (int64_t(coord.x) << 32) ^ (int64_t(coord.z) & 0xFFFFFFFFLL);
}

void TerrainStreamingSystem::RebuildChunkLookup(const TerrainComponent& terrain)
{
   m_ChunkIndexByCoord.clear();
   m_ChunkIndexByCoord.reserve(terrain.Chunks.size() * 2);
   for (size_t i = 0; i < terrain.Chunks.size(); ++i)
   {
      const TerrainChunk& chunk = terrain.Chunks[i];
      ChunkCoord coord;
      coord.x = static_cast<int32_t>(chunk.GridX);
      coord.z = static_cast<int32_t>(chunk.GridZ);
      m_ChunkIndexByCoord[EncodeCoord(coord)] = static_cast<uint32_t>(i);
   }
}

bool TerrainStreamingSystem::TryGetChunkIndex(const ChunkCoord& coord, uint32_t& outChunkIndex) const
{
   const auto it = m_ChunkIndexByCoord.find(EncodeCoord(coord));
   if (it == m_ChunkIndexByCoord.end())
      return false;
   outChunkIndex = it->second;
   return true;
}

void TerrainStreamingSystem::Init(TerrainComponent& terrain, JobSystem& jobs, const StreamingConfig& config)
{
   if (m_Initialized)
      Shutdown();
   
   m_Config = config;
   m_Jobs = &jobs;
   m_HasLastCameraSample = false;
   RebuildChunkLookup(terrain);
   
   // Initially mark all chunks as loaded (for non-streaming scenarios)
   for (size_t i = 0; i < terrain.Chunks.size(); ++i)
   {
      TerrainChunk& chunk = terrain.Chunks[i];
      chunk.StreamState = ChunkStreamState::Resident;
      
      ChunkCoord coord;
      coord.x = static_cast<int32_t>(chunk.GridX);
      coord.z = static_cast<int32_t>(chunk.GridZ);
      m_LoadedChunks.insert(coord);
   }
   
   m_Initialized = true;
}

void TerrainStreamingSystem::Shutdown()
{
   // Wait for pending loads to complete
   while (m_PendingLoads.load() > 0)
   {
      std::this_thread::yield();
   }
   
   // Clear all state
   {
      std::lock_guard<std::mutex> lock(m_RequestMutex);
      while (!m_RequestQueue.empty())
         m_RequestQueue.pop();
   }
   
   {
      std::lock_guard<std::mutex> lock(m_UploadMutex);
      while (!m_UploadQueue.empty())
         m_UploadQueue.pop();
   }
   
   m_LoadedChunks.clear();
   m_PendingChunks.clear();
   m_ChunkIndexByCoord.clear();
   m_TotalRequests = 0;
   m_HasLastCameraSample = false;
   m_Jobs = nullptr;
   m_Initialized = false;
}

ChunkCoord TerrainStreamingSystem::WorldToChunkCoord(const glm::vec3& worldPos, const TerrainComponent& terrain) const
{
   if (terrain.ChunksX == 0 || terrain.ChunksZ == 0)
      return {0, 0};
   
   const float chunkWorldSizeX = terrain.WorldSize.x / static_cast<float>(terrain.ChunksX);
   const float chunkWorldSizeZ = terrain.WorldSize.y / static_cast<float>(terrain.ChunksZ);
   
   ChunkCoord coord;
   coord.x = static_cast<int32_t>(std::floor(worldPos.x / chunkWorldSizeX));
   coord.z = static_cast<int32_t>(std::floor(worldPos.z / chunkWorldSizeZ));
   
   // Clamp to valid range
   coord.x = std::clamp(coord.x, 0, static_cast<int32_t>(terrain.ChunksX - 1));
   coord.z = std::clamp(coord.z, 0, static_cast<int32_t>(terrain.ChunksZ - 1));
   
   return coord;
}

StreamPriority TerrainStreamingSystem::CalculatePriority(float distance) const
{
   if (distance < m_Config.LoadRadius * 0.25f)
      return StreamPriority::Immediate;
   else if (distance < m_Config.LoadRadius * 0.5f)
      return StreamPriority::High;
   else if (distance < m_Config.LoadRadius)
      return StreamPriority::Normal;
   else
      return StreamPriority::Low;
}

void TerrainStreamingSystem::Update(TerrainComponent& terrain, 
                                     const glm::vec3& cameraWorldPos,
                                     const glm::mat4& terrainWorldMatrix)
{
   auto updateStart = std::chrono::high_resolution_clock::now();
   Profiler::Get().AddCounter("TerrainStreaming/Updates", 1);
   if (!m_Initialized || !m_Config.Enabled || !m_Jobs)
      return;
   if (terrain.Chunks.empty())
      return;
   if (m_ChunkIndexByCoord.size() != terrain.Chunks.size())
      RebuildChunkLookup(terrain);
   
   // Transform camera to terrain local space
   glm::mat4 invWorld = glm::inverse(terrainWorldMatrix);
   glm::vec4 localCam4 = invWorld * glm::vec4(cameraWorldPos, 1.0f);
   glm::vec3 localCam = glm::vec3(localCam4);

   const ChunkCoord cameraChunk = WorldToChunkCoord(localCam, terrain);
   const float chunkSizeX = terrain.WorldSize.x / static_cast<float>(std::max(1u, terrain.ChunksX));
   const float chunkSizeZ = terrain.WorldSize.y / static_cast<float>(std::max(1u, terrain.ChunksZ));
   const float movementThreshold = std::max(1.0f, std::min(chunkSizeX, chunkSizeZ) * 0.25f);
   const bool chunkCellChanged = !m_HasLastCameraSample ||
                                 (cameraChunk.x != m_LastCameraChunk.x) ||
                                 (cameraChunk.z != m_LastCameraChunk.z);
   const float movedDist = m_HasLastCameraSample ? glm::length(localCam - m_LastCameraLocalPos) : FLT_MAX;
   if (!chunkCellChanged && movedDist < movementThreshold) {
      Profiler::Get().AddCounter("TerrainStreaming/UpdatesSkipped", 1);
      return;
   }

   m_LastCameraLocalPos = localCam;
   m_LastCameraChunk = cameraChunk;
   m_HasLastCameraSample = true;

   struct LoadCandidate {
      ChunkCoord coord;
      uint32_t chunkIndex = 0;
      float distance = 0.0f;
   };

   std::unordered_set<int64_t> keepLoaded;
   keepLoaded.reserve(128);
   std::vector<LoadCandidate> loadCandidates;
   loadCandidates.reserve(128);

   const int maxDx = static_cast<int>(std::ceil(m_Config.UnloadRadius / std::max(1.0f, chunkSizeX)));
   const int maxDz = static_cast<int>(std::ceil(m_Config.UnloadRadius / std::max(1.0f, chunkSizeZ)));

   auto budgetExceeded = [&]() -> bool {
      if (m_Config.MaxUpdateMs <= 0.0) return false;
      const auto now = std::chrono::high_resolution_clock::now();
      const double elapsedMs = std::chrono::duration<double, std::milli>(now - updateStart).count();
      return elapsedMs >= m_Config.MaxUpdateMs;
   };

   for (int dz = -maxDz; dz <= maxDz; ++dz)
   {
      for (int dx = -maxDx; dx <= maxDx; ++dx)
      {
         if (budgetExceeded()) {
            break;
         }
         ChunkCoord coord;
         coord.x = cameraChunk.x + dx;
         coord.z = cameraChunk.z + dz;
         if (coord.x < 0 || coord.z < 0 ||
             coord.x >= static_cast<int32_t>(terrain.ChunksX) ||
             coord.z >= static_cast<int32_t>(terrain.ChunksZ))
         {
            continue;
         }

         uint32_t chunkIndex = 0;
         if (!TryGetChunkIndex(coord, chunkIndex) || chunkIndex >= terrain.Chunks.size())
            continue;

         const TerrainChunk& chunk = terrain.Chunks[chunkIndex];
         const float distance = DistanceToAabbXZ(localCam, chunk.WorldMin, chunk.WorldMax);
         if (distance <= m_Config.UnloadRadius)
         {
            keepLoaded.insert(EncodeCoord(coord));
         }
         if (distance <= m_Config.LoadRadius)
         {
            loadCandidates.push_back({coord, chunkIndex, distance});
         }
      }
      if (budgetExceeded()) {
         break;
      }
   }

   uint32_t loadsQueued = 0;
   if (m_Config.MaxLoadsPerFrame > 0)
   {
      std::vector<LoadCandidate> bestLoads;
      bestLoads.reserve(m_Config.MaxLoadsPerFrame + 1);
      for (const LoadCandidate& candidate : loadCandidates)
      {
         if (m_LoadedChunks.count(candidate.coord) > 0 || m_PendingChunks.count(candidate.coord) > 0)
            continue;

         if (bestLoads.size() < m_Config.MaxLoadsPerFrame)
         {
            bestLoads.push_back(candidate);
            continue;
         }

         size_t worstIndex = 0;
         float worstDistance = bestLoads[0].distance;
         for (size_t i = 1; i < bestLoads.size(); ++i)
         {
            if (bestLoads[i].distance > worstDistance)
            {
               worstDistance = bestLoads[i].distance;
               worstIndex = i;
            }
         }
         if (candidate.distance < worstDistance)
         {
            bestLoads[worstIndex] = candidate;
         }
      }

      std::sort(bestLoads.begin(), bestLoads.end(),
         [](const LoadCandidate& a, const LoadCandidate& b) { return a.distance < b.distance; });

      for (const LoadCandidate& candidate : bestLoads)
      {
         QueueLoad(candidate.coord, candidate.chunkIndex, CalculatePriority(candidate.distance));
         ++loadsQueued;
      }
   }

   std::vector<ChunkCoord> loadedSnapshot;
   loadedSnapshot.reserve(m_LoadedChunks.size());
   for (const ChunkCoord& coord : m_LoadedChunks)
      loadedSnapshot.push_back(coord);

   uint32_t unloadsQueued = 0;
   uint32_t unloadBudget = m_Config.MaxUnloadsPerFrame;
   for (const ChunkCoord& coord : loadedSnapshot)
   {
      if (unloadBudget == 0) break;
      const int64_t encoded = EncodeCoord(coord);
      if (keepLoaded.find(encoded) != keepLoaded.end())
         continue;
      if (m_PendingChunks.count(coord) > 0)
         continue;

      uint32_t chunkIndex = 0;
      if (!TryGetChunkIndex(coord, chunkIndex) || chunkIndex >= terrain.Chunks.size())
         continue;

      TerrainChunk& chunk = terrain.Chunks[chunkIndex];
      if (chunk.StreamState == ChunkStreamState::Resident)
      {
         chunk.StreamState = ChunkStreamState::Unloaded;
      }
      QueueUnload(coord, chunkIndex);
      ++unloadsQueued;
      --unloadBudget;
   }

   Profiler::Get().SetCounter("TerrainStreaming/LoadsQueued", loadsQueued);
   Profiler::Get().SetCounter("TerrainStreaming/UnloadsQueued", unloadsQueued);
   const auto updateEnd = std::chrono::high_resolution_clock::now();
   const double elapsedMs = std::chrono::duration<double, std::milli>(updateEnd - updateStart).count();
   Profiler::Get().SetCounter("TerrainStreaming/UpdateUs", static_cast<uint64_t>(elapsedMs * 1000.0));
}

void TerrainStreamingSystem::QueueLoad(const ChunkCoord& coord, uint32_t chunkIndex, StreamPriority priority)
{
   // Mark as pending
   {
      std::lock_guard<std::mutex> lock(m_RequestMutex);
      
      if (m_PendingChunks.count(coord) > 0)
         return;  // Already pending
      
      m_PendingChunks.insert(coord);
   }
   
   m_PendingLoads.fetch_add(1);
   ++m_TotalRequests;
   
   // Note: In a full implementation, this would load from disk.
   // For now, we'll just mark as loaded since all data is already in memory.
   // The actual disk streaming would be implemented per-project needs.
   
   // Queue the "load" result immediately (data is already in memory)
   ChunkLoadResult result;
   result.Coord = coord;
   result.ChunkIndex = chunkIndex;
   result.Success = true;
   // HeightData and SplatData left empty - we'll use the existing terrain data
   
   {
      std::lock_guard<std::mutex> lock(m_UploadMutex);
      m_UploadQueue.push(std::move(result));
   }
   
   m_PendingLoads.fetch_sub(1);
}

void TerrainStreamingSystem::QueueUnload(const ChunkCoord& coord, uint32_t chunkIndex)
{
   // For now, just mark as unloaded but don't actually free GPU resources
   // (the unified heightmap texture is shared)
   
   m_LoadedChunks.erase(coord);
   
   // In a full streaming implementation, you would:
   // 1. Destroy per-chunk GPU resources
   // 2. Free CPU memory
   // 3. Update chunk state
}

void TerrainStreamingSystem::LoadChunkAsync(TerrainComponent& terrain, 
                                             const ChunkCoord& coord, 
                                             uint32_t chunkIndex)
{
   // This would be called on a background thread in a full implementation
   // For disk streaming, you would:
   // 1. Read height data from terrain asset file
   // 2. Read splat data from terrain asset file
   // 3. Decompress if needed
   // 4. Queue result for GPU upload
   
   ChunkLoadResult result;
   result.Coord = coord;
   result.ChunkIndex = chunkIndex;
   
   // Extract chunk's portion of the height/splat data
   if (chunkIndex < terrain.Chunks.size())
   {
      const TerrainChunk& chunk = terrain.Chunks[chunkIndex];
      
      // Copy height data for this chunk
      const uint32_t startX = static_cast<uint32_t>(chunk.Start.x);
      const uint32_t startZ = static_cast<uint32_t>(chunk.Start.y);
      const uint32_t endX = startX + chunk.VertexCountX;
      const uint32_t endZ = startZ + chunk.VertexCountZ;
      
      result.HeightData.reserve(chunk.VertexCountX * chunk.VertexCountZ);
      
      for (uint32_t z = startZ; z < endZ && z < terrain.GridResolution; ++z)
      {
         for (uint32_t x = startX; x < endX && x < terrain.GridResolution; ++x)
         {
            size_t idx = static_cast<size_t>(z) * terrain.GridResolution + x;
            if (idx < terrain.HeightMap.size())
               result.HeightData.push_back(terrain.HeightMap[idx]);
            else
               result.HeightData.push_back(0);
         }
      }
      
      // Copy splat data similarly
      result.SplatData.reserve(chunk.VertexCountX * chunk.VertexCountZ);
      for (uint32_t z = startZ; z < endZ && z < terrain.GridResolution; ++z)
      {
         for (uint32_t x = startX; x < endX && x < terrain.GridResolution; ++x)
         {
            size_t idx = static_cast<size_t>(z) * terrain.GridResolution + x;
            if (idx < terrain.SplatMap.size())
               result.SplatData.push_back(terrain.SplatMap[idx]);
            else
               result.SplatData.push_back(glm::u8vec4(255, 0, 0, 0));
         }
      }
      
      result.Success = true;
   }
   
   // Queue for GPU upload
   {
      std::lock_guard<std::mutex> lock(m_UploadMutex);
      m_UploadQueue.push(std::move(result));
   }
   
   m_PendingLoads.fetch_sub(1);
}

void TerrainStreamingSystem::ProcessGpuUploads(TerrainComponent& terrain)
{
   if (!m_Initialized)
      return;
   
   uint32_t uploadsProcessed = 0;
   
   while (uploadsProcessed < m_Config.MaxUploadsPerFrame)
   {
      ChunkLoadResult result;
      
      {
         std::lock_guard<std::mutex> lock(m_UploadMutex);
         if (m_UploadQueue.empty())
            break;
         
         result = std::move(m_UploadQueue.front());
         m_UploadQueue.pop();
      }
      
      if (result.Success && result.ChunkIndex < terrain.Chunks.size())
      {
         TerrainChunk& chunk = terrain.Chunks[result.ChunkIndex];
         
         // Mark as loaded
         chunk.StreamState = ChunkStreamState::Resident;
         m_LoadedChunks.insert(result.Coord);
         
         // Remove from pending
         {
            std::lock_guard<std::mutex> lock(m_RequestMutex);
            m_PendingChunks.erase(result.Coord);
         }
         
         // Note: For a fully streaming system, you would create per-chunk
         // height textures here. Since we're using a unified heightmap,
         // we just mark the chunk as ready.
      }
      
      ++uploadsProcessed;
   }
   Profiler::Get().SetCounter("TerrainStreaming/UploadsProcessed", uploadsProcessed);
}

void TerrainStreamingSystem::UploadChunkToGpu(TerrainComponent& terrain, const ChunkLoadResult& result)
{
   if (result.ChunkIndex >= terrain.Chunks.size())
      return;
   
   TerrainChunk& chunk = terrain.Chunks[result.ChunkIndex];
   
   // For per-chunk textures (if not using unified heightmap):
   // You would create chunk.HeightTexture and chunk.SplatTexture here
   // from result.HeightData and result.SplatData
   
   // Since we're using a unified heightmap, just mark as resident
   chunk.StreamState = ChunkStreamState::Resident;
}

} // namespace terrain

