$input a_position, a_texcoord0
$output v_position, v_texcoord0, v_heightUV, v_worldPos, v_normal

#include <bgfx_shader.sh>

SAMPLER2D(s_heightTexture, 0);

uniform vec4 u_chunkParams;  // xy = chunk start in local XZ, zw = chunk size in local XZ
uniform vec4 u_heightParams; // x = MaxHeight, yz = cell size (for future use)
uniform vec4 u_texelSize;    // xy = 1 / heightTextureResolution (per axis)
uniform mat4 u_normalMat;    // CPU-provided normal matrix (transpose(inverse(mat3(model))))

void main()
{
    // Compute chunk-local UVs from the vertex XZ position and chunk layout.
    vec2 localXZ   = a_position.xz - u_chunkParams.xy;
    vec2 chunkSize = max(u_chunkParams.zw, vec2(1e-6, 1e-6));
    vec2 chunkUV   = clamp(localXZ / chunkSize, vec2(0.0, 0.0), vec2(1.0, 1.0));

    // Center sampling inside texels based on the actual height-texture resolution.
    vec2 texelSize         = max(u_texelSize.xy, vec2(1e-6, 1e-6));
    vec2 texelCenterOffset = texelSize * 0.5;
    vec2 sampleUV          = chunkUV * (vec2(1.0, 1.0) - texelSize) + texelCenterOffset;

    // Heightmap texture stores a normalized 0..1 height in a float channel.
    float h = texture2DLod(s_heightTexture, sampleUV, 0.0).x;

    vec3 displaced = a_position;
    displaced.y    = h * u_heightParams.x; // u_heightParams.x == MaxHeight

    // Compute normal from height gradient using finite differences
    // Sample neighboring heights for normal calculation
    // Clamp UVs to prevent sampling outside texture bounds
    vec2 texelStep = texelSize.xy;
    float hCenter = h;
    vec2 clampedUVL = clamp(sampleUV + vec2(-texelStep.x, 0.0), vec2(0.0, 0.0), vec2(1.0, 1.0));
    vec2 clampedUVR = clamp(sampleUV + vec2( texelStep.x, 0.0), vec2(0.0, 0.0), vec2(1.0, 1.0));
    vec2 clampedUVD = clamp(sampleUV + vec2(0.0, -texelStep.y), vec2(0.0, 0.0), vec2(1.0, 1.0));
    vec2 clampedUVU = clamp(sampleUV + vec2(0.0,  texelStep.y), vec2(0.0, 0.0), vec2(1.0, 1.0));
    float hL = texture2DLod(s_heightTexture, clampedUVL, 0.0).x;
    float hR = texture2DLod(s_heightTexture, clampedUVR, 0.0).x;
    float hD = texture2DLod(s_heightTexture, clampedUVD, 0.0).x;
    float hU = texture2DLod(s_heightTexture, clampedUVU, 0.0).x;
    
    // Calculate height differences in world space
    vec2 sampleSpacing = max(u_heightParams.yz, vec2(1e-6, 1e-6));
    float dhdx = (hR - hL) * u_heightParams.x / (2.0 * sampleSpacing.x);
    float dhdz = (hU - hD) * u_heightParams.x / (2.0 * sampleSpacing.y);
    
    // Normal is perpendicular to the surface gradient
    // N = normalize(vec3(-dh/dx, 1.0, -dh/dz))
    vec3 normal = normalize(vec3(-dhdx, 1.0, -dhdz));

    v_texcoord0 = a_texcoord0;
    v_heightUV  = sampleUV;  // Pass the correct height texture UV to fragment shader
    v_position  = displaced;
    v_worldPos  = mul(u_model[0], vec4(displaced, 1.0)).xyz; // Transform to world space
    // Transform normal to world space using proper normal matrix (handles non-uniform scaling correctly)
    v_normal    = normalize(mul((mat3)u_normalMat, normal));
    gl_Position = mul(u_modelViewProj, vec4(displaced, 1.0));
}