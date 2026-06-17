#include "PhysicsInterop.h"
#include "core/physics/Physics.h"
#include "core/physics/PhysicsLayerManager.h"
#ifdef CLAYMORE_EDITOR
#include "DotNetHost.h"  // For ResolveManagedDelegate (editor only)
#endif
#include <iostream>

// Function pointers for managed interop
Physics_SetGravity_fn Physics_SetGravityPtr = &Physics_SetGravity;
Physics_GetGravity_fn Physics_GetGravityPtr = &Physics_GetGravity;
Physics_Raycast_fn Physics_RaycastPtr = &Physics_Raycast;
Physics_RaycastPoints_fn Physics_RaycastPointsPtr = &Physics_RaycastPoints;
Physics_Spherecast_fn Physics_SpherecastPtr = &Physics_Spherecast;
Physics_SpherecastPoints_fn Physics_SpherecastPointsPtr = &Physics_SpherecastPoints;
Physics_RegisterLayer_fn Physics_RegisterLayerPtr = &Physics_RegisterLayer;
Physics_GetLayerIndex_fn Physics_GetLayerIndexPtr = &Physics_GetLayerIndex;
Physics_GetLayerMask_fn Physics_GetLayerMaskPtr = &Physics_GetLayerMask;
Physics_GetLayerCount_fn Physics_GetLayerCountPtr = &Physics_GetLayerCount;

void Physics_SetGravity(float x, float y, float z)
{
    Physics::SetGravity(glm::vec3(x, y, z));
}

void Physics_GetGravity(float* x, float* y, float* z)
{
    glm::vec3 gravity = Physics::GetGravity();
    if (x) *x = gravity.x;
    if (y) *y = gravity.y;
    if (z) *z = gravity.z;
}

bool Physics_Raycast(
    float originX, float originY, float originZ,
    float dirX, float dirY, float dirZ,
    float maxDistance,
    uint32_t layerMask,
    float* hitPointX, float* hitPointY, float* hitPointZ,
    float* hitNormalX, float* hitNormalY, float* hitNormalZ,
    float* hitDistance,
    int* hitEntityId)
{
    Physics::RaycastHit hit;
    bool result = Physics::Raycast(
        glm::vec3(originX, originY, originZ),
        glm::vec3(dirX, dirY, dirZ),
        maxDistance,
        hit,
        layerMask
    );
    
    if (result)
    {
        if (hitPointX) *hitPointX = hit.point.x;
        if (hitPointY) *hitPointY = hit.point.y;
        if (hitPointZ) *hitPointZ = hit.point.z;
        if (hitNormalX) *hitNormalX = hit.normal.x;
        if (hitNormalY) *hitNormalY = hit.normal.y;
        if (hitNormalZ) *hitNormalZ = hit.normal.z;
        if (hitDistance) *hitDistance = hit.distance;
        if (hitEntityId) *hitEntityId = static_cast<int>(hit.entityId);
    }
    else
    {
        // Initialize output to safe defaults even on miss
        if (hitPointX) *hitPointX = 0;
        if (hitPointY) *hitPointY = 0;
        if (hitPointZ) *hitPointZ = 0;
        if (hitNormalX) *hitNormalX = 0;
        if (hitNormalY) *hitNormalY = 1;
        if (hitNormalZ) *hitNormalZ = 0;
        if (hitDistance) *hitDistance = 0;
        if (hitEntityId) *hitEntityId = 0;
    }
    
    return result;
}

bool Physics_RaycastPoints(
    float fromX, float fromY, float fromZ,
    float toX, float toY, float toZ,
    uint32_t layerMask,
    float* hitPointX, float* hitPointY, float* hitPointZ,
    float* hitNormalX, float* hitNormalY, float* hitNormalZ,
    float* hitDistance,
    int* hitEntityId)
{
    Physics::RaycastHit hit;
    bool result = Physics::RaycastPoints(
        glm::vec3(fromX, fromY, fromZ),
        glm::vec3(toX, toY, toZ),
        hit,
        layerMask
    );
    
    if (result)
    {
        if (hitPointX) *hitPointX = hit.point.x;
        if (hitPointY) *hitPointY = hit.point.y;
        if (hitPointZ) *hitPointZ = hit.point.z;
        if (hitNormalX) *hitNormalX = hit.normal.x;
        if (hitNormalY) *hitNormalY = hit.normal.y;
        if (hitNormalZ) *hitNormalZ = hit.normal.z;
        if (hitDistance) *hitDistance = hit.distance;
        if (hitEntityId) *hitEntityId = static_cast<int>(hit.entityId);
    }
    else
    {
        // Initialize output to safe defaults even on miss
        if (hitPointX) *hitPointX = 0;
        if (hitPointY) *hitPointY = 0;
        if (hitPointZ) *hitPointZ = 0;
        if (hitNormalX) *hitNormalX = 0;
        if (hitNormalY) *hitNormalY = 1;
        if (hitNormalZ) *hitNormalZ = 0;
        if (hitDistance) *hitDistance = 0;
        if (hitEntityId) *hitEntityId = 0;
    }
    
    return result;
}

