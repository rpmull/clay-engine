#pragma once
#include "Material.h"
#include <bgfx/bgfx.h>
#include <cstdint>
#include <string>
#include <glm/glm.hpp>

class PBRMaterial : public Material
   {
   public:
      static constexpr float kDefaultMetallic = 0.0f;
      static constexpr float kDefaultRoughness = 0.5f;
      static constexpr float kDefaultNormalScale = 1.0f;
      static constexpr float kDefaultAO = 1.0f;
      static constexpr float kDefaultEmissionStrength = 0.0f;
      static constexpr float kDefaultDisplacementScale = 0.0f;

      PBRMaterial(const std::string& name, bgfx::ProgramHandle program);

      PBRMaterial(const std::string& name,
          bgfx::ProgramHandle program,
          uint64_t stateFlags);

      // Destructor to clean up sampler handles
      virtual ~PBRMaterial();

      // Create a deep copy with fresh bgfx handles
      std::shared_ptr<Material> Clone() const override;

      void SetAlbedoTexture(bgfx::TextureHandle texture, const std::string& sourcePath = std::string());
      void SetMetallicRoughnessTexture(bgfx::TextureHandle texture, const std::string& sourcePath = std::string());
      void SetNormalTexture(bgfx::TextureHandle texture, const std::string& sourcePath = std::string());
      void SetAmbientOcclusionTexture(bgfx::TextureHandle texture, const std::string& sourcePath = std::string());
      void SetEmissionTexture(bgfx::TextureHandle texture, const std::string& sourcePath = std::string());
      void SetDisplacementTexture(bgfx::TextureHandle texture, const std::string& sourcePath = std::string());
      void SetTintMaskTexture(bgfx::TextureHandle texture, const std::string& sourcePath = std::string());

      // Convenience setters that also remember source paths for serialization
      void SetAlbedoTextureFromPath(const std::string& path);
      void SetMetallicRoughnessTextureFromPath(const std::string& path);
      void SetNormalTextureFromPath(const std::string& path);
      void SetAmbientOcclusionTextureFromPath(const std::string& path);
      void SetEmissionTextureFromPath(const std::string& path);
      void SetDisplacementTextureFromPath(const std::string& path);
      void SetTintMaskTextureFromPath(const std::string& path);

      void SetMetallic(float value);
      void SetRoughness(float value);
      void SetNormalScale(float value);
      void SetAmbientOcclusion(float value);
      void SetEmissionStrength(float value);
      void SetEmissionColor(const glm::vec3& color);
      void SetDisplacementScale(float value);
      void SetUVScale(const glm::vec2& scale);
      void SetUVOffset(const glm::vec2& offset);
      void SetUVTransform(const glm::vec2& scale, const glm::vec2& offset);
      void SetReceiveShadowsOverride(bool enabled);
      void SetReceiveShadows(bool enabled);

      float GetMetallic() const { return m_Metallic; }
      float GetRoughness() const { return m_Roughness; }
      float GetNormalScale() const { return m_NormalScale; }
      float GetAmbientOcclusion() const { return m_AOScalar; }
      float GetEmissionStrength() const { return m_EmissionStrength; }
      glm::vec3 GetEmissionColor() const { return m_EmissionColor; }
      float GetDisplacementScale() const { return m_DisplacementScale; }
      glm::vec2 GetUVScale() const { return glm::vec2(m_UVTransform.x, m_UVTransform.y); }
      glm::vec2 GetUVOffset() const { return glm::vec2(m_UVTransform.z, m_UVTransform.w); }
      bool GetReceiveShadowsOverride() const { return m_ReceiveShadowsOverride; }
      bool GetReceiveShadows() const { return m_ReceiveShadows; }

      void BindUniforms() const override;
      
      bgfx::TextureHandle m_AlbedoTex;
      bgfx::TextureHandle m_MetallicRoughnessTex;
      bgfx::TextureHandle m_NormalTex;
      bgfx::TextureHandle m_AOTex;
      bgfx::TextureHandle m_EmissionTex;
      bgfx::TextureHandle m_DisplacementTex;
      mutable bgfx::TextureHandle m_TintMaskTex = BGFX_INVALID_HANDLE;

      // Persistable source paths for textures (optional)
      const std::string& GetAlbedoPath() const { return m_AlbedoPath; }
      const std::string& GetMetallicRoughnessPath() const { return m_MetallicRoughnessPath; }
      const std::string& GetNormalPath() const { return m_NormalPath; }
      const std::string& GetTintMaskPath() const { return m_TintMaskPath; }
      const std::string& GetAOPath() const { return m_AOPath; }
      const std::string& GetEmissionPath() const { return m_EmissionPath; }
      const std::string& GetDisplacementPath() const { return m_DisplacementPath; }

   protected:
      void RefreshPathBackedTextures() const;
      void SyncScalarUniforms();
      void SyncUVUniform();
      void SyncTextureUsageUniform() const;
      
      bgfx::UniformHandle u_AlbedoSampler = BGFX_INVALID_HANDLE;
      bgfx::UniformHandle u_MetallicRoughnessSampler = BGFX_INVALID_HANDLE;
      bgfx::UniformHandle u_NormalSampler = BGFX_INVALID_HANDLE;
      bgfx::UniformHandle u_AOSampler = BGFX_INVALID_HANDLE;
      bgfx::UniformHandle u_EmissionSampler = BGFX_INVALID_HANDLE;
      bgfx::UniformHandle u_TintMaskSampler = BGFX_INVALID_HANDLE;
      bgfx::UniformHandle u_DisplacementSampler = BGFX_INVALID_HANDLE;

      std::string m_AlbedoPath;
      std::string m_MetallicRoughnessPath;
      std::string m_NormalPath;
      std::string m_AOPath;
      std::string m_EmissionPath;
      std::string m_DisplacementPath;
      mutable std::string m_TintMaskPath;

      float m_Metallic = kDefaultMetallic;
      float m_Roughness = kDefaultRoughness;
      float m_NormalScale = kDefaultNormalScale;
      float m_AOScalar = kDefaultAO;
      float m_EmissionStrength = kDefaultEmissionStrength;
      float m_DisplacementScale = kDefaultDisplacementScale;
      glm::vec3 m_EmissionColor = glm::vec3(1.0f);
      glm::vec4 m_UVTransform = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
      mutable glm::vec4 m_TextureUsage = glm::vec4(0.0f);
      mutable uint64_t m_LastTextureCacheGeneration = 0;
      mutable bool m_PathBackedTexturesDirty = true;
      bool m_ReceiveShadowsOverride = false;
      bool m_ReceiveShadows = false;
   };
