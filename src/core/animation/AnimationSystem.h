#pragma once

#include "core/ecs/Scene.h"
#include "core/animation/AnimationEvaluator.h"
#include "core/animation/AnimationPlayerComponent.h"
#include "core/ecs/AnimationComponents.h"

namespace cm {
namespace animation {

class AnimationSystem {
public:
    // Call each frame.
    // Evaluates all animated skeletons with an AnimationPlayer in the scene.
    // Writes local bone transforms; skinning is applied in SkinningSystem::Update.
    static void Update(::Scene& scene, float deltaTime);
};

} // namespace animation
} // namespace cm
