#include "PakArchive.h"
#include <fstream>
#include <filesystem>
#include <cstring>
#include <algorithm>
#include <iostream>

namespace {
    constexpr uint32_t kPakVersion1 = 1;  // Legacy uncompressed
    constexpr uint32_t kPakVersion2 = 2;  // With compression support
    constexpr char kMagic[4] = {'C','L','Y','P'};
    
    // Flag bits
    constexpr uint32_t kFlagCompressed = 0x1;
}

void PakArchive::AddFile(const std::string& virtualPath, const std::vector<uint8_t>& data) {
    m_Files.push_back({ virtualPath, data });
}

uint64_t PakArchive::GetTotalUncompressedSize() const {
    uint64_t total = 0;
    for (const auto& f : m_Files) {
        total += f.data.size();
    }
    return total;
}

// Simple LZ-style compression (lightweight, no external deps)
// Uses run-length encoding with literal fallback
std::vector<uint8_t> PakArchive::CompressData(const std::vector<uint8_t>& input) {
    if (input.empty()) return {};
    
    std::vector<uint8_t> output;
    output.reserve(input.size());
    
    size_t i = 0;
    while (i < input.size()) {
        // Look for runs of same byte
        size_t runLen = 1;
        while (i + runLen < input.size() && input[i + runLen] == input[i] && runLen < 127) {
            runLen++;
        }
        
        if (runLen >= 4) {
            // Encode as run: 0x80 | length, value
            output.push_back(static_cast<uint8_t>(0x80 | runLen));
            output.push_back(input[i]);
            i += runLen;
        } else {
            // Count literals until we hit a run
            size_t literalStart = i;
            size_t literalLen = 0;
            while (i < input.size() && literalLen < 127) {
                size_t nextRun = 1;
                while (i + nextRun < input.size() && input[i + nextRun] == input[i] && nextRun < 4) {
                    nextRun++;
                }
                if (nextRun >= 4) break;
                i++;
                literalLen++;
            }
            if (literalLen > 0) {
                // Encode literals: length, bytes...
                output.push_back(static_cast<uint8_t>(literalLen));
                for (size_t j = 0; j < literalLen; j++) {
                    output.push_back(input[literalStart + j]);
                }
            }
        }
    }
    
    return output;
}

std::vector<uint8_t> PakArchive::DecompressData(const std::vector<uint8_t>& input, uint64_t uncompressedSize) {
    std::vector<uint8_t> output;
    output.reserve(static_cast<size_t>(uncompressedSize));
    
    size_t i = 0;
    while (i < input.size() && output.size() < uncompressedSize) {
        uint8_t ctrl = input[i++];
        if (ctrl & 0x80) {
            // Run
            size_t runLen = ctrl & 0x7F;
            if (i >= input.size()) break;
            uint8_t val = input[i++];
            for (size_t j = 0; j < runLen && output.size() < uncompressedSize; j++) {
                output.push_back(val);
            }
        } else {
            // Literals
            size_t literalLen = ctrl;
            for (size_t j = 0; j < literalLen && i < input.size() && output.size() < uncompressedSize; j++) {
                output.push_back(input[i++]);
            }
        }
    }
    
    return output;
}

