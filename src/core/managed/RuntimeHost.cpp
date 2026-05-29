#include "RuntimeHost.h"
#include "RuntimeInterop.h"
#include "ScriptInterop.h"
#include "ScriptSystem.h"
#include "ScriptReflection.h"
#include "core/multiplayer/MultiplayerBridge.h"
#include "core/vfs/VirtualFS.h"
#include "editor/Project.h"
#include <vector>
#include <algorithm>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <iostream>
#include <fstream>
#include <string>

// .NET hosting headers
extern "C" {
#include "nethost.h"
}
#include "coreclr_delegates.h"
#include "hostfxr.h"

// Forward declarations for registration functions (implementations at end of file)
extern "C" __declspec(dllexport) void NativeRegisterScriptType(const char* className, int priority);
extern "C" __declspec(dllexport) void NativeRegisterScriptFlags(const char* className, uint32_t flags);
extern "C" __declspec(dllexport) void RegisterScriptPropertyNative(const char*, const char*, int, void*, const char*);
extern "C" __declspec(dllexport) void RegisterScriptPropertyExtended(const char*, const char*, int, void*, const char*, const char*, const char*, int, const char*, const char*, bool, bool);
extern "C" __declspec(dllexport) void ClearScriptPropertiesNative(const char* className);

// Struct passed to managed side for script registration callbacks
// Must match ScriptRegistrationInterop in InteropExports.cs
struct ScriptRegistrationInterop {
    void (*RegisterScriptType)(const char*, int priority);
    void (*RegisterScriptFlags)(const char*, uint32_t flags);
    void (*RegisterScriptProperty)(const char*, const char*, int, void*, const char*);
    void (*RegisterScriptPropertyExtended)(const char*, const char*, int, void*, const char*, const char*, const char*, int, const char*, const char*, bool, bool);
    void (*ClearScriptProperties)(const char*);
};

// Global script registration interop - initialized here so it's available throughout file
static ScriptRegistrationInterop g_ScriptRegInterop = { 
    &NativeRegisterScriptType,
    &NativeRegisterScriptFlags,
    &RegisterScriptPropertyNative,
    &RegisterScriptPropertyExtended,
    &ClearScriptPropertiesNative
};

namespace cm {
namespace runtime {

// HostFXR function pointers
static hostfxr_initialize_for_runtime_config_fn s_init_fptr = nullptr;
static hostfxr_get_runtime_delegate_fn s_get_delegate_fptr = nullptr;
static hostfxr_close_fn s_close_fptr = nullptr;
static load_assembly_and_get_function_pointer_fn s_load_assembly_and_get_fn = nullptr;

// Script interop function pointers
static Script_Create_fn s_Script_Create = nullptr;
static Script_Bind_fn s_Script_Bind = nullptr;
static Script_OnCreate_fn s_Script_OnCreate = nullptr;
static Script_OnUpdate_fn s_Script_OnUpdate = nullptr;
static Script_OnDestroy_fn s_Script_OnDestroy = nullptr;
static Script_Invoke_fn s_Script_Invoke = nullptr;
static Script_Destroy_fn s_Script_Destroy = nullptr;

// SyncContext control
using FlushSyncContext_fn = void(__stdcall*)();
static FlushSyncContext_fn s_FlushSyncContext = nullptr;
using ManagedFrameUpdate_fn = void(*)(float);
static ManagedFrameUpdate_fn s_ManagedFrameUpdate = nullptr;

// RegisterAllScripts function pointer - triggers managed script registration
using RegisterAllScriptsFn = void(CORECLR_DELEGATE_CALLTYPE*)(void* fnPtr);
static RegisterAllScriptsFn s_RegisterAllScripts = nullptr;

static bool s_RuntimeReady = false;
static bool s_UsingBundledDotnetRoot = false;

static std::filesystem::path GetBundledDotnetRoot(const std::filesystem::path& exeDir) {
    return exeDir / L"dotnet";
}

static bool HasBundledDotnetRuntime(const std::filesystem::path& dotnetRoot) {
    return std::filesystem::exists(dotnetRoot / L"host" / L"fxr") &&
           std::filesystem::exists(dotnetRoot / L"shared" / L"Microsoft.NETCore.App");
}

static bool HasLegacySelfContainedHost(const std::filesystem::path& exeDir) {
    return std::filesystem::exists(exeDir / L"hostfxr.dll") ||
           std::filesystem::exists(exeDir / L"coreclr.dll") ||
           std::filesystem::exists(exeDir / L"hostpolicy.dll");
}

// Entity interop function pointers (exported to managed)
extern "C" void GetEntityWorldPosition(int entityID, float* outX, float* outY, float* outZ);
extern "C" void SetEntityWorldPosition(int entityID, float x, float y, float z);
extern "C" int FindEntityByName(const char* name);

static bool LoadHostFxrInternal() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    const std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
    const std::filesystem::path bundledDotnetRoot = GetBundledDotnetRoot(exeDir);

