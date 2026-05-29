#include "ShaderGraphPanel.h"
#include "ShaderGraphNodes.h"
#include "ShaderGraphSerializer.h"
#include "ShaderGraphCodeGen.h"
#include "ShaderGraphMaterial.h"
#include "editor/ui/FileDialogs.h"
#include "editor/ui/utility/TextureSlotPicker.h"
#include "editor/Project.h"
#include "core/rendering/Renderer.h"
#include "core/rendering/MaterialCache.h"
#include "core/rendering/MaterialManager.h"
#include "core/rendering/StandardMeshManager.h"
#include "core/rendering/TextureLoader.h"
#include "core/ecs/Components.h"
#include "core/vfs/VirtualFS.h"

#include <imnodes.h>
#include <imgui.h>
#include <algorithm>
#include <filesystem>
#include <cstring>
#include <cmath>
#include <glm/glm.hpp>

namespace shadergraph {

namespace {
    constexpr float kLeftPaneWidth = 220.0f;
    constexpr float kRightPaneWidth = 300.0f;
    constexpr float kErrorPanelHeight = 150.0f;
    constexpr float kPreviewMinHeight = 180.0f;
    constexpr float kPreviewMaxHeight = 260.0f;
    constexpr int kAttrMultiplier = 100000;
    constexpr int kInputOffset = 0;
    constexpr int kOutputOffset = 50000;

    std::string NormalizeTextureAssetPath(const std::string& path) {
        if (path.empty()) {
            return {};
        }

        std::string normalized = IVirtualFS::NormalizePath(path);
        std::string vfsPath = VFS::StripToKnownPrefix(normalized);
        if (!vfsPath.empty()) {
            return vfsPath;
        }

        try {
            std::filesystem::path fsPath(normalized);
            if (fsPath.is_absolute()) {
                const std::filesystem::path projectRoot = Project::GetProjectDirectory();
                if (!projectRoot.empty()) {
                    std::error_code ec;
                    std::filesystem::path relative = std::filesystem::relative(fsPath, projectRoot, ec);
                    if (!ec) {
                        std::string relativePath = IVirtualFS::NormalizePath(relative.string());
                        if (relativePath.find("../") == std::string::npos) {
                            std::string relativeVfs = VFS::StripToKnownPrefix(relativePath);
                            return relativeVfs.empty() ? relativePath : relativeVfs;
                        }
                    }
                }
            }
        } catch (...) {
        }

        return normalized;
    }

    std::vector<MaterialParameter> BuildMaterialParametersFromCompileResult(const ShaderCompileResult& compileResult) {
        std::vector<MaterialParameter> parameters;
        parameters.reserve(compileResult.parameters.size());

        for (const auto& compileParam : compileResult.parameters) {
            MaterialParameter parameter;
            parameter.name = compileParam.name;
            parameter.displayName = compileParam.displayName;
            parameter.type = compileParam.type;
            parameter.value = compileParam.defaultValue;
            parameter.texturePath = compileParam.texturePath;
            parameter.textureSlot = compileParam.textureSlot;
            parameters.push_back(std::move(parameter));
        }

        return parameters;
    }

    bool IsTexturePathExtension(const std::string& extension) {
        std::string lower = extension;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return lower == ".png" || lower == ".jpg" || lower == ".jpeg" || lower == ".tga" || lower == ".bmp" || lower == ".hdr";
    }
}

struct ShaderGraphPanel::ShaderPreviewState {
    static constexpr uint16_t kViewIdBase = 244;

