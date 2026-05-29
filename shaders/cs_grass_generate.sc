#include "bgfx_compute.sh"

#if BGFX_SHADER_LANGUAGE_HLSL
#define MAT3 float3x3
#else
#define MAT3 mat3
#endif

vec3 ApplyNormal(MAT3 m, vec3 v)
{
#if BGFX_SHADER_LANGUAGE_HLSL
    return mul(v, m);
#else
    return m * v;
#endif
}

// Instance buffer as flat vec4 array (4 vec4s per instance)
// This matches the bgfx indirect draw pattern from example 48-drawindirect
BUFFER_WO(u_InstanceBuffer, vec4, 0);
// Counter as 1x1 R32U texture for atomic operations
IMAGE2D_RW(u_CounterTexture, r32ui, 1);

// bgfx uses a single compute binding table for buffers and images, so these
// read-only images need unique stages that don't collide with the writable ones.
IMAGE2D_RO(u_DensityTexture, r8, 2);
IMAGE2D_RO(u_HeightTexture, r32f, 3);
IMAGE2D_RO(u_SplatTexture, rgba8, 4);
IMAGE2D_RO(u_HoleTexture, r8, 5);

uniform vec4 u_GrassTerrain0;   // x=cellSizeX, y=cellSizeY, z=cellArea, w=maxHeight
uniform vec4 u_GrassTerrain1;   // x=gridResolution, y=invGridMinusOne, z=capacity, w=maxBladesPerTexel
uniform vec4 u_GrassLayer0;     // x=densityPerSqM, y=heightMin, z=heightMax, w=maxSlopeRadians
uniform vec4 u_GrassLayer1;     // x=scaleMin, y=scaleMax, z=randomYawRadians, w=windStrength
uniform vec4 u_GrassMaskParams; // x=mask enum, y=minDistance, z=maxDistance, w=splatAvailable
uniform vec4 u_GrassLayerParams; // x=mode (0=billboard,1=mesh)
uniform vec4 u_GrassCameraPos;
uniform vec4 u_GrassCameraRight;
uniform vec4 u_GrassCameraForward;
uniform vec4 u_GrassWorld[4];
uniform vec4 u_GrassNormal[3];
uniform vec4 u_GrassDispatch;   // x=groupsX, y=groupsY, z=grassGridRes, w=terrainTexRes
uniform vec4 u_GrassSeedTime;   // x=seed, y=time, z=var0, w=var1

uint Hash(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352d;
    value ^= value >> 15;
    value *= 0x846ca68b;
    value ^= value >> 16;
    return value;
}

float RandomFloat01(inout uint seed)
{
    seed = Hash(seed);
    return float(seed) / 4294967295.0;
}

mat4 GetWorldMatrix()
{
    return mat4(u_GrassWorld[0], u_GrassWorld[1], u_GrassWorld[2], u_GrassWorld[3]);
}

MAT3 GetNormalMatrix()
{
#if BGFX_SHADER_LANGUAGE_HLSL
    return MAT3(
        u_GrassNormal[0].xyz,
        u_GrassNormal[1].xyz,
        u_GrassNormal[2].xyz);
#else
    return mat3(
        u_GrassNormal[0].xyz,
        u_GrassNormal[1].xyz,
        u_GrassNormal[2].xyz);
#endif
}

float SampleHeightTexel(ivec2 coord, int maxCoord)
{
    // Component-wise clamp for HLSL compatibility
    coord.x = max(0, min(coord.x, maxCoord));
    coord.y = max(0, min(coord.y, maxCoord));
    return imageLoad(u_HeightTexture, coord).x;
}

// Bilinear interpolation for sub-texel height sampling
float SampleHeightBilinear(vec2 uv, uint resolution)
{
    // Convert UV to texel coordinates
    vec2 texelCoord = uv * float(resolution) - 0.5;
    int maxCoord = int(resolution) - 1;
    
    // Use floor for HLSL/GLSL compatibility
    vec2 flooredCoord = floor(texelCoord);
    ivec2 coord00 = ivec2(int(flooredCoord.x), int(flooredCoord.y));
    ivec2 coord10 = ivec2(coord00.x + 1, coord00.y);
    ivec2 coord01 = ivec2(coord00.x, coord00.y + 1);
    ivec2 coord11 = ivec2(coord00.x + 1, coord00.y + 1);
    
    // Sample heights at corners (clamping happens inside)
    float h00 = SampleHeightTexel(coord00, maxCoord);
    float h10 = SampleHeightTexel(coord10, maxCoord);
    float h01 = SampleHeightTexel(coord01, maxCoord);
    float h11 = SampleHeightTexel(coord11, maxCoord);
    
    // Fractional part for interpolation
    vec2 frac_part = texelCoord - flooredCoord;
    
    // Bilinear interpolation
    float h0 = mix(h00, h10, frac_part.x);
    float h1 = mix(h01, h11, frac_part.x);
    return mix(h0, h1, frac_part.y);
}