    HMODULE hostfxr = nullptr;

    HMODULE nethost = LoadLibraryW(L"nethost.dll");
    if (!nethost) {
        std::wcerr << L"[RuntimeHost] Failed to load nethost.dll\n";
        std::wcerr << L"[RuntimeHost] Either bundle a private .NET runtime under dotnet\\ or install .NET 10.\n";
        return false;
    }

    using get_hostfxr_path_fn = int(__cdecl*)(char_t*, size_t*, const get_hostfxr_parameters*);
    auto get_hostfxr_path = (get_hostfxr_path_fn)GetProcAddress(nethost, "get_hostfxr_path");
    if (!get_hostfxr_path) {
        std::cerr << "[RuntimeHost] Failed to resolve get_hostfxr_path\n";
        return false;
    }

    if (HasLegacySelfContainedHost(exeDir)) {
        std::wcerr << L"[RuntimeHost] Ignoring app-local hostfxr/coreclr files.\n";
        std::wcerr << L"[RuntimeHost] .NET component hosting expects nethost + runtimeconfig, not self-contained app layout.\n";
    }

    wchar_t buffer[MAX_PATH];
    size_t size = sizeof(buffer) / sizeof(wchar_t);
    if (HasBundledDotnetRuntime(bundledDotnetRoot)) {
        get_hostfxr_parameters params{};
        params.size = sizeof(get_hostfxr_parameters);
        params.dotnet_root = bundledDotnetRoot.c_str();

        int rc = get_hostfxr_path(buffer, &size, &params);
        if (rc == 0) {
            hostfxr = LoadLibraryW(buffer);
            if (hostfxr) {
                s_UsingBundledDotnetRoot = true;
                std::wcout << L"[RuntimeHost] Using bundled .NET runtime root: " << bundledDotnetRoot << std::endl;
            } else {
                std::wcerr << L"[RuntimeHost] Failed to load hostfxr.dll from bundled runtime root: " << buffer << std::endl;
            }
        } else {
            std::wcerr << L"[RuntimeHost] Failed to resolve hostfxr from bundled runtime root: " << bundledDotnetRoot << std::endl;
        }
    }

    if (!hostfxr) {
        size = sizeof(buffer) / sizeof(wchar_t);
        if (get_hostfxr_path(buffer, &size, nullptr) != 0) {
            std::wcerr << L"[RuntimeHost] .NET 10 runtime not found.\n";
            return false;
        }

        hostfxr = LoadLibraryW(buffer);
        if (!hostfxr) {
            std::wcerr << L"[RuntimeHost] Failed to load hostfxr.dll\n";
            return false;
        }
        std::wcout << L"[RuntimeHost] Using system .NET runtime\n";
    }

    s_init_fptr = (hostfxr_initialize_for_runtime_config_fn)GetProcAddress(hostfxr, "hostfxr_initialize_for_runtime_config");
    s_get_delegate_fptr = (hostfxr_get_runtime_delegate_fn)GetProcAddress(hostfxr, "hostfxr_get_runtime_delegate");
    s_close_fptr = (hostfxr_close_fn)GetProcAddress(hostfxr, "hostfxr_close");

