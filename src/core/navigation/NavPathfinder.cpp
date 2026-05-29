// High-Performance Navigation Pathfinder Implementation
#include "core/navigation/NavPathfinder.h"
#include "core/navigation/NavMesh.h"
#include "core/navigation/NavQueries.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <iostream>

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace nav {

// ============================================================================
// SIMD-accelerated distance calculation
// ============================================================================

inline float FastDistance(const glm::vec3& a, const glm::vec3& b) {
    glm::vec3 d = a - b;
    return std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
}

inline float FastDistanceSq(const glm::vec3& a, const glm::vec3& b) {
    glm::vec3 d = a - b;
    return d.x*d.x + d.y*d.y + d.z*d.z;
}

inline float PointAABBDistSq(const glm::vec3& p, const Bounds& b) {
    float dx = std::max(std::max(b.min.x - p.x, 0.0f), p.x - b.max.x);
    float dy = std::max(std::max(b.min.y - p.y, 0.0f), p.y - b.max.y);
    float dz = std::max(std::max(b.min.z - p.z, 0.0f), p.z - b.max.z);
    return dx*dx + dy*dy + dz*dz;
}

inline glm::vec3 TriCenter(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    return (a + b + c) * (1.0f / 3.0f);
}

// ============================================================================
// Path Cache Implementation
// ============================================================================

static uint64_t HashParams(const NavAgentParams& params, NavFlags include, NavFlags exclude)
{
    uint64_t h = 14695981039346656037ULL;
    auto mixU32 = [&](uint32_t v) {
        h ^= v; h *= 1099511628211ULL;
    };
    auto mixF = [&](float v) {
        uint32_t u;
        memcpy(&u, &v, sizeof(u));
        mixU32(u);
    };
    mixF(params.radius);
    mixF(params.height);
    mixF(params.maxSlopeDeg);
    mixF(params.maxStep);
    mixF(params.maxSpeed);
    mixF(params.maxAccel);
    mixU32(static_cast<uint32_t>(params.preferredDomainId));
    mixU32(include.mask);
    mixU32(exclude.mask);
    return h;
}

static void AtomicAdd(std::atomic<double>& target, double delta)
{
    double current = target.load(std::memory_order_relaxed);
    while (!target.compare_exchange_weak(
        current,
        current + delta,
        std::memory_order_relaxed,
        std::memory_order_relaxed)) {
    }
}

uint64_t PathCache::HashPosition(const glm::vec3& pos) const {
    // Spatial hash - quantize position to grid cells
    int32_t x = static_cast<int32_t>(std::floor(pos.x / m_CellSize));
    int32_t y = static_cast<int32_t>(std::floor(pos.y / m_CellSize));
    int32_t z = static_cast<int32_t>(std::floor(pos.z / m_CellSize));
    
    // FNV-1a hash
    uint64_t h = 14695981039346656037ULL;
    h ^= static_cast<uint64_t>(x); h *= 1099511628211ULL;
    h ^= static_cast<uint64_t>(y); h *= 1099511628211ULL;
    h ^= static_cast<uint64_t>(z); h *= 1099511628211ULL;
    return h;
}

bool PathCache::Get(const NavMeshRuntime& navmesh, const glm::vec3& start, const glm::vec3& end,
                    const NavAgentParams& params, NavFlags include, NavFlags exclude, NavPath& outPath) {
    CacheKey key{
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&navmesh)),
        HashParams(params, include, exclude),
        HashPosition(start),
        HashPosition(end)
    };
    
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Cache.find(key);
    if (it != m_Cache.end()) {
        outPath = it->second.path;
        it->second.accessTime = m_AccessCounter++;
        m_Hits++;
        return true;
    }
    m_Misses++;
    return false;
}

void PathCache::Put(const NavMeshRuntime& navmesh, const glm::vec3& start, const glm::vec3& end,
                    const NavAgentParams& params, NavFlags include, NavFlags exclude, const NavPath& path) {
    CacheKey key{
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&navmesh)),
        HashParams(params, include, exclude),
        HashPosition(start),
        HashPosition(end)
    };
    
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Cache.size() >= m_MaxEntries) {
        EvictOldest();
    }
    m_Cache[key] = {path, m_AccessCounter++};
}

