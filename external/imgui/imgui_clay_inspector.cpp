#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include "imgui_clay_inspector.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cfloat>
#include <string>

namespace ImGui
{
    namespace
    {
        ClayInspectorStyle InitDefaultInspectorStyle()
        {
            ClayInspectorStyle style;
            style.HeaderBg = ImVec4(0.215f, 0.215f, 0.215f, 1.0f);
            style.HeaderBgHovered = ImVec4(0.245f, 0.245f, 0.245f, 1.0f);
            style.HeaderBgActive = ImVec4(0.265f, 0.265f, 0.265f, 1.0f);
            style.HeaderBorder = ImVec4(0.08f, 0.08f, 0.08f, 1.0f);
            style.HeaderText = ImVec4(0.92f, 0.92f, 0.92f, 1.0f);
            style.HeaderTextDisabled = ImVec4(0.54f, 0.54f, 0.54f, 1.0f);
            style.FieldBg = ImVec4(0.18f, 0.18f, 0.18f, 1.0f);
            style.FieldBgHovered = ImVec4(0.22f, 0.22f, 0.22f, 1.0f);
            style.FieldBgActive = ImVec4(0.26f, 0.26f, 0.26f, 1.0f);
            style.FieldBorder = ImVec4(0.06f, 0.06f, 0.06f, 1.0f);
            style.LabelColor = ImVec4(0.82f, 0.82f, 0.82f, 1.0f);
            style.SecondaryText = ImVec4(0.64f, 0.64f, 0.64f, 1.0f);
            return style;
        }

        void DrawRoundedRect(ImDrawList* dl, const ImRect& rect, ImU32 col, float rounding, float thickness = 1.0f)
        {
            dl->AddRect(rect.Min, rect.Max, col, rounding, ImDrawFlags_None, thickness);
        }

        ImRect MakeRect(const ImVec2& min, const ImVec2& size)
        {
            return ImRect(min, ImVec2(min.x + size.x, min.y + size.y));
        }

        void ClayEnsureTableRowContributes(ImGuiTable* table)
        {
            if (!table)
                return;
            ImGuiWindow* inner = table->InnerWindow;
            if (!inner)
                return;

            const float row_bottom = table->RowPosY2;
            inner->DC.CursorMaxPos.y = ImMax(inner->DC.CursorMaxPos.y, row_bottom);
            inner->ContentSize.y = ImMax(inner->ContentSize.y, row_bottom - inner->DC.CursorStartPos.y);
            inner->ScrollMax.y = ImMax(0.0f, inner->ContentSize.y - inner->Size.y);
        }

        void ClayItemSizeEvenWhenSkipped(ImGuiWindow* window, const ImVec2& size, float text_baseline_y)
        {
            if (!window)
                return;

            bool wasSkipping = window->SkipItems;
            if (wasSkipping)
                window->SkipItems = false;

            ItemSize(size, text_baseline_y);

            if (wasSkipping)
                window->SkipItems = true;
        }
    } // namespace

    ClayInspectorStyle& ClayInspectorGetStyle()
    {
        static ClayInspectorStyle s_Style = InitDefaultInspectorStyle();
        return s_Style;
    }

