#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "core/animation/AnimationAsset.h"  // For RootMotionMode, RootMotionSettings

/// @brief Import settings for animation files that persist across reimports
/// Stored in the .anim.meta file alongside the animation asset
struct AnimationImportSettings
{
    /// Rotation correction applied to all animation tracks during import
    /// Useful for fixing axis orientation mismatches (e.g., +90 or -90 degrees on X)
    enum class RotationCorrection { None, RotateX_Plus90, RotateX_Minus90 };
    RotationCorrection XAxisCorrection = RotationCorrection::None;
    
    // =========================================================================
    // Root Motion Settings (baked into animation asset)
    // =========================================================================
    
    /// Root motion mode for this animation
    cm::animation::RootMotionMode RootMotionMode = cm::animation::RootMotionMode::InPlace;
    
    /// Include horizontal (XZ plane) motion when mode is ApplyToEntity
    bool RootMotionIncludeXZ = true;
    
    /// Include vertical (Y axis) motion when mode is ApplyToEntity
    bool RootMotionIncludeY = true;
    
    /// Include rotation from root bone when mode is ApplyToEntity
    bool RootMotionIncludeRotation = false;
    
    /// Override gravity while root motion Y is active (for climbing, etc.)
    bool RootMotionOverrideGravity = false;
    
    /// Name of the bone to extract root motion from (empty = auto-detect hips/root)
    std::string RootMotionBoneName;
    
    // =========================================================================
    // Legacy Settings (kept for backward compatibility)
    // =========================================================================
    
    /// @deprecated Use RootMotionMode instead
    bool ExtractXZRootMotion = false;
    
    /// @deprecated Use RootMotionMode instead  
    bool ExtractYRootMotion = false;
    
    // =========================================================================
    // Source File Information
    // =========================================================================
    
    /// Source file path this animation was imported from (for reimport)
    std::string SourceFilePath;
    
    /// Animation index within the source file (for multi-animation FBX/glTF files)
    int SourceAnimationIndex = 0;

    /// Optional source rig model path used to resolve the animation's bind pose.
    /// Useful for skeleton-only animation files that need a mesh-backed rig.
    std::string SourceRigModelPath;

    /// Optional target model path used to bake animation deltas for a target rig.
    /// Used for Rig-B -> base_human style retarget baking during import.
    std::string SourceAvatarModelPath;

    /// @deprecated Root source is now auto-resolved at runtime.
    bool UseHipsAsRoot = false;
    
    // JSON serialization
    nlohmann::json ToJson() const;
    static AnimationImportSettings FromJson(const nlohmann::json& j);
    
    // Load/save from animation .meta file
    static bool LoadFromMeta(const std::string& animPath, AnimationImportSettings& out);
    static bool SaveToMeta(const std::string& animPath, const AnimationImportSettings& settings);
    
    // Helper to get meta path from anim path
    static std::string GetMetaPath(const std::string& animPath);
    
    /// Convert to RootMotionSettings for baking into animation asset
    cm::animation::RootMotionSettings ToRootMotionSettings() const {
        cm::animation::RootMotionSettings s;
        s.Mode = RootMotionMode;
        s.IncludeXZ = RootMotionIncludeXZ;
        s.IncludeY = RootMotionIncludeY;
        s.IncludeRotation = RootMotionIncludeRotation;
        s.OverrideGravity = RootMotionOverrideGravity;
        s.RootBoneName = RootMotionBoneName;
        return s;
    }
    
    /// Load from RootMotionSettings (for editing existing asset)
    void FromRootMotionSettings(const cm::animation::RootMotionSettings& s) {
        RootMotionMode = s.Mode;
        RootMotionIncludeXZ = s.IncludeXZ;
        RootMotionIncludeY = s.IncludeY;
        RootMotionIncludeRotation = s.IncludeRotation;
        RootMotionOverrideGravity = s.OverrideGravity;
        RootMotionBoneName = s.RootBoneName;
    }
};

/// @brief Root motion data extracted from an animation during import
/// This is stored in the .anim file itself under a "rootMotion" key
struct AnimationRootMotion
{
    /// Per-keyframe root position deltas (world-space XZ or Y depending on mode)
    struct RootKey { float time; float deltaX; float deltaY; float deltaZ; };
    std::vector<RootKey> Keys;
    
    /// Total distance traveled (for preview/debugging)
    float TotalDistanceXZ = 0.0f;
    float TotalDistanceY = 0.0f;
    
    nlohmann::json ToJson() const;
    static AnimationRootMotion FromJson(const nlohmann::json& j);
};

