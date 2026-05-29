#include "AnimationSystem.h"
#include "core/ecs/Scene.h"
#include "core/ecs/Entity.h"
#include "core/ecs/NpcScalability.h"
#include "core/world/RuntimeWorld.h"
#include <cmath>
#include <cctype>
#include <limits>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/transform.hpp>
#include "core/ecs/EntityData.h"
#include "AnimationSerializer.h"
#include "AnimatorController.h"
#include "AnimatorControllerIO.h"
#include "AnimatorControllerOverride.h"
#include "AnimatorControllerOverrideIO.h"
#include "AnimationAsset.h"
#include "AnimationAssetCache.h"
#include "AnimationEvaluator.h"
#include "BindingCache.h"
#include "HumanoidRetargeter.h"
#include "AvatarSerializer.h"
#include "HumanoidBone.h"
#include "AvatarMask.h"
// Script event dispatch to managed C# scripts
#include "managed/interop/ManagedScriptComponent.h"
#include "core/managed/ScriptInterop.h"
#ifndef CLAYMORE_RUNTIME
#include "managed/interop/AnimationEventInterop.h"
#endif
// Parallel processing
#include "core/jobs/ParallelFor.h"
#include "core/jobs/Jobs.h"
#include "core/navigation/NavAgent.h"
#include "core/rendering/Camera.h"
#include "core/utils/PrefabPerfDiagnostics.h"
#include "core/utils/Profiler.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <atomic>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>


namespace cm {
namespace animation {

namespace {

struct ResolvedAnimationBinding {
    std::string AssetPath;
    std::string ClipPath;

    const std::string& EffectivePath() const {
        return !AssetPath.empty() ? AssetPath : ClipPath;
    }
};

bool AnimationAssetNeedsRuntimeScalePreservation(const AnimationAsset* asset)
{
    if (!asset) {
        return false;
    }
    return asset->GetRuntimeView().HasUnmutedBoneTracks;
}

float LookupOrLoadAnimationDuration(AnimationPlayerComponent& player,
                                    int cacheKey,
                                    const ResolvedAnimationBinding& binding)
{
    auto durationIt = player.CachedDurations.find(cacheKey);
    if (durationIt != player.CachedDurations.end()) {
        return durationIt->second;
    }

    float duration = 0.0f;
    if (!binding.AssetPath.empty()) {
        std::shared_ptr<AnimationAsset> asset;
        auto assetIt = player.CachedAssets.find(cacheKey);
        if (assetIt != player.CachedAssets.end()) {
            asset = assetIt->second;
        } else {
            asset = LoadAnimationAssetCached(binding.AssetPath, true);
            if (asset && asset->Duration() > 0.0f) {
                player.CachedAssets[cacheKey] = asset;
            }
        }
        duration = asset ? asset->Duration() : 0.0f;
    } else if (!binding.ClipPath.empty()) {
        std::shared_ptr<AnimationClip> clip;
        auto clipIt = player.CachedClips.find(cacheKey);
        if (clipIt != player.CachedClips.end()) {
            clip = clipIt->second;
        } else {
            clip = std::make_shared<AnimationClip>(LoadAnimationClip(binding.ClipPath));
            if (clip) {
                player.CachedClips[cacheKey] = clip;
            }
        }
        duration = clip ? clip->Duration : 0.0f;
    }

    player.CachedDurations[cacheKey] = duration;
    return duration;
}

ResolvedAnimationBinding ResolveAnimationBinding(const AnimationPlayerComponent& player,
                                                const std::string& assetPath,
                                                const std::string& clipPath)
{
    if (player.ControllerOverride &&
        player.ControllerOverride->MatchesController(player.ControllerPath)) {
        if (!assetPath.empty()) {
            if (std::string overridePath = player.ControllerOverride->Resolve(assetPath); !overridePath.empty()) {
                return { overridePath, {} };
            }
        }
        if (!clipPath.empty()) {
            if (std::string overridePath = player.ControllerOverride->Resolve(clipPath); !overridePath.empty()) {
                return { overridePath, {} };
            }
        }
    }

    return { assetPath, clipPath };
}

ResolvedAnimationBinding ResolveAnimationBinding(const AnimationPlayerComponent& player,
                                                const AnimatorState& state)
{
    return ResolveAnimationBinding(player, state.AnimationAssetPath, state.ClipPath);
}

ResolvedAnimationBinding ResolveAnimationBinding(const AnimationPlayerComponent& player,
                                                const Blend1DEntry& entry)
{
    return ResolveAnimationBinding(player, entry.AssetPath, entry.ClipPath);
}

ResolvedAnimationBinding ResolveAnimationBinding(const AnimationPlayerComponent& player,
                                                const Blend2DEntry& entry)
{
    return ResolveAnimationBinding(player, entry.AssetPath, entry.ClipPath);
}

struct Blend1DKeyRange {
    float Min = 0.0f;
    float Max = 1.0f;
};

Blend1DKeyRange GetBlend1DKeyRange(const AnimatorState& state)
{
    if (state.Blend1DEntries.empty()) {
        return {};
    }

    const float firstKey = state.Blend1DEntries.front().Key;
    const float lastKey = state.Blend1DEntries.back().Key;
    if (firstKey <= lastKey) {
        return { firstKey, lastKey };
    }

    return { lastKey, firstKey };
}

float ReadBlend1DParameter(const AnimatorBlackboard& blackboard,
                           const AnimatorState& state)
{
    const float value = blackboard.GetFloatSlot(state.RuntimeBlend1DParamSlot);

    const Blend1DKeyRange range = GetBlend1DKeyRange(state);
    if (range.Max > range.Min) {
        return glm::clamp(value, range.Min, range.Max);
    }

    return value;
}

bool ContainsCaseInsensitive(const std::string& haystack, const char* needle)
{
    if (haystack.empty() || needle == nullptr || *needle == '\0') {
        return false;
    }

    const size_t needleLen = std::strlen(needle);
    if (haystack.size() < needleLen) {
        return false;
    }

    for (size_t offset = 0; offset + needleLen <= haystack.size(); ++offset) {
        bool match = true;
        for (size_t i = 0; i < needleLen; ++i) {
            const char lhs = static_cast<char>(std::tolower(static_cast<unsigned char>(haystack[offset + i])));
            const char rhs = static_cast<char>(std::tolower(static_cast<unsigned char>(needle[i])));
            if (lhs != rhs) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }

    return false;
}

bool IsIdleLikeStateName(const std::string& stateName)
{
    return ContainsCaseInsensitive(stateName, "idle") ||
           ContainsCaseInsensitive(stateName, "ambient") ||
           ContainsCaseInsensitive(stateName, "lookaround") ||
           ContainsCaseInsensitive(stateName, "look_around") ||
           ContainsCaseInsensitive(stateName, "rest") ||
           ContainsCaseInsensitive(stateName, "sleep");
}

bool HasArmedTriggers(const AnimatorBlackboard& blackboard)
{
    for (const auto& [name, armed] : blackboard.Triggers) {
        (void)name;
        if (armed) {
            return true;
        }
    }
    return false;
}

bool IsOffscreenDormancyEligible(const AnimationPlayerComponent& player)
{
    if (!player.OffscreenDormancyEnabled) {
        return false;
    }

    if (!player.Controller || !player.Enabled || !player.IsPlaying) {
        return false;
    }

    const AnimatorPlayback& playback = player.AnimatorInstance.Playback();
    if (player.CurrentStateId < 0 || playback.CurrentStateId < 0) {
        return false;
    }

    if (player.AnimatorInstance.IsCrossfading()) {
        return false;
    }

    if (playback.NextStateId >= 0 && playback.NextStateId != player.CurrentStateId) {
        return false;
    }

    if (HasArmedTriggers(player.AnimatorInstance.Blackboard())) {
        return false;
    }

    const int nextState = player.AnimatorInstance.ChooseNextState();
    if (nextState >= 0 && nextState != player.CurrentStateId) {
        return false;
    }

    const AnimatorState* state = player.Controller->FindStateInLayer(0, player.CurrentStateId);
    if (!state || !state->Loop) {
        return false;
    }

    return IsIdleLikeStateName(state->Name);
}

uint64_t HashCombineValue(uint64_t seed, uint64_t value)
{
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    return seed;
}

uint64_t HashStringValue(const std::string& value)
{
    constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
    constexpr uint64_t kFnvPrime = 1099511628211ULL;
    uint64_t hash = kFnvOffset;
    for (unsigned char c : value) {
        hash ^= static_cast<uint64_t>(c);
        hash *= kFnvPrime;
    }
    return hash;
}

bool AnimationAssetAllowsCrowdPoseShare(const AnimationAsset* asset)
{
    if (asset == nullptr) {
        return false;
    }
    return asset->GetRuntimeView().AllowsCrowdPoseShare;
}

bool TryResolveAnimationLodWorldBounds(const cm::world::RuntimeWorld* runtimeWorld,
                                       EntityID rootId,
                                       const EntityData& data,
                                       glm::vec3& outMin,
                                       glm::vec3& outMax)
{
    if (runtimeWorld != nullptr && data.Skeleton != nullptr) {
        if (const auto* cache = runtimeWorld->TryGetSkinningGroupCache(rootId)) {
            if (runtimeWorld->TryGetSkinningGroupWorldBounds(*cache, outMin, outMax)) {
                constexpr float kConservativePadding = 0.35f;
                outMin -= glm::vec3(kConservativePadding);
                outMax += glm::vec3(kConservativePadding);
                return true;
            }
        }
    }

    if (runtimeWorld != nullptr) {
        const cm::world::RuntimeEntityHandle handle = runtimeWorld->TryGetHandle(rootId);
        if (handle.IsValid()) {
            if (const auto* bounds = runtimeWorld->TryGetBounds(handle);
                bounds != nullptr && bounds->Valid) {
                outMin = bounds->WorldMin;
                outMax = bounds->WorldMax;
                return true;
            }
        }
    }

    return false;
}

bool AnimationPlayerHasActiveOverlayLayers(const AnimationPlayerComponent& player)
{
    for (const auto& layerState : player.AnimatorInstance.LayerStates()) {
        if (layerState.LayerIndex <= 0) {
            continue;
        }

        if (layerState.IsCrossfading()) {
            return true;
        }

        if (layerState.Weight > 0.001f || layerState.TargetWeight > 0.001f) {
            return true;
        }
    }

    return false;
}

EntityID ResolveActiveCameraOwnerEntity(::Scene& scene)
{
    const EntityID activeCameraEntity = scene.GetActiveCameraEntityID();
    if (activeCameraEntity == INVALID_ENTITY_ID) {
        return INVALID_ENTITY_ID;
    }

    auto* cameraData = scene.GetEntityData(activeCameraEntity);
    if (!cameraData) {
        return activeCameraEntity;
    }

    return cameraData->Parent != INVALID_ENTITY_ID
        ? cameraData->Parent
        : activeCameraEntity;
}

bool IsEntityDescendantOf(::Scene& scene, EntityID entityId, EntityID ancestorId)
{
    if (entityId == INVALID_ENTITY_ID || ancestorId == INVALID_ENTITY_ID) {
        return false;
    }

    EntityID current = entityId;
    while (current != INVALID_ENTITY_ID) {
        if (current == ancestorId) {
            return true;
        }

        auto* data = scene.GetEntityData(current);
        if (!data) {
            break;
        }

        current = data->Parent;
    }

    return false;
}

bool HasActiveLookAtConstraint(const EntityData& data)
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

bool HasPotentialIkConstraint(const EntityData& data)
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

bool ShouldWriteAnimatedBoneEntities(::Scene& scene,
                                     const EntityData& skeletonData)
{
    (void)skeletonData;

    if (!scene.m_IsPlaying) {
        return true;
    }

    return false;
}

struct AnimationLodDistanceThresholds {
    float NearSq = 0.0f;
    float MediumSq = 0.0f;
    float FarSq = 0.0f;
};

AnimationLodDistanceThresholds ResolveAnimationLodDistanceThresholds(
    const AnimationPlayerComponent& player)
{
    const float nearDist = std::max(0.0f, player.LODNearDistance);
    const float mediumDist = std::max(nearDist, player.LODMediumDistance);
    const float farDist = std::max(mediumDist, player.LODFarDistance);
    return {
        nearDist * nearDist,
        mediumDist * mediumDist,
        farDist * farDist
    };
}

float ResolveVisibleAnimationLodInterval(const AnimationPlayerComponent& player, float distSq)
{
    if (!player.LODEnabled) {
        return 0.0f;
    }

    const AnimationLodDistanceThresholds thresholds =
        ResolveAnimationLodDistanceThresholds(player);
    if (distSq < thresholds.NearSq) {
        return 0.0f;
    }
    if (distSq < thresholds.MediumSq) {
        return std::max(0.0f, player.LODMediumInterval);
    }
    if (distSq < thresholds.FarSq) {
        return std::max(0.0f, player.LODFarInterval);
    }
    return std::max(0.0f, player.LODVeryFarInterval);
}

float ResolveOffscreenAnimationLodInterval(const AnimationPlayerComponent& player, float distSq)
{
    if (!player.LODEnabled) {
        return 0.0f;
    }

    const AnimationLodDistanceThresholds thresholds =
        ResolveAnimationLodDistanceThresholds(player);
    if (distSq < thresholds.NearSq) {
        return std::max(0.0f, player.OffscreenNearInterval);
    }
    if (distSq < thresholds.MediumSq) {
        return std::max(0.0f, player.OffscreenMediumInterval);
    }
    if (distSq < thresholds.FarSq) {
        return std::max(0.0f, player.OffscreenFarInterval);
    }
    return std::max(0.0f, player.OffscreenVeryFarInterval);
}

bool AnimationHasActiveMotionDriver(const EntityData& data)
{
    constexpr float kMotionEpsilonSq = 0.0025f;

    if (data.NavAgent && data.NavAgent->Enabled) {
        const float currentSpeedSq = glm::dot(
            data.NavAgent->CurrentVelocity,
            data.NavAgent->CurrentVelocity);
        const float smoothedSpeedSq = glm::dot(
            data.NavAgent->SmoothedVelocity,
            data.NavAgent->SmoothedVelocity);
        if (currentSpeedSq > kMotionEpsilonSq || smoothedSpeedSq > kMotionEpsilonSq) {
            return true;
        }
        if (data.NavAgent->HasDestination ||
            data.NavAgent->PathRequested ||
            data.NavAgent->HasPath()) {
            return true;
        }
    }

    if (data.CharacterController) {
        const float desiredSpeedSq = glm::dot(
            data.CharacterController->DesiredVelocity,
            data.CharacterController->DesiredVelocity);
        if (desiredSpeedSq > kMotionEpsilonSq ||
            std::abs(data.CharacterController->VerticalVelocity) > 0.01f ||
            data.CharacterController->JumpRequested) {
            return true;
        }
    }

    if (data.RigidBody) {
        const float linearSpeedSq = glm::dot(
            data.RigidBody->LinearVelocity,
            data.RigidBody->LinearVelocity);
        if (linearSpeedSq > kMotionEpsilonSq) {
            return true;
        }
    }

    return false;
}

uint64_t BuildCrowdPoseSkeletonSignature(const SkeletonComponent& skeleton)
{
    return skeleton.GetCachedTopologySignature();
}

uint16_t QuantizeCrowdPoseUnitValue(float value, uint32_t bucketCount)
{
    if (bucketCount <= 1u) {
        return 0u;
    }

    float wrapped = std::fmod(value, 1.0f);
    if (wrapped < 0.0f) {
        wrapped += 1.0f;
    }
    const float scaled = wrapped * static_cast<float>(bucketCount);
    const uint32_t bucket = std::min<uint32_t>(
        static_cast<uint32_t>(scaled),
        bucketCount - 1u);
    return static_cast<uint16_t>(bucket);
}

uint16_t QuantizeCrowdPoseRangeValue(float value,
                                     float minValue,
                                     float maxValue,
                                     uint32_t bucketCount)
{
    if (bucketCount <= 1u || maxValue <= minValue) {
        return 0u;
    }

    const float normalized =
        glm::clamp((value - minValue) / (maxValue - minValue), 0.0f, 1.0f);
    const float scaled = normalized * static_cast<float>(bucketCount);
    const uint32_t bucket = std::min<uint32_t>(
        static_cast<uint32_t>(scaled),
        bucketCount - 1u);
    return static_cast<uint16_t>(bucket);
}

struct CrowdPoseShareKey {
    uint64_t SkeletonSignature = 0;
    uint64_t ControllerSignature = 0;
    int32_t StateId = -1;
    uint16_t PhaseBucket = 0;
    uint16_t ParamBucketX = 0;
    uint16_t ParamBucketY = 0;
    uint16_t BlendEntryA = 0xffffu;
    uint16_t BlendEntryB = 0xffffu;
    std::array<uint16_t, 4> Blend2DEntries = { 0xffffu, 0xffffu, 0xffffu, 0xffffu };
    uint8_t StateKind = 0;

    bool operator==(const CrowdPoseShareKey& other) const
    {
        return SkeletonSignature == other.SkeletonSignature &&
               ControllerSignature == other.ControllerSignature &&
               StateId == other.StateId &&
               PhaseBucket == other.PhaseBucket &&
               ParamBucketX == other.ParamBucketX &&
               ParamBucketY == other.ParamBucketY &&
               BlendEntryA == other.BlendEntryA &&
               BlendEntryB == other.BlendEntryB &&
               Blend2DEntries == other.Blend2DEntries &&
               StateKind == other.StateKind;
    }
};

struct CrowdPoseShareKeyHash {
    size_t operator()(const CrowdPoseShareKey& key) const noexcept
    {
        uint64_t hash = 14695981039346656037ULL;
        hash = HashCombineValue(hash, key.SkeletonSignature);
        hash = HashCombineValue(hash, key.ControllerSignature);
        hash = HashCombineValue(hash, static_cast<uint64_t>(static_cast<int64_t>(key.StateId) + 2));
        hash = HashCombineValue(hash, key.PhaseBucket);
        hash = HashCombineValue(hash, key.ParamBucketX);
        hash = HashCombineValue(hash, key.ParamBucketY);
        hash = HashCombineValue(hash, key.BlendEntryA);
        hash = HashCombineValue(hash, key.BlendEntryB);
        for (uint16_t entryIndex : key.Blend2DEntries) {
            hash = HashCombineValue(hash, entryIndex);
        }
        hash = HashCombineValue(hash, key.StateKind);
        return static_cast<size_t>(hash);
    }
};

void BuildSkeletonLocalPaletteFromLocalTransforms(SkeletonComponent& skeleton,
                                                  const std::vector<glm::mat4>& localTransforms,
                                                  uint32_t poseFrameId,
                                                  bool retainRuntimeLocalPose)
{
    const size_t boneCount = std::min(
        localTransforms.size(),
        skeleton.InverseBindPoses.size());
    if (boneCount == 0) {
        skeleton.BonePalette.clear();
        skeleton.BoneCount = 0;
        skeleton.PoseFrameId = poseFrameId;
        skeleton.AnimatedPosePaletteValid = false;
        skeleton.ClearRuntimeLocalPose(poseFrameId);
        return;
    }

    if (retainRuntimeLocalPose) {
        skeleton.StoreRuntimeLocalPose(localTransforms, boneCount, poseFrameId);
        skeleton.EnsureRuntimeLocalPoseTrsSize(boneCount);
        auto decomposeRuntimeLocalPoseTrs = [](const glm::mat4& m,
                                               glm::vec3& T,
                                               glm::quat& R,
                                               glm::vec3& S) {
            T = glm::vec3(m[3]);
            glm::vec3 X = glm::vec3(m[0]);
            glm::vec3 Y = glm::vec3(m[1]);
            glm::vec3 Z = glm::vec3(m[2]);
            S = glm::vec3(glm::length(X), glm::length(Y), glm::length(Z));
            if (S.x > 1e-6f) X /= S.x;
            if (S.y > 1e-6f) Y /= S.y;
            if (S.z > 1e-6f) Z /= S.z;
            glm::mat3 rotMat(X, Y, Z);
            R = glm::normalize(glm::quat_cast(rotMat));
        };
        for (size_t boneIndex = 0; boneIndex < boneCount; ++boneIndex) {
            decomposeRuntimeLocalPoseTrs(
                localTransforms[boneIndex],
                skeleton.RuntimeLocalTranslations[boneIndex],
                skeleton.RuntimeLocalRotations[boneIndex],
                skeleton.RuntimeLocalScales[boneIndex]);
        }
        skeleton.RuntimeLocalPoseTrsValid = true;
    } else {
        skeleton.ClearRuntimeLocalPose(poseFrameId);
    }

    skeleton.EnsureRuntimeBonePaletteSize(boneCount);
    skeleton.EnsureRuntimeBoneGlobalSize(boneCount);
    thread_local std::vector<uint8_t> resolved;
    auto& skeletonLocalGlobals = skeleton.RuntimeBoneGlobals;

    const bool canResolveLinearly = skeleton.CanResolveHierarchyLinearly(boneCount);
    if (canResolveLinearly) {
        for (size_t boneIndex = 0; boneIndex < boneCount; ++boneIndex) {
            const int parentIndex = (boneIndex < skeleton.BoneParents.size())
                ? skeleton.BoneParents[boneIndex]
                : -1;
            if (parentIndex >= 0 && static_cast<size_t>(parentIndex) < boneCount) {
                skeletonLocalGlobals[boneIndex] =
                    skeletonLocalGlobals[static_cast<size_t>(parentIndex)] *
                    localTransforms[boneIndex];
            } else {
                skeletonLocalGlobals[boneIndex] = localTransforms[boneIndex];
            }
            skeleton.BonePalette[boneIndex] =
                skeletonLocalGlobals[boneIndex] *
                skeleton.InverseBindPoses[boneIndex];
        }
        skeleton.BoneCount = static_cast<uint32_t>(boneCount);
        skeleton.PoseFrameId = poseFrameId;
        skeleton.AnimatedPosePaletteValid = true;
        skeleton.RuntimeBoneGlobalsFrameId = poseFrameId;
        skeleton.RuntimeBoneGlobalsValid = true;
        return;
    }

    if (resolved.size() < boneCount) {
        resolved.resize(boneCount);
    }
    std::fill_n(resolved.begin(), boneCount, uint8_t{0u});

    auto resolveGlobal = [&](auto&& self, size_t boneIndex) -> const glm::mat4& {
        if (resolved[boneIndex] != 0u) {
            return skeletonLocalGlobals[boneIndex];
        }

        const int parentIndex =
            (boneIndex < skeleton.BoneParents.size()) ? skeleton.BoneParents[boneIndex] : -1;
        if (parentIndex >= 0 && static_cast<size_t>(parentIndex) < boneCount) {
            skeletonLocalGlobals[boneIndex] =
                self(self, static_cast<size_t>(parentIndex)) * localTransforms[boneIndex];
        } else {
            skeletonLocalGlobals[boneIndex] = localTransforms[boneIndex];
        }
        resolved[boneIndex] = 1u;
        return skeletonLocalGlobals[boneIndex];
    };

    for (size_t boneIndex = 0; boneIndex < boneCount; ++boneIndex) {
        const glm::mat4& skeletonLocal =
            resolved[boneIndex] != 0u ? skeletonLocalGlobals[boneIndex]
                                      : resolveGlobal(resolveGlobal, boneIndex);
        skeleton.BonePalette[boneIndex] = skeletonLocal * skeleton.InverseBindPoses[boneIndex];
    }

    skeleton.BoneCount = static_cast<uint32_t>(boneCount);
    skeleton.PoseFrameId = poseFrameId;
    skeleton.AnimatedPosePaletteValid = true;
    skeleton.RuntimeBoneGlobalsFrameId = poseFrameId;
    skeleton.RuntimeBoneGlobalsValid = true;
}

void EnsureAnimatorRuntimeState(AnimationPlayerComponent& player)
{
    if (player.NeedsRuntimeControllerSync()) {
        player.SyncRuntimeControllerState();
    }
}

} // namespace

// =========================================================================
// PERF: AnimationPlayerComponent::PreloadAllControllerAssets
// Preloads all animation assets referenced by the controller to avoid
// hitching during gameplay when transitioning between states.
// =========================================================================
bool AnimationPlayerComponent::PreloadAllControllerAssets(bool allowParallel) {
    if (!Controller) {
        _AnimationsPreloaded = true;
        return true;
    }
    if (_AnimationsPreloaded) {
        return true;
    }
    
    struct PreloadEntry {
        int key = -1;
        std::string path;
    };
    
    std::vector<PreloadEntry> entries;
    
    auto queueAsset = [this, &entries](int key, const std::string& path) {
        if (path.empty()) return;
        if (CachedAssets.find(key) != CachedAssets.end()) return;
        entries.push_back({ key, path });
    };
    
    auto queueState = [&](const AnimatorState& state, int layerIdx) {
        const bool isOverlay = layerIdx > 0;
        const int baseKey = isOverlay ? (layerIdx * 10000 + state.Id) : state.Id;

        const ResolvedAnimationBinding resolvedState = ResolveAnimationBinding(*this, state);
        queueAsset(baseKey, resolvedState.EffectivePath());
        
        if (state.Kind == AnimatorStateKind::Blend1D) {
            for (int i = 0; i < static_cast<int>(state.Blend1DEntries.size()); ++i) {
                const auto& entry = state.Blend1DEntries[i];
                std::string path = ResolveAnimationBinding(*this, entry).EffectivePath();
                if (path.empty()) continue;
                
                const int cacheKey = isOverlay
                    ? (layerIdx * 10000 + state.Id * 100 + i)
                    : (state.Id * 1000 + i);
                queueAsset(cacheKey, path);
            }
        } else if (state.Kind == AnimatorStateKind::Blend2D) {
            for (int i = 0; i < static_cast<int>(state.Blend2DEntries.size()); ++i) {
                const auto& entry = state.Blend2DEntries[i];
                std::string path = ResolveAnimationBinding(*this, entry).EffectivePath();
                if (path.empty()) continue;

                const int cacheKey = isOverlay
                    ? (layerIdx * 10000 + state.Id * 100 + i)
                    : (state.Id * 1000 + i);
                queueAsset(cacheKey, path);
            }
        }
    };
    
    if (!Controller->Layers.empty()) {
        for (const auto& layer : Controller->Layers) {
            for (const auto& state : layer.States) {
                queueState(state, layer.Index);
            }
        }
    } else {
        for (const auto& state : Controller->States) {
            queueState(state, 0);
        }
    }
    
    const bool requestAsync = allowParallel && cm::g_JobSystem != nullptr;
    bool allReady = true;

    for (const auto& entry : entries) {
        auto it = CachedAssets.find(entry.key);
        if (it != CachedAssets.end() && it->second) {
            continue;
        }

        std::shared_ptr<AnimationAsset> asset = requestAsync
            ? TryGetAnimationAssetCached(entry.path)
            : LoadAnimationAssetCached(entry.path, true);

        if (!asset && requestAsync) {
            RequestAnimationAssetPreload(entry.path, true);
            allReady = false;
            continue;
        }

        if (!asset) {
            allReady = false;
            continue;
        }

        CachedAssets[entry.key] = asset;
    }

    _AnimationsPreloaded = allReady;
    return _AnimationsPreloaded;
}

