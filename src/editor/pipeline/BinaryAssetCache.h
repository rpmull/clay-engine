#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <filesystem>
#include <vector>
#include "core/assets/IAssetResolver.h"

// BinaryAssetCache: Manages the hidden .bin folder for runtime binaries
// 
// Key responsibilities:
// 1. Generate binary assets from source files (.scene -> .sceneb, .mat -> .matbin, etc.)
// 2. Track freshness - rebuild when source is modified
// 3. Provide transparent path resolution for play mode
// 4. Hide cache from project browser
//
// Directory structure:
// .bin/
//   scenes/       <- .sceneb files
//   materials/    <- .matbin files  
//   prefabs/      <- .prefabb files
//   animations/   <- .animbin files
//   animators/    <- .actrlbin files (animator controllers)
//   manifest.json <- maps source paths to cache paths with timestamps
//
// Note: .library/ is reserved for Roslyn-compiled GameScripts.dll

class BinaryAssetCache {
public:
    static BinaryAssetCache& Instance();
    
    // Initialize cache for project
    void Initialize(const std::filesystem::path& projectRoot);
    
    // Shutdown and cleanup
    void Shutdown();
    
    // Check if binary cache exists and is up-to-date for source
    bool IsCurrent(const std::string& sourcePath) const;
    
    // Get binary cache path for source (creates directories if needed)
    std::string GetBinaryPath(const std::string& sourcePath) const;
    
    // Ensure binary is up-to-date, building if necessary
    // Returns true if binary is ready, false on failure
    bool EnsureBinary(const std::string& sourcePath);
    
    // Force rebuild of binary for source
    bool RebuildBinary(const std::string& sourcePath);
    
    // Invalidate binary cache for source
    void Invalidate(const std::string& sourcePath);
    
    // Invalidate all binaries
    void InvalidateAll();
    
    // Rebuild all binaries (for clean build)
    void RebuildAll();
    
    // Build all outdated binaries (for entering play mode)
    void EnsureAllCurrent();

    struct EnsureStats {
        size_t inspected = 0;
        size_t alreadyCurrent = 0;
        size_t rebuilt = 0;
        size_t failed = 0;
    };

    // Build only stale binaries reachable from a scene dependency graph.
    bool EnsureSceneDependenciesCurrent(const std::string& scenePath,
                                        bool includeEntrySceneBinary,
                                        EnsureStats* outStats = nullptr);

    // Build only stale binaries among the supplied dependency paths.
    bool EnsureDependenciesCurrent(const std::vector<std::string>& dependencyPaths,
                                   EnsureStats* outStats = nullptr);

    // Collect authoring assets that are missing binaries
    bool CollectMissingBinaries(std::vector<std::string>& outMissing) const;
    
    // Get cache root directory
    const std::filesystem::path& GetCacheRoot() const { return m_CacheRoot; }
    
    // Check if a path is within the cache (for hiding from project browser)
    bool IsInCache(const std::filesystem::path& path) const;
    
    // Asset type detection
    enum class AssetType {
        Unknown,
        Scene,
        Material,
        Prefab,
        Animation,
        AnimatorController,
        // Add more as needed
    };
    
    static AssetType GetAssetType(const std::string& path);
    static std::string GetBinaryExtension(AssetType type);

    bool BuildAnimationBinary(const std::string& sourcePath, const std::string& binaryPath);

private:
    BinaryAssetCache() = default;
    ~BinaryAssetCache() = default;
    
    // Build binary for specific asset type
    bool BuildSceneBinary(const std::string& sourcePath, const std::string& binaryPath);
    bool BuildMaterialBinary(const std::string& sourcePath, const std::string& binaryPath);
    bool BuildPrefabBinary(const std::string& sourcePath, const std::string& binaryPath);
    
    bool BuildAnimatorControllerBinary(const std::string& sourcePath, const std::string& binaryPath);
    
    // Manifest management
    void LoadManifest();
    void SaveManifest();
    
    // Path utilities
    std::string MakeCacheKey(const std::string& sourcePath) const;
    std::filesystem::path SourceToCache(const std::string& sourcePath, AssetType type) const;
    
    struct CacheEntry {
        std::string binaryPath;
        uint64_t sourceModTime = 0;
        uint64_t binaryModTime = 0;
    };
    
    std::filesystem::path m_ProjectRoot;
    std::filesystem::path m_CacheRoot;
    std::unordered_map<std::string, CacheEntry> m_Manifest;
    mutable std::mutex m_Mutex;
    bool m_Initialized = false;
};

// EditorAssetResolver: Implementation of IAssetResolver for editor
// Provides transparent binary resolution based on load mode
class EditorAssetResolver : public IAssetResolver {
public:
    EditorAssetResolver();
    ~EditorAssetResolver() override = default;
    
    // Set load mode (editor vs play mode)
    void SetLoadMode(AssetLoadMode mode) { m_Mode = mode; }
    void SetPlayModeBinaryOnly(bool enabled) { m_PlayModeBinaryOnly = enabled; }
    bool IsPlayModeBinaryOnly() const { return m_PlayModeBinaryOnly; }
    
    // IAssetResolver interface
    std::string ResolvePath(const std::string& virtualPath) const override;
    std::string GetBinaryPath(const std::string& sourcePath) const override;
    bool IsBinaryCurrent(const std::string& sourcePath) const override;
    ClaymoreGUID GetGUID(const std::string& path) const override;
    std::string GetPathForGUID(const ClaymoreGUID& guid) const override;
    AssetLoadMode GetLoadMode() const override { return m_Mode; }
    bool AllowSourceFallback() const override;

private:
    AssetLoadMode m_Mode = AssetLoadMode::Editor;
    bool m_PlayModeBinaryOnly = false;
};


