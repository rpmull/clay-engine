#include "SoftbodySystem.h"

#include "Scene.h"
#include "EntityData.h"
#include "Components.h"
#include "core/physics/Physics.h"
#include "core/rendering/Mesh.h"
#include "core/rendering/VertexTypes.h"

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>

#include <bgfx/bgfx.h>
#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace {

constexpr float kWeightEpsilon = 1.0e-4f;

JPH::SoftBodySharedSettings::EBendType ToJoltBendMode(SoftbodyBendMode mode)
{
   switch (mode) {
      case SoftbodyBendMode::None:
         return JPH::SoftBodySharedSettings::EBendType::None;
      case SoftbodyBendMode::Dihedral:
         return JPH::SoftBodySharedSettings::EBendType::Dihedral;
      case SoftbodyBendMode::Distance:
      default:
         return JPH::SoftBodySharedSettings::EBendType::Distance;
   }
}

void DestroyMeshBuffers(Mesh& mesh)
{
   if (bgfx::isValid(mesh.vbh)) {
      bgfx::destroy(mesh.vbh);
      mesh.vbh = BGFX_INVALID_HANDLE;
   }
   if (bgfx::isValid(mesh.dvbh)) {
      bgfx::destroy(mesh.dvbh);
      mesh.dvbh = BGFX_INVALID_HANDLE;
   }
   if (bgfx::isValid(mesh.ibh)) {
      bgfx::destroy(mesh.ibh);
      mesh.ibh = BGFX_INVALID_HANDLE;
   }
   mesh.pendingBuffer.reset();
}

bool CreateDynamicBuffers(Mesh& mesh)
{
   DestroyMeshBuffers(mesh);

   if (mesh.Vertices.empty() || mesh.Indices.empty()) {
      return false;
   }

   const size_t vertexCount = mesh.Vertices.size();
   std::vector<PBRVertex> vertices(vertexCount);
   for (size_t i = 0; i < vertexCount; ++i) {
      PBRVertex& v = vertices[i];
      v.x = mesh.Vertices[i].x;
      v.y = mesh.Vertices[i].y;
      v.z = mesh.Vertices[i].z;
      v.nx = i < mesh.Normals.size() ? mesh.Normals[i].x : 0.0f;
      v.ny = i < mesh.Normals.size() ? mesh.Normals[i].y : 1.0f;
      v.nz = i < mesh.Normals.size() ? mesh.Normals[i].z : 0.0f;
      v.u = i < mesh.UVs.size() ? mesh.UVs[i].x : 0.0f;
      v.v = i < mesh.UVs.size() ? mesh.UVs[i].y : 0.0f;
   }

   const bgfx::Memory* vbMem = bgfx::copy(
      vertices.data(),
      static_cast<uint32_t>(vertices.size() * sizeof(PBRVertex)));
   mesh.dvbh = bgfx::createDynamicVertexBuffer(vbMem, PBRVertex::layout);

   const bool use32BitIndices =
      !mesh.Indices.empty() &&
      (*std::max_element(mesh.Indices.begin(), mesh.Indices.end()) > std::numeric_limits<uint16_t>::max());

   if (use32BitIndices) {
      const bgfx::Memory* ibMem = bgfx::copy(
         mesh.Indices.data(),
         static_cast<uint32_t>(mesh.Indices.size() * sizeof(uint32_t)));
      mesh.ibh = bgfx::createIndexBuffer(ibMem, BGFX_BUFFER_INDEX32);
   }
   else {
      std::vector<uint16_t> indices16(mesh.Indices.size());
      for (size_t i = 0; i < mesh.Indices.size(); ++i) {
         indices16[i] = static_cast<uint16_t>(mesh.Indices[i]);
      }
      const bgfx::Memory* ibMem = bgfx::copy(
         indices16.data(),
         static_cast<uint32_t>(indices16.size() * sizeof(uint16_t)));
      mesh.ibh = bgfx::createIndexBuffer(ibMem);
   }

   mesh.Dynamic = true;
   mesh.vbh = BGFX_INVALID_HANDLE;
   mesh.numVertices = static_cast<uint32_t>(vertexCount);
   mesh.numIndices = static_cast<uint32_t>(mesh.Indices.size());
   return bgfx::isValid(mesh.dvbh) && bgfx::isValid(mesh.ibh);
}

