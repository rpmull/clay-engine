#pragma once
#include <cstdint>
#include <string>

class Scene;

//----------------------------------------------------------------------
// EntityID
//----------------------------------------------------------------------
using EntityID = uint32_t;
constexpr EntityID INVALID_ENTITY_ID = static_cast<EntityID>(-1);


//----------------------------------------------------------------------
// Entity: Lightweight handle to an entity in a scene
//----------------------------------------------------------------------
class Entity {
public:
   Entity() : m_ID(0), m_Scene(nullptr) {}
   Entity(EntityID id) : m_ID(id), m_Scene(nullptr) {}
   Entity(EntityID id, Scene* scene) : m_ID(id), m_Scene(scene) {}

   bool IsValid() const { return m_Scene != nullptr && m_ID != 0; }
   EntityID GetID() const { return m_ID; }
   Scene* GetScene() const { return m_Scene; }

   const std::string& GetName() const;
   void SetName(const std::string& name);

private:
   EntityID m_ID;
   Scene* m_Scene;
   
   // Allow Scene to update Entity pointers after move operations
   friend class Scene;
   void UpdateScenePointer(Scene* newScene) { m_Scene = newScene; }
   };
