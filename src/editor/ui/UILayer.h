#pragma once
#include "panels/ProjectPanel.h"
#include "panels/InspectorPanel.h"
#include "panels/ViewportPanel.h"
#include "panels/SceneHierarchyPanel.h"
#include "panels/ToolbarPanel.h"
#include "panels/MenuBarPanel.h"
#include "panels/ConsolePanel.h"
#include "panels/ProfilerPanel.h"
#include "panels/PrefabEditorPanel.h"
#include "panels/AnimationControllerPanel.h"
#include "editor/animation/AnimationTimelinePanel.h"
#include "editor/animation/AnimationGraphPanel.h"
#include "panels/AvatarBuilderPanel.h"
#include "core/ecs/Scene.h"
#include "panels/ScriptRegistryPanel.h"
#include "panels/AssetRegistryPanel.h"
#include "panels/PrefabEditorPanel.h"
#include "panels/CodeEditorPanel.h"
#include "panels/NodeGraphPanel.h"
#include "editor/panels/ProjectSettingsPanel.h"
#include "editor/tools/SerializerSanityWindow.h"
#include "editor/tools/TintMaskEditorWindow.h"
#include "editor/tools/WorldGenerationPanel.h"
#include "editor/tools/TerrainEvolutionPanel.h"
#include "editor/tools/RiverCutterPanel.h"
#include "editor/tools/SplineToolPanel.h"
#include "editor/tools/SoftbodyPainter.h"
#include "editor/tools/SplatmapGeneratorPanel.h"
#include "editor/tools/ResourceLayerPanel.h"
#include "editor/tools/TextureCleanupPanel.h"
#include "editor/tools/UIPrefabLayoutDesignerPanel.h"
#include "editor/ui/panels/IconGeneratorPanel.h"
#include "editor/nodegraph/ShaderGraphPanel.h"
#include "panels/DialogueEditorPanel.h"
#include "panels/QuestEditorPanel.h"
#include "platform/EditorActionRegistry.h"
#include "platform/EditorCommandPalette.h"
#include "platform/EditorContextMenuRegistry.h"
#include "platform/EditorNotifications.h"
#include "platform/EditorPreferences.h"

#include <vector>
#include <memory>
#include <string>
 
extern std::vector<std::string> g_RegisteredScriptNames;

class IAssetResolver;
class RuntimeAssetResolver;

class UILayer {
public:
    UILayer();
    ~UILayer();

    void OnUIRender();
    void OnAttach();

    void LoadProject(std::string path);
    void RequestLayoutReset();
    void OpenCommandPalette();
    bool ExecuteEditorAction(const std::string& id);
    bool SetEditorActionChecked(const std::string& id, bool checked);
    const editorui::EditorActionDefinition* FindEditorAction(const std::string& id) const;
    std::vector<const editorui::EditorActionDefinition*> GetEditorActionsForCategory(const std::string& category) const;
    editorui::EditorContextMenuRegistry& GetEditorContextMenus() { return m_ContextMenuRegistry; }
    const editorui::EditorContextMenuRegistry& GetEditorContextMenus() const { return m_ContextMenuRegistry; }
    void QueueNotification(editorui::EditorNotificationLevel level, const std::string& message, float lifetimeSeconds = 4.5f);

    Scene& GetScene() { return m_Scene; }
    class AnimationInspectorPanel* GetAnimationInspector() { return m_AnimationInspector.get(); }

    bool GetUseFastInstantiation() const { return m_UseFastInstantiation; }
    void SetUseFastInstantiation(bool value) { m_UseFastInstantiation = value; }

    // Camera controls (forwarded to viewport)
    void HandleCameraControls() { m_ViewportPanel.HandleCameraControls(); }
    void StampEditorCameraMetadata(Scene& scene);
    void ResetEditorCamera();

    // Picking interface (used by Application loop)
    bool HasPickRequest() const { return m_ViewportPanel.HasPickRequest(); }
    std::pair<float, float> GetNormalizedPickCoords() const { return m_ViewportPanel.GetNormalizedPickCoords(); }
    void ClearPickRequest() { m_ViewportPanel.ClearPickRequest(); }

    // Entity selection
    void SetSelectedEntity(EntityID id) { m_SelectedEntity = id; }
    EntityID GetSelectedEntity() const { return m_SelectedEntity; }
    EntityID GetHoveredEntity() const { return m_ViewportPanel.GetHoveredEntity(); }
    void ExpandHierarchyTo(EntityID id);

