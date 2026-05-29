// IKComponent.h
#pragma once

#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "core/animation/ik/IKTypes.h"
#include <algorithm>

struct SkeletonComponent;

namespace cm { namespace animation { namespace ik {

struct IKComponent {
    // Authoring
    bool Enabled = true;
    EntityID TargetEntity = 0;
    EntityID PoleEntity = 0; // optional
    
    // GUID storage for stable entity references across saves/prefab instantiation
    // These are populated during serialization and used during deserialization to resolve targets
    uint64_t TargetEntityGuidHigh = 0;
    uint64_t TargetEntityGuidLow = 0;
    uint64_t PoleEntityGuidHigh = 0;
    uint64_t PoleEntityGuidLow = 0;
    
    std::vector<BoneId> Chain; // ordered root..effector
    float Weight = 1.0f;
    float MaxIterations = 12.0f;
    float Tolerance = 0.001f;
    float Damping = 0.2f; // 0..1
    bool UseTwoBone = true;
    bool LockAxisX = false; // lock local X rotation delta
    bool LockAxisY = false; // lock local Y rotation delta
    bool LockAxisZ = false; // lock local Z rotation delta
    BoneId ChainRootHint = -1;    // authoring selection
    BoneId ChainEffectorHint = -1;

    struct Constraint { float twistMinDeg=0, twistMaxDeg=0; float hingeMinDeg=0, hingeMaxDeg=0; bool useTwist=false, useHinge=false; };
    std::vector<Constraint> Constraints; // per joint (size = Chain.size()-1)
    bool Visualize = false;

    // Runtime/cached
    glm::vec3 LastSolvedEffectorPos{0.0f};
    glm::quat LastSolvedBoneRots[kMaxChainLen];
    bool WasValidLastFrame = false;
    SkeletonComponent* Skeleton = nullptr; // not owned
    uint64_t ManagedHandle = 0;
    float RuntimeErrorMeters = 0.0f;
    int   RuntimeIterations = 0;

    // Methods
    bool ValidateChain(const SkeletonComponent& skeleton) const;
    void SetWeight(float w) { Weight = glm::clamp(w, 0.0f, 1.0f); }
    void SetTarget(EntityID e) { TargetEntity = e; }
    void SetPole(EntityID e) { PoleEntity = e; }
    void SetChain(const std::vector<BoneId>& ids);
    void SetChain(const BoneId* ids, size_t count);
};

} } }


