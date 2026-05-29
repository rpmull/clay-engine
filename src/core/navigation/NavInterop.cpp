#include "core/navigation/NavInterop.h"
#include "core/navigation/Navigation.h"
#include "core/ecs/Scene.h"
#include "core/ecs/EntityData.h"
#include "core/ecs/Components.h"
#include <atomic>

using namespace nav;
using namespace nav::interop;

namespace {
    std::atomic<Fn_Nav_FindPath> g_FindPath{nullptr};
    std::atomic<Fn_Agent_SetDestination> g_SetDest{nullptr};
    std::atomic<Fn_Agent_Stop> g_Stop{nullptr};
    std::atomic<Fn_Agent_Warp> g_Warp{nullptr};
    std::atomic<Fn_Agent_RemainingDist> g_Remain{nullptr};
    std::atomic<Fn_OnPathComplete> g_OnPathComplete{nullptr};
}

void nav::interop::Nav_RegisterManagedCallbacks(Fn_Nav_FindPath findPath,
                                                Fn_Agent_SetDestination setDest,
                                                Fn_Agent_Stop stop,
                                                Fn_Agent_Warp warp,
                                                Fn_Agent_RemainingDist remaining)
{
    g_FindPath.store(findPath);
    g_SetDest.store(setDest);
    g_Stop.store(stop);
    g_Warp.store(warp);
    g_Remain.store(remaining);
}

void nav::interop::Nav_SetOnPathComplete(Fn_OnPathComplete cb)
{
    g_OnPathComplete.store(cb);
}

void nav::interop::FireOnPathComplete(uint64_t managedHandle, bool success)
{
    if (auto fn = g_OnPathComplete.load()) fn(managedHandle, success);
}

// ---------------- Native functions exposed to managed via init table ----------------
static bool Nav_FindPath_Native(EntityID navMeshEntity, float startX, float startY, float startZ,
                                float endX, float endY, float endZ,
                                const NavAgentParams& p, uint32_t includeFlags, uint32_t excludeFlags,
                                /*out*/ NavPath* outPath)
{
    if (!outPath) return false;
    Scene& scene = Scene::Get();
    NavPath path;
    glm::vec3 start(startX, startY, startZ);
    glm::vec3 end(endX, endY, endZ);
    bool ok = Navigation::Get().FindPath(scene, navMeshEntity, start, end, p, NavFlags{includeFlags}, NavFlags{excludeFlags}, path);
    *outPath = std::move(path);
    return ok;
}

static void Nav_Agent_SetDestination_Native(EntityID agentEntity, float destX, float destY, float destZ)
{
    if (auto* d = Scene::Get().GetEntityData(agentEntity)) {
        if (d->NavAgent) d->NavAgent->SetDestination(glm::vec3(destX, destY, destZ));
    }
}

static void Nav_Agent_Stop_Native(EntityID agentEntity)
{
    if (auto* d = Scene::Get().GetEntityData(agentEntity)) {
        if (d->NavAgent) d->NavAgent->Stop();
    }
}

static void Nav_Agent_Warp_Native(EntityID agentEntity, float posX, float posY, float posZ)
{
    if (auto* d = Scene::Get().GetEntityData(agentEntity)) {
        if (d->NavAgent) {
            d->NavAgent->Warp(glm::vec3(posX, posY, posZ), &d->Transform, &Physics::Get(), d->RigidBody.get(), d->Collider.get());
            Scene::Get().MarkTransformDirty(agentEntity);
        }
    }
}

static float Nav_Agent_RemainingDist_Native(EntityID agentEntity)
{
    if (auto* d = Scene::Get().GetEntityData(agentEntity)) {
        if (d->NavAgent) {
            glm::vec3 cur = glm::vec3(d->Transform.WorldMatrix[3]);
            return d->NavAgent->RemainingDistance(cur);
        }
    }
    return 0.0f;
}

// ---- New state/parameter getters/setters ----

static bool Nav_Agent_IsStopped_Native(EntityID agentEntity)
{
    if (auto* d = Scene::Get().GetEntityData(agentEntity)) {
        if (d->NavAgent) {
            return !d->NavAgent->HasDestination && !d->NavAgent->HasPath();
        }
    }
    return true;
}

static bool Nav_Agent_IsMoving_Native(EntityID agentEntity)
{
    if (auto* d = Scene::Get().GetEntityData(agentEntity)) {
        if (d->NavAgent) {
            return d->NavAgent->HasPath() && d->NavAgent->PathCursor < d->NavAgent->CurrentPath.points.size();
        }
    }
    return false;
}

static bool Nav_Agent_HasPath_Native(EntityID agentEntity)
{
    if (auto* d = Scene::Get().GetEntityData(agentEntity)) {
        if (d->NavAgent) return d->NavAgent->HasPath();
    }
    return false;
}

static float Nav_Agent_GetSpeed_Native(EntityID agentEntity)
{
    if (auto* d = Scene::Get().GetEntityData(agentEntity)) {
        if (d->NavAgent) return d->NavAgent->Params.maxSpeed;
    }
    return 0.0f;
}

static void Nav_Agent_SetSpeed_Native(EntityID agentEntity, float value)
{
    if (auto* d = Scene::Get().GetEntityData(agentEntity)) {
        if (d->NavAgent) d->NavAgent->Params.maxSpeed = value;
    }
}

static float Nav_Agent_GetAcceleration_Native(EntityID agentEntity)
{
    if (auto* d = Scene::Get().GetEntityData(agentEntity)) {
        if (d->NavAgent) return d->NavAgent->Params.maxAccel;
    }
    return 0.0f;
}

