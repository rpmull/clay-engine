#include "RagdollSystem.h"
#include "core/physics/Physics.h"
#include "core/ecs/Scene.h"
#include "core/ecs/EntityData.h"
#include "core/ecs/AnimationComponents.h"
#include "core/jobs/ParallelFor.h"
#include "core/jobs/Jobs.h"
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <iostream>
#include <atomic>
#include <limits>

namespace cm::physics {

static RagdollSystem* g_RagdollSystem = nullptr;

RagdollSystem* GetRagdollSystem() { return g_RagdollSystem; }

void InitRagdollSystem(JPH::PhysicsSystem* phys, JPH::BodyInterface* bi) {
    if (!g_RagdollSystem) {
        g_RagdollSystem = new RagdollSystem(phys, bi);
    }
}

void ShutdownRagdollSystem() {
    delete g_RagdollSystem;
    g_RagdollSystem = nullptr;
}

RagdollSystem::RagdollSystem(JPH::PhysicsSystem* phys, JPH::BodyInterface* bi)
    : m_Physics(phys), m_BodyInterface(bi) {}

RagdollSystem::~RagdollSystem() {
    // Destroy all active ragdolls
    std::lock_guard<std::mutex> lock(m_Mutex);
    for (auto& [entityId, ragdoll] : m_Ragdolls) {
        // Remove joints first
        for (auto& joint : ragdoll.Joints) {
            if (joint.Constraint) {
                m_Physics->RemoveConstraint(joint.Constraint);
            }
        }
        // Then remove bodies
        for (auto& bone : ragdoll.Bones) {
            if (!bone.BodyID.IsInvalid()) {
                m_BodyInterface->RemoveBody(bone.BodyID);
                m_BodyInterface->DestroyBody(bone.BodyID);
            }
        }
    }
    m_Ragdolls.clear();
    m_SkeletonToOwner.clear();
    m_ActiveSkeletons.clear();
    m_ActiveSkeletonBounds.clear();
}

EntityID RagdollSystem::FindSkeletonEntity(EntityID rootEntity) const {
    Scene& scene = Scene::Get();
    auto* data = scene.GetEntityData(rootEntity);
    if (!data) return INVALID_ENTITY_ID;
    
    // Check if this entity has a skeleton
    if (data->Skeleton) return rootEntity;
    
    // Search children recursively
    for (EntityID childId : data->Children) {
        EntityID found = FindSkeletonEntity(childId);
        if (found != INVALID_ENTITY_ID) return found;
    }
    
    return INVALID_ENTITY_ID;
}

static glm::vec3 ExtractPosition(const glm::mat4& m) {
    return glm::vec3(m[3]);
}

static glm::quat ExtractRotation(const glm::mat4& m) {
    glm::vec3 X = glm::normalize(glm::vec3(m[0]));
    glm::vec3 Y = glm::normalize(glm::vec3(m[1]));
    glm::vec3 Z = glm::normalize(glm::vec3(m[2]));
    return glm::quat_cast(glm::mat3(X, Y, Z));
}

RagdollBone RagdollSystem::CreateBoneBody(EntityID boneEntity, const std::string& boneName,
                                           int boneIndex, int parentIndex, const glm::mat4& worldTransform,
                                           uint32_t physicsLayer) {
    RagdollBone bone;
    bone.BoneIndex = boneIndex;
    bone.ParentBoneIndex = parentIndex;
    
    // Get bone size hints based on name
    // NOTE: Don't scale by transform scale - the scale compensates for model import scale,
    // the visual character is already human-sized in world space
    BoneSizeHint sizeHint = GetHumanoidBoneSize(boneName);
    bone.Radius = sizeHint.Radius;
    bone.HalfHeight = sizeHint.Length * 0.5f;
    
    std::cout << "[RagdollSystem] Bone '" << boneName << "' radius=" << bone.Radius 
              << " pos=(" << worldTransform[3][0] << "," << worldTransform[3][1] << "," << worldTransform[3][2] << ")\n";
    
    // Extract position and rotation from world transform
    glm::vec3 pos = ExtractPosition(worldTransform);
    glm::quat rot = ExtractRotation(worldTransform);
    
    // Normalize quaternion - Jolt Physics requires normalized quaternions
    // Floating-point precision errors from matrix->quaternion conversion can cause slight denormalization
    rot = glm::normalize(rot);
    
    // Create capsule shape (or sphere for very short bones)
    JPH::RefConst<JPH::Shape> shape;
    if (sizeHint.Length < sizeHint.Radius * 2.0f) {
        // Use sphere for very short/round bones
        shape = new JPH::SphereShape(bone.Radius);
    } else {
        // Use capsule for elongated bones
        shape = new JPH::CapsuleShape(bone.HalfHeight, bone.Radius);
    }
    
    // Create body settings
    // Clamp layer to valid range
    JPH::ObjectLayer objectLayer = static_cast<JPH::ObjectLayer>(
        physicsLayer < MAX_PHYSICS_LAYERS ? physicsLayer : 1
    );
    JPH::BodyCreationSettings settings(
        shape,
        JPH::RVec3(pos.x, pos.y, pos.z),
        JPH::Quat(rot.x, rot.y, rot.z, rot.w),
        JPH::EMotionType::Dynamic,
        objectLayer
    );
    
    settings.mMassPropertiesOverride.mMass = sizeHint.Mass;
    settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    // Stabilize ragdolls: higher angular damping + extra solver iterations reduce floppy limbs and post-impact rocking.
    settings.mLinearDamping = 0.35f;
    settings.mAngularDamping = 0.92f;
    settings.mFriction = 1.0f;
    settings.mRestitution = 0.0f;     // No bouncing
    settings.mGravityFactor = 1.0f;
    settings.mInertiaMultiplier = 1.4f;
    settings.mNumVelocityStepsOverride = 12;
    settings.mNumPositionStepsOverride = 4;
    settings.mMaxAngularVelocity = 18.0f;
    
    // Create the body
    JPH::Body* body = m_BodyInterface->CreateBody(settings);
    if (body) {
        body->SetUserData((uint64_t)boneEntity);
        m_BodyInterface->AddBody(body->GetID(), JPH::EActivation::Activate);
        bone.BodyID = body->GetID();
    }
    
    return bone;
}

void RagdollSystem::CreateJoints(RagdollComponent& ragdoll, SkeletonComponent& skeleton) {
    Scene& scene = Scene::Get();
    
    // Build a map from bone index to ragdoll bone array index
    std::unordered_map<int, size_t> boneIndexToArrayIndex;
    for (size_t i = 0; i < ragdoll.Bones.size(); ++i) {
        if (ragdoll.Bones[i].BoneIndex >= 0 && !ragdoll.Bones[i].BodyID.IsInvalid()) {
            boneIndexToArrayIndex[ragdoll.Bones[i].BoneIndex] = i;
        }
    }
    
    int jointCount = 0;
    
    // Create joints between parent-child bone pairs
    for (size_t childIdx = 0; childIdx < ragdoll.Bones.size(); ++childIdx) {
        const RagdollBone& childBone = ragdoll.Bones[childIdx];
        
        // Skip if this bone has no valid body or no parent
        if (childBone.BodyID.IsInvalid() || childBone.BoneIndex < 0) continue;
        if (childBone.ParentBoneIndex < 0) continue;  // Root bone has no parent
        
        // Find the parent bone in our ragdoll (might be skipped)
        auto parentIt = boneIndexToArrayIndex.find(childBone.ParentBoneIndex);
        if (parentIt == boneIndexToArrayIndex.end()) continue;
        
        const RagdollBone& parentBone = ragdoll.Bones[parentIt->second];
        if (parentBone.BodyID.IsInvalid()) continue;
        
        // Get bone entities and transforms
        EntityID childEntityId = skeleton.BoneEntities[childBone.BoneIndex];
        EntityID parentEntityId = skeleton.BoneEntities[parentBone.BoneIndex];
        auto* childData = scene.GetEntityData(childEntityId);
        auto* parentData = scene.GetEntityData(parentEntityId);
        if (!childData || !parentData) continue;
        
        // Joint position is at the child bone's origin (where it connects to parent)
        // This should match the actual connection point between bones
        glm::vec3 jointWorldPos = glm::vec3(childData->Transform.WorldMatrix[3]);
        JPH::RVec3 jointWorld(jointWorldPos.x, jointWorldPos.y, jointWorldPos.z);
        
        // Verify bodies are positioned correctly before creating constraint
        // Get actual body positions to ensure they're aligned
        const JPH::BodyLockInterfaceNoLock& lockInterface = m_Physics->GetBodyLockInterfaceNoLock();
        JPH::Body* parentBody = lockInterface.TryGetBody(parentBone.BodyID);
        JPH::Body* childBody = lockInterface.TryGetBody(childBone.BodyID);
        if (!parentBody || !childBody) continue;
        
        // Ensure bodies are at their correct positions (they should be from CreateBoneBody)
        // If there's a mismatch, the constraint will try to correct it, which can cause stretching
        
        // Get bone name for specialized limits
        std::string boneName = (childBone.BoneIndex < (int)skeleton.BoneNames.size()) 
            ? skeleton.BoneNames[childBone.BoneIndex] : "";
        
        // Extract bone direction (Y-axis in bone local space typically points along the bone)
        // Use the child bone's world rotation to determine twist axis
        glm::mat4 childWorld = childData->Transform.WorldMatrix;
        glm::vec3 boneY = glm::normalize(glm::vec3(childWorld[1])); // Local Y in world space
        glm::vec3 boneX = glm::normalize(glm::vec3(childWorld[0])); // Local X in world space
        glm::vec3 boneZ = glm::normalize(glm::vec3(childWorld[2])); // Local Z in world space
        
        // Default joint limits (conservative) to avoid limb splay/explosive poses.
        float normalHalfCone = 30.0f;  // Swing limit in degrees
        float planeHalfCone = 30.0f;   // Swing limit in degrees
        float twistMin = -20.0f;       // Twist limit min
        float twistMax = 20.0f;        // Twist limit max
        float maxFrictionTorque = 22.0f; // Passive angular friction torque (N m)
        
        // Determine twist axis based on bone type
        // For limbs, twist is along the bone length (Y axis typically)
        // For spine, twist is vertical
        glm::vec3 twistAxisGlm = boneY;
        glm::vec3 planeAxisGlm = boneX;
        
        // Ensure axes are normalized and orthogonal
        // SwingTwist constraint requires normalized, orthogonal axes
        twistAxisGlm = glm::normalize(twistAxisGlm);
        
        // Make plane axis orthogonal to twist axis using Gram-Schmidt
        float dot = glm::dot(planeAxisGlm, twistAxisGlm);
        planeAxisGlm = glm::normalize(planeAxisGlm - dot * twistAxisGlm);
        
        // Validate axes are not zero or invalid
        float twistLen = glm::length(twistAxisGlm);
        float planeLen = glm::length(planeAxisGlm);
        if (twistLen < 0.001f || planeLen < 0.001f || 
            std::isnan(twistLen) || std::isnan(planeLen) ||
            std::isinf(twistLen) || std::isinf(planeLen)) {
            std::cerr << "[RagdollSystem] Invalid axes for bone '" << boneName 
                      << "' - skipping joint creation\n";
            continue;
        }
        
        JPH::Vec3 twistAxis(twistAxisGlm.x, twistAxisGlm.y, twistAxisGlm.z);
        JPH::Vec3 planeAxis(planeAxisGlm.x, planeAxisGlm.y, planeAxisGlm.z);
        
        // Specialized limits for different joint types
        if (boneName.find("Knee") != std::string::npos || 
            (boneName.find("Leg") != std::string::npos && boneName.find("Up") == std::string::npos)) {
            // Knee - hinge-like, bends backward only
            normalHalfCone = 5.0f;
            planeHalfCone = 60.0f;  // Main bend axis
            twistMin = -2.0f;
            twistMax = 2.0f;
            maxFrictionTorque = 46.0f;
        }
        else if (boneName.find("Elbow") != std::string::npos || 
                 boneName.find("ForeArm") != std::string::npos) {
            // Elbow - hinge-like
            normalHalfCone = 5.0f;
            planeHalfCone = 55.0f;
            twistMin = -25.0f;  // Forearm can twist (pronation/supination), but keep it controlled
            twistMax = 25.0f;
            maxFrictionTorque = 38.0f;
        }
        else if (boneName.find("Spine") != std::string::npos) {
            // Spine - limited bending in all directions
            normalHalfCone = 12.0f;
            planeHalfCone = 12.0f;
            twistMin = -8.0f;
            twistMax = 8.0f;
            maxFrictionTorque = 55.0f;
        }
        else if (boneName.find("Neck") != std::string::npos) {
            // Neck - moderate flexibility
            normalHalfCone = 18.0f;
            planeHalfCone = 18.0f;
            twistMin = -20.0f;
            twistMax = 20.0f;
            maxFrictionTorque = 34.0f;
        }
        else if (boneName.find("Head") != std::string::npos) {
            // Head - can nod and turn
            normalHalfCone = 22.0f;
            planeHalfCone = 22.0f;
            twistMin = -26.0f;
            twistMax = 26.0f;
            maxFrictionTorque = 30.0f;
        }
        else if (boneName.find("Shoulder") != std::string::npos) {
            // Clavicle/shoulder root - keep compact to prevent "arms flying up".
            normalHalfCone = 12.0f;
            planeHalfCone = 18.0f;
            twistMin = -10.0f;
            twistMax = 10.0f;
            maxFrictionTorque = 44.0f;
        }
        else if ((boneName.find("Arm") != std::string::npos && boneName.find("Fore") == std::string::npos) ||
                 boneName.find("UpperArm") != std::string::npos) {
            // Upper arm - still flexible, but much tighter than before to avoid splay.
            normalHalfCone = 38.0f;
            planeHalfCone = 42.0f;
            twistMin = -28.0f;
            twistMax = 28.0f;
            maxFrictionTorque = 36.0f;
        }
        else if (boneName.find("UpLeg") != std::string::npos ||
                 boneName.find("Thigh") != std::string::npos) {
            // Hip - ball socket with limits
            normalHalfCone = 34.0f;
            planeHalfCone = 34.0f;
            twistMin = -18.0f;
            twistMax = 18.0f;
            maxFrictionTorque = 52.0f;
        }
        else if (boneName.find("Foot") != std::string::npos) {
            // Ankle - limited range
            normalHalfCone = 15.0f;
            planeHalfCone = 20.0f;
            twistMin = -10.0f;
            twistMax = 10.0f;
            maxFrictionTorque = 30.0f;
        }
        else if (boneName.find("Hand") != std::string::npos) {
            // Wrist - moderate flexibility
            normalHalfCone = 30.0f;
            planeHalfCone = 30.0f;
            twistMin = -20.0f;
            twistMax = 20.0f;
            maxFrictionTorque = 18.0f;
        }
        
        // Create SwingTwist constraint with computed axes
        // SwingTwistConstraint includes a PointConstraintPart that keeps bones connected at the joint
        // When using WorldSpace, both positions should be the same world position
        // Jolt will automatically convert to each body's local space
        
        // Validate angle limits are within valid ranges
        // Jolt Physics requires: twistMax >= twistMin, and cone angles >= 0
        if (twistMax < twistMin) {
            std::cerr << "[RagdollSystem] Invalid twist limits for bone '" << boneName 
                      << "' (min=" << twistMin << ", max=" << twistMax << ") - skipping joint\n";
            continue;
        }
        
        JPH::SwingTwistConstraintSettings settings;
        settings.mSpace = JPH::EConstraintSpace::WorldSpace;
        settings.mSwingType = JPH::ESwingType::Pyramid;
        settings.mPosition1 = settings.mPosition2 = jointWorld;
        settings.mTwistAxis1 = settings.mTwistAxis2 = twistAxis;
        settings.mPlaneAxis1 = settings.mPlaneAxis2 = planeAxis;
        settings.mNormalHalfConeAngle = JPH::DegreesToRadians(normalHalfCone);
        settings.mPlaneHalfConeAngle = JPH::DegreesToRadians(planeHalfCone);
        settings.mTwistMinAngle = JPH::DegreesToRadians(twistMin);
        settings.mTwistMaxAngle = JPH::DegreesToRadians(twistMax);
        settings.mMaxFrictionTorque = maxFrictionTorque;
        
        // Validate constraint settings before creating
        if (settings.mNormalHalfConeAngle < 0.0f || settings.mPlaneHalfConeAngle < 0.0f ||
            std::isnan(settings.mNormalHalfConeAngle) || std::isnan(settings.mPlaneHalfConeAngle) ||
            std::isnan(settings.mTwistMinAngle) || std::isnan(settings.mTwistMaxAngle)) {
            std::cerr << "[RagdollSystem] Invalid constraint angles for bone '" << boneName 
                      << "' - skipping joint\n";
            continue;
        }
        
        // Ensure constraint is created with bodies in correct positions
        // The PointConstraintPart in SwingTwistConstraint will keep bones connected
        // If stretching occurs, it's likely due to constraint solver iterations or timestep issues
        
        // Create the constraint (bodies were already retrieved above)
        if (parentBody && childBody) {
            JPH::Constraint* constraint = settings.Create(*parentBody, *childBody);
            if (constraint) {
                m_Physics->AddConstraint(constraint);
                
                RagdollJoint joint;
                joint.Constraint = constraint;
                joint.ParentBoneIndex = (int)parentIt->second;
                joint.ChildBoneIndex = (int)childIdx;
                ragdoll.Joints.push_back(joint);
                
                jointCount++;
            }
        }
    }
    
    std::cout << "[RagdollSystem] Created " << jointCount << " joints\n";
}

bool RagdollSystem::CreateRagdoll(EntityID entityId, bool includeFingersAndToes, uint32_t physicsLayer) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    // Check if ragdoll already exists
    if (m_Ragdolls.find(entityId) != m_Ragdolls.end()) {
        std::cout << "[RagdollSystem] Ragdoll already exists for entity " << entityId << "\n";
        return false;
    }
    
