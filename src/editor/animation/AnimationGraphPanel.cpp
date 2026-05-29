#include "AnimationGraphPanel.h"

#include <imgui.h>
#include <imgui_clay_inspector.h>
#include <imnodes.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_set>
#include <vector>

#include "core/animation/AnimatorController.h"
#include "core/animation/AnimatorControllerIO.h"
#include "core/animation/AnimationPlayerComponent.h"
#include "core/animation/AvatarMask.h"
#include "core/ecs/Scene.h"
#include "core/ecs/EntityData.h"
#include "core/vfs/FileSystem.h"
#include "editor/Project.h"
#include "editor/ui/FileDialogs.h"
#include "editor/ui/panels/InspectorPanel.h"
#include "editor/ui/utility/AnimationAssetListCache.h"

using nlohmann::json;
using cm::animation::AnimatorController;
using cm::animation::AnimatorState;
using cm::animation::AnimatorTransition;
using cm::animation::AnimatorParameter;
using cm::animation::AnimatorCondition;
using cm::animation::ConditionMode;
using cm::animation::AnimatorParamType;
using cm::animation::AnimatorStateKind;
using cm::animation::Blend1DEntry;
using cm::animation::Blend2DEntry;

namespace fs = std::filesystem;

// ============================================================================
// Unity-style colors
// ============================================================================
namespace {
    // State node colors
    constexpr ImU32 kColorStateNormal       = IM_COL32(90, 90, 90, 255);
    constexpr ImU32 kColorStateHovered      = IM_COL32(110, 110, 110, 255);
    constexpr ImU32 kColorStateSelected     = IM_COL32(70, 130, 180, 255);
    constexpr ImU32 kColorStateDefault      = IM_COL32(230, 150, 50, 255);   // Orange for default
    constexpr ImU32 kColorStateDefaultHover = IM_COL32(250, 170, 70, 255);
    
    // Entry node (green)
    constexpr ImU32 kColorEntry             = IM_COL32(80, 160, 80, 255);
    constexpr ImU32 kColorEntryHover        = IM_COL32(100, 180, 100, 255);
    
    // Any State (cyan/teal)
    constexpr ImU32 kColorAnyState          = IM_COL32(80, 160, 160, 255);
    constexpr ImU32 kColorAnyStateHover     = IM_COL32(100, 180, 180, 255);
    
    // Blend node (purple)
    constexpr ImU32 kColorBlend             = IM_COL32(140, 100, 160, 255);
    constexpr ImU32 kColorBlendHover        = IM_COL32(160, 120, 180, 255);
    
    // Link colors
    constexpr ImU32 kColorLink              = IM_COL32(200, 200, 200, 200);
    constexpr ImU32 kColorLinkHovered       = IM_COL32(255, 255, 255, 255);
    constexpr ImU32 kColorLinkSelected      = IM_COL32(100, 180, 230, 255);
    
    // Grid background
    constexpr ImU32 kColorGridBg            = IM_COL32(40, 40, 40, 255);
    constexpr ImU32 kColorGridLines         = IM_COL32(50, 50, 50, 255);
}

// ============================================================================
// Constructor / Destructor
// ============================================================================
AnimationGraphPanel::AnimationGraphPanel() {
    m_NodeEditorContext = ImNodes::EditorContextCreate();
    m_TimelinePanel.SetContext(nullptr, nullptr);
    // Note: ImNodes style configuration is deferred to first render when context is active
}

AnimationGraphPanel::~AnimationGraphPanel() {
    if (m_NodeEditorContext) {
        ImNodes::EditorContextFree(m_NodeEditorContext);
        m_NodeEditorContext = nullptr;
    }
}

// ============================================================================
// Context Setup
// ============================================================================
void AnimationGraphPanel::SetContext(Scene* scene, EntityID* selectedEntity) {
    m_SceneContext = scene;
    m_SelectedEntityPtr = selectedEntity;
    m_TimelinePanel.SetContext(scene, selectedEntity);
}

// ============================================================================
// Main Render
// ============================================================================
void AnimationGraphPanel::OnImGuiRender() {
    if (!m_Open) {
        m_IsWindowFocusedOrHovered = false;
        return;
    }
    
    ImGui::SetNextWindowSize(ImVec2(1200, 800), ImGuiCond_FirstUseEver);
    
    // Build window title with modification indicator
    std::string windowTitle = "Animator";
    if (!m_OpenPath.empty()) {
        windowTitle += " - " + fs::path(m_OpenPath).stem().string();
    }
    if (m_Modified) {
        windowTitle += "*";
    }
    windowTitle += "###AnimationGraph";
    
    if (!ImGui::Begin(windowTitle.c_str(), &m_Open, ImGuiWindowFlags_MenuBar)) {
        m_IsWindowFocusedOrHovered = false;
        ImGui::End();
        return;
    }
    m_IsWindowFocusedOrHovered =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ||
        ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);

    DrawMenuBar();
    DrawToolbar();
    ImGui::Separator();

    if (!m_Controller) {
        // Empty state
        ImVec2 center = ImGui::GetContentRegionAvail();
        center.x = center.x * 0.5f;
        center.y = center.y * 0.5f;
        ImGui::SetCursorPos(ImVec2(center.x - 150, center.y - 30));
        ImGui::TextDisabled("No Animator Controller loaded.");
        ImGui::SetCursorPos(ImVec2(center.x - 100, center.y));
        if (ImGui::Button("New Controller", ImVec2(200, 30))) {
            auto ctrl = std::make_shared<AnimatorController>();
            ctrl->Name = "New Controller";
            // Create base layer for new controller
            cm::animation::AnimatorLayer baseLayer;
            baseLayer.Index = 0;
            baseLayer.Name = "Base Layer";
            baseLayer.DefaultState = -1;
            ctrl->Layers.push_back(std::move(baseLayer));
            m_Controller = std::move(ctrl);
            m_OpenPath.clear();
            m_Modified = true;
            EnsureIdCounters();
            ResetEditorState();
        }
        ImGui::SetCursorPos(ImVec2(center.x - 100, center.y + 40));
        if (ImGui::Button("Open Controller...", ImVec2(200, 30))) {
            std::string p = ShowOpenFileDialogExt(L"Animator Controllers (*.animctrl)", L"animctrl");
            if (!p.empty()) LoadController(p);
        }
        ImGui::End();
        return;
    }

    // Three-pane layout with resizable splitters
    const float fullWidth = ImGui::GetContentRegionAvail().x;
    const float fullHeight = ImGui::GetContentRegionAvail().y;

    // Clamp panel widths
    m_LeftPanelWidth = std::clamp(m_LeftPanelWidth, 150.0f, fullWidth - 400.0f);
    m_RightPanelWidth = std::clamp(m_RightPanelWidth, 200.0f, fullWidth - 400.0f);

    // Left panel (Parameters/Layers)
    ImGui::BeginChild("LeftPanel", ImVec2(m_LeftPanelWidth, fullHeight), true);
    DrawLayersPanel(m_LeftPanelWidth);
    ImGui::EndChild();

    // Left splitter
    ImGui::SameLine();
    ImGui::ClaySplitterConfig leftSplitter;
    leftSplitter.Vertical = true;
    leftSplitter.Thickness = m_SplitterSize;
    leftSplitter.MinPrimary = 150.0f;
    leftSplitter.MinSecondary = 200.0f;
    leftSplitter.HoverCursor = ImGuiMouseCursor_ResizeEW;
    const float leftTotal = fullWidth - m_RightPanelWidth - m_SplitterSize;
    ImGui::ClaySplitter("AG_SplitterL", &m_LeftPanelWidth, leftTotal, fullHeight, leftSplitter);

    // Center canvas
    float centerWidth = fullWidth - m_LeftPanelWidth - m_RightPanelWidth - m_SplitterSize * 2;
    if (centerWidth < 200.0f) centerWidth = 200.0f;

    ImGui::SameLine();
    ImGui::BeginChild("CenterCanvas", ImVec2(centerWidth, fullHeight), true, ImGuiWindowFlags_NoScrollbar);
    DrawGraphCanvas();
    ImGui::EndChild();

    // Right splitter
    ImGui::SameLine();
    ImGui::ClaySplitterConfig rightSplitter;
    rightSplitter.Vertical = true;
    rightSplitter.InvertAxis = true;
    rightSplitter.Thickness = m_SplitterSize;
    rightSplitter.MinPrimary = 200.0f;
    rightSplitter.MinSecondary = 200.0f;
    rightSplitter.HoverCursor = ImGuiMouseCursor_ResizeEW;
    const float rightTotal = fullWidth - m_LeftPanelWidth - m_SplitterSize;
    ImGui::ClaySplitter("AG_SplitterR", &m_RightPanelWidth, rightTotal, fullHeight, rightSplitter);

    // Right panel (Inspector)
    ImGui::SameLine();
    ImGui::BeginChild("RightPanel", ImVec2(m_RightPanelWidth, fullHeight), true);
    DrawInspectorPanel(m_RightPanelWidth);
    ImGui::EndChild();

    HandleKeyboardShortcuts();

    ImGui::End();
}

// ============================================================================
// Load / Save with VFS
// ============================================================================
bool AnimationGraphPanel::OpenControllerAsset(const std::string& path) {
    return LoadController(path);
}

bool AnimationGraphPanel::LoadController(const std::string& path) {
    if (path.empty()) return false;
    
    std::string text;
    
    // Try VFS first (handles pak files and falls back to disk)
    if (!FileSystem::Instance().ReadTextFile(path, text)) {
        // Try project-relative path
        fs::path projectDir = Project::GetProjectDirectory();
        if (!projectDir.empty()) {
            fs::path fullPath = projectDir / path;
            if (!FileSystem::Instance().ReadTextFile(fullPath.string(), text)) {
                // Last resort: direct file read
                std::ifstream in(path);
                if (!in.is_open()) {
                    std::cerr << "[AnimationGraphPanel] Failed to load controller: " << path << "\n";
                    return false;
                }
                std::stringstream ss;
                ss << in.rdbuf();
                text = ss.str();
            }
        } else {
            std::ifstream in(path);
            if (!in.is_open()) {
                std::cerr << "[AnimationGraphPanel] Failed to load controller: " << path << "\n";
                return false;
            }
            std::stringstream ss;
            ss << in.rdbuf();
            text = ss.str();
        }
    }
    
    try {
        json j = json::parse(text);
        auto ctrl = std::make_shared<AnimatorController>();
        nlohmann::from_json(j, *ctrl);
        m_Controller = std::move(ctrl);
    } catch (const std::exception& e) {
        std::cerr << "[AnimationGraphPanel] Failed to parse controller '" << path << "': " << e.what() << "\n";
        return false;
    }
    
    // Migrate legacy flat structure to layer architecture if needed
    if (!m_Controller->HasLayers()) {
        m_Controller->MigrateToLayers();
        std::cout << "[AnimationGraphPanel] Migrated legacy controller to layer architecture\n";
    }

    m_OpenPath = path;
    m_Modified = false;
    EnsureIdCounters();
    ResetEditorState();
    RestoreNodePositionsFromModel();
    
    std::cout << "[AnimationGraphPanel] Loaded controller: " << path << "\n";
    return true;
}

bool AnimationGraphPanel::SaveController(const std::string& path) {
    if (!m_Controller || path.empty()) return false;
    
    // Sync node positions to model before saving
    SyncNodePositionsToModel();
    
    // Use SaveAnimatorController which handles both JSON save and binary cache update
    bool success = cm::animation::SaveAnimatorController(*m_Controller, path);
    
    if (success) {
        m_OpenPath = path;
        m_Modified = false;
        std::cout << "[AnimationGraphPanel] Saved controller: " << path << "\n";
    } else {
        std::cerr << "[AnimationGraphPanel] Failed to save controller: " << path << "\n";
    }
    
    return success;
}

bool AnimationGraphPanel::SaveCurrent()
{
    if (!m_OpenPath.empty()) {
        return SaveController(m_OpenPath);
    }
    return SaveCurrentAsDialog();
}

bool AnimationGraphPanel::SaveCurrentAsDialog()
{
    std::string filename = m_Controller ? m_Controller->Name + ".animctrl" : "Controller.animctrl";
    std::wstring wfilename(filename.begin(), filename.end());
    const std::string path = ShowSaveFileDialogExt(wfilename.c_str(), L"Animator Controllers (*.animctrl)", L"animctrl");
    if (path.empty()) {
        return false;
    }
    return SaveController(path);
}

// ============================================================================
// Menu Bar
// ============================================================================
void AnimationGraphPanel::DrawMenuBar() {
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New", "Ctrl+N")) {
                auto ctrl = std::make_shared<AnimatorController>();
                ctrl->Name = "New Controller";
                // Create base layer for new controller
                cm::animation::AnimatorLayer baseLayer;
                baseLayer.Index = 0;
                baseLayer.Name = "Base Layer";
                baseLayer.DefaultState = -1;
                ctrl->Layers.push_back(std::move(baseLayer));
                m_Controller = std::move(ctrl);
                m_OpenPath.clear();
                m_Modified = true;
                EnsureIdCounters();
                ResetEditorState();
            }
            if (ImGui::MenuItem("Open...", "Ctrl+O")) {
                std::string p = ShowOpenFileDialogExt(L"Animator Controllers (*.animctrl)", L"animctrl");
                if (!p.empty()) LoadController(p);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save", "Ctrl+S", false, !m_OpenPath.empty())) {
                SaveController(m_OpenPath);
            }
            if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S")) {
                std::string filename = m_Controller ? m_Controller->Name + ".animctrl" : "Controller.animctrl";
                std::wstring wfilename(filename.begin(), filename.end());
                std::string p = ShowSaveFileDialogExt(wfilename.c_str(), L"Animator Controllers (*.animctrl)", L"animctrl");
                if (!p.empty()) SaveController(p);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Reload", nullptr, false, !m_OpenPath.empty())) {
                LoadController(m_OpenPath);
            }
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Delete Selected", "Delete", false, m_SelectedStateId >= 0 || m_SelectedTransitionId >= 0)) {
                if (m_SelectedStateId >= 0) {
                    DeleteStateById(m_SelectedStateId);
                    m_SelectedStateId = -1;
                } else if (m_SelectedTransitionId >= 0) {
                    DeleteTransitionById(m_SelectedTransitionId);
                    m_SelectedTransitionId = -1;
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Frame All", "F")) {
                FrameAllNodesNextFrame();
            }
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Show Entry Node", nullptr, &m_ShowEntryNode);
            ImGui::MenuItem("Show Any State Node", nullptr, &m_ShowAnyStateNode);
            ImGui::EndMenu();
        }
        
        ImGui::EndMenuBar();
    }
}

