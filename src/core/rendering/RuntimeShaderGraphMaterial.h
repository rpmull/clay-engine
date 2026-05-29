#pragma once

#include "Material.h"
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <bgfx/bgfx.h>

namespace cm {

// Shader value types (matching shadergraph::ShaderValueType)
enum class RuntimeShaderValueType : uint32_t {
    Float = 0,
    Float2 = 1,
    Float3 = 2,
    Float4 = 3,
    Texture2D = 4,
    // Add others as needed
};

// Parameter for runtime shader graph materials
struct RuntimeMaterialParameter {
    std::string name;
    std::string displayName;
    RuntimeShaderValueType type = RuntimeShaderValueType::Float4;
    glm::vec4 value = glm::vec4(0.0f);
    std::string texturePath;
    int32_t textureSlot = -1;
    
    // Runtime handles
    bgfx::UniformHandle uniformHandle = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle textureHandle = BGFX_INVALID_HANDLE;
};

// Runtime-compatible shader graph material (no editor dependencies)
class RuntimeShaderGraphMaterial : public Material {
public:
    RuntimeShaderGraphMaterial(const std::string& name, bgfx::ProgramHandle program);
    ~RuntimeShaderGraphMaterial();
    
    std::shared_ptr<Material> Clone() const override;
    
    // Set parameter values
    void SetParameter(const std::string& name, const glm::vec4& value);
    void SetTexture(const std::string& name, bgfx::TextureHandle texture, int slot = -1);
    void SetTextureFromPath(const std::string& name, const std::string& path, int slot = -1);
    
    // UV transform
    void SetUVScale(const glm::vec2& scale);
    void SetUVOffset(const glm::vec2& offset);
    glm::vec2 GetUVScale() const { return glm::vec2(m_UVTransform.x, m_UVTransform.y); }
    glm::vec2 GetUVOffset() const { return glm::vec2(m_UVTransform.z, m_UVTransform.w); }
    
    // Bind all uniforms and textures
    void BindUniforms() const override;
    
    // Add a parameter
    void AddParameter(const RuntimeMaterialParameter& param);
    
    // Get parameters
    const std::vector<RuntimeMaterialParameter>& GetParameters() const { return m_Parameters; }
    std::vector<RuntimeMaterialParameter>& GetParameters() { return m_Parameters; }
    
    // Source info
    const std::string& GetShaderGraphPath() const { return m_ShaderGraphPath; }
    void SetShaderGraphPath(const std::string& path) { m_ShaderGraphPath = path; }

private:
    RuntimeMaterialParameter* FindParameter(const std::string& name);
    void EnsureUniformHandles();
    void RefreshTextureParametersFromPaths() const;
    
    std::string m_ShaderGraphPath;
    std::vector<RuntimeMaterialParameter> m_Parameters;
    glm::vec4 m_UVTransform = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
    bgfx::UniformHandle m_UVTransformUniform = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_TimeUniform = BGFX_INVALID_HANDLE;
};

} // namespace cm

