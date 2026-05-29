#define SKINNING_USE_UNIFORM_INSTANCE_RECORD 1

$input  a_position, a_normal, a_texcoord0, a_texcoord1, a_indices, a_weight
$output v_worldPos, v_normal

#include <bgfx_shader.sh>
#include "lib_skinning.sh"
#include "lib_morph_targets.sh"

uniform vec4 u_psxWorld;  // x=vertexWorldAmp (meters), y=tileSize (meters), z=unused, w=unused
uniform mat4 u_lightViewProj;

void main()
{
    ivec4 idx = ivec4(a_indices);
    vec4 w = a_weight;
    float sumW = max(w.x + w.y + w.z + w.w, 1e-6);
    w /= sumW;

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
    vec3 worldNormal = normalize(mul((mat3)u_model[0], mul((mat3)skin, localNormal)));

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
