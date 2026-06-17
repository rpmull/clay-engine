#include "BinaryAssetCache.h"
#include "BuildDependencyGraph.h"
#include "EntityBinaryWriter.h"
#include "MaterialBinaryWriter.h"
#include "AssetLibrary.h"
#include "ModelImportCache.h"
#include "core/ecs/Scene.h"
#include "core/ecs/ScenePostProcessing.h"
#include "core/serialization/Serializer.h"
#include "core/rendering/MaterialAssetCache.h"
#include "core/animation/AnimationAssetCache.h"
#include "core/animation/AnimationSerializer.h"
#include "core/animation/AnimatorController.h"
#include "core/animation/AnimatorControllerIO.h"
#include "core/prefab/PrefabAPI.h"
#include "core/prefab/PrefabAsset.h"
#include "core/prefab/PrefabDelta.h"
#include "core/prefab/PrefabPrewarm.h"
#include "core/prefab/RuntimePrefabInstantiator.h"
#include "core/assets/BinaryFormats.h"
#include "core/vfs/FileSystem.h"
#include "core/managed/ScriptReflection.h"  // For resolving script entity references
#include "editor/Project.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <chrono>
#include <cctype>
#include <functional>
#include <type_traits>
#include <unordered_set>

namespace fs = std::filesystem;
using json = nlohmann::json;

static fs::path ResolveSourcePathForRead(const std::string& sourcePath) {
    fs::path resolved(sourcePath);
    if (!resolved.is_absolute()) {
        fs::path proj = Project::GetProjectDirectory();
        if (!proj.empty()) {
            resolved = proj / resolved;
        }
    }
    return resolved;
}

static std::string ToLowerExt(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return ext;
}

static uint64_t GetFileTimestampToken(const fs::path& path, std::error_code& ec) {
    ec.clear();
    const auto writeTime = fs::last_write_time(path, ec);
    if (ec) {
        return 0ull;
    }

    using rep = fs::file_time_type::duration::rep;
    const rep ticks = writeTime.time_since_epoch().count();

    if constexpr (std::is_signed_v<rep>) {
        using unsigned_rep = std::make_unsigned_t<rep>;
        constexpr unsigned_rep signBias =
            unsigned_rep{1} << ((sizeof(unsigned_rep) * 8u) - 1u);
        return static_cast<uint64_t>(static_cast<unsigned_rep>(ticks) ^ signBias);
    } else {
        return static_cast<uint64_t>(ticks);
    }
}

static bool HasCurrentBinarySchemaVersion(const fs::path& binaryPath, BinaryAssetCache::AssetType type) {
    switch (type) {
        case BinaryAssetCache::AssetType::Scene:
        case BinaryAssetCache::AssetType::Material:
        case BinaryAssetCache::AssetType::Prefab:
        case BinaryAssetCache::AssetType::AnimatorController:
            break;
        default:
            return true;
    }

    std::ifstream in(binaryPath, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }

    binary::BinaryAssetHeader header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!in.good()) {
        return false;
    }

    switch (type) {
        case BinaryAssetCache::AssetType::Scene:
            return header.magic == binary::SCENE_MAGIC && header.version == binary::SCENE_VERSION;
        case BinaryAssetCache::AssetType::Material:
            return header.magic == binary::MATERIAL_MAGIC && header.version == binary::MATERIAL_VERSION;
        case BinaryAssetCache::AssetType::Prefab:
            return header.magic == binary::PREFAB_MAGIC && header.version == binary::PREFAB_VERSION;
        case BinaryAssetCache::AssetType::AnimatorController:
            return header.magic == binary::ANIM_CTRL_MAGIC && header.version == binary::ANIM_CTRL_VERSION;
        default:
            return true;
    }
}

static bool IsModelSourceExtension(const std::string& ext) {
    return ext == ".fbx" || ext == ".gltf" || ext == ".glb" || ext == ".obj";
}

static std::string ResolveModelSourcePathForCache(const std::string& rawPath);

static bool TryResolveModelBuildSource(const std::string& rawPath, std::string& outSourcePath) {
    fs::path resolved = ResolveSourcePathForRead(rawPath);
    std::string ext = ToLowerExt(resolved);
    if (!IsModelSourceExtension(ext) && ext != ".meta" && ext != ".meshbin" && ext != ".skelbin") {
        return false;
    }

    std::string sourcePath = ResolveModelSourcePathForCache(rawPath);
    fs::path sourceFsPath(sourcePath);
    if (!IsModelSourceExtension(ToLowerExt(sourceFsPath)) || !fs::exists(sourceFsPath)) {
        return false;
    }

    outSourcePath = sourceFsPath.lexically_normal().string();
    return true;
}

static std::string NormalizeDependencySetKey(const std::string& rawPath) {
    fs::path resolved = ResolveSourcePathForRead(rawPath).lexically_normal();
    std::string key = resolved.generic_string();
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return key;
}

