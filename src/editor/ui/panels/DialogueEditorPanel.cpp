#include "DialogueEditorPanel.h"
#include <imgui.h>
#include <imgui_clay_inspector.h>
#include <imgui_internal.h>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <regex>
#include <iostream>
#include <unordered_set>
#include <filesystem>
#include "editor/pipeline/AssetRegistry.h"
#include "editor/pipeline/AssetLibrary.h"

// Platform file dialogs
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>
// Undefine Windows macros that conflict with our method names
#undef LoadLibrary
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif

static uint64_t Fnv1a64(const std::string& data, uint64_t seed) {
    uint64_t hash = 14695981039346656037ULL ^ seed;
    for (unsigned char c : data) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static ClaymoreGUID DeterministicGuidFromPath(const std::string& path) {
    if (path.empty()) return ClaymoreGUID();

    std::string normalized = path;
    for (char& c : normalized) {
        if (c == '\\') c = '/';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    uint64_t high = Fnv1a64(normalized, 0);
    uint64_t low = Fnv1a64(normalized, 0x9e3779b97f4a7c15ULL);
    return ClaymoreGUID(high, low);
}

//------------------------------------------------------------------------------
// Constructor/Destructor
//------------------------------------------------------------------------------
DialogueEditorPanel::DialogueEditorPanel()
{
    // Initialize auto-complete items
    m_AutoCompleteItems = GetCommandCompletions();
}

DialogueEditorPanel::~DialogueEditorPanel() = default;

//------------------------------------------------------------------------------
// Library Management
//------------------------------------------------------------------------------
bool DialogueEditorPanel::NewDialogueLibrary()
{
    if (m_Modified && !ConfirmDiscard()) return false;
    
    m_Library = std::make_shared<Dialogue::DialogueLibrary>();
    m_Library->SetGuid(ClaymoreGUID::Generate());
    m_Library->SetDisplayName("New Dialogue Library");
    m_OpenPath.clear();
    m_SelectedEntryIndex = -1;
    m_SelectedEntry = nullptr;
    m_EditorText.clear();
    m_Modified = false;
    m_ValidationErrors.clear();
    m_UndoStack.clear();
    m_RedoStack.clear();
    
    return true;
}

bool DialogueEditorPanel::LoadDialogueLibrary(const std::string& path)
{
    if (m_Modified && !ConfirmDiscard()) return false;
    
    auto library = Dialogue::DialogueLibrary::LoadFromFile(path);
    if (!library) {
        std::cerr << "[DialogueEditor] Failed to load: " << path << std::endl;
        return false;
    }
    
    m_Library = std::shared_ptr<Dialogue::DialogueLibrary>(std::move(library));
    m_OpenPath = path;
    m_SelectedEntryIndex = -1;
    m_SelectedEntry = nullptr;
    m_EditorText.clear();
    m_Modified = false;
    m_ValidationErrors.clear();
    m_UndoStack.clear();
    m_RedoStack.clear();
    
    // Select first entry if available
    if (!m_Library->GetEntries().empty()) {
        m_SelectedEntryIndex = 0;
        m_SelectedEntry = &m_Library->GetEntries()[0];
        m_EditorText = m_SelectedEntry->rawText;
    }
    
    UpdateKnownSpeakers();
    
    std::cout << "[DialogueEditor] Loaded: " << path << std::endl;
    return true;
}

bool DialogueEditorPanel::SaveDialogueLibrary(const std::string& path)
{
    if (!m_Library) return false;
    
    std::string savePath = path.empty() ? m_OpenPath : path;
    if (savePath.empty()) {
        return SaveLibraryDialog();
    }
    
    // Update current entry text
    if (m_SelectedEntry) {
        m_SelectedEntry->rawText = m_EditorText;
        m_SelectedEntry->InvalidateCache();
    }

    // Ensure library GUID is valid and stable
    ClaymoreGUID prevGuid = m_Library->GetGuid();
    const AssetMetadata* existingMeta = AssetRegistry::Instance().GetMetadata(savePath);
    ClaymoreGUID metaGuid = existingMeta ? existingMeta->guid : ClaymoreGUID();
    if (prevGuid == ClaymoreGUID()) {
        if (metaGuid != ClaymoreGUID()) {
            m_Library->SetGuid(metaGuid);
        } else {
            m_Library->SetGuid(DeterministicGuidFromPath(savePath));
        }
        prevGuid = metaGuid;
    }

    if (m_Library->SaveToFile(savePath)) {
        m_OpenPath = savePath;
        m_Modified = false;
        std::cout << "[DialogueEditor] Saved: " << savePath << std::endl;
        
        // Invalidate runtime registry cache so dialogue reloads fresh at runtime
        // This ensures changes are picked up without restarting play mode
        auto guid = m_Library->GetGuid();
        if (guid.high != 0 || guid.low != 0) {
            // Unregister old cached version
            Dialogue::DialogueLibraryRegistry::Get().Unregister(guid);
            // Re-register with updated library
            Dialogue::DialogueLibraryRegistry::Get().Register(guid, m_Library);
            std::cout << "[DialogueEditor] Registry cache refreshed for GUID: " << guid.ToString() << std::endl;
        }

        // Keep asset registry/library in sync so inspector dropdowns don't duplicate
        AssetMetadata meta = existingMeta ? *existingMeta : AssetMetadata{};
        meta.sourcePath = savePath;
        meta.processedPath = savePath;
        meta.type = "dialogue";
        meta.guid = guid;
        meta.reference = AssetReference(guid, 0, static_cast<int32_t>(AssetType::Scriptable));
        AssetRegistry::Instance().SetMetadata(savePath, meta);

        if (prevGuid != ClaymoreGUID() && prevGuid != guid) {
            AssetLibrary::Instance().UnregisterAsset(AssetReference(prevGuid, 0, static_cast<int32_t>(AssetType::Scriptable)));
        }
        std::string assetName = std::filesystem::path(savePath).stem().string();
        AssetLibrary::Instance().RegisterAsset(meta.reference, AssetType::Scriptable, savePath, assetName);

        return true;
    }
    
    return false;
}

void DialogueEditorPanel::SetLibrary(std::shared_ptr<Dialogue::DialogueLibrary> library)
{
    m_Library = library;
    m_SelectedEntryIndex = -1;
    m_SelectedEntry = nullptr;
    m_EditorText.clear();
    m_Modified = false;
    UpdateKnownSpeakers();
}

//------------------------------------------------------------------------------
// Main Render
//------------------------------------------------------------------------------
void DialogueEditorPanel::OnImGuiRender()
{
    if (!m_Open) {
        m_WindowFocusedOrHovered = false;
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(1200, 800), ImGuiCond_FirstUseEver);
    
    std::string title = GetWindowTitle();
    if (!ImGui::Begin(title.c_str(), &m_Open, ImGuiWindowFlags_MenuBar)) {
        m_WindowFocusedOrHovered = false;
        ImGui::End();
        return;
    }
    m_WindowFocusedOrHovered =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ||
        ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);

    DrawMenuBar();
    DrawToolbar();

    // Main content area with three columns + bottom preview
    const float availWidth = ImGui::GetContentRegionAvail().x;
    const float totalHeight = ImGui::GetContentRegionAvail().y;
    const float splitter = m_SplitterSize;

    constexpr float minBrowser = 150.0f;
    constexpr float minInspector = 200.0f;
    constexpr float minEditor = 260.0f;
    constexpr float minPreview = 100.0f;
    constexpr float minTop = 200.0f;

    const float maxPreview = std::max(minPreview, totalHeight - splitter - minTop);
    m_PreviewHeight = std::clamp(m_PreviewHeight, minPreview, maxPreview);
    const float topHeight = std::max(minTop, totalHeight - m_PreviewHeight - splitter);

    const float maxInspector = std::max(minInspector, availWidth - splitter * 2.0f - minBrowser - minEditor);
    m_InspectorWidth = std::clamp(m_InspectorWidth, minInspector, maxInspector);
    const float maxBrowser = std::max(minBrowser, availWidth - splitter * 2.0f - m_InspectorWidth - minEditor);
    m_BrowserWidth = std::clamp(m_BrowserWidth, minBrowser, maxBrowser);

    // Library Browser (left)
    ImGui::BeginChild("##LibraryBrowser", ImVec2(m_BrowserWidth, topHeight), true);
    DrawLibraryBrowser();
    ImGui::EndChild();

    // Vertical splitter 1
    ImGui::SameLine();
    ImGui::ClaySplitterConfig leftSplitter;
    leftSplitter.Vertical = true;
    leftSplitter.Thickness = splitter;
    leftSplitter.MinPrimary = minBrowser;
    leftSplitter.MinSecondary = minEditor;
    leftSplitter.HoverCursor = ImGuiMouseCursor_ResizeEW;
    const float leftTotal = availWidth - m_InspectorWidth - splitter;
    ImGui::ClaySplitter("Dialogue_VSplitter_Left", &m_BrowserWidth, leftTotal, topHeight, leftSplitter);

    // Script Editor (center)
    ImGui::SameLine();
    float editorWidth = availWidth - m_BrowserWidth - m_InspectorWidth - splitter * 2.0f;
    if (editorWidth < minEditor) editorWidth = minEditor;
    ImGui::BeginChild("##ScriptEditor", ImVec2(editorWidth, topHeight), true);
    DrawScriptEditor();
    ImGui::EndChild();

    // Handle shortcuts after editor rendered so focus state is current
    HandleKeyboardShortcuts();

    // Vertical splitter 2
    ImGui::SameLine();
    ImGui::ClaySplitterConfig rightSplitter;
    rightSplitter.Vertical = true;
    rightSplitter.InvertAxis = true;
    rightSplitter.Thickness = splitter;
    rightSplitter.MinPrimary = minInspector;
    rightSplitter.MinSecondary = minEditor;
    rightSplitter.HoverCursor = ImGuiMouseCursor_ResizeEW;
    const float rightTotal = availWidth - m_BrowserWidth - splitter;
    ImGui::ClaySplitter("Dialogue_VSplitter_Right", &m_InspectorWidth, rightTotal, topHeight, rightSplitter);

    // Inspector (right)
    ImGui::SameLine();
    ImGui::BeginChild("##Inspector", ImVec2(m_InspectorWidth, topHeight), true);
    DrawInspector();
    ImGui::EndChild();

    // Horizontal splitter
    ImGui::ClaySplitterConfig horizontalSplitter;
    horizontalSplitter.Vertical = false;
    horizontalSplitter.InvertAxis = true;
    horizontalSplitter.Thickness = splitter;
    horizontalSplitter.MinPrimary = minPreview;
    horizontalSplitter.MinSecondary = minTop;
    horizontalSplitter.HoverCursor = ImGuiMouseCursor_ResizeNS;
    ImGui::ClaySplitter("Dialogue_HSplitter", &m_PreviewHeight, totalHeight, availWidth, horizontalSplitter);

    // Preview panel (bottom)
    ImGui::BeginChild("##Preview", ImVec2(availWidth, m_PreviewHeight), true);
    DrawPreview();
    ImGui::EndChild();

    // Condition builder popup
    if (m_ShowConditionBuilder) {
        DrawConditionBuilder();
    }

    ImGui::End();
}

//------------------------------------------------------------------------------
// Menu Bar
//------------------------------------------------------------------------------
void DialogueEditorPanel::DrawMenuBar()
{
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Library", "Ctrl+N")) NewDialogueLibrary();
            if (ImGui::MenuItem("Open...", "Ctrl+O")) OpenLibraryDialog();
            ImGui::Separator();
            if (ImGui::MenuItem("Save", "Ctrl+S", false, m_Library != nullptr)) SaveDialogueLibrary();
            if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S", false, m_Library != nullptr)) SaveLibraryDialog();
            ImGui::Separator();
            if (ImGui::MenuItem("Close")) m_Open = false;
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !m_UndoStack.empty())) Undo();
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !m_RedoStack.empty())) Redo();
            ImGui::Separator();
            if (ImGui::MenuItem("New Entry", "Ctrl+E", false, m_Library != nullptr)) CreateNewEntry();
            if (ImGui::MenuItem("Delete Entry", "Delete", false, m_SelectedEntry != nullptr)) DeleteSelectedEntry();
            if (ImGui::MenuItem("Duplicate Entry", "Ctrl+D", false, m_SelectedEntry != nullptr)) DuplicateSelectedEntry();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Show Validation Errors", nullptr, &m_ShowAutoComplete); // Placeholder
            ImGui::Separator();
            if (ImGui::MenuItem("Reset Layout")) {
                m_BrowserWidth = 220.0f;
                m_InspectorWidth = 280.0f;
                m_PreviewHeight = 180.0f;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Syntax Reference")) {
                // TODO: Show syntax reference popup
            }
            if (ImGui::MenuItem("Command List")) {
                // TODO: Show command list
            }
            ImGui::EndMenu();
        }

        ImGui::EndMenuBar();
    }
}

