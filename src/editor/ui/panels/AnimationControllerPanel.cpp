#include "AnimationControllerPanel.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <sstream>

#include "core/animation/AnimationAsset.h"
#include "core/animation/AnimationSerializer.h"
#include "core/animation/AnimationTypes.h"
#include "editor/animation/AnimationClipPreviewWindow.h"
#include "editor/Project.h"
#include "editor/ui/AssetPicker.h"
#include "editor/ui/FileDialogs.h"

using cm::animation::AnimationAsset;
using cm::animation::AssetScriptEvent;
using cm::animation::AssetScriptEventTrack;
using cm::animation::TrackType;
using nlohmann::json;

// -----------------------------------------------------------------------------
// Animation clip authoring overview (2025-11-26)
// -----------------------------------------------------------------------------
//  * `cm::animation::AnimationAsset` stores tracks, curves, and script events.
//    Asset data lives on disk via `AnimationSerializer`, so edits must mutate
//    the asset directly for persistence.
//  * This panel exposes a workflow around a single clip: a loader,
//    playback controls with scrubber, event tracks for trigger editing, and a
//    dedicated humanoid preview window (AnimationClipPreviewWindow) which renders
//    inside its own ImGui pane without touching the main editor scene.
//  * Flow: user selects/loads an animation asset → timeline edits update
//    AssetScriptEventTrack data → Save writes through AnimationSerializer →
//    preview uses the same asset + scrub time to drive AnimationPreviewPlayer.
// -----------------------------------------------------------------------------

namespace {

struct InputTextCallbackUserData {
    std::string* Str;
};

int InputTextStringCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* user = static_cast<InputTextCallbackUserData*>(data->UserData);
        user->Str->resize(data->BufTextLen);
        data->Buf = user->Str->data();
    }
    return 0;
}

bool InputTextString(const char* label, std::string& str, ImGuiInputTextFlags flags = 0) {
    if (str.capacity() == 0) str.reserve(64);
    flags |= ImGuiInputTextFlags_CallbackResize;
    InputTextCallbackUserData data{ &str };
    return ImGui::InputText(label, str.data(), str.capacity() + 1, flags, InputTextStringCallback, &data);
}

bool InputTextMultilineString(const char* label, std::string& str, const ImVec2& size, ImGuiInputTextFlags flags = 0) {
    if (str.capacity() == 0) str.reserve(64);
    flags |= ImGuiInputTextFlags_CallbackResize;
    InputTextCallbackUserData data{ &str };
    return ImGui::InputTextMultiline(label, str.data(), str.capacity() + 1, size, flags, InputTextStringCallback, &data);
}

float DetermineTickStep(float visibleSpanSeconds) {
    static constexpr float kSteps[] = {
        0.01f, 0.02f, 0.05f,
        0.1f, 0.2f, 0.5f,
        1.0f, 2.0f, 5.0f,
        10.0f, 20.0f, 30.0f,
        60.0f
    };
    const float desiredTickCount = 8.0f;
    float target = visibleSpanSeconds / desiredTickCount;
    constexpr std::size_t kCount = sizeof(kSteps) / sizeof(kSteps[0]);
    for (std::size_t i = 0; i < kCount; ++i) {
        if (kSteps[i] >= target) return kSteps[i];
    }
    return kSteps[kCount - 1];
}

} // namespace

AnimationControllerPanel::AnimationControllerPanel() {
    m_PathBuffer.reserve(512);
    m_ClipNameBuffer.reserve(128);
    m_PayloadBuffer.reserve(1024);
    m_PayloadBuffer = "{}";
}

AnimationControllerPanel::~AnimationControllerPanel() = default;

