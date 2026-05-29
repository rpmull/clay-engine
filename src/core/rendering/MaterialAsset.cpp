#include "MaterialAsset.h"
#include "ShaderManager.h"
#include "TextureLoader.h"
#include "PBRMaterial.h"
#include "core/vfs/FileSystem.h"
#ifndef CLAYMORE_RUNTIME
#include "editor/nodegraph/ShaderGraphMaterial.h"
#endif
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <sstream>

using json = nlohmann::json;

static void to_json(json& j, const MaterialAssetDesc& m) {
    j = json{
        {"name", m.name},
        {"shaderVS", m.shaderVS},
        {"shaderFS", m.shaderFS},
        {"shaderGraph", m.shaderGraphPath},
        {"albedo", m.albedoPath},
        {"metallicRoughness", m.metallicRoughnessPath},
        {"normal", m.normalPath},
        {"ao", m.aoPath},
        {"emission", m.emissionPath},
        {"displacement", m.displacementPath},
        {"metallicScalar", m.metallicScalar},
        {"roughnessScalar", m.roughnessScalar},
        {"normalScale", m.normalScale},
        {"aoScalar", m.aoScalar},
        {"emissionStrength", m.emissionStrength},
        {"displacementScale", m.displacementScale},
        {"emissionColor", { m.emissionColor.x, m.emissionColor.y, m.emissionColor.z }},
        {"uvScale", { m.uvScale.x, m.uvScale.y }},
        {"uvOffset", { m.uvOffset.x, m.uvOffset.y }},
        {"twoSided", m.twoSided},
        {"alphaClip", m.alphaClip},
        {"alphaClipThreshold", m.alphaClipThreshold},
        {"receiveShadowsOverride", m.receiveShadowsOverride},
        {"receiveShadows", m.receiveShadows}
    };
    // vec4 uniforms as name -> [x,y,z,w]
    json uniforms = json::object();
    for (const auto& kv : m.vec4Uniforms) {
        uniforms[kv.first] = { kv.second.x, kv.second.y, kv.second.z, kv.second.w };
    }
    j["uniforms"] = std::move(uniforms);
    
    // Texture uniforms for shader graph materials
    if (!m.textureUniforms.empty()) {
        json textures = json::object();
        for (const auto& kv : m.textureUniforms) {
            textures[kv.first] = kv.second;
        }
        j["textures"] = std::move(textures);
    }
}

static void from_json(const json& j, MaterialAssetDesc& m) {
    if (j.contains("name")) m.name = j.at("name").get<std::string>();
    if (j.contains("shaderVS")) m.shaderVS = j.at("shaderVS").get<std::string>();
    if (j.contains("shaderFS")) m.shaderFS = j.at("shaderFS").get<std::string>();
    if (j.contains("shaderGraph")) m.shaderGraphPath = j.at("shaderGraph").get<std::string>();
    if (j.contains("albedo")) m.albedoPath = j.at("albedo").get<std::string>();
    if (j.contains("metallicRoughness")) m.metallicRoughnessPath = j.at("metallicRoughness").get<std::string>();
    if (j.contains("normal")) m.normalPath = j.at("normal").get<std::string>();
    if (j.contains("ao")) m.aoPath = j.at("ao").get<std::string>();
    if (j.contains("emission")) m.emissionPath = j.at("emission").get<std::string>();
    if (j.contains("displacement")) m.displacementPath = j.at("displacement").get<std::string>();
    if (j.contains("metallicScalar")) m.metallicScalar = j.at("metallicScalar").get<float>();
    if (j.contains("roughnessScalar")) m.roughnessScalar = j.at("roughnessScalar").get<float>();
    if (j.contains("normalScale")) m.normalScale = j.at("normalScale").get<float>();
    if (j.contains("aoScalar")) m.aoScalar = j.at("aoScalar").get<float>();
    if (j.contains("emissionStrength")) m.emissionStrength = j.at("emissionStrength").get<float>();
    if (j.contains("displacementScale")) m.displacementScale = j.at("displacementScale").get<float>();
    if (j.contains("emissionColor") && j["emissionColor"].is_array() && j["emissionColor"].size() == 3) {
        m.emissionColor = glm::vec3(j["emissionColor"][0], j["emissionColor"][1], j["emissionColor"][2]);
    }
    if (j.contains("uvScale") && j["uvScale"].is_array() && j["uvScale"].size() == 2) {
        m.uvScale = glm::vec2(j["uvScale"][0], j["uvScale"][1]);
    }
    if (j.contains("uvOffset") && j["uvOffset"].is_array() && j["uvOffset"].size() == 2) {
        m.uvOffset = glm::vec2(j["uvOffset"][0], j["uvOffset"][1]);
    }
    if (j.contains("twoSided")) m.twoSided = j.at("twoSided").get<bool>();
    if (j.contains("alphaClip")) m.alphaClip = j.at("alphaClip").get<bool>();
    if (j.contains("alphaClipThreshold")) m.alphaClipThreshold = j.at("alphaClipThreshold").get<float>();
    if (j.contains("receiveShadowsOverride")) m.receiveShadowsOverride = j.at("receiveShadowsOverride").get<bool>();
    if (j.contains("receiveShadows")) m.receiveShadows = j.at("receiveShadows").get<bool>();
    if (j.contains("uniforms") && j["uniforms"].is_object()) {
        for (auto it = j["uniforms"].begin(); it != j["uniforms"].end(); ++it) {
            const auto& arr = it.value();
            if (arr.is_array() && arr.size() == 4) {
                m.vec4Uniforms[it.key()] = glm::vec4(arr[0], arr[1], arr[2], arr[3]);
            }
        }
    }
    if (j.contains("textures") && j["textures"].is_object()) {
        for (auto it = j["textures"].begin(); it != j["textures"].end(); ++it) {
            if (it.value().is_string()) {
                m.textureUniforms[it.key()] = it.value().get<std::string>();
            }
        }
    }
}

