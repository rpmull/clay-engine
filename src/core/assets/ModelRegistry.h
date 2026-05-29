#pragma once

#include "RuntimeModelManifest.h"
#include <unordered_map>
#include <string>
#include <mutex>

namespace cm {

/**
 * Centralized Model Registry
 * 
 * Single database of all model manifests, loaded once at startup from PAK.
 * Provides fast GUID-based lookup for model instantiation.
 * 
 * This is the runtime equivalent of iterating .meta files in the editor.
 * Scripts don't need to know which system is in use - the API is identical.
 */
class ModelRegistry {
public:
    static ModelRegistry& Instance();
    
    /**
     * Load the registry from a binary file (typically from PAK)
     * Called once at runtime startup
     */
    bool Load(const std::string& registryPath);
    
    /**
     * Load from memory (for embedded/streaming scenarios)
     */
    bool LoadFromMemory(const uint8_t* data, size_t size);
    
    /**
     * Check if a model GUID exists in the registry
     */
    bool HasModel(const ClaymoreGUID& guid) const;
    
    /**
     * Get the manifest for a model GUID
     * Returns nullptr if not found
     */
    const RuntimeModelManifest* GetManifest(const ClaymoreGUID& guid) const;
    
    /**
     * Get total number of registered models
     */
    size_t GetModelCount() const { return m_manifests.size(); }
    
    /**
     * Check if registry is loaded and ready
     */
    bool IsLoaded() const { return m_loaded; }
    
    /**
     * Clear all loaded data
     */
    void Clear();
    
private:
    ModelRegistry() = default;
    ~ModelRegistry() = default;
    ModelRegistry(const ModelRegistry&) = delete;
    ModelRegistry& operator=(const ModelRegistry&) = delete;
    
    // GUID packed as single 128-bit key for fast lookup
    struct GuidHash {
        size_t operator()(const ClaymoreGUID& g) const {
            return std::hash<uint64_t>()(g.high) ^ (std::hash<uint64_t>()(g.low) << 1);
        }
    };
    struct GuidEqual {
        bool operator()(const ClaymoreGUID& a, const ClaymoreGUID& b) const {
            return a.high == b.high && a.low == b.low;
        }
    };
    
    std::unordered_map<ClaymoreGUID, RuntimeModelManifest, GuidHash, GuidEqual> m_manifests;
    mutable std::mutex m_mutex;
    bool m_loaded = false;
};

/**
 * Binary format for model_registry.bin:
 * 
 * Header:
 *   uint32_t magic          // 'MREG'
 *   uint32_t version        // 1
 *   uint32_t modelCount
 * 
 * Models[modelCount]:
 *   (Each model is the same format as RuntimeModelManifest binary)
 *   - Model GUID is the key for lookup
 */

} // namespace cm


