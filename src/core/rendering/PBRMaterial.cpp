#include "PBRMaterial.h"
#include "TextureLoader.h"
#include "MaterialCache.h"
#include "core/ecs/Scene.h"
#include <filesystem>

// Forward declarations for helpers used in BindUniforms
static bool TryResolveTintMaskPath(const std::string& albedoPath, std::string& outPath);

PBRMaterial::PBRMaterial(const std::string& name, bgfx::ProgramHandle program)
    : Material(name, program,
        BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z |
        BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_MSAA | BGFX_STATE_CULL_CW)
{
    SetBindCacheSafe(false);
    u_AlbedoSampler = bgfx::createUniform("s_albedo", bgfx::UniformType::Sampler);
    u_MetallicRoughnessSampler = bgfx::createUniform("s_metallicRoughness", bgfx::UniformType::Sampler);
    u_NormalSampler = bgfx::createUniform("s_normalMap", bgfx::UniformType::Sampler);
    u_TintMaskSampler = bgfx::createUniform("s_tintMask", bgfx::UniformType::Sampler);
    u_AOSampler = bgfx::createUniform("s_ao", bgfx::UniformType::Sampler);
    u_EmissionSampler = bgfx::createUniform("s_emission", bgfx::UniformType::Sampler);
    u_DisplacementSampler = bgfx::createUniform("s_displacement", bgfx::UniformType::Sampler);
   
    m_AlbedoTex = BGFX_INVALID_HANDLE;
    m_MetallicRoughnessTex = BGFX_INVALID_HANDLE;
    m_NormalTex = BGFX_INVALID_HANDLE;
    m_AOTex = BGFX_INVALID_HANDLE;
    m_EmissionTex = BGFX_INVALID_HANDLE;
    m_DisplacementTex = BGFX_INVALID_HANDLE;
    m_TintMaskTex = BGFX_INVALID_HANDLE;
    // Default tint to white so shaders multiply by 1
    SetUniform("u_ColorTint", glm::vec4(1.0f));
    // Default tint params: mode=0 (Normal/multiply), threshold=0.5
    SetUniform("u_TintParams", glm::vec4(0.0f, 0.5f, 0.0f, 0.0f));
    SyncScalarUniforms();
    SyncUVUniform();
    m_TextureUsage = glm::vec4(0.0f);
    SetUniform("u_TextureUsage", m_TextureUsage);
    m_LastTextureCacheGeneration = GetTextureCacheGeneration();
    m_PathBackedTexturesDirty = true;
   } 

PBRMaterial::PBRMaterial(const std::string& name, bgfx::ProgramHandle program, uint64_t stateFlags)
    : Material(name, program, stateFlags) {
    SetBindCacheSafe(false);
	u_AlbedoSampler = bgfx::createUniform("s_albedo", bgfx::UniformType::Sampler);
	u_MetallicRoughnessSampler = bgfx::createUniform("s_metallicRoughness", bgfx::UniformType::Sampler);
	u_NormalSampler = bgfx::createUniform("s_normalMap", bgfx::UniformType::Sampler);
    u_TintMaskSampler = bgfx::createUniform("s_tintMask", bgfx::UniformType::Sampler);
    u_AOSampler = bgfx::createUniform("s_ao", bgfx::UniformType::Sampler);
    u_EmissionSampler = bgfx::createUniform("s_emission", bgfx::UniformType::Sampler);
    u_DisplacementSampler = bgfx::createUniform("s_displacement", bgfx::UniformType::Sampler);

	m_AlbedoTex = BGFX_INVALID_HANDLE;
	m_MetallicRoughnessTex = BGFX_INVALID_HANDLE;
	m_NormalTex = BGFX_INVALID_HANDLE;
    m_AOTex = BGFX_INVALID_HANDLE;
    m_EmissionTex = BGFX_INVALID_HANDLE;
    m_DisplacementTex = BGFX_INVALID_HANDLE;
    m_TintMaskTex = BGFX_INVALID_HANDLE;
    // Default tint to white
    SetUniform("u_ColorTint", glm::vec4(1.0f));
    // Default tint params: mode=0 (Normal/multiply), threshold=0.5
    SetUniform("u_TintParams", glm::vec4(0.0f, 0.5f, 0.0f, 0.0f));
    SyncScalarUniforms();
    SyncUVUniform();
    m_TextureUsage = glm::vec4(0.0f);
    SetUniform("u_TextureUsage", m_TextureUsage);
    m_LastTextureCacheGeneration = GetTextureCacheGeneration();
    m_PathBackedTexturesDirty = true;
}

