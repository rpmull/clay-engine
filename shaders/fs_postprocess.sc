$input v_texcoord0

#include <bgfx_shader.sh>

// Samplers
SAMPLER2D(s_SceneColor, 0);
SAMPLER2D(s_SceneDepth, 1);
SAMPLER2D(s_VelocityBuffer, 2);  // Motion vectors (RG = velocity)

// Post-processing uniforms
uniform vec4 u_PostProcessParams0;  // x=dofEnabled, y=tonemapMode, z=motionBlurEnabled, w=unused
uniform vec4 u_DOFParams;           // x=focusDist, y=focusRange, z=bokehRadius, w=bokehIntensity
uniform vec4 u_TonemapParams;       // x=exposure, y=contrast, z=saturation, w=unused
uniform vec4 u_MotionBlurParams;    // x=intensity, y=samples, z=maxVelocity, w=unused
uniform vec4 u_CameraParams;        // x=nearPlane, y=farPlane, z=unused, w=unused
uniform vec4 u_TexelSize;           // x=1/width, y=1/height, z=width, w=height

// ============================================================================
// Utility Functions
// ============================================================================

float linearizeDepth(float depth, float near, float far) {
    return near * far / (far - depth * (far - near));
}

// ============================================================================
// Depth of Field
// ============================================================================

float getCircleOfConfusion(float depth, float focusDist, float focusRange) {
    float coc = (depth - focusDist) / focusRange;
    return clamp(coc, -1.0, 1.0);
}

vec3 applyDepthOfField(vec2 uv, float depth, float focusDist, float focusRange, float bokehRadius, float bokehIntensity) {
    float coc = abs(getCircleOfConfusion(depth, focusDist, focusRange));
    
    if (coc < 0.01) {
        return texture2DLod(s_SceneColor, uv, 0.0).rgb;
    }
    
    // Bokeh blur using disc samples - fixed 16 samples for shader compilation
    vec3 color = vec3_splat(0.0);
    float totalWeight = 0.0;
    
    // Golden angle spiral sampling for bokeh
    float goldenAngle = 2.39996323;
    float radiusScale = coc * bokehRadius * u_TexelSize.x;
    
    // Precomputed sample offsets for 16 samples (golden angle spiral)
    // Using texture2DLod to avoid gradient issues in the blur pass
    #define DOF_SAMPLE(idx) \
    { \
        float r = sqrt(float(idx) / 16.0); \
        float theta = float(idx) * goldenAngle; \
        vec2 offset = vec2(cos(theta), sin(theta)) * r * radiusScale; \
        vec2 sampleUV = uv + offset; \
        vec3 sampleColor = texture2DLod(s_SceneColor, sampleUV, 0.0).rgb; \
        float luminance = dot(sampleColor, vec3(0.299, 0.587, 0.114)); \
        float weight = 1.0 + luminance * bokehIntensity; \
        color += sampleColor * weight; \
        totalWeight += weight; \
    }
    
    DOF_SAMPLE(0)
    DOF_SAMPLE(1)
    DOF_SAMPLE(2)
    DOF_SAMPLE(3)
    DOF_SAMPLE(4)
    DOF_SAMPLE(5)
    DOF_SAMPLE(6)
    DOF_SAMPLE(7)
    DOF_SAMPLE(8)
    DOF_SAMPLE(9)
    DOF_SAMPLE(10)
    DOF_SAMPLE(11)
    DOF_SAMPLE(12)
    DOF_SAMPLE(13)
    DOF_SAMPLE(14)
    DOF_SAMPLE(15)
    
    #undef DOF_SAMPLE
    
    return color / max(totalWeight, 0.001);
}

// ============================================================================
// Tonemapping
// ============================================================================

// ACES Filmic Tonemapping
vec3 ACESFilm(vec3 x) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Reinhard Tonemapping
vec3 Reinhard(vec3 x) {
    return x / (1.0 + x);
}