    // Find skeleton
    EntityID skeletonEntity = FindSkeletonEntity(entityId);
    if (skeletonEntity == INVALID_ENTITY_ID) {
        std::cerr << "[RagdollSystem] No skeleton found for entity " << entityId << "\n";
        return false;
    }
    
    Scene& scene = Scene::Get();
    auto* skelData = scene.GetEntityData(skeletonEntity);
    if (!skelData || !skelData->Skeleton) {
        std::cerr << "[RagdollSystem] Invalid skeleton data\n";
        return false;
    }
    
    SkeletonComponent& skeleton = *skelData->Skeleton;
    const size_t boneCount = skeleton.BoneEntities.size();
    
    if (boneCount == 0) {
        std::cerr << "[RagdollSystem] Skeleton has no bones\n";
        return false;
    }
    
    std::cout << "[RagdollSystem] Creating ragdoll with " << boneCount << " bones\n";
    
    // Prepare bone data for parallel creation
    struct BoneCreateData {
        EntityID boneEntity;
        std::string boneName;
        int boneIndex;
        int parentIndex;
        glm::mat4 worldTransform;
        bool skip;  // Skip fingers/toes if not wanted
    };
    
    std::vector<BoneCreateData> boneData(boneCount);
    
    // Gather bone data (single-threaded - just data gathering)
    for (size_t i = 0; i < boneCount; ++i) {
        BoneCreateData& bd = boneData[i];
        bd.boneEntity = skeleton.BoneEntities[i];
        bd.boneIndex = (int)i;
        bd.parentIndex = (i < skeleton.BoneParents.size()) ? skeleton.BoneParents[i] : -1;
        bd.boneName = (i < skeleton.BoneNames.size()) ? skeleton.BoneNames[i] : "";
        bd.skip = false;
        
        // Skip small bones if requested
        if (!includeFingersAndToes) {
            if (bd.boneName.find("Finger") != std::string::npos ||
                bd.boneName.find("Thumb") != std::string::npos ||
                bd.boneName.find("Toe") != std::string::npos ||
                bd.boneName.find("Index") != std::string::npos ||
                bd.boneName.find("Middle") != std::string::npos ||
                bd.boneName.find("Ring") != std::string::npos ||
                bd.boneName.find("Pinky") != std::string::npos) {
                bd.skip = true;
                continue;
            }
        }
        
        // Get world transform
        auto* boneEntityData = scene.GetEntityData(bd.boneEntity);
        if (boneEntityData) {
            bd.worldTransform = boneEntityData->Transform.WorldMatrix;
        } else {
            bd.worldTransform = glm::mat4(1.0f);
        }
    }
    
