// AnimationPreviewPlayer.cpp
#include "AnimationPreviewPlayer.h"
#include "core/animation/AnimationTypes.h"
#include "core/animation/AnimationEvaluator.h"
#include "core/animation/AnimationAsset.h"
#include "core/animation/BindingCache.h"
#include "editor/preview/PreviewContext.h"
#include "core/animation/HumanoidRetargeter.h"
#include "core/ecs/AnimationComponents.h"
#include "core/ecs/Scene.h"

#include <algorithm>
#include <cstdint>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>

using cm::animation::AnimationClip;

// -----------------------------------------------------------------------------
// Preview player data flow
// -----------------------------------------------------------------------------
// AnimationInspectorPanel (and future animation panels) own an instance of this
// helper to evaluate either legacy `AnimationClip` data or the unified
// `AnimationAsset` format against a temporary skeleton. The caller provides:
//   * `Scene` + `SkeletonComponent` that contain the preview bones.
//   * Optional humanoid avatars/retarget maps for clip↔skeleton remapping.
// Time, loop mode, and playback speed live entirely on this object. The editor
// drives playback via `Update()` / `SetTime()` and can either let the player
// write poses into a lightweight preview scene or request them explicitly with
// `SampleTo(PreviewContext&)` for fully manual rendering paths.
// -----------------------------------------------------------------------------

namespace {

glm::mat4 ComputeLocalBind(const SkeletonComponent& skeleton, int boneIndex)
{
    if (boneIndex < 0 || boneIndex >= (int)skeleton.InverseBindPoses.size()) return glm::mat4(1.0f);
    glm::mat4 invBind = skeleton.InverseBindPoses[boneIndex];
    glm::mat4 globalBind = glm::inverse(invBind);
    int parent = (boneIndex < (int)skeleton.BoneParents.size()) ? skeleton.BoneParents[boneIndex] : -1;
    glm::mat4 parentGlobal = glm::mat4(1.0f);
    if (parent >= 0 && parent < (int)skeleton.InverseBindPoses.size()) {
        parentGlobal = glm::inverse(skeleton.InverseBindPoses[parent]);
    }
    return glm::inverse(parentGlobal) * globalBind;
}

void FillMissingWithBind(const SkeletonComponent& skeleton,
                         std::vector<glm::mat4>& locals,
                         const std::vector<uint8_t>* touched)
{
    const int limit = std::min<int>((int)locals.size(), (int)skeleton.InverseBindPoses.size());
    for (int i = 0; i < limit; ++i) {
        bool needsBind = false;
        if (touched) {
            needsBind = (i >= (int)touched->size()) || !(*touched)[(size_t)i];
        } else {
            needsBind = (locals[i] == glm::mat4(1.0f));
        }
        if (needsBind) {
            locals[i] = ComputeLocalBind(skeleton, i);
        }
    }
}

void DecomposeTRS(const glm::mat4& m, glm::vec3& T, glm::quat& R, glm::vec3& S)
{
    T = glm::vec3(m[3]);
    glm::vec3 X = glm::vec3(m[0]);
    glm::vec3 Y = glm::vec3(m[1]);
    glm::vec3 Z = glm::vec3(m[2]);
    S = glm::vec3(glm::length(X), glm::length(Y), glm::length(Z));
    if (S.x > 1e-6f) X /= S.x;
    if (S.y > 1e-6f) Y /= S.y;
    if (S.z > 1e-6f) Z /= S.z;
    glm::mat3 rotMat(X, Y, Z);
    R = glm::quat_cast(rotMat);
}

void ApplyHumanoidConstraints(const SkeletonComponent& skeleton,
                              std::vector<glm::mat4>& locals)
{
    if (!skeleton.Avatar) return;
    int hipsIdx = skeleton.Avatar->GetMappedBoneIndex(cm::animation::HumanoidBone::Hips);
    int rootIdx = skeleton.Avatar->GetMappedBoneIndex(cm::animation::HumanoidBone::Root);
    for (int i = 0; i < (int)locals.size(); ++i) {
        if (i == hipsIdx || i == rootIdx) continue;
        glm::vec3 Ta, Sa; glm::quat Ra;
        DecomposeTRS(locals[i], Ta, Ra, Sa);
        glm::vec3 Tb, Sb; glm::quat Rb;
        DecomposeTRS(ComputeLocalBind(skeleton, i), Tb, Rb, Sb);
        glm::mat4 result = glm::mat4(1.0f);
        result = glm::translate(result, Tb);
        result *= glm::mat4_cast(glm::normalize(Ra));
        result = glm::scale(result, Sb);
        locals[i] = result;
    }
}

void ApplyLocalToTransform(Scene* scene, EntityID boneId, const glm::mat4& local)
{
    if (!scene || boneId == (EntityID)-1) return;
    if (auto* data = scene->GetEntityData(boneId)) {
        glm::vec3 translation;
        glm::vec3 scale;
        glm::vec3 skew;
        glm::vec4 perspective;
        glm::quat rotation;
        if (glm::decompose(local, scale, rotation, translation, skew, perspective)) {
            data->Transform.Position = translation;
            data->Transform.Scale = scale;
            data->Transform.RotationQ = glm::normalize(rotation);
            data->Transform.Rotation = glm::degrees(glm::eulerAngles(rotation));
            data->Transform.UseQuatRotation = true;
        } else {
            data->Transform.Position = glm::vec3(local[3]);
            data->Transform.Scale = glm::vec3(1.0f);
            data->Transform.RotationQ = glm::quat(1, 0, 0, 0);
            data->Transform.Rotation = glm::vec3(0.0f);
            data->Transform.UseQuatRotation = true;
        }
        data->Transform.TransformDirty = true;
        data->Transform.LocalMatrix = local;
    }
}

} // namespace