void PathCache::Clear() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Cache.clear();
}

void PathCache::SetMaxEntries(size_t maxEntries) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_MaxEntries = std::max<size_t>(1, maxEntries);
    while (m_Cache.size() > m_MaxEntries) {
        EvictOldest();
    }
}

void PathCache::EvictOldest() {
    // Find entry with lowest access time (LRU)
    auto oldest = m_Cache.begin();
    uint64_t oldestTime = UINT64_MAX;
    for (auto it = m_Cache.begin(); it != m_Cache.end(); ++it) {
        if (it->second.accessTime < oldestTime) {
            oldestTime = it->second.accessTime;
            oldest = it;
        }
    }
    if (oldest != m_Cache.end()) {
        m_Cache.erase(oldest);
    }
}

// ============================================================================
// NavPathfinder Implementation
// ============================================================================

NavPathfinder::NavPathfinder() {}

NavPathfinder::~NavPathfinder() {
    Shutdown();
}

void NavPathfinder::Initialize(const Config& config) {
    m_Config = config;
    m_Cache.SetMaxEntries(config.maxCacheEntries);
    m_Cache.SetCellSize(config.cacheCellSize);
    
    // Determine worker thread count
    uint32_t numWorkers = config.numWorkerThreads;
    if (numWorkers == 0) {
        numWorkers = std::max(1u, std::thread::hardware_concurrency() - 1);
    }
    
    m_Running = true;
    
    // Spawn worker threads
    m_Workers.reserve(numWorkers);
    for (uint32_t i = 0; i < numWorkers; ++i) {
        m_Workers.emplace_back(&NavPathfinder::WorkerThread, this);
    }
    
    std::cout << "[NavPathfinder] Initialized with " << numWorkers << " worker threads" << std::endl;
}

void NavPathfinder::Shutdown() {
    m_Running = false;
    m_QueueCV.notify_all();
    
    for (auto& t : m_Workers) {
        if (t.joinable()) t.join();
    }
    m_Workers.clear();
}

void NavPathfinder::WorkerThread() {
    while (m_Running) {
        PathRequest request;
        
        {
            std::unique_lock<std::mutex> lock(m_QueueMutex);
            m_QueueCV.wait(lock, [this]{ return !m_RequestQueue.empty() || !m_Running; });
            
            if (!m_Running && m_RequestQueue.empty()) break;
            if (m_RequestQueue.empty()) continue;
            
            request = std::move(m_RequestQueue.front());
            m_RequestQueue.pop();
        }
        
        // Execute pathfinding
        auto startTime = std::chrono::high_resolution_clock::now();
        
        PathResult result;
        result.requestId = request.requestId;
        result.success = FindPath(*request.navmesh, request.start, request.end,
                                   request.params, request.includeFlags, request.excludeFlags,
                                   result.path);
        result.navmeshPtr = request.navmeshPtr;
        
        auto endTime = std::chrono::high_resolution_clock::now();
        result.computeTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();
        
        // Pass through userData
        result.userData = request.userData;
        
        // Store result for main thread processing
        {
            std::lock_guard<std::mutex> lock(m_ResultsMutex);
            m_CompletedResults.push_back(std::move(result));
        }
        
        // NOTE: Callback is NOT called from worker thread anymore
        // Results should be processed on main thread via ProcessCompletedRequests()
        
        m_Stats.completedRequests++;
        AtomicAdd(m_Stats.totalComputeTimeMs, static_cast<double>(result.computeTimeMs));
    }
}

bool NavPathfinder::FindPath(const NavMeshRuntime& navmesh, const glm::vec3& start, const glm::vec3& end,
                              const NavAgentParams& params, NavFlags include, NavFlags exclude, NavPath& outPath) {
    m_Stats.totalRequests++;
    
    // Check cache first
    if (m_Cache.Get(navmesh, start, end, params, include, exclude, outPath)) {
        m_Stats.cachedHits++;
        return outPath.valid;
    }
    
    bool success = false;
    
    if (!navmesh.HasDetour()) {
        std::cerr << "[Nav][PathDiag] FindPath aborted: pathfinder requires Detour runtime." << std::endl;
        return false;
    }

    // Preferred path: Detour query pipeline.
    success = queries::FindPath(navmesh, start, end, params, include, exclude, outPath);
    if (success) m_Stats.directPaths++;
    
    // Cache the result
    if (success) {
        m_Cache.Put(navmesh, start, end, params, include, exclude, outPath);
    }
    
    return success;
}