vec3 ComputeLocalNormal(ivec2 coord, uint resolution, float heightCenter)
{
    const ivec2 maxCoord = ivec2(int(resolution) - 1, int(resolution) - 1);
    const ivec2 rightCoord = ivec2(min(coord.x + 1, maxCoord.x), coord.y);
    const ivec2 upCoord = ivec2(coord.x, min(coord.y + 1, maxCoord.y));

    const float hRight = imageLoad(u_HeightTexture, rightCoord).x;
    const float hUp = imageLoad(u_HeightTexture, upCoord).x;
    const float heightScale = u_GrassTerrain0.w;

    vec3 tangentX = vec3(u_GrassTerrain0.x, (hRight - heightCenter) * heightScale, 0.0);
    vec3 tangentZ = vec3(0.0, (hUp - heightCenter) * heightScale, u_GrassTerrain0.y);
    vec3 normal = normalize(cross(tangentZ, tangentX));
    if (!all(isfinite(normal)))
    {
        normal = vec3(0.0, 1.0, 0.0);
    }
    return normal;
}

// Sample mask with UV-based bilinear interpolation for grass sampling multiplier support
float SampleMask(vec2 uv, int maskMode, bool splatValid, float texRes)
{
    if (maskMode < 1 || maskMode > 4 || !splatValid)
    {
        return 1.0;
    }

    // Convert UV to texture coordinates
    float maxCoord = texRes - 1.0;
    int maxCoordInt = int(maxCoord);
    vec2 texCoord = uv * maxCoord;
    ivec2 coord0 = ivec2(floor(texCoord));
    ivec2 coord1 = min(coord0 + ivec2(1, 1), ivec2(maxCoordInt, maxCoordInt));
    vec2 frac = texCoord - vec2(coord0);
    
    // Bilinear sample splat
    vec4 s00 = imageLoad(u_SplatTexture, coord0) / 255.0;
    vec4 s10 = imageLoad(u_SplatTexture, ivec2(coord1.x, coord0.y)) / 255.0;
    vec4 s01 = imageLoad(u_SplatTexture, ivec2(coord0.x, coord1.y)) / 255.0;
    vec4 s11 = imageLoad(u_SplatTexture, coord1) / 255.0;
    vec4 splat = mix(mix(s00, s10, frac.x), mix(s01, s11, frac.x), frac.y);
    
    if (maskMode == 1) return splat.r;
    if (maskMode == 2) return splat.g;
    if (maskMode == 3) return splat.b;
    return splat.a;
}

// Sample paint density with UV-based bilinear interpolation
float SamplePaintDensityBilinear(vec2 uv, float texRes)
{
    float maxCoord = texRes - 1.0;
    int maxCoordInt = int(maxCoord);
    vec2 texCoord = uv * maxCoord;
    ivec2 coord0 = ivec2(floor(texCoord));
    ivec2 coord1 = min(coord0 + ivec2(1, 1), ivec2(maxCoordInt, maxCoordInt));
    vec2 frac = texCoord - vec2(coord0);
    
    float d00 = imageLoad(u_DensityTexture, coord0).x;
    float d10 = imageLoad(u_DensityTexture, ivec2(coord1.x, coord0.y)).x;
    float d01 = imageLoad(u_DensityTexture, ivec2(coord0.x, coord1.y)).x;
    float d11 = imageLoad(u_DensityTexture, coord1).x;
    
    return mix(mix(d00, d10, frac.x), mix(d01, d11, frac.x), frac.y);
}

