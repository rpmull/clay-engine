$input v_worldPos, v_normal, v_texcoord0, v_viewDir
$output

#include <bgfx_shader.sh>
#include "lib_tint_blend.sh"

SAMPLER2D(s_albedo, 0);
SAMPLER2D(s_tintMask, 3);
uniform vec4 u_ColorTint;
uniform vec4 u_TintParams;  // x = blend mode (0-8), y = unused, z = unused, w = unused
uniform vec4 u_psxParams; // x=jitter_amp_px, y=affine_factor, z=light_influence [0..1], w=normalPerturbAmp
uniform vec4 u_toonParams; // x=bands (>0), y=softness [0..1], z,w unused
uniform vec4 u_posterize; // x=levels (0=off, >=1 quantization levels), yzw unused

// Minimal lighting inputs (reuse PBR light uniforms for consistency)
uniform vec4 u_lightColors[5];     // rgb=color, a=intensity
uniform vec4 u_lightPositions[5];  // xyz=dir/pos, w=type (0=dir,1=point)
uniform vec4 u_lightParams[5];     // x=range, y=const, z=linear, w=quadratic
uniform vec4 u_ambientFog;         // xyz=ambient color, w=fog enabled flag
uniform vec4 u_fogParams;          // x=fogDensity, yzw=fog color
uniform vec4 u_cameraPos;

// Shadowing
SAMPLER2DSHADOW(s_shadowMap, 7);
uniform mat4 u_lightViewProj;
uniform mat4 u_lightViewProjCSM[4];
uniform vec4 u_CascadeSplits;    // xyz = splits, w = count
uniform vec4 u_CascadeScaleBias[4]; // xy scale, zw bias into atlas
uniform vec4 u_shadowParams;     // x=bias, y=normalBias, z=softness, w=strength
uniform vec4 u_shadowTexelSize;  // x=1/w, y=1/h, z=samples, w=originBottomLeft
uniform vec4 u_shadowLightDir;   // xyz = light dir (world)
uniform vec4 u_shadowReceive;    // x = receive shadows (0/1)
SAMPLER2D(s_pointShadowMap, 8);
uniform vec4 u_pointShadowMeta[4];     // x=enabled, y=light slot, z=range, w=bias
uniform vec4 u_pointShadowLightPos[4]; // xyz=light position
uniform vec4 u_pointShadowAtlas;    // x=tileCols, y=tileRows, z=1/atlasW, w=1/atlasH

vec2 PointShadowFaceUV(vec3 dir, out int faceIndex) {
    vec3 ad = abs(dir);
    vec2 uv;
    // Must match face view basis used by bx::mtxLookAt in shadow pass.
    // Horizontal axis uses right = cross(up, forward), so signs differ from
    // some cubemap lookup tables that assume cross(forward, up).
    if (ad.x >= ad.y && ad.x >= ad.z) {
        if (dir.x >= 0.0) { faceIndex = 0; uv = vec2( dir.z, dir.y) / max(ad.x, 1e-6); }
        else              { faceIndex = 1; uv = vec2(-dir.z, dir.y) / max(ad.x, 1e-6); }
    } else if (ad.y >= ad.x && ad.y >= ad.z) {
        if (dir.y >= 0.0) { faceIndex = 2; uv = vec2(-dir.x,-dir.z) / max(ad.y, 1e-6); }
        else              { faceIndex = 3; uv = vec2(-dir.x, dir.z) / max(ad.y, 1e-6); }
    } else {
        if (dir.z >= 0.0) { faceIndex = 4; uv = vec2(-dir.x, dir.y) / max(ad.z, 1e-6); }
        else              { faceIndex = 5; uv = vec2( dir.x, dir.y) / max(ad.z, 1e-6); }
    }
    return uv * 0.5 + 0.5;
}

