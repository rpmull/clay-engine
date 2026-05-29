#include "EditorPreferences.h"

#include "EditorActionRegistry.h"

#include <algorithm>
#include <fstream>

#include <nlohmann/json.hpp>

namespace editorui {
namespace {

using json = nlohmann::json;

json Vec4ToJson(const glm::vec4& value)
{
    return json::array({ value.x, value.y, value.z, value.w });
}

glm::vec4 JsonToVec4(const json& value, const glm::vec4& fallback)
{
    if (!value.is_array() || value.size() < 4) {
        return fallback;
    }
    return glm::vec4(
        value[0].get<float>(),
        value[1].get<float>(),
        value[2].get<float>(),
        value[3].get<float>());
}

} // namespace

bool EditorPreferences::Load(const std::filesystem::path& projectRoot)
{
    m_FilePath = projectRoot / ".library" / "editor" / "editor_ui.json";
    m_Loaded = true;
    m_Dirty = false;
    m_UIScale = 1.0f;
    m_LayoutIni.clear();
    m_ActionUsageCounts.clear();
    m_WindowOpenStates.clear();
    m_Viewport = ViewportSnapshot{};
    std::fill(std::begin(m_GroupRevisions), std::end(m_GroupRevisions), 0ull);

    if (!std::filesystem::exists(m_FilePath)) {
        return true;
    }

    std::ifstream input(m_FilePath);
    if (!input.is_open()) {
        return false;
    }

    json root;
    try {
        input >> root;
    } catch (...) {
        return false;
    }

    m_UIScale = root.value("uiScale", 1.0f);
    m_LayoutIni = root.value("layoutIni", std::string());

    if (root.contains("actionUsage") && root["actionUsage"].is_object()) {
        for (auto it = root["actionUsage"].begin(); it != root["actionUsage"].end(); ++it) {
            m_ActionUsageCounts[it.key()] = it.value().get<int>();
        }
    }

    if (root.contains("openWindows") && root["openWindows"].is_object()) {
        for (auto it = root["openWindows"].begin(); it != root["openWindows"].end(); ++it) {
            m_WindowOpenStates[it.key()] = it.value().get<bool>();
        }
    }

    if (root.contains("viewport") && root["viewport"].is_object()) {
        const json& vp = root["viewport"];
        m_Viewport.ZoomBaseSpeed = vp.value("zoomBaseSpeed", m_Viewport.ZoomBaseSpeed);
        m_Viewport.ZoomAcceleration = vp.value("zoomAcceleration", m_Viewport.ZoomAcceleration);
        m_Viewport.ZoomMinDistance = vp.value("zoomMinDistance", m_Viewport.ZoomMinDistance);
        m_Viewport.ZoomMaxDistance = vp.value("zoomMaxDistance", m_Viewport.ZoomMaxDistance);
        m_Viewport.SmoothZoomEnabled = vp.value("smoothZoomEnabled", m_Viewport.SmoothZoomEnabled);
        m_Viewport.ZoomSmoothness = vp.value("zoomSmoothness", m_Viewport.ZoomSmoothness);
        m_Viewport.OrbitSensitivity = vp.value("orbitSensitivity", m_Viewport.OrbitSensitivity);
        m_Viewport.PanSpeedFactor = vp.value("panSpeedFactor", m_Viewport.PanSpeedFactor);
        m_Viewport.FocusDuration = vp.value("focusDuration", m_Viewport.FocusDuration);
        m_Viewport.FocusDistancePadding = vp.value("focusDistancePadding", m_Viewport.FocusDistancePadding);
        m_Viewport.FocusDefaultDistance = vp.value("focusDefaultDistance", m_Viewport.FocusDefaultDistance);
        m_Viewport.DeselectOnEmptyClick = vp.value("deselectOnEmptyClick", m_Viewport.DeselectOnEmptyClick);
        m_Viewport.PickingTolerance = vp.value("pickingTolerance", m_Viewport.PickingTolerance);
        m_Viewport.PickingSkipHidden = vp.value("pickingSkipHidden", m_Viewport.PickingSkipHidden);
        m_Viewport.PickingSkipLocked = vp.value("pickingSkipLocked", m_Viewport.PickingSkipLocked);
        m_Viewport.GridAxisColorX = JsonToVec4(vp.value("gridAxisColorX", json()), m_Viewport.GridAxisColorX);
        m_Viewport.GridAxisColorY = JsonToVec4(vp.value("gridAxisColorY", json()), m_Viewport.GridAxisColorY);
        m_Viewport.GridAxisColorZ = JsonToVec4(vp.value("gridAxisColorZ", json()), m_Viewport.GridAxisColorZ);
        m_Viewport.GridColor = JsonToVec4(vp.value("gridColor", json()), m_Viewport.GridColor);
        m_Viewport.GridMajorColor = JsonToVec4(vp.value("gridMajorColor", json()), m_Viewport.GridMajorColor);
        m_Viewport.GridMajorLineInterval = vp.value("gridMajorLineInterval", m_Viewport.GridMajorLineInterval);
        m_Viewport.GridShowAxisLines = vp.value("gridShowAxisLines", m_Viewport.GridShowAxisLines);
        m_Viewport.GridPlaneOrientation = vp.value("gridPlaneOrientation", m_Viewport.GridPlaneOrientation);
        m_Viewport.GizmoBaseScale = vp.value("gizmoBaseScale", m_Viewport.GizmoBaseScale);
        m_Viewport.GizmoAutoScale = vp.value("gizmoAutoScale", m_Viewport.GizmoAutoScale);
        m_Viewport.GizmoMinScreenSize = vp.value("gizmoMinScreenSize", m_Viewport.GizmoMinScreenSize);
        m_Viewport.GizmoMaxScreenSize = vp.value("gizmoMaxScreenSize", m_Viewport.GizmoMaxScreenSize);
        m_Viewport.ShowHoverOutline = vp.value("showHoverOutline", m_Viewport.ShowHoverOutline);
        m_Viewport.HoverOutlineColor = JsonToVec4(vp.value("hoverOutlineColor", json()), m_Viewport.HoverOutlineColor);
        m_Viewport.HoverOutlineThickness = vp.value("hoverOutlineThickness", m_Viewport.HoverOutlineThickness);
    }

    return true;
}

void EditorPreferences::ApplyViewportSettings(EditorSettings& settings) const
{
    settings.ZoomBaseSpeed = m_Viewport.ZoomBaseSpeed;
    settings.ZoomAcceleration = m_Viewport.ZoomAcceleration;
    settings.ZoomMinDistance = m_Viewport.ZoomMinDistance;
    settings.ZoomMaxDistance = m_Viewport.ZoomMaxDistance;
    settings.SmoothZoomEnabled = m_Viewport.SmoothZoomEnabled;
    settings.ZoomSmoothness = m_Viewport.ZoomSmoothness;
    settings.OrbitSensitivity = m_Viewport.OrbitSensitivity;
    settings.PanSpeedFactor = m_Viewport.PanSpeedFactor;
    settings.FocusDuration = m_Viewport.FocusDuration;
    settings.FocusDistancePadding = m_Viewport.FocusDistancePadding;
    settings.FocusDefaultDistance = m_Viewport.FocusDefaultDistance;
    settings.DeselectOnEmptyClick = m_Viewport.DeselectOnEmptyClick;
    settings.PickingTolerance = m_Viewport.PickingTolerance;
    settings.PickingSkipHidden = m_Viewport.PickingSkipHidden;
    settings.PickingSkipLocked = m_Viewport.PickingSkipLocked;
    settings.GridAxisColorX = m_Viewport.GridAxisColorX;
    settings.GridAxisColorY = m_Viewport.GridAxisColorY;
    settings.GridAxisColorZ = m_Viewport.GridAxisColorZ;
    settings.GridColor = m_Viewport.GridColor;
    settings.GridMajorColor = m_Viewport.GridMajorColor;
    settings.GridMajorLineInterval = m_Viewport.GridMajorLineInterval;
    settings.GridShowAxisLines = m_Viewport.GridShowAxisLines;
    settings.GridPlaneOrientation = m_Viewport.GridPlaneOrientation;
    settings.GizmoBaseScale = m_Viewport.GizmoBaseScale;
    settings.GizmoAutoScale = m_Viewport.GizmoAutoScale;
    settings.GizmoMinScreenSize = m_Viewport.GizmoMinScreenSize;
    settings.GizmoMaxScreenSize = m_Viewport.GizmoMaxScreenSize;
    settings.ShowHoverOutline = m_Viewport.ShowHoverOutline;
    settings.HoverOutlineColor = m_Viewport.HoverOutlineColor;
    settings.HoverOutlineThickness = m_Viewport.HoverOutlineThickness;
}

void EditorPreferences::CaptureViewportSettings(const EditorSettings& settings)
{
    m_Viewport.ZoomBaseSpeed = settings.ZoomBaseSpeed;
    m_Viewport.ZoomAcceleration = settings.ZoomAcceleration;
    m_Viewport.ZoomMinDistance = settings.ZoomMinDistance;
    m_Viewport.ZoomMaxDistance = settings.ZoomMaxDistance;
    m_Viewport.SmoothZoomEnabled = settings.SmoothZoomEnabled;
    m_Viewport.ZoomSmoothness = settings.ZoomSmoothness;
    m_Viewport.OrbitSensitivity = settings.OrbitSensitivity;
    m_Viewport.PanSpeedFactor = settings.PanSpeedFactor;
    m_Viewport.FocusDuration = settings.FocusDuration;
    m_Viewport.FocusDistancePadding = settings.FocusDistancePadding;
    m_Viewport.FocusDefaultDistance = settings.FocusDefaultDistance;
    m_Viewport.DeselectOnEmptyClick = settings.DeselectOnEmptyClick;
    m_Viewport.PickingTolerance = settings.PickingTolerance;
    m_Viewport.PickingSkipHidden = settings.PickingSkipHidden;
    m_Viewport.PickingSkipLocked = settings.PickingSkipLocked;
    m_Viewport.GridAxisColorX = settings.GridAxisColorX;
    m_Viewport.GridAxisColorY = settings.GridAxisColorY;
    m_Viewport.GridAxisColorZ = settings.GridAxisColorZ;
    m_Viewport.GridColor = settings.GridColor;
    m_Viewport.GridMajorColor = settings.GridMajorColor;
    m_Viewport.GridMajorLineInterval = settings.GridMajorLineInterval;
    m_Viewport.GridShowAxisLines = settings.GridShowAxisLines;
    m_Viewport.GridPlaneOrientation = settings.GridPlaneOrientation;
    m_Viewport.GizmoBaseScale = settings.GizmoBaseScale;
    m_Viewport.GizmoAutoScale = settings.GizmoAutoScale;
    m_Viewport.GizmoMinScreenSize = settings.GizmoMinScreenSize;
    m_Viewport.GizmoMaxScreenSize = settings.GizmoMaxScreenSize;
    m_Viewport.ShowHoverOutline = settings.ShowHoverOutline;
    m_Viewport.HoverOutlineColor = settings.HoverOutlineColor;
    m_Viewport.HoverOutlineThickness = settings.HoverOutlineThickness;
}

void EditorPreferences::SetUIScale(float scale)
{
    m_UIScale = scale;
    RequestSave(EditorPreferenceGroup::Workspace);
}

void EditorPreferences::SetLayoutIni(std::string layoutIni)
{
    m_LayoutIni = std::move(layoutIni);
    RequestSave(EditorPreferenceGroup::Workspace);
}

void EditorPreferences::SetActionUsageCounts(std::unordered_map<std::string, int> usageCounts)
{
    m_ActionUsageCounts = std::move(usageCounts);
    RequestSave(EditorPreferenceGroup::Actions);
}

void EditorPreferences::CaptureWindowStates(const EditorActionRegistry& registry)
{
    m_WindowOpenStates.clear();
    for (const EditorActionDefinition* action : registry.GetPersistedActions()) {
        if (!action || !action->IsChecked) {
            continue;
        }
        m_WindowOpenStates[action->Id] = action->IsChecked();
    }
}

void EditorPreferences::RestoreWindowStates(EditorActionRegistry& registry) const
{
    for (const auto& entry : m_WindowOpenStates) {
        registry.SetChecked(entry.first, entry.second);
    }
}

void EditorPreferences::RequestSave(EditorPreferenceGroup group)
{
    const EditorPreferenceGroup normalized = NormalizeGroup(group);
    ++m_GroupRevisions[static_cast<size_t>(normalized)];
    m_Dirty = true;
    m_SaveDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
}

void EditorPreferences::SaveIfDue()
{
    if (!m_Dirty || !m_Loaded) {
        return;
    }
    if (std::chrono::steady_clock::now() < m_SaveDeadline) {
        return;
    }

    SaveNow();
    m_Dirty = false;
    m_SaveDeadline = std::chrono::steady_clock::time_point{};
}

uint64_t EditorPreferences::GetRevision(EditorPreferenceGroup group) const
{
    return m_GroupRevisions[static_cast<size_t>(NormalizeGroup(group))];
}

void EditorPreferences::SaveNow() const
{
    std::error_code ec;
    std::filesystem::create_directories(m_FilePath.parent_path(), ec);

    json root;
    root["version"] = 1;
    root["uiScale"] = m_UIScale;
    root["layoutIni"] = m_LayoutIni;
    root["actionUsage"] = m_ActionUsageCounts;
    root["openWindows"] = m_WindowOpenStates;

    json viewport;
    viewport["zoomBaseSpeed"] = m_Viewport.ZoomBaseSpeed;
    viewport["zoomAcceleration"] = m_Viewport.ZoomAcceleration;
    viewport["zoomMinDistance"] = m_Viewport.ZoomMinDistance;
    viewport["zoomMaxDistance"] = m_Viewport.ZoomMaxDistance;
    viewport["smoothZoomEnabled"] = m_Viewport.SmoothZoomEnabled;
    viewport["zoomSmoothness"] = m_Viewport.ZoomSmoothness;
    viewport["orbitSensitivity"] = m_Viewport.OrbitSensitivity;
    viewport["panSpeedFactor"] = m_Viewport.PanSpeedFactor;
    viewport["focusDuration"] = m_Viewport.FocusDuration;
    viewport["focusDistancePadding"] = m_Viewport.FocusDistancePadding;
    viewport["focusDefaultDistance"] = m_Viewport.FocusDefaultDistance;
    viewport["deselectOnEmptyClick"] = m_Viewport.DeselectOnEmptyClick;
    viewport["pickingTolerance"] = m_Viewport.PickingTolerance;
    viewport["pickingSkipHidden"] = m_Viewport.PickingSkipHidden;
    viewport["pickingSkipLocked"] = m_Viewport.PickingSkipLocked;
    viewport["gridAxisColorX"] = Vec4ToJson(m_Viewport.GridAxisColorX);
    viewport["gridAxisColorY"] = Vec4ToJson(m_Viewport.GridAxisColorY);
    viewport["gridAxisColorZ"] = Vec4ToJson(m_Viewport.GridAxisColorZ);
    viewport["gridColor"] = Vec4ToJson(m_Viewport.GridColor);
    viewport["gridMajorColor"] = Vec4ToJson(m_Viewport.GridMajorColor);
    viewport["gridMajorLineInterval"] = m_Viewport.GridMajorLineInterval;
    viewport["gridShowAxisLines"] = m_Viewport.GridShowAxisLines;
    viewport["gridPlaneOrientation"] = m_Viewport.GridPlaneOrientation;
    viewport["gizmoBaseScale"] = m_Viewport.GizmoBaseScale;
    viewport["gizmoAutoScale"] = m_Viewport.GizmoAutoScale;
    viewport["gizmoMinScreenSize"] = m_Viewport.GizmoMinScreenSize;
    viewport["gizmoMaxScreenSize"] = m_Viewport.GizmoMaxScreenSize;
    viewport["showHoverOutline"] = m_Viewport.ShowHoverOutline;
    viewport["hoverOutlineColor"] = Vec4ToJson(m_Viewport.HoverOutlineColor);
    viewport["hoverOutlineThickness"] = m_Viewport.HoverOutlineThickness;
    root["viewport"] = std::move(viewport);

    std::ofstream output(m_FilePath, std::ios::trunc);
    if (!output.is_open()) {
        return;
    }
    output << root.dump(2);
}

EditorPreferenceGroup EditorPreferences::NormalizeGroup(EditorPreferenceGroup group)
{
    const size_t index = static_cast<size_t>(group);
    if (index >= static_cast<size_t>(EditorPreferenceGroup::Count)) {
        return EditorPreferenceGroup::Workspace;
    }
    return group;
}

} // namespace editorui
