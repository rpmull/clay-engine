#pragma once
#include <cstdint>
#include "core/ecs/Entity.h"

namespace cm::physics {

// Native -> Managed bridge setup
using AreaEvent_OnBodyEnteredFn = void(*)(int areaEntity, int otherEntity);
using AreaEvent_OnBodyExitedFn  = void(*)(int areaEntity, int otherEntity);
using AreaEvent_OnAreaEnteredFn = void(*)(int areaEntity, int otherEntity);
using AreaEvent_OnAreaExitedFn  = void(*)(int areaEntity, int otherEntity);

void Set_Area_OnBodyEntered_Callback(AreaEvent_OnBodyEnteredFn fn);
void Set_Area_OnBodyExited_Callback(AreaEvent_OnBodyExitedFn fn);
void Set_Area_OnAreaEntered_Callback(AreaEvent_OnAreaEnteredFn fn);
void Set_Area_OnAreaExited_Callback(AreaEvent_OnAreaExitedFn fn);

// Dispatch utility called by AreaSystem
void AreaInterop_Dispatch(int kind, int areaEntity, int otherEntity);

} // namespace cm::physics


