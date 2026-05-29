#include "SkelBinLoader.h"
#include "core/ecs/AnimationComponents.h"
#include "core/vfs/FileSystem.h"
#include "core/vfs/VirtualFS.h"
#include <iostream>
#include <cstring>

namespace cm {
namespace SkelBinLoader {

struct Header { 
    uint32_t magic; 
    uint32_t version; 
    uint32_t jointCount; 
};

std::unique_ptr<SkeletonComponent> Load(const std::string& skelBinPath) {
    std::vector<uint8_t> data;
    
    // Try VFS first (for PAK access at runtime)
    if (VFS::Get() && VFS::Get()->ReadFile(skelBinPath, data)) {
        // Loaded from VFS
    } else if (FileSystem::Instance().ReadFile(skelBinPath, data)) {
        // Loaded from FileSystem
    } else {
        std::cerr << "[SkelBinLoader] Failed to read file: " << skelBinPath << std::endl;
        return nullptr;
    }
    
    if (data.size() < sizeof(Header)) {
        std::cerr << "[SkelBinLoader] File too small: " << skelBinPath << std::endl;
        return nullptr;
    }
    
    size_t offset = 0;
    Header h;
    std::memcpy(&h, data.data(), sizeof(Header));
    offset += sizeof(Header);
    
    if (h.magic != SKEL_BIN_MAGIC) {
        std::cerr << "[SkelBinLoader] Invalid magic: " << skelBinPath << std::endl;
        return nullptr;
    }
    
    if (h.version != 1 && h.version != SKEL_BIN_VERSION) {
        std::cerr << "[SkelBinLoader] Unsupported version " << h.version << ": " << skelBinPath << std::endl;
        return nullptr;
    }
    
    auto skeleton = std::make_unique<SkeletonComponent>();
    
    if (h.jointCount == 0) {
        return skeleton;
    }
    
    // Read inverse bind poses
    size_t ibpSize = h.jointCount * sizeof(glm::mat4);
    if (offset + ibpSize > data.size()) {
        std::cerr << "[SkelBinLoader] Truncated inverse bind poses: " << skelBinPath << std::endl;
        return nullptr;
    }
    skeleton->InverseBindPoses.resize(h.jointCount);
    std::memcpy(skeleton->InverseBindPoses.data(), data.data() + offset, ibpSize);
    offset += ibpSize;
    
    // Read bone parents
    size_t parentsSize = h.jointCount * sizeof(int);
    if (offset + parentsSize > data.size()) {
        std::cerr << "[SkelBinLoader] Truncated bone parents: " << skelBinPath << std::endl;
        return nullptr;
    }
    skeleton->BoneParents.resize(h.jointCount);
    std::memcpy(skeleton->BoneParents.data(), data.data() + offset, parentsSize);
    offset += parentsSize;
    
    // Read bone names (v2+)
    if (h.version >= 2 && offset + sizeof(uint32_t) <= data.size()) {
        uint32_t nameCount;
        std::memcpy(&nameCount, data.data() + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        
        skeleton->BoneNames.resize(nameCount);
        for (uint32_t i = 0; i < nameCount && offset < data.size(); ++i) {
            if (offset + sizeof(uint32_t) > data.size()) break;
            
            uint32_t len;
            std::memcpy(&len, data.data() + offset, sizeof(uint32_t));
            offset += sizeof(uint32_t);
            
            if (len > 0 && offset + len <= data.size()) {
                skeleton->BoneNames[i].assign(reinterpret_cast<const char*>(data.data() + offset), len);
                offset += len;
            }
            
            skeleton->BoneNameToIndex[skeleton->BoneNames[i]] = static_cast<int>(i);
        }
    }
    
    // Initialize bone entities to invalid
    skeleton->BoneEntities.resize(h.jointCount, static_cast<EntityID>(-1));
    
    std::cout << "[SkelBinLoader] Loaded skeleton with " << h.jointCount << " bones: " << skelBinPath << std::endl;
    return skeleton;
}

} // namespace SkelBinLoader
} // namespace cm


