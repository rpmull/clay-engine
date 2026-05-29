#include "IconGeneratorPanel.h"
#include <imgui.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iostream>

#include "core/ecs/Scene.h"
#include "core/rendering/Camera.h"
#include "core/rendering/Renderer.h"
#include "core/ecs/SkinningSystem.h"
#include "editor/Project.h"
#include "core/resources/ResourceManifest.h"
#include "core/assets/AssetMetadata.h"
#include "editor/pipeline/AssetLibrary.h"
#include "ui/utility/ModelSnapshotUtils.h"

#include <stb_image_write.h>

namespace fs = std::filesystem;

// Helper: Save PNG
static bool SavePNG(const std::string& filepath, const uint8_t* pixels, int width, int height, int channels = 4) {
    return stbi_write_png(filepath.c_str(), width, height, channels, pixels, width * channels) != 0;
}

IconGeneratorPanel::IconGeneratorPanel(UILayer* uiLayer)
    : m_UILayer(uiLayer)
{
}

IconGeneratorPanel::~IconGeneratorPanel() {
    CancelGeneration();
}

bool IconGeneratorPanel::IsModelExtension(const std::string& ext) const {
    std::string lower = ext;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower == ".fbx" || lower == ".gltf" || lower == ".glb" || lower == ".obj";
}

void IconGeneratorPanel::EnsureResourceIconsFolder() {
    fs::path iconsDir = Project::GetProjectDirectory() / "resources" / "icons";
    std::error_code ec;
    fs::create_directories(iconsDir, ec);
}

void IconGeneratorPanel::ScanModels() {
    m_Models.clear();
    m_ModelsScanned = true;
    
    fs::path modelsDir = Project::GetProjectDirectory() / "assets" / "models";
    fs::path iconsDir = Project::GetProjectDirectory() / "resources" / "icons";
    
    if (!fs::exists(modelsDir)) {
        std::cout << "[IconGenerator] No assets/models folder found" << std::endl;
        return;
    }
    
    EnsureResourceIconsFolder();
    
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(modelsDir, ec)) {
        if (entry.is_regular_file()) {
            std::string ext = entry.path().extension().string();
            if (IsModelExtension(ext)) {
                ModelEntry model;
                model.path = entry.path().string();
                model.name = entry.path().stem().string();
                model.iconPath = (iconsDir / (model.name + ".png")).string();
                model.hasIcon = fs::exists(model.iconPath);
                model.selected = !model.hasIcon;
                m_Models.push_back(model);
            }
        }
    }
    
    std::cout << "[IconGenerator] Found " << m_Models.size() << " models in assets/models" << std::endl;
}

