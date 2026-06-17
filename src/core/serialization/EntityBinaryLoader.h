#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <functional>
#include "core/assets/BinaryFormats.h"
#include "core/ecs/Entity.h"

class Scene;
struct EntityData;

namespace binary {

//==============================================================================
// Shared Component Read Context - used by both scene and prefab loaders
//==============================================================================
struct ComponentLoadContext {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;
    uint32_t version = 0;  // Binary format version for version-specific component loading
    std::function<std::string(uint32_t)> readString;
    bool prewarm = false;  // Skip heavy loads (used by prewarm)
    
    bool Read(void* dst, size_t count);
    std::string ReadString(uint32_t index) const { return readString ? readString(index) : ""; }
    
    template<typename T>
    bool Read(T& out) { return Read(&out, sizeof(T)); }
};

//==============================================================================
// Shared Component Loading - THE SINGLE SOURCE OF TRUTH
// Used by both EntityBinaryLoader (scenes) and RuntimePrefabInstantiator (prefabs)
// dataSize is needed for legacy format detection in some components
//==============================================================================
bool LoadComponentBinary(ComponentLoadContext& ctx, EntityData* data, ComponentTypeId typeId, uint32_t dataSize = 0);

//==============================================================================
// Scene Binary Loader
//==============================================================================
class EntityBinaryLoader {
public:
    // Load scene from file path (uses VFS)
    static bool Load(const std::string& path, Scene& scene);
    
    // Load scene from memory buffer
    static bool LoadFromMemory(const uint8_t* data, size_t size, Scene& scene);
    
    // Quick validation - check magic and version
    static bool Validate(const std::string& path);
    static bool ValidateMemory(const uint8_t* data, size_t size);

    // Streaming loader context (incremental loading)
    struct LoadContext {
        const uint8_t* data = nullptr;
        size_t size = 0;
        size_t pos = 0;
        
        // Format version (needed for version-specific component loading)
        uint32_t version = 0;
        
        // String table for name lookups
        std::vector<std::string> strings;
        const std::vector<std::string>* stringView = nullptr;
        
        // Asset reference table
        std::vector<AssetRefEntry> assetRefs;
        const std::vector<AssetRefEntry>* assetRefView = nullptr;
        
        bool Read(void* dst, size_t count);
        std::string ReadString(uint32_t index) const;
        
        template<typename T>
        bool Read(T& out) { return Read(&out, sizeof(T)); }
    };

    struct StreamingContext {
        LoadContext ctx;
        SceneBinaryHeader header{};
        bool initialized = false;
        bool headerLoaded = false;
        bool stringsLoaded = false;
        bool assetRefsLoaded = false;
        bool parentsApplied = false;
        bool refsRemapped = false;
        bool envLoaded = false;
        bool modelDeltasLoaded = false;
        bool meshesResolved = false;
        bool materialsResolved = false;
        bool avatarsBuilt = false;
        bool animationsFixed = false;
        bool complete = false;
        bool failed = false;
        uint32_t entityIndex = 0;
        std::vector<std::pair<EntityID, EntityID>> parentRelationships;
        std::unordered_map<EntityID, EntityID> oldToNewIdMap;
        std::vector<EntityID> entityOrder;
        size_t meshResolveIndex = 0;
        size_t materialResolveIndex = 0;
        int meshesLoaded = 0;
        int meshesFailed = 0;
        int materialsLoaded = 0;
        int materialsFailed = 0;
        int inlineMaterialsCreated = 0;
        int entitiesWithMesh = 0;
        int entitiesWithMaterialPaths = 0;
    };

    static bool BeginStreaming(StreamingContext& stream, const uint8_t* data, size_t size);
    static bool StepStreaming(StreamingContext& stream, Scene& scene,
                              uint32_t maxEntities,
                              uint32_t maxMeshes,
                              uint32_t maxMaterials);
    static bool IsStreamingComplete(const StreamingContext& stream) { return stream.complete; }
    static float GetStreamingProgress(const StreamingContext& stream);
    