    // Create ragdoll component
    RagdollComponent& ragdoll = m_Ragdolls[entityId];
    ragdoll.SkeletonEntity = skeletonEntity;
    ragdoll.PhysicsLayer = physicsLayer;
    ragdoll.Bones.resize(boneCount);
    
    // Count non-skipped bones for logging
    std::atomic<int> createdCount{0};
    
    // Parallel bone body creation
    // Note: Jolt's CreateBody is thread-safe when using separate BodyCreationSettings
    auto createBone = [&](size_t start, size_t count) {
        for (size_t i = start; i < start + count && i < boneCount; ++i) {
            const BoneCreateData& bd = boneData[i];
            if (bd.skip) {
                ragdoll.Bones[i].BoneIndex = -1;  // Mark as skipped
                continue;
            }
            
            ragdoll.Bones[i] = CreateBoneBody(
                bd.boneEntity, bd.boneName, bd.boneIndex, bd.parentIndex, bd.worldTransform,
                physicsLayer
            );
            
            if (!ragdoll.Bones[i].BodyID.IsInvalid()) {
                createdCount++;
            }
        }
    };
    
    // Use parallel_for if we have enough bones and job system is available
    const size_t minBonesForParallel = 8;
    if (boneCount >= minBonesForParallel && cm::g_JobSystem != nullptr) {
        parallel_for(Jobs(), size_t{0}, boneCount, size_t{4}, createBone);
    } else {
        // Sequential fallback
        createBone(0, boneCount);
    }
    
