#pragma once

#include "editor/ui/panels/EditorPanel.h"
#include "AnimationTimelinePanel.h"

#include <imgui.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct ImNodesEditorContext;

class InspectorPanel;

namespace cm { namespace animation {
    enum class AnimatorStateKind;
    struct AnimatorController;
    struct AnimatorState;
    struct AnimatorTransition;
}}

// Unity-like Animation Graph Editor
// Provides a visual state machine editor for animation controllers
class AnimationGraphPanel : public EditorPanel {
public:
    AnimationGraphPanel();
    ~AnimationGraphPanel();

    void OnImGuiRender();

    void Open() { m_Open = true; }
    void SetOpen(bool open) { m_Open = open; }
    bool IsOpen() const { return m_Open; }
    bool IsWindowFocusedOrHovered() const { return m_IsWindowFocusedOrHovered; }

    void SetContext(Scene* scene, EntityID* selectedEntity);
    void SetInspectorPanel(InspectorPanel* inspector) { m_Inspector = inspector; }

    bool OpenControllerAsset(const std::string& path);
    bool LoadController(const std::string& path);
    bool SaveController(const std::string& path);
    bool SaveCurrent();
    bool SaveCurrentAsDialog();
    
    // Get current controller for external access
    std::shared_ptr<cm::animation::AnimatorController> GetController() const { return m_Controller; }
    const std::string& GetCurrentPath() const { return m_OpenPath; }
    bool IsModified() const { return m_Modified; }

private:
    // Main UI components
    void DrawMenuBar();
    void DrawToolbar();
    void DrawLayersPanel(float width);
    void DrawLayersList();
    void DrawParameterList();
    void DrawGraphCanvas();
    void DrawInspectorPanel(float width);
    void DrawClipEditor(float height);

    void DrawAnyStateProperties();

    // Context menus
    void DrawCanvasContextMenu();
    void DrawStateContextMenu();
    void DrawTransitionContextMenu();
    void DrawEntryContextMenu();

    // Keyboard and input handling
    void HandleKeyboardShortcuts();
    void HandleDragDrop();

    // State management
    void EnsureIdCounters();
    void ResetEditorState();
    void SyncNodePositionsToModel();
    void RestoreNodePositionsFromModel();
    void SyncTimelineToSelection();
    void UpdateInspectorBinding();
    void MarkModified();

    // Node/state operations
    void CreateStateAtPosition(const ImVec2& gridPos, cm::animation::AnimatorStateKind kind);
    void DeleteStateById(int stateId);
    void DeleteTransitionById(int transitionId);
    void RemoveConditionsReferencingParam(const std::string& paramName);
    void FrameAllNodesNextFrame();
    void ApplyPendingNodePlacement();

    // Lookups
    cm::animation::AnimatorState* FindState(int id);
    const cm::animation::AnimatorState* FindState(int id) const;
    cm::animation::AnimatorTransition* FindTransition(int id);
    
    // Layer helpers - return states/transitions for currently selected layer
    std::vector<cm::animation::AnimatorState>& GetCurrentStates();
    const std::vector<cm::animation::AnimatorState>& GetCurrentStates() const;
    std::vector<cm::animation::AnimatorTransition>& GetCurrentTransitions();
    const std::vector<cm::animation::AnimatorTransition>& GetCurrentTransitions() const;
    int& GetCurrentDefaultState();
    cm::animation::AnimatorLayer* GetCurrentLayer();
    const cm::animation::AnimatorLayer* GetCurrentLayer() const;

    // Path utilities
    std::string ResolveAssetPath(const std::string& path) const;
    std::string MakeControllerRelativePath(const std::string& path) const;
    std::string MakeVFSPath(const std::string& absolutePath) const;

    // Attribute ID utilities for imnodes
    int MakeInputAttr(int nodeId) const;
    int MakeOutputAttr(int nodeId) const;
    int AttrToNode(int attrId) const;
    bool IsOutputAttr(int attrId) const;

