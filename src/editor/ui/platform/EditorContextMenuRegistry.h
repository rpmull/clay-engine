#pragma once

#include "core/ecs/Scene.h"

#include <functional>
#include <string>
#include <vector>

namespace editorui {

struct ProjectItemContext {
    std::string Path;
    std::string ParentFolder;
    std::string ExtensionLower;
    bool IsDirectory = false;
};

struct ProjectBackgroundContext {
    std::string FolderPath;
};

struct HierarchyEntityContext {
    Scene* Context = nullptr;
    EntityID Entity = INVALID_ENTITY_ID;
    bool IsPrefabMode = false;
};

struct HierarchyBackgroundContext {
    Scene* Context = nullptr;
    bool IsPrefabMode = false;
};

class EditorContextMenuRegistry {
public:
    using ProjectItemRenderer = std::function<bool(const ProjectItemContext&)>;
    using ProjectBackgroundRenderer = std::function<bool(const ProjectBackgroundContext&)>;
    using HierarchyEntityRenderer = std::function<bool(const HierarchyEntityContext&)>;
    using HierarchyBackgroundRenderer = std::function<bool(const HierarchyBackgroundContext&)>;

    void Clear();

    void RegisterProjectItem(std::string id, int sortOrder, ProjectItemRenderer renderer);
    void RegisterProjectBackground(std::string id, int sortOrder, ProjectBackgroundRenderer renderer);
    void RegisterHierarchyEntity(std::string id, int sortOrder, HierarchyEntityRenderer renderer);
    void RegisterHierarchyBackground(std::string id, int sortOrder, HierarchyBackgroundRenderer renderer);

    bool RenderProjectItem(const ProjectItemContext& context) const;
    bool RenderProjectBackground(const ProjectBackgroundContext& context) const;
    bool RenderHierarchyEntity(const HierarchyEntityContext& context) const;
    bool RenderHierarchyBackground(const HierarchyBackgroundContext& context) const;

private:
    template <typename TRenderer>
    struct Entry {
        std::string Id;
        int SortOrder = 0;
        TRenderer Renderer;
    };

    template <typename TEntry, typename TRenderer>
    void Register(std::vector<TEntry>& entries, std::string id, int sortOrder, TRenderer renderer);

    template <typename TEntry, typename TContext>
    bool Render(const std::vector<TEntry>& entries, const TContext& context) const;

private:
    std::vector<Entry<ProjectItemRenderer>> m_ProjectItemEntries;
    std::vector<Entry<ProjectBackgroundRenderer>> m_ProjectBackgroundEntries;
    std::vector<Entry<HierarchyEntityRenderer>> m_HierarchyEntityEntries;
    std::vector<Entry<HierarchyBackgroundRenderer>> m_HierarchyBackgroundEntries;
};

} // namespace editorui