    std::cout << "[RagdollSystem] Created " << createdCount.load() << " bone bodies\n";
    
    // Create joints between bones to keep them connected
    CreateJoints(ragdoll, skeleton);
    
    ragdoll.Active = true;
    m_SkeletonToOwner[skeletonEntity] = entityId;
    m_ActiveSkeletons[skeletonEntity] = 1u;
    
    return true;
}

void RagdollSystem::DestroyRagdoll(EntityID entityId) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    auto it = m_Ragdolls.find(entityId);
    if (it == m_Ragdolls.end()) return;
    
    RagdollComponent& ragdoll = it->second;
    
    // Remove all joints first (must be done before removing bodies)
    for (auto& joint : ragdoll.Joints) {
        if (joint.Constraint) {
            m_Physics->RemoveConstraint(joint.Constraint);
        }
    }
    ragdoll.Joints.clear();
    
    // Remove and destroy all bone bodies
    for (auto& bone : ragdoll.Bones) {
        if (!bone.BodyID.IsInvalid()) {
            m_BodyInterface->RemoveBody(bone.BodyID);
            m_BodyInterface->DestroyBody(bone.BodyID);
        }
    }
    
    m_ActiveSkeletons.erase(ragdoll.SkeletonEntity);
    m_ActiveSkeletonBounds.erase(ragdoll.SkeletonEntity);
    m_SkeletonToOwner.erase(ragdoll.SkeletonEntity);
    m_Ragdolls.erase(it);
    std::cout << "[RagdollSystem] Destroyed ragdoll for entity " << entityId << "\n";
}