    ClayHeaderBarResult ClayHeaderBar(const char* label, const ClayHeaderBarConfig& config)
    {
        ClayHeaderBarResult result;
        ImGuiWindow* window = GetCurrentWindow();
        if (!window)
            return result;
        
        ImGuiContext& g = *GImGui;
        ClayInspectorStyle& style = ClayInspectorGetStyle();
        const char* labelEnd = FindRenderedTextEnd(label);
        std::string displayLabel(label, labelEnd);

        const float width = (config.WidthOverride > 0.0f) ? config.WidthOverride : GetContentRegionAvail().x;
        const float headerHeight = ImMax(style.HeaderHeight, g.FontSize + g.Style.FramePadding.y * 2.0f);
        const ImVec2 start = window->DC.CursorPos;
        ImRect bb = MakeRect(start, ImVec2(width, headerHeight));
        
        ClayItemSizeEvenWhenSkipped(window, bb.GetSize(), style.HeaderSpacing);
        const auto advanceClippedHeader = [&]() {
            window->DC.CursorPos.y = bb.Max.y;
            window->DC.CursorMaxPos.y = ImMax(window->DC.CursorMaxPos.y, bb.Max.y);
            if (window->ParentWindow == nullptr || window->ParentWindow == window)
                window->ContentSize.y = ImMax(window->ContentSize.y, bb.Max.y - window->DC.CursorStartPos.y);
        };

        ImGuiID id = config.IdOverride ? config.IdOverride : window->GetID(label);
        ImGuiStorage* storage = window->DC.StateStorage;
        bool isOpen = storage->GetInt(id, config.DefaultOpen ? 1 : 0) != 0;
        const auto finalizeClippedHeader = [&]() -> ClayHeaderBarResult&
        {
            result.rectMin = bb.Min;
            result.rectMax = bb.Max;
            result.open = isOpen;
            return result;
        };

        if (window->SkipItems || width <= 0.0f)
        {
            advanceClippedHeader();
            return finalizeClippedHeader();
        }

        if (!ItemAdd(bb, id))
        {
            advanceClippedHeader();
            return finalizeClippedHeader();
        }

        result.rectMin = bb.Min;
        result.rectMax = bb.Max;

        if (g.NextItemData.HasFlags & ImGuiNextItemDataFlags_HasOpen)
        {
            const bool openOverride = g.NextItemData.OpenVal;
            if (g.NextItemData.OpenCond & ImGuiCond_Always)
            {
                isOpen = openOverride;
                storage->SetInt(id, openOverride ? 1 : 0);
            }
            else
            {
                const int stored = storage->GetInt(id, -1);
                if (stored == -1)
                {
                    isOpen = openOverride;
                    storage->SetInt(id, openOverride ? 1 : 0);
                }
            }
            g.NextItemData.ClearFlags();
        }

        const float checkboxSize = style.HeaderCheckboxSize;
        const float arrowSize = style.HeaderArrowSize;
        const float gearSize = checkboxSize;

        float cursorX = bb.Min.x + style.HeaderPaddingX;
        ImRect checkboxRect = MakeRect(ImVec2(cursorX, bb.Min.y + (headerHeight - checkboxSize) * 0.5f), ImVec2(checkboxSize, checkboxSize));
        if (config.Enabled)
            cursorX = checkboxRect.Max.x + style.HeaderPaddingX * 0.5f;

        ImRect arrowRect = MakeRect(ImVec2(cursorX, bb.Min.y + (headerHeight - arrowSize) * 0.5f), ImVec2(arrowSize, arrowSize));
        cursorX = arrowRect.Max.x + style.HeaderArrowLabelSpacing;

        const float textBaseline = bb.Min.y + (headerHeight - g.FontSize) * 0.5f;
        ImRect textRect(ImVec2(cursorX, textBaseline), ImVec2(bb.Max.x, textBaseline + g.FontSize));

        ImRect gearRect = MakeRect(ImVec2(bb.Max.x - style.HeaderPaddingX - gearSize, bb.Min.y + (headerHeight - gearSize) * 0.5f), ImVec2(gearSize, gearSize));

        ImGuiButtonFlags buttonFlags = ImGuiButtonFlags_NoNavFocus | ImGuiButtonFlags_PressedOnClickRelease | ImGuiButtonFlags_PressedOnDragDropHold;
        bool hovered = false;
        bool held = false;
        bool pressed = ButtonBehavior(bb, id, &hovered, &held, buttonFlags);

        const ImVec2 clickPos = g.IO.MouseClickedPos[0];
        const bool clickedOption = config.ShowOptionsButton && gearRect.Contains(clickPos);
        const bool clickedCheckbox = config.Enabled != nullptr && checkboxRect.Contains(clickPos);
        if (pressed && (clickedOption || clickedCheckbox))
            pressed = false;

        if (pressed && g.DragDropHoldJustPressedId != id)
        {
            isOpen = !isOpen;
            storage->SetInt(id, isOpen ? 1 : 0);
            result.toggled = true;
        }

        result.open = isOpen;
        result.hovered = hovered;
        result.pressed = pressed;

        const ImVec4 baseCol = hovered ? (held ? style.HeaderBgActive : style.HeaderBgHovered) : style.HeaderBg;
        ImVec4 topLine = ImLerp(baseCol, style.HeaderText, 0.06f);
        topLine.w = 0.78f;
        ImVec4 bottomLine = ImLerp(baseCol, style.HeaderBorder, 0.72f);
        bottomLine.w = 0.92f;
        ImDrawList* dl = window->DrawList;
        const ImU32 baseCol32 = ColorConvertFloat4ToU32(baseCol);
        dl->AddRectFilled(bb.Min, bb.Max, baseCol32, style.HeaderRounding);
        dl->AddLine(ImVec2(bb.Min.x + 1.0f, bb.Min.y + 1.0f),
                    ImVec2(bb.Max.x - 1.0f, bb.Min.y + 1.0f),
                    ColorConvertFloat4ToU32(topLine),
                    1.0f);
        dl->AddLine(ImVec2(bb.Min.x + 1.0f, bb.Max.y - 1.0f),
                    ImVec2(bb.Max.x - 1.0f, bb.Max.y - 1.0f),
                    ColorConvertFloat4ToU32(bottomLine),
                    1.0f);
        DrawRoundedRect(dl, bb, ColorConvertFloat4ToU32(style.HeaderBorder), style.HeaderRounding, style.HeaderBorderThickness);

        if (config.Enabled)
        {
            SetNextItemAllowOverlap();
            SetCursorScreenPos(checkboxRect.Min);
            PushID("ClayHeaderEnable");
            if (InvisibleButton("##enable", checkboxRect.GetSize()))
            {
                *config.Enabled = !*config.Enabled;
                result.toggled = true;
            }
            PopID();
            ImU32 fillCol = *config.Enabled ? ColorConvertFloat4ToU32(ImVec4(0.9f, 0.9f, 0.9f, 1.0f)) : ColorConvertFloat4ToU32(ImVec4(0.25f, 0.25f, 0.25f, 1.0f));
            dl->AddRect(checkboxRect.Min, checkboxRect.Max, ColorConvertFloat4ToU32(style.HeaderBorder), 2.0f, ImDrawFlags_None, 1.0f);
            if (*config.Enabled)
                RenderCheckMark(dl, checkboxRect.Min + ImVec2(2.0f, 2.0f), fillCol, checkboxSize - 4.0f);
        }

        RenderArrow(dl, ImVec2(arrowRect.Min.x, arrowRect.Min.y), ColorConvertFloat4ToU32(style.HeaderText), isOpen ? ImGuiDir_Down : ImGuiDir_Right, 1.0f);

        PushClipRect(bb.Min, bb.Max, true);
        const ImVec4 textColor = config.Enabled && !*config.Enabled ? style.HeaderTextDisabled : style.HeaderText;
        dl->AddText(ImVec2(textRect.Min.x, textRect.Min.y), ColorConvertFloat4ToU32(textColor), displayLabel.c_str());
        PopClipRect();

        if (config.ShowOptionsButton)
        {
            SetNextItemAllowOverlap();
            SetCursorScreenPos(gearRect.Min);
            PushID("ClayHeaderGear");
            if (InvisibleButton("##gear", gearRect.GetSize()))
                result.optionsClicked = true;
            PopID();
            const float cx = (gearRect.Min.x + gearRect.Max.x) * 0.5f;
            const float cy = (gearRect.Min.y + gearRect.Max.y) * 0.5f;
            const float r = gearSize * 0.35f;
            const ImU32 gearCol = ColorConvertFloat4ToU32(hovered ? style.HeaderText : style.SecondaryText);
            dl->PathClear();
            dl->PathArcTo(ImVec2(cx, cy), r, 0.0f, IM_PI * 2.0f, 12);
            dl->PathStroke(gearCol, 0, 1.5f);
            dl->AddCircleFilled(ImVec2(cx, cy), r * 0.4f, gearCol);
        }

        if (config.AllowContextClick && hovered && g.IO.MouseClicked[1])
        {
            result.contextRequested = true;
        }

        return result;
    }