PBRMaterial::~PBRMaterial() {
    cm::rendering::SafeDestroyHandle(u_AlbedoSampler);
    cm::rendering::SafeDestroyHandle(u_MetallicRoughnessSampler);
    cm::rendering::SafeDestroyHandle(u_NormalSampler);
    cm::rendering::SafeDestroyHandle(u_AOSampler);
    cm::rendering::SafeDestroyHandle(u_EmissionSampler);
    cm::rendering::SafeDestroyHandle(u_TintMaskSampler);
    cm::rendering::SafeDestroyHandle(u_DisplacementSampler);
}

std::shared_ptr<Material> PBRMaterial::Clone() const {
    auto clone = std::make_shared<PBRMaterial>(GetName() + "_Clone", GetProgram(), GetStateFlags());
    
    // Copy textures (handles are shared, not owned by the material)
    clone->m_AlbedoTex = m_AlbedoTex;
    clone->m_MetallicRoughnessTex = m_MetallicRoughnessTex;
    clone->m_NormalTex = m_NormalTex;
    clone->m_AOTex = m_AOTex;
    clone->m_EmissionTex = m_EmissionTex;
    clone->m_DisplacementTex = m_DisplacementTex;
    clone->m_TintMaskTex = m_TintMaskTex;
    
    // Copy texture paths
    clone->m_AlbedoPath = m_AlbedoPath;
    clone->m_MetallicRoughnessPath = m_MetallicRoughnessPath;
    clone->m_NormalPath = m_NormalPath;
    clone->m_AOPath = m_AOPath;
    clone->m_EmissionPath = m_EmissionPath;
    clone->m_DisplacementPath = m_DisplacementPath;
    clone->m_TintMaskPath = m_TintMaskPath;
    
    // Copy scalar values
    clone->m_Metallic = m_Metallic;
    clone->m_Roughness = m_Roughness;
    clone->m_NormalScale = m_NormalScale;
    clone->m_AOScalar = m_AOScalar;
    clone->m_EmissionStrength = m_EmissionStrength;
    clone->m_DisplacementScale = m_DisplacementScale;
    clone->m_EmissionColor = m_EmissionColor;
    clone->m_UVTransform = m_UVTransform;
    clone->m_TextureUsage = m_TextureUsage;
    clone->m_ReceiveShadowsOverride = m_ReceiveShadowsOverride;
    clone->m_ReceiveShadows = m_ReceiveShadows;
    
    // Sync uniforms
    clone->SyncScalarUniforms();
    clone->SyncUVUniform();
    clone->SyncTextureUsageUniform();
    clone->m_LastTextureCacheGeneration = GetTextureCacheGeneration();
    clone->m_PathBackedTexturesDirty = false;
    
    return clone;
}

