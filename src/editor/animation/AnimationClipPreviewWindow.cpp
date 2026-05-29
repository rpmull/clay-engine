#include "editor/animation/AnimationClipPreviewWindow.h"

#include <imgui.h>
#include <bgfx/bgfx.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string_view>

#include "core/animation/AnimationAsset.h"
#include "editor/preview/AnimationPreviewPlayer.h"
#include "editor/preview/PreviewScene.h"
#include "editor/Project.h"
#include "core/ecs/SkinningSystem.h"
#include "core/rendering/TextureLoader.h"

AnimationClipPreviewWindow::AnimationClipPreviewWindow() = default;
AnimationClipPreviewWindow::~AnimationClipPreviewWindow() = default;

void AnimationClipPreviewWindow::SuggestModel(const std::string& assetPathHint) {
    m_CurrentAssetPath = assetPathHint;
    // Heuristic: if clip path mentions humanoid, prefer the default humanoid prefab.
    std::string lower = assetPathHint;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    if (lower.find("humanoid") != std::string::npos) {
        auto assets = Project::GetAssetDirectory();
        if (assets.empty()) assets = std::filesystem::path("assets");
        auto candidate = assets / "prefabs" / "default_humanoid.fbx";
        if (std::filesystem::exists(candidate)) {
            m_SelectedModelPath = NormalizePath(candidate.string());
            m_ModelListDirty = true;
        }
    }
}

void AnimationClipPreviewWindow::Draw(bool& open,
                                      const cm::animation::AnimationAsset* asset,
                                      float playbackTimeSeconds,
                                      bool loop,
                                      float speed)
{
    if (!open) return;
    ImGui::SetNextWindowSize(ImVec2(520, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Preview Humanoid", &open)) {
        ImGui::End();
        return;
    }

    if (!asset) {
        ImGui::TextUnformatted("No animation asset selected.");
        ImGui::TextDisabled("Load a clip in the Animation Controller panel first.");
        ImGui::End();
        return;
    }

    EnsureScene();
    EnsureModelList();
    if (m_SelectedModelPath.empty() && !m_ModelPaths.empty()) {
        m_SelectedModelPath = m_ModelPaths.front();
        EnsureModelLoaded(m_SelectedModelPath);
    }
    if (!m_SelectedModelPath.empty()) {
        EnsureModelLoaded(m_SelectedModelPath);
    }

    DrawModelPicker();
    ImGui::SameLine();
    if (ImGui::Button("Reset Camera") && m_PreviewScene) {
        m_PreviewScene->ResetCamera();
    }

    ImGui::Separator();

    ImVec2 viewportAvail = ImGui::GetContentRegionAvail();
    if (viewportAvail.x < 16.0f) viewportAvail.x = 16.0f;
    if (viewportAvail.y < 16.0f) viewportAvail.y = 16.0f;

    ImGui::BeginChild("AnimationPreviewViewport", viewportAvail, true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImVec2 innerSize = ImGui::GetContentRegionAvail();
    ResizeIfNeeded(innerSize);
    SyncPlayer(asset, playbackTimeSeconds, loop, speed);
    if (m_PreviewScene) {
        m_PreviewScene->Render(ImGui::GetIO().DeltaTime);
    }
    DrawViewport(innerSize);
    ImGui::EndChild();

    ImGui::End();
}

void AnimationClipPreviewWindow::EnsureScene() {
    if (!m_PreviewScene) {
        m_PreviewScene = std::make_unique<PreviewScene>();
        m_PreviewScene->Init(640, 360);
    }
    if (!m_Player) {
        m_Player = std::make_unique<AnimationPreviewPlayer>();
    }
    if (m_PreviewScene && m_Player) {
        m_Player->SetScene(m_PreviewScene->GetScene());
    }
}

void AnimationClipPreviewWindow::EnsureModelList() {
    if (!m_ModelListDirty) return;
    m_ModelPaths.clear();
    std::filesystem::path assetsRoot = Project::GetAssetDirectory();
    if (assetsRoot.empty()) assetsRoot = std::filesystem::path("assets");
    std::error_code ec;
    if (std::filesystem::exists(assetsRoot, ec)) {
        for (auto it = std::filesystem::recursive_directory_iterator(assetsRoot, ec);
             it != std::filesystem::recursive_directory_iterator(); ++it)
        {
            if (ec) break;
            if (!it->is_regular_file()) continue;
            std::string ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)std::tolower(c); });
            if (ext == ".fbx" || ext == ".glb" || ext == ".gltf") {
                m_ModelPaths.push_back(NormalizePath(it->path().string()));
            }
        }
    }
    std::sort(m_ModelPaths.begin(), m_ModelPaths.end());
    m_ModelPaths.erase(std::unique(m_ModelPaths.begin(), m_ModelPaths.end()), m_ModelPaths.end());
    m_ModelListDirty = false;

    if (!m_SelectedModelPath.empty()) {
        auto it = std::find(m_ModelPaths.begin(), m_ModelPaths.end(), m_SelectedModelPath);
        if (it != m_ModelPaths.end()) {
            m_SelectedModel = static_cast<int>(std::distance(m_ModelPaths.begin(), it));
        } else if (!m_ModelPaths.empty()) {
            m_SelectedModelPath = m_ModelPaths.front();
            m_SelectedModel = 0;
        }
    } else {
        LoadDefaultHumanoid();
    }
}

