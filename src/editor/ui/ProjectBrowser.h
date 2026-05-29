#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include <memory>

class Win32Window;
struct ImGuiContext; // forward decl to avoid including imgui in header

struct RegisteredProject {
    std::string name;
    std::filesystem::path root;
};

class ProjectBrowserWindow {
public:
    ProjectBrowserWindow();
    ~ProjectBrowserWindow();

    // Returns empty path if user cancels/quits
    std::filesystem::path OpenModal();

private:
    void LoadRegistry();
    void SaveRegistry();
    bool CreateBrowserWindow();
    void DestroyBrowserWindow();
    void BeginFrame();
    void EndFrame();
    bool PumpOnce();
    void DrawUI();

    // Actions
    void ActionImportFromFilesystem();
    void ActionCreateNewProject();
    void ActionRemoveSelected();

private:
    std::unique_ptr<Win32Window> m_Window;
    void* m_Hwnd = nullptr;
    int m_Width = 900;
    int m_Height = 600;
    bool m_ShouldClose = false;

    std::vector<RegisteredProject> m_Projects;
    int m_SelectedIndex = -1;

    // registry path in %APPDATA%/Claymore/projects.json
    std::filesystem::path m_RegistryPath;
    // result
    std::filesystem::path m_SelectedProjectRoot;
    // Tracks whether the browser window created and owns an ImGui/bgfx context
    bool m_BrowserActive = false;
    // The ImGui context owned by the project browser modal
    ImGuiContext* m_ImGuiContext = nullptr;
};


