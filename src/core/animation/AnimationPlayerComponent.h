#pragma once

#include <array>
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "core/animation/AnimationTypes.h"
#include "core/animation/AnimatorRuntime.h"
#include "core/animation/AnimationAsset.h"
#include "core/animation/AvatarMask.h"
#include "core/ecs/Entity.h"

namespace cm {
namespace animation {

struct AnimatorControllerOverrideAsset;

struct AnimationState {
    const AnimationClip* LegacyClip = nullptr;      // legacy
    const AnimationAsset* Asset = nullptr;          // unified
    float Time = 0.0f;                              // Current playback time (seconds)
    float Weight = 1.0f;                            // Blend weight (0..1)
    bool Loop = true;
};

// Note: Animation layering is now defined in the AnimatorController
// (see AnimatorLayer in AnimatorController.h).
// Runtime layer state is tracked in Animator::LayerStates().

/// Determines where root motion is applied when extracted from animation
enum class RootMotionTarget : uint8_t {
    Self = 0,                   ///< Apply to this entity's transform directly (no physics)
    FindCharacterController,    ///< Search hierarchy for CharacterController, inject as velocity
    FindRigidBody,              ///< Search hierarchy for kinematic RigidBody, inject as velocity  
    ExplicitEntity              ///< Use explicit target entity ID
};

/// Runtime output from animation system - consumed by physics integration
struct RootMotionOutput {
    glm::vec3 PositionDelta{0.0f};      ///< Position delta this frame (world space)
    glm::quat RotationDelta{1,0,0,0};   ///< Rotation delta this frame
    bool Valid = false;                  ///< True if delta was computed this frame
    bool OverrideGravity = false;        ///< True if gravity should be disabled this frame

    void Reset() { 
        PositionDelta = glm::vec3(0.0f); 
        RotationDelta = glm::quat(1,0,0,0); 
        Valid = false; 
        OverrideGravity = false;
    }
};

struct AnimationPlayerComponent {
    std::vector<AnimationState> ActiveStates;   // Multiple layers / states
    float PlaybackSpeed = 1.0f;
    
    /// When false, animation evaluation is skipped entirely.
    /// Use this when ragdoll takes over bone transforms.
    /// Bone transforms will still be used for skinning (from ragdoll or last pose).
    bool Enabled = true;
    // Optional: Animator controller
    std::string ControllerPath; // .animctrl JSON file
    std::string ControllerOverridePath; // .animoverride JSON file

    // Runtime controller & animator
    std::shared_ptr<AnimatorController> Controller;
    std::shared_ptr<AnimatorControllerOverrideAsset> ControllerOverride;
    Animator AnimatorInstance;
    int CurrentStateId = -1;
    // Cache of loaded assets per state id
    std::unordered_map<int, std::shared_ptr<AnimationClip>> CachedClips; // legacy clips
    std::unordered_map<int, std::shared_ptr<AnimationAsset>> CachedAssets; // unified assets
    std::unordered_map<int, float> CachedDurations; // resolved asset/clip durations by cache key

    struct CachedHipsReferenceSample {
        bool Valid = false;
        bool Found = false;
        float Y = 0.0f;
        float RangeY = 0.0f;
    };

    // Runtime-only cache for in-place grounding. The key combines the animation
    // asset and target avatar because humanoid projection depends on both.
    std::unordered_map<std::uintptr_t, std::array<CachedHipsReferenceSample, 2>> _HipsReferenceCache;

    // =========================================================================
    // Root Motion Configuration (runtime - target selection)
    // The actual mode (None/InPlace/ApplyToEntity) comes from the AnimationAsset
    // =========================================================================
    
    /// @deprecated Runtime now auto-resolves root-motion driver from self/direct parent physics.
    RootMotionTarget MotionTarget = RootMotionTarget::FindCharacterController;
    
    /// @deprecated Legacy explicit target field kept for backward compatibility.
    EntityID ExplicitTargetEntityId = INVALID_ENTITY_ID;
    
    // =========================================================================
    // Root Motion Runtime State (internal - not serialized)
    // =========================================================================
    
    /// Output delta computed by AnimationSystem, consumed by physics integration
    RootMotionOutput _RootMotionOutput;

