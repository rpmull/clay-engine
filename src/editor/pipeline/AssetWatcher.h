#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <filesystem>
#include <chrono>
#include <mutex>

class AssetPipeline;

class AssetWatcher {
public:
    AssetWatcher(AssetPipeline& pipeline, const std::string& rootPath);
    ~AssetWatcher();

    void Start();
    void Stop();

    void SetRootPath(const std::string& path) { 
        std::lock_guard<std::mutex> lock(m_RootPathMutex);
        m_RootPath = path; 
    }

private:
    void WatchLoop();
    bool HasFileChanged(const std::string& path, std::filesystem::file_time_type lastWriteTime);
    
    // Thread-safe getter for root path
    std::string GetRootPathSafe() {
        std::lock_guard<std::mutex> lock(m_RootPathMutex);
        return m_RootPath;
    }

    AssetPipeline& m_Pipeline;
    std::string m_RootPath;
    mutable std::mutex m_RootPathMutex;
    std::atomic<bool> m_Running;
    std::thread m_Thread;

    std::unordered_map<std::string, std::filesystem::file_time_type> m_FileTimestamps;
    std::mutex m_TimestampMutex;

    // Poll interval reduced from 2s to 500ms for more responsive hot reload
    static constexpr auto POLL_INTERVAL = std::chrono::milliseconds(500);
};
