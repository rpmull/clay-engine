#include "AreaSystem.h"
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <unordered_map>
#include <iostream>
#include <shared_mutex>
#include "core/ecs/Scene.h"
#include "core/ecs/EntityData.h"
#include "core/ecs/Components.h"
#include "AreaInterop.h"
#include <glm/glm.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include "core/jobs/ParallelFor.h"
#include "core/jobs/Jobs.h"

using namespace cm::physics;

static inline bool IsAreaBody(const JPH::Body& body)
{
    // We use sensor flag to identify areas vs rigid bodies. Bodies with mIsSensor && Static/Kinematic are areas.
    return body.IsSensor() && (body.GetMotionType() != JPH::EMotionType::Dynamic);
}

// ------------------------------------------------------------
// CharacterVirtual overlap helpers (capsule vs area shapes)
// ------------------------------------------------------------
static inline glm::vec3 mat4_get_translation(const glm::mat4& m)
{
    return glm::vec3(m[3]);
}

static inline glm::quat mat4_get_rotation_quat(const glm::mat4& m)
{
    // Extract rotation ignoring potential scale/skew by normalizing basis vectors
    glm::vec3 X = glm::vec3(m[0]);
    glm::vec3 Y = glm::vec3(m[1]);
    glm::vec3 Z = glm::vec3(m[2]);
    float lx = glm::length(X), ly = glm::length(Y), lz = glm::length(Z);
    if (lx > 1e-6f) X /= lx;
    if (ly > 1e-6f) Y /= ly;
    if (lz > 1e-6f) Z /= lz;
    return glm::quat_cast(glm::mat3(X, Y, Z));
}

static inline glm::vec3 mat4_get_scale(const glm::mat4& m)
{
    // Extract scale from the lengths of the basis vectors
    return glm::vec3(
        glm::length(glm::vec3(m[0])),
        glm::length(glm::vec3(m[1])),
        glm::length(glm::vec3(m[2]))
    );
}

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

static float distance_sq_point_segment(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b)
{
    glm::vec3 ab = b - a;
    float denom = glm::dot(ab, ab);
    if (denom <= std::numeric_limits<float>::epsilon()) return glm::dot(p - a, p - a);
    float t = glm::dot(p - a, ab) / denom;
    t = clampf(t, 0.0f, 1.0f);
    glm::vec3 c = a + t * ab;
    return glm::dot(p - c, p - c);
}

