#pragma once

#include <string>
#include <vector>
#include "core/animation/AnimationTypes.h"
#include "core/animation/AnimationAsset.h"

// Forward declaration
struct AnimationImportSettings;
struct AnimationRootMotion;

namespace cm {
namespace animation {

class AnimationImporter {
public:
    // Extract all animations from a model file (fbx, gltf, etc.) using Assimp
    static std::vector<AnimationClip> ImportFromModel(const std::string& filepath);
    // New unified import convenience: build a single unified AnimationAsset and save it
    static bool ImportUnifiedAnimationFromFBX(const std::string& filepath, const std::string& outAnimPath);
    
    /// Import with custom settings (rotation correction, root motion extraction)
    /// @param filepath Source model file (FBX, glTF, etc.)
    /// @param outAnimPath Output .anim file path
    /// @param settings Import settings with rotation correction and root motion flags
    /// @return true if import succeeded
    static bool ImportUnifiedAnimationFromFBXWithSettings(
        const std::string& filepath, 
        const std::string& outAnimPath,
        const AnimationImportSettings& settings);
    
    /// Extract root motion data from an animation asset
    /// @param asset The animation asset to analyze
    /// @param settings Import settings (for root bone name override)
    /// @param outRootMotion Output root motion data
    /// @return true if root motion was successfully extracted
    static bool ExtractRootMotion(
        const AnimationAsset& asset,
        const AnimationImportSettings& settings,
        AnimationRootMotion& outRootMotion);
};

} // namespace animation
} // namespace cm
