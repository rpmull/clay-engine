// LookAtInterop.h
// Managed interop for LookAt/Aim constraints
#pragma once

#include <cstdint>

using EntityID = uint32_t;

namespace cm { namespace animation { namespace lookat { namespace interop {

// Function pointer types for managed interop
using Fn_LookAt_SetEnabled        = void(*)(EntityID entity, bool enabled);
using Fn_LookAt_SetWeight         = void(*)(EntityID entity, float weight);
using Fn_LookAt_SetTarget         = void(*)(EntityID entity, EntityID targetEntity);
using Fn_LookAt_SetSmoothingSpeed = void(*)(EntityID entity, float speed);
using Fn_LookAt_SetMaxAngles      = void(*)(EntityID entity, float maxYaw, float maxPitch, float maxRoll);
using Fn_LookAt_GetEnabled        = bool(*)(EntityID entity);
using Fn_LookAt_GetWeight         = float(*)(EntityID entity);
using Fn_LookAt_SetMode           = void(*)(EntityID entity, int mode);  // 0=LookAtPosition, 1=MatchRotation
using Fn_LookAt_GetMode           = int(*)(EntityID entity);

// Legacy registration (optional, for symmetry with IK)
extern "C" void LookAt_RegisterManagedCallbacks(
    Fn_LookAt_SetEnabled,
    Fn_LookAt_SetWeight,
    Fn_LookAt_SetTarget,
    Fn_LookAt_SetSmoothingSpeed,
    Fn_LookAt_SetMaxAngles,
    Fn_LookAt_GetEnabled,
    Fn_LookAt_GetWeight,
    Fn_LookAt_SetMode,
    Fn_LookAt_GetMode
);

} } } } // namespace cm::animation::lookat::interop