    void TogglePlayMode();
    void EndRuntimePreview();
    void StopExternalRuntimePreview();
    bool IsExternalRuntimePreviewActive() const { return m_ExternalRuntimePreviewActive; }

    // Prefab editor management
    void OpenPrefabEditor(const std::string& prefabPath);
    bool AnyPrefabViewportFocused() const;
    // Returns the currently focused/hovered prefab editor, or nullptr if none
    PrefabEditorPanel* GetActivePrefabEditor();
    // Returns the "sticky" prefab editor (persists until explicitly closed)
    PrefabEditorPanel* GetStickyPrefabEditor() { return m_StickyPrefabEditor; }
    // Exit prefab editing mode (clears sticky editor)
    void ExitPrefabEditMode();
    // Returns all open prefab editors (for iteration)
    const std::vector<std::unique_ptr<PrefabEditorPanel>>& GetPrefabEditors() const { return m_PrefabEditors; }
    // Code editor management
    void OpenCodeEditor(const std::string& filePath);
    // Access to Project panel
    ProjectPanel& GetProjectPanel() { return m_ProjectPanel; }
    AnimTimelinePanel& GetTimelinePanel() { return m_AnimTimelinePanel; }
    ProfilerPanel& GetProfilerPanel() { return m_ProfilerPanel; }
    AnimationControllerPanel& GetAnimControllerPanel() { return m_AnimCtrlPanel; }
    AnimationGraphPanel& GetAnimationGraphPanel() { return m_AnimGraphPanel; }
    NodeGraphPanel& GetNodeGraphPanel() { return m_NodeGraphPanel; }
    WorldGenerationPanel& GetWorldGenerationPanel() { return m_WorldGenPanel; }
    TerrainEvolutionPanel& GetTerrainEvolutionPanel() { return m_TerrainEvolutionPanel; }
    RiverCutterPanel& GetRiverCutterPanel() { return m_RiverCutterPanel; }
    SplatmapGeneratorPanel& GetSplatmapGeneratorPanel() { return m_SplatmapGeneratorPanel; }
    cm::resourcelayer::ResourceLayerPanel& GetResourceLayerPanel() { return m_ResourceLayerPanel; }
    UIPrefabLayoutDesignerPanel& GetUIPrefabLayoutDesignerPanel() { return m_UIPrefabLayoutDesignerPanel; }
    TextureCleanupPanel& GetTextureCleanupPanel() { return m_TextureCleanupPanel; }
    IconGeneratorPanel& GetIconGeneratorPanel() { return m_IconGeneratorPanel; }
    shadergraph::ShaderGraphPanel& GetShaderGraphPanel() { return m_ShaderGraphPanel; }
    DialogueEditorPanel& GetDialogueEditorPanel() { return m_DialogueEditorPanel; }
    QuestEditorPanel& GetQuestEditorPanel() { return m_QuestEditorPanel; }
    bool IsScriptRegistryVisible() const { return m_ShowScriptRegistry; }
    void SetScriptRegistryVisible(bool show) { m_ShowScriptRegistry = show; }
    bool IsAssetRegistryVisible() const { return m_ShowAssetRegistry; }
    void SetAssetRegistryVisible(bool show) { m_ShowAssetRegistry = show; }
    
    // Deferred scene loading
    void DeferSceneLoad(const std::string& filepath);
    void ProcessDeferredSceneLoad();
    void SetCurrentScenePath(const std::string& path) { m_CurrentScenePath = path; }
    const std::string& GetCurrentScenePath() const { return m_CurrentScenePath; }
    // Project Settings
    void ShowProjectSettings(bool show) { m_ShowProjectSettings = show; }
    void AdjustUIScale(float delta);
    bool SaveCurrentScene(bool saveAs = false);
    bool SaveActiveDocument();
    bool SaveActiveDocumentAs();
    void NewScene();
    void PromptLoadScene();
    void PromptProjectSettings() { m_ShowProjectSettings = true; }
    
