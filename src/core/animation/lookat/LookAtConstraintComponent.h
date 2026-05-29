// LookAtConstraintComponent.h
// Pre-IK rotation-only constraint for look/aim behavior
// Operates AFTER animation sampling, BEFORE IK solving
#pragma once

#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

using EntityID = uint32_t;

namespace cm { namespace animation { namespace lookat {

using BoneId = int32_t;

//------------------------------------------------------------------------------
// AxisMask: Which axes to affect in the look rotation
//------------------------------------------------------------------------------
enum class AxisMask : uint8_t {
    None      = 0,
    Yaw       = 1 << 0,  // Y-axis rotation (left/right)
    Pitch     = 1 << 1,  // X-axis rotation (up/down)
    Roll      = 1 << 2,  // Z-axis rotation (tilt)
    YawPitch  = Yaw | Pitch,
    All       = Yaw | Pitch | Roll
};

inline AxisMask operator|(AxisMask a, AxisMask b) {
    return static_cast<AxisMask>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
inline AxisMask operator&(AxisMask a, AxisMask b) {
    return static_cast<AxisMask>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}
inline bool HasFlag(AxisMask mask, AxisMask flag) {
    return (static_cast<uint8_t>(mask) & static_cast<uint8_t>(flag)) != 0;
}

//------------------------------------------------------------------------------
// LookAtSpace: Reference space for the constraint
//------------------------------------------------------------------------------
enum class LookAtSpace : uint8_t {
    Local,      // Rotation relative to bone's local space
    Component,  // Rotation relative to skeleton root (component space)
    World       // Rotation in world space (for stationary targets)
};

//------------------------------------------------------------------------------
// DistributionMode: How rotation is distributed across bone chain
//------------------------------------------------------------------------------
enum class DistributionMode : uint8_t {
    Equal,      // Equal distribution across all bones
    Linear,     // Linear falloff (root gets more, tip gets less)
    Weighted,   // Use explicit per-bone weights
};

//------------------------------------------------------------------------------
// LookAtMode: How the constraint derives rotation
//------------------------------------------------------------------------------
enum class LookAtMode : uint8_t {
    LookAtPosition,  // Rotate bones to face target entity's world position (default)
    MatchRotation,   // Copy target entity's yaw/pitch rotation (for FPS cameras)
};

//------------------------------------------------------------------------------
// LookAtConstraintComponent
// 
// Rotates a chain of bones toward a target entity. This is a rotation-only
// constraint that does NOT move bones - it's designed to work in layers:
//   1. Animation → base pose
//   2. LookAt/Aim → rotation adjustments (this system)
//   3. IK → positional corrections
//
// For spine look-at, assign 3-5 spine bones with distributed rotation.
// For head tracking, assign just the head/neck bones.
//------------------------------------------------------------------------------
struct LookAtConstraintComponent {
    // ========================================================================
    // Authoring (Serialized)
    // ========================================================================
    
    // Master enable flag
    bool Enabled = true;
    
    // How to derive rotation from target
    // LookAtPosition: face the target's world position (for third-person, NPCs)
    // MatchRotation: copy target's yaw/pitch rotation (for FPS cameras)
    LookAtMode Mode = LookAtMode::LookAtPosition;
    
    // Target entity to look at (camera, aim pivot, other character, etc.)
    EntityID TargetEntity = 0;
    
    // GUID of target entity for robust serialization (resolved after scene load)
    // This is populated during serialization and used to restore TargetEntity
    uint64_t TargetEntityGuidHigh = 0;
    uint64_t TargetEntityGuidLow = 0;
    
    // DEPRECATED: No longer used. MatchRotation mode now automatically assumes
    // camera convention (-Z forward). Kept for serialization compatibility.
    bool TargetUsesNegativeZForward = false;
    
    // Bones to apply rotation to (ordered root → tip for distribution)
    // For spine: [Spine, Spine1, Spine2, Chest, UpperChest]
    // For head: [Neck, Head]
    std::vector<BoneId> BoneChain;
    
    // Blend weight [0, 1] - interpolates between original and look rotation
    float Weight = 1.0f;
    
    // Which axes to affect
    AxisMask Axes = AxisMask::YawPitch;
    
    // Reference space for rotation
    LookAtSpace Space = LookAtSpace::Component;
    
    // How to distribute rotation across the bone chain
    DistributionMode Distribution = DistributionMode::Linear;
    
    // Per-bone weights when Distribution == Weighted
    // Size should match BoneChain; values are normalized internally
    std::vector<float> BoneWeights;
    
    // ========================================================================
    // Limits (Serialized)
    // ========================================================================
    
    // Maximum rotation angles (degrees) - prevents over-rotation
    float MaxYawDeg = 70.0f;    // Left/right limit
    float MaxPitchDeg = 45.0f;  // Up/down limit
    float MaxRollDeg = 15.0f;   // Tilt limit (rarely used)
    
    // ========================================================================
    // Smoothing (Serialized)
    // ========================================================================
    
    // Smoothing speed (0 = instant, higher = smoother/slower)
    // Implemented as exponential smoothing: new = lerp(old, target, 1 - exp(-speed * dt))
    float SmoothingSpeed = 10.0f;
    
    // ========================================================================
    // Debug (Serialized)
    // ========================================================================
    
    // Show debug visualization in editor
    bool Visualize = false;
    
    // ========================================================================
    // Runtime Cache (NOT Serialized)
    // ========================================================================
    
    // Smoothed rotation values (persisted across frames)
    float SmoothedYaw = 0.0f;
    float SmoothedPitch = 0.0f;
    float SmoothedRoll = 0.0f;
    
    // Normalized distribution weights (computed once when chain changes)
    std::vector<float> NormalizedWeights;
    
    // Last known valid state
    bool WasValidLastFrame = false;
    
    // Managed interop handle (if exposed to C#)
    uint64_t ManagedHandle = 0;
    
    // ========================================================================
    // Methods
    // ========================================================================
    
    // Set blend weight with clamping
    void SetWeight(float w) { Weight = glm::clamp(w, 0.0f, 1.0f); }
    
    // Set target entity
    void SetTarget(EntityID e) { TargetEntity = e; }
    
    // Set bone chain (triggers weight recalculation)
    void SetBoneChain(const std::vector<BoneId>& bones) {
        BoneChain = bones;
        InvalidateWeights();
    }
    
    // Invalidate cached weights (call when chain or distribution mode changes)
    void InvalidateWeights() {
        NormalizedWeights.clear();
    }
    
    // Compute normalized weights based on distribution mode
    void ComputeNormalizedWeights() {
        const size_t n = BoneChain.size();
        if (n == 0) {
            NormalizedWeights.clear();
            return;
        }
        
        NormalizedWeights.resize(n);
        
        switch (Distribution) {
            case DistributionMode::Equal: {
                float w = 1.0f / static_cast<float>(n);
                for (size_t i = 0; i < n; ++i) {
                    NormalizedWeights[i] = w;
                }
            } break;
            
            case DistributionMode::Linear: {
                // Linear falloff: root bones get more rotation
                // Weight[i] = (n - i) / sum(1..n)
                float sum = static_cast<float>(n * (n + 1)) / 2.0f;
                for (size_t i = 0; i < n; ++i) {
                    NormalizedWeights[i] = static_cast<float>(n - i) / sum;
                }
            } break;
            
            case DistributionMode::Weighted: {
                // Use explicit weights, normalize to sum to 1
                float sum = 0.0f;
                for (size_t i = 0; i < n; ++i) {
                    float w = (i < BoneWeights.size()) ? BoneWeights[i] : 1.0f;
                    NormalizedWeights[i] = w;
                    sum += w;
                }
                if (sum > 1e-6f) {
                    for (size_t i = 0; i < n; ++i) {
                        NormalizedWeights[i] /= sum;
                    }
                }
            } break;
        }
    }
    
    // Get weight for bone at index (computes if needed)
    float GetBoneWeight(size_t index) {
        if (NormalizedWeights.empty()) {
            ComputeNormalizedWeights();
        }
        return (index < NormalizedWeights.size()) ? NormalizedWeights[index] : 0.0f;
    }
};

} } } // namespace cm::animation::lookat
