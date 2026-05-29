#pragma once

#include <string>
#include <vector>
#include <queue>
#include <memory>
#include <filesystem>
#include <cstdint>
#include <bgfx/bgfx.h>
#include <glm/glm.hpp>
#include "EditorPanel.h"

class Scene;
class Camera;
class UILayer;

/**
 * @brief Panel for batch generating item icons from 3D models.
 * 
 * Scans assets/models and generates 128x128 PNG icons saved to resources/icons.
 * Icons are automatically registered with the ResourceManifest for runtime access.
 */
class IconGeneratorPanel : public EditorPanel {
public:
    IconGeneratorPanel(UILayer* uiLayer);
    ~IconGeneratorPanel();

    void OnImGuiRender();
    void Open() { m_IsOpen = true; }
    void SetOpen(bool open) { m_IsOpen = open; }
    bool IsOpen() const { return m_IsOpen; }

private:
    struct ModelEntry {
        std::string path;           // Full filesystem path
        std::string name;           // Model name (stem)
        std::string iconPath;       // Expected output path
        bool hasIcon = false;       // Whether icon already exists
        bool selected = true;       // Whether to generate for this model
    };

    struct SnapshotState {
        bool active = false;
        std::string modelPath;
        std::string outputPath;
        std::unique_ptr<Scene> scene;
        std::unique_ptr<Camera> camera;
        EntityID modelRoot = -1;
        
        uint32_t pendingFrame = 0;
        uint32_t pendingStartFrame = 0;
        bool pendingReadback = false;
        
        bgfx::TextureHandle readbackTexture = BGFX_INVALID_HANDLE;
        std::vector<uint8_t> readbackBuffer;
        
        uint32_t width = 128;
        uint32_t height = 128;
        uint32_t clearColor = 0xFF00FFFF;  // Magenta key
        uint8_t clearKey[3] = { 0xFF, 0x00, 0xFF };
        
        uint16_t viewIdBase = 220;  // Different range from other snapshots
    };

    void ScanModels();
    void StartBatchGeneration();
    void ProcessNextModel();
    void UpdateSnapshot();
    bool StartSnapshot(const std::string& modelPath, const std::string& outputPath);
    void FinalizeSnapshot();
    void CancelGeneration();
    
    void EnsureResourceIconsFolder();
    
    bool IsModelExtension(const std::string& ext) const;

private:
    UILayer* m_UILayer = nullptr;
    bool m_IsOpen = false;
    
    // Model list
    std::vector<ModelEntry> m_Models;
    bool m_ModelsScanned = false;
    bool m_ShowOnlyMissing = true;
    
    // Batch processing state
    std::queue<size_t> m_PendingQueue;
    bool m_BatchActive = false;
    size_t m_TotalToProcess = 0;
    size_t m_ProcessedCount = 0;
    std::string m_CurrentModelName;
    std::vector<std::string> m_GeneratedIcons;
    std::vector<std::string> m_FailedModels;
    
    // Snapshot state
    SnapshotState m_Snapshot;
    
    // Settings
    int m_Resolution[2] = { 128, 128 };
    glm::vec3 m_DefaultRotation = glm::vec3(0.0f);
    glm::vec3 m_DefaultScale = glm::vec3(1.0f);
};
