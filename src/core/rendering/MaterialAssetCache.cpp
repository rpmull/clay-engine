#include "MaterialAssetCache.h"

#include "core/rendering/MaterialAsset.h"
#include "core/rendering/MaterialManager.h"
#include "core/rendering/PBRMaterial.h"
#include "core/vfs/VirtualFS.h"
#include "editor/Project.h"

#include <unordered_map>
#include <mutex>
#include <filesystem>

namespace
{
    std::string NormalizePath(const std::string& in)
    {
        if (in.empty()) return {};
        std::string path = IVirtualFS::NormalizePath(in);
        std::string vpath = VFS::StripToKnownPrefix(path);
        if (!vpath.empty()) return vpath;

        try
        {
            std::filesystem::path p(path);
            if (p.is_absolute())
            {
                auto proj = Project::GetProjectDirectory();
                if (!proj.empty())
                {
                    std::error_code ec;
                    auto rel = std::filesystem::relative(p, proj, ec);
                    if (!ec)
                    {
                        std::string relStr = IVirtualFS::NormalizePath(rel.string());
                        // Only use if it doesn't go up directories
                        if (relStr.find("../") == std::string::npos) {
                            std::string relVpath = VFS::StripToKnownPrefix(relStr);
                            return relVpath.empty() ? relStr : relVpath;
                        }
                    }
                }
            }
        }
        catch (...) {}
        return path;
    }

    std::shared_ptr<Material> CreateMaterialFromPath(const std::string& normalizedPath)
    {
        MaterialAssetDesc desc;
        std::string absPath = normalizedPath;
        if (normalizedPath.find("assets/") == 0)
        {
            absPath = (std::filesystem::path(Project::GetProjectDirectory()) / normalizedPath).string();
        }
        if (!LoadMaterialAsset(absPath, desc))
        {
            return nullptr;
        }
        return CreateMaterialFromAsset(desc);
    }

    std::mutex s_mutex;
    std::unordered_map<std::string, std::weak_ptr<Material>> s_cache;
}

namespace MaterialAssetCache
{

std::shared_ptr<Material> Acquire(const std::string& assetPath)
{
    std::string key = NormalizePath(assetPath);
    if (key.empty()) return nullptr;

    {
        std::lock_guard<std::mutex> guard(s_mutex);
        auto it = s_cache.find(key);
        if (it != s_cache.end())
        {
            if (auto existing = it->second.lock())
            {
                return existing;
            }
        }
    }

    std::shared_ptr<Material> material = CreateMaterialFromPath(key);
    if (!material)
    {
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> guard(s_mutex);
        s_cache[key] = material;
    }
    return material;
}

std::shared_ptr<Material> Reload(const std::string& assetPath)
{
    Invalidate(assetPath);
    return Acquire(assetPath);
}

void Invalidate(const std::string& assetPath)
{
    std::string key = NormalizePath(assetPath);
    if (key.empty()) return;
    std::lock_guard<std::mutex> guard(s_mutex);
    s_cache.erase(key);
}

void Clear()
{
    std::lock_guard<std::mutex> guard(s_mutex);
    s_cache.clear();
}

} // namespace MaterialAssetCache
