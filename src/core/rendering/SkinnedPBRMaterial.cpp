#include "SkinnedPBRMaterial.h"

SkinnedPBRMaterial::SkinnedPBRMaterial(const std::string& name, bgfx::ProgramHandle program)
    : PBRMaterial(name, program,
        BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z |
        BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_MSAA | BGFX_STATE_CULL_CW)
{
}   

std::shared_ptr<Material> SkinnedPBRMaterial::Clone() const {
    auto clone = std::make_shared<SkinnedPBRMaterial>(MakeCloneName(GetName()), GetProgram());
    
    // Copy PBRMaterial textures (handles are shared, not owned by the material)
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
    
    // Copy shadow-receive flags (previously dropped on clone)
    clone->m_ReceiveShadowsOverride = m_ReceiveShadowsOverride;
    clone->m_ReceiveShadows = m_ReceiveShadows;

    // Copy state flags
    clone->m_StateFlags = m_StateFlags;

    // Copy ALL generic vec4 uniforms (PSX family, tint params, etc.) so a
    // skinned PSX clone stays a PSX material. See PBRMaterial::Clone.
    CopyUniformValuesTo(*clone);

    // Sync uniforms managed by typed PBR fields (clone will create fresh handles)
    clone->SyncScalarUniforms();
    clone->SyncUVUniform();
    clone->SyncTextureUsageUniform();

    return clone;
}

void SkinnedPBRMaterial::BindUniforms() const
{
    PBRMaterial::BindUniforms();
}
