#pragma once
#include <cstdint>
#include <vector>
#include <unordered_set>
#include <mutex>
#include "core/ecs/Entity.h"
#include <glm/glm.hpp>

namespace JPH { class Body; class Shape; }

namespace cm::physics {

enum class AreaShapeType : uint8_t {
    Box = 0,
    Capsule = 1,
    Sphere = 2,
};

enum class AreaSpaceEffect : uint8_t {
    None    = 0,
    Gravity = 1 << 0,
    LinDamp = 1 << 1,
    AngDamp = 1 << 2,
};
inline AreaSpaceEffect operator|(AreaSpaceEffect a, AreaSpaceEffect b){ return (AreaSpaceEffect)((uint8_t)a | (uint8_t)b); }
inline bool Has(AreaSpaceEffect f, AreaSpaceEffect m){ return (((uint8_t)f & (uint8_t)m) != 0); }

struct AreaComponent
{
    AreaComponent() = default;
    AreaComponent(const AreaComponent& other)
    {
        Enabled = other.Enabled;
        MonitorBodies = other.MonitorBodies;
        MonitorAreas = other.MonitorAreas;

        ShapeType = other.ShapeType;
        Offset = other.Offset;
        Size = other.Size;
        Radius = other.Radius;
        Height = other.Height;

        CollisionLayer = other.CollisionLayer;
        CollisionMask = other.CollisionMask;

        Effects = other.Effects;
        GravityOverride = other.GravityOverride;
        LinearDamp = other.LinearDamp;
        AngularDamp = other.AngularDamp;
        Priority = other.Priority;

        // Do not copy runtime state
        Body = nullptr;
        Dirty = true;
        OverlappingBodies.clear();
        OverlappingAreas.clear();
        Shapes.clear();
    }

    AreaComponent& operator=(const AreaComponent& other)
    {
        if (this == &other) return *this;
        Enabled = other.Enabled;
        MonitorBodies = other.MonitorBodies;
        MonitorAreas = other.MonitorAreas;

        ShapeType = other.ShapeType;
        Offset = other.Offset;
        Size = other.Size;
        Radius = other.Radius;
        Height = other.Height;

        CollisionLayer = other.CollisionLayer;
        CollisionMask = other.CollisionMask;

        Effects = other.Effects;
        GravityOverride = other.GravityOverride;
        LinearDamp = other.LinearDamp;
        AngularDamp = other.AngularDamp;
        Priority = other.Priority;

        Body = nullptr;
        Dirty = true;
        OverlappingBodies.clear();
        OverlappingAreas.clear();
        Shapes.clear();
        return *this;
    }

    bool Enabled = true;
    bool MonitorBodies = true;
    bool MonitorAreas = true;

    // Shape config
    AreaShapeType ShapeType = AreaShapeType::Box;
    glm::vec3 Offset = glm::vec3(0.0f);
    glm::vec3 Size   = glm::vec3(1.0f);   // Box
    float Radius     = 0.5f;              // Capsule
    float Height     = 1.0f;              // Capsule (total height)

    uint32_t CollisionLayer = 1u << 3;
    uint32_t CollisionMask  = 0xFFFFFFFFu;

    AreaSpaceEffect Effects = AreaSpaceEffect::None;
    float GravityOverride   = 0.0f;
    float LinearDamp        = 0.0f;
    float AngularDamp       = 0.0f;
    int   Priority          = 0;

    // Optional: explicit shapes (unused for now)
    std::vector<JPH::Shape*> Shapes;

    // Runtime
    JPH::Body* Body = nullptr;
    bool Dirty = true;

    std::unordered_set<EntityID> OverlappingBodies;
    std::unordered_set<EntityID> OverlappingAreas;
    mutable std::mutex Mutex;
};

} // namespace cm::physics


