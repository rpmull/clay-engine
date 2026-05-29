#include "editor/tools/TextureCleanupPanel.h"
#include "editor/Project.h"

void TextureCleanupPanel::OnImGuiRender() {
    if (!m_Open) return;
    if (!ImGui::Begin("Texture Cleanup", &m_Open)) {
        ImGui::End();
        return;
    }

    if (Project::GetProjectDirectory().empty()) {
        ImGui::TextUnformatted("No project loaded.");
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted("Scan all imported model metas for duplicate texture content.");
    ImGui::TextUnformatted("Shared textures are moved to assets/textures/shared and references updated.");
    ImGui::Spacing();

    if (ImGui::Button("Run Cleanup")) {
        m_LastReport = texture_cleanup::CleanupReport{};
        m_LastOk = texture_cleanup::CleanupSharedTextures(m_LastReport);
        m_HasRun = true;
    }

    if (m_HasRun) {
        ImGui::Separator();
        ImGui::Text("Result: %s", m_LastOk ? "Success" : "Failed");
        ImGui::Text("Models scanned: %d", m_LastReport.modelsScanned);
        ImGui::Text("Models updated: %d", m_LastReport.modelsUpdated);
        ImGui::Text("Textures referenced: %d", m_LastReport.texturesReferenced);
        ImGui::Text("Duplicate groups: %d", m_LastReport.duplicateGroups);
        ImGui::Text("Moved to shared: %d", m_LastReport.texturesMovedToShared);
        ImGui::Text("Textures removed: %d", m_LastReport.texturesRemoved);
        ImGui::Text("Texture meta updates: %d", m_LastReport.textureMetaUpdated);
        ImGui::Text("Empty folders removed: %d", m_LastReport.emptyFoldersRemoved);
        ImGui::Text("Scenes scanned: %d", m_LastReport.scenesScanned);
        ImGui::Text("Scenes updated: %d", m_LastReport.scenesUpdated);
        ImGui::Text("Scene texture remaps: %d", m_LastReport.sceneTexturePathRemaps);
        ImGui::Text("Scene overrides reset: %d", m_LastReport.sceneMaterialOverridesReset);
        ImGui::Text("Prefabs scanned: %d", m_LastReport.prefabsScanned);
        ImGui::Text("Prefabs updated: %d", m_LastReport.prefabsUpdated);
        ImGui::Text("Prefab texture remaps: %d", m_LastReport.prefabTexturePathRemaps);
        ImGui::Text("Prefab overrides reset: %d", m_LastReport.prefabMaterialOverridesReset);

        ImGui::Spacing();
        ImGui::TextUnformatted("Log:");
        ImGui::BeginChild("TextureCleanupLog", ImVec2(0, 200), true);
        if (m_LastReport.logLines.empty()) {
            ImGui::TextUnformatted("(no messages)");
        } else {
            for (const auto& line : m_LastReport.logLines) {
                ImGui::TextUnformatted(line.c_str());
            }
        }
        ImGui::EndChild();
    }

    ImGui::End();
}

