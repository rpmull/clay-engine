#pragma once

#include <glm/glm.hpp>
#include <bgfx/bgfx.h>
#include <cstdint>
#include <vector>
#include <queue>
#include <mutex>
#include <atomic>
#include <functional>
#include <unordered_map>
#include <unordered_set>

// Forward declarations
struct TerrainComponent;
struct TerrainChunk;
class JobSystem;

namespace terrain {

// Chunk coordinate for streaming (grid position)
struct ChunkCoord
{
   int32_t x = 0;
   int32_t z = 0;
   
   bool operator==(const ChunkCoord& other) const
   {
      return x == other.x && z == other.z;
   }
};

// Hash function for ChunkCoord
struct ChunkCoordHash
{
   size_t operator()(const ChunkCoord& c) const
   {
      return std::hash<int32_t>()(c.x) ^ (std::hash<int32_t>()(c.z) << 16);
   }
};

// Streaming request types
enum class StreamRequestType : uint8_t
{
   Load = 0,      // Load chunk data from disk
   Unload = 1,    // Unload chunk data from memory
   GpuUpload = 2  // Upload loaded data to GPU
};

// Streaming priority (lower = higher priority)
enum class StreamPriority : uint8_t
{
   Immediate = 0,  // Camera chunk
   High = 1,       // Adjacent to camera
   Normal = 2,     // Within load radius
   Low = 3         // Prefetch
};

// Pending stream request
struct StreamRequest
{
   ChunkCoord Coord;
   StreamRequestType Type;
   StreamPriority Priority;
   uint32_t ChunkIndex;  // Index into terrain.Chunks
   
   // Priority comparison for queue (lower priority value = higher priority)
   bool operator<(const StreamRequest& other) const
   {
      return static_cast<int>(Priority) > static_cast<int>(other.Priority);
   }
};

// Loaded chunk data (before GPU upload)
struct ChunkLoadResult
{
   ChunkCoord Coord;
   uint32_t ChunkIndex;
   std::vector<uint16_t> HeightData;
   std::vector<glm::u8vec4> SplatData;
   bool Success = false;
};

// Streaming system configuration
struct StreamingConfig
{
   float LoadRadius = 500.0f;       // Distance to start loading chunks
   float UnloadRadius = 600.0f;     // Distance to unload chunks
   float PrefetchRadius = 700.0f;   // Distance for low-priority prefetch
   uint32_t MaxLoadsPerFrame = 2;   // Max background loads to start per frame
   uint32_t MaxUploadsPerFrame = 4; // Max GPU uploads per frame
   uint32_t MaxUnloadsPerFrame = 8; // Max chunk unload decisions per frame
   double MaxUpdateMs = 0.75;       // Main-thread budget for Update() decision work
   bool Enabled = false;            // Enable/disable streaming
};

// Main terrain streaming manager
class TerrainStreamingSystem
{
public:
   TerrainStreamingSystem() = default;
   ~TerrainStreamingSystem() { Shutdown(); }
   
   // Initialize streaming for a terrain
   void Init(TerrainComponent& terrain, JobSystem& jobs, const StreamingConfig& config = StreamingConfig{});
   
   // Shutdown and cleanup
   void Shutdown();
   
   // Update streaming based on camera position
   // Call once per frame before rendering
   void Update(TerrainComponent& terrain, const glm::vec3& cameraWorldPos, const glm::mat4& terrainWorldMatrix);
   
   // Process pending GPU uploads (must be called from main/render thread)
   void ProcessGpuUploads(TerrainComponent& terrain);
   
   // Check if streaming is busy (has pending work)
   bool IsBusy() const { return m_PendingLoads.load() > 0 || !m_UploadQueue.empty(); }
   
   // Get pending load count
   uint32_t GetPendingLoadCount() const { return m_PendingLoads.load(); }
   
   // Get streaming stats
   uint32_t GetLoadedChunkCount() const { return static_cast<uint32_t>(m_LoadedChunks.size()); }
   uint32_t GetTotalStreamRequests() const { return m_TotalRequests; }
   
   // Enable/disable streaming
   void SetEnabled(bool enabled) { m_Config.Enabled = enabled; }
   bool IsEnabled() const { return m_Config.Enabled; }
   
   // Configuration access
   StreamingConfig& GetConfig() { return m_Config; }
   const StreamingConfig& GetConfig() const { return m_Config; }

private:
   static int64_t EncodeCoord(const ChunkCoord& coord);
   void RebuildChunkLookup(const TerrainComponent& terrain);
   bool TryGetChunkIndex(const ChunkCoord& coord, uint32_t& outChunkIndex) const;

   // Queue a load request
   void QueueLoad(const ChunkCoord& coord, uint32_t chunkIndex, StreamPriority priority);
   
   // Queue an unload request
   void QueueUnload(const ChunkCoord& coord, uint32_t chunkIndex);
   
   // Background load function (runs on job thread)
   void LoadChunkAsync(TerrainComponent& terrain, const ChunkCoord& coord, uint32_t chunkIndex);
   
   // Create height texture from loaded data
   void UploadChunkToGpu(TerrainComponent& terrain, const ChunkLoadResult& result);
   
   // Calculate streaming priority based on distance
   StreamPriority CalculatePriority(float distance) const;
   
   // Convert world position to chunk coordinate
   ChunkCoord WorldToChunkCoord(const glm::vec3& worldPos, const TerrainComponent& terrain) const;
   
   StreamingConfig m_Config;
   JobSystem* m_Jobs = nullptr;
   
   // Request queue (thread-safe access)
   std::mutex m_RequestMutex;
   std::priority_queue<StreamRequest> m_RequestQueue;
   
   // Upload queue (results from background loads)
   std::mutex m_UploadMutex;
   std::queue<ChunkLoadResult> m_UploadQueue;
   
   // Tracking
   std::unordered_set<ChunkCoord, ChunkCoordHash> m_LoadedChunks;
   std::unordered_set<ChunkCoord, ChunkCoordHash> m_PendingChunks;
   std::unordered_map<int64_t, uint32_t> m_ChunkIndexByCoord;

   glm::vec3 m_LastCameraLocalPos{0.0f};
   ChunkCoord m_LastCameraChunk{};
   bool m_HasLastCameraSample = false;
   
   std::atomic<uint32_t> m_PendingLoads{0};
   uint32_t m_TotalRequests = 0;
   
   bool m_Initialized = false;
};

} // namespace terrain