    ClayHeaderStripResult ClayHeaderStrip(const char* label, const ClayHeaderStripConfig& config)
    {
        ClayHeaderStripResult result;
        ImGuiWindow* window = GetCurrentWindow();
        if (!window)
            return result;

        ImGuiContext& g = *GImGui;
        ClayInspectorStyle& style = ClayInspectorGetStyle();

        float width = (config.WidthOverride > 0.0f) ? config.WidthOverride : GetContentRegionAvail().x;
        float height = (config.HeightOverride > 0.0f)
            ? config.HeightOverride
            : ImMax(style.HeaderHeight, g.FontSize + g.Style.FramePadding.y * 2.0f);

        ImVec2 start = window->DC.CursorPos;
        ImRect bb(start, ImVec2(start.x + width, start.y + height));
        ClayItemSizeEvenWhenSkipped(window, bb.GetSize(), style.HeaderSpacing);

        result.rectMin = bb.Min;
        result.rectMax = bb.Max;

        if (width <= 0.0f || window->SkipItems)
            return result;

        ImGuiID id = window->GetID(label);
        if (!ItemAdd(bb, id))
            return result;

        bool hovered = false;
        bool held = false;
        ImGuiButtonFlags buttonFlags = ImGuiButtonFlags_NoNavFocus | ImGuiButtonFlags_PressedOnClickRelease;
        bool pressed = ButtonBehavior(bb, id, &hovered, &held, buttonFlags);

        result.hovered = hovered;
        result.pressed = pressed;
        if (config.AllowContextClick && hovered && g.IO.MouseClicked[1])
            result.contextRequested = true;

        ImDrawList* dl = window->DrawList;
        ImVec4 baseCol = hovered ? (held ? style.HeaderBgActive : style.HeaderBgHovered) : style.HeaderBg;
        ImVec4 topLine = ImLerp(baseCol, style.HeaderText, 0.06f);
        topLine.w = 0.78f;
        ImVec4 bottomLine = ImLerp(baseCol, style.HeaderBorder, 0.72f);
        bottomLine.w = 0.92f;
        dl->AddRectFilled(bb.Min, bb.Max, ColorConvertFloat4ToU32(baseCol), style.HeaderRounding);
        dl->AddLine(ImVec2(bb.Min.x + 1.0f, bb.Min.y + 1.0f),
                    ImVec2(bb.Max.x - 1.0f, bb.Min.y + 1.0f),
                    ColorConvertFloat4ToU32(topLine),
                    1.0f);
        dl->AddLine(ImVec2(bb.Min.x + 1.0f, bb.Max.y - 1.0f),
                    ImVec2(bb.Max.x - 1.0f, bb.Max.y - 1.0f),
                    ColorConvertFloat4ToU32(bottomLine),
                    1.0f);
        DrawRoundedRect(dl, bb, ColorConvertFloat4ToU32(style.HeaderBorder), style.HeaderRounding, style.HeaderBorderThickness);

        PushClipRect(bb.Min, bb.Max, true);
        ImVec2 textPos(bb.Min.x + style.HeaderPaddingX, bb.Min.y + (height - g.FontSize) * 0.5f);
        dl->AddText(textPos, ColorConvertFloat4ToU32(style.HeaderText), label);
        PopClipRect();

        return result;
    }