void AnimationControllerPanel::OnImGuiRender() {
    if (!m_Open) return;

    if (!ImGui::Begin("Animation Controller", &m_Open)) {
        ImGui::End();
        DrawPreviewWindow();
        return;
    }
    DrawHeader();
    ImGui::Separator();

    if (!m_Clip) {
        ImGui::TextDisabled("No animation clip loaded.");
        ImGui::TextWrapped("Click Open to load an existing .anim or press New Clip to start from an empty asset.");
        if (ImGui::Button("New Clip")) {
            m_Clip = std::make_unique<AnimationAsset>();
            m_Clip->name = "NewClip";
            m_Clip->meta.version = 1;
            m_Clip->meta.fps = 30.0f;
            m_Clip->meta.length = 2.0f;
            m_CurrentPath.clear();
            m_PathBuffer.clear();
            m_ClipNameBuffer = m_Clip->name;
            m_Timeline = {};
            m_Timeline.Loop = true;
            m_Timeline.SnapToFrame = true;
            m_Timeline.PlaySpeed = 1.0f;
            m_PayloadBuffer = "{}";
            m_PayloadDirty = false;
            m_LastPayloadError.clear();
            m_Dirty = true;
            m_NextEventId = 1;
            m_NextTrackId = 1;
            EnsureScriptTrackExists();
        }
        ImGui::End();
        return;
    }

    DrawPlaybackControls();
    ImGui::Separator();
    DrawTimeline();
    ImGui::Separator();
    DrawEventInspector();
    ImGui::End();

    DrawPreviewWindow();
}

bool AnimationControllerPanel::LoadClip(const std::string& path) {
    try {
        auto asset = std::make_unique<AnimationAsset>(cm::animation::LoadAnimationAsset(path));
        if (asset->tracks.empty()) {
            auto legacy = cm::animation::LoadAnimationClip(path);
            *asset = cm::animation::WrapLegacyClipAsAsset(legacy);
        }
        m_Clip = std::move(asset);
    } catch (...) {
        return false;
    }

    m_CurrentPath = path;
    m_PathBuffer = path;
    m_ClipNameBuffer = m_Clip ? m_Clip->name : std::string{};
    m_Timeline = {};
    m_Timeline.Loop = true;
    m_Timeline.SnapToFrame = true;
    m_Timeline.PlaySpeed = 1.0f;
    m_SelectedEvent = {};
    m_BufferedSelection = {};
    m_PayloadBuffer = "{}";
    m_PayloadDirty = false;
    m_LastPayloadError.clear();
    m_Dirty = false;
    m_NextEventId = 1;
    m_NextTrackId = 1;

    if (m_Clip) {
        for (auto& track : m_Clip->tracks) {
            if (!track) continue;
            m_NextTrackId = std::max<uint64_t>(m_NextTrackId, track->id + 1);
            if (track->type != TrackType::ScriptEvent) continue;
            auto* scriptTrack = static_cast<AssetScriptEventTrack*>(track.get());
            for (auto& ev : scriptTrack->events) {
                if (ev.id == 0) ev.id = m_NextEventId++;
                m_NextEventId = std::max<uint64_t>(m_NextEventId, ev.id + 1);
            }
        }
    }

    EnsureScriptTrackExists();
    if (m_PreviewWindow) m_PreviewWindow->SuggestModel(path);
    return true;
}

bool AnimationControllerPanel::SaveClip(const std::string& path) {
    if (!m_Clip) return false;
    bool ok = cm::animation::SaveAnimationAsset(*m_Clip, path);
    if (ok) {
        m_CurrentPath = path;
        m_PathBuffer = path;
        m_Dirty = false;
        m_AnimCacheDirty = true;
    }
    return ok;
}

void AnimationControllerPanel::RefreshAnimListIfNeeded() {
    constexpr double kRefreshInterval = 1.0;
    const double now = ImGui::GetTime();
    auto root = Project::GetAssetDirectory();
    if (root.empty()) root = std::filesystem::path("assets");
    const std::string rootStr = root.string();
    const bool rootChanged = (rootStr != m_AnimCacheRoot);

    if (!m_AnimCacheDirty && !rootChanged && (now - m_AnimCacheBuildTime) < kRefreshInterval) {
        return;
    }

    m_AnimCacheDirty = false;
    m_AnimCacheBuildTime = now;
    m_AnimCacheRoot = rootStr;
    m_AnimOptions.clear();

    if (std::filesystem::exists(root)) {
        for (auto& p : std::filesystem::recursive_directory_iterator(root)) {
            if (!p.is_regular_file()) continue;
            auto ext = p.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".anim") {
                m_AnimOptions.push_back({ p.path().stem().string(), p.path().string() });
            }
        }
    }
    std::sort(m_AnimOptions.begin(), m_AnimOptions.end(), [](const AnimOption& a, const AnimOption& b) {
        return a.name < b.name;
    });
}

