#pragma once

#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <limits>
#include <cstddef>
#include <shared_mutex>
#include <atomic>
#include <string>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <deque>
#include <glm/glm.hpp>
#include "NavTypes.h"
#include "core/assets/AssetReference.h"
#include "core/ecs/Entity.h"

class Scene;
class Mesh;
struct dtNavMesh;
struct dtNavMeshQuery;

using EntityID = uint32_t;

namespace nav
{
    class NavMeshRuntime;
    class NavChunkManager;
    enum class NavChunkingMode : uint8_t
    {
        Terrain = 0,
        WorldGrid = 1
    };

    struct OffMeshLink
    {
        glm::vec3 a{0};
        glm::vec3 b{0};
        float radius = 0.5f;
        float cost = 1.0f; // Cost multiplier for traversing this link
        uint32_t flags = 0;
        uint8_t bidir = 1; // 1 = bidirectional
    };

    struct NavLinkComponent
    {
        bool Enabled = true;
        glm::vec3 Start{0.0f};
        glm::vec3 End{0.0f};
        float Radius = 0.5f;
        float Cost = 1.0f; // Cost multiplier for link traversal
        uint32_t Flags = 0;
        bool Bidirectional = true;
        bool UseWorldSpace = false; // If true, Start/End are in world space
    };

    // ECS component: authoring and baked data reference
    struct NavMeshComponent
    {
        bool Enabled = true;
        NavBakeSettings Bake;
        Bounds AABB;
        uint64_t BakeHash = 0;
        std::shared_ptr<NavMeshRuntime> Runtime;
        std::shared_ptr<NavChunkManager> ChunkManager;

        // Persistent asset storage (similar to terrain)
        ClaymoreGUID NavMeshDataGuid;         // Stable identifier for the navmesh binary
        std::string AssetPath;                // Path to .navbin file (e.g., .bin/nav/navmesh_{guid}.navbin)
        bool AssetDirty = false;              // True if navmesh needs to be saved
        bool LoadAttempted = false;           // Prevent repeated load attempts if failed

        // Terrain-specific settings
        uint32_t TerrainSampleStep = 2;       // Step size for terrain sampling (1=full res, 2=half res, etc.)
        bool GeometryIncludeRegexEnabled = false; // If true, include only mesh entities whose names match regex
        std::string GeometryIncludeRegexPattern; // ECMAScript regex used against EntityData::Name (case-insensitive)
        bool BakeVisibleChunksOnly = false;   // If true, bake only visible terrain chunks
        uint32_t BakeVisibleChunkPadding = 0; // Extra chunk padding for visible-only bake
        bool BakeMissingChunksOnly = false;   // If true, skip chunks that already have baked payload in navpack
        bool ChunkedNavEnabled = false;       // If true, bake + stream per chunk
        NavChunkingMode ChunkingMode = NavChunkingMode::Terrain; // Chunking scheme
        float ChunkWorldSize = 64.0f;         // World grid chunk size (meters)
        uint32_t ChunkBakePadding = 1;        // Extra chunk padding for per-chunk bake
        float ChunkStreamRadius = 300.0f;     // Runtime stream radius for chunk navs
        std::string NavPackPath;              // Scene-level navpack container path

        // Stitching / connectivity settings
        bool EnableStitching = false;
        float StitchEpsilon = 0.05f;          // Quantization epsilon for edge matching
        float StitchMaxNormalAngleDeg = 45.0f;// Max normal angle difference to connect
        float StitchMaxHeight = 0.35f;        // Max height gap for terrain bridging
        float StitchMaxXZ = 0.5f;             // Max horizontal gap for terrain bridging

        // Multi-navmesh domains + auto portals
        int32_t DomainId = 0;                 // Domain identifier for cross-mesh pathing
        int32_t DomainPriority = 0;           // Higher priority domains are preferred
        bool AutoPortalEnabled = false;       // Auto-generate portals to nearby navmeshes
        float AutoPortalMaxXZ = 0.5f;         // Max horizontal gap for auto portals
        float AutoPortalMaxHeight = 0.35f;    // Max height gap for auto portals

        // Bake diagnostics (runtime)
        std::atomic<float> LastBakeCostMin{1.0f};
        std::atomic<float> LastBakeCostMax{1.0f};
        std::atomic<uint32_t> LastBakeIslands{0};
        std::atomic<uint32_t> LastBakeRemovedIslands{0};

