#pragma once
#include <bgfx/bgfx.h>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <functional>
#include <glm/glm.hpp>

// Local definition matching stb_truetype's stbtt_bakedchar
// This avoids conflicts with imgui's bundled stb_truetype
namespace text_renderer_internal {
    struct BakedChar {
        unsigned short x0, y0, x1, y1;
        float xoff, yoff, xadvance;
    };
}

// Lightweight text rendering using stb_truetype baked atlas
// Bakes ASCII 32..126 from a TTF into an R8 atlas and renders with a simple shader

class Scene;
struct TextRendererComponent;

class TextRenderer {
public:
    TextRenderer();
    ~TextRenderer();

    // Initialize with default font path and shader program
    bool Init(const std::string& ttfPath, bgfx::ProgramHandle program, uint16_t atlasWidth = 512, uint16_t atlasHeight = 512, float basePixelSize = 48.0f);
    // Load/switch font at runtime; returns true if atlas re-baked
    bool SetFont(const std::string& ttfPath, float basePixelSize = 48.0f);
    // Update the default font path used when no override is provided
    void SetDefaultFontPath(const std::string& ttfPath);

    // Render all TextRendererComponent instances in the scene
    void RenderTexts(Scene& scene,
                     const float* viewMtx,
                     const float* projMtx,
                     uint32_t backbufferWidth,
                     uint32_t backbufferHeight,
                     uint16_t worldViewId = 1,
                     uint16_t screenViewId = 0);

    // Render only screen-space texts with given order (already sorted) and apply opacity multiplier
    // scissorHandle: UINT16_MAX means no scissor, otherwise use cached scissor rect
    void RenderScreenTexts(const std::vector<std::pair<const TextRendererComponent*, glm::vec2>>& items,
                           float opacityMultiplier,
                           uint32_t backbufferWidth,
                           uint32_t backbufferHeight,
                           bgfx::ViewId screenViewId,
                           uint16_t scissorHandle = UINT16_MAX);

    // Submit one screen-space text with extra opacity multiplier
    void SubmitStringScreenWithOpacity(const TextRendererComponent& tc,
                                       float x, float y,
                                       uint32_t backbufferWidth,
                                       uint32_t backbufferHeight,
                                       float opacityMultiplier,
                                       bgfx::ViewId viewId);

private:
    struct Baked {
        std::vector<text_renderer_internal::BakedChar> chars; // 95 glyphs (32..126)
        uint16_t width = 0;
        uint16_t height = 0;
        float basePixelSize = 48.0f;
        // Font vertical metrics at basePixelSize (pixels)
        float ascentPx = 0.0f;
        float descentPx = 0.0f;
        float lineGapPx = 0.0f;
    };

    struct CachedFont {
        Baked baked;
        bgfx::TextureHandle atlas = BGFX_INVALID_HANDLE;
    };

    struct FontKey {
        std::string path;
        float pixelSize = 0.0f;
        bool operator==(const FontKey& other) const {
            return path == other.path && pixelSize == other.pixelSize;
        }
    };

    struct FontKeyHasher {
        size_t operator()(const FontKey& key) const {
            size_t h1 = std::hash<std::string>{}(key.path);
            size_t h2 = std::hash<float>{}(key.pixelSize);
            return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
        }
    };

    struct Vertex {
        float x, y, z;
        float u, v;
        uint32_t abgr;
        static bgfx::VertexLayout Layout;
        static void InitLayout();
    };

    bool BakeFont(const std::string& ttfPath, uint16_t w, uint16_t h, float pixelSize, CachedFont& outFont);
    void ClearFontCache();

    void SubmitStringWorld(const TextRendererComponent& tc,
                           const glm::mat4& world,
                           bgfx::ViewId viewId,
                           uint32_t colorAbgr,
                           const glm::vec2& pixelOffset);

    void SubmitStringWorldOutline(const TextRendererComponent& tc,
                                  const glm::mat4& world,
                                  bgfx::ViewId viewId,
                                  uint32_t colorAbgr,
                                  float outlineThickness);

    void SubmitStringScreen(const TextRendererComponent& tc,
                            float x, float y,
                            uint32_t backbufferWidth,
                            uint32_t backbufferHeight,
                            bgfx::ViewId viewId,
                            uint32_t colorAbgr,
                            const glm::vec2& pixelOffset);

    void SubmitStringScreenWrapped(const TextRendererComponent& tc,
                                   float x, float y,
                                   uint32_t backbufferWidth,
                                   uint32_t backbufferHeight,
                                   bgfx::ViewId viewId,
                                   uint32_t colorAbgr,
                                   const glm::vec2& pixelOffset);

    bgfx::TextureHandle m_Atlas = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_Sampler = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_TextParams = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_Program = BGFX_INVALID_HANDLE;

    Baked m_Baked;
    bool m_Ready = false;
    std::string m_CurrentFontPath;  // Track current font to avoid reloading every frame
    float m_CurrentPixelSize = 0.0f;  // Track current pixel size to avoid re-baking same font
    std::string m_DefaultFontPath;
    uint16_t m_AtlasWidth = 512;
    uint16_t m_AtlasHeight = 512;
    std::unordered_map<FontKey, CachedFont, FontKeyHasher> m_FontCache;
};


