#include "ProjectSettingsPanel.h"
#include <imgui.h>
#include <filesystem>
#include <cstring>
#include "editor/Project.h"
#include "managed/ModuleLoader.h"
#include "managed/interop/ModuleInterop.h"
#include "ui/Logger.h"
#include "core/physics/PhysicsLayerManager.h"
#include "core/rendering/MaterialCache.h"
#include "core/rendering/TextureLoader.h"
#include "core/rendering/Renderer.h"
#include "editor/ui/utility/TextureSlotPicker.h"
#include "editor/ui/utility/EditorThemeUtils.h"
#include "editor/pipeline/AssetLibrary.h"
#include "editor/pipeline/AssetPipeline.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef _WIN32
#include <Windows.h>
#endif

namespace {
void CopyStringToBuffer(const std::string& value, char* buffer, size_t bufferSize) {
    if (bufferSize == 0) {
        return;
    }

    std::strncpy(buffer, value.c_str(), bufferSize - 1);
    buffer[bufferSize - 1] = '\0';
}
}

void ProjectSettingsPanel::OnImGuiRender() {
    if (!ImGui::Begin("Project Settings")) { ImGui::End(); return; }
    OnImGuiRenderEmbedded();
    ImGui::End();
}

void ProjectSettingsPanel::OnImGuiRenderEmbedded() {
    SyncProjectNameDraft();

    const std::filesystem::path projectFileName = Project::GetProjectFile().filename();
    const std::string projectFileLabel = projectFileName.empty()
        ? std::string("(not yet saved)")
        : projectFileName.string();

    ImGui::Text("Project");
    ImGui::SetNextItemWidth(320.0f);
    const bool submittedRename = ImGui::InputText("Project Name",
                                                  m_ProjectNameBuffer,
                                                  sizeof(m_ProjectNameBuffer),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Apply Name") || submittedRename) {
        ApplyProjectNameChange();
    }
    ImGui::TextDisabled("Project file: %s", projectFileLabel.c_str());
    ImGui::TextWrapped("Changing the project name updates the .clayproj, .csproj, and .sln filenames together.");
    if (!m_ProjectNameStatus.empty()) {
        const ImVec4 statusColor = m_ProjectNameStatusIsError
            ? ImVec4(0.88f, 0.42f, 0.42f, 1.0f)
            : ImVec4(0.52f, 0.80f, 0.56f, 1.0f);
        ImGui::TextColored(statusColor, "%s", m_ProjectNameStatus.c_str());
    }
    ImGui::Separator();
    
    // Tab bar for different settings categories
    if (ImGui::BeginTabBar("ProjectSettingsTabs")) {
        if (ImGui::BeginTabItem("Scripting")) {
            DrawScriptingTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Modules")) {
            DrawModulesTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Physics Layers")) {
            DrawPhysicsLayersTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Game Cursor")) {
            DrawGameCursorTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Appearance")) {
            DrawAppearanceTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void ProjectSettingsPanel::DrawScriptingTab() {
    ImGui::Text("Managed Script Debugging");
    ImGui::Separator();

    bool managedDebuggingEnabled = Project::GetManagedScriptDebuggingEnabled();
    if (ImGui::Checkbox("Enable C# Breakpoint Support", &managedDebuggingEnabled)) {
        const bool previousValue = Project::GetManagedScriptDebuggingEnabled();
        Project::SetManagedScriptDebuggingEnabled(managedDebuggingEnabled);
        if (!Project::Save()) {
            Project::SetManagedScriptDebuggingEnabled(previousValue);
            Logger::LogError("[ProjectSettings] Failed to save managed script debugging setting");
        } else {
            Logger::Log(std::string("[ProjectSettings] Managed script debugging ")
                        + (managedDebuggingEnabled ? "enabled" : "disabled"));

            if (AssetPipeline::Instance().HasAnyScripts()) {
                if (AssetPipeline::Instance().ForceRebuildScripts()) {
                    Logger::Log("[ProjectSettings] Rebuilt GameScripts.dll to apply managed debugging changes");
                } else {
                    Logger::LogWarning("[ProjectSettings] Managed debugging setting was saved, but the script rebuild failed");
                }
            }
        }
    }
    ImGui::SetItemTooltip("When enabled, editor-side script compiles emit GameScripts.pdb and use debug-friendly compile settings.");

    ImGui::Spacing();
    ImGui::TextWrapped("Attach Visual Studio to ClaymoreEditor.exe with Managed (.NET) or Managed + Native debugging, then enter Play mode to hit C# breakpoints.");
    ImGui::TextWrapped("This only affects editor-side script builds for this project. Release exports continue to skip PDBs.");
    ImGui::TextWrapped("Turning this on increases script compile and hot-reload cost a bit, but it should not add meaningful per-frame overhead when you are not rebuilding scripts.");
}

void ProjectSettingsPanel::SyncProjectNameDraft() {
    const std::string& currentProjectName = Project::GetProjectName();
    if (m_ProjectNameBufferSource == currentProjectName) {
        return;
    }

    CopyStringToBuffer(currentProjectName, m_ProjectNameBuffer, sizeof(m_ProjectNameBuffer));
    m_ProjectNameBufferSource = currentProjectName;
}

void ProjectSettingsPanel::ApplyProjectNameChange() {
    const std::string previousProjectName = Project::GetProjectName();
    if (!Project::RenameProject(m_ProjectNameBuffer)) {
        m_ProjectNameStatus = "Couldn't rename the project. Use a valid file name and make sure the target .clayproj does not already exist.";
        m_ProjectNameStatusIsError = true;
        Logger::LogError("[ProjectSettings] Failed to rename project");
        return;
    }

    const std::string& updatedProjectName = Project::GetProjectName();
    m_ProjectNameBufferSource.clear();
    SyncProjectNameDraft();
    m_ProjectNameStatusIsError = false;
    if (updatedProjectName == previousProjectName) {
        m_ProjectNameStatus = "Project name is already up to date.";
    } else {
        m_ProjectNameStatus = "Project renamed and Visual Studio files were refreshed.";
        Logger::Log("[ProjectSettings] Project renamed to " + updatedProjectName);
    }
}

void ProjectSettingsPanel::DrawAppearanceTab() {
    ImGui::Text("Editor Appearance");
    ImGui::Separator();
    ImGui::TextWrapped("Choose a color scheme for this project. The selection is saved in the project file.");
    ImGui::Spacing();

    EditorColorScheme scheme = Project::GetEditorColorScheme();
    int selected = static_cast<int>(scheme);
    const char* labels[] = { "Claymore", "Solarized", "Dark", "Light", "Classic", "Medieval", "Custom" };

    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::Combo("Color Scheme", &selected, labels, IM_ARRAYSIZE(labels))) {
        EditorColorScheme newScheme = static_cast<EditorColorScheme>(selected);
        Project::SetEditorColorScheme(newScheme);

        editorui::ApplyEditorScheme(newScheme, ImGui::GetFontSize());

        Project::Save();
        Logger::Log("[ProjectSettings] Editor color scheme updated");
    }

    if (Project::GetEditorColorScheme() == EditorColorScheme::Custom) {
        EditorCustomPalette palette = Project::GetEditorCustomPalette();
        bool changed = false;

        auto editColor = [&](const char* label, ColorRGBA& value) {
            ImVec4 c = editorui::ToImVec4(value);
            if (ImGui::ColorEdit4(label, &c.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaBar)) {
                value = editorui::ToColorRGBA(c);
                changed = true;
            }
        };

        ImGui::Spacing();
        ImGui::SeparatorText("Custom Palette");
        editColor("Accent", palette.accent);
        editColor("Accent Soft", palette.accentSoft);
        editColor("Accent Muted", palette.accentMuted);
        editColor("Background 00", palette.background00);
        editColor("Background 0", palette.background0);
        editColor("Background 1", palette.background1);
        editColor("Background 2", palette.background2);
        editColor("Background 3", palette.background3);
        editColor("Text", palette.text);
        editColor("Text Dim", palette.textDim);
        editColor("Warning", palette.warning);

        if (ImGui::Button("Reset Custom Palette to Claymore Defaults")) {
            palette = EditorCustomPalette{};
            changed = true;
        }

        if (changed) {
            Project::SetEditorCustomPalette(palette);
            editorui::ApplyProjectEditorStyle(ImGui::GetFontSize());
            Project::Save();
            Logger::Log("[ProjectSettings] Custom editor palette updated");
        }
    }
}

void ProjectSettingsPanel::DrawPhysicsLayersTab() {
    auto& layerMgr = PhysicsLayers::PhysicsLayerManager::Get();
    const auto& layers = layerMgr.GetAllLayers();
    
    ImGui::Text("Physics Layers (%u / 32)", layerMgr.GetLayerCount());
    ImGui::Separator();
    ImGui::TextWrapped("Define physics layers for your project. Layers can be assigned to colliders, rigid bodies, and character controllers. Use layer masks in raycasts to filter collisions.");
    ImGui::Spacing();
    
    // Layer list
    ImGui::BeginChild("##layers_list", ImVec2(0, ImGui::GetContentRegionAvail().y - 60), true);
    for (uint32_t i = 0; i < layers.size(); ++i) {
        ImGui::PushID(i);
        
        // Show layer index and name
        char indexBuf[16];
        snprintf(indexBuf, sizeof(indexBuf), "[%2u]", i);
        ImGui::TextDisabled("%s", indexBuf);
        ImGui::SameLine();
        
        // Editable name
        static char nameBuf[64];
        strncpy(nameBuf, layers[i].c_str(), sizeof(nameBuf) - 1);
        nameBuf[sizeof(nameBuf) - 1] = '\0';
        
        ImGui::SetNextItemWidth(200);
        if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
            // TODO: Implement layer rename (requires updating all references)
            // For now, layers are immutable once created
        }
        
        ImGui::SameLine();
        ImGui::TextDisabled("Mask: 0x%08X", 1u << i);
        
        ImGui::PopID();
    }
    ImGui::EndChild();
    
    // Add new layer
    static char newLayerName[64] = "";
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##newlayer", "New layer name...", newLayerName, sizeof(newLayerName));
    ImGui::SameLine();
    bool canAdd = strlen(newLayerName) > 0 && layerMgr.GetLayerCount() < 32 && !layerMgr.HasLayer(newLayerName);
    if (!canAdd) ImGui::BeginDisabled();
    if (ImGui::Button("Add Layer")) {
        layerMgr.RegisterLayer(newLayerName);
        Project::Save(); // Save to project file
        newLayerName[0] = '\0';
    }
    if (!canAdd) ImGui::EndDisabled();
    
    if (layerMgr.GetLayerCount() >= 32) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "Max layers reached!");
    } else if (strlen(newLayerName) > 0 && layerMgr.HasLayer(newLayerName)) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "Layer already exists");
    }
}

