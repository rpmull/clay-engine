#include "editor/tools/UIPrefabLayoutDesignerPanel.h"
#include "editor/ui/UILayer.h"
#include "editor/Project.h"
#include "editor/prefab/PrefabEditorAPI.h"
#include "editor/pipeline/AssetLibrary.h"
#include "editor/pipeline/AssetRegistry.h"
#include "editor/pipeline/AssetPipeline.h"
#include "core/assets/AssetMetadata.h"
#include "core/prefab/PrefabAPI.h"
#include "core/ecs/EntityData.h"
#include "core/ecs/Components.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace {
    const char* kNodeTypeLabels[] = { "Panel", "Button", "Text", "Image", "ScrollView", "Spacer" };
    const char* kTemplateLabels[] = { "Blank", "Tabbed Menu (Top Tabs)", "Scrollable List", "Split Panel" };
    const char* kAnchorLabels[] = {
        "Top Left", "Top", "Top Right",
        "Left", "Center", "Right",
        "Bottom Left", "Bottom", "Bottom Right"
    };
    const char* kScaleModeLabels[] = {
        "Constant Pixel Size",
        "Scale With Width",
        "Scale With Height",
        "Scale With Smallest",
        "Scale With Largest",
        "Expand"
    };
    const char* kRenderSpaceLabels[] = { "Screen Space", "World Space" };
    const char* kContentSizeLabels[] = { "Match Viewport", "Manual", "Estimated Items", "From Children" };
    const char* kLayoutDirectionLabels[] = { "Horizontal", "Vertical" };
    const char* kAlignmentLabels[] = { "Start", "Center", "End" };

    void AnchorPresetToNormalized(UIAnchorPreset preset, float& x, float& y) {
        switch (preset) {
        case UIAnchorPreset::TopLeft:     x = 0.0f; y = 0.0f; break;
        case UIAnchorPreset::Top:         x = 0.5f; y = 0.0f; break;
        case UIAnchorPreset::TopRight:    x = 1.0f; y = 0.0f; break;
        case UIAnchorPreset::Left:        x = 0.0f; y = 0.5f; break;
        case UIAnchorPreset::Center:      x = 0.5f; y = 0.5f; break;
        case UIAnchorPreset::Right:       x = 1.0f; y = 0.5f; break;
        case UIAnchorPreset::BottomLeft:  x = 0.0f; y = 1.0f; break;
        case UIAnchorPreset::Bottom:      x = 0.5f; y = 1.0f; break;
        case UIAnchorPreset::BottomRight: x = 1.0f; y = 1.0f; break;
        default:                          x = 0.5f; y = 0.5f; break;
        }
    }

    std::string NormalizeSlashes(std::string value) {
        std::replace(value.begin(), value.end(), '\\', '/');
        return value;
    }
}

UIPrefabLayoutDesignerPanel::UIPrefabLayoutDesignerPanel(Scene* scene, UILayer* uiLayer)
    : m_UILayer(uiLayer) {
    m_Context = scene;
    ResetLayout();
}

void UIPrefabLayoutDesignerPanel::Open() {
    m_Open = true;
    m_FocusNextFrame = true;
}

void UIPrefabLayoutDesignerPanel::OnImGuiRender() {
    if (!m_Open) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(1120.0f, 720.0f), ImGuiCond_FirstUseEver);
    if (m_FocusNextFrame) {
        ImGui::SetNextWindowFocus();
        m_FocusNextFrame = false;
    }

    if (!ImGui::Begin("UI Prefab Layout Designer", &m_Open)) {
        ImGui::End();
        return;
    }

    DrawToolbar();
    ImGui::Separator();
    DrawPrefabSettings();
    DrawCanvasSettings();
    DrawTemplateSettings();

    ImGui::Separator();
    ImGui::Columns(2, "UILayoutColumns");
    DrawLayoutTree();
    ImGui::NextColumn();
    DrawNodeInspector();
    ImGui::Columns(1);

    ImGui::Separator();
    DrawValidationSummary();
    DrawExportSection();

    ImGui::End();
}

void UIPrefabLayoutDesignerPanel::ResetLayout() {
    m_Nodes.clear();
    m_NextNodeId = 1;

    LayoutNode& root = CreateNode(NodeType::Panel, "Root");
    root.parentId = -1;
    root.anchorEnabled = true;
    root.anchor = UIAnchorPreset::TopLeft;
    root.anchorOffset = {0.0f, 0.0f};
    root.size = GetDesignCanvasSize();

    m_RootNodeId = root.id;
    m_SelectedNodeId = root.id;
}

void UIPrefabLayoutDesignerPanel::BuildTemplate(TemplateType type) {
    m_SelectedTemplate = type;
    switch (type) {
    case TemplateType::TabbedMenu:
        BuildTabbedMenuTemplate();
        break;
    case TemplateType::ScrollList:
        BuildScrollListTemplate();
        break;
    case TemplateType::SplitPanel:
        BuildSplitPanelTemplate();
        break;
    default:
        ResetLayout();
        break;
    }
}

