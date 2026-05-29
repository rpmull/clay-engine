#include "core/animation/AnimatorRuntime.h"
#include "core/animation/AnimatorController.h"
#include "core/animation/AnimationSerializer.h"
#include "core/animation/AnimationAsset.h"

namespace cm {
namespace animation {

void Animator::ResetToDefaults() {
    m_Blackboard = AnimatorBlackboard{};
    if (!m_Controller) { m_Playback = {}; return; }
    m_Controller->CompileRuntimeData();
    m_Blackboard.ResetDenseBuffers(m_Controller.get());
    for (const auto& p : m_Controller->Parameters) {
        switch (p.Type) {
            case AnimatorParamType::Bool:
                m_Blackboard.Bools[p.Name] = p.DefaultBool;
                m_Blackboard.SetBoolSlot(p.RuntimeSlot, p.DefaultBool);
                break;
            case AnimatorParamType::Int:
                m_Blackboard.Ints[p.Name] = p.DefaultInt;
                m_Blackboard.SetIntSlot(p.RuntimeSlot, p.DefaultInt);
                break;
            case AnimatorParamType::Float:
                m_Blackboard.Floats[p.Name] = p.DefaultFloat;
                m_Blackboard.SetFloatSlot(p.RuntimeSlot, p.DefaultFloat);
                break;
            case AnimatorParamType::Trigger:
                m_Blackboard.Triggers[p.Name] = false;
                m_Blackboard.SetTriggerSlot(p.RuntimeSlot, false);
                break;
        }
    }
    m_Playback = {};
    if (const AnimatorLayer* baseLayer = m_Controller->GetLayer(0)) {
        m_Playback.CurrentStateId = baseLayer->DefaultState;
    } else {
        m_Playback.CurrentStateId = m_Controller->DefaultState;
    }
    m_Playback.StateTime = 0.0f;
}

static bool EvaluateCondition(const AnimatorCondition& c, const AnimatorBlackboard& bb) {
    if (c.RuntimeParameterSlot >= 0) {
        switch (c.Mode) {
            case ConditionMode::If:
                if (c.RuntimeParameterType == AnimatorParamType::Bool) return bb.GetBoolSlot(c.RuntimeParameterSlot);
                if (c.RuntimeParameterType == AnimatorParamType::Trigger) return bb.GetTriggerSlot(c.RuntimeParameterSlot);
                break;
            case ConditionMode::IfNot:
                if (c.RuntimeParameterType == AnimatorParamType::Bool) return !bb.GetBoolSlot(c.RuntimeParameterSlot);
                if (c.RuntimeParameterType == AnimatorParamType::Trigger) return !bb.GetTriggerSlot(c.RuntimeParameterSlot);
                break;
            case ConditionMode::Greater:
                if (c.RuntimeParameterType == AnimatorParamType::Float) return bb.GetFloatSlot(c.RuntimeParameterSlot) > c.Threshold;
                if (c.RuntimeParameterType == AnimatorParamType::Int) return bb.GetIntSlot(c.RuntimeParameterSlot) > c.IntThreshold;
                if (c.RuntimeParameterType == AnimatorParamType::Bool) return bb.GetBoolSlot(c.RuntimeParameterSlot) && c.Threshold == 0.0f;
                break;
            case ConditionMode::Less:
                if (c.RuntimeParameterType == AnimatorParamType::Float) return bb.GetFloatSlot(c.RuntimeParameterSlot) < c.Threshold;
                if (c.RuntimeParameterType == AnimatorParamType::Int) return bb.GetIntSlot(c.RuntimeParameterSlot) < c.IntThreshold;
                if (c.RuntimeParameterType == AnimatorParamType::Bool) return !bb.GetBoolSlot(c.RuntimeParameterSlot) && c.Threshold > 0.0f;
                break;
            case ConditionMode::Equals:
                if (c.RuntimeParameterType == AnimatorParamType::Float) return bb.GetFloatSlot(c.RuntimeParameterSlot) == c.Threshold;
                if (c.RuntimeParameterType == AnimatorParamType::Int) return bb.GetIntSlot(c.RuntimeParameterSlot) == c.IntThreshold;
                break;
            case ConditionMode::NotEquals:
                if (c.RuntimeParameterType == AnimatorParamType::Float) return bb.GetFloatSlot(c.RuntimeParameterSlot) != c.Threshold;
                if (c.RuntimeParameterType == AnimatorParamType::Int) return bb.GetIntSlot(c.RuntimeParameterSlot) != c.IntThreshold;
                break;
            case ConditionMode::Trigger:
                if (c.RuntimeParameterType == AnimatorParamType::Trigger) return bb.GetTriggerSlot(c.RuntimeParameterSlot);
                break;
        }
    }

    switch (c.Mode) {
        case ConditionMode::If: {
            auto it = bb.Bools.find(c.Parameter); return it != bb.Bools.end() && it->second;
        }
        case ConditionMode::IfNot: {
            auto it = bb.Bools.find(c.Parameter); return it != bb.Bools.end() && !it->second;
        }
        case ConditionMode::Greater: {
            auto it = bb.Floats.find(c.Parameter); if (it != bb.Floats.end()) return it->second > c.Threshold;
            auto iti = bb.Ints.find(c.Parameter); if (iti != bb.Ints.end()) return iti->second > c.IntThreshold;
            // Also support bool comparison: true > false (i.e., true > 0 threshold means bool must be true)
            auto itb = bb.Bools.find(c.Parameter); if (itb != bb.Bools.end()) return itb->second && c.Threshold == 0.0f;
            return false;
        }
        case ConditionMode::Less: {
            auto it = bb.Floats.find(c.Parameter); if (it != bb.Floats.end()) return it->second < c.Threshold;
            auto iti = bb.Ints.find(c.Parameter); if (iti != bb.Ints.end()) return iti->second < c.IntThreshold;
            // Also support bool comparison: false < true (i.e., less than non-zero threshold means bool must be false)
            auto itb = bb.Bools.find(c.Parameter); if (itb != bb.Bools.end()) return !itb->second && c.Threshold > 0.0f;
            return false;
        }
        case ConditionMode::Equals: {
            auto it = bb.Floats.find(c.Parameter); if (it != bb.Floats.end()) return it->second == c.Threshold;
            auto iti = bb.Ints.find(c.Parameter); if (iti != bb.Ints.end()) return iti->second == c.IntThreshold;
            return false;
        }
        case ConditionMode::NotEquals: {
            auto it = bb.Floats.find(c.Parameter); if (it != bb.Floats.end()) return it->second != c.Threshold;
            auto iti = bb.Ints.find(c.Parameter); if (iti != bb.Ints.end()) return iti->second != c.IntThreshold;
            return false;
        }
        case ConditionMode::Trigger: {
            auto it = bb.Triggers.find(c.Parameter); return it != bb.Triggers.end() && it->second;
        }
    }
    return false;
}

int Animator::ChooseNextState() const {
    if (!m_Controller) return -1;
    
    // Use base layer (layer 0) transitions - this is the standard path
    // The legacy m_Controller->Transitions is no longer used; all transitions are in layers
    if (m_Controller->Layers.empty()) return -1;
    const auto& transitions = m_Controller->Layers[0].Transitions;
    
    const int current = m_Playback.CurrentStateId;

    auto transitionMatches = [&](const AnimatorTransition& t) -> bool {
        bool ok = true;
        for (const auto& cond : t.Conditions) {
            if (!EvaluateCondition(cond, m_Blackboard)) { ok = false; break; }
        }
        if (!ok) return false;
        if (t.HasExitTime) {
            float epsilon = (t.ExitTime >= 0.95f) ? 0.05f : 0.02f;
            bool wrapped = m_Playback.PrevStateNormalized > m_Playback.StateNormalized && 
                          m_Playback.HasCompletedOneCycle;
            if (wrapped && t.ExitTime >= 0.5f) {
                return true;
            }
            if (m_Playback.StateNormalized + epsilon < t.ExitTime) return false;
        }
        return true;
    };

    // Pass 1: Prefer transitions authored from the current state
    for (const auto& t : transitions) {
        if (t.FromState == current && transitionMatches(t)) return t.ToState;
    }
    // Pass 2: Fallback to AnyState transitions
    // Note: Skip AnyState transitions that target the current state (CanTransitionToSelf = false by default)
    for (const auto& t : transitions) {
        if (t.FromState == -1 && t.ToState != current && transitionMatches(t)) return t.ToState;
    }
    return -1;
}

const AnimatorTransition* Animator::FindTransitionTo(int toStateId) const {
    if (!m_Controller) return nullptr;
    if (m_Controller->Layers.empty()) return nullptr;
    const auto& transitions = m_Controller->Layers[0].Transitions;
    
    const int current = m_Playback.CurrentStateId;
    auto transitionMatches = [&](const AnimatorTransition& t) -> bool {
        if (t.ToState != toStateId) return false;
        bool ok = true;
        for (const auto& cond : t.Conditions) {
            if (!EvaluateCondition(cond, m_Blackboard)) { ok = false; break; }
        }
        if (!ok) return false;
        if (t.HasExitTime) {
            float epsilon = (t.ExitTime >= 0.95f) ? 0.05f : 0.02f;
            bool wrapped = m_Playback.PrevStateNormalized > m_Playback.StateNormalized && 
                          m_Playback.HasCompletedOneCycle;
            if (wrapped && t.ExitTime >= 0.5f) {
                return true;
            }
            if (m_Playback.StateNormalized + epsilon < t.ExitTime) return false;
        }
        return true;
    };
    for (const auto& t : transitions) {
        if (t.FromState == current && transitionMatches(t)) return &t;
    }
    for (const auto& t : transitions) {
        if (t.FromState == -1 && transitionMatches(t)) return &t;
    }
    return nullptr;
}

void Animator::ConsumeTriggers() {
    for (auto& value : m_Blackboard.TriggerValues) value = 0u;
    for (auto& kv : m_Blackboard.Triggers) kv.second = false;
}

void Animator::ConsumeTriggersForTransition(const AnimatorTransition* trans) {
    if (!trans) return;
    // Only consume triggers that are actually used by this transition's conditions
    for (const auto& cond : trans->Conditions) {
        if (cond.Mode == ConditionMode::Trigger) {
            m_Blackboard.SetTriggerSlot(cond.RuntimeParameterSlot, false);
            auto it = m_Blackboard.Triggers.find(cond.Parameter);
            if (it != m_Blackboard.Triggers.end()) {
                it->second = false;
            }
        }
    }
}

void Animator::ConsumeSelfTransitionTriggers() {
    if (!m_Controller) return;
    if (m_Controller->Layers.empty()) return;
    const auto& transitions = m_Controller->Layers[0].Transitions;
    
    const int current = m_Playback.CurrentStateId;
    
    // Find AnyState transitions that target the current state and have trigger conditions
    // Consume those triggers since they would have fired but were blocked by CanTransitionToSelf=false
    for (const auto& t : transitions) {
        if (t.FromState != -1) continue; // Only AnyState transitions
        if (t.ToState != current) continue; // Only self-targeting transitions
        
        // Check if this transition has active trigger conditions
        for (const auto& cond : t.Conditions) {
            if (cond.Mode == ConditionMode::Trigger) {
                if (cond.RuntimeParameterSlot >= 0 && m_Blackboard.GetTriggerSlot(cond.RuntimeParameterSlot)) {
                    m_Blackboard.SetTriggerSlot(cond.RuntimeParameterSlot, false);
                }
                auto it = m_Blackboard.Triggers.find(cond.Parameter);
                if (it != m_Blackboard.Triggers.end() && it->second) {
                    it->second = false; // Consume the trigger
                }
            }
        }
    }
}

void Animator::Update(float deltaTime, float clipDuration) {
    if (!m_Controller) return;
    if (m_Playback.CurrentStateId < 0) {
        // Use default state from base layer
        m_Playback.CurrentStateId = m_Controller->Layers.empty() ? -1 : m_Controller->Layers[0].DefaultState;
    }
    
    // Track previous normalized time for wrap detection
    m_Playback.PrevStateNormalized = m_Playback.StateNormalized;
    
    m_Playback.StateTime += deltaTime;
    if (clipDuration > 0.0f) {
        // Detect if we've completed at least one full cycle
        if (m_Playback.StateTime >= clipDuration) {
            m_Playback.HasCompletedOneCycle = true;
        }
        m_Playback.StateNormalized = fmod(m_Playback.StateTime, clipDuration) / clipDuration;
    }
    else {
        m_Playback.StateNormalized = 0.0f;
    }
}

void Animator::BeginCrossfade(int toStateId, float durationSeconds)
{
    m_Playback.NextStateId = toStateId;
    m_Playback.CrossfadeDuration = std::max(0.0f, durationSeconds);
    m_Playback.CrossfadeTime = 0.0f;
    m_Playback.NextStateTime = 0.0f;
}

void Animator::SetCurrentState(int stateId, bool resetTime)
{
    m_Playback.CurrentStateId = stateId;
    // Reset cycle tracking when changing states
    m_Playback.HasCompletedOneCycle = false;
    m_Playback.PrevStateNormalized = 0.0f;
    if (resetTime) {
        m_Playback.StateTime = 0.0f;
        m_Playback.StateNormalized = 0.0f;
    }
}

// =========================================================================
// Layer-Aware Methods
// =========================================================================

AnimatorLayerState& Animator::GetOrCreateLayerState(int layerIndex) {
    for (auto& ls : m_LayerStates) {
        if (ls.LayerIndex == layerIndex) return ls;
    }
    // Create new layer state
    AnimatorLayerState ls;
    ls.LayerIndex = layerIndex;
    
    // Initialize from controller if available
    if (m_Controller) {
        const AnimatorLayer* layer = m_Controller->GetLayer(layerIndex);
        if (layer) {
            ls.CurrentStateId = layer->DefaultState;
            ls.Weight = layer->DefaultWeight;
            ls.TargetWeight = layer->DefaultWeight;
            ls.Mask.ApplyPreset(layer->MaskPreset);
            ls.BlendMode = layer->BlendMode;
        }
    }
    
    m_LayerStates.push_back(ls);
    return m_LayerStates.back();
}

void Animator::InitializeLayerStates() {
    m_LayerStates.clear();
    if (!m_Controller) return;
    
    // If controller uses new layer architecture
    if (m_Controller->HasLayers()) {
        for (const auto& layer : m_Controller->Layers) {
            AnimatorLayerState ls;
            ls.LayerIndex = layer.Index;
            ls.CurrentStateId = layer.DefaultState;
            ls.StateTime = 0.0f;
            ls.Weight = layer.DefaultWeight;
            ls.TargetWeight = layer.DefaultWeight;
            ls.Mask.ApplyPreset(layer.MaskPreset);
            ls.BlendMode = layer.BlendMode;
            m_LayerStates.push_back(ls);
        }
    } else {
        // Legacy: create single base layer state from flat structure
        AnimatorLayerState ls;
        ls.LayerIndex = 0;
        ls.CurrentStateId = m_Controller->DefaultState;
        ls.StateTime = 0.0f;
        ls.Weight = 1.0f;
        ls.TargetWeight = 1.0f;
        ls.Mask = AvatarMask::FullBody();
        ls.BlendMode = AnimatorLayerBlendMode::Override;
        m_LayerStates.push_back(ls);
    }
}

void Animator::UpdateLayer(int layerIndex, float deltaTime, float clipDuration) {
    AnimatorLayerState* ls = GetLayerState(layerIndex);
    if (!ls) return;
    
    // Update weight blending
    if (std::abs(ls->Weight - ls->TargetWeight) > 0.001f) {
        float blendStep = ls->BlendSpeed * deltaTime;
        if (ls->Weight < ls->TargetWeight) {
            ls->Weight = std::min(ls->Weight + blendStep, ls->TargetWeight);
        } else {
            ls->Weight = std::max(ls->Weight - blendStep, ls->TargetWeight);
        }
    }
    
    // Skip FSM update if layer is inactive
    if (!ls->IsActive()) return;
    
    // Get the layer's states/transitions
    const AnimatorLayer* layer = m_Controller ? m_Controller->GetLayer(layerIndex) : nullptr;
    if (!layer && m_Controller && layerIndex == 0 && !m_Controller->HasLayers()) {
        // Legacy path: use flat structure for layer 0
        // This is handled by the legacy Update() call
        return;
    }
    if (!layer) return;
    
    // Initialize state if needed (only if layer has a default state)
    if (ls->CurrentStateId < 0 && layer->DefaultState >= 0) {
        ls->CurrentStateId = layer->DefaultState;
    }
    
    // Skip time advancement if layer has no active state
    // (transitions can still be evaluated by ChooseNextStateForLayer)
    if (ls->CurrentStateId < 0) return;
    
    // Track previous normalized time for wrap detection
    ls->PrevStateNormalized = ls->StateNormalized;
    
    // Advance state time
    ls->StateTime += deltaTime;
    if (clipDuration > 0.0f) {
        if (ls->StateTime >= clipDuration) {
            ls->HasCompletedOneCycle = true;
        }
        ls->StateNormalized = fmod(ls->StateTime, clipDuration) / clipDuration;
    } else {
        ls->StateNormalized = 0.0f;
    }
}

int Animator::ChooseNextStateForLayer(int layerIndex) const {
    if (!m_Controller) return -1;
    
    const AnimatorLayerState* ls = GetLayerState(layerIndex);
    if (!ls) return -1;
    
    const AnimatorLayer* layer = m_Controller->GetLayer(layerIndex);
    if (!layer && layerIndex == 0 && !m_Controller->HasLayers()) {
        // Legacy path: use flat structure
        return ChooseNextState();
    }
    if (!layer) return -1;
    
    const int current = ls->CurrentStateId;
    
    auto transitionMatches = [&](const AnimatorTransition& t) -> bool {
        bool ok = true;
        for (const auto& cond : t.Conditions) {
            if (!EvaluateCondition(cond, m_Blackboard)) { ok = false; break; }
        }
        if (!ok) return false;
        if (t.HasExitTime) {
            float epsilon = (t.ExitTime >= 0.95f) ? 0.05f : 0.02f;
            bool wrapped = ls->PrevStateNormalized > ls->StateNormalized && ls->HasCompletedOneCycle;
            if (wrapped && t.ExitTime >= 0.5f) return true;
            if (ls->StateNormalized + epsilon < t.ExitTime) return false;
        }
        return true;
    };
    
    // Pass 1: Prefer transitions from current state
    for (const auto& t : layer->Transitions) {
        if (t.FromState == current && transitionMatches(t)) return t.ToState;
    }
    // Pass 2: AnyState transitions
    for (const auto& t : layer->Transitions) {
        if (t.FromState == -1 && t.ToState != current && transitionMatches(t)) return t.ToState;
    }
    return -1;
}

const AnimatorTransition* Animator::FindTransitionToForLayer(int layerIndex, int toStateId) const {
    if (!m_Controller) return nullptr;
    
    const AnimatorLayerState* ls = GetLayerState(layerIndex);
    if (!ls) return nullptr;
    
    const AnimatorLayer* layer = m_Controller->GetLayer(layerIndex);
    if (!layer && layerIndex == 0 && !m_Controller->HasLayers()) {
        return FindTransitionTo(toStateId);
    }
    if (!layer) return nullptr;
    
    const int current = ls->CurrentStateId;
    
    auto transitionMatches = [&](const AnimatorTransition& t) -> bool {
        if (t.ToState != toStateId) return false;
        bool ok = true;
        for (const auto& cond : t.Conditions) {
            if (!EvaluateCondition(cond, m_Blackboard)) { ok = false; break; }
        }
        if (!ok) return false;
        if (t.HasExitTime) {
            float epsilon = (t.ExitTime >= 0.95f) ? 0.05f : 0.02f;
            bool wrapped = ls->PrevStateNormalized > ls->StateNormalized && ls->HasCompletedOneCycle;
            if (wrapped && t.ExitTime >= 0.5f) return true;
            if (ls->StateNormalized + epsilon < t.ExitTime) return false;
        }
        return true;
    };
    
    for (const auto& t : layer->Transitions) {
        if (t.FromState == current && transitionMatches(t)) return &t;
    }
    for (const auto& t : layer->Transitions) {
        if (t.FromState == -1 && transitionMatches(t)) return &t;
    }
    return nullptr;
}

void Animator::SetLayerWeight(int layerIndex, float weight) {
    AnimatorLayerState* ls = GetLayerState(layerIndex);
    if (!ls) {
        ls = &GetOrCreateLayerState(layerIndex);
    }
    ls->Weight = glm::clamp(weight, 0.0f, 1.0f);
    ls->TargetWeight = ls->Weight;
}

void Animator::SetLayerWeight(const std::string& layerName, float weight) {
    if (!m_Controller) return;
    const AnimatorLayer* layer = m_Controller->GetLayerByName(layerName);
    if (layer) {
        SetLayerWeight(layer->Index, weight);
    }
}

void Animator::BlendLayerWeight(int layerIndex, float targetWeight, float blendSpeed) {
    AnimatorLayerState* ls = GetLayerState(layerIndex);
    if (!ls) {
        ls = &GetOrCreateLayerState(layerIndex);
    }
    ls->TargetWeight = glm::clamp(targetWeight, 0.0f, 1.0f);
    ls->BlendSpeed = blendSpeed;
}

void Animator::BlendLayerWeight(const std::string& layerName, float targetWeight, float blendSpeed) {
    if (!m_Controller) return;
    const AnimatorLayer* layer = m_Controller->GetLayerByName(layerName);
    if (layer) {
        BlendLayerWeight(layer->Index, targetWeight, blendSpeed);
    }
}

float Animator::GetLayerWeight(int layerIndex) const {
    const AnimatorLayerState* ls = GetLayerState(layerIndex);
    return ls ? ls->Weight : 0.0f;
}

float Animator::GetLayerWeight(const std::string& layerName) const {
    if (!m_Controller) return 0.0f;
    const AnimatorLayer* layer = m_Controller->GetLayerByName(layerName);
    return layer ? GetLayerWeight(layer->Index) : 0.0f;
}

void Animator::SetBool(const std::string& name, bool value) {
    m_Blackboard.Bools[name] = value;
    if (!m_Controller) return;
    if (const AnimatorParameter* param = m_Controller->FindParameter(name)) {
        if (param->Type == AnimatorParamType::Bool) {
            m_Blackboard.SetBoolSlot(param->RuntimeSlot, value);
        }
    }
}

void Animator::SetInt(const std::string& name, int value) {
    m_Blackboard.Ints[name] = value;
    if (!m_Controller) return;
    if (const AnimatorParameter* param = m_Controller->FindParameter(name)) {
        if (param->Type == AnimatorParamType::Int) {
            m_Blackboard.SetIntSlot(param->RuntimeSlot, value);
        }
    }
}

void Animator::SetFloat(const std::string& name, float value) {
    m_Blackboard.Floats[name] = value;
    if (!m_Controller) return;
    if (const AnimatorParameter* param = m_Controller->FindParameter(name)) {
        if (param->Type == AnimatorParamType::Float) {
            m_Blackboard.SetFloatSlot(param->RuntimeSlot, value);
        }
    }
}

void Animator::SetTrigger(const std::string& name) {
    m_Blackboard.Triggers[name] = true;
    if (!m_Controller) return;
    if (const AnimatorParameter* param = m_Controller->FindParameter(name)) {
        if (param->Type == AnimatorParamType::Trigger) {
            m_Blackboard.SetTriggerSlot(param->RuntimeSlot, true);
        }
    }
}

void Animator::ResetTrigger(const std::string& name) {
    m_Blackboard.Triggers[name] = false;
    if (!m_Controller) return;
    if (const AnimatorParameter* param = m_Controller->FindParameter(name)) {
        if (param->Type == AnimatorParamType::Trigger) {
            m_Blackboard.SetTriggerSlot(param->RuntimeSlot, false);
        }
    }
}

float Animator::GetFloat(const std::string& name, float defaultValue) const {
    if (m_Controller) {
        if (const AnimatorParameter* param = m_Controller->FindParameter(name)) {
            if (param->Type == AnimatorParamType::Float) {
                return m_Blackboard.GetFloatSlot(param->RuntimeSlot);
            }
        }
    }
    auto it = m_Blackboard.Floats.find(name);
    return it != m_Blackboard.Floats.end() ? it->second : defaultValue;
}

float Animator::GetFloatSlot(int slot, float defaultValue) const {
    if (slot < 0 || static_cast<size_t>(slot) >= m_Blackboard.FloatValues.size()) {
        return defaultValue;
    }
    return m_Blackboard.GetFloatSlot(slot);
}

} // namespace animation
} // namespace cm


