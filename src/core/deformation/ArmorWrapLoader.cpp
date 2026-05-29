#include "ArmorWrapLoader.h"
#include "core/vfs/FileSystem.h"
#include <fstream>
#include <iostream>
#include <filesystem>

namespace cm { namespace deformation {

// ============================================================================
// Helper: Ensure file is on disk (extract from VFS/pak if needed)
// ============================================================================
static std::string EnsureFileOnDisk(const std::string& filePath)
{
    namespace fs = std::filesystem;
    
    // If file exists on disk, use it directly
    if (fs::exists(filePath))
    {
        return filePath;
    }
    
    // Try to extract from virtual filesystem (pak)
    std::vector<uint8_t> data;
    if (FileSystem::Instance().ReadFile(filePath, data))
    {
        // Extract to temp cache
        fs::path cacheDir = fs::temp_directory_path() / "claymore_pak_cache";
        std::error_code ec;
        fs::create_directories(cacheDir, ec);
        
        // Create a unique cache path based on the original path
        size_t h = std::hash<std::string>{}(filePath);
        fs::path cachePath = cacheDir / ("wrapbin_" + std::to_string(h) + ".wrapbin");
        
        std::ofstream out(cachePath, std::ios::binary | std::ios::trunc);
        if (out.is_open() && !data.empty())
        {
            out.write(reinterpret_cast<const char*>(data.data()), data.size());
            out.close();
            return cachePath.string();
        }
    }
    
    // Return original path (will fail to open if doesn't exist)
    return filePath;
}

// ============================================================================
// ArmorWrapLoader::Load
// ============================================================================
std::shared_ptr<ArmorWrapData> ArmorWrapLoader::Load(const std::string& wrapBinPath)
{
    const std::string diskPath = EnsureFileOnDisk(wrapBinPath);
    
    std::ifstream in(diskPath, std::ios::binary);
    if (!in.is_open())
    {
        std::cerr << "[ArmorWrapLoader] Failed to open: " << wrapBinPath << std::endl;
        return nullptr;
    }
    
    // Read and validate header
    WrapBinHeader header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    
    if (!in)
    {
        std::cerr << "[ArmorWrapLoader] Failed to read header: " << wrapBinPath << std::endl;
        return nullptr;
    }
    
    if (header.magic != WRAPBIN_MAGIC)
    {
        std::cerr << "[ArmorWrapLoader] Invalid magic (expected WRPB): " << wrapBinPath << std::endl;
        return nullptr;
    }
    
    if (header.version != WRAPBIN_VERSION)
    {
        std::cerr << "[ArmorWrapLoader] Version mismatch (got " << header.version 
                  << ", expected " << WRAPBIN_VERSION << "): " << wrapBinPath << std::endl;
        return nullptr;
    }
    
    if (header.vertexCount == 0)
    {
        std::cerr << "[ArmorWrapLoader] Empty wrap data: " << wrapBinPath << std::endl;
        return nullptr;
    }
    
    // Sanity check: reasonable vertex count (prevent massive allocations on corrupt files)
    constexpr uint32_t MAX_REASONABLE_VERTICES = 10000000; // 10M vertices max
    if (header.vertexCount > MAX_REASONABLE_VERTICES)
    {
        std::cerr << "[ArmorWrapLoader] Unreasonable vertex count (" << header.vertexCount 
                  << "): " << wrapBinPath << std::endl;
        return nullptr;
    }
    
    // Allocate and read influence data directly
    auto wrapData = std::make_shared<ArmorWrapData>();
    wrapData->Influences.resize(header.vertexCount);
    wrapData->SourcePath = wrapBinPath;
    
    const size_t dataSize = header.vertexCount * sizeof(ArmorWrapInfluence);
    in.read(reinterpret_cast<char*>(wrapData->Influences.data()), dataSize);
    
    if (!in)
    {
        std::cerr << "[ArmorWrapLoader] Failed to read influence data: " << wrapBinPath << std::endl;
        return nullptr;
    }
    
    std::cout << "[ArmorWrapLoader] Loaded " << header.vertexCount 
              << " wrap influences from: " << wrapBinPath << std::endl;
    
    return wrapData;
}

// ============================================================================
// ArmorWrapLoader::Validate
// ============================================================================
bool ArmorWrapLoader::Validate(const std::string& wrapBinPath)
{
    const std::string diskPath = EnsureFileOnDisk(wrapBinPath);
    
    std::ifstream in(diskPath, std::ios::binary);
    if (!in.is_open())
    {
        return false;
    }
    
    WrapBinHeader header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    
    if (!in)
    {
        return false;
    }
    
    return (header.magic == WRAPBIN_MAGIC && header.version == WRAPBIN_VERSION);
}

// ============================================================================
// ArmorWrapLoader::GetVertexCount
// ============================================================================
uint32_t ArmorWrapLoader::GetVertexCount(const std::string& wrapBinPath)
{
    const std::string diskPath = EnsureFileOnDisk(wrapBinPath);
    
    std::ifstream in(diskPath, std::ios::binary);
    if (!in.is_open())
    {
        return 0;
    }
    
    WrapBinHeader header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    
    if (!in || header.magic != WRAPBIN_MAGIC)
    {
        return 0;
    }
    
    return header.vertexCount;
}

}} // namespace cm::deformation

