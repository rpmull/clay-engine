#include "NodeGraphPanel.h"
#include <imgui.h>
#include <imgui_clay_inspector.h>
#include <imnodes.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cstring>
#include "editor/ui/FileDialogs.h"
#include "managed/interop/NodeGraphInterop.h"
#include "editor/pipeline/AssetLibrary.h"

namespace fs = std::filesystem;

NodeGraphPanel::NodeGraphPanel()
{
    m_NodeEditorContext = ImNodes::EditorContextCreate();
}

NodeGraphPanel::~NodeGraphPanel()
{
    ReleaseEditorContext();
}

void NodeGraphPanel::ReleaseEditorContext()
{
    if (m_NodeEditorContext) {
        ImNodes::EditorContextFree(m_NodeEditorContext);
        m_NodeEditorContext = nullptr;
    }
}

bool NodeGraphPanel::Load(const std::string& path)
{
    nodegraph::GraphAsset loaded;
    if (!nodegraph::GraphSerializer::LoadFromFile(path, loaded)) return false;
    m_Asset = std::move(loaded);
    m_OpenPath = path;
    return true;
}

bool NodeGraphPanel::Save(const std::string& path)
{
    // Persist editor positions from ImNodes if available
    for (const auto& n : m_Asset.graph.nodes)
    {
        ImVec2 pos = ImNodes::GetNodeEditorSpacePos(n.id);
        m_Asset.editor.nodePositions[n.id] = { pos.x, pos.y };
    }
    bool ok = nodegraph::GraphSerializer::SaveToFile(m_Asset, path);
    if (ok) m_OpenPath = path;
    return ok;
}

bool NodeGraphPanel::SaveCurrent()
{
    if (!m_OpenPath.empty()) {
        return Save(m_OpenPath);
    }
    return SaveCurrentAsDialog();
}

bool NodeGraphPanel::SaveCurrentAsDialog()
{
    std::wstring defaultName = L"NewGraph.ngraph";
    if (!m_OpenPath.empty()) {
        defaultName.assign(m_OpenPath.begin(), m_OpenPath.end());
    } else if (!m_Asset.graph.name.empty()) {
        std::string filename = m_Asset.graph.name + ".ngraph";
        defaultName.assign(filename.begin(), filename.end());
    }

    const std::string path = ShowSaveFileDialogExt(defaultName.c_str(), L"Node Graph (*.ngraph)", L"ngraph");
    if (path.empty()) {
        return false;
    }
    return Save(path);
}

void NodeGraphPanel::OnImGuiRender()
{
    if (!m_Open) {
        m_WindowFocusedOrHovered = false;
        return;
    }
    if (!ImGui::Begin("Node Graph", &m_Open)) {
        m_WindowFocusedOrHovered = false;
        ImGui::End();
        return;
    }
    m_WindowFocusedOrHovered =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ||
        ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
    if (!m_Registry) {
        extern NodeTypeRegistry* GetGlobalNodeTypeRegistry();
        m_Registry = GetGlobalNodeTypeRegistry();
    }
    DrawToolbar();
    ImGui::Separator();

    // Three-pane layout with resizable splitters
    const float fullWidth = ImGui::GetContentRegionAvail().x;
    const float fullHeight = ImGui::GetContentRegionAvail().y;

    // Left palette
    ImGui::BeginChild("Palette", ImVec2(m_PaletteWidth, fullHeight), true);
    DrawPalette();
    ImGui::EndChild();

    // Left splitter
    ImGui::SameLine();
    ImGui::ClaySplitterConfig leftSplitter;
    leftSplitter.Vertical = true;
    leftSplitter.Thickness = m_SplitterSize;
    leftSplitter.MinPrimary = 150.0f;
    leftSplitter.MinSecondary = 150.0f;
    leftSplitter.HoverCursor = ImGuiMouseCursor_ResizeEW;
    const float leftTotal = fullWidth - m_InspectorWidth - m_SplitterSize;
    ImGui::ClaySplitter("NG_Splitter_L", &m_PaletteWidth, leftTotal, fullHeight, leftSplitter);

    // Center editor width is whatever remains after left/right panes and splitters
    float centerWidth = fullWidth - m_PaletteWidth - m_InspectorWidth - m_SplitterSize * 2;
    if (centerWidth < 100.0f) centerWidth = 100.0f;

    ImGui::SameLine();
    ImGui::BeginChild("GraphCanvas", ImVec2(centerWidth, fullHeight), true, ImGuiWindowFlags_NoNav);
    DrawEditor();
    ImGui::EndChild();

    // Right splitter
    ImGui::SameLine();
    ImGui::ClaySplitterConfig rightSplitter;
    rightSplitter.Vertical = true;
    rightSplitter.InvertAxis = true;
    rightSplitter.Thickness = m_SplitterSize;
    rightSplitter.MinPrimary = 200.0f;
    rightSplitter.MinSecondary = 150.0f;
    rightSplitter.HoverCursor = ImGuiMouseCursor_ResizeEW;
    const float rightTotal = fullWidth - m_PaletteWidth - m_SplitterSize;
    ImGui::ClaySplitter("NG_Splitter_R", &m_InspectorWidth, rightTotal, fullHeight, rightSplitter);

    ImGui::SameLine();
    ImGui::BeginChild("Inspector", ImVec2(m_InspectorWidth, fullHeight), true);
    DrawInspector();
    ImGui::EndChild();
    ImGui::End();
}

