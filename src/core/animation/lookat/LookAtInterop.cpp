// LookAtInterop.cpp
// Managed interop implementation for LookAt/Aim constraints
#include "core/animation/lookat/LookAtInterop.h"
#include "core/animation/lookat/LookAtConstraintComponent.h"
#include "core/ecs/Scene.h"
#include "core/ecs/EntityData.h"
#include <glm/glm.hpp>
#include <atomic>

using namespace cm::animation::lookat;
using namespace cm::animation::lookat::interop;

// --------------------------------------------------------------------------------------
// Helper: Get first LookAt constraint on entity (or skeleton owner)
// Mirrors IK pattern: operates on first constraint for simplicity
// --------------------------------------------------------------------------------------
static inline LookAtConstraintComponent* GetFirstLookAt(EntityID entity)
{
    auto* data = Scene::Get().GetEntityData(entity);
    if (!data) return nullptr;
    
    // If this entity has constraints, use them
    if (!data->LookAtConstraints.empty()) {
        return &data->LookAtConstraints.front();
    }
    
    // Walk up to skeleton owner (same pattern as IK)
    EntityID parent = data->Parent;
    while (parent != INVALID_ENTITY_ID) {
        auto* parentData = Scene::Get().GetEntityData(parent);
        if (!parentData) break;
        if (!parentData->LookAtConstraints.empty()) {
            return &parentData->LookAtConstraints.front();
        }
        parent = parentData->Parent;
    }
    
    return nullptr;
}

// --------------------------------------------------------------------------------------
// Native implementations
// --------------------------------------------------------------------------------------
static void LookAt_SetEnabled_Native(EntityID entity, bool enabled)
{
    if (auto* lac = GetFirstLookAt(entity)) {
        lac->Enabled = enabled;
    }
}

static void LookAt_SetWeight_Native(EntityID entity, float weight)
{
    if (auto* lac = GetFirstLookAt(entity)) {
        lac->SetWeight(weight);
    }
}

static void LookAt_SetTarget_Native(EntityID entity, EntityID targetEntity)
{
    if (auto* lac = GetFirstLookAt(entity)) {
        lac->SetTarget(targetEntity);
    }
}

static void LookAt_SetSmoothingSpeed_Native(EntityID entity, float speed)
{
    if (auto* lac = GetFirstLookAt(entity)) {
        lac->SmoothingSpeed = glm::max(0.0f, speed);
    }
}

static void LookAt_SetMaxAngles_Native(EntityID entity, float maxYaw, float maxPitch, float maxRoll)
{
    if (auto* lac = GetFirstLookAt(entity)) {
        lac->MaxYawDeg = glm::clamp(maxYaw, 0.0f, 180.0f);
        lac->MaxPitchDeg = glm::clamp(maxPitch, 0.0f, 90.0f);
        lac->MaxRollDeg = glm::clamp(maxRoll, 0.0f, 90.0f);
    }
}

static bool LookAt_GetEnabled_Native(EntityID entity)
{
    if (auto* lac = GetFirstLookAt(entity)) {
        return lac->Enabled;
    }
    return false;
}

static float LookAt_GetWeight_Native(EntityID entity)
{
    if (auto* lac = GetFirstLookAt(entity)) {
        return lac->Weight;
    }
    return 0.0f;
}

static void LookAt_SetMode_Native(EntityID entity, int mode)
{
    if (auto* lac = GetFirstLookAt(entity)) {
        lac->Mode = static_cast<LookAtMode>(glm::clamp(mode, 0, 1));
    }
}

static int LookAt_GetMode_Native(EntityID entity)
{
    if (auto* lac = GetFirstLookAt(entity)) {
        return static_cast<int>(lac->Mode);
    }
    return 0; // Default to LookAtPosition
}

static void LookAt_SetTargetUsesNegativeZForward_Native(EntityID entity, bool value)
{
    if (auto* lac = GetFirstLookAt(entity)) {
        lac->TargetUsesNegativeZForward = value;
    }
}