// ============================================================================
// Toolbar
// ============================================================================
void AnimationGraphPanel::DrawToolbar() {
    // Controller name
    if (m_Controller) {
        char nameBuf[128];
        std::strncpy(nameBuf, m_Controller->Name.c_str(), sizeof(nameBuf) - 1);
        nameBuf[sizeof(nameBuf) - 1] = 0;
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::InputText("##ControllerName", nameBuf, sizeof(nameBuf))) {
            m_Controller->Name = nameBuf;
            MarkModified();
        }
        ImGui::SameLine();
    }
    
    // Quick actions
    if (ImGui::Button("+ State")) {
        CreateStateAtPosition(ImVec2(200.0f, 200.0f), AnimatorStateKind::Single);
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Blend Tree 1D")) {
        CreateStateAtPosition(ImVec2(200.0f, 300.0f), AnimatorStateKind::Blend1D);
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Blend Tree 2D")) {
        CreateStateAtPosition(ImVec2(200.0f, 360.0f), AnimatorStateKind::Blend2D);
    }
    ImGui::SameLine();
    
    // Layer selector dropdown
    if (m_Controller && m_Controller->HasLayers() && !m_Controller->Layers.empty()) {
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::Text("Layer:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        const auto& currentLayer = m_Controller->Layers[m_SelectedLayerIndex];
        if (ImGui::BeginCombo("##LayerSelect", currentLayer.Name.c_str())) {
            for (size_t i = 0; i < m_Controller->Layers.size(); ++i) {
                bool selected = (m_SelectedLayerIndex == static_cast<int>(i));
                const auto& layer = m_Controller->Layers[i];
                char label[128];
                snprintf(label, sizeof(label), "%s [%s]", 
                    layer.Name.c_str(),
                    cm::animation::AvatarMask::GetPresetName(layer.MaskPreset));
                if (ImGui::Selectable(label, selected)) {
                    if (m_SelectedLayerIndex != static_cast<int>(i)) {
                        m_SelectedLayerIndex = static_cast<int>(i);
                        m_SelectedStateId = -1;
                        m_SelectedTransitionId = -1;
                        EnsureIdCounters();
                    }
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
    }
    
    // File path indicator
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (m_OpenPath.empty()) {
        ImGui::TextDisabled("<unsaved>");
    } else {
        ImGui::TextDisabled("%s", fs::path(m_OpenPath).filename().string().c_str());
    }
}

// ============================================================================
// Left Panel - Layers/Parameters
// ============================================================================
void AnimationGraphPanel::DrawLayersPanel(float width) {
    if (!m_Controller) return;
    
    // Tabs: Parameters | Layers
    if (ImGui::BeginTabBar("LeftPanelTabs")) {
        if (ImGui::BeginTabItem("Parameters")) {
            m_LeftPanelTab = 0;
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Layers")) {
            m_LeftPanelTab = 1;
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    
    ImGui::Separator();
    
    if (m_LeftPanelTab == 0) {
        DrawParameterList();
    } else {
        DrawLayersList();
    }
}

void AnimationGraphPanel::DrawLayersList() {
    if (!m_Controller) {
        ImGui::TextDisabled("No controller loaded.");
        return;
    }
    
    float contentWidth = ImGui::GetContentRegionAvail().x;
    
    // Show documentation
    ImGui::TextColored(ImVec4(0.4f, 0.7f, 0.9f, 1.0f), "Animation Layers");
    ImGui::Spacing();
    ImGui::TextWrapped("Layers allow different animations to control different body parts. Each layer has its own state machine.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    
    // Migrate legacy controller to layers if needed
    if (!m_Controller->HasLayers() && (!GetCurrentStates().empty() || m_Controller->DefaultState >= 0)) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Legacy controller detected");
        if (ImGui::Button("Migrate to Layer System", ImVec2(contentWidth, 0))) {
            m_Controller->MigrateToLayers();
            MarkModified();
        }
        ImGui::TextDisabled("This will move the current state machine");
        ImGui::TextDisabled("into 'Base Layer' and enable multi-layer support.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }
    
    // Add Layer button
    if (ImGui::Button("+ Add Layer", ImVec2(contentWidth, 0))) {
        cm::animation::AnimatorLayer newLayer;
        newLayer.Index = static_cast<int>(m_Controller->Layers.size());
        newLayer.Name = "Layer " + std::to_string(newLayer.Index);
        newLayer.DefaultState = -1;
        newLayer.MaskPreset = cm::animation::BodyMaskPreset::FullBody;
        newLayer.BlendMode = cm::animation::AnimatorLayerBlendMode::Override;
        newLayer.DefaultWeight = 0.0f; // Start disabled
        m_Controller->Layers.push_back(newLayer);
        MarkModified();
    }
    
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    
    // Layer list
    if (m_Controller->Layers.empty()) {
        ImGui::TextDisabled("No layers defined.");
        ImGui::TextDisabled("Click '+ Add Layer' to create one.");
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 0.7f, 0.9f, 1.0f), "Body Masks");
        ImGui::Spacing();
        ImGui::BulletText("FullBody - All bones");
        ImGui::BulletText("UpperBody - Spine, arms, head");
        ImGui::BulletText("LowerBody - Hips and legs");
        ImGui::BulletText("LeftArm / RightArm");
        ImGui::BulletText("Head - Neck, head, eyes");
    } else {
        int deleteIndex = -1;
        for (size_t i = 0; i < m_Controller->Layers.size(); ++i) {
            auto& layer = m_Controller->Layers[i];
            ImGui::PushID(static_cast<int>(i));
            
            // Layer header - selected layer is highlighted
            bool isSelected = (m_SelectedLayerIndex == static_cast<int>(i));
            bool isBaseLayer = (layer.Index == 0);
            ImVec4 headerColor;
            if (isSelected) {
                headerColor = ImVec4(0.3f, 0.5f, 0.7f, 1.0f);  // Blue for selected
            } else if (isBaseLayer) {
                headerColor = ImVec4(0.2f, 0.4f, 0.2f, 1.0f);  // Green for base
            } else {
                headerColor = ImVec4(0.3f, 0.3f, 0.4f, 1.0f);  // Blue-ish for overlays
            }
            ImGui::PushStyleColor(ImGuiCol_Header, headerColor);
            
            char headerLabel[128];
            snprintf(headerLabel, sizeof(headerLabel), "%s%s [%s]###layer%d", 
                isSelected ? "> " : "",
                layer.Name.c_str(),
                cm::animation::AvatarMask::GetPresetName(layer.MaskPreset),
                layer.Index);
            
            bool expanded = ImGui::CollapsingHeader(headerLabel, 
                isBaseLayer ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None);
            ImGui::PopStyleColor();
            
            // Double-click on header to select this layer's graph
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                if (m_SelectedLayerIndex != static_cast<int>(i)) {
                    m_SelectedLayerIndex = static_cast<int>(i);
                    m_SelectedStateId = -1;      // Clear selection when switching layers
                    m_SelectedTransitionId = -1;
                    EnsureIdCounters();          // Recalculate ID counters for new layer
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Double-click to view this layer's graph");
            }
            
            // Default weight indicator bar on the right
            ImVec2 headerMin = ImGui::GetItemRectMin();
            ImVec2 headerMax = ImGui::GetItemRectMax();
            float barWidth = 50.0f;
            float barHeight = 4.0f;
            ImVec2 barPos(headerMax.x - barWidth - 30.0f, (headerMin.y + headerMax.y) / 2.0f - barHeight / 2.0f);
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(barPos, ImVec2(barPos.x + barWidth, barPos.y + barHeight), 
                IM_COL32(60, 60, 60, 255), 2.0f);
            draw->AddRectFilled(barPos, ImVec2(barPos.x + barWidth * layer.DefaultWeight, barPos.y + barHeight), 
                isBaseLayer ? IM_COL32(100, 180, 100, 255) : IM_COL32(100, 100, 180, 255), 2.0f);
            
            if (expanded) {
                ImGui::Indent();
                
                // Name
                char nameBuf[128];
                strncpy(nameBuf, layer.Name.c_str(), sizeof(nameBuf));
                nameBuf[sizeof(nameBuf) - 1] = 0;
                ImGui::SetNextItemWidth(contentWidth - 40);
                if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) {
                    layer.Name = nameBuf;
                    MarkModified();
                }
                
                // Body Mask (not editable for base layer)
                const char* maskNames[] = {
                    "Full Body", "Upper Body", "Lower Body", 
                    "Left Arm", "Right Arm", "Head", "Spine",
                    "Left Hand", "Right Hand", "Both Arms", "Legs", "Custom"
                };
                int maskIdx = static_cast<int>(layer.MaskPreset);
                if (maskIdx > 11) maskIdx = 11; // Custom
                
                if (isBaseLayer) {
                    ImGui::BeginDisabled();
                    ImGui::SetNextItemWidth(contentWidth - 40);
                    ImGui::Combo("Body Mask", &maskIdx, maskNames, IM_ARRAYSIZE(maskNames));
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Base layer always affects all bones.");
                    }
                } else {
                    ImGui::SetNextItemWidth(contentWidth - 40);
                    if (ImGui::Combo("Body Mask", &maskIdx, maskNames, IM_ARRAYSIZE(maskNames))) {
                        if (maskIdx <= 10) {
                            layer.MaskPreset = static_cast<cm::animation::BodyMaskPreset>(maskIdx);
                            MarkModified();
                        }
                    }
                }
                
                // Blend Mode
                const char* blendModes[] = { "Override", "Additive" };
                int blendMode = static_cast<int>(layer.BlendMode);
                ImGui::SetNextItemWidth(contentWidth - 40);
                if (ImGui::Combo("Blend Mode", &blendMode, blendModes, IM_ARRAYSIZE(blendModes))) {
                    layer.BlendMode = static_cast<cm::animation::AnimatorLayerBlendMode>(blendMode);
                    MarkModified();
                }
                
                // Default Weight
                float defaultWeight = layer.DefaultWeight;
                ImGui::SetNextItemWidth(contentWidth - 40);
                if (ImGui::SliderFloat("Default Weight", &defaultWeight, 0.0f, 1.0f)) {
                    layer.DefaultWeight = defaultWeight;
                    MarkModified();
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Weight when animation starts.\n0 = disabled, 1 = fully active.\nScripts can change weight at runtime.");
                }
                
                // State count info
                ImGui::TextDisabled("States: %d, Transitions: %d", 
                    static_cast<int>(layer.States.size()), 
                    static_cast<int>(layer.Transitions.size()));
                
                // Default state (entry state)
                if (!layer.States.empty() || !isBaseLayer) {
                    std::vector<const char*> stateNames;
                    std::vector<int> stateIds;
                    int currentIdx = -1;
                    
                    // For non-base layers, add "None" option at the beginning
                    if (!isBaseLayer) {
                        stateNames.push_back("(None)");
                        stateIds.push_back(-1);
                        if (layer.DefaultState < 0) {
                            currentIdx = 0;
                        }
                    }
                    
                    for (size_t s = 0; s < layer.States.size(); ++s) {
                        stateNames.push_back(layer.States[s].Name.c_str());
                        stateIds.push_back(layer.States[s].Id);
                        if (layer.States[s].Id == layer.DefaultState) {
                            currentIdx = static_cast<int>(stateNames.size() - 1);
                        }
                    }
                    
                    // Fallback if currentIdx wasn't set
                    if (currentIdx < 0) currentIdx = 0;
                    
                    ImGui::SetNextItemWidth(contentWidth - 40);
                    if (ImGui::Combo("Default State", &currentIdx, stateNames.data(), static_cast<int>(stateNames.size()))) {
                        layer.DefaultState = stateIds[currentIdx];
                        MarkModified();
                    }
                    if (!isBaseLayer) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(?)");
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("Non-base layers can have no entry state.\nSet to (None) to transition only via script/triggers.");
                        }
                    }
                }
                
                // Actions
                ImGui::Spacing();
                if (!isBaseLayer) {
                    if (ImGui::Button("Delete Layer", ImVec2(contentWidth - 20, 0))) {
                        deleteIndex = static_cast<int>(i);
                    }
                } else {
                    ImGui::BeginDisabled();
                    ImGui::Button("Delete Layer", ImVec2(contentWidth - 20, 0));
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::TextDisabled("(base layer)");
                }
                
                ImGui::Unindent();
            }
            
            ImGui::PopID();
        }
        
        // Handle deletion
        if (deleteIndex > 0 && deleteIndex < static_cast<int>(m_Controller->Layers.size())) {
            m_Controller->Layers.erase(m_Controller->Layers.begin() + deleteIndex);
            // Re-index layers
            for (size_t i = 0; i < m_Controller->Layers.size(); ++i) {
                m_Controller->Layers[i].Index = static_cast<int>(i);
            }
            MarkModified();
        }
    }
}

void AnimationGraphPanel::DrawParameterList() {
    if (!m_Controller) return;
    
    float contentWidth = ImGui::GetContentRegionAvail().x;
    
    // Add parameter button
    if (ImGui::Button("+ Add Parameter", ImVec2(contentWidth, 0))) {
        AnimatorParameter param;
        param.Name = "NewParam" + std::to_string(m_Controller->Parameters.size());
        param.Type = AnimatorParamType::Float;
        m_Controller->Parameters.push_back(param);
        MarkModified();
    }
    
    ImGui::Separator();
    
    // Parameter list
    int deleteIndex = -1;
    for (size_t i = 0; i < m_Controller->Parameters.size(); ++i) {
        auto& p = m_Controller->Parameters[i];
        ImGui::PushID(static_cast<int>(i));
        
        // Type icon
        const char* typeIcon = "?";
        switch (p.Type) {
            case AnimatorParamType::Float: typeIcon = "F"; break;
            case AnimatorParamType::Int: typeIcon = "I"; break;
            case AnimatorParamType::Bool: typeIcon = "B"; break;
            case AnimatorParamType::Trigger: typeIcon = "T"; break;
        }
        
        ImGui::TextDisabled("[%s]", typeIcon);
        ImGui::SameLine();
        
        // Name input
        char nameBuf[128];
        std::strncpy(nameBuf, p.Name.c_str(), sizeof(nameBuf) - 1);
        nameBuf[sizeof(nameBuf) - 1] = 0;
        ImGui::SetNextItemWidth(contentWidth - 80);
        if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf))) {
            p.Name = nameBuf;
            MarkModified();
        }
        
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) {
            deleteIndex = static_cast<int>(i);
        }
        
        // Type dropdown - order must match AnimatorParamType enum: Bool=0, Int=1, Float=2, Trigger=3
        const char* types[] = { "Bool", "Int", "Float", "Trigger" };
        int t = static_cast<int>(p.Type);
        ImGui::SetNextItemWidth(contentWidth - 20);
        if (ImGui::Combo("##type", &t, types, IM_ARRAYSIZE(types))) {
            p.Type = static_cast<AnimatorParamType>(t);
            MarkModified();
        }
        
        // Default value based on type
        ImGui::SetNextItemWidth(contentWidth - 20);
        switch (p.Type) {
            case AnimatorParamType::Float:
                if (ImGui::DragFloat("##defaultFloat", &p.DefaultFloat, 0.01f)) {
                    MarkModified();
                }
                break;
            case AnimatorParamType::Int:
                if (ImGui::DragInt("##defaultInt", &p.DefaultInt)) {
                    MarkModified();
                }
                break;
            case AnimatorParamType::Bool:
                if (ImGui::Checkbox("##defaultBool", &p.DefaultBool)) {
                    MarkModified();
                }
                break;
            case AnimatorParamType::Trigger:
                ImGui::TextDisabled("(trigger)");
                break;
        }
        
        ImGui::Separator();
        ImGui::PopID();
    }
    
    // Handle deletion
    if (deleteIndex >= 0) {
        RemoveConditionsReferencingParam(m_Controller->Parameters[deleteIndex].Name);
        m_Controller->Parameters.erase(m_Controller->Parameters.begin() + deleteIndex);
        MarkModified();
    }
}

