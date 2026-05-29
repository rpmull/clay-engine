#pragma once
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_claymore_style.h>
#include <cmath>
#include <string>

inline ImVec4 Clay_UI_Lerp(const ImVec4& a, const ImVec4& b, float t) {
	return ImVec4(
		a.x + (b.x - a.x) * t,
		a.y + (b.y - a.y) * t,
		a.z + (b.z - a.z) * t,
		a.w + (b.w - a.w) * t);
}

inline float Clay_UI_RowHeight() {
	const ImGuiStyle& style = ImGui::GetStyle();
	if (style.TreeNodeRowHeight > 0.0f)
		return style.TreeNodeRowHeight;
	return ImGui::GetFontSize() + style.FramePadding.y * 2.0f;
}

inline void Clay_UI_DrawRowBackground(ImDrawList* drawList, const ImRect& rect, bool hovered, bool selected) {
	if (!drawList) return;
	if (!hovered && !selected) return;
	const ClayEditorTheme& theme = Clay_GetEditorTheme();
	ImVec4 fill = selected ? theme.SelectionFill : theme.SelectionHover;
	fill.w *= selected ? 0.92f : 0.52f;
	const ImVec4 border = selected ? theme.SelectionOutline : theme.BorderSubtle;
	const ImVec4 accent = theme.SelectionAccent;
	ImRect bg = rect;
	bg.Min.x += 1.0f;
	bg.Max.x -= 1.0f;
	drawList->AddRectFilled(bg.Min, bg.Max, ImGui::GetColorU32(fill), 3.0f);
	if (selected) {
		drawList->AddRectFilled(ImVec2(bg.Min.x + 1.0f, bg.Min.y + 1.0f),
								ImVec2(bg.Min.x + 3.5f, bg.Max.y - 1.0f),
								ImGui::GetColorU32(ImVec4(accent.x, accent.y, accent.z, 0.95f)),
								2.0f);
		drawList->AddRect(bg.Min, bg.Max, ImGui::GetColorU32(ImVec4(border.x, border.y, border.z, 0.55f)), 3.0f);
	} else {
		drawList->AddRect(bg.Min, bg.Max, ImGui::GetColorU32(ImVec4(border.x, border.y, border.z, 0.24f)), 3.0f);
	}
}

inline void Clay_UI_DrawTileBackground(ImDrawList* drawList, const ImRect& rect, bool hovered, bool selected) {
	if (!drawList) return;
	if (!selected && !hovered) return;
	const ClayEditorTheme& theme = Clay_GetEditorTheme();
	const ImGuiStyle& style = ImGui::GetStyle();
	ImVec4 fill = selected ? theme.SelectionFill : Clay_UI_Lerp(theme.SurfaceInset, theme.SelectionHover, 0.56f);
	fill.w *= selected ? 0.84f : 0.48f;
	ImVec4 border = selected ? theme.SelectionOutline : theme.BorderSubtle;
	border.w *= selected ? 0.90f : 0.50f;
	const float rounding = ImMax(3.0f, style.FrameRounding - 1.0f);
	drawList->AddRectFilled(rect.Min, rect.Max, ImGui::GetColorU32(fill), rounding);
	drawList->AddRect(rect.Min, rect.Max, ImGui::GetColorU32(border), rounding);
	if (selected) {
		drawList->AddLine(ImVec2(rect.Min.x + 1.0f, rect.Max.y - 1.0f),
						  ImVec2(rect.Max.x - 1.0f, rect.Max.y - 1.0f),
						  ImGui::GetColorU32(theme.SelectionAccent),
						  1.0f);
	}
}

inline void Clay_UI_AlignTextToRowStart() {
	float row_h = Clay_UI_RowHeight();
	float font = ImGui::GetFontSize();
	float pad = (row_h - font) * 0.5f;
	ImGui::SetCursorPosY(std::floor(ImGui::GetCursorPosY() + pad));
	ImGui::AlignTextToFramePadding();
}

namespace uihelpers {

// Global toggle for the UI layout probe (inline for single definition across TU)
inline bool g_EnableUILayoutProbe = false;

// Authoritative per-row height helper
static inline float UI_GetRowHeight() {
	return Clay_UI_RowHeight();
}

// Vertically center the upcoming item of height box_h within current row height
static inline void UI_CenterCursorY(float box_h) {
	float y = ImGui::GetCursorPosY();
	float frame_h = Clay_UI_RowHeight();
	float pad = (frame_h - box_h) * 0.5f;
	if (pad > 0.0f) ImGui::SetCursorPosY(y + pad);
}

// Snap Y cursor to integer pixel and align text baseline to frame padding
static inline void UI_AlignToRowStart() {
	Clay_UI_AlignTextToRowStart();
}

// Right align next item within current content region
static inline void UI_RightAlignNextItem(float item_w) {
	float full = ImGui::GetContentRegionAvail().x;
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + full - item_w);
}

// Right-aligned square icon/text button hugging the column/window right edge
static inline bool UI_IconButtonRightAligned(const char* id, const char* text_or_icon) {
	float w = ImGui::GetFrameHeight();
	UI_RightAlignNextItem(w);
	UI_CenterCursorY(w);
	return ImGui::Button(id, ImVec2(w, w));
}

// Simple layout probe to visualize height mismatches against the canonical row height
struct UILayoutProbe {
	float row_h;
	float tolerance;
	UILayoutProbe(float desired_row_h, float tol = 1.0f) : row_h(desired_row_h), tolerance(tol) {}
	void CheckLastItem(const char* tag) {
		if (!g_EnableUILayoutProbe) return;
		ImVec2 min = ImGui::GetItemRectMin();
		ImVec2 max = ImGui::GetItemRectMax();
		float h = max.y - min.y;
		if (fabsf(h - row_h) > tolerance) {
			ImGui::SetNextWindowBgAlpha(0.85f);
			ImGui::BeginTooltip();
			ImGui::Text("Misaligned (%s): h=%.2f vs row_h=%.2f", tag, h, row_h);
			ImGui::EndTooltip();
			ImGui::GetForegroundDrawList()->AddRect(min, max, IM_COL32(255,80,80,255));
		}
	}
};

} // namespace uihelpers
