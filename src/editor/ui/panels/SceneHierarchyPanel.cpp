#include "SceneHierarchyPanel.h"
#include <iostream>
#include "core/ecs/EntityData.h"
#include "core/rendering/TextureLoader.h"
#include "core/rendering/StandardMeshManager.h"
#include "core/rendering/MaterialManager.h"
#include <cstring>
#include "core/input/Input.h"
#include "ui/utility/CreateEntityMenu.h"
#include "core/serialization/Serializer.h"
#include "editor/Project.h"
#include <algorithm>
#include <filesystem>
#include "core/prefab/PrefabAsset.h"
#include "core/prefab/PrefabAPI.h"
#include "editor/prefab/PrefabEditorAPI.h"
#include "ui/utility/UIHelpers.h"
#include <imgui_internal.h>
#include <imgui_clay_inspector.h>
#include <imgui_claymore_style.h>
#include "editor/ui/UILayer.h"
#include "editor/undo/EditorSceneUndoStack.h"
#include "PrefabEditorPanel.h"

namespace
{
ClaymoreGUID GetPrefabRootGuid(PrefabEditorPanel* prefabEditor)
{
    ClaymoreGUID prefabGuid{};
    if (!prefabEditor) {
        return prefabGuid;
    }

    Scene* prefabScene = prefabEditor->GetScene();
    if (!prefabScene) {
        return prefabGuid;
    }

    if (EntityData* rootData = prefabScene->GetEntityData(prefabEditor->GetPrefabRootEntity())) {
        prefabGuid = rootData->PrefabGuid;
    }

    return prefabGuid;
}
}

EntityID SceneHierarchyPanel::s_ClipboardEntity = -1;
Scene* SceneHierarchyPanel::s_ClipboardScene = nullptr;

SceneHierarchyPanel::SceneHierarchyPanel(Scene* scene, EntityID* selectedEntity)
   : m_SelectedEntity(selectedEntity) {
   SetContext(scene);
   }

void SceneHierarchyPanel::RefreshPrefabFrameContext() {
   m_FramePrefabEditor = nullptr;
   m_FramePrefabModeActive = false;

   if (!m_UILayer) {
      return;
   }

   m_UILayer->GetActivePrefabEditor();
   m_FramePrefabEditor = m_UILayer->GetStickyPrefabEditor();
   if (!m_FramePrefabEditor) {
      return;
   }

   m_FramePrefabEditor->EnsureDeltasComputed();
   m_FramePrefabModeActive = m_FramePrefabEditor->IsInPrefabMode();
}

void SceneHierarchyPanel::OnImGuiRender() {
   if (!ImGui::Begin("Scene Hierarchy")) {
       ImGui::End();
       return;
   }

   RefreshPrefabFrameContext();
   PrefabEditorPanel* stickyPrefab = m_FramePrefabEditor;
   if (stickyPrefab) {
       DrawPrefabModeChip();
   } else {
       // Normal scene hierarchy header
       std::string headerLabel = m_SceneDisplayName.empty() ? std::string("Untitled Scene") : m_SceneDisplayName;
       ImGui::ClayHeaderStripConfig headerCfg;
       headerCfg.WidthOverride = ImGui::GetContentRegionAvail().x;
       ImGui::ClayHeaderStrip(headerLabel.c_str(), headerCfg);
   }
   
   ImGui::Dummy(ImVec2(0.0f, 3.0f));
   ImGui::SetNextItemWidth(-1);
   ImGui::InputTextWithHint("##search", "Filter entities...", m_Filter, IM_ARRAYSIZE(m_Filter));
   ImGui::Dummy(ImVec2(0.0f, 4.0f));
   
   // If in prefab mode, show the prefab hierarchy instead
   if (stickyPrefab) {
       // Draw the prefab's scene hierarchy
       Scene* prefabScene = stickyPrefab->GetScene();
       EntityID* prefabSelection = stickyPrefab->GetSelectedEntityPtr();
       EntityID editorLightID = stickyPrefab->GetEditorLightEntity();
       if (prefabScene) {
           // Temporarily switch context for drawing
           Scene* originalContext = m_Context;
           EntityID* originalSelection = m_SelectedEntity;
           EntityID originalEditorLight = m_HiddenEditorLightID;
           m_Context = prefabScene;
           m_SelectedEntity = prefabSelection;
           m_HiddenEditorLightID = editorLightID; // Hide editor light from hierarchy
           DrawHierarchyContents();
           m_Context = originalContext;
           m_SelectedEntity = originalSelection;
           m_HiddenEditorLightID = originalEditorLight;
       }
   } else {
       DrawHierarchyContents();
   }
   
   ImGui::End();
   }

void SceneHierarchyPanel::OnImGuiRenderEmbedded() {
   RefreshPrefabFrameContext();
   DrawHierarchyContents();
}

