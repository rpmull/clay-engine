#pragma once
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <mutex>
#include <unordered_map>
#include "CollisionEvents.h"

namespace JPH {
    class Body;
    class SubShapeIDPair;
    class ContactManifold;
    class ContactSettings;
    class PhysicsSystem;
}

namespace cm::physics {

class RigidBodyCollisionSystem {
public:
    explicit RigidBodyCollisionSystem(JPH::PhysicsSystem* phys);

    void OnContactAdded(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold&, JPH::ContactSettings&);
    void OnContactRemoved(const JPH::SubShapeIDPair& inSubShapePair);
    void DispatchEventsToInterop();
    void Clear();

private:
    struct PairInfo {
        EntityID aId;
        EntityID bId;
        CollisionBodyKind aKind;
        CollisionBodyKind bKind;
        bool aNotify;
        bool bNotify;
        uint32_t contactCount;
    };

    CollisionBodyKind ResolveKind(const JPH::Body& body, EntityID& outId) const;
    static bool IsSupportedOtherKind(CollisionBodyKind kind);

    JPH::PhysicsSystem* physics;
    CollisionEventBuffer buffer;
    std::mutex pairsMutex;
    std::unordered_map<uint64_t, PairInfo> activePairs;
};

} // namespace cm::physics

