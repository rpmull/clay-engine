#define SKINNING_USE_UNIFORM_INSTANCE_RECORD 1

#include "bgfx_compute.sh"
#include "lib_skinning.sh"
#include "lib_morph_targets.sh"

BUFFER_RO(u_sourcePositions, vec4, 0);
BUFFER_RO(u_sourceNormals, vec4, 1);
BUFFER_RO(u_sourceUvs, vec4, 2);
BUFFER_RO(u_sourceBoneIndices, vec4, 3);
BUFFER_RO(u_sourceBoneWeights, vec4, 4);
BUFFER_RW(u_outputVertices, vec4, 5);

uniform vec4 u_materializedSkinningDispatch; // x = vertexCount

NUM_THREADS(64, 1, 1)
void main()
{
    uint vertexId = gl_GlobalInvocationID.x;
    uint vertexCount = uint(u_materializedSkinningDispatch.x + 0.5);
    if (vertexId >= vertexCount) {
        return;
    }

    vec3 localPos = u_sourcePositions[vertexId].xyz;
    vec3 localNormal = u_sourceNormals[vertexId].xyz;
    vec2 uv = u_sourceUvs[vertexId].xy;
    ivec4 idx = ivec4(u_sourceBoneIndices[vertexId] + vec4_splat(0.5));
    vec4 weights = u_sourceBoneWeights[vertexId];

    float sumW = weights.x + weights.y + weights.z + weights.w;
    if (sumW > 0.0) {
        weights /= sumW;
    }

    vec4 skinningParams;
    vec4 skinningExtra;
    vec4 morphParams;
    vec4 objectIdPacked;
    mat4 meshFromSkeleton;
    ResolveSkinningState(
        vec4_splat(0.0),
        skinningParams,
        skinningExtra,
        meshFromSkeleton,
        morphParams,
        objectIdPacked);

    ApplyMorphTargets(morphParams, float(vertexId), localPos, localNormal);

    mat4 skin = ComputeSkinMatrixWithState(
        idx,
        weights,
        skinningParams,
        skinningExtra,
        meshFromSkeleton);
    vec3 skinnedPos = mul(skin, vec4(localPos, 1.0)).xyz;
    vec3 skinnedNormal = mul(skin, vec4(localNormal, 0.0)).xyz;
    float normalLenSq = dot(skinnedNormal, skinnedNormal);
    if (normalLenSq > 1e-10) {
        skinnedNormal *= inversesqrt(normalLenSq);
    }

    uint outputBase = vertexId * 2u;
    u_outputVertices[outputBase + 0u] = vec4(skinnedPos, skinnedNormal.x);
    u_outputVertices[outputBase + 1u] = vec4(skinnedNormal.y, skinnedNormal.z, uv.x, uv.y);
}
