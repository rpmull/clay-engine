#pragma once

#include <imgui.h>
#include <imgui_claymore_style.h>

#include "editor/Project.h"

namespace editorui {

ImVec4 ToImVec4(const ColorRGBA& color);
ColorRGBA ToColorRGBA(const ImVec4& color);
ClayEditorPalette BuildEditorPalette(const EditorCustomPalette& palette);
ClayEditorPalette BuildProjectEditorPalette();
void ApplyEditorScheme(EditorColorScheme scheme, float baseFontSize);
void ApplyProjectEditorStyle(float baseFontSize);

} // namespace editorui
