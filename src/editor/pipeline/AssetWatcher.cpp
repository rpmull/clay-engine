#include "AssetWatcher.h"
#include "AssetPipeline.h"
#include "AssetLibrary.h"
#include "AssetRegistry.h"
#include "BinaryAssetCache.h"
#include "core/assets/AssetMetadata.h"
#include "core/resources/ResourceManifest.h"
#include "editor/Project.h"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <unordered_set>

namespace fs = std::filesystem;

AssetWatcher::AssetWatcher(AssetPipeline& pipeline, const std::string& rootPath)
    : m_Pipeline(pipeline), m_RootPath(rootPath), m_Running(false) {
}

AssetWatcher::~AssetWatcher() {
    Stop();
}

void AssetWatcher::Start() {
    if (m_Running) return;
    m_Running = true;
    m_Thread = std::thread(&AssetWatcher::WatchLoop, this);
    std::cout << "[AssetWatcher] Started for: " << m_RootPath << std::endl;
}

void AssetWatcher::Stop() {
    m_Running = false;
    if (m_Thread.joinable()) {
        m_Thread.join();
        std::cout << "[AssetWatcher] Stopped." << std::endl;
    }
}

void AssetWatcher::WatchLoop() {
    std::cout << "[AssetWatcher] Watching: " << m_RootPath << std::endl;
    
    // Track files that existed last iteration for deletion detection
    std::unordered_set<std::string> previousFiles;

    while (m_Running) {
        try {
            // Take a snapshot of the root path at the start of each iteration
            // to avoid data races if SetRootPath is called from another thread
            std::string rootPathSnapshot = GetRootPathSafe();
            if (rootPathSnapshot.empty() || !fs::exists(rootPathSnapshot)) {
                std::this_thread::sleep_for(POLL_INTERVAL);
                continue;
            }
            
            // Periodically try to discover resources folder if not yet found
            if (!ResourceManifest::Get().HasResourcesFolder()) {
                if (ResourceManifest::Get().TryDiscoverResourcesFolder(rootPathSnapshot)) {
                    // Resources folder was just discovered, do initial scan
                    ResourceManifest::Get().Scan();
                    std::cout << "[AssetWatcher] Discovered resources folder - initial scan complete" << std::endl;
                }
            }
            
            // Track current files for deletion detection
            std::unordered_set<std::string> currentFiles;
            
            for (auto& entry : fs::recursive_directory_iterator(rootPathSnapshot)) {
                if (!entry.is_regular_file()) continue;

                std::string filePath = entry.path().string();
                std::string ext = entry.path().extension().string();

                if (!m_Pipeline.IsSupportedAsset(ext)) continue;
                
                // Track this file for deletion detection
                currentFiles.insert(filePath);

                auto lastWriteTime = fs::last_write_time(entry);

                if (HasFileChanged(filePath, lastWriteTime)) {
                    m_Pipeline.EnqueueAssetImport(filePath);
                    
                    // Invalidate binary cache for this asset so it gets rebuilt
                    // when entering play mode or exporting
                    auto assetType = BinaryAssetCache::GetAssetType(filePath);
                    if (assetType != BinaryAssetCache::AssetType::Unknown) {
                        BinaryAssetCache::Instance().Invalidate(filePath);
                    }
                    // Re-emit binaries immediately for animations/controllers to avoid stale playmode data.
                    if (assetType == BinaryAssetCache::AssetType::Animation ||
                        assetType == BinaryAssetCache::AssetType::AnimatorController) {
                        BinaryAssetCache::Instance().EnsureBinary(filePath);
                    }
                    
                    // Notify ResourceManifest if this is a resource file
                    if (ResourceManifest::Get().IsResourcePath(filePath)) {
                        ResourceManifest::Get().RefreshFile(filePath);
                    }
                }

                // Opportunistically refresh GUID?path registration using sidecar .meta (handles renames/moves)
                try {
                    // Sidecar .meta next to asset
                    fs::path metaPath = entry.path(); metaPath += ".meta";
                    if (fs::exists(metaPath)) {
                        // Parse meta
                        std::ifstream mi(metaPath.string());
                        nlohmann::json mj; mi >> mj; mi.close();
                        AssetMetadata meta = mj.get<AssetMetadata>();
                        if (meta.guid.high != 0 || meta.guid.low != 0) {
                            // Build virtual path relative to project root, normalize to forward slashes
                            std::error_code ec;
                            fs::path rel = fs::relative(entry.path(), Project::GetProjectDirectory(), ec);
                            std::string vpath = (ec ? entry.path().string() : rel.string());
                            std::replace(vpath.begin(), vpath.end(), '\\', '/');
                            // Ensure it starts with assets/
                            size_t pos = vpath.find("assets/");
                            if (pos != std::string::npos) vpath = vpath.substr(pos);
                            // Infer AssetType from extension
                            std::string lowerExt = ext; std::transform(lowerExt.begin(), lowerExt.end(), lowerExt.begin(), ::tolower);
                            AssetType at = AssetType::Mesh;
                            if (lowerExt == ".fbx" || lowerExt == ".gltf" || lowerExt == ".glb" || lowerExt == ".obj") at = AssetType::Mesh;
                            else if (lowerExt == ".png" || lowerExt == ".jpg" || lowerExt == ".jpeg" || lowerExt == ".tga") at = AssetType::Texture;
                            else if (lowerExt == ".prefab" || (lowerExt == ".json" && vpath.find("/assets/prefabs/") != std::string::npos)) at = AssetType::Prefab;
                            else if (lowerExt == ".ngraph") at = AssetType::NodeGraph;
                            else if (lowerExt == ".ttf" || lowerExt == ".otf") at = AssetType::Font;
                            else if (lowerExt == ".cs") at = AssetType::Script;
                            // Register mapping and alias (RegisterAsset now dedupes silently)
                            AssetLibrary::Instance().RegisterAsset(AssetReference(meta.guid, 0, static_cast<int32_t>(at)), at, vpath, entry.path().filename().string());
                            AssetLibrary::Instance().RegisterPathAlias(meta.guid, filePath);
                        }
                    }
                } catch(...) { /* silent; watcher continues */ }
            }
            
            // Detect deleted files (files that were in previousFiles but not in currentFiles)
            for (const auto& prevFile : previousFiles) {
                if (currentFiles.find(prevFile) == currentFiles.end()) {
                    // File was deleted
                    // Notify ResourceManifest if this was a resource file
                    if (ResourceManifest::Get().IsResourcePath(prevFile)) {
                        ResourceManifest::Get().RemoveFile(prevFile);
                    }
                    
                    // Remove from timestamps
                    {
                        std::lock_guard<std::mutex> lock(m_TimestampMutex);
                        m_FileTimestamps.erase(prevFile);
                    }
                }
            }
            
            // Update previous files for next iteration
            previousFiles = std::move(currentFiles);
        }
        catch (const std::exception& e) {
            std::cerr << "[AssetWatcher] Error: " << e.what() << std::endl;
        }

        std::this_thread::sleep_for(POLL_INTERVAL);
    }
}

bool AssetWatcher::HasFileChanged(const std::string& path, fs::file_time_type lastWriteTime) {
    std::lock_guard<std::mutex> lock(m_TimestampMutex);

    auto it = m_FileTimestamps.find(path);
    if (it == m_FileTimestamps.end()) {
        // New file detected
        m_FileTimestamps[path] = lastWriteTime;
        return true;
    }

    if (it->second != lastWriteTime) {
        // Modified file detected
        it->second = lastWriteTime;
        return true;
    }

    return false;
}
