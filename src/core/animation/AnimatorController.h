#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <nlohmann/json.hpp>
#include <filesystem>
#include "core/vfs/FileSystem.h"
#include "core/animation/AvatarMask.h"

namespace cm {
namespace animation {

// Node kinds
enum class AnimatorStateKind {
    Single = 0,
    Blend1D = 1,
    Blend2D = 2
};

/// Blend mode for animation layers
enum class AnimatorLayerBlendMode : uint8_t {
    Override = 0,   ///< Replace underlying animation (lerp based on weight)
    Additive = 1    ///< Add rotation/position deltas on top of underlying animation
};

struct Blend1DEntry {
    float Key = 0.0f; // authored scalar key (not limited to 0..1)
    std::string ClipPath; // Legacy .anim
    std::string AssetPath; // Unified .anim
};

struct Blend2DEntry {
    float X = 0.0f; // typically -1..1 for locomotion grids
    float Y = 0.0f; // typically -1..1 for locomotion grids
    std::string ClipPath; // Legacy .anim
    std::string AssetPath; // Unified .anim
};

// Parameters
enum class AnimatorParamType {
    Bool,
    Int,
    Float,
    Trigger
};

struct AnimatorParameter {
    std::string Name;
    AnimatorParamType Type = AnimatorParamType::Float;
    // Defaults
    bool DefaultBool = false;
    int DefaultInt = 0;
    float DefaultFloat = 0.0f;
    int RuntimeSlot = -1; // runtime-only dense slot within the parameter type
};

// Conditions
enum class ConditionMode {
    If,
    IfNot,
    Greater,
    Less,
    Equals,
    NotEquals,
    Trigger
};

struct AnimatorCondition {
    std::string Parameter;
    ConditionMode Mode = ConditionMode::If;
    // For numeric comparisons
    float Threshold = 0.0f;
    int IntThreshold = 0;
    AnimatorParamType RuntimeParameterType = AnimatorParamType::Float;
    int RuntimeParameterSlot = -1;
};

// State & Transition
struct AnimatorState {
    int Id = -1;
    std::string Name;
    std::string ClipPath; // Legacy .anim
    std::string AnimationAssetPath; // Unified .anim (new)
    float Speed = 1.0f;
    bool Loop = true;
    AnimatorStateKind Kind = AnimatorStateKind::Single;
    // Blend1D specific
    std::string Blend1DParam; // name of float parameter
    std::vector<Blend1DEntry> Blend1DEntries; // sorted by Key
    // Blend2D specific
    std::string Blend2DParamX; // name of X float parameter
    std::string Blend2DParamY; // name of Y float parameter
    std::vector<Blend2DEntry> Blend2DEntries; // arbitrary point cloud
    int RuntimeBlend1DParamSlot = -1;
    int RuntimeBlend2DParamXSlot = -1;
    int RuntimeBlend2DParamYSlot = -1;
    // If true, this state does not contribute to runtime Y placement/root vertical changes.
    // Useful for reaction/upper-body focused one-shots that should not disturb grounded height.
    bool BypassYMotion = false;
    // Editor visualization
    float EditorPosX = 0.0f;
    float EditorPosY = 0.0f;
    
    // Per-state body mask override (useful for base layer locomotion to exclude upper body)
    // 0 = inherit from layer (no override), non-zero = bitmask of enabled body part groups
    // Bits: 0=Hips, 1=Spine, 2=Head, 3=LeftArm, 4=RightArm, 5=LeftLeg, 6=RightLeg
    uint8_t StateMaskBits = 0;
    
    // Body part group bit flags for StateMaskBits
    static constexpr uint8_t MaskBit_Hips     = 1 << 0;  // 1
    static constexpr uint8_t MaskBit_Spine    = 1 << 1;  // 2  (spine, chest, upper chest)
    static constexpr uint8_t MaskBit_Head     = 1 << 2;  // 4  (neck, head, eyes, jaw)
    static constexpr uint8_t MaskBit_LeftArm  = 1 << 3;  // 8  (shoulder through fingers)
    static constexpr uint8_t MaskBit_RightArm = 1 << 4;  // 16
    static constexpr uint8_t MaskBit_LeftLeg  = 1 << 5;  // 32 (upper leg through toes)
    static constexpr uint8_t MaskBit_RightLeg = 1 << 6;  // 64
    
