#pragma once
#include <unordered_map>
#include <functional>
#include <string>
#include "core/ecs/Entity.h"

class Scene;

class ComponentDrawerRegistry {
public:
    using DrawFunc = std::function<void(void*)>;

    static ComponentDrawerRegistry& Instance() {
        static ComponentDrawerRegistry instance;
        return instance;
    }

    template<typename T>
    void Register(const std::string& name, std::function<void(T&)> drawFn) {
        m_Drawers[name] = [drawFn](void* comp) {
            drawFn(*static_cast<T*>(comp));
            };
    }

    void DrawComponentUI(const std::string& name, void* ptr) const {
        auto it = m_Drawers.find(name);
        if (it != m_Drawers.end()) {
            it->second(ptr);
        }
    }

    void SetDrawContext(Scene* scene, EntityID entity) {
        m_CurrentScene = scene;
        m_CurrentEntity = entity;
    }

    Scene* GetCurrentScene() const {
        return m_CurrentScene;
    }

    EntityID GetCurrentEntity() const {
        return m_CurrentEntity;
    }

    const std::unordered_map<std::string, DrawFunc>& GetAllDrawers() const {
        return m_Drawers;
    }

private:
    std::unordered_map<std::string, DrawFunc> m_Drawers;
    Scene* m_CurrentScene = nullptr;
    EntityID m_CurrentEntity = INVALID_ENTITY_ID;
};
