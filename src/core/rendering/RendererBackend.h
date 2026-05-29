#pragma once

#include <bgfx/bgfx.h>

namespace cm::rendering {

inline bgfx::RendererType::Enum GetDefaultBgfxRendererType()
{
#if defined(_WIN32)
    return bgfx::RendererType::Direct3D11;
#else
    return bgfx::RendererType::Count;
#endif
}

inline const char* DescribeBgfxRendererType(bgfx::RendererType::Enum type)
{
    return type == bgfx::RendererType::Count ? "auto" : bgfx::getRendererName(type);
}

} // namespace cm::rendering