static bool LookAt_GetTargetUsesNegativeZForward_Native(EntityID entity)
{
    if (auto* lac = GetFirstLookAt(entity)) {
        return lac->TargetUsesNegativeZForward;
    }
    return false;
}

// --------------------------------------------------------------------------------------
// Legacy registration (optional, for symmetry)
// --------------------------------------------------------------------------------------
namespace {
    std::atomic<Fn_LookAt_SetEnabled>        g_SetEnabled{nullptr};
    std::atomic<Fn_LookAt_SetWeight>         g_SetWeight{nullptr};
    std::atomic<Fn_LookAt_SetTarget>         g_SetTarget{nullptr};
    std::atomic<Fn_LookAt_SetSmoothingSpeed> g_SetSmoothingSpeed{nullptr};
    std::atomic<Fn_LookAt_SetMaxAngles>      g_SetMaxAngles{nullptr};
    std::atomic<Fn_LookAt_GetEnabled>        g_GetEnabled{nullptr};
    std::atomic<Fn_LookAt_GetWeight>         g_GetWeight{nullptr};
    std::atomic<Fn_LookAt_SetMode>           g_SetMode{nullptr};
    std::atomic<Fn_LookAt_GetMode>           g_GetMode{nullptr};
}

extern "C" void LookAt_RegisterManagedCallbacks(
    Fn_LookAt_SetEnabled a,
    Fn_LookAt_SetWeight b,
    Fn_LookAt_SetTarget c,
    Fn_LookAt_SetSmoothingSpeed d,
    Fn_LookAt_SetMaxAngles e,
    Fn_LookAt_GetEnabled f,
    Fn_LookAt_GetWeight g,
    Fn_LookAt_SetMode h,
    Fn_LookAt_GetMode i)
{
    g_SetEnabled.store(a);
    g_SetWeight.store(b);
    g_SetTarget.store(c);
    g_SetSmoothingSpeed.store(d);
    g_SetMaxAngles.store(e);
    g_GetEnabled.store(f);
    g_GetWeight.store(g);
    g_SetMode.store(h);
    g_GetMode.store(i);
}

// --------------------------------------------------------------------------------------
// Raw pointer getters consumed by DotNetHost to bootstrap managed interop
// --------------------------------------------------------------------------------------
extern "C" void* Get_LookAt_SetEnabled_Ptr()        { return (void*)&LookAt_SetEnabled_Native; }
extern "C" void* Get_LookAt_SetWeight_Ptr()         { return (void*)&LookAt_SetWeight_Native; }
extern "C" void* Get_LookAt_SetTarget_Ptr()         { return (void*)&LookAt_SetTarget_Native; }
extern "C" void* Get_LookAt_SetSmoothingSpeed_Ptr() { return (void*)&LookAt_SetSmoothingSpeed_Native; }
extern "C" void* Get_LookAt_SetMaxAngles_Ptr()      { return (void*)&LookAt_SetMaxAngles_Native; }
extern "C" void* Get_LookAt_GetEnabled_Ptr()        { return (void*)&LookAt_GetEnabled_Native; }
extern "C" void* Get_LookAt_GetWeight_Ptr()         { return (void*)&LookAt_GetWeight_Native; }
extern "C" void* Get_LookAt_SetMode_Ptr()           { return (void*)&LookAt_SetMode_Native; }
extern "C" void* Get_LookAt_GetMode_Ptr()           { return (void*)&LookAt_GetMode_Native; }
extern "C" void* Get_LookAt_SetTargetUsesNegativeZForward_Ptr() { return (void*)&LookAt_SetTargetUsesNegativeZForward_Native; }
extern "C" void* Get_LookAt_GetTargetUsesNegativeZForward_Ptr() { return (void*)&LookAt_GetTargetUsesNegativeZForward_Native; }
