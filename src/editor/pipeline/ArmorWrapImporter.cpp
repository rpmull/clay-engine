#include "ArmorWrapImporter.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <cmath>
#include "editor/Project.h"
#include "editor/pipeline/BinaryAssetCache.h"

namespace editor {

std::string ArmorWrapImporter::s_LastError;

// ============================================================================
// HasWrapSidecar
// ============================================================================
bool ArmorWrapImporter::HasWrapSidecar(const std::string& meshPath)
{
    return std::filesystem::exists(GetSidecarPath(meshPath));
}

// ============================================================================
// GetSidecarPath
// ============================================================================
std::string ArmorWrapImporter::GetSidecarPath(const std::string& meshPath)
{
    namespace fs = std::filesystem;
    fs::path p(meshPath);
    
    // Replace extension with .wrap.json
    // e.g., "armor.fbx" -> "armor.wrap.json"
    fs::path sidecar = p.parent_path() / (p.stem().string() + ".wrap.json");
    return sidecar.string();
}

// ============================================================================
// GetWrapBinCachePath
// ============================================================================
std::string ArmorWrapImporter::GetWrapBinCachePath(const std::string& jsonPath)
{
    namespace fs = std::filesystem;
    if (jsonPath.empty()) return {};
    
    fs::path proj = Project::GetProjectDirectory();
    fs::path relPath = jsonPath;
    if (!proj.empty()) {
        std::error_code ec;
        fs::path candidate = fs::relative(jsonPath, proj, ec);
        if (!ec) {
            relPath = candidate;
        }
    }
    
    std::string rel = relPath.generic_string();
    fs::path keyPath(rel);
    std::string baseName = keyPath.filename().string();
    const std::string suffix = ".wrap.json";
    if (baseName.size() > suffix.size() && baseName.rfind(suffix) == baseName.size() - suffix.size()) {
        baseName = baseName.substr(0, baseName.size() - suffix.size());
    }
    std::string flatParent = keyPath.parent_path().string();
    for (char& c : flatParent) {
        if (c == '/' || c == '\\') c = '_';
    }
    std::string fileName = flatParent.empty() ? baseName : (flatParent + "_" + baseName);
    
    fs::path out = BinaryAssetCache::Instance().GetCacheRoot() / "wraps" / (fileName + ".wrapbin");
    return out.string();
}

// ============================================================================
// LoadWrapJson
// ============================================================================
std::vector<ArmorWrapJsonEntry> ArmorWrapImporter::LoadWrapJson(const std::string& jsonPath)
{
    std::vector<ArmorWrapJsonEntry> result;
    s_LastError.clear();
    
    std::ifstream in(jsonPath);
    if (!in.is_open())
    {
        s_LastError = "Failed to open JSON file: " + jsonPath;
        return result;
    }
    
    try
    {
        nlohmann::json doc;
        in >> doc;
        
        if (!doc.contains("wrap") || !doc["wrap"].is_array())
        {
            s_LastError = "JSON missing 'wrap' array";
            return result;
        }
        
        const auto& wrapArray = doc["wrap"];
        result.reserve(wrapArray.size());
        
        for (size_t i = 0; i < wrapArray.size(); ++i)
        {
            const auto& entry = wrapArray[i];
            
            ArmorWrapJsonEntry e{};
            
            // Required fields
            if (!entry.contains("tri") || !entry["tri"].is_number())
            {
                s_LastError = "Entry " + std::to_string(i) + " missing or invalid 'tri'";
                return {};
            }
            e.tri = entry["tri"].get<uint32_t>();
            
            if (!entry.contains("w0") || !entry["w0"].is_number())
            {
                s_LastError = "Entry " + std::to_string(i) + " missing or invalid 'w0'";
                return {};
            }
            e.w0 = entry["w0"].get<float>();
            
            if (!entry.contains("w1") || !entry["w1"].is_number())
            {
                s_LastError = "Entry " + std::to_string(i) + " missing or invalid 'w1'";
                return {};
            }
            e.w1 = entry["w1"].get<float>();
            
            if (!entry.contains("w2") || !entry["w2"].is_number())
            {
                s_LastError = "Entry " + std::to_string(i) + " missing or invalid 'w2'";
                return {};
            }
            e.w2 = entry["w2"].get<float>();
            
            // Optional fields with defaults
            e.weight = entry.value("weight", 1.0f);
            e.flags = entry.value("flags", 0);
            
            result.push_back(e);
        }
        
        std::cout << "[ArmorWrapImporter] Loaded " << result.size() 
                  << " wrap entries from: " << jsonPath << std::endl;
    }
    catch (const nlohmann::json::exception& ex)
    {
        s_LastError = std::string("JSON parse error: ") + ex.what();
        return {};
    }
    
    return result;
}

// ============================================================================
// ValidateWrapData
// ============================================================================
ArmorWrapValidationResult ArmorWrapImporter::ValidateWrapData(
    const std::vector<ArmorWrapJsonEntry>& entries,
    uint32_t armorVertexCount,
    uint32_t bodyTriangleCount)
{
    ArmorWrapValidationResult result;
    result.vertexCount = static_cast<uint32_t>(entries.size());
    
    // Check vertex count match
    if (entries.size() != armorVertexCount)
    {
        result.errorMessage = "Vertex count mismatch: wrap has " + 
            std::to_string(entries.size()) + " entries, armor mesh has " +
            std::to_string(armorVertexCount) + " vertices";
        return result;
    }
    
    // Validate each entry
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const auto& e = entries[i];
        
        // Triangle index bounds check
        if (e.tri >= bodyTriangleCount)
        {
            result.errorMessage = "Entry " + std::to_string(i) + 
                ": triangle index " + std::to_string(e.tri) + 
                " >= body triangle count " + std::to_string(bodyTriangleCount);
            return result;
        }
        
        result.maxTriIndex = std::max(result.maxTriIndex, e.tri);
        
        // Barycentric weight validation
        // w0, w1, w2 should each be in [0, 1]
        if (e.w0 < -0.01f || e.w0 > 1.01f)
        {
            result.errorMessage = "Entry " + std::to_string(i) + 
                ": w0 out of range: " + std::to_string(e.w0);
            return result;
        }
        
        if (e.w1 < -0.01f || e.w1 > 1.01f)
        {
            result.errorMessage = "Entry " + std::to_string(i) + 
                ": w1 out of range: " + std::to_string(e.w1);
            return result;
        }
        
        if (e.w2 < -0.01f || e.w2 > 1.01f)
        {
            result.errorMessage = "Entry " + std::to_string(i) + 
                ": w2 out of range: " + std::to_string(e.w2);
            return result;
        }
        
        // Barycentric sum should be ~1.0 (allow small float error)
        const float sum = e.w0 + e.w1 + e.w2;
        if (std::abs(sum - 1.0f) > 0.05f)
        {
            result.errorMessage = "Entry " + std::to_string(i) + 
                ": barycentric weights sum to " + std::to_string(sum) + 
                " (expected ~1.0)";
            return result;
        }
        
        // Wrap weight should be in [0, 1]
        if (e.weight < 0.0f || e.weight > 1.0f)
        {
            result.errorMessage = "Entry " + std::to_string(i) + 
                ": weight out of range: " + std::to_string(e.weight);
            return result;
        }
    }
    