    bool ClaySplitter(const char* id, float* primarySize, float totalSize, float crossAxisSize, const ClaySplitterConfig& config)
    {
        if (!primarySize || totalSize <= 0.0f || crossAxisSize <= 0.0f)
            return false;

        const float thickness = ImMax(1.0f, config.Thickness);
        const float minPrimary = ImMax(0.0f, config.MinPrimary);
        const float minSecondary = ImMax(0.0f, config.MinSecondary);
        const float maxPrimary = ImMax(minPrimary, totalSize - thickness - minSecondary);

        if (*primarySize < minPrimary) *primarySize = minPrimary;
        if (*primarySize > maxPrimary) *primarySize = maxPrimary;

        const ImVec2 buttonSize = config.Vertical
            ? ImVec2(thickness, crossAxisSize)
            : ImVec2(crossAxisSize, thickness);

        bool changed = false;
        InvisibleButton(id, buttonSize);
        if (ImGui::IsItemActive())
        {
            float delta = config.Vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
            if (config.InvertAxis)
                delta = -delta;
            const float oldSize = *primarySize;
            *primarySize = ImClamp(*primarySize + delta, minPrimary, maxPrimary);
            changed = (*primarySize != oldSize);
        }
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetMouseCursor(config.HoverCursor);

        return changed;
    }

