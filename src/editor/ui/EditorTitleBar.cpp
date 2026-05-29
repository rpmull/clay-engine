#include "EditorTitleBar.h"
#include <imgui_internal.h>
#include <windows.h>

EditorTitleBar::EditorTitleBar() {
    m_TitleBarHeight = s_TitleBarHeight;
}

float EditorTitleBar::Render() {
    if (!m_hWnd)
        return m_TitleBarHeight;

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!window)
        return m_TitleBarHeight;

    ImVec2 windowPos  = ImGui::GetWindowPos();
    ImVec2 windowSize = ImGui::GetWindowSize();

    // Title bar background
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImRect titleBarRect(windowPos.x,
                        windowPos.y,
                        windowPos.x + windowSize.x,
                        windowPos.y + m_TitleBarHeight);

    ImU32 bgColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.22f, 0.22f, 0.23f, 1.0f));
    drawList->AddRectFilled(titleBarRect.Min, titleBarRect.Max, bgColor);

    // Bottom separator
    ImVec2 separatorStart(windowPos.x,              windowPos.y + m_TitleBarHeight);
    ImVec2 separatorEnd  (windowPos.x + windowSize.x, windowPos.y + m_TitleBarHeight);
    ImU32 separatorColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.15f, 0.15f, 0.16f, 1.0f));
    drawList->AddLine(separatorStart, separatorEnd, separatorColor, 1.0f);

    // Draw left-side icon + title
    DrawTitleAndIcon();

    // Draw right-side window buttons
    DrawWindowControlButtons();

    return m_TitleBarHeight;
}

void EditorTitleBar::DrawTitleAndIcon() {
    ImVec2 windowPos = ImGui::GetWindowPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    
    float iconX = windowPos.x + s_IconPadding;
    float iconY = windowPos.y + (m_TitleBarHeight - s_IconSize) * 0.5f;
    
    // Simple icon placeholder - a small square/logo area
    // In a real implementation, you'd load the actual Claymore icon texture here
    ImRect iconRect(iconX, iconY, iconX + s_IconSize, iconY + s_IconSize);
    ImU32 iconColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.28f, 0.55f, 0.92f, 1.0f)); // Claymore blue
    drawList->AddRectFilled(iconRect.Min, iconRect.Max, iconColor);
    
    // Title text
    float titleX = iconX + s_IconSize + s_TitlePadding;
    float titleY = windowPos.y + (m_TitleBarHeight - ImGui::GetFontSize()) * 0.5f;
    
    ImVec2 titlePos(titleX, titleY);
    ImU32 titleColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
    drawList->AddText(titlePos, titleColor, "Claymore Editor");
}

