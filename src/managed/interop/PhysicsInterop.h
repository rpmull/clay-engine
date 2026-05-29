#pragma once
#include <string>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// Gravity
__declspec(dllexport) void Physics_SetGravity(float x, float y, float z);
__declspec(dllexport) void Physics_GetGravity(float* x, float* y, float* z);

// Raycast from origin along direction up to maxDistance
// Returns true if hit, fills out parameters with hit info
// layerMask: bitmask of layers to include (0xFFFFFFFF = all)
__declspec(dllexport) bool Physics_Raycast(
    float originX, float originY, float originZ,
    float dirX, float dirY, float dirZ,
    float maxDistance,
    uint32_t layerMask,
    float* hitPointX, float* hitPointY, float* hitPointZ,
    float* hitNormalX, float* hitNormalY, float* hitNormalZ,
    float* hitDistance,
    int* hitEntityId);

// Raycast between two points
// layerMask: bitmask of layers to include (0xFFFFFFFF = all)
__declspec(dllexport) bool Physics_RaycastPoints(
    float fromX, float fromY, float fromZ,
    float toX, float toY, float toZ,
    uint32_t layerMask,
    float* hitPointX, float* hitPointY, float* hitPointZ,
    float* hitNormalX, float* hitNormalY, float* hitNormalZ,
    float* hitDistance,
    int* hitEntityId);

// Layer management
__declspec(dllexport) uint32_t Physics_RegisterLayer(const char* name);
__declspec(dllexport) int32_t Physics_GetLayerIndex(const char* name);
__declspec(dllexport) uint32_t Physics_GetLayerMask(const char* name);
__declspec(dllexport) uint32_t Physics_GetLayerCount();

#ifdef __cplusplus
}
#endif

// Function pointer types for interop
using Physics_SetGravity_fn = void(*)(float, float, float);
using Physics_GetGravity_fn = void(*)(float*, float*, float*);
using Physics_Raycast_fn = bool(*)(float, float, float, float, float, float, float, uint32_t,
    float*, float*, float*, float*, float*, float*, float*, int*);
using Physics_RaycastPoints_fn = bool(*)(float, float, float, float, float, float, uint32_t,
    float*, float*, float*, float*, float*, float*, float*, int*);
using Physics_RegisterLayer_fn = uint32_t(*)(const char*);
using Physics_GetLayerIndex_fn = int32_t(*)(const char*);
using Physics_GetLayerMask_fn = uint32_t(*)(const char*);
using Physics_GetLayerCount_fn = uint32_t(*)();

// Function pointers for managed interop
extern Physics_SetGravity_fn Physics_SetGravityPtr;
extern Physics_GetGravity_fn Physics_GetGravityPtr;
extern Physics_Raycast_fn Physics_RaycastPtr;
extern Physics_RaycastPoints_fn Physics_RaycastPointsPtr;
extern Physics_RegisterLayer_fn Physics_RegisterLayerPtr;
extern Physics_GetLayerIndex_fn Physics_GetLayerIndexPtr;
extern Physics_GetLayerMask_fn Physics_GetLayerMaskPtr;
extern Physics_GetLayerCount_fn Physics_GetLayerCountPtr;

// Number of function pointers (must match managed side)
constexpr int kPhysicsInteropCount = 8;

// Setup physics interop with managed runtime
void SetupPhysicsInterop(const std::wstring& assemblyPath);





