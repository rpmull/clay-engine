$input v_position, v_texcoord0, v_heightUV, v_worldPos, v_normal

#include <bgfx_shader.sh>
#include "lib_pbr_common.sh"

SAMPLERCUBE(s_skybox, 9);
// Splatmap textures: RGBA channels store weights for layers 0-3 and 4-7
SAMPLER2D(s_splatTexture, 1);
SAMPLER2D(s_splatTexture2, 2);  // Second splatmap for layers 4-7

// Layer texture arrays (all layers combined)
SAMPLER2DARRAY(s_layerAlbedoArray, 3);
SAMPLER2DARRAY(s_layerNormalArray, 4);
SAMPLER2D(s_holeTexture, 5);

// Layer properties: x=tiling, y=hasAlbedo (1.0 if texture exists, 0.0 otherwise), z=hasNormal, w=unused
uniform vec4 u_layerTiling[8];
// Layer placeholder colors (used as tint if texture exists, base color if not)
uniform vec4 u_layerColor[8];
// Layer count: x = number of active layers (1-8)
uniform vec4 u_layerCount;

// Lighting uniforms - matching fs_pbr.sc
uniform vec4 u_lightColors[5];     // rgb = color, a = intensity
uniform vec4 u_lightPositions[5];  // xyz = position, w = light type (0=directional, 1=point, 2=spot)
uniform vec4 u_lightDirections[5]; // xyz = forward/light direction
uniform vec4 u_lightParams[5];     // x = range, y = constant, z = linear, w = quadratic
uniform vec4 u_lightSpotParams[5]; // x = inner cone cos, y = outer cone cos
uniform vec4 u_cameraPos;          // camera position in world space
uniform vec4 u_ambientFog;         // xyz = ambient color * intensity, w = flags (bit0: fog enabled)
uniform vec4 u_fogParams;          // x = fogDensity, yzw = fog color
uniform vec4 u_skyParams;          // x = proceduralSky flag, y = skybox available, z = max skybox mip
uniform vec4 u_skyTopColor;        // rgb = zenith sky color
uniform vec4 u_skyHorizonColor;    // rgb = horizon color
uniform vec4 u_groundColor;        // rgb = ground/underside color
uniform vec4 u_sunDirection;       // xyz = primary sun direction
uniform vec4 u_skySunParams;       // x = sun size, y = convergence, z = intensity
uniform vec4 u_skyAtmosphereParams;// x = thickness, y = horizon fade, z = sky exposure
uniform vec4 u_sceneColorGrade;    // x=exposure, y=contrast, z=saturation, w=tonemapEnabled

// Terrain material properties (can be extended later)
uniform vec4 u_terrainMaterial;    // x=metallic, y=roughness, z=ao, w=normalStrength

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

// Sample layer albedo from texture array
vec3 SampleLayerAlbedo(int layerIdx, vec2 uv) {
    return texture2DArray(s_layerAlbedoArray, vec3(uv, float(layerIdx))).rgb;
}

// Sample layer normal from texture array
vec3 SampleLayerNormal(int layerIdx, vec2 uv) {
    return texture2DArray(s_layerNormalArray, vec3(uv, float(layerIdx))).xyz * 2.0 - 1.0;
}

// Get splat weight for a specific layer (0-7)
float GetSplatWeight(vec4 splat0, vec4 splat1, int layerIdx) {
    if (layerIdx == 0) return splat0.r;
    if (layerIdx == 1) return splat0.g;
    if (layerIdx == 2) return splat0.b;
    if (layerIdx == 3) return splat0.a;
    if (layerIdx == 4) return splat1.r;
    if (layerIdx == 5) return splat1.g;
    if (layerIdx == 6) return splat1.b;
    return splat1.a; // layerIdx == 7
}

vec3 BuildTerrainTangent(vec3 N) {
    vec3 terrainU = normalize(mul((mat3)u_model[0], vec3(1.0, 0.0, 0.0)));
    vec3 T = terrainU - N * dot(N, terrainU);
    if (length(T) < 0.001) {
        vec3 terrainV = normalize(mul((mat3)u_model[0], vec3(0.0, 0.0, 1.0)));
        T = cross(terrainV, N);
    }
    return normalize(T);
}