bool RagdollSystem::HasRagdoll(EntityID entityId) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Ragdolls.find(entityId) != m_Ragdolls.end();
}

bool RagdollSystem::IsSkeletonRagdollActive(EntityID skeletonEntity) const {
    if (skeletonEntity == INVALID_ENTITY_ID) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_ActiveSkeletons.find(skeletonEntity) != m_ActiveSkeletons.end();
}

bool RagdollSystem::TryGetActiveSkeletonBounds(EntityID skeletonEntity, glm::vec3& outMin, glm::vec3& outMax) const {
    if (skeletonEntity == INVALID_ENTITY_ID) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_Mutex);
    const auto it = m_ActiveSkeletonBounds.find(skeletonEntity);
    if (it == m_ActiveSkeletonBounds.end()) {
        return false;
    }

    outMin = it->second.first;
    outMax = it->second.second;
    return true;
}

bool RagdollSystem::HasActiveRagdolls() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return !m_ActiveSkeletons.empty();
}

void RagdollSystem::Update(float dt) {
    (void)dt;
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    if (m_Ragdolls.empty() || m_ActiveSkeletons.empty()) return;
    
    Scene& scene = Scene::Get();
    m_ActiveSkeletonBounds.clear();
    m_ActiveSkeletonBounds.reserve(m_ActiveSkeletons.size());
    
    for (auto& [entityId, ragdoll] : m_Ragdolls) {
        (void)entityId;
        if (!ragdoll.Active) continue;
        
        auto* skelData = scene.GetEntityData(ragdoll.SkeletonEntity);
        if (!skelData || !skelData->Skeleton) continue;
        
        SkeletonComponent& skeleton = *skelData->Skeleton;
        
        // First pass: Copy physics transforms to bones that have bodies
        for (const auto& bone : ragdoll.Bones) {
            if (bone.BodyID.IsInvalid() || bone.BoneIndex < 0) continue;
            if (bone.BoneIndex >= (int)skeleton.BoneEntities.size()) continue;
            
            EntityID boneEntityId = skeleton.BoneEntities[bone.BoneIndex];
            auto* boneData = scene.GetEntityData(boneEntityId);
            if (!boneData) continue;
            
            // Get physics transform (world space)
            JPH::RVec3 pos;
            JPH::Quat rot;
            m_BodyInterface->GetPositionAndRotation(bone.BodyID, pos, rot);
            
            // Build world matrix from physics transform
            glm::vec3 worldPos((float)pos.GetX(), (float)pos.GetY(), (float)pos.GetZ());
            glm::quat worldRot(rot.GetW(), rot.GetX(), rot.GetY(), rot.GetZ());
            
            // Extract the original scale from the current WorldMatrix
            glm::vec3 origScale(
                glm::length(glm::vec3(boneData->Transform.WorldMatrix[0])),
                glm::length(glm::vec3(boneData->Transform.WorldMatrix[1])),
                glm::length(glm::vec3(boneData->Transform.WorldMatrix[2]))
            );
            
            // Construct world matrix: Translation * Rotation * Scale
            glm::mat4 worldMatrix = glm::translate(glm::mat4(1.0f), worldPos) 
                                  * glm::mat4_cast(worldRot)
                                  * glm::scale(glm::mat4(1.0f), origScale);
            
            boneData->Transform.WorldMatrix = worldMatrix;
            boneData->Transform.TransformDirty = false;
            scene.NotifyWorldTransformOverride(boneEntityId);
        }
        
        // Second pass: Update non-physics bones based on their parent's new transform
        // Process bones in order (parents before children) using BoneParents array
        for (size_t i = 0; i < skeleton.BoneEntities.size(); ++i) {
            // Skip bones that have physics bodies (already updated)
            if (i < ragdoll.Bones.size()) {
                const RagdollBone& ragdollBone = ragdoll.Bones[i];
                if (!ragdollBone.BodyID.IsInvalid() && ragdollBone.BoneIndex >= 0) {
                    continue;
                }
            }
            
            EntityID boneEntityId = skeleton.BoneEntities[i];
            auto* boneData = scene.GetEntityData(boneEntityId);
            if (!boneData) continue;
            
            // Get parent bone index
            int parentIdx = (i < skeleton.BoneParents.size()) ? skeleton.BoneParents[i] : -1;
            if (parentIdx < 0) continue;  // No parent, skip
            
            // Get parent's world matrix
            glm::mat4 parentWorld = glm::mat4(1.0f);
            if (parentIdx < (int)skeleton.BoneEntities.size()) {
                EntityID parentEntityId = skeleton.BoneEntities[parentIdx];
                auto* parentData = scene.GetEntityData(parentEntityId);
                if (parentData) {
                    parentWorld = parentData->Transform.WorldMatrix;
                }
            }
            
            // Recompute this bone's world matrix from its local matrix and parent's world
            boneData->Transform.WorldMatrix = parentWorld * boneData->Transform.LocalMatrix;
            boneData->Transform.TransformDirty = false;
            scene.NotifyWorldTransformOverride(boneEntityId);
        }

        glm::vec3 minBounds(std::numeric_limits<float>::max());
        glm::vec3 maxBounds(-std::numeric_limits<float>::max());
        bool hasBounds = false;
        for (EntityID boneEntityId : skeleton.BoneEntities) {
            auto* boneData = scene.GetEntityData(boneEntityId);
            if (!boneData) continue;

            const glm::vec3 bonePos = glm::vec3(boneData->Transform.WorldMatrix[3]);
            minBounds = glm::min(minBounds, bonePos);
            maxBounds = glm::max(maxBounds, bonePos);
            hasBounds = true;
        }

        if (hasBounds) {
            m_ActiveSkeletonBounds[ragdoll.SkeletonEntity] = std::make_pair(minBounds, maxBounds);
        }
    }
}

