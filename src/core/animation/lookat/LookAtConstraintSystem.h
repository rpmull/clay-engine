// LookAtConstraintSystem.h
// Pre-IK rotation-only constraint system for look/aim behavior
#pragma once

#include "LookAtConstraintComponent.h"
#include <cstdint>

class Scene;

namespace cm { namespace animation { namespace lookat {

//------------------------------------------------------------------------------
// LookAtConstraintSystem
//
// Processes all LookAtConstraints in the scene, applying rotation-only
// adjustments to bone chains. This runs in the animation pipeline as:
//
//   Animation Sampling → LookAt/Aim (this) → IK Solving → Transform Update
//
// Design principles:
//   - Rotation only: bones rotate toward target, never translate
//   - Layered: works additively on top of animation pose
//   - Pre-IK: IK sees already-rotated bones, solves naturally
//   - Distributed: rotation can be spread across multiple bones (spine)
//   - Smooth: optional temporal smoothing prevents snapping
//
// Performance considerations:
//   - Single pass over skeleton entities
//   - Minimal allocations (reuses internal buffers)
//   - Early-out for disabled/zero-weight constraints
//   - Vectorizable angle math
//------------------------------------------------------------------------------
class LookAtConstraintSystem {
public:
    // Singleton access
    static LookAtConstraintSystem& Get() { 
        static LookAtConstraintSystem s_instance; 
        return s_instance; 
    }

    // Process all LookAtConstraints in the scene
    // Call AFTER AnimationSystem::Update(), BEFORE IKSystem::SolveAndBlend()
    void Apply(Scene& scene, float deltaTime);

private:
    LookAtConstraintSystem() = default;
    
    // Internal work buffers (reused to avoid allocations)
    std::vector<glm::mat4> m_LocalTransforms;
    std::vector<glm::mat4> m_WorldTransforms;
    std::vector<uint32_t> m_LocalLoadedStamp;
    std::vector<uint32_t> m_WorldValidStamp;
    std::vector<uint32_t> m_ModifiedStamp;
    std::vector<int> m_ModifiedBones;
    uint32_t m_LocalScratchStamp = 1;
    uint32_t m_WorldScratchStamp = 1;
};

} } } // namespace cm::animation::lookat
