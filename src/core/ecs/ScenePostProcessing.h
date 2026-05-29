#pragma once

#include "core/ecs/Scene.h"
#include <vector>

namespace cm {

/**
 * Scene Post-Processing Module
 * 
 * This module provides reusable post-processing functions that resolve entity
 * references and initialize components after entities are deserialized.
 * 
 * These functions are the SINGLE SOURCE OF TRUTH for entity fixups and should
 * be called by:
 *   - Scene JSON loading (DeserializeScene)
 *   - Scene binary loading (EntityBinaryLoader)
 *   - Prefab instantiation (PrefabAPI)
 *   - Model delta application (PrefabDelta)
 * 
 * This ensures prefabs behave identically to scenes ("packed scenes" concept).
 */

/**
 * Full post-processing for a set of entities.
 * Calls all fixup functions in the correct order.
 * 
 * @param scene The scene containing the entities
 * @param entities Entity IDs to process (empty = process all entities in scene)
 */
void PostProcessEntities(Scene& scene, const std::vector<EntityID>& entities = {});

/**
 * Resolve skinning skeleton root references.
 * For each entity with Skinning where SkeletonRoot is INVALID_ENTITY_ID,
 * walks up the hierarchy to find a parent with a Skeleton component.
 * Also searches siblings and sibling children for armor/clothing meshes.
 * 
 * @param scene The scene containing the entities
 * @param entities Entity IDs to process (empty = process all)
 */
void FixupSkeletonReferences(Scene& scene, const std::vector<EntityID>& entities = {});

/**
 * Resolve bone attachment skeleton references.
 * For each entity with BoneAttachment where SkeletonEntity is INVALID_ENTITY_ID,
 * searches the hierarchy for a skeleton and resolves bone names to entity IDs.
 * 
 * @param scene The scene containing the entities
 * @param entities Entity IDs to process (empty = process all)
 */
void FixupBoneAttachments(Scene& scene, const std::vector<EntityID>& entities = {});

/**
 * Initialize skinning system for entities with resolved skeleton references.
 * Creates bone palettes and uniform handles for GPU skinning.
 * 
 * @param scene The scene containing the entities
 * @param entities Entity IDs to process (empty = process all)
 */
void InitializeSkinningComponents(Scene& scene, const std::vector<EntityID>& entities = {});

/**
 * Ensure skinned meshes have appropriate skinned materials.
 * If a mesh has skinning data but uses a non-skinned material, 
 * creates a skinned material variant.
 * 
 * @param scene The scene containing the entities
 * @param entities Entity IDs to process (empty = process all)
 */
void EnsureSkinnedMaterials(Scene& scene, const std::vector<EntityID>& entities = {});

/**
 * Build skeleton avatars for humanoid animation retargeting.
 * For skeletons without avatars, attempts to build one from bone names.
 * 
 * @param scene The scene containing the entities
 * @param entities Entity IDs to process (empty = process all)
 */
void BuildSkeletonAvatars(Scene& scene, const std::vector<EntityID>& entities = {});

/**
 * Fixup animation components - ensure AnimationPlayer is on same entity as Skeleton.
 * Migrates AnimationPlayer if needed for the animation system to work correctly.
 * 
 * @param scene The scene containing the entities
 * @param entities Entity IDs to process (empty = process all)
 */
void FixupAnimationComponents(Scene& scene, const std::vector<EntityID>& entities = {});

} // namespace cm