uint64_t NavPathfinder::RequestPathAsync(std::shared_ptr<NavMeshRuntime> navmesh,
                                          const glm::vec3& start, const glm::vec3& end,
                                          const NavAgentParams& params, NavFlags include, NavFlags exclude,
                                          std::function<void(const NavPath&, bool)> callback,
                                          uint64_t userData) {
    PathRequest request;
    request.requestId = m_NextRequestId++;
    request.start = start;
    request.end = end;
    request.params = params;
    request.includeFlags = include;
    request.excludeFlags = exclude;
    request.navmesh = navmesh;
    request.navmeshPtr = navmesh.get();
    request.callback = callback;
    request.userData = userData;
    
    {
        std::lock_guard<std::mutex> lock(m_QueueMutex);
        m_RequestQueue.push(std::move(request));
    }
    m_QueueCV.notify_one();
    
    return request.requestId;
}

void NavPathfinder::CancelRequest(uint64_t requestId) {
    std::lock_guard<std::mutex> lock(m_QueueMutex);
    // Note: Simple implementation - in production would use a more efficient data structure
    std::queue<PathRequest> filtered;
    while (!m_RequestQueue.empty()) {
        auto req = std::move(m_RequestQueue.front());
        m_RequestQueue.pop();
        if (req.requestId != requestId) {
            filtered.push(std::move(req));
        }
    }
    m_RequestQueue = std::move(filtered);
}

std::vector<PathResult> NavPathfinder::ProcessCompletedRequests() {
    std::vector<PathResult> results;
    {
        std::lock_guard<std::mutex> lock(m_ResultsMutex);
        results = std::move(m_CompletedResults);
        m_CompletedResults.clear();
    }
    return results;
}

size_t NavPathfinder::GetPendingRequestCount() const {
    std::lock_guard<std::mutex> lock(m_QueueMutex);
    return m_RequestQueue.size();
}

size_t NavPathfinder::GetCompletedResultCount() const {
    std::lock_guard<std::mutex> lock(m_ResultsMutex);
    return m_CompletedResults.size();
}

void NavPathfinder::ResetStats() {
    m_Stats.totalRequests = 0;
    m_Stats.completedRequests = 0;
    m_Stats.cachedHits = 0;
    m_Stats.totalComputeTimeMs = 0;
    m_Stats.hierarchicalPaths = 0;
    m_Stats.directPaths = 0;
}

// ============================================================================
// BVH-accelerated nearest polygon search
// ============================================================================

uint32_t NavPathfinder::FindNearestPoly(const NavMeshRuntime& nm, const glm::vec3& pos) {
    if (nm.m_Polys.empty()) return 0;
    
    if (nm.m_BVH.empty()) {
        // Fallback: linear search
        float best = FLT_MAX;
        uint32_t bestIdx = 0;
        for (uint32_t i = 0; i < nm.m_Polys.size(); ++i) {
            const auto& p = nm.m_Polys[i];
            if (p.i0 >= nm.m_Vertices.size()) continue;
            glm::vec3 c = TriCenter(nm.m_Vertices[p.i0], nm.m_Vertices[p.i1], nm.m_Vertices[p.i2]);
            float d = FastDistanceSq(pos, c);
            if (d < best) { best = d; bestIdx = i; }
        }
        return bestIdx;
    }
    
    // BVH-accelerated search
    struct Entry { uint32_t nodeIdx; float distSq; };
    auto cmp = [](const Entry& a, const Entry& b) { return a.distSq > b.distSq; };
    std::priority_queue<Entry, std::vector<Entry>, decltype(cmp)> pq(cmp);
    
    float bestDistSq = FLT_MAX;
    uint32_t bestIdx = 0;
    
    pq.push({0, PointAABBDistSq(pos, nm.m_BVH[0].b)});
    
    while (!pq.empty()) {
        Entry e = pq.top(); pq.pop();
        if (e.distSq > bestDistSq) continue;
        
        const auto& node = nm.m_BVH[e.nodeIdx];
        
        if (node.count > 0) {
            // Leaf
            for (uint32_t i = node.start; i < node.start + node.count && i < nm.m_BVHIndices.size(); ++i) {
                uint32_t polyIdx = nm.m_BVHIndices[i];
                if (polyIdx >= nm.m_Polys.size()) continue;
                const auto& p = nm.m_Polys[polyIdx];
                if (p.i0 >= nm.m_Vertices.size()) continue;
                glm::vec3 c = TriCenter(nm.m_Vertices[p.i0], nm.m_Vertices[p.i1], nm.m_Vertices[p.i2]);
                float d = FastDistanceSq(pos, c);
                if (d < bestDistSq) { bestDistSq = d; bestIdx = polyIdx; }
            }
        } else {
            if (node.left < nm.m_BVH.size()) {
                float ld = PointAABBDistSq(pos, nm.m_BVH[node.left].b);
                if (ld < bestDistSq) pq.push({node.left, ld});
            }
            if (node.right < nm.m_BVH.size()) {
                float rd = PointAABBDistSq(pos, nm.m_BVH[node.right].b);
                if (rd < bestDistSq) pq.push({node.right, rd});
            }
        }
    }
    
    return bestIdx;
}

