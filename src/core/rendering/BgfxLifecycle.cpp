#include "BgfxLifecycle.h"

#include <atomic>

namespace
{
std::atomic<bool> s_bgfxActive{false};
}

namespace cm::rendering {

bool IsBgfxActive()
{
    return s_bgfxActive.load(std::memory_order_acquire);
}

void SetBgfxActive(bool active)
{
    s_bgfxActive.store(active, std::memory_order_release);
}

} // namespace cm::rendering
