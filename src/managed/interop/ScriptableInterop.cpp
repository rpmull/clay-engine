#include "ScriptableInterop.h"
#include "DotNetHost.h"
#include "assets/ScriptableRegistry.h"
#include "core/assets/IAssetResolver.h"
#include "core/vfs/FileSystem.h"
#ifndef CLAYMORE_RUNTIME
#include "editor/pipeline/AssetLibrary.h"
#include "editor/Project.h"
#endif
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <windows.h>

using json = nlohmann::json;

static ManagedScriptableAPI g_Managed{};
static std::mutex g_InitMutex;

// Function pointer for invalidating clay object cache (resolved from C#)
using InvalidateCacheFn = void (*)(const char* guidStr);
InvalidateCacheFn g_InvalidateCache = nullptr;

static FieldDesc ParseFieldDescJson(const json& jf);

// Native callbacks exposed to managed
// Helper to split pipe-separated string into vector
static std::vector<std::string> SplitPipe(const char* str) {
    std::vector<std::string> result;
    if (!str || !*str) return result;
    std::string s(str);
    size_t start = 0, end;
    while ((end = s.find('|', start)) != std::string::npos) {
        result.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    result.push_back(s.substr(start));
    return result;
}

// Helper to split pipe-separated integers
static std::vector<int> SplitPipeInt(const char* str) {
    std::vector<int> result;
    if (!str || !*str) return result;
    auto strs = SplitPipe(str);
    result.reserve(strs.size());
    for (const auto& s : strs) {
        try { result.push_back(std::stoi(s)); } catch (...) { result.push_back(0); }
    }
    return result;
}

bool Scriptable_NativeRegisterType(const ScriptableTypeDescInterop& interop) {
    // Convert from blittable interop struct to real ScriptableTypeDesc
    ScriptableTypeDesc d;
    d.id = interop.id;
    d.fullName = interop.fullName ? interop.fullName : "";
    d.menuPath = interop.menuPath ? interop.menuPath : "";
    d.defaultFile = interop.defaultFile ? interop.defaultFile : "";
    d.order = interop.order;
    d.version = interop.version;
    d.CreateNative = reinterpret_cast<void*(*)()>(interop.CreateNative);
    
    // Convert field descriptors
    if (interop.fields && interop.fieldCount > 0) {
        d.fields.reserve(interop.fieldCount);
        for (int i = 0; i < interop.fieldCount; ++i) {
            const FieldDescInterop& fi = interop.fields[i];
            FieldDesc fd;
            fd.name = fi.name ? fi.name : "";
            fd.type = static_cast<ValueType>(fi.type);
            fd.flags = fi.flags;
            fd.arrayRank = fi.arrayRank;
            fd.enumType = fi.enumType ? fi.enumType : "";
            
            // Parse enum metadata if present
            if (fd.type == ValueType::Enum && fi.enumNames && fi.enumValues) {
                fd.enumNames = SplitPipe(fi.enumNames);
                fd.enumValues = SplitPipeInt(fi.enumValues);
            }

            // Extended metadata
            fd.listElementType = static_cast<ValueType>(fi.listElementType);
            fd.listElementTypeName = fi.listElementTypeName ? fi.listElementTypeName : "";
            fd.auxType = fi.auxType ? fi.auxType : "";
            fd.structFieldsJson = fi.structFieldsJson ? fi.structFieldsJson : "";

            if (fi.populateFromResources) fd.flags |= FieldFlags::PopulateFromResources;
            if (fi.selectFromResources) fd.flags |= FieldFlags::SelectFromResources;

            fd.showIfField = fi.showIfField ? fi.showIfField : "";
            fd.showIfValue = fi.showIfValue ? fi.showIfValue : "";
            fd.showIfMode = fi.showIfMode;

            // Parse nested struct fields if provided
            if (!fd.structFieldsJson.empty()) {
                try {
                    json parsed = json::parse(fd.structFieldsJson);
                    if (parsed.is_array()) {
                        for (const auto& jf : parsed) {
                            fd.structFields.push_back(ParseFieldDescJson(jf));
                        }
                    }
                } catch (const std::exception& ex) {
                    std::cerr << "[ScriptableInterop] Failed to parse structFieldsJson for field " << fd.name
                              << ": " << ex.what() << std::endl;
                }
            }
            d.fields.push_back(fd);
        }
    }
    
    return ScriptableTypeRegistry::Get().Register(d);
}

// Convert JSON field description (produced by managed side) into FieldDesc
static FieldDesc ParseFieldDescJson(const json& jf) {
    FieldDesc fd;
    fd.name = jf.value("name", std::string());
    fd.type = static_cast<ValueType>(jf.value("type", 0));
    fd.flags = jf.value("flags", (uint32_t)FieldFlags::Serialized);
    fd.arrayRank = jf.value("arrayRank", 0);

    // Tolerate nulls by only reading strings when present and string-typed
    if (jf.contains("enumType") && jf["enumType"].is_string()) fd.enumType = jf["enumType"].get<std::string>();
    if (jf.contains("enumNames") && jf["enumNames"].is_array()) {
        for (const auto& n : jf["enumNames"]) fd.enumNames.push_back(n.get<std::string>());
    }
    if (jf.contains("enumValues") && jf["enumValues"].is_array()) {
        for (const auto& v : jf["enumValues"]) fd.enumValues.push_back(v.get<int>());
    }

    fd.listElementType = static_cast<ValueType>(jf.value("listElementType", 0));
    if (jf.contains("listElementTypeName") && jf["listElementTypeName"].is_string())
        fd.listElementTypeName = jf["listElementTypeName"].get<std::string>();

    if (jf.contains("auxType") && jf["auxType"].is_string())
        fd.auxType = jf["auxType"].get<std::string>();

    if (jf.contains("structFieldsJson") && jf["structFieldsJson"].is_string())
        fd.structFieldsJson = jf["structFieldsJson"].get<std::string>();

    if (jf.value("populateFromResources", false)) fd.flags |= FieldFlags::PopulateFromResources;
    if (jf.value("selectFromResources", false)) fd.flags |= FieldFlags::SelectFromResources;

    if (jf.contains("structFields") && jf["structFields"].is_array()) {
        for (const auto& sub : jf["structFields"]) {
            fd.structFields.push_back(ParseFieldDescJson(sub));
        }
    }

    return fd;
}

bool Scriptable_NativeSetField(void* nativeInstance, const char* field, const ManagedValue& mv) {
    if (!nativeInstance || !field) return false;
    auto* so = reinterpret_cast<ScriptableObject*>(nativeInstance);
    Variant v;
    switch ((ValueType)mv.type) {
        case ValueType::Bool:   v.value = (*(uint8_t*)mv.data) != 0; break;
        case ValueType::Int32:  v.value = *(int32_t*)mv.data; break;
        case ValueType::Int64:  v.value = *(int64_t*)mv.data; break;
        case ValueType::Float:  v.value = *(float*)mv.data; break;
        case ValueType::Double: v.value = *(double*)mv.data; break;
        case ValueType::String: v.value = std::string((const char*)mv.data); break;
        default: break;
    }
    return so->SetField(field, v);
}

bool Scriptable_NativeGetField(void* nativeInstance, const char* field, ManagedValue* out) {
    if (!nativeInstance || !field || !out) return false;
    auto* so = reinterpret_cast<ScriptableObject*>(nativeInstance);
    Variant v; if (!so->GetField(field, v)) return false;
    // Minimal: only numbers/bools/strings
    if (auto p = std::get_if<bool>(&v.value)) { out->type = (uint32_t)ValueType::Bool; out->isArray = 0; out->data = (void*)p; out->count = 1; return true; }
    if (auto p = std::get_if<int32_t>(&v.value)) { out->type = (uint32_t)ValueType::Int32; out->isArray = 0; out->data = (void*)p; out->count = 1; return true; }
    if (auto p = std::get_if<int64_t>(&v.value)) { out->type = (uint32_t)ValueType::Int64; out->isArray = 0; out->data = (void*)p; out->count = 1; return true; }
    if (auto p = std::get_if<float>(&v.value)) { out->type = (uint32_t)ValueType::Float; out->isArray = 0; out->data = (void*)p; out->count = 1; return true; }
    if (auto p = std::get_if<double>(&v.value)) { out->type = (uint32_t)ValueType::Double; out->isArray = 0; out->data = (void*)p; out->count = 1; return true; }
    if (auto p = std::get_if<std::string>(&v.value)) { out->type = (uint32_t)ValueType::String; out->isArray = 0; out->data = (void*)p->c_str(); out->count = (uint32_t)p->size(); return true; }
    return false;
}

bool Scriptable_NativeMarkDirty(void* nativeInstance) {
    auto* so = reinterpret_cast<ScriptableObject*>(nativeInstance);
    so->NotifyChanged();
    return true;
}

void Scriptable_SetManagedAPI(const ManagedScriptableAPI& api) {
    std::lock_guard<std::mutex> lk(g_InitMutex);
    g_Managed = api;
}

const ManagedScriptableAPI* GetManagedScriptable() { return g_Managed.EnumerateTypes ? &g_Managed : nullptr; }

// ============================================================================
// Re-entrant safe string buffer for interop returns
// Uses rotating thread-local buffers to handle nested calls safely
// ============================================================================
namespace {
    static constexpr int kNumStringBuffers = 8;
    
    std::string& GetRotatingStringBuffer() {
        thread_local std::string s_Buffers[kNumStringBuffers];
        thread_local int s_CurrentBuffer = 0;
        s_CurrentBuffer = (s_CurrentBuffer + 1) % kNumStringBuffers;
        return s_Buffers[s_CurrentBuffer];
    }
}

// Helper to get project root directory (works in both editor and runtime)
static std::filesystem::path GetProjectRoot() {
#ifndef CLAYMORE_RUNTIME
    return Project::GetProjectDirectory();
#else
    return FileSystem::Instance().GetProjectRoot();
#endif
}

const char* Scriptable_GetPathForGUID(const char* guidStr) {
    if (!guidStr || !*guidStr) return nullptr;
    
    ClaymoreGUID guid = ClaymoreGUID::FromString(guidStr);
    if (guid.high == 0 && guid.low == 0) return nullptr;
    
    std::string virtualPath;
    
    // Try IAssetResolver first (works in both editor and runtime)
    IAssetResolver* resolver = Assets::GetResolver();
    if (resolver) {
        virtualPath = resolver->GetPathForGUID(guid);
    }
    
#ifndef CLAYMORE_RUNTIME
    // Editor fallback: also try AssetLibrary
    if (virtualPath.empty()) {
        virtualPath = AssetLibrary::Instance().GetPathForGUID(guid);
    }
#endif
    
    if (virtualPath.empty()) return nullptr;
    
    // Resolve virtual path to absolute filesystem path
    std::string& buf = GetRotatingStringBuffer();
    std::filesystem::path vp(virtualPath);
    if (vp.is_absolute()) {
        // Already absolute, use as-is
        buf = vp.string();
    } else {
        // Combine with project root to get absolute path
        std::filesystem::path projectRoot = GetProjectRoot();
        if (!projectRoot.empty()) {
            std::filesystem::path absPath = projectRoot / vp;
            buf = absPath.string();
        } else {
            // Fallback: try current working directory
            buf = (std::filesystem::current_path() / vp).string();
        }
    }
    
    // Normalize slashes for consistency
    std::replace(buf.begin(), buf.end(), '\\', '/');
    
    return buf.c_str();
}

// Read file contents via VFS (works with PAK files at runtime)
// Caller must free the returned buffer with Scriptable_FreeBuffer

const char* Scriptable_ReadFileContents(const char* path) {
    if (!path || !*path) return nullptr;
    
    std::vector<uint8_t> data;
    if (!FileSystem::Instance().ReadFile(path, data)) {
        std::cerr << "[ScriptableInterop] Failed to read file via VFS: " << path << std::endl;
        return nullptr;
    }
    
    // Store in rotating thread-local buffer and return pointer
    std::string& buf = GetRotatingStringBuffer();
    buf.assign(reinterpret_cast<const char*>(data.data()), data.size());
    return buf.c_str();
}

bool Scriptable_IsTypeAssignable(const char* assetTypeName, const char* expectedTypeName) {
    if (!assetTypeName || !expectedTypeName) return false;
    
    // Exact match
    if (strcmp(assetTypeName, expectedTypeName) == 0) return true;
    
    // For inheritance checking, we need to load the .clayobj and check the type hierarchy
    // This would require managed-side reflection, so for now we'll be permissive
    // and let runtime validation handle mismatches.
    // The managed side can use ClayScriptableObjectLoader.IsTypeAssignable for proper checking.
    
    // Check if expected type is the base ClayScriptableObject (accepts all)
    if (strcmp(expectedTypeName, "ClaymoreEngine.Scripting.Scriptable.ClayScriptableObject") == 0) {
        return true;
    }
    
    // For specific types, defer to managed-side validation
    return true; // Permissive - let managed side validate
}

void Scriptable_InvalidateCache(const char* guidStr) {
    if (!guidStr || !*guidStr) return;
    
    // Call managed side to invalidate the cache for this GUID
    if (g_InvalidateCache) {
        g_InvalidateCache(guidStr);
    } else {
        std::cerr << "[ScriptableInterop] InvalidateCache function pointer not initialized" << std::endl;
    }
}