    /// Effective root motion mode resolved for the current base-layer clip/blend.
    /// Updated during animation evaluation and queried by managed scripting.
    RootMotionMode _CurrentRootMotionMode = RootMotionMode::None;
    
    /// Previous frame's root bone model-space position (for delta calculation)
    glm::vec3 _PrevRootModelPos{0.0f};
    
    /// Previous frame's root bone model-space rotation (for rotation delta)
    glm::quat _PrevRootModelRot{1,0,0,0};
    
    /// Previous animation time (to detect loops/transitions)
    float _PrevAnimTime = 0.0f;
    
    /// Whether previous frame data is valid (false after state change or first frame)
    bool _PrevRootValid = false;
    
    /// Track which state we were in (to detect transitions and reset tracking)
    int _RootMotionPrevStateId = -1;
    
    /// Cached resolved target entity (to avoid re-searching hierarchy every frame)
    EntityID _ResolvedTargetEntityId = INVALID_ENTITY_ID;
    bool _TargetResolved = false;
    
    // =========================================================================
    // InPlace Y Bounce Tracking
    // =========================================================================
    
    /// Baseline animated Y position (captured on first frame after state change)
    float _InPlaceBaselineY = 0.0f;
    
    /// Scale factor for Y deltas (handles mismatched animation/skeleton scales)
    float _InPlaceScaleFactor = 1.0f;
    
    /// Whether baseline has been captured
    bool _InPlaceBaselineValid = false;
    
    /// Previous state ID for baseline reset detection
    int _InPlacePrevStateId = -1;
    
    /// Previous blend parameter for Blend1D states (reset baseline when blend changes)
    float _InPlacePrevBlendParam = 0.0f;

    /// Last applied local Y for the pose anchor bone (hips/root), used to keep
    /// transition continuity and avoid one-frame baseline pops.
    float _InPlaceLastAppliedY = 0.0f;
    bool _InPlaceLastAppliedYValid = false;

    /// Small continuity correction applied when the grounding reference source
    /// changes (state handoff, blend-tree clip pair change, etc.). This keeps
    /// the model planted while the new state's baseline settles in.
    float _InPlaceContinuityOffsetY = 0.0f;
    uint64_t _InPlaceYSourceSignature = 0;
    bool _InPlaceYSourceSignatureValid = false;

    /// Feet-aware anchor state (runtime only). Helps keep pose grounding stable
    /// across blend transitions while preserving authored crouch compression.
    float _FeetAnchorWeight = 0.0f;
    float _FeetAnchorSmoothedY = 0.0f;
    bool _FeetAnchorSmoothedYValid = false;
    float _PrevLeftFootModelY = 0.0f;
    float _PrevRightFootModelY = 0.0f;
    bool _PrevFeetModelYValid = false;

    // =========================================================================
    // Animation Configuration
    // =========================================================================
    
    /// When no ControllerPath is set, this path is used to auto-generate a simple
    /// single-state controller at runtime. Provides a simple way to play an animation
    /// without needing to author a full controller file.
    std::string SingleClipPath;      ///< Path to a unified .anim (preferred) or legacy clip
    bool PlayOnStart = true;         ///< If true, auto-begin playback on start
    bool CrowdThrottleEnabled = true; ///< Allow crowd rank throttling and pose sharing
    bool LODEnabled = true;          ///< Allow distance/offscreen animation throttling
    float LODNearDistance = 25.0f;
    float LODMediumDistance = 55.0f;
    float LODFarDistance = 110.0f;
    float LODMediumInterval = 0.03333334f;
    float LODFarInterval = 0.06666667f;
    float LODVeryFarInterval = 0.13333334f;
    bool OffscreenDormancyEnabled = true; ///< Allow stable idle animators to freeze offscreen
    float OffscreenNearInterval = 0.20000000f;
    float OffscreenMediumInterval = 0.33333334f;
    float OffscreenFarInterval = 0.50000000f;
    float OffscreenVeryFarInterval = 1.00000000f;
    float LodAccumulatedTime = 0.0f; ///< Runtime-only animation LOD accumulator
    float LodLastDistance = 0.0f;    ///< Runtime-only last camera distance used by LOD
    bool IsPlaying = false;          ///< Runtime playing flag (for Play/Stop API)
    bool _InitApplied = false;       ///< Internal guard to apply PlayOnStart once
    bool _AutoControllerGenerated = false; ///< True if controller was auto-generated from SingleClipPath