    ClayInspectorContentScope::ClayInspectorContentScope(const char* id, float labelColumnWidth)
    {
        m_LabelColumnWidth = (labelColumnWidth > 0.0f) ? labelColumnWidth : ClayInspectorGetStyle().LabelColumnWidth;
        PushID(id);
        PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.0f, 1.0f));
        PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ClayInspectorGetStyle().FieldSpacing, 2.0f));
        // Disable per-row clipping so every row always contributes to the host window's
        // CursorMaxPos. Without this, tables inside scrollable inspectors can report
        // shrinking content sizes once rows scroll out of view, which in turn resets
        // the parent window's scroll position (see SCROLL_BUG.md).
        const ImGuiTableFlags flags =
            ImGuiTableFlags_SizingFixedFit |
            ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_RowBg |
            ImGuiTableFlags_NoClip;
        if (BeginTable("##ClayInspectorGrid", 2, flags, ImVec2(-FLT_MIN, 0.0f)))
        {
            TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, m_LabelColumnWidth);
            TableSetupColumn("field", ImGuiTableColumnFlags_WidthStretch);
            m_TableOpen = true;
        }
    }

    ClayInspectorContentScope::~ClayInspectorContentScope()
    {
        if (m_TableOpen)
        {
            if (m_InRow)
                EndRow();
            EndTable();
        }
        PopStyleVar(2);
        PopID();
    }

    bool ClayInspectorContentScope::BeginRow(const char* label, float minHeight)
    {
        if (!m_TableOpen)
            return false;
        if (m_InRow)
            EndRow();
        const float height = ImMax(minHeight, ClayInspectorGetStyle().RowMinHeight);
        TableNextRow(ImGuiTableRowFlags_None, height);
        const int rowIndex = TableGetRowIndex();
        const bool labelVisible = TableSetColumnIndex(0);
        PushStyleColor(ImGuiCol_Text, ClayInspectorGetStyle().LabelColor);
        AlignTextToFramePadding();
        TextUnformatted(label);
        PopStyleColor();
        const bool fieldVisible = TableSetColumnIndex(1);
        if (!labelVisible && !fieldVisible)
        {
            ClayEnsureTableRowContributes(ImGui::GetCurrentTable());
        }
        m_LastFieldWidth = ImMax(1.0f, GetContentRegionAvail().x);
        m_InRow = true;
        return true;
    }

    void ClayInspectorContentScope::EndRow()
    {
        if (!m_InRow)
            return;
        m_InRow = false;
    }

    float ClayInspectorContentScope::FieldWidth() const
    {
        if (m_LastFieldWidth > 0.0f)
            return m_LastFieldWidth;
        return ImMax(1.0f, GetContentRegionAvail().x);
    }

    namespace
    {
        void PushFieldStyle()
        {
            ClayInspectorStyle& style = ClayInspectorGetStyle();
            PushStyleColor(ImGuiCol_FrameBg, style.FieldBg);
            PushStyleColor(ImGuiCol_FrameBgHovered, style.FieldBgHovered);
            PushStyleColor(ImGuiCol_FrameBgActive, style.FieldBgActive);
            PushStyleColor(ImGuiCol_Border, style.FieldBorder);
            PushStyleVar(ImGuiStyleVar_FrameRounding, style.FieldRounding);
            PushStyleVar(ImGuiStyleVar_FrameBorderSize, style.FieldBorderThickness);
        }

        void PopFieldStyle()
        {
            PopStyleVar(2);
            PopStyleColor(4);
        }
    } // namespace

    bool ClayFieldFloat(ClayInspectorContentScope& scope, const char* label, float* value, float speed, float min, float max, const char* format)
    {
        if (!scope.BeginRow(label))
            return false;
        PushID(label);
        PushFieldStyle();
        const float width = ImMax(1.0f, scope.FieldWidth());
        SetNextItemWidth(width);
        bool clamp = min < max;
        bool changed = DragFloat("##value", value, speed, clamp ? min : 0.0f, clamp ? max : 0.0f, format);
        PopFieldStyle();
        PopID();
        scope.EndRow();
        return changed;
    }

    bool ClayFieldInt(ClayInspectorContentScope& scope, const char* label, int* value, float speed, int min, int max, const char* format)
    {
        if (!scope.BeginRow(label))
            return false;
        PushID(label);
        PushFieldStyle();
        const float width = ImMax(1.0f, scope.FieldWidth());
        SetNextItemWidth(width);
        bool clamp = min < max;
        bool changed = DragInt("##value", value, speed, clamp ? min : 0, clamp ? max : 0, format);
        PopFieldStyle();
        PopID();
        scope.EndRow();
        return changed;
    }

    bool ClayFieldCheckbox(ClayInspectorContentScope& scope, const char* label, bool* value)
    {
        if (!scope.BeginRow(label))
            return false;
        PushID(label);
        bool changed = Checkbox("##value", value);
        PopID();
        scope.EndRow();
        return changed;
    }

    bool ClayFieldDropdown(ClayInspectorContentScope& scope, const char* label, const char* preview, const ClayDropdownBuilder& builder)
    {
        if (!scope.BeginRow(label))
            return false;
        PushID(label);
        PushFieldStyle();
        bool changed = false;
        ClayInspectorStyle& style = ClayInspectorGetStyle();
        const float width = ImMax(1.0f, scope.FieldWidth());
        const bool hasPreview = preview && preview[0] != '\0';
        const char* previewLabel = hasPreview ? preview : "Unassigned";
        const ImVec4 dropdownButton = ImLerp(style.FieldBg, style.FieldBgActive, 0.28f);
        const ImVec4 dropdownButtonHovered = ImLerp(style.FieldBgHovered, style.FieldBgActive, 0.36f);
        const ImVec4 textColor = hasPreview ? GetStyleColorVec4(ImGuiCol_Text) : style.SecondaryText;

        PushStyleColor(ImGuiCol_Button, dropdownButton);
        PushStyleColor(ImGuiCol_ButtonHovered, dropdownButtonHovered);
        PushStyleColor(ImGuiCol_ButtonActive, dropdownButtonHovered);
        PushStyleColor(ImGuiCol_Text, textColor);
        PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
        SetNextItemWidth(width);
        const bool open = BeginCombo("##dropdown", previewLabel, ImGuiComboFlags_HeightLargest);
        PopStyleVar();
        PopStyleColor(4);
        if (open)
        {
            if (builder)
                changed = builder();
            EndCombo();
        }
        PopFieldStyle();
        PopID();
        scope.EndRow();
        return changed;
    }

    bool ClayFieldVec(ClayInspectorContentScope& scope, const char* label, float* values, int components, float speed)
    {
        if (!scope.BeginRow(label))
            return false;
        PushID(label);
        bool changed = false;
        ClayInspectorStyle& style = ClayInspectorGetStyle();
        const float fullWidth = ImMax(1.0f, scope.FieldWidth());
        const float spacing = ImMax(3.0f, style.FieldSpacing - 1.0f);
        const float perWidth = (fullWidth - spacing * (components - 1)) / components;
        const ImVec4 axisColors[4] = {
            ImVec4(0.71f, 0.33f, 0.31f, 1.0f),
            ImVec4(0.35f, 0.61f, 0.39f, 1.0f),
            ImVec4(0.36f, 0.51f, 0.72f, 1.0f),
            ImVec4(0.67f, 0.61f, 0.34f, 1.0f)
        };
        const char* axisLabels[4] = { "X", "Y", "Z", "W" };

        for (int i = 0; i < components; ++i)
        {
            if (i > 0)
                SameLine(0.0f, spacing);
            PushID(i);
            PushFieldStyle();
            ImGuiStyle& imguiStyle = GetStyle();
            ImVec2 padding = imguiStyle.FramePadding;
            padding.x = ImMax(4.0f, padding.x - 1.0f) + style.VectorLabelWidth;
            padding.y = ImMax(2.0f, padding.y - 1.0f);
            PushStyleVar(ImGuiStyleVar_FramePadding, padding);
            SetNextItemWidth(perWidth);
            bool componentChanged = DragFloat("##component", &values[i], speed);
            PopStyleVar();
            ImRect rect(GetItemRectMin(), GetItemRectMax());
            PopFieldStyle();
            ImDrawList* dl = GetWindowDrawList();
            ImRect labelRect(rect.Min, ImVec2(rect.Min.x + style.VectorLabelWidth, rect.Max.y));
            dl->AddRectFilled(labelRect.Min, labelRect.Max, ColorConvertFloat4ToU32(axisColors[i]), style.FieldRounding, ImDrawFlags_RoundCornersLeft);
            const ImVec2 axisTextSize = CalcTextSize(axisLabels[i]);
            const ImVec2 axisTextPos(
                labelRect.Min.x + (labelRect.GetWidth() - axisTextSize.x) * 0.5f,
                labelRect.Min.y + (labelRect.GetHeight() - axisTextSize.y) * 0.5f - 0.5f);
            dl->AddText(axisTextPos,
                        ColorConvertFloat4ToU32(ImVec4(0.95f, 0.96f, 0.98f, 1.0f)),
                        axisLabels[i]);
            dl->AddRect(rect.Min, rect.Max, GetColorU32(ImGuiCol_Border), style.FieldRounding, ImDrawFlags_None, style.FieldBorderThickness);
            if (componentChanged)
                changed = true;
            PopID();
        }

        PopID();
        scope.EndRow();
        return changed;
    }

    bool ClayFieldSlider(ClayInspectorContentScope& scope, const char* label, float* value, float min, float max, const char* format)
    {
        if (!scope.BeginRow(label))
            return false;
        PushID(label);
        const float fullWidth = ImMax(1.0f, scope.FieldWidth());
        const float numberWidth = ClayInspectorGetStyle().SliderNumberWidth;
        const float sliderWidth = ImMax(1.0f, fullWidth - numberWidth - ClayInspectorGetStyle().FieldSpacing);

        PushFieldStyle();
        SetNextItemWidth(sliderWidth);
        bool changed = SliderFloat("##slider", value, min, max, format);
        PopFieldStyle();

        SameLine(0.0f, ClayInspectorGetStyle().FieldSpacing);
        PushFieldStyle();
        SetNextItemWidth(numberWidth);
        changed |= InputFloat("##slider_value", value, 0.0f, 0.0f, format, ImGuiInputTextFlags_CharsScientific);
        PopFieldStyle();

        PopID();
        scope.EndRow();
        return changed;
    }
} // namespace ImGui


