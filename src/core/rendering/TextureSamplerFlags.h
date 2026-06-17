#pragma once

#include <bgfx/bgfx.h>

#include "Environment.h"

namespace cm::rendering {

inline uint32_t GetTextureFilterFlags(Environment::TextureFilterMode mode)
{
    switch (mode) {
    case Environment::TextureFilterMode::Point:
        return BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT;
    case Environment::TextureFilterMode::Anisotropic:
        return BGFX_SAMPLER_MIN_ANISOTROPIC | BGFX_SAMPLER_MAG_ANISOTROPIC;
    case Environment::TextureFilterMode::Linear:
    default:
        return BGFX_SAMPLER_NONE;
    }
}

inline uint32_t GetTextureSamplerFlags(const Environment& env)
{
    return GetTextureFilterFlags(env.TextureFilter);
}

inline uint32_t GetClampTextureSamplerFlags(const Environment& env, bool clampW = false)
{
    uint32_t flags = GetTextureFilterFlags(env.TextureFilter)
        | BGFX_SAMPLER_U_CLAMP
        | BGFX_SAMPLER_V_CLAMP;
    if (clampW) {
        flags |= BGFX_SAMPLER_W_CLAMP;
    }
    return flags;
}

} // namespace cm::rendering
