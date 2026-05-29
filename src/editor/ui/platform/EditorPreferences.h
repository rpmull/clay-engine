#pragma once

#include "editor/EditorSettings.h"

#include <glm/glm.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace editorui {

class EditorActionRegistry;

enum class EditorPreferenceGroup {
    Workspace = 0,
    Viewport,
    Actions,
    Windows,
    Count
};

class EditorPreferences {
public:
    bool Load(const std::filesystem::path& projectRoot);

    void ApplyViewportSettings(EditorSettings& settings) const;
    void CaptureViewportSettings(const EditorSettings& settings);

    float GetUIScale() const { return m_UIScale; }
    void SetUIScale(float scale);

    const std::string& GetLayoutIni() const { return m_LayoutIni; }
    void SetLayoutIni(std::string layoutIni);

    const std::unordered_map<std::string, int>& GetActionUsageCounts() const { return m_ActionUsageCounts; }
    void SetActionUsageCounts(std::unordered_map<std::string, int> usageCounts);

    void CaptureWindowStates(const EditorActionRegistry& registry);
    void RestoreWindowStates(EditorActionRegistry& registry) const;

    void RequestSave(EditorPreferenceGroup group);
    void SaveIfDue();

    uint64_t GetRevision(EditorPreferenceGroup group) const;
    bool HasLoadedProject() const { return m_Loaded; }

private:
    struct ViewportSnapshot {
        float ZoomBaseSpeed = 1.0f;
        float ZoomAcceleration = 0.15f;
        float ZoomMinDistance = 0.1f;
        float ZoomMaxDistance = 5000.0f;
        bool SmoothZoomEnabled = true;
        float ZoomSmoothness = 0.15f;
        float OrbitSensitivity = 0.2f;
        float PanSpeedFactor = 0.01f;
        float FocusDuration = 0.35f;
        float FocusDistancePadding = 1.5f;
        float FocusDefaultDistance = 5.0f;
        bool DeselectOnEmptyClick = true;
        float PickingTolerance = 0.0f;
        bool PickingSkipHidden = true;
        bool PickingSkipLocked = true;
        glm::vec4 GridAxisColorX = glm::vec4(0.8f, 0.2f, 0.2f, 0.7f);
        glm::vec4 GridAxisColorY = glm::vec4(0.2f, 0.8f, 0.2f, 0.7f);
        glm::vec4 GridAxisColorZ = glm::vec4(0.2f, 0.2f, 0.8f, 0.7f);
        glm::vec4 GridColor = glm::vec4(0.5f, 0.5f, 0.5f, 0.35f);
        glm::vec4 GridMajorColor = glm::vec4(0.6f, 0.6f, 0.6f, 0.5f);
        int GridMajorLineInterval = 10;
        bool GridShowAxisLines = true;
        int GridPlaneOrientation = 0;
        float GizmoBaseScale = 1.0f;
        bool GizmoAutoScale = true;
        float GizmoMinScreenSize = 80.0f;
        float GizmoMaxScreenSize = 200.0f;
        bool ShowHoverOutline = false;
        glm::vec4 HoverOutlineColor = glm::vec4(1.0f, 0.8f, 0.2f, 0.6f);
        float HoverOutlineThickness = 2.0f;
    };

    void SaveNow() const;
    static EditorPreferenceGroup NormalizeGroup(EditorPreferenceGroup group);

private:
    std::filesystem::path m_FilePath;
    bool m_Loaded = false;
    bool m_Dirty = false;
    float m_UIScale = 1.0f;
    std::string m_LayoutIni;
    std::unordered_map<std::string, int> m_ActionUsageCounts;
    std::unordered_map<std::string, bool> m_WindowOpenStates;
    ViewportSnapshot m_Viewport;
    uint64_t m_GroupRevisions[static_cast<size_t>(EditorPreferenceGroup::Count)] = {};
    std::chrono::steady_clock::time_point m_SaveDeadline{};
};

} // namespace editorui