std::shared_ptr<Mesh> CloneMeshForSoftbodyRuntime(const Mesh& source)
{
   auto clone = std::make_shared<Mesh>();
   clone->Dynamic = true;
   clone->SkinnedLayout = false;
   clone->Vertices = source.Vertices;
   clone->Normals = source.Normals;
   clone->UVs = source.UVs;
   clone->Indices = source.Indices;
   clone->Submeshes = source.Submeshes;
   clone->BoundsMin = source.BoundsMin;
   clone->BoundsMax = source.BoundsMax;
   clone->numVertices = source.numVertices;
   clone->numIndices = source.numIndices;
   clone->BoneWeights.clear();
   clone->BoneIndices.clear();
   clone->SkinningBoneRemap.clear();

   if (!CreateDynamicBuffers(*clone)) {
      return nullptr;
   }

   return clone;
}

bool HasAnyAnchors(const SoftbodyComponent& softbody)
{
   return std::any_of(
      softbody.AnchorVertices.begin(),
      softbody.AnchorVertices.end(),
      [](uint8_t value) { return value != 0; });
}

bool BuildSharedSettings(const EntityData& data,
                         const Mesh& sourceMesh,
                         SoftbodyComponent& softbody,
                         JPH::Ref<JPH::SoftBodySharedSettings>& outSettings)
{
   if (sourceMesh.Vertices.empty() || sourceMesh.Indices.size() < 3) {
      return false;
   }

   JPH::Ref<JPH::SoftBodySharedSettings> settings = new JPH::SoftBodySharedSettings();
   settings->mVertices.reserve(static_cast<int>(sourceMesh.Vertices.size()));

   const bool enableLRA = softbody.EnableLongRangeAttachments && HasAnyAnchors(softbody);
   std::vector<JPH::SoftBodySharedSettings::VertexAttributes> attributes;
   attributes.resize(sourceMesh.Vertices.size());

   for (size_t i = 0; i < sourceMesh.Vertices.size(); ++i) {
      const glm::vec3 worldPos = glm::vec3(data.Transform.WorldMatrix * glm::vec4(sourceMesh.Vertices[i], 1.0f));
      const bool anchored = i < softbody.AnchorVertices.size() && softbody.AnchorVertices[i] != 0;
      float weight = i < softbody.VertexWeights.size() ? softbody.VertexWeights[i] : 1.0f;
      weight = std::clamp(weight, 0.0f, 1.0f);

      JPH::SoftBodySharedSettings::Vertex vertex;
      vertex.mPosition = JPH::Float3(worldPos.x, worldPos.y, worldPos.z);
      vertex.mInvMass = anchored ? 0.0f : std::max(kWeightEpsilon, softbody.WeightFloor + (1.0f - softbody.WeightFloor) * weight);
      settings->mVertices.push_back(vertex);

      attributes[i] = JPH::SoftBodySharedSettings::VertexAttributes(
         softbody.EdgeCompliance,
         softbody.ShearCompliance,
         softbody.BendCompliance,
         enableLRA ? JPH::SoftBodySharedSettings::ELRAType::EuclideanDistance : JPH::SoftBodySharedSettings::ELRAType::None,
         softbody.LRAMaxDistanceMultiplier);
   }

   for (size_t i = 0; i + 2 < sourceMesh.Indices.size(); i += 3) {
      const uint32_t i0 = sourceMesh.Indices[i + 0];
      const uint32_t i1 = sourceMesh.Indices[i + 1];
      const uint32_t i2 = sourceMesh.Indices[i + 2];
      if (i0 >= sourceMesh.Vertices.size() || i1 >= sourceMesh.Vertices.size() || i2 >= sourceMesh.Vertices.size()) {
         continue;
      }
      if (i0 == i1 || i0 == i2 || i1 == i2) {
         continue;
      }
      settings->AddFace(JPH::SoftBodySharedSettings::Face(i0, i1, i2));
   }

   if (settings->mFaces.empty()) {
      return false;
   }

   settings->CreateConstraints(
      attributes.data(),
      static_cast<JPH::uint>(attributes.size()),
      ToJoltBendMode(softbody.BendMode));
   settings->Optimize();

   outSettings = settings;
   return true;
}

