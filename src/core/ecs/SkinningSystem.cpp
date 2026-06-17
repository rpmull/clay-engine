#include "SkinningSystem.h"
#include "core/ecs/AnimationComponents.h"
#include "core/ecs/NpcScalability.h"
#include "core/rendering/VertexTypes.h"
#include "core/rendering/ShaderManager.h"
#include "core/physics/ragdoll/RagdollSystem.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <bgfx/bgfx.h>
#include <iostream>

#include "core/utils/DebugModelDump.h"

#include "core/jobs/JobSystem.h"   
#include "core/jobs/ParallelFor.h"
#include "core/jobs/Jobs.h"
#include "core/rendering/Camera.h"
#include "core/rendering/Renderer.h"
#include "core/utils/PrefabPerfDiagnostics.h"
#include "core/utils/Profiler.h"
#include "core/world/RuntimeWorld.h"

#include <chrono>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

// ---------- Blendshape kernel (adds pre-accumulated deltas to base) ----------
struct MorphBlendArgs {
	const glm::vec3* basePos; const glm::vec3* baseNrm;
	const glm::vec3* accDP;   const glm::vec3* accDN; // weighted sums per vertex
	size_t           vCount;
	// Output contiguous arrays (avoid assuming interleaved vertex stride)
	glm::vec3* outPos; glm::vec3* outNrm;
	int start, count;
};
static inline void MorphBlendKernel(const MorphBlendArgs& a) {
	const int end = a.start + a.count;
	for (int i = a.start; i < end; ++i) {
		a.outPos[i] = a.basePos[i] + a.accDP[i];
		a.outNrm[i] = a.baseNrm[i] + a.accDN[i];
	}
}

static constexpr float kMaterializedMorphWeightThreshold = 1e-4f;

static inline glm::mat4 GetWorldOrIdentity(Scene& scene, EntityID id)
{
	auto* data = scene.GetEntityData(id);
	if (data)
	{
		return data->Transform.WorldMatrix;
	}
	return glm::mat4(1.0f);
}

static bool HasActiveLookAtConstraint(const EntityData& data)
{
	return std::any_of(
		data.LookAtConstraints.begin(),
		data.LookAtConstraints.end(),
		[](const auto& constraint) {
			return constraint.Enabled &&
				constraint.Weight > 0.0f &&
				!constraint.BoneChain.empty() &&
				constraint.TargetEntity != 0;
		});
}

static bool HasPotentialIkConstraint(const EntityData& data)
{
	if (std::any_of(
		data.IKs.begin(),
		data.IKs.end(),
		[](const auto& constraint) {
			return constraint.Enabled &&
				constraint.Weight > 0.0f &&
				constraint.TargetEntity != 0;
		})) {
		return true;
	}

	if (!data.Extra.is_object() ||
		!data.Extra.contains("ik") ||
		!data.Extra["ik"].is_array()) {
		return false;
	}

	for (const auto& entry : data.Extra["ik"]) {
		if (!entry.is_object()) {
			continue;
		}

		if (!entry.value("enabled", true)) {
			continue;
		}

		if (entry.value("weight", 1.0f) <= 0.0f) {
			continue;
		}

		if (!entry.contains("target") || !entry["target"].is_number_integer()) {
			continue;
		}

		if (entry["target"].get<int64_t>() != 0) {
			return true;
		}
	}

	return false;
}

static bool HasActivePostAnimationConstraint(const EntityData& data)
{
	return HasActiveLookAtConstraint(data) ||
		HasPotentialIkConstraint(data);
}

struct SkinningPlane {
	glm::vec4 p;
};

struct SkinningFrustum {
	SkinningPlane planes[6];
};

struct CrowdLodBudget {
	uint32_t FullRateCount = 0;
	uint32_t ThirtyHzCount = 0;
	uint32_t FifteenHzCount = 0;
	float OverflowInterval = 0.100f;
};

static SkinningFrustum BuildSkinningFrustum(const glm::mat4& view, const glm::mat4& proj)
{
	const glm::mat4 vp = proj * view;
	auto row = [&](int r) { return glm::vec4(vp[0][r], vp[1][r], vp[2][r], vp[3][r]); };
	const glm::vec4 r0 = row(0);
	const glm::vec4 r1 = row(1);
	const glm::vec4 r2 = row(2);
	const glm::vec4 r3 = row(3);

	SkinningFrustum f{};
	glm::vec4 planes[6] = {
	   r3 + r0, // left
	   r3 - r0, // right
	   r3 + r1, // bottom
	   r3 - r1, // top
	   r3 + r2, // near
	   r3 - r2  // far
	};
	for (int i = 0; i < 6; ++i) {
		const glm::vec3 n(planes[i]);
		const float len = glm::length(n);
		if (len > 1e-6f) {
			planes[i] /= len;
		}
		f.planes[i].p = planes[i];
	}
	return f;
}

static bool AabbIntersectsSkinningFrustum(const SkinningFrustum& f, const glm::vec3& wmin, const glm::vec3& wmax)
{
	for (int i = 0; i < 6; ++i) {
		const glm::vec4& pl = f.planes[i].p;
		glm::vec3 p;
		p.x = (pl.x >= 0.0f) ? wmax.x : wmin.x;
		p.y = (pl.y >= 0.0f) ? wmax.y : wmin.y;
		p.z = (pl.z >= 0.0f) ? wmax.z : wmin.z;
		const float d = pl.x * p.x + pl.y * p.y + pl.z * p.z + pl.w;
		if (d < 0.0f) {
			return false;
		}
	}
	return true;
}

static EntityID ResolveActiveCameraOwnerEntity(Scene& scene)
{
	const EntityID activeCameraEntity = scene.GetActiveCameraEntityID();
	if (activeCameraEntity == INVALID_ENTITY_ID) {
		return INVALID_ENTITY_ID;
	}

	EntityData* cameraData = scene.GetEntityData(activeCameraEntity);
	if (!cameraData) {
		return activeCameraEntity;
	}

	return cameraData->Parent != INVALID_ENTITY_ID
		? cameraData->Parent
		: activeCameraEntity;
}

static bool IsEntityDescendantOf(Scene& scene, EntityID entityId, EntityID ancestorId)
{
	if (entityId == INVALID_ENTITY_ID || ancestorId == INVALID_ENTITY_ID) {
		return false;
	}

	EntityID current = entityId;
	while (current != INVALID_ENTITY_ID)
	{
		if (current == ancestorId) {
			return true;
		}

		EntityData* data = scene.GetEntityData(current);
		if (!data) {
			break;
		}

		current = data->Parent;
	}

	return false;
}

static CrowdLodBudget ResolveCrowdLodBudget(size_t visibleCount)
{
	CrowdLodBudget budget{};
	budget.OverflowInterval = (visibleCount >= 48) ? 0.133f : 0.100f;

	if (visibleCount == 0) {
		return budget;
	}

	if (visibleCount <= 10) {
		budget.FullRateCount = static_cast<uint32_t>(visibleCount);
		return budget;
	}

	budget.FullRateCount = 10;

	if (visibleCount <= 16) {
		budget.ThirtyHzCount = static_cast<uint32_t>(visibleCount) - budget.FullRateCount;
		return budget;
	}

	budget.ThirtyHzCount = 6;

	if (visibleCount <= 24) {
		budget.FifteenHzCount = static_cast<uint32_t>(visibleCount) - budget.FullRateCount - budget.ThirtyHzCount;
		return budget;
	}

	if (visibleCount <= 40) {
		budget.FullRateCount = 8;
		budget.ThirtyHzCount = 8;
		budget.FifteenHzCount = 12;
		return budget;
	}

	budget.FullRateCount = 6;
	budget.ThirtyHzCount = 10;
	budget.FifteenHzCount = 16;
	return budget;
}

static float ResolveCrowdLodInterval(uint32_t rank, size_t visibleCount, bool* throttled = nullptr)
{
	const CrowdLodBudget budget = ResolveCrowdLodBudget(visibleCount);
	if (rank < budget.FullRateCount) {
		if (throttled) *throttled = false;
		return 0.0f;
	}

	if (throttled) *throttled = true;
	if (rank < budget.FullRateCount + budget.ThirtyHzCount) {
		return 0.033f;
	}
	if (rank < budget.FullRateCount + budget.ThirtyHzCount + budget.FifteenHzCount) {
		return 0.067f;
	}
	return budget.OverflowInterval;
}

static inline bool MatrixNearlyEqual(const glm::mat4& a, const glm::mat4& b, float eps = 1e-4f)
{
	for (int c = 0; c < 4; ++c) {
		for (int r = 0; r < 4; ++r) {
			if (std::abs(a[c][r] - b[c][r]) > eps) {
				return false;
			}
		}
	}
	return true;
}

static bool CanBootstrapGpuSharedSkeletonSourceForGpuMorph(
	const SkinningComponent* skinning,
	const Mesh* meshPtr,
	uint32_t paletteBoneCount)
{
	if (!skinning || !meshPtr || paletteBoneCount == 0) {
		return false;
	}

	const bool needsDynamicGpuRetarget =
		skinning->UseParentSkeleton &&
		!skinning->BoneRemap.empty() &&
		!skinning->SkeletonsIdentical &&
		!skinning->UsesFullSkeletonIndices;

	if (needsDynamicGpuRetarget) {
		const size_t expected = static_cast<size_t>(paletteBoneCount);
		if (paletteBoneCount > SkinningComponent::MaxDirectVertexBones) {
			return false;
		}
		if (skinning->GpuBoneIndexRemap.size() != expected) {
			return false;
		}
		if (!skinning->GpuBoneCorrectionPalette.empty() &&
			skinning->GpuBoneCorrectionPalette.size() != expected) {
			return false;
		}
		return true;
	}

	if (meshPtr->UsesCompactSkinningPalette()) {
		return true;
	}

	return paletteBoneCount <= SkinningComponent::MaxDirectVertexBones;
}

// Minimum number of LOD-relevant instances sharing a batch key before GPU crowd
// morph activates. Restored to the original author value of 8 after profiling
// (world.scene) showed the relevant gate is the render-state check
// (Skinning/GpuMorphRenderStateBlocked), not batch size
// (Skinning/GpuMorphBatchTooSmall == 0), and that NPC morph weights are ~0 so no
// per-frame morph work exists to move. This is a tuning knob for morph-heavy crowds.
constexpr uint32_t kGpuMorphCrowdMinBatchSize = 8;

static uint64_t HashCombineStable(uint64_t seed, uint64_t value)
{
	seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
	return seed;
}

static bool HasBlockingGpuMorphPropertyBlocks(const MeshComponent& mesh)
{
	if (!mesh.PropertyBlock.Empty()) {
		return true;
	}

	for (const MaterialPropertyBlock& slotBlock : mesh.SlotPropertyBlocks) {
		if (!slotBlock.Empty()) {
			return true;
		}
	}

	return false;
}