void ProjectSettingsPanel::DrawModulesTab() {
    // Modules UI (original code)

    // Fill entire modal width: use columns with explicit size fractions
    ImGui::Columns(2, nullptr, false);
    float fullW = ImGui::GetContentRegionAvail().x;
    ImGui::SetColumnWidth(0, fullW * 0.55f);

    // Left: Registered modules list with enable/disable and remove
    // Fill remaining height to avoid nested scrollbars from arbitrary offsets
    ImVec2 leftSize(0, ImGui::GetContentRegionAvail().y);
    if (ImGui::BeginChild("##mods_list", leftSize, true)) {
        auto mods = Project::GetModules();
        for (size_t i = 0; i < mods.size(); ++i) {
            auto m = mods[i];
            ImGui::PushID((int)i);
            bool en = m.enabled;
            if (ImGui::Checkbox(m.id.c_str(), &en)) {
                Project::SetModuleEnabled(m.id, en);
                Project::Save();
                if (en) {
                    NativeAPIs n{}; ManagedAPIs man{};
                    auto* handle = ModuleLoader::LoadModule(m.dll, n, &man);
                    (void)handle;
                    if (handle) Logger::Log(std::string("[ProjectSettings] Enabled module: ") + m.id);
                    else Logger::LogError(std::string("[ProjectSettings] Enable failed for: ") + m.id);
                } else {
                    // Unload matching loaded module
                    for (auto info : ModuleLoader::GetLoaded()) {
                        if (info.id == m.id) { ModuleLoader::UnloadModule(info.handle); break; }
                    }
                    Logger::Log(std::string("[ProjectSettings] Disabled module: ") + m.id);
                    // Optional: remove descriptors from registry by prefix
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", m.dll.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) {
                // Remove from project list by re-saving without this entry
                auto current = Project::GetModules();
                std::vector<ProjectModuleRef> kept;
                kept.reserve(current.size());
                for (const auto& e : current) if (e.id != m.id) kept.push_back(e);
                // Rebuild project modules vector
                for (const auto& r : kept) Project::SetOrAddModule(r);
                // Save: SetOrAddModule appends; we need a quick rewrite – easiest path: toggle all to rebuild via Save()
                Project::Save();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::NextColumn();

    // Right: Add module (browse project Modules / engine folder)
    ImVec2 rightSize(0, ImGui::GetContentRegionAvail().y);
    if (ImGui::BeginChild("##mods_actions", rightSize, true)) {
        ImGui::Text("Add Module");
        ImGui::Separator();
        ImGui::TextWrapped("Place module DLLs in your project's 'Modules' folder or next to the engine executable.");
        ImGui::Spacing();
        try {
            std::vector<std::filesystem::path> bases;
            bases.push_back(Project::GetProjectDirectory() / "Modules");
            bases.push_back(Project::GetProjectDirectory() / "modules");
            // Also scan next to the engine executable
            wchar_t exePathW[1024]{};
            #ifdef _WIN32
            DWORD n = GetModuleFileNameW(NULL, exePathW, 1024);
            if (n > 0)
            #endif
            {
                std::filesystem::path exeDir = std::filesystem::path(exePathW).parent_path();
                bases.push_back(exeDir / "Modules");
                bases.push_back(exeDir / "modules");
            }

            // Aggregate discovered DLLs
            std::vector<std::filesystem::path> discovered;
            for (const auto& base : bases) {
                std::cout << "[ProjectSettings] Scanning for modules in: " << base << std::endl;
                if (!std::filesystem::exists(base)) {
                    std::cout << "[ProjectSettings] Directory does not exist: " << base << std::endl;
                    continue;
                }
                try {
                    for (auto it = std::filesystem::recursive_directory_iterator(base); it != std::filesystem::recursive_directory_iterator(); ++it) {
                        const auto& p = *it;
                        if (!p.is_regular_file()) continue;
                        auto ext = p.path().extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                        if (ext == ".dll") {
                            discovered.push_back(p.path());
                            std::cout << "[ProjectSettings] Found module DLL: " << p.path() << std::endl;
                        }
                    }
                } catch(...) {}
            }

            static int s_selected = -1;
            if (discovered.empty()) {
                ImGui::TextDisabled("No module DLLs found in project or engine folders.");
                ImGui::TextDisabled("Project: %s", Project::GetProjectDirectory().string().c_str());
            } else {
                ImGui::Text("Discovered DLLs:");
                // Use all remaining height for the discovery list; buttons will naturally flow after the child
                if (ImGui::BeginChild("##disc_list", ImVec2(0, ImGui::GetContentRegionAvail().y), true)) {
                    for (int i = 0; i < (int)discovered.size(); ++i) {
                        const auto& path = discovered[i];
                        std::string base = path.filename().string();
                        std::string label = base + std::string("##") + path.string();
                        bool sel = (i == s_selected);
                        if (ImGui::Selectable(label.c_str(), sel)) s_selected = i;
                        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                            ProjectModuleRef r; r.id = path.stem().string(); 
                            
                            // Only make relative if the path is under the project directory
                            std::error_code ec;
                            auto relativePath = std::filesystem::relative(path, Project::GetProjectDirectory(), ec);
                            std::cout << "[ProjectSettings] Adding module from: " << path << std::endl;
                            std::cout << "[ProjectSettings] Project directory: " << Project::GetProjectDirectory() << std::endl;
                            std::cout << "[ProjectSettings] Relative path: " << (ec ? "ERROR" : relativePath.string()) << std::endl;
                            
                            if (!ec && relativePath.string().substr(0, 2) != "..") {
                                // Path is under project directory, use relative path
                                r.dll = relativePath.string();
                                std::cout << "[ProjectSettings] Using relative path: " << r.dll << std::endl;
                            } else {
                                // Path is outside project directory (e.g., engine modules), use absolute
                                r.dll = path.string();
                                std::cout << "[ProjectSettings] Using absolute path: " << r.dll << std::endl;
                            }
                            r.enabled = true;
                            Project::SetOrAddModule(r);
                            Project::Save();
                            // Immediately load and log
                            NativeAPIs n{}; ManagedAPIs man{};
                            auto* h = ModuleLoader::LoadModule(r.dll, n, &man);
                            if (h) Logger::Log(std::string("[ProjectSettings] Added and loaded module: ") + r.id + " -> " + r.dll);
                            else Logger::LogError(std::string("[ProjectSettings] Failed to load module: ") + r.id + " -> " + r.dll);
                        }
                        if (sel) s_selected = i;
                    }
                }
                ImGui::EndChild();
                bool canAdd = (s_selected >= 0 && s_selected < (int)discovered.size());
                if (!canAdd) ImGui::BeginDisabled();
                if (ImGui::Button("Add Selected")) {
                    const auto& path = discovered[s_selected];
                    ProjectModuleRef r; r.id = path.stem().string(); 
                    
                    // Only make relative if the path is under the project directory
                    std::error_code ec;
                    auto relativePath = std::filesystem::relative(path, Project::GetProjectDirectory(), ec);
                    std::cout << "[ProjectSettings] Adding selected module from: " << path << std::endl;
                    std::cout << "[ProjectSettings] Project directory: " << Project::GetProjectDirectory() << std::endl;
                    std::cout << "[ProjectSettings] Relative path: " << (ec ? "ERROR" : relativePath.string()) << std::endl;
                    
                    if (!ec && relativePath.string().substr(0, 2) != "..") {
                        // Path is under project directory, use relative path
                        r.dll = relativePath.string();
                        std::cout << "[ProjectSettings] Using relative path: " << r.dll << std::endl;
                    } else {
                        // Path is outside project directory (e.g., engine modules), use absolute
                        r.dll = path.string();
                        std::cout << "[ProjectSettings] Using absolute path: " << r.dll << std::endl;
                    }
                    r.enabled = true;
                    Project::SetOrAddModule(r);
                    Project::Save();
                    NativeAPIs n{}; ManagedAPIs man{};
                    auto* h = ModuleLoader::LoadModule(r.dll, n, &man);
                    if (h) Logger::Log(std::string("[ProjectSettings] Added and loaded module: ") + r.id + " -> " + r.dll);
                    else Logger::LogError(std::string("[ProjectSettings] Failed to load module: ") + r.id + " -> " + r.dll);
                    s_selected = -1;
                }
                if (!canAdd) ImGui::EndDisabled();
            }
        } catch(...) {}
    }
    ImGui::EndChild();

    ImGui::Columns(1);
}

void ProjectSettingsPanel::DrawGameCursorTab() {
    namespace fs = std::filesystem;
    
    GameCursorSettings settings = Project::GetCursorSettings();
    bool cursorChanged = false;
    bool fontChanged = false;
    std::string defaultFontPath = Project::GetDefaultFontPath();
    
    ImGui::Text("Custom Game Cursor");
    ImGui::Separator();
    ImGui::TextWrapped("Set a custom cursor texture for your game. The cursor will be used in runtime builds. Enable 'Preview in Editor Play Mode' to test it in the editor.");
    ImGui::Spacing();
    
    // Default font for TextRenderer
    if (ImGui::CollapsingHeader("Default Font", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();
        ImGui::TextWrapped("Used when TextRenderer has no explicit font override. Leave unset to use the built-in default (Roboto-Regular.ttf).");
        ImGui::Spacing();
        
        // Font selection from Asset Library (registered TTF/OTF assets)
        {
            auto assets = AssetLibrary::Instance().GetAllAssets();
            std::vector<std::string> fontPaths; fontPaths.reserve(assets.size());
            for (auto& tup : assets) {
                const std::string& path = std::get<0>(tup);
                AssetType type = std::get<2>(tup);
                if (type == AssetType::Font) { fontPaths.push_back(path); continue; }
                // Back-compat: also pick up .ttf/.otf if registered before Font type existed
                std::string lower = path; std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                if (lower.size() >= 4 && (lower.rfind(".ttf") == lower.size()-4 || lower.rfind(".otf") == lower.size()-4)) {
                    fontPaths.push_back(path);
                }
            }
            
            std::string fallbackLabel = "Default (Roboto-Regular.ttf)";
            std::string currentLabel = defaultFontPath.empty()
                ? fallbackLabel
                : std::filesystem::path(defaultFontPath).filename().string();
            
            if (ImGui::BeginCombo("Font", currentLabel.c_str())) {
                bool selected = defaultFontPath.empty();
                if (ImGui::Selectable(fallbackLabel.c_str(), selected)) {
                    defaultFontPath.clear();
                    fontChanged = true;
                }
                if (selected) ImGui::SetItemDefaultFocus();
                
                for (int i = 0; i < (int)fontPaths.size(); ++i) {
                    const std::string& path = fontPaths[i];
                    bool isSelected = (path == defaultFontPath);
                    std::string label = std::filesystem::path(path).filename().string();
                    if (ImGui::Selectable(label.c_str(), isSelected)) {
                        defaultFontPath = path;
                        fontChanged = true;
                    }
                    if (isSelected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            
            if (ImGui::Button("Clear##default_font")) {
                if (!defaultFontPath.empty()) {
                    defaultFontPath.clear();
                    fontChanged = true;
                }
            }
            
            if (fontPaths.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("No fonts registered (.ttf/.otf)");
            }
        }
        
        ImGui::Unindent();
    }
    
    ImGui::Spacing();
    
    // Cursor Texture Section
    if (ImGui::CollapsingHeader("Cursor Texture", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();
        
        // Show current texture path
        if (!settings.texturePath.empty()) {
            std::string filename = fs::path(settings.texturePath).filename().string();
            ImGui::Text("Current: %s", filename.c_str());
        } else {
            ImGui::TextDisabled("No cursor texture set (using system cursor)");
        }
        
        // Texture preview and picker
        ImGui::PushID("CursorTexture");
        
        m_CursorPreviewPath = settings.texturePath;
        m_CursorPreviewTex = BGFX_INVALID_HANDLE;
        if (!settings.texturePath.empty()) {
            fs::path absPath = Project::GetAssetDirectory() / settings.texturePath;
            if (fs::exists(absPath)) {
                TextureSpecifier spec;
                spec.Path = absPath.string();
                m_CursorPreviewTex = AcquireTextureHandle(spec, TextureColorSpace::sRGB);
            }
        }
        
        // Preview button/image
        bool requestPicker = false;
        ImVec2 previewSize(64, 64);
        
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.35f, 0.35f, 0.38f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        
        if (bgfx::isValid(m_CursorPreviewTex)) {
            if (ImGui::ImageButton("##cursorPreview", TextureLoader::ToImGuiTextureID(m_CursorPreviewTex), previewSize)) {
                requestPicker = true;
            }
        } else {
            if (ImGui::Button("Select Texture##cursor", previewSize)) {
                requestPicker = true;
            }
        }
        
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        
        // Drag-drop support
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
                const char* path = static_cast<const char*>(payload->Data);
                if (path) {
                    std::string ext = fs::path(path).extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp") {
                        // Make path relative to asset directory
                        fs::path fullPath(path);
                        std::error_code ec;
                        fs::path relPath = fs::relative(fullPath, Project::GetAssetDirectory(), ec);
                        settings.texturePath = ec ? path : relPath.string();
                        cursorChanged = true;
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Clear##cursor")) {
            settings.texturePath.clear();
            cursorChanged = true;
            m_CursorPreviewTex = BGFX_INVALID_HANDLE;
            m_CursorPreviewPath.clear();
        }
        
        if (requestPicker) {
            ImGui::OpenPopup("CursorTexturePicker");
        }
        
        // Texture picker popup
        texturepicker::DrawTexturePickerPopup("CursorTexturePicker",
            [&](const std::string& selectedPath) {
                // Make path relative to asset directory
                fs::path fullPath(selectedPath);
                std::error_code ec;
                fs::path relPath = fs::relative(fullPath, Project::GetAssetDirectory(), ec);
                settings.texturePath = ec ? selectedPath : relPath.string();
                cursorChanged = true;
            },
            settings.texturePath);
        
        ImGui::PopID();
        ImGui::Unindent();
    }
    
    // Cursor Settings Section
    if (ImGui::CollapsingHeader("Cursor Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();
        
        // Hotspot
        ImGui::Text("Hotspot (click point in texture):");
        int hotspot[2] = { settings.hotspotX, settings.hotspotY };
        if (ImGui::InputInt2("X, Y##hotspot", hotspot)) {
            settings.hotspotX = std::max(0, hotspot[0]);
            settings.hotspotY = std::max(0, hotspot[1]);
            cursorChanged = true;
        }
        ImGui::SetItemTooltip("The pixel position in the cursor texture where clicks are registered (0,0 = top-left)");
        
        // Scale
        if (ImGui::SliderFloat("Base Scale", &settings.baseScale, 0.5f, 4.0f, "%.2f")) {
            cursorChanged = true;
        }
        ImGui::SetItemTooltip("Base scale multiplier for the cursor size (1.0 = native texture size at 96 DPI)");
        
        // DPI Scaling
        if (ImGui::Checkbox("Scale with DPI", &settings.useDPIScaling)) {
            cursorChanged = true;
        }
        ImGui::SetItemTooltip("When enabled, cursor scales automatically based on display DPI to maintain consistent physical size");
        
        ImGui::Spacing();
        
        // Editor Preview
        if (ImGui::Checkbox("Preview in Editor Play Mode", &settings.previewInEditor)) {
            cursorChanged = true;
        }
        ImGui::SetItemTooltip("When enabled, the custom cursor will be shown during Play mode in the editor viewport");
        
        ImGui::Unindent();
    }
    
    // Info box
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextWrapped("Tip: For best results, use a PNG with transparency. Recommended cursor size is 32x32 to 64x64 pixels at 96 DPI.");
    
    // Save changes
    if (cursorChanged || fontChanged) {
        if (cursorChanged) {
            Project::SetCursorSettings(settings);
        }
        if (fontChanged) {
            Project::SetDefaultFontPath(defaultFontPath);
            Renderer::Get().SetDefaultTextFont(defaultFontPath);
        }
        Project::Save();
        Logger::Log("[ProjectSettings] Project settings updated");
    }
}
