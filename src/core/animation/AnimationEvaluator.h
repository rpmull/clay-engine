#pragma once

#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "core/animation/AnimationTypes.h"
#include "core/animation/AvatarDefinition.h"
#include "core/ecs/AnimationComponents.h"

// New unified interfaces
#include "core/animation/AnimationAsset.h"
#include "core/animation/BindingCache.h"

struct SkeletonComponent; // forward

namespace cm {
namespace animation {

// Legacy helpers
glm::vec3 SampleVec3(const std::vector<KeyframeVec3>& keys, float time, size_t& cacheIdx);
glm::quat SampleQuat(const std::vector<KeyframeQuat>& keys, float time, size_t& cacheIdx);

void EvaluateAnimation(const AnimationClip& clip,
                       float time,
                       const ::SkeletonComponent& skeleton,
                       std::vector<glm::mat4>& outLocalTransforms,
                       const AvatarDefinition* avatar = nullptr);

// Unified evaluator API
struct PoseBuffer { std::vector<glm::mat4> local; std::vector<uint8_t> touched; };
struct PoseTRSBuffer {
    std::vector<glm::vec3> translation;
    std::vector<glm::quat> rotation;
    std::vector<glm::vec3> scale;
    std::vector<uint8_t> touched;
};
struct EvalInputs {
    const AnimationAsset* asset = nullptr;
    float time = 0.0f;
    bool loop = true;
    float prevTime = 0.0f;
    bool hasPrevTime = false;
    int loopCount = 0;
};
struct EvalTargets {
    PoseBuffer* pose = nullptr;
    PoseTRSBuffer* poseTrs = nullptr;
};
struct AvatarDefinition; // forward
struct EvalContext { const BindingCache* bindings = nullptr; const AvatarDefinition* avatar = nullptr; const ::SkeletonComponent* skeleton = nullptr; };

void SampleAsset(const EvalInputs&, const EvalContext&, EvalTargets& out,
                 std::vector<ScriptEvent>* firedEvents = nullptr, nlohmann::json* propertyWrites = nullptr);

// Convenience overload matching editor-facing API: returns pose buffer and optional property writes as a list
struct PropertyWrite { std::uint64_t id = 0; nlohmann::json value; };
void SampleAsset(const EvalInputs&, const EvalContext&, PoseBuffer& outPose,
                 std::vector<ScriptEvent>* outEvents = nullptr,
                 std::vector<PropertyWrite>* outProps = nullptr);

} // namespace animation
} // namespace cm
