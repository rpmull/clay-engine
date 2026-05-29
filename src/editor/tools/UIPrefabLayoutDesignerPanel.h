#pragma once

#include "editor/ui/panels/EditorPanel.h"
#include "core/ecs/Entity.h"
#include "core/ecs/UIComponents.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>
#include <unordered_map>

class UILayer;

class UIPrefabLayoutDesignerPanel : public EditorPanel {
public:
    UIPrefabLayoutDesignerPanel(Scene* scene, UILayer* uiLayer);

    void OnImGuiRender();
    void Open();
    void SetOpen(bool open) { m_Open = open; }
    bool IsOpen() const { return m_Open; }

private:
    enum class NodeType {
        Panel = 0,
        Button,
        Text,
        Image,
        ScrollView,
        Spacer
    };

    enum class TemplateType {
        Blank = 0,
        TabbedMenu,
        ScrollList,
        SplitPanel
    };

    enum class ContentSizeMode {
        Viewport = 0,
        Manual,
        EstimatedItems,
        FromChildren
    };

    struct LayoutGroupSettings {
        bool enabled = false;
        LayoutGroupComponent::LayoutDirection direction = LayoutGroupComponent::LayoutDirection::Vertical;
        glm::vec4 padding = {10.0f, 10.0f, 10.0f, 10.0f};
        float spacing = 5.0f;
        LayoutGroupComponent::Alignment childAlignment = LayoutGroupComponent::Alignment::Start;
        LayoutGroupComponent::Alignment crossAlignment = LayoutGroupComponent::Alignment::Start;
        bool controlChildWidth = false;
        bool controlChildHeight = false;
        bool childForceExpandWidth = false;
        bool childForceExpandHeight = false;
        bool reverseOrder = false;
        int columns = 0;
        int rows = 0;
        glm::vec2 cellSize = {100.0f, 100.0f};
    };

    struct FitToContentSettings {
        bool enabled = false;
        bool fitWidth = true;
        bool fitHeight = true;
        glm::vec4 padding = {10.0f, 10.0f, 10.0f, 10.0f};
        glm::vec2 minSize = {0.0f, 0.0f};
        glm::vec2 maxSize = {0.0f, 0.0f};
        bool directChildrenOnly = true;
    };

    struct TextSettings {
        std::string value = "Label";
        float pixelSize = 24.0f;
        bool wordWrap = false;
        glm::vec2 rectSize = {0.0f, 0.0f};
    };

    struct ButtonSettings {
        std::string label = "Button";
        bool toggle = false;
    };

    struct ScrollViewSettings {
        bool horizontal = false;
        bool vertical = true;
        bool showScrollbars = true;
        float scrollSensitivity = 30.0f;
        float scrollbarWidth = 12.0f;

        ContentSizeMode contentSizeMode = ContentSizeMode::Viewport;
        glm::vec2 manualContentSize = {0.0f, 0.0f};
        int estimatedItemCount = 8;
        glm::vec2 estimatedItemSize = {240.0f, 36.0f};
        float estimatedSpacing = 6.0f;
        glm::vec4 estimatedPadding = {10.0f, 10.0f, 10.0f, 10.0f};

        bool contentLayoutEnabled = true;
        LayoutGroupSettings contentLayout;
        bool markDynamicList = false;
        bool includeExampleItem = false;
        bool exampleItemVisible = true;
        int exampleItemCount = 1;
    };

    struct LayoutNode {
        int id = 0;
        int parentId = -1;
        NodeType type = NodeType::Panel;
        std::string name;
        bool enabled = true;
        bool visible = true;

        glm::vec2 size = {200.0f, 100.0f};
        glm::vec2 position = {0.0f, 0.0f};
        glm::vec2 pivot = {0.5f, 0.5f};

        bool anchorEnabled = false;
        UIAnchorPreset anchor = UIAnchorPreset::TopLeft;
        glm::vec2 anchorOffset = {0.0f, 0.0f};

        bool parentAnchorEnabled = false;
        UIAnchorPreset parentAnchor = UIAnchorPreset::TopLeft;
        glm::vec2 parentAnchorOffset = {0.0f, 0.0f};

        glm::vec4 tint = {1.0f, 1.0f, 1.0f, 1.0f};
        float opacity = 1.0f;

        LayoutGroupSettings layout;
        FitToContentSettings fit;
        TextSettings text;
        ButtonSettings button;
        ScrollViewSettings scroll;