AnimationPreviewPlayer::AnimationPreviewPlayer() = default;
AnimationPreviewPlayer::~AnimationPreviewPlayer() = default;

void AnimationPreviewPlayer::SetClip(std::shared_ptr<AnimationClip> clip) { m_Clip = std::move(clip); m_Time = 0.0f; }
void AnimationPreviewPlayer::SetAsset(const cm::animation::AnimationAsset* asset) { m_Asset = asset; m_Time = 0.0f; }
void AnimationPreviewPlayer::SetSkeleton(SkeletonComponent* skel) { m_Skeleton = skel; }
void AnimationPreviewPlayer::SetAvatar(const cm::animation::AvatarDefinition* avatar, const SkeletonComponent* skeleton) { m_Humanoid = avatar; m_Skeleton = skeleton; }
void AnimationPreviewPlayer::SetLoop(bool loop) { m_Loop = loop; }
void AnimationPreviewPlayer::SetSpeed(float s) { m_Speed = s; }
void AnimationPreviewPlayer::SetRetargetMap(const cm::animation::AvatarDefinition* map) { m_Retarget = map; }
void AnimationPreviewPlayer::SetScene(Scene* scene) { m_Scene = scene; }
void AnimationPreviewPlayer::SetTime(float t) { m_Time = t; }
float AnimationPreviewPlayer::GetTime() const { return m_Time; }
float AnimationPreviewPlayer::GetDuration() const {
    if (m_Clip) return m_Clip->Duration;
    if (m_Asset) return m_Asset->Duration();
    return 0.0f;
}

void AnimationPreviewPlayer::Update(float dt)
{
    m_Time += dt * m_Speed;
    // Legacy skeletal clip preview (writes only to PreviewScene)
    if (m_Clip && m_Skeleton && m_Scene) {
        if (m_Loop && m_Clip->Duration > 0.0f) {
            while (m_Time > m_Clip->Duration) m_Time -= m_Clip->Duration;
            if (m_Time < 0.0f) m_Time += m_Clip->Duration;
        } else {
            if (m_Time > m_Clip->Duration) m_Time = m_Clip->Duration;
            if (m_Time < 0.0f) m_Time = 0.0f;
        }
        std::vector<glm::mat4> localPose;
        cm::animation::EvaluateAnimation(*m_Clip, m_Time, *m_Skeleton, localPose, nullptr);
        FillMissingWithBind(*m_Skeleton, localPose, nullptr);
        ApplyHumanoidConstraints(*m_Skeleton, localPose);
        size_t boneCount = std::min(localPose.size(), m_Skeleton->BoneEntities.size());
        for (size_t i = 0; i < boneCount; ++i) {
            ApplyLocalToTransform(m_Scene, m_Skeleton->BoneEntities[i], localPose[i]);
        }
        return;
    }

    // Unified asset preview path (writes only to PreviewScene)
    if (m_Asset && m_Skeleton && m_Scene) {
        float len = m_Asset->Duration();
        if (m_Loop && len > 0.0f) {
            while (m_Time > len) m_Time -= len;
            if (m_Time < 0.0f) m_Time += len;
        } else {
            if (m_Time > len) m_Time = len;
            if (m_Time < 0.0f) m_Time = 0.0f;
        }
        if (!m_Bindings) m_Bindings = std::make_unique<cm::animation::BindingCache>();
        m_Bindings->SetSkeleton(m_Skeleton);
        cm::animation::PoseBuffer pose; pose.local.resize(m_Skeleton->BoneEntities.size(), glm::mat4(1.0f)); pose.touched.resize(m_Skeleton->BoneEntities.size(), false);
        cm::animation::EvalInputs in{ m_Asset, m_Time, m_Loop };
        cm::animation::EvalTargets tgt{ &pose };
        cm::animation::EvalContext ctx{ m_Bindings.get(), m_Humanoid, m_Skeleton };
        std::vector<cm::animation::ScriptEvent> events; nlohmann::json props;
        cm::animation::SampleAsset(in, ctx, tgt, &events, &props);
        FillMissingWithBind(*m_Skeleton, pose.local, &pose.touched);
        ApplyHumanoidConstraints(*m_Skeleton, pose.local);
        size_t boneCount = std::min(pose.local.size(), m_Skeleton->BoneEntities.size());
        for (size_t i = 0; i < boneCount; ++i) {
            ApplyLocalToTransform(m_Scene, m_Skeleton->BoneEntities[i], pose.local[i]);
        }
    }
}

void AnimationPreviewPlayer::SampleTo(cm::animation::PreviewContext& ctx)
{
    if (!m_Asset || !m_Skeleton) return;
    if (!m_Bindings) m_Bindings = std::make_unique<cm::animation::BindingCache>();
    m_Bindings->SetSkeleton(m_Skeleton);

    cm::animation::EvalInputs in{ m_Asset, m_Time, m_Loop };
    cm::animation::EvalTargets out{ &ctx.pose };
    cm::animation::EvalContext cxt{ m_Bindings.get(), m_Humanoid, m_Skeleton };
    std::vector<cm::animation::ScriptEvent> events;
    nlohmann::json propWrites;
    cm::animation::SampleAsset(in, cxt, out, &events, &propWrites);
}


