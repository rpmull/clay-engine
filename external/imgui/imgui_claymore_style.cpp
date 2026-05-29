#include "imgui_claymore_style.h"
#include "imgui_clay_inspector.h"
#include <algorithm>

namespace
{
    ClayEditorTheme g_EditorTheme{};

    inline ImVec4 LerpColor(const ImVec4& a, const ImVec4& b, float t)
    {
        return ImVec4(
            a.x + (b.x - a.x) * t,
            a.y + (b.y - a.y) * t,
            a.z + (b.z - a.z) * t,
            a.w + (b.w - a.w) * t
        );
    }

    inline ImVec4 WithAlpha(const ImVec4& color, float alpha)
    {
        return ImVec4(color.x, color.y, color.z, alpha);
    }

    ClayEditorPalette BuildPaletteFromCurrentStyle(const ImGuiStyle& style)
    {
        const ImVec4* colors = style.Colors;

        ClayEditorPalette palette{};
        palette.Accent = colors[ImGuiCol_TabSelectedOverline];
        palette.AccentSoft = colors[ImGuiCol_SliderGrab];
        palette.AccentMuted = colors[ImGuiCol_Header];
        palette.Background00 = colors[ImGuiCol_TitleBg];
        palette.Background0 = colors[ImGuiCol_WindowBg];
        palette.Background1 = colors[ImGuiCol_ChildBg];
        palette.Background2 = colors[ImGuiCol_FrameBg];
        palette.Background3 = colors[ImGuiCol_TableHeaderBg];
        palette.Text = colors[ImGuiCol_Text];
        palette.TextDim = colors[ImGuiCol_TextDisabled];
        palette.Warning = colors[ImGuiCol_PlotHistogram];
        return palette;
    }

    void RebuildEditorTheme(const ClayEditorPalette* paletteOverride)
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const ImVec4* colors = style.Colors;
        const ClayEditorPalette palette = paletteOverride ? *paletteOverride : BuildPaletteFromCurrentStyle(style);

        g_EditorTheme.Palette = palette;
        g_EditorTheme.SurfaceSidebar = LerpColor(palette.Background00, palette.Background0, 0.58f);
        g_EditorTheme.SurfaceInset = LerpColor(palette.Background00, palette.Background1, 0.14f);
        g_EditorTheme.SurfaceBase = LerpColor(palette.Background0, palette.Background1, 0.48f);
        g_EditorTheme.SurfaceRaised = LerpColor(palette.Background1, palette.Background2, 0.62f);
        g_EditorTheme.SurfaceRaisedAlt = LerpColor(palette.Background2, palette.Background3, 0.28f);
        g_EditorTheme.SurfacePopup = colors[ImGuiCol_PopupBg];
        g_EditorTheme.SurfaceTitle = colors[ImGuiCol_TitleBgActive];
        g_EditorTheme.HeaderBg = colors[ImGuiCol_Header];
        g_EditorTheme.HeaderBgHovered = colors[ImGuiCol_HeaderHovered];
        g_EditorTheme.HeaderBgActive = colors[ImGuiCol_HeaderActive];
        g_EditorTheme.BorderSubtle = WithAlpha(colors[ImGuiCol_Border], 0.44f);
        g_EditorTheme.BorderStrong = WithAlpha(LerpColor(colors[ImGuiCol_Border], palette.TextDim, 0.10f), 0.88f);
        g_EditorTheme.BorderFocus = WithAlpha(palette.AccentSoft, 0.86f);
        g_EditorTheme.Text = palette.Text;
        g_EditorTheme.TextMuted = palette.TextDim;
        g_EditorTheme.SelectionFill = LerpColor(colors[ImGuiCol_HeaderActive], palette.AccentSoft, 0.08f);
        g_EditorTheme.SelectionHover = LerpColor(colors[ImGuiCol_HeaderHovered], palette.AccentSoft, 0.04f);
        g_EditorTheme.SelectionOutline = WithAlpha(palette.AccentSoft, 0.72f);
        g_EditorTheme.SelectionAccent = palette.AccentSoft;
        g_EditorTheme.TreeLine = colors[ImGuiCol_TreeLines];
        g_EditorTheme.DragTarget = colors[ImGuiCol_DragDropTarget];
        g_EditorTheme.PropertyAxisX = ImVec4(0.86f, 0.36f, 0.34f, 1.0f);
        g_EditorTheme.PropertyAxisY = ImVec4(0.47f, 0.77f, 0.41f, 1.0f);
        g_EditorTheme.PropertyAxisZ = ImVec4(0.40f, 0.57f, 0.92f, 1.0f);
        g_EditorTheme.PropertyAxisW = ImVec4(0.82f, 0.66f, 0.35f, 1.0f);
        g_EditorTheme.PrefabNodeText = LerpColor(palette.AccentSoft, palette.Text, 0.34f);
        g_EditorTheme.ModelNodeText = ImVec4(0.72f, 0.61f, 0.91f, 1.0f);
        g_EditorTheme.Success = ImVec4(0.42f, 0.78f, 0.47f, 1.0f);
        g_EditorTheme.Warning = palette.Warning;
        g_EditorTheme.Error = ImVec4(0.84f, 0.38f, 0.38f, 1.0f);
        g_EditorTheme.ModifiedPrefabText = LerpColor(palette.Warning, palette.Text, 0.08f);
        g_EditorTheme.AddedPrefabText = LerpColor(g_EditorTheme.Success, palette.Text, 0.08f);
    }
}

