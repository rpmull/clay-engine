#pragma once
#include <string>
#include <iostream>
#include <vector>
#include <array>
#include <utility>
#include <algorithm>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <limits>
#include <bgfx/bgfx.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_precision.hpp>
#include <glm/gtc/constants.hpp>

// Core includes
#include "Entity.h"
#include "core/assets/BinaryFormats.h"
#include "UIComponents.h"
#include "AnimationComponents.h"
#include "core/rendering/VertexTypes.h"
#include "core/rendering/Material.h"
#include "core/rendering/Mesh.h"
#include "core/rendering/Camera.h"
#include "core/rendering/MaterialPropertyBlock.h"
#include "core/rendering/MaterialSource.h"
#include "core/particles/ParticleSystem.h"
#include "core/assets/AssetReference.h"
#include "core/audio/AudioComponents.h"
#include "core/ecs/InstancerComponent.h"

// GLM
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/transform.hpp>

// Physics
#include "core/physics/Physics.h"
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>

// Forward declaration to avoid circular dependency
class Terrain;


///----------------------------------------------------------------------
/// Transform Component
///----------------------------------------------------------------------
struct TransformComponent {
   glm::vec3 Position = glm::vec3(0.0f);
   glm::vec3 Rotation = glm::vec3(0.0f); // Euler degrees (XYZ order) — kept for UI
   glm::quat RotationQ = glm::quat(1, 0, 0, 0); // Authoritative when UseQuatRotation is true
   glm::vec3 Scale = glm::vec3(1.0f);

   bool UseQuatRotation = true; // If true, build rotation from RotationQ instead of Euler

   glm::mat4 LocalMatrix = glm::mat4(1.0f);  // Local transform
   glm::mat4 WorldMatrix = glm::mat4(1.0f);  // Computed

   bool TransformDirty = true;

   inline glm::mat4 CalculateLocalMatrix() {
      const glm::mat4 translation = glm::translate(glm::mat4(1.0f), Position);

      const glm::mat4 rotation = UseQuatRotation
         ? glm::toMat4(glm::normalize(RotationQ))
         : (
            glm::yawPitchRoll(
               glm::radians(Rotation.y),
               glm::radians(Rotation.x),
               glm::radians(Rotation.z)
            )
            );

      const glm::mat4 scale = glm::scale(glm::mat4(1.0f), Scale);

      LocalMatrix = translation * rotation * scale;
      return LocalMatrix;
      }
   };


// Forward declaration
struct BlendShapeComponent;


///----------------------------------------------------------------------
/// Mesh Component
///----------------------------------------------------------------------
struct MeshComponent {
   std::shared_ptr<Mesh> mesh;

   std::string MeshName;

   AssetReference meshReference;

   std::shared_ptr<Material> material;

   // Optional: Support multiple materials per mesh (slot 0..N-1)
   std::vector<std::shared_ptr<Material>> materials;
   // Imported/authored slot labels aligned with `materials`
   std::vector<std::string> MaterialSlotNames;
   // Optional: remember which asset (if any) drives each slot so shared edits can be applied
   std::vector<std::string> MaterialAssetPaths;

   // Tracks which slots already own a per-entity cloned material instance.
   // This is transient runtime/editor state and is not serialized.
   std::vector<bool> OwnedMaterialSlots;
   
   // Inline material data (deserialized from binary format, for materials embedded in scene)
   std::vector<binary::InlineMaterialData> InlineMaterials;
   
   // Shader graph material data (for runtime shader graph materials)
   std::vector<binary::ShaderGraphMaterialData> ShaderGraphMaterials;

   // Optional: per-slot property blocks, aligned with materials
   std::vector<MaterialPropertyBlock> SlotPropertyBlocks;

   // Source descriptors captured at import time so embedded textures survive serialization
   std::vector<MaterialSource> MaterialSources;

   // Optional: proxies controlling each submesh slot (entity IDs aligned with Submeshes)
   std::vector<EntityID> SubmeshOwners;

   // Per-mesh property block (applied after per-slot blocks)
   MaterialPropertyBlock PropertyBlock;

   // Persistable file paths for texture overrides in PropertyBlock
   std::unordered_map<std::string, std::string> PropertyBlockTexturePaths;

   // Persistable per-slot texture override paths (aligned with SlotPropertyBlocks)
   std::vector<std::unordered_map<std::string, std::string>> SlotPropertyBlockTexturePaths;

   // For merged meshes: the original submesh indices (fileIDs) that were combined
   std::vector<int> CombinedSubmeshFileIDs;
    
   // Rendering overrides
   // When true, draws this mesh on top of others (disables Z-write and uses depth test ALWAYS)
   bool RenderOnTop = false;

   // Reserved for future explicit sorting if needed
   int RenderOrder = 0;

   // Per-mesh override to show backfaces by disabling culling
   bool ShowBackfaces = false;

   // When true, skip CPU frustum culling for this mesh (useful for first-person arms)
   bool SkipFrustumCulling = false;

   // Multiplier for AABB bounds used in frustum culling (1.0 = no padding)
   // Increase for skinned meshes to account for animation poses (recommended: 1.5-2.0)
   float BoundsPadding = 1.0f;

   // When true this mesh owns a unique cloned material instance (not shared via cache)
   bool UniqueMaterial = false;
   
   // =========================================================================
   // PERF: GPU Instancing support
   // When true, this mesh is eligible for instanced batching with other meshes
   // that share the same mesh+material and have no per-entity property blocks.
   // Skinned meshes cannot be instanced (each has unique bone palette).
   // =========================================================================
   bool EnableInstancing = false;

   // Blendshapes
   BlendShapeComponent* BlendShapes = nullptr;


   // Constructors
   MeshComponent(std::shared_ptr<Mesh> m, const std::string& name, std::shared_ptr<Material> mat)
      : mesh(std::move(m)), MeshName(name), material(std::move(mat)) {
      if (material) {
         materials = { material };
         MaterialAssetPaths = { std::string() };
         OwnedMaterialSlots = { false };
         SlotPropertyBlocks = { MaterialPropertyBlock{} };
         SlotPropertyBlockTexturePaths = { {} };
         MaterialSources = { MaterialSource{} };
      }
      }

   MeshComponent() = default;
   };

struct MeshProxyComponent {
   EntityID TargetMesh = INVALID_ENTITY_ID;
   std::vector<uint32_t> SubmeshSlots;
   std::unordered_map<std::string, std::string> PropertyBlockTexturePaths;
   std::vector<MaterialPropertyBlock> SlotPropertyBlocks;
   std::vector<std::unordered_map<std::string, std::string>> SlotPropertyBlockTexturePaths;
   std::vector<std::shared_ptr<Material>> SlotMaterialOverrides;
   std::vector<std::string> SlotMaterialAssetPaths;
   // Quick lookup from absolute slot index -> local SlotPropertyBlocks entry
   std::unordered_map<uint32_t, size_t> SlotIndexLookup;
   MaterialPropertyBlock PropertyBlock;
   // Serialization / relink hints
   EntityID SerializedTarget = INVALID_ENTITY_ID;     // original entity ID stored in scene files
   std::string TargetScenePathHint;                  // absolute scene path hint (Root/Child/Sub)
   std::string TargetModelPathHint;                  // relative path under model root (used for compact model overrides)
   ClaymoreGUID TargetGuidHint{};                    // persisted target GUID for robust relinking
   ClaymoreGUID TargetModelGuidHint{};               // persisted model GUID for model-relative lookups
};



//----------------------------------------------------------------------
// Light Component
//----------------------------------------------------------------------
enum class LightType {
   Directional,
   Point,
   Spot
   };

struct LightComponent {
   LightType Type = LightType::Directional;
   glm::vec3 Color = { 1.0f, 1.0f, 1.0f };
   float Intensity = 1.0f;
   float Range = 50.0f;
   float SpotInnerAngleDegrees = 20.0f;
   float SpotOuterAngleDegrees = 30.0f;
   // Point-light shadow opt-in (directional shadows remain environment-driven).
   bool PointShadowsEnabled = false;

   LightComponent(LightType type = LightType::Directional,
      const glm::vec3& color = { 1.0f, 1.0f, 1.0f },
      float intensity = 1.0f)
      : Type(type), Color(color), Intensity(intensity) {
      }
   // For directional: use rotation from TransformComponent
   // For point/spot: use position from TransformComponent
   };


//----------------------------------------------------------------------
// Collider Component
//----------------------------------------------------------------------
struct ColliderComponent {
   ColliderShape ShapeType = ColliderShape::Box;

   glm::vec3 Offset = glm::vec3(0.0f);         // Local offset from entity transform
   glm::vec3 Size = glm::vec3(1.0f);           // For Box
   float Radius = 0.5f, Height = 1.0f;         // For Capsule/Sphere (Radius for Sphere, both for Capsule)
   std::string MeshPath;                      // For Mesh

   bool IsTrigger = false;
   
   // Physics layer (index 0-31, name stored for serialization)
   uint32_t PhysicsLayer = 0;
   std::string PhysicsLayerName = "Default";

   // Cached Jolt shape
   JPH::RefConst<JPH::Shape> Shape;

   // Runtime interop may create a default collider as the companion for a
   // script-added rigidbody. Remove that collider when the rigidbody is removed.
   bool _RuntimeOwnedByRigidBody = false;
   
   // Last-known shape parameters for detecting runtime changes
   // When these differ from current values, the physics body shape is recreated
   ColliderShape _LastShapeType = ColliderShape::Box;
   glm::vec3 _LastSize = glm::vec3(-1.0f);
   float _LastRadius = -1.0f;
   float _LastHeight = -1.0f;
   glm::vec3 _LastOffset = glm::vec3(-9999.0f); // Track offset changes separately
   glm::vec3 _LastWorldScale = glm::vec3(-1.0f);

   ColliderComponent() = default;

   //----------------------------------------------------------------------
   // Build the collision shape based on type and parameters.
   // If ShapeType is Mesh, a valid Mesh pointer must be provided to
   // auto-detect appropriate shape based on mesh bounds.
   //----------------------------------------------------------------------
   void BuildShape(const Mesh* mesh = nullptr,
      const glm::vec3& worldScale = glm::vec3(1.0f)) {
      constexpr float kMinExtent = 0.01f;
      constexpr float kMinRadius = 0.05f;
      constexpr float kMinHalfHeight = 0.01f;

      auto createOrFallback = [&](auto& settings) {
         auto res = settings.Create();

         if (res.HasError()) {

            std::cerr << "[Collider] Shape creation failed: " <<
               res.GetError() << ". Using fallback 0.5m box.\n";

            // Set fallback settings and create
            JPH::BoxShapeSettings fb(JPH::Vec3(0.25f, 0.25f, 0.25f));
            auto fbRes = fb.Create();
            if (!fbRes.HasError())
               {
               Shape = fbRes.Get();
               }

            return;
            }

         // Success
         Shape = res.Get();
         };

      switch (ShapeType) {

         // Box
            case ColliderShape::Box: {
            glm::vec3 absSize = glm::abs(Size) * glm::abs(worldScale);
            JPH::Vec3 he(
               std::max(absSize.x * 0.5f, kMinExtent),
               std::max(absSize.y * 0.5f, kMinExtent),
               std::max(absSize.z * 0.5f, kMinExtent));
            JPH::BoxShapeSettings settings(he);

            createOrFallback(settings);
            break;
            }

                                   // Capsule
            case ColliderShape::Capsule: {
            glm::vec3 ws = glm::abs(worldScale);

            float r = std::max(Radius * std::max(ws.x, ws.z), kMinRadius);
            float hh = std::max((Height * ws.y) * 0.5f, kMinHalfHeight);

            // Jolt CapsuleShapeSettings takes (halfHeight, radius) - NOT (radius, halfHeight)!
            JPH::CapsuleShapeSettings settings(hh, r);

            createOrFallback(settings);
            break;
            }

                                   // Sphere
            case ColliderShape::Sphere: {
            glm::vec3 ws = glm::abs(worldScale);
            // Use average of scale components for uniform sphere scaling
            float avgScale = (ws.x + ws.y + ws.z) / 3.0f;
            float r = std::max(Radius * avgScale, kMinRadius);

            JPH::SphereShapeSettings settings(r);

            createOrFallback(settings);
            break;
            }

                                       // Mesh
         case ColliderShape::Mesh: {
            if (mesh && !mesh->Vertices.empty()) {
               const glm::vec3 ws = glm::abs(worldScale);

               JPH::VertexList vertices;
               vertices.reserve(mesh->Vertices.size());
               for (const auto& v : mesh->Vertices) {
                  vertices.emplace_back(v.x * ws.x, v.y * ws.y, v.z * ws.z);
                  }

               JPH::IndexedTriangleList triangles;
               if (!mesh->Indices.empty()) {
                  triangles.reserve(mesh->Indices.size() / 3);
                  for (size_t i = 0; i + 2 < mesh->Indices.size(); i += 3) {
                     const uint32_t i0 = mesh->Indices[i + 0];
                     const uint32_t i1 = mesh->Indices[i + 1];
                     const uint32_t i2 = mesh->Indices[i + 2];
                     if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
                        continue;
                        }
                     triangles.emplace_back(i0, i1, i2, 0);
                     }
                  }
               else {
                  triangles.reserve(mesh->Vertices.size() / 3);
                  for (size_t i = 0; i + 2 < mesh->Vertices.size(); i += 3) {
                     triangles.emplace_back(static_cast<uint32_t>(i),
                                            static_cast<uint32_t>(i + 1),
                                            static_cast<uint32_t>(i + 2),
                                            0);
                     }
                  }

               if (!triangles.empty()) {
                  JPH::MeshShapeSettings settings(std::move(vertices), std::move(triangles));
                  createOrFallback(settings);
                  break;
                  }
               }

            // Fallback to box shape if no valid mesh data provided
            glm::vec3 absSize = glm::abs(Size) * glm::abs(worldScale);
            JPH::BoxShapeSettings settings(JPH::Vec3(
               std::max(absSize.x * 0.5f, kMinExtent),
               std::max(absSize.y * 0.5f, kMinExtent),
               std::max(absSize.z * 0.5f, kMinExtent)));

            createOrFallback(settings);
            break;
            }
         }
      }
   };