// ============================================================================
// Graph Canvas
// ============================================================================
void AnimationGraphPanel::DrawGraphCanvas() {
    if (!m_Controller || !m_NodeEditorContext) return;
    
    ImNodes::EditorContextSet(m_NodeEditorContext);
    
    // Configure imnodes style for this context (deferred from constructor for safety)
    if (!m_NodeStyleConfigured) {
        ImNodesStyle& style = ImNodes::GetStyle();
        style.NodeCornerRounding = 4.0f;
        style.NodePadding = ImVec2(8.0f, 4.0f);
        style.NodeBorderThickness = 1.0f;
        style.LinkThickness = 2.5f;
        style.LinkLineSegmentsPerLength = 0.1f;
        style.PinCircleRadius = 4.0f;
        style.PinQuadSideLength = 7.0f;
        style.PinTriangleSideLength = 9.0f;
        style.PinLineThickness = 1.5f;
        style.PinHoverRadius = 10.0f;
        style.PinOffset = 0.0f;
        style.MiniMapPadding = ImVec2(8.0f, 8.0f);
        style.MiniMapOffset = ImVec2(4.0f, 4.0f);
        style.Flags = ImNodesStyleFlags_GridLines | ImNodesStyleFlags_GridSnapping;
        style.GridSpacing = 24.0f;
        
        // Colors
        style.Colors[ImNodesCol_NodeBackground] = kColorStateNormal;
        style.Colors[ImNodesCol_NodeBackgroundHovered] = kColorStateHovered;
        style.Colors[ImNodesCol_NodeBackgroundSelected] = kColorStateSelected;
        style.Colors[ImNodesCol_NodeOutline] = IM_COL32(60, 60, 60, 255);
        style.Colors[ImNodesCol_TitleBar] = kColorStateNormal;
        style.Colors[ImNodesCol_TitleBarHovered] = kColorStateHovered;
        style.Colors[ImNodesCol_TitleBarSelected] = kColorStateSelected;
        style.Colors[ImNodesCol_Link] = kColorLink;
        style.Colors[ImNodesCol_LinkHovered] = kColorLinkHovered;
        style.Colors[ImNodesCol_LinkSelected] = kColorLinkSelected;
        style.Colors[ImNodesCol_Pin] = IM_COL32(150, 150, 150, 255);
        style.Colors[ImNodesCol_PinHovered] = IM_COL32(200, 200, 200, 255);
        style.Colors[ImNodesCol_BoxSelector] = IM_COL32(100, 150, 200, 100);
        style.Colors[ImNodesCol_BoxSelectorOutline] = IM_COL32(100, 150, 200, 200);
        style.Colors[ImNodesCol_GridBackground] = kColorGridBg;
        style.Colors[ImNodesCol_GridLine] = kColorGridLines;
        style.Colors[ImNodesCol_GridLinePrimary] = IM_COL32(70, 70, 70, 255);
        style.Colors[ImNodesCol_MiniMapBackground] = IM_COL32(30, 30, 30, 200);
        style.Colors[ImNodesCol_MiniMapBackgroundHovered] = IM_COL32(40, 40, 40, 200);
        style.Colors[ImNodesCol_MiniMapOutline] = IM_COL32(100, 100, 100, 200);
        style.Colors[ImNodesCol_MiniMapOutlineHovered] = IM_COL32(150, 150, 150, 200);
        style.Colors[ImNodesCol_MiniMapNodeBackground] = IM_COL32(80, 80, 80, 200);
        style.Colors[ImNodesCol_MiniMapNodeBackgroundHovered] = IM_COL32(100, 100, 100, 200);
        style.Colors[ImNodesCol_MiniMapNodeBackgroundSelected] = IM_COL32(100, 150, 200, 200);
        style.Colors[ImNodesCol_MiniMapNodeOutline] = IM_COL32(100, 100, 100, 200);
        style.Colors[ImNodesCol_MiniMapLink] = IM_COL32(150, 150, 150, 200);
        style.Colors[ImNodesCol_MiniMapLinkSelected] = IM_COL32(100, 150, 200, 200);
        style.Colors[ImNodesCol_MiniMapCanvas] = IM_COL32(60, 60, 60, 200);
        style.Colors[ImNodesCol_MiniMapCanvasOutline] = IM_COL32(100, 100, 100, 200);
        m_NodeStyleConfigured = true;
    }
    
    ImNodes::BeginNodeEditor();
    
    ApplyPendingNodePlacement();
    
    // -------------------------------------------------------------------------
    // Draw Entry Node (special green node)
    // -------------------------------------------------------------------------
    if (m_ShowEntryNode) {
        ImNodes::PushColorStyle(ImNodesCol_TitleBar, kColorEntry);
        ImNodes::PushColorStyle(ImNodesCol_TitleBarHovered, kColorEntryHover);
        ImNodes::PushColorStyle(ImNodesCol_TitleBarSelected, kColorEntry);
        ImNodes::PushColorStyle(ImNodesCol_NodeBackground, kColorEntry);
        ImNodes::PushColorStyle(ImNodesCol_NodeBackgroundHovered, kColorEntryHover);
        ImNodes::PushColorStyle(ImNodesCol_NodeBackgroundSelected, kColorEntry);
        
        // Position entry node BEFORE BeginNode (required by ImNodes)
        if (!m_PositionSeed.count(kEntryNodeId)) {
            ImNodes::SetNodeGridSpacePos(kEntryNodeId, m_EntryNodePos);
            m_PositionSeed.insert(kEntryNodeId);
        }
        
        ImNodes::BeginNode(kEntryNodeId);
        ImNodes::BeginNodeTitleBar();
        ImGui::TextUnformatted("Entry");
        ImNodes::EndNodeTitleBar();
        
        // Entry node only has output (no input - it's the start point)
        // Note: Entry doesn't support manual link creation, it auto-connects to default state
        ImNodes::BeginOutputAttribute(MakeOutputAttr(kEntryNodeId));
        ImGui::TextUnformatted(" ");
        ImNodes::EndOutputAttribute();
        
        ImNodes::EndNode();
        
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();
        
        // Note: Entry->Default link is drawn by DrawTransitionArrows(), not ImNodes::Link
    }
    
    // -------------------------------------------------------------------------
    // Draw Any State Node (special cyan node)
    // -------------------------------------------------------------------------
    if (m_ShowAnyStateNode) {
        ImNodes::PushColorStyle(ImNodesCol_TitleBar, kColorAnyState);
        ImNodes::PushColorStyle(ImNodesCol_TitleBarHovered, kColorAnyStateHover);
        ImNodes::PushColorStyle(ImNodesCol_TitleBarSelected, kColorAnyState);
        ImNodes::PushColorStyle(ImNodesCol_NodeBackground, kColorAnyState);
        ImNodes::PushColorStyle(ImNodesCol_NodeBackgroundHovered, kColorAnyStateHover);
        ImNodes::PushColorStyle(ImNodesCol_NodeBackgroundSelected, kColorAnyState);
        
        // Position any state node BEFORE BeginNode (required by ImNodes)
        if (!m_PositionSeed.count(kAnyStateNodeId)) {
            ImNodes::SetNodeGridSpacePos(kAnyStateNodeId, m_AnyStateNodePos);
            m_PositionSeed.insert(kAnyStateNodeId);
        }
        
        ImNodes::BeginNode(kAnyStateNodeId);
        ImNodes::BeginNodeTitleBar();
        ImGui::TextUnformatted("Any State");
        ImNodes::EndNodeTitleBar();
        
        // Any State only has output - drag from here to create transitions that can fire from any state
        ImNodes::BeginOutputAttribute(MakeOutputAttr(kAnyStateNodeId));
        ImGui::TextUnformatted(" ");
        ImNodes::EndOutputAttribute();
        
        ImNodes::EndNode();
        
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();
    }
    
    // -------------------------------------------------------------------------
    // Draw State Nodes
    // -------------------------------------------------------------------------
    for (auto& s : GetCurrentStates()) {
        bool isDefault = (GetCurrentDefaultState() == s.Id);
        bool isBlend = (s.Kind == AnimatorStateKind::Blend1D || s.Kind == AnimatorStateKind::Blend2D);
        bool isSelected = (m_SelectedStateId == s.Id);
        
        // Choose color based on state type
        ImU32 titleColor = kColorStateNormal;
        ImU32 titleColorHover = kColorStateHovered;
        if (isDefault) {
            titleColor = kColorStateDefault;
            titleColorHover = kColorStateDefaultHover;
        } else if (isBlend) {
            titleColor = kColorBlend;
            titleColorHover = kColorBlendHover;
        }
        if (isSelected) {
            titleColor = kColorStateSelected;
        }
        
        // Position node BEFORE BeginNode (required by ImNodes)
        if (!m_PositionSeed.count(s.Id)) {
            ImNodes::SetNodeGridSpacePos(s.Id, ImVec2(s.EditorPosX, s.EditorPosY));
            m_PositionSeed.insert(s.Id);
        }
        
        ImNodes::PushColorStyle(ImNodesCol_TitleBar, titleColor);
        ImNodes::PushColorStyle(ImNodesCol_TitleBarHovered, titleColorHover);
        ImNodes::PushColorStyle(ImNodesCol_TitleBarSelected, titleColor);
        
        ImNodes::BeginNode(s.Id);
        
        // Title bar
        ImNodes::BeginNodeTitleBar();
        if (isDefault) {
            ImGui::TextUnformatted("\xE2\x9E\xA4 "); // Unicode arrow
            ImGui::SameLine();
        }
        ImGui::TextUnformatted(s.Name.c_str());
        ImNodes::EndNodeTitleBar();
        
        // Show clip name or blend info
        if (isBlend) {
            ImGui::TextDisabled(s.Kind == AnimatorStateKind::Blend2D ? "Blend Tree (2D)" : "Blend Tree (1D)");
            if (s.Kind == AnimatorStateKind::Blend2D) {
                if (!s.Blend2DParamX.empty() || !s.Blend2DParamY.empty()) {
                    ImGui::TextDisabled("X: %s", s.Blend2DParamX.empty() ? "<none>" : s.Blend2DParamX.c_str());
                    ImGui::TextDisabled("Y: %s", s.Blend2DParamY.empty() ? "<none>" : s.Blend2DParamY.c_str());
                }
            } else if (!s.Blend1DParam.empty()) {
                ImGui::TextDisabled("Param: %s", s.Blend1DParam.c_str());
            }
        } else {
            std::string clipDisplay = s.AnimationAssetPath.empty() ? s.ClipPath : s.AnimationAssetPath;
            if (clipDisplay.empty()) {
                ImGui::TextDisabled("(no clip)");
            } else {
                std::string clipName = fs::path(clipDisplay).stem().string();
                ImGui::TextDisabled("%s", clipName.c_str());
            }
        }
        
        // Input/Output pins
        ImNodes::BeginInputAttribute(MakeInputAttr(s.Id));
        ImGui::TextUnformatted(" ");
        ImNodes::EndInputAttribute();
        
        ImNodes::BeginOutputAttribute(MakeOutputAttr(s.Id));
        ImGui::TextUnformatted(" ");
        ImNodes::EndOutputAttribute();
        
        ImNodes::EndNode();
        
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();
    }
    
    // -------------------------------------------------------------------------
    // Draw Transitions as Direct Arrows (custom drawing for cleaner look)
    // Must be done BEFORE EndNodeEditor while still in the canvas context
    // -------------------------------------------------------------------------
    DrawTransitionArrows();
    
    ImNodes::EndNodeEditor();
    
    // -------------------------------------------------------------------------
    // Handle transition click detection (must be AFTER EndNodeEditor)
    // ImNodes functions like IsNodeHovered require being outside editor scope
    // -------------------------------------------------------------------------
    if (ImNodes::IsEditorHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        int hoveredNode = -1;
        bool nodeHovered = ImNodes::IsNodeHovered(&hoveredNode);
        if (!nodeHovered) {
            ImVec2 mousePos = ImGui::GetMousePos();
            int hitTransition = GetTransitionAtPoint(mousePos);
            if (hitTransition >= 0) {
                m_SelectedTransitionId = hitTransition;
                m_SelectedStateId = -1;
                ImNodes::ClearNodeSelection();
            }
        }
    }
    // Handle transition right-click for context menu
    if (ImNodes::IsEditorHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        int hoveredNode = -1;
        bool nodeHovered = ImNodes::IsNodeHovered(&hoveredNode);
        if (!nodeHovered) {
            ImVec2 mousePos = ImGui::GetMousePos();
            int hitTransition = GetTransitionAtPoint(mousePos);
            if (hitTransition >= 0) {
                m_ContextTransitionId = hitTransition;
                ImGui::OpenPopup("TransitionContextMenu");
            }
        }
    }
    
    // -------------------------------------------------------------------------
    // Handle link creation
    // -------------------------------------------------------------------------
    int startAttr = 0, endAttr = 0;
    if (ImNodes::IsLinkCreated(&startAttr, &endAttr)) {
        int node1 = AttrToNode(startAttr);
        int node2 = AttrToNode(endAttr);
        bool attr1IsOutput = IsOutputAttr(startAttr);
        bool attr2IsOutput = IsOutputAttr(endAttr);
        
        // Determine direction: from output pin to input pin
        // If user dragged backwards (input to output), swap them
        int fromNode, toNode;
        if (attr1IsOutput && !attr2IsOutput) {
            fromNode = node1;
            toNode = node2;
        } else if (!attr1IsOutput && attr2IsOutput) {
            fromNode = node2;
            toNode = node1;
        } else {
            // Both same type - invalid (output to output or input to input)
            fromNode = toNode = -1;
        }
        
        // Validate the link
        bool validLink = (fromNode != toNode) && 
                         (fromNode >= 0 || fromNode == kAnyStateNodeId) &&
                         (toNode >= 0) &&
                         (toNode != kEntryNodeId) &&     // Can't link TO Entry
                         (toNode != kAnyStateNodeId) &&  // Can't link TO Any State
                         (fromNode != kEntryNodeId);     // Entry doesn't create manual transitions
        
        if (validLink) {
            // Map Any State to FromState = -1 for storage
            int actualFrom = (fromNode == kAnyStateNodeId) ? -1 : fromNode;
            
            // Check if this transition already exists
            bool exists = false;
            for (const auto& t : GetCurrentTransitions()) {
                if (t.FromState == actualFrom && t.ToState == toNode) {
                    exists = true;
                    break;
                }
            }
            
            if (!exists) {
                AnimatorTransition tr;
                tr.FromState = actualFrom;
                tr.ToState = toNode;
                tr.Id = m_NextTransitionId++;
                tr.Duration = 0.25f; // Default blend duration
                GetCurrentTransitions().push_back(tr);
                MarkModified();
            }
        }
    }
    
    // -------------------------------------------------------------------------
    // Handle node selection (via ImNodes)
    // Transition selection is handled before EndNodeEditor
    // -------------------------------------------------------------------------
    int numSelectedNodes = ImNodes::NumSelectedNodes();
    if (numSelectedNodes == 1) {
        int selected;
        ImNodes::GetSelectedNodes(&selected);
        if (selected == kAnyStateNodeId) {
            // Any State selected - use special marker
            m_SelectedStateId = kAnyStateNodeId;
            m_SelectedTransitionId = -1;
        } else if (selected != kEntryNodeId) {
            m_SelectedStateId = selected;
            m_SelectedTransitionId = -1;
        } else {
            m_SelectedStateId = -1;
        }
    }
    
    // -------------------------------------------------------------------------
    // Context menus
    // -------------------------------------------------------------------------
    // Right-click on canvas (transition context menu is handled before EndNodeEditor)
    if (ImNodes::IsEditorHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        m_PendingSpawnPos = ImGui::GetMousePos();
        m_HasSpawnPos = true;
        
        // Check if clicking on a node
        int hoveredNode = -1;
        if (ImNodes::IsNodeHovered(&hoveredNode)) {
            if (hoveredNode == kEntryNodeId) {
                // Entry node right-clicked - open entry context menu
                ImGui::OpenPopup("EntryContextMenu");
            } else if (hoveredNode >= 0) { // Regular state node
                m_ContextStateId = hoveredNode;
                ImGui::OpenPopup("StateContextMenu");
            }
        } else {
            // Only open canvas menu if not clicking on a transition
            // (transition context menu is opened before EndNodeEditor)
            if (m_ContextTransitionId < 0) {
                ImGui::OpenPopup("CanvasContextMenu");
            }
        }
    }
    // Reset context transition ID after handling
    // (it's set before EndNodeEditor if right-clicking a transition)
    
    DrawCanvasContextMenu();
    DrawStateContextMenu();
    DrawTransitionContextMenu();
    DrawEntryContextMenu();
    
    // Handle drag-drop for animation files
    HandleDragDrop();
}

// ============================================================================
// Context Menus
// ============================================================================
void AnimationGraphPanel::DrawCanvasContextMenu() {
    if (ImGui::BeginPopup("CanvasContextMenu")) {
        if (ImGui::BeginMenu("Create State")) {
            if (ImGui::MenuItem("Empty")) {
                CreateStateAtPosition(m_PendingSpawnPos, AnimatorStateKind::Single);
            }
            if (ImGui::MenuItem("From Animation Clip...")) {
                std::string clipPath = ShowOpenFileDialogExt(L"Animation Clips (*.anim)", L"anim");
                if (!clipPath.empty()) {
                    AnimatorState s;
                    s.Id = m_NextStateId++;
                    s.Name = fs::path(clipPath).stem().string();
                    s.AnimationAssetPath = MakeControllerRelativePath(clipPath);
                    s.Kind = AnimatorStateKind::Single;
                    GetCurrentStates().push_back(s);
                    m_PendingNewNodeId = s.Id;
                    m_PendingNewNodePos = m_PendingSpawnPos;
                    if (GetCurrentDefaultState() < 0) {
                        GetCurrentDefaultState() = s.Id;
                    }
                    MarkModified();
                }
            }
            ImGui::EndMenu();
        }
        
        if (ImGui::MenuItem("Create Blend Tree (1D)")) {
            CreateStateAtPosition(m_PendingSpawnPos, AnimatorStateKind::Blend1D);
        }
        if (ImGui::MenuItem("Create Blend Tree (2D)")) {
            CreateStateAtPosition(m_PendingSpawnPos, AnimatorStateKind::Blend2D);
        }
        
        ImGui::Separator();
        
        if (ImGui::MenuItem("Frame All")) {
            FrameAllNodesNextFrame();
        }
        
        ImGui::EndPopup();
    }
    m_HasSpawnPos = false;
}

