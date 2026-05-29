#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <nlohmann/json.hpp>
#include "core/assets/AssetReference.h"
#include "core/ecs/Entity.h"

//==============================================================================
// PrefabAsset - Unified Prefab Asset Format
//==============================================================================
// A prefab asset stores a hierarchy of entities that can be instantiated into
// a scene. The entity data uses the EXACT same JSON format as 
// Serializer::SerializeEntity() produces. This ensures:
//
// 1. Component parity: Any component that can be serialized to a scene can
//    automatically be saved in a prefab.
// 2. Single code path: No separate prefab serialization logic to maintain.
// 3. Future compatibility: New components work in prefabs automatically.
//
// File Format (.prefab):
// {
//     "version": "2.0",
//     "guid": <ClaymoreGUID>,
//     "name": "PrefabName",
//     "rootGuid": <ClaymoreGUID>,
//     "entities": [
//         { /* Serializer::SerializeEntity output */ },
//         { /* ... */ }
//     ]
// }
//
// Each entity in the "entities" array has the exact format from SerializeEntity:
// {
//     "id": <EntityID>,       // (ignored on load, new IDs assigned)
//     "name": "EntityName",
//     "guid": <ClaymoreGUID>, // Stable identity
//     "transform": { ... },
//     "mesh": { ... },
//     // ... all other components as serialized by Serializer
// }
//==============================================================================

struct PrefabAsset {
    //--------------------------------------------------------------------------
    // Prefab identity
    //--------------------------------------------------------------------------
    ClaymoreGUID Guid;          // Unique prefab asset GUID
    std::string Name;           // Display name
    ClaymoreGUID RootGuid;      // GUID of the root entity
    
    //--------------------------------------------------------------------------
    // Entity storage - unified format
    // Each element is the JSON output of Serializer::SerializeEntity()
    //--------------------------------------------------------------------------
    nlohmann::json Entities;    // JSON array of serialized entities
    
    //--------------------------------------------------------------------------
    // Raw JSON - full prefab file including assetMap for GUID resolution
    // This enables prefabs to load meshes just like scenes do
    //--------------------------------------------------------------------------
    nlohmann::json Raw;         // Complete prefab JSON including assetMap
    
    //--------------------------------------------------------------------------
    // Helper methods
    //--------------------------------------------------------------------------
    
    // Get number of entities
    size_t EntityCount() const {
        if (Entities.is_array()) return Entities.size();
        return 0;
    }
    
    // Check if prefab is valid (has entities)
    bool IsValid() const {
        return Entities.is_array() && !Entities.empty();
    }
    
    // Find entity JSON by GUID
    const nlohmann::json* FindEntityByGuid(const ClaymoreGUID& guid) const {
        if (!Entities.is_array()) return nullptr;
        for (const auto& e : Entities) {
            if (e.contains("guid")) {
                ClaymoreGUID g;
                try { e.at("guid").get_to(g); } catch (...) { continue; }
                if (g == guid) return &e;
            }
        }
        return nullptr;
    }
    
    // Find entity JSON by GUID (mutable)
    nlohmann::json* FindEntityByGuid(const ClaymoreGUID& guid) {
        if (!Entities.is_array()) return nullptr;
        for (auto& e : Entities) {
            if (e.contains("guid")) {
                ClaymoreGUID g;
                try { e.at("guid").get_to(g); } catch (...) { continue; }
                if (g == guid) return &e;
            }
        }
        return nullptr;
    }
    
    // Find root entity JSON
    const nlohmann::json* FindRootEntity() const {
        return FindEntityByGuid(RootGuid);
    }
    
    // Clear all data
    void Clear() {
        Guid = {};
        Name.clear();
        RootGuid = {};
        Entities = nlohmann::json::array();
    }
};

//==============================================================================
// PrefabIO - Prefab File Operations
//==============================================================================
// Simple I/O functions that use Serializer format directly.

namespace PrefabIO {
    // Load a prefab from disk
    bool LoadPrefab(const std::string& path, PrefabAsset& out);

    // Load only the authoring JSON, bypassing play-mode binary resolution.
    bool LoadPrefabSource(const std::string& path, PrefabAsset& out);
    
    // Save a prefab to disk
    bool SavePrefab(const std::string& path, const PrefabAsset& in);
}

