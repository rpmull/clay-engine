#include "ui/utility/ProjectAssetIndex.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "editor/Project.h"

namespace ui {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

struct QueryCacheEntry {
    ProjectAssetQuery Query;
    fs::path Root;
    fs::file_time_type LastWrite{};
    Clock::time_point LastScan{};
    std::vector<ProjectAssetEntry> Entries;
    bool Initialized = false;
};

std::unordered_map<std::string, QueryCacheEntry> g_QueryCache;
std::mutex g_QueryMutex;

constexpr auto kMaxCacheAge = std::chrono::seconds(5);

std::string ToLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string NormalizePathKey(std::string value)
{
    std::replace(value.begin(), value.end(), '\\', '/');
    return ToLowerAscii(std::move(value));
}

bool EndsWith(const std::string& value, const std::string& suffix)
{
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

fs::path ResolveAssetRoot()
{
    fs::path root = Project::GetAssetDirectory();
    if (root.empty()) {
        root = fs::path("assets");
    }
    return root;
}

std::string SerializeQueryKey(const ProjectAssetQuery& query)
{
    std::vector<std::string> extensions = query.extensions;
    std::vector<std::string> suffixes = query.suffixes;
    std::sort(extensions.begin(), extensions.end());
    std::sort(suffixes.begin(), suffixes.end());

    std::string key = "ext:";
    for (const std::string& ext : extensions) {
        key += ToLowerAscii(ext);
        key.push_back(';');
    }
    key += "|suffix:";
    for (const std::string& suffix : suffixes) {
        key += ToLowerAscii(suffix);
        key.push_back(';');
    }
    return key;
}

bool MatchesQuery(const ProjectAssetQuery& query,
                  const std::string& extensionLower,
                  const std::string& normalizedRelativePath)
{
    if (query.extensions.empty() && query.suffixes.empty()) {
        return true;
    }

    bool extensionMatch = query.extensions.empty();
    for (const std::string& ext : query.extensions) {
        if (extensionLower == ext) {
            extensionMatch = true;
            break;
        }
    }

    bool suffixMatch = query.suffixes.empty();
    for (const std::string& suffix : query.suffixes) {
        if (EndsWith(normalizedRelativePath, suffix)) {
            suffixMatch = true;
            break;
        }
    }

    if (!query.extensions.empty() && !query.suffixes.empty()) {
        return extensionMatch || suffixMatch;
    }
    return extensionMatch && suffixMatch;
}

void NormalizeQuery(ProjectAssetQuery& query)
{
    for (std::string& ext : query.extensions) {
        ext = ToLowerAscii(std::move(ext));
        if (!ext.empty() && ext[0] != '.') {
            ext.insert(ext.begin(), '.');
        }
    }
    for (std::string& suffix : query.suffixes) {
        suffix = NormalizePathKey(std::move(suffix));
    }
}

void RebuildCache(QueryCacheEntry& cache)
{
    cache.Entries.clear();
    cache.LastWrite = {};

    const fs::path root = cache.Root;
    std::error_code ec;
    if (root.empty() || !fs::exists(root, ec)) {
        cache.LastScan = Clock::now();
        cache.Initialized = true;
        return;
    }

    const fs::path projectRoot = Project::GetProjectDirectory();
    std::unordered_set<std::string> seenPaths;

    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    while (!ec && it != end) {
        const fs::directory_entry& entry = *it;
        if (entry.is_regular_file(ec)) {
            const fs::path fullPath = entry.path().lexically_normal();
            const std::string absolutePath = fullPath.string();
            const std::string normalizedAbsolutePath = NormalizePathKey(absolutePath);

            std::string relativePath;
            if (!projectRoot.empty()) {
                std::error_code relEc;
                fs::path relative = fs::relative(fullPath, projectRoot, relEc);
                if (!relEc && !relative.empty()) {
                    relativePath = relative.generic_string();
                }
            }
            if (relativePath.empty()) {
                std::error_code relEc;
                fs::path relative = fs::relative(fullPath, root.parent_path(), relEc);
                relativePath = (!relEc && !relative.empty()) ? relative.generic_string() : fullPath.filename().string();
            }

            ProjectAssetEntry asset;
            asset.absolutePath = absolutePath;
            asset.projectRelativePath = relativePath;
            asset.normalizedAbsolutePath = std::move(normalizedAbsolutePath);
            asset.normalizedProjectRelativePath = NormalizePathKey(asset.projectRelativePath);
            asset.extensionLower = ToLowerAscii(fullPath.extension().string());
            asset.name = fullPath.stem().string();
            if (asset.name.empty()) {
                asset.name = fullPath.filename().string();
            }

            if (MatchesQuery(cache.Query, asset.extensionLower, asset.normalizedProjectRelativePath) &&
                seenPaths.insert(asset.normalizedAbsolutePath).second) {
                cache.Entries.push_back(std::move(asset));
            }
        }
        ++it;
    }

    std::sort(cache.Entries.begin(), cache.Entries.end(), [](const ProjectAssetEntry& a, const ProjectAssetEntry& b) {
        if (a.name == b.name) {
            return a.projectRelativePath < b.projectRelativePath;
        }
        return a.name < b.name;
    });

    cache.LastWrite = fs::last_write_time(root, ec);
    cache.LastScan = Clock::now();
    cache.Initialized = true;
}

} // namespace

ProjectAssetQuery MakeExtensionQuery(std::initializer_list<const char*> extensions)
{
    ProjectAssetQuery query;
    query.extensions.reserve(extensions.size());
    for (const char* ext : extensions) {
        if (ext && *ext) {
            query.extensions.emplace_back(ext);
        }
    }
    NormalizeQuery(query);
    return query;
}

ProjectAssetQuery MakeSuffixQuery(std::initializer_list<const char*> suffixes)
{
    ProjectAssetQuery query;
    query.suffixes.reserve(suffixes.size());
    for (const char* suffix : suffixes) {
        if (suffix && *suffix) {
            query.suffixes.emplace_back(suffix);
        }
    }
    NormalizeQuery(query);
    return query;
}

const std::vector<ProjectAssetEntry>& GetProjectAssetEntries(const ProjectAssetQuery& query)
{
    std::lock_guard<std::mutex> lock(g_QueryMutex);

    ProjectAssetQuery normalizedQuery = query;
    NormalizeQuery(normalizedQuery);

    const std::string key = SerializeQueryKey(normalizedQuery);
    QueryCacheEntry& cache = g_QueryCache[key];

    const fs::path root = ResolveAssetRoot();
    bool needsRebuild = cache.Root != root || !cache.Initialized;
    cache.Root = root;
    cache.Query = normalizedQuery;

    if (!needsRebuild && !root.empty()) {
        std::error_code ec;
        const fs::file_time_type lastWrite = fs::last_write_time(root, ec);
        if (ec || lastWrite != cache.LastWrite) {
            needsRebuild = true;
        }
    }

    if (!needsRebuild && (Clock::now() - cache.LastScan) > kMaxCacheAge) {
        needsRebuild = true;
    }

    if (needsRebuild) {
        RebuildCache(cache);
    }

    return cache.Entries;
}

void InvalidateProjectAssetIndex()
{
    std::lock_guard<std::mutex> lock(g_QueryMutex);
    g_QueryCache.clear();
}

} // namespace ui
