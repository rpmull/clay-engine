#include "VirtualFS.h"
#include <algorithm>
#include <vector>
#include <cctype>

// Global VFS instance
static std::unique_ptr<IVirtualFS> g_VFS;

namespace VFS {
    IVirtualFS* Get() {
        return g_VFS.get();
    }
    
    void Set(std::unique_ptr<IVirtualFS> vfs) {
        g_VFS = std::move(vfs);
    }

    const std::vector<std::string>& GetKnownPrefixes() {
        static const std::vector<std::string> prefixes = {
            "assets/",
            "shaders/",
            "scenes/",
            "resources/",
            ".bin/"
        };
        return prefixes;
    }

    std::string StripToKnownPrefix(const std::string& path) {
        std::string normalized = IVirtualFS::NormalizePath(path);
        size_t bestPos = std::string::npos;
        for (const auto& prefix : GetKnownPrefixes()) {
            auto pos = normalized.find(prefix);
            if (pos != std::string::npos && (bestPos == std::string::npos || pos < bestPos)) {
                bestPos = pos;
            }
        }
        if (bestPos == std::string::npos) {
            return "";
        }
        return normalized.substr(bestPos);
    }
}

std::string IVirtualFS::NormalizePath(const std::string& path) {
    std::string s = path;
    // Convert backslashes to forward slashes
    std::replace(s.begin(), s.end(), '\\', '/');
    
    // Split into components and resolve . and .. segments
    std::vector<std::string> components;
    std::string current;
    bool isAbsolute = !s.empty() && s[0] == '/';
    
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '/') {
            if (!current.empty()) {
                if (current == ".") {
                    // Skip current directory references
                } else if (current == "..") {
                    // Go up one directory if possible
                    if (!components.empty() && components.back() != "..") {
                        components.pop_back();
                    } else if (!isAbsolute) {
                        // Preserve leading .. for relative paths
                        components.push_back("..");
                    }
                    // For absolute paths, .. at root is ignored
                } else {
                    components.push_back(current);
                }
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    
    // Handle last component
    if (!current.empty()) {
        if (current == ".") {
            // Skip
        } else if (current == "..") {
            if (!components.empty() && components.back() != "..") {
                components.pop_back();
            } else if (!isAbsolute) {
                components.push_back("..");
            }
        } else {
            components.push_back(current);
        }
    }
    
    // Rebuild path
    std::string out;
    if (isAbsolute) out.push_back('/');
    for (size_t i = 0; i < components.size(); ++i) {
        if (i > 0) out.push_back('/');
        out += components[i];
    }
    
    return out.empty() ? "." : out;
}

std::string IVirtualFS::NormalizePathLowercase(const std::string& path) {
    std::string normalized = NormalizePath(path);
    // Convert to lowercase for case-insensitive matching
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return normalized;
}








