#include "core/prefab/PrefabAPI.h"
#include "core/prefab/RuntimePrefabInstantiator.h"
#include "core/ecs/Scene.h"
#include "core/assets/IAssetResolver.h"
#ifndef CLAYMORE_RUNTIME
#include "editor/pipeline/AssetLibrary.h"
#endif
#include <filesystem>

// Native thunk used by managed PrefabInterop to instantiate a prefab by GUID
static int Prefab_InstantiateByGuid(unsigned long long hi, unsigned long long lo)
{
    ClaymoreGUID g{}; g.high = hi; g.low = lo;
    Scene& scene = Scene::Get();
    EntityID id = InstantiatePrefab(g, scene);
    return (int)id;
}

static int Prefab_InstantiateByGuidBlocking(unsigned long long hi, unsigned long long lo)
{
    ClaymoreGUID g{}; g.high = hi; g.low = lo;
    Scene& scene = Scene::Get();
    EntityID id = InstantiatePrefabBlocking(g, scene);
    return (int)id;
}

static int Prefab_InstantiateByGuidWithRoot(unsigned long long hi, unsigned long long lo, int rootEntityId, bool useExistingRoot)
{
    ClaymoreGUID g{}; g.high = hi; g.low = lo;
    Scene& scene = Scene::Get();
    if (!useExistingRoot || rootEntityId <= 0) {
        EntityID id = InstantiatePrefab(g, scene);
        return (int)id;
    }
    EntityID id = InstantiatePrefab(g, scene, static_cast<EntityID>(rootEntityId), true);
    return (int)id;
}

static int Prefab_GetAsyncStatus(int entityId)
{
    Scene& scene = Scene::Get();
    runtime::PrefabAsyncStatus status = runtime::RuntimePrefabInstantiator::GetAsyncStatus(entityId, scene);
    return static_cast<int>(status);
}

static int Prefab_PreloadByGuid(unsigned long long hi, unsigned long long lo)
{
    ClaymoreGUID g{};
    g.high = hi;
    g.low = lo;
    return runtime::RuntimePrefabInstantiator::PreloadByGuid(g) ? 1 : 0;
}

static const char* Prefab_GetAssetNameByGuid(unsigned long long hi, unsigned long long lo)
{
    ClaymoreGUID guid{};
    guid.high = hi;
    guid.low = lo;
    
    if (guid.high == 0 && guid.low == 0) {
        return "";
    }
    
    static thread_local std::string s_nameBuffer;
    s_nameBuffer.clear();
    
#ifndef CLAYMORE_RUNTIME
    // Editor: Use AssetLibrary first
    auto* asset = AssetLibrary::Instance().GetAsset(guid);
    if (asset) {
        s_nameBuffer = asset->name;
        return s_nameBuffer.c_str();
    }
#endif
    
    // Try loading the prefab asset to get its name
    std::string prefabPath;
    IAssetResolver* resolver = Assets::GetResolver();
    if (resolver) {
        prefabPath = resolver->GetPathForGUID(guid);
    }
    
#ifndef CLAYMORE_RUNTIME
    if (prefabPath.empty()) {
        prefabPath = AssetLibrary::Instance().GetPathForGUID(guid);
    }
#endif
    
    if (!prefabPath.empty()) {
        // Try loading the prefab to get its name
        PrefabAsset asset;
        if (PrefabIO::LoadPrefab(prefabPath, asset)) {
            if (!asset.Name.empty()) {
                s_nameBuffer = asset.Name;
                return s_nameBuffer.c_str();
            }
        }
        
        // Fallback to filename if prefab load fails or has no name
        s_nameBuffer = std::filesystem::path(prefabPath).stem().string();
        return s_nameBuffer.c_str();
    }
    
    return "";
}

extern "C" void* Get_Prefab_InstantiateByGuid_Ptr()
{
    return (void*)&Prefab_InstantiateByGuid;
}

extern "C" void* Get_Prefab_InstantiateByGuidBlocking_Ptr()
{
    return (void*)&Prefab_InstantiateByGuidBlocking;
}

extern "C" void* Get_Prefab_InstantiateByGuidWithRoot_Ptr()
{
    return (void*)&Prefab_InstantiateByGuidWithRoot;
}

extern "C" void* Get_Prefab_GetAsyncStatus_Ptr()
{
    return (void*)&Prefab_GetAsyncStatus;
}

extern "C" void* Get_Prefab_GetAssetNameByGuid_Ptr()
{
    return (void*)&Prefab_GetAssetNameByGuid;
}

extern "C" void* Get_Prefab_PreloadByGuid_Ptr()
{
    return (void*)&Prefab_PreloadByGuid;
}