float SampleHoleBilinear(vec2 uv, float texRes)
{
    float maxCoord = texRes - 1.0;
    int maxCoordInt = int(maxCoord);
    vec2 texCoord = uv * maxCoord;
    ivec2 coord0 = ivec2(floor(texCoord));
    ivec2 coord1 = min(coord0 + ivec2(1, 1), ivec2(maxCoordInt, maxCoordInt));
    vec2 frac = texCoord - vec2(coord0);

    float h00 = imageLoad(u_HoleTexture, coord0).x;
    float h10 = imageLoad(u_HoleTexture, ivec2(coord1.x, coord0.y)).x;
    float h01 = imageLoad(u_HoleTexture, ivec2(coord0.x, coord1.y)).x;
    float h11 = imageLoad(u_HoleTexture, coord1).x;

    return mix(mix(h00, h10, frac.x), mix(h01, h11, frac.x), frac.y);
}

void StoreInstance(uint index, vec3 side, vec3 up, vec3 forward, vec3 position, float randColor, float windPhase)
{
    // Write 4 vec4s per instance (stride = 4 vec4s = 64 bytes)
    // This matches the C++ GrassInstanceData layout and bgfx example 48-drawindirect pattern
    uint baseIdx = index * 4u;
    u_InstanceBuffer[baseIdx + 0u] = vec4(side, randColor);
    u_InstanceBuffer[baseIdx + 1u] = vec4(up, length(up));
    u_InstanceBuffer[baseIdx + 2u] = vec4(forward, 0.0);
    u_InstanceBuffer[baseIdx + 3u] = vec4(position, windPhase);
}