void AnimationControllerPanel::DrawHeader() {
    ImGui::TextUnformatted("Clip File");
    ImGui::SameLine();
    if (m_Dirty) {
        ImGui::TextColored(ImVec4(0.98f, 0.75f, 0.2f, 1.0f), "*");
        ImGui::SameLine();
        ImGui::TextDisabled("(unsaved)");
    }

    // Animation clip dropdown
    {
        RefreshAnimListIfNeeded();

        // Sync selection to current path
        m_SelectedAnimIndex = -1;
        for (int i = 0; i < (int)m_AnimOptions.size(); ++i) {
            if (m_AnimOptions[i].path == m_CurrentPath) {
                m_SelectedAnimIndex = i;
                break;
            }
        }

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 260.0f);
        const char* currentLabel = (m_SelectedAnimIndex >= 0 ? m_AnimOptions[m_SelectedAnimIndex].name.c_str() : "<Select Animation Clip>");
        if (ImGui::BeginCombo("##ClipFile", currentLabel)) {
            if (ImGui::Selectable("<None>", m_SelectedAnimIndex == -1)) {
                m_SelectedAnimIndex = -1;
                m_CurrentPath.clear();
                m_PathBuffer.clear();
                m_Clip.reset();
                m_Dirty = false;
            }
            for (int i = 0; i < (int)m_AnimOptions.size(); ++i) {
                bool isSel = (i == m_SelectedAnimIndex);
                if (ImGui::Selectable(m_AnimOptions[i].name.c_str(), isSel)) {
                    m_SelectedAnimIndex = i;
                    LoadClip(m_AnimOptions[i].path);
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        if (m_CurrentPath.empty()) {
            std::string path = ShowSaveFileDialogExt(L"NewAnimation.anim", L"Animation Assets (*.anim)", L"anim");
            if (!path.empty()) SaveClip(path);
        } else {
            SaveClip(m_CurrentPath);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Save As")) {
        std::string path = ShowSaveFileDialogExt(L"Animation.anim", L"Animation Assets (*.anim)", L"anim");
        if (!path.empty()) SaveClip(path);
    }
}

void AnimationControllerPanel::DrawPlaybackControls() {
    if (!m_Clip) return;

    InputTextString("Clip Name", m_ClipNameBuffer);
    if (m_Clip->name != m_ClipNameBuffer) {
        m_Clip->name = m_ClipNameBuffer;
        MarkDirty();
    }

    float duration = ClipDurationSeconds();
    float lengthField = m_Clip->meta.length;
    if (ImGui::DragFloat("Duration (seconds)", &lengthField, 0.01f, 0.0f, 1200.0f, "%.3f")) {
        m_Clip->meta.length = std::max(0.0f, lengthField);
        MarkDirty();
    }
    float fpsField = m_Clip->meta.fps;
    if (ImGui::DragFloat("Frames per second", &fpsField, 0.1f, 1.0f, 240.0f, "%.1f")) {
        m_Clip->meta.fps = std::max(1.0f, fpsField);
        MarkDirty();
    }

    if (ImGui::Checkbox("Loop Playback", &m_Timeline.Loop)) {
        if (!m_Timeline.Loop) m_Timeline.Playing = false;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Snap to frames", &m_Timeline.SnapToFrame);
    ImGui::SameLine();
    ImGui::Checkbox("Preview Window", &m_PreviewOpen);

    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragFloat("Speed", &m_Timeline.PlaySpeed, 0.01f, 0.1f, 4.0f, "%.2fx");

    UpdatePlayback(ImGui::GetIO().DeltaTime);
    float t = std::clamp(m_Timeline.CurrentTime, 0.0f, duration);
    if (ImGui::SliderFloat("Current Time", &t, 0.0f, std::max(0.001f, duration), "%.3fs")) {
        m_Timeline.CurrentTime = SnapTime(t);
        m_Timeline.Playing = false;
    }

    if (ImGui::Button(m_Timeline.Playing ? "Pause" : "Play")) {
        if (duration > 0.0f) {
            m_Timeline.Playing = !m_Timeline.Playing;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop")) {
        m_Timeline.Playing = false;
        m_Timeline.CurrentTime = 0.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Preview Humanoid")) {
        m_PreviewOpen = true;
        if (!m_PreviewWindow) m_PreviewWindow = std::make_unique<AnimationClipPreviewWindow>();
        if (!m_CurrentPath.empty()) m_PreviewWindow->SuggestModel(m_CurrentPath);
    }
}

void AnimationControllerPanel::UpdatePlayback(float deltaSeconds) {
    if (!m_Clip || !m_Timeline.Playing) return;
    float duration = ClipDurationSeconds();
    if (duration <= 0.0f) {
        m_Timeline.Playing = false;
        return;
    }
    m_Timeline.CurrentTime += deltaSeconds * m_Timeline.PlaySpeed;
    if (m_Timeline.Loop) {
        while (m_Timeline.CurrentTime > duration) m_Timeline.CurrentTime -= duration;
        while (m_Timeline.CurrentTime < 0.0f) m_Timeline.CurrentTime += duration;
    } else {
        if (m_Timeline.CurrentTime > duration) {
            m_Timeline.CurrentTime = duration;
            m_Timeline.Playing = false;
        }
        if (m_Timeline.CurrentTime < 0.0f) m_Timeline.CurrentTime = 0.0f;
    }
}

void AnimationControllerPanel::DrawTimeline() {
    if (!m_Clip) return;

    BuildTrackCache();

    const float duration = ClipDurationSeconds();
    float visible = std::max(0.1f, duration / std::max(0.1f, m_Timeline.Zoom));
    ClampScroll(duration, visible);
    float startTime = m_Timeline.Scroll;
    float endTime = startTime + visible;

    ImVec2 region = ImGui::GetContentRegionAvail();
    float canvasHeight = std::max(200.0f, region.y * 0.65f);
    ImGui::BeginChild("TimelineCanvas", ImVec2(region.x, canvasHeight), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    float laneWidth = ImGui::GetContentRegionAvail().x;
    float headerHeight = 26.0f;
    float laneHeight = 38.0f;
    float totalTrackHeight = std::max(1u, (unsigned)m_ScriptTracks.size()) * laneHeight;
    float canvasWidthClamped = std::max(10.0f, laneWidth);
    float pxPerSecond = canvasWidthClamped / visible;

    ImGui::InvisibleButton("TimelineSurface", ImVec2(canvasWidthClamped, headerHeight + totalTrackHeight),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);

    // Background
    ImVec2 headerMin = canvasPos;
    ImVec2 headerMax = ImVec2(canvasPos.x + canvasWidthClamped, canvasPos.y + headerHeight);
    drawList->AddRectFilled(headerMin, headerMax, IM_COL32(28, 28, 28, 255));
    drawList->AddRect(headerMin, headerMax, IM_COL32(64, 64, 64, 255));

    // Tick marks
    float tickStep = DetermineTickStep(visible);
    float firstTick = std::floor(startTime / tickStep) * tickStep;
    for (float t = firstTick; t < endTime + tickStep; t += tickStep) {
        float x = canvasPos.x + (t - startTime) * pxPerSecond;
        drawList->AddLine(ImVec2(x, headerMin.y), ImVec2(x, headerMax.y), IM_COL32(90, 90, 90, 255));
        std::ostringstream ss;
        ss.setf(std::ios::fixed, std::ios::floatfield);
        ss.precision(2);
        ss << std::max(0.0f, t);
        drawList->AddText(ImVec2(x + 2.0f, headerMin.y + 4.0f), IM_COL32_WHITE, ss.str().c_str());
    }

    // Handle zoom/pan interactions
    const ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
        float wheel = io.MouseWheel;
        if (wheel != 0.0f) {
            if (io.KeyCtrl) {
                float zoomMul = 1.0f + wheel * 0.2f;
                m_Timeline.Zoom = std::clamp(m_Timeline.Zoom * zoomMul, 0.2f, 32.0f);
                visible = std::max(0.1f, duration / m_Timeline.Zoom);
                float mouseTime = startTime + std::clamp((io.MousePos.x - canvasPos.x) / pxPerSecond, 0.0f, visible);
                m_Timeline.Scroll = mouseTime - visible * 0.5f;
                ClampScroll(duration, visible);
                startTime = m_Timeline.Scroll;
                endTime = startTime + visible;
            } else {
                m_Timeline.Scroll -= wheel * (visible * 0.1f);
                ClampScroll(duration, visible);
                startTime = m_Timeline.Scroll;
                endTime = startTime + visible;
            }
        }
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) && std::abs(io.MouseDelta.x) > 0.0f) {
            m_Timeline.Scroll -= io.MouseDelta.x / pxPerSecond;
            ClampScroll(duration, visible);
            startTime = m_Timeline.Scroll;
            endTime = startTime + visible;
        }
    }

    // Tracks
    bool clickedEvent = false;
    for (std::size_t trackIdx = 0; trackIdx < m_ScriptTracks.size(); ++trackIdx) {
        const float laneTop = headerMax.y + trackIdx * laneHeight;
        ImVec2 laneMin(canvasPos.x, laneTop);
        ImVec2 laneMax(canvasPos.x + canvasWidthClamped, laneTop + laneHeight);
        drawList->AddRectFilled(laneMin, laneMax, IM_COL32(20, 20, 20, 255));
        drawList->AddRect(laneMin, laneMax, IM_COL32(46, 46, 46, 255));

        auto* track = m_ScriptTracks[trackIdx].track;
        if (!track) continue;
        drawList->AddText(ImVec2(laneMin.x + 6.0f, laneMin.y + 4.0f), IM_COL32(200, 200, 200, 255), track->name.c_str());

        for (std::size_t eventIdx = 0; eventIdx < track->events.size(); ++eventIdx) {
            const auto& ev = track->events[eventIdx];
            if (ev.time < startTime || ev.time > endTime) continue;
            float x = canvasPos.x + (ev.time - startTime) * pxPerSecond;
            float y0 = laneMin.y + 12.0f;
            float y1 = laneMax.y - 6.0f;
            bool selected = m_SelectedEvent.track == trackIdx && m_SelectedEvent.event == eventIdx;
            ImU32 color = selected ? IM_COL32(255, 205, 120, 255) : IM_COL32(120, 190, 255, 255);
            drawList->AddTriangleFilled(ImVec2(x, y0), ImVec2(x - 6.0f, y1), ImVec2(x + 6.0f, y1), color);
            ImVec2 hitMin(x - 8.0f, laneMin.y);
            ImVec2 hitMax(x + 8.0f, laneMax.y);
            if (ImGui::IsMouseHoveringRect(hitMin, hitMax)) {
                ImGui::BeginTooltip();
                ImGui::Text("%s::%s (%.3fs)", ev.className.c_str(), ev.method.c_str(), ev.time);
                ImGui::EndTooltip();
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    m_SelectedEvent = {trackIdx, eventIdx};
                    SyncSelectionPayload();
                    BeginEventDrag(trackIdx, eventIdx);
                    clickedEvent = true;
                }
            }
        }

        // Lane interactions (add event via double click)
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hovered) {
            if (ImGui::IsMouseHoveringRect(laneMin, laneMax)) {
                float newTime = startTime + std::clamp((io.MousePos.x - laneMin.x) / pxPerSecond, 0.0f, visible);
                AddEventAt(trackIdx, newTime);
                clickedEvent = true;
            }
        }

        // Track context menu
        ImGui::PushID((int)trackIdx);
        if (ImGui::BeginPopupContextItem("TrackCtx")) {
            static char renameBuf[64] = {};
            std::snprintf(renameBuf, sizeof(renameBuf), "%s", track->name.c_str());
            if (ImGui::InputText("Name", renameBuf, sizeof(renameBuf))) {
                track->name = renameBuf;
                MarkDirty();
            }
            if (ImGui::MenuItem("Delete Track", nullptr, false, m_ScriptTracks.size() > 1)) {
                m_Clip->tracks.erase(m_Clip->tracks.begin() + (long)m_ScriptTracks[trackIdx].assetIndex);
                BuildTrackCache();
                ClearSelection();
                MarkDirty();
                ImGui::EndPopup();
                ImGui::PopID();
                break;
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    // Scrubber interactions
    if (!clickedEvent && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        float newTime = startTime + std::clamp((io.MousePos.x - canvasPos.x) / pxPerSecond, 0.0f, visible);
        m_Timeline.CurrentTime = SnapTime(std::clamp(newTime, 0.0f, duration));
        m_Timeline.Playing = false;
        m_Timeline.Scrubbing = true;
    }
    if (m_Timeline.Scrubbing) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float newTime = startTime + std::clamp((io.MousePos.x - canvasPos.x) / pxPerSecond, 0.0f, visible);
            m_Timeline.CurrentTime = SnapTime(std::clamp(newTime, 0.0f, duration));
        } else {
            m_Timeline.Scrubbing = false;
        }
    }

    // Event dragging
    if (!m_Timeline.DraggingEvent && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.001f)) {
        if (m_Timeline.DragTarget.IsValid()) {
            m_Timeline.DraggingEvent = true;
        }
    }
    if (m_Timeline.DraggingEvent) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            auto& drag = m_Timeline.DragTarget;
            if (drag.track < m_ScriptTracks.size()) {
                auto* track = m_ScriptTracks[drag.track].track;
                if (track && drag.event < track->events.size()) {
                    float newTime = startTime + (io.MousePos.x - canvasPos.x) / pxPerSecond;
                    newTime = SnapTime(std::clamp(newTime, 0.0f, duration));
                    track->events[drag.event].time = newTime;
                    std::sort(track->events.begin(), track->events.end(),
                              [](const AssetScriptEvent& a, const AssetScriptEvent& b){ return a.time < b.time; });
                    for (std::size_t idx = 0; idx < track->events.size(); ++idx) {
                        if (track->events[idx].id == m_SelectedEvent.IsValid() ? track->events[m_SelectedEvent.event].id : track->events[idx].id) {
                            m_SelectedEvent.event = idx;
                            break;
                        }
                    }
                    MarkDirty();
                }
            }
        } else {
            m_Timeline.DraggingEvent = false;
            m_Timeline.DragTarget = {};
        }
    }

    // Scrubber line
    float scrubX = canvasPos.x + (m_Timeline.CurrentTime - startTime) * pxPerSecond;
    drawList->AddLine(ImVec2(scrubX, headerMin.y), ImVec2(scrubX, headerMin.y + headerHeight + totalTrackHeight),
                      IM_COL32(235, 120, 64, 255), 2.0f);

    ImGui::EndChild();

    if (ImGui::Button("+ Script Track")) {
        auto track = std::make_unique<AssetScriptEventTrack>();
        track->id = m_NextTrackId++;
        track->name = "Events";
        m_Clip->tracks.push_back(std::move(track));
        BuildTrackCache();
        MarkDirty();
    }
}

