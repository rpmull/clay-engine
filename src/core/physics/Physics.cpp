// Physics.cpp
#include "Physics.h"
#include "PhysicsDebug.h"
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <iostream>
#include <memory>
#include <Jolt/Math/Mat44.h>
#include <glm/gtc/type_ptr.hpp> // for glm::value_ptr
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include "core/physics/area/AreaSystem.h"
#include "core/physics/collision/CollisionSystem.h"
#include "core/physics/ragdoll/RagdollSystem.h"
#include "core/ecs/Scene.h"
#include "core/ecs/EntityData.h"

// Include the layer manager
#include "PhysicsLayerManager.h"

// MAX_PHYSICS_LAYERS is now defined in Physics.h

// --- Static member definitions ---
// Public Jolt members
JPH::TempAllocatorImpl* Physics::s_TempAllocator = nullptr;
JPH::JobSystemThreadPool* Physics::s_JobSystem = nullptr;
JPH::PhysicsSystem* Physics::s_PhysicsSystem = nullptr;
cm::physics::AreaSystem* Physics::s_AreaSystem = nullptr;
cm::physics::RigidBodyCollisionSystem* Physics::s_CollisionSystem = nullptr;

// Your custom classes used for filtering
BroadPhaseLayerInterfaceImpl* Physics::s_BroadPhaseInterface = nullptr;
ObjectVsBroadPhaseLayerFilterImpl* Physics::s_ObjectVsBroadPhaseFilter = nullptr;
ObjectLayerPairFilterImpl* Physics::s_ObjectLayerPairFilter = nullptr;


class ObjectLayerPairFilterImpl : public JPH::ObjectLayerPairFilter {
public:
    virtual bool ShouldCollide(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const override {
        return true; // Allow everything for now
    }
};

class ObjectVsBroadPhaseLayerFilterImpl : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    virtual bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override {
        return true; // Allow everything for now
    }
};

// Broad phase layers (coarse grouping for broad phase)
namespace BroadPhaseLayers {
    constexpr JPH::BroadPhaseLayer NON_MOVING(0);
    constexpr JPH::BroadPhaseLayer MOVING(1);
    constexpr uint32_t NUM_LAYERS = 2;
}

// Maps object layers to broad phase layers
// Since layers are now user-defined, we use a simple heuristic:
// The broad phase layer is determined by motion type when the body is created,
// not by the object layer. All layers map to MOVING by default (safer for collision).
class BroadPhaseLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
    BroadPhaseLayerInterfaceImpl() {
        // Default all layers to MOVING - the actual broad phase is determined
        // by the body's motion type (static vs dynamic) at creation time
        for (uint32_t i = 0; i < MAX_PHYSICS_LAYERS; i++) {
            mObjectToBroadPhase[i] = BroadPhaseLayers::MOVING;
        }
    }

    virtual JPH::uint GetNumBroadPhaseLayers() const override {
        return BroadPhaseLayers::NUM_LAYERS;
    }

    virtual JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override {
        if (inLayer < MAX_PHYSICS_LAYERS)
            return mObjectToBroadPhase[inLayer];
        return BroadPhaseLayers::MOVING;
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    virtual const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override {
        switch ((JPH::BroadPhaseLayer::Type)inLayer) {
        case 0: return "NonMoving";
        case 1: return "Moving";
        default: return "Unknown";
        }
    }
#endif

private:
    JPH::BroadPhaseLayer mObjectToBroadPhase[MAX_PHYSICS_LAYERS];
};

// Body filter for raycast layer masking
class LayerMaskBodyFilter : public JPH::BodyFilter {
public:
    LayerMaskBodyFilter(uint32_t layerMask, JPH::PhysicsSystem* system) 
        : mLayerMask(layerMask), mSystem(system) {}
    
