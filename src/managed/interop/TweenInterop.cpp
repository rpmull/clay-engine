#include "TweenInterop.h"
#include "DotNetHost.h"  // For function pointer typedefs
#include "core/animation/Tween.h"
#include "core/ecs/Scene.h"

using cm::animation::TweenManager;
using cm::animation::EasingType;

extern "C" {

void Tween_Position(int entityID, float x, float y, float z, float duration, int easing)
{
    TweenManager::Get().TweenPosition(entityID, glm::vec3{x,y,z}, duration, static_cast<EasingType>(easing));
}

void Tween_RotationEuler(int entityID, float x, float y, float z, float duration, int easing)
{
    TweenManager::Get().TweenRotationEuler(entityID, glm::vec3{x,y,z}, duration, static_cast<EasingType>(easing));
}

void Tween_Scale(int entityID, float x, float y, float z, float duration, int easing)
{
    TweenManager::Get().TweenScale(entityID, glm::vec3{x,y,z}, duration, static_cast<EasingType>(easing));
}

void Tween_LightIntensity(int entityID, float to, float duration, int easing)
{
    TweenManager::Get().TweenLightIntensity(entityID, to, duration, static_cast<EasingType>(easing));
}

void Tween_ManagedFloat(int entityID, const char* className, const char* field, float to, float duration, int easing)
{
    TweenManager::Get().TweenManagedFloat(entityID, className, field, to, duration, static_cast<EasingType>(easing));
}

void Tween_ManagedVec3(int entityID, const char* className, const char* field, float x, float y, float z, float duration, int easing)
{
    TweenManager::Get().TweenManagedVec3(entityID, className, field, glm::vec3{x,y,z}, duration, static_cast<EasingType>(easing));
}

void Tween_SetFinishedCallback(TweenFinishedFn cb)
{
    TweenManager::SetFinishedCallback(reinterpret_cast<TweenManager::FinishedFn>(cb));
}

}

#ifndef CLAYMORE_EDITOR
// Function pointer definitions for runtime interop only
// Editor build defines these in DotNetHost.cpp
Tween_Position_fn Tween_PositionPtr = &Tween_Position;
Tween_RotationEuler_fn Tween_RotationEulerPtr = &Tween_RotationEuler;
Tween_Scale_fn Tween_ScalePtr = &Tween_Scale;
Tween_LightIntensity_fn Tween_LightIntensityPtr = &Tween_LightIntensity;
Tween_ManagedFloat_fn Tween_ManagedFloatPtr = &Tween_ManagedFloat;
Tween_ManagedVec3_fn Tween_ManagedVec3Ptr = &Tween_ManagedVec3;
Tween_SetFinishedCallback_fn Tween_SetFinishedCallbackPtr = reinterpret_cast<Tween_SetFinishedCallback_fn>(&Tween_SetFinishedCallback);
#endif



