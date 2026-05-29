#include "MenuBarPanel.h"
#include "core/ecs/Entity.h"
#include "core/ecs/Components.h"
#include "editor/Project.h"
#include "editor/ProjectGenerator.h"
#include "editor/EditorSettings.h"
#include "core/serialization/Serializer.h"
#include "core/ecs/EntityData.h"
#include "ui/UILayer.h"
#include "editor/pipeline/AssetPipeline.h"
#include "editor/pipeline/AssetRegistry.h"
#include "editor/pipeline/AssetLibrary.h"
#include "editor/pipeline/BuildExporter.h"
#include "editor/tools/WorldGraphBake.h"
#include "editor/undo/EditorSceneUndoStack.h"
#include "ui/Logger.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>  // JSON serialization
#include "core/rendering/StandardMeshManager.h"
#include "core/rendering/MaterialManager.h"
#include "ui/utility/CreateEntityMenu.h"
#include "core/rendering/Environment.h"
#include "editor/ui/FileDialogs.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include "ui/UILayer.h"
#include <cstring>
#include "ui/utility/UIHelpers.h"
#include <algorithm>
#include <cctype>

#include "core/rendering/GlobalShaderProperties.h"
#include "core/rendering/Renderer.h"

using json = nlohmann::json;

namespace {
bool ShouldSkipProjectBuildDirectory(const std::filesystem::path& path) {
    std::string name = path.filename().string();
    std::transform(name.begin(), name.end(), name.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return name == "bin" || name == "obj" || name == ".library" || name == ".git" || name == ".vs";
}

template <typename Fn>
void ForEachProjectEntry(const std::filesystem::path& rootPath, Fn&& fn) {
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        rootPath,
        std::filesystem::directory_options::skip_permission_denied,
        ec);
    std::filesystem::recursive_directory_iterator end;

    while (!ec && it != end) {
        const auto entry = *it;
        if (entry.is_directory(ec) && ShouldSkipProjectBuildDirectory(entry.path())) {
            it.disable_recursion_pending();
            ++it;
            continue;
        }

        fn(entry);
        ++it;
    }
}
} // namespace

static std::string WideToUtf8(const std::wstring& wide)
{
    if (wide.empty()) return {};
    int requiredBytes = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (requiredBytes <= 0) return {};
    std::string utf8(static_cast<size_t>(requiredBytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, utf8.data(), requiredBytes, nullptr, nullptr);
    return utf8;
}

static std::wstring Utf8ToWide(const std::string& utf8)
{
    if (utf8.empty()) return {};
    int requiredWchars = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (requiredWchars <= 0) return {};
    std::wstring wide(static_cast<size_t>(requiredWchars - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), requiredWchars);
    return wide;
}

std::string ShowOpenFolderDialog() {
    IFileDialog* pFileDialog;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pFileDialog));
    if (FAILED(hr)) return "";

    DWORD options;
    pFileDialog->GetOptions(&options);
    pFileDialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);

    hr = pFileDialog->Show(nullptr);
    if (SUCCEEDED(hr)) {
        IShellItem* pItem;
        hr = pFileDialog->GetResult(&pItem);
        if (SUCCEEDED(hr)) {
            PWSTR path;
            pItem->GetDisplayName(SIGDN_FILESYSPATH, &path);
            std::wstring ws(path);
            CoTaskMemFree(path);
            pItem->Release();
            pFileDialog->Release();
            return WideToUtf8(ws);
        }
    }
    pFileDialog->Release();
    return "";
}

std::string ShowSaveFileDialog(const std::string& defaultName = "scene.scene") {
    IFileDialog* pFileDialog;
    HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pFileDialog));
    if (FAILED(hr)) return "";

    // Set file type filter
    COMDLG_FILTERSPEC filterSpec[] = {
        { L"Scene Files", L"*.scene" },
        { L"All Files", L"*.*" }
    };
    pFileDialog->SetFileTypes(2, filterSpec);
    pFileDialog->SetDefaultExtension(L"scene");

    // Set default file name
    std::wstring defaultNameW = Utf8ToWide(defaultName);
    pFileDialog->SetFileName(defaultNameW.c_str());

    hr = pFileDialog->Show(nullptr);
    if (SUCCEEDED(hr)) {
        IShellItem* pItem;
        hr = pFileDialog->GetResult(&pItem);
        if (SUCCEEDED(hr)) {
            PWSTR path;
            pItem->GetDisplayName(SIGDN_FILESYSPATH, &path);
            std::wstring ws(path);
            CoTaskMemFree(path);
            pItem->Release();
            pFileDialog->Release();
            return WideToUtf8(ws);
        }
    }

    pFileDialog->Release();
    return "";
}

