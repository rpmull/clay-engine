#pragma once
#include <imgui.h>
#include <imgui_internal.h>
#include <windows.h>

// Handles rendering and interaction for the custom window chrome
class EditorTitleBar {
public:
    EditorTitleBar();
    ~EditorTitleBar() = default;

    // Render the custom title bar UI
    // Returns the height of the title bar in pixels
    float Render();

    // Get the title bar height
    float GetHeight() const { return m_TitleBarHeight; }

    // Check if a point (in ImGui coordinates) is within the title bar drag region
    bool IsPointInDragRegion(const ImVec2& point) const;

    // Get the bounding rectangle of the title bar (for hit-testing)
    ImRect GetDragRegion() const;

    // Window control button actions (called from UI)
    void MinimizeWindow();
    void MaximizeRestoreWindow();
    void CloseWindow();

    // Set the window handle (needed for window operations)
    void SetWindowHandle(HWND hwnd) { m_hWnd = hwnd; }

    // Check if window is maximized
    bool IsMaximized() const;

private:
    void DrawWindowControlButtons();
    void DrawTitleAndIcon();

    HWND m_hWnd = nullptr;
    float m_TitleBarHeight = 30.0f;
    
    // Button states for hover/press feedback
    bool m_MinimizeHovered = false;
    bool m_MaximizeHovered = false;
    bool m_CloseHovered = false;
    bool m_MinimizePressed = false;
    bool m_MaximizePressed = false;
    bool m_ClosePressed = false;

    // Layout constants
    static constexpr float s_TitleBarHeight = 30.0f;
    static constexpr float s_IconPadding    = 8.0f;
    static constexpr float s_IconSize       = 14.0f;
    static constexpr float s_TitlePadding   = 6.0f;
    static constexpr float s_ButtonWidth    = 46.0f;
};

