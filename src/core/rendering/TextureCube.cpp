#include "TextureCube.h"

#include "BgfxLifecycle.h"
#include "core/vfs/FileSystem.h"
#include "core/vfs/VirtualFS.h"

#include <stb_image.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/packing.hpp>
#include <array>
#include <vector>
#include <iostream>
#include <cmath>
#include <algorithm>

namespace
{
    struct FloatImage {
        int width = 0;
        int height = 0;
        std::vector<float> pixels; // RGB
    };

    uint8_t ComputeMipCount(uint16_t size)
    {
        uint8_t mipCount = 1;
        while (size > 1) {
            size = std::max<uint16_t>(1u, static_cast<uint16_t>(size >> 1));
            ++mipCount;
        }
        return mipCount;
    }

    std::vector<uint8_t> GenerateNextMipRGBA8(const std::vector<uint8_t>& src, int srcW, int srcH)
    {
        const int dstW = std::max(1, srcW >> 1);
        const int dstH = std::max(1, srcH >> 1);
        std::vector<uint8_t> dst(static_cast<size_t>(dstW) * static_cast<size_t>(dstH) * 4u, 0u);

        for (int y = 0; y < dstH; ++y) {
            for (int x = 0; x < dstW; ++x) {
                const int sx0 = std::min(srcW - 1, x * 2);
                const int sx1 = std::min(srcW - 1, sx0 + 1);
                const int sy0 = std::min(srcH - 1, y * 2);
                const int sy1 = std::min(srcH - 1, sy0 + 1);

                const size_t i00 = (static_cast<size_t>(sy0) * static_cast<size_t>(srcW) + static_cast<size_t>(sx0)) * 4u;
                const size_t i10 = (static_cast<size_t>(sy0) * static_cast<size_t>(srcW) + static_cast<size_t>(sx1)) * 4u;
                const size_t i01 = (static_cast<size_t>(sy1) * static_cast<size_t>(srcW) + static_cast<size_t>(sx0)) * 4u;
                const size_t i11 = (static_cast<size_t>(sy1) * static_cast<size_t>(srcW) + static_cast<size_t>(sx1)) * 4u;
                const size_t o = (static_cast<size_t>(y) * static_cast<size_t>(dstW) + static_cast<size_t>(x)) * 4u;

                for (int c = 0; c < 4; ++c) {
                    const uint32_t sum = static_cast<uint32_t>(src[i00 + c]) +
                                         static_cast<uint32_t>(src[i10 + c]) +
                                         static_cast<uint32_t>(src[i01 + c]) +
                                         static_cast<uint32_t>(src[i11 + c]);
                    dst[o + c] = static_cast<uint8_t>((sum + 2u) / 4u);
                }
            }
        }

        return dst;
    }

    std::vector<float> GenerateNextMipRGBA32F(const std::vector<float>& src, int srcW, int srcH)
    {
        const int dstW = std::max(1, srcW >> 1);
        const int dstH = std::max(1, srcH >> 1);
        std::vector<float> dst(static_cast<size_t>(dstW) * static_cast<size_t>(dstH) * 4u, 0.0f);

        for (int y = 0; y < dstH; ++y) {
            for (int x = 0; x < dstW; ++x) {
                const int sx0 = std::min(srcW - 1, x * 2);
                const int sx1 = std::min(srcW - 1, sx0 + 1);
                const int sy0 = std::min(srcH - 1, y * 2);
                const int sy1 = std::min(srcH - 1, sy0 + 1);

                const size_t i00 = (static_cast<size_t>(sy0) * static_cast<size_t>(srcW) + static_cast<size_t>(sx0)) * 4u;
                const size_t i10 = (static_cast<size_t>(sy0) * static_cast<size_t>(srcW) + static_cast<size_t>(sx1)) * 4u;
                const size_t i01 = (static_cast<size_t>(sy1) * static_cast<size_t>(srcW) + static_cast<size_t>(sx0)) * 4u;
                const size_t i11 = (static_cast<size_t>(sy1) * static_cast<size_t>(srcW) + static_cast<size_t>(sx1)) * 4u;
                const size_t o = (static_cast<size_t>(y) * static_cast<size_t>(dstW) + static_cast<size_t>(x)) * 4u;

                for (int c = 0; c < 4; ++c) {
                    dst[o + c] = (src[i00 + c] + src[i10 + c] + src[i01 + c] + src[i11 + c]) * 0.25f;
                }
            }
        }

        return dst;
    }

