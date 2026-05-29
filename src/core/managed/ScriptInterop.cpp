#include "ScriptInterop.h"

namespace cm {
namespace script {

// ============================================================================
// Global Script Function Pointers - Definition
// ============================================================================
// These are set by RuntimeHost::InitializeDotNet() for runtime builds
// or by DotNetHost::LoadDotnetRuntime() for editor builds.

CreateInstance_fn g_CreateInstance = nullptr;
OnBind_fn g_OnBind = nullptr;
OnCreate_fn g_OnCreate = nullptr;
OnUpdate_fn g_OnUpdate = nullptr;
OnDestroy_fn g_OnDestroy = nullptr;
Invoke_fn g_Invoke = nullptr;
Destroy_fn g_Destroy = nullptr;
FlushSyncContext_fn g_FlushSyncContext = nullptr;
UpdateButtons_fn g_UpdateButtons = nullptr;
SetField_fn g_SetField = nullptr;
SetManagedField_fn g_SetManagedField = nullptr;
ManagedFrameUpdate_fn g_ManagedFrameUpdate = nullptr;
EnsureInstalled_fn g_EnsureInstalled = nullptr;

} // namespace script
} // namespace cm

// ============================================================================
// Legacy compatibility - SetManagedFieldPtr, GetManagedFieldPtr and EnsureInstalledPtr
// These are defined here for both editor and runtime builds.
// DotNetHost.cpp (editor) and RuntimeHost.cpp (runtime) will set these pointers.
// Scene.cpp uses them for RuntimeClone property binding and sync context.
// ============================================================================
void (*SetManagedFieldPtr)(void* handle, const char* fieldName, void* boxedValue) = nullptr;
bool (*GetManagedFieldPtr)(void* handle, const char* fieldName, int propertyType, void* boxedOut) = nullptr;

#ifdef _WIN32
void (__stdcall *EnsureInstalledPtr)() = nullptr;
#else
void (*EnsureInstalledPtr)() = nullptr;
#endif

