#pragma once
#include <string>
#include <vector>
#include <bgfx/bgfx.h>
#include <imgui.h>
#include <memory>
#include <filesystem>
#include <unordered_map>
#include <glm/glm.hpp>
#include "core/ecs/Scene.h"
#include "EditorPanel.h"
#include "core/serialization/Serializer.h"
#include "editor/pipeline/AssetLibrary.h"
#include "assets/ScriptableRegistry.h"

namespace cm { namespace animation { struct AvatarDefinition; } }

struct FileNode {
   std::string name;
   std::string path;
   bool isDirectory;
   std::vector<FileNode> children;
};

class UILayer; // Forward declaration
class Camera;  // Forward declaration

class ProjectPanel : public EditorPanel {
public:
   ProjectPanel(Scene* scene, UILayer* uiLayer);
   ~ProjectPanel();

   void OnImGuiRender();
   void LoadProject(const std::string& projectPath);

    const std::string& GetSelectedItemName() const { return m_SelectedItemName; }
    const std::string& GetSelectedItemPath() const { return m_SelectedItemPath; }
   bool HasSelection() const { return !m_SelectedItemPath.empty(); }
   std::string GetSelectedItemExtension() const {
        if (m_SelectedItemPath.empty()) return std::string();
        std::string ext = std::filesystem::path(m_SelectedItemPath).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        return ext;
    }
   void ClearSelection();

   // Scene and prefab operations
   void LoadSceneFile(const std::string& filepath);
   void CreatePrefabFromEntity(EntityID entityId, const std::string& prefabPath);
   
   // Navigation
   void NavigateTo(const std::string& folderPath);

   // Release all image thumbnails (bgfx textures) - must be called before bgfx shutdown
   void ReleaseAllImageThumbnails();

private:
   enum FilterBits : uint32_t {
       FilterScenes    = 1u << 0,
       FilterPrefabs   = 1u << 1,
       FilterMeshes    = 1u << 2,
       FilterMaterials = 1u << 3,
       FilterScripts   = 1u << 4,
       FilterTextures  = 1u << 5,
       FilterAnimations= 1u << 6
   };
   static constexpr uint32_t kDefaultFilterMask = FilterScenes | FilterPrefabs | FilterMeshes |
       FilterMaterials | FilterScripts | FilterTextures | FilterAnimations;

   static std::string ToLowerCopy(std::string value);
   static uint32_t DetermineFilterMask(const std::filesystem::path& p, bool isDirectory);

   void DrawFolderTree(FileNode& node);
   void DrawFileList(const std::string& folderPath);
    void RebuildFileListCache(const std::string& folderPath);
    bool ShouldRebuildFileListCache(const std::string& folderPath);
   void RebuildVisibleFileListCache(const std::string& folderPath);
   bool ShouldRebuildVisibleFileListCache(const std::string& folderPath) const;
   void CreateMaterialAt(const std::string& materialPath, const std::string& shaderPath);
   void CreateAnimationControllerOverrideAt(const std::string& overridePath);
   void DrawSelectedInspector();
   void DrawScenePreviewInspector(const std::string& scenePath);
   FileNode BuildFileTree(const std::string& path);
   void PasteInto(const std::string& destFolder);
   void DrawFilterChips();
   void RenderFilenameLabel(const std::string& label, float textWrapWidth) const;
   void BeginRename(const std::string& fullPath, const std::string& fileName);
   void DrawRenamePopup();
   void DrawModelSnapshotPopup();
   void UpdateModelSnapshot();
   bool StartModelSnapshotCapture();
   void DrawPrefabSnapshotPopup();
   void UpdatePrefabSnapshot();
   bool StartPrefabSnapshotCapture();
   bool CapturePrefabSnapshot();  // Internal: captures after particle warmup
   bool LoadHumanoidAvatarForModel(const std::filesystem::path& modelPath,
                                   cm::animation::AvatarDefinition& outAvatar) const;
   bool ReimportModelAsset(const std::filesystem::directory_entry& entry,
                           bool slicedHumanoid,
                           const cm::animation::AvatarDefinition* avatarOverride);
   bool ReimportModelAsArmor(const std::filesystem::directory_entry& entry);
   
   // Animation generation utilities
   void GenerateFlippedAnimation(const std::string& animPath);
   void GenerateReversedAnimation(const std::string& animPath);
   
   // Noise texture generation
   void CreateNoiseTexture(const std::string& destFolder, const std::string& noiseType, int width = 256, int height = 256);
   void GeneratePerlinNoise(uint8_t* pixels, int width, int height);
   void GeneratePixelNoise(uint8_t* pixels, int width, int height);
   void GenerateSimplexNoise(uint8_t* pixels, int width, int height);
   void GenerateValueNoise(uint8_t* pixels, int width, int height);
   void GenerateVoronoiNoise(uint8_t* pixels, int width, int height);
   bool SavePNG(const std::string& filepath, const uint8_t* pixels, int width, int height, int channels = 4);
   
   // Background context menu for empty space
   void DrawBackgroundContextMenu();
   
   // Helper functions for file operations
   bool IsSceneFile(const std::string& filepath) const;
   bool IsPrefabFile(const std::string& filepath) const;
    ImTextureID GetFileIconForPath(const std::string& path) const;
    void EnsureExtraIconsLoaded() const;
    static AssetType GuessAssetTypeFromPath(const std::string& path);
    bool IsImageAsset(const std::string& path) const;
    ImTextureID GetImageThumbnail(const std::string& path, ImVec2* nativeSize = nullptr) const;
    ImVec2 ProbeImageDimensions(const std::string& path) const;
    static std::string NormalizePathKey(const std::string& path);

private:
   std::string m_ProjectPath;
   FileNode m_ProjectRoot;
   std::string m_CurrentFolder;
   std::string m_SearchQuery;
   std::string m_SearchLower;
    std::string m_SelectedItemName;
    std::string m_SelectedItemPath;
    uint32_t m_FileFilterMask = kDefaultFilterMask;
    char m_SearchBuffer[128] = {0};
    double m_LastClickTime = 0.0;
    std::string m_LastClickedItem;