    void DownscaleRGBA8ToMaxDimension(std::vector<uint8_t>& pixels, int& width, int& height, uint16_t maxDimension)
    {
        if (maxDimension == 0) {
            return;
        }

        while (width > static_cast<int>(maxDimension) || height > static_cast<int>(maxDimension)) {
            pixels = GenerateNextMipRGBA8(pixels, width, height);
            width = std::max(1, width >> 1);
            height = std::max(1, height >> 1);
        }
    }

    bool LoadImageAsRGBA8(const std::string& path, int& outWidth, int& outHeight, std::vector<uint8_t>& outPixels)
    {
        stbi_set_flip_vertically_on_load(false);

        std::vector<uint8_t> fileData;
        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc* pixels = nullptr;

        if (VFS::Get() && VFS::Get()->ReadFile(path, fileData)) {
            pixels = stbi_load_from_memory(
                fileData.data(),
                static_cast<int>(fileData.size()),
                &width,
                &height,
                &channels,
                4);
        }

        if (!pixels && FileSystem::Instance().ReadFile(path, fileData)) {
            pixels = stbi_load_from_memory(
                fileData.data(),
                static_cast<int>(fileData.size()),
                &width,
                &height,
                &channels,
                4);
        }
#ifndef CLAYMORE_RUNTIME
        if (!pixels) {
            pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
        }
#endif

        if (!pixels) {
            return false;
        }

        const size_t sizeBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
        outPixels.assign(pixels, pixels + sizeBytes);
        stbi_image_free(pixels);
        outWidth = width;
        outHeight = height;
        return true;
    }

    bool LoadImageAsFloat(const std::string& path, FloatImage& out)
    {
        // Ensure consistent texture orientation - stbi's flip flag is global
        stbi_set_flip_vertically_on_load(false);
        
        std::vector<uint8_t> fileData;
        int width = 0;
        int height = 0;
        int channels = 0;
        const int desiredChannels = 3;
        float* hdrPixels = nullptr;
        stbi_uc* ldrPixels = nullptr;
        bool isHdr = false;

        // Try VFS first (for PAK file access at runtime)
        bool readFromVFS = VFS::Get() && VFS::Get()->ReadFile(path, fileData);
        // Fallback to FileSystem (handles VFS delegation + disk)
        if (!readFromVFS) {
            readFromVFS = FileSystem::Instance().ReadFile(path, fileData);
        }
        
        if (readFromVFS) {
            isHdr = stbi_is_hdr_from_memory(fileData.data(), static_cast<int>(fileData.size())) != 0;
            if (isHdr) {
                hdrPixels = stbi_loadf_from_memory(
                    fileData.data(),
                    static_cast<int>(fileData.size()),
                    &width, &height, &channels, desiredChannels);
            } else {
                ldrPixels = stbi_load_from_memory(
                    fileData.data(),
                    static_cast<int>(fileData.size()),
                    &width, &height, &channels, desiredChannels);
            }
        }
#ifndef CLAYMORE_RUNTIME
        // Direct disk fallback for editor only
        if (!hdrPixels && !ldrPixels) {
            isHdr = stbi_is_hdr(path.c_str()) != 0;
            if (isHdr) {
                hdrPixels = stbi_loadf(path.c_str(), &width, &height, &channels, desiredChannels);
            } else {
                ldrPixels = stbi_load(path.c_str(), &width, &height, &channels, desiredChannels);
            }
        }
#endif

        if (!hdrPixels && !ldrPixels) {
            std::cerr << "[TextureCube] Failed to load skybox source: " << path << std::endl;
            return false;
        }

        out.width = width;
        out.height = height;
        out.pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * desiredChannels);