    virtual bool ShouldCollide(const JPH::BodyID& inBodyID) const override {
        JPH::BodyLockRead lock(mSystem->GetBodyLockInterface(), inBodyID);
        if (lock.Succeeded()) {
            JPH::ObjectLayer layer = lock.GetBody().GetObjectLayer();
            // Check if this layer's bit is set in the mask
            return layer < MAX_PHYSICS_LAYERS && (mLayerMask & (1u << layer)) != 0;
        }
        return true; // If we can't read the body, allow collision
    }
    
    virtual bool ShouldCollideLocked(const JPH::Body& inBody) const override {
        JPH::ObjectLayer layer = inBody.GetObjectLayer();
        return layer < MAX_PHYSICS_LAYERS && (mLayerMask & (1u << layer)) != 0;
    }

private:
    uint32_t mLayerMask;
    JPH::PhysicsSystem* mSystem;
};

namespace {

static EntityID ResolveCollisionMaskEntity(const JPH::Body& body)
{
    EntityID id = static_cast<EntityID>(body.GetUserData());
    if (id == 0) {
        if (auto* areaSystem = Physics::Get().GetAreaSystem()) {
            EntityID owner = 0;
            if (areaSystem->TryResolveInnerBodyOwner(body.GetID().GetIndex(), owner) && owner != 0) {
                id = owner;
            }
        }
    }
    return id;
}

static bool RigidBodyCollisionMaskAllows(EntityID entityId, JPH::ObjectLayer otherLayer)
{
    if (otherLayer >= MAX_PHYSICS_LAYERS) {
        return true;
    }

    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->RigidBody) {
        return true;
    }

    const uint32_t otherLayerMask = 1u << static_cast<uint32_t>(otherLayer);
    return (data->RigidBody->CollisionMask & otherLayerMask) != 0;
}

static bool BodiesPassRigidBodyCollisionMasks(const JPH::Body& body1, const JPH::Body& body2)
{
    const EntityID id1 = ResolveCollisionMaskEntity(body1);
    const EntityID id2 = ResolveCollisionMaskEntity(body2);

    if (id1 != 0 && !RigidBodyCollisionMaskAllows(id1, body2.GetObjectLayer())) {
        return false;
    }

    if (id2 != 0 && !RigidBodyCollisionMaskAllows(id2, body1.GetObjectLayer())) {
        return false;
    }

    return true;
}

class PhysicsContactListener final : public JPH::ContactListener {
public:
    PhysicsContactListener(JPH::PhysicsSystem* sys,
                           cm::physics::AreaSystem* areaSystem,
                           cm::physics::RigidBodyCollisionSystem* collisionSystem)
        : m_areaListener(sys, areaSystem->GetEventBuffer(), areaSystem),
          m_collisionSystem(collisionSystem)
    {
    }

    JPH::ValidateResult OnContactValidate(const JPH::Body& inBody1, const JPH::Body& inBody2, JPH::RVec3Arg inBaseOffset, const JPH::CollideShapeResult& inCollisionResult) override
    {
        if (!BodiesPassRigidBodyCollisionMasks(inBody1, inBody2)) {
            return JPH::ValidateResult::RejectAllContactsForThisBodyPair;
        }
        return m_areaListener.OnContactValidate(inBody1, inBody2, inBaseOffset, inCollisionResult);
    }

    void OnContactAdded(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold& inManifold, JPH::ContactSettings& ioSettings) override
    {
        m_areaListener.OnContactAdded(inBody1, inBody2, inManifold, ioSettings);
        if (m_collisionSystem) {
            m_collisionSystem->OnContactAdded(inBody1, inBody2, inManifold, ioSettings);
        }
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& inSubShapePair) override
    {
        m_areaListener.OnContactRemoved(inSubShapePair);
        if (m_collisionSystem) {
            m_collisionSystem->OnContactRemoved(inSubShapePair);
        }
    }

private:
    cm::physics::AreaContactListener m_areaListener;
    cm::physics::RigidBodyCollisionSystem* m_collisionSystem;
};

