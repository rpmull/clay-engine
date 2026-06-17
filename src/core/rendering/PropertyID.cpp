#include "PropertyID.h"
#include <cassert>

// PropertyID static methods
PropertyID PropertyID::Get(const std::string& name) {
    return PropertyRegistry::Instance().GetOrCreate(name);
}

PropertyID PropertyID::Get(const char* name) {
    return PropertyRegistry::Instance().GetOrCreate(std::string(name));
}

const std::string& PropertyID::GetName(PropertyID id) {
    return PropertyRegistry::Instance().GetName(id);
}

void PropertyID::RegisterCommonProperties() {
    PropertyRegistry::Instance().RegisterCommonProperties();
}

// PropertyRegistry implementation
PropertyRegistry& PropertyRegistry::Instance() {
    static PropertyRegistry instance;
    return instance;
}

PropertyID PropertyRegistry::GetOrCreate(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    auto it = m_nameToId.find(name);
    if (it != m_nameToId.end()) {
        return PropertyID(it->second);
    }
    
    uint32_t id = m_nextId++;
    m_nameToId[name] = id;
    m_idToName.push_back(name);
    
    return PropertyID(id);
}

const std::string& PropertyRegistry::GetName(PropertyID id) const {
    static const std::string empty;
    // IDs are 1-based, so valid IDs range from 1 to m_idToName.size()
    if (!id.IsValid() || id.Value() > m_idToName.size()) {
        return empty;
    }
    // id.Value() - 1 is the 0-based index into the array
    return m_idToName[id.Value() - 1];
}

bgfx::UniformHandle PropertyRegistry::GetUniformHandle(PropertyID id, bgfx::UniformType::Enum type) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    auto it = m_uniformHandles.find(id.Value());
    if (it != m_uniformHandles.end()) {
        return it->second;
    }
    
    // Create new handle
    const std::string& name = GetName(id);
    if (name.empty()) {
        return BGFX_INVALID_HANDLE;
    }
    
    bgfx::UniformHandle handle = bgfx::createUniform(name.c_str(), type);
    if (bgfx::isValid(handle)) {
        m_uniformHandles[id.Value()] = handle;
    }
    return handle;
}

void PropertyRegistry::RegisterCommonProperties() {
    CommonProperties::Initialize();
}

// Common property definitions
namespace CommonProperties {
    PropertyID Albedo;
    PropertyID MetallicRoughness;
    PropertyID NormalMap;
    PropertyID AO;
    PropertyID Emission;
    PropertyID TintMask;
    PropertyID Displacement;
    
    PropertyID ColorTint;
    PropertyID TextureUsage;
    PropertyID UVScaleOffset;
    PropertyID Metallic;
    PropertyID Roughness;
    PropertyID NormalScale;
    PropertyID EmissionStrength;
    PropertyID EmissionColor;
    
    PropertyID PSXParams;
    PropertyID PSXWorld;
    PropertyID ToonParams;
    PropertyID Posterize;
    PropertyID PSXShadowParams;
    PropertyID PSXEmission;

    static bool s_initialized = false;
    
    void Initialize() {
        if (s_initialized) return;
        s_initialized = true;
        
        // Texture samplers
        Albedo = PropertyID::Get("s_albedo");
        MetallicRoughness = PropertyID::Get("s_metallicRoughness");
        NormalMap = PropertyID::Get("s_normalMap");
        AO = PropertyID::Get("s_ao");
        Emission = PropertyID::Get("s_emission");
        TintMask = PropertyID::Get("s_tintMask");
        Displacement = PropertyID::Get("s_displacement");
        
        // Vec4 uniforms
        ColorTint = PropertyID::Get("u_ColorTint");
        TextureUsage = PropertyID::Get("u_TextureUsage");
        UVScaleOffset = PropertyID::Get("u_UVScaleOffset");
        Metallic = PropertyID::Get("u_Metallic");
        Roughness = PropertyID::Get("u_Roughness");
        NormalScale = PropertyID::Get("u_NormalScale");
        EmissionStrength = PropertyID::Get("u_EmissionStrength");
        EmissionColor = PropertyID::Get("u_EmissionColor");
        
        // PSX
        PSXParams = PropertyID::Get("u_psxParams");
        PSXWorld = PropertyID::Get("u_psxWorld");
        ToonParams = PropertyID::Get("u_toonParams");
        Posterize = PropertyID::Get("u_posterize");
        PSXShadowParams = PropertyID::Get("u_psxShadowParams");
        PSXEmission = PropertyID::Get("u_psxEmission");
    }
}

