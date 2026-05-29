#include "ResourceInterop.h"
#include "core/resources/ResourceManifest.h"
#include "core/assets/IAssetResolver.h"
#ifdef CLAYMORE_RUNTIME
#include "core/assets/RuntimeAssetResolver.h"
#else
#include "editor/pipeline/AssetLibrary.h"
#endif
#include <string>
#include <cstring>
#include <vector>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <filesystem>

// ============================================================================
// Re-entrant safe string buffers for interop returns
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
    
    // String pool for batch returns (GUIDs, names arrays)
    // Uses rotation to prevent re-entrant corruption
    static constexpr int kNumStringPools = 4;
    
    std::vector<std::string>& GetRotatingStringPool() {
        thread_local std::vector<std::string> s_Pools[kNumStringPools];
        thread_local int s_CurrentPool = 0;
        s_CurrentPool = (s_CurrentPool + 1) % kNumStringPools;
        return s_Pools[s_CurrentPool];
    }
}

// Function pointer exports
Resources_GetResourceCount_fn Resources_GetResourceCountPtr = &Resources_GetResourceCount;
Resources_GetResourceGUIDs_fn Resources_GetResourceGUIDsPtr = &Resources_GetResourceGUIDs;
Resources_GetResourceByName_fn Resources_GetResourceByNamePtr = &Resources_GetResourceByName;
Resources_GetResourceNames_fn Resources_GetResourceNamesPtr = &Resources_GetResourceNames;
Resources_IsInitialized_fn Resources_IsInitializedPtr = &Resources_IsInitialized;
Resources_SubscribeToType_fn Resources_SubscribeToTypePtr = &Resources_SubscribeToType;
Resources_UnsubscribeFromType_fn Resources_UnsubscribeFromTypePtr = &Resources_UnsubscribeFromType;
Resources_HasResourcesFolder_fn Resources_HasResourcesFolderPtr = &Resources_HasResourcesFolder;
Resources_TryDiscoverFolder_fn Resources_TryDiscoverFolderPtr = &Resources_TryDiscoverFolder;
Resources_FlushPendingChanges_fn Resources_FlushPendingChangesPtr = &Resources_FlushPendingChanges;
Resources_GetPendingChangeCount_fn Resources_GetPendingChangeCountPtr = &Resources_GetPendingChangeCount;
Resources_GetPendingChanges_fn Resources_GetPendingChangesPtr = &Resources_GetPendingChanges;
Resources_GetGuidFromPath_fn Resources_GetGuidFromPathPtr = &Resources_GetGuidFromPath;
Resources_GetAssetCountByType_fn Resources_GetAssetCountByTypePtr = &Resources_GetAssetCountByType;
Resources_GetAssetGUIDsByType_fn Resources_GetAssetGUIDsByTypePtr = &Resources_GetAssetGUIDsByType;