    Scene scene;
    Camera camera{40.0f, 1.0f, 0.1f, 32.0f};
    EntityID sphereEntity = INVALID_ENTITY_ID;
    EntityID keyLightEntity = INVALID_ENTITY_ID;
    EntityID fillLightEntity = INVALID_ENTITY_ID;
    std::shared_ptr<Material> fallbackMaterial;
    std::shared_ptr<Material> previewMaterial;
    glm::vec3 target = glm::vec3(0.0f);
    float distance = 3.2f;
    float yaw = 25.0f;
    float pitch = 12.0f;
    bool hasCompiledPreview = false;
    std::string statusMessage = "Compile the graph to preview the current shader.";
};

ShaderGraphPanel::ShaderGraphPanel() {
    m_NodeEditorContext = ImNodes::EditorContextCreate();
    m_Graph = std::make_unique<ShaderGraph>();
}

ShaderGraphPanel::~ShaderGraphPanel() {
    if (m_Preview) {
        Renderer::Get().ReleaseOffscreenTarget(ShaderPreviewState::kViewIdBase);
    }
    if (m_NodeEditorContext) {
        ImNodes::EditorContextFree(m_NodeEditorContext);
        m_NodeEditorContext = nullptr;
    }
}

bool ShaderGraphPanel::NewGraph() {
    m_Graph = std::make_unique<ShaderGraph>();
    m_OpenPath.clear();
    m_Modified = false;
    m_PositionSeed.clear();
    m_CompileErrors.clear();
    m_CompileWarnings.clear();
    m_SelectedNodeId = -1;
    m_SelectedLinkId = -1;
    m_FrameGraphNext = true;
    RefreshPreviewMaterial();
    return true;
}

bool ShaderGraphPanel::OpenGraph(const std::string& path) {
    auto newGraph = std::make_unique<ShaderGraph>();
    if (!ShaderGraphSerializer::LoadFromFile(path, *newGraph)) {
        return false;
    }
    
    m_Graph = std::move(newGraph);
    m_OpenPath = path;
    m_Modified = false;
    m_PositionSeed.clear();
    m_CompileErrors.clear();
    m_CompileWarnings.clear();
    m_SelectedNodeId = -1;
    m_SelectedLinkId = -1;
    m_FrameGraphNext = true;
    RefreshPreviewMaterial();
    return true;
}

bool ShaderGraphPanel::SaveGraph() {
    if (m_OpenPath.empty()) {
        std::string path = ShowSaveFileDialogExt(L"NewShaderGraph.shgraph", L"Shader Graph (*.shgraph)", L"shgraph");
        if (path.empty()) return false;
        return SaveGraphAs(path);
    }
    return SaveGraphAs(m_OpenPath);
}

bool ShaderGraphPanel::SaveGraphAs(const std::string& path) {
    if (!m_Graph) return false;
    
    // Update node editor positions before saving (only for drawn nodes)
    for (auto& node : m_Graph->nodes) {
        if (m_PositionSeed.count(node.id)) {
            ImVec2 pos = ImNodes::GetNodeGridSpacePos(node.id);
            node.editorPos.x = pos.x;
            node.editorPos.y = pos.y;
        }
    }
    
    if (!ShaderGraphSerializer::SaveToFile(*m_Graph, path)) {
        return false;
    }
    
    m_OpenPath = path;
    m_Modified = false;
    
    // Auto-compile after saving
    CompileGraph();
    return true;
}

bool ShaderGraphPanel::SaveCurrentAsDialog()
{
    const std::string path = ShowSaveFileDialogExt(L"ShaderGraph.shgraph", L"Shader Graph (*.shgraph)", L"shgraph");
    if (path.empty()) {
        return false;
    }
    return SaveGraphAs(path);
}

bool ShaderGraphPanel::CompileGraph() {
    if (!m_Graph) return false;
    
    m_CompileErrors.clear();
    m_CompileWarnings.clear();
    
    ShaderGraphCodeGen codegen(*m_Graph);
    ShaderCompileResult result = codegen.Compile();
    
    m_CompileErrors = result.errors;
    m_CompileWarnings = result.warnings;
    
    if (!result.success) {
        m_ShowErrors = true;
        m_Graph->isCompiled = false;
        RefreshPreviewMaterial();
        return false;
    }
    
    // Generate base name from graph name or file name
    std::string baseName = m_Graph->name;
    if (!m_OpenPath.empty()) {
        baseName = std::filesystem::path(m_OpenPath).stem().string();
    }
    // Sanitize name
    std::replace(baseName.begin(), baseName.end(), ' ', '_');
    baseName = "shgraph_" + baseName;
    
    // Determine project root from the .shgraph file path
    // The project root is the directory containing the assets folder
    std::string projectRoot;
    if (!m_OpenPath.empty()) {
        std::filesystem::path graphPath = std::filesystem::path(m_OpenPath).parent_path();
        // Walk up until we find the directory that contains "assets"
        for (auto p = graphPath; !p.empty() && p.has_parent_path(); p = p.parent_path()) {
            if (std::filesystem::exists(p / "assets") || 
                std::filesystem::exists(p / ".library") ||
                p.filename() == "assets") {
                projectRoot = (p.filename() == "assets") ? p.parent_path().string() : p.string();
                break;
            }
        }
    }
    if (projectRoot.empty()) {
        projectRoot = std::filesystem::current_path().string();
    }
    
    ShaderCompileOutput compileOutput = WriteAndCompileShaders(result, projectRoot, baseName);
    if (!compileOutput.success) {
        m_CompileErrors.push_back(compileOutput.error);
        m_ShowErrors = true;
        m_Graph->isCompiled = false;
        RefreshPreviewMaterial();
        return false;
    }
    
    // Update the graph's compiled shader info
    m_Graph->compiledVSName = compileOutput.vsName;
    m_Graph->compiledFSName = compileOutput.fsName;
    m_Graph->compiledVSPath = compileOutput.vsBinPath;
    m_Graph->compiledFSPath = compileOutput.fsBinPath;
    m_Graph->isCompiled = true;
    
    // Re-save the graph to persist compiled info (without triggering recompile)
    if (!m_OpenPath.empty()) {
        if (ShaderGraphSerializer::SaveToFile(*m_Graph, m_OpenPath)) {
            m_Modified = false;
        }
    }
    
    m_ShowErrors = !m_CompileErrors.empty() || !m_CompileWarnings.empty();
    RefreshPreviewMaterial();
    if (!m_OpenPath.empty() && m_OnGraphSaved) {
        m_OnGraphSaved(m_OpenPath);
    }
    return true;
}

void ShaderGraphPanel::OnImGuiRender() {
    if (!m_Open) {
        m_IsWindowFocusedOrHovered = false;
        return;
    }
    
    ImGui::SetNextWindowSize(ImVec2(1400.0f, 800.0f), ImGuiCond_FirstUseEver);
    
    if (!ImGui::Begin("Shader Graph", &m_Open, ImGuiWindowFlags_MenuBar)) {
        m_IsWindowFocusedOrHovered = false;
        ImGui::End();
        return;
    }
    m_IsWindowFocusedOrHovered =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ||
        ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
    
    HandleKeyboardShortcuts();
    DrawMenuBar();
    DrawToolbar();
    ImGui::Separator();
    
    if (!m_Graph) {
        ImGui::TextDisabled("No shader graph loaded. Use File > New or File > Open.");
        ImGui::End();
        return;
    }
    
    // Calculate layout
    float totalHeight = ImGui::GetContentRegionAvail().y;
    float errorHeight = (m_ShowErrors && (!m_CompileErrors.empty() || !m_CompileWarnings.empty())) ? kErrorPanelHeight : 0.0f;
    float mainHeight = totalHeight - errorHeight - ImGui::GetStyle().ItemSpacing.y;
    float centerWidth = std::max(400.0f, ImGui::GetContentRegionAvail().x - kLeftPaneWidth - kRightPaneWidth - ImGui::GetStyle().ItemSpacing.x * 2.0f);
    
    // Main content area
    ImGui::BeginChild("SG_MainArea", ImVec2(0, mainHeight), false, ImGuiWindowFlags_NoScrollbar);
    
    // Left pane - Node palette
    ImGui::BeginChild("SG_LeftPane", ImVec2(kLeftPaneWidth, 0), true);
    DrawNodePalette(kLeftPaneWidth);
    ImGui::EndChild();
    
    ImGui::SameLine();
    
    // Center - Graph canvas
    ImGui::BeginChild("SG_GraphCanvas", ImVec2(centerWidth, 0), true, ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar);
    DrawGraphCanvas();
    ImGui::EndChild();
    
    ImGui::SameLine();
    
    // Right pane - Properties
    ImGui::BeginChild("SG_RightPane", ImVec2(kRightPaneWidth, 0), true);
    DrawPreviewPanel(kRightPaneWidth);
    ImGui::Spacing();
    DrawPropertiesPanel(kRightPaneWidth);
    ImGui::EndChild();
    
    ImGui::EndChild();
    
    // Error panel (if needed)
    if (m_ShowErrors && (!m_CompileErrors.empty() || !m_CompileWarnings.empty())) {
        ImGui::Separator();
        DrawErrorPanel(errorHeight);
    }
    
    ImGui::End();
}

void ShaderGraphPanel::DrawMenuBar() {
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New", "Ctrl+N")) {
                NewGraph();
            }
            if (ImGui::MenuItem("Open...", "Ctrl+O")) {
                std::string path = ShowOpenFileDialogExt(L"Shader Graph (*.shgraph)", L"shgraph");
                if (!path.empty()) {
                    OpenGraph(path);
                }
            }
            if (ImGui::MenuItem("Save", "Ctrl+S")) {
                SaveGraph();
            }
            if (ImGui::MenuItem("Save As...")) {
                std::string path = ShowSaveFileDialogExt(L"ShaderGraph.shgraph", L"Shader Graph (*.shgraph)", L"shgraph");
                if (!path.empty()) {
                    SaveGraphAs(path);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Close")) {
                m_Open = false;
            }
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Delete", "Delete", false, m_SelectedNodeId >= 0 || m_SelectedLinkId >= 0)) {
                if (m_SelectedNodeId >= 0) DeleteSelectedNodes();
                else if (m_SelectedLinkId >= 0) DeleteSelectedLinks();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Frame All", "F")) {
                FrameAllNodes();
            }
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("Graph")) {
            if (ImGui::MenuItem("Compile", "F5")) {
                CompileGraph();
            }
            ImGui::EndMenu();
        }
        
        ImGui::EndMenuBar();
    }
}

