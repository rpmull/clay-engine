// LookAtConstraintSystem.cpp
#include "LookAtConstraintSystem.h"
#include "core/ecs/Scene.h"
#include "core/ecs/EntityData.h"
#include "core/ecs/AnimationComponents.h"
#include "core/utils/Profiler.h"
#include <algorithm>
#include <functional>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <cmath>

namespace cm { namespace animation { namespace lookat {

//------------------------------------------------------------------------------
// Helper: Decompose TRS from matrix
//------------------------------------------------------------------------------
static inline void DecomposeTRS(const glm::mat4& m, glm::vec3& T, glm::quat& R, glm::vec3& S) {
    T = glm::vec3(m[3]);
    glm::vec3 X = glm::vec3(m[0]);
    glm::vec3 Y = glm::vec3(m[1]);
    glm::vec3 Z = glm::vec3(m[2]);
    S = glm::vec3(glm::length(X), glm::length(Y), glm::length(Z));
    if (S.x > 1e-6f) X /= S.x;
    if (S.y > 1e-6f) Y /= S.y;
    if (S.z > 1e-6f) Z /= S.z;
    glm::mat3 rotMat(X, Y, Z);
    R = glm::quat_cast(rotMat);
}

//------------------------------------------------------------------------------
// Helper: Compose TRS into matrix
//------------------------------------------------------------------------------
static inline glm::mat4 ComposeTRS(const glm::vec3& T, const glm::quat& R, const glm::vec3& S) {
    return glm::translate(glm::mat4(1.0f), T) 
         * glm::mat4_cast(glm::normalize(R)) 
         * glm::scale(glm::mat4(1.0f), S);
}

//------------------------------------------------------------------------------
// Helper: Calculate yaw/pitch from direction vector
// Returns: x = pitch (up/down), y = yaw (left/right), z = 0 (no roll from direction)
// 
// Uses +Z as forward reference (matching model/skeleton convention).
// yaw=0 means facing +Z, positive yaw rotates toward +X.
//------------------------------------------------------------------------------
static inline glm::vec3 DirectionToYawPitch(const glm::vec3& dir) {
    if (glm::length(dir) < 1e-6f) {
        return glm::vec3(0.0f);
    }
    
    glm::vec3 d = glm::normalize(dir);
    
    // Yaw: rotation around Y axis
    // +Z forward: yaw=0 when facing +Z, yaw=90° when facing +X
    float yaw = atan2f(d.x, d.z);
    
    // Pitch: rotation around X axis (asin of Y component)
    float pitch = asinf(glm::clamp(d.y, -1.0f, 1.0f));
    
    return glm::vec3(pitch, yaw, 0.0f);
}

//------------------------------------------------------------------------------
// Helper: Exponential smoothing
//------------------------------------------------------------------------------
static inline float ExpSmooth(float current, float target, float speed, float dt) {
    if (speed <= 0.0f) return target;
    float t = 1.0f - expf(-speed * dt);
    return glm::mix(current, target, t);
}

//------------------------------------------------------------------------------
// Helper: Clamp angle to range (degrees)
//------------------------------------------------------------------------------
static inline float ClampAngle(float angleDeg, float maxDeg) {
    return glm::clamp(angleDeg, -maxDeg, maxDeg);
}

//------------------------------------------------------------------------------
// Helper: Get forward direction from rotation quaternion
// 
// Extracts the -Z direction after rotation (camera/OpenGL convention).
// This is used for MatchRotation mode where the target is always a camera.
// Cameras use -Z as forward (Transform.forward), so we extract that direction.
//------------------------------------------------------------------------------
static inline glm::vec3 GetCameraForwardFromQuat(const glm::quat& q) {
    // Cameras use -Z as forward (OpenGL convention)
    return glm::normalize(q * glm::vec3(0.0f, 0.0f, -1.0f));
}

//------------------------------------------------------------------------------
// LookAtConstraintSystem::Apply
//------------------------------------------------------------------------------
void LookAtConstraintSystem::Apply(Scene& scene, float deltaTime) {
    uint64_t activeSkeletons = 0;
    uint64_t skippedOffscreenSkeletons = 0;

    // Iterate entities with Skeleton and LookAtConstraints
    for (const auto& ent : scene.GetEntities()) {
        auto* data = scene.GetEntityData(ent.GetID());
        if (!data || !data->Skeleton) continue;
        if (data->LookAtConstraints.empty()) continue;

        bool hasActiveConstraint = false;
        for (const auto& constraint : data->LookAtConstraints) {
            if (constraint.Enabled &&
                constraint.Weight > 0.0f &&
                !constraint.BoneChain.empty() &&
                constraint.TargetEntity != 0) {
                hasActiveConstraint = true;
                break;
            }
        }
        if (!hasActiveConstraint) continue;
        
        auto& skeleton = *data->Skeleton;
        if (scene.m_IsPlaying && !skeleton.VisualWorkVisibleThisFrame) {
            for (auto& constraint : data->LookAtConstraints) {
                constraint.WasValidLastFrame = false;
            }
            ++skippedOffscreenSkeletons;
            continue;
        }
        ++activeSkeletons;

        const size_t boneCount = skeleton.BoneEntities.size();
        if (boneCount == 0) continue;
        const bool useRuntimeLocalPoseTrs =
            scene.m_IsPlaying &&
            skeleton.RuntimeLocalPoseTrsValid &&
            skeleton.RuntimeLocalTranslations.size() >= boneCount &&
            skeleton.RuntimeLocalRotations.size() >= boneCount &&
            skeleton.RuntimeLocalScales.size() >= boneCount;
        const bool useRuntimeLocalPose =
            scene.m_IsPlaying &&
            (useRuntimeLocalPoseTrs ||
             (skeleton.RuntimeLocalPoseValid &&
              skeleton.RuntimeLocalPose.size() >= boneCount));
        
        // Get skeleton root's world transform for component space calculations
        glm::mat4 skeletonWorldMatrix = data->Transform.WorldMatrix;
        glm::mat4 invSkeletonWorld = glm::inverse(skeletonWorldMatrix);

        // Lazily build local/world transform caches for just the bones touched by
        // active constraints instead of rebuilding the entire skeleton each frame.
        m_LocalTransforms.resize(boneCount);
        m_WorldTransforms.resize(boneCount);
        m_LocalLoadedStamp.resize(boneCount, 0u);
        m_WorldValidStamp.resize(boneCount, 0u);
        m_ModifiedStamp.resize(boneCount, 0u);
        m_ModifiedBones.clear();

        auto advanceLocalStamp = [&]() {
            ++m_LocalScratchStamp;
            if (m_LocalScratchStamp == 0u) {
                std::fill(m_LocalLoadedStamp.begin(), m_LocalLoadedStamp.end(), 0u);
                std::fill(m_ModifiedStamp.begin(), m_ModifiedStamp.end(), 0u);
                m_LocalScratchStamp = 1u;
            }
        };
        auto advanceWorldStamp = [&]() {
            ++m_WorldScratchStamp;
            if (m_WorldScratchStamp == 0u) {
                std::fill(m_WorldValidStamp.begin(), m_WorldValidStamp.end(), 0u);
                m_WorldScratchStamp = 1u;
            }
        };
        advanceLocalStamp();
        advanceWorldStamp();

        auto loadLocalTransform = [&](int boneIdx) -> const glm::mat4& {
            if (boneIdx < 0 || boneIdx >= static_cast<int>(boneCount)) {
                static glm::mat4 s_identity(1.0f);
                return s_identity;
            }
            if (m_LocalLoadedStamp[boneIdx] == m_LocalScratchStamp) {
                return m_LocalTransforms[boneIdx];
            }

            if (useRuntimeLocalPoseTrs) {
                m_LocalTransforms[boneIdx] =
                    skeleton.ComposeRuntimeLocalPoseMatrix(static_cast<size_t>(boneIdx));
            } else if (useRuntimeLocalPose) {
                m_LocalTransforms[boneIdx] = skeleton.RuntimeLocalPose[static_cast<size_t>(boneIdx)];
            } else {
                EntityID boneEntity = skeleton.BoneEntities[boneIdx];
                if (auto* boneData = scene.GetEntityData(boneEntity)) {
                    glm::mat4 T = glm::translate(glm::mat4(1.0f), boneData->Transform.Position);
                    glm::mat4 R = glm::toMat4(glm::normalize(boneData->Transform.RotationQ));
                    glm::mat4 S = glm::scale(glm::mat4(1.0f), boneData->Transform.Scale);
                    m_LocalTransforms[boneIdx] = T * R * S;
                } else {
                    m_LocalTransforms[boneIdx] = glm::mat4(1.0f);
                }
            }

            m_LocalLoadedStamp[boneIdx] = m_LocalScratchStamp;
            return m_LocalTransforms[boneIdx];
        };

        auto getWorldTransform = [&](auto&& self, int boneIdx) -> const glm::mat4& {
            if (boneIdx < 0 || boneIdx >= static_cast<int>(boneCount)) {
                static glm::mat4 s_identity(1.0f);
                return s_identity;
            }
            if (m_WorldValidStamp[boneIdx] == m_WorldScratchStamp) {
                return m_WorldTransforms[boneIdx];
            }

            const glm::mat4& local = loadLocalTransform(boneIdx);
            int parentIdx = (boneIdx < static_cast<int>(skeleton.BoneParents.size())) ? skeleton.BoneParents[boneIdx] : -1;
            if (parentIdx >= 0 && parentIdx < static_cast<int>(boneCount)) {
                m_WorldTransforms[boneIdx] = self(self, parentIdx) * local;
            } else {
                m_WorldTransforms[boneIdx] = local;
            }

            m_WorldValidStamp[boneIdx] = m_WorldScratchStamp;
            return m_WorldTransforms[boneIdx];
        };

        auto markBoneModified = [&](int boneIdx) {
            if (boneIdx < 0 || boneIdx >= static_cast<int>(boneCount)) {
                return;
            }
            if (m_ModifiedStamp[boneIdx] != m_LocalScratchStamp) {
                m_ModifiedStamp[boneIdx] = m_LocalScratchStamp;
                m_ModifiedBones.push_back(boneIdx);
            }
        };
        
        // Process each LookAtConstraint
        for (auto& constraint : data->LookAtConstraints) {
            if (!constraint.Enabled || constraint.Weight <= 0.0f) continue;
            if (constraint.BoneChain.empty()) continue;
            
            // Get target entity data
            EntityData* targetData = nullptr;
            if (constraint.TargetEntity != 0) {
                targetData = scene.GetEntityData(constraint.TargetEntity);
            }
            
            if (!targetData) {
                constraint.WasValidLastFrame = false;
                continue;
            }
            
            // Get root bone for reference
            BoneId rootBoneId = constraint.BoneChain[0];
            if (rootBoneId < 0 || rootBoneId >= (int)boneCount) continue;
            
            float targetYaw = 0.0f;
            float targetPitch = 0.0f;
            float targetRoll = 0.0f;
            
            if (constraint.Mode == LookAtMode::MatchRotation) {
                //--------------------------------------------------------------
                // MatchRotation mode: Match the camera's facing direction
                // This is designed for FPS cameras where we want the spine to
                // rotate with the camera's facing direction.
                // 
                // Convention handling:
                // 1. Camera forward: -Z (OpenGL convention)
                // 2. Skeleton forward: +Z (model convention)  
                // 3. Pitch sign: negated because GLM's angleAxis(+angle, X) 
                //    rotates +Z toward -Y (down), but we want +pitch = look up
                //--------------------------------------------------------------
                
                // Get target's world rotation from its world matrix
                glm::mat4 targetWorldMat = targetData->Transform.WorldMatrix;
                glm::vec3 targetPos, targetScale;
                glm::quat targetRot;
                DecomposeTRS(targetWorldMat, targetPos, targetRot, targetScale);
                
                // Get the camera's forward direction in world space
                // Cameras use -Z as forward (OpenGL/Transform.forward convention)
                glm::vec3 targetForwardWorld = GetCameraForwardFromQuat(targetRot);
                
                // Convert direction to yaw/pitch based on space
                glm::vec3 lookAngles(0.0f);
                
                switch (constraint.Space) {
                    case LookAtSpace::World: {
                        // Direct world-space angles from the forward direction
                        lookAngles = DirectionToYawPitch(targetForwardWorld);
                    } break;
                    
                    case LookAtSpace::Component: {
                        // Transform forward direction to skeleton's local space
                        // This gives us angles relative to the skeleton's orientation
                        glm::vec3 forwardInSkeleton = glm::vec3(invSkeletonWorld * glm::vec4(targetForwardWorld, 0.0f));
                        lookAngles = DirectionToYawPitch(forwardInSkeleton);
                    } break;
                    
                    case LookAtSpace::Local: {
                        // Transform to root bone's local space
                        glm::mat4 invRootWorld =
                            glm::inverse(skeletonWorldMatrix * getWorldTransform(getWorldTransform, rootBoneId));
                        glm::vec3 forwardInBone = glm::vec3(invRootWorld * glm::vec4(targetForwardWorld, 0.0f));
                        lookAngles = DirectionToYawPitch(forwardInBone);
                    } break;
                }
                
                targetYaw = glm::degrees(lookAngles.y);
                // Negate pitch: DirectionToYawPitch gives +pitch for "pointing up" direction,
                // but GLM's angleAxis(+angle, X) rotates bones to look DOWN.
                targetPitch = -glm::degrees(lookAngles.x);
                targetRoll = 0.0f; // Roll not derived from forward direction
                
            } else {
                //--------------------------------------------------------------
                // LookAtPosition mode: Rotate to face target's world position
                // This is ideal for third-person cameras, NPCs, etc.
                //--------------------------------------------------------------
                
                glm::vec3 targetWorld = glm::vec3(targetData->Transform.WorldMatrix[3]);
                
                // Get root bone world position
                glm::vec3 rootWorldPos =
                    glm::vec3(skeletonWorldMatrix * getWorldTransform(getWorldTransform, rootBoneId)[3]);
                
                // Direction from root bone to target
                glm::vec3 lookDirWorld = targetWorld - rootWorldPos;
                if (glm::length(lookDirWorld) < 1e-4f) {
                    continue; // Target too close
                }
                lookDirWorld = glm::normalize(lookDirWorld);
                
                // Convert direction to yaw/pitch based on space
                glm::vec3 lookAngles(0.0f);
                
                switch (constraint.Space) {
                    case LookAtSpace::World: {
                        // Direct world-space angles
                        lookAngles = DirectionToYawPitch(lookDirWorld);
                    } break;
                    
                    case LookAtSpace::Component: {
                        // Transform direction to skeleton's local space
                        glm::vec3 lookDirLocal = glm::vec3(invSkeletonWorld * glm::vec4(lookDirWorld, 0.0f));
                        lookAngles = DirectionToYawPitch(lookDirLocal);
                    } break;
                    
                    case LookAtSpace::Local: {
                        // Transform to root bone's local space
                        glm::mat4 invRootWorld =
                            glm::inverse(skeletonWorldMatrix * getWorldTransform(getWorldTransform, rootBoneId));
                        glm::vec3 lookDirBone = glm::vec3(invRootWorld * glm::vec4(lookDirWorld, 0.0f));
                        lookAngles = DirectionToYawPitch(lookDirBone);
                    } break;
                }
                
                targetYaw = glm::degrees(lookAngles.y);
                targetPitch = glm::degrees(lookAngles.x);
                targetRoll = 0.0f; // Roll not derived from look direction
            }
            
            // Apply limits
            targetYaw = ClampAngle(targetYaw, constraint.MaxYawDeg);
            targetPitch = ClampAngle(targetPitch, constraint.MaxPitchDeg);
            targetRoll = ClampAngle(targetRoll, constraint.MaxRollDeg);
            
            // Apply smoothing
            if (!constraint.WasValidLastFrame) {
                // First valid frame - snap to avoid lerp from zero
                constraint.SmoothedYaw = targetYaw;
                constraint.SmoothedPitch = targetPitch;
                constraint.SmoothedRoll = targetRoll;
            } else {
                constraint.SmoothedYaw = ExpSmooth(constraint.SmoothedYaw, targetYaw, constraint.SmoothingSpeed, deltaTime);
                constraint.SmoothedPitch = ExpSmooth(constraint.SmoothedPitch, targetPitch, constraint.SmoothingSpeed, deltaTime);
                constraint.SmoothedRoll = ExpSmooth(constraint.SmoothedRoll, targetRoll, constraint.SmoothingSpeed, deltaTime);
            }
            constraint.WasValidLastFrame = true;
            
            const size_t modifiedBeforeConstraint = m_ModifiedBones.size();

            // Apply axis mask
            float yawRad = HasFlag(constraint.Axes, AxisMask::Yaw) ? glm::radians(constraint.SmoothedYaw) : 0.0f;
            float pitchRad = HasFlag(constraint.Axes, AxisMask::Pitch) ? glm::radians(constraint.SmoothedPitch) : 0.0f;
            float rollRad = HasFlag(constraint.Axes, AxisMask::Roll) ? glm::radians(constraint.SmoothedRoll) : 0.0f;
            
            // Distribute rotation across bone chain
            for (size_t i = 0; i < constraint.BoneChain.size(); ++i) {
                BoneId boneId = constraint.BoneChain[i];
                if (boneId < 0 || boneId >= (int)boneCount) continue;
                
                // Get weight for this bone
                float boneWeight = constraint.GetBoneWeight(i) * constraint.Weight;
                if (boneWeight < 1e-6f) continue;
                
                // Calculate rotation delta for this bone
                float boneYaw = yawRad * boneWeight;
                float bonePitch = pitchRad * boneWeight;
                float boneRoll = rollRad * boneWeight;
                
                // Create rotation quaternion using explicit axis-angle rotations
                // This avoids euler angle coupling issues that can cause unwanted roll
                // Order: Yaw (Y) -> Pitch (X) -> Roll (Z)
                glm::quat yawQ = glm::angleAxis(boneYaw, glm::vec3(0.0f, 1.0f, 0.0f));
                glm::quat pitchQ = glm::angleAxis(bonePitch, glm::vec3(1.0f, 0.0f, 0.0f));
                glm::quat rollQ = glm::angleAxis(boneRoll, glm::vec3(0.0f, 0.0f, 1.0f));
                glm::quat deltaRot = yawQ * pitchQ * rollQ;
                
                // Get current local transform
                glm::vec3 T;
                glm::vec3 S;
                glm::quat R;
                if (useRuntimeLocalPoseTrs) {
                    T = skeleton.RuntimeLocalTranslations[static_cast<size_t>(boneId)];
                    R = skeleton.RuntimeLocalRotations[static_cast<size_t>(boneId)];
                    S = skeleton.RuntimeLocalScales[static_cast<size_t>(boneId)];
                } else {
                    DecomposeTRS(loadLocalTransform(boneId), T, R, S);
                }
                
                // Apply rotation additively
                // The rotation is applied in the bone's local space
                glm::quat newR = glm::normalize(deltaRot * R);
                
                // Recompose and store
                m_LocalTransforms[boneId] = ComposeTRS(T, newR, S);
                if (useRuntimeLocalPoseTrs) {
                    skeleton.RuntimeLocalTranslations[static_cast<size_t>(boneId)] = T;
                    skeleton.RuntimeLocalRotations[static_cast<size_t>(boneId)] = newR;
                    skeleton.RuntimeLocalScales[static_cast<size_t>(boneId)] = S;
                }
                m_LocalLoadedStamp[boneId] = m_LocalScratchStamp;
                markBoneModified(boneId);
            }

            if (m_ModifiedBones.size() != modifiedBeforeConstraint) {
                advanceWorldStamp();
            }
        }
        
        // Write back only the bones that were actually modified by the active chains.
#ifndef CLAYMORE_RUNTIME
        const bool keepEulerInspectorSync = !scene.m_IsPlaying;
#else
        const bool keepEulerInspectorSync = false;
#endif
        if (useRuntimeLocalPoseTrs) {
            for (int boneIdx : m_ModifiedBones) {
                if (boneIdx < 0 || boneIdx >= static_cast<int>(boneCount)) {
                    continue;
                }
                if (skeleton.RuntimeLocalPoseValid &&
                    skeleton.RuntimeLocalPose.size() >= boneCount) {
                    skeleton.RuntimeLocalPose[static_cast<size_t>(boneIdx)] =
                        m_LocalTransforms[boneIdx];
                }
            }
            if (!m_ModifiedBones.empty()) {
                const uint32_t poseFrameId = skeleton.RuntimeLocalPoseFrameId != 0
                    ? skeleton.RuntimeLocalPoseFrameId
                    : skeleton.PoseFrameId;
                skeleton.RebuildRuntimeBonePaletteFromLocalPoseTrs(poseFrameId);
                scene.NotifyAnimationPosePaletteChanged();
            }

            for (int boneIdx : m_ModifiedBones) {
                if (boneIdx < 0 || boneIdx >= static_cast<int>(boneCount)) {
                    continue;
                }

                EntityID boneEntity = skeleton.BoneEntities[boneIdx];
                if (boneEntity == (EntityID)-1) {
                    continue;
                }

                if (auto* boneData = scene.GetEntityData(boneEntity)) {
                    boneData->Transform.Position =
                        skeleton.RuntimeLocalTranslations[static_cast<size_t>(boneIdx)];
                    boneData->Transform.Scale =
                        skeleton.RuntimeLocalScales[static_cast<size_t>(boneIdx)];
                    boneData->Transform.RotationQ = glm::normalize(
                        skeleton.RuntimeLocalRotations[static_cast<size_t>(boneIdx)]);
                    boneData->Transform.UseQuatRotation = true;
                    if (keepEulerInspectorSync) {
                        boneData->Transform.Rotation = glm::degrees(
                            glm::eulerAngles(boneData->Transform.RotationQ));
                    }
                    scene.MarkTransformDirty(boneEntity);
                }
            }
        } else if (useRuntimeLocalPose) {
            for (int boneIdx : m_ModifiedBones) {
                if (boneIdx < 0 || boneIdx >= static_cast<int>(boneCount)) {
                    continue;
                }
                skeleton.RuntimeLocalPose[static_cast<size_t>(boneIdx)] =
                    m_LocalTransforms[boneIdx];
            }
            if (!m_ModifiedBones.empty()) {
                const uint32_t poseFrameId = skeleton.RuntimeLocalPoseFrameId != 0
                    ? skeleton.RuntimeLocalPoseFrameId
                    : skeleton.PoseFrameId;
                skeleton.RebuildRuntimeBonePaletteFromLocalPose(poseFrameId);
                scene.NotifyAnimationPosePaletteChanged();
            }

            // Keep script-side bone consumers in sync (camera anchors, head-track
            // queries) without re-materializing full skeletons each frame.
            for (int boneIdx : m_ModifiedBones) {
                if (boneIdx < 0 || boneIdx >= static_cast<int>(boneCount)) {
                    continue;
                }

                EntityID boneEntity = skeleton.BoneEntities[boneIdx];
                if (boneEntity == (EntityID)-1) {
                    continue;
                }

                if (auto* boneData = scene.GetEntityData(boneEntity)) {
                    glm::vec3 T, S;
                    glm::quat R;
                    DecomposeTRS(m_LocalTransforms[boneIdx], T, R, S);

                    boneData->Transform.Position = T;
                    boneData->Transform.Scale = S;
                    boneData->Transform.RotationQ = glm::normalize(R);
                    boneData->Transform.UseQuatRotation = true;
                    if (keepEulerInspectorSync) {
                        boneData->Transform.Rotation = glm::degrees(glm::eulerAngles(boneData->Transform.RotationQ));
                    }
                    scene.MarkTransformDirty(boneEntity);
                }
            }
        } else {
            for (int boneIdx : m_ModifiedBones) {
                if (boneIdx < 0 || boneIdx >= static_cast<int>(boneCount)) {
                    continue;
                }

                EntityID boneEntity = skeleton.BoneEntities[boneIdx];
                if (boneEntity == (EntityID)-1) continue;
                
                if (auto* boneData = scene.GetEntityData(boneEntity)) {
                    glm::vec3 T, S;
                    glm::quat R;
                    DecomposeTRS(m_LocalTransforms[boneIdx], T, R, S);

                    boneData->Transform.Position = T;
                    boneData->Transform.Scale = S;
                    boneData->Transform.RotationQ = glm::normalize(R);
                    boneData->Transform.UseQuatRotation = true;
                    if (keepEulerInspectorSync) {
                        boneData->Transform.Rotation = glm::degrees(glm::eulerAngles(boneData->Transform.RotationQ));
                    }
                    scene.MarkTransformDirty(boneEntity);
                }
            }
        }
    }

    auto& profiler = Profiler::Get();
    profiler.SetCounter("LookAt/ActiveSkeletons", activeSkeletons);
    profiler.SetCounter("LookAt/SkippedOffscreenSkeletons", skippedOffscreenSkeletons);
}

} } } // namespace cm::animation::lookat
