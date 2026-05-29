#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "core/assets/BinaryFormats.h"
#include "core/rendering/MaterialAsset.h"

namespace binary {

// Write a material to binary format (.matbin)
// Editor-only - generates binaries for play mode and export
class MaterialBinaryWriter {
public:
    // Write material from asset description to file
    static bool Write(const MaterialAssetDesc& desc, const std::string& outputPath);
    
    // Write material to memory buffer
    static bool WriteToMemory(const MaterialAssetDesc& desc, std::vector<uint8_t>& outData);
    
    // Convert .mat JSON to .matbin binary
    static bool ConvertMaterial(const std::string& sourcePath, const std::string& outputPath);

private:
    struct WriteContext {
        std::vector<uint8_t> data;
        std::vector<std::string> strings;
        std::unordered_map<std::string, uint32_t> stringLookup;
        
        void Write(const void* src, size_t count);
        uint32_t AddString(const std::string& str);
        
        template<typename T>
        void Write(const T& val) { Write(&val, sizeof(T)); }
        
        size_t Position() const { return data.size(); }
        void WriteAt(size_t offset, const void* src, size_t count);
    };
};

} // namespace binary