static bool TryBuildGpuMorphCrowdBatchKey(
	const EntityData* data,
	const Mesh* meshPtr,
	const BlendShapeComponent* blendShapes,
	bgfx::ProgramHandle skinnedPbrProgram,
	uint64_t& outKey)
{
	outKey = 0;
	if (!data || !data->Mesh || !meshPtr || !blendShapes ||
		!bgfx::isValid(skinnedPbrProgram)) {
		return false;
	}

	const MeshComponent& meshComponent = *data->Mesh;
	if (data->RenderOverrides ||
		meshComponent.RenderOnTop ||
		!meshComponent.SubmeshOwners.empty() ||
		HasBlockingGpuMorphPropertyBlocks(meshComponent)) {
		return false;
	}

	auto materialForSlot = [&](size_t slot) -> const std::shared_ptr<Material>& {
		if (slot < meshComponent.materials.size() && meshComponent.materials[slot]) {
			return meshComponent.materials[slot];
		}
		return meshComponent.material;
	};

	auto materialIsSupported = [&](const std::shared_ptr<Material>& material) -> bool {
		if (!material || !bgfx::isValid(material->GetProgram()) ||
			material->GetProgram().idx != skinnedPbrProgram.idx) {
			return false;
		}

		// The current skinned instancing path is opaque-only. Transparent meshes
		// would fall back to per-draw GPU morphing and multiply the shader work
		// across passes, so keep them on the CPU dynamic-buffer path.
		return (material->GetStateFlags() & BGFX_STATE_BLEND_ALPHA) == 0;
	};

	uint64_t key = 1469598103934665603ULL;
	// CROWD-BATCH FIX: key on stable mesh *content* identity rather than the
	// per-instance Dynamic Mesh pointer. Identical-prefab NPCs each own a distinct
	// Dynamic Mesh clone (so blendshape morph targets can be written per instance),
	// so hashing the pointer gave every instance a unique batch key -> each batch
	// size 1 < kGpuMorphCrowdMinBatchSize -> GPU morph never activated
	// (0 active / N eligible). Content identity lets instances of the same source
	// mesh share a key and form a crowd batch, while the geometry signature +
	// vertex/index counts + bone-remap hash still strongly distinguish different
	// meshes. (The shared material pointer below already separates batches by
	// appearance, which is the desired behavior.)
	key = HashCombineStable(key, static_cast<uint64_t>(meshPtr->numVertices));
	key = HashCombineStable(key, static_cast<uint64_t>(meshPtr->numIndices));
	key = HashCombineStable(key, meshPtr->GetCachedSkinningBoneRemapHash());
	key = HashCombineStable(key, meshPtr->SkinnedLayout ? 1ull : 0ull);
	key = HashCombineStable(key, blendShapes->GetGeometrySignature(meshPtr->Vertices.size()));
	key = HashCombineStable(key, meshComponent.ShowBackfaces ? 1ull : 0ull);

	if (!meshPtr->Submeshes.empty()) {
		key = HashCombineStable(key, static_cast<uint64_t>(meshPtr->Submeshes.size()));
		for (const auto& submesh : meshPtr->Submeshes) {
			const std::shared_ptr<Material>& material = materialForSlot(submesh.materialSlot);
			if (!materialIsSupported(material)) {
				return false;
			}
			key = HashCombineStable(key, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(material.get())));
			key = HashCombineStable(key, static_cast<uint64_t>(material->GetProgram().idx));
			key = HashCombineStable(key, material->GetStateFlags());
			key = HashCombineStable(key, submesh.materialSlot);
			key = HashCombineStable(key, submesh.indexStart);
			key = HashCombineStable(key, submesh.indexCount);
		}
	} else {
		const std::shared_ptr<Material>& material = materialForSlot(0);
		if (!materialIsSupported(material)) {
			return false;
		}
		key = HashCombineStable(key, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(material.get())));
		key = HashCombineStable(key, static_cast<uint64_t>(material->GetProgram().idx));
		key = HashCombineStable(key, material->GetStateFlags());
		key = HashCombineStable(key, meshPtr->numIndices);
	}

	outKey = key == 0 ? 1 : key;
	return true;
}

static bool CanSkipCpuBlendshapeUpdateForGpuMorph(
	const EntityData* data,
	const Mesh* meshPtr,
	uint32_t paletteBoneCount)
{
	(void)paletteBoneCount;
	if (!data || !data->Mesh || !data->Skinning || !data->BlendShapes || !meshPtr || !meshPtr->Dynamic) {
		return false;
	}

	Renderer& renderer = Renderer::Get();
	if (!renderer.SupportsGpuSkinningAtlas()) {
		return false;
	}

	return renderer.CanRenderGpuMorphTargets(
		data->Skinning.get(),
		meshPtr,
		data->BlendShapes.get());
}

static bool CanUseMaterializedSkinningColorMaterials(
	const EntityData* data,
	const Mesh* meshPtr)
{
	if (!data || !data->Mesh || !meshPtr) {
		return false;
	}

	static bgfx::ProgramHandle s_skinnedPbrProgram = BGFX_INVALID_HANDLE;
	static bgfx::ProgramHandle s_skinnedPsxProgram = BGFX_INVALID_HANDLE;
	if (!bgfx::isValid(s_skinnedPbrProgram)) {
		s_skinnedPbrProgram =
			ShaderManager::Instance().LoadProgram("vs_pbr_skinned", "fs_pbr_skinned");
	}
	if (!bgfx::isValid(s_skinnedPsxProgram)) {
		s_skinnedPsxProgram =
			ShaderManager::Instance().LoadProgram("vs_psx_skinned", "fs_psx");
	}

	auto materialForSlot = [&](size_t slot) -> const std::shared_ptr<Material>& {
		if (slot < data->Mesh->materials.size() && data->Mesh->materials[slot]) {
			return data->Mesh->materials[slot];
		}
		return data->Mesh->material;
	};

	auto materialIsSupported = [&](const std::shared_ptr<Material>& material) -> bool {
		if (!material || !bgfx::isValid(material->GetProgram())) {
			return false;
		}

		const uint16_t programIdx = material->GetProgram().idx;
		return (bgfx::isValid(s_skinnedPbrProgram) && programIdx == s_skinnedPbrProgram.idx) ||
			(bgfx::isValid(s_skinnedPsxProgram) && programIdx == s_skinnedPsxProgram.idx);
	};

	if (!meshPtr->Submeshes.empty()) {
		for (const auto& submesh : meshPtr->Submeshes) {
			if (!materialIsSupported(materialForSlot(submesh.materialSlot))) {
				return false;
			}
		}
		return true;
	}

	return materialIsSupported(materialForSlot(0));
}

static bool CanEnableGpuMorphForMaterializedSkinning(
	const EntityData* data,
	const Mesh* meshPtr)
{
	if (!data || !data->Mesh || !data->Skinning || !data->BlendShapes || !meshPtr || !meshPtr->Dynamic) {
		return false;
	}

	Renderer& renderer = Renderer::Get();
	if (!renderer.SupportsGpuSkinningAtlas() ||
		!CanUseMaterializedSkinningColorMaterials(data, meshPtr)) {
		return false;
	}

	return renderer.CanUseGpuMaterializedSkinning(
		data->Skinning.get(),
		meshPtr,
		data->BlendShapes.get());
}

// ============================================================================
// Skeleton Resolution: Walk up parent hierarchy to find skeleton root
// ============================================================================
static EntityID ResolveSkeletonFromHierarchy(Scene& scene, EntityID startEntity)
{
	EntityID current = startEntity;
	while (current != INVALID_ENTITY_ID)
	{
		EntityData* data = scene.GetEntityData(current);
		if (!data)
			break;

		// Check if this entity has a skeleton
		if (data->Skeleton && !data->Skeleton->BoneEntities.empty())
		{
			return current;
		}

		// Walk up to parent
		current = data->Parent;
	}

	return INVALID_ENTITY_ID;
}

// ============================================================================
// Build bone remap table for skeleton retargeting
// Maps: original bone index -> target skeleton bone index (-1 if not found)
// Returns true if retargeting is actually needed (different skeleton structure)
// Returns false if skeletons are identical (can use fast path)
// ============================================================================
static bool BuildBoneRemap(SkinningComponent* skinning, SkeletonComponent* targetSkel)
{
	if (!skinning || !targetSkel)
		return false;

	const size_t originalBoneCount = skinning->OriginalBoneNames.size();
	if (originalBoneCount == 0)
	{
		// No original bone names stored - can't remap
		skinning->BoneRemap.clear();
		skinning->SkeletonsIdentical = false;
		return false;
	}

	skinning->BoneRemap.resize(originalBoneCount, -1);

	// Build lookup from target skeleton bone names to indices
	std::unordered_map<std::string, int> targetBoneIndex;
	for (size_t i = 0; i < targetSkel->BoneNames.size(); ++i)
	{
		targetBoneIndex[targetSkel->BoneNames[i]] = static_cast<int>(i);
	}

	// Map each original bone to target skeleton by name
	int foundCount = 0;
	bool isIdentityMapping = true;
	for (size_t i = 0; i < originalBoneCount; ++i)
	{
		const std::string& origName = skinning->OriginalBoneNames[i];
		auto it = targetBoneIndex.find(origName);
		if (it != targetBoneIndex.end())
		{
			skinning->BoneRemap[i] = it->second;
			++foundCount;
			// Check if this is NOT an identity mapping (bone indices differ)
			if (it->second != static_cast<int>(i))
			{
				isIdentityMapping = false;
			}
		}
		else
		{
			skinning->BoneRemap[i] = -1; // Bone not found in target skeleton
			isIdentityMapping = false;
			std::cerr << "[SkinningSystem] Warning: Bone '" << origName
				<< "' not found in target skeleton" << std::endl;
		}
	}

	// Check if original inverse bind poses are available for proper retargeting
	const bool hasOriginalInvBind = !skinning->OriginalInverseBindPoses.empty();

	if (!hasOriginalInvBind) {
		std::cerr << "[SkinningSystem] WARNING: No original inverse bind poses stored for armor mesh.\n"
			<< "  Ensure the armor model was reimported with 'Reimport as Armor'." << std::endl;
	}

	// ============================================================================
	// Same-skeleton detection: Skip retargeting when armor uses identical skeleton
	// ============================================================================
	// When armor is authored on the EXACT same skeleton as the character:
	// 1. All bone names match (foundCount == originalBoneCount)
	// 2. Bone indices are identity (BoneRemap[i] == i for all i)
	// 3. Inverse bind poses match within epsilon tolerance
	//
	// In this case, retargeting introduces floating-point precision errors
	// (e.g., inverse(parentInvBind) * armorInvBind ≈ identity but not exactly)
	// which can cause artifacts like warped shoulders during animation.
	// ============================================================================

	bool skeletonsIdentical = false;
	if (isIdentityMapping &&
		foundCount == static_cast<int>(originalBoneCount) &&
		originalBoneCount == targetSkel->BoneNames.size() &&
		hasOriginalInvBind &&
		skinning->OriginalInverseBindPoses.size() == targetSkel->InverseBindPoses.size())
	{
		// Check if inverse bind poses are identical within tolerance
		constexpr float EPSILON = 1e-4f;
		bool posesMatch = true;

		for (size_t i = 0; i < originalBoneCount && posesMatch; ++i)
		{
			const glm::mat4& orig = skinning->OriginalInverseBindPoses[i];
			const glm::mat4& target = targetSkel->InverseBindPoses[i];

			for (int c = 0; c < 4 && posesMatch; ++c)
			{
				for (int r = 0; r < 4 && posesMatch; ++r)
				{
					if (std::abs(orig[c][r] - target[c][r]) > EPSILON)
					{
						posesMatch = false;
					}
				}
			}
		}

		if (posesMatch)
		{
			skeletonsIdentical = true;
		}
	}

	skinning->SkeletonsIdentical = skeletonsIdentical;

	// Return true if retargeting is actually needed
	return !skeletonsIdentical;
}

