#include "core/ecs/Scene.h"
#include "core/assets/AssetReference.h"
#include "core/assets/IAssetResolver.h"
#include "core/serialization/MeshBinaryLoader.h"
#include "core/assets/RuntimeModelInstantiator.h"
#ifndef CLAYMORE_RUNTIME
#include "editor/pipeline/AssetLibrary.h"
#include "editor/Project.h"
#endif
#include "core/rendering/Mesh.h"
#include "core/ecs/Components.h"
#include <iostream>
#include <cstring>
#include <filesystem>

// ============================================================================
// MeshComponent property accessors
// ============================================================================

static bool Mesh_HasComponent(int entityID)
{
    auto* data = Scene::Get().GetEntityData(entityID);
    return data && data->Mesh != nullptr;
}

static void Mesh_GetReference(int entityID, unsigned long long* outHi, unsigned long long* outLo, int* outFileID)
{
    auto* data = Scene::Get().GetEntityData(entityID);
    if (!data || !data->Mesh) {
        if (outHi) *outHi = 0;
        if (outLo) *outLo = 0;
        if (outFileID) *outFileID = 0;
        return;
    }
    if (outHi) *outHi = data->Mesh->meshReference.guid.high;
    if (outLo) *outLo = data->Mesh->meshReference.guid.low;
    if (outFileID) *outFileID = data->Mesh->meshReference.fileID;
}

static int Mesh_GetVertexCount(int entityID)
{
    auto* data = Scene::Get().GetEntityData(entityID);
    if (!data || !data->Mesh || !data->Mesh->mesh) return 0;
    return (int)data->Mesh->mesh->numVertices;
}

static int Mesh_GetIndexCount(int entityID)
{
    auto* data = Scene::Get().GetEntityData(entityID);
    if (!data || !data->Mesh || !data->Mesh->mesh) return 0;
    return (int)data->Mesh->mesh->numIndices;
}

static int Mesh_GetSubmeshCount(int entityID)
{
    auto* data = Scene::Get().GetEntityData(entityID);
    if (!data || !data->Mesh || !data->Mesh->mesh) return 0;
    return (int)data->Mesh->mesh->Submeshes.size();
}

static int Mesh_GetMaterialSlotCount(int entityID)
{
    auto* data = Scene::Get().GetEntityData(entityID);
    if (!data || !data->Mesh) return 0;
    if (!data->Mesh->MaterialSlotNames.empty()) return (int)data->Mesh->MaterialSlotNames.size();
    if (!data->Mesh->materials.empty()) return (int)data->Mesh->materials.size();
    if (data->Mesh->material) return 1;
    return 0;
}

static const char* Mesh_GetMaterialSlotName(int entityID, int slotIndex)
{
    auto* data = Scene::Get().GetEntityData(entityID);
    if (!data || !data->Mesh || slotIndex < 0) return "";

    static thread_local std::string s_slotName;
    s_slotName.clear();

    if (slotIndex < (int)data->Mesh->MaterialSlotNames.size())
    {
        s_slotName = data->Mesh->MaterialSlotNames[slotIndex];
    }

    if (s_slotName.empty() && slotIndex < (int)data->Mesh->materials.size() && data->Mesh->materials[slotIndex])
    {
        s_slotName = data->Mesh->materials[slotIndex]->GetName();
    }

    if (s_slotName.empty() && slotIndex == 0 && data->Mesh->material)
    {
        s_slotName = data->Mesh->material->GetName();
    }

    if (s_slotName.empty())
    {
        s_slotName = "Slot " + std::to_string(slotIndex);
    }

    return s_slotName.c_str();
}


static void Mesh_GetBoundsMin(int entityID, float* x, float* y, float* z)
{
    auto* data = Scene::Get().GetEntityData(entityID);
    if (!data || !data->Mesh || !data->Mesh->mesh) {
        if (x) *x = 0; if (y) *y = 0; if (z) *z = 0;
        return;
    }
    const auto& bounds = data->Mesh->mesh->BoundsMin;
    if (x) *x = bounds.x;
    if (y) *y = bounds.y;
    if (z) *z = bounds.z;
}