// -----------------------------------------------------------------------------
// RigidBody Component
// -----------------------------------------------------------------------------
struct RigidBodyComponent {

   // Physics properties
   float Mass = 1.0f;
   float Friction = 0.5f;
   float Restitution = 0.0f;
   bool UseGravity = true;
   bool IsKinematic = false;
   
   // Physics layer (index 0-31, name stored for serialization)
   uint32_t PhysicsLayer = 0;
   std::string PhysicsLayerName = "Default";
   uint32_t CollisionMask = 0xFFFFFFFFu;

   // Physics body reference
   JPH::BodyID BodyID = JPH::BodyID();

   // Velocity and angular velocity (for kinematic bodies)
   glm::vec3 LinearVelocity = glm::vec3(0.0f);
   glm::vec3 AngularVelocity = glm::vec3(0.0f);

   // Runtime-only root motion injected for the current physics step.
   // This keeps animation-driven kinematic motion from accumulating into
   // the authored/persistent body velocity.
   glm::vec3 _RootMotionLinearVelocity = glm::vec3(0.0f);

#if !defined(CLAYMORE_RUNTIME)
   // Editor-only live value shown in the inspector during play mode.
   glm::vec3 _EditorDisplayLinearVelocity = glm::vec3(0.0f);
#endif

   // Transient dynamic-body force / impulse requests from managed code.
   // These are consumed during the next dynamic physics step, which makes
   // one-shot calls safe even when the physics body is created later.
   glm::vec3 PendingForce = glm::vec3(0.0f);
   glm::vec3 PendingTorque = glm::vec3(0.0f);
   glm::vec3 PendingImpulse = glm::vec3(0.0f);
   glm::vec3 PendingAngularImpulse = glm::vec3(0.0f);

   // Cached runtime state to avoid redundant physics API updates every frame.
   float _LastAppliedGravityFactor = -1.0f;
   uint32_t _DebugQueuedPhysicsCommandCount = 0;
   uint32_t _DebugImmediatePhysicsCommandCount = 0;
   uint32_t _DebugAppliedPhysicsCommandCount = 0;

   RigidBodyComponent() = default;
   };


// -----------------------------------------------------------------------------
// StaticBody Component
// -----------------------------------------------------------------------------
struct StaticBodyComponent {
   float Friction = 0.5f;
   float Restitution = 0.0f;

   // Physics layer (index 0-31, name stored for serialization)
   uint32_t PhysicsLayer = 0;
   std::string PhysicsLayerName = "Default";

   // Physics body reference
   JPH::BodyID BodyID = JPH::BodyID();

   StaticBodyComponent() = default;
   };

enum class SoftbodyBendMode : uint8_t {
   None = 0,
   Distance = 1,
   Dihedral = 2
};

// -----------------------------------------------------------------------------
// Softbody Component
// -----------------------------------------------------------------------------
// Native cloth / soft-body authoring data and runtime state.
// Authoring arrays persist with the scene; runtime handles are rebuilt on play.
// -----------------------------------------------------------------------------
struct SoftbodyComponent {
   bool Enabled = true;
   uint32_t SolverIterations = 6;
   float LinearDamping = 0.03f;
   float Friction = 0.5f;
   float Restitution = 0.0f;
   float Pressure = 0.0f;
   float GravityFactor = 1.0f;
   float VertexRadius = 0.015f;
   float MaxLinearVelocity = 120.0f;

   float EdgeCompliance = 1.0e-6f;
   float ShearCompliance = 1.0e-5f;
   float BendCompliance = 1.0e-4f;
   bool EnableLongRangeAttachments = true;
   float LRAMaxDistanceMultiplier = 1.05f;
   SoftbodyBendMode BendMode = SoftbodyBendMode::Distance;
   bool FacesDoubleSided = true;

   // Unity-style mobility paint: 0 = stiff/heavy, 1 = fully free.
   // Anchors are authored separately and always win.
   float WeightFloor = 0.05f;

   // Physics layer (index 0-31, name stored for serialization)
   uint32_t PhysicsLayer = 0;
   std::string PhysicsLayerName = "Default";

   // Persisted authoring data
   std::vector<float> VertexWeights;
   std::vector<uint8_t> AnchorVertices;
   uint32_t SourceVertexCount = 0;
   uint32_t SourceIndexCount = 0;

   // Runtime-only state
   JPH::BodyID BodyID = JPH::BodyID();
   JPH::Ref<JPH::SoftBodySharedSettings> RuntimeSharedSettings;
   std::shared_ptr<Mesh> RuntimeMesh;
   std::shared_ptr<Mesh> SourceMesh;
   std::vector<glm::vec3> RuntimeNormals;
   std::vector<PBRVertex> ScratchVertices;
   std::vector<SkinnedPBRVertex> ScratchSkinnedVertices;

   SoftbodyComponent() = default;
};

// -----------------------------------------------------------------------------
// Character Controller (Jolt CharacterVirtual)
// -----------------------------------------------------------------------------
struct CharacterControllerComponent {
   
   // Geometry (capsule)
   float Radius = 0.4f;
   
   // full height excluding hemispheres
   float Height = 1.6f; 
   
   // Orientation
   glm::vec3 Up = glm::vec3(0.0f, 1.0f, 0.0f);

   // Local offset from entity origin (before feet-to-center)
   glm::vec3 Offset = glm::vec3(0.0f);
   
   // Physics layer (index 0-31, name stored for serialization)
   uint32_t PhysicsLayer = 1; // Default to "Player" layer (index 1)
   std::string PhysicsLayerName = "Player";

   // Collision mask: which physics layers this character is allowed to collide with.
   // Bit i corresponds to physics layer index i. Mirrors RigidBodyComponent::CollisionMask.
   // Default: collide with everything.
   uint32_t CollisionMask = 0xFFFFFFFFu;

   // Movement / navigation
   glm::vec3 DesiredVelocity = glm::vec3(0.0f);
   float VerticalVelocity = 0.0f; // internal running vertical speed for gravity/jump
   float MaxSlopeDegrees = 50.0f;
   float JumpSpeed = 6.0f;
   bool StickToFloor = true;
   bool EnableWalkStairs = true;
   
   // Jump control (request set by interop per-frame)
   bool JumpRequested = false;
   float JumpRequestSpeed = 6.0f;

   // Runtime state
   bool IsGrounded = false;
   JPH::Ref<JPH::CharacterVirtual> Character; // created at runtime
   
   /// Set by root motion when animation overrides gravity (e.g., climbing)
   /// Reset each frame after physics update
   bool _RootMotionOverrideGravity = false;
   
   /// Set when root motion injects vertical velocity this frame.
   /// Used to avoid clamping valid grounded downward animation motion.
   bool _RootMotionAppliedVertical = false;

#if !defined(CLAYMORE_RUNTIME)
   // Editor-only live value shown in the inspector during play mode.
   glm::vec3 _EditorDisplayDesiredVelocity = glm::vec3(0.0f);
#endif

   // Last-known shape parameters for detecting changes at runtime
   // When these differ from Radius/Height, the character capsule is recreated
   float _LastRadius = -1.0f;
   float _LastHeight = -1.0f;
   uint32_t _LastPhysicsLayer = std::numeric_limits<uint32_t>::max();

   CharacterControllerComponent() = default;
   };

//------------------------------------------------------------------------------
// Grass Deformer Component
// Entities with this component will push grass away as they move
//------------------------------------------------------------------------------
struct GrassDeformerComponent
{
   bool Enabled = true;
   float Radius = 0.5f;           // Deformation radius in world units
   float Strength = 1.0f;         // Deformation strength (0-1)
   float HeightOffset = 0.0f;     // Vertical offset from entity position (for feet placement)
   bool UseVelocity = true;       // Use movement velocity for directional deformation
   
   // Runtime state
   glm::vec3 LastPosition = glm::vec3(0.0f);
   glm::vec3 Velocity = glm::vec3(0.0f);
};


//------------------------------------------------------------------------------
// Camera Component
//------------------------------------------------------------------------------
struct CameraComponent {
   
   // Internal Camera object
   Camera Camera;

   // If true, this camera is considered for rendering
   bool Active = false;

   // Lower values render first, higher values render last
   int priority = 0; 

   // Layer-based culling mask 
   // (bit i corresponds to EntityData::Layer == i)
   uint32_t LayerMask = 0xFFFFFFFFu;

   // Settings
   float FieldOfView = 60.0f;
   float NearClip = 0.1f;
   float FarClip = 1000.0f;
   bool IsPerspective = true;

   CameraComponent() = default;


   ///----------------------------------------------------------------------
   /// UpdateProjection: Update camera projection matrix based on aspect ratio
   ///----------------------------------------------------------------------
   void UpdateProjection(float aspectRatio) {
      if (IsPerspective)
         Camera.SetPerspective(FieldOfView, aspectRatio, NearClip, FarClip);
      else {
         // Orthographic projection
         float orthoSize = 10.0f; // Default ortho size
         Camera.SetOrthographic(orthoSize, aspectRatio, NearClip, FarClip);
         }
      }


   ///----------------------------------------------------------------------
   /// SyncWithTransform: Update camera position/rotation from entity transform
   ///----------------------------------------------------------------------
   void SyncWithTransform(const TransformComponent& transform) {
      
      // Use world-space transform so parenting is respected
      const glm::mat4& world = transform.WorldMatrix;
      
      glm::vec3 position = glm::vec3(world[3]);
      
      // Extract rotation without scale from world matrix
      glm::vec3 X = glm::vec3(world[0]);
      glm::vec3 Y = glm::vec3(world[1]);
      glm::vec3 Z = glm::vec3(world[2]);
      glm::vec3 S = glm::vec3(glm::length(X), 
                              glm::length(Y), 
                              glm::length(Z));
      
      if (S.x > 1e-6f) X /= S.x;
      if (S.y > 1e-6f) Y /= S.y;
      if (S.z > 1e-6f) Z /= S.z;
      
      glm::quat rotQ = glm::normalize(glm::quat_cast(glm::mat3(X, Y, Z)));

      Camera.SetPosition(position);
      Camera.SetRotationQuat(rotQ);

      }
   };


//------------------------------------------------------------------------------
// Terrain Component
//------------------------------------------------------------------------------
enum class TerrainBrushMode : uint8_t
{
   Height = 0,
   Texture = 1,
   Grass = 2,
   SmoothHeight = 3,
   StampHeight = 4,
   ErosionNoise = 5,
   FlattenHeight = 6,
   CliffStamp = 7,
   MountainStamp = 8,
   Hole = 9,
   HeightmapStamp = 10,
   Instancer = 11
};

struct TerrainBrush
{
   TerrainBrushMode Mode = TerrainBrushMode::Height;
   float Radius = 4.0f;
   float Strength = 1.25f;
   float TextureStrength = 0.5f;
   float GrassStrength = 0.5f;
   float InstancerStrength = 0.5f;
   float Falloff = 2.0f;
   bool AlignToNormal = true;
   int ActiveLayer = 0;
   int ActiveGrassLayer = 0;
   int ActiveInstancerLayer = 0;
   // Erosion noise parameters
   float ErosionNoiseScale = 0.15f;       // Noise frequency (smaller = larger features)
   int ErosionNoiseOctaves = 4;           // Number of noise layers
   float ErosionNoisePersistence = 0.5f;  // How much each octave contributes
   float ErosionNoiseStrength = 1.0f;     // How strongly noise affects terrain
   // Flatten height parameters
   float FlattenTargetHeight = 0.0f;      // Target world height for flatten mode
   // Heightmap stamp parameters
   std::string HeightmapStampTexturePath;
   bgfx::TextureHandle HeightmapStampTexture = BGFX_INVALID_HANDLE;
   std::vector<float> HeightmapStampSamples;
   int HeightmapStampWidth = 0;
   int HeightmapStampHeight = 0;
   bool HeightmapStampAdditive = false;
   bool HeightmapStampSubtractive = false;
   float HeightmapStampMinY = 0.0f;
   float HeightmapStampBaselineY = 0.0f;
   float HeightmapStampMaxY = 20.0f;
   // Cliff stamp parameters
   float CliffHeight = 10.0f;             // Height of the cliff face
   float CliffRoughness = 0.3f;           // Amount of jagged detail on cliff face
   float CliffLayering = 0.5f;            // Horizontal sedimentary banding strength
   // Mountain stamp parameters
   float MountainHeight = 20.0f;          // Peak height of mountain
   float MountainRidgeScale = 0.2f;       // Scale of ridge features
   float MountainRockiness = 0.4f;        // Surface rock detail amount
   float MountainSteepness = 1.5f;        // How steep the mountain slopes are
};

// Maximum number of terrain texture layers supported (uses 2 splatmaps: RGBA each)
constexpr uint32_t kMaxTerrainLayers = 8;

// Texture resize filter for terrain layer texture arrays
enum class TerrainTextureFilter : uint8_t
{
   Nearest = 0,    // Fastest, pixelated (good for pixel art)
   Bilinear = 1,   // Good balance of quality/speed (default)
   Bicubic = 2     // Best quality, slower (Catmull-Rom)
};