std::string ShowOpenFileDialog() {
    IFileDialog* pFileDialog;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pFileDialog));
    if (FAILED(hr)) return "";

    // Set file type filter
    COMDLG_FILTERSPEC filterSpec[] = {
        { L"Scene Files", L"*.scene" },
        { L"All Files", L"*.*" }
    };
    pFileDialog->SetFileTypes(2, filterSpec);

    hr = pFileDialog->Show(nullptr);
    if (SUCCEEDED(hr)) {
        IShellItem* pItem;
        hr = pFileDialog->GetResult(&pItem);
        if (SUCCEEDED(hr)) {
            PWSTR path;
            pItem->GetDisplayName(SIGDN_FILESYSPATH, &path);
            std::wstring ws(path);
            CoTaskMemFree(path);
            pItem->Release();
            pFileDialog->Release();
            return WideToUtf8(ws);
        }
    }

    pFileDialog->Release();
    return "";
}

void MenuBarPanel::OpenExportDialog() {
    m_ExportPopupOpen = true;
    m_ShowExportResult = false;
    m_ExportOutDir = (Project::GetProjectDirectory() / "dist" / "Windows" / Project::GetProjectName()).string();
    m_ExportEntryScene.clear();
    if (m_UILayer) {
        const std::string& currentScenePath = m_UILayer->GetCurrentScenePath();
        if (!currentScenePath.empty()) {
            m_ExportEntryScene = currentScenePath;
        }
    }
    ImGui::OpenPopup("Export Standalone");
}

bool MenuBarPanel::RenderRegisteredAction(const char* id) {
    if (!m_UILayer || !id) {
        return false;
    }

    const editorui::EditorActionDefinition* action = m_UILayer->FindEditorAction(id);
    if (!action) {
        return false;
    }

    const bool checked = action->IsChecked ? action->IsChecked() : false;
    const bool enabled = !action->IsEnabled || action->IsEnabled();
    const char* shortcut = action->ShortcutLabel.empty() ? nullptr : action->ShortcutLabel.c_str();
    if (ImGui::MenuItem(action->Label.c_str(), shortcut, checked, enabled)) {
        if (action->SetChecked && action->IsChecked) {
            m_UILayer->SetEditorActionChecked(action->Id, !checked);
        } else {
            m_UILayer->ExecuteEditorAction(action->Id);
        }
    }
    return true;
}

bool MenuBarPanel::RenderRegisteredActionMenu(const char* category) {
    if (!m_UILayer || !category) {
        return false;
    }

    const auto actions = m_UILayer->GetEditorActionsForCategory(category);
    bool renderedAny = false;
    for (const editorui::EditorActionDefinition* action : actions) {
        if (!action) {
            continue;
        }

        renderedAny = true;
        const bool checked = action->IsChecked ? action->IsChecked() : false;
        const bool enabled = !action->IsEnabled || action->IsEnabled();
        const char* shortcut = action->ShortcutLabel.empty() ? nullptr : action->ShortcutLabel.c_str();
        if (ImGui::MenuItem(action->Label.c_str(), shortcut, checked, enabled)) {
            if (action->SetChecked && action->IsChecked) {
                m_UILayer->SetEditorActionChecked(action->Id, !checked);
            } else {
                m_UILayer->ExecuteEditorAction(action->Id);
            }
        }
    }

    return renderedAny;
}

