#include "Material.h"
#include "MaterialPropertyBlock.h"

// === Fast PropertyID-based API ===

void Material::SetUniform(PropertyID id, const glm::vec4& value)
{
    if (!id.IsValid()) return;  // Safety check
    
    EnsureUniformIndexCapacity(id.Value());
    
    int index = m_UniformIndexByID[id.Value()];
    if (index >= 0) {
        // Update existing
        m_UniformsFlat[index].value = value;
    } else {
        // Create new - but validate we have a valid name first
        const std::string& name = PropertyID::GetName(id);
        if (name.empty()) return;  // Can't create uniform without valid name
        
        bgfx::UniformHandle handle = bgfx::createUniform(name.c_str(), bgfx::UniformType::Vec4);
        if (!bgfx::isValid(handle)) return;  // bgfx failed to create uniform
        
        index = static_cast<int>(m_UniformsFlat.size());
        m_UniformsFlat.push_back({ handle, value, id });
        m_UniformIndexByID[id.Value()] = index;
        
        // Also update legacy map for backward compatibility
        m_Uniforms[name] = { handle, value, id };
    }
}

bool Material::TryGetUniform(PropertyID id, glm::vec4& outValue) const
{
    if (id.Value() >= m_UniformIndexByID.size()) return false;
    int index = m_UniformIndexByID[id.Value()];
    if (index < 0) return false;
    outValue = m_UniformsFlat[index].value;
    return true;
}

bgfx::UniformHandle Material::GetUniformHandle(PropertyID id) const
{
    if (id.Value() >= m_UniformIndexByID.size()) return BGFX_INVALID_HANDLE;
    int index = m_UniformIndexByID[id.Value()];
    if (index < 0) return BGFX_INVALID_HANDLE;
    return m_UniformsFlat[index].handle;
}

bgfx::UniformHandle Material::GetOrCreateSamplerHandle(PropertyID id) const
{
    if (!id.IsValid()) {
        return BGFX_INVALID_HANDLE;
    }

    EnsureSamplerIndexCapacity(id.Value());
    bgfx::UniformHandle& sampler = m_SamplerHandlesByID[id.Value()];
    if (bgfx::isValid(sampler)) {
        return sampler;
    }

    const std::string& name = PropertyID::GetName(id);
    if (name.empty()) {
        return BGFX_INVALID_HANDLE;
    }

    sampler = bgfx::createUniform(name.c_str(), bgfx::UniformType::Sampler);
    return sampler;
}

// === Legacy string-based API ===

void Material::SetUniform(const std::string& name, const glm::vec4& value)
{
    PropertyID id = PropertyID::Get(name);
    SetUniform(id, value);
}

bool Material::TryGetUniform(const std::string& name, glm::vec4& outValue) const
{
    auto it = m_Uniforms.find(name);
    if (it == m_Uniforms.end()) return false;
    outValue = it->second.value;
    return true;
}

void Material::BindUniforms() const
{
    // Use flat array for cache-friendly iteration
    for (const auto& uniform : m_UniformsFlat) {
        bgfx::setUniform(uniform.handle, &uniform.value);
    }
}

// === Property Block Application ===

// Helper to get texture stage mappings - lazily initialized after CommonProperties::Initialize()
static const std::unordered_map<uint32_t, uint8_t>& GetTextureStageMap() {
    static std::unordered_map<uint32_t, uint8_t> s_map;
    static bool s_initialized = false;
    if (!s_initialized && CommonProperties::Albedo.IsValid()) {
        s_map[CommonProperties::Albedo.Value()] = 0;
        s_map[CommonProperties::MetallicRoughness.Value()] = 1;
        s_map[CommonProperties::NormalMap.Value()] = 2;
        s_map[CommonProperties::TintMask.Value()] = 3;
        s_map[CommonProperties::AO.Value()] = 4;
        s_map[CommonProperties::Emission.Value()] = 5;
        if (CommonProperties::Displacement.IsValid()) {
            s_map[CommonProperties::Displacement.Value()] = 6;
        }
        s_initialized = true;
    }
    return s_map;
}

static uint8_t ResolveTextureStage(uint32_t propId, uint8_t& nextFreeSlot) {
    const auto& textureStageMap = GetTextureStageMap();
    auto stageIt = textureStageMap.find(propId);
    if (stageIt != textureStageMap.end()) {
        return stageIt->second;
    }
    return nextFreeSlot++;
}

