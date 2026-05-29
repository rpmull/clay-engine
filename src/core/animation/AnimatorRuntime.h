#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <vector>
#include "core/animation/AnimatorController.h"
#include "core/animation/AnimationTypes.h"
#include "core/animation/AvatarMask.h"

namespace cm {
namespace animation {

struct AnimatorBlackboard {
    std::unordered_map<std::string, bool> Bools;
    std::unordered_map<std::string, int> Ints;
    std::unordered_map<std::string, float> Floats;
    std::unordered_map<std::string, bool> Triggers; // consumed when read
    std::vector<uint8_t> BoolValues;
    std::vector<int> IntValues;
    std::vector<float> FloatValues;
    std::vector<uint8_t> TriggerValues;

    void ResetDenseBuffers(const AnimatorController* controller) {
        const size_t boolCount = controller ? controller->RuntimeBoolCount : 0u;
        const size_t intCount = controller ? controller->RuntimeIntCount : 0u;
        const size_t floatCount = controller ? controller->RuntimeFloatCount : 0u;
        const size_t triggerCount = controller ? controller->RuntimeTriggerCount : 0u;
        BoolValues.assign(boolCount, uint8_t{0u});
        IntValues.assign(intCount, 0);
        FloatValues.assign(floatCount, 0.0f);
        TriggerValues.assign(triggerCount, uint8_t{0u});
    }

    bool GetBoolSlot(int slot) const {
        return slot >= 0 && static_cast<size_t>(slot) < BoolValues.size() && BoolValues[static_cast<size_t>(slot)] != 0u;
    }
    int GetIntSlot(int slot) const {
        return slot >= 0 && static_cast<size_t>(slot) < IntValues.size() ? IntValues[static_cast<size_t>(slot)] : 0;
    }
    float GetFloatSlot(int slot) const {
        return slot >= 0 && static_cast<size_t>(slot) < FloatValues.size() ? FloatValues[static_cast<size_t>(slot)] : 0.0f;
    }
    bool GetTriggerSlot(int slot) const {
        return slot >= 0 && static_cast<size_t>(slot) < TriggerValues.size() && TriggerValues[static_cast<size_t>(slot)] != 0u;
    }
    void SetBoolSlot(int slot, bool value) {
        if (slot >= 0 && static_cast<size_t>(slot) < BoolValues.size()) {
            BoolValues[static_cast<size_t>(slot)] = value ? uint8_t{1u} : uint8_t{0u};
        }
    }
    void SetIntSlot(int slot, int value) {
        if (slot >= 0 && static_cast<size_t>(slot) < IntValues.size()) {
            IntValues[static_cast<size_t>(slot)] = value;
        }
    }
    void SetFloatSlot(int slot, float value) {
        if (slot >= 0 && static_cast<size_t>(slot) < FloatValues.size()) {
            FloatValues[static_cast<size_t>(slot)] = value;
        }
    }
    void SetTriggerSlot(int slot, bool value) {
        if (slot >= 0 && static_cast<size_t>(slot) < TriggerValues.size()) {
            TriggerValues[static_cast<size_t>(slot)] = value ? uint8_t{1u} : uint8_t{0u};
        }
    }
};

struct AnimatorPlayback {
    int CurrentStateId = -1;
    float StateTime = 0.0f; // seconds
    float StateNormalized = 0.0f; // 0..1 (cached)
    float PrevStateNormalized = 0.0f; // previous frame's normalized time (for wrap detection)
    bool HasCompletedOneCycle = false; // true if StateTime exceeded animation duration at least once
    int NextStateId = -1; // for cross-fade
    float CrossfadeTime = 0.0f;
    float CrossfadeDuration = 0.0f; // seconds; 0 means no crossfade
    float NextStateTime = 0.0f; // seconds accumulator for next state during crossfade
};

// =========================================================================
// Per-Layer Runtime State
// =========================================================================

/// Runtime state for a single animation layer.
/// Stores playback state, weight, and cached references for the layer's current animation.
struct AnimatorLayerState {
    int LayerIndex = 0;                         ///< Index of the layer in the controller
    
    // FSM playback state (mirrors AnimatorPlayback but per-layer)
    int CurrentStateId = -1;
    float StateTime = 0.0f;
    float StateNormalized = 0.0f;
    float PrevStateNormalized = 0.0f;
    bool HasCompletedOneCycle = false;
    
    // Crossfade state (per-layer)
    int NextStateId = -1;
    float CrossfadeTime = 0.0f;
    float CrossfadeDuration = 0.0f;
    float NextStateTime = 0.0f;
    