void ShaderGraphPanel::DrawToolbar() {
    if (ImGui::Button("Compile")) {
        CompileGraph();
    }
    ImGui::SameLine();
    
    // Surface type selection
    ImGui::SetNextItemWidth(100.0f);
    const char* surfaceTypes[] = {"Lit (PBR)", "Unlit", "Custom"};
    int currentSurface = static_cast<int>(m_Graph->surfaceType);
    if (ImGui::Combo("##SurfaceType", &currentSurface, surfaceTypes, IM_ARRAYSIZE(surfaceTypes))) {
        m_Graph->surfaceType = static_cast<SurfaceType>(currentSurface);
        m_Modified = true;
    }
    ImGui::SameLine();
    
    // Environment options (fog and ambient)
    if (ImGui::Checkbox("Fog", &m_Graph->applyFog)) {
        m_Modified = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Apply distance fog to this material");
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Ambient", &m_Graph->applyAmbient)) {
        m_Modified = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Apply ambient/environment lighting to this material");
    }
    ImGui::SameLine();
    
    // Graph name
    static char nameBuf[256] = {};
    std::strncpy(nameBuf, m_Graph->name.c_str(), sizeof(nameBuf));
    nameBuf[sizeof(nameBuf) - 1] = 0;
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) {
        m_Graph->name = nameBuf;
        m_Modified = true;
    }
    ImGui::SameLine();
    
    // Modified indicator
    if (m_Modified) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "*");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", m_OpenPath.empty() ? "<unsaved>" : m_OpenPath.c_str());
}

void ShaderGraphPanel::DrawNodePalette(float width) {
    ImGui::TextUnformatted("Nodes");
    ImGui::Separator();
    
    // Search
    ImGui::SetNextItemWidth(width - 20.0f);
    ImGui::InputTextWithHint("##Search", "Search...", m_SearchBuffer, sizeof(m_SearchBuffer));
    std::string searchStr = m_SearchBuffer;
    std::transform(searchStr.begin(), searchStr.end(), searchStr.begin(), ::tolower);
    
    // Category buttons
    const char* categories[] = {"Input", "Math", "Vector", "Texture", "Output"};
    for (const char* cat : categories) {
        bool selected = (m_SelectedCategory == cat);
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
        }
        if (ImGui::Button(cat, ImVec2(width / 2.5f - 4.0f, 0))) {
            m_SelectedCategory = selected ? "" : cat;
        }
        if (selected) {
            ImGui::PopStyleColor();
        }
        if (cat != categories[4]) ImGui::SameLine();
    }
    
    ImGui::Separator();
    
    // Node list
    ImGui::BeginChild("NodeList", ImVec2(0, 0), false);
    
    const auto& registry = NodeRegistry::Instance();
    std::string lastCategory;
    
    for (const auto& def : registry.GetAll()) {
        // Filter by search
        if (!searchStr.empty()) {
            std::string name = def.displayName;
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            if (name.find(searchStr) == std::string::npos) continue;
        }
        
        // Filter by category
        if (!m_SelectedCategory.empty() && def.typeId.category != m_SelectedCategory) {
            continue;
        }
        
        // Category header
        if (def.typeId.category != lastCategory) {
            if (!lastCategory.empty()) ImGui::Spacing();
            ImGui::TextDisabled("%s", def.typeId.category.c_str());
            lastCategory = def.typeId.category;
        }
        
        // Node button with drag source
        if (ImGui::Selectable(def.displayName.c_str())) {
            // Create at center of canvas
            ImVec2 pos = ImVec2(0.0f, 0.0f);
            CreateNodeAtPosition(def.typeId, pos);
        }
        
        // Tooltip
        if (ImGui::IsItemHovered() && !def.description.empty()) {
            ImGui::SetTooltip("%s", def.description.c_str());
        }
        
        // Drag source for dropping on canvas
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            std::string fullId = def.typeId.Full();
            ImGui::SetDragDropPayload("SG_NODE_TYPE", fullId.c_str(), fullId.size() + 1);
            ImGui::Text("Create %s", def.displayName.c_str());
            ImGui::EndDragDropSource();
        }
    }
    
    ImGui::EndChild();
}