static bool StartsWithIgnoreCase(const std::string& value, const std::string& prefix) {
    if (value.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(value[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

static std::string NormalizeResourceVfsPath(const char* path) {
    if (!path) return {};
    std::string normalized = path;
    for (char& c : normalized) {
        if (c == '\\') c = '/';
    }
    while (!normalized.empty() && std::isspace(static_cast<unsigned char>(normalized.front()))) {
        normalized.erase(normalized.begin());
    }
    while (!normalized.empty() && std::isspace(static_cast<unsigned char>(normalized.back()))) {
        normalized.pop_back();
    }
    if (normalized.empty()) return normalized;

    std::string lower = normalized;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    const std::string prefix = "resources/";
    size_t pos = lower.find(prefix);
    if (pos != std::string::npos) {
        normalized = normalized.substr(pos);
    } else if (!StartsWithIgnoreCase(normalized, prefix)) {
        normalized = prefix + normalized;
    }

    while (!normalized.empty() && normalized.front() == '/') {
        normalized.erase(normalized.begin());
    }

    return normalized;
}

static bool IsResourcesPath(const std::string& path) {
    if (path.empty()) return false;
    std::string normalized = path;
    for (char& c : normalized) {
        if (c == '\\') c = '/';
    }
    std::string lower = normalized;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (StartsWithIgnoreCase(normalized, "resources/")) return true;
    return lower.find("/resources/") != std::string::npos;
}

static bool TryBuildResourceVfsPath(const std::filesystem::path& resourcesRoot,
                                    const std::filesystem::path& filePath,
                                    std::string& outVfsPath) {
    std::error_code ec;
    std::filesystem::path rel = std::filesystem::relative(filePath, resourcesRoot, ec);
    if (ec || rel.empty()) return false;
    outVfsPath = "resources/" + rel.generic_string();
    return true;
}

static bool MatchesAssetTypeByPath(const std::string& typeName, const std::string& path) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto dot = lower.rfind('.');
    std::string ext = dot != std::string::npos ? lower.substr(dot) : "";

    if (typeName == "ClaymoreEngine.Texture") {
        return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" ||
               ext == ".bmp" || ext == ".dds" || ext == ".hdr" || ext == ".ktx" || ext == ".ktx2";
    }
    if (typeName == "ClaymoreEngine.Mesh") {
        return ext == ".meshbin";
    }
    if (typeName == "ClaymoreEngine.Prefab") {
        return ext == ".prefab" || ext == ".prefabb";
    }
    if (typeName == "ClaymoreEngine.AudioClip") {
        return ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac";
    }
    return false;
}

#ifndef CLAYMORE_RUNTIME
static bool TryGetAssetTypeFromPath(const std::string& path, AssetType& outType) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto dot = lower.rfind('.');
    std::string ext = dot != std::string::npos ? lower.substr(dot) : "";

    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" ||
        ext == ".dds" || ext == ".hdr" || ext == ".ktx" || ext == ".ktx2") {
        outType = AssetType::Texture;
        return true;
    }
    if (ext == ".meshbin" || ext == ".fbx" || ext == ".gltf" || ext == ".glb" || ext == ".obj") {
        outType = AssetType::Mesh;
        return true;
    }
    if (ext == ".prefab" || ext == ".prefabb") {
        outType = AssetType::Prefab;
        return true;
    }
    if (ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac") {
        outType = AssetType::Audio;
        return true;
    }
    return false;
}

static bool TryGetAssetTypeForManaged(const std::string& typeName, AssetType& outType) {
    if (typeName == "ClaymoreEngine.Texture") { outType = AssetType::Texture; return true; }
    if (typeName == "ClaymoreEngine.Mesh") { outType = AssetType::Mesh; return true; }
    if (typeName == "ClaymoreEngine.Prefab") { outType = AssetType::Prefab; return true; }
    if (typeName == "ClaymoreEngine.AudioClip") { outType = AssetType::Audio; return true; }
    return false;
}
#endif

// Lazy-initialized pending changes storage for polling approach
static std::vector<ResourceChangeEvent>& GetPendingChangesCache() {
    static std::vector<ResourceChangeEvent> s_PendingChangesCache;
    return s_PendingChangesCache;
}

extern "C" {

int Resources_GetResourceCount(const char* typeName) {
    if (!typeName) return 0;
    
    auto resources = ResourceManifest::Get().GetResourcesByType(typeName);
    return static_cast<int>(resources.size());
}

int Resources_GetResourceGUIDs(const char* typeName, char** outGuids, int maxCount) {
    if (!typeName || !outGuids || maxCount <= 0) return 0;
    
    auto resources = ResourceManifest::Get().GetResourcesByType(typeName);
    
    // Clear and resize string pool (rotating for re-entrancy safety)
    auto& pool = GetRotatingStringPool();
    pool.clear();
    
    int count = 0;
    for (const auto* res : resources) {
        if (count >= maxCount) break;
        
        pool.push_back(res->guid.ToString());
        outGuids[count] = const_cast<char*>(pool.back().c_str());
        count++;
    }
    
    return count;
}

const char* Resources_GetResourceByName(const char* typeName, const char* name) {
    if (!typeName || !name) return "";
    
    const ResourceEntry* res = ResourceManifest::Get().GetResource(typeName, name);
    if (!res) {
        std::cout << "[ResourceInterop] Resource not found: " << typeName << "::" << name << std::endl;
        return "";
    }
    
    auto& buffer = GetRotatingStringBuffer();
    buffer = res->guid.ToString();
    return buffer.c_str();
}

int Resources_GetResourceNames(const char* typeName, char** outNames, int maxCount) {
    if (!typeName || !outNames || maxCount <= 0) return 0;
    
    auto resources = ResourceManifest::Get().GetResourcesByType(typeName);
    
    // Clear and resize string pool (rotating for re-entrancy safety)
    auto& pool = GetRotatingStringPool();
    pool.clear();
    
    int count = 0;
    for (const auto* res : resources) {
        if (count >= maxCount) break;
        
        pool.push_back(res->name);
        outNames[count] = const_cast<char*>(pool.back().c_str());
        count++;
    }
    
    return count;
}

bool Resources_IsInitialized() {
    return ResourceManifest::Get().IsInitialized();
}

int Resources_Initialize(const char* projectRoot) {
    if (!projectRoot) return 0;
    
    ResourceManifest::Get().Initialize(projectRoot);
    return ResourceManifest::Get().Scan();
}

void Resources_RefreshFile(const char* path) {
    if (!path) return;
    ResourceManifest::Get().RefreshFile(path);
}

void Resources_RemoveFile(const char* path) {
    if (!path) return;
    ResourceManifest::Get().RemoveFile(path);
}

// ========== Change Notification System ==========

void Resources_SubscribeToType(const char* typeName) {
    if (!typeName) return;
    ResourceManifest::Get().SubscribeToType(typeName);
}

void Resources_UnsubscribeFromType(const char* typeName) {
    if (!typeName) return;
    ResourceManifest::Get().UnsubscribeFromType(typeName);
}

bool Resources_HasResourcesFolder() {
    return ResourceManifest::Get().HasResourcesFolder();
}

bool Resources_TryDiscoverFolder(const char* projectRoot) {
    if (!projectRoot) return false;
    return ResourceManifest::Get().TryDiscoverResourcesFolder(projectRoot);
}

int Resources_FlushPendingChanges() {
    // Flush and capture changes
    ResourceManifest::Get().FlushPendingChanges();
    return 0; // Changes are processed by callbacks
}

int Resources_GetPendingChangeCount() {
    // This will capture changes when we flush
    return static_cast<int>(GetPendingChangesCache().size());
}

int Resources_GetPendingChanges(
    int* outTypes,
    char** outTypeNames,
    char** outNames,
    char** outGuids,
    int maxCount) 
{
    if (!outTypes || !outTypeNames || !outNames || !outGuids || maxCount <= 0) return 0;
    
    // Clear string pool for this call (rotating for re-entrancy safety)
    auto& pool = GetRotatingStringPool();
    pool.clear();
    
    auto& pendingCache = GetPendingChangesCache();
    int count = 0;
    for (const auto& change : pendingCache) {
        if (count >= maxCount) break;
        
        outTypes[count] = static_cast<int>(change.type);
        
        pool.push_back(change.typeName);
        outTypeNames[count] = const_cast<char*>(pool.back().c_str());
        
        pool.push_back(change.name);
        outNames[count] = const_cast<char*>(pool.back().c_str());
        
        pool.push_back(change.guid.ToString());
        outGuids[count] = const_cast<char*>(pool.back().c_str());
        
        count++;
    }
    
    // Clear cached changes after returning them
    pendingCache.clear();
    
    return count;
}

const char* Resources_GetGuidFromPath(const char* vfsPath) {
    std::string normalized = NormalizeResourceVfsPath(vfsPath);
    if (normalized.empty()) return "";

    ClaymoreGUID guid{};
    if (IAssetResolver* resolver = Assets::GetResolver()) {
        guid = resolver->GetGUID(normalized);
    }

#ifndef CLAYMORE_RUNTIME
    if (guid == ClaymoreGUID()) {
        guid = AssetLibrary::Instance().GetGUIDForPath(normalized);
    }
#endif

    if (guid == ClaymoreGUID()) {
        guid = ResourceManifest::DeterministicGuidFromPath(normalized);
    }

#ifndef CLAYMORE_RUNTIME
    if (guid != ClaymoreGUID()) {
        AssetType assetType;
        if (TryGetAssetTypeFromPath(normalized, assetType)) {
            std::string name = std::filesystem::path(normalized).stem().string();
            AssetReference ref(guid, 0, static_cast<int>(assetType));
            AssetLibrary::Instance().RegisterAsset(ref, assetType, normalized, name);
        }
    }
#endif

    if (guid == ClaymoreGUID()) return "";

    auto& buffer = GetRotatingStringBuffer();
    buffer = guid.ToString();
    return buffer.c_str();
}

int Resources_GetAssetCountByType(const char* typeName) {
    if (!typeName) return 0;
    std::vector<ClaymoreGUID> guids;

#ifndef CLAYMORE_RUNTIME
    AssetType assetType;
    if (!TryGetAssetTypeForManaged(typeName, assetType)) return 0;

    if (ResourceManifest::Get().HasResourcesFolder()) {
        const auto& resourcesRoot = ResourceManifest::Get().GetResourcesPath();
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(resourcesRoot, ec), end; it != end; it.increment(ec)) {
            if (ec || !it->is_regular_file(ec)) continue;
            std::string vfsPath;
            if (!TryBuildResourceVfsPath(resourcesRoot, it->path(), vfsPath)) continue;
            if (!IsResourcesPath(vfsPath)) continue;

            AssetType pathType;
            if (!TryGetAssetTypeFromPath(vfsPath, pathType) || pathType != assetType) continue;

            ClaymoreGUID guid = ResourceManifest::DeterministicGuidFromPath(vfsPath);
            if (guid == ClaymoreGUID()) continue;

            std::string name = it->path().stem().string();
            AssetReference ref(guid, 0, static_cast<int>(assetType));
            AssetLibrary::Instance().RegisterAsset(ref, assetType, vfsPath, name);
            guids.push_back(guid);
        }
    }
#else
    if (auto* resolver = dynamic_cast<RuntimeAssetResolver*>(Assets::GetResolver())) {
        std::vector<std::pair<ClaymoreGUID, std::string>> entries;
        resolver->GetAssetsByPathPrefix("resources/", entries);
        for (const auto& [guid, path] : entries) {
            if (guid == ClaymoreGUID()) continue;
            if (MatchesAssetTypeByPath(typeName, path)) {
                guids.push_back(guid);
            }
        }
    }
#endif

    return static_cast<int>(guids.size());
}

int Resources_GetAssetGUIDsByType(const char* typeName, char** outGuids, int maxCount) {
    if (!typeName || !outGuids || maxCount <= 0) return 0;

    std::vector<ClaymoreGUID> guids;

#ifndef CLAYMORE_RUNTIME
    AssetType assetType;
    if (!TryGetAssetTypeForManaged(typeName, assetType)) return 0;

    if (ResourceManifest::Get().HasResourcesFolder()) {
        const auto& resourcesRoot = ResourceManifest::Get().GetResourcesPath();
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(resourcesRoot, ec), end; it != end; it.increment(ec)) {
            if (ec || !it->is_regular_file(ec)) continue;
            std::string vfsPath;
            if (!TryBuildResourceVfsPath(resourcesRoot, it->path(), vfsPath)) continue;
            if (!IsResourcesPath(vfsPath)) continue;

            AssetType pathType;
            if (!TryGetAssetTypeFromPath(vfsPath, pathType) || pathType != assetType) continue;

            ClaymoreGUID guid = ResourceManifest::DeterministicGuidFromPath(vfsPath);
            if (guid == ClaymoreGUID()) continue;

            std::string name = it->path().stem().string();
            AssetReference ref(guid, 0, static_cast<int>(assetType));
            AssetLibrary::Instance().RegisterAsset(ref, assetType, vfsPath, name);
            guids.push_back(guid);
        }
    }
#else
    if (auto* resolver = dynamic_cast<RuntimeAssetResolver*>(Assets::GetResolver())) {
        std::vector<std::pair<ClaymoreGUID, std::string>> entries;
        resolver->GetAssetsByPathPrefix("resources/", entries);
        for (const auto& [guid, path] : entries) {
            if (guid == ClaymoreGUID()) continue;
            if (MatchesAssetTypeByPath(typeName, path)) {
                guids.push_back(guid);
            }
        }
    }
#endif

    auto& pool = GetRotatingStringPool();
    pool.clear();

    int count = 0;
    for (const auto& guid : guids) {
        if (count >= maxCount) break;
        pool.push_back(guid.ToString());
        outGuids[count] = const_cast<char*>(pool.back().c_str());
        count++;
    }

    return count;
}

} // extern "C"

// Callback registration to capture changes for polling
static uint32_t s_ChangeCallbackHandle = 0;

void RegisterResourceChangeCallback() {
    if (s_ChangeCallbackHandle == 0) {
        s_ChangeCallbackHandle = ResourceManifest::Get().RegisterChangeCallback(
            [](const std::vector<ResourceChangeEvent>& events) {
                auto& cache = GetPendingChangesCache();
                cache.insert(cache.end(), events.begin(), events.end());
            });
        std::cout << "[ResourceInterop] Registered change callback." << std::endl;
    }
}

void UnregisterResourceChangeCallback() {
    if (s_ChangeCallbackHandle != 0) {
        ResourceManifest::Get().UnregisterChangeCallback(s_ChangeCallbackHandle);
        s_ChangeCallbackHandle = 0;
    }
}

