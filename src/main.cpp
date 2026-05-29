// Claymore Editor - Full development environment
// Uses ImGui, asset pipeline, project browser, etc.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#endif

#include "editor/application.h"
#include "editor/ui/ProjectBrowser.h"
#include "editor/Project.h"
#include "editor/pipeline/AssetPipeline.h"
#include "editor/tools/SerializerSanityWindow.h"
#include <filesystem>
#include <iostream>
#include <string>

namespace {

#ifdef _WIN32
#ifndef PROCESS_POWER_THROTTLING_CURRENT_VERSION
#define PROCESS_POWER_THROTTLING_CURRENT_VERSION 1
#endif

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 reinterpret_cast<DPI_AWARENESS_CONTEXT>(-4)
#endif

#ifndef PROCESS_POWER_THROTTLING_EXECUTION_SPEED
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED 0x1
typedef struct PROCESS_POWER_THROTTLING_STATE {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
} PROCESS_POWER_THROTTLING_STATE, *PPROCESS_POWER_THROTTLING_STATE;
#endif

#ifndef ProcessPowerThrottling
#define ProcessPowerThrottling static_cast<PROCESS_INFORMATION_CLASS>(4)
#endif

struct WindowsLaunchPolicyState {
    bool dpiAwarenessEnabled = false;
    bool priorityRaised = false;
    bool powerThrottlingDisabled = false;
};

WindowsLaunchPolicyState ApplyWindowsLaunchPolicy() {
    WindowsLaunchPolicyState state{};

    if (HMODULE user32 = ::GetModuleHandleW(L"user32.dll")) {
        using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        auto setDpiAwarenessContext = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
            ::GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setDpiAwarenessContext) {
            state.dpiAwarenessEnabled =
                !!setDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        } else {
            using SetProcessDPIAwareFn = BOOL(WINAPI*)();
            auto setProcessDPIAware = reinterpret_cast<SetProcessDPIAwareFn>(
                ::GetProcAddress(user32, "SetProcessDPIAware"));
            if (setProcessDPIAware) {
                state.dpiAwarenessEnabled = !!setProcessDPIAware();
            }
        }
    }

    state.priorityRaised = !!::SetPriorityClass(::GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    if (HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll")) {
        using SetProcessInformationFn = BOOL(WINAPI*)(HANDLE, PROCESS_INFORMATION_CLASS, LPVOID, DWORD);
        auto setProcessInformation = reinterpret_cast<SetProcessInformationFn>(
            ::GetProcAddress(kernel32, "SetProcessInformation"));
        if (setProcessInformation) {
            PROCESS_POWER_THROTTLING_STATE throttling{};
            throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
            throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
            throttling.StateMask = 0;
            state.powerThrottlingDisabled = !!setProcessInformation(
                ::GetCurrentProcess(),
                ProcessPowerThrottling,
                &throttling,
                sizeof(throttling));
        }
    }

    return state;
}
#endif

} // namespace

int main(int argc, char** argv) {
    std::cout << "[Claymore Editor] Starting..." << std::endl;
#ifdef _WIN32
    const WindowsLaunchPolicyState launchPolicy = ApplyWindowsLaunchPolicy();
    std::cout << "[Claymore Editor] Windows launch policy:"
              << " per-monitor-dpi=" << (launchPolicy.dpiAwarenessEnabled ? "enabled" : "unchanged")
              << " priority=" << (launchPolicy.priorityRaised ? "above-normal" : "default")
              << " power-throttling=" << (launchPolicy.powerThrottlingDisabled ? "disabled" : "default")
              << std::endl;
#endif
    bool runSanity = false;
    std::string projectArg;
    std::string sceneArg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--sanity") {
            runSanity = true;
        } else if (arg == "--project" && i + 1 < argc) {
            projectArg = argv[++i];
        } else if (arg == "--scene" && i + 1 < argc) {
            sceneArg = argv[++i];
        }
    }
    
    // Check if running as standalone game (pak file or marker exists next to exe)
    std::filesystem::path exeDir = std::filesystem::current_path();
    bool isStandaloneGame = std::filesystem::exists(exeDir / "game_mode_only.marker");
    
    // Also check for any .pak file
    if (!isStandaloneGame) {
        for (auto& entry : std::filesystem::directory_iterator(exeDir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".pak") {
                isStandaloneGame = true;
                break;
            }
        }
    }
    
    bool skipProjectBrowser = !projectArg.empty() || runSanity;

    // Skip project browser for standalone game builds or CLI-specified project
    if (!isStandaloneGame && !skipProjectBrowser) {
        // Launch project browser first (editor mode)
        ProjectBrowserWindow browser;
        
        std::filesystem::path chosen = browser.OpenModal();
        if (!chosen.empty()) {
            Project::SetProjectDirectory(chosen);
            // If a .clayproj exists, load it for name/assets path
            for (auto& e : std::filesystem::directory_iterator(chosen)) {
                if (e.path().extension() == ".clayproj") { 
                    Project::Load(e.path()); 
                    break; 
                }
            }
            // Ensure scripts are compiled for the selected project
            AssetPipeline::Instance().CheckAndCompileScriptsAtStartup();
        }
    } else if (!projectArg.empty()) {
        std::filesystem::path chosen = projectArg;
        Project::SetProjectDirectory(chosen);
        // If a .clayproj exists, load it for name/assets path
        for (auto& e : std::filesystem::directory_iterator(chosen)) {
            if (e.path().extension() == ".clayproj") { 
                Project::Load(e.path()); 
                break; 
            }
        }
        AssetPipeline::Instance().CheckAndCompileScriptsAtStartup();
    }

    if (runSanity) {
        if (projectArg.empty() || sceneArg.empty()) {
            std::cerr << "[SerializerSanity] Usage: --sanity --project <path> --scene <scene.sceneb|scene.scene>" << std::endl;
            return 2;
        }
        SerializerSanityWindow::AutoRunOptions opts;
        opts.enabled = true;
        opts.exitOnComplete = true;
        std::filesystem::path scenePath = sceneArg;
        if (!scenePath.is_absolute()) {
            scenePath = std::filesystem::path(projectArg) / scenePath;
        }
        opts.scenePath = scenePath.string();
        SerializerSanityWindow::ConfigureAutoRun(opts);
    }

    Application app(1920, 1080, "Claymore Engine");
    app.Run();
    
    std::cout << "[Claymore Editor] Exiting." << std::endl;
    return 0;
}