ClayEditorPalette Clay_DefaultEditorPalette()
{
    ClayEditorPalette p{};
    // Neutral editor graphite aimed closer to Unity/Unreal than "styled ImGui".
    p.Accent = ImVec4(0.36f, 0.47f, 0.60f, 1.00f);
    p.AccentSoft = ImVec4(0.44f, 0.53f, 0.65f, 1.00f);
    p.AccentMuted = ImVec4(0.29f, 0.31f, 0.34f, 1.00f);
    p.Background00 = ImVec4(0.15f, 0.15f, 0.16f, 1.00f);
    p.Background0 = ImVec4(0.18f, 0.18f, 0.19f, 1.00f);
    p.Background1 = ImVec4(0.21f, 0.215f, 0.22f, 1.00f);
    p.Background2 = ImVec4(0.24f, 0.245f, 0.252f, 1.00f);
    p.Background3 = ImVec4(0.30f, 0.305f, 0.315f, 1.00f);
    p.Text = ImVec4(0.90f, 0.90f, 0.91f, 1.00f);
    p.TextDim = ImVec4(0.61f, 0.63f, 0.66f, 1.00f);
    p.Warning = ImVec4(0.90f, 0.60f, 0.29f, 1.00f);
    return p;
}

ClayEditorPalette Clay_SolarizedEditorPalette()
{
    // Legacy blue-toned Claymore aesthetic.
    ClayEditorPalette p{};
    p.Accent = ImVec4(0.30f, 0.58f, 0.98f, 1.00f);
    p.AccentSoft = ImVec4(0.22f, 0.45f, 0.82f, 1.00f);
    p.AccentMuted = ImVec4(0.16f, 0.31f, 0.55f, 1.00f);
    p.Background00 = ImVec4(0.08f, 0.09f, 0.11f, 1.00f);
    p.Background0 = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    p.Background1 = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
    p.Background2 = ImVec4(0.17f, 0.18f, 0.22f, 1.00f);
    p.Background3 = ImVec4(0.24f, 0.25f, 0.30f, 1.00f);
    p.Text = ImVec4(0.93f, 0.94f, 0.96f, 1.00f);
    p.TextDim = ImVec4(0.60f, 0.64f, 0.70f, 1.00f);
    p.Warning = ImVec4(0.96f, 0.74f, 0.35f, 1.00f);
    return p;
}

