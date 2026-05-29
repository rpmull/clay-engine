#pragma once
#include <string>
#include <queue>
#include <mutex>
#include <functional>
#include <vector>
#include "core/assets/AssetMetadata.h"
#include "AssetRegistry.h"
#include "core/rendering/ShaderManager.h"
#include <bgfx/bgfx.h>
#include <deque>
#include <unordered_set>

#include "ModelImportCache.h"
#include "ShaderImporter.h"

// ---------------------------
// GPU Upload Job Struct
// ---------------------------
struct PendingGPUUpload {
    enum class Type { Texture, Model, Shader };
    Type type;

    std::string sourcePath;

    // Texture
    std::vector<uint8_t> pixelData;
    int width = 0, height = 0;
    bgfx::TextureFormat::Enum format = bgfx::TextureFormat::RGBA8;

    // Model
    std::vector<float> vertices;
    std::vector<uint16_t> indices;

    // Shader
    std::string compiledShaderPath;
    ShaderType shaderType;

    // Upload callback
    std::function<void()> Upload;
};

class AssetPipeline {
public:
    struct MainThreadBudgetConfig {
        bool Enabled = true;
        double MaxMainThreadMs = 1.0;
        size_t MaxQueuedTasksPerFrame = 256;
        size_t MaxGpuUploadsPerFrame = 128;
    };

    struct MainThreadBudgetStats {
        uint64_t ImportedAssets = 0;
        uint64_t ExecutedMainThreadTasks = 0;
        uint64_t ExecutedGpuUploads = 0;
        uint64_t DeferredMainThreadTasks = 0;
        uint64_t DeferredGpuUploads = 0;
        double ElapsedMs = 0.0;
    };

	static AssetPipeline& Instance() {
		static AssetPipeline instance;
		return instance;
	}

    AssetPipeline() = default;

    // File scanning and import queue
    void ScanProject(const std::string& rootPath);
    void EnqueueAssetImport(const std::string& path);
    const std::vector<std::string>& GetLastScanList() const { return m_LastScanList; }
    // After a full reimport, fix up scene/prefab GUID references by resolving by name or path
    void FixupAssetReferencesByName(const std::string& projectRoot);
    // Editor convenience: drain all pending imports and tasks synchronously (blocking)
    void ProcessAllBlocking();

    // Main-thread execution
    void ProcessMainThreadTasks();
    void ProcessGPUUploads();
    void EnqueueMainThreadTask(std::function<void()> task);
    void EnqueueGPUUpload(PendingGPUUpload&& task);

    // -------- Model Import Queue (editor-only) --------
    struct ImportRequest {
        std::string sourcePath;       // dropped .fbx/.gltf/.glb/.obj
        std::string preferredVPath;   // suggested virtual path base (e.g., assets/models)
        std::function<void(const BuiltModelPaths&)> onReady; // main-thread callback
    };
    void EnqueueModelImport(const ImportRequest& req);

    // Asset importing
    // Set force=true to bypass metadata hash checks (e.g., user-triggered reimport)
    void ImportAsset(const std::string& path, bool force = false);
    void ImportModel(const std::string& path);
    // Hot-swap entities in the active scene that reference this model path/GUID
    void HotSwapModelInScene(const std::string& modelPath);
    // Hot-swap prefab instances in the active scene that reference this prefab path
    void HotSwapPrefabInScene(const std::string& prefabPath);
    void ImportTexture(const std::string& path);
    void ImportShader(const std::string& path);
    void ImportMaterial(const std::string& path);

    void ImportScript(const std::string& path, bool forceRebuild = false);

    // CPU-side texture pre-process for async
    void ImportTextureCPU(const std::string& path);

    // Utility helpers
    bool IsSupportedAsset(const std::string& ext) const;
    std::string DetermineType(const std::string& ext);
    std::string ComputeHash(const std::string& path) const;
    std::string ComputeFileHash(const std::string& path);
    std::string GetCurrentTimestamp() const;

    void CheckAndCompileScriptsAtStartup();
    // Force a rebuild of GameScripts.dll regardless of timestamps. Returns true on success.
    bool ForceRebuildScripts();
    // Rebuild all prefab binaries (.prefabb) from their source prefabs.
    void RebuildAllPrefabBinaries();

    const AssetMetadata* GetMetadata(const std::string& path) const {
        return AssetRegistry::Instance().GetMetadata(path);
    }

public:
    bool AreScriptsCompiled() const { return m_ScriptsCompiled; }
    void SetScriptsCompiled(bool success) { m_ScriptsCompiled = success; }
    bool HasAnyScripts() const { return m_HasScripts; }
    void SetMainThreadBudgetConfig(const MainThreadBudgetConfig& cfg) { m_MainThreadBudgetConfig = cfg; }
    const MainThreadBudgetConfig& GetMainThreadBudgetConfig() const { return m_MainThreadBudgetConfig; }
    const MainThreadBudgetStats& GetLastMainThreadBudgetStats() const { return m_LastBudgetStats; }

private:
    bool m_ScriptsCompiled = true;
    bool m_HasScripts = false;
    // Queues
    std::queue<std::string> m_ImportQueue;
    std::mutex m_QueueMutex;

    std::queue<std::function<void()>> m_MainThreadTasks;
    std::mutex m_MainThreadQueueMutex;

    std::queue<PendingGPUUpload> m_GPUUploadQueue;
    std::mutex m_GPUQueueMutex;

    // Debug: snapshot of assets collected by last ScanProject
    std::vector<std::string> m_LastScanList;

    // (deprecated) model-specific main-thread queue removed; use EnqueueMainThreadTask instead
    MainThreadBudgetConfig m_MainThreadBudgetConfig{};
    MainThreadBudgetStats m_LastBudgetStats{};
};
