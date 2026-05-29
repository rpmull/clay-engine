#pragma once

#include <string>
#include <cstdint>
#include <iostream>
#if defined(_MSC_VER)
#include <excpt.h>
#endif

// ============================================================================
// ScriptInterop: Unified script function pointers for both editor and runtime
// ============================================================================
// This header provides a clean abstraction layer between managed script
// execution and the native engine. Both RuntimeHost (for exported games)
// and DotNetHost (for the editor) set these function pointers during
// initialization.
//
// ManagedScriptComponent and other script-related code should use ONLY these
// functions rather than directly calling RuntimeHost or DotNetHost functions.
// ============================================================================

namespace cm {
namespace script {

// Script interop function pointer types
using CreateInstance_fn = void* (*)(const char* className);
using OnBind_fn = void (*)(void* handle, int entityID);    // Binds script to entity (registers in ScriptRegistry)
using OnCreate_fn = void (*)(void* handle, int entityID);  // Calls user OnCreate (after all scripts bound)
using OnUpdate_fn = void (*)(void* handle, float dt);
using OnDestroy_fn = void (*)(void* handle);
using Invoke_fn = void (*)(void* handle, const char* methodName);
using Destroy_fn = void (*)(void* handle);
#ifdef _WIN32
using FlushSyncContext_fn = void (__stdcall*)();
#else
using FlushSyncContext_fn = void (*)();
#endif
#ifdef _WIN32
using UpdateButtons_fn = void (__stdcall*)();
#else
using UpdateButtons_fn = void (*)();
#endif
using SetField_fn = void (*)(void* handle, const char* fieldName, int fieldType, void* value);
using SetManagedField_fn = void (*)(void* handle, const char* fieldName, void* boxedValue);
using ManagedFrameUpdate_fn = void (*)(float dt);
#ifdef _WIN32
using EnsureInstalled_fn = void (__stdcall*)();
#else
using EnsureInstalled_fn = void (*)();
#endif

// ============================================================================
// Global Script Function Pointers
// ============================================================================
// These are set by RuntimeHost::InitializeDotNet() or DotNetHost::LoadDotnetRuntime()

extern CreateInstance_fn g_CreateInstance;
extern OnBind_fn g_OnBind;
extern OnCreate_fn g_OnCreate;
extern OnUpdate_fn g_OnUpdate;
extern OnDestroy_fn g_OnDestroy;
extern Invoke_fn g_Invoke;
extern Destroy_fn g_Destroy;
extern FlushSyncContext_fn g_FlushSyncContext;
extern UpdateButtons_fn g_UpdateButtons;
extern SetField_fn g_SetField;
extern SetManagedField_fn g_SetManagedField;
extern ManagedFrameUpdate_fn g_ManagedFrameUpdate;
extern EnsureInstalled_fn g_EnsureInstalled;

// ============================================================================
// Helper Functions (safer than using raw pointers)
// ============================================================================

// Returns true if the script system is initialized and ready
inline bool IsReady() {
    return g_CreateInstance != nullptr && g_OnCreate != nullptr && g_OnUpdate != nullptr;
}

// Create a new managed script instance by class name
// Returns nullptr if script system not ready or creation fails
inline void* CreateInstance(const std::string& className) {
    return g_CreateInstance ? g_CreateInstance(className.c_str()) : nullptr;
}

// Call Bind on a script instance (registers it in ScriptRegistry)
// Must be called on ALL scripts BEFORE any OnCreate calls
inline void CallOnBind(void* handle, int entityID) {
    if (handle && g_OnBind) {
        g_OnBind(handle, entityID);
    }
}

// Call OnCreate on a script instance
// Must be called AFTER all scripts have been bound
inline void CallOnCreate(void* handle, int entityID, const char* debugClassName = nullptr) {
    if (handle && g_OnCreate) {
        // Safety check for obviously invalid pointers
        if (reinterpret_cast<uintptr_t>(handle) < 0x10000) {
            std::cerr << "[ScriptInterop] CallOnCreate: Invalid handle pointer\n";
            return;
        }
        
        // PERF: SEH has overhead - use CLAYMORE_FAST_SCRIPT_CALLS to disable in release
#if defined(_MSC_VER) && !defined(CLAYMORE_FAST_SCRIPT_CALLS)
        __try {
            g_OnCreate(handle, entityID);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            std::cerr << "[ScriptInterop] Script threw exception during OnCreate (entityID=" << entityID;
            if (debugClassName && debugClassName[0] != '\0') {
                std::cerr << ", class=" << debugClassName;
            }
            std::cerr << ") - check console for details\n";
        }
#else
        g_OnCreate(handle, entityID);
#endif
    }
}


// Call OnUpdate on a script instance
inline void CallOnUpdate(void* handle, float dt, const char* debugClassName = nullptr) {
    if (handle && g_OnUpdate) {
        // Safety check for obviously invalid pointers
        if (reinterpret_cast<uintptr_t>(handle) < 0x10000) {
            std::cerr << "[ScriptInterop] CallOnUpdate: Invalid handle pointer\n";
            return;
        }
        
        // PERF: SEH has overhead - use CLAYMORE_FAST_SCRIPT_CALLS to disable in release
#if defined(_MSC_VER) && !defined(CLAYMORE_FAST_SCRIPT_CALLS)
        __try {
            g_OnUpdate(handle, dt);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            std::cerr << "[ScriptInterop] Script threw exception during OnUpdate";
            if (debugClassName && debugClassName[0] != '\0') {
                std::cerr << " (class=" << debugClassName << ")";
            }
            std::cerr << " handle=" << handle << " g_OnUpdate=" << (void*)g_OnUpdate
                      << " - check console for details\n";
        }
#else
        g_OnUpdate(handle, dt);
#endif
    }
}

// Call OnDestroy on a script instance (before releasing GCHandle)
// Safe to call during shutdown - will skip if runtime is not ready
inline void CallOnDestroy(void* handle) {
    // Skip if script system not fully initialized or shutting down
    if (!IsReady()) return;
    if (handle && g_OnDestroy) {
        // Safety check for obviously invalid pointers
        if (reinterpret_cast<uintptr_t>(handle) < 0x10000) return;
        g_OnDestroy(handle);
    }
}

// Invoke an arbitrary method on a script instance
inline void Invoke(void* handle, const char* methodName) {
    if (handle && g_Invoke && methodName) {
        g_Invoke(handle, methodName);
    }
}

// Destroy a script instance (releases GCHandle)
inline void DestroyInstance(void* handle) {
    if (handle && g_Destroy) {
        g_Destroy(handle);
    }
}

// Flush pending async callbacks from the .NET SyncContext
inline void FlushSyncContext() {
    if (!IsReady())
        return;
    if (g_FlushSyncContext) {
#if defined(_MSC_VER) && !defined(CLAYMORE_FAST_SCRIPT_CALLS)
        __try {
            g_FlushSyncContext();
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            std::cerr << "[ScriptInterop] FlushSyncContext threw exception\n";
        }
#else
        g_FlushSyncContext();
#endif
    }
}

// Update managed button events (poll native button state)
inline void UpdateButtons() {
    if (!IsReady())
        return;
    if (g_UpdateButtons) {
#if defined(_MSC_VER) && !defined(CLAYMORE_FAST_SCRIPT_CALLS)
        __try {
            g_UpdateButtons();
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            std::cerr << "[ScriptInterop] UpdateButtons threw exception\n";
        }
#else
        g_UpdateButtons();
#endif
    }
}

// Set a field value on a script instance (with type)
inline void SetField(void* handle, const char* fieldName, int fieldType, void* value) {
    if (handle && g_SetField && fieldName) {
        g_SetField(handle, fieldName, fieldType, value);
    }
}

// Set a managed field value (boxed, type inferred by managed side)
inline void SetManagedField(void* handle, const char* fieldName, void* boxedValue) {
    if (handle && g_SetManagedField && fieldName) {
        g_SetManagedField(handle, fieldName, boxedValue);
    }
}

// Ensure .NET sync context is installed on current thread
inline void EnsureInstalled() {
    if (g_EnsureInstalled) {
        g_EnsureInstalled();
    }
}

inline void UpdateManagedFrame(float dt) {
    if (g_ManagedFrameUpdate) {
#if defined(_MSC_VER) && !defined(CLAYMORE_FAST_SCRIPT_CALLS)
        __try {
            g_ManagedFrameUpdate(dt);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            std::cerr << "[ScriptInterop] UpdateManagedFrame threw exception\n";
        }
#else
        g_ManagedFrameUpdate(dt);
#endif
    }
}

} // namespace script
} // namespace cm

// ============================================================================
// Legacy compatibility - SetManagedFieldPtr, GetManagedFieldPtr and EnsureInstalledPtr
// for Scene.cpp RuntimeClone and sync context
// Defined in: ScriptInterop.cpp (set by DotNetHost or RuntimeHost at startup)
// ============================================================================
extern void (*SetManagedFieldPtr)(void* handle, const char* fieldName, void* boxedValue);
// GetManagedField: returns true if successful, writes boxed value to boxedOut
// boxedOut must point to allocated memory large enough for the value type
// For strings/lists, boxedOut should be IntPtr* to receive the string pointer (caller must free)
// NOTE: GetManagedFieldPtr is only set up in editor builds (DotNetHost), not runtime (RuntimeHost)
// This is intentional - runtime builds don't need inspector reflection
extern bool (*GetManagedFieldPtr)(void* handle, const char* fieldName, int propertyType, void* boxedOut);

#ifdef _WIN32
extern void (__stdcall *EnsureInstalledPtr)();
#else
extern void (*EnsureInstalledPtr)();
#endif