        if (hdrPixels) {
            const size_t total = static_cast<size_t>(width) * static_cast<size_t>(height) * desiredChannels;
            std::copy(hdrPixels, hdrPixels + total, out.pixels.begin());
            stbi_image_free(hdrPixels);
        } else {
            const size_t total = static_cast<size_t>(width) * static_cast<size_t>(height) * desiredChannels;
            for (size_t i = 0; i < total; ++i) {
                // LDR sky textures are authored in sRGB; convert to linear radiance.
                const float srgb = static_cast<float>(ldrPixels[i]) / 255.0f;
                out.pixels[i] = std::pow(std::max(srgb, 0.0f), 2.2f);
            }
            stbi_image_free(ldrPixels);
        }

        return true;
    }

    inline glm::vec3 SamplePixel(const FloatImage& img, int x, int y)
    {
        if (img.width <= 0 || img.height <= 0) {
            return glm::vec3(0.0f);
        }
        x = std::clamp(x, 0, img.width - 1);
        y = std::clamp(y, 0, img.height - 1);
        const size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(img.width) + static_cast<size_t>(x)) * 3u;
        return glm::vec3(
            img.pixels[idx + 0],
            img.pixels[idx + 1],
            img.pixels[idx + 2]);
    }

    glm::vec3 SampleEquirectangular(const FloatImage& img, const glm::vec3& dir)
    {
        if (img.width <= 0 || img.height <= 0) {
            return glm::vec3(0.0f);
        }

        float theta = std::atan2(dir.z, dir.x);               // -pi .. pi
        float phi = std::acos(glm::clamp(dir.y, -1.0f, 1.0f)); // 0 .. pi
        float u = theta / (2.0f * glm::pi<float>()) + 0.5f;
        float v = phi / glm::pi<float>();
        u = u - std::floor(u); // wrap horizontally
        v = glm::clamp(v, 0.0f, 1.0f);

        const int widthMinusOne = std::max(1, img.width - 1);
        const int heightMinusOne = std::max(1, img.height - 1);
        float px = u * static_cast<float>(widthMinusOne);
        float py = v * static_cast<float>(heightMinusOne);

        int x0 = static_cast<int>(std::floor(px));
        int y0 = static_cast<int>(std::floor(py));
        int x1 = (x0 + 1) % img.width;
        int y1 = std::min(y0 + 1, img.height - 1);

        float tx = px - static_cast<float>(x0);
        float ty = py - static_cast<float>(y0);

        glm::vec3 c00 = SamplePixel(img, x0, y0);
        glm::vec3 c10 = SamplePixel(img, x1, y0);
        glm::vec3 c01 = SamplePixel(img, x0, y1);
        glm::vec3 c11 = SamplePixel(img, x1, y1);

        glm::vec3 cx0 = glm::mix(c00, c10, tx);
        glm::vec3 cx1 = glm::mix(c01, c11, tx);
        return glm::mix(cx0, cx1, ty);
    }

    glm::vec3 CubemapDirectionFromFace(uint8_t face, float u, float v)
    {
        switch (face)
        {
        case 0: return glm::normalize(glm::vec3( 1.0f,       v,      -u)); // +X
        case 1: return glm::normalize(glm::vec3(-1.0f,       v,       u)); // -X
        case 2: return glm::normalize(glm::vec3(    u,   1.0f,       v)); // +Y
        case 3: return glm::normalize(glm::vec3(    u,  -1.0f,      -v)); // -Y
        case 4: return glm::normalize(glm::vec3(    u,       v,   1.0f)); // +Z
        case 5: return glm::normalize(glm::vec3(   -u,       v,  -1.0f)); // -Z
        default: return glm::vec3(0.0f, 1.0f, 0.0f);
        }
    }

    void UploadFaceMipChainRGBA8(bgfx::TextureHandle handle,
                                 uint8_t face,
                                 int width,
                                 int height,
                                 std::vector<uint8_t> pixels)
    {
        uint8_t mip = 0;
        while (bgfx::isValid(handle)) {
            const bgfx::Memory* mem = bgfx::copy(pixels.data(), static_cast<uint32_t>(pixels.size()));
            bgfx::updateTextureCube(
                handle,
                0,
                face,
                mip,
                0,
                0,
                static_cast<uint16_t>(width),
                static_cast<uint16_t>(height),
                mem);

            if (width == 1 && height == 1) {
                break;
            }

            pixels = GenerateNextMipRGBA8(pixels, width, height);
            width = std::max(1, width >> 1);
            height = std::max(1, height >> 1);
            ++mip;
        }
    }

    void UploadFaceMipChainFloat(bgfx::TextureHandle handle,
                                 uint8_t face,
                                 int width,
                                 int height,
                                 std::vector<float> pixels,
                                 bgfx::TextureFormat::Enum format)
    {
        uint8_t mip = 0;
        while (bgfx::isValid(handle)) {
            if (format == bgfx::TextureFormat::RGBA16F) {
                std::vector<uint16_t> halfPixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
                for (size_t i = 0; i < halfPixels.size(); ++i) {
                    halfPixels[i] = glm::packHalf1x16(pixels[i]);
                }
                const bgfx::Memory* mem = bgfx::copy(
                    halfPixels.data(),
                    static_cast<uint32_t>(halfPixels.size() * sizeof(uint16_t)));
                bgfx::updateTextureCube(
                    handle,
                    0,
                    face,
                    mip,
                    0,
                    0,
                    static_cast<uint16_t>(width),
                    static_cast<uint16_t>(height),
                    mem);
            } else {
                const bgfx::Memory* mem = bgfx::copy(
                    pixels.data(),
                    static_cast<uint32_t>(pixels.size() * sizeof(float)));
                bgfx::updateTextureCube(
                    handle,
                    0,
                    face,
                    mip,
                    0,
                    0,
                    static_cast<uint16_t>(width),
                    static_cast<uint16_t>(height),
                    mem);
            }

            if (width == 1 && height == 1) {
                break;
            }

            pixels = GenerateNextMipRGBA32F(pixels, width, height);
            width = std::max(1, width >> 1);
            height = std::max(1, height >> 1);
            ++mip;
        }
    }
}

