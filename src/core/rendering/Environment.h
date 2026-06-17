#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include <glm/glm.hpp>

class TextureCube; // forward declaration

struct Environment {
    enum class TextureFilterMode {
        Linear,
        Point,
        Anisotropic
    };
    enum class AmbientMode {
        FlatColor,
        Skybox
    };

    AmbientMode Ambient = AmbientMode::FlatColor;
    glm::vec3 AmbientColor = glm::vec3(0.2f);
    float AmbientIntensity = 1.0f;

    bool UseSkybox = false;
    std::shared_ptr<TextureCube> SkyboxTexture = nullptr;
    std::array<std::string, 6> SkyboxFacePaths{};
    bool UseSkyboxEquirectangular = false;
    std::string SkyboxEquirectangularPath;
    uint16_t SkyboxEquirectangularResolution = 1024;

    // Exposure/tonemapping placeholder
    float Exposure = 1.0f;

    // Fog
    bool EnableFog = false;
    glm::vec3 FogColor = glm::vec3(0.5f, 0.6f, 0.7f);
    float FogDensity = 0.02f; // exponential fog density

    // Procedural sky
    bool ProceduralSky = false;
    glm::vec3 SkyTopColor = glm::vec3(0.5f, 0.55f, 0.65f);
    glm::vec3 SkyHorizonColor = glm::vec3(0.65f, 0.6f, 0.55f);
    glm::vec3 SkyGroundColor = glm::vec3(0.369f, 0.349f, 0.341f);
    // Legacy tint fields kept for compatibility with existing shader uniforms.
    glm::vec3 SkyTint = glm::vec3(0.5f, 0.5f, 0.5f);
    glm::vec3 GroundColor = glm::vec3(0.369f, 0.349f, 0.341f);
    float SunSize = 0.04f;
    float SunSizeConvergence = 5.0f;
    float SunIntensity = 1.0f;
    float AtmosphereThickness = 1.0f;
    float HorizonFade = 0.5f;
    float SkyExposure = 1.0f;

    // Screen-space outline (cosmetic)
    bool OutlineEnabled = false;
    glm::vec3 OutlineColor = glm::vec3(0.0f, 0.0f, 0.0f);
    float OutlineThickness = 2.0f; // pixels (1..8)

    // Shadows (directional)
    bool ShadowsEnabled = false;
    int ShadowMapResolution = 1024; // 512..4096
    float ShadowDistance = 50.0f;   // Ortho size covered around camera
    float ShadowBias = 0.0015f;     // Depth bias
    float ShadowNormalBias = 0.5f;  // Normal-based bias scale
    float ShadowSoftness = 1.0f;    // PCF radius in texels
    int ShadowSamples = 9;          // 1,4,9,16
    float ShadowStrength = 1.0f;    // 0..1 multiplier

    // Scene-wide texture sampling preference for default materials
    TextureFilterMode TextureFilter = TextureFilterMode::Linear;

    // Scene-wide texture quality cap. 0 keeps authored size.
    uint16_t TextureMaxDimension = 0;

    // Fixed internal scene render resolution. 0x0 tracks the viewport/window.
    uint16_t RenderResolutionWidth = 0;
    uint16_t RenderResolutionHeight = 0;

    // Runtime-only cache key for lazily rebuilt skybox GPU resources.
    std::string SkyboxRuntimeCacheKey;

    [[nodiscard]] bool HasFixedRenderResolution() const {
        return RenderResolutionWidth > 0 && RenderResolutionHeight > 0;
    }
};