// ============================================================================
// Direct A* Pathfinding (for smaller meshes)
// ============================================================================

bool NavPathfinder::FindPathDirect(const NavMeshRuntime& nm, const glm::vec3& start, const glm::vec3& end,
                                    const NavAgentParams& params, NavPath& outPath) {
    outPath.points.clear();
    outPath.valid = false;
    
    if (nm.m_Polys.empty()) {
        std::cerr << "[Nav] FindPathDirect: No polygons in navmesh" << std::endl;
        return false;
    }
    if (nm.m_Adjacency.empty()) {
        std::cerr << "[Nav] FindPathDirect: No adjacency data - pathfinding impossible" << std::endl;
        return false;
    }
    
    uint32_t startPoly = FindNearestPoly(nm, start);
    uint32_t endPoly = FindNearestPoly(nm, end);
    
    // Validate polygon indices
    if (startPoly >= nm.m_Polys.size()) {
        std::cerr << "[Nav] FindPathDirect: Invalid startPoly=" << startPoly << " (polyCount=" << nm.m_Polys.size() << ")" << std::endl;
        return false;
    }
    if (endPoly >= nm.m_Polys.size()) {
        std::cerr << "[Nav] FindPathDirect: Invalid endPoly=" << endPoly << " (polyCount=" << nm.m_Polys.size() << ")" << std::endl;
        return false;
    }
    
    // Check adjacency for start/end polys
    size_t startNeighbors = nm.m_Adjacency[startPoly].size();
    size_t endNeighbors = nm.m_Adjacency[endPoly].size();
    
    if (startPoly == endPoly) {
        // Trivial path
        outPath.points = {start, end};
        outPath.valid = true;
        return true;
    }
    
    // Use bidirectional search if enabled
    if (m_Config.useBidirectional) {
        bool result = FindPathBidirectional(nm, startPoly, endPoly, outPath);
        if (!result) {
            std::cerr << "[Nav] FindPathDirect(bidirectional) FAILED: startPoly=" << startPoly 
                      << " endPoly=" << endPoly << " startNeighbors=" << startNeighbors 
                      << " endNeighbors=" << endNeighbors << std::endl;
        }
        return result;
    }
    
    // Standard A*
    struct Node {
        float g, f;
        uint32_t parent;
        bool hasParent;
        bool closed;
    };
    
    std::vector<Node> nodes(nm.m_Polys.size(), {FLT_MAX, FLT_MAX, 0, false, false});
    
    auto getCenter = [&](uint32_t i) -> glm::vec3 {
        const auto& p = nm.m_Polys[i];
        return TriCenter(nm.m_Vertices[p.i0], nm.m_Vertices[p.i1], nm.m_Vertices[p.i2]);
    };
    
    glm::vec3 endCenter = getCenter(endPoly);
    
    auto heuristic = [&](uint32_t i) {
        return FastDistance(getCenter(i), endCenter);
    };
    
    struct PQEntry { uint32_t idx; float f; };
    auto cmp = [](const PQEntry& a, const PQEntry& b) { return a.f > b.f; };
    std::priority_queue<PQEntry, std::vector<PQEntry>, decltype(cmp)> openSet(cmp);
    
    nodes[startPoly] = {0.0f, heuristic(startPoly), 0, false, false};
    openSet.push({startPoly, nodes[startPoly].f});
    
    uint32_t iterations = 0;
    const uint32_t maxIter = m_Config.maxIterationsPerPath;
    
    while (!openSet.empty() && iterations < maxIter) {
        iterations++;
        
        PQEntry current = openSet.top();
        openSet.pop();
        
        if (nodes[current.idx].closed) continue;
        nodes[current.idx].closed = true;
        
        if (current.idx == endPoly) {
            // Reconstruct path
            std::vector<uint32_t> polyPath;
            uint32_t at = endPoly;
            while (true) {
                polyPath.push_back(at);
                if (!nodes[at].hasParent) break;
                at = nodes[at].parent;
            }
            std::reverse(polyPath.begin(), polyPath.end());
            
            outPath.points.reserve(polyPath.size() + 2);
            outPath.points.push_back(start);
            for (uint32_t p : polyPath) {
                outPath.points.push_back(getCenter(p));
            }
            outPath.points.push_back(end);
            outPath.valid = true;
            return true;
        }
        
        if (current.idx >= nm.m_Adjacency.size()) continue;
        
        for (uint32_t neighbor : nm.m_Adjacency[current.idx]) {
            if (neighbor >= nm.m_Polys.size() || nodes[neighbor].closed) continue;
            
            const float polyCost = std::max(nm.m_Polys[neighbor].cost, 0.01f);
            const float stepCost = FastDistance(getCenter(current.idx), getCenter(neighbor)) * polyCost;
            float tentativeG = nodes[current.idx].g + stepCost;
            
            if (tentativeG < nodes[neighbor].g) {
                nodes[neighbor].g = tentativeG;
                nodes[neighbor].f = tentativeG + heuristic(neighbor);
                nodes[neighbor].parent = current.idx;
                nodes[neighbor].hasParent = true;
                openSet.push({neighbor, nodes[neighbor].f});
            }
        }
    }
    
    // Path not found - log diagnostic info
    std::cerr << "[Nav] FindPathDirect FAILED: iterations=" << iterations 
              << " startPoly=" << startPoly << " endPoly=" << endPoly 
              << " startNeighbors=" << startNeighbors << " endNeighbors=" << endNeighbors 
              << " openSetEmpty=" << openSet.empty() << std::endl;
    return false;
}