    return (s_init_fptr && s_get_delegate_fptr && s_close_fptr);
}

bool InitializeDotNet() {
    if (s_RuntimeReady) return true;

    std::cout << "[RuntimeHost] Initializing .NET runtime...\n";

    if (!LoadHostFxrInternal()) {
        std::cerr << "[RuntimeHost] Failed to load hostfxr\n";
        return false;
    }

    // Get exe directory
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
    
    // Find runtime config
    std::filesystem::path runtimeConfigPath = exeDir / L"ClaymoreEngine.runtimeconfig.json";
    
    if (!std::filesystem::exists(runtimeConfigPath)) {
        std::wcerr << L"[RuntimeHost] Runtime config not found: " << runtimeConfigPath << std::endl;
        return false;
    }

    std::ifstream configFile(runtimeConfigPath);
    if (configFile.is_open()) {
        std::string content((std::istreambuf_iterator<char>(configFile)),
                             std::istreambuf_iterator<char>());
        configFile.close();

        if (content.find("\"includedFrameworks\"") != std::string::npos) {
            std::wcerr << L"[RuntimeHost] Incompatible runtimeconfig detected: " << runtimeConfigPath << std::endl;
            std::wcerr << L"[RuntimeHost] ClaymoreEngine must use a framework-dependent runtimeconfig when hosted as a component.\n";
            std::wcerr << L"[RuntimeHost] Re-stage the managed output from bin\\<Config>\\net10.0 or use a bundled dotnet\\ runtime root.\n";
            return false;
        }
    }

    std::wstring runtimeConfigStr = runtimeConfigPath.wstring();
    std::wcout << L"[RuntimeHost] Using config: " << runtimeConfigStr << std::endl;

    hostfxr_handle handle = nullptr;
    int rc = s_init_fptr(runtimeConfigStr.c_str(), nullptr, &handle);
    if (rc != 0 || handle == nullptr) {
        std::cerr << "[RuntimeHost] Failed to initialize runtime. HRESULT: 0x" << std::hex << rc << std::endl;
        return false;
    }

    rc = s_get_delegate_fptr(handle, hdt_load_assembly_and_get_function_pointer, (void**)&s_load_assembly_and_get_fn);
    s_close_fptr(handle);
    if (rc != 0 || !s_load_assembly_and_get_fn) {
        std::cerr << "[RuntimeHost] Failed to get assembly loader. HRESULT: 0x" << std::hex << rc << std::endl;
        return false;
    }

    // Find ClaymoreEngine.dll
    std::filesystem::path engineDllPath = exeDir / L"ClaymoreEngine.dll";
    if (!std::filesystem::exists(engineDllPath)) {
        std::wcerr << L"[RuntimeHost] ClaymoreEngine.dll not found\n";
        return false;
    }

    std::wstring engineDll = engineDllPath.wstring();

    // Set environment variable for game scripts location
    std::filesystem::path gameScriptsDll = exeDir / L"GameScripts.dll";
    if (!std::filesystem::exists(gameScriptsDll)) {
        const std::filesystem::path projectScriptsDll =
            std::filesystem::path(Project::GetProjectDirectory()) / ".library" / "GameScripts.dll";
        if (std::filesystem::exists(projectScriptsDll)) {
            gameScriptsDll = projectScriptsDll;
        }
    }
    if (std::filesystem::exists(gameScriptsDll)) {
        SetEnvironmentVariableW(L"CLAYMORE_SCRIPTS_DLL", gameScriptsDll.c_str());
        std::wcout << L"[RuntimeHost] Using GameScripts.dll from " << gameScriptsDll << L"\n";
    } else {
        std::wcerr << L"[RuntimeHost] Warning: GameScripts.dll not found\n";
    }

    // Load managed entry point
    using component_entry_point_fn = int (CORECLR_DELEGATE_CALLTYPE*)(void*, int);
    component_entry_point_fn entryPoint = nullptr;

    rc = s_load_assembly_and_get_fn(
        engineDll.c_str(),
        L"ClaymoreEngine.EngineEntry, ClaymoreEngine",
        L"ManagedStart",
        L"ClaymoreEngine.EntryPointDelegate, ClaymoreEngine",
        nullptr,
        (void**)&entryPoint
    );

    if (rc == 0 && entryPoint) {
        int result = entryPoint(nullptr, 0);
        std::cout << "[RuntimeHost] ManagedStart returned: " << result << "\n";
        if (result != 0) {
            std::cerr << "[RuntimeHost] WARNING: ManagedStart failed (scripts may not be loaded)\n";
        }
    } else {
        std::cerr << "[RuntimeHost] ERROR: Failed to resolve ManagedStart (rc=0x" << std::hex << rc << std::dec << ")\n";
    }

    // Resolve script interop functions
    rc = s_load_assembly_and_get_fn(
        engineDll.c_str(),
        L"ClaymoreEngine.InteropExports, ClaymoreEngine",
        L"Script_Create",
        L"ClaymoreEngine.Script_CreateDelegate, ClaymoreEngine",
        nullptr,
        (void**)&s_Script_Create
    );

    rc |= s_load_assembly_and_get_fn(
        engineDll.c_str(),
        L"ClaymoreEngine.InteropExports, ClaymoreEngine",
        L"Script_Bind",
        L"ClaymoreEngine.Script_BindDelegate, ClaymoreEngine",
        nullptr,
        (void**)&s_Script_Bind
    );

    rc |= s_load_assembly_and_get_fn(
        engineDll.c_str(),
        L"ClaymoreEngine.InteropExports, ClaymoreEngine",
        L"Script_OnCreate",
        L"ClaymoreEngine.Script_OnCreateDelegate, ClaymoreEngine",
        nullptr,
        (void**)&s_Script_OnCreate
    );

    rc |= s_load_assembly_and_get_fn(
        engineDll.c_str(),
        L"ClaymoreEngine.InteropExports, ClaymoreEngine",
        L"Script_OnUpdate",
        L"ClaymoreEngine.Script_OnUpdateDelegate, ClaymoreEngine",
        nullptr,
        (void**)&s_Script_OnUpdate
    );

    s_load_assembly_and_get_fn(
        engineDll.c_str(),
        L"ClaymoreEngine.InteropExports, ClaymoreEngine",
        L"Script_OnDestroy",
        L"ClaymoreEngine.Script_OnDestroyDelegate, ClaymoreEngine",
        nullptr,
        (void**)&s_Script_OnDestroy
    );

    s_load_assembly_and_get_fn(
        engineDll.c_str(),
        L"ClaymoreEngine.InteropExports, ClaymoreEngine",
        L"Script_Invoke",
        L"ClaymoreEngine.Script_InvokeDelegate, ClaymoreEngine",
        nullptr,
        (void**)&s_Script_Invoke
    );

    s_load_assembly_and_get_fn(
        engineDll.c_str(),
        L"ClaymoreEngine.InteropExports, ClaymoreEngine",
        L"Script_Destroy",
        L"ClaymoreEngine.Script_DestroyDelegate, ClaymoreEngine",
        nullptr,
        (void**)&s_Script_Destroy
    );

    // Resolve FlushSyncContext
    s_load_assembly_and_get_fn(
        engineDll.c_str(),
        L"ClaymoreEngine.EngineSyncContext, ClaymoreEngine",
        L"Flush",
        L"ClaymoreEngine.VoidDelegate, ClaymoreEngine",
        nullptr,
        (void**)&s_FlushSyncContext
    );

    // Resolve Button.UpdateAll (managed button pump)
    void* updateButtonsFn = nullptr;
    s_load_assembly_and_get_fn(
        engineDll.c_str(),
        L"ClaymoreEngine.Button, ClaymoreEngine",
        L"UpdateAll",
        L"ClaymoreEngine.VoidDelegate, ClaymoreEngine",
        nullptr,
        &updateButtonsFn
    );
    if (updateButtonsFn) {
        cm::script::g_UpdateButtons = reinterpret_cast<cm::script::UpdateButtons_fn>(updateButtonsFn);
        std::cout << "[RuntimeHost] Button.UpdateAll resolved\n";
    } else {
        cm::script::g_UpdateButtons = nullptr;
        std::cerr << "[RuntimeHost] WARNING: Button.UpdateAll not resolved\n";
    }

    // Resolve ManagedFrameBridge.UpdateExport so runtime-owned systems can
    // advance tweens and similar frame services without any script pump.
    s_load_assembly_and_get_fn(
        engineDll.c_str(),
        L"ClaymoreEngine.ManagedFrameBridge, ClaymoreEngine",
        L"UpdateExport",
        L"ClaymoreEngine.ManagedFrameUpdateDelegate, ClaymoreEngine",
        nullptr,
        (void**)&s_ManagedFrameUpdate
    );
    if (s_ManagedFrameUpdate) {
        cm::script::g_ManagedFrameUpdate = reinterpret_cast<cm::script::ManagedFrameUpdate_fn>(s_ManagedFrameUpdate);
        std::cout << "[RuntimeHost] ManagedFrameBridge.UpdateExport resolved\n";
    } else {
        cm::script::g_ManagedFrameUpdate = nullptr;
        std::cerr << "[RuntimeHost] WARNING: ManagedFrameBridge.UpdateExport not resolved\n";
    }
    
    // Resolve EnsureInstalledHere (for managed async/await support)
    // This must be called every frame to ensure the sync context is installed on the main thread
    void* ensureInstalledFn = nullptr;
    s_load_assembly_and_get_fn(
        engineDll.c_str(),
        L"ClaymoreEngine.EngineSyncContext, ClaymoreEngine",
        L"EnsureInstalledHereFromNative",
        L"ClaymoreEngine.VoidDelegate, ClaymoreEngine",
        nullptr,
        &ensureInstalledFn
    );
    if (ensureInstalledFn) {
        cm::script::g_EnsureInstalled = reinterpret_cast<cm::script::EnsureInstalled_fn>(ensureInstalledFn);
        std::cout << "[RuntimeHost] EnsureInstalled resolved for async support\n";
    } else {
        std::cerr << "[RuntimeHost] WARNING: EnsureInstalled not resolved - async callbacks may not work\n";
    }
    
    // Resolve SetManagedField - CRITICAL for script property initialization
    void* setFieldFn = nullptr;
    s_load_assembly_and_get_fn(
        engineDll.c_str(),
        L"ClaymoreEngine.InteropExports, ClaymoreEngine",
        L"SetManagedField",
        L"ClaymoreEngine.InteropExports+SetFieldDelegate, ClaymoreEngine",
        nullptr,
        &setFieldFn
    );
    if (setFieldFn) {
        cm::script::g_SetManagedField = reinterpret_cast<cm::script::SetManagedField_fn>(setFieldFn);
        SetManagedFieldPtr = reinterpret_cast<cm::script::SetManagedField_fn>(setFieldFn);
        std::cout << "[RuntimeHost] SetManagedField resolved successfully\n";
    } else {
        std::cerr << "[RuntimeHost] WARNING: SetManagedField not resolved - script properties won't be set\n";
    }

    if (!s_Script_Create || !s_Script_OnCreate || !s_Script_OnUpdate) {
        std::cerr << "[RuntimeHost] Failed to resolve script interop functions\n";
        std::cerr << "[RuntimeHost] Create=" << (void*)s_Script_Create 
                  << " OnCreate=" << (void*)s_Script_OnCreate 
                  << " OnUpdate=" << (void*)s_Script_OnUpdate << std::endl;
        return false;
    }

    // Set the unified ScriptInterop global pointers so ManagedScriptComponent works
    cm::script::g_CreateInstance = s_Script_Create;
    cm::script::g_OnBind = s_Script_Bind;
    cm::script::g_OnCreate = s_Script_OnCreate;
    cm::script::g_OnUpdate = s_Script_OnUpdate;
    cm::script::g_OnDestroy = s_Script_OnDestroy;
    cm::script::g_Invoke = s_Script_Invoke;
    cm::script::g_Destroy = s_Script_Destroy;
    cm::script::g_FlushSyncContext = reinterpret_cast<cm::script::FlushSyncContext_fn>(s_FlushSyncContext);
    std::cout << "[RuntimeHost] Set unified ScriptInterop function pointers\n";
    std::cout << "[RuntimeHost]   g_CreateInstance=" << (void*)cm::script::g_CreateInstance << "\n";
    std::cout << "[RuntimeHost]   g_OnCreate=" << (void*)cm::script::g_OnCreate << "\n";
    std::cout << "[RuntimeHost]   g_OnUpdate=" << (void*)cm::script::g_OnUpdate << "\n";

    // Resolve RegisterAllScripts to trigger managed script registration
    // This populates ScriptReflection with property metadata for each script class
    void* regAllFn = nullptr;
    rc = s_load_assembly_and_get_fn(
        engineDll.c_str(),
        L"ClaymoreEngine.InteropExports, ClaymoreEngine",
        L"RegisterAllScripts",
        L"ClaymoreEngine.RegisterAllScriptsDelegate, ClaymoreEngine",
        nullptr,
        &regAllFn
    );
    if (rc == 0 && regAllFn) {
        s_RegisterAllScripts = reinterpret_cast<RegisterAllScriptsFn>(regAllFn);
        std::cout << "[RuntimeHost] RegisterAllScripts resolved, invoking script registration...\n";
        s_RegisterAllScripts(reinterpret_cast<void*>(&g_ScriptRegInterop));
        std::cout << "[RuntimeHost] Script registration complete\n";
    } else {
        std::cerr << "[RuntimeHost] WARNING: RegisterAllScripts not resolved - scripts won't be registered\n";
    }

    // Setup all managed interop systems (entity, input, physics, navigation, etc.)
    // This mirrors the interop setup done by DotNetHost in the editor
    if (!SetupAllInterop(s_load_assembly_and_get_fn, engineDll)) {
        std::cerr << "[RuntimeHost] WARNING: Some interop systems failed to initialize\n";
        // Continue anyway - scripts may still partially work
    }

    if (!cm::multiplayer::InitializeManagedBridge(s_load_assembly_and_get_fn, engineDll)) {
        std::cerr << "[RuntimeHost] Multiplayer bridge unavailable - networking will stay offline\n";
    }

    s_RuntimeReady = true;
    std::cout << "[RuntimeHost] .NET runtime initialized successfully\n";
    return true;
}

bool IsDotNetReady() {
    return s_RuntimeReady;
}

void ShutdownDotNet() {
    std::cout << "[RuntimeHost] Shutting down .NET runtime..." << std::endl;

    cm::multiplayer::ShutdownManagedBridge();
    
    // Clear function pointers FIRST to prevent any more calls into .NET
    // This ensures that ManagedScriptComponent destructors don't call into freed memory
    s_Script_Create = nullptr;
    s_Script_Bind = nullptr;
    s_Script_OnCreate = nullptr;
    s_Script_OnUpdate = nullptr;
    s_Script_OnDestroy = nullptr;
    s_Script_Invoke = nullptr;
    s_Script_Destroy = nullptr;
    s_FlushSyncContext = nullptr;
    s_ManagedFrameUpdate = nullptr;
    
    // Clear the global script interop pointers as well
    cm::script::g_CreateInstance = nullptr;
    cm::script::g_OnBind = nullptr;
    cm::script::g_OnCreate = nullptr;
    cm::script::g_OnUpdate = nullptr;
    cm::script::g_OnDestroy = nullptr;
    cm::script::g_Invoke = nullptr;
    cm::script::g_Destroy = nullptr;
    cm::script::g_FlushSyncContext = nullptr;
    cm::script::g_UpdateButtons = nullptr;
    cm::script::g_SetManagedField = nullptr;
    cm::script::g_ManagedFrameUpdate = nullptr;
    
    // Mark .NET as not ready
    s_RuntimeReady = false;
    
    std::cout << "[RuntimeHost] .NET runtime shutdown complete" << std::endl;
}

Script_Create_fn GetScriptCreate() { return s_Script_Create; }
Script_Bind_fn GetScriptBind() { return s_Script_Bind; }
Script_OnCreate_fn GetScriptOnCreate() { return s_Script_OnCreate; }
Script_OnUpdate_fn GetScriptOnUpdate() { return s_Script_OnUpdate; }
Script_Invoke_fn GetScriptInvoke() { return s_Script_Invoke; }
Script_Destroy_fn GetScriptDestroy() { return s_Script_Destroy; }

void* CreateScriptInstance(const std::string& className) {
    if (!s_RuntimeReady || !s_Script_Create) return nullptr;
    return s_Script_Create(className.c_str());
}

void CallBind(void* instance, int entityID) {
    if (!instance || !s_Script_Bind) return;
    s_Script_Bind(instance, entityID);
}

void CallOnCreate(void* instance, int entityID) {
    if (!instance || !s_Script_OnCreate) return;
    if (reinterpret_cast<uintptr_t>(instance) < 0x10000) {
        std::cerr << "[RuntimeHost] CallOnCreate: Invalid instance pointer\n";
        return;
    }
    
    // PERF: SEH has overhead - use CLAYMORE_FAST_SCRIPT_CALLS to disable in release
#if defined(_MSC_VER) && !defined(CLAYMORE_FAST_SCRIPT_CALLS)
    __try {
        s_Script_OnCreate(instance, entityID);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        std::cerr << "[RuntimeHost] Script threw exception during OnCreate (entityID=" << entityID << ") - check console for details\n";
    }
#else
    s_Script_OnCreate(instance, entityID);
#endif
}

void CallOnUpdate(void* instance, float dt) {
    if (!instance || !s_Script_OnUpdate) return;
    if (reinterpret_cast<uintptr_t>(instance) < 0x10000) {
        std::cerr << "[RuntimeHost] CallOnUpdate: Invalid instance pointer\n";
        return;
    }
    
    // PERF: SEH has overhead - use CLAYMORE_FAST_SCRIPT_CALLS to disable in release
#if defined(_MSC_VER) && !defined(CLAYMORE_FAST_SCRIPT_CALLS)
    __try {
        s_Script_OnUpdate(instance, dt);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        std::cerr << "[RuntimeHost] Script threw exception during OnUpdate - check console for details\n";
    }
#else
    s_Script_OnUpdate(instance, dt);
#endif
}

void FlushSyncContext() {
    if (s_FlushSyncContext) {
        s_FlushSyncContext();
    }
}

} // namespace runtime
} // namespace cm

