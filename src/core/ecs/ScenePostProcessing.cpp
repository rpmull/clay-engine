#include "core/ecs/ScenePostProcessing.h"
#include "core/ecs/EntityData.h"
#include "core/ecs/AnimationComponents.h"
#include "core/rendering/MaterialCache.h"
#include "core/animation/AvatarDefinition.h"
#include "core/jobs/Jobs.h"
#include "core/jobs/ParallelFor.h"
#include <algorithm>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <atomic>
#include <mutex>

namespace cm {

// Helper: Get entities to process for post-processing operations.
// - If entities is empty: returns ALL entities in the scene
// - If entities is non-empty: returns those entities AND all their descendants
// This ensures that when processing prefab roots, child components (like BoneAttachments
// on mesh children) are also processed. Callers can pass a flat list of all entities
// they want processed - duplicates are handled gracefully by the processing functions.
static std::vector<EntityID> GetEntitiesToProcess(Scene& scene, const std::vector<EntityID>& entities) {
    if (entities.empty()) {
        std::vector<EntityID> all;
        for (const Entity& e : scene.GetEntities()) {
            all.push_back(e.GetID());
        }
        return all;
    }
    
    // Collect the specified entities AND all their descendants to ensure
    // child entities (e.g., mesh children with BoneAttachment) are processed
    std::vector<EntityID> result;
    std::function<void(EntityID)> collectWithDescendants = [&](EntityID id) {
        result.push_back(id);
        auto* data = scene.GetEntityData(id);
        if (data) {
            for (EntityID child : data->Children) {
                collectWithDescendants(child);
            }
        }
    };
    
    for (EntityID id : entities) {
        collectWithDescendants(id);
    }
    
    return result;
}

// Helper: Find skeleton by walking up hierarchy
static EntityID FindSkeletonUpHierarchy(Scene& scene, EntityID startId) {
    EntityID cur = startId;
    size_t guard = 0;
    while (cur != INVALID_ENTITY_ID && guard++ < 100000) {
        auto* data = scene.GetEntityData(cur);
        if (!data) break;
        if (data->Skeleton) return cur;
        cur = data->Parent;
    }
    return INVALID_ENTITY_ID;
}

// Helper: Find skeleton in siblings and sibling children (for armor meshes)
static EntityID FindSkeletonInSiblings(Scene& scene, EntityID entityId) {
    auto* data = scene.GetEntityData(entityId);
    if (!data || data->Parent == INVALID_ENTITY_ID) return INVALID_ENTITY_ID;
    
    auto* parentData = scene.GetEntityData(data->Parent);
    if (!parentData) return INVALID_ENTITY_ID;
    
    // Search sibling subtrees
    std::function<EntityID(EntityID)> searchSubtree = [&](EntityID id) -> EntityID {
        auto* d = scene.GetEntityData(id);
        if (!d) return INVALID_ENTITY_ID;
        if (d->Skeleton) return id;
        for (EntityID child : d->Children) {
            EntityID found = searchSubtree(child);
            if (found != INVALID_ENTITY_ID) return found;
        }
        return INVALID_ENTITY_ID;
    };
    
    for (EntityID sibling : parentData->Children) {
        if (sibling == entityId) continue;
        EntityID found = searchSubtree(sibling);
        if (found != INVALID_ENTITY_ID) return found;
    }
    
    return INVALID_ENTITY_ID;
}

// Helper: Find skeleton searching down from a root
static EntityID FindSkeletonInSubtree(Scene& scene, EntityID rootId) {
    std::function<EntityID(EntityID)> search = [&](EntityID id) -> EntityID {
        auto* d = scene.GetEntityData(id);
        if (!d) return INVALID_ENTITY_ID;
        if (d->Skeleton) return id;
        for (EntityID child : d->Children) {
            EntityID found = search(child);
            if (found != INVALID_ENTITY_ID) return found;
        }
        return INVALID_ENTITY_ID;
    };
    return search(rootId);
}

// Helper: Build bone name to entity map from skeleton (with fuzzy name matching)
static std::unordered_map<std::string, EntityID> BuildBoneNameMap(Scene& scene, EntityID skeletonRoot) {
    std::unordered_map<std::string, EntityID> boneMap;
    
    std::function<void(EntityID)> build = [&](EntityID id) {
        auto* data = scene.GetEntityData(id);
        if (!data) return;
        
        // Add full name
        boneMap[data->Name] = id;
        
        // Also add without numeric suffix for fuzzy matching
        // (handles cases like "mixamorig:Hips_98" -> "mixamorig:Hips")
        size_t underscore = data->Name.find_last_of('_');
        if (underscore != std::string::npos) {
            bool allDigits = true;
            for (size_t i = underscore + 1; i < data->Name.size(); ++i) {
                if (!std::isdigit(static_cast<unsigned char>(data->Name[i]))) {
                    allDigits = false;
                    break;
                }
            }
            if (allDigits) {
                boneMap[data->Name.substr(0, underscore)] = id;
            }
        }
        
        for (EntityID child : data->Children) {
            build(child);
        }
    };
    
    build(skeletonRoot);
    return boneMap;
}

static bool IsDescendantOrSelf(Scene& scene, EntityID candidate, EntityID root) {
    if (candidate == INVALID_ENTITY_ID || candidate == static_cast<EntityID>(-1)) {
        return false;
    }

    EntityID cur = candidate;
    size_t guard = 0;
    while (cur != INVALID_ENTITY_ID && cur != static_cast<EntityID>(-1) && guard++ < 100000) {
        if (cur == root) {
            return true;
        }
        auto* data = scene.GetEntityData(cur);
        if (!data) {
            break;
        }
        cur = data->Parent;
    }
    return false;
}

static bool IsValidPrefabSkinningSkeletonRoot(Scene& scene, EntityID meshId, EntityID skeletonRoot) {
    if (skeletonRoot == INVALID_ENTITY_ID || skeletonRoot == static_cast<EntityID>(-1)) {
        return false;
    }

    auto* skelData = scene.GetEntityData(skeletonRoot);
    if (!skelData || !skelData->Skeleton) {
        return false;
    }

    if (meshId == skeletonRoot) {
        return true;
    }

    if (IsDescendantOrSelf(scene, meshId, skeletonRoot)) {
        return true;
    }

    auto* meshData = scene.GetEntityData(meshId);
    if (!meshData) {
        return false;
    }

    if (IsDescendantOrSelf(scene, skeletonRoot, meshId)) {
        return true;
    }

    if (meshData->Parent != INVALID_ENTITY_ID && meshData->Parent != static_cast<EntityID>(-1)) {
        auto* parentData = scene.GetEntityData(meshData->Parent);
        if (parentData) {
            for (EntityID sibling : parentData->Children) {
                if (IsDescendantOrSelf(scene, skeletonRoot, sibling)) {
                    return true;
                }
            }
        }
    }

    return false;
}

static void RebuildSkeletonBoneEntities(Scene& scene, const std::vector<EntityID>& entities) {
    auto toProcess = GetEntitiesToProcess(scene, entities);
    if (toProcess.empty()) return;

    std::unordered_set<EntityID> seenSkeletons;
    for (EntityID id : toProcess) {
        auto* data = scene.GetEntityData(id);
        if (!data || !data->Skeleton || !seenSkeletons.insert(id).second) continue;

        SkeletonComponent& skeleton = *data->Skeleton;
        const size_t boneCount = !skeleton.BoneNames.empty()
            ? skeleton.BoneNames.size()
            : skeleton.InverseBindPoses.size();
        if (boneCount == 0) continue;
        skeleton.BoneCount = static_cast<uint32_t>(boneCount);
        skeleton.EnsureRuntimeBonePaletteSize(boneCount);

        if (skeleton.BindPoseGlobals.size() != skeleton.InverseBindPoses.size()) {
            skeleton.BindPoseGlobals.resize(skeleton.InverseBindPoses.size());
            for (size_t i = 0; i < skeleton.InverseBindPoses.size(); ++i) {
                skeleton.BindPoseGlobals[i] = glm::inverse(skeleton.InverseBindPoses[i]);
            }
        }

        if (skeleton.BoneNameToIndex.empty() && !skeleton.BoneNames.empty()) {
            skeleton.BoneNameToIndex.clear();
            for (size_t i = 0; i < skeleton.BoneNames.size(); ++i) {
                skeleton.BoneNameToIndex[skeleton.BoneNames[i]] = static_cast<int>(i);
            }
        }

        bool needsRebuild = (skeleton.BoneEntities.size() != boneCount);
        if (!needsRebuild) {
            for (size_t i = 0; i < boneCount; ++i) {
                EntityID boneId = skeleton.BoneEntities[i];
                if (boneId == INVALID_ENTITY_ID || boneId == static_cast<EntityID>(-1)) {
                    needsRebuild = true;
                    break;
                }
                auto* boneData = scene.GetEntityData(boneId);
                if (!boneData || !IsDescendantOrSelf(scene, boneId, id)) {
                    needsRebuild = true;
                    break;
                }
            }
        }

        if (!needsRebuild) {
            // BoneSkeletonEntity/BoneIndex are runtime-only (not serialized), so even
            // when the loaded BoneEntities are valid we must (re)stamp the markers.
            scene.RebindSkeletonBoneMarkers(id);
            continue;
        }

        const auto boneMap = BuildBoneNameMap(scene, id);
        skeleton.BoneEntities.assign(boneCount, INVALID_ENTITY_ID);

        size_t resolvedCount = 0;
        for (size_t i = 0; i < boneCount; ++i) {
            const std::string& boneName =
                (i < skeleton.BoneNames.size()) ? skeleton.BoneNames[i] : std::string();
            if (boneName.empty()) continue;

            auto it = boneMap.find(boneName);
            if (it == boneMap.end()) continue;
            if (!IsDescendantOrSelf(scene, it->second, id)) continue;

            skeleton.BoneEntities[i] = it->second;
            ++resolvedCount;
        }

        // Stage 3 foundation: stamp runtime-only bone markers after rebinding by name.
        scene.RebindSkeletonBoneMarkers(id);

        std::cout << "[ScenePostProcessing] Rebuilt " << resolvedCount << "/" << boneCount
                  << " bone entity refs for skeleton '" << data->Name
                  << "' (entity=" << id << ")" << std::endl;
    }
}

//==============================================================================
// Public API Implementation
//==============================================================================

void PostProcessEntities(Scene& scene, const std::vector<EntityID>& entities) {
    // Order matters - each step may depend on results from previous steps:
    // 1. Skeleton references (skinning needs to know its skeleton)
    FixupSkeletonReferences(scene, entities);

    // 2. Rebuild skeleton bone entity refs if binary remapping left them stale.
    RebuildSkeletonBoneEntities(scene, entities);
    
    // 3. Initialize skinning (needs resolved skeleton reference)
    InitializeSkinningComponents(scene, entities);
    
    // 4. Bone attachments (needs skeleton for bone lookup)
    FixupBoneAttachments(scene, entities);
    
    // 5. Materials (needs skinning info to determine material type)
    EnsureSkinnedMaterials(scene, entities);
    
    // 6. Avatars (for animation retargeting)
    BuildSkeletonAvatars(scene, entities);
    
    // 7. Animation fixups (needs avatars)
    FixupAnimationComponents(scene, entities);
}

void FixupSkeletonReferences(Scene& scene, const std::vector<EntityID>& entities) {
    auto toProcess = GetEntitiesToProcess(scene, entities);
    if (toProcess.empty()) return;
    
    // Filter to entities that need skeleton resolution
    std::vector<EntityID> needsResolution;
    for (EntityID id : toProcess) {
        auto* data = scene.GetEntityData(id);
        if (!data || !data->Skinning) continue;
        if (data->Skinning->SkeletonRoot != INVALID_ENTITY_ID &&
            data->Skinning->SkeletonRoot != static_cast<EntityID>(-1)) {
            auto* skelData = scene.GetEntityData(data->Skinning->SkeletonRoot);
            const bool prefabScoped = data->PrefabGuid.high != 0 || data->PrefabGuid.low != 0;
            const bool samePrefab = !prefabScoped ||
                (skelData && skelData->PrefabGuid == data->PrefabGuid);
            const bool validScope = !prefabScoped ||
                IsValidPrefabSkinningSkeletonRoot(scene, id, data->Skinning->SkeletonRoot);
            if (skelData && skelData->Skeleton && samePrefab && validScope) {
                continue;
            }
            // The skeleton root points to a non-skeleton/missing entity; invalidate and re-resolve.
            data->Skinning->SkeletonRoot = static_cast<EntityID>(-1);
            data->Skinning->ResolvedSkeletonRoot = INVALID_ENTITY_ID;
            data->Skinning->InvalidateRemap();
        }
        needsResolution.push_back(id);
    }
    
    if (needsResolution.empty()) return;
    
    // Parallelize skeleton resolution - each entity's processing is independent
    const size_t chunk = ComputeOptimalChunkSize(needsResolution.size(), Jobs().GetWorkerCount(), 8);
    parallel_for(Jobs(), size_t{0}, needsResolution.size(), chunk,
        [&scene, &needsResolution](size_t start, size_t count) {
            for (size_t i = start; i < start + count; ++i) {
                EntityID id = needsResolution[i];
                auto* data = scene.GetEntityData(id);
                if (!data || !data->Skinning) continue;
                
                // Strategy 1: Walk up hierarchy to find skeleton
                EntityID found = FindSkeletonUpHierarchy(scene, id);
                
                // Strategy 2: Check siblings and their children (armor mesh case)
                if (found == INVALID_ENTITY_ID) {
                    found = FindSkeletonInSiblings(scene, id);
                }
                
                // Strategy 3: If this entity has children, search down (might be model root)
                if (found == INVALID_ENTITY_ID && data->Children.size() > 0) {
                    found = FindSkeletonInSubtree(scene, id);
                }
                
                if (found != INVALID_ENTITY_ID) {
                    data->Skinning->SkeletonRoot = found;
                }
            }
        });
}

void FixupBoneAttachments(Scene& scene, const std::vector<EntityID>& entities) {
    auto toProcess = GetEntitiesToProcess(scene, entities);
    if (toProcess.empty()) return;
    
    // First pass: collect entities needing bone attachment resolution and their skeletons
    std::vector<EntityID> needsResolution;
    std::unordered_set<EntityID> skeletonIds;
    
    for (EntityID id : toProcess) {
        auto* data = scene.GetEntityData(id);
        if (!data || !data->BoneAttachment) continue;
        
        // Skip if already resolved
        if (data->BoneAttachment->SkeletonEntity != INVALID_ENTITY_ID &&
            data->BoneAttachment->SkeletonEntity != static_cast<EntityID>(-1) &&
            data->BoneAttachment->ResolutionAttempted) {
            continue;
        }
        
        // Find skeleton for this entity
        EntityID skelId = data->BoneAttachment->SkeletonEntity;
        if (skelId == INVALID_ENTITY_ID || skelId == static_cast<EntityID>(-1)) {
            skelId = FindSkeletonUpHierarchy(scene, id);
            if (skelId == INVALID_ENTITY_ID) {
                skelId = FindSkeletonInSiblings(scene, id);
            }
        }
        
        if (skelId != INVALID_ENTITY_ID) {
            data->BoneAttachment->SkeletonEntity = skelId;
            data->BoneAttachment->ResolvedSkeletonEntity = skelId;
            skeletonIds.insert(skelId);
            needsResolution.push_back(id);
        } else {
            data->BoneAttachment->ResolutionAttempted = true;
        }
    }
    
    if (needsResolution.empty()) return;
    
    // Build bone maps for all required skeletons (sequential - map building is not thread-safe)
    std::unordered_map<EntityID, std::unordered_map<std::string, EntityID>> skeletonBoneMaps;
    for (EntityID skelId : skeletonIds) {
        skeletonBoneMaps[skelId] = BuildBoneNameMap(scene, skelId);
    }
    
    // Second pass: resolve bone names in parallel
    const size_t chunk = ComputeOptimalChunkSize(needsResolution.size(), Jobs().GetWorkerCount(), 16);
    parallel_for(Jobs(), size_t{0}, needsResolution.size(), chunk,
        [&scene, &needsResolution, &skeletonBoneMaps](size_t start, size_t count) {
            for (size_t i = start; i < start + count; ++i) {
                EntityID id = needsResolution[i];
                auto* data = scene.GetEntityData(id);
                if (!data || !data->BoneAttachment) continue;
                
                EntityID skelId = data->BoneAttachment->SkeletonEntity;
                auto mapIt = skeletonBoneMaps.find(skelId);
                if (mapIt == skeletonBoneMaps.end()) {
                    data->BoneAttachment->ResolutionAttempted = true;
                    continue;
                }
                
                const auto& boneMap = mapIt->second;
                
                if (!data->BoneAttachment->TargetBoneName.empty()) {
                    auto it = boneMap.find(data->BoneAttachment->TargetBoneName);
                    if (it != boneMap.end()) {
                        data->BoneAttachment->ResolvedBoneEntity = it->second;
                    }
                }
                
                data->BoneAttachment->ResolutionAttempted = true;
            }
        });
}

void InitializeSkinningComponents(Scene& scene, const std::vector<EntityID>& entities) {
    auto toProcess = GetEntitiesToProcess(scene, entities);
    if (toProcess.empty()) return;
    
    // Filter to entities that need skinning initialization
    std::vector<EntityID> needsInit;
    for (EntityID id : toProcess) {
        auto* data = scene.GetEntityData(id);
        if (!data || !data->Skinning) continue;
        
        EntityID skelRoot = data->Skinning->SkeletonRoot;
        if (skelRoot == INVALID_ENTITY_ID || skelRoot == static_cast<EntityID>(-1)) continue;
        
        auto* skelData = scene.GetEntityData(skelRoot);
        if (!skelData || !skelData->Skeleton) continue;
        if (skelData->Skeleton->InverseBindPoses.empty()) continue;
        
        needsInit.push_back(id);
    }
    
    if (needsInit.empty()) return;
    
    // Initialize on the main thread to avoid contention with other systems
    for (EntityID id : needsInit) {
        auto* data = scene.GetEntityData(id);
        if (!data || !data->Skinning) continue;
        
        EntityID skelRoot = data->Skinning->SkeletonRoot;
        auto* skelData = scene.GetEntityData(skelRoot);
        if (!skelData || !skelData->Skeleton) continue;
        
        if (skelData->Skeleton->InverseBindPoses.empty()) continue;
        data->Skinning->BoneCount = 0;
    }
}

void EnsureSkinnedMaterials(Scene& scene, const std::vector<EntityID>& entities) {
    auto toProcess = GetEntitiesToProcess(scene, entities);
    
    // Material creation may not be thread-safe, so keep sequential
    for (EntityID id : toProcess) {
        auto* data = scene.GetEntityData(id);
        if (!data || !data->Skinning || !data->Mesh) continue;
        
        // Check if mesh has skinning data
        bool meshIsSkinned = data->Mesh->mesh && data->Mesh->mesh->HasSkinning();
        if (!meshIsSkinned) continue;

        MeshComponent& mesh = *data->Mesh;
        const size_t slotCount = std::max<size_t>(1, mesh.materials.size());
        if (mesh.materials.size() < slotCount) {
            mesh.materials.resize(slotCount);
        }
        if (mesh.OwnedMaterialSlots.size() < slotCount) {
            mesh.OwnedMaterialSlots.resize(slotCount, false);
        }

        for (size_t slot = 0; slot < slotCount; ++slot) {
            std::shared_ptr<Material> current =
                mesh.materials[slot]
                    ? mesh.materials[slot]
                    : ((slot == 0) ? mesh.material : nullptr);
            if (!current && slot != 0) {
                continue;
            }
            std::shared_ptr<Material> skinned =
                AcquireSkinnedMaterialVariant(scene, current);
            if (!skinned) {
                continue;
            }

            mesh.materials[slot] = skinned;
            mesh.OwnedMaterialSlots[slot] =
                !GetMaterialEquivalenceKey(skinned.get()).EquivalentSafe;
        }

        if (!mesh.materials.empty() && mesh.materials[0]) {
            mesh.material = mesh.materials[0];
        } else {
            mesh.material = AcquireSkinnedMaterialVariant(scene, mesh.material);
        }

        mesh.UniqueMaterial = std::any_of(
            mesh.OwnedMaterialSlots.begin(),
            mesh.OwnedMaterialSlots.end(),
            [](bool owned) { return owned; });
    }
}

void BuildSkeletonAvatars(Scene& scene, const std::vector<EntityID>& entities) {
    auto toProcess = GetEntitiesToProcess(scene, entities);
    if (toProcess.empty()) return;
    
    // Filter to entities that need avatar building
    std::vector<EntityID> needsAvatar;
    for (EntityID id : toProcess) {
        auto* data = scene.GetEntityData(id);
        if (!data || !data->Skeleton) continue;
        if (data->Skeleton->Avatar) continue;
        needsAvatar.push_back(id);
    }
    
    if (needsAvatar.empty()) return;
    
    // Parallelize avatar building
    const size_t chunk = ComputeOptimalChunkSize(needsAvatar.size(), Jobs().GetWorkerCount(), 4);
    parallel_for(Jobs(), size_t{0}, needsAvatar.size(), chunk,
        [&scene, &needsAvatar](size_t start, size_t count) {
            for (size_t i = start; i < start + count; ++i) {
                EntityID id = needsAvatar[i];
                auto* data = scene.GetEntityData(id);
                if (!data || !data->Skeleton) continue;
                if (data->Skeleton->Avatar) continue;
                
                data->Skeleton->Avatar = std::make_unique<cm::animation::AvatarDefinition>();
                cm::animation::avatar_builders::BuildFromSkeleton(*data->Skeleton, *data->Skeleton->Avatar, true);
            }
        });
}

void FixupAnimationComponents(Scene& scene, const std::vector<EntityID>& entities) {
    auto toProcess = GetEntitiesToProcess(scene, entities);
    
    // Animation component migration involves moving unique_ptrs between entities
    // Keep sequential to avoid race conditions
    std::unordered_set<EntityID> processSet(toProcess.begin(), toProcess.end());
    
    for (EntityID id : toProcess) {
        auto* data = scene.GetEntityData(id);
        if (!data) continue;
        
        // If entity has AnimationPlayer but no Skeleton, find skeleton in children
        if (data->AnimationPlayer && !data->Skeleton) {
            EntityID skelChild = FindSkeletonInSubtree(scene, id);
            if (skelChild != INVALID_ENTITY_ID && skelChild != id) {
                auto* skelData = scene.GetEntityData(skelChild);
                if (skelData && !skelData->AnimationPlayer) {
                    // Move AnimationPlayer to skeleton entity
                    skelData->AnimationPlayer = std::move(data->AnimationPlayer);
                }
            }
        }
    }
}

} // namespace cm