// ============================================================================
// Bidirectional A* (searches from both ends, meets in middle)
// ============================================================================

bool NavPathfinder::FindPathBidirectional(const NavMeshRuntime& nm, uint32_t startPoly, uint32_t endPoly,
                                           NavPath& outPath) {
    if (startPoly == endPoly) {
        const auto& p = nm.m_Polys[startPoly];
        glm::vec3 c = TriCenter(nm.m_Vertices[p.i0], nm.m_Vertices[p.i1], nm.m_Vertices[p.i2]);
        outPath.points = {c};
        outPath.valid = true;
        return true;
    }
    
    struct Node {
        float g;
        uint32_t parent;
        bool hasParent;
        int8_t direction; // 0 = none, 1 = forward, -1 = backward
    };
    
    std::vector<Node> nodes(nm.m_Polys.size(), {FLT_MAX, 0, false, 0});
    
    auto getCenter = [&](uint32_t i) -> glm::vec3 {
        const auto& p = nm.m_Polys[i];
        return TriCenter(nm.m_Vertices[p.i0], nm.m_Vertices[p.i1], nm.m_Vertices[p.i2]);
    };
    
    glm::vec3 startCenter = getCenter(startPoly);
    glm::vec3 endCenter = getCenter(endPoly);
    
    struct PQEntry { uint32_t idx; float f; int8_t dir; };
    auto cmp = [](const PQEntry& a, const PQEntry& b) { return a.f > b.f; };
    std::priority_queue<PQEntry, std::vector<PQEntry>, decltype(cmp)> openSet(cmp);
    
    // Initialize both directions
    nodes[startPoly] = {0.0f, 0, false, 1};
    nodes[endPoly] = {0.0f, 0, false, -1};
    
    float hStart = FastDistance(startCenter, endCenter);
    openSet.push({startPoly, hStart, 1});
    openSet.push({endPoly, hStart, -1});
    
    uint32_t meetingPoly = UINT32_MAX;
    float bestPathCost = FLT_MAX;
    
    uint32_t iterations = 0;
    const uint32_t maxIter = m_Config.maxIterationsPerPath;
    
    while (!openSet.empty() && iterations < maxIter) {
        iterations++;
        
        PQEntry current = openSet.top();
        openSet.pop();
        
        // Check if we've found a meeting point
        if (nodes[current.idx].direction != 0 && nodes[current.idx].direction != current.dir) {
            // Paths have met!
            float pathCost = nodes[current.idx].g + current.f - FastDistance(getCenter(current.idx), 
                current.dir > 0 ? endCenter : startCenter);
            if (pathCost < bestPathCost) {
                bestPathCost = pathCost;
                meetingPoly = current.idx;
            }
            continue;
        }
        
        if (nodes[current.idx].direction == current.dir) continue; // Already processed
        nodes[current.idx].direction = current.dir;
        
        if (current.idx >= nm.m_Adjacency.size()) continue;
        
        for (uint32_t neighbor : nm.m_Adjacency[current.idx]) {
            if (neighbor >= nm.m_Polys.size()) continue;
            
            const float polyCost = std::max(nm.m_Polys[neighbor].cost, 0.01f);
            float stepCost = FastDistance(getCenter(current.idx), getCenter(neighbor)) * polyCost;
            float tentativeG = nodes[current.idx].g + stepCost;
            
            if (nodes[neighbor].direction == -current.dir) {
                // Found connection to other direction!
                float pathCost = tentativeG + nodes[neighbor].g;
                if (pathCost < bestPathCost) {
                    bestPathCost = pathCost;
                    meetingPoly = neighbor;
                }
            }
            
            if (nodes[neighbor].direction == 0 || tentativeG < nodes[neighbor].g) {
                nodes[neighbor].g = tentativeG;
                nodes[neighbor].parent = current.idx;
                nodes[neighbor].hasParent = true;
                
                glm::vec3 target = current.dir > 0 ? endCenter : startCenter;
                float h = FastDistance(getCenter(neighbor), target);
                openSet.push({neighbor, tentativeG + h, current.dir});
            }
        }
    }
    
    if (meetingPoly == UINT32_MAX) return false;
    
    // Reconstruct path from both directions
    std::vector<uint32_t> forwardPath, backwardPath;
    
    // Forward path (start -> meeting)
    uint32_t at = meetingPoly;
    while (nodes[at].direction == 1 && nodes[at].hasParent) {
        forwardPath.push_back(at);
        at = nodes[at].parent;
    }
    forwardPath.push_back(startPoly);
    std::reverse(forwardPath.begin(), forwardPath.end());
    
    // Backward path (meeting -> end)
    at = meetingPoly;
    while (nodes[at].direction == -1 && nodes[at].hasParent) {
        if (at != meetingPoly) backwardPath.push_back(at);
        at = nodes[at].parent;
    }
    if (at != meetingPoly) backwardPath.push_back(endPoly);
    
    // Combine paths
    outPath.points.reserve(forwardPath.size() + backwardPath.size() + 2);
    outPath.points.push_back(getCenter(startPoly));
    for (uint32_t p : forwardPath) outPath.points.push_back(getCenter(p));
    for (uint32_t p : backwardPath) outPath.points.push_back(getCenter(p));
    outPath.points.push_back(getCenter(endPoly));
    outPath.valid = true;
    
    return true;
}

