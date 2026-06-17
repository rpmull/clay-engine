#include "RuntimeModelInstantiator.h"
#include "RuntimeModelManifest.h"
#include "ModelRegistry.h"
#include "core/assets/IAssetResolver.h"
#include "core/assets/BinaryFormats.h"
#include "core/ecs/Scene.h"
#include "core/ecs/EntityData.h"
#include "core/serialization/MeshBinaryLoader.h"
#include "core/serialization/SkelBinLoader.h"
#include "core/ecs/AnimationComponents.h"
#include "core/animation/AnimationPlayerComponent.h"
#include "core/rendering/MaterialCache.h"
#include "core/rendering/PBRMaterial.h"
#include "core/rendering/SkinnedPBRMaterial.h"
#include "core/rendering/TextureLoader.h"
#include "core/rendering/ShaderManager.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <cmath>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

namespace cm {

// Forward declaration for BuildUnifiedMorphComponent
static void BuildUnifiedMorphComponent(Scene& scene, EntityID targetRoot, const std::vector<EntityID>& meshEntities);

// Helper to compute bind pose globals from inverse bind poses
static void ComputeBindPoseGlobals(SkeletonComponent& skeleton) {
    skeleton.BindPoseGlobals.resize(skeleton.InverseBindPoses.size());
    for (size_t i = 0; i < skeleton.InverseBindPoses.size(); ++i) {
        skeleton.BindPoseGlobals[i] = glm::inverse(skeleton.InverseBindPoses[i]);
    }
}

// Build unified morph component for blendshapes (matches editor behavior)
static void BuildUnifiedMorphComponent(Scene& scene, EntityID targetRoot, const std::vector<EntityID>& meshEntities) {
    std::unordered_map<std::string, int> nameCounts;
    for (EntityID meshID : meshEntities) {
        auto* data = scene.GetEntityData(meshID);
        if (!data || !data->BlendShapes || data->BlendShapes->Shapes.empty()) continue;
        for (const auto& shape : data->BlendShapes->Shapes) {
            nameCounts[shape.Name]++;
        }
    }
    
    std::vector<std::string> unifiedNames;
    for (const auto& kv : nameCounts) {
        if (kv.second >= 1) {
            unifiedNames.push_back(kv.first);
        }
    }
    if (unifiedNames.empty()) return;
    
    auto* targetData = scene.GetEntityData(targetRoot);
    if (!targetData) return;
    
    targetData->UnifiedMorph = std::make_unique<UnifiedMorphComponent>();
    targetData->UnifiedMorph->Names = unifiedNames;
    targetData->UnifiedMorph->Weights.assign(unifiedNames.size(), 0.0f);
    targetData->UnifiedMorph->NameIndexDirty = true;
    targetData->UnifiedMorph->NameIndex.clear();
    targetData->UnifiedMorph->MemberMeshes.clear();
    for (EntityID meshID : meshEntities) {
        if (meshID != INVALID_ENTITY_ID) {
            targetData->UnifiedMorph->MemberMeshes.push_back(meshID);
        }
    }
}

static bool IsDefaultTransform(const TransformComponent& t) {
    const float eps = 0.0001f;
    auto nearEq = [&](float a, float b) { return std::fabs(a - b) <= eps; };
    return nearEq(t.Position.x, 0.0f) && nearEq(t.Position.y, 0.0f) && nearEq(t.Position.z, 0.0f) &&
           nearEq(t.Scale.x, 1.0f) && nearEq(t.Scale.y, 1.0f) && nearEq(t.Scale.z, 1.0f) &&
           nearEq(t.RotationQ.x, 0.0f) && nearEq(t.RotationQ.y, 0.0f) && nearEq(t.RotationQ.z, 0.0f) &&
           nearEq(t.RotationQ.w, 1.0f);
}

namespace {
std::mutex s_runtimeMaterialCacheMutex;
std::unordered_map<std::string, std::weak_ptr<Material>> s_runtimeMaterialCache;

uint32_t RuntimeMaterialFloatBits(float value)
{
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float size mismatch");
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::string BuildRuntimeMaterialCacheKey(const RuntimeMaterialSlot& slot, bool skinned)
{
    std::ostringstream key;
    key << (skinned ? '1' : '0')
        << '|' << slot.albedoGuid.high << ':' << slot.albedoGuid.low
        << '|' << slot.normalGuid.high << ':' << slot.normalGuid.low
        << '|' << slot.metallicRoughnessGuid.high << ':' << slot.metallicRoughnessGuid.low
        << '|' << slot.aoGuid.high << ':' << slot.aoGuid.low
        << '|' << slot.emissionGuid.high << ':' << slot.emissionGuid.low
        << '|' << slot.displacementGuid.high << ':' << slot.displacementGuid.low
        << '|' << RuntimeMaterialFloatBits(slot.metallic)
        << '|' << RuntimeMaterialFloatBits(slot.roughness)
        << '|' << RuntimeMaterialFloatBits(slot.normalScale)
        << '|' << RuntimeMaterialFloatBits(slot.aoStrength)
        << '|' << RuntimeMaterialFloatBits(slot.tint.x)
        << '|' << RuntimeMaterialFloatBits(slot.tint.y)
        << '|' << RuntimeMaterialFloatBits(slot.tint.z)
        << '|' << RuntimeMaterialFloatBits(slot.tint.w)
        << '|' << (slot.alphaBlend ? '1' : '0')
        << '|' << (slot.alphaCutout ? '1' : '0')
        << '|' << (slot.twoSided ? '1' : '0')
        << '|' << (slot.hasTint ? '1' : '0')
        << '|' << RuntimeMaterialFloatBits(slot.alphaCutoutThreshold);
    return key.str();
}

bool IsEquivalentMaterialShared(const std::shared_ptr<Material>& material)
{
    return material &&
        GetMaterialEquivalenceKey(material.get()).EquivalentSafe;
}

std::shared_ptr<Material> CreateSharedRuntimeMaterial(
    Scene& scene,
    const RuntimeMaterialSlot& slot,
    IAssetResolver* resolver,
    bool skinned)
{
    const std::string cacheKey = BuildRuntimeMaterialCacheKey(slot, skinned);
    {
        std::lock_guard<std::mutex> lock(s_runtimeMaterialCacheMutex);
        auto it = s_runtimeMaterialCache.find(cacheKey);
        if (it != s_runtimeMaterialCache.end()) {
            if (auto existing = it->second.lock()) {
                return existing;
            }
            s_runtimeMaterialCache.erase(it);
        }
    }

    const std::string shaderVS = skinned ? "vs_pbr_skinned" : "vs_pbr";
    const std::string shaderFS = skinned ? "fs_pbr_skinned" : "fs_pbr";
    const bgfx::ProgramHandle program =
        ShaderManager::Instance().LoadProgram(shaderVS, shaderFS);
    if (!bgfx::isValid(program)) {
        std::cerr << "[RuntimeModelInstantiator] Failed to load PBR shader program" << std::endl;
        return nullptr;
    }

    std::shared_ptr<PBRMaterial> pbrMaterial =
        skinned
            ? std::static_pointer_cast<PBRMaterial>(
                std::make_shared<SkinnedPBRMaterial>("RuntimeMaterial", program))
            : std::make_shared<PBRMaterial>("RuntimeMaterial", program);
    if (!pbrMaterial) {
        return nullptr;
    }

    pbrMaterial->SetMetallic(slot.metallic);
    pbrMaterial->SetRoughness(slot.roughness);
    pbrMaterial->SetNormalScale(slot.normalScale);
    pbrMaterial->SetAmbientOcclusion(slot.aoStrength);

    if (slot.tint != glm::vec4(1.0f)) {
        pbrMaterial->SetUniform("u_ColorTint", slot.tint);
    }

    auto setTextureFromGuid = [&](const ClaymoreGUID& guid, auto&& setter) {
        if (!resolver || (guid.high == 0 && guid.low == 0)) {
            return;
        }
        const std::string path = resolver->GetPathForGUID(guid);
        if (!path.empty()) {
            setter(path);
        }
    };

    setTextureFromGuid(slot.albedoGuid, [&](const std::string& path) {
        pbrMaterial->SetAlbedoTextureFromPath(path);
    });
    setTextureFromGuid(slot.normalGuid, [&](const std::string& path) {
        pbrMaterial->SetNormalTextureFromPath(path);
    });
    setTextureFromGuid(slot.metallicRoughnessGuid, [&](const std::string& path) {
        pbrMaterial->SetMetallicRoughnessTextureFromPath(path);
    });
    setTextureFromGuid(slot.aoGuid, [&](const std::string& path) {
        pbrMaterial->SetAmbientOcclusionTextureFromPath(path);
    });
    setTextureFromGuid(slot.emissionGuid, [&](const std::string& path) {
        pbrMaterial->SetEmissionTextureFromPath(path);
    });
    setTextureFromGuid(slot.displacementGuid, [&](const std::string& path) {
        pbrMaterial->SetDisplacementTextureFromPath(path);
    });

    if (slot.alphaBlend && !slot.alphaCutout) {
        pbrMaterial->m_StateFlags =
            pbrMaterial->GetStateFlags() | BGFX_STATE_BLEND_ALPHA;
    }

    if (slot.twoSided) {
        pbrMaterial->m_StateFlags =
            pbrMaterial->GetStateFlags() &
            ~(BGFX_STATE_CULL_CW | BGFX_STATE_CULL_CCW);
    }

    if (slot.alphaCutout) {
        pbrMaterial->SetUniform(
            "u_PBRScalar1",
            glm::vec4(
                pbrMaterial->GetEmissionStrength(),
                slot.alphaCutoutThreshold,
                0.0f,
                0.0f));
    }

    std::shared_ptr<Material> shared = AcquireEquivalentMaterial(pbrMaterial);
    {
        std::lock_guard<std::mutex> lock(s_runtimeMaterialCacheMutex);
        s_runtimeMaterialCache[cacheKey] = shared;
    }
    return shared;
}
} // namespace

EntityID RuntimeModelInstantiator::Instantiate(const std::string& manifestPath, Scene& scene, 
                                                const glm::vec3& position,
                                                EntityID existingRoot) {
    // Legacy: Load from individual .modelrt file
    RuntimeModelManifest manifest;
    if (!RuntimeModelManifestLoader::Load(manifestPath, manifest)) {
        std::cerr << "[RuntimeModelInstantiator] Failed to load manifest: " << manifestPath << std::endl;
        return -1;
    }
    return InstantiateFromManifest(manifest, scene, position, existingRoot);
}

EntityID RuntimeModelInstantiator::InstantiateByGuid(const ClaymoreGUID& modelGuid, Scene& scene,
                                                      const glm::vec3& position,
                                                      EntityID existingRoot) {
    // First try centralized registry (production path)
    const RuntimeModelManifest* manifest = ModelRegistry::Instance().GetManifest(modelGuid);
    if (manifest) {
        return InstantiateFromManifest(*manifest, scene, position, existingRoot);
    }
    
    // Fallback: Try to load individual .modelrt file (legacy/debug)
    IAssetResolver* resolver = Assets::GetResolver();
    if (!resolver) {
        std::cerr << "[RuntimeModelInstantiator] No asset resolver and model not in registry: " 
                  << modelGuid.ToString() << std::endl;
        return -1;
    }
    
    std::string modelPath = resolver->GetPathForGUID(modelGuid);
    if (modelPath.empty()) {
        std::cerr << "[RuntimeModelInstantiator] Failed to resolve GUID: " << modelGuid.ToString() << std::endl;
        return -1;
    }
    
    // Convert model path to manifest path
    std::string manifestPath = modelPath;
    auto dotPos = manifestPath.rfind('.');
    if (dotPos != std::string::npos) {
        manifestPath = manifestPath.substr(0, dotPos);
    }
    manifestPath += ".modelrt";
    
    return Instantiate(manifestPath, scene, position, existingRoot);
}

EntityID RuntimeModelInstantiator::InstantiateFromManifest(const RuntimeModelManifest& manifest, Scene& scene,
                                                            const glm::vec3& position,
                                                            EntityID existingRoot) {
    if (!manifest.isValid()) {
        std::cerr << "[RuntimeModelInstantiator] Invalid manifest" << std::endl;
        return -1;
    }
    return CreateHierarchy(manifest, scene, position, existingRoot);
}

EntityID RuntimeModelInstantiator::CreateHierarchy(const RuntimeModelManifest& manifest, Scene& scene,
                                                    const glm::vec3& position,
                                                    EntityID existingRoot) {
    IAssetResolver* resolver = Assets::GetResolver();
    if (!resolver) {
        std::cerr << "[RuntimeModelInstantiator] No asset resolver" << std::endl;
        return -1;
    }
    
    // Resolve meshbin path
    std::string meshbinPath;
    if (manifest.meshbinGuid.high != 0 || manifest.meshbinGuid.low != 0) {
        meshbinPath = resolver->GetPathForGUID(manifest.meshbinGuid);
    }
    if (meshbinPath.empty()) {
        std::cerr << "[RuntimeModelInstantiator] Failed to resolve meshbin GUID" << std::endl;
        return -1;
    }
    
    // Load skeleton from .skelbin if present
    std::unique_ptr<SkeletonComponent> skeletonData;
    if (manifest.hasSkeleton()) {
        std::string skelPath = resolver->GetPathForGUID(manifest.skeletonGuid);
        if (!skelPath.empty()) {
            skeletonData = cm::SkelBinLoader::Load(skelPath);
            if (!skeletonData) {
                std::cerr << "[RuntimeModelInstantiator] Failed to load skeleton: " << skelPath << std::endl;
            }
        }
    }
    
    // Create or reuse root entity
    std::string rootName = "Model";
    if (!manifest.nodes.empty()) {
        rootName = manifest.nodes[0].name;
    }
    
    EntityID rootId = existingRoot;
    EntityData* rootData = nullptr;
    bool reuseRoot = (existingRoot != INVALID_ENTITY_ID);
    if (reuseRoot) {
        rootData = scene.GetEntityData(rootId);
        if (!rootData) {
            reuseRoot = false;
        }
    }
    if (!reuseRoot) {
        Entity rootEntity = scene.CreateEntity(rootName);
        rootId = rootEntity.GetID();
        rootData = scene.GetEntityData(rootId);
    }
    if (!rootData) {
        std::cerr << "[RuntimeModelInstantiator] Failed to create root entity" << std::endl;
        return -1;
    }
    if (reuseRoot) {
        scene.SetEntityName(rootId, rootName);
    }
    
    bool preserveTransform = reuseRoot && !IsDefaultTransform(rootData->Transform);
    if (!preserveTransform) {
        // Set root transform
        rootData->Transform.Position = position + manifest.rootPosition;
        rootData->Transform.RotationQ = manifest.rootRotation;
        rootData->Transform.UseQuatRotation = true;
        rootData->Transform.Scale = manifest.rootScale;
        scene.MarkTransformDirty(rootId);
    }
    rootData->ModelAssetGuid = manifest.modelGuid;
    
    // Track created entities
    std::vector<EntityID> nodeEntityIds;
    nodeEntityIds.push_back(rootId);
    
    // Track mesh entities for UnifiedMorph setup
    std::vector<EntityID> meshEntities;
    
    // =========================================================================
    // Create skeleton hierarchy (matches editor's BuildSkeletonEntities)
    // =========================================================================
    EntityID skeletonRootId = INVALID_ENTITY_ID;
    if (skeletonData && !skeletonData->BoneNames.empty()) {
        // Create SkeletonRoot entity
        Entity skeletonRootEnt = scene.CreateEntity("SkeletonRoot");
        skeletonRootId = skeletonRootEnt.GetID();
        scene.SetParent(skeletonRootId, rootId);
        
        auto* skelRootData = scene.GetEntityData(skeletonRootId);
        if (skelRootData) {
            // Transfer skeleton component to skeleton root
            skelRootData->Skeleton = std::move(skeletonData);
            SkeletonComponent& skeleton = *skelRootData->Skeleton;
            
            // Compute BindPoseGlobals (critical for skinning)
            ComputeBindPoseGlobals(skeleton);
            
            // Build BoneNameToIndex map
            skeleton.BoneNameToIndex.clear();
            for (size_t i = 0; i < skeleton.BoneNames.size(); ++i) {
                skeleton.BoneNameToIndex[skeleton.BoneNames[i]] = static_cast<int>(i);
            }
            
            // Set skeleton GUID
            skeleton.SkeletonGuid = manifest.modelGuid;
            
            // Create bone entities.
            // PERF: see Scene.cpp bone-creation note. Bones are index-addressed
            // internal children; CreateEntity's O(N) unique-name scan and per-entity
            // dirty/hierarchy invalidation make spawning a skeleton O(bones x
            // sceneEntities). Use the batched fast path, invalidate once after.
            skeleton.BoneEntities.resize(skeleton.BoneNames.size(), INVALID_ENTITY_ID);
            for (size_t i = 0; i < skeleton.BoneNames.size(); ++i) {
                Entity boneEnt = scene.CreateEntityExactFast(skeleton.BoneNames[i]);
                skeleton.BoneEntities[i] = boneEnt.GetID();
            }
            scene.MarkDirty();
            scene.InvalidateHierarchyCache();
            
            // Set up bone parent hierarchy
            for (size_t i = 0; i < skeleton.BoneEntities.size(); ++i) {
                EntityID boneID = skeleton.BoneEntities[i];
                int parentIndex = (i < skeleton.BoneParents.size()) ? skeleton.BoneParents[i] : -1;
                EntityID parentEntity = (parentIndex >= 0 && parentIndex < (int)skeleton.BoneEntities.size())
                    ? skeleton.BoneEntities[parentIndex] : skeletonRootId;
                scene.SetParent(boneID, parentEntity);
                
                // Compute and apply local transform from bind pose
                auto* boneData = scene.GetEntityData(boneID);
                if (boneData) {
                    glm::mat4 thisGlobal = skeleton.BindPoseGlobals[i];
                    glm::mat4 parentGlobal = (parentIndex >= 0) ? skeleton.BindPoseGlobals[parentIndex] : glm::mat4(1.0f);
                    glm::mat4 localBind = glm::inverse(parentGlobal) * thisGlobal;
                    
                    // Decompose local matrix to transform components
                    glm::vec3 scale, translation, skew;
                    glm::vec4 perspective;
                    glm::quat rotation;
                    glm::decompose(localBind, scale, rotation, translation, skew, perspective);
                    
                    boneData->Transform.Position = translation;
                    boneData->Transform.RotationQ = rotation;
                    boneData->Transform.UseQuatRotation = true;
                    boneData->Transform.Scale = scale;
                    scene.MarkTransformDirty(boneID);
                }
            }

            // Stage 3 foundation: stamp the per-bone back-reference marker so these
            // bones resolve their world matrix from the skeleton pose palette (parity
            // with the Scene.cpp instantiation path; previously only that path set it).
            scene.RebindSkeletonBoneMarkers(skeletonRootId);

            // Add AnimationPlayer to skeleton root
            skelRootData->AnimationPlayer = std::make_unique<cm::animation::AnimationPlayerComponent>();
        }
    }
    
    // =========================================================================
    // Create mesh entities
    // =========================================================================
    for (size_t i = 0; i < manifest.nodes.size(); ++i) {
        const auto& node = manifest.nodes[i];
        
        // Skip root (already created) unless it has a mesh (single-mesh model)
        if (i == 0) {
            // For single-mesh models, mesh goes on root
            if (manifest.nodes.size() == 1 && node.meshFileId >= 0) {
                bool meshSkinned = false;
                auto mesh = MeshBinaryLoader::LoadMesh(meshbinPath, node.meshFileId, &meshSkinned);
                if (mesh) {
                    rootData->Mesh = std::make_unique<MeshComponent>(mesh, node.name, nullptr);
                    rootData->Mesh->meshReference = AssetReference(manifest.modelGuid, node.meshFileId, 3);
                    
                    auto blendShapes = MeshBinaryLoader::LoadBlendShapes(meshbinPath, node.meshFileId);
                    if (blendShapes) {
                        rootData->BlendShapes = std::move(blendShapes);
                    }
                    
                    // Apply materials (use node.skinned as authoritative, mesh might not know)
                    ApplyMaterials(scene, rootData, node.materials, resolver, node.skinned || meshSkinned);
                    
                    meshEntities.push_back(rootId);
                }
            }
            continue;
        }
        
        // Create entity for this node
        Entity childEntity = scene.CreateEntity(node.name);
        EntityID childId = childEntity.GetID();
        EntityData* childData = scene.GetEntityData(childId);
        if (!childData) continue;
        
        // Set transform
        childData->Transform.Position = node.position;
        childData->Transform.RotationQ = node.rotation;
        childData->Transform.UseQuatRotation = true;
        childData->Transform.Scale = node.scale;
        scene.MarkTransformDirty(childId);
        
        // Set parent
        int32_t parentIdx = node.parentIndex;
        if (parentIdx >= 0 && parentIdx < static_cast<int32_t>(nodeEntityIds.size())) {
            scene.SetParent(childId, nodeEntityIds[parentIdx]);
        } else {
            // Default: parent to skeleton root if skinned, otherwise to root
            scene.SetParent(childId, (skeletonRootId != INVALID_ENTITY_ID && node.skinned) 
                                      ? skeletonRootId : rootId);
        }
        
        nodeEntityIds.push_back(childId);
        
        // Load mesh if this node has one
        if (node.meshFileId >= 0) {
            bool meshSkinned = false;
            auto mesh = MeshBinaryLoader::LoadMesh(meshbinPath, node.meshFileId, &meshSkinned);
            if (mesh) {
                childData->Mesh = std::make_unique<MeshComponent>(mesh, node.name, nullptr);
                childData->Mesh->meshReference = AssetReference(manifest.modelGuid, node.meshFileId, 3);
                
                // Determine if this mesh uses skinning
                bool useSkinning = node.skinned || meshSkinned;
                
                // Set up skinning
                if (useSkinning && skeletonRootId != INVALID_ENTITY_ID) {
                    childData->Skinning = std::make_unique<SkinningComponent>();
                    childData->Skinning->UseParentSkeleton = true;
                    childData->Skinning->SkeletonRoot = skeletonRootId;
                    
                    // Copy original bone names/IBPs from skeleton for skinning
                    auto* skelData = scene.GetEntityData(skeletonRootId);
                    if (skelData && skelData->Skeleton) {
                        childData->Skinning->OriginalBoneNames = skelData->Skeleton->BoneNames;
                        childData->Skinning->OriginalInverseBindPoses = skelData->Skeleton->InverseBindPoses;
                    }
                }
                
                // Load blend shapes
                auto blendShapes = MeshBinaryLoader::LoadBlendShapes(meshbinPath, node.meshFileId);
                if (blendShapes) {
                    childData->BlendShapes = std::move(blendShapes);
                }
                
                // Apply materials (create actual Material objects, not just InlineMaterials)
                ApplyMaterials(scene, childData, node.materials, resolver, useSkinning);
                
                meshEntities.push_back(childId);
            }
        }
    }
    
    // =========================================================================
    // Create MeshProxy entities (for submesh-based rendering)
    // =========================================================================
    for (const auto& proxy : manifest.proxies) {
        Entity proxyEntity = scene.CreateEntity(proxy.displayName.empty() ? proxy.name : proxy.displayName);
        EntityID proxyId = proxyEntity.GetID();
        EntityData* proxyData = scene.GetEntityData(proxyId);
        if (!proxyData) continue;
        
        // Set transform
        proxyData->Transform.Position = proxy.position;
        proxyData->Transform.RotationQ = proxy.rotation;
        proxyData->Transform.UseQuatRotation = true;
        proxyData->Transform.Scale = proxy.scale;
        scene.MarkTransformDirty(proxyId);
        
        // Parent to model root (or skeleton root for skinned proxies)
        scene.SetParent(proxyId, (skeletonRootId != INVALID_ENTITY_ID && proxy.skinned) 
                                  ? skeletonRootId : rootId);
        
        // Create MeshProxyComponent
        proxyData->MeshProxy = std::make_unique<MeshProxyComponent>();
        
        // Resolve target mesh entity
        int32_t targetNodeIndex = proxy.meshEntryIndex;
        if (targetNodeIndex >= 0 && targetNodeIndex < static_cast<int32_t>(nodeEntityIds.size())) {
            proxyData->MeshProxy->TargetMesh = nodeEntityIds[targetNodeIndex];
        }
        
        // Copy submesh slots
        proxyData->MeshProxy->SubmeshSlots = proxy.submeshSlots;
        
        // Build slot index lookup
        proxyData->MeshProxy->SlotIndexLookup.clear();
        for (size_t i = 0; i < proxy.submeshSlots.size(); ++i) {
            proxyData->MeshProxy->SlotIndexLookup[proxy.submeshSlots[i]] = i;
        }
        
        // Set up skinning for skinned proxies
        if (proxy.skinned && skeletonRootId != INVALID_ENTITY_ID) {
            proxyData->Skinning = std::make_unique<SkinningComponent>();
            proxyData->Skinning->UseParentSkeleton = true;
            proxyData->Skinning->SkeletonRoot = skeletonRootId;
            
            auto* skelData = scene.GetEntityData(skeletonRootId);
            if (skelData && skelData->Skeleton) {
                proxyData->Skinning->OriginalBoneNames = skelData->Skeleton->BoneNames;
                proxyData->Skinning->OriginalInverseBindPoses = skelData->Skeleton->InverseBindPoses;
            }
        }
    }
    
    // Build UnifiedMorph component for blendshapes (matches editor behavior)
    EntityID morphRoot = (skeletonRootId != INVALID_ENTITY_ID) ? skeletonRootId : rootId;
    BuildUnifiedMorphComponent(scene, morphRoot, meshEntities);
    
    std::cout << "[RuntimeModelInstantiator] Created model with " << nodeEntityIds.size() 
              << " entities, " << meshEntities.size() << " meshes, " 
              << manifest.proxies.size() << " proxies from " 
              << manifest.modelGuid.ToString() << std::endl;
    
    return rootId;
}

// Helper to apply materials from manifest to entity - creates actual Material objects
void RuntimeModelInstantiator::ApplyMaterials(Scene& scene,
                                               EntityData* data, 
                                               const std::vector<RuntimeMaterialSlot>& materials,
                                               IAssetResolver* resolver,
                                               bool skinned) {
    if (!data || !data->Mesh || materials.empty()) return;
    bool allSlotsTwoSided = true;

    data->Mesh->materials.resize(materials.size());
    data->Mesh->MaterialSlotNames.assign(materials.size(), std::string());
    data->Mesh->MaterialAssetPaths.assign(materials.size(), std::string());
    data->Mesh->OwnedMaterialSlots.assign(materials.size(), false);
    data->Mesh->MaterialSources.assign(materials.size(), MaterialSource{});
    data->Mesh->SlotPropertyBlocks.assign(materials.size(), MaterialPropertyBlock{});
    data->Mesh->SlotPropertyBlockTexturePaths.assign(materials.size(), {});
    data->Mesh->material.reset();
    data->Mesh->ShowBackfaces = false;

    for (size_t i = 0; i < materials.size(); ++i) {
        const auto& mat = materials[i];

        std::shared_ptr<Material> sharedMaterial =
            CreateSharedRuntimeMaterial(scene, mat, resolver, skinned);
        data->Mesh->materials[i] = sharedMaterial;
        data->Mesh->MaterialSlotNames[i] =
            mat.name.empty() ? (std::string("Slot ") + std::to_string(i)) : mat.name;
        data->Mesh->OwnedMaterialSlots[i] =
            !IsEquivalentMaterialShared(sharedMaterial);

        MaterialSource source = CaptureMaterialSource(sharedMaterial);
        source.Name = data->Mesh->MaterialSlotNames[i];
        source.Skinned = skinned;
        source.AlphaBlend = mat.alphaBlend;
        source.AlphaCutout = mat.alphaCutout;
        source.AlphaCutoutThreshold = mat.alphaCutoutThreshold;
        source.TwoSided = mat.twoSided;
        source.HasTint = mat.hasTint || mat.tint != glm::vec4(1.0f);
        source.ColorTint = mat.tint;
        data->Mesh->MaterialSources[i] = source;

        if (!mat.twoSided) {
            allSlotsTwoSided = false;
        }
    }

    if (!data->Mesh->materials.empty() && data->Mesh->materials[0]) {
        data->Mesh->material = data->Mesh->materials[0];
    }
    data->Mesh->ShowBackfaces = allSlotsTwoSided;
    data->Mesh->UniqueMaterial = std::any_of(
        data->Mesh->OwnedMaterialSlots.begin(),
        data->Mesh->OwnedMaterialSlots.end(),
        [](bool owned) { return owned; });
    
    std::cout << "[RuntimeModelInstantiator] Created " << materials.size() 
              << " materials for entity '" << data->Name << "'" << std::endl;
}

} // namespace cm

