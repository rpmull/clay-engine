#include "TextRenderer.h"
#include "core/ecs/Components.h"
#include "core/ecs/Scene.h"
#include <glm/gtc/type_ptr.hpp>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <cstring>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>
#include "core/vfs/FileSystem.h"
#include "core/vfs/VirtualFS.h"

namespace {
constexpr int kTextGlyphPadding = 18;
constexpr float kMaxShaderOutlinePixels = float(kTextGlyphPadding - 1);
}

// Verify our BakedChar matches stbtt_bakedchar layout
static_assert(sizeof(text_renderer_internal::BakedChar) == sizeof(stbtt_bakedchar), 
              "BakedChar must match stbtt_bakedchar size");
static_assert(alignof(text_renderer_internal::BakedChar) == alignof(stbtt_bakedchar),
              "BakedChar must match stbtt_bakedchar alignment");
#include <filesystem>
#ifndef CLAYMORE_RUNTIME
#include "editor/Project.h"
#endif

namespace {
uint32_t ApplyOpacityToAbgr(uint32_t abgr, float opacityMultiplier) {
    const float opacity = std::max(0.0f, std::min(1.0f, opacityMultiplier));
    const uint8_t a = static_cast<uint8_t>((abgr >> 24) & 0xFFu);
    const float scaledAlpha = (a / 255.0f) * opacity;
    const uint8_t outA = static_cast<uint8_t>(std::round(std::max(0.0f, std::min(1.0f, scaledAlpha)) * 255.0f));
    return (uint32_t(outA) << 24) | (abgr & 0x00FFFFFFu);
}

void BuildOutlineOffsets(float thickness, std::vector<glm::vec2>& outOffsets) {
    outOffsets.clear();
    const float t = std::max(0.0f, thickness);
    if (t <= 0.0f) return;
    outOffsets.push_back(glm::vec2(-t, 0.0f));
    outOffsets.push_back(glm::vec2(t, 0.0f));
    outOffsets.push_back(glm::vec2(0.0f, -t));
    outOffsets.push_back(glm::vec2(0.0f, t));
    outOffsets.push_back(glm::vec2(-t, -t));
    outOffsets.push_back(glm::vec2(-t, t));
    outOffsets.push_back(glm::vec2(t, -t));
    outOffsets.push_back(glm::vec2(t, t));
}

float AlignmentFactor(TextRendererComponent::Alignment alignment) {
    switch (alignment) {
        case TextRendererComponent::Alignment::Center: return 0.5f;
        case TextRendererComponent::Alignment::Right: return 1.0f;
        case TextRendererComponent::Alignment::Left:
        default: return 0.0f;
    }
}

bool IsLineBreak(char ch) {
    return ch == '\n' || ch == '\r';
}

void AdvanceLineBreak(const char*& cursor) {
    if (*cursor == '\r' && cursor[1] == '\n') {
        cursor += 2;
    } else {
        ++cursor;
    }
}

glm::mat4 BuildTextBillboardMatrix(const glm::mat4& worldMatrix, const glm::mat4& viewMatrix) {
    glm::vec3 position = glm::vec3(worldMatrix[3]);
    glm::vec3 scale(
        std::max(glm::length(glm::vec3(worldMatrix[0])), 0.0001f),
        std::max(glm::length(glm::vec3(worldMatrix[1])), 0.0001f),
        std::max(glm::length(glm::vec3(worldMatrix[2])), 0.0001f));

    glm::mat4 cameraWorld = glm::inverse(viewMatrix);
    glm::mat3 cameraRotation(cameraWorld);

    glm::mat4 billboard(1.0f);
    billboard[0] = glm::vec4(cameraRotation[0] * scale.x, 0.0f);
    billboard[1] = glm::vec4(cameraRotation[1] * scale.y, 0.0f);
    billboard[2] = glm::vec4(cameraRotation[2] * scale.z, 0.0f);
    billboard[3] = glm::vec4(position, 1.0f);
    return billboard;
}
}

bgfx::VertexLayout TextRenderer::Vertex::Layout;
void TextRenderer::Vertex::InitLayout() {
    if (Layout.getStride() == 0) {
        Layout.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
            .end();
    }
}

TextRenderer::TextRenderer() {}
TextRenderer::~TextRenderer() {
    ClearFontCache();
    if (bgfx::isValid(m_Sampler)) bgfx::destroy(m_Sampler);
    if (bgfx::isValid(m_TextParams)) bgfx::destroy(m_TextParams);
}

void TextRenderer::ClearFontCache() {
    for (auto& entry : m_FontCache) {
        if (bgfx::isValid(entry.second.atlas)) {
            bgfx::destroy(entry.second.atlas);
            entry.second.atlas = BGFX_INVALID_HANDLE;
        }
    }
    m_FontCache.clear();
    m_Atlas = BGFX_INVALID_HANDLE;
    m_Baked = Baked{};
    m_CurrentFontPath.clear();
    m_CurrentPixelSize = 0.0f;
    m_Ready = false;
}

