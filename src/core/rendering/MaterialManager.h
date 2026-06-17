#pragma once
#include <bgfx/bgfx.h>
#include <memory>
#include "PBRMaterial.h"
#include "SkinnedPBRMaterial.h"

// Forward declare - DebugMaterial is editor-only
class DebugMaterial;

class MaterialManager {
public:
    static MaterialManager& Instance();
    bgfx::VertexLayout GetPBRVertexLayout();
    
    std::shared_ptr<PBRMaterial> CreateDefaultPBRMaterial();
    std::shared_ptr<SkinnedPBRMaterial> CreateSkinnedPBRMaterial();
    // Explicit PSX creators for menus
    std::shared_ptr<class PBRMaterial> CreatePSXMaterial();
    std::shared_ptr<class SkinnedPBRMaterial> CreateSkinnedPSXMaterial();
    // Scene-preset aware creators (dispatch to PBR or PSX)
    std::shared_ptr<class Material> CreateSceneDefaultMaterial(class Scene* scene);
    std::shared_ptr<class Material> CreateSceneSkinnedDefaultMaterial(class Scene* scene);

    std::shared_ptr<DebugMaterial> CreateDefaultDebugMaterial();

    // Seeds every PSX-family uniform with a defined default value.
    // IMPORTANT: per-entity property-block overrides are only bound for uniforms
    // the material actually owns (see Material::ApplyPropertyBlockFast), so every
    // PSX uniform that should be overridable from the inspector or managed code
    // must be initialized here. Call this from any code path that constructs a
    // PSX material directly (scene loaders, prefab instantiation, etc.).
    // skinned: skinned PSX defaults to lightInfluence=1 (matches existing creators).
    static void InitializePSXUniformDefaults(class Material& material, bool skinned);
};
