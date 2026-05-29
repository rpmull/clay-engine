uniform vec4 u_skinningBoneAtlasInfo;   // xy = atlas size in texels, zw = inverse size
uniform vec4 u_skinningRemapAtlasInfo;  // xy = atlas size in texels, zw = inverse size
uniform vec4 u_skinningInstanceAtlasInfo; // xy = atlas size in texels, zw = inverse size
uniform vec4 u_skinningParams;          // x = bone base matrix, y = remap base texel, z = use remap, w = max bone index
uniform vec4 u_skinningExtra;           // x = correction base matrix, y = use correction, z = max source index, w = unused
uniform vec4 u_skinningInstanceRecord;  // x = atlas record index for non-instanced GPU-morph draws
uniform mat4 u_skinningMeshFromSkeleton;
uniform vec4 u_skinningMorphParams;     // x = morph vertex base, y = active base, z = active count, w = enabled

SAMPLER2D(s_boneAtlas, 10);
SAMPLER2D(s_boneRemapAtlas, 11);
SAMPLER2D(s_skinningInstanceAtlas, 12);

const float kSkinningInstanceRecordTexelCount = 8.0;

vec2 SkinningAtlasUv(float texelIndex, vec4 atlasInfo)
{
    float width = max(atlasInfo.x, 1.0);
    float y = floor(texelIndex / width);
    float x = texelIndex - y * width;
    return vec2((x + 0.5) * atlasInfo.z, (y + 0.5) * atlasInfo.w);
}

mat4 SkinningMatrixFromAtlasColumns(vec4 c0, vec4 c1, vec4 c2, vec4 c3)
{
    return mtxFromCols4(c0, c1, c2, c3);
}

vec4 FetchSkinningInstanceRecordTexel(float recordIndex, float texelOffset)
{
    float texelIndex = max(recordIndex, 0.0) * kSkinningInstanceRecordTexelCount + texelOffset;
    return texture2DLod(s_skinningInstanceAtlas, SkinningAtlasUv(texelIndex, u_skinningInstanceAtlasInfo), 0.0);
}

void ResolveSkinningState(vec4 instanceMetadata,
                          out vec4 skinningParams,
                          out vec4 skinningExtra,
                          out mat4 meshFromSkeleton,
                          out vec4 morphParams,
                          out vec4 objectIdPacked)
{
#if defined(SKINNING_USE_INSTANCE_RECORD)
    float recordIndex = instanceMetadata.x;
    if (recordIndex >= 0.0) {
        skinningParams = FetchSkinningInstanceRecordTexel(recordIndex, 0.0);
        skinningExtra = FetchSkinningInstanceRecordTexel(recordIndex, 1.0);
        vec4 c0 = FetchSkinningInstanceRecordTexel(recordIndex, 2.0);
        vec4 c1 = FetchSkinningInstanceRecordTexel(recordIndex, 3.0);
        vec4 c2 = FetchSkinningInstanceRecordTexel(recordIndex, 4.0);
        vec4 c3 = FetchSkinningInstanceRecordTexel(recordIndex, 5.0);
        meshFromSkeleton = SkinningMatrixFromAtlasColumns(c0, c1, c2, c3);
        morphParams = FetchSkinningInstanceRecordTexel(recordIndex, 6.0);
        objectIdPacked = FetchSkinningInstanceRecordTexel(recordIndex, 7.0);
    } else {
        skinningParams = vec4_splat(0.0);
        skinningExtra = vec4_splat(0.0);
        meshFromSkeleton = mat4(
            vec4(1.0, 0.0, 0.0, 0.0),
            vec4(0.0, 1.0, 0.0, 0.0),
            vec4(0.0, 0.0, 1.0, 0.0),
            vec4(0.0, 0.0, 0.0, 1.0)
        );
        morphParams = vec4_splat(0.0);
        objectIdPacked = vec4(0.0, 0.0, 0.0, 1.0);
    }
#elif defined(SKINNING_USE_UNIFORM_INSTANCE_RECORD)
    skinningParams = u_skinningParams;
    skinningExtra = u_skinningExtra;
    meshFromSkeleton = u_skinningMeshFromSkeleton;
    morphParams = u_skinningMorphParams;
    objectIdPacked = vec4(0.0, 0.0, 0.0, 1.0);
#else
    skinningParams = u_skinningParams;
    skinningExtra = u_skinningExtra;
    meshFromSkeleton = u_skinningMeshFromSkeleton;
    morphParams = vec4_splat(0.0);
    objectIdPacked = vec4(0.0, 0.0, 0.0, 1.0);
#endif
}

int ResolveSkinningSourceIndex(int compactIndex, vec4 skinningParams, vec4 skinningExtra)
{
    int maxSourceIndex = int(floor(skinningExtra.z + 0.5));
    return clamp(compactIndex, 0, maxSourceIndex);
}

int ResolveSkinningBoneIndex(int compactIndex, vec4 skinningParams, vec4 skinningExtra)
{
    int sourceIndex = ResolveSkinningSourceIndex(compactIndex, skinningParams, skinningExtra);
    int maxBoneIndex = int(floor(skinningParams.w + 0.5));
    if (skinningParams.z > 0.5) {
        float remapTexelIndex = skinningParams.y + float(sourceIndex);
        vec2 remapUv = SkinningAtlasUv(remapTexelIndex, u_skinningRemapAtlasInfo);
        float encoded = texture2DLod(s_boneRemapAtlas, remapUv, 0.0).r;
        int mappedIndex = int(floor(encoded * 65535.0 + 0.5));
        return clamp(mappedIndex, 0, maxBoneIndex);
    }

    return clamp(sourceIndex, 0, maxBoneIndex);
}