bool TextRenderer::BakeFont(const std::string& ttfPath, uint16_t w, uint16_t h, float pixelSize, CachedFont& outFont) {
    std::vector<uint8_t> ttf;
    bool loaded = false;
    
    // Try VFS first (for PAK file access at runtime)
    if (VFS::Get() && VFS::Get()->ReadFile(ttfPath, ttf)) {
        std::cout << "[TextRenderer] Loaded font from VFS: " << ttfPath << std::endl;
        loaded = true;
    }
    
    // Fallback to FileSystem (handles both VFS delegation and disk fallback)
    if (!loaded && FileSystem::Instance().ReadFile(ttfPath, ttf)) {
        std::cout << "[TextRenderer] Loaded font from FileSystem: " << ttfPath << std::endl;
        loaded = true;
    }
    
    if (!loaded) {
        // Direct file fallbacks
        auto tryOpen = [&](const std::filesystem::path& p) -> bool {
            FILE* f = fopen(p.string().c_str(), "rb");
            if (!f) return false;
            fseek(f, 0, SEEK_END);
            long size = ftell(f); rewind(f);
            ttf.resize(static_cast<size_t>(size));
            fread(ttf.data(), 1, size, f); fclose(f);
            return true;
        };
        if (tryOpen(std::filesystem::path(ttfPath))) {
            std::cout << "[TextRenderer] Loaded font from direct path: " << ttfPath << std::endl;
            loaded = true;
        }
#ifndef CLAYMORE_RUNTIME
        else {
            std::filesystem::path proj = Project::GetProjectDirectory();
            if (!proj.empty() && tryOpen(proj / ttfPath)) {
                std::cout << "[TextRenderer] Loaded font from project: " << (proj / ttfPath) << std::endl;
                loaded = true;
            } else {
                // Try repo root: parent of project dir
                std::filesystem::path root = proj.parent_path();
                if (tryOpen(root / ttfPath)) {
                    std::cout << "[TextRenderer] Loaded font from repo root: " << (root / ttfPath) << std::endl;
                    loaded = true;
                }
            }
        }
#endif
    }
    
    if (!loaded) {
#ifdef CLAYMORE_RUNTIME
        std::cerr << "[TextRenderer] ERROR: Font not found in PAK: " << ttfPath << std::endl;
        std::cerr << "[TextRenderer] Ensure the font is included in the build export." << std::endl;
#else
        std::cerr << "[TextRenderer] ERROR: Font not found: " << ttfPath << std::endl;
#endif
        return false;
    }

    outFont.baked.chars.resize(96);
    std::vector<unsigned char> pixels;
    pixels.assign(w * h, 0);
    stbtt_pack_context packContext{};
    if (!stbtt_PackBegin(&packContext, pixels.data(), w, h, 0, kTextGlyphPadding, nullptr)) {
        return false;
    }
    stbtt_PackSetOversampling(&packContext, 1, 1);
    std::vector<stbtt_packedchar> packedChars(96);
    const int res = stbtt_PackFontRange(&packContext,
                                        reinterpret_cast<const unsigned char*>(ttf.data()),
                                        0,
                                        pixelSize,
                                        32,
                                        96,
                                        packedChars.data());
    stbtt_PackEnd(&packContext);
    if (res == 0) return false;
    for (size_t i = 0; i < packedChars.size(); ++i) {
        const stbtt_packedchar& src = packedChars[i];
        auto& dst = outFont.baked.chars[i];
        dst.x0 = src.x0;
        dst.y0 = src.y0;
        dst.x1 = src.x1;
        dst.y1 = src.y1;
        dst.xoff = src.xoff;
        dst.yoff = src.yoff;
        dst.xadvance = src.xadvance;
    }
    outFont.baked.width = w; outFont.baked.height = h; outFont.baked.basePixelSize = pixelSize;
    // Extract font vertical metrics to compute baseline/line height precisely
    stbtt_fontinfo info;
    if (stbtt_InitFont(&info, (const unsigned char*)ttf.data(), stbtt_GetFontOffsetForIndex((const unsigned char*)ttf.data(), 0))) {
        int ascent, descent, lineGap;
        stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);
        float scale = stbtt_ScaleForPixelHeight(&info, pixelSize);
        outFont.baked.ascentPx = ascent * scale;
        outFont.baked.descentPx = -descent * scale; // positive value
        outFont.baked.lineGapPx = lineGap * scale;
    }

    const bgfx::Memory* mem = bgfx::copy(pixels.data(), w * h);
    if (bgfx::isValid(outFont.atlas)) bgfx::destroy(outFont.atlas);
    outFont.atlas = bgfx::createTexture2D(w, h, false, 1, bgfx::TextureFormat::R8, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, mem);
    return bgfx::isValid(outFont.atlas);
}

bool TextRenderer::Init(const std::string& ttfPath, bgfx::ProgramHandle program, uint16_t atlasWidth, uint16_t atlasHeight, float basePixelSize) {
    Vertex::InitLayout();
    m_Program = program;
    m_AtlasWidth = atlasWidth;
    m_AtlasHeight = atlasHeight;
    m_DefaultFontPath = ttfPath;
    if (!bgfx::isValid(m_Sampler)) m_Sampler = bgfx::createUniform("s_text", bgfx::UniformType::Sampler);
    if (!bgfx::isValid(m_TextParams)) m_TextParams = bgfx::createUniform("u_textParams", bgfx::UniformType::Vec4);
    ClearFontCache();
    return SetFont(ttfPath, basePixelSize);
}

