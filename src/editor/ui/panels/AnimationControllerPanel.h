#pragma once

#include "editor/ui/panels/EditorPanel.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cm { namespace animation {
    struct AnimationAsset;
    struct AssetScriptEvent;
    struct AssetScriptEventTrack;
} }

class AnimationClipPreviewWindow;

class AnimationControllerPanel : public EditorPanel {
public:
    AnimationControllerPanel();
    ~AnimationControllerPanel();

    void OnImGuiRender();
    void Open() { m_Open = true; }
    void SetOpen(bool open) { m_Open = open; }
    bool IsOpen() const { return m_Open; }

private:
    bool LoadClip(const std::string& path);
    bool SaveClip(const std::string& path);
    void RefreshAnimListIfNeeded();

    void DrawHeader();
    void DrawPlaybackControls();
    void DrawTimeline();
    void DrawEventInspector();
    void DrawPreviewWindow();

    void UpdatePlayback(float deltaSeconds);
    void EnsureScriptTrackExists();
    void BuildTrackCache();
    float ClipDurationSeconds() const;
    float SnapTime(float t) const;
    void BeginEventDrag(std::size_t trackIdx, std::size_t eventIdx);
    void ClearSelection();
    void SyncSelectionPayload();
    void ApplyPayloadBuffer();
    void AddEventAt(std::size_t trackIdx, float timeSeconds);
    void RemoveEvent(std::size_t trackIdx, std::size_t eventIdx);
    cm::animation::AssetScriptEvent* GetSelectedEvent();
    void ClampScroll(float duration, float visibleSpan);
    void MarkDirty() { m_Dirty = true; }

    struct ScriptTrackRef {
        std::size_t assetIndex = 0;
        cm::animation::AssetScriptEventTrack* track = nullptr;
    };

    struct EventSelection {
        std::size_t track = SIZE_MAX;
        std::size_t event = SIZE_MAX;
        bool IsValid() const { return track != SIZE_MAX && event != SIZE_MAX; }
    };

    struct AnimOption {
        std::string name;
        std::string path;
    };

    std::unique_ptr<cm::animation::AnimationAsset> m_Clip;
    std::string m_CurrentPath;
    bool m_Dirty = false;

    std::vector<ScriptTrackRef> m_ScriptTracks;
    EventSelection m_SelectedEvent;
    EventSelection m_BufferedSelection;

    uint64_t m_NextEventId = 1;
    uint64_t m_NextTrackId = 1;

    struct TimelineState {
        bool Playing = false;
        bool Loop = true;
        bool SnapToFrame = true;
        float Zoom = 1.0f;
        float Scroll = 0.0f;
        float CurrentTime = 0.0f;
        float PlaySpeed = 1.0f;
        bool Scrubbing = false;
        bool DraggingEvent = false;
        EventSelection DragTarget;
    } m_Timeline;

    std::string m_PathBuffer;
    std::string m_ClipNameBuffer;
    std::string m_PayloadBuffer;
    bool m_PayloadDirty = false;
    std::string m_LastPayloadError;
    std::vector<AnimOption> m_AnimOptions;
    std::string m_AnimCacheRoot;
    double m_AnimCacheBuildTime = 0.0;
    int m_SelectedAnimIndex = -1;
    bool m_AnimCacheDirty = true;

    bool m_Open = false;
    bool m_PreviewOpen = false;
    std::unique_ptr<AnimationClipPreviewWindow> m_PreviewWindow;
};


