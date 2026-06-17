// AnimationInspector.cpp
#include "editor/panels/AnimationInspector.h"
#include "ui/UILayer.h"
#include "editor/ui/panels/ProjectPanel.h"
#include "editor/preview/PreviewScene.h"
#include "editor/preview/PreviewAvatarCache.h"
// Keep include here to ensure complete type for unique_ptr destruction in this TU
#include "editor/preview/AnimationPreviewPlayer.h"
#include "core/animation/AnimationSerializer.h"
#include "core/animation/AvatarSerializer.h"
#include "core/animation/HumanoidBone.h"
#include "editor/pipeline/AssetLibrary.h"
#include "editor/pipeline/AnimationImportSettings.h"
#include "editor/pipeline/BinaryAssetCache.h"
#include "editor/import/AnimationImporter.h"
#include "core/animation/AnimationAssetCache.h"
#include "core/ecs/Scene.h"
#include <utils/Time.h>
#include <filesystem>
#include <iostream>
#include <glm/gtc/quaternion.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

using cm::animation::AnimationClip;

namespace {
const char* RootMotionModeLabel(cm::animation::RootMotionMode mode)
{
    switch (mode) {
        case cm::animation::RootMotionMode::None: return "None";
        case cm::animation::RootMotionMode::InPlace: return "In-Place";
        case cm::animation::RootMotionMode::ApplyToEntity: return "Apply To Entity";
        default: return "Unknown";
    }
}
}

AnimationInspectorPanel::AnimationInspectorPanel(UILayer* uiLayer)
    : m_UILayer(uiLayer)
{
    m_Preview = std::make_unique<PreviewScene>();
    m_AvatarCache = std::make_unique<PreviewAvatarCache>();
    m_Player = std::make_unique<AnimationPreviewPlayer>();
}

AnimationInspectorPanel::~AnimationInspectorPanel() = default;

bool AnimationInspectorPanel::IsVisible() const {
    // Inspector window visibility is handled outside; this panel renders inside it on demand
    return true;
}

void AnimationInspectorPanel::LoadClip(const std::string& path)
{
    m_CurrentClipPath = path;
    
    // Load import settings from meta file
    AnimationImportSettings::LoadFromMeta(path, m_ImportSettings);
    m_ImportSettingsDirty = false;
    
    // Prefer unified asset; fall back to legacy clip for compatibility
    auto asset = cm::animation::LoadAnimationAsset(path);
    bool loadedAsset = !asset.tracks.empty();
    auto clip = loadedAsset ? cm::animation::AnimationClip{} : cm::animation::LoadAnimationClip(path);
    auto [unusedModel, unusedSkel, humanoid] = m_AvatarCache->ResolveForClip(clip);
    m_Preview->Shutdown();
    m_Preview->Init(480, 320);
    // For humanoid clips, always preview on our default mannequin
    if (clip.IsHumanoid) {
        m_Preview->SetModelPath("assets/prefabs/default_humanoid.fbx");
        m_Preview->ResetCamera();
    }
    if (loadedAsset) {
        m_CurrentAsset = std::make_unique<cm::animation::AnimationAsset>(std::move(asset));
        m_CurrentClip.reset();
        m_Player->SetClip(nullptr);
        m_Player->SetAsset(m_CurrentAsset.get());
    } else {
        m_CurrentAsset.reset();
        auto clipPtr = std::make_shared<AnimationClip>(clip);
        m_CurrentClip = clipPtr;
        m_Player->SetClip(clipPtr);
        m_Player->SetAsset(nullptr);
    }
    m_Player->SetSkeleton(m_Preview->GetSkeleton());
    m_Player->SetScene(m_Preview->GetScene());
    // If no skeleton/model is available yet, ensure a default model is loaded
    if (!m_Preview->GetSkeleton()) {
        m_Preview->SetModelPath("assets/prefabs/default_humanoid.fbx");
        m_Player->SetSkeleton(m_Preview->GetSkeleton());
    }
    m_Player->SetLoop(m_Loop);
    m_Player->SetSpeed(m_Speed);
    if (humanoid) m_Player->SetRetargetMap(humanoid);
}