void RecomputeNormals(const Mesh& mesh, std::vector<glm::vec3>& normals)
{
   normals.assign(mesh.Vertices.size(), glm::vec3(0.0f));

   for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3) {
      const uint32_t i0 = mesh.Indices[i + 0];
      const uint32_t i1 = mesh.Indices[i + 1];
      const uint32_t i2 = mesh.Indices[i + 2];
      if (i0 >= mesh.Vertices.size() || i1 >= mesh.Vertices.size() || i2 >= mesh.Vertices.size()) {
         continue;
      }

      const glm::vec3& p0 = mesh.Vertices[i0];
      const glm::vec3& p1 = mesh.Vertices[i1];
      const glm::vec3& p2 = mesh.Vertices[i2];
      const glm::vec3 faceNormal = glm::cross(p1 - p0, p2 - p0);
      if (glm::dot(faceNormal, faceNormal) <= 1.0e-12f) {
         continue;
      }

      normals[i0] += faceNormal;
      normals[i1] += faceNormal;
      normals[i2] += faceNormal;
   }

   for (glm::vec3& normal : normals) {
      const float lenSq = glm::dot(normal, normal);
      normal = lenSq > 1.0e-12f ? glm::normalize(normal) : glm::vec3(0.0f, 1.0f, 0.0f);
   }
}

bool UploadRuntimeMesh(Mesh& mesh, SoftbodyComponent& softbody)
{
   if (!bgfx::isValid(mesh.dvbh)) {
      if (!CreateDynamicBuffers(mesh)) {
         return false;
      }
   }

   softbody.ScratchVertices.resize(mesh.Vertices.size());
   for (size_t i = 0; i < mesh.Vertices.size(); ++i) {
      PBRVertex& v = softbody.ScratchVertices[i];
      v.x = mesh.Vertices[i].x;
      v.y = mesh.Vertices[i].y;
      v.z = mesh.Vertices[i].z;
      v.nx = i < mesh.Normals.size() ? mesh.Normals[i].x : 0.0f;
      v.ny = i < mesh.Normals.size() ? mesh.Normals[i].y : 1.0f;
      v.nz = i < mesh.Normals.size() ? mesh.Normals[i].z : 0.0f;
      v.u = i < mesh.UVs.size() ? mesh.UVs[i].x : 0.0f;
      v.v = i < mesh.UVs.size() ? mesh.UVs[i].y : 0.0f;
   }

   const bgfx::Memory* vbMem = bgfx::copy(
      softbody.ScratchVertices.data(),
      static_cast<uint32_t>(softbody.ScratchVertices.size() * sizeof(PBRVertex)));
   bgfx::update(mesh.dvbh, 0, vbMem);
   return true;
}

bool InitializeRuntime(Scene& scene, EntityID id, EntityData& data)
{
   if (!data.Softbody || !data.Mesh || !data.Mesh->mesh) {
      return false;
   }

   SoftbodyComponent& softbody = *data.Softbody;
   if (!softbody.SourceMesh || data.Mesh->mesh != softbody.RuntimeMesh) {
      softbody.SourceMesh = data.Mesh->mesh;
   }

   if (!softbody.SourceMesh) {
      return false;
   }

   if (!softbody.RuntimeMesh) {
      softbody.RuntimeMesh = CloneMeshForSoftbodyRuntime(*softbody.SourceMesh);
      if (!softbody.RuntimeMesh) {
         return false;
      }
   }

   data.Mesh->mesh = softbody.RuntimeMesh;

   JPH::Ref<JPH::SoftBodySharedSettings> settings;
   if (!BuildSharedSettings(data, *softbody.SourceMesh, softbody, settings)) {
      data.Mesh->mesh = softbody.SourceMesh;
      if (softbody.RuntimeMesh) {
         DestroyMeshBuffers(*softbody.RuntimeMesh);
         softbody.RuntimeMesh.reset();
      }
      return false;
   }

   JPH::SoftBodyCreationSettings creation(
      settings.GetPtr(),
      JPH::RVec3::sZero(),
      JPH::Quat::sIdentity(),
      static_cast<JPH::ObjectLayer>(std::min<uint32_t>(softbody.PhysicsLayer, MAX_PHYSICS_LAYERS - 1)));

   creation.mNumIterations = std::max<uint32_t>(1, softbody.SolverIterations);
   creation.mLinearDamping = std::max(0.0f, softbody.LinearDamping);
   creation.mMaxLinearVelocity = std::max(1.0f, softbody.MaxLinearVelocity);
   creation.mFriction = std::max(0.0f, softbody.Friction);
   creation.mRestitution = std::max(0.0f, softbody.Restitution);
   creation.mPressure = softbody.Pressure;
   creation.mGravityFactor = softbody.GravityFactor;
   creation.mVertexRadius = std::max(0.0f, softbody.VertexRadius);
   creation.mFacesDoubleSided = softbody.FacesDoubleSided;
   creation.mUserData = static_cast<uint64_t>(id);

   JPH::BodyInterface& bodyInterface = Physics::Get().GetBodyInterface();
   const JPH::BodyID bodyID = bodyInterface.CreateAndAddSoftBody(creation, JPH::EActivation::Activate);
   if (bodyID.IsInvalid()) {
      data.Mesh->mesh = softbody.SourceMesh;
      if (softbody.RuntimeMesh) {
         DestroyMeshBuffers(*softbody.RuntimeMesh);
         softbody.RuntimeMesh.reset();
      }
      return false;
   }

   softbody.BodyID = bodyID;
   softbody.RuntimeSharedSettings = settings;
   softbody.RuntimeNormals.resize(softbody.RuntimeMesh->Vertices.size(), glm::vec3(0.0f, 1.0f, 0.0f));
   return true;
}