void IconGeneratorPanel::OnImGuiRender() {
    if (!m_IsOpen) return;
    
    ImGui::SetNextWindowSize(ImVec2(500, 450), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Icon Generator", &m_IsOpen)) {
        ImGui::End();
        return;
    }
    
    // Header
    ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "Batch Model Icon Generator");
    ImGui::TextDisabled("Generates item icons from 3D models in assets/models");
    ImGui::TextDisabled("Output: resources/icons/");
    ImGui::Separator();
    
    // Scan button
    if (!m_BatchActive) {
        if (ImGui::Button("Scan Models")) {
            ScanModels();
        }
        ImGui::SameLine();
        ImGui::Checkbox("Show only missing", &m_ShowOnlyMissing);
    }
    
    // Settings
    if (!m_BatchActive) {
        if (ImGui::CollapsingHeader("Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::PushItemWidth(100);
            ImGui::InputInt2("Resolution", m_Resolution);
            m_Resolution[0] = std::clamp(m_Resolution[0], 32, 512);
            m_Resolution[1] = std::clamp(m_Resolution[1], 32, 512);
            ImGui::PopItemWidth();
        }
    }
    
    // Model list
    if (m_ModelsScanned && !m_Models.empty()) {
        ImGui::Separator();
        
        int selected = 0;
        int total = 0;
        for (const auto& m : m_Models) {
            if (!m_ShowOnlyMissing || !m.hasIcon) {
                total++;
                if (m.selected) selected++;
            }
        }
        
        ImGui::Text("Models: %d total, %d selected", total, selected);
        
        if (!m_BatchActive) {
            ImGui::SameLine();
            if (ImGui::Button("Select All")) {
                for (auto& m : m_Models) {
                    if (!m_ShowOnlyMissing || !m.hasIcon) m.selected = true;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Select None")) {
                for (auto& m : m_Models) m.selected = false;
            }
        }
        
        // Scrolling list
        ImGui::BeginChild("ModelList", ImVec2(0, 200), true);
        for (size_t i = 0; i < m_Models.size(); ++i) {
            auto& m = m_Models[i];
            if (m_ShowOnlyMissing && m.hasIcon) continue;
            
            ImGui::PushID(static_cast<int>(i));
            
            if (!m_BatchActive) {
                ImGui::Checkbox("##sel", &m.selected);
                ImGui::SameLine();
            }
            
            if (m.hasIcon) {
                ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "[OK]");
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "[--]");
            }
            ImGui::SameLine();
            ImGui::Text("%s", m.name.c_str());
            
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    
    // Batch generation controls
    ImGui::Separator();
    
    if (m_BatchActive) {
        // Progress
        float progress = m_TotalToProcess > 0 
            ? static_cast<float>(m_ProcessedCount) / static_cast<float>(m_TotalToProcess) 
            : 0.0f;
        
        ImGui::Text("Processing: %s", m_CurrentModelName.c_str());
        ImGui::ProgressBar(progress, ImVec2(-1, 0), 
            (std::to_string(m_ProcessedCount) + " / " + std::to_string(m_TotalToProcess)).c_str());
        
        if (ImGui::Button("Cancel")) {
            CancelGeneration();
        }
        
        // Update snapshot
        UpdateSnapshot();
    } else {
        int selectedCount = 0;
        for (const auto& m : m_Models) {
            if (m.selected && (!m_ShowOnlyMissing || !m.hasIcon)) selectedCount++;
        }
        
        bool canGenerate = selectedCount > 0 && m_ModelsScanned;
        if (!canGenerate) ImGui::BeginDisabled();
        
        if (ImGui::Button("Generate Icons")) {
            StartBatchGeneration();
        }
        
        if (!canGenerate) ImGui::EndDisabled();
        
        // Results
        if (!m_GeneratedIcons.empty() || !m_FailedModels.empty()) {
            ImGui::Separator();
            if (!m_GeneratedIcons.empty()) {
                ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "Generated %zu icons", m_GeneratedIcons.size());
            }
            if (!m_FailedModels.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "Failed: %zu models", m_FailedModels.size());
                for (const auto& f : m_FailedModels) {
                    ImGui::BulletText("%s", f.c_str());
                }
            }
        }
    }
    
    ImGui::End();
}

void IconGeneratorPanel::StartBatchGeneration() {
    m_PendingQueue = std::queue<size_t>();
    m_GeneratedIcons.clear();
    m_FailedModels.clear();
    m_ProcessedCount = 0;
    
    for (size_t i = 0; i < m_Models.size(); ++i) {
        if (m_Models[i].selected && (!m_ShowOnlyMissing || !m_Models[i].hasIcon)) {
            m_PendingQueue.push(i);
        }
    }
    
    m_TotalToProcess = m_PendingQueue.size();
    if (m_TotalToProcess == 0) return;
    
    m_BatchActive = true;
    EnsureResourceIconsFolder();
    ProcessNextModel();
}

