#include "SkelBin.h"
#include <fstream>
#include <filesystem>
#include "core/vfs/FileSystem.h"

namespace skelbin {

struct Header { uint32_t magic; uint32_t version; uint32_t jointCount; };

// Helper: Ensures file is available on disk for reading.
// If file doesn't exist but is in a mounted pak, extracts to temp cache.
static std::string EnsureFileOnDisk(const std::string& filePath) {
    namespace fs = std::filesystem;
    if (fs::exists(filePath)) return filePath;
    
    std::vector<uint8_t> data;
    if (FileSystem::Instance().ReadFile(filePath, data)) {
        fs::path cacheDir = fs::temp_directory_path() / "claymore_pak_cache";
        std::error_code ec; fs::create_directories(cacheDir, ec);
        size_t h = std::hash<std::string>{}(filePath);
        std::string ext = fs::path(filePath).extension().string();
        fs::path cachePath = cacheDir / ("cache_" + std::to_string(h) + ext);
        std::ofstream out(cachePath, std::ios::binary | std::ios::trunc);
        if (out.is_open() && !data.empty()) {
            out.write(reinterpret_cast<const char*>(data.data()), data.size());
            out.close();
            return cachePath.string();
        }
    }
    return filePath;
}

bool WriteSkelBin(const PackedSkeleton& s, const std::string& filePath) {
    try {
        std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        uint32_t n = (uint32_t)s.inverseBindPoses.size();
        Header h{ SKEL_BIN_MAGIC, SKEL_BIN_VERSION, n };
        out.write((const char*)&h, sizeof(h));
        if (n) {
            out.write((const char*)s.inverseBindPoses.data(), (std::streamsize)(n * sizeof(glm::mat4)));
            out.write((const char*)s.boneParents.data(), (std::streamsize)(n * sizeof(int)));
            // Write string table for bone names (uint32 length + bytes per name)
            uint32_t count = (uint32_t)s.boneNames.size();
            out.write((const char*)&count, sizeof(count));
            for (const auto& nm : s.boneNames) {
                uint32_t len = (uint32_t)nm.size();
                out.write((const char*)&len, sizeof(len));
                if (len) out.write(nm.data(), len);
            }
        }
        return true;
    } catch (...) { return false; }
}

bool ReadSkelBin(const std::string& filePath, PackedSkeleton& out) {
    std::string diskPath = EnsureFileOnDisk(filePath);
    std::ifstream in(diskPath, std::ios::binary | std::ios::ate); if (!in.is_open()) return false;
    size_t sz = (size_t)in.tellg(); in.seekg(0);
    if (sz < sizeof(Header)) return false;
    Header h{}; in.read((char*)&h, sizeof(h));
    if (h.magic != SKEL_BIN_MAGIC || (h.version != 1 && h.version != SKEL_BIN_VERSION)) return false;
    out.inverseBindPoses.resize(h.jointCount);
    out.boneParents.resize(h.jointCount);
    if (h.jointCount) {
        in.read((char*)out.inverseBindPoses.data(), (std::streamsize)(h.jointCount * sizeof(glm::mat4)));
        in.read((char*)out.boneParents.data(), (std::streamsize)(h.jointCount * sizeof(int)));
        if (h.version >= 2) {
            uint32_t count=0; in.read((char*)&count, sizeof(count));
            out.boneNames.resize(count);
            for (uint32_t i=0;i<count;++i) {
                uint32_t len=0; in.read((char*)&len, sizeof(len));
                out.boneNames[i].resize(len);
                if (len) in.read(out.boneNames[i].data(), len);
            }
        } else {
            out.boneNames.clear();
        }
    }
    return true;
}

}


