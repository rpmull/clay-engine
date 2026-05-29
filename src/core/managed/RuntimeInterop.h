#pragma once

#include "hostfxr.h"
#include "coreclr_delegates.h"
#include <string>

namespace cm {
namespace runtime {

// Set up all managed interop systems
// Call this after .NET runtime is initialized
bool SetupAllInterop(load_assembly_and_get_function_pointer_fn loader, const std::wstring& engineDll);

// Individual setup functions (called by SetupAllInterop)
void SetupRuntimeInteropLoader(load_assembly_and_get_function_pointer_fn loader, const std::wstring& engineDll);
bool SetupEntityInterop();
bool SetupNetworkingInterop();
bool SetupInputInterop();
bool SetupPhysicsInterop();
bool SetupAreaInterop();
bool SetupPortalInterop();
bool SetupCollisionInterop();
bool SetupNavigationInterop();
bool SetupWorldGraphInterop();
bool SetupIKInterop();
bool SetupLookAtInterop();
bool SetupRagdollInterop();
bool SetupScriptableInterop();
bool SetupPrefabInterop();
bool SetupMeshInterop();
bool SetupMaterialInterop();
bool SetupParticleInterop();
bool SetupResourceInterop();
bool SetupDialogueInterop();
bool SetupAnimationEventInterop();
bool SetupQuestInterop();

} // namespace runtime
} // namespace cm



