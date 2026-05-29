#include "MaterialBinaryLoader.h"
#include "core/rendering/Material.h"
#include "core/rendering/PBRMaterial.h"
#include "core/rendering/SkinnedPBRMaterial.h"
#include "core/rendering/ShaderManager.h"
#include "core/vfs/VirtualFS.h"
#include "core/vfs/FileSystem.h"
#include <cstring>
#include <iostream>

namespace binary {

bool MaterialBinaryLoader::LoadContext::Read(void* dst, size_t count) {
    if (pos + count > size) return false;
    std::memcpy(dst, data + pos, count);
    pos += count;
    return true;
}

std::string MaterialBinaryLoader::LoadContext::ReadString(uint32_t index) const {
    if (index < strings.size()) {
        return strings[index];
    }
    return "";
}

std::shared_ptr<Material> MaterialBinaryLoader::Load(const std::string& path, bool skinned) {
    std::vector<uint8_t> data;
    
    std::cout << "[MaterialBinaryLoader] Attempting to load: " << path << " (skinned=" << skinned << ")" << std::endl;
    
    // Try VFS first
    if (VFS::Get()) {
        if (VFS::Get()->ReadFile(path, data)) {
            std::cout << "[MaterialBinaryLoader] VFS read success: " << path << " (" << data.size() << " bytes)" << std::endl;
            return LoadFromMemory(data.data(), data.size(), skinned);
        } else {
            std::cout << "[MaterialBinaryLoader] VFS read failed: " << path << std::endl;
        }
    }
    
    // Fallback to FileSystem
    if (FileSystem::Instance().ReadFile(path, data)) {
        std::cout << "[MaterialBinaryLoader] FileSystem read success: " << path << " (" << data.size() << " bytes)" << std::endl;
        return LoadFromMemory(data.data(), data.size(), skinned);
    }
    
    std::cerr << "[MaterialBinaryLoader] Failed to read file from all sources: " << path << std::endl;
    return nullptr;
}

std::shared_ptr<Material> MaterialBinaryLoader::LoadFromMemory(const uint8_t* data, size_t size, bool skinned) {
    if (!data || size < sizeof(MaterialBinaryHeader)) {
        std::cerr << "[MaterialBinaryLoader] Invalid data or size" << std::endl;
        return nullptr;
    }
    
    LoadContext ctx;
    ctx.data = data;
    ctx.size = size;
    ctx.pos = 0;
    
    MaterialBinaryHeader header;
    if (!LoadHeader(ctx, header)) {
        return nullptr;
    }
    
    if (!LoadStringTable(ctx, header)) {
        return nullptr;
    }
    
    // Get shader names
    std::string shaderVS = skinned ? "vs_pbr_skinned" : "vs_pbr";
    std::string shaderFS = "fs_pbr";
    
    // Read shader name if present (but override VS for skinned)
    if (header.shaderNameOffset < ctx.strings.size()) {
        std::string shaderName = ctx.ReadString(header.shaderNameOffset);
        if (!shaderName.empty()) {
            // Use skinned variant if needed
            shaderVS = skinned ? ("vs_" + shaderName + "_skinned") : ("vs_" + shaderName);
            shaderFS = "fs_" + shaderName;
        }
    }
    
    // Load shader program
    auto program = ShaderManager::Instance().LoadProgram(shaderVS, shaderFS);
    
    // Create appropriate material type
    std::shared_ptr<PBRMaterial> mat;
    if (skinned) {
        mat = std::make_shared<SkinnedPBRMaterial>("Material", program);
    } else {
        mat = std::make_shared<PBRMaterial>("Material", program);
    }
    
    // Load textures
    ctx.pos = header.textureTableOffset;
    for (uint32_t i = 0; i < header.textureCount; ++i) {
        TextureSlotEntry slot;
        if (!ctx.Read(slot)) break;
        
        std::string texPath = ctx.ReadString(slot.pathOffset);
        if (texPath.empty()) continue;
        
        auto pbr = std::dynamic_pointer_cast<PBRMaterial>(mat);
        if (!pbr) continue;
        
        switch (slot.slotId) {
            case 0: pbr->SetAlbedoTextureFromPath(texPath); break;
            case 1: pbr->SetNormalTextureFromPath(texPath); break;
            case 2: pbr->SetMetallicRoughnessTextureFromPath(texPath); break;
            case 3: pbr->SetAmbientOcclusionTextureFromPath(texPath); break;
            case 4: pbr->SetEmissionTextureFromPath(texPath); break;
            case 5: pbr->SetDisplacementTextureFromPath(texPath); break;
        }
    }
    
    // Load uniforms
    ctx.pos = header.uniformTableOffset;
    for (uint32_t i = 0; i < header.uniformCount; ++i) {
        UniformEntry uniform;
        if (!ctx.Read(uniform)) break;
        
        std::string name = ctx.ReadString(uniform.nameOffset);
        
        auto pbr = std::dynamic_pointer_cast<PBRMaterial>(mat);
        if (!pbr) continue;
        
        // Handle known PBR uniforms
        if (name == "metallic") {
            pbr->SetMetallic(uniform.data[0]);
        } else if (name == "roughness") {
            pbr->SetRoughness(uniform.data[0]);
        } else if (name == "normalScale") {
            pbr->SetNormalScale(uniform.data[0]);
        } else if (name == "ao") {
            pbr->SetAmbientOcclusion(uniform.data[0]);
        } else if (name == "emissionStrength") {
            pbr->SetEmissionStrength(uniform.data[0]);
        } else if (name == "displacementScale") {
            pbr->SetDisplacementScale(uniform.data[0]);
        } else if (name == "emissionColor") {
            pbr->SetEmissionColor(glm::vec3(uniform.data[0], uniform.data[1], uniform.data[2]));
        } else if (name == "uvScale") {
            pbr->SetUVScale(glm::vec2(uniform.data[0], uniform.data[1]));
        } else if (name == "uvOffset") {
            pbr->SetUVOffset(glm::vec2(uniform.data[0], uniform.data[1]));
        } else if (name == "u_shadowReceive") {
            pbr->SetReceiveShadowsOverride(true);
            pbr->SetReceiveShadows(uniform.data[0] > 0.5f);
        } else {
            // Custom uniform
            mat->SetUniform(name, glm::vec4(uniform.data[0], uniform.data[1], 
                                            uniform.data[2], uniform.data[3]));
        }
    }
    
    return mat;
}

bool MaterialBinaryLoader::Validate(const std::string& path) {
    std::vector<uint8_t> data;
    
    if (VFS::Get() && VFS::Get()->ReadFile(path, data)) {
        if (data.size() < sizeof(MaterialBinaryHeader)) return false;
        const MaterialBinaryHeader* h = reinterpret_cast<const MaterialBinaryHeader*>(data.data());
        return h->base.magic == MATERIAL_MAGIC && h->base.version <= MATERIAL_VERSION;
    }
    
    return false;
}

bool MaterialBinaryLoader::LoadHeader(LoadContext& ctx, MaterialBinaryHeader& header) {
    if (!ctx.Read(header)) {
        std::cerr << "[MaterialBinaryLoader] Failed to read header" << std::endl;
        return false;
    }
    
    if (header.base.magic != MATERIAL_MAGIC) {
        std::cerr << "[MaterialBinaryLoader] Invalid magic number" << std::endl;
        return false;
    }
    
    if (header.base.version > MATERIAL_VERSION) {
        std::cerr << "[MaterialBinaryLoader] Unsupported version" << std::endl;
        return false;
    }
    
    return true;
}

bool MaterialBinaryLoader::LoadStringTable(LoadContext& ctx, const MaterialBinaryHeader& header) {
    if (header.stringTableOffset == 0) return true;
    
    ctx.pos = header.stringTableOffset;
    
    uint32_t stringCount = 0;
    if (!ctx.Read(stringCount)) return false;
    
    ctx.strings.reserve(stringCount);
    
    for (uint32_t i = 0; i < stringCount; ++i) {
        uint32_t len = 0;
        if (!ctx.Read(len)) return false;
        
        std::string str;
        str.resize(len);
        if (len > 0 && !ctx.Read(str.data(), len)) return false;
        
        ctx.strings.push_back(std::move(str));
    }
    
    return true;
}

} // namespace binary