struct TerrainLayerDesc
{
   std::string Name;
   std::string AlbedoPath;
   std::string NormalPath;
   bgfx::TextureHandle AlbedoHandle = BGFX_INVALID_HANDLE;
   bgfx::TextureHandle NormalHandle = BGFX_INVALID_HANDLE;
   float Tiling = 5.0f;
   float NavCost = 1.0f;
   glm::vec3 PlaceholderColor = glm::vec3(0.5f);

   TerrainLayerDesc() = default;
   ~TerrainLayerDesc() { ReleaseHandles(); }
   TerrainLayerDesc(const TerrainLayerDesc& other);
   TerrainLayerDesc& operator=(const TerrainLayerDesc& other);
   TerrainLayerDesc(TerrainLayerDesc&& other) noexcept;
   TerrainLayerDesc& operator=(TerrainLayerDesc&& other) noexcept;

private:
   void ReleaseHandles();
};

enum class GrassRenderMode : uint8_t
{
   Billboard = 0,          // Y-axis billboard that faces the camera
   BillboardFixed = 1,     // Non-billboarded quad with random Y-rotation
   Mesh = 2
};

enum class GrassMaskSource : uint8_t
{
   None = 0,
   SplatR,
   SplatG,
   SplatB,
   SplatA,
   Painted
};

enum class TerrainInstancerMaskSource : uint8_t
{
   Painted = 0,
   SplatR,
   SplatG,
   SplatB,
   SplatA
};

// GPU instance layout description used by both CPU fallback paths and the new
// compute-driven runtime. Each instance expands to four vec4 values mapped to
// TEXCOORD4..7 for bgfx instancing.
struct GrassInstanceData
{
   static constexpr uint32_t kVecCount = 4;
   glm::vec4 Data[kVecCount]{};
};

struct GrassPatchKey
{
   uint32_t X = 0;
   uint32_t Z = 0;

   bool operator==(const GrassPatchKey& other) const
   {
      return X == other.X && Z == other.Z;
   }
};

struct GrassPatchKeyHash
{
   size_t operator()(const GrassPatchKey& key) const noexcept
   {
      return (static_cast<size_t>(key.X) << 32) ^ static_cast<size_t>(key.Z);
   }
};

struct GrassPatchCacheEntry
{
   std::vector<GrassInstanceData> Instances;
   uint32_t LastUsedFrame = 0;
   bool Ready = false;
};

struct GrassPatchJobResult
{
   GrassPatchKey Key{};
   std::vector<GrassInstanceData> Instances;
};

struct GrassLayerRuntime
{
   // GPU compute buffers
   bgfx::DynamicVertexBufferHandle InstanceBuffer = BGFX_INVALID_HANDLE;
   bgfx::TextureHandle CounterTexture = BGFX_INVALID_HANDLE;  // R32U texture for atomic counter UAV
   bgfx::IndirectBufferHandle IndirectArgs = BGFX_INVALID_HANDLE;
   uint32_t Capacity = 0;
   
   // CPU instance cache
   std::vector<GrassInstanceData> CpuInstanceCache;
   bool CpuCacheValid = false;
   glm::vec3 CachedCameraPos = glm::vec3(0.0f);
   float LastCpuUpdateTime = -1000.0f;

   // CPU patch cache for async grass generation
   std::unordered_map<GrassPatchKey, GrassPatchCacheEntry, GrassPatchKeyHash> PatchCache;
   std::unordered_set<GrassPatchKey, GrassPatchKeyHash> PatchInFlight;
   std::vector<GrassPatchJobResult> PatchResults;
   std::mutex PatchResultMutex;

   ~GrassLayerRuntime()
   {
      if (bgfx::isValid(InstanceBuffer))
      {
         bgfx::destroy(InstanceBuffer);
         InstanceBuffer = BGFX_INVALID_HANDLE;
      }
      if (bgfx::isValid(CounterTexture))
      {
         bgfx::destroy(CounterTexture);
         CounterTexture = BGFX_INVALID_HANDLE;
      }
      if (bgfx::isValid(IndirectArgs))
      {
         bgfx::destroy(IndirectArgs);
         IndirectArgs = BGFX_INVALID_HANDLE;
      }
      Capacity = 0;
   }

   void Reset()
   {
      if (bgfx::isValid(InstanceBuffer))
      {
         bgfx::destroy(InstanceBuffer);
         InstanceBuffer = BGFX_INVALID_HANDLE;
      }
      if (bgfx::isValid(CounterTexture))
      {
         bgfx::destroy(CounterTexture);
         CounterTexture = BGFX_INVALID_HANDLE;
      }
      if (bgfx::isValid(IndirectArgs))
      {
         bgfx::destroy(IndirectArgs);
         IndirectArgs = BGFX_INVALID_HANDLE;
      }
      Capacity = 0;
      CpuInstanceCache.clear();
      CpuCacheValid = false;
      LastCpuUpdateTime = -1000.0f;
      PatchCache.clear();
      PatchInFlight.clear();
      PatchResults.clear();
   }
};

struct TerrainGrassLayerDesc
{
   ClaymoreGUID Guid = ClaymoreGUID::Generate();
   std::string Name = "Grass Layer";
   bool Enabled = true;
   bool UseGPU = false;  // Use GPU compute for grass generation (experimental)
   GrassRenderMode RenderMode = GrassRenderMode::Billboard;
   GrassMaskSource Mask = GrassMaskSource::SplatG;

   // Procedural splat sampling (CPU mode when Mask is a splat channel)
   uint32_t SplatSeed = static_cast<uint32_t>(Guid.low ^ Guid.high);
   float SplatNoiseScale = 0.15f;
   float SplatNoiseStrength = 0.5f;
   float SplatThreshold = 0.2f;  // Minimum splatmap channel value (0-1) required for grass to spawn. Higher values = cleaner separation from other textures.

   float DensityPerSquareMeter = 30.0f;
   glm::vec2 ScaleRange = glm::vec2(0.8f, 1.2f);
   float RandomYawDegrees = 180.0f;
   glm::vec2 HeightRange = glm::vec2(-10000.0f, 10000.0f);
   float MaxSlopeDegrees = 55.0f;
   float MinDistance = 0.5f;
   float MaxDistance = 80.0f;
   float WindStrength = 1.0f;
   float WindDirectionDegrees = 0.0f;  // Wind direction in degrees (0 = +X, 90 = +Z)
   glm::vec3 BaseColor = glm::vec3(0.85f, 0.9f, 0.65f);
   glm::vec3 ColorVariance = glm::vec3(0.1f);

   std::string BillboardTexturePath;
   bgfx::TextureHandle BillboardTexture = BGFX_INVALID_HANDLE;

   AssetReference MeshAsset;
   std::string MeshPath;
   std::shared_ptr<Mesh> Mesh;

   // TODO(grass-density): Legacy grass density is stored as a CPU array per texel.
   // Painter writes into PaintedMask, but runtime later bakes it into CPU chunks.
   // We'll replace this with a texture-backed density map shared with terrain data.
   std::vector<uint8_t> PaintedMask;
   bool RuntimeDirty = true;
   bgfx::TextureHandle DensityTexture = BGFX_INVALID_HANDLE;
   uint32_t DensityTextureResolution = 0;
   glm::ivec2 DensityDirtyMin = glm::ivec2(0);
   glm::ivec2 DensityDirtyMax = glm::ivec2(0);
   bool DensityDirty = true;
   std::unique_ptr<GrassLayerRuntime> Runtime;

   TerrainGrassLayerDesc() = default;
   TerrainGrassLayerDesc(const TerrainGrassLayerDesc& other);
   TerrainGrassLayerDesc(TerrainGrassLayerDesc&& other) noexcept;
   TerrainGrassLayerDesc& operator=(const TerrainGrassLayerDesc& other);
   TerrainGrassLayerDesc& operator=(TerrainGrassLayerDesc&& other) noexcept;
   ~TerrainGrassLayerDesc() { ReleaseHandles(); }

   void EnsureMaskSize(uint32_t gridResolution)
   {
      const size_t count = static_cast<size_t>(gridResolution) * static_cast<size_t>(gridResolution);
      if (PaintedMask.size() != count)
      {
         PaintedMask.assign(count, 0);
         RuntimeDirty = true;
         DensityDirty = true;
         DensityDirtyMin = glm::ivec2(0);
         DensityDirtyMax = glm::ivec2(static_cast<int>(gridResolution) - 1);
      }
   }

   void ReleaseHandles()
   {
      // BillboardTexture is a shared asset handle, so just drop our reference.
      BillboardTexture = BGFX_INVALID_HANDLE;
      Mesh.reset();
      if (bgfx::isValid(DensityTexture))
      {
         bgfx::destroy(DensityTexture);
         DensityTexture = BGFX_INVALID_HANDLE;
      }
      DensityTextureResolution = 0;
      if (Runtime)
      {
         Runtime->Reset();
         Runtime.reset();
      }
   }
};

// Chunk streaming state for background loading
enum class ChunkStreamState : uint8_t
{
   Unloaded = 0,    // Data not in memory
   Loading = 1,     // Background load in progress
   Loaded = 2,      // Data ready, GPU upload pending
   Resident = 3     // Fully loaded and GPU-resident
};

struct TerrainChunk
{
   glm::ivec2 Start = glm::ivec2(0);      // Grid position in vertex indices
   uint32_t VertexCountX = 0;
   uint32_t VertexCountZ = 0;
   glm::ivec2 DirtyMin = glm::ivec2(0);
   glm::ivec2 DirtyMax = glm::ivec2(0);
   bool HeightDirty = true;
   bool SplatDirty = true;
   bool HoleDirty = true;
   bgfx::TextureHandle HeightTexture = BGFX_INVALID_HANDLE;
   bgfx::TextureHandle SplatTexture = BGFX_INVALID_HANDLE;   // Layers 0-3 weights
   bgfx::TextureHandle SplatTexture2 = BGFX_INVALID_HANDLE;  // Layers 4-7 weights (optional)
   bgfx::TextureHandle HoleTexture = BGFX_INVALID_HANDLE;    // Visibility mask: 0=solid, 255=hole
   
   // LOD runtime state (updated per frame)
   int CurrentLOD = 0;           // Current LOD level (0 = highest detail)
   int DesiredLOD = 0;           // Target LOD before restriction enforcement
   bool Visible = true;          // Frustum culling result
   float DistanceToCamera = 0.0f; // For LOD selection
   float MorphFactor = 0.0f;     // LOD transition blend (0 = this LOD, 1 = coarser)
   glm::vec3 WorldMin{0.0f};     // AABB for culling (local space)
   glm::vec3 WorldMax{0.0f};
   
   // Chunk-based rendering (UV mapping into unified heightmap/splatmap)
   glm::vec2 UVOffset{0.0f};     // Where this chunk starts in UV space (0-1)
   glm::vec2 UVScale{1.0f};      // Size in UV space
   glm::vec2 WorldOffset{0.0f};  // World XZ position (local to terrain)
   glm::vec2 WorldExtent{0.0f};  // World XZ size
   
   // Neighbor LOD levels for edge morphing (N, E, S, W)
   // -1 means no neighbor (edge of terrain)
   int NeighborLODs[4] = { -1, -1, -1, -1 };
   
   // Grid position within chunk array
   uint32_t GridX = 0;
   uint32_t GridZ = 0;
   
   // Streaming state
   ChunkStreamState StreamState = ChunkStreamState::Resident;
};

// Terrain LOD configuration
struct TerrainLODConfig
{
   bool Enabled = true;
   static constexpr uint32_t kMaxLODLevels = 4;
   float LODDistances[kMaxLODLevels] = { 50.0f, 150.0f, 400.0f, 1000.0f };
   uint32_t LODSteps[kMaxLODLevels] = { 1, 2, 4, 8 }; // Vertex skip factors
};

// Grass deformation configuration
struct GrassDeformationConfig
{
   bool Enabled = true;
   uint32_t TextureResolution = 128;  // Resolution of deformation texture (power of 2)
   float DecayRate = 2.0f;            // How fast deformation fades (units/sec)
   float DefaultRadius = 0.5f;        // Default deformer radius in world units
   float DefaultStrength = 1.0f;      // Default deformation strength (0-1)
   float MaxDeformation = 1.0f;       // Maximum bend amount (0-1 of blade height)
};

// Individual grass deformer instance (tracked per-entity)
struct GrassDeformerInstance
{
   glm::vec3 Position = glm::vec3(0.0f);
   glm::vec3 Velocity = glm::vec3(0.0f);  // For directional deformation
   float Radius = 0.5f;
   float Strength = 1.0f;
   bool Active = true;
};

struct TerrainInstancerCollisionSettings
{
   bool Enabled = false;
   float ActivationDistance = 35.0f;
   uint32_t MaxActiveBodies = 128;
   uint32_t PhysicsLayer = 0;
   std::string PhysicsLayerName = "Default";
   bool UseSharedMeshShape = true;
};

struct TerrainInstancerLayerDesc
{
   ClaymoreGUID Guid = ClaymoreGUID::Generate();
   std::string Name = "Instancer Layer";
   bool Enabled = true;
   TerrainInstancerMaskSource Mask = TerrainInstancerMaskSource::Painted;
   float SplatThreshold = 0.2f;

   cm::instancer::InstancerComponent Instancer;
   TerrainInstancerCollisionSettings Collision;

