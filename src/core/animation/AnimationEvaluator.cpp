#include "core/animation/AnimationEvaluator.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#include "core/animation/AvatarDefinition.h"
#include "core/animation/AnimationAsset.h"
#include "core/animation/BindingCache.h"
#include "core/animation/Retargeting.h"
#include "core/ecs/AnimationComponents.h" // for SkeletonComponent::BoneNameToIndex
#include <nlohmann/json.hpp>

namespace cm {
namespace animation {

template<typename KeyContainer>
static size_t FindKeyframeIndex(const KeyContainer& keys, float time, size_t startIdx) {
    // Fast-forward cached index until the next key's time is > time
    while (startIdx + 1 < keys.size() && keys[startIdx + 1].Time < time) {
        ++startIdx;
    }
    return startIdx;
}

glm::vec3 SampleVec3(const std::vector<KeyframeVec3>& keys, float time, size_t& cacheIdx) {
    if (keys.empty()) return glm::vec3(0.0f);
    if (keys.size() == 1) return keys.front().Value;

    cacheIdx = FindKeyframeIndex(keys, time, cacheIdx);

    const auto& k0 = keys[cacheIdx];
    if (cacheIdx + 1 == keys.size()) return k0.Value;

    const auto& k1 = keys[cacheIdx + 1];
    float t = (time - k0.Time) / (k1.Time - k0.Time);
    return glm::mix(k0.Value, k1.Value, t);
}

glm::quat SampleQuat(const std::vector<KeyframeQuat>& keys, float time, size_t& cacheIdx) {
    if (keys.empty()) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    if (keys.size() == 1) return keys.front().Value;

    cacheIdx = FindKeyframeIndex(keys, time, cacheIdx);

    const auto& k0 = keys[cacheIdx];
    if (cacheIdx + 1 == keys.size()) return k0.Value;

    const auto& k1 = keys[cacheIdx + 1];
    float t = (time - k0.Time) / (k1.Time - k0.Time);
    return glm::slerp(k0.Value, k1.Value, t);
}

void EvaluateAnimation(const AnimationClip& clip,
                       float time,
                       const ::SkeletonComponent& skeleton,
                       std::vector<glm::mat4>& outLocalTransforms,
                       const AvatarDefinition* avatar) {
    // Ensure output container is sized correctly.
    outLocalTransforms.resize(skeleton.BoneEntities.size(), glm::mat4(1.0f));

    // Cache for each bone track – storing last index to speed up sampling.
    struct CacheEntry { size_t pos = 0; size_t rot = 0; size_t scale = 0; };
    std::unordered_map<std::string, CacheEntry> cache;
    cache.reserve(clip.BoneTracks.size());

    // For every animated bone track in the clip
    for (const auto& [boneName, track] : clip.BoneTracks) {
        const std::string* resolvedName = &boneName;
        std::string tmp;
        // Note: name-to-name indirection is not performed here with AvatarDefinition;
        // proper retargeting is handled by HumanoidRetargeter. Keep generic evaluation.

        // Find bone index in skeleton via name mapping
        int idx = skeleton.GetBoneIndex(*resolvedName);
        if (idx < 0 || static_cast<size_t>(idx) >= skeleton.BoneEntities.size()) {
            continue; // Not found in this skeleton
        }
        size_t boneIdx = static_cast<size_t>(idx);

        CacheEntry& ce = cache[*resolvedName];

        glm::vec3 pos = track.PositionKeys.empty() ? glm::vec3(0.0f) : SampleVec3(track.PositionKeys, time, ce.pos);
        glm::quat rot = track.RotationKeys.empty() ? glm::quat(1,0,0,0) : SampleQuat(track.RotationKeys, time, ce.rot);
        glm::vec3 scl = track.ScaleKeys.empty() ? glm::vec3(1.0f) : SampleVec3(track.ScaleKeys, time, ce.scale);

        glm::mat4 mat = glm::translate(pos) * glm::mat4_cast(rot) * glm::scale(scl);
        outLocalTransforms[boneIdx] = mat;
    }
}

} // namespace animation
} // namespace cm