void RagdollSystem::ActivateRagdoll(EntityID entityId) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    auto it = m_Ragdolls.find(entityId);
    if (it == m_Ragdolls.end()) return;
    
    it->second.Active = true;
    m_ActiveSkeletons[it->second.SkeletonEntity] = 1u;
    m_ActiveSkeletonBounds.erase(it->second.SkeletonEntity);
    
    // Increase mesh bounds padding for ragdoll entities to fix frustum culling
    // Ragdoll bones can stretch significantly, so we need larger bounds
    Scene& scene = Scene::Get();
    auto* rootData = scene.GetEntityData(entityId);
    if (rootData && rootData->Mesh) {
        // Increase bounds padding to account for ragdoll stretching (3.0x should cover most cases)
        rootData->Mesh->BoundsPadding = std::max(rootData->Mesh->BoundsPadding, 3.0f);
    }
    
    // Also update child meshes (armor, etc.)
    if (rootData) {
        for (EntityID childId : rootData->Children) {
            auto* childData = scene.GetEntityData(childId);
            if (childData && childData->Mesh) {
                childData->Mesh->BoundsPadding = std::max(childData->Mesh->BoundsPadding, 3.0f);
            }
        }
    }
    
    // TODO: Disable animation on this entity
    // This would need integration with AnimationSystem
}

