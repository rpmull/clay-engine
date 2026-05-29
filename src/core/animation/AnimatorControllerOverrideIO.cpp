#include "core/animation/AnimatorControllerOverrideIO.h"

#include "core/animation/AnimatorControllerOverride.h"
#include "core/vfs/FileSystem.h"
#include "core/vfs/VirtualFS.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <unordered_map>

#ifdef CLAYMORE_EDITOR
#include "editor/Project.h"
#endif

namespace cm {
namespace animation {

namespace {

std::string NormalizeSlashes(std::string value)
{
    for (char& c : value) {
        if (c == '\\') c = '/';
    }
    return value;
}

void AppendUnique(std::vector<std::string>& out, const std::string& candidate)
{
    if (candidate.empty()) return;
    if (std::find(out.begin(), out.end(), candidate) != out.end()) return;
    out.push_back(candidate);
}

std::vector<std::string> BuildPathCandidates(const std::string& path)
{
    std::vector<std::string> candidates;
    if (path.empty()) return candidates;

    std::string normalized = NormalizeSlashes(path);
    AppendUnique(candidates, normalized);

    size_t assetsPos = normalized.find("/assets/");
    if (assetsPos != std::string::npos) {
        AppendUnique(candidates, normalized.substr(assetsPos + 1));
    }

    assetsPos = normalized.find("assets/");
    if (assetsPos != std::string::npos && assetsPos > 0) {
        AppendUnique(candidates, normalized.substr(assetsPos));
    }

    try {
        std::filesystem::path p(normalized);
        if (!p.is_absolute() && normalized.rfind("assets/", 0) != 0) {
            AppendUnique(candidates, std::string("assets/") + normalized);
        }
    } catch (...) {}

    return candidates;
}

struct CachedOverride {
    std::shared_ptr<AnimatorControllerOverrideAsset> asset;
    std::filesystem::file_time_type timestamp;
    bool hasTimestamp = false;
};

std::mutex g_overrideCacheMutex;
std::unordered_map<std::string, CachedOverride> g_overrideCache;

std::string NormalizeCacheKey(const std::string& path)
{
    if (path.empty()) return {};
    std::string normalized = NormalizeAnimatorOverridePath(path);
    if (!normalized.empty()) {
        return normalized;
    }
    return NormalizeSlashes(path);
}

bool TryGetTimestamp(const std::string& path, std::filesystem::file_time_type& outTime)
{
    if (path.empty()) return false;
    std::error_code ec;
    std::filesystem::path p(path);
    if (!std::filesystem::exists(p, ec)) return false;
    outTime = std::filesystem::last_write_time(p, ec);
    return !ec;
}

std::shared_ptr<AnimatorControllerOverrideAsset> TryGetCached(const std::string& key,
                                                              const std::string& timestampPath)
{
    if (key.empty()) return nullptr;

    std::lock_guard<std::mutex> lock(g_overrideCacheMutex);
    auto it = g_overrideCache.find(key);
    if (it == g_overrideCache.end() || !it->second.asset) return nullptr;

    if (it->second.hasTimestamp) {
        std::filesystem::file_time_type current;
        if (TryGetTimestamp(timestampPath, current) && current != it->second.timestamp) {
            g_overrideCache.erase(it);
            return nullptr;
        }
    }

    return it->second.asset;
}

void StoreCached(const std::string& key,
                 const std::string& timestampPath,
                 const std::shared_ptr<AnimatorControllerOverrideAsset>& asset)
{
    if (!asset || key.empty()) return;

    CachedOverride entry;
    entry.asset = asset;
    std::filesystem::file_time_type ts;
    if (TryGetTimestamp(timestampPath, ts)) {
        entry.timestamp = ts;
        entry.hasTimestamp = true;
    }

    std::lock_guard<std::mutex> lock(g_overrideCacheMutex);
    g_overrideCache[key] = std::move(entry);
}

std::shared_ptr<AnimatorControllerOverrideAsset> TryLoadOverride(const std::string& path)
{
    std::string text;
    if (!FileSystem::Instance().ReadTextFile(path, text)) {
        return nullptr;
    }

    try {
        nlohmann::json j = nlohmann::json::parse(text);
        auto asset = std::make_shared<AnimatorControllerOverrideAsset>();
        nlohmann::from_json(j, *asset);
        asset->RebuildLookup();
        return asset;
    } catch (const std::exception& ex) {
        std::cerr << "[AnimatorControllerOverrideIO] Failed to parse override '" << path
                  << "': " << ex.what() << "\n";
        return nullptr;
    }
}

} // namespace

std::string NormalizeAnimatorOverridePath(const std::string& path)
{
    if (path.empty()) return {};
    std::string normalized = NormalizeSlashes(path);

    size_t assetsPos = normalized.find("/assets/");
    if (assetsPos != std::string::npos) {
        normalized = normalized.substr(assetsPos + 1);
    } else {
        assetsPos = normalized.find("assets/");
        if (assetsPos != std::string::npos) {
            normalized = normalized.substr(assetsPos);
        }
    }

    try {
        normalized = IVirtualFS::NormalizePath(normalized);
    } catch (...) {
        normalized = NormalizeSlashes(normalized);
    }
    return normalized;
}

std::string MakeAnimatorOverrideProjectRelative(const std::string& path)
{
    if (path.empty()) return {};

    std::string normalized = NormalizeAnimatorOverridePath(path);
    if (normalized.rfind("assets/", 0) == 0) {
        return normalized;
    }

    try {
        std::filesystem::path p(path);
        std::filesystem::path base = FileSystem::Instance().GetProjectRoot();
        if (!base.empty() && p.is_absolute()) {
            std::error_code ec;
            std::filesystem::path rel = std::filesystem::relative(p, base, ec);
            if (!ec) {
                std::string relStr = NormalizeSlashes(rel.string());
                if (relStr.find("../") == std::string::npos) {
                    return relStr;
                }
            }
        }
    } catch (...) {}

    return NormalizeSlashes(path);
}

std::string CleanupAnimatorOverrideLoadedPath(const std::string& path)
{
    return NormalizeAnimatorOverridePath(path);
}

void AnimatorControllerOverrideAsset::RebuildLookup()
{
    Lookup.clear();
    Lookup.reserve(Entries.size());
    for (const auto& entry : Entries) {
        const std::string source = NormalizeAnimatorOverridePath(entry.SourcePath);
        const std::string overridePath = NormalizeAnimatorOverridePath(entry.OverridePath);
        if (source.empty() || overridePath.empty()) continue;
        Lookup[source] = overridePath;
    }
}

bool AnimatorControllerOverrideAsset::MatchesController(const std::string& controllerPath) const
{
    if (BaseControllerPath.empty()) return false;
    return NormalizeAnimatorOverridePath(BaseControllerPath) == NormalizeAnimatorOverridePath(controllerPath);
}

std::string AnimatorControllerOverrideAsset::Resolve(const std::string& sourcePath) const
{
    const std::string normalized = NormalizeAnimatorOverridePath(sourcePath);
    if (normalized.empty()) return {};
    auto it = Lookup.find(normalized);
    if (it == Lookup.end()) return {};
    return it->second;
}

std::shared_ptr<AnimatorControllerOverrideAsset> LoadAnimatorControllerOverrideFromFile(const std::string& path)
{
    if (path.empty()) return nullptr;

    auto tryLoadCandidates = [](const std::vector<std::string>& candidates) -> std::shared_ptr<AnimatorControllerOverrideAsset> {
        for (const auto& candidate : candidates) {
            const std::string key = NormalizeCacheKey(candidate);
            if (auto cached = TryGetCached(key, candidate)) {
                return cached;
            }
            if (auto asset = TryLoadOverride(candidate)) {
                StoreCached(key, candidate, asset);
                return asset;
            }
        }
        return nullptr;
    };

#ifdef CLAYMORE_EDITOR
    std::filesystem::path projectPath;
    try {
        projectPath = Project::GetProjectDirectory();
    } catch (...) {}

    std::string fullPath = path;
    if (!projectPath.empty() && !std::filesystem::path(path).is_absolute()) {
        fullPath = (projectPath / path).string();
    }

    std::vector<std::string> candidates = BuildPathCandidates(fullPath);
    if (fullPath != path) {
        for (const auto& extra : BuildPathCandidates(path)) {
            AppendUnique(candidates, extra);
        }
    }

    if (auto asset = tryLoadCandidates(candidates)) {
        return asset;
    }
#endif

    if (auto asset = tryLoadCandidates(BuildPathCandidates(path))) {
        return asset;
    }

    return nullptr;
}

bool SaveAnimatorControllerOverride(const AnimatorControllerOverrideAsset& asset, const std::string& path)
{
    try {
        nlohmann::json j;
        nlohmann::to_json(j, asset);

        std::ofstream out(path, std::ios::trunc);
        if (!out.is_open()) {
            std::cerr << "[AnimatorControllerOverrideIO] Failed to open file for writing: " << path << "\n";
            return false;
        }

        out << j.dump(4);
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "[AnimatorControllerOverrideIO] Failed to save override '" << path
                  << "': " << ex.what() << "\n";
        return false;
    }
}

} // namespace animation
} // namespace cm