    // Convenience combinations
    static constexpr uint8_t MaskBits_FullBody  = 0x7F;  // All bits
    static constexpr uint8_t MaskBits_UpperBody = MaskBit_Spine | MaskBit_Head | MaskBit_LeftArm | MaskBit_RightArm;
    static constexpr uint8_t MaskBits_LowerBody = MaskBit_Hips | MaskBit_LeftLeg | MaskBit_RightLeg;
    static constexpr uint8_t MaskBits_Arms      = MaskBit_LeftArm | MaskBit_RightArm;
    static constexpr uint8_t MaskBits_Legs      = MaskBit_LeftLeg | MaskBit_RightLeg;
    
    /// Returns true if this state has a mask override
    bool HasMaskOverride() const { return StateMaskBits != 0; }
    
    /// Get the mask bits (0 means use layer default)
    uint8_t GetMaskBits() const { return StateMaskBits; }
};

struct AnimatorTransition {
    int Id = -1;               // Stable link id for editor selection
    int FromState = -1; // -1 means AnyState
    int ToState = -1;
    bool HasExitTime = false;
    float ExitTime = 0.0f; // normalized 0..1
    float Duration = 0.0f; // seconds (or normalized if DurationNormalized)
    bool DurationNormalized = false; // if true, Duration is 0..1 of source state length
    std::vector<AnimatorCondition> Conditions;
};

// =========================================================================
// Animation Layers (authored as part of the controller)
// =========================================================================

/// Defines an animation layer with its own state machine.
/// Layer 0 (base) is always FullBody. Additional layers can override specific body parts.
/// Each layer has its own states and transitions, sharing the controller's parameters.
struct AnimatorLayer {
    int Index = 0;                              ///< Layer index (0 = base layer)
    std::string Name;                           ///< Human-readable name (e.g., "RightArmOverride")
    
    // State machine for this layer
    std::vector<AnimatorState> States;          ///< States specific to this layer
    std::vector<AnimatorTransition> Transitions;///< Transitions specific to this layer
    int DefaultState = -1;                      ///< Default state ID for this layer
    
    // Layer configuration
    BodyMaskPreset MaskPreset = BodyMaskPreset::FullBody; ///< Which body parts this layer affects
    AnimatorLayerBlendMode BlendMode = AnimatorLayerBlendMode::Override;
    float DefaultWeight = 1.0f;                 ///< Default blend weight (0 = inactive, 1 = full)
    
    // Helpers
    const AnimatorState* FindState(int id) const {
        for (const auto& s : States) if (s.Id == id) return &s;
        return nullptr;
    }
    AnimatorState* FindState(int id) {
        for (auto& s : States) if (s.Id == id) return &s;
        return nullptr;
    }
};

struct AnimatorController {
    std::string Name;
    std::vector<AnimatorParameter> Parameters;  ///< Shared parameters across all layers
    std::unordered_map<std::string, size_t> RuntimeParameterLookup;
    uint32_t RuntimeBoolCount = 0;
    uint32_t RuntimeIntCount = 0;
    uint32_t RuntimeFloatCount = 0;
    uint32_t RuntimeTriggerCount = 0;
    bool RuntimeCompiled = false;
    
    // =========================================================================
    // Layer-based architecture (new)
    // =========================================================================
    std::vector<AnimatorLayer> Layers;          ///< All layers (index 0 = base layer)
    
    // =========================================================================
    // Legacy flat structure (for backward compatibility with existing controllers)
    // These are used when Layers is empty - represents a single base layer
    // =========================================================================
    std::vector<AnimatorState> States;
    std::vector<AnimatorTransition> Transitions;
    int DefaultState = -1;
    
    // =========================================================================
    // Helper Methods
    // =========================================================================
    
    /// Get a layer by index (returns nullptr if out of range)
    const AnimatorLayer* GetLayer(int index) const {
        if (index < 0) return nullptr;
        // If using new layer architecture
        if (!Layers.empty()) {
            if (index < static_cast<int>(Layers.size())) return &Layers[index];
            return nullptr;
        }
        // Legacy: only base layer exists (index 0)
        return nullptr;
    }
    AnimatorLayer* GetLayer(int index) {
        if (index < 0) return nullptr;
        if (!Layers.empty()) {
            if (index < static_cast<int>(Layers.size())) return &Layers[index];
            return nullptr;
        }
        return nullptr;
    }
    