    /// Exposed runtime info for UI and scripting
    std::string Debug_CurrentAnimationName;      ///< clip/asset name currently bound
    std::string Debug_CurrentControllerStateName; ///< controller state name when in controller mode

    // Runtime-only dormancy bit set by AnimationSystem when an offscreen
    // animator is frozen on a stable idle pose. Skinning can use this to
    // skip palette rebuilds until the actor becomes relevant again.
    bool DormantOffscreenIdle = false;
    
    // =========================================================================
    // Script Event Callbacks (for managed interop)
    // =========================================================================
    
    /// Registered event listeners: maps event name to managed callback handle (GCHandle)
    /// When an animation fires an event with a matching name, the callback is invoked
    std::unordered_map<std::string, void*> _EventCallbacks;
    
    /// Tracks which script event IDs have been fired in the current animation cycle (base layer)
    /// Reset when animation loops or state changes
    std::unordered_set<int> _FiredEventIds;
    
    /// Previous state time/ID for loop detection (to reset fired events tracking)
    float _PrevEventStateTime = 0.0f;
    int _PrevEventStateId = -1;
    
    // =========================================================================
    // Helper Methods
    // =========================================================================
    
    /// Reset root motion tracking state (call on state transitions)
    void ResetRootMotionTracking() {
        _PrevRootValid = false;
        _RootMotionOutput.Reset();
        _InPlaceBaselineValid = false;
        _InPlaceLastAppliedYValid = false;
        _InPlaceContinuityOffsetY = 0.0f;
        _InPlaceYSourceSignature = 0;
        _InPlaceYSourceSignatureValid = false;
        _FeetAnchorWeight = 0.0f;
        _FeetAnchorSmoothedYValid = false;
        _PrevFeetModelYValid = false;
    }
    
    /// Invalidate cached target (call when target config changes)
    void InvalidateTargetCache() {
        _TargetResolved = false;
        _ResolvedTargetEntityId = INVALID_ENTITY_ID;
    }

    int ResolveBaseDefaultStateId() const {
        if (!Controller) return -1;
        if (const auto* baseLayer = Controller->GetLayer(0)) {
            return baseLayer->DefaultState;
        }
        return Controller->DefaultState;
    }

    bool NeedsRuntimeControllerSync() const {
        if (AnimatorInstance.GetController().get() != Controller.get()) {
            return true;
        }
        if (!Controller) {
            return !AnimatorInstance.LayerStates().empty() || CurrentStateId != -1;
        }

        const size_t expectedLayerCount = Controller->HasLayers()
            ? Controller->Layers.size()
            : size_t(1);
        if (AnimatorInstance.LayerStates().size() != expectedLayerCount) {
            return true;
        }
        return expectedLayerCount > 0 && AnimatorInstance.GetLayerState(0) == nullptr;
    }

    void SyncRuntimeControllerState() {
        AnimatorInstance.SetController(Controller);
        AnimatorInstance.ResetToDefaults();
        AnimatorInstance.InitializeLayerStates();

        if (!Controller) {
            CurrentStateId = -1;
            return;
        }

        CurrentStateId = ResolveBaseDefaultStateId();
        if (const auto* baseLayerState = AnimatorInstance.GetLayerState(0)) {
            CurrentStateId = baseLayerState->CurrentStateId;
        }
    }
    
    // =========================================================================
    // PERF: Animation Preloading
    // =========================================================================
    
    /// Whether animations have been preloaded for this player
    bool _AnimationsPreloaded = false;
    uint64_t _CrowdPoseControllerSignature = 0;
    size_t _CrowdPoseControllerPathHash = 0;
    size_t _CrowdPoseControllerOverridePathHash = 0;
    size_t _CrowdPoseSingleClipPathHash = 0;
    const void* _CrowdPoseControllerPtr = nullptr;
    const void* _CrowdPoseControllerOverridePtr = nullptr;
    bool _CrowdPoseControllerSignatureValid = false;
    
    /// Preload all animation assets referenced by the controller.
    /// Call this after setting ControllerPath to avoid hitching on state transitions.
    /// Safe to call multiple times - will only load once until controller changes.
    /// @param allowParallel When true and a job system is available, missing assets are requested in the background.
    /// @return True once every referenced animation has been loaded or resolved from cache.
    bool PreloadAllControllerAssets(bool allowParallel = false);
    