void Material::BuildPackedPropertyOverrides(const MaterialPropertyBlock& block,
                                           PackedPropertyOverrides& out) const
{
    out.Clear();

    auto appendVec4Override = [&](PropertyID id, const glm::vec4& value) {
        const bgfx::UniformHandle handle = GetUniformHandle(id);
        if (!bgfx::isValid(handle)) {
            return;
        }
        out.Vec4Overrides.push_back({ handle, value });
    };

    auto appendTextureOverride = [&](PropertyID id,
                                     bgfx::TextureHandle texture,
                                     uint8_t& nextFreeSlot,
                                     glm::vec4& usageOverride,
                                     bool& usageDirty,
                                     const glm::vec4& baseUsage,
                                     uint16_t maxSlots) {
        if (!bgfx::isValid(texture)) {
            return;
        }

        const bgfx::UniformHandle sampler = GetOrCreateSamplerHandle(id);
        if (!bgfx::isValid(sampler)) {
            return;
        }

        const uint8_t stage = ResolveTextureStage(id.Value(), nextFreeSlot);
        if (maxSlots > 0 && stage >= maxSlots) {
            return;
        }

        out.TextureOverrides.push_back({ sampler, texture, stage });

        if (!CommonProperties::MetallicRoughness.IsValid()) {
            return;
        }

        if (!usageDirty) {
            usageOverride = baseUsage;
            usageDirty = true;
        }

        if (id == CommonProperties::MetallicRoughness) usageOverride.x = 1.0f;
        else if (id == CommonProperties::NormalMap) usageOverride.y = 1.0f;
        else if (id == CommonProperties::AO) usageOverride.z = 1.0f;
        else if (id == CommonProperties::Emission) usageOverride.w = 1.0f;
    };

    glm::vec4 baseUsage(0.0f);
    if (CommonProperties::TextureUsage.IsValid() &&
        CommonProperties::TextureUsage.Value() < m_UniformIndexByID.size()) {
        const int idx = m_UniformIndexByID[CommonProperties::TextureUsage.Value()];
        if (idx >= 0) {
            baseUsage = m_UniformsFlat[idx].value;
        }
    }

    glm::vec4 usageOverride(0.0f);
    bool usageDirty = false;
    uint8_t nextFreeSlot = 12;
    const bgfx::Caps* caps = bgfx::getCaps();
    const uint16_t maxSlots = caps ? caps->limits.maxTextureSamplers : 0;

    if (!block.Vec4ByID.empty() || !block.TexturesByID.empty()) {
        const auto& vecOverrides = block.GetVec4OverridesFlat();
        const auto& textureOverrides = block.GetTextureOverridesFlat();
        out.Vec4Overrides.reserve(vecOverrides.size());
        out.TextureOverrides.reserve(textureOverrides.size());

        for (const auto& entry : vecOverrides) {
            appendVec4Override(PropertyID(entry.PropertyId), entry.Value);
        }

        for (const auto& entry : textureOverrides) {
            appendTextureOverride(PropertyID(entry.PropertyId),
                                  entry.Texture,
                                  nextFreeSlot,
                                  usageOverride,
                                  usageDirty,
                                  baseUsage,
                                  maxSlots);
        }
    } else {
        out.Vec4Overrides.reserve(block.Vec4Uniforms.size());
        out.TextureOverrides.reserve(block.Textures.size());

        for (const auto& kv : block.Vec4Uniforms) {
            appendVec4Override(PropertyID::Get(kv.first), kv.second);
        }

        for (const auto& kv : block.Textures) {
            appendTextureOverride(PropertyID::Get(kv.first),
                                  kv.second,
                                  nextFreeSlot,
                                  usageOverride,
                                  usageDirty,
                                  baseUsage,
                                  maxSlots);
        }
    }

    if (usageDirty && CommonProperties::TextureUsage.IsValid()) {
        out.TextureUsageHandle = GetUniformHandle(CommonProperties::TextureUsage);
        out.TextureUsageValue = usageOverride;
        out.TextureUsageDirty = bgfx::isValid(out.TextureUsageHandle);
    }
}

void Material::ApplyPackedPropertyOverrides(const PackedPropertyOverrides& packed) const
{
    for (const auto& entry : packed.Vec4Overrides) {
        if (bgfx::isValid(entry.Handle)) {
            bgfx::setUniform(entry.Handle, &entry.Value);
        }
    }

    for (const auto& entry : packed.TextureOverrides) {
        if (bgfx::isValid(entry.Sampler) && bgfx::isValid(entry.Texture)) {
            bgfx::setTexture(entry.Stage, entry.Sampler, entry.Texture);
        }
    }

    if (packed.TextureUsageDirty && bgfx::isValid(packed.TextureUsageHandle)) {
        bgfx::setUniform(packed.TextureUsageHandle, &packed.TextureUsageValue);
    }
}

