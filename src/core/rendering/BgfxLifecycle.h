#pragma once

#include <bgfx/bgfx.h>

namespace cm::rendering {

bool IsBgfxActive();
void SetBgfxActive(bool active);

template <typename HandleT>
inline void SafeDestroyHandle(HandleT& handle)
{
    if (!bgfx::isValid(handle)) {
        return;
    }

    if (IsBgfxActive()) {
        bgfx::destroy(handle);
    }

    handle = BGFX_INVALID_HANDLE;
}

} // namespace cm::rendering