    /// Get layer by name (returns nullptr if not found)
    const AnimatorLayer* GetLayerByName(const std::string& name) const {
        for (const auto& layer : Layers) {
            if (layer.Name == name) return &layer;
        }
        return nullptr;
    }
    AnimatorLayer* GetLayerByName(const std::string& name) {
        for (auto& layer : Layers) {
            if (layer.Name == name) return &layer;
        }
        return nullptr;
    }
    
    /// Get total layer count (1 if using legacy flat structure)
    int GetLayerCount() const {
        return Layers.empty() ? 1 : static_cast<int>(Layers.size());
    }
    
    /// Check if this controller uses the new layer architecture
    bool HasLayers() const { return !Layers.empty(); }
    
    /// Find state by layer index and state ID (layer-aware lookup)
    const AnimatorState* FindStateInLayer(int layerIndex, int stateId) const {
        if (HasLayers()) {
            if (layerIndex >= 0 && layerIndex < static_cast<int>(Layers.size())) {
                return Layers[layerIndex].FindState(stateId);
            }
            return nullptr;
        }
        // Legacy: layerIndex 0 uses flat States
        if (layerIndex == 0) {
            for (const auto& s : States) if (s.Id == stateId) return &s;
        }
        return nullptr;
    }
    AnimatorState* FindStateInLayer(int layerIndex, int stateId) {
        if (HasLayers()) {
            if (layerIndex >= 0 && layerIndex < static_cast<int>(Layers.size())) {
                return Layers[layerIndex].FindState(stateId);
            }
            return nullptr;
        }
        if (layerIndex == 0) {
            for (auto& s : States) if (s.Id == stateId) return &s;
        }
        return nullptr;
    }
    
    /// Migrate legacy flat structure to layer architecture (called on save if needed)
    void MigrateToLayers() {
        if (!Layers.empty()) return;  // Already using layers
        if (States.empty() && DefaultState < 0) return;  // Nothing to migrate
        
        // Create base layer from legacy data
        AnimatorLayer baseLayer;
        baseLayer.Index = 0;
        baseLayer.Name = "Base Layer";
        baseLayer.States = std::move(States);
        baseLayer.Transitions = std::move(Transitions);
        baseLayer.DefaultState = DefaultState;
        baseLayer.MaskPreset = BodyMaskPreset::FullBody;
        baseLayer.BlendMode = AnimatorLayerBlendMode::Override;
        baseLayer.DefaultWeight = 1.0f;
        
        Layers.push_back(std::move(baseLayer));
        
        // Clear legacy fields
        States.clear();
        Transitions.clear();
        DefaultState = -1;
        CompileRuntimeData();
    }
    
    /// Find state in base layer (legacy compatibility helper)
    const AnimatorState* FindState(int id) const {
        // Try layers first
        if (!Layers.empty() && !Layers[0].States.empty()) {
            return Layers[0].FindState(id);
        }
        // Fall back to legacy flat structure
        for (const auto& s : States) if (s.Id == id) return &s;
        return nullptr;
    }
    AnimatorState* FindState(int id) {
        if (!Layers.empty() && !Layers[0].States.empty()) {
            return Layers[0].FindState(id);
        }
        for (auto& s : States) if (s.Id == id) return &s;
        return nullptr;
    }

