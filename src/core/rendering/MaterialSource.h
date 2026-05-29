#pragma once

#include <vector>
#include <string>
#include <glm/glm.hpp>

struct EmbeddedTextureData
{
    std::vector<uint8_t> Bytes;
    int Width = 0;
    int Height = 0;
    bool IsCompressed = false;
    std::string FormatHint;

    bool HasData() const { return !Bytes.empty(); }
};

struct TextureSpecifier
{
    std::string Path;
    EmbeddedTextureData Embedded;
};

struct MaterialSource
{
    std::string Name;
    bool Skinned = false;
    bool AlphaBlend = false;
    bool AlphaCutout = false;
    float AlphaCutoutThreshold = 0.5f;
    bool TwoSided = false;
    bool HasTint = false;
    glm::vec4 ColorTint = glm::vec4(1.0f);
    TextureSpecifier Albedo;
    TextureSpecifier MetallicRoughness;
    TextureSpecifier Normal;
    TextureSpecifier AO;
    TextureSpecifier Emission;
    TextureSpecifier Displacement;
};


