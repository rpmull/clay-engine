#pragma once

#include <cstdint>

namespace cm {
namespace animation {

// Callback signature for dispatching animation events to managed code
// Parameters: (entityId, eventName, className, methodName, payloadJson)
using AnimationEventCallback = void(*)(int entityId, const char* eventName, const char* className, const char* methodName, const char* payloadJson);

// Set the global callback for dispatching animation events to managed side
void SetAnimationEventCallback(AnimationEventCallback callback);

// Dispatch an animation event to the managed side (called by AnimationSystem)
void DispatchAnimationEvent(int entityId, const char* eventName, const char* className, const char* methodName, const char* payloadJson);

// Native functions exposed to managed code for registering per-entity event listeners
extern "C" {
    // Register a managed callback for a specific event name on an entity's animator
    // The callbackHandle is a GCHandle to the managed Action delegate
    __declspec(dllexport) void Animator_SetEventCallback(int entityId, const char* eventName, void* callbackHandle);
    
    // Remove a registered callback for a specific event name
    __declspec(dllexport) void Animator_ClearEventCallback(int entityId, const char* eventName);
    
    // Clear all event callbacks for an entity (called when entity is destroyed)
    __declspec(dllexport) void Animator_ClearAllEventCallbacks(int entityId);
}

} // namespace animation
} // namespace cm

// Extern "C" getter functions for managed interop initialization
extern "C" __declspec(dllexport) void* Get_AnimEvent_SetCallback_Ptr();
extern "C" __declspec(dllexport) void* Get_AnimEvent_SetEntityCallback_Ptr();
extern "C" __declspec(dllexport) void* Get_AnimEvent_ClearEntityCallback_Ptr();
extern "C" __declspec(dllexport) void* Get_AnimEvent_ClearAllCallbacks_Ptr();