void NodeGraphPanel::DrawToolbar()
{
    if (ImGui::Button("New")) {
        m_Asset = nodegraph::GraphAsset();
        m_Asset.graph.name = "New Graph";
        m_OpenPath.clear();
    }
    ImGui::SameLine();
    static char pathBuf[512] = {0};
    ImGui::SetNextItemWidth(240);
    ImGui::InputText("##ngpath", pathBuf, sizeof(pathBuf));
    ImGui::SameLine();
    if (ImGui::Button("Open")) {
        std::string p = ShowOpenFileDialogExt(L"Node Graph (*.ngraph)", L"ngraph");
        if (!p.empty()) { strncpy(pathBuf, p.c_str(), sizeof(pathBuf)); pathBuf[sizeof(pathBuf)-1]=0; Load(p); }
        else if (pathBuf[0]) Load(pathBuf);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        if (m_OpenPath.empty()) {
            std::string p = ShowSaveFileDialogExt(L"NewGraph.ngraph", L"Node Graph (*.ngraph)", L"ngraph");
            if (!p.empty()) Save(p);
        } else {
            Save(m_OpenPath);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Save As")) {
        std::wstring def = L"NewGraph.ngraph";
        std::string p = ShowSaveFileDialogExt(def.c_str(), L"Node Graph (*.ngraph)", L"ngraph");
        if (!p.empty()) Save(p);
    }
}

void NodeGraphPanel::DrawPalette()
{
    ImGui::TextUnformatted("Node Types");
    ImGui::Separator();
    if (!m_Registry) { ImGui::TextDisabled("<No registry>"); return; }
    for (const auto& kv : m_Registry->byId)
    {
        const NodeType& t = kv.second; ImGui::PushID((int)t.typeId);
        if (ImGui::Selectable(t.name, false)) {
            Node n; n.id = m_Asset.graph.NewNodeId(); n.typeId = t.typeId; n.name = t.name; m_Asset.graph.nodes.push_back(n);
            if (t.Create) { t.Create(m_Asset.graph, m_Asset.graph.nodes.back()); }
        }
        ImGui::PopID();
    }
}

void NodeGraphPanel::DrawEditor()
{
    if (m_NodeEditorContext) {
        ImNodes::EditorContextSet(m_NodeEditorContext);
    }
    ImNodes::BeginNodeEditor();

    // Draw nodes
    for (auto& n : m_Asset.graph.nodes)
    {
        ImNodes::BeginNode(n.id);
        ImNodes::BeginNodeTitleBar();
        ImGui::TextUnformatted(n.name.c_str());
        ImNodes::EndNodeTitleBar();

        // Inputs
        for (auto pid : n.inputs)
        {
            ImNodes::BeginInputAttribute(pid);
            for (auto& p : m_Asset.graph.pins) if (p.id == pid) { ImGui::Text("%s", p.name.c_str()); break; }
            ImNodes::EndInputAttribute();
        }
        // Outputs
        for (auto pid : n.outputs)
        {
            ImNodes::BeginOutputAttribute(pid);
            for (auto& p : m_Asset.graph.pins) if (p.id == pid) { ImGui::Text("%s", p.name.c_str()); break; }
            ImNodes::EndOutputAttribute();
        }

        ImNodes::EndNode();

        // Restore positions if we have them
        auto it = m_Asset.editor.nodePositions.find(n.id);
        if (it != m_Asset.editor.nodePositions.end())
            ImNodes::SetNodeGridSpacePos(n.id, ImVec2(it->second.first, it->second.second));
    }

    // Draw links (prevent invalid self-links)
    for (auto& l : m_Asset.graph.links)
    {
        if (l.from != l.to)
            ImNodes::Link(l.id, l.from, l.to);
    }

    // Context menu: right-click in empty space -> Add Node
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ImNodes::IsEditorHovered())
    {
        m_PendingNewNodeScreenPos = ImGui::GetMousePos();
        ImGui::OpenPopup("NG_AddNodePopup");
    }

    if (ImGui::BeginPopup("NG_AddNodePopup"))
    {
        if (!m_Registry || m_Registry->byId.empty()) {
            ImGui::MenuItem("<No node types>", nullptr, false, false);
        } else {
            for (const auto& kv : m_Registry->byId)
            {
                const NodeType& t = kv.second; ImGui::PushID((int)t.typeId);
                if (ImGui::MenuItem(t.name))
                {
                    Node n; n.id = m_Asset.graph.NewNodeId(); n.typeId = t.typeId; n.name = t.name; m_Asset.graph.nodes.push_back(n);
                    if (t.Create) { t.Create(m_Asset.graph, m_Asset.graph.nodes.back()); }
                    // Defer placement until after EndNodeEditor, once the node is submitted
                    m_PendingNewNodeId = n.id;
                }
                ImGui::PopID();
            }
        }
        ImGui::EndPopup();
    }

    ImNodes::EndNodeEditor();

    // Create links by drag (must be called after EndNodeEditor per imnodes API)
    int startAttr = 0, endAttr = 0;
    if (ImNodes::IsLinkCreated(&startAttr, &endAttr))
    {
        if (startAttr != endAttr)
        {
            LinkId lid = m_Asset.graph.NewLinkId();
            m_Asset.graph.links.push_back({ lid, startAttr, endAttr });
        }
    }

    // After the editor finished, position any node created this frame
    if (m_PendingNewNodeId >= 0 && m_PendingNewNodeScreenPos.x >= 0.0f)
    {
        ImNodes::SetNodeScreenSpacePos(m_PendingNewNodeId, m_PendingNewNodeScreenPos);
        m_PendingNewNodeId = -1;
        m_PendingNewNodeScreenPos = ImVec2(-1.0f, -1.0f);
    }

    // Selection
    int hoveredNode = -1, hoveredLink = -1;
    if (ImNodes::IsNodeHovered(&hoveredNode) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        m_SelectedNode = hoveredNode; m_SelectedLink = -1;
    }
    if (ImNodes::IsLinkHovered(&hoveredLink) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        m_SelectedLink = hoveredLink; m_SelectedNode = -1;
    }

    HandleDragDrop();
}