    // Animation clip browsing
    void RefreshAnimationClipList();
    void DrawAnimationClipPicker(std::string& clipPath);

    // Properties panel helpers
    void DrawStateProperties(cm::animation::AnimatorState* state);
    void DrawBlend2DStateProperties(cm::animation::AnimatorState* state, float contentWidth);
    void DrawTransitionProperties(cm::animation::AnimatorTransition* trans);
    void DrawConditionEditor(cm::animation::AnimatorTransition* trans);
    
    // Custom transition arrow drawing (cleaner than ImNodes bezier curves)
    void DrawTransitionArrows();
    void DrawArrow(ImVec2 from, ImVec2 to, ImU32 color, float thickness, bool selected);
    bool IsNodeDrawn(int nodeId) const;
    ImVec2 GetNodeCenter(int nodeId);
    ImVec2 GetNodeEdgePoint(int nodeId, ImVec2 direction);
    int GetTransitionAtPoint(ImVec2 point, float threshold = 12.0f);

    // Special node IDs (negative to avoid conflicts with state IDs which start at 1)
    static constexpr int kEntryNodeId = -1000;
    static constexpr int kAnyStateNodeId = -1001;

private:
    // Controller data
    std::shared_ptr<cm::animation::AnimatorController> m_Controller;
    std::string m_OpenPath;
    bool m_Modified = false;
    bool m_IsWindowFocusedOrHovered = false;

    // ID counters
    int m_NextStateId = 1;
    int m_NextTransitionId = 1;

    // Selection state
    int m_SelectedStateId = -1;
    int m_SelectedTransitionId = -1;
    int m_ContextStateId = -1;
    int m_ContextTransitionId = -1;

    // External references
    InspectorPanel* m_Inspector = nullptr;
    Scene* m_SceneContext = nullptr;
    EntityID* m_SelectedEntityPtr = nullptr;
    bool m_Open = false;

    // Embedded timeline
    AnimTimelinePanel m_TimelinePanel;
    std::string m_LastTimelinePath;

    // ImNodes state
    ImNodesEditorContext* m_NodeEditorContext = nullptr;
    bool m_NodeStyleConfigured = false;
    std::unordered_set<int> m_PositionSeed;
    int m_PendingNewNodeId = -1;
    ImVec2 m_PendingNewNodePos = ImVec2(0.0f, 0.0f);
    ImVec2 m_PendingSpawnPos = ImVec2(0.0f, 0.0f);
    bool m_HasSpawnPos = false;
    bool m_FrameGraphNext = false;

    // Blend1D state selection
    std::unordered_map<int, int> m_BlendEntrySelection;

    // Panel layout (resizable)
    float m_LeftPanelWidth = 200.0f;
    float m_RightPanelWidth = 280.0f;
    float m_SplitterSize = 4.0f;

    // Layers/Parameters tab
    int m_LeftPanelTab = 0; // 0 = Parameters, 1 = Layers
    int m_SelectedLayerIndex = 0; // Which layer's graph to display (0 = base layer)

    // Animation clip cache for browsing
    std::vector<std::string> m_AnimationClipPaths;
    bool m_AnimationClipListDirty = true;
    char m_ClipSearchBuffer[128] = {};

    // State renaming
    int m_RenamingStateId = -1;
    char m_RenameBuffer[128] = {};

    // Entry/AnyState special nodes
    bool m_ShowEntryNode = true;
    bool m_ShowAnyStateNode = true;
    ImVec2 m_EntryNodePos = ImVec2(50.0f, 200.0f);
    ImVec2 m_AnyStateNodePos = ImVec2(50.0f, 100.0f);
    
    // Cached arrow segments for hit detection (populated during DrawTransitionArrows)
    struct ArrowSegment {
        int transitionId;
        ImVec2 from;
        ImVec2 to;
    };
    std::vector<ArrowSegment> m_CachedArrowSegments;
};

