#include "ModelRegistry.h"
#include "core/vfs/FileSystem.h"
#include <iostream>
#include <cstring>

namespace cm {

namespace {
    constexpr uint32_t REGISTRY_MAGIC = 0x4745524D;  // 'MREG'
    constexpr uint32_t REGISTRY_VERSION = 1;
}

ModelRegistry& ModelRegistry::Instance() {
    static ModelRegistry s_instance;
    return s_instance;
}

bool ModelRegistry::Load(const std::string& registryPath) {
    std::vector<uint8_t> data;
    if (!FileSystem::Instance().ReadFile(registryPath, data)) {
        std::cerr << "[ModelRegistry] Failed to read: " << registryPath << std::endl;
        return false;
    }
    return LoadFromMemory(data.data(), data.size());
}

bool ModelRegistry::LoadFromMemory(const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (size < 12) {
        std::cerr << "[ModelRegistry] File too small" << std::endl;
        return false;
    }
    
    size_t pos = 0;
    
    // Read header
    uint32_t magic, version, modelCount;
    std::memcpy(&magic, data + pos, 4); pos += 4;
    std::memcpy(&version, data + pos, 4); pos += 4;
    std::memcpy(&modelCount, data + pos, 4); pos += 4;
    
    if (magic != REGISTRY_MAGIC) {
        std::cerr << "[ModelRegistry] Invalid magic" << std::endl;
        return false;
    }
    if (version > REGISTRY_VERSION) {
        std::cerr << "[ModelRegistry] Unsupported version: " << version << std::endl;
        return false;
    }
    
    m_manifests.clear();
    m_manifests.reserve(modelCount);
    
    // Read each model manifest
    for (uint32_t i = 0; i < modelCount; ++i) {
        // Each manifest starts with its size
        if (pos + 4 > size) {
            std::cerr << "[ModelRegistry] Unexpected end of data at model " << i << std::endl;
            return false;
        }
        
        uint32_t manifestSize;
        std::memcpy(&manifestSize, data + pos, 4); pos += 4;
        
        if (pos + manifestSize > size) {
            std::cerr << "[ModelRegistry] Manifest size overflow at model " << i << std::endl;
            return false;
        }
        
        RuntimeModelManifest manifest;
        if (RuntimeModelManifestLoader::LoadFromMemory(data + pos, manifestSize, manifest)) {
            m_manifests[manifest.modelGuid] = std::move(manifest);
        } else {
            std::cerr << "[ModelRegistry] Failed to load manifest " << i << std::endl;
        }
        
        pos += manifestSize;
    }
    
    m_loaded = true;
    std::cout << "[ModelRegistry] Loaded " << m_manifests.size() << " model manifests" << std::endl;
    return true;
}

bool ModelRegistry::HasModel(const ClaymoreGUID& guid) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_manifests.find(guid) != m_manifests.end();
}

const RuntimeModelManifest* ModelRegistry::GetManifest(const ClaymoreGUID& guid) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_manifests.find(guid);
    if (it != m_manifests.end()) {
        return &it->second;
    }
    return nullptr;
}

void ModelRegistry::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_manifests.clear();
    m_loaded = false;
}

} // namespace cm


