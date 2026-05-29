#include "AreaInterop.h"
#include <atomic>

namespace cm::physics {

static std::atomic<AreaEvent_OnBodyEnteredFn> s_OnBodyEntered{ nullptr };
static std::atomic<AreaEvent_OnBodyExitedFn>  s_OnBodyExited{ nullptr };
static std::atomic<AreaEvent_OnAreaEnteredFn> s_OnAreaEntered{ nullptr };
static std::atomic<AreaEvent_OnAreaExitedFn>  s_OnAreaExited{ nullptr };

void Set_Area_OnBodyEntered_Callback(AreaEvent_OnBodyEnteredFn fn) { s_OnBodyEntered.store(fn, std::memory_order_relaxed); }
void Set_Area_OnBodyExited_Callback (AreaEvent_OnBodyExitedFn fn)  { s_OnBodyExited.store(fn, std::memory_order_relaxed); }
void Set_Area_OnAreaEntered_Callback(AreaEvent_OnAreaEnteredFn fn) { s_OnAreaEntered.store(fn, std::memory_order_relaxed); }
void Set_Area_OnAreaExited_Callback (AreaEvent_OnAreaExitedFn fn)  { s_OnAreaExited.store(fn, std::memory_order_relaxed); }

void AreaInterop_Dispatch(int kind, int areaEntity, int otherEntity)
{
    switch (kind) {
        case 0: if (auto f = s_OnBodyEntered.load(std::memory_order_relaxed)) f(areaEntity, otherEntity); break;
        case 1: if (auto f = s_OnBodyExited.load(std::memory_order_relaxed))  f(areaEntity, otherEntity); break;
        case 2: if (auto f = s_OnAreaEntered.load(std::memory_order_relaxed)) f(areaEntity, otherEntity); break;
        case 3: if (auto f = s_OnAreaExited.load(std::memory_order_relaxed))  f(areaEntity, otherEntity); break;
        default: break;
    }
}

} // namespace cm::physics

extern "C" __declspec(dllexport) void* Get_Area_SetOnBodyEntered_Ptr() {
    return (void*)(&cm::physics::Set_Area_OnBodyEntered_Callback);
}
extern "C" __declspec(dllexport) void* Get_Area_SetOnBodyExited_Ptr() {
    return (void*)(&cm::physics::Set_Area_OnBodyExited_Callback);
}
extern "C" __declspec(dllexport) void* Get_Area_SetOnAreaEntered_Ptr() {
    return (void*)(&cm::physics::Set_Area_OnAreaEntered_Callback);
}
extern "C" __declspec(dllexport) void* Get_Area_SetOnAreaExited_Ptr() {
    return (void*)(&cm::physics::Set_Area_OnAreaExited_Callback);
}


