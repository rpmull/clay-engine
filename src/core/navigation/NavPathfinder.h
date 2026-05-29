// High-Performance Navigation Pathfinder
// Features:
// - Lock-free concurrent path requests
// - Hierarchical pathfinding for large meshes
// - Bidirectional A* search
// - Path caching with LRU eviction
// - SIMD-accelerated distance calculations
#pragma once

#include "core/navigation/NavTypes.h"
#include "core/navigation/NavMesh.h"
#include <glm/glm.hpp>
#include <atomic>
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>
#include <memory>
#include <future>

namespace nav {

// Forward declarations
class NavMeshRuntime;

// ============================================================================
// Path Request/Result structures
// ============================================================================

struct PathRequest {
    uint64_t requestId;
    glm::vec3 start;
    glm::vec3 end;
    NavAgentParams params;
    NavFlags includeFlags;
    NavFlags excludeFlags;
    std::shared_ptr<NavMeshRuntime> navmesh;
    const NavMeshRuntime* navmeshPtr = nullptr; // Snapshot runtime identity for stale-result checks
    std::function<void(const NavPath&, bool)> callback;
    uint64_t userData = 0;  // Application-specific data (e.g., agent entity ID)
};

struct PathResult {
    uint64_t requestId;
    NavPath path;
    bool success;
    float computeTimeMs;
    const NavMeshRuntime* navmeshPtr = nullptr; // Runtime that produced this result
    uint64_t userData = 0;  // Passed through from request
};

// ============================================================================
// Hierarchical Navigation Mesh
// ============================================================================

struct NavRegion {
    uint32_t id;
    std::vector<uint32_t> polygons;     // Polygon indices in this region
    glm::vec3 center;                    // Region centroid
    Bounds bounds;                       // Region AABB
    std::vector<uint32_t> neighbors;     // Adjacent region IDs
    std::vector<uint32_t> borderPolys;   // Polygons on region boundary
};

struct HierarchicalNavMesh {
    std::vector<NavRegion> regions;
    std::vector<std::vector<uint32_t>> polyToRegion; // polygon -> region mapping
    
    // High-level graph for region-to-region pathfinding
    struct RegionEdge {
        uint32_t targetRegion;
        float cost;
        uint32_t borderPoly; // Entry polygon
    };
    std::vector<std::vector<RegionEdge>> regionGraph;
    
    bool IsBuilt() const { return !regions.empty(); }
};

// ============================================================================
// Path Cache with LRU eviction
// ============================================================================

class PathCache {
public:
    struct CacheKey {
        uint64_t navmeshHash;
        uint64_t paramsHash;
        uint64_t startHash;
        uint64_t endHash;
        
        bool operator==(const CacheKey& other) const {
            return navmeshHash == other.navmeshHash &&
                   paramsHash == other.paramsHash &&
                   startHash == other.startHash &&
                   endHash == other.endHash;
        }
    };
    
    struct CacheKeyHash {
        size_t operator()(const CacheKey& k) const {
            size_t h = std::hash<uint64_t>()(k.navmeshHash);
            h ^= (std::hash<uint64_t>()(k.paramsHash) << 1);
            h ^= (std::hash<uint64_t>()(k.startHash) << 1);
            h ^= (std::hash<uint64_t>()(k.endHash) << 2);
            return h;
        }
    };
    
    PathCache(size_t maxEntries = 1024) : m_MaxEntries(maxEntries) {}
    
    bool Get(const NavMeshRuntime& navmesh, const glm::vec3& start, const glm::vec3& end,
             const NavAgentParams& params, NavFlags include, NavFlags exclude, NavPath& outPath);
    void Put(const NavMeshRuntime& navmesh, const glm::vec3& start, const glm::vec3& end,
             const NavAgentParams& params, NavFlags include, NavFlags exclude, const NavPath& path);
    void Clear();
    void SetMaxEntries(size_t maxEntries);
    void SetCellSize(float size) { m_CellSize = size; }
    
    size_t Size() const {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Cache.size();
    }
    size_t Hits() const { return m_Hits.load(std::memory_order_relaxed); }
    size_t Misses() const { return m_Misses.load(std::memory_order_relaxed); }
    
private:
    uint64_t HashPosition(const glm::vec3& pos) const;
    void EvictOldest();
    
    struct CacheEntry {
        NavPath path;
        uint64_t accessTime;
    };
    
