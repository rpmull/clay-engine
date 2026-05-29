#pragma once

#include <memory>
#include <string>
#include <cstdint>

class Material;

/**
 * RuntimeMaterialCache - Material caching for runtime prefab instantiation
 * 
 * Caches loaded materials to avoid redundant file I/O and shader compilation
 * when instantiating multiple prefabs that share materials.
 * 
 * Uses MaterialBinaryLoader under the hood (runtime-safe, no editor dependencies).
 * Thread-safe via internal mutex.
 * 
 * Performance Impact:
 * - Eliminates 10-100ms file I/O + shader loading per redundant material
 * - For prefabs with shared materials, can save 100-500ms total
 */
namespace RuntimeMaterialCache {

/**
 * Get or load a material from cache.
 * If material is not in cache, loads from disk via MaterialBinaryLoader and caches it.
 * 
 * @param matBinPath Path to the .matbin file
 * @param needsSkinned Whether to load as a skinned material variant
 * @return Shared pointer to the material (shared across all users)
 */
std::shared_ptr<Material> GetOrLoad(const std::string& matBinPath, bool needsSkinned);

/**
 * Invalidate cached material for a specific file.
 * Call this when a material file is modified to force reload on next access.
 * 
 * @param matBinPath Path to the .matbin file to invalidate
 */
void InvalidateCache(const std::string& matBinPath);

/**
 * Clear all cached materials.
 * Call on scene unload or when memory pressure requires cleanup.
 */
void ClearCache();

/**
 * Get statistics about cache usage.
 */
void GetStats(uint32_t& outCacheHits, uint32_t& outCacheMisses, uint32_t& outCachedMaterials);

} // namespace RuntimeMaterialCache