bool PakArchive::SaveToFile(const std::string& pakPath, bool compress) const {
    std::ofstream out(pakPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;

    // Write header
    out.write(kMagic, 4);
    uint32_t version = compress ? kPakVersion2 : kPakVersion1;
    out.write(reinterpret_cast<const char*>(&version), sizeof(version));
    
    if (version == kPakVersion2) {
        uint32_t flags = compress ? kFlagCompressed : 0;
        out.write(reinterpret_cast<const char*>(&flags), sizeof(flags));
    }

    uint32_t fileCount = static_cast<uint32_t>(m_Files.size());
    out.write(reinterpret_cast<const char*>(&fileCount), sizeof(fileCount));

    // Prepare compressed data if needed
    std::vector<std::vector<uint8_t>> compressedData;
    if (compress) {
        compressedData.reserve(m_Files.size());
        for (const auto& f : m_Files) {
            auto compressed = CompressData(f.data);
            // Only use compression if it actually saves space
            if (compressed.size() < f.data.size()) {
                compressedData.push_back(std::move(compressed));
            } else {
                compressedData.push_back({}); // Empty = use original
            }
        }
    }

    // Calculate table size
    // v1: 4 + pathLen + 8 + 8 per file
    // v2: 4 + pathLen + 8 + 8 + 8 + 4 per file
    uint64_t tableSize = 0;
    for (const auto& f : m_Files) {
        tableSize += 4 + static_cast<uint32_t>(f.path.size());
        if (version == kPakVersion2) {
            tableSize += 8 + 8 + 8 + 4; // offset, compressedSize, uncompressedSize, flags
        } else {
            tableSize += 8 + 8; // offset, size
        }
    }
    
    uint64_t headerSize = 4 + 4 + (version == kPakVersion2 ? 4 : 0) + 4 + tableSize;
    uint64_t runningOffset = headerSize;
    
    // Write table
    for (size_t idx = 0; idx < m_Files.size(); idx++) {
        const auto& f = m_Files[idx];
        uint32_t pathLen = static_cast<uint32_t>(f.path.size());
        out.write(reinterpret_cast<const char*>(&pathLen), sizeof(pathLen));
        out.write(f.path.data(), pathLen);
        out.write(reinterpret_cast<const char*>(&runningOffset), sizeof(runningOffset));
        
        if (version == kPakVersion2) {
            bool useCompressed = compress && !compressedData[idx].empty();
            uint64_t compSize = useCompressed ? compressedData[idx].size() : f.data.size();
            uint64_t uncompSize = f.data.size();
            uint32_t fileFlags = useCompressed ? kFlagCompressed : 0;
            
            out.write(reinterpret_cast<const char*>(&compSize), sizeof(compSize));
            out.write(reinterpret_cast<const char*>(&uncompSize), sizeof(uncompSize));
            out.write(reinterpret_cast<const char*>(&fileFlags), sizeof(fileFlags));
            runningOffset += compSize;
        } else {
            uint64_t size = static_cast<uint64_t>(f.data.size());
            out.write(reinterpret_cast<const char*>(&size), sizeof(size));
            runningOffset += size;
        }
    }

    // Write blobs
    for (size_t idx = 0; idx < m_Files.size(); idx++) {
        const auto& f = m_Files[idx];
        if (version == kPakVersion2 && compress && !compressedData[idx].empty()) {
            // Write compressed data
            out.write(reinterpret_cast<const char*>(compressedData[idx].data()), 
                     static_cast<std::streamsize>(compressedData[idx].size()));
        } else {
            // Write original data
            if (!f.data.empty()) {
                out.write(reinterpret_cast<const char*>(f.data.data()), 
                         static_cast<std::streamsize>(f.data.size()));
            }
        }
    }

    return true;
}

bool PakArchive::Open(const std::string& pakPath) {
    m_Index.clear();
    m_PakPath = pakPath;
    m_IsCompressed = false;
    m_Version = kPakVersion1;

    std::ifstream in(pakPath, std::ios::binary);
    if (!in.is_open()) return false;

    char magic[4] = {};
    in.read(magic, 4);
    if (std::memcmp(magic, kMagic, 4) != 0) return false;

    uint32_t version = 0;
    in.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (version != kPakVersion1 && version != kPakVersion2) return false;
    m_Version = version;
    
    if (version == kPakVersion2) {
        uint32_t flags = 0;
        in.read(reinterpret_cast<char*>(&flags), sizeof(flags));
        m_IsCompressed = (flags & kFlagCompressed) != 0;
    }

    uint32_t fileCount = 0;
    in.read(reinterpret_cast<char*>(&fileCount), sizeof(fileCount));

    for (uint32_t i = 0; i < fileCount; ++i) {
        uint32_t pathLen = 0;
        in.read(reinterpret_cast<char*>(&pathLen), sizeof(pathLen));
        std::string path;
        path.resize(pathLen);
        if (pathLen) in.read(path.data(), pathLen);
        
        Entry e{};
        in.read(reinterpret_cast<char*>(&e.offset), sizeof(e.offset));
        
        if (version == kPakVersion2) {
            in.read(reinterpret_cast<char*>(&e.compressedSize), sizeof(e.compressedSize));
            in.read(reinterpret_cast<char*>(&e.uncompressedSize), sizeof(e.uncompressedSize));
            uint32_t fileFlags = 0;
            in.read(reinterpret_cast<char*>(&fileFlags), sizeof(fileFlags));
            e.compressed = (fileFlags & kFlagCompressed) != 0;
        } else {
            // v1 format: just size
            uint64_t size = 0;
            in.read(reinterpret_cast<char*>(&size), sizeof(size));
            e.compressedSize = size;
            e.uncompressedSize = size;
            e.compressed = false;
        }
        
        m_Index.emplace(std::move(path), e);
    }
    return true;
}

bool PakArchive::Contains(const std::string& virtualPath) const {
    return m_Index.find(virtualPath) != m_Index.end();
}

bool PakArchive::ReadFile(const std::string& virtualPath, std::vector<uint8_t>& outData) const {
    auto it = m_Index.find(virtualPath);
    if (it == m_Index.end()) return false;
    const Entry& e = it->second;

    std::ifstream in(m_PakPath, std::ios::binary);
    if (!in.is_open()) return false;
    in.seekg(static_cast<std::streamoff>(e.offset), std::ios::beg);
    
    std::vector<uint8_t> rawData(static_cast<size_t>(e.compressedSize));
    if (e.compressedSize != 0) {
        in.read(reinterpret_cast<char*>(rawData.data()), static_cast<std::streamsize>(e.compressedSize));
    }
    
    if (e.compressed) {
        outData = DecompressData(rawData, e.uncompressedSize);
    } else {
        outData = std::move(rawData);
    }
    
    return true;
}


