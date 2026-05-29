// AnimationInspector.h
#pragma once

#include <string>
#include <memory>
#include <bgfx/bgfx.h>
#include <imgui.h>
#include "editor/pipeline/AnimationImportSettings.h"

namespace cm { namespace animation { struct AnimationClip; struct AnimationAsset; } }
class UILayer;

class PreviewScene;
class PreviewAvatarCache;
#include "editor/preview/AnimationPreviewPlayer.h"

class AnimationInspectorPanel {
public:
    explicit AnimationInspectorPanel(UILayer* uiLayer);
    ~AnimationInspectorPanel();

    void OnImGuiRender();

private:
    void LoadClip(const std::string& path);
    bool IsVisible() const;
    bool IsHumanoidAnimation() const;
    void DrawAvatarMappingPanel();
    
    // Import settings UI and actions
    void DrawImportSettingsChip();
    void ReimportAnimation();
    bool CalculateRootMotion(AnimationRootMotion& outRootMotion);

private:
    UILayer* m_UILayer = nullptr; // non-owning
    std::unique_ptr<PreviewScene> m_Preview;
    std::unique_ptr<PreviewAvatarCache> m_AvatarCache;
    std::unique_ptr<AnimationPreviewPlayer> m_Player;

    std::string m_CurrentClipPath;
    std::shared_ptr<cm::animation::AnimationClip> m_CurrentClip;
    std::unique_ptr<cm::animation::AnimationAsset> m_CurrentAsset;
    bool m_Playing = true;
    bool m_Loop = true;
    bool m_ShowBones = false;
    bool m_Wireframe = false;
    bool m_AutoRebuildOnChange = true;
    float m_Speed = 1.0f;
    bool m_ShowFrames = false;
    int m_LastKnownWidth = 0;
    int m_LastKnownHeight = 0;
    
    // Import settings
    AnimationImportSettings m_ImportSettings;
    bool m_ImportSettingsDirty = false;
};


