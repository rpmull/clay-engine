// PhysicsLayerManager.cpp
#include "PhysicsLayerManager.h"
#include <iostream>

namespace PhysicsLayers {

uint32_t PhysicsLayerManager::RegisterLayer(const std::string& name) {
    // Check if already registered
    auto it = m_NameToIndex.find(name);
    if (it != m_NameToIndex.end()) {
        return it->second;
    }
    
    // Check if we have room for more layers
    if (m_LayerNames.size() >= MAX_LAYERS) {
        std::cerr << "[PhysicsLayers] Cannot register layer '" << name 
                  << "': maximum of " << MAX_LAYERS << " layers reached\n";
        return 0; // Return default layer
    }
    
    uint32_t index = static_cast<uint32_t>(m_LayerNames.size());
    m_LayerNames.push_back(name);
    m_NameToIndex[name] = index;
    
    std::cout << "[PhysicsLayers] Registered layer '" << name << "' at index " << index << "\n";
    return index;
}

int32_t PhysicsLayerManager::GetLayerIndex(const std::string& name) const {
    auto it = m_NameToIndex.find(name);
    if (it != m_NameToIndex.end()) {
        return static_cast<int32_t>(it->second);
    }
    return -1;
}

uint32_t PhysicsLayerManager::GetLayerMask(const std::string& name) const {
    auto it = m_NameToIndex.find(name);
    if (it != m_NameToIndex.end()) {
        return 1u << it->second;
    }
    return 0;
}

std::string PhysicsLayerManager::GetLayerName(uint32_t index) const {
    if (index < m_LayerNames.size()) {
        return m_LayerNames[index];
    }
    return "";
}

bool PhysicsLayerManager::HasLayer(const std::string& name) const {
    return m_NameToIndex.find(name) != m_NameToIndex.end();
}

uint32_t PhysicsLayerManager::GetCombinedMask(const std::vector<std::string>& layerNames) const {
    uint32_t mask = 0;
    for (const auto& name : layerNames) {
        mask |= GetLayerMask(name);
    }
    return mask;
}

void PhysicsLayerManager::Clear() {
    m_NameToIndex.clear();
    m_LayerNames.clear();
    RegisterDefaults();
}

void PhysicsLayerManager::RegisterDefaults() {
    // Register some common default layers
    // Users can add more via the editor or scripts
    RegisterLayer("Default");   // 0 - General geometry
    RegisterLayer("Player");    // 1 - Player character
    RegisterLayer("Enemy");     // 2 - Enemies
    RegisterLayer("Terrain");   // 3 - Terrain/ground
    RegisterLayer("Projectile");// 4 - Projectiles
    RegisterLayer("Trigger");   // 5 - Trigger volumes
    RegisterLayer("Dynamic");   // 6 - Dynamic objects
    RegisterLayer("Static");    // 7 - Static world geometry
}

} // namespace PhysicsLayers

