#include "editor/pipeline/ModelImportSettings.h"
#include <fstream>
#include <iostream>
#include <algorithm>

using json = nlohmann::json;

void MeshMaterialPreset::ApplyTo(MaterialSource& target) const
{
    // If using a custom material asset, that takes precedence
    // (handled separately at instantiation time)
    if (UseCustomMaterial && !MaterialAssetPath.empty())
        return;
    
    // Apply individual texture overrides
    if (OverrideAlbedo && !AlbedoPath.empty())
    {
        target.Albedo.Path = AlbedoPath;
        target.Albedo.Embedded = EmbeddedTextureData{};
    }
    
    if (OverrideNormal && !NormalPath.empty())
    {
        target.Normal.Path = NormalPath;
        target.Normal.Embedded = EmbeddedTextureData{};
    }
    
    if (OverrideMetallicRoughness && !MetallicRoughnessPath.empty())
    {
        target.MetallicRoughness.Path = MetallicRoughnessPath;
        target.MetallicRoughness.Embedded = EmbeddedTextureData{};
    }
    
    if (OverrideAO && !AOPath.empty())
    {
        target.AO.Path = AOPath;
        target.AO.Embedded = EmbeddedTextureData{};
    }
    
    if (OverrideEmission && !EmissionPath.empty())
    {
        target.Emission.Path = EmissionPath;
        target.Emission.Embedded = EmbeddedTextureData{};
    }

    if (OverrideDisplacement && !DisplacementPath.empty())
    {
        target.Displacement.Path = DisplacementPath;
        target.Displacement.Embedded = EmbeddedTextureData{};
    }
    
    if (OverrideTint)
    {
        target.HasTint = true;
        target.ColorTint = ColorTint;
    }
    
    if (OverrideAlphaBlend) {
        // Alpha mode: cutout and blend are mutually exclusive
        target.AlphaCutout = AlphaCutout;
        target.AlphaCutoutThreshold = AlphaCutoutThreshold;
        target.AlphaBlend = AlphaCutout ? false : AlphaBlend;
    }
    
    if (OverrideTwoSided)
        target.TwoSided = TwoSided;
}

json MeshMaterialPreset::ToJson() const
{
    json j;
    j["meshName"] = MeshName;
    j["materialSlot"] = MaterialSlot;
    
    if (OverrideAlbedo)
    {
        j["albedo"] = AlbedoPath;
    }
    if (OverrideNormal)
    {
        j["normal"] = NormalPath;
    }
    if (OverrideMetallicRoughness)
    {
        j["metallicRoughness"] = MetallicRoughnessPath;
    }
    if (OverrideAO)
    {
        j["ao"] = AOPath;
    }
    if (OverrideEmission)
    {
        j["emission"] = EmissionPath;
    }
    if (OverrideDisplacement)
    {
        j["displacement"] = DisplacementPath;
    }
    if (OverrideTint)
    {
        j["tint"] = { ColorTint.r, ColorTint.g, ColorTint.b, ColorTint.a };
    }
    if (OverrideAlphaBlend)
    {
        j["alphaBlend"] = AlphaBlend;
        j["alphaCutout"] = AlphaCutout;
        j["alphaCutoutThreshold"] = AlphaCutoutThreshold;
    }
    if (OverrideTwoSided)
    {
        j["twoSided"] = TwoSided;
    }
    if (UseCustomMaterial)
    {
        j["customMaterial"] = MaterialAssetPath;
    }
    
    return j;
}

