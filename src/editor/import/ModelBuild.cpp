#include "ModelBuild.h"
#include "editor/pipeline/AssetLibrary.h"
#include "core/rendering/MaterialManager.h"
#include "core/rendering/SkinnedPBRMaterial.h"
#include "core/animation/SkeletonBinding.h"
#include <iostream>
#include <algorithm>

static inline bool IsZeroGuid(const ClaymoreGUID& g) { return g.high == 0 && g.low == 0; }

// Parse entity name suffix keywords like "_a" (alpha) and "_bf" (backfaces).
// Order-independent; scans all '_' tokens.
static inline void ParseNameSuffixHints(const std::string& name, bool& outAlphaBlend, bool& outShowBackfaces)
{
    outAlphaBlend = false; outShowBackfaces = false;
    if (name.empty()) return;
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    size_t start = 0;
    while (start <= lower.size()) {
        size_t us = lower.find('_', start);
        size_t end = (us == std::string::npos) ? lower.size() : us;
        if (end > start) {
            std::string tok = lower.substr(start, end - start);
            bool numeric = !tok.empty() && std::all_of(tok.begin(), tok.end(), [](unsigned char c){ return std::isdigit(c) != 0; });
            if (!numeric) {
                if (tok == "a") outAlphaBlend = true;
                else if (tok == "bf") outShowBackfaces = true;
            }
        }
        if (us == std::string::npos) break;
        start = us + 1;
    }
}

