#pragma once

#include <memory>
#include <string>

class Material;

namespace MaterialAssetCache
{
    // Returns a shared material instance for the given asset path (project-relative or absolute).
    // Creates and caches the material on first request.
    std::shared_ptr<Material> Acquire(const std::string& assetPath);

    // Forces a reload of the asset on disk and updates the cache entry.
    // Returns the refreshed material (or nullptr on failure).
    std::shared_ptr<Material> Reload(const std::string& assetPath);

    // Removes a cached material entry so the next Acquire will rebuild it.
    void Invalidate(const std::string& assetPath);
}

