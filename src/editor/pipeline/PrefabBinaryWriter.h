#pragma once

#include "core/prefab/PrefabAsset.h"
#include "core/ecs/Entity.h"
#include <string>
#include <filesystem>

class Scene;

namespace binary {

/**
 * Writes PrefabAsset to binary format (.prefabb)
 * 
 * Binary format v2 (preferred - uses WriteFromScene):
 * - Header: PrefabBinaryHeader
 * - Prefab GUID + name index
 * - Entity headers: array of EntityHeader (GUID, parent GUID, name, component offset)
 * - Component data: binary component entries + data (same format as SceneBinaryWriter)
 * - String entries: array of StringEntry
 * - String table: null-terminated strings
 * 
 * Binary format v1 (legacy - uses Write(PrefabAsset)):
 * - Same structure but component data contains JSON blobs
 * - Identified by flags == 1 in header
 * 
 * Use WriteFromScene for proper binary component serialization.
 */
class PrefabBinaryWriter {
public:
    /**
     * Write a prefab from scene entities to binary file (preferred method)
     * Uses pure binary component serialization for runtime efficiency.
     * @param scene The scene containing the entities (non-const for GetEntityData access)
     * @param rootId The root entity ID of the prefab hierarchy
     * @param outputPath Path to write the .prefabb file
     * @return true on success
     */
    static bool WriteFromScene(Scene& scene, EntityID rootId, const std::filesystem::path& outputPath);
    
    /**
     * Write a prefab from scene entities to a byte vector (preferred method)
     * @param scene The scene containing the entities (non-const for GetEntityData access)
     * @param rootId The root entity ID of the prefab hierarchy
     * @param outData Output byte vector
     * @return true on success
     */
    static bool WriteFromScene(Scene& scene, EntityID rootId, std::vector<uint8_t>& outData);
    
    /**
     * Write a PrefabAsset to binary file (legacy - uses JSON blobs)
     * @param asset The prefab to serialize
     * @param outputPath Path to write the .prefabb file
     * @return true on success
     */
    static bool Write(const PrefabAsset& asset, const std::filesystem::path& outputPath);
    
    /**
     * Write a PrefabAsset to a byte vector (legacy - uses JSON blobs)
     * @param asset The prefab to serialize
     * @param outData Output byte vector
     * @return true on success
     */
    static bool Write(const PrefabAsset& asset, std::vector<uint8_t>& outData);
};

} // namespace binary