        std::vector<int> children;
    };

private:
    void ResetLayout();
    void BuildTemplate(TemplateType type);
    void BuildTabbedMenuTemplate();
    void BuildScrollListTemplate();
    void BuildSplitPanelTemplate();

    void DrawToolbar();
    void DrawPrefabSettings();
    void DrawCanvasSettings();
    void DrawTemplateSettings();
    void DrawLayoutTree();
    void DrawNodeInspector();
    void DrawValidationSummary();
    void DrawExportSection();

    void DrawLayoutGroupSettings(LayoutGroupSettings& settings, const char* header = "Layout Group", bool showEnabledToggle = true);
    void DrawFitToContentSettings(FitToContentSettings& settings);
    void DrawScrollViewSettings(LayoutNode& node);
    void DrawAnchorSettings(LayoutNode& node, bool allowParentAnchor);

    LayoutNode& CreateNode(NodeType type, const std::string& baseName);
    bool CanHaveChildren(NodeType type) const;
    bool CanEditLayout(NodeType type) const;
    int AddChild(int parentId, int childId);
    void RemoveNodeRecursive(int nodeId);
    int DuplicateNodeRecursive(int nodeId, int parentId);
    void MoveNodeWithinParent(int nodeId, int direction);
    LayoutNode* FindNode(int nodeId);
    const LayoutNode* FindNode(int nodeId) const;
    bool IsDescendant(int nodeId, int ancestorId) const;

    std::string SanitizeName(const std::string& input) const;
    std::string MakeUniqueName(int parentId, const std::string& baseName) const;
    glm::vec2 GetDesignCanvasSize() const;

    bool ExportPrefab();
    bool BuildPrefabScene(Scene& scene, EntityID& outRoot);
    EntityID BuildNodeRecursive(Scene& scene, int nodeId, EntityID parentId);
    EntityID BuildButtonLabel(Scene& scene, EntityID parentId, const LayoutNode& node);
    EntityID BuildScrollContentContainer(Scene& scene, EntityID scrollId, const LayoutNode& node, glm::vec2 contentSize);
    void BuildExampleListItems(Scene& scene, EntityID contentId, const LayoutNode& node);

    glm::vec2 ComputeScrollContentSize(const LayoutNode& node) const;
    glm::vec2 ComputeContentSizeFromChildren(const LayoutNode& node) const;
    glm::vec2 ComputeContentSizeFromEstimate(const LayoutNode& node) const;

    void ApplyPanelSettings(PanelComponent& panel, const LayoutNode& node) const;
    void ApplyLayoutSettings(LayoutGroupComponent& layout, const LayoutGroupSettings& settings) const;
    void ApplyFitToContentSettings(FitToContentComponent& fit, const FitToContentSettings& settings) const;
    void ApplyScrollSettings(ScrollViewComponent& scroll, const ScrollViewSettings& settings, const glm::vec2& contentSize) const;

    void CollectValidationIssues(std::vector<std::string>& errors, std::vector<std::string>& warnings) const;

private:
    UILayer* m_UILayer = nullptr;
    bool m_Open = false;
    bool m_FocusNextFrame = false;

    TemplateType m_SelectedTemplate = TemplateType::Blank;
    int m_AddNodeTypeIndex = 0;

    int m_NextNodeId = 1;
    int m_RootNodeId = 0;
    int m_SelectedNodeId = 0;
    std::unordered_map<int, LayoutNode> m_Nodes;

    char m_PrefabName[128] = "UI_Prefab";
    char m_OutputDirectory[260] = "assets/prefabs/UI";
    bool m_OverwriteExisting = false;
    bool m_OpenAfterExport = true;
    bool m_SelectInProject = true;

    int m_CanvasWidth = 0;
    int m_CanvasHeight = 0;
    float m_CanvasDpiScale = 1.0f;
    int m_CanvasSortOrder = 0;
    float m_CanvasOpacity = 1.0f;
    bool m_CanvasBlockSceneInput = true;
    int m_CanvasReferenceWidth = 1920;
    int m_CanvasReferenceHeight = 1080;
    int m_CanvasScaleMode = static_cast<int>(CanvasComponent::ScaleMode::ScaleWithWidth);
    int m_CanvasSpace = static_cast<int>(CanvasComponent::RenderSpace::ScreenSpace);

    std::string m_LastExportPath;
    std::string m_LastExportStatus;
    bool m_LastExportSuccess = true;
};
