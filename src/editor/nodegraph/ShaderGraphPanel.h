#pragma once

#include "ShaderGraph.h"
#include <string>
#include <unordered_set>
#include <functional>
#include <imgui.h>

struct ImNodesEditorContext;

namespace shadergraph {

class ShaderGraphPanel {
public:
    ShaderGraphPanel();
    ~ShaderGraphPanel();
    
    // Open/close the panel
    void Open() { m_Open = true; }
    void Close() { m_Open = false; }
    bool IsOpen() const { return m_Open; }
    void SetOpen(bool open) { m_Open = open; }
    bool IsWindowFocusedOrHovered() const { return m_IsWindowFocusedOrHovered; }
    
    // Render the panel
    void OnImGuiRender();
    
    // Load/save operations
    bool NewGraph();
    bool OpenGraph(const std::string& path);
    bool SaveGraph();
    bool SaveGraphAs(const std::string& path);
    bool SaveCurrent() { return SaveGraph(); }
    bool SaveCurrentAsDialog();
    
    // Get current graph for external access
    ShaderGraph* GetCurrentGraph() { return m_Graph.get(); }
    const std::string& GetCurrentPath() const { return m_OpenPath; }
    void SetGraphSavedCallback(std::function<void(const std::string&)> callback) { m_OnGraphSaved = std::move(callback); }
    
    // Compile the current graph
    bool CompileGraph();
    
private:
    struct ShaderPreviewState;

    // UI Drawing
    void DrawMenuBar();
    void DrawToolbar();
    void DrawNodePalette(float width);
    void DrawGraphCanvas();
    void DrawPreviewPanel(float width);
    void DrawPropertiesPanel(float width);
    void DrawErrorPanel(float height);
    
    // Context menus
    void DrawCanvasContextMenu();
    void DrawNodeContextMenu();
    void DrawLinkContextMenu();
    
    // Node operations
    void CreateNodeAtPosition(const NodeTypeId& typeId, const ImVec2& gridPos);
    void DeleteSelectedNodes();
    void DeleteSelectedLinks();
    
    // Keyboard shortcuts
    void HandleKeyboardShortcuts();
    
    // Helper for ImNodes attribute IDs
    int MakeInputAttr(PinId pinId) const;
    int MakeOutputAttr(PinId pinId) const;
    bool IsOutputAttr(int attrId) const;
    PinId AttrToPin(int attrId) const;
    
    // Get color for value type
    ImU32 GetPinColor(ShaderValueType type) const;
    
    // Node positioning
    void ApplyPendingNodePlacement();
    void FrameAllNodes();
    void EnsurePreviewScene();
    void RefreshPreviewMaterial(bool keepStatusMessage = false);
    
private:
    std::unique_ptr<ShaderGraph> m_Graph;
    std::unique_ptr<ShaderPreviewState> m_Preview;
    std::string m_OpenPath;
    bool m_Open = false;
    bool m_Modified = false;
    bool m_IsWindowFocusedOrHovered = false;
    std::function<void(const std::string&)> m_OnGraphSaved;
    
    // ImNodes context
    ImNodesEditorContext* m_NodeEditorContext = nullptr;
    
    // Node palette state
    char m_SearchBuffer[256] = {};
    std::string m_SelectedCategory;
    
    // Selection state
    int m_SelectedNodeId = -1;
    int m_SelectedLinkId = -1;
    int m_ContextNodeId = -1;
    int m_ContextLinkId = -1;
    
    // Pending node creation
    int m_PendingNewNodeId = -1;
    ImVec2 m_PendingNewNodePos = ImVec2(0.0f, 0.0f);
    ImVec2 m_PendingSpawnGridPos = ImVec2(0.0f, 0.0f);
    bool m_HasSpawnPos = false;
    bool m_FrameGraphNext = false;
    
    // Nodes that have had their position set
    std::unordered_set<int> m_PositionSeed;
    
    // Compilation state
    std::vector<std::string> m_CompileErrors;
    std::vector<std::string> m_CompileWarnings;
    bool m_ShowErrors = false;
    
    // Properties panel state
    int m_EditingPinId = -1;
    char m_ExposedNameBuffer[128] = {};
};

} // namespace shadergraph

