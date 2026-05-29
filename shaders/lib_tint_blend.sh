// lib_tint_blend.sh - Blender-style color blend modes for tinting
//
// Blend mode constants (matches TintBlendMode enum in C++)
#define TINT_BLEND_NORMAL     0.0
#define TINT_BLEND_MULTIPLY   1.0
#define TINT_BLEND_OVERLAY    2.0
#define TINT_BLEND_ADD        3.0
#define TINT_BLEND_SCREEN     4.0
#define TINT_BLEND_SOFTLIGHT  5.0
#define TINT_BLEND_COLORDODGE 6.0
#define TINT_BLEND_COLORBURN  7.0
#define TINT_BLEND_DIFFERENCE 8.0
#define TINT_BLEND_DETAIL     9.0

// sRGB <-> Linear color space conversion (Blender works in linear space)
vec3 srgbToLinear(vec3 srgb) {
    // Approximate sRGB to linear conversion (gamma 2.2)
    return pow(srgb, vec3(2.2, 2.2, 2.2));
}

vec3 linearToSrgb(vec3 linearColor) {
    // Approximate linear to sRGB conversion (gamma 1/2.2)
    return pow(max(linearColor, vec3(0.0, 0.0, 0.0)), vec3(0.4545, 0.4545, 0.4545));
}

