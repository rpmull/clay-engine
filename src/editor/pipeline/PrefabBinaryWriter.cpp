#include "PrefabBinaryWriter.h"
#include "core/assets/BinaryFormats.h"
#include "core/serialization/ComponentBinarySerializer.h"
#include "core/model/ModelDelta.h"
#include "core/model/ModelDeltaExtractor.h"
#include "core/ecs/EntityData.h"
#include "core/ecs/Scene.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include <unordered_map>

namespace binary {

namespace {

// Helper to write a value to a byte vector
template<typename T>
void WriteValue(std::vector<uint8_t>& data, const T& value) {
    size_t offset = data.size();
    data.resize(offset + sizeof(T));
    std::memcpy(data.data() + offset, &value, sizeof(T));
}

// Write context adapter for shared component serializer
struct PrefabWriteContext {
    std::vector<uint8_t>& data;
    std::vector<std::string>& strings;
    std::unordered_map<std::string, uint32_t>& stringLookup;
    
    size_t Position() const { return data.size(); }
    
    template<typename T>
    void Write(const T& value) {
        size_t offset = data.size();
        data.resize(offset + sizeof(T));
        std::memcpy(data.data() + offset, &value, sizeof(T));
    }
    
    void Write(const void* src, size_t count) {
        size_t offset = data.size();
        data.resize(offset + count);
        std::memcpy(data.data() + offset, src, count);
    }
    
