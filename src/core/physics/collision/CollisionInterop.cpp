#include "CollisionInterop.h"
#include <atomic>

namespace cm::physics {

static std::atomic<CollisionEvent_OnEnterFn> s_OnEnter{ nullptr };
static std::atomic<CollisionEvent_OnExitFn>  s_OnExit{ nullptr };

void Set_Collision_OnEnter_Callback(CollisionEvent_OnEnterFn fn) { s_OnEnter.store(fn, std::memory_order_relaxed); }
void Set_Collision_OnExit_Callback (CollisionEvent_OnExitFn fn)  { s_OnExit.store(fn, std::memory_order_relaxed); }

void CollisionInterop_Dispatch(int kind, int selfEntity, int otherEntity, int otherKind)
{
    switch (kind) {
        case 0: if (auto f = s_OnEnter.load(std::memory_order_relaxed)) f(selfEntity, otherEntity, otherKind); break;
        case 1: if (auto f = s_OnExit.load(std::memory_order_relaxed))  f(selfEntity, otherEntity, otherKind); break;
        default: break;
    }
}

} // namespace cm::physics

extern "C" __declspec(dllexport) void* Get_Collision_SetOnEnter_Ptr() {
    return (void*)(&cm::physics::Set_Collision_OnEnter_Callback);
}
extern "C" __declspec(dllexport) void* Get_Collision_SetOnExit_Ptr() {
    return (void*)(&cm::physics::Set_Collision_OnExit_Callback);
}