   std::vector<uint8_t> PaintedMask;
   bool PaintedMaskBoundsDirty = true;
   bool PaintedMaskHasCoverage = false;
   glm::ivec2 PaintedMaskMin = glm::ivec2(0);
   glm::ivec2 PaintedMaskMax = glm::ivec2(0);
   bool RuntimeDirty = true;
   bool RuntimeRegionDirty = false;
   glm::ivec2 RuntimeDirtyMin = glm::ivec2(0);
   glm::ivec2 RuntimeDirtyMax = glm::ivec2(0);
   bool RuntimeRebuildInProgress = false;
   uint64_t RuntimeRebuildNextCell = 0;

   JPH::RefConst<JPH::Shape> SharedCollisionShape;
   std::unordered_map<uint32_t, JPH::BodyID> ActiveCollisionBodies;

   TerrainInstancerLayerDesc();
   TerrainInstancerLayerDesc(const TerrainInstancerLayerDesc& other);
   TerrainInstancerLayerDesc(TerrainInstancerLayerDesc&& other) noexcept;
   TerrainInstancerLayerDesc& operator=(const TerrainInstancerLayerDesc& other);
   TerrainInstancerLayerDesc& operator=(TerrainInstancerLayerDesc&& other) noexcept;
   ~TerrainInstancerLayerDesc();

   void EnsureMaskSize(uint32_t gridResolution);
   void InvalidatePaintedMaskBounds();
   void RebuildPaintedMaskBounds(uint32_t gridResolution);
   void MarkRuntimeDirty();
   void MarkRuntimeRegionDirty(const glm::ivec2& minCell, const glm::ivec2& maxCell);
   void ReleaseCollisionBodies();
   void ReleaseRuntime();
};

struct TerrainComponent
{
   uint32_t GridResolution = 256;
   glm::vec2 WorldSize = glm::vec2(64.0f, 64.0f);
   float MaxHeight = 20.0f;
   uint32_t ChunkResolution = 256;  // Size of each LOD chunk (in vertices)
   uint32_t GrassChunkResolution = 32;
   uint32_t GrassSamplingMultiplier = 1;  // Multiplier for grass grid (1=terrain res, 2=2x, 4=4x, etc.)

   TerrainBrush Brush;

   std::vector<uint16_t> HeightMap;
   std::vector<glm::u8vec4> SplatMap;       // Layers 0-3 weights (RGBA)
   std::vector<glm::u8vec4> SplatMap2;      // Layers 4-7 weights (RGBA) - optional for 8-layer support
   std::vector<uint8_t> HoleMask;           // 0=solid, 255=hole
   std::vector<TerrainLayerDesc> Layers;
   std::vector<TerrainGrassLayerDesc> GrassLayers;
   std::vector<TerrainInstancerLayerDesc> InstancerLayers;

   std::vector<TerrainChunk> Chunks;
   bgfx::VertexBufferHandle ChunkVB = BGFX_INVALID_HANDLE;
   bgfx::IndexBufferHandle ChunkIB = BGFX_INVALID_HANDLE;
   
   // LOD System (legacy index-buffer based - disabled by default)
   TerrainLODConfig LODConfig;
   std::array<bgfx::IndexBufferHandle, TerrainLODConfig::kMaxLODLevels> LODIndexBuffers = {{
      BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE
   }};
   std::array<uint32_t, TerrainLODConfig::kMaxLODLevels> LODIndexCounts = {{ 0, 0, 0, 0 }};
   uint32_t LODLevelCount = 0;
   bool LODBuffersDirty = true;
   uint32_t ChunksX = 0;  // Number of chunks along X axis
   uint32_t ChunksZ = 0;  // Number of chunks along Z axis
   
   // Geometry Clipmap System (modern GPU-friendly LOD)
   // When enabled, uses concentric ring meshes for efficient terrain rendering
   bool UseClipmaps = false;              // Enable clipmap rendering (now default false - prefer chunked)
   uint32_t ClipmapLevels = 4;            // Number of clipmap LOD levels
   uint32_t ClipmapGridSize = 64;         // Vertices per clipmap block (power of 2)
   bool ClipmapMorphing = true;           // Enable smooth LOD transitions
   // Runtime clipmap state (managed by renderer, not serialized)
   void* ClipmapSystem = nullptr;         // Pointer to terrain::TerrainClipmapSystem (opaque)
   
   // Chunked Terrain System (Skyrim-style cells with unified textures)
   // When enabled, terrain is divided into NxN chunks for efficient culling/LOD
   bool UseChunkedTerrain = true;         // Enable chunked rendering (default: true)
   uint32_t ChunkVertexSize = 33;         // Vertices per chunk edge (power of 2 + 1: 17, 33, 65)
   bool ChunkMorphing = true;             // Enable smooth LOD transitions at chunk boundaries
   float ChunkMorphRegion = 0.3f;         // Fraction of LOD distance band for morphing
   bool ChunkStreaming = false;           // Enable background chunk streaming (for large worlds)
   float StreamingLoadRadius = 500.0f;    // Distance to start loading chunks
   float StreamingUnloadRadius = 600.0f;  // Distance to unload chunks
   void* ChunkStreamingSystem = nullptr;  // Runtime streaming state (terrain::TerrainStreamingSystem)
   // Per-render call token used to avoid recomputing chunk visibility/LOD for both
   // shadow and color terrain passes in the same frame.
   uint64_t RuntimeChunkPrepToken = 0;
   
   // Shared chunk mesh buffers (all chunks share same VB, use per-LOD IBs)
   bgfx::VertexBufferHandle SharedChunkVB = BGFX_INVALID_HANDLE;
   std::array<bgfx::IndexBufferHandle, TerrainLODConfig::kMaxLODLevels> ChunkLODIndexBuffers = {{
      BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE
   }};
   std::array<uint32_t, TerrainLODConfig::kMaxLODLevels> ChunkLODIndexCounts = {{ 0, 0, 0, 0 }};
   bool ChunkMeshDirty = true;            // Rebuild chunk mesh buffers

   // Physics collision body (heightfield-based)
   JPH::BodyID PhysicsBodyID = JPH::BodyID();
   bool PhysicsDirty = true; // Set to true when height data changes and physics needs rebuild
   ClaymoreGUID TerrainDataGuid = ClaymoreGUID::Generate();
   std::string AssetPath;
   bool AssetDirty = true;

   bool MeshDirty = true;
   bool HeightDataDirty = true;
   bool SplatDataDirty = true;
   bool HoleDataDirty = true;
   bool GrassStructureDirty = true;
   bool GrassMasksDirty = true;
   bool InstancerLayersDirty = true;

   // Layer Texture Array System (for 8-layer support)
   // All layer textures are resized to this resolution and stored in texture arrays
   uint32_t LayerTextureResolution = 1024;  // 256, 512, 1024, 2048
   TerrainTextureFilter LayerResizeFilter = TerrainTextureFilter::Bilinear;
   bool LayerTextureArraysDirty = true;     // Set when layers/resolution/filter change

   // Editor/runtime transient: terrain brushes can touch many mask/height samples
   // per frame. Defer expensive instancer regeneration until the active stroke ends.
   uint32_t DeferredInstancerDirtyDepth = 0;
   bool DeferredInstancerFullDirty = false;
   std::vector<size_t> DeferredInstancerLayerDirty;
   struct DeferredInstancerRegion
   {
      size_t LayerIndex = 0;
      glm::ivec2 Min = glm::ivec2(0);
      glm::ivec2 Max = glm::ivec2(0);
   };
   std::vector<DeferredInstancerRegion> DeferredInstancerRegions;

   // Grass Deformation System
   GrassDeformationConfig DeformationConfig;
   bgfx::TextureHandle DeformationTexture = BGFX_INVALID_HANDLE;
   std::vector<glm::vec4> DeformationData;  // CPU-side deformation buffer (RG=direction*strength, B=unused, A=time)
   bool DeformationDirty = false;
   float DeformationTime = 0.0f;  // Accumulated time for decay calculation

   TerrainComponent();
   TerrainComponent(const TerrainComponent& other);
   TerrainComponent& operator=(const TerrainComponent& other);
   ~TerrainComponent();

   void Resize(uint32_t newResolution);
   void ResizeWithResampling(uint32_t newResolution);  // Preserves terrain data via bilinear interpolation
   void EnsureMapSize();
   void EnsureSplatMap2();  // Allocates SplatMap2 when using more than 4 layers
   void DestroyGpuResources();
   void MarkDataDirty();
   void MarkInstancersDirty();
   void MarkInstancerLayerDirty(size_t layerIndex);
   void MarkInstancerLayerRegionDirty(size_t layerIndex, const glm::ivec2& minCell, const glm::ivec2& maxCell);
   void BeginDeferredInstancerDirty();
   void EndDeferredInstancerDirty();
   static std::string BuildDefaultAssetPath(const ClaymoreGUID& guid);
   void ResetAssetIdentity();

private:
   void CopyFrom(const TerrainComponent& other);
};

//------------------------------------------------------------------------------
// River Component - Stores river path data for mesh generation and serialization
//------------------------------------------------------------------------------
struct RiverPathPoint
{
   glm::vec3 Position{ 0.0f };   // Local position relative to terrain
   glm::vec3 Normal{ 0.0f, 1.0f, 0.0f };
};

//------------------------------------------------------------------------------
// Spline Component - Catmull-Rom spline path for scripting and instancer
// Points are in local space relative to the entity's transform.
//------------------------------------------------------------------------------
struct SplinePathPoint
{
   glm::vec3 Position{ 0.0f };   // Local position relative to spline entity
   glm::vec3 Normal{ 0.0f, 1.0f, 0.0f };
};

struct SplineComponent
{
   std::vector<SplinePathPoint> ControlPoints;
   int SplineSubdivision = 4;    // Subdivisions between control points for smooth curves
   bool Closed = false;          // If true, connect last point to first
};

struct RiverComponent
{
   // Path control points
   std::vector<RiverPathPoint> PathPoints;
   
   // Generation settings
   float Width = 2.0f;           // River width (uses terrain brush radius by default)
   float Depth = 1.5f;           // Carve depth
   float BankSmoothing = 0.7f;
   int SplineSubdivision = 4;
   float WaterHeight = 0.1f;     // Height offset above carved terrain for water surface
   
   // Material settings
   std::string MaterialName = "Water";
   glm::vec4 WaterColor{ 0.2f, 0.4f, 0.6f, 0.8f };
   float FlowSpeed = 1.0f;
   
   // Generated mesh entity (child of terrain)
   EntityID MeshEntity = INVALID_ENTITY_ID;
   
   // Asset path for binary mesh data (similar to terrain asset pattern)
   std::string MeshAssetPath;
   ClaymoreGUID MeshAssetGuid = ClaymoreGUID::Generate();
   bool MeshAssetDirty = false;
   
   // Flags
   bool NeedsRegeneration = true;
   bool MeshGenerated = false;
   
   // Build default asset path from GUID
   static std::string BuildDefaultMeshAssetPath(const ClaymoreGUID& guid)
   {
      return "assets/rivers/" + guid.ToString() + ".rivermesh";
   }
};

inline void TerrainLayerDesc::ReleaseHandles()
{
   // Path-backed terrain textures are shared asset handles; the cache owns
   // the underlying bgfx lifetime.
   AlbedoHandle = BGFX_INVALID_HANDLE;
   NormalHandle = BGFX_INVALID_HANDLE;
}

inline TerrainLayerDesc::TerrainLayerDesc(const TerrainLayerDesc& other)
   : Name(other.Name)
   , AlbedoPath(other.AlbedoPath)
   , NormalPath(other.NormalPath)
   , Tiling(other.Tiling)
   , NavCost(other.NavCost)
   , PlaceholderColor(other.PlaceholderColor)
{
   AlbedoHandle = BGFX_INVALID_HANDLE;
   NormalHandle = BGFX_INVALID_HANDLE;
}

inline TerrainLayerDesc& TerrainLayerDesc::operator=(const TerrainLayerDesc& other)
{
   if (this == &other) return *this;
   ReleaseHandles();
   Name = other.Name;
   AlbedoPath = other.AlbedoPath;
   NormalPath = other.NormalPath;
   Tiling = other.Tiling;
   NavCost = other.NavCost;
   PlaceholderColor = other.PlaceholderColor;
   AlbedoHandle = BGFX_INVALID_HANDLE;
   NormalHandle = BGFX_INVALID_HANDLE;
   return *this;
}

inline TerrainLayerDesc::TerrainLayerDesc(TerrainLayerDesc&& other) noexcept
   : Name(std::move(other.Name))
   , AlbedoPath(std::move(other.AlbedoPath))
   , NormalPath(std::move(other.NormalPath))
   , AlbedoHandle(other.AlbedoHandle)
   , NormalHandle(other.NormalHandle)
   , Tiling(other.Tiling)
   , NavCost(other.NavCost)
   , PlaceholderColor(other.PlaceholderColor)
{
   other.AlbedoHandle = BGFX_INVALID_HANDLE;
   other.NormalHandle = BGFX_INVALID_HANDLE;
}

inline TerrainLayerDesc& TerrainLayerDesc::operator=(TerrainLayerDesc&& other) noexcept
{
   if (this == &other) return *this;
   ReleaseHandles();
   Name = std::move(other.Name);
   AlbedoPath = std::move(other.AlbedoPath);
   NormalPath = std::move(other.NormalPath);
   Tiling = other.Tiling;
   NavCost = other.NavCost;
   PlaceholderColor = other.PlaceholderColor;
   AlbedoHandle = other.AlbedoHandle;
   NormalHandle = other.NormalHandle;
   other.AlbedoHandle = BGFX_INVALID_HANDLE;
   other.NormalHandle = BGFX_INVALID_HANDLE;
   return *this;
}

