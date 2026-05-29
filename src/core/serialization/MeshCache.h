#pragma once

#include <memory>
#include <string>
#include <cstdint>

struct Mesh;
struct BlendShapeComponent;

/**
 * MeshCache - Global mesh caching system for runtime prefab instantiation
 * 
 * Caches loaded meshes and blend shapes to avoid redundant file I/O and
 * GPU buffer creation when instantiating multiple prefabs that share meshes.
 * 
 * Thread-safe via internal mutex.
 * 
 * Performance Impact:
 * - Eliminates 5-50ms file I/O per redundant mesh load
 * - Eliminates 1-10ms GPU buffer creation per redundant mesh
 * - For prefabs with shared meshes, can save 50-500ms total
 */
namespace MeshCache {

/**
 * Get or load a mesh from cache.
 * If mesh is not in cache, loads from disk and caches it.
 * 
 * @param meshBinPath Path to the .meshbin file
 * @param submeshIndex Index of the submesh to load
 * @param outSkinned Optional output: whether mesh has skinning data
 * @return Shared pointer to the mesh (shared across all users of the same mesh)
 */
std::shared_ptr<Mesh> GetOrLoadMesh(const std::string& meshBinPath, 
                                     uint32_t submeshIndex, 
                                     bool* outSkinned = nullptr);

/**
 * Get or load blend shapes from cache.
 * Returns a CLONE of the cached blend shapes to avoid sharing mutable state
 * (each entity needs its own weight values).
 * 
 * @param meshBinPath Path to the .meshbin file
 * @param submeshIndex Index of the submesh
 * @return Unique pointer to cloned blend shapes, or nullptr if none exist
 */
std::unique_ptr<BlendShapeComponent> GetOrLoadBlendShapes(const std::string& meshBinPath, 
                                                           uint32_t submeshIndex);

/**
 * Invalidate cached mesh data for a specific file.
 * Call this when a mesh file is modified to force reload on next access.
 * 
 * @param meshBinPath Path to the .meshbin file to invalidate
 */
void InvalidateCache(const std::string& meshBinPath);

/**
 * Clear all cached meshes.
 * Call on scene unload or when memory pressure requires cleanup.
 */
void ClearCache();

/**
 * Get statistics about cache usage.
 * @param outCacheHits Output: number of cache hits
 * @param outCacheMisses Output: number of cache misses
 * @param outCachedMeshes Output: number of meshes currently cached
 */
void GetStats(uint32_t& outCacheHits, uint32_t& outCacheMisses, uint32_t& outCachedMeshes);

/**
 * Reset cache statistics.
 */
void ResetStats();

} // namespace MeshCache
