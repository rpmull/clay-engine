#include "Scene.h"
#include "core/managed/ScriptInterop.h"
#include <tuple>
#include "managed/interop/DotNetHost.h"
#include "EntityData.h"
#include <algorithm>
#include <functional>
#include <filesystem>
#include <cctype>
#include <cmath>
#include <iostream>
#include <limits>
#include "core/rendering/MaterialCache.h"
#include "core/rendering/MaterialAssetCache.h"
#include "core/rendering/StandardMeshManager.h"
#include "core/rendering/MaterialAsset.h"
#include "core/rendering/PBRMaterial.h"
#include "core/ecs/AnimationComponents.h"
#include "core/ecs/NpcScalability.h"
#include "core/ecs/ParticleEmitterSystem.h"
#include "core/particles/SpriteLoader.h"
#include "core/ecs/SkinningSystem.h"
#include "core/ecs/SoftbodySystem.h"
#include "core/ecs/InstancerSystem.h"
#include "core/ecs/ComponentUtils.h"
#include "core/world/PortalSystem.h"
#include "core/world/RuntimeWorld.h"
#include "core/utils/DebugModelDump.h"
#include "core/rendering/TextureLoader.h"
#include "core/rendering/MaterialManager.h"
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include "core/managed/ManagedScriptComponent.h"
#include "core/managed/ScriptReflection.h"
#include "core/managed/ScriptSystem.h"
#include "core/managed/ScriptOrder.h"
#include "core/managed/DeferredScriptInit.h"
#include "core/animation/AvatarDefinition.h"
#include "core/animation/AnimationSystem.h"
#include "core/animation/AnimationAsset.h"
#include "core/animation/AnimationAssetCache.h"
#include "core/animation/SkeletonBinding.h"
#include "core/animation/AnimationSerializer.h"
#include "core/animation/AnimationPlayerComponent.h"
#include "core/dialogue/DialogueManager.h"
#include "core/assets/IAssetResolver.h"
#include "core/prefab/PrefabPrewarm.h"
#include "core/prefab/RuntimePrefabInstantiator.h"
#include "core/physics/area/AreaSystem.h"
#ifndef CLAYMORE_RUNTIME
#include "editor/pipeline/AssetLibrary.h"
#endif



namespace {
   constexpr float kMinLoadDisplaySeconds = 0.25f;

   std::string ResolveAudioSourcePath(const AudioSourceComponent& source) {
      if (source.AudioClip.IsValid()) {
         if (IAssetResolver* resolver = Assets::GetResolver()) {
            std::string path = resolver->GetPathForGUID(source.AudioClip.guid);
            if (!path.empty()) {
               return path;
            }
         }
#ifndef CLAYMORE_RUNTIME
         std::string editorPath = AssetLibrary::Instance().GetAudioPath(source.AudioClip.guid);
         if (!editorPath.empty()) {
            return editorPath;
         }
#endif
      }

      return source.AudioPath;
   }

   bool HasPresentationHiddenAncestor(Scene& scene, EntityID id) {
      EntityID cursor = id;
      while (cursor != INVALID_ENTITY_ID) {
         EntityData* data = scene.GetEntityData(cursor);
         if (!data) {
            break;
         }
         if (data->PresentationHidden) {
            return true;
         }
         cursor = data->Parent;
      }
      return false;
   }

   EntityID ResolvePhysicsBodyEntity(const JPH::Body& body) {
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

   bool CollisionMaskIncludesLayer(uint32_t mask, JPH::ObjectLayer layer) {
      if (layer >= MAX_PHYSICS_LAYERS) {
         return true;
      }
      return (mask & (1u << static_cast<uint32_t>(layer))) != 0;
   }

   bool PhysicsBodyAllowsCharacterLayer(const JPH::Body& body, JPH::ObjectLayer characterLayer) {
      EntityID entityId = ResolvePhysicsBodyEntity(body);
      if (entityId == 0) {
         return true;
      }

      EntityData* data = Scene::Get().GetEntityData(entityId);
      if (!data || !data->RigidBody) {
         return true;
      }

      return CollisionMaskIncludesLayer(data->RigidBody->CollisionMask, characterLayer);
   }

   class CharacterControllerBodyFilter final : public JPH::BodyFilter {
   public:
      CharacterControllerBodyFilter(JPH::PhysicsSystem* system, JPH::ObjectLayer characterLayer,
                                    uint32_t characterCollisionMask = 0xFFFFFFFFu)
         : m_System(system)
         , m_CharacterLayer(characterLayer)
         , m_CharacterCollisionMask(characterCollisionMask) {}

      bool ShouldCollide(const JPH::BodyID& inBodyID) const override {
         if (!m_System) {
            return true;
         }

         JPH::BodyLockRead lock(m_System->GetBodyLockInterface(), inBodyID);
         return !lock.Succeeded() || ShouldCollideLocked(lock.GetBody());
      }

      bool ShouldCollideLocked(const JPH::Body& inBody) const override {
         // The character only collides with bodies whose object layer is enabled in its
         // own collision mask (symmetric with how a rigidbody's mask gates the character).
         if (!CollisionMaskIncludesLayer(m_CharacterCollisionMask, inBody.GetObjectLayer())) {
            return false;
         }
         return PhysicsBodyAllowsCharacterLayer(inBody, m_CharacterLayer);
      }

   private:
      JPH::PhysicsSystem* m_System = nullptr;
      JPH::ObjectLayer m_CharacterLayer = 0;
      uint32_t m_CharacterCollisionMask = 0xFFFFFFFFu;
   };

   void ApplyPendingRigidBodyCommands(RigidBodyComponent& rigidBody) {
      if (rigidBody.IsKinematic || rigidBody.BodyID.IsInvalid()) {
         return;
      }

      const float pendingForceLenSq = glm::dot(rigidBody.PendingForce, rigidBody.PendingForce);
      const float pendingTorqueLenSq = glm::dot(rigidBody.PendingTorque, rigidBody.PendingTorque);
      const float pendingImpulseLenSq = glm::dot(rigidBody.PendingImpulse, rigidBody.PendingImpulse);
      const float pendingAngularImpulseLenSq = glm::dot(rigidBody.PendingAngularImpulse, rigidBody.PendingAngularImpulse);
      if (pendingForceLenSq <= 1e-10f &&
          pendingTorqueLenSq <= 1e-10f &&
          pendingImpulseLenSq <= 1e-10f &&
          pendingAngularImpulseLenSq <= 1e-10f) {
         return;
      }

      JPH::PhysicsSystem* system = Physics::GetSystem();
      if (!system) {
         return;
      }

      {
         JPH::BodyLockRead lock(system->GetBodyLockInterface(), rigidBody.BodyID);
         if (!lock.Succeeded() || lock.GetBody().GetMotionType() != JPH::EMotionType::Dynamic) {
            return;
         }
      }

      JPH::BodyInterface& bodyInterface = Physics::Get().GetBodyInterface();
      bodyInterface.ActivateBody(rigidBody.BodyID);

      if (pendingImpulseLenSq > 1e-10f) {
         const glm::vec3 impulse = rigidBody.PendingImpulse;
         bodyInterface.AddImpulse(rigidBody.BodyID, JPH::Vec3(impulse.x, impulse.y, impulse.z));
         const JPH::Vec3 velocity = bodyInterface.GetLinearVelocity(rigidBody.BodyID);
         rigidBody.LinearVelocity = glm::vec3(velocity.GetX(), velocity.GetY(), velocity.GetZ());
         rigidBody.PendingImpulse = glm::vec3(0.0f);
         ++rigidBody._DebugAppliedPhysicsCommandCount;
      }
      if (pendingAngularImpulseLenSq > 1e-10f) {
         const glm::vec3 impulse = rigidBody.PendingAngularImpulse;
         bodyInterface.AddAngularImpulse(rigidBody.BodyID, JPH::Vec3(impulse.x, impulse.y, impulse.z));
         const JPH::Vec3 velocity = bodyInterface.GetAngularVelocity(rigidBody.BodyID);
         rigidBody.AngularVelocity = glm::vec3(velocity.GetX(), velocity.GetY(), velocity.GetZ());
         rigidBody.PendingAngularImpulse = glm::vec3(0.0f);
         ++rigidBody._DebugAppliedPhysicsCommandCount;
      }
      if (pendingForceLenSq > 1e-10f) {
         const glm::vec3 force = rigidBody.PendingForce;
         bodyInterface.AddForce(rigidBody.BodyID, JPH::Vec3(force.x, force.y, force.z));
         rigidBody.PendingForce = glm::vec3(0.0f);
         ++rigidBody._DebugAppliedPhysicsCommandCount;
      }
      if (pendingTorqueLenSq > 1e-10f) {
         const glm::vec3 torque = rigidBody.PendingTorque;
         bodyInterface.AddTorque(rigidBody.BodyID, JPH::Vec3(torque.x, torque.y, torque.z));
         rigidBody.PendingTorque = glm::vec3(0.0f);
         ++rigidBody._DebugAppliedPhysicsCommandCount;
      }
   }
}
#if !defined(CLAYMORE_RUNTIME)
#include "core/serialization/Serializer.h"
#else
#include "core/serialization/EntityBinaryLoader.h"
#endif
#include <nlohmann/json.hpp>
#include "core/ecs/ComponentRegistry.h"


#include <sstream>
#include <glm/gtc/quaternion.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/euler_angles.hpp>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#include "core/animation/AvatarSerializer.h"
#include "core/serialization/Serializer.h"
#include "core/rendering/VertexTypes.h"
#include <bgfx/bgfx.h>
#include "core/animation/Tween.h"
#include "core/deformation/ArmorFitComponent.h"
#include "core/rendering/Renderer.h"
#include "core/rendering/Terrain.h"

#include "core/jobs/JobSystem.h"
#include "core/jobs/ParallelFor.h"
#include "core/jobs/Jobs.h"
#include "core/utils/PrefabPerfDiagnostics.h"
#include "core/utils/Profiler.h"
#include "core/prefab/PrefabAPI.h"
#include "core/animation/ik/IKSystem.h"
#include "core/animation/lookat/LookAtConstraintSystem.h"
#include "core/audio/Audio.h"
#include "core/physics/area/AreaSystem.h"
#include "core/physics/collision/CollisionSystem.h"
#include "core/physics/ragdoll/RagdollSystem.h"
#include "core/multiplayer/MultiplayerBridge.h"


// Editor-only includes (not available in runtime builds)
#ifndef CLAYMORE_RUNTIME
#include "editor/pipeline/ModelImportCache.h"
#include "editor/pipeline/ModelNodeIdentity.h"
#include "editor/pipeline/ModelImportSettings.h"
#include "editor/pipeline/MeshBin.h"
#include "editor/pipeline/SkelBin.h"
#include "editor/pipeline/MaterialSourceSerialization.h"
#include "editor/import/ModelPreprocessor.h"
#include "editor/ui/Logger.h"
#include "editor/Project.h"
#endif



#include <iostream>
#include <cstdlib>

#include "core/vfs/FileSystem.h"
#include "core/vfs/VirtualFS.h"
#include "core/serialization/EntityBinaryLoader.h"
#include <chrono>
#include <atomic>
#include <mutex>

namespace fs = std::filesystem;
using json = nlohmann::json;

struct Scene::AsyncLoadJob {
   std::string sourcePath;
   std::string binaryPath;
   std::vector<uint8_t> data;
   std::atomic<bool> readReady{false};
   std::atomic<bool> readFailed{false};
   std::string readError;
   std::mutex dataMutex;
   bool dataReady = false;
   bool failed = false;
   bool completed = false;
   binary::EntityBinaryLoader::StreamingContext stream;
   std::chrono::steady_clock::time_point startTime{};
   std::chrono::steady_clock::time_point streamStartTime{};
   std::chrono::steady_clock::time_point lastProgressTime{};
   float lastProgress = 0.0f;
};


namespace {
std::string Trimmed(std::string value) {
   auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
   value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
   value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
   return value;
}

std::vector<uint8_t> ReadSceneBinary(const std::string& path) {
   std::vector<uint8_t> data;
   if (VFS::Get() && VFS::Get()->ReadFile(path, data)) {
      return data;
   }
   FileSystem::Instance().ReadFile(path, data);
   return data;
}


std::string DeriveModelRootName(const std::string& assetPath, const std::string& fallback = "ImportedModel") {
   if (assetPath.empty())
   {
      return fallback;
   }

   fs::path p(assetPath);
   std::string stem = p.stem().string();
   if (p.extension() == ".meta")
   {
      stem = fs::path(stem).stem().string();
   }

   if (stem.empty())
   {
      stem = fallback;
   }

   for (char& c : stem)
   {
      unsigned char uc = static_cast<unsigned char>(c);
      if (std::iscntrl(uc) || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
      {
         c = '_';
      }
   }

   stem = Trimmed(stem);
   if (stem.empty())
   {
      stem = fallback;
   }
   return stem;
}

// Utility functions for matrix decomposition and world transform evaluation
// Used by SetParent and other functions in both editor and runtime builds
static void ApplyMatrixToTransform(EntityData* data, const glm::mat4& matrix)
{
   if (!data) return;
   glm::vec3 translation, scale, skew;
   glm::vec4 perspective;
   glm::quat rotationQuat;
   glm::decompose(matrix, scale, rotationQuat, translation, skew, perspective);
   data->Transform.Position = translation;
   data->Transform.Scale = scale;
   data->Transform.RotationQ = glm::normalize(rotationQuat);
   data->Transform.UseQuatRotation = true;
   data->Transform.Rotation = glm::degrees(glm::eulerAngles(rotationQuat));
   data->Transform.TransformDirty = true;
}

static bool NearlyEqualVec3(const glm::vec3& a, const glm::vec3& b, float epsilon = 1e-4f)
{
   const glm::vec3 diff = glm::abs(a - b);
   return diff.x <= epsilon && diff.y <= epsilon && diff.z <= epsilon;
}

static bool NearlyEqualQuat(const glm::quat& a, const glm::quat& b, float epsilon = 1e-4f)
{
   const glm::quat na = glm::normalize(a);
   const glm::quat nb = glm::normalize(b);
   return std::abs(std::abs(glm::dot(na, nb)) - 1.0f) <= epsilon;
}

static bool NearlyEqualMat4(const glm::mat4& a, const glm::mat4& b, float epsilon = 1e-4f)
{
   for (int column = 0; column < 4; ++column) {
      for (int row = 0; row < 4; ++row) {
         if (std::abs(a[column][row] - b[column][row]) > epsilon) {
            return false;
         }
      }
   }
   return true;
}

static bool DecomposeWorldTransform(const TransformComponent& transform,
   glm::vec3& worldPosition,
   glm::quat& worldRotation,
   glm::vec3* worldScale = nullptr)
{
   glm::vec3 scale(1.0f), skew(0.0f);
   glm::vec4 perspective(0.0f);
   glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
   if (!glm::decompose(transform.WorldMatrix, scale, rotation, worldPosition, skew, perspective)) {
      worldPosition = glm::vec3(transform.WorldMatrix[3]);
      worldRotation = transform.UseQuatRotation
         ? glm::normalize(transform.RotationQ)
         : glm::quat(glm::radians(transform.Rotation));
      if (worldScale) {
         *worldScale = glm::vec3(1.0f);
      }
      return false;
   }

   worldRotation = glm::normalize(rotation);
   if (worldScale) {
      *worldScale = scale;
   }
   return true;
}

static glm::vec3 TransformColliderOffsetToWorld(const TransformComponent& transform,
   const glm::vec3& localOffset,
   glm::quat* worldRotationOut = nullptr)
{
   glm::vec3 worldPosition(0.0f);
   glm::quat worldRotation(1.0f, 0.0f, 0.0f, 0.0f);
   DecomposeWorldTransform(transform, worldPosition, worldRotation);
   if (worldRotationOut) {
      *worldRotationOut = worldRotation;
   }
   const glm::vec3 worldOffsetPoint =
      glm::vec3(transform.WorldMatrix * glm::vec4(localOffset, 1.0f));
   return worldOffsetPoint - worldPosition;
}

static glm::vec3 RotateRootMotionDeltaToWorldYaw(const glm::vec3& localDelta,
   const glm::quat& targetWorldRotation)
{
   glm::vec3 forward = targetWorldRotation * glm::vec3(0.0f, 0.0f, 1.0f);
   forward.y = 0.0f;
   if (!glm::all(glm::isfinite(forward)) || glm::dot(forward, forward) < 1e-8f) {
      forward = glm::vec3(0.0f, 0.0f, 1.0f);
   } else {
      forward = glm::normalize(forward);
   }

   glm::vec3 right = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), forward);
   if (!glm::all(glm::isfinite(right)) || glm::dot(right, right) < 1e-8f) {
      right = glm::vec3(1.0f, 0.0f, 0.0f);
   } else {
      right = glm::normalize(right);
   }

   return (right * localDelta.x) + glm::vec3(0.0f, localDelta.y, 0.0f) + (forward * localDelta.z);
}

static glm::vec3 ConvertWorldVectorToParentLocal(Scene& scene,
   const EntityData& data,
   const glm::vec3& worldVector)
{
   if (data.Parent == INVALID_ENTITY_ID) {
      return worldVector;
   }

   const EntityData* parentData = scene.GetEntityData(data.Parent);
   if (!parentData) {
      return worldVector;
   }

   const glm::mat4 parentWorldInv = glm::inverse(parentData->Transform.WorldMatrix);
   return glm::vec3(parentWorldInv * glm::vec4(worldVector, 0.0f));
}

static glm::mat4 RemoveColliderOffsetFromBodyTransform(const glm::mat4& bodyWorld,
   const TransformComponent& entityTransform,
   const glm::vec3& localOffset)
{
   glm::vec3 scale(1.0f), skew(0.0f), bodyPosition(0.0f);
   glm::vec4 perspective(0.0f);
   glm::quat bodyRotation(1.0f, 0.0f, 0.0f, 0.0f);
   if (!glm::decompose(bodyWorld, scale, bodyRotation, bodyPosition, skew, perspective)) {
      glm::mat4 result = bodyWorld;
      result[3] -= glm::vec4(TransformColliderOffsetToWorld(entityTransform, localOffset), 0.0f);
      return result;
   }

   glm::vec3 entityWorldPosition(0.0f);
   glm::quat entityWorldRotation(1.0f, 0.0f, 0.0f, 0.0f);
   glm::vec3 entityWorldScale(1.0f);
   DecomposeWorldTransform(entityTransform, entityWorldPosition, entityWorldRotation, &entityWorldScale);
   (void)entityWorldPosition;
   (void)entityWorldRotation;
   bodyPosition -= glm::normalize(bodyRotation) * (localOffset * entityWorldScale);
   glm::mat4 result = bodyWorld;
   result[3] = glm::vec4(bodyPosition, 1.0f);
   return result;
}

static bool ApplyPhysicsDrivenLocalTransformIfChanged(Scene& scene,
   EntityID id,
   EntityData& data,
   const glm::vec3& translation,
   const glm::quat& rotationQuat)
{
   const glm::quat normalizedRotation = glm::normalize(rotationQuat);
   const bool rotationChanged =
      !NearlyEqualQuat(data.Transform.RotationQ, normalizedRotation) ||
      !data.Transform.UseQuatRotation;
   const bool positionChanged = !NearlyEqualVec3(data.Transform.Position, translation);
   if (!positionChanged && !rotationChanged)
   {
      return false;
   }

   data.Transform.Position = translation;
   data.Transform.RotationQ = normalizedRotation;
   data.Transform.UseQuatRotation = true;
   data.Transform.Rotation = glm::degrees(glm::eulerAngles(normalizedRotation));
   scene.MarkTransformDirty(id);
   return true;
}

static void SyncBoneAttachmentPhysicsBody(EntityData& data)
{
   JPH::BodyID bodyId;
   if (data.RigidBody && !data.RigidBody->BodyID.IsInvalid()) {
      bodyId = data.RigidBody->BodyID;
   } else if (data.StaticBody && !data.StaticBody->BodyID.IsInvalid()) {
      bodyId = data.StaticBody->BodyID;
   } else {
      return;
   }

   glm::vec3 worldPos = glm::vec3(data.Transform.WorldMatrix[3]);
   if (data.Collider) {
      worldPos += TransformColliderOffsetToWorld(data.Transform, data.Collider->Offset);
   }

   glm::vec3 scale(1.0f), skew(0.0f);
   glm::vec4 perspective(0.0f);
   glm::quat worldRot(1.0f, 0.0f, 0.0f, 0.0f);
   glm::vec3 dummyPos(0.0f);
   if (!glm::decompose(data.Transform.WorldMatrix, scale, worldRot, dummyPos, skew, perspective)) {
      return;
   }

   JPH::BodyInterface& bodyInterface = Physics::Get().GetBodyInterface();
   bodyInterface.SetPositionAndRotation(
      bodyId,
      JPH::RVec3(worldPos.x, worldPos.y, worldPos.z),
      JPH::Quat(worldRot.x, worldRot.y, worldRot.z, worldRot.w),
      JPH::EActivation::Activate);

   if (data.RigidBody) {
      data.RigidBody->LinearVelocity = glm::vec3(0.0f);
      data.RigidBody->AngularVelocity = glm::vec3(0.0f);
      data.RigidBody->_RootMotionLinearVelocity = glm::vec3(0.0f);
#if !defined(CLAYMORE_RUNTIME)
      data.RigidBody->_EditorDisplayLinearVelocity = glm::vec3(0.0f);
#endif
      Physics::Get().SetBodyLinearVelocity(bodyId, glm::vec3(0.0f));
      Physics::Get().SetBodyAngularVelocity(bodyId, glm::vec3(0.0f));
   }
}

static bool SyncPhysicsBodyToSceneTransform(EntityData& data,
   JPH::BodyID bodyId,
   JPH::EActivation activation,
   bool resetRigidBodyVelocity)
{
   if (bodyId.IsInvalid() || !Physics::GetSystem()) {
      return false;
   }

   glm::vec3 worldPos(0.0f);
   glm::quat worldRot(1.0f, 0.0f, 0.0f, 0.0f);
   DecomposeWorldTransform(data.Transform, worldPos, worldRot);
   if (data.Collider) {
      worldPos += TransformColliderOffsetToWorld(data.Transform, data.Collider->Offset);
   }
   worldRot = glm::normalize(worldRot);

   const glm::mat4 currentWorld = Physics::Get().GetBodyTransform(bodyId);
   if (currentWorld != glm::mat4(0.0f)) {
      const glm::vec3 currentPos = glm::vec3(currentWorld[3]);
      const glm::quat currentRot = glm::normalize(glm::quat_cast(currentWorld));
      if (NearlyEqualVec3(currentPos, worldPos) && NearlyEqualQuat(currentRot, worldRot)) {
         return false;
      }
   }

   JPH::BodyInterface& bodyInterface = Physics::Get().GetBodyInterface();
   bodyInterface.SetPositionAndRotation(
      bodyId,
      JPH::RVec3(worldPos.x, worldPos.y, worldPos.z),
      JPH::Quat(worldRot.x, worldRot.y, worldRot.z, worldRot.w),
      activation);

   if (resetRigidBodyVelocity && data.RigidBody) {
      data.RigidBody->LinearVelocity = glm::vec3(0.0f);
      data.RigidBody->AngularVelocity = glm::vec3(0.0f);
      data.RigidBody->_RootMotionLinearVelocity = glm::vec3(0.0f);
#if !defined(CLAYMORE_RUNTIME)
      data.RigidBody->_EditorDisplayLinearVelocity = glm::vec3(0.0f);
#endif
      Physics::SetBodyLinearVelocity(bodyId, glm::vec3(0.0f));
      Physics::SetBodyAngularVelocity(bodyId, glm::vec3(0.0f));
   }

   return true;
}

static glm::mat4 EvaluateWorldMatrix(Scene& scene, EntityID id)
{
   glm::mat4 result(1.0f);
   std::vector<EntityID> chain;
   EntityID current = id;
   while (current != INVALID_ENTITY_ID)
   {
      auto* data = scene.GetEntityData(current);
      if (!data)
      {
         chain.clear();
         break;
      }
      chain.push_back(current);
      current = data->Parent;
   }

   for (auto it = chain.rbegin(); it != chain.rend(); ++it)
   {
      auto* data = scene.GetEntityData(*it);
      if (!data)
      {
         continue;
      }
      const glm::mat4 local = data->Transform.CalculateLocalMatrix();
      result *= local;
   }

   return result;
}

} // end anonymous namespace (core utilities)

// ============================================================================
// EDITOR-ONLY MODEL IMPORT TYPES AND FUNCTIONS
// These are only compiled in editor builds, not in runtime/exported games.
// Runtime uses pre-built binary scenes loaded via EntityBinaryLoader.
// ============================================================================
#ifndef CLAYMORE_RUNTIME
namespace { // editor-only anonymous namespace

// Parse entity name suffix keywords like "_a" (alpha) and "_bf" (backfaces).
// Order-independent; scans all '_' tokens.
static inline void ParseNameSuffixHints(const std::string& name, bool& outAlphaBlend, bool& outShowBackfaces)
{
   outAlphaBlend = false; outShowBackfaces = false;
   if (name.empty()) return;
   std::string lower = name;
   std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c){ return (char)std::tolower(c); });
   size_t start = 0;
   while (start <= lower.size()) {
      size_t us = lower.find('_', start);
      size_t end = (us == std::string::npos) ? lower.size() : us;
      if (end > start) {
         std::string tok = lower.substr(start, end - start);
         bool numeric = !tok.empty() && std::all_of(tok.begin(), tok.end(), [](unsigned char c){ return std::isdigit(c) != 0; });
         if (!numeric) {
            if (tok == "a") outAlphaBlend = true;
            else if (tok == "bf") outShowBackfaces = true;
         }
      }
      if (us == std::string::npos) break;
      start = us + 1;
   }
}

struct PreparedMeshInstance
{
   std::string Name;
   glm::mat4 LocalTransform = glm::mat4(1.0f);
   bool Skinned = false;
   std::vector<int> SourceMeshIndices;
   std::vector<MaterialSource> Materials;
   std::vector<std::string> MaterialSlotNames;
   std::vector<std::string> MaterialAssetPaths;
   std::shared_ptr<Mesh> MeshData;
   std::unique_ptr<BlendShapeComponent> BlendTemplate;
};

struct PreparedProxyInstance
{
   std::string Name;
    std::string DisplayName;
   glm::mat4 LocalTransform = glm::mat4(1.0f);
   glm::mat4 RelativeToMesh = glm::mat4(1.0f);
   size_t MeshIndex = 0;
   std::vector<uint32_t> SubmeshSlots;
   bool Skinned = false;
   int OriginalIndex = -1;
};

struct PreparedModelInstance
{
   glm::mat4 RootTransform = glm::mat4(1.0f);
   PreparedSkeleton Skeleton;
   std::vector<PreparedMeshInstance> Meshes;
   std::vector<PreparedProxyInstance> Proxies;
   ClaymoreGUID AssetGuid;
   std::string SourcePath;
   std::string MetaPath;
   bool LoadedFromCache = false;
   // Armor mode: when true, mesh is instantiated without skeleton hierarchy,
   // with UseParentSkeleton enabled and ArmorFitComponent attached
   bool ArmorMode = false;
};


namespace
{
json SerializeMatrixDbg(const glm::mat4& m)
{
   json arr = json::array();
   const float* ptr = glm::value_ptr(m);
   for (int i = 0; i < 16; ++i)
   {
      arr.push_back(ptr[i]);
   }
   return arr;
}

json SerializeVec3Dbg(const glm::vec3& v)
{
   return json::array({ v.x, v.y, v.z });
}

json SerializeTransformDbg(const TransformComponent& transform)
{
   json j;
   j["position"] = SerializeVec3Dbg(transform.Position);
   j["rotationDegrees"] = SerializeVec3Dbg(transform.Rotation);
   j["scale"] = SerializeVec3Dbg(transform.Scale);
   j["useQuat"] = transform.UseQuatRotation;
   return j;
}

json SerializeEntityDbg(Scene& scene, EntityID id)
{
   json j;
   j["entityId"] = id;
   auto* data = scene.GetEntityData(id);
   if (!data)
   {
      j["missing"] = true;
      return j;
   }
   j["name"] = data->Name;
   j["parent"] = data->Parent;
   j["localTransform"] = SerializeTransformDbg(data->Transform);
   j["worldMatrix"] = SerializeMatrixDbg(data->Transform.WorldMatrix);
   return j;
}

json SerializeMatrixArrayDbg(const std::vector<glm::mat4>& mats)
{
   json arr = json::array();
   for (const auto& mat : mats)
   {
      arr.push_back(SerializeMatrixDbg(mat));
   }
   return arr;
}

static bool s_LogModelInstantiation = false;

static void LogMatrixTRS(const std::string& message, const glm::mat4& matrix)
{
   glm::vec3 translation, scale, skew;
   glm::vec4 perspective;
   glm::quat rotationQuat;
   glm::decompose(matrix, scale, rotationQuat, translation, skew, perspective);
   
   glm::vec3 rotationDegrees = glm::degrees(glm::eulerAngles(rotationQuat));
   
   std::cout << "[Scene] " << message
             << " T=(" << translation.x << "," << translation.y << "," << translation.z << ")"
             << " R=(" << rotationDegrees.x << "," << rotationDegrees.y << "," << rotationDegrees.z << ")"
             << " S=(" << scale.x << "," << scale.y << "," << scale.z << ")\n";
}

static void DumpPreparedInstanceDebug(const PreparedModelInstance& instance,
                                      const char* stage)
{
   if (!DebugModelDump::ShouldDump(instance.SourcePath, instance.MetaPath))
   {
      return;
   }

   json j;
   j["stage"] = stage ? stage : "instance";
   j["sourcePath"] = instance.SourcePath;
   j["metaPath"] = instance.MetaPath;
   j["loadedFromCache"] = instance.LoadedFromCache;
   j["rootTransform"] = SerializeMatrixDbg(instance.RootTransform);

   json meshes = json::array();
   for (size_t i = 0; i < instance.Meshes.size(); ++i)
   {
      const auto& mesh = instance.Meshes[i];
      json m;
      m["index"] = i;
      m["name"] = mesh.Name;
      m["skinned"] = mesh.Skinned;
      m["transform"] = SerializeMatrixDbg(mesh.LocalTransform);
      m["sourceIndices"] = mesh.SourceMeshIndices;
      meshes.push_back(std::move(m));
   }
   j["meshes"] = std::move(meshes);

   json proxies = json::array();
   for (const auto& proxy : instance.Proxies)
   {
      json p;
      p["name"] = proxy.Name;
      p["meshIndex"] = proxy.MeshIndex;
      p["skinned"] = proxy.Skinned;
      p["transform"] = SerializeMatrixDbg(proxy.LocalTransform);
      p["slots"] = proxy.SubmeshSlots;
      proxies.push_back(std::move(p));
   }
   j["proxies"] = std::move(proxies);

   j["skeleton"]["boneNames"] = instance.Skeleton.BoneNames;
   j["skeleton"]["boneParents"] = instance.Skeleton.BoneParents;
   j["skeleton"]["inverseBindPoses"] = SerializeMatrixArrayDbg(instance.Skeleton.InverseBindPoses);

   const std::string key = DebugModelDump::ResolveDumpKey(instance.SourcePath, instance.MetaPath);
   DebugModelDump::WriteDump(key, j["stage"].get<std::string>().c_str(), j);
}

static void DumpSceneSnapshotDebug(Scene& scene,
                                   EntityID rootID,
                                   EntityID skeletonRootID,
                                   const std::vector<EntityID>& meshEntities,
                                   const PreparedModelInstance& instance,
                                   const char* stage)
{
   if (!DebugModelDump::ShouldDump(instance.SourcePath, instance.MetaPath))
   {
      return;
   }

   json j;
   j["stage"] = stage ? stage : "scene";
   j["loadedFromCache"] = instance.LoadedFromCache;
   j["sourcePath"] = instance.SourcePath;
   j["metaPath"] = instance.MetaPath;

   if (rootID != INVALID_ENTITY_ID)
   {
      j["rootEntity"] = SerializeEntityDbg(scene, rootID);
   }
   if (skeletonRootID != INVALID_ENTITY_ID)
   {
      j["skeletonRoot"] = SerializeEntityDbg(scene, skeletonRootID);
   }

   json meshes = json::array();
   for (size_t i = 0; i < meshEntities.size(); ++i)
   {
      EntityID id = meshEntities[i];
      if (id == INVALID_ENTITY_ID)
      {
         continue;
      }
      json entry = SerializeEntityDbg(scene, id);
      entry["meshIndex"] = i;
      if (auto* data = scene.GetEntityData(id))
      {
         entry["hasSkinning"] = (data->Skinning != nullptr);
         if (data->Skinning)
         {
            entry["boneCount"] = data->Skinning->BoneCount;
            entry["gpuRemapSize"] = data->Skinning->GpuBoneIndexRemap.size();
         }
      }
      meshes.push_back(std::move(entry));
   }
   j["meshEntities"] = std::move(meshes);

   const std::string key = DebugModelDump::ResolveDumpKey(instance.SourcePath, instance.MetaPath);
   DebugModelDump::WriteDump(key, j["stage"].get<std::string>().c_str(), j);
}

static void MarkSkeletonForDumpDebug(SkeletonComponent& skeleton,
                                     const PreparedModelInstance& instance)
{
   if (!DebugModelDump::ShouldDump(instance.SourcePath, instance.MetaPath))
   {
      return;
   }
   skeleton.DebugSourcePath = DebugModelDump::ResolveDumpKey(instance.SourcePath, instance.MetaPath);
   skeleton.DebugStageHint = instance.LoadedFromCache ? "fast" : "slow";
   skeleton.DebugDumpPending = true;
}
} // namespace

struct SkeletonBuildResult
{
   EntityID SkeletonRoot = INVALID_ENTITY_ID;
   std::unordered_map<std::string, EntityID> BoneNameToEntity;
};

// ApplyMatrixToTransform and EvaluateWorldMatrix are now defined in the core namespace
// (before the #ifndef CLAYMORE_RUNTIME block) so they're available in runtime builds too

static PreparedModelInstance BuildInstanceFromPrepared(const PreparedModel& prepared,
                                                      ClaymoreGUID assetGuid,
                                                      const std::string& sourcePath,
                                                      const std::string& metaPath)
{
   PreparedModelInstance instance;
   instance.RootTransform = prepared.RootLocal;
   instance.Skeleton = prepared.Skeleton;
   instance.AssetGuid = assetGuid;
   instance.SourcePath = sourcePath;
   instance.MetaPath = metaPath;
   instance.LoadedFromCache = false;
   instance.Meshes.reserve(prepared.Meshes.size());
   for (const auto& meshEntry : prepared.Meshes)
   {
      PreparedMeshInstance meshInst;
      meshInst.Name = meshEntry.NodeName;
      meshInst.LocalTransform = meshEntry.LocalTransform;
      meshInst.Skinned = meshEntry.Skinned;
      meshInst.SourceMeshIndices = meshEntry.SourceMeshIndices;
      meshInst.Materials = meshEntry.Materials;
      meshInst.MaterialSlotNames = meshEntry.MaterialSlotNames;
      meshInst.MeshData = meshEntry.MeshData;
      if (!meshEntry.BlendShapes.Shapes.empty())
      {
         meshInst.BlendTemplate = std::make_unique<BlendShapeComponent>(meshEntry.BlendShapes);
      }
      LogMatrixTRS(std::string("BuildInstanceFromPrepared mesh ") + meshInst.Name, meshInst.LocalTransform);
      
      instance.Meshes.push_back(std::move(meshInst));
   }
   instance.Proxies.reserve(prepared.Proxies.size());
   for (const auto& proxy : prepared.Proxies)
   {
      PreparedProxyInstance proxyInst;
      proxyInst.Name = proxy.NodeName;
        proxyInst.DisplayName = proxy.DisplayName.empty() ? proxy.NodeName : proxy.DisplayName;
      proxyInst.MeshIndex = proxy.MeshEntryIndex;
      proxyInst.SubmeshSlots = proxy.SubmeshSlots;
      proxyInst.Skinned = proxy.Skinned;
      proxyInst.OriginalIndex = proxy.OriginalMeshIndex;
      proxyInst.LocalTransform = proxy.LocalTransform;
      instance.Proxies.push_back(std::move(proxyInst));
   }
   DumpPreparedInstanceDebug(instance, "FromPrepared");
   return instance;
}

static glm::mat4 MatrixFromJsonTransform(const nlohmann::json& tf)
{
   if (tf.contains("m") && tf["m"].is_array() && tf["m"].size() == 16)
   {
      glm::mat4 m(1.0f);
      for (int c = 0; c < 4; ++c)
      {
         for (int r = 0; r < 4; ++r)
         {
            m[c][r] = tf["m"][c * 4 + r].get<float>();
         }
      }
      return m;
   }

   glm::vec3 translation(0.0f);
   glm::vec3 scale(1.0f);
   glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);

   if (tf.contains("t") && tf["t"].is_array() && tf["t"].size() == 3)
   {
      translation = glm::vec3(
         tf["t"][0].get<float>(),
         tf["t"][1].get<float>(),
         tf["t"][2].get<float>());
   }
   if (tf.contains("s") && tf["s"].is_array() && tf["s"].size() == 3)
   {
      scale = glm::vec3(
         tf["s"][0].get<float>(),
         tf["s"][1].get<float>(),
         tf["s"][2].get<float>());
   }
   if (tf.contains("r") && tf["r"].is_array() && tf["r"].size() == 4)
   {
      rotation = glm::quat(
         tf["r"][0].get<float>(),
         tf["r"][1].get<float>(),
         tf["r"][2].get<float>(),
         tf["r"][3].get<float>());
      rotation = glm::normalize(rotation);
   }

   glm::mat4 m(1.0f);
   m = glm::translate(glm::mat4(1.0f), translation) *
       glm::mat4_cast(rotation) *
       glm::scale(glm::mat4(1.0f), scale);
   return m;
}

static bool IsFiniteMatrix(const glm::mat4& m)
{
    for (int c = 0; c < 4; ++c)
    {
        for (int r = 0; r < 4; ++r)
        {
            if (!std::isfinite(m[c][r]))
            {
                return false;
            }
        }
    }
    return true;
}

static PreparedModelInstance BuildInstanceFromMeta(const nlohmann::json& meta,
                                                  const BuiltModelPaths& built,
                                                  const skelbin::PackedSkeleton& skeleton,
                                                  ClaymoreGUID guid,
                                                  const std::string& sourcePath,
                                                  const std::string& metaPath,
                                                  const std::vector<int>& requestedMeshIndices,
                                                  meshbin::PrefetchResult&& meshes)
{
   PreparedModelInstance instance;
   instance.AssetGuid = guid;
   instance.SourcePath = sourcePath;
   instance.MetaPath = metaPath;
   instance.LoadedFromCache = true;
   instance.RootTransform = glm::mat4(1.0f);
   if (meta.contains("rootTransform") && meta["rootTransform"].is_object())
   {
      instance.RootTransform = MatrixFromJsonTransform(meta["rootTransform"]);
   }

   // Load import settings (material presets, etc.) from meta
   ModelImportSettings importSettings;
   if (meta.contains("importSettings"))
   {
      importSettings = ModelImportSettings::FromJson(meta["importSettings"]);
   }

   // Check for armor mode - used for armor pieces that should use parent skeleton
   instance.ArmorMode = meta.value("armorMode", false);

   if (!skeleton.inverseBindPoses.empty())
   {
      instance.Skeleton.HasSkeleton = true;
      instance.Skeleton.InverseBindPoses = skeleton.inverseBindPoses;
      instance.Skeleton.BoneParents = skeleton.boneParents;
      instance.Skeleton.BoneNames = skeleton.boneNames;
   }

   std::unordered_map<int, size_t> meshIndexLookup;
   for (size_t i = 0; i < requestedMeshIndices.size() && i < meshes.meshes.size(); ++i)
   {
      meshIndexLookup[requestedMeshIndices[i]] = i;
   }

   if (meta.contains("entries") && meta["entries"].is_array())
   {
      for (const auto& entry : meta["entries"])
      {
         if (!entry.is_object())
         {
            continue;
         }
         int meshIndex = entry.value("meshIndex", -1);
         if (meshIndex < 0)
         {
            continue;
         }
         auto itMesh = meshIndexLookup.find(meshIndex);
         if (itMesh == meshIndexLookup.end())
         {
            continue;
         }
         size_t prefetchIdx = itMesh->second;
         if (prefetchIdx >= meshes.meshes.size())
         {
            continue;
         }
         std::shared_ptr<Mesh> meshPtr = meshes.meshes[prefetchIdx];
         if (!meshPtr)
         {
            continue;
         }

         PreparedMeshInstance meshInst;
         meshInst.Name = entry.value("name", std::string());
         meshInst.Skinned = entry.value("skinned", false);
         meshInst.LocalTransform = glm::mat4(1.0f);
         if (entry.contains("transform") && entry["transform"].is_object())
         {
            meshInst.LocalTransform = MatrixFromJsonTransform(entry["transform"]);
         }
         if (entry.contains("sources") && entry["sources"].is_array())
         {
            meshInst.SourceMeshIndices.reserve(entry["sources"].size());
            for (const auto& srcIdx : entry["sources"])
            {
               meshInst.SourceMeshIndices.push_back(srcIdx.get<int>());
            }
         }
         if (entry.contains("materials") && entry["materials"].is_array())
         {
            meshInst.MaterialAssetPaths.assign(entry["materials"].size(), std::string());
            int materialSlot = 0;
            for (const auto& matJson : entry["materials"])
            {
               MaterialSource matSource = material_serialization::FromJson(matJson);

               // Resolve the slot's material name (prefer slotNames, fall back to the
               // material's own "name") so shared, name-keyed overrides can match.
               std::string slotMatName;
               if (entry.contains("slotNames") && entry["slotNames"].is_array() &&
                   materialSlot < static_cast<int>(entry["slotNames"].size()) &&
                   entry["slotNames"][materialSlot].is_string())
               {
                  slotMatName = entry["slotNames"][materialSlot].get<std::string>();
               }
               if (slotMatName.empty())
                  slotMatName = matJson.value("name", std::string());

               // Apply material preset overrides if defined (per-mesh first, then shared-by-name)
               const MeshMaterialPreset* preset = importSettings.ResolvePreset(meshInst.Name, materialSlot, slotMatName);
               if (preset)
               {
                  if (preset->UseCustomMaterial && !preset->MaterialAssetPath.empty())
                  {
                     if (materialSlot < static_cast<int>(meshInst.MaterialAssetPaths.size()))
                     {
                        meshInst.MaterialAssetPaths[materialSlot] = preset->MaterialAssetPath;
                     }
                  }
                  else
                  {
                     preset->ApplyTo(matSource);
                  }
               }
               
               meshInst.Materials.push_back(std::move(matSource));
               ++materialSlot;
            }
         }
         if (entry.contains("slotNames") && entry["slotNames"].is_array())
         {
            meshInst.MaterialSlotNames.reserve(entry["slotNames"].size());
            for (const auto& nameJson : entry["slotNames"])
            {
               meshInst.MaterialSlotNames.push_back(nameJson.is_string() ? nameJson.get<std::string>() : std::string());
            }
         }
         LogMatrixTRS(std::string("BuildInstanceFromMeta mesh ") + meshInst.Name, meshInst.LocalTransform);
         
         meshInst.MeshData = meshPtr;
         if (prefetchIdx < meshes.blendShapes.size() && meshes.blendShapes[prefetchIdx])
         {
            meshInst.BlendTemplate = std::make_unique<BlendShapeComponent>(*meshes.blendShapes[prefetchIdx]);
         }
         instance.Meshes.push_back(std::move(meshInst));
      }
   }

   if (meta.contains("proxies") && meta["proxies"].is_array())
   {
      for (const auto& proxyJson : meta["proxies"])
      {
         if (!proxyJson.is_object())
         {
            continue;
         }
         PreparedProxyInstance proxy;
         proxy.Name = proxyJson.value("name", std::string());
        proxy.DisplayName = proxyJson.value("displayName", proxy.Name);
         proxy.MeshIndex = static_cast<size_t>(std::max(0, proxyJson.value("meshIndex", 0)));
         proxy.Skinned = proxyJson.value("skinned", false);
         proxy.OriginalIndex = proxyJson.value("originalIndex", -1);
         proxy.LocalTransform = glm::mat4(1.0f);
         if (proxyJson.contains("transform") && proxyJson["transform"].is_object())
         {
            proxy.LocalTransform = MatrixFromJsonTransform(proxyJson["transform"]);
         }
         if (proxyJson.contains("slots") && proxyJson["slots"].is_array())
         {
            for (const auto& slot : proxyJson["slots"])
            {
               proxy.SubmeshSlots.push_back(slot.get<uint32_t>());
            }
         }
         instance.Proxies.push_back(std::move(proxy));
      }
   }
   DumpPreparedInstanceDebug(instance, "FromMeta");
   return instance;
}

static meshbin::PrefetchResult PrefetchMeshesBlocking(const std::string& meshPath,
                                                     const std::vector<int>& indices)
{
   meshbin::PrefetchResult res;
   res.meshes.reserve(indices.size());
   res.blendShapes.reserve(indices.size());
   for (int idx : indices)
   {
      bool skinned = false;
      std::unique_ptr<BlendShapeComponent> blend;
      auto mesh = meshbin::ReadMeshFromBin(meshPath, static_cast<uint32_t>(idx), skinned, &blend);
      res.meshes.push_back(std::move(mesh));
      res.blendShapes.push_back(std::move(blend));
   }
   return res;
}
static void ConfigureAvatarAndAnimation(Scene& scene, EntityData* skeletonEntity, const PreparedModelInstance& instance)
{
   if (!skeletonEntity || !skeletonEntity->Skeleton)
   {
      return;
   }

   SkeletonComponent& skeleton = *skeletonEntity->Skeleton;
   if (!skeleton.Avatar)
   {
      skeleton.Avatar = std::make_unique<cm::animation::AvatarDefinition>();
   }

   std::filesystem::path sourcePath = instance.SourcePath.empty() ? instance.MetaPath : instance.SourcePath;
   if (!sourcePath.empty())
   {
      std::filesystem::path avatarPath = sourcePath;
      avatarPath.replace_extension(".avatar");
      if (!cm::animation::LoadAvatar(*skeleton.Avatar, avatarPath.string()))
      {
         cm::animation::avatar_builders::BuildFromSkeleton(skeleton, *skeleton.Avatar, true);
      }
      else
      {
         cm::animation::avatar_builders::PopulateBindDataFromSkeleton(skeleton, *skeleton.Avatar);
      }
   }
   else
   {
      cm::animation::avatar_builders::BuildFromSkeleton(skeleton, *skeleton.Avatar, true);
   }

   if (!skeletonEntity->AnimationPlayer)
   {
      skeletonEntity->AnimationPlayer = std::make_unique<cm::animation::AnimationPlayerComponent>();
   }
   auto& player = *skeletonEntity->AnimationPlayer;
   if (player.ActiveStates.empty())
   {
      player.ActiveStates.push_back({});
   }
   player.ActiveStates.front().Loop = true;

   std::filesystem::path searchPath = sourcePath.empty() ? std::filesystem::path() : sourcePath.parent_path();
   if (!searchPath.empty())
   {
      std::string stem = sourcePath.stem().string();
      std::string chosenAnim;
      std::error_code ec;
      for (auto it = std::filesystem::directory_iterator(searchPath, ec); !ec && it != std::filesystem::end(it); it.increment(ec))
      {
         if (!it->is_regular_file(ec))
         {
            continue;
         }
         std::string ext = it->path().extension().string();
         std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
         if (ext != ".anim")
         {
            continue;
         }
         std::string filename = it->path().filename().string();
         if (filename.rfind(stem + "_", 0) == 0)
         {
            chosenAnim = it->path().string();
            break;
         }
         if (chosenAnim.empty())
         {
            chosenAnim = it->path().string();
         }
      }

      if (!chosenAnim.empty())
      {
         if (auto assetPtr = cm::animation::LoadAnimationAssetCached(chosenAnim))
         {
            player.CachedAssets[0] = assetPtr;
            player.ActiveStates.front().Asset = assetPtr.get();
            player.ActiveStates.front().Time = 0.0f;
            player.ActiveStates.front().Weight = 1.0f;
            player.SingleClipPath = chosenAnim;
            player.IsPlaying = true;
         }
      }
   }
}

static SkeletonBuildResult BuildSkeletonEntities(Scene& scene,
                                                 const PreparedModelInstance& instance,
                                                 EntityID rootID)
{
   SkeletonBuildResult result{};
   const PreparedSkeleton& preparedSkeleton = instance.Skeleton;
   if (!preparedSkeleton.HasSkeleton || preparedSkeleton.BoneNames.empty())
   {
      return result;
   }

   Entity skeletonRootEnt = scene.CreateEntity("SkeletonRoot");
   EntityID skeletonRootID = skeletonRootEnt.GetID();
   result.SkeletonRoot = skeletonRootID;
   scene.SetParent(skeletonRootID, rootID);
   auto* skeletonData = scene.GetEntityData(skeletonRootID);
   if (!skeletonData)
   {
      return result;
   }
   ApplyMatrixToTransform(skeletonData, glm::mat4(1.0f));
   if (auto* rootDataPtr = scene.GetEntityData(rootID))
   {
      skeletonData->Transform.LocalMatrix = glm::mat4(1.0f);
      skeletonData->Transform.WorldMatrix = rootDataPtr->Transform.WorldMatrix;
      skeletonData->Transform.TransformDirty = false;
   }

   skeletonData->Skeleton = std::make_unique<SkeletonComponent>();
   SkeletonComponent& skeletonComp = *skeletonData->Skeleton;
   skeletonComp.InverseBindPoses = preparedSkeleton.InverseBindPoses;
   skeletonComp.BoneParents = preparedSkeleton.BoneParents;
   skeletonComp.BoneNames = preparedSkeleton.BoneNames;
   skeletonComp.DebugStageHint = instance.LoadedFromCache ? "fast" : "slow";
   skeletonComp.BindPoseGlobals.resize(skeletonComp.InverseBindPoses.size());
   for (size_t i = 0; i < skeletonComp.InverseBindPoses.size(); ++i)
   {
      skeletonComp.BindPoseGlobals[i] = glm::inverse(skeletonComp.InverseBindPoses[i]);
   }
   if (auto* rootData = scene.GetEntityData(rootID))
   {
      skeletonData->Transform.WorldMatrix = rootData->Transform.WorldMatrix * skeletonData->Transform.LocalMatrix;
   }
   skeletonData->Transform.TransformDirty = false;
   skeletonComp.BoneEntities.assign(preparedSkeleton.BoneNames.size(), INVALID_ENTITY_ID);
   skeletonComp.BoneNameToIndex.clear();
   for (size_t i = 0; i < preparedSkeleton.BoneNames.size(); ++i)
   {
      skeletonComp.BoneNameToIndex[preparedSkeleton.BoneNames[i]] = static_cast<int>(i);
   }
   if (instance.AssetGuid.high != 0 || instance.AssetGuid.low != 0)
   {
      skeletonComp.SkeletonGuid = instance.AssetGuid;
      ComputeSkeletonJointGuids(skeletonComp);
   }

   MarkSkeletonForDumpDebug(skeletonComp, instance);

   // Create bone entities.
   // PERF: bones are internal child entities looked up by index, never by a
   // root-unique name, so the O(N) MakeUniqueName scan + per-entity dirty/
   // hierarchy invalidation that CreateEntity performs is pure churn here.
   // Spawning one ~100-bone skeleton through CreateEntity is O(bones x sceneEntities)
   // and dominates spawn cost as the scene grows. Use the batched fast path and
   // invalidate once after the loop instead of per bone.
   for (size_t i = 0; i < preparedSkeleton.BoneNames.size(); ++i)
   {
      Entity boneEnt = scene.CreateEntityExactFast(preparedSkeleton.BoneNames[i]);
      skeletonComp.BoneEntities[i] = boneEnt.GetID();
      // Back-reference: tag this entity as bone `i` of skeleton `skeletonRootID`
      // so consumers can resolve its world matrix from the animated pose buffer.
      if (auto* boneRefData = scene.GetEntityData(boneEnt.GetID()))
      {
         boneRefData->BoneSkeletonEntity = skeletonRootID;
         boneRefData->BoneIndex = static_cast<int>(i);
      }
   }
   // Batched equivalent of the per-entity bookkeeping CreateEntity would have done.
   scene.MarkDirty();
   scene.InvalidateHierarchyCache();

   for (size_t i = 0; i < skeletonComp.BoneEntities.size(); ++i)
   {
      EntityID boneID = skeletonComp.BoneEntities[i];
      int parentIndex = (i < skeletonComp.BoneParents.size()) ? skeletonComp.BoneParents[i] : -1;
      EntityID parentEntity = (parentIndex >= 0 && parentIndex < (int)skeletonComp.BoneEntities.size())
         ? skeletonComp.BoneEntities[parentIndex] : skeletonRootID;
      scene.SetParent(boneID, parentEntity);

      glm::mat4 thisGlobal = glm::inverse(skeletonComp.InverseBindPoses[i]);
      glm::mat4 parentGlobal = (parentIndex >= 0) ? glm::inverse(skeletonComp.InverseBindPoses[parentIndex]) : glm::mat4(1.0f);
      glm::mat4 localBind = glm::inverse(parentGlobal) * thisGlobal;
      if (s_LogModelInstantiation && i == 0)
      {
         std::string stage = instance.LoadedFromCache ? "[fast]" : "[slow]";
         LogMatrixTRS(std::string("[Scene] Skeleton bindPoseGlobal[0] ") + stage, thisGlobal);
         LogMatrixTRS(std::string("[Scene] Skeleton invBind[0] ") + stage, skeletonComp.InverseBindPoses[i]);
         LogMatrixTRS(std::string("[Scene] Skeleton localBind[0] ") + stage, localBind);
      }
      if (s_LogModelInstantiation)
      {
         auto itRightArm = skeletonComp.BoneNameToIndex.find("mixamorig:RightArm");
         if (itRightArm != skeletonComp.BoneNameToIndex.end() && itRightArm->second == static_cast<int>(i))
         {
            std::string stage = instance.LoadedFromCache ? "[fast]" : "[slow]";
            LogMatrixTRS(std::string("[Scene] Skeleton bindPoseGlobal[RightArm] ") + stage, thisGlobal);
            LogMatrixTRS(std::string("[Scene] Skeleton invBind[RightArm] ") + stage, skeletonComp.InverseBindPoses[i]);
            LogMatrixTRS(std::string("[Scene] Skeleton localBind[RightArm] ") + stage, localBind);
         }
      }
      auto* boneData = scene.GetEntityData(boneID);
      ApplyMatrixToTransform(boneData, localBind);
      if (boneData)
      {
         boneData->Transform.LocalMatrix = localBind;
         glm::mat4 parentWorld = glm::mat4(1.0f);
         if (parentEntity != INVALID_ENTITY_ID)
         {
            if (auto* parentData = scene.GetEntityData(parentEntity))
            {
               parentWorld = parentData->Transform.WorldMatrix;
            }
         }
         boneData->Transform.WorldMatrix = parentWorld * boneData->Transform.LocalMatrix;
         boneData->Transform.TransformDirty = false;
      }
      if (i < preparedSkeleton.BoneNames.size() && !preparedSkeleton.BoneNames[i].empty())
      {
         result.BoneNameToEntity[preparedSkeleton.BoneNames[i]] = boneID;
      }
   }

   ConfigureAvatarAndAnimation(scene, skeletonData, instance);
   return result;
}

static std::shared_ptr<Material> AcquireDefaultMaterial(Scene& scene, bool skinned)
{
   if (skinned)
   {
      return MaterialManager::Instance().CreateSceneSkinnedDefaultMaterial(&scene);
   }
   return MaterialManager::Instance().CreateSceneDefaultMaterial(&scene);
}

static void PopulateMaterialSlots(Scene& scene,
                                  MeshComponent& meshComponent,
                                  const PreparedMeshInstance& meshInfo)
{
   size_t slotCount = meshInfo.Materials.empty() ? 1 : meshInfo.Materials.size();
   meshComponent.materials.resize(slotCount);
   meshComponent.MaterialSlotNames.assign(slotCount, std::string());
   meshComponent.MaterialAssetPaths.assign(slotCount, std::string());
   meshComponent.SlotPropertyBlocks.assign(slotCount, MaterialPropertyBlock{});
   meshComponent.SlotPropertyBlockTexturePaths.assign(slotCount, {});
   meshComponent.MaterialSources = meshInfo.Materials;
   if (meshComponent.MaterialSources.size() < slotCount)
   {
      meshComponent.MaterialSources.resize(slotCount);
   }
   if (!meshInfo.MaterialAssetPaths.empty())
   {
      meshComponent.MaterialAssetPaths = meshInfo.MaterialAssetPaths;
      if (meshComponent.MaterialAssetPaths.size() < slotCount)
      {
         meshComponent.MaterialAssetPaths.resize(slotCount);
      }
   }
   if (!meshInfo.MaterialSlotNames.empty())
   {
      meshComponent.MaterialSlotNames = meshInfo.MaterialSlotNames;
      if (meshComponent.MaterialSlotNames.size() < slotCount)
      {
         meshComponent.MaterialSlotNames.resize(slotCount);
      }
   }

   for (size_t slot = 0; slot < slotCount; ++slot)
   {
      std::shared_ptr<Material> slotMaterial;
      bool hasCustomMaterial = (slot < meshComponent.MaterialAssetPaths.size() &&
         !meshComponent.MaterialAssetPaths[slot].empty());
      if (hasCustomMaterial)
      {
         slotMaterial = MaterialAssetCache::Acquire(meshComponent.MaterialAssetPaths[slot]);
      }
      if (!slotMaterial && slot < meshInfo.Materials.size())
      {
         slotMaterial = AcquireMaterialFromSource(meshComponent.MaterialSources[slot], scene);
      }
      if (!slotMaterial)
      {
         slotMaterial = AcquireDefaultMaterial(scene, meshInfo.Skinned);
      }
      meshComponent.materials[slot] = slotMaterial;
      if (slot == 0)
      {
         meshComponent.material = slotMaterial;
      }
      if (meshComponent.MaterialSlotNames[slot].empty())
      {
         if (slot < meshComponent.MaterialSources.size() && !meshComponent.MaterialSources[slot].Name.empty())
         {
            meshComponent.MaterialSlotNames[slot] = meshComponent.MaterialSources[slot].Name;
         }
         else
         {
            meshComponent.MaterialSlotNames[slot] = std::string("Slot ") + std::to_string(slot);
         }
      }

      if (slot < meshComponent.SlotPropertyBlocks.size() &&
          slot < meshComponent.SlotPropertyBlockTexturePaths.size())
      {
         if (!hasCustomMaterial)
         {
            PopulatePropertyBlockFromSource(meshComponent.MaterialSources[slot],
               meshComponent.SlotPropertyBlocks[slot],
               meshComponent.SlotPropertyBlockTexturePaths[slot]);
            
            // Apply alpha cutout threshold using per-slot property block
            if (meshComponent.MaterialSources[slot].AlphaCutout)
            {
               glm::vec4 scalar1(0.0f);
               if (auto pbr = dynamic_cast<PBRMaterial*>(slotMaterial.get()))
               {
                  scalar1.x = pbr->GetEmissionStrength();
               }
               auto it = meshComponent.SlotPropertyBlocks[slot].Vec4Uniforms.find("u_PBRScalar1");
               if (it != meshComponent.SlotPropertyBlocks[slot].Vec4Uniforms.end())
               {
                  scalar1 = it->second;
               }
               scalar1.y = meshComponent.MaterialSources[slot].AlphaCutoutThreshold;
               meshComponent.SlotPropertyBlocks[slot].SetVector("u_PBRScalar1", scalar1);
            }
         }
         else
         {
            meshComponent.SlotPropertyBlocks[slot].Clear();
            meshComponent.SlotPropertyBlockTexturePaths[slot].clear();
         }
      }
   }
   }

static EntityID CreateMeshEntity(Scene& scene,
                                 const PreparedModelInstance& instance,
                                 const PreparedMeshInstance& meshInfo,
                                 size_t meshIndex,
                                 EntityID rootID,
                                 EntityID skeletonRootID)
{
   if (!meshInfo.MeshData)
   {
      return INVALID_ENTITY_ID;
   }

   // Use original node name directly - no longer need "_Mesh" suffix since
   // proxy entities are not created for skinned meshes
   std::string meshEntityName = meshInfo.Name.empty() ? "Mesh" : meshInfo.Name;
   Entity meshEntity = scene.CreateEntity(meshEntityName.empty() ? "Mesh" : meshEntityName);
   EntityID meshID = meshEntity.GetID();
   bool useSkeletonParent = meshInfo.Skinned && skeletonRootID != INVALID_ENTITY_ID;
   scene.SetParent(meshID, useSkeletonParent ? skeletonRootID : rootID);
   auto* meshData = scene.GetEntityData(meshID);
   if (!meshData)
   {
      return INVALID_ENTITY_ID;
   }

   // Prepared skinned meshes are already baked into skeleton space (see ModelPreprocessor),
   // so we always respect the serialized local transform and avoid applying proxy offsets here.
   glm::mat4 meshTransform = meshInfo.LocalTransform;
   ApplyMatrixToTransform(meshData, meshTransform);
   if (meshData)
   {
      meshData->Transform.LocalMatrix = meshTransform;
      glm::mat4 parentWorld = glm::mat4(1.0f);
      if (auto* parentData = scene.GetEntityData(useSkeletonParent ? skeletonRootID : rootID))
      {
         parentWorld = parentData->Transform.WorldMatrix;
      }
      meshData->Transform.WorldMatrix = parentWorld * meshData->Transform.LocalMatrix;
      meshData->Transform.TransformDirty = false;
   }
   if (s_LogModelInstantiation && meshInfo.Skinned)
   {
      std::string stage = instance.LoadedFromCache ? "[fast]" : "[slow]";
      LogMatrixTRS(std::string("Mesh world ") + meshInfo.Name + " " + stage, meshData->Transform.WorldMatrix);
   }
   std::shared_ptr<Material> primaryMaterial;
   if (!meshInfo.Materials.empty())
   {
      primaryMaterial = AcquireMaterialFromSource(meshInfo.Materials.front(), scene);
   }
   if (!primaryMaterial)
   {
      primaryMaterial = AcquireDefaultMaterial(scene, meshInfo.Skinned);
   }
   meshData->Mesh = std::make_unique<MeshComponent>(meshInfo.MeshData, meshInfo.Name, primaryMaterial);
   if (!meshData->RenderOverrides)
   {
      meshData->RenderOverrides = std::make_unique<RenderOverridesComponent>();
   }
   if (meshData->Mesh && meshData->Mesh->mesh)
   {
      meshData->Mesh->SubmeshOwners.assign(meshData->Mesh->mesh->Submeshes.size(), INVALID_ENTITY_ID);
   }
   if (!meshData->Mesh)
   {
      return meshID;
   }

   if (s_LogModelInstantiation && meshInfo.Skinned && meshData->Mesh && meshData->Mesh->mesh)
   {
      Mesh* runtimeMesh = meshData->Mesh->mesh.get();
      if (!runtimeMesh->BoneIndices.empty() && !runtimeMesh->BoneWeights.empty())
      {
         glm::ivec4 bi = runtimeMesh->BoneIndices.front();
         glm::vec4 bw = runtimeMesh->BoneWeights.front();
         std::cout << "[Scene] Mesh " << meshInfo.Name
                   << " firstVertex bones=(" << bi.x << "," << bi.y << "," << bi.z << "," << bi.w << ")"
                   << " weights=(" << bw.x << "," << bw.y << "," << bw.z << "," << bw.w << ") "
                   << (instance.LoadedFromCache ? "[fast]" : "[slow]") << "\n";
         
         // Debug: Analyze bone index distribution
         std::map<int, int> boneIndexCounts;
         int maxBoneIndex = 0;
         for (const auto& indices : runtimeMesh->BoneIndices) {
            for (int c = 0; c < 4; ++c) {
               int idx = indices[c];
               boneIndexCounts[idx]++;
               if (idx > maxBoneIndex) maxBoneIndex = idx;
            }
         }
         std::cout << "[Scene] Mesh " << meshInfo.Name << " bone index distribution (max=" << maxBoneIndex << "):\n";
         for (const auto& [idx, count] : boneIndexCounts) {
            std::cout << "  bone[" << idx << "]: " << count << " references\n";
         }
      }
   }

   PopulateMaterialSlots(scene, *meshData->Mesh, meshInfo);
   meshData->Mesh->CombinedSubmeshFileIDs = meshInfo.SourceMeshIndices;
   if (instance.AssetGuid.high != 0 || instance.AssetGuid.low != 0)
   {
      // Use meshIndex as the fileID - this is the index in the prepared meshes array
      // which directly corresponds to the submesh index in the meshbin file.
      // Note: SourceMeshIndices contains original model mesh indices (before preprocessing),
      // which may differ from meshbin indices when meshes are combined/filtered.
      int32_t fileId = static_cast<int32_t>(meshIndex);
      meshData->Mesh->meshReference = AssetReference(instance.AssetGuid, fileId, static_cast<int32_t>(AssetType::Mesh));
   }

   // Apply alpha blend from MaterialSource (e.g., from import presets)
   // This must happen before name-based hints so presets take precedence
   if (!meshInfo.Materials.empty() && meshInfo.Materials.front().AlphaBlend)
   {
      meshData->RenderOverrides->AlphaBlendEnabled = true;
   }
   if (!meshInfo.Materials.empty() &&
       std::all_of(meshInfo.Materials.begin(), meshInfo.Materials.end(),
           [](const MaterialSource& source) { return source.TwoSided; }))
   {
      meshData->Mesh->ShowBackfaces = true;
   }

   // Apply name-based material hints on instantiation ("_a" alpha, "_bf" backfaces)
   // IMPORTANT: Do NOT modify shared material state flags here - that would affect all entities
   // using the cached material! Instead, use RenderOverridesComponent for per-entity overrides.
   {
      bool wantBlend = false, wantBackfaces = false;
      ParseNameSuffixHints(meshInfo.Name, wantBlend, wantBackfaces);
      if (wantBlend || wantBackfaces)
      {
         if (wantBlend)
         {
            meshData->RenderOverrides->AlphaBlendEnabled = true;
         }
         if (wantBackfaces)
         {
            meshData->Mesh->ShowBackfaces = true;
         }
      }
   }

   if (meshInfo.Skinned && skeletonRootID != INVALID_ENTITY_ID)
   {
      meshData->Skinning = std::make_unique<SkinningComponent>();
      meshData->Skinning->SkeletonRoot = skeletonRootID;
      // Store original bone names and inverse bind poses from the mesh's source skeleton.
      // This is required for bone remapping when UseParentSkeleton is enabled and the
      // mesh is bound to a different skeleton than the one it was authored with.
      if (!instance.Skeleton.BoneNames.empty())
      {
         meshData->Skinning->OriginalBoneNames = instance.Skeleton.BoneNames;
         meshData->Skinning->OriginalInverseBindPoses = instance.Skeleton.InverseBindPoses;
      }
   }
   
   // Handle armor mode: skinned mesh without explicit skeleton root
   // Still needs SkinningComponent with original bone data for UseParentSkeleton to work
   if (meshInfo.Skinned && skeletonRootID == INVALID_ENTITY_ID && !instance.Skeleton.BoneNames.empty())
   {
      if (!meshData->Skinning)
      {
         meshData->Skinning = std::make_unique<SkinningComponent>();
      }
      
      // Store the original bone data from the mesh's source skeleton
      meshData->Skinning->OriginalBoneNames = instance.Skeleton.BoneNames;
      meshData->Skinning->OriginalInverseBindPoses = instance.Skeleton.InverseBindPoses;
      
   }

   if (meshInfo.BlendTemplate)
   {
      auto blendCopy = std::make_unique<BlendShapeComponent>(*meshInfo.BlendTemplate);
      meshData->Mesh->BlendShapes = blendCopy.get();
      meshData->BlendShapes = std::move(blendCopy);
      
      // Apply any pending blend shape weights from scene/prefab deserialization
      if (!meshData->PendingBlendShapeWeights.empty()) {
         for (auto& shape : meshData->BlendShapes->Shapes) {
            auto it = meshData->PendingBlendShapeWeights.find(shape.Name);
            if (it != meshData->PendingBlendShapeWeights.end()) {
               shape.Weight = it->second;
            }
         }
         meshData->BlendShapes->Dirty = true;
         meshData->PendingBlendShapeWeights.clear();
      }
   }

   return meshID;
}

static void InstantiateProxies(Scene& scene,
                               const PreparedModelInstance& instance,
                               const std::vector<EntityID>& meshEntities,
                               EntityID rootID,
                               const SkeletonBuildResult& skeletonResult)
{
   for (const PreparedProxyInstance& proxy : instance.Proxies)
   {
      // Skip proxy creation for skinned meshes - they now use shared skeleton bone buffer
      // and material overrides are applied directly via MeshComponent.materials/SlotPropertyBlocks
      if (proxy.Skinned)
      {
         continue;
      }
      
      if (proxy.MeshIndex >= meshEntities.size())
      {
         continue;
      }
      EntityID targetMeshEntity = meshEntities[proxy.MeshIndex];
      if (targetMeshEntity == INVALID_ENTITY_ID)
      {
         continue;
      }

      std::string proxyEntityName = proxy.DisplayName.empty() ? proxy.Name : proxy.DisplayName;
      Entity proxyEntity = scene.CreateEntity(proxyEntityName.empty() ? "MeshProxy" : proxyEntityName);
      EntityID proxyID = proxyEntity.GetID();
      EntityID desiredParent = rootID;
      if (!proxy.Name.empty())
      {
         auto it = skeletonResult.BoneNameToEntity.find(proxy.Name);
         if (it != skeletonResult.BoneNameToEntity.end())
         {
            desiredParent = it->second;
         }
      }
      scene.SetParent(proxyID, desiredParent);
      auto* proxyData = scene.GetEntityData(proxyID);
      if (!proxyData)
      {
         continue;
      }
      ApplyMatrixToTransform(proxyData, proxy.LocalTransform);
      scene.MarkTransformDirty(proxyID);

      proxyData->MeshProxy = std::make_unique<MeshProxyComponent>();
      auto& proxyComp = *proxyData->MeshProxy;
      proxyComp.TargetMesh = targetMeshEntity;
      proxyComp.SerializedTarget = targetMeshEntity;
      proxyComp.SubmeshSlots = proxy.SubmeshSlots;
      proxyComp.SlotPropertyBlocks.assign(proxy.SubmeshSlots.size(), {});
      proxyComp.SlotPropertyBlockTexturePaths.assign(proxy.SubmeshSlots.size(), {});
      proxyComp.SlotMaterialOverrides.assign(proxy.SubmeshSlots.size(), nullptr);
      proxyComp.SlotMaterialAssetPaths.assign(proxy.SubmeshSlots.size(), std::string());
      proxyComp.SlotIndexLookup.clear();
      for (size_t i = 0; i < proxy.SubmeshSlots.size(); ++i)
      {
         proxyComp.SlotIndexLookup[proxy.SubmeshSlots[i]] = i;
      }

      auto* ownerData = scene.GetEntityData(targetMeshEntity);
      if (ownerData)
      {
         proxyComp.TargetGuidHint = ownerData->EntityGuid;
      }
      if (ownerData && ownerData->Mesh && ownerData->Mesh->mesh)
      {
         if (ownerData->Mesh->SubmeshOwners.size() < ownerData->Mesh->mesh->Submeshes.size())
         {
            ownerData->Mesh->SubmeshOwners.resize(ownerData->Mesh->mesh->Submeshes.size(), INVALID_ENTITY_ID);
         }
         for (uint32_t slot : proxy.SubmeshSlots)
         {
            if (slot < ownerData->Mesh->SubmeshOwners.size())
            {
               ownerData->Mesh->SubmeshOwners[slot] = proxyID;
            }
         }
      }
   }
}



static void BuildUnifiedMorphComponent(Scene& scene,
                                       EntityID targetRoot,
                                       const std::vector<EntityID>& meshEntities)
{
   std::unordered_map<std::string, int> nameCounts;
   for (EntityID meshID : meshEntities)
   {
      auto* data = scene.GetEntityData(meshID);
      if (!data || !data->Mesh || !data->BlendShapes || !data->BlendShapes->Shapes.size())
      {
         continue;
      }
      for (const auto& shape : data->BlendShapes->Shapes)
      {
         nameCounts[shape.Name]++;
      }
   }

   std::vector<std::string> unifiedNames;
   for (const auto& kv : nameCounts)
   {
      // Include all blendshapes (even if only on one mesh) so they can be 
      // controlled from the model root. Threshold lowered from 2 to 1.
      if (kv.second >= 1)
      {
         unifiedNames.push_back(kv.first);
      }
   }
   if (unifiedNames.empty())
   {
      return;
   }

   auto* targetData = scene.GetEntityData(targetRoot);
   if (!targetData)
   {
      return;
   }
   targetData->UnifiedMorph = std::make_unique<UnifiedMorphComponent>();
   targetData->UnifiedMorph->Names = unifiedNames;
   targetData->UnifiedMorph->Weights.assign(unifiedNames.size(), 0.0f);
   targetData->UnifiedMorph->NameIndexDirty = true;
   targetData->UnifiedMorph->NameIndex.clear();
   targetData->UnifiedMorph->MemberMeshes.clear();
   for (EntityID meshID : meshEntities)
   {
      if (meshID != INVALID_ENTITY_ID)
      {
         targetData->UnifiedMorph->MemberMeshes.push_back(meshID);
      }
   }
   
   // Apply any pending unified morph weights from scene/prefab deserialization
   if (!targetData->PendingUnifiedMorphWeights.empty()) {
      for (size_t i = 0; i < targetData->UnifiedMorph->Names.size(); ++i) {
         auto it = targetData->PendingUnifiedMorphWeights.find(targetData->UnifiedMorph->Names[i]);
         if (it != targetData->PendingUnifiedMorphWeights.end()) {
            targetData->UnifiedMorph->Weights[i] = it->second;
         }
      }
      targetData->PendingUnifiedMorphWeights.clear();
      
      // Propagate the restored weights to child mesh blend shapes
      for (EntityID meshId : targetData->UnifiedMorph->MemberMeshes) {
         auto* meshData = scene.GetEntityData(meshId);
         if (!meshData || !meshData->BlendShapes) continue;
         for (auto& shape : meshData->BlendShapes->Shapes) {
            for (size_t i = 0; i < targetData->UnifiedMorph->Names.size(); ++i) {
               if (shape.Name == targetData->UnifiedMorph->Names[i]) {
                  shape.Weight = targetData->UnifiedMorph->Weights[i];
                  meshData->BlendShapes->Dirty = true;
               }
            }
         }
      }
   }
}

static bool IsDefaultTransform(const TransformComponent& t)
{
   const float eps = 0.0001f;
   auto nearEq = [&](float a, float b) { return std::fabs(a - b) <= eps; };
   return nearEq(t.Position.x, 0.0f) && nearEq(t.Position.y, 0.0f) && nearEq(t.Position.z, 0.0f) &&
      nearEq(t.Scale.x, 1.0f) && nearEq(t.Scale.y, 1.0f) && nearEq(t.Scale.z, 1.0f) &&
      nearEq(t.RotationQ.x, 0.0f) && nearEq(t.RotationQ.y, 0.0f) && nearEq(t.RotationQ.z, 0.0f) &&
      nearEq(t.RotationQ.w, 1.0f);
}

static EntityID InstantiatePreparedModel(Scene& scene,
                                         const PreparedModelInstance& instance,
                                         const glm::vec3& spawnOffset,
                                         EntityID existingRoot)
{
   const std::string sourcePath = instance.SourcePath.empty() ? instance.MetaPath : instance.SourcePath;
   const std::string rootName = DeriveModelRootName(sourcePath, "ImportedModel");
   EntityID rootID = existingRoot;
   EntityData* rootData = nullptr;
   bool reuseRoot = (existingRoot != INVALID_ENTITY_ID);
   if (reuseRoot)
   {
      rootData = scene.GetEntityData(rootID);
      if (!rootData)
      {
         reuseRoot = false;
      }
   }
   if (!reuseRoot)
   {
      Entity rootEntity = scene.CreateEntity(rootName);
      rootID = rootEntity.GetID();
      rootData = scene.GetEntityData(rootID);
   }
   if (!rootData)
   {
      return INVALID_ENTITY_ID;
   }
   if (reuseRoot)
   {
      rootData->Name = rootName;
   }

   bool preserveTransform = reuseRoot && !IsDefaultTransform(rootData->Transform);
   if (!preserveTransform)
   {
      ApplyMatrixToTransform(rootData, instance.RootTransform);
      if (rootData)
      {
         rootData->Transform.LocalMatrix = instance.RootTransform;
         rootData->Transform.WorldMatrix = rootData->Transform.LocalMatrix;
         rootData->Transform.TransformDirty = false;
      }
      rootData->Transform.Position += spawnOffset;
   }
   // Mark transform dirty so world matrix is recalculated with the spawn offset
   scene.MarkTransformDirty(rootID);

   // Store model asset GUID on root entity for prefab serialization (Core stores GUID only)
   if (instance.AssetGuid.high != 0 || instance.AssetGuid.low != 0)
   {
      rootData->ModelAssetGuid = instance.AssetGuid;
   }
   // Note: Zero AssetGuid is common for runtime-created models - not a warning

   // In armor mode, skip skeleton hierarchy - meshes will use parent skeleton
   SkeletonBuildResult skeletonResult;
   EntityID skeletonRootID = INVALID_ENTITY_ID;
   if (!instance.ArmorMode)
   {
      skeletonResult = BuildSkeletonEntities(scene, instance, rootID);
      skeletonRootID = skeletonResult.SkeletonRoot;
   }

   std::vector<EntityID> meshEntities(instance.Meshes.size(), INVALID_ENTITY_ID);
   for (size_t i = 0; i < instance.Meshes.size(); ++i)
   {
      // In armor mode, pass INVALID_ENTITY_ID as skeleton root so meshes are direct children of root
      EntityID meshSkeletonRoot = instance.ArmorMode ? INVALID_ENTITY_ID : skeletonRootID;
      meshEntities[i] = CreateMeshEntity(scene,
         instance,
         instance.Meshes[i],
         i,
         rootID,
         meshSkeletonRoot);
      // In armor mode, configure skinning to use parent skeleton and add ArmorFit component
      if (auto* md = scene.GetEntityData(meshEntities[i]))
      {
         if (instance.ArmorMode && md->Skinning)
         {
            md->Skinning->UseParentSkeleton = true;
         }
         if (instance.ArmorMode && md->Mesh && md->Skinning)
         {
            // Add ArmorFitComponent for armor wrap deformation
            md->ArmorFit = std::make_unique<cm::deformation::ArmorFitComponent>();
            md->ArmorFit->GlobalWrapWeight = 1.0f;
         }
      }
   }

   InstantiateProxies(scene, instance, meshEntities, rootID, skeletonResult);
   EntityID morphRoot = (skeletonRootID != INVALID_ENTITY_ID) ? skeletonRootID : rootID;
   BuildUnifiedMorphComponent(scene, morphRoot, meshEntities);
   DumpSceneSnapshotDebug(scene,
                          rootID,
                          skeletonRootID,
                          meshEntities,
                          instance,
                          instance.LoadedFromCache ? "InstantiateFast" : "InstantiateSlow");
   return rootID;
}

} // end editor-only anonymous namespace
#endif // CLAYMORE_RUNTIME - End of editor-only model import types and functions

//-----------------------------------------------------------------------------------------
// Job System Kernels (core - used by UpdateTransforms)
//-----------------------------------------------------------------------------------------
namespace { // core anonymous namespace

struct RootArgs {
   Scene* scene;
   const std::vector<EntityID>* level;   // level 0 (roots)
   std::vector<uint8_t>* recomputed;     // size >= scene->m_NextID
};

static inline void RootsKernel(const RootArgs& a, size_t start, size_t count) {
   for (size_t i = start; i < start + count; ++i) {
      EntityID id = (*a.level)[i];
      auto* d = a.scene->GetEntityData(id);
      if (!d)
      {
         continue;
      }
      const bool wasDirty = d->Transform.TransformDirty;
      if (wasDirty)
      {
         d->Transform.CalculateLocalMatrix();
      }
      d->Transform.WorldMatrix = d->Transform.LocalMatrix; // root: parent = I
      d->Transform.TransformDirty = false;
      (*a.recomputed)[id] = wasDirty ? 1u : 0u;
   }
}

struct PropArgs {
   Scene* scene;
   const std::vector<EntityID>* level;       // current level
   std::vector<uint8_t>* recomputed;         // per-entity "updated this frame" flag
};

static inline void PropagateKernel(const PropArgs& a, size_t start, size_t count) {
   for (size_t i = start; i < start + count; ++i) {
      EntityID id = (*a.level)[i];
      auto* d = a.scene->GetEntityData(id);
      if (!d)
      {
         continue;
      }

      // parent info (guaranteed processed on a previous level)
      glm::mat4 parentWorld = glm::mat4(1.0f);
      bool parentUpdated = false;
      if (d->Parent != -1) {
         auto* p = a.scene->GetEntityData(d->Parent);
         parentWorld = p ? p->Transform.WorldMatrix : glm::mat4(1.0f);
         parentUpdated = p ? ((*a.recomputed)[d->Parent] != 0) : false;
      }

      const bool wasDirty = d->Transform.TransformDirty;
      const bool needs = wasDirty || parentUpdated;
      if (needs) {
         // Local changed
         if (wasDirty)
         {
            d->Transform.CalculateLocalMatrix();
         }
         d->Transform.WorldMatrix = parentWorld * d->Transform.LocalMatrix;
         d->Transform.TransformDirty = false;
      }
      (*a.recomputed)[id] = needs ? 1u : 0u;
   }
}

// --- Build breadth levels from your existing parent/children lists
static inline void BuildHierarchyLevels(Scene& scene,
   std::vector<std::vector<EntityID>>& levels)
{
   levels.clear();
   std::vector<EntityID> roots;
   for (const auto& e : scene.GetEntities()) {
      auto* d = scene.GetEntityData(e.GetID());
      if (d && d->Parent == -1) roots.push_back(e.GetID());
   }
   if (roots.empty())
   {
      return;
   }
   levels.push_back(std::move(roots));

   // BFS: each level is the concatenation of all children from the previous level
   for (;;) {
      std::vector<EntityID> next;
      for (EntityID id : levels.back()) {
         auto* d = scene.GetEntityData(id);
         if (!d)
         {
            continue;
         }
         next.insert(next.end(), d->Children.begin(), d->Children.end());
      }
      if (next.empty())
      {
         break;
      }
      levels.push_back(std::move(next));
   }
}

} // end core anonymous namespace

// =========================================================================
// PERF: Rebuild hierarchy cache only when structure changes
// =========================================================================
void Scene::RebuildHierarchyCacheIfNeeded() const {
   if (!m_HierarchyDirty) return;
   
   BuildHierarchyLevels(const_cast<Scene&>(*this), m_CachedHierarchyLevels);
   
   // Also resize the recomputed flags buffer to current entity count
   // This avoids per-frame allocation in UpdateTransforms
   if (m_CachedRecomputedFlags.size() < m_NextID) {
      m_CachedRecomputedFlags.resize(m_NextID, 0);
   }
   
   m_HierarchyDirty = false;
}

void Scene::RebuildPhysicsParticipantCacheIfNeeded() {
   const bool periodicRescanDue = (m_PhysicsParticipantRescanFrames == 0);
   const bool entityCountChanged = (m_PhysicsParticipantEntityCount != m_Entities.size());
   const bool revisionChanged = (m_PhysicsParticipantCacheRevision != m_DirtyRevision);
   if (!m_PhysicsParticipantCacheDirty && !periodicRescanDue && !entityCountChanged && !revisionChanged) {
      if (m_PhysicsParticipantRescanFrames > 0) {
         --m_PhysicsParticipantRescanFrames;
      }
      return;
   }

   m_PhysicsParticipants.clear();
   m_PhysicsParticipants.reserve(m_Entities.size());
   for (const auto& [id, data] : m_Entities) {
      if (data.RigidBody || data.StaticBody || data.CharacterController || data.Collider || data.Area) {
         m_PhysicsParticipants.push_back(id);
      }
   }

   m_PhysicsParticipantCacheRevision = m_DirtyRevision;
   m_PhysicsParticipantEntityCount = m_Entities.size();
   m_PhysicsParticipantRescanFrames = 60; // safety net for runtime component toggles.
   m_PhysicsParticipantCacheDirty = false;
}
// -----------------------------------------------------------------------------------------

Scene* Scene::CurrentScene = nullptr;

void Scene::SetEditorViewportState(const EditorViewportState& state) {
    m_EditorViewportState = state;
    m_HasEditorViewportState = true;
}

void Scene::ClearEditorViewportState() {
    m_HasEditorViewportState = false;
}



///-----------------------------------------------------------------------------------------
/// Destructor: Clean up scene resources
///-----------------------------------------------------------------------------------------
Scene::Scene() {
   EnsureRuntimeWorldTransformTrackingCapacity(static_cast<size_t>(m_NextID));
}

Scene::~Scene() {
   // Mark that we're being destroyed - scripts should not try to destroy entities
   m_IsBeingDestroyed = true;
   
   // CRITICAL: Wait for all pending JobSystem work to complete before destroying entities
   // This prevents use-after-free crashes if parallel jobs are still accessing component data
   // (e.g., SkinningSystem parallel_for accessing GPU skinning caches)
   if (cm::g_JobSystem) {
      cm::g_JobSystem->WaitIdle();
   }
   
   // Entities will be cleaned up automatically by m_Entities destructor
}

Scene::Scene(Scene&& other) noexcept
   : m_Entities(std::move(other.m_Entities))
   , m_EntityList(std::move(other.m_EntityList))
   , m_NextID(other.m_NextID)
   , m_Environment(std::move(other.m_Environment))
   , m_PendingRemovals(std::move(other.m_PendingRemovals))
   , m_PendingCreations(std::move(other.m_PendingCreations))
   , m_IsDirty(other.m_IsDirty)
   , m_IsBeingDestroyed(other.m_IsBeingDestroyed)
   , m_DefaultShaderPreset(other.m_DefaultShaderPreset)
   , m_EditorViewportState(std::move(other.m_EditorViewportState))
   , m_HasEditorViewportState(other.m_HasEditorViewportState)
   , m_PendingSceneLoad(other.m_PendingSceneLoad)
   , m_PendingSceneLoadAsync(other.m_PendingSceneLoadAsync)
   , m_PendingSceneLoadPath(std::move(other.m_PendingSceneLoadPath))
   , m_Loading(other.m_Loading)
   , m_Loaded(other.m_Loaded)
   , m_LoadingAsync(other.m_LoadingAsync)
   , m_LoadProgress(other.m_LoadProgress)
   , m_LoadScriptsTotal(other.m_LoadScriptsTotal)
   , m_LoadScriptsProcessed(other.m_LoadScriptsProcessed)
   , m_LoadScriptsReady(other.m_LoadScriptsReady)
   , m_DeferScriptOnCreate(other.m_DeferScriptOnCreate)
   , m_RestoreDeferredScriptInit(other.m_RestoreDeferredScriptInit)
   , m_PrevDeferredScriptInit(other.m_PrevDeferredScriptInit)
   , m_LoadingScenePath(std::move(other.m_LoadingScenePath))
   , m_ScenePath(std::move(other.m_ScenePath))
   , m_LoadStartTime(other.m_LoadStartTime)
   , m_LoadHoldStartTime(other.m_LoadHoldStartTime)
   , m_LoadHoldDuration(other.m_LoadHoldDuration)
   , m_LoadHoldStartProgress(other.m_LoadHoldStartProgress)
   , m_LoadingHold(other.m_LoadingHold)
   , m_AsyncLoadJob(std::move(other.m_AsyncLoadJob))
   , m_BodyMap(std::move(other.m_BodyMap))
   , m_EditScene(std::move(other.m_EditScene))
   , m_RuntimeScene(std::move(other.m_RuntimeScene))
   , m_IsPlaying(other.m_IsPlaying)
   , m_IsPaused(other.m_IsPaused)
   , m_CachedHierarchyLevels(std::move(other.m_CachedHierarchyLevels))
   , m_CachedRecomputedFlags(std::move(other.m_CachedRecomputedFlags))
   , m_HierarchyDirty(other.m_HierarchyDirty)
   , m_PhysicsParticipants(std::move(other.m_PhysicsParticipants))
   , m_PhysicsParticipantCacheRevision(other.m_PhysicsParticipantCacheRevision)
   , m_PhysicsParticipantEntityCount(other.m_PhysicsParticipantEntityCount)
   , m_PhysicsParticipantRescanFrames(other.m_PhysicsParticipantRescanFrames)
   , m_PhysicsParticipantCacheDirty(other.m_PhysicsParticipantCacheDirty)
   , m_RuntimeWorldBridgeRevision(other.m_RuntimeWorldBridgeRevision)
   , m_RuntimeWorldBridgeFullSyncPending(other.m_RuntimeWorldBridgeFullSyncPending)
   , m_RuntimeWorldBridgeDirtyMasks(std::move(other.m_RuntimeWorldBridgeDirtyMasks))
   , m_RuntimeWorldBridgeDirtyEntities(std::move(other.m_RuntimeWorldBridgeDirtyEntities))
   , m_RuntimeWorldBridgeRemovedEntities(std::move(other.m_RuntimeWorldBridgeRemovedEntities))
   , m_RuntimeWorldPendingTransformBitset(std::move(other.m_RuntimeWorldPendingTransformBitset))
   , m_RuntimeWorldPendingTransformBitWordCount(other.m_RuntimeWorldPendingTransformBitWordCount)
   , m_RuntimeWorldBridge(std::move(other.m_RuntimeWorldBridge))
{
   other.m_RuntimeWorldPendingTransformBitWordCount = 0;

   // Update all Entity objects to point to this Scene after move
   for (Entity& e : m_EntityList) {
      e.UpdateScenePointer(this);
   }
}

Scene& Scene::operator=(Scene&& other) noexcept {
   if (this != &other) {
      m_Entities = std::move(other.m_Entities);
      m_EntityList = std::move(other.m_EntityList);
      m_NextID = other.m_NextID;
      m_Environment = std::move(other.m_Environment);
      m_PendingRemovals = std::move(other.m_PendingRemovals);
      m_PendingCreations = std::move(other.m_PendingCreations);
      m_IsDirty = other.m_IsDirty;
      m_IsBeingDestroyed = other.m_IsBeingDestroyed;
      m_DefaultShaderPreset = other.m_DefaultShaderPreset;
      m_EditorViewportState = std::move(other.m_EditorViewportState);
      m_HasEditorViewportState = other.m_HasEditorViewportState;
      m_PendingSceneLoad = other.m_PendingSceneLoad;
      m_PendingSceneLoadAsync = other.m_PendingSceneLoadAsync;
      m_PendingSceneLoadPath = std::move(other.m_PendingSceneLoadPath);
      m_Loading = other.m_Loading;
      m_Loaded = other.m_Loaded;
      m_LoadingAsync = other.m_LoadingAsync;
      m_LoadProgress = other.m_LoadProgress;
      m_LoadScriptsTotal = other.m_LoadScriptsTotal;
      m_LoadScriptsProcessed = other.m_LoadScriptsProcessed;
      m_LoadScriptsReady = other.m_LoadScriptsReady;
      m_DeferScriptOnCreate = other.m_DeferScriptOnCreate;
      m_RestoreDeferredScriptInit = other.m_RestoreDeferredScriptInit;
      m_PrevDeferredScriptInit = other.m_PrevDeferredScriptInit;
      m_LoadingScenePath = std::move(other.m_LoadingScenePath);
      m_LoadStartTime = other.m_LoadStartTime;
      m_LoadHoldStartTime = other.m_LoadHoldStartTime;
      m_LoadHoldDuration = other.m_LoadHoldDuration;
      m_LoadHoldStartProgress = other.m_LoadHoldStartProgress;
      m_LoadingHold = other.m_LoadingHold;
      m_AsyncLoadJob = std::move(other.m_AsyncLoadJob);
      m_ScenePath = std::move(other.m_ScenePath);
      m_BodyMap = std::move(other.m_BodyMap);
      m_EditScene = std::move(other.m_EditScene);
      m_RuntimeScene = std::move(other.m_RuntimeScene);
      m_IsPlaying = other.m_IsPlaying;
      m_IsPaused = other.m_IsPaused;
      m_CachedHierarchyLevels = std::move(other.m_CachedHierarchyLevels);
      m_CachedRecomputedFlags = std::move(other.m_CachedRecomputedFlags);
      m_HierarchyDirty = other.m_HierarchyDirty;
      m_PhysicsParticipants = std::move(other.m_PhysicsParticipants);
      m_PhysicsParticipantCacheRevision = other.m_PhysicsParticipantCacheRevision;
      m_PhysicsParticipantEntityCount = other.m_PhysicsParticipantEntityCount;
      m_PhysicsParticipantRescanFrames = other.m_PhysicsParticipantRescanFrames;
      m_PhysicsParticipantCacheDirty = other.m_PhysicsParticipantCacheDirty;
      m_RuntimeWorldBridgeRevision = other.m_RuntimeWorldBridgeRevision;
      m_RuntimeWorldBridgeFullSyncPending = other.m_RuntimeWorldBridgeFullSyncPending;
      m_RuntimeWorldBridgeDirtyMasks = std::move(other.m_RuntimeWorldBridgeDirtyMasks);
      m_RuntimeWorldBridgeDirtyEntities = std::move(other.m_RuntimeWorldBridgeDirtyEntities);
      m_RuntimeWorldBridgeRemovedEntities = std::move(other.m_RuntimeWorldBridgeRemovedEntities);
      m_RuntimeWorldPendingTransformBitset = std::move(other.m_RuntimeWorldPendingTransformBitset);
      m_RuntimeWorldPendingTransformBitWordCount = other.m_RuntimeWorldPendingTransformBitWordCount;
      m_RuntimeWorldBridge = std::move(other.m_RuntimeWorldBridge);

      other.m_RuntimeWorldPendingTransformBitWordCount = 0;
      
      // Update all Entity objects to point to this Scene after move
      for (Entity& e : m_EntityList) {
         e.UpdateScenePointer(this);
      }
   }
   return *this;
}

void Scene::EnsureRuntimeWorldTransformTrackingCapacity(size_t requiredEntityCapacity) {
   if (requiredEntityCapacity == 0) {
      requiredEntityCapacity = 1;
   }

   const size_t requiredWordCount = (requiredEntityCapacity + 63u) / 64u;
   if (m_RuntimeWorldPendingTransformBitset &&
       m_RuntimeWorldBridgeDirtyMasks.size() >= requiredEntityCapacity &&
       m_RuntimeWorldPendingTransformBitWordCount >= requiredWordCount) {
      return;
   }

   const size_t grownCapacity = std::max<size_t>(
      requiredEntityCapacity,
      std::max<size_t>(64u, m_RuntimeWorldPendingTransformBitWordCount * 128u));
   const size_t wordCount = (grownCapacity + 63u) / 64u;

   auto nextBitset = std::make_unique<std::atomic<uint64_t>[]>(wordCount);
   for (size_t i = 0; i < wordCount; ++i) {
      nextBitset[i].store(0u, std::memory_order_relaxed);
   }
   for (size_t i = 0; i < std::min(wordCount, m_RuntimeWorldPendingTransformBitWordCount); ++i) {
      nextBitset[i].store(
         m_RuntimeWorldPendingTransformBitset
            ? m_RuntimeWorldPendingTransformBitset[i].load(std::memory_order_relaxed)
            : 0u,
         std::memory_order_relaxed);
   }

   m_RuntimeWorldPendingTransformBitset = std::move(nextBitset);
   m_RuntimeWorldPendingTransformBitWordCount = wordCount;
   if (m_RuntimeWorldBridgeDirtyMasks.size() < grownCapacity) {
      m_RuntimeWorldBridgeDirtyMasks.resize(grownCapacity, 0u);
   }
   if (m_RuntimeWorldBridgeDirtyEntities.capacity() < grownCapacity) {
      m_RuntimeWorldBridgeDirtyEntities.reserve(grownCapacity);
   }
}

void Scene::QueueRuntimeWorldTransformDirty(EntityID id) {
   if (id == INVALID_ENTITY_ID) {
      return;
   }

   if (!m_RuntimeWorldPendingTransformBitset) {
      return;
   }

   const size_t wordIndex = static_cast<size_t>(id) >> 6u;
   if (wordIndex >= m_RuntimeWorldPendingTransformBitWordCount) {
      return;
   }

   const uint64_t mask = 1ull << (static_cast<size_t>(id) & 63u);
   m_RuntimeWorldPendingTransformBitset[wordIndex].fetch_or(mask, std::memory_order_release);
}

void Scene::ResetRuntimeWorldTransformTracking() {
   for (size_t i = 0; i < m_RuntimeWorldPendingTransformBitWordCount; ++i) {
      m_RuntimeWorldPendingTransformBitset[i].store(0u, std::memory_order_release);
   }
}

void Scene::ResetRuntimeWorldDirtyTracking() {
   for (EntityID id : m_RuntimeWorldBridgeDirtyEntities) {
      if (id != INVALID_ENTITY_ID && static_cast<size_t>(id) < m_RuntimeWorldBridgeDirtyMasks.size()) {
         m_RuntimeWorldBridgeDirtyMasks[id] = 0u;
      }
   }
   m_RuntimeWorldBridgeDirtyEntities.clear();
}

cm::world::RuntimeWorld& Scene::EnsureRuntimeWorld() {
   EnsureRuntimeWorldTransformTrackingCapacity(static_cast<size_t>(m_NextID));
   if (!m_RuntimeWorldBridge) {
      m_RuntimeWorldBridge = std::make_unique<cm::world::RuntimeWorld>();
   }
   return *m_RuntimeWorldBridge;
}

void Scene::MarkRuntimeWorldDirty(EntityID id, cm::world::RuntimeDirtyBits bits) {
   if (id == INVALID_ENTITY_ID) {
      return;
   }

   EnsureRuntimeWorldTransformTrackingCapacity(static_cast<size_t>(id) + 1u);
   uint32_t& dirtyMask = m_RuntimeWorldBridgeDirtyMasks[id];
   if (dirtyMask == 0u) {
      m_RuntimeWorldBridgeDirtyEntities.push_back(id);
   }
   dirtyMask |= static_cast<uint32_t>(bits);
   ++m_RuntimeWorldBridgeRevision;
}

void Scene::NotifyTransformChanged(EntityID id) {
   if (id == INVALID_ENTITY_ID || !GetEntityData(id)) {
      return;
   }

   m_IsDirty = true;
   ++m_DirtyRevision;
   MarkTransformDirty(id);
}

void Scene::NotifyComponentChanged(EntityID id, cm::world::RuntimeDirtyBits bits) {
   if (id == INVALID_ENTITY_ID ||
       bits == cm::world::RuntimeDirtyBits::None ||
       !GetEntityData(id)) {
      return;
   }

   m_IsDirty = true;
   ++m_DirtyRevision;
   if (cm::world::HasAny(bits, cm::world::RuntimeDirtyBits::Metadata)) {
      m_PhysicsParticipantCacheDirty = true;
      m_BoneAttachmentEntitiesDirty = true;
   }
   MarkRuntimeWorldDirty(id, bits);
}

void Scene::MarkEntityStructureDirty(EntityID id) {
   MarkDirty();
   if (id != INVALID_ENTITY_ID) {
      MarkRuntimeWorldDirty(id, cm::world::RuntimeDirtyBits::All);
   }
}

void Scene::MarkRuntimeWorldDirtySubtree(EntityID id, cm::world::RuntimeDirtyBits bits) {
   auto* data = GetEntityData(id);
   if (!data) {
      return;
   }

   MarkRuntimeWorldDirty(id, bits);
   for (EntityID child : data->Children) {
      MarkRuntimeWorldDirtySubtree(child, bits);
   }
}

void Scene::MarkRuntimeWorldRemoved(EntityID id) {
   if (id == INVALID_ENTITY_ID) {
      return;
   }

   if (static_cast<size_t>(id) < m_RuntimeWorldBridgeDirtyMasks.size()) {
      m_RuntimeWorldBridgeDirtyMasks[id] = 0u;
   }
   m_RuntimeWorldBridgeRemovedEntities.push_back(id);
   ++m_RuntimeWorldBridgeRevision;
}

void Scene::MarkBoneAttachmentCacheDirty() {
   m_BoneAttachmentEntitiesDirty = true;
}

void Scene::NotifyAnimationPosePaletteChanged() {
   m_AnimationPosePaletteDirtyForAttachments = true;
}

const std::vector<EntityID>& Scene::GetBoneAttachmentEntities() {
   if (!m_BoneAttachmentEntitiesDirty) {
      return m_BoneAttachmentEntities;
   }

   m_BoneAttachmentEntities.clear();
   m_BoneAttachmentEntities.reserve(32);
   for (const auto& [id, data] : m_Entities) {
      if (data.BoneAttachment) {
         m_BoneAttachmentEntities.push_back(id);
      }
   }

   m_BoneAttachmentEntitiesDirty = false;
   return m_BoneAttachmentEntities;
}

void Scene::SyncRuntimeWorld(bool forceFullRebuild) {
   if (!forceFullRebuild && m_RuntimeWorldBridge && !HasPendingRuntimeWorldSyncWork()) {
      return;
   }

   Profiler::Get().AddCounter("Scene/RuntimeWorldSyncFlushes", 1);
   auto& runtimeWorld = EnsureRuntimeWorld();
   runtimeWorld.SyncFromScene(*this,
      forceFullRebuild ? cm::world::RuntimeSyncReason::FullRebuild
                       : cm::world::RuntimeSyncReason::SceneUpdate,
      forceFullRebuild);
}

bool Scene::HasPendingRuntimeWorldStructuralSyncWork() const {
   if (m_RuntimeWorldBridgeFullSyncPending ||
       !m_RuntimeWorldBridgeRemovedEntities.empty()) {
      return true;
   }

   const cm::world::RuntimeDirtyBits worldOverrideOnlyMask =
      cm::world::RuntimeDirtyBits::TransformWorld |
      cm::world::RuntimeDirtyBits::Bounds;

   for (EntityID sceneEntity : m_RuntimeWorldBridgeDirtyEntities) {
      if (sceneEntity == INVALID_ENTITY_ID ||
          static_cast<size_t>(sceneEntity) >= m_RuntimeWorldBridgeDirtyMasks.size()) {
         continue;
      }

      const cm::world::RuntimeDirtyBits bits =
         static_cast<cm::world::RuntimeDirtyBits>(m_RuntimeWorldBridgeDirtyMasks[sceneEntity]);
      if (bits == cm::world::RuntimeDirtyBits::None) {
         continue;
      }

      if ((bits & ~worldOverrideOnlyMask) != cm::world::RuntimeDirtyBits::None) {
         return true;
      }
   }

   return false;
}

bool Scene::HasPendingRuntimeWorldSyncWork() const {
   if (HasPendingRuntimeWorldStructuralSyncWork()) {
      return true;
   }

   return HasPendingTransformUpdates();
}

const cm::world::RuntimeRenderWorld& Scene::BuildRuntimeRenderWorld(uint32_t activeLayerMask, bool enforceLayerMask) {
   auto& runtimeWorld = EnsureRuntimeWorld();
   runtimeWorld.BuildRenderWorld(*this, activeLayerMask, enforceLayerMask);
   return runtimeWorld.GetRenderWorld();
}

void Scene::FinalizeRuntimeTransforms() {
   if (m_RuntimeWorldBridge) {
      m_RuntimeWorldBridge->FinalizeTransformStage(*this);
   }
}

void Scene::NotifyWorldTransformOverride(EntityID id) {
   const cm::world::RuntimeDirtyBits overrideBits =
      cm::world::RuntimeDirtyBits::TransformWorld |
      cm::world::RuntimeDirtyBits::Bounds;

   bool mirroredOverride = false;
   if (m_RuntimeWorldBridge) {
      const cm::world::RuntimeEntityHandle handle = m_RuntimeWorldBridge->TryGetHandle(id);
      if (handle.IsValid()) {
         if (EntityData* data = GetEntityData(id)) {
            m_RuntimeWorldBridge->ApplyWorldTransformOverride(id, data->Transform.WorldMatrix);
            mirroredOverride = true;
         }
      }
   }

   if (!mirroredOverride) {
      const cm::world::RuntimeDirtyBits queuedBits =
         m_RuntimeWorldBridge
            ? (overrideBits | cm::world::RuntimeDirtyBits::Metadata)
            : overrideBits;
      MarkRuntimeWorldDirty(id, queuedBits);
   }
}

bool Scene::HasPendingTransformUpdates() const {
   if (m_RuntimeWorldPendingTransformBitset) {
      for (size_t i = 0; i < m_RuntimeWorldPendingTransformBitWordCount; ++i) {
         if (m_RuntimeWorldPendingTransformBitset[i].load(std::memory_order_acquire) != 0u) {
            return true;
         }
      }
   }

   for (const auto& [id, data] : m_Entities) {
      (void)id;
      if (data.Transform.TransformDirty) {
         return true;
      }
   }
   return false;
}

void Scene::SyncCameraComponentsFromTransforms() {
   const Environment& env = GetEnvironment();
   const float rw = env.HasFixedRenderResolution()
      ? static_cast<float>(env.RenderResolutionWidth)
      : static_cast<float>(Renderer::Get().GetWidth());
   float rh = env.HasFixedRenderResolution()
      ? static_cast<float>(env.RenderResolutionHeight)
      : static_cast<float>(Renderer::Get().GetHeight());
   if (rh <= 0.0f) {
      rh = 1.0f;
   }
   const float aspectRatio = (rw > 0.0f) ? (rw / rh) : 1.0f;

   if (m_RuntimeWorldBridge) {
      for (EntityID id : m_RuntimeWorldBridge->GetCameraSceneEntities()) {
         auto* data = GetEntityData(id);
         if (!data || !data->Camera) {
            continue;
         }
         data->Camera->SyncWithTransform(data->Transform);
         data->Camera->UpdateProjection(aspectRatio);
      }
      return;
   }

   for (auto& [id, data] : m_Entities) {
      (void)id;
      if (!data.Camera) {
         continue;
      }
      data.Camera->SyncWithTransform(data.Transform);
      data.Camera->UpdateProjection(aspectRatio);
   }
}

namespace {
static std::string MakeUniqueName(const std::string& desired,
   const std::function<bool(const std::string&)>& exists) {
   if (!exists(desired)) return desired;
   std::string base = desired;
   const size_t underscore = base.find_last_of('_');
   if (underscore != std::string::npos && underscore + 1 < base.size()) {
      bool allDigits = true;
      for (size_t i = underscore + 1; i < base.size(); ++i) {
         if (!std::isdigit(static_cast<unsigned char>(base[i]))) {
            allDigits = false;
            break;
         }
      }
      if (allDigits) {
         base = base.substr(0, underscore);
      }
   }
   if (base.empty()) {
      base = desired.empty() ? std::string("Entity") : desired;
   }
   int suffix = 1;
   std::string candidate;
   do {
      candidate = base + "_" + std::to_string(suffix++);
   } while (exists(candidate));
   return candidate;
}
} // namespace

///-----------------------------------------------------------------------------------------
/// Create Entity: Spawn a new entity in the scene
///-----------------------------------------------------------------------------------------
Entity Scene::CreateEntity(const std::string& name) {
   EntityID id = m_NextID++;
   EnsureRuntimeWorldTransformTrackingCapacity(static_cast<size_t>(m_NextID));
   EntityData data;

   auto hasRootName = [&](const std::string& candidate) -> bool {
      return std::any_of(m_Entities.begin(), m_Entities.end(), [&](const auto& pair) {
         return pair.second.Parent == INVALID_ENTITY_ID && pair.second.Name == candidate;
      });
   };
   data.Name = MakeUniqueName(name, hasRootName);

   m_Entities.emplace(id, std::move(data));

   Entity entity(id, this);
   m_EntityList.push_back(entity);

   // Editor: mark scene dirty on structural change
   MarkDirty();
   MarkBoneAttachmentCacheDirty();
   
   // PERF: Invalidate hierarchy cache since we added a new root entity
   InvalidateHierarchyCache();
   MarkRuntimeWorldDirty(id, cm::world::RuntimeDirtyBits::All);

   return entity;
}


///-----------------------------------------------------------------------------------------
/// Create Entity Exact: Create an entity with an exact name, used for deserialization
///-----------------------------------------------------------------------------------------
Entity Scene::CreateEntityExact(const std::string& name) {
   EntityID id = m_NextID++;
   EnsureRuntimeWorldTransformTrackingCapacity(static_cast<size_t>(m_NextID));
   EntityData data;
   data.Name = name;

   m_Entities.emplace(id, std::move(data));

   Entity entity(id, this);
   m_EntityList.push_back(entity);

   // Editor: mark scene dirty on structural change
   MarkDirty();
   MarkBoneAttachmentCacheDirty();
   
   // PERF: Invalidate hierarchy cache since we added a new root entity
   InvalidateHierarchyCache();
   MarkRuntimeWorldDirty(id, cm::world::RuntimeDirtyBits::All);

   return entity;
}

Entity Scene::CreateEntityExactFast(const std::string& name) {
   EntityID id = m_NextID++;
   EnsureRuntimeWorldTransformTrackingCapacity(static_cast<size_t>(m_NextID));
   EntityData data;
   data.Name = name;

   m_Entities.emplace(id, std::move(data));

   Entity entity(id, this);
   m_EntityList.push_back(entity);

   // No dirty/hierarchy invalidation here; caller batches updates.
   MarkBoneAttachmentCacheDirty();
   MarkRuntimeWorldDirty(id, cm::world::RuntimeDirtyBits::All);
   return entity;
}


///-----------------------------------------------------------------------------------------
/// Remove Entity: Remove an entity from the scene by ID.
///-----------------------------------------------------------------------------------------
void Scene::RemoveEntity(EntityID id) {
   auto* data = GetEntityData(id);

   // Ensure Data exists
   if (!data)
   {
      return;
   }

   MarkRuntimeWorldRemoved(id);

   // 0. Clean up river component on parent terrain if this is a river mesh entity
   if (data->Parent != INVALID_ENTITY_ID) {
      auto* parentData = GetEntityData(data->Parent);
      if (parentData && parentData->River && parentData->River->MeshEntity == id)
      {
         // This is a river mesh entity - clean up the River component from the terrain
         // Note: heightmap changes remain baked into terrain, we only clean up serialization data
         
         // Optionally delete the mesh asset file
         if (!parentData->River->MeshAssetPath.empty())
         {
            try {
               namespace fs = std::filesystem;
               fs::path assetPath(parentData->River->MeshAssetPath);
               if (assetPath.is_relative())
               {
                  const fs::path& projectRoot = FileSystem::Instance().GetProjectRoot();
                  if (!projectRoot.empty())
                  {
                     assetPath = projectRoot / assetPath;
                  }
               }
               if (fs::exists(assetPath))
               {
                  std::error_code ec;
                  fs::remove(assetPath, ec);
                  if (!ec)
                  {
                     std::cout << "[River] Deleted river mesh asset: " << assetPath.string() << std::endl;
                  }
               }
            } catch (...) {
               // Ignore file deletion errors
            }
         }
         
         // Clear the River component from the terrain
         parentData->River.reset();
         std::cout << "[River] Cleared River component from terrain: " << parentData->Name << std::endl;
      }
   }

   // 1. Clean up parent-child relationships
   if (data->Parent != INVALID_ENTITY_ID) {
      auto* parentData = GetEntityData(data->Parent);
      if (parentData)
      {
         parentData->Children.erase(
            std::remove(parentData->Children.begin(),
               parentData->Children.end(),
               id),
            parentData->Children.end()
         );
      }
   }

   // 2. Clean up children (recursively remove all children)
   // Make a copy to avoid issues with iterating while removing
   std::vector<EntityID> childrenToRemove = data->Children;
   for (EntityID childID : childrenToRemove) {
      RemoveEntity(childID);
   }

   // 3. Clean up physics body
   DestroyPhysicsBody(id);

   // 4. Clean up allocated components (unique_ptr handles deletion)
   // NOTE: Do NOT destroy texture handles in property blocks!
   // Textures are shared via the global texture cache (s_textureCache in MaterialCache.cpp).
   // Destroying them here would invalidate cached handles, causing crashes when other
   // entities/materials try to use the same textures. Just clear the maps.
   auto releasePBBlock = [](MaterialPropertyBlock& block)
   {
      block.Clear();  // Clear all maps (both string and ID-based) - don't destroy textures
   };
   if (data->MeshProxy) {
      EntityID target = data->MeshProxy->TargetMesh;
      auto* targetData = GetEntityData(target);
      if (targetData && targetData->Mesh && targetData->Mesh->mesh) {
         for (uint32_t slot : data->MeshProxy->SubmeshSlots) {
            if (slot < targetData->Mesh->SubmeshOwners.size()) {
               targetData->Mesh->SubmeshOwners[slot] = INVALID_ENTITY_ID;
            }
         }
      }
      releasePBBlock(data->MeshProxy->PropertyBlock);
      for (auto& pb : data->MeshProxy->SlotPropertyBlocks) releasePBBlock(pb);
      data->MeshProxy.reset();
   }
   if (data->Softbody) {
      SoftbodySystem::ReleaseRuntime(*data, false);
      data->Softbody.reset();
   }
   if (data->Mesh) {
      releasePBBlock(data->Mesh->PropertyBlock);
      for (auto& pb : data->Mesh->SlotPropertyBlocks) releasePBBlock(pb);
      data->Mesh->mesh = nullptr;
      data->Mesh->material.reset();
      data->Mesh->BlendShapes = nullptr;
      data->Mesh.reset();
   }
   if (data->Light) {
      data->Light.reset();
   }
   if (data->Collider) {
      data->Collider.reset();
   }
   if (data->Camera) {
      data->Camera.reset();
   }
   if (data->RigidBody) {
      data->RigidBody.reset();
   }
   if (data->StaticBody) {
      data->StaticBody.reset();
   }
   if (data->Emitter) {
      if (!data->Emitter->SpritePath.empty() && ps::isValid(data->Emitter->SpriteHandle)) {
         particles::ReleaseSprite(data->Emitter->SpriteHandle);
         data->Emitter->SpriteHandle = { uint16_t{UINT16_MAX} };
         data->Emitter->Uniforms.m_handle = { uint16_t{UINT16_MAX} };
      }
      if (ps::isValid(data->Emitter->Handle)) {
         // Unregister from particle emitter system before destroying
         ecs::ParticleEmitterSystem::Get().UnregisterEmitterOwnership(data->Emitter->Handle);
         ps::destroyEmitter(data->Emitter->Handle);
         data->Emitter->Handle = { uint16_t{UINT16_MAX} };
      }
      data->Emitter->Uniforms.reset();
      data->Emitter->Enabled = false;
      data->Emitter.reset();
   }
   if (data->BlendShapes) {
      data->BlendShapes.reset();
   }
   if (data->UnifiedMorph) {
      data->UnifiedMorph.reset();
   }
   if (data->Skeleton) {
      data->Skeleton.reset();
   }
   if (data->Skinning) {
      data->Skinning.reset();
   }
   
   // Clean up River component and its mesh asset if this entity has one
   if (data->River) {
      // Delete the mesh asset file if it exists
      if (!data->River->MeshAssetPath.empty())
      {
         try {
            namespace fs = std::filesystem;
            fs::path assetPath(data->River->MeshAssetPath);
            if (assetPath.is_relative())
            {
               const fs::path& projectRoot = FileSystem::Instance().GetProjectRoot();
               if (!projectRoot.empty())
               {
                  assetPath = projectRoot / assetPath;
               }
            }
            if (fs::exists(assetPath))
            {
               std::error_code ec;
               fs::remove(assetPath, ec);
               if (!ec)
               {
                  std::cout << "[River] Deleted river mesh asset on terrain removal: " << assetPath.string() << std::endl;
               }
            }
         } catch (...) {
            // Ignore file deletion errors
         }
      }
      data->River.reset();
   }

   // 5. Clean up scripts
   for (auto& script : data->Scripts) {
      // Note: Script_Destroy is not exposed to C++, so we just clear the vector
      // The shared_ptr will handle cleanup of native script instances
      // For managed scripts, the GCHandle cleanup happens in C# when the script is destroyed
   }
   data->Scripts.clear();

   // 6. Remove from entity collections (erase from list first to avoid iterator use during render)
   m_EntityList.erase(
      std::remove_if(m_EntityList.begin(), m_EntityList.end(),
         [&](const Entity& e) { return e.GetID() == id; }),
      m_EntityList.end());
   m_Entities.erase(id);

   // Editor: mark scene dirty on structural change
   MarkDirty();
   MarkBoneAttachmentCacheDirty();
   
   // PERF: Invalidate hierarchy cache since entity was removed
   InvalidateHierarchyCache();

   const std::string msg = "[Scene] Removed entity " + std::to_string(id);
#ifndef CLAYMORE_RUNTIME
   Logger::Log(msg);
#else
   std::cout << msg << std::endl;
#endif
}


///-----------------------------------------------------------------------------------------
/// GetEntityData: Get the data for an entity in the scene by ID.
///-----------------------------------------------------------------------------------------
EntityData* Scene::GetEntityData(EntityID id) {
   auto it = m_Entities.find(id);
   return (it != m_Entities.end()) ? &it->second : nullptr;
}


///-----------------------------------------------------------------------------------------
/// QueueRemoveEntity: Enqueue the removal of an entity from the scene by ID.
///-----------------------------------------------------------------------------------------
void Scene::QueueRemoveEntity(EntityID id) {
   // Allow duplicates; we'll dedupe when processing
   m_PendingRemovals.push_back(id);
}


///-----------------------------------------------------------------------------------------
/// ProcessPendingRemovals: At appropriate frame time, remove enqueued entities
///-----------------------------------------------------------------------------------------
void Scene::ProcessPendingRemovals() {

   if (m_PendingRemovals.empty())
   {
      return;
   }

   // Deduplicate while preserving order of first occurrence
   std::unordered_set<EntityID> seen;
   std::vector<EntityID> unique;

   unique.reserve(m_PendingRemovals.size());
   for (EntityID id : m_PendingRemovals) {
      if (seen.insert(id).second)
      {
         unique.push_back(id);
      }
   }

   // Actual removal of entities
   for (EntityID id : unique)
   {
      RemoveEntity(id);
   }

   m_PendingRemovals.clear();
}


///-----------------------------------------------------------------------------------------
/// QueueCreateEntity: Create entity data immediately but defer list addition.
/// EntityData is added to m_Entities immediately so components can be added right away.
/// List addition to m_EntityList is deferred to avoid vector iterator invalidation.
/// Returns the ID that is usable immediately for component operations.
///-----------------------------------------------------------------------------------------
EntityID Scene::QueueCreateEntity(const std::string& name, EntityID parentID) {
    // Allocate ID and create EntityData immediately so components can be added
    EntityID id = m_NextID++;
    EnsureRuntimeWorldTransformTrackingCapacity(static_cast<size_t>(m_NextID));
    
    EntityData data;
    data.Name = name.empty() ? std::string("Entity") : name;
    if (parentID == INVALID_ENTITY_ID) {
        auto hasRootName = [&](const std::string& check) -> bool {
            for (const auto& pair : m_Entities) {
                if (pair.first == id) continue;
                if (pair.second.Parent == INVALID_ENTITY_ID && pair.second.Name == check) return true;
            }
            return false;
        };
        if (hasRootName(data.Name)) {
            const std::string baseName = data.Name;
            std::string candidate = baseName + "_" + std::to_string(id);
            int suffix = 1;
            while (hasRootName(candidate)) {
                candidate = baseName + "_" + std::to_string(id) + "_" + std::to_string(suffix++);
            }
            data.Name = candidate;
        }
    }
    m_Entities.emplace(id, std::move(data));
    
    // Queue the list addition and parenting for later (to avoid list iterator invalidation)
    m_PendingCreations.push_back({id, m_Entities[id].Name, parentID});
    return id;
}


///-----------------------------------------------------------------------------------------
/// ProcessPendingCreations: At appropriate frame time, finalize enqueued entities
///-----------------------------------------------------------------------------------------
void Scene::ProcessPendingCreations() {
    if (m_PendingCreations.empty()) {
        return;
    }
    
    for (const auto& pending : m_PendingCreations) {
        // EntityData already exists (created in QueueCreateEntity)
        // Just add to the entity list now
        auto it = m_Entities.find(pending.id);
        if (it == m_Entities.end()) {
            continue; // Entity was removed before finalization
        }
        
        // Add to the entity list
        Entity entity(pending.id, this);
        m_EntityList.push_back(entity);
        MarkRuntimeWorldDirty(pending.id, cm::world::RuntimeDirtyBits::All);
        
        // Set parent if specified
        if (pending.parentID != INVALID_ENTITY_ID) {
            SetParent(pending.id, pending.parentID, false);
        }
    }
    
    if (!m_PendingCreations.empty()) {
      InvalidateHierarchyCache();
   }
    
    m_PendingCreations.clear();
}


///-----------------------------------------------------------------------------------------
/// QueueRemoveModelChild: Remove entity and track it as a deleted model node if applicable.
/// Returns true if the entity was tracked as a deleted model child.
///-----------------------------------------------------------------------------------------
bool Scene::QueueRemoveModelChild(EntityID id) {
    auto* data = GetEntityData(id);
    if (!data) {
        QueueRemoveEntity(id);
        return false;
    }
    
    // Walk up the hierarchy to find a model root (entity with ModelAssetGuid)
    EntityID modelRoot = INVALID_ENTITY_ID;
    EntityID current = data->Parent;
    std::vector<std::string> pathParts;
    pathParts.push_back(data->Name);  // Start with the deleted entity's name
    
    while (current != INVALID_ENTITY_ID) {
        auto* parentData = GetEntityData(current);
        if (!parentData) break;
        
        // Check if this is a model root
        if (parentData->ModelAssetGuid.high != 0 || parentData->ModelAssetGuid.low != 0) {
            modelRoot = current;
            break;
        }
        
        pathParts.push_back(parentData->Name);
        current = parentData->Parent;
    }
    
    if (modelRoot != INVALID_ENTITY_ID) {
        // Build relative path from model root to the deleted entity
        std::reverse(pathParts.begin(), pathParts.end());
        
        std::string relativePath;
        for (size_t i = 0; i < pathParts.size(); ++i) {
            if (i > 0) relativePath += "/";
            relativePath += pathParts[i];
        }
        
        // Add to model root's DeletedModelNodes if not already present
        auto* rootData = GetEntityData(modelRoot);
        if (rootData && !relativePath.empty()) {
            bool alreadyDeleted = std::find(rootData->DeletedModelNodes.begin(), 
                                            rootData->DeletedModelNodes.end(), 
                                            relativePath) != rootData->DeletedModelNodes.end();
            if (!alreadyDeleted) {
                rootData->DeletedModelNodes.push_back(relativePath);
                std::cout << "[Scene] Tracked model node deletion: " << relativePath << std::endl;
            }
        }
        
        QueueRemoveEntity(id);
        return true;
    }
    
    // Not a model child, just queue for removal
    QueueRemoveEntity(id);
    return false;
}


#if !defined(CLAYMORE_RUNTIME)
///-----------------------------------------------------------------------------------------
/// ResetModelChildrenToDefault: Reset all children of a model root to model-default
/// transforms while preserving scene-added colliders and refreshing them against
/// the new transforms. Keeps the model root's transform unchanged.
/// Uses latest import from cache.
///-----------------------------------------------------------------------------------------
bool Scene::ResetModelChildrenToDefault(EntityID modelRootId) {
    auto* rootData = GetEntityData(modelRootId);
    if (!rootData) return false;
    if (rootData->ModelAssetGuid.high == 0 && rootData->ModelAssetGuid.low == 0) return false;

    std::string modelPath = AssetLibrary::Instance().GetPathForGUID(rootData->ModelAssetGuid);
    if (modelPath.empty()) {
        std::cerr << "[Scene] ResetModelChildrenToDefault: No path for model GUID" << std::endl;
        return false;
    }

    // Resolve to absolute and handle .meta path (GetPathForGUID often returns .meta)
    auto resolveToAbsolute = [](const std::string& path) -> std::string {
        if (path.empty()) return {};
        std::filesystem::path p(path);
        if (p.is_absolute()) return p.string();
        try {
            std::filesystem::path base = Project::GetProjectDirectory();
            if (!base.empty()) return (base / p).string();
        } catch (...) {}
        return p.string();
    };

    std::string cacheKey = modelPath;
    std::string ext;
    {
        std::filesystem::path p(modelPath);
        ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    }
    if (ext == ".meta") {
        // HasModelCache expects source path (.fbx/.gltf); read meta for "source" via VFS
        std::string metaContent;
        if (FileSystem::Instance().ReadTextFile(modelPath, metaContent)) {
            try {
                json meta = json::parse(metaContent);
                std::string source = meta.value("source", std::string());
                if (!source.empty()) cacheKey = resolveToAbsolute(source);
            } catch (...) {}
        }
        if (cacheKey == modelPath) cacheKey = resolveToAbsolute(modelPath);  // fallback
    } else {
        cacheKey = resolveToAbsolute(modelPath);
    }

    BuiltModelPaths built{};
    if (!HasModelCache(cacheKey, built)) {
        std::cerr << "[Scene] ResetModelChildrenToDefault: No cache for " << modelPath << std::endl;
        return false;
    }

    // Instantiate a fresh model to get default transforms (at origin, will remove after)
    EntityID tempRoot = InstantiateModelFast(built.metaPath, glm::vec3(0.0f), true);
    if (tempRoot == INVALID_ENTITY_ID || tempRoot == (EntityID)0) {
        std::cerr << "[Scene] ResetModelChildrenToDefault: Failed to instantiate model" << std::endl;
        return false;
    }

    struct ModelChildDefaultState {
        TransformComponent Transform;
        bool HasCollider = false;
        ColliderComponent Collider;
    };

    auto copyColliderAuthoring = [](const ColliderComponent& src, ColliderComponent& dst) {
        dst.ShapeType = src.ShapeType;
        dst.Offset = src.Offset;
        dst.Size = src.Size;
        dst.Radius = src.Radius;
        dst.Height = src.Height;
        dst.MeshPath = src.MeshPath;
        dst.IsTrigger = src.IsTrigger;
        dst.PhysicsLayer = src.PhysicsLayer;
        dst.PhysicsLayerName = src.PhysicsLayerName;
        dst.Shape = nullptr;
        dst._RuntimeOwnedByRigidBody = false;
        dst._LastShapeType = ColliderShape::Box;
        dst._LastSize = glm::vec3(-1.0f);
        dst._LastRadius = -1.0f;
        dst._LastHeight = -1.0f;
        dst._LastOffset = glm::vec3(-9999.0f);
        dst._LastWorldScale = glm::vec3(-1.0f);
    };

    // Build path -> default child state map from the fresh instance (children only, not root)
    std::unordered_map<std::string, ModelChildDefaultState> pathToDefault;
    std::function<void(EntityID, const std::string&)> collectDefaults = [&](EntityID id, const std::string& parentPath) {
        auto* d = GetEntityData(id);
        if (!d) return;
        std::string path = parentPath.empty() ? d->Name : (parentPath + "/" + d->Name);
        ModelChildDefaultState state;
        state.Transform = d->Transform;
        if (d->Collider) {
            state.HasCollider = true;
            copyColliderAuthoring(*d->Collider, state.Collider);
        }
        pathToDefault[path] = state;
        pathToDefault[ModelNodeIdentity::NormalizePath(path)] = state;
        for (EntityID c : d->Children) collectDefaults(c, path);
    };
    for (EntityID c : GetEntityData(tempRoot)->Children) {
        collectDefaults(c, "");
    }

    // Remove the temp instance
    RemoveEntity(tempRoot);

    // Build path from model root to node (same logic as HotSwap)
    auto relPathOf = [this](EntityID root, EntityID node) -> std::string {
        std::vector<std::string> parts;
        EntityID cur = node;
        while (cur != INVALID_ENTITY_ID) {
            auto* d = GetEntityData(cur);
            if (!d) break;
            parts.push_back(d->Name);
            if (cur == root) break;
            cur = d->Parent;
        }
        if (parts.empty()) return std::string();
        std::reverse(parts.begin(), parts.end());
        if (!parts.empty()) parts.erase(parts.begin());
        std::string s;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i > 0) s += "/";
            s += parts[i];
        }
        return s;
    };

    // Apply default transforms and refresh colliders on all children (skip nested model roots)
    int transformApplied = 0;
    int colliderApplied = 0;
    std::vector<EntityID> colliderRefreshIds;
    std::function<void(EntityID)> applyToChildren = [&](EntityID id) {
        auto* d = GetEntityData(id);
        if (!d) return;
        // Skip nested model roots - they have their own model identity
        if (id != modelRootId && d->ModelAssetGuid.high != 0 && d->ModelAssetGuid.low != 0) return;
        for (EntityID c : d->Children) {
            auto* cd = GetEntityData(c);
            if (!cd) continue;
            if (cd->ModelAssetGuid.high != 0 || cd->ModelAssetGuid.low != 0) {
                applyToChildren(c);  // Recurse into nested model but don't reset it
                continue;
            }
            std::string path = relPathOf(modelRootId, c);
            if (path.empty()) continue;
            auto it = pathToDefault.find(path);
            if (it == pathToDefault.end()) {
                it = pathToDefault.find(ModelNodeIdentity::NormalizePath(path));
            }
            if (it != pathToDefault.end()) {
                cd->Transform = it->second.Transform;
                MarkTransformDirty(c);
                ++transformApplied;

                bool colliderChanged = false;
                if (it->second.HasCollider) {
                    if (!cd->Collider) {
                        cd->Collider = std::make_unique<ColliderComponent>();
                    }
                    copyColliderAuthoring(it->second.Collider, *cd->Collider);
                    colliderChanged = true;
                } else {
                    if (!cd->Collider) {
                        if (cd->RigidBody) {
                            EnsureCollider(cd->RigidBody.get(), cd);
                            colliderChanged = true;
                        } else if (cd->StaticBody) {
                            EnsureCollider(cd->StaticBody.get(), cd);
                            colliderChanged = true;
                        }
                    }

                    // Preserve scene-added box colliders, but re-fit them to the current mesh
                    // bounds so reset fixes import-scale changes instead of keeping stale sizes.
                    if (cd->Collider && cd->Collider->ShapeType == ColliderShape::Box) {
                        ApplyMeshBoundsToBoxCollider(*cd->Collider, *cd);
                        colliderChanged = true;
                    }
                }

                if (cd->Collider) {
                    colliderRefreshIds.push_back(c);
                }
                if (colliderChanged) {
                    ++colliderApplied;
                }
            }
            applyToChildren(c);
        }
    };
    applyToChildren(modelRootId);

    if (!colliderRefreshIds.empty()) {
        UpdateTransforms();

        if (m_IsPlaying) {
            for (EntityID entityId : colliderRefreshIds) {
                DestroyPhysicsBody(entityId);

                auto* data = GetEntityData(entityId);
                if (!data || !data->Collider || data->CharacterController) {
                    continue;
                }

                glm::vec3 wscale(1.0f);
                glm::vec3 wpos, wskew;
                glm::vec4 wpersp;
                glm::quat wrot;
                glm::decompose(data->Transform.WorldMatrix, wscale, wrot, wpos, wskew, wpersp);
                data->Collider->BuildShape(
                    data->Mesh && data->Mesh->mesh ? data->Mesh->mesh.get() : nullptr,
                    glm::abs(wscale));
                if (data->Collider->Shape) {
                    CreatePhysicsBody(entityId, data->Transform, *data->Collider);
                }
            }
        }
    }

    if (transformApplied > 0 || colliderApplied > 0) {
        MarkDirty();
    }
    std::cout << "[Scene] Reset " << transformApplied
              << " model child transform(s) and " << colliderApplied
              << " collider reset/refit(s)" << std::endl;
    return transformApplied > 0 || colliderApplied > 0;
}

/// ResetModelChildrenMaterialsToDefault: Reset all children of a model root to the
/// material/parameter/texture state defined by the model's import settings. The
/// authoritative defaults are taken from a fresh instantiation of the model .meta,
/// which already applies per-material import overrides -- so overridden materials
/// get their override while the rest populate like a normal import.
bool Scene::ResetModelChildrenMaterialsToDefault(EntityID modelRootId) {
    auto* rootData = GetEntityData(modelRootId);
    if (!rootData) return false;
    if (rootData->ModelAssetGuid.high == 0 && rootData->ModelAssetGuid.low == 0) return false;

    std::string modelPath = AssetLibrary::Instance().GetPathForGUID(rootData->ModelAssetGuid);
    if (modelPath.empty()) {
        std::cerr << "[Scene] ResetModelChildrenMaterialsToDefault: No path for model GUID" << std::endl;
        return false;
    }

    // Resolve to absolute and handle .meta path (GetPathForGUID often returns .meta)
    auto resolveToAbsolute = [](const std::string& path) -> std::string {
        if (path.empty()) return {};
        std::filesystem::path p(path);
        if (p.is_absolute()) return p.string();
        try {
            std::filesystem::path base = Project::GetProjectDirectory();
            if (!base.empty()) return (base / p).string();
        } catch (...) {}
        return p.string();
    };

    std::string cacheKey = modelPath;
    std::string ext;
    {
        std::filesystem::path p(modelPath);
        ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    }
    if (ext == ".meta") {
        std::string metaContent;
        if (FileSystem::Instance().ReadTextFile(modelPath, metaContent)) {
            try {
                json meta = json::parse(metaContent);
                std::string source = meta.value("source", std::string());
                if (!source.empty()) cacheKey = resolveToAbsolute(source);
            } catch (...) {}
        }
        if (cacheKey == modelPath) cacheKey = resolveToAbsolute(modelPath);
    } else {
        cacheKey = resolveToAbsolute(modelPath);
    }

    BuiltModelPaths built{};
    if (!HasModelCache(cacheKey, built)) {
        std::cerr << "[Scene] ResetModelChildrenMaterialsToDefault: No cache for " << modelPath << std::endl;
        return false;
    }

    // Instantiate a fresh model. Its material state is exactly the import result,
    // including any per-material import overrides.
    EntityID tempRoot = InstantiateModelFast(built.metaPath, glm::vec3(0.0f), true);
    if (tempRoot == INVALID_ENTITY_ID || tempRoot == (EntityID)0) {
        std::cerr << "[Scene] ResetModelChildrenMaterialsToDefault: Failed to instantiate model" << std::endl;
        return false;
    }

    struct ModelChildMaterialState {
        bool HasMesh = false;
        std::shared_ptr<Material> material;
        std::vector<std::shared_ptr<Material>> materials;
        std::vector<std::string> MaterialSlotNames;
        std::vector<std::string> MaterialAssetPaths;
        std::vector<bool> OwnedMaterialSlots;
        std::vector<binary::InlineMaterialData> InlineMaterials;
        std::vector<binary::ShaderGraphMaterialData> ShaderGraphMaterials;
        std::vector<MaterialPropertyBlock> SlotPropertyBlocks;
        std::vector<MaterialSource> MaterialSources;
        MaterialPropertyBlock PropertyBlock;
        std::unordered_map<std::string, std::string> PropertyBlockTexturePaths;
        std::vector<std::unordered_map<std::string, std::string>> SlotPropertyBlockTexturePaths;
    };

    auto snapshotMaterials = [](const MeshComponent& src, ModelChildMaterialState& dst) {
        dst.HasMesh = true;
        dst.material = src.material;
        dst.materials = src.materials;
        dst.MaterialSlotNames = src.MaterialSlotNames;
        dst.MaterialAssetPaths = src.MaterialAssetPaths;
        dst.OwnedMaterialSlots = src.OwnedMaterialSlots;
        dst.InlineMaterials = src.InlineMaterials;
        dst.ShaderGraphMaterials = src.ShaderGraphMaterials;
        dst.SlotPropertyBlocks = src.SlotPropertyBlocks;
        dst.MaterialSources = src.MaterialSources;
        dst.PropertyBlock = src.PropertyBlock;
        dst.PropertyBlockTexturePaths = src.PropertyBlockTexturePaths;
        dst.SlotPropertyBlockTexturePaths = src.SlotPropertyBlockTexturePaths;
    };
    auto applyMaterials = [](MeshComponent& dst, const ModelChildMaterialState& s) {
        dst.material = s.material;
        dst.materials = s.materials;
        dst.MaterialSlotNames = s.MaterialSlotNames;
        dst.MaterialAssetPaths = s.MaterialAssetPaths;
        dst.OwnedMaterialSlots = s.OwnedMaterialSlots;
        dst.InlineMaterials = s.InlineMaterials;
        dst.ShaderGraphMaterials = s.ShaderGraphMaterials;
        dst.SlotPropertyBlocks = s.SlotPropertyBlocks;
        dst.MaterialSources = s.MaterialSources;
        dst.PropertyBlock = s.PropertyBlock;
        dst.PropertyBlockTexturePaths = s.PropertyBlockTexturePaths;
        dst.SlotPropertyBlockTexturePaths = s.SlotPropertyBlockTexturePaths;
    };

    // Build path -> default material state map from the fresh instance.
    std::unordered_map<std::string, ModelChildMaterialState> pathToDefault;
    std::function<void(EntityID, const std::string&)> collectDefaults = [&](EntityID id, const std::string& parentPath) {
        auto* d = GetEntityData(id);
        if (!d) return;
        std::string path = parentPath.empty() ? d->Name : (parentPath + "/" + d->Name);
        ModelChildMaterialState state;
        if (d->Mesh) snapshotMaterials(*d->Mesh, state);
        pathToDefault[path] = state;
        pathToDefault[ModelNodeIdentity::NormalizePath(path)] = state;
        for (EntityID c : d->Children) collectDefaults(c, path);
    };
    for (EntityID c : GetEntityData(tempRoot)->Children) {
        collectDefaults(c, "");
    }

    // The snapshots hold shared_ptr refs to the fresh materials, so removing the
    // temp instance won't free them before we re-assign them onto the live children.
    RemoveEntity(tempRoot);

    auto relPathOf = [this](EntityID root, EntityID node) -> std::string {
        std::vector<std::string> parts;
        EntityID cur = node;
        while (cur != INVALID_ENTITY_ID) {
            auto* d = GetEntityData(cur);
            if (!d) break;
            parts.push_back(d->Name);
            if (cur == root) break;
            cur = d->Parent;
        }
        if (parts.empty()) return std::string();
        std::reverse(parts.begin(), parts.end());
        if (!parts.empty()) parts.erase(parts.begin());
        std::string s;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i > 0) s += "/";
            s += parts[i];
        }
        return s;
    };

    int applied = 0;
    std::function<void(EntityID)> applyToChildren = [&](EntityID id) {
        auto* d = GetEntityData(id);
        if (!d) return;
        // Skip nested model roots - they have their own model identity.
        if (id != modelRootId && d->ModelAssetGuid.high != 0 && d->ModelAssetGuid.low != 0) return;
        for (EntityID c : d->Children) {
            auto* cd = GetEntityData(c);
            if (!cd) continue;
            if (cd->ModelAssetGuid.high != 0 || cd->ModelAssetGuid.low != 0) {
                applyToChildren(c);  // recurse into nested model but don't reset it
                continue;
            }
            std::string path = relPathOf(modelRootId, c);
            if (!path.empty()) {
                auto it = pathToDefault.find(path);
                if (it == pathToDefault.end()) {
                    it = pathToDefault.find(ModelNodeIdentity::NormalizePath(path));
                }
                if (it != pathToDefault.end() && it->second.HasMesh && cd->Mesh) {
                    applyMaterials(*cd->Mesh, it->second);
                    ++applied;
                }
            }
            applyToChildren(c);
        }
    };
    applyToChildren(modelRootId);

    if (applied > 0) {
        MarkDirty();
    }
    std::cout << "[Scene] Reset materials on " << applied << " model child mesh(es)" << std::endl;
    return applied > 0;
}

bool Scene::AlignModelRootChildrenToTerrain(EntityID modelRootId) {
    auto* rootData = GetEntityData(modelRootId);
    if (!rootData) return false;
    if (rootData->ModelAssetGuid.high == 0 && rootData->ModelAssetGuid.low == 0) return false;
    if (rootData->Children.empty()) return false;

    std::vector<EntityID> terrainEntities;
    terrainEntities.reserve(8);
    for (const auto& entity : GetEntities()) {
        auto* d = GetEntityData(entity.GetID());
        if (d && d->Terrain) {
            terrainEntities.push_back(entity.GetID());
        }
    }
    if (terrainEntities.empty()) return false;

    auto transformAabbByMatrix = [](const glm::vec3& localMin,
                                    const glm::vec3& localMax,
                                    const glm::mat4& world,
                                    glm::vec3& ioMin,
                                    glm::vec3& ioMax) {
        const glm::vec3 corners[8] = {
            glm::vec3(localMin.x, localMin.y, localMin.z),
            glm::vec3(localMax.x, localMin.y, localMin.z),
            glm::vec3(localMin.x, localMax.y, localMin.z),
            glm::vec3(localMax.x, localMax.y, localMin.z),
            glm::vec3(localMin.x, localMin.y, localMax.z),
            glm::vec3(localMax.x, localMin.y, localMax.z),
            glm::vec3(localMin.x, localMax.y, localMax.z),
            glm::vec3(localMax.x, localMax.y, localMax.z),
        };
        for (const glm::vec3& c : corners) {
            const glm::vec3 wc = glm::vec3(world * glm::vec4(c, 1.0f));
            ioMin = glm::min(ioMin, wc);
            ioMax = glm::max(ioMax, wc);
        }
    };

    auto collectSubtreeWorldAabb = [this, &transformAabbByMatrix](EntityID subtreeRoot,
                                                                   glm::vec3& outMin,
                                                                   glm::vec3& outMax) -> bool {
        bool hasBounds = false;
        outMin = glm::vec3(std::numeric_limits<float>::max());
        outMax = glm::vec3(-std::numeric_limits<float>::max());

        std::function<void(EntityID)> visit = [&](EntityID id) {
            auto* d = GetEntityData(id);
            if (!d) return;

            if (d->Mesh && d->Mesh->mesh) {
                const glm::vec3 localMin = d->Mesh->mesh->BoundsMin;
                const glm::vec3 localMax = d->Mesh->mesh->BoundsMax;
                const glm::mat4 world = EvaluateWorldMatrix(*this, id);
                transformAabbByMatrix(localMin, localMax, world, outMin, outMax);
                hasBounds = true;
            }

            for (EntityID c : d->Children) {
                visit(c);
            }
        };

        visit(subtreeRoot);
        return hasBounds;
    };

    int alignedCount = 0;
    for (EntityID child : rootData->Children) {
        auto* childData = GetEntityData(child);
        if (!childData) continue;

        glm::vec3 worldMin(0.0f), worldMax(0.0f);
        if (!collectSubtreeWorldAabb(child, worldMin, worldMax)) {
            continue;
        }

        const float sampleX = (worldMin.x + worldMax.x) * 0.5f;
        const float sampleZ = (worldMin.z + worldMax.z) * 0.5f;
        const float castStartY = std::max(worldMax.y + 1000.0f, 10000.0f);
        const glm::vec3 rayOrigin(sampleX, castStartY, sampleZ);
        const glm::vec3 rayDir(0.0f, -1.0f, 0.0f);

        bool hitAnyTerrain = false;
        float bestTerrainY = -std::numeric_limits<float>::max();
        for (EntityID terrainId : terrainEntities) {
            auto* terrainData = GetEntityData(terrainId);
            if (!terrainData || !terrainData->Terrain) continue;

            glm::vec3 terrainHit(0.0f);
            if (Terrain::Raycast(terrainData->Transform, *terrainData->Terrain, rayOrigin, rayDir, &terrainHit, nullptr, nullptr, nullptr)) {
                if (!hitAnyTerrain || terrainHit.y > bestTerrainY) {
                    bestTerrainY = terrainHit.y;
                    hitAnyTerrain = true;
                }
            }
        }
        if (!hitAnyTerrain) {
            continue;
        }

        const float deltaY = bestTerrainY - worldMin.y;
        if (std::abs(deltaY) <= 1e-4f) {
            continue;
        }

        glm::mat4 childWorld = EvaluateWorldMatrix(*this, child);
        childWorld[3][1] += deltaY;

        const EntityID parentId = childData->Parent;
        glm::mat4 parentWorld(1.0f);
        if (parentId != INVALID_ENTITY_ID) {
            parentWorld = EvaluateWorldMatrix(*this, parentId);
        }

        const glm::mat4 newLocal = (parentId != INVALID_ENTITY_ID)
            ? (glm::inverse(parentWorld) * childWorld)
            : childWorld;
        ApplyMatrixToTransform(childData, newLocal);
        MarkTransformDirty(child);
        ++alignedCount;
    }

    if (alignedCount > 0) {
        MarkDirty();
    }
    std::cout << "[Scene] Aligned " << alignedCount << " model child subtree(s) to terrain" << std::endl;
    return alignedCount > 0;
}
#else
bool Scene::ResetModelChildrenToDefault(EntityID) { return false; }
bool Scene::ResetModelChildrenMaterialsToDefault(EntityID) { return false; }
bool Scene::AlignModelRootChildrenToTerrain(EntityID) { return false; }
#endif


///-----------------------------------------------------------------------------------------
/// FindEntityByID: return the Entity associated with the given ID.
///-----------------------------------------------------------------------------------------
Entity Scene::FindEntityByID(EntityID id) {
   auto it = std::find_if(m_EntityList.begin(), m_EntityList.end(),
      [&](const Entity& e) { return e.GetID() == id; });
   return (it != m_EntityList.end()) ? *it : Entity();
}


///-----------------------------------------------------------------------------------------
/// FindEntityByGUID: Find an entity by its GUID.
///-----------------------------------------------------------------------------------------
EntityID Scene::FindEntityByGUID(const ClaymoreGUID& guid) const {
    if (guid.high == 0 && guid.low == 0) {
        return INVALID_ENTITY_ID;
    }
    
    for (const auto& [id, data] : m_Entities) {
        if (data.EntityGuid.high == guid.high && data.EntityGuid.low == guid.low) {
            return id;
        }
    }
    return INVALID_ENTITY_ID;
}


///-----------------------------------------------------------------------------------------
/// FindEntityByPath: Find an entity by hierarchical path (e.g., "Parent/Child/GrandChild").
/// Returns INVALID_ENTITY_ID if not found.
///-----------------------------------------------------------------------------------------
EntityID Scene::FindEntityByPath(const std::string& path) const {
    if (path.empty()) {
        return INVALID_ENTITY_ID;
    }

    // Parse the path into segments
    std::vector<std::string> segments;
    std::stringstream ss(path);
    std::string segment;
    while (std::getline(ss, segment, '/')) {
        if (!segment.empty()) {
            segments.push_back(segment);
        }
    }
    
    if (segments.empty()) {
        return INVALID_ENTITY_ID;
    }
    
    // Find root entity (first segment)
    EntityID current = INVALID_ENTITY_ID;
    for (const auto& [id, data] : m_Entities) {
        if (data.Name == segments[0] && data.Parent == INVALID_ENTITY_ID) {
            current = id;
            break;
        }
    }
    
    if (current == INVALID_ENTITY_ID) {
        return INVALID_ENTITY_ID;
    }
    
    // Walk down the hierarchy
    for (size_t i = 1; i < segments.size(); ++i) {
        auto it = m_Entities.find(current);
        if (it == m_Entities.end()) {
            return INVALID_ENTITY_ID;
        }
        
        EntityID found = INVALID_ENTITY_ID;
        for (EntityID childId : it->second.Children) {
            auto childIt = m_Entities.find(childId);
            if (childIt != m_Entities.end() && childIt->second.Name == segments[i]) {
                found = childId;
                break;
            }
        }
        
        if (found == INVALID_ENTITY_ID) {
            return INVALID_ENTITY_ID;
        }
        current = found;
    }
    
    return current;
}

namespace {
static std::string NormalizeModelNodeName(const std::string& name) {
    size_t underscore = name.rfind('_');
    if (underscore == std::string::npos) {
        return name;
    }
    if (underscore + 1 >= name.size()) {
        return name;
    }
    for (size_t i = underscore + 1; i < name.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
            return name;
        }
    }
    return name.substr(0, underscore);
}

static EntityID FindChildByNameOrNormalized(Scene& scene, EntityID parent, const std::string& name) {
    auto* pd = scene.GetEntityData(parent);
    if (!pd) return INVALID_ENTITY_ID;
    for (EntityID c : pd->Children) {
        auto* cd = scene.GetEntityData(c);
        if (cd && cd->Name == name) return c;
    }
    const std::string normalized = NormalizeModelNodeName(name);
    for (EntityID c : pd->Children) {
        auto* cd = scene.GetEntityData(c);
        if (cd && NormalizeModelNodeName(cd->Name) == normalized) return c;
    }
    return INVALID_ENTITY_ID;
}

static EntityID ResolveEntityRefFromMetadata(Scene& scene, const ScriptEntityRefMetadata& meta) {
    if (meta.guid.high != 0 || meta.guid.low != 0) {
        EntityID byGuid = scene.FindEntityByGUID(meta.guid);
        if (byGuid != INVALID_ENTITY_ID) return byGuid;
    }

    if (meta.modelGuid.high != 0 || meta.modelGuid.low != 0) {
        EntityID modelRoot = INVALID_ENTITY_ID;
        int modelRootMatches = 0;

        if (meta.modelRootGuid.high != 0 || meta.modelRootGuid.low != 0) {
            EntityID hintedRoot = scene.FindEntityByGUID(meta.modelRootGuid);
            if (hintedRoot != INVALID_ENTITY_ID) {
                auto* hintedData = scene.GetEntityData(hintedRoot);
                if (hintedData &&
                    hintedData->ModelAssetGuid.high == meta.modelGuid.high &&
                    hintedData->ModelAssetGuid.low == meta.modelGuid.low) {
                    modelRoot = hintedRoot;
                    modelRootMatches = 1;
                }
            }
        }

        if (modelRoot == INVALID_ENTITY_ID) {
            for (const auto& e : scene.GetEntities()) {
                auto* d = scene.GetEntityData(e.GetID());
                if (!d) continue;
                if (d->ModelAssetGuid.high == meta.modelGuid.high &&
                    d->ModelAssetGuid.low == meta.modelGuid.low) {
                    modelRoot = e.GetID();
                    ++modelRootMatches;
                }
            }
        }
        if (modelRoot == INVALID_ENTITY_ID) return INVALID_ENTITY_ID;
        // Ambiguous model-path refs are unsafe at scene scope (multiple instances of same model).
        // Leave unresolved instead of binding to an arbitrary instance.
        if (modelRootMatches > 1) return INVALID_ENTITY_ID;
        if (meta.modelNodePath.empty()) return modelRoot;

        EntityID cur = modelRoot;
        std::stringstream ss(meta.modelNodePath);
        std::string seg;
        while (std::getline(ss, seg, '/')) {
            if (seg.empty()) continue;
            EntityID next = FindChildByNameOrNormalized(scene, cur, seg);
            if (next == INVALID_ENTITY_ID) {
                return INVALID_ENTITY_ID;
            }
            cur = next;
        }
        return cur;
    }

    return INVALID_ENTITY_ID;
}

static bool IsEntityLikeProperty(PropertyType t) {
    return t == PropertyType::Entity || t == PropertyType::ComponentRef || t == PropertyType::ScriptRef;
}

static bool IsEntityIdValid(Scene& scene, int id) {
    if (id <= 0 || id == static_cast<int>(INVALID_ENTITY_ID)) return false;
    return scene.GetEntityData(static_cast<EntityID>(id)) != nullptr;
}
} // namespace

void Scene::ResolveScriptEntityReferencesFromMetadata() {
    for (const auto& e : GetEntities()) {
        auto* d = GetEntityData(e.GetID());
        if (!d) continue;

        for (auto& script : d->Scripts) {
            if (script.EntityRefMetadata.empty()) continue;

            // Resolve single entity references
            for (auto& [key, value] : script.Values) {
                auto metaIt = script.EntityRefMetadata.find(key);
                if (metaIt == script.EntityRefMetadata.end()) continue;

                ScriptEntityRefMetadata& meta = metaIt->second;
                if (!std::holds_alternative<int>(value)) continue;

                int currentId = std::get<int>(value);
                if (IsEntityIdValid(*this, currentId)) {
                    meta.entityId = currentId;
                    meta.unresolved = false;
                    continue;
                }

                EntityID resolved = ResolveEntityRefFromMetadata(*this, meta);
                if (resolved != INVALID_ENTITY_ID) {
                    value = static_cast<int>(resolved);
                    meta.entityId = static_cast<int>(resolved);
                    meta.unresolved = false;
                } else {
                    meta.unresolved = true;
                }
            }

            // Resolve list entity references
            for (auto& [key, value] : script.Values) {
                if (!std::holds_alternative<std::shared_ptr<ListPropertyValue>>(value)) continue;
                auto listPtr = std::get<std::shared_ptr<ListPropertyValue>>(value);
                if (!listPtr || !IsEntityLikeProperty(listPtr->elementType)) continue;

                for (size_t i = 0; i < listPtr->elements.size(); ++i) {
                    if (!std::holds_alternative<int>(listPtr->elements[i])) continue;
                    int currentId = std::get<int>(listPtr->elements[i]);
                    if (IsEntityIdValid(*this, currentId)) continue;

                    if (i >= listPtr->entityRefs.size()) continue;
                    ScriptEntityRefMetadata& meta = listPtr->entityRefs[i];
                    EntityID resolved = ResolveEntityRefFromMetadata(*this, meta);
                    if (resolved != INVALID_ENTITY_ID) {
                        listPtr->elements[i] = static_cast<int>(resolved);
                        meta.entityId = static_cast<int>(resolved);
                        meta.unresolved = false;
                    } else {
                        meta.unresolved = true;
                    }
                }
            }
        }
    }
}

void Scene::PrepareSceneForLoad() {
    if (!m_IsPlaying) return;
    std::unordered_set<EntityID> persistent = CollectPersistentEntities();
    std::vector<EntityID> ids;
    ids.reserve(GetEntities().size());
    for (const auto& e : GetEntities()) {
        EntityID id = e.GetID();
        if (!persistent.empty() && persistent.find(id) != persistent.end()) continue;
        ids.push_back(id);
    }
    for (EntityID id : ids) {
        RemoveEntity(id);
    }
    EntityID maxId = 0;
    for (const auto& e : GetEntities()) {
        maxId = std::max(maxId, e.GetID());
    }
    ResetEntityIdCounter(maxId + 1);
}

void Scene::BeginLoadTracking(const std::string& path, bool async) {
    m_Loading = true;
    m_Loaded = false;
    m_LoadingAsync = async;
    m_LoadProgress = 0.0f;
    m_LoadScriptsTotal = 0;
    m_LoadScriptsProcessed = 0;
    m_LoadScriptsReady = false;
    m_LoadingScenePath = path;
    m_LoadStartTime = std::chrono::steady_clock::now();
    m_LoadHoldStartTime = {};
    m_LoadHoldDuration = 0.0f;
    m_LoadHoldStartProgress = 0.0f;
    m_LoadingHold = false;
    // Play-mode loads need the same gate even for synchronous scene loads:
    // scripts can spawn prefabs in OnCreate/first input frames, and those
    // prefabs must be prewarmed before script execution starts.
    m_DeferScriptOnCreate = async || m_IsPlaying;
    m_RestoreDeferredScriptInit = false;
    m_PrevDeferredScriptInit = DeferredScriptInit::g_EnableDeferredScriptInit;
    if (m_DeferScriptOnCreate) {
        if (DeferredScriptInit::HasPendingScripts()) {
            DeferredScriptInit::ClearQueue();
        }
        if (!DeferredScriptInit::g_EnableDeferredScriptInit) {
            DeferredScriptInit::g_EnableDeferredScriptInit = true;
            m_RestoreDeferredScriptInit = true;
        }
    }
}

void Scene::FinalizeLoadTracking(bool success) {
    m_Loading = false;
    m_Loaded = success;
    m_LoadingAsync = false;
    const float prevProgress = m_LoadProgress;
    m_LoadProgress = success ? 1.0f : 0.0f;
    m_LoadScriptsTotal = 0;
    m_LoadScriptsProcessed = 0;
    m_LoadScriptsReady = false;
    if (success && !m_LoadingScenePath.empty()) {
        m_ScenePath = m_LoadingScenePath;
    }
    m_LoadingScenePath.clear();
    m_DeferScriptOnCreate = false;
    if (!success && DeferredScriptInit::HasPendingScripts()) {
        DeferredScriptInit::ClearQueue();
    }
    if (m_RestoreDeferredScriptInit) {
        DeferredScriptInit::g_EnableDeferredScriptInit = m_PrevDeferredScriptInit;
        m_RestoreDeferredScriptInit = false;
    }
    m_AsyncLoadJob.reset();

    if (success) {
        const auto now = std::chrono::steady_clock::now();
        const float elapsed = std::chrono::duration<float>(now - m_LoadStartTime).count();
        if (elapsed < kMinLoadDisplaySeconds) {
            m_LoadingHold = true;
            m_LoadHoldStartTime = now;
            m_LoadHoldDuration = std::max(0.05f, kMinLoadDisplaySeconds - elapsed);
            m_LoadHoldStartProgress = std::min(prevProgress, 0.95f);
            m_LoadProgress = m_LoadHoldStartProgress;
            m_Loading = true;
        }
    }
}

void Scene::PostLoadFixups() {
    // Recompute transforms and fix up references before script OnCreate.
    UpdateTransforms();
    ResolveScriptEntityReferencesFromMetadata();
    cm::world::PortalSystem::RebuildRuntimePortals(*this);
    
    for (const auto& e : GetEntities()) {
        EntityID id = e.GetID();
        auto* data = GetEntityData(id);
        if (!data || !data->Collider) continue;

        bool wantsBody = (data->RigidBody != nullptr) || (data->StaticBody != nullptr);
        if (!wantsBody) continue;

        bool hasBody = (data->RigidBody && !data->RigidBody->BodyID.IsInvalid()) ||
                       (data->StaticBody && !data->StaticBody->BodyID.IsInvalid());
        if (hasBody) continue;

        glm::vec3 wscale(1.0f);
        glm::vec3 wpos, wskew;
        glm::vec4 wpersp;
        glm::quat wrot;
        glm::decompose(data->Transform.WorldMatrix, wscale, wrot, wpos, wskew, wpersp);

        if (!data->Collider->Shape) {
            data->Collider->BuildShape(
                data->Mesh && data->Mesh->mesh ? data->Mesh->mesh.get() : nullptr,
                glm::abs(wscale));
        }

        if (data->Collider->Shape) {
            CreatePhysicsBody(id, data->Transform, *data->Collider);
        }
    }
}

bool Scene::LoadSceneImmediate(const std::string& path, bool async)
{
    if (path.empty()) return false;
    BeginLoadTracking(path, async);
    PrepareSceneForLoad();

    bool success = false;
#if !defined(CLAYMORE_RUNTIME)
    if (Serializer::LoadSceneFromFile(path, *this)) {
        m_LoadProgress = 0.7f;
        PostLoadFixups();
        m_LoadProgress = 0.85f;
        success = true;
    }
    if (!success && m_IsPlaying && Assets::ShouldLoadBinary() && !Assets::AllowSourceFallback()) {
        std::cerr << "[Scene] Binary scene missing; falling back to authoring scene: " << path << std::endl;
        try {
            nlohmann::json sceneData;
            std::string sceneText;
            if (FileSystem::Instance().ReadTextFile(path, sceneText)) {
                sceneData = nlohmann::json::parse(sceneText);
            } else {
                std::vector<uint8_t> bytes;
                if (FileSystem::Instance().ReadFile(path, bytes)) {
                    sceneData = nlohmann::json::parse(
                        std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
                } else {
                    std::cerr << "[Scene] Scene file does not exist or cannot be read: " << path << std::endl;
                    return false;
                }
            }
            success = Serializer::DeserializeScene(sceneData, *this);
            if (success) {
                prefab::QueueScenePrefabs(*this);
                std::cout << "[Scene] Scene loaded from source: " << path << std::endl;
                m_LoadProgress = 0.7f;
                PostLoadFixups();
                m_LoadProgress = 0.85f;
            }
        } catch (const std::exception& e) {
            std::cerr << "[Scene] Error loading authoring scene: " << e.what() << std::endl;
        }
    }
#else
    std::string scenePath(path);
    if (scenePath.find(".sceneb") == std::string::npos) {
        std::filesystem::path p(scenePath);
        p.replace_extension(".sceneb");
        scenePath = p.string();
    }
    if (binary::EntityBinaryLoader::Load(scenePath, *this)) {
        m_LoadProgress = 0.7f;
        PostLoadFixups();
        m_LoadProgress = 0.85f;
        success = true;
    }
#endif
    if (!success) {
        FinalizeLoadTracking(false);
        return false;
    }

    prefab::QueueScenePrefabs(*this);

    if (m_DeferScriptOnCreate) {
        m_LoadScriptsTotal = DeferredScriptInit::GetPendingCount();
        m_LoadScriptsReady = true;
        if (m_LoadScriptsTotal == 0 && !prefab::HasPendingWork()) {
            FinalizeLoadTracking(true);
        }
    } else {
        FinalizeLoadTracking(true);
    }
    return true;
}

void Scene::StartAsyncLoadJob(const std::string& path) {
    if (path.empty()) {
        FinalizeLoadTracking(false);
        return;
    }
    BeginLoadTracking(path, true);
    PrepareSceneForLoad();
    
    m_AsyncLoadJob = std::make_shared<AsyncLoadJob>();
    m_AsyncLoadJob->sourcePath = path;
    m_AsyncLoadJob->startTime = std::chrono::steady_clock::now();
    
    std::string scenePath(path);
    if (scenePath.find(".sceneb") == std::string::npos) {
        std::filesystem::path p(scenePath);
        p.replace_extension(".sceneb");
        scenePath = p.string();
    }
    if (Assets::ShouldLoadBinary()) {
        bool resolvedPreparedBinary = false;
        if (IAssetResolver* resolver = Assets::GetResolver()) {
            if (resolver->IsBinaryCurrent(path)) {
                std::string binPath = resolver->GetBinaryPath(path);
                if (!binPath.empty()) {
                    scenePath = binPath;
                    resolvedPreparedBinary = true;
                }
            }
        }
        const bool requirePreparedBinary =
            Assets::GetLoadMode() == AssetLoadMode::PlayMode && !FileSystem::Instance().IsPakMounted();
        if (requirePreparedBinary &&
            !resolvedPreparedBinary &&
            std::filesystem::path(path).extension() != ".sceneb") {
            scenePath.clear();
        }
    }
    m_AsyncLoadJob->binaryPath = scenePath;
    
    std::string readPath = m_AsyncLoadJob->binaryPath;
    auto job = m_AsyncLoadJob;
    auto readTask = [job, readPath]() {
        std::vector<uint8_t> local = ReadSceneBinary(readPath);
        if (local.empty()) {
            std::lock_guard<std::mutex> lock(job->dataMutex);
            job->readError = "empty data";
            job->readFailed.store(true, std::memory_order_release);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(job->dataMutex);
            job->data = std::move(local);
        }
        job->readReady.store(true, std::memory_order_release);
    };
    if (cm::g_JobSystem) {
        if (!Jobs().Enqueue(readTask, JobSystem::Priority::Low)) {
            readTask();
        }
    } else {
        readTask();
    }
}

void Scene::TickAsyncLoadJob() {
    if (!m_AsyncLoadJob) return;
    
    AsyncLoadJob& job = *m_AsyncLoadJob;
    const std::string sourcePath = job.sourcePath;
    const std::string binaryPath = job.binaryPath;
    static constexpr auto kAsyncReadTimeout = std::chrono::seconds(15);
    static constexpr auto kAsyncStreamTimeout = std::chrono::seconds(20);
    if (!job.dataReady) {
        if (job.readFailed.load(std::memory_order_acquire)) {
            std::string err;
            {
                std::lock_guard<std::mutex> lock(job.dataMutex);
                err = job.readError;
            }
            std::cerr << "[Scene] Async scene load failed (" << (err.empty() ? "read failed" : err) << "): "
                      << binaryPath << std::endl;
            m_AsyncLoadJob.reset();
            LoadSceneImmediate(sourcePath, false);
            return;
        }
        if (!job.readReady.load(std::memory_order_acquire)) {
            const auto now = std::chrono::steady_clock::now();
            if (job.startTime.time_since_epoch().count() != 0 &&
                now - job.startTime > kAsyncReadTimeout) {
                std::cerr << "[Scene] Async scene load timed out while reading: " << binaryPath << std::endl;
                m_AsyncLoadJob.reset();
                LoadSceneImmediate(sourcePath, false);
            }
            return;
        }
        {
            std::lock_guard<std::mutex> lock(job.dataMutex);
            if (job.data.empty()) {
                std::cerr << "[Scene] Async scene load failed (empty data): " << binaryPath << std::endl;
                m_AsyncLoadJob.reset();
                LoadSceneImmediate(sourcePath, false);
                return;
            }
        }
        job.dataReady = true;
        if (!binary::EntityBinaryLoader::BeginStreaming(job.stream, job.data.data(), job.data.size())) {
            std::cerr << "[Scene] Async scene load failed (invalid binary): " << binaryPath << std::endl;
            // Fall back to synchronous source load (editor/runtime preview)
            m_AsyncLoadJob.reset();
            LoadSceneImmediate(sourcePath, false);
            return;
        }
        job.streamStartTime = std::chrono::steady_clock::now();
        job.lastProgressTime = job.streamStartTime;
        job.lastProgress = 0.0f;
    }
    
    static constexpr uint32_t kEntityBatch = 64;
    static constexpr uint32_t kMeshBatch = 16;
    static constexpr uint32_t kMaterialBatch = 8;
    
    if (!binary::EntityBinaryLoader::StepStreaming(job.stream, *this, kEntityBatch, kMeshBatch, kMaterialBatch)) {
        std::cerr << "[Scene] Async scene streaming failed: " << binaryPath << std::endl;
        // Fall back to synchronous source load if streaming fails mid-way
        m_AsyncLoadJob.reset();
        LoadSceneImmediate(sourcePath, false);
        return;
    }
    
    m_LoadProgress = 0.85f * binary::EntityBinaryLoader::GetStreamingProgress(job.stream);
    
    const auto now = std::chrono::steady_clock::now();
    if (m_LoadProgress > job.lastProgress + 0.0001f) {
        job.lastProgress = m_LoadProgress;
        job.lastProgressTime = now;
    } else if (job.streamStartTime.time_since_epoch().count() != 0 &&
               now - job.lastProgressTime > kAsyncStreamTimeout) {
        std::cerr << "[Scene] Async scene load timed out during streaming: " << binaryPath << std::endl;
        m_AsyncLoadJob.reset();
        LoadSceneImmediate(sourcePath, false);
        return;
    }
    
    if (binary::EntityBinaryLoader::IsStreamingComplete(job.stream)) {
        PostLoadFixups();
        prefab::QueueScenePrefabs(*this);
        m_LoadProgress = 0.85f;
        if (m_DeferScriptOnCreate) {
            m_LoadScriptsTotal = DeferredScriptInit::GetPendingCount();
            m_LoadScriptsReady = true;
            if (m_LoadScriptsTotal == 0 && !prefab::HasPendingWork()) {
                FinalizeLoadTracking(true);
            }
        } else {
            FinalizeLoadTracking(true);
        }
    }
}

void Scene::RequestSceneLoad(const std::string& path)
{
    RequestSceneLoad(path, true);
}

void Scene::RequestSceneLoad(const std::string& path, bool async)
{
    m_PendingSceneLoadPath = path;
    m_PendingSceneLoad = !path.empty();
    m_PendingSceneLoadAsync = async;
    if (m_PendingSceneLoad) {
        m_Loading = true;
        m_Loaded = false;
        m_LoadingAsync = async;
        m_LoadProgress = 0.0f;
        m_LoadScriptsTotal = 0;
        m_LoadScriptsProcessed = 0;
        m_LoadScriptsReady = false;
        m_LoadingScenePath = path;
    }
}

std::unordered_set<EntityID> Scene::CollectPersistentEntities()
{
    std::unordered_set<EntityID> persistent;

    // Clear previous flags
    for (const auto& e : GetEntities()) {
        if (auto* d = GetEntityData(e.GetID())) {
            d->PersistAcrossScenes = false;
        }
    }

    if (!m_IsPlaying) return persistent;

    auto isDontDestroyScript = [&](const std::string& className) -> bool {
        if (ScriptSystem::Instance().IsScriptDontDestroyOnLoad(className)) {
            return true;
        }
        if (className.find('.') == std::string::npos) {
            const auto& registry = ScriptSystem::Instance().GetRegistry();
            std::string matchedFullName;
            for (const auto& entry : registry) {
                const std::string& fullName = entry.first;
                if (fullName.size() <= className.size()) continue;
                if (fullName.compare(fullName.size() - className.size(), className.size(), className) != 0) continue;
                if (fullName[fullName.size() - className.size() - 1] != '.') continue;
                if (!matchedFullName.empty() && matchedFullName != fullName) {
                    return false;
                }
                matchedFullName = fullName;
            }
            if (!matchedFullName.empty()) {
                return ScriptSystem::Instance().IsScriptDontDestroyOnLoad(matchedFullName);
            }
        }
        return false;
    };

    auto hasPersistentScript = [&](const EntityData& d) -> bool {
        for (const auto& script : d.Scripts) {
            if (isDontDestroyScript(script.ClassName)) {
                return true;
            }
        }
        return false;
    };

    std::vector<EntityID> marked;
    marked.reserve(GetEntities().size());
    for (const auto& e : GetEntities()) {
        auto* d = GetEntityData(e.GetID());
        if (!d) continue;
        if (!hasPersistentScript(*d)) continue;
        marked.push_back(e.GetID());
    }

    if (marked.empty()) return persistent;

    std::cout << "[Scene] Persisting entities due to DontDestroyOnLoad:\n";
    for (EntityID id : marked) {
        auto* d = GetEntityData(id);
        if (!d) continue;
        std::string scriptNames;
        for (const auto& script : d->Scripts) {
            if (isDontDestroyScript(script.ClassName)) {
                if (!scriptNames.empty()) scriptNames += ", ";
                scriptNames += script.ClassName;
            }
        }
        std::cout << "  id=" << id << " name=\"" << d->Name << "\" scripts=[" << scriptNames << "]\n";
    }

    auto markSubtree = [&](EntityID root) {
        std::vector<EntityID> stack;
        stack.push_back(root);
        while (!stack.empty()) {
            EntityID id = stack.back();
            stack.pop_back();
            if (!persistent.insert(id).second) continue;

            auto* d = GetEntityData(id);
            if (!d) continue;
            for (EntityID child : d->Children) {
                stack.push_back(child);
            }
        }
    };

    std::unordered_set<EntityID> markedSet(marked.begin(), marked.end());
    std::unordered_set<EntityID> processedRoots;

    for (EntityID id : marked) {
        EntityID root = id;
        auto* r = GetEntityData(root);
        while (r && r->Parent != INVALID_ENTITY_ID) {
            root = r->Parent;
            r = GetEntityData(root);
        }

        if (markedSet.find(root) != markedSet.end()) {
            if (processedRoots.insert(root).second) {
                markSubtree(root);
            }
            continue;
        }

        // Mark the full subtree for the marked entity.
        markSubtree(id);

        // Mark ancestor chain up to the root-level parent (but not siblings).
        EntityID cur = id;
        auto* d = GetEntityData(cur);
        while (d && d->Parent != INVALID_ENTITY_ID) {
            cur = d->Parent;
            if (!persistent.insert(cur).second) break;
            d = GetEntityData(cur);
        }
    }

    for (EntityID id : persistent) {
        if (auto* d = GetEntityData(id)) {
            d->PersistAcrossScenes = true;
        }
    }

    std::cout << "[Scene] Persisted entity count: " << persistent.size() << "\n";

    return persistent;
}


///-----------------------------------------------------------------------------------------
/// CreateLight: Create an Entity with a light component.
///-----------------------------------------------------------------------------------------
Entity Scene::CreateLight(const std::string& name, LightType type, const glm::vec3& color, float intensity) {
   Entity entity = CreateEntity(name);
   if (auto* data = GetEntityData(entity.GetID())) {
      data->Light = std::make_unique<LightComponent>(type, color, intensity);
   }
   return entity;
}

// ============================================================================
// EDITOR-ONLY ASSET/MODEL INSTANTIATION
// These functions use editor pipeline code and are not available in runtime.
// Runtime uses pre-built binary scenes loaded via EntityBinaryLoader.
// ============================================================================
#ifndef CLAYMORE_RUNTIME

///-----------------------------------------------------------------------------------------
/// InstantiateAsset: Instantiate an imported Asset into the scene.
///-----------------------------------------------------------------------------------------
EntityID Scene::InstantiateAsset(const std::string& path, const glm::vec3& position) {

   std::string ext = fs::path(path).extension().string();
   std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

   auto finalizePrefabInstance = [&](EntityID rootId) -> EntityID {
      if (rootId == INVALID_ENTITY_ID || rootId == (EntityID)0) {
         return INVALID_ENTITY_ID;
      }
      if (auto* d = GetEntityData(rootId)) {
         d->Transform.Position = position;
         // Record source so scene serialization can emit a compact prefab reference.
         try {
            std::string norm = path;
            for (char& c : norm) {
               if (c == '\\') c = '/';
            }
            std::error_code ec;
            fs::path rel = fs::relative(norm, Project::GetProjectDirectory(), ec);
            std::string vpath = ec ? norm : rel.string();
            for (char& c : vpath) {
               if (c == '\\') c = '/';
            }
            size_t pos = vpath.find("assets/");
            if (pos != std::string::npos) {
               vpath = vpath.substr(pos);
            }
            d->PrefabSource = vpath;
         } catch (...) {
         }
         MarkTransformDirty(rootId);
      }
      return rootId;
   };

   auto instantiatePrefab = [&]() -> EntityID {
      std::string binaryPath;
      if (IAssetResolver* resolver = Assets::GetResolver()) {
         if (resolver->IsBinaryCurrent(path)) {
            binaryPath = resolver->GetBinaryPath(path);
         }
      } else {
         binaryPath = Assets::GetBinaryPath(path);
      }
      if (!binaryPath.empty()) {
         runtime::RuntimePrefabInstantiator::Preload(binaryPath);
         EntityID rootId =
            runtime::RuntimePrefabInstantiator::InstantiateBlocking(binaryPath, *this);
         if (rootId != INVALID_ENTITY_ID && rootId != (EntityID)0) {
            return rootId;
         }
      }
      return InstantiatePrefabFromPath(path, *this);
   };

   if (ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb")
   {
      return InstantiateModel(path, position);
   }
   else if (ext == ".prefab") {
      EntityID rootId = instantiatePrefab();
      if (rootId == INVALID_ENTITY_ID) {
         std::cerr << "[Scene] Failed to load prefab: " << path << std::endl;
         return INVALID_ENTITY_ID;
      }
      return finalizePrefabInstance(rootId);
   }
   else if (ext == ".json") {
      EntityID rootId = instantiatePrefab();
      if (rootId == INVALID_ENTITY_ID || rootId == (EntityID)0) {
         std::cerr << "[Scene] Failed to instantiate authoring prefab: " << path << std::endl;
         return INVALID_ENTITY_ID;
      }
      return finalizePrefabInstance(rootId);
   }
   else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg") {
      // Create a simple textured quad
      Entity entity = CreateEntity("ImageQuad");
      auto* data = GetEntityData(entity.GetID());
      if (!data)
      {
         return -1;
      }

      data->Transform.Position = glm::vec3(0.0f);
      data->Transform.Rotation = glm::vec3(0.0f);
      data->Transform.Scale = glm::vec3(1.0f);

      std::shared_ptr<Mesh> quadMesh = StandardMeshManager::Instance().GetPlaneMesh();
      auto material = MaterialManager::Instance().CreateSceneDefaultMaterial(this);
      if (auto pbr = std::dynamic_pointer_cast<PBRMaterial>(material)) {
         pbr->SetAlbedoTextureFromPath(path);
      }

      data->Mesh = std::make_unique<MeshComponent>(quadMesh, "ImageQuad", material);
      data->Mesh->meshReference = AssetReference::CreatePrimitive("Plane");
      if (!data->RenderOverrides) {
         data->RenderOverrides = std::make_unique<RenderOverridesComponent>();
      }

      return entity.GetID();
   }
   else {
      std::cerr << "[Scene] Unsupported asset type: " << ext << std::endl;
      return -1;
   }
}


///-----------------------------------------------------------------------------------------
/// InstantiateModel: Instantiate a 3D model into the scene.
/// -----------------------------------------------------------------------------------------
EntityID Scene::InstantiateModel(const std::string& path, const glm::vec3& rootPosition, EntityID existingRoot) {

   auto instantiateFastFromMeta = [&](const std::string& metaPath) -> EntityID
   {
      EntityID fastId = InstantiateModelFast(metaPath, rootPosition, true, existingRoot);
      if (fastId == INVALID_ENTITY_ID)
      {
         std::cerr << "[Scene] Fast instantiation failed for " << metaPath
            << " (run Import/Reimport Asset if the cache is stale)." << std::endl;
      }
      return fastId;
   };

   std::filesystem::path requested(path);
   std::string ext = requested.extension().string();
   std::transform(ext.begin(), ext.end(), ext.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

   if (ext == ".meta")
   {
      return instantiateFastFromMeta(path);
   }

   BuiltModelPaths built{};
   if (!HasModelCache(path, built)) {
#if !defined(CLAYMORE_RUNTIME)
      if (!EnsureModelCache(path, built))
      {
         std::cerr << "[Scene] Missing or stale model cache for " << path
            << " (editor auto-rebuild failed; use Import/Reimport Asset if needed)."
            << std::endl;
         return INVALID_ENTITY_ID;
      }
#else
      std::cerr << "[Scene] Missing or stale model cache for " << path
         << " (use Import/Reimport Asset to generate meshbin/skelbin/meta before instantiating)." << std::endl;
      return INVALID_ENTITY_ID;
#endif
   }

   return instantiateFastFromMeta(built.metaPath);
}

///-------------------------------------------------------------------------------------------
/// Instantiate Model Fast: Use cached binaries (skelbin, meshbin, meta) to quickly instantiate
/// a model in the scene. This shares the same instantiation pipeline as the slow path by
/// always going through BuildInstanceFromMeta + InstantiatePreparedModel.
///-------------------------------------------------------------------------------------------
EntityID Scene::InstantiateModelFast(const std::string& metaPath, const glm::vec3& position, bool synchronous, EntityID existingRoot)
{
    if (!synchronous)
    {
        std::cout << "[Scene] InstantiateModelFast: async mode not supported, loading synchronously\n";
    }

    auto loadJson = [](const std::string& path, nlohmann::json& out) -> bool {
        // Use VFS so runtime and editor behave identically
        std::string content;
        if (!FileSystem::Instance().ReadTextFile(path, content)) {
            return false;
        }
        try {
            out = nlohmann::json::parse(content);
            return true;
        } catch (...) {
            return false;
        }
    };

    try
    {
        nlohmann::json metaJson;
        BuiltModelPaths built{};

        std::filesystem::path requested(metaPath);
        std::string ext = requested.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
            [](unsigned char c) { return (char)std::tolower(c); });

        std::string sourcePath;
         
        // If we were given a .meta, read its JSON to get the original source path
        if (ext == ".meta")
        { 
            if (!loadJson(metaPath, metaJson))
            {
                std::cerr << "[Scene] Failed to read meta: " << metaPath << std::endl;
                return -1;
            }
            sourcePath = metaJson.value("source", std::string());
        }
         
        auto resolveToAbsolute = [](const std::string& path) -> std::string {
            if (path.empty()) return {};
            std::filesystem::path p(path);
            if (p.is_absolute())
                return p.string();
            try
            {
                std::filesystem::path base = Project::GetProjectDirectory();
                if (!base.empty())
                    return (base / p).string();
            }
            catch (...) {}
            return p.string();
            };

        // Decide which path key we use to locate the cache
        std::string cacheKey =
            (ext == ".meta" && !sourcePath.empty())
            ? resolveToAbsolute(sourcePath)
            : resolveToAbsolute(metaPath);

        bool recoveredCacheInPlayMode = false;
        if (!HasModelCache(cacheKey, built))
        {
#if !defined(CLAYMORE_RUNTIME)
            const std::string recoverySourcePath =
                !sourcePath.empty()
                    ? resolveToAbsolute(sourcePath)
                    : (ext != ".meta" ? cacheKey : std::string());
            if (!recoverySourcePath.empty() && EnsureModelCache(recoverySourcePath, built))
            {
                recoveredCacheInPlayMode = true;
            }
            else
            {
                std::cerr << "[Scene] InstantiateModelFast: missing or stale model cache for "
                    << cacheKey
                    << " (editor auto-rebuild failed)"
                    << std::endl;
                return -1;
            }
#else
            std::cerr << "[Scene] InstantiateModelFast: missing or stale model cache for "
                << cacheKey
                << " (run Import/Reimport Asset to rebuild)" << std::endl;
            return -1;
#endif
        }

        // Prefer the on-disk meta from the cache unless we were explicitly given a specific .meta
        std::string metaJsonPath = (ext == ".meta" && !recoveredCacheInPlayMode) ? metaPath : built.metaPath;
        if (!loadJson(metaJsonPath, metaJson))
        {
            std::cerr << "[Scene] Failed to read meta: " << metaJsonPath << std::endl;
            return -1;
        }
        if (!metaJson.is_object())
        {
            std::cerr << "[Scene] Invalid meta JSON: " << metaJsonPath << std::endl;
            return -1;
        }

        // Final source path to record in the PreparedModelInstance
        std::string rawSourcePath = metaJson.value("source", sourcePath);
        if (!rawSourcePath.empty())
        {
            sourcePath = rawSourcePath;
        }
        if (sourcePath.empty())
        {
            // Fallback: treat the cache key or meta as the source
            sourcePath = (ext == ".meta") ? metaPath : cacheKey;
        }
        std::string resolvedSourcePath = resolveToAbsolute(sourcePath);
        if (!resolvedSourcePath.empty())
        {
            sourcePath = resolvedSourcePath;
        }

        // Load skeleton from skelbin
        skelbin::PackedSkeleton skeleton;
        (void)skelbin::ReadSkelBin(built.skelPath, skeleton);

        // Resolve model GUID
        ClaymoreGUID modelGuid{};
        try
        {
            if (metaJson.contains("guid") && metaJson["guid"].is_string())
            {
                modelGuid = ClaymoreGUID::FromString(metaJson["guid"].get<std::string>());
            }
            else if (metaJson.contains("guid") && metaJson["guid"].is_object())
            {
                metaJson["guid"].get_to(modelGuid);
            }
        }
        catch (...) {}

        if (modelGuid.high == 0 && modelGuid.low == 0)
            modelGuid = AssetLibrary::Instance().GetGUIDForPath(built.metaPath);

        if (modelGuid.high == 0 && modelGuid.low == 0)
        {
            modelGuid = ClaymoreGUID::Generate();
            AssetReference ref(modelGuid, 0, static_cast<int32_t>(AssetType::Mesh));
            AssetLibrary::Instance().RegisterAsset(
                ref, AssetType::Mesh, built.metaPath, DeriveModelRootName(cacheKey));
        }

        // Register aliases so serialization and asset resolution work the same as the slow path
        AssetLibrary::Instance().RegisterPathAlias(modelGuid, built.metaPath);
        AssetLibrary::Instance().RegisterPathAlias(modelGuid, built.meshPath);
        AssetLibrary::Instance().RegisterPathAlias(modelGuid, built.skelPath);
        if (!sourcePath.empty())
            AssetLibrary::Instance().RegisterPathAlias(modelGuid, sourcePath);
        if (!rawSourcePath.empty() && rawSourcePath != sourcePath)
            AssetLibrary::Instance().RegisterPathAlias(modelGuid, rawSourcePath);

        // Determine which mesh indices to load
        std::vector<int> meshIndices;
        if (metaJson.contains("entries") && metaJson["entries"].is_array())
        {
            for (const auto& entry : metaJson["entries"])
            {
                if (!entry.is_object()) continue;
                int meshIndex = entry.value("meshIndex", -1);
                if (meshIndex >= 0)
                    meshIndices.push_back(meshIndex);
            }
        }

        if (meshIndices.empty())
        {
            uint32_t count = meshbin::GetSubmeshCount(built.meshPath);
            for (uint32_t i = 0; i < count; ++i)
                meshIndices.push_back(static_cast<int>(i));
        }

        // Prefetch meshes and build the PreparedModelInstance from meta/skeleton/meshbin
        meshbin::PrefetchResult meshes = PrefetchMeshesBlocking(built.meshPath, meshIndices);
        PreparedModelInstance instance = BuildInstanceFromMeta(
            metaJson,
            built,
            skeleton,
            modelGuid,
            sourcePath,
            built.metaPath,
            meshIndices,
            std::move(meshes));

        // Shared instantiation pipeline (same as slow path)
        return InstantiatePreparedModel(*this, instance, position, existingRoot);
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Scene] InstantiateModelFast error: " << e.what() << std::endl;
        return -1;
    }
    catch (...)
    {
        std::cerr << "[Scene] InstantiateModelFast error: unknown exception" << std::endl;
        return -1;
    }
}

#endif // CLAYMORE_RUNTIME - End of editor-only asset/model instantiation

void Scene::SetParent(EntityID child, EntityID parent, bool preserveWorldTransform) {
   if (parent == 0) {
      parent = INVALID_ENTITY_ID;
   }

   auto* childData = GetEntityData(child);
   if (!childData)
   {
      return;
   }

   // No-op or self-parent guard
   if (child == parent || childData->Parent == parent)
   {
      return;
   }

   // Handle UNPARENTING (parent == INVALID_ENTITY_ID / -1)
   // This makes the child a root-level entity with no parent
   if (parent == INVALID_ENTITY_ID) {
      glm::mat4 childWorld = glm::mat4(1.0f);
      if (preserveWorldTransform && childData->Parent != INVALID_ENTITY_ID) {
         childWorld = EvaluateWorldMatrix(*this, child);
      }
      
      // Remove from old parent's Children vector
      if (childData->Parent != INVALID_ENTITY_ID) {
         auto* oldParent = GetEntityData(childData->Parent);
         if (oldParent) {
            oldParent->Children.erase(
               std::remove(oldParent->Children.begin(), oldParent->Children.end(), child),
               oldParent->Children.end());
         }
      }
      
      // Set parent to invalid (child becomes root-level)
      childData->Parent = INVALID_ENTITY_ID;
      
      // Ensure unique name among root-level siblings
      auto hasRootName = [&](const std::string& name) -> bool {
         for (const auto& pair : m_Entities) {
            if (pair.first == child) continue;
            if (pair.second.Parent == INVALID_ENTITY_ID && pair.second.Name == name) return true;
         }
         return false;
      };
      if (hasRootName(childData->Name)) {
         childData->Name = MakeUniqueName(childData->Name, hasRootName);
      }
      
      // If preserving world transform, local = world (since there's no parent)
      if (preserveWorldTransform) {
         ApplyMatrixToTransform(childData, childWorld);
      }
      
      InvalidateHierarchyCache();
      MarkTransformDirty(child);
      MarkRuntimeWorldDirty(child,
         cm::world::RuntimeDirtyBits::Hierarchy |
         cm::world::RuntimeDirtyBits::Metadata);
      return;
   }

   // Normal case: parent is a valid entity
   auto* parentData = GetEntityData(parent);
   if (!parentData)
   {
      return;
   }

   // Prevent cycles: ensure 'parent' is not in child's subtree
   {
      EntityID cur = parent;
      while (cur != INVALID_ENTITY_ID) {
         // Cycle detected; ignore
         if (cur == child)
         {
            return;
         }
         auto* d = GetEntityData(cur);
         if (!d)
         {
            break;
         }
         cur = d->Parent;
      }
   }

   glm::mat4 childWorld = glm::mat4(1.0f);
   glm::mat4 parentWorld = glm::mat4(1.0f);
   if (preserveWorldTransform)
   {
      childWorld = EvaluateWorldMatrix(*this, child);
      parentWorld = EvaluateWorldMatrix(*this, parent);
   }

   if (childData->Parent != INVALID_ENTITY_ID) {
      auto* oldParent = GetEntityData(childData->Parent);
      if (oldParent)
         oldParent->Children.erase(std::remove(oldParent->Children.begin(), oldParent->Children.end(), child),
            oldParent->Children.end());
   }

   // Ensure unique name among new parent's children
   // BUT: Skip suffix logic for model-internal children (both child and parent belong to same model root)
   // Model internal structure should preserve original names without index suffixes
   // Only the model root has ModelAssetGuid set, so we need to walk up to find the model root
   bool isModelInternal = false;
   
   // Find model root for child (walk up hierarchy)
   EntityID childModelRoot = INVALID_ENTITY_ID;
   if (childData->ModelAssetGuid.high != 0 || childData->ModelAssetGuid.low != 0) {
      childModelRoot = child;
   } else {
      EntityID cur = childData->Parent;
      while (cur != INVALID_ENTITY_ID) {
         auto* d = GetEntityData(cur);
         if (!d) break;
         if (d->ModelAssetGuid.high != 0 || d->ModelAssetGuid.low != 0) {
            childModelRoot = cur;
            break;
         }
         cur = d->Parent;
      }
   }
   
   // Find model root for parent (walk up hierarchy)
   EntityID parentModelRoot = INVALID_ENTITY_ID;
   if (parentData->ModelAssetGuid.high != 0 || parentData->ModelAssetGuid.low != 0) {
      parentModelRoot = parent;
   } else {
      EntityID cur = parentData->Parent;
      while (cur != INVALID_ENTITY_ID) {
         auto* d = GetEntityData(cur);
         if (!d) break;
         if (d->ModelAssetGuid.high != 0 || d->ModelAssetGuid.low != 0) {
            parentModelRoot = cur;
            break;
         }
         cur = d->Parent;
      }
   }
   
   // If both child and parent belong to the same model root, skip suffix logic
   if (childModelRoot != INVALID_ENTITY_ID && parentModelRoot != INVALID_ENTITY_ID && 
       childModelRoot == parentModelRoot) {
      isModelInternal = true;
   }
   
   // Only enforce sibling name uniqueness if NOT model-internal
   // For model-internal children, names are already unique from the model asset
   if (!isModelInternal) {
      auto hasSiblingName = [&](const std::string& name) -> bool {
         for (EntityID siblingId : parentData->Children) {
            if (siblingId == child) continue;
            auto* sd = GetEntityData(siblingId);
            if (sd && sd->Name == name) return true;
         }
         return false;
      };
      if (hasSiblingName(childData->Name)) {
         childData->Name = MakeUniqueName(childData->Name, hasSiblingName);
      }
   }

   childData->Parent = parent;
   parentData->Children.push_back(child);

   if (childData->Text && childData->Text->WorldSpace) {
      EntityID current = parent;
      while (current != INVALID_ENTITY_ID) {
         auto* currentData = GetEntityData(current);
         if (!currentData) {
            break;
         }
         if (currentData->Canvas) {
            childData->Text->WorldSpace = false;
            break;
         }
         current = currentData->Parent;
      }
   }

   if (preserveWorldTransform)
   {
      glm::mat4 parentInverse = glm::mat4(1.0f);
      const float det = glm::determinant(parentWorld);
      if (std::fabs(det) > 1e-8f)
      {
         parentInverse = glm::inverse(parentWorld);
      }
      glm::mat4 newLocal = parentInverse * childWorld;
      ApplyMatrixToTransform(childData, newLocal);
   }

   // PERF: Invalidate hierarchy cache since parent-child relationship changed
   InvalidateHierarchyCache();
   
   // Mark child subtree dirty so transforms recompute relative to new parent
   MarkTransformDirty(child);
   MarkRuntimeWorldDirty(child,
      cm::world::RuntimeDirtyBits::Hierarchy |
      cm::world::RuntimeDirtyBits::Metadata);

   // Runtime-only presentation suppression should flow to newly parented
   // children without mutating authored visibility flags.
   if (HasPresentationHiddenAncestor(*this, parent)) {
      SetEntityPresentationHidden(child, true);
   }
}

void Scene::SetParentFast(EntityID child, EntityID parent) {
   auto* childData = GetEntityData(child);
   if (!childData) {
      return;
   }
   auto* parentData = GetEntityData(parent);
   if (!parentData) {
      return;
   }
   if (child == parent) {
      return;
   }
   if (childData->Parent == parent) {
      return;
   }
   if (childData->Parent != INVALID_ENTITY_ID) {
      auto* oldParent = GetEntityData(childData->Parent);
      if (oldParent) {
         oldParent->Children.erase(
            std::remove(oldParent->Children.begin(), oldParent->Children.end(), child),
            oldParent->Children.end());
      }
   }
   childData->Parent = parent;
   parentData->Children.push_back(child);
   if (childData->Text && childData->Text->WorldSpace) {
      EntityID current = parent;
      while (current != INVALID_ENTITY_ID) {
         auto* currentData = GetEntityData(current);
         if (!currentData) {
            break;
         }
         if (currentData->Canvas) {
            childData->Text->WorldSpace = false;
            break;
         }
         current = currentData->Parent;
      }
   }
   InvalidateHierarchyCache();
   MarkTransformDirty(child);
   MarkRuntimeWorldDirty(child, cm::world::RuntimeDirtyBits::Hierarchy);

   // Mirror SetParent behavior for runtime presentation suppression.
   if (HasPresentationHiddenAncestor(*this, parent)) {
      SetEntityPresentationHidden(child, true);
   }
}

bool Scene::ReorderEntity(EntityID entity, EntityID targetSibling, bool placeAfter) {
   if (entity == targetSibling) {
      return false;
   }
   auto* entityData = GetEntityData(entity);
   auto* targetData = GetEntityData(targetSibling);
   if (!entityData || !targetData) {
      return false;
   }
   if (entityData->Parent != targetData->Parent) {
      return false;
   }

   const EntityID parentId = entityData->Parent;
   if (parentId == INVALID_ENTITY_ID) {
      auto findIndex = [&](EntityID id) -> size_t {
         for (size_t i = 0; i < m_EntityList.size(); ++i) {
            if (m_EntityList[i].GetID() == id) return i;
         }
         return m_EntityList.size();
      };
      size_t entityIndex = findIndex(entity);
      size_t targetIndex = findIndex(targetSibling);
      if (entityIndex >= m_EntityList.size() || targetIndex >= m_EntityList.size()) {
         return false;
      }

      Entity moved = m_EntityList[entityIndex];
      m_EntityList.erase(m_EntityList.begin() + entityIndex);
      if (entityIndex < targetIndex) {
         targetIndex--;
      }
      size_t insertIndex = placeAfter ? targetIndex + 1 : targetIndex;
      if (insertIndex > m_EntityList.size()) insertIndex = m_EntityList.size();
      m_EntityList.insert(m_EntityList.begin() + insertIndex, moved);
   } else {
      auto* parentData = GetEntityData(parentId);
      if (!parentData) {
         return false;
      }
      auto& children = parentData->Children;
      auto itEntity = std::find(children.begin(), children.end(), entity);
      auto itTarget = std::find(children.begin(), children.end(), targetSibling);
      if (itEntity == children.end() || itTarget == children.end()) {
         return false;
      }
      size_t entityIndex = static_cast<size_t>(std::distance(children.begin(), itEntity));
      size_t targetIndex = static_cast<size_t>(std::distance(children.begin(), itTarget));
      if (entityIndex == targetIndex) {
         return false;
      }

      children.erase(itEntity);
      if (entityIndex < targetIndex) {
         targetIndex--;
      }
      size_t insertIndex = placeAfter ? targetIndex + 1 : targetIndex;
      if (insertIndex > children.size()) insertIndex = children.size();
      children.insert(children.begin() + insertIndex, entity);
   }

   MarkDirty();
   InvalidateHierarchyCache();
   return true;
}


void Scene::UpdateTransforms()
{
   auto& runtimeWorld = EnsureRuntimeWorld();
   runtimeWorld.UpdateTransforms(*this);
   cm::physics::RagdollSystem* ragdollSystem = cm::physics::GetRagdollSystem();
   const BoneAttachmentProcessMode attachmentMode =
      (m_IsPlaying && ragdollSystem && ragdollSystem->HasActiveRagdolls())
      ? BoneAttachmentProcessMode::ExcludeActiveRagdolls
      : BoneAttachmentProcessMode::All;
   ProcessBoneAttachments(attachmentMode);
   m_AnimationPosePaletteDirtyForAttachments = false;
   runtimeWorld.FinalizeTransformStage(*this);
}

static bool TryGetAnimatedAttachmentBoneWorldMatrix(Scene& scene,
                                                    const BoneAttachmentComponent& attachment,
                                                    bool ragdollDriven,
                                                    glm::mat4& outBoneWorld)
{
   if (ragdollDriven ||
       attachment.ResolvedSkeletonEntity == INVALID_ENTITY_ID ||
       attachment.ResolvedBoneIndex < 0) {
      return false;
   }

   auto* skeletonData = scene.GetEntityData(attachment.ResolvedSkeletonEntity);
   if (!skeletonData || !skeletonData->Skeleton) {
      return false;
   }

   SkeletonComponent& skeleton = *skeletonData->Skeleton;
   const size_t boneIndex = static_cast<size_t>(attachment.ResolvedBoneIndex);
   if (!skeleton.AnimatedPosePaletteValid ||
       boneIndex >= skeleton.BoneCount ||
       boneIndex >= skeleton.InverseBindPoses.size()) {
      return false;
   }

   glm::mat4 bindGlobal(1.0f);
   if (boneIndex < skeleton.BindPoseGlobals.size()) {
      bindGlobal = skeleton.BindPoseGlobals[boneIndex];
   } else {
      bindGlobal = glm::inverse(skeleton.InverseBindPoses[boneIndex]);
   }

   const glm::mat4 skeletonLocalBone = skeleton.BonePalette[boneIndex] * bindGlobal;
   outBoneWorld = skeletonData->Transform.WorldMatrix * skeletonLocalBone;
   return true;
}

bool Scene::TryGetBoneWorldMatrix(EntityID id, glm::mat4& outWorld)
{
   EntityData* data = GetEntityData(id);
   if (!data || data->BoneSkeletonEntity == INVALID_ENTITY_ID || data->BoneIndex < 0)
   {
      return false;
   }

   EntityData* skeletonData = GetEntityData(data->BoneSkeletonEntity);
   if (!skeletonData || !skeletonData->Skeleton)
   {
      return false;
   }

   SkeletonComponent& skeleton = *skeletonData->Skeleton;
   const size_t boneIndex = static_cast<size_t>(data->BoneIndex);
   // Only use the pose buffer when the animated palette is current. When it is not
   // (e.g. editor not playing, or ragdoll-driven skeletons that clear the flag),
   // callers fall back to the propagated AoS transform, preserving existing behavior.
   if (!skeleton.AnimatedPosePaletteValid ||
       boneIndex >= skeleton.BoneCount ||
       boneIndex >= skeleton.InverseBindPoses.size() ||
       boneIndex >= skeleton.BonePalette.size())
   {
      return false;
   }

   glm::mat4 bindGlobal(1.0f);
   if (boneIndex < skeleton.BindPoseGlobals.size())
   {
      bindGlobal = skeleton.BindPoseGlobals[boneIndex];
   }
   else
   {
      bindGlobal = glm::inverse(skeleton.InverseBindPoses[boneIndex]);
   }

   // BonePalette[i] == animatedBoneGlobal * InverseBindPose, so
   // BonePalette[i] * bindGlobal == animatedBoneGlobal (bone transform in skeleton
   // space). World = skeletonRootWorld * animatedBoneGlobal. (Same derivation the
   // bone-attachment path uses.)
   const glm::mat4 skeletonLocalBone = skeleton.BonePalette[boneIndex] * bindGlobal;
   outWorld = skeletonData->Transform.WorldMatrix * skeletonLocalBone;
   return true;
}

void Scene::RebindSkeletonBoneMarkers(EntityID skeletonRootId)
{
   EntityData* skeletonData = GetEntityData(skeletonRootId);
   if (!skeletonData || !skeletonData->Skeleton)
   {
      return;
   }

   const SkeletonComponent& skeleton = *skeletonData->Skeleton;
   for (size_t i = 0; i < skeleton.BoneEntities.size(); ++i)
   {
      const EntityID boneId = skeleton.BoneEntities[i];
      if (boneId == INVALID_ENTITY_ID || boneId == static_cast<EntityID>(-1) || boneId == 0)
      {
         continue;
      }
      EntityData* boneData = GetEntityData(boneId);
      if (!boneData)
      {
         continue;
      }
      boneData->BoneSkeletonEntity = skeletonRootId;
      boneData->BoneIndex = static_cast<int>(i);
   }
}

bool Scene::ProcessBoneAttachments(BoneAttachmentProcessMode mode)
{
   // Find entities with BoneAttachmentComponent and update their transforms
   // based on their target bone's current world transform
   cm::physics::RagdollSystem* ragdollSystem =
      (m_IsPlaying && cm::physics::GetRagdollSystem()) ? cm::physics::GetRagdollSystem() : nullptr;
   if (mode == BoneAttachmentProcessMode::OnlyActiveRagdolls &&
       (!ragdollSystem || !ragdollSystem->HasActiveRagdolls())) {
      return false;
   }

   const std::vector<EntityID>& attachmentEntities = GetBoneAttachmentEntities();
   if (attachmentEntities.empty()) {
      return false;
   }

   std::unordered_map<EntityID, bool> ragdollActiveCache;
   if (ragdollSystem) {
      ragdollActiveCache.reserve(attachmentEntities.size());
   }
   bool anyAttachmentChanged = false;

   for (EntityID id : attachmentEntities) {
      EntityData* attachmentData = GetEntityData(id);
      if (!attachmentData || !attachmentData->BoneAttachment || !attachmentData->BoneAttachment->Enabled) {
         continue;
      }
      if (!attachmentData->Active || !attachmentData->Visible || attachmentData->PresentationHidden) {
         continue;
      }
      
      auto& data = *attachmentData;
      auto& ba = *data.BoneAttachment;
      
      // Resolve skeleton and bone on first use or if invalidated
      if (!ba.ResolutionAttempted) {
         ba.ResolutionAttempted = true;
         
         // Find skeleton entity - either explicit or by walking up hierarchy
         EntityID skelEntity = ba.SkeletonEntity;
         if (skelEntity == INVALID_ENTITY_ID) {
            // Strategy 1: Walk up parent hierarchy to find skeleton directly on an ancestor
            EntityID cur = data.Parent;
            while (cur != INVALID_ENTITY_ID) {
               auto* pd = GetEntityData(cur);
               if (!pd) break;
               if (pd->Skeleton && !pd->Skeleton->BoneEntities.empty()) {
                  skelEntity = cur;
                  break;
               }
               cur = pd->Parent;
            }
            
            // Strategy 2: Check direct siblings for skeleton (entity is sibling of skeleton root)
            if (skelEntity == INVALID_ENTITY_ID && data.Parent != INVALID_ENTITY_ID) {
               auto* parentData = GetEntityData(data.Parent);
               if (parentData) {
                  for (EntityID siblingId : parentData->Children) {
                     if (siblingId == id) continue;  // Skip self
                     auto* siblingData = GetEntityData(siblingId);
                     if (siblingData && siblingData->Skeleton && !siblingData->Skeleton->BoneEntities.empty()) {
                        skelEntity = siblingId;
                        break;
                     }
                  }
               }
            }
            
            // Strategy 3: Walk up to find model roots, then check siblings and their direct children
            // This handles: base_human/SkeletonRoot (sibling model root of) Sword01/Sword_84
            // where Sword_84 needs to find SkeletonRoot (a direct child of sibling model root base_human)
            if (skelEntity == INVALID_ENTITY_ID) {
               cur = data.Parent;
               while (cur != INVALID_ENTITY_ID) {
                  auto* pd = GetEntityData(cur);
                  if (!pd) break;
                  
                  // Check if current entity is a model root (has ModelAssetGuid)
                  bool isModelRoot = (pd->ModelAssetGuid.high != 0 || pd->ModelAssetGuid.low != 0);
                  
                  if (isModelRoot && pd->Parent != INVALID_ENTITY_ID) {
                     auto* grandparentData = GetEntityData(pd->Parent);
                     if (grandparentData) {
                        // Check each sibling of this model root
                        for (EntityID siblingId : grandparentData->Children) {
                           if (siblingId == cur) continue;  // Skip self (current model root)
                           auto* siblingData = GetEntityData(siblingId);
                           if (!siblingData) continue;
                           
                           // Check if sibling itself has skeleton
                           if (siblingData->Skeleton && !siblingData->Skeleton->BoneEntities.empty()) {
                              skelEntity = siblingId;
                              break;
                           }
                           
                           // Check sibling's direct children for skeleton
                           // (skeleton is always a direct child of a model root)
                           for (EntityID nephewId : siblingData->Children) {
                              auto* nephewData = GetEntityData(nephewId);
                              if (nephewData && nephewData->Skeleton && !nephewData->Skeleton->BoneEntities.empty()) {
                                 skelEntity = nephewId;
                                 break;
                              }
                           }
                           if (skelEntity != INVALID_ENTITY_ID) break;
                        }
                     }
                     if (skelEntity != INVALID_ENTITY_ID) break;
                  }
                  
                  cur = pd->Parent;
               }
            }
         }
         
         if (skelEntity != INVALID_ENTITY_ID) {
            auto* skelData = GetEntityData(skelEntity);
            if (skelData && skelData->Skeleton) {
               // Find bone by name with fuzzy matching (same logic as animation evaluator)
               auto tryResolveBone = [&](const std::string& name) -> int {
                  // Try exact match first
                  int idx = skelData->Skeleton->GetBoneIndex(name);
                  if (idx >= 0) return idx;
                  
                  // Try suffix after common namespace separators (mixamorig:, etc.)
                  size_t pos = name.find_last_of(':');
                  if (pos != std::string::npos && pos + 1 < name.size()) {
                     idx = skelData->Skeleton->GetBoneIndex(name.substr(pos + 1));
                     if (idx >= 0) return idx;
                  }
                  pos = name.find_last_of('|');
                  if (pos != std::string::npos && pos + 1 < name.size()) {
                     idx = skelData->Skeleton->GetBoneIndex(name.substr(pos + 1));
                     if (idx >= 0) return idx;
                  }
                  pos = name.find_last_of('.');
                  if (pos != std::string::npos && pos + 1 < name.size()) {
                     idx = skelData->Skeleton->GetBoneIndex(name.substr(pos + 1));
                     if (idx >= 0) return idx;
                  }
                  
                  // Final fallback: suffix match against known bone names
                  // This allows "RightHand" to match "mixamorig:RightHand"
                  for (const auto& kv : skelData->Skeleton->BoneNameToIndex) {
                     const std::string& skName = kv.first;
                     if (skName.size() >= name.size()) {
                        // Check if skeleton bone name ends with search name
                        if (skName.compare(skName.size() - name.size(), name.size(), name) == 0) {
                           // Verify there's a separator before the match (not partial word)
                           if (skName.size() == name.size() || 
                               skName[skName.size() - name.size() - 1] == ':' ||
                               skName[skName.size() - name.size() - 1] == '|' ||
                               skName[skName.size() - name.size() - 1] == '.') {
                              return kv.second;
                           }
                        }
                     } else {
                        // Check if search name ends with skeleton bone name
                        if (name.compare(name.size() - skName.size(), skName.size(), skName) == 0) {
                           if (name.size() == skName.size() ||
                               name[name.size() - skName.size() - 1] == ':' ||
                               name[name.size() - skName.size() - 1] == '|' ||
                               name[name.size() - skName.size() - 1] == '.') {
                              return kv.second;
                           }
                        }
                     }
                  }
                  return -1;
               };
               
               int boneIndex = tryResolveBone(ba.TargetBoneName);
               if (boneIndex >= 0 && (size_t)boneIndex < skelData->Skeleton->BoneEntities.size()) {
                  ba.ResolvedSkeletonEntity = skelEntity;
                  ba.ResolvedBoneIndex = boneIndex;
                  ba.ResolvedBoneEntity = skelData->Skeleton->BoneEntities[(size_t)boneIndex];
               }
            }
         }
      }
      
      // Skip if resolution failed
      if (ba.ResolvedSkeletonEntity == INVALID_ENTITY_ID || ba.ResolvedBoneIndex < 0) continue;

      bool ragdollDriven = false;
      if (ragdollSystem && ba.ResolvedSkeletonEntity != INVALID_ENTITY_ID) {
         auto cacheIt = ragdollActiveCache.find(ba.ResolvedSkeletonEntity);
         if (cacheIt == ragdollActiveCache.end()) {
            cacheIt = ragdollActiveCache.emplace(
               ba.ResolvedSkeletonEntity,
               ragdollSystem->IsSkeletonRagdollActive(ba.ResolvedSkeletonEntity)).first;
         }
         ragdollDriven = cacheIt->second;
      }

      if (mode == BoneAttachmentProcessMode::ExcludeActiveRagdolls && ragdollDriven) {
         continue;
      }
      if (mode == BoneAttachmentProcessMode::OnlyActiveRagdolls && !ragdollDriven) {
         continue;
      }
      
      glm::mat4 boneWorld(1.0f);
      const bool usingAnimatedPose =
         TryGetAnimatedAttachmentBoneWorldMatrix(*this, ba, ragdollDriven, boneWorld);
      if (!usingAnimatedPose) {
         auto* boneData = GetEntityData(ba.ResolvedBoneEntity);
         if (!boneData) continue;
         boneWorld = boneData->Transform.WorldMatrix;
      }
      
      // Build final world transform from bone + local offset
      const glm::mat4& localOffset = ba.GetLocalOffsetMatrix();
      
      glm::mat4 finalWorld = glm::mat4(1.0f);
      if (ba.InheritRotation && ba.InheritScale) {
         // Full bone transform inheritance
         finalWorld = boneWorld * localOffset;
      } else if (ba.InheritRotation) {
         // Position and rotation only, ignore bone scale
         glm::vec3 bonePos = glm::vec3(boneWorld[3]);
         glm::mat3 boneRot = glm::mat3(boneWorld);
         // Remove scale from rotation matrix
         for (int i = 0; i < 3; ++i) {
            float len = glm::length(glm::vec3(boneRot[i]));
            if (len > 1e-6f) boneRot[i] /= len;
         }
         glm::mat4 boneNoScale = glm::mat4(boneRot);
         boneNoScale[3] = glm::vec4(bonePos, 1.0f);
         finalWorld = boneNoScale * localOffset;
      } else {
         // Position only
         glm::vec3 bonePos = glm::vec3(boneWorld[3]);
         glm::mat4 posOnly = glm::translate(glm::mat4(1.0f), bonePos);
         finalWorld = posOnly * localOffset;
      }

      glm::mat4 parentWorld = glm::mat4(1.0f);
      if (data.Parent != INVALID_ENTITY_ID) {
         if (auto* parentData = GetEntityData(data.Parent)) {
            parentWorld = parentData->Transform.WorldMatrix;
         }
      }

      const glm::mat4 localMatrix = glm::inverse(parentWorld) * finalWorld;
      if (NearlyEqualMat4(finalWorld, data.Transform.WorldMatrix) &&
          NearlyEqualMat4(localMatrix, data.Transform.LocalMatrix)) {
         continue;
      }
      ApplyMatrixToTransform(&data, localMatrix);
      data.Transform.LocalMatrix = localMatrix;
      data.Transform.WorldMatrix = finalWorld;
      data.Transform.TransformDirty = false;
      SyncBoneAttachmentPhysicsBody(data);
      NotifyWorldTransformOverride(id);
      anyAttachmentChanged = true;
   }
   return anyAttachmentChanged;
}

void Scene::PropagateUnifiedMorphWeights(EntityID unifiedMorphEntity)
{
   auto* data = GetEntityData(unifiedMorphEntity);
   if (!data || !data->UnifiedMorph) return;
   
   auto* um = data->UnifiedMorph.get();
   if (!um || um->MemberMeshes.empty() || um->Names.empty()) return;
   
   // Ensure name index is built for fast lookup
   if (um->NameIndexDirty || um->NameIndex.size() != um->Names.size()) {
      um->NameIndex.clear();
      um->NameIndex.reserve(um->Names.size());
      for (size_t i = 0; i < um->Names.size(); ++i) {
         um->NameIndex[HashBlendShapeName(um->Names[i])] = i;
      }
      um->NameIndexDirty = false;
   }
   
   // Parallel propagation to child meshes
   const size_t meshCount = um->MemberMeshes.size();
   const std::vector<EntityID>& meshIds = um->MemberMeshes;
   
   parallel_for(Jobs(), size_t{0}, meshCount, size_t{1}, [&](size_t start, size_t count) {
      for (size_t m = start; m < start + count; ++m) {
         EntityID meshId = meshIds[m];
         auto* meshData = GetEntityData(meshId);
         if (!meshData || !meshData->BlendShapes) continue;
         
         bool anyChanged = false;
         for (auto& shape : meshData->BlendShapes->Shapes) {
            if (shape.NameHash == 0 && !shape.Name.empty()) {
               shape.UpdateNameHash();
            }
            auto it = um->NameIndex.find(shape.NameHash);
            if (it != um->NameIndex.end()) {
               size_t idx = it->second;
               if (idx < um->Weights.size()) {
                  float w = um->Weights[idx];
                  if (shape.Weight != w) {
                     shape.Weight = w;
                     anyChanged = true;
                  }
               }
            }
         }
         if (anyChanged) {
            meshData->BlendShapes->Dirty = true;
         }
      }
   });
}


void Scene::TopologicalSortEntities(std::vector<EntityID>& outSorted) {
   std::unordered_set<EntityID> visited;

   std::function<void(EntityID)> visit = [&](EntityID id) {
      if (visited.count(id))
      {
         return;
      }
      visited.insert(id);

      EntityData* data = GetEntityData(id);
      if (data && data->Children.size()) {
         for (EntityID child : data->Children) {
            visit(child);
         }
      }

      outSorted.push_back(id);
      };

   for (const Entity& e : m_EntityList) {
      EntityID id = e.GetID();
      EntityData* data = GetEntityData(id);
      if (data && data->Parent == -1)
      {
         visit(id);
      }
   }

   std::reverse(outSorted.begin(), outSorted.end()); // root-first
}

void Scene::SetPosition(EntityID id, const glm::vec3& pos) {
   auto* data = GetEntityData(id);
   if (data) {
      data->Transform.Position = pos;
      MarkTransformDirty(id);
   }
}

void Scene::MarkTransformDirty(EntityID id) {
   auto* data = GetEntityData(id);
   if (!data)
   {
      return;
   }

   // Keep transform dirtying worker-safe. Animation, prefab streaming, and
   // instancing paths can mark transforms from job threads, so do not touch
   // the runtime bridge's unordered_map here. Instead, set a worker-safe dirty
   // bit that RuntimeWorld consumes on the main thread during sync.
   data->Transform.TransformDirty = true;
   QueueRuntimeWorldTransformDirty(id);
}


// --------------------------------------------------------
// Create a clone of the current scene for play mode.
// This will copy entities, their data, and scripts.
// --------------------------------------------------------
std::shared_ptr<Scene> Scene::RuntimeClone() {
   auto clone = std::make_shared<Scene>();
   std::vector<std::tuple<ScriptInstance*, Entity, size_t>> toInitialize;

   clone->m_Entities.clear();
   clone->m_EntityList.clear();
   clone->m_BodyMap.clear();
   clone->m_NextID = m_NextID;
   clone->EnsureRuntimeWorldTransformTrackingCapacity(static_cast<size_t>(clone->m_NextID));
   clone->m_ScenePath = this->m_ScenePath;
   // Copy environment so play mode preserves edit-time settings
   clone->m_Environment = this->m_Environment;
   clone->m_EditorViewportState = m_EditorViewportState;
   clone->m_HasEditorViewportState = m_HasEditorViewportState;

   // Clone entities
   for (const Entity& e : m_EntityList) {
      EntityID id = e.GetID();
      const EntityData& sourceData = m_Entities.at(id);

      clone->m_EntityList.emplace_back(id, clone.get());
      clone->m_Entities.emplace(id, sourceData.DeepCopy(id, clone.get()));

      auto& data = clone->m_Entities[id];

      if (sourceData.Terrain && data.Terrain) {
         TerrainComponent& sourceTerrain = *sourceData.Terrain;
         TerrainComponent& clonedTerrain = *data.Terrain;
         clonedTerrain.InstancerLayersDirty = sourceTerrain.InstancerLayersDirty;

         const size_t layerCount = std::min(sourceTerrain.InstancerLayers.size(), clonedTerrain.InstancerLayers.size());
         for (size_t layerIndex = 0; layerIndex < layerCount; ++layerIndex) {
            const TerrainInstancerLayerDesc& sourceLayer = sourceTerrain.InstancerLayers[layerIndex];
            TerrainInstancerLayerDesc& clonedLayer = clonedTerrain.InstancerLayers[layerIndex];

            clonedLayer.RuntimeDirty = sourceLayer.RuntimeDirty;
            clonedLayer.Instancer.Runtime = sourceLayer.Instancer.Runtime;
            clonedLayer.Instancer.CachedMesh = sourceLayer.Instancer.CachedMesh;
            clonedLayer.Instancer.Stats = sourceLayer.Instancer.Stats;
            clonedLayer.Instancer.NeedsRegeneration = sourceLayer.Instancer.NeedsRegeneration;
            clonedLayer.Instancer.NeedsMeshReload = sourceLayer.Instancer.NeedsMeshReload;
         }
      }

      // Mark transform as dirty so world matrices are computed
      clone->MarkTransformDirty(id);

      // Ensure animator runtime flags are initialized for play mode
      if (data.AnimationPlayer) {
         // Reset one-shot init gate so PlayOnStart will apply in runtime
         data.AnimationPlayer->_InitApplied = false;
         
         // CRITICAL: Clear the Controller pointer so it reloads from file on play start.
         // Without this, edits made in the Animation Graph Panel are not reflected because
         // the shared_ptr from edit mode is copied, not the saved file contents.
         data.AnimationPlayer->Controller.reset();
         data.AnimationPlayer->AnimatorInstance = cm::animation::Animator{}; // Reset animator state
         data.AnimationPlayer->CurrentStateId = -1;
         data.AnimationPlayer->CachedClips.clear();
         data.AnimationPlayer->CachedAssets.clear();
         
         // Seed playing state from PlayOnStart
         data.AnimationPlayer->IsPlaying = data.AnimationPlayer->PlayOnStart;
         data.AnimationPlayer->_AutoControllerGenerated = false; // Force re-generation if needed
         if (!data.AnimationPlayer->ActiveStates.empty() && data.AnimationPlayer->PlayOnStart) {
            data.AnimationPlayer->ActiveStates.front().Time = 0.0f;
         }
      }

      // Reset audio source runtime state so PlayOnAwake re-triggers cleanly each
      // time play starts (the deep copy carries over edit-time runtime fields).
      if (data.AudioSource) {
         data.AudioSource->Initialized = false;
         data.AudioSource->IsPlaying = false;
         data.AudioSource->IsPaused = false;
         data.AudioSource->SoundHandle = INVALID_AUDIO_HANDLE;
         data.AudioSource->PlayRequested = false;
         data.AudioSource->StopRequested = false;
         data.AudioSource->PauseRequested = false;
         data.AudioSource->ResumeRequested = false;
      }

      for (size_t i = 0; i < data.Scripts.size(); ++i) {
         auto& script = data.Scripts[i];
         if (script.Instance)
            toInitialize.emplace_back(&script, Entity(id, clone.get()), i);
      }
   }

   // Sort by [Priority] then stable tie-breakers so OnCreate order is deterministic and global
   std::sort(toInitialize.begin(), toInitialize.end(), [](const auto& a, const auto& b) {
      ScriptInstance* sa = std::get<0>(a);
      ScriptInstance* sb = std::get<0>(b);
      const Entity& ea = std::get<1>(a);
      const Entity& eb = std::get<1>(b);
      size_t ia = std::get<2>(a), ib = std::get<2>(b);
      int pa = ScriptSystem::Instance().GetScriptPriority(sa->ClassName);
      int pb = ScriptSystem::Instance().GetScriptPriority(sb->ClassName);
      uint64_t ta = ScriptOrder::StableHashScriptClassName(sa->ClassName);
      uint64_t tb = ScriptOrder::StableHashScriptClassName(sb->ClassName);
      return ScriptOrder::OrderLess(pa, ta, ea.GetID(), ia, pb, tb, eb.GetID(), ib);
   });

   // Initialize transforms for the cloned scene BEFORE creating physics bodies
   clone->UpdateTransforms();

   // Now create physics bodies with properly computed transforms
   for (const Entity& e : clone->m_EntityList) {
      EntityID id = e.GetID();
      auto& data = clone->m_Entities[id];

      // Only create physics bodies for entities with colliders
      if (data.Collider && !data.CharacterController) {
         // Use WORLD scale for shape creation so parent scales are respected and avoid double-scaling
         glm::vec3 wpos, wscale, wskew; glm::vec4 wpersp; glm::quat wrot;
         glm::decompose(data.Transform.WorldMatrix, wscale, wrot, wpos, wskew, wpersp);
         data.Collider->BuildShape(data.Mesh && data.Mesh->mesh ? data.Mesh->mesh.get() : nullptr, glm::abs(wscale));
         clone->CreatePhysicsBody(id, data.Transform, *data.Collider);
      }
   }


   // Mark runtime clone as playing before any script OnCreate calls.
   clone->m_IsPlaying = true;

	// CRITICAL: Set CurrentScene to the clone BEFORE calling OnCreate
	// This ensures any entities created by scripts during OnCreate are added to the runtime scene
	Scene* previousScene = Scene::CurrentScene;
	Scene::CurrentScene = clone.get();
	
	// Clear managed component caches to prevent stale references from previous play sessions
	// The cache uses (entityID, Type) as key - since RuntimeClone preserves IDs, stale cached
	// components from a previous runtime scene would be returned instead of fresh ones
	if (g_ClearComponentCaches) {
		g_ClearComponentCaches();
	}

	// TWO-PHASE INITIALIZATION:
	// Phase 1: Apply property values and call Bind() on ALL scripts first
	// This registers all scripts in ScriptRegistry so GetScript<T>() works in OnCreate
	for (auto& entry : toInitialize) {
		ScriptInstance* scriptPtr = std::get<0>(entry);
		Entity& entity = std::get<1>(entry);
		(void)std::get<2>(entry); // scriptIndex unused in Phase 1
		if (scriptPtr && scriptPtr->Instance &&
			scriptPtr->Instance->GetBackend() == ScriptBackend::Managed) {
			auto managed = std::dynamic_pointer_cast<ManagedScriptComponent>(scriptPtr->Instance);
			if (managed && SetManagedFieldPtr) {
				void* handle = managed->GetHandle();
				auto& properties = ScriptReflection::GetScriptProperties(scriptPtr->ClassName);
				for (auto& property : properties) {
					// Prefer persisted per-entity value if present; otherwise fall back to default
					PropertyValue valueToApply = property.defaultValue;
					auto it = scriptPtr->Values.find(property.name);
					if (it != scriptPtr->Values.end()) valueToApply = it->second;
					switch (property.type) {
					case PropertyType::Int: {
						int v = std::get<int>(valueToApply);
						SetManagedFieldPtr(handle, property.name.c_str(), &v);
						break;
					}
					case PropertyType::Float: {
						float v = std::get<float>(valueToApply);
						SetManagedFieldPtr(handle, property.name.c_str(), &v);
						break;
					}
					case PropertyType::Bool: {
						bool v = std::get<bool>(valueToApply);
						SetManagedFieldPtr(handle, property.name.c_str(), &v);
						break;
					}
					case PropertyType::String: {
						const std::string& s = std::get<std::string>(valueToApply);
						const char* cstr = s.c_str();
						SetManagedFieldPtr(handle, property.name.c_str(), (void*)cstr);
						break;
					}
					case PropertyType::Vector3: {
						glm::vec3 v = std::get<glm::vec3>(valueToApply);
						SetManagedFieldPtr(handle, property.name.c_str(), &v);
						break;
					}
					case PropertyType::Entity: {
						int id = std::get<int>(valueToApply);
						SetManagedFieldPtr(handle, property.name.c_str(), &id);
						break;
					}
                    case PropertyType::ComponentRef: {
                        // Pass entity id; managed side resolves to specific component type
                        int id = std::get<int>(valueToApply);
                        SetManagedFieldPtr(handle, property.name.c_str(), &id);
                        break;
                    }
                    case PropertyType::ScriptRef: {
                        // Pass entity id; managed side resolves to specific script instance
                        int id = std::get<int>(valueToApply);
                        SetManagedFieldPtr(handle, property.name.c_str(), &id);
                        break;
                    }
                    case PropertyType::Enum: {
                        // Enum stored as int value
                        int v = std::get<int>(valueToApply);
                        SetManagedFieldPtr(handle, property.name.c_str(), &v);
                        break;
                    }
                    case PropertyType::Prefab: {
                        // Pass GUID string; managed side constructs Prefab struct
                        const std::string& guid = std::get<std::string>(valueToApply);
                        const char* cstr = guid.c_str();
                        SetManagedFieldPtr(handle, property.name.c_str(), (void*)cstr);
                        break;
                    }
                    case PropertyType::Mesh: {
                        // Pass "GUID:fileID" string; managed side constructs Mesh struct
                        const std::string& meshRef = std::get<std::string>(valueToApply);
                        const char* cstr = meshRef.c_str();
                        SetManagedFieldPtr(handle, property.name.c_str(), (void*)cstr);
                        break;
                    }
                    case PropertyType::Texture: {
                        const std::string& textureRef = std::get<std::string>(valueToApply);
                        const char* cstr = textureRef.c_str();
                        SetManagedFieldPtr(handle, property.name.c_str(), (void*)cstr);
                        break;
                    }
                    case PropertyType::Audio: {
                        const std::string& guid = std::get<std::string>(valueToApply);
                        const char* cstr = guid.c_str();
                        SetManagedFieldPtr(handle, property.name.c_str(), (void*)cstr);
                        break;
                    }
                    case PropertyType::ClayObject: {
                        // Pass GUID string; managed side loads ClayScriptableObject instance
                        const std::string& guid = std::get<std::string>(valueToApply);
                        const char* cstr = guid.c_str();
                        SetManagedFieldPtr(handle, property.name.c_str(), (void*)cstr);
                        break;
                    }
                    case PropertyType::DialogueLibrary: {
                        // Pass GUID string; managed side constructs DialogueLibraryRef struct
                        const std::string& guid = std::get<std::string>(valueToApply);
                        const char* cstr = guid.c_str();
                        SetManagedFieldPtr(handle, property.name.c_str(), (void*)cstr);
                        break;
                    }
                    case PropertyType::AnimatorController: {
                        // Pass controller path string; managed side constructs AnimationController struct
                        const std::string& path = std::get<std::string>(valueToApply);
                        const char* cstr = path.c_str();
                        SetManagedFieldPtr(handle, property.name.c_str(), (void*)cstr);
                        break;
                    }
                    case PropertyType::AnimatorControllerOverride: {
                        const std::string& path = std::get<std::string>(valueToApply);
                        const char* cstr = path.c_str();
                        SetManagedFieldPtr(handle, property.name.c_str(), (void*)cstr);
                        break;
                    }
                    case PropertyType::List: {
                        static thread_local std::string listSerialBuffer;
                        listSerialBuffer = ScriptReflection::PropertyValueToString(valueToApply);
                        SetManagedFieldPtr(handle, property.name.c_str(), (void*)listSerialBuffer.c_str());
                        break;
                    }
					}
				}
			}
		}
		// Call Bind to register script in ScriptRegistry (but NOT OnCreate yet)
		scriptPtr->Instance->OnBind(entity);
	}

	// Phase 2: Now call OnCreate on ALL scripts in sorted order - GetScript<T>() will work
	for (auto& entry : toInitialize) {
		ScriptInstance* scriptPtr = std::get<0>(entry);
		Entity& entity = std::get<1>(entry);
		scriptPtr->Instance->OnCreate(entity);
	}

   return clone;
}



void Scene::InvalidateAllAnimatorAssetCaches() {
   auto invalidateScene = [](Scene& targetScene) {
      for (const auto& entity : targetScene.GetEntities()) {
         auto* data = targetScene.GetEntityData(entity.GetID());
         if (!data || !data->AnimationPlayer) continue;

         auto& player = *data->AnimationPlayer;
         player.InvalidateAssetCache();
         player._FiredEventIds.clear();
         player._PrevEventStateId = -1;
         player._PrevEventStateTime = 0.0f;
         player.ResetRootMotionTracking();
      }
   };

   invalidateScene(*this);
   if (m_RuntimeScene && m_RuntimeScene.get() != this) {
      invalidateScene(*m_RuntimeScene);
   }
}

void Scene::OnStop() {
   // Mark that we're stopping to prevent any scripts from trying to destroy entities
   m_IsBeingDestroyed = true;

   // Stop any sounds this scene started. Audio handles live in the global Audio
   // system (not on the scene), so tearing the scene down won't stop them. OnStop
   // is the shared exit-play teardown for every route (toolbar Stop, command,
   // application), so stopping here covers them all.
   Audio::StopAll();

   // Collect entity IDs first to avoid issues if entities are modified during iteration
   std::vector<EntityID> entityIds;
   entityIds.reserve(m_Entities.size());
   for (const auto& [id, data] : m_Entities) {
      entityIds.push_back(id);
   }
   
   // Destroy bodies stored in component data (new system)
   for (EntityID id : entityIds) {
      auto* data = GetEntityData(id);
      if (!data) continue; // Entity was already destroyed
      
      // Character controllers: unregister inner body and release runtime object
      if (data->CharacterController && data->CharacterController->Character) {
         try {
            JPH::BodyID inner = data->CharacterController->Character->GetInnerBodyID();
            auto* areaSystem = Physics::Get().GetAreaSystem();
            if (areaSystem && !inner.IsInvalid()) {
               areaSystem->UnregisterCharacterInnerBody(inner);
            }
         } catch (...) {
            // Ignore errors during cleanup - we're shutting down
         }
         data->CharacterController->Character = nullptr;
         data->CharacterController->IsGrounded = false;
      }
      // Area sensor bodies
      if (data->Area) {
         auto* areaSystem = Physics::Get().GetAreaSystem();
         if (areaSystem) {
            try {
               areaSystem->OnDestroy(Entity(id, this), *data->Area);
            } catch (...) {
               // Ignore errors during cleanup - we're shutting down
            }
         }
      }
      // Dynamic / kinematic bodies
      if (data->RigidBody && !data->RigidBody->BodyID.IsInvalid()) {
         try {
            Physics::Get().DestroyBody(data->RigidBody->BodyID);
         } catch (...) {
            // Ignore errors during cleanup - we're shutting down
         }
         data->RigidBody->BodyID = JPH::BodyID();
      }
      // Static bodies
      if (data->StaticBody && !data->StaticBody->BodyID.IsInvalid()) {
         try {
            Physics::Get().DestroyBody(data->StaticBody->BodyID);
         } catch (...) {
            // Ignore errors during cleanup - we're shutting down
         }
         data->StaticBody->BodyID = JPH::BodyID();
      }
      if (data->Softbody) {
         SoftbodySystem::ReleaseRuntime(*data, true);
      }
   }

   // Stop and destroy particle emitters owned by this scene
   for (EntityID id : entityIds) {
      auto* data = GetEntityData(id);
      if (!data || !data->Emitter) continue;

      if (!data->Emitter->SpritePath.empty() && ps::isValid(data->Emitter->SpriteHandle)) {
         particles::ReleaseSprite(data->Emitter->SpriteHandle);
         data->Emitter->SpriteHandle = { uint16_t{UINT16_MAX} };
         data->Emitter->Uniforms.m_handle = { uint16_t{UINT16_MAX} };
      }
      if (ps::isValid(data->Emitter->Handle)) {
         ecs::ParticleEmitterSystem::Get().UnregisterEmitterOwnership(data->Emitter->Handle);
         ps::destroyEmitter(data->Emitter->Handle);
         data->Emitter->Handle = { uint16_t{UINT16_MAX} };
      }
      data->Emitter->IsPlaying = false;
      data->Emitter->HasEmitted = false;
      data->Emitter->ElapsedTime = 0.0f;
      data->Emitter->JustCreated = false;
   }
   ecs::ParticleEmitterSystem::Get().ClearSceneEmitters(this);

   // Destroy any bodies that are still tracked in the legacy map
   // Make a copy to avoid iterator invalidation
   std::vector<JPH::BodyID> legacyBodies;
   legacyBodies.reserve(m_BodyMap.size());
   for (const auto& kv : m_BodyMap) {
      if (!kv.second.IsInvalid()) {
         legacyBodies.push_back(kv.second);
      }
   }
   m_BodyMap.clear();
   
   for (JPH::BodyID bodyId : legacyBodies) {
      try {
         Physics::Get().DestroyBody(bodyId);
      } catch (...) {
         // Ignore errors during cleanup - we're shutting down
      }
   }
}

// --------------------------------------------------------
// RecreateScriptInstances: Recreate all script instances 
// after hot-reload to use types from the new assembly.
// --------------------------------------------------------
int Scene::RecreateScriptInstances() {
   int recreated = 0;
   
   for (auto& [id, data] : m_Entities) {
      for (auto& script : data.Scripts) {
         if (script.ClassName.empty()) continue;
         
         // Create a fresh instance from the current (reloaded) assembly
         auto created = ScriptSystem::Instance().Create(script.ClassName);
         if (!created) {
            std::cerr << "[Scene::RecreateScriptInstances] Failed to create: " 
                      << script.ClassName << " for entity " << data.Name << "\n";
            continue;
         }
         
         // Replace the old instance with the new one
         // The old GCHandle will be released when the shared_ptr destructor runs
         script.Instance = created;
         
         // Apply persisted property values to the new managed instance
         if (script.Instance->GetBackend() == ScriptBackend::Managed) {
            auto managed = std::dynamic_pointer_cast<ManagedScriptComponent>(script.Instance);
            if (managed && SetManagedFieldPtr) {
               void* handle = managed->GetHandle();
               auto& properties = ScriptReflection::GetScriptProperties(script.ClassName);
               for (auto& property : properties) {
                  // Prefer persisted per-entity value if present; otherwise fall back to default
                  PropertyValue valueToApply = property.defaultValue;
                  auto it = script.Values.find(property.name);
                  if (it != script.Values.end()) valueToApply = it->second;
                  
                  // Apply value using the same logic as RuntimeClone
                  switch (property.type) {
                  case PropertyType::Int: {
                     int v = std::get<int>(valueToApply);
                     SetManagedFieldPtr(handle, property.name.c_str(), &v);
                     break;
                  }
                  case PropertyType::Float: {
                     float v = std::get<float>(valueToApply);
                     SetManagedFieldPtr(handle, property.name.c_str(), &v);
                     break;
                  }
                  case PropertyType::Bool: {
                     bool v = std::get<bool>(valueToApply);
                     SetManagedFieldPtr(handle, property.name.c_str(), &v);
                     break;
                  }
                  case PropertyType::String: {
                     const std::string& s = std::get<std::string>(valueToApply);
                     const char* cstr = s.c_str();
                     SetManagedFieldPtr(handle, property.name.c_str(), (void*)cstr);
                     break;
                  }
                  case PropertyType::Vector3: {
                     glm::vec3 v = std::get<glm::vec3>(valueToApply);
                     SetManagedFieldPtr(handle, property.name.c_str(), &v);
                     break;
                  }
                  case PropertyType::Entity: {
                     int eid = std::get<int>(valueToApply);
                     SetManagedFieldPtr(handle, property.name.c_str(), &eid);
                     break;
                  }
                  case PropertyType::ComponentRef:
                  case PropertyType::ScriptRef: {
                     int eid = std::get<int>(valueToApply);
                     SetManagedFieldPtr(handle, property.name.c_str(), &eid);
                     break;
                  }
                  case PropertyType::Enum: {
                     int v = std::get<int>(valueToApply);
                     SetManagedFieldPtr(handle, property.name.c_str(), &v);
                     break;
                  }
                  case PropertyType::Prefab:
                  case PropertyType::Mesh:
                  case PropertyType::Texture:
                  case PropertyType::Audio:
                  case PropertyType::ClayObject:
                  case PropertyType::DialogueLibrary: {
                     const std::string& guid = std::get<std::string>(valueToApply);
                     const char* cstr = guid.c_str();
                     SetManagedFieldPtr(handle, property.name.c_str(), (void*)cstr);
                     break;
                  }
                  case PropertyType::List: {
                     // Lists may serialize as legacy pipe strings or structured JSON.
                     static thread_local std::string listSerialBuffer;
                     listSerialBuffer = ScriptReflection::PropertyValueToString(valueToApply);
                     SetManagedFieldPtr(handle, property.name.c_str(), (void*)listSerialBuffer.c_str());
                     break;
                  }
                  default:
                     break;
                  }
               }
            }
         }
         
         // Re-invoke OnCreate for the new instance
         if (m_IsPlaying) {
            script.Instance->OnCreate(Entity(id, this));
         }
         
         ++recreated;
      }
   }
   
   std::cout << "[Scene] Recreated " << recreated << " script instances after hot-reload\n";
   return recreated;
}

EntityID Scene::DuplicateEntity(EntityID srcRoot) {
   auto* src = GetEntityData(srcRoot);
   if (!src)
   {
      return -1;
   }

   std::unordered_map<EntityID, EntityID> remap;
   std::vector<EntityID> stack; stack.push_back(srcRoot);
   std::vector<EntityID> order;
   while (!stack.empty()) {
      EntityID id = stack.back(); stack.pop_back();
      order.push_back(id);
      if (auto* d = GetEntityData(id))
      {
         for (auto it = d->Children.rbegin(); it != d->Children.rend(); ++it)
            stack.push_back(*it);
      }
   }

   for (EntityID oldId : order) {
      auto* d = GetEntityData(oldId);
      if (!d)
      {
         continue;
      }
      std::string baseName = d->Name.empty() ? std::string("Entity") : d->Name;
      Entity newEnt = CreateEntity(baseName);
      EntityID newId = newEnt.GetID();
      remap[oldId] = newId;
      m_Entities[newId] = d->DeepCopy(newId, this);
      auto& nd = m_Entities[newId];
      nd.Parent = INVALID_ENTITY_ID;
      nd.Children.clear();
      MarkTransformDirty(newId);
   }

   for (EntityID oldId : order) {
      auto* d = GetEntityData(oldId);
      if (!d)
      {
         continue;
      }
      EntityID newId = remap[oldId];
      if (d->Parent != INVALID_ENTITY_ID)
      {
         auto it = remap.find(d->Parent);
         if (it != remap.end())
         {
            SetParent(newId, it->second);
         }
      }
      for (EntityID oldChild : d->Children)
      {
         auto itc = remap.find(oldChild);
         if (itc != remap.end())
         {
            SetParent(itc->second, newId);
         }
      }
   }

   EntityID newRoot = remap[srcRoot];
   if (auto* nd = GetEntityData(newRoot)) {
      nd->Transform.Position += glm::vec3(0.0f, 0.0f, 0.5f);
      MarkTransformDirty(newRoot);
   }

   if (m_IsPlaying) {
      std::vector<EntityID> stack;
      stack.push_back(newRoot);
      std::vector<EntityID> order;
      while (!stack.empty()) {
         EntityID id = stack.back();
         stack.pop_back();
         order.push_back(id);
         if (auto* d = GetEntityData(id)) {
            for (auto it = d->Children.rbegin(); it != d->Children.rend(); ++it) {
               stack.push_back(*it);
            }
         }
      }

      std::vector<std::tuple<ScriptInstance*, Entity, size_t>> toInitialize;
      toInitialize.reserve(order.size());

      for (EntityID id : order) {
         auto* data = GetEntityData(id);
         if (!data) continue;
         Entity entity(id, this);
         for (size_t i = 0; i < data->Scripts.size(); ++i) {
            auto& script = data->Scripts[i];
            if (!script.Instance) continue;
            toInitialize.emplace_back(&script, entity, i);
         }
      }

      std::sort(toInitialize.begin(), toInitialize.end(), [](const auto& a, const auto& b) {
         ScriptInstance* sa = std::get<0>(a);
         ScriptInstance* sb = std::get<0>(b);
         const Entity& ea = std::get<1>(a);
         const Entity& eb = std::get<1>(b);
         size_t ia = std::get<2>(a), ib = std::get<2>(b);
         int pa = ScriptSystem::Instance().GetScriptPriority(sa->ClassName);
         int pb = ScriptSystem::Instance().GetScriptPriority(sb->ClassName);
         uint64_t ta = ScriptOrder::StableHashScriptClassName(sa->ClassName);
         uint64_t tb = ScriptOrder::StableHashScriptClassName(sb->ClassName);
         return ScriptOrder::OrderLess(pa, ta, ea.GetID(), ia, pb, tb, eb.GetID(), ib);
      });

      for (auto& entry : toInitialize) {
         ScriptInstance* scriptPtr = std::get<0>(entry);
         Entity& entity = std::get<1>(entry);
         if (scriptPtr && scriptPtr->Instance &&
             scriptPtr->Instance->GetBackend() == ScriptBackend::Managed) {
            auto managed = std::dynamic_pointer_cast<ManagedScriptComponent>(scriptPtr->Instance);
            if (managed && SetManagedFieldPtr) {
               void* handle = managed->GetHandle();
               auto& properties = ScriptReflection::GetScriptProperties(scriptPtr->ClassName);
               for (auto& property : properties) {
                  PropertyValue valueToApply = property.defaultValue;
                  auto it = scriptPtr->Values.find(property.name);
                  if (it != scriptPtr->Values.end()) valueToApply = it->second;
                  switch (property.type) {
                  case PropertyType::Int: {
                     int v = std::get<int>(valueToApply);
                     SetManagedFieldPtr(handle, property.name.c_str(), &v);
                     break;
                  }
                  case PropertyType::Float: {
                     float v = std::get<float>(valueToApply);
                     SetManagedFieldPtr(handle, property.name.c_str(), &v);
                     break;
                  }
                  case PropertyType::Bool: {
                     bool v = std::get<bool>(valueToApply);
                     SetManagedFieldPtr(handle, property.name.c_str(), &v);
                     break;
                  }
                  case PropertyType::String: {
                     const std::string& s = std::get<std::string>(valueToApply);
                     const char* cstr = s.c_str();
                     SetManagedFieldPtr(handle, property.name.c_str(), (void*)cstr);
                     break;
                  }
                  case PropertyType::Vector3: {
                     glm::vec3 v = std::get<glm::vec3>(valueToApply);
                     SetManagedFieldPtr(handle, property.name.c_str(), &v);
                     break;
                  }
                  case PropertyType::Entity: {
                     int id = std::get<int>(valueToApply);
                     SetManagedFieldPtr(handle, property.name.c_str(), &id);
                     break;
                  }
                  case PropertyType::ComponentRef: {
                     int id = std::get<int>(valueToApply);
                     SetManagedFieldPtr(handle, property.name.c_str(), &id);
                     break;
                  }
                  case PropertyType::ScriptRef: {
                     int id = std::get<int>(valueToApply);
                     SetManagedFieldPtr(handle, property.name.c_str(), &id);
                     break;
                  }
                  case PropertyType::Enum: {
                     int v = std::get<int>(valueToApply);
                     SetManagedFieldPtr(handle, property.name.c_str(), &v);
                     break;
                  }
                  case PropertyType::Prefab: {
                     const std::string& guid = std::get<std::string>(valueToApply);
                     const char* cstr = guid.c_str();
                     SetManagedFieldPtr(handle, property.name.c_str(), (void*)cstr);
                     break;
                  }
                  case PropertyType::Mesh: {
                     const std::string& meshRef = std::get<std::string>(valueToApply);
                     const char* cstr = meshRef.c_str();
                     SetManagedFieldPtr(handle, property.name.c_str(), (void*)cstr);
                     break;
                  }
                  case PropertyType::Texture: {
                     const std::string& textureRef = std::get<std::string>(valueToApply);
                     const char* cstr = textureRef.c_str();
                     SetManagedFieldPtr(handle, property.name.c_str(), (void*)cstr);
                     break;
                  }
                  case PropertyType::Audio: {
                     const std::string& guid = std::get<std::string>(valueToApply);
                     const char* cstr = guid.c_str();
                     SetManagedFieldPtr(handle, property.name.c_str(), (void*)cstr);
                     break;
                  }
                  case PropertyType::ClayObject: {
                     const std::string& guid = std::get<std::string>(valueToApply);
                     const char* cstr = guid.c_str();
                     SetManagedFieldPtr(handle, property.name.c_str(), (void*)cstr);
                     break;
                  }
                  case PropertyType::DialogueLibrary: {
                     const std::string& guid = std::get<std::string>(valueToApply);
                     const char* cstr = guid.c_str();
                     SetManagedFieldPtr(handle, property.name.c_str(), (void*)cstr);
                     break;
                  }
                  case PropertyType::AnimatorController: {
                     const std::string& path = std::get<std::string>(valueToApply);
                     const char* cstr = path.c_str();
                     SetManagedFieldPtr(handle, property.name.c_str(), (void*)cstr);
                     break;
                  }
                  case PropertyType::AnimatorControllerOverride: {
                     const std::string& path = std::get<std::string>(valueToApply);
                     const char* cstr = path.c_str();
                     SetManagedFieldPtr(handle, property.name.c_str(), (void*)cstr);
                     break;
                  }
                  case PropertyType::List: {
                     static thread_local std::string listSerialBuffer;
                     listSerialBuffer = ScriptReflection::PropertyValueToString(valueToApply);
                     SetManagedFieldPtr(handle, property.name.c_str(), (void*)listSerialBuffer.c_str());
                     break;
                  }
                  default:
                     break;
                  }
               }
            }
         }

         scriptPtr->Instance->OnBind(entity);
      }

      if (DeferredScriptInit::g_EnableDeferredScriptInit) {
         for (auto& entry : toInitialize) {
            Entity& entity = std::get<1>(entry);
            size_t scriptIndex = std::get<2>(entry);
            DeferredScriptInit::QueueScriptOnCreate(this, entity.GetID(), scriptIndex);
         }
         if (!m_DeferScriptOnCreate) {
            DeferredScriptInit::ProcessDeferredScripts();
         }
      } else {
         for (auto& entry : toInitialize) {
            ScriptInstance* scriptPtr = std::get<0>(entry);
            Entity& entity = std::get<1>(entry);
            scriptPtr->Instance->OnCreate(entity);
         }
      }
   }

   return newRoot;
}

void Scene::SetEntityVisible(EntityID id, bool visible) {
   auto* data = GetEntityData(id);
   if (!data) return;
   
   data->Visible = visible;
   
   // Propagate visibility to all descendants
   std::function<void(EntityID)> setChildrenVisible = [&](EntityID parentId) {
      auto* parentData = GetEntityData(parentId);
      if (!parentData) return;
      
      for (EntityID childId : parentData->Children) {
         auto* childData = GetEntityData(childId);
         if (childData) {
            // Set entity-level visibility (checked first in rendering)
            childData->Visible = visible;
            
            // Also set component-level visibility flags for UI components
            if (childData->Panel) {
               childData->Panel->Visible = visible;
            }
            if (childData->Text) {
               childData->Text->Visible = visible;
            }
            if (childData->ProgressBar) {
               childData->ProgressBar->Visible = visible;
            }
            if (childData->Toggle) {
               childData->Toggle->Visible = visible;
            }
            if (childData->ScrollView) {
               childData->ScrollView->Visible = visible;
            }
            if (childData->InputField) {
               childData->InputField->Visible = visible;
            }
            if (childData->Dropdown) {
               childData->Dropdown->Visible = visible;
            }
            
            // Recursively set visibility on grandchildren
            setChildrenVisible(childId);
         }
      }
   };
   setChildrenVisible(id);
   MarkRuntimeWorldDirtySubtree(id, cm::world::RuntimeDirtyBits::Visibility);
}

void Scene::SetEntityPresentationHidden(EntityID id, bool hidden) {
   auto* data = GetEntityData(id);
   if (!data) {
      return;
   }

   data->PresentationHidden = hidden;

   std::function<void(EntityID)> setChildrenHidden = [&](EntityID parentId) {
      auto* parentData = GetEntityData(parentId);
      if (!parentData) {
         return;
      }

      for (EntityID childId : parentData->Children) {
         auto* childData = GetEntityData(childId);
         if (!childData) {
            continue;
         }
         childData->PresentationHidden = hidden;
         setChildrenHidden(childId);
      }
   };

   setChildrenHidden(id);
   MarkRuntimeWorldDirtySubtree(id, cm::world::RuntimeDirtyBits::Visibility);
}

void Scene::SetEntityVisibleDirect(EntityID id, bool visible) {
   auto* data = GetEntityData(id);
   if (!data || data->Visible == visible) {
      return;
   }

   data->Visible = visible;
   MarkRuntimeWorldDirty(id, cm::world::RuntimeDirtyBits::Visibility);
}

void Scene::SetEntityActive(EntityID id, bool active) {
   auto* data = GetEntityData(id);
   if (!data || data->Active == active) {
      return;
   }

   data->Active = active;
   MarkRuntimeWorldDirty(id, cm::world::RuntimeDirtyBits::Visibility);
}

void Scene::SetEntityName(EntityID id, const std::string& name) {
   auto* data = GetEntityData(id);
   if (!data || data->Name == name) {
      return;
   }

   data->Name = name;
   MarkRuntimeWorldDirty(id, cm::world::RuntimeDirtyBits::Metadata);
}

void Scene::SetEntityTag(EntityID id, const std::string& tag) {
   auto* data = GetEntityData(id);
   if (!data || data->Tag == tag) {
      return;
   }

   data->Tag = tag;
   MarkRuntimeWorldDirty(id, cm::world::RuntimeDirtyBits::Metadata);
}

void Scene::SetEntityLayer(EntityID id, int layer) {
   auto* data = GetEntityData(id);
   if (!data || data->Layer == layer) {
      return;
   }

   data->Layer = layer;
   MarkRuntimeWorldDirty(id, cm::world::RuntimeDirtyBits::Metadata);
}

void Scene::SetMeshBoundsPadding(EntityID id, float boundsPadding) {
   auto* data = GetEntityData(id);
   if (!data || !data->Mesh) {
      return;
   }

   if (std::abs(data->Mesh->BoundsPadding - boundsPadding) <= 1e-6f) {
      return;
   }

   data->Mesh->BoundsPadding = boundsPadding;
   MarkRuntimeWorldDirty(id, cm::world::RuntimeDirtyBits::Bounds);
}

void Scene::SetMeshSkipFrustumCulling(EntityID id, bool skipFrustumCulling) {
   auto* data = GetEntityData(id);
   if (!data || !data->Mesh || data->Mesh->SkipFrustumCulling == skipFrustumCulling) {
      return;
   }

   data->Mesh->SkipFrustumCulling = skipFrustumCulling;
   MarkRuntimeWorldDirty(id, cm::world::RuntimeDirtyBits::RenderBinding);
}

namespace {

void ClearAreaOverlapState(cm::physics::AreaComponent& area) {
   std::lock_guard<std::mutex> lock(area.Mutex);
   area.OverlappingBodies.clear();
   area.OverlappingAreas.clear();
}

void ReleaseCharacterControllerRuntime(EntityData& data, cm::physics::AreaSystem* areaSystem) {
   if (!data.CharacterController || !data.CharacterController->Character) {
      return;
   }

   try {
      JPH::BodyID inner = data.CharacterController->Character->GetInnerBodyID();
      if (areaSystem && !inner.IsInvalid()) {
         areaSystem->UnregisterCharacterInnerBody(inner);
      }
   } catch (...) {
   }

   data.CharacterController->Character = nullptr;
   data.CharacterController->IsGrounded = false;
   data.CharacterController->DesiredVelocity = glm::vec3(0.0f);
   data.CharacterController->VerticalVelocity = 0.0f;
   data.CharacterController->JumpRequested = false;
#if !defined(CLAYMORE_RUNTIME)
   data.CharacterController->_EditorDisplayDesiredVelocity = glm::vec3(0.0f);
#endif
}

void ReleaseAreaRuntime(Scene& scene, EntityID id, EntityData& data, cm::physics::AreaSystem* areaSystem) {
   if (!data.Area) {
      return;
   }

   if (data.Area->Body != nullptr) {
      try {
         if (areaSystem) {
            areaSystem->OnDestroy(Entity(id, &scene), *data.Area);
         } else {
            Physics::Get().DestroyBody(data.Area->Body->GetID());
            data.Area->Body = nullptr;
         }
      } catch (...) {
         data.Area->Body = nullptr;
      }
   }

   ClearAreaOverlapState(*data.Area);
}

} // namespace

void Scene::DestroyPhysicsBody(EntityID id) {
   auto* data = GetEntityData(id);
   if (!data)
   {
      return;
   }

   auto* areaSystem = Physics::Get().GetAreaSystem();
   ReleaseCharacterControllerRuntime(*data, areaSystem);
   ReleaseAreaRuntime(*this, id, *data, areaSystem);

   bool destroyedAnyBody = false;

   auto destroyBody = [&](JPH::BodyID& bodyID) {
      if (bodyID.IsInvalid()) {
         return;
      }

      Physics::Get().DestroyBody(bodyID);
      bodyID = JPH::BodyID();
      destroyedAnyBody = true;
   };

   if (data->RigidBody) {
      destroyBody(data->RigidBody->BodyID);
   }

   if (data->StaticBody) {
      destroyBody(data->StaticBody->BodyID);
   }

   if (data->Softbody) {
      destroyBody(data->Softbody->BodyID);
   }

   auto it = m_BodyMap.find(id);
   if (it != m_BodyMap.end()) {
      destroyBody(it->second);
      m_BodyMap.erase(it);
   }

   if (destroyedAnyBody) {
      std::cout << "[Scene] Destroyed physics body for Entity " << id << std::endl;
   }

   if (data->Softbody) {
      SoftbodySystem::ReleaseRuntime(*data, false);
   }
}

void Scene::ReleasePhysicsRuntimeState() {
   std::vector<EntityID> entityIds;
   entityIds.reserve(m_Entities.size());
   for (const auto& [id, data] : m_Entities) {
      (void)data;
      entityIds.push_back(id);
   }

   auto* areaSystem = Physics::Get().GetAreaSystem();

   for (EntityID id : entityIds) {
      auto* data = GetEntityData(id);
      if (!data) continue;

      ReleaseCharacterControllerRuntime(*data, areaSystem);
      ReleaseAreaRuntime(*this, id, *data, areaSystem);

      // Dynamic / kinematic bodies.
      if (data->RigidBody && !data->RigidBody->BodyID.IsInvalid()) {
         try {
            Physics::Get().DestroyBody(data->RigidBody->BodyID);
         } catch (...) {
         }
         data->RigidBody->BodyID = JPH::BodyID();
      }

      // Static bodies.
      if (data->StaticBody && !data->StaticBody->BodyID.IsInvalid()) {
         try {
            Physics::Get().DestroyBody(data->StaticBody->BodyID);
         } catch (...) {
         }
         data->StaticBody->BodyID = JPH::BodyID();
      }
      if (data->Softbody) {
         SoftbodySystem::ReleaseRuntime(*data, true);
      }
   }

   // Legacy fallback bodies.
   std::vector<JPH::BodyID> legacyBodies;
   legacyBodies.reserve(m_BodyMap.size());
   for (const auto& kv : m_BodyMap) {
      if (!kv.second.IsInvalid()) {
         legacyBodies.push_back(kv.second);
      }
   }
   m_BodyMap.clear();
   for (JPH::BodyID bodyId : legacyBodies) {
      try {
         Physics::Get().DestroyBody(bodyId);
      } catch (...) {
      }
   }
}



void Scene::CreatePhysicsBody(EntityID id, const TransformComponent& transform, const ColliderComponent& collider) {
   if (!collider.Shape) {
      std::cerr << "[Scene] Cannot create physics body: shape is null\n";
      return;
   }

   auto* data = GetEntityData(id);
   if (!data)
   {
      return;
   }

   const bool wantsComponentBody = data->RigidBody || data->StaticBody;

   // Check if a physics body already exists for this entity
   if ((data->RigidBody && !data->RigidBody->BodyID.IsInvalid()) ||
      (data->StaticBody && !data->StaticBody->BodyID.IsInvalid())) {
      return;
   }

   auto legacyBodyIt = m_BodyMap.find(id);
   if (legacyBodyIt != m_BodyMap.end()) {
      if (!wantsComponentBody) {
         return;
      }

      if (!legacyBodyIt->second.IsInvalid()) {
         Physics::Get().DestroyBody(legacyBodyIt->second);
      }
      m_BodyMap.erase(legacyBodyIt);
   }

   // Extract world pose from WorldMatrix for body creation (includes parent transforms).
   glm::vec3 worldPos(0.0f);
   glm::quat rot(1.0f, 0.0f, 0.0f, 0.0f);
   DecomposeWorldTransform(transform, worldPos, rot);
   glm::vec3 pos = worldPos + TransformColliderOffsetToWorld(transform, collider.Offset);

   // Convert to Jolt types
   JPH::RVec3 joltPosition(pos.x, pos.y, pos.z);
   JPH::Quat joltRotation(rot.x, rot.y, rot.z, rot.w);

   // Determine motion type
   JPH::EMotionType motionType = JPH::EMotionType::Static;
   if (data->RigidBody) {
      motionType = data->RigidBody->IsKinematic
         ? JPH::EMotionType::Kinematic
         : JPH::EMotionType::Dynamic;
   }

   // Use the motion component's layer when present. Collider-only query bodies
   // use the collider layer so trigger/marker colliders can be raycast by mask.
   uint8_t objectLayer = 0;
   std::string layerName = "Default";
   if (data->RigidBody) {
      objectLayer = static_cast<uint8_t>(data->RigidBody->PhysicsLayer);
      layerName = data->RigidBody->PhysicsLayerName;
   } else if (data->StaticBody) {
      objectLayer = static_cast<uint8_t>(data->StaticBody->PhysicsLayer);
      layerName = data->StaticBody->PhysicsLayerName;
   } else {
      objectLayer = static_cast<uint8_t>(collider.PhysicsLayer);
      layerName = collider.PhysicsLayerName;
   }

   JPH::BodyCreationSettings settings(collider.Shape, joltPosition, joltRotation, motionType, objectLayer);
   // Set friction: prefer RigidBody value, fall back to StaticBody, otherwise default
   if (data->RigidBody)
      settings.mFriction = data->RigidBody->Friction;
   else if (data->StaticBody)
      settings.mFriction = data->StaticBody->Friction;
   else
      settings.mFriction = 0.5f;
   settings.mRestitution = data->RigidBody ? data->RigidBody->Restitution : data->StaticBody ? data->StaticBody->Restitution : 0.0f;
   settings.mAllowSleeping = true;
   settings.mIsSensor = collider.IsTrigger;
   if (data->RigidBody && data->RigidBody->IsKinematic) {
      // Required for kinematic bodies to emit contacts against static / non-dynamic bodies.
      settings.mCollideKinematicVsNonDynamic = true;
   }

   if (data->RigidBody) {
      settings.mMotionQuality = JPH::EMotionQuality::LinearCast; // Optional: for fast-moving objects
      settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
      settings.mMassPropertiesOverride.mMass = std::max(data->RigidBody->Mass, 1e-6f);
      // Respect gravity toggle on creation
      settings.mGravityFactor = data->RigidBody->UseGravity ? 1.0f : 0.0f;
   }

   JPH::BodyInterface& bodyInterface = Physics::Get().GetBodyInterface();
   JPH::Body* body = bodyInterface.CreateBody(settings);

   if (!body) {
      std::cerr << "[Scene] Failed to create Jolt body for Entity " << id << std::endl;
      return;
   }

   // Tag the body with our EntityID so systems like Area can resolve the entity
   body->SetUserData((uint64_t)id);

   JPH::BodyID bodyID = body->GetID();
   bodyInterface.AddBody(bodyID, JPH::EActivation::Activate);

   // Store the BodyID
   if (data->RigidBody) {
      data->RigidBody->BodyID = bodyID;
      data->RigidBody->_LastAppliedGravityFactor = data->RigidBody->UseGravity ? 1.0f : 0.0f;
      ApplyPendingRigidBodyCommands(*data->RigidBody);
   }
   else if (data->StaticBody) {
      data->StaticBody->BodyID = bodyID;
   }
   else {
      m_BodyMap[id] = bodyID; // Fallback
   }
   
   // Initialize last-known collider parameters for runtime shape change detection
   if (data->Collider) {
      data->Collider->_LastShapeType = data->Collider->ShapeType;
      data->Collider->_LastSize = data->Collider->Size;
      data->Collider->_LastRadius = data->Collider->Radius;
      data->Collider->_LastHeight = data->Collider->Height;
      data->Collider->_LastOffset = data->Collider->Offset;
      glm::vec3 wscale(1.0f);
      glm::vec3 wpos, wskew;
      glm::vec4 wpersp;
      glm::quat wrot;
      glm::decompose(transform.WorldMatrix, wscale, wrot, wpos, wskew, wpersp);
      data->Collider->_LastWorldScale = glm::abs(wscale);
   }
}

uint64_t Scene::SyncPhysicsBodiesFromSceneTransforms(bool includeDynamicRigidBodies) {
   if (!Physics::GetSystem()) {
      return 0;
   }

   uint64_t syncedCount = 0;
   for (auto& [entityId, data] : m_Entities) {
      JPH::BodyID bodyId = JPH::BodyID();
      bool hasRigidBody = false;
      bool dynamicRigidBody = false;

      if (data.RigidBody && !data.RigidBody->BodyID.IsInvalid()) {
         hasRigidBody = true;
         dynamicRigidBody = !data.RigidBody->IsKinematic;
         if (dynamicRigidBody &&
            (!includeDynamicRigidBodies || data.Parent == INVALID_ENTITY_ID)) {
            continue;
         }
         bodyId = data.RigidBody->BodyID;
      } else if (data.StaticBody && !data.StaticBody->BodyID.IsInvalid()) {
         bodyId = data.StaticBody->BodyID;
      } else {
         auto legacyBodyIt = m_BodyMap.find(entityId);
         if (legacyBodyIt != m_BodyMap.end()) {
            bodyId = legacyBodyIt->second;
         }
      }

      if (bodyId.IsInvalid()) {
         continue;
      }

      const JPH::EActivation activation =
         hasRigidBody ? JPH::EActivation::Activate : JPH::EActivation::DontActivate;
      const bool resetRigidBodyVelocity =
         dynamicRigidBody && data.Parent != INVALID_ENTITY_ID;
      if (SyncPhysicsBodyToSceneTransform(data, bodyId, activation, resetRigidBodyVelocity)) {
         ++syncedCount;
      }
   }

   return syncedCount;
}

bool Scene::SyncPhysicsBodyFromSceneTransform(EntityID id, bool resetRigidBodyVelocity) {
   if (!Physics::GetSystem()) {
      return false;
   }

   auto* data = GetEntityData(id);
   if (!data) {
      return false;
   }

   JPH::BodyID bodyId = JPH::BodyID();
   bool hasRigidBody = false;
   if (data->RigidBody && !data->RigidBody->BodyID.IsInvalid()) {
      hasRigidBody = true;
      bodyId = data->RigidBody->BodyID;
   } else if (data->StaticBody && !data->StaticBody->BodyID.IsInvalid()) {
      bodyId = data->StaticBody->BodyID;
   } else {
      auto legacyBodyIt = m_BodyMap.find(id);
      if (legacyBodyIt != m_BodyMap.end()) {
         bodyId = legacyBodyIt->second;
      }
   }

   if (bodyId.IsInvalid()) {
      return false;
   }

   const JPH::EActivation activation =
      hasRigidBody ? JPH::EActivation::Activate : JPH::EActivation::DontActivate;
   return SyncPhysicsBodyToSceneTransform(*data, bodyId, activation, resetRigidBodyVelocity);
}


///--------------------------------------------------------------------------------------------------------
/// Update: Master scene update method
///--------------------------------------------------------------------------------------------------------
void Scene::Update(float dt) {

   // Profiling
   ScopedTimer tScene("Scene/Update Total");
   m_RuntimeWorldFrameSyncLocked = false;
#ifdef CM_DEBUG_SCENE_THREAD
   static bool once = (std::cout << "[C++] Scene::Update thread: " << GetCurrentThreadId() << "\n", true);
#endif


   if (m_PendingSceneLoad) {
      std::string pending = m_PendingSceneLoadPath;
      bool async = m_PendingSceneLoadAsync;
      m_PendingSceneLoad = false;
      m_PendingSceneLoadAsync = false;
      m_PendingSceneLoadPath.clear();
      if (async) {
         StartAsyncLoadJob(pending);
      } else {
         LoadSceneImmediate(pending, false);
      }
   }

   // Ensure any queued deletions are processed at a safe point each frame
   ProcessPendingRemovals();
   
   if (m_AsyncLoadJob) {
      TickAsyncLoadJob();
   }

   if (m_Loading && m_DeferScriptOnCreate && m_LoadScriptsReady) {
      { ScopedTimer t("Prefab/PrewarmLoad"); prefab::Update(4.0); }
      if (prefab::HasPendingWork()) {
         const float prewarmRatio = prefab::GetProgress();
         const float prewarmSpan = (m_LoadScriptsTotal > 0) ? 0.075f : 0.15f;
         m_LoadProgress = 0.85f + (prewarmSpan * prewarmRatio);
      } else {
         static constexpr size_t kMaxScriptsPerFrame = 64;
         size_t processed = DeferredScriptInit::ProcessDeferredScripts(kMaxScriptsPerFrame);
         if (processed > 0 && m_LoadScriptsTotal > 0) {
            m_LoadScriptsProcessed = std::min(m_LoadScriptsProcessed + processed, m_LoadScriptsTotal);
            float ratio = static_cast<float>(m_LoadScriptsProcessed) / static_cast<float>(m_LoadScriptsTotal);
            m_LoadProgress = 0.925f + (0.075f * ratio);
         }
         if (!DeferredScriptInit::HasPendingScripts()) {
            FinalizeLoadTracking(true);
         }
      }
   }

   if (m_LoadingHold) {
      const auto now = std::chrono::steady_clock::now();
      const float elapsed = std::chrono::duration<float>(now - m_LoadHoldStartTime).count();
      const float t = (m_LoadHoldDuration <= 0.0f)
         ? 1.0f
         : std::min(elapsed / m_LoadHoldDuration, 1.0f);
      m_LoadProgress = m_LoadHoldStartProgress + (1.0f - m_LoadHoldStartProgress) * t;
      if (t >= 1.0f) {
         m_LoadProgress = 1.0f;
         m_LoadingHold = false;
         m_Loading = false;
      }
   }

   constexpr float kScriptLodNearSq = 20.0f * 20.0f;
   constexpr float kScriptLodMediumSq = 50.0f * 50.0f;
   constexpr float kScriptLodFarSq = 100.0f * 100.0f;
   std::vector<EntityID> cachedScriptEntityIds;
   const std::vector<EntityID>* scriptEntityIds = nullptr;
   std::unordered_map<EntityID, bool> scriptEntityShouldUpdate;
   uint64_t scriptsLodEligible = 0;

   // Prefab prewarm (non-blocking, time-budgeted)
  { ScopedTimer t("Prefab/Prewarm"); prefab::Update(2.0); }
   
   // Async prefab instantiation (non-blocking, time-budgeted)
  { ScopedTimer t("Prefab/UpdateAsync"); runtime::RuntimePrefabInstantiator::UpdateAsync(2.0); }

   if (m_IsPlaying) {
      ScopedTimer t("Multiplayer/PreUpdate");
      cm::multiplayer::PreUpdate(dt);
   }

   if (m_IsPlaying) {
      {
         ScopedTimer t("Managed/FrameUpdate");
         cm::script::UpdateManagedFrame(dt);
      }

      {
         ScopedTimer t("NpcScalability");
         cm::npc::UpdateScalability(*this, dt);
      }

      if (m_RuntimeWorldBridge) {
         scriptEntityIds = &m_RuntimeWorldBridge->GetScriptedSceneEntities();
      } else {
         cachedScriptEntityIds.reserve(m_Entities.size());
         for (const auto& [id, data] : m_Entities) {
            if (!data.Scripts.empty()) {
               cachedScriptEntityIds.push_back(id);
            }
         }
         scriptEntityIds = &cachedScriptEntityIds;
      }

      glm::vec3 scriptLodCameraPos(0.0f);
      if (Camera* cam = GetActiveCamera()) {
         scriptLodCameraPos = cam->GetPosition();
      }

      if (scriptEntityIds) {
         scriptEntityShouldUpdate.reserve(scriptEntityIds->size());
         for (EntityID id : *scriptEntityIds) {
            auto it = m_Entities.find(id);
            if (it == m_Entities.end()) {
               continue;
            }

            auto& data = it->second;
            bool shouldUpdateScripts = true;
            const bool useScriptLod =
               !data.ScriptLodForceDisabled &&
               (data.ScriptLodEnabled ||
                data.AnimationPlayer != nullptr ||
                data.NavAgent != nullptr ||
                data.Skeleton != nullptr);
            if (useScriptLod) {
               ++scriptsLodEligible;
               float updateInterval = data.NpcScalability.ScriptUpdateInterval;
               data.ScriptLodLastDistance = data.NpcScalability.CameraDistance;

               if (updateInterval <= 0.0f && GetActiveCamera()) {
                  glm::vec3 entityPos = glm::vec3(data.Transform.WorldMatrix[3]);
                  float distSq = glm::dot(entityPos - scriptLodCameraPos, entityPos - scriptLodCameraPos);
                  data.ScriptLodLastDistance = std::sqrt(distSq);
                  if (distSq < kScriptLodNearSq) {
                     updateInterval = 0.0f;
                  } else if (distSq < kScriptLodMediumSq) {
                     updateInterval = 0.033f;
                  } else if (distSq < kScriptLodFarSq) {
                     updateInterval = 0.067f;
                  } else {
                     updateInterval = 0.133f;
                  }
               }

               data.ScriptLodAccumulatedTime += dt;
               if (data.ScriptLodAccumulatedTime < updateInterval) {
                  shouldUpdateScripts = false;
               } else {
                  data.ScriptLodAccumulatedTime = 0.0f;
               }
            }

            scriptEntityShouldUpdate.emplace(id, shouldUpdateScripts);
         }
      }

      struct PreAnimationScriptWorkItem {
         EntityID EntityId = INVALID_ENTITY_ID;
         size_t ScriptIndex = 0;
         int Priority = 0;
         uint64_t StableTypeId = 0;
      };

      std::vector<PreAnimationScriptWorkItem> preAnimationScripts;
      if (scriptEntityIds) {
         preAnimationScripts.reserve(scriptEntityIds->size());
         for (EntityID id : *scriptEntityIds) {
            auto it = m_Entities.find(id);
            if (it == m_Entities.end()) {
               continue;
            }

            auto& data = it->second;
            if (!data.Active || data.Scripts.empty()) {
               continue;
            }

            auto eligibilityIt = scriptEntityShouldUpdate.find(id);
            if (eligibilityIt != scriptEntityShouldUpdate.end() && !eligibilityIt->second) {
               continue;
            }

            for (size_t scriptIndex = 0; scriptIndex < data.Scripts.size(); ++scriptIndex) {
               auto& script = data.Scripts[scriptIndex];
               if (!script.Instance ||
                   !ScriptSystem::Instance().HasScriptFlag(
                      script.ClassName,
                      ScriptSystem::ScriptType_PreAnimationUpdate)) {
                  continue;
               }

               preAnimationScripts.push_back({
                  id,
                  scriptIndex,
                  ScriptSystem::Instance().GetScriptPriority(script.ClassName),
                  ScriptOrder::StableHashScriptClassName(script.ClassName)
               });
            }
         }
      }

      std::sort(preAnimationScripts.begin(), preAnimationScripts.end(),
         [](const PreAnimationScriptWorkItem& lhs, const PreAnimationScriptWorkItem& rhs) {
            return ScriptOrder::OrderLess(
               lhs.Priority, lhs.StableTypeId, lhs.EntityId, lhs.ScriptIndex,
               rhs.Priority, rhs.StableTypeId, rhs.EntityId, rhs.ScriptIndex);
         });

      bool ranPreAnimationScripts = false;
      for (const PreAnimationScriptWorkItem& workItem : preAnimationScripts) {
         auto it = m_Entities.find(workItem.EntityId);
         if (it == m_Entities.end()) {
            continue;
         }

         auto& data = it->second;
         if (!data.Active || workItem.ScriptIndex >= data.Scripts.size()) {
            continue;
         }

         auto& script = data.Scripts[workItem.ScriptIndex];
         if (!script.Instance) {
            continue;
         }

         ScopedTimer tScript("Scripts/PreAnimationUpdate");
         script.Instance->OnUpdate(dt);
         ranPreAnimationScripts = true;
      }

      if (ranPreAnimationScripts && HasPendingTransformUpdates()) {
         ScopedTimer t("Transforms/PostPreAnimationScripts");
         UpdateTransforms();
         SyncCameraComponentsFromTransforms();
      }
   }


   if (m_IsPlaying && (!m_RuntimeWorldBridge || HasPendingRuntimeWorldStructuralSyncWork())) {
      ScopedTimer t("RuntimeWorld/PreSimulationSync");
      SyncRuntimeWorld(false);
   }
   if (m_IsPlaying && m_RuntimeWorldBridge) {
      m_RuntimeWorldFrameSyncLocked = true;
   }

   // In play mode, evaluate animations before recomputing world transforms
   if (m_IsPlaying) {
      ScopedTimer t("Animation");
      cm::animation::AnimationSystem::Update(*this, dt);
   }

   // Update tweens (native + managed field tweens)
   {
      ScopedTimer t("Tweens");
      cm::animation::TweenManager::Get().Update(*this, dt);
   }

   // Update dialogue system (typewriter effects, timing, etc.)
   if (m_IsPlaying) {
      { ScopedTimer t("Dialogue"); Dialogue::GetGlobalDialogueManager().Update(dt); }
   }

   // LookAt/Aim pass: rotation-only constraints applied after animation, before IK
   // This allows IK to see already-rotated bones and solve naturally
   if (m_IsPlaying) {
      ScopedTimer t("LookAtConstraints");
      cm::animation::lookat::LookAtConstraintSystem::Get().Apply(*this, dt);
   }

   // IK pass: after animation sampling and LookAt, before transforms/skinning
   if (m_IsPlaying) {
      { ScopedTimer t("IK"); cm::animation::ik::IKSystem::Get().SolveAndBlend(*this, dt); }
   }

   // Recompute world transforms only when animation, LookAt, or IK actually
   // dirtied scene-space transforms. Pure palette-space animation updates can
   // skip this phase entirely.
   if (HasPendingTransformUpdates()) {
      ScopedTimer t("Transforms");
      UpdateTransforms();
   } else if (m_AnimationPosePaletteDirtyForAttachments) {
      const bool hasBoneAttachments = !GetBoneAttachmentEntities().empty();
      if (hasBoneAttachments) {
         ScopedTimer t("Transforms/BoneAttachments");
         cm::physics::RagdollSystem* ragdollSystemForAttachments =
            cm::physics::GetRagdollSystem();
         const BoneAttachmentProcessMode attachmentMode =
            (ragdollSystemForAttachments && ragdollSystemForAttachments->HasActiveRagdolls())
            ? BoneAttachmentProcessMode::ExcludeActiveRagdolls
            : BoneAttachmentProcessMode::All;
         if (ProcessBoneAttachments(attachmentMode)) {
            FinalizeRuntimeTransforms();
         }
      }
      m_AnimationPosePaletteDirtyForAttachments = false;
   }

   // Portal auto-detection (play mode only)
   {
      ScopedTimer t("Portals");
      cm::world::PortalSystem::Get().Update(*this, dt);
   }

   // Update ragdoll system AFTER transforms but BEFORE skinning
   // This allows ragdoll physics to override bone WorldMatrices
   cm::physics::RagdollSystem* ragdollSystem = m_IsPlaying ? cm::physics::GetRagdollSystem() : nullptr;
   if (ragdollSystem) {
      ScopedTimer t("Ragdoll");
      ragdollSystem->Update(dt);
      
      // Re-process BoneAttachments after ragdoll update so attached entities
      // (weapons, accessories) follow the ragdoll-driven bone positions
      if (ragdollSystem->HasActiveRagdolls()) {
         if (ProcessBoneAttachments(BoneAttachmentProcessMode::OnlyActiveRagdolls)) {
            FinalizeRuntimeTransforms();
         }
      }
   }

   // Update GPU skinning state after transforms (and ragdoll overrides)
   if (m_IsPlaying) {
      // Step skinning after animation so the shared skinning atlases
      // reflect the latest pose
      ScopedTimer t("Skinning");
      SkinningSystem::Update(*this, dt);
   }

   // Update instancer system (LOD, hot-swap, visibility)
   {
      ScopedTimer t("Instancers");
      cm::instancer::InstancerSystem::Instance().Update(*this, dt);
   }

   // Apply TintController tints to matching child meshes
   // OPTIMIZATION: Only apply when tint values have actually changed
   // Supports both legacy NamePattern mode AND new explicit Targets mode
   {
      for (auto& e : GetEntities()) {
         auto* data = GetEntityData(e.GetID());
         if (!data || !data->TintController) continue;
         auto& t = *data->TintController;
         
         // Skip if neither NamePattern nor Targets are set
         bool hasLegacyMode = !t.NamePattern.empty();
         bool hasTargetsMode = !t.Targets.empty();
         if (!hasLegacyMode && !hasTargetsMode) continue;
         
         // === LEGACY NamePattern Mode ===
         if (hasLegacyMode) {
            // Refresh matching meshes if needed
            if (t.NeedsRefresh) {
               t.MatchingMeshes.clear();
               std::function<void(EntityID)> gather = [&](EntityID id) {
                  auto* d = GetEntityData(id);
                  if (!d) return;
                  if (d->Name.find(t.NamePattern) == 0 && d->Mesh) {
                     t.MatchingMeshes.push_back(id);
                  }
                  for (EntityID c : d->Children) gather(c);
               };
               for (EntityID c : data->Children) gather(c);
               t.NeedsRefresh = false;
               t.TintDirty = true; // Force apply after mesh list refresh
            }
         }
         
         // Check if tint values changed - skip if nothing changed
         if (!t.CheckAndUpdateDirty()) continue;

         const PropertyID scalar0Id = PropertyID::Get("u_PBRScalar0");
         const PropertyID scalar1Id = PropertyID::Get("u_PBRScalar1");
         const PropertyID emissionColorId = PropertyID::Get("u_EmissionColor");
        const PropertyID textureUsageId = PropertyID::Get("u_TextureUsage");

         auto applyPbrOverrides = [&](MaterialPropertyBlock& pb, Material* material) {
            const bool shouldApply = t.UsePbrOverrides || t.PbrOverridesApplied;
            if (!shouldApply) return;

            auto* pbr = dynamic_cast<PBRMaterial*>(material);
            const float baseMetallic = pbr ? pbr->GetMetallic() : t.OverrideMetallic;
            const float baseRoughness = pbr ? pbr->GetRoughness() : t.OverrideRoughness;
            const float baseAO = pbr ? pbr->GetAmbientOcclusion() : 1.0f;
            const float baseNormal = pbr ? pbr->GetNormalScale() : 1.0f;
            const float baseEmissionStrength = pbr ? pbr->GetEmissionStrength() : t.OverrideEmissionStrength;
            const glm::vec3 baseEmissionColor = pbr ? pbr->GetEmissionColor() : t.OverrideEmissionColor;
            const glm::vec4 baseTextureUsage = pbr
                ? glm::vec4(
                    bgfx::isValid(pbr->m_MetallicRoughnessTex) ? 1.0f : 0.0f,
                    bgfx::isValid(pbr->m_NormalTex) ? 1.0f : 0.0f,
                    bgfx::isValid(pbr->m_AOTex) ? 1.0f : 0.0f,
                    bgfx::isValid(pbr->m_EmissionTex) ? 1.0f : 0.0f)
                : glm::vec4(0.0f);

            glm::vec4 scalar0(baseMetallic, baseRoughness, baseAO, baseNormal);
            glm::vec4 scalar1(baseEmissionStrength, 0.0f, 0.0f, 0.0f);

            glm::vec4 existing0;
            if (pb.TryGetVector(scalar0Id, existing0)) scalar0 = existing0;
            glm::vec4 existing1;
            if (pb.TryGetVector(scalar1Id, existing1)) scalar1 = existing1;
            glm::vec4 textureUsage = baseTextureUsage;
            glm::vec4 existingUsage;
            if (pb.TryGetVector(textureUsageId, existingUsage)) textureUsage = existingUsage;

            scalar0.x = t.UsePbrOverrides ? t.OverrideMetallic : baseMetallic;
            scalar0.y = t.UsePbrOverrides ? t.OverrideRoughness : baseRoughness;
            scalar1.x = t.UsePbrOverrides ? t.OverrideEmissionStrength : baseEmissionStrength;
            textureUsage.x = t.UsePbrOverrides ? 0.0f : baseTextureUsage.x; // disable MR map when overriding
            textureUsage.w = t.UsePbrOverrides ? 0.0f : baseTextureUsage.w; // disable emission map when overriding

            pb.SetVector(scalar0Id, scalar0);
            pb.SetVector(scalar1Id, scalar1);
            pb.SetVector(textureUsageId, textureUsage);

            glm::vec3 emissionColor = t.UsePbrOverrides ? t.OverrideEmissionColor : baseEmissionColor;
            pb.SetVector(emissionColorId, glm::vec4(emissionColor, 1.0f));

            t.PbrOverridesApplied = t.UsePbrOverrides;
         };

         auto computeTintSlotCount = [](MeshComponent* mesh) -> size_t {
            if (!mesh) return 0;
            size_t slotCount = mesh->materials.size();
            slotCount = std::max(slotCount, mesh->MaterialSlotNames.size());
            slotCount = std::max(slotCount, mesh->SlotPropertyBlocks.size());
            if (mesh->mesh) {
               slotCount = std::max(slotCount, mesh->mesh->Submeshes.size());
            }
            if (slotCount == 0) slotCount = 1;
            return slotCount;
         };
         
         // === Apply tints to legacy NamePattern matches ===
         if (hasLegacyMode) {
            for (EntityID meshId : t.MatchingMeshes) {
               auto* meshData = GetEntityData(meshId);
               if (!meshData || !meshData->Mesh) continue;
               auto* mesh = meshData->Mesh.get();
               const size_t slotCount = computeTintSlotCount(mesh);
               if (mesh->SlotPropertyBlocks.size() < slotCount) {
                  mesh->SlotPropertyBlocks.resize(slotCount);
               }
               for (size_t slot = 0; slot < slotCount; ++slot) {
                  auto& pb = (slot < mesh->SlotPropertyBlocks.size()) ? mesh->SlotPropertyBlocks[slot] : mesh->PropertyBlock;
                  Material* material = (slot < mesh->materials.size() && mesh->materials[slot]) ? mesh->materials[slot].get() : nullptr;
                  if (t.UseTintMask) {
                     pb.SetVector("u_TintColor0", t.TintColor0);
                     pb.SetVector("u_TintColor1", t.TintColor1);
                     pb.SetVector("u_TintColor2", t.TintColor2);
                     pb.SetVector("u_TintColor3", t.TintColor3);
                  } else {
                     pb.SetVector("u_ColorTint", t.BaseTint);
                  }
                  applyPbrOverrides(pb, material);
               }
            }
         }
         
         // === Apply tints to explicit Targets ===
         if (hasTargetsMode) {
            for (const TintTarget& target : t.Targets) {
               if (target.TargetEntity == INVALID_ENTITY_ID) continue;
               
               auto* targetData = GetEntityData(target.TargetEntity);
               if (!targetData || !targetData->Mesh) continue;
               
               auto* mesh = targetData->Mesh.get();
               const size_t slotCount = computeTintSlotCount(mesh);
               if (mesh->SlotPropertyBlocks.size() < slotCount) {
                  mesh->SlotPropertyBlocks.resize(slotCount);
               }
               
               // Determine which slots to apply to
               int startSlot = (target.MaterialSlot >= 0) ? target.MaterialSlot : 0;
               int endSlot = (target.MaterialSlot >= 0) ? target.MaterialSlot + 1 : static_cast<int>(slotCount);
               
               for (int slot = startSlot; slot < endSlot && slot < static_cast<int>(mesh->SlotPropertyBlocks.size()); ++slot) {
                  auto& pb = mesh->SlotPropertyBlocks[slot];
                  Material* material = (slot < static_cast<int>(mesh->materials.size()) && mesh->materials[slot]) ? mesh->materials[slot].get() : nullptr;
                  
                  // Set blend mode parameter
                  float blendModeValue = static_cast<float>(static_cast<int>(target.BlendMode));
                  pb.SetVector("u_TintParams", glm::vec4(blendModeValue, 0.5f, 0.0f, 0.0f));
                  
                  // Determine tint color (per-target override or global)
                  glm::vec4 tintColor = target.UseTargetColor ? target.Color : t.BaseTint;
                  
                  if (t.UseTintMask) {
                     pb.SetVector("u_TintColor0", t.TintColor0);
                     pb.SetVector("u_TintColor1", t.TintColor1);
                     pb.SetVector("u_TintColor2", t.TintColor2);
                     pb.SetVector("u_TintColor3", t.TintColor3);
                  } else {
                     pb.SetVector("u_ColorTint", tintColor);
                  }
                  applyPbrOverrides(pb, material);
               }
            }
         }
         
         // Clear dirty flag after applying
         t.TintDirty = false;
      }
   }

   // Update navigation agents and debug drawing
   {
      ScopedTimer t("Navigation");
      nav::Navigation::Get().Update(*this, dt);
   }

   // Update audio sources and listener
   if (m_IsPlaying && Audio::IsInitialized()) {
      ScopedTimer t("Audio");

      if (!m_RuntimeWorldBridge ||
          (HasPendingRuntimeWorldStructuralSyncWork() && !IsRuntimeWorldFrameSyncLocked())) {
         SyncRuntimeWorld(false);
      }

      const std::vector<EntityID>* audioListenerIds =
         m_RuntimeWorldBridge ? &m_RuntimeWorldBridge->GetAudioListenerSceneEntities() : nullptr;
      const std::vector<EntityID>* audioSourceIds =
         m_RuntimeWorldBridge ? &m_RuntimeWorldBridge->GetAudioSourceSceneEntities() : nullptr;
      
      // Find and update the active audio listener
      bool listenerSet = false;
      int highestPriority = INT_MIN;

      auto updateListenerCandidate = [&](EntityData* data) {
         if (!data || !data->AudioListener) return;
         auto& listener = *data->AudioListener;
         
         if (listener.Active && listener.Priority > highestPriority) {
            highestPriority = listener.Priority;
            
            {
               const auto& world = data->Transform.WorldMatrix;
               glm::vec3 position = glm::vec3(world[3]);
               glm::vec3 forward = -glm::normalize(glm::vec3(world[2])); // -Z is forward
               glm::vec3 up = glm::normalize(glm::vec3(world[1]));
               
               Audio::SetListenerTransform(position, forward, up);
               
               // Calculate velocity for doppler
               glm::vec3 velocity = (position - listener.LastPosition) / dt;
               Audio::SetListenerVelocity(velocity);
               listener.LastPosition = position;
               
               listenerSet = true;
            }
         }
      };

      if (audioListenerIds) {
         for (EntityID id : *audioListenerIds) {
            updateListenerCandidate(GetEntityData(id));
         }
      } else {
         for (auto& e : GetEntities()) {
            updateListenerCandidate(GetEntityData(e.GetID()));
         }
      }
      
      // Update audio sources
      auto updateAudioSource = [&](EntityData* data) {
         if (!data || !data->AudioSource) return;
         auto& source = *data->AudioSource;
         
         // Process control requests
         if (source.StopRequested) {
            if (source.SoundHandle != INVALID_AUDIO_HANDLE) {
               Audio::Stop(source.SoundHandle);
               source.SoundHandle = INVALID_AUDIO_HANDLE;
            }
            source.IsPlaying = false;
            source.IsPaused = false;
            source.StopRequested = false;
         }
         
         if (source.PauseRequested) {
            if (source.SoundHandle != INVALID_AUDIO_HANDLE) {
               Audio::Pause(source.SoundHandle);
            }
            source.IsPaused = true;
            source.PauseRequested = false;
         }
         
         if (source.ResumeRequested) {
            if (source.SoundHandle != INVALID_AUDIO_HANDLE) {
               Audio::Resume(source.SoundHandle);
            }
            source.IsPaused = false;
            source.ResumeRequested = false;
         }
         
         if (source.PlayRequested || (!source.Initialized && source.PlayOnAwake)) {
            const bool explicitRequest = source.PlayRequested;
            std::string path = ResolveAudioSourcePath(source);
            if (!path.empty()) {
               // Stop any currently playing sound
               if (source.SoundHandle != INVALID_AUDIO_HANDLE) {
                  Audio::Stop(source.SoundHandle);
               }

               float volume = source.Mute ? 0.0f : source.Volume;

               if (source.Spatial) {
                  glm::vec3 pos = glm::vec3(data->Transform.WorldMatrix[3]);
                  source.SoundHandle = Audio::Play3D(path, pos, volume, source.Loop,
                                                      source.MinDistance, source.MaxDistance);
                  source.LastPosition = pos;
               } else {
                  source.SoundHandle = Audio::Play(path, volume, source.Loop);
               }

               source.IsPlaying = (source.SoundHandle != INVALID_AUDIO_HANDLE);
               if (source.IsPlaying) {
                  Audio::SetPitch(source.SoundHandle, source.Pitch);
               }
            }
            source.PlayRequested = false;
            // Latch the auto-play (PlayOnAwake) gate only once the sound has
            // actually started. Otherwise a transient first-frame failure -- the
            // audio engine, the asset, or the runtime-world audio-source list not
            // being ready yet on the first play frame -- would set Initialized and
            // permanently suppress PlayOnAwake. Keep retrying until it starts.
            // An explicit Play() request is one-shot regardless of success.
            if (explicitRequest || source.IsPlaying) {
               source.Initialized = true;
            }
         }
         
         // Update properties for playing sounds (volume, mute, pitch)
         if (source.IsPlaying && source.SoundHandle != INVALID_AUDIO_HANDLE) {
            float effectiveVolume = source.Mute ? 0.0f : source.Volume;
            Audio::SetVolume(source.SoundHandle, effectiveVolume);
            Audio::SetPitch(source.SoundHandle, source.Pitch);
         }
         
         // Update 3D position for playing spatial sounds
         if (source.IsPlaying && source.Spatial && source.SoundHandle != INVALID_AUDIO_HANDLE) {
            glm::vec3 pos = glm::vec3(data->Transform.WorldMatrix[3]);
            Audio::SetPosition(source.SoundHandle, pos);
            
            // Calculate velocity for doppler effect
            glm::vec3 velocity = (pos - source.LastPosition) / dt;
            Audio::SetVelocity(source.SoundHandle, velocity * source.DopplerFactor);
            source.LastPosition = pos;
         }
         
         // Check if sound finished playing
         if (source.IsPlaying && source.SoundHandle != INVALID_AUDIO_HANDLE) {
            if (!Audio::IsPlaying(source.SoundHandle) && !source.Loop) {
               source.SoundHandle = INVALID_AUDIO_HANDLE;
               source.IsPlaying = false;
            }
         }
      };

      if (audioSourceIds) {
         for (EntityID id : *audioSourceIds) {
            updateAudioSource(GetEntityData(id));
         }
      } else {
         for (auto& e : GetEntities()) {
            updateAudioSource(GetEntityData(e.GetID()));
         }
      }
   }

   // Managed side: Ensure sync context is installed (uses global pointer from ScriptInterop.h)
   if (EnsureInstalledPtr) EnsureInstalledPtr();


   if (m_IsPlaying) {

      // Step physics simulation
      static int physicsStepCount = 0;
      physicsStepCount++;

      // Debug: Print gravity and step info for te first few steps
      if (physicsStepCount <= 5) {
         glm::vec3 gravity = Physics::Get().GetGravity();

         const std::string msg = "[Physics] Step " +
            std::to_string(physicsStepCount) + " - dt: " + std::to_string(dt)
            + " - Gravity: (" + std::to_string(gravity.x) + ", " +
            std::to_string(gravity.y) + ", " + std::to_string(gravity.z) + ")";

#ifndef CLAYMORE_RUNTIME
         Logger::Log(msg);
#else
         std::cout << msg << std::endl;
#endif
      }

      const std::vector<EntityID>* physicsParticipantIds = nullptr;
      if (m_RuntimeWorldBridge) {
         physicsParticipantIds = &m_RuntimeWorldBridge->GetPhysicsSceneEntities();
      } else {
         RebuildPhysicsParticipantCacheIfNeeded();
         physicsParticipantIds = &m_PhysicsParticipants;
      }

      // Ensure Area bodies exist for any entities with AreaComponent
      if (Physics::Get().GetAreaSystem())
      {
         for (EntityID id : *physicsParticipantIds)
         {
            auto it = m_Entities.find(id);
            if (it == m_Entities.end()) continue;
            auto& data = it->second;
            if (data.Area && data.Area->Enabled && data.Area->Body == nullptr)
            {
               Physics::Get().GetAreaSystem()->OnCreate(Entity(id, this), *data.Area);
            }
         }
      }

      // =========================================================================
      // Collider Shape Change Detection for RigidBodies/StaticBodies
      // =========================================================================
      // If collider properties have changed at runtime, rebuild the physics body
      // with the updated shape. This mirrors CharacterController behavior.
      // =========================================================================
      for (EntityID id : *physicsParticipantIds) {
         auto it = m_Entities.find(id);
         if (it == m_Entities.end()) continue;
         auto& data = it->second;
         if (!data.Collider) continue;
         bool hasBody = (data.RigidBody && !data.RigidBody->BodyID.IsInvalid()) ||
                        (data.StaticBody && !data.StaticBody->BodyID.IsInvalid());
         if (!hasBody) {
            // Self-heal missing rigid/static body handles (e.g. after play-mode source cleanup).
            const bool wantsBody = (data.RigidBody != nullptr) || (data.StaticBody != nullptr);
            if (!wantsBody || data.CharacterController) {
               continue;
            }

            glm::vec3 wscale(1.0f);
            glm::vec3 wpos, wskew;
            glm::vec4 wpersp;
            glm::quat wrot;
            glm::decompose(data.Transform.WorldMatrix, wscale, wrot, wpos, wskew, wpersp);

            if (!data.Collider->Shape) {
               data.Collider->BuildShape(
                  data.Mesh && data.Mesh->mesh ? data.Mesh->mesh.get() : nullptr,
                  glm::abs(wscale));
            }
            if (data.Collider->Shape) {
               CreatePhysicsBody(id, data.Transform, *data.Collider);
            }
            continue;
         }
         
         auto& col = *data.Collider;
         bool shapeChanged = false;
         bool offsetChanged = false;

         glm::vec3 wscale(1.0f);
         glm::vec3 wpos, wskew;
         glm::vec4 wpersp;
         glm::quat wrot;
         glm::decompose(data.Transform.WorldMatrix, wscale, wrot, wpos,
            wskew, wpersp);
         const glm::vec3 absWorldScale = glm::abs(wscale);
         
         // Check for offset change (separate from shape changes - affects position, not shape)
         if (glm::any(glm::epsilonNotEqual(col._LastOffset, col.Offset, 1e-5f))) {
            offsetChanged = true;
         }

         if (glm::any(glm::epsilonNotEqual(col._LastWorldScale,
             absWorldScale, 1e-5f))) {
            shapeChanged = true;
            offsetChanged = true;
         }
         
         // Check for shape type change
         if (col._LastShapeType != col.ShapeType) {
            shapeChanged = true;
         }
         // Check shape-specific parameter changes
         else {
            switch (col.ShapeType) {
               case ColliderShape::Box:
                  if (glm::any(glm::epsilonNotEqual(col._LastSize, col.Size, 1e-5f))) {
                     shapeChanged = true;
                  }
                  break;
               case ColliderShape::Capsule:
                  if (std::abs(col._LastRadius - col.Radius) > 1e-5f ||
                      std::abs(col._LastHeight - col.Height) > 1e-5f) {
                     shapeChanged = true;
                  }
                  break;
               case ColliderShape::Sphere:
                  if (std::abs(col._LastRadius - col.Radius) > 1e-5f) {
                     shapeChanged = true;
                  }
                  break;
               default:
                  break;
            }
         }
         
         // Handle offset changes: update physics body position
         if (offsetChanged) {
            JPH::BodyID bodyId;
            if (data.RigidBody && !data.RigidBody->BodyID.IsInvalid()) {
               bodyId = data.RigidBody->BodyID;
            } else if (data.StaticBody && !data.StaticBody->BodyID.IsInvalid()) {
               bodyId = data.StaticBody->BodyID;
            } else {
               auto legacyBodyIt = m_BodyMap.find(id);
               if (legacyBodyIt != m_BodyMap.end()) {
                  bodyId = legacyBodyIt->second;
               }
            }
            
            if (!bodyId.IsInvalid()) {
               // Recompute world pose with new offset (same logic as CreatePhysicsBody).
               glm::vec3 worldPos(0.0f);
               glm::quat worldRot(1.0f, 0.0f, 0.0f, 0.0f);
               DecomposeWorldTransform(data.Transform, worldPos, worldRot);
               glm::vec3 pos = worldPos + TransformColliderOffsetToWorld(data.Transform, col.Offset);
               
               JPH::RVec3 joltPosition(pos.x, pos.y, pos.z);
               JPH::Quat joltRotation(worldRot.x, worldRot.y, worldRot.z, worldRot.w);
               
               JPH::BodyInterface& bodyInterface = Physics::Get().GetBodyInterface();
               bodyInterface.SetPositionAndRotation(bodyId, joltPosition, joltRotation, JPH::EActivation::Activate);
            }
            
            // Update last-known offset
            col._LastOffset = col.Offset;
         }
         
         if (shapeChanged) {
            // Rebuild the shape with current parameters
            col.BuildShape(data.Mesh && data.Mesh->mesh ? data.Mesh->mesh.get() : nullptr, absWorldScale);
            
            // Update the physics body shape
            if (col.Shape) {
               JPH::BodyID bodyId;
               if (data.RigidBody && !data.RigidBody->BodyID.IsInvalid()) {
                  bodyId = data.RigidBody->BodyID;
               } else if (data.StaticBody && !data.StaticBody->BodyID.IsInvalid()) {
                  bodyId = data.StaticBody->BodyID;
               }
               
               if (!bodyId.IsInvalid()) {
                  JPH::BodyInterface& bodyInterface = Physics::Get().GetBodyInterface();
                  bodyInterface.SetShape(bodyId, col.Shape, true, JPH::EActivation::Activate);
               }
            }
            
            // Update last-known values
            col._LastShapeType = col.ShapeType;
            col._LastSize = col.Size;
            col._LastRadius = col.Radius;
            col._LastHeight = col.Height;
            col._LastWorldScale = absWorldScale;
            // Also update offset when shape changes (body is recreated with new offset)
            col._LastOffset = col.Offset;
         }
      }

      // =========================================================================
      // Root Motion -> Physics Integration
      // =========================================================================
      // Process root motion deltas computed by AnimationSystem and inject them
      // as velocity into CharacterController or RigidBody for physics-correct movement.
      // This must happen BEFORE the physics pre-step so velocities are set correctly.
      // =========================================================================
      static int s_rmDebugCounter = 0;
      std::vector<EntityID> fallbackAnimationRootIds;
      const std::vector<EntityID>* animationRootIds = nullptr;
      if (m_RuntimeWorldBridge) {
         animationRootIds = &m_RuntimeWorldBridge->GetAnimationRootSceneEntities();
      } else {
         fallbackAnimationRootIds.reserve(m_Entities.size());
         for (const auto& [id, data] : m_Entities) {
            if (data.AnimationPlayer && data.Skeleton) {
               fallbackAnimationRootIds.push_back(id);
            }
         }
         animationRootIds = &fallbackAnimationRootIds;
      }

      for (EntityID animId : *animationRootIds)
      {
         auto animIt = m_Entities.find(animId);
         if (animIt == m_Entities.end()) {
            continue;
         }
         auto& animData = animIt->second;
         if (!animData.AnimationPlayer) continue;
         auto& player = *animData.AnimationPlayer;
         
         // Skip if no valid root motion delta this frame
         if (!player._RootMotionOutput.Valid) continue;
         
         // Debug: log every 60 frames
         bool doLog = (++s_rmDebugCounter % 60 == 1);
         if (doLog) {
            std::cout << "[RootMotion] Entity " << animId << " has valid delta: ("
                      << player._RootMotionOutput.PositionDelta.x << ", "
                      << player._RootMotionOutput.PositionDelta.y << ", "
                      << player._RootMotionOutput.PositionDelta.z << ")" << std::endl;
         }
         
         // Resolve root-motion driver automatically.
         // If animator entity or its direct parent has a CharacterController / kinematic RigidBody,
         // route through physics. Otherwise drive local transform directly.
         EntityID targetId = INVALID_ENTITY_ID;
         EntityData* targetData = nullptr;
         enum class RootMotionDriveKind { Transform, CharacterController, KinematicRigidBody };
         RootMotionDriveKind driveKind = RootMotionDriveKind::Transform;

         auto evaluatePhysicsDriver = [&](EntityID candidateId, EntityData* candidateData) -> bool {
            if (candidateId == INVALID_ENTITY_ID || !candidateData) return false;
            if (candidateData->CharacterController) {
               targetId = candidateId;
               targetData = candidateData;
               driveKind = RootMotionDriveKind::CharacterController;
               return true;
            }
            if (candidateData->RigidBody && candidateData->RigidBody->IsKinematic) {
               targetId = candidateId;
               targetData = candidateData;
               driveKind = RootMotionDriveKind::KinematicRigidBody;
               return true;
            }
            return false;
         };

         evaluatePhysicsDriver(animId, &animData);
         if (targetId == INVALID_ENTITY_ID && animData.Parent != INVALID_ENTITY_ID) {
            if (auto* parentData = GetEntityData(animData.Parent)) {
               evaluatePhysicsDriver(animData.Parent, parentData);
            }
         }

         if (targetId == INVALID_ENTITY_ID) {
            targetId = animId;
            targetData = &animData;
            driveKind = RootMotionDriveKind::Transform;
         }

         if (doLog) {
            std::cout << "[RootMotion] Auto target -> entity " << targetId
                      << " driveKind=" << (driveKind == RootMotionDriveKind::CharacterController ? "CharacterController"
                                        : driveKind == RootMotionDriveKind::KinematicRigidBody ? "RigidBody" : "Transform")
                      << std::endl;
         }
         
         // Apply root motion to target
         if (targetId != INVALID_ENTITY_ID) {
            if (targetData) {
               const glm::vec3& posDelta = player._RootMotionOutput.PositionDelta;
               glm::vec3 sourceWorldPosition(0.0f);
               glm::quat sourceWorldRotation(1.0f, 0.0f, 0.0f, 0.0f);
               DecomposeWorldTransform(animData.Transform, sourceWorldPosition, sourceWorldRotation);
               (void)sourceWorldPosition;

               // Root motion deltas are extracted in the animator entity's local/model space.
               // When gameplay rotates a visual child (e.g. SkeletonRoot) while physics stays on
               // the parent CharacterController, steer the motion using the animator's world yaw,
               // then inject the resulting world-space delta into the chosen movement target.
               const glm::vec3 steeredPosDelta = RotateRootMotionDeltaToWorldYaw(posDelta, sourceWorldRotation);
               
               // Convert position delta to velocity (delta / dt)
               glm::vec3 velocity = (dt > 0.0001f) ? (steeredPosDelta / dt) : glm::vec3(0.0f);
               
               // CASE 1: CharacterController - inject as DesiredVelocity
               if (driveKind == RootMotionDriveKind::CharacterController && targetData->CharacterController) {
                  auto& cc = *targetData->CharacterController;
                  // Add root motion velocity to existing desired velocity
                  // This allows scripts to ALSO contribute velocity (e.g., for strafing)
                  cc.DesiredVelocity.x += velocity.x;
                  cc.DesiredVelocity.z += velocity.z;
                  // Root-motion clips that override gravity treat animation Y as authoritative,
                  // which keeps climbing/bed-entry motion aligned with the authored keyframes.
                  if (player._RootMotionOutput.OverrideGravity) {
                     cc.VerticalVelocity = velocity.y;
                     cc._RootMotionAppliedVertical = std::abs(velocity.y) > 0.001f;
                  } else if (std::abs(velocity.y) > 0.001f) {
                     // Otherwise root motion contributes on top of the controller's existing
                     // vertical velocity so jumps/falls can still combine with animation.
                     cc.VerticalVelocity += velocity.y;
                     cc._RootMotionAppliedVertical = true;
                  }
                  // If animation overrides gravity (e.g., climbing), set flag to skip gravity this frame
                  if (player._RootMotionOutput.OverrideGravity) {
                     cc._RootMotionOverrideGravity = true;
                  }
                  if (doLog) {
                     std::cout << "[RootMotion] Applied to CharacterController:"
                              << " posDelta=(" << steeredPosDelta.x << ", " << steeredPosDelta.y << ", " << steeredPosDelta.z << ")"
                              << " dt=" << dt
                              << " vel=(" << velocity.x << ", " << velocity.y << ", " << velocity.z << ")"
                              << " DesiredVel now=(" << cc.DesiredVelocity.x << ", " << cc.DesiredVelocity.z << ")"
                               << " VertVel=" << cc.VerticalVelocity
                               << " OverrideGravity=" << (player._RootMotionOutput.OverrideGravity ? "true" : "false") << std::endl;
                  }
               }
               // CASE 2: Kinematic RigidBody - inject as LinearVelocity
               else if (driveKind == RootMotionDriveKind::KinematicRigidBody && targetData->RigidBody && targetData->RigidBody->IsKinematic) {
                  // Root motion is a per-frame contribution, not a persistent
                  // change to the kinematic body's authored velocity.
                  targetData->RigidBody->_RootMotionLinearVelocity += velocity;
               }
               // CASE 3: No physics body - apply directly to transform (fallback)
               else {
                  targetData->Transform.Position += ConvertWorldVectorToParentLocal(*this, *targetData, steeredPosDelta);
                  MarkTransformDirty(targetId);
                  if (doLog) {
                     std::cout << "[RootMotion] Applied to Transform directly (no physics)" << std::endl;
                  }
               }
               
               // Apply rotation delta if present
               const glm::quat& rotDelta = player._RootMotionOutput.RotationDelta;
               if (rotDelta != glm::quat(1,0,0,0)) {
                  // Apply rotation to entity
                  if (targetData->Transform.UseQuatRotation) {
                     targetData->Transform.RotationQ = rotDelta * targetData->Transform.RotationQ;
                  } else {
                     // Convert euler to quat, apply delta, convert back
                     glm::quat current = glm::quat(glm::radians(targetData->Transform.Rotation));
                     glm::quat result = rotDelta * current;
                     targetData->Transform.Rotation = glm::degrees(glm::eulerAngles(result));
                  }
                  MarkTransformDirty(targetId);
               }
            }
         } else {
            // No target found - apply to animator entity's transform directly (fallback)
            animData.Transform.Position += player._RootMotionOutput.PositionDelta;
            MarkTransformDirty(animId);
         }
         
         // Clear the delta after consumption (it's been processed)
         player._RootMotionOutput.Reset();
      }

      Profiler::Get().SetCounter("Physics/Participants", static_cast<uint64_t>(physicsParticipantIds->size()));

      // Pre-step: move all kinematic bodies so Jolt can resolve collisions this frame
      for (EntityID physicsEntityId : *physicsParticipantIds) {
         auto physIt = m_Entities.find(physicsEntityId);
         if (physIt == m_Entities.end()) continue;
         EntityID id = physicsEntityId;
         auto& data = physIt->second;

         if (data.RigidBody && !data.RigidBody->BodyID.IsInvalid()) {
            const JPH::EMotionType desiredMotionType = data.RigidBody->IsKinematic
               ? JPH::EMotionType::Kinematic
               : JPH::EMotionType::Dynamic;

            JPH::EMotionType actualMotionType = desiredMotionType;
            if (JPH::PhysicsSystem* system = Physics::GetSystem()) {
               JPH::BodyLockRead lock(system->GetBodyLockInterface(), data.RigidBody->BodyID);
               if (lock.Succeeded()) {
                  actualMotionType = lock.GetBody().GetMotionType();
               }
            }

            if (actualMotionType != desiredMotionType) {
               JPH::BodyInterface& bodyInterface = Physics::Get().GetBodyInterface();
               bodyInterface.SetMotionType(
                  data.RigidBody->BodyID,
                  desiredMotionType,
                  JPH::EActivation::Activate);

               if (JPH::PhysicsSystem* system = Physics::GetSystem()) {
                  JPH::BodyLockWrite lock(system->GetBodyLockInterface(), data.RigidBody->BodyID);
                  if (lock.Succeeded()) {
                     lock.GetBody().SetCollideKinematicVsNonDynamic(data.RigidBody->IsKinematic);
                  }
               }

               if (data.RigidBody->IsKinematic) {
                  bodyInterface.SetLinearVelocity(data.RigidBody->BodyID, JPH::Vec3::sZero());
                  bodyInterface.SetAngularVelocity(data.RigidBody->BodyID, JPH::Vec3::sZero());
                  data.RigidBody->LinearVelocity = glm::vec3(0.0f);
                  data.RigidBody->AngularVelocity = glm::vec3(0.0f);
                  data.RigidBody->_RootMotionLinearVelocity = glm::vec3(0.0f);
#if !defined(CLAYMORE_RUNTIME)
                  data.RigidBody->_EditorDisplayLinearVelocity = glm::vec3(0.0f);
#endif
               } else {
                  Physics::SetBodyLinearVelocity(data.RigidBody->BodyID, data.RigidBody->LinearVelocity);
                  Physics::SetBodyAngularVelocity(data.RigidBody->BodyID, data.RigidBody->AngularVelocity);
                  const float targetGravityFactor = data.RigidBody->UseGravity ? 1.0f : 0.0f;
                  bodyInterface.SetGravityFactor(data.RigidBody->BodyID, targetGravityFactor);
                  data.RigidBody->_LastAppliedGravityFactor = targetGravityFactor;
               }
            }

            if (!data.RigidBody->IsKinematic) {
               ApplyPendingRigidBodyCommands(*data.RigidBody);
            }
         }

         // Character Controller update
         if (data.CharacterController) {
            auto& cc = *data.CharacterController;

            // Sanitize controller geometry
            float safeRadius = glm::max(0.05f, cc.Radius);
            float safeHeight = glm::max(0.0f, cc.Height);
            uint32_t safePhysicsLayer = cc.PhysicsLayer < MAX_PHYSICS_LAYERS ? cc.PhysicsLayer : 0;
            JPH::ObjectLayer characterLayer = static_cast<JPH::ObjectLayer>(safePhysicsLayer);

            // Check if shape parameters changed (requires character recreation)
            bool shapeChanged = cc.Character && 
               (std::abs(cc._LastRadius - safeRadius) > 1e-5f || 
                std::abs(cc._LastHeight - safeHeight) > 1e-5f);

            // Preserve state if recreating due to shape change
            JPH::Vec3 savedPosition = JPH::Vec3::sZero();
            JPH::Quat savedRotation = JPH::Quat::sIdentity();
            JPH::Vec3 savedVelocity = JPH::Vec3::sZero();
            bool wasGrounded = false;
            float savedVerticalVelocity = 0.0f;

            if (shapeChanged && cc.Character) {
               // Save current state before destroying
               JPH::RMat44 w = cc.Character->GetWorldTransform();
               savedPosition = JPH::Vec3((float)w(0,3), (float)w(1,3), (float)w(2,3));
               savedRotation = cc.Character->GetRotation();
               savedVelocity = cc.Character->GetLinearVelocity();
               wasGrounded = cc.IsGrounded;
               savedVerticalVelocity = cc.VerticalVelocity;

               // Unregister from AreaSystem before destroying
               if (Physics::Get().GetAreaSystem()) {
                  JPH::BodyID inner = cc.Character->GetInnerBodyID();
                  if (!inner.IsInvalid()) {
                     Physics::Get().GetAreaSystem()->UnregisterCharacterInnerBody(inner);
                  }
               }

               // Destroy old character
               cc.Character = nullptr;
            }

            if (!cc.Character) {

               // Height is cylinder height (excluding hemispheres). Jolt expects half-cylinder height.
               float halfHeight = glm::max(0.0f, safeHeight * 0.5f);

               // Jolt CapsuleShapeSettings takes (halfHeight, radius) - NOT (radius, halfHeight)!
               JPH::CapsuleShapeSettings cap(halfHeight, safeRadius);
               auto capRes = cap.Create();
               if (capRes.HasError()) {
                  std::cerr << "[CharacterController] Capsule creation failed: "
                     << capRes.GetError() << ". Using fallback capsule.\n";

                  JPH::CapsuleShapeSettings fallback(0.5f, 0.25f);
                  capRes = fallback.Create();
               }
               if (capRes.HasError()) {

                  // As a last resort, skip creating the character this frame
                  std::cerr << "[CharacterController] Fallback capsule creation"
                     << " also failed.Skipping character init.\n";

                  continue;
               }

               JPH::RefConst<JPH::Shape> capsule = capRes.Get();
               JPH::CharacterVirtualSettings settings;

               settings.mShape = capsule;
               settings.mMaxSlopeAngle = JPH::DegreesToRadians(cc.MaxSlopeDegrees);
               settings.mEnhancedInternalEdgeRemoval = true;
               settings.mCharacterPadding = 0.005f;
               settings.mPredictiveContactDistance = 0.02f;
               settings.mPenetrationRecoverySpeed = 1.0f;
               settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
               settings.mInnerBodyShape = capsule;

               settings.mInnerBodyLayer = characterLayer;

               glm::vec3 up = glm::normalize(cc.Up);
               if (!glm::all(glm::isfinite(up)) || glm::length(up) < 1e-4f)
               {
                  up = glm::vec3(0.0f, 1.0f, 0.0f);
               }

               float feetToCenter = halfHeight + safeRadius;
               JPH::RVec3 spawnPos;
               JPH::Quat spawnRot;

               if (shapeChanged) {
                  // Recreating: use saved position (already includes offset and feet-to-center)
                  // Adjust for new feetToCenter since capsule size changed
                  float oldFeetToCenter = (cc._LastHeight * 0.5f) + cc._LastRadius;
                  glm::vec3 adjustment = up * (feetToCenter - oldFeetToCenter);
                  spawnPos = savedPosition + JPH::Vec3(adjustment.x, adjustment.y, adjustment.z);
                  spawnRot = savedRotation;
               } else {
                  // Fresh creation: compute from entity transform
                  glm::vec3 pos = glm::vec3(data.Transform.WorldMatrix[3]);
                  glm::quat rq = data.Transform.UseQuatRotation ?
                     data.Transform.RotationQ : glm::quat(glm::radians(data.Transform.Rotation));
                  // Offset is in local space, so rotate it by entity rotation
                  glm::vec3 offsetWorld = rq * cc.Offset;
                  // up is world-space direction, feetToCenter is along world up
                  glm::vec3 spawn = pos + offsetWorld + up * feetToCenter;
                  spawnPos = JPH::RVec3(spawn.x, spawn.y, spawn.z);
                  spawnRot = JPH::Quat(rq.x, rq.y, rq.z, rq.w);
               }

               // Create character with new shape
               cc.Character = new JPH::CharacterVirtual(&settings,
                  spawnPos,
                  spawnRot,
                  static_cast<uint64_t>(id),
                  Physics::Get().GetSystem());
               cc.Character->SetUp(JPH::Vec3(up.x, up.y, up.z));

               // Restore velocity if recreating
               if (shapeChanged) {
                  cc.Character->SetLinearVelocity(savedVelocity);
                  cc.IsGrounded = wasGrounded;
                  cc.VerticalVelocity = savedVerticalVelocity;
               }

               // Store current shape parameters for future change detection
               cc._LastRadius = safeRadius;
               cc._LastHeight = safeHeight;
               cc._LastPhysicsLayer = safePhysicsLayer;

               // Register character inner body with AreaSystem so 
               // area contacts can resolve the Entity ID correctly.
               if (Physics::Get().GetAreaSystem()) {
                  // CharacterVirtual exposes its inner body ID for contact filtering
                  JPH::BodyID inner = cc.Character->GetInnerBodyID();
                  if (!inner.IsInvalid())
                  {
                     Physics::Get()
                        .GetAreaSystem()->RegisterCharacterInnerBody(inner, id);
                  }

               }
            }

            if (cc.Character && cc._LastPhysicsLayer != safePhysicsLayer) {
               JPH::BodyID inner = cc.Character->GetInnerBodyID();
               if (!inner.IsInvalid()) {
                  Physics::SetBodyLayer(inner, safePhysicsLayer);
               }
               cc._LastPhysicsLayer = safePhysicsLayer;
            }

            if (cc.Character) {

               glm::vec3 gravity = Physics::Get().GetGravity();
               auto groundState = cc.Character->GetGroundState();
               cc.IsGrounded = (groundState == JPH::CharacterBase::EGroundState::OnGround);
               const bool rootMotionVerticalThisFrame = cc._RootMotionAppliedVertical;

               // Clamp grounded downward drift unless animation explicitly injected vertical root motion.
               if (cc.IsGrounded && cc.VerticalVelocity < 0.0f && !rootMotionVerticalThisFrame) {
                  cc.VerticalVelocity = 0.0f;
               }

               // Handle Jump request
               if (cc.JumpRequested && cc.IsGrounded) {
                  cc.VerticalVelocity = cc.JumpRequestSpeed;
                  cc.JumpRequested = false;
                  cc.IsGrounded = false; // jump takes us off the ground immediately
               }

               // Apply gravity while airborne so the controller eventually falls back down.
               // Skip gravity if root motion is overriding it (e.g., climbing animations)
               if (!cc.IsGrounded && !cc._RootMotionOverrideGravity) {
                  cc.VerticalVelocity += gravity.y * dt;
               }
               // Reset the override flag after use (it's set per-frame by root motion)
               cc._RootMotionOverrideGravity = false;

#if !defined(CLAYMORE_RUNTIME)
               cc._EditorDisplayDesiredVelocity = cc.DesiredVelocity;
#endif
               glm::vec3 v = cc.DesiredVelocity + glm::vec3(0.0f, cc.VerticalVelocity, 0.0f);
               cc.Character->SetLinearVelocity(JPH::Vec3(v.x, v.y, v.z));

               JPH::CharacterVirtual::ExtendedUpdateSettings eus;

               // Stick to Floor
               if (!cc.StickToFloor)
               {
                  eus.mStickToFloorStepDown = JPH::Vec3::sZero();
               }

               // Stair Walking
               if (!cc.EnableWalkStairs)
               {
                  eus.mWalkStairsStepUp = JPH::Vec3::sZero();
               }

               CharacterControllerBodyFilter characterBodyFilter(Physics::GetSystem(), characterLayer, cc.CollisionMask);
               cc.Character->ExtendedUpdate(
                  dt,
                  JPH::Vec3(0.0f, gravity.y, 0.0f),
                  eus,
                  JPH::BroadPhaseLayerFilter{},  // Default: collides with everything
                  JPH::ObjectLayerFilter{},      // Default: collides with everything
                  characterBodyFilter, {}, *Physics::Get().GetTempAllocator());

               // Read back the runtime velocity so we stay in sync with Jolt's gravity integration.
               const JPH::Vec3 newVelocity = cc.Character->GetLinearVelocity();
               cc.VerticalVelocity = newVelocity.GetY();
               cc.IsGrounded = (cc.Character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround);
               if (cc.IsGrounded && cc.VerticalVelocity < 0.0f && !rootMotionVerticalThisFrame) {
                  cc.VerticalVelocity = 0.0f;
               }
               cc._RootMotionAppliedVertical = false;
               
               // Reset DesiredVelocity after applying - it should be set fresh each frame
               // by scripts or root motion, not accumulated
               cc.DesiredVelocity = glm::vec3(0.0f);

               // Sync entity LOCAL transform from character 
               // world transform (respect parent hierarchy)
               JPH::RMat44 w = cc.Character->GetWorldTransform();
               glm::mat4 worldM(1.0f);
               for (int r = 0; r < 4; ++r)
               {
                  for (int c = 0; c < 4; ++c)
                  {
                     worldM[c][r] = w(r, c);
                  }
               }

               // Remove feet-to-center offset so entity pivot (authored) matches feet location
               float safeRadius = glm::max(0.05f, cc.Radius);
               float halfHeight = glm::max(0.0f, cc.Height * 0.5f);
               float feetToCenter = halfHeight + safeRadius;


               glm::vec3 up = glm::normalize(cc.Up);
               if (!glm::all(glm::isfinite(up)) || glm::length(up) < 1e-4f)
               {
                  up = glm::vec3(0.0f, 1.0f, 0.0f);
               }

               worldM = glm::translate(worldM, -up * feetToCenter);
               worldM = glm::translate(worldM, -cc.Offset);

               glm::mat4 parentWorld = glm::mat4(1.0f);

               if (data.Parent != INVALID_ENTITY_ID) {
                  auto* p = GetEntityData(data.Parent);
                  if (p)
                  {
                     parentWorld = p->Transform.WorldMatrix;
                  }
               }

               glm::mat4 localM = glm::inverse(parentWorld) * worldM;
               glm::vec3 t, s, skew; glm::vec4 persp; glm::quat rq;
               glm::decompose(localM, s, rq, t, skew, persp);

               // Character controllers often report the same resolved pose frame-to-frame.
               // Avoid dirtying the entire skeleton subtree unless the authored local pose
               // actually changed.
               ApplyPhysicsDrivenLocalTransformIfChanged(*this, id, data, t, rq);
            }
         }
         if (data.RigidBody && !data.RigidBody->BodyID.IsInvalid() &&
            data.RigidBody->IsKinematic)
         {
            const glm::vec3 stepLinearVelocity =
               data.RigidBody->LinearVelocity + data.RigidBody->_RootMotionLinearVelocity;
#if !defined(CLAYMORE_RUNTIME)
            data.RigidBody->_EditorDisplayLinearVelocity = stepLinearVelocity;
#endif
            Physics::Get().MoveKinematicBody(
               data.RigidBody->BodyID,
               stepLinearVelocity,
               data.RigidBody->AngularVelocity,
               dt);
            data.RigidBody->_RootMotionLinearVelocity = glm::vec3(0.0f);
         }
      }

      // Runtime-world participant lists can lag behind same-frame component changes.
      // Flush pending managed physics commands by component state as a final guard
      // before the Jolt step, so one-shot forces/impulses are not lost to indexing.
      for (auto& [entityId, data] : m_Entities) {
         (void)entityId;
         if (data.RigidBody && !data.RigidBody->BodyID.IsInvalid() && !data.RigidBody->IsKinematic) {
            ApplyPendingRigidBodyCommands(*data.RigidBody);
         }
      }

      if (HasPendingTransformUpdates()) {
         ScopedTimer t("Transforms/PrePhysics");
         UpdateTransforms();
         SyncPhysicsBodiesFromSceneTransforms(true);
         SyncCameraComponentsFromTransforms();
      }

      SoftbodySystem::PrePhysics(*this, dt);

      {
         ScopedTimer t("Physics/Step");
         Physics::Get().Step(dt);
      }

      SoftbodySystem::PostPhysics(*this);

      // Update area system (custom area-area overlap detection)
      if (Physics::Get().GetAreaSystem()) {
         Physics::Get().GetAreaSystem()->OnUpdate(dt);
      }

      auto& profiler = Profiler::Get();
      profiler.SetCounter("Scene/Entities", static_cast<uint64_t>(m_Entities.size()));
      uint64_t physicsEntitiesScanned = 0;
      uint64_t physicsBodiesSynced = 0;
      uint64_t scriptsEntitiesScanned = 0;
      uint64_t scriptsEntitiesUpdated = 0;
      uint64_t scriptsInstancesUpdated = 0;
      uint64_t scriptsLodSkipped = 0;
      const bool collectDetailedPrefabPerf = cm::debug::PrefabPerfDetailedTimingsEnabled();
      struct PrefabScriptPerfSample {
         double TotalMs = 0.0;
         uint64_t EntitiesUpdated = 0;
         uint64_t ScriptInstancesUpdated = 0;
         std::unordered_map<std::string, uint64_t> ScriptClasses;
      };
      std::unordered_map<EntityID, PrefabScriptPerfSample> prefabScriptPerf;
      if (collectDetailedPrefabPerf) {
         prefabScriptPerf.reserve(16);
      }

      auto syncDynamicRigidBodyFromPhysics = [&](EntityID id, EntityData& data) -> bool {
         if (!data.RigidBody || data.RigidBody->BodyID.IsInvalid() || data.RigidBody->IsKinematic) {
            return false;
         }

         JPH::BodyInterface& bodyInterface = Physics::Get().GetBodyInterface();
         const JPH::Vec3 linearVelocity = bodyInterface.GetLinearVelocity(data.RigidBody->BodyID);
         const JPH::Vec3 angularVelocity = bodyInterface.GetAngularVelocity(data.RigidBody->BodyID);
         data.RigidBody->LinearVelocity = glm::vec3(linearVelocity.GetX(), linearVelocity.GetY(), linearVelocity.GetZ());
         data.RigidBody->AngularVelocity = glm::vec3(angularVelocity.GetX(), angularVelocity.GetY(), angularVelocity.GetZ());
#if !defined(CLAYMORE_RUNTIME)
         if (!data.RigidBody->IsKinematic) {
            data.RigidBody->_EditorDisplayLinearVelocity = data.RigidBody->LinearVelocity;
         }
#endif

         // For dynamic bodies, only update gravity factor when it actually changes.
         const float targetGravityFactor = data.RigidBody->UseGravity ? 1.0f : 0.0f;
         if (std::abs(data.RigidBody->_LastAppliedGravityFactor - targetGravityFactor) > 1e-5f) {
            bodyInterface.SetGravityFactor(data.RigidBody->BodyID, targetGravityFactor);
            data.RigidBody->_LastAppliedGravityFactor = targetGravityFactor;
         }

         const bool shouldSyncDynamic = bodyInterface.IsActive(data.RigidBody->BodyID);
         if (!shouldSyncDynamic) {
            return false;
         }

         glm::mat4 physicsWorld = Physics::Get().GetBodyTransform(data.RigidBody->BodyID);
         if (physicsWorld == glm::mat4(0.0f)) {
            return false;
         }

         if (data.Collider) {
            physicsWorld = RemoveColliderOffsetFromBodyTransform(physicsWorld, data.Transform, data.Collider->Offset);
         }
         glm::mat4 parentWorld = glm::mat4(1.0f);

         if (data.Parent != INVALID_ENTITY_ID)
         {
            auto* p = GetEntityData(data.Parent);
            if (p)
            {
               parentWorld = p->Transform.WorldMatrix;
            }
         }
         glm::mat4 localM = glm::inverse(parentWorld) * physicsWorld;
         glm::vec3 t, s, skew; glm::vec4 persp; glm::quat rq;
         glm::decompose(localM, s, rq, t, skew, persp);

         return ApplyPhysicsDrivenLocalTransformIfChanged(*this, id, data, t, rq);
      };

      std::unordered_set<EntityID> physicsParticipantSet;
      physicsParticipantSet.reserve(physicsParticipantIds->size());
      
      for (EntityID id : *physicsParticipantIds) {
         physicsParticipantSet.insert(id);
         auto it = m_Entities.find(id);
         if (it == m_Entities.end()) continue;  // Entity was removed
         auto& data = it->second;
         ++physicsEntitiesScanned;

         // Sync physics bodies with transforms
         if (data.RigidBody && !data.RigidBody->BodyID.IsInvalid()) {

            // Post-step: sync transforms. Kinematic bodies: read back target result; Dynamic: read back simulated
            if (data.RigidBody->IsKinematic) {
               glm::mat4 physicsWorld = Physics::Get().GetBodyTransform(data.RigidBody->BodyID);

               if (physicsWorld != glm::mat4(0.0f)) {
                  if (data.Collider) {
                     physicsWorld = RemoveColliderOffsetFromBodyTransform(physicsWorld, data.Transform, data.Collider->Offset);
                  }
                  glm::mat4 parentWorld = glm::mat4(1.0f);

                  if (data.Parent != INVALID_ENTITY_ID)
                  {
                     auto* p = GetEntityData(data.Parent);
                     if (p)
                     {
                        parentWorld = p->Transform.WorldMatrix;
                     }
                  }

                  glm::mat4 localM = glm::inverse(parentWorld) * physicsWorld;
                  glm::vec3 t, s, skew; glm::vec4 persp; glm::quat rq;
                  glm::decompose(localM, s, rq, t, skew, persp);

                  if (ApplyPhysicsDrivenLocalTransformIfChanged(*this, id, data, t, rq)) {
                     ++physicsBodiesSynced;
                  }
               }
            }
            else {
               if (syncDynamicRigidBodyFromPhysics(id, data)) {
                  ++physicsBodiesSynced;
               }
            }
         }

         // Sync static bodies (they don't move, but we need to ensure they're positioned correctly)
         if (data.StaticBody && !data.StaticBody->BodyID.IsInvalid()) {
            // Static bodies don't move, but we can sync their initial position
            // This is mainly for when static bodies are created
         }
      }

      // RuntimeWorld owns the normal participant list, but forces/impulses are
      // one-shot native physics commands. If the list lags a same-frame binding
      // change, still pull simulated dynamic bodies back into the scene graph.
      for (auto& [entityId, data] : m_Entities) {
         if (physicsParticipantSet.find(entityId) != physicsParticipantSet.end()) {
            continue;
         }

         if (syncDynamicRigidBodyFromPhysics(entityId, data)) {
            ++physicsBodiesSynced;
         }
      }

      if (HasPendingTransformUpdates()) {
         ScopedTimer t("Transforms/PostPhysics");
         UpdateTransforms();
         physicsBodiesSynced += SyncPhysicsBodiesFromSceneTransforms(true);
      }

      SyncCameraComponentsFromTransforms();

      // Dispatch physics-driven callbacks after transforms have been synchronized
      // from the latest body state. This keeps managed collision handlers from
      // observing stale projectile / rigidbody positions from the previous frame.
      if (Physics::Get().GetAreaSystem()) {
         Physics::Get().GetAreaSystem()->DispatchEventsToInterop();
      }
      if (Physics::Get().GetCollisionSystem()) {
         Physics::Get().GetCollisionSystem()->DispatchEventsToInterop();
      }

      for (EntityID id : *scriptEntityIds) {
          auto it = m_Entities.find(id);
          if (it == m_Entities.end()) continue;  // Entity was removed
          auto& data = it->second;

          // Skip scripts if entity is not Active
          if (!data.Active) {
             continue;
         }
         if (data.Scripts.empty()) {
            continue;
         }
         ++scriptsEntitiesScanned;

         bool shouldUpdateScripts = true;
         auto eligibilityIt = scriptEntityShouldUpdate.find(id);
         if (eligibilityIt != scriptEntityShouldUpdate.end()) {
            shouldUpdateScripts = eligibilityIt->second;
         }
         if (!shouldUpdateScripts) {
            ++scriptsLodSkipped;
         }
         
         if (shouldUpdateScripts) {
            const EntityID prefabPerfRoot = collectDetailedPrefabPerf
               ? cm::debug::ResolveOwningPrefabRoot(*this, id)
               : INVALID_ENTITY_ID;
            const bool collectPrefabPerfForEntity =
               collectDetailedPrefabPerf && prefabPerfRoot != INVALID_ENTITY_ID;
            const auto prefabPerfStart = collectPrefabPerfForEntity
               ? std::chrono::high_resolution_clock::now()
               : std::chrono::high_resolution_clock::time_point{};
            uint64_t entityScriptInstancesUpdated = 0;
            std::vector<std::string> entityScriptClasses;
            if (collectPrefabPerfForEntity) {
               entityScriptClasses.reserve(data.Scripts.size());
            }

            ScopedTimer tScript("Scripts/Update");
            ++scriptsEntitiesUpdated;
            for (auto& script : data.Scripts) {
               if (ScriptSystem::Instance().HasScriptFlag(
                      script.ClassName,
                      ScriptSystem::ScriptType_PreAnimationUpdate)) {
                  continue;
               }

               if (script.Instance) {
                  ++scriptsInstancesUpdated;
                  ++entityScriptInstancesUpdated;
                  if (collectPrefabPerfForEntity) {
                     entityScriptClasses.push_back(script.ClassName);
                  }
                  // PERF: Only profile if enabled to avoid expensive clock calls in hot path
                  if (profiler.IsEnabled()) {
                     auto scriptStart = std::chrono::high_resolution_clock::now();
                     script.Instance->OnUpdate(dt);
                     auto scriptEnd = std::chrono::high_resolution_clock::now();
                     double ms = std::chrono::duration<double, std::milli>(scriptEnd - scriptStart).count();
                     profiler.RecordScriptSample(script.ClassName, ms);
                  } else {
                     // Fast path: no profiling overhead
                     script.Instance->OnUpdate(dt);
                  }
               }
            }

            if (collectPrefabPerfForEntity) {
               const auto prefabPerfEnd = std::chrono::high_resolution_clock::now();
               const double entityPrefabMs =
                  std::chrono::duration<double, std::milli>(prefabPerfEnd - prefabPerfStart).count();
               auto& sample = prefabScriptPerf[prefabPerfRoot];
               sample.TotalMs += entityPrefabMs;
               ++sample.EntitiesUpdated;
               sample.ScriptInstancesUpdated += entityScriptInstancesUpdated;
               for (const std::string& className : entityScriptClasses) {
                  ++sample.ScriptClasses[className];
               }
            }
         }


      }
      profiler.SetCounter("Physics/EntitiesScanned", physicsEntitiesScanned);
      profiler.SetCounter("Physics/BodiesSynced", physicsBodiesSynced);
      profiler.SetCounter("Scripts/EntitiesScanned", scriptsEntitiesScanned);
      profiler.SetCounter("Scripts/EntitiesUpdated", scriptsEntitiesUpdated);
      profiler.SetCounter("Scripts/InstancesUpdated", scriptsInstancesUpdated);
      profiler.SetCounter("Scripts/LodEligible", scriptsLodEligible);
      profiler.SetCounter("Scripts/LodSkipped", scriptsLodSkipped);
      profiler.SetCounter("Scripts/PrefabRootsUpdated", static_cast<uint64_t>(prefabScriptPerf.size()));
      if (collectDetailedPrefabPerf) {
         for (const auto& [rootId, sample] : prefabScriptPerf) {
            const auto label = cm::debug::DescribePrefabRoot(*this, rootId);
            if (!label.IsValid()) {
               continue;
            }
            profiler.Record(cm::debug::MakePrefabProfilerSection("Scripts/Prefab", label), sample.TotalMs);
         }

         if (cm::debug::PrefabPerfConsoleLoggingEnabled()) {
            static uint64_t s_PrefabScriptLogFrame = 0;
            const uint32_t logInterval = cm::debug::PrefabPerfConsoleLogInterval();
            ++s_PrefabScriptLogFrame;
            if (logInterval > 0 && (s_PrefabScriptLogFrame % logInterval) == 0u && !prefabScriptPerf.empty()) {
               std::vector<std::pair<EntityID, const PrefabScriptPerfSample*>> ordered;
               ordered.reserve(prefabScriptPerf.size());
               for (const auto& entry : prefabScriptPerf) {
                  ordered.emplace_back(entry.first, &entry.second);
               }
               std::sort(ordered.begin(), ordered.end(),
                  [](const auto& lhs, const auto& rhs) {
                     return lhs.second->TotalMs > rhs.second->TotalMs;
                  });

               const size_t limit = std::min<size_t>(3, ordered.size());
               std::cout << "[PrefabPerf][Scripts] Top prefab roots this frame:" << std::endl;
               for (size_t i = 0; i < limit; ++i) {
                  const EntityID rootId = ordered[i].first;
                  const PrefabScriptPerfSample& sample = *ordered[i].second;
                  const auto label = cm::debug::DescribePrefabRoot(*this, rootId);

                  std::vector<std::pair<std::string, uint64_t>> classCounts(
                     sample.ScriptClasses.begin(), sample.ScriptClasses.end());
                  std::sort(classCounts.begin(), classCounts.end(),
                     [](const auto& lhs, const auto& rhs) {
                        return lhs.second > rhs.second;
                     });

                  std::ostringstream classes;
                  const size_t classLimit = std::min<size_t>(3, classCounts.size());
                  for (size_t classIndex = 0; classIndex < classLimit; ++classIndex) {
                     if (classIndex > 0) {
                        classes << ", ";
                     }
                     classes << classCounts[classIndex].first << "=" << classCounts[classIndex].second;
                  }

                  std::cout << "[PrefabPerf][Scripts]   " << (i + 1) << ". "
                     << cm::debug::MakePrefabDebugLabel(label)
                     << " total=" << sample.TotalMs << "ms"
                     << " entities=" << sample.EntitiesUpdated
                     << " instances=" << sample.ScriptInstancesUpdated;
                  if (!classCounts.empty()) {
                     std::cout << " classes={" << classes.str() << "}";
                  }
                  std::cout << std::endl;
               }
            }
         }
      }

      // Update managed button events (poll native button state)
      {
         ScopedTimer t("Buttons/Update");
         cm::script::UpdateButtons();
      }

      // Flush managed SynchronizationContext so that 
      // await continuations run on the main thread
      {
         ScopedTimer t("SyncContext");
         cm::script::FlushSyncContext();
      }

      {
         ScopedTimer t("Multiplayer/PostUpdate");
         cm::multiplayer::PostUpdate(dt);
      }

      if (HasPendingTransformUpdates()) {
         ScopedTimer t("Transforms/PostSimulation");
         UpdateTransforms();
         SyncPhysicsBodiesFromSceneTransforms(true);
         SyncCameraComponentsFromTransforms();
      }
   }

   // Step particles from the frame's final pose so bone-attached/local-space
   // emitters stay locked to late physics/script movement in both play mode
   // and exported runtime.
   {
      ScopedTimer t("Particles");
      ecs::ParticleEmitterSystem::Get().Update(*this, dt);
   }
   m_RuntimeWorldFrameSyncLocked = false;
}

bool Scene::HasComponent(const char* componentName) {
   for (const auto& entity : m_EntityList) {
      const EntityData* data = GetEntityData(entity.GetID());
      if (!data)
      {
         continue;
      }
      if (strcmp(componentName, "MeshComponent") == 0 && data->Mesh)
         return true;
      if (strcmp(componentName, "LightComponent") == 0 && data->Light)
         return true;
      if (strcmp(componentName, "ColliderComponent") == 0 && data->Collider)
         return true;
      if (strcmp(componentName, "CameraComponent") == 0 && data->Camera)
         return true;
      if (strcmp(componentName, "AudioSourceComponent") == 0 && data->AudioSource)
         return true;
      if (strcmp(componentName, "AudioListenerComponent") == 0 && data->AudioListener)
         return true;
      if (strcmp(componentName, "RigidBodyComponent") == 0 && data->RigidBody)
         return true;
      if (strcmp(componentName, "StaticBodyComponent") == 0 && data->StaticBody)
         return true;
      if (strcmp(componentName, "SoftbodyComponent") == 0 && data->Softbody)
         return true;
      if (strcmp(componentName, "BlendShapeComponent") == 0 && data->BlendShapes)
         return true;
      if (strcmp(componentName, "SkeletonComponent") == 0 && data->Skeleton)
         return true;
      if (strcmp(componentName, "SkinningComponent") == 0 && data->Skinning)
         return true;
      if (strcmp(componentName, "CharacterControllerComponent") == 0 && data->CharacterController)
         return true;
      if (strcmp(componentName, "CanvasComponent") == 0 && data->Canvas)
         return true;
      if (strcmp(componentName, "PanelComponent") == 0 && data->Panel)
         return true;
      if (strcmp(componentName, "ButtonComponent") == 0 && data->Button)
         return true;
      if ((strcmp(componentName, "PortalComponent") == 0 || strcmp(componentName, "Portal") == 0) && data->Portal)
         return true;
   }
   return false;
}

EntityID Scene::GetActiveCameraEntityID() {
   int minPriority = std::numeric_limits<int>::max();
   EntityID selectedEntity = INVALID_ENTITY_ID;

   if (m_RuntimeWorldBridge) {
      for (EntityID id : m_RuntimeWorldBridge->GetCameraSceneEntities()) {
         auto* data = GetEntityData(id);
         if (data && data->Camera && data->Camera->Active) {
            if (data->Camera->priority < minPriority) {
               minPriority = data->Camera->priority;
               selectedEntity = id;
            }
         }
      }
   } else {
      for (const auto& entity : m_EntityList) {
         auto* data = GetEntityData(entity.GetID());
         if (data && data->Camera && data->Camera->Active) {
            if (data->Camera->priority < minPriority) {
               minPriority = data->Camera->priority;
               selectedEntity = entity.GetID();
            }
         }
      }
   }

   return selectedEntity;
}

Camera* Scene::GetActiveCamera() {
   const EntityID selectedEntity = GetActiveCameraEntityID();
   if (selectedEntity == INVALID_ENTITY_ID) {
      return nullptr;
   }

   auto* entityData = GetEntityData(selectedEntity);
   return entityData ? &entityData->Camera->Camera : nullptr;
}

bool Scene::AddDynamicComponent(EntityID id, const cm::TypeId& typeId) {
   auto* d = GetEntityData(id);
   if (!d) return false;
   if (d->Dynamic.find(typeId) != d->Dynamic.end()) return true;
   cm::ModuleComponent comp(typeId, 1);
   if (const auto* desc = cm::ComponentRegistry::Instance().Find(typeId)) {
      comp.SetVersion(desc->version);
      comp.DefineFields(desc->fields);
   }
   d->Dynamic.emplace(typeId, std::move(comp));
   MarkDirty();
   return true;
}

bool Scene::RemoveDynamicComponent(EntityID id, const cm::TypeId& typeId) {
   auto* d = GetEntityData(id);
   if (!d) return false;
   auto it = d->Dynamic.find(typeId);
   if (it == d->Dynamic.end()) return false;
   d->Dynamic.erase(it);
   MarkDirty();
   return true;
}

cm::ModuleComponent* Scene::GetDynamicComponent(EntityID id, const cm::TypeId& typeId) {
   auto* d = GetEntityData(id);
   if (!d) return nullptr;
   auto it = d->Dynamic.find(typeId);
   return it == d->Dynamic.end() ? nullptr : &it->second;
}