void PBRMaterial::SetAlbedoTexture(bgfx::TextureHandle texture, const std::string& sourcePath) {
    m_AlbedoTex = texture;
    if (!sourcePath.empty()) {
        m_AlbedoPath = sourcePath;
    } else {
        m_AlbedoPath.clear();
    }
    m_TintMaskTex = BGFX_INVALID_HANDLE;
    m_TintMaskPath.clear();
    m_LastTextureCacheGeneration = GetTextureCacheGeneration();
    m_PathBackedTexturesDirty = !sourcePath.empty();
    SyncTextureUsageUniform();
}
void PBRMaterial::SetMetallicRoughnessTexture(bgfx::TextureHandle texture, const std::string& sourcePath) {
    m_MetallicRoughnessTex = texture;
    if (!sourcePath.empty()) {
        m_MetallicRoughnessPath = sourcePath;
    } else {
        m_MetallicRoughnessPath.clear();
    }
    if (bgfx::isValid(texture) && m_Metallic == kDefaultMetallic && m_Roughness == kDefaultRoughness) {
        m_Metallic = 1.0f;
        m_Roughness = 1.0f;
        SyncScalarUniforms();
    }
    m_LastTextureCacheGeneration = GetTextureCacheGeneration();
    m_PathBackedTexturesDirty = !sourcePath.empty();
    SyncTextureUsageUniform();
}
void PBRMaterial::SetNormalTexture(bgfx::TextureHandle texture, const std::string& sourcePath) {
    m_NormalTex = texture;
    if (!sourcePath.empty()) {
        m_NormalPath = sourcePath;
    } else {
        m_NormalPath.clear();
    }
    m_LastTextureCacheGeneration = GetTextureCacheGeneration();
    m_PathBackedTexturesDirty = !sourcePath.empty();
    SyncTextureUsageUniform();
}
void PBRMaterial::SetAmbientOcclusionTexture(bgfx::TextureHandle texture, const std::string& sourcePath) {
    m_AOTex = texture;
    if (!sourcePath.empty()) {
        m_AOPath = sourcePath;
    } else {
        m_AOPath.clear();
    }
    m_LastTextureCacheGeneration = GetTextureCacheGeneration();
    m_PathBackedTexturesDirty = !sourcePath.empty();
    SyncTextureUsageUniform();
}
void PBRMaterial::SetEmissionTexture(bgfx::TextureHandle texture, const std::string& sourcePath) {
    m_EmissionTex = texture;
    if (!sourcePath.empty()) {
        m_EmissionPath = sourcePath;
    } else {
        m_EmissionPath.clear();
    }
    m_LastTextureCacheGeneration = GetTextureCacheGeneration();
    m_PathBackedTexturesDirty = !sourcePath.empty();
    SyncTextureUsageUniform();
}
void PBRMaterial::SetDisplacementTexture(bgfx::TextureHandle texture, const std::string& sourcePath) {
    m_DisplacementTex = texture;
    if (!sourcePath.empty()) {
        m_DisplacementPath = sourcePath;
    } else {
        m_DisplacementPath.clear();
    }
    m_LastTextureCacheGeneration = GetTextureCacheGeneration();
    m_PathBackedTexturesDirty = !sourcePath.empty();
    SyncTextureUsageUniform();
    SyncScalarUniforms();
}

void PBRMaterial::SetTintMaskTexture(bgfx::TextureHandle texture, const std::string& sourcePath) {
    m_TintMaskTex = texture;
    if (!sourcePath.empty()) {
        m_TintMaskPath = sourcePath;
    } else {
        m_TintMaskPath.clear();
    }
    m_LastTextureCacheGeneration = GetTextureCacheGeneration();
    m_PathBackedTexturesDirty = !sourcePath.empty();
}

void PBRMaterial::SetAlbedoTextureFromPath(const std::string& path) {
    m_AlbedoPath = path;
    m_TintMaskTex = BGFX_INVALID_HANDLE;
    m_TintMaskPath.clear();
    if (path.empty()) {
        m_AlbedoTex = BGFX_INVALID_HANDLE;
        m_LastTextureCacheGeneration = GetTextureCacheGeneration();
        m_PathBackedTexturesDirty = true;
        SyncTextureUsageUniform();
        return;
    }
    // Keep albedo loading in linear path for legacy content compatibility.
    TextureSpecifier spec;
    spec.Path = path;
    m_AlbedoTex = AcquireTextureHandle(spec, TextureColorSpace::Linear);
    m_LastTextureCacheGeneration = GetTextureCacheGeneration();
    m_PathBackedTexturesDirty = true;
    SyncTextureUsageUniform();
} 

void PBRMaterial::SetMetallicRoughnessTextureFromPath(const std::string& path) {
    m_MetallicRoughnessPath = path;
    if (path.empty()) {
        m_MetallicRoughnessTex = BGFX_INVALID_HANDLE;
        m_LastTextureCacheGeneration = GetTextureCacheGeneration();
        m_PathBackedTexturesDirty = true;
        SyncTextureUsageUniform();
        return;
    }
    // Metallic/Roughness is data - use linear (no gamma conversion)
    TextureSpecifier spec;
    spec.Path = path;
    m_MetallicRoughnessTex = AcquireTextureHandle(spec, TextureColorSpace::Linear);
    if (bgfx::isValid(m_MetallicRoughnessTex) &&
        m_Metallic == kDefaultMetallic &&
        m_Roughness == kDefaultRoughness) {
        m_Metallic = 1.0f;
        m_Roughness = 1.0f;
        SyncScalarUniforms();
    }
    m_LastTextureCacheGeneration = GetTextureCacheGeneration();
    m_PathBackedTexturesDirty = true;
    SyncTextureUsageUniform();
}

