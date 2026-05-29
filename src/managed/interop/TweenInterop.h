#pragma once

#ifdef __cplusplus
extern "C" {
#endif

__declspec(dllexport) void Tween_Position(int entityID, float x, float y, float z, float duration, int easing);
__declspec(dllexport) void Tween_RotationEuler(int entityID, float x, float y, float z, float duration, int easing);
__declspec(dllexport) void Tween_Scale(int entityID, float x, float y, float z, float duration, int easing);
__declspec(dllexport) void Tween_LightIntensity(int entityID, float to, float duration, int easing);

// Managed property tweens; pass null className to target first managed script
__declspec(dllexport) void Tween_ManagedFloat(int entityID, const char* className, const char* field, float to, float duration, int easing);
__declspec(dllexport) void Tween_ManagedVec3(int entityID, const char* className, const char* field, float x, float y, float z, float duration, int easing);

// Completion callback registration
typedef void (*TweenFinishedFn)(int entityID, const char* tag);
__declspec(dllexport) void Tween_SetFinishedCallback(TweenFinishedFn cb);

#ifdef __cplusplus
}
#endif