static std::string ResolveModelSourcePathForCache(const std::string& rawPath) {
    fs::path resolved = ResolveSourcePathForRead(rawPath);
    std::string ext = ToLowerExt(resolved);
    if (IsModelSourceExtension(ext)) {
        return resolved.string();
    }

    if (ext != ".meta" && ext != ".meshbin" && ext != ".skelbin") {
        return resolved.string();
    }

    fs::path metaPath = resolved;
    if (ext != ".meta") {
        metaPath.replace_extension(".meta");
    }

    json metaJson;
    std::ifstream metaFile(metaPath);
    if (metaFile.is_open()) {
        try {
            metaFile >> metaJson;
        } catch (...) {
            metaJson = json{};
        }
    }

    if (metaJson.is_object()) {
        const char* sourceKeys[] = { "source", "sourcePath", "processedPath" };
        for (const char* key : sourceKeys) {
            if (!metaJson.contains(key) || !metaJson[key].is_string()) {
                continue;
            }

            fs::path candidate(metaJson[key].get<std::string>());
            if (!candidate.is_absolute()) {
                fs::path projectResolved = ResolveSourcePathForRead(candidate.string());
                if (fs::exists(projectResolved)) {
                    candidate = projectResolved;
                } else {
                    candidate = metaPath.parent_path() / candidate;
                }
            }

            if (IsModelSourceExtension(ToLowerExt(candidate)) && fs::exists(candidate)) {
                return candidate.lexically_normal().string();
            }
        }
    }

    for (const char* candidateExt : { ".fbx", ".gltf", ".glb", ".obj" }) {
        fs::path candidate = metaPath;
        candidate.replace_extension(candidateExt);
        if (fs::exists(candidate)) {
            return candidate.string();
        }
    }

    return resolved.string();
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

static bool TryResolveGeneratedReversedAnimationBase(const std::string& rawPath, std::string& outBasePath) {
    fs::path resolved = ResolveSourcePathForRead(rawPath);
    if (ToLowerExt(resolved) != ".anim") {
        return false;
    }

    std::string baseStem;
    if (!TryStripGeneratedReversedSuffix(resolved.stem().string(), baseStem)) {
        return false;
    }

    fs::path basePath = resolved.parent_path() / (baseStem + resolved.extension().string());
    if (!fs::exists(basePath)) {
        return false;
    }

    outBasePath = basePath.lexically_normal().string();
    return true;
}

static bool TryLoadAnimationSourceAsset(const std::string& sourcePath,
                                        cm::animation::AnimationAsset& outAsset,
                                        std::string* outFailureReason = nullptr) {
    try {
        std::ifstream file(ResolveSourcePathForRead(sourcePath));
        if (!file.is_open()) {
            if (outFailureReason) {
                *outFailureReason = "failed to open source";
            }
            return false;
        }

        json j;
        file >> j;
        file.close();

        if (j.contains("tracks") && j["tracks"].is_array()) {
            outAsset = cm::animation::DeserializeAnimationAsset(j);
        } else {
            cm::animation::AnimationClip clip = cm::animation::DeserializeAnimationClip(j);
            outAsset = cm::animation::WrapLegacyClipAsAsset(clip);
        }

        if (outAsset.tracks.empty()) {
            if (outFailureReason) {
                *outFailureReason = "animation contains no tracks";
            }
            return false;
        }

        return true;
    } catch (const std::exception& e) {
        if (outFailureReason) {
            *outFailureReason = e.what();
        }
        return false;
    }
}

template <typename TKey>
static void ReverseCurveKeys(std::vector<TKey>& keys, float duration) {
    for (auto& key : keys) {
        key.t = std::max(0.0f, duration - key.t);
    }
    std::sort(keys.begin(), keys.end(), [](const TKey& a, const TKey& b) {
        return a.t < b.t;
    });
}

static cm::animation::CurveFloat ReverseCurve(cm::animation::CurveFloat curve, float duration) {
    ReverseCurveKeys(curve.keys, duration);
    curve.cache = {};
    return curve;
}

static cm::animation::CurveVec2 ReverseCurve(cm::animation::CurveVec2 curve, float duration) {
    ReverseCurveKeys(curve.keys, duration);
    curve.cache = {};
    return curve;
}

static cm::animation::CurveVec3 ReverseCurve(cm::animation::CurveVec3 curve, float duration) {
    ReverseCurveKeys(curve.keys, duration);
    curve.cache = {};
    return curve;
}

static cm::animation::CurveQuat ReverseCurve(cm::animation::CurveQuat curve, float duration) {
    ReverseCurveKeys(curve.keys, duration);
    curve.cache = {};
    return curve;
}

static cm::animation::CurveColor ReverseCurve(cm::animation::CurveColor curve, float duration) {
    ReverseCurveKeys(curve.keys, duration);
    curve.cache = {};
    return curve;
}

static std::unique_ptr<cm::animation::ITrack> CloneReversedAnimationTrack(const cm::animation::ITrack& sourceTrack,
                                                                          float duration) {
    using namespace cm::animation;

    switch (sourceTrack.type) {
        case TrackType::Bone: {
            const auto& src = static_cast<const AssetBoneTrack&>(sourceTrack);
            auto dst = std::make_unique<AssetBoneTrack>();
            dst->id = src.id;
            dst->name = src.name;
            dst->muted = src.muted;
            dst->boneId = src.boneId;
            dst->t = ReverseCurve(src.t, duration);
            dst->r = ReverseCurve(src.r, duration);
            dst->s = ReverseCurve(src.s, duration);
            return dst;
        }
        case TrackType::Avatar: {
            const auto& src = static_cast<const AssetAvatarTrack&>(sourceTrack);
            auto dst = std::make_unique<AssetAvatarTrack>();
            dst->id = src.id;
            dst->name = src.name;
            dst->muted = src.muted;
            dst->humanBoneId = src.humanBoneId;
            dst->t = ReverseCurve(src.t, duration);
            dst->r = ReverseCurve(src.r, duration);
            dst->s = ReverseCurve(src.s, duration);
            return dst;
        }
        case TrackType::Property: {
            const auto& src = static_cast<const AssetPropertyTrack&>(sourceTrack);
            auto dst = std::make_unique<AssetPropertyTrack>();
            dst->id = src.id;
            dst->name = src.name;
            dst->muted = src.muted;
            dst->binding = src.binding;
            switch (src.binding.type) {
                case cm::animation::PropertyType::Float:
                    dst->curve = ReverseCurve(std::get<CurveFloat>(src.curve), duration);
                    break;
                case cm::animation::PropertyType::Vec2:
                    dst->curve = ReverseCurve(std::get<CurveVec2>(src.curve), duration);
                    break;
                case cm::animation::PropertyType::Vec3:
                    dst->curve = ReverseCurve(std::get<CurveVec3>(src.curve), duration);
                    break;
                case cm::animation::PropertyType::Quat:
                    dst->curve = ReverseCurve(std::get<CurveQuat>(src.curve), duration);
                    break;
                case cm::animation::PropertyType::Color:
                    dst->curve = ReverseCurve(std::get<CurveColor>(src.curve), duration);
                    break;
            }
            return dst;
        }
        case TrackType::ScriptEvent: {
            const auto& src = static_cast<const AssetScriptEventTrack&>(sourceTrack);
            auto dst = std::make_unique<AssetScriptEventTrack>();
            dst->id = src.id;
            dst->name = src.name;
            dst->muted = src.muted;
            dst->events = src.events;
            for (auto& event : dst->events) {
                event.time = std::max(0.0f, duration - event.time);
            }
            std::sort(dst->events.begin(), dst->events.end(), [](const AssetScriptEvent& a, const AssetScriptEvent& b) {
                return a.time < b.time;
            });
            return dst;
        }
    }

    return nullptr;
}

static bool TryBuildGeneratedReversedAnimationAsset(const std::string& requestedPath,
                                                    cm::animation::AnimationAsset& outAsset) {
    std::string baseSourcePath;
    if (!TryResolveGeneratedReversedAnimationBase(requestedPath, baseSourcePath)) {
        return false;
    }

    cm::animation::AnimationAsset baseAsset;
    std::string loadFailureReason;
    if (!TryLoadAnimationSourceAsset(baseSourcePath, baseAsset, &loadFailureReason)) {
        std::cerr << "[BinaryAssetCache] Failed to load base animation for reversed asset: "
                  << baseSourcePath << " (" << loadFailureReason << ")" << std::endl;
        return false;
    }

    const float duration = std::max(baseAsset.meta.length, baseAsset.Duration());
    if (duration <= 0.0f || baseAsset.tracks.empty()) {
        std::cerr << "[BinaryAssetCache] Base animation is empty for reversed asset: "
                  << baseSourcePath << std::endl;
        return false;
    }

    cm::animation::AnimationAsset reversedAsset;
    reversedAsset.name = fs::path(requestedPath).stem().string();
    if (reversedAsset.name.empty()) {
        reversedAsset.name = baseAsset.name.empty()
            ? fs::path(baseSourcePath).stem().string() + "_Reversed"
            : baseAsset.name + "_Reversed";
    }
    reversedAsset.meta = baseAsset.meta;
    reversedAsset.meta.length = duration;
    reversedAsset.LegacySourceAvatar = baseAsset.LegacySourceAvatar;
    reversedAsset.tracks.reserve(baseAsset.tracks.size());

    for (const auto& track : baseAsset.tracks) {
        if (!track) {
            continue;
        }

        std::unique_ptr<cm::animation::ITrack> reversedTrack =
            CloneReversedAnimationTrack(*track, duration);
        if (reversedTrack) {
            reversedAsset.tracks.push_back(std::move(reversedTrack));
        }
    }

    if (reversedAsset.tracks.empty()) {
        return false;
    }

    outAsset = std::move(reversedAsset);
    std::cout << "[BinaryAssetCache] Synthesized reversed animation binary from base source: "
              << baseSourcePath << " -> " << requestedPath << std::endl;
    return true;
}

BinaryAssetCache& BinaryAssetCache::Instance() {
    static BinaryAssetCache instance;
    return instance;
}

void BinaryAssetCache::Initialize(const fs::path& projectRoot) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    m_ProjectRoot = projectRoot;
    m_CacheRoot = projectRoot / ".bin";
    
    // Set project root on FileSystem so relative paths resolve correctly
    FileSystem::Instance().SetProjectRoot(projectRoot);
    
    // Create cache directories
    std::error_code ec;
    fs::create_directories(m_CacheRoot / "scenes", ec);
    fs::create_directories(m_CacheRoot / "materials", ec);
    fs::create_directories(m_CacheRoot / "prefabs", ec);
    fs::create_directories(m_CacheRoot / "animations", ec);
    fs::create_directories(m_CacheRoot / "animators", ec);
    
    // Load existing manifest
    LoadManifest();
    
    m_Initialized = true;
    std::cout << "[BinaryAssetCache] Initialized at: " << m_CacheRoot << std::endl;
}