bool SyncRuntimeMesh(EntityData& data)
{
   if (!data.Softbody || !data.Mesh || !data.Mesh->mesh) {
      return false;
   }

   SoftbodyComponent& softbody = *data.Softbody;
   Mesh& runtimeMesh = *data.Mesh->mesh;
   if (softbody.BodyID.IsInvalid()) {
      return false;
   }

   JPH::BodyLockRead lock(Physics::Get().GetSystem()->GetBodyLockInterface(), softbody.BodyID);
   if (!lock.Succeeded()) {
      return false;
   }

   const JPH::Body& body = lock.GetBody();
   if (!body.IsSoftBody()) {
      return false;
   }

   const auto* motionProperties = static_cast<const JPH::SoftBodyMotionProperties*>(body.GetMotionProperties());
   if (!motionProperties) {
      return false;
   }

   const auto& vertices = motionProperties->GetVertices();
   if (vertices.size() != runtimeMesh.Vertices.size()) {
      return false;
   }

   const glm::mat4 invWorld = glm::inverse(data.Transform.WorldMatrix);
   glm::vec3 boundsMin(std::numeric_limits<float>::max());
   glm::vec3 boundsMax(std::numeric_limits<float>::lowest());

   for (size_t i = 0; i < runtimeMesh.Vertices.size(); ++i) {
      const JPH::RVec3 worldPos = body.GetCenterOfMassTransform() * vertices[static_cast<int>(i)].mPosition;
      const glm::vec3 world(
         static_cast<float>(worldPos.GetX()),
         static_cast<float>(worldPos.GetY()),
         static_cast<float>(worldPos.GetZ()));
      const glm::vec3 local = glm::vec3(invWorld * glm::vec4(world, 1.0f));

      runtimeMesh.Vertices[i] = local;
      boundsMin = glm::min(boundsMin, local);
      boundsMax = glm::max(boundsMax, local);
   }

   runtimeMesh.BoundsMin = boundsMin;
   runtimeMesh.BoundsMax = boundsMax;

   RecomputeNormals(runtimeMesh, softbody.RuntimeNormals);
   runtimeMesh.Normals = softbody.RuntimeNormals;
   return UploadRuntimeMesh(runtimeMesh, softbody);
}

} // namespace

bool SoftbodySystem::SupportsMesh(const EntityData& data, std::string* outReason)
{
   auto fail = [&](const std::string& reason) -> bool {
      if (outReason) {
         *outReason = reason;
      }
      return false;
   };

   if (!data.Mesh || !data.Mesh->mesh) {
      return fail("Softbody requires a mesh asset on the entity.");
   }

   const Mesh& mesh = *data.Mesh->mesh;
   if (mesh.Vertices.empty()) {
      return fail("Softbody requires CPU vertex data on the mesh.");
   }
   if (mesh.Indices.empty() || (mesh.Indices.size() % 3) != 0) {
      return fail("Softbody requires triangle indices.");
   }
   if (mesh.HasSkinning()) {
      return fail("SoftbodySystem currently supports static meshes only.");
   }

   return true;
}