    /// Force reload of all cached assets (call after controller file changes)
    void InvalidateAssetCache() {
        CachedClips.clear();
        CachedAssets.clear();
        CachedDurations.clear();
        _HipsReferenceCache.clear();
        _AnimationsPreloaded = false;
        _CrowdPoseControllerSignatureValid = false;
        for (auto& state : ActiveStates) {
            state.Asset = nullptr;
            state.LegacyClip = nullptr;
            state.Time = 0.0f;
        }
        Debug_CurrentAnimationName.clear();
        Debug_CurrentControllerStateName.clear();
    }

    /// Clear cached animation assets while preserving the current runtime state/time.
    /// Used when a controller override swaps sampled clips without changing the state machine.
    void InvalidateResolvedAssetCache() {
        for (auto& state : ActiveStates) {
            state.Asset = nullptr;
            state.LegacyClip = nullptr;
        }
        CachedClips.clear();
        CachedAssets.clear();
        CachedDurations.clear();
        _HipsReferenceCache.clear();
        _AnimationsPreloaded = false;
        _CrowdPoseControllerSignatureValid = false;
        Debug_CurrentAnimationName.clear();
        Debug_CurrentControllerStateName.clear();
    }

    uint64_t GetCrowdPoseControllerSignature() {
        const size_t controllerPathHash = std::hash<std::string>{}(ControllerPath);
        const size_t controllerOverridePathHash = std::hash<std::string>{}(ControllerOverridePath);
        const size_t singleClipPathHash = std::hash<std::string>{}(SingleClipPath);
        const void* controllerPtr = Controller.get();
        const void* controllerOverridePtr = ControllerOverride.get();
        if (_CrowdPoseControllerSignatureValid &&
            _CrowdPoseControllerPtr == controllerPtr &&
            _CrowdPoseControllerOverridePtr == controllerOverridePtr &&
            _CrowdPoseControllerPathHash == controllerPathHash &&
            _CrowdPoseControllerOverridePathHash == controllerOverridePathHash &&
            _CrowdPoseSingleClipPathHash == singleClipPathHash) {
            return _CrowdPoseControllerSignature;
        }

        auto hashCombine = [](uint64_t seed, uint64_t value) {
            seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
            return seed;
        };

        uint64_t signature = 14695981039346656037ULL;
        signature = hashCombine(signature, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(controllerPtr)));
        signature = hashCombine(signature, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(controllerOverridePtr)));
        signature = hashCombine(signature, static_cast<uint64_t>(controllerPathHash));
        signature = hashCombine(signature, static_cast<uint64_t>(controllerOverridePathHash));
        signature = hashCombine(signature, static_cast<uint64_t>(singleClipPathHash));

        _CrowdPoseControllerSignature = signature;
        _CrowdPoseControllerPathHash = controllerPathHash;
        _CrowdPoseControllerOverridePathHash = controllerOverridePathHash;
        _CrowdPoseSingleClipPathHash = singleClipPathHash;
        _CrowdPoseControllerPtr = controllerPtr;
        _CrowdPoseControllerOverridePtr = controllerOverridePtr;
        _CrowdPoseControllerSignatureValid = true;
        return _CrowdPoseControllerSignature;
    }

    // =========================================================================
    // Controller Switch Blending (runtime-only)
    // =========================================================================
    std::vector<glm::mat4> _LastPose;
    bool _LastPoseValid = false;
    std::vector<glm::mat4> _ControllerSwitchPose;
    float _ControllerSwitchBlendTime = 0.0f;
    float _ControllerSwitchBlendDuration = 0.0f;
    bool _ControllerSwitchBlendActive = false;

    void BeginControllerSwitchBlend(float durationSeconds) {
        _ControllerSwitchBlendActive = false;
        if (durationSeconds <= 0.0f) return;
        if (!_LastPoseValid || _LastPose.empty()) return;
        _ControllerSwitchPose = _LastPose;
        _ControllerSwitchBlendTime = 0.0f;
        _ControllerSwitchBlendDuration = durationSeconds;
        _ControllerSwitchBlendActive = true;
    }
};

} // namespace animation
} // namespace cm