void SceneHierarchyPanel::DrawHierarchyContents() {
   if (!m_Context) {
      ImGui::Text("No scene loaded.");
      return;
      }

    EnsureIconsLoaded();

    // Hierarchy table with fixed row height (we draw row backgrounds manually to respect indent)
    ImGuiTableFlags flags = ImGuiTableFlags_SizingFixedFit |
        ImGuiTableFlags_NoHostExtendX;
    if (ImGui::BeginTable("Hierarchy", 1, flags)) {
        ImGui::TableSetupColumn("##row", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_IndentEnable);
        // Draw root-level entities (those with no parent)
        // Use const reference to avoid copying the entity list every frame.
        // Context menu actions that modify the entity list now use deferred operations
        // or copy on demand within the specific action.
        const auto& entities = m_Context->GetEntities();
        bool hasFilter = (m_Filter[0] != '\\0');

        // Snapshot root ids up front so structural edits during row rendering
        // don't invalidate the active hierarchy iteration.
        std::vector<EntityID> rootIds;
        rootIds.reserve(entities.size());
        for (const auto& entity : entities) {
            EntityID id = entity.GetID();
            // Skip hidden editor entities (like prefab editor light)
            if (id == m_HiddenEditorLightID) continue;
            EntityData* data = m_Context->GetEntityData(id);
            // Also skip entities with "__" prefix (editor-only by convention)
            if (data && data->Name.rfind("__", 0) == 0) continue;
            if (data && data->Parent == -1) {
                if (!hasFilter || data->Name.find(m_Filter) != std::string::npos) {
                    rootIds.push_back(id);
                }
            }
        }

        for (EntityID rootId : rootIds) {
            EntityData* data = m_Context->GetEntityData(rootId);
            if (!data) continue;
            if (rootId == m_HiddenEditorLightID) continue;
            if (data->Name.rfind("__", 0) == 0) continue;
            if (data->Parent != -1) continue;
            if (hasFilter && data->Name.find(m_Filter) == std::string::npos) continue;
            const bool isModelRoot = data->ModelAssetGuid.high != 0 || data->ModelAssetGuid.low != 0;
            const bool isPrefabRoot = data->PrefabGuid.high != 0 || data->PrefabGuid.low != 0 || !data->PrefabSource.empty();
            DrawEntityNode(rootId, 0, isModelRoot, isPrefabRoot);
        }
        ImGui::EndTable();
    }

    // Background context menu for hierarchy window (only when not over an item)
    // In prefab mode, disable root-level creation (must create under an existing entity)
    const bool inPrefabMode = (m_FramePrefabEditor != nullptr);
    
    if (ImGui::BeginPopupContextWindow("HierarchyBlankCtx", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        if (inPrefabMode) {
            ImGui::TextDisabled("Right-click an entity to create children");
            ImGui::TextDisabled("(Cannot create root-level entities in prefab mode)");
        } else {
            if (ImGui::BeginMenu("Create")) {
                DrawCreateEntityMenuItems(m_Context, m_SelectedEntity);
                ImGui::EndMenu();
            }
            const bool canPaste = (s_ClipboardScene == m_Context
                && s_ClipboardEntity != -1
                && m_Context->GetEntityData(s_ClipboardEntity));
            if (ImGui::MenuItem("Paste", "Ctrl+V", false, canPaste)) {
                EntityID dup = m_Context->DuplicateEntity(s_ClipboardEntity);
                if (dup != -1 && m_SelectedEntity) {
                    *m_SelectedEntity = dup;
                    ExpandTo(dup);
                }
            }
        }
        if (m_UILayer) {
            editorui::HierarchyBackgroundContext context;
            context.Context = m_Context;
            context.IsPrefabMode = inPrefabMode;
            m_UILayer->GetEditorContextMenus().RenderHierarchyBackground(context);
        }
        ImGui::EndPopup();
    }

    // Keyboard shortcuts when the hierarchy window is hovered and no text field is active
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows)
        && !ImGui::IsAnyItemActive()
        && m_SelectedEntity && *m_SelectedEntity != -1) {
        if (Input::WasKeyPressedThisFrame(KeyCode::Delete)) {
            EditorSceneUndoStack::Get().RequestDeferredCommit(m_Context, "Delete Entity");
            m_Context->QueueRemoveModelChild(*m_SelectedEntity);  // Track deletions if model child
            *m_SelectedEntity = -1;
        }
        bool ctrl = ImGui::GetIO().KeyCtrl;
        if (ctrl && Input::WasKeyPressedThisFrame(KeyCode::C)) {
            s_ClipboardEntity = *m_SelectedEntity;
            s_ClipboardScene = m_Context;
        }
        if (ctrl && Input::WasKeyPressedThisFrame(KeyCode::V)) {
            const bool canPaste = (s_ClipboardScene == m_Context
                && s_ClipboardEntity != -1
                && m_Context->GetEntityData(s_ClipboardEntity));
            if (canPaste) {
                PrefabEditorPanel* ctxPrefabEditor = m_FramePrefabEditor;
                ClaymoreGUID prefabGuid = GetPrefabRootGuid(ctxPrefabEditor);
                EntityID parentId = *m_SelectedEntity;
                if (parentId != INVALID_ENTITY_ID) {
                    EntityID dup = m_Context->DuplicateEntity(s_ClipboardEntity);
                    if (dup != -1) {
                        m_Context->SetParent(dup, parentId, true);
                        if (ctxPrefabEditor && (prefabGuid.high != 0 || prefabGuid.low != 0)) {
                            if (auto* dupData = m_Context->GetEntityData(dup)) {
                                dupData->PrefabGuid = prefabGuid;
                            }
                            ctxPrefabEditor->MarkDeltasStale();
                        }
                        *m_SelectedEntity = dup;
                        ExpandTo(dup);
                    }
                }
            }
        }
        if (ctrl && Input::WasKeyPressedThisFrame(KeyCode::D)) {
            PrefabEditorPanel* ctxPrefabEditor = m_FramePrefabEditor;
            ClaymoreGUID prefabGuid = GetPrefabRootGuid(ctxPrefabEditor);
            DuplicateEntityAsSibling(*m_SelectedEntity, ctxPrefabEditor, prefabGuid);
        }
        if (Input::WasKeyPressedThisFrame(KeyCode::F2)) {
            m_RenamingEntity = *m_SelectedEntity;
            if (auto* d = m_Context->GetEntityData(*m_SelectedEntity)) {
                strncpy(m_RenameBuffer, d->Name.c_str(), sizeof(m_RenameBuffer));
                m_RenameBuffer[sizeof(m_RenameBuffer)-1] = 0;
            }
        }
    }

    // Clear any one-shot expand target after drawing the list once
    m_ExpandTarget = -1;
}