void AnimationGraphPanel::DrawStateContextMenu() {
    if (ImGui::BeginPopup("StateContextMenu")) {
        auto* state = FindState(m_ContextStateId);
        if (state) {
            ImGui::Text("%s", state->Name.c_str());
            ImGui::Separator();
            
            bool isDefault = (GetCurrentDefaultState() == state->Id);
            if (ImGui::MenuItem("Set as Default", nullptr, isDefault)) {
                GetCurrentDefaultState() = state->Id;
                MarkModified();
            }
            
            if (ImGui::MenuItem("Create Transition to...")) {
                // TODO: Enable link creation mode
            }
            
            ImGui::Separator();
            
            if (ImGui::MenuItem("Delete", "Delete")) {
                DeleteStateById(m_ContextStateId);
                if (m_SelectedStateId == m_ContextStateId) {
                    m_SelectedStateId = -1;
                }
            }
        }
        m_ContextStateId = -1;
        ImGui::EndPopup();
    }
}

void AnimationGraphPanel::DrawTransitionContextMenu() {
    if (ImGui::BeginPopup("TransitionContextMenu")) {
        auto* trans = FindTransition(m_ContextTransitionId);
        if (trans) {
            auto* from = FindState(trans->FromState);
            auto* to = FindState(trans->ToState);
            ImGui::Text("%s -> %s", 
                from ? from->Name.c_str() : "Any State",
                to ? to->Name.c_str() : "?");
            ImGui::Separator();
            
            if (ImGui::MenuItem("Delete", "Delete")) {
                DeleteTransitionById(m_ContextTransitionId);
                if (m_SelectedTransitionId == m_ContextTransitionId) {
                    m_SelectedTransitionId = -1;
                }
            }
        }
        m_ContextTransitionId = -1;
        ImGui::EndPopup();
    }
}

void AnimationGraphPanel::DrawEntryContextMenu() {
    if (ImGui::BeginPopup("EntryContextMenu")) {
        ImGui::Text("Entry");
        ImGui::Separator();
        
        // Check if this is not the base layer (index > 0)
        bool isNonBaseLayer = m_SelectedLayerIndex > 0;
        
        if (isNonBaseLayer) {
            bool hasDefaultState = GetCurrentDefaultState() >= 0;
            if (hasDefaultState) {
                if (ImGui::MenuItem("Clear Entry")) {
                    GetCurrentDefaultState() = -1;
                    MarkModified();
                }
            } else {
                ImGui::TextDisabled("Entry is cleared");
            }
            ImGui::Separator();
            ImGui::TextDisabled("Non-base layers can have no entry state");
        } else {
            ImGui::TextDisabled("Base layer requires an entry state");
        }
        
        ImGui::EndPopup();
    }
}

// ============================================================================
// Inspector Panel (Right Side)
// ============================================================================
void AnimationGraphPanel::DrawInspectorPanel(float width) {
    if (!m_Controller) return;
    
    if (m_SelectedStateId == kAnyStateNodeId) {
        DrawAnyStateProperties();
    } else if (m_SelectedStateId >= 0) {
        if (auto* state = FindState(m_SelectedStateId)) {
            DrawStateProperties(state);
        }
    } else if (m_SelectedTransitionId >= 0) {
        if (auto* trans = FindTransition(m_SelectedTransitionId)) {
            DrawTransitionProperties(trans);
        }
    } else {
        ImGui::TextDisabled("Select a state or transition");
        ImGui::Separator();
        
        // Controller info
        ImGui::Text("Controller: %s", m_Controller->Name.c_str());
        if (auto* layer = GetCurrentLayer()) {
            ImGui::Text("Layer: %s", layer->Name.c_str());
        }
        ImGui::Text("States: %zu", GetCurrentStates().size());
        ImGui::Text("Transitions: %zu", GetCurrentTransitions().size());
        ImGui::Text("Parameters: %zu", m_Controller->Parameters.size());
    }
}