MeshMaterialPreset MeshMaterialPreset::FromJson(const json& j)
{
    MeshMaterialPreset preset;
    preset.MeshName = j.value("meshName", std::string());
    preset.MaterialSlot = j.value("materialSlot", 0);
    
    if (j.contains("albedo"))
    {
        preset.OverrideAlbedo = true;
        preset.AlbedoPath = j["albedo"].get<std::string>();
    }
    if (j.contains("normal"))
    {
        preset.OverrideNormal = true;
        preset.NormalPath = j["normal"].get<std::string>();
    }
    if (j.contains("metallicRoughness"))
    {
        preset.OverrideMetallicRoughness = true;
        preset.MetallicRoughnessPath = j["metallicRoughness"].get<std::string>();
    }
    if (j.contains("ao"))
    {
        preset.OverrideAO = true;
        preset.AOPath = j["ao"].get<std::string>();
    }
    if (j.contains("emission"))
    {
        preset.OverrideEmission = true;
        preset.EmissionPath = j["emission"].get<std::string>();
    }
    if (j.contains("displacement"))
    {
        preset.OverrideDisplacement = true;
        preset.DisplacementPath = j["displacement"].get<std::string>();
    }
    if (j.contains("tint") && j["tint"].is_array() && j["tint"].size() >= 4)
    {
        preset.OverrideTint = true;
        preset.ColorTint.r = j["tint"][0].get<float>();
        preset.ColorTint.g = j["tint"][1].get<float>();
        preset.ColorTint.b = j["tint"][2].get<float>();
        preset.ColorTint.a = j["tint"][3].get<float>();
    }
    if (j.contains("alphaBlend"))
    {
        preset.OverrideAlphaBlend = true;
        preset.AlphaBlend = j["alphaBlend"].get<bool>();
    }
    if (j.contains("alphaCutout"))
    {
        preset.OverrideAlphaBlend = true;
        preset.AlphaCutout = j["alphaCutout"].get<bool>();
    }
    if (j.contains("alphaCutoutThreshold"))
    {
        preset.OverrideAlphaBlend = true;
        preset.AlphaCutoutThreshold = j["alphaCutoutThreshold"].get<float>();
    }
    if (j.contains("twoSided"))
    {
        preset.OverrideTwoSided = true;
        preset.TwoSided = j["twoSided"].get<bool>();
    }
    if (j.contains("customMaterial"))
    {
        preset.UseCustomMaterial = true;
        preset.MaterialAssetPath = j["customMaterial"].get<std::string>();
    }
    
    return preset;
}

const MeshMaterialPreset* ModelImportSettings::FindPreset(const std::string& meshName, int slot) const
{
    for (const auto& preset : MaterialPresets)
    {
        if (preset.MeshName == meshName && preset.MaterialSlot == slot)
            return &preset;
    }
    return nullptr;
}

MeshMaterialPreset* ModelImportSettings::FindOrCreatePreset(const std::string& meshName, int slot)
{
    for (auto& preset : MaterialPresets)
    {
        if (preset.MeshName == meshName && preset.MaterialSlot == slot)
            return &preset;
    }
    
    // Create new preset
    MeshMaterialPreset newPreset;
    newPreset.MeshName = meshName;
    newPreset.MaterialSlot = slot;
    MaterialPresets.push_back(newPreset);
    return &MaterialPresets.back();
}

void ModelImportSettings::RemovePreset(const std::string& meshName, int slot)
{
    MaterialPresets.erase(
        std::remove_if(MaterialPresets.begin(), MaterialPresets.end(),
            [&](const MeshMaterialPreset& p) {
                return p.MeshName == meshName && p.MaterialSlot == slot;
            }),
        MaterialPresets.end());
}

const MeshMaterialPreset* ModelImportSettings::FindSharedPreset(const std::string& materialName) const
{
    for (const auto& preset : SharedMaterialPresets)
    {
        if (preset.MeshName == materialName)
            return &preset;
    }
    return nullptr;
}

MeshMaterialPreset* ModelImportSettings::FindOrCreateSharedPreset(const std::string& materialName)
{
    for (auto& preset : SharedMaterialPresets)
    {
        if (preset.MeshName == materialName)
            return &preset;
    }

    MeshMaterialPreset newPreset;
    newPreset.MeshName = materialName; // MeshName field stores the material name here
    newPreset.MaterialSlot = 0;
    SharedMaterialPresets.push_back(newPreset);
    return &SharedMaterialPresets.back();
}