    static bool StateMaskIncludesBone(HumanoidBone b, uint8_t maskBits) {
        switch (b) {
            // Hips
            case HumanoidBone::Hips:
                return (maskBits & AnimatorState::MaskBit_Hips) != 0;
            
            // Spine group (spine, chest, upper chest)
            case HumanoidBone::Spine:
            case HumanoidBone::Chest:
            case HumanoidBone::UpperChest:
                return (maskBits & AnimatorState::MaskBit_Spine) != 0;
            
            // Head group (neck, head, eyes)
            case HumanoidBone::Neck:
            case HumanoidBone::Head:
            case HumanoidBone::LeftEye:
            case HumanoidBone::RightEye:
                return (maskBits & AnimatorState::MaskBit_Head) != 0;
            
            // Left Arm group
            case HumanoidBone::LeftShoulder:
            case HumanoidBone::LeftUpperArm:
            case HumanoidBone::LeftLowerArm:
            case HumanoidBone::LeftHand:
            case HumanoidBone::LeftUpperArmTwist:
            case HumanoidBone::LeftLowerArmTwist:
            // Left fingers
            case HumanoidBone::LeftThumbProx:
            case HumanoidBone::LeftThumbInter:
            case HumanoidBone::LeftThumbDist:
            case HumanoidBone::LeftIndexProx:
            case HumanoidBone::LeftIndexInter:
            case HumanoidBone::LeftIndexDist:
            case HumanoidBone::LeftMiddleProx:
            case HumanoidBone::LeftMiddleInter:
            case HumanoidBone::LeftMiddleDist:
            case HumanoidBone::LeftRingProx:
            case HumanoidBone::LeftRingInter:
            case HumanoidBone::LeftRingDist:
            case HumanoidBone::LeftLittleProx:
            case HumanoidBone::LeftLittleInter:
            case HumanoidBone::LeftLittleDist:
                return (maskBits & AnimatorState::MaskBit_LeftArm) != 0;
            
            // Right Arm group
            case HumanoidBone::RightShoulder:
            case HumanoidBone::RightUpperArm:
            case HumanoidBone::RightLowerArm:
            case HumanoidBone::RightHand:
            case HumanoidBone::RightUpperArmTwist:
            case HumanoidBone::RightLowerArmTwist:
            // Right fingers
            case HumanoidBone::RightThumbProx:
            case HumanoidBone::RightThumbInter:
            case HumanoidBone::RightThumbDist:
            case HumanoidBone::RightIndexProx:
            case HumanoidBone::RightIndexInter:
            case HumanoidBone::RightIndexDist:
            case HumanoidBone::RightMiddleProx:
            case HumanoidBone::RightMiddleInter:
            case HumanoidBone::RightMiddleDist:
            case HumanoidBone::RightRingProx:
            case HumanoidBone::RightRingInter:
            case HumanoidBone::RightRingDist:
            case HumanoidBone::RightLittleProx:
            case HumanoidBone::RightLittleInter:
            case HumanoidBone::RightLittleDist:
                return (maskBits & AnimatorState::MaskBit_RightArm) != 0;
            
            // Left Leg group
            case HumanoidBone::LeftUpperLeg:
            case HumanoidBone::LeftLowerLeg:
            case HumanoidBone::LeftFoot:
            case HumanoidBone::LeftToes:
            case HumanoidBone::LeftUpperLegTwist:
            case HumanoidBone::LeftLowerLegTwist:
                return (maskBits & AnimatorState::MaskBit_LeftLeg) != 0;
            
            // Right Leg group
            case HumanoidBone::RightUpperLeg:
            case HumanoidBone::RightLowerLeg:
            case HumanoidBone::RightFoot:
            case HumanoidBone::RightToes:
            case HumanoidBone::RightUpperLegTwist:
            case HumanoidBone::RightLowerLegTwist:
                return (maskBits & AnimatorState::MaskBit_RightLeg) != 0;
            
            // Root bone - always included if any lower body part is
            case HumanoidBone::Root:
                return (maskBits & (AnimatorState::MaskBit_Hips | 
                                   AnimatorState::MaskBit_LeftLeg | 
                                   AnimatorState::MaskBit_RightLeg)) != 0;
            
            default:
                return true; // Unknown bones: keep animated
        }
    }

    static bool IsArmLayerSupportBone(HumanoidBone b) {
        switch (b) {
            case HumanoidBone::Chest:
            case HumanoidBone::UpperChest:
            case HumanoidBone::Neck:
            case HumanoidBone::Head:
            case HumanoidBone::LeftEye:
            case HumanoidBone::RightEye:
                return true;
            default:
                return false;
        }
    }

    static bool LayerPresetCanBorrowUpperBodySupport(BodyMaskPreset preset) {
        switch (preset) {
            case BodyMaskPreset::LeftArm:
            case BodyMaskPreset::RightArm:
            case BodyMaskPreset::LeftHand:
            case BodyMaskPreset::RightHand:
            case BodyMaskPreset::Arms:
                return true;
            default:
                return false;
        }
    }

    static bool MaskBitsIncludeUpperBodySupport(uint8_t maskBits) {
        return (maskBits & (AnimatorState::MaskBit_Spine | AnimatorState::MaskBit_Head)) != 0;
    }

    static bool LayerPresetProvidesUpperBodySupport(BodyMaskPreset preset) {
        switch (preset) {
            case BodyMaskPreset::FullBody:
            case BodyMaskPreset::UpperBody:
            case BodyMaskPreset::Spine:
            case BodyMaskPreset::Head:
                return true;
            default:
                return false;
        }
    }

    void decomposeTRS(const glm::mat4& m, glm::vec3& T, glm::quat& R, glm::vec3& S) {
        T = glm::vec3(m[3]);
        glm::vec3 X = glm::vec3(m[0]);
        glm::vec3 Y = glm::vec3(m[1]);
        glm::vec3 Z = glm::vec3(m[2]);
        S = glm::vec3(glm::length(X), glm::length(Y), glm::length(Z));
        if (S.x > 1e-6f) X /= S.x;
        if (S.y > 1e-6f) Y /= S.y;
        if (S.z > 1e-6f) Z /= S.z;
        glm::mat3 rotMat(X, Y, Z);
        R = glm::quat_cast(rotMat);
    }

    struct AnimationLodPlane {
        glm::vec4 p;
    };

    struct AnimationLodFrustum {
        AnimationLodPlane planes[6];
    };

    struct CrowdLodBudget {
        uint32_t fullRateCount = 0;
        uint32_t thirtyHzCount = 0;
        uint32_t fifteenHzCount = 0;
        float overflowInterval = 0.10000000f;
    };