void AnimationGraphPanel::DrawStateProperties(AnimatorState* state) {
    if (!state) return;
    
    float contentWidth = ImGui::GetContentRegionAvail().x;
    
    ImGui::Text("State");
    ImGui::Separator();
    
    // Name
    char nameBuf[128];
    std::strncpy(nameBuf, state->Name.c_str(), sizeof(nameBuf) - 1);
    nameBuf[sizeof(nameBuf) - 1] = 0;
    ImGui::SetNextItemWidth(contentWidth);
    if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) {
        state->Name = nameBuf;
        MarkModified();
    }
    
    // Default state indicator
    bool isDefault = (GetCurrentDefaultState() == state->Id);
    if (isDefault) {
        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "Default Entry State");
    } else {
        if (ImGui::Button("Set as Default", ImVec2(contentWidth, 0))) {
            GetCurrentDefaultState() = state->Id;
            MarkModified();
        }
    }
    
    ImGui::Separator();
    
    // Motion settings
    ImGui::Text("Motion");
    
    if (state->Kind == AnimatorStateKind::Single) {
        // Animation clip picker
        ImGui::Text("Animation Clip:");
        std::string& clipPath = state->AnimationAssetPath.empty() ? state->ClipPath : state->AnimationAssetPath;
        DrawAnimationClipPicker(clipPath);
        
        // Speed
        ImGui::SetNextItemWidth(contentWidth);
        if (ImGui::DragFloat("Speed", &state->Speed, 0.01f, 0.0f, 10.0f, "%.2f")) {
            MarkModified();
        }
        
        // Loop
        if (ImGui::Checkbox("Loop", &state->Loop)) {
            MarkModified();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Enable for continuous animations (idle, walk).\nDisable for one-shot actions (attack, jump) when using Exit Time transitions.");
        }
    } else if (state->Kind == AnimatorStateKind::Blend1D) {
        // Blend1D settings
        ImGui::Text("Blend Tree (1D)");
        
        // Parameter selector
        if (ImGui::BeginCombo("Parameter", state->Blend1DParam.empty() ? "<None>" : state->Blend1DParam.c_str())) {
            for (const auto& p : m_Controller->Parameters) {
                if (p.Type == AnimatorParamType::Float) {
                    bool selected = (p.Name == state->Blend1DParam);
                    if (ImGui::Selectable(p.Name.c_str(), selected)) {
                        state->Blend1DParam = p.Name;
                        MarkModified();
                    }
                }
            }
            ImGui::EndCombo();
        }
        
        ImGui::Separator();
        ImGui::Text("Blend Entries:");
        
        // Blend entries list
        int deleteEntry = -1;
        for (size_t i = 0; i < state->Blend1DEntries.size(); ++i) {
            auto& entry = state->Blend1DEntries[i];
            ImGui::PushID(static_cast<int>(i));
            
            ImGui::SetNextItemWidth(60);
            if (ImGui::DragFloat("##key", &entry.Key, 0.01f, 0.0f, 1.0f, "%.2f")) {
                MarkModified();
            }
            ImGui::SameLine();
            
            std::string clipName = entry.AssetPath.empty() ? 
                (entry.ClipPath.empty() ? "(none)" : fs::path(entry.ClipPath).stem().string()) :
                fs::path(entry.AssetPath).stem().string();
            
            ImGui::SetNextItemWidth(contentWidth - 100);
            if (ImGui::Button(clipName.c_str(), ImVec2(contentWidth - 110, 0))) {
                std::string p = ShowOpenFileDialogExt(L"Animation Clips (*.anim)", L"anim");
                if (!p.empty()) {
                    entry.AssetPath = MakeControllerRelativePath(p);
                    MarkModified();
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) {
                deleteEntry = static_cast<int>(i);
            }
            
            ImGui::PopID();
        }
        
        if (deleteEntry >= 0) {
            state->Blend1DEntries.erase(state->Blend1DEntries.begin() + deleteEntry);
            MarkModified();
        }
        
        if (ImGui::Button("+ Add Entry", ImVec2(contentWidth, 0))) {
            Blend1DEntry entry;
            entry.Key = state->Blend1DEntries.empty() ? 0.0f : 
                state->Blend1DEntries.back().Key + 0.25f;
            state->Blend1DEntries.push_back(entry);
            MarkModified();
        }
    } else if (state->Kind == AnimatorStateKind::Blend2D) {
        DrawBlend2DStateProperties(state, contentWidth);
    }

    // Per-state Y bypass: keep vertical placement continuous with surrounding states.
    if (ImGui::Checkbox("Bypass Y Motion", &state->BypassYMotion)) {
        MarkModified();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Ignore this state's contribution to runtime Y placement.\n"
            "Useful for hit-react/upper-body one-shots that should not sink or lift the character."
        );
    }
    
    ImGui::Separator();
    
    // Body mask override (for base layer states, e.g., locomotion excluding upper body)
    if (ImGui::CollapsingHeader("Body Mask Filter")) {
        ImGui::TextDisabled("Check body parts this state should animate.\nUnchecked = inherit from layer (all checked = full body).");
        
        uint8_t& bits = state->StateMaskBits;
        bool modified = false;
        
        // Quick preset buttons
        if (ImGui::SmallButton("All")) { bits = AnimatorState::MaskBits_FullBody; modified = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton("None")) { bits = 0; modified = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton("Lower")) { bits = AnimatorState::MaskBits_LowerBody; modified = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton("Upper")) { bits = AnimatorState::MaskBits_UpperBody; modified = true; }
        
        ImGui::Separator();
        
        // Individual checkboxes in two columns
        ImGui::Columns(2, nullptr, false);
        
        // Left column
        bool hips = (bits & AnimatorState::MaskBit_Hips) != 0;
        if (ImGui::Checkbox("Hips", &hips)) {
            bits = hips ? (bits | AnimatorState::MaskBit_Hips) : (bits & ~AnimatorState::MaskBit_Hips);
            modified = true;
        }
        
        bool spine = (bits & AnimatorState::MaskBit_Spine) != 0;
        if (ImGui::Checkbox("Spine/Chest", &spine)) {
            bits = spine ? (bits | AnimatorState::MaskBit_Spine) : (bits & ~AnimatorState::MaskBit_Spine);
            modified = true;
        }
        
        bool head = (bits & AnimatorState::MaskBit_Head) != 0;
        if (ImGui::Checkbox("Head/Neck", &head)) {
            bits = head ? (bits | AnimatorState::MaskBit_Head) : (bits & ~AnimatorState::MaskBit_Head);
            modified = true;
        }
        
        bool leftArm = (bits & AnimatorState::MaskBit_LeftArm) != 0;
        if (ImGui::Checkbox("Left Arm", &leftArm)) {
            bits = leftArm ? (bits | AnimatorState::MaskBit_LeftArm) : (bits & ~AnimatorState::MaskBit_LeftArm);
            modified = true;
        }
        
        // Right column
        ImGui::NextColumn();
        
        bool rightArm = (bits & AnimatorState::MaskBit_RightArm) != 0;
        if (ImGui::Checkbox("Right Arm", &rightArm)) {
            bits = rightArm ? (bits | AnimatorState::MaskBit_RightArm) : (bits & ~AnimatorState::MaskBit_RightArm);
            modified = true;
        }
        
        bool leftLeg = (bits & AnimatorState::MaskBit_LeftLeg) != 0;
        if (ImGui::Checkbox("Left Leg", &leftLeg)) {
            bits = leftLeg ? (bits | AnimatorState::MaskBit_LeftLeg) : (bits & ~AnimatorState::MaskBit_LeftLeg);
            modified = true;
        }
        
        bool rightLeg = (bits & AnimatorState::MaskBit_RightLeg) != 0;
        if (ImGui::Checkbox("Right Leg", &rightLeg)) {
            bits = rightLeg ? (bits | AnimatorState::MaskBit_RightLeg) : (bits & ~AnimatorState::MaskBit_RightLeg);
            modified = true;
        }
        
        ImGui::Columns(1);
        
        if (modified) MarkModified();
        
        // Show current mask summary
        if (bits == 0) {
            ImGui::TextDisabled("No override (uses layer mask)");
        } else if (bits == AnimatorState::MaskBits_FullBody) {
            ImGui::TextDisabled("Full body");
        } else if (bits == AnimatorState::MaskBits_LowerBody) {
            ImGui::TextDisabled("Lower body only (hips + legs)");
        } else if (bits == AnimatorState::MaskBits_UpperBody) {
            ImGui::TextDisabled("Upper body only");
        } else {
            ImGui::TextDisabled("Custom mask");
        }
    }
    
    ImGui::Separator();
    
    // Transitions FROM this state (outgoing) - expandable
    if (ImGui::CollapsingHeader("Outgoing Transitions", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool hasOutgoing = false;
        int deleteTransitionId = -1;
        
        for (auto& t : GetCurrentTransitions()) {
            if (t.FromState == state->Id) {
                hasOutgoing = true;
                auto* to = FindState(t.ToState);
                ImGui::PushID(t.Id);
                
                // Tree node for each transition
                std::string label = "-> " + (to ? to->Name : "(deleted)");
                bool nodeOpen = ImGui::TreeNodeEx("##trans", ImGuiTreeNodeFlags_AllowOverlap);
                ImGui::SameLine();
                ImGui::Text("%s", label.c_str());
                
                // Delete button on same line
                ImGui::SameLine(contentWidth - 25);
                if (ImGui::SmallButton("X")) {
                    deleteTransitionId = t.Id;
                }
                
                if (nodeOpen) {
                    ImGui::Indent();
                    float innerWidth = ImGui::GetContentRegionAvail().x;
                    
                    // Exit Time - use table for cleaner layout
                    if (ImGui::Checkbox("Has Exit Time", &t.HasExitTime)) {
                        MarkModified();
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Wait until animation reaches Exit Time %% before transitioning");
                    }
                    if (t.HasExitTime) {
                        ImGui::Text("Exit at:");
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(innerWidth - 100);
                        // Use 0-100 range for display, convert back to 0-1 for storage
                        float exitPercent = t.ExitTime * 100.0f;
                        if (ImGui::SliderFloat("##exitTime", &exitPercent, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
                            t.ExitTime = exitPercent / 100.0f;
                            MarkModified();
                        }
                    }
                    
                    // Blend Duration
                    ImGui::Text("Blend:");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(innerWidth - 100);
                    if (ImGui::DragFloat("##duration", &t.Duration, 0.01f, 0.0f, 5.0f, "%.2f s")) {
                        MarkModified();
                    }
                    
                    // Conditions
                    ImGui::Text("Conditions:");
                    if (t.Conditions.empty()) {
                        ImGui::TextDisabled("  (none - always transitions)");
                    }
                    
                    int deleteCondIdx = -1;
                    for (size_t ci = 0; ci < t.Conditions.size(); ++ci) {
                        auto& cond = t.Conditions[ci];
                        ImGui::PushID(static_cast<int>(ci));
                        
                        // Parameter dropdown
                        ImGui::SetNextItemWidth(80);
                        if (ImGui::BeginCombo("##param", cond.Parameter.empty() ? "<None>" : cond.Parameter.c_str())) {
                            for (const auto& p : m_Controller->Parameters) {
                                if (ImGui::Selectable(p.Name.c_str(), p.Name == cond.Parameter)) {
                                    cond.Parameter = p.Name;
                                    MarkModified();
                                }
                            }
                            ImGui::EndCombo();
                        }
                        
                        ImGui::SameLine();
                        
                        // Find parameter type
                        AnimatorParamType paramType = AnimatorParamType::Float;
                        for (const auto& p : m_Controller->Parameters) {
                            if (p.Name == cond.Parameter) {
                                paramType = p.Type;
                                break;
                            }
                        }
                        
                        // Condition value based on type
                        if (paramType == AnimatorParamType::Bool) {
                            // Fix invalid modes (e.g., Greater/Less from old data) - treat them as IfNot (false)
                            if (cond.Mode != ConditionMode::If && cond.Mode != ConditionMode::IfNot) {
                                cond.Mode = ConditionMode::IfNot;
                                MarkModified();
                            }
                            bool isTrue = (cond.Mode == ConditionMode::If);
                            if (ImGui::Checkbox("##boolVal", &isTrue)) {
                                cond.Mode = isTrue ? ConditionMode::If : ConditionMode::IfNot;
                                MarkModified();
                            }
                            ImGui::SameLine();
                            ImGui::TextDisabled(isTrue ? "= true" : "= false");
                        } else if (paramType == AnimatorParamType::Trigger) {
                            ImGui::TextDisabled("(trigger)");
                        } else {
                            // Float/Int comparison
                            const char* modes[] = { ">", "<", "==", "!=" };
                            int modeIdx = 0;
                            switch (cond.Mode) {
                                case ConditionMode::Greater: modeIdx = 0; break;
                                case ConditionMode::Less: modeIdx = 1; break;
                                case ConditionMode::Equals: modeIdx = 2; break;
                                case ConditionMode::NotEquals: modeIdx = 3; break;
                                default: cond.Mode = ConditionMode::Greater; break;
                            }
                            ImGui::SetNextItemWidth(45);
                            if (ImGui::Combo("##mode", &modeIdx, modes, 4)) {
                                switch (modeIdx) {
                                    case 0: cond.Mode = ConditionMode::Greater; break;
                                    case 1: cond.Mode = ConditionMode::Less; break;
                                    case 2: cond.Mode = ConditionMode::Equals; break;
                                    case 3: cond.Mode = ConditionMode::NotEquals; break;
                                }
                                MarkModified();
                            }
                            ImGui::SameLine();
                            ImGui::SetNextItemWidth(50);
                            if (paramType == AnimatorParamType::Float) {
                                if (ImGui::DragFloat("##thresh", &cond.Threshold, 0.01f)) {
                                    MarkModified();
                                }
                            } else {
                                if (ImGui::DragInt("##ithresh", &cond.IntThreshold)) {
                                    MarkModified();
                                }
                            }
                        }
                        
                        ImGui::SameLine();
                        if (ImGui::SmallButton("X##cond")) {
                            deleteCondIdx = static_cast<int>(ci);
                        }
                        
                        ImGui::PopID();
                    }
                    
                    if (deleteCondIdx >= 0) {
                        t.Conditions.erase(t.Conditions.begin() + deleteCondIdx);
                        MarkModified();
                    }
                    
                    // Add condition button
                    if (ImGui::SmallButton("+ Condition")) {
                        AnimatorCondition cond;
                        if (!m_Controller->Parameters.empty()) {
                            cond.Parameter = m_Controller->Parameters.front().Name;
                            // Set correct Mode based on parameter type
                            for (const auto& p : m_Controller->Parameters) {
                                if (p.Name == cond.Parameter) {
                                    if (p.Type == AnimatorParamType::Trigger) cond.Mode = ConditionMode::Trigger;
                                    else if (p.Type == AnimatorParamType::Bool) cond.Mode = ConditionMode::If;
                                    else cond.Mode = ConditionMode::Greater;
                                    break;
                                }
                            }
                        }
                        t.Conditions.push_back(cond);
                        MarkModified();
                    }
                    
                    ImGui::Unindent();
                    ImGui::TreePop();
                }
                
                ImGui::PopID();
            }
        }
        
        if (deleteTransitionId >= 0) {
            DeleteTransitionById(deleteTransitionId);
        }
        
        if (!hasOutgoing) {
            ImGui::TextDisabled("  (none)");
        }
        
        // Button to create new transition
        if (ImGui::Button("+ Add Transition...", ImVec2(contentWidth, 0))) {
            ImGui::OpenPopup("AddTransitionPopup");
        }
        
        // Popup to select destination state
        if (ImGui::BeginPopup("AddTransitionPopup")) {
            ImGui::Text("Transition to:");
            ImGui::Separator();
            for (const auto& s : GetCurrentStates()) {
                if (s.Id != state->Id) {
                    if (ImGui::Selectable(s.Name.c_str())) {
                        bool exists = false;
                        for (const auto& t : GetCurrentTransitions()) {
                            if (t.FromState == state->Id && t.ToState == s.Id) {
                                exists = true;
                                break;
                            }
                        }
                        if (!exists) {
                            AnimatorTransition tr;
                            tr.FromState = state->Id;
                            tr.ToState = s.Id;
                            tr.Id = m_NextTransitionId++;
                            tr.Duration = 0.25f;
                            GetCurrentTransitions().push_back(tr);
                            MarkModified();
                        }
                    }
                }
            }
            ImGui::EndPopup();
        }
    }
    
    // Incoming transitions - fully editable
    if (ImGui::CollapsingHeader("Incoming Transitions")) {
        bool hasIncoming = false;
        int deleteTransitionId = -1;
        
        for (auto& t : GetCurrentTransitions()) {
            if (t.ToState == state->Id) {
                hasIncoming = true;
                auto* from = FindState(t.FromState);
                std::string srcName = from ? from->Name : "Any State";
                
                ImGui::PushID(t.Id + 50000); // Offset to avoid collision with outgoing
                
                // Tree node for each transition
                std::string label = srcName + " ->";
                bool nodeOpen = ImGui::TreeNodeEx("##intrans", ImGuiTreeNodeFlags_AllowOverlap);
                ImGui::SameLine();
                ImGui::Text("%s", label.c_str());
                
                // Delete button on same line
                ImGui::SameLine(contentWidth - 25);
                if (ImGui::SmallButton("X")) {
                    deleteTransitionId = t.Id;
                }
                
                if (nodeOpen) {
                    ImGui::Indent();
                    float innerWidth = ImGui::GetContentRegionAvail().x;
                    
                    // Exit Time - use table for cleaner layout
                    if (ImGui::Checkbox("Has Exit Time", &t.HasExitTime)) {
                        MarkModified();
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Wait until animation reaches Exit Time %% before transitioning");
                    }
                    if (t.HasExitTime) {
                        ImGui::Text("Exit at:");
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(innerWidth - 100);
                        // Use 0-100 range for display, convert back to 0-1 for storage
                        float exitPercent = t.ExitTime * 100.0f;
                        if (ImGui::SliderFloat("##exitTime", &exitPercent, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
                            t.ExitTime = exitPercent / 100.0f;
                            MarkModified();
                        }
                    }
                    
                    // Blend Duration
                    ImGui::Text("Blend:");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(innerWidth - 100);
                    if (ImGui::DragFloat("##duration", &t.Duration, 0.01f, 0.0f, 5.0f, "%.2f s")) {
                        MarkModified();
                    }
                    
                    // Conditions
                    ImGui::Text("Conditions:");
                    if (t.Conditions.empty()) {
                        if (t.FromState == -1) { // Any State
                            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "  WARNING: No conditions!");
                            ImGui::TextDisabled("  (will transition immediately)");
                        } else {
                            ImGui::TextDisabled("  (none - always transitions)");
                        }
                    }
                    
                    int deleteCondIdx = -1;
                    for (size_t ci = 0; ci < t.Conditions.size(); ++ci) {
                        auto& cond = t.Conditions[ci];
                        ImGui::PushID(static_cast<int>(ci));
                        
                        // Parameter dropdown
                        ImGui::SetNextItemWidth(80);
                        if (ImGui::BeginCombo("##param", cond.Parameter.empty() ? "<None>" : cond.Parameter.c_str())) {
                            for (const auto& p : m_Controller->Parameters) {
                                if (ImGui::Selectable(p.Name.c_str(), p.Name == cond.Parameter)) {
                                    cond.Parameter = p.Name;
                                    MarkModified();
                                }
                            }
                            ImGui::EndCombo();
                        }
                        
                        ImGui::SameLine();
                        
                        // Find parameter type
                        AnimatorParamType paramType = AnimatorParamType::Float;
                        for (const auto& p : m_Controller->Parameters) {
                            if (p.Name == cond.Parameter) {
                                paramType = p.Type;
                                break;
                            }
                        }
                        
                        // Condition value based on type
                        if (paramType == AnimatorParamType::Bool) {
                            // Fix invalid modes (e.g., Greater/Less from old data) - treat them as IfNot (false)
                            if (cond.Mode != ConditionMode::If && cond.Mode != ConditionMode::IfNot) {
                                cond.Mode = ConditionMode::IfNot;
                                MarkModified();
                            }
                            bool isTrue = (cond.Mode == ConditionMode::If);
                            if (ImGui::Checkbox("##boolVal", &isTrue)) {
                                cond.Mode = isTrue ? ConditionMode::If : ConditionMode::IfNot;
                                MarkModified();
                            }
                            ImGui::SameLine();
                            ImGui::TextDisabled(isTrue ? "= true" : "= false");
                        } else if (paramType == AnimatorParamType::Trigger) {
                            ImGui::TextDisabled("(trigger)");
                        } else {
                            // Float/Int comparison
                            const char* modes[] = { ">", "<", "==", "!=" };
                            int modeIdx = 0;
                            switch (cond.Mode) {
                                case ConditionMode::Greater: modeIdx = 0; break;
                                case ConditionMode::Less: modeIdx = 1; break;
                                case ConditionMode::Equals: modeIdx = 2; break;
                                case ConditionMode::NotEquals: modeIdx = 3; break;
                                default: cond.Mode = ConditionMode::Greater; break;
                            }
                            ImGui::SetNextItemWidth(45);
                            if (ImGui::Combo("##mode", &modeIdx, modes, 4)) {
                                switch (modeIdx) {
                                    case 0: cond.Mode = ConditionMode::Greater; break;
                                    case 1: cond.Mode = ConditionMode::Less; break;
                                    case 2: cond.Mode = ConditionMode::Equals; break;
                                    case 3: cond.Mode = ConditionMode::NotEquals; break;
                                }
                                MarkModified();
                            }
                            ImGui::SameLine();
                            ImGui::SetNextItemWidth(50);
                            if (paramType == AnimatorParamType::Float) {
                                if (ImGui::DragFloat("##thresh", &cond.Threshold, 0.01f)) {
                                    MarkModified();
                                }
                            } else {
                                if (ImGui::DragInt("##ithresh", &cond.IntThreshold)) {
                                    MarkModified();
                                }
                            }
                        }
                        
                        ImGui::SameLine();
                        if (ImGui::SmallButton("X##cond")) {
                            deleteCondIdx = static_cast<int>(ci);
                        }
                        
                        ImGui::PopID();
                    }
                    
                    if (deleteCondIdx >= 0) {
                        t.Conditions.erase(t.Conditions.begin() + deleteCondIdx);
                        MarkModified();
                    }
                    
                    // Add condition button
                    if (ImGui::SmallButton("+ Condition")) {
                        AnimatorCondition cond;
                        if (!m_Controller->Parameters.empty()) {
                            cond.Parameter = m_Controller->Parameters.front().Name;
                            // Set correct Mode based on parameter type
                            for (const auto& p : m_Controller->Parameters) {
                                if (p.Name == cond.Parameter) {
                                    if (p.Type == AnimatorParamType::Trigger) cond.Mode = ConditionMode::Trigger;
                                    else if (p.Type == AnimatorParamType::Bool) cond.Mode = ConditionMode::If;
                                    else cond.Mode = ConditionMode::Greater;
                                    break;
                                }
                            }
                        }
                        t.Conditions.push_back(cond);
                        MarkModified();
                    }
                    
                    ImGui::Unindent();
                    ImGui::TreePop();
                }
                
                ImGui::PopID();
            }
        }
        
        if (deleteTransitionId >= 0) {
            DeleteTransitionById(deleteTransitionId);
        }
        
        if (!hasIncoming) {
            ImGui::TextDisabled("  (none)");
        }
    }
}

void AnimationGraphPanel::DrawBlend2DStateProperties(AnimatorState* state, float contentWidth) {
    if (!state || !m_Controller) return;

    ImGui::Text("Blend Tree (2D)");
    ImGui::TextDisabled("Click to add/select points, drag to reposition, right-click a point to remove.");

    auto drawParamSelector = [&](const char* label, std::string& selectedParam) {
        if (ImGui::BeginCombo(label, selectedParam.empty() ? "<None>" : selectedParam.c_str())) {
            for (const auto& p : m_Controller->Parameters) {
                if (p.Type != AnimatorParamType::Float) {
                    continue;
                }
                bool selected = (p.Name == selectedParam);
                if (ImGui::Selectable(p.Name.c_str(), selected)) {
                    selectedParam = p.Name;
                    MarkModified();
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
    };

    drawParamSelector("Parameter X", state->Blend2DParamX);
    drawParamSelector("Parameter Y", state->Blend2DParamY);

    ImGui::Separator();

    const ImVec2 canvasSize(contentWidth, 220.0f);
    const ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##Blend2DCanvas", canvasSize);
    const bool canvasHovered = ImGui::IsItemHovered();
    const bool canvasActive = ImGui::IsItemActive();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 bgColor = IM_COL32(32, 32, 36, 255);
    const ImU32 borderColor = IM_COL32(90, 90, 100, 255);
    const ImU32 gridColor = IM_COL32(60, 60, 70, 255);
    const ImU32 axisColor = IM_COL32(115, 115, 130, 255);
    drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), bgColor, 4.0f);
    drawList->AddRect(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), borderColor, 4.0f);

    for (int i = 1; i < 4; ++i) {
        float tx = static_cast<float>(i) / 4.0f;
        float ty = static_cast<float>(i) / 4.0f;
        float x = canvasPos.x + tx * canvasSize.x;
        float y = canvasPos.y + ty * canvasSize.y;
        drawList->AddLine(ImVec2(x, canvasPos.y), ImVec2(x, canvasPos.y + canvasSize.y), gridColor);
        drawList->AddLine(ImVec2(canvasPos.x, y), ImVec2(canvasPos.x + canvasSize.x, y), gridColor);
    }

    const ImVec2 center(canvasPos.x + canvasSize.x * 0.5f, canvasPos.y + canvasSize.y * 0.5f);
    drawList->AddLine(ImVec2(center.x, canvasPos.y), ImVec2(center.x, canvasPos.y + canvasSize.y), axisColor, 1.5f);
    drawList->AddLine(ImVec2(canvasPos.x, center.y), ImVec2(canvasPos.x + canvasSize.x, center.y), axisColor, 1.5f);

    auto toCanvas = [&](float x, float y) -> ImVec2 {
        const float nx = std::clamp((x + 1.0f) * 0.5f, 0.0f, 1.0f);
        const float ny = std::clamp((1.0f - (y + 1.0f) * 0.5f), 0.0f, 1.0f);
        return ImVec2(canvasPos.x + nx * canvasSize.x, canvasPos.y + ny * canvasSize.y);
    };
    auto fromCanvas = [&](const ImVec2& p, float& x, float& y) {
        const float nx = std::clamp((p.x - canvasPos.x) / std::max(1.0f, canvasSize.x), 0.0f, 1.0f);
        const float ny = std::clamp((p.y - canvasPos.y) / std::max(1.0f, canvasSize.y), 0.0f, 1.0f);
        x = std::clamp(nx * 2.0f - 1.0f, -1.0f, 1.0f);
        y = std::clamp((1.0f - ny) * 2.0f - 1.0f, -1.0f, 1.0f);
    };

    int selectedIndex = -1;
    auto selIt = m_BlendEntrySelection.find(state->Id);
    if (selIt != m_BlendEntrySelection.end()) {
        selectedIndex = selIt->second;
    }
    if (selectedIndex >= static_cast<int>(state->Blend2DEntries.size())) {
        selectedIndex = -1;
        m_BlendEntrySelection.erase(state->Id);
    }

    int hoveredIndex = -1;
    float hoveredDistSq = 100.0f;
    const ImVec2 mousePos = ImGui::GetIO().MousePos;
    for (size_t i = 0; i < state->Blend2DEntries.size(); ++i) {
        const auto p = toCanvas(state->Blend2DEntries[i].X, state->Blend2DEntries[i].Y);
        const float dx = mousePos.x - p.x;
        const float dy = mousePos.y - p.y;
        const float d2 = dx * dx + dy * dy;
        if (d2 <= hoveredDistSq) {
            hoveredDistSq = d2;
            hoveredIndex = static_cast<int>(i);
        }
    }

    if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hoveredIndex >= 0) {
            m_BlendEntrySelection[state->Id] = hoveredIndex;
            selectedIndex = hoveredIndex;
        } else {
            Blend2DEntry entry;
            fromCanvas(mousePos, entry.X, entry.Y);
            state->Blend2DEntries.push_back(entry);
            selectedIndex = static_cast<int>(state->Blend2DEntries.size()) - 1;
            m_BlendEntrySelection[state->Id] = selectedIndex;
            MarkModified();
        }
    }

    if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hoveredIndex >= 0) {
        state->Blend2DEntries.erase(state->Blend2DEntries.begin() + hoveredIndex);
        if (selectedIndex == hoveredIndex) {
            selectedIndex = -1;
            m_BlendEntrySelection.erase(state->Id);
        } else if (selectedIndex > hoveredIndex) {
            selectedIndex--;
            m_BlendEntrySelection[state->Id] = selectedIndex;
        }
        MarkModified();
    }

    if (canvasActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left) &&
        selectedIndex >= 0 && selectedIndex < static_cast<int>(state->Blend2DEntries.size())) {
        float x = state->Blend2DEntries[selectedIndex].X;
        float y = state->Blend2DEntries[selectedIndex].Y;
        fromCanvas(mousePos, x, y);
        if (std::abs(x - state->Blend2DEntries[selectedIndex].X) > 1e-6f ||
            std::abs(y - state->Blend2DEntries[selectedIndex].Y) > 1e-6f) {
            state->Blend2DEntries[selectedIndex].X = x;
            state->Blend2DEntries[selectedIndex].Y = y;
            MarkModified();
        }
    }

    for (size_t i = 0; i < state->Blend2DEntries.size(); ++i) {
        const bool selected = (selectedIndex == static_cast<int>(i));
        const bool hovered = (hoveredIndex == static_cast<int>(i));
        const auto p = toCanvas(state->Blend2DEntries[i].X, state->Blend2DEntries[i].Y);
        const ImU32 pointColor = selected
            ? IM_COL32(80, 180, 255, 255)
            : (hovered ? IM_COL32(245, 220, 120, 255) : IM_COL32(220, 220, 220, 255));
        drawList->AddCircleFilled(p, selected ? 6.0f : 5.0f, pointColor);

        char coord[64];
        std::snprintf(coord, sizeof(coord), "(%.2f, %.2f)", state->Blend2DEntries[i].X, state->Blend2DEntries[i].Y);
        drawList->AddText(ImVec2(p.x + 8.0f, p.y - 8.0f), IM_COL32(210, 210, 210, 220), coord);
    }

    ImGui::Spacing();
    if (canvasHovered) {
        float mx = 0.0f;
        float my = 0.0f;
        fromCanvas(mousePos, mx, my);
        ImGui::TextDisabled("Cursor: (%.2f, %.2f)", mx, my);
    } else {
        ImGui::TextDisabled("Blend space range: X[-1..1], Y[-1..1]");
    }

    if (ImGui::Button("+ Add Point", ImVec2(contentWidth, 0))) {
        Blend2DEntry entry;
        state->Blend2DEntries.push_back(entry);
        selectedIndex = static_cast<int>(state->Blend2DEntries.size()) - 1;
        m_BlendEntrySelection[state->Id] = selectedIndex;
        MarkModified();
    }

    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(state->Blend2DEntries.size())) {
        auto& selected = state->Blend2DEntries[selectedIndex];
        ImGui::Separator();
        ImGui::Text("Selected Point #%d", selectedIndex);

        ImGui::SetNextItemWidth(contentWidth);
        if (ImGui::DragFloat("Point X", &selected.X, 0.01f, -1.0f, 1.0f, "%.2f")) {
            MarkModified();
        }
        ImGui::SetNextItemWidth(contentWidth);
        if (ImGui::DragFloat("Point Y", &selected.Y, 0.01f, -1.0f, 1.0f, "%.2f")) {
            MarkModified();
        }

        ImGui::Text("Animation Clip:");
        std::string selectedClip = !selected.AssetPath.empty() ? selected.AssetPath : selected.ClipPath;
        DrawAnimationClipPicker(selectedClip);
        const std::string existingClip = !selected.AssetPath.empty() ? selected.AssetPath : selected.ClipPath;
        if (selectedClip != existingClip) {
            selected.AssetPath = selectedClip;
            selected.ClipPath.clear();
            MarkModified();
        }

        if (ImGui::Button("Remove Selected Point", ImVec2(contentWidth, 0))) {
            state->Blend2DEntries.erase(state->Blend2DEntries.begin() + selectedIndex);
            m_BlendEntrySelection.erase(state->Id);
            MarkModified();
        }
    }
}

