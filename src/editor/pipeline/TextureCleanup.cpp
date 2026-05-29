#include "TextureCleanup.h"

#include "editor/Project.h"
#include "editor/pipeline/AssetLibrary.h"
#include "editor/pipeline/AssetPipeline.h"
#include "editor/pipeline/AssetRegistry.h"
#include "editor/pipeline/ModelImportCache.h"
#include "editor/pipeline/ModelImportSettings.h"
#include "editor/pipeline/MaterialSourceSerialization.h"
#include "editor/import/ModelLoader.h"
#include "editor/import/ModelPreprocessor.h"

#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <algorithm>
#include <sstream>
#include <cctype>
#include <cstring>
#include <limits>
#include <optional>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace texture_cleanup {
namespace {

struct SourceTextureFilenameHint {
    std::string albedo;
    std::string metallicRoughness;
    std::string normal;
    std::string ao;
    std::string emission;
    std::string displacement;
};

struct ModelTextureInfo {
    std::string modelPath;
    std::string metaPath;
    std::string modelName;
    fs::path textureDir;
    json metaJson;
    std::vector<std::string> texturePaths; // normalized virtual paths
    std::unordered_map<std::string, std::vector<SourceTextureFilenameHint>> sourceFilenameHints;
};

struct TextureFileInfo {
    std::string absPath;
    std::string virtualPath;
    std::unordered_set<std::string> models;
    std::string hash;
};

struct TextureReferenceHint {
    std::string modelName;
    std::string modelReferenceKey;
    std::string materialName;
    std::string usage;
    std::string preferredFilename;
};

struct FileRepairStats {
    int filesScanned = 0;
    int filesUpdated = 0;
    int texturePathRemaps = 0;
    int materialOverridesReset = 0;
};

struct ProjectRepairStats {
    FileRepairStats scenes;
    FileRepairStats prefabs;
};

static std::string NormalizeForCompare(const fs::path& p);
static bool IsSafeExtension(std::string ext);
static bool IsModelAssetExtension(const std::string& ext);
static bool IsTextureExtension(const fs::path& p);
static bool IsUnderDirectory(const fs::path& path, const fs::path& dir);
static std::string NormalizeAssetPath(const std::string& path, const fs::path& projectDir);
static bool FilenameLooksModelScoped(const std::string& filename,
                                     const std::vector<TextureReferenceHint>& hints);
static std::string NormalizePreferredFilenameExtension(const std::string& preferredFilename,
                                                       const std::string& fallbackExtension);
static std::string FindCanonicalFilenameByHash(
    const std::string& hash,
    const std::vector<TextureReferenceHint>& hints,
    const std::unordered_map<std::string, std::vector<fs::path>>* texturesByHash,
    const fs::path& sharedDir,
    const fs::path& projectDir,
    const std::string& fallbackExtension);
static std::vector<fs::path> FindSceneFiles(const fs::path& projectDir);
static std::vector<fs::path> FindPrefabFiles(const fs::path& projectDir);
static std::vector<fs::path> FindMaterialAssetFiles(const fs::path& assetsDir);

static void AddPathMapping(std::unordered_map<std::string, std::string>& mapping,
                           const std::string& oldVirtual,
                           const std::string& newVirtual) {
    if (oldVirtual.empty() || newVirtual.empty()) return;
    if (oldVirtual == newVirtual) return;
    mapping[oldVirtual] = newVirtual;
    std::string filename = fs::path(oldVirtual).filename().string();
    if (!filename.empty()) {
        std::string alias = std::string("assets/") + filename;
        if (alias != oldVirtual) {
            mapping[alias] = newVirtual;
        }
    }
}

static fs::file_time_type SafeLastWriteTime(const fs::path& p) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    if (ec) {
        return fs::file_time_type::min();
    }
    return t;
}

static std::string ToLower(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return v;
}