bool AnimationClipPreviewWindow::LoadDefaultHumanoid() {
    auto assets = Project::GetAssetDirectory();
    if (assets.empty()) assets = std::filesystem::path("assets");
    auto defaultPath = assets / "prefabs" / "default_humanoid.fbx";
    if (std::filesystem::exists(defaultPath)) {
        m_SelectedModelPath = NormalizePath(defaultPath.string());
        auto it = std::find(m_ModelPaths.begin(), m_ModelPaths.end(), m_SelectedModelPath);
        if (it != m_ModelPaths.end()) {
            m_SelectedModel = static_cast<int>(std::distance(m_ModelPaths.begin(), it));
        } else {
            m_ModelPaths.insert(m_ModelPaths.begin(), m_SelectedModelPath);
            m_SelectedModel = 0;
        }
        EnsureModelLoaded(m_SelectedModelPath);
        return true;
    }
    return false;
}

void AnimationClipPreviewWindow::EnsureModelLoaded(const std::string& path) {
    if (path.empty() || !m_PreviewScene) return;
    if (path == m_LastLoadedModelPath) return;
    auto resolved = ResolvePathForLoad(path);
    if (resolved.empty()) return;
    m_PreviewScene->SetModelPath(resolved);
    glm::vec3 preferredForward(0.0f, 0.0f, 1.0f);
    if (auto* skeleton = m_PreviewScene->GetSkeleton()) {
        if (!skeleton->BoneNameToIndex.empty()) {
            auto it = skeleton->BoneNameToIndex.find("Hips");
            if (it != skeleton->BoneNameToIndex.end()) {
                preferredForward = glm::vec3(0.0f, 0.0f, 1.0f);
            }
        }
    }
    m_PreviewScene->EnsureInView(0.2f, true, preferredForward);
    RefreshSkeletonBinding();
    m_LastLoadedModelPath = path;
}

void AnimationClipPreviewWindow::RefreshSkeletonBinding() {
    if (!m_PreviewScene || !m_Player) return;
    if (auto* skeleton = m_PreviewScene->GetSkeleton()) {
        m_Player->SetSkeleton(skeleton);
        if (skeleton->Avatar) {
            m_Player->SetAvatar(skeleton->Avatar.get(), skeleton);
            m_Player->SetRetargetMap(skeleton->Avatar.get());
        } else {
            m_Player->SetAvatar(nullptr, skeleton);
        }
    }
    if (auto* scene = m_PreviewScene->GetScene()) {
        m_Player->SetScene(scene);
    }
}

