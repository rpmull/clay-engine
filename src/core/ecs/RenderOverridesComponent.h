#pragma once
#include <string>
#include <cstdint>

// Per-entity render override switches that should not mutate shared materials
struct RenderOverridesComponent {
    // Visibility control
    bool Visible = true;            // Whether this entity is rendered at all
    
    // Shadow control
    bool CastShadows = true;        // Whether this entity casts shadows
    bool ReceiveShadows = true;     // Whether this entity receives shadows
    
    // Blending and depth
    bool AlphaBlendEnabled = false;
    bool UseAlphaCutout = false;
    float AlphaCutoutThreshold = 0.5f;
    bool DepthWriteEnabled = true;
    bool DepthTestEnabled = true;
    
    // Force draw on top (useful for gizmos/highlight). Typically implies no Z-write.
    bool ShowOnTop = false;

    // Sorting/render order (higher values render later/on top)
    int32_t SortingOrder = 0;
    
    // Optional shader override by name (e.g., "PBR", "PSX", or a custom registered one)
    std::string ShaderOverrideName;
};