void IconGeneratorPanel::ProcessNextModel() {
    if (m_PendingQueue.empty()) {
        m_BatchActive = false;
        m_CurrentModelName.clear();
        
        // Rescan to update icon status
        ScanModels();
        
        // Refresh resource manifest
        auto& manifest = ResourceManifest::Get();
        if (!manifest.IsInitialized()) {
            manifest.Initialize(Project::GetProjectDirectory());
        }
        manifest.Scan();
        
        std::cout << "[IconGenerator] Batch complete. Generated " << m_GeneratedIcons.size() 
                  << " icons, " << m_FailedModels.size() << " failed" << std::endl;
        return;
    }
    
    size_t idx = m_PendingQueue.front();
    m_PendingQueue.pop();
    
    const auto& model = m_Models[idx];
    m_CurrentModelName = model.name;
    
    if (!StartSnapshot(model.path, model.iconPath)) {
        m_FailedModels.push_back(model.name);
        m_ProcessedCount++;
        ProcessNextModel();
    }
}

bool IconGeneratorPanel::StartSnapshot(const std::string& modelPath, const std::string& outputPath) {
    if (m_Snapshot.active) {
        std::cerr << "[IconGenerator] Snapshot already in progress" << std::endl;
        return false;
    }
    
    m_Snapshot.modelPath = modelPath;
    m_Snapshot.outputPath = outputPath;
    m_Snapshot.width = static_cast<uint32_t>(m_Resolution[0]);
    m_Snapshot.height = static_cast<uint32_t>(m_Resolution[1]);
    
    // Create isolated scene
    m_Snapshot.scene = std::make_unique<Scene>();
    m_Snapshot.camera = std::make_unique<Camera>(60.0f, 1.0f, 0.1f, 1000.0f);
    
    // Configure environment
    Environment& env = m_Snapshot.scene->GetEnvironment();
    env.ProceduralSky = false;
    env.UseSkybox = false;
    env.EnableFog = false;
    env.OutlineEnabled = false;
    env.AmbientColor = glm::vec3(0.6f);
    env.AmbientIntensity = 1.2f;
    
    // Add lights
    Entity keyLight = m_Snapshot.scene->CreateLight("Key", LightType::Directional, glm::vec3(1.0f), 1.25f);
    if (auto* lightData = m_Snapshot.scene->GetEntityData(keyLight.GetID())) {
        lightData->Transform.Rotation = glm::vec3(-35.0f, 45.0f, 0.0f);
        lightData->Transform.UseQuatRotation = false;
        lightData->Transform.TransformDirty = true;
    }
    Entity fillLight = m_Snapshot.scene->CreateLight("Fill", LightType::Directional, glm::vec3(0.9f), 0.65f);
    if (auto* lightData = m_Snapshot.scene->GetEntityData(fillLight.GetID())) {
        lightData->Transform.Rotation = glm::vec3(-15.0f, -135.0f, 0.0f);
        lightData->Transform.UseQuatRotation = false;
        lightData->Transform.TransformDirty = true;
    }
    
    // Instantiate model
    m_Snapshot.modelRoot = m_Snapshot.scene->InstantiateModel(modelPath, glm::vec3(0.0f));
    if (m_Snapshot.modelRoot == INVALID_ENTITY_ID) {
        std::cerr << "[IconGenerator] Failed to instantiate: " << modelPath << std::endl;
        m_Snapshot.scene.reset();
        m_Snapshot.camera.reset();
        return false;
    }
    
    // Apply transforms
    if (auto* data = m_Snapshot.scene->GetEntityData(m_Snapshot.modelRoot)) {
        data->Transform.Rotation = m_DefaultRotation;
        data->Transform.UseQuatRotation = false;
        data->Transform.Scale = glm::max(m_DefaultScale, glm::vec3(0.001f));
        data->Transform.TransformDirty = true;
    }
    
    m_Snapshot.scene->MarkTransformDirty(m_Snapshot.modelRoot);
    m_Snapshot.scene->UpdateTransforms();
    m_Snapshot.scene->ProcessBoneAttachments();
    SkinningSystem::Update(*m_Snapshot.scene);
    
    // Frame the model
    glm::vec3 boundsMin, boundsMax;
    snapshot_utils::ComputeModelWorldBounds(*m_Snapshot.scene, m_Snapshot.modelRoot, boundsMin, boundsMax);
    glm::vec3 center = 0.5f * (boundsMin + boundsMax);
    glm::vec3 extents = 0.5f * (boundsMax - boundsMin);
    extents = glm::max(extents, glm::vec3(0.001f));
    
    snapshot_utils::SnapshotViewFit viewFit = snapshot_utils::ComputeSnapshotViewFit(extents);
    
    float aspect = m_Snapshot.height > 0 
        ? static_cast<float>(m_Snapshot.width) / static_cast<float>(m_Snapshot.height) 
        : 1.0f;
    const float fovDegrees = 60.0f;
    const float halfFov = glm::radians(fovDegrees * 0.5f);
    const float distHeight = viewFit.halfHeight / std::tan(halfFov);
    const float distWidth = viewFit.halfWidth / (std::tan(halfFov) * aspect);
    float distance = std::max(distHeight, distWidth) * 1.15f;
    float radius = std::max(extents.x, std::max(extents.y, extents.z));
    float farClip = std::max(100.0f, distance + radius * 4.0f);
    
    m_Snapshot.camera->SetPerspective(fovDegrees, aspect, 0.1f, farClip);
    m_Snapshot.camera->SetViewportSize(static_cast<float>(m_Snapshot.width), static_cast<float>(m_Snapshot.height));
    m_Snapshot.camera->SetPosition(center + viewFit.viewDir * distance);
    m_Snapshot.camera->LookAt(center, viewFit.upDir);

    m_Snapshot.clearColor = 0xFF00FFFF;
    m_Snapshot.clearKey[0] = 0xFF;
    m_Snapshot.clearKey[1] = 0x00;
    m_Snapshot.clearKey[2] = 0xFF;
    
    // Render
    Scene* prevScene = Scene::CurrentScene;
    Scene::CurrentScene = m_Snapshot.scene.get();
    bgfx::TextureHandle tex = Renderer::Get().RenderSceneToTexture(
        m_Snapshot.scene.get(),
        m_Snapshot.width,
        m_Snapshot.height,
        m_Snapshot.camera.get(),
        m_Snapshot.viewIdBase,
        false,
        m_Snapshot.clearColor);
    Scene::CurrentScene = prevScene;
    
    if (!bgfx::isValid(tex)) {
        std::cerr << "[IconGenerator] Render failed: " << modelPath << std::endl;
        Renderer::Get().ReleaseOffscreenTarget(m_Snapshot.viewIdBase);
        m_Snapshot.scene.reset();
        m_Snapshot.camera.reset();
        return false;
    }
    
    // Create readback texture
    const bgfx::Caps* caps = bgfx::getCaps();
    if (!caps || (caps->supported & BGFX_CAPS_TEXTURE_BLIT) == 0 || 
        (caps->supported & BGFX_CAPS_TEXTURE_READ_BACK) == 0) {
        std::cerr << "[IconGenerator] Readback not supported" << std::endl;
        Renderer::Get().ReleaseOffscreenTarget(m_Snapshot.viewIdBase);
        m_Snapshot.scene.reset();
        m_Snapshot.camera.reset();
        return false;
    }
    
    if (bgfx::isValid(m_Snapshot.readbackTexture)) {
        bgfx::destroy(m_Snapshot.readbackTexture);
        m_Snapshot.readbackTexture = BGFX_INVALID_HANDLE;
    }
    
    const uint64_t readbackFlags = BGFX_TEXTURE_READ_BACK | BGFX_TEXTURE_BLIT_DST;
    m_Snapshot.readbackTexture = bgfx::createTexture2D(
        (uint16_t)m_Snapshot.width,
        (uint16_t)m_Snapshot.height,
        false, 1,
        bgfx::TextureFormat::RGBA8,
        readbackFlags);
    
    if (!bgfx::isValid(m_Snapshot.readbackTexture)) {
        std::cerr << "[IconGenerator] Failed to create readback texture" << std::endl;
        Renderer::Get().ReleaseOffscreenTarget(m_Snapshot.viewIdBase);
        m_Snapshot.scene.reset();
        m_Snapshot.camera.reset();
        return false;
    }
    
    // Blit and read
    const uint16_t blitViewId = (uint16_t)(m_Snapshot.viewIdBase + 2);
    bgfx::setViewRect(blitViewId, 0, 0, (uint16_t)m_Snapshot.width, (uint16_t)m_Snapshot.height);
    bgfx::touch(blitViewId);
    bgfx::blit(blitViewId, m_Snapshot.readbackTexture, 0, 0, tex, 0, 0, 
               (uint16_t)m_Snapshot.width, (uint16_t)m_Snapshot.height);
    
    m_Snapshot.readbackBuffer.assign(
        static_cast<size_t>(m_Snapshot.width) * static_cast<size_t>(m_Snapshot.height) * 4u, 0u);
    m_Snapshot.pendingFrame = bgfx::readTexture(m_Snapshot.readbackTexture, m_Snapshot.readbackBuffer.data());
    
    if (m_Snapshot.pendingFrame == std::numeric_limits<uint32_t>::max()) {
        std::cerr << "[IconGenerator] Readback request failed" << std::endl;
        Renderer::Get().ReleaseOffscreenTarget(m_Snapshot.viewIdBase);
        if (bgfx::isValid(m_Snapshot.readbackTexture)) {
            bgfx::destroy(m_Snapshot.readbackTexture);
            m_Snapshot.readbackTexture = BGFX_INVALID_HANDLE;
        }
        m_Snapshot.scene.reset();
        m_Snapshot.camera.reset();
        m_Snapshot.readbackBuffer.clear();
        return false;
    }
    
    m_Snapshot.pendingStartFrame = Renderer::Get().GetLastSubmittedFrame();
    m_Snapshot.pendingReadback = true;
    m_Snapshot.active = true;
    return true;
}

