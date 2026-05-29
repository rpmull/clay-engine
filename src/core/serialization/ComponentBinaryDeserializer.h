#pragma once

#include "core/assets/BinaryFormats.h"
#include "core/ecs/EntityData.h"
#include <vector>
#include <string>
#include <cstdint>
#include <functional>

namespace binary {

/**
 * Read context for binary component deserialization.
 * Used by prefab and model instantiation at runtime.
 */
struct ComponentReadContext {
    const uint8_t* data;
    size_t dataSize;
    size_t offset;
    
    // String table access
    std::function<std::string(uint32_t)> readString;
    
    bool Read(void* dst, size_t count) {
        if (offset + count > dataSize) return false;
        std::memcpy(dst, data + offset, count);
        offset += count;
        return true;
    }
    
    template<typename T>
    bool Read(T& value) {
        return Read(&value, sizeof(T));
    }
    
    std::string ReadString(uint32_t index) {
        return readString(index);
    }
    
    void Seek(size_t newOffset) {
        offset = newOffset;
    }
};

/**
 * Deserialize a component from binary data into an EntityData structure.
 * This mirrors the binary format written by ComponentBinarySerializer.
 * 
 * @param ctx Read context positioned at the component data
 * @param data Target EntityData to populate
 * @param typeId Component type to deserialize
 * @param dataSize Size of component data in bytes
 * @return true on success
 */
bool ReadComponentBinary(ComponentReadContext& ctx, EntityData* data, ComponentTypeId typeId, uint32_t dataSize);

} // namespace binary