void BinaryAssetCache::Shutdown() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    SaveManifest();
    m_Manifest.clear();
    m_Initialized = false;
}

bool BinaryAssetCache::IsCurrent(const std::string& sourcePath) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    std::string key = MakeCacheKey(sourcePath);
    auto it = m_Manifest.find(key);
    if (it == m_Manifest.end()) {
        return false;
    }
    
    const AssetType type = GetAssetType(sourcePath);
    if (type == AssetType::Unknown) {
        return false;
    }

    const fs::path expectedBinaryPath = SourceToCache(sourcePath, type);
    const fs::path recordedBinaryPath = it->second.binaryPath.empty()
        ? expectedBinaryPath
        : fs::path(it->second.binaryPath);
    if (!expectedBinaryPath.empty() && !fs::exists(expectedBinaryPath)) {
        return false;
    }
    if (!recordedBinaryPath.empty() && recordedBinaryPath != expectedBinaryPath && !fs::exists(recordedBinaryPath)) {
        return false;
    }
    if (!expectedBinaryPath.empty() && fs::exists(expectedBinaryPath) &&
        !HasCurrentBinarySchemaVersion(expectedBinaryPath, type)) {
        return false;
    }
    if (!recordedBinaryPath.empty() && recordedBinaryPath != expectedBinaryPath &&
        fs::exists(recordedBinaryPath) &&
        !HasCurrentBinarySchemaVersion(recordedBinaryPath, type)) {
        return false;
    }

    // Check if source has been modified (must resolve VFS/project-relative paths;
    // raw relative paths are CWD-dependent and break cache correctness).
    std::error_code ec;
    const fs::path sourceFsPath = ResolveSourcePathForRead(sourcePath);
    const uint64_t sourceModTime = GetFileTimestampToken(sourceFsPath, ec);
    if (ec) return false;

    return sourceModTime <= it->second.sourceModTime;
}

std::string BinaryAssetCache::GetBinaryPath(const std::string& sourcePath) const {
    AssetType type = GetAssetType(sourcePath);
    if (type == AssetType::Unknown) {
        return "";
    }
    
    fs::path cachePath = SourceToCache(sourcePath, type);
    return cachePath.string();
}

bool BinaryAssetCache::EnsureBinary(const std::string& sourcePath) {
    if (IsCurrent(sourcePath)) {
        return true;
    }
    
    return RebuildBinary(sourcePath);
}

bool BinaryAssetCache::RebuildBinary(const std::string& sourcePath) {
    AssetType type = GetAssetType(sourcePath);
    if (type == AssetType::Unknown) {
        std::cerr << "[BinaryAssetCache] Unknown asset type: " << sourcePath << std::endl;
        return false;
    }

    std::string key = MakeCacheKey(sourcePath);
    static thread_local std::unordered_set<std::string> s_BuildStack;
    if (!s_BuildStack.insert(key).second) {
        std::cerr << "[BinaryAssetCache] Re-entrant binary build blocked for: "
                  << sourcePath << std::endl;
        return false;
    }
    struct BuildStackGuard {
        std::unordered_set<std::string>& stack;
        std::string key;
        ~BuildStackGuard() { stack.erase(key); }
    } buildStackGuard{ s_BuildStack, key };
    
    std::string binaryPath = GetBinaryPath(sourcePath);
    if (binaryPath.empty()) {
        return false;
    }
    
    // Ensure parent directory exists
    std::error_code ec;
    fs::create_directories(fs::path(binaryPath).parent_path(), ec);
    
    bool success = false;
    switch (type) {
        case AssetType::Scene:
            success = BuildSceneBinary(sourcePath, binaryPath);
            break;
        case AssetType::Material:
            success = BuildMaterialBinary(sourcePath, binaryPath);
            break;
        case AssetType::Prefab:
            success = BuildPrefabBinary(sourcePath, binaryPath);
            break;
        case AssetType::Animation:
            success = BuildAnimationBinary(sourcePath, binaryPath);
            break;
        case AssetType::AnimatorController:
            success = BuildAnimatorControllerBinary(sourcePath, binaryPath);
            break;
        default:
            break;
    }
    
    if (success) {
        switch (type) {
            case AssetType::Material:
                MaterialAssetCache::Invalidate(sourcePath);
                MaterialAssetCache::Invalidate(binaryPath);
                break;
            case AssetType::Prefab:
                runtime::RuntimePrefabInstantiator::InvalidateCache(sourcePath);
                runtime::RuntimePrefabInstantiator::InvalidateCache(binaryPath);
                break;
            case AssetType::Animation:
                cm::animation::InvalidateAnimationAssetCache(sourcePath);
                cm::animation::InvalidateAnimationAssetCache(binaryPath);
                break;
            case AssetType::AnimatorController:
                cm::animation::InvalidateAnimatorControllerCache(sourcePath);
                cm::animation::InvalidateAnimatorControllerCache(binaryPath);
                break;
            default:
                break;
        }
        // Update manifest
        std::lock_guard<std::mutex> lock(m_Mutex);
        
        CacheEntry entry;
        entry.binaryPath = binaryPath;
        
        const fs::path sourceFsPath = ResolveSourcePathForRead(sourcePath);
        entry.sourceModTime = GetFileTimestampToken(sourceFsPath, ec);
        
        entry.binaryModTime = GetFileTimestampToken(binaryPath, ec);
        
        m_Manifest[key] = entry;
        SaveManifest();
    }
    
    return success;
}

