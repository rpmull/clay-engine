#include "AnimationEventInterop.h"
#include "core/ecs/Scene.h"
#include "core/ecs/EntityData.h"
#include "core/animation/AnimationPlayerComponent.h"
#include <atomic>
#include <iostream>

namespace cm {
namespace animation {

// Global callback for dispatching events to managed code
static std::atomic<AnimationEventCallback> s_AnimationEventCallback{nullptr};

void SetAnimationEventCallback(AnimationEventCallback callback) {
    s_AnimationEventCallback.store(callback, std::memory_order_relaxed);
    std::cout << "[AnimationEventInterop] SetAnimationEventCallback: " << (callback ? "registered" : "cleared") << std::endl;
}

void DispatchAnimationEvent(int entityId, const char* eventName, const char* className, const char* methodName, const char* payloadJson) {
    auto callback = s_AnimationEventCallback.load(std::memory_order_relaxed);
    if (callback) {
        callback(entityId, eventName, className, methodName, payloadJson);
    }
}

// Wrapper function for SetAnimationEventCallback (used by managed interop)
static void SetAnimationEventCallbackWrapper(AnimationEventCallback callback) {
    SetAnimationEventCallback(callback);
}

extern "C" {

__declspec(dllexport) void Animator_SetEventCallback(int entityId, const char* eventName, void* callbackHandle) {
    if (!eventName) {
        std::cerr << "[AnimationEventInterop] SetEventCallback: eventName is null\n";
        return;
    }
    
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data) {
        std::cerr << "[AnimationEventInterop] SetEventCallback: entity " << entityId << " not found\n";
        return;
    }
    
    if (!data->AnimationPlayer) {
        std::cerr << "[AnimationEventInterop] SetEventCallback: entity " << entityId << " has no AnimationPlayer\n";
        return;
    }
    
    // Store the callback handle (managed GCHandle)
    data->AnimationPlayer->_EventCallbacks[eventName] = callbackHandle;
}

__declspec(dllexport) void Animator_ClearEventCallback(int entityId, const char* eventName) {
    if (!eventName) return;
    
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->AnimationPlayer) return;
    
    data->AnimationPlayer->_EventCallbacks.erase(eventName);
}

__declspec(dllexport) void Animator_ClearAllEventCallbacks(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->AnimationPlayer) return;
    
    data->AnimationPlayer->_EventCallbacks.clear();
}

} // extern "C"

} // namespace animation
} // namespace cm

// Getters for function pointers used by managed interop initialization
void* Get_AnimEvent_SetCallback_Ptr() {
    return (void*)(&cm::animation::SetAnimationEventCallback);
}

void* Get_AnimEvent_SetEntityCallback_Ptr() {
    return (void*)(&cm::animation::Animator_SetEventCallback);
}

void* Get_AnimEvent_ClearEntityCallback_Ptr() {
    return (void*)(&cm::animation::Animator_ClearEventCallback);
}

void* Get_AnimEvent_ClearAllCallbacks_Ptr() {
    return (void*)(&cm::animation::Animator_ClearAllEventCallbacks);
}

