#include "core/navigation/NavAgent.h"
#include "core/ecs/Components.h"
#include "core/physics/Physics.h"
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>

using namespace nav;

void NavAgentComponent::SetDestination(const glm::vec3& dest)
{
    // Only request new path if destination changed significantly
    // This prevents constant repathing when following a moving target
    const float kRepathThreshold = 2.0f; // Only repath if destination moved >2 units
    
    glm::vec3 oldDest = Destination;
    const bool hadDestination = HasDestination;
    Destination = dest;
    HasDestination = true;
    
    // Determine if we need a new path
    bool needsNewPath = false;
    
    if (HasPath()) {
        // We have a valid path - only repath if destination moved significantly
        float distSq = glm::distance2(oldDest, dest);
        needsNewPath = (distSq > kRepathThreshold * kRepathThreshold);
    } else {
        // No valid path. Avoid resetting retry/backoff every frame when scripts repeatedly
        // set the same destination; only reinitialize if destination moved meaningfully.
        if (!hadDestination) {
            needsNewPath = true;
        } else {
            float distSq = glm::distance2(oldDest, dest);
            needsNewPath = (distSq > kRepathThreshold * kRepathThreshold);
        }
    }
    
    if (needsNewPath) {
        CurrentPath.valid = false;
        CurrentPath.points.clear();
        PathCursor = 0;
        PathRequested = false; // Schedule new path on next update
        RepathTimer = 0.0f;
        PathFailCount = 0;
        PathRetryTimer = 0.0f;
    }
}

void NavAgentComponent::Stop()
{
    HasDestination = false;
    CurrentPath.points.clear();
    CurrentPath.valid = false;
    PathCursor = 0;
}

void NavAgentComponent::Warp(const glm::vec3& pos, ::TransformComponent* transform, ::Physics* physics, ::RigidBodyComponent* rb, ::ColliderComponent* collider)
{
    if (rb && !rb->BodyID.IsInvalid() && physics) {
        glm::vec3 bodyTargetPos = pos;
        glm::vec3 bodyTargetEuler(0.0f);

        if (transform) {
            glm::vec3 scale, skew, worldPos;
            glm::vec4 perspective;
            glm::quat worldRot(1.0f, 0.0f, 0.0f, 0.0f);

            if (glm::decompose(transform->WorldMatrix, scale, worldRot, worldPos, skew, perspective)) {
                bodyTargetEuler = glm::degrees(glm::eulerAngles(glm::normalize(worldRot)));
            } else {
                worldPos = glm::vec3(transform->WorldMatrix[3]);
                bodyTargetEuler = transform->Rotation;
            }

            glm::mat4 currentBodyWorld = physics->GetBodyTransform(rb->BodyID);
            if (collider) {
                const glm::quat localRot = transform->UseQuatRotation
                    ? transform->RotationQ
                    : glm::quat(glm::radians(transform->Rotation));
                const glm::vec3 authoredOffset = localRot * collider->Offset;
                if (glm::all(glm::isfinite(authoredOffset))) {
                    bodyTargetPos += authoredOffset;
                }
            } else if (currentBodyWorld != glm::mat4(0.0f)) {
                const glm::vec3 currentBodyPos = glm::vec3(currentBodyWorld[3]);
                const glm::vec3 currentBodyOffset = currentBodyPos - worldPos;
                if (glm::all(glm::isfinite(currentBodyOffset))) {
                    bodyTargetPos += currentBodyOffset;
                }
            }

            if (currentBodyWorld != glm::mat4(0.0f)) {
                glm::vec3 bodyScale, bodySkew, bodyPos;
                glm::vec4 bodyPerspective;
                glm::quat bodyRot(1.0f, 0.0f, 0.0f, 0.0f);
                if (glm::decompose(currentBodyWorld, bodyScale, bodyRot, bodyPos, bodySkew, bodyPerspective)) {
                    bodyTargetEuler = glm::degrees(glm::eulerAngles(glm::normalize(bodyRot)));
                }
            }
        }

        physics->SetBodyTransform(rb->BodyID, bodyTargetPos, bodyTargetEuler);
        physics->SetBodyLinearVelocity(rb->BodyID, glm::vec3(0.0f));
        physics->SetBodyAngularVelocity(rb->BodyID, glm::vec3(0.0f));
        rb->LinearVelocity = glm::vec3(0.0f);
        rb->AngularVelocity = glm::vec3(0.0f);
    }
    if (transform) {
        transform->Position = pos;
        transform->TransformDirty = true;
    }
    Stop();
}

float NavAgentComponent::RemainingDistance(const glm::vec3& currentPos) const
{
    if (!HasPath()) return 0.0f;
    float sum = 0.0f;
    glm::vec3 prev = currentPos;
    for (size_t i = PathCursor; i < CurrentPath.points.size(); ++i) {
        sum += glm::length(CurrentPath.points[i] - prev);
        prev = CurrentPath.points[i];
    }
    return sum;
}


