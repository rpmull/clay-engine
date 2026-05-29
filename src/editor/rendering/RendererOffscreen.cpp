#include "core/rendering/Renderer.h"
#include "core/rendering/RenderContext.h"
#include <bgfx/bgfx.h>
#include <unordered_map>

struct OffscreenTarget {
    uint32_t width  = 0;
    uint32_t height = 0;
    bgfx::FrameBufferHandle fb  = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle     tex = BGFX_INVALID_HANDLE;
};
static std::unordered_map<uint16_t, OffscreenTarget> g_OffscreenTargets;

static void DestroyTargetResources(OffscreenTarget& target)
{
    if (bgfx::isValid(target.fb)) {
        // The framebuffer owns its attachments because we create it with
        // destroyTextures=true, so destroying the framebuffer also releases the
        // color/depth textures.
        bgfx::destroy(target.fb);
        target.fb = BGFX_INVALID_HANDLE;
        target.tex = BGFX_INVALID_HANDLE;
    } else if (bgfx::isValid(target.tex)) {
        bgfx::destroy(target.tex);
        target.tex = BGFX_INVALID_HANDLE;
    }

    target.width = 0;
    target.height = 0;
}

static OffscreenTarget& GetOrCreateTarget(uint16_t viewIdBase, uint32_t width, uint32_t height)
{
    OffscreenTarget& target = g_OffscreenTargets[viewIdBase];
    if (!bgfx::isValid(target.fb) || target.width != width || target.height != height)
    {
        DestroyTargetResources(target);

        const uint64_t colorFlags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
        const uint64_t depthFlags = BGFX_TEXTURE_RT_WRITE_ONLY;
        target.tex = bgfx::createTexture2D((uint16_t)width, (uint16_t)height, false, 1, bgfx::TextureFormat::RGBA8, colorFlags);
        bgfx::TextureHandle depth = bgfx::createTexture2D((uint16_t)width, (uint16_t)height, false, 1, bgfx::TextureFormat::D24S8, depthFlags);
        bgfx::TextureHandle attachments[] = { target.tex, depth };
        target.fb  = bgfx::createFrameBuffer(2, attachments, true);
        target.width  = width;
        target.height = height;
    }
    return target;
}

bgfx::TextureHandle Renderer::RenderSceneToTexture(Scene* scene, uint32_t width, uint32_t height, Camera* camera,
    uint16_t viewIdBase, bool showGrid, uint32_t clearColor, bool renderUIOverlay,
    const std::unordered_set<EntityID>* allowedEntities, bool forceFogDisabled)
{
    if (!scene || width == 0 || height == 0 || !camera)
        return BGFX_INVALID_HANDLE;

    // Allocate a dedicated offscreen target for this view range
    OffscreenTarget& target = GetOrCreateTarget(viewIdBase, width, height);

    // Create RenderContext for this offscreen render
    RenderContext ctx;
    ctx.scene = scene;
    ctx.camera = camera;
    ctx.viewId = viewIdBase;
    ctx.uiViewId = (uint16_t)(viewIdBase + 1);
    ctx.width = width;
    ctx.height = height;
    ctx.framebuffer = target.fb;
    ctx.isOffscreen = true;
    ctx.showGrid = showGrid;
    ctx.enableFrustumCulling = true;
    ctx.allowUIInput = false;
    ctx.renderUIOverlay = renderUIOverlay;
    ctx.enableShadows = false;
    ctx.clearColor = clearColor;
    ctx.allowedEntities = allowedEntities;
    ctx.forceFogDisabled = forceFogDisabled;
    ctx.UpdateFromCamera();
    
    // Render using the context (includes scene-filtered particle rendering)
    RenderScene(ctx);

    return target.tex;
}

bgfx::TextureHandle Renderer::EnsureOffscreenTexture(uint16_t viewIdBase, uint32_t width, uint32_t height,
    bgfx::FrameBufferHandle* outFramebuffer)
{
    if (width == 0 || height == 0) {
        if (outFramebuffer) {
            *outFramebuffer = BGFX_INVALID_HANDLE;
        }
        return BGFX_INVALID_HANDLE;
    }

    OffscreenTarget& target = GetOrCreateTarget(viewIdBase, width, height);
    if (outFramebuffer) {
        *outFramebuffer = target.fb;
    }
    return target.tex;
}

void Renderer::ReleaseOffscreenTarget(uint16_t viewIdBase)
{
    auto it = g_OffscreenTargets.find(viewIdBase);
    if (it == g_OffscreenTargets.end()) return;
    DestroyTargetResources(it->second);
    g_OffscreenTargets.erase(it);
}

void Renderer::ReleaseAllOffscreenTargets()
{
    for (auto& kv : g_OffscreenTargets) {
        DestroyTargetResources(kv.second);
    }
    g_OffscreenTargets.clear();
}