    // Layer blending (runtime weight can differ from default)
    float Weight = 1.0f;                        ///< Current blend weight (0 = inactive, 1 = full)
    float TargetWeight = 1.0f;                  ///< Weight to blend towards
    float BlendSpeed = 8.0f;                    ///< Weight transition speed (units per second)
    
    // Resolved mask (cached from controller)
    AvatarMask Mask;
    AnimatorLayerBlendMode BlendMode = AnimatorLayerBlendMode::Override;
    
    // Script event tracking (to prevent duplicate firing within same animation cycle)
    std::unordered_set<int> _FiredEventIds;
    float _PrevEventStateTime = 0.0f;
    int _PrevEventStateId = -1;
    
    // Helpers
    bool IsCrossfading() const { 
        return CrossfadeDuration > 0.0f && CrossfadeTime < CrossfadeDuration; 
    }
    float CrossfadeAlpha() const { 
        return CrossfadeDuration > 0.0f ? glm::clamp(CrossfadeTime / CrossfadeDuration, 0.0f, 1.0f) : 1.0f; 
    }
    bool IsActive() const { return Weight > 0.001f || TargetWeight > 0.001f; }
    
    void BeginCrossfade(int toStateId, float durationSeconds) {
        NextStateId = toStateId;
        CrossfadeTime = 0.0f;
        CrossfadeDuration = durationSeconds;
        NextStateTime = 0.0f;
    }
    
    void AdvanceCrossfade(float deltaSeconds) {
        if (CrossfadeDuration <= 0.0f) return;
        CrossfadeTime += deltaSeconds;
        NextStateTime += deltaSeconds;
        if (CrossfadeTime >= CrossfadeDuration) {
            CrossfadeTime = CrossfadeDuration;
        }
    }
    
    void SetCurrentState(int stateId, bool resetTime) {
        CurrentStateId = stateId;
        if (resetTime) {
            StateTime = 0.0f;
            StateNormalized = 0.0f;
            PrevStateNormalized = 0.0f;
            HasCompletedOneCycle = false;
        }
        CrossfadeTime = 0.0f;
        CrossfadeDuration = 0.0f;
        NextStateId = -1;
        NextStateTime = 0.0f;
    }
    
    void SetStateTime(float time, float clipDuration = 0.0f) {
        StateTime = time;
        if (clipDuration > 0.0f) {
            StateNormalized = fmod(time, clipDuration) / clipDuration;
            HasCompletedOneCycle = (time >= clipDuration);
        }
        PrevStateNormalized = StateNormalized;
        CrossfadeTime = 0.0f;
        CrossfadeDuration = 0.0f;
        NextStateId = -1;
        NextStateTime = 0.0f;
    }
};

class Animator {
public:
    Animator() = default;

    void SetController(std::shared_ptr<AnimatorController> controller) {
        m_Controller = std::move(controller);
        if (!m_Controller) {
            m_Blackboard = {};
            m_Playback = {};
            m_LayerStates.clear();
            return;
        }
        m_Controller->CompileRuntimeData();
    }
    std::shared_ptr<AnimatorController> GetController() const { return m_Controller; }

    AnimatorBlackboard& Blackboard() { return m_Blackboard; }
    const AnimatorBlackboard& Blackboard() const { return m_Blackboard; }
    const AnimatorPlayback& Playback() const { return m_Playback; }

    void ResetToDefaults();
    void Update(float deltaTime, float clipDuration);
    int ChooseNextState() const; // returns -1 if none
    // Returns the specific transition that would be chosen right now to reach 'toStateId',
    // or nullptr if none. Mirrors the preference logic in ChooseNextState.
    const AnimatorTransition* FindTransitionTo(int toStateId) const;
    void ConsumeTriggers();
    /// Consume only the triggers used by a specific transition (for per-layer independence)
    void ConsumeTriggersForTransition(const AnimatorTransition* trans);
    // Consume triggers for AnyState transitions that would target the current state (blocked by CanTransitionToSelf=false)
    void ConsumeSelfTransitionTriggers();

    // Explicitly set the current state used for transition evaluation and timing.
    // Optionally reset time accumulators so the new state's playback starts at t=0.
    void SetCurrentState(int stateId, bool resetTime);
    
