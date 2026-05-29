#pragma once
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Constraints/Constraint.h>
#include <vector>
#include <glm/glm.hpp>
#include "core/ecs/Entity.h"

namespace cm::physics {

/// Per-bone ragdoll body info
struct RagdollBone {
    JPH::BodyID BodyID;
    int BoneIndex = -1;
    int ParentBoneIndex = -1;
    float Radius = 0.05f;       // Capsule radius
    float HalfHeight = 0.1f;    // Capsule half-height
};

/// Joint connecting two ragdoll bones
struct RagdollJoint {
    JPH::Ref<JPH::Constraint> Constraint;
    int ParentBoneIndex = -1;   // Index into Bones array
    int ChildBoneIndex = -1;    // Index into Bones array
};

/// Ragdoll state for an entity
struct RagdollComponent {
    bool Active = false;
    EntityID SkeletonEntity = INVALID_ENTITY_ID;
    std::vector<RagdollBone> Bones;
    std::vector<RagdollJoint> Joints;
    uint32_t PhysicsLayer = 1;  // Default physics layer for ragdoll bones
};

/// Bone size estimation based on common humanoid proportions
struct BoneSizeHint {
    float Radius = 0.04f;
    float Length = 0.2f;
    float Mass = 2.0f;
};

/// Get reasonable size estimates for humanoid bones
inline BoneSizeHint GetHumanoidBoneSize(const std::string& boneName) {
    BoneSizeHint hint;
    
    // Head
    if (boneName.find("Head") != std::string::npos) {
        hint.Radius = 0.1f; hint.Length = 0.15f; hint.Mass = 5.0f;
    }
    // Spine/Torso
    else if (boneName.find("Spine") != std::string::npos || 
             boneName.find("Hips") != std::string::npos) {
        hint.Radius = 0.12f; hint.Length = 0.15f; hint.Mass = 15.0f;
    }
    // Upper arm
    else if (boneName.find("Arm") != std::string::npos && 
             boneName.find("Fore") == std::string::npos) {
        hint.Radius = 0.045f; hint.Length = 0.25f; hint.Mass = 3.5f;
    }
    // Forearm
    else if (boneName.find("ForeArm") != std::string::npos) {
        hint.Radius = 0.038f; hint.Length = 0.22f; hint.Mass = 2.2f;
    }
    // Hand
    else if (boneName.find("Hand") != std::string::npos) {
        hint.Radius = 0.032f; hint.Length = 0.08f; hint.Mass = 0.9f;
    }
    // Upper leg
    else if (boneName.find("UpLeg") != std::string::npos || 
             boneName.find("Thigh") != std::string::npos) {
        hint.Radius = 0.06f; hint.Length = 0.4f; hint.Mass = 8.0f;
    }
    // Lower leg
    else if (boneName.find("Leg") != std::string::npos) {
        hint.Radius = 0.05f; hint.Length = 0.35f; hint.Mass = 5.0f;
    }
    // Foot
    else if (boneName.find("Foot") != std::string::npos || 
             boneName.find("Toe") != std::string::npos) {
        hint.Radius = 0.03f; hint.Length = 0.12f; hint.Mass = 1.0f;
    }
    // Neck
    else if (boneName.find("Neck") != std::string::npos) {
        hint.Radius = 0.04f; hint.Length = 0.08f; hint.Mass = 1.0f;
    }
    // Shoulder
    else if (boneName.find("Shoulder") != std::string::npos) {
        hint.Radius = 0.05f; hint.Length = 0.1f; hint.Mass = 1.5f;
    }
    // Default for fingers, etc.
    else {
        hint.Radius = 0.015f; hint.Length = 0.03f; hint.Mass = 0.1f;
    }
    
    return hint;
}

} // namespace cm::physics