    std::unordered_map<CacheKey, CacheEntry, CacheKeyHash> m_Cache;
    mutable std::mutex m_Mutex;
    size_t m_MaxEntries;
    float m_CellSize = 1.0f; // Spatial hash cell size
    std::atomic<uint64_t> m_AccessCounter{0};
    std::atomic<size_t> m_Hits{0};
    std::atomic<size_t> m_Misses{0};
};

// ============================================================================
// High-Performance Pathfinder
// ============================================================================

class NavPathfinder {
public:
    struct Config {
        uint32_t numWorkerThreads = 0;    // 0 = auto (hardware_concurrency - 1)
        size_t maxCacheEntries = 2048;
        float cacheCellSize = 2.0f;
        uint32_t maxIterationsPerPath = 100000;
        bool useHierarchical = true;
        uint32_t regionSize = 64;          // Target polygons per region
        bool useBidirectional = true;
    };
    
    NavPathfinder();
    ~NavPathfinder();
    
    // Initialize with configuration
    void Initialize(const Config& config = {});
    void Shutdown();
    
    // Synchronous pathfinding (blocks until complete)
    bool FindPath(const NavMeshRuntime& navmesh, const glm::vec3& start, const glm::vec3& end,
                  const NavAgentParams& params, NavFlags include, NavFlags exclude, NavPath& outPath);
    
    // Asynchronous pathfinding (returns immediately, calls callback when done)
    // userData is passed through to the result for caller-specific tracking (e.g., entity ID)
    uint64_t RequestPathAsync(std::shared_ptr<NavMeshRuntime> navmesh,
                              const glm::vec3& start, const glm::vec3& end,
                              const NavAgentParams& params, NavFlags include, NavFlags exclude,
                              std::function<void(const NavPath&, bool)> callback = nullptr,
                              uint64_t userData = 0);
    
    // Cancel a pending request
    void CancelRequest(uint64_t requestId);
    
    // Process completed requests (call from main thread)
    // Returns completed results for main-thread processing
    std::vector<PathResult> ProcessCompletedRequests();
    size_t GetPendingRequestCount() const;
    size_t GetCompletedResultCount() const;
    
    // Build hierarchical structure for a navmesh
    void BuildHierarchy(NavMeshRuntime& navmesh);
    
    // Statistics
    struct Stats {
        std::atomic<uint64_t> totalRequests{0};
        std::atomic<uint64_t> completedRequests{0};
        std::atomic<uint64_t> cachedHits{0};
        std::atomic<double> totalComputeTimeMs{0};
        std::atomic<uint64_t> hierarchicalPaths{0};
        std::atomic<uint64_t> directPaths{0};
    };
    const Stats& GetStats() const { return m_Stats; }
    void ResetStats();
    
    // Cache management
    PathCache& GetCache() { return m_Cache; }
    void ClearCache() { m_Cache.Clear(); }
    
private:
    // Worker thread function
    void WorkerThread();
    
    // Internal pathfinding implementations
    bool FindPathDirect(const NavMeshRuntime& nm, const glm::vec3& start, const glm::vec3& end,
                        const NavAgentParams& params, NavPath& outPath);
    bool FindPathHierarchical(const NavMeshRuntime& nm, const glm::vec3& start, const glm::vec3& end,
                              const NavAgentParams& params, NavPath& outPath);
    bool FindPathBidirectional(const NavMeshRuntime& nm, uint32_t startPoly, uint32_t endPoly,
                               NavPath& outPath);
    
    // Spatial queries
    uint32_t FindNearestPoly(const NavMeshRuntime& nm, const glm::vec3& pos);
    uint32_t FindRegionForPoly(const NavMeshRuntime& nm, uint32_t polyIdx);
    
    // Hierarchy building
    void PartitionIntoRegions(NavMeshRuntime& nm, uint32_t targetRegionSize);
    void BuildRegionGraph(NavMeshRuntime& nm);
    
    Config m_Config;
    PathCache m_Cache;
    Stats m_Stats;
    
    // Thread pool
    std::vector<std::thread> m_Workers;
    std::atomic<bool> m_Running{false};
    
    // Request queue (lock-free would be better, but mutex is simpler for now)
    std::queue<PathRequest> m_RequestQueue;
    mutable std::mutex m_QueueMutex;
    std::condition_variable m_QueueCV;
    
    // Completed results waiting for main thread
    std::vector<PathResult> m_CompletedResults;
    mutable std::mutex m_ResultsMutex;
    
    std::atomic<uint64_t> m_NextRequestId{1};
    
    // Per-navmesh hierarchical data
    std::unordered_map<const NavMeshRuntime*, HierarchicalNavMesh> m_Hierarchies;
    std::mutex m_HierarchyMutex;
};

// Global pathfinder instance
NavPathfinder& GetPathfinder();

} // namespace nav