BuildResult BuildRendererFromAssets(const BuildModelParams& p)
{
    BuildResult result{};
    if (!p.scene || p.entity == (EntityID)-1) {
        std::cerr << "[ModelBuild] ERROR: Invalid scene/entity for build." << std::endl;
        return result;
    }

    EntityData* data = p.scene->GetEntityData(p.entity);
    if (!data) {
        std::cerr << "[ModelBuild] ERROR: Entity not found: " << p.entity << std::endl;
        return result;
    }

    // Ensure MeshComponent exists and mesh is loaded
    if (!data->Mesh) data->Mesh = std::make_unique<MeshComponent>();

    // Load mesh by AssetReference if not already present
    if (!data->Mesh->mesh) {
        if (IsZeroGuid(p.meshGuid)) {
            std::cerr << "[ModelBuild] ERROR: meshGuid is missing." << std::endl;
            return result;
        }
        AssetReference meshRef(p.meshGuid, p.meshFileId, (int)AssetType::Mesh);
        data->Mesh->meshReference = meshRef;
        data->Mesh->mesh = AssetLibrary::Instance().LoadMesh(meshRef);
        if (!data->Mesh->mesh) {
            std::cerr << "[ModelBuild] ERROR: Failed to load mesh for GUID " << p.meshGuid.ToString() << " fileID=" << p.meshFileId << std::endl;
            return result;
        }
    }

    Mesh* meshPtr = data->Mesh->mesh.get();
    const bool meshIsSkinned = meshPtr && meshPtr->HasSkinning();
    result.isSkinned = meshIsSkinned;

    // Classify and enforce contract: if skinned, we require a skeleton in hierarchy
    if (meshIsSkinned) {
        // Find nearest ancestor SkeletonComponent
        EntityID cur = data->Parent;
        EntityID foundSkeletonRoot = (EntityID)-1;
        SkeletonComponent* foundSkel = nullptr;
        size_t guard = 0;
        while (cur != (EntityID)-1 && guard++ < 200000) {
            EntityData* pd = p.scene->GetEntityData(cur);
            if (!pd) break;
            if (pd->Skeleton) { foundSkeletonRoot = cur; foundSkel = pd->Skeleton.get(); break; }
            cur = pd->Parent;
        }

        if (!foundSkel) {
            std::cerr << "[ModelBuild] ERROR: Skinned mesh requires a skeleton ancestor; none found for entity '" << data->Name << "'." << std::endl;
            return result;
        }
        // If a skeletonGuid was provided, validate it
        if (!IsZeroGuid(p.skeletonGuid)) {
            if (foundSkel->SkeletonGuid.high != p.skeletonGuid.high || foundSkel->SkeletonGuid.low != p.skeletonGuid.low) {
                std::cerr << "[ModelBuild] ERROR: Skinned mesh skeleton GUID mismatch for entity '" << data->Name << "'." << std::endl;
                return result;
            }
        }

        // Ensure SkinningComponent exists and is bound to this skeleton root
        if (!data->Skinning) data->Skinning = std::make_unique<SkinningComponent>();
        data->Skinning->SkeletonRoot = foundSkeletonRoot;
        
        // Capture original bone names and inverse bind poses for skeleton retargeting
        // This allows the mesh to be re-parented under a different skeleton with
        // matching bone names and have bone indices remapped automatically.
        data->Skinning->OriginalBoneNames = foundSkel->BoneNames;
        data->Skinning->OriginalInverseBindPoses = foundSkel->InverseBindPoses;

        // Ensure skinned PBR material is used
        if (!std::dynamic_pointer_cast<SkinnedPBRMaterial>(data->Mesh->material)) {
            data->Mesh->material = MaterialManager::Instance().CreateSceneSkinnedDefaultMaterial(&Scene::Get());
        }

        // Build remap and used-joint list using current mesh and skeleton
        std::vector<uint16_t> remap, used;
        if (!BuildBoneRemap(*meshPtr, *foundSkel, remap, used)) {
            // Mesh had no skinning or mismatch; treat as error to avoid silent downgrade
            std::cerr << "[ModelBuild] ERROR: Failed to build bone remap for skinned mesh on entity '" << data->Name << "'." << std::endl;
            return result;
        }
        result.usedJointList = used;
        result.remap = remap;

        // Apply name-based material hints on instantiation ("_a" alpha, "_bf" backfaces)
        try {
            bool wantBlend = false, wantBackfaces = false;
            ParseNameSuffixHints(data->Name, wantBlend, wantBackfaces);
            if (auto mat = data->Mesh->material) {
                if (wantBlend) {
                    uint64_t f = mat->GetStateFlags();
                    f |= BGFX_STATE_BLEND_ALPHA;
                    mat->m_StateFlags = f;
                    if (!data->RenderOverrides) data->RenderOverrides = std::make_unique<RenderOverridesComponent>();
                    data->RenderOverrides->AlphaBlendEnabled = true;
                }
                if (wantBackfaces) {
                    uint64_t f = mat->GetStateFlags();
                    f &= ~(BGFX_STATE_CULL_CW | BGFX_STATE_CULL_CCW);
                    mat->m_StateFlags = f;
                    data->Mesh->ShowBackfaces = true; // persist via serializer
                }
            }
        } catch(...) {}

        result.ok = true;
        return result;
    }

    // Static path: ensure we have a default PBR material if none
    if (!data->Mesh->material) {
        data->Mesh->material = MaterialManager::Instance().CreateSceneDefaultMaterial(&Scene::Get());
    }

    // Ensure no stale SkinningComponent remains on static meshes
    if (data->Skinning) {
        data->Skinning.reset();
    }

    // Static path name-based hints
    try {
        bool wantBlend = false, wantBackfaces = false;
        ParseNameSuffixHints(data->Name, wantBlend, wantBackfaces);
        if (auto mat = data->Mesh->material) {
            if (wantBlend) {
                uint64_t f = mat->GetStateFlags();
                f |= BGFX_STATE_BLEND_ALPHA;
                mat->m_StateFlags = f;
                if (!data->RenderOverrides) data->RenderOverrides = std::make_unique<RenderOverridesComponent>();
                data->RenderOverrides->AlphaBlendEnabled = true;
            }
            if (wantBackfaces) {
                uint64_t f = mat->GetStateFlags();
                f &= ~(BGFX_STATE_CULL_CW | BGFX_STATE_CULL_CCW);
                mat->m_StateFlags = f;
                data->Mesh->ShowBackfaces = true;
            }
        }
    } catch(...) {}

    result.ok = true;
    return result;
}