void ShaderGraphPanel::DrawGraphCanvas() {
    if (!m_Graph) return;
    
    ImNodes::EditorContextSet(m_NodeEditorContext);
    ImNodes::BeginNodeEditor();
    
    const ImVec2 editorOrigin = ImGui::GetCursorScreenPos();
    
    auto screenToGrid = [&](const ImVec2& screen) {
        ImVec2 pan = ImNodes::EditorContextGetPanning();
        return ImVec2(screen.x - editorOrigin.x - pan.x, screen.y - editorOrigin.y - pan.y);
    };
    
    // Draw all nodes
    for (const auto& node : m_Graph->nodes) {
        // Position seeding
        if (!m_PositionSeed.count(node.id)) {
            ImNodes::SetNodeGridSpacePos(node.id, ImVec2(node.editorPos.x, node.editorPos.y));
            m_PositionSeed.insert(node.id);
        }
        
        if (m_PendingNewNodeId == node.id) {
            ImNodes::SetNodeGridSpacePos(node.id, m_PendingNewNodePos);
        }
        
        // Node color based on category
        bool isOutput = (node.typeId.category == "Output");
        bool isInput = (node.typeId.category == "Input");
        bool isTexture = (node.typeId.category == "Texture");
        bool isSelected = (m_SelectedNodeId == node.id);
        
        ImU32 titleColor = ImColor(70, 100, 150);
        if (isOutput) titleColor = ImColor(150, 90, 70);
        else if (isInput) titleColor = ImColor(90, 130, 90);
        else if (isTexture) titleColor = ImColor(130, 90, 130);
        if (isSelected) titleColor = ImColor(200, 150, 60);
        
        ImNodes::PushColorStyle(ImNodesCol_TitleBar, titleColor);
        ImNodes::PushColorStyle(ImNodesCol_TitleBarHovered, titleColor);
        ImNodes::PushColorStyle(ImNodesCol_TitleBarSelected, titleColor);
        
        ImNodes::BeginNode(node.id);
        
        // Title bar
        ImNodes::BeginNodeTitleBar();
        ImGui::TextUnformatted(node.displayName.c_str());
        ImNodes::EndNodeTitleBar();
        
        // Input pins
        for (PinId pinId : node.inputPins) {
            const ShaderPin* pin = m_Graph->FindPin(pinId);
            if (!pin) continue;
            
            ImNodes::PushColorStyle(ImNodesCol_Pin, GetPinColor(pin->type));
            ImNodes::BeginInputAttribute(MakeInputAttr(pinId));
            
            // Show indicator if exposed
            if (pin->exposed) {
                ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.3f, 1.0f), "*");
                ImGui::SameLine();
            }
            
            ImGui::Text("%s", pin->name.c_str());
            ImNodes::EndInputAttribute();
            ImNodes::PopColorStyle();
        }
        
        // Output pins
        for (PinId pinId : node.outputPins) {
            const ShaderPin* pin = m_Graph->FindPin(pinId);
            if (!pin) continue;
            
            ImNodes::PushColorStyle(ImNodesCol_Pin, GetPinColor(pin->type));
            ImNodes::BeginOutputAttribute(MakeOutputAttr(pinId));
            
            float textWidth = ImGui::CalcTextSize(pin->name.c_str()).x;
            ImGui::Indent(100.0f - textWidth);
            ImGui::Text("%s", pin->name.c_str());
            
            ImNodes::EndOutputAttribute();
            ImNodes::PopColorStyle();
        }
        
        ImNodes::EndNode();
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();
    }
    
    // Draw all links
    for (const auto& link : m_Graph->links) {
        bool selected = (link.id == m_SelectedLinkId);
        
        const ShaderPin* fromPin = m_Graph->FindPin(link.fromPin);
        ImU32 linkColor = fromPin ? GetPinColor(fromPin->type) : ImColor(200, 200, 200);
        
        if (selected) {
            ImNodes::PushColorStyle(ImNodesCol_Link, ImColor(240, 196, 45));
        } else {
            ImNodes::PushColorStyle(ImNodesCol_Link, linkColor);
        }
        
        ImNodes::Link(link.id, MakeOutputAttr(link.fromPin), MakeInputAttr(link.toPin));
        ImNodes::PopColorStyle();
    }
    
    // Context menu handling
    bool requestCanvasMenu = false;
    ImVec2 canvasSpawnPos = ImVec2(0.0f, 0.0f);
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ImNodes::IsEditorHovered()) {
        canvasSpawnPos = screenToGrid(ImGui::GetMousePos());
        requestCanvasMenu = true;
    }
    
    // Drag-drop target for node creation
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SG_NODE_TYPE")) {
            std::string fullId((const char*)payload->Data);
            const NodeDefinition* def = NodeRegistry::Instance().Find(fullId);
            if (def) {
                ImVec2 dropPos = screenToGrid(ImGui::GetMousePos());
                CreateNodeAtPosition(def->typeId, dropPos);
            }
        }
        ImGui::EndDragDropTarget();
    }
    
    ImNodes::EndNodeEditor();
    
    // Handle context menus
    if (requestCanvasMenu) {
        m_PendingSpawnGridPos = canvasSpawnPos;
        m_HasSpawnPos = true;
        ImGui::OpenPopup("SG_CanvasContext");
    }
    DrawCanvasContextMenu();
    
    // Handle node selection
    int hoveredNode = -1;
    if (ImNodes::IsNodeHovered(&hoveredNode)) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            m_SelectedNodeId = hoveredNode;
            m_SelectedLinkId = -1;
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
            m_ContextNodeId = hoveredNode;
            ImGui::OpenPopup("SG_NodeContext");
        }
    }
    DrawNodeContextMenu();
    
    // Handle link selection
    int hoveredLink = -1;
    if (ImNodes::IsLinkHovered(&hoveredLink)) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            m_SelectedLinkId = hoveredLink;
            m_SelectedNodeId = -1;
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
            m_ContextLinkId = hoveredLink;
            ImGui::OpenPopup("SG_LinkContext");
        }
    }
    DrawLinkContextMenu();
    
    // Handle link creation
    int startAttr = 0, endAttr = 0;
    if (ImNodes::IsLinkCreated(&startAttr, &endAttr)) {
        PinId fromPin, toPin;
        
        if (IsOutputAttr(startAttr)) {
            fromPin = AttrToPin(startAttr);
            toPin = AttrToPin(endAttr);
        } else {
            fromPin = AttrToPin(endAttr);
            toPin = AttrToPin(startAttr);
        }
        
        if (m_Graph->CanConnect(fromPin, toPin)) {
            m_Graph->AddLink(fromPin, toPin);
            m_Modified = true;
        }
    }
    
    // Handle link deletion
    int destroyedLink = -1;
    if (ImNodes::IsLinkDestroyed(&destroyedLink)) {
        m_Graph->RemoveLink(destroyedLink);
        m_Modified = true;
        if (m_SelectedLinkId == destroyedLink) {
            m_SelectedLinkId = -1;
        }
    }
    
    // Update node positions (only for nodes that have been drawn)
    for (auto& node : m_Graph->nodes) {
        // Only get position if the node has been drawn at least once
        if (m_PositionSeed.count(node.id)) {
            ImVec2 pos = ImNodes::GetNodeGridSpacePos(node.id);
            node.editorPos.x = pos.x;
            node.editorPos.y = pos.y;
        }
    }
    
    // Frame all nodes if requested (defer until next frame when nodes are drawn)
    if (m_FrameGraphNext) {
        m_FrameGraphNext = false;
        // Only frame if all nodes have been drawn
        bool allNodesDrawn = true;
        for (const auto& node : m_Graph->nodes) {
            if (!m_PositionSeed.count(node.id)) {
                allNodesDrawn = false;
                m_FrameGraphNext = true;  // Try again next frame
                break;
            }
        }
        if (allNodesDrawn) {
            FrameAllNodes();
        }
    }
    
    ApplyPendingNodePlacement();
}

