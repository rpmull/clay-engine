#define SKINNING_USE_INSTANCE_RECORD 1

$input  a_position, a_normal, a_texcoord0, a_texcoord1, a_indices, a_weight, i_data0, i_data1, i_data2, i_data3, i_data4
$output v_texcoord1

#include <bgfx_shader.sh>
#include "lib_skinning.sh"
#include "lib_morph_targets.sh"

uniform vec4 u_UVTransform;
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
    ResolveSkinningState(i_data4, skinningParams, skinningExtra, meshFromSkeleton, morphParams, objectIdPacked);

    vec3 localPos = a_position;
    vec3 localNormal = a_normal;
    ApplyMorphTargets(morphParams, a_texcoord1.x, localPos, localNormal);

    mat4 skin = ComputeSkinMatrixWithState(idx, w, skinningParams, skinningExtra, meshFromSkeleton);
    vec4 skinnedPos = mul(skin, vec4(localPos, 1.0));
    vec3 skinnedNormal = mul(skin, vec4(localNormal, 0.0)).xyz;

    vec3 basisX = i_data0.xyz;
    vec3 basisY = i_data1.xyz;
    vec3 basisZ = i_data2.xyz;
    vec3 translation = i_data3.xyz;

    vec4 worldPos = vec4(
        translation * skinnedPos.w +
        basisX * skinnedPos.x +
        basisY * skinnedPos.y +
        basisZ * skinnedPos.z,
        1.0
    );

    vec3 c0 = cross(basisY, basisZ);
    vec3 c1 = cross(basisZ, basisX);
    vec3 c2 = cross(basisX, basisY);
    float det = dot(basisX, c0);
    float invDet = (abs(det) > 1e-8) ? (1.0 / det) : 1.0;
    vec3 worldNormal = normalize(
        (c0 * skinnedNormal.x + c1 * skinnedNormal.y + c2 * skinnedNormal.z) * invDet
    );

    vec2 scaledUV = a_texcoord0.xy * u_UVTransform.xy + u_UVTransform.zw;
    if (abs(u_DisplacementParams.x) > 1e-6) {
        float h = texture2DLod(s_displacement, scaledUV, 0.0).r;
        float disp = (h - u_DisplacementParams.y) * u_DisplacementParams.x;
        worldPos.xyz += worldNormal * disp;
    }

    v_texcoord1 = objectIdPacked;
    gl_Position = mul(u_viewProj, worldPos);
}
