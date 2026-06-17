#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <bgfx/bgfx.h>

class TextureCube {
public:
    TextureCube() = default;
    ~TextureCube();

    TextureCube(const TextureCube&) = delete;
    TextureCube& operator=(const TextureCube&) = delete;

    bool LoadFromFaceFiles(const std::array<std::string, 6>& facePaths, uint16_t maxDimension = 0);
    bool LoadFromEquirectangular(const std::string& path, uint16_t targetSize);
    bool IsValid() const { return bgfx::isValid(m_Handle); }
    bgfx::TextureHandle GetHandle() const { return m_Handle; }
    uint16_t GetSize() const { return m_Size; }
    uint8_t GetMipCount() const { return m_MipCount; }

    void Reset();

private:
    bgfx::TextureHandle m_Handle = BGFX_INVALID_HANDLE;
    uint16_t m_Size = 0;
    uint8_t m_MipCount = 0;
};


