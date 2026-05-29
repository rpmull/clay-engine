#include "EditorCommandPalette.h"

#include <algorithm>

#include <imgui.h>
#include <imgui_internal.h>

namespace editorui {

void EditorCommandPalette::Open()
{
    m_OpenRequested = true;
    m_Selection = 0;
    m_Filter[0] = '\0';
}

void EditorCommandPalette::Render(EditorActionRegistry& registry)
{
    if (m_OpenRequested) {
        ImGui::OpenPopup("Command Palette");
        m_OpenRequested = false;
    }

    ImGui::SetNextWindowSize(ImVec2(720.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Command Palette", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    if (ImGui::IsWindowAppearing()) {
        m_LastRegistryVersion = 0;
        m_LastQuery.clear();
        ImGui::SetKeyboardFocusHere();
    }

    ImGui::SeparatorText("Actions");
    ImGui::InputTextWithHint(
        "##CommandPaletteFilter",
        "Search actions, windows, and tools...",
        m_Filter,
        IM_ARRAYSIZE(m_Filter),
        ImGuiInputTextFlags_AutoSelectAll);
    ImGui::TextDisabled("Enter to run, Up/Down to navigate, Esc to close");
    ImGui::Spacing();

    RefreshResults(registry);

    if (m_Selection >= static_cast<int>(m_CachedResults.size())) {
        m_Selection = std::max(0, static_cast<int>(m_CachedResults.size()) - 1);
    }

    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) && !m_CachedResults.empty()) {
        m_Selection = (m_Selection + 1) % static_cast<int>(m_CachedResults.size());
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) && !m_CachedResults.empty()) {
        m_Selection = (m_Selection - 1 + static_cast<int>(m_CachedResults.size())) % static_cast<int>(m_CachedResults.size());
    }

    ImGui::BeginChild("##CommandPaletteResults", ImVec2(720.0f, 380.0f), true);
    if (m_CachedResults.empty()) {
        ImGui::TextDisabled("No actions match \"%s\".", m_Filter);
    } else {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_CachedResults.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const EditorActionDefinition* action = m_CachedResults[static_cast<size_t>(row)].Action;
                if (!action) {
                    continue;
                }

                const bool selected = row == m_Selection;
                ImGui::PushID(action->Id.c_str());
                if (ImGui::Selectable(action->Label.c_str(), selected, ImGuiSelectableFlags_None, ImVec2(0.0f, 0.0f))) {
                    registry.Execute(action->Id);
                    ImGui::CloseCurrentPopup();
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }

                if (!action->Category.empty()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("  %s", action->Category.c_str());
                }

                if (!action->ShortcutLabel.empty()) {
                    const float rightX = ImGui::GetWindowContentRegionMax().x;
                    const float shortcutWidth = ImGui::CalcTextSize(action->ShortcutLabel.c_str()).x;
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), rightX - shortcutWidth));
                    ImGui::TextDisabled("%s", action->ShortcutLabel.c_str());
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();

    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && !m_CachedResults.empty()) {
        if (const EditorActionDefinition* action = m_CachedResults[static_cast<size_t>(m_Selection)].Action) {
            registry.Execute(action->Id);
        }
        ImGui::CloseCurrentPopup();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void EditorCommandPalette::RefreshResults(EditorActionRegistry& registry)
{
    const std::string currentQuery = m_Filter;
    if (m_LastRegistryVersion == registry.GetVersion() && m_LastQuery == currentQuery) {
        return;
    }

    m_LastRegistryVersion = registry.GetVersion();
    m_LastQuery = currentQuery;
    m_CachedResults = registry.Search(currentQuery, 96);
    if (m_Selection >= static_cast<int>(m_CachedResults.size())) {
        m_Selection = std::max(0, static_cast<int>(m_CachedResults.size()) - 1);
    }
}

} // namespace editorui
