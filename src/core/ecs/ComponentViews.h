#pragma once
#include "Entity.h"
#include "EntityData.h"
#include <vector>
#include <unordered_map>
#include <functional>
#include <algorithm>

// Cache-friendly component views for high-performance ECS iteration
// Reduces cache misses by grouping entities with specific component types
// Targets Unity/Unreal-class iteration performance

namespace cm {
namespace ecs {

class Scene; // Forward declare

// Base view type for iterating entities with specific components
template<typename... Components>
class ComponentView {
public:
    using EntityList = std::vector<EntityID>;
    using EntityDataMap = std::unordered_map<EntityID, EntityData>;

    ComponentView(const EntityList& entities, EntityDataMap& dataMap)
        : m_entities(entities), m_dataMap(dataMap) {}

    // Iterate with callback receiving entity ID and component references
    template<typename Func>
    void ForEach(Func&& fn) {
        for (EntityID id : m_filteredEntities) {
            auto it = m_dataMap.find(id);
            if (it == m_dataMap.end()) continue;
            fn(id, it->second);
        }
    }

    // Parallel iteration using job system
    template<typename JobSys, typename Func>
    void ParallelForEach(JobSys& js, Func&& fn, size_t chunkSize = 64) {
        if (m_filteredEntities.empty()) return;
        
        auto& filtered = m_filteredEntities;
        auto& dataMap = m_dataMap;
        
        parallel_for(js, 0, filtered.size(), chunkSize,
            [&filtered, &dataMap, &fn](size_t start, size_t count) {
                for (size_t i = start; i < start + count; ++i) {
                    EntityID id = filtered[i];
                    auto it = dataMap.find(id);
                    if (it != dataMap.end()) {
                        fn(id, it->second);
                    }
                }
            });
    }

    // Get filtered count
    size_t Size() const { return m_filteredEntities.size(); }
    bool Empty() const { return m_filteredEntities.empty(); }

    // Rebuild filter (call when entities change)
    void Refresh() {
        m_filteredEntities.clear();
        m_filteredEntities.reserve(m_entities.size());
        for (const auto& entity : m_entities) {
            EntityID id = entity.GetID();
            auto it = m_dataMap.find(id);
            if (it == m_dataMap.end()) continue;
            if (MatchesFilter(it->second)) {
                m_filteredEntities.push_back(id);
            }
        }
    }

protected:
    virtual bool MatchesFilter(const EntityData& data) const = 0;

    const EntityList& m_entities;
    EntityDataMap& m_dataMap;
    std::vector<EntityID> m_filteredEntities;
};

// Specialized view for mesh entities (most common rendering case)
class MeshView {
public:
    struct CachedMeshEntity {
        EntityID id;
        EntityData* data;
        TransformComponent* transform;
        MeshComponent* mesh;
        SkinningComponent* skinning;  // May be null
    };

    MeshView() = default;

    void Refresh(const std::vector<Entity>& entities, 
                 std::unordered_map<EntityID, EntityData>& dataMap,
                 bool visibleOnly = true,
                 bool activeOnly = true) {
        m_cached.clear();
        m_cached.reserve(entities.size());

        for (const auto& entity : entities) {
            EntityID id = entity.GetID();
            auto it = dataMap.find(id);
            if (it == dataMap.end()) continue;
            
            EntityData& data = it->second;
            if (visibleOnly && !data.Visible) continue;
            if (activeOnly && !data.Active) continue;
            if (!data.Mesh || !data.Mesh->mesh) continue;

            CachedMeshEntity cached;
            cached.id = id;
            cached.data = &data;
            cached.transform = &data.Transform;
            cached.mesh = data.Mesh.get();
            cached.skinning = data.Skinning.get();
            m_cached.push_back(cached);
        }

        // Sort by mesh then material for optimal batching
        std::sort(m_cached.begin(), m_cached.end(),
            [](const CachedMeshEntity& a, const CachedMeshEntity& b) {
                // Group skinned vs non-skinned
                bool aS = a.skinning != nullptr;
                bool bS = b.skinning != nullptr;
                if (aS != bS) return aS < bS;
                
                // Then by vertex buffer handle
                auto avbh = a.mesh->mesh->Dynamic ? a.mesh->mesh->dvbh.idx : a.mesh->mesh->vbh.idx;
                auto bvbh = b.mesh->mesh->Dynamic ? b.mesh->mesh->dvbh.idx : b.mesh->mesh->vbh.idx;
                if (avbh != bvbh) return avbh < bvbh;
                
                // Then by index buffer
                return a.mesh->mesh->ibh.idx < b.mesh->mesh->ibh.idx;
            });
    }

    template<typename Func>
    void ForEach(Func&& fn) {
        for (auto& cached : m_cached) {
            fn(cached);
        }
    }

    size_t Size() const { return m_cached.size(); }
    bool Empty() const { return m_cached.empty(); }

    const std::vector<CachedMeshEntity>& GetCached() const { return m_cached; }

private:
    std::vector<CachedMeshEntity> m_cached;
};

// Specialized view for light entities
class LightView {
public:
    struct CachedLight {
        EntityID id;
        const TransformComponent* transform;
        const LightComponent* light;
    };

    void Refresh(const std::vector<Entity>& entities,
                 std::unordered_map<EntityID, EntityData>& dataMap) {
        m_cached.clear();
        m_cached.reserve(16); // Typically few lights

        for (const auto& entity : entities) {
            EntityID id = entity.GetID();
            auto it = dataMap.find(id);
            if (it == dataMap.end()) continue;

            const EntityData& data = it->second;
            if (!data.Visible || !data.Active || !data.Light) continue;

            CachedLight cached;
            cached.id = id;
            cached.transform = &data.Transform;
            cached.light = data.Light.get();
            m_cached.push_back(cached);
        }
    }

    template<typename Func>
    void ForEach(Func&& fn) const {
        for (const auto& cached : m_cached) {
            fn(cached);
        }
    }

    size_t Size() const { return m_cached.size(); }

private:
    std::vector<CachedLight> m_cached;
};

// View for physics entities (rigidbodies, character controllers)
class PhysicsView {
public:
    struct CachedPhysicsEntity {
        EntityID id;
        EntityData* data;
        RigidBodyComponent* rigidBody;
        CharacterControllerComponent* characterController;
        ColliderComponent* collider;
    };

    void Refresh(const std::vector<Entity>& entities,
                 std::unordered_map<EntityID, EntityData>& dataMap) {
        m_cached.clear();
        m_cached.reserve(entities.size() / 4); // Assume 25% have physics

        for (const auto& entity : entities) {
            EntityID id = entity.GetID();
            auto it = dataMap.find(id);
            if (it == dataMap.end()) continue;

            EntityData& data = it->second;
            if (!data.Active) continue;
            if (!data.RigidBody && !data.CharacterController) continue;

            CachedPhysicsEntity cached;
            cached.id = id;
            cached.data = &data;
            cached.rigidBody = data.RigidBody.get();
            cached.characterController = data.CharacterController.get();
            cached.collider = data.Collider.get();
            m_cached.push_back(cached);
        }
    }

    template<typename Func>
    void ForEach(Func&& fn) {
        for (auto& cached : m_cached) {
            fn(cached);
        }
    }

    size_t Size() const { return m_cached.size(); }

private:
    std::vector<CachedPhysicsEntity> m_cached;
};

} // namespace ecs
} // namespace cm

