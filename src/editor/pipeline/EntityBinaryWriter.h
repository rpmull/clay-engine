#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include "core/assets/BinaryFormats.h"
#include "core/assets/AssetReference.h"
#include "core/ecs/Entity.h"

class Scene;
struct EntityData;

namespace binary {

// Write entities to binary format (.sceneb for scenes, .prefabb for prefabs)
// This is editor-only - generates binaries for play mode and export
// Unified writer for both scenes and prefabs - single source of truth
class EntityBinaryWriter {
public:
    //--------------------------------------------------------------------------
    // Scene writing (full scene with environment)
    //--------------------------------------------------------------------------
    
    // Write scene to file (.sceneb)
    static bool Write(const Scene& scene, const std::string& outputPath);
    
    // Write scene to memory buffer
    static bool WriteToMemory(const Scene& scene, std::vector<uint8_t>& outData);
    
    //--------------------------------------------------------------------------
    // Prefab writing (entity subtree without environment)
    //--------------------------------------------------------------------------
    
    // Write prefab subtree to file (.prefabb)
    static bool WritePrefab(Scene& scene, EntityID rootId, const std::string& outputPath);
    
    // Write prefab subtree to memory buffer
    static bool WritePrefabToMemory(Scene& scene, EntityID rootId, std::vector<uint8_t>& outData);

    struct WriteContext {
        std::vector<uint8_t> data;
        std::vector<std::string> strings;
        std::unordered_map<std::string, uint32_t> stringLookup;
        std::vector<AssetRefEntry> assetRefs;
        Scene* scene = nullptr;
        EntityID currentEntityId = INVALID_ENTITY_ID;
        bool sanitizeDerivedPrefabRefs = false;
        const std::unordered_map<EntityID, uint32_t>* prefabEntityIndexMap = nullptr;
        
        void Write(const void* src, size_t count);
        uint32_t AddString(const std::string& str);
        void AddAssetRef(const ClaymoreGUID& guid, const std::string& path, uint32_t typeHint);
        
        template<typename T>
        void Write(const T& val) { Write(&val, sizeof(T)); }
        
        size_t Position() const { return data.size(); }
        void WriteAt(size_t offset, const void* src, size_t count);
    };

private:
    // Scene-specific writers
    static void WriteHeader(WriteContext& ctx, SceneBinaryHeader& header);
    static void WriteEntities(WriteContext& ctx, const Scene& scene, SceneBinaryHeader& header);
    static void WriteStringTable(WriteContext& ctx, SceneBinaryHeader& header);
    static void WriteAssetRefTable(WriteContext& ctx, SceneBinaryHeader& header);
    static void WriteEnvironment(WriteContext& ctx, const Scene& scene, SceneBinaryHeader& header);
    static void WriteModelDeltas(WriteContext& ctx, const Scene& scene, SceneBinaryHeader& header);
    
    // Shared component writer
    static void WriteComponent(WriteContext& ctx, const EntityData* data, ComponentTypeId typeId);
};

} // namespace binary