void ShaderGraphPanel::EnsurePreviewScene() {
    if (m_Preview) {
        return;
    }

    m_Preview = std::make_unique<ShaderPreviewState>();
    ShaderPreviewState& preview = *m_Preview;
    preview.fallbackMaterial = MaterialManager::Instance().CreateSceneDefaultMaterial(&preview.scene);

    Environment& env = preview.scene.GetEnvironment();
    env.Ambient = Environment::AmbientMode::FlatColor;
    env.AmbientColor = glm::vec3(0.18f, 0.19f, 0.22f);
    env.AmbientIntensity = 0.8f;
    env.EnableFog = false;
    env.UseSkybox = false;
    env.ProceduralSky = false;

    Entity sphere = preview.scene.CreateEntity("Shader Preview Sphere");
    preview.sphereEntity = sphere.GetID();
    if (auto* data = preview.scene.GetEntityData(preview.sphereEntity)) {
        data->Mesh = std::make_unique<MeshComponent>();
        data->Mesh->mesh = StandardMeshManager::Instance().GetSphereMesh();
        data->Mesh->MeshName = "Sphere";
        data->Mesh->material = preview.fallbackMaterial;
        data->Mesh->materials = { preview.fallbackMaterial };
        data->Transform.Position = glm::vec3(0.0f);
    }

    Entity keyLight = preview.scene.CreateLight("Shader Preview Key", LightType::Point, glm::vec3(1.0f, 0.97f, 0.92f), 3.25f);
    preview.keyLightEntity = keyLight.GetID();
    if (auto* data = preview.scene.GetEntityData(preview.keyLightEntity)) {
        data->Transform.Position = glm::vec3(2.2f, 1.7f, 2.6f);
    }

    Entity fillLight = preview.scene.CreateLight("Shader Preview Fill", LightType::Point, glm::vec3(0.72f, 0.8f, 1.0f), 0.9f);
    preview.fillLightEntity = fillLight.GetID();
    if (auto* data = preview.scene.GetEntityData(preview.fillLightEntity)) {
        data->Transform.Position = glm::vec3(-2.4f, 0.65f, 1.1f);
    }

    preview.scene.UpdateTransforms();
}

void ShaderGraphPanel::RefreshPreviewMaterial(bool keepStatusMessage) {
    EnsurePreviewScene();

    ShaderPreviewState& preview = *m_Preview;
    if (!preview.fallbackMaterial) {
        preview.fallbackMaterial = MaterialManager::Instance().CreateSceneDefaultMaterial(&preview.scene);
    }

    preview.previewMaterial.reset();
    preview.hasCompiledPreview = false;

    std::shared_ptr<Material> nextMaterial = preview.fallbackMaterial;
    std::string nextStatus = preview.statusMessage;

    if (!m_Graph || m_OpenPath.empty()) {
        nextStatus = "Save and compile the graph to preview the current shader.";
    } else if (!m_Graph->isCompiled) {
        nextStatus = "Compile the graph to preview the current shader.";
    } else {
        ShaderGraphMaterialDesc desc;
        desc.name = m_Graph->name.empty() ? "ShaderGraphPreview" : m_Graph->name + "_Preview";
        desc.shaderGraphPath = m_OpenPath;
        desc.vertexShaderName = m_Graph->compiledVSName;
        desc.fragmentShaderName = m_Graph->compiledFSName;
        ShaderGraphCodeGen codegen(*m_Graph);
        ShaderCompileResult compileResult = codegen.Compile();
        if (compileResult.success) {
            desc.parameters = BuildMaterialParametersFromCompileResult(compileResult);
        }

        auto previewMaterial = ShaderGraphMaterial::CreateFromDesc(desc);
        if (previewMaterial) {
            preview.previewMaterial = previewMaterial;
            preview.hasCompiledPreview = true;
            nextMaterial = preview.previewMaterial;
            nextStatus.clear();
        } else {
            nextStatus = "Preview could not load the compiled shader.";
        }
    }

    if (!keepStatusMessage || !preview.hasCompiledPreview) {
        preview.statusMessage = nextStatus;
    }

    if (auto* data = preview.scene.GetEntityData(preview.sphereEntity); data && data->Mesh) {
        data->Mesh->material = nextMaterial;
        if (data->Mesh->materials.empty()) {
            data->Mesh->materials.push_back(nextMaterial);
        } else {
            data->Mesh->materials[0] = nextMaterial;
        }
    }
}