// ============================================================================
// Hierarchical Pathfinding
// ============================================================================

void NavPathfinder::BuildHierarchy(NavMeshRuntime& navmesh) {
    std::lock_guard<std::mutex> lock(m_HierarchyMutex);
    
    auto& hierarchy = m_Hierarchies[&navmesh];
    
    // Partition into regions
    PartitionIntoRegions(navmesh, m_Config.regionSize);
    
    // Build high-level graph
    BuildRegionGraph(navmesh);
    
    std::cout << "[NavPathfinder] Built hierarchy with " << hierarchy.regions.size() << " regions" << std::endl;
}

void NavPathfinder::PartitionIntoRegions(NavMeshRuntime& nm, uint32_t targetRegionSize) {
    auto& hierarchy = m_Hierarchies[&nm];
    hierarchy.regions.clear();
    hierarchy.polyToRegion.clear();
    hierarchy.polyToRegion.resize(nm.m_Polys.size());
    
    std::vector<bool> assigned(nm.m_Polys.size(), false);
    uint32_t regionId = 0;
    
    auto getCenter = [&](uint32_t i) -> glm::vec3 {
        const auto& p = nm.m_Polys[i];
        return TriCenter(nm.m_Vertices[p.i0], nm.m_Vertices[p.i1], nm.m_Vertices[p.i2]);
    };
    
    // Simple flood-fill partitioning
    for (uint32_t seed = 0; seed < nm.m_Polys.size(); ++seed) {
        if (assigned[seed]) continue;
        
        NavRegion region;
        region.id = regionId++;
        region.bounds.min = glm::vec3(FLT_MAX);
        region.bounds.max = glm::vec3(-FLT_MAX);
        
        std::queue<uint32_t> queue;
        queue.push(seed);
        assigned[seed] = true;
        
        while (!queue.empty() && region.polygons.size() < targetRegionSize) {
            uint32_t current = queue.front();
            queue.pop();
            
            region.polygons.push_back(current);
            hierarchy.polyToRegion[current] = {region.id};
            
            glm::vec3 c = getCenter(current);
            region.bounds.expand(c);
            
            if (current < nm.m_Adjacency.size()) {
                for (uint32_t neighbor : nm.m_Adjacency[current]) {
                    if (neighbor < nm.m_Polys.size() && !assigned[neighbor]) {
                        assigned[neighbor] = true;
                        queue.push(neighbor);
                    }
                }
            }
        }
        
        // Compute region center
        glm::vec3 sum(0);
        for (uint32_t p : region.polygons) sum += getCenter(p);
        region.center = sum / (float)region.polygons.size();
        
        hierarchy.regions.push_back(std::move(region));
    }
}

