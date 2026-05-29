#pragma once

#include <string>
#include <vector>

namespace texture_cleanup {

struct CleanupReport {
    int modelsScanned = 0;
    int modelsUpdated = 0;
    int texturesReferenced = 0;
    int duplicateGroups = 0;
    int texturesMovedToShared = 0;
    int texturesRemoved = 0;
    int textureMetaUpdated = 0;
    int emptyFoldersRemoved = 0;
    int scenesScanned = 0;
    int scenesUpdated = 0;
    int sceneTexturePathRemaps = 0;
    int sceneMaterialOverridesReset = 0;
    int prefabsScanned = 0;
    int prefabsUpdated = 0;
    int prefabTexturePathRemaps = 0;
    int prefabMaterialOverridesReset = 0;
    std::vector<std::string> logLines;
};

struct ImportDedupReport {
    int texturesChecked = 0;
    int texturesShared = 0;
    int texturesRemoved = 0;
    int metaUpdates = 0;
    int emptyFoldersRemoved = 0;
    int scenesScanned = 0;
    int scenesUpdated = 0;
    int sceneTexturePathRemaps = 0;
    int sceneMaterialOverridesReset = 0;
    int prefabsScanned = 0;
    int prefabsUpdated = 0;
    int prefabTexturePathRemaps = 0;
    int prefabMaterialOverridesReset = 0;
    std::vector<std::string> logLines;
};

// Full project cleanup: find shared texture content across all models,
// move to assets/textures/shared, update model metadata/import settings,
// and remove redundant copies.
bool CleanupSharedTextures(CleanupReport& out);

// Post-import helper: deduplicate textures for a single model against the project.
bool DeduplicateImportedModelTextures(const std::string& modelPath, ImportDedupReport* out = nullptr);

} // namespace texture_cleanup