static void Mesh_GetBoundsMax(int entityID, float* x, float* y, float* z)
{
    auto* data = Scene::Get().GetEntityData(entityID);
    if (!data || !data->Mesh || !data->Mesh->mesh) {
        if (x) *x = 0; if (y) *y = 0; if (z) *z = 0;
        return;
    }
    const auto& bounds = data->Mesh->mesh->BoundsMax;
    if (x) *x = bounds.x;
    if (y) *y = bounds.y;
    if (z) *z = bounds.z;
}

static float Mesh_GetBoundsPadding(int entityID)
{
    auto* data = Scene::Get().GetEntityData(entityID);
    if (!data || !data->Mesh) return 1.0f;
    return data->Mesh->BoundsPadding;
}

static void Mesh_SetBoundsPadding(int entityID, float value)
{
    Scene::Get().SetMeshBoundsPadding(entityID, value);
}

static bool Mesh_GetRenderOnTop(int entityID)
{
    auto* data = Scene::Get().GetEntityData(entityID);
    if (!data || !data->Mesh) return false;
    return data->Mesh->RenderOnTop;
}

static void Mesh_SetRenderOnTop(int entityID, bool value)
{
    auto* data = Scene::Get().GetEntityData(entityID);
    if (!data || !data->Mesh) return;
    data->Mesh->RenderOnTop = value;
}

static bool Mesh_GetShowBackfaces(int entityID)
{
    auto* data = Scene::Get().GetEntityData(entityID);
    if (!data || !data->Mesh) return false;
    return data->Mesh->ShowBackfaces;
}

static void Mesh_SetShowBackfaces(int entityID, bool value)
{
    auto* data = Scene::Get().GetEntityData(entityID);
    if (!data || !data->Mesh) return;
    data->Mesh->ShowBackfaces = value;
}

static bool Mesh_GetSkipFrustumCulling(int entityID)
{
    auto* data = Scene::Get().GetEntityData(entityID);
    if (!data || !data->Mesh) return false;
    return data->Mesh->SkipFrustumCulling;
}

static void Mesh_SetSkipFrustumCulling(int entityID, bool value)
{
    Scene::Get().SetMeshSkipFrustumCulling(entityID, value);
}

static int Mesh_GetRenderOrder(int entityID)
{
    auto* data = Scene::Get().GetEntityData(entityID);
    if (!data || !data->Mesh) return 0;
    return data->Mesh->RenderOrder;
}

static void Mesh_SetRenderOrder(int entityID, int value)
{
    auto* data = Scene::Get().GetEntityData(entityID);
    if (!data || !data->Mesh) return;
    data->Mesh->RenderOrder = value;
}

static bool Mesh_GetUniqueMaterial(int entityID)
{
    auto* data = Scene::Get().GetEntityData(entityID);
    if (!data || !data->Mesh) return false;
    return data->Mesh->UniqueMaterial;
}

static void Mesh_SetUniqueMaterial(int entityID, bool value)
{
    auto* data = Scene::Get().GetEntityData(entityID);
    if (!data || !data->Mesh) return;
    data->Mesh->UniqueMaterial = value;
}

static bool Mesh_HasSkinning(int entityID)
{
    auto* data = Scene::Get().GetEntityData(entityID);
    if (!data || !data->Mesh || !data->Mesh->mesh) return false;
    return data->Mesh->mesh->HasSkinning();
}

static const char* Mesh_GetName(int entityID)
{
    auto* data = Scene::Get().GetEntityData(entityID);
    if (!data || !data->Mesh) return "";
    static thread_local std::string s_nameBuffer;
    s_nameBuffer = data->Mesh->MeshName;
    return s_nameBuffer.c_str();
}

static const char* Mesh_GetAssetNameByGuid(unsigned long long hi, unsigned long long lo)
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
    // Editor: Use AssetLibrary
    auto* asset = AssetLibrary::Instance().GetAsset(guid);
    if (asset) {
        s_nameBuffer = asset->name;
        return s_nameBuffer.c_str();
    }
#endif
    
    // Runtime: Try resolver, then fallback to filename from path
    IAssetResolver* resolver = Assets::GetResolver();
    if (resolver) {
        std::string path = resolver->GetPathForGUID(guid);
        if (!path.empty()) {
            s_nameBuffer = std::filesystem::path(path).stem().string();
            return s_nameBuffer.c_str();
        }
    }
    
    return "";
}

// ============================================================================
// Mesh instantiation and assignment
// ============================================================================