void NavPathfinder::BuildRegionGraph(NavMeshRuntime& nm) {
    auto& hierarchy = m_Hierarchies[&nm];
    hierarchy.regionGraph.resize(hierarchy.regions.size());
    
    // Find border polygons and region connections
    for (auto& region : hierarchy.regions) {
        region.borderPolys.clear();
        region.neighbors.clear();
        
        for (uint32_t polyIdx : region.polygons) {
            if (polyIdx >= nm.m_Adjacency.size()) continue;
            
            for (uint32_t neighbor : nm.m_Adjacency[polyIdx]) {
                if (neighbor >= hierarchy.polyToRegion.size()) continue;
                
                for (uint32_t neighborRegion : hierarchy.polyToRegion[neighbor]) {
                    if (neighborRegion != region.id) {
                        // This is a border polygon
                        region.borderPolys.push_back(polyIdx);
                        
                        // Add neighbor region if not already added
                        if (std::find(region.neighbors.begin(), region.neighbors.end(), neighborRegion) == region.neighbors.end()) {
                            region.neighbors.push_back(neighborRegion);
                            
                            // Add edge to region graph
                            float cost = FastDistance(region.center, hierarchy.regions[neighborRegion].center);
                            hierarchy.regionGraph[region.id].push_back({neighborRegion, cost, polyIdx});
                        }
                        break;
                    }
                }
            }
        }
    }
}

uint32_t NavPathfinder::FindRegionForPoly(const NavMeshRuntime& nm, uint32_t polyIdx) {
    std::lock_guard<std::mutex> lock(m_HierarchyMutex);
    
    auto it = m_Hierarchies.find(&nm);
    if (it == m_Hierarchies.end() || polyIdx >= it->second.polyToRegion.size()) {
        return UINT32_MAX;
    }
    
    const auto& regions = it->second.polyToRegion[polyIdx];
    return regions.empty() ? UINT32_MAX : regions[0];
}