void MenuBarPanel::OnImGuiRender() {
    // FILE MENU
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Scene")) {
            if (m_UILayer) {
                m_UILayer->NewScene();
            } else {
                *m_Context = Scene();
                m_Context->SetScenePath("");
                m_Context->ClearDirty();
                *m_SelectedEntity = -1;
                EditorSceneUndoStack::Get().ResetToScene(m_Context, m_SelectedEntity);
            }
        }

        ImGui::Separator();
        
        if (ImGui::MenuItem("Save Scene")) {
            if (m_UILayer) {
                m_UILayer->SaveCurrentScene(false);
            } else {
                std::string scenePath = ShowSaveFileDialog("NewScene.scene");
                if (!scenePath.empty()) {
                    Serializer::SaveSceneToFile(*m_Context, scenePath);
                }
            }
        }
        
        if (ImGui::MenuItem("Save Scene As...")) {
            if (m_UILayer) {
                m_UILayer->SaveCurrentScene(true);
            } else {
                std::string scenePath = ShowSaveFileDialog("NewScene.scene");
                if (!scenePath.empty()) {
                    Serializer::SaveSceneToFile(*m_Context, scenePath);
                }
            }
        }
        
        if (ImGui::MenuItem("Load Scene...")) {
            if (m_UILayer) {
                m_UILayer->PromptLoadScene();
            } else {
                std::string scenePath = ShowOpenFileDialog();
                if (!scenePath.empty() && Serializer::LoadSceneFromFile(scenePath, *m_Context)) {
                    *m_SelectedEntity = -1;
                    EditorSceneUndoStack::Get().ResetToScene(m_Context, m_SelectedEntity);
                }
            }
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Open Project...")) {
            std::string path = ShowOpenFolderDialog();
            if (!path.empty()) {
                std::filesystem::path projPath = path;
                std::filesystem::path clayprojFile;

                // Look for existing .clayproj file
                for (const auto& entry : std::filesystem::directory_iterator(projPath)) {
                    if (entry.path().extension() == ".clayproj") {
                        clayprojFile = entry.path();
                        break;
                    }
                }

                // If no .clayproj found, create one
                if (clayprojFile.empty()) {
                    clayprojFile = projPath / (projPath.filename().string() + ".clayproj");

                    json projectJson = {
                        { "name", projPath.filename().string() },
                        { "version", "1.0" },
                        { "scenes", json::array() }
                    };

                    std::ofstream outFile(clayprojFile);
                    if (!outFile) {
                        std::cerr << "[MenuBarPanel] Failed to create project file: " << clayprojFile << std::endl;
                        return;
                    }

                    outFile << projectJson.dump(4); // Pretty print with indent
                    outFile.close();

                    std::cout << "[MenuBarPanel] Created new .clayproj file: " << clayprojFile << std::endl;
                }
                else {
                    std::cout << "[MenuBarPanel] Found existing project file: " << clayprojFile << std::endl;
                }

                Project::Load(clayprojFile.string());
                Renderer::Get().SetDefaultTextFont(Project::GetDefaultFontPath());
                if (m_UILayer) {
                    m_UILayer->LoadProject(projPath.string());
                } else {
                    m_ProjectPanel->LoadProject(projPath.string());
                }
            }
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Exit")) {
            // Hook into application quit logic
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        EditorSceneUndoStack& undoStack = EditorSceneUndoStack::Get();
        const std::string undoLabel = undoStack.GetUndoLabel();
        const std::string redoLabel = undoStack.GetRedoLabel();
        std::string undoMenu = undoLabel.empty() ? "Undo" : ("Undo " + undoLabel);
        std::string redoMenu = redoLabel.empty() ? "Redo" : ("Redo " + redoLabel);
        if (ImGui::MenuItem(undoMenu.c_str(), "Ctrl+Z", false, undoStack.CanUndo())) {
            undoStack.Undo();
        }
        if (ImGui::MenuItem(redoMenu.c_str(), "Ctrl+Y", false, undoStack.CanRedo())) {
            undoStack.Redo();
        }
        ImGui::EndMenu();
    }

    // BUILD MENU
    if (ImGui::BeginMenu("Build")) {
        RenderRegisteredAction("build.export_standalone");
        ImGui::EndMenu();
    }
    // Note: actual popup rendering is triggered from UILayer after EndMenuBar

    // SCENE MENU
    if (ImGui::BeginMenu("Scene")) {
        if (ImGui::BeginMenu("Environment")) {
            Environment& env = m_Context->GetEnvironment();
            if (ImGui::BeginTabBar("##envtabs")) {
                if (ImGui::BeginTabItem("Fog & Ambient")) {
                    ImGui::Checkbox("Enable Fog", &env.EnableFog);
                    ImGui::ColorEdit3("Fog Color", (float*)&env.FogColor);
                    ImGui::SliderFloat("Fog Density", &env.FogDensity, 0.0f, 0.2f, "%.3f");
                    ImGui::Separator();
                    ImGui::ColorEdit3("Ambient Color", (float*)&env.AmbientColor);
                    ImGui::SliderFloat("Ambient Intensity", &env.AmbientIntensity, 0.0f, 5.0f, "%.2f");
                    ImGui::Separator();
                    ImGui::SliderFloat("Exposure", &env.Exposure, 0.1f, 5.0f, "%.2f");
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Global Shader Properties")) {
                    ImGui::TextDisabled("Globals apply to all materials (persisted with scene)");

                    // Texture Filter (scene default for materials)
                    {
                        int cur = (env.TextureFilter == Environment::TextureFilterMode::Linear) ? 0 : 1;
                        const char* items[] = { "Linear", "Point" };
                        if (ImGui::Combo("Texture Filter", &cur, items, IM_ARRAYSIZE(items))) {
                            env.TextureFilter = (cur == 0)
                                ? Environment::TextureFilterMode::Linear
                                : Environment::TextureFilterMode::Point;
                        }
                        ImGui::Separator();
                    }

                    // Shader dropdown: PBR, PSX
                    static int s_selectedShader = 0; // 0=PBR, 1=PSX
                    const char* shaderOptions[] = { "PBR", "PSX" };
                    ImGui::Combo("Shader", &s_selectedShader, shaderOptions, IM_ARRAYSIZE(shaderOptions));
                    ImGui::Separator();

                    if (s_selectedShader == 0) {
                        // PBR globals
                        glm::vec4 tint(1.0f);
                        GlobalShaderProperties::Instance().TryGetVec4("u_ColorTint", tint);
                        if (ImGui::ColorEdit4("Global Tint (Project)", &tint.x)) {
                            GlobalShaderProperties::Instance().SetVec4("u_ColorTint", tint);
                        }
                    } else if (s_selectedShader == 1) {
                        // PSX globals: u_psxParams(x=jitter px, y=affine 0..1, z=light influence 0..1, w=unused)
                        //             u_toonParams(x=bands 1..8, y=band softness 0..1)
                        glm::vec4 psx(0.0f, 0.0f, 0.0f, 0.0f);
                        GlobalShaderProperties::Instance().TryGetVec4("u_psxParams", psx);
                        float jitter = psx.x;
                        float affine = psx.y;
                        float lightInf = psx.z;
                        bool changed = false;
                        changed |= ImGui::SliderFloat("Vertex Jitter (px)", &jitter, 0.0f, 4.0f, "%.1f");
                        changed |= ImGui::SliderFloat("Affine Warp", &affine, 0.0f, 1.0f, "%.2f");
                        changed |= ImGui::SliderFloat("Light Influence", &lightInf, 0.0f, 1.0f, "%.2f");
                        if (changed) {
                            psx.x = jitter; psx.y = affine; psx.z = lightInf;
                            GlobalShaderProperties::Instance().SetVec4("u_psxParams", psx);
                        }

                        glm::vec4 toon(3.0f, 1.0f, 0.0f, 0.0f);
                        GlobalShaderProperties::Instance().TryGetVec4("u_toonParams", toon);
                        float bands = toon.x;
                        float soft = toon.y;
                        bool changedToon = false;
                        changedToon |= ImGui::SliderFloat("Shadow Bands", &bands, 1.0f, 8.0f, "%.0f");
                        changedToon |= ImGui::SliderFloat("Smoothness", &soft, 0.0f, 1.0f, "%.2f");
                        if (changedToon) {
                            toon.x = bands; toon.y = soft;
                            GlobalShaderProperties::Instance().SetVec4("u_toonParams", toon);
                        }

                        // Posterization for albedo: u_posterize.x = levels (0=off)
                        glm::vec4 poster(0.0f, 0.0f, 0.0f, 0.0f);
                        GlobalShaderProperties::Instance().TryGetVec4("u_posterize", poster);
                        float posterLevels = poster.x;
                        if (ImGui::SliderFloat("Albedo Posterize Levels", &posterLevels, 0.0f, 16.0f, "%.0f")) {
                            poster.x = posterLevels;
                            GlobalShaderProperties::Instance().SetVec4("u_posterize", poster);
                        }
                    }

                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Sky")) {
                    ImGui::Checkbox("Use Skybox (Texture)", &env.UseSkybox);
                    if (!env.UseSkybox) {
                        env.UseSkyboxEquirectangular = false;
                    }
                    if (env.UseSkybox) {
                        ImGui::Indent();
                        ImGui::Checkbox("Use Single Image (Equirectangular)", &env.UseSkyboxEquirectangular);
                        if (env.UseSkyboxEquirectangular) {
                            char buffer[512];
                            std::strncpy(buffer, env.SkyboxEquirectangularPath.c_str(), sizeof(buffer));
                            buffer[sizeof(buffer) - 1] = '\0';
                            ImGui::InputText("Image Path", buffer, sizeof(buffer));
                            if (std::string(buffer) != env.SkyboxEquirectangularPath) {
                                env.SkyboxEquirectangularPath = buffer;
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("Pick##SkyEq")) {
                                std::string picked = ShowOpenFileDialogExt(L"Skybox Image", L"*");
                                if (!picked.empty()) {
                                    env.SkyboxEquirectangularPath = picked;
                                }
                            }
                            int cubeRes = static_cast<int>(env.SkyboxEquirectangularResolution);
                            if (ImGui::SliderInt("Cubemap Resolution", &cubeRes, 32, 2048, "%d")) {
                                env.SkyboxEquirectangularResolution = static_cast<uint16_t>(std::clamp(cubeRes, 32, 2048));
                            }
                        } else {
                            static const char* kFaceLabels[6] = { "+X (Right)", "-X (Left)", "+Y (Top)", "-Y (Bottom)", "+Z (Front)", "-Z (Back)" };
                            for (int i = 0; i < 6; ++i) {
                                ImGui::PushID(i);
                                char buffer[512];
                                std::strncpy(buffer, env.SkyboxFacePaths[i].c_str(), sizeof(buffer));
                                buffer[sizeof(buffer) - 1] = '\0';
                                ImGui::InputText(kFaceLabels[i], buffer, sizeof(buffer));
                                if (std::string(buffer) != env.SkyboxFacePaths[i]) {
                                    env.SkyboxFacePaths[i] = buffer;
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("Pick")) {
                                    std::string picked = ShowOpenFileDialogExt(L"Texture File", L"*");
                                    if (!picked.empty()) {
                                        env.SkyboxFacePaths[i] = picked;
                                    }
                                }
                                ImGui::PopID();
                            }
                            ImGui::TextDisabled("Order: +X, -X, +Y, -Y, +Z, -Z");
                        }
                        ImGui::Unindent();
                        ImGui::Separator();
                    }
                    
                    ImGui::Checkbox("Procedural Sky", &env.ProceduralSky);
                    if (env.ProceduralSky) {
                        ImGui::Indent();
                        
                        ImGui::Text("Sky Colors");
                        ImGui::ColorEdit3("Top Color", (float*)&env.SkyTopColor);
                        ImGui::ColorEdit3("Horizon Color", (float*)&env.SkyHorizonColor);
                        ImGui::ColorEdit3("Ground Color", (float*)&env.SkyGroundColor);
                        
                        ImGui::Separator();
                        
                        ImGui::Text("Sun");
                        ImGui::SliderFloat("Sun Size", &env.SunSize, 0.0f, 1.0f, "%.3f");
                        ImGui::SliderFloat("Sun Size Convergence", &env.SunSizeConvergence, 1.0f, 20.0f, "%.1f");
                        ImGui::SliderFloat("Sun Intensity", &env.SunIntensity, 0.0f, 5.0f, "%.2f");
                        
                        ImGui::Separator();
                        
                        ImGui::Text("Atmosphere");
                        ImGui::SliderFloat("Atmosphere Thickness", &env.AtmosphereThickness, 0.0f, 2.0f, "%.2f");
                        ImGui::SliderFloat("Horizon Fade", &env.HorizonFade, 0.0f, 1.0f, "%.2f");
                        
                        ImGui::Separator();
                        
                        ImGui::Text("Exposure");
                        ImGui::SliderFloat("Sky Exposure", &env.SkyExposure, 0.0f, 5.0f, "%.2f");
                        
                        ImGui::Unindent();
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Outline")) {
                    ImGui::Checkbox("Enable Outline", &env.OutlineEnabled);
                    ImGui::ColorEdit3("Outline Color", (float*)&env.OutlineColor);
                    ImGui::SliderFloat("Thickness (px)", &env.OutlineThickness, 1.0f, 8.0f, "%.0f");
                    ImGui::TextDisabled("Screen-space outline applied to all visible meshes.");
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Shadows")) {
                    ImGui::Checkbox("Enable Shadows", &env.ShadowsEnabled);
                    ImGui::Separator();
                    static const int kResOptions[] = { 512, 1024, 2048, 4096 };
                    int curr = 1; // default to 1024 index
                    for (int i = 0; i < 4; ++i) if (env.ShadowMapResolution == kResOptions[i]) curr = i;
                    if (ImGui::SliderInt("Resolution", &curr, 0, 3, "%d")) {
                        env.ShadowMapResolution = kResOptions[curr];
                    }
                    ImGui::SliderFloat("Distance/Size", &env.ShadowDistance, 5.0f, 200.0f, "%.1f");
                    ImGui::SliderFloat("Bias", &env.ShadowBias, 0.0001f, 0.01f, "%.4f");
                    ImGui::SliderFloat("Normal Bias", &env.ShadowNormalBias, 0.0f, 3.0f, "%.2f");
                    ImGui::SliderFloat("Softness (radius)", &env.ShadowSoftness, 0.0f, 3.0f, "%.2f");
                    int samplesIdx = 2; // 9
                    if (env.ShadowSamples == 1) samplesIdx = 0; else if (env.ShadowSamples == 4) samplesIdx = 1; else if (env.ShadowSamples == 16) samplesIdx = 3;
                    if (ImGui::SliderInt("Samples", &samplesIdx, 0, 3, "%d")) {
                        env.ShadowSamples = (samplesIdx==0)?1:(samplesIdx==1)?4:(samplesIdx==2)?9:16;
                    }
                    ImGui::SliderFloat("Strength", &env.ShadowStrength, 0.0f, 1.0f, "%.2f");
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            // If we're in play mode (runtime scene active), mirror environment to the runtime scene so changes apply immediately
            if (m_Context && m_Context->m_RuntimeScene) {
                m_Context->m_RuntimeScene->GetEnvironment() = env;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    // EDIT MENU
    if (ImGui::BeginMenu("Edit")) {
        RenderRegisteredAction("workspace.command_palette");
        ImGui::Separator();
        RenderRegisteredAction("workspace.project_settings");
        
        if (ImGui::BeginMenu("Viewport Settings")) {
            EditorSettings& settings = EditorSettings::Get();
            
            // Camera Navigation
            if (ImGui::CollapsingHeader("Camera Navigation", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::SliderFloat("Zoom Speed", &settings.ZoomBaseSpeed, 0.1f, 3.0f, "%.1f");
                ImGui::SliderFloat("Zoom Acceleration", &settings.ZoomAcceleration, 0.0f, 0.5f, "%.2f");
                ImGui::Checkbox("Smooth Zoom", &settings.SmoothZoomEnabled);
                if (settings.SmoothZoomEnabled) {
                    ImGui::SliderFloat("Zoom Smoothness", &settings.ZoomSmoothness, 0.0f, 0.5f, "%.2f");
                }
                ImGui::SliderFloat("Orbit Sensitivity", &settings.OrbitSensitivity, 0.05f, 0.5f, "%.2f");
                ImGui::SliderFloat("Pan Speed", &settings.PanSpeedFactor, 0.001f, 0.05f, "%.3f");
            }
            
            // Focus Settings
            if (ImGui::CollapsingHeader("Focus (F Key)")) {
                ImGui::SliderFloat("Animation Duration", &settings.FocusDuration, 0.0f, 1.0f, "%.2f s");
                ImGui::SliderFloat("Distance Padding", &settings.FocusDistancePadding, 1.0f, 3.0f, "%.1f");
            }
            
            // Picking Settings
            if (ImGui::CollapsingHeader("Entity Picking")) {
                ImGui::Checkbox("Deselect on Empty Click", &settings.DeselectOnEmptyClick);
                ImGui::Checkbox("Skip Hidden Entities", &settings.PickingSkipHidden);
                ImGui::Checkbox("Skip Locked Entities", &settings.PickingSkipLocked);
            }
            
            // Grid Settings
            if (ImGui::CollapsingHeader("Grid")) {
                ImGui::Checkbox("Show Axis Lines", &settings.GridShowAxisLines);
                ImGui::SliderInt("Major Line Interval", &settings.GridMajorLineInterval, 0, 50);
                if (settings.GridShowAxisLines) {
                    ImGui::ColorEdit4("X Axis Color", &settings.GridAxisColorX.x, ImGuiColorEditFlags_NoInputs);
                    ImGui::ColorEdit4("Z Axis Color", &settings.GridAxisColorZ.x, ImGuiColorEditFlags_NoInputs);
                }
                ImGui::ColorEdit4("Grid Color", &settings.GridColor.x, ImGuiColorEditFlags_NoInputs);
                ImGui::ColorEdit4("Major Line Color", &settings.GridMajorColor.x, ImGuiColorEditFlags_NoInputs);
            }
            
            // Gizmo Settings
            if (ImGui::CollapsingHeader("Gizmos")) {
                ImGui::SliderFloat("Gizmo Scale", &settings.GizmoBaseScale, 0.5f, 2.0f, "%.1f");
                ImGui::Checkbox("Auto-Scale Gizmos", &settings.GizmoAutoScale);
            }
            
            // Selection Feedback
            if (ImGui::CollapsingHeader("Selection Feedback")) {
                ImGui::Checkbox("Show Hover Outline", &settings.ShowHoverOutline);
                if (settings.ShowHoverOutline) {
                    ImGui::ColorEdit4("Hover Outline Color", &settings.HoverOutlineColor.x, ImGuiColorEditFlags_NoInputs);
                    ImGui::SliderFloat("Hover Outline Thickness", &settings.HoverOutlineThickness, 1.0f, 5.0f, "%.0f px");
                }
            }
            
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    // WINDOW MENU
    if (ImGui::BeginMenu("Window")) {
        if (!RenderRegisteredActionMenu("Window")) {
            ImGui::MenuItem("Animation Controller", nullptr, false, false);
            ImGui::MenuItem("Animation Graph", nullptr, false, false);
            ImGui::MenuItem("Animation Timeline", nullptr, false, false);
            ImGui::MenuItem("Node Graph", nullptr, false, false);
            ImGui::MenuItem("Shader Graph", nullptr, false, false);
            ImGui::MenuItem("Dialogue Editor", nullptr, false, false);
            ImGui::MenuItem("Quest Editor", nullptr, false, false);
            ImGui::Separator();
            ImGui::MenuItem("Script Registry", nullptr, false, false);
            ImGui::MenuItem("Asset Registry", nullptr, false, false);
        }
        ImGui::EndMenu();
    }

    // TOOLS MENU
    if (ImGui::BeginMenu("Tools")) {
        const bool renderedToolActions = RenderRegisteredActionMenu("Tool");
        if (renderedToolActions) {
            ImGui::Separator();
        }
        if (ImGui::MenuItem("Bake World Graph")) {
            cm::editor::worldgraph::BakeWorldGraph();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Create .csproj")) {
            namespace fs = std::filesystem;

            fs::path projectDir = Project::GetProjectDirectory();
            std::string projectName = Project::GetProjectName();
            if (projectDir.empty() || projectName.empty()) {
                std::cerr << "[MenuBarPanel] No project open. Cannot create .csproj." << std::endl;
            } else {
                if (!ProjectGenerator::EnsureManagedScriptProject(projectDir, projectName, Project::GetModules())) {
                    std::cerr << "[MenuBarPanel] Failed to create .csproj for " << projectDir << std::endl;
                }
            }
        }
        if (ImGui::MenuItem("Reimport Assets")) {
            namespace fs = std::filesystem;
            std::string projectDir = Project::GetProjectDirectory().string();

            // 1. Delete all .meta files in project directory
            ForEachProjectEntry(projectDir, [&](const fs::directory_entry& entry) {
                if (entry.is_regular_file() && entry.path().extension() == ".meta") {
                    try {
                        fs::remove(entry.path());
                    } catch (const std::exception& e) {
                        std::cerr << "[ReimportAssets] Failed to delete meta file: " << entry.path() << "\n";
                    }
                }
            });

            // 2. Optionally clear cached processed assets folder
            if (fs::exists("cache")) {
                try { fs::remove_all("cache"); }
                catch(...) {}
            }

            // 3. Clear registries so they repopulate on import
            AssetRegistry::Instance().Clear();
            AssetLibrary::Instance().Clear();

            // 4. Scan and import all assets anew (this will compile scripts as well)
            AssetPipeline::Instance().ScanProject(projectDir);
            // Drain imports synchronously so registry is populated before fixup and printing
            AssetPipeline::Instance().ProcessAllBlocking();
            // Log what will be imported this run
            const auto& list = AssetPipeline::Instance().GetLastScanList();
            std::cout << "[MenuBarPanel] Reimport Assets triggered. Files scanned: " << list.size() << std::endl;
            for (const auto& p : list) {
                std::cout << "  - " << p << std::endl;
            }
            // 5. Fix up scenes/prefabs GUIDs by name/path where needed
            AssetPipeline::Instance().FixupAssetReferencesByName(Project::GetProjectDirectory().string());
            // 6. One more pass to pick up newly fixed paths in registration
            AssetPipeline::Instance().ScanProject(projectDir);
            AssetPipeline::Instance().ProcessAllBlocking();
        }
        if (ImGui::MenuItem("Rebuild Prefab Binaries")) {
            AssetPipeline::Instance().RebuildAllPrefabBinaries();
        }

        if (ImGui::MenuItem("Reimport Scripts")) {
            namespace fs = std::filesystem;
            std::string projectDir = Project::GetProjectDirectory().string();

            // 1. Delete .meta files for scripts and collect first script path
            std::string firstScriptPath;
            ForEachProjectEntry(projectDir, [&](const fs::directory_entry& entry) {
                if (entry.is_regular_file() && entry.path().extension() == ".cs") {
                    fs::path meta = entry.path();
                    meta += ".meta";
                    if (fs::exists(meta)) {
                        try { fs::remove(meta); } catch(...) {}
                    }

                    AssetRegistry::Instance().RemoveMetadata(entry.path().string());

                    if (firstScriptPath.empty())
                        firstScriptPath = entry.path().string();
                }
            });

            // 2. Remove existing compiled DLL to force rebuild (if present)
            try {
                fs::path projDir = Project::GetProjectDirectory();
                fs::path dllPath = projDir / ".library" / "GameScripts.dll";
                if (fs::exists(dllPath)) fs::remove(dllPath);
            } catch(...) {}

            // 3. Recompile scripts if any found
            if (!firstScriptPath.empty()) {
                AssetPipeline::Instance().ImportScript(firstScriptPath);
                std::cout << "[MenuBarPanel] Reimport Scripts triggered." << std::endl;
            } else {
                std::cerr << "[MenuBarPanel] No .cs scripts found in project." << std::endl;
            }
        }
        ImGui::Separator();
        if (!RenderRegisteredAction("workspace.reset_layout")) {
            if (ImGui::MenuItem("Reset Layout") && m_UILayer) {
                m_UILayer->RequestLayoutReset();
            }
        }
        ImGui::Separator();
        if (!RenderRegisteredAction("workspace.ui_scale_increase")) {
            if (ImGui::MenuItem("UI Scale +") && m_UILayer) m_UILayer->AdjustUIScale(0.1f);
        }
        if (!RenderRegisteredAction("workspace.ui_scale_decrease")) {
            if (ImGui::MenuItem("UI Scale -") && m_UILayer) m_UILayer->AdjustUIScale(-0.1f);
        }
        ImGui::Separator();
        bool probe = uihelpers::g_EnableUILayoutProbe;
        if (ImGui::Checkbox("UI Layout Probe", &probe)) {
            uihelpers::g_EnableUILayoutProbe = probe;
        }
        ImGui::EndMenu();
    }

    // DEBUG MENU
    if (ImGui::BeginMenu("Debug")) {
        if (ImGui::MenuItem("Serializer Test")) {
            if (m_UILayer) m_UILayer->OpenSerializerSanity();
        }
        ImGui::EndMenu();
    }

    // ENTITY MENU
    if (ImGui::BeginMenu("Entity")) {
        if (ImGui::BeginMenu("Create")) {
            DrawCreateEntityMenuItems(m_Context, m_SelectedEntity);
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
}

void MenuBarPanel::RenderExportPopup() {
    // Set a fixed width for the export dialog
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Always);
    
    if (ImGui::BeginPopupModal("Export Standalone", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        // If showing result after export
        if (m_ShowExportResult) {
            if (m_ExportSucceeded) {
                ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "Build Completed Successfully!");
                ImGui::Spacing();
                ImGui::Text("Output: %s", m_ExportOutDir.c_str());
            } else {
                ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.2f, 1.0f), "Build Failed");
            }
            
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Text("Build Log:");
            
            // Scrollable log area
            ImGui::BeginChild("##buildlog", ImVec2(0, 200), true);
            for (const auto& line : m_ExportLog) {
                // Color errors red, warnings yellow
                if (line.find("ERROR") != std::string::npos) {
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", line.c_str());
                } else if (line.find("WARNING") != std::string::npos || line.find("Warning") != std::string::npos) {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s", line.c_str());
                } else {
                    ImGui::TextUnformatted(line.c_str());
                }
            }
            ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
            
            ImGui::Spacing();
            if (m_ExportSucceeded) {
                if (ImGui::Button("Open Folder", ImVec2(120, 0))) {
                    // Open the output folder in explorer
                    std::string cmd = "explorer \"" + m_ExportOutDir + "\"";
                    system(cmd.c_str());
                }
                ImGui::SameLine();
            }
            if (ImGui::Button("Close", ImVec2(120, 0))) {
                m_ShowExportResult = false;
                m_ExportLog.clear();
                ImGui::CloseCurrentPopup();
                m_ExportPopupOpen = false;
            }
            ImGui::EndPopup();
            return;
        }
        
        // Normal export configuration UI
        ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "Build Settings");
        ImGui::Separator();
        ImGui::Spacing();
        
        // Output Directory
        ImGui::Text("Output Directory:");
        static char outBuf[1024];
        if (m_ExportOutDir.size() >= sizeof(outBuf)) m_ExportOutDir.resize(sizeof(outBuf)-1);
        std::strncpy(outBuf, m_ExportOutDir.c_str(), sizeof(outBuf));
        outBuf[sizeof(outBuf)-1] = '\0';
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 80);
        if (ImGui::InputText("##outdir", outBuf, sizeof(outBuf))) {
            m_ExportOutDir = outBuf;
        }
        ImGui::SameLine();
        if (ImGui::Button("Browse##dir", ImVec2(70, 0))) {
            std::string sel = ShowOpenFolderDialog();
            if (!sel.empty()) m_ExportOutDir = sel;
        }
        
        ImGui::Spacing();
        
        // Entry Scene
        ImGui::Text("Entry Scene:");
        static char sceneBuf[1024];
        if (m_ExportEntryScene.size() >= sizeof(sceneBuf)) m_ExportEntryScene.resize(sizeof(sceneBuf)-1);
        std::strncpy(sceneBuf, m_ExportEntryScene.c_str(), sizeof(sceneBuf));
        sceneBuf[sizeof(sceneBuf)-1] = '\0';
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 80);
        if (ImGui::InputText("##entryscene", sceneBuf, sizeof(sceneBuf))) {
            m_ExportEntryScene = sceneBuf;
        }
        ImGui::SameLine();
        if (ImGui::Button("Browse##scene", ImVec2(70, 0))) {
            std::string sel = ShowOpenFileDialog();
            if (!sel.empty()) m_ExportEntryScene = sel;
        }
        
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        
        // Options
        ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "Options");
        ImGui::Checkbox("Validate before export", &m_ExportValidateFirst);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Run pre-flight checks to verify all required files exist");
        }
        ImGui::Checkbox("Compress pak file", &m_ExportCompressPak);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Compress assets in the pak file to reduce build size");
        }
        
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        
        // Validation preview
        bool canExport = !m_ExportOutDir.empty() && !m_ExportEntryScene.empty();
        if (!canExport) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "Please select an output folder and an entry scene.");
        } else {
            // Quick validation status
            bool sceneExists = std::filesystem::exists(m_ExportEntryScene);
            if (sceneExists) {
                ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "Ready to export");
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Entry scene not found!");
                canExport = false;
            }
        }
        
        ImGui::Spacing();
        
        // Export button
        ImGui::BeginDisabled(!canExport);
        if (ImGui::Button("Build", ImVec2(120, 30))) {
            m_ExportLog.clear();
            m_ExportProgress = 0.0f;
            m_ExportStatus = "Starting...";
            
            BuildExporter::Options opts;
            opts.outputDirectory = m_ExportOutDir;
            opts.entryScenes = { m_ExportEntryScene };
            opts.validateBeforeExport = m_ExportValidateFirst;
            opts.compressPak = m_ExportCompressPak;
            
            // Progress callback to capture log
            auto progressCb = [this](float progress, const std::string& status) {
                m_ExportProgress = progress;
                m_ExportStatus = status;
                m_ExportLog.push_back(status);
            };
            
            Logger::Log("Starting standalone export...");
            m_ExportLog.push_back("=== Build Started ===");
            
            m_ExportSucceeded = BuildExporter::ExportProject(opts, progressCb);
            
            if (m_ExportSucceeded) {
                m_ExportLog.push_back("=== Build Succeeded ===");
                Logger::Log(std::string("Export completed successfully to: ") + m_ExportOutDir);
                if (m_UILayer) {
                    m_UILayer->QueueNotification(editorui::EditorNotificationLevel::Success, "Standalone export completed.");
                }
            } else {
                m_ExportLog.push_back("=== Build Failed ===");
                Logger::LogError("Export failed - see log for details");
                if (m_UILayer) {
                    m_UILayer->QueueNotification(editorui::EditorNotificationLevel::Error, "Standalone export failed.");
                }
            }
            
            m_ShowExportResult = true;
        }
        ImGui::EndDisabled();
        
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 30))) {
            ImGui::CloseCurrentPopup();
            m_ExportPopupOpen = false;
        }
        
        ImGui::EndPopup();
    } else if (m_ExportPopupOpen) {
        // If not visible this frame, reopen to ensure it shows after click
        ImGui::OpenPopup("Export Standalone");
    }
}