void AnimationControllerPanel::DrawEventInspector() {
    ImGui::BeginChild("EventInspector", ImVec2(0, 0), true);
    SyncSelectionPayload();
    auto* ev = GetSelectedEvent();
    if (!ev) {
        ImGui::TextDisabled("Select an event to edit its properties.");
        ImGui::EndChild();
        return;
    }

    auto* track = m_ScriptTracks[m_SelectedEvent.track].track;
    ImGui::Text("Track: %s", track ? track->name.c_str() : "<Unknown>");
    float duration = ClipDurationSeconds();
    if (ImGui::DragFloat("Time", &ev->time, 0.001f, 0.0f, duration, "%.3fs")) {
        ev->time = SnapTime(std::clamp(ev->time, 0.0f, duration));
        std::sort(track->events.begin(), track->events.end(),
                  [](const AssetScriptEvent& a, const AssetScriptEvent& b){ return a.time < b.time; });
        for (std::size_t idx = 0; idx < track->events.size(); ++idx) {
            if (track->events[idx].id == ev->id) {
                m_SelectedEvent.event = idx;
                break;
            }
        }
        MarkDirty();
    }

    char classBuf[128] = {};
    std::snprintf(classBuf, sizeof(classBuf), "%s", ev->className.c_str());
    if (ImGui::InputText("Script Class", classBuf, sizeof(classBuf))) {
        ev->className = classBuf;
        MarkDirty();
    }
    char methodBuf[128] = {};
    std::snprintf(methodBuf, sizeof(methodBuf), "%s", ev->method.c_str());
    if (ImGui::InputText("Method", methodBuf, sizeof(methodBuf))) {
        ev->method = methodBuf;
        MarkDirty();
    }

    ImGui::TextUnformatted("Payload (JSON)");
    ImVec2 payloadSize = ImVec2(ImGui::GetContentRegionAvail().x, std::max(120.0f, ImGui::GetContentRegionAvail().y * 0.5f));
    if (InputTextMultilineString("##payload", m_PayloadBuffer, payloadSize, ImGuiInputTextFlags_AllowTabInput)) {
        m_PayloadDirty = true;
    }
    if (m_PayloadDirty) {
        if (ImGui::Button("Apply Payload")) {
            ApplyPayloadBuffer();
        }
        if (!m_LastPayloadError.empty()) {
            ImGui::TextColored(ImVec4(0.98f, 0.35f, 0.35f, 1.0f), "%s", m_LastPayloadError.c_str());
        }
    }

    if (ImGui::Button("Delete Event")) {
        RemoveEvent(m_SelectedEvent.track, m_SelectedEvent.event);
    }

    ImGui::EndChild();
}