// ============================================================================
// Register armor blend shapes with parent skeleton's UnifiedMorphComponent
// Called when an armor mesh validates its skeleton binding (one-way registration)
// ============================================================================
static void RegisterArmorBlendShapesWithUnifiedMorph(Scene& scene, EntityID armorMeshId, EntityID skeletonRootId)
{
	// Check if the armor mesh has blend shapes
	auto* armorData = scene.GetEntityData(armorMeshId);
	if (!armorData || !armorData->BlendShapes || armorData->BlendShapes->Shapes.empty()) {
		return;
	}

	// Find UnifiedMorphComponent on the skeleton entity or walk up hierarchy
	UnifiedMorphComponent* unifiedMorph = nullptr;
	EntityID cur = skeletonRootId;
	while (cur != INVALID_ENTITY_ID && !unifiedMorph) {
		auto* data = scene.GetEntityData(cur);
		if (!data) break;
		if (data->UnifiedMorph) {
			unifiedMorph = data->UnifiedMorph.get();
		}
		cur = data->Parent;
	}

	if (!unifiedMorph) {
		// No UnifiedMorphComponent found in skeleton hierarchy
		return;
	}

	// Check if this armor mesh is already registered
	bool alreadyMember = false;
	for (EntityID existing : unifiedMorph->MemberMeshes) {
		if (existing == armorMeshId) {
			alreadyMember = true;
			break;
		}
	}

	if (!alreadyMember) {
		// Register the armor mesh as a member
		unifiedMorph->MemberMeshes.push_back(armorMeshId);
		std::cout << "[SkinningSystem] Registered armor mesh '" << armorData->Name
			<< "' (ID=" << armorMeshId << ") with UnifiedMorphComponent" << std::endl;
	}

	// Add any morph names from the armor that aren't already in the unified list
	bool addedNames = false;
	for (const auto& shape : armorData->BlendShapes->Shapes) {
		bool found = false;
		for (const std::string& existingName : unifiedMorph->Names) {
			if (existingName == shape.Name) {
				found = true;
				break;
			}
		}
		if (!found) {
			unifiedMorph->Names.push_back(shape.Name);
			unifiedMorph->Weights.push_back(0.0f);
			addedNames = true;
			std::cout << "[SkinningSystem] Added morph '" << shape.Name
				<< "' from armor to UnifiedMorphComponent" << std::endl;
		}
	}
	if (addedNames) {
		unifiedMorph->NameIndexDirty = true;
	}

	// Propagate current unified morph weights to this armor mesh
	// This ensures the armor immediately syncs with existing character morphs
	if (!alreadyMember || addedNames) {
		for (auto& shape : armorData->BlendShapes->Shapes) {
			for (size_t i = 0; i < unifiedMorph->Names.size(); ++i) {
				if (shape.Name == unifiedMorph->Names[i]) {
					shape.Weight = unifiedMorph->Weights[i];
					armorData->BlendShapes->Dirty = true;
					break;
				}
			}
		}
	}
}

// ============================================================================
// Resolve skeleton root for a skinning component based on its settings
// ============================================================================
static EntityID ResolveSkeletonRoot(Scene& scene, EntityID meshEntity, SkinningComponent* skinning)
{
	if (!skinning)
		return INVALID_ENTITY_ID;

	if (skinning->ResolvedSkeletonRoot != INVALID_ENTITY_ID &&
		skinning->ResolvedSkeletonRoot != (EntityID)-1)
	{
		EntityData* resolvedData = scene.GetEntityData(skinning->ResolvedSkeletonRoot);
		if (resolvedData && resolvedData->Skeleton) {
			return skinning->ResolvedSkeletonRoot;
		}
		skinning->ResolvedSkeletonRoot = INVALID_ENTITY_ID;
	}

	// Priority 1: Explicit skeleton override GUID (future use)
	// if (!skinning->SkeletonOverrideGuid.IsNull()) { ... resolve by GUID ... }

	// Priority 2: UseParentSkeleton - walk up hierarchy
	if (skinning->UseParentSkeleton)
	{
		EntityData* meshData = scene.GetEntityData(meshEntity);
		if (meshData && meshData->Parent != INVALID_ENTITY_ID)
		{
			EntityID resolved = ResolveSkeletonFromHierarchy(scene, meshData->Parent);
			if (resolved != INVALID_ENTITY_ID)
			{
				skinning->ResolvedSkeletonRoot = resolved;
				return resolved;
			}
		}
	}

	// Priority 3: Explicit SkeletonRoot (original behavior)
	if (skinning->SkeletonRoot != INVALID_ENTITY_ID && skinning->SkeletonRoot != (EntityID)-1)
	{
		EntityData* skelData = scene.GetEntityData(skinning->SkeletonRoot);
		if (skelData && skelData->Skeleton) {
			skinning->ResolvedSkeletonRoot = skinning->SkeletonRoot;
			return skinning->SkeletonRoot;
		}
		// Stale/non-skeleton target: force re-resolution path on subsequent passes.
		skinning->SkeletonRoot = (EntityID)-1;
		skinning->ResolvedSkeletonRoot = INVALID_ENTITY_ID;
		skinning->InvalidateRemap();
	}

	EntityData* meshData = scene.GetEntityData(meshEntity);
	if (meshData && meshData->Parent != INVALID_ENTITY_ID)
	{
		EntityID resolved = ResolveSkeletonFromHierarchy(scene, meshData->Parent);
		if (resolved != INVALID_ENTITY_ID)
		{
			skinning->SkeletonRoot = resolved;
			skinning->ResolvedSkeletonRoot = resolved;
			return resolved;
		}
	}

	return INVALID_ENTITY_ID;
}

static bool ComputeSkeletonWorldBounds(Scene& scene,
	EntityID skeletonRoot,
	glm::vec3& outMin,
	glm::vec3& outMax)
{
	if (skeletonRoot == INVALID_ENTITY_ID) {
		return false;
	}

	EntityData* skeletonData = scene.GetEntityData(skeletonRoot);
	if (!skeletonData || !skeletonData->Skeleton || skeletonData->Skeleton->BoneEntities.empty()) {
		return false;
	}

	glm::vec3 minBounds(std::numeric_limits<float>::max());
	glm::vec3 maxBounds(-std::numeric_limits<float>::max());
	bool hasBounds = false;
	for (EntityID boneEntity : skeletonData->Skeleton->BoneEntities) {
		EntityData* boneData = scene.GetEntityData(boneEntity);
		if (!boneData) {
			continue;
		}

		const glm::vec3 bonePos = glm::vec3(boneData->Transform.WorldMatrix[3]);
		minBounds = glm::min(minBounds, bonePos);
		maxBounds = glm::max(maxBounds, bonePos);
		hasBounds = true;
	}

	if (!hasBounds) {
		return false;
	}

	outMin = minBounds;
	outMax = maxBounds;
	return true;
}

static glm::vec3 ComputeMeshWorldExtents(const EntityData* data, const Mesh* mesh)
{
	if (!data || !data->Mesh || !mesh) {
		return glm::vec3(0.0f);
	}

	const glm::vec3 localExtents =
		(mesh->BoundsMax - mesh->BoundsMin) * 0.5f * std::max(0.01f, data->Mesh->BoundsPadding);
	const glm::mat4& M = data->Transform.WorldMatrix;
	glm::vec3 extents;
	extents.x = std::abs(M[0][0]) * localExtents.x + std::abs(M[1][0]) * localExtents.y + std::abs(M[2][0]) * localExtents.z;
	extents.y = std::abs(M[0][1]) * localExtents.x + std::abs(M[1][1]) * localExtents.y + std::abs(M[2][1]) * localExtents.z;
	extents.z = std::abs(M[0][2]) * localExtents.x + std::abs(M[1][2]) * localExtents.y + std::abs(M[2][2]) * localExtents.z;
	return extents;
}