void BinaryAssetCache::Invalidate(const std::string& sourcePath) {
    const AssetType type = GetAssetType(sourcePath);
    std::string binaryPath = GetBinaryPath(sourcePath);

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        std::string key = MakeCacheKey(sourcePath);
        auto it = m_Manifest.find(key);
        if (it != m_Manifest.end()) {
            std::error_code ec;
            if (!it->second.binaryPath.empty()) {
                binaryPath = it->second.binaryPath;
            }
            fs::remove(it->second.binaryPath, ec);
            m_Manifest.erase(it);
            SaveManifest();
        }
    }

    switch (type) {
        case AssetType::Material:
            MaterialAssetCache::Invalidate(sourcePath);
            MaterialAssetCache::Invalidate(binaryPath);
            break;
        case AssetType::Prefab:
            runtime::RuntimePrefabInstantiator::InvalidateCache(sourcePath);
            runtime::RuntimePrefabInstantiator::InvalidateCache(binaryPath);
            break;
        case AssetType::Animation:
            cm::animation::InvalidateAnimationAssetCache(sourcePath);
            cm::animation::InvalidateAnimationAssetCache(binaryPath);
            break;
        case AssetType::AnimatorController:
            cm::animation::InvalidateAnimatorControllerCache(sourcePath);
            cm::animation::InvalidateAnimatorControllerCache(binaryPath);
            break;
        default:
            break;
    }
}

void BinaryAssetCache::InvalidateAll() {
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        // Delete all cached binaries.
        std::error_code ec;
        fs::remove_all(m_CacheRoot / "scenes", ec);
        fs::remove_all(m_CacheRoot / "materials", ec);
        fs::remove_all(m_CacheRoot / "prefabs", ec);
        fs::remove_all(m_CacheRoot / "animations", ec);
        fs::remove_all(m_CacheRoot / "animators", ec);

        // Recreate directories.
        fs::create_directories(m_CacheRoot / "scenes", ec);
        fs::create_directories(m_CacheRoot / "materials", ec);
        fs::create_directories(m_CacheRoot / "prefabs", ec);
        fs::create_directories(m_CacheRoot / "animations", ec);
        fs::create_directories(m_CacheRoot / "animators", ec);

        m_Manifest.clear();
        SaveManifest();
    }

    MaterialAssetCache::Clear();
    runtime::RuntimePrefabInstantiator::ResetRuntimeCaches();
    cm::animation::ClearAnimationAssetCache();
    cm::animation::ClearAnimatorControllerCache();
}

void BinaryAssetCache::RebuildAll() {
    InvalidateAll();
    EnsureAllCurrent();
}

void BinaryAssetCache::EnsureAllCurrent() {
    // Scan project for assets and ensure all have current binaries
    // Include both assets/ and scenes/ directories
    std::vector<fs::path> dirsToScan = {
        m_ProjectRoot / "assets",
        m_ProjectRoot / "scenes"
    };
    
    std::error_code ec;
    for (const auto& dir : dirsToScan) {
        if (!fs::exists(dir, ec)) continue;
        
        for (auto& entry : fs::recursive_directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            
            std::string path = entry.path().string();
            AssetType type = GetAssetType(path);
            
            if (type != AssetType::Unknown) {
                if (!IsCurrent(path)) {
                    std::cout << "[BinaryAssetCache] Building binary for: " << entry.path().filename() << std::endl;
                    RebuildBinary(path);
                }
                continue;
            }

            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".fbx" || ext == ".gltf" || ext == ".glb" || ext == ".obj") {
                BuiltModelPaths builtPaths;
                if (!HasModelCache(path, builtPaths)) {
                    std::cout << "[BinaryAssetCache] Building model runtime cache for: "
                              << entry.path().filename() << std::endl;
                    EnsureModelCache(path, builtPaths);
                }
            }
        }
    }
}

bool BinaryAssetCache::EnsureSceneDependenciesCurrent(const std::string& scenePath,
                                                      bool includeEntrySceneBinary,
                                                      EnsureStats* outStats) {
    if (outStats) {
        *outStats = EnsureStats{};
    }
    if (scenePath.empty()) {
        return true;
    }

    BuildDependencyGraph graph;
    graph.AddEntryScene(scenePath);

    std::vector<std::string> paths = graph.FlattenExistingPaths();
    if (!includeEntrySceneBinary) {
        const std::string sceneKey = NormalizeDependencySetKey(scenePath);
        paths.erase(std::remove_if(paths.begin(), paths.end(),
            [&](const std::string& path) {
                return NormalizeDependencySetKey(path) == sceneKey;
            }), paths.end());
    }

    return EnsureDependenciesCurrent(paths, outStats);
}