void AnimationControllerPanel::DrawPreviewWindow() {
    if (!m_PreviewOpen || !m_Clip) return;
    if (!m_PreviewWindow) m_PreviewWindow = std::make_unique<AnimationClipPreviewWindow>();
    m_PreviewWindow->Draw(m_PreviewOpen, m_Clip.get(), m_Timeline.CurrentTime, m_Timeline.Loop, m_Timeline.PlaySpeed);
}

void AnimationControllerPanel::EnsureScriptTrackExists() {
    if (!m_Clip) return;
    bool hasScript = false;
    for (const auto& track : m_Clip->tracks) {
        if (track && track->type == TrackType::ScriptEvent) {
            hasScript = true;
            break;
        }
    }
    if (!hasScript) {
        auto track = std::make_unique<AssetScriptEventTrack>();
        track->id = m_NextTrackId++;
        track->name = "Script Events";
        m_Clip->tracks.push_back(std::move(track));
    }
    BuildTrackCache();
}

void AnimationControllerPanel::BuildTrackCache() {
    m_ScriptTracks.clear();
    if (!m_Clip) return;
    for (std::size_t i = 0; i < m_Clip->tracks.size(); ++i) {
        auto* track = m_Clip->tracks[i].get();
        if (!track || track->type != TrackType::ScriptEvent) continue;
        m_ScriptTracks.push_back({i, static_cast<AssetScriptEventTrack*>(track)});
    }
    if (m_ScriptTracks.empty()) EnsureScriptTrackExists();
}