bool TextRenderer::SetFont(const std::string& ttfPath, float basePixelSize) {
    std::string resolvedPath = ttfPath.empty() ? m_DefaultFontPath : ttfPath;
    if (resolvedPath.empty()) return false;

    auto normalizePixelSize = [](float size) -> float {
        if (size <= 0.0f) return 0.0f;
        return std::round(size * 100.0f) / 100.0f;
    };
    float normalizedSize = normalizePixelSize(basePixelSize);
    if (normalizedSize <= 0.0f) {
        normalizedSize = normalizePixelSize(m_CurrentPixelSize);
        if (normalizedSize <= 0.0f) {
            normalizedSize = 48.0f;
        }
    }

    // Skip if already using this font with the same pixel size
    if (m_CurrentFontPath == resolvedPath && m_CurrentPixelSize == normalizedSize && m_Ready) return true;

    FontKey key{ resolvedPath, normalizedSize };
    auto it = m_FontCache.find(key);
    if (it == m_FontCache.end()) {
        CachedFont newFont;
        uint16_t w = std::max<uint16_t>(m_AtlasWidth ? m_AtlasWidth : 1024, 1024);
        uint16_t h = std::max<uint16_t>(m_AtlasHeight ? m_AtlasHeight : 1024, 1024);
        if (!BakeFont(resolvedPath, w, h, normalizedSize, newFont)) {
            return false;
        }
        auto inserted = m_FontCache.emplace(std::move(key), std::move(newFont));
        it = inserted.first;
    }

    m_Baked = it->second.baked;
    m_Atlas = it->second.atlas;
    m_CurrentFontPath = resolvedPath;
    m_CurrentPixelSize = normalizedSize;
    m_Ready = bgfx::isValid(m_Atlas);
    return m_Ready;
}

void TextRenderer::SetDefaultFontPath(const std::string& ttfPath) {
    m_DefaultFontPath = ttfPath;
}

