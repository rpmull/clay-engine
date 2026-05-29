$input v_worldPos, v_normal, v_texcoord0, v_viewDir

#include <bgfx_shader.sh>
#include "lib_pbr_common.sh"
#include "lib_tint_blend.sh"

SAMPLER2D(s_albedo, 0);
SAMPLERCUBE(s_skybox, 9);
// Optional tint mask; if provided, mask value controls tint blend strength
SAMPLER2D(s_tintMask, 3);
SAMPLER2D(s_metallicRoughness, 1);
SAMPLER2D(s_normalMap, 2);
SAMPLER2D(s_ao, 4);
SAMPLER2D(s_emission, 5);

// Light uniforms - support up to 5 lights
uniform vec4 u_lightColors[5];     // rgb = color, a = intensity
uniform vec4 u_lightPositions[5];  // xyz = position/direction, w = light type (0=directional, 1=point)
uniform vec4 u_lightParams[5];     // x = range (for point lights), y = constant, z = linear, w = quadratic
uniform vec4 u_cameraPos;          // camera position in world space
uniform vec4 u_ambientFog;         // xyz = ambient color * intensity, w = flags (bit0: fog enabled)
uniform vec4 u_fogParams;          // x = fogDensity, yzw = fog color
uniform vec4 u_skyParams;          // x = proceduralSky flag, y = skybox available
uniform vec4 u_skyTopColor;        // rgb = zenith sky color
uniform vec4 u_skyHorizonColor;    // rgb = horizon color
uniform vec4 u_groundColor;        // rgb = ground/underside color
uniform vec4 u_sunDirection;       // xyz = primary sun direction
uniform vec4 u_skySunParams;       // x = sun size, y = convergence, z = intensity
uniform vec4 u_skyAtmosphereParams;// x = thickness, y = horizon fade, z = sky exposure
uniform vec4 u_sceneColorGrade;    // x=exposure, y=contrast, z=saturation, w=tonemapEnabled
uniform vec4 u_ColorTint;          // rgb = tint color, a = unused
uniform vec4 u_TintParams;         // x = blend mode (0-9), y = unused, z = unused, w = unused
uniform vec4 u_PBRScalar0; // x=metallicScalar, y=roughnessScalar, z=aoScalar, w=normalStrength
uniform vec4 u_PBRScalar1; // x=emissionStrength, y=alphaCutoffThreshold (0=disabled)
uniform vec4 u_EmissionColor;
uniform vec4 u_TextureUsage; // x=mrTex, y=normalTex, z=aoTex, w=emissionTex

// Shadowing
SAMPLER2DSHADOW(s_shadowMap, 7);
uniform mat4 u_lightViewProj;
uniform mat4 u_lightViewProjCSM[4];
uniform vec4 u_CascadeSplits;    // xyz = splits, w = count
uniform vec4 u_CascadeScaleBias[4]; // xy scale, zw bias into atlas
uniform vec4 u_shadowParams;     // x=bias, y=normalBias, z=softness, w=strength
uniform vec4 u_shadowTexelSize;  // x=1/w, y=1/h, z=samples, w=originBottomLeft
uniform vec4 u_shadowLightDir;   // xyz = light dir (world), w unused
uniform vec4 u_shadowReceive;    // x = receive shadows (0/1)
SAMPLER2D(s_pointShadowMap, 8);
uniform vec4 u_pointShadowMeta[4];     // x=enabled, y=light slot, z=range, w=bias
uniform vec4 u_pointShadowLightPos[4]; // xyz=light position
uniform vec4 u_pointShadowAtlas;    // x=tileCols, y=tileRows, z=1/atlasW, w=1/atlasH

vec3 derivativeX(vec3 value) {
    return vec3(dFdx(value.x), dFdx(value.y), dFdx(value.z));
}

vec3 derivativeY(vec3 value) {
    return vec3(dFdy(value.x), dFdy(value.y), dFdy(value.z));
}

vec3 ApplyNormalMap(vec3 N, vec2 uv, vec3 worldPos) {
    vec3 dp1 = derivativeX(worldPos);
    vec3 dp2 = derivativeY(worldPos);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);
    vec3 tangent = normalize(dp1 * duv2.y - dp2 * duv1.y);
    vec3 bitangent = normalize(dp2 * duv1.x - dp1 * duv2.x);
    mat3 TBN = mat3(tangent, bitangent, N);
    vec3 sampled = texture2D(s_normalMap, uv).xyz * 2.0 - 1.0;
    sampled.xy *= u_PBRScalar0.w;
    return normalize(mul(TBN, sampled));
}

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

