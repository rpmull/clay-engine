#pragma once

#include "imgui.h"

struct ClayEditorPalette {
    ImVec4 Accent;
    ImVec4 AccentSoft;
    ImVec4 AccentMuted;
    ImVec4 Background00;
    ImVec4 Background0;
    ImVec4 Background1;
    ImVec4 Background2;
    ImVec4 Background3;
    ImVec4 Text;
    ImVec4 TextDim;
    ImVec4 Warning;
};

struct ClayEditorTheme {
    ClayEditorPalette Palette;
    ImVec4 SurfaceBase;
    ImVec4 SurfaceRaised;
    ImVec4 SurfaceRaisedAlt;
    ImVec4 SurfaceInset;
    ImVec4 SurfaceSidebar;
    ImVec4 SurfacePopup;
    ImVec4 SurfaceTitle;
    ImVec4 HeaderBg;
    ImVec4 HeaderBgHovered;
    ImVec4 HeaderBgActive;
    ImVec4 BorderSubtle;
    ImVec4 BorderStrong;
    ImVec4 BorderFocus;
    ImVec4 Text;
    ImVec4 TextMuted;
    ImVec4 SelectionFill;
    ImVec4 SelectionHover;
    ImVec4 SelectionOutline;
    ImVec4 SelectionAccent;
    ImVec4 TreeLine;
    ImVec4 DragTarget;
    ImVec4 PropertyAxisX;
    ImVec4 PropertyAxisY;
    ImVec4 PropertyAxisZ;
    ImVec4 PropertyAxisW;
    ImVec4 PrefabNodeText;
    ImVec4 ModelNodeText;
    ImVec4 ModifiedPrefabText;
    ImVec4 AddedPrefabText;
    ImVec4 Success;
    ImVec4 Warning;
    ImVec4 Error;
};

IMGUI_API ClayEditorPalette Clay_DefaultEditorPalette();
IMGUI_API ClayEditorPalette Clay_SolarizedEditorPalette();
IMGUI_API ClayEditorPalette Clay_MedievalEditorPalette();
IMGUI_API const ClayEditorTheme& Clay_GetEditorTheme();
IMGUI_API void Clay_ApplyEditorMetrics(float baseFontSize = 15.0f);
IMGUI_API void Clay_SyncInspectorStyleFromImGui(float baseFontSize = 15.0f);
IMGUI_API void Clay_ApplySubtleResizeGrips(float intensity = 0.35f);

// Applies the Claymore editor palette + layout metrics in one place.
// Pass the base font size before global scaling so row heights scale with DPI.
IMGUI_API void Clay_ApplyEditorStyle(float baseFontSize = 15.0f, const ClayEditorPalette* paletteOverride = nullptr);

