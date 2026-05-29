#pragma once

#include <glm/glm.hpp>
#include <memory>
#include "core/navigation/NavTypes.h"
#include "core/ecs/Components.h"
#include "core/physics/Physics.h"

using EntityID = uint32_t;

namespace nav
{
    struct NavMeshComponent;

    struct NavAgentComponent
    {
        bool Enabled = true;
        EntityID NavMeshEntity = 0;
        NavAgentParams Params;
        glm::vec3 Destination{ 0 };
        float ArriveThreshold = 0.15f;
        float RepathInterval = 0.5f;
        bool AutoRepath = true;
        float AvoidanceRadiusMul = 1.2f;
        
        // Steering smoothness: 0 = very smooth (slow turns), 1 = instant (no smoothing)
        // Default 0.15 gives natural-looking movement without jarring turns
        float SteeringSmoothness = 0.15f;
        
        // Arrival slowdown: agent decelerates within this distance of destination
        // Prevents overshooting and gives smooth stops. Set to 0 to disable.
        float ArrivalSlowdownDist = 2.0f;

        // Runtime
        NavPath CurrentPath;
        size_t PathCursor = 0;
        float RepathTimer = 0.0f;
        bool HasDestination = false;
        bool PathRequested = false;
        uint64_t ManagedHandle = 0;
        
        // Current velocity (computed by Navigation::Update, exposed to managed code)
        glm::vec3 CurrentVelocity = glm::vec3(0.0f);
        
        // Smoothed velocity - the actual velocity being applied after steering smoothing
        glm::vec3 SmoothedVelocity = glm::vec3(0.0f);
        
        // Failure tracking (prevents spam when pathfinding repeatedly fails)
        int PathFailCount = 0;
        float PathRetryTimer = 0.0f;
        static constexpr int kMaxRetries = 3;
        static constexpr float kRetryDelay = 2.0f; // seconds between retries after failure

        // Methods
        void SetDestination(const glm::vec3& dest);
        void Stop();
        void Warp(const glm::vec3& pos, TransformComponent* transform, Physics* physics, RigidBodyComponent* rb, ColliderComponent* collider);
        bool HasPath() const { return CurrentPath.valid && !CurrentPath.points.empty(); }
        float RemainingDistance(const glm::vec3& currentPos) const;
    };
}