static bool StartsWithIgnoreCase(const std::string& value, const std::string& prefix) {
    if (prefix.size() > value.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(value[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

static std::string NormalizeSlashes(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

static bool IsDigitsOnly(const std::string& value) {
    if (value.empty()) return false;
    for (char c : value) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

static bool IsGenericTextureFilename(const std::string& filename) {
    std::string stem = ToLower(fs::path(filename).stem().string());
    if (stem.empty()) return false;
    if (stem.rfind("shared_", 0) == 0) return true;
    if (stem.rfind("emb_auto_", 0) == 0) return true;
    if (stem.rfind("emb_", 0) == 0) {
        std::string suffix = stem.substr(4);
        size_t split = suffix.find('_');
        if (split != std::string::npos) {
            std::string left = suffix.substr(0, split);
            std::string right = suffix.substr(split + 1);
            if (IsDigitsOnly(left) && IsDigitsOnly(right)) {
                return true;
            }
        }
    }
    return false;
}

static bool HasNumericDuplicateTextureSuffix(const std::string& filename) {
    std::string stem = fs::path(filename).stem().string();
    if (stem.empty()) return false;

    size_t dot = stem.find_last_of('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= stem.size()) {
        return false;
    }

    std::string suffix = stem.substr(dot + 1);
    return suffix.size() >= 3 && IsDigitsOnly(suffix);
}

static bool IsWeakTextureFilename(const std::string& filename) {
    return IsGenericTextureFilename(filename) || HasNumericDuplicateTextureSuffix(filename);
}

static std::string SanitizeFilenameToken(const std::string& value) {
    std::string sanitized;
    sanitized.reserve(value.size());
    bool lastUnderscore = false;
    for (char c : value) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            sanitized.push_back(c);
            lastUnderscore = false;
        } else if (!lastUnderscore) {
            sanitized.push_back('_');
            lastUnderscore = true;
        }
    }
    while (!sanitized.empty() && sanitized.front() == '_') sanitized.erase(sanitized.begin());
    while (!sanitized.empty() && sanitized.back() == '_') sanitized.pop_back();
    return sanitized;
}

static std::string TrimNumericSuffixToken(const std::string& value) {
    if (value.empty()) return value;
    size_t dot = value.find_last_of('.');
    if (dot != std::string::npos && dot + 1 < value.size()) {
        bool digitsOnly = true;
        for (size_t i = dot + 1; i < value.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(value[i]))) {
                digitsOnly = false;
                break;
            }
        }
        if (digitsOnly) {
            return value.substr(0, dot);
        }
    }
    return value;
}

static std::string StripModelPrefixToken(const std::string& value, const std::string& modelName) {
    std::string sanitizedValue = SanitizeFilenameToken(value);
    std::string sanitizedModel = SanitizeFilenameToken(modelName);
    if (sanitizedValue.empty() || sanitizedModel.empty()) {
        return sanitizedValue;
    }

    std::string lowerValue = ToLower(sanitizedValue);
    std::string lowerModel = ToLower(sanitizedModel);
    if (StartsWithIgnoreCase(sanitizedValue, sanitizedModel)) {
        size_t offset = sanitizedModel.size();
        if (offset < sanitizedValue.size() && sanitizedValue[offset] == '_') {
            return sanitizedValue.substr(offset + 1);
        }
        if (offset == sanitizedValue.size()) {
            return {};
        }
    }

    if (lowerValue.rfind(lowerModel + "_", 0) == 0) {
        return sanitizedValue.substr(sanitizedModel.size() + 1);
    }

    return sanitizedValue;
}

static bool IsWeakFilenameHintToken(const std::string& token) {
    std::string lower = ToLower(SanitizeFilenameToken(token));
    if (lower.empty()) return true;
    if (lower == "material" || lower == "defaultmaterial" || lower == "default") return true;
    if (lower.rfind("slot", 0) == 0) return true;
    return false;
}

static bool IsModelAssetExtension(const std::string& ext) {
    std::string lower = ToLower(ext);
    return lower == ".meta" ||
           lower == ".fbx" ||
           lower == ".obj" ||
           lower == ".gltf" ||
           lower == ".glb";
}

static std::string CanonicalizeHintToken(const std::string& raw,
                                         const std::string& modelName) {
    std::string trimmed = TrimNumericSuffixToken(raw);
    std::string stripped = StripModelPrefixToken(trimmed, modelName);
    if (!stripped.empty()) {
        return stripped;
    }
    return SanitizeFilenameToken(trimmed);
}

static std::string BuildReadableFilenameFromHints(const std::vector<TextureReferenceHint>& hints,
                                                  const std::string& ext) {
    for (const auto& hint : hints) {
        std::vector<std::string> tokens;
        std::unordered_set<std::string> seen;
        auto addToken = [&](const std::string& raw, bool canonicalize = false) {
            std::string clean = canonicalize ? CanonicalizeHintToken(raw, hint.modelName)
                                             : SanitizeFilenameToken(raw);
            if (clean.empty()) return;
            std::string lower = ToLower(clean);
            if (!seen.insert(lower).second) return;
            tokens.push_back(clean);
        };

        std::string materialToken = CanonicalizeHintToken(hint.materialName, hint.modelName);
        if (!materialToken.empty()) {
            addToken(materialToken);
        }
        if (materialToken.empty() || IsWeakFilenameHintToken(materialToken)) {
            addToken(hint.modelName, true);
        }
        addToken(hint.usage);

        if (tokens.empty()) continue;

        std::string stem;
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (i > 0) stem += "_";
            stem += tokens[i];
        }
        if (!stem.empty()) {
            return stem + ext;
        }
    }
    return {};
}

static std::vector<std::string> SplitLowerUnderscoreTokens(const std::string& value) {
    std::vector<std::string> tokens;
    std::string current;
    for (char c : ToLower(value)) {
        if (c == '_') {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

static bool IsSlotToken(const std::string& token) {
    if (token == "slot") return true;
    if (token.rfind("slot", 0) != 0) return false;
    return IsDigitsOnly(token.substr(4));
}

static bool IsTextureUsageToken(const std::string& token) {
    return token == "albedo" ||
           token == "normal" ||
           token == "ao" ||
           token == "emission" ||
           token == "displacement" ||
           token == "metallicroughness";
}

static bool FilenameLooksHintDerived(const std::string& filename,
                                     const std::vector<TextureReferenceHint>& hints) {
    std::string stem = ToLower(fs::path(filename).stem().string());
    if (stem.empty()) return false;

    std::vector<std::string> tokens = SplitLowerUnderscoreTokens(stem);
    bool hasSlotToken = false;
    bool hasUsageToken = false;
    for (const auto& token : tokens) {
        hasSlotToken |= IsSlotToken(token);
        hasUsageToken |= IsTextureUsageToken(token);
    }
    if (hasSlotToken) {
        return true;
    }

    for (const auto& hint : hints) {
        std::string modelToken = ToLower(SanitizeFilenameToken(hint.modelName));
        std::string materialToken = ToLower(CanonicalizeHintToken(hint.materialName, hint.modelName));
        std::string usageToken = ToLower(SanitizeFilenameToken(hint.usage));

        if (!modelToken.empty() && stem.rfind(modelToken + "_", 0) == 0) {
            return true;
        }
        if (!materialToken.empty() && stem.rfind(materialToken + "_", 0) == 0 && hasUsageToken) {
            return true;
        }
        if (!materialToken.empty() && !usageToken.empty() && stem == materialToken + "_" + usageToken) {
            return true;
        }
        if (!modelToken.empty() && !usageToken.empty() && stem == modelToken + "_" + usageToken) {
            return true;
        }
    }

    return false;
}

static std::string FindPreferredSourceFilename(const std::vector<TextureReferenceHint>& hints) {
    struct PreferredFilenameChoice {
        std::string filename;
        int count = 0;
        size_t firstIndex = std::numeric_limits<size_t>::max();
    };

    std::unordered_map<std::string, PreferredFilenameChoice> choices;
    for (size_t i = 0; i < hints.size(); ++i) {
        std::string preferredFilename = fs::path(hints[i].preferredFilename).filename().string();
        if (preferredFilename.empty() || IsWeakTextureFilename(preferredFilename)) {
            continue;
        }

        std::string key = ToLower(preferredFilename);
        auto& choice = choices[key];
        if (choice.count == 0) {
            choice.filename = preferredFilename;
            choice.firstIndex = i;
        }
        choice.count += 1;
    }

    const PreferredFilenameChoice* best = nullptr;
    for (const auto& kv : choices) {
        const PreferredFilenameChoice& candidate = kv.second;
        if (!best ||
            candidate.count > best->count ||
            (candidate.count == best->count && candidate.firstIndex < best->firstIndex)) {
            best = &candidate;
        }
    }

    return best ? best->filename : std::string{};
}

static std::string ChoosePreferredSharedFilename(const std::vector<fs::path>& candidatePaths,
                                                 const std::vector<TextureReferenceHint>& hints,
                                                 const std::string& hash,
                                                 const std::unordered_map<std::string, std::vector<fs::path>>* texturesByHash = nullptr,
                                                 const fs::path& sharedDir = {},
                                                 const fs::path& projectDir = {}) {
    std::string ext;
    for (const auto& candidate : candidatePaths) {
        std::string candidateExt = ToLower(candidate.extension().string());
        if (IsSafeExtension(candidateExt)) {
            ext = candidateExt;
            break;
        }
    }
    if (!IsSafeExtension(ext)) {
        ext = ".png";
    }

    std::string preferredSourceFilename = FindPreferredSourceFilename(hints);
    if (!preferredSourceFilename.empty()) {
        return NormalizePreferredFilenameExtension(preferredSourceFilename, ext);
    }

    std::string canonicalHashFilename =
        FindCanonicalFilenameByHash(hash, hints, texturesByHash, sharedDir, projectDir, ext);
    if (!canonicalHashFilename.empty()) {
        return canonicalHashFilename;
    }

    bool hasGeneratedCandidate = false;
    bool hasWeakDuplicateCandidate = false;
    for (const auto& candidate : candidatePaths) {
        std::string filename = candidate.filename().string();
        if (filename.empty()) {
            continue;
        }

        hasGeneratedCandidate |= IsGenericTextureFilename(filename) ||
                                 FilenameLooksHintDerived(filename, hints) ||
                                 FilenameLooksModelScoped(filename, hints);
        hasWeakDuplicateCandidate |= HasNumericDuplicateTextureSuffix(filename);

        if (!filename.empty() &&
            !IsWeakTextureFilename(filename) &&
            !FilenameLooksHintDerived(filename, hints)) {
            return filename;
        }
    }

    if (hasGeneratedCandidate) {
        std::string hinted = BuildReadableFilenameFromHints(hints, ext);
        if (!hinted.empty()) {
            return hinted;
        }
    }

    if (hasWeakDuplicateCandidate) {
        for (const auto& candidate : candidatePaths) {
            std::string filename = candidate.filename().string();
            if (!filename.empty()) {
                return filename;
            }
        }
    }

    for (const auto& candidate : candidatePaths) {
        std::string filename = candidate.filename().string();
        if (!filename.empty()) {
            return filename;
        }
    }

    std::string suffix = hash.empty() ? std::string("shared") : std::string("shared_") + hash.substr(0, std::min<size_t>(hash.size(), 8));
    return suffix + ext;
}

static fs::path ResolveDesiredSharedPath(const fs::path& sharedDir,
                                         const std::string& preferredFilename,
                                         const std::string& hash,
                                         const fs::path& currentPath = {}) {
    fs::path desired = sharedDir / preferredFilename;
    if (!currentPath.empty() && NormalizeForCompare(currentPath) == NormalizeForCompare(desired)) {
        return desired;
    }
    if (!fs::exists(desired)) {
        return desired;
    }
    if (!IsWeakTextureFilename(preferredFilename)) {
        return desired;
    }

    if (!hash.empty()) {
        try {
            std::string existingHash = AssetPipeline::Instance().ComputeFileHash(desired.string());
            if (existingHash == hash) {
                return desired;
            }
        } catch (...) {}
    }

    std::string stem = desired.stem().string();
    std::string ext = desired.extension().string();
    std::string suffix = hash.empty() ? std::string("_shared")
                                      : std::string("_") + hash.substr(0, std::min<size_t>(hash.size(), 8));

    fs::path candidate = sharedDir / (stem + suffix + ext);
    int counter = 1;
    while (fs::exists(candidate)) {
        if (!hash.empty()) {
            try {
                std::string existingHash = AssetPipeline::Instance().ComputeFileHash(candidate.string());
                if (existingHash == hash) {
                    return candidate;
                }
            } catch (...) {}
        }
        candidate = sharedDir / (stem + suffix + "_" + std::to_string(counter++) + ext);
    }
    return candidate;
}

static std::string ResolveFinalMappedPath(const std::string& path,
                                          const std::unordered_map<std::string, std::string>& mapping) {
    std::string resolved = path;
    std::unordered_set<std::string> visited;
    while (!resolved.empty()) {
        auto it = mapping.find(resolved);
        if (it == mapping.end()) break;
        if (!visited.insert(resolved).second) break;
        resolved = it->second;
    }
    return resolved;
}

static std::unordered_map<std::string, std::string> CollapsePathMapping(
    const std::unordered_map<std::string, std::string>& mapping) {
    std::unordered_map<std::string, std::string> collapsed;
    collapsed.reserve(mapping.size());
    for (const auto& kv : mapping) {
        std::string resolved = ResolveFinalMappedPath(kv.second, mapping);
        if (resolved.empty() || resolved == kv.first) continue;
        collapsed[kv.first] = resolved;
    }
    return collapsed;
}

static void AppendTextureFilenameIndex(const fs::path& rootDir,
                                       std::unordered_map<std::string, std::vector<fs::path>>& index) {
    if (rootDir.empty() || !fs::exists(rootDir)) {
        return;
    }

    for (const auto& entry : fs::recursive_directory_iterator(rootDir)) {
        if (!entry.is_regular_file()) continue;
        if (!IsTextureExtension(entry.path())) continue;
        index[ToLower(entry.path().filename().string())].push_back(entry.path());
    }
}

static std::unordered_map<std::string, std::vector<fs::path>> BuildTextureFilenameIndex(const fs::path& texturesDir,
                                                                                        const fs::path& projectDir = {}) {
    std::unordered_map<std::string, std::vector<fs::path>> index;
    AppendTextureFilenameIndex(texturesDir, index);
    if (!projectDir.empty()) {
        AppendTextureFilenameIndex(projectDir / "resources" / "textures", index);
    }
    return index;
}

static void AppendUniqueTextureRoot(std::vector<fs::path>& roots,
                                    std::unordered_set<std::string>& seenRoots,
                                    const fs::path& dir) {
    if (dir.empty() || !fs::exists(dir)) {
        return;
    }

    std::string key = NormalizeForCompare(dir);
    if (key.empty() || !seenRoots.insert(key).second) {
        return;
    }

    roots.push_back(dir);
}

static std::vector<fs::path> CollectCanonicalTextureRoots(const fs::path& texturesDir,
                                                          const fs::path& projectDir) {
    std::vector<fs::path> roots;
    std::unordered_set<std::string> seenRoots;

    AppendUniqueTextureRoot(roots, seenRoots, texturesDir);
    if (!projectDir.empty()) {
        AppendUniqueTextureRoot(roots, seenRoots, projectDir / "resources" / "textures");
        AppendUniqueTextureRoot(roots, seenRoots, projectDir / "art" / "textures");
        AppendUniqueTextureRoot(roots, seenRoots, projectDir.parent_path() / "art" / "textures");
    }

    return roots;
}

static std::unordered_map<std::string, std::vector<fs::path>> BuildTextureHashIndex(const std::vector<fs::path>& rootDirs) {
    std::unordered_map<std::string, std::vector<fs::path>> index;

    for (const fs::path& rootDir : rootDirs) {
        if (rootDir.empty() || !fs::exists(rootDir)) {
            continue;
        }

        for (const auto& entry : fs::recursive_directory_iterator(rootDir)) {
            if (!entry.is_regular_file()) continue;
            if (!IsTextureExtension(entry.path())) continue;

            std::string hash;
            try {
                hash = AssetPipeline::Instance().ComputeFileHash(entry.path().string());
            } catch (...) {
                hash.clear();
            }

            if (!hash.empty()) {
                index[hash].push_back(entry.path());
            }
        }
    }

    return index;
}

static std::string NormalizePreferredFilenameExtension(const std::string& preferredFilename,
                                                       const std::string& fallbackExtension) {
    fs::path preferredPath(preferredFilename);
    std::string preferredExt = ToLower(preferredPath.extension().string());
    if (!IsSafeExtension(preferredExt) || preferredExt != fallbackExtension) {
        preferredPath.replace_extension(fallbackExtension);
    }
    return preferredPath.filename().string();
}

static int ScoreCanonicalHashCandidate(const fs::path& candidate,
                                       const std::vector<TextureReferenceHint>& hints,
                                       const fs::path& sharedDir,
                                       const fs::path& projectDir) {
    const std::string filename = candidate.filename().string();
    if (filename.empty()) {
        return std::numeric_limits<int>::min();
    }

    int score = 0;
    if (IsWeakTextureFilename(filename)) score -= 250;
    if (FilenameLooksHintDerived(filename, hints)) score -= 140;
    if (FilenameLooksModelScoped(filename, hints)) score -= 100;

    if (!IsWeakTextureFilename(filename) &&
        !FilenameLooksHintDerived(filename, hints) &&
        !FilenameLooksModelScoped(filename, hints)) {
        score += 120;
    }

    if (!IsUnderDirectory(candidate, sharedDir)) {
        score += 40;
    }

    if (!projectDir.empty()) {
        if (IsUnderDirectory(candidate, projectDir / "resources" / "textures")) {
            score += 80;
        }
        if (IsUnderDirectory(candidate, projectDir / "art" / "textures")) {
            score += 90;
        }
        if (IsUnderDirectory(candidate, projectDir.parent_path() / "art" / "textures")) {
            score += 110;
        }
    }

    return score;
}

static std::string FindCanonicalFilenameByHash(
    const std::string& hash,
    const std::vector<TextureReferenceHint>& hints,
    const std::unordered_map<std::string, std::vector<fs::path>>* texturesByHash,
    const fs::path& sharedDir,
    const fs::path& projectDir,
    const std::string& fallbackExtension) {
    if (!texturesByHash || hash.empty()) {
        return {};
    }

    auto it = texturesByHash->find(hash);
    if (it == texturesByHash->end() || it->second.empty()) {
        return {};
    }

    const fs::path* best = nullptr;
    int bestScore = std::numeric_limits<int>::min();
    for (const fs::path& candidate : it->second) {
        int score = ScoreCanonicalHashCandidate(candidate, hints, sharedDir, projectDir);
        if (score > bestScore) {
            bestScore = score;
            best = &candidate;
        }
    }

    if (!best) {
        return {};
    }

    const std::string filename = best->filename().string();
    if (filename.empty() ||
        IsWeakTextureFilename(filename) ||
        FilenameLooksHintDerived(filename, hints) ||
        FilenameLooksModelScoped(filename, hints)) {
        return {};
    }

    return NormalizePreferredFilenameExtension(filename, fallbackExtension);
}

static int ScoreTextureCandidateForHints(const fs::path& candidate,
                                         const std::vector<TextureReferenceHint>& hints,
                                         const fs::path& sharedDir) {
    int score = 0;
    if (IsUnderDirectory(candidate, sharedDir)) score += 1000;

    std::string candidateLower = ToLower(NormalizeSlashes(candidate.generic_string()));
    for (const auto& hint : hints) {
        std::string modelToken = ToLower(SanitizeFilenameToken(hint.modelName));
        std::string materialToken = ToLower(CanonicalizeHintToken(hint.materialName, hint.modelName));
        if (!modelToken.empty() && candidateLower.find(modelToken) != std::string::npos) score += 25;
        if (!materialToken.empty() && candidateLower.find(materialToken) != std::string::npos) score += 20;
    }
    return score;
}

static fs::path ChooseBestTextureCandidate(const std::vector<fs::path>& candidates,
                                           const std::vector<TextureReferenceHint>& hints,
                                           const fs::path& sharedDir) {
    fs::path best;
    int bestScore = std::numeric_limits<int>::min();
    for (const auto& candidate : candidates) {
        int score = ScoreTextureCandidateForHints(candidate, hints, sharedDir);
        if (score > bestScore) {
            bestScore = score;
            best = candidate;
        }
    }
    return best;
}

static bool EndsWithIgnoreCase(const std::string& value, const std::string& suffix) {
    if (suffix.size() > value.size()) return false;
    return StartsWithIgnoreCase(value.substr(value.size() - suffix.size()), suffix);
}

static std::string StripKnownTextureUsageSuffix(const std::string& value) {
    static const std::array<const char*, 6> kUsageSuffixes = {{
        "_albedo",
        "_normal",
        "_ao",
        "_emission",
        "_displacement",
        "_metallicroughness"
    }};

    for (const char* suffix : kUsageSuffixes) {
        if (EndsWithIgnoreCase(value, suffix)) {
            return value.substr(0, value.size() - std::strlen(suffix));
        }
    }

    return value;
}

static void AddFilenameCandidate(std::vector<std::string>& outCandidates,
                                 std::unordered_set<std::string>& seenCandidates,
                                 const std::string& filename) {
    std::string leaf = fs::path(filename).filename().string();
    if (leaf.empty()) {
        return;
    }

    std::string key = ToLower(leaf);
    if (!seenCandidates.insert(key).second) {
        return;
    }

    outCandidates.push_back(leaf);
}

static std::vector<std::string> BuildTextureFilenameSearchCandidates(const std::string& preferredFilename,
                                                                     const std::string& materialName,
                                                                     const std::string& usage,
                                                                     const std::string& fallbackPath,
                                                                     bool allowMaterialAliases) {
    std::vector<std::string> candidates;
    std::unordered_set<std::string> seenCandidates;

    std::string ext = ToLower(fs::path(preferredFilename).extension().string());
    if (!IsSafeExtension(ext)) {
        ext = ToLower(fs::path(fallbackPath).extension().string());
    }
    if (!IsSafeExtension(ext)) {
        ext = ".png";
    }

    std::string preferredLeaf = fs::path(preferredFilename).filename().string();
    if (!preferredLeaf.empty() && !(allowMaterialAliases && IsWeakTextureFilename(preferredLeaf))) {
        AddFilenameCandidate(candidates, seenCandidates, preferredLeaf);

        std::string preferredStem = fs::path(preferredLeaf).stem().string();
        std::string strippedStem = StripKnownTextureUsageSuffix(preferredStem);
        if (!strippedStem.empty() && ToLower(strippedStem) != ToLower(preferredStem)) {
            AddFilenameCandidate(candidates, seenCandidates, strippedStem + ext);
        }
    }

    if (!allowMaterialAliases) {
        return candidates;
    }

    std::string materialToken = CanonicalizeHintToken(materialName, std::string{});
    if (materialToken.empty() || IsWeakFilenameHintToken(materialToken)) {
        return candidates;
    }

    std::string lowerUsage = ToLower(SanitizeFilenameToken(usage));
    std::string singularToken = materialToken;
    if (singularToken.size() > 1 && (singularToken.back() == 's' || singularToken.back() == 'S')) {
        singularToken.pop_back();
    }

    if (lowerUsage == "albedo") {
        AddFilenameCandidate(candidates, seenCandidates, materialToken + ext);
        if (!singularToken.empty() && ToLower(singularToken) != ToLower(materialToken)) {
            AddFilenameCandidate(candidates, seenCandidates, singularToken + ext);
        }
        AddFilenameCandidate(candidates, seenCandidates, materialToken + "map" + ext);
        if (!singularToken.empty()) {
            AddFilenameCandidate(candidates, seenCandidates, singularToken + "map" + ext);
        }
    }

    if (!lowerUsage.empty()) {
        AddFilenameCandidate(candidates, seenCandidates, materialToken + "_" + lowerUsage + ext);
        if (!singularToken.empty() && ToLower(singularToken) != ToLower(materialToken)) {
            AddFilenameCandidate(candidates, seenCandidates, singularToken + "_" + lowerUsage + ext);
        }
    }

    return candidates;
}

static std::string ResolveTexturePathFromHints(const std::string& preferredFilename,
                                               const std::vector<TextureReferenceHint>& hints,
                                               const std::string& currentPath,
                                               bool allowMaterialAliases,
                                               const std::unordered_map<std::string, std::vector<fs::path>>* texturesByFilename,
                                               const fs::path& sharedDir,
                                               const fs::path& projectDir) {
    if (!texturesByFilename) {
        return {};
    }

    std::vector<std::string> candidateFilenames;
    std::unordered_set<std::string> seenCandidates;

    auto appendCandidates = [&](const std::string& materialName, const std::string& usage) {
        std::vector<std::string> localCandidates =
            BuildTextureFilenameSearchCandidates(preferredFilename,
                                                 materialName,
                                                 usage,
                                                 currentPath,
                                                 allowMaterialAliases);
        for (const std::string& candidate : localCandidates) {
            std::string key = ToLower(candidate);
            if (!seenCandidates.insert(key).second) {
                continue;
            }
            candidateFilenames.push_back(candidate);
        }
    };

    if (hints.empty()) {
        appendCandidates(std::string{}, std::string{});
    } else {
        for (const auto& hint : hints) {
            appendCandidates(hint.materialName, hint.usage);
        }
    }

    for (const std::string& candidateFilename : candidateFilenames) {
        auto it = texturesByFilename->find(ToLower(candidateFilename));
        if (it == texturesByFilename->end() || it->second.empty()) {
            continue;
        }

        fs::path bestCandidate = ChooseBestTextureCandidate(it->second, hints, sharedDir);
        if (!bestCandidate.empty()) {
            return NormalizeAssetPath(bestCandidate.string(), projectDir);
        }
    }

    return {};
}

static std::string NormalizeForCompare(const fs::path& p) {
    std::string s = p.lexically_normal().generic_string();
    return ToLower(s);
}

static bool IsTextureExtension(const fs::path& p) {
    std::string ext = ToLower(p.extension().string());
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga";
}

static bool IsSafeExtension(std::string ext) {
    if (ext.empty()) return false;
    if (ext[0] != '.') return false;
    for (size_t i = 1; i < ext.size(); ++i) {
        if (!std::isalnum(static_cast<unsigned char>(ext[i]))) return false;
    }
    return true;
}

static std::string NormalizeAssetPath(const std::string& path, const fs::path& projectDir) {
    if (path.empty()) return {};
    std::string normalized = NormalizeSlashes(path);
    size_t pos = normalized.find("assets/");
    if (pos != std::string::npos) {
        return normalized.substr(pos);
    }
    size_t resourcesPos = normalized.find("resources/");
    if (resourcesPos != std::string::npos) {
        return normalized.substr(resourcesPos);
    }
    fs::path p(normalized);
    if (p.is_absolute() && !projectDir.empty()) {
        std::error_code ec;
        fs::path rel = fs::relative(p, projectDir, ec);
        if (!ec) {
            std::string relStr = NormalizeSlashes(rel.string());
            if (relStr.find("../") != std::string::npos) return normalized;
            if (relStr.rfind("assets/", 0) != 0 &&
                relStr.rfind("resources/", 0) != 0) {
                relStr = "assets/" + relStr;
            }
            return relStr;
        }
    }
    if (normalized.rfind("assets/", 0) != 0 &&
        normalized.rfind("resources/", 0) != 0) {
        return "assets/" + normalized;
    }
    return normalized;
}

static void AddTextureReferenceHint(
    std::unordered_map<std::string, std::vector<TextureReferenceHint>>& hintsByPath,
    const std::string& path,
    const fs::path& projectDir,
    const std::string& modelName,
    const std::string& modelReferenceKey,
    const std::string& materialName,
    const char* usage,
    const std::string& preferredFilename = {}) {
    std::string normalized = NormalizeAssetPath(path, projectDir);
    if (normalized.empty()) return;

    TextureReferenceHint hint;
    hint.modelName = modelName;
    hint.modelReferenceKey = modelReferenceKey;
    hint.materialName = materialName;
    hint.usage = usage ? usage : "";
    hint.preferredFilename = preferredFilename;
    hintsByPath[normalized].push_back(std::move(hint));
}

static std::string ExtractPreferredTextureFilename(const TextureSpecifier& tex) {
    if (tex.Path.empty()) return {};
    std::string normalized = NormalizeSlashes(tex.Path);
    if (normalized.rfind("embedded://", 0) == 0) {
        size_t slash = normalized.find_last_of('/');
        if (slash != std::string::npos && slash + 1 < normalized.size()) {
            std::string tail = normalized.substr(slash + 1);
            if (!tail.empty() && !IsDigitsOnly(tail) && tail.find('.') != std::string::npos) {
                return tail;
            }
        }
        return {};
    }
    return fs::path(normalized).filename().string();
}

static std::unordered_map<std::string, std::vector<SourceTextureFilenameHint>>
BuildSourceTextureFilenameHints(const fs::path& modelPath) {
    std::unordered_map<std::string, std::vector<SourceTextureFilenameHint>> hintsByMeshName;

    try {
        Model sourceModel = ModelLoader::LoadModel(modelPath.string());
        PreparedModel prepared = BuildPreparedModel(sourceModel);

        for (const auto& entry : prepared.Meshes) {
            auto& slots = hintsByMeshName[entry.NodeName];
            if (slots.size() < entry.Materials.size()) {
                slots.resize(entry.Materials.size());
            }

            for (size_t slotIndex = 0; slotIndex < entry.Materials.size(); ++slotIndex) {
                const MaterialSource& src = entry.Materials[slotIndex];
                SourceTextureFilenameHint& slotHints = slots[slotIndex];
                slotHints.albedo = ExtractPreferredTextureFilename(src.Albedo);
                slotHints.metallicRoughness = ExtractPreferredTextureFilename(src.MetallicRoughness);
                slotHints.normal = ExtractPreferredTextureFilename(src.Normal);
                slotHints.ao = ExtractPreferredTextureFilename(src.AO);
                slotHints.emission = ExtractPreferredTextureFilename(src.Emission);
                slotHints.displacement = ExtractPreferredTextureFilename(src.Displacement);
            }
        }
    } catch (...) {}

    return hintsByMeshName;
}

static std::string BuildModelReferenceKey(const std::string& path, const fs::path& projectDir) {
    std::string normalized = NormalizeAssetPath(path, projectDir);
    if (normalized.empty()) return {};

    fs::path referencePath = fs::path(NormalizeSlashes(normalized));
    if (IsModelAssetExtension(referencePath.extension().string())) {
        referencePath.replace_extension();
    }

    return ToLower(NormalizeSlashes(referencePath.string()));
}

static void MarkAffectedModelReference(const std::string& path,
                                       const fs::path& projectDir,
                                       std::unordered_set<std::string>& outAffectedModelKeys) {
    std::string key = BuildModelReferenceKey(path, projectDir);
    if (!key.empty()) {
        outAffectedModelKeys.insert(std::move(key));
    }
}

static fs::path AbsoluteFromVirtualPath(const std::string& path, const fs::path& projectDir) {
    if (path.empty()) return {};
    std::string normalized = NormalizeSlashes(path);
    fs::path p(normalized);
    if (p.is_absolute()) return p;
    if (!projectDir.empty()) return projectDir / normalized;
    return p;
}

static bool IsUnderDirectory(const fs::path& path, const fs::path& dir) {
    std::string p = NormalizeForCompare(path);
    std::string d = NormalizeForCompare(dir);
    if (d.empty()) return false;
    if (d.back() != '/') d.push_back('/');
    return p.rfind(d, 0) == 0;
}

static bool MoveFileSafe(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    fs::rename(src, dst, ec);
    if (!ec) return true;
    ec.clear();
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    fs::remove(src, ec);
    return !ec;
}

static bool WriteTextureMeta(const fs::path& metaPath,
                             const std::string& vpath,
                             const fs::path& absPath,
                             int& outMetaUpdates) {
    AssetMetadata meta;
    bool hasMeta = false;
    if (fs::exists(metaPath)) {
        try {
            std::ifstream in(metaPath);
            if (in.is_open()) {
                json j; in >> j;
                meta = j.get<AssetMetadata>();
                hasMeta = true;
            }
        } catch (...) {}
    }
    if (meta.guid.high == 0 && meta.guid.low == 0) {
        meta.guid = ClaymoreGUID::Generate();
    }
    meta.reference = AssetReference(meta.guid, 0, static_cast<int>(AssetType::Texture));
    meta.type = "texture";
    meta.sourcePath = vpath;
    meta.processedPath = vpath;
    try {
        meta.hash = AssetPipeline::Instance().ComputeHash(absPath.string());
    } catch (...) {}

    try {
        std::ofstream out(metaPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        json j = meta;
        out << j.dump(4);
        out.close();
    } catch (...) {
        return false;
    }

    AssetRegistry::Instance().SetMetadata(vpath, meta);
    AssetLibrary::Instance().RegisterAsset(meta.reference, AssetType::Texture, vpath, fs::path(vpath).filename().string());
    outMetaUpdates += hasMeta ? 1 : 0;
    return true;
}

static bool EnsureSharedTextureByName(const fs::path& sharedDir,
                                      const std::string& filename,
                                      const fs::path& canonicalAbs,
                                      CleanupReport& out,
                                      fs::path& outSharedAbs) {
    outSharedAbs = sharedDir / filename;
    if (canonicalAbs.empty()) return false;

    fs::file_time_type canonicalTime = SafeLastWriteTime(canonicalAbs);
    bool sharedExists = fs::exists(outSharedAbs);
    fs::file_time_type sharedTime = sharedExists ? SafeLastWriteTime(outSharedAbs) : fs::file_time_type::min();

    if (!sharedExists || sharedTime < canonicalTime) {
        if (!sharedExists) {
            if (!MoveFileSafe(canonicalAbs, outSharedAbs)) {
                out.logLines.push_back("Failed to move texture to shared: " + canonicalAbs.string());
                return false;
            }
            fs::path srcMeta = canonicalAbs; srcMeta += ".meta";
            fs::path dstMeta = outSharedAbs; dstMeta += ".meta";
            if (fs::exists(srcMeta)) {
                MoveFileSafe(srcMeta, dstMeta);
            }
        } else {
            std::error_code ec;
            fs::copy_file(canonicalAbs, outSharedAbs, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                out.logLines.push_back("Failed to overwrite shared texture: " + outSharedAbs.string());
                return false;
            }
        }
        out.texturesMovedToShared += 1;
    }
    return true;
}

static bool EnsureSharedTextureByNameImport(const fs::path& sharedDir,
                                            const fs::path& modelTexDir,
                                            const std::string& filename,
                                            const fs::path& canonicalAbs,
                                            ImportDedupReport& out,
                                            fs::path& outSharedAbs) {
    outSharedAbs = sharedDir / filename;
    if (canonicalAbs.empty()) return false;

    fs::file_time_type canonicalTime = SafeLastWriteTime(canonicalAbs);
    bool sharedExists = fs::exists(outSharedAbs);
    fs::file_time_type sharedTime = sharedExists ? SafeLastWriteTime(outSharedAbs) : fs::file_time_type::min();

    if (!sharedExists || sharedTime < canonicalTime) {
        if (IsUnderDirectory(canonicalAbs, modelTexDir)) {
            if (!MoveFileSafe(canonicalAbs, outSharedAbs)) {
                out.logLines.push_back("Failed to move texture to shared: " + canonicalAbs.string());
                return false;
            }
            fs::path srcMeta = canonicalAbs; srcMeta += ".meta";
            fs::path dstMeta = outSharedAbs; dstMeta += ".meta";
            if (fs::exists(srcMeta)) {
                MoveFileSafe(srcMeta, dstMeta);
            }
        } else {
            std::error_code ec;
            fs::copy_file(canonicalAbs, outSharedAbs, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                out.logLines.push_back("Failed to copy texture to shared: " + canonicalAbs.string());
                return false;
            }
        }
        out.texturesShared += 1;
    }
    return true;
}

static bool SyncTextureToPreferredSharedPath(const fs::path& sharedDir,
                                             const fs::path& sourceAbs,
                                             const std::string& preferredFilename,
                                             const std::string& sourceHash,
                                             const fs::path& moveRootDir,
                                             std::vector<std::string>* logLines,
                                             int* outSharedWrites,
                                             fs::path& outSharedAbs) {
    if (sourceAbs.empty() || preferredFilename.empty()) {
        return false;
    }

    const bool weakPreferredName = IsWeakTextureFilename(preferredFilename);
    outSharedAbs = weakPreferredName
        ? ResolveDesiredSharedPath(sharedDir, preferredFilename, sourceHash, sourceAbs)
        : (sharedDir / preferredFilename);
    if (outSharedAbs.empty()) {
        return false;
    }

    const bool sharedExists = fs::exists(outSharedAbs);
    const bool sourceCanMove = !moveRootDir.empty() &&
                               IsUnderDirectory(sourceAbs, moveRootDir) &&
                               NormalizeForCompare(sourceAbs) != NormalizeForCompare(outSharedAbs);

    if (sharedExists) {
        if (!sourceHash.empty()) {
            try {
                std::string existingHash = AssetPipeline::Instance().ComputeFileHash(outSharedAbs.string());
                if (existingHash == sourceHash) {
                    return true;
                }
            } catch (...) {}
        }

        if (weakPreferredName || SafeLastWriteTime(outSharedAbs) >= SafeLastWriteTime(sourceAbs)) {
            return true;
        }
    }

    std::error_code ec;
    fs::create_directories(outSharedAbs.parent_path(), ec);

    auto logFailure = [&](const std::string& action) {
        if (!logLines) return;
        logLines->push_back("Failed to " + action + " shared texture: " + sourceAbs.string());
    };

    if (!sharedExists && sourceCanMove) {
        if (!MoveFileSafe(sourceAbs, outSharedAbs)) {
            logFailure("move preferred");
            return false;
        }
        fs::path srcMeta = sourceAbs;
        srcMeta += ".meta";
        fs::path dstMeta = outSharedAbs;
        dstMeta += ".meta";
        if (fs::exists(srcMeta)) {
            MoveFileSafe(srcMeta, dstMeta);
        }
    } else {
        ec.clear();
        fs::copy_file(sourceAbs, outSharedAbs, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            logFailure(sharedExists ? "overwrite preferred" : "copy preferred");
            return false;
        }
    }

    if (outSharedWrites) {
        *outSharedWrites += 1;
    }

    return true;
}

static bool EnsureSharedTextureCopy(const fs::path& sharedDir,
                                    const std::string& filename,
                                    const fs::path& canonicalAbs,
                                    std::vector<std::string>* logLines,
                                    fs::path& outSharedAbs) {
    outSharedAbs = sharedDir / filename;
    if (canonicalAbs.empty()) return false;

    fs::file_time_type canonicalTime = SafeLastWriteTime(canonicalAbs);
    bool sharedExists = fs::exists(outSharedAbs);
    fs::file_time_type sharedTime = sharedExists ? SafeLastWriteTime(outSharedAbs) : fs::file_time_type::min();

    if (!sharedExists || sharedTime < canonicalTime) {
        std::error_code ec;
        fs::create_directories(outSharedAbs.parent_path(), ec);
        ec.clear();
        fs::copy_file(canonicalAbs, outSharedAbs, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            if (logLines) {
                logLines->push_back("Failed to copy canonical texture to shared: " + canonicalAbs.string());
            }
            return false;
        }

        fs::path srcMeta = canonicalAbs;
        srcMeta += ".meta";
        fs::path dstMeta = outSharedAbs;
        dstMeta += ".meta";
        if (fs::exists(srcMeta)) {
            std::error_code metaEc;
            fs::copy_file(srcMeta, dstMeta, fs::copy_options::overwrite_existing, metaEc);
        }
    }

    return true;
}

static void RemoveTextureFileAndMeta(const fs::path& absPath,
                                     const std::string& virtualPath,
                                     int& outRemoved,
                                     int& outMetaUpdates) {
    fs::path metaPath = absPath;
    metaPath += ".meta";
    if (fs::exists(metaPath)) {
        try {
            std::ifstream in(metaPath);
            if (in.is_open()) {
                json j; in >> j;
                AssetMetadata meta = j.get<AssetMetadata>();
                if (!(meta.guid.high == 0 && meta.guid.low == 0)) {
                    AssetLibrary::Instance().UnregisterAsset(meta.reference);
                }
                AssetRegistry::Instance().RemoveMetadata(virtualPath);
                outMetaUpdates += 1;
            }
        } catch (...) {}
    }
    std::error_code ec;
    fs::remove(metaPath, ec);
    ec.clear();
    if (fs::remove(absPath, ec)) {
        outRemoved += 1;
    }
}

static bool RemapTexturePath(std::string& path,
                             const std::unordered_map<std::string, std::string>& mapping,
                             const fs::path& projectDir) {
    if (path.empty()) return false;
    std::string norm = NormalizeAssetPath(path, projectDir);
    std::string resolved = ResolveFinalMappedPath(norm, mapping);
    if (resolved.empty() || resolved == norm) return false;
    path = resolved;
    return true;
}

static bool ApplyTextureMappingToMeta(json& meta,
                                      const std::unordered_map<std::string, std::string>& mapping,
                                      const fs::path& projectDir,
                                      const std::unordered_map<std::string, std::vector<SourceTextureFilenameHint>>* sourceFilenameHints = nullptr,
                                      const std::unordered_map<std::string, std::vector<fs::path>>* texturesByFilename = nullptr,
                                      const fs::path& sharedDir = {}) {
    bool changed = false;
    std::optional<ModelImportSettings> importSettings;
    if (meta.contains("importSettings") && meta["importSettings"].is_object()) {
        importSettings = ModelImportSettings::FromJson(meta["importSettings"]);
    }

    std::unordered_map<std::string, std::vector<std::string>> materialNamesByEntry;
    if (meta.contains("entries") && meta["entries"].is_array()) {
        for (const auto& entry : meta["entries"]) {
            if (!entry.contains("materials") || !entry["materials"].is_array()) {
                continue;
            }

            std::string entryName = entry.value("name", std::string{});
            auto& materialNames = materialNamesByEntry[entryName];
            materialNames.reserve(entry["materials"].size());
            for (const auto& matJson : entry["materials"]) {
                MaterialSource src = material_serialization::FromJson(matJson);
                materialNames.push_back(src.Name);
            }
        }
    }

    auto hasExistingTextureFile = [&](const std::string& path) -> bool {
        if (path.empty()) return false;
        fs::path abs = AbsoluteFromVirtualPath(path, projectDir);
        return !abs.empty() && fs::exists(abs);
    };

    auto resolvesUnderSharedDir = [&](const std::string& path) -> bool {
        if (sharedDir.empty() || path.empty()) {
            return false;
        }

        fs::path abs = AbsoluteFromVirtualPath(path, projectDir);
        return !abs.empty() && IsUnderDirectory(abs, sharedDir);
    };

    auto buildHintContext = [](const std::string& materialName,
                               const char* usage,
                               const std::string& modelName = std::string{}) {
        std::vector<TextureReferenceHint> hints;
        TextureReferenceHint hint;
        hint.modelName = modelName;
        hint.materialName = materialName;
        hint.usage = usage ? usage : "";
        hints.push_back(std::move(hint));
        return hints;
    };

    auto resolveCurrentTexturePath = [&](const std::string& currentPath) -> std::string {
        std::string currentNormalized = NormalizeAssetPath(currentPath, projectDir);
        std::string currentResolved = currentNormalized.empty()
            ? NormalizeSlashes(currentPath)
            : ResolveFinalMappedPath(currentNormalized, mapping);
        if (currentResolved.empty()) {
            currentResolved = currentNormalized.empty() ? NormalizeSlashes(currentPath) : currentNormalized;
        }
        return currentResolved;
    };

    auto currentTextureNeedsHeuristicRepair = [&](const std::string& currentPath) -> bool {
        std::string currentResolved = resolveCurrentTexturePath(currentPath);
        if (!hasExistingTextureFile(currentResolved)) {
            return true;
        }

        std::string currentFilename = fs::path(currentResolved).filename().string();
        return IsWeakTextureFilename(currentFilename);
    };

    auto resolvePreferredTexturePath = [&](const std::string& preferredFilename,
                                           const std::vector<TextureReferenceHint>& hintContext,
                                           const std::string& currentPath,
                                           bool allowMaterialAliases) -> std::string {
        return ResolveTexturePathFromHints(preferredFilename,
                                           hintContext,
                                           currentPath,
                                           allowMaterialAliases,
                                           texturesByFilename,
                                           sharedDir,
                                           projectDir);
    };

    auto shouldRepairToResolvedTexturePath = [&](const std::string& currentPath,
                                                 const std::string& resolvedPreferredPath) -> bool {
        if (resolvedPreferredPath.empty()) {
            return false;
        }

        std::string currentResolved = resolveCurrentTexturePath(currentPath);
        std::string currentFilename = fs::path(currentResolved).filename().string();
        std::string desiredFilename = fs::path(resolvedPreferredPath).filename().string();
        bool currentExists = hasExistingTextureFile(currentResolved);
        bool currentWeak = IsWeakTextureFilename(currentFilename);

        if (currentResolved == resolvedPreferredPath) {
            return false;
        }

        if (currentExists &&
            ToLower(currentFilename) == ToLower(desiredFilename) &&
            !currentWeak) {
            return false;
        }

        if (!currentExists) {
            return true;
        }

        if (currentWeak) {
            return true;
        }

        // Shared textures are the normalized cache. If a model slot already has a
        // known authored filename for this texture, keep the shared filename aligned
        // with that source name even when the current shared file exists.
        if (resolvesUnderSharedDir(currentResolved) &&
            ToLower(currentFilename) != ToLower(desiredFilename)) {
            return true;
        }

        return false;
    };

    auto repairTexturePathToPreferredFilename = [&](std::string& path,
                                                    const std::string& preferredFilename,
                                                    const std::vector<TextureReferenceHint>& hintContext) -> bool {
        if (path.empty() && preferredFilename.empty()) {
            return false;
        }

        bool allowMaterialAliases = preferredFilename.empty() &&
                                    !path.empty() &&
                                    currentTextureNeedsHeuristicRepair(path);
        std::string resolvedPreferredPath =
            resolvePreferredTexturePath(preferredFilename, hintContext, path, allowMaterialAliases);
        if (!shouldRepairToResolvedTexturePath(path, resolvedPreferredPath)) {
            return false;
        }

        path = resolvedPreferredPath;
        return true;
    };

    auto repairTextureSpecifierToPreferredFilename = [&](TextureSpecifier& spec,
                                                         const std::string& preferredFilename,
                                                         const std::vector<TextureReferenceHint>& hintContext) -> bool {
        if (!repairTexturePathToPreferredFilename(spec.Path, preferredFilename, hintContext)) {
            return false;
        }
        spec.Embedded = EmbeddedTextureData{};
        return true;
    };

    auto usageTokenFromHints = [&](const std::vector<TextureReferenceHint>& hintContext) -> std::string {
        for (const auto& hint : hintContext) {
            std::string usage = ToLower(SanitizeFilenameToken(hint.usage));
            if (!usage.empty()) {
                return usage;
            }
        }
        return {};
    };

    auto shouldClearCorruptedNonAlbedoPath = [&](const std::string& path,
                                                 const std::string& albedoPath,
                                                 const std::string& preferredFilename,
                                                 const std::string& albedoPreferredFilename,
                                                 const std::vector<TextureReferenceHint>& hintContext) -> bool {
        if (!preferredFilename.empty() || albedoPreferredFilename.empty()) {
            return false;
        }
        if (path.empty() || albedoPath.empty()) {
            return false;
        }

        std::string usage = usageTokenFromHints(hintContext);
        if (usage.empty() || usage == "albedo") {
            return false;
        }

        std::string resolvedPath = resolveCurrentTexturePath(path);
        std::string resolvedAlbedoPath = resolveCurrentTexturePath(albedoPath);
        if (resolvedPath.empty() || resolvedAlbedoPath.empty()) {
            return false;
        }
        if (ToLower(resolvedPath) != ToLower(resolvedAlbedoPath)) {
            return false;
        }

        std::string filename = ToLower(fs::path(resolvedPath).filename().string());
        if (filename.empty()) {
            return false;
        }

        if (usage == "normal" && filename.find("norm") != std::string::npos) {
            return false;
        }
        if (usage == "ao" && filename.find("ao") != std::string::npos) {
            return false;
        }
        if (usage == "emission" && filename.find("emiss") != std::string::npos) {
            return false;
        }
        if (usage == "displacement" && filename.find("disp") != std::string::npos) {
            return false;
        }
        if (usage == "metallicroughness" &&
            (filename.find("metal") != std::string::npos ||
             filename.find("rough") != std::string::npos ||
             filename.find("_mr") != std::string::npos ||
             StartsWithIgnoreCase(filename, "mr_"))) {
            return false;
        }

        return true;
    };

    auto clearCorruptedTextureSpecifierIfNeeded = [&](TextureSpecifier& spec,
                                                      const TextureSpecifier& albedoSpec,
                                                      const std::string& preferredFilename,
                                                      const std::string& albedoPreferredFilename,
                                                      const std::vector<TextureReferenceHint>& hintContext) -> bool {
        if (!shouldClearCorruptedNonAlbedoPath(spec.Path,
                                               albedoSpec.Path,
                                               preferredFilename,
                                               albedoPreferredFilename,
                                               hintContext)) {
            return false;
        }

        spec.Path.clear();
        spec.Embedded = EmbeddedTextureData{};
        return true;
    };

    auto clearCorruptedPresetOverrideIfNeeded = [&](bool& overrideEnabled,
                                                    std::string& path,
                                                    const std::string& albedoPath,
                                                    const std::string& preferredFilename,
                                                    const std::string& albedoPreferredFilename,
                                                    const std::vector<TextureReferenceHint>& hintContext) -> bool {
        if (!overrideEnabled) {
            return false;
        }
        if (!shouldClearCorruptedNonAlbedoPath(path,
                                               albedoPath,
                                               preferredFilename,
                                               albedoPreferredFilename,
                                               hintContext)) {
            return false;
        }

        overrideEnabled = false;
        path.clear();
        return true;
    };

    auto applyPresetFallback = [&](const std::string& entryName, size_t slotIndex, MaterialSource& src) -> bool {
        if (!importSettings.has_value() || entryName.empty()) return false;
        bool localChanged = false;
        for (const auto& preset : importSettings->MaterialPresets) {
            if (preset.MeshName != entryName) continue;
            if (static_cast<size_t>(std::max(0, preset.MaterialSlot)) != slotIndex) continue;

            if (preset.OverrideAlbedo && !preset.AlbedoPath.empty() && !hasExistingTextureFile(src.Albedo.Path)) {
                src.Albedo.Path = NormalizeAssetPath(preset.AlbedoPath, projectDir);
                localChanged = true;
            }
            if (preset.OverrideNormal && !preset.NormalPath.empty() && !hasExistingTextureFile(src.Normal.Path)) {
                src.Normal.Path = NormalizeAssetPath(preset.NormalPath, projectDir);
                localChanged = true;
            }
            if (preset.OverrideMetallicRoughness && !preset.MetallicRoughnessPath.empty() && !hasExistingTextureFile(src.MetallicRoughness.Path)) {
                src.MetallicRoughness.Path = NormalizeAssetPath(preset.MetallicRoughnessPath, projectDir);
                localChanged = true;
            }
            if (preset.OverrideAO && !preset.AOPath.empty() && !hasExistingTextureFile(src.AO.Path)) {
                src.AO.Path = NormalizeAssetPath(preset.AOPath, projectDir);
                localChanged = true;
            }
            if (preset.OverrideEmission && !preset.EmissionPath.empty() && !hasExistingTextureFile(src.Emission.Path)) {
                src.Emission.Path = NormalizeAssetPath(preset.EmissionPath, projectDir);
                localChanged = true;
            }
            if (preset.OverrideDisplacement && !preset.DisplacementPath.empty() && !hasExistingTextureFile(src.Displacement.Path)) {
                src.Displacement.Path = NormalizeAssetPath(preset.DisplacementPath, projectDir);
                localChanged = true;
            }
            break;
        }
        return localChanged;
    };

    if (meta.contains("entries") && meta["entries"].is_array()) {
        for (auto& entry : meta["entries"]) {
            if (!entry.contains("materials") || !entry["materials"].is_array()) continue;
            std::string entryName = entry.value("name", std::string{});
            const std::vector<SourceTextureFilenameHint>* slotHints = nullptr;
            if (sourceFilenameHints) {
                auto sourceIt = sourceFilenameHints->find(entryName);
                if (sourceIt != sourceFilenameHints->end()) {
                    slotHints = &sourceIt->second;
                }
            }
            size_t slotIndex = 0;
            for (auto& matJson : entry["materials"]) {
                MaterialSource src = material_serialization::FromJson(matJson);
                const SourceTextureFilenameHint* sourceHint =
                    (slotHints && slotIndex < slotHints->size()) ? &(*slotHints)[slotIndex] : nullptr;
                std::vector<TextureReferenceHint> albedoHintContext = buildHintContext(src.Name, "albedo", entryName);
                std::vector<TextureReferenceHint> metallicHintContext = buildHintContext(src.Name, "metallicRoughness", entryName);
                std::vector<TextureReferenceHint> normalHintContext = buildHintContext(src.Name, "normal", entryName);
                std::vector<TextureReferenceHint> aoHintContext = buildHintContext(src.Name, "ao", entryName);
                std::vector<TextureReferenceHint> emissionHintContext = buildHintContext(src.Name, "emission", entryName);
                std::vector<TextureReferenceHint> displacementHintContext = buildHintContext(src.Name, "displacement", entryName);
                bool matChanged = false;
                matChanged |= RemapTexturePath(src.Albedo.Path, mapping, projectDir);
                matChanged |= RemapTexturePath(src.MetallicRoughness.Path, mapping, projectDir);
                matChanged |= RemapTexturePath(src.Normal.Path, mapping, projectDir);
                matChanged |= RemapTexturePath(src.AO.Path, mapping, projectDir);
                matChanged |= RemapTexturePath(src.Emission.Path, mapping, projectDir);
                matChanged |= RemapTexturePath(src.Displacement.Path, mapping, projectDir);
                matChanged |= repairTextureSpecifierToPreferredFilename(src.Albedo, sourceHint ? sourceHint->albedo : std::string{}, albedoHintContext);
                matChanged |= repairTextureSpecifierToPreferredFilename(src.MetallicRoughness, sourceHint ? sourceHint->metallicRoughness : std::string{}, metallicHintContext);
                matChanged |= repairTextureSpecifierToPreferredFilename(src.Normal, sourceHint ? sourceHint->normal : std::string{}, normalHintContext);
                matChanged |= repairTextureSpecifierToPreferredFilename(src.AO, sourceHint ? sourceHint->ao : std::string{}, aoHintContext);
                matChanged |= repairTextureSpecifierToPreferredFilename(src.Emission, sourceHint ? sourceHint->emission : std::string{}, emissionHintContext);
                matChanged |= repairTextureSpecifierToPreferredFilename(src.Displacement, sourceHint ? sourceHint->displacement : std::string{}, displacementHintContext);
                matChanged |= applyPresetFallback(entryName, slotIndex, src);
                matChanged |= clearCorruptedTextureSpecifierIfNeeded(src.MetallicRoughness,
                                                                     src.Albedo,
                                                                     sourceHint ? sourceHint->metallicRoughness : std::string{},
                                                                     sourceHint ? sourceHint->albedo : std::string{},
                                                                     metallicHintContext);
                matChanged |= clearCorruptedTextureSpecifierIfNeeded(src.Normal,
                                                                     src.Albedo,
                                                                     sourceHint ? sourceHint->normal : std::string{},
                                                                     sourceHint ? sourceHint->albedo : std::string{},
                                                                     normalHintContext);
                matChanged |= clearCorruptedTextureSpecifierIfNeeded(src.AO,
                                                                     src.Albedo,
                                                                     sourceHint ? sourceHint->ao : std::string{},
                                                                     sourceHint ? sourceHint->albedo : std::string{},
                                                                     aoHintContext);
                matChanged |= clearCorruptedTextureSpecifierIfNeeded(src.Emission,
                                                                     src.Albedo,
                                                                     sourceHint ? sourceHint->emission : std::string{},
                                                                     sourceHint ? sourceHint->albedo : std::string{},
                                                                     emissionHintContext);
                matChanged |= clearCorruptedTextureSpecifierIfNeeded(src.Displacement,
                                                                     src.Albedo,
                                                                     sourceHint ? sourceHint->displacement : std::string{},
                                                                     sourceHint ? sourceHint->albedo : std::string{},
                                                                     displacementHintContext);
                if (matChanged) {
                    matJson = material_serialization::ToJson(src);
                    changed = true;
                }
                ++slotIndex;
            }
        }
    }

    if (importSettings.has_value()) {
        ModelImportSettings settings = *importSettings;
        bool settingsChanged = false;
        for (auto& preset : settings.MaterialPresets) {
            const SourceTextureFilenameHint* sourceHint = nullptr;
            std::string materialName;
            if (sourceFilenameHints) {
                auto sourceIt = sourceFilenameHints->find(preset.MeshName);
                if (sourceIt != sourceFilenameHints->end()) {
                    size_t slotIndex = static_cast<size_t>(std::max(0, preset.MaterialSlot));
                    if (slotIndex < sourceIt->second.size()) {
                        sourceHint = &sourceIt->second[slotIndex];
                    }
                }
            }
            auto materialNamesIt = materialNamesByEntry.find(preset.MeshName);
            if (materialNamesIt != materialNamesByEntry.end()) {
                size_t slotIndex = static_cast<size_t>(std::max(0, preset.MaterialSlot));
                if (slotIndex < materialNamesIt->second.size()) {
                    materialName = materialNamesIt->second[slotIndex];
                }
            }

            std::vector<TextureReferenceHint> albedoHintContext = buildHintContext(materialName, "albedo", preset.MeshName);
            std::vector<TextureReferenceHint> normalHintContext = buildHintContext(materialName, "normal", preset.MeshName);
            std::vector<TextureReferenceHint> metallicHintContext = buildHintContext(materialName, "metallicRoughness", preset.MeshName);
            std::vector<TextureReferenceHint> aoHintContext = buildHintContext(materialName, "ao", preset.MeshName);
            std::vector<TextureReferenceHint> emissionHintContext = buildHintContext(materialName, "emission", preset.MeshName);
            std::vector<TextureReferenceHint> displacementHintContext = buildHintContext(materialName, "displacement", preset.MeshName);

            if (preset.OverrideAlbedo) settingsChanged |= RemapTexturePath(preset.AlbedoPath, mapping, projectDir);
            if (preset.OverrideNormal) settingsChanged |= RemapTexturePath(preset.NormalPath, mapping, projectDir);
            if (preset.OverrideMetallicRoughness) settingsChanged |= RemapTexturePath(preset.MetallicRoughnessPath, mapping, projectDir);
            if (preset.OverrideAO) settingsChanged |= RemapTexturePath(preset.AOPath, mapping, projectDir);
            if (preset.OverrideEmission) settingsChanged |= RemapTexturePath(preset.EmissionPath, mapping, projectDir);
            if (preset.OverrideDisplacement) settingsChanged |= RemapTexturePath(preset.DisplacementPath, mapping, projectDir);
            if (preset.OverrideAlbedo) settingsChanged |= repairTexturePathToPreferredFilename(preset.AlbedoPath, sourceHint ? sourceHint->albedo : std::string{}, albedoHintContext);
            if (preset.OverrideNormal) settingsChanged |= repairTexturePathToPreferredFilename(preset.NormalPath, sourceHint ? sourceHint->normal : std::string{}, normalHintContext);
            if (preset.OverrideMetallicRoughness) settingsChanged |= repairTexturePathToPreferredFilename(preset.MetallicRoughnessPath, sourceHint ? sourceHint->metallicRoughness : std::string{}, metallicHintContext);
            if (preset.OverrideAO) settingsChanged |= repairTexturePathToPreferredFilename(preset.AOPath, sourceHint ? sourceHint->ao : std::string{}, aoHintContext);
            if (preset.OverrideEmission) settingsChanged |= repairTexturePathToPreferredFilename(preset.EmissionPath, sourceHint ? sourceHint->emission : std::string{}, emissionHintContext);
            if (preset.OverrideDisplacement) settingsChanged |= repairTexturePathToPreferredFilename(preset.DisplacementPath, sourceHint ? sourceHint->displacement : std::string{}, displacementHintContext);
            settingsChanged |= clearCorruptedPresetOverrideIfNeeded(preset.OverrideMetallicRoughness,
                                                                    preset.MetallicRoughnessPath,
                                                                    preset.AlbedoPath,
                                                                    sourceHint ? sourceHint->metallicRoughness : std::string{},
                                                                    sourceHint ? sourceHint->albedo : std::string{},
                                                                    metallicHintContext);
            settingsChanged |= clearCorruptedPresetOverrideIfNeeded(preset.OverrideNormal,
                                                                    preset.NormalPath,
                                                                    preset.AlbedoPath,
                                                                    sourceHint ? sourceHint->normal : std::string{},
                                                                    sourceHint ? sourceHint->albedo : std::string{},
                                                                    normalHintContext);
            settingsChanged |= clearCorruptedPresetOverrideIfNeeded(preset.OverrideAO,
                                                                    preset.AOPath,
                                                                    preset.AlbedoPath,
                                                                    sourceHint ? sourceHint->ao : std::string{},
                                                                    sourceHint ? sourceHint->albedo : std::string{},
                                                                    aoHintContext);
            settingsChanged |= clearCorruptedPresetOverrideIfNeeded(preset.OverrideEmission,
                                                                    preset.EmissionPath,
                                                                    preset.AlbedoPath,
                                                                    sourceHint ? sourceHint->emission : std::string{},
                                                                    sourceHint ? sourceHint->albedo : std::string{},
                                                                    emissionHintContext);
            settingsChanged |= clearCorruptedPresetOverrideIfNeeded(preset.OverrideDisplacement,
                                                                    preset.DisplacementPath,
                                                                    preset.AlbedoPath,
                                                                    sourceHint ? sourceHint->displacement : std::string{},
                                                                    sourceHint ? sourceHint->albedo : std::string{},
                                                                    displacementHintContext);
        }
        if (settingsChanged) {
            meta["importSettings"] = settings.ToJson();
            changed = true;
        }
    }
    return changed;
}

static bool ResolveModelMetaPath(const fs::path& modelPath, std::string& outMetaPath) {
    fs::path metaPath = modelPath.parent_path() / (modelPath.stem().string() + ".meta");
    if (fs::exists(metaPath)) {
        outMetaPath = metaPath.string();
        return true;
    }
    BuiltModelPaths built;
    if (EnsureModelCache(modelPath.string(), built)) {
        outMetaPath = built.metaPath;
        return true;
    }
    return false;
}

static void CollectTexturePathsFromMeta(const json& meta,
                                        const fs::path& projectDir,
                                        std::vector<std::string>& outPaths) {
    auto addPath = [&](const std::string& p) {
        if (p.empty()) return;
        std::string norm = NormalizeAssetPath(p, projectDir);
        if (norm.empty()) return;
        outPaths.push_back(norm);
    };

    if (meta.contains("entries") && meta["entries"].is_array()) {
        for (const auto& entry : meta["entries"]) {
            if (!entry.contains("materials") || !entry["materials"].is_array()) continue;
            for (const auto& matJson : entry["materials"]) {
                MaterialSource src = material_serialization::FromJson(matJson);
                addPath(src.Albedo.Path);
                addPath(src.MetallicRoughness.Path);
                addPath(src.Normal.Path);
                addPath(src.AO.Path);
                addPath(src.Emission.Path);
                addPath(src.Displacement.Path);
            }
        }
    }

    if (meta.contains("importSettings") && meta["importSettings"].is_object()) {
        ModelImportSettings settings = ModelImportSettings::FromJson(meta["importSettings"]);
        for (const auto& preset : settings.MaterialPresets) {
            if (preset.OverrideAlbedo) addPath(preset.AlbedoPath);
            if (preset.OverrideNormal) addPath(preset.NormalPath);
            if (preset.OverrideMetallicRoughness) addPath(preset.MetallicRoughnessPath);
            if (preset.OverrideAO) addPath(preset.AOPath);
            if (preset.OverrideEmission) addPath(preset.EmissionPath);
            if (preset.OverrideDisplacement) addPath(preset.DisplacementPath);
        }
    }
}

static void CollectTextureReferenceHintsFromMeta(
    const json& meta,
    const std::string& modelName,
    const std::string& modelReferencePath,
    const fs::path& projectDir,
    const std::unordered_map<std::string, std::vector<SourceTextureFilenameHint>>* sourceFilenameHints,
    std::unordered_map<std::string, std::vector<TextureReferenceHint>>& hintsByPath) {
    std::string modelReferenceKey = BuildModelReferenceKey(modelReferencePath, projectDir);
    if (meta.contains("entries") && meta["entries"].is_array()) {
        for (const auto& entry : meta["entries"]) {
            if (!entry.contains("materials") || !entry["materials"].is_array()) continue;
            std::string entryName = entry.value("name", std::string{});
            const std::vector<SourceTextureFilenameHint>* slotHints = nullptr;
            if (sourceFilenameHints) {
                auto sourceIt = sourceFilenameHints->find(entryName);
                if (sourceIt != sourceFilenameHints->end()) {
                    slotHints = &sourceIt->second;
                }
            }
            size_t slotIndex = 0;
            for (const auto& matJson : entry["materials"]) {
                MaterialSource src = material_serialization::FromJson(matJson);
                const SourceTextureFilenameHint* sourceHint =
                    (slotHints && slotIndex < slotHints->size()) ? &(*slotHints)[slotIndex] : nullptr;
                AddTextureReferenceHint(hintsByPath, src.Albedo.Path, projectDir, modelName, modelReferenceKey, src.Name, "albedo",
                                        sourceHint ? sourceHint->albedo : std::string{});
                AddTextureReferenceHint(hintsByPath, src.MetallicRoughness.Path, projectDir, modelName, modelReferenceKey, src.Name, "metallicRoughness",
                                        sourceHint ? sourceHint->metallicRoughness : std::string{});
                AddTextureReferenceHint(hintsByPath, src.Normal.Path, projectDir, modelName, modelReferenceKey, src.Name, "normal",
                                        sourceHint ? sourceHint->normal : std::string{});
                AddTextureReferenceHint(hintsByPath, src.AO.Path, projectDir, modelName, modelReferenceKey, src.Name, "ao",
                                        sourceHint ? sourceHint->ao : std::string{});
                AddTextureReferenceHint(hintsByPath, src.Emission.Path, projectDir, modelName, modelReferenceKey, src.Name, "emission",
                                        sourceHint ? sourceHint->emission : std::string{});
                AddTextureReferenceHint(hintsByPath, src.Displacement.Path, projectDir, modelName, modelReferenceKey, src.Name, "displacement",
                                        sourceHint ? sourceHint->displacement : std::string{});
                ++slotIndex;
            }
        }
    }

    if (meta.contains("importSettings") && meta["importSettings"].is_object()) {
        ModelImportSettings settings = ModelImportSettings::FromJson(meta["importSettings"]);
        for (const auto& preset : settings.MaterialPresets) {
            std::string presetLabel = preset.MeshName;
            if (!presetLabel.empty()) presetLabel += "_";
            presetLabel += "slot" + std::to_string(preset.MaterialSlot);

            const SourceTextureFilenameHint* sourceHint = nullptr;
            if (sourceFilenameHints) {
                auto sourceIt = sourceFilenameHints->find(preset.MeshName);
                if (sourceIt != sourceFilenameHints->end()) {
                    size_t slotIndex = static_cast<size_t>(std::max(0, preset.MaterialSlot));
                    if (slotIndex < sourceIt->second.size()) {
                        sourceHint = &sourceIt->second[slotIndex];
                    }
                }
            }

            if (preset.OverrideAlbedo) AddTextureReferenceHint(hintsByPath, preset.AlbedoPath, projectDir, modelName, modelReferenceKey, presetLabel, "albedo",
                                                               sourceHint ? sourceHint->albedo : std::string{});
            if (preset.OverrideNormal) AddTextureReferenceHint(hintsByPath, preset.NormalPath, projectDir, modelName, modelReferenceKey, presetLabel, "normal",
                                                               sourceHint ? sourceHint->normal : std::string{});
            if (preset.OverrideMetallicRoughness) AddTextureReferenceHint(hintsByPath, preset.MetallicRoughnessPath, projectDir, modelName, modelReferenceKey, presetLabel, "metallicRoughness",
                                                                          sourceHint ? sourceHint->metallicRoughness : std::string{});
            if (preset.OverrideAO) AddTextureReferenceHint(hintsByPath, preset.AOPath, projectDir, modelName, modelReferenceKey, presetLabel, "ao",
                                                           sourceHint ? sourceHint->ao : std::string{});
            if (preset.OverrideEmission) AddTextureReferenceHint(hintsByPath, preset.EmissionPath, projectDir, modelName, modelReferenceKey, presetLabel, "emission",
                                                                 sourceHint ? sourceHint->emission : std::string{});
            if (preset.OverrideDisplacement) AddTextureReferenceHint(hintsByPath, preset.DisplacementPath, projectDir, modelName, modelReferenceKey, presetLabel, "displacement",
                                                                     sourceHint ? sourceHint->displacement : std::string{});
        }
    }
}

static std::unordered_map<std::string, std::vector<TextureReferenceHint>> ResolveHintsByFinalPath(
    const std::unordered_map<std::string, std::vector<TextureReferenceHint>>& hintsByPath,
    const std::unordered_map<std::string, std::string>& pathMapping) {
    std::unordered_map<std::string, std::vector<TextureReferenceHint>> resolvedHints;
    for (const auto& kv : hintsByPath) {
        std::string resolvedPath = ResolveFinalMappedPath(kv.first, pathMapping);
        if (resolvedPath.empty()) continue;
        auto& bucket = resolvedHints[resolvedPath];
        bucket.insert(bucket.end(), kv.second.begin(), kv.second.end());
    }
    return resolvedHints;
}

static bool FilenameLooksModelScoped(const std::string& filename,
                                     const std::vector<TextureReferenceHint>& hints) {
    std::string stem = ToLower(fs::path(filename).stem().string());
    if (stem.empty()) return false;
    for (const auto& hint : hints) {
        std::string modelToken = ToLower(SanitizeFilenameToken(hint.modelName));
        if (modelToken.empty()) continue;
        if (stem.rfind(modelToken + "_", 0) == 0) {
            return true;
        }
    }
    return false;
}

static std::vector<fs::path> GatherRenameCandidatePaths(const std::string& resolvedVirtualPath,
                                                        const fs::path& currentAbs,
                                                        const std::unordered_map<std::string, std::string>& pathMapping) {
    std::vector<fs::path> candidatePaths;
    if (!currentAbs.empty()) {
        candidatePaths.push_back(currentAbs);
    }

    for (const auto& mapEntry : pathMapping) {
        if (ResolveFinalMappedPath(mapEntry.first, pathMapping) != resolvedVirtualPath) {
            continue;
        }
        if (mapEntry.first == resolvedVirtualPath) {
            continue;
        }
        candidatePaths.emplace_back(NormalizeSlashes(mapEntry.first));
    }

    return candidatePaths;
}

static void NormalizeSharedTextureNames(
    const std::unordered_map<std::string, std::vector<TextureReferenceHint>>& hintsByPath,
    const fs::path& sharedDir,
    const fs::path& projectDir,
    std::unordered_map<std::string, std::string>& pathMapping,
    std::vector<std::string>& logLines,
    int& outMetaUpdates,
    const std::unordered_map<std::string, std::vector<fs::path>>* texturesByHash = nullptr,
    std::unordered_set<std::string>* outAffectedModelKeys = nullptr) {
    auto resolvedHints = ResolveHintsByFinalPath(hintsByPath, pathMapping);
    for (const auto& kv : resolvedHints) {
        const std::string& oldVirtual = kv.first;
        fs::path oldAbs = AbsoluteFromVirtualPath(oldVirtual, projectDir);
        if (oldAbs.empty() || !fs::exists(oldAbs)) continue;
        if (!IsUnderDirectory(oldAbs, sharedDir)) continue;

        bool currentIsGeneric = IsGenericTextureFilename(oldAbs.filename().string());
        bool currentIsWeak = IsWeakTextureFilename(oldAbs.filename().string());
        bool currentIsModelScoped = FilenameLooksModelScoped(oldAbs.filename().string(), kv.second);
        bool currentLooksHintDerived = FilenameLooksHintDerived(oldAbs.filename().string(), kv.second);
        if (outAffectedModelKeys && (currentIsWeak || currentIsGeneric || currentIsModelScoped || currentLooksHintDerived)) {
            for (const auto& hint : kv.second) {
                if (!hint.modelReferenceKey.empty()) {
                    outAffectedModelKeys->insert(hint.modelReferenceKey);
                }
            }
        }
        if (!currentIsWeak && !currentIsGeneric && !currentIsModelScoped && !currentLooksHintDerived) {
            continue;
        }

        std::string hash;
        try {
            hash = AssetPipeline::Instance().ComputeFileHash(oldAbs.string());
        } catch (...) {}

        std::vector<fs::path> candidatePaths = GatherRenameCandidatePaths(oldVirtual, oldAbs, pathMapping);
        std::string preferredFilename = ChoosePreferredSharedFilename(candidatePaths,
                                                                     kv.second,
                                                                     hash,
                                                                     texturesByHash,
                                                                     sharedDir,
                                                                     projectDir);
        if (preferredFilename.empty()) continue;

        fs::path newAbs;
        if (!SyncTextureToPreferredSharedPath(sharedDir,
                                              oldAbs,
                                              preferredFilename,
                                              hash,
                                              sharedDir,
                                              &logLines,
                                              nullptr,
                                              newAbs)) {
            logLines.push_back("Failed to normalize shared texture: " + oldAbs.string());
            continue;
        }
        std::string newVirtual = NormalizeAssetPath(newAbs.string(), projectDir);
        if (newVirtual.empty() || newVirtual == oldVirtual) continue;

        fs::path metaPath = newAbs;
        metaPath += ".meta";
        if (!WriteTextureMeta(metaPath, newVirtual, newAbs, outMetaUpdates)) {
            logLines.push_back("Failed to update shared texture meta: " + newAbs.string());
            continue;
        }

        AddPathMapping(pathMapping, oldVirtual, newVirtual);
    }
}

static std::vector<TextureReferenceHint> GatherTextureHintsForPaths(
    const std::vector<std::string>& paths,
    const std::unordered_map<std::string, std::vector<TextureReferenceHint>>& hintsByPath,
    const fs::path& projectDir) {
    std::vector<TextureReferenceHint> hints;
    for (const auto& path : paths) {
        std::string virtualPath = NormalizeAssetPath(path, projectDir);
        if (virtualPath.empty()) continue;
        auto it = hintsByPath.find(virtualPath);
        if (it == hintsByPath.end()) continue;
        hints.insert(hints.end(), it->second.begin(), it->second.end());
    }
    return hints;
}

static void RecoverMissingTexturePathMappingsFromHints(
    const std::unordered_map<std::string, std::vector<TextureReferenceHint>>& hintsByPath,
    const std::unordered_map<std::string, std::vector<fs::path>>& texturesByFilename,
    const fs::path& texturesDir,
    const fs::path& sharedDir,
    const fs::path& projectDir,
    std::unordered_map<std::string, std::string>& pathMapping,
    std::vector<std::string>* logLines = nullptr,
    int* outMetaUpdates = nullptr) {
    std::unordered_map<std::string, std::vector<fs::path>> canonicalTexturesByFilename = texturesByFilename;
    if (!projectDir.empty()) {
        AppendTextureFilenameIndex(projectDir / "art" / "textures", canonicalTexturesByFilename);
        AppendTextureFilenameIndex(projectDir.parent_path() / "art" / "textures", canonicalTexturesByFilename);
    }

    for (const auto& kv : hintsByPath) {
        const std::string& oldVirtual = kv.first;
        if (oldVirtual.empty()) continue;
        if (pathMapping.find(oldVirtual) != pathMapping.end()) continue;

        fs::path oldPath = fs::path(NormalizeSlashes(oldVirtual));
        if (!IsTextureExtension(oldPath)) continue;

        fs::path oldAbs = AbsoluteFromVirtualPath(oldVirtual, projectDir);
        if (!oldAbs.empty() && fs::exists(oldAbs)) continue;

        std::string preferredFilename = ChoosePreferredSharedFilename({ oldPath }, kv.second, {});
        if (preferredFilename.empty()) continue;

        fs::path preferredSharedAbs = sharedDir / preferredFilename;
        if (fs::exists(preferredSharedAbs)) {
            AddPathMapping(pathMapping, oldVirtual, NormalizeAssetPath(preferredSharedAbs.string(), projectDir));
            continue;
        }

        fs::path sameNameSharedAbs = sharedDir / oldPath.filename();
        if (fs::exists(sameNameSharedAbs)) {
            AddPathMapping(pathMapping, oldVirtual, NormalizeAssetPath(sameNameSharedAbs.string(), projectDir));
            continue;
        }

        std::string resolvedPreferredPath =
            ResolveTexturePathFromHints(preferredFilename,
                                        kv.second,
                                        oldVirtual,
                                        true,
                                        &canonicalTexturesByFilename,
                                        sharedDir,
                                        projectDir);
        if (!resolvedPreferredPath.empty()) {
            fs::path indexedPreferred = AbsoluteFromVirtualPath(resolvedPreferredPath, projectDir);
            if (!indexedPreferred.empty() && fs::exists(indexedPreferred)) {
                fs::path promotedSharedAbs;
                if (EnsureSharedTextureCopy(sharedDir,
                                            preferredFilename,
                                            indexedPreferred,
                                            logLines,
                                            promotedSharedAbs)) {
                    std::string promotedSharedVirtual = NormalizeAssetPath(promotedSharedAbs.string(), projectDir);
                    if (!promotedSharedVirtual.empty()) {
                        int ignoredMetaUpdates = 0;
                        int& metaUpdates = outMetaUpdates ? *outMetaUpdates : ignoredMetaUpdates;
                        fs::path metaPath = promotedSharedAbs;
                        metaPath += ".meta";
                        WriteTextureMeta(metaPath, promotedSharedVirtual, promotedSharedAbs, metaUpdates);
                        AddPathMapping(pathMapping, oldVirtual, promotedSharedVirtual);
                        continue;
                    }
                }
            }

            if (!indexedPreferred.empty() && IsUnderDirectory(indexedPreferred, texturesDir)) {
                AddPathMapping(pathMapping, oldVirtual, resolvedPreferredPath);
            }
        }
    }
}

static std::vector<fs::path> FindModelFiles(const fs::path& assetsDir) {
    std::vector<fs::path> result;
    if (assetsDir.empty() || !fs::exists(assetsDir)) return result;
    for (const auto& entry : fs::recursive_directory_iterator(assetsDir)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = ToLower(entry.path().extension().string());
        if (ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb") {
            result.push_back(entry.path());
        }
    }
    return result;
}

static std::vector<fs::path> FindMaterialAssetFiles(const fs::path& assetsDir) {
    std::vector<fs::path> result;
    if (assetsDir.empty() || !fs::exists(assetsDir)) return result;
    for (const auto& entry : fs::recursive_directory_iterator(assetsDir)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = ToLower(entry.path().extension().string());
        if (ext == ".mat" || ext == ".sgmat") {
            result.push_back(entry.path());
        }
    }
    return result;
}

static bool LooksLikeTexturePathCandidate(const std::string& value) {
    if (value.empty()) return false;
    std::string lower = ToLower(NormalizeSlashes(value));
    return lower.find(".png") != std::string::npos ||
           lower.find(".jpg") != std::string::npos ||
           lower.find(".jpeg") != std::string::npos ||
           lower.find(".tga") != std::string::npos;
}

static void CollectSharedTextureReferencesFromJsonNode(const json& node,
                                                       const fs::path& projectDir,
                                                       const fs::path& sharedDir,
                                                       std::unordered_set<std::string>& referenced) {
    if (node.is_string()) {
        std::string path = node.get<std::string>();
        if (!LooksLikeTexturePathCandidate(path)) return;

        std::string normalized = NormalizeAssetPath(path, projectDir);
        if (normalized.empty()) return;

        fs::path abs = AbsoluteFromVirtualPath(normalized, projectDir);
        if (abs.empty()) return;
        if (!IsTextureExtension(abs)) return;
        if (!IsUnderDirectory(abs, sharedDir)) return;

        referenced.insert(NormalizeAssetPath(abs.string(), projectDir));
        return;
    }

    if (node.is_array()) {
        for (const auto& item : node) {
            CollectSharedTextureReferencesFromJsonNode(item, projectDir, sharedDir, referenced);
        }
        return;
    }

    if (!node.is_object()) return;
    for (const auto& item : node.items()) {
        CollectSharedTextureReferencesFromJsonNode(item.value(), projectDir, sharedDir, referenced);
    }
}

static void CollectSharedTextureReferencesFromJsonFile(const fs::path& filePath,
                                                       const char* label,
                                                       const fs::path& projectDir,
                                                       const fs::path& sharedDir,
                                                       std::unordered_set<std::string>& referenced,
                                                       std::vector<std::string>* logLines = nullptr) {
    try {
        std::ifstream in(filePath, std::ios::binary);
        if (!in.is_open()) {
            if (logLines) {
                logLines->push_back(std::string("Failed to read ") + label + " while collecting shared refs: " + filePath.string());
            }
            return;
        }

        json root;
        in >> root;
        CollectSharedTextureReferencesFromJsonNode(root, projectDir, sharedDir, referenced);
    } catch (...) {
        if (logLines) {
            logLines->push_back(std::string("Failed to parse ") + label + " while collecting shared refs: " + filePath.string());
        }
    }
}

static std::unordered_set<std::string> CollectReferencedSharedTexturePaths(const fs::path& projectDir,
                                                                           const fs::path& sharedDir,
                                                                           std::vector<std::string>* logLines = nullptr) {
    std::unordered_set<std::string> referenced;

    fs::path assetsDir = Project::GetAssetDirectory();
    if (assetsDir.empty()) assetsDir = projectDir / "assets";

    for (const auto& modelPath : FindModelFiles(assetsDir)) {
        std::string metaPath;
        if (!ResolveModelMetaPath(modelPath, metaPath)) {
            continue;
        }

        try {
            std::ifstream in(metaPath);
            if (!in.is_open()) continue;

            json meta;
            in >> meta;
            CollectSharedTextureReferencesFromJsonNode(meta, projectDir, sharedDir, referenced);
        } catch (...) {
            if (logLines) {
                logLines->push_back("Failed to read model meta while collecting shared refs: " + metaPath);
            }
        }
    }

    for (const auto& scenePath : FindSceneFiles(projectDir)) {
        CollectSharedTextureReferencesFromJsonFile(scenePath,
                                                   "scene",
                                                   projectDir,
                                                   sharedDir,
                                                   referenced,
                                                   logLines);
    }

    for (const auto& prefabPath : FindPrefabFiles(projectDir)) {
        CollectSharedTextureReferencesFromJsonFile(prefabPath,
                                                   "prefab",
                                                   projectDir,
                                                   sharedDir,
                                                   referenced,
                                                   logLines);
    }

    for (const auto& materialPath : FindMaterialAssetFiles(assetsDir)) {
        CollectSharedTextureReferencesFromJsonFile(materialPath,
                                                   "material asset",
                                                   projectDir,
                                                   sharedDir,
                                                   referenced,
                                                   logLines);
    }

    return referenced;
}

static void RemoveUnusedSharedTextures(const fs::path& sharedDir,
                                       const fs::path& projectDir,
                                       int& outRemoved,
                                       int& outMetaUpdates,
                                       std::vector<std::string>& logLines) {
    if (sharedDir.empty() || !fs::exists(sharedDir)) {
        return;
    }

    std::unordered_set<std::string> referenced = CollectReferencedSharedTexturePaths(projectDir, sharedDir, &logLines);
    for (const auto& entry : fs::directory_iterator(sharedDir)) {
        if (!entry.is_regular_file()) continue;

        fs::path abs = entry.path();
        if (abs.extension() == ".meta") {
            fs::path withoutMeta = abs;
            withoutMeta.replace_extension();
            if (!fs::exists(withoutMeta)) {
                std::error_code ec;
                fs::remove(abs, ec);
                if (!ec) {
                    outMetaUpdates += 1;
                }
            }
            continue;
        }

        if (!IsTextureExtension(abs)) continue;

        std::string virtualPath = NormalizeAssetPath(abs.string(), projectDir);
        if (virtualPath.empty()) continue;
        if (referenced.find(virtualPath) != referenced.end()) continue;

        RemoveTextureFileAndMeta(abs, virtualPath, outRemoved, outMetaUpdates);
    }
}

static int ApplyTextureMappingToAllModelMetas(const std::unordered_map<std::string, std::string>& mapping,
                                              const fs::path& projectDir,
                                              std::vector<std::string>& logLines,
                                              std::unordered_set<std::string>* outUpdatedModelKeys = nullptr) {
    fs::path assetsDir = Project::GetAssetDirectory();
    if (assetsDir.empty()) assetsDir = projectDir / "assets";
    fs::path texturesDir = assetsDir / "textures";
    fs::path sharedDir = texturesDir / "shared";
    std::unordered_map<std::string, std::vector<fs::path>> texturesByFilename = BuildTextureFilenameIndex(texturesDir, projectDir);

    int updatedModels = 0;
    for (const auto& modelPath : FindModelFiles(assetsDir)) {
        std::string metaPath;
        if (!ResolveModelMetaPath(modelPath, metaPath)) {
            continue;
        }

        try {
            std::ifstream in(metaPath);
            if (!in.is_open()) continue;

            json meta;
            in >> meta;
            in.close();

            std::unordered_map<std::string, std::vector<SourceTextureFilenameHint>> sourceFilenameHints =
                BuildSourceTextureFilenameHints(modelPath);
            if (!ApplyTextureMappingToMeta(meta,
                                           mapping,
                                           projectDir,
                                           &sourceFilenameHints,
                                           &texturesByFilename,
                                           sharedDir)) {
                continue;
            }

            std::ofstream out(metaPath, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                logLines.push_back("Failed to write meta: " + metaPath);
                continue;
            }
            out << meta.dump(4);
            updatedModels += 1;
            if (outUpdatedModelKeys) {
                MarkAffectedModelReference(metaPath, projectDir, *outUpdatedModelKeys);
            }
        } catch (...) {
            logLines.push_back("Failed to update model meta: " + metaPath);
        }
    }

    return updatedModels;
}

static std::vector<fs::path> FindSceneFiles(const fs::path& projectDir) {
    std::vector<fs::path> result;
    fs::path scenesDir = projectDir / "scenes";
    if (!fs::exists(scenesDir)) {
        return result;
    }

    for (const auto& entry : fs::recursive_directory_iterator(scenesDir)) {
        if (!entry.is_regular_file()) continue;
        if (ToLower(entry.path().extension().string()) == ".scene") {
            result.push_back(entry.path());
        }
    }
    return result;
}

static std::vector<fs::path> FindPrefabFiles(const fs::path& projectDir) {
    std::vector<fs::path> result;
    fs::path assetsDir = Project::GetAssetDirectory();
    if (assetsDir.empty()) assetsDir = projectDir / "assets";
    if (!fs::exists(assetsDir)) {
        return result;
    }

    for (const auto& entry : fs::recursive_directory_iterator(assetsDir)) {
        if (!entry.is_regular_file()) continue;
        if (ToLower(entry.path().extension().string()) == ".prefab") {
            result.push_back(entry.path());
        }
    }
    return result;
}

static bool RemapJsonTextureField(json& object,
                                  const char* key,
                                  const std::unordered_map<std::string, std::string>& mapping,
                                  const fs::path& projectDir,
                                  int& remapCount) {
    if (!object.contains(key) || !object[key].is_string()) return false;
    std::string path = object[key].get<std::string>();
    if (!RemapTexturePath(path, mapping, projectDir)) return false;
    object[key] = path;
    remapCount += 1;
    return true;
}

static bool ClearJsonTextureField(json& object,
                                  const char* key,
                                  int& resetCount) {
    if (!object.contains(key)) return false;
    bool hadValue = object[key].is_string() && !object[key].get<std::string>().empty();
    object.erase(key);
    if (hadValue) {
        resetCount += 1;
    }
    return true;
}

static bool IsStaleSerializedTexturePath(const std::string& path,
                                         const std::unordered_map<std::string, std::string>& mapping,
                                         const fs::path& projectDir) {
    if (path.empty()) return false;

    std::string normalized = NormalizeAssetPath(path, projectDir);
    std::string resolved = normalized.empty() ? NormalizeSlashes(path)
                                              : ResolveFinalMappedPath(normalized, mapping);
    if (resolved.empty()) {
        resolved = normalized.empty() ? NormalizeSlashes(path) : normalized;
    }

    fs::path abs = AbsoluteFromVirtualPath(resolved, projectDir);
    return abs.empty() || !fs::exists(abs);
}

static bool MeshHasStaleTextureOverrides(const json& meshJson,
                                         const std::unordered_map<std::string, std::string>& mapping,
                                         const fs::path& projectDir) {
    if (meshJson.contains("slotPropertyBlocks") && meshJson["slotPropertyBlocks"].is_array()) {
        for (const auto& block : meshJson["slotPropertyBlocks"]) {
            if (!block.is_object() || !block.contains("textures") || !block["textures"].is_object()) continue;
            for (const auto& [sampler, value] : block["textures"].items()) {
                if (!value.is_string()) continue;
                if (IsStaleSerializedTexturePath(value.get<std::string>(), mapping, projectDir)) {
                    return true;
                }
            }
        }
    }

    static const char* kLegacyMeshTextureKeys[] = {
        "mat_albedoPath",
        "mat_mrPath",
        "mat_normalPath"
    };
    for (const char* key : kLegacyMeshTextureKeys) {
        if (meshJson.contains(key) && meshJson[key].is_string() &&
            IsStaleSerializedTexturePath(meshJson[key].get<std::string>(), mapping, projectDir)) {
            return true;
        }
    }

    if (meshJson.contains("slotMaterials") && meshJson["slotMaterials"].is_array()) {
        static const char* kSlotTextureKeys[] = {
            "albedoPath",
            "mrPath",
            "normalPath",
            "aoPath",
            "emissionPath",
            "displacementPath"
        };

        for (const auto& slot : meshJson["slotMaterials"]) {
            if (!slot.is_object()) continue;
            for (const char* key : kSlotTextureKeys) {
                if (slot.contains(key) && slot[key].is_string() &&
                    IsStaleSerializedTexturePath(slot[key].get<std::string>(), mapping, projectDir)) {
                    return true;
                }
            }
        }
    }

    return false;
}

static bool RepairMeshSceneOverrides(json& meshJson,
                                     bool resetToDefaults,
                                     const std::unordered_map<std::string, std::string>& mapping,
                                     const fs::path& projectDir,
                                     FileRepairStats& stats) {
    bool changed = false;

    if (meshJson.contains("slotPropertyBlocks") && meshJson["slotPropertyBlocks"].is_array()) {
        for (auto& block : meshJson["slotPropertyBlocks"]) {
            if (!block.is_object() || !block.contains("textures") || !block["textures"].is_object()) continue;
            if (resetToDefaults) {
                stats.materialOverridesReset += static_cast<int>(block["textures"].size());
                block.erase("textures");
                changed = true;
                continue;
            }

            for (auto& [sampler, value] : block["textures"].items()) {
                if (!value.is_string()) continue;
                std::string path = value.get<std::string>();
                if (!RemapTexturePath(path, mapping, projectDir)) continue;
                value = path;
                stats.texturePathRemaps += 1;
                changed = true;
            }
        }
    }

    static const char* kLegacyMeshTextureKeys[] = {
        "mat_albedoPath",
        "mat_mrPath",
        "mat_normalPath"
    };
    for (const char* key : kLegacyMeshTextureKeys) {
        if (resetToDefaults) {
            changed |= ClearJsonTextureField(meshJson, key, stats.materialOverridesReset);
        } else {
            changed |= RemapJsonTextureField(meshJson, key, mapping, projectDir, stats.texturePathRemaps);
        }
    }

    if (meshJson.contains("slotMaterials") && meshJson["slotMaterials"].is_array()) {
        static const char* kSlotTextureKeys[] = {
            "albedoPath",
            "mrPath",
            "normalPath",
            "aoPath",
            "emissionPath",
            "displacementPath"
        };

        for (auto& slot : meshJson["slotMaterials"]) {
            if (!slot.is_object()) continue;
            for (const char* key : kSlotTextureKeys) {
                if (resetToDefaults) {
                    changed |= ClearJsonTextureField(slot, key, stats.materialOverridesReset);
                } else {
                    changed |= RemapJsonTextureField(slot, key, mapping, projectDir, stats.texturePathRemaps);
                }
            }
        }
    }

    return changed;
}

static bool RepairEntityRecursive(json& entityJson,
                                  const std::unordered_set<std::string>& affectedModelKeys,
                                  const std::unordered_map<std::string, std::string>& mapping,
                                  const fs::path& projectDir,
                                  FileRepairStats& stats,
                                  const std::string& inheritedModelPath = {}) {
    if (!entityJson.is_object()) return false;

    bool changed = false;
    std::string modelPathContext = inheritedModelPath;
    if (entityJson.contains("asset") && entityJson["asset"].is_object()) {
        const json& asset = entityJson["asset"];
        if (asset.value("type", std::string()) == "model" &&
            asset.contains("path") &&
            asset["path"].is_string()) {
            modelPathContext = NormalizeAssetPath(asset["path"].get<std::string>(), projectDir);
        }
    }

    if (entityJson.contains("mesh") && entityJson["mesh"].is_object()) {
        json& meshJson = entityJson["mesh"];
        std::string meshModelPath = meshJson.value("meshPath", std::string());
        meshModelPath = NormalizeAssetPath(meshModelPath, projectDir);
        if (meshModelPath.empty()) {
            meshModelPath = modelPathContext;
        }

        std::string meshModelKey = BuildModelReferenceKey(meshModelPath, projectDir);
        bool hasStaleOverrides = !meshModelKey.empty() &&
                                 MeshHasStaleTextureOverrides(meshJson, mapping, projectDir);
        bool resetToDefaults = !meshModelKey.empty() &&
                               (affectedModelKeys.find(meshModelKey) != affectedModelKeys.end() ||
                                hasStaleOverrides);
        changed |= RepairMeshSceneOverrides(meshJson, resetToDefaults, mapping, projectDir, stats);
    }

    if (entityJson.contains("children") && entityJson["children"].is_array()) {
        for (auto& child : entityJson["children"]) {
            changed |= RepairEntityRecursive(child,
                                             affectedModelKeys,
                                             mapping,
                                             projectDir,
                                             stats,
                                             modelPathContext);
        }
    }

    return changed;
}

static void ApplyRepairToEntityFiles(const std::vector<fs::path>& filePaths,
                                     const char* label,
                                     const std::unordered_set<std::string>& affectedModelKeys,
                                     const std::unordered_map<std::string, std::string>& mapping,
                                     const fs::path& projectDir,
                                     std::vector<std::string>& logLines,
                                     FileRepairStats& outStats) {
    outStats = FileRepairStats{};
    if (mapping.empty() && affectedModelKeys.empty()) return;

    for (const auto& filePath : filePaths) {
        outStats.filesScanned += 1;
        try {
            std::ifstream in(filePath, std::ios::binary);
            if (!in.is_open()) {
                logLines.push_back(std::string("Failed to read ") + label + ": " + filePath.string());
                continue;
            }

            json rootJson;
            in >> rootJson;
            in.close();

            bool changed = false;
            if (rootJson.contains("entities") && rootJson["entities"].is_array()) {
                for (auto& entity : rootJson["entities"]) {
                    changed |= RepairEntityRecursive(entity,
                                                     affectedModelKeys,
                                                     mapping,
                                                     projectDir,
                                                     outStats);
                }
            }

            if (!changed) continue;

            std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                logLines.push_back(std::string("Failed to write ") + label + ": " + filePath.string());
                continue;
            }

            out << rootJson.dump(4);
            outStats.filesUpdated += 1;
        } catch (...) {
            logLines.push_back(std::string("Failed to repair ") + label + ": " + filePath.string());
        }
    }
}

static void ApplySceneAndPrefabRepairToProject(const std::unordered_map<std::string, std::string>& mapping,
                                               const std::unordered_set<std::string>& affectedModelKeys,
                                               const fs::path& projectDir,
                                               std::vector<std::string>& logLines,
                                               ProjectRepairStats& outStats) {
    outStats = ProjectRepairStats{};
    if (mapping.empty() && affectedModelKeys.empty()) return;

    ApplyRepairToEntityFiles(FindSceneFiles(projectDir),
                             "scene",
                             affectedModelKeys,
                             mapping,
                             projectDir,
                             logLines,
                             outStats.scenes);
    ApplyRepairToEntityFiles(FindPrefabFiles(projectDir),
                             "prefab",
                             affectedModelKeys,
                             mapping,
                             projectDir,
                             logLines,
                             outStats.prefabs);
}

} // namespace

bool CleanupSharedTextures(CleanupReport& out) {
    out = CleanupReport{};

    fs::path projectDir = Project::GetProjectDirectory();
    if (projectDir.empty()) {
        out.logLines.push_back("No project directory loaded.");
        return false;
    }
    fs::path assetsDir = Project::GetAssetDirectory();
    if (assetsDir.empty()) assetsDir = projectDir / "assets";
    fs::path texturesDir = assetsDir / "textures";
    fs::path sharedDir = texturesDir / "shared";
    if (!fs::exists(texturesDir)) {
        out.logLines.push_back("No assets/textures directory found.");
        return false;
    }
    std::error_code ec;
    fs::create_directories(sharedDir, ec);

    std::vector<fs::path> modelFiles = FindModelFiles(assetsDir);
    out.modelsScanned = static_cast<int>(modelFiles.size());

    std::vector<ModelTextureInfo> models;
    models.reserve(modelFiles.size());
    std::unordered_map<std::string, std::vector<TextureReferenceHint>> hintsByPath;
    for (const auto& modelPath : modelFiles) {
        std::string metaPath;
        if (!ResolveModelMetaPath(modelPath, metaPath)) {
            out.logLines.push_back("Missing model meta: " + modelPath.string());
            continue;
        }
        json meta;
        try {
            std::ifstream in(metaPath);
            if (!in.is_open()) continue;
            in >> meta;
        } catch (...) {
            out.logLines.push_back("Failed to read meta: " + metaPath);
            continue;
        }

        ModelTextureInfo info;
        info.modelPath = modelPath.string();
        info.metaPath = metaPath;
        info.modelName = modelPath.stem().string();
        info.textureDir = texturesDir / info.modelName;
        info.metaJson = std::move(meta);
        CollectTexturePathsFromMeta(info.metaJson, projectDir, info.texturePaths);
        info.sourceFilenameHints = BuildSourceTextureFilenameHints(modelPath);
        CollectTextureReferenceHintsFromMeta(info.metaJson,
                                             info.modelName,
                                             info.metaPath,
                                             projectDir,
                                             &info.sourceFilenameHints,
                                             hintsByPath);
        std::sort(info.texturePaths.begin(), info.texturePaths.end());
        info.texturePaths.erase(std::unique(info.texturePaths.begin(), info.texturePaths.end()), info.texturePaths.end());
        models.push_back(std::move(info));
    }

    std::unordered_map<std::string, TextureFileInfo> texturesByAbs;
    for (const auto& model : models) {
        for (const auto& vpath : model.texturePaths) {
            if (vpath.rfind("assets/", 0) != 0) continue;
            if (!IsTextureExtension(fs::path(NormalizeSlashes(vpath)))) continue;
            fs::path abs = AbsoluteFromVirtualPath(vpath, projectDir);
            if (abs.empty() || !fs::exists(abs)) {
                out.logLines.push_back("Missing texture file: " + vpath);
                continue;
            }
            std::string absKey = NormalizeForCompare(abs);
            auto& info = texturesByAbs[absKey];
            info.absPath = abs.string();
            info.virtualPath = vpath;
            info.models.insert(model.modelPath);
        }
    }

    out.texturesReferenced = static_cast<int>(texturesByAbs.size());
    std::unordered_map<std::string, std::vector<fs::path>> texturesByFilename = BuildTextureFilenameIndex(texturesDir, projectDir);
    std::unordered_map<std::string, std::vector<fs::path>> canonicalTexturesByHash =
        BuildTextureHashIndex(CollectCanonicalTextureRoots(texturesDir, projectDir));
    for (auto& kv : texturesByAbs) {
        try {
            kv.second.hash = AssetPipeline::Instance().ComputeFileHash(kv.second.absPath);
        } catch (...) {
            kv.second.hash.clear();
        }
    }

    std::unordered_map<std::string, std::vector<std::string>> hashToAbsPaths;
    std::unordered_map<std::string, std::unordered_set<std::string>> hashToModels;
    for (const auto& kv : texturesByAbs) {
        const auto& info = kv.second;
        if (info.hash.empty()) continue;
        hashToAbsPaths[info.hash].push_back(info.absPath);
        auto& modelSet = hashToModels[info.hash];
        modelSet.insert(info.models.begin(), info.models.end());
    }

    std::unordered_map<std::string, std::string> pathMapping;
    std::unordered_map<std::string, fs::path> hashToSharedAbs;
    std::unordered_map<std::string, std::string> hashToSharedVirtual;

    for (const auto& kv : hashToAbsPaths) {
        const std::string& hash = kv.first;
        const auto& paths = kv.second;
        auto modelIt = hashToModels.find(hash);
        size_t modelCount = modelIt != hashToModels.end() ? modelIt->second.size() : 0;
        if (paths.size() <= 1 && modelCount <= 1) continue;

        out.duplicateGroups += 1;
        fs::path destAbs;
        std::string destVirtual;
        for (const auto& p : paths) {
            fs::path abs(p);
            if (IsUnderDirectory(abs, sharedDir)) {
                destAbs = abs;
                destVirtual = NormalizeAssetPath(abs.string(), projectDir);
                break;
            }
        }
        if (destAbs.empty()) {
            fs::path src = fs::path(paths.front());
            std::vector<TextureReferenceHint> groupHints = GatherTextureHintsForPaths(paths, hintsByPath, projectDir);
            std::vector<fs::path> candidatePaths;
            candidatePaths.reserve(paths.size());
            for (const auto& candidate : paths) {
                candidatePaths.emplace_back(candidate);
            }
            std::string preferredFilename = ChoosePreferredSharedFilename(
                candidatePaths,
                groupHints,
                hash,
                &canonicalTexturesByHash,
                sharedDir,
                projectDir);
            if (!SyncTextureToPreferredSharedPath(sharedDir,
                                                  src,
                                                  preferredFilename,
                                                  hash,
                                                  fs::path{},
                                                  &out.logLines,
                                                  &out.texturesMovedToShared,
                                                  destAbs)) {
                out.logLines.push_back("Failed to materialize preferred shared texture: " + src.string());
                continue;
            }
            destVirtual = NormalizeAssetPath(destAbs.string(), projectDir);
            if (destVirtual.empty()) continue;
        }

        fs::path metaPath = destAbs; metaPath += ".meta";
        WriteTextureMeta(metaPath, destVirtual, destAbs, out.textureMetaUpdated);
        hashToSharedAbs[hash] = destAbs;
        hashToSharedVirtual[hash] = destVirtual;

        for (const auto& p : paths) {
            std::string oldVirtual = NormalizeAssetPath(p, projectDir);
            if (!oldVirtual.empty() && oldVirtual != destVirtual) {
                AddPathMapping(pathMapping, oldVirtual, destVirtual);
            }
        }
    }

    // --------- Name-based dedupe (identical filenames) ---------
    std::unordered_map<std::string, std::vector<std::string>> nameToPaths;
    for (const auto& kv : texturesByAbs) {
        fs::path abs = kv.second.absPath;
        if (abs.empty()) continue;
        std::string filename = ToLower(abs.filename().string());
        if (filename.empty()) continue;
        nameToPaths[filename].push_back(abs.string());
    }

    for (const auto& kv : nameToPaths) {
        const std::string& filenameLower = kv.first;
        const auto& paths = kv.second;
        if (paths.size() <= 1) continue;

        fs::path sharedCandidate = sharedDir / fs::path(paths.front()).filename();
        fs::file_time_type bestTime = fs::file_time_type::min();
        fs::path bestPath;

        // Consider referenced paths
        for (const auto& p : paths) {
            fs::path abs(p);
            fs::file_time_type t = SafeLastWriteTime(abs);
            if (t > bestTime) {
                bestTime = t;
                bestPath = abs;
            }
        }

        // Consider existing shared file with same name
        if (fs::exists(sharedCandidate)) {
            fs::file_time_type t = SafeLastWriteTime(sharedCandidate);
            if (t > bestTime) {
                bestTime = t;
                bestPath = sharedCandidate;
            }
        }

        if (bestPath.empty()) continue;

        fs::path sharedAbs;
        std::string sharedName = bestPath.filename().string();
        if (!EnsureSharedTextureByName(sharedDir, sharedName, bestPath, out, sharedAbs)) {
            continue;
        }

        std::string sharedVirtual = NormalizeAssetPath(sharedAbs.string(), projectDir);
        fs::path metaPath = sharedAbs; metaPath += ".meta";
        WriteTextureMeta(metaPath, sharedVirtual, sharedAbs, out.textureMetaUpdated);

        out.duplicateGroups += 1;
        for (const auto& p : paths) {
            std::string oldVirtual = NormalizeAssetPath(p, projectDir);
            if (!oldVirtual.empty() && oldVirtual != sharedVirtual) {
                AddPathMapping(pathMapping, oldVirtual, sharedVirtual);
            }
        }
    }

    RecoverMissingTexturePathMappingsFromHints(hintsByPath,
                                               texturesByFilename,
                                               texturesDir,
                                               sharedDir,
                                               projectDir,
                                               pathMapping,
                                               &out.logLines,
                                               &out.textureMetaUpdated);

    std::unordered_set<std::string> affectedModelKeys;
    NormalizeSharedTextureNames(hintsByPath,
                                sharedDir,
                                projectDir,
                                pathMapping,
                                out.logLines,
                                out.textureMetaUpdated,
                                &canonicalTexturesByHash,
                                &affectedModelKeys);

    std::unordered_map<std::string, std::string> finalPathMapping = CollapsePathMapping(pathMapping);
    std::unordered_map<std::string, std::vector<fs::path>> finalTexturesByFilename = BuildTextureFilenameIndex(texturesDir, projectDir);

    for (auto& model : models) {
        bool changed = ApplyTextureMappingToMeta(model.metaJson,
                                                 finalPathMapping,
                                                 projectDir,
                                                 &model.sourceFilenameHints,
                                                 &finalTexturesByFilename,
                                                 sharedDir);
        if (!changed) continue;
        try {
            std::ofstream outFile(model.metaPath, std::ios::binary | std::ios::trunc);
            if (outFile.is_open()) {
                outFile << model.metaJson.dump(4);
                out.modelsUpdated += 1;
                MarkAffectedModelReference(model.metaPath, projectDir, affectedModelKeys);
            }
        } catch (...) {
            out.logLines.push_back("Failed to write meta: " + model.metaPath);
        }
    }

    if (!finalPathMapping.empty()) {
        std::unordered_set<std::string> destSet;
        destSet.reserve(finalPathMapping.size());
        for (const auto& kv : finalPathMapping) {
            destSet.insert(kv.second);
        }
        for (const auto& kv : finalPathMapping) {
            const std::string& oldVirtual = kv.first;
            const std::string& destVirtual = kv.second;
            if (destSet.find(oldVirtual) != destSet.end()) continue;
            if (oldVirtual == destVirtual) continue;
            fs::path abs = AbsoluteFromVirtualPath(oldVirtual, projectDir);
            if (!abs.empty()) {
                RemoveTextureFileAndMeta(abs, oldVirtual, out.texturesRemoved, out.textureMetaUpdated);
            }
        }
    }

    out.modelsUpdated += ApplyTextureMappingToAllModelMetas(finalPathMapping,
                                                            projectDir,
                                                            out.logLines,
                                                            &affectedModelKeys);

    for (const auto& model : models) {
        if (model.textureDir.empty() || !fs::exists(model.textureDir)) continue;
        std::error_code ec2;
        if (fs::is_empty(model.textureDir, ec2) && !ec2) {
            fs::remove(model.textureDir, ec2);
            if (!ec2) out.emptyFoldersRemoved += 1;
        }
    }

    ProjectRepairStats repairStats;
    ApplySceneAndPrefabRepairToProject(finalPathMapping,
                                       affectedModelKeys,
                                       projectDir,
                                       out.logLines,
                                       repairStats);
    out.scenesScanned = repairStats.scenes.filesScanned;
    out.scenesUpdated = repairStats.scenes.filesUpdated;
    out.sceneTexturePathRemaps = repairStats.scenes.texturePathRemaps;
    out.sceneMaterialOverridesReset = repairStats.scenes.materialOverridesReset;
    out.prefabsScanned = repairStats.prefabs.filesScanned;
    out.prefabsUpdated = repairStats.prefabs.filesUpdated;
    out.prefabTexturePathRemaps = repairStats.prefabs.texturePathRemaps;
    out.prefabMaterialOverridesReset = repairStats.prefabs.materialOverridesReset;

    RemoveUnusedSharedTextures(sharedDir,
                               projectDir,
                               out.texturesRemoved,
                               out.textureMetaUpdated,
                               out.logLines);

    return true;
}

bool DeduplicateImportedModelTextures(const std::string& modelPath, ImportDedupReport* outReport) {
    ImportDedupReport localReport;
    ImportDedupReport& report = outReport ? *outReport : localReport;
    report = ImportDedupReport{};

    fs::path projectDir = Project::GetProjectDirectory();
    if (projectDir.empty()) {
        report.logLines.push_back("No project directory loaded.");
        return false;
    }
    fs::path assetsDir = Project::GetAssetDirectory();
    if (assetsDir.empty()) assetsDir = projectDir / "assets";
    fs::path texturesDir = assetsDir / "textures";
    fs::path sharedDir = texturesDir / "shared";
    if (!fs::exists(texturesDir)) return false;
    std::error_code ec;
    fs::create_directories(sharedDir, ec);

    fs::path modelPathFs(modelPath);
    if (!fs::exists(modelPathFs)) return false;
    std::string modelName = modelPathFs.stem().string();
    fs::path modelTexDir = texturesDir / modelName;
    bool modelTexDirExists = fs::exists(modelTexDir);

    std::unordered_map<std::string, std::vector<TextureReferenceHint>> hintsByPath;
    std::string currentMetaPath;
    if (ResolveModelMetaPath(modelPathFs, currentMetaPath)) {
        try {
            std::ifstream in(currentMetaPath);
            if (in.is_open()) {
                json currentMeta;
                in >> currentMeta;
                std::unordered_map<std::string, std::vector<SourceTextureFilenameHint>> sourceFilenameHints =
                    BuildSourceTextureFilenameHints(modelPathFs);
                CollectTextureReferenceHintsFromMeta(currentMeta,
                                                     modelName,
                                                     currentMetaPath,
                                                     projectDir,
                                                     &sourceFilenameHints,
                                                     hintsByPath);
            }
        } catch (...) {
            report.logLines.push_back("Failed to read model meta: " + currentMetaPath);
        }
    }

    // Build index of existing textures outside this model's texture folder
    std::unordered_map<std::string, fs::path> hashToExisting;
    std::unordered_map<std::string, fs::path> hashToShared;
    std::unordered_map<std::string, std::vector<fs::path>> nameToExisting;
    for (const auto& entry : fs::recursive_directory_iterator(texturesDir)) {
        if (!entry.is_regular_file()) continue;
        if (!IsTextureExtension(entry.path())) continue;
        fs::path abs = entry.path();
        if (IsUnderDirectory(abs, modelTexDir)) continue;
        std::string filenameLower = ToLower(abs.filename().string());
        if (!filenameLower.empty()) {
            nameToExisting[filenameLower].push_back(abs);
        }
        std::string hash = AssetPipeline::Instance().ComputeFileHash(abs.string());
        if (hash.empty()) continue;
        if (IsUnderDirectory(abs, sharedDir)) {
            hashToShared[hash] = abs;
        } else if (hashToExisting.find(hash) == hashToExisting.end()) {
            hashToExisting[hash] = abs;
        }
    }

    std::unordered_map<std::string, std::string> pathMapping;
    std::unordered_map<std::string, std::vector<fs::path>> texturesByFilename = BuildTextureFilenameIndex(texturesDir, projectDir);
    std::unordered_map<std::string, std::vector<fs::path>> canonicalTexturesByHash =
        BuildTextureHashIndex(CollectCanonicalTextureRoots(texturesDir, projectDir));

    // --------- Name-based dedupe (identical filenames) ---------
    if (modelTexDirExists) {
        std::vector<fs::path> modelFiles;
        for (const auto& entry : fs::recursive_directory_iterator(modelTexDir)) {
            if (!entry.is_regular_file()) continue;
            if (!IsTextureExtension(entry.path())) continue;
            modelFiles.push_back(entry.path());
        }

        for (const auto& abs : modelFiles) {
            report.texturesChecked += 1;
            std::string filenameLower = ToLower(abs.filename().string());
            auto nameIt = nameToExisting.find(filenameLower);
            if (nameIt == nameToExisting.end()) {
                continue;
            }

            fs::file_time_type bestTime = SafeLastWriteTime(abs);
            fs::path bestPath = abs;
            for (const auto& candidate : nameIt->second) {
                fs::file_time_type t = SafeLastWriteTime(candidate);
                if (t > bestTime) {
                    bestTime = t;
                    bestPath = candidate;
                }
            }

            fs::path sharedAbs;
            std::string filename = abs.filename().string();
            if (!EnsureSharedTextureByNameImport(sharedDir, modelTexDir, filename, bestPath, report, sharedAbs)) {
                continue;
            }

            std::string sharedVirtual = NormalizeAssetPath(sharedAbs.string(), projectDir);
            fs::path metaPath = sharedAbs; metaPath += ".meta";
            WriteTextureMeta(metaPath, sharedVirtual, sharedAbs, report.metaUpdates);

            std::string oldVirtual = NormalizeAssetPath(abs.string(), projectDir);
            if (!oldVirtual.empty() && oldVirtual != sharedVirtual) {
                AddPathMapping(pathMapping, oldVirtual, sharedVirtual);
                RemoveTextureFileAndMeta(abs, oldVirtual, report.texturesRemoved, report.metaUpdates);
            }
        }

        // --------- Hash-based dedupe (content identical) ---------
        for (const auto& entry : fs::recursive_directory_iterator(modelTexDir)) {
            if (!entry.is_regular_file()) continue;
            if (!IsTextureExtension(entry.path())) continue;
            fs::path abs = entry.path();
            report.texturesChecked += 1;
            std::string hash = AssetPipeline::Instance().ComputeFileHash(abs.string());
            if (hash.empty()) continue;

            fs::path sharedAbs;
            auto sharedIt = hashToShared.find(hash);
            if (sharedIt != hashToShared.end()) {
                sharedAbs = sharedIt->second;
            } else {
                std::vector<TextureReferenceHint> textureHints =
                    GatherTextureHintsForPaths({ abs.string() }, hintsByPath, projectDir);
                std::string preferredFilename = ChoosePreferredSharedFilename({ abs },
                                                                              textureHints,
                                                                              hash,
                                                                              &canonicalTexturesByHash,
                                                                              sharedDir,
                                                                              projectDir);
                if (!SyncTextureToPreferredSharedPath(sharedDir,
                                                      abs,
                                                      preferredFilename,
                                                      hash,
                                                      modelTexDir,
                                                      &report.logLines,
                                                      &report.texturesShared,
                                                      sharedAbs)) {
                    report.logLines.push_back("Failed to materialize preferred import texture: " + abs.string());
                    continue;
                }
                fs::path metaPath = sharedAbs; metaPath += ".meta";
                std::string sharedVirtual = NormalizeAssetPath(sharedAbs.string(), projectDir);
                WriteTextureMeta(metaPath, sharedVirtual, sharedAbs, report.metaUpdates);
                hashToShared[hash] = sharedAbs;
            }

            std::string sharedVirtual = NormalizeAssetPath(sharedAbs.string(), projectDir);
            std::string oldVirtual = NormalizeAssetPath(abs.string(), projectDir);
            if (!oldVirtual.empty() && oldVirtual != sharedVirtual) {
                AddPathMapping(pathMapping, oldVirtual, sharedVirtual);
                RemoveTextureFileAndMeta(abs, oldVirtual, report.texturesRemoved, report.metaUpdates);
            }
        }
    }

    RecoverMissingTexturePathMappingsFromHints(hintsByPath,
                                               texturesByFilename,
                                               texturesDir,
                                               sharedDir,
                                               projectDir,
                                               pathMapping,
                                               &report.logLines,
                                               &report.metaUpdates);

    std::unordered_set<std::string> affectedModelKeys;
    NormalizeSharedTextureNames(hintsByPath,
                                sharedDir,
                                projectDir,
                                pathMapping,
                                report.logLines,
                                report.metaUpdates,
                                &canonicalTexturesByHash,
                                &affectedModelKeys);

    std::unordered_map<std::string, std::string> finalPathMapping = CollapsePathMapping(pathMapping);
    report.metaUpdates += ApplyTextureMappingToAllModelMetas(finalPathMapping,
                                                             projectDir,
                                                             report.logLines,
                                                             &affectedModelKeys);
    MarkAffectedModelReference(currentMetaPath, projectDir, affectedModelKeys);

    ProjectRepairStats repairStats;
    ApplySceneAndPrefabRepairToProject(finalPathMapping,
                                       affectedModelKeys,
                                       projectDir,
                                       report.logLines,
                                       repairStats);
    report.scenesScanned = repairStats.scenes.filesScanned;
    report.scenesUpdated = repairStats.scenes.filesUpdated;
    report.sceneTexturePathRemaps = repairStats.scenes.texturePathRemaps;
    report.sceneMaterialOverridesReset = repairStats.scenes.materialOverridesReset;
    report.prefabsScanned = repairStats.prefabs.filesScanned;
    report.prefabsUpdated = repairStats.prefabs.filesUpdated;
    report.prefabTexturePathRemaps = repairStats.prefabs.texturePathRemaps;
    report.prefabMaterialOverridesReset = repairStats.prefabs.materialOverridesReset;

    RemoveUnusedSharedTextures(sharedDir,
                               projectDir,
                               report.texturesRemoved,
                               report.metaUpdates,
                               report.logLines);

    if (fs::exists(modelTexDir)) {
        std::error_code ec2;
        if (fs::is_empty(modelTexDir, ec2) && !ec2) {
            fs::remove(modelTexDir, ec2);
            if (!ec2) report.emptyFoldersRemoved += 1;
        }
    }

    return true;
}

} // namespace texture_cleanup