// Native thunk used by managed MeshInterop to instantiate a mesh as an entity
// Returns the entity ID of the newly created entity with a MeshComponent
// For skinned meshes, uses the full model instantiation path to set up skeleton properly
static int Mesh_InstantiateByGuidInternal(unsigned long long hi, unsigned long long lo, int fileID, const char* entityName, int rootEntityId, bool useExistingRoot)
{
    ClaymoreGUID guid{}; 
    guid.high = hi; 
    guid.low = lo;
    
    std::cout << "[MeshInterop] InstantiateByGuid called: GUID=" << guid.ToString() << ", fileID=" << fileID << std::endl;
    
    // Check if GUID is zero
    if (guid.high == 0 && guid.low == 0) {
        std::cerr << "[MeshInterop] ERROR: GUID is zero/invalid!" << std::endl;
        return -1;
    }
    
    Scene& scene = Scene::Get();
    EntityID overrideRoot = (useExistingRoot && rootEntityId > 0) ? static_cast<EntityID>(rootEntityId) : INVALID_ENTITY_ID;
    EntityData* overrideData = (overrideRoot != INVALID_ENTITY_ID) ? scene.GetEntityData(overrideRoot) : nullptr;
    if (overrideRoot != INVALID_ENTITY_ID && !overrideData) {
        overrideRoot = INVALID_ENTITY_ID;
    }
    
    // Get asset path from resolver (works for both editor and runtime)
    std::string assetPath;
    IAssetResolver* resolver = Assets::GetResolver();
    if (resolver) {
        assetPath = resolver->GetPathForGUID(guid);
    }
#ifndef CLAYMORE_RUNTIME
    // Editor fallback: also try AssetLibrary
    if (assetPath.empty()) {
        assetPath = AssetLibrary::Instance().GetPathForGUID(guid);
    }
#endif
    
    std::cout << "[MeshInterop] Asset path for GUID: " << (assetPath.empty() ? "(not found)" : assetPath) << std::endl;
    
    if (assetPath.empty()) {
        std::cerr << "[MeshInterop] ERROR: No asset path registered for GUID!" << std::endl;
        return -1;
    }
    
#ifdef CLAYMORE_RUNTIME
    // Runtime mode: Use RuntimeModelInstantiator for full model hierarchy
    // This provides identical behavior to editor's Scene::InstantiateModel()
    // The scripter doesn't need to know which mode they're in - API is transparent
    
    std::cout << "[MeshInterop] Runtime: Using RuntimeModelInstantiator for full hierarchy" << std::endl;
    
    EntityID rootId = cm::RuntimeModelInstantiator::InstantiateByGuid(guid, scene, glm::vec3(0.0f), overrideRoot);
    if (rootId == (EntityID)-1) {
        // Fallback: If model isn't in registry, try loading single mesh
        // This handles simple meshes that don't need full hierarchy
        std::cout << "[MeshInterop] Runtime: Model not in registry, falling back to single mesh" << std::endl;
        
        std::filesystem::path meshPath(assetPath);
        if (meshPath.extension() != ".meshbin") {
            meshPath.replace_extension(".meshbin");
        }
        
        bool skinned = false;
        std::shared_ptr<Mesh> mesh = MeshBinaryLoader::LoadMesh(meshPath.string(), std::max(0, fileID), &skinned);
        if (!mesh) {
            std::cerr << "[MeshInterop] Runtime: Failed to load mesh from " << meshPath.string() << std::endl;
            return -1;
        }
        
        std::string name = entityName && entityName[0] ? entityName : "MeshEntity";
        EntityID targetId = overrideRoot != INVALID_ENTITY_ID ? overrideRoot : scene.CreateEntity(name).GetID();
        EntityData* data = scene.GetEntityData(targetId);
        if (!data) {
            std::cerr << "[MeshInterop] Failed to create entity" << std::endl;
            return -1;
        }
        if (overrideRoot != INVALID_ENTITY_ID && entityName && entityName[0]) {
            data->Name = name;
        }
        
        AssetReference ref(guid, fileID, 3);
        data->Mesh = std::make_unique<MeshComponent>(mesh, name, nullptr);
        data->Mesh->meshReference = ref;
        
        auto blendShapes = MeshBinaryLoader::LoadBlendShapes(meshPath.string(), std::max(0, fileID));
        if (blendShapes) {
            data->BlendShapes = std::move(blendShapes);
        }
        
        if (skinned) {
            std::cout << "[MeshInterop] WARNING: Skinned mesh loaded without skeleton hierarchy" << std::endl;
        }
        
        return (int)targetId;
    }
    
    // Rename root entity if a name was provided
    if (entityName && entityName[0]) {
        EntityData* rootData = scene.GetEntityData(rootId);
        if (rootData) {
            rootData->Name = entityName;
        }
    }
    
    std::cout << "[MeshInterop] Runtime: Model instantiated with root ID: " << rootId << std::endl;
    return (int)rootId;
    
#else
    // Editor mode: Use full model instantiation path
    
    // Resolve to absolute path for proper cache lookup
    std::string absolutePath = assetPath;
    std::filesystem::path ap(assetPath);
    if (!ap.is_absolute()) {
        std::filesystem::path projectDir = Project::GetProjectDirectory();
        if (!projectDir.empty()) {
            absolutePath = (projectDir / assetPath).string();
        }
    }
    // Normalize slashes
    for (char& c : absolutePath) if (c == '\\') c = '/';
    
    std::cout << "[MeshInterop] Resolved absolute path: " << absolutePath << std::endl;
    
    // Use Scene::InstantiateModel for proper model instantiation (handles skinning, skeleton, etc.)
    // This is the correct way to instantiate FBX/GLB models with skeletal meshes
    std::cout << "[MeshInterop] Using InstantiateModel for proper model setup..." << std::endl;
    EntityID rootId = scene.InstantiateModel(absolutePath, glm::vec3(0.0f), overrideRoot);
    
    if (rootId == INVALID_ENTITY_ID) {
        std::cerr << "[MeshInterop] InstantiateModel failed for: " << absolutePath << std::endl;

        std::string sourceExt = std::filesystem::path(assetPath).extension().string();
        for (char& c : sourceExt) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const bool authoredModelAsset =
            sourceExt == ".fbx" ||
            sourceExt == ".obj" ||
            sourceExt == ".gltf" ||
            sourceExt == ".glb" ||
            sourceExt == ".meta";

        // For authored model assets, failing over to a single placeholder mesh entity
        // causes higher-level gameplay code to treat the spawn as successful and hide
        // the underlying character body. Return invalid instead so callers can keep
        // existing visuals intact.
        if (authoredModelAsset) {
            std::cerr << "[MeshInterop] Refusing placeholder fallback for authored model asset: "
                      << assetPath << std::endl;
            return -1;
        }
        
        // Fallback: try simple mesh creation for non-skinned meshes
        std::cout << "[MeshInterop] Falling back to simple mesh instantiation..." << std::endl;
        
        AssetReference ref(guid, fileID, 3);
        std::shared_ptr<Mesh> mesh = AssetLibrary::Instance().LoadMesh(ref);
        if (!mesh) {
            std::cerr << "[MeshInterop] Fallback also failed - could not load mesh" << std::endl;
            return -1;
        }
        
        std::string name = entityName && entityName[0] ? entityName : "MeshEntity";
        EntityID targetId = overrideRoot != INVALID_ENTITY_ID ? overrideRoot : scene.CreateEntity(name).GetID();
        EntityData* data = scene.GetEntityData(targetId);
        if (!data) {
            std::cerr << "[MeshInterop] Failed to create entity" << std::endl;
            return -1;
        }
        if (overrideRoot != INVALID_ENTITY_ID && entityName && entityName[0]) {
            data->Name = name;
        }
        
        data->Mesh = std::make_unique<MeshComponent>(mesh, name, nullptr);
        data->Mesh->meshReference = ref;
        
        auto blendShapes = AssetLibrary::Instance().LoadMeshBlendShapes(ref);
        if (blendShapes) {
            data->BlendShapes = std::move(blendShapes);
        }
        
        std::cout << "[MeshInterop] Fallback entity created with ID: " << targetId << std::endl;
        return (int)targetId;
    }
    
    // Rename root entity if a name was provided
    if (entityName && entityName[0]) {
        EntityData* rootData = scene.GetEntityData(rootId);
        if (rootData) {
            rootData->Name = entityName;
        }
    }
    
    // Log details about the instantiated model
    EntityData* rootData = scene.GetEntityData(rootId);
    if (rootData) {
        std::cout << "[MeshInterop] Model instantiated: ID=" << rootId 
                  << " Name='" << rootData->Name << "'"
                  << " Children=" << rootData->Children.size() 
                  << " Visible=" << (rootData->Visible ? "true" : "false")
                  << " Active=" << (rootData->Active ? "true" : "false")
                  << std::endl;
        
        // Check if any child has skinning with UseParentSkeleton
        for (EntityID childId : rootData->Children) {
            if (auto* childData = scene.GetEntityData(childId)) {
                std::cout << "[MeshInterop]   Child ID=" << childId << " Name='" << childData->Name << "'";
                if (childData->Skinning) {
                    std::cout << " UseParentSkeleton=" << (childData->Skinning->UseParentSkeleton ? "true" : "false");
                    std::cout << " BoneRemap.size=" << childData->Skinning->BoneRemap.size();
                }
                if (childData->Mesh) {
                    std::cout << " HasMesh=true";
                    if (childData->Mesh->mesh) {
                        std::cout << " verts=" << childData->Mesh->mesh->numVertices;
                    }
                }
                std::cout << " Visible=" << (childData->Visible ? "true" : "false");
                std::cout << " Active=" << (childData->Active ? "true" : "false");
                std::cout << std::endl;
            }
        }
    }
    
    return (int)rootId;
#endif
}