    void CompileRuntimeData() {
        RuntimeParameterLookup.clear();
        RuntimeBoolCount = 0;
        RuntimeIntCount = 0;
        RuntimeFloatCount = 0;
        RuntimeTriggerCount = 0;

        for (size_t i = 0; i < Parameters.size(); ++i) {
            auto& param = Parameters[i];
            RuntimeParameterLookup[param.Name] = i;
            switch (param.Type) {
                case AnimatorParamType::Bool:
                    param.RuntimeSlot = static_cast<int>(RuntimeBoolCount++);
                    break;
                case AnimatorParamType::Int:
                    param.RuntimeSlot = static_cast<int>(RuntimeIntCount++);
                    break;
                case AnimatorParamType::Float:
                    param.RuntimeSlot = static_cast<int>(RuntimeFloatCount++);
                    break;
                case AnimatorParamType::Trigger:
                    param.RuntimeSlot = static_cast<int>(RuntimeTriggerCount++);
                    break;
            }
        }

        auto resolveFloatSlot = [&](const std::string& name) -> int {
            auto it = RuntimeParameterLookup.find(name);
            if (it == RuntimeParameterLookup.end() || it->second >= Parameters.size()) {
                return -1;
            }
            const AnimatorParameter& param = Parameters[it->second];
            return param.Type == AnimatorParamType::Float ? param.RuntimeSlot : -1;
        };

        auto resolveConditions = [&](std::vector<AnimatorTransition>& transitions) {
            for (auto& transition : transitions) {
                for (auto& condition : transition.Conditions) {
                    condition.RuntimeParameterSlot = -1;
                    auto it = RuntimeParameterLookup.find(condition.Parameter);
                    if (it == RuntimeParameterLookup.end() || it->second >= Parameters.size()) {
                        continue;
                    }
                    const AnimatorParameter& param = Parameters[it->second];
                    condition.RuntimeParameterType = param.Type;
                    condition.RuntimeParameterSlot = param.RuntimeSlot;
                }
            }
        };

        auto resolveStates = [&](std::vector<AnimatorState>& states) {
            for (auto& state : states) {
                state.RuntimeBlend1DParamSlot = resolveFloatSlot(state.Blend1DParam);
                state.RuntimeBlend2DParamXSlot = resolveFloatSlot(state.Blend2DParamX);
                state.RuntimeBlend2DParamYSlot = resolveFloatSlot(state.Blend2DParamY);
            }
        };

        resolveStates(States);
        resolveConditions(Transitions);
        for (auto& layer : Layers) {
            resolveStates(layer.States);
            resolveConditions(layer.Transitions);
        }

        RuntimeCompiled = true;
    }

    const AnimatorParameter* FindParameter(const std::string& name) const {
        auto it = RuntimeParameterLookup.find(name);
        if (it != RuntimeParameterLookup.end() && it->second < Parameters.size()) {
            return &Parameters[it->second];
        }
        for (const auto& param : Parameters) {
            if (param.Name == name) {
                return &param;
            }
        }
        return nullptr;
    }
};

// Serialization helpers for nlohmann::json (must be in same namespace for ADL)
    inline std::string ToString(AnimatorParamType t) {
        switch (t) {
            case AnimatorParamType::Bool: return "bool";
            case AnimatorParamType::Int: return "int";
            case AnimatorParamType::Float: return "float";
            case AnimatorParamType::Trigger: return "trigger";
        }
        return "float";
    }
    inline AnimatorParamType ParamTypeFromString(const std::string& s) {
        if (s == "bool") return AnimatorParamType::Bool;
        if (s == "int") return AnimatorParamType::Int;
        if (s == "float") return AnimatorParamType::Float;
        if (s == "trigger") return AnimatorParamType::Trigger;
        return AnimatorParamType::Float;
    }

    inline std::string ToString(ConditionMode m) {
        switch (m) {
            case ConditionMode::If: return "if";
            case ConditionMode::IfNot: return "if_not";
            case ConditionMode::Greater: return "greater";
            case ConditionMode::Less: return "less";
            case ConditionMode::Equals: return "equals";
            case ConditionMode::NotEquals: return "not_equals";
            case ConditionMode::Trigger: return "trigger";
        }
        return "if";
    }
    inline ConditionMode ConditionModeFromString(const std::string& s) {
        if (s == "if") return ConditionMode::If;
        if (s == "if_not") return ConditionMode::IfNot;
        if (s == "greater") return ConditionMode::Greater;
        if (s == "less") return ConditionMode::Less;
        if (s == "equals") return ConditionMode::Equals;
        if (s == "not_equals") return ConditionMode::NotEquals;
        if (s == "trigger") return ConditionMode::Trigger;
        return ConditionMode::If;
    }

    inline void to_json(nlohmann::json& j, const AnimatorParameter& p) {
        j = nlohmann::json{{"name", p.Name}, {"type", ToString(p.Type)}, {"defaultBool", p.DefaultBool}, {"defaultInt", p.DefaultInt}, {"defaultFloat", p.DefaultFloat}};
    }
    inline void from_json(const nlohmann::json& j, AnimatorParameter& p) {
        p.Name = j.value("name", "");
        p.Type = ParamTypeFromString(j.value("type", "float"));
        p.DefaultBool = j.value("defaultBool", false);
        p.DefaultInt = j.value("defaultInt", 0);
        p.DefaultFloat = j.value("defaultFloat", 0.0f);
    }

