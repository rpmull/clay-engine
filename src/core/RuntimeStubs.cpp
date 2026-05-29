// RuntimeStubs.cpp - Stub implementations for editor-only symbols
// These are referenced by shared code but not actually needed at runtime

#include <iostream>
#include <string>
#include <vector>
#include <filesystem>

// Forward declarations from editor headers (avoid including editor headers)
struct InteropComponentDesc;

struct NativeAPIs {
    bool (*RegisterComponent)(const InteropComponentDesc&);
};

struct ManagedAPIs {
    void (*EnumerateComponents)(void* user);
};

struct ModuleHandle {
    void* alc = nullptr;
    std::string id;
    std::string path;
    void* instance = nullptr;
};

//==============================================================================
// ModuleLoader stubs - runtime doesn't support dynamic module loading
//==============================================================================

namespace ModuleLoader {

struct LoadedModuleInfo { std::string id; std::string path; ModuleHandle* handle; };
static std::vector<LoadedModuleInfo> s_EmptyModules;

ModuleHandle* LoadModule(const std::string& path, const NativeAPIs& /*native*/, ManagedAPIs* /*out*/) {
    std::cerr << "[Runtime] ModuleLoader::LoadModule not supported at runtime: " << path << std::endl;
    return nullptr;
}

void UnloadModule(ModuleHandle*& h) {
    h = nullptr;
}

const std::vector<LoadedModuleInfo>& GetLoaded() {
    return s_EmptyModules;
}

} // namespace ModuleLoader

//==============================================================================
// FillModuleNativeAPIs stub
//==============================================================================

void FillModuleNativeAPIs(NativeAPIs& out) {
    out.RegisterComponent = nullptr;
}

//==============================================================================
// g_ClearComponentCaches - runtime uses ScriptInterop, not DotNetHost
//==============================================================================

using ClearComponentCaches_fn = void (*)();
ClearComponentCaches_fn g_ClearComponentCaches = nullptr;

//==============================================================================
// InstantiatePrefab - Runtime prefab instantiation using RuntimePrefabInstantiator
// Only needed for runtime builds; editor builds use PrefabAPI.cpp implementations
//==============================================================================

#ifndef CLAYMORE_EDITOR
#include "core/assets/AssetReference.h"
#include "core/assets/IAssetResolver.h"
#include "core/prefab/PrefabAPI.h"
#include "core/prefab/PrefabBinaryLoader.h"
#include "core/prefab/RuntimePrefabInstantiator.h"
#include "core/ecs/Scene.h"
#include "core/vfs/FileSystem.h"

#include <nlohmann/json.hpp>

using EntityID = uint32_t;

namespace {

std::string ResolvePrefabPath(const std::string& prefabPath) {
    if (prefabPath.empty()) {
        return {};
    }

    if (FileSystem::Instance().Exists(prefabPath)) {
        return prefabPath;
    }

    const std::filesystem::path projectRoot = FileSystem::Instance().GetProjectRoot();
    if (!projectRoot.empty()) {
        const std::string rootedPath = (projectRoot / prefabPath).string();
        if (FileSystem::Instance().Exists(rootedPath)) {
            return rootedPath;
        }
    }

    return prefabPath;
}

std::string ResolvePrefabBinaryPath(const ClaymoreGUID& prefabGuid) {
    IAssetResolver* resolver = Assets::GetResolver();
    if (!resolver) {
        return {};
    }

    std::string path = resolver->GetPathForGUID(prefabGuid);
    if (path.empty()) {
        return {};
    }

    if (resolver->ShouldLoadBinary()) {
        const std::string binaryPath = resolver->GetBinaryPath(path);
        if (!binaryPath.empty()) {
            return binaryPath;
        }
    }

    std::filesystem::path binaryPath(path);
    if (binaryPath.extension() != ".prefabb") {
        binaryPath.replace_extension(".prefabb");
    }

    return binaryPath.string();
}

bool LoadPrefabJson(const std::string& resolvedPath, PrefabAsset& out) {
    std::string jsonText;
    if (!FileSystem::Instance().ReadTextFile(resolvedPath, jsonText)) {
        std::cerr << "[PrefabIO] Cannot read prefab from VFS: " << resolvedPath << std::endl;
        return false;
    }

    nlohmann::json jsonRoot;
    try {
        jsonRoot = nlohmann::json::parse(jsonText);
    } catch (const std::exception& e) {
        std::cerr << "[PrefabIO] JSON parse error in " << resolvedPath << ": " << e.what() << std::endl;
        return false;
    }

    try {
        out.Raw = jsonRoot;
        if (jsonRoot.contains("guid")) {
            jsonRoot.at("guid").get_to(out.Guid);
        }
        out.Name = jsonRoot.value("name", "");
        if (jsonRoot.contains("rootGuid")) {
            jsonRoot.at("rootGuid").get_to(out.RootGuid);
        } else if (jsonRoot.contains("root")) {
            jsonRoot.at("root").get_to(out.RootGuid);
        }
        if (jsonRoot.contains("entities") && jsonRoot["entities"].is_array()) {
            out.Entities = jsonRoot["entities"];
        } else {
            out.Entities = nlohmann::json::array();
        }
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[PrefabIO] Error loading prefab: " << e.what() << std::endl;
        return false;
    }
}

} // namespace

