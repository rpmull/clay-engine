#pragma once

#include <string>
#include <unordered_map>
#include <memory>
#include <glm/glm.hpp>

// Simple editable material asset description persisted as .mat (JSON)
// Focused on PBR-like usage with optional custom uniforms.
// Also supports shader graph materials via shaderGraphPath.
struct MaterialAssetDesc {
    std::string name;
    std::string shaderVS;   // e.g. "vs_pbr" or "vs_pbr_skinned"
    std::string shaderFS;   // e.g. "fs_pbr"
    
    // Shader graph support: if non-empty, creates a ShaderGraphMaterial instead of PBRMaterial
    std::string shaderGraphPath;  // Path to .shgraph file (e.g. "assets/shaders/MyShader.shgraph")

    // Common PBR texture slots
    std::string albedoPath;
    std::string metallicRoughnessPath;
    std::string normalPath;
    std::string aoPath;
    std::string emissionPath;
    std::string displacementPath;

    float metallicScalar = 0.0f;
    float roughnessScalar = 0.5f;
    float normalScale = 1.0f;
    float aoScalar = 1.0f;
    float emissionStrength = 0.0f;
    float displacementScale = 0.0f;
    glm::vec3 emissionColor = glm::vec3(1.0f);
    glm::vec2 uvScale = glm::vec2(1.0f);
    glm::vec2 uvOffset = glm::vec2(0.0f);
    
    // Shader graph render options
    bool twoSided = false;
    bool alphaClip = false;
    float alphaClipThreshold = 0.5f;
    bool receiveShadowsOverride = false;
    bool receiveShadows = false;

    // Optional parameter block (vec4 uniforms) keyed by uniform name
    std::unordered_map<std::string, glm::vec4> vec4Uniforms;
    
    // Texture uniforms for shader graph materials (sampler name -> path)
    std::unordered_map<std::string, std::string> textureUniforms;
};

// IO helpers
bool LoadMaterialAsset(const std::string& path, MaterialAssetDesc& out);
bool SaveMaterialAsset(const std::string& path, const MaterialAssetDesc& in);

// Runtime creation helper
class Material;
std::shared_ptr<Material> CreateMaterialFromAsset(const MaterialAssetDesc& desc);