    inline void to_json(nlohmann::json& j, const AnimatorCondition& c) {
        j = nlohmann::json{{"param", c.Parameter}, {"mode", ToString(c.Mode)}, {"threshold", c.Threshold}, {"iThreshold", c.IntThreshold}};
    }
    inline void from_json(const nlohmann::json& j, AnimatorCondition& c) {
        c.Parameter = j.value("param", "");
        c.Mode = ConditionModeFromString(j.value("mode", "if"));
        c.Threshold = j.value("threshold", 0.0f);
        c.IntThreshold = j.value("iThreshold", 0);
    }

    inline static std::string _NormalizeSlashes(const std::string& in) {
        std::string s = in; for (char& c : s) if (c == '\\') c = '/'; return s;
    }
    inline static std::string _MakeProjectRelative(const std::string& path) {
        if (path.empty()) return path;
        std::string normalized = _NormalizeSlashes(path);
        
        // If already a VFS path (starts with "assets/"), keep it
        if (normalized.find("assets/") == 0) {
            return normalized;
        }
        
        // Look for "/assets/" in the path and extract from there
        // This handles paths like: C:/Users/.../out/build/x64-Debug/assets/animations/Idle.anim
        size_t assetsPos = normalized.find("/assets/");
        if (assetsPos != std::string::npos) {
            return normalized.substr(assetsPos + 1); // Skip the leading '/'
        }
        
        // Also check for backslash version before normalization missed it
        size_t assetsPos2 = normalized.find("\\assets\\");
        if (assetsPos2 != std::string::npos) {
            std::string result = normalized.substr(assetsPos2 + 1);
            return _NormalizeSlashes(result);
        }
        
        // If path is relative and doesn't contain "../", it's probably already good
        std::filesystem::path p(normalized);
        if (!p.is_absolute() && normalized.find("../") == std::string::npos) {
            return normalized;
        }
        
        // Try to make relative to project directory, but only if result is clean
        try {
            std::filesystem::path base = FileSystem::Instance().GetProjectRoot();
            if (!base.empty() && p.is_absolute()) {
                std::error_code ec;
                std::filesystem::path rel = std::filesystem::relative(p, base, ec);
                if (!ec) {
                    std::string relStr = _NormalizeSlashes(rel.string());
                    // Only use if it doesn't go up directories
                    if (relStr.find("../") == std::string::npos) {
                        return relStr;
                    }
                }
            }
        } catch (...) {}
        
        // Last resort: just return the filename with assets/animations/ prefix
        std::string filename = std::filesystem::path(normalized).filename().string();
        if (!filename.empty() && (filename.find(".anim") != std::string::npos)) {
            return "assets/animations/" + filename;
        }
        
        return normalized;
    }
    inline static std::string _ResolveProjectRelative(const std::string& path) {
        if (path.empty()) return path;
        try {
            std::filesystem::path p(path);
            if (p.is_absolute()) return _NormalizeSlashes(p.string());
            std::filesystem::path base = FileSystem::Instance().GetProjectRoot();
            if (!base.empty()) return _NormalizeSlashes((base / p).string());
        } catch (...) {}
        return _NormalizeSlashes(path);
    }

    // Helper to clean up paths when loading (fixes any bad relative paths)
    inline static std::string _CleanupLoadedPath(const std::string& path) {
        if (path.empty()) return path;
        std::string normalized = _NormalizeSlashes(path);
        
        // If path has weird "../" components, try to extract the assets/ part
        if (normalized.find("../") != std::string::npos) {
            size_t assetsPos = normalized.find("/assets/");
            if (assetsPos != std::string::npos) {
                return normalized.substr(assetsPos + 1);
            }
            assetsPos = normalized.find("assets/");
            if (assetsPos != std::string::npos) {
                return normalized.substr(assetsPos);
            }
        }
        
        return normalized;
    }

    inline void to_json(nlohmann::json& j, const Blend1DEntry& e) {
        j = nlohmann::json{{"key", e.Key}, {"clip", _MakeProjectRelative(e.ClipPath)}, {"asset", _MakeProjectRelative(e.AssetPath)}};
    }
    inline void from_json(const nlohmann::json& j, Blend1DEntry& e) {
        e.Key = j.value("key", 0.0f);
        e.ClipPath = _CleanupLoadedPath(j.value("clip", ""));
        e.AssetPath = _CleanupLoadedPath(j.value("asset", ""));
    }