void PBRMaterial::SetNormalTextureFromPath(const std::string& path) {
    m_NormalPath = path;
    if (path.empty()) {
        m_NormalTex = BGFX_INVALID_HANDLE;
        m_LastTextureCacheGeneration = GetTextureCacheGeneration();
        m_PathBackedTexturesDirty = true;
        SyncTextureUsageUniform();
        return;
    }
    // Normal maps are data - use linear (no gamma conversion)
    TextureSpecifier spec;
    spec.Path = path;
    m_NormalTex = AcquireTextureHandle(spec, TextureColorSpace::Linear);
    m_LastTextureCacheGeneration = GetTextureCacheGeneration();
    m_PathBackedTexturesDirty = true;
    SyncTextureUsageUniform();
}

void PBRMaterial::SetAmbientOcclusionTextureFromPath(const std::string& path) {
    m_AOPath = path;
    if (path.empty()) {
        m_AOTex = BGFX_INVALID_HANDLE;
        m_LastTextureCacheGeneration = GetTextureCacheGeneration();
        m_PathBackedTexturesDirty = true;
        SyncTextureUsageUniform();
        return;
    }
    // AO is data - use linear (no gamma conversion)
    TextureSpecifier spec;
    spec.Path = path;
    m_AOTex = AcquireTextureHandle(spec, TextureColorSpace::Linear);
    m_LastTextureCacheGeneration = GetTextureCacheGeneration();
    m_PathBackedTexturesDirty = true;
    SyncTextureUsageUniform();
}

void PBRMaterial::SetEmissionTextureFromPath(const std::string& path) {
    m_EmissionPath = path;
    if (path.empty()) {
        m_EmissionTex = BGFX_INVALID_HANDLE;
        m_LastTextureCacheGeneration = GetTextureCacheGeneration();
        m_PathBackedTexturesDirty = true;
        SyncTextureUsageUniform();
        return;
    }
    // Keep emission loading in linear path for legacy content compatibility.
    TextureSpecifier spec;
    spec.Path = path;
    m_EmissionTex = AcquireTextureHandle(spec, TextureColorSpace::Linear);
    m_LastTextureCacheGeneration = GetTextureCacheGeneration();
    m_PathBackedTexturesDirty = true;
    SyncTextureUsageUniform();
}

void PBRMaterial::SetDisplacementTextureFromPath(const std::string& path) {
    m_DisplacementPath = path;
    if (path.empty()) {
        m_DisplacementTex = BGFX_INVALID_HANDLE;
        m_LastTextureCacheGeneration = GetTextureCacheGeneration();
        m_PathBackedTexturesDirty = true;
        SyncTextureUsageUniform();
        SyncScalarUniforms();
        return;
    }
    // Displacement is data - use linear (no gamma conversion)
    TextureSpecifier spec;
    spec.Path = path;
    m_DisplacementTex = AcquireTextureHandle(spec, TextureColorSpace::Linear);
    m_LastTextureCacheGeneration = GetTextureCacheGeneration();
    m_PathBackedTexturesDirty = true;
    SyncTextureUsageUniform();
    SyncScalarUniforms();
}

void PBRMaterial::SetTintMaskTextureFromPath(const std::string& path) {
    m_TintMaskPath = path;
    if (path.empty()) {
        m_TintMaskTex = BGFX_INVALID_HANDLE;
        m_LastTextureCacheGeneration = GetTextureCacheGeneration();
        m_PathBackedTexturesDirty = true;
        return;
    }

    TextureSpecifier spec;
    spec.Path = path;
    m_TintMaskTex = AcquireTextureHandle(spec, TextureColorSpace::Linear);
    m_LastTextureCacheGeneration = GetTextureCacheGeneration();
    m_PathBackedTexturesDirty = true;
}

void PBRMaterial::SetMetallic(float value) {
    m_Metallic = glm::clamp(value, 0.0f, 1.0f);
    SyncScalarUniforms();
}

void PBRMaterial::SetRoughness(float value) {
    m_Roughness = glm::clamp(value, 0.0f, 1.0f);
    SyncScalarUniforms();
}

void PBRMaterial::SetNormalScale(float value) {
    m_NormalScale = glm::clamp(value, 0.0f, 8.0f);
    SyncScalarUniforms();
}

void PBRMaterial::SetAmbientOcclusion(float value) {
    m_AOScalar = glm::clamp(value, 0.0f, 1.0f);
    SyncScalarUniforms();
}

void PBRMaterial::SetEmissionStrength(float value) {
    m_EmissionStrength = glm::max(0.0f, value);
    SyncScalarUniforms();
}