void NodeGraphPanel::HandleDragDrop()
{
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE", ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
        {
            const char* path = (const char*)payload->Data;
            std::string ext = fs::path(path).extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".ngraph")
            {
                // Spawn a Subgraph node placeholder with a GUID path payload
                Node n; n.id = m_Asset.graph.NewNodeId(); n.typeId = 0; n.name = "Subgraph";
                Pin in; in.id = m_Asset.graph.NewPinId(); in.nodeId = n.id; in.kind = PinKind::Input; in.type = PinValueType::Any; in.name = "In"; n.inputs.push_back(in.id);
                Pin out; out.id = m_Asset.graph.NewPinId(); out.nodeId = n.id; out.kind = PinKind::Output; out.type = PinValueType::Any; out.name = "Out"; n.outputs.push_back(out.id);
                m_Asset.graph.pins.push_back(in); m_Asset.graph.pins.push_back(out);
                m_Asset.graph.nodes.push_back(n);
                // Store payload: absolute path (AssetLibrary will have GUID mapping)
                nlohmann::json payloadJson; payloadJson["subgraphPath"] = std::string(path);
                m_Asset.nodePayloads[n.id] = std::move(payloadJson);
            }
        }
        ImGui::EndDragDropTarget();
    }
}

void NodeGraphPanel::DrawInspector()
{
    if (m_SelectedNode >= 0)
    {
        for (auto& n : m_Asset.graph.nodes) if (n.id == m_SelectedNode)
        {
            ImGui::Text("Node %d", n.id);
            char buf[128]; strncpy(buf, n.name.c_str(), sizeof(buf)); buf[sizeof(buf)-1]=0;
            if (ImGui::InputText("Name", buf, sizeof(buf))) n.name = buf;
            // Payload editing for Subgraph placeholder
            auto it = m_Asset.nodePayloads.find(n.id);
            if (it != m_Asset.nodePayloads.end())
            {
                std::string path = it->second.value("subgraphPath", std::string());
                char pbuf[512]; strncpy(pbuf, path.c_str(), sizeof(pbuf)); pbuf[sizeof(pbuf)-1]=0;
                if (ImGui::InputText("Subgraph", pbuf, sizeof(pbuf))) {
                    it->second["subgraphPath"] = std::string(pbuf);
                }
                ImGui::SameLine();
                if (ImGui::Button("...")) {
                    std::string p = ShowOpenFileDialogExt(L"Node Graph (*.ngraph)", L"ngraph");
                    if (!p.empty()) it->second["subgraphPath"] = p;
                }
            }
            break;
        }
    }
    else if (m_SelectedLink >= 0)
    {
        ImGui::Text("Link %d", m_SelectedLink);
    }
    else
    {
        ImGui::TextDisabled("Select a node or link.");
    }
}


