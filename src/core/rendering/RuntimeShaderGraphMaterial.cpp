#include "RuntimeShaderGraphMaterial.h"
#include "MaterialCache.h"
#include "TextureLoader.h"
#include <iostream>
#include <cmath>
#include <cctype>
#include <bx/timer.h>

namespace cm {

namespace {
bgfx::TextureHandle GetFallbackTextureForParameter(const std::string& parameterName)
{
    TextureSpecifier spec;
    std::string lowerName = parameterName;
    for (char& ch : lowerName) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }

    if (lowerName.find("normal") != std::string::npos) {
        spec.Path = "assets/debug/normal.png";
    } else if (lowerName.find("metal") != std::string::npos ||
               lowerName.find("rough") != std::string::npos ||
               lowerName.find("orm") != std::string::npos) {
        spec.Path = "assets/debug/metallic_roughness.png";
    } else {
        spec.Path = "assets/debug/white.png";
    }

    return AcquireTextureHandle(spec, TextureColorSpace::Linear);
}
}

RuntimeShaderGraphMaterial::RuntimeShaderGraphMaterial(const std::string& name, bgfx::ProgramHandle program)
    : Material(name, program)
{
    SetBindCacheSafe(false);
    // Create common uniforms
    m_UVTransformUniform = bgfx::createUniform("u_uvTransform", bgfx::UniformType::Vec4);
    m_TimeUniform = bgfx::createUniform("u_time", bgfx::UniformType::Vec4);
}

RuntimeShaderGraphMaterial::~RuntimeShaderGraphMaterial() {
    cm::rendering::SafeDestroyHandle(m_UVTransformUniform);
    cm::rendering::SafeDestroyHandle(m_TimeUniform);
    
    // Destroy parameter uniform handles (but not textures - they may be shared)
    for (auto& param : m_Parameters) {
        cm::rendering::SafeDestroyHandle(param.uniformHandle);
    }
}

std::shared_ptr<Material> RuntimeShaderGraphMaterial::Clone() const {
    auto clone = std::make_shared<RuntimeShaderGraphMaterial>(GetName(), GetProgram());
    clone->m_ShaderGraphPath = m_ShaderGraphPath;
    clone->m_UVTransform = m_UVTransform;
    clone->m_StateFlags = GetStateFlags();
    
    // Copy parameters
    for (const auto& param : m_Parameters) {
        RuntimeMaterialParameter newParam = param;
        newParam.uniformHandle = BGFX_INVALID_HANDLE; // Will be recreated
        if (param.type == RuntimeShaderValueType::Texture2D && !param.texturePath.empty()) {
            TextureSpecifier spec;
            spec.Path = param.texturePath;
            newParam.textureHandle = AcquireTextureHandle(spec, TextureColorSpace::Linear);
        }
        clone->m_Parameters.push_back(newParam);
    }
    clone->EnsureUniformHandles();
    
    return clone;
}

void RuntimeShaderGraphMaterial::AddParameter(const RuntimeMaterialParameter& param) {
    m_Parameters.push_back(param);
    
    // Create uniform handle for the new parameter
    auto& p = m_Parameters.back();
    if (!bgfx::isValid(p.uniformHandle)) {
        if (p.type == RuntimeShaderValueType::Texture2D) {
            p.uniformHandle = bgfx::createUniform(p.name.c_str(), bgfx::UniformType::Sampler);
        } else {
            p.uniformHandle = bgfx::createUniform(p.name.c_str(), bgfx::UniformType::Vec4);
        }
    }
}

