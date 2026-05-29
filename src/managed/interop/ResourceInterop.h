#pragma once

#include <cstdint>

// ResourceInterop: Native exports for the managed Resources API
// These functions are called by the managed side via interop delegates

extern "C" {
    // Get count of resources for a given type
    // typeName: Full type name (e.g., "MyNamespace.Item")
    // Returns: Number of resources of that type
    __declspec(dllexport) int Resources_GetResourceCount(const char* typeName);
    
    // Get all resource GUIDs for a given type
    // typeName: Full type name
    // outGuids: Pointer to array that will receive GUID strings (must be pre-allocated)
    // maxCount: Maximum number of GUIDs to return
    // Returns: Actual number of GUIDs written
    __declspec(dllexport) int Resources_GetResourceGUIDs(const char* typeName, char** outGuids, int maxCount);
    
    // Get a single resource GUID by type and name
    // typeName: Full type name
    // name: Resource name (filename without extension)
    // Returns: GUID string (static buffer, valid until next call) or empty string if not found
    __declspec(dllexport) const char* Resources_GetResourceByName(const char* typeName, const char* name);
    
    // Get all resource names for a given type (for editor display)
    // typeName: Full type name
    // outNames: Pointer to array that will receive name strings (must be pre-allocated)
    // maxCount: Maximum number of names to return
    // Returns: Actual number of names written
    __declspec(dllexport) int Resources_GetResourceNames(const char* typeName, char** outNames, int maxCount);
    
    // Check if the resources system is initialized
    __declspec(dllexport) bool Resources_IsInitialized();
    
    // Initialize/scan resources folder (called by editor on project load)
    // projectRoot: Path to project directory
    // Returns: Number of resources found
    __declspec(dllexport) int Resources_Initialize(const char* projectRoot);
    
    // Refresh a single resource file (called on file change)
    __declspec(dllexport) void Resources_RefreshFile(const char* path);
    
    // Remove a resource (called on file delete)
    __declspec(dllexport) void Resources_RemoveFile(const char* path);
    
    // ========== Change Notification System ==========
    
    // Subscribe to changes for a specific type
    // typeName: Full type name to track
    __declspec(dllexport) void Resources_SubscribeToType(const char* typeName);
    
    // Unsubscribe from changes for a specific type
    __declspec(dllexport) void Resources_UnsubscribeFromType(const char* typeName);
    
    // Check if resources folder exists
    __declspec(dllexport) bool Resources_HasResourcesFolder();
    
    // Try to discover resources folder (case-insensitive)
    // Called when folder might have been created
    __declspec(dllexport) bool Resources_TryDiscoverFolder(const char* projectRoot);
    
    // Flush pending changes and trigger callbacks
    // Returns number of changes that were processed
    __declspec(dllexport) int Resources_FlushPendingChanges();
    
    // Get pending change count (for polling)
    __declspec(dllexport) int Resources_GetPendingChangeCount();
    
    // Get pending changes (for polling approach)
    // outTypes: Array to receive change types (0=Added, 1=Modified, 2=Removed)
    // outTypeNames: Array to receive type names
    // outNames: Array to receive resource names
    // outGuids: Array to receive GUID strings
    // maxCount: Maximum number of changes to return
    // Returns: Actual number of changes
    __declspec(dllexport) int Resources_GetPendingChanges(
        int* outTypes,
        char** outTypeNames,
        char** outNames,
        char** outGuids,
        int maxCount);

    // Resolve a VFS path (resources/...) to a GUID
    __declspec(dllexport) const char* Resources_GetGuidFromPath(const char* vfsPath);

    // Get count of assets under resources/ for a given managed type name
    __declspec(dllexport) int Resources_GetAssetCountByType(const char* typeName);

    // Get asset GUIDs under resources/ for a given managed type name
    __declspec(dllexport) int Resources_GetAssetGUIDsByType(const char* typeName, char** outGuids, int maxCount);
}

// Function pointer types for managed interop
using Resources_GetResourceCount_fn = int(*)(const char*);
using Resources_GetResourceGUIDs_fn = int(*)(const char*, char**, int);
using Resources_GetResourceByName_fn = const char*(*)(const char*, const char*);
using Resources_GetResourceNames_fn = int(*)(const char*, char**, int);
using Resources_IsInitialized_fn = bool(*)();
using Resources_SubscribeToType_fn = void(*)(const char*);
using Resources_UnsubscribeFromType_fn = void(*)(const char*);
using Resources_HasResourcesFolder_fn = bool(*)();
using Resources_TryDiscoverFolder_fn = bool(*)(const char*);
using Resources_FlushPendingChanges_fn = int(*)();
using Resources_GetPendingChangeCount_fn = int(*)();
using Resources_GetPendingChanges_fn = int(*)(int*, char**, char**, char**, int);
using Resources_GetGuidFromPath_fn = const char*(*)(const char*);
using Resources_GetAssetCountByType_fn = int(*)(const char*);
using Resources_GetAssetGUIDsByType_fn = int(*)(const char*, char**, int);

// Exported function pointers
extern Resources_GetResourceCount_fn Resources_GetResourceCountPtr;
extern Resources_GetResourceGUIDs_fn Resources_GetResourceGUIDsPtr;
extern Resources_GetResourceByName_fn Resources_GetResourceByNamePtr;
extern Resources_GetResourceNames_fn Resources_GetResourceNamesPtr;
extern Resources_IsInitialized_fn Resources_IsInitializedPtr;
extern Resources_SubscribeToType_fn Resources_SubscribeToTypePtr;
extern Resources_UnsubscribeFromType_fn Resources_UnsubscribeFromTypePtr;
extern Resources_HasResourcesFolder_fn Resources_HasResourcesFolderPtr;
extern Resources_TryDiscoverFolder_fn Resources_TryDiscoverFolderPtr;
extern Resources_FlushPendingChanges_fn Resources_FlushPendingChangesPtr;
extern Resources_GetPendingChangeCount_fn Resources_GetPendingChangeCountPtr;
extern Resources_GetPendingChanges_fn Resources_GetPendingChangesPtr;
extern Resources_GetGuidFromPath_fn Resources_GetGuidFromPathPtr;
extern Resources_GetAssetCountByType_fn Resources_GetAssetCountByTypePtr;
extern Resources_GetAssetGUIDsByType_fn Resources_GetAssetGUIDsByTypePtr;