float AnimationControllerPanel::ClipDurationSeconds() const {
    if (!m_Clip) return 0.0f;
    if (m_Clip->meta.length > 0.0f) return m_Clip->meta.length;
    return std::max(0.001f, m_Clip->Duration());
}

float AnimationControllerPanel::SnapTime(float t) const {
    if (m_Timeline.SnapToFrame && m_Clip && m_Clip->meta.fps > 0.0f) {
        float frame = std::round(t * m_Clip->meta.fps);
        return frame / m_Clip->meta.fps;
    }
    return t;
}

void AnimationControllerPanel::BeginEventDrag(std::size_t trackIdx, std::size_t eventIdx) {
    m_Timeline.DraggingEvent = false;
    m_Timeline.DragTarget = {trackIdx, eventIdx};
}

void AnimationControllerPanel::ClearSelection() {
    m_SelectedEvent = {};
    m_BufferedSelection = {};
    m_PayloadBuffer = "{}";
    m_PayloadDirty = false;
    m_LastPayloadError.clear();
}

void AnimationControllerPanel::SyncSelectionPayload() {
    if (m_SelectedEvent.track == m_BufferedSelection.track &&
        m_SelectedEvent.event == m_BufferedSelection.event) return;

    auto* ev = GetSelectedEvent();
    if (!ev) {
        ClearSelection();
        return;
    }

    m_BufferedSelection = m_SelectedEvent;
    m_PayloadBuffer = ev->payload.dump(2);
    m_PayloadDirty = false;
    m_LastPayloadError.clear();
}