// Legacy signature (no root override)
static int Mesh_InstantiateByGuid(unsigned long long hi, unsigned long long lo, int fileID, const char* entityName)
{
    return Mesh_InstantiateByGuidInternal(hi, lo, fileID, entityName, static_cast<int>(INVALID_ENTITY_ID), false);
}

// Optional root override
static int Mesh_InstantiateByGuidWithRoot(unsigned long long hi, unsigned long long lo, int fileID, const char* entityName, int rootEntityId, bool useExistingRoot)
{
    return Mesh_InstantiateByGuidInternal(hi, lo, fileID, entityName, rootEntityId, useExistingRoot);
}

// Native thunk to set mesh on an existing entity by GUID
// Returns true on success
static bool Mesh_SetByGuid(int entityID, unsigned long long hi, unsigned long long lo, int fileID)
{
    ClaymoreGUID guid{};
    guid.high = hi;
    guid.low = lo;
    
    Scene& scene = Scene::Get();
    EntityData* data = scene.GetEntityData(entityID);
    if (!data) {
        std::cerr << "[MeshInterop] Entity not found: " << entityID << std::endl;
        return false;
    }
    
    AssetReference ref(guid, fileID, 3);
    std::shared_ptr<Mesh> mesh = nullptr;
    
#ifdef CLAYMORE_RUNTIME
    // Runtime: Load from meshbin using VFS
    IAssetResolver* resolver = Assets::GetResolver();
    if (resolver) {
        std::string assetPath = resolver->GetPathForGUID(guid);
        if (!assetPath.empty()) {
            std::filesystem::path meshPath(assetPath);
            if (meshPath.extension() != ".meshbin") {
                meshPath.replace_extension(".meshbin");
            }
            bool skinned = false;
            mesh = MeshBinaryLoader::LoadMesh(meshPath.string(), std::max(0, fileID), &skinned);
        }
    }
#else
    // Editor: Use AssetLibrary
    mesh = AssetLibrary::Instance().LoadMesh(ref);
#endif
    
    if (!mesh) {
        std::cerr << "[MeshInterop] Failed to load mesh for GUID: " << guid.ToString() << std::endl;
        return false;
    }
    
    // Create or update MeshComponent
    if (!data->Mesh) {
        data->Mesh = std::make_unique<MeshComponent>(mesh, data->Name, nullptr);
    } else {
        data->Mesh->mesh = mesh;
    }
    data->Mesh->meshReference = ref;
    
    // Load blend shapes if available
#ifdef CLAYMORE_RUNTIME
    IAssetResolver* resolverBS = Assets::GetResolver();
    if (resolverBS) {
        std::string assetPath = resolverBS->GetPathForGUID(guid);
        if (!assetPath.empty()) {
            std::filesystem::path meshPath(assetPath);
            if (meshPath.extension() != ".meshbin") {
                meshPath.replace_extension(".meshbin");
            }
            auto blendShapes = MeshBinaryLoader::LoadBlendShapes(meshPath.string(), std::max(0, fileID));
            if (blendShapes) {
                data->BlendShapes = std::move(blendShapes);
            }
        }
    }
#else
    auto blendShapes = AssetLibrary::Instance().LoadMeshBlendShapes(ref);
    if (blendShapes) {
        data->BlendShapes = std::move(blendShapes);
    }
#endif
    
    return true;
}

