#define SKINNING_USE_UNIFORM_INSTANCE_RECORD 1

$input  a_position, a_normal, a_texcoord0, a_texcoord1, a_indices, a_weight
$output v_worldPos, v_normal, v_texcoord0, v_viewDir

#include <bgfx_shader.sh>
#include "lib_skinning.sh"
#include "lib_morph_targets.sh"

uniform vec4 u_cameraPos;
uniform vec4 u_UVTransform;
uniform mat4 u_normalMat; // provided by CPU (transpose(inverse(mat3(model))))
uniform vec4 u_DisplacementParams; // x=scale, y=bias

SAMPLER2D(s_displacement, 6);

void main()
{
    ivec4 idx = ivec4(a_indices);
    vec4 w = a_weight;

    float sumW = w.x + w.y + w.z + w.w;
    if (sumW > 0.0) {
        w /= sumW;
    }

    vec4 skinningParams;
    vec4 skinningExtra;
    vec4 morphParams;
    vec4 objectIdPacked;
    mat4 meshFromSkeleton;
    ResolveSkinningState(vec4_splat(0.0), skinningParams, skinningExtra, meshFromSkeleton, morphParams, objectIdPacked);

    vec3 localPos = a_position;
    vec3 localNormal = a_normal;
    ApplyMorphTargets(morphParams, a_texcoord1.x, localPos, localNormal);

    mat4 skin = ComputeSkinMatrixWithState(idx, w, skinningParams, skinningExtra, meshFromSkeleton);
    mat4 skinModel = mul(u_model[0], skin);

    vec4 worldPos = mul(skinModel, vec4(localPos, 1.0));
    vec3 skinnedNormal = mul(skin, vec4(localNormal, 0.0)).xyz;
    vec3 worldNormal = normalize(mul((mat3)u_normalMat, skinnedNormal));
    v_normal = worldNormal;

    vec2 scaledUV = a_texcoord0.xy * u_UVTransform.xy + u_UVTransform.zw;
    v_texcoord0.xy = scaledUV;
    v_texcoord0.zw = a_texcoord0.zw;

    if (abs(u_DisplacementParams.x) > 1e-6) {
        float h = texture2DLod(s_displacement, scaledUV, 0.0).r;
        float disp = (h - u_DisplacementParams.y) * u_DisplacementParams.x;
        worldPos.xyz += worldNormal * disp;
    }

    v_worldPos = worldPos.xyz;
    v_viewDir = normalize(u_cameraPos.xyz - worldPos.xyz);
    gl_Position = mul(u_viewProj, worldPos);
}
