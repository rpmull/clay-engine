#pragma once

#include <memory>
#include <string>
#include <vector>

struct ImVec2;

namespace cm { namespace animation { struct AnimationAsset; } }

class PreviewScene;
class AnimationPreviewPlayer;

// Dedicated humanoid/skinned animation preview window rendered inside ImGui.
// Owns a tiny PreviewScene + AnimationPreviewPlayer pair so it never touches
// the main editor scene or renderer views. The window is driven by the
// Animation Controller panel: each frame we receive the current clip pointer,
// playback time, loop flag, and speed so the preview stays in lockstep with
// the timeline scrubber.
class AnimationClipPreviewWindow {
public:
    AnimationClipPreviewWindow();
    ~AnimationClipPreviewWindow();

    // Draw the preview UI + viewport. The `open` flag behaves like ImGui::Begin:
    // when set to false the window closes itself.
    void Draw(bool& open,
              const cm::animation::AnimationAsset* asset,
              float playbackTimeSeconds,
              bool loop,
              float speed);

    void InvalidateModelList() { m_ModelListDirty = true; }
    void SuggestModel(const std::string& assetPathHint);

private:
    void EnsureScene();
    void EnsureModelList();
    void EnsureModelLoaded(const std::string& path);
    void SyncPlayer(const cm::animation::AnimationAsset* asset, float time, bool loop, float speed);
    void RefreshSkeletonBinding();
    void ResizeIfNeeded(const ImVec2& viewportSize);
    void DrawViewport(const ImVec2& viewportSize);
    void DrawModelPicker();
    std::string NormalizePath(const std::string& absolutePath) const;
    std::string ResolvePathForLoad(const std::string& path) const;
    bool LoadDefaultHumanoid();

private:
    std::unique_ptr<PreviewScene> m_PreviewScene;
    std::unique_ptr<AnimationPreviewPlayer> m_Player;

    const cm::animation::AnimationAsset* m_CurrentAsset = nullptr;
    std::string m_CurrentAssetPath;
    float m_LastAppliedTime = 0.0f;

    std::vector<std::string> m_ModelPaths;
    bool m_ModelListDirty = true;
    int m_SelectedModel = -1;
    std::string m_SelectedModelPath;
    std::string m_LastLoadedModelPath;

    int m_LastViewportWidth = 0;
    int m_LastViewportHeight = 0;
    bool m_ShowBones = false;
    bool m_Wireframe = false;
};


