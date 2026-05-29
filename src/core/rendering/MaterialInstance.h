#pragma once

#include "Material.h"
#include "MaterialPropertyBlock.h"
#include <memory>

/**
 * MaterialInstance - Lightweight per-instance material variation
 * 
 * Similar to Unreal's Dynamic Material Instance (DMI), this class allows
 * per-object material parameter overrides without duplicating the entire
 * material. The instance shares the parent's shader program and base state,
 * only storing the delta/overrides.
 * 
 * Usage:
 *   auto instance = MaterialInstance::Create(baseMaterial);
 *   instance->SetVector(CommonProperties::ColorTint, glm::vec4(1, 0, 0, 1));
 *   instance->SetTexture(CommonProperties::Albedo, myTexture);
 *   meshComponent->material = instance;  // Can be used anywhere a Material is expected
 * 
 * Benefits:
 *   - Shares shader program with parent (no shader switch cost)
 *   - Only stores overridden properties (memory efficient)
 *   - Can be created at runtime without asset overhead
 *   - Batching-friendly: same parent = potential batch
 */
class MaterialInstance : public Material
{
public:
    // Create a new instance from a parent material
    static std::shared_ptr<MaterialInstance> Create(std::shared_ptr<Material> parent);
    
    // Create from a parent with initial property overrides
    static std::shared_ptr<MaterialInstance> Create(
        std::shared_ptr<Material> parent, 
        const MaterialPropertyBlock& initialOverrides);
    
    virtual ~MaterialInstance() = default;

    std::shared_ptr<Material> Clone() const override;
    
    // === Override API (PropertyID-based, fast) ===
    
    void SetVector(PropertyID id, const glm::vec4& value);
    void SetTexture(PropertyID id, bgfx::TextureHandle texture);
    void SetTexture(PropertyID id, bgfx::TextureHandle texture, const std::string& sourcePath);
    
    bool HasOverride(PropertyID id) const;
    void ClearOverride(PropertyID id);
    void ClearAllOverrides();
    
    // === Override API (string-based, backward compatible) ===
    
    void SetVector(const std::string& name, const glm::vec4& value);
    void SetTexture(const std::string& name, bgfx::TextureHandle texture);
    
    // === Material interface overrides ===
    
    void BindUniforms() const override;
    
    // Get the parent material
    std::shared_ptr<Material> GetParent() const { return m_Parent; }
    
    // Get the overrides block (for inspection/serialization)
    const MaterialPropertyBlock& GetOverrides() const { return m_Overrides; }
    MaterialPropertyBlock& GetOverridesMutable() { return m_Overrides; }
    
    // Get texture source paths (for serialization)
    const std::unordered_map<std::string, std::string>& GetTexturePaths() const { return m_TexturePaths; }
    
    // Check if this instance has any overrides
    bool HasAnyOverrides() const { return !m_Overrides.Empty(); }
    
    // Copy overrides from another instance
    void CopyOverridesFrom(const MaterialInstance& other);
    void CopyOverridesFrom(const MaterialPropertyBlock& block);

protected:
    MaterialInstance(std::shared_ptr<Material> parent);
    
private:
    std::shared_ptr<Material> m_Parent;
    MaterialPropertyBlock m_Overrides;
    std::unordered_map<std::string, std::string> m_TexturePaths;  // For serialization
};

/**
 * Utility functions for material instancing
 */
namespace MaterialInstancing {
    // Create an instance if the material isn't already one, or return as-is if it is
    std::shared_ptr<MaterialInstance> EnsureInstance(std::shared_ptr<Material>& material);
    
    // Check if a material is an instance
    bool IsInstance(const std::shared_ptr<Material>& material);
    
    // Get the root parent (non-instance) material
    std::shared_ptr<Material> GetRootMaterial(const std::shared_ptr<Material>& material);
}

