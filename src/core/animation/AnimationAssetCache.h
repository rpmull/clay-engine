#pragma once

#include <memory>
#include <string>

namespace cm {
namespace animation {

struct AnimationAsset;

// Loads a unified AnimationAsset from disk (with optional legacy clip fallback) and caches it by path.
// Subsequent calls reuse the cached shared_ptr, avoiding repeated JSON deserialization for large clips.
std::shared_ptr<AnimationAsset> LoadAnimationAssetCached(const std::string& path, bool allowLegacyFallback = true);

// Returns the cached asset when it is already available, or nullptr if it has not finished loading yet.
std::shared_ptr<AnimationAsset> TryGetAnimationAssetCached(const std::string& path);

// Requests a background preload for the asset when a job system is available.
// Falls back to an immediate load when background jobs are unavailable.
void RequestAnimationAssetPreload(const std::string& path, bool allowLegacyFallback = true);

// Removes a single cached entry (e.g., after an asset was reimported).
void InvalidateAnimationAssetCache(const std::string& path);

// Clears the entire cache.
void ClearAnimationAssetCache();

} // namespace animation
} // namespace cm


