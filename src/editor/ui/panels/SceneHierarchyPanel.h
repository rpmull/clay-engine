#pragma once
#include <imgui.h>
#include <string>
#include "core/ecs/Scene.h"
#include "EditorPanel.h"

// Forward declaration
class UILayer;
class PrefabEditorPanel;

class SceneHierarchyPanel : public EditorPanel {
public:
   SceneHierarchyPanel(Scene* scene, EntityID* selectedEntity);
   ~SceneHierarchyPanel() = default;

   void OnImGuiRender();
    // Render only the contents without opening a separate window (for embedding)
    void OnImGuiRenderEmbedded();
    // Allow switching the selected entity pointer at runtime (to follow active scene)
    void SetSelectedEntityPtr(EntityID* ptr) { m_SelectedEntity = ptr; }
    void SetSceneDisplayName(const std::string& name) { m_SceneDisplayName = name; }
    // Programmatically expand the hierarchy to reveal a specific entity
    void ExpandTo(EntityID id);
    // Callback for opening prefab editor from hierarchy (set by UILayer)
    void SetOpenPrefabCallback(std::function<void(const std::string&)> cb) { m_OnOpenPrefab = std::move(cb); }
    // Set UILayer context for prefab mode detection
    void SetUILayerContext(UILayer* layer) { m_UILayer = layer; }

private:

   void RefreshPrefabFrameContext();
   void DrawEntityNode(EntityID entityId, int depth, bool inheritedModelNode, bool inheritedPrefabNode);
    void EnsureIconsLoaded();
    void DrawHierarchyContents();
    // Draw special prefab editing mode header/chip
    void DrawPrefabModeChip();
   EntityID DuplicateEntityAsSibling(EntityID sourceId, PrefabEditorPanel* prefabEditor, const ClaymoreGUID& prefabGuid);
   EntityID* m_SelectedEntity;
   // Target to expand to next frame (-1 = none)
   EntityID m_ExpandTarget = -1;
    // Icons for visibility toggles
    bool m_IconsLoaded = false;
    ImTextureID m_VisibleIcon{};
    ImTextureID m_NotVisibleIcon{};
    // Rename state
    EntityID m_RenamingEntity = -1;
    char m_RenameBuffer[128] = {0};
    // Selection handling that ignores drag begin
    EntityID m_PendingSelect = -1;
    // Filter
    char m_Filter[128] = {0};
    // Open prefab editor callback
    std::function<void(const std::string&)> m_OnOpenPrefab;
    std::string m_SceneDisplayName;
    // UILayer context for prefab mode detection
    UILayer* m_UILayer = nullptr;
    PrefabEditorPanel* m_FramePrefabEditor = nullptr;
    bool m_FramePrefabModeActive = false;
    // Prefab icon for prefab instances in hierarchy
    ImTextureID m_PrefabIcon{};
    // Entity to hide from hierarchy (e.g., editor-only light in prefab mode)
    EntityID m_HiddenEditorLightID = -1;

   // Clipboard (copy/paste)
   static EntityID s_ClipboardEntity;
   static Scene* s_ClipboardScene;
   };