    inline void to_json(nlohmann::json& j, const Blend2DEntry& e) {
        j = nlohmann::json{
            {"x", e.X},
            {"y", e.Y},
            {"clip", _MakeProjectRelative(e.ClipPath)},
            {"asset", _MakeProjectRelative(e.AssetPath)}
        };
    }
    inline void from_json(const nlohmann::json& j, Blend2DEntry& e) {
        e.X = j.value("x", 0.0f);
        e.Y = j.value("y", 0.0f);
        e.ClipPath = _CleanupLoadedPath(j.value("clip", ""));
        e.AssetPath = _CleanupLoadedPath(j.value("asset", ""));
    }

    inline void to_json(nlohmann::json& j, const AnimatorState& s) {
        j = nlohmann::json{
            {"id", s.Id}, {"name", s.Name}, 
            {"clip", _MakeProjectRelative(s.ClipPath)}, 
            {"asset", _MakeProjectRelative(s.AnimationAssetPath)}, 
            {"speed", s.Speed}, {"loop", s.Loop}, 
            {"x", s.EditorPosX}, {"y", s.EditorPosY}, 
            {"kind", (int)s.Kind}, {"blendParam", s.Blend1DParam}, 
            {"entries", s.Blend1DEntries},
            {"blendParamX", s.Blend2DParamX},
            {"blendParamY", s.Blend2DParamY},
            {"entries2D", s.Blend2DEntries},
            {"bypassYMotion", s.BypassYMotion},
            {"maskBits", s.StateMaskBits}  // 0 = inherit from layer, bitmask of body parts
        };
    }
    inline void from_json(const nlohmann::json& j, AnimatorState& s) {
        s.Id = j.value("id", -1);
        s.Name = j.value("name", "");
        s.ClipPath = _CleanupLoadedPath(j.value("clip", ""));
        s.AnimationAssetPath = _CleanupLoadedPath(j.value("asset", ""));
        s.Speed = j.value("speed", 1.0f);
        s.Loop = j.value("loop", true);
        s.EditorPosX = j.value("x", 0.0f);
        s.EditorPosY = j.value("y", 0.0f);
        s.Kind = (AnimatorStateKind)j.value("kind", 0);
        s.Blend1DParam = j.value("blendParam", "");
        if (j.contains("entries")) s.Blend1DEntries = j["entries"].get<std::vector<Blend1DEntry>>();
        s.Blend2DParamX = j.value("blendParamX", "");
        s.Blend2DParamY = j.value("blendParamY", "");
        if (j.contains("entries2D")) s.Blend2DEntries = j["entries2D"].get<std::vector<Blend2DEntry>>();
        s.BypassYMotion = j.value("bypassYMotion", false);
        s.StateMaskBits = j.value("maskBits", (uint8_t)0);  // Default: inherit from layer
        // Backwards compatibility: convert old maskOverride (-1=inherit, 0+=preset index)
        if (j.contains("maskOverride") && !j.contains("maskBits")) {
            int oldVal = j.value("maskOverride", -1);
            if (oldVal == 2) s.StateMaskBits = AnimatorState::MaskBits_LowerBody;  // LowerBody preset
            else if (oldVal == 1) s.StateMaskBits = AnimatorState::MaskBits_UpperBody;  // UpperBody preset
            else if (oldVal >= 0) s.StateMaskBits = AnimatorState::MaskBits_FullBody;  // Any other = full
        }
    }

    inline void to_json(nlohmann::json& j, const AnimatorTransition& t) {
        j = nlohmann::json{{"id", t.Id}, {"from", t.FromState}, {"to", t.ToState}, {"exit", t.HasExitTime}, {"exitTime", t.ExitTime}, {"duration", t.Duration}, {"durationNormalized", t.DurationNormalized}, {"conditions", t.Conditions}};
    }
    inline void from_json(const nlohmann::json& j, AnimatorTransition& t) {
        t.Id = j.value("id", -1);
        t.FromState = j.value("from", -1);
        t.ToState = j.value("to", -1);
        t.HasExitTime = j.value("exit", false);
        t.ExitTime = j.value("exitTime", 0.0f);
        t.Duration = j.value("duration", 0.0f);
        t.DurationNormalized = j.value("durationNormalized", false);
        if (j.contains("conditions")) t.Conditions = j["conditions"].get<std::vector<AnimatorCondition>>();
    }

