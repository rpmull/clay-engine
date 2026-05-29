#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <glm/mat4x4.hpp>
#include <glm/gtc/quaternion.hpp>
#include "core/animation/HumanoidBone.h"

// Forward declare SkeletonComponent to avoid heavy include
struct SkeletonComponent;

namespace cm {
namespace animation {

struct HumanoidMapEntry {
    HumanoidBone Bone;
    int32_t BoneIndex = -1;     // Index into Skeleton bones; -1 if unmapped
    std::string BoneName;       // Original rig name
};

struct AvatarAxes {
    enum class Axis { X, Y, Z };
    Axis Up = Axis::Y;
    Axis Forward = Axis::Z; // Z+ forward default
    bool RightHanded = true;
};

struct AvatarDefinition {
    std::string RigName;
    std::vector<HumanoidMapEntry> Map; // size = HumanoidBone::Count
    AvatarAxes Axes;
    float UnitsPerMeter = 1.0f;

    // Rest/bind:
    std::vector<glm::mat4> BindModel; // per mapped bone, model-space (size = Count)
    std::vector<glm::mat4> BindLocal; // per mapped bone, local-space (size = Count)

    // Optional precomputed:
    std::vector<glm::mat4> RetargetModel; // R[b] = T_bind * inverse(S_bind)
    std::vector<bool>      Present;       // mapped or not (size = Count)
    std::vector<glm::quat> RestOffsetRot; // A-pose to T-pose correction

    AvatarDefinition();

    bool IsBonePresent(HumanoidBone b) const;
    int32_t GetMappedBoneIndex(HumanoidBone b) const;
    const std::string& GetMappedBoneName(HumanoidBone b) const;
    bool GetAnimationBindTRS(HumanoidBone b,
                             glm::vec3& outT,
                             glm::quat& outR,
                             glm::vec3& outS) const;
};

// Utilities to build an avatar from a scene skeleton using heuristics
namespace avatar_builders {

// Populate Map and bind transforms from a SkeletonComponent.
// If autoMap is true, uses name heuristics to guess mapping.
void BuildFromSkeleton(const SkeletonComponent& skeleton,
                       AvatarDefinition& outAvatar,
                       bool autoMap = true,
                       const std::unordered_map<HumanoidBone, std::vector<std::string>>& nameMap = {});

// Recompute bind-local/model transforms for an already-mapped avatar against a skeleton.
// Useful for older .avatar files that only stored bone mappings.
void PopulateBindDataFromSkeleton(const SkeletonComponent& skeleton,
                                  AvatarDefinition& avatar);

// Standard initial name seeds
const std::unordered_map<HumanoidBone, std::vector<std::string>>& DefaultNameSeeds();

}

} // namespace animation
} // namespace cm


