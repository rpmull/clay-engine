#pragma once
#include <string>
#include <vector>
#include <memory>

struct ModuleHandle {
    void* alc = nullptr;    // collectible AssemblyLoadContext (opaque)
    std::string id;
    std::string path;
    void* instance = nullptr; // managed IClayModule instance handle (opaque)
};

struct NativeAPIs; // defined in interop/ModuleInterop.h
struct ManagedAPIs; // defined in interop/ModuleInterop.h

namespace ModuleLoader {

ModuleHandle* LoadModule(const std::string& path, const NativeAPIs& native, ManagedAPIs* out);
void UnloadModule(ModuleHandle*& h);

// Runtime tracking for editor UI
struct LoadedModuleInfo { std::string id; std::string path; ModuleHandle* handle; };
const std::vector<LoadedModuleInfo>& GetLoaded();

}