// ================= Unified Evaluator (new) =================
namespace cm {
namespace animation {

namespace {

glm::mat4 ComposeTRS(const glm::vec3& translation,
                     const glm::quat& rotation,
                     const glm::vec3& scale)
{
    glm::mat4 m = glm::mat4_cast(rotation);
    m[0] *= scale.x;
    m[1] *= scale.y;
    m[2] *= scale.z;
    m[3] = glm::vec4(translation, 1.0f);
    return m;
}

void EnsurePoseMatrixSlot(PoseBuffer& pose, size_t boneIndex)
{
    if (boneIndex >= pose.local.size()) {
        pose.local.resize(boneIndex + 1, glm::mat4(1.0f));
    }
    if (boneIndex >= pose.touched.size()) {
        pose.touched.resize(boneIndex + 1, false);
    }
}

void EnsurePoseTrsSlot(PoseTRSBuffer& pose, size_t boneIndex)
{
    if (boneIndex >= pose.translation.size()) {
        pose.translation.resize(boneIndex + 1, glm::vec3(0.0f));
    }
    if (boneIndex >= pose.rotation.size()) {
        pose.rotation.resize(boneIndex + 1, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    }
    if (boneIndex >= pose.scale.size()) {
        pose.scale.resize(boneIndex + 1, glm::vec3(1.0f));
    }
    if (boneIndex >= pose.touched.size()) {
        pose.touched.resize(boneIndex + 1, false);
    }
}

struct AvatarBindTRSCache {
    const AvatarDefinition* avatar = nullptr;
    const glm::mat4* bindLocalData = nullptr;
    size_t bindLocalSize = 0;
    const glm::mat4* bindModelData = nullptr;
    size_t bindModelSize = 0;
    std::array<glm::vec3, HumanoidBoneCount> translations{};
    std::array<glm::quat, HumanoidBoneCount> rotations{};
    std::array<glm::vec3, HumanoidBoneCount> scales{};
    std::array<uint8_t, HumanoidBoneCount> valid{};
    float hipsHeight = 0.0f;
    bool hipsHeightValid = false;
};

AvatarBindTRSCache& GetAvatarBindTRSCache(const AvatarDefinition* avatar)
{
    thread_local std::vector<AvatarBindTRSCache> caches;
    const glm::mat4* bindLocalData = avatar && !avatar->BindLocal.empty()
        ? avatar->BindLocal.data()
        : nullptr;
    const glm::mat4* bindModelData = avatar && !avatar->BindModel.empty()
        ? avatar->BindModel.data()
        : nullptr;
    const size_t bindLocalSize = avatar ? avatar->BindLocal.size() : 0;
    const size_t bindModelSize = avatar ? avatar->BindModel.size() : 0;

    for (auto& cache : caches) {
        if (cache.avatar == avatar &&
            cache.bindLocalData == bindLocalData &&
            cache.bindLocalSize == bindLocalSize &&
            cache.bindModelData == bindModelData &&
            cache.bindModelSize == bindModelSize) {
            return cache;
        }
    }

    caches.push_back({});
    AvatarBindTRSCache& cache = caches.back();
    cache.avatar = avatar;
    cache.bindLocalData = bindLocalData;
    cache.bindLocalSize = bindLocalSize;
    cache.bindModelData = bindModelData;
    cache.bindModelSize = bindModelSize;
    cache.valid.fill(0u);
    cache.hipsHeight = 0.0f;
    cache.hipsHeightValid = false;
    return cache;
}

bool GetCachedAnimationBindTRS(const AvatarDefinition* avatar,
                               HumanoidBone bone,
                               glm::vec3& outT,
                               glm::quat& outR,
                               glm::vec3& outS)
{
    const uint16_t idx = static_cast<uint16_t>(bone);
    if (!avatar || idx >= avatar->BindLocal.size()) {
        outT = glm::vec3(0.0f);
        outR = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        outS = glm::vec3(1.0f);
        return false;
    }

    AvatarBindTRSCache& cache = GetAvatarBindTRSCache(avatar);
    if (cache.valid[idx] != 0u) {
        outT = cache.translations[idx];
        outR = cache.rotations[idx];
        outS = cache.scales[idx];
        return true;
    }

    glm::vec3 skew(0.0f);
    glm::vec4 persp(0.0f);
    glm::decompose(avatar->BindLocal[idx], outS, outR, outT, skew, persp);
    outR = glm::normalize(outR);
    if (bone == HumanoidBone::Hips && idx < avatar->BindModel.size() && std::abs(outT.y) < 0.01f) {
        outT = glm::vec3(avatar->BindModel[idx][3]);
    }

    cache.translations[idx] = outT;
    cache.rotations[idx] = outR;
    cache.scales[idx] = outS;
    cache.valid[idx] = 1u;
    return true;
}

float GetCachedAvatarHipsHeight(const AvatarDefinition* avatar)
{
    if (!avatar) return 0.0f;

    AvatarBindTRSCache& cache = GetAvatarBindTRSCache(avatar);
    if (cache.hipsHeightValid) {
        return cache.hipsHeight;
    }

    cache.hipsHeight = 0.0f;
    const uint16_t hipsIdx = static_cast<uint16_t>(HumanoidBone::Hips);
    if (hipsIdx < avatar->BindModel.size()) {
        cache.hipsHeight = std::abs(avatar->BindModel[hipsIdx][3].y);
    }
    cache.hipsHeightValid = true;
    return cache.hipsHeight;
}

int ResolveBoneTrackIndex(const ::SkeletonComponent& skeleton, const std::string& name)
{
    int idx = skeleton.GetBoneIndex(name);
    if (idx >= 0) return idx;

    size_t pos = name.find_last_of(':');
    if (pos != std::string::npos && pos + 1 < name.size()) {
        idx = skeleton.GetBoneIndex(name.substr(pos + 1));
        if (idx >= 0) return idx;
    }
    pos = name.find_last_of('|');
    if (pos != std::string::npos && pos + 1 < name.size()) {
        idx = skeleton.GetBoneIndex(name.substr(pos + 1));
        if (idx >= 0) return idx;
    }
    pos = name.find_last_of('.');
    if (pos != std::string::npos && pos + 1 < name.size()) {
        idx = skeleton.GetBoneIndex(name.substr(pos + 1));
        if (idx >= 0) return idx;
    }

    for (const auto& kv : skeleton.BoneNameToIndex) {
        const std::string& skName = kv.first;
        if (skName.size() >= name.size()) {
            if (skName.compare(skName.size() - name.size(), name.size(), name) == 0) return kv.second;
        } else {
            if (name.compare(name.size() - skName.size(), skName.size(), skName) == 0) return kv.second;
        }
    }
    return -1;
}

} // namespace

void SampleAsset(const EvalInputs& in, const EvalContext& ctx, EvalTargets& out,
                 std::vector<ScriptEvent>* firedEvents, nlohmann::json* propertyWrites)
{
    if (!in.asset) return;
    const float clipLen = in.asset->Duration();
    auto wrapTime = [](float time, float length) -> float {
        if (length <= 0.0f) return time;
        return std::fmod(std::fmod(time, length) + length, length);
    };
    float t = in.time;
    if (in.loop && clipLen > 0.0f) t = wrapTime(t, clipLen);
    // The sample time is normalized once here; inner curves should not fmod again.
    constexpr bool kCurveShouldWrapTime = false;

    if (out.pose) {
        if (out.pose->touched.size() < out.pose->local.size()) out.pose->touched.resize(out.pose->local.size(), false);
        std::fill(out.pose->touched.begin(), out.pose->touched.end(), false);
    }
    if (out.poseTrs) {
        if (out.poseTrs->touched.size() < out.poseTrs->translation.size()) {
            out.poseTrs->touched.resize(out.poseTrs->translation.size(), false);
        }
        std::fill(out.poseTrs->touched.begin(), out.poseTrs->touched.end(), false);
    }

    auto writePoseSample = [&](size_t boneIndex,
                               const glm::vec3& pos,
                               const glm::quat& rot,
                               const glm::vec3& scl) {
        if (out.pose) {
            EnsurePoseMatrixSlot(*out.pose, boneIndex);
            out.pose->local[boneIndex] = ComposeTRS(pos, rot, scl);
            out.pose->touched[boneIndex] = true;
        }
        if (out.poseTrs) {
            EnsurePoseTrsSlot(*out.poseTrs, boneIndex);
            out.poseTrs->translation[boneIndex] = pos;
            out.poseTrs->rotation[boneIndex] = rot;
            out.poseTrs->scale[boneIndex] = scl;
            out.poseTrs->touched[boneIndex] = true;
        }
    };

    const AnimationRuntimeView& runtimeView = in.asset->GetRuntimeView();

    if ((out.pose || out.poseTrs) && ctx.skeleton && ctx.skeleton->Avatar && !runtimeView.avatarTracks.empty()) {
        const auto* avatar = ctx.skeleton->Avatar.get();
        float positionScale = 1.0f;
        if (in.asset->meta.referenceHipsHeight > 0.1f && avatar->IsBonePresent(HumanoidBone::Hips)) {
            const float targetHeight = GetCachedAvatarHipsHeight(avatar);
            if (targetHeight > 0.01f) {
                positionScale = targetHeight / in.asset->meta.referenceHipsHeight;
            }
        }

        const bool legacyMode = in.asset->meta.humanoidTrackMode == HumanoidTrackMode::LegacyAbsolute;
        const auto* sourceAvatar = in.asset->LegacySourceAvatar.get();

        for (const RuntimeAvatarTrackView& track : runtimeView.avatarTracks) {
            if (track.muted && *track.muted) continue;

            const int humanId = track.humanBoneId;
            const auto humanBone = static_cast<HumanoidBone>(humanId);
            if (!avatar->IsBonePresent(humanBone)) continue;

            const int32_t boneIndex = avatar->Map[static_cast<size_t>(humanId)].BoneIndex;
            if (boneIndex < 0) continue;
            const size_t bi = static_cast<size_t>(boneIndex);

            if (legacyMode) {
                const glm::vec3 pos = track.t ? track.t->Sample(t, kCurveShouldWrapTime, clipLen) : glm::vec3(0.0f);
                const glm::quat rot = track.r ? track.r->Sample(t, kCurveShouldWrapTime, clipLen) : glm::quat(1,0,0,0);
                const glm::vec3 scl = track.s ? track.s->Sample(t, kCurveShouldWrapTime, clipLen) : glm::vec3(1.0f);

                glm::vec3 dstBindT(0.0f), dstBindS(1.0f);
                glm::quat dstBindR(1.0f, 0.0f, 0.0f, 0.0f);
                GetCachedAnimationBindTRS(avatar, humanBone, dstBindT, dstBindR, dstBindS);

                if (sourceAvatar && sourceAvatar->IsBonePresent(humanBone)) {
                    glm::vec3 srcBindT(0.0f), srcBindS(1.0f);
                    glm::quat srcBindR(1.0f, 0.0f, 0.0f, 0.0f);
                    GetCachedAnimationBindTRS(sourceAvatar, humanBone, srcBindT, srcBindR, srcBindS);

                    const glm::vec3 finalPos = track.drivesTranslationAndScale
                        ? dstBindT + (pos - srcBindT) * positionScale
                        : dstBindT;
                    const glm::quat deltaRot = glm::normalize(rot * glm::conjugate(srcBindR));
                    const glm::quat finalRot = glm::normalize(deltaRot * dstBindR);
                    const glm::vec3 retargetedScale(
                        (std::abs(srcBindS.x) > 1e-6f) ? (scl.x / srcBindS.x) * dstBindS.x : scl.x,
                        (std::abs(srcBindS.y) > 1e-6f) ? (scl.y / srcBindS.y) * dstBindS.y : scl.y,
                        (std::abs(srcBindS.z) > 1e-6f) ? (scl.z / srcBindS.z) * dstBindS.z : scl.z
                    );
                    const glm::vec3 finalScale = track.drivesTranslationAndScale
                        ? retargetedScale
                        : dstBindS;
                    writePoseSample(bi, finalPos, finalRot, finalScale);
                } else {
                    const glm::vec3 finalPos = track.drivesTranslationAndScale ? pos : dstBindT;
                    const glm::vec3 finalScale = track.drivesTranslationAndScale ? scl : dstBindS;
                    writePoseSample(bi, finalPos, rot, finalScale);
                }
                continue;
            }

            const glm::vec3 dpos = track.t ? track.t->Sample(t, kCurveShouldWrapTime, clipLen) : glm::vec3(0.0f);
            const glm::quat drot = track.r ? track.r->Sample(t, kCurveShouldWrapTime, clipLen) : glm::quat(1,0,0,0);
            const glm::vec3 dscale = track.s ? track.s->Sample(t, kCurveShouldWrapTime, clipLen) : glm::vec3(1.0f);

            glm::vec3 bindT(0.0f), bindS(1.0f);
            glm::quat bindR(1.0f, 0.0f, 0.0f, 0.0f);
            GetCachedAnimationBindTRS(avatar, humanBone, bindT, bindR, bindS);

            const glm::vec3 finalPos = track.drivesTranslationAndScale
                ? bindT + dpos * positionScale
                : bindT;
            const glm::quat finalRot = glm::normalize(drot * bindR);
            const glm::vec3 finalScale = track.drivesTranslationAndScale
                ? dscale * bindS
                : bindS;

            writePoseSample(bi, finalPos, finalRot, finalScale);
        }
    }

    if ((out.pose || out.poseTrs) && !runtimeView.boneTracks.empty()) {
        for (const RuntimeBoneTrackView& track : runtimeView.boneTracks) {
            if (track.muted && *track.muted) continue;

            const glm::vec3 pos = track.t ? track.t->Sample(t, kCurveShouldWrapTime, clipLen) : glm::vec3(0.0f);
            const glm::quat rot = track.r ? track.r->Sample(t, kCurveShouldWrapTime, clipLen) : glm::quat(1,0,0,0);
            const glm::vec3 scl = track.s ? track.s->Sample(t, kCurveShouldWrapTime, clipLen) : glm::vec3(1.0f);

            int boneIndex = track.boneId;
            if (boneIndex < 0 && ctx.skeleton && track.name && !track.name->empty()) {
                boneIndex = ctx.bindings
                    ? ctx.bindings->ResolveBoneByTrackName(track.name)
                    : ResolveBoneTrackIndex(*ctx.skeleton, *track.name);
            }

            if (boneIndex >= 0) {
                const size_t bi = static_cast<size_t>(boneIndex);
                writePoseSample(bi, pos, rot, scl);
            }
        }
    }

    if (propertyWrites && ctx.bindings && runtimeView.HasPropertyTracks) {
        for (const AssetPropertyTrack* pt : runtimeView.propertyTracks) {
            if (!pt || pt->muted) continue;

            std::uint64_t id = pt->binding.resolvedId ? pt->binding.resolvedId : ctx.bindings->ResolveProperty(pt->binding.path);
            const std::string key = std::to_string(id);
            switch (pt->binding.type) {
                case PropertyType::Float: {
                    float v = std::get<CurveFloat>(pt->curve).Sample(t, kCurveShouldWrapTime, clipLen);
                    (*propertyWrites)[key] = v;
                } break;
                case PropertyType::Vec2:  {
                    glm::vec2 v = std::get<CurveVec2>(pt->curve).Sample(t, kCurveShouldWrapTime, clipLen);
                    (*propertyWrites)[key] = { v.x, v.y };
                } break;
                case PropertyType::Vec3:  {
                    glm::vec3 v = std::get<CurveVec3>(pt->curve).Sample(t, kCurveShouldWrapTime, clipLen);
                    (*propertyWrites)[key] = { v.x, v.y, v.z };
                } break;
                case PropertyType::Quat:  {
                    glm::quat v = std::get<CurveQuat>(pt->curve).Sample(t, kCurveShouldWrapTime, clipLen);
                    (*propertyWrites)[key] = { v.x, v.y, v.z, v.w };
                } break;
                case PropertyType::Color: {
                    glm::vec4 v = std::get<CurveColor>(pt->curve).Sample(t, kCurveShouldWrapTime, clipLen);
                    (*propertyWrites)[key] = { v.x, v.y, v.z, v.w };
                } break;
            }
        }
    }

    if (firedEvents && runtimeView.HasScriptEventTracks) {
        const float eps = (in.asset->meta.fps > 0.0f)
            ? (1.0f / std::max(1.0f, in.asset->meta.fps))
            : 0.0f;
        float prevWrapped = in.prevTime;
        if (in.hasPrevTime && in.loop && clipLen > 0.0f) {
            prevWrapped = wrapTime(in.prevTime, clipLen);
        }

        for (const AssetScriptEventTrack* st : runtimeView.scriptEventTracks) {
            if (!st || st->muted) continue;

            for (const auto& e : st->events) {
                if (!in.hasPrevTime || clipLen <= 0.0f) {
                    if (std::abs(e.time - t) <= eps) firedEvents->push_back(e);
                    continue;
                }
                if (!in.loop) {
                    if (e.time + eps >= prevWrapped && e.time - eps <= t) firedEvents->push_back(e);
                    continue;
                }
                if (in.loopCount > 1) {
                    firedEvents->push_back(e);
                    continue;
                }
                if (in.loopCount > 0) {
                    if (e.time + eps >= prevWrapped || e.time - eps <= t) firedEvents->push_back(e);
                } else {
                    if (e.time + eps >= prevWrapped && e.time - eps <= t) firedEvents->push_back(e);
                }
            }
        }
    }
}

} // namespace animation
} // namespace cm

namespace cm { namespace animation {
void SampleAsset(const EvalInputs& in,
                 const EvalContext& ctx,
                 PoseBuffer& outPose,
                 std::vector<ScriptEvent>* outEvents,
                 std::vector<PropertyWrite>* outProps)
{
    EvalTargets tgt{ &outPose };
    nlohmann::json propWrites;
    SampleAsset(in, ctx, tgt, outEvents, outProps ? &propWrites : nullptr);
    if (outProps) {
        outProps->clear();
        for (auto it = propWrites.begin(); it != propWrites.end(); ++it) {
            PropertyWrite w; w.id = std::strtoull(it.key().c_str(), nullptr, 10); w.value = it.value();
            outProps->push_back(std::move(w));
        }
    }
}
} }