EntityID SceneHierarchyPanel::DuplicateEntityAsSibling(EntityID sourceId, PrefabEditorPanel* prefabEditor, const ClaymoreGUID& prefabGuid) {
    if (!m_Context) return -1;
    auto* sourceData = m_Context->GetEntityData(sourceId);
    if (!sourceData) return -1;

    EntityID dup = m_Context->DuplicateEntity(sourceId);
    if (dup == -1) return -1;

    const EntityID parentId = sourceData->Parent;
    if (parentId != INVALID_ENTITY_ID) {
        m_Context->SetParent(dup, parentId, false);
    }

    // Place directly after the source within its sibling list (or root list).
    m_Context->ReorderEntity(dup, sourceId, true);

    if (prefabEditor && (prefabGuid.high != 0 || prefabGuid.low != 0)) {
        if (auto* dupData = m_Context->GetEntityData(dup)) {
            dupData->PrefabGuid = prefabGuid;
        }
        prefabEditor->MarkDeltasStale();
    }

    if (m_SelectedEntity) {
        *m_SelectedEntity = dup;
    }
    ExpandTo(dup);
    return dup;
}


void SceneHierarchyPanel::DrawEntityNode(EntityID id, int depth, bool inheritedModelNode, bool inheritedPrefabNode) {
   EntityData* data = m_Context->GetEntityData(id);
   if (!data) return;

    const bool isCurrentModelNode = inheritedModelNode ||
        data->ModelAssetGuid.high != 0 || data->ModelAssetGuid.low != 0;
    const bool isCurrentPrefabNode = inheritedPrefabNode ||
        data->PrefabGuid.high != 0 || data->PrefabGuid.low != 0 || !data->PrefabSource.empty();

    ImGuiTreeNodeFlags flags = 0
      | ImGuiTreeNodeFlags_OpenOnArrow
      | ImGuiTreeNodeFlags_SpanAvailWidth;

    bool hasChildren = !data->Children.empty();
    if (!hasChildren)
       flags |= ImGuiTreeNodeFlags_Leaf;

    const float rowHeight = Clay_UI_RowHeight();
    ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
    ImGui::TableNextColumn();

    ImVec2 cellCursor = ImGui::GetCursorPos();
    ImVec2 cellScreen = ImGui::GetCursorScreenPos();
    ImVec2 winPos = ImGui::GetWindowPos();
    ImVec2 contentMax = ImGui::GetWindowContentRegionMax();
    ImRect rowRect(ImVec2(cellScreen.x, cellScreen.y),
                   ImVec2(winPos.x + contentMax.x, cellScreen.y + rowHeight));
    bool isSelected = (m_SelectedEntity && *m_SelectedEntity == id);
    bool rowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        ImGui::IsMouseHoveringRect(rowRect.Min, rowRect.Max);
    Clay_UI_DrawRowBackground(ImGui::GetWindowDrawList(), rowRect, rowHovered, isSelected);

    ImGui::PushID((int)id);
    uihelpers::UILayoutProbe P(rowHeight);

    ImTextureID visIcon = data->Visible ? m_VisibleIcon : m_NotVisibleIcon;
    ImGuiStyle& st = ImGui::GetStyle();
    float iconSide = std::max(12.0f, rowHeight - st.FramePadding.y * 2.0f - 1.0f);

    float indentDepth = depth * st.IndentSpacing;
    float arrowWidth = ImGui::GetFontSize();
    float labelStartX = cellScreen.x + indentDepth + arrowWidth + ImGui::GetTreeNodeToLabelSpacing();
    float iconX = labelStartX - iconSide - st.ItemInnerSpacing.x;
    if (iconX < cellScreen.x + st.FramePadding.x)
        iconX = cellScreen.x + st.FramePadding.x;
    ImRect iconRect(ImVec2(iconX, rowRect.Min.y + (rowHeight - iconSide) * 0.5f),
                    ImVec2(iconX + iconSide, rowRect.Min.y + (rowHeight - iconSide) * 0.5f + iconSide));

    if (m_ExpandTarget != -1) {
        EntityID cur = m_ExpandTarget;
        while (cur != -1) {
            if (cur == id) { ImGui::SetNextItemOpen(true); break; }
            auto* d2 = m_Context->GetEntityData(cur);
            if (!d2) break;
            cur = d2->Parent;
        }
    }

    // Draw the row-selectable first and allow overlay controls (eye icon) to reuse its hit region.
    ImGui::SetNextItemAllowOverlap();
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0,0,0,0));
    bool opened = ImGui::TreeNodeEx((void*)(intptr_t)id, flags | ImGuiTreeNodeFlags_SpanFullWidth, "");
    bool treeItemHovered = ImGui::IsItemHovered();
    bool treeItemClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    bool treeItemDoubleClicked = treeItemHovered && ImGui::IsMouseDoubleClicked(0);
    ImGui::PopStyleColor(3);
    bool hasTreePush = opened;
    P.CheckLastItem("entity-row");

    bool entityDeleted = false;
    bool sceneStructureChanged = false;
    auto endCurrentNodeAfterMutation = [&]() {
        if (opened && hasTreePush) {
            ImGui::TreePop();
        }
        ImGui::PopID();
    };
    // Attach the context menu immediately so the tree row (not later controls) owns the right-click region.
    if (ImGui::BeginPopupContextItem()) {
        // Check if we're in prefab mode for special handling
        PrefabEditorPanel* ctxPrefabEditor = m_FramePrefabEditor;
        ClaymoreGUID prefabGuid = GetPrefabRootGuid(ctxPrefabEditor);
        
        // Create Child menu - available in both normal and prefab mode
        if (ImGui::BeginMenu("Create Child")) {
            EntityID newChild = (EntityID)-1;
            if (DrawCreateEntityMenuItems(m_Context, &newChild, id)) {
                // Parent under the current entity
                if (newChild != (EntityID)-1) {
                    m_Context->SetParent(newChild, id);
                    // Set PrefabGuid in prefab mode
                    if (ctxPrefabEditor && (prefabGuid.high != 0 || prefabGuid.low != 0)) {
                        if (auto* newData = m_Context->GetEntityData(newChild)) {
                            newData->PrefabGuid = prefabGuid;
                        }
                        ctxPrefabEditor->MarkDeltasStale();  // Refresh delta colors
                    }
                    if (m_SelectedEntity) *m_SelectedEntity = newChild;
                    ExpandTo(newChild);
                    sceneStructureChanged = true;
                }
            }
            ImGui::EndMenu();
            if (sceneStructureChanged) {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                endCurrentNodeAfterMutation();
                return;
            }
        }
        
        ImGui::Separator();
        
        if (ImGui::MenuItem("Rename")) {
            m_RenamingEntity = id;
            strncpy(m_RenameBuffer, data->Name.c_str(), sizeof(m_RenameBuffer));
            m_RenameBuffer[sizeof(m_RenameBuffer)-1] = 0;
        }
        if (ImGui::MenuItem("Copy", "Ctrl+C")) {
            s_ClipboardEntity = id;
            s_ClipboardScene = m_Context;
        }
        const bool canPaste = (s_ClipboardScene == m_Context
            && s_ClipboardEntity != -1
            && m_Context->GetEntityData(s_ClipboardEntity));
        if (ImGui::MenuItem("Paste as Child", "Ctrl+V", false, canPaste)) {
            EntityID dup = m_Context->DuplicateEntity(s_ClipboardEntity);
            if (dup != -1) {
                m_Context->SetParent(dup, id, true);
                if (ctxPrefabEditor && (prefabGuid.high != 0 || prefabGuid.low != 0)) {
                    if (auto* dupData = m_Context->GetEntityData(dup)) {
                        dupData->PrefabGuid = prefabGuid;
                    }
                    ctxPrefabEditor->MarkDeltasStale();  // Refresh delta colors
                }
                if (m_SelectedEntity) *m_SelectedEntity = dup;
                ExpandTo(dup);
                sceneStructureChanged = true;
            }
        }
        if (sceneStructureChanged) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            endCurrentNodeAfterMutation();
            return;
        }
        if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
            EntityID dup = DuplicateEntityAsSibling(id, ctxPrefabEditor, prefabGuid);
            if (dup != INVALID_ENTITY_ID) {
                sceneStructureChanged = true;
            }
        }
        if (sceneStructureChanged) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            endCurrentNodeAfterMutation();
            return;
        }
        if (ImGui::MenuItem("Delete")) {
            EditorSceneUndoStack::Get().RequestDeferredCommit(m_Context, "Delete Entity");
            m_Context->QueueRemoveModelChild(id);  // Track deletions if model child
            if (m_SelectedEntity && *m_SelectedEntity == id)
                *m_SelectedEntity = -1;
            entityDeleted = true;
            if (ctxPrefabEditor) ctxPrefabEditor->MarkDeltasStale();  // Refresh delta colors
        }
        
        // Model root: Reset children to model default (clears position/scale deltas from hot reload)
        bool isModelRoot = data->ModelAssetGuid.high != 0 || data->ModelAssetGuid.low != 0;
        if (isModelRoot && ImGui::MenuItem("Reset Children to Model Default")) {
            if (m_Context->ResetModelChildrenToDefault(id)) {
                if (ctxPrefabEditor) ctxPrefabEditor->MarkDeltasStale();
                sceneStructureChanged = true;
            }
        }
        if (sceneStructureChanged) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            endCurrentNodeAfterMutation();
            return;
        }
        if (isModelRoot && ImGui::MenuItem("Align Children to Terrain (AABB Y)")) {
            if (m_Context->AlignModelRootChildrenToTerrain(id)) {
                if (ctxPrefabEditor) ctxPrefabEditor->MarkDeltasStale();
            }
        }
        
        // Only show "Save to Prefab" in main scene mode, not prefab edit mode
        if (!ctxPrefabEditor && ImGui::MenuItem("Save to Prefab")) {
            namespace fs = std::filesystem;
            fs::path folder = Project::GetProjectDirectory() / "assets/prefabs";
            std::error_code ec; fs::create_directories(folder, ec);
            auto sanitize = [](std::string s){
                const std::string invalid = "<>:\"/\\|?*";
                for (char& c : s) if (invalid.find(c) != std::string::npos) c = '_';
                size_t start = s.find_first_not_of(' ');
                size_t end = s.find_last_not_of(' ');
                if (start == std::string::npos) return std::string("Prefab");
                return s.substr(start, end - start + 1);
            };
            std::string desired = sanitize(data->Name.empty() ? std::string("Prefab") : data->Name);
            if (desired.empty()) desired = "Prefab";
            fs::path out = folder / (desired + ".prefab");
            const bool overwriting = fs::exists(out);
            PrefabAsset asset;
            bool built = prefab_editor::BuildPrefabAssetFromScene(*m_Context, id, asset);
            if (built && overwriting) {
                prefab_editor::AdoptExistingPrefabAssetGuid(out.string(), asset);
            }
            bool ok = built && PrefabIO::SavePrefab(out.string(), asset);
            if (ok) {
                std::cout << "[Hierarchy] Prefab "
                          << (overwriting ? "overwritten: " : "saved: ")
                          << out.string() << std::endl;
                std::string vpath;
                if (!prefab_editor::FinalizeSavedPrefabFromScene(*m_Context, id, out.string(), asset, &vpath)) {
                    std::cerr << "[Hierarchy] Prefab was saved but could not be linked: "
                              << out.string() << std::endl;
                }
            } else {
                std::cerr << "[Hierarchy] Failed to save prefab: " << out.string() << std::endl;
            }
        }
        if (m_UILayer) {
            editorui::HierarchyEntityContext context;
            context.Context = m_Context;
            context.Entity = id;
            context.IsPrefabMode = (ctxPrefabEditor != nullptr);
            m_UILayer->GetEditorContextMenus().RenderHierarchyEntity(context);
        }
        ImGui::EndPopup();
    }

    if (sceneStructureChanged) {
        if (opened && hasTreePush) {
            ImGui::TreePop();
        }
        ImGui::PopID();
        return;
    }

    if (ImGui::BeginDragDropSource()) {
        if (m_PendingSelect == id) m_PendingSelect = -1;
        ImGui::SetDragDropPayload("ENTITY_ID", &id, sizeof(EntityID));
        ImGui::Text("Drag %s", data->Name.c_str());
        ImGui::EndDragDropSource();
    }

    ImGui::SetCursorScreenPos(iconRect.Min);
    ImGui::PushID("vis");
    ImGui::InvisibleButton("##button", iconRect.GetSize(), ImGuiButtonFlags_AllowOverlap);
    ImGui::PopID();
    bool iconHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);
    bool iconClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    ImGui::SetCursorPos(cellCursor);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (iconHovered)
        drawList->AddRectFilled(iconRect.Min, iconRect.Max, ImGui::GetColorU32(Clay_GetEditorTheme().SelectionHover), 2.0f);
    drawList->AddImage(visIcon, iconRect.Min, iconRect.Max);
    if (iconHovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (iconClicked) {
        bool newVisible = !data->Visible;
        m_Context->SetEntityVisible(id, newVisible);
        if (m_PendingSelect == id) m_PendingSelect = -1;
    }

    float textStartX = iconRect.Max.x + st.ItemInnerSpacing.x;
    if (textStartX < labelStartX)
        textStartX = labelStartX;
    float textY = rowRect.Min.y + (rowHeight - ImGui::GetFontSize()) * 0.5f;
    if (m_RenamingEntity == id) {
        ImVec2 renamePos(textStartX - winPos.x, rowRect.Min.y + (rowHeight - ImGui::GetFrameHeight()) * 0.5f - winPos.y);
        ImGui::SetCursorPos(renamePos);
        ImGui::SetKeyboardFocusHere();
        float renameWidth = rowRect.Max.x - textStartX - st.FramePadding.x;
        if (renameWidth < 80.0f) renameWidth = 80.0f;
        ImGui::SetNextItemWidth(renameWidth);
        if (ImGui::InputText("##rename", m_RenameBuffer, IM_ARRAYSIZE(m_RenameBuffer), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            std::string desired = m_RenameBuffer;
            if (desired.empty()) desired = "Entity";
            int suffix = 1;
            std::string finalName = desired;
            bool unique = false;
            while (!unique) {
                unique = true;
                for (const auto& e : m_Context->GetEntities()) {
                    if (e.GetID() == id) continue;
                    auto* ed = m_Context->GetEntityData(e.GetID());
                    if (ed && ed->Name == finalName) { unique = false; break; }
                }
                if (!unique) { finalName = desired + "_" + std::to_string(suffix++); }
            }
            data->Name = finalName;
            m_RenamingEntity = -1;
        }
        if (!ImGui::IsItemActive() && ImGui::IsMouseClicked(0)) m_RenamingEntity = -1;
    } else {
        // Entity hierarchy color coding:
        // Priority order for colors:
        // 1. Added prefab nodes (green) - highest priority, user needs to know these are new
        // 2. Changed prefab nodes (orange) - user needs to know these differ from baseline
        // 3. Prefab nodes (blue) - entities from a prefab instance
        // 4. Model nodes (purple) - entities from an imported model  
        // 5. Default (white) - regular scene entities
        
        // Check if we're in prefab mode and apply semantic colors once per row.
        bool isNewEntity = false;
        bool isModifiedEntity = false;
        bool isBaselineEntity = false;
        const ClayEditorTheme& theme = Clay_GetEditorTheme();
        
        if (m_FramePrefabEditor) {
            isNewEntity = m_FramePrefabEditor->IsEntityNew(data->EntityGuid);
            isModifiedEntity = m_FramePrefabEditor->IsEntityModified(data->EntityGuid);
            isBaselineEntity = m_FramePrefabEditor->IsBaselineEntity(data->EntityGuid);
        }
        
        // Check entity type classification
        const bool isPrefabInstance = m_FramePrefabModeActive ? isBaselineEntity
                                     : isCurrentPrefabNode;
        const bool isModelInstance = !isPrefabInstance && isCurrentModelNode;
        
        // Determine text color based on priority
        ImU32 textColor;
        if (isNewEntity) {
            // Added nodes = Green (highest priority in prefab edit mode)
            textColor = ImGui::ColorConvertFloat4ToU32(theme.AddedPrefabText);
        } else if (isModifiedEntity) {
            // Changed nodes = Orange/Yellow
            textColor = ImGui::ColorConvertFloat4ToU32(theme.ModifiedPrefabText);
        } else if (isPrefabInstance) {
            // Prefab nodes = Blue (even if previously model nodes)
            textColor = ImGui::ColorConvertFloat4ToU32(theme.PrefabNodeText);
        } else if (isModelInstance) {
            // Model nodes = Purple
            textColor = ImGui::ColorConvertFloat4ToU32(theme.ModelNodeText);
        } else {
            // Default = white
            textColor = ImGui::GetColorU32(ImGuiCol_Text);
        }
        
        // If this is a prefab root, draw a small prefab icon before the name
        if (!data->PrefabSource.empty() && m_PrefabIcon) {
            float prefabIconSize = ImGui::GetFontSize();
            ImVec2 iconPos(textStartX, textY);
            drawList->AddImage(m_PrefabIcon, iconPos, ImVec2(iconPos.x + prefabIconSize, iconPos.y + prefabIconSize), 
                              ImVec2(0,0), ImVec2(1,1), ImGui::ColorConvertFloat4ToU32(theme.PrefabNodeText));
            textStartX += prefabIconSize + 4.0f;
        }

        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(textColor));
        ImGui::RenderTextEllipsis(drawList,
                                  ImVec2(textStartX, textY),
                                  ImVec2(rowRect.Max.x - st.FramePadding.x, rowRect.Max.y),
                                  rowRect.Max.x - st.FramePadding.x,
                                  data->Name.c_str(),
                                  nullptr,
                                  nullptr);
        ImGui::PopStyleColor();
    }

    if (treeItemClicked) {
        m_PendingSelect = id;
    }
    if (treeItemDoubleClicked) {
        m_RenamingEntity = id;
        strncpy(m_RenameBuffer, data->Name.c_str(), sizeof(m_RenameBuffer));
        m_RenameBuffer[sizeof(m_RenameBuffer)-1] = 0;
    }

    if (entityDeleted) {
        if (opened) {
            ImGui::TreePop();
        }
        ImGui::PopID();
        return;
    }

    if (!data->PrefabSource.empty()) {
        float w = rowHeight;
        float avail = ImGui::GetContentRegionAvail().x;
        if (avail >= w + 1.0f) {
            ImGui::SameLine(0.0f, 0.0f);
            uihelpers::UI_RightAlignNextItem(w);
            uihelpers::UI_CenterCursorY(w);
            if (ImGui::ArrowButton("##openpf", ImGuiDir_Right)) {
                try {
                    std::filesystem::path full = Project::GetProjectDirectory() / data->PrefabSource;
                    if (m_OnOpenPrefab) m_OnOpenPrefab(full.string());
                } catch(...) {}
            }
        }
    }

    if (m_PendingSelect == id && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        if (m_SelectedEntity) *m_SelectedEntity = id;
        m_PendingSelect = -1;
    }

    enum class DropZone { Before, Child, After };
    auto computeDropZone = [&](float mouseY) -> DropZone {
        const float h = rowRect.GetHeight();
        const float top = rowRect.Min.y + h * 0.25f;
        const float bottom = rowRect.Max.y - h * 0.25f;
        if (mouseY < top) return DropZone::Before;
        if (mouseY > bottom) return DropZone::After;
        return DropZone::Child;
    };
    if (ImGui::IsDragDropActive() && ImGui::IsMouseHoveringRect(rowRect.Min, rowRect.Max)) {
        const ImGuiPayload* payload = ImGui::GetDragDropPayload();
        if (payload && std::strcmp(payload->DataType, "ENTITY_ID") == 0) {
            DropZone zone = computeDropZone(ImGui::GetMousePos().y);
            ImDrawList* hintDraw = ImGui::GetWindowDrawList();
            ImU32 hintColor = ImGui::GetColorU32(ImGuiCol_DragDropTarget);
            if (zone == DropZone::Child) {
                hintDraw->AddRect(rowRect.Min, rowRect.Max, hintColor, 2.0f, 0, 2.0f);
            } else {
                float lineY = (zone == DropZone::Before) ? rowRect.Min.y : rowRect.Max.y;
                hintDraw->AddLine(ImVec2(rowRect.Min.x, lineY), ImVec2(rowRect.Max.x, lineY), hintColor, 2.0f);
            }
        }
    }
    if (ImGui::BeginDragDropTargetCustom(rowRect, ImGui::GetID((void*)(intptr_t)id))) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY_ID")) {
            EntityID draggedID = *(EntityID*)payload->Data;
            if (draggedID != id) {
                EntityData* draggedData = m_Context->GetEntityData(draggedID);
                if (draggedData) {
                    DropZone zone = computeDropZone(ImGui::GetMousePos().y);
                    PrefabEditorPanel* ctxPrefabEditor = m_FramePrefabEditor;
                    if (zone == DropZone::Child) {
                        m_Context->SetParent(draggedID, id, true);
                    } else {
                        const EntityID targetParent = data->Parent;
                        if (draggedData->Parent != targetParent) {
                            m_Context->SetParent(draggedID, targetParent, true);
                        }
                        const bool placeAfter = (zone == DropZone::After);
                        m_Context->ReorderEntity(draggedID, id, placeAfter);
                    }
                    if (ctxPrefabEditor) ctxPrefabEditor->MarkDeltasStale();
                }
            }
        }
        // Accept model files from project panel - instantiate as child
        if (const ImGuiPayload* assetPayload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
            const char* assetPath = static_cast<const char*>(assetPayload->Data);
            if (assetPath && *assetPath) {
                std::filesystem::path p(assetPath);
                std::string ext = p.extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                // Check if it's a model file
                if (ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb") {
                    // Instantiate model at origin, then parent to this entity
                    const std::string parentName = data->Name;
                    EntityID modelRoot = m_Context->InstantiateModel(assetPath, glm::vec3(0.0f));
                    if (modelRoot != INVALID_ENTITY_ID && modelRoot != (EntityID)-1) {
                        // Use preserveWorldTransform = false so the model's local transform is zeroed
                        // relative to the parent. This is critical for armor dropped onto skeleton roots
                        // to avoid frustum culling issues (armor at world origin instead of character position).
                        m_Context->SetParent(modelRoot, id, false);
                        // Select the newly instantiated model
                        if (m_SelectedEntity) *m_SelectedEntity = modelRoot;
                        ExpandTo(modelRoot);
                        std::cout << "[Hierarchy] Instantiated model as child: " << assetPath << " -> " << parentName << std::endl;
                        sceneStructureChanged = true;
                    }
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

   if (sceneStructureChanged) {
      if (opened && hasTreePush) {
         ImGui::TreePop();
      }
      ImGui::PopID();
      return;
   }

    if (opened && hasTreePush) {
       // Copy children IDs to avoid iterator invalidation if children are added/removed
       std::vector<EntityID> children = data->Children;
       for (EntityID childID : children) {
          DrawEntityNode(childID, depth + 1, isCurrentModelNode, isCurrentPrefabNode);
          }
       ImGui::TreePop();
       }

    ImGui::PopID();
   }

void SceneHierarchyPanel::ExpandTo(EntityID id) {
    m_ExpandTarget = id;
}

void SceneHierarchyPanel::EnsureIconsLoaded() {
    if (m_IconsLoaded) return;
    m_VisibleIcon    = TextureLoader::ToImGuiTextureID(TextureLoader::LoadIconTexture("assets/icons/visible.svg"));
    m_NotVisibleIcon = TextureLoader::ToImGuiTextureID(TextureLoader::LoadIconTexture("assets/icons/not_visible.svg"));
    m_PrefabIcon     = TextureLoader::ToImGuiTextureID(TextureLoader::LoadIconTexture("assets/icons/cube.svg"));
    m_IconsLoaded = true;
}

void SceneHierarchyPanel::DrawPrefabModeChip() {
    PrefabEditorPanel* activePrefab = m_FramePrefabEditor;
    if (!activePrefab) return;
    
    // Get prefab name from path
    std::string prefabPath = activePrefab->GetPrefabPath();
    std::string prefabName = std::filesystem::path(prefabPath).stem().string();
    if (activePrefab->IsDirty()) prefabName += "*";
    
    const ClayEditorTheme& theme = Clay_GetEditorTheme();
    const ImVec4 borderColor = theme.BorderStrong;
    const ImVec4 textDim = theme.TextMuted;
    const ImVec4 textColor = theme.Text;
    const ImVec4 nameColor = activePrefab->IsDirty() ? theme.ModifiedPrefabText : theme.PrefabNodeText;
    float availWidth = ImGui::GetContentRegionAvail().x;
    float chipHeight = ImGui::GetFrameHeight() + 4.0f;
    const float rounding = 4.0f;
    
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 cursorPos = ImGui::GetCursorScreenPos();
    ImRect chipRect(cursorPos, ImVec2(cursorPos.x + availWidth, cursorPos.y + chipHeight));
    
    bool hovered = ImGui::IsMouseHoveringRect(chipRect.Min, chipRect.Max);
    const ImVec4 bg = hovered ? theme.SurfaceRaised : theme.SurfaceSidebar;
    drawList->AddRectFilled(chipRect.Min, chipRect.Max, ImGui::ColorConvertFloat4ToU32(bg), rounding);
    drawList->AddRectFilled(ImVec2(chipRect.Min.x + 1.0f, chipRect.Min.y + 1.0f),
                            ImVec2(chipRect.Min.x + 4.0f, chipRect.Max.y - 1.0f),
                            ImGui::ColorConvertFloat4ToU32(theme.SelectionAccent),
                            2.0f);
    drawList->AddRect(chipRect.Min, chipRect.Max, ImGui::ColorConvertFloat4ToU32(borderColor), rounding);
    
    // Layout: [Icon] "Editing Prefab:" [Name] [Close Button]
    float padding = 6.0f;
    float iconSize = chipHeight - 12.0f;
    float closeButtonSize = chipHeight - 12.0f;
    
    // Prefab icon
    ImVec2 iconPos(cursorPos.x + padding, cursorPos.y + (chipHeight - iconSize) * 0.5f);
    if (m_PrefabIcon) {
        drawList->AddImage(m_PrefabIcon, iconPos, ImVec2(iconPos.x + iconSize, iconPos.y + iconSize));
    }
    
    // "Editing Prefab:" label
    float textStartX = iconPos.x + iconSize + padding;
    float textY = cursorPos.y + (chipHeight - ImGui::GetFontSize()) * 0.5f;
    drawList->AddText(ImVec2(textStartX, textY), ImGui::ColorConvertFloat4ToU32(textDim), "Editing Prefab:");

    // Prefab name (highlighted)
    float labelWidth = ImGui::CalcTextSize("Editing Prefab:").x;
    const float nameMinX = textStartX + labelWidth + 5.0f;
    const float nameMaxX = chipRect.Max.x - closeButtonSize - padding * 2.0f;
    ImGui::PushStyleColor(ImGuiCol_Text, nameColor);
    ImGui::RenderTextEllipsis(drawList,
                              ImVec2(nameMinX, textY),
                              ImVec2(nameMaxX, chipRect.Max.y),
                              nameMaxX,
                              prefabName.c_str(),
                              nullptr,
                              nullptr);
    ImGui::PopStyleColor();
    
    // Close/back button (X)
    float closeX = chipRect.Max.x - closeButtonSize - padding;
    float closeY = cursorPos.y + (chipHeight - closeButtonSize) * 0.5f;
    ImRect closeRect(ImVec2(closeX, closeY), ImVec2(closeX + closeButtonSize, closeY + closeButtonSize));
    
    bool closeHovered = ImGui::IsMouseHoveringRect(closeRect.Min, closeRect.Max);
    if (closeHovered) {
        drawList->AddRectFilled(closeRect.Min, closeRect.Max, ImGui::ColorConvertFloat4ToU32(theme.SelectionFill), 3.0f);
        drawList->AddRect(closeRect.Min, closeRect.Max, ImGui::ColorConvertFloat4ToU32(theme.SelectionOutline), 3.0f);
    }
    
    // Draw X
    float xPad = closeButtonSize * 0.25f;
    ImU32 xColor = ImGui::ColorConvertFloat4ToU32(textColor);
    drawList->AddLine(ImVec2(closeRect.Min.x + xPad, closeRect.Min.y + xPad), 
                      ImVec2(closeRect.Max.x - xPad, closeRect.Max.y - xPad), xColor, 2.0f);
    drawList->AddLine(ImVec2(closeRect.Max.x - xPad, closeRect.Min.y + xPad), 
                      ImVec2(closeRect.Min.x + xPad, closeRect.Max.y - xPad), xColor, 2.0f);
    
    // Handle close button click - exit prefab editing mode
    ImGui::SetCursorScreenPos(closeRect.Min);
    ImGui::InvisibleButton("##closePrefabMode", closeRect.GetSize());
    if (ImGui::IsItemClicked()) {
        // Exit prefab editing mode
        if (m_UILayer) {
            m_UILayer->ExitPrefabEditMode();
        }
    }
    
    // Reserve space for the chip
    ImGui::SetCursorScreenPos(ImVec2(cursorPos.x, cursorPos.y + chipHeight + 3.0f));
    
    // Tooltip
    if (hovered && !closeHovered) {
        ImGui::SetTooltip("Click X or focus main viewport to return to scene editing");
    }
}
