#include "MaterialManager.h"
#include "ShaderManager.h"
#include "TextureLoader.h"
#include "core/ecs/Scene.h"
#include "editor/rendering/DebugMaterial.h"

MaterialManager& MaterialManager::Instance() {
    static MaterialManager instance;
    return instance;
}
 
bgfx::VertexLayout MaterialManager::GetPBRVertexLayout() {
    static bgfx::VertexLayout layout;
    static bool initialized = false;

    if (!initialized) {
        layout.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .end();
        initialized = true;
    }
    return layout;
}

std::shared_ptr<PBRMaterial> MaterialManager::CreateDefaultPBRMaterial() {
    // NOTE: Previously this returned a static singleton, which caused issues when
    // per-instance texture overrides were applied - they would affect all meshes
    // using the "shared" material. Now we create fresh instances each time.
    // Proper sharing should be handled by MaterialCache::AcquireMaterialFromSource().
    auto program = ShaderManager::Instance().LoadProgram("vs_pbr", "fs_pbr");
    auto material = std::make_shared<PBRMaterial>("DefaultPBR", program);
    material->SetMetallic(PBRMaterial::kDefaultMetallic);
    material->SetRoughness(PBRMaterial::kDefaultRoughness);
    return material;
}

std::shared_ptr<SkinnedPBRMaterial> MaterialManager::CreateSkinnedPBRMaterial() {
    auto program = ShaderManager::Instance().LoadProgram("vs_pbr_skinned", "fs_pbr_skinned");
    auto mat = std::make_shared<SkinnedPBRMaterial>("SkinnedPBR", program);
    return mat;
}

std::shared_ptr<DebugMaterial> MaterialManager::CreateDefaultDebugMaterial() {
    // Create fresh instance each time to avoid shared state issues
    auto program = ShaderManager::Instance().LoadProgram("vs_debug", "fs_debug");
    return std::make_shared<DebugMaterial>("DefaultDebug", program);
}

// Scene-preset aware creators (temporarily only PBR; PSX added later)
std::shared_ptr<Material> MaterialManager::CreateSceneDefaultMaterial(Scene* scene) {
    if (scene && scene->GetDefaultShaderPreset() == Scene::ShaderPreset::PSX) {
        auto program = ShaderManager::Instance().LoadProgram("vs_psx", "fs_psx");
        auto mat = std::make_shared<PBRMaterial>("PSX", program);
        // Defaults similar to PBR for texture slots
        try { static_cast<PBRMaterial*>(mat.get())->SetAlbedoTextureFromPath("assets/debug/white.png"); } catch(...) {}
        // Initialize PSX-specific uniforms so shaders have defined values
        mat->SetUniform("u_psxParams", glm::vec4(0.0f, 0.0f, 0.0f, 0.0f));      // jitter=0, affine=0, lightInfluence=0, normalPerturb=0
        mat->SetUniform("u_psxWorld",  glm::vec4(0.0f, 0.25f, 0.0f, 0.0f));     // worldAmp=0m, tileSize=0.25m
        mat->SetUniform("u_toonParams", glm::vec4(3.0f, 1.0f, 0.0f, 0.0f));     // bands=3, softness=1
        return mat;
    }
    return CreateDefaultPBRMaterial();
}

std::shared_ptr<Material> MaterialManager::CreateSceneSkinnedDefaultMaterial(Scene* scene) {
    if (scene && scene->GetDefaultShaderPreset() == Scene::ShaderPreset::PSX) {
        auto program = ShaderManager::Instance().LoadProgram("vs_psx_skinned", "fs_psx");
        auto mat = std::make_shared<SkinnedPBRMaterial>("SkinnedPSX", program);
        try { static_cast<SkinnedPBRMaterial*>(mat.get())->SetAlbedoTextureFromPath("assets/debug/white.png"); } catch(...) {}
        // Initialize PSX-specific uniforms so shaders have defined values
        mat->SetUniform("u_psxParams", glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
        mat->SetUniform("u_psxWorld",  glm::vec4(0.0f, 0.25f, 0.0f, 0.0f));
        mat->SetUniform("u_toonParams", glm::vec4(3.0f, 1.0f, 0.0f, 0.0f));
        return mat;
    }
    return CreateSkinnedPBRMaterial();
}

std::shared_ptr<PBRMaterial> MaterialManager::CreatePSXMaterial() {
    auto program = ShaderManager::Instance().LoadProgram("vs_psx", "fs_psx");
    auto mat = std::make_shared<PBRMaterial>("PSX", program);
    try { mat->SetAlbedoTextureFromPath("assets/debug/white.png"); } catch(...) {}
    // Initialize PSX uniforms
    mat->SetUniform("u_psxParams", glm::vec4(0.0f, 0.0f, 0.0f, 0.0f));
    mat->SetUniform("u_psxWorld",  glm::vec4(0.0f, 0.25f, 0.0f, 0.0f));
    mat->SetUniform("u_toonParams", glm::vec4(3.0f, 1.0f, 0.0f, 0.0f));
    return mat;
}

std::shared_ptr<SkinnedPBRMaterial> MaterialManager::CreateSkinnedPSXMaterial() {
    auto program = ShaderManager::Instance().LoadProgram("vs_psx_skinned", "fs_psx");
    auto mat = std::make_shared<SkinnedPBRMaterial>("SkinnedPSX", program);
    try { mat->SetAlbedoTextureFromPath("assets/debug/white.png"); } catch(...) {}
    // Initialize PSX uniforms
    mat->SetUniform("u_psxParams", glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
    mat->SetUniform("u_psxWorld",  glm::vec4(0.0f, 0.25f, 0.0f, 0.0f));
    mat->SetUniform("u_toonParams", glm::vec4(3.0f, 1.0f, 0.0f, 0.0f));
    return mat;
}
