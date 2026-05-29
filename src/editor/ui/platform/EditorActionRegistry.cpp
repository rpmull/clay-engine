#include "EditorActionRegistry.h"

#include <algorithm>
#include <cctype>

#include <imgui_internal.h>

namespace editorui {
namespace {

std::string ToLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::vector<std::string> TokenizeQuery(const std::string& query)
{
    std::vector<std::string> tokens;
    std::string current;
    current.reserve(query.size());

    for (char ch : query) {
        if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }

    if (!current.empty()) {
        tokens.push_back(current);
    }

    return tokens;
}

std::string BuildSearchIndex(const EditorActionDefinition& definition)
{
    std::string text = definition.Id;
    text.push_back(' ');
    text += definition.Label;
    text.push_back(' ');
    text += definition.Category;
    text.push_back(' ');
    text += definition.ShortcutLabel;
    text.push_back(' ');
    text += definition.SearchText;
    return ToLowerAscii(std::move(text));
}

} // namespace

void EditorActionRegistry::Clear()
{
    m_Actions.clear();
    m_IndexById.clear();
    ++m_Version;
}

void EditorActionRegistry::Register(EditorActionDefinition definition)
{
    if (definition.Id.empty() || definition.Label.empty() || !definition.Execute) {
        return;
    }

    StoredAction action;
    action.LabelLower = ToLowerAscii(definition.Label);
    action.CategoryLower = ToLowerAscii(definition.Category);
    action.SearchIndex = BuildSearchIndex(definition);
    action.Definition = std::move(definition);

    const size_t existingIndex = m_IndexById.count(action.Definition.Id) != 0
        ? m_IndexById[action.Definition.Id]
        : static_cast<size_t>(-1);

    if (existingIndex != static_cast<size_t>(-1)) {
        m_Actions[existingIndex] = std::move(action);
    } else {
        m_IndexById[action.Definition.Id] = m_Actions.size();
        m_Actions.push_back(std::move(action));
    }

    ++m_Version;
}

bool EditorActionRegistry::Execute(const std::string& id)
{
    const StoredAction* stored = FindStored(id);
    if (!stored) {
        return false;
    }

    if (stored->Definition.IsEnabled && !stored->Definition.IsEnabled()) {
        return false;
    }

    stored->Definition.Execute();
    TouchUsage(id);
    return true;
}

bool EditorActionRegistry::TryDispatchShortcut()
{
    if (GImGui != nullptr && GImGui->OpenPopupStack.Size > 0) {
        return false;
    }

    for (const StoredAction& stored : m_Actions) {
        if (!ShortcutMatches(stored.Definition.Shortcut)) {
            continue;
        }
        if (stored.Definition.IsEnabled && !stored.Definition.IsEnabled()) {
            continue;
        }

        stored.Definition.Execute();
        TouchUsage(stored.Definition.Id);
        return true;
    }

    return false;
}

const EditorActionDefinition* EditorActionRegistry::Find(const std::string& id) const
{
    const StoredAction* stored = FindStored(id);
    return stored ? &stored->Definition : nullptr;
}

bool EditorActionRegistry::SetChecked(const std::string& id, bool checked)
{
    const StoredAction* stored = FindStored(id);
    if (!stored || !stored->Definition.SetChecked) {
        return false;
    }

    stored->Definition.SetChecked(checked);
    ++m_Version;
    return true;
}

std::vector<const EditorActionDefinition*> EditorActionRegistry::GetActionsForCategory(const std::string& category) const
{
    const std::string normalizedCategory = ToLowerAscii(category);
    std::vector<const EditorActionDefinition*> actions;
    actions.reserve(m_Actions.size());
    for (const StoredAction& stored : m_Actions) {
        if (stored.CategoryLower == normalizedCategory) {
            actions.push_back(&stored.Definition);
        }
    }

    std::sort(actions.begin(), actions.end(), [](const EditorActionDefinition* a, const EditorActionDefinition* b) {
        if (a == nullptr || b == nullptr) {
            return a != nullptr;
        }
        if (a->SortOrder == b->SortOrder) {
            return a->Label < b->Label;
        }
        return a->SortOrder < b->SortOrder;
    });
    return actions;
}

std::vector<EditorActionSearchResult> EditorActionRegistry::Search(const std::string& query, size_t maxResults) const
{
    std::vector<EditorActionSearchResult> results;
    results.reserve(m_Actions.size());

    const std::string normalizedQuery = ToLowerAscii(query);
    const std::vector<std::string> tokens = TokenizeQuery(normalizedQuery);

    for (const StoredAction& stored : m_Actions) {
        if (!stored.Definition.ShowInCommandPalette) {
            continue;
        }
        if (stored.Definition.IsEnabled && !stored.Definition.IsEnabled()) {
            continue;
        }

        const float score = ScoreAction(stored, tokens, normalizedQuery);
        if (score < 0.0f) {
            continue;
        }

        results.push_back(EditorActionSearchResult{ &stored.Definition, score });
    }

    std::sort(results.begin(), results.end(), [](const EditorActionSearchResult& a, const EditorActionSearchResult& b) {
        if (a.Score == b.Score) {
            return a.Action->Label < b.Action->Label;
        }
        return a.Score > b.Score;
    });

    if (results.size() > maxResults) {
        results.resize(maxResults);
    }
    return results;
}

std::vector<const EditorActionDefinition*> EditorActionRegistry::GetPersistedActions() const
{
    std::vector<const EditorActionDefinition*> actions;
    actions.reserve(m_Actions.size());
    for (const StoredAction& stored : m_Actions) {
        if (stored.Definition.PersistWindowState) {
            actions.push_back(&stored.Definition);
        }
    }
    return actions;
}

void EditorActionRegistry::SetUsageCounts(std::unordered_map<std::string, int> usageCounts)
{
    m_UsageCounts = std::move(usageCounts);
    ++m_Version;
}

const EditorActionRegistry::StoredAction* EditorActionRegistry::FindStored(const std::string& id) const
{
    const auto it = m_IndexById.find(id);
    if (it == m_IndexById.end()) {
        return nullptr;
    }
    return &m_Actions[it->second];
}

bool EditorActionRegistry::ShortcutMatches(const EditorShortcut& shortcut) const
{
    if (shortcut.Key == ImGuiKey_None) {
        return false;
    }

    const ImGuiIO& io = ImGui::GetIO();
    if (!shortcut.AllowWhenTextInput && io.WantTextInput) {
        return false;
    }

    if (shortcut.Ctrl != io.KeyCtrl ||
        shortcut.Shift != io.KeyShift ||
        shortcut.Alt != io.KeyAlt ||
        shortcut.Super != io.KeySuper) {
        return false;
    }

    return ImGui::IsKeyPressed(shortcut.Key, false);
}

float EditorActionRegistry::ScoreAction(const StoredAction& action,
                                        const std::vector<std::string>& tokens,
                                        const std::string& normalizedQuery) const
{
    const auto usageIt = m_UsageCounts.find(action.Definition.Id);
    const float usageBoost = usageIt != m_UsageCounts.end()
        ? static_cast<float>(std::min(usageIt->second, 40)) * 1.75f
        : 0.0f;

    if (normalizedQuery.empty()) {
        return 24.0f + usageBoost - static_cast<float>(action.Definition.SortOrder) * 0.05f;
    }

    for (const std::string& token : tokens) {
        if (action.SearchIndex.find(token) == std::string::npos) {
            return -1.0f;
        }
    }

    float score = usageBoost;

    if (action.LabelLower == normalizedQuery) {
        score += 260.0f;
    } else if (action.LabelLower.find(normalizedQuery) == 0) {
        score += 180.0f;
    } else if (action.Definition.Id == normalizedQuery) {
        score += 220.0f;
    } else if (!action.CategoryLower.empty() && action.CategoryLower.find(normalizedQuery) == 0) {
        score += 90.0f;
    } else {
        score += 60.0f;
    }

    score += static_cast<float>(tokens.size()) * 8.0f;
    score -= static_cast<float>(action.Definition.SortOrder) * 0.05f;
    return score;
}

void EditorActionRegistry::TouchUsage(const std::string& id)
{
    ++m_UsageCounts[id];
    ++m_Version;
}

} // namespace editorui
