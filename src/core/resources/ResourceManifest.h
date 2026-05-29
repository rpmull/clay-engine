#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <filesystem>
#include <functional>
#include "core/assets/AssetReference.h"

// ResourceEntry: Represents a single resource in the resources/ folder
struct ResourceEntry {
    ClaymoreGUID guid;           // Asset GUID
    std::string path;            // Full path to .clayobj file
    std::string name;            // Resource name (filename without extension)
    std::string typeName;        // Full type name (e.g., "MyNamespace.Item")
    std::vector<ClaymoreGUID> dependencies; // GUIDs of referenced assets (meshes, etc.)
};

// ResourceChangeType: Type of change to a resource
enum class ResourceChangeType {
    Added,
    Modified,
    Removed
};

// ResourceChangeEvent: Notification about resource changes
struct ResourceChangeEvent {
    ResourceChangeType type;
    std::string typeName;    // Type of the resource that changed
    std::string name;        // Name of the resource
    ClaymoreGUID guid;       // GUID of the resource
};

// Callback for resource change notifications
using ResourceChangeCallback = std::function<void(const std::vector<ResourceChangeEvent>&)>;

// ResourceManifest: Tracks all ClayScriptableObjects in the resources/ folder
// Provides fast lookup by type and name for runtime access
class ResourceManifest {
public:
    static ResourceManifest& Get() {
        static ResourceManifest instance;
        return instance;
    }

    // Initialize/refresh the manifest by scanning the resources folder
    // projectRoot: Path to project directory (parent of resources/)
    // Returns true if resources folder was found
    bool Initialize(const std::filesystem::path& projectRoot);
    
    // Try to discover and initialize resources folder (case-insensitive)
    // Called periodically or on folder creation events
    // Returns true if resources folder was found and initialized
    bool TryDiscoverResourcesFolder(const std::filesystem::path& projectRoot);
    
    // Scan the resources folder and update manifest
    // Returns number of resources found
    int Scan();
    
    // Refresh a single resource file (called on file change)
    // Returns true if the file was a valid resource and triggers change events
    bool RefreshFile(const std::string& path);
    
    // Remove a resource from manifest (called on file delete)
    // Returns true if a resource was removed
    bool RemoveFile(const std::string& path);
    
    // Check if manifest is initialized and resources folder exists
    bool IsInitialized() const { return m_Initialized && m_ResourcesFolderExists; }
    
    // Check if a resources folder has been discovered
    bool HasResourcesFolder() const { return m_ResourcesFolderExists; }
    
    // Get all resources of a specific type
    std::vector<const ResourceEntry*> GetResourcesByType(const std::string& typeName) const;
    
    // Get a resource by type and name
    const ResourceEntry* GetResource(const std::string& typeName, const std::string& name) const;
    
    // Get resource by GUID
    const ResourceEntry* GetResourceByGUID(const ClaymoreGUID& guid) const;
    
    // Get all resources (for build export)
    std::vector<const ResourceEntry*> GetAllResources() const;
    
    // Get all unique dependencies across all resources
    std::vector<ClaymoreGUID> GetAllDependencies() const;
    
    // Get the resources folder path
    const std::filesystem::path& GetResourcesPath() const { return m_ResourcesPath; }

    // Deterministic GUID from a normalized VFS path (e.g., "resources/icons/sword.png")
    static ClaymoreGUID DeterministicGuidFromPath(const std::string& path);
    
    // Check if a path is within the resources folder
    bool IsResourcePath(const std::string& path) const;
    
    // Save manifest to JSON file (for build export optimization)
    bool SaveToFile(const std::string& path) const;
    
    // Load manifest from JSON file (for runtime)
    bool LoadFromFile(const std::string& path);
    
    // Load manifest from JSON string (for runtime PAK loading)
    bool LoadFromJson(const std::string& jsonString);
    
    // Clear the manifest
    void Clear();
    
    // ========== Change Notification System ==========
    
    // Register a callback to receive resource change notifications
    // Returns a handle that can be used to unregister
    uint32_t RegisterChangeCallback(ResourceChangeCallback callback);
    
    // Unregister a change callback
    void UnregisterChangeCallback(uint32_t handle);
    
    // Get types that have active subscribers (for optimization)
    std::vector<std::string> GetSubscribedTypes() const;
    
    // Register interest in a specific type (called by managed side)
    void SubscribeToType(const std::string& typeName);
    
    // Unsubscribe from a type
    void UnsubscribeFromType(const std::string& typeName);
    
    // Check if any scripts are subscribed to a type
    bool HasSubscribers(const std::string& typeName) const;
    
    // Flush any pending change events (call from main thread)
    void FlushPendingChanges();

private:
    ResourceManifest() = default;
    
    // Parse a .clayobj file to extract type, name, and dependencies
    bool ParseClayObject(const std::filesystem::path& path, ResourceEntry& entry);
    
    // Extract dependencies from a parsed JSON clayobj
    void ExtractDependencies(const struct nlohmann::json& fields, std::vector<ClaymoreGUID>& deps);
    
    // Find resources folder with case-insensitive matching
    std::filesystem::path FindResourcesFolder(const std::filesystem::path& projectRoot) const;
    
    // Queue a change event for batch notification
    void QueueChangeEvent(ResourceChangeType type, const ResourceEntry& entry);
    
    // Notify all registered callbacks
    void NotifyChanges(const std::vector<ResourceChangeEvent>& events);
    
    mutable std::mutex m_Mutex;
    std::filesystem::path m_ProjectRoot;
    std::filesystem::path m_ResourcesPath;
    bool m_Initialized = false;
    bool m_ResourcesFolderExists = false;
    
    // Primary storage: GUID -> ResourceEntry
    std::unordered_map<ClaymoreGUID, ResourceEntry> m_Resources;
    
    // Lookup indices
    std::unordered_map<std::string, std::vector<ClaymoreGUID>> m_TypeIndex; // typeName -> GUIDs
    std::unordered_map<std::string, ClaymoreGUID> m_NameIndex; // "typeName:name" -> GUID
    std::unordered_map<std::string, ClaymoreGUID> m_PathIndex; // normalized path -> GUID
    
    // Change notification system
    std::unordered_map<uint32_t, ResourceChangeCallback> m_ChangeCallbacks;
    uint32_t m_NextCallbackHandle = 1;
    std::unordered_set<std::string> m_SubscribedTypes; // Types with active subscribers
    std::vector<ResourceChangeEvent> m_PendingChanges; // Batched changes
    mutable std::mutex m_ChangeMutex;
};