void RagdollSystem::DeactivateRagdoll(EntityID entityId) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    auto it = m_Ragdolls.find(entityId);
    if (it == m_Ragdolls.end()) return;
    
    it->second.Active = false;
    m_ActiveSkeletons.erase(it->second.SkeletonEntity);
    m_ActiveSkeletonBounds.erase(it->second.SkeletonEntity);
    
    // Make all bodies kinematic
    for (auto& bone : it->second.Bones) {
        if (!bone.BodyID.IsInvalid()) {
            m_BodyInterface->SetMotionType(bone.BodyID, JPH::EMotionType::Kinematic, JPH::EActivation::DontActivate);
        }
    }
}

void RagdollSystem::ApplyImpulse(EntityID entityId, int boneIndex, const glm::vec3& impulse) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    auto it = m_Ragdolls.find(entityId);
    if (it == m_Ragdolls.end()) return;
    
    for (auto& bone : it->second.Bones) {
        if (bone.BoneIndex == boneIndex && !bone.BodyID.IsInvalid()) {
            m_BodyInterface->AddImpulse(bone.BodyID, JPH::Vec3(impulse.x, impulse.y, impulse.z));
            break;
        }
    }
}

void RagdollSystem::ApplyImpulseToAll(EntityID entityId, const glm::vec3& impulse) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    auto it = m_Ragdolls.find(entityId);
    if (it == m_Ragdolls.end()) return;
    
    JPH::Vec3 joltImpulse(impulse.x, impulse.y, impulse.z);
    for (auto& bone : it->second.Bones) {
        if (!bone.BodyID.IsInvalid()) {
            m_BodyInterface->AddImpulse(bone.BodyID, joltImpulse);
        }
    }
}