void EditorTitleBar::DrawWindowControlButtons() {
    if (!m_hWnd)
        return;

    ImVec2 windowPos  = ImGui::GetWindowPos();
    ImVec2 windowSize = ImGui::GetWindowSize();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    const float titleBarHeight = m_TitleBarHeight;
    const float buttonWidth    = s_ButtonWidth;
    const float totalButtonWidth = buttonWidth * 3.0f;

    const float buttonTopY    = windowPos.y;
    const float buttonBottomY = windowPos.y + titleBarHeight;
    const float buttonStartX  = windowPos.x + windowSize.x - totalButtonWidth;

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mousePos = io.MousePos;

    auto DrawCenteredIcon = [&](const ImRect& rect, const char* icon, ImU32 color) {
        ImVec2 textSize = ImGui::CalcTextSize(icon);
        ImVec2 iconPos(
            rect.Min.x + 0.5f * (rect.GetWidth()  - textSize.x),
            rect.Min.y + 0.5f * (rect.GetHeight() - textSize.y)
        );
        drawList->AddText(iconPos, color, icon);
    };

    auto DrawButton = [&](float xMin, const char* icon,
                          bool& hoveredFlag, bool& pressedFlag,
                          bool isCloseButton, bool isMaximizeButton) {
        ImRect rect(xMin, buttonTopY, xMin + buttonWidth, buttonBottomY);
        bool hovered = rect.Contains(mousePos);
        bool clicked = hovered && ImGui::IsMouseClicked(0);

        hoveredFlag = hovered;
        if (clicked) pressedFlag = true;

        ImU32 baseColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.22f, 0.22f, 0.23f, 1.0f));
        ImU32 hoverColor = isCloseButton
            ? ImGui::ColorConvertFloat4ToU32(ImVec4(0.8f, 0.25f, 0.25f, 1.0f))
            : ImGui::ColorConvertFloat4ToU32(ImVec4(0.35f, 0.35f, 0.35f, 1.0f));
        ImU32 pressedColor = isCloseButton
            ? ImGui::ColorConvertFloat4ToU32(ImVec4(0.7f, 0.2f, 0.2f, 1.0f))
            : ImGui::ColorConvertFloat4ToU32(ImVec4(0.3f, 0.3f, 0.3f, 1.0f));

        ImU32 fillColor = baseColor;
        if (pressedFlag && ImGui::IsMouseDown(0))
            fillColor = pressedColor;
        else if (hovered)
            fillColor = hoverColor;

        drawList->AddRectFilled(rect.Min, rect.Max, fillColor);

        ImU32 iconColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.9f, 0.9f, 0.9f, 1.0f));

        if (isMaximizeButton) {
            bool maxed = IsMaximized();

            if (maxed) {
                float iconSize = ImGui::GetFontSize() * 0.6f;
                ImVec2 center  = rect.GetCenter();
                float offset   = iconSize * 0.25f;

                ImVec2 frontMin(center.x - iconSize * 0.5f - offset,
                                center.y - iconSize * 0.5f - offset);
                ImVec2 frontMax(center.x + iconSize * 0.5f - offset,
                                center.y + iconSize * 0.5f - offset);
                ImVec2 backMin(center.x - iconSize * 0.5f + offset,
                               center.y - iconSize * 0.5f + offset);
                ImVec2 backMax(center.x + iconSize * 0.5f + offset,
                               center.y + iconSize * 0.5f + offset);

                drawList->AddRect(backMin,  backMax,  iconColor, 0.0f, 0, 1.5f);
                drawList->AddRect(frontMin, frontMax, iconColor, 0.0f, 0, 1.5f);
            } else {
                DrawCenteredIcon(rect, icon, iconColor);
            }
        } else {
            DrawCenteredIcon(rect, icon, iconColor);
        }

        return clicked;
    };

    float xMinimize = buttonStartX;
    float xMaximize = buttonStartX + buttonWidth;
    float xClose    = buttonStartX + 2.0f * buttonWidth;

    bool clickedMin  = DrawButton(xMinimize, "−", m_MinimizeHovered, m_MinimizePressed, false, false);
    bool clickedMax  = DrawButton(xMaximize, "□", m_MaximizeHovered, m_MaximizePressed, false, true);
    bool clickedClose= DrawButton(xClose,    "×", m_CloseHovered,    m_ClosePressed,    true,  false);

    if (clickedMin)   MinimizeWindow();
    if (clickedMax)   MaximizeRestoreWindow();
    if (clickedClose) CloseWindow();

    if (!ImGui::IsMouseDown(0)) {
        m_MinimizePressed = false;
        m_MaximizePressed = false;
        m_ClosePressed    = false;
    }
}

bool EditorTitleBar::IsPointInDragRegion(const ImVec2& point) const {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!window) return false;
    
    ImVec2 windowPos = window->Pos;
    ImVec2 windowSize = window->Size;
    
    // Check if point is within title bar height
    if (point.y < windowPos.y || point.y > windowPos.y + m_TitleBarHeight) {
        return false;
    }
    
    // Exclude button regions on the right
    float buttonStartX = windowPos.x + windowSize.x - (s_ButtonWidth * 3.0f);
    if (point.x >= buttonStartX) {
        return false;
    }
    
    // Allow dragging from the entire title bar except buttons
    return true;
}

ImRect EditorTitleBar::GetDragRegion() const {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!window)
        return ImRect(0, 0, 0, 0);

    ImVec2 windowPos  = ImGui::GetWindowPos();
    ImVec2 windowSize = ImGui::GetWindowSize();

    const float totalButtonWidth = s_ButtonWidth * 3.0f;
    const float dragRight        = windowPos.x + windowSize.x - totalButtonWidth;

    return ImRect(
        windowPos.x,
        windowPos.y,
        dragRight,
        windowPos.y + m_TitleBarHeight
    );
}

void EditorTitleBar::MinimizeWindow() {
    if (m_hWnd) {
        ShowWindow(m_hWnd, SW_MINIMIZE);
    }
}

void EditorTitleBar::MaximizeRestoreWindow() {
    if (!m_hWnd) return;
    
    WINDOWPLACEMENT wp;
    wp.length = sizeof(WINDOWPLACEMENT);
    if (GetWindowPlacement(m_hWnd, &wp)) {
        if (wp.showCmd == SW_SHOWMAXIMIZED) {
            ShowWindow(m_hWnd, SW_RESTORE);
        } else {
            ShowWindow(m_hWnd, SW_MAXIMIZE);
        }
    }
}

void EditorTitleBar::CloseWindow() {
    if (m_hWnd) {
        PostMessage(m_hWnd, WM_CLOSE, 0, 0);
    }
}

bool EditorTitleBar::IsMaximized() const {
    if (!m_hWnd) return false;
    
    WINDOWPLACEMENT wp;
    wp.length = sizeof(WINDOWPLACEMENT);
    if (GetWindowPlacement(m_hWnd, &wp)) {
        return wp.showCmd == SW_SHOWMAXIMIZED;
    }
    return false;
}