void PBRMaterial::SetEmissionColor(const glm::vec3& color) {
    m_EmissionColor = glm::clamp(color, glm::vec3(0.0f), glm::vec3(1.0f));
    SyncScalarUniforms();
}

void PBRMaterial::SetDisplacementScale(float value) {
    m_DisplacementScale = glm::max(0.0f, value);
    SyncScalarUniforms();
}

void PBRMaterial::SetUVScale(const glm::vec2& scale) {
    glm::vec2 clamped = glm::max(scale, glm::vec2(0.0001f));
    m_UVTransform.x = clamped.x;
    m_UVTransform.y = clamped.y;
    SyncUVUniform();
}

void PBRMaterial::SetUVOffset(const glm::vec2& offset) {
    m_UVTransform.z = offset.x;
    m_UVTransform.w = offset.y;
    SyncUVUniform();
}

void PBRMaterial::SetUVTransform(const glm::vec2& scale, const glm::vec2& offset) {
    glm::vec2 clamped = glm::max(scale, glm::vec2(0.0001f));
    m_UVTransform = glm::vec4(clamped, offset);
    SyncUVUniform();
}

void PBRMaterial::SetReceiveShadowsOverride(bool enabled) {
    m_ReceiveShadowsOverride = enabled;
}

void PBRMaterial::SetReceiveShadows(bool enabled) {
    m_ReceiveShadows = enabled;
}

void PBRMaterial::BindUniforms() const
   {
   RefreshPathBackedTextures();
   Material::BindUniforms();

   // Ensure sane defaults so missing textures don't render black
   // Engine does not use sRGB gamma correction - all textures loaded as Linear
   static bgfx::TextureHandle s_defaultWhite = BGFX_INVALID_HANDLE;
   static bgfx::TextureHandle s_defaultMR    = BGFX_INVALID_HANDLE;
   static bgfx::TextureHandle s_defaultNrm   = BGFX_INVALID_HANDLE;
   auto ensureDefaults = []()
   {
       if (!bgfx::isValid(s_defaultWhite)) {
           TextureSpecifier spec; spec.Path = "assets/debug/white.png";
           s_defaultWhite = AcquireTextureHandle(spec, TextureColorSpace::Linear);
       }
       if (!bgfx::isValid(s_defaultMR)) {
           TextureSpecifier spec; spec.Path = "assets/debug/metallic_roughness.png";
           s_defaultMR = AcquireTextureHandle(spec, TextureColorSpace::Linear);
       }
       if (!bgfx::isValid(s_defaultNrm)) {
           TextureSpecifier spec; spec.Path = "assets/debug/normal.png";
           s_defaultNrm = AcquireTextureHandle(spec, TextureColorSpace::Linear);
       }
   };
   ensureDefaults();

   // Apply scene-selected filtering to material textures
   uint32_t samplerFlags = 0;
   {
       // Default to linear filtering; allow point based on scene setting
       auto& scene = Scene::Get();
       auto mode = scene.GetDefaultShaderPreset(); // keep; but we need filter from Environment
   }
   {
       // Read from Environment
       const Environment& env = Scene::Get().GetEnvironment();
       if (env.TextureFilter == Environment::TextureFilterMode::Point) {
           samplerFlags |= BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT;
       } else {
           samplerFlags |= BGFX_SAMPLER_MIN_ANISOTROPIC | BGFX_SAMPLER_MAG_ANISOTROPIC; // fallback to linear if unsupported
       }
   }

   bgfx::setTexture(0, u_AlbedoSampler, bgfx::isValid(m_AlbedoTex) ? m_AlbedoTex : s_defaultWhite, samplerFlags);
   bgfx::setTexture(1, u_MetallicRoughnessSampler, bgfx::isValid(m_MetallicRoughnessTex) ? m_MetallicRoughnessTex : s_defaultMR, samplerFlags);
   bgfx::setTexture(2, u_NormalSampler, bgfx::isValid(m_NormalTex) ? m_NormalTex : s_defaultNrm, samplerFlags);
   bgfx::setTexture(3, u_TintMaskSampler, bgfx::isValid(m_TintMaskTex) ? m_TintMaskTex : s_defaultWhite, samplerFlags);
   bgfx::setTexture(4, u_AOSampler, bgfx::isValid(m_AOTex) ? m_AOTex : s_defaultWhite, samplerFlags);
   bgfx::setTexture(5, u_EmissionSampler, bgfx::isValid(m_EmissionTex) ? m_EmissionTex : s_defaultWhite, samplerFlags);
   bgfx::setTexture(6, u_DisplacementSampler, bgfx::isValid(m_DisplacementTex) ? m_DisplacementTex : s_defaultWhite, samplerFlags);
   }

