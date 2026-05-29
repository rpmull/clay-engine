#include "FileSystem.h"
#include "VirtualFS.h"
#include <fstream>
#include <iostream>

bool EditorVFS::MountPak(const std::string& pakPath) {
    bool ok = m_Pak.Open(pakPath);
    m_MountedPakPath = ok ? pakPath : std::string();
    return ok;
}

void EditorVFS::UnmountPak() {
    m_Pak = PakReader();
    m_MountedPakPath.clear();
}

bool EditorVFS::IsPakMounted() const {
    // Check our own PAK first
    if (m_Pak.IsOpen()) return true;
    
    // Check if global VFS is set and is a PAK-based system
    // In runtime mode, VFS::Get() is set to a PakReader
    IVirtualFS* globalVfs = VFS::Get();
    if (globalVfs && globalVfs != this) {
        // If global VFS exists and it's not us, assume runtime PAK mode
        return true;
    }
    
    return false;
}

bool EditorVFS::IsRuntimeMode() const {
    // Runtime mode = VFS::Get() is set to something other than us
    IVirtualFS* globalVfs = VFS::Get();
    return globalVfs != nullptr && globalVfs != this;
}

std::filesystem::path EditorVFS::ResolvePath(const std::string& virtualPath) const {
    std::string normalized = IVirtualFS::NormalizePath(virtualPath);
    
    // If it's already an absolute path that exists, use it
    std::filesystem::path absPath(virtualPath);
    if (absPath.is_absolute() && std::filesystem::exists(absPath)) {
        return absPath;
    }
    
    // Try relative to project root
    if (!m_ProjectRoot.empty()) {
        std::filesystem::path projPath = m_ProjectRoot / normalized;
        if (std::filesystem::exists(projPath)) {
            return projPath;
        }
        
        // Try stripping to known prefixes
        const auto& prefixes = VFS::GetKnownPrefixes();
        for (const auto& prefix : prefixes) {
            auto pos = normalized.find(prefix);
            if (pos != std::string::npos) {
                std::filesystem::path alt = m_ProjectRoot / normalized.substr(pos);
                if (std::filesystem::exists(alt)) return alt;
            }
        }
    }
    
    // Try relative to current working directory
    std::filesystem::path cwdPath = std::filesystem::current_path() / normalized;
    if (std::filesystem::exists(cwdPath)) {
        return cwdPath;
    }
    
    // Return the original as-is (may not exist)
    return std::filesystem::path(virtualPath);
}

bool EditorVFS::ReadFile(const std::string& path, std::vector<uint8_t>& outData) {
    // CRITICAL: In runtime mode, delegate to global VFS first
    // This ensures exported games using PakReader via VFS::Set() work correctly
    IVirtualFS* globalVfs = VFS::Get();
    if (globalVfs && globalVfs != this) {
        if (globalVfs->ReadFile(path, outData)) {
            return true;
        }
        // Fallback: try normalized path variants for VFS
        std::string normalized = IVirtualFS::NormalizePath(path);
        if (normalized != path && globalVfs->ReadFile(normalized, outData)) {
            return true;
        }
    }
    
    // Try our own PAK
    if (m_Pak.IsOpen() && m_Pak.ReadFile(path, outData)) {
        return true;
    }

    bool allowDiskFallback = m_AllowDiskFallback;
    if (IsRuntimeMode()) allowDiskFallback = false;
#ifdef CLAYMORE_RUNTIME
    allowDiskFallback = false;
#endif

    if (!allowDiskFallback) {
        return false;
    }
    
    // Fall back to disk (editor mode)
    std::filesystem::path resolved = ResolvePath(path);
    std::ifstream in(resolved, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }
    
    in.seekg(0, std::ios::end);
    size_t size = static_cast<size_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    
    outData.resize(size);
    if (size != 0) {
        in.read(reinterpret_cast<char*>(outData.data()), static_cast<std::streamsize>(size));
    }
    
    return true;
}

bool EditorVFS::Exists(const std::string& path) {
    // CRITICAL: In runtime mode, delegate to global VFS first
    IVirtualFS* globalVfs = VFS::Get();
    if (globalVfs && globalVfs != this) {
        if (globalVfs->Exists(path)) {
            return true;
        }
        std::string normalized = IVirtualFS::NormalizePath(path);
        if (normalized != path && globalVfs->Exists(normalized)) {
            return true;
        }
    }
    
    // Check our own PAK
    if (m_Pak.IsOpen() && m_Pak.Exists(path)) {
        return true;
    }

    bool allowDiskFallback = m_AllowDiskFallback;
    if (IsRuntimeMode()) allowDiskFallback = false;
#ifdef CLAYMORE_RUNTIME
    allowDiskFallback = false;
#endif
    if (!allowDiskFallback) {
        return false;
    }
    
    // Check disk (editor mode)
    std::filesystem::path resolved = ResolvePath(path);
    return std::filesystem::exists(resolved);
}

bool EditorVFS::IsDirectory(const std::string& path) {
    bool allowDiskFallback = m_AllowDiskFallback;
    if (IsRuntimeMode()) allowDiskFallback = false;
#ifdef CLAYMORE_RUNTIME
    allowDiskFallback = false;
#endif
    if (!allowDiskFallback) {
        return false;
    }
    std::filesystem::path resolved = ResolvePath(path);
    return std::filesystem::is_directory(resolved);
}

std::vector<std::string> EditorVFS::ListDirectory(const std::string& path) {
    std::vector<std::string> result;
    bool allowDiskFallback = m_AllowDiskFallback;
    if (IsRuntimeMode()) allowDiskFallback = false;
#ifdef CLAYMORE_RUNTIME
    allowDiskFallback = false;
#endif
    if (!allowDiskFallback) {
        return result;
    }
    std::filesystem::path resolved = ResolvePath(path);
    
    std::error_code ec;
    for (auto& entry : std::filesystem::directory_iterator(resolved, ec)) {
        result.push_back(entry.path().filename().string());
    }
    
    return result;
}

bool EditorVFS::WriteFile(const std::string& path, const std::vector<uint8_t>& data) {
    std::filesystem::path resolved = ResolvePath(path);
    
    // Ensure parent directory exists
    std::error_code ec;
    std::filesystem::create_directories(resolved.parent_path(), ec);
    
    std::ofstream out(resolved, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    
    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    
    return true;
}

bool EditorVFS::WriteTextFile(const std::string& path, const std::string& text) {
    return WriteFile(path, std::vector<uint8_t>(text.begin(), text.end()));
}

