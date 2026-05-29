#define SKINNING_USE_INSTANCE_RECORD 1

$input  a_position, a_normal, a_indices, a_weight, i_data0, i_data1, i_data2, i_data3, i_data4
$output v_worldPos, v_normal

#include <bgfx_shader.sh>
#include "lib_skinning.sh"

uniform vec4 u_psxWorld;  // x=vertexWorldAmp (meters), y=tileSize (meters), z=unused, w=unused
uniform mat4 u_lightViewProj;

void main()
{
    ivec4 idx = ivec4(a_indices);
    vec4 w = a_weight;
    float s = max(w.x + w.y + w.z + w.w, 1e-6);
    w /= s;

    vec4 skinningParams;
    vec4 skinningExtra;
    vec4 morphParams;
    vec4 objectIdPacked;
    mat4 meshFromSkeleton;
    ResolveSkinningState(i_data4, skinningParams, skinningExtra, meshFromSkeleton, morphParams, objectIdPacked);

    mat4 skin = ComputeSkinMatrixWithState(idx, w, skinningParams, skinningExtra, meshFromSkeleton);
    vec4 skinnedPos = mul(skin, vec4(a_position, 1.0));
    vec3 skinnedNormal = mul(skin, vec4(a_normal, 0.0)).xyz;

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
    gl_Position = mul(u_lightViewProj, worldPos);
}
