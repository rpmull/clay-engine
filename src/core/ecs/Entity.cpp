#include "Entity.h"
#include "Scene.h"

///----------------------------------------------------------------------
/// GetName: Returns the name of the entity
///----------------------------------------------------------------------
const std::string& Entity::GetName() const {
    static const std::string kEmptyName = "";
    if (!m_Scene) return kEmptyName;
    auto* data = m_Scene->GetEntityData(m_ID);
    return data ? data->Name : kEmptyName;
}



///----------------------------------------------------------------------
/// SetName: Sets the name of the entity
///----------------------------------------------------------------------
void Entity::SetName(const std::string& name) {
    if (m_Scene) {
        m_Scene->SetEntityName(m_ID, name);
    }
}
