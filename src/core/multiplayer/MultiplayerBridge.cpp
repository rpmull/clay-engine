#include "MultiplayerBridge.h"

#include <iostream>

namespace cm {
namespace multiplayer {

namespace {

using MultiplayerUpdate_fn = void(*)(float);
using MultiplayerVoid_fn = void(*)();

MultiplayerUpdate_fn g_PreUpdate = nullptr;
MultiplayerUpdate_fn g_PostUpdate = nullptr;
MultiplayerVoid_fn g_Shutdown = nullptr;

constexpr const wchar_t* kManagedType = L"ClaymoreEngine.Networking.MultiplayerRuntime, ClaymoreEngine";
constexpr const wchar_t* kUpdateDelegate = L"ClaymoreEngine.Networking.MultiplayerUpdateDelegate, ClaymoreEngine";
constexpr const wchar_t* kVoidDelegate = L"ClaymoreEngine.Networking.MultiplayerVoidDelegate, ClaymoreEngine";

} // namespace

bool InitializeManagedBridge(load_assembly_and_get_function_pointer_fn loader,
                             const std::wstring& engineDllPath) {
    if (!loader) {
        std::cerr << "[MultiplayerBridge] Cannot initialize without managed loader\n";
        return false;
    }

    void* preUpdateFn = nullptr;
    void* postUpdateFn = nullptr;
    void* shutdownFn = nullptr;

    int rcPre = loader(
        engineDllPath.c_str(),
        kManagedType,
        L"PreUpdateExport",
        kUpdateDelegate,
        nullptr,
        &preUpdateFn
    );

    int rcPost = loader(
        engineDllPath.c_str(),
        kManagedType,
        L"PostUpdateExport",
        kUpdateDelegate,
        nullptr,
        &postUpdateFn
    );

    int rcShutdown = loader(
        engineDllPath.c_str(),
        kManagedType,
        L"ShutdownExport",
        kVoidDelegate,
        nullptr,
        &shutdownFn
    );

    if (rcPre != 0 || rcPost != 0 || rcShutdown != 0 || !preUpdateFn || !postUpdateFn || !shutdownFn) {
        std::cerr << "[MultiplayerBridge] Multiplayer runtime delegates unavailable"
                  << " (pre=0x" << std::hex << rcPre
                  << ", post=0x" << rcPost
                  << ", shutdown=0x" << rcShutdown << std::dec << ")\n";
        g_PreUpdate = nullptr;
        g_PostUpdate = nullptr;
        g_Shutdown = nullptr;
        return false;
    }

    g_PreUpdate = reinterpret_cast<MultiplayerUpdate_fn>(preUpdateFn);
    g_PostUpdate = reinterpret_cast<MultiplayerUpdate_fn>(postUpdateFn);
    g_Shutdown = reinterpret_cast<MultiplayerVoid_fn>(shutdownFn);

    std::cout << "[MultiplayerBridge] Managed multiplayer bridge initialized\n";
    return true;
}

void ShutdownManagedBridge() {
    if (g_Shutdown) {
        g_Shutdown();
    }

    g_PreUpdate = nullptr;
    g_PostUpdate = nullptr;
    g_Shutdown = nullptr;
}

void PreUpdate(float dt) {
    if (g_PreUpdate) {
        g_PreUpdate(dt);
    }
}

void PostUpdate(float dt) {
    if (g_PostUpdate) {
        g_PostUpdate(dt);
    }
}

bool IsManagedBridgeReady() {
    return g_PreUpdate != nullptr && g_PostUpdate != nullptr;
}

} // namespace multiplayer
} // namespace cm
