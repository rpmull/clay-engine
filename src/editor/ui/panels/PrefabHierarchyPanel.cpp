#include "PrefabHierarchyPanel.h"
#include <cstring>
#include "core/input/Input.h"
#include "ui/utility/CreateEntityMenu.h"
#include "core/rendering/TextureLoader.h"
#include "ui/utility/UIHelpers.h"
#include <imgui_clay_inspector.h>

// Prefab hierarchy styling - matches prefab instances in main hierarchy
static const ImVec4 kPrefabEntityTextColor = ImVec4(0.4f, 0.7f, 1.0f, 1.0f);

void PrefabHierarchyPanel::OnImGuiRenderEmbedded() {
    // Small header indicating this is the prefab hierarchy
    ImGui::PushStyleColor(ImGuiCol_Text, kPrefabEntityTextColor);
    ImGui::Text("Prefab Hierarchy");
    ImGui::PopStyleColor();
    ImGui::Separator();
    DrawHierarchyContents();
}

void PrefabHierarchyPanel::DrawHierarchyContents() {
    if (!m_Context) { ImGui::Text("No scene loaded."); return; }
    EnsureIconsLoaded();

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##search_prefab", "Filter entities...", m_Filter, IM_ARRAYSIZE(m_Filter));
    ImGui::Separator();

    ImGuiTableFlags flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX;
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8, s.FramePadding.y));
    if (ImGui::BeginTable("PrefabHierarchy", 1, flags)) {
        ImGui::TableSetupColumn("##row", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_IndentEnable);
        bool hasFilter = (m_Filter[0] != '\\0');
        for (auto& entity : m_Context->GetEntities()) {
            auto* data = m_Context->GetEntityData(entity.GetID());
            if (data && data->Parent == -1) {
                if (!hasFilter || data->Name.find(m_Filter) != std::string::npos)
                    DrawEntityNode(entity);
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();

    if (ImGui::Button("Add Entity")) {
        Entity e = m_Context->CreateEntity("Empty"); (void)e;
    }

    if (ImGui::BeginPopupContextWindow("PrefabHierarchyBlankCtx", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::BeginMenu("Create")) {
            DrawCreateEntityMenuItems(m_Context, m_SelectedEntity);
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }

    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows)
        && !ImGui::IsAnyItemActive()
        && m_SelectedEntity && *m_SelectedEntity != -1) {
        if (Input::WasKeyPressedThisFrame(KeyCode::Delete)) {
            m_Context->QueueRemoveEntity(*m_SelectedEntity);
            *m_SelectedEntity = -1;
        }
        bool ctrl = ImGui::GetIO().KeyCtrl;
        if (ctrl && Input::WasKeyPressedThisFrame(KeyCode::D)) {
            EntityID dup = m_Context->DuplicateEntity(*m_SelectedEntity);
            if (dup != -1) { *m_SelectedEntity = dup; ExpandTo(dup); }
        }
        if (Input::WasKeyPressedThisFrame(KeyCode::F2)) {
            m_RenamingEntity = *m_SelectedEntity;
            if (auto* d = m_Context->GetEntityData(*m_SelectedEntity)) {
                strncpy(m_Filter, d->Name.c_str(), sizeof(m_Filter));
                m_Filter[sizeof(m_Filter)-1] = 0;
            }
        }
    }

    m_ExpandTarget = -1;
}

void PrefabHierarchyPanel::DrawEntityNode(const Entity& entity) {
    EntityID id = entity.GetID();
    auto* data = m_Context->GetEntityData(id); if (!data) return;

    ImGuiTreeNodeFlags flags = 0
        | ImGuiTreeNodeFlags_OpenOnArrow
        | ImGuiTreeNodeFlags_SpanAvailWidth;

    bool hasChildren = !data->Children.empty();
    if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf;

    const float ROW_H = ImGui::GetFrameHeight();
    ImGui::TableNextRow(ImGuiTableRowFlags_None, ROW_H);
    float rowY = ImGui::GetCursorScreenPos().y;
    float rightX = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;

    ImGui::PushID((int)id);
    ImGui::TableNextColumn();

    // Row background like SceneHierarchyPanel
    {
        ImU32 baseCol = ImGui::GetColorU32(ImGuiCol_FrameBg);
        ImU32 hovCol  = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
        ImU32 selCol  = ImGui::GetColorU32(ImGuiCol_HeaderActive);
        bool hovered = ImGui::IsMouseHoveringRect(ImVec2(ImGui::GetCursorScreenPos().x, rowY), ImVec2(rightX, rowY + ROW_H));
        ImU32 col = baseCol;
        if (m_SelectedEntity && *m_SelectedEntity == id) col = selCol; else if (hovered) col = hovCol;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(ImGui::GetCursorScreenPos().x, rowY), ImVec2(rightX, rowY + ROW_H), col, 0.0f);
    }

    // Visibility icon
    ImTextureID visIcon = data->Visible ? m_VisibleIcon : m_NotVisibleIcon;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0,0,0,0));
    float img = ROW_H - 2.0f * ImGui::GetStyle().FramePadding.y; if (img < 12.0f) img = 12.0f;
    bool toggled = ImGui::ImageButton("##vis", visIcon, ImVec2(img, img));
    ImGui::PopStyleColor(3);
    if (toggled) {
        bool newVisible = !data->Visible;
        m_Context->SetEntityVisible(id, newVisible);
    }
    ImGui::SameLine();

    // Tree label with prefab-colored text
    ImGui::PushStyleColor(ImGuiCol_Text, kPrefabEntityTextColor);
    bool opened = ImGui::TreeNodeEx((void*)(intptr_t)id, flags, "%s", entity.GetName().c_str());
    ImGui::PopStyleColor();

    // Selection
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) m_PendingSelect = id;
    if (m_PendingSelect == id && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        if (m_SelectedEntity) *m_SelectedEntity = id; m_PendingSelect = -1;
    }

    // Context menu (Duplicate/Delete)
    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
            EntityID dup = m_Context->DuplicateEntity(id);
            if (dup != -1 && m_SelectedEntity) { *m_SelectedEntity = dup; ExpandTo(dup); }
        }
        if (ImGui::MenuItem("Delete")) {
            m_Context->QueueRemoveEntity(id);
            if (m_SelectedEntity && *m_SelectedEntity == id) *m_SelectedEntity = -1;
        }
        ImGui::EndPopup();
    }

    // Drag source (cancel pending click selection when an actual drag starts)
    if (ImGui::BeginDragDropSource()) {
        if (m_PendingSelect == id) m_PendingSelect = -1;
        ImGui::SetDragDropPayload("ENTITY_ID", &id, sizeof(EntityID));
        ImGui::Text("Move %s", entity.GetName().c_str());
        ImGui::EndDragDropSource();
    }

    // Drop target (reparent)
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY_ID")) {
            EntityID draggedID = *(EntityID*)payload->Data;
            if (draggedID != id) m_Context->SetParent(draggedID, id, true);
        }
        ImGui::EndDragDropTarget();
    }

    if (opened) {
        for (EntityID c : data->Children) DrawEntityNode(m_Context->FindEntityByID(c));
        ImGui::TreePop();
    }

    ImGui::PopID();
}

void PrefabHierarchyPanel::EnsureIconsLoaded() {
    if (m_IconsLoaded) return;
    m_VisibleIcon    = TextureLoader::ToImGuiTextureID(TextureLoader::LoadIconTexture("assets/icons/visible.svg"));
    m_NotVisibleIcon = TextureLoader::ToImGuiTextureID(TextureLoader::LoadIconTexture("assets/icons/not_visible.svg"));
    m_IconsLoaded = true;
}