EntityID InstantiatePrefab(const ClaymoreGUID& prefabGuid, Scene& dst, EntityID existingRoot, bool useExistingRoot) {
    return runtime::RuntimePrefabInstantiator::InstantiateByGuid(prefabGuid, dst, existingRoot, useExistingRoot);
}

EntityID InstantiatePrefabFromPath(const std::string& prefabPath, Scene& dst, EntityID existingRoot, bool useExistingRoot) {
    return runtime::RuntimePrefabInstantiator::Instantiate(prefabPath, dst, existingRoot, useExistingRoot);
}

EntityID InstantiatePrefabBlocking(const ClaymoreGUID& prefabGuid, Scene& dst, EntityID existingRoot, bool useExistingRoot) {
    const std::string prefabPath = ResolvePrefabBinaryPath(prefabGuid);
    if (prefabPath.empty()) {
        return INVALID_ENTITY_ID;
    }

    return runtime::RuntimePrefabInstantiator::InstantiateBlocking(prefabPath, dst, existingRoot, useExistingRoot);
}

EntityID InstantiatePrefabFromPathBlocking(const std::string& prefabPath, Scene& dst, EntityID existingRoot, bool useExistingRoot) {
    return runtime::RuntimePrefabInstantiator::InstantiateBlocking(prefabPath, dst, existingRoot, useExistingRoot);
}

EntityID InstantiatePrefabAsset(const PrefabAsset& asset, Scene& dst, EntityID existingRoot, bool useExistingRoot, const char* /*prefabPathForInstance*/) {
    return runtime::RuntimePrefabInstantiator::InstantiateFromAsset(asset, dst, existingRoot, useExistingRoot);
}

namespace PrefabIO {

bool LoadPrefab(const std::string& path, PrefabAsset& out) {
    const std::string resolvedPath = ResolvePrefabPath(path);
    const std::string extension = std::filesystem::path(resolvedPath).extension().string();

    if (extension == ".prefabb") {
        return binary::PrefabBinaryLoader::Load(resolvedPath, out);
    }

    if (Assets::ShouldLoadBinary()) {
        const std::string binaryPath = Assets::GetBinaryPath(resolvedPath);
        if (!binaryPath.empty() && binary::PrefabBinaryLoader::Load(binaryPath, out)) {
            return true;
        }
    }

    if (Assets::ShouldLoadBinary() && !Assets::AllowSourceFallback()) {
        std::cerr << "[PrefabIO] Missing binary prefab (binary-only mode): " << path << std::endl;
        return false;
    }

    return LoadPrefabJson(resolvedPath, out);
}

bool SavePrefab(const std::string& path, const PrefabAsset& /*in*/) {
    std::cerr << "[PrefabIO] SavePrefab is not supported in runtime builds: " << path << std::endl;
    return false;
}

} // namespace PrefabIO
#endif // !CLAYMORE_EDITOR

//==============================================================================
// NOTE: Scriptable, Mesh, and Prefab interop implementations are now in:
//   - src/managed/interop/ScriptableInterop.cpp
//   - src/managed/interop/MeshInterop.cpp
//   - src/managed/interop/PrefabInterop.cpp
// These files have #ifdef CLAYMORE_RUNTIME blocks with real implementations.
//==============================================================================

//==============================================================================
// ModuleComponent Interop Function Pointers (stubs)
//==============================================================================

// Include the header to get the exact type definitions
#include "managed/interop/DotNetHost.h"