vec3 BuildTerrainBitangent(vec3 N, vec3 T) {
    vec3 terrainV = normalize(mul((mat3)u_model[0], vec3(0.0, 0.0, 1.0)));
    vec3 B = normalize(cross(N, T));
    if (dot(B, terrainV) < 0.0) {
        B = -B;
    }
    return B;
}

// Apply normal map from terrain layers (blended via splatmap).
// Terrain textures are laid out in the terrain's local X/Z space, so we
// build the tangent basis from those axes directly instead of using screen
// derivatives, which can flip the terrain response relative to the sun.
vec3 ApplyTerrainNormalMap(vec3 N, vec2 uv, vec4 splat0, vec4 splat1) {
    vec3 finalNormal = N;
    float normalStrength = u_terrainMaterial.w;
    int numLayers = int(u_layerCount.x + 0.5);
    
    // Sample normal maps from each layer and blend based on splat weights
    vec3 blendedNormal = vec3(0.0, 0.0, 1.0); // Start with up vector in tangent space
    
    for (int layerIdx = 0; layerIdx < 8; ++layerIdx) {
        if (layerIdx >= numLayers) break;
        
        float weight = GetSplatWeight(splat0, splat1, layerIdx);
        if (u_layerTiling[layerIdx].z > 0.5 && weight > 0.001) {
            vec2 tiledUV = uv * u_layerTiling[layerIdx].x;
            vec3 normalSample = SampleLayerNormal(layerIdx, tiledUV);
            normalSample.xy *= normalStrength;
            blendedNormal += normalSample * weight;
        }
    }
    
    // If any normal maps were applied, blend with world-space normal using the
    // terrain's own X/Z axes so lighting stays consistent with the light pitch.
    if (length(blendedNormal - vec3(0.0, 0.0, 1.0)) > 0.001) {
        vec3 T = BuildTerrainTangent(N);
        vec3 B = BuildTerrainBitangent(N, T);
        mat3 TBN = mat3(T, B, N);
        finalNormal = normalize(mul(TBN, normalize(blendedNormal)));
    }
    
    return finalNormal;
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

float ComputeSpotAttenuation(int lightIndex, vec3 L) {
    if (u_lightPositions[lightIndex].w < 1.5) {
        return 1.0;
    }
    float innerCos = u_lightSpotParams[lightIndex].x;
    float outerCos = u_lightSpotParams[lightIndex].y;
    float theta = dot(normalize(u_lightDirections[lightIndex].xyz), -L);
    float denom = max(innerCos - outerCos, 1e-4);
    return clamp((theta - outerCos) / denom, 0.0, 1.0);
}

vec3 ComputePointLightReflectionFallback(vec3 worldPos, vec3 N, float roughness) {
    vec3 fallbackRadiance = vec3(0.0, 0.0, 0.0);
    float roughWeight = mix(0.15, 1.0, 1.0 - roughness);
    for (int lightIdx = 0; lightIdx < 5; ++lightIdx) {
        if (u_lightPositions[lightIdx].w < 0.5) {
            continue;
        }
        vec3 toLight = u_lightPositions[lightIdx].xyz - worldPos;
        float dist = length(toLight);
        float range = u_lightParams[lightIdx].x;
        if (range > 0.0 && dist > range) {
            continue;
        }
        vec3 L = normalize(toLight);
        float constant = u_lightParams[lightIdx].y;
        float linearTerm = u_lightParams[lightIdx].z;
        float quadratic = u_lightParams[lightIdx].w;
        float attenuation = 1.0 / (constant + linearTerm * dist + quadratic * dist * dist + 0.0001);
        attenuation *= ComputeSpotAttenuation(lightIdx, L);
        float grazing = 1.0 - max(dot(N, L), 0.0);
        float scatter = mix(0.25, 1.0, grazing);
        fallbackRadiance += u_lightColors[lightIdx].rgb * u_lightColors[lightIdx].a * attenuation * scatter * roughWeight;
    }
    return fallbackRadiance * 0.12;
}

vec3 SampleSkyboxPrefiltered(vec3 envDir, float roughness) {
    float maxMipLevel = max(u_skyParams.z, 0.0);
    float lod = clamp(roughness * roughness * maxMipLevel, 0.0, maxMipLevel);
    return textureCubeLod(s_skybox, normalize(envDir), lod).rgb;
}

void main()
{
    if (texture2D(s_holeTexture, v_heightUV).r > 0.5) {
        discard;
    }

    int numLayers = int(u_layerCount.x + 0.5);
    
    // Sample splatmaps to get blend weights for each layer
    vec4 splat0 = texture2D(s_splatTexture, v_heightUV);
    vec4 splat1 = vec4(0.0, 0.0, 0.0, 0.0);
    if (numLayers > 4) {
        splat1 = texture2D(s_splatTexture2, v_heightUV);
    }
    
    // Normalize weights across all active layers
    float weightSum = 0.0;
    for (int layerIdx = 0; layerIdx < 8; ++layerIdx) {
        if (layerIdx >= numLayers) break;
        weightSum += GetSplatWeight(splat0, splat1, layerIdx);
    }
    
    if (weightSum > 0.001) {
        splat0 /= weightSum;
        splat1 /= weightSum;
    } else {
        // Fallback: use first layer if splatmap is empty
        splat0 = vec4(1.0, 0.0, 0.0, 0.0);
        splat1 = vec4(0.0, 0.0, 0.0, 0.0);
    }
    
    // Accumulate blended color from all layers
    vec3 baseColor = vec3(0.0, 0.0, 0.0);
    vec2 layerUV = v_texcoord0.xy;
    
    for (int layerIdx = 0; layerIdx < 8; ++layerIdx) {
        if (layerIdx >= numLayers) break;
        
        float weight = GetSplatWeight(splat0, splat1, layerIdx);
        if (weight < 0.001) continue;
        
        float tiling = u_layerTiling[layerIdx].x;
        float hasAlbedo = u_layerTiling[layerIdx].y;
        vec3 layerColor = u_layerColor[layerIdx].rgb;
        
        vec3 color = layerColor;
        if (hasAlbedo > 0.5) {
            vec3 texColor = SampleLayerAlbedo(layerIdx, layerUV * tiling);
            color = texColor * layerColor;
        }
        baseColor += color * weight;
    }
    
    // Normal computation
    vec3 N = normalize(v_normal);
    // Apply normal mapping from terrain layers if available
    N = ApplyTerrainNormalMap(N, layerUV, splat0, splat1);
    
    vec3 V = normalize(u_cameraPos.xyz - v_worldPos);
    
    // Material properties (defaults if uniform not set)
    float metallic = u_terrainMaterial.x;
    float roughness = clamp(u_terrainMaterial.y, 0.04, 1.0);
    roughness = ApplySpecularAA(roughness, N);
    float ao = u_terrainMaterial.z;
    
    vec3 ambientIrradiance = u_ambientFog.rgb;
    vec3 diffuseColor = baseColor * (1.0 - metallic);
    vec3 ambientColor = diffuseColor * ambientIrradiance * ao;
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
                vec2 uvBias = u_CascadeScaleBias[cascadeIndex].zw;
                uv = uv * scale + uvBias;
                shadowUvMin = uvBias + u_shadowTexelSize.xy;
                shadowUvMax = uvBias + scale - u_shadowTexelSize.xy;
                uv = clamp(uv, shadowUvMin, shadowUvMax);
            }
            vec3 toLight = normalize(-u_shadowLightDir.xyz);
            float normalBias = u_shadowParams.y * (1.0 - max(dot(N, toLight), 0.0)) * max(u_shadowTexelSize.x, u_shadowTexelSize.y);
            float shadowBias = u_shadowParams.x + normalBias;
            int taps = 1;
            if (u_shadowParams.z > 0.01) {
                taps = (u_shadowTexelSize.z > 12.5) ? 16 : (u_shadowTexelSize.z > 8.5 ? 9 : (u_shadowTexelSize.z > 2.5 ? 4 : 1));
            }
            float sum = 0.0;
            float r = u_shadowParams.z;
            if (taps == 1) {
                sum = shadow2D(s_shadowMap, vec3(clamp(uv, shadowUvMin, shadowUvMax), depth - shadowBias));
            } else if (taps == 4) {
                vec2 o = u_shadowTexelSize.xy * r;
                sum += shadow2D(s_shadowMap, vec3(clamp(uv + vec2(-o.x, -o.y), shadowUvMin, shadowUvMax), depth - shadowBias));
                sum += shadow2D(s_shadowMap, vec3(clamp(uv + vec2( o.x, -o.y), shadowUvMin, shadowUvMax), depth - shadowBias));
                sum += shadow2D(s_shadowMap, vec3(clamp(uv + vec2(-o.x,  o.y), shadowUvMin, shadowUvMax), depth - shadowBias));
                sum += shadow2D(s_shadowMap, vec3(clamp(uv + vec2( o.x,  o.y), shadowUvMin, shadowUvMax), depth - shadowBias));
                sum *= 0.25;
            } else if (taps == 9) {
                vec2 o = u_shadowTexelSize.xy * r;
                for (int sy = -1; sy <= 1; ++sy) {
                    for (int sx = -1; sx <= 1; ++sx) {
                        vec2 off = vec2(float(sx), float(sy)) * o;
                        sum += shadow2D(s_shadowMap, vec3(clamp(uv + off, shadowUvMin, shadowUvMax), depth - shadowBias));
                    }
                }
                sum /= 9.0;
            } else { // 16 taps
                vec2 o = u_shadowTexelSize.xy * r;
                for (int sy = -2; sy <= 1; ++sy) {
                    for (int sx = -2; sx <= 1; ++sx) {
                        vec2 off = vec2(float(sx) + 0.5, float(sy) + 0.5) * o;
                        sum += shadow2D(s_shadowMap, vec3(clamp(uv + off, shadowUvMin, shadowUvMax), depth - shadowBias));
                    }
                }
                sum /= 16.0;
            }
            shadowFactor = sum;
        }
    }

    // Process each light (directional and point)
    for (int lightIdx = 0; lightIdx < 5; lightIdx++) {
        float lightType = u_lightPositions[lightIdx].w;
        vec3 lightColor = u_lightColors[lightIdx].rgb;
        float lightIntensity = u_lightColors[lightIdx].a;
        
        vec3 L;
        float attenuation = 1.0;
        
        if (lightType < 0.5) {
            // Directional light
            L = normalize(-u_lightDirections[lightIdx].xyz);
        } else {
            vec3 lightPos = u_lightPositions[lightIdx].xyz;
            vec3 lightDir = lightPos - v_worldPos;
            float dist = length(lightDir);
            L = normalize(lightDir);
            
            // Check if light is within range
            float range = u_lightParams[lightIdx].x;
            if (range > 0.0 && dist > range) {
                continue; // Skip this light if out of range
            }
            
            // Calculate attenuation
            float constant = u_lightParams[lightIdx].y;
            float linearTerm = u_lightParams[lightIdx].z;
            float quadratic = u_lightParams[lightIdx].w;
            attenuation = 1.0 / (constant + linearTerm * dist + quadratic * dist * dist);
            attenuation *= ComputeSpotAttenuation(lightIdx, L);
            if (attenuation <= 0.0) {
                continue;
            }
        }
        
        float shadowWeight = u_shadowParams.w * clamp(u_shadowReceive.x, 0.0, 1.0);
        float sf = 1.0;
        if (lightType < 0.5) {
            sf = mix(1.0, shadowFactor, shadowWeight);
        } else if (lightType < 1.5) {
            for (int shadowIdx = 0; shadowIdx < 4; ++shadowIdx) {
                if (u_pointShadowMeta[shadowIdx].x > 0.5 && abs(float(lightIdx) - u_pointShadowMeta[shadowIdx].y) < 0.25) {
                    float pointShadowFactor = SamplePointShadow(v_worldPos, shadowIdx);
                    sf = mix(1.0, pointShadowFactor, shadowWeight);
                    break;
                }
            }
        }
        finalColor += CalculatePBRLighting(N, V, L, baseColor, metallic, roughness, lightColor, lightIntensity) * attenuation * sf * ao;
    }

    // Exponential fog
    if (u_ambientFog.w > 0.5) {
        float dist = length(v_worldPos - u_cameraPos.xyz);
        float fogFactor = 1.0 - clamp(exp(-u_fogParams.x * dist), 0.0, 1.0);
        vec3 fogColor = u_fogParams.yzw;
        finalColor = mix(finalColor, fogColor, fogFactor);
    }

    finalColor = ApplySceneColorGrade(finalColor, u_sceneColorGrade);

    gl_FragColor = vec4(finalColor, 1.0);
}