    uint32_t AddString(const std::string& str) {
        auto it = stringLookup.find(str);
        if (it != stringLookup.end()) {
            return it->second;
        }
        uint32_t index = static_cast<uint32_t>(strings.size());
        strings.push_back(str);
        stringLookup[str] = index;
        return index;
    }
};

// Convert PrefabWriteContext to ComponentWriteContext
ComponentWriteContext ToComponentContext(PrefabWriteContext& ctx) {
    return ComponentWriteContext{
        ctx.data,
        ctx.strings,
        [&ctx](const std::string& s) { return ctx.AddString(s); }
    };
}

} // namespace

bool PrefabBinaryWriter::WriteFromScene(Scene& scene, EntityID rootId, std::vector<uint8_t>& outData) {
    outData.clear();
    
    // Collect all entities under root (including root)
    std::vector<EntityID> entityIds;
    std::unordered_map<EntityID, uint32_t> entityIndexMap;
    std::unordered_map<EntityID, std::vector<EntityID>> childrenByParent;
    for (const auto& entity : scene.GetEntities()) {
        const EntityID id = entity.GetID();
        const EntityData* data = scene.GetEntityData(id);
        if (!data || data->Parent == INVALID_ENTITY_ID) {
            continue;
        }
        childrenByParent[data->Parent].push_back(id);
    }
    size_t repairedChildLinks = 0;
    
    // Recursively collect entities
    // CRITICAL: Use both Children array AND parent relationship to ensure no children are missed
    // This handles cases where Children might not be properly maintained
    std::function<void(EntityID)> collectEntities = [&](EntityID id) {
        const EntityData* data = scene.GetEntityData(id);
        if (!data) return;
        
        // Skip if already collected
        if (entityIndexMap.find(id) != entityIndexMap.end()) return;
        
        entityIndexMap[id] = static_cast<uint32_t>(entityIds.size());
        entityIds.push_back(id);
        
        // Collect via Children array (primary method)
        for (EntityID childId : data->Children) {
            collectEntities(childId);
        }
        
        auto fallbackIt = childrenByParent.find(id);
        if (fallbackIt != childrenByParent.end()) {
            for (EntityID childId : fallbackIt->second) {
                if (entityIndexMap.find(childId) != entityIndexMap.end()) {
                    continue;
                }
                ++repairedChildLinks;
                collectEntities(childId);
            }
        }
    };
    
    collectEntities(rootId);
    if (repairedChildLinks > 0) {
        std::cerr << "[PrefabBinaryWriter] Recovered " << repairedChildLinks
                  << " child links from Parent fields while writing prefab.\n";
    }
    
    if (entityIds.empty()) {
        std::cerr << "[PrefabBinaryWriter] No entities to write\n";
        return false;
    }
    
    // Prepare write context
    std::vector<std::string> strings;
    std::unordered_map<std::string, uint32_t> stringLookup;
    strings.push_back(""); // Index 0 = empty string
    stringLookup[""] = 0;
    
    std::vector<uint8_t> entityData;
    std::vector<uint8_t> componentData;
    
    // Entity header structure (v5, shared with runtime v4+ loader)
    // Matches RuntimePrefabInstantiator::EntityHeader for parity
    struct EntityHeader {
        uint64_t guidHigh;
        uint64_t guidLow;
        uint64_t parentGuidHigh;
        uint64_t parentGuidLow;
        uint32_t entityId;          // Original entity ID for reference remapping
        uint32_t nameIndex;
        uint8_t  flags;             // 0x01=Active, 0x02=Visible, 0x04=HasModelGuid
        uint8_t  padding[3];        // Alignment
        int32_t  layer;
        uint32_t tagIndex;
        uint64_t modelGuidHigh;
        uint64_t modelGuidLow;
        uint32_t componentCount;
        uint32_t componentOffset;   // Absolute offset into component data section
    };
    
    std::vector<EntityHeader> entityHeaders;
    
    // Write component data for each entity
    for (EntityID id : entityIds) {
        const EntityData* data = scene.GetEntityData(id);
        if (!data) continue;
        
        EntityHeader header{};
        header.guidHigh = data->EntityGuid.high;
        header.guidLow = data->EntityGuid.low;
        header.entityId = static_cast<uint32_t>(id);  // Store original entity ID for reference remapping
        
        // Get parent GUID
        if (data->Parent != static_cast<EntityID>(-1)) {
            const EntityData* parentData = scene.GetEntityData(data->Parent);
            if (parentData) {
                header.parentGuidHigh = parentData->EntityGuid.high;
                header.parentGuidLow = parentData->EntityGuid.low;
            }
        }
        
        // Add name/tag to string table
        PrefabWriteContext ctx{componentData, strings, stringLookup};
        header.nameIndex = ctx.AddString(data->Name);
        header.tagIndex = ctx.AddString(data->Tag);
        header.layer = data->Layer;
        header.flags = 0;
        if (data->Active) header.flags |= 0x01;
        if (data->Visible) header.flags |= 0x02;
        
        // Model GUID (used for model delta/root identification)
        header.modelGuidHigh = data->ModelAssetGuid.high;
        header.modelGuidLow = data->ModelAssetGuid.low;
        if (header.modelGuidHigh != 0 || header.modelGuidLow != 0) {
            header.flags |= 0x04;
        }
        
        header.componentOffset = static_cast<uint32_t>(componentData.size());
        
        // Get components for this entity
        auto components = GetEntityComponents(data);
        header.componentCount = static_cast<uint32_t>(components.size());
        
        // Write each component
        for (ComponentTypeId typeId : components) {
            // Write component entry
            ComponentEntry entry;
            entry.typeId = typeId;
            entry.flags = 0;
            
            size_t entryPos = componentData.size();
            componentData.resize(entryPos + sizeof(ComponentEntry));  // Reserve space for entry
            
            entry.dataOffset = static_cast<uint32_t>(componentData.size());
            
            // Write component data using shared serializer
            auto compCtx = ToComponentContext(ctx);
            compCtx.scene = &scene;
            compCtx.currentEntityId = id;
            size_t compSize = WriteComponentBinary(compCtx, data, typeId);
            entry.dataSize = static_cast<uint32_t>(compSize);
            
            // Write entry header at reserved position
            std::memcpy(componentData.data() + entryPos, &entry, sizeof(ComponentEntry));
        }
        
        entityHeaders.push_back(header);
    }
    
    // Extract model deltas for any model roots in the prefab hierarchy
    using namespace cm::model;
    std::vector<std::pair<EntityID, ModelDelta>> modelDeltas;
    std::vector<std::pair<size_t, std::vector<uint8_t>>> deltaBlobs;
    
    for (EntityID id : entityIds) {
        EntityData* data = scene.GetEntityData(id);
        if (!data) continue;
        
        // Check if this entity is a model root (has ModelAssetGuid)
        if (data->ModelAssetGuid.high != 0 || data->ModelAssetGuid.low != 0) {
            ModelDeltaExtractor extractor(scene);
            DeltaExtractionConfig extractConfig{};
            extractConfig.extractScripts = false;
            extractConfig.extractEntityOverrides = false;
            ModelDelta delta = extractor.Extract(id, extractConfig);
            
            // Only add non-empty deltas
            if (!delta.IsEmpty()) {
                nlohmann::json deltaJson = delta.ToJson();
                std::string deltaStr = deltaJson.dump();
                std::vector<uint8_t> deltaData(deltaStr.begin(), deltaStr.end());
                
                size_t idx = modelDeltas.size();
                modelDeltas.emplace_back(id, std::move(delta));
                deltaBlobs.emplace_back(idx, std::move(deltaData));
            }
        }
    }
    
    // Calculate layout
    size_t headerSize = sizeof(PrefabBinaryHeader);
    size_t prefabGuidSize = 16;  // GUID high + low
    size_t prefabNameSize = 4;   // Name index
    size_t entityTableOffset = headerSize + prefabGuidSize + prefabNameSize;
    size_t entityTableSize = entityHeaders.size() * sizeof(EntityHeader);
    size_t componentTableOffset = entityTableOffset + entityTableSize;
    size_t stringTableOffset = componentTableOffset + componentData.size();
    
    // Adjust componentOffset in entity headers to be absolute file offsets
    for (auto& eh : entityHeaders) {
        eh.componentOffset += static_cast<uint32_t>(componentTableOffset);
    }
    
    // Build string table binary
    std::vector<uint8_t> stringTableData;
    std::vector<StringEntry> stringEntries;
    for (const auto& str : strings) {
        StringEntry entry;
        entry.offset = static_cast<uint32_t>(stringTableData.size());
        entry.length = static_cast<uint32_t>(str.size());
        stringTableData.insert(stringTableData.end(), str.begin(), str.end());
        stringTableData.push_back(0);  // null terminator
        stringEntries.push_back(entry);
    }
    
    // Calculate model delta table offset (after string table)
    size_t modelDeltaTableOffset = stringTableOffset + 
                                   stringEntries.size() * sizeof(StringEntry) + 
                                   stringTableData.size();
    
    // Reserve output buffer
    size_t deltaTableSize = deltaBlobs.size() * sizeof(ModelDeltaEntry);
    size_t deltaBlobsSize = 0;
    for (const auto& [idx, blob] : deltaBlobs) {
        deltaBlobsSize += blob.size();
    }
    size_t totalSize = modelDeltaTableOffset + deltaTableSize + deltaBlobsSize;
    outData.reserve(totalSize);
    
    // Write header
    PrefabBinaryHeader header;
    header.base.magic = PREFAB_MAGIC;
    header.base.version = PREFAB_VERSION;
    header.base.flags = 0;
    header.base.reserved = 0;
    header.entityCount = static_cast<uint32_t>(entityHeaders.size());
    header.rootEntityIndex = 0;  // Root is always first
        header.componentTableOffset = static_cast<uint32_t>(componentTableOffset);
        // stringTableOffset should point to raw string data, after the StringEntry array
        header.stringTableOffset = static_cast<uint32_t>(stringTableOffset + stringEntries.size() * sizeof(StringEntry));
        header.assetRefTableOffset = 0;  // Not used yet
        header.modelDeltaTableOffset = static_cast<uint32_t>(modelDeltaTableOffset);
        header.modelDeltaCount = static_cast<uint32_t>(deltaBlobs.size());
        
        WriteValue(outData, header);
        
        // Write prefab GUID (use prefab asset GUID if available; fallback to root entity GUID)
        EntityData* rootData = scene.GetEntityData(rootId);
        ClaymoreGUID prefabGuid = rootData ? rootData->PrefabGuid : ClaymoreGUID{};
        if (prefabGuid.high == 0 && prefabGuid.low == 0 && rootData) {
            // Fallback to root entity GUID to avoid zero GUID binaries
            prefabGuid = rootData->EntityGuid;
        }
        WriteValue(outData, prefabGuid.high);
        WriteValue(outData, prefabGuid.low);
    
    // Write prefab name index
    PrefabWriteContext nameCtx{outData, strings, stringLookup};
    uint32_t prefabNameIdx = nameCtx.AddString(rootData ? rootData->Name : "Prefab");
    WriteValue(outData, prefabNameIdx);
    
    // Write entity headers
    for (const auto& eh : entityHeaders) {
        WriteValue(outData, eh);
    }
    
    // Write component data
    outData.insert(outData.end(), componentData.begin(), componentData.end());
    
    // Write string entries
    for (const auto& se : stringEntries) {
        WriteValue(outData, se);
    }
    
    // Write string table data
    outData.insert(outData.end(), stringTableData.begin(), stringTableData.end());
    
    // Write model delta table and data (v3+)
    if (!deltaBlobs.empty()) {
        size_t deltaBlobStart = outData.size() + deltaBlobs.size() * sizeof(ModelDeltaEntry);
        size_t currentBlobOffset = deltaBlobStart;
        
        // Write delta entries
        for (size_t i = 0; i < deltaBlobs.size(); ++i) {
            const auto& [idx, blob] = deltaBlobs[i];
            const auto& [modelEntityId, delta] = modelDeltas[idx];
            EntityData* modelData = scene.GetEntityData(modelEntityId);
            
            ModelDeltaEntry entry;
            entry.modelGuidHigh = modelData ? modelData->ModelAssetGuid.high : 0;
            entry.modelGuidLow = modelData ? modelData->ModelAssetGuid.low : 0;
            entry.rootEntityId = static_cast<uint32_t>(modelEntityId);
            entry.dataOffset = static_cast<uint32_t>(currentBlobOffset);
            entry.dataSize = static_cast<uint32_t>(blob.size());
            
            WriteValue(outData, entry);
            currentBlobOffset += blob.size();
        }
        
        // Write delta data blobs
        for (const auto& [idx, blob] : deltaBlobs) {
            outData.insert(outData.end(), blob.begin(), blob.end());
        }
        
        std::cout << "[PrefabBinaryWriter] Wrote " << deltaBlobs.size() << " model deltas\n";
    }
    
    std::cout << "[PrefabBinaryWriter] Wrote " << entityHeaders.size() 
              << " entities with binary components, " << outData.size() << " bytes total\n";
    return true;
}

bool PrefabBinaryWriter::Write(const PrefabAsset& asset, std::vector<uint8_t>& outData) {
    // This version is for when we have a PrefabAsset (with JSON entities)
    // We need a scene to properly serialize binary components
    // For now, fall back to JSON-based approach (deprecated)
    std::cerr << "[PrefabBinaryWriter] WARNING: Write(PrefabAsset) uses legacy JSON format. "
              << "Use WriteFromScene() for binary component format.\n";
    
    outData.clear();
    
    if (!asset.Entities.is_array()) {
        std::cerr << "[PrefabBinaryWriter] Invalid prefab: Entities is not an array\n";
        return false;
    }
    
    // Legacy JSON blob approach - kept for compatibility
    std::vector<uint8_t> stringTable;
    std::vector<StringEntry> stringEntries;
    
    auto writeString = [&](const std::string& str) -> uint32_t {
        uint32_t offset = static_cast<uint32_t>(stringTable.size());
        uint32_t length = static_cast<uint32_t>(str.size());
        stringTable.insert(stringTable.end(), str.begin(), str.end());
        stringTable.push_back(0);
        StringEntry entry{offset, length};
        stringEntries.push_back(entry);
        return static_cast<uint32_t>(stringEntries.size() - 1);
    };
    
    writeString("");  // Index 0 = empty
    
    struct EntityRecord {
        uint64_t guidHigh;
        uint64_t guidLow;
        uint64_t parentGuidHigh;
        uint64_t parentGuidLow;
        uint32_t nameIndex;
        uint32_t componentJsonOffset;
        uint32_t componentJsonSize;
        uint32_t childCount;
    };
    
    std::vector<EntityRecord> entityRecords;
    std::vector<std::vector<uint8_t>> componentJsonBlobs;
    
    uint32_t rootIndex = 0;
    for (size_t i = 0; i < asset.Entities.size(); ++i) {
        const auto& entityJson = asset.Entities[i];
        if (!entityJson.is_object()) continue;
        
        ClaymoreGUID guid{};
        if (entityJson.contains("guid")) {
            try { entityJson.at("guid").get_to(guid); } catch (...) {}
        }
        
        if (guid.high == asset.RootGuid.high && guid.low == asset.RootGuid.low) {
            rootIndex = static_cast<uint32_t>(i);
            break;
        }
    }
    
    for (const auto& entityJson : asset.Entities) {
        if (!entityJson.is_object()) continue;
        
        EntityRecord rec{};
        
        if (entityJson.contains("guid")) {
            try {
                ClaymoreGUID g;
                entityJson.at("guid").get_to(g);
                rec.guidHigh = g.high;
                rec.guidLow = g.low;
            } catch (...) {}
        }
        
        if (entityJson.contains("_parentGuid")) {
            try {
                ClaymoreGUID pg;
                entityJson.at("_parentGuid").get_to(pg);
                rec.parentGuidHigh = pg.high;
                rec.parentGuidLow = pg.low;
            } catch (...) {}
        }
        
        std::string name = entityJson.value("name", "");
        rec.nameIndex = writeString(name);
        rec.childCount = 0;
        
        std::string jsonStr = entityJson.dump();
        std::vector<uint8_t> jsonBlob(jsonStr.begin(), jsonStr.end());
        rec.componentJsonSize = static_cast<uint32_t>(jsonBlob.size());
        
        componentJsonBlobs.push_back(std::move(jsonBlob));
        entityRecords.push_back(rec);
    }
    
    size_t headerSize = sizeof(PrefabBinaryHeader);
    size_t prefabGuidSize = 16;
    size_t prefabNameSize = 4;
    size_t entityTableSize = entityRecords.size() * sizeof(EntityRecord);
    size_t componentDataOffset = headerSize + prefabGuidSize + prefabNameSize + entityTableSize;
    
    size_t componentDataSize = 0;
    for (const auto& blob : componentJsonBlobs) {
        componentDataSize += blob.size();
    }
    
    size_t stringTableOffset = componentDataOffset + componentDataSize;
    
    outData.reserve(stringTableOffset + stringTable.size());
    
    PrefabBinaryHeader header;
    header.base.magic = PREFAB_MAGIC;
    header.base.version = PREFAB_VERSION;
    header.base.flags = 1;  // Flag 1 = legacy JSON format
    header.base.reserved = 0;
    header.entityCount = static_cast<uint32_t>(entityRecords.size());
    header.rootEntityIndex = rootIndex;
    header.componentTableOffset = static_cast<uint32_t>(componentDataOffset);
    header.stringTableOffset = static_cast<uint32_t>(stringTableOffset);
    header.assetRefTableOffset = 0;
    
    WriteValue(outData, header);
    WriteValue(outData, asset.Guid.high);
    WriteValue(outData, asset.Guid.low);
    
    uint32_t prefabNameIndex = writeString(asset.Name);
    WriteValue(outData, prefabNameIndex);
    
    uint32_t currentComponentOffset = static_cast<uint32_t>(componentDataOffset);
    for (size_t i = 0; i < entityRecords.size(); ++i) {
        auto& rec = entityRecords[i];
        rec.componentJsonOffset = currentComponentOffset;
        currentComponentOffset += rec.componentJsonSize;
        WriteValue(outData, rec);
    }
    
    for (const auto& blob : componentJsonBlobs) {
        outData.insert(outData.end(), blob.begin(), blob.end());
    }
    
    outData.insert(outData.end(), stringTable.begin(), stringTable.end());
    
    std::cout << "[PrefabBinaryWriter] (Legacy) Wrote " << entityRecords.size() 
              << " entities with JSON blobs, " << outData.size() << " bytes\n";
    return true;
}

bool PrefabBinaryWriter::Write(const PrefabAsset& asset, const std::filesystem::path& outputPath) {
    std::vector<uint8_t> data;
    if (!Write(asset, data)) {
        return false;
    }
    
    std::error_code ec;
    std::filesystem::create_directories(outputPath.parent_path(), ec);
    
    std::ofstream file(outputPath, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        std::cerr << "[PrefabBinaryWriter] Failed to open file: " << outputPath << std::endl;
        return false;
    }
    
    file.write(reinterpret_cast<const char*>(data.data()), data.size());
    return file.good();
}

bool PrefabBinaryWriter::WriteFromScene(Scene& scene, EntityID rootId, const std::filesystem::path& outputPath) {
    std::vector<uint8_t> data;
    if (!WriteFromScene(scene, rootId, data)) {
        return false;
    }
    
    std::error_code ec;
    std::filesystem::create_directories(outputPath.parent_path(), ec);
    
    std::ofstream file(outputPath, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        std::cerr << "[PrefabBinaryWriter] Failed to open file: " << outputPath << std::endl;
        return false;
    }
    
    file.write(reinterpret_cast<const char*>(data.data()), data.size());
    return file.good();
}

} // namespace binary
