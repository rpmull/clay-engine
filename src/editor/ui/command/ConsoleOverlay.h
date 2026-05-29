#pragma once

#include <string>
#include <vector>
#include <imgui.h>

// A simple bottom-line console overlay rendered inside the scene viewport during play mode.
class CommandConsole {
public:
    static CommandConsole& Instance() { static CommandConsole c; return c; }

    // Render the console inside the viewport rectangle. Only active in play mode when requested.
    void RenderInViewport(const ImVec2& viewportTL, const ImVec2& viewportSize, bool isPlayMode);

    bool IsActive() const { return m_Active; }

private:
    CommandConsole() = default;

    bool m_Active = false;
    char m_Buffer[512]{};
    std::vector<std::string> m_History;
    int m_HistoryPos = -1;
};