//------------------------------------------------------------------------------
// Toolbar
//------------------------------------------------------------------------------
void DialogueEditorPanel::DrawToolbar()
{
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
    
    if (ImGui::Button("New")) NewDialogueLibrary();
    ImGui::SameLine();
    if (ImGui::Button("Open")) OpenLibraryDialog();
    ImGui::SameLine();
    if (ImGui::Button("Save")) SaveDialogueLibrary();
    
    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();
    
    if (ImGui::Button("+ Entry")) CreateNewEntry();
    ImGui::SameLine();
    
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();
    
    // Preview controls
    if (m_PreviewActive) {
        if (ImGui::Button("Stop")) StopPreview();
    } else {
        if (ImGui::Button("Preview")) StartPreview();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) ResetPreview();
    
    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();
    
    // Validation status
    if (!m_ValidationErrors.empty()) {
        int errors = 0, warnings = 0;
        for (const auto& e : m_ValidationErrors) {
            if (e.severity == ValidationError::Error) errors++;
            else warnings++;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, errors > 0 ? 0xFF4444FF : 0xFF44AAFF);
        ImGui::Text("%d errors, %d warnings", errors, warnings);
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, 0xFF44FF44);
        ImGui::Text("No issues");
        ImGui::PopStyleColor();
    }
    
    ImGui::PopStyleVar();
    ImGui::Separator();
}