ClayEditorPalette Clay_MedievalEditorPalette()
{
    ClayEditorPalette p{};
    // Worn papyrus palette.
    p.Accent = ImVec4(0.47f, 0.34f, 0.17f, 1.00f);
    p.AccentSoft = ImVec4(0.60f, 0.48f, 0.30f, 1.00f);
    p.AccentMuted = ImVec4(0.41f, 0.30f, 0.18f, 1.00f);
    p.Background00 = ImVec4(0.80f, 0.73f, 0.58f, 1.00f);
    p.Background0 = ImVec4(0.90f, 0.84f, 0.70f, 1.00f);
    p.Background1 = ImVec4(0.94f, 0.89f, 0.76f, 1.00f);
    p.Background2 = ImVec4(0.86f, 0.79f, 0.64f, 1.00f);
    p.Background3 = ImVec4(0.76f, 0.69f, 0.54f, 1.00f);
    p.Text = ImVec4(0.22f, 0.17f, 0.10f, 1.00f);
    p.TextDim = ImVec4(0.39f, 0.31f, 0.21f, 1.00f);
    p.Warning = ImVec4(0.60f, 0.20f, 0.13f, 1.00f);
    return p;
}

const ClayEditorTheme& Clay_GetEditorTheme()
{
    return g_EditorTheme;
}

void Clay_SyncInspectorStyleFromImGui(float baseFontSize)
{
    ImGuiStyle& style = ImGui::GetStyle();
    const ImVec4* colors = style.Colors;
    const float fontSize = std::max(baseFontSize, 12.0f);
    const float rowHeight = fontSize + style.FramePadding.y * 2.0f + 1.0f;

    ImGui::ClayInspectorStyle& insp = ImGui::ClayInspectorGetStyle();
    insp.HeaderHeight = rowHeight;
    insp.HeaderRounding = 4.0f;
    insp.HeaderBorderThickness = 1.0f;
    insp.HeaderPaddingX = 8.0f;
    insp.HeaderArrowLabelSpacing = 7.0f;
    insp.HeaderCheckboxSize = 15.0f;
    insp.HeaderArrowSize = 10.0f;
    insp.HeaderSpacing = 3.0f;
    insp.LabelColumnWidth = 148.0f;
    insp.RowMinHeight = rowHeight - 1.0f;
    insp.FieldSpacing = 5.0f;
    insp.VectorLabelWidth = 14.0f;
    insp.SliderNumberWidth = 70.0f;
    insp.FieldRounding = 4.0f;
    insp.FieldBorderThickness = 1.0f;

    insp.HeaderBg = colors[ImGuiCol_Header];
    insp.HeaderBgHovered = colors[ImGuiCol_HeaderHovered];
    insp.HeaderBgActive = colors[ImGuiCol_HeaderActive];
    insp.HeaderBorder = colors[ImGuiCol_Border];
    insp.HeaderText = colors[ImGuiCol_Text];
    insp.HeaderTextDisabled = colors[ImGuiCol_TextDisabled];
    insp.FieldBg = colors[ImGuiCol_FrameBg];
    insp.FieldBgHovered = colors[ImGuiCol_FrameBgHovered];
    insp.FieldBgActive = colors[ImGuiCol_FrameBgActive];
    insp.FieldBorder = colors[ImGuiCol_Border];
    insp.LabelColor = colors[ImGuiCol_TextDisabled];
    insp.SecondaryText = colors[ImGuiCol_TextDisabled];
}

