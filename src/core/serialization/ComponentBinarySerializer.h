#pragma once

#include "core/assets/BinaryFormats.h"
#include "core/ecs/EntityData.h"
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <functional>

namespace binary {

/**
 * Shared binary serialization for components
 * Used by both SceneBinaryWriter and PrefabBinaryWriter to ensure
 * exact parity between scene and prefab binary formats.
 * 
 * The loader counterpart (ComponentBinaryDeserializer) is used by
 * both SceneBinaryLoader and RuntimePrefabInstantiator.
 */

// Write context for component serialization
struct ComponentWriteContext {
    std::vector<uint8_t>& data;
    std::vector<std::string>& stringTable;
    std::function<uint32_t(const std::string&)> addString;
    class Scene* scene = nullptr;     // Optional: for script/entity reference serialization
    EntityID currentEntityId = INVALID_ENTITY_ID;
    
    size_t Position() const { return data.size(); }
    
    template<typename T>
    void Write(const T& value) {
        size_t offset = data.size();
        data.resize(offset + sizeof(T));
        std::memcpy(data.data() + offset, &value, sizeof(T));
    }
    
    void Write(const void* ptr, size_t size) {
        size_t offset = data.size();
        data.resize(offset + size);
        std::memcpy(data.data() + offset, ptr, size);
    }
    
    uint32_t AddString(const std::string& s) {
        return addString(s);
    }
};

// Write a single component to binary format
// Returns the size of the written component data
size_t WriteComponentBinary(ComponentWriteContext& ctx, const EntityData* data, ComponentTypeId typeId);

// Get list of components present on an entity
std::vector<ComponentTypeId> GetEntityComponents(const EntityData* data);

} // namespace binary

