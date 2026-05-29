#pragma once
#include <imgui.h>
#include <string>
#include "core/ecs/Scene.h"
#include "core/ecs/Entity.h"

// A hierarchy panel embedded inside PrefabEditor; mirrors SceneHierarchyPanel behavior
// without owning its own window label. Context menus exclude "Save to Prefab" and
// prefab-open arrows.
class PrefabHierarchyPanel {
public:
    PrefabHierarchyPanel(Scene* scene, EntityID* selectedEntity)
        : m_Context(scene), m_SelectedEntity(selectedEntity) {}

    void SetContext(Scene* scene) { m_Context = scene; }
    void SetSelectedEntityPtr(EntityID* ptr) { m_SelectedEntity = ptr; }

    void OnImGuiRenderEmbedded();
    void ExpandTo(EntityID id) { m_ExpandTarget = id; }

private:
    void DrawHierarchyContents();
    void DrawEntityNode(const Entity& entity);
    void EnsureIconsLoaded();

private:
    Scene* m_Context = nullptr;
    EntityID* m_SelectedEntity = nullptr;
    EntityID m_PendingSelect = -1;
    EntityID m_RenamingEntity = -1;
    EntityID m_ExpandTarget = -1;
    char m_Filter[128]{};
    // Icons
    bool m_IconsLoaded = false;
    ImTextureID m_VisibleIcon{};
    ImTextureID m_NotVisibleIcon{};
};


