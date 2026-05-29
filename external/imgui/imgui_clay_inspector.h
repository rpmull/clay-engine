#pragma once

#include "imgui.h"
#include <functional>

namespace ImGui
{
    struct ClayInspectorStyle
    {
        float HeaderHeight = 26.0f;
        float HeaderRounding = 3.0f;
        float HeaderBorderThickness = 1.0f;
        float HeaderPaddingX = 8.0f;
        float HeaderArrowLabelSpacing = 6.0f;
        float HeaderCheckboxSize = 16.0f;
        float HeaderArrowSize = 10.0f;
        float HeaderSpacing = 3.0f;

        float LabelColumnWidth = 130.0f;
        float RowMinHeight = 22.0f;
        float FieldSpacing = 6.0f;
        float VectorLabelWidth = 18.0f;
        float SliderNumberWidth = 70.0f;
        float FieldRounding = 2.0f;
        float FieldBorderThickness = 1.0f;

        ImVec4 HeaderBg;
        ImVec4 HeaderBgHovered;
        ImVec4 HeaderBgActive;
        ImVec4 HeaderBorder;
        ImVec4 HeaderText;
        ImVec4 HeaderTextDisabled;
        ImVec4 FieldBg;
        ImVec4 FieldBgHovered;
        ImVec4 FieldBgActive;
        ImVec4 FieldBorder;
        ImVec4 LabelColor;
        ImVec4 SecondaryText;
    };

    IMGUI_API ClayInspectorStyle& ClayInspectorGetStyle();

    struct ClayHeaderBarConfig
    {
        bool* Enabled = nullptr;
        bool DefaultOpen = true;
        bool ShowOptionsButton = true;
        bool AllowContextClick = true;
        ImGuiID IdOverride = 0;
        float WidthOverride = 0.0f;
    };

    struct ClayHeaderBarResult
    {
        bool open = false;
        bool toggled = false;
        bool optionsClicked = false;
        bool contextRequested = false;
        bool hovered = false;
        bool pressed = false;
        ImVec2 rectMin{0.0f, 0.0f};
        ImVec2 rectMax{0.0f, 0.0f};
    };

    IMGUI_API ClayHeaderBarResult ClayHeaderBar(const char* label, const ClayHeaderBarConfig& config = ClayHeaderBarConfig());

    struct ClayHeaderStripConfig
    {
        float WidthOverride = 0.0f;
        float HeightOverride = 0.0f;
        bool AllowContextClick = false;
    };

    struct ClayHeaderStripResult
    {
        bool hovered = false;
        bool pressed = false;
        bool contextRequested = false;
        ImVec2 rectMin{0.0f, 0.0f};
        ImVec2 rectMax{0.0f, 0.0f};
    };

    IMGUI_API ClayHeaderStripResult ClayHeaderStrip(const char* label, const ClayHeaderStripConfig& config = ClayHeaderStripConfig());

    struct ClaySplitterConfig
    {
        bool Vertical = true;              // true = left/right splitter (X axis), false = top/bottom splitter (Y axis)
        bool InvertAxis = false;           // invert drag direction (useful when primary pane is on the right/bottom)
        float Thickness = 4.0f;
        float MinPrimary = 120.0f;
        float MinSecondary = 120.0f;
        ImGuiMouseCursor HoverCursor = ImGuiMouseCursor_ResizeEW;
    };
    // Draws a splitter and updates primarySize while preserving minimum sizes.
    IMGUI_API bool ClaySplitter(const char* id, float* primarySize, float totalSize, float crossAxisSize, const ClaySplitterConfig& config = ClaySplitterConfig());

    class ClayInspectorContentScope
    {
    public:
        ClayInspectorContentScope(const char* id, float labelColumnWidth = -1.0f);
        ~ClayInspectorContentScope();

        bool BeginRow(const char* label, float minHeight = -1.0f);
        void EndRow();
        float FieldWidth() const;

    private:
        float m_LabelColumnWidth = 0.0f;
        bool m_InRow = false;
        bool m_TableOpen = false;
        float m_LastFieldWidth = 0.0f;
    };

    IMGUI_API bool ClayFieldFloat(ClayInspectorContentScope& scope, const char* label, float* value, float speed = 0.1f, float min = 0.0f, float max = 0.0f, const char* format = "%.3f");
    IMGUI_API bool ClayFieldInt(ClayInspectorContentScope& scope, const char* label, int* value, float speed = 1.0f, int min = 0, int max = 0, const char* format = "%d");
    IMGUI_API bool ClayFieldCheckbox(ClayInspectorContentScope& scope, const char* label, bool* value);
    using ClayDropdownBuilder = std::function<bool()>;
    IMGUI_API bool ClayFieldDropdown(ClayInspectorContentScope& scope, const char* label, const char* preview, const ClayDropdownBuilder& builder);
    IMGUI_API bool ClayFieldVec(ClayInspectorContentScope& scope, const char* label, float* values, int components, float speed = 0.1f);
    IMGUI_API bool ClayFieldSlider(ClayInspectorContentScope& scope, const char* label, float* value, float min, float max, const char* format = "%.2f");
}




















