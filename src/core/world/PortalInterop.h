#pragma once
#include <cstdint>

namespace cm::world {

// Managed callback signature: portalEntity, otherEntity, entering(1)/exiting(0)
using PortalEvent_OnCrossedFn = void(*)(int, int, int);

void Set_Portal_OnCrossed_Callback(PortalEvent_OnCrossedFn fn);
void PortalInterop_Dispatch(int portalEntity, int otherEntity, int entering);

} // namespace cm::world

extern "C" __declspec(dllexport) void* Get_Portal_SetOnCrossed_Ptr();