bool SoftbodySystem::EnsureAuthoringData(EntityData& data)
{
   if (!data.Softbody || !data.Mesh || !data.Mesh->mesh) {
      return false;
   }

   SoftbodyComponent& softbody = *data.Softbody;
   const Mesh& mesh = *data.Mesh->mesh;
   const size_t vertexCount = mesh.Vertices.size();
   const uint32_t indexCount = static_cast<uint32_t>(mesh.Indices.size());

   bool changed = false;

   if (softbody.VertexWeights.size() != vertexCount) {
      std::vector<float> resized(vertexCount, 1.0f);
      const size_t copyCount = std::min(resized.size(), softbody.VertexWeights.size());
      for (size_t i = 0; i < copyCount; ++i) {
         resized[i] = std::clamp(softbody.VertexWeights[i], 0.0f, 1.0f);
      }
      softbody.VertexWeights = std::move(resized);
      changed = true;
   }

   if (softbody.AnchorVertices.size() != vertexCount) {
      std::vector<uint8_t> resized(vertexCount, 0);
      const size_t copyCount = std::min(resized.size(), softbody.AnchorVertices.size());
      for (size_t i = 0; i < copyCount; ++i) {
         resized[i] = softbody.AnchorVertices[i] != 0 ? 1 : 0;
      }
      softbody.AnchorVertices = std::move(resized);
      changed = true;
   }

   if (softbody.SourceVertexCount != vertexCount || softbody.SourceIndexCount != indexCount) {
      softbody.SourceVertexCount = static_cast<uint32_t>(vertexCount);
      softbody.SourceIndexCount = indexCount;
      changed = true;
   }

   return changed;
}

void SoftbodySystem::PrePhysics(Scene& scene, float dt)
{
   (void)dt;

   for (const Entity& entity : scene.GetEntities()) {
      EntityData* data = scene.GetEntityData(entity.GetID());
      if (!data || !data->Softbody) {
         continue;
      }

      SoftbodyComponent& softbody = *data->Softbody;
      if (!softbody.Enabled) {
         if (!softbody.BodyID.IsInvalid() || softbody.RuntimeMesh) {
            ReleaseRuntime(*data, true);
         }
         continue;
      }

      EnsureAuthoringData(*data);

      std::string reason;
      if (!SupportsMesh(*data, &reason)) {
         if (!softbody.BodyID.IsInvalid() || softbody.RuntimeMesh) {
            ReleaseRuntime(*data, true);
         }
         continue;
      }

      if (softbody.BodyID.IsInvalid()) {
         InitializeRuntime(scene, entity.GetID(), *data);
      }
   }
}

void SoftbodySystem::PostPhysics(Scene& scene)
{
   for (const Entity& entity : scene.GetEntities()) {
      EntityData* data = scene.GetEntityData(entity.GetID());
      if (!data || !data->Softbody || data->Softbody->BodyID.IsInvalid()) {
         continue;
      }

      if (!data->Softbody->Enabled) {
         ReleaseRuntime(*data, true);
         continue;
      }

      SyncRuntimeMesh(*data);
   }
}

void SoftbodySystem::ReleaseRuntime(EntityData& data, bool destroyBody)
{
   if (!data.Softbody) {
      return;
   }

   SoftbodyComponent& softbody = *data.Softbody;

   if (destroyBody && !softbody.BodyID.IsInvalid()) {
      try {
         Physics::Get().DestroyBody(softbody.BodyID);
      }
      catch (...) {
      }
      softbody.BodyID = JPH::BodyID();
   }

   if (data.Mesh && softbody.SourceMesh && data.Mesh->mesh == softbody.RuntimeMesh) {
      data.Mesh->mesh = softbody.SourceMesh;
   }

   if (softbody.RuntimeMesh) {
      DestroyMeshBuffers(*softbody.RuntimeMesh);
   }

   softbody.BodyID = JPH::BodyID();
   softbody.RuntimeSharedSettings = nullptr;
   softbody.RuntimeMesh.reset();
   softbody.SourceMesh.reset();
   softbody.RuntimeNormals.clear();
   softbody.ScratchVertices.clear();
   softbody.ScratchSkinnedVertices.clear();
}