static void Nav_Agent_SetAcceleration_Native(EntityID agentEntity, float value)
{
    if (auto* d = Scene::Get().GetEntityData(agentEntity)) {
        if (d->NavAgent) d->NavAgent->Params.maxAccel = value;
    }
}

static float Nav_Agent_GetRadius_Native(EntityID agentEntity)
{
    if (auto* d = Scene::Get().GetEntityData(agentEntity)) {
        if (d->NavAgent) return d->NavAgent->Params.radius;
    }
    return 0.0f;
}

static void Nav_Agent_SetRadius_Native(EntityID agentEntity, float value)
{
    if (auto* d = Scene::Get().GetEntityData(agentEntity)) {
        if (d->NavAgent) d->NavAgent->Params.radius = value;
    }
}

static float Nav_Agent_GetHeight_Native(EntityID agentEntity)
{
    if (auto* d = Scene::Get().GetEntityData(agentEntity)) {
        if (d->NavAgent) return d->NavAgent->Params.height;
    }
    return 0.0f;
}

static void Nav_Agent_SetHeight_Native(EntityID agentEntity, float value)
{
    if (auto* d = Scene::Get().GetEntityData(agentEntity)) {
        if (d->NavAgent) d->NavAgent->Params.height = value;
    }
}

static float Nav_Agent_GetStoppingDistance_Native(EntityID agentEntity)
{
    if (auto* d = Scene::Get().GetEntityData(agentEntity)) {
        if (d->NavAgent) return d->NavAgent->ArriveThreshold;
    }
    return 0.0f;
}

static void Nav_Agent_SetStoppingDistance_Native(EntityID agentEntity, float value)
{
    if (auto* d = Scene::Get().GetEntityData(agentEntity)) {
        if (d->NavAgent) d->NavAgent->ArriveThreshold = value;
    }
}

// ---- Velocity getters (read-only, computed by navigation system) ----

static float Nav_Agent_GetVelocityX_Native(EntityID agentEntity)
{
    if (auto* d = Scene::Get().GetEntityData(agentEntity)) {
        if (d->NavAgent) return d->NavAgent->CurrentVelocity.x;
    }
    return 0.0f;
}

static float Nav_Agent_GetVelocityY_Native(EntityID agentEntity)
{
    if (auto* d = Scene::Get().GetEntityData(agentEntity)) {
        if (d->NavAgent) return d->NavAgent->CurrentVelocity.y;
    }
    return 0.0f;
}

static float Nav_Agent_GetVelocityZ_Native(EntityID agentEntity)
{
    if (auto* d = Scene::Get().GetEntityData(agentEntity)) {
        if (d->NavAgent) return d->NavAgent->CurrentVelocity.z;
    }
    return 0.0f;
}

// Expose raw pointers for host bootstrap
extern "C" void* Get_Nav_FindPath_Ptr() { return (void*)&Nav_FindPath_Native; }
extern "C" void* Get_Nav_Agent_SetDest_Ptr() { return (void*)&Nav_Agent_SetDestination_Native; }
extern "C" void* Get_Nav_Agent_Stop_Ptr() { return (void*)&Nav_Agent_Stop_Native; }
extern "C" void* Get_Nav_Agent_Warp_Ptr() { return (void*)&Nav_Agent_Warp_Native; }
extern "C" void* Get_Nav_Agent_Remaining_Ptr() { return (void*)&Nav_Agent_RemainingDist_Native; }
extern "C" void* Get_Nav_SetOnPathComplete_Ptr() { return (void*)&nav::interop::Nav_SetOnPathComplete; }

// New accessors
extern "C" void* Get_Nav_Agent_IsStopped_Ptr() { return (void*)&Nav_Agent_IsStopped_Native; }
extern "C" void* Get_Nav_Agent_IsMoving_Ptr() { return (void*)&Nav_Agent_IsMoving_Native; }
extern "C" void* Get_Nav_Agent_HasPath_Ptr() { return (void*)&Nav_Agent_HasPath_Native; }
extern "C" void* Get_Nav_Agent_GetSpeed_Ptr() { return (void*)&Nav_Agent_GetSpeed_Native; }
extern "C" void* Get_Nav_Agent_SetSpeed_Ptr() { return (void*)&Nav_Agent_SetSpeed_Native; }
extern "C" void* Get_Nav_Agent_GetAccel_Ptr() { return (void*)&Nav_Agent_GetAcceleration_Native; }
extern "C" void* Get_Nav_Agent_SetAccel_Ptr() { return (void*)&Nav_Agent_SetAcceleration_Native; }
extern "C" void* Get_Nav_Agent_GetRadius_Ptr() { return (void*)&Nav_Agent_GetRadius_Native; }
extern "C" void* Get_Nav_Agent_SetRadius_Ptr() { return (void*)&Nav_Agent_SetRadius_Native; }
extern "C" void* Get_Nav_Agent_GetHeight_Ptr() { return (void*)&Nav_Agent_GetHeight_Native; }
extern "C" void* Get_Nav_Agent_SetHeight_Ptr() { return (void*)&Nav_Agent_SetHeight_Native; }
extern "C" void* Get_Nav_Agent_GetStopDist_Ptr() { return (void*)&Nav_Agent_GetStoppingDistance_Native; }
extern "C" void* Get_Nav_Agent_SetStopDist_Ptr() { return (void*)&Nav_Agent_SetStoppingDistance_Native; }
extern "C" void* Get_Nav_Agent_GetVelocityX_Ptr() { return (void*)&Nav_Agent_GetVelocityX_Native; }
extern "C" void* Get_Nav_Agent_GetVelocityY_Ptr() { return (void*)&Nav_Agent_GetVelocityY_Native; }
extern "C" void* Get_Nav_Agent_GetVelocityZ_Ptr() { return (void*)&Nav_Agent_GetVelocityZ_Native; }


