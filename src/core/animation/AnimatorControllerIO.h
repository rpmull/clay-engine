#pragma once

#include <memory>
#include <string>

namespace cm {
namespace animation {

struct AnimatorController;

// Loads an AnimatorController from disk, resolving project-relative paths.
// In play mode, prefers binary format from .bin cache.
// Returns nullptr if the file cannot be opened or parsed.
std::shared_ptr<AnimatorController> LoadAnimatorControllerFromFile(const std::string& path);

// Alias for consistency with other asset loaders
inline std::shared_ptr<AnimatorController> LoadAnimatorController(const std::string& path) {
    return LoadAnimatorControllerFromFile(path);
}

// Save an AnimatorController to disk and update binary cache
bool SaveAnimatorController(const AnimatorController& ctrl, const std::string& path);

// Removes cached controller entries for a source or compiled controller path.
void InvalidateAnimatorControllerCache(const std::string& path);

// Clears all cached controllers.
void ClearAnimatorControllerCache();

} // namespace animation
} // namespace cm