void AnimationInspectorPanel::OnImGuiRender()
{
    // Determine current selection from Project panel
    if (m_UILayer) {
        const std::string selPath = m_UILayer->GetProjectPanel().GetSelectedItemPath();
        std::string ext = std::filesystem::path(selPath).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".anim" && selPath != m_CurrentClipPath) {
            LoadClip(selPath);
        }
    }

    // Render only if selected item is an animation
    // Caller should have gated this, but keep a light guard in case
    ImGui::TextUnformatted("Animation");
    ImGui::Separator();

    // Top row controls
    if (ImGui::Checkbox("Play", &m_Playing)) {}
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &m_Loop);
    ImGui::SameLine();
    ImGui::SliderFloat("Speed", &m_Speed, 0.1f, 2.0f, "%.2fx");
    ImGui::Checkbox("Show Bones", &m_ShowBones);
    ImGui::SameLine();
    ImGui::Checkbox("Wireframe", &m_Wireframe);
    ImGui::SameLine();
    ImGui::Checkbox("Auto-rebuild on asset change", &m_AutoRebuildOnChange);

    // Timeline
    float duration = m_Player->GetDuration();
    float t = m_Player->GetTime();
    if (ImGui::SliderFloat(m_ShowFrames ? "Frame" : "Time", &t, 0.0f, std::max(0.001f, duration))) m_Player->SetTime(t);

    // Import Settings Chip
    DrawImportSettingsChip();

    if (IsHumanoidAnimation()) {
        DrawAvatarMappingPanel();
    }
}

bool AnimationInspectorPanel::IsHumanoidAnimation() const
{
    if (m_CurrentAsset) {
        for (const auto& track : m_CurrentAsset->tracks) {
            if (track && track->type == cm::animation::TrackType::Avatar) return true;
        }
    }
    if (m_CurrentClip) return m_CurrentClip->IsHumanoid;
    return false;
}

void AnimationInspectorPanel::DrawAvatarMappingPanel()
{
    ImGui::Separator();
    ImGui::TextDisabled("Avatar Mapping");
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float desiredHeight = std::max(140.0f, avail.y * 0.35f);
    ImGui::BeginChild("AnimAvatarMapping", ImVec2(-1, desiredHeight), true, ImGuiWindowFlags_NoScrollbar);

    std::string avatarPath;
    if (!m_ImportSettings.SourceRigModelPath.empty()) {
        std::filesystem::path src(m_ImportSettings.SourceRigModelPath);
        avatarPath = (src.parent_path() / (src.stem().string() + ".avatar")).string();
    } else if (!m_ImportSettings.SourceFilePath.empty()) {
        std::filesystem::path src(m_ImportSettings.SourceFilePath);
        avatarPath = (src.parent_path() / (src.stem().string() + ".avatar")).string();
    }

    cm::animation::AvatarDefinition avatar;
    bool hasAvatar = !avatarPath.empty() && cm::animation::LoadAvatar(avatar, avatarPath);

    std::unordered_set<int> trackedHumanoidIds;
    if (m_CurrentAsset) {
        for (const auto& track : m_CurrentAsset->tracks) {
            if (track && track->type == cm::animation::TrackType::Avatar) {
                auto* avatarTrack = static_cast<cm::animation::AssetAvatarTrack*>(track.get());
                if (avatarTrack->humanBoneId >= 0) trackedHumanoidIds.insert(avatarTrack->humanBoneId);
            }
        }
    } else if (m_CurrentClip) {
        for (const auto& [id, bt] : m_CurrentClip->HumanoidTracks) {
            (void)bt;
            trackedHumanoidIds.insert(id);
        }
    }

    if (!hasAvatar) {
        ImGui::TextDisabled("No avatar mapping found for this clip.");
        if (!avatarPath.empty()) {
            ImGui::TextDisabled("Expected: %s", avatarPath.c_str());
        }
        ImGui::EndChild();
        return;
    }

    if (ImGui::BeginTable("AvatarMapTable", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Humanoid Bone");
        ImGui::TableSetupColumn("Mapped Bone");
        ImGui::TableSetupColumn("Status");
        ImGui::TableHeadersRow();

        for (int id = 0; id < (int)cm::animation::HumanoidBoneCount; ++id) {
            if (!trackedHumanoidIds.empty() && trackedHumanoidIds.count(id) == 0) continue;
            const auto& entry = avatar.Map[(size_t)id];
            bool mapped = avatar.Present[(size_t)id] && !entry.BoneName.empty() && entry.BoneIndex >= 0;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(cm::animation::ToString(static_cast<cm::animation::HumanoidBone>(id)));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(mapped ? entry.BoneName.c_str() : "(unmapped)");
            ImGui::TableNextColumn();
            if (mapped) {
                ImGui::TextUnformatted("Mapped");
            } else {
                ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "Missing");
            }
        }
        ImGui::EndTable();
    }

    ImGui::EndChild();
}