// Runtime script registration - exported so managed code can register script types
static std::vector<std::string> g_RegisteredScriptNames;

extern "C" __declspec(dllexport) void NativeRegisterScriptType(const char* className, int priority)
{
    if (std::find(g_RegisteredScriptNames.begin(), g_RegisteredScriptNames.end(), className) == g_RegisteredScriptNames.end())
        g_RegisteredScriptNames.emplace_back(className);
    std::cout << "[RuntimeHost] Registered script type: " << className << " priority=" << priority << std::endl;

    ScriptSystem::Instance().RegisterManaged(className);
    ScriptSystem::Instance().SetScriptPriority(className, priority);
}

extern "C" __declspec(dllexport) void NativeRegisterScriptFlags(const char* className, uint32_t flags)
{
    if (!className) return;
    ScriptSystem::Instance().SetScriptFlags(className, flags);
}

extern "C" __declspec(dllexport) void ClearScriptPropertiesNative(const char* className)
{
    if (!className) return;
    ScriptReflection::ClearScriptPropertiesForClass(className);
}

extern "C" __declspec(dllexport) void RegisterScriptPropertyNative(
    const char* className,
    const char* fieldName,
    int propType,
    void* boxedDefault,
    const char* auxTypeFullName)
{
    PropertyInfo info;
    info.name = fieldName;
    info.type = static_cast<PropertyType>(propType);
    info.currentValue = ScriptReflection::BoxToValue(boxedDefault, info.type);
    info.defaultValue = info.currentValue;
    if (auxTypeFullName) info.auxTypeName = auxTypeFullName;

    ScriptReflection::RegisterScriptProperty(className, info);
}

