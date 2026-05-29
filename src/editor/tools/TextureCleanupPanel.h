#pragma once

#include "editor/ui/panels/EditorPanel.h"
#include "editor/pipeline/TextureCleanup.h"
#include <imgui.h>
#include <string>

class TextureCleanupPanel : public EditorPanel {
public:
    void OnImGuiRender();
    void Open() { m_Open = true; }
    void SetOpen(bool open) { m_Open = open; }
    bool IsOpen() const { return m_Open; }

private:
    bool m_Open = false;
    bool m_HasRun = false;
    bool m_LastOk = false;
    texture_cleanup::CleanupReport m_LastReport{};
};

