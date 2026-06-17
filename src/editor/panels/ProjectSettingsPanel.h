#pragma once
#include "editor/ui/panels/EditorPanel.h"
#include <string>
#include <bgfx/bgfx.h>

class ProjectSettingsPanel : public EditorPanel {
public:
    ProjectSettingsPanel() = default;
    void OnImGuiRender();
    void OnImGuiRenderEmbedded();
    
private:
    void SyncProjectNameDraft();
    void ApplyProjectNameChange();
    void DrawScriptingTab();
    void DrawModulesTab();
    void DrawPhysicsLayersTab();
    void DrawGameCursorTab();
    void DrawAppearanceTab();
    void DrawViewportTab();
    
    // Cursor preview texture handle
    bgfx::TextureHandle m_CursorPreviewTex = BGFX_INVALID_HANDLE;
    std::string m_CursorPreviewPath;
    char m_ProjectNameBuffer[256] = {};
    std::string m_ProjectNameBufferSource;
    std::string m_ProjectNameStatus;
    bool m_ProjectNameStatusIsError = false;
};