mat4 FetchSkinningBoneMatrix(int compactIndex, vec4 skinningParams, vec4 skinningExtra)
{
    int boneIndex = ResolveSkinningBoneIndex(compactIndex, skinningParams, skinningExtra);
    float matrixBaseTexel = (skinningParams.x + float(boneIndex)) * 4.0;
    vec4 c0 = texture2DLod(s_boneAtlas, SkinningAtlasUv(matrixBaseTexel + 0.0, u_skinningBoneAtlasInfo), 0.0);
    vec4 c1 = texture2DLod(s_boneAtlas, SkinningAtlasUv(matrixBaseTexel + 1.0, u_skinningBoneAtlasInfo), 0.0);
    vec4 c2 = texture2DLod(s_boneAtlas, SkinningAtlasUv(matrixBaseTexel + 2.0, u_skinningBoneAtlasInfo), 0.0);
    vec4 c3 = texture2DLod(s_boneAtlas, SkinningAtlasUv(matrixBaseTexel + 3.0, u_skinningBoneAtlasInfo), 0.0);
    return SkinningMatrixFromAtlasColumns(c0, c1, c2, c3);
}

mat4 FetchSkinningCorrectionMatrix(int compactIndex, vec4 skinningParams, vec4 skinningExtra)
{
    if (skinningExtra.y <= 0.5) {
        return mat4(
            vec4(1.0, 0.0, 0.0, 0.0),
            vec4(0.0, 1.0, 0.0, 0.0),
            vec4(0.0, 0.0, 1.0, 0.0),
            vec4(0.0, 0.0, 0.0, 1.0)
        );
    }

    int sourceIndex = ResolveSkinningSourceIndex(compactIndex, skinningParams, skinningExtra);
    float matrixBaseTexel = (skinningExtra.x + float(sourceIndex)) * 4.0;
    vec4 c0 = texture2DLod(s_boneAtlas, SkinningAtlasUv(matrixBaseTexel + 0.0, u_skinningBoneAtlasInfo), 0.0);
    vec4 c1 = texture2DLod(s_boneAtlas, SkinningAtlasUv(matrixBaseTexel + 1.0, u_skinningBoneAtlasInfo), 0.0);
    vec4 c2 = texture2DLod(s_boneAtlas, SkinningAtlasUv(matrixBaseTexel + 2.0, u_skinningBoneAtlasInfo), 0.0);
    vec4 c3 = texture2DLod(s_boneAtlas, SkinningAtlasUv(matrixBaseTexel + 3.0, u_skinningBoneAtlasInfo), 0.0);
    return SkinningMatrixFromAtlasColumns(c0, c1, c2, c3);
}

mat4 SkinningZeroMatrix()
{
    return mat4(
        vec4(0.0, 0.0, 0.0, 0.0),
        vec4(0.0, 0.0, 0.0, 0.0),
        vec4(0.0, 0.0, 0.0, 0.0),
        vec4(0.0, 0.0, 0.0, 0.0)
    );
}

mat4 AccumulateSkinningInfluence(mat4 skin,
                                 int compactIndex,
                                 float weight,
                                 vec4 skinningParams,
                                 vec4 skinningExtra)
{
    if (weight <= 0.0) {
        return skin;
    }

    mat4 bone = FetchSkinningBoneMatrix(compactIndex, skinningParams, skinningExtra);
    if (skinningExtra.y > 0.5) {
        bone = mul(bone, FetchSkinningCorrectionMatrix(compactIndex, skinningParams, skinningExtra));
    }

    return skin + weight * bone;
}

mat4 ComputeSkinMatrixWithState(ivec4 idx,
                                vec4 weights,
                                vec4 skinningParams,
                                vec4 skinningExtra,
                                mat4 meshFromSkeleton)
{
    mat4 skin = SkinningZeroMatrix();
    skin = AccumulateSkinningInfluence(skin, idx.x, weights.x, skinningParams, skinningExtra);
    skin = AccumulateSkinningInfluence(skin, idx.y, weights.y, skinningParams, skinningExtra);
    skin = AccumulateSkinningInfluence(skin, idx.z, weights.z, skinningParams, skinningExtra);
    skin = AccumulateSkinningInfluence(skin, idx.w, weights.w, skinningParams, skinningExtra);

    return mul(meshFromSkeleton, skin);
}

mat4 ComputeSkinMatrix(ivec4 idx, vec4 weights)
{
    vec4 skinningParams;
    vec4 skinningExtra;
    vec4 morphParams;
    vec4 objectIdPacked;
    mat4 meshFromSkeleton;
    ResolveSkinningState(vec4_splat(0.0), skinningParams, skinningExtra, meshFromSkeleton, morphParams, objectIdPacked);
    return ComputeSkinMatrixWithState(idx, weights, skinningParams, skinningExtra, meshFromSkeleton);
}

vec4 FetchSkinningObjectIdPacked(vec4 instanceMetadata)
{
    vec4 skinningParams;
    vec4 skinningExtra;
    vec4 morphParams;
    vec4 objectIdPacked;
    mat4 meshFromSkeleton;
    ResolveSkinningState(instanceMetadata, skinningParams, skinningExtra, meshFromSkeleton, morphParams, objectIdPacked);
    return objectIdPacked;
}