static std::unique_ptr<PhysicsContactListener> s_ContactListener;

} // namespace


void Physics::Init() {
    JPH::RegisterDefaultAllocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();

    s_TempAllocator = new JPH::TempAllocatorImpl(10 * 1024 * 1024);
    uint32_t hardwareThreads = JPH::thread::hardware_concurrency();
    uint32_t workerThreads = hardwareThreads > 1 ? hardwareThreads - 1 : 1;
    s_JobSystem = new JPH::JobSystemThreadPool(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, workerThreads);

    s_BroadPhaseInterface = new BroadPhaseLayerInterfaceImpl();
    s_ObjectVsBroadPhaseFilter = new ObjectVsBroadPhaseLayerFilterImpl();
    s_ObjectLayerPairFilter = new ObjectLayerPairFilterImpl();

    s_PhysicsSystem = new JPH::PhysicsSystem();
    s_PhysicsSystem->Init(
        1024, 0, 1024, 1024,
        *s_BroadPhaseInterface,
        *s_ObjectVsBroadPhaseFilter,
        *s_ObjectLayerPairFilter
    );

    // Set gravity explicitly (Jolt defaults to (0, -9.81, 0) but let's be explicit)
    s_PhysicsSystem->SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));

    std::cout << "[Physics] Jolt Physics initialized with gravity (0, -9.81, 0).\n";

    // Install area system (sensor contacts)
    s_AreaSystem = new cm::physics::AreaSystem(s_PhysicsSystem, &s_PhysicsSystem->GetBodyInterface());

    // Install rigidbody collision system
    s_CollisionSystem = new cm::physics::RigidBodyCollisionSystem(s_PhysicsSystem);

    // Install combined contact listener (area + rigidbody collisions)
    s_ContactListener = std::make_unique<PhysicsContactListener>(s_PhysicsSystem, s_AreaSystem, s_CollisionSystem);
    s_PhysicsSystem->SetContactListener(s_ContactListener.get());

    // Initialize ragdoll system
    cm::physics::InitRagdollSystem(s_PhysicsSystem, &s_PhysicsSystem->GetBodyInterface());
}


void Physics::Shutdown() {
    // Shutdown ragdoll system first (depends on physics system)
    cm::physics::ShutdownRagdollSystem();

    if (s_PhysicsSystem) {
        // Clear listener pointer to prevent callbacks during teardown
        s_PhysicsSystem->SetContactListener(nullptr);
    }
    s_ContactListener.reset();

    if (s_CollisionSystem) {
        delete s_CollisionSystem;
        s_CollisionSystem = nullptr;
    }
    if (s_AreaSystem) {
        delete s_AreaSystem;
        s_AreaSystem = nullptr;
    }
    delete s_PhysicsSystem;
    s_PhysicsSystem = nullptr;  // Set to nullptr after deletion to prevent use-after-free
    delete s_JobSystem;
    s_JobSystem = nullptr;
    delete s_TempAllocator;
    s_TempAllocator = nullptr;
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
    std::cout << "[Physics] Jolt Physics shut down.\n";
}

void Physics::Step(float deltaTime) {
    if (!s_PhysicsSystem || !s_TempAllocator || !s_JobSystem) return;
    s_PhysicsSystem->Update(deltaTime, 1, s_TempAllocator, s_JobSystem);
}

glm::vec3 Physics::GetGravity() {
    if (!s_PhysicsSystem) return glm::vec3(0.0f, -9.81f, 0.0f);
    JPH::Vec3 gravity = s_PhysicsSystem->GetGravity();
    return glm::vec3(gravity.GetX(), gravity.GetY(), gravity.GetZ());
}

void Physics::SetGravity(const glm::vec3& gravity) {
    if (!s_PhysicsSystem) return;
    s_PhysicsSystem->SetGravity(JPH::Vec3(gravity.x, gravity.y, gravity.z));
}

