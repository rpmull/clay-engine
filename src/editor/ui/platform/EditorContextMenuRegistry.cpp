#include "EditorContextMenuRegistry.h"

#include <algorithm>
#include <utility>

namespace editorui {

void EditorContextMenuRegistry::Clear()
{
    m_ProjectItemEntries.clear();
    m_ProjectBackgroundEntries.clear();
    m_HierarchyEntityEntries.clear();
    m_HierarchyBackgroundEntries.clear();
}

void EditorContextMenuRegistry::RegisterProjectItem(std::string id, int sortOrder, ProjectItemRenderer renderer)
{
    Register(m_ProjectItemEntries, std::move(id), sortOrder, std::move(renderer));
}

void EditorContextMenuRegistry::RegisterProjectBackground(std::string id, int sortOrder, ProjectBackgroundRenderer renderer)
{
    Register(m_ProjectBackgroundEntries, std::move(id), sortOrder, std::move(renderer));
}

void EditorContextMenuRegistry::RegisterHierarchyEntity(std::string id, int sortOrder, HierarchyEntityRenderer renderer)
{
    Register(m_HierarchyEntityEntries, std::move(id), sortOrder, std::move(renderer));
}

void EditorContextMenuRegistry::RegisterHierarchyBackground(std::string id, int sortOrder, HierarchyBackgroundRenderer renderer)
{
    Register(m_HierarchyBackgroundEntries, std::move(id), sortOrder, std::move(renderer));
}

bool EditorContextMenuRegistry::RenderProjectItem(const ProjectItemContext& context) const
{
    return Render(m_ProjectItemEntries, context);
}

bool EditorContextMenuRegistry::RenderProjectBackground(const ProjectBackgroundContext& context) const
{
    return Render(m_ProjectBackgroundEntries, context);
}

bool EditorContextMenuRegistry::RenderHierarchyEntity(const HierarchyEntityContext& context) const
{
    return Render(m_HierarchyEntityEntries, context);
}

bool EditorContextMenuRegistry::RenderHierarchyBackground(const HierarchyBackgroundContext& context) const
{
    return Render(m_HierarchyBackgroundEntries, context);
}

template <typename TEntry, typename TRenderer>
void EditorContextMenuRegistry::Register(std::vector<TEntry>& entries, std::string id, int sortOrder, TRenderer renderer)
{
    if (id.empty() || !renderer) {
        return;
    }

    auto it = std::find_if(entries.begin(), entries.end(), [&](const TEntry& entry) {
        return entry.Id == id;
    });

    TEntry entry;
    entry.Id = std::move(id);
    entry.SortOrder = sortOrder;
    entry.Renderer = std::move(renderer);

    if (it != entries.end()) {
        *it = std::move(entry);
    } else {
        entries.push_back(std::move(entry));
    }

    std::sort(entries.begin(), entries.end(), [](const TEntry& a, const TEntry& b) {
        if (a.SortOrder == b.SortOrder) {
            return a.Id < b.Id;
        }
        return a.SortOrder < b.SortOrder;
    });
}

template <typename TEntry, typename TContext>
bool EditorContextMenuRegistry::Render(const std::vector<TEntry>& entries, const TContext& context) const
{
    bool renderedAny = false;
    for (const TEntry& entry : entries) {
        if (!entry.Renderer) {
            continue;
        }
        renderedAny |= entry.Renderer(context);
    }
    return renderedAny;
}

template void EditorContextMenuRegistry::Register(std::vector<Entry<ProjectItemRenderer>>&, std::string, int, ProjectItemRenderer);
template void EditorContextMenuRegistry::Register(std::vector<Entry<ProjectBackgroundRenderer>>&, std::string, int, ProjectBackgroundRenderer);
template void EditorContextMenuRegistry::Register(std::vector<Entry<HierarchyEntityRenderer>>&, std::string, int, HierarchyEntityRenderer);
template void EditorContextMenuRegistry::Register(std::vector<Entry<HierarchyBackgroundRenderer>>&, std::string, int, HierarchyBackgroundRenderer);

template bool EditorContextMenuRegistry::Render(const std::vector<Entry<ProjectItemRenderer>>&, const ProjectItemContext&) const;
template bool EditorContextMenuRegistry::Render(const std::vector<Entry<ProjectBackgroundRenderer>>&, const ProjectBackgroundContext&) const;
template bool EditorContextMenuRegistry::Render(const std::vector<Entry<HierarchyEntityRenderer>>&, const HierarchyEntityContext&) const;
template bool EditorContextMenuRegistry::Render(const std::vector<Entry<HierarchyBackgroundRenderer>>&, const HierarchyBackgroundContext&) const;

} // namespace editorui
