#include "ToolbarPanel.h"
#include "imgui.h"
#include "ui/UILayer.h"
#include "editor/pipeline/AssetPipeline.h"
#include "editor/pipeline/BinaryAssetCache.h"
#include "ui/Logger.h"
#include "core/rendering/TextureLoader.h"
#include "core/rendering/Renderer.h"
#include "core/assets/IAssetResolver.h"
#include "core/vfs/FileSystem.h"
#include "core/prefab/RuntimePrefabInstantiator.h"
#include "editor/Project.h"
#include <navigation/NavDebugDraw.h>
#include "managed/interop/DotNetHost.h"
#include <algorithm>
#include <vector>

namespace {
struct ResolvedResolutionPreset {
    std::string Label;
    uint32_t Width = 0;
    uint32_t Height = 0;
    bool Native = false;
};

std::vector<ResolvedResolutionPreset> BuildResolutionPresetList()
{
    std::vector<ResolvedResolutionPreset> presets;
    presets.push_back({ "Native", 0, 0, true });
    const ViewportResolutionPreset builtins[] = {
        { "640 x 360", 640, 360 },
        { "854 x 480", 854, 480 },
        { "1280 x 720", 1280, 720 },
        { "1600 x 900", 1600, 900 },
        { "1920 x 1080", 1920, 1080 },
        { "2560 x 1440", 2560, 1440 },
        { "3840 x 2160", 3840, 2160 },
    };
    for (const auto& builtin : builtins) {
        presets.push_back({ builtin.label, builtin.width, builtin.height, false });
    }
    for (const auto& preset : Project::GetViewportResolutionPresets()) {
        presets.push_back({ preset.label, preset.width, preset.height, false });
    }
    return presets;
}

bool MatchesPreset(const Environment& env, const ResolvedResolutionPreset& preset)
{
    if (preset.Native) {
        return !env.HasFixedRenderResolution();
    }
    return env.RenderResolutionWidth == preset.Width && env.RenderResolutionHeight == preset.Height;
}
}
// Renders the main toolbar panel
void ToolbarPanel::OnImGuiRender(ImGuiID dockspace_id) {
    ImGui::Begin("Toolbar", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoResize);

    EnsureIconsLoaded();

    auto drawIconButton = [&](const char* id, ImTextureID icon, bool active, const char* tooltip) {
        ImVec2 iconSize(20.0f, 20.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3.0f, 3.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImVec4 base      = ImGui::GetStyleColorVec4(ImGuiCol_Button);
        ImVec4 hover     = ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered);
        ImVec4 pressed   = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
        if (!active) {
            base.w = 0.0f;
            hover.w = 0.18f;
            pressed.w = 0.3f;
        }
        ImGui::PushStyleColor(ImGuiCol_Button, base);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, pressed);
        bool pressedBtn = ImGui::ImageButton(id, icon, iconSize);
        if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s", tooltip);
        }
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(2);
        return pressedBtn;
    };

    // Play button (always enabled)
    if (!m_PlayMode) {
        if (drawIconButton("##play", m_PlayIcon, false, "Enter Play Mode (Ctrl+P)")) {
            TogglePlayMode();
        }
    } else {
        // While in play mode, show Pause and Stop. Pause toggles, Stop exits play mode.
        if (drawIconButton("##pause", m_PauseIcon, m_Paused, "Pause/Resume")) {
            TogglePause();
        }
        ImGui::SameLine();
        if (drawIconButton("##stop", m_StopIcon, false, "Exit Play Mode")) {
            TogglePlayMode();
        }
    }

    ImGui::SameLine();

    // Play mode type selector (highly visible)
    ImGui::BeginDisabled(m_PlayMode);
    ImGui::TextUnformatted("Play Mode:");
    ImGui::SameLine();
    const char* currentModeLabel = m_UseBinaryPlayMode ? "Binary Preview" : "Live";
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::BeginCombo("##PlayModeSelect", currentModeLabel)) {
        bool liveSelected = !m_UseBinaryPlayMode;
        if (ImGui::Selectable("Live", liveSelected)) {
            m_UseBinaryPlayMode = false;
        }
        bool binarySelected = m_UseBinaryPlayMode;
        if (ImGui::Selectable("Binary Preview", binarySelected)) {
            m_UseBinaryPlayMode = true;
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::TextUnformatted("Window:");
    ImGui::SameLine();
    const char* currentWindowLabel = "Editor";
    switch (m_PlayWindowMode) {
        case PlayWindowMode::Windowed: currentWindowLabel = "Windowed"; break;
        case PlayWindowMode::Fullscreen: currentWindowLabel = "Fullscreen"; break;
        case PlayWindowMode::Editor:
        default: currentWindowLabel = "Editor"; break;
    }
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::BeginCombo("##PlayWindowMode", currentWindowLabel)) {
        if (ImGui::Selectable("Editor", m_PlayWindowMode == PlayWindowMode::Editor)) {
            SetPlayWindowMode(PlayWindowMode::Editor);
        }
        if (ImGui::Selectable("Windowed", m_PlayWindowMode == PlayWindowMode::Windowed)) {
            SetPlayWindowMode(PlayWindowMode::Windowed);
        }
        if (ImGui::Selectable("Fullscreen", m_PlayWindowMode == PlayWindowMode::Fullscreen)) {
            SetPlayWindowMode(PlayWindowMode::Fullscreen);
        }
        ImGui::EndCombo();
    }

    if (!m_UseBinaryPlayMode && m_PlayWindowMode == PlayWindowMode::Editor) {
        m_UseTempPak = false;
    }
    if (m_PlayWindowMode != PlayWindowMode::Editor) {
        m_UseBinaryPlayMode = true;
        m_UseTempPak = true;
        ImGui::SameLine();
        ImGui::TextDisabled("External play uses a temporary runtime build");
    } else if (m_UseBinaryPlayMode) {
        ImGui::SameLine();
        ImGui::Checkbox("Temp PAK", &m_UseTempPak);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Build a temporary runtime PAK in .bin for this play session.");
        }
    }
    ImGui::EndDisabled();

    if (m_UILayer) {
        ImGui::SameLine();
        ImGui::TextUnformatted("Render:");
        ImGui::SameLine();
        auto presets = BuildResolutionPresetList();
        Scene& scene = m_UILayer->GetScene();
        Environment& env = scene.GetEnvironment();
        size_t selectedIndex = 0;
        for (size_t i = 0; i < presets.size(); ++i) {
            if (MatchesPreset(env, presets[i])) {
                selectedIndex = i;
                break;
            }
        }
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::BeginCombo("##ViewportRenderResolution", presets[selectedIndex].Label.c_str())) {
            for (size_t i = 0; i < presets.size(); ++i) {
                const bool selected = i == selectedIndex;
                if (ImGui::Selectable(presets[i].Label.c_str(), selected)) {
                    if (presets[i].Native) {
                        env.RenderResolutionWidth = 0;
                        env.RenderResolutionHeight = 0;
                    } else {
                        env.RenderResolutionWidth = static_cast<uint16_t>(presets[i].Width);
                        env.RenderResolutionHeight = static_cast<uint16_t>(presets[i].Height);
                    }
                    scene.MarkDirty();
                    if (scene.m_RuntimeScene) {
                        scene.m_RuntimeScene->GetEnvironment() = env;
                    }
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
    }

    // Gizmo toggle
    ImGui::Checkbox("Show Gizmos", &m_ShowGizmos);

    ImGui::SameLine();

    // Gizmo operation mode
    if (drawIconButton("##move", m_MoveIcon, m_CurrentOperation == GizmoOperation::Translate, "Translate (W)")) SetOperation(GizmoOperation::Translate);
    ImGui::SameLine();
    if (drawIconButton("##rotate", m_RotateIcon, m_CurrentOperation == GizmoOperation::Rotate, "Rotate (E)")) SetOperation(GizmoOperation::Rotate);
    ImGui::SameLine();
    if (drawIconButton("##scale", m_ScaleIcon, m_CurrentOperation == GizmoOperation::Scale, "Scale (R)")) SetOperation(GizmoOperation::Scale);

    ImGui::SameLine();

    // Debug draw dropdown
    if (ImGui::BeginMenu("Debug")) {
        bool uiRects = m_UILayer ? Renderer::Get().GetShowUIRects() : false;
        if (ImGui::MenuItem("UI Rects", nullptr, uiRects)) {
            Renderer::Get().SetShowUIRects(!uiRects);
        }
        bool showUI = true;
        if (ImGui::MenuItem("Show UI Overlay", nullptr, &showUI)) {
            Renderer::Get().SetShowUIOverlay(showUI);
        }
        // Forward to existing nav debug mask window (shown in UILayer); provide quick toggles here too
        uint32_t mask = (uint32_t)nav::debug::GetMask();
        auto toggle = [&](const char* label, nav::NavDrawMask bit){ bool v = (mask & (uint32_t)bit)!=0; if(ImGui::MenuItem(label, nullptr, v)){ if(v) mask &= ~(uint32_t)bit; else mask |= (uint32_t)bit; }};
        toggle("Nav Triangles", nav::NavDrawMask::TriMesh);
        toggle("Nav Polys",     nav::NavDrawMask::Polys);
        toggle("Nav Agents",    nav::NavDrawMask::Agents);
        nav::Navigation::Get().SetDebugMask((nav::NavDrawMask)mask);
        ImGui::EndMenu();
    }

    // Shader preset now lives in UILayer next to Options

    ImGui::End();
}

void ToolbarPanel::TogglePlayMode() {
   if (!m_UILayer) return;

   // Before toggling, ensure scripts are compiled if scripts exist.
   // If no scripts are present, allow entering play mode.
   auto& pipeline = AssetPipeline::Instance();
   if (!pipeline.HasAnyScripts() && !pipeline.AreScriptsCompiled()) {
       // No scripts present: allow play, but keep watching for future scripts
   } else if (!pipeline.AreScriptsCompiled()) {
       Logger::LogError("[PlayMode] Cannot enter Play Mode until scripts compile successfully.");
       m_UILayer->FocusConsoleNextFrame();
       return;
   }

   auto& scene = m_UILayer->GetScene();

   if (!m_PlayMode) {
      if (m_UseBinaryPlayMode && m_UseTempPak) {
         if (scene.IsDirty() || m_UILayer->GetCurrentScenePath().empty()) {
            Logger::LogError("[PlayMode] Runtime Preview requires a saved scene. Save the scene and try again.");
            m_UILayer->FocusConsoleNextFrame();
            return;
         }
      }
      if (m_UseBinaryPlayMode && !m_UseTempPak) {
         const std::string& scenePath = m_UILayer->GetCurrentScenePath();
         if (!scenePath.empty() && !scene.IsDirty()) {
            BinaryAssetCache::EnsureStats stats{};
            Logger::Log("[PlayMode] Checking scene dependency binaries...");
            if (!BinaryAssetCache::Instance().EnsureSceneDependenciesCurrent(scenePath, false, &stats)) {
               Logger::LogError("[PlayMode] Failed to build one or more binaries used by the current scene.");
               m_UILayer->FocusConsoleNextFrame();
               return;
            }
            if (stats.rebuilt > 0) {
               Logger::Log("[PlayMode] Rebuilt " + std::to_string(stats.rebuilt) + " scene dependency binary asset(s).");
            } else {
               Logger::Log("[PlayMode] Scene dependency binaries are current.");
            }
         }
      }

      if (m_PlayWindowMode == PlayWindowMode::Editor) {
         // Switch asset resolver to play mode (loads from binary cache)
         if (auto* resolver = dynamic_cast<EditorAssetResolver*>(Assets::GetResolver())) {
             resolver->SetLoadMode(AssetLoadMode::PlayMode);
             resolver->SetPlayModeBinaryOnly(m_UseBinaryPlayMode);
         }
         // Match runtime VFS behavior for binary-only play mode
         if (m_UseBinaryPlayMode && !m_UseTempPak) {
            m_PrePlayDiskFallbackAllowed = FileSystem::Instance().IsDiskFallbackAllowed();
            FileSystem::Instance().SetAllowDiskFallback(false);
            m_DidOverrideDiskFallback = true;
         }
      }

      m_PlayMode = true;
      m_Paused = false;

      // Entering Play Mode - request async so overlay can render immediately
      m_UILayer->RequestBeginPlayAsync(m_UseBinaryPlayMode,
                                       m_UseBinaryPlayMode && m_UseTempPak,
                                       m_PlayWindowMode);
   } else {
      if (m_UILayer->IsExternalRuntimePreviewActive()) {
         m_UILayer->StopExternalRuntimePreview();
         m_PlayMode = false;
         m_Paused = false;
         return;
      }

      // Exiting Play Mode
      if (scene.m_RuntimeScene) {
         runtime::RuntimePrefabInstantiator::CancelAsyncForScene(*scene.m_RuntimeScene, true);
         scene.m_RuntimeScene->OnStop();
         scene.m_RuntimeScene = nullptr;
         runtime::RuntimePrefabInstantiator::ResetRuntimeCaches();
         // Restore global scene pointer immediately to avoid any late calls
         // referencing the destroyed runtime clone in the remainder of the frame
         Scene::CurrentScene = &scene;

         // Clear managed component caches to prevent stale references in next play session
         if (g_ClearComponentCaches) {
            g_ClearComponentCaches();
         }

         m_UILayer->EndRuntimePreview();

         if (m_DidOverrideDiskFallback) {
            FileSystem::Instance().SetAllowDiskFallback(m_PrePlayDiskFallbackAllowed);
            m_DidOverrideDiskFallback = false;
         }

         // Switch asset resolver back to editor mode
         if (auto* resolver = dynamic_cast<EditorAssetResolver*>(Assets::GetResolver())) {
             resolver->SetLoadMode(AssetLoadMode::Editor);
             resolver->SetPlayModeBinaryOnly(false);
         }

         m_UILayer->TogglePlayMode();
      }

      m_PlayMode = false;
      m_Paused = false;
   }
}

void ToolbarPanel::AbortPlayMode() {
   m_PlayMode = false;
   m_Paused = false;
   if (m_DidOverrideDiskFallback) {
      FileSystem::Instance().SetAllowDiskFallback(m_PrePlayDiskFallbackAllowed);
      m_DidOverrideDiskFallback = false;
   }
   if (auto* resolver = dynamic_cast<EditorAssetResolver*>(Assets::GetResolver())) {
      resolver->SetLoadMode(AssetLoadMode::Editor);
      resolver->SetPlayModeBinaryOnly(false);
   }
}

void ToolbarPanel::EnsureIconsLoaded() {
    if (m_IconsLoaded) return;
    m_PlayIcon   = TextureLoader::ToImGuiTextureID(TextureLoader::LoadIconTexture("assets/icons/play.svg"));
    m_PauseIcon  = TextureLoader::ToImGuiTextureID(TextureLoader::LoadIconTexture("assets/icons/pause.svg"));
    m_StopIcon   = TextureLoader::ToImGuiTextureID(TextureLoader::LoadIconTexture("assets/icons/stop.svg"));
    m_MoveIcon   = TextureLoader::ToImGuiTextureID(TextureLoader::LoadIconTexture("assets/icons/move.svg"));
    m_RotateIcon = TextureLoader::ToImGuiTextureID(TextureLoader::LoadIconTexture("assets/icons/rotate.svg"));
    m_ScaleIcon  = TextureLoader::ToImGuiTextureID(TextureLoader::LoadIconTexture("assets/icons/scale.svg"));
    m_IconsLoaded = true;
}