void Physics::DestroyBody(JPH::BodyID bodyID) {
    if (!s_PhysicsSystem) return;

    JPH::BodyInterface& bodyInterface = s_PhysicsSystem->GetBodyInterface();
    bodyInterface.RemoveBody(bodyID);
    bodyInterface.DestroyBody(bodyID);
}


JPH::BodyID Physics::CreateBody(const glm::mat4& transform, JPH::RefConst<JPH::Shape> shape, bool isStatic, uint32_t layer) {
    if (!s_PhysicsSystem || !shape)
        return JPH::BodyID(); // Invalid

    // Extract position and rotation from glm::mat4
    glm::vec3 position = glm::vec3(transform[3]); // Extract translation
    glm::quat rotation = glm::quat_cast(transform); // Convert matrix to quaternion

    // Normalize the quaternion to ensure it's valid for Jolt
    rotation = glm::normalize(rotation);

    // Verify quaternion normalization (only in debug builds)
    #ifdef _DEBUG
    float quatLength = glm::length(rotation);
    if (std::abs(quatLength - 1.0f) > 0.001f) {
        std::cerr << "[Physics] Warning: Quaternion not properly normalized, length: " << quatLength << std::endl;
    }
    #endif

    // Convert to Jolt types
    JPH::Vec3 joltPosition(position.x, position.y, position.z);
    JPH::Quat joltRotation(rotation.x, rotation.y, rotation.z, rotation.w);

    // Use provided layer, clamped to valid range
    JPH::ObjectLayer objectLayer = static_cast<JPH::ObjectLayer>(layer < MAX_PHYSICS_LAYERS ? layer : 0);

    // Create body settings with the specified layer
    JPH::BodyCreationSettings settings(
        shape,
        joltPosition,
        joltRotation,
        isStatic ? JPH::EMotionType::Static : JPH::EMotionType::Dynamic,
        objectLayer
    );

    // Set mass for dynamic bodies (default to 1.0 kg)
    if (!isStatic) {
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = 1.0f;
        settings.mGravityFactor = 1.0f; // Ensure gravity is applied
    }

    JPH::BodyInterface& bodyInterface = s_PhysicsSystem->GetBodyInterface();

    // Create and add the body to the simulation
    JPH::Body* body = bodyInterface.CreateBody(settings);
    if (!body) {
        std::cerr << "[Physics] Failed to create body\n";
        return JPH::BodyID();
    }

    bodyInterface.AddBody(body->GetID(), JPH::EActivation::Activate);
    
    // Keep dynamic-body creation logging out of shipping/runtime hot paths.
#ifdef _DEBUG
    if (!isStatic) {
        std::cout << "[Physics] Created dynamic body with ID " << body->GetID().GetIndex()
                  << " at position (" << position.x << ", " << position.y << ", " << position.z << ")" << std::endl;
    }
#endif
    
    return body->GetID();
}

void Physics::SetBodyLinearVelocity(JPH::BodyID bodyID, const glm::vec3& velocity) {
    if (!bodyID.IsInvalid() && s_PhysicsSystem) {
        JPH::Vec3 joltVelocity(velocity.x, velocity.y, velocity.z);
        s_PhysicsSystem->GetBodyInterface().SetLinearVelocity(bodyID, joltVelocity);
    }
}

void Physics::SetBodyAngularVelocity(JPH::BodyID bodyID, const glm::vec3& velocity) {
    if (!bodyID.IsInvalid() && s_PhysicsSystem) {
        JPH::Vec3 joltVelocity(velocity.x, velocity.y, velocity.z);
        s_PhysicsSystem->GetBodyInterface().SetAngularVelocity(bodyID, joltVelocity);
    }
}

// -----------------------------------------------------------------------------
// Public helper to retrieve BodyInterface so that other systems (e.g. Scene)
// can create / manipulate bodies without touching the Physics internals.
// -----------------------------------------------------------------------------
JPH::BodyInterface& Physics::GetBodyInterface() {
    return s_PhysicsSystem->GetBodyInterface();
}

