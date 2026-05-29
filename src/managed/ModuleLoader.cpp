#include "ModuleLoader.h"
#include <iostream>
#include <algorithm>
#include <filesystem>
#include "managed/interop/ModuleInterop.h"
#include "managed/interop/DotNetHost.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace {
   std::vector<ModuleLoader::LoadedModuleInfo> g_Loaded;
}

namespace ModuleLoader {

ModuleHandle* LoadModule(const std::string& path, const NativeAPIs& /*native*/, ManagedAPIs* out) {
    if (!IsDotnetRuntimeReady()) {
        std::cerr << "[ModuleLoader] .NET runtime not ready; cannot load module: " << path << std::endl;
        return nullptr;
    }
    auto full = std::filesystem::absolute(path);
    if (!std::filesystem::exists(full)) {
        std::cerr << "[ModuleLoader] Module DLL not found: " << full << std::endl;
        return nullptr;
    }

    // Resolve functions from the engine assembly (next to the executable)
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::filesystem::path engineDLL = std::filesystem::path(exePath).parent_path() / L"ClaymoreEngine.dll";

    // 1) Load target module assembly into the AppDomain
    {
        void* fnLoad = nullptr;
        if (!ResolveManagedDelegate(engineDLL.wstring(),
                                    L"ClaymoreEngine.ModuleInteropExports, ClaymoreEngine",
                                    L"LoadModuleAssembly",
                                    L"ClaymoreEngine.ModuleInteropExports+LoadModuleAssemblyDelegate, ClaymoreEngine",
                                    &fnLoad))
        {
            std::cerr << "[ModuleLoader] Failed to resolve LoadModuleAssembly from engine." << std::endl;
            return nullptr;
        }
        using LoadModuleAssemblyFn = void(__cdecl*)(const wchar_t*);
        LoadModuleAssemblyFn loadAsm = reinterpret_cast<LoadModuleAssemblyFn>(fnLoad);
        std::wcout << L"[ModuleLoader] Loading assembly: " << full.wstring() << std::endl;
        loadAsm(full.wstring().c_str());
        std::cout << "[ModuleLoader] Assembly load call completed" << std::endl;
    }

    // 2) Get managed API table provider from engine assembly
    void* fn = nullptr;
    if (!ResolveManagedDelegate(engineDLL.wstring(),
                                L"ClaymoreEngine.ModuleInteropExports, ClaymoreEngine",
                                L"GetModuleAPI",
                                L"ClaymoreEngine.ModuleInteropExports+GetManagedModuleAPIDelegate, ClaymoreEngine",
                                &fn))
    {
        std::cerr << "[ModuleLoader] Failed to resolve GetModuleAPI from engine." << std::endl;
        return nullptr;
    }

    using GetManagedModuleAPIDelegate = void(*)(void*, void*);
    GetManagedModuleAPIDelegate getApi = reinterpret_cast<GetManagedModuleAPIDelegate>(fn);

    ::NativeAPIs nat{}; FillModuleNativeAPIs(nat);
    ::ManagedAPIs man{};
    
    // Note: C++ try-catch cannot catch .NET exceptions (0xE0434352)
    // The managed side now has comprehensive error handling
    std::cout << "[ModuleLoader] Calling GetModuleAPI..." << std::endl;
    getApi(&nat, &man);
    if (out) *out = man; 
    std::cout << "[ModuleLoader] Successfully called GetModuleAPI" << std::endl;
       
    auto* h = new ModuleHandle();
    h->path = full.string();
    h->id = std::filesystem::path(h->path).stem().string();
    h->alc = nullptr; h->instance = nullptr;
    g_Loaded.push_back({ h->id, h->path, h });
    std::cout << "[ModuleLoader] Loaded module: " << h->id << " from " << h->path << std::endl;

    if (man.EnumerateComponents) {
        using EnumerateComponentsDelegate = void(*)(void*);
        EnumerateComponentsDelegate enumerateComponents = reinterpret_cast<EnumerateComponentsDelegate>(man.EnumerateComponents);
        enumerateComponents(nullptr);
    }
    return h;
}  

void UnloadModule(ModuleHandle*& h) {
    if (!h) return;
    std::cout << "[ModuleLoader] Unloading module: " << h->path << std::endl;
    g_Loaded.erase(std::remove_if(g_Loaded.begin(), g_Loaded.end(), [&](const LoadedModuleInfo& i){ return i.handle == h; }), g_Loaded.end());
    delete h; h = nullptr;
}

const std::vector<LoadedModuleInfo>& GetLoaded() { return g_Loaded; }

}


