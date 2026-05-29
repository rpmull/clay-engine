#pragma once
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include "core/assets/BinaryFormats.h"

class Material;

namespace binary {

// Load a material from binary format (.matbin)
// Runtime-safe loader - no JSON, no editor dependencies
class MaterialBinaryLoader {
public:
    // Load material from file path (uses VFS)
    // If skinned=true, creates a SkinnedPBRMaterial with vs_pbr_skinned shader
    static std::shared_ptr<Material> Load(const std::string& path, bool skinned = false);
    
    // Load material from memory buffer
    static std::shared_ptr<Material> LoadFromMemory(const uint8_t* data, size_t size, bool skinned = false);
    
    // Quick validation
    static bool Validate(const std::string& path);

private:
    struct LoadContext {
        const uint8_t* data = nullptr;
        size_t size = 0;
        size_t pos = 0;
        std::vector<std::string> strings;
        
        bool Read(void* dst, size_t count);
        std::string ReadString(uint32_t offset) const;
        
        template<typename T>
        bool Read(T& out) { return Read(&out, sizeof(T)); }
    };
    
    static bool LoadHeader(LoadContext& ctx, MaterialBinaryHeader& header);
    static bool LoadStringTable(LoadContext& ctx, const MaterialBinaryHeader& header);
};

} // namespace binary


