#include "BuildDependencyGraph.h"

#include "AssetLibrary.h"
#include "core/assets/AssetReference.h"
#include "core/resources/ResourceManifest.h"
#include "editor/Project.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <regex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

using LocalAssetMap = std::unordered_map<std::string, std::string>;

const std::vector<std::string> kDependencyExts = {
    ".fbx", ".obj", ".gltf", ".glb",
    ".png", ".jpg", ".jpeg", ".tga", ".dds", ".ktx", ".ktx2",
    ".anim", ".animc", ".avatar", ".controller", ".animctrl", ".animoverride", ".ngraph",
    ".wav", ".mp3", ".ogg", ".flac",
    ".ttf", ".otf",
    ".cs", ".dll",
    ".mat", ".json", ".prefab", ".scene", ".clayobj",
    ".dlglib", ".dlg",
    ".terrainbin", ".navbin", ".navpack",
    ".meshbin", ".skelbin", ".animbin", ".actrlbin", ".modelrt", ".wrapbin",
    ".shader", ".hlsl", ".graph"
};

static std::string NormalizeSlashes(std::string value) {
    for (char& c : value) {
        if (c == '\\') c = '/';
    }
    return value;
}

static fs::path ResolveProjectPath(const std::string& rawPath) {
    fs::path path(rawPath);
    if (path.is_absolute()) {
        return path.lexically_normal();
    }

    const fs::path& projectDir = Project::GetProjectDirectory();
    if (!projectDir.empty()) {
        return (projectDir / path).lexically_normal();
    }

    return path.lexically_normal();
}

static bool TryStripGeneratedReversedSuffix(const std::string& stem, std::string& outBaseStem) {
    constexpr const char* kSuffix = "_Reversed";
    constexpr size_t kSuffixLen = 10;
    if (stem.size() <= kSuffixLen) {
        return false;
    }

    if (stem.compare(stem.size() - kSuffixLen, kSuffixLen, kSuffix) == 0) {
        outBaseStem = stem.substr(0, stem.size() - kSuffixLen);
        return !outBaseStem.empty();
    }

    if (stem.size() <= (kSuffixLen + 2)) {
        return false;
    }

    const size_t suffixPos = stem.rfind(kSuffix);
    if (suffixPos == std::string::npos || suffixPos == 0) {
        return false;
    }

    const size_t digitsPos = suffixPos + kSuffixLen;
    if (digitsPos >= stem.size() || stem[digitsPos] != '_') {
        return false;
    }

    if (digitsPos + 1 >= stem.size()) {
        return false;
    }

    for (size_t i = digitsPos + 1; i < stem.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(stem[i]))) {
            return false;
        }
    }

    outBaseStem = stem.substr(0, suffixPos);
    return !outBaseStem.empty();
}

static bool TryResolveGeneratedAnimationBase(const fs::path& requestedPath, fs::path& outBasePath) {
    std::string ext = NormalizeSlashes(requestedPath.extension().string());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (ext != ".anim") {
        return false;
    }

    std::string baseStem;
    if (!TryStripGeneratedReversedSuffix(requestedPath.stem().string(), baseStem)) {
        return false;
    }

    fs::path basePath = requestedPath.parent_path() / (baseStem + requestedPath.extension().string());
    if (!fs::exists(basePath)) {
        return false;
    }

    outBasePath = basePath.lexically_normal();
    return true;
}

static bool LooksLikeAssetPath(const std::string& value) {
    fs::path path(value);
    std::string ext = NormalizeSlashes(path.extension().string());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return std::find(kDependencyExts.begin(), kDependencyExts.end(), ext) != kDependencyExts.end();
}

static bool LooksLikeGuidString(const std::string& value) {
    auto isHexString = [](std::string_view text) -> bool {
        if (text.empty()) {
            return false;
        }

        for (char ch : text) {
            if (!std::isxdigit(static_cast<unsigned char>(ch))) {
                return false;
            }
        }
        return true;
    };

    if (value.size() == 32) {
        return isHexString(value);
    }

    if (value.size() == 36 &&
        value[8] == '-' &&
        value[13] == '-' &&
        value[18] == '-' &&
        value[23] == '-') {
        std::string compact;
        compact.reserve(32);
        for (char ch : value) {
            if (ch != '-') {
                compact.push_back(ch);
            }
        }
        return compact.size() == 32 && isHexString(compact);
    }

    return false;
}

