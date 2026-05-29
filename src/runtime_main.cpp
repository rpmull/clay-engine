// Claymore Runtime - Minimal game executable
// This is used for exported standalone games
// No editor, no ImGui, no asset pipeline, no Assimp

#include "core/RuntimeApplication.h"
#include <algorithm>
#include <iostream>
#include <fstream>
#include <filesystem>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

namespace {

RuntimeLaunchOptions ParseLaunchOptions() {
    RuntimeLaunchOptions options;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return options;
    }

    auto nextValue = [&](int& index) -> std::wstring {
        if (index + 1 >= argc) {
            return {};
        }
        ++index;
        return argv[index];
    };

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--pak") {
            options.pakPath = std::filesystem::path(nextValue(i)).string();
        } else if (arg == L"--content-root") {
            options.contentRoot = std::filesystem::path(nextValue(i)).string();
        } else if (arg == L"--title") {
            options.title = std::filesystem::path(nextValue(i)).string();
        } else if (arg == L"--windowed") {
            options.windowMode = RuntimeWindowMode::Windowed;
        } else if (arg == L"--fullscreen") {
            options.windowMode = RuntimeWindowMode::Fullscreen;
        } else if (arg == L"--width") {
            const std::wstring value = nextValue(i);
            if (!value.empty()) {
                options.width = std::max(1, _wtoi(value.c_str()));
            }
        } else if (arg == L"--height") {
            const std::wstring value = nextValue(i);
            if (!value.empty()) {
                options.height = std::max(1, _wtoi(value.c_str()));
            }
        }
    }

    LocalFree(argv);
    return options;
}

} // namespace

// Custom stream buffer that writes to both file and original stream
class TeeStreamBuf : public std::streambuf {
public:
    TeeStreamBuf(std::streambuf* original, std::ofstream& file) 
        : m_Original(original), m_File(file) {}
    
    std::streambuf* GetOriginal() const { return m_Original; }
    
protected:
    int overflow(int c) override {
        if (c != EOF) {
            if (m_Original) m_Original->sputc(c);
            if (m_File.is_open()) m_File.put(c);
        }
        return c;
    }
    
    int sync() override {
        if (m_Original) m_Original->pubsync();
        if (m_File.is_open()) m_File.flush();
        return 0;
    }
    
private:
    std::streambuf* m_Original;
    std::ofstream& m_File;
};

int main() {
    // Set up logging to file in the same directory as the executable
    std::ofstream logFile("runtime.log", std::ios::out | std::ios::trunc);
    RuntimeLaunchOptions launchOptions = ParseLaunchOptions();
    
    // Redirect cout and cerr to both console and file
    TeeStreamBuf coutTee(std::cout.rdbuf(), logFile);
    TeeStreamBuf cerrTee(std::cerr.rdbuf(), logFile);
    std::cout.rdbuf(&coutTee);
    std::cerr.rdbuf(&cerrTee);
    
    std::cout << "[Claymore Runtime] Starting..." << std::endl;
    std::cout << "[Claymore Runtime] Working directory: " << std::filesystem::current_path() << std::endl;
    
    try {
        RuntimeApplication app(launchOptions);
        app.Run();
    } catch (const std::exception& e) {
        std::cerr << "[Claymore Runtime] FATAL ERROR: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "[Claymore Runtime] FATAL ERROR: Unknown exception" << std::endl;
    }
    
    std::cout << "[Claymore Runtime] Exiting." << std::endl;
    
    // Restore original streams before closing log
    std::cout.rdbuf(coutTee.GetOriginal());
    std::cerr.rdbuf(cerrTee.GetOriginal());
    logFile.close();
    
    return 0;
}