void ShaderGraphPanel::DrawPreviewPanel(float width) {
    EnsurePreviewScene();

    ShaderPreviewState& preview = *m_Preview;

    ImGui::TextUnformatted("Preview");
    ImGui::Separator();

    const float availableWidth = std::max(1.0f, std::min(width, ImGui::GetContentRegionAvail().x));
    const float previewHeight = std::clamp(availableWidth * 0.8f, kPreviewMinHeight, kPreviewMaxHeight);

    ImGui::BeginChild("SG_PreviewViewport", ImVec2(0.0f, previewHeight), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const ImVec2 viewportSize = ImGui::GetContentRegionAvail();
    if (viewportSize.x >= 1.0f && viewportSize.y >= 1.0f) {
        const float aspect = viewportSize.x / std::max(1.0f, viewportSize.y);
        preview.camera.SetPerspective(40.0f, aspect, 0.1f, 32.0f);

        const float yawRad = glm::radians(preview.yaw);
        const float pitchRad = glm::radians(preview.pitch);
        const glm::vec3 cameraPosition = preview.target + glm::vec3(
            preview.distance * std::cos(pitchRad) * std::sin(yawRad),
            preview.distance * std::sin(pitchRad),
            preview.distance * std::cos(pitchRad) * std::cos(yawRad));

        preview.camera.SetPosition(cameraPosition);
        preview.camera.LookAt(preview.target);
        preview.scene.UpdateTransforms();

        bgfx::TextureHandle tex = Renderer::Get().RenderSceneToTexture(
            &preview.scene,
            static_cast<uint32_t>(std::max(1.0f, viewportSize.x)),
            static_cast<uint32_t>(std::max(1.0f, viewportSize.y)),
            &preview.camera,
            ShaderPreviewState::kViewIdBase,
            false,
            0x1c1e24ff,
            false,
            nullptr,
            true);

        if (bgfx::isValid(tex)) {
            ImGui::Image(TextureLoader::ToImGuiTextureID(tex), viewportSize, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));

            if (ImGui::IsItemHovered()) {
                const ImVec2 delta = ImGui::GetIO().MouseDelta;
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    preview.yaw += delta.x * 0.4f;
                    preview.pitch = std::clamp(preview.pitch - delta.y * 0.4f, -80.0f, 80.0f);
                }

                if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
                    const glm::vec3 forward = glm::normalize(preview.target - cameraPosition);
                    const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
                    const glm::vec3 up = glm::normalize(glm::cross(right, forward));
                    preview.target -= right * (delta.x * 0.01f * preview.distance);
                    preview.target += up * (delta.y * 0.01f * preview.distance);
                }

                const float wheel = ImGui::GetIO().MouseWheel;
                if (wheel != 0.0f) {
                    preview.distance = std::clamp(preview.distance - wheel * 0.35f, 1.5f, 10.0f);
                }
            }

            if (!preview.hasCompiledPreview && !preview.statusMessage.empty()) {
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                const ImVec2 imageMin = ImGui::GetItemRectMin();
                const ImVec2 imageMax = ImGui::GetItemRectMax();
                const float padding = 12.0f;
                drawList->AddRectFilled(
                    ImVec2(imageMin.x + padding, imageMax.y - 34.0f),
                    ImVec2(imageMax.x - padding, imageMax.y - 10.0f),
                    IM_COL32(18, 20, 24, 220),
                    6.0f);
                drawList->AddText(
                    ImVec2(imageMin.x + padding * 1.5f, imageMax.y - 29.0f),
                    IM_COL32(220, 223, 228, 255),
                    preview.statusMessage.c_str());
            }
        } else {
            ImGui::TextDisabled("Preview render target unavailable.");
        }
    } else {
        ImGui::TextDisabled("Preview viewport is too small.");
    }

    ImGui::EndChild();
}

