#pragma once

#include <string>
#include <memory>
#include <vector>
#include <unordered_map>

#include <imnodes.h>
#include <nlohmann/json.hpp>

#include "editor/nodegraph/GraphCore.h"
#include "editor/nodegraph/GraphSerializer.h"
#include "editor/nodegraph/NodeRegsitry.h"

class NodeGraphPanel
{
public:
    NodeGraphPanel();
    ~NodeGraphPanel();
    void ReleaseEditorContext();

    // Load/save .ngraph
    bool Load(const std::string& path);
    bool Save(const std::string& path);
    bool SaveCurrent();
    bool SaveCurrentAsDialog();
    const std::string& GetCurrentPath() const { return m_OpenPath; }

    // Render the panel window
    void OnImGuiRender();

    // Open/close state
    void Open() { m_Open = true; }
    void SetOpen(bool open) { m_Open = open; }
    bool IsOpen() const { return m_Open; }
    bool IsWindowFocusedOrHovered() const { return m_WindowFocusedOrHovered; }

    // Optional: set registry pointer if external
    void SetRegistry(NodeTypeRegistry* reg) { m_Registry = reg; }

private:
    void DrawToolbar();
    void DrawEditor();
    void DrawInspector();
    void DrawPalette();
    void HandleDragDrop();

    // Layout state
    float m_PaletteWidth = 240.0f;
    float m_InspectorWidth = 280.0f;
    float m_SplitterSize = 5.0f;
    // For placing nodes from context menu (defer until node has been submitted this frame)
    ImVec2 m_PendingNewNodeScreenPos{ -1.0f, -1.0f };
    NodeId m_PendingNewNodeId{ -1 };

    nodegraph::GraphAsset m_Asset;
    std::string m_OpenPath;
    NodeTypeRegistry* m_Registry{ nullptr };

    // Selection
    NodeId m_SelectedNode{ -1 };
    LinkId m_SelectedLink{ -1 };

    // ImNodes context - each editor needs its own
    ImNodesEditorContext* m_NodeEditorContext = nullptr;

    bool m_Open = false;
    bool m_WindowFocusedOrHovered = false;
};