TextureCube::~TextureCube() {
    Reset();
}

void TextureCube::Reset() {
    cm::rendering::SafeDestroyHandle(m_Handle);
    m_Size = 0;
    m_MipCount = 0;
}

bool TextureCube::LoadFromFaceFiles(const std::array<std::string, 6>& facePaths, uint16_t maxDimension) {
    Reset();

    int baseWidth = 0;
    int baseHeight = 0;
    uint16_t uploadSize = 0;
    const uint64_t flags =
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_W_CLAMP |
        BGFX_TEXTURE_SRGB;

    for (int face = 0; face < 6; ++face) {
        const std::string& path = facePaths[face];
        if (path.empty()) {
            std::cerr << "[TextureCube] Missing path for face " << face << std::endl;
            Reset();
            return false;
        }

        int width = 0;
        int height = 0;
        std::vector<uint8_t> pixels;
        if (!LoadImageAsRGBA8(path, width, height, pixels)) {
            std::cerr << "[TextureCube] Failed to load cubemap face: " << path << std::endl;
            Reset();
            return false;
        }

        if (width != height) {
            std::cerr << "[TextureCube] Cubemap face is not square: " << path << std::endl;
            Reset();
            return false;
        }

        if (face == 0) {
            baseWidth = width;
            baseHeight = height;
        } else if (width != baseWidth || height != baseHeight) {
            std::cerr << "[TextureCube] Cubemap faces must share identical resolution." << std::endl;
            Reset();
            return false;
        }

        DownscaleRGBA8ToMaxDimension(pixels, width, height, maxDimension);

        if (!bgfx::isValid(m_Handle)) {
            uploadSize = static_cast<uint16_t>(std::max(1, width));
            if (!bgfx::isTextureValid(0, true, 1, bgfx::TextureFormat::RGBA8, flags)) {
                std::cerr << "[TextureCube] RGBA8 cubemap format is unsupported." << std::endl;
                Reset();
                return false;
            }

            m_Handle = bgfx::createTextureCube(
                uploadSize,
                true,
                1,
                bgfx::TextureFormat::RGBA8,
                flags);

            if (!bgfx::isValid(m_Handle)) {
                std::cerr << "[TextureCube] Failed to create bgfx cubemap texture." << std::endl;
                Reset();
                return false;
            }

            m_Size = uploadSize;
            m_MipCount = ComputeMipCount(uploadSize);
        }

        UploadFaceMipChainRGBA8(m_Handle, static_cast<uint8_t>(face), width, height, std::move(pixels));
    }

    if (baseWidth == 0 || baseHeight == 0 || !bgfx::isValid(m_Handle)) {
        std::cerr << "[TextureCube] Invalid cubemap resolution." << std::endl;
        Reset();
        return false;
    }

    return true;
}

