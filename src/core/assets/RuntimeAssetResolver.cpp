#include "RuntimeAssetResolver.h"
#include "core/resources/ResourceManifest.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <filesystem>
#include <unordered_set>

RuntimeAssetResolver::RuntimeAssetResolver() {
    // Register as global resolver
    Assets::SetResolver(this);
}

bool RuntimeAssetResolver::LoadManifest(const std::string& manifestJson) {
    try {
        nlohmann::json j = nlohmann::json::parse(manifestJson);
        
        if (j.contains("assetMap") && j["assetMap"].is_array()) {
            for (const auto& entry : j["assetMap"]) {
                if (entry.contains("guid") && entry.contains("path")) {
                    std::string guidStr = entry["guid"].get<std::string>();
                    std::string path = entry["path"].get<std::string>();
                    
                    m_GuidToPath[guidStr] = path;
                    
                    ClaymoreGUID guid = ClaymoreGUID::FromString(guidStr);
                    m_PathToGuid[path] = guid;
                }
            }
        }
        
        std::cout << "[RuntimeAssetResolver] Loaded " << m_GuidToPath.size() << " asset mappings" << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[RuntimeAssetResolver] Failed to load manifest: " << e.what() << std::endl;
        return false;
    }
}

std::string RuntimeAssetResolver::ResolvePath(const std::string& virtualPath) const {
    // In runtime mode, paths are already virtual/binary paths from PAK
    // Just normalize the path
    std::string normalized = virtualPath;
    for (char& c : normalized) {
        if (c == '\\') c = '/';
    }
    return normalized;
}

std::string RuntimeAssetResolver::GetBinaryPath(const std::string& sourcePath) const {
    // In runtime, everything is already binary - just change extension
    std::filesystem::path p(sourcePath);
    std::string ext = p.extension().string();
    
    // Map source extensions to binary extensions
    if (ext == ".scene") return p.replace_extension(".sceneb").string();
    if (ext == ".mat") return p.replace_extension(".matbin").string();
    if (ext == ".prefab") return p.replace_extension(".prefabb").string();
    if (ext == ".anim") return p.replace_extension(".animbin").string();
    if (ext == ".animctrl" || ext == ".controller") return p.replace_extension(".actrlbin").string();
    
    // For other assets, return as-is
    return sourcePath;
}

bool RuntimeAssetResolver::IsBinaryCurrent(const std::string& /*sourcePath*/) const {
    // In runtime, binaries are always "current" (we only have binaries)
    return true;
}

ClaymoreGUID RuntimeAssetResolver::GetGUID(const std::string& path) const {
    // Normalize path
    std::string normalized = path;
    for (char& c : normalized) {
        if (c == '\\') c = '/';
    }
    
    // Try exact match
    auto it = m_PathToGuid.find(normalized);
    if (it != m_PathToGuid.end()) {
        return it->second;
    }
    
    // Try stripping to assets/ prefix
    auto pos = normalized.find("assets/");
    if (pos != std::string::npos) {
        std::string rel = normalized.substr(pos);
        it = m_PathToGuid.find(rel);
        if (it != m_PathToGuid.end()) {
            return it->second;
        }
    }
    
    return ClaymoreGUID();
}


std::string RuntimeAssetResolver::GetPathForGUID(const ClaymoreGUID& guid) const {
    // Check if GUID is valid (non-zero)
    if (guid.high == 0 && guid.low == 0) return "";
    
    std::string guidStr = guid.ToString();
    auto it = m_GuidToPath.find(guidStr);
    if (it != m_GuidToPath.end()) {
        return it->second;
    }
    
    return "";
}

void RuntimeAssetResolver::GetAssetsByPathPrefix(const std::string& prefix,
                                                 std::vector<std::pair<ClaymoreGUID, std::string>>& out) const {
    out.clear();
    if (prefix.empty()) return;

    std::string normPrefix = prefix;
    for (char& c : normPrefix) {
        if (c == '\\') c = '/';
    }
    if (normPrefix.back() != '/') {
        normPrefix.push_back('/');
    }

    std::unordered_set<ClaymoreGUID> seen;
    seen.reserve(m_PathToGuid.size());

    for (const auto& [path, guid] : m_PathToGuid) {
        if (guid == ClaymoreGUID()) continue;
        std::string normPath = path;
        for (char& c : normPath) {
            if (c == '\\') c = '/';
        }
        if (normPath.rfind(normPrefix, 0) == 0) {
            if (seen.insert(guid).second) {
                out.emplace_back(guid, normPath);
            }
        }
    }
}

bool RuntimeAssetResolver::LoadResourceManifest(const std::string& resourceManifestJson) {
    // Use the ResourceManifest singleton to load directly from JSON string
    bool result = ResourceManifest::Get().LoadFromJson(resourceManifestJson);
    if (result) {
        std::cout << "[RuntimeAssetResolver] Resource manifest loaded successfully" << std::endl;
    }
    return result;
}

