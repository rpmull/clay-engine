#include "RuntimeModelManifestWriter.h"
#include <nlohmann/json.hpp>
#include "editor/pipeline/ModelImportSettings.h"
#include <fstream>
#include <iostream>
#include <cstring>

namespace cm {

namespace {

class BinaryWriter {
public:
    void Write(const void* data, size_t bytes) {
        const uint8_t* src = static_cast<const uint8_t*>(data);
        m_buffer.insert(m_buffer.end(), src, src + bytes);
    }
    
    template<typename T>
    void Write(const T& value) {
        Write(&value, sizeof(T));
    }
    
    void WriteString(const std::string& str) {
        uint16_t len = static_cast<uint16_t>(std::min(str.size(), size_t(65535)));
        Write(len);
        Write(str.data(), len);
    }
    
    std::vector<uint8_t>& Data() { return m_buffer; }
    
private:
    std::vector<uint8_t> m_buffer;
};

glm::vec3 ParseVec3(const nlohmann::json& j, const char* key, glm::vec3 def = glm::vec3(0.0f)) {
    if (!j.contains(key) || !j[key].is_array() || j[key].size() < 3) return def;
    return glm::vec3(j[key][0].get<float>(), j[key][1].get<float>(), j[key][2].get<float>());
}

glm::quat ParseQuat(const nlohmann::json& j, const char* key, glm::quat def = glm::quat(1.0f, 0.0f, 0.0f, 0.0f)) {
    if (!j.contains(key) || !j[key].is_array() || j[key].size() < 4) return def;
    // JSON stores as [w, x, y, z]
    return glm::quat(j[key][0].get<float>(), j[key][1].get<float>(), 
                     j[key][2].get<float>(), j[key][3].get<float>());
}

glm::vec4 ParseVec4(const nlohmann::json& j, const char* key, glm::vec4 def = glm::vec4(1.0f)) {
    if (!j.contains(key) || !j[key].is_array() || j[key].size() < 4) return def;
    return glm::vec4(j[key][0].get<float>(), j[key][1].get<float>(), 
                     j[key][2].get<float>(), j[key][3].get<float>());
}

} // anonymous namespace

bool RuntimeModelManifestWriter::CompileFromMeta(
    const std::string& metaPath,
    RuntimeModelManifest& outManifest,
    std::function<ClaymoreGUID(const std::string&)> pathToGuidResolver)
{
    std::ifstream file(metaPath);
    if (!file.is_open()) {
        std::cerr << "[RuntimeModelManifestWriter] Failed to open: " << metaPath << std::endl;
        return false;
    }
    
    nlohmann::json metaJson;
    try {
        file >> metaJson;
    } catch (const std::exception& e) {
        std::cerr << "[RuntimeModelManifestWriter] JSON parse error in " << metaPath << ": " << e.what() << std::endl;
        return false;
    }
    
    return CompileFromJson(metaJson, outManifest, pathToGuidResolver);
}

bool RuntimeModelManifestWriter::CompileFromJson(
    const nlohmann::json& metaJson,
    RuntimeModelManifest& outManifest,
    std::function<ClaymoreGUID(const std::string&)> pathToGuidResolver)
{
    ModelImportSettings importSettings;
    if (metaJson.contains("importSettings") && metaJson["importSettings"].is_object()) {
        importSettings = ModelImportSettings::FromJson(metaJson["importSettings"]);
    }
    // Model GUID
    if (metaJson.contains("guid") && metaJson["guid"].is_string()) {
        outManifest.modelGuid = ClaymoreGUID::FromString(metaJson["guid"].get<std::string>());
    }
    
    // Meshbin GUID (resolve path to GUID)
    if (metaJson.contains("meshbin") && metaJson["meshbin"].is_string()) {
        std::string meshbinPath = metaJson["meshbin"].get<std::string>();
        outManifest.meshbinGuid = pathToGuidResolver(meshbinPath);
    }
    
    // Skeleton GUID
    if (metaJson.contains("skeleton") && metaJson["skeleton"].is_string()) {
        std::string skelPath = metaJson["skeleton"].get<std::string>();
        outManifest.skeletonGuid = pathToGuidResolver(skelPath);
    }
    
    // Root transform
    if (metaJson.contains("rootTransform")) {
        const auto& rt = metaJson["rootTransform"];
        outManifest.rootPosition = ParseVec3(rt, "t");
        outManifest.rootRotation = ParseQuat(rt, "r");
        outManifest.rootScale = ParseVec3(rt, "s", glm::vec3(1.0f));
    }
    
    // Entries (mesh nodes)
    if (metaJson.contains("entries") && metaJson["entries"].is_array()) {
        for (const auto& entry : metaJson["entries"]) {
            RuntimeMeshNode node;
            
            node.name = entry.value("name", "");
            node.meshFileId = entry.value("meshIndex", -1);
            node.skinned = entry.value("skinned", false);
            node.parentIndex = -1; // Entries are flat, all children of root
            
            if (entry.contains("transform")) {
                const auto& t = entry["transform"];
                node.position = ParseVec3(t, "t");
                node.rotation = ParseQuat(t, "r");
                node.scale = ParseVec3(t, "s", glm::vec3(1.0f));
            }
            
            // Materials
            if (entry.contains("materials") && entry["materials"].is_array()) {
                int materialSlot = 0;
                for (const auto& mat : entry["materials"]) {
                    RuntimeMaterialSlot slot;
                    slot.name = mat.value("name", std::string());
                    
                    // Resolve texture paths to GUIDs
                    if (mat.contains("albedo") && mat["albedo"].is_object() && mat["albedo"].contains("path")) {
                        slot.albedoGuid = pathToGuidResolver(mat["albedo"]["path"].get<std::string>());
                    }
                    if (mat.contains("normal") && mat["normal"].is_object() && mat["normal"].contains("path")) {
                        slot.normalGuid = pathToGuidResolver(mat["normal"]["path"].get<std::string>());
                    }
                    if (mat.contains("metallicRoughness") && mat["metallicRoughness"].is_object() && mat["metallicRoughness"].contains("path")) {
                        slot.metallicRoughnessGuid = pathToGuidResolver(mat["metallicRoughness"]["path"].get<std::string>());
                    }
                    if (mat.contains("ao") && mat["ao"].is_object() && mat["ao"].contains("path")) {
                        slot.aoGuid = pathToGuidResolver(mat["ao"]["path"].get<std::string>());
                    }
                    if (mat.contains("emission") && mat["emission"].is_object() && mat["emission"].contains("path")) {
                        slot.emissionGuid = pathToGuidResolver(mat["emission"]["path"].get<std::string>());
                    }
                    if (mat.contains("displacement") && mat["displacement"].is_object() && mat["displacement"].contains("path")) {
                        slot.displacementGuid = pathToGuidResolver(mat["displacement"]["path"].get<std::string>());
                    }
                    
                    slot.tint = ParseVec4(mat, "tint", glm::vec4(1.0f));
                    slot.metallic = mat.value("metallic", 0.0f);
                    slot.roughness = mat.value("roughness", 1.0f);
                    slot.normalScale = mat.value("normalScale", 1.0f);
                    slot.aoStrength = mat.value("aoStrength", 1.0f);
                    slot.alphaBlend = mat.value("alphaBlend", false);
                    slot.alphaCutout = mat.value("alphaCutout", false);
                    slot.alphaCutoutThreshold = mat.value("alphaCutoutThreshold", 0.5f);
                    slot.twoSided = mat.value("twoSided", false);
                    slot.hasTint = mat.value("hasTint", false);
                    if (slot.alphaCutout) {
                        slot.alphaBlend = false;
                    }

                    // Apply material preset overrides if defined
                    const MeshMaterialPreset* preset = importSettings.FindPreset(node.name, materialSlot);
                    if (preset) {
                        if (!(preset->UseCustomMaterial && !preset->MaterialAssetPath.empty())) {
                            if (preset->OverrideAlbedo && !preset->AlbedoPath.empty()) {
                                slot.albedoGuid = pathToGuidResolver(preset->AlbedoPath);
                            }
                            if (preset->OverrideNormal && !preset->NormalPath.empty()) {
                                slot.normalGuid = pathToGuidResolver(preset->NormalPath);
                            }
                            if (preset->OverrideMetallicRoughness && !preset->MetallicRoughnessPath.empty()) {
                                slot.metallicRoughnessGuid = pathToGuidResolver(preset->MetallicRoughnessPath);
                            }
                            if (preset->OverrideAO && !preset->AOPath.empty()) {
                                slot.aoGuid = pathToGuidResolver(preset->AOPath);
                            }
                            if (preset->OverrideEmission && !preset->EmissionPath.empty()) {
                                slot.emissionGuid = pathToGuidResolver(preset->EmissionPath);
                            }
                            if (preset->OverrideDisplacement && !preset->DisplacementPath.empty()) {
                                slot.displacementGuid = pathToGuidResolver(preset->DisplacementPath);
                            }
                            if (preset->OverrideTint) {
                                slot.tint = preset->ColorTint;
                                slot.hasTint = true;
                            }
                            if (preset->OverrideAlphaBlend) {
                                slot.alphaCutout = preset->AlphaCutout;
                                slot.alphaCutoutThreshold = preset->AlphaCutoutThreshold;
                                slot.alphaBlend = preset->AlphaCutout ? false : preset->AlphaBlend;
                            }
                            if (preset->OverrideTwoSided) {
                                slot.twoSided = preset->TwoSided;
                            }
                        }
                    }
                    
                    node.materials.push_back(slot);
                    ++materialSlot;
                }
            }
            
            outManifest.nodes.push_back(std::move(node));
        }
    }
    
    // Proxy entries (for submesh-based rendering)
    if (metaJson.contains("proxies") && metaJson["proxies"].is_array()) {
        for (const auto& proxy : metaJson["proxies"]) {
            RuntimeMeshProxy proxyNode;
            
            proxyNode.name = proxy.value("name", "");
            proxyNode.displayName = proxy.value("displayName", "");
            proxyNode.meshEntryIndex = proxy.value("meshIndex", -1);
            proxyNode.originalMeshIndex = proxy.value("originalIndex", -1);
            proxyNode.skinned = proxy.value("skinned", false);
            
            if (proxy.contains("transform")) {
                const auto& t = proxy["transform"];
                proxyNode.position = ParseVec3(t, "t");
                proxyNode.rotation = ParseQuat(t, "r");
                proxyNode.scale = ParseVec3(t, "s", glm::vec3(1.0f));
            }
            
            if (proxy.contains("slots") && proxy["slots"].is_array()) {
                for (const auto& slot : proxy["slots"]) {
                    proxyNode.submeshSlots.push_back(slot.get<uint32_t>());
                }
            }
            
            outManifest.proxies.push_back(std::move(proxyNode));
        }
    }
    
    // Skeleton info
    if (metaJson.contains("skeletonInfo")) {
        const auto& skelInfo = metaJson["skeletonInfo"];
        
        if (skelInfo.contains("boneNames") && skelInfo["boneNames"].is_array()) {
            for (const auto& name : skelInfo["boneNames"]) {
                outManifest.boneNames.push_back(name.get<std::string>());
            }
        }
        
        if (skelInfo.contains("boneParents") && skelInfo["boneParents"].is_array()) {
            for (const auto& parent : skelInfo["boneParents"]) {
                outManifest.boneParents.push_back(parent.get<int32_t>());
            }
        }
    }
    
    return true;
}

bool RuntimeModelManifestWriter::WriteBinary(const RuntimeModelManifest& manifest, std::vector<uint8_t>& outData) {
    BinaryWriter writer;
    
    // Header
    writer.Write(RuntimeModelManifest::MAGIC);
    writer.Write(RuntimeModelManifest::VERSION);
    
    // GUIDs
    writer.Write(manifest.modelGuid.high);
    writer.Write(manifest.modelGuid.low);
    writer.Write(manifest.meshbinGuid.high);
    writer.Write(manifest.meshbinGuid.low);
    writer.Write(manifest.skeletonGuid.high);
    writer.Write(manifest.skeletonGuid.low);
    
    // Root transform
    writer.Write(manifest.rootPosition.x);
    writer.Write(manifest.rootPosition.y);
    writer.Write(manifest.rootPosition.z);
    writer.Write(manifest.rootRotation.w);
    writer.Write(manifest.rootRotation.x);
    writer.Write(manifest.rootRotation.y);
    writer.Write(manifest.rootRotation.z);
    writer.Write(manifest.rootScale.x);
    writer.Write(manifest.rootScale.y);
    writer.Write(manifest.rootScale.z);
    
    // Counts
    uint32_t nodeCount = static_cast<uint32_t>(manifest.nodes.size());
    uint32_t boneCount = static_cast<uint32_t>(manifest.boneNames.size());
    uint32_t proxyCount = static_cast<uint32_t>(manifest.proxies.size());
    writer.Write(nodeCount);
    writer.Write(boneCount);
    writer.Write(proxyCount);
    
    // Nodes
    for (const auto& node : manifest.nodes) {
        writer.WriteString(node.name);
        writer.Write(node.parentIndex);
        writer.Write(node.meshFileId);
        
        writer.Write(node.position.x);
        writer.Write(node.position.y);
        writer.Write(node.position.z);
        writer.Write(node.rotation.w);
        writer.Write(node.rotation.x);
        writer.Write(node.rotation.y);
        writer.Write(node.rotation.z);
        writer.Write(node.scale.x);
        writer.Write(node.scale.y);
        writer.Write(node.scale.z);
        
        uint8_t skinned = node.skinned ? 1 : 0;
        writer.Write(skinned);
        
        uint16_t materialCount = static_cast<uint16_t>(node.materials.size());
        writer.Write(materialCount);
        
        for (const auto& mat : node.materials) {
            if (RuntimeModelManifest::VERSION >= 5) {
                writer.WriteString(mat.name);
            }
            writer.Write(mat.albedoGuid.high);
            writer.Write(mat.albedoGuid.low);
            writer.Write(mat.normalGuid.high);
            writer.Write(mat.normalGuid.low);
            writer.Write(mat.metallicRoughnessGuid.high);
            writer.Write(mat.metallicRoughnessGuid.low);
            writer.Write(mat.aoGuid.high);
            writer.Write(mat.aoGuid.low);
            writer.Write(mat.emissionGuid.high);
            writer.Write(mat.emissionGuid.low);
            if (RuntimeModelManifest::VERSION >= 4) {
                writer.Write(mat.displacementGuid.high);
                writer.Write(mat.displacementGuid.low);
            }
            
            writer.Write(mat.tint.r);
            writer.Write(mat.tint.g);
            writer.Write(mat.tint.b);
            writer.Write(mat.tint.a);
            
            // Extended material properties (v2)
            writer.Write(mat.metallic);
            writer.Write(mat.roughness);
            writer.Write(mat.normalScale);
            writer.Write(mat.aoStrength);
            
            uint8_t flags = 0;
            if (mat.alphaBlend) flags |= 0x01;
            if (mat.twoSided) flags |= 0x02;
            if (mat.hasTint) flags |= 0x04;
            if (mat.alphaCutout) flags |= 0x08;
            writer.Write(flags);
            if (RuntimeModelManifest::VERSION >= 3) {
                writer.Write(mat.alphaCutoutThreshold);
            }
        }
    }
    
    // Proxies
    for (const auto& proxy : manifest.proxies) {
        writer.WriteString(proxy.name);
        writer.WriteString(proxy.displayName);
        writer.Write(proxy.meshEntryIndex);
        writer.Write(proxy.originalMeshIndex);
        
        writer.Write(proxy.position.x);
        writer.Write(proxy.position.y);
        writer.Write(proxy.position.z);
        writer.Write(proxy.rotation.w);
        writer.Write(proxy.rotation.x);
        writer.Write(proxy.rotation.y);
        writer.Write(proxy.rotation.z);
        writer.Write(proxy.scale.x);
        writer.Write(proxy.scale.y);
        writer.Write(proxy.scale.z);
        
        uint8_t skinned = proxy.skinned ? 1 : 0;
        writer.Write(skinned);
        
        uint16_t slotCount = static_cast<uint16_t>(proxy.submeshSlots.size());
        writer.Write(slotCount);
        for (uint32_t slot : proxy.submeshSlots) {
            writer.Write(slot);
        }
    }
    
    // Bones
    for (size_t i = 0; i < boneCount; ++i) {
        writer.WriteString(manifest.boneNames[i]);
        writer.Write(manifest.boneParents[i]);
    }
    
    outData = std::move(writer.Data());
    return true;
}

bool RuntimeModelManifestWriter::WriteToFile(const RuntimeModelManifest& manifest, const std::string& outputPath) {
    std::vector<uint8_t> data;
    if (!WriteBinary(manifest, data)) {
        return false;
    }
    
    std::ofstream file(outputPath, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[RuntimeModelManifestWriter] Failed to create: " << outputPath << std::endl;
        return false;
    }
    
    file.write(reinterpret_cast<const char*>(data.data()), data.size());
    return file.good();
}

bool RuntimeModelManifestWriter::WriteRegistry(const std::vector<RuntimeModelManifest>& manifests,
                                                std::vector<uint8_t>& outData) {
    BinaryWriter writer;
    
    // Registry header
    constexpr uint32_t REGISTRY_MAGIC = 0x4745524D;  // 'MREG'
    constexpr uint32_t REGISTRY_VERSION = 1;
    
    writer.Write(REGISTRY_MAGIC);
    writer.Write(REGISTRY_VERSION);
    writer.Write(static_cast<uint32_t>(manifests.size()));
    
    // Write each manifest with size prefix
    for (const auto& manifest : manifests) {
        std::vector<uint8_t> manifestData;
        if (!WriteBinary(manifest, manifestData)) {
            std::cerr << "[RuntimeModelManifestWriter] Failed to serialize manifest: " 
                      << manifest.modelGuid.ToString() << std::endl;
            continue;
        }
        
        // Size prefix for this manifest
        writer.Write(static_cast<uint32_t>(manifestData.size()));
        writer.Write(manifestData.data(), manifestData.size());
    }
    
    outData = std::move(writer.Data());
    return true;
}

bool RuntimeModelManifestWriter::WriteRegistryToFile(const std::vector<RuntimeModelManifest>& manifests,
                                                      const std::string& outputPath) {
    std::vector<uint8_t> data;
    if (!WriteRegistry(manifests, data)) {
        return false;
    }
    
    std::ofstream file(outputPath, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[RuntimeModelManifestWriter] Failed to create registry: " << outputPath << std::endl;
        return false;
    }
    
    file.write(reinterpret_cast<const char*>(data.data()), data.size());
    std::cout << "[RuntimeModelManifestWriter] Wrote model registry: " << outputPath 
              << " (" << manifests.size() << " models, " << data.size() << " bytes)" << std::endl;
    return file.good();
}

} // namespace cm