bool TextureCube::LoadFromEquirectangular(const std::string& path, uint16_t targetSize)
{
    Reset();

    const uint16_t size = std::max<uint16_t>(targetSize, 1u);
    FloatImage source;
    if (!LoadImageAsFloat(path, source) || source.width <= 0 || source.height <= 0) {
        std::cerr << "[TextureCube] Invalid equirectangular texture: " << path << std::endl;
        return false;
    }

    const uint64_t flags =
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_W_CLAMP;
    bgfx::TextureFormat::Enum format = bgfx::TextureFormat::RGBA16F;
    if (!bgfx::isTextureValid(0, true, 1, format, flags)) {
        format = bgfx::TextureFormat::RGBA32F;
    }
    if (!bgfx::isTextureValid(0, true, 1, format, flags)) {
        std::cerr << "[TextureCube] No floating-point cubemap format is supported for skyboxes." << std::endl;
        return false;
    }

    m_Handle = bgfx::createTextureCube(
        size,
        true,
        1,
        format,
        flags);

    if (!bgfx::isValid(m_Handle)) {
        std::cerr << "[TextureCube] Failed to allocate cubemap for equirectangular source." << std::endl;
        Reset();
        return false;
    }

    for (uint8_t face = 0; face < 6; ++face) {
        std::vector<float> facePixels(static_cast<size_t>(size) * static_cast<size_t>(size) * 4u, 0.0f);
        for (uint16_t y = 0; y < size; ++y) {
            const float fy = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
            const float v = fy * 2.0f - 1.0f;
            for (uint16_t x = 0; x < size; ++x) {
                const float fx = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
                const float u = fx * 2.0f - 1.0f;
                const glm::vec3 dir = CubemapDirectionFromFace(face, u, v);
                const glm::vec3 color = SampleEquirectangular(source, dir);
                const size_t idx = (static_cast<size_t>(y) * size + x) * 4u;
                facePixels[idx + 0] = color.r;
                facePixels[idx + 1] = color.g;
                facePixels[idx + 2] = color.b;
                facePixels[idx + 3] = 1.0f;
            }
        }
        UploadFaceMipChainFloat(m_Handle, face, size, size, std::move(facePixels), format);
    }

    m_Size = size;
    m_MipCount = ComputeMipCount(size);
    return true;
}


