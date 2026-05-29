#include "core/prefab/PrefabAsset.h"
#include "core/prefab/PrefabCache.h"
#include "core/serialization/Serializer.h"
#include "core/ecs/Scene.h"
#include <iostream>

// Migration tool for legacy prefabs - now a no-op since legacy format is removed
namespace EditorMigration {

bool MigrateLegacyPrefabToAuthoring(const std::string& legacyPath, const ClaymoreGUID& newGuid) {
    // Legacy prefab format no longer supported
    std::cout << "[Migration] Legacy prefab migration no longer needed - format unified\n";
    return false;
}

}