const MeshMaterialPreset* ModelImportSettings::ResolvePreset(const std::string& meshName, int slot,
                                                             const std::string& materialName) const
{
    // Per-mesh override is the most specific and always wins.
    if (const MeshMaterialPreset* perMesh = FindPreset(meshName, slot))
        return perMesh;
    // Fall back to the shared, material-name-keyed override.
    if (!materialName.empty())
        return FindSharedPreset(materialName);
    return nullptr;
}

json ModelImportSettings::ToJson() const
{
    json j;
    j["generateTangents"] = GenerateTangents;
    j["flipUVs"] = FlipUVs;
    j["importScale"] = ImportScale;
    
    if (!MaterialPresets.empty())
    {
        j["materialPresets"] = json::array();
        for (const auto& preset : MaterialPresets)
        {
            j["materialPresets"].push_back(preset.ToJson());
        }
    }

    if (!SharedMaterialPresets.empty())
    {
        j["sharedMaterialPresets"] = json::array();
        for (const auto& preset : SharedMaterialPresets)
        {
            // Reuse the per-preset serializer, but key by material name instead of
            // the (meshName, slot) identity fields.
            json pj = preset.ToJson();
            pj.erase("meshName");
            pj.erase("materialSlot");
            pj["materialName"] = preset.MeshName;
            j["sharedMaterialPresets"].push_back(pj);
        }
    }

    return j;
}

ModelImportSettings ModelImportSettings::FromJson(const json& j)
{
    ModelImportSettings settings;
    settings.GenerateTangents = j.value("generateTangents", true);
    settings.FlipUVs = j.value("flipUVs", true);
    settings.ImportScale = j.value("importScale", 1.0f);
    
    if (j.contains("materialPresets") && j["materialPresets"].is_array())
    {
        for (const auto& presetJ : j["materialPresets"])
        {
            settings.MaterialPresets.push_back(MeshMaterialPreset::FromJson(presetJ));
        }
    }

    if (j.contains("sharedMaterialPresets") && j["sharedMaterialPresets"].is_array())
    {
        for (const auto& presetJ : j["sharedMaterialPresets"])
        {
            MeshMaterialPreset preset = MeshMaterialPreset::FromJson(presetJ);
            preset.MeshName = presetJ.value("materialName", std::string());
            preset.MaterialSlot = 0;
            settings.SharedMaterialPresets.push_back(preset);
        }
    }

    return settings;
}

bool ModelImportSettings::LoadFromMeta(const std::string& metaPath, ModelImportSettings& out)
{
    try
    {
        std::ifstream in(metaPath);
        if (!in.is_open())
            return false;
        
        json j;
        in >> j;
        in.close();
        
        if (j.contains("importSettings"))
        {
            out = FromJson(j["importSettings"]);
            return true;
        }
        
        // No import settings found, return defaults
        out = ModelImportSettings{};
        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[ModelImportSettings] Failed to load from " << metaPath << ": " << e.what() << std::endl;
        return false;
    }
}

bool ModelImportSettings::SaveToMeta(const std::string& metaPath, const ModelImportSettings& settings)
{
    try
    {
        // Read existing meta
        json j;
        {
            std::ifstream in(metaPath);
            if (in.is_open())
            {
                in >> j;
                in.close();
            }
        }
        
        // Update import settings section
        j["importSettings"] = settings.ToJson();
        
        // Write back
        std::ofstream out(metaPath);
        if (!out.is_open())
        {
            std::cerr << "[ModelImportSettings] Failed to open " << metaPath << " for writing" << std::endl;
            return false;
        }
        
        out << j.dump(4);
        out.close();
        
        std::cout << "[ModelImportSettings] Saved import settings to " << metaPath << std::endl;
        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[ModelImportSettings] Failed to save to " << metaPath << ": " << e.what() << std::endl;
        return false;
    }
}