vec3 ComputePointLightReflectionFallback(vec3 worldPos, vec3 N, float roughness) {
    vec3 fallbackRadiance = vec3(0.0, 0.0, 0.0);
    float roughWeight = mix(0.15, 1.0, 1.0 - roughness);
    for (int i = 0; i < 5; ++i) {
        if (u_lightPositions[i].w < 0.5) {
            continue;
        }
        vec3 toLight = u_lightPositions[i].xyz - worldPos;
        float distance = length(toLight);
        float range = u_lightParams[i].x;
        if (range > 0.0 && distance > range) {
            continue;
        }
        vec3 L = normalize(toLight);
        float constant = u_lightParams[i].y;
        float linearTerm = u_lightParams[i].z;
        float quadratic = u_lightParams[i].w;
        float attenuation = 1.0 / (constant + linearTerm * distance + quadratic * distance * distance + 0.0001);
        float grazing = 1.0 - max(dot(N, L), 0.0);
        float scatter = mix(0.25, 1.0, grazing);
        fallbackRadiance += u_lightColors[i].rgb * u_lightColors[i].a * attenuation * scatter * roughWeight;
    }
    return fallbackRadiance * 0.12;
}

vec3 SampleSkyboxPrefiltered(vec3 envDir, float roughness) {
    vec3 dir = normalize(envDir);
    vec3 up = abs(dir.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, dir));
    vec3 bitangent = normalize(cross(dir, tangent));
    float cone = roughness * roughness * 0.55;

    vec3 c0 = textureCube(s_skybox, dir).rgb;
    vec3 c1 = textureCube(s_skybox, normalize(dir + tangent * cone)).rgb;
    vec3 c2 = textureCube(s_skybox, normalize(dir - tangent * cone)).rgb;
    vec3 c3 = textureCube(s_skybox, normalize(dir + bitangent * cone)).rgb;
    vec3 c4 = textureCube(s_skybox, normalize(dir - bitangent * cone)).rgb;

    return c0 * 0.40 + (c1 + c2 + c3 + c4) * 0.15;
}