    // Play mode and viewport state queries
    bool IsPlayMode() const { return m_PlayMode; }
    bool IsRuntimeStatsPanelOpen() const { return m_ShowRuntimeStatsPanel; }
    bool IsViewportHovered() const { return m_ViewportPanel.IsViewportHovered(); }
    
public:
    void FocusConsoleNextFrame() { m_FocusConsoleNextFrame = true; }
	SceneHierarchyPanel& GetSceneHierarchyPanel() { return m_SceneHierarchyPanel; }
    // Check if any prefab editor is currently focused (for panels to adjust behavior)
    bool IsPrefabEditModeActive() const;
    
    // Blocking overlay helpers
    void BeginBlockingOverlay(const std::string& label, float progress = -1.0f);
    void EndBlockingOverlay();

    bool HasTerrainSelection();
    bool HasNavMeshSelection();
    bool HasSplineSelection();
    bool HasSoftbodySelection();

    // Async Play start (non-blocking overlay)
    void RequestBeginPlayAsync(bool binaryOnly, bool useTempPak, PlayWindowMode windowMode);
    void ProcessBeginPlayAsync();

    // Open Serializer Sanity window
    void OpenSerializerSanity() { m_SerializerSanityWindow.Open(); }
    // Open Tint Mask editor
    void OpenTintMaskEditor(const std::string& imagePath) { m_TintMaskEditor.OpenForImage(imagePath); }
    // Run serialization sanity checks automatically when enabled
    void RunSerializerSanityIfEnabled(Scene& scene) { if (m_SerializerSanityWindow.IsRunOnSaveEnabled()) m_SerializerSanityWindow.RunAllChecks(scene); }
    void OpenAnimatorController(const std::string& path);
    void OpenShaderGraph(const std::string& path);

private:
    void BeginDockspace();
    void CreateDebugCubeEntity();
    void SyncSelectionContext();
    void ApplyEditorCameraState(Scene& scene);
    void UpdateCachedSceneTitle();
    void RefreshShaderGraphDependencies(const std::string& shaderGraphPath);
    void RegisterEditorActions();
    void RegisterEditorContextMenus();
    void ApplyEditorWorkspacePreferences(bool loadLayout);
    void SyncEditorPersistence();
    void CaptureViewportPreferences();
    float GetBaseEditorFontSize() const;
    uint64_t ComputePersistedWindowStateHash() const;
    uint64_t ComputeViewportSettingsHash() const;
    uint64_t ComputeActionUsageHash() const;
    PrefabEditorPanel* GetFocusedPrefabEditor();
    CodeEditorPanel* GetFocusedCodeEditor();
    bool IsSpecializedDocumentFocused() const;

    void CreateDefaultLight();
    void RenderBlockingOverlay();
    void EnforceDefaultActiveTabs();
    void PollExternalRuntimePreview();
    bool LaunchExternalRuntimePreview(const std::string& runtimeExePath,
                                      const std::string& contentRootPath,
                                      const std::string& pakPath,
                                      PlayWindowMode windowMode);

public:
    ImGuiID GetMainDockspaceID() const { return m_MainDockspaceID; }

private:
    Scene m_Scene;
    std::unique_ptr<class AnimationInspectorPanel> m_AnimationInspector;
    EntityID m_SelectedEntity = -1;
    EntityID m_PreviousSelectedEntity = -1;

    ProjectPanel m_ProjectPanel;
    InspectorPanel m_InspectorPanel;
    ViewportPanel m_ViewportPanel;
    SceneHierarchyPanel m_SceneHierarchyPanel;
    ToolbarPanel m_ToolbarPanel;
    MenuBarPanel m_MenuBarPanel;
    ConsolePanel m_ConsolePanel;
    ScriptRegistryPanel m_ScriptPanel;
    AssetRegistryPanel m_AssetRegistryPanel;
    AnimTimelinePanel m_AnimTimelinePanel;
    AnimationGraphPanel m_AnimGraphPanel;
    AnimationControllerPanel m_AnimCtrlPanel;
	ProfilerPanel m_ProfilerPanel;
    NodeGraphPanel m_NodeGraphPanel;
    AvatarBuilderPanel m_AvatarBuilderPanel;
    ProjectSettingsPanel m_ProjectSettingsPanel;
    SerializerSanityWindow m_SerializerSanityWindow;
    TintMaskEditorWindow m_TintMaskEditor;
    WorldGenerationPanel m_WorldGenPanel;
    TerrainEvolutionPanel m_TerrainEvolutionPanel;
    RiverCutterPanel m_RiverCutterPanel;
    SplineToolPanel m_SplineToolPanel;
    SoftbodyPainter m_SoftbodyPainter;
    SplatmapGeneratorPanel m_SplatmapGeneratorPanel;
    cm::resourcelayer::ResourceLayerPanel m_ResourceLayerPanel;
    TextureCleanupPanel m_TextureCleanupPanel;
    IconGeneratorPanel m_IconGeneratorPanel;
    UIPrefabLayoutDesignerPanel m_UIPrefabLayoutDesignerPanel;
    shadergraph::ShaderGraphPanel m_ShaderGraphPanel;
    DialogueEditorPanel m_DialogueEditorPanel;
    QuestEditorPanel m_QuestEditorPanel;
    std::vector<std::unique_ptr<PrefabEditorPanel>> m_PrefabEditors;
    std::vector<std::unique_ptr<CodeEditorPanel>> m_CodeEditors;
    PrefabEditorPanel* m_StickyPrefabEditor = nullptr; // The "active" prefab editor until explicitly closed