cm::physics::AreaSystem* Physics::GetAreaSystem() { return s_AreaSystem; }
cm::physics::RagdollSystem* Physics::GetRagdollSystem() { return cm::physics::GetRagdollSystem(); }
cm::physics::RigidBodyCollisionSystem* Physics::GetCollisionSystem() { return s_CollisionSystem; }

glm::mat4 Physics::GetBodyTransform(JPH::BodyID bodyID) {
    if (bodyID.IsInvalid() || !s_PhysicsSystem) {
        return glm::mat4(0.0f); // Return invalid transform
    }

    JPH::Mat44 joltTransform = s_PhysicsSystem->GetBodyInterface().GetWorldTransform(bodyID);
    
    // Convert Jolt (row-major) matrix to GLM (column-major) matrix by transposing during copy
    glm::mat4 glmTransform(1.0f);
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            glmTransform[col][row] = joltTransform(row, col);
        }
    }
    return glmTransform;
}

JPH::PhysicsSystem* Physics::GetSystem() { return s_PhysicsSystem; }
JPH::TempAllocatorImpl* Physics::GetTempAllocator() { return s_TempAllocator; }
void Physics::SetBodyTransform(JPH::BodyID bodyID, const glm::vec3& position, const glm::vec3& eulerDegrees) {
    if (bodyID.IsInvalid() || !s_PhysicsSystem) return;
    JPH::BodyInterface& bi = s_PhysicsSystem->GetBodyInterface();
    // Convert Euler degrees to quaternion
    glm::quat rot = glm::quat(glm::radians(eulerDegrees));
    JPH::RVec3 pos(position.x, position.y, position.z);
    JPH::Quat q(rot.x, rot.y, rot.z, rot.w);
    bi.SetPositionAndRotation(bodyID, pos, q, JPH::EActivation::Activate);
}

void Physics::MoveKinematicBody(JPH::BodyID bodyID, const glm::vec3& linearVelocity, const glm::vec3& angularVelocity, float deltaTime)
{
    if (bodyID.IsInvalid() || !s_PhysicsSystem) return;
    JPH::BodyInterface& bi = s_PhysicsSystem->GetBodyInterface();

    // Get current transform
    JPH::Mat44 current = bi.GetWorldTransform(bodyID);
    JPH::RVec3 pos = current.GetTranslation();
    JPH::Quat rot = current.GetQuaternion();

    // Integrate position: p' = p + v * dt
    JPH::RVec3 deltaPos(linearVelocity.x * deltaTime, linearVelocity.y * deltaTime, linearVelocity.z * deltaTime);
    JPH::RVec3 newPos = pos + deltaPos;

    // Integrate rotation from angular velocity (radians/sec) over dt
    float angSpeed = std::sqrt(angularVelocity.x * angularVelocity.x + angularVelocity.y * angularVelocity.y + angularVelocity.z * angularVelocity.z);
    JPH::Quat deltaRot = JPH::Quat::sIdentity();
    if (angSpeed > 1e-6f)
    {
        float angle = angSpeed * deltaTime;
        JPH::Vec3 axis(angularVelocity.x / angSpeed, angularVelocity.y / angSpeed, angularVelocity.z / angSpeed);
        deltaRot = JPH::Quat::sRotation(axis, angle);
    }
    JPH::Quat newRot = deltaRot * rot;

    // Use MoveKinematic so CCD etc. can be applied by the engine
    bi.MoveKinematic(bodyID, newPos, newRot, deltaTime);
}

// Thread-local flag to prevent duplicate debug visualization when RaycastPoints calls Raycast
static thread_local bool s_InternalRaycast = false;
static thread_local uint32_t s_CurrentLayerMask = PhysicsLayers::MASK_ALL;