void AnimationControllerPanel::ApplyPayloadBuffer() {
    auto* ev = GetSelectedEvent();
    if (!ev) return;
    try {
        json parsed = json::parse(m_PayloadBuffer.empty() ? "{}" : m_PayloadBuffer);
        ev->payload = parsed;
        m_PayloadDirty = false;
        m_LastPayloadError.clear();
        MarkDirty();
    } catch (const std::exception& e) {
        m_LastPayloadError = e.what();
    }
}

void AnimationControllerPanel::AddEventAt(std::size_t trackIdx, float timeSeconds) {
    if (trackIdx >= m_ScriptTracks.size()) return;
    auto* track = m_ScriptTracks[trackIdx].track;
    if (!track) return;
    AssetScriptEvent ev;
    ev.id = m_NextEventId++;
    ev.time = SnapTime(timeSeconds);
    ev.className = "ScriptEvent";
    ev.method = "OnEvent";
    ev.payload = json::object();
    track->events.push_back(ev);
    std::sort(track->events.begin(), track->events.end(),
              [](const AssetScriptEvent& a, const AssetScriptEvent& b){ return a.time < b.time; });
    for (std::size_t idx = 0; idx < track->events.size(); ++idx) {
        if (track->events[idx].id == ev.id) {
            m_SelectedEvent = {trackIdx, idx};
            break;
        }
    }
    SyncSelectionPayload();
    MarkDirty();
}

