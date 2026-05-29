#include "core/animation/AvatarDefinition.h"
#include "core/animation/HumanoidAvatar.h"
#include "core/ecs/AnimationComponents.h"
#include <algorithm>
#include <cctype>
#include <cmath>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

namespace cm {
namespace animation {

AvatarDefinition::AvatarDefinition()
{
    Map.resize(HumanoidBoneCount);
    BindModel.assign(HumanoidBoneCount, glm::mat4(1.0f));
    BindLocal.assign(HumanoidBoneCount, glm::mat4(1.0f));
    RetargetModel.assign(HumanoidBoneCount, glm::mat4(1.0f));
    Present.assign(HumanoidBoneCount, false);
    RestOffsetRot.assign(HumanoidBoneCount, glm::quat(1,0,0,0));
    // Initialize HumanoidMapEntry bones
    for (uint16_t i = 0; i < HumanoidBoneCount; ++i) {
        Map[i].Bone = static_cast<HumanoidBone>(i);
    }
}

bool AvatarDefinition::IsBonePresent(HumanoidBone b) const
{
    return Present[static_cast<uint16_t>(b)];
}

int32_t AvatarDefinition::GetMappedBoneIndex(HumanoidBone b) const
{
    return Map[static_cast<uint16_t>(b)].BoneIndex;
}

const std::string& AvatarDefinition::GetMappedBoneName(HumanoidBone b) const
{
    return Map[static_cast<uint16_t>(b)].BoneName;
}

bool AvatarDefinition::GetAnimationBindTRS(HumanoidBone b,
                                           glm::vec3& outT,
                                           glm::quat& outR,
                                           glm::vec3& outS) const
{
    const uint16_t idx = static_cast<uint16_t>(b);
    if (idx >= BindLocal.size()) {
        outT = glm::vec3(0.0f);
        outR = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        outS = glm::vec3(1.0f);
        return false;
    }

    glm::vec3 skew(0.0f);
    glm::vec4 persp(0.0f);
    glm::decompose(BindLocal[idx], outS, outR, outT, skew, persp);
    outR = glm::normalize(outR);

    // Older avatar assets could lose bind-local hips translation. Keep the runtime fallback
    // centralized so import-time delta baking and playback reconstruct against the same frame.
    if (b == HumanoidBone::Hips && idx < BindModel.size() && std::abs(outT.y) < 0.01f) {
        outT = glm::vec3(BindModel[idx][3]);
    }

    return true;
}

static std::string to_canonical(const std::string& in)
{
    std::string s;
    s.reserve(in.size());
    for (char c : in) {
        if (std::isalnum(static_cast<unsigned char>(c))) s.push_back((char)std::tolower(c));
    }
    return s;
}

// Determines if a humanoid bone is left-sided
static bool is_left_bone(HumanoidBone hb)
{
    switch (hb) {
        case HumanoidBone::LeftShoulder:
        case HumanoidBone::LeftUpperArm:
        case HumanoidBone::LeftLowerArm:
        case HumanoidBone::LeftHand:
        case HumanoidBone::LeftUpperLeg:
        case HumanoidBone::LeftLowerLeg:
        case HumanoidBone::LeftFoot:
        case HumanoidBone::LeftToes:
        case HumanoidBone::LeftEye:
        case HumanoidBone::LeftThumbProx:
        case HumanoidBone::LeftThumbInter:
        case HumanoidBone::LeftThumbDist:
        case HumanoidBone::LeftIndexProx:
        case HumanoidBone::LeftIndexInter:
        case HumanoidBone::LeftIndexDist:
        case HumanoidBone::LeftMiddleProx:
        case HumanoidBone::LeftMiddleInter:
        case HumanoidBone::LeftMiddleDist:
        case HumanoidBone::LeftRingProx:
        case HumanoidBone::LeftRingInter:
        case HumanoidBone::LeftRingDist:
        case HumanoidBone::LeftLittleProx:
        case HumanoidBone::LeftLittleInter:
        case HumanoidBone::LeftLittleDist:
        case HumanoidBone::LeftUpperArmTwist:
        case HumanoidBone::LeftLowerArmTwist:
        case HumanoidBone::LeftUpperLegTwist:
        case HumanoidBone::LeftLowerLegTwist:
            return true;
        default:
            return false;
    }
}

// Determines if a humanoid bone is right-sided
static bool is_right_bone(HumanoidBone hb)
{
    switch (hb) {
        case HumanoidBone::RightShoulder:
        case HumanoidBone::RightUpperArm:
        case HumanoidBone::RightLowerArm:
        case HumanoidBone::RightHand:
        case HumanoidBone::RightUpperLeg:
        case HumanoidBone::RightLowerLeg:
        case HumanoidBone::RightFoot:
        case HumanoidBone::RightToes:
        case HumanoidBone::RightEye:
        case HumanoidBone::RightThumbProx:
        case HumanoidBone::RightThumbInter:
        case HumanoidBone::RightThumbDist:
        case HumanoidBone::RightIndexProx:
        case HumanoidBone::RightIndexInter:
        case HumanoidBone::RightIndexDist:
        case HumanoidBone::RightMiddleProx:
        case HumanoidBone::RightMiddleInter:
        case HumanoidBone::RightMiddleDist:
        case HumanoidBone::RightRingProx:
        case HumanoidBone::RightRingInter:
        case HumanoidBone::RightRingDist:
        case HumanoidBone::RightLittleProx:
        case HumanoidBone::RightLittleInter:
        case HumanoidBone::RightLittleDist:
        case HumanoidBone::RightUpperArmTwist:
        case HumanoidBone::RightLowerArmTwist:
        case HumanoidBone::RightUpperLegTwist:
        case HumanoidBone::RightLowerLegTwist:
            return true;
        default:
            return false;
    }
}

// Check if the character at position is a word boundary (not alphanumeric)
static bool is_boundary_before(const std::string& s, size_t pos)
{
    if (pos == 0) return true;
    // Check if character before is not a letter (common prefixes end in non-alpha or just before our match)
    char prev = s[pos - 1];
    // Common rig prefixes end in ':', '_', or non-alpha chars. Allow match after such chars.
    // Also allow if previous char is part of common prefix patterns like "mixamorig" ending
    return !std::isalpha(static_cast<unsigned char>(prev));
}

static bool name_matches(const std::string& name, const std::vector<std::string>& candidates, HumanoidBone targetBone)
{
    const std::string canon = to_canonical(name);
    
    // Sidedness validation: reject skeleton bones that contain the OPPOSITE side marker
    // This prevents e.g. RightHand from matching "LeftHandRing1" due to substring collision
    bool expectLeft = is_left_bone(targetBone);
    bool expectRight = is_right_bone(targetBone);
    
    if (expectLeft && canon.find("right") != std::string::npos) {
        return false; // Left bone cannot match a skeleton bone containing "right"
    }
    if (expectRight && canon.find("left") != std::string::npos) {
        return false; // Right bone cannot match a skeleton bone containing "left"
    }
    
    for (const auto& c : candidates) {
        const std::string tok = to_canonical(c);
        size_t pos = canon.find(tok);
        if (pos == std::string::npos) continue;

        // If the token is purely alphabetic (e.g. "Spine"), do not match when a digit follows immediately
        // in the source name (prevents matching "Spine" to "Spine2").
        bool alpha_only = true;
        for (char ch : tok) {
            if (!std::isalpha(static_cast<unsigned char>(ch))) { alpha_only = false; break; }
        }
        if (alpha_only) {
            size_t after = pos + tok.size();
            if (after < canon.size() && std::isdigit(static_cast<unsigned char>(canon[after]))) {
                continue; // reject this match; try next candidate
            }
        }
        
        // Additional check: for short tokens that could be substrings of other bone names,
        // require the match to be at a reasonable position (after prefix like "mixamorig")
        // This prevents "handr" (from hand_r) matching inside "lefthandring"
        if (tok.size() <= 6) {
            // For short tokens, verify the match isn't in the middle of another bone name
            // by checking that what follows isn't more bone-name-like content
            size_t after = pos + tok.size();
            if (after < canon.size()) {
                // If the token ends with a sidedness marker (l/r) and more alpha chars follow,
                // this might be a false positive (e.g., "handr" in "handring")
                char lastTokChar = tok.back();
                bool endsWithSide = (lastTokChar == 'l' || lastTokChar == 'r');
                if (endsWithSide && std::isalpha(static_cast<unsigned char>(canon[after]))) {
                    // Check if this looks like a false substring match
                    // "handr" matching "handring" is bad, but "handr" matching "hand_r" is good
                    // Since we canonicalize, we can't distinguish _ from nothing, so check for
                    // common patterns that indicate false matches
                    std::string remainder = canon.substr(after);
                    // Common suffixes that indicate this is a different bone: "ing", "oll", etc.
                    if (remainder.find("ing") == 0 || remainder.find("oll") == 0) {
                        continue; // False positive, try next candidate
                    }
                }
            }
        }

        return true;
    }
    return false;
}

const std::unordered_map<HumanoidBone, std::vector<std::string>>& avatar_builders::DefaultNameSeeds()
{
    static std::unordered_map<HumanoidBone, std::vector<std::string>> map = {
        { HumanoidBone::Root, {"Root","Armature","ArmatureRoot","root","B_Root","B_Armature","B_ArmatureRoot"} },
        { HumanoidBone::Hips, {"Hips","Pelvis","hip","pelvis","root_pelvis","B_Hips","B_Pelvis"} },
        { HumanoidBone::Spine, {"Spine","Spine1","spine01","torso","B_Spine","B_Spine1"} },
        { HumanoidBone::Chest, {"Chest","Spine2","upperchest","chest","B_Chest","B_Spine2"} },
        { HumanoidBone::UpperChest, {"UpperChest","Spine3","upper_spine","B_UpperChest","B_Spine3"} },
        { HumanoidBone::Neck, {"Neck","neck","B_Neck"} },
        { HumanoidBone::Head, {"Head","head","B_Head"} },
        // Eyes
        { HumanoidBone::LeftEye, {"LeftEye","Eye_L","Eye.L","eye_l","lefteye"} },
        { HumanoidBone::RightEye, {"RightEye","Eye_R","Eye.R","eye_r","righteye"} },
        { HumanoidBone::LeftShoulder, {"LeftShoulder","L_Shoulder","clavicle_l","shoulder_l","B_LeftShoulder","B_L_Shoulder"} },
        { HumanoidBone::LeftUpperArm, {"LeftArm","LeftUpperArm","upperarm_l","arm_l","B_LeftUpperArm","B_L_UpperArm"} },
        { HumanoidBone::LeftLowerArm, {"LeftForeArm","LeftLowerArm","lowerarm_l","forearm_l","B_LeftLowerArm","B_L_ForeArm"} },
        { HumanoidBone::LeftHand, {"LeftHand","hand_l","B_LeftHand","B_L_Hand"} },
        { HumanoidBone::RightShoulder, {"RightShoulder","R_Shoulder","clavicle_r","shoulder_r","B_RightShoulder","B_R_Shoulder"} },
        { HumanoidBone::RightUpperArm, {"RightArm","RightUpperArm","upperarm_r","arm_r","B_RightUpperArm","B_R_UpperArm"} },
        { HumanoidBone::RightLowerArm, {"RightForeArm","RightLowerArm","lowerarm_r","forearm_r","B_RightLowerArm","B_R_ForeArm"} },
        { HumanoidBone::RightHand, {"RightHand","hand_r","B_RightHand","B_R_Hand"} },
        { HumanoidBone::LeftUpperLeg, {"LeftUpLeg","LeftThigh","thigh_l","upleg_l","B_LeftUpperLeg","B_L_UpperLeg"} },
        { HumanoidBone::LeftLowerLeg, {"LeftLeg","LeftCalf","calf_l","leg_l","B_LeftLowerLeg","B_L_LowerLeg"} },
        { HumanoidBone::LeftFoot, {"LeftFoot","foot_l","B_LeftFoot","B_L_Foot"} },
        { HumanoidBone::LeftToes, {"LeftToeBase","toe_l","toes_l","B_LeftToes","B_L_Toe"} },
        { HumanoidBone::RightUpperLeg, {"RightUpLeg","RightThigh","thigh_r","upleg_r","B_RightUpperLeg","B_R_UpperLeg"} },
        { HumanoidBone::RightLowerLeg, {"RightLeg","RightCalf","calf_r","leg_r","B_RightLowerLeg","B_R_LowerLeg"} },
        { HumanoidBone::RightFoot, {"RightFoot","foot_r","B_RightFoot","B_R_Foot"} },
        { HumanoidBone::RightToes, {"RightToeBase","toe_r","toes_r","B_RightToes","B_R_Toe"} },

        // Left fingers (Mixamo style: LeftHand{Thumb/Index/Middle/Ring/Pinky}{1,2,3})
        { HumanoidBone::LeftThumbProx, {"LeftHandThumb1","Thumb1_L","LThumb1","thumb_01_l","B-thumb01.L"} },
        { HumanoidBone::LeftThumbInter, {"LeftHandThumb2","Thumb2_L","LThumb2","thumb_02_l","B-thumb02.L"} },
        { HumanoidBone::LeftThumbDist, {"LeftHandThumb3","Thumb3_L","LThumb3","thumb_03_l","B-thumb03.L"} },

        { HumanoidBone::LeftIndexProx, {"LeftHandIndex1","Index1_L","LIndex1","index_01_l","B-index01.L"} },
        { HumanoidBone::LeftIndexInter, {"LeftHandIndex2","Index2_L","LIndex2","index_02_l","B-index02.L"} },
        { HumanoidBone::LeftIndexDist, {"LeftHandIndex3","Index3_L","LIndex3","index_03_l","B-index03.L"} },

        { HumanoidBone::LeftMiddleProx, {"LeftHandMiddle1","Middle1_L","LMiddle1","middle_01_l","B-middle01.L"} },
        { HumanoidBone::LeftMiddleInter, {"LeftHandMiddle2","Middle2_L","LMiddle2","middle_02_l","B-middle02.L"} },
        { HumanoidBone::LeftMiddleDist, {"LeftHandMiddle3","Middle3_L","LMiddle3","middle_03_l","B-middle03.L"} },

        { HumanoidBone::LeftRingProx, {"LeftHandRing1","Ring1_L","LRing1","ring_01_l","B-ring01.L"} },
        { HumanoidBone::LeftRingInter, {"LeftHandRing2","Ring2_L","LRing2","ring_02_l","B-ring02.L"} },
        { HumanoidBone::LeftRingDist, {"LeftHandRing3","Ring3_L","LRing3","ring_03_l","B-ring03.L"} },

        { HumanoidBone::LeftLittleProx, {"LeftHandPinky1","Pinky1_L","LLittle1","pinky_01_l","little_01_l","B-pinky01.L"} },
        { HumanoidBone::LeftLittleInter, {"LeftHandPinky2","Pinky2_L","LLittle2","pinky_02_l","little_02_l","B-pinky02.L"} },
        { HumanoidBone::LeftLittleDist, {"LeftHandPinky3","Pinky3_L","LLittle3","pinky_03_l","little_03_l","B-pinky03.L"} },

        // Right fingers
        { HumanoidBone::RightThumbProx, {"RightHandThumb1","Thumb1_R","RThumb1","thumb_01_r","B-thumb01.R"} },
        { HumanoidBone::RightThumbInter, {"RightHandThumb2","Thumb2_R","RThumb2","thumb_02_r","B-thumb02.R"} },
        { HumanoidBone::RightThumbDist, {"RightHandThumb3","Thumb3_R","RThumb3","thumb_03_r","B-thumb03.R"} },

        { HumanoidBone::RightIndexProx, {"RightHandIndex1","Index1_R","RIndex1","index_01_r","B-index01.R"} },
        { HumanoidBone::RightIndexInter, {"RightHandIndex2","Index2_R","RIndex2","index_02_r","B-index02.R"} },
        { HumanoidBone::RightIndexDist, {"RightHandIndex3","Index3_R","RIndex3","index_03_r","B-index03.R"} },

        { HumanoidBone::RightMiddleProx, {"RightHandMiddle1","Middle1_R","RMiddle1","middle_01_r","B-middle01.R"} },
        { HumanoidBone::RightMiddleInter, {"RightHandMiddle2","Middle2_R","RMiddle2","middle_02_r","B-middle02.R"} },
        { HumanoidBone::RightMiddleDist, {"RightHandMiddle3","Middle3_R","RMiddle3","middle_03_r","B-middle03.R"} },

        { HumanoidBone::RightRingProx, {"RightHandRing1","Ring1_R","RRing1","ring_01_r","B-ring01.R"} },
        { HumanoidBone::RightRingInter, {"RightHandRing2","Ring2_R","RRing2","ring_02_r","B-ring02.R"} },
        { HumanoidBone::RightRingDist, {"RightHandRing3","Ring3_R","RRing3","ring_03_r","B-ring03.R"} },

        { HumanoidBone::RightLittleProx, {"RightHandPinky1","Pinky1_R","RLittle1","pinky_01_r","little_01_r","B-pinky01.R"} },
        { HumanoidBone::RightLittleInter, {"RightHandPinky2","Pinky2_R","RLittle2","pinky_02_r","little_02_r","B-pinky02.R"} },
        { HumanoidBone::RightLittleDist, {"RightHandPinky3","Pinky3_R","RLittle3","pinky_03_r","little_03_r","B-pinky03.R"} },

        // Common twist naming seen across rigs (include Mixamo-style and Roll variants)
        { HumanoidBone::LeftUpperArmTwist, {"LeftUpperArmTwist","LeftArmTwist","UpperArmTwist_L","upperarm_twist_l","arm_twist_01_l","LeftArmRoll"} },
        { HumanoidBone::LeftLowerArmTwist, {"LeftLowerArmTwist","LeftForeArmTwist","ForeArmTwist_L","forearm_twist_l","arm_twist_02_l","LeftForeArmRoll"} },
        { HumanoidBone::RightUpperArmTwist, {"RightUpperArmTwist","RightArmTwist","UpperArmTwist_R","upperarm_twist_r","arm_twist_01_r","RightArmRoll"} },
        { HumanoidBone::RightLowerArmTwist, {"RightLowerArmTwist","RightForeArmTwist","ForeArmTwist_R","forearm_twist_r","arm_twist_02_r","RightForeArmRoll"} },
        { HumanoidBone::LeftUpperLegTwist, {"LeftUpperLegTwist","LeftUpLegTwist","ThighTwist_L","thigh_twist_01_l","LeftUpLegRoll"} },
        { HumanoidBone::LeftLowerLegTwist, {"LeftLowerLegTwist","LeftLegTwist","CalfTwist_L","calf_twist_01_l","LeftLegRoll"} },
        { HumanoidBone::RightUpperLegTwist, {"RightUpperLegTwist","RightUpLegTwist","ThighTwist_R","thigh_twist_01_r","RightUpLegRoll"} },
        { HumanoidBone::RightLowerLegTwist, {"RightLowerLegTwist","RightLegTwist","CalfTwist_R","calf_twist_01_r","RightLegRoll"} },
    };
    return map;
}

void avatar_builders::BuildFromSkeleton(const SkeletonComponent& skeleton,
                                        AvatarDefinition& outAvatar,
                                        bool autoMap,
                                        const std::unordered_map<HumanoidBone, std::vector<std::string>>& nameMap)
{
    const auto& seeds = nameMap.empty() ? DefaultNameSeeds() : nameMap;

    // Build reverse name list for quick testing
    std::vector<std::string> names;
    names.reserve(skeleton.BoneNameToIndex.size());
    for (const auto& [name, idx] : skeleton.BoneNameToIndex) {
        if (idx >= 0) {
            if ((size_t)idx >= names.size()) names.resize(idx + 1);
            names[idx] = name;
        }
    }

    // Reset mapping
    for (uint16_t i = 0; i < HumanoidBoneCount; ++i) {
        outAvatar.Map[i].BoneIndex = -1;
        outAvatar.Map[i].BoneName.clear();
        outAvatar.Present[i] = false;
        outAvatar.BindModel[i] = glm::mat4(1.0f);
        outAvatar.BindLocal[i] = glm::mat4(1.0f);
    }

    if (autoMap) {
        // Track which skeleton bones have already been claimed to avoid duplicates
        std::vector<bool> used(names.size(), false);
        for (uint16_t i = 0; i < HumanoidBoneCount; ++i) {
            HumanoidBone hb = static_cast<HumanoidBone>(i);
            auto it = seeds.find(hb);
            if (it == seeds.end()) continue;

            // Linear scan bone names for candidates
            for (size_t bi = 0; bi < names.size(); ++bi) {
                const std::string& n = names[bi];
                if (n.empty()) continue;
                if (bi < used.size() && used[bi]) continue; // already assigned to another humanoid bone
                if (name_matches(n, it->second, hb)) {
                    outAvatar.Map[i].BoneIndex = (int32_t)bi;
                    outAvatar.Map[i].BoneName = n;
                    outAvatar.Present[i] = true;
                    if (bi < used.size()) used[bi] = true;
                    break;
                }
            }
        }
    }

    PopulateBindDataFromSkeleton(skeleton, outAvatar);
}

void avatar_builders::PopulateBindDataFromSkeleton(const SkeletonComponent& skeleton,
                                                   AvatarDefinition& avatar)
{
    if (avatar.Map.size() < HumanoidBoneCount) avatar.Map.resize(HumanoidBoneCount);
    if (avatar.BindModel.size() < HumanoidBoneCount) avatar.BindModel.resize(HumanoidBoneCount, glm::mat4(1.0f));
    if (avatar.BindLocal.size() < HumanoidBoneCount) avatar.BindLocal.resize(HumanoidBoneCount, glm::mat4(1.0f));
    if (avatar.RetargetModel.size() < HumanoidBoneCount) avatar.RetargetModel.resize(HumanoidBoneCount, glm::mat4(1.0f));
    if (avatar.Present.size() < HumanoidBoneCount) avatar.Present.resize(HumanoidBoneCount, false);
    if (avatar.RestOffsetRot.size() < HumanoidBoneCount) avatar.RestOffsetRot.resize(HumanoidBoneCount, glm::quat(1, 0, 0, 0));

    std::vector<glm::mat4> modelBind(skeleton.InverseBindPoses.size(), glm::mat4(1.0f));
    for (size_t bi = 0; bi < skeleton.InverseBindPoses.size(); ++bi) {
        modelBind[bi] = glm::inverse(skeleton.InverseBindPoses[bi]);
    }

    for (uint16_t i = 0; i < HumanoidBoneCount; ++i) {
        avatar.Map[i].Bone = static_cast<HumanoidBone>(i);
        avatar.BindModel[i] = glm::mat4(1.0f);
        avatar.BindLocal[i] = glm::mat4(1.0f);

        if (!avatar.Present[i]) continue;

        const int32_t bi = avatar.Map[i].BoneIndex;
        if (bi < 0 || static_cast<size_t>(bi) >= modelBind.size()) {
            avatar.Present[i] = false;
            continue;
        }

        if (avatar.Map[i].BoneName.empty() && static_cast<size_t>(bi) < skeleton.BoneNames.size()) {
            avatar.Map[i].BoneName = skeleton.BoneNames[static_cast<size_t>(bi)];
        }

        avatar.BindModel[i] = modelBind[static_cast<size_t>(bi)];
        int parentIdx = (static_cast<size_t>(bi) < skeleton.BoneParents.size()) ? skeleton.BoneParents[static_cast<size_t>(bi)] : -1;
        glm::mat4 parentModel = (parentIdx >= 0 && static_cast<size_t>(parentIdx) < modelBind.size())
            ? modelBind[static_cast<size_t>(parentIdx)]
            : glm::mat4(1.0f);
        avatar.BindLocal[i] = glm::inverse(parentModel) * avatar.BindModel[i];
    }
}

int HumanoidAvatar::HumanToSkeleton(int humanBoneId, const ::SkeletonComponent& skeleton) const {
    HumanBone hb = static_cast<HumanBone>(humanBoneId);
    auto it = BoneMapping.find(hb);
    if (it == BoneMapping.end()) return -1;
    return skeleton.GetBoneIndex(it->second);
}

} // namespace animation
} // namespace cm


