#pragma once

#include <string>
#include <filesystem>

// RuntimeHost: Core-only .NET hosting for runtime builds
// Unlike DotNetHost in managed/, this has no editor dependencies
// and provides minimal scripting support for exported games

namespace cm {
namespace runtime {

// Initialize the .NET runtime for game execution
// Must be called before any script operations
// Returns true on success
bool InitializeDotNet();

// Check if .NET runtime is ready
bool IsDotNetReady();

// Shutdown the .NET runtime
void ShutdownDotNet();

// Script interop function pointers (resolved at runtime)
using Script_Create_fn = void* (*)(const char* className);
using Script_Bind_fn = void (*)(void* handle, int entityID);
using Script_OnCreate_fn = void (*)(void* handle, int entityID);
using Script_OnUpdate_fn = void (*)(void* handle, float dt);
using Script_OnDestroy_fn = void (*)(void* handle);
using Script_Invoke_fn = void (*)(void* handle, const char* methodName);
using Script_Destroy_fn = void (*)(void* handle);

// Get the script function pointers (null if .NET not initialized)
Script_Create_fn GetScriptCreate();
Script_Bind_fn GetScriptBind();
Script_OnCreate_fn GetScriptOnCreate();
Script_OnUpdate_fn GetScriptOnUpdate();
Script_Invoke_fn GetScriptInvoke();
Script_Destroy_fn GetScriptDestroy();

// Create a script instance by class name
void* CreateScriptInstance(const std::string& className);

// Call Bind on a script instance (registers it, must be called BEFORE OnCreate)
void CallBind(void* instance, int entityID);

// Call OnCreate on a script instance (must be called AFTER all scripts bound)
void CallOnCreate(void* instance, int entityID);

// Call OnUpdate on a script instance
void CallOnUpdate(void* instance, float dt);

// Flush any pending async callbacks (SyncContext)
void FlushSyncContext();

} // namespace runtime
} // namespace cm

