#include "EditorNotifications.h"

#include <algorithm>

#include <imgui.h>
#include <imgui_claymore_style.h>

namespace editorui {
namespace {

ImVec4 ColorForLevel(EditorNotificationLevel level)
{
    const ClayEditorTheme& theme = Clay_GetEditorTheme();
    switch (level) {
        case EditorNotificationLevel::Success: return theme.Success;
        case EditorNotificationLevel::Warning: return theme.Warning;
        case EditorNotificationLevel::Error: return theme.Error;
        case EditorNotificationLevel::Info:
        default: return theme.SelectionAccent;
    }
}

} // namespace

void EditorNotifications::Push(EditorNotificationLevel level, std::string message, float lifetimeSeconds)
{
    if (message.empty()) {
        return;
    }

    const double now = ImGui::GetTime();
    for (Toast& toast : m_Toasts) {
        if (toast.Level == level && toast.Message == message && (now - toast.CreatedAt) < 1.25) {
            toast.CreatedAt = now;
            toast.LifetimeSeconds = std::max(toast.LifetimeSeconds, lifetimeSeconds);
            ++toast.Count;
            return;
        }
    }

    Toast toast;
    toast.Id = m_NextId++;
    toast.Level = level;
    toast.Message = std::move(message);
    toast.CreatedAt = now;
    toast.LifetimeSeconds = lifetimeSeconds;
    m_Toasts.push_back(std::move(toast));
}

void EditorNotifications::Render()
{
    if (m_Toasts.empty()) {
        return;
    }

    const double now = ImGui::GetTime();
    m_Toasts.erase(std::remove_if(m_Toasts.begin(), m_Toasts.end(), [&](const Toast& toast) {
        return (now - toast.CreatedAt) >= toast.LifetimeSeconds;
    }), m_Toasts.end());

    if (m_Toasts.empty()) {
        return;
    }

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (!viewport) {
        return;
    }

    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 18.0f, viewport->WorkPos.y + 18.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.0f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_NoDocking |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoFocusOnAppearing;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 8.0f));
    if (ImGui::Begin("##EditorNotifications", nullptr, flags)) {
        const ClayEditorTheme& theme = Clay_GetEditorTheme();
        for (const Toast& toast : m_Toasts) {
            ImGui::PushID(static_cast<int>(toast.Id));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.SurfaceRaised);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
            if (ImGui::BeginChild("##toast", ImVec2(340.0f, 0.0f), true)) {
                const ImVec4 accent = ColorForLevel(toast.Level);
                const float elapsed = static_cast<float>(now - toast.CreatedAt);
                const float progress = toast.LifetimeSeconds > 0.0f
                    ? std::clamp(1.0f - elapsed / toast.LifetimeSeconds, 0.0f, 1.0f)
                    : 0.0f;

                ImGui::PushStyleColor(ImGuiCol_Text, accent);
                ImGui::TextUnformatted(toast.Message.c_str());
                ImGui::PopStyleColor();

                if (toast.Count > 1) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("x%d", toast.Count);
                }

                const ImVec2 barMin = ImGui::GetCursorScreenPos();
                const float width = ImGui::GetContentRegionAvail().x;
                const ImVec2 barMax(barMin.x + width, barMin.y + 2.0f);
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                drawList->AddRectFilled(barMin, barMax, ImGui::GetColorU32(theme.BorderSubtle), 1.0f);
                drawList->AddRectFilled(barMin, ImVec2(barMin.x + width * progress, barMax.y), ImGui::GetColorU32(accent), 1.0f);
                ImGui::Dummy(ImVec2(width, 4.0f));
            }
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

} // namespace editorui
