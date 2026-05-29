#pragma once

#include <cstdint>

// Ragdoll interop exports for managed code
extern "C" {
    // Create a ragdoll for an entity with a skeleton
    // Returns true if successful
    __declspec(dllexport) bool Ragdoll_Create(int entityId, bool includeFingersAndToes, uint32_t physicsLayer);
    
    // Destroy the ragdoll for an entity
    __declspec(dllexport) void Ragdoll_Destroy(int entityId);
    
    // Check if entity has a ragdoll
    __declspec(dllexport) bool Ragdoll_Has(int entityId);
    
    // Activate ragdoll physics (disable animation, enable physics)
    __declspec(dllexport) void Ragdoll_Activate(int entityId);
    
    // Deactivate ragdoll (make bodies kinematic)
    __declspec(dllexport) void Ragdoll_Deactivate(int entityId);
    
    // Apply impulse to a specific bone
    __declspec(dllexport) void Ragdoll_ApplyImpulse(int entityId, int boneIndex, float x, float y, float z);
    
    // Apply impulse to all bones
    __declspec(dllexport) void Ragdoll_ApplyImpulseToAll(int entityId, float x, float y, float z);
    
    // Set physics layer for all bones in a ragdoll
    __declspec(dllexport) void Ragdoll_SetPhysicsLayer(int entityId, uint32_t layer);
    
    // Find the ragdoll owner entity from a bone entity
    __declspec(dllexport) int Ragdoll_GetOwnerFromBone(int boneEntityId);
    
    // Get function pointers for interop initialization
    __declspec(dllexport) void* Get_Ragdoll_Create_Ptr();
    __declspec(dllexport) void* Get_Ragdoll_Destroy_Ptr();
    __declspec(dllexport) void* Get_Ragdoll_Has_Ptr();
    __declspec(dllexport) void* Get_Ragdoll_Activate_Ptr();
    __declspec(dllexport) void* Get_Ragdoll_Deactivate_Ptr();
    __declspec(dllexport) void* Get_Ragdoll_ApplyImpulse_Ptr();
    __declspec(dllexport) void* Get_Ragdoll_ApplyImpulseToAll_Ptr();
    __declspec(dllexport) void* Get_Ragdoll_SetPhysicsLayer_Ptr();
    __declspec(dllexport) void* Get_Ragdoll_GetOwnerFromBone_Ptr();
}