    result.success = true;
    return result;
}

// ============================================================================
// ConvertToBinary
// ============================================================================
std::vector<cm::deformation::ArmorWrapInfluence> ArmorWrapImporter::ConvertToBinary(
    const std::vector<ArmorWrapJsonEntry>& entries)
{
    using namespace cm::deformation;
    
    std::vector<ArmorWrapInfluence> result;
    result.reserve(entries.size());
    
    for (const auto& e : entries)
    {
        ArmorWrapInfluence inf{};
        inf.triIndex = e.tri;
        inf.w0 = QuantizeWeight(e.w0);
        inf.w1 = QuantizeWeight(e.w1);
        inf.wrapWeight = QuantizeWeight(e.weight);
        inf.flags = e.flags;
        result.push_back(inf);
    }
    
    return result;
}

// ============================================================================
// WriteWrapBin
// ============================================================================
bool ArmorWrapImporter::WriteWrapBin(
    const std::string& outputPath,
    const std::vector<cm::deformation::ArmorWrapInfluence>& influences)
{
    using namespace cm::deformation;
    
    s_LastError.clear();
    
    // Ensure parent directory exists
    namespace fs = std::filesystem;
    fs::path outPath(outputPath);
    if (outPath.has_parent_path())
    {
        std::error_code ec;
        fs::create_directories(outPath.parent_path(), ec);
    }
    
    std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
    {
        s_LastError = "Failed to create output file: " + outputPath;
        return false;
    }
    
    // Write header
    WrapBinHeader header{};
    header.magic = WRAPBIN_MAGIC;
    header.version = WRAPBIN_VERSION;
    header.vertexCount = static_cast<uint32_t>(influences.size());
    header.flags = 0;
    
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    
    // Write influence data
    if (!influences.empty())
    {
        const size_t dataSize = influences.size() * sizeof(ArmorWrapInfluence);
        out.write(reinterpret_cast<const char*>(influences.data()), dataSize);
    }
    
    if (!out)
    {
        s_LastError = "Failed to write data to: " + outputPath;
        return false;
    }
    
    std::cout << "[ArmorWrapImporter] Wrote " << influences.size() 
              << " influences to: " << outputPath << std::endl;
    
    return true;
}

// ============================================================================
// ImportAndWriteWrapBin
// ============================================================================
bool ArmorWrapImporter::ImportAndWriteWrapBin(
    const std::string& jsonPath,
    const std::string& outputPath,
    uint32_t armorVertexCount,
    uint32_t bodyTriangleCount)
{
    s_LastError.clear();
    
    // Load JSON
    auto entries = LoadWrapJson(jsonPath);
    if (entries.empty())
    {
        // s_LastError already set by LoadWrapJson
        return false;
    }
    
    // Validate
    auto validation = ValidateWrapData(entries, armorVertexCount, bodyTriangleCount);
    if (!validation.success)
    {
        s_LastError = "Validation failed: " + validation.errorMessage;
        return false;
    }
    
    // Convert to binary
    auto binary = ConvertToBinary(entries);
    
    // Write output
    return WriteWrapBin(outputPath, binary);
}

// ============================================================================
// GetLastError
// ============================================================================
const std::string& ArmorWrapImporter::GetLastError()
{
    return s_LastError;
}

} // namespace editor