    // Overlay state
    bool m_BlockingOverlayActive = false;
    std::string m_BlockingOverlayLabel;
    float m_BlockingOverlayProgress = -1.0f;
    // Async play toggle state
    bool m_BeginPlayRequested = false;
    enum class PlayModeStartPhase {
        Idle = 0,
        Prewarming = 1
    };
    struct PlayModeStartOptions {
        bool binaryOnly = false;
        bool useTempPak = false;
        PlayWindowMode windowMode = PlayWindowMode::Editor;
    };
    PlayModeStartOptions m_PendingPlayOptions{};
    PlayModeStartPhase m_PlayModeStartPhase = PlayModeStartPhase::Idle;
    std::shared_ptr<Scene> m_PendingPlayBaseScene;
    bool m_RuntimePreviewActive = false;
    bool m_ExternalRuntimePreviewActive = false;
    void* m_ExternalRuntimeProcessHandle = nullptr;
    void* m_ExternalRuntimeThreadHandle = nullptr;
    bool m_PrePlayDiskFallbackAllowed = true;
    std::string m_PrePlayPakPath;
    class IAssetResolver* m_PrePlayResolver = nullptr;
    std::unique_ptr<class RuntimeAssetResolver> m_RuntimePreviewResolver;
    std::string m_PrePlayResourceManifestPath;
    bool m_RestoreResourceManifest = false;

    // Dockspace state
    ImGuiID m_MainDockspaceID = 0;
    bool m_LayoutInitialized = false;
    bool m_ResetLayoutRequested = false;

    // Sticky routing state
    Scene* m_ActiveEditorScene = nullptr;
    EntityID* m_ActiveSelectedEntityPtr = nullptr;
    enum class InspectorSelectionSource { None, Entity, Asset };
    InspectorSelectionSource m_SelectionSource = InspectorSelectionSource::None;
    std::string m_LastAssetSelection;
    EntityID m_LastEntitySelectionTracked = -1;

    // Misc
    bool m_FocusConsoleNextFrame = false;
    bool m_PlayMode = false;
    bool m_ShowRuntimeStatsPanel = true;
    bool m_ShowProjectSettings = false;
    bool m_ProjectSettingsOpen = false;
    bool m_EditorLightingOverride = false;
    bool m_ShowScriptRegistry = false;
    bool m_ShowAssetRegistry = false;
    editorui::EditorActionRegistry m_ActionRegistry;
    editorui::EditorCommandPalette m_CommandPalette;
    editorui::EditorContextMenuRegistry m_ContextMenuRegistry;
    editorui::EditorNotifications m_Notifications;
    editorui::EditorPreferences m_EditorPreferences;
    uint64_t m_LastPersistedWindowStateHash = 0;
    uint64_t m_LastViewportSettingsHash = 0;
    uint64_t m_LastActionUsageHash = 0;

    // One-off docking for viewport when using dynamic window label
    bool m_DockViewportOnce = false;
    bool m_EnforcedTabsOnce = false;

    // Deferred load
    bool m_HasDeferredSceneLoad = false;
    std::string m_DeferredScenePath;
    std::string m_CurrentScenePath;
    std::string m_CachedSceneTitle;
    std::string m_CachedScenePath;
    bool m_CachedSceneDirty = false;

    bool m_UseFastInstantiation = true;
};
