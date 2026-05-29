#include "SpriteLoader.h"
#include "core/vfs/VirtualFS.h"
#include "core/vfs/FileSystem.h"
#include <stb_image.h>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <chrono>

namespace particles
{
    using SpriteClock = std::chrono::steady_clock;

    struct SpriteCacheEntry
    {
        ps::EmitterSpriteHandle handle{ uint16_t{UINT16_MAX} };
        uint32_t refCount = 0;
    };
    
    // Sprite cache to prevent atlas exhaustion when multiple prefabs use the same sprite
    // Key: normalized file path, Value: cached sprite handle + ref count
    static std::unordered_map<std::string, SpriteCacheEntry> s_SpriteCache;
    static std::unordered_map<uint16_t, std::string> s_SpriteHandleToPath;
    static std::unordered_map<std::string, std::chrono::time_point<SpriteClock>> s_FailedSpriteCache;
    static std::mutex s_SpriteCacheMutex;
    static constexpr auto kFailedSpriteRetryDelay = std::chrono::milliseconds(2000);

    static std::string TryToAssetRelative(const std::string& inputPath)
    {
        if (inputPath.empty()) return {};

        std::string normalized = inputPath;
        std::replace(normalized.begin(), normalized.end(), '\\', '/');

        std::string lowered = normalized;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        static constexpr const char* kMarkers[] = {
            "/assets/",
            "assets/",
            "/resources/",
            "resources/"
        };

        for (const char* marker : kMarkers) {
            const std::string needle(marker);
            const size_t pos = lowered.find(needle);
            if (pos == std::string::npos) continue;

            // Keep the relative root token (assets/... or resources/...) as the cache key.
            if (needle[0] == '/') {
                if (pos + 1 < normalized.size()) {
                    return normalized.substr(pos + 1);
                }
            } else {
                return normalized.substr(pos);
            }
        }

        return {};
    }
    
    static std::string NormalizeSpriteKey(const std::string& path)
    {
        if (path.empty()) return {};
        try {
            std::string normalized = IVirtualFS::NormalizePath(path);
            std::filesystem::path p(normalized);
            if (p.is_absolute()) {
                const auto& root = FileSystem::Instance().GetProjectRoot();
                if (!root.empty()) {
                    std::error_code ec;
                    std::filesystem::path abs = std::filesystem::weakly_canonical(p, ec);
                    std::filesystem::path rootAbs = std::filesystem::weakly_canonical(root, ec);
                    if (!ec) {
                        std::filesystem::path rel = std::filesystem::relative(abs, rootAbs, ec);
                        const std::string relStr = rel.generic_string();
                        if (!ec && (relStr.empty() || relStr.rfind("..", 0) != 0)) {
                            normalized = rel.generic_string();
                        }
                    }
                }
            }

            // Fallback: normalize absolute extraction/cache paths to logical asset-relative keys.
            // This prevents the same sprite file from being re-cached under different temp roots.
            std::string assetRelative = TryToAssetRelative(normalized);
            if (!assetRelative.empty()) {
                normalized = assetRelative;
            }

            return IVirtualFS::NormalizePathLowercase(normalized);
        } catch (...) {
            std::string normalized = path;
            std::replace(normalized.begin(), normalized.end(), '\\', '/');
            std::string assetRelative = TryToAssetRelative(normalized);
            if (!assetRelative.empty()) {
                normalized = assetRelative;
            }
            std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return normalized;
        }
    }
    
    static void FlipImageY(stbi_uc* data, int width, int height, int channels)
    {
        if (!data || width <= 0 || height <= 1 || channels <= 0) return;
        const int rowBytes = width * channels;
        std::vector<stbi_uc> temp(rowBytes);
        for (int y = 0; y < height / 2; ++y) {
            stbi_uc* row = data + y * rowBytes;
            stbi_uc* opp = data + (height - 1 - y) * rowBytes;
            std::memcpy(temp.data(), row, rowBytes);
            std::memcpy(row, opp, rowBytes);
            std::memcpy(opp, temp.data(), rowBytes);
        }
    }
    
    static ps::EmitterSpriteHandle LoadSpriteInternal(const std::string& path, bool flipY)
    {
        if (path.empty()) {
            return { UINT16_MAX };
        }
        
        // IMPORTANT: Avoid stbi_set_flip_vertically_on_load (global state).
        // We manually flip the loaded pixel data when requested.
        int width, height, channels;
        stbi_uc* data = nullptr;
        
        // Try VFS first (for PAK file access at runtime)
        std::vector<uint8_t> fileData;
        if (VFS::Get() && VFS::Get()->ReadFile(path, fileData)) {
            data = stbi_load_from_memory(fileData.data(), static_cast<int>(fileData.size()), &width, &height, &channels, 4);
        }
        // Fallback to FileSystem (handles VFS delegation + disk)
        if (!data && FileSystem::Instance().ReadFile(path, fileData)) {
            data = stbi_load_from_memory(fileData.data(), static_cast<int>(fileData.size()), &width, &height, &channels, 4);
        }
#ifndef CLAYMORE_RUNTIME
        // Direct disk fallback for editor only
        if (!data) {
            data = stbi_load(path.c_str(), &width, &height, &channels, 4);
        }
#endif
        
        if (!data)
        {
            std::cerr << "[SpriteLoader] Failed to load image: " << path << std::endl;
            return { UINT16_MAX };
        }
        
        if (flipY) {
            FlipImageY(data, width, height, 4);
        }

        ps::EmitterSpriteHandle sprite = ps::createSprite((uint16_t)width, (uint16_t)height, data);
        stbi_image_free(data);
        if (!ps::isValid(sprite))
        {
            std::cerr << "[SpriteLoader] Sprite allocation failed for '" << path
                      << "' (" << width << "x" << height << "). Used sprites="
                      << ps::GetSpriteSlotCount() << "/" << ps::GetSpriteSlotCapacity()
                      << "." << std::endl;
        }
        
        return sprite;
    }

