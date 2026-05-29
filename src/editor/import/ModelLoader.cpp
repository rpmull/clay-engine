// ModelLoader.cpp
#include "ModelLoader.h"
#include "core/rendering/VertexTypes.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/material.h>
#include <assimp/material.h>

#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <fstream>
#include "core/vfs/FileSystem.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include "editor/Project.h"
#include <assimp/GltfMaterial.h>

// --------------------------------- Helpers ---------------------------------
static glm::mat4 AiToGlmTransposed(const aiMatrix4x4& m)
{
    // Assimp matrices are row-major; GLM is column-major -> transpose the constructed mat.
    glm::mat4 mat(
        m.a1, m.b1, m.c1, m.d1,
        m.a2, m.b2, m.c2, m.d2,
        m.a3, m.b3, m.c3, m.d3,
        m.a4, m.b4, m.c4, m.d4
    );
    return glm::transpose(mat);
}

static inline glm::mat4 AiToGlm(const aiMatrix4x4& m)
{
    // GLM's 16-float ctor is column-major: we pass columns (a*, b*, c*, d*)
    return glm::mat4(
        m.a1, m.b1, m.c1, m.d1,
        m.a2, m.b2, m.c2, m.d2,
        m.a3, m.b3, m.c3, m.d3,
        m.a4, m.b4, m.c4, m.d4
    );
}


static std::string GetTexPath(const aiMaterial* mat, aiTextureType type)
{
    if (!mat) return {};
    aiString str;
    if (AI_SUCCESS == mat->GetTexture(type, 0, &str))
        return std::string(str.C_Str());
    return {};
}

// Some glTF 2.0 packs ORM; this tries common slots in reasonable order.
static void ExtractPbrTextures(const aiMaterial* aim,
    std::string& albedo,
    std::string& metallicRoughness,
    std::string& normal)
{
    // Base color or Diffuse
    albedo = GetTexPath(aim, aiTextureType_BASE_COLOR);
    if (albedo.empty()) albedo = GetTexPath(aim, aiTextureType_DIFFUSE);

    // Metallic-Roughness (try specific then fallbacks)
    metallicRoughness = GetTexPath(aim, aiTextureType_UNKNOWN);
    if (metallicRoughness.empty()) metallicRoughness = GetTexPath(aim, aiTextureType_METALNESS);
    if (metallicRoughness.empty()) metallicRoughness = GetTexPath(aim, aiTextureType_DIFFUSE_ROUGHNESS);

    // Normal map (FBX can surface this as NORMAL_CAMERA)
    normal = GetTexPath(aim, aiTextureType_NORMAL_CAMERA);
    if (normal.empty()) normal = GetTexPath(aim, aiTextureType_NORMALS);
    if (normal.empty()) normal = GetTexPath(aim, aiTextureType_HEIGHT); // some exporters misuse this
}

static std::string GetAuthoredMaterialName(const aiMaterial* mat)
{
    if (!mat) return {};
    aiString name;
    if (AI_SUCCESS == mat->Get(AI_MATKEY_NAME, name))
    {
        return std::string(name.C_Str());
    }
    return {};
}

