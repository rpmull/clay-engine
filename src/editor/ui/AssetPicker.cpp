#include "editor/ui/AssetPicker.h"
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "ui/utility/ProjectAssetIndex.h"

namespace {
static std::vector<std::string> s_recent;

static bool MatchesGlob(const std::filesystem::path& p, const std::string& glob)
{
    // Very basic suffix match for patterns like "*.anim"; extend if needed
    if (glob.size() >= 2 && glob[0] == '*' && glob[1] == '.') {
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        std::string want = glob.substr(1); // e.g., ".anim"
        std::transform(want.begin(), want.end(), want.begin(), ::tolower);
        return ext == want;
    }
    // Fallback: accept all
    return true;
}

static ui::ProjectAssetQuery QueryFromGlob(const std::string& glob)
{
    if (glob.size() >= 3 && glob[0] == '*' && glob[1] == '.') {
        const std::string suffix = glob.substr(1);
        if (suffix.find('.', 1) != std::string::npos) {
            return ui::MakeSuffixQuery({ suffix.c_str() });
        }
        return ui::MakeExtensionQuery({ suffix.c_str() });
    }
    return {};
}
}

AssetPickerResult DrawAssetPicker(AssetPickerConfig cfg)
{
    AssetPickerResult result{};

    // Ensure unique ID scope per picker instance (prevents visible ID collisions when multiple pickers are drawn)
    ImGui::PushID(cfg.title ? cfg.title : (cfg.glob ? cfg.glob : "AssetPicker"));
    ImGui::TextUnformatted(cfg.title ? cfg.title : "Assets");
    ImGui::Separator();

    static std::string s_filter;
    char filterBuf[256];
    std::strncpy(filterBuf, s_filter.c_str(), sizeof(filterBuf));
    filterBuf[sizeof(filterBuf) - 1] = 0;
    ImGui::SetNextItemWidth(220);
    if (ImGui::InputText("Filter", filterBuf, sizeof(filterBuf))) {
        s_filter = filterBuf;
    }

    std::vector<std::string> all;
    const auto& entries = ui::GetProjectAssetEntries(QueryFromGlob(cfg.glob ? cfg.glob : "*.*"));
    const std::string filterLower = [&]() {
        std::string lower = s_filter;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        return lower;
    }();
    all.reserve(entries.size());
    for (const ui::ProjectAssetEntry& entry : entries) {
        if (!filterLower.empty()) {
            std::string searchable = entry.normalizedProjectRelativePath;
            searchable.push_back(' ');
            std::string lowerName = entry.name;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
            searchable += lowerName;
            if (searchable.find(filterLower) == std::string::npos) {
                continue;
            }
        }
        if (!MatchesGlob(std::filesystem::path(entry.absolutePath), cfg.glob ? cfg.glob : "*.*")) {
            continue;
        }
        all.push_back(entry.absolutePath);
    }
    std::sort(all.begin(), all.end());

    // Recents
    if (cfg.showRecents && !s_recent.empty()) {
        ImGui::TextDisabled("Recent");
        ImGui::BeginChild("ap_recent", ImVec2(0, 64), true);
        for (const auto& r : s_recent) {
            if (!s_filter.empty() && r.find(s_filter) == std::string::npos) continue;
            if (ImGui::Selectable(r.c_str(), false)) {
                result.chosen = true; result.path = r; 
                // Move to front
                s_recent.erase(std::remove(s_recent.begin(), s_recent.end(), r), s_recent.end());
                s_recent.insert(s_recent.begin(), r);
                ImGui::EndChild();
                return result;
            }
        }
        ImGui::EndChild();
    }

    ImGui::TextDisabled("All");
    ImGui::BeginChild("ap_all", ImVec2(0, 180), true);
    for (const auto& a : all) {
        if (ImGui::Selectable(a.c_str(), false)) {
            result.chosen = true; result.path = a;
            // Update recents
            s_recent.erase(std::remove(s_recent.begin(), s_recent.end(), a), s_recent.end());
            s_recent.insert(s_recent.begin(), a);
            ImGui::EndChild();
            return result;
        }
    }

    // Accept drag-drop from Project panel or external
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
            const char* path = static_cast<const char*>(payload->Data);
            if (path && *path) {
                std::filesystem::path p(path);
                if (MatchesGlob(p, cfg.glob ? cfg.glob : "*.*")) {
                    result.chosen = true; result.path = p.string();
                    s_recent.erase(std::remove(s_recent.begin(), s_recent.end(), result.path), s_recent.end());
                    s_recent.insert(s_recent.begin(), result.path);
                    ImGui::EndDragDropTarget();
                    ImGui::EndChild();
                    return result;
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::EndChild();
    ImGui::PopID();

    return result;
}


