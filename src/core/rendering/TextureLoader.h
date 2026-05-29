#pragma once
// Ensure Windows macro LoadIcon does not collide with our method name
#ifdef LoadIcon
#undef LoadIcon
#endif

#include <bgfx/bgfx.h>
#include <string>
#include <cstdint>

// ImTextureID is ImU64 in modern ImGui (1.91.4+)
// Define it here if not using ImGui, matching imgui's definition
#ifndef ImTextureID
typedef uint64_t ImTextureID;
#endif

// Color space for texture loading
// sRGB: For color textures (albedo, diffuse, emission) - GPU converts to linear on sample
// Linear: For data textures (normal, metallic, roughness, AO, height) - sampled as-is
enum class TextureColorSpace {
    Linear,  // Data textures: normal maps, metallic/roughness, AO, height
    sRGB     // Color textures: albedo, diffuse, emission
};

class TextureLoader
   {
   public:
      // 2-D textures generate and upload full mip chains by default.
      // Pass generateMips=false only for UI/icon/cursor-style assets that should stay base-level only.
      // colorSpace controls whether GPU performs sRGB->linear conversion on sample
      static bgfx::TextureHandle Load2D(const std::string& path, bool generateMips = true, TextureColorSpace colorSpace = TextureColorSpace::Linear);
      
      // Convenience functions for explicit color space handling
      // Use for albedo, diffuse, emission textures - converts sRGB to linear on GPU
      static bgfx::TextureHandle Load2DsRGB(const std::string& path, bool generateMips = true);
      // Use for normal maps, metallic/roughness, AO, height - no conversion
      static bgfx::TextureHandle Load2DLinear(const std::string& path, bool generateMips = true);
      
      static bgfx::TextureHandle LoadIconTexture(const std::string& path);
      static ImTextureID ToImGuiTextureID(bgfx::TextureHandle handle);
      static bgfx::TextureHandle FromImGuiTextureID(ImTextureID textureId);

      // Resolve a texture path using cached fallback aliases.
      // Returns the original path if no alias is known.
      static std::string ResolveTexturePath(const std::string& path);
      static void ResetPathCaches();

      // Load texture from an encoded image in memory (PNG/JPG/TGA/etc.).
      // Decodes via stb_image and uploads RGBA8 to GPU.
      static bgfx::TextureHandle Load2DFromEncodedMemory(const void* data, int sizeBytes, bool generateMips = true, TextureColorSpace colorSpace = TextureColorSpace::Linear);

      // Load texture directly from raw RGBA8 pixels.
      // rowPitchBytes is bytes per row; if 0, it will be computed as width*4.
      static bgfx::TextureHandle Load2DFromRGBA(const void* rgbaPixels, int width, int height, bool generateMips = true, uint32_t rowPitchBytes = 0, TextureColorSpace colorSpace = TextureColorSpace::Linear);
   };