void TextRenderer::SubmitStringWorld(const TextRendererComponent& tc,
                                     const glm::mat4& world,
                                     bgfx::ViewId viewId,
                                     uint32_t colorAbgr,
                                     const glm::vec2& pixelOffset) {
    if (!bgfx::isValid(m_Program) || !bgfx::isValid(m_Sampler) || !bgfx::isValid(m_Atlas)) {
        return;
    }

    const char* text = tc.Text.c_str();
    const float unitScale = 0.01f; // Map 100 pixels to 1 world unit to avoid huge glyphs
    const float originX = pixelOffset.x * unitScale;
    const float originY = pixelOffset.y * unitScale;
    std::vector<Vertex> vertices;
    std::vector<uint16_t> indices;
    vertices.reserve(strlen(text) * 4);
    indices.reserve(strlen(text) * 6);

    uint32_t color = colorAbgr;
    const float pixelScale = (m_Baked.basePixelSize > 0.0f) ? (tc.PixelSize / m_Baked.basePixelSize) : 1.0f;
    const float scale = pixelScale * unitScale;
    const float lineHeight = ((m_Baked.ascentPx + m_Baked.descentPx + m_Baked.lineGapPx) > 0.0f)
        ? (m_Baked.ascentPx + m_Baked.descentPx + m_Baked.lineGapPx) * pixelScale * unitScale
        : (tc.PixelSize * 1.1f * unitScale);
    const float alignFactor = AlignmentFactor(tc.TextAlignment);

    auto measureLine = [&](const char* begin, const char* end) -> float {
        float lineWidth = 0.0f;
        for (const char* c = begin; c < end; ++c) {
            const unsigned char ch = static_cast<unsigned char>(*c);
            if (ch < 32 || ch >= 128) continue;
            const stbtt_bakedchar* b = ((const stbtt_bakedchar*)m_Baked.chars.data()) + (ch - 32);
            lineWidth += b->xadvance * scale;
        }
        return lineWidth;
    };

    auto emitLine = [&](const char* begin, const char* end, float lineY) {
        float penX = originX - measureLine(begin, end) * alignFactor;
        for (const char* c = begin; c < end; ++c) {
            const unsigned char ch = static_cast<unsigned char>(*c);
            if (ch < 32 || ch >= 128) continue;
            const stbtt_bakedchar* b = ((const stbtt_bakedchar*)m_Baked.chars.data()) + (ch - 32);
            float x0 = penX + b->xoff * scale;
            float y0 = lineY + b->yoff * scale;
            float x1 = x0 + (b->x1 - b->x0) * scale;
            float y1 = y0 + (b->y1 - b->y0) * scale;
            float u0 = b->x0 / float(m_Baked.width);
            float v0 = b->y0 / float(m_Baked.height);
            float u1 = b->x1 / float(m_Baked.width);
            float v1 = b->y1 / float(m_Baked.height);

            uint16_t base = (uint16_t)vertices.size();
            // Flip Y for world space (Y-up world vs baked-down metrics)
            vertices.push_back({ x0, -y0, 0.0f, u0, v0, color });
            vertices.push_back({ x1, -y0, 0.0f, u1, v0, color });
            vertices.push_back({ x1, -y1, 0.0f, u1, v1, color });
            vertices.push_back({ x0, -y1, 0.0f, u0, v1, color });
            indices.push_back(base + 0); indices.push_back(base + 1); indices.push_back(base + 2);
            indices.push_back(base + 0); indices.push_back(base + 2); indices.push_back(base + 3);

            penX += b->xadvance * scale;
        }
    };

    const char* lineBegin = text;
    float lineY = originY;
    for (const char* cursor = text;;) {
        if (*cursor == '\0' || IsLineBreak(*cursor)) {
            emitLine(lineBegin, cursor, lineY);
            if (*cursor == '\0') break;
            AdvanceLineBreak(cursor);
            lineBegin = cursor;
            lineY += lineHeight;
            continue;
        }
        ++cursor;
    }

    if (vertices.empty()) return;

    float mtx[16]; memcpy(mtx, glm::value_ptr(world), sizeof(mtx));
    bgfx::setTransform(mtx);
    const float textParams[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (bgfx::isValid(m_TextParams)) bgfx::setUniform(m_TextParams, textParams);
    bgfx::setTexture(0, m_Sampler, m_Atlas, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    const bgfx::Memory* vmem = bgfx::copy(vertices.data(), (uint32_t)(vertices.size() * sizeof(Vertex)));
    const bgfx::Memory* imem = bgfx::copy(indices.data(), (uint32_t)(indices.size() * sizeof(uint16_t)));
    bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(vmem, Vertex::Layout);
    bgfx::IndexBufferHandle ibh = bgfx::createIndexBuffer(imem);
    bgfx::setVertexBuffer(0, vbh);
    bgfx::setIndexBuffer(ibh);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA | BGFX_STATE_DEPTH_TEST_LEQUAL);
    bgfx::submit(viewId, m_Program);
    bgfx::destroy(vbh);
    bgfx::destroy(ibh);
}

void TextRenderer::SubmitStringWorldOutline(const TextRendererComponent& tc,
                                            const glm::mat4& world,
                                            bgfx::ViewId viewId,
                                            uint32_t colorAbgr,
                                            float outlineThickness) {
    if (!bgfx::isValid(m_Program) || !bgfx::isValid(m_Sampler) || !bgfx::isValid(m_Atlas)) {
        return;
    }

    const float outlinePixels = std::min(std::max(0.0f, outlineThickness), kMaxShaderOutlinePixels);
    if (outlinePixels <= 0.0f) {
        return;
    }

    const char* text = tc.Text.c_str();
    const float unitScale = 0.01f;
    std::vector<Vertex> vertices;
    std::vector<uint16_t> indices;
    vertices.reserve(strlen(text) * 4);
    indices.reserve(strlen(text) * 6);

    const float pixelScale = (m_Baked.basePixelSize > 0.0f) ? (tc.PixelSize / m_Baked.basePixelSize) : 1.0f;
    const float scale = pixelScale * unitScale;
    const float lineHeight = ((m_Baked.ascentPx + m_Baked.descentPx + m_Baked.lineGapPx) > 0.0f)
        ? (m_Baked.ascentPx + m_Baked.descentPx + m_Baked.lineGapPx) * pixelScale * unitScale
        : (tc.PixelSize * 1.1f * unitScale);
    const float alignFactor = AlignmentFactor(tc.TextAlignment);
    const float localExpand = outlinePixels * scale;
    const float uExpand = outlinePixels / float(m_Baked.width);
    const float vExpand = outlinePixels / float(m_Baked.height);

    auto measureLine = [&](const char* begin, const char* end) -> float {
        float lineWidth = 0.0f;
        for (const char* c = begin; c < end; ++c) {
            const unsigned char ch = static_cast<unsigned char>(*c);
            if (ch < 32 || ch >= 128) continue;
            const stbtt_bakedchar* b = ((const stbtt_bakedchar*)m_Baked.chars.data()) + (ch - 32);
            lineWidth += b->xadvance * scale;
        }
        return lineWidth;
    };

    auto emitLine = [&](const char* begin, const char* end, float lineY) {
        float penX = -measureLine(begin, end) * alignFactor;
        for (const char* c = begin; c < end; ++c) {
            const unsigned char ch = static_cast<unsigned char>(*c);
            if (ch < 32 || ch >= 128) continue;
            const stbtt_bakedchar* b = ((const stbtt_bakedchar*)m_Baked.chars.data()) + (ch - 32);
            if (b->x1 > b->x0 && b->y1 > b->y0) {
                float x0 = penX + b->xoff * scale - localExpand;
                float y0 = lineY + b->yoff * scale - localExpand;
                float x1 = penX + b->xoff * scale + (b->x1 - b->x0) * scale + localExpand;
                float y1 = lineY + b->yoff * scale + (b->y1 - b->y0) * scale + localExpand;
                float u0 = b->x0 / float(m_Baked.width) - uExpand;
                float v0 = b->y0 / float(m_Baked.height) - vExpand;
                float u1 = b->x1 / float(m_Baked.width) + uExpand;
                float v1 = b->y1 / float(m_Baked.height) + vExpand;

                uint16_t base = (uint16_t)vertices.size();
                vertices.push_back({ x0, -y0, 0.0f, u0, v0, colorAbgr });
                vertices.push_back({ x1, -y0, 0.0f, u1, v0, colorAbgr });
                vertices.push_back({ x1, -y1, 0.0f, u1, v1, colorAbgr });
                vertices.push_back({ x0, -y1, 0.0f, u0, v1, colorAbgr });
                indices.push_back(base + 0); indices.push_back(base + 1); indices.push_back(base + 2);
                indices.push_back(base + 0); indices.push_back(base + 2); indices.push_back(base + 3);
            }

            penX += b->xadvance * scale;
        }
    };

    const char* lineBegin = text;
    float lineY = 0.0f;
    for (const char* cursor = text;;) {
        if (*cursor == '\0' || IsLineBreak(*cursor)) {
            emitLine(lineBegin, cursor, lineY);
            if (*cursor == '\0') break;
            AdvanceLineBreak(cursor);
            lineBegin = cursor;
            lineY += lineHeight;
            continue;
        }
        ++cursor;
    }

    if (vertices.empty()) return;

    float mtx[16]; memcpy(mtx, glm::value_ptr(world), sizeof(mtx));
    bgfx::setTransform(mtx);
    const float textParams[4] = {
        1.0f,
        outlinePixels / float(m_Baked.width),
        outlinePixels / float(m_Baked.height),
        0.0f
    };
    if (bgfx::isValid(m_TextParams)) bgfx::setUniform(m_TextParams, textParams);
    bgfx::setTexture(0, m_Sampler, m_Atlas, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    const bgfx::Memory* vmem = bgfx::copy(vertices.data(), (uint32_t)(vertices.size() * sizeof(Vertex)));
    const bgfx::Memory* imem = bgfx::copy(indices.data(), (uint32_t)(indices.size() * sizeof(uint16_t)));
    bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(vmem, Vertex::Layout);
    bgfx::IndexBufferHandle ibh = bgfx::createIndexBuffer(imem);
    bgfx::setVertexBuffer(0, vbh);
    bgfx::setIndexBuffer(ibh);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA | BGFX_STATE_DEPTH_TEST_LEQUAL);
    bgfx::submit(viewId, m_Program);
    bgfx::destroy(vbh);
    bgfx::destroy(ibh);
}

void TextRenderer::SubmitStringScreen(const TextRendererComponent& tc,
                                      float x, float y,
                                      uint32_t backbufferWidth,
                                      uint32_t backbufferHeight,
                                      bgfx::ViewId viewId,
                                      uint32_t colorAbgr,
                                      const glm::vec2& pixelOffset) {
    if (!bgfx::isValid(m_Program) || !bgfx::isValid(m_Sampler) || !bgfx::isValid(m_Atlas)) {
        return;
    }

    const char* text = tc.Text.c_str();
    const float scale = (m_Baked.basePixelSize > 0.0f) ? (tc.PixelSize / m_Baked.basePixelSize) : 1.0f;
    const float lineHeight = ((m_Baked.ascentPx + m_Baked.descentPx + m_Baked.lineGapPx) > 0.0f)
        ? (m_Baked.ascentPx + m_Baked.descentPx + m_Baked.lineGapPx) * scale
        : (tc.PixelSize * 1.1f);
    const float alignFactor = AlignmentFactor(tc.TextAlignment);
    std::vector<Vertex> vertices;
    std::vector<uint16_t> indices;
    vertices.reserve(strlen(text) * 4);
    indices.reserve(strlen(text) * 6);
    uint32_t color = colorAbgr;

    auto measureLine = [&](const char* begin, const char* end) -> float {
        float lineWidth = 0.0f;
        for (const char* c = begin; c < end; ++c) {
            const unsigned char ch = static_cast<unsigned char>(*c);
            if (ch < 32 || ch >= 128) continue;
            const stbtt_bakedchar* b = ((const stbtt_bakedchar*)m_Baked.chars.data()) + (ch - 32);
            lineWidth += b->xadvance * scale;
        }
        return lineWidth;
    };

    auto emitLine = [&](const char* begin, const char* end, float lineY) {
        const float textWidth = measureLine(begin, end);
        const float alignOffset = tc.RectSize.x > 0.0f
            ? (tc.RectSize.x - textWidth) * alignFactor
            : -textWidth * alignFactor;
        float penx = x + pixelOffset.x + alignOffset;

        for (const char* c = begin; c < end; ++c) {
            const unsigned char ch = static_cast<unsigned char>(*c);
            if (ch < 32 || ch >= 128) continue;
            const stbtt_bakedchar* b = ((const stbtt_bakedchar*)m_Baked.chars.data()) + (ch - 32);
            float x0 = penx + b->xoff * scale;
            float y0 = lineY + b->yoff * scale;
            float x1 = x0 + (b->x1 - b->x0) * scale;
            float y1 = y0 + (b->y1 - b->y0) * scale;
            float u0 = b->x0 / float(m_Baked.width);
            float v0 = b->y0 / float(m_Baked.height);
            float u1 = b->x1 / float(m_Baked.width);
            float v1 = b->y1 / float(m_Baked.height);

            uint16_t base = (uint16_t)vertices.size();
            vertices.push_back({ x0, y0, 0.0f, u0, v0, color });
            vertices.push_back({ x1, y0, 0.0f, u1, v0, color });
            vertices.push_back({ x1, y1, 0.0f, u1, v1, color });
            vertices.push_back({ x0, y1, 0.0f, u0, v1, color });
            indices.push_back(base + 0); indices.push_back(base + 1); indices.push_back(base + 2);
            indices.push_back(base + 0); indices.push_back(base + 2); indices.push_back(base + 3);

            penx += b->xadvance * scale;
        }
    };

    const char* lineBegin = text;
    float lineY = y + pixelOffset.y;
    for (const char* cursor = text;;) {
        if (*cursor == '\0' || IsLineBreak(*cursor)) {
            emitLine(lineBegin, cursor, lineY);
            if (*cursor == '\0') break;
            AdvanceLineBreak(cursor);
            lineBegin = cursor;
            lineY += lineHeight;
            continue;
        }
        ++cursor;
    }

    if (vertices.empty()) return;

    // Model is identity; view/proj are set by caller for screen view
    float id[16]; bx::mtxIdentity(id); bgfx::setTransform(id);
    const float textParams[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (bgfx::isValid(m_TextParams)) bgfx::setUniform(m_TextParams, textParams);
    bgfx::setTexture(0, m_Sampler, m_Atlas, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    const bgfx::Memory* vmem = bgfx::copy(vertices.data(), (uint32_t)(vertices.size() * sizeof(Vertex)));
    const bgfx::Memory* imem = bgfx::copy(indices.data(), (uint32_t)(indices.size() * sizeof(uint16_t)));
    bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(vmem, Vertex::Layout);
    bgfx::IndexBufferHandle ibh = bgfx::createIndexBuffer(imem);
    bgfx::setVertexBuffer(0, vbh);
    bgfx::setIndexBuffer(ibh);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_ALWAYS | BGFX_STATE_BLEND_ALPHA);
    bgfx::submit(viewId, m_Program);
    bgfx::destroy(vbh);
    bgfx::destroy(ibh);
}

void TextRenderer::SubmitStringScreenWrapped(const TextRendererComponent& tc,
                                             float x, float y,
                                             uint32_t backbufferWidth,
                                             uint32_t backbufferHeight,
                                             bgfx::ViewId viewId,
                                             uint32_t colorAbgr,
                                             const glm::vec2& pixelOffset) {
    if (!bgfx::isValid(m_Program) || !bgfx::isValid(m_Sampler) || !bgfx::isValid(m_Atlas)) {
        return;
    }

    // If no rect or wrap disabled, fallback
    if (tc.RectSize.x <= 0.0f || tc.RectSize.y <= 0.0f || !tc.WordWrap) {
        SubmitStringScreen(tc, x, y, backbufferWidth, backbufferHeight, viewId, colorAbgr, pixelOffset);
        return;
    }

    const char* text = tc.Text.c_str();
    const float scale = (m_Baked.basePixelSize > 0.0f) ? (tc.PixelSize / m_Baked.basePixelSize) : 1.0f;
    float maxWidth = tc.RectSize.x;
    float maxHeight = tc.RectSize.y;
    const float originX = x + pixelOffset.x;
    const float originY = y + pixelOffset.y;

    std::vector<Vertex> vertices; vertices.reserve(strlen(text) * 4);
    std::vector<uint16_t> indices; indices.reserve(strlen(text) * 6);
    uint32_t color = colorAbgr;

    auto emitGlyph = [&](float gx0, float gy0, float gx1, float gy1, float u0, float v0, float u1, float v1) {
        uint16_t base = (uint16_t)vertices.size();
        vertices.push_back({ gx0, gy0, 0.0f, u0, v0, color });
        vertices.push_back({ gx1, gy0, 0.0f, u1, v0, color });
        vertices.push_back({ gx1, gy1, 0.0f, u1, v1, color });
        vertices.push_back({ gx0, gy1, 0.0f, u0, v1, color });
        indices.push_back(base + 0); indices.push_back(base + 1); indices.push_back(base + 2);
        indices.push_back(base + 0); indices.push_back(base + 2); indices.push_back(base + 3);
    };

    auto measureText = [&](const std::string& s)->float {
        float w = 0.0f;
        for (char ch : s) {
            if (ch < 32 || ch >= 128) continue;
            const stbtt_bakedchar* b = ((const stbtt_bakedchar*)m_Baked.chars.data()) + (ch - 32);
            w += b->xadvance * scale;
        }
        return w;
    };

    const stbtt_bakedchar* spaceChar = ((const stbtt_bakedchar*)m_Baked.chars.data()) + (' ' - 32);
    const float spaceWidth = spaceChar->xadvance * scale;
    std::vector<std::string> lines;
    std::string line;
    float lineWidth = 0.0f;

    auto pushLine = [&]() {
        lines.push_back(line);
        line.clear();
        lineWidth = 0.0f;
    };

    const char* cursor = text;
    bool pendingSpace = false;
    while (*cursor) {
        if (IsLineBreak(*cursor)) {
            pushLine();
            pendingSpace = false;
            AdvanceLineBreak(cursor);
            continue;
        }

        if (*cursor == ' ') {
            pendingSpace = true;
            ++cursor;
            continue;
        }

        std::string word;
        while (*cursor && *cursor != ' ' && !IsLineBreak(*cursor)) {
            word.push_back(*cursor);
            ++cursor;
        }

        float wordWidth = measureText(word);
        float prefixWidth = (!line.empty() && pendingSpace) ? spaceWidth : 0.0f;
        if (!line.empty() && lineWidth + prefixWidth + wordWidth > maxWidth && wordWidth < maxWidth) {
            pushLine();
            prefixWidth = 0.0f;
        }

        if (wordWidth <= maxWidth) {
            if (!line.empty() && pendingSpace) {
                line.push_back(' ');
                lineWidth += spaceWidth;
            }
            line += word;
            lineWidth += wordWidth;
        } else {
            for (char ch : word) {
                if (ch < 32 || ch >= 128) continue;
                const stbtt_bakedchar* b = ((const stbtt_bakedchar*)m_Baked.chars.data()) + (ch - 32);
                float adv = b->xadvance * scale;
                if (!line.empty() && lineWidth + adv > maxWidth) {
                    pushLine();
                }
                line.push_back(ch);
                lineWidth += adv;
            }
        }
        pendingSpace = false;
    }
    lines.push_back(line);

    // Align first baseline inside rect: start pen at top + ascent so glyphs fall within rect
    float ascentScaled = (m_Baked.basePixelSize > 0.0f && m_Baked.ascentPx > 0.0f)
        ? (m_Baked.ascentPx * (tc.PixelSize / m_Baked.basePixelSize))
        : (tc.PixelSize * 0.8f);
    float descentScaled = (m_Baked.basePixelSize > 0.0f && m_Baked.descentPx > 0.0f)
        ? (m_Baked.descentPx * (tc.PixelSize / m_Baked.basePixelSize))
        : (tc.PixelSize * 0.2f);
    float lineGapScaled = (m_Baked.basePixelSize > 0.0f)
        ? (m_Baked.lineGapPx * (tc.PixelSize / m_Baked.basePixelSize))
        : (tc.PixelSize * 0.1f);
    float lineHeight = ascentScaled + descentScaled + lineGapScaled;
    float lineY = originY + ascentScaled;
    const float alignFactor = AlignmentFactor(tc.TextAlignment);
    const float rx0 = originX;
    const float ry0 = originY;
    const float rx1 = originX + maxWidth;
    const float ry1 = originY + maxHeight;

    for (const std::string& textLine : lines) {
        if (lineY > originY + maxHeight - descentScaled) break;

        const float currentLineWidth = measureText(textLine);
        float penx = originX + (maxWidth - currentLineWidth) * alignFactor;

        for (char ch : textLine) {
            if (ch < 32 || ch >= 128) continue;
            const stbtt_bakedchar* b = ((const stbtt_bakedchar*)m_Baked.chars.data()) + (ch - 32);
            float x0 = penx + b->xoff * scale;
            float y0 = lineY + b->yoff * scale;
            float x1 = x0 + (b->x1 - b->x0) * scale;
            float y1 = y0 + (b->y1 - b->y0) * scale;
            float u0 = b->x0 / float(m_Baked.width);
            float v0 = b->y0 / float(m_Baked.height);
            float u1 = b->x1 / float(m_Baked.width);
            float v1 = b->y1 / float(m_Baked.height);

            if (x1 < rx0 || x0 > rx1 || y1 < ry0 || y0 > ry1) {
                penx += b->xadvance * scale;
                continue;
            }
            float cgx0 = std::max(x0, rx0);
            float cgy0 = std::max(y0, ry0);
            float cgx1 = std::min(x1, rx1);
            float cgy1 = std::min(y1, ry1);
            float uw = (u1 - u0) / (x1 - x0);
            float vh = (v1 - v0) / (y1 - y0);
            float cu0 = u0 + (cgx0 - x0) * uw;
            float cv0 = v0 + (cgy0 - y0) * vh;
            float cu1 = u1 - (x1 - cgx1) * uw;
            float cv1 = v1 - (y1 - cgy1) * vh;
            emitGlyph(cgx0, cgy0, cgx1, cgy1, cu0, cv0, cu1, cv1);

            penx += b->xadvance * scale;
        }

        lineY += lineHeight;
    }

    if (vertices.empty()) return;

    float id[16]; bx::mtxIdentity(id); bgfx::setTransform(id);
    const float textParams[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (bgfx::isValid(m_TextParams)) bgfx::setUniform(m_TextParams, textParams);
    bgfx::setTexture(0, m_Sampler, m_Atlas, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    const bgfx::Memory* vmem = bgfx::copy(vertices.data(), (uint32_t)(vertices.size() * sizeof(Vertex)));
    const bgfx::Memory* imem = bgfx::copy(indices.data(), (uint32_t)(indices.size() * sizeof(uint16_t)));
    bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(vmem, Vertex::Layout);
    bgfx::IndexBufferHandle ibh = bgfx::createIndexBuffer(imem);
    bgfx::setVertexBuffer(0, vbh);
    bgfx::setIndexBuffer(ibh);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_ALWAYS | BGFX_STATE_BLEND_ALPHA);
    bgfx::submit(viewId, m_Program);
    bgfx::destroy(vbh);
    bgfx::destroy(ibh);
}

void TextRenderer::RenderScreenTexts(const std::vector<std::pair<const TextRendererComponent*, glm::vec2>>& items,
                                     float opacityMultiplier,
                                     uint32_t backbufferWidth,
                                     uint32_t backbufferHeight,
                                     bgfx::ViewId viewId,
                                     uint16_t scissorHandle) {
    if (!bgfx::isValid(m_Program) || !bgfx::isValid(m_Sampler) || !bgfx::isValid(m_Atlas)) return;

    const bgfx::Caps* caps = bgfx::getCaps();
    float ortho[16];
    bx::mtxOrtho(ortho, 0.0f, float(backbufferWidth), float(backbufferHeight), 0.0f, 0.0f, 100.0f, 0.0f, caps->homogeneousDepth);
    float viewIdMat[16]; bx::mtxIdentity(viewIdMat);
    bgfx::setViewTransform(viewId, viewIdMat, ortho);
    bgfx::setViewRect(viewId, 0, 0, (uint16_t)backbufferWidth, (uint16_t)backbufferHeight);

    // Apply scissor if provided (must be set before each submit since bgfx resets state)
    auto applyScissor = [scissorHandle]() {
        if (scissorHandle != UINT16_MAX) {
            bgfx::setScissor(scissorHandle);
        }
    };

    for (const auto& it : items) {
        const TextRendererComponent* tc = it.first;
        if (!tc) continue;
        if (!tc->Visible || tc->Text.empty()) continue;
        const float combinedOpacity = std::max(0.0f, std::min(1.0f, tc->Opacity)) *
                                      std::max(0.0f, std::min(1.0f, opacityMultiplier));
        const uint32_t mainColor = ApplyOpacityToAbgr(tc->ColorAbgr, combinedOpacity);
        const uint32_t outlineColor = ApplyOpacityToAbgr(tc->OutlineColorAbgr, combinedOpacity);
        const uint32_t shadowColor = ApplyOpacityToAbgr(tc->ShadowColorAbgr, combinedOpacity);

        if (!SetFont(tc->FontPath, tc->PixelSize)) continue;
        if (!bgfx::isValid(m_Atlas)) continue;

        auto submitScreenPass = [&](uint32_t passColor, const glm::vec2& passOffset) {
            applyScissor();
            if (tc->WordWrap && tc->RectSize.x > 0.0f && tc->RectSize.y > 0.0f) {
                SubmitStringScreenWrapped(*tc, it.second.x, it.second.y, backbufferWidth, backbufferHeight, viewId, passColor, passOffset);
            } else {
                SubmitStringScreen(*tc, it.second.x, it.second.y, backbufferWidth, backbufferHeight, viewId, passColor, passOffset);
            }
        };

        if (tc->ShadowEnabled && ((shadowColor >> 24) & 0xFFu) != 0u) {
            submitScreenPass(shadowColor, tc->ShadowOffset);
        }

        if (tc->OutlineEnabled && tc->OutlineThickness > 0.0f && ((outlineColor >> 24) & 0xFFu) != 0u) {
            std::vector<glm::vec2> outlineOffsets;
            BuildOutlineOffsets(tc->OutlineThickness, outlineOffsets);
            for (const glm::vec2& offset : outlineOffsets) {
                submitScreenPass(outlineColor, offset);
            }
        }

        if (((mainColor >> 24) & 0xFFu) != 0u) {
            submitScreenPass(mainColor, glm::vec2(0.0f, 0.0f));
        }
    }
}

void TextRenderer::RenderTexts(Scene& scene,
                               const float* viewMtx,
                               const float* projMtx,
                               uint32_t backbufferWidth,
                               uint32_t backbufferHeight,
                               uint16_t worldViewId,
                               uint16_t screenViewId) {
    if (!bgfx::isValid(m_Program)) return;

    const glm::mat4 viewMatrix = glm::make_mat4(viewMtx);
    // For world space texts we assume view/proj already set for worldViewId
    for (auto& e : scene.GetEntities()) {
        auto* data = scene.GetEntityData(e.GetID());
        if (!data || !data->Visible || !data->Active || !data->Text) continue;
        auto& tc = *data->Text;
        if (tc.Text.empty()) continue;

        // Draw world-space texts here; screen-space texts are handled in the UI pass
        if (tc.WorldSpace) {
            if (!SetFont(tc.FontPath, tc.PixelSize)) continue;
            if (!bgfx::isValid(m_Atlas)) continue;
            const float worldOpacity = std::max(0.0f, std::min(1.0f, tc.Opacity));
            const uint32_t mainColor = ApplyOpacityToAbgr(tc.ColorAbgr, worldOpacity);
            const uint32_t outlineColor = ApplyOpacityToAbgr(tc.OutlineColorAbgr, worldOpacity);
            const uint32_t shadowColor = ApplyOpacityToAbgr(tc.ShadowColorAbgr, worldOpacity);

            const glm::mat4 textWorld = tc.Billboard
                ? BuildTextBillboardMatrix(data->Transform.WorldMatrix, viewMatrix)
                : data->Transform.WorldMatrix;

            if (tc.ShadowEnabled && ((shadowColor >> 24) & 0xFFu) != 0u) {
                SubmitStringWorld(tc, textWorld, worldViewId, shadowColor, tc.ShadowOffset);
            }

            if (tc.OutlineEnabled && tc.OutlineThickness > 0.0f && ((outlineColor >> 24) & 0xFFu) != 0u) {
                SubmitStringWorldOutline(tc, textWorld, worldViewId, outlineColor, tc.OutlineThickness);
            }

            if (((mainColor >> 24) & 0xFFu) != 0u) {
                SubmitStringWorld(tc, textWorld, worldViewId, mainColor, glm::vec2(0.0f, 0.0f));
            }
        } else {
            // Defer screen-space rendering to UI pass for correct z-ordering
            continue;
        }
    }
}



