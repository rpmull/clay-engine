#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "core/deformation/ArmorWrapTypes.h"

namespace editor {

// ============================================================================
// ArmorWrapImporter - Editor-side JSON to binary converter
// ============================================================================
// This class handles the EDITOR-ONLY operations:
//   1. Reading armor.wrap.json sidecar files
//   2. Validating the JSON data
//   3. Converting to native ArmorWrapInfluence[] format
//   4. Writing .wrapbin files
//
// RUNTIME NEVER uses this class - it loads .wrapbin directly.
// ============================================================================

struct ArmorWrapJsonEntry
{
    uint32_t tri;       // Body triangle index
    float w0;           // Barycentric weight 0
    float w1;           // Barycentric weight 1
    float w2;           // Barycentric weight 2 (for validation; not stored)
    float weight;       // Per-vertex wrap blend strength (0-1)
    uint16_t flags;     // Optional flags
};

struct ArmorWrapValidationResult
{
    bool success = false;
    std::string errorMessage;
    uint32_t vertexCount = 0;
    uint32_t maxTriIndex = 0;
};

class ArmorWrapImporter
{
public:
    // ========================================================================
    // Import Pipeline
    // ========================================================================
    
    // Check if a wrap JSON sidecar exists for a mesh file
    // e.g., for "armor.fbx", looks for "armor.wrap.json"
    static bool HasWrapSidecar(const std::string& meshPath);
    
    // Get the sidecar path for a mesh file
    static std::string GetSidecarPath(const std::string& meshPath);

    // Get the cache path for a wrap JSON file (.bin/wraps/...)
    static std::string GetWrapBinCachePath(const std::string& jsonPath);
    
    // Load and parse wrap JSON file
    // Returns empty vector on failure (check error via GetLastError())
    static std::vector<ArmorWrapJsonEntry> LoadWrapJson(const std::string& jsonPath);
    
    // Validate wrap data against mesh/body constraints
    // - armorVertexCount: expected number of vertices in armor mesh
    // - bodyTriangleCount: number of triangles in body mesh (indices/3)
    static ArmorWrapValidationResult ValidateWrapData(
        const std::vector<ArmorWrapJsonEntry>& entries,
        uint32_t armorVertexCount,
        uint32_t bodyTriangleCount);
    
    // Convert JSON entries to native binary format
    static std::vector<cm::deformation::ArmorWrapInfluence> ConvertToBinary(
        const std::vector<ArmorWrapJsonEntry>& entries);
    
    // Write .wrapbin file from native struct array
    static bool WriteWrapBin(
        const std::string& outputPath,
        const std::vector<cm::deformation::ArmorWrapInfluence>& influences);
    
    // ========================================================================
    // Convenience: Full import pipeline in one call
    // ========================================================================
    
    // Import wrap JSON and write wrapbin in one step
    // Returns true on success, false on failure (check GetLastError())
    static bool ImportAndWriteWrapBin(
        const std::string& jsonPath,
        const std::string& outputPath,
        uint32_t armorVertexCount,
        uint32_t bodyTriangleCount);
    
    // Get the last error message
    static const std::string& GetLastError();
    
private:
    static std::string s_LastError;
};

} // namespace editor

