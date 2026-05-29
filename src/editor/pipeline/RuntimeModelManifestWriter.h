#pragma once

#include "core/assets/RuntimeModelManifest.h"
#include <string>
#include <vector>
#include <nlohmann/json_fwd.hpp>

namespace cm {

/**
 * Compiles editor .meta files into runtime model manifests
 * 
 * This is a "build metadata" to "runtime metadata" transformation:
 * - Strips editor-only fields (importSettings, source paths)
 * - Converts texture paths to GUIDs
 * - Flattens hierarchy into indexed parent references
 * - Outputs compact binary format
 * 
 * Supports both:
 * - Per-model .modelrt files (legacy/debug)
 * - Centralized model_registry.bin (production)
 */
class RuntimeModelManifestWriter {
public:
    /**
     * Compile a .meta file into a RuntimeModelManifest
     * @param metaPath Path to the source .meta file
     * @param outManifest Output manifest
     * @param pathToGuidResolver Callback to resolve asset paths to GUIDs
     * @return true on success
     */
    static bool CompileFromMeta(
        const std::string& metaPath,
        RuntimeModelManifest& outManifest,
        std::function<ClaymoreGUID(const std::string&)> pathToGuidResolver
    );
    
    /**
     * Compile from already-loaded JSON
     */
    static bool CompileFromJson(
        const nlohmann::json& metaJson,
        RuntimeModelManifest& outManifest,
        std::function<ClaymoreGUID(const std::string&)> pathToGuidResolver
    );
    
    /**
     * Write a single RuntimeModelManifest to binary format
     */
    static bool WriteBinary(const RuntimeModelManifest& manifest, std::vector<uint8_t>& outData);
    
    /**
     * Write single manifest to file (legacy per-model format)
     */
    static bool WriteToFile(const RuntimeModelManifest& manifest, const std::string& outputPath);
    
    /**
     * Write centralized model registry containing all manifests
     * This is the production format - single file loaded at startup
     */
    static bool WriteRegistry(const std::vector<RuntimeModelManifest>& manifests, 
                              std::vector<uint8_t>& outData);
    
    /**
     * Write registry to file
     */
    static bool WriteRegistryToFile(const std::vector<RuntimeModelManifest>& manifests,
                                     const std::string& outputPath);
};

} // namespace cm