void Clay_ApplyEditorMetrics(float baseFontSize)
{
    ImGuiStyle& style = ImGui::GetStyle();
    const float fontSize = std::max(baseFontSize, 12.0f);

    style.Alpha = 1.0f;
    style.DisabledAlpha = 0.62f;
    style.WindowPadding = ImVec2(10.0f, 8.0f);
    style.WindowMinSize = ImVec2(150.0f, 96.0f);
    style.FramePadding = ImVec2(7.0f, 4.0f);
    style.ItemSpacing = ImVec2(7.0f, 5.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.CellPadding = ImVec2(7.0f, 4.0f);
    style.TouchExtraPadding = ImVec2(0.0f, 0.0f);
    style.IndentSpacing = fontSize + 10.0f;
    style.ColumnsMinSpacing = 8.0f;
    style.ScrollbarSize = 11.0f;
    style.ScrollbarRounding = 12.0f;
    style.ScrollbarPadding = 1.0f;
    style.GrabMinSize = 11.0f;
    style.GrabRounding = 10.0f;
    style.LogSliderDeadzone = 4.0f;
    style.ImageBorderSize = 0.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.WindowRounding = 6.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 5.0f;
    style.TabRounding = 4.0f;
    style.TabBorderSize = 0.0f;
    style.TabMinWidthBase = 76.0f;
    style.TabMinWidthShrink = 58.0f;
    style.TabBarBorderSize = 0.0f;
    style.TabBarOverlineSize = 2.0f;
    style.TreeLinesFlags = ImGuiTreeNodeFlags_DrawLinesToNodes;
    style.TreeLinesSize = 1.0f;
    style.TreeLinesRounding = 6.0f;
    style.DragDropTargetRounding = 8.0f;
    style.DragDropTargetBorderSize = 1.5f;
    style.DragDropTargetPadding = 4.0f;
    style.WindowTitleAlign = ImVec2(0.04f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Left;
    style.ColorButtonPosition = ImGuiDir_Right;
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign = ImVec2(0.0f, 0.5f);
    style.SeparatorTextBorderSize = 1.0f;
    style.SeparatorTextAlign = ImVec2(0.0f, 0.5f);
    style.SeparatorTextPadding = ImVec2(6.0f, style.FramePadding.y + 1.0f);
    style.DisplayWindowPadding = ImVec2(14.0f, 14.0f);
    style.DisplaySafeAreaPadding = ImVec2(4.0f, 4.0f);
    style.DockingSeparatorSize = 1.35f;
    style.AntiAliasedLines = true;
    style.AntiAliasedLinesUseTex = true;
    style.AntiAliasedFill = true;

    const float rowHeight = fontSize + style.FramePadding.y * 2.0f + 1.0f;
    style.TreeNodeRowHeight = rowHeight;

    Clay_SyncInspectorStyleFromImGui(baseFontSize);
    Clay_ApplySubtleResizeGrips(1.0f);
    RebuildEditorTheme(nullptr);
}

void Clay_ApplySubtleResizeGrips(float intensity)
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;
    const float t = (intensity < 0.0f) ? 0.0f : ((intensity > 1.0f) ? 1.0f : intensity);
    const ImVec4 base = colors[ImGuiCol_WindowBg];
    const ImVec4 border = colors[ImGuiCol_Border];
    colors[ImGuiCol_ResizeGrip] = LerpColor(base, border, 0.14f * t);
    colors[ImGuiCol_ResizeGripHovered] = LerpColor(base, border, 0.22f * t);
    colors[ImGuiCol_ResizeGripActive] = LerpColor(base, border, 0.32f * t);
    colors[ImGuiCol_ResizeGrip].w *= 0.22f;
    colors[ImGuiCol_ResizeGripHovered].w *= 0.35f;
    colors[ImGuiCol_ResizeGripActive].w *= 0.50f;

    // Dock split bars use separator colors; keep them near-transparent by default.
    colors[ImGuiCol_Separator] = LerpColor(base, border, 0.16f * t);
    colors[ImGuiCol_SeparatorHovered] = LerpColor(base, border, 0.24f * t);
    colors[ImGuiCol_SeparatorActive] = LerpColor(base, border, 0.34f * t);
    colors[ImGuiCol_Separator].w *= 0.20f;
    colors[ImGuiCol_SeparatorHovered].w *= 0.33f;
    colors[ImGuiCol_SeparatorActive].w *= 0.48f;
}

void Clay_ApplyEditorStyle(float baseFontSize, const ClayEditorPalette* paletteOverride)
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    const ClayEditorPalette palette = paletteOverride ? *paletteOverride : Clay_DefaultEditorPalette();
    const ImVec4 accent      = palette.Accent;
    const ImVec4 accentSoft  = palette.AccentSoft;
    const ImVec4 accentMuted = palette.AccentMuted;
    const ImVec4 bg00        = palette.Background00;
    const ImVec4 bg0         = palette.Background0;
    const ImVec4 bg1         = palette.Background1;
    const ImVec4 bg2         = palette.Background2;
    const ImVec4 bg3         = palette.Background3;
    const ImVec4 text        = palette.Text;
    const ImVec4 textDim     = palette.TextDim;
    const ImVec4 warning     = palette.Warning;

    colors[ImGuiCol_Text]                 = text;
    colors[ImGuiCol_TextDisabled]         = textDim;
    colors[ImGuiCol_WindowBg]             = bg0;
    colors[ImGuiCol_ChildBg]              = LerpColor(bg0, bg1, 0.35f);
    colors[ImGuiCol_PopupBg]              = WithAlpha(LerpColor(bg00, bg1, 0.35f), 0.985f);
    colors[ImGuiCol_Border]               = WithAlpha(LerpColor(bg3, text, 0.12f), 0.78f);
    colors[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]              = LerpColor(bg1, bg2, 0.68f);
    colors[ImGuiCol_FrameBgHovered]       = LerpColor(bg2, accentSoft, 0.10f);
    colors[ImGuiCol_FrameBgActive]        = LerpColor(bg2, accent, 0.14f);
    colors[ImGuiCol_TitleBg]              = LerpColor(bg00, bg1, 0.24f);
    colors[ImGuiCol_TitleBgActive]        = LerpColor(bg00, bg1, 0.36f);
    colors[ImGuiCol_TitleBgCollapsed]     = LerpColor(bg00, bg1, 0.18f);
    colors[ImGuiCol_MenuBarBg]            = LerpColor(bg00, bg1, 0.38f);
    colors[ImGuiCol_ScrollbarBg]          = LerpColor(bg00, bg0, 0.65f);
    colors[ImGuiCol_ScrollbarGrab]        = LerpColor(bg2, bg3, 0.55f);
    colors[ImGuiCol_ScrollbarGrabHovered] = LerpColor(bg3, accentSoft, 0.08f);
    colors[ImGuiCol_ScrollbarGrabActive]  = LerpColor(bg3, accent, 0.12f);
    colors[ImGuiCol_CheckMark]            = accent;
    colors[ImGuiCol_SliderGrab]           = accentSoft;
    colors[ImGuiCol_SliderGrabActive]     = accent;
    colors[ImGuiCol_Button]               = LerpColor(bg1, bg2, 0.74f);
    colors[ImGuiCol_ButtonHovered]        = LerpColor(bg2, accentSoft, 0.10f);
    colors[ImGuiCol_ButtonActive]         = LerpColor(bg2, accent, 0.14f);
    colors[ImGuiCol_Header]               = LerpColor(bg1, accentMuted, 0.12f);
    colors[ImGuiCol_HeaderHovered]        = LerpColor(bg2, accentSoft, 0.10f);
    colors[ImGuiCol_HeaderActive]         = LerpColor(bg2, accent, 0.14f);
    colors[ImGuiCol_Separator]            = WithAlpha(LerpColor(colors[ImGuiCol_Border], textDim, 0.16f), 0.28f);
    colors[ImGuiCol_SeparatorHovered]     = WithAlpha(LerpColor(accentSoft, text, 0.18f), 0.56f);
    colors[ImGuiCol_SeparatorActive]      = WithAlpha(accent, 0.72f);
    colors[ImGuiCol_InputTextCursor]      = text;
    colors[ImGuiCol_Tab]                  = LerpColor(bg0, bg2, 0.52f);
    colors[ImGuiCol_TabHovered]           = LerpColor(bg2, accentSoft, 0.10f);
    colors[ImGuiCol_TabSelected]          = LerpColor(bg2, accentSoft, 0.08f);
    colors[ImGuiCol_TabSelectedOverline]  = accentSoft;
    colors[ImGuiCol_TabDimmed]            = LerpColor(bg00, bg1, 0.50f);
    colors[ImGuiCol_TabDimmedSelected]    = LerpColor(bg1, accentMuted, 0.10f);
    colors[ImGuiCol_TabDimmedSelectedOverline] = WithAlpha(accentSoft, 0.58f);
    colors[ImGuiCol_DockingPreview]       = WithAlpha(accent, 0.28f);
    colors[ImGuiCol_DockingEmptyBg]       = LerpColor(bg00, bg1, 0.12f);
    colors[ImGuiCol_PlotLines]            = accentSoft;
    colors[ImGuiCol_PlotLinesHovered]     = accent;
    colors[ImGuiCol_PlotHistogram]        = warning;
    colors[ImGuiCol_PlotHistogramHovered] = LerpColor(warning, text, 0.18f);
    colors[ImGuiCol_TableHeaderBg]        = LerpColor(bg1, bg2, 0.70f);
    colors[ImGuiCol_TableBorderStrong]    = WithAlpha(colors[ImGuiCol_Border], 0.92f);
    colors[ImGuiCol_TableBorderLight]     = WithAlpha(LerpColor(bg2, textDim, 0.10f), 0.32f);
    colors[ImGuiCol_TableRowBg]           = WithAlpha(bg1, 0.00f);
    colors[ImGuiCol_TableRowBgAlt]        = WithAlpha(bg3, 0.08f);
    colors[ImGuiCol_ResizeGrip]           = WithAlpha(LerpColor(bg2, accent, 0.10f), 0.12f);
    colors[ImGuiCol_ResizeGripHovered]    = WithAlpha(LerpColor(bg2, accent, 0.18f), 0.22f);
    colors[ImGuiCol_ResizeGripActive]     = WithAlpha(LerpColor(bg2, accent, 0.28f), 0.32f);
    colors[ImGuiCol_TextLink]             = accent;
    colors[ImGuiCol_TextSelectedBg]       = WithAlpha(accent, 0.32f);
    colors[ImGuiCol_TreeLines]            = WithAlpha(LerpColor(bg3, textDim, 0.18f), 0.24f);
    colors[ImGuiCol_DragDropTarget]       = warning;
    colors[ImGuiCol_DragDropTargetBg]     = WithAlpha(warning, 0.18f);
    colors[ImGuiCol_UnsavedMarker]        = warning;
    colors[ImGuiCol_NavCursor]            = WithAlpha(accent, 0.94f);
    colors[ImGuiCol_NavWindowingHighlight]= WithAlpha(text, 0.78f);
    colors[ImGuiCol_NavWindowingDimBg]    = WithAlpha(bg00, 0.42f);
    colors[ImGuiCol_ModalWindowDimBg]     = WithAlpha(bg00, 0.58f);

    Clay_ApplyEditorMetrics(baseFontSize);

    ImGui::ClayInspectorStyle& insp = ImGui::ClayInspectorGetStyle();
    insp.HeaderBg = LerpColor(bg1, bg2, 0.66f);
    insp.HeaderBgHovered = LerpColor(bg2, accentSoft, 0.10f);
    insp.HeaderBgActive = LerpColor(bg2, accent, 0.14f);
    insp.HeaderBorder = colors[ImGuiCol_Border];
    insp.HeaderText = colors[ImGuiCol_Text];
    insp.HeaderTextDisabled = textDim;
    insp.FieldBg = LerpColor(colors[ImGuiCol_FrameBg], bg00, 0.14f);
    insp.FieldBgHovered = colors[ImGuiCol_FrameBgHovered];
    insp.FieldBgActive = colors[ImGuiCol_FrameBgActive];
    insp.FieldBorder = colors[ImGuiCol_Border];
    insp.LabelColor = colors[ImGuiCol_TextDisabled];
    insp.SecondaryText = textDim;
    RebuildEditorTheme(&palette);
}

