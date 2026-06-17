#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include "core/navigation/NavTypes.h"
#include "core/ecs/Components.h"

class Scene;

namespace nav
{
    struct NavAgentComponent;
    struct NavMeshComponent;
    class NavMeshRuntime;

    class Navigation
    {
    public:
        static Navigation& Get();

        void Update(Scene& scene, float dt);

        // Global debug mask
        void SetDebugMask(NavDrawMask mask);
        NavDrawMask GetDebugMask() const;

        // C++ API wrappers
        bool FindPath(Scene& scene, uint32_t navMeshEntity, const glm::vec3& start, const glm::vec3& end,
                      const NavAgentParams& p, NavFlags include, NavFlags exclude, NavPath& out);
        bool Raycast(Scene& scene, uint32_t navMeshEntity, const glm::vec3& start, const glm::vec3& end, float& tHit, glm::vec3& hitNormal);
        bool NearestPoint(Scene& scene, uint32_t navMeshEntity, const glm::vec3& pos, float maxDist, glm::vec3& outOnMesh);

    private:
        Navigation() = default;
        NavDrawMask m_DebugMask = NavDrawMask::None;

        struct NavMeshEntry {
            EntityID entity = INVALID_ENTITY_ID;
            std::shared_ptr<NavMeshRuntime> runtime;
            Bounds bounds;
            int32_t domainId = 0;
            int32_t domainPriority = 0;
            bool autoPortalEnabled = true;
            float autoPortalMaxXZ = 0.5f;
            float autoPortalMaxHeight = 0.35f;
            float maxNormalAngleDeg = 45.0f;
            float agentPlacementOffset = 0.0f;
            bool fromChunkedPack = false;
            int32_t chunkX = 0;
            int32_t chunkZ = 0;
        };

        struct NavPortal {
            uint32_t from = 0;
            uint32_t to = 0;
            glm::vec3 fromPos{0.0f};
            glm::vec3 toPos{0.0f};
            float cost = 0.0f;
        };

        bool BuildNavMeshGraph(Scene& scene, bool loadRuntime);
        bool FindPathAcrossMeshes(Scene& scene, const glm::vec3& start, const glm::vec3& end,
                                  const NavAgentParams& p, NavFlags include, NavFlags exclude, NavPath& out,
                                  std::vector<EntityID>* outMeshChain = nullptr,
                                  bool ensureGraphBuilt = true);
        void RefreshEntityCaches(Scene& scene);

        struct PendingPathRequest {
            EntityID entityId = INVALID_ENTITY_ID;
            EntityID navMeshEntity = INVALID_ENTITY_ID;
            glm::vec3 position{0.0f};
            glm::vec3 destination{0.0f};
            NavAgentParams params{};
        };

        std::vector<NavMeshEntry> m_GraphMeshes;
        std::vector<std::vector<NavPortal>> m_GraphPortals;
        uint64_t m_GraphHash = 0;
        uint64_t m_LastObservedSceneRevision = 0;
        std::unordered_map<EntityID, uint64_t> m_ObservedStreamRevisions;
        uint64_t m_CachedEntityRevision = 0;
        size_t m_CachedEntityCount = 0;
        std::vector<EntityID> m_CachedNavAgentEntities;
        std::vector<EntityID> m_CachedNavMeshEntities;
        std::deque<PendingPathRequest> m_PendingPathRequests;
        std::unordered_set<EntityID> m_PendingPathRequestEntities;
        uint32_t m_MaxPathRequestsPerFrame = 32;
        uint32_t m_MaxAsyncPathRequestsPerFrame = 24;
        uint32_t m_MaxSyncPathSolvesPerFrame = 4;
    };
}


