uniform vec4 u_morphVertexAtlasInfo; // xy = atlas size in texels, zw = inverse size
uniform vec4 u_morphEntryAtlasInfo;  // xy = atlas size in texels, zw = inverse size
uniform vec4 u_morphActiveAtlasInfo; // xy = atlas size in texels, zw = inverse size

SAMPLER2D(s_morphVertexAtlas, 13);
SAMPLER2D(s_morphEntryAtlas, 14);
SAMPLER2D(s_morphActiveAtlas, 15);

const int kMorphMaxEntriesPerVertex = 32;
const int kMorphMaxActiveShapes = 24;
const float kMorphEntryTexelCount = 2.0;

vec4 FetchMorphVertexTexel(float texelIndex)
{
    return texture2DLod(s_morphVertexAtlas, SkinningAtlasUv(texelIndex, u_morphVertexAtlasInfo), 0.0);
}

vec4 FetchMorphEntryTexel(float entryIndex, float texelOffset)
{
    float texelIndex = entryIndex * kMorphEntryTexelCount + texelOffset;
    return texture2DLod(s_morphEntryAtlas, SkinningAtlasUv(texelIndex, u_morphEntryAtlasInfo), 0.0);
}

vec4 FetchMorphActiveTexel(float texelIndex)
{
    return texture2DLod(s_morphActiveAtlas, SkinningAtlasUv(texelIndex, u_morphActiveAtlasInfo), 0.0);
}

float ResolveMorphWeight(float activeBase, float activeCount, float shapeIndex)
{
    for (int i = 0; i < kMorphMaxActiveShapes; ++i) {
        if (float(i) >= activeCount) {
            break;
        }

        vec4 active = FetchMorphActiveTexel(activeBase + float(i));
        if (abs(active.x - shapeIndex) < 0.5) {
            return active.y;
        }
    }

    return 0.0;
}

void ApplyMorphTargets(vec4 morphParams,
                       float vertexIndex,
                       inout vec3 position,
                       inout vec3 normal)
{
    if (morphParams.w <= 0.5) {
        return;
    }

    vec4 range = FetchMorphVertexTexel(morphParams.x + vertexIndex);
    float entryBase = range.x;
    float entryCount = range.y;

    for (int i = 0; i < kMorphMaxEntriesPerVertex; ++i) {
        if (float(i) >= entryCount) {
            break;
        }

        vec4 posDelta = FetchMorphEntryTexel(entryBase + float(i), 0.0);
        float weight = ResolveMorphWeight(morphParams.y, morphParams.z, posDelta.x);
        if (abs(weight) <= 1e-6) {
            continue;
        }

        vec4 normDelta = FetchMorphEntryTexel(entryBase + float(i), 1.0);
        position += posDelta.yzw * weight;
        normal += normDelta.xyz * weight;
    }

    float normalLenSq = dot(normal, normal);
    if (normalLenSq > 1e-10) {
        normal *= inversesqrt(normalLenSq);
    }
}