static float distance_sq_segment_segment(const glm::vec3& p1, const glm::vec3& q1, const glm::vec3& p2, const glm::vec3& q2)
{
    // From Real-Time Collision Detection (Christer Ericson)
    glm::vec3 d1 = q1 - p1; // Direction vector of segment S1
    glm::vec3 d2 = q2 - p2; // Direction vector of segment S2
    glm::vec3 r = p1 - p2;
    float a = glm::dot(d1, d1); // Squared length of segment S1
    float e = glm::dot(d2, d2); // Squared length of segment S2
    float f = glm::dot(d2, r);

    float s, t;
    if (a <= std::numeric_limits<float>::epsilon() && e <= std::numeric_limits<float>::epsilon()) {
        // Both segments degenerate to points
        return glm::dot(p1 - p2, p1 - p2);
    }
    if (a <= std::numeric_limits<float>::epsilon()) {
        // First segment degenerates to a point
        s = 0.0f;
        t = clampf(f / e, 0.0f, 1.0f);
    } else {
        float c = glm::dot(d1, r);
        if (e <= std::numeric_limits<float>::epsilon()) {
            // Second segment degenerates to a point
            t = 0.0f;
            s = clampf(-c / a, 0.0f, 1.0f);
        } else {
            float b = glm::dot(d1, d2);
            float denom = a * e - b * b;
            if (denom != 0.0f)
                s = clampf((b * f - c * e) / denom, 0.0f, 1.0f);
            else
                s = 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = clampf(-c / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = clampf((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    glm::vec3 c1 = p1 + d1 * s;
    glm::vec3 c2 = p2 + d2 * t;
    return glm::dot(c1 - c2, c1 - c2);
}

static bool segment_intersects_obb(const glm::vec3& p0_world,
                                   const glm::vec3& p1_world,
                                   const glm::vec3& box_center_world,
                                   const glm::quat& box_rot_world,
                                   const glm::vec3& box_half_extents_expanded)
{
    // Transform segment into box local space
    glm::vec3 dir = p1_world - p0_world;
    glm::vec3 p0 = glm::inverse(box_rot_world) * (p0_world - box_center_world);
    glm::vec3 d = glm::inverse(box_rot_world) * dir;

    glm::vec3 minB = -box_half_extents_expanded;
    glm::vec3 maxB = box_half_extents_expanded;

    float tmin = 0.0f, tmax = 1.0f;
    for (int i = 0; i < 3; ++i) {
        float pi = (&p0.x)[i];
        float di = (&d.x)[i];
        float minI = (&minB.x)[i];
        float maxI = (&maxB.x)[i];
        if (std::abs(di) < 1e-8f) {
            if (pi < minI || pi > maxI) return false; // Parallel and outside slab
        } else {
            float inv = 1.0f / di;
            float t1 = (minI - pi) * inv;
            float t2 = (maxI - pi) * inv;
            if (t1 > t2) std::swap(t1, t2);
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            if (tmin > tmax) return false;
        }
    }
    return true;
}

static inline void compute_capsule_segment(const glm::vec3& center,
                                           const glm::vec3& up_dir_normalized,
                                           float half_height,
                                           glm::vec3& out_a,
                                           glm::vec3& out_b)
{
    out_a = center - up_dir_normalized * half_height;
    out_b = center + up_dir_normalized * half_height;
}

static EntityID GetEntityFromBody(const JPH::Body& body)
{
    return (EntityID)body.GetUserData();
}

// ============================================================================
// Area-Area intersection helpers
// ============================================================================

// Sphere vs Sphere
static bool spheres_intersect(const glm::vec3& c1, float r1, const glm::vec3& c2, float r2)
{
    float combinedRadius = r1 + r2;
    float d2 = glm::dot(c1 - c2, c1 - c2);
    return d2 <= combinedRadius * combinedRadius;
}

// Point inside OBB
static bool point_in_obb(const glm::vec3& point, const glm::vec3& boxCenter, const glm::quat& boxRot, const glm::vec3& halfExtents)
{
    glm::vec3 localPoint = glm::inverse(boxRot) * (point - boxCenter);
    return std::abs(localPoint.x) <= halfExtents.x &&
           std::abs(localPoint.y) <= halfExtents.y &&
           std::abs(localPoint.z) <= halfExtents.z;
}

// Sphere vs OBB
static bool sphere_vs_obb(const glm::vec3& sphereCenter, float sphereRadius,
                          const glm::vec3& boxCenter, const glm::quat& boxRot, const glm::vec3& halfExtents)
{
    // Transform sphere center to box local space
    glm::vec3 localSphere = glm::inverse(boxRot) * (sphereCenter - boxCenter);
    
    // Find closest point on box to sphere center
    glm::vec3 closest;
    closest.x = clampf(localSphere.x, -halfExtents.x, halfExtents.x);
    closest.y = clampf(localSphere.y, -halfExtents.y, halfExtents.y);
    closest.z = clampf(localSphere.z, -halfExtents.z, halfExtents.z);
    
    float d2 = glm::dot(localSphere - closest, localSphere - closest);
    return d2 <= sphereRadius * sphereRadius;
}

// OBB vs OBB using Separating Axis Theorem (simplified)
static bool obb_vs_obb(const glm::vec3& c1, const glm::quat& r1, const glm::vec3& h1,
                       const glm::vec3& c2, const glm::quat& r2, const glm::vec3& h2)
{
    // Get rotation matrices
    glm::mat3 rot1 = glm::mat3_cast(r1);
    glm::mat3 rot2 = glm::mat3_cast(r2);
    
    // Compute rotation matrix expressing b2 in b1's coordinate frame
    glm::mat3 R, AbsR;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            R[i][j] = glm::dot(rot1[i], rot2[j]);
    
    // Translation vector
    glm::vec3 t = c2 - c1;
    // Bring translation into b1's coordinate frame
    t = glm::vec3(glm::dot(t, rot1[0]), glm::dot(t, rot1[1]), glm::dot(t, rot1[2]));
    
    // Compute absolute rotation matrix with epsilon for parallel edges
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            AbsR[i][j] = std::abs(R[i][j]) + 1e-6f;
    
    float ra, rb;
    
    // Test axes L = A0, L = A1, L = A2
    for (int i = 0; i < 3; i++) {
        ra = (&h1.x)[i];
        rb = h2.x * AbsR[i][0] + h2.y * AbsR[i][1] + h2.z * AbsR[i][2];
        if (std::abs((&t.x)[i]) > ra + rb) return false;
    }
    
    // Test axes L = B0, L = B1, L = B2
    for (int i = 0; i < 3; i++) {
        ra = h1.x * AbsR[0][i] + h1.y * AbsR[1][i] + h1.z * AbsR[2][i];
        rb = (&h2.x)[i];
        if (std::abs(t.x * R[0][i] + t.y * R[1][i] + t.z * R[2][i]) > ra + rb) return false;
    }
    
    // Test axis L = A0 x B0 through A2 x B2 (9 cross product axes)
    // A0 x B0
    ra = h1.y * AbsR[2][0] + h1.z * AbsR[1][0];
    rb = h2.y * AbsR[0][2] + h2.z * AbsR[0][1];
    if (std::abs(t.z * R[1][0] - t.y * R[2][0]) > ra + rb) return false;
    
    // A0 x B1
    ra = h1.y * AbsR[2][1] + h1.z * AbsR[1][1];
    rb = h2.x * AbsR[0][2] + h2.z * AbsR[0][0];
    if (std::abs(t.z * R[1][1] - t.y * R[2][1]) > ra + rb) return false;
    
    // A0 x B2
    ra = h1.y * AbsR[2][2] + h1.z * AbsR[1][2];
    rb = h2.x * AbsR[0][1] + h2.y * AbsR[0][0];
    if (std::abs(t.z * R[1][2] - t.y * R[2][2]) > ra + rb) return false;
    
    // A1 x B0
    ra = h1.x * AbsR[2][0] + h1.z * AbsR[0][0];
    rb = h2.y * AbsR[1][2] + h2.z * AbsR[1][1];
    if (std::abs(t.x * R[2][0] - t.z * R[0][0]) > ra + rb) return false;
    
    // A1 x B1
    ra = h1.x * AbsR[2][1] + h1.z * AbsR[0][1];
    rb = h2.x * AbsR[1][2] + h2.z * AbsR[1][0];
    if (std::abs(t.x * R[2][1] - t.z * R[0][1]) > ra + rb) return false;
    
    // A1 x B2
    ra = h1.x * AbsR[2][2] + h1.z * AbsR[0][2];
    rb = h2.x * AbsR[1][1] + h2.y * AbsR[1][0];
    if (std::abs(t.x * R[2][2] - t.z * R[0][2]) > ra + rb) return false;
    
    // A2 x B0
    ra = h1.x * AbsR[1][0] + h1.y * AbsR[0][0];
    rb = h2.y * AbsR[2][2] + h2.z * AbsR[2][1];
    if (std::abs(t.y * R[0][0] - t.x * R[1][0]) > ra + rb) return false;
    
    // A2 x B1
    ra = h1.x * AbsR[1][1] + h1.y * AbsR[0][1];
    rb = h2.x * AbsR[2][2] + h2.z * AbsR[2][0];
    if (std::abs(t.y * R[0][1] - t.x * R[1][1]) > ra + rb) return false;
    
    // A2 x B2
    ra = h1.x * AbsR[1][2] + h1.y * AbsR[0][2];
    rb = h2.x * AbsR[2][1] + h2.y * AbsR[2][0];
    if (std::abs(t.y * R[0][2] - t.x * R[1][2]) > ra + rb) return false;
    
    // No separating axis found, boxes intersect
    return true;
}

// Capsule vs Sphere
static bool capsule_vs_sphere(const glm::vec3& capA, const glm::vec3& capB, float capRadius,
                              const glm::vec3& sphereCenter, float sphereRadius)
{
    float d2 = distance_sq_point_segment(sphereCenter, capA, capB);
    float combinedRadius = capRadius + sphereRadius;
    return d2 <= combinedRadius * combinedRadius;
}

// Capsule vs OBB (conservative using expanded OBB)
static bool capsule_vs_obb(const glm::vec3& capA, const glm::vec3& capB, float capRadius,
                           const glm::vec3& boxCenter, const glm::quat& boxRot, const glm::vec3& halfExtents)
{
    // Expand the box by capsule radius and test segment intersection
    glm::vec3 expanded = halfExtents + glm::vec3(capRadius);
    return segment_intersects_obb(capA, capB, boxCenter, boxRot, expanded);
}

// Capsule vs Capsule
static bool capsule_vs_capsule(const glm::vec3& a1, const glm::vec3& a2, float r1,
                                const glm::vec3& b1, const glm::vec3& b2, float r2)
{
    float d2 = distance_sq_segment_segment(a1, a2, b1, b2);
    float combinedRadius = r1 + r2;
    return d2 <= combinedRadius * combinedRadius;
}

// Generic area-area intersection test (takes raw parameters to avoid depending on AreaWorld struct)
// scale1/scale2 are the entity scales that must be applied to Size/Radius/Height
static bool areas_intersect(
    const glm::vec3& pos1, const glm::quat& rot1, const glm::vec3& scale1, const AreaComponent& c1,
    const glm::vec3& pos2, const glm::quat& rot2, const glm::vec3& scale2, const AreaComponent& c2)
{
    // Get shape parameters for area1 - apply entity scale
    glm::vec3 h1 = (c1.Size * scale1) * 0.5f;
    float r1 = c1.Radius * std::max({scale1.x, scale1.y, scale1.z}); // Use max scale for radius
    float halfH1 = (c1.Height * scale1.y) * 0.5f; // Height scales with Y
    glm::vec3 up1 = rot1 * glm::vec3(0, 1, 0);
    glm::vec3 seg1A, seg1B;
    if (c1.ShapeType == AreaShapeType::Capsule)
        compute_capsule_segment(pos1, glm::normalize(up1), halfH1, seg1A, seg1B);
    
    // Get shape parameters for area2 - apply entity scale
    glm::vec3 h2 = (c2.Size * scale2) * 0.5f;
    float r2 = c2.Radius * std::max({scale2.x, scale2.y, scale2.z}); // Use max scale for radius
    float halfH2 = (c2.Height * scale2.y) * 0.5f; // Height scales with Y
    glm::vec3 up2 = rot2 * glm::vec3(0, 1, 0);
    glm::vec3 seg2A, seg2B;
    if (c2.ShapeType == AreaShapeType::Capsule)
        compute_capsule_segment(pos2, glm::normalize(up2), halfH2, seg2A, seg2B);
    
    // Test based on shape types
    if (c1.ShapeType == AreaShapeType::Sphere && c2.ShapeType == AreaShapeType::Sphere)
        return spheres_intersect(pos1, r1, pos2, r2);
    
    if (c1.ShapeType == AreaShapeType::Box && c2.ShapeType == AreaShapeType::Box)
        return obb_vs_obb(pos1, rot1, h1, pos2, rot2, h2);
    
    if (c1.ShapeType == AreaShapeType::Sphere && c2.ShapeType == AreaShapeType::Box)
        return sphere_vs_obb(pos1, r1, pos2, rot2, h2);
    
    if (c1.ShapeType == AreaShapeType::Box && c2.ShapeType == AreaShapeType::Sphere)
        return sphere_vs_obb(pos2, r2, pos1, rot1, h1);
    
    if (c1.ShapeType == AreaShapeType::Capsule && c2.ShapeType == AreaShapeType::Sphere)
        return capsule_vs_sphere(seg1A, seg1B, r1, pos2, r2);
    
    if (c1.ShapeType == AreaShapeType::Sphere && c2.ShapeType == AreaShapeType::Capsule)
        return capsule_vs_sphere(seg2A, seg2B, r2, pos1, r1);
    
    if (c1.ShapeType == AreaShapeType::Capsule && c2.ShapeType == AreaShapeType::Box)
        return capsule_vs_obb(seg1A, seg1B, r1, pos2, rot2, h2);
    
    if (c1.ShapeType == AreaShapeType::Box && c2.ShapeType == AreaShapeType::Capsule)
        return capsule_vs_obb(seg2A, seg2B, r2, pos1, rot1, h1);
    
    if (c1.ShapeType == AreaShapeType::Capsule && c2.ShapeType == AreaShapeType::Capsule)
        return capsule_vs_capsule(seg1A, seg1B, r1, seg2A, seg2B, r2);
    
    return false;
}

JPH::ValidateResult AreaContactListener::OnContactValidate(const JPH::Body& inBody1, const JPH::Body& inBody2, JPH::RVec3Arg, const JPH::CollideShapeResult&)
{
    // Allow evaluation but we will not produce a physical response for sensors
    return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
}

void AreaContactListener::OnContactAdded(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold&, JPH::ContactSettings&)
{
    const bool a1 = IsAreaBody(inBody1);
    const bool a2 = IsAreaBody(inBody2);
    
    if (!a1 && !a2) return; // only track if an area is involved

    Entity area(a1 ? GetEntityFromBody(inBody1) : GetEntityFromBody(inBody2), &Scene::Get());
    Entity other(a1 ? GetEntityFromBody(inBody2) : GetEntityFromBody(inBody1), &Scene::Get());
    // If contact is Area vs inner body (user data == 0), resolve via inner-body map
    if (!(a1 && a2) && other.GetID() == 0)
    {
        const uint32_t otherIndex = (a1 ? inBody2 : inBody1).GetID().GetIndex();
        EntityID owner = 0;
        if (m_owner && m_owner->TryResolveInnerBodyOwner(otherIndex, owner) && owner != 0)
            other = Entity(owner, &Scene::Get());
    }

    // De-duplicate using our own activePairs map keyed by ordered (areaBodyIndex, otherBodyIndex)
    const EntityID areaId = area.GetID();
    const EntityID otherId = other.GetID();
    const uint32_t areaKey = (a1 ? inBody1 : inBody2).GetID().GetIndex();
    const uint32_t otherKey = (a1 ? inBody2 : inBody1).GetID().GetIndex();
    uint64_t key = ((uint64_t)areaKey << 32) ^ (uint64_t)otherKey;
    {
        std::lock_guard<std::mutex> guard(m_owner->pairsMutex);
        auto it = m_owner->activePairs.find(key);
        if (it != m_owner->activePairs.end()) return; // already active
        m_owner->activePairs[key] = { areaId, otherId, a1 && a2 };
    }

    // De-duplicate enter events using the area's overlap sets (for component-level state)
    auto* ad = Scene::Get().GetEntityData(areaId);
    if (!ad || !ad->Area) return;

    auto& comp = *ad->Area;
    if (a1 && a2) {
        // Area vs Area - handled by custom detection in OnUpdate, skip here
        // (Jolt doesn't reliably detect sensor-sensor collisions)
        return;
    } else {
        // Area vs Body
        if (!comp.MonitorBodies) return;
        std::lock_guard<std::mutex> lk(comp.Mutex);
        if (comp.OverlappingBodies.insert(otherId).second) {
            AreaEvent ev; ev.AreaId = areaId; ev.OtherId = otherId; ev.Kind = AreaEventKind::BodyEntered; buf->Push(ev);
        }
    }
}

void AreaContactListener::OnContactRemoved(const JPH::SubShapeIDPair& pair)
{
    if (!physics) return;
    // Avoid BodyLock calls here to prevent contention; derive keys from body indices
    const uint32_t b1 = pair.GetBody1ID().GetIndex();
    const uint32_t b2 = pair.GetBody2ID().GetIndex();
    uint64_t k1 = ((uint64_t)b1 << 32) ^ (uint64_t)b2;
    uint64_t k2 = ((uint64_t)b2 << 32) ^ (uint64_t)b1;

    cm::physics::AreaSystem::ActivePairInfo info{}; bool found = false; uint64_t keyFound = 0;
    {
        std::lock_guard<std::mutex> guard(m_owner->pairsMutex);
        auto it = m_owner->activePairs.find(k1);
        if (it == m_owner->activePairs.end()) it = m_owner->activePairs.find(k2);
        if (it != m_owner->activePairs.end()) { info = it->second; keyFound = it->first; m_owner->activePairs.erase(it); found = true; }
    }
    if (!found) return;

    // Update component overlap sets (best-effort; guard with component mutex and checks)
    auto* ad = Scene::Get().GetEntityData(info.areaId);
    if (ad && ad->Area) {
        std::lock_guard<std::mutex> lk(ad->Area->Mutex);
        if (info.bothAreas) {
            ad->Area->OverlappingAreas.erase(info.otherId);
            AreaEvent ev{ AreaEventKind::AreaExited, info.areaId, info.otherId };
            buf->Push(ev);
        } else {
            ad->Area->OverlappingBodies.erase(info.otherId);
            AreaEvent ev{ AreaEventKind::BodyExited, info.areaId, info.otherId };
            buf->Push(ev);
        }
    }
}

AreaSystem::AreaSystem(JPH::PhysicsSystem* phys, JPH::BodyInterface* bi)
    : physics(phys), bodyInterface(bi)
{
    // Contact listener is installed by Physics to combine area + rigidbody collisions.
}

void AreaSystem::RegisterCharacterInnerBody(JPH::BodyID innerBody, EntityID ownerEntity)
{
    std::lock_guard<std::mutex> g(m_InnerMapMutex);
    m_InnerBodyToEntity[innerBody.GetIndex()] = ownerEntity;
}

void AreaSystem::UnregisterCharacterInnerBody(JPH::BodyID innerBody)
{
    std::lock_guard<std::mutex> g(m_InnerMapMutex);
    m_InnerBodyToEntity.erase(innerBody.GetIndex());
}

bool AreaSystem::TryResolveInnerBodyOwner(uint32_t bodyIndex, EntityID& outOwner) const
{
    std::lock_guard<std::mutex> g(m_InnerMapMutex);
    auto it = m_InnerBodyToEntity.find(bodyIndex);
    if (it == m_InnerBodyToEntity.end()) return false;
    outOwner = it->second;
    return true;
}

void AreaSystem::OnCreate(Entity e, AreaComponent& area)
{
    auto* data = Scene::Get().GetEntityData(e.GetID());
    if (!data) return;

    // Clear any stale overlap data from previous play sessions
    {
        std::lock_guard<std::mutex> lk(area.Mutex);
        area.OverlappingBodies.clear();
        area.OverlappingAreas.clear();
    }

    // Build body settings - extract world transform components
    const glm::mat4& world = data->Transform.WorldMatrix * glm::translate(glm::mat4(1.0f), area.Offset);
    glm::vec3 worldScale = mat4_get_scale(data->Transform.WorldMatrix);
    
    // Build shape from AreaComponent config, applying world scale
    // Ensure scale is valid (non-zero, positive) to avoid Jolt shape creation errors
    const float kMinScale = 0.001f;
    worldScale = glm::max(worldScale, glm::vec3(kMinScale));
    
    JPH::RefConst<JPH::Shape> shape;
    JPH::Result<JPH::Ref<JPH::Shape>> shapeResult;
    
    if (area.ShapeType == AreaShapeType::Box) {
        glm::vec3 scaledSize = glm::max(area.Size * worldScale, glm::vec3(kMinScale));
        JPH::BoxShapeSettings settings(JPH::Vec3(scaledSize.x * 0.5f, scaledSize.y * 0.5f, scaledSize.z * 0.5f));
        shapeResult = settings.Create();
    } else if (area.ShapeType == AreaShapeType::Capsule) {
        // Jolt CapsuleShapeSettings takes (halfHeight, radius) - NOT (radius, halfHeight)!
        float maxScale = std::max({worldScale.x, worldScale.y, worldScale.z});
        float halfHeight = std::max(area.Height * worldScale.y * 0.5f, kMinScale);
        float radius = std::max(area.Radius * maxScale, kMinScale);
        JPH::CapsuleShapeSettings settings(halfHeight, radius);
        shapeResult = settings.Create();
    } else { // Sphere
        float maxScale = std::max({worldScale.x, worldScale.y, worldScale.z});
        float radius = std::max(area.Radius * maxScale, kMinScale);
        JPH::SphereShapeSettings settings(radius);
        shapeResult = settings.Create();
    }
    
    // Check if shape creation succeeded
    if (shapeResult.HasError()) {
        std::cerr << "[AreaSystem] Failed to create shape for entity " << e.GetID() 
                  << ": " << shapeResult.GetError() << std::endl;
        return;
    }
    shape = shapeResult.Get();

    // Extract position and rotation from world matrix
    glm::vec3 pos = glm::vec3(world[3]);
    glm::quat rot = mat4_get_rotation_quat(world);
    glm::vec4 rotVec(rot.x, rot.y, rot.z, rot.w);
    if (!glm::all(glm::isfinite(rotVec))) {
        rot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    } else {
        float len = glm::length(rot);
        if (len > 1e-6f)
            rot /= len;
        else
            rot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
    // Use Kinematic instead of Static so that Jolt will detect area-area overlaps.
    // Static-Static collision detection is not performed by Jolt (optimization).
    
    // Determine physics layer: inherit from CharacterController or RigidBody if present,
    // otherwise use Default (0). This ensures Areas on the player use the Player layer.
    JPH::ObjectLayer areaLayer = 0;
    if (data->CharacterController) {
        areaLayer = static_cast<JPH::ObjectLayer>(data->CharacterController->PhysicsLayer);
    } else if (data->RigidBody) {
        areaLayer = static_cast<JPH::ObjectLayer>(data->RigidBody->PhysicsLayer);
    }
    
    JPH::BodyCreationSettings settings(
        shape,
        JPH::RVec3(pos.x, pos.y, pos.z),
        JPH::Quat(rot.x, rot.y, rot.z, rot.w),
        JPH::EMotionType::Kinematic,
        areaLayer
    );
    settings.mIsSensor = true;
    JPH::Body* body = bodyInterface->CreateBody(settings);
    if (!body) return;
    body->SetUserData((uint64_t)e.GetID());
    bodyInterface->AddBody(body->GetID(), JPH::EActivation::Activate);
    area.Body = body;
    area.Dirty = false;
}

void AreaSystem::OnDestroy(Entity e, AreaComponent& area)
{
    if (area.Body) {
        JPH::BodyID id = area.Body->GetID();
        bodyInterface->RemoveBody(id);
        bodyInterface->DestroyBody(id);
        area.Body = nullptr;
    }

    {
        std::lock_guard<std::mutex> lk(area.Mutex);
        area.OverlappingBodies.clear();
        area.OverlappingAreas.clear();
    }

    {
        std::lock_guard<std::mutex> guard(pairsMutex);
        for (auto it = activePairs.begin(); it != activePairs.end(); ) {
            if (it->second.areaId == e.GetID() || it->second.otherId == e.GetID()) {
                it = activePairs.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void AreaSystem::OnUpdate(float dt)
{
    (void)dt;
    // CharacterVirtual overlap detection against Areas
    auto& scene = Scene::Get();
    
    const auto& entities = scene.GetEntities();
    const size_t entityCount = entities.size();
    
    // OPTIMIZATION: Increment frame counter and check if full refresh is needed
    ++m_FrameCounter;
    bool forceFullRefresh = (m_FrameCounter >= kFullRefreshInterval);
    if (forceFullRefresh) m_FrameCounter = 0;
    
    // Quick entity count check - if no entities or no areas were found before and count unchanged, early out
    if (!forceFullRefresh && entityCount == m_LastEntityCount && 
        m_FrameAreas.empty() && m_FrameCharacters.empty() && m_FrameKinematicBodies.empty()) {
        return; // Nothing to do
    }
    m_LastEntityCount = entityCount;

    // 1) Gather pass: build SoA-like arrays for areas and characters (zero-alloc via reused members)
    m_FrameAreas.clear();
    m_FrameCharacters.clear();
    m_FrameAreas.reserve(64);
    m_FrameCharacters.reserve(64);

    // Gather areas (for body monitoring)
    static bool loggedAreaCount = false;
    int totalAreasFound = 0;
    for (const auto& ent : entities)
    {
        auto* data = scene.GetEntityData(ent.GetID());
        if (!data || !data->Visible || !data->Area) continue;
        AreaComponent* comp = data->Area.get();
        totalAreasFound++;
        if (!comp->Enabled) continue;

        const glm::mat4 areaWorld = data->Transform.WorldMatrix * glm::translate(glm::mat4(1.0f), comp->Offset);
        AreaWorld aw;
        aw.id = ent.GetID();
        aw.position = mat4_get_translation(areaWorld);
        aw.rotation = mat4_get_rotation_quat(areaWorld);
        aw.scale = mat4_get_scale(data->Transform.WorldMatrix);  // Extract scale for intersection tests
        aw.comp = comp;
        m_FrameAreas.push_back(aw);
    }
    if (!loggedAreaCount && totalAreasFound > 0) {
        std::cout << "[AreaSystem] Found " << totalAreasFound << " total areas, " << m_FrameAreas.size() << " enabled" << std::endl;
        loggedAreaCount = true;
    }

    // Build set of currently active area IDs for cleanup pass
    std::unordered_set<EntityID> activeAreaIds;
    for (const auto& aw : m_FrameAreas) {
        activeAreaIds.insert(aw.id);
    }

    // Cleanup pass: remove overlaps from areas that are no longer active/visible
    // This handles cases where an area gets disabled, destroyed, or hidden
    for (const auto& aw : m_FrameAreas)
    {
        AreaComponent& area = *aw.comp;
        
        // Check OverlappingAreas for stale entries
        if (area.MonitorAreas)
        {
            std::vector<EntityID> toRemove;
            {
                std::lock_guard<std::mutex> lk(area.Mutex);
                for (EntityID otherId : area.OverlappingAreas) {
                    if (activeAreaIds.find(otherId) == activeAreaIds.end()) {
                        toRemove.push_back(otherId);
                    }
                }
                for (EntityID id : toRemove) {
                    area.OverlappingAreas.erase(id);
                }
            }
            // Fire exit events for removed overlaps
            for (EntityID otherId : toRemove) {
                AreaEvent ev{ AreaEventKind::AreaExited, aw.id, otherId };
                buffer.Push(ev);
            }
        }
        
        // Check OverlappingBodies for stale entries (characters that left the scene)
        if (area.MonitorBodies)
        {
            std::vector<EntityID> toRemove;
            {
                std::lock_guard<std::mutex> lk(area.Mutex);
                for (EntityID otherId : area.OverlappingBodies) {
                    // Check if this body/character still exists and is visible
                    auto* otherData = scene.GetEntityData(otherId);
                    bool stillValid = otherData && otherData->Visible && 
                                     (otherData->CharacterController || otherData->RigidBody);
                    if (!stillValid) {
                        toRemove.push_back(otherId);
                    }
                }
                for (EntityID id : toRemove) {
                    area.OverlappingBodies.erase(id);
                }
            }
            // Fire exit events for removed overlaps
            for (EntityID otherId : toRemove) {
                AreaEvent ev{ AreaEventKind::BodyExited, aw.id, otherId };
                buffer.Push(ev);
            }
        }
    }

    // Gather character capsules
    for (const auto& ent : entities)
    {
        auto* data = scene.GetEntityData(ent.GetID());
        if (!data || !data->Visible || !data->CharacterController) continue;
        auto& cc = *data->CharacterController;
        if (!cc.Character) continue;

        JPH::RMat44 w = cc.Character->GetWorldTransform();
        glm::vec3 charCenter((float)w(0,3), (float)w(1,3), (float)w(2,3));
        glm::vec3 up = glm::normalize(cc.Up);
        if (!glm::all(glm::isfinite(up)) || glm::length(up) < 1e-4f) up = glm::vec3(0.0f, 1.0f, 0.0f);
        float charRadius = std::max(0.0f, cc.Radius);
        float charHalfHeight = std::max(0.0f, cc.Height * 0.5f);
        glm::vec3 segA, segB;
        compute_capsule_segment(charCenter, up, charHalfHeight, segA, segB);

        CharacterCapsule c{};
        c.id = ent.GetID();
        c.segA = segA;
        c.segB = segB;
        c.radius = charRadius;
        m_FrameCharacters.push_back(c);
    }

    // Gather kinematic rigidbodies (Jolt doesn't detect kinematic vs kinematic sensor collisions)
    m_FrameKinematicBodies.clear();
    m_FrameKinematicBodies.reserve(32);
    for (const auto& ent : entities)
    {
        auto* data = scene.GetEntityData(ent.GetID());
        if (!data || !data->Visible) continue;
        // Only kinematic rigidbodies with colliders (dynamic bodies are detected by Jolt contact listener)
        if (!data->RigidBody || !data->RigidBody->IsKinematic || !data->Collider) continue;
        if (data->RigidBody->BodyID.IsInvalid()) continue;

        auto& col = *data->Collider;
        glm::vec3 pos = glm::vec3(data->Transform.WorldMatrix[3]);
        glm::quat rot = mat4_get_rotation_quat(data->Transform.WorldMatrix);
        glm::vec3 scale = glm::vec3(
            glm::length(glm::vec3(data->Transform.WorldMatrix[0])),
            glm::length(glm::vec3(data->Transform.WorldMatrix[1])),
            glm::length(glm::vec3(data->Transform.WorldMatrix[2]))
        );

        RigidBodyShape rb{};
        rb.id = ent.GetID();
        rb.position = pos + rot * col.Offset;
        rb.rotation = rot;
        rb.shapeType = col.ShapeType;

        switch (col.ShapeType) {
            case ColliderShape::Capsule: {
                float maxScaleXZ = std::max(scale.x, scale.z);
                rb.radius = col.Radius * maxScaleXZ;
                float halfHeight = col.Height * scale.y * 0.5f;
                glm::vec3 up = rot * glm::vec3(0.0f, 1.0f, 0.0f);
                // Capsule with feet at origin (matching debug draw)
                float feetToCenter = halfHeight + rb.radius;
                glm::vec3 center = rb.position + up * feetToCenter;
                compute_capsule_segment(center, glm::normalize(up), halfHeight, rb.segA, rb.segB);
                break;
            }
            case ColliderShape::Sphere: {
                float avgScale = (scale.x + scale.y + scale.z) / 3.0f;
                rb.radius = col.Radius * avgScale;
                rb.segA = rb.segB = rb.position;  // point for sphere
                break;
            }
            case ColliderShape::Box: {
                rb.halfExtents = col.Size * scale * 0.5f;
                break;
            }
            default:
                continue;  // Skip mesh colliders etc.
        }
        m_FrameKinematicBodies.push_back(rb);
    }
    
    // OPTIMIZATION: Track counts for change detection
    m_LastAreaCount = m_FrameAreas.size();
    m_LastCharacterCount = m_FrameCharacters.size();
    m_LastKinematicCount = m_FrameKinematicBodies.size();
    
    // Early out if nothing to check
    if (m_FrameAreas.empty()) return;

    // 2) Character vs Area overlaps
    if (!m_FrameAreas.empty() && !m_FrameCharacters.empty())
    {
        // Partition areas to keep per-task work coarse
        const size_t count = m_FrameAreas.size();
        const size_t chunk = std::max<size_t>(8, count / 4);
        parallel_for(Jobs(), size_t{0}, count, chunk, [&](size_t s, size_t c){
            for (size_t i = 0; i < c; ++i)
            {
                const AreaWorld& aw = m_FrameAreas[s + i];
                AreaComponent& area = *aw.comp;
                if (!area.MonitorBodies) continue;  // Skip if not monitoring bodies

                for (const CharacterCapsule& ch : m_FrameCharacters)
                {
                    // Skip self-detection: if the area belongs to the same entity as the character
                    if (aw.id == ch.id) continue;
                    
                    bool intersects = false;
                    // Apply entity scale to area dimensions
                    float maxScale = std::max({aw.scale.x, aw.scale.y, aw.scale.z});
                    
                    switch (area.ShapeType)
                    {
                        case AreaShapeType::Sphere:
                        {
                            float scaledRadius = area.Radius * maxScale;
                            float expanded = scaledRadius + ch.radius;
                            float d2 = distance_sq_point_segment(aw.position, ch.segA, ch.segB);
                            intersects = (d2 <= expanded * expanded);
                            break;
                        }
                        case AreaShapeType::Box:
                        {
                            glm::vec3 scaledSize = area.Size * aw.scale;
                            glm::vec3 halfExt = glm::max(scaledSize * 0.5f, glm::vec3(0.0f));
                            glm::vec3 expanded = halfExt + glm::vec3(ch.radius);
                            intersects = segment_intersects_obb(ch.segA, ch.segB, aw.position, aw.rotation, expanded);
                            break;
                        }
                        case AreaShapeType::Capsule:
                        {
                            float scaledHeight = area.Height * aw.scale.y;
                            float scaledRadius = area.Radius * maxScale;
                            float areaHalf = std::max(0.0f, scaledHeight * 0.5f);
                            glm::vec3 capUp = aw.rotation * glm::vec3(0.0f, 1.0f, 0.0f);
                            glm::vec3 aA, aB;
                            compute_capsule_segment(aw.position, glm::normalize(capUp), areaHalf, aA, aB);
                            float r = scaledRadius + ch.radius;
                            float d2 = distance_sq_segment_segment(aA, aB, ch.segA, ch.segB);
                            intersects = (d2 <= r * r);
                            break;
                        }
                    }

                    const EntityID areaId = aw.id;
                    const EntityID otherId = ch.id;

                    if (intersects)
                    {
                        std::lock_guard<std::mutex> lk(area.Mutex);
                        if (area.OverlappingBodies.insert(otherId).second)
                        {
                            AreaEvent ev; ev.AreaId = areaId; ev.OtherId = otherId; ev.Kind = AreaEventKind::BodyEntered; buffer.Push(ev);
                        }
                    }
                    else
                    {
                        bool wasOverlapping = false;
                        {
                            std::lock_guard<std::mutex> lk(area.Mutex);
                            auto it = area.OverlappingBodies.find(otherId);
                            if (it != area.OverlappingBodies.end()) { area.OverlappingBodies.erase(it); wasOverlapping = true; }
                        }
                        if (wasOverlapping)
                        {
                            AreaEvent ev{ AreaEventKind::BodyExited, areaId, otherId };
                            buffer.Push(ev);
                        }
                    }
                }
            }
        });
    }

    // 2b) Kinematic RigidBody vs Area overlaps (Jolt doesn't detect kinematic vs kinematic sensor)
    if (!m_FrameAreas.empty() && !m_FrameKinematicBodies.empty())
    {
        for (const AreaWorld& aw : m_FrameAreas)
        {
            AreaComponent& area = *aw.comp;
            if (!area.MonitorBodies) continue;

            for (const RigidBodyShape& rb : m_FrameKinematicBodies)
            {
                // Skip self-detection
                if (aw.id == rb.id) continue;

                bool intersects = false;
                float maxScale = std::max({aw.scale.x, aw.scale.y, aw.scale.z});

                // Detect based on rigidbody collider shape
                switch (rb.shapeType)
                {
                    case ColliderShape::Capsule:
                    case ColliderShape::Sphere:
                    {
                        // Treat as capsule/point with radius
                        switch (area.ShapeType)
                        {
                            case AreaShapeType::Sphere:
                            {
                                float scaledRadius = area.Radius * maxScale;
                                float expanded = scaledRadius + rb.radius;
                                float d2 = distance_sq_point_segment(aw.position, rb.segA, rb.segB);
                                intersects = (d2 <= expanded * expanded);
                                break;
                            }
                            case AreaShapeType::Box:
                            {
                                glm::vec3 scaledSize = area.Size * aw.scale;
                                glm::vec3 halfExt = glm::max(scaledSize * 0.5f, glm::vec3(0.0f));
                                glm::vec3 expanded = halfExt + glm::vec3(rb.radius);
                                intersects = segment_intersects_obb(rb.segA, rb.segB, aw.position, aw.rotation, expanded);
                                break;
                            }
                            case AreaShapeType::Capsule:
                            {
                                float scaledHeight = area.Height * aw.scale.y;
                                float scaledRadius = area.Radius * maxScale;
                                float areaHalf = std::max(0.0f, scaledHeight * 0.5f);
                                glm::vec3 capUp = aw.rotation * glm::vec3(0.0f, 1.0f, 0.0f);
                                glm::vec3 aA, aB;
                                compute_capsule_segment(aw.position, glm::normalize(capUp), areaHalf, aA, aB);
                                float r = scaledRadius + rb.radius;
                                float d2 = distance_sq_segment_segment(aA, aB, rb.segA, rb.segB);
                                intersects = (d2 <= r * r);
                                break;
                            }
                        }
                        break;
                    }
                    case ColliderShape::Box:
                    {
                        // Box vs Area - simplified AABB check for now
                        switch (area.ShapeType)
                        {
                            case AreaShapeType::Sphere:
                            {
                                float scaledRadius = area.Radius * maxScale;
                                // Check if sphere center is within expanded box
                                glm::vec3 expanded = rb.halfExtents + glm::vec3(scaledRadius);
                                glm::vec3 local = glm::inverse(rb.rotation) * (aw.position - rb.position);
                                intersects = glm::all(glm::lessThanEqual(glm::abs(local), expanded));
                                break;
                            }
                            case AreaShapeType::Box:
                            {
                                // Box vs Box - use separating axis theorem (simplified AABB)
                                glm::vec3 scaledSize = area.Size * aw.scale;
                                glm::vec3 areaHalf = scaledSize * 0.5f;
                                glm::vec3 diff = glm::abs(aw.position - rb.position);
                                glm::vec3 sumHalf = areaHalf + rb.halfExtents;
                                intersects = glm::all(glm::lessThanEqual(diff, sumHalf));
                                break;
                            }
                            case AreaShapeType::Capsule:
                            {
                                // Capsule vs Box - expand box by capsule radius and check segment
                                float scaledHeight = area.Height * aw.scale.y;
                                float scaledRadius = area.Radius * maxScale;
                                float areaHalf = std::max(0.0f, scaledHeight * 0.5f);
                                glm::vec3 capUp = aw.rotation * glm::vec3(0.0f, 1.0f, 0.0f);
                                glm::vec3 aA, aB;
                                compute_capsule_segment(aw.position, glm::normalize(capUp), areaHalf, aA, aB);
                                glm::vec3 expanded = rb.halfExtents + glm::vec3(scaledRadius);
                                intersects = segment_intersects_obb(aA, aB, rb.position, rb.rotation, expanded);
                                break;
                            }
                        }
                        break;
                    }
                    default:
                        break;
                }

                const EntityID areaId = aw.id;
                const EntityID otherId = rb.id;

                if (intersects)
                {
                    std::lock_guard<std::mutex> lk(area.Mutex);
                    if (area.OverlappingBodies.insert(otherId).second)
                    {
                        AreaEvent ev; ev.AreaId = areaId; ev.OtherId = otherId; ev.Kind = AreaEventKind::BodyEntered; buffer.Push(ev);
                    }
                }
                else
                {
                    bool wasOverlapping = false;
                    {
                        std::lock_guard<std::mutex> lk(area.Mutex);
                        auto it = area.OverlappingBodies.find(otherId);
                        if (it != area.OverlappingBodies.end()) { area.OverlappingBodies.erase(it); wasOverlapping = true; }
                    }
                    if (wasOverlapping)
                    {
                        AreaEvent ev{ AreaEventKind::BodyExited, areaId, otherId };
                        buffer.Push(ev);
                    }
                }
            }
        }
    }

    // 3) Area vs Area overlaps (custom detection since Jolt doesn't detect sensor-sensor collisions)
    static bool loggedOnce = false;
    if (m_FrameAreas.size() >= 2)
    {
        if (!loggedOnce) {
            std::cout << "[AreaSystem] Area-Area detection running with " << m_FrameAreas.size() << " areas" << std::endl;
            loggedOnce = true;
        }
        const size_t areaCount = m_FrameAreas.size();
        std::vector<float> boundRadius(areaCount, 1.0f);
        float maxRadius = 1.0f;
        for (size_t i = 0; i < areaCount; ++i) {
            const AreaWorld& area = m_FrameAreas[i];
            const AreaComponent& comp = *area.comp;
            const float maxScale = std::max({area.scale.x, area.scale.y, area.scale.z});
            float radius = 1.0f;
            switch (comp.ShapeType) {
                case AreaShapeType::Sphere:
                    radius = std::max(0.1f, comp.Radius * maxScale);
                    break;
                case AreaShapeType::Box: {
                    glm::vec3 scaledHalf = glm::max(comp.Size * area.scale * 0.5f, glm::vec3(0.0f));
                    radius = std::max(0.1f, glm::length(scaledHalf));
                    break;
                }
                case AreaShapeType::Capsule: {
                    float h = std::max(0.0f, comp.Height * area.scale.y * 0.5f);
                    float r = std::max(0.0f, comp.Radius * maxScale);
                    radius = std::max(0.1f, h + r);
                    break;
                }
            }
            boundRadius[i] = radius;
            maxRadius = std::max(maxRadius, radius);
        }

        const float cellSize = std::max(1.0f, maxRadius * 2.0f);
        auto cellCoord = [&](const glm::vec3& p) -> glm::ivec3 {
            return glm::ivec3(
                static_cast<int>(std::floor(p.x / cellSize)),
                static_cast<int>(std::floor(p.y / cellSize)),
                static_cast<int>(std::floor(p.z / cellSize)));
        };
        auto hashCell = [](const glm::ivec3& c) -> int64_t {
            return (static_cast<int64_t>(c.x) << 42) ^
                   (static_cast<int64_t>(c.y) << 21) ^
                   static_cast<int64_t>(c.z);
        };

        std::vector<glm::ivec3> areaCells(areaCount);
        std::unordered_map<int64_t, std::vector<size_t>> areaGrid;
        areaGrid.reserve(areaCount * 2);
        for (size_t i = 0; i < areaCount; ++i) {
            areaCells[i] = cellCoord(m_FrameAreas[i].position);
            areaGrid[hashCell(areaCells[i])].push_back(i);
        }

        auto updateAreaOverlapState = [&](const AreaWorld& areaA, const AreaWorld& areaB, bool intersects) {
            const EntityID areaId = areaA.id;
            const EntityID otherId = areaB.id;
            if (intersects) {
                std::lock_guard<std::mutex> lk(areaA.comp->Mutex);
                if (areaA.comp->OverlappingAreas.insert(otherId).second) {
                    AreaEvent ev{ AreaEventKind::AreaEntered, areaId, otherId };
                    buffer.Push(ev);
                }
            } else {
                bool wasOverlapping = false;
                {
                    std::lock_guard<std::mutex> lk(areaA.comp->Mutex);
                    auto it = areaA.comp->OverlappingAreas.find(otherId);
                    if (it != areaA.comp->OverlappingAreas.end()) {
                        areaA.comp->OverlappingAreas.erase(it);
                        wasOverlapping = true;
                    }
                }
                if (wasOverlapping) {
                    AreaEvent ev{ AreaEventKind::AreaExited, areaId, otherId };
                    buffer.Push(ev);
                }
            }
        };

        for (size_t i = 0; i < areaCount; ++i) {
            const AreaWorld& area1 = m_FrameAreas[i];
            if (!area1.comp->MonitorAreas) continue;

            const glm::ivec3 c = areaCells[i];
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const glm::ivec3 nc(c.x + dx, c.y + dy, c.z + dz);
                        auto itCell = areaGrid.find(hashCell(nc));
                        if (itCell == areaGrid.end()) continue;

                        for (size_t j : itCell->second) {
                            if (j <= i) continue;
                            const AreaWorld& area2 = m_FrameAreas[j];
                            if (area1.id == area2.id) continue;

                            const glm::vec3 delta = area1.position - area2.position;
                            const float maxDist = boundRadius[i] + boundRadius[j];
                            if (glm::dot(delta, delta) > (maxDist * maxDist)) {
                                updateAreaOverlapState(area1, area2, false);
                                if (area2.comp->MonitorAreas) {
                                    updateAreaOverlapState(area2, area1, false);
                                }
                                continue;
                            }

                            const bool intersects = areas_intersect(
                                area1.position, area1.rotation, area1.scale, *area1.comp,
                                area2.position, area2.rotation, area2.scale, *area2.comp);
                            updateAreaOverlapState(area1, area2, intersects);
                            if (area2.comp->MonitorAreas) {
                                updateAreaOverlapState(area2, area1, intersects);
                            }
                        }
                    }
                }
            }
        }
    }
}


void AreaSystem::DispatchEventsToInterop()
{
    std::vector<AreaEvent> local;
    buffer.SwapTo(local);

    for (const auto& ev : local) {
        int kind = 0;
        switch (ev.Kind) {
            case AreaEventKind::BodyEntered: kind = 0; break;
            case AreaEventKind::BodyExited:  kind = 1; break;
            case AreaEventKind::AreaEntered: kind = 2; break;
            case AreaEventKind::AreaExited:  kind = 3; break;
        }
        AreaInterop_Dispatch(kind, (int)ev.AreaId, (int)ev.OtherId);
    }
}

void AreaSystem::DetachListener()
{
    if (physics) {
        // Clear listener pointer to prevent callbacks during destruction
        physics->SetContactListener(nullptr);
    }
}