bool Physics::Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, RaycastHit& outHit, uint32_t layerMask)
{
    outHit = RaycastHit{}; // Reset output
    
    if (!s_PhysicsSystem) return false;
    
    // Validate direction - avoid NaN from normalizing zero vector
    float dirLengthSq = glm::dot(direction, direction);
    if (dirLengthSq < 0.000001f || maxDistance <= 0.0f) return false;
    
    // Normalize direction and create Jolt ray
    glm::vec3 dir = direction / std::sqrt(dirLengthSq);
    JPH::RRayCast ray(
        JPH::RVec3(origin.x, origin.y, origin.z),
        JPH::Vec3(dir.x * maxDistance, dir.y * maxDistance, dir.z * maxDistance)
    );
    
    // Create a closest hit collector
    JPH::RayCastResult result;
    JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> collector;
    
    // Create layer mask filter
    LayerMaskBodyFilter bodyFilter(layerMask, s_PhysicsSystem);
    
    // Perform the raycast using the narrow phase query with body filter
    const JPH::NarrowPhaseQuery& query = s_PhysicsSystem->GetNarrowPhaseQuery();
    query.CastRay(ray, JPH::RayCastSettings(), collector, {}, {}, bodyFilter);
    
    bool hitResult = false;
    if (collector.HadHit())
    {
        const JPH::RayCastResult& hit = collector.mHit;
        
        outHit.hit = true;
        outHit.distance = hit.mFraction * maxDistance;
        
        // Calculate hit point
        JPH::RVec3 hitPoint = ray.GetPointOnRay(hit.mFraction);
        outHit.point = glm::vec3(hitPoint.GetX(), hitPoint.GetY(), hitPoint.GetZ());
        
        // Get hit normal from the body
        JPH::BodyLockRead lock(s_PhysicsSystem->GetBodyLockInterface(), hit.mBodyID);
        if (lock.Succeeded())
        {
            const JPH::Body& body = lock.GetBody();
            JPH::Vec3 normal = body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, hitPoint);
            outHit.normal = glm::vec3(normal.GetX(), normal.GetY(), normal.GetZ());
            
            // Get entity ID from body user data
            outHit.entityId = static_cast<uint32_t>(body.GetUserData());
        }
        
        hitResult = true;
    }
    
    // Debug visualization (only if not called from RaycastPoints to avoid duplicates)
    if (!s_InternalRaycast && PhysicsDebug::IsEnabled()) {
        PhysicsDebug::AddRaycast(origin, direction, maxDistance, hitResult, outHit.point, outHit.normal);
    }
    
    return hitResult;
}

bool Physics::RaycastPoints(const glm::vec3& from, const glm::vec3& to, RaycastHit& outHit, uint32_t layerMask)
{
    glm::vec3 direction = to - from;
    float distance = glm::length(direction);
    
    if (distance < 0.0001f) {
        outHit = RaycastHit{};
        return false;
    }
    
    // Mark as internal call to prevent duplicate debug visualization
    s_InternalRaycast = true;
    s_CurrentLayerMask = layerMask;
    bool result = Raycast(from, direction / distance, distance, outHit, layerMask);
    s_InternalRaycast = false;
    
    // Debug visualization for linecast (shows from/to points)
    if (PhysicsDebug::IsEnabled()) {
        PhysicsDebug::AddLinecast(from, to, result, outHit.point, outHit.normal);
    }
    
    return result;
}

// Set the physics layer of a body
void Physics::SetBodyLayer(JPH::BodyID bodyID, uint32_t layer) {
    if (bodyID.IsInvalid() || !s_PhysicsSystem) return;
    if (layer >= MAX_PHYSICS_LAYERS) layer = 0;
    
    JPH::BodyInterface& bi = s_PhysicsSystem->GetBodyInterface();
    bi.SetObjectLayer(bodyID, static_cast<JPH::ObjectLayer>(layer));
    bi.InvalidateContactCache(bodyID);
}
