#include "core/animation/AnimationAsset.h"

#include <algorithm>

#include "core/animation/AvatarDefinition.h"

namespace cm {
namespace animation {

const ITrack* AnimationAsset::FindTrack(TrackID id) const {
    for (const auto& t : tracks) if (t && t->id == id) return t.get();
    return nullptr;
}

ITrack* AnimationAsset::FindTrack(TrackID id) {
    for (auto& t : tracks) if (t && t->id == id) return t.get();
    return nullptr;
}

const AnimationRuntimeView& AnimationAsset::GetRuntimeView() const
{
    const auto* trackData = tracks.empty() ? nullptr : tracks.data();
    if (!runtimeViewCache ||
        runtimeViewCache->sourceTrackCount != tracks.size() ||
        runtimeViewCache->sourceTrackData != trackData) {
        RebuildRuntimeView();
    }
    return *runtimeViewCache;
}

void AnimationAsset::RebuildRuntimeView() const
{
    auto view = std::make_unique<AnimationRuntimeView>();
    view->sourceTrackCount = tracks.size();
    view->sourceTrackData = tracks.empty() ? nullptr : tracks.data();
    view->boneTracks.reserve(tracks.size());
    view->avatarTracks.reserve(tracks.size());
    view->AllowsCrowdPoseShare = meta.rootMotion.Mode != RootMotionMode::ApplyToEntity;

    for (const auto& trackPtr : tracks) {
        if (!trackPtr) continue;

        switch (trackPtr->type) {
            case TrackType::Bone: {
                const auto* track = static_cast<const AssetBoneTrack*>(trackPtr.get());
                RuntimeBoneTrackView runtimeTrack;
                runtimeTrack.boneId = track->boneId;
                runtimeTrack.name = &track->name;
                runtimeTrack.muted = &track->muted;
                runtimeTrack.t = track->t.keys.empty() ? nullptr : &track->t;
                runtimeTrack.r = track->r.keys.empty() ? nullptr : &track->r;
                runtimeTrack.s = track->s.keys.empty() ? nullptr : &track->s;
                view->boneTracks.push_back(runtimeTrack);
                if (!track->muted) {
                    view->HasUnmutedBoneTracks = true;
                }
            } break;
            case TrackType::Avatar: {
                const auto* track = static_cast<const AssetAvatarTrack*>(trackPtr.get());
                if (track->humanBoneId < 0 ||
                    track->humanBoneId >= static_cast<int>(HumanoidBoneCount)) {
                    break;
                }

                const auto humanBone = static_cast<HumanoidBone>(track->humanBoneId);
                RuntimeAvatarTrackView runtimeTrack;
                runtimeTrack.humanBoneId = track->humanBoneId;
                runtimeTrack.drivesTranslationAndScale =
                    humanBone == HumanoidBone::Root ||
                    humanBone == HumanoidBone::Hips;
                runtimeTrack.muted = &track->muted;
                runtimeTrack.t = track->t.keys.empty() ? nullptr : &track->t;
                runtimeTrack.r = track->r.keys.empty() ? nullptr : &track->r;
                runtimeTrack.s = track->s.keys.empty() ? nullptr : &track->s;
                view->avatarTracks.push_back(runtimeTrack);
            } break;
            case TrackType::Property: {
                const auto* track = static_cast<const AssetPropertyTrack*>(trackPtr.get());
                view->propertyTracks.push_back(track);
                if (!track->muted) {
                    view->HasPropertyTracks = true;
                    view->AllowsCrowdPoseShare = false;
                }
            } break;
            case TrackType::ScriptEvent: {
                const auto* track = static_cast<const AssetScriptEventTrack*>(trackPtr.get());
                view->scriptEventTracks.push_back(track);
                if (!track->muted && !track->events.empty()) {
                    view->HasScriptEventTracks = true;
                    view->AllowsCrowdPoseShare = false;
                }
            } break;
        }
    }

    view->boneTracks.shrink_to_fit();
    view->avatarTracks.shrink_to_fit();
    view->propertyTracks.shrink_to_fit();
    view->scriptEventTracks.shrink_to_fit();
    runtimeViewCache = std::move(view);
}

void AnimationAsset::InvalidateRuntimeView() const
{
    runtimeViewCache.reset();
    durationCacheValid = false;
}

static float maxTimeInCurves(const AssetPropertyTrack& pt) {
    float m = 0.0f;
    auto scan = [&](auto const& curve){
        for (const auto& k : curve.keys) m = std::max(m, k.t);
    };
    switch (pt.binding.type) {
        case PropertyType::Float: scan(std::get<CurveFloat>(pt.curve)); break;
        case PropertyType::Vec2:  scan(std::get<CurveVec2>(pt.curve)); break;
        case PropertyType::Vec3:  scan(std::get<CurveVec3>(pt.curve)); break;
        case PropertyType::Quat:  scan(std::get<CurveQuat>(pt.curve)); break;
        case PropertyType::Color: scan(std::get<CurveColor>(pt.curve)); break;
    }
    return m;
}

float AnimationAsset::Duration() const {
    if (meta.length > 0.0f) return meta.length;
    if (durationCacheValid) return durationCache;

    float maxT = 0.0f;
    for (const auto& t : tracks) {
        if (!t || t->muted) continue;
        switch (t->type) {
            case TrackType::Bone: {
                const auto* bt = static_cast<const AssetBoneTrack*>(t.get());
                for (const auto& k : bt->t.keys) maxT = std::max(maxT, k.t);
                for (const auto& k : bt->r.keys) maxT = std::max(maxT, k.t);
                for (const auto& k : bt->s.keys) maxT = std::max(maxT, k.t);
            } break;
            case TrackType::Avatar: {
                const auto* at = static_cast<const AssetAvatarTrack*>(t.get());
                for (const auto& k : at->t.keys) maxT = std::max(maxT, k.t);
                for (const auto& k : at->r.keys) maxT = std::max(maxT, k.t);
                for (const auto& k : at->s.keys) maxT = std::max(maxT, k.t);
            } break;
            case TrackType::Property: {
                maxT = std::max(maxT, maxTimeInCurves(*static_cast<const AssetPropertyTrack*>(t.get())));
            } break;
            case TrackType::ScriptEvent: {
                const auto* st = static_cast<const AssetScriptEventTrack*>(t.get());
                for (const auto& e : st->events) maxT = std::max(maxT, e.time);
            } break;
        }
    }
    durationCache = maxT;
    durationCacheValid = true;
    return durationCache;
}

} // namespace animation
} // namespace cm


