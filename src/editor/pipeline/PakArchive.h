#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// .pak archive format (version 2 with optional compression):
// [magic: "CLYP" 4 bytes]
// [version: uint32 = 2]
// [flags: uint32] - bit 0: compressed
// [fileCount: uint32]
// repeated fileCount times:
//   [pathLen: uint32]
//   [path bytes UTF-8]
//   [offset: uint64]
//   [compressedSize: uint64] - size in pak (may be compressed)
//   [uncompressedSize: uint64] - original size
//   [flags: uint32] - per-file: bit 0 = compressed
// [blob data...]

class PakArchive {
public:
    struct Entry {
        uint64_t offset = 0;
        uint64_t compressedSize = 0;
        uint64_t uncompressedSize = 0;
        bool compressed = false;
    };

    // Writer API
    void AddFile(const std::string& virtualPath, const std::vector<uint8_t>& data);
    bool SaveToFile(const std::string& pakPath, bool compress = false) const;

    // Reader API
    bool Open(const std::string& pakPath);
    bool Contains(const std::string& virtualPath) const;
    bool ReadFile(const std::string& virtualPath, std::vector<uint8_t>& outData) const;
    
    // Stats for debugging/display
    size_t GetFileCount() const { return m_Files.size(); }
    uint64_t GetTotalUncompressedSize() const;

private:
    struct FileData {
        std::string path;
        std::vector<uint8_t> data;
    };

    // For writer
    std::vector<FileData> m_Files;

    // For reader
    std::string m_PakPath;
    std::unordered_map<std::string, Entry> m_Index;
    bool m_IsCompressed = false;
    uint32_t m_Version = 1;
    
    // Compression helpers (simple RLE-like for now, can upgrade to zlib later)
    static std::vector<uint8_t> CompressData(const std::vector<uint8_t>& input);
    static std::vector<uint8_t> DecompressData(const std::vector<uint8_t>& input, uint64_t uncompressedSize);
};


