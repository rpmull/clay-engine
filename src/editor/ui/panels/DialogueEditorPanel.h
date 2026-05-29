#pragma once

#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>

#include "core/dialogue/Dialogue.h"
#include "core/assets/AssetReference.h"

//------------------------------------------------------------------------------
// DialogueEditorPanel
// Professional dialogue authoring tool with:
// - Library browser
// - Conversation entry list
// - Script editor with syntax highlighting
// - Condition builder
// - Live preview
// - Speaker management
//------------------------------------------------------------------------------
class DialogueEditorPanel
{
public:
    DialogueEditorPanel();
    ~DialogueEditorPanel();

    // Load/save dialogue library
    bool LoadDialogueLibrary(const std::string& path);
    bool SaveDialogueLibrary(const std::string& path = "");
    bool NewDialogueLibrary();
    
    // Create new library with file dialog
    void NewLibraryDialog();
    void OpenLibraryDialog();
    bool SaveLibraryDialog();
    bool SaveCurrent() { return SaveDialogueLibrary(); }
    bool SaveCurrentAsDialog() { return SaveLibraryDialog(); }

    // Render the panel window
    void OnImGuiRender();

    // Open/close state
    void Open() { m_Open = true; }
    void SetOpen(bool open) { m_Open = open; }
    bool IsOpen() const { return m_Open; }
    bool IsWindowFocusedOrHovered() const { return m_WindowFocusedOrHovered; }

    // Set current library directly
    void SetLibrary(std::shared_ptr<Dialogue::DialogueLibrary> library);

    // Get modification state
    bool IsModified() const { return m_Modified; }

private:
    //--------------------------------------------------------------------------
    // Layout Components
    //--------------------------------------------------------------------------
    void DrawMenuBar();
    void DrawToolbar();
    void DrawLibraryBrowser();      // Left panel - entry list
    void DrawScriptEditor();         // Center - text editor with highlighting
    void DrawInspector();            // Right panel - entry properties
    void DrawPreview();              // Bottom - live dialogue preview
    void DrawConditionBuilder();     // Popup for condition editing
    void DrawVariableWatcher();      // Debug state viewer

    //--------------------------------------------------------------------------
    // Entry Management
    //--------------------------------------------------------------------------
    void CreateNewEntry();
    void DeleteSelectedEntry();
    void DuplicateSelectedEntry();
    void MoveEntryUp();
    void MoveEntryDown();

    //--------------------------------------------------------------------------
    // Script Editor
    //--------------------------------------------------------------------------
    void RenderSyntaxHighlightedText();
    void HandleEditorInput();
    void ApplyAutoComplete();
    void UpdateLineNumbers();
    void ValidateSyntax();
    
    // Syntax highlighting colors
    struct SyntaxColors {
        uint32_t title = 0xFF88CCFF;       // {Title} - light blue
        uint32_t speaker = 0xFF88FF88;     // Speaker: - green
        uint32_t emote = 0xFFFFAA44;       // (emote) - orange
        uint32_t condition = 0xFFAA88FF;   // <condition> - purple
        uint32_t choice = 0xFFFFFF88;      // - choice - yellow
        uint32_t command = 0xFFFF8888;     // \Command - red
        uint32_t comment = 0xFF888888;     // # comment - gray
        uint32_t label = 0xFF88FFFF;       // @label - cyan
        uint32_t variable = 0xFFFFCC88;    // {variable} - peach
        uint32_t text = 0xFFFFFFFF;        // Regular text - white
        uint32_t lineNumber = 0xFF666666;  // Line numbers
        uint32_t currentLine = 0x30FFFFFF; // Current line highlight
        uint32_t error = 0xFF4444FF;       // Syntax error
    };
    SyntaxColors m_Colors;

    //--------------------------------------------------------------------------
    // Preview System
    //--------------------------------------------------------------------------
    void StartPreview();
    void StopPreview();
    void AdvancePreview();
    void SelectPreviewChoice(int index);
    void ResetPreview();
    
    bool m_PreviewActive = false;
    std::shared_ptr<Dialogue::Conversation> m_PreviewConversation;
    size_t m_PreviewCommandIndex = 0;
    std::string m_PreviewDisplayedText;
    std::string m_PreviewSpeaker;
    std::vector<std::string> m_PreviewChoices;
    std::unordered_map<std::string, std::string> m_PreviewState;

    //--------------------------------------------------------------------------
    // Auto-completion
    //--------------------------------------------------------------------------
    struct AutoCompleteItem {
        std::string text;
        std::string description;
        std::string insertText;
    };
    std::vector<AutoCompleteItem> m_AutoCompleteItems;
    int m_AutoCompleteSelectedIndex = -1;
    bool m_ShowAutoComplete = false;
    std::string m_AutoCompleteTrigger;
    
    void BuildAutoCompleteList(const std::string& prefix);
    static std::vector<AutoCompleteItem> GetCommandCompletions();
    static std::vector<AutoCompleteItem> GetEmoteCompletions();

    //--------------------------------------------------------------------------
    // Validation
    //--------------------------------------------------------------------------
    struct ValidationError {
        int line;
        std::string message;
        enum Severity { Warning, Error } severity;
    };
    std::vector<ValidationError> m_ValidationErrors;
    void RunValidation();

    //--------------------------------------------------------------------------
    // State
    //--------------------------------------------------------------------------
    bool m_Open = false;
    bool m_Modified = false;
    bool m_WindowFocusedOrHovered = false;
    std::string m_OpenPath;
    std::string m_LibraryName;

    // Current library
    std::shared_ptr<Dialogue::DialogueLibrary> m_Library;
    int m_SelectedEntryIndex = -1;
    Dialogue::DialogueEntry* m_SelectedEntry = nullptr;

    // Editor state
    std::string m_EditorText;
    int m_CursorLine = 0;
    int m_CursorColumn = 0;
    bool m_EditorFocused = false;
    float m_ScrollY = 0;

    // Layout
    float m_BrowserWidth = 220.0f;
    float m_InspectorWidth = 280.0f;
    float m_PreviewHeight = 180.0f;
    float m_SplitterSize = 4.0f;

    // Search/filter
    std::string m_SearchFilter;
    bool m_ShowOnlyDefault = false;
    bool m_ShowOnlyConditional = false;

    // Character/speaker management
    std::vector<std::string> m_KnownSpeakers;
    void UpdateKnownSpeakers();

    // Undo/redo (simple text-based)
    std::vector<std::string> m_UndoStack;
    std::vector<std::string> m_RedoStack;
    void PushUndo();
    void Undo();
    void Redo();
    static const size_t MAX_UNDO_HISTORY = 50;

    // Keyboard shortcuts
    void HandleKeyboardShortcuts();

    //--------------------------------------------------------------------------
    // Condition Builder
    //--------------------------------------------------------------------------
    bool m_ShowConditionBuilder = false;
    std::string m_ConditionBuilderTarget; // "entry" or "choice:N"
    
    struct ConditionBuilderState {
        std::string stateKey;
        std::string stateValue;
        std::string comparison = "==";
        bool isNegated = false;
        std::string questId;
        std::string stepId;
        std::string stepStatus;
    };
    ConditionBuilderState m_ConditionBuilder;

    //--------------------------------------------------------------------------
    // Helper functions
    //--------------------------------------------------------------------------
    void MarkModified();
    bool ConfirmDiscard();
    std::string GetWindowTitle() const;
    void ParseSpeakersFromText(const std::string& text);
};

