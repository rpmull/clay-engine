#pragma once
#include "RagdollComponent.h"
#include <Jolt/Physics/PhysicsSystem.h>
#include <unordered_map>
#include <mutex>

struct SkeletonComponent; // Forward declaration

namespace cm::physics {

/// Ragdoll system - creates and manages physics ragdolls from skeleton data
class RagdollSystem {
public:
    RagdollSystem(JPH::PhysicsSystem* phys, JPH::BodyInterface* bi);
    ~RagdollSystem();
    
    /// Create a ragdoll for an entity with a skeleton.
    /// Bones are created in parallel for performance.
    /// @param entityId Entity that has a SkeletonComponent (or whose child has one)
    /// @param includeFingersAndToes If false, skips small bones for better perf
    /// @param physicsLayer Physics layer for ragdoll bones (default: 1)
    /// @return true if ragdoll was created successfully
    bool CreateRagdoll(EntityID entityId, bool includeFingersAndToes = false, uint32_t physicsLayer = 1);
    
    /// Destroy ragdoll for an entity, removing all physics bodies
    void DestroyRagdoll(EntityID entityId);
    
    /// Check if entity has an active ragdoll
    bool HasRagdoll(EntityID entityId) const;
    
    /// Check whether a skeleton entity is currently driven by any active ragdoll.
    bool IsSkeletonRagdollActive(EntityID skeletonEntity) const;

    /// Fetch cached world-space bounds for an active ragdoll-driven skeleton.
    bool TryGetActiveSkeletonBounds(EntityID skeletonEntity, glm::vec3& outMin, glm::vec3& outMax) const;

    /// Returns true when any ragdoll is currently active.
    bool HasActiveRagdolls() const;
    
    /// Update all active ragdolls - copies physics transforms to bone entities
    void Update(float dt);
    
    /// Disable animation and enable ragdoll (convenience method)
    void ActivateRagdoll(EntityID entityId);
    
    /// Re-enable animation and disable ragdoll physics
    void DeactivateRagdoll(EntityID entityId);
    
    /// Apply an impulse to a specific bone
    void ApplyImpulse(EntityID entityId, int boneIndex, const glm::vec3& impulse);
    
    /// Apply an impulse to all bones (explosion effect, etc.)
    void ApplyImpulseToAll(EntityID entityId, const glm::vec3& impulse);
    
    /// Set physics layer for all bones in a ragdoll
    void SetPhysicsLayer(EntityID entityId, uint32_t layer);
    
    /// Find the ragdoll owner entity from a bone entity (walks up hierarchy to find skeleton, then finds ragdoll owner)
    EntityID GetRagdollOwnerFromBone(EntityID boneEntityId) const;
    
private:
    JPH::PhysicsSystem* m_Physics;
    JPH::BodyInterface* m_BodyInterface;
    
    std::unordered_map<EntityID, RagdollComponent> m_Ragdolls;
    std::unordered_map<EntityID, EntityID> m_SkeletonToOwner;
    std::unordered_map<EntityID, uint8_t> m_ActiveSkeletons;
    std::unordered_map<EntityID, std::pair<glm::vec3, glm::vec3>> m_ActiveSkeletonBounds;
    mutable std::mutex m_Mutex;
    
    /// Find skeleton component on entity or its children
    EntityID FindSkeletonEntity(EntityID rootEntity) const;
    
    /// Create a single bone body (called in parallel)
    RagdollBone CreateBoneBody(EntityID boneEntity, const std::string& boneName, 
                                int boneIndex, int parentIndex, const glm::mat4& worldTransform,
                                uint32_t physicsLayer = 1);
    
    /// Create joints between parent-child bone pairs
    /// Must be called after all bone bodies are created
    void CreateJoints(RagdollComponent& ragdoll, SkeletonComponent& skeleton);
};

/// Global ragdoll system instance
RagdollSystem* GetRagdollSystem();
void InitRagdollSystem(JPH::PhysicsSystem* phys, JPH::BodyInterface* bi);
void ShutdownRagdollSystem();

} // namespace cm::physics