void RagdollSystem::SetPhysicsLayer(EntityID entityId, uint32_t layer) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    auto it = m_Ragdolls.find(entityId);
    if (it == m_Ragdolls.end()) {
        std::cerr << "[RagdollSystem] Cannot set physics layer: ragdoll not found for entity " << entityId << "\n";
        return;
    }
    
    // Clamp layer to valid range
    JPH::ObjectLayer objectLayer = static_cast<JPH::ObjectLayer>(
        layer < MAX_PHYSICS_LAYERS ? layer : 1
    );
    
    // Update all bone bodies
    for (auto& bone : it->second.Bones) {
        if (!bone.BodyID.IsInvalid()) {
            m_BodyInterface->SetObjectLayer(bone.BodyID, objectLayer);
        }
    }
    
    // Update stored layer
    it->second.PhysicsLayer = layer;
    
    std::cout << "[RagdollSystem] Set physics layer " << layer << " for ragdoll entity " << entityId << "\n";
}

EntityID RagdollSystem::GetRagdollOwnerFromBone(EntityID boneEntityId) const {
    std::lock_guard<std::mutex> lock(m_Mutex);

    // Walk up hierarchy from bone entity to find skeleton root
    EntityID skeletonEntity = FindSkeletonEntity(boneEntityId);
    if (skeletonEntity == INVALID_ENTITY_ID) {
        return INVALID_ENTITY_ID;
    }

    auto it = m_SkeletonToOwner.find(skeletonEntity);
    return (it != m_SkeletonToOwner.end()) ? it->second : INVALID_ENTITY_ID;
}

} // namespace cm::physics