//------------------------------------------------------------------------------
// Library Browser (Left Panel)
//------------------------------------------------------------------------------
void DialogueEditorPanel::DrawLibraryBrowser()
{
    ImGui::Text("Entries");
    ImGui::Separator();
    
    if (!m_Library) {
        ImGui::TextDisabled("No library loaded");
        return;
    }
    
    // Search filter
    ImGui::SetNextItemWidth(-1);
    static char searchBuffer[256] = "";
    if (ImGui::InputTextWithHint("##Search", "Search entries...", searchBuffer, sizeof(searchBuffer))) {
        m_SearchFilter = searchBuffer;
    }
    
    // Filter toggles
    ImGui::Checkbox("Default Only", &m_ShowOnlyDefault);
    ImGui::SameLine();
    ImGui::Checkbox("Conditional", &m_ShowOnlyConditional);
    
    ImGui::Separator();
    
    // Entry list
    auto& entries = m_Library->GetEntries();
    for (int i = 0; i < (int)entries.size(); i++) {
        auto& entry = entries[i];
        
        // Apply filters
        if (!m_SearchFilter.empty()) {
            if (entry.displayName.find(m_SearchFilter) == std::string::npos &&
                entry.entryId.find(m_SearchFilter) == std::string::npos) {
                continue;
            }
        }
        if (m_ShowOnlyDefault && !entry.isDefault) continue;
        if (m_ShowOnlyConditional && entry.condition.requiredStateKey.empty() && 
            entry.condition.requiredQuestId.empty()) continue;
        
        // Entry item
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                   ImGuiTreeNodeFlags_SpanAvailWidth;
        if (i == m_SelectedEntryIndex) {
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        
        // Icon based on type
        const char* icon = entry.isDefault ? "[D]" : 
                          (!entry.condition.requiredStateKey.empty() ? "[C]" : "   ");
        
        std::string label = std::string(icon) + " " + 
            (entry.displayName.empty() ? entry.entryId : entry.displayName);
        
        ImGui::TreeNodeEx((void*)(intptr_t)i, flags, "%s", label.c_str());
        
        if (ImGui::IsItemClicked()) {
            // Save current entry before switching
            if (m_SelectedEntry) {
                m_SelectedEntry->rawText = m_EditorText;
                m_SelectedEntry->InvalidateCache();
            }
            
            m_SelectedEntryIndex = i;
            m_SelectedEntry = &entry;
            m_EditorText = entry.rawText;
            m_ValidationErrors.clear();
            RunValidation();
            
            m_UndoStack.clear();
            m_RedoStack.clear();
        }
        
        // Context menu
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Set as Default")) {
                // Clear other defaults
                for (auto& e : entries) e.isDefault = false;
                entry.isDefault = true;
                MarkModified();
            }
            if (ImGui::MenuItem("Duplicate")) {
                m_SelectedEntryIndex = i;
                m_SelectedEntry = &entry;
                DuplicateSelectedEntry();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete")) {
                m_SelectedEntryIndex = i;
                m_SelectedEntry = &entry;
                DeleteSelectedEntry();
            }
            ImGui::EndPopup();
        }
        
        // Tooltip
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("ID: %s", entry.entryId.c_str());
            if (!entry.condition.requiredStateKey.empty()) {
                ImGui::Text("Condition: %s=%s", 
                    entry.condition.requiredStateKey.c_str(),
                    entry.condition.requiredStateValue.c_str());
            }
            ImGui::EndTooltip();
        }
    }
    
    // Add new entry button at bottom
    ImGui::Separator();
    if (ImGui::Button("+ New Entry", ImVec2(-1, 0))) {
        CreateNewEntry();
    }
}

