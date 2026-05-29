// Physics.h
#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/Body/BodyInterface.h>

#include <glm/glm.hpp>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <memory>
#include <string>

namespace cm { namespace physics { class AreaSystem; class RagdollSystem; class RigidBodyCollisionSystem; }}

// Maximum number of physics layers supported
constexpr uint32_t MAX_PHYSICS_LAYERS = 32;

// For layer constants and masks, use PhysicsLayerManager
// These are just convenience constants for common masks
namespace PhysicsLayerMask {
    constexpr uint32_t All  = 0xFFFFFFFF;
    constexpr uint32_t None = 0;
}

enum class ColliderShape {
    Box,
    Capsule,
    Sphere,
    Mesh
};

class Physics {
public:
	static Physics& Get() {
		static Physics instance;

		return instance;
	}

    static void Init();
    static void Shutdown();
    static void Step(float deltaTime);

    static void DestroyBody(JPH::BodyID bodyID);
    // layer: layer index (0-31), use PhysicsLayerManager to get index by name
    static JPH::BodyID CreateBody(const glm::mat4& transform, JPH::RefConst<JPH::Shape> shape, bool isStatic = false, uint32_t layer = 0);
    static void SetBodyLayer(JPH::BodyID bodyID, uint32_t layer);

    // Body control methods
    static void SetBodyLinearVelocity(JPH::BodyID bodyID, const glm::vec3& velocity);
    static void SetBodyAngularVelocity(JPH::BodyID bodyID, const glm::vec3& velocity);
    static void SetBodyTransform(JPH::BodyID bodyID, const glm::vec3& position, const glm::vec3& eulerDegrees);
    // Move a kinematic body by integrating velocities for this frame and using Jolt's MoveKinematic
    static void MoveKinematicBody(JPH::BodyID bodyID, const glm::vec3& linearVelocity, const glm::vec3& angularVelocity, float deltaTime);
    static glm::mat4 GetBodyTransform(JPH::BodyID bodyID);
    static glm::vec3 GetGravity();
    static void SetGravity(const glm::vec3& gravity);

    // New helper to expose Jolt's body interface
    static JPH::BodyInterface& GetBodyInterface();

    // Area system accessor
    static cm::physics::AreaSystem* GetAreaSystem();
    
    // Ragdoll system accessor
    static cm::physics::RagdollSystem* GetRagdollSystem();
    
    // Rigidbody collision system accessor
    static cm::physics::RigidBodyCollisionSystem* GetCollisionSystem();

    // Expose underlying Jolt systems for integrations that need filters/controllers
    static JPH::PhysicsSystem* GetSystem();
    static JPH::TempAllocatorImpl* GetTempAllocator();

    // Raycast result structure
    struct RaycastHit {
        bool hit = false;
        glm::vec3 point{ 0.0f };
        glm::vec3 normal{ 0.0f };
        float distance = 0.0f;
        uint32_t entityId = 0; // EntityID of the hit body (0 if none)
    };

    // Raycast from origin in direction, returns true if hit
    // layerMask: bitmask of layers to include (default: all layers)
    static bool Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, RaycastHit& outHit, uint32_t layerMask = PhysicsLayerMask::All);
    // Raycast between two points
    // layerMask: bitmask of layers to include (default: all layers)
    static bool RaycastPoints(const glm::vec3& from, const glm::vec3& to, RaycastHit& outHit, uint32_t layerMask = PhysicsLayerMask::All);

private:
	Physics() = default;
	~Physics() = default;

    static JPH::TempAllocatorImpl* s_TempAllocator;
    static JPH::JobSystemThreadPool* s_JobSystem;
    static JPH::PhysicsSystem* s_PhysicsSystem;

    static class BroadPhaseLayerInterfaceImpl* s_BroadPhaseInterface;
    static class ObjectVsBroadPhaseLayerFilterImpl* s_ObjectVsBroadPhaseFilter;
    static class ObjectLayerPairFilterImpl* s_ObjectLayerPairFilter;

    static cm::physics::AreaSystem* s_AreaSystem;
    static cm::physics::RigidBodyCollisionSystem* s_CollisionSystem;
};