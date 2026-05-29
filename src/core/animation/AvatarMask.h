#pragma once

#include "HumanoidBone.h"
#include <bitset>
#include <string>
#include <cstdint>

namespace cm {
namespace animation {

/// Preset body mask types for common animation layering scenarios
enum class BodyMaskPreset : uint8_t {
    FullBody = 0,
    UpperBody = 1,
    LowerBody = 2,
    LeftArm = 3,
    RightArm = 4,
    Head = 5,
    Spine = 6,
    LeftHand = 7,
    RightHand = 8,
    Arms = 9,      // Both arms
    Legs = 10,     // Both legs without hips
    Custom = 255   // User-defined mask
};

/// Bitmask defining which humanoid bones are included in an animation layer.
/// Used for partial body animation layering (e.g., upper body override, arm-only animations).
struct AvatarMask {
    std::bitset<HumanoidBoneCount> IncludedBones;
    std::string Name;
    BodyMaskPreset Preset = BodyMaskPreset::FullBody;
    
    AvatarMask() = default;
    explicit AvatarMask(BodyMaskPreset preset) { ApplyPreset(preset); }
    
    /// Check if a specific humanoid bone is included in this mask
    bool IncludesBone(HumanoidBone b) const {
        return IncludedBones[static_cast<uint16_t>(b)];
    }
    
    /// Set whether a specific bone is included
    void SetBone(HumanoidBone b, bool included) {
        IncludedBones[static_cast<uint16_t>(b)] = included;
    }
    
    /// Apply a preset mask configuration
    void ApplyPreset(BodyMaskPreset preset);
    
    /// Get human-readable name for a preset
    static const char* GetPresetName(BodyMaskPreset preset);
    
    // =========================================================================
    // Factory Methods for Common Presets
    // =========================================================================
    