void IconGeneratorPanel::UpdateSnapshot() {
    if (!m_Snapshot.pendingReadback) return;
    
    const uint32_t currentFrame = Renderer::Get().GetLastSubmittedFrame();
    if (m_Snapshot.pendingFrame > currentFrame) {
        const uint32_t framesWaited = currentFrame >= m_Snapshot.pendingStartFrame
            ? (currentFrame - m_Snapshot.pendingStartFrame) : 0u;
        if (framesWaited > 120) {
            std::cerr << "[IconGenerator] Readback timeout: " << m_CurrentModelName << std::endl;
            m_FailedModels.push_back(m_CurrentModelName);
            CancelGeneration();
            m_ProcessedCount++;
            ProcessNextModel();
        }
        return;
    }
    
    FinalizeSnapshot();
}

void IconGeneratorPanel::FinalizeSnapshot() {
    m_Snapshot.pendingReadback = false;
    
    // Flip if needed
    if (bgfx::getCaps() && bgfx::getCaps()->originBottomLeft) {
        snapshot_utils::FlipImageVertical(m_Snapshot.readbackBuffer.data(),
                     static_cast<int>(m_Snapshot.width),
                     static_cast<int>(m_Snapshot.height), 4);
    }
    
    // Alpha key
    bool hasContent = snapshot_utils::ApplySnapshotAlphaKey(
        m_Snapshot.readbackBuffer.data(),
        static_cast<int>(m_Snapshot.width),
        static_cast<int>(m_Snapshot.height),
        m_Snapshot.clearKey[0],
        m_Snapshot.clearKey[1],
        m_Snapshot.clearKey[2]);
    
    if (!hasContent) {
        const size_t pixelCount = static_cast<size_t>(m_Snapshot.width) * m_Snapshot.height;
        for (size_t i = 0; i < pixelCount; ++i) {
            m_Snapshot.readbackBuffer[i * 4 + 3] = 255;
        }
    }
    
    // Save PNG
    if (SavePNG(m_Snapshot.outputPath,
                m_Snapshot.readbackBuffer.data(),
                static_cast<int>(m_Snapshot.width),
                static_cast<int>(m_Snapshot.height), 4)) {
        std::cout << "[IconGenerator] Saved: " << m_Snapshot.outputPath << std::endl;
        m_GeneratedIcons.push_back(m_Snapshot.outputPath);
        
        // Create meta file and register asset
        try {
            fs::path p(m_Snapshot.outputPath);
            fs::path metaPath = p; metaPath += ".meta";
            AssetMetadata meta;
            bool hasMeta = false;
            if (fs::exists(metaPath)) {
                std::ifstream in(metaPath.string());
                if (in) {
                    nlohmann::json j;
                    in >> j;
                    meta = j.get<AssetMetadata>();
                    hasMeta = true;
                }
            }
            if (!hasMeta) {
                meta.guid = ClaymoreGUID::Generate();
                meta.type = "texture";
                nlohmann::json j = meta;
                std::ofstream out(metaPath.string());
                out << j.dump(4);
            }
            
            std::string name = p.filename().string();
            std::error_code ec;
            fs::path rel = fs::relative(p, Project::GetProjectDirectory(), ec);
            std::string vpath = (ec ? p.string() : rel.string());
            std::replace(vpath.begin(), vpath.end(), '\\', '/');
            
            AssetType t = AssetType::Texture;
            AssetReference aref(meta.guid, 0, (int)t);
            AssetLibrary::Instance().RegisterAsset(aref, t, vpath, name);
            AssetLibrary::Instance().RegisterPathAlias(meta.guid, m_Snapshot.outputPath);
        } catch (...) {
            std::cerr << "[IconGenerator] Failed to register asset" << std::endl;
        }
    } else {
        std::cerr << "[IconGenerator] Failed to save: " << m_Snapshot.outputPath << std::endl;
        m_FailedModels.push_back(m_CurrentModelName);
    }
    
    // Cleanup
    Renderer::Get().ReleaseOffscreenTarget(m_Snapshot.viewIdBase);
    if (bgfx::isValid(m_Snapshot.readbackTexture)) {
        bgfx::destroy(m_Snapshot.readbackTexture);
        m_Snapshot.readbackTexture = BGFX_INVALID_HANDLE;
    }
    m_Snapshot.scene.reset();
    m_Snapshot.camera.reset();
    m_Snapshot.readbackBuffer.clear();
    m_Snapshot.modelRoot = INVALID_ENTITY_ID;
    m_Snapshot.active = false;
    
    m_ProcessedCount++;
    ProcessNextModel();
}

void IconGeneratorPanel::CancelGeneration() {
    m_PendingQueue = std::queue<size_t>();
    m_BatchActive = false;
    m_CurrentModelName.clear();
    
    // Cleanup any active snapshot
    if (m_Snapshot.active || m_Snapshot.pendingReadback) {
        Renderer::Get().ReleaseOffscreenTarget(m_Snapshot.viewIdBase);
        if (bgfx::isValid(m_Snapshot.readbackTexture)) {
            bgfx::destroy(m_Snapshot.readbackTexture);
            m_Snapshot.readbackTexture = BGFX_INVALID_HANDLE;
        }
        m_Snapshot.scene.reset();
        m_Snapshot.camera.reset();
        m_Snapshot.readbackBuffer.clear();
        m_Snapshot.modelRoot = INVALID_ENTITY_ID;
        m_Snapshot.active = false;
        m_Snapshot.pendingReadback = false;
    }
}