void main()
{
    vec2 uv = v_texcoord0.xy;
    vec3 N = normalize(v_normal);
    if (u_TextureUsage.y > 0.5) {
        N = ApplyNormalMap(N, uv, v_worldPos);
    }
    vec3 V = normalize(v_viewDir);
    
    // Sample material properties
    vec4 albedoSample = texture2D(s_albedo, uv);
    
    // Alpha cutout: discard pixels below threshold if enabled
    // u_PBRScalar1.y contains the cutoff threshold (0 = disabled)
    // Use 0.001 minimum to avoid floating-point precision issues triggering false discards
    float alphaCutoff = u_PBRScalar1.y;
    if (alphaCutoff > 0.001 && albedoSample.a < alphaCutoff) {
        discard;
    }
    
    vec3 baseColor = albedoSample.rgb; 
    
    // Apply tint with blend mode (Blender-style continuous factor)
    float mask = texture2D(s_tintMask, uv).r; // default 0 if unbound
    // Use continuous blend factor (0.825) instead of binary threshold - matches Blender overlay node
    float maskStrength = clamp(mask * 0.825, 0.0, 1.0);
    float blendMode = u_TintParams.x;
    baseColor = applyTintBlend(baseColor, u_ColorTint.rgb, maskStrength, blendMode);
    vec2 mrSample = texture2D(s_metallicRoughness, uv).rg;
    float hasMR = step(0.5, u_TextureUsage.x);
    float metallic = mix(u_PBRScalar0.x, mrSample.r * u_PBRScalar0.x, hasMR);
    float roughness = mix(u_PBRScalar0.y, mrSample.g * u_PBRScalar0.y, hasMR);
    metallic = clamp(metallic, 0.0, 1.0);
    roughness = clamp(roughness, 0.04, 1.0);
    roughness = ApplySpecularAA(roughness, N);
    float ao = u_PBRScalar0.z;
    if (u_TextureUsage.z > 0.5) {
        ao *= texture2D(s_ao, uv).r;
    }
    ao = clamp(ao, 0.0, 1.0);

    vec3 ambientIrradiance = u_ambientFog.xyz;

    // Diffuse fraction of BRDF
    vec3 diffuseColor = baseColor * (1.0 - metallic);

    // True albedo-driven ambient instead of additive tint
    vec3 ambientColor = diffuseColor * ambientIrradiance * ao;

    // Analytic specular IBL approximation (no baked probes/LUT required).
    // This restores physically expected metal reflections when direct lights are weak.
    vec3 R = reflect(-V, N);
    vec3 envDir = normalize(mix(R, N, roughness * roughness));
    vec3 reflectedRadiance = ambientIrradiance;
    if (u_skyParams.x > 0.5) {
        reflectedRadiance = EvaluateProceduralSkyRadiance(
            envDir,
            u_skyTopColor.rgb,
            u_skyHorizonColor.rgb,
            u_groundColor.rgb,
            u_sunDirection.xyz,
            u_skySunParams,
            u_skyAtmosphereParams);
    } else if (u_skyParams.y > 0.5) {
        reflectedRadiance = SampleSkyboxPrefiltered(envDir, roughness);
    } else {
        reflectedRadiance += ComputePointLightReflectionFallback(v_worldPos, N, roughness);
    }
    vec3 ambientSpecular = ApproximateIBLSpecular(N, V, baseColor, metallic, roughness, reflectedRadiance) * ao;

    // Begin lighting accumulation with physically-informed ambient
    vec3 finalColor = ambientColor + ambientSpecular;
    
    // Shadow factor (single directional shadow map)
    float shadowFactor = 1.0;
    if (u_shadowReceive.x > 0.5 && u_shadowParams.w > 0.0001 && u_shadowTexelSize.x > 0.0) {
        // Select cascade
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
        float invW = 1.0 / max(lp.w, 1e-6);
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

    // Process each light
    for (int i = 0; i < 5; i++) {
        float lightType = u_lightPositions[i].w;
        vec3 lightColor = u_lightColors[i].rgb;
        float lightIntensity = u_lightColors[i].a;
        
        vec3 L;
        float attenuation = 1.0;
        
        if (lightType < 0.5) {
            // Directional light
            L = normalize(-u_lightPositions[i].xyz);
        } else {
            // Point light
            vec3 lightPos = u_lightPositions[i].xyz;
            vec3 lightDir = lightPos - v_worldPos;
            float distance = length(lightDir);
            L = normalize(lightDir);
            
            // Check if light is within range
            float range = u_lightParams[i].x;
            if (range > 0.0 && distance > range) {
                continue; // Skip this light if out of range
            }
            
            // Calculate attenuation
            float constant = u_lightParams[i].y;
            float linearTerm = u_lightParams[i].z;
            float quadratic = u_lightParams[i].w;
            attenuation = 1.0 / (constant + linearTerm * distance + quadratic * distance * distance);
        }
        
        float shadowWeight = u_shadowParams.w * clamp(u_shadowReceive.x, 0.0, 1.0);
        float sf = 1.0;
        if (lightType < 0.5) {
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
        finalColor += CalculatePBRLighting(N, V, L, baseColor, metallic, roughness, lightColor, lightIntensity) * attenuation * sf * ao;
    }

    vec3 emission = u_EmissionColor.rgb * u_PBRScalar1.x;
    if (u_TextureUsage.w > 0.5) {
        emission *= texture2D(s_emission, uv).rgb;
    }
    finalColor += emission;

    // Exponential fog
    if (u_ambientFog.w > 0.5) {
        float distance = length(v_viewDir) > 0.0 ? length(v_worldPos - u_cameraPos.xyz) : 0.0;
        float fogFactor = 1.0 - clamp(exp(-u_fogParams.x * distance), 0.0, 1.0);
        vec3 fogColor = u_fogParams.yzw;
        finalColor = mix(finalColor, fogColor, fogFactor);
    }

    finalColor = ApplySceneColorGrade(finalColor, u_sceneColorGrade);

    // Preserve texture alpha for blending
    gl_FragColor = vec4(finalColor, albedoSample.a);
}
