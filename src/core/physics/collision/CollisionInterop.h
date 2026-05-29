#pragma once
#include <cstdint>

namespace cm::physics {

// Native -> Managed bridge setup for rigidbody collision events
using CollisionEvent_OnEnterFn = void(*)(int selfEntity, int otherEntity, int otherKind);
using CollisionEvent_OnExitFn  = void(*)(int selfEntity, int otherEntity, int otherKind);

void Set_Collision_OnEnter_Callback(CollisionEvent_OnEnterFn fn);
void Set_Collision_OnExit_Callback(CollisionEvent_OnExitFn fn);

// kind: 0=Enter, 1=Exit
void CollisionInterop_Dispatch(int kind, int selfEntity, int otherEntity, int otherKind);

} // namespace cm::physics