void UIPrefabLayoutDesignerPanel::BuildTabbedMenuTemplate() {
    ResetLayout();
    LayoutNode* root = FindNode(m_RootNodeId);
    if (!root) return;

    root->layout.enabled = true;
    root->layout.direction = LayoutGroupComponent::LayoutDirection::Vertical;
    root->layout.padding = {16.0f, 16.0f, 16.0f, 16.0f};
    root->layout.spacing = 12.0f;
    root->layout.controlChildWidth = true;
    root->layout.childForceExpandWidth = true;

    const float tabBarHeight = 56.0f;
    const float contentHeight = std::max(120.0f, root->size.y - tabBarHeight - root->layout.padding.y - root->layout.padding.w - root->layout.spacing);

    LayoutNode& tabBar = CreateNode(NodeType::Panel, "Tab Bar");
    tabBar.size = {root->size.x, tabBarHeight};
    tabBar.layout.enabled = true;
    tabBar.layout.direction = LayoutGroupComponent::LayoutDirection::Horizontal;
    tabBar.layout.padding = {8.0f, 8.0f, 8.0f, 8.0f};
    tabBar.layout.spacing = 8.0f;
    tabBar.tint = {0.08f, 0.08f, 0.1f, 0.9f};
    AddChild(root->id, tabBar.id);

    LayoutNode& content = CreateNode(NodeType::Panel, "Tab Content");
    content.size = {root->size.x, contentHeight};
    content.tint = {0.05f, 0.05f, 0.07f, 0.85f};
    AddChild(root->id, content.id);

    struct TabDef {
        const char* name;
        enum class ContentType { List, Text, Map } contentType;
    };

    const TabDef tabs[] = {
        { "Items", TabDef::ContentType::List },
        { "Equipment", TabDef::ContentType::List },
        { "Quests", TabDef::ContentType::Text },
        { "Map", TabDef::ContentType::Map }
    };

    bool first = true;
    for (const auto& tab : tabs) {
        LayoutNode& tabButton = CreateNode(NodeType::Button, std::string(tab.name) + " Tab");
        tabButton.button.label = tab.name;
        tabButton.size = {130.0f, 36.0f};
        AddChild(tabBar.id, tabButton.id);

        LayoutNode& tabPanel = CreateNode(NodeType::Panel, std::string(tab.name) + " Panel");
        tabPanel.size = {content.size.x, content.size.y};
        tabPanel.visible = first;
        AddChild(content.id, tabPanel.id);

        if (tab.contentType == TabDef::ContentType::List) {
            LayoutNode& list = CreateNode(NodeType::ScrollView, std::string(tab.name) + " List");
            list.size = {tabPanel.size.x, tabPanel.size.y};
            list.scroll.vertical = true;
            list.scroll.horizontal = false;
            list.scroll.markDynamicList = true;
            list.scroll.contentSizeMode = ContentSizeMode::EstimatedItems;
            list.scroll.estimatedItemCount = 12;
            list.scroll.estimatedItemSize = {tabPanel.size.x - 40.0f, 36.0f};
            list.scroll.includeExampleItem = true;
            AddChild(tabPanel.id, list.id);
        } else if (tab.contentType == TabDef::ContentType::Text) {
            LayoutNode& text = CreateNode(NodeType::Text, std::string(tab.name) + " Text");
            text.text.value = "Quest details go here. Use word wrap to fill the panel.";
            text.text.wordWrap = true;
            text.text.rectSize = {tabPanel.size.x - 40.0f, tabPanel.size.y - 40.0f};
            text.position = {20.0f, 20.0f};
            AddChild(tabPanel.id, text.id);
        } else {
            LayoutNode& mapPanel = CreateNode(NodeType::Panel, std::string(tab.name) + " Map");
            mapPanel.size = {tabPanel.size.x - 40.0f, tabPanel.size.y - 40.0f};
            mapPanel.position = {20.0f, 20.0f};
            mapPanel.tint = {0.1f, 0.1f, 0.12f, 1.0f};
            AddChild(tabPanel.id, mapPanel.id);

            LayoutNode& mapLabel = CreateNode(NodeType::Text, "Map Label");
            mapLabel.text.value = "Map Placeholder";
            mapLabel.parentAnchorEnabled = true;
            mapLabel.parentAnchor = UIAnchorPreset::Center;
            AddChild(mapPanel.id, mapLabel.id);
        }

        first = false;
    }
}

void UIPrefabLayoutDesignerPanel::BuildScrollListTemplate() {
    ResetLayout();
    LayoutNode* root = FindNode(m_RootNodeId);
    if (!root) return;

    LayoutNode& list = CreateNode(NodeType::ScrollView, "Scrollable List");
    list.size = root->size;
    list.scroll.vertical = true;
    list.scroll.horizontal = false;
    list.scroll.markDynamicList = true;
    list.scroll.contentSizeMode = ContentSizeMode::EstimatedItems;
    list.scroll.estimatedItemCount = 16;
    list.scroll.estimatedItemSize = {root->size.x - 40.0f, 36.0f};
    list.scroll.includeExampleItem = true;
    AddChild(root->id, list.id);
}

void UIPrefabLayoutDesignerPanel::BuildSplitPanelTemplate() {
    ResetLayout();
    LayoutNode* root = FindNode(m_RootNodeId);
    if (!root) return;

    root->layout.enabled = true;
    root->layout.direction = LayoutGroupComponent::LayoutDirection::Horizontal;
    root->layout.padding = {12.0f, 12.0f, 12.0f, 12.0f};
    root->layout.spacing = 12.0f;

    LayoutNode& nav = CreateNode(NodeType::Panel, "Nav Panel");
    nav.size = {240.0f, root->size.y - 24.0f};
    nav.layout.enabled = true;
    nav.layout.direction = LayoutGroupComponent::LayoutDirection::Vertical;
    nav.layout.padding = {8.0f, 8.0f, 8.0f, 8.0f};
    nav.layout.spacing = 8.0f;
    nav.tint = {0.08f, 0.08f, 0.1f, 0.9f};
    AddChild(root->id, nav.id);

    const char* navItems[] = { "Inventory", "Equipment", "Quests", "Settings" };
    for (const char* item : navItems) {
        LayoutNode& btn = CreateNode(NodeType::Button, std::string(item) + " Button");
        btn.button.label = item;
        btn.size = {200.0f, 36.0f};
        AddChild(nav.id, btn.id);
    }

    LayoutNode& content = CreateNode(NodeType::Panel, "Content Panel");
    content.size = {root->size.x - nav.size.x - root->layout.spacing - root->layout.padding.x - root->layout.padding.z, root->size.y - 24.0f};
    content.tint = {0.06f, 0.06f, 0.08f, 0.9f};
    AddChild(root->id, content.id);
}

void UIPrefabLayoutDesignerPanel::DrawToolbar() {
    if (ImGui::Button("New Blank")) {
        ResetLayout();
    }
    ImGui::SameLine();
    if (ImGui::Button("Export Prefab")) {
        ExportPrefab();
    }
    ImGui::SameLine();
    if (ImGui::Button("Open Output Folder") && m_UILayer) {
        std::string dir = NormalizeSlashes(m_OutputDirectory);
        if (!dir.empty()) {
            m_UILayer->GetProjectPanel().NavigateTo(dir);
        }
    }
}

