#pragma once
#include "VirtualFS.h"
#include "PakReader.h"
#include <string>
#include <vector>
#include <filesystem>

// Editor-mode virtual filesystem
// Tries PAK first, falls back to disk
// Also supports write operations and directory listing
// 
// IMPORTANT: In runtime mode (when VFS::Get() is set externally),
// this class delegates read operations to VFS::Get() first before
// falling back to its own PAK/disk resolution. This ensures that
// exported games using PakReader via VFS::Set() work correctly.
class EditorVFS : public IVirtualFS {
public:
    static EditorVFS& Instance() {
        static EditorVFS fs;
        return fs;
    }

    // Mount a pak file for reading (pak takes priority over disk)
    bool MountPak(const std::string& pakPath);
    void UnmountPak();
    const std::string& GetMountedPakPath() const { return m_MountedPakPath; }
    
    // Check if a PAK is mounted either locally or via global VFS
    bool IsPakMounted() const;
    
    // Check if we're running in pure runtime mode (VFS::Get() is a PakReader)
    bool IsRuntimeMode() const;

    // Set project root directory for relative path resolution
    void SetProjectRoot(const std::filesystem::path& root) { m_ProjectRoot = root; }
    const std::filesystem::path& GetProjectRoot() const { return m_ProjectRoot; }
    
    // Allow or disallow disk fallback (runtime parity)
    void SetAllowDiskFallback(bool allowed) { m_AllowDiskFallback = allowed; }
    bool IsDiskFallbackAllowed() const { return m_AllowDiskFallback; }

    // IVirtualFS interface
    bool ReadFile(const std::string& path, std::vector<uint8_t>& outData) override;
    bool Exists(const std::string& path) override;
    bool IsDirectory(const std::string& path) override;
    std::vector<std::string> ListDirectory(const std::string& path) override;

    // Write operations (editor only)
    bool WriteFile(const std::string& path, const std::vector<uint8_t>& data);
    bool WriteTextFile(const std::string& path, const std::string& text);

private:
    EditorVFS() = default;
    ~EditorVFS() = default;

    // Try to resolve a virtual path to absolute disk path
    std::filesystem::path ResolvePath(const std::string& virtualPath) const;

    PakReader m_Pak;
    std::filesystem::path m_ProjectRoot;
    std::string m_MountedPakPath;
    bool m_AllowDiskFallback = true;
};

// Legacy compatibility - alias to EditorVFS
using FileSystem = EditorVFS;

