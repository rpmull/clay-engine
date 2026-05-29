#include "PakReader.h"
#include <fstream>
#include <cstring>
#include <iostream>

namespace {
    constexpr uint32_t kPakVersion1 = 1;
    constexpr uint32_t kPakVersion2 = 2;
    constexpr char kMagic[4] = {'C','L','Y','P'};
    constexpr uint32_t kFlagCompressed = 0x1;
}

bool PakReader::Open(const std::string& pakPath) {
    m_Index.clear();
    m_PakPath = pakPath;
    m_IsOpen = false;
    m_IsCompressed = false;
    m_Version = kPakVersion1;

    std::ifstream in(pakPath, std::ios::binary);
    if (!in.is_open()) {
        std::cerr << "[PakReader] Failed to open: " << pakPath << std::endl;
        return false;
    }

    // Read and verify magic
    char magic[4] = {};
    in.read(magic, 4);
    if (std::memcmp(magic, kMagic, 4) != 0) {
        std::cerr << "[PakReader] Invalid magic in: " << pakPath << std::endl;
        return false;
    }

    // Read version
    uint32_t version = 0;
    in.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (version != kPakVersion1 && version != kPakVersion2) {
        std::cerr << "[PakReader] Unsupported version " << version << " in: " << pakPath << std::endl;
        return false;
    }
    m_Version = version;
    
    // Read flags for v2
    if (version == kPakVersion2) {
        uint32_t flags = 0;
        in.read(reinterpret_cast<char*>(&flags), sizeof(flags));
        m_IsCompressed = (flags & kFlagCompressed) != 0;
    }

    // Read file count
    uint32_t fileCount = 0;
    in.read(reinterpret_cast<char*>(&fileCount), sizeof(fileCount));

    // Read index
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
            // v1 format: just size (no compression)
            uint64_t size = 0;
            in.read(reinterpret_cast<char*>(&size), sizeof(size));
            e.compressedSize = size;
            e.uncompressedSize = size;
            e.compressed = false;
        }
        
        m_Index.emplace(std::move(path), e);
    }

    m_IsOpen = true;
    std::cout << "[PakReader] Opened: " << pakPath << " (" << fileCount << " files)" << std::endl;
    return true;
}

const PakReader::Entry* PakReader::FindEntry(const std::string& path) const {
    std::string key = IVirtualFS::NormalizePath(path);
    
    // Try exact match first
    auto it = m_Index.find(key);
    if (it != m_Index.end()) return &it->second;
    
    // Helper lambda to try a prefix
    auto tryPrefix = [&](const char* prefix) -> const Entry* {
        auto pos = key.find(prefix);
        if (pos != std::string::npos) {
            std::string rel = key.substr(pos);
            auto found = m_Index.find(rel);
            if (found != m_Index.end()) return &found->second;
        }
        return nullptr;
    };
    
    // Try stripping to known prefixes
    // Order matters: more specific prefixes first
    static const char* kKnownPrefixes[] = {
        "assets/",
        "shaders/",
        "scenes/",
        "resources/",
        ".bin/"
    };
    
    for (const char* prefix : kKnownPrefixes) {
        if (const Entry* e = tryPrefix(prefix)) return e;
    }
    
    // Special case: shaders/compiled/windows/ variant
    auto pos = key.find("shaders/");
    if (pos != std::string::npos) {
        std::string rel = key.substr(pos);
        if (rel.rfind("shaders/", 0) == 0 && rel.find(".bin") != std::string::npos) {
            std::string candidate = "shaders/compiled/windows/" + rel.substr(8);
            it = m_Index.find(candidate);
            if (it != m_Index.end()) return &it->second;
        }
    }
    
    // Case-insensitive fallback: try lowercase key against lowercase index
    // This handles paths like "Assets/Model.png" when PAK has "assets/model.png"
    std::string lowerKey = IVirtualFS::NormalizePathLowercase(path);
    for (const auto& [indexPath, entry] : m_Index) {
        std::string lowerIndex = IVirtualFS::NormalizePathLowercase(indexPath);
        if (lowerKey == lowerIndex) {
            return &entry;
        }
        // Also try with prefix stripping
        for (const char* prefix : kKnownPrefixes) {
            auto prefixPos = lowerKey.find(prefix);
            if (prefixPos != std::string::npos) {
                if (lowerKey.substr(prefixPos) == lowerIndex) {
                    return &entry;
                }
            }
        }
    }
    
    return nullptr;
}

bool PakReader::ReadFile(const std::string& path, std::vector<uint8_t>& outData) {
    if (!m_IsOpen) return false;
    
    const Entry* e = FindEntry(path);
    if (!e) return false;

    std::ifstream in(m_PakPath, std::ios::binary);
    if (!in.is_open()) return false;
    
    in.seekg(static_cast<std::streamoff>(e->offset), std::ios::beg);
    
    std::vector<uint8_t> rawData(static_cast<size_t>(e->compressedSize));
    if (e->compressedSize != 0) {
        in.read(reinterpret_cast<char*>(rawData.data()), static_cast<std::streamsize>(e->compressedSize));
    }
    
    if (e->compressed) {
        outData = DecompressData(rawData, e->uncompressedSize);
    } else {
        outData = std::move(rawData);
    }
    
    return true;
}

bool PakReader::Exists(const std::string& path) {
    if (!m_IsOpen) return false;
    return FindEntry(path) != nullptr;
}

std::vector<std::string> PakReader::ListAllFiles() {
    std::vector<std::string> files;
    files.reserve(m_Index.size());
    for (const auto& [path, entry] : m_Index) {
        files.push_back(path);
    }
    return files;
}

std::vector<uint8_t> PakReader::DecompressData(const std::vector<uint8_t>& input, uint64_t uncompressedSize) {
    std::vector<uint8_t> output;
    output.reserve(static_cast<size_t>(uncompressedSize));
    
    size_t i = 0;
    while (i < input.size() && output.size() < uncompressedSize) {
        uint8_t ctrl = input[i++];
        if (ctrl & 0x80) {
            // Run-length encoded
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