    struct PreparedEntityRecord {
        uint32_t entityId = 0;
        uint32_t parentId = 0;
        uint32_t nameIndex = 0;
        uint64_t guidHigh = 0;
        uint64_t guidLow = 0;
        uint8_t flags = 0;
        int32_t layer = 0;
        uint32_t tagIndex = 0xFFFFFFFFu;
        std::string legacyTag;
        uint64_t modelGuidHigh = 0;
        uint64_t modelGuidLow = 0;
        uint32_t componentCount = 0;
        size_t componentOffset = 0;
    };

private:
    static bool LoadHeader(LoadContext& ctx, SceneBinaryHeader& header);
    static bool LoadFromMemoryInternal(const uint8_t* data,
                                       size_t size,
                                       Scene& scene,
                                       const std::string& cacheKey,
                                       uint64_t timestampToken);
    static bool LoadStringTable(LoadContext& ctx, const SceneBinaryHeader& header);
    static bool LoadAssetRefTable(LoadContext& ctx, const SceneBinaryHeader& header);
    static bool LoadEntities(LoadContext& ctx, const SceneBinaryHeader& header, Scene& scene);
    static bool LoadPreparedEntities(LoadContext& ctx,
                                     const SceneBinaryHeader& header,
                                     const std::vector<PreparedEntityRecord>& records,
                                     Scene& scene);
    static bool LoadSinglePreparedEntity(LoadContext& ctx,
                                         const PreparedEntityRecord& record,
                                         Scene& scene,
                                         std::vector<std::pair<EntityID, EntityID>>& parentRelationships,
                                         std::unordered_map<EntityID, EntityID>& oldToNewIdMap,
                                         std::vector<EntityID>& outEntityOrder);
    static bool LoadSingleEntity(LoadContext& ctx,
                                 const SceneBinaryHeader& header,
                                 Scene& scene,
                                 std::vector<std::pair<EntityID, EntityID>>& parentRelationships,
                                 std::unordered_map<EntityID, EntityID>& oldToNewIdMap,
                                 std::vector<EntityID>& outEntityOrder);
    static bool LoadComponent(LoadContext& ctx, const ComponentEntry& entry, Scene& scene, uint32_t entityId);
    static void LoadEnvironment(LoadContext& ctx, const SceneBinaryHeader& header, Scene& scene);
    static void LoadModelDeltas(LoadContext& ctx, const SceneBinaryHeader& header, Scene& scene);
    
    // Post-load mesh resolution for runtime (loads meshes from meshbin via VFS)
    static void ResolveMeshReferences(LoadContext& ctx, Scene& scene);
    static bool ResolveMeshReferencesRange(LoadContext& ctx,
                                           Scene& scene,
                                           const std::vector<EntityID>& entities,
                                           size_t startIndex,
                                           size_t maxCount,
                                           int& meshesLoaded,
                                           int& meshesFailed);
    
    // Post-load material resolution for runtime (loads materials from matbin via VFS)
    static void ResolveMaterialReferences(LoadContext& ctx, Scene& scene);
    static bool ResolveMaterialReferencesRange(LoadContext& ctx,
                                               Scene& scene,
                                               const std::vector<EntityID>& entities,
                                               size_t startIndex,
                                               size_t maxCount,
                                               int& materialsLoaded,
                                               int& materialsFailed,
                                               int& inlineMaterialsCreated,
                                               int& entitiesWithMesh,
                                               int& entitiesWithMaterialPaths);
    static void ApplyDefaultMaterials(Scene& scene);
    
    // Build avatars for skeletons (required for humanoid animation retargeting)
    static void BuildSkeletonAvatars(Scene& scene);
    
    // Post-load animation fixup - ensures AnimationPlayer is on same entity as Skeleton
    static void FixupAnimationComponents(Scene& scene);
    
    // Remap entity references from old (binary) IDs to new (runtime) IDs
    static void RemapEntityReferences(Scene& scene, const std::unordered_map<EntityID, EntityID>& oldToNewIdMap);
};

} // namespace binary