void ShaderGraphPanel::DrawPropertiesPanel(float width) {
    (void)width;
    ImGui::TextUnformatted("Properties");
    ImGui::Separator();
    ImGui::BeginChild("SG_Properties", ImVec2(0.0f, 0.0f), false);

    if (m_SelectedNodeId < 0) {
        ImGui::TextDisabled("Select a node to edit its properties.");
        ImGui::EndChild();
        return;
    }
    
    ShaderNode* node = m_Graph->FindNode(m_SelectedNodeId);
    if (!node) {
        ImGui::TextDisabled("Node not found.");
        ImGui::EndChild();
        return;
    }
    
    // Node type info
    ImGui::Text("Type: %s", node->typeId.Full().c_str());
    ImGui::Separator();
    
    // Display name
    char nameBuf[128];
    std::strncpy(nameBuf, node->displayName.c_str(), sizeof(nameBuf));
    nameBuf[sizeof(nameBuf) - 1] = 0;
    if (ImGui::InputText("Display Name", nameBuf, sizeof(nameBuf))) {
        node->displayName = nameBuf;
        m_Modified = true;
    }
    
    // Node properties (e.g., swizzle mask)
    const NodeDefinition* def = NodeRegistry::Instance().Find(node->typeId);
    if (def && !def->properties.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("Node Settings");
        
        for (const auto& propDef : def->properties) {
            std::string value = node->GetProperty(propDef.key, propDef.defaultValue);
            
            if (!propDef.options.empty()) {
                // Dropdown
                int current = 0;
                for (int i = 0; i < (int)propDef.options.size(); ++i) {
                    if (propDef.options[i] == value) {
                        current = i;
                        break;
                    }
                }
                
                if (ImGui::BeginCombo(propDef.displayName.c_str(), propDef.options[current].c_str())) {
                    for (int i = 0; i < (int)propDef.options.size(); ++i) {
                        bool selected = (current == i);
                        if (ImGui::Selectable(propDef.options[i].c_str(), selected)) {
                            node->SetProperty(propDef.key, propDef.options[i]);
                            m_Modified = true;
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            } else {
                // Text input
                char buf[256];
                std::strncpy(buf, value.c_str(), sizeof(buf));
                buf[sizeof(buf) - 1] = 0;
                if (ImGui::InputText(propDef.displayName.c_str(), buf, sizeof(buf))) {
                    node->SetProperty(propDef.key, buf);
                    m_Modified = true;
                }
            }
        }
    }
    
    // Input pins
    ImGui::Separator();
    ImGui::TextDisabled("Inputs");
    bool refreshPreviewForDefaults = false;
    
    for (PinId pinId : node->inputPins) {
        ShaderPin* pin = m_Graph->FindPin(pinId);
        if (!pin) continue;
        
        ImGui::PushID(pinId);
        
        // Pin name header
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s", pin->name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", ValueTypeToGLSL(pin->type));
        
        // Check if connected
        const ShaderLink* link = m_Graph->FindLinkToPin(pinId);
        if (link) {
            ImGui::TextDisabled("  [Connected]");
        } else {
            // Default value editor
            switch (pin->type) {
                case ShaderValueType::Float:
                    if (ImGui::DragFloat("##value", &pin->defaultValue.x, 0.01f)) {
                        m_Modified = true;
                        refreshPreviewForDefaults = true;
                    }
                    break;
                case ShaderValueType::Float2:
                    if (ImGui::DragFloat2("##value", &pin->defaultValue.x, 0.01f)) {
                        m_Modified = true;
                        refreshPreviewForDefaults = true;
                    }
                    break;
                case ShaderValueType::Float3:
                    if (ImGui::DragFloat3("##value", &pin->defaultValue.x, 0.01f)) {
                        m_Modified = true;
                        refreshPreviewForDefaults = true;
                    }
                    break;
                case ShaderValueType::Float4:
                    if (ImGui::DragFloat4("##value", &pin->defaultValue.x, 0.01f)) {
                        m_Modified = true;
                        refreshPreviewForDefaults = true;
                    }
                    break;
                case ShaderValueType::Color3:
                    if (ImGui::ColorEdit3("##value", &pin->defaultValue.x)) {
                        m_Modified = true;
                        refreshPreviewForDefaults = true;
                    }
                    break;
                case ShaderValueType::Color4:
                    if (ImGui::ColorEdit4("##value", &pin->defaultValue.x)) {
                        m_Modified = true;
                        refreshPreviewForDefaults = true;
                    }
                    break;
                case ShaderValueType::Int:
                    {
                        int v = static_cast<int>(pin->defaultValue.x);
                        if (ImGui::DragInt("##value", &v)) {
                            pin->defaultValue.x = static_cast<float>(v);
                            m_Modified = true;
                            refreshPreviewForDefaults = true;
                        }
                    }
                    break;
                case ShaderValueType::Bool:
                    {
                        bool v = pin->defaultValue.x > 0.5f;
                        if (ImGui::Checkbox("##value", &v)) {
                            pin->defaultValue.x = v ? 1.0f : 0.0f;
                            m_Modified = true;
                            refreshPreviewForDefaults = true;
                        }
                    }
                    break;
                case ShaderValueType::Texture2D:
                case ShaderValueType::Sampler2D:
                    {
                        bool requestPicker = false;
                        const std::string popupId = "SGPinTexPicker_" + std::to_string(pin->id);
                        const ImVec2 previewSize(64.0f, 64.0f);
                        const std::string normalizedPath = NormalizeTextureAssetPath(pin->defaultTexturePath);
                        if (normalizedPath != pin->defaultTexturePath) {
                            pin->defaultTexturePath = normalizedPath;
                            m_Modified = true;
                        }

                        bgfx::TextureHandle previewHandle = BGFX_INVALID_HANDLE;
                        if (!pin->defaultTexturePath.empty()) {
                            TextureSpecifier spec;
                            spec.Path = pin->defaultTexturePath;
                            previewHandle = AcquireTextureHandle(spec, TextureColorSpace::Linear);
                        }

                        if (bgfx::isValid(previewHandle)) {
                            if (ImGui::ImageButton("##valueTexture", TextureLoader::ToImGuiTextureID(previewHandle), previewSize)) {
                                requestPicker = true;
                            }
                        } else {
                            if (ImGui::Button("(no texture)", previewSize)) {
                                requestPicker = true;
                            }
                        }

                        if (ImGui::IsItemHovered() && !pin->defaultTexturePath.empty()) {
                            ImGui::SetTooltip("%s", pin->defaultTexturePath.c_str());
                        }

                        if (ImGui::BeginDragDropTarget()) {
                            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
                                const char* droppedPath = static_cast<const char*>(payload->Data);
                                if (droppedPath) {
                                    std::string ext = std::filesystem::path(droppedPath).extension().string();
                                    if (IsTexturePathExtension(ext)) {
                                        pin->defaultTexturePath = NormalizeTextureAssetPath(droppedPath);
                                        m_Modified = true;
                                        refreshPreviewForDefaults = true;
                                    }
                                }
                            }
                            ImGui::EndDragDropTarget();
                        }

                        ImGui::SameLine();
                        if (ImGui::SmallButton("Pick")) {
                            requestPicker = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Clear")) {
                            if (!pin->defaultTexturePath.empty()) {
                                pin->defaultTexturePath.clear();
                                m_Modified = true;
                                refreshPreviewForDefaults = true;
                            }
                        }

                        if (requestPicker) {
                            ImGui::OpenPopup(popupId.c_str());
                        }

                        texturepicker::DrawTexturePickerPopup(
                            popupId.c_str(),
                            [this, pin, &refreshPreviewForDefaults](const std::string& selectedPath) {
                                if (!pin) {
                                    return;
                                }
                                pin->defaultTexturePath = NormalizeTextureAssetPath(selectedPath);
                                m_Modified = true;
                                refreshPreviewForDefaults = true;
                            },
                            pin->defaultTexturePath);

                        if (!pin->defaultTexturePath.empty()) {
                            ImGui::TextDisabled("%s", std::filesystem::path(pin->defaultTexturePath).filename().string().c_str());
                        }
                    }
                    break;
                default:
                    break;
            }
        }
        
        // Expose as parameter toggle
        if (ImGui::Checkbox("Expose", &pin->exposed)) {
            m_Modified = true;
        }
        
        if (pin->exposed) {
            ImGui::Indent();
            char expNameBuf[128];
            std::string displayedName = pin->exposedName.empty() ? pin->name : pin->exposedName;
            std::strncpy(expNameBuf, displayedName.c_str(), sizeof(expNameBuf));
            expNameBuf[sizeof(expNameBuf) - 1] = 0;
            
            if (ImGui::InputText("Parameter Name", expNameBuf, sizeof(expNameBuf))) {
                pin->exposedName = expNameBuf;
                m_Modified = true;
            }
            ImGui::Unindent();
        }
        
        ImGui::Spacing();
        ImGui::PopID();
    }

    if (refreshPreviewForDefaults && m_Graph && m_Graph->isCompiled) {
        RefreshPreviewMaterial();
    }

    ImGui::EndChild();
}

void ShaderGraphPanel::DrawErrorPanel(float height) {
    ImGui::BeginChild("SG_ErrorPanel", ImVec2(0, height), true);
    
    ImGui::TextUnformatted("Compilation Output");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        m_CompileErrors.clear();
        m_CompileWarnings.clear();
        m_ShowErrors = false;
    }
    ImGui::Separator();
    
    // Errors
    for (const auto& err : m_CompileErrors) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "[ERROR] %s", err.c_str());
    }
    
    // Warnings
    for (const auto& warn : m_CompileWarnings) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "[WARN] %s", warn.c_str());
    }
    
    if (m_CompileErrors.empty() && m_CompileWarnings.empty()) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Compilation successful!");
    }
    
    ImGui::EndChild();
}

void ShaderGraphPanel::DrawCanvasContextMenu() {
    if (ImGui::BeginPopup("SG_CanvasContext")) {
        // Add node submenu
        if (ImGui::BeginMenu("Add Node")) {
            const auto& registry = NodeRegistry::Instance();
            std::string lastCategory;
            
            for (const auto& def : registry.GetAll()) {
                if (def.typeId.category != lastCategory) {
                    if (!lastCategory.empty()) ImGui::Separator();
                    ImGui::TextDisabled("%s", def.typeId.category.c_str());
                    lastCategory = def.typeId.category;
                }
                
                if (ImGui::MenuItem(def.displayName.c_str())) {
                    ImVec2 pos = m_HasSpawnPos ? m_PendingSpawnGridPos : ImVec2(0.0f, 0.0f);
                    CreateNodeAtPosition(def.typeId, pos);
                }
            }
            ImGui::EndMenu();
        }
        
        ImGui::Separator();
        if (ImGui::MenuItem("Frame All")) {
            FrameAllNodes();
        }
        
        ImGui::EndPopup();
    }
    m_HasSpawnPos = false;
}