static std::string NormalizeGuidKey(std::string value) {
    value.erase(std::remove(value.begin(), value.end(), '-'), value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

static bool IsRecursiveTextDependencyAsset(const fs::path& path) {
    std::string ext = NormalizeSlashes(path.extension().string());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return ext == ".scene" ||
           ext == ".prefab" ||
           ext == ".json" ||
           ext == ".mat" ||
           ext == ".clayobj" ||
           ext == ".cs" ||
           ext == ".animctrl" ||
           ext == ".animoverride" ||
           ext == ".dlglib";
}

static void AddIfExistingPath(const std::string& rawPath, std::vector<std::string>& outPaths) {
    if (rawPath.empty()) return;

    fs::path resolved = ResolveProjectPath(rawPath);
    if (fs::exists(resolved)) {
        outPaths.push_back(resolved.string());
        return;
    }

    fs::path generatedBasePath;
    if (TryResolveGeneratedAnimationBase(resolved, generatedBasePath)) {
        outPaths.push_back(resolved.string());
    }
}

static void TryResolveGuidAndAdd(const std::string& guidStr,
                                 std::vector<std::string>& outPaths,
                                 const LocalAssetMap* localAssetMap = nullptr) {
    if (guidStr.empty()) return;

    try {
        ClaymoreGUID guid = ClaymoreGUID::FromString(guidStr);
        if ((guid.high == 0 && guid.low == 0) || AssetReference::IsPrimitiveGuid(guid)) {
            return;
        }

        const std::string path = AssetLibrary::Instance().GetPathForGUID(guid);
        if (!path.empty()) {
            AddIfExistingPath(path, outPaths);
            return;
        }

        if (localAssetMap) {
            auto it = localAssetMap->find(NormalizeGuidKey(guidStr));
            if (it != localAssetMap->end()) {
                AddIfExistingPath(it->second, outPaths);
            }
        }
    } catch (...) {
    }
}

static void CollectDependencyString(const std::string& candidate,
                                    std::vector<std::string>& outPaths,
                                    const LocalAssetMap* localAssetMap = nullptr) {
    if (candidate.empty()) {
        return;
    }

    if (LooksLikeAssetPath(candidate)) {
        AddIfExistingPath(candidate, outPaths);
        return;
    }

    if (LooksLikeGuidString(candidate)) {
        TryResolveGuidAndAdd(candidate, outPaths, localAssetMap);
    }
}

static void CollectAssetMapEntries(const json& value, LocalAssetMap& outAssetMap) {
    if (value.is_array()) {
        for (const auto& item : value) {
            CollectAssetMapEntries(item, outAssetMap);
        }
        return;
    }

    if (!value.is_object()) {
        return;
    }

    auto assetMapIt = value.find("assetMap");
    if (assetMapIt != value.end() && assetMapIt->is_array()) {
        for (const auto& rec : *assetMapIt) {
            if (!rec.is_object()) {
                continue;
            }
            const auto guidIt = rec.find("guid");
            const auto pathIt = rec.find("path");
            if (guidIt != rec.end() && guidIt->is_string() &&
                pathIt != rec.end() && pathIt->is_string()) {
                const std::string guidKey = NormalizeGuidKey(guidIt->get<std::string>());
                if (!guidKey.empty()) {
                    outAssetMap[guidKey] = pathIt->get<std::string>();
                }
            }
        }
    }

    for (auto it = value.begin(); it != value.end(); ++it) {
        if (it.key() == "assetMap") {
            continue;
        }
        CollectAssetMapEntries(it.value(), outAssetMap);
    }
}

static void CollectPathsFromJson(const json& value,
                                 std::vector<std::string>& outPaths,
                                 const LocalAssetMap* localAssetMap = nullptr) {
    if (value.is_string()) {
        const std::string candidate = value.get<std::string>();
        CollectDependencyString(candidate, outPaths, localAssetMap);
        return;
    }

    if (value.is_array()) {
        for (const auto& item : value) {
            CollectPathsFromJson(item, outPaths, localAssetMap);
        }
        return;
    }

    if (!value.is_object()) {
        return;
    }

    if (value.contains("meshReference") && value["meshReference"].is_object()) {
        const json& ref = value["meshReference"];
        if (ref.contains("guid") && ref["guid"].is_string()) {
            TryResolveGuidAndAdd(ref["guid"].get<std::string>(), outPaths, localAssetMap);
        }
    }

    if (value.contains("materialReference") && value["materialReference"].is_object()) {
        const json& ref = value["materialReference"];
        if (ref.contains("guid") && ref["guid"].is_string()) {
            TryResolveGuidAndAdd(ref["guid"].get<std::string>(), outPaths, localAssetMap);
        }
    }

    if (value.contains("animationClip") && value["animationClip"].is_object()) {
        const json& ref = value["animationClip"];
        if (ref.contains("guid") && ref["guid"].is_string()) {
            TryResolveGuidAndAdd(ref["guid"].get<std::string>(), outPaths, localAssetMap);
        }
    }

    if (value.contains("prefabGuid") && value["prefabGuid"].is_string()) {
        TryResolveGuidAndAdd(value["prefabGuid"].get<std::string>(), outPaths, localAssetMap);
    }

    if (value.contains("meshGuid") && value["meshGuid"].is_string()) {
        TryResolveGuidAndAdd(value["meshGuid"].get<std::string>(), outPaths, localAssetMap);
    }

    if (value.contains("skeletonGuid") && value["skeletonGuid"].is_string()) {
        TryResolveGuidAndAdd(value["skeletonGuid"].get<std::string>(), outPaths, localAssetMap);
    }

    if (value.contains("guid") && value["guid"].is_string() &&
        (value.contains("fileID") || value.contains("type"))) {
        TryResolveGuidAndAdd(value["guid"].get<std::string>(), outPaths, localAssetMap);
    }

    if (value.contains("asset") && value["asset"].is_object()) {
        const json& asset = value["asset"];
        if (asset.contains("guid") && asset["guid"].is_string()) {
            TryResolveGuidAndAdd(asset["guid"].get<std::string>(), outPaths, localAssetMap);
        }
    }

    for (auto it = value.begin(); it != value.end(); ++it) {
        if (it.key() == "assetMap") {
            continue;
        }
        CollectPathsFromJson(it.value(), outPaths, localAssetMap);
    }
}

static std::string UnescapeSimpleStringLiteral(std::string value) {
    std::string out;
    out.reserve(value.size());

    for (size_t i = 0; i < value.size(); ++i) {
        char ch = value[i];
        if (ch == '\\' && i + 1 < value.size()) {
            char next = value[++i];
            switch (next) {
                case '\\': out.push_back('\\'); break;
                case '"': out.push_back('"'); break;
                case '/': out.push_back('/'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default:
                    out.push_back(next);
                    break;
            }
            continue;
        }

        out.push_back(ch);
    }

    return out;
}

static void CollectPathsFromText(const std::string& text, std::vector<std::string>& outPaths) {
    static const std::regex kStringLiteralRegex("\"((?:\\\\.|[^\"\\\\])*)\"");
    static const std::regex kGuidRegex(R"(\b[0-9A-Fa-f]{32}\b)");

    for (std::sregex_iterator it(text.begin(), text.end(), kStringLiteralRegex), end; it != end; ++it) {
        CollectDependencyString(UnescapeSimpleStringLiteral((*it)[1].str()), outPaths);
    }

    for (std::sregex_iterator it(text.begin(), text.end(), kGuidRegex), end; it != end; ++it) {
        TryResolveGuidAndAdd(it->str(), outPaths);
    }
}

} // namespace

void BuildDependencyGraph::AddEntryScene(const std::string& scenePath) {
    AddFileRecursive(scenePath, NodeKind::EntryScene, "entry scene", {});

    const std::string key = NormalizeKey(scenePath);
    auto it = m_Nodes.find(key);
    if (it != m_Nodes.end()) {
        it->second.root = true;
    }
}

void BuildDependencyGraph::AddProjectResources() {
    if (!ResourceManifest::Get().HasResourcesFolder()) {
        ResourceManifest::Get().TryDiscoverResourcesFolder(Project::GetProjectDirectory());
    }
    ResourceManifest::Get().Scan();

    const fs::path resourcesDir = Project::GetProjectDirectory() / "resources";
    if (fs::exists(resourcesDir)) {
        for (const auto& entry : fs::recursive_directory_iterator(resourcesDir)) {
            if (!entry.is_regular_file()) continue;
            AddFileRecursive(entry.path().string(), NodeKind::Resource, "resource file", {});
            auto it = m_Nodes.find(NormalizeKey(entry.path().string()));
            if (it != m_Nodes.end()) {
                it->second.root = true;
            }
        }
    }

    for (const auto* resource : ResourceManifest::Get().GetAllResources()) {
        if (!resource) continue;
        const std::string resourceKey = NormalizeKey(resource->path);
        AddFileRecursive(resource->path, NodeKind::Resource, "resource manifest entry", {});
        auto resourceIt = m_Nodes.find(resourceKey);
        if (resourceIt != m_Nodes.end()) {
            resourceIt->second.root = true;
        }

        for (const auto& depGuid : resource->dependencies) {
            const std::string depPath = AssetLibrary::Instance().GetPathForGUID(depGuid);
            if (depPath.empty()) continue;
            AddFileRecursive(depPath, NodeKind::ResourceDependency, "resource dependency", resourceKey);
        }
    }
}

void BuildDependencyGraph::AddExplicitAsset(const std::string& assetPath,
                                            NodeKind kind,
                                            const std::string& reason,
                                            const std::string& fromKey) {
    AddFileRecursive(assetPath, kind, reason, fromKey);
}

std::vector<std::string> BuildDependencyGraph::FlattenExistingPaths() const {
    std::vector<std::string> paths;
    paths.reserve(m_Order.size());

    for (const std::string& key : m_Order) {
        auto it = m_Nodes.find(key);
        if (it == m_Nodes.end() || (!it->second.exists && !it->second.buildable) || it->second.path.empty()) {
            continue;
        }
        paths.push_back(it->second.path);
    }

    return paths;
}

void BuildDependencyGraph::AddFileRecursive(const std::string& assetPath,
                                            NodeKind kind,
                                            const std::string& reason,
                                            const std::string& fromKey) {
    if (assetPath.empty()) return;

    const fs::path resolvedPath = ResolveProjectPath(assetPath);
    const std::string resolved = resolvedPath.string();
    const std::string key = NormalizeKey(resolved);
    const bool exists = fs::exists(resolvedPath);
    fs::path generatedBasePath;
    const bool buildable = exists || TryResolveGeneratedAnimationBase(resolvedPath, generatedBasePath);

    auto [it, inserted] = m_Nodes.emplace(key, Node{key, resolved, kind, false, exists, buildable});
    if (inserted) {
        m_Order.push_back(key);
    } else {
        if (static_cast<int>(kind) < static_cast<int>(it->second.kind)) {
            it->second.kind = kind;
        }
        it->second.exists = it->second.exists || exists;
        it->second.buildable = it->second.buildable || buildable;
    }

    if (!fromKey.empty()) {
        m_Edges.push_back({fromKey, key, reason});
    }

    if (!it->second.exists || !IsRecursiveTextDependencyAsset(resolvedPath)) {
        return;
    }

    static thread_local std::unordered_set<std::string> recursionGuard;
    if (!recursionGuard.insert(key).second) {
        return;
    }

    std::ifstream in(resolvedPath);
    if (!in.is_open()) {
        recursionGuard.erase(key);
        return;
    }

    std::vector<std::string> nestedPaths;
    std::string ext = NormalizeSlashes(resolvedPath.extension().string());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    const std::string fileText((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    bool collectedDependencies = false;

    try {
        json parsed = json::parse(fileText);
        LocalAssetMap localAssetMap;
        CollectAssetMapEntries(parsed, localAssetMap);
        CollectPathsFromJson(parsed, nestedPaths, &localAssetMap);
        collectedDependencies = true;
    } catch (...) {
    }

    if (!collectedDependencies) {
        CollectPathsFromText(fileText, nestedPaths);
    }

    for (const std::string& nested : nestedPaths) {
        AddFileRecursive(nested, NodeKind::Asset, ext == ".cs" ? "script dependency" : "json dependency", key);
    }

    recursionGuard.erase(key);
}

std::string BuildDependencyGraph::NormalizeKey(const std::string& path) const {
    try {
        fs::path resolved = ResolveProjectPath(path);
        std::error_code ec;
        if (fs::exists(resolved, ec)) {
            return NormalizeSlashes(fs::weakly_canonical(resolved, ec).string());
        }
        return NormalizeSlashes(resolved.lexically_normal().string());
    } catch (...) {
        return NormalizeSlashes(path);
    }
}
