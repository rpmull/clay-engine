#pragma once

#include "ShaderGraph.h"
#include "core/rendering/Material.h"
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <glm/glm.hpp>
#include <bgfx/bgfx.h>

namespace shadergraph {

// A material parameter stored in a shader graph material
struct MaterialParameter {
    std::string name;           // Uniform/sampler name
    std::string displayName;    // UI display name
    ShaderValueType type;
    glm::vec4 value = glm::vec4(0.0f);
    std::string texturePath;    // For texture parameters
    int textureSlot = -1;
    
    // Runtime handles
    bgfx::UniformHandle uniformHandle = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle textureHandle = BGFX_INVALID_HANDLE;
};

// Descriptor for a shader graph material asset (serialized to .sgmat)
struct ShaderGraphMaterialDesc {
    std::string name;
    std::string shaderGraphPath;    // Path to .shgraph file
    std::string vertexShaderName;   // Generated shader name (vs_shgraph_xxx)
    std::string fragmentShaderName; // Generated shader name (fs_shgraph_xxx)
    
    // Parameter values
    std::vector<MaterialParameter> parameters;
    
    // UV transform
    glm::vec2 uvScale = glm::vec2(1.0f);
    glm::vec2 uvOffset = glm::vec2(0.0f);
    
    // Render state
    uint64_t stateFlags = BGFX_STATE_DEFAULT;
    bool twoSided = false;
    bool alphaClip = false;
    float alphaClipThreshold = 0.5f;
};

// Load/save shader graph material assets
bool LoadShaderGraphMaterial(const std::string& path, ShaderGraphMaterialDesc& out);
bool SaveShaderGraphMaterial(const std::string& path, const ShaderGraphMaterialDesc& in);

// Material class backed by a shader graph
class ShaderGraphMaterial : public Material {
public:
    ShaderGraphMaterial(const std::string& name, bgfx::ProgramHandle program);
    ~ShaderGraphMaterial();
    
    // Create a deep copy with fresh bgfx handles (preserves shader graph type)
    std::shared_ptr<Material> Clone() const override;
    
    // Create from a material descriptor
    static std::shared_ptr<ShaderGraphMaterial> CreateFromDesc(const ShaderGraphMaterialDesc& desc);
    
    // Get current state as a descriptor (for serialization/cloning)
    ShaderGraphMaterialDesc GetDesc() const;
    
    // Set parameter values
    void SetFloat(const std::string& name, float value);
    void SetFloat2(const std::string& name, const glm::vec2& value);
    void SetFloat3(const std::string& name, const glm::vec3& value);
    void SetFloat4(const std::string& name, const glm::vec4& value);
    void SetColor(const std::string& name, const glm::vec4& color);
    void SetTexture(const std::string& name, bgfx::TextureHandle texture);
    void SetTextureFromPath(const std::string& name, const std::string& path);
    
    // Get parameter values
    bool GetFloat(const std::string& name, float& out) const;
    bool GetFloat2(const std::string& name, glm::vec2& out) const;
    bool GetFloat3(const std::string& name, glm::vec3& out) const;
    bool GetFloat4(const std::string& name, glm::vec4& out) const;
    
    // UV transform
    void SetUVScale(const glm::vec2& scale);
    void SetUVOffset(const glm::vec2& offset);
    glm::vec2 GetUVScale() const { return glm::vec2(m_UVTransform.x, m_UVTransform.y); }
    glm::vec2 GetUVOffset() const { return glm::vec2(m_UVTransform.z, m_UVTransform.w); }
    
    // Bind all uniforms and textures
    void BindUniforms() const override;
    
    // Get all parameters (for inspector)
    const std::vector<MaterialParameter>& GetParameters() const { return m_Parameters; }
    std::vector<MaterialParameter>& GetParameters() { return m_Parameters; }
    
    // Get source shader graph path
    const std::string& GetShaderGraphPath() const { return m_ShaderGraphPath; }
    void SetShaderGraphPath(const std::string& path) { m_ShaderGraphPath = path; }
    
    // Sync parameters with shader graph
    void SyncWithShaderGraph(const std::string& shaderGraphPath);
    
private:
    MaterialParameter* FindParameter(const std::string& name);
    const MaterialParameter* FindParameter(const std::string& name) const;
    void EnsureUniformHandles();
    void RefreshTextureParametersFromPaths() const;
    
    std::string m_ShaderGraphPath;
    std::vector<MaterialParameter> m_Parameters;
    glm::vec4 m_UVTransform = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
    bgfx::UniformHandle m_UVTransformUniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_TimeUniform = BGFX_INVALID_HANDLE;
};

} // namespace shadergraph

