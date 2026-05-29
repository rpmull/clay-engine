#pragma once
#include <string>
#include <cstdint>
#include <functional>
#include "AssetReference.h"  // Use existing ClaymoreGUID definition

// ClaymoreGUIDHasher - use the hash from AssetReference.h via std::hash<ClaymoreGUID>
struct ClaymoreGUIDHasher {
    size_t operator()(const ClaymoreGUID& g) const {
        return std::hash<ClaymoreGUID>()(g);
    }
};

// Asset resolution mode - determines where we load assets from
enum class AssetLoadMode {
    Editor,     // Load from source files (JSON, etc.) for editing
    PlayMode,   // Load from binary cache for play mode
    Runtime     // Load from PAK (exported build)
};

// Abstract interface for resolving asset paths
// Editor provides full implementation, Runtime provides VFS-only stub
class IAssetResolver {
public:
    virtual ~IAssetResolver() = default;

    // Resolve a virtual path (e.g., "assets/materials/Metal.mat") to loadable path
    // In editor mode: returns the source path
    // In play mode: returns the binary cache path
    // In runtime: returns PAK-relative path
    virtual std::string ResolvePath(const std::string& virtualPath) const = 0;

    // Get the binary cache path for a source asset
    // Returns empty if no binary exists
    virtual std::string GetBinaryPath(const std::string& sourcePath) const = 0;

    // Check if a binary cache exists and is up-to-date for the source
    virtual bool IsBinaryCurrent(const std::string& sourcePath) const = 0;

    // Get GUID for an asset path
    virtual ClaymoreGUID GetGUID(const std::string& path) const = 0;

    // Get path for a GUID
    virtual std::string GetPathForGUID(const ClaymoreGUID& guid) const = 0;

    // Get current load mode
    virtual AssetLoadMode GetLoadMode() const = 0;
    
    // Whether source (JSON) fallback is allowed when binary load fails
    virtual bool AllowSourceFallback() const { return true; }

    // Check if we should load binaries (play mode or runtime)
    bool ShouldLoadBinary() const {
        auto mode = GetLoadMode();
        return mode == AssetLoadMode::PlayMode || mode == AssetLoadMode::Runtime;
    }
};

// Global asset resolver - set by editor or runtime at startup
namespace Assets {
    IAssetResolver* GetResolver();
    void SetResolver(IAssetResolver* resolver);
    
    // Convenience functions
    inline std::string ResolvePath(const std::string& path) {
        return GetResolver() ? GetResolver()->ResolvePath(path) : path;
    }
    
    inline std::string GetBinaryPath(const std::string& path) {
        return GetResolver() ? GetResolver()->GetBinaryPath(path) : "";
    }
    
    inline bool ShouldLoadBinary() {
        return GetResolver() ? GetResolver()->ShouldLoadBinary() : false;
    }
    
    inline AssetLoadMode GetLoadMode() {
        return GetResolver() ? GetResolver()->GetLoadMode() : AssetLoadMode::Runtime;
    }

    inline bool AllowSourceFallback() {
        return GetResolver() ? GetResolver()->AllowSourceFallback() : true;
    }
}

