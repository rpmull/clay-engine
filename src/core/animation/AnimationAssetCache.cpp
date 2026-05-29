#include "core/animation/AnimationAssetCache.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <unordered_set>
#include <unordered_map>

#include "core/animation/AnimationAsset.h"
#include "core/animation/AnimationSerializer.h"
#include "core/jobs/Jobs.h"
#include "editor/Project.h"

namespace cm {
namespace animation {

namespace {

using CacheClock = std::chrono::steady_clock;

std::mutex g_cacheMutex;
struct CacheEntry {
    std::shared_ptr<AnimationAsset> asset;
    CacheClock::time_point lastUse = CacheClock::now();
};
std::unordered_map<std::string, CacheEntry> g_assetCache;
std::unordered_set<std::string> g_pendingLoads;
CacheClock::time_point g_lastPurge = CacheClock::now();

// Normalize paths so callers can use either absolute or project-relative strings.
static std::string NormalizeKey(const std::string& rawPath)
{
    if (rawPath.empty()) return {};
    try {
        std::filesystem::path p(rawPath);
        if (p.is_relative()) {
            const auto& projectRoot = Project::GetProjectDirectory();
            if (!projectRoot.empty()) {
                p = projectRoot / p;
            }
        }
        std::error_code ec;
        auto canonical = std::filesystem::weakly_canonical(p, ec);
        if (!ec) p = canonical;
        return p.generic_string();
    } catch (...) {
        std::filesystem::path fallback(rawPath);
        return fallback.generic_string();
    }
}

// Prune expired weak_ptr entries periodically to keep the map compact.
static void PurgeExpiredEntriesLocked()
{
    constexpr auto kPurgeInterval = std::chrono::seconds(5);
    const auto now = CacheClock::now();
    if ((now - g_lastPurge) < kPurgeInterval) return;

    // Keep frequently used assets alive to avoid hitches when swapping controllers.
    // Evict least-recently used entries after some idle time or when cache grows too large.
    constexpr auto kMaxIdleAge = std::chrono::seconds(120);
    constexpr size_t kMaxEntries = 512;

    for (auto it = g_assetCache.begin(); it != g_assetCache.end();) {
        const bool tooOld = (now - it->second.lastUse) > kMaxIdleAge;
        const bool overCap = g_assetCache.size() > kMaxEntries;
        if (tooOld && overCap) it = g_assetCache.erase(it);
        else ++it;
    }
    g_lastPurge = now;
}

static bool HasSkeletalTracks(const AnimationAsset& asset)
{
    for (const auto& track : asset.tracks) {
        if (!track) continue;
        if (track->type == TrackType::Bone || track->type == TrackType::Avatar) return true;
    }
    return false;
}

static std::shared_ptr<AnimationAsset> LoadAnimationAssetShared(const std::string& path, bool allowLegacyFallback)
{
    AnimationAsset asset = LoadAnimationAsset(path);
    if (allowLegacyFallback && !HasSkeletalTracks(asset)) {
        AnimationClip legacy = LoadAnimationClip(path);
        if (!legacy.BoneTracks.empty() || !legacy.HumanoidTracks.empty()) {
            asset = WrapLegacyClipAsAsset(legacy);
        }
    }
    return std::make_shared<AnimationAsset>(std::move(asset));
}

static std::shared_ptr<AnimationAsset> StoreLoadedAsset(const std::string& key,
                                                        std::shared_ptr<AnimationAsset> asset)
{
    if (!asset) return nullptr;
    if (key.empty()) return asset;

    std::lock_guard<std::mutex> lock(g_cacheMutex);
    PurgeExpiredEntriesLocked();

    auto it = g_assetCache.find(key);
    if (it != g_assetCache.end() && it->second.asset) {
        it->second.lastUse = CacheClock::now();
        return it->second.asset;
    }

    CacheEntry entry;
    entry.asset = std::move(asset);
    entry.lastUse = CacheClock::now();
    auto [insertedIt, _] = g_assetCache.emplace(key, std::move(entry));
    return insertedIt->second.asset;
}

} // namespace

std::shared_ptr<AnimationAsset> LoadAnimationAssetCached(const std::string& path, bool allowLegacyFallback)
{
    if (path.empty()) return nullptr;
    const std::string key = NormalizeKey(path);
    if (!key.empty()) {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        PurgeExpiredEntriesLocked();
        if (auto it = g_assetCache.find(key); it != g_assetCache.end()) {
            it->second.lastUse = CacheClock::now();
            return it->second.asset;
        }
    }

    auto shared = LoadAnimationAssetShared(path, allowLegacyFallback);
    return StoreLoadedAsset(key, std::move(shared));
}

std::shared_ptr<AnimationAsset> TryGetAnimationAssetCached(const std::string& path)
{
    if (path.empty()) return nullptr;
    const std::string key = NormalizeKey(path);
    if (key.empty()) return nullptr;

    std::lock_guard<std::mutex> lock(g_cacheMutex);
    PurgeExpiredEntriesLocked();
    auto it = g_assetCache.find(key);
    if (it == g_assetCache.end()) {
        return nullptr;
    }
    it->second.lastUse = CacheClock::now();
    return it->second.asset;
}

void RequestAnimationAssetPreload(const std::string& path, bool allowLegacyFallback)
{
    if (path.empty()) return;
    if (!cm::g_JobSystem) {
        LoadAnimationAssetCached(path, allowLegacyFallback);
        return;
    }

    const std::string key = NormalizeKey(path);
    if (key.empty()) {
        LoadAnimationAssetCached(path, allowLegacyFallback);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        PurgeExpiredEntriesLocked();
        if (auto it = g_assetCache.find(key); it != g_assetCache.end() && it->second.asset) {
            it->second.lastUse = CacheClock::now();
            return;
        }
        if (!g_pendingLoads.insert(key).second) {
            return;
        }
    }

    const std::string loadPath = path;
    auto loadTask = [key, loadPath, allowLegacyFallback]() {
        auto loaded = LoadAnimationAssetShared(loadPath, allowLegacyFallback);
        {
            std::lock_guard<std::mutex> lock(g_cacheMutex);
            PurgeExpiredEntriesLocked();
            auto it = g_assetCache.find(key);
            if (it == g_assetCache.end()) {
                CacheEntry entry;
                entry.asset = std::move(loaded);
                entry.lastUse = CacheClock::now();
                g_assetCache.emplace(key, std::move(entry));
            } else {
                it->second.lastUse = CacheClock::now();
            }
            g_pendingLoads.erase(key);
        }
    };

    if (!Jobs().Enqueue(loadTask, JobSystem::Priority::Low)) {
        loadTask();
    }
}

void InvalidateAnimationAssetCache(const std::string& path)
{
    const std::string key = NormalizeKey(path);
    if (key.empty()) return;
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    g_assetCache.erase(key);
    g_pendingLoads.erase(key);
}

void ClearAnimationAssetCache()
{
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    g_assetCache.clear();
    g_pendingLoads.clear();
}

} // namespace animation
} // namespace cm