void AnimationGraphPanel::DrawAnyStateProperties() {
    if (!m_Controller) return;
    
    float contentWidth = ImGui::GetContentRegionAvail().x;
    
    ImGui::Text("Any State");
    ImGui::Separator();
    
    ImGui::TextWrapped("Transitions from Any State can fire from any other state. Use conditions to control when they trigger.");
    
    ImGui::Separator();
    
    // Transitions FROM Any State (outgoing) - expandable
    ImGui::Text("Transitions");
    
    int deleteTransitionId = -1;
    bool hasTransitions = false;
    
    for (auto& t : GetCurrentTransitions()) {
        if (t.FromState == -1) { // Any State transitions have FromState = -1
            hasTransitions = true;
            auto* to = FindState(t.ToState);
            ImGui::PushID(t.Id);
            
            // Tree node for each transition
            std::string label = "-> " + (to ? to->Name : "(deleted)");
            bool nodeOpen = ImGui::TreeNodeEx("##trans", ImGuiTreeNodeFlags_AllowOverlap);
            ImGui::SameLine();
            ImGui::Text("%s", label.c_str());
            
            // Delete button on same line
            ImGui::SameLine(contentWidth - 25);
            if (ImGui::SmallButton("X")) {
                deleteTransitionId = t.Id;
            }
            
            if (nodeOpen) {
                ImGui::Indent();
                float innerWidth = ImGui::GetContentRegionAvail().x;
                
                // Exit Time - use table for cleaner layout
                if (ImGui::Checkbox("Has Exit Time", &t.HasExitTime)) {
                    MarkModified();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Wait until animation reaches Exit Time %% before transitioning");
                }
                if (t.HasExitTime) {
                    ImGui::Text("Exit at:");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(innerWidth - 100);
                    // Use 0-100 range for display, convert back to 0-1 for storage
                    float exitPercent = t.ExitTime * 100.0f;
                    if (ImGui::SliderFloat("##exitTime", &exitPercent, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
                        t.ExitTime = exitPercent / 100.0f;
                        MarkModified();
                    }
                }
                
                // Blend Duration
                ImGui::Text("Blend:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(innerWidth - 100);
                if (ImGui::DragFloat("##duration", &t.Duration, 0.01f, 0.0f, 5.0f, "%.2f s")) {
                    MarkModified();
                }
                
                // Conditions
                ImGui::Text("Conditions:");
                if (t.Conditions.empty()) {
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "  WARNING: No conditions!");
                    ImGui::TextDisabled("  (will transition immediately)");
                }
                
                int deleteCondIdx = -1;
                for (size_t ci = 0; ci < t.Conditions.size(); ++ci) {
                    auto& cond = t.Conditions[ci];
                    ImGui::PushID(static_cast<int>(ci));
                    
                    // Parameter dropdown
                    ImGui::SetNextItemWidth(80);
                    if (ImGui::BeginCombo("##param", cond.Parameter.empty() ? "<None>" : cond.Parameter.c_str())) {
                        for (const auto& p : m_Controller->Parameters) {
                            if (ImGui::Selectable(p.Name.c_str(), p.Name == cond.Parameter)) {
                                cond.Parameter = p.Name;
                                MarkModified();
                            }
                        }
                        ImGui::EndCombo();
                    }
                    
                    ImGui::SameLine();
                    
                    // Find parameter type
                    AnimatorParamType paramType = AnimatorParamType::Float;
                    for (const auto& p : m_Controller->Parameters) {
                        if (p.Name == cond.Parameter) {
                            paramType = p.Type;
                            break;
                        }
                    }
                    
                    // Condition value based on type
                    if (paramType == AnimatorParamType::Bool) {
                        // Fix invalid modes (e.g., Greater/Less from old data) - treat them as IfNot (false)
                        if (cond.Mode != ConditionMode::If && cond.Mode != ConditionMode::IfNot) {
                            cond.Mode = ConditionMode::IfNot;
                            MarkModified();
                        }
                        bool isTrue = (cond.Mode == ConditionMode::If);
                        if (ImGui::Checkbox("##boolVal", &isTrue)) {
                            cond.Mode = isTrue ? ConditionMode::If : ConditionMode::IfNot;
                            MarkModified();
                        }
                        ImGui::SameLine();
                        ImGui::TextDisabled(isTrue ? "= true" : "= false");
                    } else if (paramType == AnimatorParamType::Trigger) {
                        ImGui::TextDisabled("(trigger)");
                    } else {
                        // Float/Int comparison
                        const char* modes[] = { ">", "<", "==", "!=" };
                        int modeIdx = 0;
                        switch (cond.Mode) {
                            case ConditionMode::Greater: modeIdx = 0; break;
                            case ConditionMode::Less: modeIdx = 1; break;
                            case ConditionMode::Equals: modeIdx = 2; break;
                            case ConditionMode::NotEquals: modeIdx = 3; break;
                            default: cond.Mode = ConditionMode::Greater; break;
                        }
                        ImGui::SetNextItemWidth(45);
                        if (ImGui::Combo("##mode", &modeIdx, modes, 4)) {
                            switch (modeIdx) {
                                case 0: cond.Mode = ConditionMode::Greater; break;
                                case 1: cond.Mode = ConditionMode::Less; break;
                                case 2: cond.Mode = ConditionMode::Equals; break;
                                case 3: cond.Mode = ConditionMode::NotEquals; break;
                            }
                            MarkModified();
                        }
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(50);
                        if (paramType == AnimatorParamType::Float) {
                            if (ImGui::DragFloat("##thresh", &cond.Threshold, 0.01f)) {
                                MarkModified();
                            }
                        } else {
                            if (ImGui::DragInt("##ithresh", &cond.IntThreshold)) {
                                MarkModified();
                            }
                        }
                    }
                    
                    ImGui::SameLine();
                    if (ImGui::SmallButton("X##cond")) {
                        deleteCondIdx = static_cast<int>(ci);
                    }
                    
                    ImGui::PopID();
                }
                
                if (deleteCondIdx >= 0) {
                    t.Conditions.erase(t.Conditions.begin() + deleteCondIdx);
                    MarkModified();
                }
                
                // Add condition button
                if (ImGui::SmallButton("+ Condition")) {
                    AnimatorCondition cond;
                    if (!m_Controller->Parameters.empty()) {
                        cond.Parameter = m_Controller->Parameters.front().Name;
                        // Set correct Mode based on parameter type
                        for (const auto& p : m_Controller->Parameters) {
                            if (p.Name == cond.Parameter) {
                                if (p.Type == AnimatorParamType::Trigger) cond.Mode = ConditionMode::Trigger;
                                else if (p.Type == AnimatorParamType::Bool) cond.Mode = ConditionMode::If;
                                else cond.Mode = ConditionMode::Greater;
                                break;
                            }
                        }
                    }
                    t.Conditions.push_back(cond);
                    MarkModified();
                }
                
                ImGui::Unindent();
                ImGui::TreePop();
            }
            
            ImGui::PopID();
        }
    }
    
    if (deleteTransitionId >= 0) {
        DeleteTransitionById(deleteTransitionId);
    }
    
    if (!hasTransitions) {
        ImGui::TextDisabled("  (none)");
    }
    
    ImGui::Separator();
    
    // Button to create new transition from Any State
    if (ImGui::Button("+ Add Transition...", ImVec2(contentWidth, 0))) {
        ImGui::OpenPopup("AddAnyStateTransitionPopup");
    }
    
    // Popup to select destination state
    if (ImGui::BeginPopup("AddAnyStateTransitionPopup")) {
        ImGui::Text("Transition to:");
        ImGui::Separator();
        for (const auto& s : GetCurrentStates()) {
            if (ImGui::Selectable(s.Name.c_str())) {
                // Check if transition already exists
                bool exists = false;
                for (const auto& t : GetCurrentTransitions()) {
                    if (t.FromState == -1 && t.ToState == s.Id) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    AnimatorTransition tr;
                    tr.FromState = -1; // Any State
                    tr.ToState = s.Id;
                    tr.Id = m_NextTransitionId++;
                    tr.Duration = 0.25f;
                    GetCurrentTransitions().push_back(tr);
                    MarkModified();
                }
            }
        }
        ImGui::EndPopup();
    }
}

void AnimationGraphPanel::DrawTransitionProperties(AnimatorTransition* trans) {
    if (!trans) return;
    
    float contentWidth = ImGui::GetContentRegionAvail().x;
    
    auto* from = FindState(trans->FromState);
    auto* to = FindState(trans->ToState);
    
    ImGui::Text("Transition");
    ImGui::Separator();
    
    ImGui::Text("From: %s", from ? from->Name.c_str() : "Any State");
    ImGui::Text("To: %s", to ? to->Name.c_str() : "(deleted)");
    
    ImGui::Separator();
    
    // Settings
    ImGui::Text("Settings");
    
    if (ImGui::Checkbox("Has Exit Time", &trans->HasExitTime)) {
        MarkModified();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Wait until source animation reaches Exit Time %% before transitioning");
    }
    
    if (trans->HasExitTime) {
        ImGui::Text("Exit at:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(contentWidth - 80);
        // Use 0-100 range for display, convert back to 0-1 for storage
        float exitPercent = trans->ExitTime * 100.0f;
        if (ImGui::SliderFloat("##exitTime", &exitPercent, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
            trans->ExitTime = exitPercent / 100.0f;
            MarkModified();
        }
    }
    
    ImGui::Text("Blend:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(contentWidth - 80);
    if (ImGui::DragFloat("##duration", &trans->Duration, 0.01f, 0.0f, 5.0f, "%.2f s")) {
        MarkModified();
    }
    
    if (ImGui::Checkbox("Fixed Duration", &trans->DurationNormalized)) {
        MarkModified();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("If enabled, duration is in seconds. Otherwise, it's normalized (0-1).");
    }
    
    ImGui::Separator();
    
    // Conditions
    DrawConditionEditor(trans);
    
    ImGui::Separator();
    
    // Delete button
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
    if (ImGui::Button("Delete Transition", ImVec2(contentWidth, 0))) {
        DeleteTransitionById(trans->Id);
        m_SelectedTransitionId = -1;
    }
    ImGui::PopStyleColor();
}

void AnimationGraphPanel::DrawConditionEditor(AnimatorTransition* trans) {
    if (!trans) return;
    
    float contentWidth = ImGui::GetContentRegionAvail().x;
    
    ImGui::Text("Conditions");
    
    int deleteIndex = -1;
    for (size_t i = 0; i < trans->Conditions.size(); ++i) {
        auto& cond = trans->Conditions[i];
        ImGui::PushID(static_cast<int>(i));
        
        // Parameter selector
        ImGui::SetNextItemWidth(100);
        if (ImGui::BeginCombo("##param", cond.Parameter.empty() ? "<None>" : cond.Parameter.c_str())) {
            for (const auto& p : m_Controller->Parameters) {
                if (ImGui::Selectable(p.Name.c_str(), p.Name == cond.Parameter)) {
                    cond.Parameter = p.Name;
                    // Update Mode to match new parameter type
                    if (p.Type == AnimatorParamType::Trigger) cond.Mode = ConditionMode::Trigger;
                    else if (p.Type == AnimatorParamType::Bool) cond.Mode = ConditionMode::If;
                    else if (cond.Mode == ConditionMode::Trigger || cond.Mode == ConditionMode::If || cond.Mode == ConditionMode::IfNot)
                        cond.Mode = ConditionMode::Greater; // Reset to valid numeric mode
                    MarkModified();
                }
            }
            ImGui::EndCombo();
        }
        
        ImGui::SameLine();
        
        // Find parameter type
        AnimatorParamType paramType = AnimatorParamType::Float;
        for (const auto& p : m_Controller->Parameters) {
            if (p.Name == cond.Parameter) {
                paramType = p.Type;
                break;
            }
        }
        
        // Mode selector and threshold (depends on parameter type)
        if (paramType == AnimatorParamType::Bool) {
            // Bool: simple true/false checkbox
            // Fix invalid modes (e.g., Greater/Less from old data) - treat them as IfNot (false)
            if (cond.Mode != ConditionMode::If && cond.Mode != ConditionMode::IfNot) {
                cond.Mode = ConditionMode::IfNot;
                MarkModified();
            }
            bool isTrue = (cond.Mode == ConditionMode::If);
            if (ImGui::Checkbox("##boolValue", &isTrue)) {
                cond.Mode = isTrue ? ConditionMode::If : ConditionMode::IfNot;
                MarkModified();
            }
            ImGui::SameLine();
            ImGui::TextDisabled(isTrue ? "= true" : "= false");
        } else if (paramType == AnimatorParamType::Trigger) {
            // Trigger: just show "Trigger" text, no options needed
            // Fix invalid modes - must be Trigger mode for trigger parameters
            if (cond.Mode != ConditionMode::Trigger) {
                cond.Mode = ConditionMode::Trigger;
                MarkModified();
            }
            ImGui::TextDisabled("(trigger)");
        } else {
            // Float/Int: show comparison mode and threshold
            const char* numericModes[] = { ">", "<", "==", "!=" };
            // Map ConditionMode to numeric mode index (Greater=2, Less=3, Equals=4, NotEquals=5)
            int modeIdx = 0;
            switch (cond.Mode) {
                case ConditionMode::Greater: modeIdx = 0; break;
                case ConditionMode::Less: modeIdx = 1; break;
                case ConditionMode::Equals: modeIdx = 2; break;
                case ConditionMode::NotEquals: modeIdx = 3; break;
                default: modeIdx = 0; cond.Mode = ConditionMode::Greater; MarkModified(); break;
            }
            ImGui::SetNextItemWidth(50);
            if (ImGui::Combo("##mode", &modeIdx, numericModes, IM_ARRAYSIZE(numericModes))) {
                switch (modeIdx) {
                    case 0: cond.Mode = ConditionMode::Greater; break;
                    case 1: cond.Mode = ConditionMode::Less; break;
                    case 2: cond.Mode = ConditionMode::Equals; break;
                    case 3: cond.Mode = ConditionMode::NotEquals; break;
                }
                MarkModified();
            }
            
            ImGui::SameLine();
            
            // Threshold value
            if (paramType == AnimatorParamType::Float) {
                ImGui::SetNextItemWidth(60);
                if (ImGui::DragFloat("##thresh", &cond.Threshold, 0.01f)) {
                    MarkModified();
                }
            } else if (paramType == AnimatorParamType::Int) {
                ImGui::SetNextItemWidth(60);
                if (ImGui::DragInt("##ithresh", &cond.IntThreshold)) {
                    MarkModified();
                }
            }
        }
        
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) {
            deleteIndex = static_cast<int>(i);
        }
        
        ImGui::PopID();
    }
    
    if (deleteIndex >= 0) {
        trans->Conditions.erase(trans->Conditions.begin() + deleteIndex);
        MarkModified();
    }
    
    if (ImGui::Button("+ Add Condition", ImVec2(contentWidth, 0))) {
        AnimatorCondition cond;
        if (!m_Controller->Parameters.empty()) {
            cond.Parameter = m_Controller->Parameters.front().Name;
        }
        trans->Conditions.push_back(cond);
        MarkModified();
    }
}

// ============================================================================
// Animation Clip Picker
// ============================================================================
void AnimationGraphPanel::RefreshAnimationClipList() {
    m_AnimationClipPaths.clear();

    const auto& animationOptions = ui::GetAnimationAssetOptions();
    m_AnimationClipPaths.reserve(animationOptions.size());
    for (const ui::AnimationAssetOption& option : animationOptions) {
        m_AnimationClipPaths.push_back(MakeControllerRelativePath(option.path));
    }

    std::sort(m_AnimationClipPaths.begin(), m_AnimationClipPaths.end());
    m_AnimationClipPaths.erase(std::unique(m_AnimationClipPaths.begin(), m_AnimationClipPaths.end()), m_AnimationClipPaths.end());
    m_AnimationClipListDirty = false;
}

void AnimationGraphPanel::DrawAnimationClipPicker(std::string& clipPath) {
    if (m_AnimationClipListDirty) {
        RefreshAnimationClipList();
    }
    
    float contentWidth = ImGui::GetContentRegionAvail().x;
    
    // Current clip display
    std::string displayName = clipPath.empty() ? "(None)" : fs::path(clipPath).stem().string();
    
    ImGui::SetNextItemWidth(contentWidth - 30);
    if (ImGui::BeginCombo("##clipPicker", displayName.c_str())) {
        // Search filter
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::InputTextWithHint("##search", "Search...", m_ClipSearchBuffer, sizeof(m_ClipSearchBuffer));
        
        std::string searchStr = m_ClipSearchBuffer;
        std::transform(searchStr.begin(), searchStr.end(), searchStr.begin(), ::tolower);
        
        ImGui::Separator();
        
        if (ImGui::Selectable("(None)", clipPath.empty())) {
            clipPath.clear();
            MarkModified();
        }
        
        // Normalize clipPath for comparison
        std::string clipPathNormalized = clipPath;
        for (char& c : clipPathNormalized) if (c == '\\') c = '/';
        
        for (const auto& path : m_AnimationClipPaths) {
            std::string name = fs::path(path).stem().string();
            std::string nameLower = name;
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
            
            if (!searchStr.empty() && nameLower.find(searchStr) == std::string::npos) {
                continue;
            }
            
            // Compare normalized paths
            bool selected = (path == clipPathNormalized);
            if (ImGui::Selectable(name.c_str(), selected)) {
                clipPath = path; // path is already VFS-relative
                MarkModified();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", path.c_str());
            }
        }
        
        ImGui::EndCombo();
    }
    
    ImGui::SameLine();
    if (ImGui::Button("...", ImVec2(25, 0))) {
        std::string p = ShowOpenFileDialogExt(L"Animation Clips (*.anim)", L"anim");
        if (!p.empty()) {
            clipPath = MakeControllerRelativePath(p);
            MarkModified();
        }
    }
    
    // Show current path as tooltip
    if (ImGui::IsItemHovered() && !clipPath.empty()) {
        ImGui::SetTooltip("%s", clipPath.c_str());
    }
}

// ============================================================================
// Keyboard Shortcuts
// ============================================================================
void AnimationGraphPanel::HandleKeyboardShortcuts() {
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) return;
    
    bool ctrl = ImGui::GetIO().KeyCtrl;
    
    // Delete
    if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        if (m_SelectedStateId >= 0) {
            DeleteStateById(m_SelectedStateId);
            m_SelectedStateId = -1;
        } else if (m_SelectedTransitionId >= 0) {
            DeleteTransitionById(m_SelectedTransitionId);
            m_SelectedTransitionId = -1;
        }
    }
    
    // Frame all
    if (ImGui::IsKeyPressed(ImGuiKey_F)) {
        FrameAllNodesNextFrame();
    }
    
    // New
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_N)) {
        auto ctrl = std::make_shared<AnimatorController>();
        ctrl->Name = "New Controller";
        // Create base layer for new controller
        cm::animation::AnimatorLayer baseLayer;
        baseLayer.Index = 0;
        baseLayer.Name = "Base Layer";
        baseLayer.DefaultState = -1;
        ctrl->Layers.push_back(std::move(baseLayer));
        m_Controller = std::move(ctrl);
        m_OpenPath.clear();
        m_Modified = true;
        EnsureIdCounters();
        ResetEditorState();
    }
    
    // Open
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O)) {
        std::string p = ShowOpenFileDialogExt(L"Animator Controllers (*.animctrl)", L"animctrl");
        if (!p.empty()) LoadController(p);
    }
}

