#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

namespace cm {
namespace animation {

std::string NormalizeAnimatorOverridePath(const std::string& path);
std::string MakeAnimatorOverrideProjectRelative(const std::string& path);
std::string CleanupAnimatorOverrideLoadedPath(const std::string& path);

struct AnimatorControllerOverrideEntry {
    std::string SourcePath;
    std::string OverridePath;
};

struct AnimatorControllerOverrideAsset {
    std::string BaseControllerPath;
    std::vector<AnimatorControllerOverrideEntry> Entries;
    std::unordered_map<std::string, std::string> Lookup;

    void RebuildLookup();
    bool MatchesController(const std::string& controllerPath) const;
    std::string Resolve(const std::string& sourcePath) const;
};

inline void to_json(nlohmann::json& j, const AnimatorControllerOverrideEntry& entry) {
    j = nlohmann::json{
        {"source", MakeAnimatorOverrideProjectRelative(entry.SourcePath)},
        {"override", MakeAnimatorOverrideProjectRelative(entry.OverridePath)}
    };
}

inline void from_json(const nlohmann::json& j, AnimatorControllerOverrideEntry& entry) {
    entry.SourcePath = CleanupAnimatorOverrideLoadedPath(j.value("source", ""));
    entry.OverridePath = CleanupAnimatorOverrideLoadedPath(j.value("override", ""));
}

inline void to_json(nlohmann::json& j, const AnimatorControllerOverrideAsset& asset) {
    j = nlohmann::json{
        {"controller", MakeAnimatorOverrideProjectRelative(asset.BaseControllerPath)},
        {"entries", asset.Entries}
    };
}

inline void from_json(const nlohmann::json& j, AnimatorControllerOverrideAsset& asset) {
    asset.BaseControllerPath = CleanupAnimatorOverrideLoadedPath(j.value("controller", ""));
    if (j.contains("entries")) {
        asset.Entries = j["entries"].get<std::vector<AnimatorControllerOverrideEntry>>();
    } else {
        asset.Entries.clear();
    }
    asset.RebuildLookup();
}

} // namespace animation
} // namespace cm