void AnimationControllerPanel::RemoveEvent(std::size_t trackIdx, std::size_t eventIdx) {
    if (trackIdx >= m_ScriptTracks.size()) return;
    auto* track = m_ScriptTracks[trackIdx].track;
    if (!track || eventIdx >= track->events.size()) return;
    track->events.erase(track->events.begin() + (long)eventIdx);
    ClearSelection();
    MarkDirty();
}

AssetScriptEvent* AnimationControllerPanel::GetSelectedEvent() {
    if (!m_Clip || !m_SelectedEvent.IsValid()) return nullptr;
    if (m_SelectedEvent.track >= m_ScriptTracks.size()) return nullptr;
    auto* track = m_ScriptTracks[m_SelectedEvent.track].track;
    if (!track) return nullptr;
    if (m_SelectedEvent.event >= track->events.size()) return nullptr;
    return &track->events[m_SelectedEvent.event];
}

void AnimationControllerPanel::ClampScroll(float duration, float visibleSpan) {
    float maxScroll = std::max(0.0f, duration - visibleSpan);
    if (!std::isfinite(m_Timeline.Scroll)) m_Timeline.Scroll = 0.0f;
    if (m_Timeline.Scroll < 0.0f) m_Timeline.Scroll = 0.0f;
    if (m_Timeline.Scroll > maxScroll) m_Timeline.Scroll = maxScroll;
}
