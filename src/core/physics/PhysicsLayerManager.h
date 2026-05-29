// PhysicsLayerManager.h
// Data-driven physics layer system - layers are defined by users at runtime
#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace PhysicsLayers {

// Maximum number of layers (32 bits in mask)
constexpr uint32_t MAX_LAYERS = 32;

// Default layer masks
constexpr uint32_t MASK_ALL = 0xFFFFFFFF;
constexpr uint32_t MASK_NONE = 0;

class PhysicsLayerManager {
public:
    static PhysicsLayerManager& Get() {
        static PhysicsLayerManager instance;
        return instance;
    }

    // Register a layer by name, returns the layer index (0-31)
    // Returns the same index if already registered
    uint32_t RegisterLayer(const std::string& name);
    
    // Get layer index by name (-1 if not found)
    int32_t GetLayerIndex(const std::string& name) const;
    
    // Get layer mask (single bit) by name (0 if not found)
    uint32_t GetLayerMask(const std::string& name) const;
    
    // Get layer name by index (empty if invalid)
    std::string GetLayerName(uint32_t index) const;
    
    // Get all registered layer names
    const std::vector<std::string>& GetAllLayers() const { return m_LayerNames; }
    
    // Check if a layer exists
    bool HasLayer(const std::string& name) const;
    
    // Get number of registered layers
    uint32_t GetLayerCount() const { return static_cast<uint32_t>(m_LayerNames.size()); }
    
    // Combine multiple layer names into a mask
    uint32_t GetCombinedMask(const std::vector<std::string>& layerNames) const;
    
    // Clear all layers (for scene reload)
    void Clear();
    
    // Register default layers (called on init)
    void RegisterDefaults();

private:
    PhysicsLayerManager() { RegisterDefaults(); }
    ~PhysicsLayerManager() = default;
    
    std::unordered_map<std::string, uint32_t> m_NameToIndex;
    std::vector<std::string> m_LayerNames;
};

// Convenience functions (forward to singleton)
inline uint32_t RegisterLayer(const std::string& name) { return PhysicsLayerManager::Get().RegisterLayer(name); }
inline int32_t GetLayerIndex(const std::string& name) { return PhysicsLayerManager::Get().GetLayerIndex(name); }
inline uint32_t GetLayerMask(const std::string& name) { return PhysicsLayerManager::Get().GetLayerMask(name); }

} // namespace PhysicsLayers

