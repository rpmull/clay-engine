#pragma once
#include "VirtualFS.h"
#include <unordered_map>
#include <cstdint>

// Read-only PAK file reader for runtime
// This is a minimal implementation that only reads from .pak files
// No disk fallback, no writing - pure runtime asset access
class PakReader : public IVirtualFS {
public:
    PakReader() = default;
    ~PakReader() override = default;

    // Open a pak file for reading
    bool Open(const std::string& pakPath);
    
    // Check if pak is open
    bool IsOpen() const { return m_IsOpen; }
    
    // Get number of files in pak
    size_t GetFileCount() const { return m_Index.size(); }
    
    // IVirtualFS interface
    bool ReadFile(const std::string& path, std::vector<uint8_t>& outData) override;
    bool Exists(const std::string& path) override;
    std::vector<std::string> ListAllFiles() override;

private:
    struct Entry {
        uint64_t offset = 0;
        uint64_t compressedSize = 0;
        uint64_t uncompressedSize = 0;
        bool compressed = false;
    };

    std::string m_PakPath;
    std::unordered_map<std::string, Entry> m_Index;
    bool m_IsOpen = false;
    bool m_IsCompressed = false;
    uint32_t m_Version = 1;

    // Try to find file with various path normalizations
    const Entry* FindEntry(const std::string& path) const;
    
    // Decompression (simple RLE)
    static std::vector<uint8_t> DecompressData(const std::vector<uint8_t>& input, uint64_t uncompressedSize);
};







