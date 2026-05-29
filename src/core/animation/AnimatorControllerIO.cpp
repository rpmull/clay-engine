#include "core/animation/AnimatorControllerIO.h"

#include "core/animation/AnimatorController.h"
#include "core/assets/IAssetResolver.h"
#include "core/vfs/FileSystem.h"
#include "core/vfs/VirtualFS.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <cstring>
#include <unordered_map>
#include <mutex>
#include <algorithm>
#include <vector>

#ifdef CLAYMORE_EDITOR
#include "editor/Project.h"
#include "editor/pipeline/BinaryAssetCache.h"
#endif

namespace cm {
namespace animation {

namespace {

// Binary format constants
constexpr uint32_t kCtrlBinMagic = 'A' | ('C' << 8) | ('T' << 16) | ('B' << 24);
constexpr uint32_t kCtrlBinVersion = 1;

#if defined(CLAYMORE_EDITOR)
bool TryEnsureBinaryForPlayMode(const std::string& sourcePath) {
    if (sourcePath.empty()) return false;
    if (Assets::GetLoadMode() != AssetLoadMode::PlayMode) return false;
    if (FileSystem::Instance().IsPakMounted()) return false;
    return BinaryAssetCache::Instance().EnsureBinary(sourcePath);
}
#endif

struct CachedController {
    std::shared_ptr<AnimatorController> controller;
    std::filesystem::file_time_type timestamp;
    bool hasTimestamp = false;
};

std::mutex g_controllerCacheMutex;
std::unordered_map<std::string, CachedController> g_controllerCache;

static std::string NormalizeCacheKey(const std::string& path)
{
    if (path.empty()) return {};
    try {
        return IVirtualFS::NormalizePath(path);
    } catch (...) {
        std::string normalized = path;
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        return normalized;
    }
}

static void AppendUnique(std::vector<std::string>& out, const std::string& candidate)
{
    if (candidate.empty()) return;
    if (std::find(out.begin(), out.end(), candidate) != out.end()) return;
    out.push_back(candidate);
}

static std::vector<std::string> BuildControllerPathCandidates(const std::string& path)
{
    std::vector<std::string> candidates;
    if (path.empty()) return candidates;
    
    std::string normalized = NormalizeCacheKey(path);
    AppendUnique(candidates, normalized);
    
    // Extract assets/ prefix if embedded in an absolute path
    size_t assetsPos = normalized.find("/assets/");
    if (assetsPos != std::string::npos) {
        AppendUnique(candidates, normalized.substr(assetsPos + 1));
    }
    assetsPos = normalized.find("assets/");
    if (assetsPos != std::string::npos && assetsPos > 0) {
        AppendUnique(candidates, normalized.substr(assetsPos));
    }
    
    // If path is relative and not already under assets/, try prefixing assets/
    try {
        std::filesystem::path p(normalized);
        if (!p.is_absolute() && normalized.find("assets/") != 0) {
            AppendUnique(candidates, std::string("assets/") + normalized);
        }
    } catch (...) {}
    
    return candidates;
}

static bool TryGetTimestamp(const std::string& path, std::filesystem::file_time_type& outTime)
{
    if (path.empty()) return false;
    std::error_code ec;
    std::filesystem::path p(path);
    if (!std::filesystem::exists(p, ec)) return false;
    outTime = std::filesystem::last_write_time(p, ec);
    return !ec;
}

static std::shared_ptr<AnimatorController> TryGetCached(const std::string& key, const std::string& timestampPath)
{
    if (key.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(g_controllerCacheMutex);
    auto it = g_controllerCache.find(key);
    if (it == g_controllerCache.end() || !it->second.controller) return nullptr;
    if (it->second.hasTimestamp) {
        std::filesystem::file_time_type current;
        if (TryGetTimestamp(timestampPath, current) && current != it->second.timestamp) {
            g_controllerCache.erase(it);
            return nullptr;
        }
    }
    return it->second.controller;
}

static void StoreCached(const std::string& key, const std::string& timestampPath, const std::shared_ptr<AnimatorController>& controller)
{
    if (!controller || key.empty()) return;
    CachedController entry;
    entry.controller = controller;
    std::filesystem::file_time_type ts;
    if (TryGetTimestamp(timestampPath, ts)) {
        entry.timestamp = ts;
        entry.hasTimestamp = true;
    }
    std::lock_guard<std::mutex> lock(g_controllerCacheMutex);
    g_controllerCache[key] = std::move(entry);
}

std::shared_ptr<AnimatorController> TryLoadControllerFromVFS(const std::string& path) {
    std::string text;
    if (!FileSystem::Instance().ReadTextFile(path, text)) {
        return nullptr;
    }
    try {
        nlohmann::json j = nlohmann::json::parse(text);
        auto ctrl = std::make_shared<AnimatorController>();
        nlohmann::from_json(j, *ctrl);
        ctrl->CompileRuntimeData();
        return ctrl;
    } catch (const std::exception& e) {
        std::cerr << "[AnimatorControllerIO] Failed to parse controller '" << path << "': " << e.what() << "\n";
        return nullptr;
    } catch (...) {
        std::cerr << "[AnimatorControllerIO] Failed to parse controller '" << path << "' (unknown error)\n";
        return nullptr;
    }
}

std::shared_ptr<AnimatorController> TryLoadControllerBinary(const std::string& path) {
    std::vector<uint8_t> data;
    
    // Try VFS first (for PAK files at runtime)
    if (VFS::Get() && VFS::Get()->ReadFile(path, data)) {
        // Successfully read from VFS
    } else if (!FileSystem::Instance().ReadFile(path, data)) {
        return nullptr;
    }
    
    if (data.size() < 12) return nullptr;
    
    // Read and validate header
    uint32_t magic = 0, version = 0, jsonLen = 0;
    std::memcpy(&magic, data.data(), 4);
    std::memcpy(&version, data.data() + 4, 4);
    std::memcpy(&jsonLen, data.data() + 8, 4);
    
    if (magic != kCtrlBinMagic || version != kCtrlBinVersion) {
        return nullptr;
    }
    
    if (jsonLen > data.size() - 12) return nullptr;
    
    try {
        std::string jsonStr(reinterpret_cast<const char*>(data.data() + 12), jsonLen);
        nlohmann::json j = nlohmann::json::parse(jsonStr);
        auto ctrl = std::make_shared<AnimatorController>();
        nlohmann::from_json(j, *ctrl);
        ctrl->CompileRuntimeData();
        return ctrl;
    } catch (const std::exception& e) {
        std::cerr << "[AnimatorControllerIO] Failed to parse binary controller '" << path << "': " << e.what() << "\n";
        return nullptr;
    }
}

} // namespace

std::shared_ptr<AnimatorController> LoadAnimatorControllerFromFile(const std::string& path) {
    if (path.empty()) return nullptr;

    const bool shouldLoadBinary = Assets::ShouldLoadBinary();
    const bool allowSourceFallback = Assets::AllowSourceFallback();
    const bool useBinaryCache = shouldLoadBinary && (Assets::GetLoadMode() == AssetLoadMode::PlayMode);

#ifdef CLAYMORE_EDITOR
    // In editor (including play mode), try project-relative path
    std::filesystem::path projectPath;
    try {
        projectPath = Project::GetProjectDirectory();
    } catch (...) {}
    
    // Build full path if we have project directory
    std::string fullPath = path;
    if (!projectPath.empty() && !std::filesystem::path(path).is_absolute()) {
        fullPath = (projectPath / path).string();
    }
    
    std::vector<std::string> candidates = BuildControllerPathCandidates(fullPath);
    if (fullPath != path) {
        for (const auto& extra : BuildControllerPathCandidates(path)) {
            AppendUnique(candidates, extra);
        }
    }
    
    // In play mode (not runtime), try loading from .bin cache first
    if (useBinaryCache) {
        for (const auto& candidate : candidates) {
            std::string binaryPath = BinaryAssetCache::Instance().GetBinaryPath(candidate);
            if (binaryPath.empty()) continue;
            const std::string key = NormalizeCacheKey(binaryPath);
            if (auto cached = TryGetCached(key, binaryPath)) {
                return cached;
            }
            if (auto ctrl = TryLoadControllerBinary(binaryPath)) {
                StoreCached(key, binaryPath, ctrl);
                return ctrl;
            }
#if defined(CLAYMORE_EDITOR)
            if (TryEnsureBinaryForPlayMode(candidate)) {
                binaryPath = BinaryAssetCache::Instance().GetBinaryPath(candidate);
                if (!binaryPath.empty()) {
                    const std::string compiledKey = NormalizeCacheKey(binaryPath);
                    if (auto cached = TryGetCached(compiledKey, binaryPath)) {
                        return cached;
                    }
                    if (auto ctrl = TryLoadControllerBinary(binaryPath)) {
                        StoreCached(compiledKey, binaryPath, ctrl);
                        return ctrl;
                    }
                }
            }
#endif
        }
    }
    
    // Try full path first
    if (allowSourceFallback) {
        for (const auto& candidate : candidates) {
            const std::string key = NormalizeCacheKey(candidate);
            if (auto cached = TryGetCached(key, candidate)) {
                return cached;
            }
            if (auto ctrl = TryLoadControllerFromVFS(candidate)) {
                StoreCached(key, candidate, ctrl);
                return ctrl;
            }
        }
    }
#endif

    // Try VFS with original path (handles pak files and falls back to disk)
    if (allowSourceFallback) {
        const std::string key = NormalizeCacheKey(path);
        if (auto cached = TryGetCached(key, path)) {
            return cached;
        }
        if (auto ctrl = TryLoadControllerFromVFS(path)) {
            StoreCached(key, path, ctrl);
            return ctrl;
        }
    }

    // Also try .actrlbin extension directly (for runtime/PAK)
    if (shouldLoadBinary) {
        for (const auto& candidate : BuildControllerPathCandidates(path)) {
            std::filesystem::path binPath(candidate);
            binPath.replace_extension(".actrlbin");
            const std::string binStr = binPath.string();
            const std::string key = NormalizeCacheKey(binStr);
            if (auto cached = TryGetCached(key, binStr)) {
                return cached;
            }
            if (auto ctrl = TryLoadControllerBinary(binStr)) {
                StoreCached(key, binStr, ctrl);
                return ctrl;
            }
        }
    }

    if (shouldLoadBinary && !allowSourceFallback) {
        std::cerr << "[AnimatorControllerIO] Missing binary controller: " << path << "\n";
    }

    return nullptr;
}

bool SaveAnimatorController(const AnimatorController& ctrl, const std::string& path) {
    // Save JSON source file
    try {
        nlohmann::json j;
        nlohmann::to_json(j, ctrl);
        
        std::ofstream out(path, std::ios::trunc);
        if (!out.is_open()) {
            std::cerr << "[AnimatorControllerIO] Failed to open file for writing: " << path << "\n";
            return false;
        }
        out << j.dump(4);
        out.close();
    } catch (const std::exception& e) {
        std::cerr << "[AnimatorControllerIO] Failed to save controller: " << e.what() << "\n";
        return false;
    }

#ifdef CLAYMORE_EDITOR
    // Update binary cache
    std::string binaryPath = BinaryAssetCache::Instance().GetBinaryPath(path);
    if (!binaryPath.empty()) {
        try {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(binaryPath).parent_path(), ec);
            
            nlohmann::json j;
            nlohmann::to_json(j, ctrl);
            std::string jsonStr = j.dump();
            
            std::ofstream out(binaryPath, std::ios::binary | std::ios::trunc);
            if (out.is_open()) {
                out.write(reinterpret_cast<const char*>(&kCtrlBinMagic), 4);
                out.write(reinterpret_cast<const char*>(&kCtrlBinVersion), 4);
                uint32_t jsonLen = static_cast<uint32_t>(jsonStr.size());
                out.write(reinterpret_cast<const char*>(&jsonLen), 4);
                out.write(jsonStr.data(), jsonLen);
            }
        } catch (const std::exception& e) {
            std::cerr << "[AnimatorControllerIO] Failed to update binary cache: " << e.what() << "\n";
        }
    }
#endif

    return true;
}

} // namespace animation
} // namespace cm
