#pragma once

#include <cstdint>

namespace cm {
namespace animation {

// Phase values passed to the managed state callback.
enum class AnimationStatePhase : int {
    Exited = 0,
    Entered = 1
};

// Callback signature for dispatching animation state transitions to managed code.
// Parameters: (entityId, stateName, otherStateName, phase)
//   - phase 1 (Entered): otherStateName is the state that was just left (previous state).
//   - phase 0 (Exited):  otherStateName is the state that is being entered (next state).
using AnimationStateCallback = void(*)(int entityId, const char* stateName, const char* otherStateName, int phase);

// Set the global callback for dispatching animation state transitions to the managed side.
void SetAnimationStateCallback(AnimationStateCallback callback);

// Dispatch a state enter/exit transition to the managed side (called by AnimationSystem).
// Cheap when no managed listener is registered (single atomic load + null check).
void DispatchAnimationStateEvent(int entityId, const char* stateName, const char* otherStateName, int phase);

} // namespace animation
} // namespace cm

// Extern "C" getter used by managed interop initialization.
extern "C" __declspec(dllexport) void* Get_AnimState_SetCallback_Ptr();