// ============================================================================
// Drag-Drop for Animation Files
// ============================================================================
void AnimationGraphPanel::HandleDragDrop() {
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
            const char* path = static_cast<const char*>(payload->Data);
            std::string ext = fs::path(path).extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            
            if (ext == ".anim") {
                // Create a new state with this animation
                AnimatorState s;
                s.Id = m_NextStateId++;
                s.Name = fs::path(path).stem().string();
                s.AnimationAssetPath = MakeControllerRelativePath(path);
                s.Kind = AnimatorStateKind::Single;
                GetCurrentStates().push_back(s);
                
                // Position at mouse
                ImVec2 mousePos = ImGui::GetMousePos();
                m_PendingNewNodeId = s.Id;
                m_PendingNewNodePos = mousePos;
                
                if (GetCurrentDefaultState() < 0) {
                    GetCurrentDefaultState() = s.Id;
                }
                MarkModified();
            }
        }
        ImGui::EndDragDropTarget();
    }
}

// ============================================================================
// State Management
// ============================================================================
void AnimationGraphPanel::EnsureIdCounters() {
    m_NextStateId = 1;
    m_NextTransitionId = 1;
    if (m_Controller) {
        for (const auto& s : GetCurrentStates())
            m_NextStateId = std::max(m_NextStateId, s.Id + 1);
        for (const auto& t : GetCurrentTransitions())
            m_NextTransitionId = std::max(m_NextTransitionId, t.Id + 1);
    }
}

void AnimationGraphPanel::ResetEditorState() {
    m_SelectedStateId = -1;
    m_SelectedTransitionId = -1;
    m_SelectedLayerIndex = 0;  // Reset to base layer when loading new controller
    m_PositionSeed.clear();
    m_PendingNewNodeId = -1;
    m_FrameGraphNext = true;
    m_AnimationClipListDirty = true;
}

void AnimationGraphPanel::SyncNodePositionsToModel() {
    if (!m_Controller || !m_NodeEditorContext) return;
    
    // Must set the editor context to query node positions
    // (Save can be triggered from menu bar before DrawGraphCanvas sets the context)
    ImNodes::EditorContextSet(m_NodeEditorContext);
    
    // Only query positions for nodes that have been drawn (exist in m_PositionSeed)
    // This prevents crashes when saving before the graph canvas has rendered
    for (auto& s : GetCurrentStates()) {
        if (m_PositionSeed.count(s.Id)) {
            ImVec2 pos = ImNodes::GetNodeGridSpacePos(s.Id);
            s.EditorPosX = pos.x;
            s.EditorPosY = pos.y;
        }
    }
    
    // Also save special node positions if they've been drawn
    if (m_ShowEntryNode && m_PositionSeed.count(kEntryNodeId)) {
        m_EntryNodePos = ImNodes::GetNodeGridSpacePos(kEntryNodeId);
    }
    if (m_ShowAnyStateNode && m_PositionSeed.count(kAnyStateNodeId)) {
        m_AnyStateNodePos = ImNodes::GetNodeGridSpacePos(kAnyStateNodeId);
    }
}

void AnimationGraphPanel::RestoreNodePositionsFromModel() {
    if (!m_Controller) return;
    
    m_PositionSeed.clear();
    
    // Positions will be restored in DrawGraphCanvas when the node is first drawn
    // and not yet in m_PositionSeed
}

void AnimationGraphPanel::MarkModified() {
    m_Modified = true;
}

void AnimationGraphPanel::CreateStateAtPosition(const ImVec2& gridPos, AnimatorStateKind kind) {
    if (!m_Controller) return;
    
    AnimatorState s;
    s.Id = m_NextStateId++;
    switch (kind) {
        case AnimatorStateKind::Blend1D:
            s.Name = "Blend Tree 1D";
            break;
        case AnimatorStateKind::Blend2D:
            s.Name = "Blend Tree 2D";
            break;
        case AnimatorStateKind::Single:
        default:
            s.Name = "State " + std::to_string(GetCurrentStates().size());
            break;
    }
    s.Kind = kind;
    GetCurrentStates().push_back(s);
    
    m_PendingNewNodeId = s.Id;
    m_PendingNewNodePos = gridPos;
    
    if (GetCurrentDefaultState() < 0) {
        GetCurrentDefaultState() = s.Id;
    }
    
    MarkModified();
}

void AnimationGraphPanel::DeleteStateById(int stateId) {
    if (!m_Controller) return;
    
    // Remove transitions referencing this state
    GetCurrentTransitions().erase(
        std::remove_if(GetCurrentTransitions().begin(), GetCurrentTransitions().end(),
            [stateId](const AnimatorTransition& t) {
                return t.FromState == stateId || t.ToState == stateId;
            }),
        GetCurrentTransitions().end());
    
    // Remove the state
    GetCurrentStates().erase(
        std::remove_if(GetCurrentStates().begin(), GetCurrentStates().end(),
            [stateId](const AnimatorState& s) { return s.Id == stateId; }),
        GetCurrentStates().end());
    
    // Update default state if needed
    if (GetCurrentDefaultState() == stateId) {
        GetCurrentDefaultState() = GetCurrentStates().empty() ? -1 : GetCurrentStates().front().Id;
    }
    
    MarkModified();
}

void AnimationGraphPanel::DeleteTransitionById(int transitionId) {
    if (!m_Controller) return;
    GetCurrentTransitions().erase(
        std::remove_if(GetCurrentTransitions().begin(), GetCurrentTransitions().end(),
            [transitionId](const AnimatorTransition& t) { return t.Id == transitionId; }),
        GetCurrentTransitions().end());
    MarkModified();
}

void AnimationGraphPanel::RemoveConditionsReferencingParam(const std::string& paramName) {
    if (!m_Controller) return;
    for (auto& t : GetCurrentTransitions()) {
        t.Conditions.erase(
            std::remove_if(t.Conditions.begin(), t.Conditions.end(),
                [&paramName](const AnimatorCondition& c) {
                    return c.Parameter == paramName;
                }),
            t.Conditions.end());
    }
}

void AnimationGraphPanel::FrameAllNodesNextFrame() {
    m_FrameGraphNext = true;
}

void AnimationGraphPanel::ApplyPendingNodePlacement() {
    if (m_PendingNewNodeId >= 0) {
        ImVec2 pan = ImNodes::EditorContextGetPanning();
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImVec2 gridPos = ImVec2(m_PendingNewNodePos.x - origin.x - pan.x, 
                                 m_PendingNewNodePos.y - origin.y - pan.y);
        ImNodes::SetNodeGridSpacePos(m_PendingNewNodeId, gridPos);
        m_PositionSeed.insert(m_PendingNewNodeId);
        m_PendingNewNodeId = -1;
    }
}

// ============================================================================
// Lookups
// ============================================================================
cm::animation::AnimatorState* AnimationGraphPanel::FindState(int id) {
    if (!m_Controller) return nullptr;
    for (auto& s : GetCurrentStates())
        if (s.Id == id) return &s;
    return nullptr;
}

const cm::animation::AnimatorState* AnimationGraphPanel::FindState(int id) const {
    if (!m_Controller) return nullptr;
    for (const auto& s : GetCurrentStates())
        if (s.Id == id) return &s;
    return nullptr;
}

cm::animation::AnimatorTransition* AnimationGraphPanel::FindTransition(int id) {
    if (!m_Controller) return nullptr;
    for (auto& t : GetCurrentTransitions())
        if (t.Id == id) return &t;
    return nullptr;
}