float SamplePointShadow(vec3 worldPos, int shadowIdx) {
    vec4 shadowMeta = u_pointShadowMeta[shadowIdx];
    if (shadowMeta.x < 0.5 || shadowMeta.z <= 0.0) {
        return 1.0;
    }
    vec3 toFrag = worldPos - u_pointShadowLightPos[shadowIdx].xyz;
    float currentDepth = length(toFrag) / max(shadowMeta.z, 1e-6);
    currentDepth = clamp(currentDepth, 0.0, 1.0);
    int faceIndex = 0;
    vec2 faceUV = PointShadowFaceUV(toFrag, faceIndex);
    vec2 tile = vec2(
        mod(float(faceIndex), u_pointShadowAtlas.x),
        floor(float(faceIndex) / u_pointShadowAtlas.x) + float(shadowIdx * 2)
    );
    vec2 atlasUV = (faceUV + tile) / vec2(u_pointShadowAtlas.x, u_pointShadowAtlas.y);
    float storedDepth = texture2D(s_pointShadowMap, atlasUV).r;
    return ((currentDepth - shadowMeta.w) <= storedDepth) ? 1.0 : 0.0;
}

void main()
{
    // Affine warp by pushing UVs toward their triangle average; we don't have barycentrics here,
    // so approximate by quantizing UVs which induces affine-looking swim when animated.
    float aff = clamp(u_psxParams.y, 0.0, 1.0);
    vec2 uv = v_texcoord0.xy;
    float stepUV = mix(0.0, 1.0/64.0, aff);
    if (stepUV > 0.0) {
        uv = floor(uv / stepUV + 0.5) * stepUV;
    }

    vec4 tex = texture2D(s_albedo, uv);
    float mask = texture2D(s_tintMask, uv).r;
    
    // Apply tint with blend mode (Blender-style continuous factor)
    // Use continuous blend factor (0.825) instead of binary threshold - matches Blender overlay node
    float maskStrength = clamp(mask * 0.825, 0.0, 1.0);
    float blendMode = u_TintParams.x;
    vec3 base = applyTintBlend(tex.rgb, u_ColorTint.rgb, maskStrength, blendMode);
    float alpha = tex.a;

    // Optional posterization of albedo (pre-lighting). 0 disables.
    float levels = max(u_posterize.x, 0.0);
    if (levels >= 1.0) {
        float stepC = 1.0 / max(levels, 1.0);
        base = floor(base / stepC + 0.5) * stepC;
    }

    // Unlit by default
    vec3 finalRgb = base.rgb;

    // Soft lighting influence (0 = unlit, 1 = softly lit)
    float lightInfluence = clamp(u_psxParams.z, 0.0, 1.0);
    if (lightInfluence > 0.0001) {
        // Normal-only world-space perturbation for PSX shimmer (stable per world tile)
        vec3 N = normalize(v_normal);
        float nAmp = clamp(u_psxParams.w, 0.0, 0.25);
        if (nAmp > 0.0) {
            float cell = max(0.05, nAmp * 8.0);
            vec3 key = floor(v_worldPos / cell);
            // simple hash to [-1,1]^3
            float h1 = fract(sin(dot(key, vec3(12.9898, 78.233, 45.164))) * 43758.5453);
            float h2 = fract(sin(dot(key + 13.37, vec3(12.9898, 78.233, 45.164))) * 43758.5453);
            float h3 = fract(sin(dot(key + 37.13, vec3(12.9898, 78.233, 45.164))) * 43758.5453);
            vec3 nJit = normalize(vec3(h1, h2, h3) * 2.0 - 1.0) * nAmp;
            // Fade by pixel footprint to avoid noisy distance
            float pix = length(fwidth(v_worldPos));
            float fade = clamp(0.75 / max(pix, 1e-4), 0.0, 1.0);
            nJit *= fade;
            N = normalize(N + nJit);
        }
        vec3 V = normalize(v_viewDir);
        vec3 lit = base.rgb * u_ambientFog.xyz;

        // Shadow factor (hardware comparison + PCF)
        float shadowFactor = 1.0;
        if (u_shadowReceive.x > 0.5 && u_shadowParams.w > 0.0001 && u_shadowTexelSize.x > 0.0) {
            int cascadeIndex = 0;
            vec4 viewPos = mul(u_view, vec4(v_worldPos, 1.0));
            float camDepth = max(-viewPos.z, 0.0);
            int cascadeCount = int(u_CascadeSplits.w + 0.5);
            if (cascadeCount > 1) {
                if (camDepth > u_CascadeSplits.x) cascadeIndex = 1;
                if (cascadeCount > 2 && camDepth > u_CascadeSplits.y) cascadeIndex = 2;
                if (cascadeCount > 3 && camDepth > u_CascadeSplits.z) cascadeIndex = 3;
                cascadeIndex = min(cascadeIndex, cascadeCount - 1);
            }
            mat4 LVP = (cascadeCount > 1) ? u_lightViewProjCSM[cascadeIndex] : u_lightViewProj;
            vec4 lp = mul(LVP, vec4(v_worldPos, 1.0));
            float invW = 1.0 / max(lp.w, 1.0e-6);
            vec3 ndc = lp.xyz * invW;
#if BGFX_SHADER_LANGUAGE_GLSL
            float depth = ndc.z * 0.5 + 0.5;
#else
            float depth = ndc.z;
#endif
            vec2 uv = ndc.xy * 0.5 + 0.5;
            bool insideShadowProjection =
                uv.x >= 0.0 && uv.x <= 1.0 &&
                uv.y >= 0.0 && uv.y <= 1.0 &&
                depth >= 0.0 && depth <= 1.0;
            if (insideShadowProjection) {
                if (u_shadowTexelSize.w < 0.5) {
                    uv.y = 1.0 - uv.y;
                }
                vec2 shadowUvMin = u_shadowTexelSize.xy;
                vec2 shadowUvMax = vec2(1.0, 1.0) - u_shadowTexelSize.xy;
                if (cascadeCount > 1) {
                    vec2 scale = u_CascadeScaleBias[cascadeIndex].xy;
                    vec2 bias  = u_CascadeScaleBias[cascadeIndex].zw;
                    uv = uv * scale + bias;
                    shadowUvMin = bias + u_shadowTexelSize.xy;
                    shadowUvMax = bias + scale - u_shadowTexelSize.xy;
                    uv = clamp(uv, shadowUvMin, shadowUvMax);
                }
                vec3 toLight = normalize(-u_shadowLightDir.xyz);
                float normalBias = u_shadowParams.y * (1.0 - max(dot(N, toLight), 0.0)) * max(u_shadowTexelSize.x, u_shadowTexelSize.y);
                float bias = u_shadowParams.x + normalBias;
                int taps = 1;
                if (u_shadowParams.z > 0.01) {
                    taps = (u_shadowTexelSize.z > 12.5) ? 16 : (u_shadowTexelSize.z > 8.5 ? 9 : (u_shadowTexelSize.z > 2.5 ? 4 : 1));
                }
                float sum = 0.0;
                float r = u_shadowParams.z;
                if (taps == 1) {
                    sum = shadow2D(s_shadowMap, vec3(clamp(uv, shadowUvMin, shadowUvMax), depth - bias));
                } else if (taps == 4) {
                    vec2 o = u_shadowTexelSize.xy * r;
                    sum += shadow2D(s_shadowMap, vec3(clamp(uv + vec2(-o.x, -o.y), shadowUvMin, shadowUvMax), depth - bias));
                    sum += shadow2D(s_shadowMap, vec3(clamp(uv + vec2( o.x, -o.y), shadowUvMin, shadowUvMax), depth - bias));
                    sum += shadow2D(s_shadowMap, vec3(clamp(uv + vec2(-o.x,  o.y), shadowUvMin, shadowUvMax), depth - bias));
                    sum += shadow2D(s_shadowMap, vec3(clamp(uv + vec2( o.x,  o.y), shadowUvMin, shadowUvMax), depth - bias));
                    sum *= 0.25;
                } else if (taps == 9) {
                    vec2 o = u_shadowTexelSize.xy * r;
                    for (int y = -1; y <= 1; ++y) {
                        for (int x = -1; x <= 1; ++x) {
                            vec2 off = vec2(float(x), float(y)) * o;
                            sum += shadow2D(s_shadowMap, vec3(clamp(uv + off, shadowUvMin, shadowUvMax), depth - bias));
                        }
                    }
                    sum /= 9.0;
                } else { // 16 taps
                    vec2 o = u_shadowTexelSize.xy * r;
                    for (int y = -2; y <= 1; ++y) {
                        for (int x = -2; x <= 1; ++x) {
                            vec2 off = vec2(float(x) + 0.5, float(y) + 0.5) * o;
                            sum += shadow2D(s_shadowMap, vec3(clamp(uv + off, shadowUvMin, shadowUvMax), depth - bias));
                        }
                    }
                    sum /= 16.0;
                }
                shadowFactor = sum;
            }
        }

        // Simple Lambert + distance attenuation with optional toon banding
        for (int i = 0; i < 5; ++i) {
            float type = u_lightPositions[i].w;
            vec3 L;
            float attenuation = 1.0;
            if (type < 0.5) {
                L = normalize(-u_lightPositions[i].xyz);
            } else {
                vec3 pos = u_lightPositions[i].xyz;
                vec3 toL = pos - v_worldPos;
                float dist = length(toL);
                if (u_lightParams[i].x > 0.0 && dist > u_lightParams[i].x) {
                    continue;
                }
                L = (dist > 1e-5) ? (toL / dist) : vec3(0.0,0.0,1.0);
                float constant = u_lightParams[i].y;
                float linearT  = u_lightParams[i].z;
                float quad     = u_lightParams[i].w;
                attenuation = 1.0 / max(constant + linearT * dist + quad * dist * dist, 1e-4);
            }

            float NdotL = max(dot(N, L), 0.0);
            // Smooth baseline shading
            float diffuseSmooth = sqrt(NdotL);
            // Toon banding
            float levels = max(u_toonParams.x, 1.0);
            float diffuseBanded = floor(diffuseSmooth * levels) / levels;
            float bandSoftness = clamp(u_toonParams.y, 0.0, 1.0);
            float diffuse = mix(diffuseBanded, diffuseSmooth, bandSoftness);
            vec3 color = u_lightColors[i].rgb * u_lightColors[i].a;
            float shadowWeight = u_shadowParams.w * clamp(u_shadowReceive.x, 0.0, 1.0);
            float sf = 1.0;
            if (type < 0.5) {
                sf = mix(1.0, shadowFactor, shadowWeight);
            } else {
                for (int shadowIdx = 0; shadowIdx < 4; ++shadowIdx) {
                    if (u_pointShadowMeta[shadowIdx].x > 0.5 && abs(float(i) - u_pointShadowMeta[shadowIdx].y) < 0.25) {
                        float pointShadowFactor = SamplePointShadow(v_worldPos, shadowIdx);
                        sf = mix(1.0, pointShadowFactor, shadowWeight);
                        break;
                    }
                }
            }
            lit += base.rgb * color * diffuse * attenuation * sf;
        }

        finalRgb = mix(base.rgb, lit, lightInfluence);
    }

    // Exponential fog (matches PBR fog behavior)
    if (u_ambientFog.w > 0.5) {
        float distance = length(v_worldPos - u_cameraPos.xyz);
        float fogFactor = 1.0 - clamp(exp(-u_fogParams.x * distance), 0.0, 1.0);
        vec3 fogColor = u_fogParams.yzw;
        finalRgb = mix(finalRgb, fogColor, fogFactor);
    }

    gl_FragColor = vec4(finalRgb, alpha);
}