bool BinaryAssetCache::EnsureDependenciesCurrent(const std::vector<std::string>& dependencyPaths,
                                                 EnsureStats* outStats) {
    EnsureStats stats{};
    std::unordered_set<std::string> visited;
    bool success = true;

    for (const std::string& path : dependencyPaths) {
        if (path.empty()) {
            continue;
        }

        std::string buildPath = path;
        AssetType type = GetAssetType(buildPath);
        bool isModelSource = false;

        if (type == AssetType::Unknown) {
            isModelSource = TryResolveModelBuildSource(path, buildPath);
            if (!isModelSource) {
                continue;
            }
        }

        const std::string key = NormalizeDependencySetKey(buildPath);
        if (!visited.insert(key).second) {
            continue;
        }

        ++stats.inspected;
        if (isModelSource) {
            BuiltModelPaths builtPaths;
            if (HasModelCache(buildPath, builtPaths)) {
                ++stats.alreadyCurrent;
                continue;
            }

            std::cout << "[BinaryAssetCache] Building model runtime cache for dependency: "
                      << fs::path(buildPath).filename() << std::endl;
            if (EnsureModelCache(buildPath, builtPaths)) {
                ++stats.rebuilt;
            } else {
                ++stats.failed;
                success = false;
            }
            continue;
        }

        if (IsCurrent(buildPath)) {
            ++stats.alreadyCurrent;
            continue;
        }

        std::cout << "[BinaryAssetCache] Building binary for dependency: "
                  << fs::path(buildPath).filename() << std::endl;
        if (RebuildBinary(buildPath)) {
            ++stats.rebuilt;
        } else {
            ++stats.failed;
            success = false;
        }
    }

    if (outStats) {
        *outStats = stats;
    }

    std::cout << "[BinaryAssetCache] Dependency binary check: inspected="
              << stats.inspected
              << " current=" << stats.alreadyCurrent
              << " rebuilt=" << stats.rebuilt
              << " failed=" << stats.failed << std::endl;
    return success;
}

bool BinaryAssetCache::CollectMissingBinaries(std::vector<std::string>& outMissing) const {
    outMissing.clear();
    if (!m_Initialized) return false;

    std::vector<fs::path> dirsToScan = {
        m_ProjectRoot / "assets",
        m_ProjectRoot / "scenes"
    };

    std::error_code ec;
    for (const auto& dir : dirsToScan) {
        if (!fs::exists(dir, ec)) continue;

        for (auto& entry : fs::recursive_directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;

            std::string path = entry.path().string();
            AssetType type = GetAssetType(path);
            if (type == AssetType::Unknown) continue;

            std::string binaryPath = GetBinaryPath(path);
            if (binaryPath.empty() || !fs::exists(binaryPath, ec)) {
                outMissing.push_back(path);
            }
        }
    }

    return true;
}

bool BinaryAssetCache::IsInCache(const fs::path& path) const {
    if (!m_Initialized) return false;
    
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(path, ec);
    fs::path cacheCanonical = fs::weakly_canonical(m_CacheRoot, ec);
    
    // Check if path starts with cache root
    auto [cacheEnd, pathEnd] = std::mismatch(
        cacheCanonical.begin(), cacheCanonical.end(), 
        canonical.begin(), canonical.end());
    
    return cacheEnd == cacheCanonical.end();
}

BinaryAssetCache::AssetType BinaryAssetCache::GetAssetType(const std::string& path) {
    fs::path p(path);
    std::string ext = p.extension().string();
    
    // Convert to lowercase
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    if (ext == ".scene") return AssetType::Scene;
    if (ext == ".mat") return AssetType::Material;
    if (ext == ".prefab") return AssetType::Prefab;
    // Also recognize .json files under assets/prefabs/ as authoring prefabs
    if (ext == ".json") {
        std::string normalized = path;
        for (char& c : normalized) { if (c == '\\') c = '/'; }
        if (normalized.find("assets/prefabs/") != std::string::npos || 
            normalized.find("/assets/prefabs/") != std::string::npos) {
            return AssetType::Prefab;
        }
    }
    if (ext == ".anim") return AssetType::Animation;
    if (ext == ".animctrl" || ext == ".controller") return AssetType::AnimatorController;
    
    return AssetType::Unknown;
}

std::string BinaryAssetCache::GetBinaryExtension(AssetType type) {
    switch (type) {
        case AssetType::Scene: return ".sceneb";
        case AssetType::Material: return ".matbin";
        case AssetType::Prefab: return ".prefabb";
        case AssetType::Animation: return ".animbin";
        case AssetType::AnimatorController: return ".actrlbin";
        default: return "";
    }
}