        // Debug visualization
        float DebugDrawOffset = 0.15f;        // Vertical offset for debug rendering (above terrain)
        float AgentPlacementOffset = 0.0f;    // Positive lowers agents below the sampled navmesh surface
        bool CostAwareSmoothing = true;       // Use cost-aware smoothing during pathfinding

        // baking state (async)
        std::atomic<bool> Baking{false};
        std::atomic<float> BakingProgress{0.0f};
        std::atomic<bool> BakingCancel{false};
        std::atomic<uint32_t> BakingStage{0};

        uint64_t ComputeBakeHash(Scene& scene) const;
        // Returns all scene entities with valid mesh geometry.
        void GetEffectiveSources(Scene& scene, std::vector<EntityID>& out) const;
        // Returns all scene entities with terrain geometry.
        void GetTerrainSources(Scene& scene, std::vector<EntityID>& out) const;
        void RequestBake(Scene& scene);
        void CancelBake();
        bool IsBaking() const { return Baking.load(); }
        float BakeProgress() const { return BakingProgress.load(); }
        bool EnsureRuntimeLoaded();
        bool EnsureRuntimeLoaded(Scene& scene);

        // Asset path helpers
        static std::string BuildDefaultAssetPath(const ClaymoreGUID& guid);
        void EnsureGuid();  // Generate GUID if not set
    };

    // Runtime built from navbin
    class NavMeshRuntime
    {
    public:
        // Polygon is just a triangle for now (indices into m_Vertices)
        struct Poly { uint32_t i0, i1, i2; uint16_t area; uint32_t flags; float cost = 1.0f; };

        struct LinkAdjacency
        {
            uint32_t to = 0;
            glm::vec3 from{0.0f};
            glm::vec3 toPos{0.0f};
            float cost = 1.0f;
            uint32_t flags = 0;
        };

        // Adjacency by poly index -> neighboring polys that share an edge
        std::vector<std::vector<uint32_t>> m_Adjacency;

        // Geometry
        std::vector<glm::vec3> m_Vertices;
        std::vector<Poly> m_Polys;
        std::vector<OffMeshLink> m_Links;
        std::vector<OffMeshLink> m_ExternalLinks;
        std::vector<std::vector<LinkAdjacency>> m_LinkAdjacency;

        // Accel structures
        struct BVNode { Bounds b; uint32_t left = UINT32_MAX, right = UINT32_MAX, start = 0, count = 0; };
        std::vector<BVNode> m_BVH;
        std::vector<uint32_t> m_BVHIndices; // triangle indices per leaf

        Bounds m_Bounds;

        // Area costs (per NavAreaId.value)
        std::array<float, 64> m_AreaCost{}; // default 1.0f
        bool UseCostAwareSmoothing = true;

        mutable std::shared_mutex m_Lock;
        std::vector<uint8_t> m_DetourNavData;
        ::dtNavMesh* m_DetourMesh = nullptr;
        mutable ::dtNavMeshQuery* m_DetourQuery = nullptr;

        NavMeshRuntime();
        ~NavMeshRuntime();
        NavMeshRuntime(const NavMeshRuntime&) = delete;
        NavMeshRuntime& operator=(const NavMeshRuntime&) = delete;

        bool FindPath(const glm::vec3& start, const glm::vec3& end, NavPath& out, const NavAgentParams& params, NavFlags include, NavFlags exclude) const;
        bool Raycast(const glm::vec3& start, const glm::vec3& end, float& tHit, glm::vec3& hitNormal) const;
        bool NearestPoint(const glm::vec3& pos, float maxDist, glm::vec3& outOnMesh) const;

        bool HasDetour() const;
        bool SetDetourNavData(const uint8_t* data, size_t size);
        bool SetDetourNavData(std::vector<uint8_t> data);
        bool InitDetourTileWorld(const glm::vec3& origin, float tileWidth, float tileHeight, int maxTiles, int maxPolysPerTile);
        bool AddDetourTileCopy(const uint8_t* data, size_t size, uint64_t& outTileRef,
                               int forcedTileX = std::numeric_limits<int>::min(),
                               int forcedTileY = std::numeric_limits<int>::min());
        bool RemoveDetourTile(uint64_t tileRef);
        void GetDetourDebugStats(uint32_t& outTiles, uint32_t& outPolys) const;
        const std::vector<uint8_t>& GetDetourNavData() const { return m_DetourNavData; }
        void ClearDetour();
        bool SyncLegacyGeometryFromDetour();