// ============================================================================
// Layer Helpers
// ============================================================================
std::vector<cm::animation::AnimatorState>& AnimationGraphPanel::GetCurrentStates() {
    static std::vector<cm::animation::AnimatorState> empty;
    if (!m_Controller) return empty;
    
    // If using new layer architecture
    if (m_Controller->HasLayers()) {
        if (m_SelectedLayerIndex >= 0 && m_SelectedLayerIndex < static_cast<int>(m_Controller->Layers.size())) {
            return m_Controller->Layers[m_SelectedLayerIndex].States;
        }
        return empty;
    }
    // Legacy flat structure not supported in editor - return empty to avoid crash
    return empty;
}

const std::vector<cm::animation::AnimatorState>& AnimationGraphPanel::GetCurrentStates() const {
    static const std::vector<cm::animation::AnimatorState> empty;
    if (!m_Controller) return empty;
    
    if (m_Controller->HasLayers()) {
        if (m_SelectedLayerIndex >= 0 && m_SelectedLayerIndex < static_cast<int>(m_Controller->Layers.size())) {
            return m_Controller->Layers[m_SelectedLayerIndex].States;
        }
        return empty;
    }
    // Legacy flat structure not supported in editor - return empty to avoid crash
    return empty;
}

std::vector<cm::animation::AnimatorTransition>& AnimationGraphPanel::GetCurrentTransitions() {
    static std::vector<cm::animation::AnimatorTransition> empty;
    if (!m_Controller) return empty;
    
    if (m_Controller->HasLayers()) {
        if (m_SelectedLayerIndex >= 0 && m_SelectedLayerIndex < static_cast<int>(m_Controller->Layers.size())) {
            return m_Controller->Layers[m_SelectedLayerIndex].Transitions;
        }
        return empty;
    }
    // Legacy flat structure not supported in editor - return empty to avoid crash
    return empty;
}

const std::vector<cm::animation::AnimatorTransition>& AnimationGraphPanel::GetCurrentTransitions() const {
    static const std::vector<cm::animation::AnimatorTransition> empty;
    if (!m_Controller) return empty;
    
    if (m_Controller->HasLayers()) {
        if (m_SelectedLayerIndex >= 0 && m_SelectedLayerIndex < static_cast<int>(m_Controller->Layers.size())) {
            return m_Controller->Layers[m_SelectedLayerIndex].Transitions;
        }
        return empty;
    }
    // Legacy flat structure not supported in editor - return empty to avoid crash
    return empty;
}

int& AnimationGraphPanel::GetCurrentDefaultState() {
    static int dummy = -1;
    if (!m_Controller) return dummy;
    
    if (m_Controller->HasLayers()) {
        if (m_SelectedLayerIndex >= 0 && m_SelectedLayerIndex < static_cast<int>(m_Controller->Layers.size())) {
            return m_Controller->Layers[m_SelectedLayerIndex].DefaultState;
        }
        return dummy;
    }
    return m_Controller->DefaultState;
}

cm::animation::AnimatorLayer* AnimationGraphPanel::GetCurrentLayer() {
    if (!m_Controller || !m_Controller->HasLayers()) return nullptr;
    if (m_SelectedLayerIndex >= 0 && m_SelectedLayerIndex < static_cast<int>(m_Controller->Layers.size())) {
        return &m_Controller->Layers[m_SelectedLayerIndex];
    }
    return nullptr;
}

const cm::animation::AnimatorLayer* AnimationGraphPanel::GetCurrentLayer() const {
    if (!m_Controller || !m_Controller->HasLayers()) return nullptr;
    if (m_SelectedLayerIndex >= 0 && m_SelectedLayerIndex < static_cast<int>(m_Controller->Layers.size())) {
        return &m_Controller->Layers[m_SelectedLayerIndex];
    }
    return nullptr;
}

// ============================================================================
// Path Utilities
// ============================================================================
std::string AnimationGraphPanel::ResolveAssetPath(const std::string& path) const {
    if (path.empty()) return {};
    fs::path p(path);
    if (p.is_absolute()) return p.string();
    fs::path project = Project::GetProjectDirectory();
    if (project.empty()) return path;
    return (project / p).string();
}

std::string AnimationGraphPanel::MakeControllerRelativePath(const std::string& path) const {
    if (path.empty()) return {};
    
    // Normalize slashes
    std::string normalized = path;
    for (char& c : normalized) {
        if (c == '\\') c = '/';
    }
    
    // Look for "assets/" in the path and extract from there
    // This handles paths from build directories like:
    // C:/Users/.../out/build/x64-Debug/assets/animations/Idle.anim
    // -> assets/animations/Idle.anim
    size_t assetsPos = normalized.find("/assets/");
    if (assetsPos != std::string::npos) {
        return normalized.substr(assetsPos + 1); // Skip the leading '/'
    }
    
    // Also check if path starts with "assets/"
    if (normalized.find("assets/") == 0) {
        return normalized;
    }
    
    // Try to make it relative to project directory
    try {
        fs::path p(path);
        fs::path projectDir = Project::GetProjectDirectory();
        fs::path assetsDir = Project::GetAssetDirectory();
        
        // If the path is under the assets directory, make it relative to project
        if (!assetsDir.empty()) {
            std::string assetsStr = assetsDir.string();
            for (char& c : assetsStr) if (c == '\\') c = '/';
            
            if (normalized.find(assetsStr) != std::string::npos) {
                // Extract relative to assets parent
                fs::path rel = fs::relative(p, assetsDir.parent_path());
                std::string result = rel.string();
                for (char& c : result) if (c == '\\') c = '/';
                return result;
            }
        }
        
        // Fallback: if under project directory, make relative
        if (!projectDir.empty() && p.is_absolute()) {
            std::string projStr = projectDir.string();
            for (char& c : projStr) if (c == '\\') c = '/';
            
            if (normalized.find(projStr) != std::string::npos) {
                fs::path rel = fs::relative(p, projectDir);
                std::string result = rel.string();
                for (char& c : result) if (c == '\\') c = '/';
                // Avoid paths that go up (../)
                if (result.find("../") == std::string::npos) {
                    return result;
                }
            }
        }
    } catch (...) {}
    
    // Last resort: just return the filename and assume it's in assets/animations/
    fs::path p(path);
    std::string filename = p.filename().string();
    if (!filename.empty() && filename.find(".anim") != std::string::npos) {
        return "assets/animations/" + filename;
    }
    
    return normalized;
}

std::string AnimationGraphPanel::MakeVFSPath(const std::string& absolutePath) const {
    // Delegate to MakeControllerRelativePath which now handles VFS paths properly
    return MakeControllerRelativePath(absolutePath);
}

// ============================================================================
// Attribute ID Utilities
// ============================================================================
int AnimationGraphPanel::MakeInputAttr(int nodeId) const {
    return nodeId * 2 + 10000;
}

int AnimationGraphPanel::MakeOutputAttr(int nodeId) const {
    return nodeId * 2 + 1 + 10000;
}

int AnimationGraphPanel::AttrToNode(int attrId) const {
    int shifted = attrId - 10000;
    // Use floor division for correct handling of negative node IDs
    // C++ integer division truncates toward zero, but we need toward negative infinity
    return shifted >= 0 ? shifted / 2 : (shifted - 1) / 2;
}

bool AnimationGraphPanel::IsOutputAttr(int attrId) const {
    int shifted = attrId - 10000;
    // For negative shifted values, we need to check if the original nodeId*2 was odd
    // The pattern is: even shifted = input (nodeId*2), odd shifted = output (nodeId*2+1)
    // This works correctly for both positive and negative because we add 1 for output
    return (shifted & 1) == 1;
}

// ============================================================================
// Custom Transition Arrow Drawing
// ============================================================================
bool AnimationGraphPanel::IsNodeDrawn(int nodeId) const {
    return m_PositionSeed.count(nodeId) > 0;
}

ImVec2 AnimationGraphPanel::GetNodeCenter(int nodeId) {
    // Safety check: return zero if node hasn't been drawn
    if (!IsNodeDrawn(nodeId)) {
        return ImVec2(0.0f, 0.0f);
    }
    ImVec2 pos = ImNodes::GetNodeScreenSpacePos(nodeId);
    ImVec2 dim = ImNodes::GetNodeDimensions(nodeId);
    return ImVec2(pos.x + dim.x * 0.5f, pos.y + dim.y * 0.5f);
}

ImVec2 AnimationGraphPanel::GetNodeEdgePoint(int nodeId, ImVec2 direction) {
    // Safety check: return zero if node hasn't been drawn
    if (!IsNodeDrawn(nodeId)) {
        return ImVec2(0.0f, 0.0f);
    }
    ImVec2 pos = ImNodes::GetNodeScreenSpacePos(nodeId);
    ImVec2 dim = ImNodes::GetNodeDimensions(nodeId);
    ImVec2 center(pos.x + dim.x * 0.5f, pos.y + dim.y * 0.5f);
    
    // Normalize direction
    float len = sqrtf(direction.x * direction.x + direction.y * direction.y);
    if (len < 0.001f) return center;
    direction.x /= len;
    direction.y /= len;
    
    // Find intersection with node rectangle (with small padding)
    float halfW = dim.x * 0.5f + 4.0f;
    float halfH = dim.y * 0.5f + 4.0f;
    
    // Calculate intersection using parametric line-box intersection
    float tX = (direction.x != 0.0f) ? halfW / fabsf(direction.x) : 1e10f;
    float tY = (direction.y != 0.0f) ? halfH / fabsf(direction.y) : 1e10f;
    float t = std::min(tX, tY);
    
    return ImVec2(center.x + direction.x * t, center.y + direction.y * t);
}

void AnimationGraphPanel::DrawArrow(ImVec2 from, ImVec2 to, ImU32 color, float thickness, bool selected) {
    // Use window draw list - we draw after EndNodeEditor() when channels are merged
    ImDrawList* dl = ImGui::GetWindowDrawList();
    
    ImVec2 dir(to.x - from.x, to.y - from.y);
    float len = sqrtf(dir.x * dir.x + dir.y * dir.y);
    if (len < 1.0f) return;
    
    // Normalize
    dir.x /= len;
    dir.y /= len;
    
    // Draw the main line
    if (selected) {
        // Draw a thicker glow behind selected transitions
        dl->AddLine(from, to, IM_COL32(100, 180, 230, 100), thickness + 4.0f);
    }
    dl->AddLine(from, to, color, thickness);
    
    // Draw arrowhead
    float arrowSize = 10.0f;
    ImVec2 perpendicular(-dir.y, dir.x);
    
    ImVec2 arrowP1(to.x - dir.x * arrowSize + perpendicular.x * arrowSize * 0.4f,
                   to.y - dir.y * arrowSize + perpendicular.y * arrowSize * 0.4f);
    ImVec2 arrowP2(to.x - dir.x * arrowSize - perpendicular.x * arrowSize * 0.4f,
                   to.y - dir.y * arrowSize - perpendicular.y * arrowSize * 0.4f);
    
    dl->AddTriangleFilled(to, arrowP1, arrowP2, color);
}

int AnimationGraphPanel::GetTransitionAtPoint(ImVec2 point, float threshold) {
    // Use cached arrow segments from the last DrawTransitionArrows call
    // This ensures we use the same coordinates that were used for drawing
    for (const auto& seg : m_CachedArrowSegments) {
        ImVec2 from = seg.from;
        ImVec2 to = seg.to;
        
        // Point-to-line-segment distance check
        ImVec2 lineVec(to.x - from.x, to.y - from.y);
        ImVec2 pointVec(point.x - from.x, point.y - from.y);
        float lineLen = sqrtf(lineVec.x * lineVec.x + lineVec.y * lineVec.y);
        if (lineLen < 1.0f) continue;
        
        // Project point onto line
        float t_param = (pointVec.x * lineVec.x + pointVec.y * lineVec.y) / (lineLen * lineLen);
        t_param = std::clamp(t_param, 0.0f, 1.0f);
        
        ImVec2 closest(from.x + t_param * lineVec.x, from.y + t_param * lineVec.y);
        float dist = sqrtf((point.x - closest.x) * (point.x - closest.x) + 
                          (point.y - closest.y) * (point.y - closest.y));
        
        if (dist < threshold) {
            return seg.transitionId;
        }
    }
    
    return -1;
}

void AnimationGraphPanel::DrawTransitionArrows() {
    if (!m_Controller) return;
    
    // Clear cached arrow segments for hit detection
    m_CachedArrowSegments.clear();
    
    // Build a set of bidirectional transition pairs for offset calculation
    std::unordered_set<int64_t> bidirectionalPairs;
    for (const auto& t : GetCurrentTransitions()) {
        for (const auto& other : GetCurrentTransitions()) {
            if (other.FromState == t.ToState && other.ToState == t.FromState) {
                // Use a consistent key for the pair
                int minId = std::min(t.FromState, t.ToState);
                int maxId = std::max(t.FromState, t.ToState);
                bidirectionalPairs.insert((int64_t(minId) << 32) | int64_t(maxId + 0x80000000));
            }
        }
    }
    
    // Draw Entry -> Default State link (green)
    if (m_ShowEntryNode && GetCurrentDefaultState() >= 0 && 
        IsNodeDrawn(kEntryNodeId) && IsNodeDrawn(GetCurrentDefaultState())) {
        ImVec2 entryCenter = GetNodeCenter(kEntryNodeId);
        ImVec2 defaultCenter = GetNodeCenter(GetCurrentDefaultState());
        ImVec2 dir(defaultCenter.x - entryCenter.x, defaultCenter.y - entryCenter.y);
        
        ImVec2 from = GetNodeEdgePoint(kEntryNodeId, dir);
        ImVec2 to = GetNodeEdgePoint(GetCurrentDefaultState(), ImVec2(-dir.x, -dir.y));
        
        DrawArrow(from, to, kColorEntry, 3.0f, false);
    }
    
    // Draw all transitions
    for (const auto& t : GetCurrentTransitions()) {
        int fromId = (t.FromState == -1) ? kAnyStateNodeId : t.FromState;
        int toId = t.ToState;
        
        // Skip if "Any State" is hidden for AnyState transitions
        if (t.FromState == -1 && !m_ShowAnyStateNode) continue;
        
        // Skip if source state doesn't exist (not AnyState)
        if (t.FromState != -1 && !FindState(t.FromState)) continue;
        if (!FindState(t.ToState)) continue;
        
        // Skip if nodes haven't been drawn yet
        if (!IsNodeDrawn(fromId) || !IsNodeDrawn(toId)) continue;
        
        ImVec2 fromCenter = GetNodeCenter(fromId);
        ImVec2 toCenter = GetNodeCenter(toId);
        ImVec2 dir(toCenter.x - fromCenter.x, toCenter.y - fromCenter.y);
        
        ImVec2 from = GetNodeEdgePoint(fromId, dir);
        ImVec2 to = GetNodeEdgePoint(toId, ImVec2(-dir.x, -dir.y));
        
        // Check if this is part of a bidirectional pair
        int minState = std::min(t.FromState, t.ToState);
        int maxState = std::max(t.FromState, t.ToState);
        int64_t pairKey = (int64_t(minState) << 32) | int64_t(maxState + 0x80000000);
        bool isBidirectional = bidirectionalPairs.count(pairKey) > 0;
        
        if (isBidirectional) {
            // Offset perpendicular to the line direction
            float len = sqrtf(dir.x * dir.x + dir.y * dir.y);
            if (len > 0.001f) {
                float offset = 6.0f;
                ImVec2 perp(-dir.y / len * offset, dir.x / len * offset);
                from.x += perp.x; from.y += perp.y;
                to.x += perp.x; to.y += perp.y;
            }
        }
        
        // Choose color
        ImU32 color = (t.FromState == -1) ? kColorAnyState : kColorLink;
        bool selected = (m_SelectedTransitionId == t.Id);
        if (selected) {
            color = kColorLinkSelected;
        }
        
        DrawArrow(from, to, color, 3.0f, selected);
        
        // Cache the arrow segment for hit detection
        m_CachedArrowSegments.push_back({t.Id, from, to});
    }
}

// ============================================================================
// Clip Editor (Placeholder)
// ============================================================================
void AnimationGraphPanel::DrawClipEditor(float height) {
    // Placeholder for inline clip editor
}

void AnimationGraphPanel::SyncTimelineToSelection() {
    // Sync timeline panel to selected state
}

void AnimationGraphPanel::UpdateInspectorBinding() {
    // Update inspector bindings
}







