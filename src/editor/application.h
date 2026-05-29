#pragma once
#include <string>
#include <filesystem>
#ifndef NOMINMAX
#define NOMINMAX
#endif

// Forward declarations
struct HWND__;
typedef HWND__* HWND;

#include <memory>
#include "core/jobs/JobSystem.h"
#include "core/ecs/Scene.h"

// Forward declarations for editor-only types
class Win32Window;
class UILayer;
class AssetWatcher;
class AssetPipeline;
class EditorAssetResolver;

class Application {
public:
    Application(int width, int height, const std::string& title); 
    ~Application();

    static Application& Get();
    static bool HasInstance() { return s_Instance != nullptr; }

    JobSystem& Jobs() { return *m_Jobs; }
    AssetWatcher* GetAssetWatcher() const { return m_AssetWatcher.get(); }
    AssetPipeline* GetAssetPipeline() const { return m_AssetPipeline.get(); }

    void Run();
    void SetMouseCaptured(bool captured);
    void SetEditModeMouseCaptureAllowed(bool allowed);
    bool IsEditModeMouseCaptureAllowed() const { return m_EditModeMouseCaptureAllowed; }
    bool IsGameViewportMouseInputAllowed() const;

    void StartPlayMode();
    void StopPlayMode();
    bool IsPlaying() const;
    
    // Window access for custom title bar
    HWND GetWindowHandle() const { return m_window; }
    Win32Window* GetWin32Window() const { return m_Win32Window.get(); }

    std::filesystem::path defaultProjPath;
    bool m_RunEditorUI = true;

private:
    static Application* s_Instance;

    std::unique_ptr<JobSystem> m_Jobs;

    void InitWindow(int width, int height, const std::string& title);
    void InitBgfx();
    void Shutdown();
    void InitImGui();

    std::shared_ptr<Scene> m_EditScene;
    std::shared_ptr<Scene> m_RuntimeScene;
    bool m_IsPlaying = false;
    bool m_EditModeMouseCaptureAllowed = false;
    bool m_ShutdownCompleted = false;

    HWND m_window = nullptr;
    int m_width = 0;
    int m_height = 0;
    
    // Deferred resize handling
    bool m_PendingResize = false;
    int m_PendingWidth = 0;
    int m_PendingHeight = 0;
    
    std::unique_ptr<UILayer> uiLayer;
    std::unique_ptr<AssetPipeline> m_AssetPipeline;
    std::unique_ptr<AssetWatcher> m_AssetWatcher;
    std::unique_ptr<EditorAssetResolver> m_EditorAssetResolver;

    std::unique_ptr<Win32Window> m_Win32Window;

    // Runtime without editor UI
    std::unique_ptr<Scene> m_GameScene;
};

