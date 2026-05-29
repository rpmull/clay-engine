// IKSystem.cpp
#include "core/animation/ik/IKSystem.h"
#include "core/ecs/Scene.h"
#include "core/ecs/EntityData.h"
#include "core/ecs/AnimationComponents.h"
#include "core/animation/ik/IKSolvers.h"
#include "core/animation/ik/IKDebugDraw.h"
#include "core/utils/Profiler.h"
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>
#include <array>
#include <cstdint>

namespace cm { namespace animation { namespace ik {

static inline void DecomposeTRS(const glm::mat4& m, glm::vec3& T, glm::quat& R, glm::vec3& S) {
    T = glm::vec3(m[3]);
    glm::vec3 X = glm::vec3(m[0]); glm::vec3 Y = glm::vec3(m[1]); glm::vec3 Z = glm::vec3(m[2]);
    S = glm::vec3(glm::length(X), glm::length(Y), glm::length(Z));
    if (S.x > 1e-6f) X /= S.x; if (S.y > 1e-6f) Y /= S.y; if (S.z > 1e-6f) Z /= S.z;
    glm::mat3 rotMat(X, Y, Z); R = glm::quat_cast(rotMat);
}

static inline glm::quat ApplyAxisLocks(const glm::quat& delta, const IKComponent& ikc) {
    if (!ikc.LockAxisX && !ikc.LockAxisY && !ikc.LockAxisZ) return delta;
    glm::vec3 euler = glm::eulerAngles(delta);
    if (ikc.LockAxisX) euler.x = 0.0f;
    if (ikc.LockAxisY) euler.y = 0.0f;
    if (ikc.LockAxisZ) euler.z = 0.0f;
    return glm::quat(euler);
}

void IKSystem::SolveAndBlend(Scene& scene, float /*deltaTime*/) {
    uint64_t activeSkeletons = 0;
    uint64_t activeConstraints = 0;
    uint64_t skippedOffscreenSkeletons = 0;
    uint64_t writtenBones = 0;

    // Iterate entities with Skeleton and potential IK authoring stored in Extra["ik"]
    for (const auto& ent : scene.GetEntities()) {
        auto* data = scene.GetEntityData(ent.GetID());
        if (!data || !data->Skeleton) continue;

        // Prefer native IK components; fall back to authored data preserved under Extra["ik"]
        std::vector<IKComponent>* ikListPtr = nullptr;
        // Attach a scratch vector in Extra runtime map if needed
        static thread_local std::vector<IKComponent> s_runtimeIK;
        s_runtimeIK.clear();

        if (!data->IKs.empty()) {
            ikListPtr = &data->IKs;
        }
        // If Extra carries authored IK blocks, materialize them into IKComponent instances
        else if (data->Extra.is_object() && data->Extra.contains("ik") && data->Extra["ik"].is_array()) {
            const auto& arr = data->Extra["ik"];
            for (const auto& j : arr) {
                IKComponent c;
                c.Enabled = j.value("enabled", true);
                c.TargetEntity = j.value("target", (EntityID)0);
                c.PoleEntity = j.value("pole", (EntityID)0);
                c.Weight = j.value("weight", 1.0f);
                c.MaxIterations = j.value("maxIterations", 12.0f);
                c.Tolerance = j.value("tolerance", 0.001f);
                c.Damping = j.value("damping", 0.2f);
                c.UseTwoBone = j.value("useTwoBone", true);
                c.Visualize = j.value("visualize", false);
            std::vector<BoneId> chainBones;
            if (j.contains("chain") && j["chain"].is_array()) {
                for (auto& b : j["chain"]) chainBones.push_back((BoneId)b.get<int>());
            }
            c.SetChain(chainBones);
            c.ChainRootHint = j.value("rootBone", c.ChainRootHint);
            c.ChainEffectorHint = j.value("tipBone", c.ChainEffectorHint);
                if (j.contains("constraints") && j["constraints"].is_array()) {
                    for (auto& cj : j["constraints"]) {
                        IKComponent::Constraint cc; cc.useHinge=cj.value("useHinge",false); cc.useTwist=cj.value("useTwist",false);
                        cc.hingeMinDeg=cj.value("hingeMinDeg",0.0f); cc.hingeMaxDeg=cj.value("hingeMaxDeg",0.0f);
                        cc.twistMinDeg=cj.value("twistMinDeg",0.0f); cc.twistMaxDeg=cj.value("twistMaxDeg",0.0f);
                        c.Constraints.push_back(cc);
                    }
                }
                c.Skeleton = data->Skeleton.get();
                s_runtimeIK.push_back(std::move(c));
            }
            ikListPtr = &s_runtimeIK;
        }

        if (!ikListPtr || ikListPtr->empty()) continue;

        auto& skeleton = *data->Skeleton;
        if (scene.m_IsPlaying && !skeleton.VisualWorkVisibleThisFrame) {
            ++skippedOffscreenSkeletons;
            continue;
        }
        const size_t boneCount = skeleton.BoneEntities.size();
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

        static thread_local std::vector<size_t> s_activeIkIndices;
        s_activeIkIndices.clear();
        s_activeIkIndices.reserve(ikListPtr->size());
        for (size_t ikIndex = 0; ikIndex < ikListPtr->size(); ++ikIndex) {
            IKComponent& ikc = (*ikListPtr)[ikIndex];
            ikc.Skeleton = data->Skeleton.get();
            if (!ikc.Enabled || ikc.Weight <= 0.0f) continue;
            if (ikc.TargetEntity == 0 || !scene.GetEntityData(ikc.TargetEntity)) continue;
            if (!ikc.ValidateChain(skeleton)) continue;
            const size_t chainSize = ikc.Chain.size();
            if (chainSize < 2 || chainSize > kMaxChainLen) continue;
            s_activeIkIndices.push_back(ikIndex);
        }
        if (s_activeIkIndices.empty()) continue;
        ++activeSkeletons;
        activeConstraints += static_cast<uint64_t>(s_activeIkIndices.size());

        // Build current local transforms buffer from bone entity TRS
        static thread_local std::vector<glm::mat4> s_local;
        static thread_local std::vector<glm::mat4> s_world;
        static thread_local std::vector<uint8_t> s_modifiedMask;
        static thread_local std::vector<int> s_modifiedBones;
        std::vector<glm::mat4>* localPtr = nullptr;
        if (useRuntimeLocalPoseTrs) {
            s_local.resize(boneCount);
            for (size_t i = 0; i < boneCount; ++i) {
                s_local[i] = skeleton.ComposeRuntimeLocalPoseMatrix(i);
            }
            localPtr = &s_local;
        } else if (useRuntimeLocalPose) {
            localPtr = &skeleton.RuntimeLocalPose;
        } else {
            localPtr = &s_local;
        }
        auto& local = *localPtr;
        auto& world = s_world;
        if (!useRuntimeLocalPose && !useRuntimeLocalPoseTrs) {
            local.resize(boneCount);
            for (size_t i=0;i<boneCount;++i) {
                EntityID be = skeleton.BoneEntities[i];
                if (auto* bd = scene.GetEntityData(be)) {
                    glm::mat4 T = glm::translate(glm::mat4(1.0f), bd->Transform.Position);
                    glm::mat4 R = glm::toMat4(glm::normalize(bd->Transform.RotationQ));
                    glm::mat4 S = glm::scale(glm::mat4(1.0f), bd->Transform.Scale);
                    local[i] = T * R * S;
                } else if (i < skeleton.InverseBindPoses.size()) {
                    // fall back to bind local from inverse bind
                    int parent = (i < skeleton.BoneParents.size()) ? skeleton.BoneParents[i] : -1;
                    glm::mat4 invBind = skeleton.InverseBindPoses[i];
                    glm::mat4 globalBind = glm::inverse(invBind);
                    glm::mat4 parentGlobal = (parent>=0)? glm::inverse(skeleton.InverseBindPoses[parent]) : glm::mat4(1.0f);
                    local[i] = glm::inverse(parentGlobal) * globalBind;
                } else {
                    local[i] = glm::mat4(1.0f);
                }
            }
        }

        // Compose world matrices for joints (model space = parent chain product)
        world.resize(boneCount);
        for (size_t i=0;i<boneCount;++i) {
            int p = (i < skeleton.BoneParents.size()) ? skeleton.BoneParents[i] : -1;
            world[i] = (p>=0) ? (world[p] * local[i]) : local[i];
        }
        s_modifiedMask.assign(boneCount, uint8_t{0});
        s_modifiedBones.clear();

        for (size_t ikIndex : s_activeIkIndices) {
            IKComponent& ikc = (*ikListPtr)[ikIndex];
            const size_t m = ikc.Chain.size(); if (m < 2 || m > kMaxChainLen) continue;

            // Resolve target & pole world positions
            glm::vec3 targetW(0.0f); bool targetValid = false;
            if (ikc.TargetEntity != 0) {
                if (auto* td = scene.GetEntityData(ikc.TargetEntity)) { targetW = glm::vec3(td->Transform.WorldMatrix[3]); targetValid = true; }
            }
            if (!targetValid) { continue; }
            bool hasPole = false; glm::vec3 poleW(0.0f);
            if (ikc.PoleEntity != 0) { if (auto* pd = scene.GetEntityData(ikc.PoleEntity)) { poleW = glm::vec3(pd->Transform.WorldMatrix[3]); hasPole = true; } }

            // Assemble joint world positions for chain
            std::array<glm::vec3, kMaxChainLen> jw{};
            for (size_t i=0;i<m;++i) jw[i] = glm::vec3(world[ikc.Chain[i]][3]);

            float outError = 0.0f; int outIter = 0;
            std::array<glm::quat, kMaxChainLen> desiredLocal{};
            for (size_t i = 0; i < m; ++i) {
                desiredLocal[i] = glm::quat(1,0,0,0);
            }

            if (ikc.UseTwoBone && m == 3) {
                TwoBoneInputs in{};
                in.rootPos = jw[0]; in.midPos = jw[1]; in.endPos = jw[2]; in.targetPos = targetW;
                in.hasPole = hasPole; in.polePos = poleW;
                in.upperLen = glm::length(jw[1] - jw[0]); in.lowerLen = glm::length(jw[2] - jw[1]);
                glm::quat r0, r1; float err;
                SolveTwoBone(in, nullptr, r0, r1, err);
                outError = err; outIter = 1;
                desiredLocal[0] = r0; desiredLocal[1] = r1; desiredLocal[2] = glm::quat(1,0,0,0);
            } else {
                std::array<glm::vec3, kMaxChainLen> jwSolved = jw;
                const glm::vec3* polePtr = hasPole ? &poleW : nullptr;
                SolveFABRIK(
                    jwSolved.data(),
                    static_cast<int>(m),
                    targetW,
                    static_cast<int>(ikc.MaxIterations),
                    ikc.Tolerance,
                    polePtr,
                    outError,
                    outIter);
                // Convert solved positions to local delta rotations
                std::array<glm::mat4, kMaxChainLen> parentW{};
                for (size_t i=0;i<m;++i) parentW[i] = world[ikc.Chain[i]];
                WorldChainToLocalRots(
                    parentW.data(),
                    jwSolved.data(),
                    static_cast<int>(m),
                    desiredLocal.data());
                jw = jwSolved;
            }

            // Damping/blend: apply R_out = slerp(I, Delta, Weight*(1-Damping)) then accumulate on FK local
            float damp = glm::clamp(ikc.Damping, 0.0f, 1.0f);
            float blend = glm::clamp(ikc.Weight * (1.0f - damp), 0.0f, 1.0f);

            ikc.RuntimeErrorMeters = outError;
            ikc.RuntimeIterations = outIter;

            for (size_t i=0;i<m;++i) {
                int bi = ikc.Chain[i];
                // Decompose current local transform, apply rotation delta
                glm::vec3 T;
                glm::vec3 S;
                glm::quat R;
                if (useRuntimeLocalPoseTrs) {
                    T = skeleton.RuntimeLocalTranslations[static_cast<size_t>(bi)];
                    R = skeleton.RuntimeLocalRotations[static_cast<size_t>(bi)];
                    S = skeleton.RuntimeLocalScales[static_cast<size_t>(bi)];
                } else {
                    DecomposeTRS(local[bi], T, R, S);
                }
                glm::quat delta = desiredLocal[i];
                glm::quat applied = glm::slerp(glm::quat(1,0,0,0), delta, blend);
                applied = ApplyAxisLocks(applied, ikc);
                glm::quat newR = glm::normalize(applied * R);
                local[bi] = glm::translate(T) * glm::mat4_cast(newR) * glm::scale(S);
                if (useRuntimeLocalPoseTrs) {
                    skeleton.RuntimeLocalTranslations[static_cast<size_t>(bi)] = T;
                    skeleton.RuntimeLocalRotations[static_cast<size_t>(bi)] = newR;
                    skeleton.RuntimeLocalScales[static_cast<size_t>(bi)] = S;
                }
                if (bi >= 0 && bi < static_cast<int>(boneCount) && s_modifiedMask[bi] == 0) {
                    s_modifiedMask[bi] = 1;
                    s_modifiedBones.push_back(bi);
                }
            }

            // Debug visualization on demand
            if (ikc.Visualize) {
                DebugChainViz viz;
                viz.jointWorld.assign(jw.data(), jw.data() + m);
                viz.targetWorld = targetW;
                viz.hasPole = hasPole;
                viz.poleWorld = poleW;
                viz.error = outError;
                viz.iterations = outIter;
                DrawChain(viz, 0);
            }
        }

        // Write back only bones touched by IK chains. Descendant world matrices
        // are still propagated by the transform system from these dirty anchors.
#ifndef CLAYMORE_RUNTIME
        const bool keepEulerInspectorSync = !scene.m_IsPlaying;
#else
        const bool keepEulerInspectorSync = false;
#endif
        if (useRuntimeLocalPoseTrs) {
            for (int boneIdx : s_modifiedBones) {
                if (boneIdx < 0 || boneIdx >= static_cast<int>(boneCount)) {
                    continue;
                }
                if (skeleton.RuntimeLocalPoseValid &&
                    skeleton.RuntimeLocalPose.size() >= boneCount) {
                    skeleton.RuntimeLocalPose[static_cast<size_t>(boneIdx)] =
                        local[static_cast<size_t>(boneIdx)];
                }
            }

            if (!s_modifiedBones.empty()) {
                const uint32_t poseFrameId = skeleton.RuntimeLocalPoseFrameId != 0
                    ? skeleton.RuntimeLocalPoseFrameId
                    : skeleton.PoseFrameId;
                skeleton.RebuildRuntimeBonePaletteFromLocalPoseTrs(poseFrameId);
                scene.NotifyAnimationPosePaletteChanged();
            }

            for (int boneIdx : s_modifiedBones) {
                if (boneIdx < 0 || boneIdx >= static_cast<int>(boneCount)) continue;
                EntityID be = skeleton.BoneEntities[boneIdx];
                if (be == (EntityID)-1) continue;
                if (auto* bd = scene.GetEntityData(be)) {
                    bd->Transform.Position =
                        skeleton.RuntimeLocalTranslations[static_cast<size_t>(boneIdx)];
                    bd->Transform.Scale =
                        skeleton.RuntimeLocalScales[static_cast<size_t>(boneIdx)];
                    bd->Transform.RotationQ = glm::normalize(
                        skeleton.RuntimeLocalRotations[static_cast<size_t>(boneIdx)]);
                    bd->Transform.UseQuatRotation = true;
                    if (keepEulerInspectorSync) {
                        bd->Transform.Rotation = glm::degrees(glm::eulerAngles(bd->Transform.RotationQ));
                    }
                    scene.MarkTransformDirty(be);
                }
            }
        } else if (useRuntimeLocalPose) {
            for (int boneIdx : s_modifiedBones) {
                if (boneIdx < 0 || boneIdx >= static_cast<int>(boneCount)) {
                    continue;
                }
                skeleton.RuntimeLocalPose[static_cast<size_t>(boneIdx)] =
                    local[static_cast<size_t>(boneIdx)];
            }

            if (!s_modifiedBones.empty()) {
                const uint32_t poseFrameId = skeleton.RuntimeLocalPoseFrameId != 0
                    ? skeleton.RuntimeLocalPoseFrameId
                    : skeleton.PoseFrameId;
                skeleton.RebuildRuntimeBonePaletteFromLocalPose(poseFrameId);
                scene.NotifyAnimationPosePaletteChanged();
            }

            // Mirror touched IK bones back to entity transforms so managed
            // systems that query skeleton nodes observe the solved pose.
            for (int boneIdx : s_modifiedBones) {
                if (boneIdx < 0 || boneIdx >= static_cast<int>(boneCount)) continue;
                EntityID be = skeleton.BoneEntities[boneIdx];
                if (be == (EntityID)-1) continue;
                if (auto* bd = scene.GetEntityData(be)) {
                    glm::vec3 T, S; glm::quat R;
                    DecomposeTRS(local[boneIdx], T, R, S);
                    bd->Transform.Position = T;
                    bd->Transform.Scale = S;
                    bd->Transform.RotationQ = glm::normalize(R);
                    bd->Transform.UseQuatRotation = true;
                    if (keepEulerInspectorSync) {
                        bd->Transform.Rotation = glm::degrees(glm::eulerAngles(bd->Transform.RotationQ));
                    }
                    scene.MarkTransformDirty(be);
                }
            }
        } else {
            for (int boneIdx : s_modifiedBones) {
                if (boneIdx < 0 || boneIdx >= static_cast<int>(boneCount)) continue;
                EntityID be = skeleton.BoneEntities[boneIdx]; if (be == (EntityID)-1) continue;
                if (auto* bd = scene.GetEntityData(be)) {
                    glm::vec3 T,S; glm::quat R; DecomposeTRS(local[boneIdx], T, R, S);
                    bd->Transform.Position = T;
                    bd->Transform.Scale = S;
                    bd->Transform.RotationQ = glm::normalize(R);
                    bd->Transform.UseQuatRotation = true;
                    if (keepEulerInspectorSync) {
                        bd->Transform.Rotation = glm::degrees(glm::eulerAngles(bd->Transform.RotationQ));
                    }
                    scene.MarkTransformDirty(be);
                }
            }
        }
        writtenBones += static_cast<uint64_t>(s_modifiedBones.size());
    }

    auto& profiler = Profiler::Get();
    profiler.SetCounter("IK/ActiveSkeletons", activeSkeletons);
    profiler.SetCounter("IK/ActiveConstraints", activeConstraints);
    profiler.SetCounter("IK/SkippedOffscreenSkeletons", skippedOffscreenSkeletons);
    profiler.SetCounter("IK/WrittenBones", writtenBones);
}

} } }