// Stub implementations
static bool StubHasModuleComponent(int, const char*) { return false; }
static void StubAddModuleComponent(int, const char*) {}
static void StubRemoveModuleComponent(int, const char*) {}
static void* StubGetModuleComponent(int, const char*) { return nullptr; }
static void* StubGetModuleComponentByFullName(int, const char*) { return nullptr; }
static bool StubGetModuleFieldBool(int, const char*, const char*) { return false; }
static int StubGetModuleFieldInt(int, const char*, const char*) { return 0; }
static long long StubGetModuleFieldInt64(int, const char*, const char*) { return 0; }
static float StubGetModuleFieldFloat(int, const char*, const char*) { return 0.0f; }
static double StubGetModuleFieldDouble(int, const char*, const char*) { return 0.0; }
static const char* StubGetModuleFieldString(int, const char*, const char*) { return ""; }
static void StubGetModuleFieldVec2(int, const char*, const char*, float*, float*) {}
static void StubGetModuleFieldVec3(int, const char*, const char*, float*, float*, float*) {}
static void StubGetModuleFieldVec4(int, const char*, const char*, float*, float*, float*, float*) {}
static void StubGetModuleFieldQuat(int, const char*, const char*, float*, float*, float*, float*) {}
static void StubSetModuleFieldBool(int, const char*, const char*, bool) {}
static void StubSetModuleFieldInt(int, const char*, const char*, int) {}
static void StubSetModuleFieldInt64(int, const char*, const char*, long long) {}
static void StubSetModuleFieldFloat(int, const char*, const char*, float) {}
static void StubSetModuleFieldDouble(int, const char*, const char*, double) {}
static void StubSetModuleFieldString(int, const char*, const char*, const char*) {}
static void StubSetModuleFieldVec2(int, const char*, const char*, float, float) {}
static void StubSetModuleFieldVec3(int, const char*, const char*, float, float, float) {}
static void StubSetModuleFieldVec4(int, const char*, const char*, float, float, float, float) {}
static void StubSetModuleFieldQuat(int, const char*, const char*, float, float, float, float) {}

// Define the global function pointers using the typedefs from DotNetHost.h
HasModuleComponent_fn HasModuleComponentPtr = &StubHasModuleComponent;
AddModuleComponent_fn AddModuleComponentPtr = &StubAddModuleComponent;
RemoveModuleComponent_fn RemoveModuleComponentPtr = &StubRemoveModuleComponent;
GetModuleComponent_fn GetModuleComponentPtr = &StubGetModuleComponent;
GetModuleComponentByFullName_fn GetModuleComponentByFullNamePtr = &StubGetModuleComponentByFullName;
GetModuleFieldBool_fn GetModuleFieldBoolPtr = &StubGetModuleFieldBool;
GetModuleFieldInt_fn GetModuleFieldIntPtr = &StubGetModuleFieldInt;
GetModuleFieldInt64_fn GetModuleFieldInt64Ptr = &StubGetModuleFieldInt64;
GetModuleFieldFloat_fn GetModuleFieldFloatPtr = &StubGetModuleFieldFloat;
GetModuleFieldDouble_fn GetModuleFieldDoublePtr = &StubGetModuleFieldDouble;
GetModuleFieldString_fn GetModuleFieldStringPtr = &StubGetModuleFieldString;
GetModuleFieldVec2_fn GetModuleFieldVec2Ptr = &StubGetModuleFieldVec2;
GetModuleFieldVec3_fn GetModuleFieldVec3Ptr = &StubGetModuleFieldVec3;
GetModuleFieldVec4_fn GetModuleFieldVec4Ptr = &StubGetModuleFieldVec4;
GetModuleFieldQuat_fn GetModuleFieldQuatPtr = &StubGetModuleFieldQuat;
SetModuleFieldBool_fn SetModuleFieldBoolPtr = &StubSetModuleFieldBool;
SetModuleFieldInt_fn SetModuleFieldIntPtr = &StubSetModuleFieldInt;
SetModuleFieldInt64_fn SetModuleFieldInt64Ptr = &StubSetModuleFieldInt64;
SetModuleFieldFloat_fn SetModuleFieldFloatPtr = &StubSetModuleFieldFloat;
SetModuleFieldDouble_fn SetModuleFieldDoublePtr = &StubSetModuleFieldDouble;
SetModuleFieldString_fn SetModuleFieldStringPtr = &StubSetModuleFieldString;
SetModuleFieldVec2_fn SetModuleFieldVec2Ptr = &StubSetModuleFieldVec2;
SetModuleFieldVec3_fn SetModuleFieldVec3Ptr = &StubSetModuleFieldVec3;
SetModuleFieldVec4_fn SetModuleFieldVec4Ptr = &StubSetModuleFieldVec4;
SetModuleFieldQuat_fn SetModuleFieldQuatPtr = &StubSetModuleFieldQuat;