void PBRMaterial::RefreshPathBackedTextures() const
{
    const uint64_t generation = GetTextureCacheGeneration();
    if (!m_PathBackedTexturesDirty && m_LastTextureCacheGeneration == generation) {
        return;
    }

    auto refreshTexture = [](const std::string& path,
                             bgfx::TextureHandle& handle,
                             TextureColorSpace colorSpace)
    {
        if (path.empty()) {
            handle = BGFX_INVALID_HANDLE;
            return;
        }

        TextureSpecifier spec;
        spec.Path = path;
        handle = AcquireTextureHandle(spec, colorSpace);
    };

    auto* self = const_cast<PBRMaterial*>(this);
    refreshTexture(self->m_AlbedoPath, self->m_AlbedoTex, TextureColorSpace::Linear);
    refreshTexture(self->m_MetallicRoughnessPath, self->m_MetallicRoughnessTex, TextureColorSpace::Linear);
    refreshTexture(self->m_NormalPath, self->m_NormalTex, TextureColorSpace::Linear);
    refreshTexture(self->m_AOPath, self->m_AOTex, TextureColorSpace::Linear);
    refreshTexture(self->m_EmissionPath, self->m_EmissionTex, TextureColorSpace::Linear);
    refreshTexture(self->m_DisplacementPath, self->m_DisplacementTex, TextureColorSpace::Linear);

    if (self->m_TintMaskPath.empty()) {
        std::string tintMaskPath;
        if (TryResolveTintMaskPath(self->m_AlbedoPath, tintMaskPath)) {
            self->m_TintMaskPath = tintMaskPath;
        }
    }
    refreshTexture(self->m_TintMaskPath, self->m_TintMaskTex, TextureColorSpace::Linear);
    self->m_LastTextureCacheGeneration = generation;
    self->m_PathBackedTexturesDirty = false;
    self->SyncTextureUsageUniform();
    self->SyncScalarUniforms();
}

void PBRMaterial::SyncScalarUniforms() {
    glm::vec4 scalar1(m_EmissionStrength, 0.0f, 0.0f, 0.0f);
    glm::vec4 previousScalar1(0.0f);
    if (TryGetUniform("u_PBRScalar1", previousScalar1)) {
        scalar1.y = previousScalar1.y;
    }
    SetUniform("u_PBRScalar0", glm::vec4(m_Metallic, m_Roughness, m_AOScalar, m_NormalScale));
    SetUniform("u_PBRScalar1", scalar1);
    SetUniform("u_EmissionColor", glm::vec4(m_EmissionColor, 1.0f));
    const float displacementScale = bgfx::isValid(m_DisplacementTex) ? m_DisplacementScale : 0.0f;
    SetUniform("u_DisplacementParams", glm::vec4(displacementScale, 0.5f, 0.0f, 0.0f));
}

void PBRMaterial::SyncUVUniform() {
    SetUniform("u_UVTransform", m_UVTransform);
}

void PBRMaterial::SyncTextureUsageUniform() const {
    glm::vec4 usage(
        bgfx::isValid(m_MetallicRoughnessTex) ? 1.0f : 0.0f,
        bgfx::isValid(m_NormalTex) ? 1.0f : 0.0f,
        bgfx::isValid(m_AOTex) ? 1.0f : 0.0f,
        bgfx::isValid(m_EmissionTex) ? 1.0f : 0.0f);
    const_cast<PBRMaterial*>(this)->SetUniform("u_TextureUsage", usage);
}

static bool TryResolveTintMaskPath(const std::string& albedoPath, std::string& outPath)
{
    if (albedoPath.empty()) return false;
    std::filesystem::path p(albedoPath);
    std::filesystem::path mp1 = p.parent_path() / (p.stem().string() + ".tintmask.bmp");
    std::filesystem::path mp2 = p.parent_path() / (p.stem().string() + ".tintmask"); // legacy
    std::error_code ec; 
    std::filesystem::path mp;
    if (std::filesystem::exists(mp1, ec)) mp = mp1; else if (std::filesystem::exists(mp2, ec)) mp = mp2; else return false;
    outPath = mp.string();
    return true;
}