void AnimationInspectorPanel::DrawImportSettingsChip()
{
    ImGui::Separator();
    
    // Collapsible import settings section
    if (ImGui::CollapsingHeader("Import Settings", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent(8.0f);
        
        // ===== Rotation Correction =====
        ImGui::TextDisabled("Rotation Correction");
        
        int rotationMode = static_cast<int>(m_ImportSettings.XAxisCorrection);
        bool rotationChanged = false;
        
        if (ImGui::RadioButton("None##rot", rotationMode == 0)) { rotationMode = 0; rotationChanged = true; }
        ImGui::SameLine();
        if (ImGui::RadioButton("+90\xc2\xb0 X##rot", rotationMode == 1)) { rotationMode = 1; rotationChanged = true; }
        ImGui::SameLine();
        if (ImGui::RadioButton("-90\xc2\xb0 X##rot", rotationMode == 2)) { rotationMode = 2; rotationChanged = true; }
        
        if (rotationChanged)
        {
            m_ImportSettings.XAxisCorrection = static_cast<AnimationImportSettings::RotationCorrection>(rotationMode);
            m_ImportSettingsDirty = true;
        }
        
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Apply rotation correction to fix axis orientation mismatches.\nUseful for animations imported with wrong up-axis.");
        
        ImGui::Spacing();

        ImGui::TextDisabled("Humanoid Helpers");
        ImGui::TextDisabled("Source Rig Model");
        if (!m_ImportSettings.SourceRigModelPath.empty()) {
            ImGui::TextWrapped("%s", m_ImportSettings.SourceRigModelPath.c_str());
        } else {
            ImGui::TextDisabled("(not set)");
        }
        if (ImGui::Button("Select Model##sourceRigModel")) {
            std::string picked = ShowOpenFileDialogExt(L"Model (*.fbx)", L"fbx");
            if (!picked.empty()) {
                m_ImportSettings.SourceRigModelPath = picked;
                m_ImportSettingsDirty = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear##sourceRigModel")) {
            m_ImportSettings.SourceRigModelPath.clear();
            m_ImportSettingsDirty = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Optional mesh-backed rig used to resolve bind pose for skeleton-only animations.");
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Target Avatar Model");
        if (!m_ImportSettings.SourceAvatarModelPath.empty()) {
            ImGui::TextWrapped("%s", m_ImportSettings.SourceAvatarModelPath.c_str());
        } else {
            ImGui::TextDisabled("(not set)");
        }
        if (ImGui::Button("Select Model##targetAvatarModel")) {
            std::string picked = ShowOpenFileDialogExt(L"Model (*.fbx)", L"fbx");
            if (!picked.empty()) {
                m_ImportSettings.SourceAvatarModelPath = picked;
                m_ImportSettingsDirty = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear##targetAvatarModel")) {
            m_ImportSettings.SourceAvatarModelPath.clear();
            m_ImportSettingsDirty = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Bake this animation to a target rig's bind pose during import.");
        }

        // ===== Root Motion Settings =====
        ImGui::TextDisabled("Root Motion");
        
        // Root motion mode selector
        const char* modeItems[] = { "None (Static)", "In-Place (Keep Y)", "Apply to Entity (Physics)" };
        int currentMode = static_cast<int>(m_ImportSettings.RootMotionMode);
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::Combo("Mode##rootmotion", &currentMode, modeItems, 3))
        {
            m_ImportSettings.RootMotionMode = static_cast<cm::animation::RootMotionMode>(currentMode);
            m_ImportSettingsDirty = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "None: Character stays completely static (root/hips zeroed)\n"
                "In-Place: Character stays in XZ but bounces in Y (for foot IK etc)\n"
                "Apply to Entity: Root motion drives physics/CharacterController");
        
        // Show additional options only when ApplyToEntity is selected
        if (m_ImportSettings.RootMotionMode == cm::animation::RootMotionMode::ApplyToEntity)
        {
            ImGui::Indent(16.0f);
            
            if (ImGui::Checkbox("Include XZ Motion", &m_ImportSettings.RootMotionIncludeXZ))
                m_ImportSettingsDirty = true;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Apply horizontal (forward/backward, strafe) motion from animation.");
            
            if (!m_ImportSettings.RootMotionIncludeY) {
                m_ImportSettings.RootMotionIncludeY = true;
                m_ImportSettingsDirty = true;
            }
            ImGui::TextDisabled("Vertical motion is always applied from animation.");
            
            if (ImGui::Checkbox("Include Rotation", &m_ImportSettings.RootMotionIncludeRotation))
                m_ImportSettingsDirty = true;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Apply rotation from root bone (turn-in-place animations).");

            if (ImGui::Checkbox("Override Gravity", &m_ImportSettings.RootMotionOverrideGravity))
                m_ImportSettingsDirty = true;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Disable gravity while this animation plays.\nUseful for climbing, ledge grabs, or other animations\nwhere Y motion should be absolute.");

            ImGui::Unindent(16.0f);
        }
        
        // Root motion bone name (optional override)
        char boneBuf[128];
        strncpy(boneBuf, m_ImportSettings.RootMotionBoneName.c_str(), sizeof(boneBuf) - 1);
        boneBuf[sizeof(boneBuf) - 1] = '\0';
        
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::InputText("Root Bone##rootmotion", boneBuf, sizeof(boneBuf)))
        {
            m_ImportSettings.RootMotionBoneName = boneBuf;
            m_ImportSettingsDirty = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Optional override bone for root motion extraction.\nLeave empty for automatic humanoid selection (Root, then Hips fallback).");
        
        // Show current animation's root motion info if loaded
        if (m_CurrentAsset)
        {
            const auto& rm = m_CurrentAsset->meta.rootMotion;
            const auto desiredRootMotion = m_ImportSettings.ToRootMotionSettings();
            if (rm != desiredRootMotion)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 196, 64, 255));
                ImGui::TextWrapped(
                    "Baked runtime clip still uses %s root motion. Save Settings will update the .anim metadata; full reimport is still required for axis or rig changes.",
                    RootMotionModeLabel(rm.Mode));
                ImGui::PopStyleColor();
            }
            if (rm.TotalDistanceXZ > 0.01f || rm.TotalDistanceY > 0.01f)
            {
                ImGui::TextDisabled("Baked: XZ=%.2fm Y=%.2fm", rm.TotalDistanceXZ, rm.TotalDistanceY);
            }
        }
        
        ImGui::Spacing();
        
        // ===== Source File Info =====
        if (!m_ImportSettings.SourceFilePath.empty())
        {
            ImGui::TextDisabled("Source: %s", m_ImportSettings.SourceFilePath.c_str());
            if (m_ImportSettings.SourceAnimationIndex > 0)
                ImGui::TextDisabled("Animation Index: %d", m_ImportSettings.SourceAnimationIndex);
        }
        
        ImGui::Spacing();
        
        // ===== Action Buttons =====
        bool canReimport = !m_ImportSettings.SourceFilePath.empty() && 
                           std::filesystem::exists(m_ImportSettings.SourceFilePath);
        
        ImGui::BeginDisabled(!canReimport);
        if (ImGui::Button("Reimport with Settings"))
        {
            ReimportAnimation();
        }
        ImGui::EndDisabled();
        
        if (!canReimport && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Source file not found. Reimport requires the original FBX/glTF file.");
        ImGui::SameLine();
        
        // Save settings without reimport
        ImGui::BeginDisabled(!m_ImportSettingsDirty);
        if (ImGui::Button("Save Settings"))
        {
            bool saved = AnimationImportSettings::SaveToMeta(m_CurrentClipPath, m_ImportSettings);
            if (saved && m_CurrentAsset)
                saved = SaveBakedRootMotionSettings();
            if (saved)
                m_ImportSettingsDirty = false;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Saves the sidecar import settings. Root motion settings are also baked into the current .anim asset; axis correction and rig-retarget changes still require reimport.");
        ImGui::EndDisabled();
        
        if (!canReimport && m_CurrentAsset)
        {
            ImGui::TextDisabled("Source file missing. Save Settings will still bake root motion metadata into this .anim asset.");
        }
        
        // Calculate root motion from current animation (without reimport)
        if (m_ImportSettings.RootMotionMode == cm::animation::RootMotionMode::ApplyToEntity)
        {
            ImGui::SameLine();
            if (ImGui::Button("Calculate Root Motion"))
            {
                AnimationRootMotion rootMotion;
                if (CalculateRootMotion(rootMotion))
                {
                    // Display calculated values
                    ImGui::OpenPopup("RootMotionResult");
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Calculate and preview root motion from current animation data.\nDoes not require reimport - uses existing animation tracks.");
        }
        
        // Root motion result popup
        if (ImGui::BeginPopup("RootMotionResult"))
        {
            ImGui::Text("Root Motion Calculated:");
            AnimationRootMotion rm;
            if (CalculateRootMotion(rm))
            {
                ImGui::Text("XZ Distance: %.2f units", rm.TotalDistanceXZ);
                ImGui::Text("Y Distance: %.2f units", rm.TotalDistanceY);
                ImGui::Text("Keyframes: %zu", rm.Keys.size());
            }
            ImGui::EndPopup();
        }
        
        ImGui::Unindent(8.0f);
    }
}

void AnimationInspectorPanel::ReimportAnimation()
{
    if (m_ImportSettings.SourceFilePath.empty() || m_CurrentClipPath.empty())
        return;
    
    // Save current settings first
    AnimationImportSettings::SaveToMeta(m_CurrentClipPath, m_ImportSettings);
    m_ImportSettingsDirty = false;
    
    // Re-import from source file with settings applied
    // This delegates to AnimationImporter which will read and apply the settings
    bool success = cm::animation::AnimationImporter::ImportUnifiedAnimationFromFBXWithSettings(
        m_ImportSettings.SourceFilePath,
        m_CurrentClipPath,
        m_ImportSettings
    );
    
    if (success)
    {
        // Clear in-memory animation caches so controllers/play mode reload fresh data.
        cm::animation::ClearAnimationAssetCache();
        Scene::Get().InvalidateAllAnimatorAssetCaches();

        // Delete any legacy .animc file to avoid stale data being loaded
        std::filesystem::path animcPath = std::filesystem::path(m_CurrentClipPath).replace_extension(".animc");
        if (std::filesystem::exists(animcPath)) {
            std::error_code ec;
            std::filesystem::remove(animcPath, ec);
            if (!ec) {
                std::cout << "[AnimationInspector] Deleted legacy .animc file: " << animcPath << std::endl;
            }
        }
        
        // Rebuild the binary cache (.animbin) so play mode uses the updated animation
        std::string binaryPath = BinaryAssetCache::Instance().GetBinaryPath(m_CurrentClipPath);
        if (!binaryPath.empty())
        {
            if (BinaryAssetCache::Instance().BuildAnimationBinary(m_CurrentClipPath, binaryPath))
            {
                std::cout << "[AnimationInspector] Rebuilt animation binary: " << binaryPath << std::endl;
            }
            else
            {
                std::cerr << "[AnimationInspector] Failed to rebuild animation binary" << std::endl;
            }
        }
        
        // Reload the clip to see changes
        LoadClip(m_CurrentClipPath);
        std::cout << "[AnimationInspector] Reimport successful: " << m_CurrentClipPath << std::endl;
    }
    else
    {
        std::cerr << "[AnimationInspector] Reimport failed for: " << m_CurrentClipPath << std::endl;
    }
}

bool AnimationInspectorPanel::SaveBakedRootMotionSettings()
{
    if (m_CurrentClipPath.empty() || !m_CurrentAsset)
        return false;

    m_CurrentAsset->meta.rootMotion = m_ImportSettings.ToRootMotionSettings();
    m_CurrentAsset->meta.rootMotion.TotalDistanceXZ = 0.0f;
    m_CurrentAsset->meta.rootMotion.TotalDistanceY = 0.0f;

    if (m_CurrentAsset->meta.rootMotion.Mode == cm::animation::RootMotionMode::ApplyToEntity)
    {
        AnimationRootMotion rootMotion;
        if (CalculateRootMotion(rootMotion))
        {
            m_CurrentAsset->meta.rootMotion.TotalDistanceXZ = rootMotion.TotalDistanceXZ;
            m_CurrentAsset->meta.rootMotion.TotalDistanceY = rootMotion.TotalDistanceY;
        }
    }

    if (!cm::animation::SaveAnimationAsset(*m_CurrentAsset, m_CurrentClipPath))
    {
        std::cerr << "[AnimationInspector] Failed to save baked root motion metadata for: "
                  << m_CurrentClipPath << std::endl;
        return false;
    }

    std::string binaryPath = BinaryAssetCache::Instance().GetBinaryPath(m_CurrentClipPath);
    if (!binaryPath.empty() &&
        !BinaryAssetCache::Instance().BuildAnimationBinary(m_CurrentClipPath, binaryPath))
    {
        std::cerr << "[AnimationInspector] Warning: failed to refresh animation binary cache for: "
                  << m_CurrentClipPath << std::endl;
    }

    cm::animation::ClearAnimationAssetCache();
    Scene::Get().InvalidateAllAnimatorAssetCaches();
    LoadClip(m_CurrentClipPath);
    std::cout << "[AnimationInspector] Updated baked root motion metadata: "
              << m_CurrentClipPath << std::endl;
    return true;
}

bool AnimationInspectorPanel::CalculateRootMotion(AnimationRootMotion& outRootMotion)
{
    outRootMotion = AnimationRootMotion{};
    
    if (!m_CurrentAsset && !m_CurrentClip)
        return false;
    
    // Find the root motion bone
    std::string rootBoneName = m_ImportSettings.RootMotionBoneName;
    
    // Auto-detect common root bone names if not specified
    static const char* kRootBoneNames[] = {
        "Root", "Hips", "mixamorig:Hips", "mixamorig_Hips",
        "Armature", "Bip001 Pelvis", "pelvis", "root"
    };
    
    const cm::animation::CurveVec3* positionCurve = nullptr;
    
    if (m_CurrentAsset)
    {
        // Search in unified asset tracks
        for (const auto& track : m_CurrentAsset->tracks)
        {
            if (!track) continue;
            
            bool isMatch = false;
            if (!rootBoneName.empty())
            {
                isMatch = (track->name == rootBoneName);
            }
            else
            {
                // Auto-detect
                for (const char* name : kRootBoneNames)
                {
                    if (track->name.find(name) != std::string::npos)
                    {
                        isMatch = true;
                        break;
                    }
                }
            }
            
            if (isMatch)
            {
                // Get position curve based on track type
                if (track->type == cm::animation::TrackType::Bone)
                {
                    auto* boneTrack = static_cast<cm::animation::AssetBoneTrack*>(track.get());
                    positionCurve = &boneTrack->t;
                }
                else if (track->type == cm::animation::TrackType::Avatar)
                {
                    auto* avatarTrack = static_cast<cm::animation::AssetAvatarTrack*>(track.get());
                    positionCurve = &avatarTrack->t;
                }
                break;
            }
        }
    }
    else if (m_CurrentClip)
    {
        // Search in legacy clip bone tracks
        for (const auto& [boneName, boneTrack] : m_CurrentClip->BoneTracks)
        {
            bool isMatch = false;
            if (!rootBoneName.empty())
            {
                isMatch = (boneName == rootBoneName);
            }
            else
            {
                for (const char* name : kRootBoneNames)
                {
                    if (boneName.find(name) != std::string::npos)
                    {
                        isMatch = true;
                        break;
                    }
                }
            }
            
            if (isMatch && !boneTrack.PositionKeys.empty())
            {
                // Convert legacy keyframes to root motion keys
                glm::vec3 prevPos(0.0f);
                bool first = true;
                
                for (const auto& key : boneTrack.PositionKeys)
                {
                    if (first)
                    {
                        prevPos = key.Value;
                        first = false;
                        continue;
                    }
                    
                    AnimationRootMotion::RootKey rmKey;
                    rmKey.time = key.Time;
                    rmKey.deltaX = key.Value.x - prevPos.x;
                    rmKey.deltaY = key.Value.y - prevPos.y;
                    rmKey.deltaZ = key.Value.z - prevPos.z;
                    
                    // Always accumulate distances for display
                    outRootMotion.TotalDistanceXZ += std::sqrt(rmKey.deltaX * rmKey.deltaX + rmKey.deltaZ * rmKey.deltaZ);
                    outRootMotion.TotalDistanceY += std::abs(rmKey.deltaY);
                    
                    outRootMotion.Keys.push_back(rmKey);
                    prevPos = key.Value;
                }
                return !outRootMotion.Keys.empty();
            }
        }
    }
    
    // Process position curve from unified asset
    if (positionCurve && !positionCurve->keys.empty())
    {
        glm::vec3 prevPos(0.0f);
        bool first = true;
        
        for (const auto& key : positionCurve->keys)
        {
            if (first)
            {
                prevPos = key.v;
                first = false;
                continue;
            }
            
            AnimationRootMotion::RootKey rmKey;
            rmKey.time = key.t;
            rmKey.deltaX = key.v.x - prevPos.x;
            rmKey.deltaY = key.v.y - prevPos.y;
            rmKey.deltaZ = key.v.z - prevPos.z;
            
            // Always accumulate distances for display (the settings just control what gets applied at runtime)
            outRootMotion.TotalDistanceXZ += std::sqrt(rmKey.deltaX * rmKey.deltaX + rmKey.deltaZ * rmKey.deltaZ);
            outRootMotion.TotalDistanceY += std::abs(rmKey.deltaY);
            
            outRootMotion.Keys.push_back(rmKey);
            prevPos = key.v;
        }
        return !outRootMotion.Keys.empty();
    }
    
    return false;
}
