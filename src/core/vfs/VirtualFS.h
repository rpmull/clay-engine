#pragma once
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

// Abstract interface for virtual file system access
// Runtime uses PakReader (read-only from .pak)
// Editor uses EditorVFS (disk with pak fallback)
class IVirtualFS {
public:
    virtual ~IVirtualFS() = default;

    // Read entire file contents into buffer
    virtual bool ReadFile(const std::string& path, std::vector<uint8_t>& outData) = 0;
    
    // Read file as text (convenience)
    virtual bool ReadTextFile(const std::string& path, std::string& outText) {
        std::vector<uint8_t> data;
        if (!ReadFile(path, data)) return false;
        outText.assign(reinterpret_cast<const char*>(data.data()), data.size());
        return true;
    }
    
    // Check if file exists
    virtual bool Exists(const std::string& path) = 0;
    
    // Check if path is a directory (optional - not all VFS support this)
    virtual bool IsDirectory(const std::string& path) { return false; }
    
    // List directory contents (optional - runtime pak doesn't support this)
    virtual std::vector<std::string> ListDirectory(const std::string& path) { return {}; }
    
    // List all files in VFS (for filename-based texture search fallback)
    virtual std::vector<std::string> ListAllFiles() { return {}; }

    // Normalize path to consistent format (forward slashes, resolves .. and .)
    static std::string NormalizePath(const std::string& path);
    
    // Normalize path with case folding (lowercase) for case-insensitive PAK matching
    static std::string NormalizePathLowercase(const std::string& path);
};

// Global VFS instance - set at startup by editor or runtime
// All file access should go through this
namespace VFS {
    // Get the active VFS instance
    IVirtualFS* Get();
    
    // Set the active VFS instance (takes ownership)
    void Set(std::unique_ptr<IVirtualFS> vfs);

    // Shared virtual path prefixes (assets/, shaders/, etc.)
    const std::vector<std::string>& GetKnownPrefixes();
    
    // Strip to the first known prefix in a normalized path; returns empty if none
    std::string StripToKnownPrefix(const std::string& path);
    
    // Convenience functions that delegate to Get()
    inline bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
        return Get() ? Get()->ReadFile(path, out) : false;
    }
    
    inline bool ReadTextFile(const std::string& path, std::string& out) {
        return Get() ? Get()->ReadTextFile(path, out) : false;
    }
    
    inline bool Exists(const std::string& path) {
        return Get() ? Get()->Exists(path) : false;
    }
}







