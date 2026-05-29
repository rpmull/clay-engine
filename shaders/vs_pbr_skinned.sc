$input  a_position, a_normal, a_texcoord0, a_indices, a_weight
$output v_worldPos, v_normal, v_texcoord0, v_viewDir

#include <bgfx_shader.sh>
#include "lib_skinning.sh"

uniform vec4 u_cameraPos;
uniform vec4 u_UVTransform;
uniform mat4 u_normalMat; // provided by CPU (transpose(inverse(mat3(model))))
uniform vec4 u_DisplacementParams; // x=scale, y=bias

SAMPLER2D(s_displacement, 6);

void main()
{
    // Bone indices & weights
    ivec4 idx = ivec4(a_indices);
    vec4  w   = a_weight;

    // Keep weights sane; avoid scaling/shear from drift.
    float sumW = w.x + w.y + w.z + w.w;
    if (sumW > 0.0) {
        w /= sumW;
    }

    mat4 skin = ComputeSkinMatrix(idx, w);

    // Model * Skin once
    mat4 skinModel = mul(u_model[0], skin);

    // World position
    vec4 worldPos  = mul(skinModel, vec4(a_position, 1.0));

    // Skinned normal (ignore translation)
    vec3 skinnedNormal = mul(skin, vec4(a_normal, 0.0)).xyz;

    // Transform to world using CPU-provided normal matrix and normalize
    vec3 worldNormal = normalize(mul((mat3)u_normalMat, skinnedNormal));
    v_normal         = worldNormal;

    // Varyings
    vec2 scaledUV = a_texcoord0.xy * u_UVTransform.xy + u_UVTransform.zw;
    v_texcoord0.xy = scaledUV;
    v_texcoord0.zw = a_texcoord0.zw;
    
    if (abs(u_DisplacementParams.x) > 1e-6) {
        float h = texture2DLod(s_displacement, scaledUV, 0.0).r;
        float disp = (h - u_DisplacementParams.y) * u_DisplacementParams.x;
        worldPos.xyz += worldNormal * disp;
    }

    v_worldPos     = worldPos.xyz;
    v_viewDir   = normalize(u_cameraPos.xyz - worldPos.xyz);

    // Clip-space
    gl_Position = mul(u_viewProj, worldPos);
}