        void RebuildBVH();
        void RebuildLinkAdjacency();
        void SetExternalLinks(std::vector<OffMeshLink> links);
    };

    struct NavChunkKey {
        int32_t x = 0;
        int32_t z = 0;
        bool operator==(const NavChunkKey& o) const { return x == o.x && z == o.z; }
    };

    struct NavChunkRecord {
        NavChunkKey key;
        Bounds bounds;
        uint64_t hash = 0;
        uint64_t offset = 0;
        uint64_t size = 0;
        bool loaded = false;
        bool missingPayload = false;
        uint8_t loadFailStreak = 0; // Non-zero means last load failed; reset when chunk leaves wanted set.
        uint64_t detourTileRef = 0;
        std::shared_ptr<NavMeshRuntime> runtime;
    };

    class NavChunkManager
    {
    public:
        struct LoadedChunkRuntime {
            NavChunkKey key;
            Bounds bounds;
            std::shared_ptr<NavMeshRuntime> runtime;
        };

        bool LoadPack(std::string path);
        void Update(Scene& scene, NavMeshComponent& comp);
        std::shared_ptr<NavMeshRuntime> GetStitchedRuntime() const;
        uint64_t GetStreamRevision() const { return m_StreamRevision; }
        void GetLoadedChunkRuntimes(std::vector<LoadedChunkRuntime>& out) const;
        void MarkDirty() { m_StitchDirty = true; }
        ~NavChunkManager();

    private:
        struct PendingChunkLoad {
            uint64_t key = 0;
            std::string path;
            int32_t gridX = 0;
            int32_t gridZ = 0;
            Bounds bounds;
            uint64_t hash = 0;
            uint64_t offset = 0;
            uint64_t size = 0;
        };
        struct CompletedChunkLoad {
            uint64_t key = 0;
            bool success = false;
            uint64_t fileHash = 0;
            std::shared_ptr<NavMeshRuntime> runtime;
        };

        void StartLoadWorker();
        void StopLoadWorker();
        bool QueueChunkLoad(uint64_t key, const NavChunkRecord& rec);
        bool IsChunkLoadInFlight(uint64_t key);
        bool TryPopChunkLoadResult(CompletedChunkLoad& out);

        bool EnsureDetourTileWorld(const NavMeshComponent& comp);
        void UpdateDetourWorldBounds();
        void RebuildStitched(const NavMeshComponent& comp);
        std::string m_Path;
        uint64_t m_LastPackWriteTimeTicks = 0;
        uint32_t m_ChunksX = 0;
        uint32_t m_ChunksZ = 0;
        uint64_t m_BakeHash = 0;
        glm::vec3 m_DetourWorldOrigin{0.0f};
        float m_DetourTileWidth = 0.0f;
        float m_DetourTileHeight = 0.0f;
        bool m_DetourWorldReady = false;
        bool m_DetourWorldInitFailed = false;
        uint64_t m_StreamRevision = 0;
        std::unordered_map<uint64_t, NavChunkRecord> m_Chunks;
        std::shared_ptr<NavMeshRuntime> m_Stitched;
        bool m_StitchDirty = false;

        std::thread m_LoadWorker;
        std::mutex m_LoadMutex;
        std::condition_variable m_LoadCv;
        bool m_LoadWorkerRunning = false;
        bool m_LoadWorkerStop = false;
        std::deque<PendingChunkLoad> m_LoadQueue;
        std::deque<CompletedChunkLoad> m_LoadResults;
        std::unordered_set<uint64_t> m_LoadInFlight;
        mutable std::mutex m_StateMutex;
        glm::vec3 m_LastStreamCameraPos{0.0f};
        uint32_t m_StreamIdleFrameCounter = 0;
        size_t m_LoadedChunkCount = 0;
        Bounds m_LoadedBounds{};
        bool m_LoadedBoundsValid = false;
        bool m_LoadedBoundsDirty = false;
    };
}