    static AnimationLodFrustum BuildAnimationLodFrustum(const glm::mat4& view, const glm::mat4& proj) {
        const glm::mat4 vp = proj * view;
        auto row = [&](int r) { return glm::vec4(vp[0][r], vp[1][r], vp[2][r], vp[3][r]); };
        const glm::vec4 r0 = row(0);
        const glm::vec4 r1 = row(1);
        const glm::vec4 r2 = row(2);
        const glm::vec4 r3 = row(3);

        AnimationLodFrustum f{};
        glm::vec4 planes[6] = {
            r3 + r0,
            r3 - r0,
            r3 + r1,
            r3 - r1,
            r3 + r2,
            r3 - r2
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

    static bool AnimationLodAabbVisible(const AnimationLodFrustum& f,
                                        const glm::vec3& wmin,
                                        const glm::vec3& wmax) {
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

    static bool AnimationLodPointVisible(const AnimationLodFrustum& f, const glm::vec3& center, float radius) {
        const glm::vec3 extents(radius);
        return AnimationLodAabbVisible(f, center - extents, center + extents);
    }

    static CrowdLodBudget ResolveCrowdLodBudget(size_t visibleCount) {
        CrowdLodBudget budget{};
        budget.overflowInterval = (visibleCount >= 48) ? 0.13333334f : 0.10000000f;

        if (visibleCount == 0) {
            return budget;
        }

        if (visibleCount <= 10) {
            budget.fullRateCount = static_cast<uint32_t>(visibleCount);
            return budget;
        }

        budget.fullRateCount = 10;

        if (visibleCount <= 16) {
            budget.thirtyHzCount = static_cast<uint32_t>(visibleCount) - budget.fullRateCount;
            return budget;
        }

        budget.thirtyHzCount = 6;

        if (visibleCount <= 24) {
            budget.fifteenHzCount = static_cast<uint32_t>(visibleCount) - budget.fullRateCount - budget.thirtyHzCount;
            return budget;
        }

        if (visibleCount <= 40) {
            budget.fullRateCount = 8;
            budget.thirtyHzCount = 8;
            budget.fifteenHzCount = 12;
            return budget;
        }

        budget.fullRateCount = 6;
        budget.thirtyHzCount = 10;
        budget.fifteenHzCount = 16;
        return budget;
    }

    static float ResolveCrowdLodInterval(uint32_t rank, size_t visibleCount, bool* throttled = nullptr) {
        const CrowdLodBudget budget = ResolveCrowdLodBudget(visibleCount);
        if (rank < budget.fullRateCount) {
            if (throttled) *throttled = false;
            return 0.0f;
        }

        if (throttled) *throttled = true;
        if (rank < budget.fullRateCount + budget.thirtyHzCount) {
            return 0.03333334f;
        }
        if (rank < budget.fullRateCount + budget.thirtyHzCount + budget.fifteenHzCount) {
            return 0.06666667f;
        }
        return budget.overflowInterval;
    }

void AnimationSystem::Update(::Scene& scene, float deltaTime) {
    // =========================================================================
    // PHASE 1: Gather animation roots and initialize controllers (SEQUENTIAL)
    // =========================================================================
    // Controller/override assignment stays on the main thread so managed/native
    // runtime state stays deterministic. Heavy clip loading is requested through
    // the async cache and entities do not enter evaluation until warmup finishes.
    // =========================================================================
    std::vector<EntityID> animRoots;
        if (!scene.GetRuntimeWorld() ||
            (scene.HasPendingRuntimeWorldStructuralSyncWork() && !scene.IsRuntimeWorldFrameSyncLocked())) {
            scene.SyncRuntimeWorld(false);
        }
    const cm::world::RuntimeWorld* runtimeWorld = scene.GetRuntimeWorld();
    const size_t animRootCapacity = runtimeWorld
        ? runtimeWorld->GetAnimationRootSceneEntities().size()
        : scene.GetEntities().size();
    animRoots.reserve(animRootCapacity);

    auto collectAnimationRoot = [&scene, &animRoots](EntityID rootId) {
        auto* data = scene.GetEntityData(rootId);
        if (!data || !data->AnimationPlayer || !data->Skeleton) {
            return;
        }
        
        auto& player = *data->AnimationPlayer;
        player.DormantOffscreenIdle = false;
        if (data->Skeleton) {
            data->Skeleton->VisualWorkVisibleThisFrame = true;
        }

        if (!player.ControllerOverride && !player.ControllerOverridePath.empty()) {
            player.ControllerOverride = LoadAnimatorControllerOverrideFromFile(player.ControllerOverridePath);
        }
        
        // Auto-load controller if path is set but runtime controller not yet created
        if (!player.Controller && !player.ControllerPath.empty()) {
            player.Controller = LoadAnimatorControllerFromFile(player.ControllerPath);
            if (player.Controller) {
                player._AutoControllerGenerated = false;
                player.SyncRuntimeControllerState();
            }
        }
        
        // Auto-generate a simple single-state controller if no controller path but SingleClipPath is set
        if (!player.Controller && player.ControllerPath.empty() && !player.SingleClipPath.empty() && !player._AutoControllerGenerated) {
            // Create a simple controller with one state
            auto ctrl = std::make_shared<AnimatorController>();
            ctrl->Name = "AutoGenerated";
            AnimatorState state;
            state.Id = 0;
            state.Name = "Default";
            state.AnimationAssetPath = player.SingleClipPath;
            state.Loop = player.ActiveStates.empty() ? true : player.ActiveStates.front().Loop;
            state.Speed = 1.0f;
            ctrl->States.push_back(state);
            ctrl->DefaultState = 0;
            
            player.Controller = ctrl;
            player._AutoControllerGenerated = true;
            player.SyncRuntimeControllerState();
        }
        
        EnsureAnimatorRuntimeState(player);

        if (player.Controller && !player._InitApplied) {
            player._InitApplied = true;
            if (player.PlayOnStart) player.IsPlaying = true;
        }

        // Only add to evaluation list if controller is ready
        if (player.Controller) {
            if (!player.PreloadAllControllerAssets(scene.m_IsPlaying)) {
                return;
            }
            animRoots.push_back(rootId);
        }
    };

    if (runtimeWorld) {
        for (EntityID rootId : runtimeWorld->GetAnimationRootSceneEntities()) {
            collectAnimationRoot(rootId);
        }
    } else {
        for (const auto& ent : scene.GetEntities()) {
            collectAnimationRoot(ent.GetID());
        }
    }
    
    struct AnimationEvalRequest {
        EntityID rootId = 0;
        bool stateOnly = false;
    };
    static uint64_t s_AnimationLodFrameCounter = 0;
    ++s_AnimationLodFrameCounter;

    const size_t totalEntityCount = animRoots.size();
    thread_local std::vector<AnimationEvalRequest> s_evalRequests;
    std::vector<AnimationEvalRequest>& evalRequests = s_evalRequests;
    evalRequests.clear();
    evalRequests.reserve(totalEntityCount);
    uint64_t lodSkippedCount = 0;
    uint64_t lodCriticalCount = 0;
    uint64_t lodMotionCriticalCount = 0;
    uint64_t lodCrowdThrottledCount = 0;
    uint64_t lodVisibleCandidateCount = 0;
    uint64_t lodStateOnlyCount = 0;

    const EntityID activeCameraOwnerEntity =
        scene.m_IsPlaying ? cm::npc::ResolveActiveCameraOwnerEntity(scene) : INVALID_ENTITY_ID;
    for (EntityID rootId : animRoots) {
        auto* data = scene.GetEntityData(rootId);
        if (!data || !data->AnimationPlayer) {
            evalRequests.push_back({ rootId, false });
            continue;
        }

        AnimationPlayerComponent& player = *data->AnimationPlayer;
        const cm::npc::ScalabilityState& scalability = data->NpcScalability;
        player.LodLastDistance = scalability.CameraDistance;

        if (data->Skeleton) {
            data->Skeleton->VisualWorkVisibleThisFrame =
                !scene.m_IsPlaying ||
                scalability.PlayerOwnedCritical ||
                scalability.VisualVisible;
        }

        if (!scene.m_IsPlaying || !scalability.Participates) {
            player.LodAccumulatedTime = 0.0f;
            evalRequests.push_back({ rootId, false });
            continue;
        }

        if (scalability.PlayerOwnedCritical || !player.LODEnabled) {
            ++lodCriticalCount;
            player.LodAccumulatedTime = 0.0f;
            evalRequests.push_back({ rootId, false });
            continue;
        }

        if (scalability.MotionCritical) {
            ++lodMotionCriticalCount;
            player.LodAccumulatedTime = 0.0f;
            evalRequests.push_back({ rootId, false });
            continue;
        }

        if (scalability.VisualVisible) {
            ++lodVisibleCandidateCount;
        }
        if (scalability.CrowdThrottled) {
            ++lodCrowdThrottledCount;
        }

        const float updateInterval = std::max(0.0f, scalability.AnimationUpdateInterval);
        if (updateInterval <= 0.0f) {
            player.LodAccumulatedTime = 0.0f;
            evalRequests.push_back({ rootId, false });
            continue;
        }

        player.LodAccumulatedTime += deltaTime;
        if (player.LodAccumulatedTime + 1e-6f < updateInterval) {
            ++lodSkippedCount;
            continue;
        }

        player.LodAccumulatedTime = 0.0f;
        if (scalability.StateOnlyWhenScheduled) {
            ++lodStateOnlyCount;
        }
        evalRequests.push_back({ rootId, scalability.StateOnlyWhenScheduled });
    }

    Profiler::Get().SetCounter("Animation/EntitiesTotal", static_cast<uint64_t>(totalEntityCount));
    Profiler::Get().SetCounter("Animation/EntitiesEvaluated", static_cast<uint64_t>(evalRequests.size()));
    Profiler::Get().SetCounter("Animation/LodSkipped", lodSkippedCount);
    Profiler::Get().SetCounter("Animation/LodCritical", lodCriticalCount);
    Profiler::Get().SetCounter("Animation/LodMotionCritical", lodMotionCriticalCount);
    Profiler::Get().SetCounter("Animation/LodCrowdThrottled", lodCrowdThrottledCount);
    Profiler::Get().SetCounter("Animation/LodVisibleCandidates", lodVisibleCandidateCount);
    Profiler::Get().SetCounter("Animation/StateOnlyTicks", lodStateOnlyCount);
    const bool collectDetailedPrefabPerf = cm::debug::PrefabPerfDetailedTimingsEnabled();
    constexpr uint32_t kCrowdPosePhaseBuckets = 48u;
    constexpr uint32_t kCrowdPoseBlendBuckets = 24u;

    struct CrowdPoseShareCache {
        std::mutex Mutex;
        std::unordered_map<CrowdPoseShareKey,
                           std::vector<glm::mat4>,
                           CrowdPoseShareKeyHash> Entries;
    };

    CrowdPoseShareCache crowdPoseShareCache;
    crowdPoseShareCache.Entries.reserve(evalRequests.size());
    std::atomic<uint64_t> crowdPoseShareEligibleCount{0};
    std::atomic<uint64_t> crowdPoseShareHitCount{0};
    std::atomic<uint64_t> crowdPoseShareLeaderCount{0};
    std::atomic<uint64_t> boneEntityWritebackRoots{0};
    std::atomic<uint64_t> boneEntityWritebackSkippedRoots{0};
    std::atomic<uint64_t> boneEntityWritebackBones{0};
    std::atomic<uint64_t> boneEntityWritebackSkippedBones{0};
    std::atomic<uint64_t> posePaletteUpdatedRoots{0};

    // =========================================================================
    // PHASE 2: Parallel Animation Evaluation
    // =========================================================================
    // Each animated entity is evaluated independently:
    // - FSM state updates
    // - Animation sampling
    // - Bone transform writes (each entity owns its own bones)
    //
    // Thread safety is ensured by:
    // - thread_local BindingCache and PoseBuffers
    // - Each entity only writes to its own skeleton bones
    // - Shared controller data is read-only after initialization
    // =========================================================================
    const size_t entityCount = evalRequests.size();
    std::vector<double> evalRootTimes;
    if (collectDetailedPrefabPerf) {
        evalRootTimes.assign(entityCount, 0.0);
    }

    struct ScopedPrefabEvalTimer {
        bool Enabled = false;
        std::vector<double>* Samples = nullptr;
        size_t Index = 0;
        std::chrono::high_resolution_clock::time_point Start{};

        ScopedPrefabEvalTimer(bool enabled, std::vector<double>* samples, size_t index)
            : Enabled(enabled),
              Samples(samples),
              Index(index),
              Start(enabled ? std::chrono::high_resolution_clock::now()
                            : std::chrono::high_resolution_clock::time_point{}) {}

        ~ScopedPrefabEvalTimer() {
            if (!Enabled || !Samples || Index >= Samples->size()) {
                return;
            }

            const auto end = std::chrono::high_resolution_clock::now();
            (*Samples)[Index] =
                std::chrono::duration<double, std::milli>(end - Start).count();
        }
    };

    // Use parallel_for only once the work is large enough to amortize queue/wait
    // overhead. Two or three humanoids are faster inline in Debug/editor builds.
    constexpr size_t kMinParallelAnimationRoots = 8;
    const bool useParallel =
        entityCount >= kMinParallelAnimationRoots &&
        cm::g_JobSystem != nullptr;
    
    auto evaluateEntity =
        [&scene,
         deltaTime,
         &evalRequests,
         &evalRootTimes,
         kCrowdPosePhaseBuckets,
         kCrowdPoseBlendBuckets,
         &crowdPoseShareCache,
         &crowdPoseShareEligibleCount,
         &crowdPoseShareHitCount,
         &crowdPoseShareLeaderCount,
         &boneEntityWritebackRoots,
         &boneEntityWritebackSkippedRoots,
         &boneEntityWritebackBones,
         &boneEntityWritebackSkippedBones,
         &posePaletteUpdatedRoots,
         activeCameraOwnerEntity,
         collectDetailedPrefabPerf](size_t evalIndex) {
        ScopedPrefabEvalTimer prefabEvalTimer(
            collectDetailedPrefabPerf, &evalRootTimes, evalIndex);
        const AnimationEvalRequest& evalRequest = evalRequests[evalIndex];
        const EntityID rootId = evalRequest.rootId;
        const bool stateOnlyRequested = evalRequest.stateOnly;
        auto* data = scene.GetEntityData(rootId);
        if (!data || !data->AnimationPlayer || !data->Skeleton) return;
        auto& player   = *data->AnimationPlayer;
        auto& skeleton = *data->Skeleton;
        player._CurrentRootMotionMode = RootMotionMode::None;

        // Skip animation evaluation if disabled (e.g., ragdoll is controlling bones)
        // Bone transforms remain unchanged - ragdoll or other systems can update them
        if (!player.Enabled) return;

        // If still no controller (no path and no single clip), skip this entity
        if (!player.Controller) {
            if (!player.ActiveStates.empty()) {
                player.ActiveStates.front().Asset = nullptr;
                player.ActiveStates.front().LegacyClip = nullptr;
            }
            player.Debug_CurrentAnimationName.clear();
            player.Debug_CurrentControllerStateName.clear();
            return; // Lambda: use return instead of continue
        }

        // Predeclare evaluation context shared across phases (needed for Blend1D sampling later)
        const cm::animation::AnimatorState* stNowForEval = nullptr;
        std::shared_ptr<cm::animation::AnimationAsset> assetNow;
        std::shared_ptr<cm::animation::AnimationAsset> assetB0, assetB1;
        thread_local std::vector<std::shared_ptr<cm::animation::AnimationAsset>> s_blendAssets2D;
        thread_local std::vector<float> s_blendWeights2D;
        auto& blendAssets2D = s_blendAssets2D;
        auto& blendWeights2D = s_blendWeights2D;
        blendAssets2D.clear();
        blendWeights2D.clear();
        if (blendAssets2D.capacity() < 4u) {
            blendAssets2D.reserve(4u);
        }
        if (blendWeights2D.capacity() < 4u) {
            blendWeights2D.reserve(4u);
        }
        int blend1DIndexA = -1;
        int blend1DIndexB = -1;
        std::array<int, 4> activeBlend2DIndices{};
        int activeBlend2DCount = 0;
        float durationNow = 0.0f;
        float blendT = 0.0f;
        bool useBlend1D = false;
        bool useBlend2D = false;
        skeleton.EnsureBindPoseLocals();

        auto makeBlendCacheKey = [](int layerIdx, int stateId, int entryIndex) -> int {
            if (layerIdx > 0) {
                return (layerIdx * 10000) + stateId * 100 + entryIndex;
            }
            return stateId * 1000 + entryIndex;
        };
        auto loadBlendAsset = [&](int layerIdx, int stateId, int entryIndex, const std::string& path)
            -> std::shared_ptr<cm::animation::AnimationAsset> {
            if (path.empty()) return nullptr;
            const int cacheKey = makeBlendCacheKey(layerIdx, stateId, entryIndex);
            auto it = player.CachedAssets.find(cacheKey);
            if (it != player.CachedAssets.end()) {
                return it->second;
            }
            auto asset = cm::animation::LoadAnimationAssetCached(path, true);
            if (asset && asset->Duration() > 0.0f) {
                player.CachedAssets[cacheKey] = asset;
                return asset;
            }
            return nullptr;
        };
        constexpr float kBlend1DEndpointEpsilon = 1e-4f;
        auto collapseBlend1DContributors = [=](
            std::shared_ptr<cm::animation::AnimationAsset>& assetA,
            std::shared_ptr<cm::animation::AnimationAsset>& assetB,
            int& indexA,
            int& indexB,
            float& t,
            float& duration) {
            const float dA = assetA ? assetA->Duration() : 0.0f;
            const float dB = assetB ? assetB->Duration() : 0.0f;
            const bool sameContributor = (indexA == indexB) || (assetA && assetA == assetB);
            if (sameContributor || t <= kBlend1DEndpointEpsilon || !assetB) {
                assetB.reset();
                indexB = indexA;
                t = 0.0f;
                duration = dA;
                return;
            }
            if (t >= 1.0f - kBlend1DEndpointEpsilon || !assetA) {
                assetA = assetB;
                assetB.reset();
                indexA = indexB;
                t = 0.0f;
                duration = dB;
                return;
            }
            duration = glm::mix(dA, dB, t);
        };
        auto resolveBlend2DWeights = [&](const cm::animation::AnimatorState* blendState,
                                         const cm::animation::AnimatorBlackboard& blackboard,
                                         std::array<int, 4>& outIndices,
                                         std::array<float, 4>& outWeights,
                                         int& outCount) -> bool {
            outCount = 0;
            if (!blendState || blendState->Blend2DEntries.empty()) return false;

            float x = 0.0f;
            float y = 0.0f;
            x = blackboard.GetFloatSlot(blendState->RuntimeBlend2DParamXSlot);
            y = blackboard.GetFloatSlot(blendState->RuntimeBlend2DParamYSlot);

            struct Candidate {
                int index = -1;
                float d2 = 0.0f;
            };
            constexpr int kMaxBlend2DSamples = 4;
            std::array<Candidate, kMaxBlend2DSamples> nearest{};
            int nearestCount = 0;

            auto insertCandidate = [&](int index, float d2) {
                int insertPos = nearestCount;
                if (nearestCount < kMaxBlend2DSamples) {
                    nearestCount++;
                } else {
                    if (d2 >= nearest[kMaxBlend2DSamples - 1].d2) {
                        return;
                    }
                    insertPos = kMaxBlend2DSamples - 1;
                }

                nearest[insertPos] = { index, d2 };
                while (insertPos > 0 && nearest[insertPos].d2 < nearest[insertPos - 1].d2) {
                    std::swap(nearest[insertPos], nearest[insertPos - 1]);
                    --insertPos;
                }
            };

            for (int i = 0; i < static_cast<int>(blendState->Blend2DEntries.size()); ++i) {
                const auto& entry = blendState->Blend2DEntries[i];
                const ResolvedAnimationBinding binding = ResolveAnimationBinding(player, entry);
                const std::string& path = binding.EffectivePath();
                if (path.empty()) continue;
                const float dx = x - entry.X;
                const float dy = y - entry.Y;
                const float d2 = dx * dx + dy * dy;
                if (d2 <= 1e-6f) {
                    outIndices[0] = i;
                    outWeights[0] = 1.0f;
                    outCount = 1;
                    return true;
                }
                insertCandidate(i, d2);
            }
            if (nearestCount <= 0) return false;

            float weightSum = 0.0f;
            for (int i = 0; i < nearestCount; ++i) {
                outIndices[i] = nearest[i].index;
                outWeights[i] = 1.0f / std::max(1e-4f, nearest[i].d2);
                weightSum += outWeights[i];
            }
            if (weightSum <= 1e-6f) return false;

            const float invWeightSum = 1.0f / weightSum;
            for (int i = 0; i < nearestCount; ++i) {
                outWeights[i] *= invWeightSum;
            }
            outCount = nearestCount;
            return true;
        };

        // Animator controller update (always driven by controller now)
        if (player.Controller) {
            // Load default state clip if needed
            if (player.CurrentStateId < 0) {
                player.SyncRuntimeControllerState();
            }

            const auto* st = player.Controller->FindStateInLayer(0, player.CurrentStateId);
            if (!st) return; // Lambda: use return instead of continue
            const ResolvedAnimationBinding stateBinding = ResolveAnimationBinding(player, *st);
            // Load or get cached unified asset (prefer) or legacy clip (fallback)
            std::shared_ptr<cm::animation::AnimationAsset> asset;
            if (!stateBinding.AssetPath.empty()) {
                auto ita = player.CachedAssets.find(st->Id);
                if (ita != player.CachedAssets.end()) asset = ita->second; else {
                    asset = std::make_shared<cm::animation::AnimationAsset>(cm::animation::LoadAnimationAsset(stateBinding.AssetPath));
                    player.CachedAssets[st->Id] = asset;
                }
            }
            std::shared_ptr<cm::animation::AnimationClip> clip;
            if (!asset && !stateBinding.ClipPath.empty()) {
                auto itc = player.CachedClips.find(st->Id);
                if (itc != player.CachedClips.end()) clip = itc->second; else {
                    clip = std::make_shared<cm::animation::AnimationClip>(cm::animation::LoadAnimationClip(stateBinding.ClipPath));
                    player.CachedClips[st->Id] = clip;
                }
            }
            // Advance animator time; if blend tree, use blended duration so normalized time progresses
            float currentDuration = 0.0f;
            if (st->Kind == cm::animation::AnimatorStateKind::Blend1D && !st->Blend1DEntries.empty()) {
                const float x = ReadBlend1DParameter(player.AnimatorInstance.Blackboard(), *st);
                const auto& e = st->Blend1DEntries;
                int i1 = 0, i2 = (int)e.size()-1;
                for (int i=0;i<(int)e.size();++i) { if (e[i].Key <= x) i1 = i; if (e[i].Key >= x) { i2 = i; break; } }
                i1 = glm::clamp(i1, 0, (int)e.size()-1); i2 = glm::clamp(i2, 0, (int)e.size()-1);
                const auto& a = e[i1]; const auto& b = e[i2];
                float denom = std::max(1e-6f, (b.Key - a.Key));
                float t = glm::clamp((x - a.Key) / denom, 0.0f, 1.0f);
                // Resolve durations for a/b using the player's per-state duration cache.
                const ResolvedAnimationBinding bindingA = ResolveAnimationBinding(player, a);
                const ResolvedAnimationBinding bindingB = ResolveAnimationBinding(player, b);
                const int cacheKeyA = st->Id * 1000 + i1;
                const int cacheKeyB = st->Id * 1000 + i2;
                const float d0 = LookupOrLoadAnimationDuration(player, cacheKeyA, bindingA);
                if (i1 == i2 || t <= kBlend1DEndpointEpsilon) {
                    currentDuration = d0;
                } else {
                    const float d1 = LookupOrLoadAnimationDuration(player, cacheKeyB, bindingB);
                    currentDuration = (t >= 1.0f - kBlend1DEndpointEpsilon) ? d1 : glm::mix(d0, d1, t);
                }
            } else if (st->Kind == cm::animation::AnimatorStateKind::Blend2D && !st->Blend2DEntries.empty()) {
                std::array<int, 4> indices{};
                std::array<float, 4> weights{};
                int sampleCount = 0;
                if (resolveBlend2DWeights(st, player.AnimatorInstance.Blackboard(), indices, weights, sampleCount)) {
                    float blendedDuration = 0.0f;
                    float loadedWeight = 0.0f;
                    for (int i = 0; i < sampleCount; ++i) {
                        const auto& entry = st->Blend2DEntries[indices[i]];
                        const ResolvedAnimationBinding binding = ResolveAnimationBinding(player, entry);
                        const std::string& path = binding.EffectivePath();
                        auto entryAsset = loadBlendAsset(0, st->Id, indices[i], path);
                        if (!entryAsset) continue;
                        blendedDuration += weights[i] * entryAsset->Duration();
                        loadedWeight += weights[i];
                    }
                    if (loadedWeight > 1e-6f) {
                        currentDuration = blendedDuration / loadedWeight;
                    }
                }
            } else {
                currentDuration = asset ? asset->Duration() : (clip ? clip->Duration : 0.0f);
            }
            player.AnimatorInstance.Update(deltaTime * st->Speed * player.PlaybackSpeed, currentDuration);
            // Check transitions
            int next = player.AnimatorInstance.ChooseNextState();
            if (next >= 0 && next != player.CurrentStateId) {
                // Do not restart the same crossfade every frame; allow it to progress
                const auto& pb = player.AnimatorInstance.Playback();
                bool alreadyCrossfadingToSame = player.AnimatorInstance.IsCrossfading() && pb.NextStateId == next;
                if (!alreadyCrossfadingToSame) {
                // Query the exact transition selected by Animator
                float duration = 0.0f; bool durationNormalized = false;
                const auto* firedTransition = player.AnimatorInstance.FindTransitionTo(next);
                if (firedTransition) {
                    duration = firedTransition->Duration;
                    durationNormalized = firedTransition->DurationNormalized;
                }
                // If normalized, convert to seconds using current state's resolved duration
                float durationSeconds = durationNormalized ? (duration * std::max(0.0f, currentDuration)) : duration;
                if (durationSeconds > 0.0f) {
                    player.AnimatorInstance.BeginCrossfade(next, durationSeconds);
                    // Only consume triggers used by THIS transition (allows other layers to use their own triggers)
                    player.AnimatorInstance.ConsumeTriggersForTransition(firedTransition);
                    // Reset InPlace baseline so Y-offset is recalculated from the blended pose
                    player._InPlaceBaselineValid = false;
                } else {
                    // Instant transition: synchronize Animator's internal state and player state
                    player.AnimatorInstance.SetCurrentState(next, /*resetTime*/true);
                    player.CurrentStateId = next;
                    // Only consume triggers used by THIS transition (allows other layers to use their own triggers)
                    player.AnimatorInstance.ConsumeTriggersForTransition(firedTransition);
                    // Reset InPlace baseline for the new state
                    player._InPlaceBaselineValid = false;
                }
                }
            } else {
                // No valid transition found - consume any triggers from AnyState transitions
                // that were blocked because they target the current state (CanTransitionToSelf=false).
                // This prevents triggers from being "stuck" and firing unexpectedly later.
                player.AnimatorInstance.ConsumeSelfTransitionTriggers();
            }

            // Evaluate current (possibly updated) state asset at time – prefer unified asset if present
            stNowForEval = player.Controller->FindStateInLayer(0, player.CurrentStateId);
            if (stNowForEval) {
                useBlend1D = false;
                useBlend2D = false;
                blendAssets2D.clear();
                blendWeights2D.clear();
                blend1DIndexA = -1;
                blend1DIndexB = -1;
                activeBlend2DCount = 0;
                if (stNowForEval->Kind == cm::animation::AnimatorStateKind::Blend1D && !stNowForEval->Blend1DEntries.empty()) {
                    const float x = ReadBlend1DParameter(player.AnimatorInstance.Blackboard(), *stNowForEval);
                    // Find two surrounding entries
                    const auto& e = stNowForEval->Blend1DEntries;
                    int i1 = 0, i2 = (int)e.size()-1;
                    for (int i=0;i<(int)e.size();++i) { if (e[i].Key <= x) i1 = i; if (e[i].Key >= x) { i2 = i; break; } }
                    i1 = glm::clamp(i1, 0, (int)e.size()-1); i2 = glm::clamp(i2, 0, (int)e.size()-1);
                    blend1DIndexA = i1;
                    blend1DIndexB = i2;
                    const auto& a = e[i1]; const auto& b = e[i2];
                    float denom = std::max(1e-6f, (b.Key - a.Key));
                    blendT = glm::clamp((x - a.Key) / denom, 0.0f, 1.0f);
                    // Resolve assets for a and b (prefer AssetPath, fallback to ClipPath loaded as AnimationAsset)
                    const ResolvedAnimationBinding bindingA = ResolveAnimationBinding(player, a);
                    const ResolvedAnimationBinding bindingB = ResolveAnimationBinding(player, b);
                    const std::string& pathA = bindingA.EffectivePath();
                    const std::string& pathB = bindingB.EffectivePath();
                    if (!pathA.empty()) { auto ita = player.CachedAssets.find(stNowForEval->Id * 1000 + i1); if (ita != player.CachedAssets.end()) assetB0 = ita->second; else { assetB0 = std::make_shared<cm::animation::AnimationAsset>(cm::animation::LoadAnimationAsset(pathA)); player.CachedAssets[stNowForEval->Id * 1000 + i1] = assetB0; } }
                    if (!pathB.empty()) { auto ita = player.CachedAssets.find(stNowForEval->Id * 1000 + i2); if (ita != player.CachedAssets.end()) assetB1 = ita->second; else { assetB1 = std::make_shared<cm::animation::AnimationAsset>(cm::animation::LoadAnimationAsset(pathB)); player.CachedAssets[stNowForEval->Id * 1000 + i2] = assetB1; } }
                    useBlend1D = true;
                    collapseBlend1DContributors(assetB0, assetB1, blend1DIndexA, blend1DIndexB, blendT, durationNow);
                } else if (stNowForEval->Kind == cm::animation::AnimatorStateKind::Blend2D && !stNowForEval->Blend2DEntries.empty()) {
                    std::array<int, 4> indices{};
                    std::array<float, 4> weights{};
                    int sampleCount = 0;
                    if (resolveBlend2DWeights(stNowForEval, player.AnimatorInstance.Blackboard(), indices, weights, sampleCount)) {
                        float loadedWeight = 0.0f;
                        durationNow = 0.0f;
                        for (int i = 0; i < sampleCount; ++i) {
                            const auto& entry = stNowForEval->Blend2DEntries[indices[i]];
                            const ResolvedAnimationBinding binding = ResolveAnimationBinding(player, entry);
                            const std::string& path = binding.EffectivePath();
                            auto assetEntry = loadBlendAsset(0, stNowForEval->Id, indices[i], path);
                            if (!assetEntry) continue;
                            if (activeBlend2DCount < static_cast<int>(activeBlend2DIndices.size())) {
                                activeBlend2DIndices[activeBlend2DCount++] = indices[i];
                            }
                            blendAssets2D.push_back(assetEntry);
                            blendWeights2D.push_back(weights[i]);
                            durationNow += weights[i] * assetEntry->Duration();
                            loadedWeight += weights[i];
                        }
                        if (loadedWeight > 1e-6f) {
                            for (float& w : blendWeights2D) {
                                w /= loadedWeight;
                            }
                            durationNow /= loadedWeight;
                            useBlend2D = !blendAssets2D.empty();
                        } else {
                            durationNow = 0.0f;
                        }
                    }
                } else {
                    // Prefer AnimationAssetPath, but also support ClipPath loaded as AnimationAsset
                    // This ensures humanoid retargeting works for both path types
                    const ResolvedAnimationBinding binding = ResolveAnimationBinding(player, *stNowForEval);
                    const std::string& assetPath = binding.EffectivePath();
                    if (!assetPath.empty()) {
                        auto ita = player.CachedAssets.find(stNowForEval->Id);
                        if (ita != player.CachedAssets.end()) assetNow = ita->second; else {
                            assetNow = std::make_shared<cm::animation::AnimationAsset>(cm::animation::LoadAnimationAsset(assetPath));
                            player.CachedAssets[stNowForEval->Id] = assetNow;
                        }
                    }
                    durationNow = assetNow ? assetNow->Duration() : 0.0f;
                }
            }

            if (player.ActiveStates.empty()) player.ActiveStates.push_back({});
            AnimationState& s0 = player.ActiveStates.front();
            s0.Asset = assetNow.get();
            s0.LegacyClip = nullptr; // Legacy path removed
            s0.Loop = stNowForEval ? stNowForEval->Loop : true;
            // Derive time from absolute state time so parameter changes (which alter duration) don't cause jumps
            float baseT = player.AnimatorInstance.Playback().StateTime;
            float time = (durationNow > 0.0f) ? fmod(baseT, durationNow) : 0.0f;
            if (!std::isfinite(time) || time < 0.0f) time = 0.0f;
            s0.Time = time;

            // Debug info - show target state during crossfade, otherwise current state
            if (player.AnimatorInstance.IsCrossfading()) {
                const auto& pb = player.AnimatorInstance.Playback();
                if (pb.NextStateId >= 0) {
                    const auto* targetState = player.Controller->FindStateInLayer(0, pb.NextStateId);
                    if (targetState) player.Debug_CurrentControllerStateName = targetState->Name;
                }
            } else if (stNowForEval) {
                player.Debug_CurrentControllerStateName = stNowForEval->Name;
            }
            player.Debug_CurrentAnimationName = assetNow ? assetNow->name : std::string();
        }

        if (player.ActiveStates.empty()) return; // Lambda: use return instead of continue

        // Evaluate pose; if crossfading, blend between two states linearly
        const AnimationState& state = player.ActiveStates.front();
        // Allow blend-tree paths to evaluate even when no single clip/asset bound in state container
        if (!useBlend1D && !useBlend2D && !state.Asset) return; // Lambda: use return instead of continue

        // Advance time
        AnimationState& mutableState = player.ActiveStates.front();
        const float prevTime = mutableState.Time;
        // Animation advances when IsPlaying is true (controlled via Play/Stop API or PlayOnStart)
        bool shouldAdvance = player.IsPlaying;
        float rawTime = prevTime;
        if (shouldAdvance) {
            rawTime += deltaTime * player.PlaybackSpeed;
        }
        mutableState.Time = rawTime;
        
        // Debug: log animation playback state periodically
        static int s_animDebugCounter = 0;

        float clipDuration = state.Asset ? state.Asset->Duration() : 0.0f;
        int loopCount = 0;
        if (clipDuration > 0.0f && mutableState.Loop) {
            if (shouldAdvance) {
                loopCount = static_cast<int>(std::floor(rawTime / clipDuration));
                if (loopCount < 0) loopCount = 0;
                mutableState.Time = fmod(mutableState.Time, clipDuration);
            }
        } else if (clipDuration > 0.0f && player._AutoControllerGenerated) {
            // For auto-generated single-clip controllers, stop at end if not looping
            if (shouldAdvance && mutableState.Time >= clipDuration) {
                mutableState.Time = clipDuration;
                player.IsPlaying = false;
            }
        }
        if (player.AnimatorInstance.IsCrossfading()) {
            player.AnimatorInstance.AdvanceCrossfade(deltaTime * player.PlaybackSpeed);
        }
        // If crossfade just finished this tick, finalize to next state immediately so we don't retrigger
        if (!player.AnimatorInstance.IsCrossfading() && player.Controller) {
            const auto& pb = player.AnimatorInstance.Playback();
            if (pb.NextStateId >= 0 && player.CurrentStateId != pb.NextStateId) {
                // CRITICAL: Preserve the accumulated NextStateTime - do NOT reset time!
                // The animation has been playing during crossfade, resetting would cause it to restart.
                float preservedTime = pb.NextStateTime;
                player.AnimatorInstance.SetCurrentState(pb.NextStateId, /*resetTime*/false);
                player.CurrentStateId = pb.NextStateId;
                
                // CRITICAL: Update stNowForEval and reload assets for the NEW state
                // Otherwise we'd evaluate the old state's animation on this frame
                stNowForEval = player.Controller->FindStateInLayer(0, player.CurrentStateId);
                useBlend1D = false;
                useBlend2D = false;
                assetNow.reset();
                assetB0.reset(); assetB1.reset();
                blendAssets2D.clear();
                blendWeights2D.clear();
                blend1DIndexA = -1;
                blend1DIndexB = -1;
                activeBlend2DCount = 0;
                durationNow = 0.0f; // Will be set from loaded asset
                
                if (stNowForEval) {
                    if (stNowForEval->Kind == cm::animation::AnimatorStateKind::Blend1D && !stNowForEval->Blend1DEntries.empty()) {
                        // Reload Blend1D entries for the new state
                        const float x = ReadBlend1DParameter(player.AnimatorInstance.Blackboard(), *stNowForEval);
                        const auto& e = stNowForEval->Blend1DEntries;
                        int i1 = 0, i2 = (int)e.size()-1;
                        for (int i=0;i<(int)e.size();++i) { if (e[i].Key <= x) i1 = i; if (e[i].Key >= x) { i2 = i; break; } }
                        i1 = glm::clamp(i1, 0, (int)e.size()-1); i2 = glm::clamp(i2, 0, (int)e.size()-1);
                        blend1DIndexA = i1;
                        blend1DIndexB = i2;
                        const auto& a = e[i1]; const auto& b = e[i2];
                        float denom = std::max(1e-6f, (b.Key - a.Key));
                        blendT = glm::clamp((x - a.Key) / denom, 0.0f, 1.0f);
                        // Load as AnimationAsset (prefer AssetPath, fallback to ClipPath)
                        const ResolvedAnimationBinding bindingA = ResolveAnimationBinding(player, a);
                        const ResolvedAnimationBinding bindingB = ResolveAnimationBinding(player, b);
                        std::string pathA = bindingA.EffectivePath();
                        std::string pathB = bindingB.EffectivePath();
                        if (!pathA.empty()) { auto ita = player.CachedAssets.find(stNowForEval->Id * 1000 + i1); if (ita != player.CachedAssets.end()) assetB0 = ita->second; else { assetB0 = std::make_shared<cm::animation::AnimationAsset>(cm::animation::LoadAnimationAsset(pathA)); player.CachedAssets[stNowForEval->Id * 1000 + i1] = assetB0; } }
                        if (!pathB.empty()) { auto ita = player.CachedAssets.find(stNowForEval->Id * 1000 + i2); if (ita != player.CachedAssets.end()) assetB1 = ita->second; else { assetB1 = std::make_shared<cm::animation::AnimationAsset>(cm::animation::LoadAnimationAsset(pathB)); player.CachedAssets[stNowForEval->Id * 1000 + i2] = assetB1; } }
                        useBlend1D = true;
                        collapseBlend1DContributors(assetB0, assetB1, blend1DIndexA, blend1DIndexB, blendT, durationNow);
                    } else if (stNowForEval->Kind == cm::animation::AnimatorStateKind::Blend2D && !stNowForEval->Blend2DEntries.empty()) {
                        std::array<int, 4> indices{};
                        std::array<float, 4> weights{};
                        int sampleCount = 0;
                        if (resolveBlend2DWeights(stNowForEval, player.AnimatorInstance.Blackboard(), indices, weights, sampleCount)) {
                            float loadedWeight = 0.0f;
                            for (int i = 0; i < sampleCount; ++i) {
                                const auto& entry = stNowForEval->Blend2DEntries[indices[i]];
                                const std::string path = ResolveAnimationBinding(player, entry).EffectivePath();
                                auto assetEntry = loadBlendAsset(0, stNowForEval->Id, indices[i], path);
                                if (!assetEntry) continue;
                                if (activeBlend2DCount < static_cast<int>(activeBlend2DIndices.size())) {
                                    activeBlend2DIndices[activeBlend2DCount++] = indices[i];
                                }
                                blendAssets2D.push_back(assetEntry);
                                blendWeights2D.push_back(weights[i]);
                                durationNow += weights[i] * assetEntry->Duration();
                                loadedWeight += weights[i];
                            }
                            if (loadedWeight > 1e-6f) {
                                for (float& w : blendWeights2D) {
                                    w /= loadedWeight;
                                }
                                durationNow /= loadedWeight;
                                useBlend2D = !blendAssets2D.empty();
                            } else {
                                durationNow = 0.0f;
                            }
                        }


                    } else {
                        // Simple state - load as AnimationAsset for proper humanoid retargeting
                        std::string assetPath = ResolveAnimationBinding(player, *stNowForEval).EffectivePath();
                        if (!assetPath.empty()) {
                            auto ita = player.CachedAssets.find(stNowForEval->Id);
                            if (ita != player.CachedAssets.end()) assetNow = ita->second; else {
                                assetNow = std::make_shared<cm::animation::AnimationAsset>(cm::animation::LoadAnimationAsset(assetPath));
                                player.CachedAssets[stNowForEval->Id] = assetNow;
                            }
                        }
                        durationNow = assetNow ? assetNow->Duration() : 0.0f;
                    }
                    
                    // Update state container for the new state
                    if (player.ActiveStates.empty()) player.ActiveStates.push_back({});
                    AnimationState& s0 = player.ActiveStates.front();
                    s0.Asset = assetNow.get();
                    s0.LegacyClip = nullptr; // Using unified asset path now
                    s0.Loop = stNowForEval->Loop;
                    
                    // CRITICAL: Preserve time from crossfade, properly wrapped to state duration
                    // This ensures the animation continues from where it was, not from 0
                    s0.Time = (durationNow > 0.0f && s0.Loop) ? fmod(preservedTime, durationNow) : preservedTime;
                    
                    // Also update animator's state time for proper exit time evaluation
                    player.AnimatorInstance.SetStateTime(preservedTime, durationNow);
                }
                
                // Reset InPlace baseline for the new state
                player._InPlaceBaselineValid = false;
            }
        }

        const bool canSkipVisualPoseWork =
            stateOnlyRequested &&
            !player._ControllerSwitchBlendActive &&
            !AnimationPlayerHasActiveOverlayLayers(player) &&
            [&]() {
                if (useBlend1D) {
                    if (!assetB0 && !assetB1) {
                        return false;
                    }
                    if (assetB0 && !AnimationAssetAllowsCrowdPoseShare(assetB0.get())) {
                        return false;
                    }
                    if (assetB1 && !AnimationAssetAllowsCrowdPoseShare(assetB1.get())) {
                        return false;
                    }
                    return true;
                }

                if (useBlend2D) {
                    if (blendAssets2D.empty()) {
                        return false;
                    }
                    for (const auto& sampleAsset : blendAssets2D) {
                        if (!AnimationAssetAllowsCrowdPoseShare(sampleAsset.get())) {
                            return false;
                        }
                    }
                    return true;
                }

                return state.Asset && AnimationAssetAllowsCrowdPoseShare(state.Asset);
            }();
        if (canSkipVisualPoseWork) {
            player.ResetRootMotionTracking();
            player.AnimatorInstance.ConsumeTriggers();
            return;
        }

        thread_local std::vector<glm::mat4> s_localTransforms;
        std::vector<glm::mat4>& localTransforms = s_localTransforms;
        localTransforms.clear();

        const size_t baseBoneCount = skeleton.BoneEntities.size();
        // Helper to compute local bind transform for a bone index
        auto computeLocalBind = [&](int boneIndex) -> glm::mat4 {
            if (boneIndex < 0 || boneIndex >= static_cast<int>(skeleton.BindPoseLocals.size())) {
                return glm::mat4(1.0f);
            }
            return skeleton.BindPoseLocals[static_cast<size_t>(boneIndex)];
        };
        auto getBindLocalTRS = [&](int boneIndex,
                                   glm::vec3& T,
                                   glm::quat& R,
                                   glm::vec3& S) {
            const size_t bindIndex = static_cast<size_t>(std::max(boneIndex, 0));
            if (boneIndex >= 0 &&
                bindIndex < skeleton.BindPoseLocalTranslations.size() &&
                bindIndex < skeleton.BindPoseLocalRotations.size() &&
                bindIndex < skeleton.BindPoseLocalScales.size()) {
                T = skeleton.BindPoseLocalTranslations[bindIndex];
                R = skeleton.BindPoseLocalRotations[bindIndex];
                S = skeleton.BindPoseLocalScales[bindIndex];
                return;
            }
            decomposeTRS(computeLocalBind(boneIndex), T, R, S);
        };
        auto preparePoseBuffer = [&](PoseBuffer& pose) {
            if (pose.local.size() != baseBoneCount) {
                pose.local.resize(baseBoneCount);
            }
            if (pose.touched.size() != baseBoneCount) {
                pose.touched.resize(baseBoneCount);
            }
            std::fill(pose.touched.begin(), pose.touched.end(), false);
        };
        auto fillUntouchedFromBind = [&](PoseBuffer& pose) {
            if (pose.touched.size() < pose.local.size()) {
                pose.touched.resize(pose.local.size(), false);
            }
            for (int i = 0; i < static_cast<int>(pose.local.size()); ++i) {
                if (!pose.touched[static_cast<size_t>(i)]) {
                    pose.local[static_cast<size_t>(i)] = computeLocalBind(i);
                }
            }
        };
        auto preparePoseTrsBuffer = [&](PoseTRSBuffer& pose) {
            if (pose.translation.size() != baseBoneCount) {
                pose.translation.resize(baseBoneCount, glm::vec3(0.0f));
            }
            if (pose.rotation.size() != baseBoneCount) {
                pose.rotation.resize(baseBoneCount, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
            }
            if (pose.scale.size() != baseBoneCount) {
                pose.scale.resize(baseBoneCount, glm::vec3(1.0f));
            }
            if (pose.touched.size() != baseBoneCount) {
                pose.touched.resize(baseBoneCount);
            }
            std::fill(pose.touched.begin(), pose.touched.end(), false);
        };
        auto fillUntouchedFromBindTrs = [&](PoseTRSBuffer& pose) {
            if (pose.touched.size() < baseBoneCount) {
                pose.touched.resize(baseBoneCount, false);
            }
            if (pose.translation.size() < baseBoneCount) {
                pose.translation.resize(baseBoneCount, glm::vec3(0.0f));
            }
            if (pose.rotation.size() < baseBoneCount) {
                pose.rotation.resize(baseBoneCount, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
            }
            if (pose.scale.size() < baseBoneCount) {
                pose.scale.resize(baseBoneCount, glm::vec3(1.0f));
            }
            for (size_t i = 0; i < baseBoneCount; ++i) {
                if (!pose.touched[i]) {
                    getBindLocalTRS(static_cast<int>(i), pose.translation[i], pose.rotation[i], pose.scale[i]);
                    pose.touched[i] = true;
                }
            }
        };
        auto materializePoseTrs = [&](const PoseTRSBuffer& pose, std::vector<glm::mat4>& out) {
            out.resize(baseBoneCount);
            for (size_t i = 0; i < baseBoneCount; ++i) {
                out[i] = glm::translate(glm::mat4(1.0f), pose.translation[i]) *
                         glm::mat4_cast(glm::normalize(pose.rotation[i])) *
                         glm::scale(glm::mat4(1.0f), pose.scale[i]);
            }
        };
        bool sampledPoseNeedsRuntimeScalePreservation = false;
        auto noteSampledAssetScaleMode = [&](const AnimationAsset* asset) {
            sampledPoseNeedsRuntimeScalePreservation =
                sampledPoseNeedsRuntimeScalePreservation ||
                AnimationAssetNeedsRuntimeScalePreservation(asset);
        };

        CrowdPoseShareKey crowdPoseShareKey{};
        bool crowdPoseShareEligible = false;
        bool reusedCrowdSharedPose = false;
        thread_local BindingCache s_sampleBindings;
        s_sampleBindings.SetScene(&scene);
        s_sampleBindings.SetSkeleton(&skeleton);
        const bool playerOwnedAnimatorRoot = IsEntityDescendantOf(
            scene,
            rootId,
            activeCameraOwnerEntity);
        if (scene.m_IsPlaying &&
            stNowForEval &&
            player.Controller &&
            player.Enabled &&
            player.IsPlaying &&
            player.CrowdThrottleEnabled &&
            !playerOwnedAnimatorRoot &&
            stNowForEval->Loop &&
            !player.AnimatorInstance.IsCrossfading() &&
            !player._ControllerSwitchBlendActive &&
            !AnimationPlayerHasActiveOverlayLayers(player) &&
            !HasArmedTriggers(player.AnimatorInstance.Blackboard()) &&
            !HasActiveLookAtConstraint(*data) &&
            !HasPotentialIkConstraint(*data)) {
            bool allAssetsShareable = false;
            if (useBlend1D) {
                allAssetsShareable = (assetB0 || assetB1);
                if (assetB0 && !AnimationAssetAllowsCrowdPoseShare(assetB0.get())) {
                    allAssetsShareable = false;
                }
                if (assetB1 && !AnimationAssetAllowsCrowdPoseShare(assetB1.get())) {
                    allAssetsShareable = false;
                }
            } else if (useBlend2D) {
                allAssetsShareable = !blendAssets2D.empty();
                for (const auto& sampleAsset : blendAssets2D) {
                    if (!AnimationAssetAllowsCrowdPoseShare(sampleAsset.get())) {
                        allAssetsShareable = false;
                        break;
                    }
                }
            } else if (state.Asset) {
                allAssetsShareable = AnimationAssetAllowsCrowdPoseShare(state.Asset);
            }

            if (allAssetsShareable) {
                crowdPoseShareKey.SkeletonSignature =
                    BuildCrowdPoseSkeletonSignature(skeleton);
                crowdPoseShareKey.ControllerSignature =
                    player.GetCrowdPoseControllerSignature();
                crowdPoseShareKey.StateId = stNowForEval->Id;
                crowdPoseShareKey.StateKind =
                    static_cast<uint8_t>(stNowForEval->Kind);
                if (durationNow > 1e-6f) {
                    const float normalizedTime =
                        std::fmod(std::max(0.0f, mutableState.Time), durationNow) /
                        durationNow;
                    crowdPoseShareKey.PhaseBucket = QuantizeCrowdPoseUnitValue(
                        normalizedTime,
                        kCrowdPosePhaseBuckets);
                }

                if (useBlend1D) {
                    const float blendX = ReadBlend1DParameter(
                        player.AnimatorInstance.Blackboard(),
                        *stNowForEval);
                    const Blend1DKeyRange blendRange = GetBlend1DKeyRange(*stNowForEval);
                    crowdPoseShareKey.ParamBucketX = QuantizeCrowdPoseRangeValue(
                        blendX,
                        blendRange.Min,
                        blendRange.Max,
                        kCrowdPoseBlendBuckets);
                    crowdPoseShareKey.BlendEntryA =
                        (blend1DIndexA >= 0)
                            ? static_cast<uint16_t>(blend1DIndexA)
                            : 0xffffu;
                    crowdPoseShareKey.BlendEntryB =
                        (blend1DIndexB >= 0)
                            ? static_cast<uint16_t>(blend1DIndexB)
                            : 0xffffu;
                } else if (useBlend2D) {
                    float blendX = glm::clamp(
                        player.AnimatorInstance.GetFloatSlot(
                            stNowForEval->RuntimeBlend2DParamXSlot,
                            0.0f),
                        -1.0f,
                        1.0f);
                    float blendY = glm::clamp(
                        player.AnimatorInstance.GetFloatSlot(
                            stNowForEval->RuntimeBlend2DParamYSlot,
                            0.0f),
                        -1.0f,
                        1.0f);
                    crowdPoseShareKey.ParamBucketX = QuantizeCrowdPoseRangeValue(
                        blendX,
                        -1.0f,
                        1.0f,
                        kCrowdPoseBlendBuckets);
                    crowdPoseShareKey.ParamBucketY = QuantizeCrowdPoseRangeValue(
                        blendY,
                        -1.0f,
                        1.0f,
                        kCrowdPoseBlendBuckets);
                    for (int i = 0; i < activeBlend2DCount &&
                                    i < static_cast<int>(crowdPoseShareKey.Blend2DEntries.size());
                         ++i) {
                        crowdPoseShareKey.Blend2DEntries[static_cast<size_t>(i)] =
                            static_cast<uint16_t>(activeBlend2DIndices[i]);
                    }
                }

                crowdPoseShareEligible = true;
                crowdPoseShareEligibleCount.fetch_add(1, std::memory_order_relaxed);

                std::lock_guard<std::mutex> lock(crowdPoseShareCache.Mutex);
                auto cachedPoseIt =
                    crowdPoseShareCache.Entries.find(crowdPoseShareKey);
                if (cachedPoseIt != crowdPoseShareCache.Entries.end()) {
                    localTransforms = cachedPoseIt->second;
                    reusedCrowdSharedPose = true;
                    crowdPoseShareHitCount.fetch_add(
                        1,
                        std::memory_order_relaxed);
                }
            }
        }

        thread_local PoseTRSBuffer workingPoseTrs;
        bool workingPoseTrsValid = false;
        if (!reusedCrowdSharedPose) {
        if (stNowForEval && stNowForEval->Kind == cm::animation::AnimatorStateKind::Blend1D && useBlend1D) {
            // Exact Blend1D endpoints collapse to one sample; otherwise sample both and blend.
            thread_local PoseTRSBuffer poseA;
            preparePoseTrsBuffer(poseA);
            const float baseT = player.AnimatorInstance.Playback().StateTime;
            const float d0 = assetB0 ? assetB0->Duration() : 0.0f;
            const float tA = (d0 > 0.0f) ? fmod(baseT, d0) : 0.0f;
            if (assetB0) {
                noteSampledAssetScaleMode(assetB0.get());
                EvalInputs in{ assetB0.get(), tA, stNowForEval->Loop };
                EvalTargets tgt{};
                tgt.poseTrs = &poseA;
                EvalContext ctx{ &s_sampleBindings, skeleton.Avatar.get(), &skeleton };
                SampleAsset(in, ctx, tgt, nullptr, nullptr);
            }
            fillUntouchedFromBindTrs(poseA);
            if (!assetB1) {
                preparePoseTrsBuffer(workingPoseTrs);
                workingPoseTrs.translation = poseA.translation;
                workingPoseTrs.rotation = poseA.rotation;
                workingPoseTrs.scale = poseA.scale;
                std::fill(workingPoseTrs.touched.begin(), workingPoseTrs.touched.end(), uint8_t{1u});
                workingPoseTrsValid = true;
            } else {
                thread_local PoseTRSBuffer poseB;
                preparePoseTrsBuffer(poseB);
                const float d1 = assetB1 ? assetB1->Duration() : 0.0f;
                const float tB = (d1 > 0.0f) ? fmod(baseT, d1) : 0.0f;
                noteSampledAssetScaleMode(assetB1.get());
                EvalInputs in{ assetB1.get(), tB, stNowForEval->Loop };
                EvalTargets tgt{};
                tgt.poseTrs = &poseB;
                EvalContext ctx{ &s_sampleBindings, skeleton.Avatar.get(), &skeleton };
                SampleAsset(in, ctx, tgt, nullptr, nullptr);
                fillUntouchedFromBindTrs(poseB);

                preparePoseTrsBuffer(workingPoseTrs);
                for (size_t i = 0; i < baseBoneCount; ++i) {
                    workingPoseTrs.translation[i] = glm::mix(poseA.translation[i], poseB.translation[i], blendT);
                    workingPoseTrs.rotation[i] = glm::normalize(glm::slerp(poseA.rotation[i], poseB.rotation[i], blendT));
                    workingPoseTrs.scale[i] = glm::mix(poseA.scale[i], poseB.scale[i], blendT);
                    workingPoseTrs.touched[i] = uint8_t{1u};
                }
                workingPoseTrsValid = true;
            }
        } else if (stNowForEval && stNowForEval->Kind == cm::animation::AnimatorStateKind::Blend2D && useBlend2D && !blendAssets2D.empty()) {
            // Evaluate multiple weighted blend points (IDW on authored 2D points).
            const float baseT = player.AnimatorInstance.Playback().StateTime;
            bool initialized = false;
            float accumulatedWeight = 0.0f;
            preparePoseTrsBuffer(workingPoseTrs);

            for (size_t sampleIdx = 0; sampleIdx < blendAssets2D.size() && sampleIdx < blendWeights2D.size(); ++sampleIdx) {
                auto& sampleAsset = blendAssets2D[sampleIdx];
                const float weight = blendWeights2D[sampleIdx];
                if (!sampleAsset || weight <= 1e-6f) continue;
                noteSampledAssetScaleMode(sampleAsset.get());

                const float duration = sampleAsset->Duration();
                const float sampleTime = (duration > 0.0f) ? fmod(baseT, duration) : 0.0f;
                thread_local PoseTRSBuffer blendPose;
                preparePoseTrsBuffer(blendPose);
                EvalInputs in{ sampleAsset.get(), sampleTime, stNowForEval->Loop };
                EvalTargets tgt{};
                tgt.poseTrs = &blendPose;
                EvalContext ctx{ &s_sampleBindings, skeleton.Avatar.get(), &skeleton };
                SampleAsset(in, ctx, tgt, nullptr, nullptr);

                fillUntouchedFromBindTrs(blendPose);

                if (!initialized) {
                    workingPoseTrs.translation = blendPose.translation;
                    workingPoseTrs.rotation = blendPose.rotation;
                    workingPoseTrs.scale = blendPose.scale;
                    std::fill(workingPoseTrs.touched.begin(), workingPoseTrs.touched.end(), uint8_t{1u});
                    accumulatedWeight = weight;
                    initialized = true;
                    continue;
                }

                const float t = weight / std::max(1e-6f, accumulatedWeight + weight);
                for (size_t i = 0; i < baseBoneCount; ++i) {
                    workingPoseTrs.translation[i] = glm::mix(workingPoseTrs.translation[i], blendPose.translation[i], t);
                    workingPoseTrs.rotation[i] = glm::normalize(glm::slerp(workingPoseTrs.rotation[i], blendPose.rotation[i], t));
                    workingPoseTrs.scale[i] = glm::mix(workingPoseTrs.scale[i], blendPose.scale[i], t);
                }
                accumulatedWeight += weight;
            }

            if (!initialized) {
                fillUntouchedFromBindTrs(workingPoseTrs);
            }
            workingPoseTrsValid = initialized || !blendAssets2D.empty();
        } else if (state.Asset) {
            // Unified evaluation into a temporary pose buffer sized to skeleton
            noteSampledAssetScaleMode(state.Asset);
            thread_local PoseTRSBuffer pose;
            preparePoseTrsBuffer(pose);
            const int currentStateId = player.AnimatorInstance.Playback().CurrentStateId;
            const bool stateChangedForEvents = (currentStateId != player._PrevEventStateId);
            const float eventPrevTime = stateChangedForEvents ? mutableState.Time : prevTime;
            const int eventLoopCount = stateChangedForEvents ? 0 : loopCount;
            EvalInputs in{ state.Asset, mutableState.Time, mutableState.Loop, eventPrevTime, !stateChangedForEvents, eventLoopCount };
            EvalTargets tgt{};
            tgt.poseTrs = &pose;
            EvalContext ctx{ &s_sampleBindings, skeleton.Avatar.get(), &skeleton };
            // Collect script events and property writes (if any)
            thread_local std::vector<cm::animation::ScriptEvent> s_firedEvents;
            auto& firedEvents = s_firedEvents;
            firedEvents.clear();
            SampleAsset(in, ctx, tgt, &firedEvents, nullptr);
            fillUntouchedFromBindTrs(pose);
            preparePoseTrsBuffer(workingPoseTrs);
            workingPoseTrs.translation = pose.translation;
            workingPoseTrs.rotation = pose.rotation;
            workingPoseTrs.scale = pose.scale;
            std::fill(workingPoseTrs.touched.begin(), workingPoseTrs.touched.end(), uint8_t{1u});
            workingPoseTrsValid = true;

            // Update event tracking even if no events fired (prevents missing events after state changes)
            const float currentTime = mutableState.Time;
            const bool animLooped = (eventLoopCount > 0) || (currentTime < player._PrevEventStateTime - 0.001f);
            if (stateChangedForEvents || animLooped) {
                player._FiredEventIds.clear();
            }
            player._PrevEventStateId = currentStateId;
            player._PrevEventStateTime = currentTime;

            // Dispatch script events to managed scripts attached to the skeleton root entity
            if (!firedEvents.empty()) {
                auto* rootData = scene.GetEntityData(rootId);
                if (rootData) {
                    for (const auto& ev : firedEvents) {
                        // Skip events that have already been fired this animation cycle
                        if (player._FiredEventIds.count(ev.id) > 0) continue;
                        player._FiredEventIds.insert(ev.id);

                        const std::string& targetClass  = ev.className;
                        const std::string& targetMethod = ev.method;

                        // Serialize payload to JSON string for interop
                        std::string payloadJson = ev.payload.dump();

                        // NEW: Dispatch to registered event listeners by event name (className)
                        // This allows any script on any entity to listen for animation events
#ifndef CLAYMORE_RUNTIME
                        DispatchAnimationEvent(
                            static_cast<int>(rootId),
                            targetClass.c_str(),
                            targetClass.c_str(),
                            targetMethod.c_str(),
                            payloadJson.c_str()
                        );
#endif

                        // LEGACY: Also dispatch to scripts attached to the skeleton root that match the class name
                        for (auto& script : rootData->Scripts) {
                            if (!script.Instance) continue;
                            if (script.ClassName != targetClass) continue;
                            if (script.Instance->GetBackend() == ScriptBackend::Managed) {
                                auto managed = std::dynamic_pointer_cast<ManagedScriptComponent>(script.Instance);
                                if (managed) {
                                    cm::script::Invoke(managed->GetHandle(), targetMethod.c_str());
                                }
                            }
                        }
                    }
                }
            }
        }
        // Legacy clip path removed - all animations now go through unified AnimationAsset evaluation
         
        // Crossfade blend if active: sample next state into TRS scratch and blend once.
        if (player.AnimatorInstance.IsCrossfading() && player.Controller) {
            int nextId = player.AnimatorInstance.Playback().NextStateId;
            const auto* nextSt = player.Controller->FindStateInLayer(0, nextId);
            if (nextSt) {
                const AnimationAsset* nextAsset = nullptr;
                std::shared_ptr<AnimationAsset> nextAssetPtr;
                // Load as AnimationAsset for proper humanoid retargeting (prefer AssetPath, fallback to ClipPath)
                const ResolvedAnimationBinding nextBinding = ResolveAnimationBinding(player, *nextSt);
                const std::string& nextAssetPath = nextBinding.EffectivePath();
                if (!nextAssetPath.empty()) { 
                    auto it = player.CachedAssets.find(nextSt->Id);
                    if (it != player.CachedAssets.end()) nextAssetPtr = it->second; else {
                        nextAssetPtr = std::make_shared<AnimationAsset>(LoadAnimationAsset(nextAssetPath));
                        player.CachedAssets[nextSt->Id] = nextAssetPtr;
                    }
                    nextAsset = nextAssetPtr.get();
                }
                thread_local PoseTRSBuffer nextPose;
                thread_local PoseTRSBuffer nextPoseA;
                thread_local PoseTRSBuffer nextPoseB;
                thread_local PoseTRSBuffer nextSamplePose;
                preparePoseTrsBuffer(nextPose);
                // Drive next state's time using its own speed
                float nextTime = player.AnimatorInstance.Playback().NextStateTime * (nextSt ? nextSt->Speed : 1.0f);
                
                // Handle Blend1D states during crossfade (next state might be Blend1D)
                if (nextSt->Kind == cm::animation::AnimatorStateKind::Blend1D && !nextSt->Blend1DEntries.empty()) {
                    // Determine blend parameter value for the next state
                    const float x = ReadBlend1DParameter(player.AnimatorInstance.Blackboard(), *nextSt);
                    // Find two surrounding entries
                    const auto& e = nextSt->Blend1DEntries;
                    int i1 = 0, i2 = (int)e.size()-1;
                    for (int i=0;i<(int)e.size();++i) { if (e[i].Key <= x) i1 = i; if (e[i].Key >= x) { i2 = i; break; } }
                    i1 = glm::clamp(i1, 0, (int)e.size()-1); i2 = glm::clamp(i2, 0, (int)e.size()-1);
                    const auto& a = e[i1]; const auto& b = e[i2];
                    float denom = std::max(1e-6f, (b.Key - a.Key));
                    float nextBlendT = glm::clamp((x - a.Key) / denom, 0.0f, 1.0f);
                    // Load assets for blend entries (prefer AssetPath, fallback to ClipPath)
                    std::shared_ptr<AnimationAsset> nextAssetB0, nextAssetB1;
                    const ResolvedAnimationBinding bindingA = ResolveAnimationBinding(player, a);
                    const ResolvedAnimationBinding bindingB = ResolveAnimationBinding(player, b);
                    const std::string& pathA = bindingA.EffectivePath();
                    const std::string& pathB = bindingB.EffectivePath();
                    if (!pathA.empty()) { auto ita = player.CachedAssets.find(nextSt->Id * 1000 + i1); if (ita != player.CachedAssets.end()) nextAssetB0 = ita->second; else { nextAssetB0 = std::make_shared<AnimationAsset>(LoadAnimationAsset(pathA)); player.CachedAssets[nextSt->Id * 1000 + i1] = nextAssetB0; } }
                    if (!pathB.empty()) { auto ita = player.CachedAssets.find(nextSt->Id * 1000 + i2); if (ita != player.CachedAssets.end()) nextAssetB1 = ita->second; else { nextAssetB1 = std::make_shared<AnimationAsset>(LoadAnimationAsset(pathB)); player.CachedAssets[nextSt->Id * 1000 + i2] = nextAssetB1; } }
                    float nextBlendDuration = 0.0f;
                    collapseBlend1DContributors(nextAssetB0, nextAssetB1, i1, i2, nextBlendT, nextBlendDuration);
                    float d0 = nextAssetB0 ? nextAssetB0->Duration() : 0.0f;
                    float d1 = nextAssetB1 ? nextAssetB1->Duration() : 0.0f;
                    float tA = (d0 > 0.0f) ? fmod(nextTime, d0) : 0.0f;
                    float tB = (d1 > 0.0f) ? fmod(nextTime, d1) : 0.0f;
                    if (nextAssetB0) {
                        noteSampledAssetScaleMode(nextAssetB0.get());
                        preparePoseTrsBuffer(nextPoseA);
                        EvalInputs in{ nextAssetB0.get(), tA, nextSt->Loop };
                        EvalTargets tgt{};
                        tgt.poseTrs = &nextPoseA;
                        EvalContext ctx{ &s_sampleBindings, skeleton.Avatar.get(), &skeleton };
                        SampleAsset(in, ctx, tgt, nullptr, nullptr);
                        fillUntouchedFromBindTrs(nextPoseA);
                    }
                    if (nextAssetB1) {
                        noteSampledAssetScaleMode(nextAssetB1.get());
                        preparePoseTrsBuffer(nextPoseB);
                        EvalInputs in{ nextAssetB1.get(), tB, nextSt->Loop };
                        EvalTargets tgt{};
                        tgt.poseTrs = &nextPoseB;
                        EvalContext ctx{ &s_sampleBindings, skeleton.Avatar.get(), &skeleton };
                        SampleAsset(in, ctx, tgt, nullptr, nullptr);
                        fillUntouchedFromBindTrs(nextPoseB);
                    }

                    const bool hasPoseA = (nextAssetB0 != nullptr);
                    const bool hasPoseB = (nextAssetB1 != nullptr);
                    if (hasPoseA && !hasPoseB) {
                        nextPose.translation = nextPoseA.translation;
                        nextPose.rotation = nextPoseA.rotation;
                        nextPose.scale = nextPoseA.scale;
                        std::fill(nextPose.touched.begin(), nextPose.touched.end(), uint8_t{1u});
                    } else if (!hasPoseA && hasPoseB) {
                        nextPose.translation = nextPoseB.translation;
                        nextPose.rotation = nextPoseB.rotation;
                        nextPose.scale = nextPoseB.scale;
                        std::fill(nextPose.touched.begin(), nextPose.touched.end(), uint8_t{1u});
                    } else if (hasPoseA && hasPoseB) {
                        for (size_t i = 0; i < baseBoneCount; ++i) {
                            nextPose.translation[i] = glm::mix(nextPoseA.translation[i], nextPoseB.translation[i], nextBlendT);
                            nextPose.rotation[i] = glm::normalize(glm::slerp(nextPoseA.rotation[i], nextPoseB.rotation[i], nextBlendT));
                            nextPose.scale[i] = glm::mix(nextPoseA.scale[i], nextPoseB.scale[i], nextBlendT);
                            nextPose.touched[i] = uint8_t{1u};
                        }
                    }
                } else if (nextSt->Kind == cm::animation::AnimatorStateKind::Blend2D && !nextSt->Blend2DEntries.empty()) {
                    std::array<int, 4> indices{};
                    std::array<float, 4> weights{};
                    int sampleCount = 0;
                    if (resolveBlend2DWeights(nextSt, player.AnimatorInstance.Blackboard(), indices, weights, sampleCount)) {
                        bool initialized = false;
                        float accumulatedWeight = 0.0f;
                        for (int wi = 0; wi < sampleCount; ++wi) {
                            const auto& entry = nextSt->Blend2DEntries[indices[wi]];
                            const ResolvedAnimationBinding binding = ResolveAnimationBinding(player, entry);
                            const std::string& path = binding.EffectivePath();
                            auto sampleAsset = loadBlendAsset(0, nextSt->Id, indices[wi], path);
                            if (!sampleAsset || weights[wi] <= 1e-6f) continue;
                            noteSampledAssetScaleMode(sampleAsset.get());

                            preparePoseTrsBuffer(nextSamplePose);
                            const float d = sampleAsset->Duration();
                            const float sampleTime = (d > 0.0f) ? fmod(nextTime, d) : 0.0f;
                            EvalInputs in{ sampleAsset.get(), sampleTime, nextSt->Loop };
                            EvalTargets tgt{};
                            tgt.poseTrs = &nextSamplePose;
                            EvalContext ctx{ &s_sampleBindings, skeleton.Avatar.get(), &skeleton };
                            SampleAsset(in, ctx, tgt, nullptr, nullptr);
                            fillUntouchedFromBindTrs(nextSamplePose);

                            if (!initialized) {
                                nextPose.translation = nextSamplePose.translation;
                                nextPose.rotation = nextSamplePose.rotation;
                                nextPose.scale = nextSamplePose.scale;
                                std::fill(nextPose.touched.begin(), nextPose.touched.end(), uint8_t{1u});
                                accumulatedWeight = weights[wi];
                                initialized = true;
                                continue;
                            }

                            const float t = weights[wi] / std::max(1e-6f, accumulatedWeight + weights[wi]);
                            for (size_t bi = 0; bi < baseBoneCount; ++bi) {
                                nextPose.translation[bi] = glm::mix(nextPose.translation[bi], nextSamplePose.translation[bi], t);
                                nextPose.rotation[bi] = glm::normalize(glm::slerp(nextPose.rotation[bi], nextSamplePose.rotation[bi], t));
                                nextPose.scale[bi] = glm::mix(nextPose.scale[bi], nextSamplePose.scale[bi], t);
                            }
                            accumulatedWeight += weights[wi];
                        }
                    }
                } else if (nextAsset) {
                    noteSampledAssetScaleMode(nextAsset);
                    preparePoseTrsBuffer(nextPose);
                    EvalInputs in{ nextAsset, nextTime, nextSt->Loop };
                    EvalTargets tgt{};
                    tgt.poseTrs = &nextPose;
                    EvalContext ctx{ &s_sampleBindings, skeleton.Avatar.get(), &skeleton };
                    SampleAsset(in, ctx, tgt, nullptr, nullptr);
                }
                
                // Fill untouched bones with bind pose locals (same as current state evaluation)
                // This prevents blending against identity matrices which causes distorted poses
                fillUntouchedFromBindTrs(nextPose);

                float a = player.AnimatorInstance.CrossfadeAlpha();
                if (workingPoseTrsValid && nextPose.translation.size() == baseBoneCount) {
                    for (size_t i = 0; i < baseBoneCount; ++i) {
                        workingPoseTrs.translation[i] = glm::mix(workingPoseTrs.translation[i], nextPose.translation[i], a);
                        workingPoseTrs.rotation[i] = glm::normalize(glm::slerp(workingPoseTrs.rotation[i], nextPose.rotation[i], a));
                        workingPoseTrs.scale[i] = glm::mix(workingPoseTrs.scale[i], nextPose.scale[i], a);
                    }
                }
                if (a >= 1.0f) {
                    // Crossfade complete: ensure Animator's current state is updated as well
                    // IMPORTANT: Do NOT reset time - the animation has already been playing during crossfade!
                    // The NextStateTime has been accumulating, so we need to preserve that progress.
                    // Resetting time would cause the animation to play again from the beginning.
                    float preservedTime = player.AnimatorInstance.Playback().NextStateTime;
                    
                    // Calculate the next state's duration for proper normalized time
                    float nextStateDuration = 0.0f;
                    if (nextSt->Kind == cm::animation::AnimatorStateKind::Blend1D && !nextSt->Blend1DEntries.empty()) {
                        // Blend1D: compute blended duration
                        const float x = ReadBlend1DParameter(player.AnimatorInstance.Blackboard(), *nextSt);
                        const auto& e = nextSt->Blend1DEntries;
                        int i1 = 0, i2 = (int)e.size()-1;
                        for (int i=0;i<(int)e.size();++i) { if (e[i].Key <= x) i1 = i; if (e[i].Key >= x) { i2 = i; break; } }
                        i1 = glm::clamp(i1, 0, (int)e.size()-1); i2 = glm::clamp(i2, 0, (int)e.size()-1);
                        const auto& ea = e[i1]; const auto& eb = e[i2];
                        float denom = std::max(1e-6f, (eb.Key - ea.Key));
                        float t = glm::clamp((x - ea.Key) / denom, 0.0f, 1.0f);
                        // Get durations
                        const ResolvedAnimationBinding bindingA = ResolveAnimationBinding(player, ea);
                        const ResolvedAnimationBinding bindingB = ResolveAnimationBinding(player, eb);
                        const int cacheKeyA = nextSt->Id * 1000 + i1;
                        const int cacheKeyB = nextSt->Id * 1000 + i2;
                        const float d0 = LookupOrLoadAnimationDuration(player, cacheKeyA, bindingA);
                        if (i1 == i2 || t <= kBlend1DEndpointEpsilon) {
                            nextStateDuration = d0;
                        } else {
                            const float d1 = LookupOrLoadAnimationDuration(player, cacheKeyB, bindingB);
                            nextStateDuration = (t >= 1.0f - kBlend1DEndpointEpsilon) ? d1 : glm::mix(d0, d1, t);
                        }
                    } else if (nextSt->Kind == cm::animation::AnimatorStateKind::Blend2D && !nextSt->Blend2DEntries.empty()) {
                        std::array<int, 4> indices{};
                        std::array<float, 4> weights{};
                        int sampleCount = 0;
                        if (resolveBlend2DWeights(nextSt, player.AnimatorInstance.Blackboard(), indices, weights, sampleCount)) {
                            float loadedWeight = 0.0f;
                            for (int i = 0; i < sampleCount; ++i) {
                                const auto& entry = nextSt->Blend2DEntries[indices[i]];
                                const std::string path = ResolveAnimationBinding(player, entry).EffectivePath();
                                auto sampleAsset = loadBlendAsset(0, nextSt->Id, indices[i], path);
                                if (!sampleAsset) continue;
                                nextStateDuration += weights[i] * sampleAsset->Duration();
                                loadedWeight += weights[i];
                            }
                            if (loadedWeight > 1e-6f) {
                                nextStateDuration /= loadedWeight;
                            }
                        }
                    } else if (nextAsset) {
                        nextStateDuration = nextAsset->Duration();
                    }
                    
                    player.AnimatorInstance.SetCurrentState(nextId, /*resetTime*/false);
                    player.AnimatorInstance.SetStateTime(preservedTime, nextStateDuration);
                    player.CurrentStateId = nextId;
                }
            }
        }

        if (workingPoseTrsValid) {
            materializePoseTrs(workingPoseTrs, localTransforms);
        }

        // Humanoid constraint: keep translation/scale only on root/hips; others use bind T/S, animated rotation.
        // Avatar tracks now apply this while sampling, so this pass is only needed for generic bone-track assets.
        if (skeleton.Avatar && sampledPoseNeedsRuntimeScalePreservation) {
            int hipsIdx = skeleton.Avatar->GetMappedBoneIndex(cm::animation::HumanoidBone::Hips);
            int rootIdx = skeleton.Avatar->GetMappedBoneIndex(cm::animation::HumanoidBone::Root);
            for (int i = 0; i < (int)localTransforms.size(); ++i) {
                if (i == hipsIdx || i == rootIdx) continue;
                glm::vec3 Ta, Sa; glm::quat Ra;
                decomposeTRS(localTransforms[i], Ta, Ra, Sa);
                glm::vec3 Tb, Sb; glm::quat Rb; // Rb unused
                getBindLocalTRS(i, Tb, Rb, Sb);
                localTransforms[i] = glm::translate(Tb) * glm::mat4_cast(glm::normalize(Ra)) * glm::scale(Sb);
            }
        }
        if (crowdPoseShareEligible) {
            std::lock_guard<std::mutex> lock(crowdPoseShareCache.Mutex);
            auto [crowdPoseIt, inserted] =
                crowdPoseShareCache.Entries.emplace(crowdPoseShareKey, localTransforms);
            (void)crowdPoseIt;
            if (inserted) {
                crowdPoseShareLeaderCount.fetch_add(1, std::memory_order_relaxed);
            }
        }
        }

        // =========================================================================
        // Root Motion Handling - Per-Animation Settings
        // =========================================================================
        // Root motion mode is now read from the animation asset itself.
        // The player component just determines WHERE to route the extracted motion.
        // =========================================================================
        
        // Reset output delta at start of frame
        player._RootMotionOutput.Reset();
        
        if (skeleton.Avatar) {
            // Compose model matrix from locals up the parent chain
            auto composeModel = [&](int boneIndex) -> glm::mat4 {
                glm::mat4 model(1.0f);
                int bi = boneIndex;
                while (bi >= 0) {
                    model = localTransforms[bi] * model;
                    bi = (bi < (int)skeleton.BoneParents.size()) ? skeleton.BoneParents[bi] : -1;
                }
                return model;
            };

            // Replace a bone's local translation with its bind local translation, preserve animated R/S
            auto zeroLocalTranslationToBind = [&](int boneIndex) {
                if (boneIndex < 0 || boneIndex >= (int)localTransforms.size()) return;
                glm::vec3 Ta, Sa; glm::quat Ra;
                decomposeTRS(localTransforms[boneIndex], Ta, Ra, Sa);
                glm::vec3 Tb, Sb; glm::quat Rb;
                getBindLocalTRS(boneIndex, Tb, Rb, Sb);
                localTransforms[boneIndex] = glm::translate(Tb) * glm::mat4_cast(glm::normalize(Ra)) * glm::scale(Sb);
            };

            // Get root motion settings from the CURRENT animation asset
            // For Blend1D states, blend the settings from both contributing animations
            RootMotionSettings rmSettings;
            const AnimationAsset* currentAsset = state.Asset;
            if (!currentAsset) {
                const AnimationAsset* soleContributor = nullptr;
                bool multipleContributors = false;
                auto considerAsset = [&](const AnimationAsset* candidate) {
                    if (!candidate || multipleContributors) return;
                    if (!soleContributor) {
                        soleContributor = candidate;
                    } else if (soleContributor != candidate) {
                        multipleContributors = true;
                    }
                };

                if (useBlend1D) {
                    considerAsset(assetB0.get());
                    considerAsset(assetB1.get());
                } else if (useBlend2D) {
                    for (const auto& assetPtr : blendAssets2D) {
                        considerAsset(assetPtr.get());
                    }
                }

                if (soleContributor && !multipleContributors) {
                    currentAsset = soleContributor;
                }
            }
            
            // Handle blending: when crossfading, we need to consider both animations' settings
            // If blending INTO an animation with root motion, that motion should come through
            // If blending OUT of root motion into in-place, the delta should fade to zero
            float crossfadeAlpha = 0.0f;
            RootMotionSettings nextRmSettings;
            bool isCrossfading = player.AnimatorInstance.IsCrossfading();
            
            // For blend-tree states, aggregate settings from all contributing clips.
            if ((useBlend1D && (assetB0 || assetB1)) || (useBlend2D && !blendAssets2D.empty())) {
                std::vector<std::pair<const AnimationAsset*, float>> contributors;
                if (useBlend1D) {
                    if (assetB0) contributors.push_back({ assetB0.get(), 1.0f - blendT });
                    if (assetB1) contributors.push_back({ assetB1.get(), blendT });
                } else {
                    for (size_t i = 0; i < blendAssets2D.size() && i < blendWeights2D.size(); ++i) {
                        if (!blendAssets2D[i]) continue;
                        contributors.push_back({ blendAssets2D[i].get(), blendWeights2D[i] });
                    }
                }

                bool anyApplyToEntity = false;
                bool anyInPlace = false;
                rmSettings.IncludeXZ = false;
                rmSettings.IncludeY = false;
                rmSettings.IncludeRotation = false;
                rmSettings.OverrideGravity = false;

                for (const auto& contributor : contributors) {
                    const auto* assetPtr = contributor.first;
                    if (!assetPtr) continue;
                    const RootMotionSettings& sample = assetPtr->meta.rootMotion;
                    anyApplyToEntity = anyApplyToEntity || (sample.Mode == RootMotionMode::ApplyToEntity);
                    anyInPlace = anyInPlace || (sample.Mode == RootMotionMode::InPlace);
                    rmSettings.IncludeXZ = rmSettings.IncludeXZ || sample.IncludeXZ;
                    rmSettings.IncludeY = rmSettings.IncludeY || sample.IncludeY;
                    rmSettings.IncludeRotation = rmSettings.IncludeRotation || sample.IncludeRotation;
                    rmSettings.OverrideGravity = rmSettings.OverrideGravity || sample.OverrideGravity;
                }

                if (anyApplyToEntity) {
                    rmSettings.Mode = RootMotionMode::ApplyToEntity;
                } else if (anyInPlace) {
                    rmSettings.Mode = RootMotionMode::InPlace;
                } else {
                    rmSettings.Mode = RootMotionMode::None;
                }
            } else if (currentAsset) {
                rmSettings = currentAsset->meta.rootMotion;
            }
            
            // During crossfade, get the next state's root motion settings for blending
            if (isCrossfading && player.Controller) {
                int nextId = player.AnimatorInstance.Playback().NextStateId;
                const auto* nextSt = player.Controller->FindStateInLayer(0, nextId);
                if (nextSt) {
                    auto itNext = player.CachedAssets.find(nextId);
                    if (itNext != player.CachedAssets.end() && itNext->second) {
                        nextRmSettings = itNext->second->meta.rootMotion;
                    }
                }
                crossfadeAlpha = player.AnimatorInstance.CrossfadeAlpha();
            }
            
            // Determine effective root motion mode (blend during crossfade)
            // Priority: if EITHER animation wants ApplyToEntity, we extract motion
            // The delta itself is blended based on which animations contribute
            RootMotionMode effectiveMode = rmSettings.Mode;
            if (isCrossfading) {
                // If next state wants root motion applied, prioritize that
                if (nextRmSettings.Mode == RootMotionMode::ApplyToEntity) {
                    effectiveMode = RootMotionMode::ApplyToEntity;
                }
            }
            player._CurrentRootMotionMode = effectiveMode;
            const bool transitionBlendActive = isCrossfading || player._ControllerSwitchBlendActive;
            const bool bypassYCurrentState = (stNowForEval && stNowForEval->BypassYMotion);
            const cm::animation::AnimatorState* nextStateForY = nullptr;
            bool bypassYNextState = false;
            if (isCrossfading && player.Controller) {
                const int nextId = player.AnimatorInstance.Playback().NextStateId;
                nextStateForY = player.Controller->FindStateInLayer(0, nextId);
                if (nextStateForY) {
                    bypassYNextState = nextStateForY->BypassYMotion;
                }
            }
            const bool bypassYDuringTransition = bypassYCurrentState || bypassYNextState;

            int hipsIdx = skeleton.Avatar->GetMappedBoneIndex(cm::animation::HumanoidBone::Hips);
            int rootIdx = skeleton.Avatar->GetMappedBoneIndex(cm::animation::HumanoidBone::Root);


            // Resolve the source bone for root motion:
            // 1) explicit RootBoneName override in clip settings
            // 2) humanoid Root mapping
            // 3) humanoid Hips mapping (default for rigs without explicit Root)
            auto toLowerAscii = [](std::string s) {
                for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return s;
            };
            auto findBoneIndexByName = [&](const std::string& name) -> int {
                if (name.empty() || skeleton.BoneNames.empty()) return -1;
                for (size_t i = 0; i < skeleton.BoneNames.size(); ++i) {
                    if (skeleton.BoneNames[i] == name) return static_cast<int>(i);
                }
                const std::string lowerName = toLowerAscii(name);
                for (size_t i = 0; i < skeleton.BoneNames.size(); ++i) {
                    if (toLowerAscii(skeleton.BoneNames[i]) == lowerName) return static_cast<int>(i);
                }
                return -1;
            };

            // Resolve a hips reference height in the same target-local pose space as the evaluated clip.
            auto getAssetHipsReferenceY = [&](const AnimationAsset* asset,
                                             bool& outFound,
                                             bool useAverageReference = false,
                                             float* outRangeY = nullptr) -> float {
                outFound = false;
                if (outRangeY) *outRangeY = 0.0f;
                if (!asset || !skeleton.Avatar) return 0.0f;

                const std::uintptr_t assetKey = reinterpret_cast<std::uintptr_t>(asset);
                const std::uintptr_t avatarKey = reinterpret_cast<std::uintptr_t>(skeleton.Avatar.get());
                std::uintptr_t cacheKey = assetKey;
                cacheKey ^= avatarKey
                    + static_cast<std::uintptr_t>(0x9e3779b97f4a7c15ULL)
                    + (cacheKey << 6)
                    + (cacheKey >> 2);

                auto& cacheByMode = player._HipsReferenceCache[cacheKey];
                auto& cached = cacheByMode[useAverageReference ? 1 : 0];
                if (cached.Valid) {
                    outFound = cached.Found;
                    if (outRangeY) *outRangeY = cached.RangeY;
                    return cached.Y;
                }

                auto storeResult = [&](bool found, float y, float rangeY) -> float {
                    cached.Valid = true;
                    cached.Found = found;
                    cached.Y = y;
                    cached.RangeY = rangeY;
                    outFound = found;
                    if (outRangeY) *outRangeY = rangeY;
                    return y;
                };

                float refY = 0.0f;

                glm::vec3 targetBindT(0.0f), targetBindS(1.0f);
                glm::quat targetBindR(1.0f, 0.0f, 0.0f, 0.0f);
                const bool hasTargetHips = skeleton.Avatar->GetAnimationBindTRS(HumanoidBone::Hips, targetBindT, targetBindR, targetBindS);
                const uint16_t hipsBoneId = static_cast<uint16_t>(HumanoidBone::Hips);
                const float targetHipsHeight =
                    (hipsBoneId < skeleton.Avatar->BindModel.size())
                    ? std::abs(skeleton.Avatar->BindModel[hipsBoneId][3].y)
                    : 0.0f;

                for (const auto& t : asset->tracks) {
                    if (!t) continue;
                    if (t->type == TrackType::Avatar) {
                        auto* at = static_cast<const AssetAvatarTrack*>(t.get());
                        if (at->humanBoneId == static_cast<int>(HumanoidBone::Hips) && !at->t.keys.empty()) {
                            const float sourceRefHeight = std::abs(asset->meta.referenceHipsHeight);
                            float positionScale = 1.0f;
                            if (sourceRefHeight > 0.1f && targetHipsHeight > 0.01f) {
                                positionScale = targetHipsHeight / sourceRefHeight;
                            }

                            auto projectBindRelativeY = [&](float sampleY) {
                                return targetBindT.y + sampleY * positionScale;
                            };
                            auto projectLegacyY = [&](float sampleY, float srcBindY) {
                                return targetBindT.y + (sampleY - srcBindY) * positionScale;
                            };
                            auto reduceReference = [&](auto&& project, float& outComputedRangeY) -> float {
                                float minY = std::numeric_limits<float>::max();
                                float maxY = std::numeric_limits<float>::lowest();
                                float sumY = 0.0f;
                                for (const auto& key : at->t.keys) {
                                    const float y = project(key.v.y);
                                    minY = std::min(minY, y);
                                    maxY = std::max(maxY, y);
                                    sumY += y;
                                }
                                outComputedRangeY = (minY <= maxY) ? (maxY - minY) : 0.0f;
                                if (useAverageReference && !at->t.keys.empty()) {
                                    return sumY / static_cast<float>(at->t.keys.size());
                                }
                                return project(at->t.keys.front().v.y);
                            };

                            if (asset->meta.humanoidTrackMode == HumanoidTrackMode::BindRelative && hasTargetHips) {
                                float rangeY = 0.0f;
                                refY = reduceReference(projectBindRelativeY, rangeY);
                                return storeResult(true, refY, rangeY);
                            }

                            if (asset->meta.humanoidTrackMode == HumanoidTrackMode::LegacyAbsolute &&
                                asset->LegacySourceAvatar &&
                                asset->LegacySourceAvatar->IsBonePresent(HumanoidBone::Hips) &&
                                hasTargetHips) {
                                glm::vec3 srcBindT(0.0f), srcBindS(1.0f);
                                glm::quat srcBindR(1.0f, 0.0f, 0.0f, 0.0f);
                                asset->LegacySourceAvatar->GetAnimationBindTRS(HumanoidBone::Hips, srcBindT, srcBindR, srcBindS);
                                float rangeY = 0.0f;
                                refY = reduceReference([&](float sampleY) { return projectLegacyY(sampleY, srcBindT.y); }, rangeY);
                                return storeResult(true, refY, rangeY);
                            }

                            float minY = std::numeric_limits<float>::max();
                            float maxY = std::numeric_limits<float>::lowest();
                            float sumY = 0.0f;
                            for (const auto& key : at->t.keys) {
                                minY = std::min(minY, key.v.y);
                                maxY = std::max(maxY, key.v.y);
                                sumY += key.v.y;
                            }
                            const float rangeY = (minY <= maxY) ? (maxY - minY) : 0.0f;
                            refY = (useAverageReference && !at->t.keys.empty())
                                ? (sumY / static_cast<float>(at->t.keys.size()))
                                : at->t.keys.front().v.y;
                            return storeResult(true, refY, rangeY);
                        }
                    }
                }
                for (const auto& t : asset->tracks) {
                    if (!t || t->type != TrackType::Bone) continue;
                    const std::string lname = toLowerAscii(t->name);
                    if (lname.find("hips") == std::string::npos && lname.find("pelvis") == std::string::npos) continue;
                    auto* bt = static_cast<const AssetBoneTrack*>(t.get());
                    if (!bt->t.keys.empty()) {
                        refY = bt->t.keys.front().v.y;
                        return storeResult(true, refY, 0.0f);
                    }
                }
                return storeResult(false, 0.0f, 0.0f);
            };

            struct StateHipsReferenceSample {
                bool found = false;
                float y = 0.0f;
                float rangeY = 0.0f;
            };

            auto resolveStateHipsReferenceY = [&](const cm::animation::AnimatorState* animState) -> StateHipsReferenceSample {
                StateHipsReferenceSample result;
                if (!animState) return result;

                const bool useAverageReference = animState->Loop;

                if (animState->Kind == cm::animation::AnimatorStateKind::Blend1D &&
                    !animState->Blend1DEntries.empty()) {
                    const float x = ReadBlend1DParameter(player.AnimatorInstance.Blackboard(), *animState);
                    const auto& entries = animState->Blend1DEntries;
                    int i1 = 0;
                    int i2 = static_cast<int>(entries.size()) - 1;
                    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
                        if (entries[i].Key <= x) i1 = i;
                        if (entries[i].Key >= x) {
                            i2 = i;
                            break;
                        }
                    }
                    i1 = glm::clamp(i1, 0, static_cast<int>(entries.size()) - 1);
                    i2 = glm::clamp(i2, 0, static_cast<int>(entries.size()) - 1);

                    const auto& a = entries[i1];
                    const auto& b = entries[i2];
                    const float denom = std::max(1e-6f, (b.Key - a.Key));
                    const float stateBlendT = glm::clamp((x - a.Key) / denom, 0.0f, 1.0f);

                    const std::string pathA = ResolveAnimationBinding(player, a).EffectivePath();
                    const std::string pathB = ResolveAnimationBinding(player, b).EffectivePath();
                    auto assetA = loadBlendAsset(0, animState->Id, i1, pathA);
                    auto assetB = loadBlendAsset(0, animState->Id, i2, pathB);

                    bool foundA = false;
                    bool foundB = false;
                    float rangeA = 0.0f;
                    float rangeB = 0.0f;
                    const float refA = getAssetHipsReferenceY(assetA.get(), foundA, useAverageReference, &rangeA);
                    const float refB = getAssetHipsReferenceY(assetB.get(), foundB, useAverageReference, &rangeB);
                    if (foundA && foundB) {
                        result.found = true;
                        result.y = glm::mix(refA, refB, stateBlendT);
                        result.rangeY = glm::mix(rangeA, rangeB, stateBlendT);
                    } else if (foundA) {
                        result.found = true;
                        result.y = refA;
                        result.rangeY = rangeA;
                    } else if (foundB) {
                        result.found = true;
                        result.y = refB;
                        result.rangeY = rangeB;
                    }
                    return result;
                }

                if (animState->Kind == cm::animation::AnimatorStateKind::Blend2D &&
                    !animState->Blend2DEntries.empty()) {
                    std::array<int, 4> indices{};
                    std::array<float, 4> weights{};
                    int sampleCount = 0;
                    if (!resolveBlend2DWeights(animState, player.AnimatorInstance.Blackboard(), indices, weights, sampleCount)) {
                        return result;
                    }

                    float weightedRefY = 0.0f;
                    float weightedRangeY = 0.0f;
                    float weightedSum = 0.0f;
                    for (int i = 0; i < sampleCount; ++i) {
                        const auto& entry = animState->Blend2DEntries[indices[i]];
                        const std::string path = ResolveAnimationBinding(player, entry).EffectivePath();
                        auto asset = loadBlendAsset(0, animState->Id, indices[i], path);
                        if (!asset || weights[i] <= 1e-6f) continue;

                        bool found = false;
                        float rangeY = 0.0f;
                        const float refY = getAssetHipsReferenceY(asset.get(), found, useAverageReference, &rangeY);
                        if (!found) continue;

                        weightedRefY += refY * weights[i];
                        weightedRangeY += rangeY * weights[i];
                        weightedSum += weights[i];
                    }

                    if (weightedSum > 1e-6f) {
                        result.found = true;
                        result.y = weightedRefY / weightedSum;
                        result.rangeY = weightedRangeY / weightedSum;
                    }
                    return result;
                }

                std::string assetPath = ResolveAnimationBinding(player, *animState).EffectivePath();
                if (assetPath.empty()) return result;

                std::shared_ptr<AnimationAsset> stateAsset;
                auto it = player.CachedAssets.find(animState->Id);
                if (it != player.CachedAssets.end()) {
                    stateAsset = it->second;
                } else {
                    stateAsset = cm::animation::LoadAnimationAssetCached(assetPath, true);
                    if (stateAsset && stateAsset->Duration() > 0.0f) {
                        player.CachedAssets[animState->Id] = stateAsset;
                    }
                }

                if (!stateAsset) return result;

                result.y = getAssetHipsReferenceY(stateAsset.get(), result.found, useAverageReference, &result.rangeY);
                return result;
            };

            auto resolveCurrentHipsReferenceY = [&]() -> StateHipsReferenceSample {
                StateHipsReferenceSample result;
                const bool useAverageReference = stNowForEval && stNowForEval->Loop;

                if (useBlend1D) {
                    bool foundA = false;
                    bool foundB = false;
                    float rangeA = 0.0f;
                    float rangeB = 0.0f;
                    const float refA = getAssetHipsReferenceY(assetB0.get(), foundA, useAverageReference, &rangeA);
                    if (!assetB1 || blendT <= kBlend1DEndpointEpsilon) {
                        result.found = foundA;
                        result.y = refA;
                        result.rangeY = rangeA;
                        return result;
                    }

                    const float refB = getAssetHipsReferenceY(assetB1.get(), foundB, useAverageReference, &rangeB);
                    if (foundA && foundB) {
                        result.found = true;
                        result.y = glm::mix(refA, refB, blendT);
                        result.rangeY = glm::mix(rangeA, rangeB, blendT);
                    } else if (foundA) {
                        result.found = true;
                        result.y = refA;
                        result.rangeY = rangeA;
                    } else if (foundB) {
                        result.found = true;
                        result.y = refB;
                        result.rangeY = rangeB;
                    }
                    return result;
                }

                if (useBlend2D) {
                    float weightedRefY = 0.0f;
                    float weightedRangeY = 0.0f;
                    float weightedSum = 0.0f;
                    for (size_t i = 0; i < blendAssets2D.size() && i < blendWeights2D.size(); ++i) {
                        const auto& asset = blendAssets2D[i];
                        const float weight = blendWeights2D[i];
                        if (!asset || weight <= 1e-6f) continue;

                        bool found = false;
                        float rangeY = 0.0f;
                        const float refY = getAssetHipsReferenceY(asset.get(), found, useAverageReference, &rangeY);
                        if (!found) continue;

                        weightedRefY += refY * weight;
                        weightedRangeY += rangeY * weight;
                        weightedSum += weight;
                    }
                    if (weightedSum > 1e-6f) {
                        result.found = true;
                        result.y = weightedRefY / weightedSum;
                        result.rangeY = weightedRangeY / weightedSum;
                    }
                    return result;
                }

                if (currentAsset) {
                    result.y = getAssetHipsReferenceY(currentAsset, result.found, useAverageReference, &result.rangeY);
                    return result;
                }

                return resolveStateHipsReferenceY(stNowForEval);
            };

            int rootMotionBoneIdx = -1;
            if (!rmSettings.RootBoneName.empty()) {
                rootMotionBoneIdx = findBoneIndexByName(rmSettings.RootBoneName);
            }
            if (rootMotionBoneIdx < 0 && rootIdx >= 0) rootMotionBoneIdx = rootIdx;
            if (rootMotionBoneIdx < 0 && hipsIdx >= 0) rootMotionBoneIdx = hipsIdx;
            const bool usingHipsAsRootSource = (rootMotionBoneIdx >= 0 && rootMotionBoneIdx == hipsIdx && rootIdx < 0);

            // Feet-aware anchoring inputs.
            int leftFootIdx = skeleton.Avatar->GetMappedBoneIndex(cm::animation::HumanoidBone::LeftFoot);
            int rightFootIdx = skeleton.Avatar->GetMappedBoneIndex(cm::animation::HumanoidBone::RightFoot);
            if (leftFootIdx < 0) leftFootIdx = skeleton.Avatar->GetMappedBoneIndex(cm::animation::HumanoidBone::LeftToes);
            if (rightFootIdx < 0) rightFootIdx = skeleton.Avatar->GetMappedBoneIndex(cm::animation::HumanoidBone::RightToes);
            const bool hasFeetRefs = leftFootIdx >= 0 || rightFootIdx >= 0;

            auto hashInPlaceSignature = [](uint64_t& hash, uint64_t value) {
                hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
            };
            auto makeInPlaceYSourceSignature = [&]() -> uint64_t {
                uint64_t hash = 1469598103934665603ULL;
                hashInPlaceSignature(hash, static_cast<uint64_t>(player.CurrentStateId + 4099));
                hashInPlaceSignature(hash, static_cast<uint64_t>(effectiveMode) + 17ULL);
                if (stNowForEval) {
                    hashInPlaceSignature(hash, static_cast<uint64_t>(stNowForEval->Id + 8193));
                    hashInPlaceSignature(hash, static_cast<uint64_t>(stNowForEval->Kind) + 257ULL);
                    hashInPlaceSignature(hash, stNowForEval->Loop ? 1ULL : 0ULL);
                }
                if (useBlend1D) {
                    hashInPlaceSignature(hash, 0xB1ULL);
                    hashInPlaceSignature(hash, static_cast<uint64_t>(blend1DIndexA + 33));
                    hashInPlaceSignature(hash, static_cast<uint64_t>(blend1DIndexB + 33));
                } else if (useBlend2D) {
                    hashInPlaceSignature(hash, 0xB2ULL);
                    hashInPlaceSignature(hash, static_cast<uint64_t>(activeBlend2DCount + 65));
                    for (int i = 0; i < activeBlend2DCount && i < static_cast<int>(activeBlend2DIndices.size()); ++i) {
                        hashInPlaceSignature(hash, static_cast<uint64_t>(activeBlend2DIndices[i] + 97));
                    }
                } else {
                    hashInPlaceSignature(hash, 0xB0ULL);
                    hashInPlaceSignature(hash, currentAsset ? (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(currentAsset)) >> 4) : 0ULL);
                }
                if (isCrossfading) {
                    hashInPlaceSignature(hash, 0xCFULL);
                    if (nextStateForY) {
                        hashInPlaceSignature(hash, static_cast<uint64_t>(nextStateForY->Id + 12289));
                    }
                }
                return hash;
            };
            auto resetInPlaceContinuity = [&]() {
                player._InPlaceContinuityOffsetY = 0.0f;
                player._InPlaceYSourceSignature = 0;
                player._InPlaceYSourceSignatureValid = false;
            };
            auto applyInPlaceContinuity = [&](float rawY) -> float {
                if (!std::isfinite(rawY)) return rawY;

                const uint64_t signature = makeInPlaceYSourceSignature();
                if (player._InPlaceYSourceSignatureValid &&
                    player._InPlaceYSourceSignature != signature &&
                    player._InPlaceLastAppliedYValid) {
                    player._InPlaceContinuityOffsetY = player._InPlaceLastAppliedY - rawY;
                }

                player._InPlaceYSourceSignature = signature;
                player._InPlaceYSourceSignatureValid = true;

                const float yWithOffset = rawY + player._InPlaceContinuityOffsetY;
                const float decayAlpha = 1.0f - std::exp(-18.0f * deltaTime);
                player._InPlaceContinuityOffsetY += (0.0f - player._InPlaceContinuityOffsetY) * decayAlpha;
                if (std::abs(player._InPlaceContinuityOffsetY) < 1e-4f) {
                    player._InPlaceContinuityOffsetY = 0.0f;
                }
                return yWithOffset;
            };

            auto composeBindModel = [&](int boneIndex) -> glm::mat4 {
                glm::mat4 model(1.0f);
                int bi = boneIndex;
                while (bi >= 0) {
                    model = computeLocalBind(bi) * model;
                    bi = (bi < (int)skeleton.BoneParents.size()) ? skeleton.BoneParents[bi] : -1;
                }
                return model;
            };

            auto isDescendantOf = [&](int boneIndex, int ancestorIndex) -> bool {
                if (boneIndex < 0 || ancestorIndex < 0) return false;
                int bi = boneIndex;
                while (bi >= 0) {
                    if (bi == ancestorIndex) return true;
                    bi = (bi < static_cast<int>(skeleton.BoneParents.size())) ? skeleton.BoneParents[bi] : -1;
                }
                return false;
            };

            auto hasGroundedCharacterController = [&]() -> bool {
                if (data->CharacterController && data->CharacterController->IsGrounded) return true;
                if (data->Parent != INVALID_ENTITY_ID) {
                    if (auto* pd = scene.GetEntityData(data->Parent)) {
                        if (pd->CharacterController && pd->CharacterController->IsGrounded) return true;
                    }
                }
                return false;
            };
            
            
            // Detect state changes to reset tracking
            if (player.CurrentStateId != player._RootMotionPrevStateId) {
                player._PrevRootValid = false;
                player._InPlaceBaselineValid = false;
                player._RootMotionPrevStateId = player.CurrentStateId;
            }
            
            // Helper to zero XZ translation while preserving authored vertical deltas.
            // Optional baseline normalization is used for multi-clip locomotion blends.
            auto zeroXZTranslationKeepY = [&](int boneIndex,
                                             bool baselineReference,
                                             bool enableFeetAnchor,
                                             bool continuityAnchorToCurrentPose = false,
                                             bool preserveFullAuthoredY = false,
                                             float referenceYOverride = std::numeric_limits<float>::quiet_NaN()) {
                if (boneIndex < 0 || boneIndex >= (int)localTransforms.size()) return;
                glm::vec3 Ta, Sa; glm::quat Ra;
                decomposeTRS(localTransforms[boneIndex], Ta, Ra, Sa);
                glm::vec3 Tb, Sb; glm::quat Rb;
                getBindLocalTRS(boneIndex, Tb, Rb, Sb);

                if (bypassYCurrentState || (transitionBlendActive && bypassYDuringTransition)) {
                    // Freeze Y continuity through this state/transition so it cannot pull the character
                    // up/down relative to surrounding locomotion states.
                    float yPos = player._InPlaceLastAppliedYValid ? player._InPlaceLastAppliedY : Tb.y;
                    if (!std::isfinite(yPos)) yPos = Tb.y;
                    player._InPlaceLastAppliedY = yPos;
                    player._InPlaceLastAppliedYValid = true;
                    player._FeetAnchorSmoothedYValid = false;
                    player._PrevFeetModelYValid = false;
                    player._FeetAnchorWeight = 0.0f;
                    localTransforms[boneIndex] = glm::translate(glm::vec3(Tb.x, yPos, Tb.z))
                                               * glm::mat4_cast(glm::normalize(Ra))
                                               * glm::scale(Sb);
                    return;
                }

                if (baselineReference && !player._InPlaceBaselineValid) {
                    // Capture baseline for this state/controller so the first sampled frame
                    // is continuous with previous applied pose, then preserve relative Y motion.
                    player._InPlaceBaselineY = Ta.y;
                    // During active state/controller blends, prefer continuity against the
                    // previously applied Y so transitions don't show visible settling.
                    if (transitionBlendActive && player._InPlaceLastAppliedYValid) {
                        player._InPlaceScaleFactor = player._InPlaceLastAppliedY;
                    } else if (continuityAnchorToCurrentPose) {
                        player._InPlaceScaleFactor = player._InPlaceLastAppliedYValid ? player._InPlaceLastAppliedY : Ta.y;
                    } else {
                        // Stable anchor in steady state prevents Blend1D ratcheting drift.
                        player._InPlaceScaleFactor = Tb.y;
                    }
                    player._InPlaceBaselineValid = true;
                }

                float yPos = Ta.y;
                if (baselineReference && player._InPlaceBaselineValid) {
                    const float yDelta = Ta.y - player._InPlaceBaselineY;
                    yPos = player._InPlaceScaleFactor + yDelta;
                } else if (!baselineReference) {
                    const float referenceY = std::isfinite(referenceYOverride) ? referenceYOverride : Tb.y;
                    float yOffset = Ta.y - referenceY;
                    if (!preserveFullAuthoredY) {
                        // Preserve authored offset from bind, but constrain extreme offsets
                        // (common when a clip is imported as Bone tracks with different bind assumptions).
                        const float bindAbsY = std::abs(Tb.y);
                        const float maxDown = std::max(0.10f, bindAbsY * 0.55f);
                        const float maxUp = std::max(0.08f, bindAbsY * 0.35f);
                        yOffset = glm::clamp(yOffset, -maxDown, maxUp);
                    }
                    yPos = Tb.y + yOffset;
                }
                if (std::isnan(yPos) || std::isinf(yPos)) yPos = Tb.y;

                if (!enableFeetAnchor) {
                    player._FeetAnchorSmoothedYValid = false;
                    player._PrevFeetModelYValid = false;
                    player._FeetAnchorWeight = 0.0f;
                }

                // Feet-aware anchor blending:
                // - If feet are planted and grounded, keep lowest foot near its bind-model floor
                //   to reduce transition pops and idle/run settle flicker.
                // - If feet are unplanted/airborne, reduce influence.
                if (enableFeetAnchor && hasFeetRefs) {
                    // Evaluate feet against the candidate grounded hips/root Y, not the raw sampled pose.
                    // Otherwise large local-Y rewrites (like crouch) cause the anchor to over-correct.
                    const float candidateAnchorDeltaY = yPos - Ta.y;
                    float leftCurY = 0.0f, rightCurY = 0.0f;
                    float leftBindY = 0.0f, rightBindY = 0.0f;
                    bool hasLeft = false, hasRight = false;
                    if (leftFootIdx >= 0) {
                        leftCurY = glm::vec3(composeModel(leftFootIdx)[3]).y;
                        if (isDescendantOf(leftFootIdx, boneIndex)) {
                            leftCurY += candidateAnchorDeltaY;
                        }
                        leftBindY = glm::vec3(composeBindModel(leftFootIdx)[3]).y;
                        hasLeft = true;
                    }
                    if (rightFootIdx >= 0) {
                        rightCurY = glm::vec3(composeModel(rightFootIdx)[3]).y;
                        if (isDescendantOf(rightFootIdx, boneIndex)) {
                            rightCurY += candidateAnchorDeltaY;
                        }
                        rightBindY = glm::vec3(composeBindModel(rightFootIdx)[3]).y;
                        hasRight = true;
                    }

                    float leftVel = 0.0f, rightVel = 0.0f;
                    if (player._PrevFeetModelYValid && deltaTime > 1e-4f) {
                        if (hasLeft) leftVel = (leftCurY - player._PrevLeftFootModelY) / deltaTime;
                        if (hasRight) rightVel = (rightCurY - player._PrevRightFootModelY) / deltaTime;
                    }
                    if (hasLeft) player._PrevLeftFootModelY = leftCurY;
                    if (hasRight) player._PrevRightFootModelY = rightCurY;
                    player._PrevFeetModelYValid = hasLeft || hasRight;

                    bool lowestIsLeft = false;
                    if (hasLeft && hasRight) lowestIsLeft = (leftCurY <= rightCurY);
                    else lowestIsLeft = hasLeft;

                    const float lowestCurY = lowestIsLeft ? leftCurY : rightCurY;
                    const float lowestBindY = lowestIsLeft ? leftBindY : rightBindY;
                    const float lowestVel = std::abs(lowestIsLeft ? leftVel : rightVel);
                    const bool planted = lowestVel < 0.08f;
                    const bool grounded = hasGroundedCharacterController();

                    const float targetWeight = (grounded && planted) ? 1.0f : (grounded ? 0.35f : 0.0f);
                    const float blendDampedTargetWeight = transitionBlendActive ? (targetWeight * 0.2f) : targetWeight;
                    const float gain = (blendDampedTargetWeight > player._FeetAnchorWeight) ? 14.0f : 7.0f;
                    const float wAlpha = 1.0f - std::exp(-gain * deltaTime);
                    player._FeetAnchorWeight += (blendDampedTargetWeight - player._FeetAnchorWeight) * wAlpha;
                    player._FeetAnchorWeight = glm::clamp(player._FeetAnchorWeight, 0.0f, 1.0f);

                    const float footAdjustY = (lowestBindY - lowestCurY);
                    const float anchoredY = yPos + footAdjustY;
                    const float blendedY = glm::mix(yPos, anchoredY, player._FeetAnchorWeight);
                    if (!player._FeetAnchorSmoothedYValid) {
                        player._FeetAnchorSmoothedY = blendedY;
                        player._FeetAnchorSmoothedYValid = true;
                    } else {
                        const float smoothAlpha = 1.0f - std::exp(-22.0f * deltaTime);
                        player._FeetAnchorSmoothedY += (blendedY - player._FeetAnchorSmoothedY) * smoothAlpha;
                    }
                    yPos = player._FeetAnchorSmoothedY;
                }

                yPos = applyInPlaceContinuity(yPos);

                player._InPlaceLastAppliedY = yPos;
                player._InPlaceLastAppliedYValid = true;
                
                // Zero XZ (bind pose), preserve authored Y, preserve rotation.
                localTransforms[boneIndex] = glm::translate(glm::vec3(Tb.x, yPos, Tb.z)) 
                                           * glm::mat4_cast(glm::normalize(Ra)) 
                                           * glm::scale(Sb);
            };

            switch (effectiveMode) {
                case RootMotionMode::None: {
                    // Keep rig completely in-place: zero ALL translation on hips and root
                    zeroLocalTranslationToBind(hipsIdx);
                    zeroLocalTranslationToBind(rootIdx);
                    player._PrevRootValid = false;
                    player._InPlaceBaselineValid = false;
                    player._InPlaceLastAppliedYValid = false;
                    resetInPlaceContinuity();
                    player._FeetAnchorSmoothedYValid = false;
                    player._PrevFeetModelYValid = false;
                    player._FeetAnchorWeight = 0.0f;
                } break;

                case RootMotionMode::InPlace: {
                    // Reset baseline on state change (but NOT every frame during blend trees)
                    if (player.CurrentStateId != player._InPlacePrevStateId) {
                        player._InPlaceBaselineValid = false;
                        player._InPlacePrevStateId = player.CurrentStateId;
                    }

                    const bool shouldNormalizeInPlaceY =
                        ((useBlend1D &&
                          stNowForEval &&
                          stNowForEval->Kind == cm::animation::AnimatorStateKind::Blend1D &&
                          stNowForEval->Blend1DEntries.size() > 1) ||
                         (useBlend2D &&
                         stNowForEval &&
                         stNowForEval->Kind == cm::animation::AnimatorStateKind::Blend2D &&
                         stNowForEval->Blend2DEntries.size() > 1));

                    const bool shouldNormalizeSingleHumanoidInPlaceY =
                        !shouldNormalizeInPlaceY &&
                        currentAsset &&
                        currentAsset->meta.humanoidTrackMode == HumanoidTrackMode::BindRelative;

                    // Keep one stable baseline for the whole blend-tree state lifetime.
                    // Re-capturing on parameter jumps introduces visible stop/land dips.
                    if (shouldNormalizeInPlaceY) {
                        float blendParamNow = 0.0f; // kept for compatibility/debugging
                        if (useBlend1D && stNowForEval) {
                            blendParamNow = ReadBlend1DParameter(
                                player.AnimatorInstance.Blackboard(),
                                *stNowForEval);
                        }
                        player._InPlacePrevBlendParam = blendParamNow;
                    }

                    // Keep XZ in-place, preserve vertical deltas from selected source bone,
                    // and bind-lock the non-source root/hips bone to prevent double offsets.
                    const int poseYBoneIdx = (hipsIdx >= 0) ? hipsIdx : rootMotionBoneIdx;
                    if (poseYBoneIdx >= 0) {
                        if (bypassYCurrentState || (transitionBlendActive && bypassYDuringTransition)) {
                            zeroXZTranslationKeepY(poseYBoneIdx, false, false);
                        } else
                        if (shouldNormalizeInPlaceY) {
                            // Use the exact current blend-tree reference so run/idle/walk share a
                            // stable floor baseline without damping away the authored bounce.
                            StateHipsReferenceSample refSample = resolveCurrentHipsReferenceY();
                            if (isCrossfading && nextStateForY) {
                                const StateHipsReferenceSample nextRefSample = resolveStateHipsReferenceY(nextStateForY);
                                if (refSample.found && nextRefSample.found) {
                                    refSample.y = glm::mix(refSample.y, nextRefSample.y, crossfadeAlpha);
                                    refSample.rangeY = glm::mix(refSample.rangeY, nextRefSample.rangeY, crossfadeAlpha);
                                } else if (nextRefSample.found) {
                                    refSample = nextRefSample;
                                }
                            }

                            glm::vec3 Ta, Sa; glm::quat Ra;
                            decomposeTRS(localTransforms[poseYBoneIdx], Ta, Ra, Sa);
                            glm::vec3 Tb, Sb; glm::quat Rb;
                            getBindLocalTRS(poseYBoneIdx, Tb, Rb, Sb);
                            const float targetRefY = refSample.found
                                ? refSample.y
                                : (player._InPlaceBaselineValid ? player._InPlaceBaselineY : Ta.y);
                            player._InPlaceBaselineY = targetRefY;
                            player._InPlaceBaselineValid = refSample.found;

                            float yPos = Tb.y + (Ta.y - targetRefY);
                            if (!std::isfinite(yPos)) yPos = Tb.y;
                            yPos = applyInPlaceContinuity(yPos);
                            player._InPlaceLastAppliedY = yPos;
                            player._InPlaceLastAppliedYValid = true;
                            localTransforms[poseYBoneIdx] = glm::translate(glm::vec3(Tb.x, yPos, Tb.z))
                                                          * glm::mat4_cast(glm::normalize(Ra))
                                                          * glm::scale(Sb);
                    } else if (shouldNormalizeSingleHumanoidInPlaceY) {
                        bool foundRef = false;
                        float hipsRange = 0.0f;
                        const bool useAverageReference = stNowForEval && stNowForEval->Loop;
                        (void)getAssetHipsReferenceY(currentAsset, foundRef, useAverageReference, &hipsRange);
                        const bool enableFeetAnchor =
                            (!stNowForEval || !stNowForEval->Loop) ||
                            hipsRange < 0.10f ||
                            !foundRef;
                        // For single humanoid clips, keep the vertical anchor continuous with the
                        // previously applied pose and preserve only the authored relative motion.
                        zeroXZTranslationKeepY(poseYBoneIdx, true, enableFeetAnchor, true);
                    } else {
                        // Non-locomotion in-place states: preserve authored Y directly.
                        zeroXZTranslationKeepY(poseYBoneIdx, false, true);
                    }
                    }
                    if (rootIdx >= 0 && rootIdx != poseYBoneIdx) {
                        zeroLocalTranslationToBind(rootIdx);
                    }
                    if (hipsIdx >= 0 && hipsIdx != poseYBoneIdx) {
                        zeroLocalTranslationToBind(hipsIdx);
                    }
                    if (poseYBoneIdx < 0) {
                        player._InPlaceLastAppliedYValid = false;
                        player._FeetAnchorSmoothedYValid = false;
                        resetInPlaceContinuity();
                    }
                    player._PrevRootValid = false;
                } break;

                case RootMotionMode::ApplyToEntity: {
                    if (!usingHipsAsRootSource) {
                        player._InPlaceBaselineValid = false;
                    }
                    resetInPlaceContinuity();
                    
                    if (rootMotionBoneIdx >= 0) {
                        // Get current animation time for loop detection
                        float curAnimTime = player.ActiveStates.empty() ? 0.0f : player.ActiveStates.front().Time;
                        
                        // Detect loop wrap: if time jumped backward significantly, reset tracking
                        // This prevents a huge negative delta when animation loops from end to start
                        if (player._PrevRootValid && curAnimTime < player._PrevAnimTime - 0.01f) {
                            player._PrevRootValid = false; // Skip delta this frame
                        }
                        
                        // Get current model-space position and rotation of root motion bone
                        glm::mat4 curModel = composeModel(rootMotionBoneIdx);
                        glm::vec3 curPos = glm::vec3(curModel[3]);
                        glm::quat curRot = glm::quat_cast(curModel);
                        
                        // Compute delta from previous frame
                        if (player._PrevRootValid) {
                            glm::vec3 posDelta = curPos - player._PrevRootModelPos;
                            glm::quat rotDelta = curRot * glm::inverse(player._PrevRootModelRot);
                            
                            // Apply settings filters
                            glm::vec3 filteredPosDelta(0.0f);
                            if (rmSettings.IncludeXZ) {
                                filteredPosDelta.x = posDelta.x;
                                filteredPosDelta.z = posDelta.z;
                            }
                            // Always preserve authored vertical motion.
                            // When hips is the fallback source (no explicit root), keep Y in pose
                            // and avoid double-applying it to entity motion.
                            filteredPosDelta.y = usingHipsAsRootSource ? 0.0f : posDelta.y;
                            if (bypassYCurrentState || (isCrossfading && bypassYDuringTransition)) {
                                filteredPosDelta.y = 0.0f;
                            }
                            
                            // During crossfade, blend the delta based on how much each animation contributes
                            // If current animation has root motion and next doesn't, fade OUT the delta
                            // If next animation has root motion and current doesn't, fade IN the delta
                            if (isCrossfading) {
                                bool currentHasRM = (rmSettings.Mode == RootMotionMode::ApplyToEntity);
                                bool nextHasRM = (nextRmSettings.Mode == RootMotionMode::ApplyToEntity);
                                
                                if (currentHasRM && !nextHasRM) {
                                    // Fading OUT of root motion - reduce delta as we blend out
                                    filteredPosDelta *= (1.0f - crossfadeAlpha);
                                    rotDelta = glm::slerp(rotDelta, glm::quat(1,0,0,0), crossfadeAlpha);
                                } else if (!currentHasRM && nextHasRM) {
                                    // Fading INTO root motion - would need delta from next state
                                    // For now, ramp up current delta (this is an approximation)
                                    filteredPosDelta *= crossfadeAlpha;
                                    rotDelta = glm::slerp(glm::quat(1,0,0,0), rotDelta, crossfadeAlpha);
                                }
                                // If both have root motion, use full delta (they'll blend naturally)
                            }
                            
                            // Output the delta for physics consumption
                            player._RootMotionOutput.PositionDelta = filteredPosDelta;
                            if (rmSettings.IncludeRotation) {
                                player._RootMotionOutput.RotationDelta = rotDelta;
                            }
                            // Only override gravity while animation is actively playing
                            // When animation finishes (non-looping), gravity should resume
                            bool animationStillActive = false;
                            // Check if animation is still active (looping OR not yet at end)
                            float clipDur = currentAsset ? currentAsset->Duration() : 0.0f;
                            animationStillActive = state.Loop || (clipDur <= 0.0f) || (curAnimTime < clipDur - 0.001f);
                            // Also respect IsPlaying flag
                            animationStillActive = animationStillActive && player.IsPlaying;
                            player._RootMotionOutput.OverrideGravity = rmSettings.OverrideGravity && animationStillActive;
                            player._RootMotionOutput.Valid = true;
                        }
                        
                        // Store current position/rotation/time for next frame's delta calculation
                        player._PrevRootModelPos = curPos;
                        player._PrevRootModelRot = curRot;
                        player._PrevAnimTime = curAnimTime;
                        player._PrevRootValid = true;
                        
                        // Keep authored vertical pose when hips is the motion source
                        // so crouches/squats do not hover when entity movement is constrained.
                        if (usingHipsAsRootSource) {
                            // For hips-driven ApplyToEntity clips (e.g., stand->crouch),
                            // preserve authored local Y directly. Baseline normalization here
                            // can erase lowered crouch poses when the clip starts from a lower hips height.
                            zeroXZTranslationKeepY(rootMotionBoneIdx, false, true);
                        } else {
                            zeroLocalTranslationToBind(rootMotionBoneIdx);
                        }
                        if (rootIdx >= 0 && rootIdx != rootMotionBoneIdx) {
                            zeroLocalTranslationToBind(rootIdx);
                        }
                    } else {
                        player._PrevRootValid = false;
                    }
                } break;
            }
        }

        // =========================================================================
        // Base Layer State Mask Filtering
        // =========================================================================
        // If the base layer's current state has a mask override (checkbox-based body parts),
        // reset masked-out bones to bind pose. This allows upper body layers to have full control
        // without any spine rotation bleeding through from walk/run animations.
        // =========================================================================
        if (player.Controller && skeleton.Avatar && stNowForEval && stNowForEval->HasMaskOverride()) {
            uint8_t maskBits = stNowForEval->GetMaskBits();
            
            // Reset bones NOT in the mask back to bind pose
            for (uint16_t hb = 0; hb < HumanoidBoneCount; ++hb) {
                const HumanoidBone humanBone = static_cast<HumanoidBone>(hb);
                if (StateMaskIncludesBone(humanBone, maskBits)) continue; // Keep animated
                
                // Bone is masked OUT - reset to bind pose
                const int skelIdx = skeleton.Avatar->GetMappedBoneIndex(humanBone);
                if (skelIdx < 0 || skelIdx >= static_cast<int>(localTransforms.size())) continue;
                
                localTransforms[skelIdx] = computeLocalBind(skelIdx);
            }
        }

        // =========================================================================
        // Animation Layer Compositing (Controller-Based Layers)
        // =========================================================================
        // Process animation layers defined in the controller.
        // Layer 0 = base layer (already evaluated above).
        // Layers 1+ are overlay layers with their own state machines and body masks.
        // 
        // Performance optimizations:
        // - Early culling of zero-weight layers (including those blending to zero)
        // - Shared blackboard access across all layers
        // - Pre-computed mask-to-skeleton index mapping
        // - Thread-local pose buffer pooling
        // =========================================================================
        if (player.Controller && player.Controller->GetLayerCount() > 1 && skeleton.Avatar) {
            // Cache shared blackboard reference - all layers read from the same parameters
            const AnimatorBlackboard& sharedBlackboard = player.AnimatorInstance.Blackboard();
            
            // Pre-allocate/reuse layer pose buffer (thread-local for safety)
            thread_local PoseTRSBuffer layerPoseBuffer;
            thread_local std::vector<std::shared_ptr<AnimationAsset>> layerBlendAssets2D;
            thread_local std::vector<float> layerBlendWeights2D;
            thread_local std::vector<cm::animation::ScriptEvent> layerFiredEvents;
            const size_t boneCount = skeleton.BoneEntities.size();
            
            // Iterate layers 1+ (layer 0 is the base layer already evaluated)
            for (int layerIdx = 1; layerIdx < player.Controller->GetLayerCount(); ++layerIdx) {
                const AnimatorLayer* controllerLayer = player.Controller->GetLayer(layerIdx);
                if (!controllerLayer) continue;
                
                // Get or create runtime layer state
                AnimatorLayerState* layerState = player.AnimatorInstance.GetLayerState(layerIdx);
                if (!layerState) {
                    layerState = &player.AnimatorInstance.GetOrCreateLayerState(layerIdx);
                }
                
                // OPTIMIZATION: Early weight culling
                // Skip if weight is zero AND not blending towards a non-zero weight
                // This avoids sampling animations for fully inactive layers
                const bool weightIsZero = layerState->Weight < 0.001f;
                const bool targetIsZero = layerState->TargetWeight < 0.001f;
                if (weightIsZero && targetIsZero) continue;
                
                // Update weight blending towards target (only if needed)
                if (std::abs(layerState->Weight - layerState->TargetWeight) > 0.001f) {
                    float blendStep = layerState->BlendSpeed * deltaTime;
                    if (layerState->Weight < layerState->TargetWeight) {
                        layerState->Weight = std::min(layerState->Weight + blendStep, layerState->TargetWeight);
                    } else {
                        layerState->Weight = std::max(layerState->Weight - blendStep, layerState->TargetWeight);
                    }
                }
                
                // Skip if weight is still effectively zero after blending
                if (layerState->Weight < 0.001f) continue;
                
                // Initialize state if needed (but only for layers that have a default state)
                if (layerState->CurrentStateId < 0 && controllerLayer->DefaultState >= 0) {
                    layerState->CurrentStateId = controllerLayer->DefaultState;
                }
                
                // Get current state for this layer (may be null for layers with no entry state)
                const AnimatorState* layerAnimState = controllerLayer->FindState(layerState->CurrentStateId);
                
                // For layers with no valid state, still check for "Any State" transitions
                // that could activate the layer
                if (!layerAnimState) {
                    // Check if there's a transition to enter this layer
                    int nextState = player.AnimatorInstance.ChooseNextStateForLayer(layerIdx);
                    if (nextState >= 0) {
                        // Found an AnyState transition - enter that state
                        layerState->SetCurrentState(nextState, true);
                        // Only consume triggers used by THIS transition
                        const auto* entryTransition = player.AnimatorInstance.FindTransitionToForLayer(layerIdx, nextState);
                        player.AnimatorInstance.ConsumeTriggersForTransition(entryTransition);
                        layerAnimState = controllerLayer->FindState(layerState->CurrentStateId);
                    }
                    // If still no valid state, skip this layer's animation sampling
                    if (!layerAnimState) continue;
                }
                
                // Variables for layer animation evaluation
                std::shared_ptr<AnimationAsset> layerAsset;
                std::shared_ptr<AnimationAsset> layerAssetB0, layerAssetB1;
                layerBlendAssets2D.clear();
                layerBlendWeights2D.clear();
                float layerDuration = 0.0f;
                float layerBlendT = 0.0f;
                bool layerUseBlend1D = false;
                bool layerUseBlend2D = false;
                
                // Handle Blend1D states in overlay layers (using shared blackboard)
                if (layerAnimState->Kind == AnimatorStateKind::Blend1D && !layerAnimState->Blend1DEntries.empty()) {
                    // Read blend parameter from shared blackboard
                    const float x = ReadBlend1DParameter(sharedBlackboard, *layerAnimState);
                    
                    // Find surrounding blend entries
                    const auto& e = layerAnimState->Blend1DEntries;
                    int i1 = 0, i2 = static_cast<int>(e.size()) - 1;
                    for (int i = 0; i < static_cast<int>(e.size()); ++i) {
                        if (e[i].Key <= x) i1 = i;
                        if (e[i].Key >= x) { i2 = i; break; }
                    }
                    i1 = glm::clamp(i1, 0, static_cast<int>(e.size()) - 1);
                    i2 = glm::clamp(i2, 0, static_cast<int>(e.size()) - 1);
                    
                    const auto& a = e[i1];
                    const auto& b = e[i2];
                    float denom = std::max(1e-6f, b.Key - a.Key);
                    layerBlendT = glm::clamp((x - a.Key) / denom, 0.0f, 1.0f);
                    
                    // Load blend entry assets
                    std::string pathA = ResolveAnimationBinding(player, a).EffectivePath();
                    std::string pathB = ResolveAnimationBinding(player, b).EffectivePath();
                    int cacheKeyA = (layerIdx * 10000) + layerAnimState->Id * 100 + i1;
                    int cacheKeyB = (layerIdx * 10000) + layerAnimState->Id * 100 + i2;
                    
                    if (!pathA.empty()) {
                        auto ita = player.CachedAssets.find(cacheKeyA);
                        if (ita != player.CachedAssets.end()) layerAssetB0 = ita->second;
                        else { layerAssetB0 = std::make_shared<AnimationAsset>(LoadAnimationAsset(pathA)); player.CachedAssets[cacheKeyA] = layerAssetB0; }
                    }
                    if (!pathB.empty()) {
                        auto ita = player.CachedAssets.find(cacheKeyB);
                        if (ita != player.CachedAssets.end()) layerAssetB1 = ita->second;
                        else { layerAssetB1 = std::make_shared<AnimationAsset>(LoadAnimationAsset(pathB)); player.CachedAssets[cacheKeyB] = layerAssetB1; }
                    }
                    
                    collapseBlend1DContributors(layerAssetB0, layerAssetB1, i1, i2, layerBlendT, layerDuration);
                    layerUseBlend1D = true;
                    
                    if (!layerAssetB0 && !layerAssetB1) continue;
                } else if (layerAnimState->Kind == AnimatorStateKind::Blend2D && !layerAnimState->Blend2DEntries.empty()) {
                    std::array<int, 4> indices{};
                    std::array<float, 4> weights{};
                    int sampleCount = 0;
                    if (!resolveBlend2DWeights(layerAnimState, sharedBlackboard, indices, weights, sampleCount)) continue;

                    float loadedWeight = 0.0f;
                    for (int i = 0; i < sampleCount; ++i) {
                        const auto& entry = layerAnimState->Blend2DEntries[indices[i]];
                        const std::string path = ResolveAnimationBinding(player, entry).EffectivePath();
                        auto sampleAsset = loadBlendAsset(layerIdx, layerAnimState->Id, indices[i], path);
                        if (!sampleAsset) continue;
                        layerBlendAssets2D.push_back(sampleAsset);
                        layerBlendWeights2D.push_back(weights[i]);
                        layerDuration += weights[i] * sampleAsset->Duration();
                        loadedWeight += weights[i];
                    }
                    if (loadedWeight <= 1e-6f || layerBlendAssets2D.empty()) continue;
                    for (float& w : layerBlendWeights2D) w /= loadedWeight;
                    layerDuration /= loadedWeight;
                    layerUseBlend2D = true;
                } else {
                    // Single animation state
                    std::string assetPath = ResolveAnimationBinding(player, *layerAnimState).EffectivePath();
                    if (assetPath.empty()) continue;
                    
                    // Cache key: encode layer index and state id to avoid collisions with base layer
                    int cacheKey = (layerIdx * 10000) + layerAnimState->Id;
                    auto ita = player.CachedAssets.find(cacheKey);
                    if (ita != player.CachedAssets.end()) {
                        layerAsset = ita->second;
                    } else {
                        layerAsset = std::make_shared<AnimationAsset>(LoadAnimationAsset(assetPath));
                        player.CachedAssets[cacheKey] = layerAsset;
                    }
                    if (!layerAsset) continue;
                    
                    layerDuration = layerAsset->Duration();
                }
                
                const float prevLayerTime = layerState->StateTime;
                // Update layer FSM (transitions, state changes)
                player.AnimatorInstance.UpdateLayer(layerIdx, deltaTime * layerAnimState->Speed * player.PlaybackSpeed, layerDuration);
                
                // Check transitions for this layer
                int nextState = player.AnimatorInstance.ChooseNextStateForLayer(layerIdx);
                if (nextState >= 0 && nextState != layerState->CurrentStateId) {
                    if (!layerState->IsCrossfading() || layerState->NextStateId != nextState) {
                        float duration = 0.0f;
                        bool durationNormalized = false;
                        const auto* firedTransition = player.AnimatorInstance.FindTransitionToForLayer(layerIdx, nextState);
                        if (firedTransition) {
                            duration = firedTransition->Duration;
                            durationNormalized = firedTransition->DurationNormalized;
                        }
                        float durationSeconds = durationNormalized ? (duration * std::max(0.0f, layerDuration)) : duration;
                        if (durationSeconds > 0.0f) {
                            layerState->BeginCrossfade(nextState, durationSeconds);
                            // Only consume triggers used by THIS transition (allows other layers to use their own triggers)
                            player.AnimatorInstance.ConsumeTriggersForTransition(firedTransition);
                        } else {
                            layerState->SetCurrentState(nextState, true);
                            // Only consume triggers used by THIS transition (allows other layers to use their own triggers)
                            player.AnimatorInstance.ConsumeTriggersForTransition(firedTransition);
                            // Reload state for new current state
                            layerAnimState = controllerLayer->FindState(layerState->CurrentStateId);
                            if (!layerAnimState) continue;
                            std::string newAssetPath = ResolveAnimationBinding(player, *layerAnimState).EffectivePath();
                            int newCacheKey = (layerIdx * 10000) + layerAnimState->Id;
                            auto newIta = player.CachedAssets.find(newCacheKey);
                            if (newIta != player.CachedAssets.end()) {
                                layerAsset = newIta->second;
                            } else {
                                layerAsset = std::make_shared<AnimationAsset>(LoadAnimationAsset(newAssetPath));
                                player.CachedAssets[newCacheKey] = layerAsset;
                            }
                            layerDuration = layerAsset ? layerAsset->Duration() : 0.0f;
                        }
                    }
                }
                
                // Handle crossfade completion for layer
                if (layerState->IsCrossfading()) {
                    layerState->AdvanceCrossfade(deltaTime * player.PlaybackSpeed);
                }
                if (!layerState->IsCrossfading() && layerState->NextStateId >= 0 && 
                    layerState->CurrentStateId != layerState->NextStateId) {
                    float preservedTime = layerState->NextStateTime;
                    layerState->SetCurrentState(layerState->NextStateId, false);
                    layerState->StateTime = preservedTime;
                    // Reload for new state
                    layerAnimState = controllerLayer->FindState(layerState->CurrentStateId);
                    if (!layerAnimState) continue;
                    std::string newAssetPath = ResolveAnimationBinding(player, *layerAnimState).EffectivePath();
                    int newCacheKey = (layerIdx * 10000) + layerAnimState->Id;
                    auto newIta = player.CachedAssets.find(newCacheKey);
                    if (newIta != player.CachedAssets.end()) {
                        layerAsset = newIta->second;
                    } else {
                        layerAsset = std::make_shared<AnimationAsset>(LoadAnimationAsset(newAssetPath));
                        player.CachedAssets[newCacheKey] = layerAsset;
                    }
                    layerDuration = layerAsset ? layerAsset->Duration() : 0.0f;
                }
                
                // NOTE: StateTime is already advanced by UpdateLayer() above
                // Only clamp/wrap for non-looping animations here (UpdateLayer uses fmod which loops)
                if (!layerAnimState->Loop && layerDuration > 0.0f) {
                    layerState->StateTime = glm::clamp(layerState->StateTime, 0.0f, layerDuration);
                }
                int layerLoopCount = 0;
                if (layerAnimState->Loop && layerDuration > 0.0f) {
                    const int prevCycles = static_cast<int>(std::floor(prevLayerTime / layerDuration));
                    const int currCycles = static_cast<int>(std::floor(layerState->StateTime / layerDuration));
                    layerLoopCount = std::max(0, currCycles - prevCycles);
                }
                const int currentLayerStateId = layerState->CurrentStateId;
                const bool layerStateChanged = (currentLayerStateId != layerState->_PrevEventStateId);
                const float layerEventPrevTime = layerStateChanged ? layerState->StateTime : prevLayerTime;
                const int layerEventLoopCount = layerStateChanged ? 0 : layerLoopCount;
                
                // Sample the layer's animation using pooled sparse TRS buffers.
                preparePoseTrsBuffer(layerPoseBuffer);
                
                // Thread-local binding cache for layer evaluation
                thread_local BindingCache layerBindings;
                layerBindings.SetScene(&scene);
                layerBindings.SetSkeleton(&skeleton);
                
                // Collect script events from layer animations
                layerFiredEvents.clear();
                
                if (layerUseBlend1D) {
                    // Blend1D: exact endpoints collapse to one layer sample.
                    thread_local PoseTRSBuffer poseA, poseB;
                    preparePoseTrsBuffer(poseA);
                    
                    const float d0 = layerAssetB0 ? layerAssetB0->Duration() : 0.0f;
                    const float tA = (d0 > 0.0f) ? fmod(layerState->StateTime, d0) : 0.0f;
                    const bool layerHasPrevTime = !layerStateChanged;
                    
                    if (layerAssetB0) {
                        noteSampledAssetScaleMode(layerAssetB0.get());
                        int loopCountA = 0;
                        if (layerHasPrevTime && layerAnimState->Loop && d0 > 0.0f) {
                            const int prevCycles = static_cast<int>(std::floor(prevLayerTime / d0));
                            const int currCycles = static_cast<int>(std::floor(layerState->StateTime / d0));
                            loopCountA = std::max(0, currCycles - prevCycles);
                        }
                        EvalInputs inA{ layerAssetB0.get(), tA, layerAnimState->Loop, layerEventPrevTime, layerHasPrevTime, loopCountA };
                        EvalTargets tgtA{};
                        tgtA.poseTrs = &poseA;
                        EvalContext ctxA{ &layerBindings, skeleton.Avatar.get(), &skeleton };
                        SampleAsset(inA, ctxA, tgtA, &layerFiredEvents, nullptr);
                    }

                    if (!layerAssetB1) {
                        for (size_t i = 0; i < boneCount; ++i) {
                            if (!poseA.touched[i]) continue;
                            layerPoseBuffer.touched[i] = true;
                            layerPoseBuffer.translation[i] = poseA.translation[i];
                            layerPoseBuffer.rotation[i] = poseA.rotation[i];
                            layerPoseBuffer.scale[i] = poseA.scale[i];
                        }
                    } else {
                        preparePoseTrsBuffer(poseB);

                        const float d1 = layerAssetB1->Duration();
                        const float tB = (d1 > 0.0f) ? fmod(layerState->StateTime, d1) : 0.0f;
                        noteSampledAssetScaleMode(layerAssetB1.get());
                        int loopCountB = 0;
                        if (layerHasPrevTime && layerAnimState->Loop && d1 > 0.0f) {
                            const int prevCycles = static_cast<int>(std::floor(prevLayerTime / d1));
                            const int currCycles = static_cast<int>(std::floor(layerState->StateTime / d1));
                            loopCountB = std::max(0, currCycles - prevCycles);
                        }
                        EvalInputs inB{ layerAssetB1.get(), tB, layerAnimState->Loop, layerEventPrevTime, layerHasPrevTime, loopCountB };
                        EvalTargets tgtB{};
                        tgtB.poseTrs = &poseB;
                        EvalContext ctxB{ &layerBindings, skeleton.Avatar.get(), &skeleton };
                        SampleAsset(inB, ctxB, tgtB, &layerFiredEvents, nullptr);

                        for (size_t i = 0; i < boneCount; ++i) {
                            bool touchedA = poseA.touched[i];
                            bool touchedB = poseB.touched[i];
                            if (!touchedA && !touchedB) continue;

                            layerPoseBuffer.touched[i] = true;
                            if (touchedA && touchedB) {
                                layerPoseBuffer.translation[i] = glm::mix(poseA.translation[i], poseB.translation[i], layerBlendT);
                                layerPoseBuffer.rotation[i] = glm::normalize(glm::slerp(poseA.rotation[i], poseB.rotation[i], layerBlendT));
                                layerPoseBuffer.scale[i] = glm::mix(poseA.scale[i], poseB.scale[i], layerBlendT);
                            } else if (touchedA) {
                                layerPoseBuffer.translation[i] = poseA.translation[i];
                                layerPoseBuffer.rotation[i] = poseA.rotation[i];
                                layerPoseBuffer.scale[i] = poseA.scale[i];
                            } else {
                                layerPoseBuffer.translation[i] = poseB.translation[i];
                                layerPoseBuffer.rotation[i] = poseB.rotation[i];
                                layerPoseBuffer.scale[i] = poseB.scale[i];
                            }
                        }
                    }
                } else if (layerUseBlend2D) {
                    bool initialized = false;
                    float accumulatedWeight = 0.0f;
                    const bool layerHasPrevTime = !layerStateChanged;

                    for (size_t bi = 0; bi < layerBlendAssets2D.size() && bi < layerBlendWeights2D.size(); ++bi) {
                        auto& sampleAsset = layerBlendAssets2D[bi];
                        const float weight = layerBlendWeights2D[bi];
                        if (!sampleAsset || weight <= 1e-6f) continue;
                        noteSampledAssetScaleMode(sampleAsset.get());

                        thread_local PoseTRSBuffer poseBlend;
                        preparePoseTrsBuffer(poseBlend);

                        const float duration = sampleAsset->Duration();
                        const float sampleTime = (duration > 0.0f) ? fmod(layerState->StateTime, duration) : 0.0f;
                        int sampleLoopCount = 0;
                        if (layerHasPrevTime && layerAnimState->Loop && duration > 0.0f) {
                            const int prevCycles = static_cast<int>(std::floor(prevLayerTime / duration));
                            const int currCycles = static_cast<int>(std::floor(layerState->StateTime / duration));
                            sampleLoopCount = std::max(0, currCycles - prevCycles);
                        }
                        EvalInputs blendIn{ sampleAsset.get(), sampleTime, layerAnimState->Loop, layerEventPrevTime, layerHasPrevTime, sampleLoopCount };
                        EvalTargets blendTgt{};
                        blendTgt.poseTrs = &poseBlend;
                        EvalContext blendCtx{ &layerBindings, skeleton.Avatar.get(), &skeleton };
                        SampleAsset(blendIn, blendCtx, blendTgt, &layerFiredEvents, nullptr);

                        for (size_t i = 0; i < boneCount; ++i) {
                            if (!poseBlend.touched[i]) {
                                continue;
                            }
                            if (!initialized) {
                                layerPoseBuffer.translation[i] = poseBlend.translation[i];
                                layerPoseBuffer.rotation[i] = poseBlend.rotation[i];
                                layerPoseBuffer.scale[i] = poseBlend.scale[i];
                                layerPoseBuffer.touched[i] = true;
                                continue;
                            }
                            if (!layerPoseBuffer.touched[i]) {
                                layerPoseBuffer.translation[i] = poseBlend.translation[i];
                                layerPoseBuffer.rotation[i] = poseBlend.rotation[i];
                                layerPoseBuffer.scale[i] = poseBlend.scale[i];
                                layerPoseBuffer.touched[i] = true;
                                continue;
                            }

                            const float t = weight / std::max(1e-6f, accumulatedWeight + weight);
                            layerPoseBuffer.translation[i] = glm::mix(layerPoseBuffer.translation[i], poseBlend.translation[i], t);
                            layerPoseBuffer.rotation[i] = glm::normalize(glm::slerp(layerPoseBuffer.rotation[i], poseBlend.rotation[i], t));
                            layerPoseBuffer.scale[i] = glm::mix(layerPoseBuffer.scale[i], poseBlend.scale[i], t);
                        }

                        if (!initialized) {
                            initialized = true;
                            accumulatedWeight = weight;
                        } else {
                            accumulatedWeight += weight;
                        }
                    }
                } else {
                    // Single animation: sample directly
                    noteSampledAssetScaleMode(layerAsset.get());
                    EvalInputs layerIn{ layerAsset.get(), layerState->StateTime, layerAnimState->Loop, layerEventPrevTime, !layerStateChanged, layerEventLoopCount };
                    EvalTargets layerTgt{};
                    layerTgt.poseTrs = &layerPoseBuffer;
                    EvalContext layerCtx{ &layerBindings, skeleton.Avatar.get(), &skeleton };
                    SampleAsset(layerIn, layerCtx, layerTgt, &layerFiredEvents, nullptr);
                }
                
                // Update event tracking even if no events fired (prevents missing events after state changes)
                const float currentLayerTime = layerState->StateTime;
                const bool layerAnimLooped = (layerEventLoopCount > 0) || (currentLayerTime < layerState->_PrevEventStateTime - 0.001f);
                if (layerStateChanged || layerAnimLooped) {
                    layerState->_FiredEventIds.clear();
                }
                layerState->_PrevEventStateId = currentLayerStateId;
                layerState->_PrevEventStateTime = currentLayerTime;

                // Dispatch script events from this layer to managed scripts
                if (!layerFiredEvents.empty()) {
                    auto* rootData = scene.GetEntityData(rootId);
                    if (rootData) {
                        for (const auto& ev : layerFiredEvents) {
                            // Skip events that have already been fired this animation cycle
                            if (layerState->_FiredEventIds.count(ev.id) > 0) continue;
                            layerState->_FiredEventIds.insert(ev.id);

                            const std::string& targetClass  = ev.className;
                            const std::string& targetMethod = ev.method;

                            // Serialize payload to JSON string for interop
                            std::string payloadJson = ev.payload.dump();

                            // Dispatch to registered event listeners by event name (className)
#ifndef CLAYMORE_RUNTIME
                            DispatchAnimationEvent(
                                static_cast<int>(rootId),
                                targetClass.c_str(),
                                targetClass.c_str(),
                                targetMethod.c_str(),
                                payloadJson.c_str()
                            );
#endif

                            // LEGACY: Also dispatch to scripts attached to the skeleton root that match the class name
                            for (auto& script : rootData->Scripts) {
                                if (!script.Instance) continue;
                                if (script.ClassName != targetClass) continue;
                                if (script.Instance->GetBackend() == ScriptBackend::Managed) {
                                    auto managed = std::dynamic_pointer_cast<ManagedScriptComponent>(script.Instance);
                                    if (managed) {
                                        cm::script::Invoke(managed->GetHandle(), targetMethod.c_str());
                                    }
                                }
                            }
                        }
                    }
                }
                
                // Cache layer weight for compositing (avoid repeated member access)
                const float layerWeight = layerState->Weight;
                const bool isAdditive = (layerState->BlendMode == AnimatorLayerBlendMode::Additive);
                
                // Composite: blend layer pose into localTransforms for masked bones only
                // Pre-cache mask reference to avoid repeated virtual calls
                const AvatarMask& mask = layerState->Mask;
                const uint8_t stateMaskBits = layerAnimState->GetMaskBits();
                const bool hasStateMask = layerAnimState->HasMaskOverride();
                const bool layerCanBorrowUpperBodySupport = LayerPresetCanBorrowUpperBodySupport(mask.Preset);
                bool earlierOverlayLayerOwnsUpperBodySupport = false;
                if (layerCanBorrowUpperBodySupport && layerAnimState->Loop && player.Controller) {
                    for (const auto& priorLayerState : player.AnimatorInstance.LayerStates()) {
                        if (priorLayerState.LayerIndex <= 0 || priorLayerState.LayerIndex >= layerIdx) continue;
                        if (!priorLayerState.IsActive()) continue;
                        if (priorLayerState.Weight <= 0.001f && priorLayerState.TargetWeight <= 0.001f) continue;

                        const auto* priorControllerLayer = player.Controller->GetLayer(priorLayerState.LayerIndex);
                        if (!priorControllerLayer) continue;
                        const auto* priorAnimState = priorControllerLayer->FindState(priorLayerState.CurrentStateId);
                        if (!priorAnimState) continue;

                        const bool priorExplicitUpperBodySupport =
                            LayerPresetProvidesUpperBodySupport(priorLayerState.Mask.Preset) ||
                            (priorAnimState->HasMaskOverride() &&
                             MaskBitsIncludeUpperBodySupport(priorAnimState->GetMaskBits()));
                        const bool priorActionUpperBodySupport =
                            LayerPresetCanBorrowUpperBodySupport(priorLayerState.Mask.Preset) &&
                            (!priorAnimState->Loop || priorLayerState.IsCrossfading());
                        if (priorExplicitUpperBodySupport || priorActionUpperBodySupport) {
                            earlierOverlayLayerOwnsUpperBodySupport = true;
                            break;
                        }
                    }
                }
                
                for (uint16_t hb = 0; hb < HumanoidBoneCount; ++hb) {
                    const HumanoidBone humanBone = static_cast<HumanoidBone>(hb);
                    // Map humanoid bone to skeleton bone index
                    const int skelIdx = skeleton.Avatar->GetMappedBoneIndex(humanBone);
                    if (skelIdx < 0 || skelIdx >= static_cast<int>(localTransforms.size())) continue;

                    const bool inLayerMask = mask.IncludesBone(humanBone);
                    const bool inStateMask = hasStateMask && StateMaskIncludesBone(humanBone, stateMaskBits);
                    const bool borrowedUpperBodySupport =
                        !inLayerMask &&
                        !inStateMask &&
                        layerCanBorrowUpperBodySupport &&
                        IsArmLayerSupportBone(humanBone) &&
                        !(earlierOverlayLayerOwnsUpperBodySupport && layerAnimState->Loop) &&
                        skelIdx < static_cast<int>(layerPoseBuffer.touched.size()) &&
                        layerPoseBuffer.touched[skelIdx];
                    const bool armLayerSpineShouldStayWithLocomotion =
                        layerCanBorrowUpperBodySupport &&
                        !inLayerMask &&
                        humanBone == HumanoidBone::Spine &&
                        inStateMask;
                    if (armLayerSpineShouldStayWithLocomotion) continue;
                    if (!inLayerMask && !inStateMask && !borrowedUpperBodySupport) continue;

                    // Only blend if the layer animation actually touched this bone
                    if (!layerPoseBuffer.touched[skelIdx]) continue;
                    
                    // Decompose both base and layer transforms
                    glm::vec3 T0, S0, T1, S1;
                    glm::quat R0, R1;
                    decomposeTRS(localTransforms[skelIdx], T0, R0, S0);
                    T1 = layerPoseBuffer.translation[skelIdx];
                    R1 = layerPoseBuffer.rotation[skelIdx];
                    S1 = layerPoseBuffer.scale[skelIdx];
                    const bool upperBodySupportFromArmLayer =
                        layerCanBorrowUpperBodySupport &&
                        IsArmLayerSupportBone(humanBone) &&
                        (inStateMask || borrowedUpperBodySupport);
                    
                    if (!isAdditive) {
                        if (upperBodySupportFromArmLayer) {
                            // Torso/head tracks on arm-only layers are support data. Apply them as
                            // deltas from bind so locomotion sway survives underneath the action pose.
                            glm::vec3 Tb, Sb; glm::quat Rb;
                            getBindLocalTRS(skelIdx, Tb, Rb, Sb);
                            const glm::vec3 supportDeltaT = T1 - Tb;
                            const glm::quat supportDeltaR = glm::normalize(R1 * glm::conjugate(Rb));
                            const glm::vec3 T = T0 + supportDeltaT * layerWeight;
                            const glm::quat weightedSupportR =
                                glm::slerp(glm::quat(1, 0, 0, 0), supportDeltaR, layerWeight);
                            const glm::quat R = glm::normalize(weightedSupportR * R0);
                            localTransforms[skelIdx] = glm::translate(glm::mat4(1.0f), T) *
                                                       glm::mat4_cast(R) *
                                                       glm::scale(glm::mat4(1.0f), S0);
                        } else {
                            // Override: lerp between base and layer based on weight
                            glm::vec3 T = glm::mix(T0, T1, layerWeight);
                            glm::quat R = glm::slerp(R0, R1, layerWeight);
                            glm::vec3 S = glm::mix(S0, S1, layerWeight);
                            // Higher layers must not affect root authority. Keep hips/root TRS from base.
                            if (layerIdx > 0 && (humanBone == HumanoidBone::Hips || humanBone == HumanoidBone::Root)) {
                                T = T0;
                                R = R0;
                                S = S0;
                            }
                            localTransforms[skelIdx] = glm::translate(glm::mat4(1.0f), T) * 
                                                       glm::mat4_cast(glm::normalize(R)) * 
                                                       glm::scale(glm::mat4(1.0f), S);
                        }
                    } else {
                        // Additive: compute delta from reference pose and add scaled by weight
                        glm::quat weightedDeltaR = glm::slerp(glm::quat(1,0,0,0), R1, layerWeight);
                        glm::vec3 weightedDeltaT = T1 * layerWeight;
                        if (layerIdx > 0 && (humanBone == HumanoidBone::Hips || humanBone == HumanoidBone::Root)) {
                            weightedDeltaT = glm::vec3(0.0f);
                            weightedDeltaR = glm::quat(1,0,0,0);
                        }
                        
                        const glm::vec3 T = T0 + weightedDeltaT;
                        const glm::quat R = glm::normalize(weightedDeltaR * R0);
                        
                        localTransforms[skelIdx] = glm::translate(glm::mat4(1.0f), T) * 
                                                   glm::mat4_cast(R) * 
                                                   glm::scale(glm::mat4(1.0f), S0);
                    }
                }
            }
        }

        // Controller switch blend: smoothly mix from last pose to the new controller pose
        if (player._ControllerSwitchBlendActive) {
            if (player._ControllerSwitchPose.size() == localTransforms.size()) {
                player._ControllerSwitchBlendTime += deltaTime;
                float alpha = player._ControllerSwitchBlendDuration > 0.0f
                    ? glm::clamp(player._ControllerSwitchBlendTime / player._ControllerSwitchBlendDuration, 0.0f, 1.0f)
                    : 1.0f;
                auto blendTRS = [](const glm::mat4& a, const glm::mat4& b, float t) {
                    glm::vec3 T0, S0, T1, S1; glm::quat R0, R1;
                    auto decompose = [](const glm::mat4& m, glm::vec3& T, glm::quat& R, glm::vec3& S){
                        T = glm::vec3(m[3]);
                        glm::vec3 X = glm::vec3(m[0]);
                        glm::vec3 Y = glm::vec3(m[1]);
                        glm::vec3 Z = glm::vec3(m[2]);
                        S = glm::vec3(glm::length(X), glm::length(Y), glm::length(Z));
                        if (S.x > 1e-6f) X /= S.x; if (S.y > 1e-6f) Y /= S.y; if (S.z > 1e-6f) Z /= S.z;
                        glm::mat3 rotMat(X, Y, Z);
                        R = glm::quat_cast(rotMat);
                    };
                    decompose(a, T0, R0, S0);
                    decompose(b, T1, R1, S1);
                    glm::vec3 T = glm::mix(T0, T1, t);
                    glm::quat R = glm::slerp(R0, R1, t);
                    glm::vec3 S = glm::mix(S0, S1, t);
                    return glm::translate(T) * glm::mat4_cast(glm::normalize(R)) * glm::scale(S);
                };
                for (size_t i = 0; i < localTransforms.size(); ++i) {
                    localTransforms[i] = blendTRS(player._ControllerSwitchPose[i], localTransforms[i], alpha);
                }
                if (alpha >= 1.0f) {
                    player._ControllerSwitchBlendActive = false;
                }
            } else {
                player._ControllerSwitchBlendActive = false;
            }
        }

        const size_t materializedBoneCount =
            std::min(localTransforms.size(), skeleton.BoneEntities.size());
        const bool hasPostAnimationConstraints =
            scene.m_IsPlaying &&
            (HasActiveLookAtConstraint(*data) ||
             HasPotentialIkConstraint(*data));
        const bool shouldWriteBoneEntities = ShouldWriteAnimatedBoneEntities(
            scene,
            *data);
        const bool shouldPreserveRuntimeScale =
            shouldWriteBoneEntities ||
            sampledPoseNeedsRuntimeScalePreservation;

        // Preserve runtime/bind scale: animations only drive position and rotation.
        if (shouldPreserveRuntimeScale && !localTransforms.empty()) {
            for (size_t i = 0; i < localTransforms.size(); ++i) {
                glm::vec3 T, S; glm::quat R;
                decomposeTRS(localTransforms[i], T, R, S);

                glm::vec3 desiredScale;
                bool hasRuntimeScale = false;
                if (shouldWriteBoneEntities && i < skeleton.BoneEntities.size()) {
                    EntityID boneId = skeleton.BoneEntities[i];
                    if (boneId != (EntityID)-1) {
                        if (auto* bd = scene.GetEntityData(boneId)) {
                            desiredScale = bd->Transform.Scale;
                            hasRuntimeScale = true;
                        }
                    }
                }
                if (!hasRuntimeScale) {
                    // Fallback to bind scale if no runtime scale is available.
                    glm::vec3 bindT, bindS; glm::quat bindR;
                    getBindLocalTRS(static_cast<int>(i), bindT, bindR, bindS);
                    desiredScale = bindS;
                }

                localTransforms[i] = glm::translate(glm::mat4(1.0f), T)
                                   * glm::mat4_cast(glm::normalize(R))
                                   * glm::scale(glm::mat4(1.0f), desiredScale);
            }
        }

        BuildSkeletonLocalPaletteFromLocalTransforms(
            skeleton,
            localTransforms,
            static_cast<uint32_t>(s_AnimationLodFrameCounter),
            hasPostAnimationConstraints);
        posePaletteUpdatedRoots.fetch_add(1, std::memory_order_relaxed);

        if (!shouldWriteBoneEntities) {
            boneEntityWritebackSkippedRoots.fetch_add(1, std::memory_order_relaxed);
            boneEntityWritebackSkippedBones.fetch_add(
                static_cast<uint64_t>(materializedBoneCount),
                std::memory_order_relaxed);
        } else {
            boneEntityWritebackRoots.fetch_add(1, std::memory_order_relaxed);
            boneEntityWritebackBones.fetch_add(
                static_cast<uint64_t>(materializedBoneCount),
                std::memory_order_relaxed);

            // Materialize animated bones back into entities only when a
            // downstream runtime system actually consumes those entities.
            auto decomposeTRS = [](const glm::mat4& m, glm::vec3& T, glm::quat& R, glm::vec3& S){
                T = glm::vec3(m[3]);
                glm::vec3 X = glm::vec3(m[0]);
                glm::vec3 Y = glm::vec3(m[1]);
                glm::vec3 Z = glm::vec3(m[2]);
                S = glm::vec3(glm::length(X), glm::length(Y), glm::length(Z));
                if (S.x > 1e-6f) X /= S.x; if (S.y > 1e-6f) Y /= S.y; if (S.z > 1e-6f) Z /= S.z;
                glm::mat3 rotMat(X, Y, Z);
                R = glm::quat_cast(rotMat);
            };
#ifndef CLAYMORE_RUNTIME
            const bool keepEulerInspectorSync = !scene.m_IsPlaying;
#else
            const bool keepEulerInspectorSync = false;
#endif
            for (size_t i = 0; i < materializedBoneCount; ++i) {
                EntityID boneId = skeleton.BoneEntities[i];
                if (boneId == (EntityID)-1) continue;
                if (auto* bd = scene.GetEntityData(boneId)) {
                    glm::vec3 T, S; glm::quat R;
                    decomposeTRS(localTransforms[i], T, R, S);
                    bd->Transform.Position = T;
                    bd->Transform.Scale    = S;
                    bd->Transform.RotationQ = glm::normalize(R);
                    bd->Transform.UseQuatRotation = true;
                    if (keepEulerInspectorSync) {
                        bd->Transform.Rotation = glm::degrees(glm::eulerAngles(bd->Transform.RotationQ));
                    }
                    scene.MarkTransformDirty(boneId);
                }
            }
        }

        // Keep a pose for rare controller-switch blends without copying the
        // full matrix array every frame. The thread-local scratch receives the
        // previous pose storage and will be reused by the next evaluation.
        player._LastPose.swap(localTransforms);
        player._LastPoseValid = !player._LastPose.empty();
        
        // Clear all triggers at end of frame - triggers are one-frame events
        // If not consumed by a transition this frame, they expire
        // This prevents "queued" attacks when clicking during wrong states
        player.AnimatorInstance.ConsumeTriggers();
    }; // end of evaluateEntity lambda
    
    // Execute animation evaluation (parallel or sequential based on entity count)
    if (useParallel) {
        // Parallel evaluation: each entity processed independently
        const size_t chunkSize = ComputeOptimalChunkSize(
            entityCount,
            cm::g_JobSystem->GetWorkerCount(),
            size_t(4));
        parallel_for(Jobs(), size_t(0), entityCount, chunkSize,
            [&evalRequests, &evaluateEntity](size_t start, size_t count) {
                for (size_t i = start; i < start + count; ++i) {
                    evaluateEntity(i);
                }
            });
    } else {
        // Sequential fallback for single entity or no job system
        for (size_t i = 0; i < evalRequests.size(); ++i) {
            evaluateEntity(i);
        }
    }

    if (scene.m_IsPlaying &&
        posePaletteUpdatedRoots.load(std::memory_order_relaxed) > 0) {
        scene.NotifyAnimationPosePaletteChanged();
    }

    Profiler::Get().SetCounter(
        "Animation/CrowdPoseShareEligible",
        crowdPoseShareEligibleCount.load(std::memory_order_relaxed));
    Profiler::Get().SetCounter(
        "Animation/CrowdPoseShareHits",
        crowdPoseShareHitCount.load(std::memory_order_relaxed));
    Profiler::Get().SetCounter(
        "Animation/CrowdPoseShareLeaders",
        crowdPoseShareLeaderCount.load(std::memory_order_relaxed));
    Profiler::Get().SetCounter(
        "Animation/CrowdPoseShareCacheSize",
        static_cast<uint64_t>(crowdPoseShareCache.Entries.size()));
    Profiler::Get().SetCounter(
        "Animation/BoneEntityWritebackRoots",
        boneEntityWritebackRoots.load(std::memory_order_relaxed));
    Profiler::Get().SetCounter(
        "Animation/BoneEntityWritebackSkippedRoots",
        boneEntityWritebackSkippedRoots.load(std::memory_order_relaxed));
    Profiler::Get().SetCounter(
        "Animation/BoneEntityWritebackBones",
        boneEntityWritebackBones.load(std::memory_order_relaxed));
    Profiler::Get().SetCounter(
        "Animation/BoneEntityWritebackSkippedBones",
        boneEntityWritebackSkippedBones.load(std::memory_order_relaxed));

    if (collectDetailedPrefabPerf) {
        struct PrefabAnimationPerfSample {
            double TotalMs = 0.0;
            uint64_t RootsEvaluated = 0;
            uint64_t BoneCount = 0;
            std::string StateName;
            std::string AnimationName;
        };

        std::unordered_map<EntityID, PrefabAnimationPerfSample> prefabAnimationPerf;
        prefabAnimationPerf.reserve(evalRequests.size());
        for (size_t i = 0; i < evalRequests.size(); ++i) {
            const EntityID animationRootId = evalRequests[i].rootId;
            const EntityID prefabRootId =
                cm::debug::ResolveOwningPrefabRoot(scene, animationRootId);
            if (prefabRootId == INVALID_ENTITY_ID) {
                continue;
            }

            auto& sample = prefabAnimationPerf[prefabRootId];
            sample.TotalMs += evalRootTimes[i];
            ++sample.RootsEvaluated;

            if (EntityData* animationRootData = scene.GetEntityData(animationRootId)) {
                if (animationRootData->Skeleton) {
                    sample.BoneCount += animationRootData->Skeleton->BoneEntities.size();
                }
                if (animationRootData->AnimationPlayer) {
                    if (sample.StateName.empty()) {
                        sample.StateName =
                            animationRootData->AnimationPlayer->Debug_CurrentControllerStateName;
                    }
                    if (sample.AnimationName.empty()) {
                        sample.AnimationName =
                            animationRootData->AnimationPlayer->Debug_CurrentAnimationName;
                    }
                }
            }
        }

        Profiler::Get().SetCounter(
            "Animation/PrefabRootsEvaluated",
            static_cast<uint64_t>(prefabAnimationPerf.size()));
        for (const auto& [prefabRootId, sample] : prefabAnimationPerf) {
            const auto label = cm::debug::DescribePrefabRoot(scene, prefabRootId);
            if (!label.IsValid()) {
                continue;
            }
            Profiler::Get().Record(
                cm::debug::MakePrefabProfilerSection("Animation/Prefab", label),
                sample.TotalMs);
        }

        if (cm::debug::PrefabPerfConsoleLoggingEnabled()) {
            static uint64_t s_PrefabAnimationLogFrame = 0;
            const uint32_t logInterval = cm::debug::PrefabPerfConsoleLogInterval();
            ++s_PrefabAnimationLogFrame;
            if (logInterval > 0 &&
                (s_PrefabAnimationLogFrame % logInterval) == 0u &&
                !prefabAnimationPerf.empty()) {
                std::vector<std::pair<EntityID, const PrefabAnimationPerfSample*>> ordered;
                ordered.reserve(prefabAnimationPerf.size());
                for (const auto& entry : prefabAnimationPerf) {
                    ordered.emplace_back(entry.first, &entry.second);
                }
                std::sort(ordered.begin(), ordered.end(),
                    [](const auto& lhs, const auto& rhs) {
                        return lhs.second->TotalMs > rhs.second->TotalMs;
                    });

                const size_t limit = std::min<size_t>(3, ordered.size());
                std::cout << "[PrefabPerf][Animation] Top prefab roots this frame:" << std::endl;
                for (size_t i = 0; i < limit; ++i) {
                    const EntityID prefabRootId = ordered[i].first;
                    const PrefabAnimationPerfSample& sample = *ordered[i].second;
                    const auto label = cm::debug::DescribePrefabRoot(scene, prefabRootId);
                    std::cout << "[PrefabPerf][Animation]   " << (i + 1) << ". "
                        << cm::debug::MakePrefabDebugLabel(label)
                        << " total=" << sample.TotalMs << "ms"
                        << " animRoots=" << sample.RootsEvaluated
                        << " bones=" << sample.BoneCount;
                    if (!sample.StateName.empty()) {
                        std::cout << " state=" << sample.StateName;
                    }
                    if (!sample.AnimationName.empty()) {
                        std::cout << " clip=" << sample.AnimationName;
                    }
                    std::cout << std::endl;
                }
            }
        }
    } else {
        Profiler::Get().SetCounter("Animation/PrefabRootsEvaluated", 0);
    }
}

} // namespace animation
} // namespace cm
