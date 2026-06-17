#pragma once

// Forward declarations
struct ColliderComponent;
struct EntityData;
struct RigidBodyComponent;
struct StaticBodyComponent;
namespace cm { namespace physics { struct AreaComponent; } }

//------------------------------------------------------------------------------
// Utility functions for components that need EntityData
//------------------------------------------------------------------------------
void ApplyMeshBoundsToBoxCollider(ColliderComponent& collider, const EntityData& entityData);
void EnsureCollider(RigidBodyComponent* rigidBody, EntityData* entityData);
void EnsureCollider(StaticBodyComponent* staticBody, EntityData* entityData); 
void InitializeAreaComponent(cm::physics::AreaComponent* area, EntityData* entityData);
