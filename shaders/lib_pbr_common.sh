#ifndef CLAYMORE_LIB_PBR_COMMON_SH
#define CLAYMORE_LIB_PBR_COMMON_SH

// Shared Cook-Torrance BRDF used by mesh PBR and terrain shading.
// Optimized for GPU: fast Fresnel approximation, pre-computed constants

#define PI 3.14159265359
#define INV_PI 0.31830988618

// Fast Fresnel-Schlick approximation (Unity/Unreal style)
// Uses exp2 approximation instead of pow(x, 5.0) - saves ~3 cycles
vec3 FresnelSchlickFast(float cosTheta, vec3 F0) {
    float fresnel = exp2((-5.55473 * cosTheta - 6.98316) * cosTheta);
    return F0 + (vec3(1.0, 1.0, 1.0) - F0) * fresnel;
}

// GGX/Trowbridge-Reitz Normal Distribution Function
float DistributionGGX(float NdotH, float alpha2) {
    float denom = (NdotH * NdotH) * (alpha2 - 1.0) + 1.0;
    return alpha2 / (PI * denom * denom);
}

// Smith's Schlick-GGX geometry function (combined for V and L)
float GeometrySmith(float NdotV, float NdotL, float k) {
    float ggx1 = NdotV / (NdotV * (1.0 - k) + k);
    float ggx2 = NdotL / (NdotL * (1.0 - k) + k);
    return ggx1 * ggx2;
}

vec3 CalculatePBRLighting(vec3 N, vec3 V, vec3 L, vec3 baseColor, float metallic, float roughness, vec3 lightColor, float lightIntensity)
{
    vec3 H = normalize(V + L);

    // Pre-compute dot products (clamped to avoid negative values)
    float NdotH = max(dot(N, H), 0.0);
    float NdotV = max(dot(N, V), 0.001); // Avoid division by zero
    float NdotL = max(dot(N, L), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    // Early out for back-facing fragments
    if (NdotL <= 0.0) return vec3(0.0, 0.0, 0.0);

    // Material F0 (reflectance at normal incidence)
    vec3 F0 = mix(vec3(0.04, 0.04, 0.04), baseColor, metallic);

    // Pre-compute roughness terms once
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float k = (roughness + 1.0) * (roughness + 1.0) * 0.125; // /8.0 as multiply

    // BRDF components
    float D = DistributionGGX(NdotH, alpha2);
    vec3  F = FresnelSchlickFast(VdotH, F0);
    float G = GeometrySmith(NdotV, NdotL, k);

    // Cook-Torrance specular BRDF
    vec3 specular = (D * F * G) / (4.0 * NdotV * NdotL + 0.0001);

    // Energy-conserving diffuse
    vec3 kD = (vec3(1.0, 1.0, 1.0) - F) * (1.0 - metallic);
    vec3 diffuse = baseColor * INV_PI;

    return (kD * diffuse + specular) * NdotL * lightColor * lightIntensity;
}

vec3 EvaluateProceduralSkyRadiance(
    vec3 dir,
    vec3 skyTopColor,
    vec3 skyHorizonColor,
    vec3 groundColor,
    vec3 sunDirection,
    vec4 skySunParams,
    vec4 skyAtmosphereParams)
{
    vec3 nDir = normalize(dir);
    float up = clamp(nDir.y * 0.5 + 0.5, 0.0, 1.0);
    float horizonBlend = pow(clamp(1.0 - abs(nDir.y), 0.0, 1.0), clamp(skyAtmosphereParams.y, 0.05, 8.0));

    vec3 skyGradient = mix(groundColor, skyTopColor, up);
    vec3 skyColor = mix(skyGradient, skyHorizonColor, horizonBlend);

    vec3 sunDir = normalize(sunDirection);
    float sunDot = max(dot(nDir, sunDir), 0.0);
    float sunSharpness = mix(256.0, 8192.0, clamp(1.0 - skySunParams.x, 0.0, 1.0));
    float sunDisk = pow(sunDot, sunSharpness);
    vec3 sunColor = vec3(skySunParams.z, skySunParams.z, skySunParams.z) * sunDisk;

    // Keep procedural-sky reflections visible regardless of ambient diffuse tuning.
    float skyExposure = max(skyAtmosphereParams.z, 0.0);
    return (skyColor + sunColor) * skyExposure;
}

vec3 ApproximateIBLSpecular(
    vec3 N,
    vec3 V,
    vec3 baseColor,
    float metallic,
    float roughness,
    vec3 reflectedRadiance)
{
    float NdotV = max(dot(N, V), 0.001);
    vec3 F0 = mix(vec3(0.04, 0.04, 0.04), baseColor, metallic);

    // UE4/Lazarov split-sum approximation (no BRDF LUT texture required).
    vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    vec2 envBRDF = vec2(-1.04, 1.04) * a004 + r.zw;
    vec3 specularIBL = F0 * envBRDF.x + envBRDF.y;

    return reflectedRadiance * specularIBL;
}

float ApplySpecularAA(float roughness, vec3 N)
{
    vec3 dndx = dFdx(N);
    vec3 dndy = dFdy(N);
    float variance = clamp(max(dot(dndx, dndx), dot(dndy, dndy)), 0.0, 0.25);
    float aa = sqrt(clamp(roughness * roughness + variance * 0.5, 0.0, 1.0));
    return clamp(aa, 0.04, 1.0);
}

vec3 ACESFilm(vec3 x)
{
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 ApplySceneColorGrade(vec3 color, vec4 sceneColorGrade)
{
    float exposure = max(sceneColorGrade.x, 0.0);
    float contrast = max(sceneColorGrade.y, 0.0);
    float saturation = max(sceneColorGrade.z, 0.0);
    float tonemapMode = sceneColorGrade.w;

    color *= exposure;
    if (tonemapMode > 0.5) {
        color = ACESFilm(color);
    }

    color = (color - 0.5) * contrast + 0.5;
    float luminance = dot(color, vec3(0.299, 0.587, 0.114));
    color = mix(vec3(luminance, luminance, luminance), color, saturation);
    return clamp(color, 0.0, 1.0);
}

#endif // CLAYMORE_LIB_PBR_COMMON_SH