void ShaderGraphPanel::DrawNodeContextMenu() {
    if (ImGui::BeginPopup("SG_NodeContext")) {
        ShaderNode* node = m_Graph->FindNode(m_ContextNodeId);
        if (node) {
            ImGui::Text("%s", node->displayName.c_str());
            ImGui::Separator();
            
            if (ImGui::MenuItem("Delete")) {
                m_Graph->RemoveNode(m_ContextNodeId);
                m_Modified = true;
                if (m_SelectedNodeId == m_ContextNodeId) {
                    m_SelectedNodeId = -1;
                }
            }
        }
        ImGui::EndPopup();
    }
}

void ShaderGraphPanel::DrawLinkContextMenu() {
    if (ImGui::BeginPopup("SG_LinkContext")) {
        if (ImGui::MenuItem("Delete Link")) {
            m_Graph->RemoveLink(m_ContextLinkId);
            m_Modified = true;
            if (m_SelectedLinkId == m_ContextLinkId) {
                m_SelectedLinkId = -1;
            }
        }
        ImGui::EndPopup();
    }
}

void ShaderGraphPanel::CreateNodeAtPosition(const NodeTypeId& typeId, const ImVec2& gridPos) {
    NodeId newId = NodeRegistry::Instance().CreateNode(*m_Graph, typeId, glm::vec2(gridPos.x, gridPos.y));
    if (newId > 0) {
        m_PendingNewNodeId = newId;
        m_PendingNewNodePos = gridPos;
        m_SelectedNodeId = newId;
        m_SelectedLinkId = -1;
        m_Modified = true;
    }
}

void ShaderGraphPanel::DeleteSelectedNodes() {
    if (m_SelectedNodeId >= 0) {
        m_Graph->RemoveNode(m_SelectedNodeId);
        m_SelectedNodeId = -1;
        m_Modified = true;
    }
}

void ShaderGraphPanel::DeleteSelectedLinks() {
    if (m_SelectedLinkId >= 0) {
        m_Graph->RemoveLink(m_SelectedLinkId);
        m_SelectedLinkId = -1;
        m_Modified = true;
    }
}

void ShaderGraphPanel::HandleKeyboardShortcuts() {
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;
    const ImGuiIO& io = ImGui::GetIO();
    
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N, false)) {
        NewGraph();
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) {
        std::string path = ShowOpenFileDialogExt(L"Shader Graph (*.shgraph)", L"shgraph");
        if (!path.empty()) OpenGraph(path);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        if (m_SelectedNodeId >= 0) DeleteSelectedNodes();
        else if (m_SelectedLinkId >= 0) DeleteSelectedLinks();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        FrameAllNodes();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
        CompileGraph();
    }
}

int ShaderGraphPanel::MakeInputAttr(PinId pinId) const {
    return pinId * 2;  // Even numbers for inputs
}

int ShaderGraphPanel::MakeOutputAttr(PinId pinId) const {
    return pinId * 2 + 1;  // Odd numbers for outputs
}

bool ShaderGraphPanel::IsOutputAttr(int attrId) const {
    return (attrId % 2) == 1;
}

PinId ShaderGraphPanel::AttrToPin(int attrId) const {
    return attrId / 2;
}

ImU32 ShaderGraphPanel::GetPinColor(ShaderValueType type) const {
    switch (type) {
        case ShaderValueType::Float:
            return ImColor(150, 200, 150);
        case ShaderValueType::Float2:
            return ImColor(150, 200, 200);
        case ShaderValueType::Float3:
            return ImColor(200, 200, 150);
        case ShaderValueType::Float4:
            return ImColor(200, 150, 200);
        case ShaderValueType::Int:
            return ImColor(100, 200, 200);
        case ShaderValueType::Bool:
            return ImColor(200, 100, 100);
        case ShaderValueType::Color3:
        case ShaderValueType::Color4:
            return ImColor(255, 200, 100);
        case ShaderValueType::Texture2D:
        case ShaderValueType::Sampler2D:
            return ImColor(255, 100, 150);
        case ShaderValueType::Any:
            return ImColor(200, 200, 200);
        default:
            return ImColor(150, 150, 150);
    }
}

void ShaderGraphPanel::ApplyPendingNodePlacement() {
    if (m_PendingNewNodeId >= 0) {
        ImNodes::SetNodeGridSpacePos(m_PendingNewNodeId, m_PendingNewNodePos);
        ShaderNode* node = m_Graph->FindNode(m_PendingNewNodeId);
        if (node) {
            node->editorPos.x = m_PendingNewNodePos.x;
            node->editorPos.y = m_PendingNewNodePos.y;
        }
        m_PendingNewNodeId = -1;
    }
}

void ShaderGraphPanel::FrameAllNodes() {
    if (!m_Graph || m_Graph->nodes.empty()) {
        ImNodes::EditorContextResetPanning(ImVec2(0, 0));
        return;
    }
    
    ImVec2 minPos(FLT_MAX, FLT_MAX);
    ImVec2 maxPos(-FLT_MAX, -FLT_MAX);
    
    for (const auto& node : m_Graph->nodes) {
        ImVec2 pos;
        // Only get position from ImNodes if the node has been drawn
        if (m_PositionSeed.count(node.id)) {
            pos = ImNodes::GetNodeGridSpacePos(node.id);
        } else {
            // Use stored position for nodes that haven't been drawn yet
            pos = ImVec2(node.editorPos.x, node.editorPos.y);
        }
        minPos.x = std::min(minPos.x, pos.x);
        minPos.y = std::min(minPos.y, pos.y);
        maxPos.x = std::max(maxPos.x, pos.x + 150.0f);  // Approximate node width
        maxPos.y = std::max(maxPos.y, pos.y + 100.0f);  // Approximate node height
    }
    
    ImVec2 center((minPos.x + maxPos.x) * 0.5f, (minPos.y + maxPos.y) * 0.5f);
    ImNodes::EditorContextResetPanning(ImVec2(-center.x, -center.y));
}

} // namespace shadergraph