NUM_THREADS(8, 8, 1)
void main()
{
    const uint grassGridRes = uint(u_GrassTerrain1.x + 0.5);
    const float terrainTexRes = u_GrassDispatch.w;  // Actual terrain texture resolution
    const ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    if (coord.x >= int(grassGridRes) || coord.y >= int(grassGridRes))
        return;

    // Convert virtual grass grid coordinate to normalized UV [0,1]
    const float uvDenom = max(float(grassGridRes) - 1.0, 1.0);
    const vec2 uv = vec2(coord) / uvDenom;

    if (SampleHoleBilinear(uv, terrainTexRes) > 0.5)
        return;
    
    // Sample density from painted mask texture using bilinear interpolation
    const float paintDensity = SamplePaintDensityBilinear(uv, terrainTexRes);
    if (paintDensity <= 0.0001)
        return;

    const int maskMode = int(u_GrassMaskParams.x + 0.5);
    const bool splatAvailable = u_GrassMaskParams.w > 0.5;
    float maskFactor = SampleMask(uv, maskMode, splatAvailable, terrainTexRes);

    float densityWeight = paintDensity;
    if (maskMode != 5)
    {
        densityWeight *= maskFactor;
    }

    if (densityWeight <= 0.0001)
        return;

    const float cellArea = max(u_GrassTerrain0.z, 1e-6);
    float bladesExact = densityWeight * u_GrassLayer0.x * cellArea;
    if (bladesExact <= 0.001)
        return;
    if (bladesExact > 0.0 && bladesExact < 1.0)
    {
        bladesExact = 1.0;
    }

    uint seed = uint(u_GrassSeedTime.x) ^
        (uint(coord.x) * 73856093u) ^
        (uint(coord.y) * 19349663u);

    uint blades = uint(bladesExact);
    const float fractional = bladesExact - float(blades);
    if (RandomFloat01(seed) < fractional)
    {
        ++blades;
    }

    const uint maxPerTexel = uint(u_GrassTerrain1.w + 0.5);
    blades = min(blades, maxPerTexel);
    if (blades == 0u)
        return;

    // Sample base height at cell center for initial height/slope rejection
    // Use UV-based sampling to properly map virtual grass grid to terrain texture
    const uint texResUint = uint(terrainTexRes + 0.5);
    const float baseCellHeightNorm = SampleHeightBilinear(uv, texResUint);
    const float baseCellHeightWorld = baseCellHeightNorm * u_GrassTerrain0.w;
    if (baseCellHeightWorld < u_GrassLayer0.y || baseCellHeightWorld > u_GrassLayer0.z)
        return;

    // Convert UV to terrain texture coord for normal computation
    ivec2 texCoord = ivec2(uv * (terrainTexRes - 1.0) + 0.5);
    int maxTexCoord = int(terrainTexRes) - 1;
    texCoord = clamp(texCoord, ivec2(0, 0), ivec2(maxTexCoord, maxTexCoord));
    vec3 normalLocal = ComputeLocalNormal(texCoord, texResUint, baseCellHeightNorm);
    const float slope = acos(clamp(normalLocal.y, 0.0, 1.0));
    if (slope > u_GrassLayer0.w)
        return;

    const mat4 worldMatrix = GetWorldMatrix();
    const MAT3 normalMatrix = GetNormalMatrix();

    const vec3 cameraRight = normalize(u_GrassCameraRight.xyz);
    const vec3 cameraForward = normalize(u_GrassCameraForward.xyz);
    const vec3 cameraPos = u_GrassCameraPos.xyz;

    for (uint i = 0u; i < blades; ++i)
    {
        const float offsetX = RandomFloat01(seed);
        const float offsetZ = RandomFloat01(seed);
        const float scale = mix(u_GrassLayer1.x, u_GrassLayer1.y, RandomFloat01(seed));
        const float yawAngle = (RandomFloat01(seed) * 2.0 - 1.0) * u_GrassLayer1.z;
        const float randColor = RandomFloat01(seed);
        const float windPhase = RandomFloat01(seed);

        // Sample height at the actual blade position using bilinear interpolation
        // This prevents grass from floating above or sinking into the terrain
        vec2 bladeUV = (vec2(float(coord.x) + offsetX, float(coord.y) + offsetZ)) / uvDenom;
        const float bladeHeightNorm = SampleHeightBilinear(bladeUV, texResUint);
        const float bladeHeightWorld = bladeHeightNorm * u_GrassTerrain0.w;

        vec3 sideLocal = normalize(cross(normalLocal, vec3(0.0, 0.0, 1.0)));
        if (length(sideLocal) < 1e-4)
        {
            sideLocal = normalize(cross(normalLocal, vec3(1.0, 0.0, 0.0)));
        }
        vec3 forwardLocal = normalize(cross(sideLocal, normalLocal));

        const float sinYaw = sin(yawAngle);
        const float cosYaw = cos(yawAngle);
        vec3 rotatedSide = normalize(sideLocal * cosYaw + forwardLocal * sinYaw);
        vec3 rotatedForward = normalize(cross(rotatedSide, normalLocal));

        vec3 upWorld = normalize(ApplyNormal(normalMatrix, normalLocal)) * scale;
        vec3 sideWorld = normalize(ApplyNormal(normalMatrix, rotatedSide)) * scale;
        vec3 forwardWorld = normalize(ApplyNormal(normalMatrix, rotatedForward)) * scale;

        const bool billboardMode = u_GrassLayerParams.x < 0.5;
        vec3 billboardSide = normalize(cross(upWorld, cameraForward));
        if (length(billboardSide) < 1e-4)
        {
            billboardSide = normalize(cross(upWorld, cameraRight));
        }
        if (billboardMode)
        {
            sideWorld = billboardSide * scale;
            forwardWorld = normalize(cross(sideWorld, upWorld));
        }

        const vec3 localPosition = vec3(
            (float(coord.x) + offsetX) * u_GrassTerrain0.x,
            bladeHeightWorld,
            (float(coord.y) + offsetZ) * u_GrassTerrain0.y);

#if BGFX_SHADER_LANGUAGE_HLSL
        const vec4 worldPos4 = mul(vec4(localPosition, 1.0), worldMatrix);
#else
        const vec4 worldPos4 = worldMatrix * vec4(localPosition, 1.0);
#endif
        vec3 worldPos = worldPos4.xyz;

        const float distanceToCamera = length(worldPos - cameraPos);
        const float minDistance = u_GrassMaskParams.y;
        const float maxDistance = u_GrassMaskParams.z;
        // Skip distance culling entirely when no valid range is provided.
        const bool distanceGateActive = (maxDistance > 0.0) && (maxDistance > (minDistance + 0.001));
        if (distanceGateActive)
        {
            if (distanceToCamera < minDistance || distanceToCamera > maxDistance)
            {
                continue;
            }
        }

        // Atomic increment on counter texture, returns previous value
        uint instanceIndex;
#if BGFX_SHADER_LANGUAGE_HLSL
        InterlockedAdd(u_CounterTexture[ivec2(0, 0)], 1u, instanceIndex);
#else
        instanceIndex = imageAtomicAdd(u_CounterTexture, ivec2(0, 0), 1u);
#endif
        if (instanceIndex >= uint(u_GrassTerrain1.z + 0.5))
        {
            // Capacity reached, exit loop
            break;
        }

        StoreInstance(instanceIndex, sideWorld, upWorld, forwardWorld, worldPos, randColor, windPhase);
    }
}
