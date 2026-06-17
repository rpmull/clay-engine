$input  a_position, a_normal, a_texcoord0, a_indices, a_weight
$output v_worldPos, v_normal, v_texcoord0, v_viewDir

#include <bgfx_shader.sh>
#include "lib_skinning.sh"

uniform vec4 u_psxParams; // x=jitter_amp_px, y=affine_factor, z=light_influence, w=normalPerturbAmp
uniform vec4 u_psxWorld;  // x=vertexWorldAmp (meters), y=tileSize (meters), z=unused, w=unused
uniform vec4 u_cameraPos;
uniform vec4 u_UVTransform; // xy=scale, zw=offset (matches PBR; defaults to (1,1,0,0))
uniform mat4 u_normalMat; // transpose(inverse(mat3(model)))

void main()
{
    ivec4 idx = ivec4(a_indices);
    vec4  w   = a_weight;
    float s = max(w.x + w.y + w.z + w.w, 1e-6);
    w /= s;
    mat4 skin = ComputeSkinMatrix(idx, w);
    vec4 worldPos = mul(mul(u_model[0], skin), vec4(a_position, 1.0));
    vec3 skinnedNormal = mul((mat3)skin, a_normal);
    vec3 worldNormal = normalize(mul((mat3)u_normalMat, skinnedNormal));

    // Planar-preserving world-space offset: per-tile constant along normal
    float worldAmp = max(u_psxWorld.x, 0.0);
    float tileSize = max(u_psxWorld.y, 1e-4);
    if (worldAmp > 0.0) {
        vec3 key = floor(worldPos.xyz / tileSize);
        float n = fract(sin(dot(key, vec3(12.9898, 78.233, 45.164))) * 43758.5453);
        float offset = (n - 0.5) * worldAmp;
        worldPos.xyz += worldNormal * offset;
    }

    v_worldPos = worldPos.xyz;
    v_normal = worldNormal;
    vec4 clip = mul(u_viewProj, worldPos);
    float px = max(u_psxParams.x, 0.0);
    if (px > 0.0) {
        // Preserve the original clip-space W sign so vertices near/behind the camera
        // do not flip NDC quadrants and explode sparse geometry like large quads.
        float safeW = (clip.w < 0.0 ? -1.0 : 1.0) * max(abs(clip.w), 1e-6);
        vec2 ndc = clip.xy / safeW;
        float step = max(px / 540.0, 1e-6);
        ndc = floor(ndc / step + 0.5) * step;
        clip.xy = ndc * clip.w;
    }
    v_texcoord0.xy = a_texcoord0.xy * u_UVTransform.xy + u_UVTransform.zw;
    v_viewDir = normalize(u_cameraPos.xyz - worldPos.xyz);
    gl_Position = clip;
}

