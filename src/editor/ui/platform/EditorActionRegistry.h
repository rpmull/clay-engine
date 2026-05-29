#pragma once

#include <imgui.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace editorui {

enum class EditorActionType {
    Command,
    Window,
    Tool
};

struct EditorShortcut {
    ImGuiKey Key = ImGuiKey_None;
    bool Ctrl = false;
    bool Shift = false;
    bool Alt = false;
    bool Super = false;
    bool AllowWhenTextInput = false;
};

struct EditorActionDefinition {
    std::string Id;
    std::string Label;
    std::string Category;
    std::string ShortcutLabel;
    std::string SearchText;
    EditorActionType Type = EditorActionType::Command;
    int SortOrder = 0;
    bool ShowInCommandPalette = true;
    bool PersistWindowState = false;
    EditorShortcut Shortcut;
    std::function<void()> Execute;
    std::function<bool()> IsEnabled;
    std::function<bool()> IsChecked;
    std::function<void(bool)> SetChecked;
};

struct EditorActionSearchResult {
    const EditorActionDefinition* Action = nullptr;
    float Score = 0.0f;
};

class EditorActionRegistry {
public:
    void Clear();
    void Register(EditorActionDefinition definition);

    bool Execute(const std::string& id);
    bool TryDispatchShortcut();

    const EditorActionDefinition* Find(const std::string& id) const;
    bool SetChecked(const std::string& id, bool checked);
    std::vector<const EditorActionDefinition*> GetActionsForCategory(const std::string& category) const;

    std::vector<EditorActionSearchResult> Search(const std::string& query, size_t maxResults = 64) const;
    std::vector<const EditorActionDefinition*> GetPersistedActions() const;

    const std::unordered_map<std::string, int>& GetUsageCounts() const { return m_UsageCounts; }
    void SetUsageCounts(std::unordered_map<std::string, int> usageCounts);

    uint64_t GetVersion() const { return m_Version; }

private:
    struct StoredAction {
        EditorActionDefinition Definition;
        std::string SearchIndex;
        std::string LabelLower;
        std::string CategoryLower;
    };

    const StoredAction* FindStored(const std::string& id) const;
    bool ShortcutMatches(const EditorShortcut& shortcut) const;
    float ScoreAction(const StoredAction& action, const std::vector<std::string>& tokens, const std::string& normalizedQuery) const;
    void TouchUsage(const std::string& id);

private:
    std::vector<StoredAction> m_Actions;
    std::unordered_map<std::string, size_t> m_IndexById;
    std::unordered_map<std::string, int> m_UsageCounts;
    uint64_t m_Version = 1;
};

} // namespace editorui
