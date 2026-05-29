#include "PortalInterop.h"
#include <atomic>

namespace cm::world {

static std::atomic<PortalEvent_OnCrossedFn> s_OnCrossed{ nullptr };

void Set_Portal_OnCrossed_Callback(PortalEvent_OnCrossedFn fn)
{
    s_OnCrossed.store(fn, std::memory_order_relaxed);
}

void PortalInterop_Dispatch(int portalEntity, int otherEntity, int entering)
{
    if (auto fn = s_OnCrossed.load(std::memory_order_relaxed)) {
        fn(portalEntity, otherEntity, entering);
    }
}

} // namespace cm::world

extern "C" __declspec(dllexport) void* Get_Portal_SetOnCrossed_Ptr()
{
    return (void*)(&cm::world::Set_Portal_OnCrossed_Callback);
}
