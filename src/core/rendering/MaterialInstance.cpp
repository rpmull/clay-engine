#include "MaterialInstance.h"
#include "MaterialCache.h"
#include <cassert>

namespace
{
void RefreshPathBackedOverrides(MaterialPropertyBlock& block,
                                const std::unordered_map<std::string, std::string>& texturePaths)
{
    for (const auto& [name, path] : texturePaths)
    {
        if (path.empty())
        {
            block.SetTexture(name, BGFX_INVALID_HANDLE);
            continue;
        }

        TextureSpecifier spec;
        spec.Path = path;
        block.SetTexture(name, AcquireTextureHandle(spec, TextureColorSpace::Linear));
    }
}
}

// === MaterialInstance Implementation ===

MaterialInstance::MaterialInstance(std::shared_ptr<Material> parent)
    : Material(parent->GetName() + "_Instance", parent->GetProgram(), parent->GetStateFlags())
    , m_Parent(std::move(parent))
{
    assert(m_Parent && "MaterialInstance requires a valid parent material");
    SetBindCacheSafe(false);
}

std::shared_ptr<MaterialInstance> MaterialInstance::Create(std::shared_ptr<Material> parent)
{
    if (!parent) return nullptr;
    return std::shared_ptr<MaterialInstance>(new MaterialInstance(std::move(parent)));
}

std::shared_ptr<MaterialInstance> MaterialInstance::Create(
    std::shared_ptr<Material> parent, 
    const MaterialPropertyBlock& initialOverrides)
{
    auto instance = Create(std::move(parent));
    if (instance) {
        instance->CopyOverridesFrom(initialOverrides);
    }
    return instance;
}

std::shared_ptr<Material> MaterialInstance::Clone() const
{
    if (!m_Parent) {
        return nullptr;
    }

    auto clone = Create(m_Parent);
    if (!clone) {
        return nullptr;
    }

    clone->m_StateFlags = GetStateFlags();
    clone->m_Overrides = m_Overrides;
    clone->m_TexturePaths = m_TexturePaths;
    return clone;
}

// === PropertyID-based API ===

void MaterialInstance::SetVector(PropertyID id, const glm::vec4& value)
{
    m_Overrides.SetVector(id, value);
}

void MaterialInstance::SetTexture(PropertyID id, bgfx::TextureHandle texture)
{
    m_Overrides.SetTexture(id, texture);
    const std::string& name = PropertyID::GetName(id);
    if (!name.empty()) {
        m_TexturePaths.erase(name);
    }
}

void MaterialInstance::SetTexture(PropertyID id, bgfx::TextureHandle texture, const std::string& sourcePath)
{
    m_Overrides.SetTexture(id, texture);
    const std::string& name = PropertyID::GetName(id);
    if (!sourcePath.empty()) {
        if (!name.empty()) {
            m_TexturePaths[name] = sourcePath;
        }
    } else if (!name.empty()) {
        m_TexturePaths.erase(name);
    }
}

bool MaterialInstance::HasOverride(PropertyID id) const
{
    return m_Overrides.HasVector(id) || m_Overrides.HasTexture(id);
}

void MaterialInstance::ClearOverride(PropertyID id)
{
    m_Overrides.RemoveVector(id);
    m_Overrides.RemoveTexture(id);
    
    const std::string& name = PropertyID::GetName(id);
    if (!name.empty()) {
        m_TexturePaths.erase(name);
    }
}

void MaterialInstance::ClearAllOverrides()
{
    m_Overrides.Clear();
    m_TexturePaths.clear();
}

// === String-based API ===

void MaterialInstance::SetVector(const std::string& name, const glm::vec4& value)
{
    m_Overrides.SetVector(name, value);
}

void MaterialInstance::SetTexture(const std::string& name, bgfx::TextureHandle texture)
{
    m_Overrides.SetTexture(name, texture);
    m_TexturePaths.erase(name);
}

// === Material interface ===

void MaterialInstance::BindUniforms() const
{
    // First bind parent's uniforms
    if (m_Parent) {
        m_Parent->BindUniforms();
    }
    
    // Then apply our overrides on top
    if (!m_Overrides.Empty()) {
        auto* self = const_cast<MaterialInstance*>(this);
        RefreshPathBackedOverrides(self->m_Overrides, self->m_TexturePaths);
        ApplyPropertyBlock(m_Overrides);
    }
}

// === Copy operations ===

void MaterialInstance::CopyOverridesFrom(const MaterialInstance& other)
{
    m_Overrides = other.m_Overrides;
    m_TexturePaths = other.m_TexturePaths;
}

void MaterialInstance::CopyOverridesFrom(const MaterialPropertyBlock& block)
{
    m_Overrides = block;
    m_TexturePaths.clear();
}

// === Utility functions ===

namespace MaterialInstancing {

std::shared_ptr<MaterialInstance> EnsureInstance(std::shared_ptr<Material>& material)
{
    if (!material) return nullptr;
    
    // Already an instance?
    if (auto instance = std::dynamic_pointer_cast<MaterialInstance>(material)) {
        return instance;
    }
    
    // Create new instance
    auto instance = MaterialInstance::Create(material);
    material = instance;
    return instance;
}

bool IsInstance(const std::shared_ptr<Material>& material)
{
    return std::dynamic_pointer_cast<const MaterialInstance>(material) != nullptr;
}

std::shared_ptr<Material> GetRootMaterial(const std::shared_ptr<Material>& material)
{
    if (!material) return nullptr;
    
    auto current = material;
    while (auto instance = std::dynamic_pointer_cast<MaterialInstance>(current)) {
        current = instance->GetParent();
        if (!current) break;
    }
    return current;
}

} // namespace MaterialInstancing