bool Physics_Spherecast(
    float originX, float originY, float originZ,
    float dirX, float dirY, float dirZ,
    float radius,
    float maxDistance,
    uint32_t layerMask,
    float* hitPointX, float* hitPointY, float* hitPointZ,
    float* hitNormalX, float* hitNormalY, float* hitNormalZ,
    float* hitDistance,
    int* hitEntityId)
{
    Physics::RaycastHit hit;
    bool result = Physics::Spherecast(
        glm::vec3(originX, originY, originZ),
        glm::vec3(dirX, dirY, dirZ),
        radius,
        maxDistance,
        hit,
        layerMask
    );

    if (result)
    {
        if (hitPointX) *hitPointX = hit.point.x;
        if (hitPointY) *hitPointY = hit.point.y;
        if (hitPointZ) *hitPointZ = hit.point.z;
        if (hitNormalX) *hitNormalX = hit.normal.x;
        if (hitNormalY) *hitNormalY = hit.normal.y;
        if (hitNormalZ) *hitNormalZ = hit.normal.z;
        if (hitDistance) *hitDistance = hit.distance;
        if (hitEntityId) *hitEntityId = static_cast<int>(hit.entityId);
    }
    else
    {
        if (hitPointX) *hitPointX = 0;
        if (hitPointY) *hitPointY = 0;
        if (hitPointZ) *hitPointZ = 0;
        if (hitNormalX) *hitNormalX = 0;
        if (hitNormalY) *hitNormalY = 1;
        if (hitNormalZ) *hitNormalZ = 0;
        if (hitDistance) *hitDistance = 0;
        if (hitEntityId) *hitEntityId = 0;
    }

    return result;
}

bool Physics_SpherecastPoints(
    float fromX, float fromY, float fromZ,
    float toX, float toY, float toZ,
    float radius,
    uint32_t layerMask,
    float* hitPointX, float* hitPointY, float* hitPointZ,
    float* hitNormalX, float* hitNormalY, float* hitNormalZ,
    float* hitDistance,
    int* hitEntityId)
{
    Physics::RaycastHit hit;
    bool result = Physics::SpherecastPoints(
        glm::vec3(fromX, fromY, fromZ),
        glm::vec3(toX, toY, toZ),
        radius,
        hit,
        layerMask
    );

    if (result)
    {
        if (hitPointX) *hitPointX = hit.point.x;
        if (hitPointY) *hitPointY = hit.point.y;
        if (hitPointZ) *hitPointZ = hit.point.z;
        if (hitNormalX) *hitNormalX = hit.normal.x;
        if (hitNormalY) *hitNormalY = hit.normal.y;
        if (hitNormalZ) *hitNormalZ = hit.normal.z;
        if (hitDistance) *hitDistance = hit.distance;
        if (hitEntityId) *hitEntityId = static_cast<int>(hit.entityId);
    }
    else
    {
        if (hitPointX) *hitPointX = 0;
        if (hitPointY) *hitPointY = 0;
        if (hitPointZ) *hitPointZ = 0;
        if (hitNormalX) *hitNormalX = 0;
        if (hitNormalY) *hitNormalY = 1;
        if (hitNormalZ) *hitNormalZ = 0;
        if (hitDistance) *hitDistance = 0;
        if (hitEntityId) *hitEntityId = 0;
    }

    return result;
}

// Layer management functions
uint32_t Physics_RegisterLayer(const char* name)
{
    if (!name) return 0;
    return PhysicsLayers::PhysicsLayerManager::Get().RegisterLayer(name);
}

int32_t Physics_GetLayerIndex(const char* name)
{
    if (!name) return -1;
    return PhysicsLayers::PhysicsLayerManager::Get().GetLayerIndex(name);
}

uint32_t Physics_GetLayerMask(const char* name)
{
    if (!name) return 0;
    return PhysicsLayers::PhysicsLayerManager::Get().GetLayerMask(name);
}

uint32_t Physics_GetLayerCount()
{
    return PhysicsLayers::PhysicsLayerManager::Get().GetLayerCount();
}

#ifdef CLAYMORE_EDITOR
// Editor-only: Uses ResolveManagedDelegate from DotNetHost.cpp
// Runtime uses RuntimeInterop.cpp's SetupPhysicsInterop instead
void SetupPhysicsInterop(const std::wstring& assemblyPath)
{
    // Resolve the managed Physics.InitializeInterop method
    using PhysicsInteropInit_fn = void(*)(void**, int);
    PhysicsInteropInit_fn initFn = nullptr;

    bool ok = ResolveManagedDelegate(
        assemblyPath,
        L"ClaymoreEngine.Physics.Physics, ClaymoreEngine",
        L"InitializeInterop",
        L"ClaymoreEngine.Physics.PhysicsInteropInitDelegate, ClaymoreEngine",
        (void**)&initFn
    );

    if (!ok || !initFn)
    {
        std::cerr << "[PhysicsInterop] Failed to resolve Physics.InitializeInterop\n";
        return;
    }

    // Build function pointer table (order must match managed side)
    // Count defined in kPhysicsInteropCount
    void* ptrs[kPhysicsInteropCount] = {
        (void*)Physics_SetGravityPtr,       // 0
        (void*)Physics_GetGravityPtr,       // 1
        (void*)Physics_RaycastPtr,          // 2
        (void*)Physics_RaycastPointsPtr,    // 3
        (void*)Physics_RegisterLayerPtr,    // 4
        (void*)Physics_GetLayerIndexPtr,    // 5
        (void*)Physics_GetLayerMaskPtr,     // 6
        (void*)Physics_GetLayerCountPtr,    // 7
        (void*)Physics_SpherecastPtr,       // 8
        (void*)Physics_SpherecastPointsPtr  // 9
    };

    // Call managed initializer
    initFn(ptrs, kPhysicsInteropCount);
    std::cout << "[PhysicsInterop] Physics interop initialized (" << kPhysicsInteropCount << " functions).\n";
}
#endif





