#include "ConsoleOverlay.h"
#include "CommandRegistry.h"
#include "core/input/Input.h"
#include "core/rendering/Renderer.h"
#include <editor/application.h>

// VK mappings in our Input are ASCII for letters/numbers and a subset of GLFW. We will use '/' (0x2F) and '`' (0x60) toggles.

void CommandConsole::RenderInViewport(const ImVec2& viewportTL, const ImVec2& viewportSize, bool isPlayMode) {
    (void)viewportTL; (void)viewportSize;
    if (!isPlayMode) { m_Active = false; Input::SetBlocked(false); return; }

    // Toggle with '/' or '~' (backtick).
    // '/' ascii 47, '`' ascii 96 (tilde shares key on US keyboards).
    if (Input::WasKeyPressedThisFrame('/')) m_Active = !m_Active;
    if (Input::WasKeyPressedThisFrame('`')) m_Active = !m_Active;

    if (!m_Active) { Input::SetBlocked(false); return; }

    // Block game input for this frame while console is active
    // Mark UI input consumed so picking/game controls are gated
    // We piggy-back on Renderer flag via a tiny dummy hitbox in UI region; just set consumed implicitly by drawing a focusable item

    // Draw a thin input bar at the bottom of the viewport-sized window (current ImGui window)
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0,0,0,0.6f));
    ImGui::SetNextWindowPos(ImVec2(viewportTL.x + 8.0f, viewportTL.y + viewportSize.y - 36.0f));
    ImGui::SetNextWindowSize(ImVec2(viewportSize.x - 16.0f, 28.0f));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;
    // While console is active, block game input (except toggle keys handled in Input)
    Input::SetBlocked(true);
    if (ImGui::Begin("##PlayConsole", nullptr, flags)) {
        ImGui::PushItemWidth(-1);
        if (ImGui::InputText("##cmd", m_Buffer, sizeof(m_Buffer), ImGuiInputTextFlags_EnterReturnsTrue)) {
            std::string cmd = m_Buffer; m_History.push_back(cmd); m_HistoryPos = (int)m_History.size();
            m_Buffer[0] = '\0';
            // Execute
            CommandRegistry::Instance().Execute(cmd);
        }
        // Consume input
        // This ensures editor picking doesn't run
        ImGui::GetIO().WantCaptureKeyboard = true;
        ImGui::GetIO().WantCaptureMouse = true;
        // History up/down
        if (ImGui::IsWindowFocused()) {
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && m_HistoryPos > 0) {
                m_HistoryPos--; strncpy(m_Buffer, m_History[m_HistoryPos].c_str(), sizeof(m_Buffer));
            } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && m_HistoryPos + 1 < (int)m_History.size()) {
                m_HistoryPos++; strncpy(m_Buffer, m_History[m_HistoryPos].c_str(), sizeof(m_Buffer));
            }
        }
        ImGui::PopItemWidth();
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}


