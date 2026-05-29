#pragma once
#include <string>
#include <vector>
#include <imgui.h>
#include <bgfx/bgfx.h>
#include "editor/ui/panels/EditorPanel.h"

// Simple editor window to draw exclusion polygons for an image tint mask.
// Saves a sidecar JSON next to the image with extension ".tintmask".
class TintMaskEditorWindow : public EditorPanel {
public:
    TintMaskEditorWindow() = default;
    ~TintMaskEditorWindow() { UnloadTexture(); }

    void OpenForImage(const std::string& imagePath);
    void Close() { m_Open = false; }
    bool IsOpen() const { return m_Open; }

    void OnImGuiRender();

private:
    void EnsureTextureLoaded();
    void UnloadTexture();
    void DrawImageAndCaptureClicks();
    void DrawOverlay();
    void SaveMaskFiles();
    void LoadMaskJSONIfExists();

    std::string GetMaskBmpPath() const;
    std::string GetMaskJsonPath() const;

private:
    bool m_Open = false;
    std::string m_ImagePath;
    int m_ImageWidth = 0;
    int m_ImageHeight = 0;
    bgfx::TextureHandle m_ImageTexture = BGFX_INVALID_HANDLE;

    // Polygons in image pixel coordinates
    std::vector<std::vector<ImVec2>> m_Polygons;
    std::vector<ImVec2> m_CurrentPoly;

    // View state
    float m_Zoom = 1.0f;
    ImVec2 m_Scroll{0,0};
    ImVec2 m_LastImgMin{0,0};
    ImVec2 m_LastImgMax{0,0};
};


