#include "PrefabBinaryLoader.h"
#include "core/assets/BinaryFormats.h"
#include "core/vfs/FileSystem.h"
#include <fstream>
#include <cstring>
#include <iostream>

namespace binary {

namespace {

// Helper to read a value from data
template<typename T>
bool ReadValue(const uint8_t* data, size_t dataSize, size_t& offset, T& outValue) {
    if (offset + sizeof(T) > dataSize) return false;
    std::memcpy(&outValue, data + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

// Helper to read a string from the string table
std::string ReadStringFromTable(const uint8_t* data, size_t dataSize, size_t stringTableOffset, uint32_t stringIndex) {
    // Find the string at stringIndex by counting through null terminators
    size_t currentPos = stringTableOffset;
    uint32_t currentIndex = 0;
    while (currentIndex < stringIndex && currentPos < dataSize) {
        while (currentPos < dataSize && data[currentPos] != 0) {
            ++currentPos;
        }
        ++currentPos; // skip null
        ++currentIndex;
    }
    
    if (currentPos >= dataSize) return "";
    
    size_t end = currentPos;
    while (end < dataSize && data[end] != 0) {
        ++end;
    }
    return std::string(reinterpret_cast<const char*>(data + currentPos), end - currentPos);
}

} // namespace

bool PrefabBinaryLoader::Load(const std::vector<uint8_t>& data, PrefabAsset& outAsset) {
    if (data.size() < sizeof(PrefabBinaryHeader)) {
        std::cerr << "[PrefabBinaryLoader] Data too small for header\n";
        return false;
    }
    
    size_t offset = 0;
    const uint8_t* ptr = data.data();
    size_t dataSize = data.size();
    
    // Read header
    PrefabBinaryHeader header;
    if (!ReadValue(ptr, dataSize, offset, header)) {
        std::cerr << "[PrefabBinaryLoader] Failed to read header\n";
        return false;
    }
    
    // Validate magic
    if (header.base.magic != PREFAB_MAGIC) {
        std::cerr << "[PrefabBinaryLoader] Invalid magic number\n";
        return false;
    }
    
    // Check version - we support both v1 (JSON blobs) and v2 (binary components)
    if (header.base.version > PREFAB_VERSION) {
        std::cerr << "[PrefabBinaryLoader] Version too new: got " << header.base.version 
                  << ", max supported " << PREFAB_VERSION << "\n";
        return false;
    }
    
    // V2 format with binary components should use RuntimePrefabInstantiator directly
    // This loader only handles legacy v1 format (JSON blobs)
    if (header.base.version == 2 && (header.base.flags & 1) == 0) {
        std::cerr << "[PrefabBinaryLoader] V2 binary format detected. "
                  << "Use RuntimePrefabInstantiator::Instantiate() instead.\n";
        return false;
    }
    
    // Read prefab GUID
    uint64_t guidHigh, guidLow;
    if (!ReadValue(ptr, dataSize, offset, guidHigh) || 
        !ReadValue(ptr, dataSize, offset, guidLow)) {
        std::cerr << "[PrefabBinaryLoader] Failed to read prefab GUID\n";
        return false;
    }
    outAsset.Guid.high = guidHigh;
    outAsset.Guid.low = guidLow;
    
    // Read prefab name index
    uint32_t nameIndex;
    if (!ReadValue(ptr, dataSize, offset, nameIndex)) {
        std::cerr << "[PrefabBinaryLoader] Failed to read name index\n";
        return false;
    }
    outAsset.Name = ReadStringFromTable(ptr, dataSize, header.stringTableOffset, nameIndex);
    
    // Read entity records
    struct EntityRecord {
        uint64_t guidHigh;
        uint64_t guidLow;
        uint64_t parentGuidHigh;
        uint64_t parentGuidLow;
        uint32_t nameIndex;
        uint32_t componentJsonOffset;
        uint32_t componentJsonSize;
        uint32_t childCount;
    };
    
    std::vector<EntityRecord> records(header.entityCount);
    for (uint32_t i = 0; i < header.entityCount; ++i) {
        if (!ReadValue(ptr, dataSize, offset, records[i])) {
            std::cerr << "[PrefabBinaryLoader] Failed to read entity record " << i << "\n";
            return false;
        }
    }
    
    // Set root GUID from root entity
    if (header.rootEntityIndex < records.size()) {
        outAsset.RootGuid.high = records[header.rootEntityIndex].guidHigh;
        outAsset.RootGuid.low = records[header.rootEntityIndex].guidLow;
    }
    
    // Parse entities into unified JSON format
    outAsset.Entities = nlohmann::json::array();
    
    for (const auto& rec : records) {
        nlohmann::json entityJson;
        
        // Set entity GUID
        ClaymoreGUID guid;
        guid.high = rec.guidHigh;
        guid.low = rec.guidLow;
        entityJson["guid"] = guid;
        
        // Set parent GUID if non-zero
        if (rec.parentGuidHigh != 0 || rec.parentGuidLow != 0) {
            ClaymoreGUID parentGuid;
            parentGuid.high = rec.parentGuidHigh;
            parentGuid.low = rec.parentGuidLow;
            entityJson["_parentGuid"] = parentGuid;
        }
        
        // Read name from string table
        entityJson["name"] = ReadStringFromTable(ptr, dataSize, header.stringTableOffset, rec.nameIndex);
        
        // Read and merge component JSON into entity
        if (rec.componentJsonOffset + rec.componentJsonSize <= dataSize) {
            std::string jsonStr(
                reinterpret_cast<const char*>(ptr + rec.componentJsonOffset),
                rec.componentJsonSize
            );
            const auto firstNonWs = jsonStr.find_first_not_of(" \t\r\n");
            const bool looksLikeJson =
                firstNonWs != std::string::npos &&
                (jsonStr[firstNonWs] == '{' || jsonStr[firstNonWs] == '[');
            try {
                if (looksLikeJson) {
                    nlohmann::json components = nlohmann::json::parse(jsonStr);
                    // Merge components into entity (unified format stores components at entity root)
                    for (auto it = components.begin(); it != components.end(); ++it) {
                        entityJson[it.key()] = it.value();
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "[PrefabBinaryLoader] Failed to parse component JSON: " << e.what() << "\n";
            }
        }
        
        outAsset.Entities.push_back(std::move(entityJson));
    }
    
    std::cout << "[PrefabBinaryLoader] Loaded prefab: " << outAsset.Name 
              << " (" << outAsset.EntityCount() << " entities)\n";
    return true;
}

bool PrefabBinaryLoader::Load(const std::string& filepath, PrefabAsset& outAsset) {
    std::vector<uint8_t> data;
    
    // Try VFS first
    if (FileSystem::Instance().ReadFile(filepath, data)) {
        return Load(data, outAsset);
    }
    
    // Try direct file read (editor-only fallback)
    if (!FileSystem::Instance().IsDiskFallbackAllowed()) {
        std::cerr << "[PrefabBinaryLoader] Failed to read prefab from VFS: " << filepath << "\n";
        return false;
    }

    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[PrefabBinaryLoader] Failed to open file: " << filepath << "\n";
        return false;
    }
    
    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    data.resize(size);
    file.read(reinterpret_cast<char*>(data.data()), size);
    
    return Load(data, outAsset);
}

} // namespace binary

