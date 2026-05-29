$input a_position, a_texcoord0
$output v_heightUV

#include <bgfx_shader.sh>

SAMPLER2D(s_heightTexture, 0);

uniform vec4 u_chunkParams;  // xy = chunk start in local XZ, zw = chunk size in local XZ
uniform vec4 u_heightParams; // x = MaxHeight, yz = cell size (for future use)
uniform vec4 u_texelSize;    // xy = 1 / heightTextureResolution (per axis)
uniform mat4 u_lightViewProj;

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

    // Transform to world space
    vec4 worldPos = mul(u_model[0], vec4(displaced, 1.0));
    v_heightUV = sampleUV;

    // Output position using light view-projection for shadow map
    gl_Position = mul(u_lightViewProj, worldPos);
}




