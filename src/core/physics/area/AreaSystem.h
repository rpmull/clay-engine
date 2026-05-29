#pragma once
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include "core/ecs/Entity.h"
#include "core/physics/Physics.h"  // for ColliderShape
#include "AreaComponent.h"
#include "AreaEvents.h"
#include <mutex>
#include <unordered_map>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace JPH { class BodyInterface; class ContactListener; class Shape; }

namespace cm::physics {
    
class AreaSystem; // fwd
 
class AreaContactListener : public JPH::ContactListener {
public:
    AreaContactListener(JPH::PhysicsSystem* sys, AreaEventBuffer* buffer, AreaSystem* owner) : physics(sys), buf(buffer), m_owner(owner) {}
    virtual JPH::ValidateResult OnContactValidate(const JPH::Body& inBody1, const JPH::Body& inBody2, JPH::RVec3Arg, const JPH::CollideShapeResult&) override;
    virtual void OnContactAdded(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold&, JPH::ContactSettings&) override;
    virtual void OnContactRemoved(const JPH::SubShapeIDPair& inSubShapePair) override;
private:
    JPH::PhysicsSystem* physics;
    AreaEventBuffer* buf;
    AreaSystem* m_owner;
};

class AreaSystem {
public:
    AreaSystem(JPH::PhysicsSystem* phys, JPH::BodyInterface* bi);

    void OnCreate(Entity e, AreaComponent& area);
    void OnDestroy(Entity e, AreaComponent& area);
    void OnUpdate(float dt);

    void DispatchEventsToInterop();
    // Access for PhysicsContactListener wiring
    AreaEventBuffer* GetEventBuffer() { return &buffer; }
    // Detach Jolt contact listener to avoid callbacks during teardown
    void DetachListener();

    // Map CharacterVirtual inner body -> owning entity ID
    void RegisterCharacterInnerBody(JPH::BodyID innerBody, EntityID ownerEntity);
    void UnregisterCharacterInnerBody(JPH::BodyID innerBody);
    bool TryResolveInnerBodyOwner(uint32_t bodyIndex, EntityID& outOwner) const;

    struct ActivePairInfo {
       EntityID areaId;
       EntityID otherId;
       bool bothAreas;
       };

    std::mutex pairsMutex;
    std::unordered_map<uint64_t, ActivePairInfo> activePairs;
private:
    JPH::PhysicsSystem* physics;
    JPH::BodyInterface* bodyInterface;
    AreaEventBuffer buffer;

    // Character inner body id (BodyID index) to entity id
    std::unordered_map<uint32_t, EntityID> m_InnerBodyToEntity;
    mutable std::mutex m_InnerMapMutex;

    // -------- Frame caches (zero-alloc where possible) --------
    struct CharacterCapsule
    {
        EntityID id;
        glm::vec3 segA;
        glm::vec3 segB;
        float radius;
    };

    struct AreaWorld
    {
        EntityID id;
        glm::vec3 position;
        glm::quat rotation;
        glm::vec3 scale;      // Entity scale - must be applied to Size/Radius
        AreaComponent* comp;  // non-owning
    };

    // For kinematic rigidbody detection (Jolt doesn't detect kinematic vs kinematic)
    struct RigidBodyShape
    {
        EntityID id;
        glm::vec3 position;
        glm::quat rotation;
        ColliderShape shapeType;
        // Capsule/Sphere
        glm::vec3 segA, segB;  // for capsule (segment endpoints)
        float radius;
        // Box
        glm::vec3 halfExtents;
    };

    std::vector<CharacterCapsule> m_FrameCharacters;
    std::vector<AreaWorld> m_FrameAreas;
    std::vector<RigidBodyShape> m_FrameKinematicBodies;
    
    // OPTIMIZATION: Change detection to skip redundant gather/overlap passes
    size_t m_LastEntityCount = 0;
    size_t m_LastAreaCount = 0;
    size_t m_LastCharacterCount = 0;
    size_t m_LastKinematicCount = 0;
    uint32_t m_FrameCounter = 0;
    static constexpr uint32_t kFullRefreshInterval = 30; // Force full refresh every N frames
};

} // namespace cm::physics