    static bool SpriteKeyMatchesPath(const std::string& cacheKey, const std::string& normalizedPath)
    {
        if (normalizedPath.empty()) return false;
        if (cacheKey == normalizedPath) return true;
        if (cacheKey.size() <= normalizedPath.size()) return false;
        return cacheKey.compare(0, normalizedPath.size(), normalizedPath) == 0
            && cacheKey[normalizedPath.size()] == '|';
    }
    
    ps::EmitterSpriteHandle AcquireSprite(const std::string& path, bool flipY)
    {
        if (path.empty()) return { UINT16_MAX };
        std::string key = NormalizeSpriteKey(path);
        key += flipY ? "|flipY" : "|noFlip";
        const auto now = SpriteClock::now();
        
        // Check cache first (thread-safe)
        {
            std::lock_guard<std::mutex> lock(s_SpriteCacheMutex);
            auto it = s_SpriteCache.find(key);
            if (it != s_SpriteCache.end() && ps::isValid(it->second.handle)) {
                ++it->second.refCount;
                s_FailedSpriteCache.erase(key);
                return it->second.handle;
            }
            auto failIt = s_FailedSpriteCache.find(key);
            if (failIt != s_FailedSpriteCache.end() && now < failIt->second) {
                return { UINT16_MAX };
            }
        }
        
        ps::EmitterSpriteHandle sprite = LoadSpriteInternal(path, flipY);
        
        // Cache the sprite if successfully created
        if (ps::isValid(sprite)) {
            std::lock_guard<std::mutex> lock(s_SpriteCacheMutex);
            auto it = s_SpriteCache.find(key);
            if (it != s_SpriteCache.end() && ps::isValid(it->second.handle)) {
                ++it->second.refCount;
                s_FailedSpriteCache.erase(key);
                ps::destroySprite(sprite);
                return it->second.handle;
            }

            SpriteCacheEntry& entry = s_SpriteCache[key];
            entry.handle = sprite;
            entry.refCount = 1;
            s_SpriteHandleToPath[sprite.idx] = key;
            s_FailedSpriteCache.erase(key);
        } else {
            std::lock_guard<std::mutex> lock(s_SpriteCacheMutex);
            s_FailedSpriteCache[key] = now + kFailedSpriteRetryDelay;
        }
        
        return sprite;
    }
    
    void ReleaseSprite(ps::EmitterSpriteHandle handle)
    {
        if (!ps::isValid(handle)) return;
        std::lock_guard<std::mutex> lock(s_SpriteCacheMutex);
        auto itPath = s_SpriteHandleToPath.find(handle.idx);
        if (itPath == s_SpriteHandleToPath.end()) {
            return;
        }
        auto it = s_SpriteCache.find(itPath->second);
        if (it == s_SpriteCache.end()) {
            s_SpriteHandleToPath.erase(itPath);
            return;
        }
        
        if (it->second.refCount > 0) {
            --it->second.refCount;
        }
        // IMPORTANT: Do NOT destroy sprite when refCount hits 0.
        // The atlas packer does not reclaim space, so destroying/recreating fragments the atlas
        // and eventually makes it appear "full" even with repeated reuse of the same sprites.
        // Sprites are destroyed only by ClearSpriteCache().
        if (it->second.refCount == 0 && !ps::isValid(it->second.handle)) {
            s_SpriteCache.erase(it);
            s_SpriteHandleToPath.erase(itPath);
        }
    }

    void InvalidateSpriteCache(const std::string& path)
    {
        if (path.empty()) return;

        const std::string normalizedPath = NormalizeSpriteKey(path);
        if (normalizedPath.empty()) return;

        std::lock_guard<std::mutex> lock(s_SpriteCacheMutex);
        for (auto it = s_SpriteCache.begin(); it != s_SpriteCache.end(); ) {
            if (!SpriteKeyMatchesPath(it->first, normalizedPath)) {
                ++it;
                continue;
            }

            if (ps::isValid(it->second.handle)) {
                s_SpriteHandleToPath.erase(it->second.handle.idx);
                ps::destroySprite(it->second.handle);
            }

            s_FailedSpriteCache.erase(it->first);
            it = s_SpriteCache.erase(it);
        }
    }
    
    ps::EmitterSpriteHandle LoadSprite(const std::string& path, bool flipY)
    {
        return AcquireSprite(path, flipY);
    }
    
    void ClearSpriteCache()
    {
        std::lock_guard<std::mutex> lock(s_SpriteCacheMutex);
        
        for (auto& kv : s_SpriteCache) {
            if (ps::isValid(kv.second.handle)) {
                ps::destroySprite(kv.second.handle);
            }
        }
        
        s_SpriteCache.clear();
        s_SpriteHandleToPath.clear();
        s_FailedSpriteCache.clear();
    }
}