bool NavPathfinder::FindPathHierarchical(const NavMeshRuntime& nm, const glm::vec3& start, const glm::vec3& end,
                                          const NavAgentParams& params, NavPath& outPath) {
    std::lock_guard<std::mutex> lock(m_HierarchyMutex);
    
    auto it = m_Hierarchies.find(&nm);
    if (it == m_Hierarchies.end() || it->second.regions.empty()) {
        // Hierarchy not built, fall back to direct
        return FindPathDirect(nm, start, end, params, outPath);
    }
    
    const auto& hierarchy = it->second;
    
    // Find start and end polygons/regions
    uint32_t startPoly = FindNearestPoly(nm, start);
    uint32_t endPoly = FindNearestPoly(nm, end);
    
    if (startPoly >= hierarchy.polyToRegion.size() || endPoly >= hierarchy.polyToRegion.size()) {
        return FindPathDirect(nm, start, end, params, outPath);
    }
    
    uint32_t startRegion = hierarchy.polyToRegion[startPoly].empty() ? UINT32_MAX : hierarchy.polyToRegion[startPoly][0];
    uint32_t endRegion = hierarchy.polyToRegion[endPoly].empty() ? UINT32_MAX : hierarchy.polyToRegion[endPoly][0];
    
    if (startRegion == UINT32_MAX || endRegion == UINT32_MAX) {
        return FindPathDirect(nm, start, end, params, outPath);
    }
    
    // Same region? Direct path
    if (startRegion == endRegion) {
        return FindPathDirect(nm, start, end, params, outPath);
    }
    
    // A* on region graph
    struct RegionNode { float g, f; uint32_t parent; bool hasParent; bool closed; };
    std::vector<RegionNode> regionNodes(hierarchy.regions.size(), {FLT_MAX, FLT_MAX, 0, false, false});
    
    auto regionHeuristic = [&](uint32_t r) {
        return FastDistance(hierarchy.regions[r].center, hierarchy.regions[endRegion].center);
    };
    
    struct PQEntry { uint32_t region; float f; };
    auto cmp = [](const PQEntry& a, const PQEntry& b) { return a.f > b.f; };
    std::priority_queue<PQEntry, std::vector<PQEntry>, decltype(cmp)> openSet(cmp);
    
    regionNodes[startRegion] = {0.0f, regionHeuristic(startRegion), 0, false, false};
    openSet.push({startRegion, regionNodes[startRegion].f});
    
    while (!openSet.empty()) {
        PQEntry current = openSet.top();
        openSet.pop();
        
        if (regionNodes[current.region].closed) continue;
        regionNodes[current.region].closed = true;
        
        if (current.region == endRegion) {
            // Reconstruct region path
            std::vector<uint32_t> regionPath;
            uint32_t at = endRegion;
            while (true) {
                regionPath.push_back(at);
                if (!regionNodes[at].hasParent) break;
                at = regionNodes[at].parent;
            }
            std::reverse(regionPath.begin(), regionPath.end());
            
            // Now path through each region
            outPath.points.clear();
            outPath.points.push_back(start);
            
            glm::vec3 currentPos = start;
            for (size_t i = 0; i < regionPath.size() - 1; ++i) {
                // Find path to border of current region
                // For simplicity, just add region centers
                outPath.points.push_back(hierarchy.regions[regionPath[i]].center);
            }
            outPath.points.push_back(end);
            outPath.valid = true;
            return true;
        }
        
        if (current.region >= hierarchy.regionGraph.size()) continue;
        
        for (const auto& edge : hierarchy.regionGraph[current.region]) {
            if (edge.targetRegion >= hierarchy.regions.size() || regionNodes[edge.targetRegion].closed) continue;
            
            float tentativeG = regionNodes[current.region].g + edge.cost;
            
            if (tentativeG < regionNodes[edge.targetRegion].g) {
                regionNodes[edge.targetRegion].g = tentativeG;
                regionNodes[edge.targetRegion].f = tentativeG + regionHeuristic(edge.targetRegion);
                regionNodes[edge.targetRegion].parent = current.region;
                regionNodes[edge.targetRegion].hasParent = true;
                openSet.push({edge.targetRegion, regionNodes[edge.targetRegion].f});
            }
        }
    }
    
    // No path found at region level
    return false;
}

// ============================================================================
// Global Instance
// ============================================================================

NavPathfinder& GetPathfinder() {
    static NavPathfinder instance;
    static bool initialized = false;
    if (!initialized) {
        instance.Initialize();
        initialized = true;
    }
    return instance;
}

} // namespace nav

