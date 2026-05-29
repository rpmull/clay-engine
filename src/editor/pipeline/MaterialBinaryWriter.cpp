#include "MaterialBinaryWriter.h"
#include <fstream>
#include <iostream>
#include <cstring>

namespace binary {

void MaterialBinaryWriter::WriteContext::Write(const void* src, size_t count) {
    size_t oldSize = data.size();
    data.resize(oldSize + count);
    std::memcpy(data.data() + oldSize, src, count);
}

void MaterialBinaryWriter::WriteContext::WriteAt(size_t offset, const void* src, size_t count) {
    if (offset + count <= data.size()) {
        std::memcpy(data.data() + offset, src, count);
    }
}

uint32_t MaterialBinaryWriter::WriteContext::AddString(const std::string& str) {
    auto it = stringLookup.find(str);
    if (it != stringLookup.end()) {
        return it->second;
    }
    uint32_t index = static_cast<uint32_t>(strings.size());
    strings.push_back(str);
    stringLookup[str] = index;
    return index;
}

bool MaterialBinaryWriter::Write(const MaterialAssetDesc& desc, const std::string& outputPath) {
    std::vector<uint8_t> data;
    if (!WriteToMemory(desc, data)) {
        return false;
    }
    
    std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        std::cerr << "[MaterialBinaryWriter] Failed to open output file: " << outputPath << std::endl;
        return false;
    }
    
    out.write(reinterpret_cast<const char*>(data.data()), data.size());
    std::cout << "[MaterialBinaryWriter] Wrote " << data.size() << " bytes to " << outputPath << std::endl;
    return true;
}

bool MaterialBinaryWriter::WriteToMemory(const MaterialAssetDesc& desc, std::vector<uint8_t>& outData) {
    WriteContext ctx;
    
    // Reserve space for header
    MaterialBinaryHeader header{};
    header.base.magic = MATERIAL_MAGIC;
    header.base.version = MATERIAL_VERSION;
    header.base.flags = 0;
    header.base.reserved = 0;
    
    ctx.data.resize(sizeof(MaterialBinaryHeader));
    
    // Add shader name to string table
    std::string shaderName = desc.shaderFS.empty() ? "pbr" : desc.shaderFS;
    // Strip "fs_" prefix if present
    if (shaderName.find("fs_") == 0) {
        shaderName = shaderName.substr(3);
    }
    header.shaderNameOffset = ctx.AddString(shaderName);
    
    // Collect textures
    std::vector<TextureSlotEntry> textures;
    auto addTexture = [&](uint32_t slot, const std::string& path) {
        if (!path.empty()) {
            TextureSlotEntry entry;
            entry.slotId = slot;
            entry.pathOffset = ctx.AddString(path);
            textures.push_back(entry);
        }
    };
    
    addTexture(0, desc.albedoPath);
    addTexture(1, desc.normalPath);
    addTexture(2, desc.metallicRoughnessPath);
    addTexture(3, desc.aoPath);
    addTexture(4, desc.emissionPath);
    addTexture(5, desc.displacementPath);
    
    header.textureCount = static_cast<uint32_t>(textures.size());
    
    // Collect uniforms
    std::vector<UniformEntry> uniforms;
    auto addUniform = [&](const std::string& name, float v0, float v1 = 0.0f, float v2 = 0.0f, float v3 = 0.0f) {
        UniformEntry entry;
        entry.nameOffset = ctx.AddString(name);
        entry.type = (v1 == 0 && v2 == 0 && v3 == 0) ? 0 : 3; // float or vec4
        entry.data[0] = v0;
        entry.data[1] = v1;
        entry.data[2] = v2;
        entry.data[3] = v3;
        uniforms.push_back(entry);
    };
    
    addUniform("metallic", desc.metallicScalar);
    addUniform("roughness", desc.roughnessScalar);
    addUniform("normalScale", desc.normalScale);
    addUniform("ao", desc.aoScalar);
    addUniform("emissionStrength", desc.emissionStrength);
    addUniform("emissionColor", desc.emissionColor.r, desc.emissionColor.g, desc.emissionColor.b);
    addUniform("displacementScale", desc.displacementScale);
    addUniform("uvScale", desc.uvScale.x, desc.uvScale.y);
    addUniform("uvOffset", desc.uvOffset.x, desc.uvOffset.y);
    if (desc.receiveShadowsOverride) {
        addUniform("u_shadowReceive", desc.receiveShadows ? 1.0f : 0.0f);
    }
    
    // Add custom vec4 uniforms
    for (const auto& kv : desc.vec4Uniforms) {
        UniformEntry entry;
        entry.nameOffset = ctx.AddString(kv.first);
        entry.type = 3; // vec4
        entry.data[0] = kv.second.x;
        entry.data[1] = kv.second.y;
        entry.data[2] = kv.second.z;
        entry.data[3] = kv.second.w;
        uniforms.push_back(entry);
    }
    
    header.uniformCount = static_cast<uint32_t>(uniforms.size());
    
    // Write texture table
    header.textureTableOffset = static_cast<uint32_t>(ctx.Position());
    for (const auto& tex : textures) {
        ctx.Write(tex);
    }
    
    // Write uniform table
    header.uniformTableOffset = static_cast<uint32_t>(ctx.Position());
    for (const auto& u : uniforms) {
        ctx.Write(u);
    }
    
    // Write string table
    header.stringTableOffset = static_cast<uint32_t>(ctx.Position());
    uint32_t stringCount = static_cast<uint32_t>(ctx.strings.size());
    ctx.Write(stringCount);
    for (const auto& str : ctx.strings) {
        uint32_t len = static_cast<uint32_t>(str.length());
        ctx.Write(len);
        if (len > 0) {
            ctx.Write(str.data(), len);
        }
    }
    
    // Update header
    ctx.WriteAt(0, &header, sizeof(header));
    
    outData = std::move(ctx.data);
    return true;
}

bool MaterialBinaryWriter::ConvertMaterial(const std::string& sourcePath, const std::string& outputPath) {
    MaterialAssetDesc desc;
    if (!LoadMaterialAsset(sourcePath, desc)) {
        std::cerr << "[MaterialBinaryWriter] Failed to load source material: " << sourcePath << std::endl;
        return false;
    }
    
    return Write(desc, outputPath);
}

} // namespace binary


