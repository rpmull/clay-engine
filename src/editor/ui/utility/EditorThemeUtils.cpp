#include "EditorThemeUtils.h"

namespace editorui {

ImVec4 ToImVec4(const ColorRGBA& color)
{
    return ImVec4(color.r, color.g, color.b, color.a);
}

ColorRGBA ToColorRGBA(const ImVec4& color)
{
    return ColorRGBA{color.x, color.y, color.z, color.w};
}

ClayEditorPalette BuildEditorPalette(const EditorCustomPalette& palette)
{
    ClayEditorPalette out{};
    out.Accent = ToImVec4(palette.accent);
    out.AccentSoft = ToImVec4(palette.accentSoft);
    out.AccentMuted = ToImVec4(palette.accentMuted);
    out.Background00 = ToImVec4(palette.background00);
    out.Background0 = ToImVec4(palette.background0);
    out.Background1 = ToImVec4(palette.background1);
    out.Background2 = ToImVec4(palette.background2);
    out.Background3 = ToImVec4(palette.background3);
    out.Text = ToImVec4(palette.text);
    out.TextDim = ToImVec4(palette.textDim);
    out.Warning = ToImVec4(palette.warning);
    return out;
}

ClayEditorPalette BuildProjectEditorPalette()
{
    return BuildEditorPalette(Project::GetEditorCustomPalette());
}

void ApplyEditorScheme(EditorColorScheme scheme, float baseFontSize)
{
    switch (scheme) {
        case EditorColorScheme::Solarized: {
            ClayEditorPalette palette = Clay_SolarizedEditorPalette();
            Clay_ApplyEditorStyle(baseFontSize, &palette);
            break;
        }
        case EditorColorScheme::Dark:
            ImGui::StyleColorsDark();
            Clay_ApplyEditorMetrics(baseFontSize);
            break;
        case EditorColorScheme::Light:
            ImGui::StyleColorsLight();
            Clay_ApplyEditorMetrics(baseFontSize);
            break;
        case EditorColorScheme::Classic:
            ImGui::StyleColorsClassic();
            Clay_ApplyEditorMetrics(baseFontSize);
            break;
        case EditorColorScheme::Medieval: {
            ClayEditorPalette palette = Clay_MedievalEditorPalette();
            Clay_ApplyEditorStyle(baseFontSize, &palette);
            break;
        }
        case EditorColorScheme::Custom: {
            ClayEditorPalette palette = BuildProjectEditorPalette();
            Clay_ApplyEditorStyle(baseFontSize, &palette);
            break;
        }
        case EditorColorScheme::Claymore:
        default:
            Clay_ApplyEditorStyle(baseFontSize);
            break;
    }
}

void ApplyProjectEditorStyle(float baseFontSize)
{
    ApplyEditorScheme(Project::GetEditorColorScheme(), baseFontSize);
}

} // namespace editorui