// Fast path using PropertyID-based lookups
void Material::ApplyPropertyBlockFast(const MaterialPropertyBlock& block) const
{
    const auto& vecOverrides = block.GetVec4OverridesFlat();
    const auto& textureOverrides = block.GetTextureOverridesFlat();

    // Vec4 overrides - cache-friendly flat iteration over the already-dirtied
    // PropertyID map instead of paying unordered_map bucket walks per draw.
    for (const auto& entry : vecOverrides) {
        uint32_t propId = entry.PropertyId;
        if (propId < m_UniformIndexByID.size()) {
            int index = m_UniformIndexByID[propId];
            if (index >= 0) {
                bgfx::setUniform(m_UniformsFlat[index].handle, &entry.Value);
            }
        }
    }
    
    glm::vec4 usageOverride(0.0f);
    bool usageDirty = false;
    
    // Get base texture usage from material (only if CommonProperties are initialized)
    glm::vec4 baseUsage(0.0f);
    if (CommonProperties::TextureUsage.IsValid() && 
        CommonProperties::TextureUsage.Value() < m_UniformIndexByID.size()) {
        int idx = m_UniformIndexByID[CommonProperties::TextureUsage.Value()];
        if (idx >= 0) baseUsage = m_UniformsFlat[idx].value;
    }
    
    const bgfx::Caps* caps = bgfx::getCaps();
    const uint16_t maxSlots = caps ? caps->limits.maxTextureSamplers : 0;
    // Reserve stages 7-11 for renderer-owned global samplers (shadow maps,
    // skybox, and GPU skinning atlases). Custom material overrides start above
    // that range so they don't stomp renderer bindings mid-draw.
    uint8_t nextFreeSlot = 12;
    for (const auto& entry : textureOverrides) {
        uint32_t propId = entry.PropertyId;
        bgfx::TextureHandle tex = entry.Texture;
        if (!bgfx::isValid(tex)) continue;

        const bgfx::UniformHandle sampler = GetOrCreateSamplerHandle(PropertyID(propId));
        if (!bgfx::isValid(sampler)) continue;

        const uint8_t stage = ResolveTextureStage(propId, nextFreeSlot);
        if (maxSlots > 0 && stage >= maxSlots) {
            continue;
        }
        
        bgfx::setTexture(stage, sampler, tex);
        
        // Update texture usage flags (only if CommonProperties are initialized)
        if (CommonProperties::MetallicRoughness.IsValid()) {
            if (!usageDirty) {
                usageOverride = baseUsage;
                usageDirty = true;
            }
            if (propId == CommonProperties::MetallicRoughness.Value()) usageOverride.x = 1.0f;
            else if (propId == CommonProperties::NormalMap.Value()) usageOverride.y = 1.0f;
            else if (propId == CommonProperties::AO.Value()) usageOverride.z = 1.0f;
            else if (propId == CommonProperties::Emission.Value()) usageOverride.w = 1.0f;
        }
    }
    
    // Apply texture usage if modified (only if CommonProperties are initialized)
    if (usageDirty && CommonProperties::TextureUsage.IsValid() &&
        CommonProperties::TextureUsage.Value() < m_UniformIndexByID.size()) {
        int idx = m_UniformIndexByID[CommonProperties::TextureUsage.Value()];
        if (idx >= 0) {
            bgfx::setUniform(m_UniformsFlat[idx].handle, &usageOverride);
        }
    }
}


// Legacy path for backward compatibility (uses string maps)
void Material::ApplyPropertyBlock(const MaterialPropertyBlock& block) const
{
    // If block has ID-based data, use fast path
    if (!block.Vec4ByID.empty() || !block.TexturesByID.empty()) {
        ApplyPropertyBlockFast(block);
        return;
    }
    
    // Fall back to string-based path for legacy data
    for (const auto& kv : block.Vec4Uniforms)
    {
        auto it = m_Uniforms.find(kv.first);
        if (it == m_Uniforms.end())
            continue;
        bgfx::setUniform(it->second.handle, &kv.second);
    }

    auto usageIt = m_Uniforms.find("u_TextureUsage");
    bool usageDirty = false;
    glm::vec4 usageOverride(0.0f);
    auto ensureUsage = [&]() {
        if (!usageDirty && usageIt != m_Uniforms.end()) {
            usageOverride = usageIt->second.value;
            usageDirty = true;
        }
    };

    // Reserve stages 7-11 for renderer-owned global samplers (shadow maps,
    // skybox, and GPU skinning atlases).
    uint8_t nextFreeSlot = 12;
    for (const auto& kv : block.Textures)
    {
        const PropertyID id = PropertyID::Get(kv.first);
        bgfx::UniformHandle sampler = GetOrCreateSamplerHandle(id);
        if (bgfx::isValid(sampler) && bgfx::isValid(kv.second))
        {
            uint8_t stage = ResolveTextureStage(id.Value(), nextFreeSlot);
            bgfx::setTexture(stage, sampler, kv.second);
            if (usageIt != m_Uniforms.end())
            {
                if (kv.first == "s_metallicRoughness") { ensureUsage(); usageOverride.x = 1.0f; }
                else if (kv.first == "s_normalMap")    { ensureUsage(); usageOverride.y = 1.0f; }
                else if (kv.first == "s_ao")           { ensureUsage(); usageOverride.z = 1.0f; }
                else if (kv.first == "s_emission")     { ensureUsage(); usageOverride.w = 1.0f; }
            }
        }
    }

    if (usageDirty && usageIt != m_Uniforms.end())
    {
        bgfx::setUniform(usageIt->second.handle, &usageOverride);
    }
}