inline TerrainGrassLayerDesc::TerrainGrassLayerDesc(const TerrainGrassLayerDesc& other)
   : Guid(other.Guid)
   , Name(other.Name)
   , Enabled(other.Enabled)
   , UseGPU(other.UseGPU)
   , RenderMode(other.RenderMode)
   , Mask(other.Mask)
   , SplatSeed(other.SplatSeed)
   , SplatNoiseScale(other.SplatNoiseScale)
   , SplatNoiseStrength(other.SplatNoiseStrength)
   , SplatThreshold(other.SplatThreshold)
   , DensityPerSquareMeter(other.DensityPerSquareMeter)
   , ScaleRange(other.ScaleRange)
   , RandomYawDegrees(other.RandomYawDegrees)
   , HeightRange(other.HeightRange)
   , MaxSlopeDegrees(other.MaxSlopeDegrees)
   , MinDistance(other.MinDistance)
   , MaxDistance(other.MaxDistance)
   , WindStrength(other.WindStrength)
   , WindDirectionDegrees(other.WindDirectionDegrees)
   , BaseColor(other.BaseColor)
   , ColorVariance(other.ColorVariance)
   , BillboardTexturePath(other.BillboardTexturePath)
   , MeshAsset(other.MeshAsset)
   , MeshPath(other.MeshPath)
   , PaintedMask(other.PaintedMask)
   , RuntimeDirty(true)
   , DensityDirty(true)
{
   BillboardTexture = BGFX_INVALID_HANDLE;
   Mesh.reset();
   DensityTexture = BGFX_INVALID_HANDLE;
   DensityTextureResolution = 0;
   DensityDirtyMin = glm::ivec2(0);
   DensityDirtyMax = glm::ivec2(0);
}

inline TerrainGrassLayerDesc& TerrainGrassLayerDesc::operator=(const TerrainGrassLayerDesc& other)
{
   if (this == &other) return *this;
   ReleaseHandles();
   Guid = other.Guid;
   Name = other.Name;
   Enabled = other.Enabled;
   UseGPU = other.UseGPU;
   RenderMode = other.RenderMode;
   Mask = other.Mask;
   SplatSeed = other.SplatSeed;
   SplatNoiseScale = other.SplatNoiseScale;
   SplatNoiseStrength = other.SplatNoiseStrength;
   SplatThreshold = other.SplatThreshold;
   DensityPerSquareMeter = other.DensityPerSquareMeter;
   ScaleRange = other.ScaleRange;
   RandomYawDegrees = other.RandomYawDegrees;
   HeightRange = other.HeightRange;
   MaxSlopeDegrees = other.MaxSlopeDegrees;
   MinDistance = other.MinDistance;
   MaxDistance = other.MaxDistance;
   WindStrength = other.WindStrength;
   WindDirectionDegrees = other.WindDirectionDegrees;
   BaseColor = other.BaseColor;
   ColorVariance = other.ColorVariance;
   BillboardTexturePath = other.BillboardTexturePath;
   MeshAsset = other.MeshAsset;
   MeshPath = other.MeshPath;
   PaintedMask = other.PaintedMask;
   BillboardTexture = BGFX_INVALID_HANDLE;
   Mesh.reset();
   RuntimeDirty = true;
   DensityTexture = BGFX_INVALID_HANDLE;
   DensityTextureResolution = 0;
   DensityDirty = true;
   DensityDirtyMin = glm::ivec2(0);
   DensityDirtyMax = glm::ivec2(0);
   Runtime.reset();
   return *this;
}

inline TerrainGrassLayerDesc::TerrainGrassLayerDesc(TerrainGrassLayerDesc&& other) noexcept
   : Guid(other.Guid)
   , Name(std::move(other.Name))
   , Enabled(other.Enabled)
   , UseGPU(other.UseGPU)
   , RenderMode(other.RenderMode)
   , Mask(other.Mask)
   , SplatSeed(other.SplatSeed)
   , SplatNoiseScale(other.SplatNoiseScale)
   , SplatNoiseStrength(other.SplatNoiseStrength)
   , SplatThreshold(other.SplatThreshold)
   , DensityPerSquareMeter(other.DensityPerSquareMeter)
   , ScaleRange(other.ScaleRange)
   , RandomYawDegrees(other.RandomYawDegrees)
   , HeightRange(other.HeightRange)
   , MaxSlopeDegrees(other.MaxSlopeDegrees)
   , MinDistance(other.MinDistance)
   , MaxDistance(other.MaxDistance)
   , WindStrength(other.WindStrength)
   , WindDirectionDegrees(other.WindDirectionDegrees)
   , BaseColor(other.BaseColor)
   , ColorVariance(other.ColorVariance)
   , BillboardTexturePath(std::move(other.BillboardTexturePath))
   , BillboardTexture(other.BillboardTexture)
   , MeshAsset(other.MeshAsset)
   , MeshPath(std::move(other.MeshPath))
   , Mesh(std::move(other.Mesh))
   , PaintedMask(std::move(other.PaintedMask))
   , RuntimeDirty(other.RuntimeDirty)
   , DensityTexture(other.DensityTexture)
   , DensityTextureResolution(other.DensityTextureResolution)
   , DensityDirtyMin(other.DensityDirtyMin)
   , DensityDirtyMax(other.DensityDirtyMax)
   , DensityDirty(other.DensityDirty)
   , Runtime(std::move(other.Runtime))
{
   // Invalidate moved-from handles to prevent double destruction
   other.BillboardTexture = BGFX_INVALID_HANDLE;
   other.DensityTexture = BGFX_INVALID_HANDLE;
}

inline TerrainGrassLayerDesc& TerrainGrassLayerDesc::operator=(TerrainGrassLayerDesc&& other) noexcept
{
   if (this == &other) return *this;
   ReleaseHandles();
   Guid = other.Guid;
   Name = std::move(other.Name);
   Enabled = other.Enabled;
   UseGPU = other.UseGPU;
   RenderMode = other.RenderMode;
   Mask = other.Mask;
   SplatSeed = other.SplatSeed;
   SplatNoiseScale = other.SplatNoiseScale;
   SplatNoiseStrength = other.SplatNoiseStrength;
   SplatThreshold = other.SplatThreshold;
   DensityPerSquareMeter = other.DensityPerSquareMeter;
   ScaleRange = other.ScaleRange;
   RandomYawDegrees = other.RandomYawDegrees;
   HeightRange = other.HeightRange;
   MaxSlopeDegrees = other.MaxSlopeDegrees;
   MinDistance = other.MinDistance;
   MaxDistance = other.MaxDistance;
   WindStrength = other.WindStrength;
   WindDirectionDegrees = other.WindDirectionDegrees;
   BaseColor = other.BaseColor;
   ColorVariance = other.ColorVariance;
   BillboardTexturePath = std::move(other.BillboardTexturePath);
   BillboardTexture = other.BillboardTexture;
   MeshAsset = other.MeshAsset;
   MeshPath = std::move(other.MeshPath);
   Mesh = std::move(other.Mesh);
   PaintedMask = std::move(other.PaintedMask);
   RuntimeDirty = other.RuntimeDirty;
   DensityTexture = other.DensityTexture;
   DensityTextureResolution = other.DensityTextureResolution;
   DensityDirtyMin = other.DensityDirtyMin;
   DensityDirtyMax = other.DensityDirtyMax;
   DensityDirty = other.DensityDirty;
   Runtime = std::move(other.Runtime);
   // Invalidate moved-from handles
   other.BillboardTexture = BGFX_INVALID_HANDLE;
   other.DensityTexture = BGFX_INVALID_HANDLE;
   return *this;
}

inline TerrainInstancerLayerDesc::TerrainInstancerLayerDesc()
{
   Instancer.Distribution.DensityPerSquareMeter = 0.025f;
   Instancer.Distribution.MinSpacing = 4.0f;
   Instancer.Distribution.MinScale = 0.8f;
   Instancer.Distribution.MaxScale = 1.25f;
   Instancer.Distribution.MaxSlopeDegrees = 55.0f;
   Instancer.Swap.SwapDistance = 35.0f;
   Instancer.Swap.CullDistance = 450.0f;
   Instancer.Swap.MaxActivePrefabs = 32;
   Instancer.PreviewColor = glm::vec3(0.18f, 0.65f, 0.32f);
}

inline TerrainInstancerLayerDesc::TerrainInstancerLayerDesc(const TerrainInstancerLayerDesc& other)
   : Guid(other.Guid)
   , Name(other.Name)
   , Enabled(other.Enabled)
   , Mask(other.Mask)
   , SplatThreshold(other.SplatThreshold)
   , Instancer(other.Instancer)
   , Collision(other.Collision)
   , PaintedMask(other.PaintedMask)
   , PaintedMaskBoundsDirty(true)
   , PaintedMaskHasCoverage(false)
   , RuntimeDirty(true)
   , RuntimeRegionDirty(false)
   , RuntimeRebuildInProgress(false)
   , RuntimeRebuildNextCell(0)
{
}

inline TerrainInstancerLayerDesc& TerrainInstancerLayerDesc::operator=(const TerrainInstancerLayerDesc& other)
{
   if (this == &other) return *this;
   ReleaseRuntime();
   Guid = other.Guid;
   Name = other.Name;
   Enabled = other.Enabled;
   Mask = other.Mask;
   SplatThreshold = other.SplatThreshold;
   Instancer = other.Instancer;
   Collision = other.Collision;
   PaintedMask = other.PaintedMask;
   PaintedMaskBoundsDirty = true;
   PaintedMaskHasCoverage = false;
   PaintedMaskMin = glm::ivec2(0);
   PaintedMaskMax = glm::ivec2(0);
   RuntimeDirty = true;
   RuntimeRegionDirty = false;
   RuntimeDirtyMin = glm::ivec2(0);
   RuntimeDirtyMax = glm::ivec2(0);
   RuntimeRebuildInProgress = false;
   RuntimeRebuildNextCell = 0;
   return *this;
}

inline TerrainInstancerLayerDesc::TerrainInstancerLayerDesc(TerrainInstancerLayerDesc&& other) noexcept
   : Guid(other.Guid)
   , Name(std::move(other.Name))
   , Enabled(other.Enabled)
   , Mask(other.Mask)
   , SplatThreshold(other.SplatThreshold)
   , Instancer(std::move(other.Instancer))
   , Collision(std::move(other.Collision))
   , PaintedMask(std::move(other.PaintedMask))
   , PaintedMaskBoundsDirty(other.PaintedMaskBoundsDirty)
   , PaintedMaskHasCoverage(other.PaintedMaskHasCoverage)
   , PaintedMaskMin(other.PaintedMaskMin)
   , PaintedMaskMax(other.PaintedMaskMax)
   , RuntimeDirty(other.RuntimeDirty)
   , RuntimeRegionDirty(other.RuntimeRegionDirty)
   , RuntimeDirtyMin(other.RuntimeDirtyMin)
   , RuntimeDirtyMax(other.RuntimeDirtyMax)
   , RuntimeRebuildInProgress(other.RuntimeRebuildInProgress)
   , RuntimeRebuildNextCell(other.RuntimeRebuildNextCell)
   , SharedCollisionShape(std::move(other.SharedCollisionShape))
   , ActiveCollisionBodies(std::move(other.ActiveCollisionBodies))
{
   other.SharedCollisionShape = nullptr;
   other.ActiveCollisionBodies.clear();
   other.PaintedMaskBoundsDirty = true;
   other.PaintedMaskHasCoverage = false;
   other.RuntimeRegionDirty = false;
   other.RuntimeRebuildInProgress = false;
   other.RuntimeRebuildNextCell = 0;
}

inline TerrainInstancerLayerDesc& TerrainInstancerLayerDesc::operator=(TerrainInstancerLayerDesc&& other) noexcept
{
   if (this == &other) return *this;
   ReleaseRuntime();
   Guid = other.Guid;
   Name = std::move(other.Name);
   Enabled = other.Enabled;
   Mask = other.Mask;
   SplatThreshold = other.SplatThreshold;
   Instancer = std::move(other.Instancer);
   Collision = std::move(other.Collision);
   PaintedMask = std::move(other.PaintedMask);
   PaintedMaskBoundsDirty = other.PaintedMaskBoundsDirty;
   PaintedMaskHasCoverage = other.PaintedMaskHasCoverage;
   PaintedMaskMin = other.PaintedMaskMin;
   PaintedMaskMax = other.PaintedMaskMax;
   RuntimeDirty = other.RuntimeDirty;
   RuntimeRegionDirty = other.RuntimeRegionDirty;
   RuntimeDirtyMin = other.RuntimeDirtyMin;
   RuntimeDirtyMax = other.RuntimeDirtyMax;
   RuntimeRebuildInProgress = other.RuntimeRebuildInProgress;
   RuntimeRebuildNextCell = other.RuntimeRebuildNextCell;
   SharedCollisionShape = std::move(other.SharedCollisionShape);
   ActiveCollisionBodies = std::move(other.ActiveCollisionBodies);
   other.SharedCollisionShape = nullptr;
   other.ActiveCollisionBodies.clear();
   other.PaintedMaskBoundsDirty = true;
   other.PaintedMaskHasCoverage = false;
   other.RuntimeRegionDirty = false;
   other.RuntimeRebuildInProgress = false;
   other.RuntimeRebuildNextCell = 0;
   return *this;
}

inline TerrainInstancerLayerDesc::~TerrainInstancerLayerDesc()
{
   ReleaseRuntime();
}

inline void TerrainInstancerLayerDesc::EnsureMaskSize(uint32_t gridResolution)
{
   const size_t count = static_cast<size_t>(gridResolution) * static_cast<size_t>(gridResolution);
   if (PaintedMask.size() != count)
   {
      PaintedMask.assign(count, 0);
      InvalidatePaintedMaskBounds();
      MarkRuntimeDirty();
   }
}

