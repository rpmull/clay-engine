#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include "core/rendering/MaterialSource.h"

// Material preset for a specific mesh slot in a model
struct MeshMaterialPreset
{
    std::string MeshName;           // Name of the mesh node this applies to
    int MaterialSlot = 0;           // Material slot index within the mesh
    
    // Material override settings - only applied if Override* flag is set
    bool OverrideAlbedo = false;
    std::string AlbedoPath;
    
    bool OverrideNormal = false;
    std::string NormalPath;
    
    bool OverrideMetallicRoughness = false;
    std::string MetallicRoughnessPath;
    
    bool OverrideAO = false;
    std::string AOPath;
    
    bool OverrideEmission = false;
    std::string EmissionPath;

    bool OverrideDisplacement = false;
    std::string DisplacementPath;
    
    bool OverrideTint = false;
    glm::vec4 ColorTint = glm::vec4(1.0f);
    
    bool OverrideAlphaBlend = false;
    bool AlphaBlend = false;
    bool AlphaCutout = false;
    float AlphaCutoutThreshold = 0.5f;
    
    bool OverrideTwoSided = false;
    bool TwoSided = false;
    
    // Full material asset override (takes precedence over individual texture overrides)
    bool UseCustomMaterial = false;
    std::string MaterialAssetPath;  // Path to .mat file
    
    // Apply this preset to a MaterialSource
    void ApplyTo(MaterialSource& target) const;
    
    nlohmann::json ToJson() const;
    static MeshMaterialPreset FromJson(const nlohmann::json& j);
};

// Model-level import settings that persist across reimports
struct ModelImportSettings
{
    // Per-mesh-slot overrides, keyed by (mesh name, slot index).
    std::vector<MeshMaterialPreset> MaterialPresets;

    // Shared overrides keyed by material name. These apply to ANY mesh slot whose
    // material has the given name, including meshes added in later reimports. The
    // MeshName field of each record holds the material name. A per-mesh preset for
    // the same slot takes precedence over a shared one.
    std::vector<MeshMaterialPreset> SharedMaterialPresets;

    // Additional import settings (can be expanded)
    bool GenerateTangents = true;
    bool FlipUVs = true;
    float ImportScale = 1.0f;

    // Find preset for a specific mesh/slot combination
    const MeshMaterialPreset* FindPreset(const std::string& meshName, int slot) const;
    MeshMaterialPreset* FindOrCreatePreset(const std::string& meshName, int slot);
    // Remove a per-mesh preset for a slot if one exists (no-op otherwise).
    void RemovePreset(const std::string& meshName, int slot);

    // Shared (material-name-keyed) preset lookup/creation.
    const MeshMaterialPreset* FindSharedPreset(const std::string& materialName) const;
    MeshMaterialPreset* FindOrCreateSharedPreset(const std::string& materialName);

    // Resolve the effective preset for a slot: per-mesh override first, then the
    // shared material-name preset. Returns nullptr if neither is defined.
    const MeshMaterialPreset* ResolvePreset(const std::string& meshName, int slot,
                                            const std::string& materialName) const;
    
    nlohmann::json ToJson() const;
    static ModelImportSettings FromJson(const nlohmann::json& j);
    
    // Load/save from model .meta file
    static bool LoadFromMeta(const std::string& metaPath, ModelImportSettings& out);
    static bool SaveToMeta(const std::string& metaPath, const ModelImportSettings& settings);
};