extern "C" __declspec(dllexport) void RegisterScriptPropertyExtended(
    const char* className,
    const char* fieldName,
    int propType,
    void* boxedDefault,
    const char* auxTypeFullName,
    const char* enumNames,
    const char* enumValues,
    int listElementType,
    const char* listElementTypeName,
    const char* structFieldsJson,
    bool populateFromResources,
    bool selectFromResources)
{
    PropertyInfo info;
    info.name = fieldName;
    info.type = static_cast<PropertyType>(propType);
    info.currentValue = ScriptReflection::BoxToValue(boxedDefault, info.type);
    info.defaultValue = info.currentValue;
    if (auxTypeFullName) info.auxTypeName = auxTypeFullName;
    
    // Handle enum metadata - parse pipe-separated strings
    // Store enum metadata for both direct enum properties AND list elements that are enums
    if (enumNames && enumValues) {
        std::string names(enumNames);
        std::string values(enumValues);
        size_t pos = 0;
        while ((pos = names.find('|')) != std::string::npos) {
            info.enumMeta.names.push_back(names.substr(0, pos));
            names.erase(0, pos + 1);
        }
        if (!names.empty()) info.enumMeta.names.push_back(names);
        
        pos = 0;
        while ((pos = values.find('|')) != std::string::npos) {
            info.enumMeta.values.push_back(std::stoi(values.substr(0, pos)));
            values.erase(0, pos + 1);
        }
        if (!values.empty()) info.enumMeta.values.push_back(std::stoi(values));
    }
    
    // Handle list element type
    if (info.type == PropertyType::List) {
        info.listElementType = static_cast<PropertyType>(listElementType);
        if (listElementTypeName) info.listElementTypeName = listElementTypeName;
        
        // If list element type is Enum, enum metadata should already be set above
        // The inspector will use property.enumMeta for rendering enum list elements
    }
    
    // Mark if this field is auto-populated from resources
    info.populateFromResources = populateFromResources;
    
    // Mark if this ClayObject field should show as a dropdown selector
    info.selectFromResources = selectFromResources;

    ScriptReflection::RegisterScriptProperty(className, info);
}