void SkinningSystem::Update(Scene& scene, float dt)
{
	const cm::world::RuntimeWorld* runtimeWorld = scene.GetRuntimeWorld();
	std::vector<EntityID> fallbackEntityIds;
	const std::vector<EntityID>* skinnedMeshEntityIds = nullptr;
	const std::vector<EntityID>* renderableMeshEntityIds = nullptr;
	if (runtimeWorld) {
		skinnedMeshEntityIds = &runtimeWorld->GetSkinnedMeshSceneEntities();
		renderableMeshEntityIds = &runtimeWorld->GetRenderableSceneEntities();
	}
	else {
		const auto& entities = scene.GetEntities();
		fallbackEntityIds.reserve(entities.size());
		for (const auto& ent : entities) {
			fallbackEntityIds.push_back(ent.GetID());
		}
		skinnedMeshEntityIds = &fallbackEntityIds;
		renderableMeshEntityIds = &fallbackEntityIds;
	}
	static uint32_t s_FrameCounter = 0;
	++s_FrameCounter;
	uint64_t skelGroupsTotal = 0;
	uint64_t skelGroupsUpdated = 0;
	uint64_t skelGroupsLodSkipped = 0;
	uint64_t skelGroupsCrowdThrottled = 0;
	uint64_t skelGroupsRagdollThrottled = 0;
	uint64_t skelGroupsNoConsumerSkipped = 0;
	uint64_t skelGroupsLodRelevant = 0;
	uint64_t skelGroupsDormantSkipped = 0;
	uint64_t skelGroupsAnimatedPaletteReused = 0;
	uint64_t skelGroupsConstraintOwned = 0;
	uint64_t skinnedPaletteSkipped = 0;
	uint64_t skinnedBlendshapeSkipped = 0;
	uint64_t skelGroupsDisabledPoseReused = 0;
	uint64_t skinnedGpuSharedSourceMeshes = 0;
	uint64_t skinnedGpuRemapMeshes = 0;
	uint64_t skinnedGpuCorrectionMeshes = 0;
	uint64_t skinnedGpuMorphCandidateMeshes = 0;
	uint64_t skinnedGpuMorphBaseCapableMeshes = 0;
	uint64_t skinnedGpuMorphCpuConsumerBlocked = 0;
	uint64_t skinnedGpuMorphRenderStateBlocked = 0;
	uint64_t skinnedGpuMorphBatchTooSmall = 0;
	uint64_t skinnedGpuMorphCrowdEligibleMeshes = 0;
	uint64_t skinnedMaterializedMorphCpuSkippedMeshes = 0;
	const bool collectDetailedPrefabPerf = cm::debug::PrefabPerfDetailedTimingsEnabled();
	struct PrefabSkinningPerfSample {
		double TotalMs = 0.0;
		uint64_t Groups = 0;
		uint64_t Meshes = 0;
		uint64_t Bones = 0;
		uint64_t GroupsLodSkipped = 0;
		uint64_t PaletteSkipped = 0;
		uint64_t BlendshapeSkipped = 0;
	};
	std::unordered_map<EntityID, PrefabSkinningPerfSample> prefabSkinningPerf;
	if (collectDetailedPrefabPerf) {
		prefabSkinningPerf.reserve(16);
	}

	cm::physics::RagdollSystem* ragdollSystem = scene.m_IsPlaying ? cm::physics::GetRagdollSystem() : nullptr;
	const bool hasActiveRagdolls = ragdollSystem && ragdollSystem->HasActiveRagdolls();
	const bool gpuSharedSkeletonSupported = Renderer::Get().SupportsGpuSkinningAtlas();
	bgfx::ProgramHandle gpuMorphSkinnedPbrProgram = BGFX_INVALID_HANDLE;
	bool gpuMorphProgramsAvailable = false;
	if (gpuSharedSkeletonSupported) {
		gpuMorphSkinnedPbrProgram =
			ShaderManager::Instance().LoadProgram("vs_pbr_skinned", "fs_pbr_skinned");
		gpuMorphProgramsAvailable =
			bgfx::isValid(gpuMorphSkinnedPbrProgram) &&
			bgfx::isValid(ShaderManager::Instance().LoadProgram("vs_pbr_skinned_morph", "fs_pbr_skinned")) &&
			bgfx::isValid(ShaderManager::Instance().LoadProgram("vs_pbr_skinned_morph_instanced", "fs_pbr_skinned")) &&
			bgfx::isValid(ShaderManager::Instance().LoadProgram("vs_pbr_skinned_morph", "fs_object_id")) &&
			bgfx::isValid(ShaderManager::Instance().LoadProgram("vs_pbr_skinned_morph_object_id_instanced", "fs_object_id_instanced")) &&
			bgfx::isValid(ShaderManager::Instance().LoadProgram("vs_depth_skinned_morph", "fs_depth")) &&
			bgfx::isValid(ShaderManager::Instance().LoadProgram("vs_depth_skinned_morph_instanced", "fs_depth")) &&
			bgfx::isValid(ShaderManager::Instance().LoadProgram("vs_depth_skinned_morph", "fs_point_shadow_depth")) &&
			bgfx::isValid(ShaderManager::Instance().LoadProgram("vs_depth_skinned_morph_instanced", "fs_point_shadow_depth"));
	}
	// PERF: ArmorFit lives on armor *mesh* entities (renderable), never on bones.
	// Scanning scene.GetEntities() walked the entire bone-inflated entity list every
	// frame just to build a set that is empty unless modular armor is equipped. Iterate
	// the cached renderable-mesh list (O(meshes), excludes bones) with the same
	// all-entities fallback the rest of this function already uses when the runtime
	// bridge isn't populated. Same established pattern as the cached camera/light lists.
	std::unordered_set<EntityID> cpuMorphConsumerMeshEntities;
	for (EntityID entId : *renderableMeshEntityIds) {
		EntityData* data = scene.GetEntityData(entId);
		if (!data || !data->ArmorFit || data->ArmorFit->BodyEntity == INVALID_ENTITY_ID) {
			continue;
		}
		if (data->ArmorFit->WrapData && data->ArmorFit->WrapData->IsValid()) {
			cpuMorphConsumerMeshEntities.insert(data->ArmorFit->BodyEntity);
		}
	}
	for (EntityID meshEntityId : *skinnedMeshEntityIds) {
		EntityData* data = scene.GetEntityData(meshEntityId);
		if (!data || !data->Skinning) {
			continue;
		}
		data->Skinning->GpuMorphRuntimeAllowed = false;
		data->Skinning->GpuMorphRuntimeBatchSize = 0;
		data->Skinning->GpuMorphRuntimeBatchKey = 0;
	}

	// 1) Group skinned meshes by SkeletonRoot and collect per-skeleton data
	struct MeshWork {
		EntityID meshId;
		glm::mat4 skeletonToMesh;
		SkinningComponent* skin;
		uint32_t paletteBoneCount = 0; // Source index domain size for this mesh's vertex data
		bool presentationVisible = true;
		bool lodRelevant = true;
		bool skipPaletteThisFrame = false;
		// Blendshape CPU data
		Mesh* meshPtr = nullptr;
		BlendShapeComponent* bs = nullptr;
		bool needsBlend = false;
		bool isSkinnedVB = false;
		bool castsShadows = true;
		bool gpuMorphShadowCaster = false;
		bool hasCpuMorphConsumer = false;
		bool gpuMorphBaseCapable = false;
		bool gpuMorphRenderEligible = false;
		uint64_t gpuMorphBatchKey = 0;
	};
	struct SkelGroup {
		EntityID root;
		EntityData* skelData;
		SkeletonComponent* skel; // non-const to allow updating shared buffer
		const cm::world::RuntimeSkinningGroupCache* runtimeCache = nullptr;
		bool anyLodRelevant = false;
		bool anyGpuMorphShadowCaster = false;
		std::vector<MeshWork> meshes;
	};

	std::vector<SkelGroup> groups;
	struct NonSkinnedWork {
		EntityID meshId;
		Mesh* meshPtr = nullptr;
		BlendShapeComponent* bs = nullptr;
		bool needsBlend = false;
	};
	std::vector<NonSkinnedWork> nonSkinned;

	// Collect non-skinned meshes for blendshape-only updates.
	for (EntityID meshEntityId : *renderableMeshEntityIds) {
		EntityData* data = scene.GetEntityData(meshEntityId);
		if (!data || !data->Mesh || data->Skinning) continue;

		auto meshPtr = data->Mesh->mesh.get();
		const bool bsDirty =
			(data->BlendShapes && meshPtr && meshPtr->Dynamic && data->Mesh->BlendShapes && data->Mesh->BlendShapes->Dirty);
		if (!bsDirty) {
			continue;
		}

		NonSkinnedWork nw{};
		nw.meshId = meshEntityId;
		nw.meshPtr = meshPtr;
		nw.bs = data->BlendShapes.get();
		nw.needsBlend = true;
		nonSkinned.push_back(nw);
	}

	auto appendMeshToGroup = [&](SkelGroup& g, EntityID meshEntityId, EntityData* data, bool renderVisibleLastFrame) {
		(void)renderVisibleLastFrame;
		MeshWork w{};
		w.meshId = meshEntityId;
		w.skin = data->Skinning.get();
		w.presentationVisible = data->Active && data->Visible && !data->PresentationHidden;
		const glm::mat4& meshWorld = data->Transform.WorldMatrix;
		const glm::mat4& skeletonWorld = g.skelData->Transform.WorldMatrix;
		if (!w.skin->CachedMeshToSkeletonValid ||
			!MatrixNearlyEqual(w.skin->CachedMeshWorld, meshWorld) ||
			!MatrixNearlyEqual(w.skin->CachedSkeletonWorld, skeletonWorld)) {
			const bool skeletonSpaceCompatible = MatrixNearlyEqual(meshWorld, skeletonWorld);
			w.skin->CachedMeshWorld = meshWorld;
			w.skin->CachedSkeletonWorld = skeletonWorld;
			w.skin->CachedSkeletonSpaceCompatible = skeletonSpaceCompatible;
			w.skin->CachedSkeletonToMesh =
				skeletonSpaceCompatible ? glm::mat4(1.0f) : (glm::inverse(meshWorld) * skeletonWorld);
			w.skin->CachedMeshToSkeletonValid = true;
		}
		w.skeletonToMesh = w.skin->CachedSkeletonToMesh;

		// Blendshape info (dynamic meshes only)
		auto meshPtr = data->Mesh->mesh.get();
		w.meshPtr = meshPtr;
		w.bs = data->BlendShapes.get();
		w.needsBlend = false;
		w.isSkinnedVB = (w.meshPtr && w.meshPtr->HasSkinning());
		w.castsShadows = (!data->RenderOverrides || data->RenderOverrides->CastShadows);
		w.lodRelevant = w.presentationVisible;

		const bool usesCompactPalette = w.meshPtr && w.meshPtr->UsesCompactSkinningPalette();
		if (usesCompactPalette) {
			w.paletteBoneCount = static_cast<uint32_t>(w.meshPtr->SkinningBoneRemap.size());
		}
		else if (!w.skin->CachedMaxBoneIndexValid) {
			int maxBoneIndex = -1;
			if (w.meshPtr && !w.meshPtr->BoneIndices.empty()) {
				for (const auto& bi : w.meshPtr->BoneIndices) {
					maxBoneIndex = std::max(maxBoneIndex, bi.x);
					maxBoneIndex = std::max(maxBoneIndex, bi.y);
					maxBoneIndex = std::max(maxBoneIndex, bi.z);
					maxBoneIndex = std::max(maxBoneIndex, bi.w);
				}
			}
			w.skin->CachedMaxBoneIndex = maxBoneIndex;
			w.skin->CachedMaxBoneIndexValid = true;
		}
		if (!usesCompactPalette && w.skin->CachedMaxBoneIndexValid && w.skin->CachedMaxBoneIndex >= 0) {
			w.paletteBoneCount = static_cast<uint32_t>(w.skin->CachedMaxBoneIndex + 1);
		}

		// Keep the CPU morph path authoritative first. A later scoped pass may
		// opt this mesh into GPU morphing only after proving a real crowd batch
		// exists and no CPU deformation consumer needs current vertices.
		if (w.skin) {
			w.skin->GpuMorphRuntimeAllowed = false;
			w.skin->GpuMorphRuntimeBatchSize = 0;
			w.skin->GpuMorphRuntimeBatchKey = 0;
		}
		w.hasCpuMorphConsumer =
			(data->ArmorFit && data->ArmorFit->WrapData && data->ArmorFit->WrapData->IsValid()) ||
			cpuMorphConsumerMeshEntities.find(meshEntityId) != cpuMorphConsumerMeshEntities.end();
		if (w.hasCpuMorphConsumer && data->BlendShapes && meshPtr && meshPtr->Dynamic) {
			++skinnedGpuMorphCpuConsumerBlocked;
		}
		w.gpuMorphBaseCapable =
			!w.hasCpuMorphConsumer &&
			gpuMorphProgramsAvailable &&
			data->BlendShapes &&
			meshPtr &&
			meshPtr->Dynamic &&
			CanBootstrapGpuSharedSkeletonSourceForGpuMorph(data->Skinning.get(), meshPtr, w.paletteBoneCount) &&
			Renderer::Get().CanUseGpuMorphTargets(meshPtr, data->BlendShapes.get());
		if (w.gpuMorphBaseCapable) {
			++skinnedGpuMorphBaseCapableMeshes;
			if (TryBuildGpuMorphCrowdBatchKey(
					data,
					meshPtr,
					data->BlendShapes.get(),
					gpuMorphSkinnedPbrProgram,
					w.gpuMorphBatchKey)) {
				w.gpuMorphRenderEligible = true;
			}
			else {
				++skinnedGpuMorphRenderStateBlocked;
			}
		}
		const bool bsDirty =
			(data->BlendShapes &&
			 meshPtr &&
			 meshPtr->Dynamic &&
			 data->Mesh->BlendShapes &&
			 data->Mesh->BlendShapes->Dirty);
		w.needsBlend = bsDirty;
		w.gpuMorphShadowCaster =
			w.presentationVisible &&
			w.castsShadows &&
			w.meshPtr &&
			w.meshPtr->Dynamic &&
			w.bs &&
			Renderer::Get().CanUseGpuMorphTargets(w.meshPtr, w.bs);

		g.anyLodRelevant = g.anyLodRelevant || w.lodRelevant;
		g.anyGpuMorphShadowCaster = g.anyGpuMorphShadowCaster || w.gpuMorphShadowCaster;
		g.meshes.push_back(std::move(w));
		};

	const auto* runtimeSkinningGroups = runtimeWorld ? &runtimeWorld->GetSkinningGroupCaches() : nullptr;
	if (runtimeSkinningGroups && !runtimeSkinningGroups->empty()) {
		groups.reserve(runtimeSkinningGroups->size());
		for (const auto& cacheGroup : *runtimeSkinningGroups) {
			EntityData* skelData = scene.GetEntityData(cacheGroup.SkeletonSceneEntity);
			if (!skelData || !skelData->Skeleton) {
				continue;
			}

			SkelGroup g{};
			g.root = cacheGroup.SkeletonSceneEntity;
			g.skelData = skelData;
			g.skel = skelData->Skeleton.get();
			g.runtimeCache = &cacheGroup;
			g.meshes.reserve(cacheGroup.MeshSceneEntities.size());

			const bool renderVisibleLastFrame = g.skel->LodMeshVisibleLastFrame;
			for (EntityID meshEntityId : cacheGroup.MeshSceneEntities) {
				EntityData* data = scene.GetEntityData(meshEntityId);
				if (!data || !data->Mesh || !data->Skinning) {
					continue;
				}
				appendMeshToGroup(g, meshEntityId, data, renderVisibleLastFrame);
			}

			if (!g.meshes.empty()) {
				groups.push_back(std::move(g));
			}
		}
	}
	else {
		std::unordered_map<EntityID, size_t> groupIndices;
		groupIndices.reserve(skinnedMeshEntityIds->size());

		// Fallback path when runtime-world caches are unavailable.
		for (EntityID meshEntityId : *skinnedMeshEntityIds) {
			EntityData* data = scene.GetEntityData(meshEntityId);
			if (!data || !data->Mesh || !data->Skinning) continue;

			EntityID root = ResolveSkeletonRoot(scene, meshEntityId, data->Skinning.get());
			if (root == INVALID_ENTITY_ID) continue;
			EntityData* skelData = scene.GetEntityData(root);
			if (!skelData || !skelData->Skeleton) continue;

			auto [it, inserted] = groupIndices.emplace(root, groups.size());
			if (inserted) {
				SkelGroup g{};
				g.root = root;
				g.skelData = skelData;
				g.skel = skelData->Skeleton.get();
				groups.push_back(std::move(g));
			}

			appendMeshToGroup(groups[it->second], meshEntityId, data, skelData->Skeleton->LodMeshVisibleLastFrame);
		}
	}

	auto resolveGroupWorldBounds = [&](const SkelGroup& g, glm::vec3& outMin, glm::vec3& outMax) -> bool {
		if (g.meshes.empty()) {
			return false;
		}

		if (hasActiveRagdolls && ragdollSystem) {
			glm::vec3 skeletonMin(0.0f);
			glm::vec3 skeletonMax(0.0f);
			const bool haveRagdollBounds =
				ragdollSystem->TryGetActiveSkeletonBounds(g.root, skeletonMin, skeletonMax) ||
				(ragdollSystem->IsSkeletonRagdollActive(g.root) &&
					ComputeSkeletonWorldBounds(scene, g.root, skeletonMin, skeletonMax));
			if (haveRagdollBounds) {
				glm::vec3 maxExtent(0.0f);
				bool haveExtent = false;
				if (runtimeWorld && g.runtimeCache) {
					haveExtent = runtimeWorld->TryGetSkinningGroupMaxWorldExtent(*g.runtimeCache, maxExtent);
				}
				if (!haveExtent) {
					for (const auto& meshWork : g.meshes) {
						const EntityData* meshData = scene.GetEntityData(meshWork.meshId);
						if (!meshData || !meshWork.meshPtr) {
							continue;
						}
						maxExtent = glm::max(maxExtent, ComputeMeshWorldExtents(meshData, meshWork.meshPtr));
						haveExtent = true;
					}
				}

				outMin = skeletonMin - maxExtent;
				outMax = skeletonMax + maxExtent;
				return true;
			}
		}

		if (runtimeWorld && g.runtimeCache &&
			runtimeWorld->TryGetSkinningGroupWorldBounds(*g.runtimeCache, outMin, outMax)) {
			return true;
		}

		glm::vec3 minBounds(std::numeric_limits<float>::max());
		glm::vec3 maxBounds(-std::numeric_limits<float>::max());
		bool hasBounds = false;
		for (const auto& meshWork : g.meshes) {
			const EntityData* meshData = scene.GetEntityData(meshWork.meshId);
			if (!meshData || !meshData->Mesh || !meshWork.meshPtr) {
				continue;
			}

			const glm::vec3 lmin = meshWork.meshPtr->BoundsMin;
			const glm::vec3 lmax = meshWork.meshPtr->BoundsMax;
			const glm::vec3 lcenter = (lmin + lmax) * 0.5f;
			float lodBoundsPadding = std::max(0.01f, meshData->Mesh->BoundsPadding);
			if (meshData->Mesh->SkipFrustumCulling) {
				// Rendering may bypass frustum culling for stability, but the LOD
				// system still needs an independent visibility estimate so crowd
				// budgets do not treat the actor as permanently on-screen.
				lodBoundsPadding = std::max(lodBoundsPadding, 2.75f);
			}
			const glm::vec3 lextents = (lmax - lmin) * 0.5f * lodBoundsPadding;
			if (lextents.x <= 1e-5f && lextents.y <= 1e-5f && lextents.z <= 1e-5f) {
				continue;
			}

			const glm::mat4& meshWorld = meshData->Transform.WorldMatrix;
			const glm::vec3 worldCenter = glm::vec3(meshWorld * glm::vec4(lcenter, 1.0f));
			glm::vec3 ex;
			ex.x = std::abs(meshWorld[0][0]) * lextents.x + std::abs(meshWorld[1][0]) * lextents.y + std::abs(meshWorld[2][0]) * lextents.z;
			ex.y = std::abs(meshWorld[0][1]) * lextents.x + std::abs(meshWorld[1][1]) * lextents.y + std::abs(meshWorld[2][1]) * lextents.z;
			ex.z = std::abs(meshWorld[0][2]) * lextents.x + std::abs(meshWorld[1][2]) * lextents.y + std::abs(meshWorld[2][2]) * lextents.z;
			minBounds = glm::min(minBounds, worldCenter - ex);
			maxBounds = glm::max(maxBounds, worldCenter + ex);
			hasBounds = true;
		}

		if (!hasBounds) {
			return false;
		}

		outMin = minBounds;
		outMax = maxBounds;
		return true;
	};

	for (auto& g : groups) {
		g.anyLodRelevant = false;
		const cm::npc::ScalabilityState* scalability =
			g.skelData ? &g.skelData->NpcScalability : nullptr;
		const bool groupRelevant =
			(scalability && scalability->Participates)
				? scalability->WantsSkinningWork()
				: true;

		for (auto& meshWork : g.meshes) {
			if (!meshWork.presentationVisible) {
				meshWork.lodRelevant = false;
				continue;
			}

			meshWork.lodRelevant = groupRelevant;
			g.anyLodRelevant = g.anyLodRelevant || meshWork.lodRelevant;
		}
	}

	std::unordered_map<uint64_t, uint32_t> gpuMorphBatchCounts;
	for (const auto& g : groups) {
		for (const auto& meshWork : g.meshes) {
			if (!meshWork.gpuMorphRenderEligible ||
				!meshWork.lodRelevant ||
				meshWork.gpuMorphBatchKey == 0) {
				continue;
			}
			++gpuMorphBatchCounts[meshWork.gpuMorphBatchKey];
		}
	}

	for (auto& g : groups) {
		for (auto& meshWork : g.meshes) {
			if (!meshWork.gpuMorphRenderEligible || !meshWork.skin) {
				continue;
			}

			const auto batchIt = gpuMorphBatchCounts.find(meshWork.gpuMorphBatchKey);
			const uint32_t batchSize =
				batchIt != gpuMorphBatchCounts.end() ? batchIt->second : 0u;
			if (meshWork.lodRelevant &&
				batchSize >= kGpuMorphCrowdMinBatchSize) {
				meshWork.skin->GpuMorphRuntimeAllowed = true;
				meshWork.skin->GpuMorphRuntimeBatchSize = batchSize;
				meshWork.skin->GpuMorphRuntimeBatchKey = meshWork.gpuMorphBatchKey;
				++skinnedGpuMorphCrowdEligibleMeshes;
				if (CanSkipCpuBlendshapeUpdateForGpuMorph(
						scene.GetEntityData(meshWork.meshId),
						meshWork.meshPtr,
						meshWork.paletteBoneCount)) {
					meshWork.needsBlend = false;
					++skinnedGpuMorphCandidateMeshes;
				}
			}
			else {
				if (meshWork.lodRelevant) {
					++skinnedGpuMorphBatchTooSmall;
				}
			}
		}
	}

	for (auto& g : groups) {
		for (auto& meshWork : g.meshes) {
			if (!meshWork.skin || !meshWork.meshPtr || !meshWork.bs || !meshWork.meshPtr->Dynamic) {
				continue;
			}

			EntityData* meshData = scene.GetEntityData(meshWork.meshId);
			if (!meshData ||
				!CanEnableGpuMorphForMaterializedSkinning(meshData, meshWork.meshPtr)) {
				continue;
			}

			const bool hasActiveMaterializedMorphWeights =
				meshWork.bs->CountActiveShapes(kMaterializedMorphWeightThreshold) > 0u;
			if (!hasActiveMaterializedMorphWeights) {
				meshWork.skin->GpuMorphRuntimeAllowed = true;
				meshWork.skin->GpuMorphRuntimeBatchSize =
					std::max(meshWork.skin->GpuMorphRuntimeBatchSize, 1u);
			}

			if (!hasActiveMaterializedMorphWeights &&
				!meshWork.hasCpuMorphConsumer &&
				meshWork.needsBlend) {
				meshWork.needsBlend = false;
				++skinnedMaterializedMorphCpuSkippedMeshes;
			}
		}
	}

	// 2) For each skeleton group, compute pose ONCE and store in shared skeleton buffer
	for (auto& g : groups) {
		const auto prefabPerfGroupStart = collectDetailedPrefabPerf
			? std::chrono::high_resolution_clock::now()
			: std::chrono::high_resolution_clock::time_point{};
		uint64_t groupPaletteSkipped = 0;
		uint64_t groupBlendshapeSkipped = 0;
		uint64_t groupLodSkipped = 0;
		++skelGroupsTotal;
		const size_t boneCountRaw = std::min(g.skel->InverseBindPoses.size(), g.skel->BoneEntities.size());
		if (boneCountRaw == 0) continue;
		const size_t boneCount = boneCountRaw;
		g.skel->BoneCount = static_cast<uint32_t>(boneCount);
		g.skel->EnsureRuntimeBonePaletteSize(boneCount);
		if (g.anyLodRelevant) {
			++skelGroupsLodRelevant;
		}

		const auto clearGpuSharedSkeletonSource = [&](auto& meshWork) {
			meshWork.skin->ResetGpuSharedSkeletonSource();
			};

		const auto clearGpuRetargetData = [&](auto& meshWork) {
			meshWork.skin->ClearGpuRetargetData();
			};

		const auto needsDynamicGpuRetarget = [&](const auto& meshWork) -> bool {
			return meshWork.skin &&
				meshWork.skin->UseParentSkeleton &&
				!meshWork.skin->BoneRemap.empty() &&
				!meshWork.skin->SkeletonsIdentical &&
				!meshWork.skin->UsesFullSkeletonIndices;
			};

		const auto buildGpuRetargetData = [&](auto& meshWork) -> bool {
			if (!needsDynamicGpuRetarget(meshWork)) {
				clearGpuRetargetData(meshWork);
				return false;
			}

			if (!meshWork.meshPtr ||
				meshWork.paletteBoneCount == 0 ||
				meshWork.paletteBoneCount > SkinningComponent::MaxDirectVertexBones) {
				clearGpuRetargetData(meshWork);
				return false;
			}

			const size_t expectedCount = static_cast<size_t>(meshWork.paletteBoneCount);
			if (meshWork.skin->RemapBuiltForSkeleton == g.root &&
				meshWork.skin->GpuBoneIndexRemap.size() == expectedCount &&
				(meshWork.skin->GpuBoneCorrectionPalette.empty() ||
					meshWork.skin->GpuBoneCorrectionPalette.size() == expectedCount)) {
				return true;
			}

			const bool useCompactPalette = meshWork.meshPtr->UsesCompactSkinningPalette();
			const std::vector<uint16_t>* compactRemap =
				useCompactPalette ? &meshWork.meshPtr->SkinningBoneRemap : nullptr;

			auto& remap = meshWork.skin->GpuBoneIndexRemap;
			auto& corrections = meshWork.skin->GpuBoneCorrectionPalette;
			remap.resize(meshWork.paletteBoneCount, 0u);
			corrections.resize(meshWork.paletteBoneCount, glm::mat4(1.0f));
			meshWork.skin->InvalidateGpuRetargetHashes();

			const glm::mat4 identity(1.0f);
			bool hasNonIdentityCorrection = false;
			for (uint32_t slot = 0; slot < meshWork.paletteBoneCount; ++slot) {
				const size_t originalBoneIndex =
					useCompactPalette ? static_cast<size_t>((*compactRemap)[slot]) : static_cast<size_t>(slot);

				uint16_t mappedIndex =
					(originalBoneIndex < boneCount) ? static_cast<uint16_t>(originalBoneIndex) : 0u;
				glm::mat4 correction = identity;

				if (originalBoneIndex < meshWork.skin->BoneRemap.size()) {
					const int targetIdx = meshWork.skin->BoneRemap[originalBoneIndex];
					if (targetIdx >= 0 && targetIdx < static_cast<int>(boneCount)) {
						mappedIndex = static_cast<uint16_t>(targetIdx);

						if (originalBoneIndex < meshWork.skin->OriginalInverseBindPoses.size() &&
							targetIdx < static_cast<int>(g.skel->InverseBindPoses.size())) {
							const glm::mat4& parentInvBind = g.skel->InverseBindPoses[targetIdx];
							const glm::mat4& armorInvBind = meshWork.skin->OriginalInverseBindPoses[originalBoneIndex];
							correction = glm::inverse(parentInvBind) * armorInvBind;
							hasNonIdentityCorrection =
								hasNonIdentityCorrection || !MatrixNearlyEqual(correction, identity);
						}
					}
				}

				remap[slot] = mappedIndex;
				corrections[slot] = correction;
			}

			if (!hasNonIdentityCorrection) {
				corrections.clear();
			}
			meshWork.skin->InvalidateGpuRetargetHashes();
			return true;
			};

		for (auto& w : g.meshes) {
			if (!w.skin->UseParentSkeleton || w.skin->OriginalBoneNames.empty()) {
				clearGpuRetargetData(w);
				continue;
			}

			if (w.skin->RemapBuiltForSkeleton != g.root) {
				BuildBoneRemap(w.skin, g.skel);
				w.skin->RemapBuiltForSkeleton = g.root;

				if (w.meshPtr && !w.meshPtr->BoneIndices.empty()) {
					int maxVertexBoneIdx = 0;
					if (w.meshPtr->UsesCompactSkinningPalette()) {
						for (uint16_t originalBoneIdx : w.meshPtr->SkinningBoneRemap) {
							maxVertexBoneIdx = std::max(maxVertexBoneIdx, static_cast<int>(originalBoneIdx));
						}
					}
					else {
						for (const auto& bi : w.meshPtr->BoneIndices) {
							maxVertexBoneIdx = std::max(maxVertexBoneIdx, bi.x);
							maxVertexBoneIdx = std::max(maxVertexBoneIdx, bi.y);
							maxVertexBoneIdx = std::max(maxVertexBoneIdx, bi.z);
							maxVertexBoneIdx = std::max(maxVertexBoneIdx, bi.w);
						}
					}

					if (maxVertexBoneIdx >= static_cast<int>(w.skin->OriginalBoneNames.size())) {
						w.skin->UsesFullSkeletonIndices = true;
					}
					else {
						w.skin->UsesFullSkeletonIndices = false;
					}
				}

				RegisterArmorBlendShapesWithUnifiedMorph(scene, w.meshId, g.root);
			}

			if (!buildGpuRetargetData(w)) {
				clearGpuRetargetData(w);
			}
		}

		const auto canUseGpuSharedSkeletonBootstrap = [&](const auto& meshWork) -> bool {
			if (!gpuSharedSkeletonSupported ||
				!meshWork.presentationVisible ||
				!meshWork.skin) {
				return false;
			}

			if (needsDynamicGpuRetarget(meshWork)) {
				const size_t expected = static_cast<size_t>(meshWork.paletteBoneCount);
				if (expected == 0) {
					return false;
				}
				if (meshWork.skin->GpuBoneIndexRemap.size() != expected) {
					return false;
				}
				if (!meshWork.skin->GpuBoneCorrectionPalette.empty() &&
					meshWork.skin->GpuBoneCorrectionPalette.size() != expected) {
					return false;
				}
				return true;
			}

			if (meshWork.meshPtr && meshWork.meshPtr->UsesCompactSkinningPalette()) {
				return true;
			}

			return meshWork.paletteBoneCount > 0 &&
				meshWork.paletteBoneCount <= SkinningComponent::MaxDirectVertexBones;
			};

		const auto canUseGpuSharedSkeletonSource = [&](const auto& meshWork) -> bool {
			return canUseGpuSharedSkeletonBootstrap(meshWork);
			};

		const auto enableGpuSharedSkeletonSource = [&](auto& meshWork) {
			meshWork.skin->GpuSourceSkeleton = g.skel;
			meshWork.skin->GpuMeshFromSkeleton = meshWork.skeletonToMesh;
			meshWork.skin->UseGpuSharedSkeletonSource = true;
			meshWork.skin->BoneCount = (meshWork.paletteBoneCount > 0)
				? meshWork.paletteBoneCount
				: g.skel->BoneCount;
			};

		const bool skeletonPoseReady =
			g.skel->BoneCount == static_cast<uint32_t>(boneCount) &&
			g.skel->BonePalette.size() >= boneCount &&
			(g.skel->AnimatedPosePaletteValid || g.skel->PoseFrameId != 0);
		bool needsBootstrap = !skeletonPoseReady;
		if (!needsBootstrap) {
			for (const auto& w : g.meshes) {
				if (!w.presentationVisible) {
					continue;
				}
				if (!canUseGpuSharedSkeletonBootstrap(w)) {
					needsBootstrap = true;
					break;
				}
			}
		}

		bool anyPaletteConsumersThisFrame = needsBootstrap;
		for (auto& w : g.meshes) {
			if (!w.presentationVisible) {
				w.skipPaletteThisFrame = true;
				clearGpuSharedSkeletonSource(w);
				continue;
			}
			w.skipPaletteThisFrame = !needsBootstrap && !w.lodRelevant;
			if (w.skipPaletteThisFrame) {
				++skinnedPaletteSkipped;
				++groupPaletteSkipped;
				if (canUseGpuSharedSkeletonBootstrap(w)) {
					enableGpuSharedSkeletonSource(w);
				}
			}
			else {
				anyPaletteConsumersThisFrame = true;
			}
		}

		bool shouldUpdatePose = true;
		bool noPaletteConsumersThisFrame = false;
		auto* animationPlayer = g.skelData->AnimationPlayer.get();
		const cm::npc::ScalabilityState* scalability =
			(scene.m_IsPlaying && g.skelData)
				? &g.skelData->NpcScalability
				: nullptr;
		const bool activeRagdoll = hasActiveRagdolls && ragdollSystem->IsSkeletonRagdollActive(g.root);
		const bool animatorDisabled = animationPlayer && !animationPlayer->Enabled;
		const bool animationLodEnabled =
			animationPlayer == nullptr || animationPlayer->LODEnabled;
		const bool hasPostAnimationConstraints =
			HasActivePostAnimationConstraint(*g.skelData);
		if (hasPostAnimationConstraints) {
			++skelGroupsConstraintOwned;
		}
		// A disabled animator pauses evaluation, but skinning should still reuse
		// the last valid animation pose until another system takes ownership.
		// Runtime-pose constraints rebuild this same palette. If a constraint
		// had to fall back to materialized bone entities, let skinning rebuild
		// from those transforms instead.
		const bool constraintsUseRuntimePose =
			!hasPostAnimationConstraints ||
			((g.skel->RuntimeLocalPoseValid &&
				g.skel->RuntimeLocalPose.size() >= boneCount) ||
			 (g.skel->RuntimeLocalPoseTrsValid &&
			  g.skel->RuntimeLocalTranslations.size() >= boneCount &&
			  g.skel->RuntimeLocalRotations.size() >= boneCount &&
			  g.skel->RuntimeLocalScales.size() >= boneCount));
		const bool animationPoseAuthoritative =
			animationPlayer &&
			animationPlayer->Controller != nullptr &&
			!activeRagdoll &&
			constraintsUseRuntimePose;
		const bool dormantOffscreenIdle = animationPlayer &&
			animationPlayer->DormantOffscreenIdle;
		const bool participatesInScalability =
			scalability && scalability->Participates;
		const float crowdDistance = participatesInScalability
			? std::max(0.0f, scalability->CameraDistance)
			: 0.0f;
		const float crowdDistSq = crowdDistance * crowdDistance;
		g.skel->LodLastDistance = crowdDistance;
		if (!animationPoseAuthoritative) {
			g.skel->AnimatedPosePaletteValid = false;
		}
		if (!needsBootstrap &&
			dormantOffscreenIdle &&
			participatesInScalability &&
			scalability->IsDormant() &&
			!g.anyLodRelevant) {
			shouldUpdatePose = false;
			g.skel->LodAccumulatedTime = 0.0f;
			++skelGroupsDormantSkipped;
		}
		else if (!needsBootstrap && !animationLodEnabled) {
			g.skel->LodAccumulatedTime = 0.0f;
		}
		else if (!needsBootstrap && g.skel->LODEnabled && participatesInScalability) {
			const float distSq = crowdDistSq;
			const float nearSq = g.skel->LODNearDistance * g.skel->LODNearDistance;
			const float mediumSq = g.skel->LODMediumDistance * g.skel->LODMediumDistance;
			const float farSq = g.skel->LODFarDistance * g.skel->LODFarDistance;
			float updateInterval = std::max(0.0f, scalability->AnimationUpdateInterval);

			if (activeRagdoll && !g.anyLodRelevant) {
				float ragdollInterval = 0.0f;
				if (distSq < nearSq) {
					ragdollInterval = 0.133f;
				}
				else if (distSq < mediumSq) {
					ragdollInterval = 0.250f;
				}
				else if (distSq < farSq) {
					ragdollInterval = 0.333f;
				}
				else {
					ragdollInterval = 0.500f;
				}

				if (ragdollInterval > updateInterval) {
					updateInterval = ragdollInterval;
					++skelGroupsRagdollThrottled;
				}
			}

			if (animatorDisabled) {
				if (distSq < nearSq) {
					updateInterval = std::max(updateInterval, 0.033f);
				}
				else if (distSq < mediumSq) {
					updateInterval = std::max(updateInterval, 0.067f);
				}
				else if (distSq < farSq) {
					updateInterval = std::max(updateInterval, 0.133f);
				}
				else {
					updateInterval = std::max(updateInterval, 0.200f);
				}
			}

			if (scalability->CrowdThrottled) {
				++skelGroupsCrowdThrottled;
			}

			if (updateInterval > 0.0f) {
				g.skel->LodAccumulatedTime += dt;
				if (g.skel->LodAccumulatedTime < updateInterval) {
					shouldUpdatePose = false;
				}
				else {
					g.skel->LodAccumulatedTime = 0.0f;
				}
			}
			else {
				g.skel->LodAccumulatedTime = 0.0f;
			}
		}

		if (!anyPaletteConsumersThisFrame) {
			shouldUpdatePose = false;
			noPaletteConsumersThisFrame = true;
			g.skel->LodAccumulatedTime = 0.0f;
		}

		if (shouldUpdatePose) {
			++skelGroupsUpdated;

			const bool canReuseAnimatedPosePalette =
				animationPoseAuthoritative &&
				g.skel->AnimatedPosePaletteValid &&
				g.skel->BoneCount == static_cast<uint32_t>(boneCount);

			if (canReuseAnimatedPosePalette) {
				++skelGroupsAnimatedPaletteReused;
				if (animatorDisabled) {
					++skelGroupsDisabledPoseReused;
				}
			}

			if (!canReuseAnimatedPosePalette) {
				const bool canReuseRuntimeGlobals =
					g.skel->RuntimeBoneGlobalsValid &&
					g.skel->RuntimeBoneGlobals.size() >= boneCount;
				if (canReuseRuntimeGlobals) {
					// PERF: run the per-group pose palette inline (no job dispatch). boneCount is
				// small (~tens), and the outer group loop is serial, so dispatching a 1-2 chunk
				// parallel_for here per group per frame is pure barrier overhead. Setting chunk
				// >= boneCount takes parallel_for's inline fast-path (total <= chunk).
				const size_t chunkPose = std::max<size_t>(boneCount, size_t{ 1 });
					parallel_for(Jobs(), size_t{ 0 }, boneCount, chunkPose, [&](size_t s, size_t c) {
						for (size_t i = s; i < s + c; ++i) {
							g.skel->BonePalette[i] =
								g.skel->RuntimeBoneGlobals[i] * g.skel->InverseBindPoses[i];
						}
					});
					g.skel->PoseFrameId =
						g.skel->RuntimeBoneGlobalsFrameId != 0
							? g.skel->RuntimeBoneGlobalsFrameId
							: s_FrameCounter;
					g.skel->AnimatedPosePaletteValid = false;
				}
				else {
					// Fallback: rebuild from the authoritative bone entities when the
					// pose comes from ragdoll or another non-runtime-buffer source.
					std::vector<glm::mat4> boneWorld(boneCount);
					const glm::mat4 skeletonWorld = g.skelData->Transform.WorldMatrix;
					const glm::mat4 invSkeletonWorld = glm::inverse(skeletonWorld);
					const bool useRuntimeBoneCache =
						runtimeWorld &&
						g.runtimeCache &&
						g.runtimeCache->BoneHandles.size() >= boneCount;

					// Debug: Check for missing bone entities (single-threaded for logging)
					static bool s_LoggedBoneEntities = false;
					int missingCount = 0;
					for (size_t i = 0; i < boneCount; ++i) {
						const EntityID be = g.skel->BoneEntities[i];
						const glm::mat4* cachedBoneWorld = nullptr;
						if (useRuntimeBoneCache) {
							cachedBoneWorld = runtimeWorld->TryGetWorldMatrix(g.runtimeCache->BoneHandles[i]);
						}

						if (cachedBoneWorld) {
							boneWorld[i] = *cachedBoneWorld;
						}
						else if (const EntityData* bd = scene.GetEntityData(be)) {
							boneWorld[i] = bd->Transform.WorldMatrix;
						}
						else {
							++missingCount;
							if (!s_LoggedBoneEntities && i < g.skel->BoneNames.size()) {
								std::cerr << "[SkinningSystem] WARNING: Bone entity missing for [" << i << "] "
									<< g.skel->BoneNames[i] << " (entity=" << be << ")" << std::endl;
							}
							if (i < g.skel->BindPoseGlobals.size()) boneWorld[i] = g.skel->BindPoseGlobals[i];
							else boneWorld[i] = glm::inverse(g.skel->InverseBindPoses[i]);
						}
					}
					if (missingCount > 0 && !s_LoggedBoneEntities) {
						std::cerr << "[SkinningSystem] Total missing bone entities: " << missingCount << "/" << boneCount << std::endl;
						s_LoggedBoneEntities = true;
					}

					// Compute final pose once in skeleton-local space. Meshes authored in the
					// skeleton root's space can bind this palette directly without any per-mesh
					// matrix work, while offset meshes can derive their palette from the same
					// shared data via a single skeleton-to-mesh matrix.
					// PERF: run the per-group pose palette inline (no job dispatch). boneCount is
				// small (~tens), and the outer group loop is serial, so dispatching a 1-2 chunk
				// parallel_for here per group per frame is pure barrier overhead. Setting chunk
				// >= boneCount takes parallel_for's inline fast-path (total <= chunk).
				const size_t chunkPose = std::max<size_t>(boneCount, size_t{ 1 });
					parallel_for(Jobs(), size_t{ 0 }, boneCount, chunkPose, [&](size_t s, size_t c) {
						for (size_t i = s; i < s + c; ++i) {
							g.skel->BonePalette[i] = invSkeletonWorld * boneWorld[i] * g.skel->InverseBindPoses[i];
						}
					});
					g.skel->PoseFrameId = s_FrameCounter;
					g.skel->AnimatedPosePaletteValid = false;
				}
			}

			// PERF: for small per-group mesh counts (the common case), run inline rather than
			// dispatching one tiny job per mesh (chunk 1). Large groups still parallelize.
			const size_t meshChunkSize = (g.meshes.size() >= 32) ? size_t{ 8 } : std::max<size_t>(g.meshes.size(), size_t{ 1 });
			parallel_for(Jobs(), size_t{ 0 }, g.meshes.size(), meshChunkSize,
				[&](size_t mStart, size_t mCount) {
					for (size_t m = mStart; m < mStart + mCount; ++m) {
						auto& w = g.meshes[m];
						if (w.skipPaletteThisFrame) {
							continue;
						}

						if (canUseGpuSharedSkeletonSource(w)) {
							enableGpuSharedSkeletonSource(w);
						}
						else {
							clearGpuSharedSkeletonSource(w);
							w.skin->BoneCount = 0;
						}
					}
				});

			// Update per-entity GPU skinning metadata for the renderer's atlas path.
			for (auto& w : g.meshes) {
				if (!w.presentationVisible) {
					continue;
				}
				if (w.skin->UsesGpuSharedSkeleton()) {
					++skinnedGpuSharedSourceMeshes;
					if (w.skin->GpuRemapAtlasCount > 0 || !w.skin->GpuBoneIndexRemap.empty()) {
						++skinnedGpuRemapMeshes;
					}
					if (w.skin->GpuCorrectionAtlasCount > 0 || !w.skin->GpuBoneCorrectionPalette.empty()) {
						++skinnedGpuCorrectionMeshes;
					}
				}
			}
		}
		else {
			if (noPaletteConsumersThisFrame) {
				++skelGroupsNoConsumerSkipped;
			}
			else {
				++skelGroupsLodSkipped;
			}
			++groupLodSkipped;
			// Keep prior GPU skinning metadata for distant skeletons and avoid costly recomputation.
			for (auto& w : g.meshes) {
				if (!w.presentationVisible) {
					continue;
				}
				if (w.skin->BoneCount == 0) {
					if (w.skin->UsesGpuSharedSkeleton()) {
						const uint32_t fallbackBoneCount =
							(w.paletteBoneCount > 0)
							? w.paletteBoneCount
							: g.skel->BoneCount;
						w.skin->BoneCount = fallbackBoneCount;
					}
					else {
						const uint32_t fallbackBoneCount =
							(w.paletteBoneCount > 0)
							? w.paletteBoneCount
							: g.skel->BoneCount;
						w.skin->BoneCount = fallbackBoneCount;
					}
				}
				if (w.skin->UsesGpuSharedSkeleton()) {
					++skinnedGpuSharedSourceMeshes;
					if (w.skin->GpuRemapAtlasCount > 0 || !w.skin->GpuBoneIndexRemap.empty()) {
						++skinnedGpuRemapMeshes;
					}
					if (w.skin->GpuCorrectionAtlasCount > 0 || !w.skin->GpuBoneCorrectionPalette.empty()) {
						++skinnedGpuCorrectionMeshes;
					}
				}
			}
		}

		// 3) Blendshapes per mesh (dynamic). Parallelize per-mesh compute; keep GPU updates on main thread.
		//    Optimized: uses sparse accumulation and reuses scratch buffers from BlendShapeComponent.
		{
			// Build a list of work items requiring blending this frame
			struct BlendWork {
				Mesh* meshPtr;
				BlendShapeComponent* bs;
				bool isSkinnedVB;
				size_t vertexStride = 0;
				size_t bufferSize = 0;
			};
			std::vector<BlendWork> blendWorks;
			blendWorks.reserve(g.meshes.size());

			for (auto& w : g.meshes) {
				if (w.skipPaletteThisFrame) {
					if (w.needsBlend) {
						++skinnedBlendshapeSkipped;
						++groupBlendshapeSkipped;
					}
					continue;
				}
				if (!w.meshPtr || !w.needsBlend) continue;
				const size_t vCountBase = w.meshPtr->Vertices.size();
				if (vCountBase == 0) { w.bs->Dirty = false; continue; }
				const size_t vSize = w.isSkinnedVB ? sizeof(SkinnedPBRVertex) : sizeof(PBRVertex);
				w.bs->EnsureBlendOutput(vCountBase, vSize);
				BlendWork bw{ w.meshPtr, w.bs, w.isSkinnedVB, vSize, vCountBase * vSize };
				const size_t vCount = vCountBase;
				blendWorks.push_back(std::move(bw));
			}

			// Compute blend on worker threads per mesh
			const size_t chunk = (blendWorks.size() >= 32) ? size_t{ 8 } : size_t{ 1 };
			parallel_for(Jobs(), size_t(0), blendWorks.size(), chunk, [&](size_t start, size_t count) {
				for (size_t i = 0; i < count; ++i) {
					BlendWork& bw = blendWorks[start + i];
					Mesh* meshPtr = bw.meshPtr; BlendShapeComponent* bsc = bw.bs;
					const size_t vCount = meshPtr->Vertices.size();
					if (vCount == 0) { bsc->Dirty = false; continue; }

					// Use scratch buffers from component (avoids per-frame allocations)
					bsc->EnsureScratchBuffers(vCount);
					glm::vec3* accDP = bsc->AccDeltaPos.data();
					glm::vec3* accDN = bsc->AccDeltaNorm.data();

					// Check if any shape is sparse
					bool anySparse = false;
					for (const auto& shape : bsc->Shapes) {
						if (shape.IsSparse && shape.Weight != 0.0f) { anySparse = true; break; }
					}

					if (anySparse) {
						// Sparse path: clear only touched vertices from previous frame
						bsc->ClearTouchedOnly();

						// Accumulate deltas for sparse shapes (only touch affected vertices)
						for (const auto& shape : bsc->Shapes) {
							const float weight = shape.Weight; if (weight == 0.0f) continue;

							if (shape.IsSparse) {
								// Sparse accumulation - only touch indexed vertices
								for (size_t si = 0; si < shape.SparseIndices.size(); ++si) {
									const uint32_t idx = shape.SparseIndices[si];
									if (idx < vCount) {
										accDP[idx] += shape.SparseDeltaPos[si] * weight;
										accDN[idx] += shape.SparseDeltaNorm[si] * weight;
									}
								}
								// Track touched indices for next frame's clear
								for (uint32_t idx : shape.SparseIndices) {
									if (idx < vCount) bsc->TouchedIndices.push_back(idx);
								}
							}
							else {
								// Dense fallback for mixed mode
								if (shape.DeltaPos.size() != vCount) continue;
								for (size_t vi = 0; vi < vCount; ++vi) {
									accDP[vi] += shape.DeltaPos[vi] * weight;
									if (vi < shape.DeltaNormal.size()) {
										accDN[vi] += shape.DeltaNormal[vi] * weight;
									}
								}
							}
						}
					}
					else {
						// Dense path: clear all accumulators
						bsc->ClearAccumulators();

						for (const auto& shape : bsc->Shapes) {
							const float weight = shape.Weight; if (weight == 0.0f) continue;
							if (shape.DeltaPos.size() != vCount) continue;
							for (size_t vi = 0; vi < vCount; ++vi) {
								accDP[vi] += shape.DeltaPos[vi] * weight;
								if (vi < shape.DeltaNormal.size()) {
									accDN[vi] += shape.DeltaNormal[vi] * weight;
								}
							}
						}
					}

					if (bw.isSkinnedVB) {
						auto* out = reinterpret_cast<SkinnedPBRVertex*>(bsc->BlendedVertices.data());
						for (size_t vi = 0; vi < vCount; ++vi) {
							out[vi].x = meshPtr->Vertices[vi].x + accDP[vi].x;
							out[vi].y = meshPtr->Vertices[vi].y + accDP[vi].y;
							out[vi].z = meshPtr->Vertices[vi].z + accDP[vi].z;
							out[vi].nx = meshPtr->Normals[vi].x + accDN[vi].x;
							out[vi].ny = meshPtr->Normals[vi].y + accDN[vi].y;
							out[vi].nz = meshPtr->Normals[vi].z + accDN[vi].z;
							if (vi < meshPtr->UVs.size()) { out[vi].u = meshPtr->UVs[vi].x; out[vi].v = meshPtr->UVs[vi].y; }
							else { out[vi].u = 0.0f; out[vi].v = 0.0f; }
							const glm::ivec4 bi = (vi < meshPtr->BoneIndices.size()) ? meshPtr->BoneIndices[vi] : glm::ivec4(0);
							out[vi].i0 = (uint8_t)bi.x; out[vi].i1 = (uint8_t)bi.y; out[vi].i2 = (uint8_t)bi.z; out[vi].i3 = (uint8_t)bi.w;
							const glm::vec4 bwg = (vi < meshPtr->BoneWeights.size()) ? meshPtr->BoneWeights[vi] : glm::vec4(1, 0, 0, 0);
							out[vi].w0 = bwg.x; out[vi].w1 = bwg.y; out[vi].w2 = bwg.z; out[vi].w3 = bwg.w;
						}
					}
					else {
						auto* out = reinterpret_cast<PBRVertex*>(bsc->BlendedVertices.data());
						for (size_t vi = 0; vi < vCount; ++vi) {
							out[vi].x = meshPtr->Vertices[vi].x + accDP[vi].x;
							out[vi].y = meshPtr->Vertices[vi].y + accDP[vi].y;
							out[vi].z = meshPtr->Vertices[vi].z + accDP[vi].z;
							out[vi].nx = meshPtr->Normals[vi].x + accDN[vi].x;
							out[vi].ny = meshPtr->Normals[vi].y + accDN[vi].y;
							out[vi].nz = meshPtr->Normals[vi].z + accDN[vi].z;
							if (vi < meshPtr->UVs.size()) { out[vi].u = meshPtr->UVs[vi].x; out[vi].v = meshPtr->UVs[vi].y; }
							else { out[vi].u = 0.0f; out[vi].v = 0.0f; }
						}
					}

					bsc->Dirty = false;
				}
				});

			// Upload results on main thread (bgfx update)
			std::unordered_map<Mesh*, BlendWork*> workByMesh;
			workByMesh.reserve(blendWorks.size());
			for (auto& bw : blendWorks) {
				workByMesh[bw.meshPtr] = &bw;
			}
			for (auto& w : g.meshes) {
				if (!w.meshPtr || !w.needsBlend) continue;
				auto it = workByMesh.find(w.meshPtr);
				if (it == workByMesh.end()) continue;
				const BlendWork* bw = it->second;
				if (!bgfx::isValid(w.meshPtr->dvbh)) continue;
				const bgfx::Memory* mem = bgfx::copy(bw->bs->BlendedVertices.data(), (uint32_t)bw->bufferSize);
				bgfx::update(w.meshPtr->dvbh, 0, mem);
			}
		}

		if (collectDetailedPrefabPerf) {
			const auto prefabPerfGroupEnd = std::chrono::high_resolution_clock::now();
			const EntityID prefabRootId = cm::debug::ResolveOwningPrefabRoot(scene, g.root);
			if (prefabRootId != INVALID_ENTITY_ID) {
				auto& sample = prefabSkinningPerf[prefabRootId];
				sample.TotalMs +=
					std::chrono::duration<double, std::milli>(prefabPerfGroupEnd - prefabPerfGroupStart).count();
				++sample.Groups;
				sample.Meshes += static_cast<uint64_t>(g.meshes.size());
				sample.Bones += static_cast<uint64_t>(boneCount);
				sample.GroupsLodSkipped += groupLodSkipped;
				sample.PaletteSkipped += groupPaletteSkipped;
				sample.BlendshapeSkipped += groupBlendshapeSkipped;
			}
		}
	}

	// 4) Non-skinned meshes: apply blendshapes separately - parallelize compute
	//    Optimized: uses sparse accumulation and reuses scratch buffers.
	if (!nonSkinned.empty()) {
		for (auto& w : nonSkinned) {
			if (!w.needsBlend || !w.meshPtr || !w.bs) continue;
			const size_t vCount = w.meshPtr->Vertices.size();
			if (vCount == 0) { w.bs->Dirty = false; continue; }
			w.bs->EnsureBlendOutput(vCount, sizeof(PBRVertex));
		}
		const size_t chunkItems = (nonSkinned.size() >= 32) ? size_t{ 8 } : size_t{ 1 };
		parallel_for(Jobs(), size_t(0), nonSkinned.size(), chunkItems, [&](size_t s, size_t c) {
			for (size_t i = s; i < s + c; ++i) {
				auto& w = nonSkinned[i];
				const size_t vCount = w.meshPtr->Vertices.size();

				// Use scratch buffers from component (avoids per-frame allocations)
				w.bs->EnsureScratchBuffers(vCount);
				glm::vec3* accDP = w.bs->AccDeltaPos.data();
				glm::vec3* accDN = w.bs->AccDeltaNorm.data();

				// Check if any shape is sparse
				bool anySparse = false;
				for (const auto& shape : w.bs->Shapes) {
					if (shape.IsSparse && shape.Weight != 0.0f) { anySparse = true; break; }
				}

				if (anySparse) {
					// Sparse path
					w.bs->ClearTouchedOnly();

					for (const auto& shape : w.bs->Shapes) {
						const float weight = shape.Weight; if (weight == 0.0f) continue;

						if (shape.IsSparse) {
							for (size_t si = 0; si < shape.SparseIndices.size(); ++si) {
								const uint32_t idx = shape.SparseIndices[si];
								if (idx < vCount) {
									accDP[idx] += shape.SparseDeltaPos[si] * weight;
									accDN[idx] += shape.SparseDeltaNorm[si] * weight;
								}
							}
							for (uint32_t idx : shape.SparseIndices) {
								if (idx < vCount) w.bs->TouchedIndices.push_back(idx);
							}
						}
						else {
							if (shape.DeltaPos.size() != vCount) continue;
							for (size_t vi = 0; vi < vCount; ++vi) {
								accDP[vi] += shape.DeltaPos[vi] * weight;
								if (vi < shape.DeltaNormal.size()) {
									accDN[vi] += shape.DeltaNormal[vi] * weight;
								}
							}
						}
					}
				}
				else {
					// Dense path
					w.bs->ClearAccumulators();

					for (const auto& shape : w.bs->Shapes) {
						const float weight = shape.Weight; if (weight == 0.0f) continue;
						if (shape.DeltaPos.size() != vCount) continue;
						for (size_t vi = 0; vi < vCount; ++vi) {
							accDP[vi] += shape.DeltaPos[vi] * weight;
							if (vi < shape.DeltaNormal.size()) {
								accDN[vi] += shape.DeltaNormal[vi] * weight;
							}
						}
					}
				}


				auto* out = reinterpret_cast<PBRVertex*>(w.bs->BlendedVertices.data());
				for (size_t vi = 0; vi < vCount; ++vi) {
					out[vi].x = w.meshPtr->Vertices[vi].x + accDP[vi].x;
					out[vi].y = w.meshPtr->Vertices[vi].y + accDP[vi].y;
					out[vi].z = w.meshPtr->Vertices[vi].z + accDP[vi].z;
					out[vi].nx = w.meshPtr->Normals[vi].x + accDN[vi].x;
					out[vi].ny = w.meshPtr->Normals[vi].y + accDN[vi].y;
					out[vi].nz = w.meshPtr->Normals[vi].z + accDN[vi].z;
					if (vi < w.meshPtr->UVs.size()) {
						out[vi].u = w.meshPtr->UVs[vi].x;
						out[vi].v = w.meshPtr->UVs[vi].y;
					}
					else {
						out[vi].u = 0.0f;
						out[vi].v = 0.0f;
					}
				}
			}
			});
		// Upload on main thread
		for (auto& w : nonSkinned) {
			if (!bgfx::isValid(w.meshPtr->dvbh)) continue;
			const size_t vCount = w.meshPtr->Vertices.size();
			if (vCount == 0) continue;
			const bgfx::Memory* mem = bgfx::copy(w.bs->BlendedVertices.data(), uint32_t(sizeof(PBRVertex) * vCount));
			bgfx::update(w.meshPtr->dvbh, 0, mem);
			w.bs->Dirty = false;
		}
	}
	auto& profiler = Profiler::Get();
	profiler.SetCounter("Skinning/GroupsTotal", skelGroupsTotal);
	profiler.SetCounter("Skinning/GroupsUpdated", skelGroupsUpdated);
	profiler.SetCounter("Skinning/LodSkipped", skelGroupsLodSkipped);
	profiler.SetCounter("Skinning/CrowdThrottled", skelGroupsCrowdThrottled);
	profiler.SetCounter("Skinning/RagdollThrottled", skelGroupsRagdollThrottled);
	profiler.SetCounter("Skinning/NoConsumerSkipped", skelGroupsNoConsumerSkipped);
	profiler.SetCounter("Skinning/LodRelevantGroups", skelGroupsLodRelevant);
	profiler.SetCounter("Skinning/DormantSkipped", skelGroupsDormantSkipped);
	profiler.SetCounter("Skinning/AnimatedPaletteReused", skelGroupsAnimatedPaletteReused);
	profiler.SetCounter("Skinning/ConstraintOwned", skelGroupsConstraintOwned);
	profiler.SetCounter("Skinning/DisabledPoseReused", skelGroupsDisabledPoseReused);
	profiler.SetCounter("Skinning/GpuSharedSourceMeshes", skinnedGpuSharedSourceMeshes);
	profiler.SetCounter("Skinning/GpuRemapMeshes", skinnedGpuRemapMeshes);
	profiler.SetCounter("Skinning/GpuCorrectionMeshes", skinnedGpuCorrectionMeshes);
	profiler.SetCounter("Skinning/GpuMorphCandidateMeshes", skinnedGpuMorphCandidateMeshes);
	profiler.SetCounter("Skinning/GpuMorphBaseCapableMeshes", skinnedGpuMorphBaseCapableMeshes);
	profiler.SetCounter("Skinning/GpuMorphCpuConsumerBlocked", skinnedGpuMorphCpuConsumerBlocked);
	profiler.SetCounter("Skinning/GpuMorphRenderStateBlocked", skinnedGpuMorphRenderStateBlocked);
	profiler.SetCounter("Skinning/GpuMorphBatchTooSmall", skinnedGpuMorphBatchTooSmall);
	profiler.SetCounter("Skinning/GpuMorphCrowdEligibleMeshes", skinnedGpuMorphCrowdEligibleMeshes);
	profiler.SetCounter("Skinning/MaterializedMorphCpuSkippedMeshes", skinnedMaterializedMorphCpuSkippedMeshes);
	profiler.SetCounter("Skinning/CpuPaletteMeshes", 0);
	profiler.SetCounter("Skinning/LegacyPaletteMeshes", 0);
	profiler.SetCounter("Skinning/MeshPaletteSkipped", skinnedPaletteSkipped);
	profiler.SetCounter("Skinning/BlendshapeSkipped", skinnedBlendshapeSkipped);
	profiler.SetCounter("Skinning/PrefabRootsUpdated", static_cast<uint64_t>(prefabSkinningPerf.size()));
	if (collectDetailedPrefabPerf) {
		for (const auto& [prefabRootId, sample] : prefabSkinningPerf) {
			const auto label = cm::debug::DescribePrefabRoot(scene, prefabRootId);
			if (!label.IsValid()) {
				continue;
			}
			profiler.Record(cm::debug::MakePrefabProfilerSection("Skinning/Prefab", label), sample.TotalMs);
		}

		if (cm::debug::PrefabPerfConsoleLoggingEnabled()) {
			static uint64_t s_PrefabSkinningLogFrame = 0;
			const uint32_t logInterval = cm::debug::PrefabPerfConsoleLogInterval();
			++s_PrefabSkinningLogFrame;
			if (logInterval > 0 && (s_PrefabSkinningLogFrame % logInterval) == 0u && !prefabSkinningPerf.empty()) {
				std::vector<std::pair<EntityID, const PrefabSkinningPerfSample*>> ordered;
				ordered.reserve(prefabSkinningPerf.size());
				for (const auto& entry : prefabSkinningPerf) {
					ordered.emplace_back(entry.first, &entry.second);
				}
				std::sort(ordered.begin(), ordered.end(),
					[](const auto& lhs, const auto& rhs) {
						return lhs.second->TotalMs > rhs.second->TotalMs;
					});

				const size_t limit = std::min<size_t>(3, ordered.size());
				std::cout << "[PrefabPerf][Skinning] Top prefab roots this frame:" << std::endl;
				for (size_t i = 0; i < limit; ++i) {
					const EntityID prefabRootId = ordered[i].first;
					const PrefabSkinningPerfSample& sample = *ordered[i].second;
					const auto label = cm::debug::DescribePrefabRoot(scene, prefabRootId);
					std::cout << "[PrefabPerf][Skinning]   " << (i + 1) << ". "
						<< cm::debug::MakePrefabDebugLabel(label)
						<< " total=" << sample.TotalMs << "ms"
						<< " groups=" << sample.Groups
						<< " meshes=" << sample.Meshes
						<< " bones=" << sample.Bones;
					if (sample.GroupsLodSkipped > 0 || sample.PaletteSkipped > 0 || sample.BlendshapeSkipped > 0) {
						std::cout << " skipped={lod=" << sample.GroupsLodSkipped
							<< ", palette=" << sample.PaletteSkipped
							<< ", blend=" << sample.BlendshapeSkipped << "}";
					}
					std::cout << std::endl;


				}
			}
		}
	}
}