    // Set the state time directly (used when preserving time after crossfade completes)
    // Also requires clipDuration to recompute normalized time immediately
    void SetStateTime(float time, float clipDuration = 0.0f) { 
        m_Playback.StateTime = time; 
        // Recompute normalized time immediately so exit time checks work on this frame
        if (clipDuration > 0.0f) {
            m_Playback.StateNormalized = fmod(time, clipDuration) / clipDuration;
            // Track if we've already completed at least one cycle based on preserved time
            m_Playback.HasCompletedOneCycle = (time >= clipDuration);
        }
        m_Playback.PrevStateNormalized = m_Playback.StateNormalized;
        // Clear crossfade state since we're now fully in the current state
        m_Playback.CrossfadeTime = 0.0f;
        m_Playback.CrossfadeDuration = 0.0f;
        m_Playback.NextStateId = -1;
        m_Playback.NextStateTime = 0.0f;
    }

    // Crossfade control (MVP): call when a transition with duration is selected
    void BeginCrossfade(int toStateId, float durationSeconds);
    bool IsCrossfading() const { return m_Playback.CrossfadeDuration > 0.0f && m_Playback.CrossfadeTime < m_Playback.CrossfadeDuration; }
    float CrossfadeAlpha() const { return m_Playback.CrossfadeDuration > 0.0f ? glm::clamp(m_Playback.CrossfadeTime / m_Playback.CrossfadeDuration, 0.0f, 1.0f) : 1.0f; }

    // Advance internal crossfade timers (used by systems to tick without touching privates)
    void AdvanceCrossfade(float deltaSeconds) {
        if (m_Playback.CrossfadeDuration <= 0.0f) return;
        m_Playback.CrossfadeTime += deltaSeconds;
        m_Playback.NextStateTime += deltaSeconds;
        if (m_Playback.CrossfadeTime >= m_Playback.CrossfadeDuration) {
            m_Playback.CrossfadeTime = m_Playback.CrossfadeDuration;
        }
    }
    
    // =========================================================================
    // Layer-Aware Methods
    // =========================================================================
    
    /// Get runtime state for a layer by index
    AnimatorLayerState* GetLayerState(int layerIndex) {
        for (auto& ls : m_LayerStates) {
            if (ls.LayerIndex == layerIndex) return &ls;
        }
        return nullptr;
    }
    const AnimatorLayerState* GetLayerState(int layerIndex) const {
        for (const auto& ls : m_LayerStates) {
            if (ls.LayerIndex == layerIndex) return &ls;
        }
        return nullptr;
    }
    
    /// Get or create layer state for a layer index
    AnimatorLayerState& GetOrCreateLayerState(int layerIndex);
    
    /// Get all layer states
    std::vector<AnimatorLayerState>& LayerStates() { return m_LayerStates; }
    const std::vector<AnimatorLayerState>& LayerStates() const { return m_LayerStates; }
    
    /// Initialize layer states from controller (call after SetController)
    void InitializeLayerStates();
    
    /// Update a specific layer's FSM
    /// @param layerIndex Index of the layer to update
    /// @param deltaTime Delta time in seconds
    /// @param clipDuration Duration of current animation clip for normalized time calculation
    void UpdateLayer(int layerIndex, float deltaTime, float clipDuration);
    
    /// Choose next state for a specific layer
    /// @param layerIndex Index of the layer
    /// @return State ID of next state, or -1 if no transition
    int ChooseNextStateForLayer(int layerIndex) const;
    
    /// Find transition for a specific layer
    const AnimatorTransition* FindTransitionToForLayer(int layerIndex, int toStateId) const;
    
    /// Set layer weight (runtime override)
    void SetLayerWeight(int layerIndex, float weight);
    void SetLayerWeight(const std::string& layerName, float weight);
    
    /// Blend layer weight towards target over time
    void BlendLayerWeight(int layerIndex, float targetWeight, float blendSpeed);
    void BlendLayerWeight(const std::string& layerName, float targetWeight, float blendSpeed);
    
    /// Get layer weight
    float GetLayerWeight(int layerIndex) const;
    float GetLayerWeight(const std::string& layerName) const;
    void SetBool(const std::string& name, bool value);
    void SetInt(const std::string& name, int value);
    void SetFloat(const std::string& name, float value);
    void SetTrigger(const std::string& name);
    void ResetTrigger(const std::string& name);
    float GetFloat(const std::string& name, float defaultValue = 0.0f) const;
    float GetFloatSlot(int slot, float defaultValue = 0.0f) const;

private:
    std::shared_ptr<AnimatorController> m_Controller;
    AnimatorBlackboard m_Blackboard;
    AnimatorPlayback m_Playback;                    ///< Legacy: base layer playback (layer 0)
    std::vector<AnimatorLayerState> m_LayerStates;  ///< Per-layer runtime states
};

} // namespace animation
} // namespace cm


