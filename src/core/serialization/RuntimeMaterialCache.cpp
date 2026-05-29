#include "MaterialCache.h"
#include "MaterialBinaryLoader.h"
#include "core/rendering/Material.h"
#include "core/debug/PrefabLog.h"

#include <unordered_map>
#include <mutex>
#include <string>

namespace RuntimeMaterialCache {

// Cache key: matBinPath:skinned
static std::string MakeCacheKey(const std::string& path, bool skinned) {
    return path + (skinned ? ":skinned" : ":static");
}

// Global cache state
static std::mutex s_cacheMutex;
static std::unordered_map<std::string, std::weak_ptr<Material>> s_cache;
static uint32_t s_cacheHits = 0;
static uint32_t s_cacheMisses = 0;

std::shared_ptr<Material> GetOrLoad(const std::string& matBinPath, bool needsSkinned) {
    std::string key = MakeCacheKey(matBinPath, needsSkinned);
    
    {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        
        auto it = s_cache.find(key);
        if (it != s_cache.end()) {
            if (auto existing = it->second.lock()) {
                // Cache hit - material still alive
                s_cacheHits++;
                MATERIAL_LOG("Cache HIT: " << key);
                return existing;
            }
            // Weak pointer expired - remove stale entry
            s_cache.erase(it);
        }
    }
    
    // Cache miss - load the material
    MATERIAL_LOG("Cache MISS: " << key << " - loading from disk");
    
    auto material = binary::MaterialBinaryLoader::Load(matBinPath, needsSkinned);
    
    if (material) {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        
        // Double-check in case another thread loaded it
        auto it = s_cache.find(key);
        if (it != s_cache.end()) {
            if (auto existing = it->second.lock()) {
                s_cacheHits++;
                return existing;
            }
        }
        
        // Insert into cache (weak_ptr so materials are freed when no longer referenced)
        s_cache[key] = material;
        s_cacheMisses++;
    } else {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        s_cacheMisses++;
    }
    
    return material;
}

void InvalidateCache(const std::string& matBinPath) {
    std::lock_guard<std::mutex> lock(s_cacheMutex);
    
    // Remove both skinned and static variants
    std::string keyStatic = MakeCacheKey(matBinPath, false);
    std::string keySkinned = MakeCacheKey(matBinPath, true);
    
    s_cache.erase(keyStatic);
    s_cache.erase(keySkinned);
    
    MATERIAL_LOG("Invalidated material cache for: " << matBinPath);
}

void ClearCache() {
    std::lock_guard<std::mutex> lock(s_cacheMutex);
    
    size_t count = s_cache.size();
    s_cache.clear();
    
    MATERIAL_LOG("Cleared material cache (" << count << " entries)");
}

void GetStats(uint32_t& outCacheHits, uint32_t& outCacheMisses, uint32_t& outCachedMaterials) {
    std::lock_guard<std::mutex> lock(s_cacheMutex);
    
    // Count live entries (non-expired weak_ptrs)
    uint32_t liveCount = 0;
    for (const auto& kv : s_cache) {
        if (!kv.second.expired()) {
            liveCount++;
        }
    }
    
    outCacheHits = s_cacheHits;
    outCacheMisses = s_cacheMisses;
    outCachedMaterials = liveCount;
}

} // namespace RuntimeMaterialCache
