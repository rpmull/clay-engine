#pragma once
#include <string>
#include <memory>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include "core/ecs/Scene.h"
#include "EditorPanel.h"
#include "editor/ui/panels/ViewportPanel.h"
#include "editor/ui/panels/SceneHierarchyPanel.h"
#include "core/serialization/Serializer.h"
#include "core/assets/AssetReference.h"
#include "core/prefab/PrefabAsset.h"
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

// Simple panel that opens a secondary viewport to edit a single prefab
class PrefabEditorPanel : public EditorPanel {
public:
    explicit PrefabEditorPanel(const std::string& prefabPath, class UILayer* uiLayer);
    ~PrefabEditorPanel();

    // Draws ImGui window(s). Returns false when the panel is closed by the user.
    void OnImGuiRender();

    bool IsOpen() const { return m_IsOpen; }
    // Query whether this editor window is the active target; used to switch hierarchy context
    bool IsWindowFocusedOrHovered() const { return m_IsFocusedOrHovered; }
    // Accessors to expose scene/selection so UILayer can point panels at this editor when active
    Scene* GetScene() { return &m_Scene; }
    EntityID* GetSelectedEntityPtr() { return &m_SelectedEntity; }
    // Prefab path and focus request for UILayer dedup/focus behavior
    const std::string& GetPrefabPath() const { return m_PrefabPath; }
    // Root entity id of the prefab being edited (stable regardless of current selection)
    EntityID GetPrefabRootEntity() const { return m_PrefabRoot; }
    // Editor-only light entity (should be hidden from hierarchy)
    EntityID GetEditorLightEntity() const { return m_EditorLight; }
    // Convenience: save current prefab (always saves from root, overwriting existing file)
    bool SavePrefab();
    // Delta state queries for entity coloring in hierarchy
    bool IsEntityNew(const ClaymoreGUID& guid) const { return m_AddedGUIDs.count(guid.ToString()) > 0; }
    bool IsEntityModified(const ClaymoreGUID& guid) const { return m_ModifiedGUIDs.count(guid.ToString()) > 0; }
    // Check if entity is part of the baseline (should be blue in hierarchy)
    // In prefab editing mode, ALL entities are baseline UNLESS they're new/modified
    bool IsBaselineEntity(const ClaymoreGUID& guid) const {
        std::string key = guid.ToString();
        // Not new AND not modified = baseline (blue)
        return m_AddedGUIDs.count(key) == 0 && m_ModifiedGUIDs.count(key) == 0;
    }
    // Check if we're in prefab editing mode (for hierarchy to know to apply prefab coloring)
    bool IsInPrefabMode() const { return m_PrefabRoot != (EntityID)-1; }
    // Trigger delta recomputation (call before querying entity states)
    void EnsureDeltasComputed() { if (m_DeltasStale) ComputeDeltas(); }
    void MarkDeltasStale() { m_DeltasStale = true; }
    void RequestFocus() { m_FocusNextFrame = true; }
    bool IsDirty() const { return m_IsDirty; }
    void ClearDirty() { m_IsDirty = false; }
    
    // Play mode awareness - prefab editor should not compute deltas or save during play mode
    void SetPlayMode(bool playing) { m_InPlayMode = playing; }
    bool IsInPlayMode() const { return m_InPlayMode; }
    
    /// @brief Check if this prefab depends on the given model asset
    bool DependsOnModel(const std::string& modelPath, ClaymoreGUID modelGuid) const;
    
    /// @brief Refresh the prefab view when an underlying model asset changes
    /// This preserves user overrides while incorporating model changes
    void RefreshFromModelChange(const std::string& modelPath, ClaymoreGUID modelGuid);
    
private:
    // Helper to load prefab file into the internal scene
    bool LoadPrefab(const std::string& path);
    void ClearLoadedPrefabScene(bool preserveEditorLight = true);
    void EnsureEditorLighting();
    void RebuildBaseline();
    void ComputeDeltas();
    size_t GetEditableEntityCount() const;
    enum class StatusLevel { Info, Warning, Error };
    void SetStatusMessage(const std::string& message, StatusLevel level = StatusLevel::Info);
    void RefreshDiagnostics(const PrefabAsset* asset = nullptr);
    
    // Initialize camera to focus on prefab AABB bounds (or default 1x1 cube if no meshes)
    void InitializeCameraToBounds();
    
    // Handle asset change events from the event bus
    void OnAssetEvent(int event, const std::string& path, ClaymoreGUID guid);

private:
    std::string m_PrefabPath;
    bool m_IsOpen = true;
    bool m_Docked = false;
    mutable bool m_IsFocusedOrHovered = false;
    bool m_FocusNextFrame = false;
    bool m_IsDirty = false;
    bool m_InPlayMode = false;

    Scene m_Scene;
    EntityID m_SelectedEntity = -1;
    EntityID m_PrefabRoot = -1;
    EntityID m_EditorLight = -1;

    // Embedded viewport for editing
    ViewportPanel m_ViewportPanel;
    // Dedicated view id base for this prefab editor (isolated render context)
    uint16_t m_ViewIdBase = 0;

    class UILayer* m_UILayer;

    // Saved state baseline: snapshot of scene after loading (for non-model prefabs)
    std::unordered_set<std::string> m_BaselineGuids;
    std::unordered_map<std::string, std::string> m_BaselineNames;
    std::unordered_map<std::string, std::string> m_BaselinePaths;
    struct BaselineTransform { glm::vec3 pos; glm::vec3 rot; glm::vec3 scale; };
    std::unordered_map<std::string, BaselineTransform> m_BaselineTransforms;
    std::unordered_map<std::string, nlohmann::json> m_BaselineComponents;
    
    // Stable prefab asset GUIDs from disk (avoid changing on save)
    ClaymoreGUID m_PrefabAssetGuid;
    ClaymoreGUID m_PrefabRootGuid;
    // Computed per-frame deltas relative to baseline
    std::vector<std::string> m_AddedDescriptions;
    std::vector<std::string> m_RemovedDescriptions;
    std::vector<std::string> m_ModifiedDescriptions;
    // Entity GUIDs for coloring (added = green, modified = yellow)
    std::unordered_set<std::string> m_AddedGUIDs;
    std::unordered_set<std::string> m_ModifiedGUIDs;
    bool m_DeltasStale = true;
    uint64_t m_LastDeltaSceneRevision = 0;
    
    // Asset event subscription handle (for model hot reload)
    int m_AssetEventSubscription = 0;
    
    // Cached model GUIDs and normalized source paths that this prefab depends on
    std::unordered_set<uint64_t> m_DependentModelGuids;
    std::unordered_set<uint64_t> m_DependentModelAssetGuids;
    std::unordered_set<std::string> m_DependentModelPaths;
    
    // Per-instance frame counter for delta detection throttling
    // (Previously was static, causing shared state across all editors)
    int m_DeltaFrameCounter = 0;

    std::vector<std::string> m_DiagnosticErrors;
    std::vector<std::string> m_DiagnosticWarnings;
    std::string m_StatusText;
    StatusLevel m_StatusLevel = StatusLevel::Info;
    bool m_PendingCloseConfirmation = false;
};