inline void TerrainInstancerLayerDesc::InvalidatePaintedMaskBounds()
{
   PaintedMaskBoundsDirty = true;
}

inline void TerrainInstancerLayerDesc::RebuildPaintedMaskBounds(uint32_t gridResolution)
{
   if (!PaintedMaskBoundsDirty)
      return;

   PaintedMaskBoundsDirty = false;
   PaintedMaskHasCoverage = false;
   PaintedMaskMin = glm::ivec2(0);
   PaintedMaskMax = glm::ivec2(0);

   const size_t count = static_cast<size_t>(gridResolution) * static_cast<size_t>(gridResolution);
   if (gridResolution < 2 || PaintedMask.size() != count)
      return;

   glm::ivec2 minCell(std::numeric_limits<int>::max());
   glm::ivec2 maxCell(std::numeric_limits<int>::min());
   for (uint32_t z = 0; z < gridResolution; ++z)
   {
      for (uint32_t x = 0; x < gridResolution; ++x)
      {
         const size_t idx = static_cast<size_t>(z) * gridResolution + x;
         if (PaintedMask[idx] == 0)
            continue;

         minCell.x = std::min(minCell.x, static_cast<int>(x));
         minCell.y = std::min(minCell.y, static_cast<int>(z));
         maxCell.x = std::max(maxCell.x, static_cast<int>(x));
         maxCell.y = std::max(maxCell.y, static_cast<int>(z));
      }
   }

   if (minCell.x <= maxCell.x && minCell.y <= maxCell.y)
   {
      PaintedMaskHasCoverage = true;
      PaintedMaskMin = minCell;
      PaintedMaskMax = maxCell;
   }
}

inline void TerrainInstancerLayerDesc::MarkRuntimeDirty()
{
   InvalidatePaintedMaskBounds();
   RuntimeDirty = true;
   RuntimeRegionDirty = false;
   RuntimeRebuildInProgress = false;
   RuntimeRebuildNextCell = 0;
   Instancer.NeedsRegeneration = true;
}

inline void TerrainInstancerLayerDesc::MarkRuntimeRegionDirty(const glm::ivec2& minCell, const glm::ivec2& maxCell)
{
   if (RuntimeDirty || Instancer.NeedsRegeneration)
      return;

   if (!RuntimeRebuildInProgress)
   {
      RuntimeDirty = false;
      Instancer.NeedsRegeneration = false;
      RuntimeRebuildNextCell = 0;
   }

   if (!RuntimeRegionDirty)
   {
      RuntimeDirtyMin = minCell;
      RuntimeDirtyMax = maxCell;
      RuntimeRegionDirty = true;
   }
   else
   {
      RuntimeDirtyMin = glm::min(RuntimeDirtyMin, minCell);
      RuntimeDirtyMax = glm::max(RuntimeDirtyMax, maxCell);
   }
}

inline void TerrainInstancerLayerDesc::ReleaseCollisionBodies()
{
   if (Physics::GetSystem() != nullptr)
   {
      for (auto& entry : ActiveCollisionBodies)
      {
         if (!entry.second.IsInvalid())
            Physics::DestroyBody(entry.second);
      }
   }
   ActiveCollisionBodies.clear();
   SharedCollisionShape = nullptr;
}

inline void TerrainInstancerLayerDesc::ReleaseRuntime()
{
   ReleaseCollisionBodies();
   Instancer.ClearCache();
   RuntimeDirty = true;
   RuntimeRegionDirty = false;
   RuntimeRebuildInProgress = false;
   RuntimeRebuildNextCell = 0;
}

inline TerrainComponent::TerrainComponent()
{
   EnsureMapSize();
   AssetPath = BuildDefaultAssetPath(TerrainDataGuid);
   Layers.reserve(4);

   auto addLayer = [this](const char* name, const glm::vec3& color)
   {
      TerrainLayerDesc layer;
      layer.Name = name;
      layer.PlaceholderColor = color;
      Layers.push_back(std::move(layer));
   };

   addLayer("Layer 0", glm::vec3(0.34f, 0.62f, 0.29f));
   addLayer("Layer 1", glm::vec3(0.62f, 0.54f, 0.33f));
   addLayer("Layer 2", glm::vec3(0.55f, 0.57f, 0.58f));
   addLayer("Layer 3", glm::vec3(0.87f, 0.84f, 0.65f));

   GrassLayers.emplace_back();
   GrassLayers.back().Name = "Default Grass";
   GrassLayers.back().EnsureMaskSize(GridResolution);

   InstancerLayers.emplace_back();
   InstancerLayers.back().Name = "Default Instancer";
   InstancerLayers.back().EnsureMaskSize(GridResolution);
}

inline TerrainComponent::TerrainComponent(const TerrainComponent& other)
{
   CopyFrom(other);
}

inline TerrainComponent& TerrainComponent::operator=(const TerrainComponent& other)
{
   if (this == &other) return *this;
   DestroyGpuResources();
   CopyFrom(other);
   return *this;
}

inline TerrainComponent::~TerrainComponent()
{
   DestroyGpuResources();
   // Clean up physics body directly (avoid needing full Terrain definition here)
   // Only destroy if physics system is still valid (check before accessing)
   if (!PhysicsBodyID.IsInvalid() && Physics::GetSystem() != nullptr)
   {
      Physics::DestroyBody(PhysicsBodyID);
      PhysicsBodyID = JPH::BodyID();
   }
}

inline void TerrainComponent::CopyFrom(const TerrainComponent& other)
{
   GridResolution = other.GridResolution;
   WorldSize = other.WorldSize;
   MaxHeight = other.MaxHeight;
   ChunkResolution = other.ChunkResolution;
   GrassChunkResolution = other.GrassChunkResolution;
   GrassSamplingMultiplier = other.GrassSamplingMultiplier;
   Brush = other.Brush;
   HeightMap = other.HeightMap;
   SplatMap = other.SplatMap;
   SplatMap2 = other.SplatMap2;
   HoleMask = other.HoleMask;
   Layers = other.Layers;
   GrassLayers = other.GrassLayers;
   InstancerLayers = other.InstancerLayers;
   TerrainDataGuid = other.TerrainDataGuid;
   AssetPath = other.AssetPath;
   AssetDirty = other.AssetDirty;
   
   // Clipmap settings (runtime state is not copied - will be re-created)
   UseClipmaps = other.UseClipmaps;
   ClipmapLevels = other.ClipmapLevels;
   ClipmapGridSize = other.ClipmapGridSize;
   ClipmapMorphing = other.ClipmapMorphing;
   ClipmapSystem = nullptr;  // Runtime state - will be initialized on render
   
   // Chunked terrain settings
   UseChunkedTerrain = other.UseChunkedTerrain;
   ChunkVertexSize = other.ChunkVertexSize;
   ChunkMorphing = other.ChunkMorphing;
   ChunkMorphRegion = other.ChunkMorphRegion;
   ChunkStreaming = other.ChunkStreaming;
   StreamingLoadRadius = other.StreamingLoadRadius;
   StreamingUnloadRadius = other.StreamingUnloadRadius;
   ChunkStreamingSystem = nullptr;  // Runtime state - will be initialized on render
   RuntimeChunkPrepToken = 0;       // Runtime cache state
   SharedChunkVB = BGFX_INVALID_HANDLE;
   for (auto& ib : ChunkLODIndexBuffers)
      ib = BGFX_INVALID_HANDLE;
   ChunkLODIndexCounts = {{ 0, 0, 0, 0 }};
   ChunkMeshDirty = true;

   ChunkVB = BGFX_INVALID_HANDLE;
   ChunkIB = BGFX_INVALID_HANDLE;
   Chunks.clear();
   MeshDirty = true;
   HeightDataDirty = true;
   SplatDataDirty = true;
   HoleDataDirty = true;
   GrassStructureDirty = true;
   GrassMasksDirty = true;
   InstancerLayersDirty = true;
   DeferredInstancerDirtyDepth = 0;
   DeferredInstancerFullDirty = false;
   DeferredInstancerLayerDirty.clear();
   DeferredInstancerRegions.clear();
   for (auto& grass : GrassLayers)
   {
      grass.EnsureMaskSize(GridResolution);
   }
   for (auto& layer : InstancerLayers)
   {
      layer.EnsureMaskSize(GridResolution);
      layer.MarkRuntimeDirty();
   }
   
   // Copy deformation config but not runtime state
   DeformationConfig = other.DeformationConfig;
   DeformationTexture = BGFX_INVALID_HANDLE;
   DeformationData.clear();
   DeformationDirty = false;
   DeformationTime = 0.0f;
}

inline void TerrainComponent::Resize(uint32_t newResolution)
{
   const uint32_t clamped = std::max(2u, std::min(4096u, newResolution));
   if (clamped == GridResolution) return;
   GridResolution = clamped;
   DestroyGpuResources();
   EnsureMapSize();
   MeshDirty = true;
   HeightDataDirty = true;
   SplatDataDirty = true;
   HoleDataDirty = true;
   PhysicsDirty = true;
   AssetDirty = true;
   GrassStructureDirty = true;
   GrassMasksDirty = true;
   MarkInstancersDirty();
   LODBuffersDirty = true;
}

