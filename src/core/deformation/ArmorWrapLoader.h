#pragma once
#include <string>
#include <memory>
#include "ArmorWrapTypes.h"
#include "ArmorFitComponent.h"

namespace cm { namespace deformation {

// ============================================================================
// ArmorWrapLoader - Runtime .wrapbin loader
// ============================================================================
// Loads .wrapbin files directly into ArmorWrapData.
// This is the RUNTIME loader - it performs NO JSON parsing.
// The binary is loaded directly into memory and interpreted as native structs.
// ============================================================================

class ArmorWrapLoader
{
public:
    // Load a .wrapbin file into an ArmorWrapData structure
    // Returns nullptr on failure (file not found, invalid format, etc.)
    static std::shared_ptr<ArmorWrapData> Load(const std::string& wrapBinPath);
    
    // Validate a .wrapbin file header without loading the full data
    // Returns true if the file exists and has a valid header
    static bool Validate(const std::string& wrapBinPath);
    
    // Get the vertex count from a .wrapbin header without loading data
    static uint32_t GetVertexCount(const std::string& wrapBinPath);
};

}} // namespace cm::deformation