    static AvatarMask FullBody();
    static AvatarMask UpperBody();
    static AvatarMask LowerBody();
    static AvatarMask LeftArm();
    static AvatarMask RightArm();
    static AvatarMask Head();
    static AvatarMask Spine();
    static AvatarMask LeftHand();
    static AvatarMask RightHand();
    static AvatarMask Arms();
    static AvatarMask Legs();
};

// =========================================================================
// Inline Implementations
// =========================================================================

inline const char* AvatarMask::GetPresetName(BodyMaskPreset preset) {
    switch (preset) {
        case BodyMaskPreset::FullBody: return "Full Body";
        case BodyMaskPreset::UpperBody: return "Upper Body";
        case BodyMaskPreset::LowerBody: return "Lower Body";
        case BodyMaskPreset::LeftArm: return "Left Arm";
        case BodyMaskPreset::RightArm: return "Right Arm";
        case BodyMaskPreset::Head: return "Head";
        case BodyMaskPreset::Spine: return "Spine";
        case BodyMaskPreset::LeftHand: return "Left Hand";
        case BodyMaskPreset::RightHand: return "Right Hand";
        case BodyMaskPreset::Arms: return "Both Arms";
        case BodyMaskPreset::Legs: return "Legs";
        case BodyMaskPreset::Custom: return "Custom";
        default: return "Unknown";
    }
}

inline AvatarMask AvatarMask::FullBody() {
    AvatarMask m;
    m.Name = "FullBody";
    m.Preset = BodyMaskPreset::FullBody;
    m.IncludedBones.set(); // All bones included
    return m;
}

inline AvatarMask AvatarMask::UpperBody() {
    AvatarMask m;
    m.Name = "UpperBody";
    m.Preset = BodyMaskPreset::UpperBody;
    
    // Spine and chest
    m.SetBone(HumanoidBone::Spine, true);
    m.SetBone(HumanoidBone::Chest, true);
    m.SetBone(HumanoidBone::UpperChest, true);
    
    // Head and neck
    m.SetBone(HumanoidBone::Neck, true);
    m.SetBone(HumanoidBone::Head, true);
    m.SetBone(HumanoidBone::LeftEye, true);
    m.SetBone(HumanoidBone::RightEye, true);
    
    // Left arm chain
    m.SetBone(HumanoidBone::LeftShoulder, true);
    m.SetBone(HumanoidBone::LeftUpperArm, true);
    m.SetBone(HumanoidBone::LeftLowerArm, true);
    m.SetBone(HumanoidBone::LeftHand, true);
    m.SetBone(HumanoidBone::LeftUpperArmTwist, true);
    m.SetBone(HumanoidBone::LeftLowerArmTwist, true);
    
    // Right arm chain
    m.SetBone(HumanoidBone::RightShoulder, true);
    m.SetBone(HumanoidBone::RightUpperArm, true);
    m.SetBone(HumanoidBone::RightLowerArm, true);
    m.SetBone(HumanoidBone::RightHand, true);
    m.SetBone(HumanoidBone::RightUpperArmTwist, true);
    m.SetBone(HumanoidBone::RightLowerArmTwist, true);
    
    // All fingers
    for (int i = static_cast<int>(HumanoidBone::LeftThumbProx); 
         i <= static_cast<int>(HumanoidBone::RightLittleDist); ++i) {
        m.SetBone(static_cast<HumanoidBone>(i), true);
    }
    
    return m;
}

inline AvatarMask AvatarMask::LowerBody() {
    AvatarMask m;
    m.Name = "LowerBody";
    m.Preset = BodyMaskPreset::LowerBody;
    
    m.SetBone(HumanoidBone::Hips, true);
    
    // Left leg
    m.SetBone(HumanoidBone::LeftUpperLeg, true);
    m.SetBone(HumanoidBone::LeftLowerLeg, true);
    m.SetBone(HumanoidBone::LeftFoot, true);
    m.SetBone(HumanoidBone::LeftToes, true);
    m.SetBone(HumanoidBone::LeftUpperLegTwist, true);
    m.SetBone(HumanoidBone::LeftLowerLegTwist, true);
    
    // Right leg
    m.SetBone(HumanoidBone::RightUpperLeg, true);
    m.SetBone(HumanoidBone::RightLowerLeg, true);
    m.SetBone(HumanoidBone::RightFoot, true);
    m.SetBone(HumanoidBone::RightToes, true);
    m.SetBone(HumanoidBone::RightUpperLegTwist, true);
    m.SetBone(HumanoidBone::RightLowerLegTwist, true);
    
    return m;
}

inline AvatarMask AvatarMask::LeftArm() {
    AvatarMask m;
    m.Name = "LeftArm";
    m.Preset = BodyMaskPreset::LeftArm;
    
    m.SetBone(HumanoidBone::LeftShoulder, true);
    m.SetBone(HumanoidBone::LeftUpperArm, true);
    m.SetBone(HumanoidBone::LeftLowerArm, true);
    m.SetBone(HumanoidBone::LeftHand, true);
    m.SetBone(HumanoidBone::LeftUpperArmTwist, true);
    m.SetBone(HumanoidBone::LeftLowerArmTwist, true);
    
    // Left fingers
    for (int i = static_cast<int>(HumanoidBone::LeftThumbProx);
         i <= static_cast<int>(HumanoidBone::LeftLittleDist); ++i) {
        m.SetBone(static_cast<HumanoidBone>(i), true);
    }
    
    return m;
}

inline AvatarMask AvatarMask::RightArm() {
    AvatarMask m;
    m.Name = "RightArm";
    m.Preset = BodyMaskPreset::RightArm;
    
    m.SetBone(HumanoidBone::RightShoulder, true);
    m.SetBone(HumanoidBone::RightUpperArm, true);
    m.SetBone(HumanoidBone::RightLowerArm, true);
    m.SetBone(HumanoidBone::RightHand, true);
    m.SetBone(HumanoidBone::RightUpperArmTwist, true);
    m.SetBone(HumanoidBone::RightLowerArmTwist, true);
    
    // Right fingers
    for (int i = static_cast<int>(HumanoidBone::RightThumbProx);
         i <= static_cast<int>(HumanoidBone::RightLittleDist); ++i) {
        m.SetBone(static_cast<HumanoidBone>(i), true);
    }
    
    return m;
}

inline AvatarMask AvatarMask::Head() {
    AvatarMask m;
    m.Name = "Head";
    m.Preset = BodyMaskPreset::Head;
    
    m.SetBone(HumanoidBone::Neck, true);
    m.SetBone(HumanoidBone::Head, true);
    m.SetBone(HumanoidBone::LeftEye, true);
    m.SetBone(HumanoidBone::RightEye, true);
    
    return m;
}

inline AvatarMask AvatarMask::Spine() {
    AvatarMask m;
    m.Name = "Spine";
    m.Preset = BodyMaskPreset::Spine;
    
    m.SetBone(HumanoidBone::Spine, true);
    m.SetBone(HumanoidBone::Chest, true);
    m.SetBone(HumanoidBone::UpperChest, true);
    
    return m;
}

inline AvatarMask AvatarMask::LeftHand() {
    AvatarMask m;
    m.Name = "LeftHand";
    m.Preset = BodyMaskPreset::LeftHand;
    
    m.SetBone(HumanoidBone::LeftHand, true);
    
    // Left fingers only
    for (int i = static_cast<int>(HumanoidBone::LeftThumbProx);
         i <= static_cast<int>(HumanoidBone::LeftLittleDist); ++i) {
        m.SetBone(static_cast<HumanoidBone>(i), true);
    }
    
    return m;
}

inline AvatarMask AvatarMask::RightHand() {
    AvatarMask m;
    m.Name = "RightHand";
    m.Preset = BodyMaskPreset::RightHand;
    
    m.SetBone(HumanoidBone::RightHand, true);
    
    // Right fingers only
    for (int i = static_cast<int>(HumanoidBone::RightThumbProx);
         i <= static_cast<int>(HumanoidBone::RightLittleDist); ++i) {
        m.SetBone(static_cast<HumanoidBone>(i), true);
    }
    
    return m;
}

inline AvatarMask AvatarMask::Arms() {
    AvatarMask m;
    m.Name = "Arms";
    m.Preset = BodyMaskPreset::Arms;
    
    // Left arm
    m.SetBone(HumanoidBone::LeftShoulder, true);
    m.SetBone(HumanoidBone::LeftUpperArm, true);
    m.SetBone(HumanoidBone::LeftLowerArm, true);
    m.SetBone(HumanoidBone::LeftHand, true);
    m.SetBone(HumanoidBone::LeftUpperArmTwist, true);
    m.SetBone(HumanoidBone::LeftLowerArmTwist, true);
    
    // Right arm
    m.SetBone(HumanoidBone::RightShoulder, true);
    m.SetBone(HumanoidBone::RightUpperArm, true);
    m.SetBone(HumanoidBone::RightLowerArm, true);
    m.SetBone(HumanoidBone::RightHand, true);
    m.SetBone(HumanoidBone::RightUpperArmTwist, true);
    m.SetBone(HumanoidBone::RightLowerArmTwist, true);
    
    // All fingers
    for (int i = static_cast<int>(HumanoidBone::LeftThumbProx);
         i <= static_cast<int>(HumanoidBone::RightLittleDist); ++i) {
        m.SetBone(static_cast<HumanoidBone>(i), true);
    }
    
    return m;
}

inline AvatarMask AvatarMask::Legs() {
    AvatarMask m;
    m.Name = "Legs";
    m.Preset = BodyMaskPreset::Legs;
    
    // Left leg (without hips - hips affect whole body)
    m.SetBone(HumanoidBone::LeftUpperLeg, true);
    m.SetBone(HumanoidBone::LeftLowerLeg, true);
    m.SetBone(HumanoidBone::LeftFoot, true);
    m.SetBone(HumanoidBone::LeftToes, true);
    m.SetBone(HumanoidBone::LeftUpperLegTwist, true);
    m.SetBone(HumanoidBone::LeftLowerLegTwist, true);
    
    // Right leg
    m.SetBone(HumanoidBone::RightUpperLeg, true);
    m.SetBone(HumanoidBone::RightLowerLeg, true);
    m.SetBone(HumanoidBone::RightFoot, true);
    m.SetBone(HumanoidBone::RightToes, true);
    m.SetBone(HumanoidBone::RightUpperLegTwist, true);
    m.SetBone(HumanoidBone::RightLowerLegTwist, true);
    
    return m;
}

inline void AvatarMask::ApplyPreset(BodyMaskPreset preset) {
    IncludedBones.reset();
    Preset = preset;
    
    switch (preset) {
        case BodyMaskPreset::FullBody:   *this = FullBody(); break;
        case BodyMaskPreset::UpperBody:  *this = UpperBody(); break;
        case BodyMaskPreset::LowerBody:  *this = LowerBody(); break;
        case BodyMaskPreset::LeftArm:    *this = LeftArm(); break;
        case BodyMaskPreset::RightArm:   *this = RightArm(); break;
        case BodyMaskPreset::Head:       *this = Head(); break;
        case BodyMaskPreset::Spine:      *this = Spine(); break;
        case BodyMaskPreset::LeftHand:   *this = LeftHand(); break;
        case BodyMaskPreset::RightHand:  *this = RightHand(); break;
        case BodyMaskPreset::Arms:       *this = Arms(); break;
        case BodyMaskPreset::Legs:       *this = Legs(); break;
        case BodyMaskPreset::Custom:     break; // Keep current bits
    }
}

} // namespace animation
} // namespace cm