inline void TerrainComponent::ResizeWithResampling(uint32_t newResolution)
{
   const uint32_t clamped = std::max(2u, std::min(4096u, newResolution));
   if (clamped == GridResolution) return;
   
   const uint32_t oldRes = GridResolution;
   const size_t oldCount = static_cast<size_t>(oldRes) * static_cast<size_t>(oldRes);
   const size_t newCount = static_cast<size_t>(clamped) * static_cast<size_t>(clamped);
   
   // Lambda for bilinear sampling from old data at normalized coordinates [0,1]
   auto sampleBilinear16 = [&](const std::vector<uint16_t>& src, float u, float v) -> uint16_t {
      if (src.size() != oldCount || oldRes < 2) return 0;
      const float maxCoord = static_cast<float>(oldRes - 1);
      const float xf = glm::clamp(u * maxCoord, 0.0f, maxCoord);
      const float zf = glm::clamp(v * maxCoord, 0.0f, maxCoord);
      const uint32_t x0 = static_cast<uint32_t>(std::floor(xf));
      const uint32_t z0 = static_cast<uint32_t>(std::floor(zf));
      const uint32_t x1 = std::min(x0 + 1, oldRes - 1);
      const uint32_t z1 = std::min(z0 + 1, oldRes - 1);
      const float tx = xf - static_cast<float>(x0);
      const float tz = zf - static_cast<float>(z0);
      const float v00 = static_cast<float>(src[z0 * oldRes + x0]);
      const float v10 = static_cast<float>(src[z0 * oldRes + x1]);
      const float v01 = static_cast<float>(src[z1 * oldRes + x0]);
      const float v11 = static_cast<float>(src[z1 * oldRes + x1]);
      const float vx0 = glm::mix(v00, v10, tx);
      const float vx1 = glm::mix(v01, v11, tx);
      return static_cast<uint16_t>(glm::clamp(glm::mix(vx0, vx1, tz), 0.0f, 65535.0f));
   };
   
   auto sampleBilinearU8Vec4 = [&](const std::vector<glm::u8vec4>& src, float u, float v) -> glm::u8vec4 {
      if (src.size() != oldCount || oldRes < 2) return glm::u8vec4(255, 0, 0, 0);
      const float maxCoord = static_cast<float>(oldRes - 1);
      const float xf = glm::clamp(u * maxCoord, 0.0f, maxCoord);
      const float zf = glm::clamp(v * maxCoord, 0.0f, maxCoord);
      const uint32_t x0 = static_cast<uint32_t>(std::floor(xf));
      const uint32_t z0 = static_cast<uint32_t>(std::floor(zf));
      const uint32_t x1 = std::min(x0 + 1, oldRes - 1);
      const uint32_t z1 = std::min(z0 + 1, oldRes - 1);
      const float tx = xf - static_cast<float>(x0);
      const float tz = zf - static_cast<float>(z0);
      const glm::vec4 v00 = glm::vec4(src[z0 * oldRes + x0]);
      const glm::vec4 v10 = glm::vec4(src[z0 * oldRes + x1]);
      const glm::vec4 v01 = glm::vec4(src[z1 * oldRes + x0]);
      const glm::vec4 v11 = glm::vec4(src[z1 * oldRes + x1]);
      const glm::vec4 vx0 = glm::mix(v00, v10, tx);
      const glm::vec4 vx1 = glm::mix(v01, v11, tx);
      const glm::vec4 result = glm::clamp(glm::mix(vx0, vx1, tz), 0.0f, 255.0f);
      return glm::u8vec4(static_cast<uint8_t>(result.r), static_cast<uint8_t>(result.g),
                         static_cast<uint8_t>(result.b), static_cast<uint8_t>(result.a));
   };
   
   auto sampleBilinearU8 = [&](const std::vector<uint8_t>& src, float u, float v) -> uint8_t {
      if (src.size() != oldCount || oldRes < 2) return 0;
      const float maxCoord = static_cast<float>(oldRes - 1);
      const float xf = glm::clamp(u * maxCoord, 0.0f, maxCoord);
      const float zf = glm::clamp(v * maxCoord, 0.0f, maxCoord);
      const uint32_t x0 = static_cast<uint32_t>(std::floor(xf));
      const uint32_t z0 = static_cast<uint32_t>(std::floor(zf));
      const uint32_t x1 = std::min(x0 + 1, oldRes - 1);
      const uint32_t z1 = std::min(z0 + 1, oldRes - 1);
      const float tx = xf - static_cast<float>(x0);
      const float tz = zf - static_cast<float>(z0);
      const float v00 = static_cast<float>(src[z0 * oldRes + x0]);
      const float v10 = static_cast<float>(src[z0 * oldRes + x1]);
      const float v01 = static_cast<float>(src[z1 * oldRes + x0]);
      const float v11 = static_cast<float>(src[z1 * oldRes + x1]);
      const float vx0 = glm::mix(v00, v10, tx);
      const float vx1 = glm::mix(v01, v11, tx);
      return static_cast<uint8_t>(glm::clamp(glm::mix(vx0, vx1, tz), 0.0f, 255.0f));
   };
   
   // Store old data
   std::vector<uint16_t> oldHeightMap = std::move(HeightMap);
   std::vector<glm::u8vec4> oldSplatMap = std::move(SplatMap);
   std::vector<glm::u8vec4> oldSplatMap2 = std::move(SplatMap2);
   std::vector<uint8_t> oldHoleMask = std::move(HoleMask);
   std::vector<std::vector<uint8_t>> oldGrassMasks;
   oldGrassMasks.reserve(GrassLayers.size());
   for (auto& grass : GrassLayers)
   {
      oldGrassMasks.push_back(std::move(grass.PaintedMask));
   }
   std::vector<std::vector<uint8_t>> oldInstancerMasks;
   oldInstancerMasks.reserve(InstancerLayers.size());
   for (auto& layer : InstancerLayers)
   {
      oldInstancerMasks.push_back(std::move(layer.PaintedMask));
   }
   
   // Update resolution and allocate new buffers
   GridResolution = clamped;
   HeightMap.resize(newCount);
   SplatMap.resize(newCount);
   if (!oldSplatMap2.empty())
      SplatMap2.resize(newCount);
   HoleMask.resize(newCount);
   
   // Resample heightmap and splatmap
   for (uint32_t z = 0; z < clamped; ++z)
   {
      const float v = static_cast<float>(z) / static_cast<float>(clamped > 1 ? clamped - 1 : 1);
      for (uint32_t x = 0; x < clamped; ++x)
      {
         const float u = static_cast<float>(x) / static_cast<float>(clamped > 1 ? clamped - 1 : 1);
         const size_t idx = static_cast<size_t>(z) * clamped + x;
         HeightMap[idx] = sampleBilinear16(oldHeightMap, u, v);
         SplatMap[idx] = sampleBilinearU8Vec4(oldSplatMap, u, v);
         if (!oldSplatMap2.empty())
            SplatMap2[idx] = sampleBilinearU8Vec4(oldSplatMap2, u, v);
         HoleMask[idx] = sampleBilinearU8(oldHoleMask, u, v);
      }
   }
   
   // Resample grass masks
   for (size_t i = 0; i < GrassLayers.size(); ++i)
   {
      GrassLayers[i].PaintedMask.resize(newCount);
      if (i < oldGrassMasks.size() && oldGrassMasks[i].size() == oldCount)
      {
         for (uint32_t z = 0; z < clamped; ++z)
         {
            const float v = static_cast<float>(z) / static_cast<float>(clamped > 1 ? clamped - 1 : 1);
            for (uint32_t x = 0; x < clamped; ++x)
            {
               const float u = static_cast<float>(x) / static_cast<float>(clamped > 1 ? clamped - 1 : 1);
               const size_t idx = static_cast<size_t>(z) * clamped + x;
               GrassLayers[i].PaintedMask[idx] = sampleBilinearU8(oldGrassMasks[i], u, v);
            }
         }
      }
      else
      {
         GrassLayers[i].PaintedMask.assign(newCount, 0);
      }
      GrassLayers[i].RuntimeDirty = true;
      GrassLayers[i].DensityDirty = true;
      GrassLayers[i].DensityDirtyMin = glm::ivec2(0);
      GrassLayers[i].DensityDirtyMax = glm::ivec2(static_cast<int>(clamped) - 1);
   }

   // Resample terrain instancer masks
   for (size_t i = 0; i < InstancerLayers.size(); ++i)
   {
      InstancerLayers[i].PaintedMask.resize(newCount);
      if (i < oldInstancerMasks.size() && oldInstancerMasks[i].size() == oldCount)
      {
         for (uint32_t z = 0; z < clamped; ++z)
         {
            const float v = static_cast<float>(z) / static_cast<float>(clamped > 1 ? clamped - 1 : 1);
            for (uint32_t x = 0; x < clamped; ++x)
            {
               const float u = static_cast<float>(x) / static_cast<float>(clamped > 1 ? clamped - 1 : 1);
               const size_t idx = static_cast<size_t>(z) * clamped + x;
               InstancerLayers[i].PaintedMask[idx] = sampleBilinearU8(oldInstancerMasks[i], u, v);
            }
         }
      }
      else
      {
         InstancerLayers[i].PaintedMask.assign(newCount, 0);
      }
      InstancerLayers[i].MarkRuntimeDirty();
   }
   
   DestroyGpuResources();
   MeshDirty = true;
   HeightDataDirty = true;
   SplatDataDirty = true;
   HoleDataDirty = true;
   PhysicsDirty = true;
   AssetDirty = true;
   GrassStructureDirty = true;
   GrassMasksDirty = true;
   InstancerLayersDirty = true;
   LODBuffersDirty = true;
}

inline void TerrainComponent::EnsureMapSize()
{
   const size_t count = static_cast<size_t>(GridResolution) * static_cast<size_t>(GridResolution);
   if (HeightMap.size() != count)
      HeightMap.assign(count, 0);
   if (SplatMap.size() != count)
      SplatMap.assign(count, glm::u8vec4(255, 0, 0, 0));
   // SplatMap2 is only allocated when using more than 4 layers
   if (!SplatMap2.empty() && SplatMap2.size() != count)
      SplatMap2.assign(count, glm::u8vec4(0, 0, 0, 0));
   if (HoleMask.size() != count)
      HoleMask.assign(count, 0);
   for (auto& grass : GrassLayers)
   {
      grass.EnsureMaskSize(GridResolution);
   }
   for (auto& layer : InstancerLayers)
   {
      layer.EnsureMaskSize(GridResolution);
   }
}

// Ensures SplatMap2 is allocated (call when adding layer 5+)
inline void TerrainComponent::EnsureSplatMap2()
{
   const size_t count = static_cast<size_t>(GridResolution) * static_cast<size_t>(GridResolution);
   if (SplatMap2.size() != count)
      SplatMap2.assign(count, glm::u8vec4(0, 0, 0, 0));
}

inline void TerrainComponent::DestroyGpuResources()
{
   if (bgfx::isValid(ChunkVB))
   {
      bgfx::destroy(ChunkVB);
      ChunkVB = BGFX_INVALID_HANDLE;
   }
   if (bgfx::isValid(ChunkIB))
   {
      bgfx::destroy(ChunkIB);
      ChunkIB = BGFX_INVALID_HANDLE;
   }
   // Destroy LOD index buffers
   for (uint32_t i = 0; i < TerrainLODConfig::kMaxLODLevels; ++i)
   {
      if (bgfx::isValid(LODIndexBuffers[i]))
      {
         bgfx::destroy(LODIndexBuffers[i]);
         LODIndexBuffers[i] = BGFX_INVALID_HANDLE;
      }
      LODIndexCounts[i] = 0;
   }
   LODLevelCount = 0;
   LODBuffersDirty = true;
   
   // Destroy chunked terrain buffers
   if (bgfx::isValid(SharedChunkVB))
   {
      bgfx::destroy(SharedChunkVB);
      SharedChunkVB = BGFX_INVALID_HANDLE;
   }
   for (uint32_t i = 0; i < TerrainLODConfig::kMaxLODLevels; ++i)
   {
      if (bgfx::isValid(ChunkLODIndexBuffers[i]))
      {
         bgfx::destroy(ChunkLODIndexBuffers[i]);
         ChunkLODIndexBuffers[i] = BGFX_INVALID_HANDLE;
      }
      ChunkLODIndexCounts[i] = 0;
   }
   ChunkMeshDirty = true;
   
   for (TerrainChunk& chunk : Chunks)
   {
      if (bgfx::isValid(chunk.HeightTexture))
      {
         bgfx::destroy(chunk.HeightTexture);
         chunk.HeightTexture = BGFX_INVALID_HANDLE;
      }
      if (bgfx::isValid(chunk.SplatTexture))
      {
         bgfx::destroy(chunk.SplatTexture);
         chunk.SplatTexture = BGFX_INVALID_HANDLE;
      }
      if (bgfx::isValid(chunk.SplatTexture2))
      {
         bgfx::destroy(chunk.SplatTexture2);
         chunk.SplatTexture2 = BGFX_INVALID_HANDLE;
      }
      if (bgfx::isValid(chunk.HoleTexture))
      {
         bgfx::destroy(chunk.HoleTexture);
         chunk.HoleTexture = BGFX_INVALID_HANDLE;
      }
   }
   Chunks.clear();
   
   // Destroy grass deformation texture
   if (bgfx::isValid(DeformationTexture))
   {
      bgfx::destroy(DeformationTexture);
      DeformationTexture = BGFX_INVALID_HANDLE;
   }
   DeformationData.clear();
   DeformationDirty = false;

   for (auto& layer : InstancerLayers)
   {
      layer.ReleaseRuntime();
   }
}


inline void TerrainComponent::MarkDataDirty()
{
   MeshDirty = true;
   HeightDataDirty = true;
   SplatDataDirty = true;
   HoleDataDirty = true;
   PhysicsDirty = true; // Mark physics as dirty when height data changes
   AssetDirty = true;
   GrassStructureDirty = true;
   GrassMasksDirty = true;
   MarkInstancersDirty();
   LODBuffersDirty = true;
   for (TerrainChunk& chunk : Chunks)
   {
      chunk.HeightDirty = true;
      chunk.SplatDirty = true;
      chunk.HoleDirty = true;
      chunk.DirtyMin = chunk.Start;
      chunk.DirtyMax = chunk.Start + glm::ivec2(
         static_cast<int>(std::max(1u, chunk.VertexCountX)) - 1,
         static_cast<int>(std::max(1u, chunk.VertexCountZ)) - 1);
   }
}

inline void TerrainComponent::MarkInstancersDirty()
{
   if (DeferredInstancerDirtyDepth > 0)
   {
      DeferredInstancerFullDirty = true;
      DeferredInstancerLayerDirty.clear();
      DeferredInstancerRegions.clear();
      return;
   }

   InstancerLayersDirty = true;
   for (auto& layer : InstancerLayers)
   {
      layer.MarkRuntimeDirty();
   }
}

inline void TerrainComponent::MarkInstancerLayerDirty(size_t layerIndex)
{
   if (layerIndex >= InstancerLayers.size())
      return;

   if (DeferredInstancerDirtyDepth > 0)
   {
      if (!DeferredInstancerFullDirty &&
          std::find(DeferredInstancerLayerDirty.begin(), DeferredInstancerLayerDirty.end(), layerIndex) == DeferredInstancerLayerDirty.end())
      {
         DeferredInstancerLayerDirty.push_back(layerIndex);
      }
      return;
   }

   InstancerLayers[layerIndex].MarkRuntimeDirty();
}

inline void TerrainComponent::MarkInstancerLayerRegionDirty(size_t layerIndex, const glm::ivec2& minCell, const glm::ivec2& maxCell)
{
   if (layerIndex >= InstancerLayers.size())
      return;

   const int maxIndex = static_cast<int>(GridResolution) - 1;
   const glm::ivec2 clampMin(0);
   const glm::ivec2 clampMax(maxIndex);
   const glm::ivec2 minClamped = glm::clamp(minCell, clampMin, clampMax);
   const glm::ivec2 maxClamped = glm::clamp(maxCell, clampMin, clampMax);

   if (DeferredInstancerDirtyDepth > 0)
   {
      if (DeferredInstancerFullDirty)
         return;

      auto it = std::find_if(DeferredInstancerRegions.begin(), DeferredInstancerRegions.end(),
         [layerIndex](const DeferredInstancerRegion& region) { return region.LayerIndex == layerIndex; });
      if (it == DeferredInstancerRegions.end())
      {
         DeferredInstancerRegions.push_back({ layerIndex, minClamped, maxClamped });
      }
      else
      {
         it->Min = glm::min(it->Min, minClamped);
         it->Max = glm::max(it->Max, maxClamped);
      }
      return;
   }

   InstancerLayers[layerIndex].MarkRuntimeRegionDirty(minClamped, maxClamped);
}

inline void TerrainComponent::BeginDeferredInstancerDirty()
{
   ++DeferredInstancerDirtyDepth;
}

inline void TerrainComponent::EndDeferredInstancerDirty()
{
   if (DeferredInstancerDirtyDepth == 0)
      return;

   --DeferredInstancerDirtyDepth;
   if (DeferredInstancerDirtyDepth > 0)
      return;

   if (DeferredInstancerFullDirty)
   {
      DeferredInstancerFullDirty = false;
      DeferredInstancerLayerDirty.clear();
      DeferredInstancerRegions.clear();
      MarkInstancersDirty();
      return;
   }

   for (size_t layerIndex : DeferredInstancerLayerDirty)
   {
      if (layerIndex < InstancerLayers.size())
      {
         InstancerLayers[layerIndex].MarkRuntimeDirty();
      }
   }
   DeferredInstancerLayerDirty.clear();

   for (const DeferredInstancerRegion& region : DeferredInstancerRegions)
   {
      if (region.LayerIndex < InstancerLayers.size())
      {
         InstancerLayers[region.LayerIndex].MarkRuntimeRegionDirty(region.Min, region.Max);
      }
   }
   DeferredInstancerRegions.clear();
}

