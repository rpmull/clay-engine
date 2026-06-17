#include "AnimationStateInterop.h"

#include <atomic>
#include <iostream>

namespace cm {
namespace animation {

// Global callback for dispatching state transitions to managed code.
static std::atomic<AnimationStateCallback> s_AnimationStateCallback{nullptr};

void SetAnimationStateCallback(AnimationStateCallback callback) {
    s_AnimationStateCallback.store(callback, std::memory_order_relaxed);
    std::cout << "[AnimationStateInterop] SetAnimationStateCallback: " << (callback ? "registered" : "cleared") << std::endl;
}

void DispatchAnimationStateEvent(int entityId, const char* stateName, const char* otherStateName, int phase) {
    auto callback = s_AnimationStateCallback.load(std::memory_order_relaxed);
    if (callback) {
        callback(entityId, stateName ? stateName : "", otherStateName ? otherStateName : "", phase);
    }
}

} // namespace animation
} // namespace cm

// Getter for the function pointer used by managed interop initialization.
void* Get_AnimState_SetCallback_Ptr() {
    return (void*)(&cm::animation::SetAnimationStateCallback);
}
