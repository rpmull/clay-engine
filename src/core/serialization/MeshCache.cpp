#include "MeshCache.h"
#include "MeshBinaryLoader.h"
#include "core/rendering/Mesh.h"
#include "core/ecs/AnimationComponents.h"
#include "core/debug/PrefabLog.h"

#include <unordered_map>
#include <mutex>
#include <string>

namespace MeshCache {

// Cache key: meshBinPath:submeshIndex
static std::string MakeCacheKey(const std::string& path, uint32_t submeshIndex) {
    return path + ":" + std::to_string(submeshIndex);
}

// Cached mesh entry
struct CachedMeshEntry {
    std::shared_ptr<Mesh> mesh;
    std::unique_ptr<BlendShapeComponent> blendShapes;  // Original loaded blend shapes (template for cloning)
    bool isSkinned = false;
};

// Global cache state
static std::mutex s_cacheMutex;
static std::unordered_map<std::string, CachedMeshEntry> s_cache;
static uint32_t s_cacheHits = 0;
static uint32_t s_cacheMisses = 0;

// Clone blend shapes for a new entity (each entity needs its own weights)
static std::unique_ptr<BlendShapeComponent> CloneBlendShapes(const BlendShapeComponent* source) {
    if (!source || source->Shapes.empty()) {
        return nullptr;
    }
    
    auto clone = std::make_unique<BlendShapeComponent>();
    clone->Shapes.reserve(source->Shapes.size());
    
    for (const auto& shape : source->Shapes) {
        BlendShape clonedShape;
        clonedShape.Name = shape.Name;
        clonedShape.Weight = shape.Weight;
        clonedShape.IsSparse = shape.IsSparse;
        
        if (shape.IsSparse) {
            // Copy sparse data
            clonedShape.SparseIndices = shape.SparseIndices;
            clonedShape.SparseDeltaPos = shape.SparseDeltaPos;
            clonedShape.SparseDeltaNorm = shape.SparseDeltaNorm;
        } else {
            // Copy dense data
            clonedShape.DeltaPos = shape.DeltaPos;
            clonedShape.DeltaNormal = shape.DeltaNormal;
        }
        
        clone->Shapes.push_back(std::move(clonedShape));
    }
    
    // Mark as dirty so the new instance processes blend shapes
    clone->Dirty = true;
    
    // Don't copy scratch buffers - they'll be resized on first use
    
    return clone;
}

std::shared_ptr<Mesh> GetOrLoadMesh(const std::string& meshBinPath, 
                                     uint32_t submeshIndex, 
                                     bool* outSkinned) {
    std::string key = MakeCacheKey(meshBinPath, submeshIndex);
    
    {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        
        auto it = s_cache.find(key);
        if (it != s_cache.end()) {
            // Cache hit
            s_cacheHits++;
            if (outSkinned) {
                *outSkinned = it->second.isSkinned;
            }
            MESH_LOG("Cache HIT: " << key);
            return it->second.mesh;
        }
    }
    
    // Cache miss - load the mesh
    MESH_LOG("Cache MISS: " << key << " - loading from disk");
    
    bool skinned = false;
    auto mesh = MeshBinaryLoader::LoadMesh(meshBinPath, submeshIndex, &skinned);
    
    if (mesh) {
        // Also load blend shapes into the cache
        auto blendShapes = MeshBinaryLoader::LoadBlendShapes(meshBinPath, submeshIndex);
        
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        
        // Double-check in case another thread loaded it
        auto it = s_cache.find(key);
        if (it != s_cache.end()) {
            // Another thread beat us - use their version
            s_cacheHits++;
            if (outSkinned) {
                *outSkinned = it->second.isSkinned;
            }
            return it->second.mesh;
        }
        
        // Insert into cache
        CachedMeshEntry entry;
        entry.mesh = mesh;
        entry.blendShapes = std::move(blendShapes);
        entry.isSkinned = skinned;
        
        s_cache[key] = std::move(entry);
        s_cacheMisses++;
        
        if (outSkinned) {
            *outSkinned = skinned;
        }
    } else {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        s_cacheMisses++;
    }
    
    return mesh;
}

std::unique_ptr<BlendShapeComponent> GetOrLoadBlendShapes(const std::string& meshBinPath, 
                                                           uint32_t submeshIndex) {
    std::string key = MakeCacheKey(meshBinPath, submeshIndex);
    
    {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        
        auto it = s_cache.find(key);
        if (it != s_cache.end() && it->second.blendShapes) {
            // Clone from cached template
            return CloneBlendShapes(it->second.blendShapes.get());
        }
    }
    
    // Not in cache - need to load the mesh first (which will also load blend shapes)
    bool skinned = false;
    GetOrLoadMesh(meshBinPath, submeshIndex, &skinned);
    
    // Now try again
    {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        
        auto it = s_cache.find(key);
        if (it != s_cache.end() && it->second.blendShapes) {
            return CloneBlendShapes(it->second.blendShapes.get());
        }
    }
    
    // No blend shapes for this mesh
    return nullptr;
}

void InvalidateCache(const std::string& meshBinPath) {
    std::lock_guard<std::mutex> lock(s_cacheMutex);
    
    // Remove all entries for this mesh file (any submesh index)
    std::vector<std::string> keysToRemove;
    std::string pathPrefix = meshBinPath + ":";
    
    for (const auto& kv : s_cache) {
        if (kv.first.compare(0, pathPrefix.size(), pathPrefix) == 0) {
            keysToRemove.push_back(kv.first);
        }
    }
    
    for (const auto& key : keysToRemove) {
        s_cache.erase(key);
    }
    
    if (!keysToRemove.empty()) {
        MESH_LOG("Invalidated " << keysToRemove.size() << " cached meshes for: " << meshBinPath);
    }
}

void ClearCache() {
    std::lock_guard<std::mutex> lock(s_cacheMutex);
    
    size_t count = s_cache.size();
    s_cache.clear();
    
    MESH_LOG("Cleared mesh cache (" << count << " entries)");
}

void GetStats(uint32_t& outCacheHits, uint32_t& outCacheMisses, uint32_t& outCachedMeshes) {
    std::lock_guard<std::mutex> lock(s_cacheMutex);
    outCacheHits = s_cacheHits;
    outCacheMisses = s_cacheMisses;
    outCachedMeshes = static_cast<uint32_t>(s_cache.size());
}

void ResetStats() {
    std::lock_guard<std::mutex> lock(s_cacheMutex);
    s_cacheHits = 0;
    s_cacheMisses = 0;
}

} // namespace MeshCache