void AnimationClipPreviewWindow::SyncPlayer(const cm::animation::AnimationAsset* asset, float time, bool loop, float speed) {
    if (!asset || !m_Player) return;
    if (asset != m_CurrentAsset) {
        m_CurrentAsset = asset;
        m_Player->SetAsset(asset);
        m_LastAppliedTime = -1.0f;
    }
    m_Player->SetLoop(loop);
    m_Player->SetSpeed(speed);
    m_Player->SetTime(time);
    m_LastAppliedTime = time;
    // Sample pose directly into the preview scene skeleton.
    m_Player->Update(0.0f);
    if (m_PreviewScene) {
        if (auto* scene = m_PreviewScene->GetScene()) {
            scene->UpdateTransforms();
            SkinningSystem::Update(*scene);
        }
    }
}

void AnimationClipPreviewWindow::ResizeIfNeeded(const ImVec2& viewportSize) {
    if (!m_PreviewScene) return;
    int w = std::max(1, (int)viewportSize.x);
    int h = std::max(1, (int)viewportSize.y);
    if (w != m_LastViewportWidth || h != m_LastViewportHeight) {
        m_LastViewportWidth = w;
        m_LastViewportHeight = h;
        m_PreviewScene->Resize(w, h);
    }
}

void AnimationClipPreviewWindow::DrawViewport(const ImVec2& viewportSize) {
    if (!m_PreviewScene) {
        ImGui::TextUnformatted("Preview scene not initialized.");
        return;
    }
    bgfx::TextureHandle tex = m_PreviewScene->GetColorTexture();
    ImVec2 uv0(0.0f, 0.0f);
    ImVec2 uv1(1.0f, 1.0f);
    if (bgfx::isValid(tex)) {
        ImGui::Image(TextureLoader::ToImGuiTextureID(tex), viewportSize, uv0, uv1);
        if (ImGui::IsItemHovered()) {
            ImVec2 delta = ImGui::GetIO().MouseDelta;
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_PreviewScene->Orbit(delta.x, delta.y);
            if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) m_PreviewScene->Pan(delta.x, delta.y);
            float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) m_PreviewScene->Dolly(wheel);
        }
    } else {
        ImGui::TextDisabled("(no render target)");
        ImGui::TextUnformatted("Load a skinned mesh to enable preview.");
    }
}

void AnimationClipPreviewWindow::DrawModelPicker() {
    if (m_ModelPaths.empty()) {
        ImGui::TextDisabled("No skinned models detected in assets/");
        if (ImGui::Button("Rescan Models")) m_ModelListDirty = true;
        return;
    }

    const char* currentLabel = "<Choose Model>";
    std::string pretty;
    if (m_SelectedModel >= 0 && m_SelectedModel < (int)m_ModelPaths.size()) {
        pretty = std::filesystem::path(m_ModelPaths[(size_t)m_SelectedModel]).filename().string();
        currentLabel = pretty.c_str();
    }

    if (ImGui::BeginCombo("Preview Model", currentLabel)) {
        for (int i = 0; i < (int)m_ModelPaths.size(); ++i) {
            std::string label = std::filesystem::path(m_ModelPaths[(size_t)i]).filename().string();
            bool selected = (i == m_SelectedModel);
            if (ImGui::Selectable(label.c_str(), selected)) {
                m_SelectedModel = i;
                m_SelectedModelPath = m_ModelPaths[(size_t)i];
                EnsureModelLoaded(m_SelectedModelPath);
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Rescan Models")) {
        m_ModelListDirty = true;
    }
}

std::string AnimationClipPreviewWindow::NormalizePath(const std::string& absolutePath) const {
    if (absolutePath.empty()) return absolutePath;
    std::filesystem::path p(absolutePath);
    std::error_code ec;
    auto projectRoot = Project::GetProjectDirectory();
    if (!projectRoot.empty()) {
        auto rel = std::filesystem::relative(p, projectRoot, ec);
        if (!ec) p = rel;
    }
    auto normalized = p.generic_string();
    return normalized;
}

std::string AnimationClipPreviewWindow::ResolvePathForLoad(const std::string& path) const {
    if (path.empty()) return path;
    std::filesystem::path p(path);
    if (!p.is_absolute()) {
        auto base = Project::GetProjectDirectory();
        if (!base.empty()) p = std::filesystem::path(base) / p;
    }
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(p, ec);
    return (ec ? p : canonical).string();
}