inline std::string TerrainComponent::BuildDefaultAssetPath(const ClaymoreGUID& guid)
{
   return ".bin/terrain/terrain_" + guid.ToString() + ".terrainbin";
}

inline void TerrainComponent::ResetAssetIdentity()
{
   TerrainDataGuid = ClaymoreGUID::Generate();
   AssetPath = BuildDefaultAssetPath(TerrainDataGuid);
   AssetDirty = true;
}



//------------------------------------------------------------------------------
// Particle Emitter Component - Modern particle system with Unity/Unreal parity
//------------------------------------------------------------------------------

// Emission shape for particles
enum class ParticleEmissionShape : int
{
    Point = 0,      // Single point emission
    Sphere,         // Emit from sphere surface/volume
    Hemisphere,     // Emit from hemisphere
    Cone,           // Emit in a cone direction
    Box,            // Emit from box volume
    Circle,         // Emit from circle edge
    Disc,           // Emit from filled disc
    Edge,           // Emit from a line segment
    Rectangle,      // Emit from rectangle perimeter
    Count
};

// Simulation space for particles
enum class ParticleSimulationSpace : int
{
    Local = 0,      // Particles follow emitter
    World           // Particles stay in world space
};

// Blend mode for particles
enum class ParticleBlendMode : int
{
    Alpha = 0,
    Additive,
    Multiply,
    Count
};

// Curve type for over-lifetime effects
enum class ParticleCurveType : int
{
    Constant = 0,   // Single constant value
    Linear,         // Linear interpolation start->end
    EaseIn,         // Ease in curve
    EaseOut,        // Ease out curve
    EaseInOut,      // Smooth ease in/out
    Custom          // Reserved for future curve editor
};

// Range helper for min/max randomization
struct ParticleRange
{
    float Min = 0.0f;
    float Max = 1.0f;
    
    float GetRandom() const { 
        if (Min >= Max) return Min;
        return Min + (Max - Min) * ((float)rand() / RAND_MAX); 
    }
    float GetMidpoint() const { return (Min + Max) * 0.5f; }
};

// Color gradient key for color over lifetime
struct ParticleColorKey
{
    float Time = 0.0f;          // 0..1 normalized lifetime
    glm::vec4 Color = glm::vec4(1.0f);
};

// Size/value curve over lifetime
struct ParticleValueOverLifetime
{
    ParticleCurveType CurveType = ParticleCurveType::Constant;
    float StartValue = 1.0f;
    float EndValue = 1.0f;
    
    float Evaluate(float t) const {
        switch (CurveType) {
            case ParticleCurveType::Constant: return StartValue;
            case ParticleCurveType::Linear: return StartValue + (EndValue - StartValue) * t;
            case ParticleCurveType::EaseIn: return StartValue + (EndValue - StartValue) * (t * t);
            case ParticleCurveType::EaseOut: { float it = 1.0f - t; return StartValue + (EndValue - StartValue) * (1.0f - it * it); }
            case ParticleCurveType::EaseInOut: { float s = t * t * (3.0f - 2.0f * t); return StartValue + (EndValue - StartValue) * s; }
            default: return StartValue;
        }
    }
};

struct ParticleEmitterComponent
{
    // ===== Core Handles =====
    ps::EmitterHandle Handle{ uint16_t{UINT16_MAX} };
    ps::EmitterUniforms Uniforms;
    ps::EmitterSpriteHandle SpriteHandle{ uint16_t{UINT16_MAX} };
    std::string SpritePath;
    
    // ===== Basic Settings =====
    bool Enabled = true;
    uint32_t MaxParticles = 1024;
    
    // ===== Duration & Looping =====
    float Duration = 5.0f;              // Duration of emission in seconds
    bool Looping = true;                // Whether to loop emission
    bool Prewarm = false;               // Simulate particles before first frame
    bool PlayOnAwake = true;            // Start emitting immediately
    
    // ===== Emission =====
    float EmissionRate = 100.0f;        // Particles per second
    bool RateOverDistance = false;      // Emit based on movement (future)
    
    // Burst emission (one-shot bursts)
    bool BurstEnabled = false;
    int BurstCount = 10;                // Number of particles in burst
    float BurstTime = 0.0f;             // Time offset for burst
    int BurstCycles = 1;                // Number of burst cycles (0 = infinite in looping)
    float BurstInterval = 1.0f;         // Time between burst cycles
    
    // ===== Shape =====
    ParticleEmissionShape Shape = ParticleEmissionShape::Cone;
    float ShapeRadius = 1.0f;           // Radius for sphere/cone/circle/disc
    float ShapeRadiusThickness = 1.0f;  // 0=surface, 1=volume for sphere/cone
    float ShapeAngle = 25.0f;           // Cone angle in degrees
    float ShapeArc = 360.0f;            // Arc angle for partial shapes
    glm::vec3 ShapeScale = glm::vec3(1.0f); // Scale for box/rectangle shapes
    float ShapeLength = 2.0f;           // Length for edge shape
    bool ShapeEmitFromEdge = false;     // Emit from edge vs volume
    bool ShapeRandomizeDirection = false;// Randomize initial direction
    
    // ===== Lifetime =====
    ParticleRange Lifetime = { 3.0f, 5.0f };
    
    // ===== Start Values (randomized per particle) =====
    ParticleRange StartSpeed = { 2.0f, 5.0f };
    ParticleRange StartSize = { 0.1f, 0.3f };
    ParticleRange StartRotation = { 0.0f, 360.0f };   // Degrees
    glm::vec4 StartColor = glm::vec4(1.0f);
    bool StartColorRandom = false;
    glm::vec4 StartColorMin = glm::vec4(1.0f);
    glm::vec4 StartColorMax = glm::vec4(1.0f);
    
    // ===== Physics =====
    float GravityModifier = 0.0f;       // Multiplier for gravity effect
    ParticleSimulationSpace SimulationSpace = ParticleSimulationSpace::World;
    float InheritVelocity = 0.0f;       // How much emitter velocity affects particles
    float DragCoefficient = 0.0f;       // Air resistance
    
    // ===== Velocity Over Lifetime =====
    bool VelocityOverLifetimeEnabled = false;
    glm::vec3 LinearVelocity = glm::vec3(0.0f);
    float OrbitalVelocity = 0.0f;       // Rotate around emission point
    float RadialVelocity = 0.0f;        // Push outward/inward
    
    // ===== Size Over Lifetime =====
    bool SizeOverLifetimeEnabled = true;
    ParticleValueOverLifetime SizeOverLifetime = { ParticleCurveType::Linear, 1.0f, 0.0f };
    
    // ===== Color Over Lifetime =====
    bool ColorOverLifetimeEnabled = true;
    std::vector<ParticleColorKey> ColorGradient = {
        { 0.0f, glm::vec4(1.0f, 1.0f, 1.0f, 0.0f) },
        { 0.1f, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f) },
        { 0.9f, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f) },
        { 1.0f, glm::vec4(1.0f, 1.0f, 1.0f, 0.0f) }
    };
    
    // ===== Rotation Over Lifetime =====
    bool RotationOverLifetimeEnabled = false;
    float AngularVelocity = 0.0f;       // Degrees per second
    bool AlignWithTrajectory = false;   // Rotate particles to follow their velocity direction
    
    // ===== Rendering =====
    ParticleBlendMode BlendMode = ParticleBlendMode::Additive;
    int RenderOrder = 0;                // Sorting priority
    bool FaceCamera = true;             // Billboard mode
    
    // ===== Texture Sheet Animation (future) =====
    bool TextureSheetEnabled = false;
    int TextureSheetTilesX = 1;
    int TextureSheetTilesY = 1;
    float TextureSheetFrameRate = 30.0f;
    bool TextureSheetRandomStart = false;
    
    // ===== Runtime State =====
    float ElapsedTime = 0.0f;           // Time since start
    bool IsPlaying = false;             // Currently emitting
    bool HasEmitted = false;            // Has ever emitted (for one-shot)
    int BurstCyclesRemaining = 0;       // Remaining burst cycles
    float NextBurstTime = 0.0f;         // Time until next burst
    bool JustCreated = false;            // True on first frame after emitter creation - prevents premature emission
    bool HasPrewarmed = false;           // Runtime guard so prewarm runs once per play cycle
    glm::vec3 PreviousWorldPosition = glm::vec3(0.0f); // Last frame's world position for inherit-velocity calculations
    bool HasPreviousWorldPosition = false;
    
    // ===== Cached State (for detecting changes) =====
    ParticleEmissionShape CachedShape = ParticleEmissionShape::Cone;  // Track shape changes
    uint32_t CachedMaxParticles = 1024;                                // Track max particles changes
    bool CachedShapeEmitFromEdge = false;                              // Rebuild direction mode when edge emission changes
    
    // ===== Lifecycle Flags =====
    bool DestroyOnComplete = false;     // Destroy entity when finished (non-looping)
    bool StopEmittingOnComplete = true; // Stop emission when duration ends
    
    ParticleEmitterComponent()
    {
        Uniforms.reset();
        // Initialize default gradient
        ColorGradient.clear();
        ColorGradient.push_back({ 0.0f, glm::vec4(1.0f, 1.0f, 1.0f, 0.0f) });
        ColorGradient.push_back({ 0.1f, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f) });
        ColorGradient.push_back({ 0.9f, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f) });
        ColorGradient.push_back({ 1.0f, glm::vec4(1.0f, 1.0f, 1.0f, 0.0f) });
    }
    
    // Control methods
    void Play() { IsPlaying = true; ElapsedTime = 0.0f; HasEmitted = false; HasPrewarmed = false; BurstCyclesRemaining = BurstCycles; NextBurstTime = BurstTime; }
    void Stop() { IsPlaying = false; }
    void Restart() { Stop(); Play(); }
    bool IsComplete() const { return !Looping && ElapsedTime >= Duration && !IsPlaying; }
};


//------------------------------------------------------------------------------
// Text Renderer Component
//------------------------------------------------------------------------------
struct TextRendererComponent
   {
   enum class Alignment { Left = 0, Center = 1, Right = 2 };

   // UTF-8 text to render
   std::string Text = "Hello World";

   // Approximate pixel height used when creating the font
   float PixelSize = 32.0f;

   // ABGR color packed as 0xAABBGGRR (bgfx convention)
   uint32_t ColorAbgr = 0xffffffffu;

   // If true, text is rendered in world space 
   // using the entity transform.
   // If false, text is rendered in screen space
   // (top-left origin) at the entity's Position.xy
   bool WorldSpace = true;

   // When true, standalone world-space text faces the active camera.
   // Canvas-backed world-space UI uses CanvasComponent::Billboard instead.
   bool Billboard = true;

   // If true, text will be anchored relative to its parent UI element's
   // rect (Panel or UIRect) when one exists. This is a first-class way
   // to make text follow parent UI layout without requiring a separate
   // UIRect configuration.
   bool AnchorToParentUI = false;

   // UI anchoring when used under a Canvas in screen space
   bool AnchorEnabled = false;
   
   // Anchor preset
   UIAnchorPreset Anchor = UIAnchorPreset::TopLeft;
   
   // Additional pixel offset from anchor position
   glm::vec2 AnchorOffset = { 0.0f, 0.0f };

   // Visibility toggle
   bool Visible = true;
   
   // Sorting within a canvas (lower renders first)
   int ZOrder = 0;
   
   // Additional opacity multiplier (0..1), 
   // applied on top of ColorAbgr alpha
   float Opacity = 1.0f;

   // Optional wrapping rectangle in screen pixels. 
   // When x or y <= 0, wrapping is disabled.
   glm::vec2 RectSize = { 0.0f, 0.0f };
   
   // If true and RectSize.x > 0, 
   // text will wrap to fit within RectSize.x
   bool WordWrap = false;

   // Optional font path (TTF) from asset registry; 
   // when empty, use default baked font
   std::string FontPath;

   // Horizontal alignment. For unbounded text, Center/Right make the
   // authored position act as the center/right growth point.
   Alignment TextAlignment = Alignment::Left;

   // Optional text outline pass (screen/world space)
   bool OutlineEnabled = false;
   // ABGR color packed as 0xAABBGGRR
   uint32_t OutlineColorAbgr = 0xff000000u;
   // Outline radius in pixels
   float OutlineThickness = 1.0f;

   // Optional text drop shadow pass (screen/world space)
   bool ShadowEnabled = false;
   // ABGR color packed as 0xAABBGGRR
   uint32_t ShadowColorAbgr = 0x80000000u;
   // Shadow offset in pixels (x,y)
   glm::vec2 ShadowOffset = { 2.0f, 2.0f };
   };


//------------------------------------------------------------------------------
// Portal Component (scene traversal between portals)
//------------------------------------------------------------------------------
struct PortalComponent
{
   // Enabled toggle for traversal/event dispatch
   bool Enabled = true;

   // Target scene path (e.g., "assets/scenes/Tavern.scene")
   std::string TargetScenePath;

   // Target portal entity GUID (in the target scene)
   ClaymoreGUID TargetPortalGuid{};

   // Optional path hint within the target scene (e.g., "Root/PortalA")
   std::string TargetPortalPath;

   // Entry/exit offsets in local space of the portal entity
   glm::vec3 EntryOffset{ 0.0f };
   glm::vec3 ExitOffset{ 0.0f };

   // Auto-detection for portal crossing (play mode only)
   bool AutoDetect = true;
   float TriggerRadius = 1.0f;
   bool FireExitEvents = true;

   // Runtime state (not serialized)
   std::unordered_set<EntityID> Overlapping;

   void ResetRuntime()
   {
      Overlapping.clear();
   }
};