bool LoadMaterialAsset(const std::string& path, MaterialAssetDesc& out) {
    std::string text;
    if (!FileSystem::Instance().ReadTextFile(path, text)) {
        if (!FileSystem::Instance().IsDiskFallbackAllowed()) return false;
        std::ifstream in(path);
        if (!in) return false;
        std::stringstream ss;
        ss << in.rdbuf();
        text = ss.str();
    }
    json j = json::parse(text);
    try { out = j.get<MaterialAssetDesc>(); }
    catch (...) { return false; }
    return true;
}

bool SaveMaterialAsset(const std::string& path, const MaterialAssetDesc& in) {
    std::ofstream outFile(path);
    if (!outFile) return false;
    json j = in;
    outFile << j.dump(4);
    return true;
}

std::shared_ptr<Material> CreateMaterialFromAsset(const MaterialAssetDesc& desc) {
#ifndef CLAYMORE_RUNTIME
    // Check if this is a shader graph material (editor-only)
    if (!desc.shaderGraphPath.empty()) {
        // Create ShaderGraphMaterial from the shader graph path
        shadergraph::ShaderGraphMaterialDesc sgDesc;
        sgDesc.name = desc.name.empty() ? "ShaderGraphMaterial" : desc.name;
        sgDesc.shaderGraphPath = desc.shaderGraphPath;
        sgDesc.uvScale = desc.uvScale;
        sgDesc.uvOffset = desc.uvOffset;
        sgDesc.twoSided = desc.twoSided;
        sgDesc.alphaClip = desc.alphaClip;
        sgDesc.alphaClipThreshold = desc.alphaClipThreshold;
        
        // Convert vec4 uniforms to material parameters
        for (const auto& kv : desc.vec4Uniforms) {
            shadergraph::MaterialParameter param;
            param.name = kv.first;
            param.displayName = kv.first;
            param.type = shadergraph::ShaderValueType::Float4;
            param.value = kv.second;
            sgDesc.parameters.push_back(param);
        }
        
        // Convert texture uniforms to material parameters
        for (const auto& kv : desc.textureUniforms) {
            shadergraph::MaterialParameter param;
            param.name = kv.first;
            param.displayName = kv.first;
            param.type = shadergraph::ShaderValueType::Texture2D;
            param.texturePath = kv.second;
            sgDesc.parameters.push_back(param);
        }
        
        auto sgMat = shadergraph::ShaderGraphMaterial::CreateFromDesc(sgDesc);
        if (sgMat) {
            std::cout << "[MaterialAsset] Created ShaderGraphMaterial from: " << desc.shaderGraphPath << std::endl;
            return sgMat;
        } else {
            std::cerr << "[MaterialAsset] Failed to create ShaderGraphMaterial, falling back to PBR" << std::endl;
        }
    }
#endif
    
    // Default: Create PBR material
    auto program = ShaderManager::Instance().LoadProgram(
        desc.shaderVS.empty() ? std::string("vs_pbr") : desc.shaderVS,
        desc.shaderFS.empty() ? std::string("fs_pbr") : desc.shaderFS);

    // Use PBRMaterial to support standard texture slots, while still allowing vec4 uniforms
    auto mat = std::make_shared<PBRMaterial>(desc.name.empty() ? std::string("Material") : desc.name, program);

    if (auto pbr = std::dynamic_pointer_cast<PBRMaterial>(mat)) {
        if (!desc.albedoPath.empty()) pbr->SetAlbedoTextureFromPath(desc.albedoPath);
        if (!desc.metallicRoughnessPath.empty()) pbr->SetMetallicRoughnessTextureFromPath(desc.metallicRoughnessPath);
        if (!desc.normalPath.empty()) pbr->SetNormalTextureFromPath(desc.normalPath);
        if (!desc.aoPath.empty()) pbr->SetAmbientOcclusionTextureFromPath(desc.aoPath);
        if (!desc.emissionPath.empty()) pbr->SetEmissionTextureFromPath(desc.emissionPath);
        if (!desc.displacementPath.empty()) pbr->SetDisplacementTextureFromPath(desc.displacementPath);
        pbr->SetMetallic(desc.metallicScalar);
        pbr->SetRoughness(desc.roughnessScalar);
        pbr->SetNormalScale(desc.normalScale);
        pbr->SetAmbientOcclusion(desc.aoScalar);
        pbr->SetEmissionStrength(desc.emissionStrength);
        pbr->SetDisplacementScale(desc.displacementScale);
        pbr->SetEmissionColor(desc.emissionColor);
        pbr->SetUVScale(desc.uvScale);
        pbr->SetUVOffset(desc.uvOffset);
        pbr->SetReceiveShadowsOverride(desc.receiveShadowsOverride);
        pbr->SetReceiveShadows(desc.receiveShadows);
    }

    for (const auto& kv : desc.vec4Uniforms) {
        mat->SetUniform(kv.first, kv.second);
    }

    return mat;
}



