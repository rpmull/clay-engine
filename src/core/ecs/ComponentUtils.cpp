#include "ComponentUtils.h"
#include "core/ecs/Components.h"
#include "core/ecs/EntityData.h"
#include "core/physics/area/AreaComponent.h"

namespace {
   void ApplyMeshBoundsToBoxCollider(ColliderComponent& collider, const EntityData& entityData) {
      if (entityData.Mesh && entityData.Mesh->mesh)
         {
         const glm::vec3 localMin = entityData.Mesh->mesh->BoundsMin;
         const glm::vec3 localMax = entityData.Mesh->mesh->BoundsMax;
         collider.Size = glm::abs(localMax - localMin);
         collider.Offset = (localMin + localMax) * 0.5f;
         }
      else {
         collider.Size = glm::vec3(1.0f);
         collider.Offset = glm::vec3(0.0f);
         }
   }
}


///----------------------------------------------------------------------
/// EnsureCollider: Ensures that the given Rigid Body entity has a 
/// ColliderComponent.
/// If not, creates a default Box collider sized to the mesh bounds if 
/// available, centered on those local bounds, or a 1x1x1 box otherwise.
///----------------------------------------------------------------------
void EnsureCollider(RigidBodyComponent* rigidBody, EntityData* entityData) {
   if (!entityData->Collider) 
      {
      entityData->Collider = std::make_unique<ColliderComponent>();
      entityData->Collider->ShapeType = ColliderShape::Box;
      ApplyMeshBoundsToBoxCollider(*entityData->Collider, *entityData);
      }
   }


///----------------------------------------------------------------------
/// EnsureCollider: Ensures that the given Static Body entity has a
/// ColliderComponent.
/// If not, creates a default Box collider sized to the mesh bounds if
/// available, centered on those local bounds, or a 1x1x1 box otherwise.
/// ----------------------------------------------------------------------
void EnsureCollider(StaticBodyComponent* staticBody, EntityData* entityData) {
   if (!entityData->Collider) 
      {
      entityData->Collider = std::make_unique<ColliderComponent>();
      entityData->Collider->ShapeType = ColliderShape::Box;
      ApplyMeshBoundsToBoxCollider(*entityData->Collider, *entityData);
      }
   }

///----------------------------------------------------------------------
/// InitializeAreaComponent: Sets up a newly added AreaComponent so it
/// starts with a reasonable volume in editor view, mirroring EnsureCollider.
///----------------------------------------------------------------------
void InitializeAreaComponent(cm::physics::AreaComponent* area, EntityData* entityData)
{
   if (!area || !entityData) return;

   area->ShapeType = cm::physics::AreaShapeType::Box;

   if (entityData->Mesh && entityData->Mesh->mesh)
      {
      glm::vec3 localSize = glm::abs(entityData->Mesh->mesh->BoundsMax - entityData->Mesh->mesh->BoundsMin);
      area->Size = glm::abs(localSize * entityData->Transform.Scale);
      }
   else
      {
      area->Size = glm::vec3(1.0f);
      }
}
