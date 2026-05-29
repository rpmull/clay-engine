#include "CollisionSystem.h"
#include "CollisionInterop.h"
#include "core/ecs/Scene.h"
#include "core/ecs/EntityData.h"
#include "core/physics/Physics.h"
#include "core/physics/area/AreaSystem.h"
#include <algorithm>

using namespace cm::physics;

RigidBodyCollisionSystem::RigidBodyCollisionSystem(JPH::PhysicsSystem* phys)
    : physics(phys)
{
}

bool RigidBodyCollisionSystem::IsSupportedOtherKind(CollisionBodyKind kind)
{
    return kind == CollisionBodyKind::RigidBody
        || kind == CollisionBodyKind::StaticBody
        || kind == CollisionBodyKind::CharacterController;
}

CollisionBodyKind RigidBodyCollisionSystem::ResolveKind(const JPH::Body& body, EntityID& outId) const
{
    outId = (EntityID)body.GetUserData();
    if (outId == 0) {
        if (auto* areaSystem = Physics::Get().GetAreaSystem()) {
            EntityID owner = 0;
            if (areaSystem->TryResolveInnerBodyOwner(body.GetID().GetIndex(), owner) && owner != 0) {
                outId = owner;
            }
        }
    }

    if (outId == 0) return CollisionBodyKind::Unknown;

    auto* data = Scene::Get().GetEntityData(outId);
    if (!data) {
        outId = 0;
        return CollisionBodyKind::Unknown;
    }

    if (data->RigidBody) return CollisionBodyKind::RigidBody;
    if (data->StaticBody || data->Terrain) return CollisionBodyKind::StaticBody;
    if (data->CharacterController) return CollisionBodyKind::CharacterController;
    return CollisionBodyKind::Unknown;
}

void RigidBodyCollisionSystem::OnContactAdded(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold&, JPH::ContactSettings&)
{
    if (!physics) return;
    if (inBody1.IsSensor() || inBody2.IsSensor()) return;
    if (inBody1.GetMotionType() == JPH::EMotionType::Static && inBody2.GetMotionType() == JPH::EMotionType::Static) return;

    EntityID id1 = 0, id2 = 0;
    CollisionBodyKind kind1 = ResolveKind(inBody1, id1);
    CollisionBodyKind kind2 = ResolveKind(inBody2, id2);

    if (id1 == 0 || id2 == 0) return;
    if (id1 == id2) return;

    const bool aNotify = (kind1 == CollisionBodyKind::RigidBody) && IsSupportedOtherKind(kind2);
    const bool bNotify = (kind2 == CollisionBodyKind::RigidBody) && IsSupportedOtherKind(kind1);

    if (!aNotify && !bNotify) return;

    const uint32_t idx1 = inBody1.GetID().GetIndex();
    const uint32_t idx2 = inBody2.GetID().GetIndex();
    const uint32_t lo = std::min(idx1, idx2);
    const uint32_t hi = std::max(idx1, idx2);
    const uint64_t key = ((uint64_t)lo << 32) ^ (uint64_t)hi;

    {
        std::lock_guard<std::mutex> guard(pairsMutex);
        auto it = activePairs.find(key);
        if (it != activePairs.end()) {
            ++it->second.contactCount;
            return;
        }
        activePairs[key] = { id1, id2, kind1, kind2, aNotify, bNotify, 1 };
    }

    if (aNotify) buffer.Push({ CollisionEventKind::Enter, id1, id2, kind2 });
    if (bNotify) buffer.Push({ CollisionEventKind::Enter, id2, id1, kind1 });
}

void RigidBodyCollisionSystem::OnContactRemoved(const JPH::SubShapeIDPair& inSubShapePair)
{
    if (!physics) return;

    const uint32_t b1 = inSubShapePair.GetBody1ID().GetIndex();
    const uint32_t b2 = inSubShapePair.GetBody2ID().GetIndex();
    const uint32_t lo = std::min(b1, b2);
    const uint32_t hi = std::max(b1, b2);
    const uint64_t key = ((uint64_t)lo << 32) ^ (uint64_t)hi;

    PairInfo info{};
    {
        std::lock_guard<std::mutex> guard(pairsMutex);
        auto it = activePairs.find(key);
        if (it == activePairs.end()) return;
        if (it->second.contactCount > 1) {
            --it->second.contactCount;
            return;
        }
        info = it->second;
        activePairs.erase(it);
    }

    if (info.aNotify) buffer.Push({ CollisionEventKind::Exit, info.aId, info.bId, info.bKind });
    if (info.bNotify) buffer.Push({ CollisionEventKind::Exit, info.bId, info.aId, info.aKind });
}

void RigidBodyCollisionSystem::DispatchEventsToInterop()
{
    std::vector<CollisionEvent> local;
    buffer.SwapTo(local);

    for (const auto& ev : local) {
        const int kind = (ev.Kind == CollisionEventKind::Enter) ? 0 : 1;
        CollisionInterop_Dispatch(kind, (int)ev.SelfId, (int)ev.OtherId, (int)ev.OtherKind);
    }
}

void RigidBodyCollisionSystem::Clear()
{
    buffer.Clear();
    std::lock_guard<std::mutex> guard(pairsMutex);
    activePairs.clear();
}

