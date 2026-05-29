#pragma once
#include <string>
#include <memory>
#include <bgfx/bgfx.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif

// Forward declarations
struct HWND__;
typedef HWND__* HWND;
class Win32Window;
class Scene;
class JobSystem;
class RuntimeAssetResolver;

// Game cursor settings (loaded from manifest)
struct RuntimeCursorSettings {
    std::string texturePath;
    float baseScale = 1.0f;
    int hotspotX = 0;
    int hotspotY = 0;
    bool useDPIScaling = true;
};

enum class RuntimeWindowMode {
    Windowed,
    Fullscreen
};

struct RuntimeLaunchOptions {
    int width = 1920;
    int height = 1080;
    std::string title = "Claymore Game";
    RuntimeWindowMode windowMode = RuntimeWindowMode::Windowed;
    std::string contentRoot;
    std::string pakPath;
};

// Minimal runtime application - no editor, no ImGui, no asset pipeline
// This is what exported games use
class RuntimeApplication {
public:
    RuntimeApplication(const RuntimeLaunchOptions& options);
    RuntimeApplication(int width, int height, const std::string& title);
    ~RuntimeApplication();

    static RuntimeApplication& Get();
    static bool HasInstance() { return s_Instance != nullptr; }

    JobSystem& Jobs() { return *m_Jobs; }
    Scene* GetScene() const { return m_Scene.get(); }

    void Run();
    void RequestShutdown() { m_ShouldClose = true; }

    // Window access
    HWND GetWindowHandle() const { return m_Window; }
    Win32Window* GetWin32Window() const { return m_Win32Window.get(); }

private:
    static RuntimeApplication* s_Instance;

    void Init();
    void InitWindow(int width, int height, const std::string& title);
    void InitBgfx();
    void Shutdown();

    void LoadEntryScene();

    std::unique_ptr<JobSystem> m_Jobs;
    std::unique_ptr<Scene> m_Scene;
    std::shared_ptr<Scene> m_RuntimeScene;  // Clone for play mode
    std::unique_ptr<RuntimeAssetResolver> m_AssetResolver;

    HWND m_Window = nullptr;
    std::unique_ptr<Win32Window> m_Win32Window;
    
    int m_Width = 0;
    int m_Height = 0;
    bool m_ShouldClose = false;
    bool m_PendingResize = false;
    int m_PendingWidth = 0;
    int m_PendingHeight = 0;
    
    // Custom game cursor
    void LoadCursorSettings(const std::string& manifestText);
    void RenderCustomCursor();
    void DestroyCursor();
    
    RuntimeCursorSettings m_CursorSettings;
    bgfx::TextureHandle m_CursorTexture = BGFX_INVALID_HANDLE;
    int m_CursorTextureWidth = 0;
    int m_CursorTextureHeight = 0;
    bool m_UseCustomCursor = false;
    RuntimeLaunchOptions m_LaunchOptions;
};