bool BinaryAssetCache::BuildSceneBinary(const std::string& sourcePath, const std::string& binaryPath) {
    // Load scene from authoring JSON directly (avoid binary-only recursion)
    Scene scene;
    json sceneData;
    std::string sceneText;
    if (FileSystem::Instance().ReadTextFile(sourcePath, sceneText)) {
        sceneData = json::parse(sceneText);
    } else {
        std::vector<uint8_t> bytes;
        if (FileSystem::Instance().ReadFile(sourcePath, bytes)) {
            sceneData = json::parse(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        } else {
            std::cerr << "[BinaryAssetCache] Scene file does not exist or cannot be read: " << sourcePath << std::endl;
            return false;
        }
    }
    if (!Serializer::DeserializeScene(sceneData, scene)) {
        std::cerr << "[BinaryAssetCache] Failed to deserialize scene: " << sourcePath << std::endl;
        return false;
    }
    prefab::QueueScenePrefabs(scene);
    
    // Write binary
    return binary::EntityBinaryWriter::Write(scene, binaryPath);
}

bool BinaryAssetCache::BuildMaterialBinary(const std::string& sourcePath, const std::string& binaryPath) {
    return binary::MaterialBinaryWriter::ConvertMaterial(sourcePath, binaryPath);
}

// Helper: Collect all model asset paths referenced in a prefab JSON
static std::unordered_set<std::string> CollectPrefabModelPaths(const PrefabAsset& asset) {
    std::unordered_set<std::string> modelPaths;
    
    for (const auto& entityJson : asset.Entities) {
        if (!entityJson.is_object()) continue;
        
        // Check for model asset
        if (entityJson.contains("asset") && entityJson["asset"].is_object()) {
            const auto& assetBlock = entityJson["asset"];
            if (assetBlock.value("type", "") == "model") {
                std::string modelPath = assetBlock.value("path", "");
                if (!modelPath.empty()) {
                    // Resolve to absolute path
                    std::string resolved = ResolveModelSourcePathForCache(modelPath);
                    for (char& c : resolved) if (c == '\\') c = '/';
                    if (fs::exists(resolved) && IsModelSourceExtension(ToLowerExt(fs::path(resolved)))) {
                        modelPaths.insert(resolved);
                    }
                }
            }
        }
    }
    
    return modelPaths;
}

// Helper: Ensure all model binaries for a prefab are built and registered
static void EnsurePrefabModelBinaries(const PrefabAsset& asset) {
    auto modelPaths = CollectPrefabModelPaths(asset);
    
    if (modelPaths.empty()) {
        std::cout << "[BinaryAssetCache] Prefab has no model references" << std::endl;
        return;
    }
    
    std::cout << "[BinaryAssetCache] Ensuring " << modelPaths.size() << " model binaries for prefab..." << std::endl;
    
    for (const auto& modelPath : modelPaths) {
        BuiltModelPaths builtPaths;
        
        // Check if model cache exists, build if not
        if (!HasModelCache(modelPath, builtPaths)) {
            std::cout << "[BinaryAssetCache] Building model cache for: " << modelPath << std::endl;
            if (EnsureModelCache(modelPath, builtPaths)) {
                std::cout << "[BinaryAssetCache]   meshbin: " << builtPaths.meshPath << std::endl;
                std::cout << "[BinaryAssetCache]   skelbin: " << builtPaths.skelPath << std::endl;
            } else {
                std::cerr << "[BinaryAssetCache] Failed to build model cache for: " << modelPath << std::endl;
            }
        } else {
            std::cout << "[BinaryAssetCache] Model cache up-to-date: " << modelPath << std::endl;
        }
        
        // Register mesh GUID with AssetLibrary for runtime resolution
        if (!builtPaths.meshPath.empty() && fs::exists(builtPaths.meshPath)) {
            // Try to get GUID from meta file
            if (!builtPaths.metaPath.empty() && fs::exists(builtPaths.metaPath)) {
                try {
                    std::ifstream metaFile(builtPaths.metaPath);
                    if (metaFile.is_open()) {
                        json metaJson;
                        metaFile >> metaJson;
                        metaFile.close();
                        
                        if (metaJson.contains("guid")) {
                            ClaymoreGUID modelGuid;
                            if (metaJson["guid"].is_string()) {
                                modelGuid = ClaymoreGUID::FromString(metaJson["guid"].get<std::string>());
                            } else if (metaJson["guid"].is_object()) {
                                metaJson["guid"].get_to(modelGuid);
                            }
                            
                            if (modelGuid.high != 0 || modelGuid.low != 0) {
                                // Register all paths with this GUID
                                AssetLibrary::Instance().RegisterPathAlias(modelGuid, builtPaths.meshPath);
                                AssetLibrary::Instance().RegisterPathAlias(modelGuid, builtPaths.metaPath);
                                if (!builtPaths.skelPath.empty()) {
                                    AssetLibrary::Instance().RegisterPathAlias(modelGuid, builtPaths.skelPath);
                                }
                                std::cout << "[BinaryAssetCache]   Registered GUID " << modelGuid.ToString() 
                                          << " -> " << builtPaths.meshPath << std::endl;
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    std::cerr << "[BinaryAssetCache] Failed to read model meta: " << e.what() << std::endl;
                }
            }
        }
    }
}

static void RemapGuidIfNeeded(const std::unordered_map<ClaymoreGUID, ClaymoreGUID>& instanceToPrefabGuid,
                              uint64_t& high,
                              uint64_t& low) {
    ClaymoreGUID current{high, low};
    auto it = instanceToPrefabGuid.find(current);
    if (it != instanceToPrefabGuid.end()) {
        high = it->second.high;
        low = it->second.low;
    }
}

static void NormalizePropertyValueEntityMetadata(
    PropertyValue& value,
    const std::unordered_map<ClaymoreGUID, ClaymoreGUID>& instanceToPrefabGuid) {
    if (std::holds_alternative<std::shared_ptr<ListPropertyValue>>(value)) {
        auto listPtr = std::get<std::shared_ptr<ListPropertyValue>>(value);
        if (!listPtr) return;
        for (auto& meta : listPtr->entityRefs) {
            auto itGuid = instanceToPrefabGuid.find(meta.guid);
            if (itGuid != instanceToPrefabGuid.end()) {
                meta.guid = itGuid->second;
            }
            auto itRoot = instanceToPrefabGuid.find(meta.modelRootGuid);
            if (itRoot != instanceToPrefabGuid.end()) {
                meta.modelRootGuid = itRoot->second;
            }
        }
        for (auto& elem : listPtr->elements) {
            NormalizePropertyValueEntityMetadata(elem, instanceToPrefabGuid);
        }
        return;
    }

    if (std::holds_alternative<std::shared_ptr<StructPropertyValue>>(value)) {
        auto structPtr = std::get<std::shared_ptr<StructPropertyValue>>(value);
        if (!structPtr) return;
        for (auto& field : structPtr->fields) {
            NormalizePropertyValueEntityMetadata(field.second, instanceToPrefabGuid);
        }
        return;
    }

    if (std::holds_alternative<std::shared_ptr<DictionaryPropertyValue>>(value)) {
        auto dictPtr = std::get<std::shared_ptr<DictionaryPropertyValue>>(value);
        if (!dictPtr) return;
        for (auto& entry : dictPtr->entries) {
            NormalizePropertyValueEntityMetadata(entry.first, instanceToPrefabGuid);
            NormalizePropertyValueEntityMetadata(entry.second, instanceToPrefabGuid);
        }
    }
}

static void RestorePrefabAuthoringIdentity(Scene& scene, EntityID rootId) {
    EntityData* rootData = scene.GetEntityData(rootId);
    if (!rootData || !rootData->PrefabInstance) {
        return;
    }

    const auto& instanceToPrefabGuid = rootData->PrefabInstance->InstanceToPrefabGuid;
    if (instanceToPrefabGuid.empty()) {
        return;
    }

    std::unordered_set<EntityID> visited;
    size_t restoredEntityGuids = 0;
    std::function<void(EntityID)> normalize = [&](EntityID id) {
        if (id == INVALID_ENTITY_ID || !visited.insert(id).second) {
            return;
        }

        EntityData* data = scene.GetEntityData(id);
        if (!data) {
            return;
        }

        auto itEntityGuid = instanceToPrefabGuid.find(data->EntityGuid);
        if (itEntityGuid != instanceToPrefabGuid.end()) {
            data->EntityGuid = itEntityGuid->second;
            ++restoredEntityGuids;
        }

        for (auto& lac : data->LookAtConstraints) {
            RemapGuidIfNeeded(instanceToPrefabGuid, lac.TargetEntityGuidHigh, lac.TargetEntityGuidLow);
        }

        for (auto& ik : data->IKs) {
            RemapGuidIfNeeded(instanceToPrefabGuid, ik.TargetEntityGuidHigh, ik.TargetEntityGuidLow);
            RemapGuidIfNeeded(instanceToPrefabGuid, ik.PoleEntityGuidHigh, ik.PoleEntityGuidLow);
        }

        for (auto& script : data->Scripts) {
            for (auto& [propertyName, meta] : script.EntityRefMetadata) {
                (void)propertyName;
                auto itGuid = instanceToPrefabGuid.find(meta.guid);
                if (itGuid != instanceToPrefabGuid.end()) {
                    meta.guid = itGuid->second;
                }
                auto itRoot = instanceToPrefabGuid.find(meta.modelRootGuid);
                if (itRoot != instanceToPrefabGuid.end()) {
                    meta.modelRootGuid = itRoot->second;
                }
            }

            for (auto& [propertyName, propertyValue] : script.Values) {
                (void)propertyName;
                NormalizePropertyValueEntityMetadata(propertyValue, instanceToPrefabGuid);
            }
        }

        for (EntityID child : data->Children) {
            normalize(child);
        }
    };

    normalize(rootId);
    std::cout << "[BinaryAssetCache] Restored " << restoredEntityGuids
              << " prefab-space entity GUIDs before prefab binary write" << std::endl;
}

bool BinaryAssetCache::BuildPrefabBinary(const std::string& sourcePath, const std::string& binaryPath) {
    std::cout << "[BinaryAssetCache] BuildPrefabBinary called:" << std::endl;
    std::cout << "[BinaryAssetCache]   sourcePath: " << sourcePath << std::endl;
    std::cout << "[BinaryAssetCache]   binaryPath: " << binaryPath << std::endl;
    
    // Load prefab from JSON authoring format. This must bypass play-mode
    // binary resolution because we are building the binary being requested.
    PrefabAsset asset;
    if (!PrefabIO::LoadPrefabSource(sourcePath, asset)) {
        std::cerr << "[BinaryAssetCache] Failed to load prefab: " << sourcePath << std::endl;
        return false;
    }
    
    // Ensure all referenced model binaries are built and registered
    EnsurePrefabModelBinaries(asset);
    
    std::cout << "[BinaryAssetCache] Loaded prefab: " << asset.Name << " with " << asset.EntityCount() << " entities" << std::endl;
    
    if (!asset.IsValid()) {
        std::cerr << "[BinaryAssetCache] Invalid prefab (no entities): " << sourcePath << std::endl;
        return false;
    }
    
    // Use single code path: core prefab instantiation (same as editor/runtime JSON load).
    // Ensures binary build and scene load never diverge on entity creation, hierarchy, or script refs.
    Scene tempScene;
    EntityID rootId = InstantiatePrefabAsset(asset, tempScene, INVALID_ENTITY_ID, false, nullptr);
    if (rootId == INVALID_ENTITY_ID) {
        std::cerr << "[BinaryAssetCache] InstantiatePrefabAsset failed for: " << sourcePath << std::endl;
        return false;
    }

    // InstantiatePrefabAsset intentionally creates scene-local instance GUIDs.
    // Prefab binaries must instead store prefab-space GUIDs so runtime can align
    // source JSON, override mappings, and script/entity metadata deterministically.
    RestorePrefabAuthoringIdentity(tempScene, rootId);

    // Write using unified binary writer (same as scenes)
    return binary::EntityBinaryWriter::WritePrefab(tempScene, rootId, binaryPath);
}

bool BinaryAssetCache::BuildAnimationBinary(const std::string& sourcePath, const std::string& binaryPath) {
    try {
        cm::animation::AnimationAsset asset;
        std::string loadFailureReason;
        const bool canSynthesizeReversed = [&]() {
            std::string generatedBaseSourcePath;
            return TryResolveGeneratedReversedAnimationBase(sourcePath, generatedBaseSourcePath);
        }();

        auto trySynthesizeReversed = [&](const char* reason) -> bool {
            if (!canSynthesizeReversed) {
                return false;
            }
            if (!TryBuildGeneratedReversedAnimationAsset(sourcePath, asset)) {
                return false;
            }
            if (reason && *reason) {
                std::cout << "[BinaryAssetCache] Falling back to synthesized reversed animation for '"
                          << sourcePath << "' (" << reason << ")" << std::endl;
            }
            loadFailureReason.clear();
            return true;
        };

        if (!TryLoadAnimationSourceAsset(sourcePath, asset, &loadFailureReason)) {
            if (!trySynthesizeReversed(loadFailureReason.c_str())) {
                std::cerr << "[BinaryAssetCache] Failed to build animation binary from source '"
                          << sourcePath << "': " << loadFailureReason << std::endl;
                return false;
            }
        }

        if (asset.tracks.empty()) {
            if (!trySynthesizeReversed("parsed animation contained no tracks")) {
                std::cerr << "[BinaryAssetCache] Failed to parse animation: " << sourcePath << std::endl;
                return false;
            }
        }
        
        std::cout << "[BinaryAssetCache] Building animation binary with RootMotion Mode: " 
                  << (int)asset.meta.rootMotion.Mode << std::endl;
        
        // Use the existing WriteCompiledAnimation function
        if (cm::animation::WriteCompiledAnimation(asset, fs::path(binaryPath))) {
            return true;
        }

        if (trySynthesizeReversed("failed to write compiled animation from source data") &&
            !asset.tracks.empty() &&
            cm::animation::WriteCompiledAnimation(asset, fs::path(binaryPath))) {
            return true;
        }

        std::cerr << "[BinaryAssetCache] Failed to write compiled animation binary: " << binaryPath << std::endl;
        return false;
    } catch (const std::exception& e) {
        std::cerr << "[BinaryAssetCache] Failed to build animation binary: " << e.what() << std::endl;
        return false;
    }
}

bool BinaryAssetCache::BuildAnimatorControllerBinary(const std::string& sourcePath, const std::string& binaryPath) {
    // Load animator controller from JSON and serialize to binary
    try {
        // Read JSON directly to avoid binary-only load rules.
        std::ifstream file(ResolveSourcePathForRead(sourcePath));
        if (!file.is_open()) {
            std::cerr << "[BinaryAssetCache] Failed to open animator controller source: " << sourcePath << std::endl;
            return false;
        }

        json j;
        file >> j;
        file.close();

        cm::animation::AnimatorController ctrl;
        nlohmann::from_json(j, ctrl);

        std::ofstream out(binaryPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;

        // Binary header
        uint32_t magic = 'A' | ('C' << 8) | ('T' << 16) | ('B' << 24);
        uint32_t version = 1;
        out.write(reinterpret_cast<const char*>(&magic), 4);
        out.write(reinterpret_cast<const char*>(&version), 4);

        // Serialize controller to JSON then to binary (compact)
        json outJson;
        nlohmann::to_json(outJson, ctrl);
        std::string jsonStr = outJson.dump();
        uint32_t jsonLen = static_cast<uint32_t>(jsonStr.size());
        out.write(reinterpret_cast<const char*>(&jsonLen), 4);
        out.write(jsonStr.data(), jsonLen);

        return true;
    } catch (const std::exception& e) {
        std::cerr << "[BinaryAssetCache] Failed to build animator controller binary: " << e.what() << std::endl;
        return false;
    }
}

std::string BinaryAssetCache::MakeCacheKey(const std::string& sourcePath) const {
    // Normalize path relative to project root
    std::string normalized = sourcePath;
    for (char& c : normalized) {
        if (c == '\\') c = '/';
    }
    
    // If it's already a VFS-relative path (starts with assets/), use it directly
    if (normalized.find("assets/") == 0) {
        return normalized;
    }
    
    // If it contains assets/ somewhere, extract from there
    size_t assetsPos = normalized.find("/assets/");
    if (assetsPos != std::string::npos) {
        return normalized.substr(assetsPos + 1); // +1 to skip the leading /
    }
    assetsPos = normalized.find("assets/");
    if (assetsPos != std::string::npos) {
        return normalized.substr(assetsPos);
    }
    
    // Try to make relative to project root
    std::error_code ec;
    fs::path absPath = fs::weakly_canonical(m_ProjectRoot / sourcePath, ec);
    if (!ec) {
        fs::path relPath = fs::relative(absPath, m_ProjectRoot, ec);
        if (!ec) {
            std::string key = relPath.generic_string();
            for (char& c : key) {
                if (c == '\\') c = '/';
            }
            return key;
        }
    }
    
    return normalized;
}

fs::path BinaryAssetCache::SourceToCache(const std::string& sourcePath, AssetType type) const {
    std::string key = MakeCacheKey(sourcePath);
    
    // Determine subdirectory
    std::string subdir;
    switch (type) {
        case AssetType::Scene: subdir = "scenes"; break;
        case AssetType::Material: subdir = "materials"; break;
        case AssetType::Prefab: subdir = "prefabs"; break;
        case AssetType::Animation: subdir = "animations"; break;
        case AssetType::AnimatorController: subdir = "animators"; break;
        default: return {};
    }
    
    // Replace extension
    fs::path keyPath(key);
    std::string stem = keyPath.stem().string();
    
    // Include subdirectory structure to avoid collisions
    fs::path parentDir = keyPath.parent_path();
    std::string flatPath = parentDir.string();
    for (char& c : flatPath) {
        if (c == '/' || c == '\\') c = '_';
    }
    
    std::string binaryName = flatPath.empty() 
        ? stem + GetBinaryExtension(type)
        : flatPath + "_" + stem + GetBinaryExtension(type);
    
    return m_CacheRoot / subdir / binaryName;
}

void BinaryAssetCache::LoadManifest() {
    fs::path manifestPath = m_CacheRoot / "manifest.json";
    
    std::ifstream in(manifestPath);
    if (!in.is_open()) {
        return; // No manifest yet
    }
    
    try {
        json j;
        in >> j;
        
        bool migratedPaths = false;
        for (auto& [key, value] : j.items()) {
            CacheEntry entry;
            entry.binaryPath = value.value("binary", "");
            entry.sourceModTime = value.value("sourceModTime", 0ull);
            entry.binaryModTime = value.value("binaryModTime", 0ull);

            const AssetType type = GetAssetType(key);
            if (type != AssetType::Unknown) {
                const fs::path expectedBinaryPath = SourceToCache(key, type);
                if (!expectedBinaryPath.empty() && entry.binaryPath != expectedBinaryPath.string()) {
                    entry.binaryPath = expectedBinaryPath.string();
                    migratedPaths = true;
                }
            }

            m_Manifest[key] = entry;
        }
        if (migratedPaths) {
            SaveManifest();
        }
    } catch (const std::exception& e) {
        std::cerr << "[BinaryAssetCache] Failed to load manifest: " << e.what() << std::endl;
    }
}

void BinaryAssetCache::SaveManifest() {
    fs::path manifestPath = m_CacheRoot / "manifest.json";
    
    json j;
    for (const auto& [key, entry] : m_Manifest) {
        j[key] = {
            {"binary", entry.binaryPath},
            {"sourceModTime", entry.sourceModTime},
            {"binaryModTime", entry.binaryModTime}
        };
    }
    
    std::ofstream out(manifestPath, std::ios::trunc);
    if (out.is_open()) {
        out << j.dump(2);
    }
}

// ============================================================================
// EditorAssetResolver implementation
// ============================================================================

EditorAssetResolver::EditorAssetResolver() {
    // Register as global resolver
    Assets::SetResolver(this);
}

std::string EditorAssetResolver::ResolvePath(const std::string& virtualPath) const {
    switch (m_Mode) {
        case AssetLoadMode::Editor:
            // In editor mode, return source path as-is
            return virtualPath;
            
        case AssetLoadMode::PlayMode: {
            BinaryAssetCache::AssetType type = BinaryAssetCache::GetAssetType(virtualPath);
            if (type == BinaryAssetCache::AssetType::Unknown) {
                return virtualPath;
            }

            std::string binaryPath = GetBinaryPath(virtualPath);
            const bool binaryReady =
                !binaryPath.empty() &&
                std::filesystem::exists(binaryPath) &&
                BinaryAssetCache::Instance().IsCurrent(virtualPath);
            if (!binaryReady) {
                if (m_PlayModeBinaryOnly) {
                    std::cerr << "[EditorAssetResolver] Missing or stale binary file for: " << virtualPath
                              << " (expected " << binaryPath << ")" << std::endl;
                    return binaryPath;
                }
                return virtualPath;
            }
            return binaryPath;
        }
        
        case AssetLoadMode::Runtime:
            // In runtime, always use binary from VFS
            return GetBinaryPath(virtualPath);
    }
    
    return virtualPath;
}

bool EditorAssetResolver::AllowSourceFallback() const {
    if (m_Mode == AssetLoadMode::PlayMode && m_PlayModeBinaryOnly) {
        return false;
    }
    return m_Mode != AssetLoadMode::Runtime;
}

std::string EditorAssetResolver::GetBinaryPath(const std::string& sourcePath) const {
    return BinaryAssetCache::Instance().GetBinaryPath(sourcePath);
}

bool EditorAssetResolver::IsBinaryCurrent(const std::string& sourcePath) const {
    return BinaryAssetCache::Instance().IsCurrent(sourcePath);
}

ClaymoreGUID EditorAssetResolver::GetGUID(const std::string& path) const {
    return AssetLibrary::Instance().GetGUIDForPath(path);
}

std::string EditorAssetResolver::GetPathForGUID(const ClaymoreGUID& guid) const {
    return AssetLibrary::Instance().GetPathForGUID(guid);
}