   ImTextureID m_FolderIcon;
   ImTextureID m_FileIcon;
    // Additional icons
    mutable bool m_IconsLoaded = false;
    mutable ImTextureID m_Icon3DModel{};
    mutable ImTextureID m_IconImage{};
    mutable ImTextureID m_IconMaterial{};
    mutable ImTextureID m_IconScene{};
    mutable ImTextureID m_IconPrefab{};
    mutable ImTextureID m_IconAnimation{};
    mutable ImTextureID m_IconCSharp{};
    mutable ImTextureID m_IconAnimController{};

   UILayer* m_UILayer = nullptr; // Non-owning pointer back to UI layer
   // Explorer clipboard and rename state
   std::string m_PendingRenamePath;
   std::string m_RenameBuffer;
   bool m_RenamePopupJustOpened = false;
   bool m_RenamePopupRequested = false;
   std::string m_ClipboardPath;
   bool m_ClipboardIsCut = false;
   // Pending delete state
   std::string m_PendingDeletePath;
   // Deferred selection: only apply selection on mouse release if no drag started
   std::string m_PendingSelectionPath;
   std::string m_PendingSelectionName;
   bool m_DragStartedThisClick = false;
    struct ImageThumbnailEntry {
        bgfx::TextureHandle handle = BGFX_INVALID_HANDLE;
        ImVec2 dimensions = ImVec2(0.0f, 0.0f);
        std::filesystem::file_time_type timestamp{};
        double lastValidationTime = -1.0;
    };
   mutable std::unordered_map<std::string, ImageThumbnailEntry> m_ImageThumbnails;
   
   // Scene file preview cache (to avoid parsing JSON every frame)
   struct ScenePreviewCache {
       std::string path;
       std::string filename;
       int entityCount = 0;
       std::vector<std::string> referencedAssets;
       bool valid = false;
   };
   mutable ScenePreviewCache m_ScenePreviewCache;

   struct ModelSnapshotState {
       bool popupRequested = false;
       bool popupJustOpened = false;
       bool pendingReadback = false;
       std::string modelPath;
       std::string outputPath;
       glm::vec3 rotation = glm::vec3(0.0f);
       glm::vec3 scale = glm::vec3(1.0f);
       int resolution[2] = { 128, 128 };
       bool overrideResolution = false;
       uint32_t clearColor = 0x00000000;
       uint8_t clearKey[3] = { 0u, 0u, 0u };
       uint32_t pendingFrame = 0;
       uint32_t pendingStartFrame = 0;
       uint32_t width = 128;
       uint32_t height = 128;
       std::vector<uint8_t> readbackBuffer;
       bgfx::TextureHandle readbackTexture = BGFX_INVALID_HANDLE;
       std::unique_ptr<Scene> scene;
       std::unique_ptr<Camera> camera;
       EntityID modelRoot = INVALID_ENTITY_ID;
       uint16_t viewIdBase = 200;
   };
   ModelSnapshotState m_ModelSnapshot;

   struct PrefabSnapshotState {
       bool popupRequested = false;
       bool popupJustOpened = false;
       bool pendingReadback = false;
       bool simulatingParticles = false;       // True while waiting for particles to warm up
       std::string prefabPath;
       std::string outputPath;
       glm::vec3 rotation = glm::vec3(0.0f);
       glm::vec3 scale = glm::vec3(1.0f);
       int resolution[2] = { 128, 128 };
       bool overrideResolution = false;
       uint32_t clearColor = 0x00000000;
       uint8_t clearKey[3] = { 0u, 0u, 0u };
       uint32_t pendingFrame = 0;
       uint32_t pendingStartFrame = 0;
       uint32_t width = 128;
       uint32_t height = 128;
       std::vector<uint8_t> readbackBuffer;
       bgfx::TextureHandle readbackTexture = BGFX_INVALID_HANDLE;
       std::unique_ptr<Scene> scene;
       std::unique_ptr<Camera> camera;
       EntityID prefabRoot = INVALID_ENTITY_ID;
       uint16_t viewIdBase = 210;              // Different view ID range from model snapshot
       bool hasParticleEmitters = false;       // Whether prefab contains particle systems
       float particleWarmupTime = 0.0f;        // Accumulated warmup time
       float particleTargetWarmup = 0.0f;      // Time to wait for peak emission
   };
   PrefabSnapshotState m_PrefabSnapshot;

   struct CachedFileEntry {
       std::string fullPath;
       std::string name;
       std::string lowerName;
       std::string extLower;
       bool isDir = false;
       uint32_t filterMask = 0;
       bool hidden = false;
   };
   std::vector<CachedFileEntry> m_FileListCache;
   std::string m_FileListCacheFolder;
   std::filesystem::file_time_type m_FileListCacheTimestamp{};
   double m_FileListCacheBuildTime = 0.0;
   bool m_FileListCacheDirty = true;
   std::vector<size_t> m_VisibleFileListCache;
   std::string m_VisibleFileListCacheFolder;
   std::string m_VisibleFileListCacheSearch;
   uint32_t m_VisibleFileListCacheMask = 0;
   bool m_VisibleFileListCacheDirty = true;
   static constexpr double kFileListRefreshInterval = 0.5;
};
