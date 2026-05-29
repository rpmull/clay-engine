#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <bgfx/bgfx.h>

/**
 * PropertyID - Fast integer-based property identification system
 * 
 * Replaces string-based uniform/texture lookups with O(1) integer lookups.
 * Similar to Unity's Shader.PropertyToID() system.
 * 
 * Usage:
 *   PropertyID albedoId = PropertyID::Get("s_albedo");  // Computed once
 *   propertyBlock.SetTexture(albedoId, texture);       // Fast lookup
 */
class PropertyID {
public:
    static constexpr uint32_t Invalid = 0;
    
    PropertyID() : m_id(Invalid) {}
    explicit PropertyID(uint32_t id) : m_id(id) {}
    
    // Get or create a PropertyID for a name (thread-safe, cached)
    static PropertyID Get(const std::string& name);
    static PropertyID Get(const char* name);
    
    // Get the name back from an ID (for debugging)
    static const std::string& GetName(PropertyID id);
    
    // Pre-register common properties for fast startup
    static void RegisterCommonProperties();
    
    uint32_t Value() const { return m_id; }
    bool IsValid() const { return m_id != Invalid; }
    
    bool operator==(PropertyID other) const { return m_id == other.m_id; }
    bool operator!=(PropertyID other) const { return m_id != other.m_id; }
    bool operator<(PropertyID other) const { return m_id < other.m_id; }
    
    // For use as map key
    struct Hash {
        size_t operator()(PropertyID id) const { return std::hash<uint32_t>{}(id.m_id); }
    };

private:
    uint32_t m_id;
};

/**
 * PropertyRegistry - Singleton managing PropertyID mappings
 * 
 * Internal implementation - use PropertyID::Get() instead.
 */
class PropertyRegistry {
public:
    static PropertyRegistry& Instance();
    
    PropertyID GetOrCreate(const std::string& name);
    const std::string& GetName(PropertyID id) const;
    
    // Get cached bgfx uniform handle for a property (creates if needed)
    bgfx::UniformHandle GetUniformHandle(PropertyID id, bgfx::UniformType::Enum type);
    
    // Pre-cache common property IDs and handles
    void RegisterCommonProperties();
    
private:
    PropertyRegistry() = default;
    
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, uint32_t> m_nameToId;
    std::vector<std::string> m_idToName;  // Index = ID - 1
    
    // Cached bgfx uniform handles per property
    std::unordered_map<uint32_t, bgfx::UniformHandle> m_uniformHandles;
    
    uint32_t m_nextId = 1;  // 0 is reserved for Invalid
};

// Common property IDs (pre-registered for zero-cost access)
namespace CommonProperties {
    // Textures
    extern PropertyID Albedo;
    extern PropertyID MetallicRoughness;
    extern PropertyID NormalMap;
    extern PropertyID AO;
    extern PropertyID Emission;
    extern PropertyID TintMask;
    extern PropertyID Displacement;
    
    // Uniforms
    extern PropertyID ColorTint;
    extern PropertyID TextureUsage;
    extern PropertyID UVScaleOffset;
    extern PropertyID Metallic;
    extern PropertyID Roughness;
    extern PropertyID NormalScale;
    extern PropertyID EmissionStrength;
    extern PropertyID EmissionColor;
    
    // PSX-specific
    extern PropertyID PSXParams;
    extern PropertyID PSXWorld;
    extern PropertyID ToonParams;
    
    void Initialize();
}

