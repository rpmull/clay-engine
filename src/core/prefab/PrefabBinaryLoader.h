#pragma once

#include "PrefabAsset.h"
#include <string>
#include <vector>

namespace binary {

/**
 * Loads PrefabAsset from binary format (.prefabb)
 * This is the core/runtime loader - no editor dependencies.
 */
class PrefabBinaryLoader {
public:
    /**
     * Load a PrefabAsset from binary file
     * @param filepath Path to the .prefabb file
     * @param outAsset Output prefab asset
     * @return true on success
     */
    static bool Load(const std::string& filepath, PrefabAsset& outAsset);
    
    /**
     * Load a PrefabAsset from binary data
     * @param data Binary data
     * @param outAsset Output prefab asset
     * @return true on success
     */
    static bool Load(const std::vector<uint8_t>& data, PrefabAsset& outAsset);
};

} // namespace binary

