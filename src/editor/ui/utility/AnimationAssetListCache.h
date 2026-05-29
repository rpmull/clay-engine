#pragma once

#include <string>
#include <vector>

namespace ui {

struct AnimationAssetOption {
    std::string name;
    std::string path;
};

// Returns a cached list of all .anim files under the project's asset directory.
// The cache automatically rebuilds when the asset root changes or after file system updates.
const std::vector<AnimationAssetOption>& GetAnimationAssetOptions();

// Explicitly clears the cached list so the next call forces a rescan.
void InvalidateAnimationAssetOptions();

} // namespace ui