    // AnimatorLayer serialization
    inline void to_json(nlohmann::json& j, const AnimatorLayer& layer) {
        j = nlohmann::json{
            {"index", layer.Index},
            {"name", layer.Name},
            {"states", layer.States},
            {"transitions", layer.Transitions},
            {"defaultState", layer.DefaultState},
            {"maskPreset", static_cast<int>(layer.MaskPreset)},
            {"blendMode", static_cast<int>(layer.BlendMode)},
            {"defaultWeight", layer.DefaultWeight}
        };
    }
    inline void from_json(const nlohmann::json& j, AnimatorLayer& layer) {
        layer.Index = j.value("index", 0);
        layer.Name = j.value("name", "");
        if (j.contains("states")) layer.States = j["states"].get<std::vector<AnimatorState>>();
        if (j.contains("transitions")) layer.Transitions = j["transitions"].get<std::vector<AnimatorTransition>>();
        layer.DefaultState = j.value("defaultState", -1);
        layer.MaskPreset = static_cast<BodyMaskPreset>(j.value("maskPreset", 0));
        layer.BlendMode = static_cast<AnimatorLayerBlendMode>(j.value("blendMode", 0));
        layer.DefaultWeight = j.value("defaultWeight", 1.0f);
    }

    inline void to_json(nlohmann::json& j, const AnimatorController& c) {
        j = nlohmann::json{{"name", c.Name}, {"parameters", c.Parameters}};
        
        // If using layer architecture, serialize layers
        if (!c.Layers.empty()) {
            j["layers"] = c.Layers;
        } else {
            // Legacy flat structure
            j["defaultState"] = c.DefaultState;
            j["states"] = c.States;
            j["transitions"] = c.Transitions;
        }
    }
    
    // Helper to fix condition modes based on parameter types (for legacy data migration)
    inline void _FixConditionModes(std::vector<AnimatorTransition>& transitions, const std::vector<AnimatorParameter>& params) {
        for (auto& t : transitions) {
            for (auto& cond : t.Conditions) {
                // Find the parameter type for this condition
                for (const auto& p : params) {
                    if (p.Name == cond.Parameter) {
                        // Fix mode to match parameter type
                        if (p.Type == AnimatorParamType::Trigger && cond.Mode != ConditionMode::Trigger) {
                            cond.Mode = ConditionMode::Trigger;
                        } else if (p.Type == AnimatorParamType::Bool && 
                                   cond.Mode != ConditionMode::If && cond.Mode != ConditionMode::IfNot) {
                            cond.Mode = ConditionMode::If;
                        } else if ((p.Type == AnimatorParamType::Float || p.Type == AnimatorParamType::Int) &&
                                   (cond.Mode == ConditionMode::Trigger || cond.Mode == ConditionMode::If || cond.Mode == ConditionMode::IfNot)) {
                            cond.Mode = ConditionMode::Greater;
                        }
                        break;
                    }
                }
            }
        }
    }
    
    // Legacy version for backward compatibility
    inline void _FixConditionModes(AnimatorController& c) {
        _FixConditionModes(c.Transitions, c.Parameters);
        for (auto& layer : c.Layers) {
            _FixConditionModes(layer.Transitions, c.Parameters);
        }
    }
    
    inline void from_json(const nlohmann::json& j, AnimatorController& c) {
        c.Name = j.value("name", "");
        if (j.contains("parameters")) c.Parameters = j["parameters"].get<std::vector<AnimatorParameter>>();
        
        // Check for new layer architecture
        if (j.contains("layers")) {
            c.Layers = j["layers"].get<std::vector<AnimatorLayer>>();
            // Clear legacy fields
            c.States.clear();
            c.Transitions.clear();
            c.DefaultState = -1;
        } else {
            // Legacy flat structure
            c.DefaultState = j.value("defaultState", -1);
            if (j.contains("states")) c.States = j["states"].get<std::vector<AnimatorState>>();
            if (j.contains("transitions")) c.Transitions = j["transitions"].get<std::vector<AnimatorTransition>>();
            c.Layers.clear();
        }
        
        // Fix any condition modes that don't match their parameter types (legacy data migration)
        _FixConditionModes(c);
        c.CompileRuntimeData();
    }
// end serialization helpers

} // namespace animation
} // namespace cm


