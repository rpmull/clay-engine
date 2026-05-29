#include "RuntimeModelManifest.h"
#include "core/vfs/FileSystem.h"
#include <cstring>
#include <iostream>

namespace cm {

namespace {
    
class BinaryReader {
public:
    BinaryReader(const uint8_t* data, size_t size) : m_data(data), m_size(size), m_pos(0) {}
    
    bool Read(void* dst, size_t bytes) {
        if (m_pos + bytes > m_size) return false;
        std::memcpy(dst, m_data + m_pos, bytes);
        m_pos += bytes;
        return true;
    }
    
    template<typename T>
    bool Read(T& value) {
        return Read(&value, sizeof(T));
    }
    
    bool ReadString(std::string& str) {
        uint16_t len;
        if (!Read(len)) return false;
        if (m_pos + len > m_size) return false;
        str.assign(reinterpret_cast<const char*>(m_data + m_pos), len);
        m_pos += len;
        return true;
    }
    
    size_t Position() const { return m_pos; }
    
private:
    const uint8_t* m_data;
    size_t m_size;
    size_t m_pos;
};

} // anonymous namespace

bool RuntimeModelManifestLoader::Load(const std::string& path, RuntimeModelManifest& outManifest) {
    std::vector<uint8_t> data;
    if (!FileSystem::Instance().ReadFile(path, data)) {
        std::cerr << "[RuntimeModelManifest] Failed to read file: " << path << std::endl;
        return false;
    }
    return LoadFromMemory(data.data(), data.size(), outManifest);
}

bool RuntimeModelManifestLoader::LoadFromMemory(const uint8_t* data, size_t size, RuntimeModelManifest& outManifest) {
    BinaryReader reader(data, size);
    
    // Header
    uint32_t magic, version;
    if (!reader.Read(magic) || magic != RuntimeModelManifest::MAGIC) {
        std::cerr << "[RuntimeModelManifest] Invalid magic" << std::endl;
        return false;
    }
    if (!reader.Read(version) || version > RuntimeModelManifest::VERSION) {
        std::cerr << "[RuntimeModelManifest] Unsupported version: " << version << std::endl;
        return false;
    }
    
    // GUIDs
    uint64_t guidHigh, guidLow;
    if (!reader.Read(guidHigh) || !reader.Read(guidLow)) return false;
    outManifest.modelGuid = ClaymoreGUID(guidHigh, guidLow);
    
    if (!reader.Read(guidHigh) || !reader.Read(guidLow)) return false;
    outManifest.meshbinGuid = ClaymoreGUID(guidHigh, guidLow);
    
    if (!reader.Read(guidHigh) || !reader.Read(guidLow)) return false;
    outManifest.skeletonGuid = ClaymoreGUID(guidHigh, guidLow);
    
    // Root transform
    if (!reader.Read(outManifest.rootPosition.x)) return false;
    if (!reader.Read(outManifest.rootPosition.y)) return false;
    if (!reader.Read(outManifest.rootPosition.z)) return false;
    if (!reader.Read(outManifest.rootRotation.w)) return false;
    if (!reader.Read(outManifest.rootRotation.x)) return false;
    if (!reader.Read(outManifest.rootRotation.y)) return false;
    if (!reader.Read(outManifest.rootRotation.z)) return false;
    if (!reader.Read(outManifest.rootScale.x)) return false;
    if (!reader.Read(outManifest.rootScale.y)) return false;
    if (!reader.Read(outManifest.rootScale.z)) return false;
    
    // Counts
    uint32_t nodeCount, boneCount, proxyCount = 0;
    if (!reader.Read(nodeCount)) return false;
    if (!reader.Read(boneCount)) return false;
    if (version >= 2) {
        if (!reader.Read(proxyCount)) return false;
    }
    
    // Nodes
    outManifest.nodes.resize(nodeCount);
    for (uint32_t i = 0; i < nodeCount; ++i) {
        auto& node = outManifest.nodes[i];
        
        if (!reader.ReadString(node.name)) return false;
        if (!reader.Read(node.parentIndex)) return false;
        if (!reader.Read(node.meshFileId)) return false;
        
        if (!reader.Read(node.position.x)) return false;
        if (!reader.Read(node.position.y)) return false;
        if (!reader.Read(node.position.z)) return false;
        
        if (!reader.Read(node.rotation.w)) return false;
        if (!reader.Read(node.rotation.x)) return false;
        if (!reader.Read(node.rotation.y)) return false;
        if (!reader.Read(node.rotation.z)) return false;
        
        if (!reader.Read(node.scale.x)) return false;
        if (!reader.Read(node.scale.y)) return false;
        if (!reader.Read(node.scale.z)) return false;
        
        uint8_t skinned;
        if (!reader.Read(skinned)) return false;
        node.skinned = skinned != 0;
        
        uint16_t materialCount;
        if (!reader.Read(materialCount)) return false;
        node.materials.resize(materialCount);
        
        for (uint16_t m = 0; m < materialCount; ++m) {
            auto& mat = node.materials[m];
            if (version >= 5) {
                if (!reader.ReadString(mat.name)) return false;
            }
            
            if (!reader.Read(guidHigh) || !reader.Read(guidLow)) return false;
            mat.albedoGuid = ClaymoreGUID(guidHigh, guidLow);
            
            if (!reader.Read(guidHigh) || !reader.Read(guidLow)) return false;
            mat.normalGuid = ClaymoreGUID(guidHigh, guidLow);
            
            if (!reader.Read(guidHigh) || !reader.Read(guidLow)) return false;
            mat.metallicRoughnessGuid = ClaymoreGUID(guidHigh, guidLow);
            
            if (!reader.Read(guidHigh) || !reader.Read(guidLow)) return false;
            mat.aoGuid = ClaymoreGUID(guidHigh, guidLow);
            
            if (!reader.Read(guidHigh) || !reader.Read(guidLow)) return false;
            mat.emissionGuid = ClaymoreGUID(guidHigh, guidLow);

            if (version >= 4) {
                if (!reader.Read(guidHigh) || !reader.Read(guidLow)) return false;
                mat.displacementGuid = ClaymoreGUID(guidHigh, guidLow);
            }
            
            if (!reader.Read(mat.tint.r)) return false;
            if (!reader.Read(mat.tint.g)) return false;
            if (!reader.Read(mat.tint.b)) return false;
            if (!reader.Read(mat.tint.a)) return false;
            
            // Extended material properties (v2+)
            if (version >= 2) {
                if (!reader.Read(mat.metallic)) return false;
                if (!reader.Read(mat.roughness)) return false;
                if (!reader.Read(mat.normalScale)) return false;
                if (!reader.Read(mat.aoStrength)) return false;
            }
            
            uint8_t flags;
            if (!reader.Read(flags)) return false;
            mat.alphaBlend = (flags & 0x01) != 0;
            mat.twoSided = (flags & 0x02) != 0;
            mat.hasTint = (flags & 0x04) != 0;
            mat.alphaCutout = (flags & 0x08) != 0;
            if (version >= 3) {
                if (!reader.Read(mat.alphaCutoutThreshold)) return false;
            }
        }
    }
    
    // Proxies (v2+)
    if (version >= 2) {
        outManifest.proxies.resize(proxyCount);
        for (uint32_t i = 0; i < proxyCount; ++i) {
            auto& proxy = outManifest.proxies[i];
            
            if (!reader.ReadString(proxy.name)) return false;
            if (!reader.ReadString(proxy.displayName)) return false;
            if (!reader.Read(proxy.meshEntryIndex)) return false;
            if (!reader.Read(proxy.originalMeshIndex)) return false;
            
            if (!reader.Read(proxy.position.x)) return false;
            if (!reader.Read(proxy.position.y)) return false;
            if (!reader.Read(proxy.position.z)) return false;
            
            if (!reader.Read(proxy.rotation.w)) return false;
            if (!reader.Read(proxy.rotation.x)) return false;
            if (!reader.Read(proxy.rotation.y)) return false;
            if (!reader.Read(proxy.rotation.z)) return false;
            
            if (!reader.Read(proxy.scale.x)) return false;
            if (!reader.Read(proxy.scale.y)) return false;
            if (!reader.Read(proxy.scale.z)) return false;
            
            uint8_t skinned;
            if (!reader.Read(skinned)) return false;
            proxy.skinned = skinned != 0;
            
            uint16_t slotCount;
            if (!reader.Read(slotCount)) return false;
            proxy.submeshSlots.resize(slotCount);
            for (uint16_t s = 0; s < slotCount; ++s) {
                if (!reader.Read(proxy.submeshSlots[s])) return false;
            }
        }
    }
    
    // Bones
    outManifest.boneNames.resize(boneCount);
    outManifest.boneParents.resize(boneCount);
    for (uint32_t i = 0; i < boneCount; ++i) {
        if (!reader.ReadString(outManifest.boneNames[i])) return false;
        if (!reader.Read(outManifest.boneParents[i])) return false;
    }
    
    return true;
}

} // namespace cm

