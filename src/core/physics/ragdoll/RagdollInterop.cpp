#include "RagdollInterop.h"
#include "RagdollSystem.h"
#include <glm/glm.hpp>

using namespace cm::physics;

extern "C" {

bool Ragdoll_Create(int entityId, bool includeFingersAndToes, uint32_t physicsLayer) {
    auto* sys = GetRagdollSystem();
    if (!sys) return false;
    return sys->CreateRagdoll((EntityID)entityId, includeFingersAndToes, physicsLayer);
}

void Ragdoll_Destroy(int entityId) {
    auto* sys = GetRagdollSystem();
    if (sys) sys->DestroyRagdoll((EntityID)entityId);
}

bool Ragdoll_Has(int entityId) {
    auto* sys = GetRagdollSystem();
    return sys ? sys->HasRagdoll((EntityID)entityId) : false;
}

void Ragdoll_Activate(int entityId) {
    auto* sys = GetRagdollSystem();
    if (sys) sys->ActivateRagdoll((EntityID)entityId);
}

void Ragdoll_Deactivate(int entityId) {
    auto* sys = GetRagdollSystem();
    if (sys) sys->DeactivateRagdoll((EntityID)entityId);
}

void Ragdoll_ApplyImpulse(int entityId, int boneIndex, float x, float y, float z) {
    auto* sys = GetRagdollSystem();
    if (sys) sys->ApplyImpulse((EntityID)entityId, boneIndex, glm::vec3(x, y, z));
}

void Ragdoll_ApplyImpulseToAll(int entityId, float x, float y, float z) {
    auto* sys = GetRagdollSystem();
    if (sys) sys->ApplyImpulseToAll((EntityID)entityId, glm::vec3(x, y, z));
}

void Ragdoll_SetPhysicsLayer(int entityId, uint32_t layer) {
    auto* sys = GetRagdollSystem();
    if (sys) sys->SetPhysicsLayer((EntityID)entityId, layer);
}

int Ragdoll_GetOwnerFromBone(int boneEntityId) {
    auto* sys = GetRagdollSystem();
    if (!sys) return -1;
    EntityID ownerId = sys->GetRagdollOwnerFromBone((EntityID)boneEntityId);
    return ownerId != INVALID_ENTITY_ID ? (int)ownerId : -1;
}

// Function pointer getters for interop
void* Get_Ragdoll_Create_Ptr()          { return (void*)&Ragdoll_Create; }
void* Get_Ragdoll_Destroy_Ptr()         { return (void*)&Ragdoll_Destroy; }
void* Get_Ragdoll_Has_Ptr()             { return (void*)&Ragdoll_Has; }
void* Get_Ragdoll_Activate_Ptr()        { return (void*)&Ragdoll_Activate; }
void* Get_Ragdoll_Deactivate_Ptr()      { return (void*)&Ragdoll_Deactivate; }
void* Get_Ragdoll_ApplyImpulse_Ptr()    { return (void*)&Ragdoll_ApplyImpulse; }
void* Get_Ragdoll_ApplyImpulseToAll_Ptr() { return (void*)&Ragdoll_ApplyImpulseToAll; }
void* Get_Ragdoll_SetPhysicsLayer_Ptr() { return (void*)&Ragdoll_SetPhysicsLayer; }
void* Get_Ragdoll_GetOwnerFromBone_Ptr() { return (void*)&Ragdoll_GetOwnerFromBone; }

}