float linearLuma(vec3 color) {
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

vec3 applyLumaPreserveTintChroma(float targetLuma, vec3 linearTint) {
    float tintLuma = max(linearLuma(linearTint), 0.0001);
    vec3 tintChroma = linearTint / tintLuma;
    return clamp(tintChroma * targetLuma, vec3(0.0, 0.0, 0.0), vec3(1.0, 1.0, 1.0));
}

vec3 boostSaturationLinear(vec3 color, float amount) {
    float luma = linearLuma(color);
    vec3 gray = vec3(luma, luma, luma);
    return clamp(mix(gray, color, amount), vec3(0.0, 0.0, 0.0), vec3(1.0, 1.0, 1.0));
}

// Individual blend mode functions
vec3 blendMultiply(vec3 base, vec3 tint) {
    return base * tint;
}

vec3 blendScreen(vec3 base, vec3 tint) {
    return vec3(1.0, 1.0, 1.0) - (vec3(1.0, 1.0, 1.0) - base) * (vec3(1.0, 1.0, 1.0) - tint);
}

vec3 blendOverlayLinear(vec3 base, vec3 tint) {
    // Core overlay blend math (expects linear space inputs)
    // For each channel: if base < 0.5, multiply mode; else screen mode
    vec3 multiply = 2.0 * base * tint;
    vec3 screen = vec3(1.0, 1.0, 1.0) - 2.0 * (vec3(1.0, 1.0, 1.0) - base) * (vec3(1.0, 1.0, 1.0) - tint);
    vec3 t = step(vec3(0.5, 0.5, 0.5), base);
    return mix(multiply, screen, t);
}

// Note: blendOverlay is handled specially in applyTintBlend() for linear space accuracy
// Use blendOverlayLinear() directly when working with linear values

vec3 blendAdd(vec3 base, vec3 tint) {
    return clamp(base + tint, vec3(0.0, 0.0, 0.0), vec3(1.0, 1.0, 1.0));
}

vec3 blendSoftLight(vec3 base, vec3 tint) {
    // Pegtop's formula for soft light
    vec3 result;
    result = (vec3(1.0, 1.0, 1.0) - 2.0 * tint) * base * base + 2.0 * tint * base;
    return result;
}

vec3 blendColorDodge(vec3 base, vec3 tint) {
    vec3 result;
    result.x = tint.x >= 1.0 ? 1.0 : clamp(base.x / (1.0 - tint.x), 0.0, 1.0);
    result.y = tint.y >= 1.0 ? 1.0 : clamp(base.y / (1.0 - tint.y), 0.0, 1.0);
    result.z = tint.z >= 1.0 ? 1.0 : clamp(base.z / (1.0 - tint.z), 0.0, 1.0);
    return result;
}

vec3 blendColorBurn(vec3 base, vec3 tint) {
    vec3 result;
    result.x = tint.x <= 0.0 ? 0.0 : clamp(1.0 - (1.0 - base.x) / tint.x, 0.0, 1.0);
    result.y = tint.y <= 0.0 ? 0.0 : clamp(1.0 - (1.0 - base.y) / tint.y, 0.0, 1.0);
    result.z = tint.z <= 0.0 ? 0.0 : clamp(1.0 - (1.0 - base.z) / tint.z, 0.0, 1.0);
    return result;
}

vec3 blendDifference(vec3 base, vec3 tint) {
    return abs(base - tint);
}

vec3 blendDetailLinear(vec3 linearBase, vec3 linearTint) {
    // Rich detail tinting:
    // - Overlay-like behavior in dark regions.
    // - Luminance-driven detail with tint chroma preservation.
    // - Mild saturation lift so skin stays lively after scene tonemapping.
    float detailLuma = linearLuma(linearBase);
    float tintLuma = linearLuma(linearTint);

    float overlayLuma;
    if (detailLuma < 0.5) {
        overlayLuma = 2.0 * detailLuma * tintLuma;
    } else {
        overlayLuma = 1.0 - 2.0 * (1.0 - detailLuma) * (1.0 - tintLuma);
    }

    float centered = (detailLuma - 0.5) * 2.0;
    float shaped;
    if (centered < 0.0) {
        shaped = -pow(abs(centered), 0.82) * 1.40;
    } else {
        shaped = pow(centered, 1.15) * 0.82;
    }
    float detailScale = exp2(shaped);
    float modulatedLuma = clamp(overlayLuma * detailScale, 0.0, 1.0);
    vec3 lumaDrivenResult = applyLumaPreserveTintChroma(modulatedLuma, linearTint);

    // For sub-mid-gray tones, bias toward Overlay behavior to preserve perceived dark contrast.
    // This keeps detail map dark regions from flattening while still using detail scaling elsewhere.
    vec3 overlayResult = blendOverlayLinear(linearBase, linearTint);
    float darkOverlayWeight = 1.0 - smoothstep(0.18, 0.60, detailLuma);

    vec3 combined = mix(lumaDrivenResult, overlayResult, darkOverlayWeight);
    float satBoost = mix(1.06, 1.18, darkOverlayWeight);
    return boostSaturationLinear(combined, satBoost);
}

// Main blend function - applies tint with specified blend mode
// mask: 0-1 value controlling blend strength (from tint mask texture)
// blendMode: one of the TINT_BLEND_* constants
vec3 applyTintBlend(vec3 baseColor, vec3 tintColor, float mask, float blendMode) {
    vec3 blended;
    
    // Overlay mode uses linear space blending (Blender-accurate)
    if (blendMode >= 1.5 && blendMode < 2.5) {
        // Convert to linear, blend, mix, then back to sRGB
        vec3 linearBase = srgbToLinear(baseColor);
        vec3 linearTint = srgbToLinear(tintColor);
        vec3 linearBlended = blendOverlayLinear(linearBase, linearTint);
        // Mix in linear space for correct blending
        vec3 linearResult = mix(linearBase, linearBlended, mask);
        return linearToSrgb(linearResult);
    }

    // Detail mode is also evaluated in linear space for stable dark detail retention.
    if (blendMode >= 8.5) {
        vec3 linearBase = srgbToLinear(baseColor);
        vec3 linearTint = srgbToLinear(tintColor);
        vec3 linearBlended = blendDetailLinear(linearBase, linearTint);
        vec3 linearResult = mix(linearBase, linearBlended, mask);
        return linearToSrgb(linearResult);
    }
    
    // Other blend modes work in sRGB space
    if (blendMode < 0.5) {
        // Normal/Default: multiply base by tint (same as original behavior)
        blended = baseColor * tintColor;
    } else if (blendMode < 1.5) {
        blended = blendMultiply(baseColor, tintColor);
    } else if (blendMode < 3.5) {
        blended = blendAdd(baseColor, tintColor);
    } else if (blendMode < 4.5) {
        blended = blendScreen(baseColor, tintColor);
    } else if (blendMode < 5.5) {
        blended = blendSoftLight(baseColor, tintColor);
    } else if (blendMode < 6.5) {
        blended = blendColorDodge(baseColor, tintColor);
    } else if (blendMode < 7.5) {
        blended = blendColorBurn(baseColor, tintColor);
    } else if (blendMode < 8.5) {
        blended = blendDifference(baseColor, tintColor);
    } else {
        blended = tintColor;
    }
    
    // Apply mask: blend between original and tinted based on mask value
    return mix(baseColor, blended, mask);
}

// Variant for physically-based shading paths where baseColor is already linear
// (e.g. sampled from sRGB textures with hardware decode).
vec3 applyTintBlendLinear(vec3 linearBaseColor, vec3 tintColorSrgb, float mask, float blendMode) {
    vec3 linearTint = srgbToLinear(tintColorSrgb);
    vec3 blendedLinear;

    if (blendMode >= 1.5 && blendMode < 2.5) {
        vec3 linearBlended = blendOverlayLinear(linearBaseColor, linearTint);
        return mix(linearBaseColor, linearBlended, mask);
    }

    if (blendMode >= 8.5) {
        vec3 linearBlended = blendDetailLinear(linearBaseColor, linearTint);
        return mix(linearBaseColor, linearBlended, mask);
    }

    if (blendMode < 0.5) {
        blendedLinear = linearBaseColor * linearTint;
    } else if (blendMode < 1.5) {
        blendedLinear = blendMultiply(linearBaseColor, linearTint);
    } else if (blendMode < 3.5) {
        blendedLinear = blendAdd(linearBaseColor, linearTint);
    } else if (blendMode < 4.5) {
        blendedLinear = blendScreen(linearBaseColor, linearTint);
    } else if (blendMode < 5.5) {
        blendedLinear = blendSoftLight(linearBaseColor, linearTint);
    } else if (blendMode < 6.5) {
        blendedLinear = blendColorDodge(linearBaseColor, linearTint);
    } else if (blendMode < 7.5) {
        blendedLinear = blendColorBurn(linearBaseColor, linearTint);
    } else if (blendMode < 8.5) {
        blendedLinear = blendDifference(linearBaseColor, linearTint);
    } else {
        blendedLinear = linearTint;
    }

    return mix(linearBaseColor, blendedLinear, mask);
}