extern "C" {
    __declspec(dllexport) void* Get_Mesh_InstantiateByGuid_Ptr() { return (void*)&Mesh_InstantiateByGuid; }
    __declspec(dllexport) void* Get_Mesh_InstantiateByGuidWithRoot_Ptr() { return (void*)&Mesh_InstantiateByGuidWithRoot; }
    __declspec(dllexport) void* Get_Mesh_SetByGuid_Ptr() { return (void*)&Mesh_SetByGuid; }
    __declspec(dllexport) void* Get_Mesh_HasComponent_Ptr() { return (void*)&Mesh_HasComponent; }
    __declspec(dllexport) void* Get_Mesh_GetReference_Ptr() { return (void*)&Mesh_GetReference; }
    __declspec(dllexport) void* Get_Mesh_GetVertexCount_Ptr() { return (void*)&Mesh_GetVertexCount; }
    __declspec(dllexport) void* Get_Mesh_GetIndexCount_Ptr() { return (void*)&Mesh_GetIndexCount; }
    __declspec(dllexport) void* Get_Mesh_GetSubmeshCount_Ptr() { return (void*)&Mesh_GetSubmeshCount; }
    __declspec(dllexport) void* Get_Mesh_GetMaterialSlotCount_Ptr() { return (void*)&Mesh_GetMaterialSlotCount; }
    __declspec(dllexport) void* Get_Mesh_GetMaterialSlotName_Ptr() { return (void*)&Mesh_GetMaterialSlotName; }
    __declspec(dllexport) void* Get_Mesh_GetBoundsMin_Ptr() { return (void*)&Mesh_GetBoundsMin; }
    __declspec(dllexport) void* Get_Mesh_GetBoundsMax_Ptr() { return (void*)&Mesh_GetBoundsMax; }
    __declspec(dllexport) void* Get_Mesh_GetBoundsPadding_Ptr() { return (void*)&Mesh_GetBoundsPadding; }
    __declspec(dllexport) void* Get_Mesh_SetBoundsPadding_Ptr() { return (void*)&Mesh_SetBoundsPadding; }
    __declspec(dllexport) void* Get_Mesh_GetRenderOnTop_Ptr() { return (void*)&Mesh_GetRenderOnTop; }
    __declspec(dllexport) void* Get_Mesh_SetRenderOnTop_Ptr() { return (void*)&Mesh_SetRenderOnTop; }
    __declspec(dllexport) void* Get_Mesh_GetShowBackfaces_Ptr() { return (void*)&Mesh_GetShowBackfaces; }
    __declspec(dllexport) void* Get_Mesh_SetShowBackfaces_Ptr() { return (void*)&Mesh_SetShowBackfaces; }
    __declspec(dllexport) void* Get_Mesh_GetSkipFrustumCulling_Ptr() { return (void*)&Mesh_GetSkipFrustumCulling; }
    __declspec(dllexport) void* Get_Mesh_SetSkipFrustumCulling_Ptr() { return (void*)&Mesh_SetSkipFrustumCulling; }
    __declspec(dllexport) void* Get_Mesh_GetRenderOrder_Ptr() { return (void*)&Mesh_GetRenderOrder; }
    __declspec(dllexport) void* Get_Mesh_SetRenderOrder_Ptr() { return (void*)&Mesh_SetRenderOrder; }
    __declspec(dllexport) void* Get_Mesh_GetUniqueMaterial_Ptr() { return (void*)&Mesh_GetUniqueMaterial; }
    __declspec(dllexport) void* Get_Mesh_SetUniqueMaterial_Ptr() { return (void*)&Mesh_SetUniqueMaterial; }
    __declspec(dllexport) void* Get_Mesh_HasSkinning_Ptr() { return (void*)&Mesh_HasSkinning; }
    __declspec(dllexport) void* Get_Mesh_GetName_Ptr() { return (void*)&Mesh_GetName; }
    __declspec(dllexport) void* Get_Mesh_GetAssetNameByGuid_Ptr() { return (void*)&Mesh_GetAssetNameByGuid; }
}