RuntimeMaterialParameter* RuntimeShaderGraphMaterial::FindParameter(const std::string& name) {
    for (auto& p : m_Parameters) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

void RuntimeShaderGraphMaterial::EnsureUniformHandles() {
    for (auto& p : m_Parameters) {
        if (!bgfx::isValid(p.uniformHandle)) {
            if (p.type == RuntimeShaderValueType::Texture2D) {
                p.uniformHandle = bgfx::createUniform(p.name.c_str(), bgfx::UniformType::Sampler);
            } else {
                p.uniformHandle = bgfx::createUniform(p.name.c_str(), bgfx::UniformType::Vec4);
            }
        }
    }
}

void RuntimeShaderGraphMaterial::SetParameter(const std::string& name, const glm::vec4& value) {
    auto* p = FindParameter(name);
    if (p) {
        p->value = value;
    }
}

void RuntimeShaderGraphMaterial::SetTexture(const std::string& name, bgfx::TextureHandle texture, int slot) {
    auto* p = FindParameter(name);
    if (p) {
        p->textureHandle = texture;
        p->texturePath.clear();
        if (slot >= 0) p->textureSlot = slot;
    }
}

void RuntimeShaderGraphMaterial::SetTextureFromPath(const std::string& name, const std::string& path, int slot) {
    auto* p = FindParameter(name);
    if (p) {
        p->texturePath = path;
        if (!path.empty()) {
            TextureSpecifier spec;
            spec.Path = path;
            p->textureHandle = AcquireTextureHandle(spec, TextureColorSpace::Linear);
        } else {
            p->textureHandle = BGFX_INVALID_HANDLE;
        }
        if (slot >= 0) p->textureSlot = slot;
    }
}

void RuntimeShaderGraphMaterial::SetUVScale(const glm::vec2& scale) {
    m_UVTransform.x = scale.x;
    m_UVTransform.y = scale.y;
}

void RuntimeShaderGraphMaterial::SetUVOffset(const glm::vec2& offset) {
    m_UVTransform.z = offset.x;
    m_UVTransform.w = offset.y;
}

void RuntimeShaderGraphMaterial::BindUniforms() const {
    RefreshTextureParametersFromPaths();
    // Bind base uniforms
    Material::BindUniforms();
    
    // Bind UV transform
    if (bgfx::isValid(m_UVTransformUniform)) {
        bgfx::setUniform(m_UVTransformUniform, &m_UVTransform);
    }
    
    // Bind time uniform (x = time, y = sin(time), z = cos(time), w = frac(time))
    if (bgfx::isValid(m_TimeUniform)) {
        static uint64_t s_startCounter = bx::getHPCounter();
        const uint64_t now = bx::getHPCounter();
        const double freq = double(bx::getHPFrequency());
        float timeSec = static_cast<float>((double(now - s_startCounter) / freq));
        
        glm::vec4 timeVec(timeSec, std::sin(timeSec), std::cos(timeSec), timeSec - std::floor(timeSec));
        bgfx::setUniform(m_TimeUniform, &timeVec);
    }
    
    // Bind all parameters
    int textureUnit = 0;
    for (const auto& param : m_Parameters) {
        if (param.type == RuntimeShaderValueType::Texture2D) {
            if (!bgfx::isValid(param.uniformHandle)) {
                continue;
            }

            const int slot = param.textureSlot >= 0 ? param.textureSlot : textureUnit++;
            bgfx::TextureHandle texture = bgfx::isValid(param.textureHandle)
                ? param.textureHandle
                : GetFallbackTextureForParameter(param.name);
            if (bgfx::isValid(texture)) {
                bgfx::setTexture(slot, param.uniformHandle, texture);
            }
        } else {
            // Uniform binding
            if (bgfx::isValid(param.uniformHandle)) {
                bgfx::setUniform(param.uniformHandle, &param.value);
            }
        }
    }
}

void RuntimeShaderGraphMaterial::RefreshTextureParametersFromPaths() const {
    auto* self = const_cast<RuntimeShaderGraphMaterial*>(this);
    for (auto& param : self->m_Parameters) {
        if (param.type != RuntimeShaderValueType::Texture2D) {
            continue;
        }
        if (param.texturePath.empty()) {
            param.textureHandle = BGFX_INVALID_HANDLE;
            continue;
        }

        TextureSpecifier spec;
        spec.Path = param.texturePath;
        param.textureHandle = AcquireTextureHandle(spec, TextureColorSpace::Linear);
    }
}

} // namespace cm