void UIPrefabLayoutDesignerPanel::DrawPrefabSettings() {
    if (!ImGui::CollapsingHeader("Prefab Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    ImGui::InputText("Prefab Name", m_PrefabName, sizeof(m_PrefabName));
    ImGui::InputText("Output Folder", m_OutputDirectory, sizeof(m_OutputDirectory));
    ImGui::Checkbox("Overwrite Existing", &m_OverwriteExisting);
    ImGui::Checkbox("Open Prefab Editor After Export", &m_OpenAfterExport);
    ImGui::Checkbox("Select Output Folder in Project Panel", &m_SelectInProject);

    std::string fileName = SanitizeName(m_PrefabName[0] ? m_PrefabName : "UI_Prefab");
    std::string outputDir = m_OutputDirectory[0] ? m_OutputDirectory : "assets/prefabs/UI";
    std::string vpath = NormalizeSlashes(outputDir + "/" + fileName + ".prefab");
    ImGui::Text("Output: %s", vpath.c_str());
}

void UIPrefabLayoutDesignerPanel::DrawCanvasSettings() {
    if (!ImGui::CollapsingHeader("Canvas Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    ImGui::InputInt("Canvas Width (0 = Screen)", &m_CanvasWidth);
    ImGui::InputInt("Canvas Height (0 = Screen)", &m_CanvasHeight);
    ImGui::InputInt("Reference Width", &m_CanvasReferenceWidth);
    ImGui::InputInt("Reference Height", &m_CanvasReferenceHeight);
    ImGui::DragFloat("DPI Scale", &m_CanvasDpiScale, 0.01f, 0.1f, 4.0f);
    ImGui::DragFloat("Canvas Opacity", &m_CanvasOpacity, 0.01f, 0.0f, 1.0f);
    ImGui::Checkbox("Block Scene Input", &m_CanvasBlockSceneInput);

    ImGui::Combo("Scale Mode", &m_CanvasScaleMode, kScaleModeLabels, IM_ARRAYSIZE(kScaleModeLabels));
    ImGui::Combo("Render Space", &m_CanvasSpace, kRenderSpaceLabels, IM_ARRAYSIZE(kRenderSpaceLabels));

    if (ImGui::Button("Match Root Size to Reference")) {
        LayoutNode* root = FindNode(m_RootNodeId);
        if (root) {
            root->size = GetDesignCanvasSize();
        }
    }
}

void UIPrefabLayoutDesignerPanel::DrawTemplateSettings() {
    if (!ImGui::CollapsingHeader("Layout Templates", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    int templateIndex = static_cast<int>(m_SelectedTemplate);
    if (ImGui::Combo("Template", &templateIndex, kTemplateLabels, IM_ARRAYSIZE(kTemplateLabels))) {
        m_SelectedTemplate = static_cast<TemplateType>(templateIndex);
    }
    if (ImGui::Button("Apply Template")) {
        BuildTemplate(m_SelectedTemplate);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset Layout")) {
        ResetLayout();
    }
}

void UIPrefabLayoutDesignerPanel::DrawLayoutTree() {
    ImGui::BeginChild("LayoutTree", ImVec2(0, 0), true);

    LayoutNode* selected = FindNode(m_SelectedNodeId);
    bool canAddChild = selected && CanHaveChildren(selected->type);

    ImGui::TextUnformatted("Layout Tree");
    ImGui::Separator();

    ImGui::BeginDisabled(!canAddChild);
    int typeIndex = m_AddNodeTypeIndex;
    ImGui::Combo("Add Child Type", &typeIndex, kNodeTypeLabels, IM_ARRAYSIZE(kNodeTypeLabels));
    m_AddNodeTypeIndex = typeIndex;
    if (ImGui::Button("Add Child")) {
        LayoutNode& node = CreateNode(static_cast<NodeType>(m_AddNodeTypeIndex), kNodeTypeLabels[m_AddNodeTypeIndex]);
        AddChild(selected->id, node.id);
        m_SelectedNodeId = node.id;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Duplicate") && selected && selected->id != m_RootNodeId) {
        int newId = DuplicateNodeRecursive(selected->id, selected->parentId);
        if (newId > 0) {
            m_SelectedNodeId = newId;
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Delete") && selected && selected->id != m_RootNodeId) {
        int parentId = selected->parentId;
        RemoveNodeRecursive(selected->id);
        m_SelectedNodeId = parentId > 0 ? parentId : m_RootNodeId;
    }

    ImGui::Separator();

    std::function<void(int)> drawNode = [&](int nodeId) {
        LayoutNode* node = FindNode(nodeId);
        if (!node) return;
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
        if (node->children.empty() || !CanHaveChildren(node->type)) {
            flags |= ImGuiTreeNodeFlags_Leaf;
        }
        if (nodeId == m_SelectedNodeId) {
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        std::string label = node->name + " (" + kNodeTypeLabels[static_cast<int>(node->type)] + ")";
        bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<intptr_t>(nodeId)), flags, "%s", label.c_str());
        if (ImGui::IsItemClicked()) {
            m_SelectedNodeId = nodeId;
        }
        if (open) {
            for (int childId : node->children) {
                drawNode(childId);
            }
            ImGui::TreePop();
        }
    };

    drawNode(m_RootNodeId);

    ImGui::EndChild();
}

void UIPrefabLayoutDesignerPanel::DrawNodeInspector() {
    ImGui::BeginChild("NodeInspector", ImVec2(0, 0), true);

    LayoutNode* node = FindNode(m_SelectedNodeId);
    if (!node) {
        ImGui::TextUnformatted("Select a node to edit.");
        ImGui::EndChild();
        return;
    }

    ImGui::Text("Selected: %s", node->name.c_str());
    ImGui::Separator();

    char nameBuffer[128];
    std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", node->name.c_str());
    if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer))) {
        node->name = nameBuffer;
    }

    ImGui::Text("Type: %s", kNodeTypeLabels[static_cast<int>(node->type)]);
    ImGui::Checkbox("Enabled", &node->enabled);
    ImGui::Checkbox("Visible", &node->visible);

    if (node->type != NodeType::Text) {
        ImGui::DragFloat2("Size", &node->size.x, 1.0f, 1.0f, 8192.0f);
    }

    if (node->type != NodeType::Text) {
        ImGui::DragFloat2("Position", &node->position.x, 1.0f, -4096.0f, 4096.0f);
        ImGui::DragFloat2("Pivot", &node->pivot.x, 0.01f, 0.0f, 1.0f);
    }

    if (node->type == NodeType::Text) {
        char textBuffer[256];
        std::snprintf(textBuffer, sizeof(textBuffer), "%s", node->text.value.c_str());
        if (ImGui::InputText("Text", textBuffer, sizeof(textBuffer))) {
            node->text.value = textBuffer;
        }
        ImGui::DragFloat("Pixel Size", &node->text.pixelSize, 0.5f, 8.0f, 128.0f);
        ImGui::Checkbox("Word Wrap", &node->text.wordWrap);
        ImGui::DragFloat2("Rect Size", &node->text.rectSize.x, 1.0f, 0.0f, 4096.0f);
    }

    if (node->type == NodeType::Button) {
        char labelBuffer[128];
        std::snprintf(labelBuffer, sizeof(labelBuffer), "%s", node->button.label.c_str());
        if (ImGui::InputText("Label", labelBuffer, sizeof(labelBuffer))) {
            node->button.label = labelBuffer;
        }
        ImGui::Checkbox("Toggle Button", &node->button.toggle);
        ImGui::DragFloat("Label Size", &node->text.pixelSize, 0.5f, 8.0f, 128.0f);
    }

    if (node->type == NodeType::Panel || node->type == NodeType::Image || node->type == NodeType::Button || node->type == NodeType::ScrollView || node->type == NodeType::Spacer) {
        ImGui::ColorEdit4("Tint", &node->tint.x);
        ImGui::DragFloat("Opacity", &node->opacity, 0.01f, 0.0f, 1.0f);
    }

    DrawAnchorSettings(*node, node->id != m_RootNodeId);

    if (CanEditLayout(node->type)) {
        DrawLayoutGroupSettings(node->layout);
        DrawFitToContentSettings(node->fit);
    }

    if (node->type == NodeType::ScrollView) {
        DrawScrollViewSettings(*node);
    }

    ImGui::Separator();

    if (node->id != m_RootNodeId) {
        if (ImGui::Button("Move Up")) {
            MoveNodeWithinParent(node->id, -1);
        }
        ImGui::SameLine();
        if (ImGui::Button("Move Down")) {
            MoveNodeWithinParent(node->id, 1);
        }
    }

    ImGui::EndChild();
}

void UIPrefabLayoutDesignerPanel::DrawValidationSummary() {
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    CollectValidationIssues(errors, warnings);

    if (!errors.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Validation Errors:");
        for (const auto& msg : errors) {
            ImGui::BulletText("%s", msg.c_str());
        }
    }

    if (!warnings.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Warnings:");
        for (const auto& msg : warnings) {
            ImGui::BulletText("%s", msg.c_str());
        }
    }
}

void UIPrefabLayoutDesignerPanel::DrawExportSection() {
    if (ImGui::Button("Export Prefab")) {
        ExportPrefab();
    }

    if (!m_LastExportStatus.empty()) {
        ImVec4 color = m_LastExportSuccess ? ImVec4(0.6f, 0.9f, 0.6f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
        ImGui::TextColored(color, "%s", m_LastExportStatus.c_str());
    }
}

void UIPrefabLayoutDesignerPanel::DrawLayoutGroupSettings(LayoutGroupSettings& settings, const char* header, bool showEnabledToggle) {
    if (!ImGui::CollapsingHeader(header)) {
        return;
    }

    if (showEnabledToggle) {
        ImGui::Checkbox("Enable Layout Group", &settings.enabled);
        ImGui::BeginDisabled(!settings.enabled);
    }
    int dirIndex = (settings.direction == LayoutGroupComponent::LayoutDirection::Horizontal) ? 0 : 1;
    if (ImGui::Combo("Direction", &dirIndex, kLayoutDirectionLabels, IM_ARRAYSIZE(kLayoutDirectionLabels))) {
        settings.direction = dirIndex == 0 ? LayoutGroupComponent::LayoutDirection::Horizontal : LayoutGroupComponent::LayoutDirection::Vertical;
    }
    ImGui::DragFloat4("Padding (L,T,R,B)", &settings.padding.x, 1.0f, 0.0f, 256.0f);
    ImGui::DragFloat("Spacing", &settings.spacing, 0.5f, 0.0f, 128.0f);
    int alignIndex = static_cast<int>(settings.childAlignment);
    if (ImGui::Combo("Child Alignment", &alignIndex, kAlignmentLabels, IM_ARRAYSIZE(kAlignmentLabels))) {
        settings.childAlignment = static_cast<LayoutGroupComponent::Alignment>(alignIndex);
    }
    int crossIndex = static_cast<int>(settings.crossAlignment);
    if (ImGui::Combo("Cross Alignment", &crossIndex, kAlignmentLabels, IM_ARRAYSIZE(kAlignmentLabels))) {
        settings.crossAlignment = static_cast<LayoutGroupComponent::Alignment>(crossIndex);
    }
    ImGui::Checkbox("Control Child Width", &settings.controlChildWidth);
    ImGui::Checkbox("Control Child Height", &settings.controlChildHeight);
    ImGui::Checkbox("Child Force Expand Width", &settings.childForceExpandWidth);
    ImGui::Checkbox("Child Force Expand Height", &settings.childForceExpandHeight);
    ImGui::Checkbox("Reverse Order", &settings.reverseOrder);
    ImGui::DragInt("Grid Columns", &settings.columns, 1.0f, 0, 24);
    ImGui::DragInt("Grid Rows", &settings.rows, 1.0f, 0, 24);
    ImGui::DragFloat2("Cell Size", &settings.cellSize.x, 1.0f, 0.0f, 2048.0f);
    if (showEnabledToggle) {
        ImGui::EndDisabled();
    }
}

void UIPrefabLayoutDesignerPanel::DrawFitToContentSettings(FitToContentSettings& settings) {
    if (!ImGui::CollapsingHeader("Fit To Content")) {
        return;
    }
    ImGui::Checkbox("Enable Fit To Content", &settings.enabled);
    ImGui::BeginDisabled(!settings.enabled);
    ImGui::Checkbox("Fit Width", &settings.fitWidth);
    ImGui::Checkbox("Fit Height", &settings.fitHeight);
    ImGui::DragFloat4("Padding (L,T,R,B)", &settings.padding.x, 1.0f, 0.0f, 256.0f);
    ImGui::DragFloat2("Min Size", &settings.minSize.x, 1.0f, 0.0f, 4096.0f);
    ImGui::DragFloat2("Max Size", &settings.maxSize.x, 1.0f, 0.0f, 4096.0f);
    ImGui::Checkbox("Direct Children Only", &settings.directChildrenOnly);
    ImGui::EndDisabled();
}

void UIPrefabLayoutDesignerPanel::DrawScrollViewSettings(LayoutNode& node) {
    if (!ImGui::CollapsingHeader("Scroll View")) {
        return;
    }

    ImGui::Checkbox("Vertical Scroll", &node.scroll.vertical);
    ImGui::Checkbox("Horizontal Scroll", &node.scroll.horizontal);
    ImGui::Checkbox("Show Scrollbars", &node.scroll.showScrollbars);
    ImGui::DragFloat("Scroll Sensitivity", &node.scroll.scrollSensitivity, 0.5f, 1.0f, 120.0f);
    ImGui::DragFloat("Scrollbar Width", &node.scroll.scrollbarWidth, 0.5f, 4.0f, 64.0f);

    int modeIndex = static_cast<int>(node.scroll.contentSizeMode);
    if (ImGui::Combo("Content Size", &modeIndex, kContentSizeLabels, IM_ARRAYSIZE(kContentSizeLabels))) {
        node.scroll.contentSizeMode = static_cast<ContentSizeMode>(modeIndex);
    }

    if (node.scroll.contentSizeMode == ContentSizeMode::Manual) {
        ImGui::DragFloat2("Manual Size", &node.scroll.manualContentSize.x, 1.0f, 0.0f, 8192.0f);
    }
    if (node.scroll.contentSizeMode == ContentSizeMode::EstimatedItems) {
        ImGui::DragInt("Estimated Count", &node.scroll.estimatedItemCount, 1.0f, 0, 999);
        ImGui::DragFloat2("Estimated Item Size", &node.scroll.estimatedItemSize.x, 1.0f, 1.0f, 4096.0f);
        ImGui::DragFloat("Estimated Spacing", &node.scroll.estimatedSpacing, 0.5f, 0.0f, 128.0f);
        ImGui::DragFloat4("Estimated Padding", &node.scroll.estimatedPadding.x, 1.0f, 0.0f, 256.0f);
    }

    ImGui::Checkbox("Content Layout Group", &node.scroll.contentLayoutEnabled);
    if (node.scroll.contentLayoutEnabled) {
        DrawLayoutGroupSettings(node.scroll.contentLayout, "Content Layout", false);
    }

    ImGui::Checkbox("Mark As Dynamic List", &node.scroll.markDynamicList);
    ImGui::Checkbox("Include Example Items", &node.scroll.includeExampleItem);
    if (node.scroll.includeExampleItem) {
        ImGui::DragInt("Example Item Count", &node.scroll.exampleItemCount, 1.0f, 1, 20);
        ImGui::Checkbox("Example Items Visible", &node.scroll.exampleItemVisible);
    }
}

void UIPrefabLayoutDesignerPanel::DrawAnchorSettings(LayoutNode& node, bool allowParentAnchor) {
    if (!ImGui::CollapsingHeader("Anchoring")) {
        return;
    }

    ImGui::Checkbox("Screen Anchor", &node.anchorEnabled);
    if (node.anchorEnabled) {
        int anchorIndex = static_cast<int>(node.anchor);
        if (ImGui::Combo("Anchor Preset", &anchorIndex, kAnchorLabels, IM_ARRAYSIZE(kAnchorLabels))) {
            node.anchor = static_cast<UIAnchorPreset>(anchorIndex);
        }
        ImGui::DragFloat2("Anchor Offset", &node.anchorOffset.x, 1.0f, -4096.0f, 4096.0f);
    }

    if (allowParentAnchor) {
        ImGui::Checkbox("Parent Anchor", &node.parentAnchorEnabled);
        if (node.parentAnchorEnabled) {
            int parentIndex = static_cast<int>(node.parentAnchor);
            if (ImGui::Combo("Parent Anchor Preset", &parentIndex, kAnchorLabels, IM_ARRAYSIZE(kAnchorLabels))) {
                node.parentAnchor = static_cast<UIAnchorPreset>(parentIndex);
            }
            ImGui::DragFloat2("Parent Offset", &node.parentAnchorOffset.x, 1.0f, -4096.0f, 4096.0f);
        }
    }
}

UIPrefabLayoutDesignerPanel::LayoutNode& UIPrefabLayoutDesignerPanel::CreateNode(NodeType type, const std::string& baseName) {
    LayoutNode node;
    node.id = m_NextNodeId++;
    node.type = type;
    node.name = baseName;

    switch (type) {
    case NodeType::Button:
        node.size = {160.0f, 36.0f};
        node.button.label = "Button";
        node.text.pixelSize = 24.0f;
        break;
    case NodeType::Text:
        node.size = {240.0f, 32.0f};
        node.text.value = "Label";
        node.text.pixelSize = 24.0f;
        break;
    case NodeType::ScrollView:
        node.size = {480.0f, 320.0f};
        node.scroll.contentLayoutEnabled = true;
        node.scroll.contentLayout.direction = LayoutGroupComponent::LayoutDirection::Vertical;
        node.scroll.contentLayout.spacing = 6.0f;
        node.scroll.contentLayout.padding = {10.0f, 10.0f, 10.0f, 10.0f};
        node.scroll.contentLayout.controlChildWidth = true;
        node.scroll.contentLayout.childForceExpandWidth = true;
        node.scroll.contentSizeMode = ContentSizeMode::Viewport;
        break;
    case NodeType::Spacer:
        node.size = {40.0f, 16.0f};
        node.tint = {1.0f, 1.0f, 1.0f, 0.0f};
        node.opacity = 0.0f;
        break;
    default:
        node.size = {240.0f, 120.0f};
        break;
    }

    auto [it, inserted] = m_Nodes.emplace(node.id, std::move(node));
    if (!inserted) {
        return it->second;
    }
    return it->second;
}

bool UIPrefabLayoutDesignerPanel::CanHaveChildren(NodeType type) const {
    return type == NodeType::Panel || type == NodeType::Image || type == NodeType::ScrollView;
}

bool UIPrefabLayoutDesignerPanel::CanEditLayout(NodeType type) const {
    return type == NodeType::Panel || type == NodeType::Image || type == NodeType::ScrollView;
}

int UIPrefabLayoutDesignerPanel::AddChild(int parentId, int childId) {
    LayoutNode* parent = FindNode(parentId);
    LayoutNode* child = FindNode(childId);
    if (!parent || !child) return -1;
    if (!CanHaveChildren(parent->type)) return -1;
    child->parentId = parentId;
    child->name = MakeUniqueName(parentId, child->name);
    parent->children.push_back(childId);
    return childId;
}

void UIPrefabLayoutDesignerPanel::RemoveNodeRecursive(int nodeId) {
    LayoutNode* node = FindNode(nodeId);
    if (!node) return;
    std::vector<int> children = node->children;
    for (int childId : children) {
        RemoveNodeRecursive(childId);
    }
    if (node->parentId != -1) {
        LayoutNode* parent = FindNode(node->parentId);
        if (parent) {
            parent->children.erase(std::remove(parent->children.begin(), parent->children.end(), nodeId), parent->children.end());
        }
    }
    m_Nodes.erase(nodeId);
}

int UIPrefabLayoutDesignerPanel::DuplicateNodeRecursive(int nodeId, int parentId) {
    LayoutNode* source = FindNode(nodeId);
    if (!source) return -1;
    LayoutNode copy = *source;
    copy.id = m_NextNodeId++;
    copy.parentId = parentId;
    copy.children.clear();
    copy.name = MakeUniqueName(parentId, copy.name);

    auto [it, inserted] = m_Nodes.emplace(copy.id, std::move(copy));
    if (!inserted) return -1;

    LayoutNode* newNode = &it->second;
    for (int childId : source->children) {
        int newChildId = DuplicateNodeRecursive(childId, newNode->id);
        if (newChildId > 0) {
            newNode->children.push_back(newChildId);
        }
    }

    LayoutNode* parent = FindNode(parentId);
    if (parent) {
        parent->children.push_back(newNode->id);
    }
    return newNode->id;
}

void UIPrefabLayoutDesignerPanel::MoveNodeWithinParent(int nodeId, int direction) {
    LayoutNode* node = FindNode(nodeId);
    if (!node || node->parentId == -1) return;
    LayoutNode* parent = FindNode(node->parentId);
    if (!parent) return;

    auto it = std::find(parent->children.begin(), parent->children.end(), nodeId);
    if (it == parent->children.end()) return;
    auto idx = static_cast<int>(std::distance(parent->children.begin(), it));
    int newIdx = idx + direction;
    if (newIdx < 0 || newIdx >= static_cast<int>(parent->children.size())) return;
    std::swap(parent->children[idx], parent->children[newIdx]);
}

UIPrefabLayoutDesignerPanel::LayoutNode* UIPrefabLayoutDesignerPanel::FindNode(int nodeId) {
    auto it = m_Nodes.find(nodeId);
    if (it == m_Nodes.end()) return nullptr;
    return &it->second;
}

const UIPrefabLayoutDesignerPanel::LayoutNode* UIPrefabLayoutDesignerPanel::FindNode(int nodeId) const {
    auto it = m_Nodes.find(nodeId);
    if (it == m_Nodes.end()) return nullptr;
    return &it->second;
}

bool UIPrefabLayoutDesignerPanel::IsDescendant(int nodeId, int ancestorId) const {
    const LayoutNode* node = FindNode(nodeId);
    while (node && node->parentId != -1) {
        if (node->parentId == ancestorId) return true;
        node = FindNode(node->parentId);
    }
    return false;
}

std::string UIPrefabLayoutDesignerPanel::SanitizeName(const std::string& input) const {
    if (input.empty()) return "Prefab";
    std::string out = input;
    const std::string invalid = "<>:\"/\\|?*";
    for (char& c : out) {
        if (invalid.find(c) != std::string::npos) c = '_';
    }
    size_t start = out.find_first_not_of(' ');
    size_t end = out.find_last_not_of(' ');
    if (start == std::string::npos) return "Prefab";
    return out.substr(start, end - start + 1);
}

std::string UIPrefabLayoutDesignerPanel::MakeUniqueName(int parentId, const std::string& baseName) const {
    const std::string base = baseName.empty() ? "Node" : baseName;
    std::string candidate = base;
    int counter = 1;
    auto isUsed = [&](const std::string& name) -> bool {
        const LayoutNode* parent = FindNode(parentId);
        if (!parent) return false;
        for (int childId : parent->children) {
            const LayoutNode* child = FindNode(childId);
            if (child && child->name == name) return true;
        }
        return false;
    };
    while (isUsed(candidate)) {
        candidate = base + "_" + std::to_string(counter++);
    }
    return candidate;
}

glm::vec2 UIPrefabLayoutDesignerPanel::GetDesignCanvasSize() const {
    float width = m_CanvasReferenceWidth > 0 ? static_cast<float>(m_CanvasReferenceWidth) : 1920.0f;
    float height = m_CanvasReferenceHeight > 0 ? static_cast<float>(m_CanvasReferenceHeight) : 1080.0f;
    return {width, height};
}

bool UIPrefabLayoutDesignerPanel::ExportPrefab() {
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    CollectValidationIssues(errors, warnings);
    if (!errors.empty()) {
        m_LastExportSuccess = false;
        m_LastExportStatus = "Fix validation errors before exporting.";
        return false;
    }

    std::string prefabName = SanitizeName(m_PrefabName[0] ? m_PrefabName : "UI_Prefab");
    std::string outputFolder = m_OutputDirectory[0] ? m_OutputDirectory : "assets/prefabs/UI";
    outputFolder = NormalizeSlashes(outputFolder);

    fs::path outputDir = outputFolder;
    if (outputDir.is_relative()) {
        outputDir = Project::GetProjectDirectory() / outputDir;
    }
    std::error_code ec;
    fs::create_directories(outputDir, ec);

    fs::path outputPath = outputDir / (prefabName + ".prefab");
    if (!m_OverwriteExisting) {
        int counter = 1;
        while (fs::exists(outputPath)) {
            outputPath = outputDir / (prefabName + "_" + std::to_string(counter++) + ".prefab");
        }
    }

    Scene buildScene;
    EntityID rootEntity = INVALID_ENTITY_ID;
    if (!BuildPrefabScene(buildScene, rootEntity)) {
        m_LastExportSuccess = false;
        m_LastExportStatus = "Failed to build prefab scene.";
        return false;
    }

    PrefabAsset asset;
    if (!prefab_editor::BuildPrefabAssetFromScene(buildScene, rootEntity, asset)) {
        m_LastExportSuccess = false;
        m_LastExportStatus = "Failed to serialize prefab.";
        return false;
    }

    // Align prefab GUID with existing meta if present
    ClaymoreGUID prefabGuid = asset.Guid;
    fs::path metaPath = outputPath; metaPath += ".meta";
    if (fs::exists(metaPath)) {
        try {
            std::ifstream in(metaPath.string());
            if (in) {
                nlohmann::json j; in >> j; in.close();
                AssetMetadata meta = j.get<AssetMetadata>();
                if (meta.guid.high != 0 || meta.guid.low != 0) {
                    prefabGuid = meta.guid;
                }
            }
        } catch (...) {
        }
    } else {
        prefabGuid = ClaymoreGUID::Generate();
    }

    asset.Guid = prefabGuid;
    asset.Name = prefabName;
    if (asset.Raw.is_object()) {
        asset.Raw["guid"] = prefabGuid;
        asset.Raw["name"] = prefabName;
    }

    if (!PrefabIO::SavePrefab(outputPath.string(), asset)) {
        m_LastExportSuccess = false;
        m_LastExportStatus = "Failed to write prefab file.";
        return false;
    }

    AssetMetadata meta;
    try {
        if (fs::exists(metaPath)) {
            std::ifstream in(metaPath.string());
            if (in) { nlohmann::json j; in >> j; in.close(); meta = j.get<AssetMetadata>(); }
        }
        meta.guid = prefabGuid;
        meta.type = "prefab";
        meta.sourcePath = outputPath.string();
        meta.processedPath = outputPath.string();
        meta.reference = AssetReference(prefabGuid, 0, static_cast<int32_t>(AssetType::Prefab));
        nlohmann::json j = meta;
        std::ofstream out(metaPath.string());
        out << j.dump(4);
    } catch (...) {
    }

    // Register asset in the library
    try {
        std::string name = outputPath.filename().string();
        std::error_code ecRel;
        fs::path rel = fs::relative(outputPath, Project::GetProjectDirectory(), ecRel);
        std::string vpath = NormalizeSlashes(ecRel ? outputPath.string() : rel.string());
        size_t pos = vpath.find("assets/");
        if (pos != std::string::npos) {
            vpath = vpath.substr(pos);
        }
        AssetReference ref(prefabGuid, 0, static_cast<int32_t>(AssetType::Prefab));
        AssetLibrary::Instance().RegisterAsset(ref, AssetType::Prefab, vpath, name);
        AssetLibrary::Instance().RegisterPathAlias(prefabGuid, outputPath.string());
        AssetRegistry::Instance().SetMetadata(outputPath.string(), meta);
    } catch (...) {
    }

    try {
        AssetPipeline::Instance().HotSwapPrefabInScene(outputPath.string());
    } catch (...) {
    }

    m_LastExportSuccess = true;
    m_LastExportPath = outputPath.string();
    m_LastExportStatus = "Prefab exported to " + m_LastExportPath;

    if (m_OpenAfterExport && m_UILayer) {
        m_UILayer->OpenPrefabEditor(m_LastExportPath);
    }
    if (m_SelectInProject && m_UILayer) {
        std::string vpath = NormalizeSlashes(m_OutputDirectory);
        if (!vpath.empty()) {
            m_UILayer->GetProjectPanel().NavigateTo(vpath);
        }
    }

    return true;
}

bool UIPrefabLayoutDesignerPanel::BuildPrefabScene(Scene& scene, EntityID& outRoot) {
    LayoutNode* rootNode = FindNode(m_RootNodeId);
    if (!rootNode) return false;

    std::string canvasName = SanitizeName(m_PrefabName[0] ? m_PrefabName : "UI_Prefab");
    Entity canvasEntity = scene.CreateEntityExact(canvasName);
    EntityData* canvasData = scene.GetEntityData(canvasEntity.GetID());
    if (!canvasData) return false;

    canvasData->Canvas = std::make_unique<CanvasComponent>();
    canvasData->Canvas->Width = m_CanvasWidth;
    canvasData->Canvas->Height = m_CanvasHeight;
    canvasData->Canvas->DPIScale = m_CanvasDpiScale;
    canvasData->Canvas->Space = static_cast<CanvasComponent::RenderSpace>(m_CanvasSpace);
    canvasData->Canvas->SortOrder = m_CanvasSortOrder;
    canvasData->Canvas->Opacity = m_CanvasOpacity;
    canvasData->Canvas->BlockSceneInput = m_CanvasBlockSceneInput;
    canvasData->Canvas->ReferenceWidth = m_CanvasReferenceWidth;
    canvasData->Canvas->ReferenceHeight = m_CanvasReferenceHeight;
    canvasData->Canvas->ReferenceScaleMode = static_cast<CanvasComponent::ScaleMode>(m_CanvasScaleMode);

    EntityID rootId = BuildNodeRecursive(scene, rootNode->id, canvasEntity.GetID());
    if (rootId == INVALID_ENTITY_ID) return false;

    outRoot = canvasEntity.GetID();
    return true;
}

EntityID UIPrefabLayoutDesignerPanel::BuildNodeRecursive(Scene& scene, int nodeId, EntityID parentId) {
    LayoutNode* node = FindNode(nodeId);
    if (!node) return INVALID_ENTITY_ID;

    std::string name = node->name.empty() ? kNodeTypeLabels[static_cast<int>(node->type)] : node->name;
    Entity entity = scene.CreateEntityExact(name);
    EntityID id = entity.GetID();
    EntityData* data = scene.GetEntityData(id);
    if (!data) return INVALID_ENTITY_ID;

    data->Active = node->enabled;
    data->Visible = node->visible;

    if (parentId != INVALID_ENTITY_ID) {
        scene.SetParentFast(id, parentId);
    }

    auto addPanel = [&]() {
        data->Panel = std::make_unique<PanelComponent>();
        ApplyPanelSettings(*data->Panel, *node);
    };

    bool handledChildren = false;
    switch (node->type) {
    case NodeType::Panel:
    case NodeType::Image:
    case NodeType::Spacer:
        addPanel();
        if (node->type == NodeType::Spacer && data->Panel) {
            data->Panel->TintColor.a = 0.0f;
            data->Panel->Opacity = 0.0f;
        }
        break;
    case NodeType::Button:
        addPanel();
        data->Button = std::make_unique<ButtonComponent>();
        data->Button->Toggle = node->button.toggle;
        BuildButtonLabel(scene, id, *node);
        break;
    case NodeType::Text:
        data->Text = std::make_unique<TextRendererComponent>();
        data->Text->Text = node->text.value;
        data->Text->PixelSize = node->text.pixelSize;
        data->Text->WordWrap = node->text.wordWrap;
        data->Text->RectSize = node->text.rectSize;
        data->Text->WorldSpace = false;
        data->Text->AnchorToParentUI = node->parentAnchorEnabled;
        data->Text->AnchorEnabled = node->anchorEnabled;
        data->Text->Anchor = node->anchor;
        data->Text->AnchorOffset = node->anchorOffset;
        data->Transform.Position.x = node->position.x;
        data->Transform.Position.y = node->position.y;
        break;
    case NodeType::ScrollView:
        addPanel();
        data->ScrollView = std::make_unique<ScrollViewComponent>();
        {
            glm::vec2 contentSize = ComputeScrollContentSize(*node);
            ApplyScrollSettings(*data->ScrollView, node->scroll, contentSize);
            EntityID contentId = BuildScrollContentContainer(scene, id, *node, contentSize);
            for (int childId : node->children) {
                BuildNodeRecursive(scene, childId, contentId);
            }
            BuildExampleListItems(scene, contentId, *node);
            handledChildren = true;
        }
    default:
        break;
    }

    if (node->layout.enabled) {
        data->LayoutGroup = std::make_unique<LayoutGroupComponent>();
        ApplyLayoutSettings(*data->LayoutGroup, node->layout);
    }

    if (node->fit.enabled) {
        data->FitToContent = std::make_unique<FitToContentComponent>();
        ApplyFitToContentSettings(*data->FitToContent, node->fit);
    }

    if (node->parentAnchorEnabled) {
        data->UIRect = std::make_unique<UIRectComponent>();
        data->UIRect->AnchorToParent = true;
        AnchorPresetToNormalized(node->parentAnchor, data->UIRect->HorizontalAnchor, data->UIRect->VerticalAnchor);
        data->UIRect->Pivot = node->pivot;
        data->UIRect->Offset = node->parentAnchorOffset;
        if (node->type == NodeType::Text) {
            data->UIRect->Size = node->text.rectSize;
        }
    }

    if (!handledChildren) {
        for (int childId : node->children) {
            BuildNodeRecursive(scene, childId, id);
        }
    }

    return id;
}

EntityID UIPrefabLayoutDesignerPanel::BuildButtonLabel(Scene& scene, EntityID parentId, const LayoutNode& node) {
    if (node.button.label.empty()) return INVALID_ENTITY_ID;
    Entity label = scene.CreateEntityExact(node.button.label);
    EntityID labelId = label.GetID();
    scene.SetParentFast(labelId, parentId);
    EntityData* labelData = scene.GetEntityData(labelId);
    if (!labelData) return INVALID_ENTITY_ID;
    labelData->Text = std::make_unique<TextRendererComponent>();
    labelData->Text->Text = node.button.label;
    labelData->Text->PixelSize = node.text.pixelSize;
    labelData->Text->WorldSpace = false;
    labelData->Text->AnchorToParentUI = true;
    labelData->Text->Visible = node.visible;
    labelData->Visible = node.visible;
    labelData->Active = node.enabled;
    return labelId;
}

EntityID UIPrefabLayoutDesignerPanel::BuildScrollContentContainer(Scene& scene, EntityID scrollId, const LayoutNode& node, glm::vec2 contentSize) {
    Entity content = scene.CreateEntityExact(node.name + " Content");
    EntityID contentId = content.GetID();
    scene.SetParentFast(contentId, scrollId);
    EntityData* contentData = scene.GetEntityData(contentId);
    if (!contentData) return INVALID_ENTITY_ID;

    contentData->Panel = std::make_unique<PanelComponent>();
    contentData->Panel->Position = {0.0f, 0.0f};
    contentData->Panel->Size = contentSize;
    contentData->Panel->TintColor = {1.0f, 1.0f, 1.0f, 0.0f};
    contentData->Panel->Opacity = 0.0f;
    contentData->Panel->Visible = true;

    contentData->UIRect = std::make_unique<UIRectComponent>();
    contentData->UIRect->AnchorToParent = true;
    contentData->UIRect->HorizontalAnchor = 0.0f;
    contentData->UIRect->VerticalAnchor = 0.0f;
    contentData->UIRect->Pivot = {0.0f, 0.0f};
    contentData->UIRect->Offset = {0.0f, 0.0f};

    if (node.scroll.contentLayoutEnabled) {
        contentData->LayoutGroup = std::make_unique<LayoutGroupComponent>();
        ApplyLayoutSettings(*contentData->LayoutGroup, node.scroll.contentLayout);
    }

    if (node.scroll.markDynamicList) {
        contentData->Groups.push_back("UI.DynamicList.Content");
    }

    return contentId;
}

void UIPrefabLayoutDesignerPanel::BuildExampleListItems(Scene& scene, EntityID contentId, const LayoutNode& node) {
    if (!node.scroll.includeExampleItem) return;
    if (node.scroll.exampleItemCount <= 0) return;

    for (int i = 0; i < node.scroll.exampleItemCount; ++i) {
        LayoutNode temp;
        temp.type = NodeType::Button;
        temp.name = "Example Item";
        temp.size = node.scroll.estimatedItemSize;
        temp.button.label = "Example Item";
        temp.text.pixelSize = node.text.pixelSize;
        temp.visible = node.scroll.exampleItemVisible;
        temp.enabled = node.scroll.exampleItemVisible;

        std::string itemName = temp.name + " " + std::to_string(i + 1);
        temp.name = itemName;

        LayoutNode* shadow = &temp;
        std::string name = shadow->name;
        Entity entity = scene.CreateEntityExact(name);
        EntityID id = entity.GetID();
        EntityData* data = scene.GetEntityData(id);
        if (!data) continue;

        scene.SetParentFast(id, contentId);
        data->Active = shadow->enabled;
        data->Visible = shadow->visible;

        data->Panel = std::make_unique<PanelComponent>();
        ApplyPanelSettings(*data->Panel, *shadow);
        data->Button = std::make_unique<ButtonComponent>();
        data->Button->Toggle = false;
        BuildButtonLabel(scene, id, *shadow);
    }
}

glm::vec2 UIPrefabLayoutDesignerPanel::ComputeScrollContentSize(const LayoutNode& node) const {
    switch (node.scroll.contentSizeMode) {
    case ContentSizeMode::Manual:
        return node.scroll.manualContentSize;
    case ContentSizeMode::EstimatedItems:
        return ComputeContentSizeFromEstimate(node);
    case ContentSizeMode::FromChildren:
        return ComputeContentSizeFromChildren(node);
    case ContentSizeMode::Viewport:
    default:
        return node.size;
    }
}

glm::vec2 UIPrefabLayoutDesignerPanel::ComputeContentSizeFromChildren(const LayoutNode& node) const {
    const LayoutNode* self = &node;
    if (!self) return node.size;
    if (self->children.empty()) return node.size;

    float width = 0.0f;
    float height = 0.0f;
    const LayoutGroupSettings& layout = node.scroll.contentLayout;
    const bool horizontal = layout.direction == LayoutGroupComponent::LayoutDirection::Horizontal;
    int count = 0;
    for (int childId : self->children) {
        const LayoutNode* child = FindNode(childId);
        if (!child) continue;
        ++count;
        if (horizontal) {
            width += child->size.x;
            height = std::max(height, child->size.y);
        } else {
            height += child->size.y;
            width = std::max(width, child->size.x);
        }
    }
    if (count > 1) {
        if (horizontal) width += layout.spacing * (count - 1);
        else height += layout.spacing * (count - 1);
    }
    width += layout.padding.x + layout.padding.z;
    height += layout.padding.y + layout.padding.w;
    return {std::max(width, node.size.x), std::max(height, node.size.y)};
}

glm::vec2 UIPrefabLayoutDesignerPanel::ComputeContentSizeFromEstimate(const LayoutNode& node) const {
    int count = std::max(0, node.scroll.estimatedItemCount);
    glm::vec2 item = node.scroll.estimatedItemSize;
    glm::vec4 pad = node.scroll.estimatedPadding;
    float spacing = node.scroll.estimatedSpacing;
    bool horizontal = node.scroll.contentLayout.direction == LayoutGroupComponent::LayoutDirection::Horizontal;

    if (count == 0) {
        return node.size;
    }

    float width = 0.0f;
    float height = 0.0f;
    if (horizontal) {
        width = item.x * count + spacing * (count - 1);
        height = item.y;
    } else {
        width = item.x;
        height = item.y * count + spacing * (count - 1);
    }
    width += pad.x + pad.z;
    height += pad.y + pad.w;
    return {std::max(width, node.size.x), std::max(height, node.size.y)};
}

void UIPrefabLayoutDesignerPanel::ApplyPanelSettings(PanelComponent& panel, const LayoutNode& node) const {
    panel.Position = node.position;
    panel.Size = node.size;
    panel.Pivot = node.pivot;
    panel.AnchorEnabled = node.anchorEnabled;
    panel.Anchor = node.anchor;
    panel.AnchorOffset = node.anchorOffset;
    panel.TintColor = node.tint;
    panel.Opacity = node.opacity;
    panel.Visible = node.visible;
}

void UIPrefabLayoutDesignerPanel::ApplyLayoutSettings(LayoutGroupComponent& layout, const LayoutGroupSettings& settings) const {
    layout.Direction = settings.direction;
    layout.Padding = settings.padding;
    layout.Spacing = settings.spacing;
    layout.ChildAlignment = settings.childAlignment;
    layout.CrossAlignment = settings.crossAlignment;
    layout.ControlChildWidth = settings.controlChildWidth;
    layout.ControlChildHeight = settings.controlChildHeight;
    layout.ChildForceExpandWidth = settings.childForceExpandWidth;
    layout.ChildForceExpandHeight = settings.childForceExpandHeight;
    layout.ReverseOrder = settings.reverseOrder;
    layout.Columns = settings.columns;
    layout.Rows = settings.rows;
    layout.CellSize = settings.cellSize;
}

void UIPrefabLayoutDesignerPanel::ApplyFitToContentSettings(FitToContentComponent& fit, const FitToContentSettings& settings) const {
    fit.Enabled = settings.enabled;
    fit.FitWidth = settings.fitWidth;
    fit.FitHeight = settings.fitHeight;
    fit.Padding = settings.padding;
    fit.MinSize = settings.minSize;
    fit.MaxSize = settings.maxSize;
    fit.DirectChildrenOnly = settings.directChildrenOnly;
}

void UIPrefabLayoutDesignerPanel::ApplyScrollSettings(ScrollViewComponent& scroll, const ScrollViewSettings& settings, const glm::vec2& contentSize) const {
    scroll.HorizontalScroll = settings.horizontal;
    scroll.VerticalScroll = settings.vertical;
    scroll.ScrollSensitivity = settings.scrollSensitivity;
    scroll.ShowScrollbars = settings.showScrollbars;
    scroll.ScrollbarWidth = settings.scrollbarWidth;
    scroll.ContentSize = contentSize;
}

void UIPrefabLayoutDesignerPanel::CollectValidationIssues(std::vector<std::string>& errors, std::vector<std::string>& warnings) const {
    if (std::string(m_PrefabName).empty()) {
        errors.push_back("Prefab name is empty.");
    }
    const LayoutNode* root = FindNode(m_RootNodeId);
    if (!root) {
        errors.push_back("Root node is missing.");
        return;
    }
    if (root->size.x <= 0.0f || root->size.y <= 0.0f) {
        errors.push_back("Root size must be greater than zero.");
    }
    for (const auto& [id, node] : m_Nodes) {
        if (node.type != NodeType::Text && (node.size.x <= 0.0f || node.size.y <= 0.0f)) {
            warnings.push_back(node.name + ": size is non-positive.");
        }
        if (node.type == NodeType::Text && node.text.wordWrap && node.text.rectSize.x <= 0.0f) {
            warnings.push_back(node.name + ": word wrap requires a width.");
        }
        if (node.type == NodeType::ScrollView) {
            if (!node.scroll.horizontal && !node.scroll.vertical) {
                warnings.push_back(node.name + ": scroll view has both axes disabled.");
            }
            if (node.scroll.contentSizeMode == ContentSizeMode::Manual &&
                (node.scroll.manualContentSize.x <= 0.0f || node.scroll.manualContentSize.y <= 0.0f)) {
                warnings.push_back(node.name + ": manual content size is empty.");
            }
            if (node.scroll.contentSizeMode == ContentSizeMode::EstimatedItems && node.scroll.estimatedItemCount <= 0) {
                warnings.push_back(node.name + ": estimated item count is zero.");
            }
        }
    }
}