// Filmic Tonemapping (Uncharted 2)
vec3 FilmicPartial(vec3 x) {
    float A = 0.15;
    float B = 0.50;
    float C = 0.10;
    float D = 0.20;
    float E = 0.02;
    float F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

vec3 FilmicTonemap(vec3 x) {
    float W = 11.2;
    vec3 whiteScale = vec3_splat(1.0) / FilmicPartial(vec3_splat(W));
    return FilmicPartial(x) * whiteScale;
}

vec3 applyTonemapping(vec3 color, float mode, float exposure, float contrast, float saturation) {
    // Apply exposure
    color *= exposure;
    
    // Apply tonemapping based on mode
    // 0 = None, 1 = ACES, 2 = Reinhard, 3 = Filmic
    if (mode > 0.5 && mode < 1.5) {
        color = ACESFilm(color);
    } else if (mode > 1.5 && mode < 2.5) {
        color = Reinhard(color);
    } else if (mode > 2.5) {
        color = FilmicTonemap(color);
    }
    
    // Apply contrast
    color = (color - 0.5) * contrast + 0.5;
    
    // Apply saturation
    float luminance = dot(color, vec3(0.299, 0.587, 0.114));
    color = mix(vec3_splat(luminance), color, saturation);
    
    return clamp(color, 0.0, 1.0);
}

// ============================================================================
// Motion Blur
// ============================================================================

vec3 applyMotionBlur(vec2 uv, vec3 color, float intensity, float numSamples, float maxVelocity) {
    vec2 velocity = texture2DLod(s_VelocityBuffer, uv, 0.0).rg;
    
    // Clamp velocity to max
    float velocityMag = length(velocity);
    if (velocityMag > maxVelocity * u_TexelSize.x) {
        velocity = normalize(velocity) * maxVelocity * u_TexelSize.x;
    }
    
    velocity *= intensity;
    
    // Skip if velocity is negligible
    if (length(velocity) < 0.0001) {
        return color;
    }
    
    // Fixed 8 samples for motion blur (using texture2DLod to avoid gradient issues)
    vec3 result = color;
    
    #define MB_SAMPLE(idx, total) \
    { \
        float t = float(idx) / float(total); \
        vec2 sampleUV = uv + velocity * (t - 0.5); \
        result += texture2DLod(s_SceneColor, sampleUV, 0.0).rgb; \
    }
    
    MB_SAMPLE(1, 8)
    MB_SAMPLE(2, 8)
    MB_SAMPLE(3, 8)
    MB_SAMPLE(4, 8)
    MB_SAMPLE(5, 8)
    MB_SAMPLE(6, 8)
    MB_SAMPLE(7, 8)
    
    #undef MB_SAMPLE
    
    return result / 8.0;
}

// ============================================================================
// Main
// ============================================================================

void main() {
    vec2 uv = v_texcoord0.xy;
    vec3 color = texture2DLod(s_SceneColor, uv, 0.0).rgb;
    
    bool dofEnabled = u_PostProcessParams0.x > 0.5;
    float tonemapMode = u_PostProcessParams0.y;
    bool motionBlurEnabled = u_PostProcessParams0.z > 0.5;
    
    // Depth of Field
    if (dofEnabled) {
        float rawDepth = texture2DLod(s_SceneDepth, uv, 0.0).r;
        float depth = linearizeDepth(rawDepth, u_CameraParams.x, u_CameraParams.y);
        color = applyDepthOfField(uv, depth, u_DOFParams.x, u_DOFParams.y, u_DOFParams.z, u_DOFParams.w);
    }
    
    // Motion Blur (applied before tonemapping for physically correct blur)
    if (motionBlurEnabled) {
        color = applyMotionBlur(uv, color, u_MotionBlurParams.x, u_MotionBlurParams.y, u_MotionBlurParams.z);
    }
    
    // Tonemapping (always last for correct gamma/color handling)
    if (tonemapMode > 0.0) {
        color = applyTonemapping(color, tonemapMode, u_TonemapParams.x, u_TonemapParams.y, u_TonemapParams.z);
    }
    
    gl_FragColor = vec4(color, 1.0);
}

