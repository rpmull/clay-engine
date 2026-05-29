#pragma once

#include "coreclr_delegates.h"
#include <string>

namespace cm {
namespace multiplayer {

bool InitializeManagedBridge(load_assembly_and_get_function_pointer_fn loader,
                             const std::wstring& engineDllPath);
void ShutdownManagedBridge();
void PreUpdate(float dt);
void PostUpdate(float dt);
bool IsManagedBridgeReady();

} // namespace multiplayer
} // namespace cm
