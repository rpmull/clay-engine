#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>
#include <nlohmann/json.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "core/animation/Curves.h"

namespace cm {
namespace animation {

struct AvatarDefinition;

enum class TrackType { Bone, Avatar, Property, ScriptEvent };

using TrackID = std::uint64_t;
using KeyID   = std::uint64_t;

// How avatar tracks are encoded inside the asset.
// Legacy assets stored raw local humanoid transforms, while newer imports store
// deltas relative to the source rig's authored rest pose.
enum class HumanoidTrackMode : uint8_t {
    LegacyAbsolute = 0,
    BindRelative = 1,
};

/// Root motion mode - determines how root/hips motion is handled at runtime
enum class RootMotionMode : uint8_t {
    None = 0,           ///< Zero all translation on root/hips (completely static)
    InPlace,            ///< Zero XZ translation but keep Y (vertical bounce preserved)
    ApplyToEntity       ///< Extract motion and apply to entity/physics (physics-correct)
};

/// Root motion settings baked into the animation asset
/// These settings are per-animation and control how root motion is extracted and applied
struct RootMotionSettings {
    RootMotionMode Mode = RootMotionMode::InPlace;
    
    /// Include horizontal (XZ) motion when Mode == ApplyToEntity
    bool IncludeXZ = true;
    
    /// Include vertical (Y) motion when Mode == ApplyToEntity.
    /// Runtime always preserves authored vertical motion for root motion clips.
    bool IncludeY = true;
    
    /// Include rotation from hips/root bone when Mode == ApplyToEntity
    bool IncludeRotation = false;
    
    /// Override gravity while this animation's root motion is active
    /// Useful for climbing, jumping, or other animations where Y motion should be absolute
    bool OverrideGravity = false;
    
    /// Name of the bone to extract root motion from (empty = auto-detect hips)
    /// Auto-detection tries: Hips, Root, mixamorig:Hips, pelvis, etc.
    std::string RootBoneName;
    
    /// Precomputed: total distance traveled in XZ plane (for UI/debugging)
    float TotalDistanceXZ = 0.0f;
    
    /// Precomputed: total vertical distance traveled (for UI/debugging)
    float TotalDistanceY = 0.0f;
    
    bool operator==(const RootMotionSettings& o) const {
        return Mode == o.Mode && IncludeXZ == o.IncludeXZ && IncludeY == o.IncludeY 
            && IncludeRotation == o.IncludeRotation && OverrideGravity == o.OverrideGravity
            && RootBoneName == o.RootBoneName;
    }
    bool operator!=(const RootMotionSettings& o) const { return !(*this == o); }
};

struct AnimationAssetMeta { 
    int version = 1; 
    float length = 0.0f; 
    float fps = 30.0f;
    HumanoidTrackMode humanoidTrackMode = HumanoidTrackMode::LegacyAbsolute;
    // Reference height of the source avatar's hips (in model-space Y) used for position normalization.
    // Avatar track positions are stored as deltas relative to bind pose, and this value allows
    // scaling those deltas when retargeting to skeletons of different sizes.
    float referenceHipsHeight = 0.0f;  // 0 = legacy/unknown, use fallback heuristics
    
    /// Root motion settings for this animation clip
    RootMotionSettings rootMotion;
};

struct ITrack {
    TrackID id = 0;
    std::string name;
    TrackType type = TrackType::Bone;
    bool muted = false;
    virtual ~ITrack() = default;
};

struct AssetBoneTrack : ITrack {
    int boneId = -1; // resolved skeleton bone index
    CurveVec3 t; CurveQuat r; CurveVec3 s;
    AssetBoneTrack() { type = TrackType::Bone; }
};

struct AssetAvatarTrack : ITrack {
    int humanBoneId = -1; // canonical humanoid enum value
    CurveVec3 t; CurveQuat r; CurveVec3 s;
    AssetAvatarTrack() { type = TrackType::Avatar; }
};

enum class PropertyType { Float, Vec2, Vec3, Quat, Color };

struct PropertyBinding { std::string path; std::uint64_t resolvedId = 0; PropertyType type = PropertyType::Float; };

struct AssetPropertyTrack : ITrack {
    PropertyBinding binding;
    std::variant<CurveFloat, CurveVec2, CurveVec3, CurveQuat, CurveColor> curve;
    AssetPropertyTrack() { type = TrackType::Property; }
};

struct AssetScriptEvent { KeyID id = 0; float time = 0.0f; std::string className; std::string method; nlohmann::json payload; };

struct AssetScriptEventTrack : ITrack { std::vector<AssetScriptEvent> events; AssetScriptEventTrack() { type = TrackType::ScriptEvent; } };

// Backward-compatible alias used by some parts of the codebase
using ScriptEvent = AssetScriptEvent;

struct RuntimeBoneTrackView {
    int boneId = -1;
    const std::string* name = nullptr;
    const bool* muted = nullptr;
    const CurveVec3* t = nullptr;
    const CurveQuat* r = nullptr;
    const CurveVec3* s = nullptr;
};

struct RuntimeAvatarTrackView {
    int humanBoneId = -1;
    bool drivesTranslationAndScale = false;
    const bool* muted = nullptr;
    const CurveVec3* t = nullptr;
    const CurveQuat* r = nullptr;
    const CurveVec3* s = nullptr;
};

struct AnimationRuntimeView {
    std::vector<RuntimeBoneTrackView> boneTracks;
    std::vector<RuntimeAvatarTrackView> avatarTracks;
    std::vector<const AssetPropertyTrack*> propertyTracks;
    std::vector<const AssetScriptEventTrack*> scriptEventTracks;
    bool AllowsCrowdPoseShare = true;
    bool HasUnmutedBoneTracks = false;
    bool HasPropertyTracks = false;
    bool HasScriptEventTracks = false;
    size_t sourceTrackCount = 0;
    const std::unique_ptr<ITrack>* sourceTrackData = nullptr;
};

struct AnimationAsset {
    std::string name;
    AnimationAssetMeta meta;
    std::vector<std::unique_ptr<ITrack>> tracks;

    // Transient runtime-only source avatar used to retarget legacy humanoid tracks
    // that were imported before bind-relative encoding existed.
    std::shared_ptr<AvatarDefinition> LegacySourceAvatar;

    const ITrack* FindTrack(TrackID id) const;
    ITrack* FindTrack(TrackID id);
    const AnimationRuntimeView& GetRuntimeView() const;
    void RebuildRuntimeView() const;
    void InvalidateRuntimeView() const;
    float Duration() const; // derived from max key time across all tracks if meta.length == 0

private:
    mutable std::unique_ptr<AnimationRuntimeView> runtimeViewCache;
    mutable float durationCache = 0.0f;
    mutable bool durationCacheValid = false;
};

} // namespace animation
} // namespace cm