static bool FileExists(const std::filesystem::path& p)
{
    if (p.empty()) return false;
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

static TextureSpecifier MakeEmbeddedTexture(const aiScene* scene, int idx, const std::string& modelName)
{
    TextureSpecifier spec;
    if (!scene || idx < 0 || (unsigned)idx >= scene->mNumTextures) return spec;
    const aiTexture* at = scene->mTextures[idx];
    if (!at) return spec;

    EmbeddedTextureData data;
    if (at->mHeight == 0)
    {
        size_t byteCount = static_cast<size_t>(at->mWidth);
        data.Bytes.resize(byteCount);
        if (byteCount > 0)
        {
            std::memcpy(data.Bytes.data(), at->pcData, byteCount);
        }
        data.IsCompressed = true;
        if (at->achFormatHint[0] != '\0')
        {
            data.FormatHint = at->achFormatHint;
        }
    }
    else
    {
        data.IsCompressed = false;
        data.Width = static_cast<int>(at->mWidth);
        data.Height = static_cast<int>(at->mHeight);
        data.Bytes.resize(static_cast<size_t>(at->mWidth) * static_cast<size_t>(at->mHeight) * 4u);
        for (int y = 0; y < at->mHeight; ++y)
        {
            for (int x = 0; x < at->mWidth; ++x)
            {
                const aiTexel& texel = at->pcData[y * at->mWidth + x];
                size_t offset = (static_cast<size_t>(y) * static_cast<size_t>(at->mWidth) + static_cast<size_t>(x)) * 4u;
                data.Bytes[offset + 0] = texel.r;
                data.Bytes[offset + 1] = texel.g;
                data.Bytes[offset + 2] = texel.b;
                data.Bytes[offset + 3] = texel.a;
            }
        }
    }
    spec.Embedded = std::move(data);
    std::string tag = modelName.empty() ? std::string("embedded://") : std::string("embedded://") + modelName + "/";
    std::string embeddedPath = tag + std::to_string(idx);
    std::string originalFilename = at->mFilename.C_Str();
    if (!originalFilename.empty())
    {
        std::filesystem::path filenamePath(originalFilename);
        std::string filenameOnly = filenamePath.filename().string();
        if (!filenameOnly.empty())
        {
            embeddedPath += "/" + filenameOnly;
        }
    }
    spec.Path = embeddedPath;
    return spec;
}

static std::string ResolveCandidatePath(const std::string& candidate,
                                        const std::string& baseDir,
                                        const std::string& modelName)
{
    if (candidate.empty()) return {};

    auto tryPath = [](const std::filesystem::path& p) -> std::string
    {
        return FileExists(p) ? p.string() : std::string();
    };

    std::filesystem::path candPath(candidate);
    if (candPath.is_absolute())
    {
        auto resolved = tryPath(candPath);
        if (!resolved.empty()) return resolved;
    }

    if (!baseDir.empty())
    {
        auto resolved = tryPath(std::filesystem::path(baseDir) / candidate);
        if (!resolved.empty()) return resolved;
    }

    std::filesystem::path assets = Project::GetAssetDirectory();
    if (!assets.empty())
    {
        std::filesystem::path texDir = assets / "textures";
        if (!modelName.empty())
        {
            texDir /= modelName;
        }
        auto resolved = tryPath(texDir / std::filesystem::path(candidate).filename());
        if (!resolved.empty()) return resolved;
    }

    return {};
}

static TextureSpecifier MakeTextureSpecifier(const std::string& candidate,
                                             const std::string& baseDir,
                                             const std::string& modelName,
                                             const aiScene* scene)
{
    TextureSpecifier spec;
    if (candidate.empty()) return spec;

    if (candidate[0] == '*')
    {
        try
        {
            int idx = std::stoi(candidate.substr(1));
            return MakeEmbeddedTexture(scene, idx, modelName);
        }
        catch (...)
        {
            return spec;
        }
    }

    std::string resolved = ResolveCandidatePath(candidate, baseDir, modelName);
    spec.Path = resolved.empty() ? candidate : resolved;
    return spec;
}

static inline bool IsFinite3(const glm::vec3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Normalize blend shape names coming from various importers/exporters.
// Some pipelines (e.g., certain FBX exports) may produce names like
// "MeshName.ShapeName" or even duplicated patterns like "Sex.Sex".
// For unified morph control we want consistent names across meshes, so
// we strip redundant prefixes when the suffix clearly repeats the same
// token or the mesh name.
static std::string NormalizeBlendShapeName(const std::string& rawName, const std::string& meshName)
{
    // Fast path: empty or short name
    if (rawName.empty()) return rawName;

    // Find the last separator among '.', ':', '/'
    size_t sepPos = rawName.find_last_of(".:/");
    if (sepPos != std::string::npos && sepPos + 1 < rawName.size())
    {
        std::string head = rawName.substr(0, sepPos);
        std::string tail = rawName.substr(sepPos + 1);

        // If the tail matches the mesh name, prefer the tail
        if (!meshName.empty() && tail == meshName)
            return tail;

        // If the last token before the separator equals the tail, prefer the tail
        size_t headSep = head.find_last_of(".:/");
        std::string headLast = (headSep == std::string::npos) ? head : head.substr(headSep + 1);
        if (!headLast.empty() && headLast == tail)
            return tail;
    }

    // Special-case duplicated token split by a single '.' (e.g., "Sex.Sex")
    size_t dot = rawName.find('.');
    if (dot != std::string::npos && dot + 1 < rawName.size())
    {
        std::string left = rawName.substr(0, dot);
        std::string right = rawName.substr(dot + 1);
        if (left == right)
            return right;
    }

    return rawName;
}

// Parse trailing name suffix keywords like "_a", "_bf" (stackable and order-independent at the end),
// e.g., "mesh_a_bf", "mesh_bf_a". Stops when a non-keyword token is encountered.
static void ParseSuffixMaterialHints(const std::string& name, bool& outAlphaBlend, bool& outShowBackfaces)
{
    outAlphaBlend = false; outShowBackfaces = false;
    if (name.empty()) return;
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    size_t pos = lower.size();
    while (pos > 0)
    {
        size_t us = lower.rfind('_', pos - 1);
        size_t start = (us == std::string::npos) ? 0 : us + 1;
        size_t len = pos - start;
        if (len == 0) break;
        std::string tok = lower.substr(start, len);
        bool matched = false;
        if (tok == "a") { outAlphaBlend = true; matched = true; }
        else if (tok == "bf") { outShowBackfaces = true; matched = true; }
        if (!matched) break; // stop when a non-keyword token is hit
        if (us == std::string::npos) break; // consumed whole string
        pos = us; // continue to the previous token
    }
}

// --------------------------------- Loader ----------------------------------
bool ModelLoader::s_FlipY = false;
bool ModelLoader::s_FlipZ = false;
bool ModelLoader::s_RotateY180 = false;

void ModelLoader::SetFlipYAxis(bool enabled) { s_FlipY = enabled; }
void ModelLoader::SetFlipZAxis(bool enabled) { s_FlipZ = enabled; }
bool ModelLoader::GetFlipYAxis() { return s_FlipY; }
bool ModelLoader::GetFlipZAxis() { return s_FlipZ; }
void ModelLoader::SetRotateY180(bool enabled) { s_RotateY180 = enabled; }
bool ModelLoader::GetRotateY180() { return s_RotateY180; }

Model ModelLoader::LoadModel(const std::string& filepath)
{
    // Initialize the predefined layouts once (from VertexTypes.h)
    static bool layoutsInit = false;
    if (!layoutsInit)
    {
        PBRVertex::Init();
        SkinnedPBRVertex::Init();
        layoutsInit = true;
    }

    Assimp::Importer importer;
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    // If file is inside a mounted pak, extract to temp cache before loading via Assimp
    std::string openPath = filepath;
    if (!std::filesystem::exists(openPath)) {
        std::vector<uint8_t> bytes;
        if (FileSystem::Instance().ReadFile(filepath, bytes)) {
            std::filesystem::path cacheDir = std::filesystem::temp_directory_path() / "claymore_pak_cache";
            std::error_code ec; std::filesystem::create_directories(cacheDir, ec);
            size_t h = std::hash<std::string>{}(filepath);
            std::string ext = std::filesystem::path(filepath).extension().string();
            std::filesystem::path outPath = cacheDir / ("model_" + std::to_string(h) + ext);
            std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
            if (out.is_open()) {
                if (!bytes.empty()) out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                out.close();
                openPath = outPath.string();
            }
        }
    }

    // Build post-process flags; avoid UV flip for glTF which is already OpenGL-style
    uint32_t ppFlags =
        aiProcess_Triangulate |
        aiProcess_GenNormals |
        aiProcess_CalcTangentSpace |
        aiProcess_JoinIdenticalVertices |
        aiProcess_ImproveCacheLocality |
        aiProcess_LimitBoneWeights |
        aiProcess_GlobalScale;
    {
        std::string ext = std::filesystem::path(filepath).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        if (!(ext == ".gltf" || ext == ".glb")) {
            ppFlags |= aiProcess_FlipUVs;
        }
    }

    const aiScene* scene = importer.ReadFile(openPath.c_str(), ppFlags);

    Model result;
    if (!scene || !scene->mRootNode)
    {
        std::cerr << "[ModelLoader] Failed to load: " << filepath
            << " (" << importer.GetErrorString() << ")\n";
        return result;
    }

    // Determine unit scale from source (e.g., FBX UnitScaleFactor). Default to 1.0.
    float importScale = 1.0f;
    if (scene->mMetaData)
    {
        double unitScaleDouble = 1.0;
        if (scene->mMetaData->Get("UnitScaleFactor", unitScaleDouble))
            importScale = static_cast<float>(unitScaleDouble);
    }

    // Normalize source up-axis to engine Y-up based on importer metadata
    int upAxis = 1, upSign = 1;
    if (scene->mMetaData)
    {
        (void)scene->mMetaData->Get("UpAxis", upAxis);
        (void)scene->mMetaData->Get("UpAxisSign", upSign);
    }
    std::cout << "[ModelLoader] UpAxis=" << upAxis << " UpAxisSign=" << upSign
              << " for '" << filepath << "'\n";
    glm::mat4 axisFix = glm::mat4(1.0f);
    if (upAxis == 2)
    {
        // Z-up -> Y-up
        axisFix = glm::rotate(glm::mat4(1.0f), (upSign >= 0 ? -glm::half_pi<float>() : glm::half_pi<float>()), glm::vec3(1.0f, 0.0f, 0.0f));
    }
    else if (upAxis == 0)
    {
        // X-up -> Y-up
        axisFix = glm::rotate(glm::mat4(1.0f), (upSign >= 0 ?  glm::half_pi<float>() : -glm::half_pi<float>()), glm::vec3(0.0f, 0.0f, 1.0f));
    }
    else if (upAxis == 1 && upSign < 0)
    {
        // -Y up -> +Y up
        axisFix = glm::rotate(glm::mat4(1.0f), glm::pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));
    }

    const std::string baseDir = std::filesystem::path(filepath).parent_path().string();
    result.SourcePath = filepath;
    result.SourceDirectory = baseDir;
    result.ModelName = std::filesystem::path(filepath).stem().string();

    // ---------------- Scene graph capture for fast instantiation ----------------
    // Helper: Assimp (row-major) -> GLM (column-major)
    auto AiToGlmLocal = [](const aiMatrix4x4& m) {
        return glm::mat4(
            m.a1, m.b1, m.c1, m.d1,
            m.a2, m.b2, m.c2, m.d2,
            m.a3, m.b3, m.c3, m.d3,
            m.a4, m.b4, m.c4, m.d4);
    };

    result.RootLocal = axisFix * AiToGlmLocal(scene->mRootNode->mTransformation);
    glm::mat4 invRootLocal = glm::inverse(result.RootLocal);
    result.MeshTransforms.assign(scene->mNumMeshes, glm::mat4(1.0f));
    result.MeshEntityNames.assign(scene->mNumMeshes, std::string());
    {
        std::function<void(aiNode*, const glm::mat4&)> traverse;
        traverse = [&](aiNode* node, const glm::mat4& parent) {
            glm::mat4 local = AiToGlmLocal(node->mTransformation);
            glm::mat4 global = parent * local;
            glm::mat4 rel = invRootLocal * global;
            for (unsigned i = 0; i < node->mNumMeshes; ++i) {
                unsigned mi = node->mMeshes[i];
                if (mi < result.MeshTransforms.size()) result.MeshTransforms[mi] = rel;
                if (mi < result.MeshEntityNames.size()) result.MeshEntityNames[mi] = node->mName.C_Str();
            }
            for (unsigned c = 0; c < node->mNumChildren; ++c) traverse(node->mChildren[c], global);
        };
        traverse(scene->mRootNode, axisFix);
    }

    // ---------------- Scene-wide bone prepass (stable indices across submeshes) ----------------
    std::unordered_map<std::string, uint32_t> boneIndexMap;
    result.BoneNames.clear();
    result.InverseBindPoses.clear();
    result.BoneParents.clear();

    auto registerBone = [&](const aiBone* abone) -> uint32_t
        {
            std::string name = abone->mName.C_Str();
            auto it = boneIndexMap.find(name);
            if (it != boneIndexMap.end()) return it->second;

            uint32_t idx = (uint32_t)result.BoneNames.size();
            boneIndexMap.emplace(name, idx);
            result.BoneNames.push_back(name);

            // Use raw construction (no extra transpose) to match skinning convention elsewhere
            glm::mat4 offset = AiToGlm(abone->mOffsetMatrix);
            result.InverseBindPoses.push_back(offset);

            return idx;
        };

    for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi)
    {
        const aiMesh* m = scene->mMeshes[mi];
        if (m->HasBones())
        {
            for (unsigned b = 0; b < m->mNumBones; ++b)
                (void)registerBone(m->mBones[b]);
        }
    }

    // Build BoneParents using the node hierarchy
    if (!result.BoneNames.empty()) {
        std::unordered_map<std::string, int> boneNameToIndex;
        for (int i = 0; i < (int)result.BoneNames.size(); ++i) boneNameToIndex[result.BoneNames[i]] = i;
        std::unordered_map<std::string, aiNode*> nodeByName;
        std::function<void(aiNode*)> gather = [&](aiNode* n){ nodeByName[n->mName.C_Str()] = n; for (unsigned c=0;c<n->mNumChildren;++c) gather(n->mChildren[c]); };
        gather(scene->mRootNode);
        result.BoneParents.assign(result.BoneNames.size(), -1);
        for (size_t i = 0; i < result.BoneNames.size(); ++i) {
            auto it = nodeByName.find(result.BoneNames[i]);
            if (it != nodeByName.end()) {
                aiNode* p = it->second->mParent;
                while (p) {
                    auto itBI = boneNameToIndex.find(p->mName.C_Str());
                    if (itBI != boneNameToIndex.end()) { result.BoneParents[i] = itBI->second; break; }
                    p = p->mParent;
                }
            }
        }
    }

    // ---------------- Material texture prepass (extract once for every material) ----------------
    struct ExtractedMaterialData
    {
        std::string AlbedoPath;
        std::string MetallicRoughnessPath;
        std::string NormalPath;
        glm::vec4   ColorTint = glm::vec4(1.0f);
        bool        HasTint   = false;
    };

    std::vector<ExtractedMaterialData> extractedMaterials;
    if (scene->HasMaterials())
    {
        extractedMaterials.resize(scene->mNumMaterials);
        for (unsigned mi = 0; mi < scene->mNumMaterials; ++mi)
        {
            std::string albedo, mr, normal;
            ExtractPbrTextures(scene->mMaterials[mi], albedo, mr, normal);
            extractedMaterials[mi].AlbedoPath = std::move(albedo);
            extractedMaterials[mi].MetallicRoughnessPath = std::move(mr);
            extractedMaterials[mi].NormalPath = std::move(normal);

            // Additional fallbacks for Blender/glTF graphs:
            // - If no albedo/base texture is supplied, use emissive texture as color source (common in unlit setups)
            if (extractedMaterials[mi].AlbedoPath.empty())
            {
                aiString em; if (scene->mMaterials[mi]->GetTexture(aiTextureType_EMISSIVE, 0, &em) == AI_SUCCESS)
                    extractedMaterials[mi].AlbedoPath = em.C_Str();
            }

            // - If still no texture, pull a color factor so material is not just default white
            // IMPORTANT: We must capture ALL colors including black and white because:
            //   1. Black is a valid intentional color choice (e.g., mouth interior)
            //   2. Each material slot must be tracked even if it uses the default white
            //   3. The old logic skipped black/white, causing material slots to appear "missing"
            if (extractedMaterials[mi].AlbedoPath.empty())
            {
                aiColor4D c{}; bool set = false;
                if (scene->mMaterials[mi]->Get(AI_MATKEY_BASE_COLOR, c) == AI_SUCCESS) {
                    // Only capture colors that are clearly intentional (not black or white defaults)
                    // Black (0,0,0) is Assimp's default "unset" value - treat as white tint
                    // White (1,1,1) is the engine's default tint anyway - no need to store
                    // Note: Alpha near 0 also indicates unset
                    float brightness = c.r + c.g + c.b;
                    bool isBlackOrUnset = (brightness < 0.01f) || (c.a < 0.001f);
                    bool isWhite = (c.r > 0.99f && c.g > 0.99f && c.b > 0.99f);
                    
                    if (!isBlackOrUnset && !isWhite) {
                        // This is an intentional non-default color - capture it
                        extractedMaterials[mi].ColorTint = glm::vec4(c.r, c.g, c.b, c.a);
                        extractedMaterials[mi].HasTint = true; 
                        set = true;
                        std::cout << "[ModelLoader] Captured base color for material " << mi 
                                  << ": (" << c.r << ", " << c.g << ", " << c.b << ", " << c.a << ")" << std::endl;
                    }
                    // Otherwise, leave HasTint = false so engine uses default white tint
                }
                if (!set && scene->mMaterials[mi]->Get(AI_MATKEY_COLOR_EMISSIVE, c) == AI_SUCCESS) {
                    // For emissive fallback, also skip black (unset) values
                    float brightness = c.r + c.g + c.b;
                    bool isBlackOrUnset = (brightness < 0.01f);
                    if (!isBlackOrUnset) {
                        extractedMaterials[mi].ColorTint = glm::vec4(c.r, c.g, c.b, 1.0f);
                        extractedMaterials[mi].HasTint = true;
                    }
                }
            }
            
            // Ensure default tint is always white (1,1,1,1) when no explicit tint is set
            if (!extractedMaterials[mi].HasTint) {
                extractedMaterials[mi].ColorTint = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
            }

            // glTF-specific extras we may care about later (two-sided flag handled during application)
            if (const aiMaterial* aim = scene->mMaterials[mi]) {
                int twoSided = 0; (void)aim->Get(AI_MATKEY_TWOSIDED, twoSided);
            }
        }
    }

    // ---------------- Convert meshes ----------------
    result.Meshes.reserve(scene->mNumMeshes);
    result.Materials.reserve(scene->mNumMeshes);
    result.MaterialSlotNames.reserve(scene->mNumMeshes);
    result.BlendShapes.reserve(scene->mNumMeshes);

    // Import options for non-skinned meshes
    const bool kFlipYOnImport = s_FlipY;
    const bool kFlipZOnImport = s_FlipZ;
    const bool kRotateY180    = s_RotateY180;

    // Build a set of bone names for fast lookup (to detect meshes parented to bones)
    std::unordered_set<std::string> boneNameSet;
    for (const auto& boneName : result.BoneNames) {
        boneNameSet.insert(boneName);
    }
    
    // Helper to check if a node (or its ancestors) is a bone
    auto isParentedToBone = [&](const std::string& meshNodeName) -> bool {
        if (boneNameSet.empty()) return false;
        // Check if the mesh's parent node name matches a bone
        // We stored mesh transforms by traversing nodes, so we need to check hierarchy
        // For now, check if the mesh node name itself is a bone (common for hair/accessories)
        if (boneNameSet.count(meshNodeName)) return true;
        return false;
    };

    for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi)
    {
        aiMesh* aMesh = scene->mMeshes[mi];
        const bool hasBoneWeights = (aMesh->mNumBones > 0);
        
        // Get the node name for this mesh
        std::string meshNodeName = (mi < result.MeshEntityNames.size() && !result.MeshEntityNames[mi].empty())
            ? result.MeshEntityNames[mi]
            : aMesh->mName.C_Str();
        
        // A mesh should be treated as skinned if:
        // 1. It has explicit bone weights (hasBoneWeights), OR
        // 2. The model has bones AND the mesh appears to be part of the character
        //    (heuristic: meshes on models with skeletons are likely skinned)
        // The actual skinning data will be provided in ModelPreprocessor if missing.
        const bool modelHasSkeleton = !result.BoneNames.empty();
        const bool meshParentedToBone = isParentedToBone(meshNodeName);
        const bool hasSkin = hasBoneWeights || (modelHasSkeleton && meshParentedToBone);
        
        // For flip/rotate transforms, only apply to truly static meshes (no skeleton at all)
        const bool flipYThisMesh = (kFlipYOnImport && !modelHasSkeleton);
        const bool flipZThisMesh = (kFlipZOnImport && !modelHasSkeleton);
        const bool rotateY180ThisMesh = (kRotateY180 && !modelHasSkeleton);

        // ---- Material capture (defer actual resource creation to instantiation time)
        MaterialSource matSource;
        // Mark as skinned ONLY if the mesh has bone weights OR is parented to a bone.
        // Unskinned meshes (like weapons, accessories) in a skeletal model should stay unskinned.
        // Users can enable UseParentSkeleton on such meshes if they need dynamic attachment.
        matSource.Skinned = hasSkin;

        std::string nodeName = (mi < result.MeshEntityNames.size() && !result.MeshEntityNames[mi].empty())
            ? result.MeshEntityNames[mi]
            : aMesh->mName.C_Str();
        bool wantBlend = false, wantBackfaces = false;
        ParseSuffixMaterialHints(nodeName, wantBlend, wantBackfaces);
        matSource.AlphaBlend = wantBlend;
        matSource.TwoSided = wantBackfaces;

        if (scene->HasMaterials() && aMesh->mMaterialIndex < scene->mNumMaterials)
        {
            const ExtractedMaterialData& md = extractedMaterials[aMesh->mMaterialIndex];
            matSource.Name = GetAuthoredMaterialName(scene->mMaterials[aMesh->mMaterialIndex]);

            if (md.HasTint)
            {
                matSource.HasTint = true;
                matSource.ColorTint = md.ColorTint;
            }

            if (const aiMaterial* aim = scene->mMaterials[aMesh->mMaterialIndex])
            {
                int twoSided = 0;
                if (AI_SUCCESS == aim->Get(AI_MATKEY_TWOSIDED, twoSided) && twoSided)
                {
                    matSource.TwoSided = true;
                }

                aiString alphaMode;
                if (AI_SUCCESS == aim->Get(AI_MATKEY_GLTF_ALPHAMODE, alphaMode))
                {
                    std::string mode = alphaMode.C_Str();
                    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                    if (mode == "blend")
                    {
                        matSource.AlphaBlend = true;
                    }
                }

                matSource.AO = MakeTextureSpecifier(GetTexPath(aim, aiTextureType_AMBIENT_OCCLUSION), baseDir, result.ModelName, scene);
                matSource.Emission = MakeTextureSpecifier(GetTexPath(aim, aiTextureType_EMISSIVE), baseDir, result.ModelName, scene);
            }

            matSource.Albedo = MakeTextureSpecifier(md.AlbedoPath, baseDir, result.ModelName, scene);
            matSource.MetallicRoughness = MakeTextureSpecifier(md.MetallicRoughnessPath, baseDir, result.ModelName, scene);
            matSource.Normal = MakeTextureSpecifier(md.NormalPath, baseDir, result.ModelName, scene);
        }

        // ---- CPU vertex arrays (final packed types for GPU upload)
        std::vector<PBRVertex>           vertices;
        std::vector<SkinnedPBRVertex>    skVertices;
        std::vector<uint32_t>            indices32;
        // CPU caches for morph blending (non-skinned meshes store base data here)
        std::vector<glm::vec3>           meshVerticesCPU; meshVerticesCPU.reserve(aMesh->mNumVertices);
        std::vector<glm::vec3>           meshNormalsCPU;  meshNormalsCPU.reserve(aMesh->mNumVertices);
        std::vector<glm::vec2>           meshUVsCPU;      meshUVsCPU.reserve(aMesh->mNumVertices);

        vertices.reserve(hasSkin ? 0 : aMesh->mNumVertices);
        skVertices.reserve(hasSkin ? aMesh->mNumVertices : 0);
        indices32.reserve(aMesh->mNumFaces * 3);

        // ---- Gather raw per-vertex base attributes
        // Also collect skinning weights/indices as top-4
        std::vector<glm::vec4>   vertWeights(aMesh->mNumVertices, glm::vec4(0.0f));
        std::vector<glm::ivec4>  vertIndices(aMesh->mNumVertices, glm::ivec4(0));

        // Pre-fill base attributes (pos/norm/uv)
        for (unsigned i = 0; i < aMesh->mNumVertices; ++i)
        {
            aiVector3D pos = aMesh->mVertices[i];
            aiVector3D norm = aMesh->mNormals ? aMesh->mNormals[i] : aiVector3D(0, 1, 0);
            aiVector3D uv = aMesh->HasTextureCoords(0) ? aMesh->mTextureCoords[0][i] : aiVector3D(0, 0, 0);

            float u = std::isfinite(uv.x) ? uv.x : 0.0f;
            float v = std::isfinite(uv.y) ? uv.y : 0.0f;

            glm::vec3 normal(norm.x, norm.y, norm.z);
            if (!IsFinite3(normal) || glm::length(normal) < 0.001f)
                normal = glm::vec3(0.0f, 1.0f, 0.0f);
            else
                normal = glm::normalize(normal);

            if (flipYThisMesh) { pos.y = -pos.y; normal.y = -normal.y; }
            if (flipZThisMesh) { pos.z = -pos.z; normal.z = -normal.z; }
            if (rotateY180ThisMesh) { pos.x = -pos.x; normal.x = -normal.x; }
            // Renormalize after flips
            normal = glm::normalize(normal);

            // Always keep a CPU copy of base data for morph blending (skinned and non-skinned)
            meshVerticesCPU.push_back(glm::vec3(pos.x, pos.y, pos.z));
            meshNormalsCPU.push_back(glm::vec3(normal.x, normal.y, normal.z));
            meshUVsCPU.push_back(glm::vec2(u, v));

            if (!hasSkin)
            {
                // PBRVertex from VertexTypes.h: (x,y,z, nx,ny,nz, u,v)
                vertices.push_back({
                    pos.x , pos.y , pos.z,
                    normal.x, normal.y, normal.z, u, v
                    });
            }
        }

        // ---- Skinning data: accumulate top-4 weights using scene-wide bone indices
        if (hasSkin)
        {
            for (unsigned b = 0; b < aMesh->mNumBones; ++b)
            {
                const aiBone* bone = aMesh->mBones[b];
                uint32_t boneIndex = boneIndexMap[bone->mName.C_Str()];

                for (unsigned w = 0; w < bone->mNumWeights; ++w)
                {
                    const aiVertexWeight& vw = bone->mWeights[w];
                    const unsigned vId = vw.mVertexId;
                    const float weight = vw.mWeight;
                    if (vId >= vertWeights.size() || weight <= 0.0f) continue;

                    // Replace smallest slot if this weight is larger
                    int slot = 0;
                    float smallest = vertWeights[vId][0];
                    for (int s = 1; s < 4; ++s)
                    {
                        if (vertWeights[vId][s] < smallest) { smallest = vertWeights[vId][s]; slot = s; }
                    }
                    if (weight > smallest)
                    {
                        vertWeights[vId][slot] = weight;
                        vertIndices[vId][slot] = (int)boneIndex;
                    }
                }
            }

            // Preserve imported skeleton indices here. If a mesh references more
            // than 8-bit bone indices, MeshBinaryLoader will compact it into a
            // local palette remap before GPU upload.
            for (size_t v = 0; v < vertWeights.size(); ++v)
            {
                for (int s = 0; s < 4; ++s)
                {
                    if (vertIndices[v][s] < 0)
                    {
                        vertWeights[v][s] = 0.0f;
                        vertIndices[v][s] = 0;
                    }
                }
                float sum = vertWeights[v].x + vertWeights[v].y + vertWeights[v].z + vertWeights[v].w;
                if (sum > 0.0001f) vertWeights[v] /= sum;
                else { vertWeights[v].x = 1.0f; vertIndices[v].x = 0; }
            }

            // Build SkinnedPBRVertex array
            skVertices.reserve(aMesh->mNumVertices);
            for (unsigned i = 0; i < aMesh->mNumVertices; ++i)
            {
                aiVector3D pos = aMesh->mVertices[i];
                aiVector3D norm = aMesh->mNormals ? aMesh->mNormals[i] : aiVector3D(0, 1, 0);
                aiVector3D uv = aMesh->HasTextureCoords(0) ? aMesh->mTextureCoords[0][i] : aiVector3D(0, 0, 0);

                float u = std::isfinite(uv.x) ? uv.x : 0.0f;
                float v = std::isfinite(uv.y) ? uv.y : 0.0f;

                glm::vec3 normal(norm.x, norm.y, norm.z);
                if (!IsFinite3(normal) || glm::length(normal) < 0.001f)
                    normal = glm::vec3(0.0f, 1.0f, 0.0f);
                else
                    normal = glm::normalize(normal);

                if (flipYThisMesh) { pos.y = -pos.y; normal.y = -normal.y; }
                if (flipZThisMesh) { pos.z = -pos.z; normal.z = -normal.z; }
                if (rotateY180ThisMesh) { pos.x = -pos.x; normal.x = -normal.x; }
                normal = glm::normalize(normal);

                const glm::ivec4& bi = vertIndices[i];
                const glm::vec4& bw = vertWeights[i];

                // SkinnedPBRVertex from VertexTypes.h:
                // (x,y,z, nx,ny,nz, u,v, i0,i1,i2,i3, w0,w1,w2,w3)
                skVertices.push_back({
                    pos.x , pos.y, pos.z,
                    normal.x, normal.y, normal.z,
                    u, v,
                    (uint8_t)bi.x, (uint8_t)bi.y, (uint8_t)bi.z, (uint8_t)bi.w,
                    bw.x, bw.y, bw.z, bw.w
                    });
            }
        }

        // ---- Indices
        for (unsigned f = 0; f < aMesh->mNumFaces; ++f)
        {
            const aiFace& face = aMesh->mFaces[f];
            if (face.mNumIndices != 3) continue;
            // Reverse winding only when the scaling changes handedness (odd number of axis flips)
            indices32.push_back((uint32_t)face.mIndices[0]);
            indices32.push_back((uint32_t)face.mIndices[1]);
            indices32.push_back((uint32_t)face.mIndices[2]);
            
        }

        // ---- Create GPU buffers (use predefined layouts from VertexTypes.h)
        const bgfx::Memory* vbMem = nullptr;
        const bgfx::VertexLayout* layoutPtr = nullptr;

        if (hasSkin)
        {
            vbMem = bgfx::copy(skVertices.data(), (uint32_t)(sizeof(SkinnedPBRVertex) * skVertices.size()));
            layoutPtr = &SkinnedPBRVertex::layout;
        }
        else
        {
            vbMem = bgfx::copy(vertices.data(), (uint32_t)(sizeof(PBRVertex) * vertices.size()));
            layoutPtr = &PBRVertex::layout;
        }

        std::shared_ptr<Mesh> mesh = std::make_shared<Mesh>();

        // Use dynamic VB only when morph targets are present; skinned meshes without morphs are static
        if (aMesh->mNumAnimMeshes > 0)
        {
            mesh->dvbh = bgfx::createDynamicVertexBuffer(vbMem, *layoutPtr);
            mesh->Dynamic = true;
        }
        else
        {
            mesh->vbh = bgfx::createVertexBuffer(vbMem, *layoutPtr);
            mesh->Dynamic = false;
        }

        // Choose 16-bit vs 32-bit indices
        uint32_t maxIndex = 0;
        for (uint32_t idx : indices32) maxIndex = std::max(maxIndex, idx);
        const bool use32 = (maxIndex >= 65536u);

        if (use32)
        {
            const bgfx::Memory* imem = bgfx::copy(indices32.data(), (uint32_t)(indices32.size() * sizeof(uint32_t)));
            mesh->ibh = bgfx::createIndexBuffer(imem, BGFX_BUFFER_INDEX32);
        }
        else
        {
            std::vector<uint16_t> idx16; idx16.reserve(indices32.size());
            for (uint32_t v : indices32) idx16.push_back((uint16_t)v);
            const bgfx::Memory* imem = bgfx::copy(idx16.data(), (uint32_t)(idx16.size() * sizeof(uint16_t)));
            mesh->ibh = bgfx::createIndexBuffer(imem);
        }

        mesh->numVertices = hasSkin ? (uint32_t)skVertices.size() : (uint32_t)vertices.size();
        mesh->numIndices = (uint32_t)indices32.size();

        // Validate handles
        bool vbValid = mesh->Dynamic ? bgfx::isValid(mesh->dvbh) : bgfx::isValid(mesh->vbh);
        if (!vbValid || !bgfx::isValid(mesh->ibh))
        {
            std::cerr << "[ModelLoader] ERROR: Failed to create GPU buffers for mesh '"
                << aMesh->mName.C_Str() << "'\n";
            continue;
        }

        // ---- CPU-side data (for picking/AABB/debug)
        mesh->Vertices.reserve(aMesh->mNumVertices);
        mesh->Normals.reserve(aMesh->mNumVertices);
        // Only assign skinning arrays when this mesh actually has skinning. Leaving
        // them empty for static meshes ensures Mesh::HasSkinning() returns false
        // and prevents skinned shader/material selection after serialization reloads.
        if (hasSkin)
        {
            mesh->BoneWeights = vertWeights;
            mesh->BoneIndices = vertIndices;
        }

        for (unsigned i = 0; i < aMesh->mNumVertices; ++i)
        {
            aiVector3D p = aMesh->mVertices[i];
            if (flipYThisMesh) p.y = -p.y;
            if (flipZThisMesh) p.z = -p.z;
            if (rotateY180ThisMesh) p.x = -p.x;
            mesh->Vertices.emplace_back(p.x , p.y , p.z );

            aiVector3D n = aMesh->mNormals ? aMesh->mNormals[i] : aiVector3D(0, 0, 1);
            if (flipYThisMesh) n.y = -n.y;
            if (flipZThisMesh) n.z = -n.z;
            if (rotateY180ThisMesh) n.x = -n.x;
            mesh->Normals.emplace_back(n.x, n.y, n.z);
        }
        mesh->Indices = indices32;

        // Sanity: indices within range
        if (!indices32.empty())
        {
            uint32_t maxIdxCpu = *std::max_element(indices32.begin(), indices32.end());
            const size_t vcount = mesh->numVertices;
            if (maxIdxCpu >= vcount)
            {
                std::cerr << "[ModelLoader] ERROR: Mesh '" << aMesh->mName.C_Str()
                    << "' has out-of-bounds index " << maxIdxCpu
                    << " (vertex count = " << vcount << ")\n";
            }
        }

        mesh->ComputeBounds();

        // ---- Blend Shapes (Morph Targets) - Sparse storage for efficiency
        BlendShapeComponent blendComp;
        if (aMesh->mNumAnimMeshes > 0)
        {
            constexpr float kSparseThreshold = 1e-6f;
            
            for (unsigned a = 0; a < aMesh->mNumAnimMeshes; ++a)
            {
                aiAnimMesh* anim = aMesh->mAnimMeshes[a];
                BlendShape bs;
                bs.Name = NormalizeBlendShapeName(anim->mName.C_Str(), aMesh->mName.C_Str());
                
                // Build sparse data directly - only store non-zero deltas
                bs.IsSparse = true;
                bs.SparseIndices.reserve(aMesh->mNumVertices / 4); // Estimate ~25% affected
                bs.SparseDeltaPos.reserve(aMesh->mNumVertices / 4);
                bs.SparseDeltaNorm.reserve(aMesh->mNumVertices / 4);

                for (unsigned v = 0; v < aMesh->mNumVertices; ++v)
                {
                    // Assimp stores target positions; convert to deltas relative to base mesh
                    const glm::vec3 baseP = (v < meshVerticesCPU.size()) ? meshVerticesCPU[v] : glm::vec3(0);
                    aiVector3D ap = anim->mVertices[v];
                    if (flipYThisMesh) ap.y = -ap.y; // match axis flip applied to base
                    if (flipZThisMesh) ap.z = -ap.z;
                    if (rotateY180ThisMesh) ap.x = -ap.x;
                    const glm::vec3 tgtP(ap.x, ap.y, ap.z);
                    const glm::vec3 deltaP = tgtP - baseP;

                    const glm::vec3 baseN = (v < meshNormalsCPU.size()) ? meshNormalsCPU[v] : glm::vec3(0, 1, 0);
                    aiVector3D an = anim->mNormals ? anim->mNormals[v] : aiVector3D(0, 0, 0);
                    if (flipYThisMesh) an.y = -an.y;
                    if (flipZThisMesh) an.z = -an.z;
                    if (rotateY180ThisMesh) an.x = -an.x;
                    const glm::vec3 tgtN(an.x, an.y, an.z);
                    const glm::vec3 deltaN = tgtN - baseN;
                    
                    // Only store if delta is significant (sparse storage)
                    const float lenSq = glm::dot(deltaP, deltaP) + glm::dot(deltaN, deltaN);
                    if (lenSq > kSparseThreshold * kSparseThreshold) {
                        bs.SparseIndices.push_back(v);
                        bs.SparseDeltaPos.push_back(deltaP);
                        bs.SparseDeltaNorm.push_back(deltaN);
                    }
                }
                
                // Shrink to fit
                bs.SparseIndices.shrink_to_fit();
                bs.SparseDeltaPos.shrink_to_fit();
                bs.SparseDeltaNorm.shrink_to_fit();
                
                blendComp.Shapes.push_back(std::move(bs));
            }
        }

        // Fill CPU caches for morph blending (both skinned and non-skinned)
        mesh->Vertices = std::move(meshVerticesCPU);
        mesh->Normals  = std::move(meshNormalsCPU);
        mesh->UVs      = std::move(meshUVsCPU);

        // ---- Append to result
        result.Meshes.push_back(mesh);
        result.Materials.push_back(matSource);
        result.MaterialSlotNames.push_back(matSource.Name);
        result.BlendShapes.push_back(std::move(blendComp));

        // Debug (optional):
        std::cout << "[ModelLoader] Mesh '" << aMesh->mName.C_Str()
            << "' verts=" << mesh->numVertices
            << " indices=" << mesh->numIndices
            << " faces=" << aMesh->mNumFaces
            << " skinned=" << (hasSkin ? "yes" : "no")
            << " animMeshes=" << aMesh->mNumAnimMeshes
            << "\n";
    }

    return result;
}