//------------------------------------------------------------------------------
// Script Editor (Center Panel)
//------------------------------------------------------------------------------
void DialogueEditorPanel::DrawScriptEditor()
{
    if (!m_SelectedEntry) {
        ImGui::TextDisabled("Select an entry to edit");
        return;
    }

    ImGui::Text("Script Editor - %s", m_SelectedEntry->displayName.c_str());
    ImGui::Separator();

    // Use InputTextMultiline with custom rendering for syntax highlighting
    // For now, basic text input with validation on change
    
    ImVec2 editorSize = ImGui::GetContentRegionAvail();
    editorSize.y -= 30; // Leave room for status bar
    
    // Line numbers column
    ImGui::BeginChild("##LineNumbers", ImVec2(40, editorSize.y), false);
    {
        int lineCount = 1;
        for (char c : m_EditorText) {
            if (c == '\n') lineCount++;
        }
        for (int i = 1; i <= lineCount; i++) {
            ImGui::PushStyleColor(ImGuiCol_Text, m_Colors.lineNumber);
            ImGui::Text("%4d", i);
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();
    
    ImGui::SameLine();
    
    // Text editor
    ImGui::BeginChild("##EditorContent", ImVec2(editorSize.x - 50, editorSize.y), true);
    {
        // Multiline input
        std::string prevText = m_EditorText;
        
        ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput;
        
        // Use a buffer for InputTextMultiline
        static char textBuffer[65536];
        strncpy(textBuffer, m_EditorText.c_str(), sizeof(textBuffer) - 1);
        textBuffer[sizeof(textBuffer) - 1] = '\0';
        
        if (ImGui::InputTextMultiline("##DialogueScript", textBuffer, sizeof(textBuffer),
                                       ImVec2(-1, -1), flags)) {
            if (m_EditorText != textBuffer) {
                PushUndo();
                m_EditorText = textBuffer;
                MarkModified();
                RunValidation();
            }
        }
        
        m_EditorFocused = ImGui::IsItemFocused();
    }
    ImGui::EndChild();
    
    // Status bar
    int lineCount = 1, charCount = 0;
    for (char c : m_EditorText) {
        if (c == '\n') lineCount++;
        charCount++;
    }
    ImGui::Text("Lines: %d | Characters: %d", lineCount, charCount);
    
    // Show validation errors inline
    if (!m_ValidationErrors.empty()) {
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 200);
        ImGui::PushStyleColor(ImGuiCol_Text, 0xFF4488FF);
        ImGui::Text("Click to jump to error");
        ImGui::PopStyleColor();
    }
}

//------------------------------------------------------------------------------
// Inspector (Right Panel)
//------------------------------------------------------------------------------
void DialogueEditorPanel::DrawInspector()
{
    ImGui::Text("Properties");
    ImGui::Separator();
    
    if (!m_Library) {
        ImGui::TextDisabled("No library loaded");
        return;
    }
    
    // Library properties
    if (ImGui::CollapsingHeader("Library", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();
        
        static char nameBuffer[256];
        strncpy(nameBuffer, m_Library->GetDisplayName().c_str(), sizeof(nameBuffer) - 1);
        if (ImGui::InputText("Display Name", nameBuffer, sizeof(nameBuffer))) {
            m_Library->SetDisplayName(nameBuffer);
            MarkModified();
        }
        
        static char charIdBuffer[256];
        strncpy(charIdBuffer, m_Library->GetCharacterId().c_str(), sizeof(charIdBuffer) - 1);
        if (ImGui::InputText("Character ID", charIdBuffer, sizeof(charIdBuffer))) {
            m_Library->SetCharacterId(charIdBuffer);
            MarkModified();
        }
        
        ImGui::Text("GUID: %s", m_Library->GetGuid().ToString().c_str());
        ImGui::Text("Entries: %zu", m_Library->GetEntries().size());
        
        ImGui::Unindent();
    }
    
    // Entry properties
    if (m_SelectedEntry && ImGui::CollapsingHeader("Entry", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();
        
        static char entryIdBuffer[256];
        strncpy(entryIdBuffer, m_SelectedEntry->entryId.c_str(), sizeof(entryIdBuffer) - 1);
        if (ImGui::InputText("Entry ID", entryIdBuffer, sizeof(entryIdBuffer))) {
            m_SelectedEntry->entryId = entryIdBuffer;
            MarkModified();
        }
        
        static char displayNameBuffer[256];
        strncpy(displayNameBuffer, m_SelectedEntry->displayName.c_str(), sizeof(displayNameBuffer) - 1);
        if (ImGui::InputText("Display Name", displayNameBuffer, sizeof(displayNameBuffer))) {
            m_SelectedEntry->displayName = displayNameBuffer;
            MarkModified();
        }
        
        if (ImGui::Checkbox("Is Default", &m_SelectedEntry->isDefault)) {
            if (m_SelectedEntry->isDefault) {
                // Clear other defaults
                for (auto& e : m_Library->GetEntries()) {
                    if (&e != m_SelectedEntry) e.isDefault = false;
                }
            }
            MarkModified();
        }
        
        ImGui::Unindent();
    }
    
    // Condition
    if (m_SelectedEntry && ImGui::CollapsingHeader("Condition", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();
        
        auto& cond = m_SelectedEntry->condition;
        
        static char stateKeyBuffer[256];
        strncpy(stateKeyBuffer, cond.requiredStateKey.c_str(), sizeof(stateKeyBuffer) - 1);
        if (ImGui::InputText("State Key", stateKeyBuffer, sizeof(stateKeyBuffer))) {
            cond.requiredStateKey = stateKeyBuffer;
            MarkModified();
        }
        
        static char stateValueBuffer[256];
        strncpy(stateValueBuffer, cond.requiredStateValue.c_str(), sizeof(stateValueBuffer) - 1);
        if (ImGui::InputText("State Value", stateValueBuffer, sizeof(stateValueBuffer))) {
            cond.requiredStateValue = stateValueBuffer;
            MarkModified();
        }
        
        ImGui::Separator();
        ImGui::Text("Quest Condition:");
        
        static char questIdBuffer[256];
        strncpy(questIdBuffer, cond.requiredQuestId.c_str(), sizeof(questIdBuffer) - 1);
        if (ImGui::InputText("Quest ID", questIdBuffer, sizeof(questIdBuffer))) {
            cond.requiredQuestId = questIdBuffer;
            MarkModified();
        }
        
        static char stepIdBuffer[256];
        strncpy(stepIdBuffer, cond.requiredStepId.c_str(), sizeof(stepIdBuffer) - 1);
        if (ImGui::InputText("Step ID", stepIdBuffer, sizeof(stepIdBuffer))) {
            cond.requiredStepId = stepIdBuffer;
            MarkModified();
        }
        
        static char stepStatusBuffer[256];
        strncpy(stepStatusBuffer, cond.requiredStepStatus.c_str(), sizeof(stepStatusBuffer) - 1);
        if (ImGui::InputText("Step Status", stepStatusBuffer, sizeof(stepStatusBuffer))) {
            cond.requiredStepStatus = stepStatusBuffer;
            MarkModified();
        }
        
        if (ImGui::Button("Condition Builder...")) {
            m_ShowConditionBuilder = true;
            m_ConditionBuilderTarget = "entry";
        }
        
        ImGui::Unindent();
    }
    
    // Validation errors
    if (!m_ValidationErrors.empty() && ImGui::CollapsingHeader("Validation", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();
        for (const auto& error : m_ValidationErrors) {
            uint32_t color = error.severity == ValidationError::Error ? 0xFF4444FF : 0xFF44AAFF;
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::Text("Line %d: %s", error.line, error.message.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Unindent();
    }
    
    // Known speakers
    if (ImGui::CollapsingHeader("Speakers")) {
        ImGui::Indent();
        UpdateKnownSpeakers();
        for (const auto& speaker : m_KnownSpeakers) {
            ImGui::BulletText("%s", speaker.c_str());
        }
        ImGui::Unindent();
    }
}

//------------------------------------------------------------------------------
// Preview Panel (Bottom)
//------------------------------------------------------------------------------
void DialogueEditorPanel::DrawPreview()
{
    ImGui::Text("Preview");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 200);
    
    if (m_PreviewActive) {
        if (ImGui::Button("Advance")) AdvancePreview();
        ImGui::SameLine();
        if (ImGui::Button("Stop")) StopPreview();
    } else {
        if (ImGui::Button("Start Preview")) StartPreview();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset State")) ResetPreview();
    
    ImGui::Separator();
    
    if (!m_PreviewActive) {
        ImGui::TextDisabled("Click 'Start Preview' to test the dialogue");
        return;
    }
    
    // Preview display
    float previewWidth = ImGui::GetContentRegionAvail().x;
    
    // Speaker
    ImGui::PushStyleColor(ImGuiCol_Text, m_Colors.speaker);
    ImGui::Text("[%s]", m_PreviewSpeaker.empty() ? "???" : m_PreviewSpeaker.c_str());
    ImGui::PopStyleColor();
    
    // Text with word wrap
    ImGui::TextWrapped("%s", m_PreviewDisplayedText.c_str());
    
    // Choices
    if (!m_PreviewChoices.empty()) {
        ImGui::Separator();
        ImGui::Text("Choices:");
        for (int i = 0; i < (int)m_PreviewChoices.size(); i++) {
            std::string label = std::to_string(i + 1) + ". " + m_PreviewChoices[i];
            if (ImGui::Button(label.c_str())) {
                SelectPreviewChoice(i);
            }
        }
    }
    
    // State viewer
    if (!m_PreviewState.empty()) {
        ImGui::Separator();
        if (ImGui::TreeNode("State Variables")) {
            for (const auto& [key, value] : m_PreviewState) {
                ImGui::Text("%s = %s", key.c_str(), value.c_str());
            }
            ImGui::TreePop();
        }
    }
}

//------------------------------------------------------------------------------
// Entry Management
//------------------------------------------------------------------------------
void DialogueEditorPanel::CreateNewEntry()
{
    if (!m_Library) return;
    
    Dialogue::DialogueEntry entry;
    entry.entryId = "entry_" + std::to_string(m_Library->GetEntries().size() + 1);
    entry.displayName = "New Entry";
    entry.rawText = "{New Conversation}\nSpeaker: Hello!\n";
    entry.isDefault = m_Library->GetEntries().empty();
    
    m_Library->AddEntry(entry);
    
    // Select the new entry
    m_SelectedEntryIndex = (int)m_Library->GetEntries().size() - 1;
    m_SelectedEntry = &m_Library->GetEntries()[m_SelectedEntryIndex];
    m_EditorText = m_SelectedEntry->rawText;
    
    MarkModified();
}

void DialogueEditorPanel::DeleteSelectedEntry()
{
    if (!m_Library || !m_SelectedEntry) return;
    
    m_Library->RemoveEntry(m_SelectedEntry->entryId);
    m_SelectedEntryIndex = -1;
    m_SelectedEntry = nullptr;
    m_EditorText.clear();
    
    MarkModified();
}

void DialogueEditorPanel::DuplicateSelectedEntry()
{
    if (!m_Library || !m_SelectedEntry) return;
    
    Dialogue::DialogueEntry newEntry = *m_SelectedEntry;
    newEntry.entryId = m_SelectedEntry->entryId + "_copy";
    newEntry.displayName = m_SelectedEntry->displayName + " (Copy)";
    newEntry.isDefault = false;
    
    m_Library->AddEntry(newEntry);
    
    m_SelectedEntryIndex = (int)m_Library->GetEntries().size() - 1;
    m_SelectedEntry = &m_Library->GetEntries()[m_SelectedEntryIndex];
    m_EditorText = m_SelectedEntry->rawText;
    
    MarkModified();
}

void DialogueEditorPanel::MoveEntryUp()
{
    if (!m_Library || m_SelectedEntryIndex <= 0) return;
    
    auto& entries = m_Library->GetEntries();
    std::swap(entries[m_SelectedEntryIndex], entries[m_SelectedEntryIndex - 1]);
    m_SelectedEntryIndex--;
    m_SelectedEntry = &entries[m_SelectedEntryIndex];
    
    MarkModified();
}

void DialogueEditorPanel::MoveEntryDown()
{
    if (!m_Library || m_SelectedEntryIndex < 0) return;
    
    auto& entries = m_Library->GetEntries();
    if (m_SelectedEntryIndex >= (int)entries.size() - 1) return;
    
    std::swap(entries[m_SelectedEntryIndex], entries[m_SelectedEntryIndex + 1]);
    m_SelectedEntryIndex++;
    m_SelectedEntry = &entries[m_SelectedEntryIndex];
    
    MarkModified();
}

//------------------------------------------------------------------------------
// Preview System
//------------------------------------------------------------------------------
void DialogueEditorPanel::StartPreview()
{
    if (!m_SelectedEntry) return;
    
    // Update entry text
    m_SelectedEntry->rawText = m_EditorText;
    m_SelectedEntry->InvalidateCache();
    
    m_PreviewConversation = m_SelectedEntry->GetConversation();
    if (!m_PreviewConversation) {
        std::cerr << "[DialogueEditor] Failed to parse conversation for preview" << std::endl;
        return;
    }
    
    m_PreviewCommandIndex = 0;
    m_PreviewActive = true;
    m_PreviewDisplayedText.clear();
    m_PreviewSpeaker.clear();
    m_PreviewChoices.clear();
    
    AdvancePreview();
}

void DialogueEditorPanel::StopPreview()
{
    m_PreviewActive = false;
    m_PreviewConversation.reset();
}

void DialogueEditorPanel::AdvancePreview()
{
    if (!m_PreviewActive || !m_PreviewConversation) return;
    
    m_PreviewChoices.clear();
    
    while (m_PreviewCommandIndex < m_PreviewConversation->commands.size()) {
        auto& cmd = m_PreviewConversation->commands[m_PreviewCommandIndex];
        
        switch (cmd->type) {
            case Dialogue::CommandType::Line: {
                auto& line = static_cast<Dialogue::LineCommand&>(*cmd);
                m_PreviewSpeaker = line.speaker;
                m_PreviewDisplayedText = line.text;
                
                for (const auto& choice : line.choices) {
                    m_PreviewChoices.push_back(choice.text);
                }
                
                m_PreviewCommandIndex++;
                return; // Wait for user
            }
            
            case Dialogue::CommandType::SetState: {
                auto& setState = static_cast<Dialogue::SetStateCommand&>(*cmd);
                m_PreviewState[setState.stateName] = setState.stateValue;
                break;
            }
            
            case Dialogue::CommandType::End:
                StopPreview();
                m_PreviewDisplayedText = "[END]";
                return;
                
            default:
                break;
        }
        
        m_PreviewCommandIndex++;
    }
    
    // Reached end
    StopPreview();
    m_PreviewDisplayedText = "[END]";
}

void DialogueEditorPanel::SelectPreviewChoice(int index)
{
    if (!m_PreviewActive || !m_PreviewConversation) return;
    if (m_PreviewCommandIndex == 0) return;
    
    // Get the line command that had the choices (it's the previous command we displayed)
    size_t lineIndex = m_PreviewCommandIndex - 1;
    if (lineIndex >= m_PreviewConversation->commands.size()) return;
    
    auto& cmd = m_PreviewConversation->commands[lineIndex];
    if (cmd->type != Dialogue::CommandType::Line) {
        AdvancePreview();
        return;
    }
    
    auto& line = static_cast<Dialogue::LineCommand&>(*cmd);
    if (index < 0 || index >= (int)line.choices.size()) {
        AdvancePreview();
        return;
    }
    
    const auto& choice = line.choices[index];
    
    // Insert choice response commands into the conversation
    if (!choice.response.empty()) {
        // Insert response commands at current position
        m_PreviewConversation->commands.insert(
            m_PreviewConversation->commands.begin() + m_PreviewCommandIndex,
            choice.response.begin(),
            choice.response.end());
    }
    
    AdvancePreview();
}

void DialogueEditorPanel::ResetPreview()
{
    m_PreviewState.clear();
    m_PreviewCommandIndex = 0;
    m_PreviewDisplayedText.clear();
    m_PreviewSpeaker.clear();
    m_PreviewChoices.clear();
    m_PreviewActive = false;
}

//------------------------------------------------------------------------------
// Validation
//------------------------------------------------------------------------------
void DialogueEditorPanel::RunValidation()
{
    m_ValidationErrors.clear();
    
    std::istringstream stream(m_EditorText);
    std::string line;
    int lineNum = 0;
    bool hasTitle = false;
    
    while (std::getline(stream, line)) {
        lineNum++;
        
        // Trim
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        std::string trimmed = line.substr(start);
        
        // Skip comments
        if (trimmed[0] == '#' || trimmed.substr(0, 2) == "//") continue;
        
        // Check for title
        if (trimmed[0] == '{' && trimmed.back() == '}') {
            hasTitle = true;
            continue;
        }
        
        // Check for unclosed brackets
        int braceCount = 0, parenCount = 0, angleCount = 0;
        for (char c : trimmed) {
            if (c == '{') braceCount++;
            else if (c == '}') braceCount--;
            else if (c == '(') parenCount++;
            else if (c == ')') parenCount--;
            else if (c == '<') angleCount++;
            else if (c == '>') angleCount--;
        }
        
        if (braceCount != 0) {
            m_ValidationErrors.push_back({lineNum, "Unclosed braces", ValidationError::Error});
        }
        if (parenCount != 0) {
            m_ValidationErrors.push_back({lineNum, "Unclosed parentheses", ValidationError::Warning});
        }
        
        // Check for unknown commands
        if (trimmed[0] == '\\') {
            static std::regex cmdRegex(R"(\\(SetState|SetEmotion|goto|end|GiveItem|TakeItem|StartQuest|CompleteStep|PlayAnim|PlaySound|Wait|Camera)\()", std::regex::icase);
            if (!std::regex_search(trimmed, cmdRegex) && trimmed != "\\end" && trimmed != "\\End") {
                m_ValidationErrors.push_back({lineNum, "Unknown command", ValidationError::Warning});
            }
        }
    }
    
    if (!hasTitle && !m_EditorText.empty()) {
        m_ValidationErrors.push_back({1, "Missing conversation title {Title}", ValidationError::Warning});
    }
}

//------------------------------------------------------------------------------
// Condition Builder
//------------------------------------------------------------------------------
void DialogueEditorPanel::DrawConditionBuilder()
{
    ImGui::OpenPopup("Condition Builder");
    
    if (ImGui::BeginPopupModal("Condition Builder", &m_ShowConditionBuilder, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Build a condition for: %s", m_ConditionBuilderTarget.c_str());
        ImGui::Separator();
        
        // State condition
        ImGui::Text("State Condition:");
        static char keyBuf[256] = "";
        static char valBuf[256] = "";
        ImGui::InputText("Key", keyBuf, sizeof(keyBuf));
        
        const char* comparisons[] = { "==", "!=", ">=", "<=", ">", "<" };
        static int compIdx = 0;
        ImGui::Combo("Comparison", &compIdx, comparisons, IM_ARRAYSIZE(comparisons));
        
        ImGui::InputText("Value", valBuf, sizeof(valBuf));
        
        ImGui::Checkbox("Negated (!)", &m_ConditionBuilder.isNegated);
        
        ImGui::Separator();
        
        // Preview
        std::string preview;
        if (m_ConditionBuilder.isNegated) preview = "!";
        preview += std::string(keyBuf) + comparisons[compIdx] + valBuf;
        ImGui::Text("Preview: %s", preview.c_str());
        
        ImGui::Separator();
        
        if (ImGui::Button("Apply", ImVec2(120, 0))) {
            if (m_ConditionBuilderTarget == "entry" && m_SelectedEntry) {
                m_SelectedEntry->condition.requiredStateKey = keyBuf;
                m_SelectedEntry->condition.requiredStateValue = valBuf;
                MarkModified();
            }
            m_ShowConditionBuilder = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            m_ShowConditionBuilder = false;
        }
        
        ImGui::EndPopup();
    }
}

//------------------------------------------------------------------------------
// Helpers
//------------------------------------------------------------------------------
void DialogueEditorPanel::MarkModified()
{
    m_Modified = true;
}

bool DialogueEditorPanel::ConfirmDiscard()
{
    // Simple confirmation - in production, use a proper modal dialog
    return true; // For now, always allow
}

std::string DialogueEditorPanel::GetWindowTitle() const
{
    std::string title = "Dialogue Editor";
    if (m_Library) {
        title += " - " + m_Library->GetDisplayName();
    }
    if (m_Modified) {
        title += " *";
    }
    return title;
}

void DialogueEditorPanel::UpdateKnownSpeakers()
{
    m_KnownSpeakers.clear();
    if (!m_Library) return;
    
    std::unordered_set<std::string> speakers;
    for (const auto& entry : m_Library->GetEntries()) {
        ParseSpeakersFromText(entry.rawText);
    }
    
    // Also parse current editor text
    ParseSpeakersFromText(m_EditorText);
}

void DialogueEditorPanel::ParseSpeakersFromText(const std::string& text)
{
    std::regex speakerRegex(R"((?:^|\r?\n)([A-Za-z0-9_]+):)");
    std::sregex_iterator it(text.begin(), text.end(), speakerRegex);
    std::sregex_iterator end;
    
    while (it != end) {
        std::string speaker = (*it)[1].str();
        if (std::find(m_KnownSpeakers.begin(), m_KnownSpeakers.end(), speaker) == m_KnownSpeakers.end()) {
            m_KnownSpeakers.push_back(speaker);
        }
        ++it;
    }
}

void DialogueEditorPanel::HandleKeyboardShortcuts()
{
    if (!m_EditorFocused) return;
    
    ImGuiIO& io = ImGui::GetIO();
    
    if (io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_O)) OpenLibraryDialog();
        else if (ImGui::IsKeyPressed(ImGuiKey_N)) NewDialogueLibrary();
        else if (ImGui::IsKeyPressed(ImGuiKey_Z)) Undo();
        else if (ImGui::IsKeyPressed(ImGuiKey_Y)) Redo();
        else if (ImGui::IsKeyPressed(ImGuiKey_E)) CreateNewEntry();
        else if (ImGui::IsKeyPressed(ImGuiKey_D)) DuplicateSelectedEntry();
    }
    
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) && m_SelectedEntry) {
        // Don't delete if typing
        if (!m_EditorFocused) DeleteSelectedEntry();
    }
}

void DialogueEditorPanel::PushUndo()
{
    m_UndoStack.push_back(m_EditorText);
    if (m_UndoStack.size() > MAX_UNDO_HISTORY) {
        m_UndoStack.erase(m_UndoStack.begin());
    }
    m_RedoStack.clear();
}

void DialogueEditorPanel::Undo()
{
    if (m_UndoStack.empty()) return;
    
    m_RedoStack.push_back(m_EditorText);
    m_EditorText = m_UndoStack.back();
    m_UndoStack.pop_back();
    
    if (m_SelectedEntry) {
        m_SelectedEntry->rawText = m_EditorText;
        m_SelectedEntry->InvalidateCache();
    }
    
    RunValidation();
}

void DialogueEditorPanel::Redo()
{
    if (m_RedoStack.empty()) return;
    
    m_UndoStack.push_back(m_EditorText);
    m_EditorText = m_RedoStack.back();
    m_RedoStack.pop_back();
    
    if (m_SelectedEntry) {
        m_SelectedEntry->rawText = m_EditorText;
        m_SelectedEntry->InvalidateCache();
    }
    
    RunValidation();
}

//------------------------------------------------------------------------------
// Auto-complete data
//------------------------------------------------------------------------------
std::vector<DialogueEditorPanel::AutoCompleteItem> DialogueEditorPanel::GetCommandCompletions()
{
    return {
        {"\\SetState", "Set a game state variable", "\\SetState(key, value)"},
        {"\\SetEmotion", "Set character emotion", "\\SetEmotion(emotionName)"},
        {"\\goto", "Jump to label", "\\goto(labelName)"},
        {"\\end", "End conversation", "\\end"},
        {"\\GiveItem", "Give item to player", "\\GiveItem(itemId, count)"},
        {"\\TakeItem", "Take item from player", "\\TakeItem(itemId, count)"},
        {"\\StartQuest", "Start a quest", "\\StartQuest(questId)"},
        {"\\CompleteStep", "Complete quest step", "\\CompleteStep(questId, stepId)"},
        {"\\PlayAnim", "Play animation", "\\PlayAnim(animationName)"},
        {"\\PlaySound", "Play sound effect", "\\PlaySound(soundPath)"},
        {"\\Wait", "Wait for duration", "\\Wait(seconds)"},
        {"\\Camera", "Camera action", "\\Camera(action, target)"},
    };
}

std::vector<DialogueEditorPanel::AutoCompleteItem> DialogueEditorPanel::GetEmoteCompletions()
{
    return {
        {"wave", "Waving gesture", "(wave)"},
        {"nod", "Nodding head", "(nod)"},
        {"shake", "Shaking head", "(shake)"},
        {"bow", "Bowing", "(bow)"},
        {"laugh", "Laughing", "(laugh)"},
        {"cry", "Crying", "(cry)"},
        {"angry", "Angry expression", "(angry)"},
        {"zoom", "Camera zoom in", "(zoom)"},
        {"pan", "Camera pan", "(pan)"},
    };
}

//------------------------------------------------------------------------------
// File Dialogs
//------------------------------------------------------------------------------
void DialogueEditorPanel::NewLibraryDialog()
{
    NewDialogueLibrary();
}

void DialogueEditorPanel::OpenLibraryDialog()
{
#ifdef _WIN32
    OPENFILENAMEA ofn = {};
    char filename[MAX_PATH] = "";
    
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = "Dialogue Library (*.dlglib)\0*.dlglib\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = "dlglib";
    
    if (GetOpenFileNameA(&ofn)) {
        LoadDialogueLibrary(filename);
    }
#endif
}

bool DialogueEditorPanel::SaveLibraryDialog()
{
#ifdef _WIN32
    OPENFILENAMEA ofn = {};
    char filename[MAX_PATH] = "";
    
    if (!m_OpenPath.empty()) {
        strncpy(filename, m_OpenPath.c_str(), MAX_PATH - 1);
    }
    
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = "Dialogue Library (*.dlglib)\0*.dlglib\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = "dlglib";
    
    if (GetSaveFileNameA(&ofn)) {
        return SaveDialogueLibrary(filename);
    }
#endif
    return false;
}

