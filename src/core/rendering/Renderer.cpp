#include "Renderer.h"
#include <bgfx/platform.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/constants.hpp>
#include "ShaderManager.h"
#include "RendererBackend.h"
#include "BgfxLifecycle.h"
#ifndef CLAYMORE_RUNTIME
#include "editor/rendering/Picking.h"
#include "editor/rendering/DebugMaterial.h"
#include "editor/EditorSettings.h"
#endif
#include "MaterialManager.h"
#include "VertexTypes.h"
#include "core/ecs/ParticleEmitterSystem.h"
#include <bx/math.h>
#include "core/rendering/MaterialPropertyBlock.h"
#include "core/rendering/PropertyID.h"
#include "core/rendering/SkinnedPBRMaterial.h"
#include "core/rendering/MaterialCache.h"
#include "core/rendering/MaterialInstance.h"
#include "core/rendering/StandardMeshManager.h"
#include "core/ecs/AnimationComponents.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/transform.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <cstring>
#include <chrono>
#include <iterator>
#include <mutex>
#include <future>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <iostream>
#include <stb_image.h>
#include "core/ecs/Components.h"
#include "core/vfs/FileSystem.h"
#include "core/vfs/VirtualFS.h"
#include "core/ecs/RenderOverridesComponent.h"
#include "core/physics/Physics.h"
#include "core/physics/PhysicsDebug.h"
#include "core/physics/ragdoll/RagdollSystem.h"
#include "core/jobs/Jobs.h"
#include "core/jobs/ParallelFor.h"
#include "core/rendering/TerrainStreaming.h"
#include "core/world/RuntimeWorld.h"

static const char* kFallbackTextFontPath = "assets/fonts/Roboto-Regular.ttf";
#include "Environment.h"


// Thread-local storage for tracking the last program submitted (for crash diagnostics)
// Note: Using pointer with lazy initialization to avoid static init order issues
static thread_local std::string* s_LastProgramNamePtr = nullptr;
static thread_local uint16_t s_LastProgramIdx = UINT16_MAX;
static std::mutex s_InvalidSubmitWarningMutex;
static std::unordered_set<std::string> s_InvalidSubmitWarnings;

static std::string& GetLastProgramName() {
    if (!s_LastProgramNamePtr) {
        s_LastProgramNamePtr = new std::string();
    }
    return *s_LastProgramNamePtr;
}

// Safe submit helper - logs the program being submitted for crash diagnostics
static inline void SafeSubmit(bgfx::ViewId viewId, bgfx::ProgramHandle program, const char* debugName = nullptr) {
    if (!bgfx::isValid(program)) {
        const std::string warningKey = debugName ? debugName : "unknown";
        bool shouldLog = false;
        {
            std::lock_guard<std::mutex> lock(s_InvalidSubmitWarningMutex);
            shouldLog = s_InvalidSubmitWarnings.insert(warningKey).second;
        }
        if (shouldLog) {
            std::cerr << "[Renderer] WARNING: Attempted to submit invalid program"
                      << (debugName ? std::string(" (") + debugName + ")" : "") << std::endl;
        }
        return;
    }
    s_LastProgramIdx = program.idx;
    GetLastProgramName() = debugName ? debugName : ShaderManager::Instance().GetProgramName(program);
    bgfx::submit(viewId, program);
}

static bool SubmitSingleSkinnedInstance(bgfx::ViewId viewId,
                                        bgfx::VertexBufferHandle vertexBuffer,
                                        bgfx::IndexBufferHandle indexBuffer,
                                        bgfx::ProgramHandle program,
                                        const cm::rendering::SkinnedInstanceData& instance,
                                        uint64_t stateFlags,
                                        const char* debugName,
                                        const std::function<void()>& bindCallback = {},
                                        uint32_t indexStart = 0u,
                                        uint32_t indexCount = UINT32_MAX)
{
    if (!bgfx::isValid(vertexBuffer) || !bgfx::isValid(indexBuffer)) {
        return false;
    }

    if (bgfx::getAvailInstanceDataBuffer(1u, sizeof(cm::rendering::SkinnedInstanceData)) == 0u) {
        return false;
    }

    bgfx::InstanceDataBuffer idb;
    bgfx::allocInstanceDataBuffer(&idb, 1u, sizeof(cm::rendering::SkinnedInstanceData));
    std::memcpy(idb.data, &instance, sizeof(instance));

    bgfx::setVertexBuffer(0, vertexBuffer);
    if (indexCount == UINT32_MAX) {
        bgfx::setIndexBuffer(indexBuffer);
    } else {
        bgfx::setIndexBuffer(indexBuffer, indexStart, indexCount);
    }
    bgfx::setInstanceDataBuffer(&idb);
    if (bindCallback) {
        bindCallback();
    }
    bgfx::setState(stateFlags);
    SafeSubmit(viewId, program, debugName);
    return true;
}

static inline uint64_t HashCombine64(uint64_t seed, uint64_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
    return seed;
}

static inline uint64_t HashBytes64(const void* data, size_t size, uint64_t seed = 1469598103934665603ull) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t hash = seed;
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint64_t>(bytes[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

static constexpr float kGpuMorphWeightThreshold = 1e-4f;

static bgfx::VertexLayout& GetComputeVec4Layout()
{
    static bgfx::VertexLayout layout;
    static bool initialized = false;
    if (!initialized) {
        layout.begin()
            .add(bgfx::Attrib::TexCoord7, 4, bgfx::AttribType::Float)
            .end();
        initialized = true;
    }
    return layout;
}

static uint64_t MakeGpuMaterializedSkinningInstanceKey(const Scene& scene, EntityID entityId)
{
    const uint64_t sceneBits = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&scene));
    return HashCombine64(sceneBits >> 4u, static_cast<uint64_t>(entityId));
}

static uint64_t HashMat4Stable(const glm::mat4& matrix)
{
    return HashBytes64(glm::value_ptr(matrix), sizeof(glm::mat4));
}

static uint64_t ComputeGpuMaterializedMorphFingerprint(const BlendShapeComponent* blendShapes)
{
    if (!blendShapes) {
        return 0ull;
    }

    uint64_t hash = 1469598103934665603ull;
    for (size_t shapeIndex = 0; shapeIndex < blendShapes->Shapes.size(); ++shapeIndex) {
        const BlendShape& shape = blendShapes->Shapes[shapeIndex];
        if (std::abs(shape.Weight) <= kGpuMorphWeightThreshold) {
            continue;
        }

        hash = HashCombine64(hash, static_cast<uint64_t>(shapeIndex));
        hash = HashCombine64(hash, HashBytes64(&shape.Weight, sizeof(shape.Weight)));
    }
    return hash;
}

static uint64_t ComputeGpuMaterializedSkinningFingerprint(const SkinningComponent* skinning,
                                                          const Mesh* mesh,
                                                          const BlendShapeComponent* blendShapes)
{
    if (!skinning || !mesh) {
        return 0ull;
    }

    uint64_t hash = 1469598103934665603ull;
    hash = HashCombine64(hash, reinterpret_cast<uint64_t>(skinning->GpuSourceSkeleton));
    if (skinning->GpuSourceSkeleton) {
        hash = HashCombine64(hash, static_cast<uint64_t>(skinning->GpuSourceSkeleton->PoseFrameId));
        hash = HashCombine64(hash, static_cast<uint64_t>(skinning->GpuSourceSkeleton->BoneCount));
    }
    hash = HashCombine64(hash, HashMat4Stable(skinning->GpuMeshFromSkeleton));
    hash = HashCombine64(hash, static_cast<uint64_t>(skinning->BoneCount));
    hash = HashCombine64(hash, skinning->GetGpuBoneIndexRemapHash());
    hash = HashCombine64(hash, skinning->GetGpuBoneCorrectionPaletteHash());
    hash = HashCombine64(hash, mesh->GetCachedSkinningBoneRemapHash());
    hash = HashCombine64(hash, ComputeGpuMaterializedMorphFingerprint(blendShapes));
    return hash;
}

// Get the last program info for crash diagnostics
static std::string GetLastSubmitInfo() {
    return "Last submit: " + GetLastProgramName() + " (idx=" + std::to_string(s_LastProgramIdx) + ")";
}

static inline bool IsPresentationVisible(const EntityData* data) {
    return data && data->Visible && !data->PresentationHidden;
}

static glm::mat4 BuildCanvasBillboardMatrix(const glm::mat4& worldMatrix, const glm::mat4& viewMatrix) {
    glm::vec3 position = glm::vec3(worldMatrix[3]);
    glm::vec3 scale(
        glm::max(glm::length(glm::vec3(worldMatrix[0])), 0.0001f),
        glm::max(glm::length(glm::vec3(worldMatrix[1])), 0.0001f),
        glm::max(glm::length(glm::vec3(worldMatrix[2])), 0.0001f));

    glm::mat4 cameraWorld = glm::inverse(viewMatrix);
    glm::mat3 cameraRotation(cameraWorld);

    glm::mat4 billboard(1.0f);
    billboard[0] = glm::vec4(cameraRotation[0] * scale.x, 0.0f);
    billboard[1] = glm::vec4(cameraRotation[1] * scale.y, 0.0f);
    billboard[2] = glm::vec4(cameraRotation[2] * scale.z, 0.0f);
    billboard[3] = glm::vec4(position, 1.0f);
    return billboard;
}

static uint32_t ResolveWorldCanvasRenderDimension(int explicitSize, int referenceSize, uint32_t fallbackSize) {
    if (explicitSize > 0) {
        return static_cast<uint32_t>(explicitSize);
    }
    if (referenceSize > 0) {
        return static_cast<uint32_t>(referenceSize);
    }
    return std::max<uint32_t>(fallbackSize, 1u);
}

#include "TextRenderer.h"
#include "TextureLoader.h"
#include "TextureCube.h"
#ifndef CLAYMORE_RUNTIME
#include "editor/pipeline/AssetLibrary.h"
#else
#include "core/assets/IAssetResolver.h"
#endif
#include "core/input/Input.h"

#ifndef CLAYMORE_RUNTIME
#include "editor/application.h"
#endif
#include "Terrain.h"
#include "TerrainClipmaps.h"
#include "TerrainChunks.h"
#include "core/rendering/TerrainGrass.h"
#include "core/resourcelayer/ResourceLayerTypes.h"
#include "core/resourcelayer/ImposterManager.h"
#include "core/ecs/InstancerSystem.h"
#include "core/rendering/DeferredGPUBuffers.h"
#include <limits>
#include <algorithm>
#include <vector>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

#include "GlobalShaderProperties.h"
#include "core/physics/area/AreaComponent.h"
#include "core/utils/Profiler.h"

namespace
{
   constexpr uint16_t kMainWorldUIViewId = 2;
   constexpr uint16_t kMainScreenUIViewId = 3;
   constexpr uint16_t kDebugOverlayViewId = 4;
   constexpr uint16_t kSkinnedOcclusionViewId = 5;
   constexpr uint16_t kGpuMaterializedSkinningViewId = 0;
   constexpr uint8_t kSkinnedOcclusionInvisibleHysteresisFrames = 2;
   constexpr uint8_t kGpuSkinningBoneAtlasStage = 10;
   constexpr uint8_t kGpuSkinningRemapAtlasStage = 11;
   constexpr uint8_t kGpuSkinningInstanceAtlasStage = 12;
   constexpr uint8_t kGpuMorphVertexAtlasStage = 13;
   constexpr uint8_t kGpuMorphEntryAtlasStage = 14;
   constexpr uint8_t kGpuMorphActiveAtlasStage = 15;
   constexpr uint8_t kGpuMaterializedSkinningSourcePositionStage = 0;
   constexpr uint8_t kGpuMaterializedSkinningSourceNormalStage = 1;
   constexpr uint8_t kGpuMaterializedSkinningSourceUvStage = 2;
   constexpr uint8_t kGpuMaterializedSkinningSourceBoneIndexStage = 3;
   constexpr uint8_t kGpuMaterializedSkinningSourceBoneWeightStage = 4;
   constexpr uint8_t kGpuMaterializedSkinningOutputStage = 5;
   constexpr uint32_t kSkinningInstanceRecordTexelCount = 8u;
   constexpr uint32_t kGpuMorphMaxActiveShapes = 24u;
   constexpr uint32_t kGpuMorphMaxEntriesPerVertex = 32u;
   constexpr uint32_t kGpuMaterializedSkinningGroupSize = 64u;
   // GPU morphing is available only as a runtime-selected crowd path. The
   // SkinningSystem sets a per-mesh eligibility bit after proving that a real
   // batch exists and no CPU deformation consumer needs current vertices.
   constexpr bool kAllowGpuMorphTargets = true;

   struct MorphGpuGeometryInfo
   {
      uint32_t VertexRangeBase = 0;
      uint32_t EntryBase = 0;
      uint32_t EntryCount = 0;
   };

   inline glm::vec4 PackObjectIdColor(EntityID entityId)
   {
      if (entityId == INVALID_ENTITY_ID) {
         return glm::vec4(0.0f);
      }

      const uint32_t id = static_cast<uint32_t>(entityId) + 1u;
      return glm::vec4(
         static_cast<float>(id & 255u) / 255.0f,
         static_cast<float>((id >> 8) & 255u) / 255.0f,
         static_cast<float>((id >> 16) & 255u) / 255.0f,
         1.0f);
   }

   bgfx::TextureFormat::Enum ChooseSceneDepthFormat(uint64_t textureFlags)
   {
      const bgfx::TextureFormat::Enum candidates[] = {
         bgfx::TextureFormat::D32F,
         bgfx::TextureFormat::D24S8,
         bgfx::TextureFormat::D16
      };

      for (bgfx::TextureFormat::Enum format : candidates) {
         if (bgfx::isTextureValid(0, false, 1, format, textureFlags)) {
            return format;
         }
      }

      return bgfx::TextureFormat::D24S8;
   }

   bgfx::TextureHandle GetDummySkyboxTexture()
   {
      static bgfx::TextureHandle s_skyDummy = BGFX_INVALID_HANDLE;
      if (!bgfx::isValid(s_skyDummy)) {
         s_skyDummy = bgfx::createTextureCube(
            1, false, 1, bgfx::TextureFormat::RGBA8,
            BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_W_CLAMP);
         if (bgfx::isValid(s_skyDummy)) {
            const uint8_t white[4] = { 255, 255, 255, 255 };
            for (uint8_t face = 0; face < 6; ++face) {
               const bgfx::Memory* mem = bgfx::copy(white, sizeof(white));
               bgfx::updateTextureCube(s_skyDummy, 0, face, 0, 0, 0, 1, 1, mem);
            }
         }
      }
      return s_skyDummy;
   }

   inline bool NearlyEqual(float a, float b, float eps = 1e-4f)
   {
      return std::abs(a - b) <= eps;
   }

   inline bool NearlyEqualVec3(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f)
   {
      return NearlyEqual(a.x, b.x, eps) &&
             NearlyEqual(a.y, b.y, eps) &&
             NearlyEqual(a.z, b.z, eps);
   }

   inline uint64_t MakeSkinnedOcclusionKey(Scene& scene, EntityID skeletonRoot)
   {
      const uint64_t sceneBits = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&scene));
      return (sceneBits >> 4) ^ (static_cast<uint64_t>(skeletonRoot) * 0x9e3779b97f4a7c15ULL);
   }

   // Directional-light forward vector from transform.
   // Uses quaternion rotation when enabled so editor quaternion mode is honored.
   inline glm::vec3 LightDirectionFromTransform(const TransformComponent& transform)
   {
      if (transform.UseQuatRotation) {
         glm::quat q = glm::normalize(transform.RotationQ);
         glm::vec3 dir = q * glm::vec3(0.0f, 0.0f, 1.0f);
         if (glm::dot(dir, dir) > 1e-8f) {
            return glm::normalize(dir);
         }
      }

      float yaw = glm::radians(transform.Rotation.y);
      float pitch = glm::radians(transform.Rotation.x);
      return glm::normalize(glm::vec3(
         cos(pitch) * sin(yaw),
         sin(pitch),
         cos(pitch) * cos(yaw)
      ));
   }

   inline bool HasValidMeshGpuBuffers(const Mesh& mesh)
   {
      const bool vbValid = mesh.Dynamic ? bgfx::isValid(mesh.dvbh) : bgfx::isValid(mesh.vbh);
      return vbValid && bgfx::isValid(mesh.ibh);
   }

   inline bool UsesSkinnedObjectIdProgram(const EntityData* data)
   {
      return data != nullptr && data->Skinning != nullptr;
   }

   inline bool HasGpuMorphVertexSource(const Renderer& renderer, const EntityData* data, const Mesh* mesh)
   {
      return data != nullptr &&
         data->Skinning != nullptr &&
         mesh != nullptr &&
         mesh->Dynamic &&
         renderer.ShouldUseGpuMorphTargets(data->Skinning.get(), mesh, data->BlendShapes.get());
   }

   inline bool HasRenderableVertexSource(const Renderer& renderer, const EntityData* data, const Mesh* mesh)
   {
      if (!mesh || !bgfx::isValid(mesh->ibh)) {
         return false;
      }

      if (mesh->Dynamic) {
         return bgfx::isValid(mesh->dvbh) || HasGpuMorphVertexSource(renderer, data, mesh);
      }

      return bgfx::isValid(mesh->vbh);
   }

   inline EntityID ResolveSkinningSkeletonRootEntity(const EntityData* data)
   {
      if (!data || !data->Skinning) {
         return INVALID_ENTITY_ID;
      }

      EntityID skelRoot = data->Skinning->ResolvedSkeletonRoot;
      if (skelRoot == INVALID_ENTITY_ID || skelRoot == (EntityID)-1) {
         skelRoot = data->Skinning->SkeletonRoot;
      }
      return skelRoot;
   }

   inline EntityID ResolveActiveCameraOwnerEntity(Scene& scene)
   {
      const EntityID activeCameraEntity = scene.GetActiveCameraEntityID();
      if (activeCameraEntity == INVALID_ENTITY_ID) {
         return INVALID_ENTITY_ID;
      }

      EntityData* cameraData = scene.GetEntityData(activeCameraEntity);
      if (!cameraData) {
         return activeCameraEntity;
      }

      return cameraData->Parent != INVALID_ENTITY_ID
         ? cameraData->Parent
         : activeCameraEntity;
   }

   inline bool IsEntityDescendantOf(Scene& scene, EntityID entityId, EntityID ancestorId)
   {
      if (entityId == INVALID_ENTITY_ID || ancestorId == INVALID_ENTITY_ID) {
         return false;
      }

      EntityID current = entityId;
      while (current != INVALID_ENTITY_ID) {
         if (current == ancestorId) {
            return true;
         }

         EntityData* data = scene.GetEntityData(current);
         if (!data) {
            break;
         }

         current = data->Parent;
      }

      return false;
   }

   inline bool ComputeSkeletonWorldBounds(Scene& scene,
                                          EntityID skeletonEntity,
                                          glm::vec3& outMin,
                                          glm::vec3& outMax)
   {
      if (skeletonEntity == INVALID_ENTITY_ID) {
         return false;
      }

      auto* skelData = scene.GetEntityData(skeletonEntity);
      if (!skelData || !skelData->Skeleton || skelData->Skeleton->BoneEntities.empty()) {
         return false;
      }

      glm::vec3 minBounds(std::numeric_limits<float>::max());
      glm::vec3 maxBounds(-std::numeric_limits<float>::max());
      bool hasBounds = false;
      for (EntityID boneEntity : skelData->Skeleton->BoneEntities) {
         auto* boneData = scene.GetEntityData(boneEntity);
         if (!boneData) {
            continue;
         }

         const glm::vec3 bonePos = glm::vec3(boneData->Transform.WorldMatrix[3]);
         minBounds = glm::min(minBounds, bonePos);
         maxBounds = glm::max(maxBounds, bonePos);
         hasBounds = true;
      }

      if (!hasBounds) {
         return false;
      }

      outMin = minBounds;
      outMax = maxBounds;
      return true;
   }

   inline glm::vec3 ComputeMeshWorldExtents(const EntityData* data, const Mesh* mesh)
   {
      if (!data || !data->Mesh || !mesh) {
         return glm::vec3(0.0f);
      }

      const glm::vec3 localExtents =
         (mesh->BoundsMax - mesh->BoundsMin) * 0.5f * std::max(0.01f, data->Mesh->BoundsPadding);
      const glm::mat4& M = data->Transform.WorldMatrix;
      glm::vec3 extents;
      extents.x = std::abs(M[0][0]) * localExtents.x + std::abs(M[1][0]) * localExtents.y + std::abs(M[2][0]) * localExtents.z;
      extents.y = std::abs(M[0][1]) * localExtents.x + std::abs(M[1][1]) * localExtents.y + std::abs(M[2][1]) * localExtents.z;
      extents.z = std::abs(M[0][2]) * localExtents.x + std::abs(M[1][2]) * localExtents.y + std::abs(M[2][2]) * localExtents.z;
      return extents;
   }

   inline bool ComputeMeshWorldBounds(const EntityData* data,
                                      const Mesh* mesh,
                                      glm::vec3& outMin,
                                      glm::vec3& outMax)
   {
      if (!data || !data->Mesh || !mesh) {
         return false;
      }

      const glm::vec3 center = (mesh->BoundsMin + mesh->BoundsMax) * 0.5f;
      const glm::vec3 extents =
         (mesh->BoundsMax - mesh->BoundsMin) * 0.5f * std::max(0.01f, data->Mesh->BoundsPadding);
      const glm::vec3 lmin = center - extents;
      const glm::vec3 lmax = center + extents;
      const glm::vec3 corners[8] = {
         { lmin.x, lmin.y, lmin.z }, { lmax.x, lmin.y, lmin.z },
         { lmin.x, lmax.y, lmin.z }, { lmax.x, lmax.y, lmin.z },
         { lmin.x, lmin.y, lmax.z }, { lmax.x, lmin.y, lmax.z },
         { lmin.x, lmax.y, lmax.z }, { lmax.x, lmax.y, lmax.z }
      };

      outMin = glm::vec3(std::numeric_limits<float>::max());
      outMax = glm::vec3(-std::numeric_limits<float>::max());
      for (const glm::vec3& corner : corners) {
         const glm::vec3 worldCorner = glm::vec3(data->Transform.WorldMatrix * glm::vec4(corner, 1.0f));
         outMin = glm::min(outMin, worldCorner);
         outMax = glm::max(outMax, worldCorner);
      }
      return true;
   }

   inline bool ComputeSkinningGroupBoundsFromScene(Scene& scene,
                                                   const cm::world::RuntimeSkinningGroupCache& cache,
                                                   glm::vec3& outMin,
                                                   glm::vec3& outMax,
                                                   glm::vec3* outMaxExtent = nullptr)
   {
      glm::vec3 minBounds(std::numeric_limits<float>::max());
      glm::vec3 maxBounds(-std::numeric_limits<float>::max());
      glm::vec3 maxExtent(0.0f);
      bool hasBounds = false;

      for (EntityID meshSceneEntity : cache.MeshSceneEntities) {
         EntityData* meshData = scene.GetEntityData(meshSceneEntity);
         if (!meshData || !meshData->Mesh || !meshData->Mesh->mesh) {
            continue;
         }

         glm::vec3 meshMin(0.0f);
         glm::vec3 meshMax(0.0f);
         if (!ComputeMeshWorldBounds(meshData, meshData->Mesh->mesh.get(), meshMin, meshMax)) {
            continue;
         }

         minBounds = glm::min(minBounds, meshMin);
         maxBounds = glm::max(maxBounds, meshMax);
         maxExtent = glm::max(maxExtent, 0.5f * (meshMax - meshMin));
         hasBounds = true;
      }

      if (!hasBounds) {
         return false;
      }

      outMin = minBounds;
      outMax = maxBounds;
      if (outMaxExtent != nullptr) {
         *outMaxExtent = maxExtent;
      }
      return true;
   }

   inline bool TryGetSharedSkinnedCharacterBounds(
      Scene& scene,
      const EntityData* data,
      const Mesh* mesh,
      const cm::world::RuntimeWorld* runtimeWorld,
      cm::physics::RagdollSystem* ragdollSystem,
      std::unordered_map<EntityID, std::pair<glm::vec3, glm::vec3>>& boundsCache,
      glm::vec3& outMin,
      glm::vec3& outMax)
   {
      if (!data || !data->Skinning || !mesh) {
         return false;
      }

      const EntityID skelRoot = ResolveSkinningSkeletonRootEntity(data);
      if (skelRoot == INVALID_ENTITY_ID) {
         return false;
      }

      auto cacheIt = boundsCache.find(skelRoot);
      if (cacheIt != boundsCache.end()) {
         outMin = cacheIt->second.first;
         outMax = cacheIt->second.second;
         return true;
      }

      const cm::world::RuntimeSkinningGroupCache* skinningGroup =
         runtimeWorld ? runtimeWorld->TryGetSkinningGroupCache(skelRoot) : nullptr;

      glm::vec3 maxExtent(0.0f);
      bool haveExtent = false;
      if (skinningGroup && runtimeWorld) {
         haveExtent = runtimeWorld->TryGetSkinningGroupMaxWorldExtent(*skinningGroup, maxExtent);
      }

      glm::vec3 groupBoundsMin(0.0f);
      glm::vec3 groupBoundsMax(0.0f);
      bool haveGroupBounds = false;
      if (skinningGroup && skinningGroup->MeshSceneEntities.size() > 1) {
         if (runtimeWorld) {
            haveGroupBounds =
               runtimeWorld->TryGetSkinningGroupWorldBounds(*skinningGroup, groupBoundsMin, groupBoundsMax);
         }

         glm::vec3 sceneGroupMin(0.0f);
         glm::vec3 sceneGroupMax(0.0f);
         glm::vec3 sceneGroupExtent(0.0f);
         if (ComputeSkinningGroupBoundsFromScene(
                scene,
                *skinningGroup,
                sceneGroupMin,
                sceneGroupMax,
                &sceneGroupExtent)) {
            if (haveGroupBounds) {
               groupBoundsMin = glm::min(groupBoundsMin, sceneGroupMin);
               groupBoundsMax = glm::max(groupBoundsMax, sceneGroupMax);
            } else {
               groupBoundsMin = sceneGroupMin;
               groupBoundsMax = sceneGroupMax;
               haveGroupBounds = true;
            }
            maxExtent = glm::max(maxExtent, sceneGroupExtent);
            haveExtent = true;
         }
      }

      if (!haveExtent) {
         maxExtent = ComputeMeshWorldExtents(data, mesh);
      }

      glm::vec3 skeletonMin(0.0f);
      glm::vec3 skeletonMax(0.0f);
      bool haveSkeletonBounds = false;
      if (ragdollSystem) {
         haveSkeletonBounds =
            ragdollSystem->TryGetActiveSkeletonBounds(skelRoot, skeletonMin, skeletonMax) ||
            (ragdollSystem->IsSkeletonRagdollActive(skelRoot) &&
             ComputeSkeletonWorldBounds(scene, skelRoot, skeletonMin, skeletonMax));
      }
      if (!haveSkeletonBounds) {
         haveSkeletonBounds = ComputeSkeletonWorldBounds(scene, skelRoot, skeletonMin, skeletonMax);
      }

      if (haveSkeletonBounds) {
         skeletonMin -= maxExtent;
         skeletonMax += maxExtent;
      }

      if (haveGroupBounds || haveSkeletonBounds) {
         const glm::vec3 boundsMin =
            haveGroupBounds && haveSkeletonBounds ? glm::min(groupBoundsMin, skeletonMin)
                                                  : (haveGroupBounds ? groupBoundsMin : skeletonMin);
         const glm::vec3 boundsMax =
            haveGroupBounds && haveSkeletonBounds ? glm::max(groupBoundsMax, skeletonMax)
                                                  : (haveGroupBounds ? groupBoundsMax : skeletonMax);
         cacheIt = boundsCache.emplace(skelRoot, std::make_pair(boundsMin, boundsMax)).first;
         outMin = cacheIt->second.first;
         outMax = cacheIt->second.second;
         return true;
      }

      return false;
   }

   // Runtime safety net: if a mesh has CPU data but invalid GPU handles, rebuild buffers.
   // This prevents "needs restart to render" cases when deferred creation or handle state desync occurs.
   static bool TryRecoverMeshGpuBuffers(Mesh& mesh)
   {
      if (HasValidMeshGpuBuffers(mesh)) {
         return true;
      }
      if (mesh.Vertices.empty() || mesh.Indices.empty()) {
         return false;
      }

      // Clean up any partially-valid stale handles first.
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

      const size_t vertexCount = mesh.Vertices.size();
      const bool skinned = mesh.SkinnedLayout || mesh.HasSkinning();

      if (skinned) {
         std::vector<SkinnedPBRVertex> vertices(vertexCount);
         for (size_t i = 0; i < vertexCount; ++i) {
            SkinnedPBRVertex& v = vertices[i];
            v.x = mesh.Vertices[i].x;
            v.y = mesh.Vertices[i].y;
            v.z = mesh.Vertices[i].z;
            v.nx = i < mesh.Normals.size() ? mesh.Normals[i].x : 0.0f;
            v.ny = i < mesh.Normals.size() ? mesh.Normals[i].y : 1.0f;
            v.nz = i < mesh.Normals.size() ? mesh.Normals[i].z : 0.0f;
            v.u = i < mesh.UVs.size() ? mesh.UVs[i].x : 0.0f;
            v.v = i < mesh.UVs.size() ? mesh.UVs[i].y : 0.0f;
            if (i < mesh.BoneIndices.size()) {
               v.i0 = static_cast<uint8_t>(mesh.BoneIndices[i].x);
               v.i1 = static_cast<uint8_t>(mesh.BoneIndices[i].y);
               v.i2 = static_cast<uint8_t>(mesh.BoneIndices[i].z);
               v.i3 = static_cast<uint8_t>(mesh.BoneIndices[i].w);
            } else {
               v.i0 = v.i1 = v.i2 = v.i3 = 0;
            }
            if (i < mesh.BoneWeights.size()) {
               v.w0 = mesh.BoneWeights[i].x;
               v.w1 = mesh.BoneWeights[i].y;
               v.w2 = mesh.BoneWeights[i].z;
               v.w3 = mesh.BoneWeights[i].w;
            } else {
               // Safe fallback: pin to bone 0.
               v.w0 = 1.0f; v.w1 = 0.0f; v.w2 = 0.0f; v.w3 = 0.0f;
            }
         }

         const bgfx::Memory* vbMem = bgfx::copy(
            vertices.data(),
            static_cast<uint32_t>(vertices.size() * sizeof(SkinnedPBRVertex)));
         if (mesh.Dynamic) {
            mesh.dvbh = bgfx::createDynamicVertexBuffer(vbMem, SkinnedPBRVertex::layout);
         } else {
            mesh.vbh = bgfx::createVertexBuffer(vbMem, SkinnedPBRVertex::layout);
         }
      } else {
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
         if (mesh.Dynamic) {
            mesh.dvbh = bgfx::createDynamicVertexBuffer(vbMem, PBRVertex::layout);
         } else {
            mesh.vbh = bgfx::createVertexBuffer(vbMem, PBRVertex::layout);
         }
      }

      const bool use32BitIndices =
         !mesh.Indices.empty() &&
         (*std::max_element(mesh.Indices.begin(), mesh.Indices.end()) > std::numeric_limits<uint16_t>::max());
      if (use32BitIndices) {
         const bgfx::Memory* ibMem = bgfx::copy(
            mesh.Indices.data(),
            static_cast<uint32_t>(mesh.Indices.size() * sizeof(uint32_t)));
         mesh.ibh = bgfx::createIndexBuffer(ibMem, BGFX_BUFFER_INDEX32);
      } else {
         std::vector<uint16_t> indices16(mesh.Indices.size());
         for (size_t i = 0; i < mesh.Indices.size(); ++i) {
            indices16[i] = static_cast<uint16_t>(mesh.Indices[i]);
         }
         const bgfx::Memory* ibMem = bgfx::copy(
            indices16.data(),
            static_cast<uint32_t>(indices16.size() * sizeof(uint16_t)));
         mesh.ibh = bgfx::createIndexBuffer(ibMem);
      }

      mesh.numVertices = static_cast<uint32_t>(vertexCount);
      mesh.numIndices = static_cast<uint32_t>(mesh.Indices.size());
      return HasValidMeshGpuBuffers(mesh);
   }

   // Program safety net: if a material loses/has invalid shader program, fall back to
   // scene default (or skinned default) so rendering continues without restart.
   static Material* ResolveMaterialWithValidProgram(
      Scene& scene,
      Material* source,
      bool needsSkinned,
      std::shared_ptr<Material>& fallbackStatic,
      std::shared_ptr<Material>& fallbackSkinned,
      size_t* createdFallbacks = nullptr)
   {
      if (source && bgfx::isValid(source->GetProgram())) {
         return source;
      }

      std::shared_ptr<Material>& fallback = needsSkinned ? fallbackSkinned : fallbackStatic;
      if (!fallback || !bgfx::isValid(fallback->GetProgram())) {
         fallback = needsSkinned
            ? MaterialManager::Instance().CreateSceneSkinnedDefaultMaterial(&scene)
            : MaterialManager::Instance().CreateSceneDefaultMaterial(&scene);
         if (createdFallbacks && fallback) {
            ++(*createdFallbacks);
         }
      }

      if (!fallback || !bgfx::isValid(fallback->GetProgram())) {
         return nullptr;
      }
      return fallback.get();
   }
}

// ---------------- Initialization ----------------
void Renderer::Init(uint32_t width, uint32_t height, void* windowHandle) {

   // Set viewport size
   m_Width = width;
   m_Height = height;

   // Initialize bgfx with the provided window handle
   bgfx::Init init;
   init.platformData.nwh = windowHandle;
   init.type = cm::rendering::GetDefaultBgfxRendererType();
   init.resolution.width = width;
   init.resolution.height = height;
   init.resolution.reset = kPresentationResetFlags;
   std::cout << "[Renderer] Requested bgfx renderer: "
             << cm::rendering::DescribeBgfxRendererType(init.type)
             << std::endl;
   
   // Increase transient buffer sizes for particle systems and UI rendering
   // Default is 6MB vertex / 2MB index; increase to handle many particles + UI elements
   init.limits.transientVbSize = 16 * 1024 * 1024;  // 16 MB vertex buffer
   init.limits.transientIbSize = 4 * 1024 * 1024;   // 4 MB index buffer
   
   if (!bgfx::init(init)) {
      throw std::runtime_error("[Renderer] bgfx initialization failed");
   }
   cm::rendering::SetBgfxActive(true);
   std::cout << "[Renderer] Active bgfx renderer: "
             << bgfx::getRendererName(bgfx::getRendererType())
             << std::endl;
   bgfx::setDebug(BGFX_DEBUG_TEXT);
   
   // Initialize common property IDs for fast material lookups
   CommonProperties::Initialize();

   // Set default camera
   m_RendererCamera = new Camera(60.0f, float(width) / float(height), 0.1f, 1000.0f);

   // Set up Render Texture
   const uint64_t texFlags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
   m_SceneTexture = bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::BGRA8, texFlags);

   // Prefer 32-bit float depth for better precision in large outdoor scenes.
   m_SceneDepthFormat = ChooseSceneDepthFormat(BGFX_TEXTURE_RT_WRITE_ONLY);
   m_SceneDepthTexture = bgfx::createTexture2D(
      width, height, false, 1,
      m_SceneDepthFormat,
      BGFX_TEXTURE_RT_WRITE_ONLY
   );

   // Create the offscreen framebuffer with color and depth textures
   bgfx::TextureHandle fbTextures[] = { m_SceneTexture, m_SceneDepthTexture };
   m_SceneFrameBuffer = bgfx::createFrameBuffer(2, fbTextures, true);

   // In editor mode, render to the offscreen framebuffer
   // In standalone mode, render directly to the backbuffer
   if (m_RenderToOffscreen) {
      bgfx::setViewFrameBuffer(0, m_SceneFrameBuffer);
      }
   else {
      bgfx::setViewFrameBuffer(0, BGFX_INVALID_HANDLE);
      }

   // Default clear
   bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x303030ff);
   bgfx::setViewClear(1, 0);


   // Initialize vertex layouts globally
   PBRVertex::Init();
   SkinnedPBRVertex::Init();  // Required for skinned mesh loading in runtime
   SkinnedPBRMorphVertex::Init();
   GridVertex::Init();
   TerrainVertex::Init();
   ParticleVertex::Init();
   UIVertex::Init();

   // Debug line program
   m_DebugLineProgram = ShaderManager::Instance().LoadProgram("vs_debug", "fs_debug");
   if (!bgfx::isValid(u_DebugColor)) u_DebugColor = bgfx::createUniform("u_debugColor", bgfx::UniformType::Vec4);
   ApplyDefaultDebugLineColor();
   
   // Cache the debug material to avoid recreating it every frame (editor-only)
#ifndef CLAYMORE_RUNTIME
   m_CachedDebugMaterial = MaterialManager::Instance().CreateDefaultDebugMaterial();
#endif

   // Outline program (fullscreen composite)
   m_OutlineProgram = ShaderManager::Instance().LoadProgram("vs_outline", "fs_outline");
   u_outlineColor = bgfx::createUniform("u_outlineColor", bgfx::UniformType::Vec4);
   // Selection pipeline resources
   m_SelectMaskProgram = ShaderManager::Instance().LoadProgram("vs_pbr", "fs_select_mask");
   m_SelectMaskProgramSkinned = ShaderManager::Instance().LoadProgram("vs_pbr_skinned", "fs_select_mask");
   m_PBRSkinnedInstancedProgram = ShaderManager::Instance().LoadProgram("vs_pbr_skinned_instanced", "fs_pbr_skinned");
   m_PBRSkinnedMorphProgram = ShaderManager::Instance().LoadProgram("vs_pbr_skinned_morph", "fs_pbr_skinned");
   m_PBRSkinnedMorphInstancedProgram = ShaderManager::Instance().LoadProgram("vs_pbr_skinned_morph_instanced", "fs_pbr_skinned");
   m_OutlineCompositeProgram = ShaderManager::Instance().LoadProgram("vs_fullscreen", "fs_outline");
   if (!bgfx::isValid(u_TexelSize)) u_TexelSize = bgfx::createUniform("uTexelSize", bgfx::UniformType::Vec4);
   if (!bgfx::isValid(u_OutlineColor)) u_OutlineColor = bgfx::createUniform("uColor", bgfx::UniformType::Vec4);
   if (!bgfx::isValid(u_OutlineParams)) u_OutlineParams = bgfx::createUniform("uParams", bgfx::UniformType::Vec4);
   if (!bgfx::isValid(s_MaskVis)) s_MaskVis = bgfx::createUniform("sMaskVis", bgfx::UniformType::Sampler);
   if (!bgfx::isValid(s_MaskOcc)) s_MaskOcc = bgfx::createUniform("sMaskOcc", bgfx::UniformType::Sampler);
   m_TintProgram = ShaderManager::Instance().LoadProgram("vs_pbr", "fs_tint");
   if (!bgfx::isValid(u_TintColor)) u_TintColor = bgfx::createUniform("u_ColorTint", bgfx::UniformType::Vec4);

   // New screen-space outline programs and uniforms
   m_ObjectIdProgram        = ShaderManager::Instance().LoadProgram("vs_pbr", "fs_object_id");
   m_ObjectIdProgramSkinned = ShaderManager::Instance().LoadProgram("vs_pbr_skinned", "fs_object_id");
   m_ObjectIdProgramSkinnedInstanced = ShaderManager::Instance().LoadProgram("vs_pbr_skinned_object_id_instanced", "fs_object_id_instanced");
   m_ObjectIdProgramSkinnedMorph = ShaderManager::Instance().LoadProgram("vs_pbr_skinned_morph", "fs_object_id");
   m_ObjectIdProgramSkinnedMorphInstanced = ShaderManager::Instance().LoadProgram("vs_pbr_skinned_morph_object_id_instanced", "fs_object_id_instanced");
   m_OutlineEdgeProgram     = ShaderManager::Instance().LoadProgram("vs_fullscreen", "fs_outline_edge");
   m_OutlineCompositeProgram2 = ShaderManager::Instance().LoadProgram("vs_fullscreen", "fs_outline_composite");
   if (!bgfx::isValid(u_ObjectIdPacked)) u_ObjectIdPacked = bgfx::createUniform("uObjectId", bgfx::UniformType::Vec4);
   if (!bgfx::isValid(u_SelectedIdPacked)) u_SelectedIdPacked = bgfx::createUniform("uSelectedId", bgfx::UniformType::Vec4);
   if (!bgfx::isValid(s_ObjectId)) s_ObjectId = bgfx::createUniform("sObjectId", bgfx::UniformType::Sampler);
   if (!bgfx::isValid(s_EdgeMask)) s_EdgeMask = bgfx::createUniform("sEdgeMask", bgfx::UniformType::Sampler);
   if (!bgfx::isValid(s_SceneColor)) s_SceneColor = bgfx::createUniform("sSceneColor", bgfx::UniformType::Sampler);

   // Create selection mask targets
   const uint64_t rFlags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
   m_VisMaskTex = bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::BGRA8, rFlags);
   m_OccMaskTex = bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::BGRA8, rFlags);
   {
      bgfx::TextureHandle visAttachments[] = { m_VisMaskTex, m_SceneDepthTexture };
      // Do not destroy attached textures; depth is shared
      m_VisMaskFB = bgfx::createFrameBuffer(2, visAttachments, false);
   }
   m_OccMaskFB = bgfx::createFrameBuffer(1, &m_OccMaskTex, true);

   // Create ObjectID and Edge mask render targets
   {
      const uint64_t idFlags = BGFX_TEXTURE_RT | BGFX_SAMPLER_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
      m_ObjectIdTex = bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::RGBA8, idFlags);
      bgfx::TextureHandle idAttachments[] = { m_ObjectIdTex, m_SceneDepthTexture };
      // Don't destroy depth; it's shared with the scene framebuffer
      m_ObjectIdFB = bgfx::createFrameBuffer(2, idAttachments, false);
   }
   if (bgfx::getCaps() &&
       (bgfx::getCaps()->supported & BGFX_CAPS_TEXTURE_BLIT) != 0 &&
       (bgfx::getCaps()->supported & BGFX_CAPS_TEXTURE_READ_BACK) != 0) {
      const uint64_t readbackFlags = BGFX_TEXTURE_READ_BACK | BGFX_TEXTURE_BLIT_DST | BGFX_SAMPLER_POINT;
      m_ObjectIdReadbackTex = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8, readbackFlags);
   }
   {
      const uint64_t edgeFlags = BGFX_TEXTURE_RT | BGFX_SAMPLER_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
      m_EdgeMaskTex = bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::R8, edgeFlags);
      m_EdgeMaskFB = bgfx::createFrameBuffer(1, &m_EdgeMaskTex, true);
   }
#ifndef CLAYMORE_RUNTIME
   InitGrid(20.0f, 1.0f);
#endif

   // Create uniforms for lighting and environment
   u_LightColors = bgfx::createUniform("u_lightColors", bgfx::UniformType::Vec4, kMaxShaderLights);
   u_LightPositions = bgfx::createUniform("u_lightPositions", bgfx::UniformType::Vec4, kMaxShaderLights);
   u_LightParams = bgfx::createUniform("u_lightParams", bgfx::UniformType::Vec4, kMaxShaderLights);
   u_cameraPos = bgfx::createUniform("u_cameraPos", bgfx::UniformType::Vec4);

   // CPU-provided normal matrix for skinned and static meshes
   u_normalMat = bgfx::createUniform("u_normalMat", bgfx::UniformType::Mat4);
   u_AmbientFog = bgfx::createUniform("u_ambientFog", bgfx::UniformType::Vec4);
   u_FogParams = bgfx::createUniform("u_fogParams", bgfx::UniformType::Vec4);
   u_SkyParams = bgfx::createUniform("u_skyParams", bgfx::UniformType::Vec4);
   u_SkyTopColor = bgfx::createUniform("u_skyTopColor", bgfx::UniformType::Vec4);
   u_SkyHorizonColor = bgfx::createUniform("u_skyHorizonColor", bgfx::UniformType::Vec4);
   u_GroundColor = bgfx::createUniform("u_groundColor", bgfx::UniformType::Vec4);
   u_SunDirection = bgfx::createUniform("u_sunDirection", bgfx::UniformType::Vec4);
   u_SkySunParams = bgfx::createUniform("u_skySunParams", bgfx::UniformType::Vec4);
   u_SkyAtmosphereParams = bgfx::createUniform("u_skyAtmosphereParams", bgfx::UniformType::Vec4);
   u_SceneColorGrade = bgfx::createUniform("u_sceneColorGrade", bgfx::UniformType::Vec4);
   s_Skybox = bgfx::createUniform("s_skybox", bgfx::UniformType::Sampler);
   // Note: u_invViewProj is a predefined bgfx uniform, no need to create it


   // Terrain resources
   m_TerrainProgram = ShaderManager::Instance().LoadProgram("vs_terrain_height_texture", "fs_terrain");
   m_TerrainDepthProgram = ShaderManager::Instance().LoadProgram("vs_terrain_depth", "fs_terrain_depth");
   s_TerrainHeightTexture = bgfx::createUniform("s_heightTexture", bgfx::UniformType::Sampler);
   s_TerrainSplatTexture = bgfx::createUniform("s_splatTexture", bgfx::UniformType::Sampler);
   s_TerrainSplatTexture2 = bgfx::createUniform("s_splatTexture2", bgfx::UniformType::Sampler);
   s_TerrainHoleTexture = bgfx::createUniform("s_holeTexture", bgfx::UniformType::Sampler);
   // Texture array samplers for all layer albedo/normal maps
   s_TerrainAlbedoArray = bgfx::createUniform("s_layerAlbedoArray", bgfx::UniformType::Sampler);
   s_TerrainNormalArray = bgfx::createUniform("s_layerNormalArray", bgfx::UniformType::Sampler);
   u_TerrainChunkParams = bgfx::createUniform("u_chunkParams", bgfx::UniformType::Vec4);
   u_TerrainHeightParams = bgfx::createUniform("u_heightParams", bgfx::UniformType::Vec4);
   u_TerrainTexelSize = bgfx::createUniform("u_texelSize", bgfx::UniformType::Vec4);
   u_TerrainLayerTiling = bgfx::createUniform("u_layerTiling", bgfx::UniformType::Vec4, kMaxTerrainLayers);
   u_TerrainLayerColor = bgfx::createUniform("u_layerColor", bgfx::UniformType::Vec4, kMaxTerrainLayers);
   u_TerrainMaterial = bgfx::createUniform("u_terrainMaterial", bgfx::UniformType::Vec4);
   u_TerrainLayerCount = bgfx::createUniform("u_layerCount", bgfx::UniformType::Vec4);
   if (!bgfx::isValid(m_TerrainFallbackAlbedo))
   {
      TextureSpecifier spec;
      spec.Path = "assets/debug/white.png";
      m_TerrainFallbackAlbedo = AcquireTextureHandle(spec, TextureColorSpace::Linear);
   }
   if (!bgfx::isValid(m_TerrainFallbackNormal))
   {
      const uint8_t flatNormal[4] = { 128, 128, 255, 255 };
      m_TerrainFallbackNormal = TextureLoader::Load2DFromRGBA(flatNormal, 1, 1, false);
   }
   if (!bgfx::isValid(m_TerrainFallbackHole))
   {
      const uint8_t solidMask = 0;
      const bgfx::Memory* solidMaskMem = bgfx::copy(&solidMask, sizeof(solidMask));
      m_TerrainFallbackHole = bgfx::createTexture2D(
         1, 1, false, 1, bgfx::TextureFormat::R8,
         BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
         solidMaskMem);
   }
   
   // Clipmap terrain rendering resources
   m_ClipmapProgram = ShaderManager::Instance().LoadProgram("vs_terrain_clipmap", "fs_terrain");
   m_ClipmapDepthProgram = ShaderManager::Instance().LoadProgram("vs_terrain_clipmap_depth", "fs_terrain_depth");
   u_ClipmapParams = bgfx::createUniform("u_clipmapParams", bgfx::UniformType::Vec4);
   u_ClipmapOffset = bgfx::createUniform("u_clipmapOffset", bgfx::UniformType::Vec4);
   u_TerrainOrigin = bgfx::createUniform("u_terrainOrigin", bgfx::UniformType::Vec4);
   
   // Chunked terrain rendering resources (Skyrim-style cells)
   m_ChunkedTerrainProgram = ShaderManager::Instance().LoadProgram("vs_terrain_chunk", "fs_terrain");
   m_ChunkedTerrainDepthProgram = ShaderManager::Instance().LoadProgram("vs_terrain_chunk_depth", "fs_terrain_depth");
   u_ChunkParams = bgfx::createUniform("u_chunkParams", bgfx::UniformType::Vec4);
   u_ChunkWorld = bgfx::createUniform("u_chunkWorld", bgfx::UniformType::Vec4);
   u_MorphParams = bgfx::createUniform("u_morphParams", bgfx::UniformType::Vec4);
   u_NeighborLODs = bgfx::createUniform("u_neighborLODs", bgfx::UniformType::Vec4);
   u_TerrainSize = bgfx::createUniform("u_terrainSize", bgfx::UniformType::Vec4);
   m_ChunkSystem = std::make_unique<terrain::TerrainChunkSystem>();
   
   // Instanced chunked terrain (batches all visible chunks per LOD into single draw calls)
   m_ChunkedTerrainInstancedProgram = ShaderManager::Instance().LoadProgram("vs_terrain_chunk_instanced", "fs_terrain");
   m_ChunkedTerrainDepthInstancedProgram = ShaderManager::Instance().LoadProgram("vs_terrain_chunk_depth_instanced", "fs_terrain_depth");

   TerrainGrass::InitializeRendererResources();

   // Procedural sky program (fullscreen triangle)
   m_SkyProgram = ShaderManager::Instance().LoadProgram("vs_sky", "fs_sky");
   // Create reusable fullscreen triangle buffers
   EnsureFullscreenTriangle();

   // Shadow uniforms
   if (!bgfx::isValid(u_LightViewProj)) u_LightViewProj = bgfx::createUniform("u_lightViewProj", bgfx::UniformType::Mat4);
   if (!bgfx::isValid(u_LightViewProjCSM)) u_LightViewProjCSM = bgfx::createUniform("u_lightViewProjCSM", bgfx::UniformType::Mat4, 4);
   if (!bgfx::isValid(u_ShadowParams)) u_ShadowParams = bgfx::createUniform("u_shadowParams", bgfx::UniformType::Vec4);
   if (!bgfx::isValid(u_ShadowTexelSize)) u_ShadowTexelSize = bgfx::createUniform("u_shadowTexelSize", bgfx::UniformType::Vec4);
   if (!bgfx::isValid(u_ShadowReceive)) u_ShadowReceive = bgfx::createUniform("u_shadowReceive", bgfx::UniformType::Vec4);
   if (!bgfx::isValid(s_ShadowMap)) s_ShadowMap = bgfx::createUniform("s_shadowMap", bgfx::UniformType::Sampler);
   if (!bgfx::isValid(u_ShadowLightDir)) u_ShadowLightDir = bgfx::createUniform("u_shadowLightDir", bgfx::UniformType::Vec4);
   if (!bgfx::isValid(u_CascadeSplits)) u_CascadeSplits = bgfx::createUniform("u_CascadeSplits", bgfx::UniformType::Vec4);
   if (!bgfx::isValid(u_CascadeScaleBias)) u_CascadeScaleBias = bgfx::createUniform("u_CascadeScaleBias", bgfx::UniformType::Vec4, 4);
   if (!bgfx::isValid(s_PointShadowMap)) s_PointShadowMap = bgfx::createUniform("s_pointShadowMap", bgfx::UniformType::Sampler);
   if (!bgfx::isValid(u_PointShadowMeta)) u_PointShadowMeta = bgfx::createUniform("u_pointShadowMeta", bgfx::UniformType::Vec4, kMaxPointShadowLights);
   if (!bgfx::isValid(u_PointShadowLightPos)) u_PointShadowLightPos = bgfx::createUniform("u_pointShadowLightPos", bgfx::UniformType::Vec4, kMaxPointShadowLights);
   if (!bgfx::isValid(u_PointShadowAtlas)) u_PointShadowAtlas = bgfx::createUniform("u_pointShadowAtlas", bgfx::UniformType::Vec4);
   if (!bgfx::isValid(s_ShadowDebug)) s_ShadowDebug = bgfx::createUniform("s_shadowDebug", bgfx::UniformType::Sampler);
   if (!bgfx::isValid(u_ShadowDebugParams)) u_ShadowDebugParams = bgfx::createUniform("u_shadowDebugParams", bgfx::UniformType::Vec4);
   if (!bgfx::isValid(m_ShadowDebugProgram)) m_ShadowDebugProgram = ShaderManager::Instance().LoadProgram("vs_fullscreen", "fs_shadow_debug");

   const bgfx::Caps* caps = bgfx::getCaps();
   const uint64_t gpuSkinningFlags =
      BGFX_SAMPLER_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
   if (!bgfx::isValid(s_BoneAtlas)) s_BoneAtlas = bgfx::createUniform("s_boneAtlas", bgfx::UniformType::Sampler);
   if (!bgfx::isValid(s_BoneRemapAtlas)) s_BoneRemapAtlas = bgfx::createUniform("s_boneRemapAtlas", bgfx::UniformType::Sampler);
   if (!bgfx::isValid(s_SkinningInstanceAtlas)) s_SkinningInstanceAtlas = bgfx::createUniform("s_skinningInstanceAtlas", bgfx::UniformType::Sampler);
   if (!bgfx::isValid(s_MorphVertexAtlas)) s_MorphVertexAtlas = bgfx::createUniform("s_morphVertexAtlas", bgfx::UniformType::Sampler);
   if (!bgfx::isValid(s_MorphEntryAtlas)) s_MorphEntryAtlas = bgfx::createUniform("s_morphEntryAtlas", bgfx::UniformType::Sampler);
   if (!bgfx::isValid(s_MorphActiveAtlas)) s_MorphActiveAtlas = bgfx::createUniform("s_morphActiveAtlas", bgfx::UniformType::Sampler);
   if (!bgfx::isValid(u_SkinningBoneAtlasInfo)) u_SkinningBoneAtlasInfo = bgfx::createUniform("u_skinningBoneAtlasInfo", bgfx::UniformType::Vec4);
   if (!bgfx::isValid(u_SkinningRemapAtlasInfo)) u_SkinningRemapAtlasInfo = bgfx::createUniform("u_skinningRemapAtlasInfo", bgfx::UniformType::Vec4);
   if (!bgfx::isValid(u_SkinningInstanceAtlasInfo)) u_SkinningInstanceAtlasInfo = bgfx::createUniform("u_skinningInstanceAtlasInfo", bgfx::UniformType::Vec4);
   if (!bgfx::isValid(u_MorphVertexAtlasInfo)) u_MorphVertexAtlasInfo = bgfx::createUniform("u_morphVertexAtlasInfo", bgfx::UniformType::Vec4);
   if (!bgfx::isValid(u_MorphEntryAtlasInfo)) u_MorphEntryAtlasInfo = bgfx::createUniform("u_morphEntryAtlasInfo", bgfx::UniformType::Vec4);
   if (!bgfx::isValid(u_MorphActiveAtlasInfo)) u_MorphActiveAtlasInfo = bgfx::createUniform("u_morphActiveAtlasInfo", bgfx::UniformType::Vec4);
   if (!bgfx::isValid(u_SkinningParams)) u_SkinningParams = bgfx::createUniform("u_skinningParams", bgfx::UniformType::Vec4);
   if (!bgfx::isValid(u_SkinningExtra)) u_SkinningExtra = bgfx::createUniform("u_skinningExtra", bgfx::UniformType::Vec4);
   if (!bgfx::isValid(u_SkinningInstanceRecord)) u_SkinningInstanceRecord = bgfx::createUniform("u_skinningInstanceRecord", bgfx::UniformType::Vec4);
   if (!bgfx::isValid(u_SkinningMeshFromSkeleton)) u_SkinningMeshFromSkeleton = bgfx::createUniform("u_skinningMeshFromSkeleton", bgfx::UniformType::Mat4);
   if (!bgfx::isValid(u_SkinningMorphParams)) u_SkinningMorphParams = bgfx::createUniform("u_skinningMorphParams", bgfx::UniformType::Vec4);
   m_GpuSkinningAtlasSupported =
      caps != nullptr &&
      caps->limits.maxTextureSamplers > kGpuSkinningInstanceAtlasStage &&
      bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::RGBA32F, gpuSkinningFlags) &&
      bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::R16, gpuSkinningFlags);
   m_GpuMorphAtlasSupported =
      m_GpuSkinningAtlasSupported &&
      caps != nullptr &&
      caps->limits.maxTextureSamplers > kGpuMorphActiveAtlasStage &&
      bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::RGBA32F, gpuSkinningFlags);
   if (!bgfx::isValid(u_GpuMaterializedSkinningDispatch)) {
      u_GpuMaterializedSkinningDispatch =
         bgfx::createUniform("u_materializedSkinningDispatch", bgfx::UniformType::Vec4);
   }
   m_GpuMaterializedSkinningSupported = false;
   if (caps != nullptr &&
       (caps->supported & BGFX_CAPS_COMPUTE) != 0 &&
       m_GpuSkinningAtlasSupported &&
       bgfx::isValid(u_GpuMaterializedSkinningDispatch)) {
      bgfx::ShaderHandle csMaterializedSkinning =
         ShaderManager::Instance().LoadShader("cs_materialized_skinning", ShaderType::Compute);
      if (bgfx::isValid(csMaterializedSkinning)) {
         m_GpuMaterializedSkinningProgram = bgfx::createProgram(csMaterializedSkinning, true);
         m_GpuMaterializedSkinningSupported = bgfx::isValid(m_GpuMaterializedSkinningProgram);
      }
   }
   if (!m_GpuSkinningAtlasSupported) {
      std::cerr << "[Renderer] GPU skinning atlas disabled: backend lacks RGBA32F/R16 mutable textures or enough sampler stages." << std::endl;
   }
   if (!m_GpuMorphAtlasSupported) {
      std::cerr << "[Renderer] GPU morph atlas disabled: backend lacks extra RGBA32F samplers for sparse morph textures." << std::endl;
   }
   if (!m_GpuMaterializedSkinningSupported) {
      std::cerr << "[Renderer] GPU materialized skinning disabled: backend lacks compute support or the compute shader failed to load." << std::endl;
   }

   // Initialize text renderer (self-contained, stb-based)
   m_TextRenderer = std::make_unique<TextRenderer>();
   // Use dedicated text shaders that sample the atlas alpha
   bgfx::ProgramHandle fontProgram = ShaderManager::Instance().LoadProgram("vs_text", "fs_text");
   if (!m_TextRenderer->Init(kFallbackTextFontPath, fontProgram, 1024, 1024, 48.0f)) {
      std::cerr << "[Renderer] Failed to initialize TextRenderer (font bake). Continuing without text." << std::endl;
      }

   // UI rendering init
   m_UIProgram = ShaderManager::Instance().LoadProgram("vs_ui", "fs_ui");
   if (!bgfx::isValid(m_UISampler)) m_UISampler = bgfx::createUniform("s_uiTex", bgfx::UniformType::Sampler);
   if (!bgfx::isValid(m_UIProgram)) {
      std::cerr << "[Renderer] UI shader program invalid; UI overlay disabled." << std::endl;
      m_ShowUIOverlay = false;
      }
   // Fallback white texture for panels without texture
   try {
      TextureSpecifier spec;
      spec.Path = "assets/debug/white.png";
      m_UIWhiteTex = AcquireTextureHandle(spec, TextureColorSpace::Linear);
      }
   catch (const std::exception& e) {
      std::cerr << "[Renderer] Failed to load UI white texture: " << e.what() << std::endl;
      m_UIWhiteTex.idx = bgfx::kInvalidHandle;
      }

   }

void Renderer::SetDefaultTextFont(const std::string& fontPath, float basePixelSize) {
   if (!m_TextRenderer) return;
   std::string resolved = fontPath.empty() ? std::string(kFallbackTextFontPath) : fontPath;
   m_TextRenderer->SetDefaultFontPath(resolved);
   m_TextRenderer->SetFont(resolved, basePixelSize);
}

void Renderer::DestroyGpuSkinningResources()
{
   if (bgfx::isValid(m_SkinningBoneAtlasTex)) {
      bgfx::destroy(m_SkinningBoneAtlasTex);
      m_SkinningBoneAtlasTex = BGFX_INVALID_HANDLE;
   }
   if (bgfx::isValid(m_SkinningRemapAtlasTex)) {
      bgfx::destroy(m_SkinningRemapAtlasTex);
      m_SkinningRemapAtlasTex = BGFX_INVALID_HANDLE;
   }
   if (bgfx::isValid(m_SkinningInstanceAtlasTex)) {
      bgfx::destroy(m_SkinningInstanceAtlasTex);
      m_SkinningInstanceAtlasTex = BGFX_INVALID_HANDLE;
   }
   if (bgfx::isValid(m_MorphVertexAtlasTex)) {
      bgfx::destroy(m_MorphVertexAtlasTex);
      m_MorphVertexAtlasTex = BGFX_INVALID_HANDLE;
   }
   if (bgfx::isValid(m_MorphEntryAtlasTex)) {
      bgfx::destroy(m_MorphEntryAtlasTex);
      m_MorphEntryAtlasTex = BGFX_INVALID_HANDLE;
   }
   if (bgfx::isValid(m_MorphActiveAtlasTex)) {
      bgfx::destroy(m_MorphActiveAtlasTex);
      m_MorphActiveAtlasTex = BGFX_INVALID_HANDLE;
   }
   for (auto& [mesh, vbh] : m_GpuMorphVertexBuffers) {
      (void)mesh;
      if (bgfx::isValid(vbh)) {
         bgfx::destroy(vbh);
      }
   }
   for (auto& [mesh, source] : m_GpuMaterializedSkinningSources) {
      (void)mesh;
      if (bgfx::isValid(source.Positions)) bgfx::destroy(source.Positions);
      if (bgfx::isValid(source.Normals)) bgfx::destroy(source.Normals);
      if (bgfx::isValid(source.UVs)) bgfx::destroy(source.UVs);
      if (bgfx::isValid(source.BoneIndices)) bgfx::destroy(source.BoneIndices);
      if (bgfx::isValid(source.BoneWeights)) bgfx::destroy(source.BoneWeights);
   }
   for (auto& [key, cache] : m_GpuMaterializedSkinnedMeshes) {
      (void)key;
      if (bgfx::isValid(cache.Output)) {
         bgfx::destroy(cache.Output);
      }
   }
   m_GpuMorphVertexBuffers.clear();
   m_CachedGpuMorphGeometries.clear();
   m_GpuMaterializedSkinningSources.clear();
   m_GpuMaterializedSkinnedMeshes.clear();
   m_GpuMaterializedSkinningCoveredInstances.clear();
   m_SkinningBoneAtlasCpu.clear();
   m_SkinningRemapAtlasCpu.clear();
   m_SkinningInstanceRecords.clear();
   m_SkinningInstanceAtlasCpu.clear();
   m_MorphVertexAtlasCpu.clear();
   m_MorphEntryAtlasCpu.clear();
   m_MorphActiveAtlasCpu.clear();
   m_SkinningBoneAtlasWidth = 0;
   m_SkinningBoneAtlasHeight = 0;
   m_SkinningRemapAtlasWidth = 0;
   m_SkinningRemapAtlasHeight = 0;
   m_SkinningInstanceAtlasWidth = 0;
   m_SkinningInstanceAtlasHeight = 0;
   m_MorphVertexAtlasWidth = 0;
   m_MorphVertexAtlasHeight = 0;
   m_MorphEntryAtlasWidth = 0;
   m_MorphEntryAtlasHeight = 0;
   m_MorphActiveAtlasWidth = 0;
   m_MorphActiveAtlasHeight = 0;
   m_SkinningBoneAtlasLastFingerprint = 0;
   m_SkinningRemapAtlasLastFingerprint = 0;
   m_SkinningInstanceAtlasLastFingerprint = 0;
   m_MorphVertexAtlasLastFingerprint = 0;
   m_MorphEntryAtlasLastFingerprint = 0;
   m_MorphActiveAtlasLastFingerprint = 0;
   m_SkinningAtlasLastBoneTexelCount = 0;
   m_SkinningAtlasLastRemapTexelCount = 0;
   m_SkinningAtlasLastInstanceTexelCount = 0;
   m_MorphAtlasLastVertexTexelCount = 0;
   m_MorphAtlasLastEntryTexelCount = 0;
   m_MorphAtlasLastActiveTexelCount = 0;
   m_SkinningAtlasUploadSerial = 0;
   m_SkinningAtlasPreparedFrameSerial = 0;
   m_SkinningAtlasPreparedScene = nullptr;
   m_SkinningAtlasUnionEntities.clear();
   m_SkinningAtlasCoveredEntities.clear();
   m_GpuMaterializedSkinningPreparedFrameSerial = 0;
   m_GpuSkinningAtlasReady = false;
}

bool Renderer::CanUseGpuMorphTargets(const Mesh* mesh, const BlendShapeComponent* blendShapes) const
{
   if (!kAllowGpuMorphTargets ||
       !m_GpuMorphAtlasSupported ||
       !mesh ||
       !blendShapes ||
       blendShapes->Shapes.empty()) {
      return false;
   }

   if (!mesh->HasSkinning()) {
      return false;
   }

   const size_t vertexCount = mesh->Vertices.size();
   if (vertexCount == 0 ||
       mesh->Normals.size() < vertexCount ||
       mesh->BoneIndices.size() < vertexCount ||
       mesh->BoneWeights.size() < vertexCount) {
      return false;
   }

   if (blendShapes->CountActiveShapes(kGpuMorphWeightThreshold) > kGpuMorphMaxActiveShapes) {
      return false;
   }

   if (blendShapes->GetMaxAffectedShapesPerVertex(vertexCount) > kGpuMorphMaxEntriesPerVertex) {
      return false;
   }

   return true;
}

bool Renderer::CanRenderGpuMorphTargets(
   const SkinningComponent* skinning,
   const Mesh* mesh,
   const BlendShapeComponent* blendShapes)
{
   if (!skinning || !skinning->GpuMorphRuntimeAllowed) {
      return false;
   }

   if (!CanUseGpuMorphTargets(mesh, blendShapes)) {
      return false;
   }

   if (!bgfx::isValid(m_PBRSkinnedMorphProgram) ||
       !bgfx::isValid(m_PBRSkinnedMorphInstancedProgram) ||
       !bgfx::isValid(m_ObjectIdProgramSkinnedMorph) ||
       !bgfx::isValid(m_ObjectIdProgramSkinnedMorphInstanced)) {
      return false;
   }

   static bgfx::ProgramHandle s_depthProgSkinnedMorph = BGFX_INVALID_HANDLE;
   static bgfx::ProgramHandle s_depthProgSkinnedMorphInstanced = BGFX_INVALID_HANDLE;
   static bgfx::ProgramHandle s_pointDepthProgSkinnedMorph = BGFX_INVALID_HANDLE;
   static bgfx::ProgramHandle s_pointDepthProgSkinnedMorphInstanced = BGFX_INVALID_HANDLE;
   if (!bgfx::isValid(s_depthProgSkinnedMorph)) {
      s_depthProgSkinnedMorph =
         ShaderManager::Instance().LoadProgram("vs_depth_skinned_morph", "fs_depth");
   }
   if (!bgfx::isValid(s_depthProgSkinnedMorphInstanced)) {
      s_depthProgSkinnedMorphInstanced =
         ShaderManager::Instance().LoadProgram("vs_depth_skinned_morph_instanced", "fs_depth");
   }
   if (!bgfx::isValid(s_pointDepthProgSkinnedMorph)) {
      s_pointDepthProgSkinnedMorph =
         ShaderManager::Instance().LoadProgram("vs_depth_skinned_morph", "fs_point_shadow_depth");
   }
   if (!bgfx::isValid(s_pointDepthProgSkinnedMorphInstanced)) {
      s_pointDepthProgSkinnedMorphInstanced =
         ShaderManager::Instance().LoadProgram("vs_depth_skinned_morph_instanced", "fs_point_shadow_depth");
   }

   if (!bgfx::isValid(s_depthProgSkinnedMorph) ||
       !bgfx::isValid(s_depthProgSkinnedMorphInstanced) ||
       !bgfx::isValid(s_pointDepthProgSkinnedMorph) ||
       !bgfx::isValid(s_pointDepthProgSkinnedMorphInstanced)) {
      return false;
   }

   const bgfx::VertexBufferHandle morphVbh = GetOrCreateGpuMorphVertexBuffer(mesh);
   return bgfx::isValid(morphVbh);
}

bool Renderer::ShouldUseGpuMorphTargets(
   const SkinningComponent* skinning,
   const Mesh* mesh,
   const BlendShapeComponent* blendShapes) const
{
   if (!skinning || !skinning->GpuMorphRuntimeAllowed) {
      return false;
   }

   return CanUseGpuMorphTargets(mesh, blendShapes);
}

bool Renderer::CanUseGpuMaterializedSkinning(
   const SkinningComponent* skinning,
   const Mesh* mesh,
   const BlendShapeComponent* blendShapes) const
{
   if (!m_GpuMaterializedSkinningSupported ||
       !skinning ||
       !mesh ||
       !mesh->HasSkinning() ||
       !skinning->UsesGpuSharedSkeleton() ||
       skinning->GpuSourceSkeleton == nullptr) {
      return false;
   }

   const size_t vertexCount = mesh->Vertices.size();
   if (vertexCount == 0 ||
       mesh->Normals.size() < vertexCount ||
       mesh->BoneIndices.size() < vertexCount ||
       mesh->BoneWeights.size() < vertexCount) {
      return false;
   }

   if (blendShapes && !blendShapes->Shapes.empty()) {
      const uint32_t activeShapeCount =
         blendShapes->CountActiveShapes(kGpuMorphWeightThreshold);
      if (activeShapeCount > 0u) {
         const bool hasCpuBlendedMorphSource =
            mesh->Dynamic &&
            blendShapes->BlendedStride == sizeof(SkinnedPBRVertex) &&
            blendShapes->BlendedVertices.size() >=
               (vertexCount * sizeof(SkinnedPBRVertex));
         if (!hasCpuBlendedMorphSource) {
            return false;
         }
      }
   }

   return true;
}

bool Renderer::TryGetGpuMaterializedSkinnedVertexBuffer(Scene& scene,
                                                        EntityID entityId,
                                                        const Mesh* mesh,
                                                        bgfx::VertexBufferHandle& outVertexBuffer) const
{
   outVertexBuffer = BGFX_INVALID_HANDLE;
   if (!mesh) {
      return false;
   }

   EntityData* data = scene.GetEntityData(entityId);
   if (!data ||
       !data->Skinning ||
       !CanUseGpuMaterializedSkinning(data->Skinning.get(), mesh, data->BlendShapes.get())) {
      return false;
   }

   const uint64_t key = MakeGpuMaterializedSkinningInstanceKey(scene, entityId);
   auto it = m_GpuMaterializedSkinnedMeshes.find(key);
   if (it == m_GpuMaterializedSkinnedMeshes.end()) {
      return false;
   }

   const GpuMaterializedSkinnedMeshCache& cache = it->second;
   if (cache.SourceMesh != mesh || !bgfx::isValid(cache.Output)) {
      return false;
   }

   outVertexBuffer = cache.Output;
   return true;
}

bool Renderer::ResolveGpuMaterializedSkinnedColorProgram(bgfx::ProgramHandle skinnedProgram,
                                                         bgfx::ProgramHandle& outProgram) const
{
   outProgram = BGFX_INVALID_HANDLE;
   if (!bgfx::isValid(skinnedProgram)) {
      return false;
   }

   static bgfx::ProgramHandle s_skinnedPbrProgram = BGFX_INVALID_HANDLE;
   static bgfx::ProgramHandle s_preSkinnedPbrProgram = BGFX_INVALID_HANDLE;
   static bgfx::ProgramHandle s_skinnedPsxProgram = BGFX_INVALID_HANDLE;
   static bgfx::ProgramHandle s_preSkinnedPsxProgram = BGFX_INVALID_HANDLE;
   if (!bgfx::isValid(s_skinnedPbrProgram)) {
      s_skinnedPbrProgram = ShaderManager::Instance().LoadProgram("vs_pbr_skinned", "fs_pbr_skinned");
   }
   if (!bgfx::isValid(s_preSkinnedPbrProgram)) {
      s_preSkinnedPbrProgram = ShaderManager::Instance().LoadProgram("vs_pbr", "fs_pbr");
   }
   if (!bgfx::isValid(s_skinnedPsxProgram)) {
      s_skinnedPsxProgram = ShaderManager::Instance().LoadProgram("vs_psx_skinned", "fs_psx");
   }
   if (!bgfx::isValid(s_preSkinnedPsxProgram)) {
      s_preSkinnedPsxProgram = ShaderManager::Instance().LoadProgram("vs_psx", "fs_psx");
   }

   if (bgfx::isValid(s_skinnedPbrProgram) &&
       bgfx::isValid(s_preSkinnedPbrProgram) &&
       skinnedProgram.idx == s_skinnedPbrProgram.idx) {
      outProgram = s_preSkinnedPbrProgram;
      return true;
   }

   if (bgfx::isValid(s_skinnedPsxProgram) &&
       bgfx::isValid(s_preSkinnedPsxProgram) &&
       skinnedProgram.idx == s_skinnedPsxProgram.idx) {
      outProgram = s_preSkinnedPsxProgram;
      return true;
   }

   return false;
}

void Renderer::PrepareGpuMaterializedSkinnedMeshes(
   Scene& scene,
   const std::vector<EntityID>& primaryEntities,
   const std::vector<EntityID>* secondaryEntities)
{
   if (!m_GpuMaterializedSkinningSupported ||
       !m_GpuSkinningAtlasReady ||
       !bgfx::isValid(m_GpuMaterializedSkinningProgram) ||
       !bgfx::isValid(u_GpuMaterializedSkinningDispatch)) {
      return;
   }

   if (m_GpuMaterializedSkinningPreparedFrameSerial != m_SkinningAtlasFrameSerial) {
      m_GpuMaterializedSkinningPreparedFrameSerial = m_SkinningAtlasFrameSerial;
      m_GpuMaterializedSkinningCoveredInstances.clear();
   }

   std::vector<std::pair<uint64_t, EntityID>> pendingEntities;
   pendingEntities.reserve(primaryEntities.size() + (secondaryEntities ? secondaryEntities->size() : 0u));
   const auto appendPending = [&](const std::vector<EntityID>& entities) {
      for (EntityID entityId : entities) {
         if (entityId == INVALID_ENTITY_ID) {
            continue;
         }

         const uint64_t key = MakeGpuMaterializedSkinningInstanceKey(scene, entityId);
         if (!m_GpuMaterializedSkinningCoveredInstances.emplace(key).second) {
            continue;
         }
         pendingEntities.emplace_back(key, entityId);
      }
   };
   appendPending(primaryEntities);
   if (secondaryEntities) {
      appendPending(*secondaryEntities);
   }

   if (pendingEntities.empty()) {
      return;
   }

   const bgfx::VertexLayout& computeLayout = GetComputeVec4Layout();
   for (const auto& pending : pendingEntities) {
      const uint64_t instanceKey = pending.first;
      const EntityID entityId = pending.second;
      EntityData* data = scene.GetEntityData(entityId);
      if (!data || !data->Mesh || !data->Mesh->mesh || !data->Skinning) {
         continue;
      }

      Mesh* mesh = data->Mesh->mesh.get();
      SkinningComponent* skinning = data->Skinning.get();
      BlendShapeComponent* blendShapes = data->BlendShapes.get();
      if (!CanUseGpuMaterializedSkinning(skinning, mesh, blendShapes)) {
         continue;
      }
      Profiler::Get().AddCounter("Render/MaterializedSkinnedMeshes", 1);

      auto sourceIt = m_GpuMaterializedSkinningSources.find(mesh);
      if (sourceIt == m_GpuMaterializedSkinningSources.end()) {
         sourceIt = m_GpuMaterializedSkinningSources.emplace(mesh, GpuMaterializedSkinningSourceBuffers{}).first;
      }

      GpuMaterializedSkinningSourceBuffers& source = sourceIt->second;
      const uint32_t vertexCount = static_cast<uint32_t>(mesh->Vertices.size());
      const uint32_t activeShapeCount =
         blendShapes ? blendShapes->CountActiveShapes(kGpuMorphWeightThreshold) : 0u;
      const bool useCpuBlendedMorphSource =
         activeShapeCount > 0u &&
         mesh->Dynamic &&
         blendShapes &&
         blendShapes->BlendedStride == sizeof(SkinnedPBRVertex) &&
         blendShapes->BlendedVertices.size() >=
            (size_t(vertexCount) * sizeof(SkinnedPBRVertex));
      uint64_t sourceFingerprint = HashCombine64(
         reinterpret_cast<uint64_t>(mesh),
         static_cast<uint64_t>(vertexCount));
      sourceFingerprint = HashCombine64(
         sourceFingerprint,
         useCpuBlendedMorphSource ? 1ull : 0ull);
      if (useCpuBlendedMorphSource) {
         sourceFingerprint = HashCombine64(
            sourceFingerprint,
            ComputeGpuMaterializedMorphFingerprint(blendShapes));
      }

      const bool sourceBuffersValid =
         bgfx::isValid(source.Positions) &&
         bgfx::isValid(source.Normals) &&
         bgfx::isValid(source.UVs) &&
         bgfx::isValid(source.BoneIndices) &&
         bgfx::isValid(source.BoneWeights);
      const bool needsSourceRecreate =
         source.VertexCount != vertexCount || !sourceBuffersValid;
      const bool needsSourceUpload =
         needsSourceRecreate ||
         source.SourceFingerprint != sourceFingerprint ||
         source.UsesCpuBlendedMorphSource != useCpuBlendedMorphSource;
      if (needsSourceUpload) {
         std::vector<glm::vec4> positions(vertexCount, glm::vec4(0.0f));
         std::vector<glm::vec4> normals(vertexCount, glm::vec4(0.0f));
         std::vector<glm::vec4> uvs(vertexCount, glm::vec4(0.0f));
         std::vector<glm::vec4> boneIndices(vertexCount, glm::vec4(0.0f));
         std::vector<glm::vec4> boneWeights(vertexCount, glm::vec4(0.0f));

         if (useCpuBlendedMorphSource) {
            const auto* blendedVertices =
               reinterpret_cast<const SkinnedPBRVertex*>(blendShapes->BlendedVertices.data());
            for (uint32_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex) {
               const SkinnedPBRVertex& blended = blendedVertices[vertexIndex];
               positions[vertexIndex] = glm::vec4(blended.x, blended.y, blended.z, 1.0f);
               normals[vertexIndex] = glm::vec4(blended.nx, blended.ny, blended.nz, 0.0f);
               uvs[vertexIndex] = glm::vec4(blended.u, blended.v, 0.0f, 0.0f);
               boneIndices[vertexIndex] = glm::vec4(
                  static_cast<float>(blended.i0),
                  static_cast<float>(blended.i1),
                  static_cast<float>(blended.i2),
                  static_cast<float>(blended.i3));
               boneWeights[vertexIndex] = glm::vec4(
                  blended.w0,
                  blended.w1,
                  blended.w2,
                  blended.w3);
            }
         } else {
            for (uint32_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex) {
               positions[vertexIndex] = glm::vec4(mesh->Vertices[vertexIndex], 1.0f);
               normals[vertexIndex] = glm::vec4(mesh->Normals[vertexIndex], 0.0f);
               if (vertexIndex < mesh->UVs.size()) {
                  uvs[vertexIndex] = glm::vec4(mesh->UVs[vertexIndex], 0.0f, 0.0f);
               }
               const glm::ivec4 indices = mesh->BoneIndices[vertexIndex];
               boneIndices[vertexIndex] = glm::vec4(
                  static_cast<float>(indices.x),
                  static_cast<float>(indices.y),
                  static_cast<float>(indices.z),
                  static_cast<float>(indices.w));
               boneWeights[vertexIndex] = mesh->BoneWeights[vertexIndex];
            }
         }

         auto uploadOrCreateSourceBuffer = [&](bgfx::DynamicVertexBufferHandle& handle,
                                               const std::vector<glm::vec4>& data) -> bool {
            const bgfx::Memory* mem = bgfx::copy(
               data.data(),
               static_cast<uint32_t>(data.size() * sizeof(glm::vec4)));
            if (needsSourceRecreate || !bgfx::isValid(handle)) {
               if (bgfx::isValid(handle)) {
                  bgfx::destroy(handle);
                  handle = BGFX_INVALID_HANDLE;
               }
               handle = bgfx::createDynamicVertexBuffer(mem, computeLayout, BGFX_BUFFER_COMPUTE_READ);
               return bgfx::isValid(handle);
            }

            bgfx::update(handle, 0, mem);
            return true;
         };

         if (needsSourceRecreate) {
            if (bgfx::isValid(source.Positions)) bgfx::destroy(source.Positions);
            if (bgfx::isValid(source.Normals)) bgfx::destroy(source.Normals);
            if (bgfx::isValid(source.UVs)) bgfx::destroy(source.UVs);
            if (bgfx::isValid(source.BoneIndices)) bgfx::destroy(source.BoneIndices);
            if (bgfx::isValid(source.BoneWeights)) bgfx::destroy(source.BoneWeights);
            source = {};
            source.VertexCount = vertexCount;
         }

         if (!uploadOrCreateSourceBuffer(source.Positions, positions) ||
             !uploadOrCreateSourceBuffer(source.Normals, normals) ||
             !uploadOrCreateSourceBuffer(source.UVs, uvs) ||
             !uploadOrCreateSourceBuffer(source.BoneIndices, boneIndices) ||
             !uploadOrCreateSourceBuffer(source.BoneWeights, boneWeights)) {
            if (bgfx::isValid(source.Positions)) bgfx::destroy(source.Positions);
            if (bgfx::isValid(source.Normals)) bgfx::destroy(source.Normals);
            if (bgfx::isValid(source.UVs)) bgfx::destroy(source.UVs);
            if (bgfx::isValid(source.BoneIndices)) bgfx::destroy(source.BoneIndices);
            if (bgfx::isValid(source.BoneWeights)) bgfx::destroy(source.BoneWeights);
            source = {};
            continue;
         }

         source.VertexCount = vertexCount;
         source.SourceFingerprint = sourceFingerprint;
         source.UsesCpuBlendedMorphSource = useCpuBlendedMorphSource;
      }
      if (source.UsesCpuBlendedMorphSource) {
         Profiler::Get().AddCounter("Render/MaterializedCpuMorphSourceMeshes", 1);
      }

      GpuMaterializedSkinnedMeshCache& cache = m_GpuMaterializedSkinnedMeshes[instanceKey];
      if (cache.SourceMesh != mesh ||
          cache.VertexCount != vertexCount ||
          !bgfx::isValid(cache.Output)) {
         if (bgfx::isValid(cache.Output)) {
            bgfx::destroy(cache.Output);
         }

         std::vector<PBRVertex> zeroed(vertexCount);
         cache = {};
         cache.SourceMesh = mesh;
         cache.VertexCount = vertexCount;
         cache.Output = bgfx::createVertexBuffer(
            bgfx::copy(zeroed.data(), static_cast<uint32_t>(zeroed.size() * sizeof(PBRVertex))),
            PBRVertex::layout,
            BGFX_BUFFER_COMPUTE_READ_WRITE);
         if (!bgfx::isValid(cache.Output)) {
            cache = {};
            continue;
         }
      }

      const uint64_t fingerprint =
         ComputeGpuMaterializedSkinningFingerprint(skinning, mesh, blendShapes);
      cache.LastTouchedFrame = m_SkinningAtlasFrameSerial;
      if (cache.LastFingerprint == fingerprint) {
         continue;
      }

      bgfx::setBuffer(
         kGpuMaterializedSkinningSourcePositionStage,
         source.Positions,
         bgfx::Access::Read);
      bgfx::setBuffer(
         kGpuMaterializedSkinningSourceNormalStage,
         source.Normals,
         bgfx::Access::Read);
      bgfx::setBuffer(
         kGpuMaterializedSkinningSourceUvStage,
         source.UVs,
         bgfx::Access::Read);
      bgfx::setBuffer(
         kGpuMaterializedSkinningSourceBoneIndexStage,
         source.BoneIndices,
         bgfx::Access::Read);
      bgfx::setBuffer(
         kGpuMaterializedSkinningSourceBoneWeightStage,
         source.BoneWeights,
         bgfx::Access::Read);
      bgfx::setBuffer(
         kGpuMaterializedSkinningOutputStage,
         cache.Output,
         bgfx::Access::Write);
      BindSkinningInstanceRecord(skinning->GpuInstanceAtlasRecordIndex);
      if (source.UsesCpuBlendedMorphSource) {
         const glm::vec4 zero(0.0f);
         bgfx::setUniform(u_SkinningMorphParams, &zero);
      }
      const glm::vec4 dispatchParams(static_cast<float>(vertexCount), 0.0f, 0.0f, 0.0f);
      bgfx::setUniform(u_GpuMaterializedSkinningDispatch, &dispatchParams.x);
      const uint16_t groupsX = static_cast<uint16_t>(
         (vertexCount + kGpuMaterializedSkinningGroupSize - 1u) / kGpuMaterializedSkinningGroupSize);
      bgfx::dispatch(kGpuMaterializedSkinningViewId, m_GpuMaterializedSkinningProgram, groupsX, 1, 1);
      cache.LastFingerprint = fingerprint;
   }

   if ((m_SkinningAtlasFrameSerial & 127ull) == 0ull) {
      for (auto it = m_GpuMaterializedSkinnedMeshes.begin(); it != m_GpuMaterializedSkinnedMeshes.end();) {
         if (it->second.LastTouchedFrame + 256ull < m_SkinningAtlasFrameSerial) {
            if (bgfx::isValid(it->second.Output)) {
               bgfx::destroy(it->second.Output);
            }
            it = m_GpuMaterializedSkinnedMeshes.erase(it);
         } else {
            ++it;
         }
      }
   }
}

bgfx::VertexBufferHandle Renderer::GetOrCreateGpuMorphVertexBuffer(const Mesh* mesh)
{
   if (!mesh) {
      return BGFX_INVALID_HANDLE;
   }

   auto it = m_GpuMorphVertexBuffers.find(mesh);
   if (it != m_GpuMorphVertexBuffers.end()) {
      return it->second;
   }

   const size_t vertexCount = mesh->Vertices.size();
   if (vertexCount == 0 ||
       mesh->Normals.size() < vertexCount ||
       mesh->BoneIndices.size() < vertexCount ||
       mesh->BoneWeights.size() < vertexCount) {
      return BGFX_INVALID_HANDLE;
   }

   std::vector<SkinnedPBRMorphVertex> vertices(vertexCount);
   for (size_t i = 0; i < vertexCount; ++i) {
      auto& out = vertices[i];
      const glm::vec3& pos = mesh->Vertices[i];
      const glm::vec3& nrm = mesh->Normals[i];
      const glm::vec2 uv = i < mesh->UVs.size() ? mesh->UVs[i] : glm::vec2(0.0f);
      const glm::ivec4 boneIndices = mesh->BoneIndices[i];
      const glm::vec4 boneWeights = mesh->BoneWeights[i];

      out.x = pos.x;
      out.y = pos.y;
      out.z = pos.z;
      out.nx = nrm.x;
      out.ny = nrm.y;
      out.nz = nrm.z;
      out.u = uv.x;
      out.v = uv.y;
      out.morphVertexId = static_cast<float>(i);
      out.i0 = static_cast<uint8_t>(boneIndices.x);
      out.i1 = static_cast<uint8_t>(boneIndices.y);
      out.i2 = static_cast<uint8_t>(boneIndices.z);
      out.i3 = static_cast<uint8_t>(boneIndices.w);
      out.w0 = boneWeights.x;
      out.w1 = boneWeights.y;
      out.w2 = boneWeights.z;
      out.w3 = boneWeights.w;
   }

   const bgfx::Memory* mem =
      bgfx::copy(vertices.data(), static_cast<uint32_t>(vertices.size() * sizeof(SkinnedPBRMorphVertex)));
   const bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(mem, SkinnedPBRMorphVertex::layout);
   if (!bgfx::isValid(vbh)) {
      return BGFX_INVALID_HANDLE;
   }

   m_GpuMorphVertexBuffers.emplace(mesh, vbh);
   return vbh;
}

bool Renderer::PrepareGpuSkinningAtlases(
   Scene& scene,
   const std::vector<EntityID>& primaryEntities,
   const std::vector<EntityID>* secondaryEntities)
{
   auto& profiler = Profiler::Get();
   profiler.AddCounter("Render/SkinningAtlasPrepCalls", 1);
   if (!m_GpuSkinningAtlasSupported ||
       !bgfx::isValid(s_BoneAtlas) ||
       !bgfx::isValid(s_BoneRemapAtlas) ||
       !bgfx::isValid(s_SkinningInstanceAtlas) ||
       !bgfx::isValid(u_SkinningBoneAtlasInfo) ||
       !bgfx::isValid(u_SkinningRemapAtlasInfo) ||
       !bgfx::isValid(u_SkinningInstanceAtlasInfo) ||
       !bgfx::isValid(u_SkinningParams) ||
       !bgfx::isValid(u_SkinningExtra) ||
       !bgfx::isValid(u_SkinningInstanceRecord) ||
       !bgfx::isValid(u_SkinningMeshFromSkeleton) ||
       !bgfx::isValid(u_SkinningMorphParams)) {
      return false;
   }

   const auto requestCoveredByPreparedAtlas = [&](const std::vector<EntityID>& entities) {
      for (EntityID eid : entities) {
         if (eid == INVALID_ENTITY_ID) {
            continue;
         }
         if (m_SkinningAtlasCoveredEntities.find(eid) == m_SkinningAtlasCoveredEntities.end()) {
            return false;
         }
      }
      return true;
   };

   if (m_SkinningAtlasPreparedFrameSerial == m_SkinningAtlasFrameSerial &&
       m_SkinningAtlasPreparedScene == &scene &&
       requestCoveredByPreparedAtlas(primaryEntities) &&
       (secondaryEntities == nullptr || requestCoveredByPreparedAtlas(*secondaryEntities))) {
      profiler.AddCounter("Render/SkinningAtlasPrepCacheHits", 1);
      return m_GpuSkinningAtlasReady;
   }

   if (m_SkinningAtlasPreparedFrameSerial != m_SkinningAtlasFrameSerial ||
       m_SkinningAtlasPreparedScene != &scene) {
      m_SkinningAtlasPreparedFrameSerial = m_SkinningAtlasFrameSerial;
      m_SkinningAtlasPreparedScene = &scene;
      m_SkinningAtlasUnionEntities.clear();
      m_SkinningAtlasCoveredEntities.clear();
   }

   bool coverageExpanded = false;
   const auto appendCoverage = [&](const std::vector<EntityID>& entities) {
      for (EntityID eid : entities) {
         if (eid == INVALID_ENTITY_ID) {
            continue;
         }
         if (m_SkinningAtlasCoveredEntities.emplace(eid).second) {
            m_SkinningAtlasUnionEntities.push_back(eid);
            coverageExpanded = true;
         }
      }
   };
   appendCoverage(primaryEntities);
   if (secondaryEntities != nullptr) {
      appendCoverage(*secondaryEntities);
   }

   if (!coverageExpanded) {
      profiler.AddCounter("Render/SkinningAtlasPrepCacheHits", 1);
      return m_GpuSkinningAtlasReady;
   }

   profiler.AddCounter("Render/SkinningAtlasPrepRebuilds", 1);
   m_GpuSkinningAtlasReady = false;

   m_SkinningBoneAtlasCpu.clear();
   m_SkinningRemapAtlasCpu.clear();
   m_SkinningInstanceRecords.clear();
   m_SkinningInstanceAtlasCpu.clear();
   m_MorphVertexAtlasCpu.clear();
   m_MorphEntryAtlasCpu.clear();
   m_MorphActiveAtlasCpu.clear();

   const glm::mat4 identity(1.0f);
   m_SkinningBoneAtlasCpu.reserve(4);
   m_SkinningBoneAtlasCpu.push_back(identity[0]);
   m_SkinningBoneAtlasCpu.push_back(identity[1]);
   m_SkinningBoneAtlasCpu.push_back(identity[2]);
   m_SkinningBoneAtlasCpu.push_back(identity[3]);

   std::unordered_set<const SkinningComponent*> visitedSkins;
   visitedSkins.reserve(m_SkinningAtlasUnionEntities.size());
   std::unordered_map<const SkeletonComponent*, uint32_t> sharedSkeletonBase;
   std::unordered_map<const Mesh*, uint32_t> compactRemapBase;
   std::unordered_map<uint64_t, MorphGpuGeometryInfo> morphGeometryBase;
   sharedSkeletonBase.reserve(64);
   compactRemapBase.reserve(64);
   morphGeometryBase.reserve(64);
   uint64_t boneAtlasFingerprint = 1469598103934665603ull;
   uint64_t remapAtlasFingerprint = 1469598103934665603ull;
   uint64_t instanceAtlasFingerprint = 1469598103934665603ull;
   uint64_t morphVertexAtlasFingerprint = 1469598103934665603ull;
   uint64_t morphEntryAtlasFingerprint = 1469598103934665603ull;
   uint64_t morphActiveAtlasFingerprint = 1469598103934665603ull;
   boneAtlasFingerprint = HashCombine64(boneAtlasFingerprint, reinterpret_cast<uint64_t>(&scene));
   remapAtlasFingerprint = HashCombine64(remapAtlasFingerprint, reinterpret_cast<uint64_t>(&scene));
   instanceAtlasFingerprint = HashCombine64(instanceAtlasFingerprint, reinterpret_cast<uint64_t>(&scene));
   morphVertexAtlasFingerprint = HashCombine64(morphVertexAtlasFingerprint, reinterpret_cast<uint64_t>(&scene));
   morphEntryAtlasFingerprint = HashCombine64(morphEntryAtlasFingerprint, reinterpret_cast<uint64_t>(&scene));
   morphActiveAtlasFingerprint = HashCombine64(morphActiveAtlasFingerprint, reinterpret_cast<uint64_t>(&scene));

   uint64_t preparedSkinnedMeshes = 0;
   uint64_t preparedSharedSkeletonMeshes = 0;
   uint64_t preparedCompactRemaps = 0;
   uint64_t preparedDynamicRemaps = 0;
   uint64_t preparedCorrectionMeshes = 0;
   uint64_t preparedInstanceRecords = 0;
   uint64_t preparedMorphMeshes = 0;
   uint64_t preparedMorphActiveLists = 0;
   uint64_t morphGeometryCacheHits = 0;
   uint64_t morphGeometryCacheMisses = 0;

   auto appendPalette = [&](const glm::mat4* palette, uint32_t matrixCount) -> uint32_t {
      const uint32_t baseMatrix = static_cast<uint32_t>(m_SkinningBoneAtlasCpu.size() / 4);
      m_SkinningBoneAtlasCpu.reserve(m_SkinningBoneAtlasCpu.size() + size_t(matrixCount) * 4u);
      for (uint32_t i = 0; i < matrixCount; ++i) {
         m_SkinningBoneAtlasCpu.push_back(palette[i][0]);
         m_SkinningBoneAtlasCpu.push_back(palette[i][1]);
         m_SkinningBoneAtlasCpu.push_back(palette[i][2]);
         m_SkinningBoneAtlasCpu.push_back(palette[i][3]);
      }
      return baseMatrix;
   };

   auto appendRemap = [&](const std::vector<uint16_t>& remapData) -> uint32_t {
      const uint32_t baseIndex = static_cast<uint32_t>(m_SkinningRemapAtlasCpu.size());
      m_SkinningRemapAtlasCpu.reserve(m_SkinningRemapAtlasCpu.size() + remapData.size());
      for (uint16_t mappedIndex : remapData) {
         m_SkinningRemapAtlasCpu.push_back(mappedIndex);
      }
      return baseIndex;
   };

   auto appendMorphGeometry = [&](const Mesh* mesh, const BlendShapeComponent* blendShapes)
      -> std::optional<MorphGpuGeometryInfo> {
      if (!CanUseGpuMorphTargets(mesh, blendShapes)) {
         return std::nullopt;
      }

      const size_t vertexCount = mesh->Vertices.size();
      const uint64_t geometryKey = HashCombine64(
         reinterpret_cast<uint64_t>(mesh),
         blendShapes->GetGeometrySignature(vertexCount));
      auto existing = morphGeometryBase.find(geometryKey);
      if (existing != morphGeometryBase.end()) {
         return existing->second;
      }

      auto cachedGeometryIt = m_CachedGpuMorphGeometries.find(geometryKey);
      if (cachedGeometryIt == m_CachedGpuMorphGeometries.end()) {
         ++morphGeometryCacheMisses;

         CachedGpuMorphGeometry cachedGeometry{};
         cachedGeometry.VertexCount = static_cast<uint32_t>(vertexCount);

         std::vector<uint32_t> entryCounts(vertexCount, 0u);
         uint32_t totalEntryCount = 0;
         const float thresholdSq = 1e-12f;
         auto countDenseDelta = [&](const BlendShape& shape) {
            const size_t denseCount = std::min(vertexCount, shape.DeltaPos.size());
            for (size_t vertexIndex = 0; vertexIndex < denseCount; ++vertexIndex) {
               const glm::vec3 deltaPos = shape.DeltaPos[vertexIndex];
               const glm::vec3 deltaNorm =
                  vertexIndex < shape.DeltaNormal.size()
                     ? shape.DeltaNormal[vertexIndex]
                     : glm::vec3(0.0f);
               if ((glm::dot(deltaPos, deltaPos) + glm::dot(deltaNorm, deltaNorm)) <= thresholdSq) {
                  continue;
               }
               ++entryCounts[vertexIndex];
               ++totalEntryCount;
            }
         };

         for (const auto& shape : blendShapes->Shapes) {
            if (shape.IsSparse) {
               for (uint32_t vertexIndex : shape.SparseIndices) {
                  if (vertexIndex >= vertexCount) {
                     continue;
                  }
                  ++entryCounts[vertexIndex];
                  ++totalEntryCount;
               }
            }
            else {
               countDenseDelta(shape);
            }
         }

         std::vector<uint32_t> entryOffsets(vertexCount, 0u);
         uint32_t runningOffset = 0;
         for (size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex) {
            entryOffsets[vertexIndex] = runningOffset;
            runningOffset += entryCounts[vertexIndex];
         }

         struct CpuMorphEntry {
            float ShapeIndex = 0.0f;
            glm::vec3 DeltaPos = glm::vec3(0.0f);
            glm::vec3 DeltaNorm = glm::vec3(0.0f);
         };
         std::vector<CpuMorphEntry> entries(totalEntryCount);
         std::vector<uint32_t> writeCursor = entryOffsets;
         auto writeDenseDelta = [&](uint32_t shapeIndex, const BlendShape& shape) {
            const size_t denseCount = std::min(vertexCount, shape.DeltaPos.size());
            for (size_t vertexIndex = 0; vertexIndex < denseCount; ++vertexIndex) {
               const glm::vec3 deltaPos = shape.DeltaPos[vertexIndex];
               const glm::vec3 deltaNorm =
                  vertexIndex < shape.DeltaNormal.size()
                     ? shape.DeltaNormal[vertexIndex]
                     : glm::vec3(0.0f);
               if ((glm::dot(deltaPos, deltaPos) + glm::dot(deltaNorm, deltaNorm)) <= thresholdSq) {
                  continue;
               }
               const uint32_t writeIndex = writeCursor[vertexIndex]++;
               entries[writeIndex].ShapeIndex = static_cast<float>(shapeIndex);
               entries[writeIndex].DeltaPos = deltaPos;
               entries[writeIndex].DeltaNorm = deltaNorm;
            }
         };

         for (size_t shapeIndex = 0; shapeIndex < blendShapes->Shapes.size(); ++shapeIndex) {
            const auto& shape = blendShapes->Shapes[shapeIndex];
            if (shape.IsSparse) {
               for (size_t sparseIndex = 0; sparseIndex < shape.SparseIndices.size(); ++sparseIndex) {
                  const uint32_t vertexIndex = shape.SparseIndices[sparseIndex];
                  if (vertexIndex >= vertexCount) {
                     continue;
                  }
                  const uint32_t writeIndex = writeCursor[vertexIndex]++;
                  entries[writeIndex].ShapeIndex = static_cast<float>(shapeIndex);
                  entries[writeIndex].DeltaPos =
                     sparseIndex < shape.SparseDeltaPos.size()
                        ? shape.SparseDeltaPos[sparseIndex]
                        : glm::vec3(0.0f);
                  entries[writeIndex].DeltaNorm =
                     sparseIndex < shape.SparseDeltaNorm.size()
                        ? shape.SparseDeltaNorm[sparseIndex]
                        : glm::vec3(0.0f);
               }
            }
            else {
               writeDenseDelta(static_cast<uint32_t>(shapeIndex), shape);
            }
         }

         cachedGeometry.VertexTexels.reserve(vertexCount);
         for (size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex) {
            cachedGeometry.VertexTexels.emplace_back(
               static_cast<float>(entryOffsets[vertexIndex]),
               static_cast<float>(entryCounts[vertexIndex]),
               0.0f,
               0.0f);
         }

         cachedGeometry.EntryTexels.reserve(size_t(totalEntryCount) * 2u);
         for (const auto& entry : entries) {
            cachedGeometry.EntryTexels.emplace_back(
               entry.ShapeIndex,
               entry.DeltaPos.x,
               entry.DeltaPos.y,
               entry.DeltaPos.z);
            cachedGeometry.EntryTexels.emplace_back(
               entry.DeltaNorm.x,
               entry.DeltaNorm.y,
               entry.DeltaNorm.z,
               0.0f);
         }
         cachedGeometry.EntryCount = totalEntryCount;

         cachedGeometryIt =
            m_CachedGpuMorphGeometries.emplace(geometryKey, std::move(cachedGeometry)).first;
      } else {
         ++morphGeometryCacheHits;
      }

      const CachedGpuMorphGeometry& cachedGeometry = cachedGeometryIt->second;
      MorphGpuGeometryInfo geometry{};
      geometry.VertexRangeBase = static_cast<uint32_t>(m_MorphVertexAtlasCpu.size());
      geometry.EntryBase = static_cast<uint32_t>(m_MorphEntryAtlasCpu.size() / 2u);
      geometry.EntryCount = cachedGeometry.EntryCount;

      m_MorphVertexAtlasCpu.reserve(m_MorphVertexAtlasCpu.size() + cachedGeometry.VertexTexels.size());
      for (const glm::vec4& texel : cachedGeometry.VertexTexels) {
         m_MorphVertexAtlasCpu.emplace_back(
            texel.x + static_cast<float>(geometry.EntryBase),
            texel.y,
            texel.z,
            texel.w);
      }

      m_MorphEntryAtlasCpu.reserve(m_MorphEntryAtlasCpu.size() + cachedGeometry.EntryTexels.size());
      m_MorphEntryAtlasCpu.insert(
         m_MorphEntryAtlasCpu.end(),
         cachedGeometry.EntryTexels.begin(),
         cachedGeometry.EntryTexels.end());

      morphGeometryBase.emplace(geometryKey, geometry);
      morphVertexAtlasFingerprint = HashCombine64(morphVertexAtlasFingerprint, geometryKey);
      morphVertexAtlasFingerprint = HashCombine64(
         morphVertexAtlasFingerprint,
         static_cast<uint64_t>(cachedGeometry.VertexCount));
      morphEntryAtlasFingerprint = HashCombine64(morphEntryAtlasFingerprint, geometryKey);
      morphEntryAtlasFingerprint = HashCombine64(
         morphEntryAtlasFingerprint,
         static_cast<uint64_t>(cachedGeometry.EntryCount));
      return geometry;
   };

   auto processEntityList = [&](const std::vector<EntityID>& entities) {
      for (EntityID eid : entities) {
         EntityData* data = scene.GetEntityData(eid);
         if (!data || !data->Mesh || !data->Mesh->mesh || !data->Skinning) {
            continue;
         }

         SkinningComponent* skinning = data->Skinning.get();
         if (!visitedSkins.emplace(skinning).second) {
            continue;
         }

         Mesh* mesh = data->Mesh->mesh.get();
         if (!mesh) {
            skinning->ResetGpuSkinningBinding();
            continue;
         }

         skinning->ResetGpuSkinningBinding();
         ++preparedSkinnedMeshes;

         if (!skinning->UsesGpuSharedSkeleton() ||
             skinning->GpuSourceSkeleton == nullptr ||
             skinning->GpuSourceSkeleton->BoneCount == 0) {
            continue;
         }

         uint32_t baseMatrix = 0;
         auto it = sharedSkeletonBase.find(skinning->GpuSourceSkeleton);
         if (it == sharedSkeletonBase.end()) {
            baseMatrix = appendPalette(
               skinning->GpuSourceSkeleton->BonePalette.data(),
               skinning->GpuSourceSkeleton->BoneCount);
            sharedSkeletonBase.emplace(skinning->GpuSourceSkeleton, baseMatrix);
            boneAtlasFingerprint = HashCombine64(
               boneAtlasFingerprint,
               reinterpret_cast<uint64_t>(skinning->GpuSourceSkeleton));
            boneAtlasFingerprint = HashCombine64(
               boneAtlasFingerprint,
               static_cast<uint64_t>(skinning->GpuSourceSkeleton->PoseFrameId));
            boneAtlasFingerprint = HashCombine64(
               boneAtlasFingerprint,
               static_cast<uint64_t>(skinning->GpuSourceSkeleton->BoneCount));
         } else {
            baseMatrix = it->second;
         }

         skinning->GpuBoneAtlasBase = baseMatrix;
         skinning->GpuBoneAtlasCount = skinning->GpuSourceSkeleton->BoneCount;
         skinning->GpuBoneAtlasBindingValid = true;

         const std::vector<uint16_t>* activeRemap = nullptr;
         bool usesDynamicRemap = false;
         if (!skinning->GpuBoneIndexRemap.empty()) {
            activeRemap = &skinning->GpuBoneIndexRemap;
            usesDynamicRemap = true;
         } else if (mesh->UsesCompactSkinningPalette()) {
            activeRemap = &mesh->SkinningBoneRemap;
         }

         if (activeRemap != nullptr && !activeRemap->empty()) {
            uint32_t baseRemap = 0;
            if (!usesDynamicRemap) {
               auto remapIt = compactRemapBase.find(mesh);
               if (remapIt == compactRemapBase.end()) {
                  baseRemap = appendRemap(*activeRemap);
                  compactRemapBase.emplace(mesh, baseRemap);
                  remapAtlasFingerprint = HashCombine64(remapAtlasFingerprint, reinterpret_cast<uint64_t>(mesh));
                  remapAtlasFingerprint = HashCombine64(
                     remapAtlasFingerprint,
                     static_cast<uint64_t>(activeRemap->size()));
                  remapAtlasFingerprint = HashCombine64(
                     remapAtlasFingerprint,
                     mesh->GetCachedSkinningBoneRemapHash());
               } else {
                  baseRemap = remapIt->second;
               }
               ++preparedCompactRemaps;
            } else {
               baseRemap = appendRemap(*activeRemap);
               remapAtlasFingerprint = HashCombine64(remapAtlasFingerprint, reinterpret_cast<uint64_t>(skinning));
               remapAtlasFingerprint = HashCombine64(
                  remapAtlasFingerprint,
                  static_cast<uint64_t>(activeRemap->size()));
               remapAtlasFingerprint = HashCombine64(
                  remapAtlasFingerprint,
                  skinning->GetGpuBoneIndexRemapHash());
               ++preparedDynamicRemaps;
            }

            skinning->GpuRemapAtlasBase = baseRemap;
            skinning->GpuRemapAtlasCount = static_cast<uint32_t>(activeRemap->size());
         }

         if (!skinning->GpuBoneCorrectionPalette.empty()) {
            const uint32_t correctionBase = appendPalette(
               skinning->GpuBoneCorrectionPalette.data(),
               static_cast<uint32_t>(skinning->GpuBoneCorrectionPalette.size()));
            skinning->GpuCorrectionAtlasBase = correctionBase;
            skinning->GpuCorrectionAtlasCount =
               static_cast<uint32_t>(skinning->GpuBoneCorrectionPalette.size());
            boneAtlasFingerprint = HashCombine64(
               boneAtlasFingerprint,
               reinterpret_cast<uint64_t>(skinning));
            boneAtlasFingerprint = HashCombine64(
               boneAtlasFingerprint,
               static_cast<uint64_t>(skinning->GpuBoneCorrectionPalette.size()));
            boneAtlasFingerprint = HashCombine64(
               boneAtlasFingerprint,
               skinning->GetGpuBoneCorrectionPaletteHash());
            ++preparedCorrectionMeshes;
         }

         const uint32_t sourceCount =
            skinning->GpuRemapAtlasCount > 0
               ? skinning->GpuRemapAtlasCount
               : (skinning->GpuCorrectionAtlasCount > 0
                     ? skinning->GpuCorrectionAtlasCount
                     : skinning->BoneCount);
         Renderer::GpuSkinningInstanceRecord record{};
         record.Params = glm::vec4(
            static_cast<float>(skinning->GpuBoneAtlasBase),
            static_cast<float>(skinning->GpuRemapAtlasBase),
            (skinning->GpuRemapAtlasCount > 0) ? 1.0f : 0.0f,
            static_cast<float>(
               std::max<int32_t>(static_cast<int32_t>(skinning->GpuBoneAtlasCount) - 1, 0)));
         record.Extra = glm::vec4(
            static_cast<float>(skinning->GpuCorrectionAtlasBase),
            (skinning->GpuCorrectionAtlasCount > 0) ? 1.0f : 0.0f,
            static_cast<float>(std::max<int32_t>(static_cast<int32_t>(sourceCount) - 1, 0)),
            0.0f);
         record.MeshFromSkeleton =
            skinning->UsesGpuSharedSkeleton() ? skinning->GpuMeshFromSkeleton : glm::mat4(1.0f);

         if (ShouldUseGpuMorphTargets(skinning, mesh, data->BlendShapes.get())) {
            const std::optional<MorphGpuGeometryInfo> geometry = appendMorphGeometry(mesh, data->BlendShapes.get());
            const uint32_t activeShapeCount = data->BlendShapes
               ? data->BlendShapes->CountActiveShapes(kGpuMorphWeightThreshold)
               : 0u;
            if (geometry.has_value() && activeShapeCount <= kGpuMorphMaxActiveShapes) {
               const uint32_t activeBase = static_cast<uint32_t>(m_MorphActiveAtlasCpu.size());
               if (data->BlendShapes) {
                  for (size_t shapeIndex = 0; shapeIndex < data->BlendShapes->Shapes.size(); ++shapeIndex) {
                     const float weight = data->BlendShapes->Shapes[shapeIndex].Weight;
                     if (std::abs(weight) <= kGpuMorphWeightThreshold) {
                        continue;
                     }
                     m_MorphActiveAtlasCpu.emplace_back(
                        static_cast<float>(shapeIndex),
                        weight,
                        0.0f,
                        0.0f);
                  }
               }

               record.Morph = glm::vec4(
                  static_cast<float>(geometry->VertexRangeBase),
                  static_cast<float>(activeBase),
                  static_cast<float>(activeShapeCount),
                  1.0f);
               morphActiveAtlasFingerprint = HashCombine64(morphActiveAtlasFingerprint, reinterpret_cast<uint64_t>(data->BlendShapes.get()));
               morphActiveAtlasFingerprint = HashCombine64(morphActiveAtlasFingerprint, static_cast<uint64_t>(activeShapeCount));
               for (size_t shapeIndex = 0; shapeIndex < data->BlendShapes->Shapes.size(); ++shapeIndex) {
                  const float weight = data->BlendShapes->Shapes[shapeIndex].Weight;
                  if (std::abs(weight) <= kGpuMorphWeightThreshold) {
                     continue;
                  }
                  morphActiveAtlasFingerprint = HashCombine64(
                     morphActiveAtlasFingerprint,
                     static_cast<uint64_t>(shapeIndex));
                  morphActiveAtlasFingerprint = HashCombine64(
                     morphActiveAtlasFingerprint,
                     HashBytes64(&weight, sizeof(weight)));
               }
               ++preparedMorphMeshes;
               ++preparedMorphActiveLists;
            }
         }

         record.ObjectIdPacked = PackObjectIdColor(eid);
         skinning->GpuInstanceAtlasRecordIndex =
            static_cast<uint32_t>(m_SkinningInstanceRecords.size());
         skinning->GpuInstanceAtlasRecordValid = true;
         m_SkinningInstanceRecords.push_back(record);
         instanceAtlasFingerprint = HashCombine64(instanceAtlasFingerprint, reinterpret_cast<uint64_t>(skinning));
         instanceAtlasFingerprint = HashCombine64(instanceAtlasFingerprint, static_cast<uint64_t>(eid));
         instanceAtlasFingerprint = HashCombine64(instanceAtlasFingerprint, HashBytes64(&record, sizeof(record)));
         ++preparedInstanceRecords;

         ++preparedSharedSkeletonMeshes;
      }
   };

   processEntityList(m_SkinningAtlasUnionEntities);

   if (preparedSkinnedMeshes == 0) {
      profiler.SetCounter("Render/SkinningAtlasSkinnedMeshes", preparedSkinnedMeshes);
      profiler.SetCounter("Render/SkinningAtlasSharedSkeletonMeshes", preparedSharedSkeletonMeshes);
      profiler.SetCounter("Render/SkinningAtlasCpuPaletteMeshes", 0);
      profiler.SetCounter("Render/SkinningAtlasLegacyMeshes", 0);
      profiler.SetCounter("Render/SkinningAtlasCompactRemapMeshes", preparedCompactRemaps);
      profiler.SetCounter("Render/SkinningAtlasDynamicRemapMeshes", preparedDynamicRemaps);
      profiler.SetCounter("Render/SkinningAtlasCorrectionMeshes", preparedCorrectionMeshes);
      profiler.SetCounter("Render/SkinningAtlasInstanceRecords", 0);
      profiler.SetCounter("Render/SkinningAtlasMatrices", 0);
      profiler.SetCounter("Render/SkinningAtlasRemapEntries", 0);
      profiler.SetCounter("Render/SkinningAtlasMorphMeshes", 0);
      profiler.SetCounter("Render/SkinningAtlasMorphActiveLists", 0);
      profiler.SetCounter("Render/SkinningAtlasMorphActiveEntries", 0);
      profiler.SetCounter("Render/MorphGeometryCacheHits", morphGeometryCacheHits);
      profiler.SetCounter("Render/MorphGeometryCacheMisses", morphGeometryCacheMisses);
      return false;
   }

   auto chooseWidth = [](uint32_t texelCount, uint32_t alignment) -> uint16_t {
      const uint32_t minWidth = std::max<uint32_t>(alignment, 1u);
      uint32_t width = std::min<uint32_t>(1024u, std::max<uint32_t>(texelCount, minWidth));
      if (alignment > 1u) {
         width = ((width + alignment - 1u) / alignment) * alignment;
      }
      return static_cast<uint16_t>(width);
   };

   const uint32_t boneTexelCount = static_cast<uint32_t>(m_SkinningBoneAtlasCpu.size());
   const uint16_t boneWidth = chooseWidth(boneTexelCount, 4u);
   const uint16_t boneHeight = static_cast<uint16_t>(std::max<uint32_t>(1u, (boneTexelCount + boneWidth - 1u) / boneWidth));
   m_SkinningBoneAtlasCpu.resize(size_t(boneWidth) * size_t(boneHeight), glm::vec4(0.0f));
   if (!m_SkinningBoneAtlasCpu.empty()) {
      boneAtlasFingerprint = HashCombine64(
         boneAtlasFingerprint,
         HashBytes64(
            m_SkinningBoneAtlasCpu.data(),
            m_SkinningBoneAtlasCpu.size() * sizeof(glm::vec4)));
   }

   const uint32_t remapTexelCount = static_cast<uint32_t>(m_SkinningRemapAtlasCpu.size());
   const uint16_t remapWidth = static_cast<uint16_t>(remapTexelCount > 0 ? chooseWidth(remapTexelCount, 1u) : 1u);
   const uint16_t remapHeight = static_cast<uint16_t>(std::max<uint32_t>(1u, (std::max<uint32_t>(remapTexelCount, 1u) + remapWidth - 1u) / remapWidth));
   m_SkinningRemapAtlasCpu.resize(size_t(remapWidth) * size_t(remapHeight), uint16_t{ 0 });

   const uint32_t instanceTexelCount = static_cast<uint32_t>(m_SkinningInstanceRecords.size()) * kSkinningInstanceRecordTexelCount;
   const uint16_t instanceWidth = static_cast<uint16_t>(instanceTexelCount > 0 ? chooseWidth(instanceTexelCount, 1u) : 1u);
   const uint16_t instanceHeight = static_cast<uint16_t>(std::max<uint32_t>(1u, (std::max<uint32_t>(instanceTexelCount, 1u) + instanceWidth - 1u) / instanceWidth));
   m_SkinningInstanceAtlasCpu.assign(size_t(instanceWidth) * size_t(instanceHeight), glm::vec4(0.0f));
   for (size_t recordIndex = 0; recordIndex < m_SkinningInstanceRecords.size(); ++recordIndex) {
      const auto& record = m_SkinningInstanceRecords[recordIndex];
      const size_t baseTexel = recordIndex * kSkinningInstanceRecordTexelCount;
      if (baseTexel + 7u >= m_SkinningInstanceAtlasCpu.size()) {
         break;
      }
      m_SkinningInstanceAtlasCpu[baseTexel + 0u] = record.Params;
      m_SkinningInstanceAtlasCpu[baseTexel + 1u] = record.Extra;
      m_SkinningInstanceAtlasCpu[baseTexel + 2u] = record.MeshFromSkeleton[0];
      m_SkinningInstanceAtlasCpu[baseTexel + 3u] = record.MeshFromSkeleton[1];
      m_SkinningInstanceAtlasCpu[baseTexel + 4u] = record.MeshFromSkeleton[2];
      m_SkinningInstanceAtlasCpu[baseTexel + 5u] = record.MeshFromSkeleton[3];
      m_SkinningInstanceAtlasCpu[baseTexel + 6u] = record.Morph;
      m_SkinningInstanceAtlasCpu[baseTexel + 7u] = record.ObjectIdPacked;
   }

   const uint32_t morphVertexTexelCount = static_cast<uint32_t>(m_MorphVertexAtlasCpu.size());
   const uint16_t morphVertexWidth = static_cast<uint16_t>(morphVertexTexelCount > 0 ? chooseWidth(morphVertexTexelCount, 1u) : 1u);
   const uint16_t morphVertexHeight = static_cast<uint16_t>(std::max<uint32_t>(1u, (std::max<uint32_t>(morphVertexTexelCount, 1u) + morphVertexWidth - 1u) / morphVertexWidth));
   m_MorphVertexAtlasCpu.resize(size_t(morphVertexWidth) * size_t(morphVertexHeight), glm::vec4(0.0f));

   const uint32_t morphEntryTexelCount = static_cast<uint32_t>(m_MorphEntryAtlasCpu.size());
   const uint16_t morphEntryWidth = static_cast<uint16_t>(morphEntryTexelCount > 0 ? chooseWidth(morphEntryTexelCount, 1u) : 1u);
   const uint16_t morphEntryHeight = static_cast<uint16_t>(std::max<uint32_t>(1u, (std::max<uint32_t>(morphEntryTexelCount, 1u) + morphEntryWidth - 1u) / morphEntryWidth));
   m_MorphEntryAtlasCpu.resize(size_t(morphEntryWidth) * size_t(morphEntryHeight), glm::vec4(0.0f));

   const uint32_t morphActiveTexelCount = static_cast<uint32_t>(m_MorphActiveAtlasCpu.size());
   const uint16_t morphActiveWidth = static_cast<uint16_t>(morphActiveTexelCount > 0 ? chooseWidth(morphActiveTexelCount, 1u) : 1u);
   const uint16_t morphActiveHeight = static_cast<uint16_t>(std::max<uint32_t>(1u, (std::max<uint32_t>(morphActiveTexelCount, 1u) + morphActiveWidth - 1u) / morphActiveWidth));
   m_MorphActiveAtlasCpu.resize(size_t(morphActiveWidth) * size_t(morphActiveHeight), glm::vec4(0.0f));

   boneAtlasFingerprint = HashCombine64(boneAtlasFingerprint, static_cast<uint64_t>(boneTexelCount));
   remapAtlasFingerprint = HashCombine64(remapAtlasFingerprint, static_cast<uint64_t>(remapTexelCount));
   instanceAtlasFingerprint = HashCombine64(instanceAtlasFingerprint, static_cast<uint64_t>(instanceTexelCount));
   morphVertexAtlasFingerprint = HashCombine64(morphVertexAtlasFingerprint, static_cast<uint64_t>(morphVertexTexelCount));
   morphEntryAtlasFingerprint = HashCombine64(morphEntryAtlasFingerprint, static_cast<uint64_t>(morphEntryTexelCount));
   morphActiveAtlasFingerprint = HashCombine64(morphActiveAtlasFingerprint, static_cast<uint64_t>(morphActiveTexelCount));

   const uint64_t textureFlags =
      BGFX_SAMPLER_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
   bool recreatedBoneAtlas = false;
   if (!bgfx::isValid(m_SkinningBoneAtlasTex) ||
       m_SkinningBoneAtlasWidth != boneWidth ||
       m_SkinningBoneAtlasHeight != boneHeight) {
      if (bgfx::isValid(m_SkinningBoneAtlasTex)) {
         bgfx::destroy(m_SkinningBoneAtlasTex);
      }
      m_SkinningBoneAtlasTex = bgfx::createTexture2D(
         boneWidth,
         boneHeight,
         false,
         1,
         bgfx::TextureFormat::RGBA32F,
         textureFlags);
      m_SkinningBoneAtlasWidth = boneWidth;
      m_SkinningBoneAtlasHeight = boneHeight;
      recreatedBoneAtlas = true;
   }

   bool recreatedRemapAtlas = false;
   if (!bgfx::isValid(m_SkinningRemapAtlasTex) ||
       m_SkinningRemapAtlasWidth != remapWidth ||
       m_SkinningRemapAtlasHeight != remapHeight) {
      if (bgfx::isValid(m_SkinningRemapAtlasTex)) {
         bgfx::destroy(m_SkinningRemapAtlasTex);
      }
      m_SkinningRemapAtlasTex = bgfx::createTexture2D(
         remapWidth,
         remapHeight,
         false,
         1,
         bgfx::TextureFormat::R16,
         textureFlags);
      m_SkinningRemapAtlasWidth = remapWidth;
      m_SkinningRemapAtlasHeight = remapHeight;
      recreatedRemapAtlas = true;
   }

   bool recreatedInstanceAtlas = false;
   if (!bgfx::isValid(m_SkinningInstanceAtlasTex) ||
       m_SkinningInstanceAtlasWidth != instanceWidth ||
       m_SkinningInstanceAtlasHeight != instanceHeight) {
      if (bgfx::isValid(m_SkinningInstanceAtlasTex)) {
         bgfx::destroy(m_SkinningInstanceAtlasTex);
      }
      m_SkinningInstanceAtlasTex = bgfx::createTexture2D(
         instanceWidth,
         instanceHeight,
         false,
         1,
         bgfx::TextureFormat::RGBA32F,
         textureFlags);
      m_SkinningInstanceAtlasWidth = instanceWidth;
      m_SkinningInstanceAtlasHeight = instanceHeight;
      recreatedInstanceAtlas = true;
   }

   const bool useMorphAtlases = kAllowGpuMorphTargets && m_GpuMorphAtlasSupported && preparedMorphMeshes > 0;
   bool recreatedMorphVertexAtlas = false;
   bool recreatedMorphEntryAtlas = false;
   bool recreatedMorphActiveAtlas = false;
   if (useMorphAtlases) {
      if (!bgfx::isValid(m_MorphVertexAtlasTex) ||
          m_MorphVertexAtlasWidth != morphVertexWidth ||
          m_MorphVertexAtlasHeight != morphVertexHeight) {
         if (bgfx::isValid(m_MorphVertexAtlasTex)) {
            bgfx::destroy(m_MorphVertexAtlasTex);
         }
         m_MorphVertexAtlasTex = bgfx::createTexture2D(
            morphVertexWidth,
            morphVertexHeight,
            false,
            1,
            bgfx::TextureFormat::RGBA32F,
            textureFlags);
         m_MorphVertexAtlasWidth = morphVertexWidth;
         m_MorphVertexAtlasHeight = morphVertexHeight;
         recreatedMorphVertexAtlas = true;
      }

      if (!bgfx::isValid(m_MorphEntryAtlasTex) ||
          m_MorphEntryAtlasWidth != morphEntryWidth ||
          m_MorphEntryAtlasHeight != morphEntryHeight) {
         if (bgfx::isValid(m_MorphEntryAtlasTex)) {
            bgfx::destroy(m_MorphEntryAtlasTex);
         }
         m_MorphEntryAtlasTex = bgfx::createTexture2D(
            morphEntryWidth,
            morphEntryHeight,
            false,
            1,
            bgfx::TextureFormat::RGBA32F,
            textureFlags);
         m_MorphEntryAtlasWidth = morphEntryWidth;
         m_MorphEntryAtlasHeight = morphEntryHeight;
         recreatedMorphEntryAtlas = true;
      }

      if (!bgfx::isValid(m_MorphActiveAtlasTex) ||
          m_MorphActiveAtlasWidth != morphActiveWidth ||
          m_MorphActiveAtlasHeight != morphActiveHeight) {
         if (bgfx::isValid(m_MorphActiveAtlasTex)) {
            bgfx::destroy(m_MorphActiveAtlasTex);
         }
         m_MorphActiveAtlasTex = bgfx::createTexture2D(
            morphActiveWidth,
            morphActiveHeight,
            false,
            1,
            bgfx::TextureFormat::RGBA32F,
            textureFlags);
         m_MorphActiveAtlasWidth = morphActiveWidth;
         m_MorphActiveAtlasHeight = morphActiveHeight;
         recreatedMorphActiveAtlas = true;
      }
   } else {
      m_MorphVertexAtlasWidth = 0;
      m_MorphVertexAtlasHeight = 0;
      m_MorphEntryAtlasWidth = 0;
      m_MorphEntryAtlasHeight = 0;
      m_MorphActiveAtlasWidth = 0;
      m_MorphActiveAtlasHeight = 0;
   }

   if (!bgfx::isValid(m_SkinningBoneAtlasTex) ||
       !bgfx::isValid(m_SkinningRemapAtlasTex) ||
       !bgfx::isValid(m_SkinningInstanceAtlasTex) ||
       (useMorphAtlases &&
          (!bgfx::isValid(m_MorphVertexAtlasTex) ||
           !bgfx::isValid(m_MorphEntryAtlasTex) ||
           !bgfx::isValid(m_MorphActiveAtlasTex)))) {
      DestroyGpuSkinningResources();
      return false;
   }

   const bool boneAtlasContentsChanged =
      recreatedBoneAtlas ||
      boneAtlasFingerprint != m_SkinningBoneAtlasLastFingerprint ||
      boneTexelCount != m_SkinningAtlasLastBoneTexelCount;
   const bool remapAtlasContentsChanged =
      recreatedRemapAtlas ||
      remapAtlasFingerprint != m_SkinningRemapAtlasLastFingerprint ||
      remapTexelCount != m_SkinningAtlasLastRemapTexelCount;
   const bool instanceAtlasContentsChanged =
      recreatedInstanceAtlas ||
      instanceAtlasFingerprint != m_SkinningInstanceAtlasLastFingerprint ||
      instanceTexelCount != m_SkinningAtlasLastInstanceTexelCount;
   const bool morphVertexAtlasContentsChanged =
      useMorphAtlases &&
      (recreatedMorphVertexAtlas ||
       morphVertexAtlasFingerprint != m_MorphVertexAtlasLastFingerprint ||
       morphVertexTexelCount != m_MorphAtlasLastVertexTexelCount);
   const bool morphEntryAtlasContentsChanged =
      useMorphAtlases &&
      (recreatedMorphEntryAtlas ||
       morphEntryAtlasFingerprint != m_MorphEntryAtlasLastFingerprint ||
       morphEntryTexelCount != m_MorphAtlasLastEntryTexelCount);
   const bool morphActiveAtlasContentsChanged =
      useMorphAtlases &&
      (recreatedMorphActiveAtlas ||
       morphActiveAtlasFingerprint != m_MorphActiveAtlasLastFingerprint ||
       morphActiveTexelCount != m_MorphAtlasLastActiveTexelCount);
   if (boneAtlasContentsChanged) {
      const bgfx::Memory* boneMem = bgfx::copy(
         m_SkinningBoneAtlasCpu.data(),
         static_cast<uint32_t>(m_SkinningBoneAtlasCpu.size() * sizeof(glm::vec4)));
      bgfx::updateTexture2D(
         m_SkinningBoneAtlasTex,
         0,
         0,
         0,
         0,
         boneWidth,
         boneHeight,
         boneMem,
         static_cast<uint16_t>(boneWidth * sizeof(glm::vec4)));
   }

   if (remapAtlasContentsChanged) {
      const bgfx::Memory* remapMem = bgfx::copy(
         m_SkinningRemapAtlasCpu.data(),
         static_cast<uint32_t>(m_SkinningRemapAtlasCpu.size() * sizeof(uint16_t)));
      bgfx::updateTexture2D(
         m_SkinningRemapAtlasTex,
         0,
         0,
         0,
         0,
         remapWidth,
         remapHeight,
         remapMem,
         static_cast<uint16_t>(remapWidth * sizeof(uint16_t)));
   }

   if (instanceAtlasContentsChanged) {
      const bgfx::Memory* instanceMem = bgfx::copy(
         m_SkinningInstanceAtlasCpu.data(),
         static_cast<uint32_t>(m_SkinningInstanceAtlasCpu.size() * sizeof(glm::vec4)));
      bgfx::updateTexture2D(
         m_SkinningInstanceAtlasTex,
         0,
         0,
         0,
         0,
         instanceWidth,
         instanceHeight,
         instanceMem,
         static_cast<uint16_t>(instanceWidth * sizeof(glm::vec4)));
   }

   if (useMorphAtlases && morphVertexAtlasContentsChanged) {
      const bgfx::Memory* morphVertexMem = bgfx::copy(
         m_MorphVertexAtlasCpu.data(),
         static_cast<uint32_t>(m_MorphVertexAtlasCpu.size() * sizeof(glm::vec4)));
      bgfx::updateTexture2D(
         m_MorphVertexAtlasTex,
         0,
         0,
         0,
         0,
         morphVertexWidth,
         morphVertexHeight,
         morphVertexMem,
         static_cast<uint16_t>(morphVertexWidth * sizeof(glm::vec4)));
   }

   if (useMorphAtlases && morphEntryAtlasContentsChanged) {
      const bgfx::Memory* morphEntryMem = bgfx::copy(
         m_MorphEntryAtlasCpu.data(),
         static_cast<uint32_t>(m_MorphEntryAtlasCpu.size() * sizeof(glm::vec4)));
      bgfx::updateTexture2D(
         m_MorphEntryAtlasTex,
         0,
         0,
         0,
         0,
         morphEntryWidth,
         morphEntryHeight,
         morphEntryMem,
         static_cast<uint16_t>(morphEntryWidth * sizeof(glm::vec4)));
   }

   if (useMorphAtlases && morphActiveAtlasContentsChanged) {
      const bgfx::Memory* morphActiveMem = bgfx::copy(
         m_MorphActiveAtlasCpu.data(),
         static_cast<uint32_t>(m_MorphActiveAtlasCpu.size() * sizeof(glm::vec4)));
      bgfx::updateTexture2D(
         m_MorphActiveAtlasTex,
         0,
         0,
         0,
         0,
         morphActiveWidth,
         morphActiveHeight,
         morphActiveMem,
         static_cast<uint16_t>(morphActiveWidth * sizeof(glm::vec4)));
   }

   if (boneAtlasContentsChanged ||
       remapAtlasContentsChanged ||
       instanceAtlasContentsChanged ||
       morphVertexAtlasContentsChanged ||
       morphEntryAtlasContentsChanged ||
       morphActiveAtlasContentsChanged) {
      profiler.AddCounter(
         "Render/SkinningAtlasUploads",
         static_cast<uint64_t>(boneAtlasContentsChanged ? 1u : 0u) +
            static_cast<uint64_t>(remapAtlasContentsChanged ? 1u : 0u) +
            static_cast<uint64_t>(instanceAtlasContentsChanged ? 1u : 0u) +
            static_cast<uint64_t>(morphVertexAtlasContentsChanged ? 1u : 0u) +
            static_cast<uint64_t>(morphEntryAtlasContentsChanged ? 1u : 0u) +
            static_cast<uint64_t>(morphActiveAtlasContentsChanged ? 1u : 0u));
      profiler.AddCounter(
         "Render/SkinningAtlasUploadBytes",
         (boneAtlasContentsChanged
             ? static_cast<uint64_t>(m_SkinningBoneAtlasCpu.size() * sizeof(glm::vec4))
             : 0ull) +
            (remapAtlasContentsChanged
                ? static_cast<uint64_t>(m_SkinningRemapAtlasCpu.size() * sizeof(uint16_t))
                : 0ull) +
            (instanceAtlasContentsChanged
                ? static_cast<uint64_t>(m_SkinningInstanceAtlasCpu.size() * sizeof(glm::vec4))
                : 0ull) +
            (morphVertexAtlasContentsChanged
                ? static_cast<uint64_t>(m_MorphVertexAtlasCpu.size() * sizeof(glm::vec4))
                : 0ull) +
            (morphEntryAtlasContentsChanged
                ? static_cast<uint64_t>(m_MorphEntryAtlasCpu.size() * sizeof(glm::vec4))
                : 0ull) +
            (morphActiveAtlasContentsChanged
                ? static_cast<uint64_t>(m_MorphActiveAtlasCpu.size() * sizeof(glm::vec4))
                : 0ull));
      if (boneAtlasContentsChanged) {
         m_SkinningBoneAtlasLastFingerprint = boneAtlasFingerprint;
         m_SkinningAtlasLastBoneTexelCount = boneTexelCount;
      }
      if (remapAtlasContentsChanged) {
         m_SkinningRemapAtlasLastFingerprint = remapAtlasFingerprint;
         m_SkinningAtlasLastRemapTexelCount = remapTexelCount;
      }
      if (instanceAtlasContentsChanged) {
         m_SkinningInstanceAtlasLastFingerprint = instanceAtlasFingerprint;
         m_SkinningAtlasLastInstanceTexelCount = instanceTexelCount;
      }
      if (morphVertexAtlasContentsChanged) {
         m_MorphVertexAtlasLastFingerprint = morphVertexAtlasFingerprint;
         m_MorphAtlasLastVertexTexelCount = morphVertexTexelCount;
      }
      if (morphEntryAtlasContentsChanged) {
         m_MorphEntryAtlasLastFingerprint = morphEntryAtlasFingerprint;
         m_MorphAtlasLastEntryTexelCount = morphEntryTexelCount;
      }
      if (morphActiveAtlasContentsChanged) {
         m_MorphActiveAtlasLastFingerprint = morphActiveAtlasFingerprint;
         m_MorphAtlasLastActiveTexelCount = morphActiveTexelCount;
      }
      ++m_SkinningAtlasUploadSerial;
   }
   else {
      profiler.AddCounter("Render/SkinningAtlasUploadSkipped", 1);
   }

   profiler.SetCounter("Render/SkinningAtlasSkinnedMeshes", preparedSkinnedMeshes);
   profiler.SetCounter("Render/SkinningAtlasSharedSkeletonMeshes", preparedSharedSkeletonMeshes);
   profiler.SetCounter("Render/SkinningAtlasCpuPaletteMeshes", 0);
   profiler.SetCounter("Render/SkinningAtlasLegacyMeshes", 0);
   profiler.SetCounter("Render/SkinningAtlasCompactRemapMeshes", preparedCompactRemaps);
   profiler.SetCounter("Render/SkinningAtlasDynamicRemapMeshes", preparedDynamicRemaps);
   profiler.SetCounter("Render/SkinningAtlasCorrectionMeshes", preparedCorrectionMeshes);
   profiler.SetCounter("Render/SkinningAtlasInstanceRecords", preparedInstanceRecords);
   profiler.SetCounter("Render/SkinningAtlasMatrices", (boneTexelCount / 4u) > 0 ? (boneTexelCount / 4u) - 1u : 0u);
   profiler.SetCounter("Render/SkinningAtlasRemapEntries", remapTexelCount);
   profiler.SetCounter("Render/SkinningAtlasMorphMeshes", preparedMorphMeshes);
   profiler.SetCounter("Render/SkinningAtlasMorphActiveLists", preparedMorphActiveLists);
   profiler.SetCounter("Render/SkinningAtlasMorphActiveEntries", static_cast<uint64_t>(morphActiveTexelCount));
   profiler.SetCounter("Render/MorphGeometryCacheHits", morphGeometryCacheHits);
   profiler.SetCounter("Render/MorphGeometryCacheMisses", morphGeometryCacheMisses);

   m_GpuSkinningAtlasReady = true;
   return true;
}

void Renderer::BindGpuSkinningAtlasGlobals()
{
   if (!m_GpuSkinningAtlasReady ||
       !bgfx::isValid(m_SkinningBoneAtlasTex) ||
       !bgfx::isValid(m_SkinningRemapAtlasTex) ||
       !bgfx::isValid(m_SkinningInstanceAtlasTex)) {
      return;
   }

   const uint32_t samplerFlags =
      BGFX_SAMPLER_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
   bgfx::setTexture(kGpuSkinningBoneAtlasStage, s_BoneAtlas, m_SkinningBoneAtlasTex, samplerFlags);
   bgfx::setTexture(kGpuSkinningRemapAtlasStage, s_BoneRemapAtlas, m_SkinningRemapAtlasTex, samplerFlags);
   bgfx::setTexture(kGpuSkinningInstanceAtlasStage, s_SkinningInstanceAtlas, m_SkinningInstanceAtlasTex, samplerFlags);
   if (kAllowGpuMorphTargets &&
       m_GpuMorphAtlasSupported &&
       bgfx::isValid(m_MorphVertexAtlasTex) &&
       bgfx::isValid(m_MorphEntryAtlasTex) &&
       bgfx::isValid(m_MorphActiveAtlasTex)) {
      bgfx::setTexture(kGpuMorphVertexAtlasStage, s_MorphVertexAtlas, m_MorphVertexAtlasTex, samplerFlags);
      bgfx::setTexture(kGpuMorphEntryAtlasStage, s_MorphEntryAtlas, m_MorphEntryAtlasTex, samplerFlags);
      bgfx::setTexture(kGpuMorphActiveAtlasStage, s_MorphActiveAtlas, m_MorphActiveAtlasTex, samplerFlags);
   }

   const glm::vec4 boneInfo(
      static_cast<float>(m_SkinningBoneAtlasWidth),
      static_cast<float>(m_SkinningBoneAtlasHeight),
      (m_SkinningBoneAtlasWidth > 0) ? (1.0f / static_cast<float>(m_SkinningBoneAtlasWidth)) : 0.0f,
      (m_SkinningBoneAtlasHeight > 0) ? (1.0f / static_cast<float>(m_SkinningBoneAtlasHeight)) : 0.0f);
   const glm::vec4 remapInfo(
      static_cast<float>(m_SkinningRemapAtlasWidth),
      static_cast<float>(m_SkinningRemapAtlasHeight),
      (m_SkinningRemapAtlasWidth > 0) ? (1.0f / static_cast<float>(m_SkinningRemapAtlasWidth)) : 0.0f,
      (m_SkinningRemapAtlasHeight > 0) ? (1.0f / static_cast<float>(m_SkinningRemapAtlasHeight)) : 0.0f);
   const glm::vec4 instanceInfo(
      static_cast<float>(m_SkinningInstanceAtlasWidth),
      static_cast<float>(m_SkinningInstanceAtlasHeight),
      (m_SkinningInstanceAtlasWidth > 0) ? (1.0f / static_cast<float>(m_SkinningInstanceAtlasWidth)) : 0.0f,
      (m_SkinningInstanceAtlasHeight > 0) ? (1.0f / static_cast<float>(m_SkinningInstanceAtlasHeight)) : 0.0f);
   const glm::vec4 morphVertexInfo(
      static_cast<float>(m_MorphVertexAtlasWidth),
      static_cast<float>(m_MorphVertexAtlasHeight),
      (m_MorphVertexAtlasWidth > 0) ? (1.0f / static_cast<float>(m_MorphVertexAtlasWidth)) : 0.0f,
      (m_MorphVertexAtlasHeight > 0) ? (1.0f / static_cast<float>(m_MorphVertexAtlasHeight)) : 0.0f);
   const glm::vec4 morphEntryInfo(
      static_cast<float>(m_MorphEntryAtlasWidth),
      static_cast<float>(m_MorphEntryAtlasHeight),
      (m_MorphEntryAtlasWidth > 0) ? (1.0f / static_cast<float>(m_MorphEntryAtlasWidth)) : 0.0f,
      (m_MorphEntryAtlasHeight > 0) ? (1.0f / static_cast<float>(m_MorphEntryAtlasHeight)) : 0.0f);
   const glm::vec4 morphActiveInfo(
      static_cast<float>(m_MorphActiveAtlasWidth),
      static_cast<float>(m_MorphActiveAtlasHeight),
      (m_MorphActiveAtlasWidth > 0) ? (1.0f / static_cast<float>(m_MorphActiveAtlasWidth)) : 0.0f,
      (m_MorphActiveAtlasHeight > 0) ? (1.0f / static_cast<float>(m_MorphActiveAtlasHeight)) : 0.0f);
   bgfx::setUniform(u_SkinningBoneAtlasInfo, &boneInfo);
   bgfx::setUniform(u_SkinningRemapAtlasInfo, &remapInfo);
   bgfx::setUniform(u_SkinningInstanceAtlasInfo, &instanceInfo);
   bgfx::setUniform(u_MorphVertexAtlasInfo, &morphVertexInfo);
   bgfx::setUniform(u_MorphEntryAtlasInfo, &morphEntryInfo);
   bgfx::setUniform(u_MorphActiveAtlasInfo, &morphActiveInfo);
}

void Renderer::BindSkinningInstanceRecord(uint32_t recordIndex)
{
   BindGpuSkinningAtlasGlobals();
   if (recordIndex >= m_SkinningInstanceRecords.size()) {
      const glm::vec4 zero(0.0f);
      const glm::vec4 invalidRecord(-1.0f, 0.0f, 0.0f, 0.0f);
      const glm::mat4 identity(1.0f);
      bgfx::setUniform(u_SkinningInstanceRecord, &invalidRecord);
      bgfx::setUniform(u_SkinningParams, &zero);
      bgfx::setUniform(u_SkinningExtra, &zero);
      bgfx::setUniform(u_SkinningMeshFromSkeleton, glm::value_ptr(identity));
      bgfx::setUniform(u_SkinningMorphParams, &zero);
      return;
   }

   const auto& record = m_SkinningInstanceRecords[recordIndex];
   const glm::vec4 recordUniform(static_cast<float>(recordIndex), 0.0f, 0.0f, 0.0f);
   bgfx::setUniform(u_SkinningInstanceRecord, &recordUniform);
   bgfx::setUniform(u_SkinningParams, &record.Params);
   bgfx::setUniform(u_SkinningExtra, &record.Extra);
   bgfx::setUniform(u_SkinningMeshFromSkeleton, glm::value_ptr(record.MeshFromSkeleton));
   bgfx::setUniform(u_SkinningMorphParams, &record.Morph);
}

glm::vec4 Renderer::GetSkinningInstanceObjectIdPacked(uint32_t recordIndex) const
{
   if (recordIndex >= m_SkinningInstanceRecords.size()) {
      return glm::vec4(0.0f);
   }

   return m_SkinningInstanceRecords[recordIndex].ObjectIdPacked;
}

void Renderer::BindSkinningIfChanged(const SkinningComponent* skinning, Renderer::SkinningBindCacheState& cache)
{
   if (!skinning) {
      return;
   }

   if (!m_GpuSkinningAtlasReady ||
       !bgfx::isValid(m_SkinningBoneAtlasTex) ||
       !bgfx::isValid(m_SkinningRemapAtlasTex) ||
       !bgfx::isValid(m_SkinningInstanceAtlasTex)) {
      return;
   }

   // bgfx consumes draw bindings on submit, so the atlas textures and metadata
   // still need to be rebound for every skinned draw even when the atlas content
   // itself has not changed this frame.
   BindGpuSkinningAtlasGlobals();
   cache.atlasGlobalsBound = true;
   cache.atlasUploadSerial = m_SkinningAtlasUploadSerial;

   const bool hasBinding = skinning->HasGpuSkinningBinding();
   const uint32_t sourceCount =
      hasBinding
         ? (skinning->GpuRemapAtlasCount > 0
               ? skinning->GpuRemapAtlasCount
               : (skinning->GpuCorrectionAtlasCount > 0
                     ? skinning->GpuCorrectionAtlasCount
                     : skinning->BoneCount))
         : 0u;
   const glm::vec4 params(
      static_cast<float>(hasBinding ? skinning->GpuBoneAtlasBase : 0u),
      static_cast<float>(hasBinding ? skinning->GpuRemapAtlasBase : 0u),
      (hasBinding && skinning->GpuRemapAtlasCount > 0) ? 1.0f : 0.0f,
      static_cast<float>(hasBinding ? std::max<int32_t>(static_cast<int32_t>(skinning->GpuBoneAtlasCount) - 1, 0) : 0));
   const glm::vec4 extra(
      static_cast<float>(hasBinding ? skinning->GpuCorrectionAtlasBase : 0u),
      (hasBinding && skinning->GpuCorrectionAtlasCount > 0) ? 1.0f : 0.0f,
      static_cast<float>(hasBinding ? std::max<int32_t>(static_cast<int32_t>(sourceCount) - 1, 0) : 0),
      0.0f);
   const glm::mat4 meshFromSkeleton =
      (hasBinding && skinning->UsesGpuSharedSkeleton())
         ? skinning->GpuMeshFromSkeleton
         : glm::mat4(1.0f);
   bgfx::setUniform(u_SkinningParams, &params);
   bgfx::setUniform(u_SkinningExtra, &extra);
   bgfx::setUniform(u_SkinningMeshFromSkeleton, glm::value_ptr(meshFromSkeleton));

   cache.skinning = skinning;
}

void Renderer::SetShadowReceive(float receive) {
   float clamped = glm::clamp(receive, 0.0f, 1.0f);
   glm::vec4 v(clamped, 0.0f, 0.0f, 0.0f);
   bgfx::setUniform(u_ShadowReceive, &v);
}


Renderer& Renderer::Get() {
   static Renderer instance;
   return instance;
}

Renderer::~Renderer() {
   m_ChunkSystem.reset();  // Reset before other members - requires complete type (TerrainChunks.h included)
   m_TextRenderer.reset();
   }


void Renderer::Shutdown() {
   TerrainGrass::ShutdownRendererResources();
   ReleaseAllOffscreenTargets();
   for (auto& [key, entry] : m_SkinnedOcclusionQueries) {
      (void)key;
      if (bgfx::isValid(entry.Handle)) {
         bgfx::destroy(entry.Handle);
         entry.Handle = BGFX_INVALID_HANDLE;
      }
   }
   m_SkinnedOcclusionQueries.clear();
   m_SkinnedOcclusionFrame = 0;
   if (bgfx::isValid(m_DebugLineProgram)) bgfx::destroy(m_DebugLineProgram);
   if (bgfx::isValid(u_DebugColor)) { bgfx::destroy(u_DebugColor); u_DebugColor = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(m_TerrainProgram)) { bgfx::destroy(m_TerrainProgram); m_TerrainProgram = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(m_TerrainDepthProgram)) { bgfx::destroy(m_TerrainDepthProgram); m_TerrainDepthProgram = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(s_TerrainHeightTexture)) { bgfx::destroy(s_TerrainHeightTexture); s_TerrainHeightTexture = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(s_TerrainSplatTexture)) { bgfx::destroy(s_TerrainSplatTexture); s_TerrainSplatTexture = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(s_TerrainSplatTexture2)) { bgfx::destroy(s_TerrainSplatTexture2); s_TerrainSplatTexture2 = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(s_TerrainHoleTexture)) { bgfx::destroy(s_TerrainHoleTexture); s_TerrainHoleTexture = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(s_TerrainAlbedoArray)) { bgfx::destroy(s_TerrainAlbedoArray); s_TerrainAlbedoArray = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(s_TerrainNormalArray)) { bgfx::destroy(s_TerrainNormalArray); s_TerrainNormalArray = BGFX_INVALID_HANDLE; }
   DestroyTerrainTextureArrays();
   if (bgfx::isValid(u_TerrainChunkParams)) { bgfx::destroy(u_TerrainChunkParams); u_TerrainChunkParams = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_TerrainHeightParams)) { bgfx::destroy(u_TerrainHeightParams); u_TerrainHeightParams = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_TerrainTexelSize)) { bgfx::destroy(u_TerrainTexelSize); u_TerrainTexelSize = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_TerrainLayerTiling)) { bgfx::destroy(u_TerrainLayerTiling); u_TerrainLayerTiling = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_TerrainLayerColor)) { bgfx::destroy(u_TerrainLayerColor); u_TerrainLayerColor = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_TerrainMaterial)) { bgfx::destroy(u_TerrainMaterial); u_TerrainMaterial = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_TerrainLayerCount)) { bgfx::destroy(u_TerrainLayerCount); u_TerrainLayerCount = BGFX_INVALID_HANDLE; }
   m_TerrainFallbackAlbedo = BGFX_INVALID_HANDLE;
   if (bgfx::isValid(m_TerrainFallbackNormal)) { bgfx::destroy(m_TerrainFallbackNormal); m_TerrainFallbackNormal = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(m_TerrainFallbackHole)) { bgfx::destroy(m_TerrainFallbackHole); m_TerrainFallbackHole = BGFX_INVALID_HANDLE; }
   // Clipmap resources
   if (bgfx::isValid(m_ClipmapProgram)) { bgfx::destroy(m_ClipmapProgram); m_ClipmapProgram = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(m_ClipmapDepthProgram)) { bgfx::destroy(m_ClipmapDepthProgram); m_ClipmapDepthProgram = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_ClipmapParams)) { bgfx::destroy(u_ClipmapParams); u_ClipmapParams = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_ClipmapOffset)) { bgfx::destroy(u_ClipmapOffset); u_ClipmapOffset = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_SkyParams)) { bgfx::destroy(u_SkyParams); u_SkyParams = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_SkyTopColor)) { bgfx::destroy(u_SkyTopColor); u_SkyTopColor = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_SkyHorizonColor)) { bgfx::destroy(u_SkyHorizonColor); u_SkyHorizonColor = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_GroundColor)) { bgfx::destroy(u_GroundColor); u_GroundColor = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_SunDirection)) { bgfx::destroy(u_SunDirection); u_SunDirection = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_SkySunParams)) { bgfx::destroy(u_SkySunParams); u_SkySunParams = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_SkyAtmosphereParams)) { bgfx::destroy(u_SkyAtmosphereParams); u_SkyAtmosphereParams = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_SceneColorGrade)) { bgfx::destroy(u_SceneColorGrade); u_SceneColorGrade = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(s_Skybox)) { bgfx::destroy(s_Skybox); s_Skybox = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_ShadowReceive)) { bgfx::destroy(u_ShadowReceive); u_ShadowReceive = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(s_PointShadowMap)) { bgfx::destroy(s_PointShadowMap); s_PointShadowMap = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_PointShadowMeta)) { bgfx::destroy(u_PointShadowMeta); u_PointShadowMeta = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_PointShadowLightPos)) { bgfx::destroy(u_PointShadowLightPos); u_PointShadowLightPos = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_PointShadowAtlas)) { bgfx::destroy(u_PointShadowAtlas); u_PointShadowAtlas = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(s_ShadowDebug)) { bgfx::destroy(s_ShadowDebug); s_ShadowDebug = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_ShadowDebugParams)) { bgfx::destroy(u_ShadowDebugParams); u_ShadowDebugParams = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(m_ShadowDebugProgram)) { bgfx::destroy(m_ShadowDebugProgram); m_ShadowDebugProgram = BGFX_INVALID_HANDLE; }
   DestroyGpuSkinningResources();
   if (bgfx::isValid(s_BoneAtlas)) { bgfx::destroy(s_BoneAtlas); s_BoneAtlas = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(s_BoneRemapAtlas)) { bgfx::destroy(s_BoneRemapAtlas); s_BoneRemapAtlas = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(s_SkinningInstanceAtlas)) { bgfx::destroy(s_SkinningInstanceAtlas); s_SkinningInstanceAtlas = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(s_MorphVertexAtlas)) { bgfx::destroy(s_MorphVertexAtlas); s_MorphVertexAtlas = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(s_MorphEntryAtlas)) { bgfx::destroy(s_MorphEntryAtlas); s_MorphEntryAtlas = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(s_MorphActiveAtlas)) { bgfx::destroy(s_MorphActiveAtlas); s_MorphActiveAtlas = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_SkinningBoneAtlasInfo)) { bgfx::destroy(u_SkinningBoneAtlasInfo); u_SkinningBoneAtlasInfo = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_SkinningRemapAtlasInfo)) { bgfx::destroy(u_SkinningRemapAtlasInfo); u_SkinningRemapAtlasInfo = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_SkinningInstanceAtlasInfo)) { bgfx::destroy(u_SkinningInstanceAtlasInfo); u_SkinningInstanceAtlasInfo = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_MorphVertexAtlasInfo)) { bgfx::destroy(u_MorphVertexAtlasInfo); u_MorphVertexAtlasInfo = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_MorphEntryAtlasInfo)) { bgfx::destroy(u_MorphEntryAtlasInfo); u_MorphEntryAtlasInfo = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_MorphActiveAtlasInfo)) { bgfx::destroy(u_MorphActiveAtlasInfo); u_MorphActiveAtlasInfo = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_SkinningParams)) { bgfx::destroy(u_SkinningParams); u_SkinningParams = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_SkinningExtra)) { bgfx::destroy(u_SkinningExtra); u_SkinningExtra = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_SkinningInstanceRecord)) { bgfx::destroy(u_SkinningInstanceRecord); u_SkinningInstanceRecord = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_SkinningMeshFromSkeleton)) { bgfx::destroy(u_SkinningMeshFromSkeleton); u_SkinningMeshFromSkeleton = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_SkinningMorphParams)) { bgfx::destroy(u_SkinningMorphParams); u_SkinningMorphParams = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(u_GpuMaterializedSkinningDispatch)) { bgfx::destroy(u_GpuMaterializedSkinningDispatch); u_GpuMaterializedSkinningDispatch = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(m_GpuMaterializedSkinningProgram)) { bgfx::destroy(m_GpuMaterializedSkinningProgram); m_GpuMaterializedSkinningProgram = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(m_ObjectIdFB)) { bgfx::destroy(m_ObjectIdFB); m_ObjectIdFB = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(m_ObjectIdTex)) { bgfx::destroy(m_ObjectIdTex); m_ObjectIdTex = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(m_ObjectIdReadbackTex)) { bgfx::destroy(m_ObjectIdReadbackTex); m_ObjectIdReadbackTex = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(m_EdgeMaskFB)) { bgfx::destroy(m_EdgeMaskFB); m_EdgeMaskFB = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(m_EdgeMaskTex)) { bgfx::destroy(m_EdgeMaskTex); m_EdgeMaskTex = BGFX_INVALID_HANDLE; }
   DestroyFullscreenTriangle();
   ShutdownShadowResources();
   
   // IMPORTANT: Destroy TextRenderer BEFORE bgfx::shutdown() to avoid use-after-free
   // TextRenderer destructor calls bgfx::destroy on atlas texture and sampler handles
   m_TextRenderer.reset();
#ifndef CLAYMORE_RUNTIME
   m_CachedDebugMaterial.reset();
#endif
   
   cm::rendering::SetBgfxActive(false);
   bgfx::shutdown();
   }

// ---------------- Frame Lifecycle ----------------
void Renderer::BeginFrame(float r, float g, float b) {
   ++m_SkinningAtlasFrameSerial;
   m_SkinningAtlasPreparedFrameSerial = 0;
   m_SkinningAtlasPreparedScene = nullptr;
   m_SkinningAtlasUnionEntities.clear();
   m_SkinningAtlasCoveredEntities.clear();

   // Debug View (0)
   bgfx::setViewRect(0, 0, 0, m_Width, m_Height);
   bgfx::setViewTransform(0, m_view, m_proj);
   if (m_RenderToOffscreen) {
      bgfx::setViewFrameBuffer(0, m_SceneFrameBuffer);
      }
   else {
      bgfx::setViewFrameBuffer(0, BGFX_INVALID_HANDLE);
      }
   bgfx::touch(0);

   // Mesh view (1)
   bgfx::setViewRect(1, 0, 0, m_Width, m_Height);
   bgfx::setViewTransform(1, m_view, m_proj);
   if (m_RenderToOffscreen) {
      bgfx::setViewFrameBuffer(1, m_SceneFrameBuffer);
      }
   else {
      bgfx::setViewFrameBuffer(1, BGFX_INVALID_HANDLE);
      }
   bgfx::touch(1);

   // World-space UI view (2) shares the main scene depth and renders after meshes.
   bgfx::setViewRect(kMainWorldUIViewId, 0, 0, m_Width, m_Height);
   if (m_RenderToOffscreen) {
      bgfx::setViewFrameBuffer(kMainWorldUIViewId, m_SceneFrameBuffer);
      }
   else {
      bgfx::setViewFrameBuffer(kMainWorldUIViewId, BGFX_INVALID_HANDLE);
      }
   bgfx::setViewClear(kMainWorldUIViewId, BGFX_CLEAR_NONE, 0, 1.0f, 0);
   bgfx::setViewTransform(kMainWorldUIViewId, m_view, m_proj);
   bgfx::setViewMode(kMainWorldUIViewId, bgfx::ViewMode::Sequential);
   bgfx::touch(kMainWorldUIViewId);

   // Screen-space UI/Text view (3) stays on top of world-space UI.
   bgfx::setViewRect(kMainScreenUIViewId, 0, 0, m_Width, m_Height);
   if (m_RenderToOffscreen) {
      bgfx::setViewFrameBuffer(kMainScreenUIViewId, m_SceneFrameBuffer);
      }
   else {
      bgfx::setViewFrameBuffer(kMainScreenUIViewId, BGFX_INVALID_HANDLE);
      }
   bgfx::setViewClear(kMainScreenUIViewId, BGFX_CLEAR_NONE, 0, 1.0f, 0);
   bgfx::setViewMode(kMainScreenUIViewId, bgfx::ViewMode::Sequential);
   bgfx::touch(kMainScreenUIViewId);

   // Debug overlay view renders gizmos after main geometry
   bgfx::setViewRect(kDebugOverlayViewId, 0, 0, m_Width, m_Height);
   bgfx::setViewTransform(kDebugOverlayViewId, m_view, m_proj);
   if (m_RenderToOffscreen) {
      bgfx::setViewFrameBuffer(kDebugOverlayViewId, m_SceneFrameBuffer);
      }
   else {
      bgfx::setViewFrameBuffer(kDebugOverlayViewId, BGFX_INVALID_HANDLE);
      }
   bgfx::touch(kDebugOverlayViewId);

   ApplyDefaultDebugLineColor();
   }


void Renderer::EndFrame() {
   const auto submitStart = std::chrono::high_resolution_clock::now();
   m_LastSubmittedFrame = bgfx::frame();
   const auto submitEnd = std::chrono::high_resolution_clock::now();
   Profiler::Get().Record(
      "Renderer/bgfx::frame",
      std::chrono::duration<double, std::milli>(submitEnd - submitStart).count());

   if (const bgfx::Stats* stats = bgfx::getStats()) {
      const double cpuFreq = stats->cpuTimerFreq > 0
         ? static_cast<double>(stats->cpuTimerFreq)
         : 0.0;
      const double gpuFreq = stats->gpuTimerFreq > 0
         ? static_cast<double>(stats->gpuTimerFreq)
         : 0.0;
      if (cpuFreq > 0.0) {
         Profiler::Get().Record(
            "BGFX/CPUFrame",
            static_cast<double>(stats->cpuTimeFrame) * 1000.0 / cpuFreq);
         Profiler::Get().Record(
            "BGFX/WaitRender",
            static_cast<double>(stats->waitRender) * 1000.0 / cpuFreq);
         Profiler::Get().Record(
            "BGFX/WaitSubmit",
            static_cast<double>(stats->waitSubmit) * 1000.0 / cpuFreq);
      }
      if (gpuFreq > 0.0 && stats->gpuTimeEnd > stats->gpuTimeBegin) {
         Profiler::Get().Record(
            "BGFX/GPUFrame",
            static_cast<double>(stats->gpuTimeEnd - stats->gpuTimeBegin) * 1000.0 / gpuFreq);
      }
      Profiler::Get().SetCounter("BGFX/DrawCalls", stats->numDraw);
      Profiler::Get().SetCounter("BGFX/ComputeCalls", stats->numCompute);
      Profiler::Get().SetCounter("BGFX/BlitCalls", stats->numBlit);
      Profiler::Get().SetCounter("BGFX/OcclusionQueries", stats->numOcclusionQueries);
   }
}

void Renderer::Resize(uint32_t width, uint32_t height) {
   m_Width = width;
   m_Height = height;

   m_RendererCamera->SetViewportSize((float)width, (float)height);

   // CRITICAL FIX: Destroy all framebuffers that reference m_SceneDepthTexture BEFORE destroying the depth texture
   // This prevents GPU memory leaks and dangling references
   
   // Destroy framebuffers that share the depth texture first
   if (bgfx::isValid(m_VisMaskFB)) { bgfx::destroy(m_VisMaskFB); m_VisMaskFB = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(m_ObjectIdFB)) { bgfx::destroy(m_ObjectIdFB); m_ObjectIdFB = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(m_SceneFrameBuffer)) { bgfx::destroy(m_SceneFrameBuffer); m_SceneFrameBuffer = BGFX_INVALID_HANDLE; }
   
   // Now safe to destroy textures
   if (bgfx::isValid(m_SceneTexture)) { bgfx::destroy(m_SceneTexture); m_SceneTexture = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(m_SceneDepthTexture)) { bgfx::destroy(m_SceneDepthTexture); m_SceneDepthTexture = BGFX_INVALID_HANDLE; }
   
   // Recreate scene textures and framebuffer
   const uint64_t texFlags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
   m_SceneTexture = bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::BGRA8, texFlags);
   m_SceneDepthTexture = bgfx::createTexture2D(width, height, false, 1, m_SceneDepthFormat, BGFX_TEXTURE_RT_WRITE_ONLY);
   {
      bgfx::TextureHandle fbTex[] = { m_SceneTexture, m_SceneDepthTexture };
      m_SceneFrameBuffer = bgfx::createFrameBuffer(2, fbTex, true);
   }
   bgfx::setViewFrameBuffer(0, m_SceneFrameBuffer);

   // Recreate selection mask RTs (destroy textures first, then framebuffers)
   if (bgfx::isValid(m_OccMaskFB)) { bgfx::destroy(m_OccMaskFB); m_OccMaskFB = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(m_VisMaskTex)) { bgfx::destroy(m_VisMaskTex); m_VisMaskTex = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(m_OccMaskTex)) { bgfx::destroy(m_OccMaskTex); m_OccMaskTex = BGFX_INVALID_HANDLE; }
   const uint64_t rFlags2 = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
   m_VisMaskTex = bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::BGRA8, rFlags2);
   m_OccMaskTex = bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::BGRA8, rFlags2);
   {
      bgfx::TextureHandle visAttachments[] = { m_VisMaskTex, m_SceneDepthTexture };
      m_VisMaskFB = bgfx::createFrameBuffer(2, visAttachments, false);
   }
   m_OccMaskFB = bgfx::createFrameBuffer(1, &m_OccMaskTex, true);

   // Shadow map resources are independent of viewport size; keep current resolution.

   // Recreate ObjectID and Edge mask RTs
   if (bgfx::isValid(m_EdgeMaskFB)) { bgfx::destroy(m_EdgeMaskFB); m_EdgeMaskFB = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(m_ObjectIdTex)) { bgfx::destroy(m_ObjectIdTex); m_ObjectIdTex = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(m_ObjectIdReadbackTex)) { bgfx::destroy(m_ObjectIdReadbackTex); m_ObjectIdReadbackTex = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(m_EdgeMaskTex)) { bgfx::destroy(m_EdgeMaskTex); m_EdgeMaskTex = BGFX_INVALID_HANDLE; }
   {
      const uint64_t idFlags = BGFX_TEXTURE_RT | BGFX_SAMPLER_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
      m_ObjectIdTex = bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::RGBA8, idFlags);
      bgfx::TextureHandle idAttachments[] = { m_ObjectIdTex, m_SceneDepthTexture };
      m_ObjectIdFB = bgfx::createFrameBuffer(2, idAttachments, false);
   }
   if (bgfx::getCaps() &&
       (bgfx::getCaps()->supported & BGFX_CAPS_TEXTURE_BLIT) != 0 &&
       (bgfx::getCaps()->supported & BGFX_CAPS_TEXTURE_READ_BACK) != 0) {
      const uint64_t readbackFlags = BGFX_TEXTURE_READ_BACK | BGFX_TEXTURE_BLIT_DST | BGFX_SAMPLER_POINT;
      m_ObjectIdReadbackTex = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8, readbackFlags);
   }
   {
      const uint64_t edgeFlags = BGFX_TEXTURE_RT | BGFX_SAMPLER_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
      m_EdgeMaskTex = bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::R8, edgeFlags);
      m_EdgeMaskFB = bgfx::createFrameBuffer(1, &m_EdgeMaskTex, true);
   }

   m_ObjectIdPickPending = false;
   m_ObjectIdPickPendingFrame = 0;
   m_ObjectIdPickResultReady = false;
   m_ObjectIdPickResultHadHit = false;
   m_ObjectIdPickResult = INVALID_ENTITY_ID;
}

// ---------------- Scene Rendering ----------------
void Renderer::RenderScene(Scene& scene) {
   static uint64_t s_RenderSceneFrameCounter = 0;
   static bool s_LastPlayModeState = false;
   static bool s_PlayModeStateInitialized = false;
   static uint64_t s_LastSceneRevisionForBindCache = 0;
   static bool s_SceneRevisionInitialized = false;
   static uint64_t s_LastRenderBindingVersionForBindCache = 0;
   static bool s_RenderBindingVersionInitialized = false;
   static uint32_t s_BindCacheCooldownFrames = 0;
   ++s_RenderSceneFrameCounter;
   const uint64_t sceneRevisionForBindCache = scene.GetDirtyRevision();
   const cm::world::RuntimeWorld* bindCacheRuntimeWorld = scene.GetRuntimeWorld();
   const uint64_t renderBindingVersionForBindCache =
      bindCacheRuntimeWorld ? bindCacheRuntimeWorld->GetStats().RenderBindingVersion : 0;
   const bool playModeTransitioned = s_PlayModeStateInitialized && (scene.m_IsPlaying != s_LastPlayModeState);
   const bool sceneRevisionChangedForBindCache =
      s_SceneRevisionInitialized && (sceneRevisionForBindCache != s_LastSceneRevisionForBindCache);
   const bool renderBindingChangedForBindCache =
      s_RenderBindingVersionInitialized &&
      (renderBindingVersionForBindCache != s_LastRenderBindingVersionForBindCache);
   // Guard against stale GPU/material state after play-mode toggles and render-binding churn.
   // During cooldown, force full material binding each draw.
   if (playModeTransitioned) {
      s_BindCacheCooldownFrames = std::max<uint32_t>(s_BindCacheCooldownFrames, 180u);
   }
   if ((!scene.m_IsPlaying && sceneRevisionChangedForBindCache) ||
       (scene.m_IsPlaying && renderBindingChangedForBindCache)) {
      s_BindCacheCooldownFrames = std::max<uint32_t>(s_BindCacheCooldownFrames, 2u);
   }
   const bool inBindCacheCooldown = (s_BindCacheCooldownFrames > 0);
   if (s_BindCacheCooldownFrames > 0) {
      --s_BindCacheCooldownFrames;
   }
   const bool enableMaterialBindCache = (s_RenderSceneFrameCounter > 120) && !inBindCacheCooldown;
   s_LastPlayModeState = scene.m_IsPlaying;
   s_PlayModeStateInitialized = true;
   s_LastSceneRevisionForBindCache = sceneRevisionForBindCache;
   s_SceneRevisionInitialized = true;
   s_LastRenderBindingVersionForBindCache = renderBindingVersionForBindCache;
   s_RenderBindingVersionInitialized = true;
   ++m_TerrainChunkPrepToken;
   Profiler::Get().SetCounter("Render/EntityScans", 0);
   Profiler::Get().SetCounter("Render/TerrainPrepRuns", 0);
   Profiler::Get().SetCounter("Render/TerrainPrepSkips", 0);
   Profiler::Get().SetCounter("Render/TerrainVisibleChunks", 0);
   Profiler::Get().SetCounter("Render/ShadowCastersScanned", 0);
   Profiler::Get().SetCounter("Render/ShadowCascadeCulled", 0);
   Profiler::Get().SetCounter("Render/ShadowSkinnedCascadeSkipped", 0);
   Profiler::Get().SetCounter("Render/ShadowSkinnedOffscreenSkipped", 0);
   Profiler::Get().SetCounter("Render/ShadowMainViewCulled", 0);
   Profiler::Get().SetCounter("Render/ShadowCascadeSubmits", 0);
   Profiler::Get().SetCounter("Render/BindCacheCooldownFrames", s_BindCacheCooldownFrames);
   Profiler::Get().SetCounter("Render/MaterialBindCacheEnabled", enableMaterialBindCache ? 1u : 0u);
   Profiler::Get().SetCounter("Render/SkinningAtlasPrepCalls", 0);
   Profiler::Get().SetCounter("Render/SkinningAtlasUploads", 0);
   Profiler::Get().SetCounter("Render/SkinningAtlasUploadBytes", 0);
   Profiler::Get().SetCounter("Render/SkinningAtlasUploadSkipped", 0);
   if (m_RuntimeStatsCaptureEnabled) {
      m_LastRuntimeStatsFrame = {};
   }

   // Prepare camera matrices. Prefer the scene's active camera only while playing.
   Camera* renderCamera = scene.m_IsPlaying ? scene.GetActiveCamera() : nullptr;
   if (!renderCamera) renderCamera = m_RendererCamera;
   if (!renderCamera) {
      std::cerr << "[Renderer] RenderScene called with null camera!" << std::endl;
      return;
   }
   glm::mat4 view = renderCamera->GetViewMatrix();
   glm::mat4 proj = renderCamera->GetProjectionMatrix();
   memcpy(m_view, glm::value_ptr(view), sizeof(float) * 16);
   memcpy(m_proj, glm::value_ptr(proj), sizeof(float) * 16);

   bgfx::setViewTransform(0, m_view, m_proj);
   bgfx::setViewTransform(1, m_view, m_proj);
   // TODO: SKY_AUDIT: bgfx derives u_invViewProj (used by fs_sky) from these view/proj matrices.
   
   // Also update the preview view (210) when used by offscreen renders
   bgfx::setViewTransform(210, m_view, m_proj);
   
   // Ensure views are touched so other view state changes don't clear them unexpectedly
   bgfx::touch(0);
   bgfx::touch(1);
   bgfx::touch(210);
   bgfx::touch(2);

   glm::vec4 camPos(renderCamera->GetPosition(), 1.0f);
   glm::vec3 cameraPosition = glm::vec3(camPos);
   bgfx::setUniform(u_cameraPos, &camPos); // TODO: SKY_AUDIT: camera position uniform consumed by fs_sky ray reconstruction.

   uint32_t activeLayerMask = 0xFFFFFFFFu;
   bool enforceLayerMask = false;
   if (scene.m_IsPlaying) {
      for (const auto& eCam : scene.GetEntities()) {
         auto* dCam = scene.GetEntityData(eCam.GetID());
         if (dCam && dCam->Camera && &dCam->Camera->Camera == renderCamera) {
            activeLayerMask = dCam->Camera->LayerMask;
            enforceLayerMask = true;
            break;
         }
      }
   }

  const bool editorLightingOverride = ShouldApplyEditorLightingOverride(scene);

  // Upload environment and cache for external systems
  m_CachedEnvironment = scene.GetEnvironment();
  m_CachedEditorLightingOverride = editorLightingOverride;
  UploadEnvironmentToShader(m_CachedEnvironment, m_CachedEditorLightingOverride);

   // Shadows: ensure resources and render shadow map if enabled
   const Environment& envSh = scene.GetEnvironment();
   Scene* prevShadowScene = m_ShadowContextScene;
   bool prevShadowEnabled = m_ShadowContextEnabled;
   m_ShadowContextScene = &scene;
   m_ShadowContextEnabled = (!editorLightingOverride && envSh.ShadowsEnabled);
  bool renderShadowMapThisFrame = false;
  if (!editorLightingOverride && envSh.ShadowsEnabled) {
      if (m_ShadowRes != (uint32_t)envSh.ShadowMapResolution || !bgfx::isValid(m_ShadowFB)) {
         InitShadowResources((uint32_t)envSh.ShadowMapResolution);
      }
      if (bgfx::isValid(m_ShadowFB)) {
         renderShadowMapThisFrame = true;
      }
   }
   // Do not override editor selection thickness/color here; global scene outline uses its own uniforms during its pass

  // --------------------------------------
  // Procedural Sky Pass
  // - fs_sky rebuilds camera rays via u_invViewProj/u_cameraPos and evaluates gradients analytically.
  // - Environment::SkyTop/Horizon/Ground/Sun/Exposure directly drive the fullscreen shader.
  // --------------------------------------
   {
   ScopedTimer t("Render/Sky");
   const Environment& envForSky = scene.GetEnvironment();
   glm::vec3 sunDir = ComputePrimarySunDirection(scene);
  glm::vec4 sunDirUniform(sunDir, 0.0f);
  bgfx::setUniform(u_SunDirection, &sunDirUniform);
   if (envForSky.ProceduralSky && bgfx::isValid(m_FullscreenVB) && bgfx::isValid(m_FullscreenIB)) {
      glm::vec4 skyParams(1.0f, 0.0f, 1.0f, 0.0f);
      bgfx::setUniform(u_SkyParams, &skyParams);

      float id[16]; bx::mtxIdentity(id);
      bgfx::setTransform(id);
      bgfx::setVertexBuffer(0, m_FullscreenVB);
      bgfx::setIndexBuffer(m_FullscreenIB);
      bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_ALWAYS);
      if (bgfx::isValid(m_SkyProgram)) {
         // fs_sky always declares s_skybox at slot 0, even when we render the
         // purely procedural branch. Bind a dummy cubemap so D3D11 debug layers
         // don't spam every fullscreen sky draw.
         const uint32_t skySamplerFlags =
            BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_W_CLAMP |
            BGFX_SAMPLER_MIN_ANISOTROPIC | BGFX_SAMPLER_MAG_ANISOTROPIC;
         bgfx::TextureHandle skyboxHandle =
            (envForSky.UseSkybox && envForSky.SkyboxTexture && envForSky.SkyboxTexture->IsValid())
               ? envForSky.SkyboxTexture->GetHandle()
               : GetDummySkyboxTexture();
         if (bgfx::isValid(s_Skybox) && bgfx::isValid(skyboxHandle)) {
            bgfx::setTexture(0, s_Skybox, skyboxHandle, skySamplerFlags);
         }
         bgfx::submit(0, m_SkyProgram);
      }
   }
   }

#ifndef CLAYMORE_RUNTIME
  const bool showGridInEditor = Application::HasInstance() && Application::Get().m_RunEditorUI && !scene.m_IsPlaying && m_ShowGrid;
   if (showGridInEditor) {
      DrawGrid();
      }
#endif

   // --------------------------------------
   // RuntimeWorld-backed render extraction
   // --------------------------------------
   const cm::world::RuntimeRenderWorld& runtimeRenderWorld =
      scene.BuildRuntimeRenderWorld(activeLayerMask, enforceLayerMask);
   uint64_t renderEntityScans =
      static_cast<uint64_t>(runtimeRenderWorld.VisibleMeshEntities.size()) +
      static_cast<uint64_t>(runtimeRenderWorld.VisibleTerrainEntities.size()) +
      static_cast<uint64_t>(runtimeRenderWorld.Lights.size());
   m_ScratchLightEntityIds.clear();
   m_ScratchVisibleMeshIds.clear();
   m_ScratchVisibleMeshData.clear();
   m_ScratchVisibleMeshHandles.clear();
   m_ScratchTerrainEntityIds.clear();
   m_ScratchTerrainData.clear();
   m_ScratchTerrainHandles.clear();
   m_ScratchShadowMeshEntityIds.clear();
   m_ScratchShadowMeshData.clear();
   m_ScratchShadowMeshHandles.clear();
   m_ScratchShadowTerrainEntityIds.clear();
   m_ScratchShadowTerrainData.clear();
   m_ScratchShadowTerrainHandles.clear();
   m_ScratchLightEntityIds.reserve(runtimeRenderWorld.Lights.size());
   m_ScratchVisibleMeshIds.reserve(runtimeRenderWorld.VisibleMeshEntities.size());
   m_ScratchVisibleMeshData.reserve(runtimeRenderWorld.VisibleMeshEntities.size());
   m_ScratchVisibleMeshHandles.reserve(runtimeRenderWorld.VisibleMeshEntities.size());
   m_ScratchTerrainEntityIds.reserve(runtimeRenderWorld.VisibleTerrainEntities.size());
   m_ScratchTerrainData.reserve(runtimeRenderWorld.VisibleTerrainEntities.size());
   m_ScratchTerrainHandles.reserve(runtimeRenderWorld.VisibleTerrainEntities.size());
   m_ScratchShadowMeshEntityIds.reserve(runtimeRenderWorld.ShadowMeshEntities.size());
   m_ScratchShadowMeshData.reserve(runtimeRenderWorld.ShadowMeshEntities.size());
   m_ScratchShadowMeshHandles.reserve(runtimeRenderWorld.ShadowMeshEntities.size());
   m_ScratchShadowTerrainEntityIds.reserve(runtimeRenderWorld.ShadowTerrainEntities.size());
   m_ScratchShadowTerrainData.reserve(runtimeRenderWorld.ShadowTerrainEntities.size());
   m_ScratchShadowTerrainHandles.reserve(runtimeRenderWorld.ShadowTerrainEntities.size());

   for (const auto& light : runtimeRenderWorld.Lights) {
      m_ScratchLightEntityIds.push_back(light.SceneEntity);
   }
   for (const auto& renderable : runtimeRenderWorld.VisibleMeshEntities) {
      m_ScratchVisibleMeshIds.push_back(renderable.SceneEntity);
      m_ScratchVisibleMeshData.push_back(scene.GetEntityData(renderable.SceneEntity));
      m_ScratchVisibleMeshHandles.push_back(renderable.Handle);
   }
   for (const auto& terrain : runtimeRenderWorld.VisibleTerrainEntities) {
      m_ScratchTerrainEntityIds.push_back(terrain.SceneEntity);
      m_ScratchTerrainData.push_back(scene.GetEntityData(terrain.SceneEntity));
      m_ScratchTerrainHandles.push_back(terrain.Handle);
   }
   for (const auto& renderable : runtimeRenderWorld.ShadowMeshEntities) {
      m_ScratchShadowMeshEntityIds.push_back(renderable.SceneEntity);
      m_ScratchShadowMeshData.push_back(scene.GetEntityData(renderable.SceneEntity));
      m_ScratchShadowMeshHandles.push_back(renderable.Handle);
   }
   for (const auto& terrain : runtimeRenderWorld.ShadowTerrainEntities) {
      m_ScratchShadowTerrainEntityIds.push_back(terrain.SceneEntity);
      m_ScratchShadowTerrainData.push_back(scene.GetEntityData(terrain.SceneEntity));
      m_ScratchShadowTerrainHandles.push_back(terrain.Handle);
   }
   Profiler::Get().SetCounter("Render/EntityScans", renderEntityScans);

  // Self-heal mesh GPU resources before any shadow/main rendering.
  // This handles cases where deferred buffers were never flushed or handles became stale.
  size_t deferredFlushedCount = 0;
  size_t recoveredMeshCount = 0;
  size_t failedRecoveryCount = 0;
  if (!m_ScratchVisibleMeshIds.empty() || !m_ScratchShadowMeshEntityIds.empty()) {
     if (DeferredGPU::GetPendingCount() > 0) {
        deferredFlushedCount += DeferredGPU::FlushPendingBuffers();
     }

     // Safety net: flush any unqueued-but-pending visible or shadow-only meshes.
     bool queuedUntrackedPending = false;
     auto queuePendingMesh = [&](EntityData* d) {
        if (!d || !d->Mesh || !d->Mesh->mesh) return;
        if (d->Mesh->mesh->HasPendingBuffers()) {
           DeferredGPU::QueueMesh(d->Mesh->mesh);
           queuedUntrackedPending = true;
        }
     };
     for (EntityData* d : m_ScratchVisibleMeshData) {
        queuePendingMesh(d);
     }
     for (EntityData* d : m_ScratchShadowMeshData) {
        queuePendingMesh(d);
     }
     if (queuedUntrackedPending) {
        deferredFlushedCount += DeferredGPU::FlushPendingBuffers();
     }

     // Last-resort recovery from CPU mesh data if handles are still invalid.
     std::unordered_set<Mesh*> recoveredMeshSet;
     recoveredMeshSet.reserve(m_ScratchVisibleMeshData.size() + m_ScratchShadowMeshData.size());
     auto recoverMesh = [&](EntityData* d) {
        if (!d || !d->Mesh || !d->Mesh->mesh) return;
        Mesh* meshPtr = d->Mesh->mesh.get();
        if (!recoveredMeshSet.insert(meshPtr).second) return;
        Mesh& mesh = *meshPtr;
        if (HasValidMeshGpuBuffers(mesh)) return;
        if (TryRecoverMeshGpuBuffers(mesh)) {
           ++recoveredMeshCount;
        } else {
           ++failedRecoveryCount;
        }
     };
     for (EntityData* d : m_ScratchVisibleMeshData) {
        recoverMesh(d);
     }
     for (EntityData* d : m_ScratchShadowMeshData) {
        recoverMesh(d);
     }
  }
  if (deferredFlushedCount > 0 || recoveredMeshCount > 0 || failedRecoveryCount > 0) {
     std::cout << "[Renderer] Mesh GPU recovery: flushedDeferred=" << deferredFlushedCount
               << " recovered=" << recoveredMeshCount
               << " failed=" << failedRecoveryCount << std::endl;
  }

   // --------------------------------------
   // Collect lights from gathered list
   // --------------------------------------
   m_ScratchLights.clear();
   if (m_ScratchLights.capacity() < static_cast<size_t>(kMaxShaderLights)) {
      m_ScratchLights.reserve(static_cast<size_t>(kMaxShaderLights));
   }
   auto buildLightData = [&](const EntityData* data) -> LightData {
      LightData ld;
      ld.type = data->Light->Type;
      ld.color = data->Light->Color * data->Light->Intensity;
      ld.position = glm::vec3(data->Transform.WorldMatrix[3]);
      if (data->Light->Type == LightType::Directional) {
         ld.direction = LightDirectionFromTransform(data->Transform);
         ld.range = 0.0f;
         ld.constant = 1.0f;
         ld.linear = 0.0f;
         ld.quadratic = 0.0f;
      } else {
         ld.direction = glm::vec3(0.0f);
         ld.range = 50.0f;
         ld.constant = 1.0f;
         ld.linear = 0.09f;
         ld.quadratic = 0.032f;
      }
      return ld;
   };
   if (!runtimeRenderWorld.Lights.empty()) {
      int primaryDirectionalIndex = -1;
      std::vector<std::pair<float, size_t>> pointCandidates;
      pointCandidates.reserve(runtimeRenderWorld.Lights.size());
      const glm::vec3 lightSelectOrigin = renderCamera ? renderCamera->GetPosition() : glm::vec3(0.0f);
      for (size_t lightIndex = 0; lightIndex < runtimeRenderWorld.Lights.size(); ++lightIndex) {
         const auto& light = runtimeRenderWorld.Lights[lightIndex];
         if (light.Type == LightType::Directional) {
            if (primaryDirectionalIndex < 0) {
               primaryDirectionalIndex = static_cast<int>(lightIndex);
            }
            continue;
         }
         const glm::vec3 delta = light.Position - lightSelectOrigin;
         const float distSq = glm::dot(delta, delta);
         pointCandidates.emplace_back(distSq, lightIndex);
      }
      std::sort(pointCandidates.begin(), pointCandidates.end(),
         [](const auto& a, const auto& b) { return a.first < b.first; });

      auto appendLight = [&](const cm::world::RuntimeLightEntry& light) {
         LightData ld{};
         ld.type = light.Type;
         ld.color = light.Color * light.Intensity;
         ld.position = light.Position;
         if (light.Type == LightType::Directional) {
            ld.direction = light.Direction;
            ld.range = 0.0f;
            ld.constant = 1.0f;
            ld.linear = 0.0f;
            ld.quadratic = 0.0f;
         } else {
            ld.direction = glm::vec3(0.0f);
            ld.range = 50.0f;
            ld.constant = 1.0f;
            ld.linear = 0.09f;
            ld.quadratic = 0.032f;
         }
         m_ScratchLights.push_back(ld);
      };

      if (primaryDirectionalIndex >= 0) {
         appendLight(runtimeRenderWorld.Lights[static_cast<size_t>(primaryDirectionalIndex)]);
      }
      for (const auto& candidate : pointCandidates) {
         if (m_ScratchLights.size() >= static_cast<size_t>(kMaxShaderLights)) break;
         appendLight(runtimeRenderWorld.Lights[candidate.second]);
      }
   } else {
      EntityID primaryDirectional = INVALID_ENTITY_ID;
      std::vector<std::pair<float, EntityID>> pointCandidates;
      pointCandidates.reserve(m_ScratchLightEntityIds.size());
      const glm::vec3 lightSelectOrigin = renderCamera ? renderCamera->GetPosition() : glm::vec3(0.0f);
      for (EntityID lightEntityId : m_ScratchLightEntityIds) {
         auto* data = scene.GetEntityData(lightEntityId);
         if (!data || !data->Light) continue;
         if (data->Light->Type == LightType::Directional) {
            if (primaryDirectional == INVALID_ENTITY_ID) {
               primaryDirectional = lightEntityId;
            }
            continue;
         }
         const glm::vec3 lp = glm::vec3(data->Transform.WorldMatrix[3]);
         const float distSq = glm::dot(lp - lightSelectOrigin, lp - lightSelectOrigin);
         pointCandidates.emplace_back(distSq, lightEntityId);
      }
      std::sort(pointCandidates.begin(), pointCandidates.end(),
         [](const auto& a, const auto& b) { return a.first < b.first; });
      if (primaryDirectional != INVALID_ENTITY_ID) {
         if (auto* d = scene.GetEntityData(primaryDirectional); d && d->Light) {
            m_ScratchLights.push_back(buildLightData(d));
         }
      }
      for (const auto& candidate : pointCandidates) {
         if (m_ScratchLights.size() >= static_cast<size_t>(kMaxShaderLights)) break;
         auto* d = scene.GetEntityData(candidate.second);
         if (!d || !d->Light) continue;
         m_ScratchLights.push_back(buildLightData(d));
      }
   }
   if (editorLightingOverride) {
      m_ScratchLights.clear();
   }
   // Upload light data to shaders
   UploadLightsToShader(m_ScratchLights);
  const PropertyID pbrScalar1Id = PropertyID::Get("u_PBRScalar1");
  const PropertyID shadowReceiveId = PropertyID::Get("u_shadowReceive");
  auto hashCombine64 = [](uint64_t h, uint64_t k) -> uint64_t {
     h ^= k + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
     return h;
  };
  auto floatBits = [](float v) -> uint32_t {
     uint32_t bits = 0;
     static_assert(sizeof(bits) == sizeof(v), "float and uint32 size mismatch");
     std::memcpy(&bits, &v, sizeof(bits));
     return bits;
  };
  uint64_t materialInstanceCacheHitCount = 0;
  uint64_t materialInstanceRebuildCount = 0;
  uint64_t materialInstanceUploadCount = 0;
  if ((s_RenderSceneFrameCounter % 240u) == 0u && !m_MaterialInstanceCache.empty()) {
     for (auto it = m_MaterialInstanceCache.begin(); it != m_MaterialInstanceCache.end(); ) {
        if (!it->second || (s_RenderSceneFrameCounter - it->second->LastTouchedFrame) > 240u) {
           it = m_MaterialInstanceCache.erase(it);
        } else {
           ++it;
        }
     }
  }
  auto resolveMaterialInstanceBinding =
     [&](Material* material, const MaterialPropertyBlockStack& stack) -> const MaterialInstanceBinding* {
        const MaterialPropertyBlock* meshBlock = (stack.MeshBlock && !stack.MeshBlock->Empty()) ? stack.MeshBlock : nullptr;
        const MaterialPropertyBlock* slotBlock = (stack.SlotBlock && !stack.SlotBlock->Empty()) ? stack.SlotBlock : nullptr;
        const MaterialPropertyBlock* proxyBlock = (stack.ProxyBlock && !stack.ProxyBlock->Empty()) ? stack.ProxyBlock : nullptr;
        if (!material || (!meshBlock && !slotBlock && !proxyBlock)) {
           return nullptr;
        }

        const MaterialInstanceCacheKey key{ material, meshBlock, slotBlock, proxyBlock };
        const uint64_t meshVersion = meshBlock ? meshBlock->GetMutationVersion() : 0;
        const uint64_t slotVersion = slotBlock ? slotBlock->GetMutationVersion() : 0;
        const uint64_t proxyVersion = proxyBlock ? proxyBlock->GetMutationVersion() : 0;
        auto [cacheIt, inserted] = m_MaterialInstanceCache.try_emplace(key, nullptr);
        if (!cacheIt->second) {
           cacheIt->second = std::make_unique<MaterialInstanceCacheEntry>();
        }

        MaterialInstanceCacheEntry& cacheEntry = *cacheIt->second;
        cacheEntry.LastTouchedFrame = s_RenderSceneFrameCounter;
        const bool needsRebuild =
           inserted ||
           cacheEntry.Binding.MaterialPtr != material ||
           cacheEntry.MeshVersion != meshVersion ||
           cacheEntry.SlotVersion != slotVersion ||
           cacheEntry.ProxyVersion != proxyVersion;

        if (!needsRebuild) {
           ++materialInstanceCacheHitCount;
           return &cacheEntry.Binding;
        }

        ++materialInstanceRebuildCount;
        cacheEntry.Binding = {};
        cacheEntry.Binding.MaterialPtr = material;
        cacheEntry.MeshVersion = meshVersion;
        cacheEntry.SlotVersion = slotVersion;
        cacheEntry.ProxyVersion = proxyVersion;

        auto compileBlock = [&](const MaterialPropertyBlock* block,
                                Material::PackedPropertyOverrides& packed) {
           packed.Clear();
           if (!block || block->Empty()) {
              return;
           }
           material->BuildPackedPropertyOverrides(*block, packed);
           if (!packed.Empty()) {
              ++materialInstanceUploadCount;
           }
        };
        compileBlock(meshBlock, cacheEntry.Binding.MeshOverrides);
        compileBlock(slotBlock, cacheEntry.Binding.SlotOverrides);
        compileBlock(proxyBlock, cacheEntry.Binding.ProxyOverrides);

        cacheEntry.Binding.PBRScalar1 = glm::vec4(0.0f);
        cacheEntry.Binding.ShadowReceive = 1.0f;
        if (auto* pbr = dynamic_cast<const PBRMaterial*>(material)) {
           cacheEntry.Binding.PBRScalar1.x = pbr->GetEmissionStrength();
           if (pbr->GetReceiveShadowsOverride()) {
              cacheEntry.Binding.ShadowReceive = pbr->GetReceiveShadows() ? 1.0f : 0.0f;
           }
        }

        auto processSpecialOverride = [&](PropertyID propertyId, const glm::vec4& value) {
           if (propertyId == pbrScalar1Id) {
              cacheEntry.Binding.HasPBRScalar1 = true;
              cacheEntry.Binding.PBRScalar1 = value;
           } else if (propertyId == shadowReceiveId) {
              cacheEntry.Binding.HasShadowReceive = true;
              cacheEntry.Binding.ShadowReceive = value.x;
           } else {
              cacheEntry.Binding.UnsupportedForInstancing = true;
           }
        };

        auto inspectBlock = [&](const MaterialPropertyBlock* block) {
           if (!block || block->Empty()) {
              return;
           }

           const auto& textureOverrides = block->GetTextureOverridesFlat();
           if (!textureOverrides.empty() || (!block->Textures.empty() && block->TexturesByID.empty())) {
              cacheEntry.Binding.UnsupportedForInstancing = true;
           }

           if (!block->Vec4ByID.empty()) {
              const auto& vecOverrides = block->GetVec4OverridesFlat();
              for (const auto& entry : vecOverrides) {
                 processSpecialOverride(PropertyID(entry.PropertyId), entry.Value);
              }
           } else {
              for (const auto& kv : block->Vec4Uniforms) {
                 processSpecialOverride(PropertyID::Get(kv.first), kv.second);
              }
           }
        };
        inspectBlock(meshBlock);
        inspectBlock(slotBlock);
        inspectBlock(proxyBlock);

        return &cacheEntry.Binding;
     };
  auto applyInstancingVariation = [&](DrawItem& item) {
     item.instancingVariationHash = 0;
     item.instancingHasPBRScalar1 = false;
     item.instancingPBRScalar1 = glm::vec4(0.0f);
     item.instancingHasShadowReceive = false;
     item.instancingShadowReceive = 1.0f;

     if (!item.canInstance && !item.canSkinnedInstance) {
        return;
     }

      bool unsupportedOverride = false;
     if (item.materialInstance) {
        unsupportedOverride = item.materialInstance->UnsupportedForInstancing;
        if (item.materialInstance->HasPBRScalar1) {
           item.instancingHasPBRScalar1 = true;
           item.instancingPBRScalar1 = item.materialInstance->PBRScalar1;
        }
        if (item.materialInstance->HasShadowReceive) {
           item.instancingHasShadowReceive = true;
           item.instancingShadowReceive = item.materialInstance->ShadowReceive;
        }
     } else {
        auto inspectBlock = [&](const MaterialPropertyBlock* block) {
           if (!block || block->Empty()) return;
           const auto& textureOverrides = block->GetTextureOverridesFlat();
           const auto& vecOverrides = block->GetVec4OverridesFlat();
           if (!textureOverrides.empty() || !block->Textures.empty()) {
              unsupportedOverride = true;
              return;
           }
           if (!vecOverrides.empty()) {
              for (const auto& entry : vecOverrides) {
                 if (entry.PropertyId == pbrScalar1Id.Value()) {
                    item.instancingHasPBRScalar1 = true;
                    item.instancingPBRScalar1 = entry.Value;
                 } else if (entry.PropertyId == shadowReceiveId.Value()) {
                    item.instancingHasShadowReceive = true;
                    item.instancingShadowReceive = entry.Value.x;
                 } else {
                    unsupportedOverride = true;
                    return;
                 }
              }
           } else if (!block->Vec4Uniforms.empty()) {
              for (const auto& kv : block->Vec4Uniforms) {
                 const PropertyID pid = PropertyID::Get(kv.first);
                 if (pid == pbrScalar1Id) {
                    item.instancingHasPBRScalar1 = true;
                    item.instancingPBRScalar1 = kv.second;
                 } else if (pid == shadowReceiveId) {
                    item.instancingHasShadowReceive = true;
                    item.instancingShadowReceive = kv.second.x;
                 } else {
                    unsupportedOverride = true;
                    return;
                 }
              }
           }
        };

        inspectBlock(item.propertyBlocks.MeshBlock);
        inspectBlock(item.propertyBlocks.SlotBlock);
        inspectBlock(item.propertyBlocks.ProxyBlock);
     }
     if (unsupportedOverride) {
        item.canInstance = false;
        item.canSkinnedInstance = false;
        return;
     }

     if (item.alphaCutoutOverride) {
        if (!item.instancingHasPBRScalar1) {
           item.instancingPBRScalar1 = glm::vec4(0.0f);
           if (auto pbr = dynamic_cast<const PBRMaterial*>(item.material)) {
              item.instancingPBRScalar1.x = pbr->GetEmissionStrength();
           }
           item.instancingHasPBRScalar1 = true;
        }
        item.instancingPBRScalar1.y = item.alphaCutoutThreshold;
     }
     if (!item.receiveShadows) {
        item.instancingHasShadowReceive = true;
        item.instancingShadowReceive = 0.0f;
     }
     item.instancingShadowReceive = glm::clamp(item.instancingShadowReceive, 0.0f, 1.0f);

     uint64_t h = 1469598103934665603ULL;
     h = hashCombine64(h, item.instancingHasPBRScalar1 ? 1ULL : 0ULL);
     if (item.instancingHasPBRScalar1) {
        h = hashCombine64(h, floatBits(item.instancingPBRScalar1.x));
        h = hashCombine64(h, floatBits(item.instancingPBRScalar1.y));
        h = hashCombine64(h, floatBits(item.instancingPBRScalar1.z));
        h = hashCombine64(h, floatBits(item.instancingPBRScalar1.w));
     }
     h = hashCombine64(h, item.instancingHasShadowReceive ? 1ULL : 0ULL);
     if (item.instancingHasShadowReceive) {
        h = hashCombine64(h, floatBits(item.instancingShadowReceive));
     }
     item.instancingVariationHash = h;
  };
  const bool supportsAlphaToCoverage =
     bgfx::getCaps() != nullptr && 0 != (bgfx::getCaps()->supported & BGFX_CAPS_ALPHA_TO_COVERAGE);
  auto resolveDrawStateFlags = [supportsAlphaToCoverage](const DrawItem& di) -> uint64_t {
     uint64_t stateFlags = di.material ? di.material->GetStateFlags() : 0u;
     if (di.alphaOverride) {
        if (di.alphaEnable) stateFlags |= BGFX_STATE_BLEND_ALPHA;
        else stateFlags &= ~BGFX_STATE_BLEND_ALPHA;
     }
     if (di.depthWriteOverride) {
        if (di.depthWriteEnable) stateFlags |= BGFX_STATE_WRITE_Z;
        else stateFlags &= ~BGFX_STATE_WRITE_Z;
     }
     if (di.depthTestOverride) {
        stateFlags &= ~BGFX_STATE_DEPTH_TEST_MASK;
        if (di.depthTestEnable) {
           stateFlags |= BGFX_STATE_DEPTH_TEST_LEQUAL;
        }
     }
     if (di.showBackfaces) {
        stateFlags &= ~(BGFX_STATE_CULL_CW | BGFX_STATE_CULL_CCW);
     }
     if (di.alphaCutoutOverride) {
        stateFlags &= ~BGFX_STATE_BLEND_MASK;
        stateFlags &= ~BGFX_STATE_BLEND_ALPHA_TO_COVERAGE;
        if (supportsAlphaToCoverage) {
           stateFlags |= BGFX_STATE_BLEND_ALPHA_TO_COVERAGE;
        }
        if (!di.depthWriteOverride || di.depthWriteEnable) {
           stateFlags |= BGFX_STATE_WRITE_Z;
        }
        if (!di.depthTestOverride || di.depthTestEnable) {
           stateFlags &= ~BGFX_STATE_DEPTH_TEST_MASK;
           stateFlags |= BGFX_STATE_DEPTH_TEST_LEQUAL;
        }
     }
     if (di.isTransparent) {
        stateFlags &= ~BGFX_STATE_BLEND_ALPHA_TO_COVERAGE;
        stateFlags &= ~BGFX_STATE_WRITE_Z;
        stateFlags &= ~BGFX_STATE_DEPTH_TEST_MASK;
        stateFlags |= BGFX_STATE_DEPTH_TEST_LEQUAL;
     } else if (!di.alphaCutoutOverride) {
        stateFlags &= ~BGFX_STATE_BLEND_ALPHA_TO_COVERAGE;
     }
     if (di.renderOnTop) {
        stateFlags &= ~BGFX_STATE_WRITE_Z;
        stateFlags &= ~BGFX_STATE_DEPTH_TEST_MASK;
        stateFlags |= BGFX_STATE_DEPTH_TEST_ALWAYS;
     }
     return stateFlags;
  };
  const PropertyID colorTintId = PropertyID::Get("u_ColorTint");
  const PropertyID tintParamsId = PropertyID::Get("u_TintParams");
  struct MaterialBindingKeyInfo {
     uint64_t key = 0;
     bool equivalentSafe = false;
  };
  auto getMaterialBindingKeyInfo = [&](const Material* material) -> MaterialBindingKeyInfo {
     if (!material) {
        return {};
     }

     MaterialBindingKeyInfo info{};
     info.key = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(material));

     // Collapse equivalent PBR/SkinnedPBR clone bindings.
     // MaterialInstance is excluded so runtime/editor/onValidate overrides remain isolated.
     if (dynamic_cast<const MaterialInstance*>(material) == nullptr) {
        if (const auto* pbr = dynamic_cast<const PBRMaterial*>(material)) {
           uint64_t h = 1469598103934665603ULL;
           h = hashCombine64(h, static_cast<uint64_t>(material->GetProgram().idx));
           h = hashCombine64(h, static_cast<uint64_t>(material->GetStateFlags()));
           h = hashCombine64(h, static_cast<uint64_t>(pbr->m_AlbedoTex.idx));
           h = hashCombine64(h, static_cast<uint64_t>(pbr->m_MetallicRoughnessTex.idx));
           h = hashCombine64(h, static_cast<uint64_t>(pbr->m_NormalTex.idx));
           h = hashCombine64(h, static_cast<uint64_t>(pbr->m_AOTex.idx));
           h = hashCombine64(h, static_cast<uint64_t>(pbr->m_EmissionTex.idx));
           h = hashCombine64(h, static_cast<uint64_t>(pbr->m_DisplacementTex.idx));
           h = hashCombine64(h, floatBits(pbr->GetMetallic()));
           h = hashCombine64(h, floatBits(pbr->GetRoughness()));
           h = hashCombine64(h, floatBits(pbr->GetNormalScale()));
           h = hashCombine64(h, floatBits(pbr->GetAmbientOcclusion()));
           h = hashCombine64(h, floatBits(pbr->GetEmissionStrength()));
           h = hashCombine64(h, floatBits(pbr->GetDisplacementScale()));
           const glm::vec3 emissionColor = pbr->GetEmissionColor();
           h = hashCombine64(h, floatBits(emissionColor.x));
           h = hashCombine64(h, floatBits(emissionColor.y));
           h = hashCombine64(h, floatBits(emissionColor.z));
           const glm::vec2 uvScale = pbr->GetUVScale();
           const glm::vec2 uvOffset = pbr->GetUVOffset();
           h = hashCombine64(h, floatBits(uvScale.x));
           h = hashCombine64(h, floatBits(uvScale.y));
           h = hashCombine64(h, floatBits(uvOffset.x));
           h = hashCombine64(h, floatBits(uvOffset.y));
           glm::vec4 tint(1.0f);
           glm::vec4 tintParams(0.0f);
           material->TryGetUniform(colorTintId, tint);
           material->TryGetUniform(tintParamsId, tintParams);
           h = hashCombine64(h, floatBits(tint.x));
           h = hashCombine64(h, floatBits(tint.y));
           h = hashCombine64(h, floatBits(tint.z));
           h = hashCombine64(h, floatBits(tint.w));
           h = hashCombine64(h, floatBits(tintParams.x));
           h = hashCombine64(h, floatBits(tintParams.y));
           h = hashCombine64(h, floatBits(tintParams.z));
           h = hashCombine64(h, floatBits(tintParams.w));
           info.key = h;
           info.equivalentSafe = true;
        }
     }

     return info;
  };

   // --------------------------------------
   // Draw all meshes
   // --------------------------------------

   // Build frustum once per frame if CPU culling is enabled
  Frustum fr;
  const bool doCull = m_EnableFrustumCulling;
  if (doCull) {
     fr = BuildFrustum(m_view, m_proj);
   }
  const cm::world::RuntimeWorld* runtimeWorld = scene.GetRuntimeWorld();
  auto* frameRagdollSystem = scene.m_IsPlaying ? cm::physics::GetRagdollSystem() : nullptr;
  std::unordered_map<EntityID, std::pair<glm::vec3, glm::vec3>> activeRagdollBoundsCache;
  activeRagdollBoundsCache.reserve(16);
  auto tryGetActiveRagdollBounds = [&](const EntityData* data,
                                       const Mesh* mesh,
                                       glm::vec3& outMin,
                                       glm::vec3& outMax) -> bool {
      return TryGetSharedSkinnedCharacterBounds(
         scene,
         data,
         mesh,
         runtimeWorld,
         frameRagdollSystem,
         activeRagdollBoundsCache,
         outMin,
         outMax);
   };
  const uint64_t sceneRevisionForBoundsCache = scene.GetDirtyRevision();
  if (m_StaticBoundsCacheSceneRevision != sceneRevisionForBoundsCache) {
     m_StaticBoundsCache.clear();
     m_StaticBoundsCacheSceneRevision = sceneRevisionForBoundsCache;
  }

  const bgfx::Caps* bgfxCaps = bgfx::getCaps();
  const bool supportsSkinnedOcclusionQueries =
     scene.m_IsPlaying &&
     bgfxCaps != nullptr &&
     bgfxCaps->limits.maxOcclusionQueries > 0;
  const EntityID activeCameraOwnerEntity =
     scene.m_IsPlaying ? ResolveActiveCameraOwnerEntity(scene) : INVALID_ENTITY_ID;
  if (supportsSkinnedOcclusionQueries) {
     ++m_SkinnedOcclusionFrame;
  }

  auto resolveMeshSkeleton = [&scene](const EntityData* data) -> SkeletonComponent* {
     if (!data || !data->Skinning) {
        return nullptr;
     }

     EntityID skelRoot = data->Skinning->SkeletonRoot;
     if (skelRoot == INVALID_ENTITY_ID) {
        skelRoot = data->Skinning->ResolvedSkeletonRoot;
     }
     if (skelRoot == INVALID_ENTITY_ID) {
        return nullptr;
     }

     auto* skelData = scene.GetEntityData(skelRoot);
     if (!skelData || !skelData->Skeleton) {
        return nullptr;
     }

     return skelData->Skeleton.get();
  };

  struct SkinnedOcclusionCandidate {
      EntityID skeletonRoot = INVALID_ENTITY_ID;
      SkeletonComponent* skeleton = nullptr;
      glm::vec3 boundsMin = glm::vec3(0.0f);
      glm::vec3 boundsMax = glm::vec3(0.0f);
  };
  std::vector<SkinnedOcclusionCandidate> skinnedOcclusionCandidates;
  skinnedOcclusionCandidates.reserve(64);
  std::unordered_map<EntityID, size_t> skinnedOcclusionCandidateLookup;
  skinnedOcclusionCandidateLookup.reserve(64);
  auto appendSkinnedOcclusionCandidate =
      [&](EntityID skeletonRoot,
          SkeletonComponent* skeleton,
          const glm::vec3& boundsMin,
          const glm::vec3& boundsMax) {
      if (!supportsSkinnedOcclusionQueries ||
          skeletonRoot == INVALID_ENTITY_ID ||
          skeleton == nullptr) {
         return;
      }

      auto it = skinnedOcclusionCandidateLookup.find(skeletonRoot);
      if (it == skinnedOcclusionCandidateLookup.end()) {
         skinnedOcclusionCandidateLookup.emplace(
            skeletonRoot,
            skinnedOcclusionCandidates.size());
         skinnedOcclusionCandidates.push_back(
            { skeletonRoot, skeleton, boundsMin, boundsMax });
         return;
      }

      SkinnedOcclusionCandidate& existing = skinnedOcclusionCandidates[it->second];
      existing.boundsMin = glm::min(existing.boundsMin, boundsMin);
      existing.boundsMax = glm::max(existing.boundsMax, boundsMax);
  };

  std::vector<uint8_t> precomputedCullValid;
  std::vector<glm::vec3> precomputedCullMin;
  std::vector<glm::vec3> precomputedCullMax;
  if (doCull && !m_ScratchVisibleMeshIds.empty()) {
      const size_t visibleCount = m_ScratchVisibleMeshIds.size();
      precomputedCullValid.assign(visibleCount, 0u);
      precomputedCullMin.resize(visibleCount);
      precomputedCullMax.resize(visibleCount);

      for (size_t visIdx = 0; visIdx < visibleCount; ++visIdx) {
         const EntityID eid = m_ScratchVisibleMeshIds[visIdx];
         auto* data = visIdx < m_ScratchVisibleMeshData.size() ? m_ScratchVisibleMeshData[visIdx] : nullptr;
         if (!data || !data->Mesh || !data->Mesh->mesh || data->Mesh->SkipFrustumCulling) {
            continue;
         }
         Mesh* meshPtr = data->Mesh->mesh.get();
         if (!meshPtr) {
            continue;
         }

         if (tryGetActiveRagdollBounds(data, meshPtr, precomputedCullMin[visIdx], precomputedCullMax[visIdx])) {
            precomputedCullValid[visIdx] = 1u;
            continue;
         }

         if (runtimeWorld && visIdx < m_ScratchVisibleMeshHandles.size()) {
            const cm::world::RuntimeBounds* runtimeBounds =
               runtimeWorld->TryGetBounds(m_ScratchVisibleMeshHandles[visIdx]);
            if (runtimeBounds && runtimeBounds->Valid) {
               precomputedCullValid[visIdx] = 1u;
               precomputedCullMin[visIdx] = runtimeBounds->WorldMin;
               precomputedCullMax[visIdx] = runtimeBounds->WorldMax;
               continue;
            }
         }

         const bool staticCacheEligible = !meshPtr->Dynamic && !data->Skinning;
         if (!staticCacheEligible) {
            continue;
         }

         const glm::mat4& M = data->Transform.WorldMatrix;
         const float* worldPtr = glm::value_ptr(M);
         auto cacheIt = m_StaticBoundsCache.find(eid);
         if (cacheIt != m_StaticBoundsCache.end()) {
            const auto& entry = cacheIt->second;
            if (entry.valid &&
                entry.mesh == meshPtr &&
                std::abs(entry.boundsPadding - data->Mesh->BoundsPadding) <= 1e-6f &&
                std::memcmp(entry.worldMatrix, worldPtr, sizeof(entry.worldMatrix)) == 0) {
               precomputedCullValid[visIdx] = 1u;
               precomputedCullMin[visIdx] = entry.worldMin;
               precomputedCullMax[visIdx] = entry.worldMax;
               continue;
            }
         }

         const glm::vec3 lmin = meshPtr->BoundsMin;
         const glm::vec3 lmax = meshPtr->BoundsMax;
         const glm::vec3 lcenter = (lmin + lmax) * 0.5f;
         const glm::vec3 lextents = (lmax - lmin) * 0.5f * data->Mesh->BoundsPadding;
         const glm::vec3 worldCenter = glm::vec3(M * glm::vec4(lcenter, 1.0f));
         glm::vec3 ex;
         ex.x = std::abs(M[0][0]) * lextents.x + std::abs(M[1][0]) * lextents.y + std::abs(M[2][0]) * lextents.z;
         ex.y = std::abs(M[0][1]) * lextents.x + std::abs(M[1][1]) * lextents.y + std::abs(M[2][1]) * lextents.z;
         ex.z = std::abs(M[0][2]) * lextents.x + std::abs(M[1][2]) * lextents.y + std::abs(M[2][2]) * lextents.z;
         const glm::vec3 wmin = worldCenter - ex;
         const glm::vec3 wmax = worldCenter + ex;

         precomputedCullValid[visIdx] = 1u;
         precomputedCullMin[visIdx] = wmin;
         precomputedCullMax[visIdx] = wmax;

         StaticBoundsCacheEntry& entry = m_StaticBoundsCache[eid];
         entry.mesh = meshPtr;
         entry.boundsPadding = data->Mesh->BoundsPadding;
         std::memcpy(entry.worldMatrix, worldPtr, sizeof(entry.worldMatrix));
         entry.worldMin = wmin;
         entry.worldMax = wmax;
         entry.valid = true;
      }
  }

  std::vector<uint8_t> mainViewVisibleMeshIds;
  struct MainViewMeshInput {
     EntityID Entity = INVALID_ENTITY_ID;
     EntityData* Data = nullptr;
     Mesh* MeshAsset = nullptr;
     SkinningComponent* Skinning = nullptr;
     SkeletonComponent* Skeleton = nullptr;
  };
  std::vector<EntityID> mainViewMeshEntityIds;
  std::vector<MainViewMeshInput> mainViewMeshInputs;
  if (!m_ScratchVisibleMeshIds.empty()) {
     mainViewVisibleMeshIds.assign(m_ScratchVisibleMeshIds.size(), 0u);
     mainViewMeshEntityIds.reserve(m_ScratchVisibleMeshIds.size());
     mainViewMeshInputs.reserve(m_ScratchVisibleMeshIds.size());

     std::unordered_map<SkeletonComponent*, bool> skeletonMainViewVisibility;
     skeletonMainViewVisibility.reserve(64);
     std::unordered_map<EntityID, bool> skinnedSharedMainViewCullCache;
     skinnedSharedMainViewCullCache.reserve(64);
     std::unordered_map<EntityID, bool> skinnedLastRenderVisibilityCache;
     skinnedLastRenderVisibilityCache.reserve(64);

     for (size_t visIdx = 0; visIdx < m_ScratchVisibleMeshIds.size(); ++visIdx) {
        const EntityID eid = m_ScratchVisibleMeshIds[visIdx];
        auto* data = visIdx < m_ScratchVisibleMeshData.size() ? m_ScratchVisibleMeshData[visIdx] : nullptr;
        SkeletonComponent* skeleton = resolveMeshSkeleton(data);
        const EntityID skelRoot = ResolveSkinningSkeletonRootEntity(data);
        if (skeleton) {
           skeletonMainViewVisibility.emplace(skeleton, false);
        }

        if (!IsPresentationVisible(data) || !data->Active || !data->Mesh || !data->Mesh->mesh) {
           continue;
        }

        Mesh* meshPtr = data->Mesh->mesh.get();
        if (!meshPtr) {
           continue;
        }

        bool passesMainViewCull = true;
        if (doCull && !data->Mesh->SkipFrustumCulling) {
           if (visIdx < precomputedCullValid.size() && precomputedCullValid[visIdx]) {
              if (skelRoot != INVALID_ENTITY_ID) {
                 auto cacheIt = skinnedSharedMainViewCullCache.find(skelRoot);
                 if (cacheIt == skinnedSharedMainViewCullCache.end()) {
                    const bool visible = AabbIntersectsFrustum(fr, precomputedCullMin[visIdx], precomputedCullMax[visIdx]);
                    cacheIt = skinnedSharedMainViewCullCache.emplace(skelRoot, visible).first;
                 }
                 passesMainViewCull = cacheIt->second;
              } else {
                 passesMainViewCull = AabbIntersectsFrustum(fr, precomputedCullMin[visIdx], precomputedCullMax[visIdx]);
              }
           } else {
              const glm::vec3 lmin = meshPtr->BoundsMin;
              const glm::vec3 lmax = meshPtr->BoundsMax;
              const glm::vec3 lcenter = (lmin + lmax) * 0.5f;
              const glm::vec3 lextents = (lmax - lmin) * 0.5f * data->Mesh->BoundsPadding;

              const glm::mat4& M = data->Transform.WorldMatrix;
              const glm::vec3 worldCenter = glm::vec3(M * glm::vec4(lcenter, 1.0f));

              glm::vec3 ex;
              ex.x = std::abs(M[0][0]) * lextents.x + std::abs(M[1][0]) * lextents.y + std::abs(M[2][0]) * lextents.z;
              ex.y = std::abs(M[0][1]) * lextents.x + std::abs(M[1][1]) * lextents.y + std::abs(M[2][1]) * lextents.z;
              ex.z = std::abs(M[0][2]) * lextents.x + std::abs(M[1][2]) * lextents.y + std::abs(M[2][2]) * lextents.z;

              const glm::vec3 wmin = worldCenter - ex;
              const glm::vec3 wmax = worldCenter + ex;
              passesMainViewCull = AabbIntersectsFrustum(fr, wmin, wmax);
           }
        }

        bool renderVisibleLastFrame = passesMainViewCull;
        const bool cameraOwnedSkinned =
           skelRoot != INVALID_ENTITY_ID &&
           (IsEntityDescendantOf(scene, skelRoot, activeCameraOwnerEntity) ||
            IsEntityDescendantOf(scene, activeCameraOwnerEntity, skelRoot));
        if (supportsSkinnedOcclusionQueries &&
            skeleton != nullptr &&
            skelRoot != INVALID_ENTITY_ID &&
            passesMainViewCull &&
            !cameraOwnedSkinned) {
           auto visibleIt = skinnedLastRenderVisibilityCache.find(skelRoot);
           if (visibleIt == skinnedLastRenderVisibilityCache.end()) {
              bool lastVisible = true;
              Renderer::SkinnedOcclusionQueryEntry* queryEntry = nullptr;
              const uint64_t queryKey = MakeSkinnedOcclusionKey(scene, skelRoot);
              auto entryIt = m_SkinnedOcclusionQueries.find(queryKey);
              if (entryIt != m_SkinnedOcclusionQueries.end()) {
                 queryEntry = &entryIt->second;
                 queryEntry->LastTouchedFrame = m_SkinnedOcclusionFrame;
              }

              if (queryEntry && bgfx::isValid(queryEntry->Handle)) {
                 int32_t visibleSamples = 0;
                 switch (bgfx::getResult(queryEntry->Handle, &visibleSamples)) {
                 case bgfx::OcclusionQueryResult::Visible:
                    queryEntry->LastVisible = true;
                    queryEntry->LastVisibleSamples = visibleSamples;
                    queryEntry->ConsecutiveInvisibleFrames = 0;
                    break;
                 case bgfx::OcclusionQueryResult::Invisible:
                    queryEntry->LastVisibleSamples = visibleSamples;
                    if (queryEntry->ConsecutiveInvisibleFrames < std::numeric_limits<uint8_t>::max()) {
                       ++queryEntry->ConsecutiveInvisibleFrames;
                    }
                    if (queryEntry->ConsecutiveInvisibleFrames >=
                        kSkinnedOcclusionInvisibleHysteresisFrames) {
                       queryEntry->LastVisible = false;
                    }
                    break;
                 case bgfx::OcclusionQueryResult::NoResult:
                 default:
                    break;
                 }
                 lastVisible = queryEntry->LastVisible;
              }

              visibleIt = skinnedLastRenderVisibilityCache.emplace(skelRoot, lastVisible).first;
           }
           renderVisibleLastFrame = visibleIt->second;
        }

        if (!passesMainViewCull) {
           continue;
        }

        if (!HasRenderableVertexSource(*this, data, meshPtr)) {
           continue;
        }

        if (skelRoot != INVALID_ENTITY_ID &&
            skeleton != nullptr &&
            !cameraOwnedSkinned) {
           if (visIdx < precomputedCullValid.size() && precomputedCullValid[visIdx]) {
              appendSkinnedOcclusionCandidate(
                 skelRoot,
                 skeleton,
                 precomputedCullMin[visIdx],
                 precomputedCullMax[visIdx]);
           } else {
              const glm::vec3 lmin = meshPtr->BoundsMin;
              const glm::vec3 lmax = meshPtr->BoundsMax;
              const glm::vec3 lcenter = (lmin + lmax) * 0.5f;
              const glm::vec3 lextents =
                 (lmax - lmin) * 0.5f * std::max(0.01f, data->Mesh->BoundsPadding);
              const glm::mat4& M = data->Transform.WorldMatrix;
              const glm::vec3 worldCenter = glm::vec3(M * glm::vec4(lcenter, 1.0f));
              glm::vec3 ex;
              ex.x = std::abs(M[0][0]) * lextents.x + std::abs(M[1][0]) * lextents.y + std::abs(M[2][0]) * lextents.z;
              ex.y = std::abs(M[0][1]) * lextents.x + std::abs(M[1][1]) * lextents.y + std::abs(M[2][1]) * lextents.z;
              ex.z = std::abs(M[0][2]) * lextents.x + std::abs(M[1][2]) * lextents.y + std::abs(M[2][2]) * lextents.z;
              appendSkinnedOcclusionCandidate(
                 skelRoot,
                 skeleton,
                 worldCenter - ex,
                 worldCenter + ex);
           }
        }

        if (!renderVisibleLastFrame) {
           continue;
        }

        mainViewVisibleMeshIds[visIdx] = 1u;
        mainViewMeshEntityIds.push_back(eid);
        MainViewMeshInput input{};
        input.Entity = eid;
        input.Data = data;
        input.MeshAsset = meshPtr;
        input.Skinning = data->Skinning.get();
        input.Skeleton = skeleton;
        mainViewMeshInputs.push_back(input);
        if (skeleton) {
           skeletonMainViewVisibility[skeleton] = true;
        }
     }

     for (const auto& [skeleton, visible] : skeletonMainViewVisibility) {
        skeleton->LodMeshVisibleLastFrame = visible;
     }

     if (m_RuntimeStatsCaptureEnabled) {
        uint64_t renderedMeshObjects = 0;
        uint64_t renderedSkinnedMeshObjects = 0;
        uint64_t totalSkinnedMeshObjects = 0;
        for (uint8_t visible : mainViewVisibleMeshIds) {
           renderedMeshObjects += (visible != 0u) ? 1u : 0u;
        }
        for (size_t visIdx = 0; visIdx < m_ScratchVisibleMeshIds.size(); ++visIdx) {
           auto* data = visIdx < m_ScratchVisibleMeshData.size() ? m_ScratchVisibleMeshData[visIdx] : nullptr;
           if (!data || !data->Skinning) {
              continue;
           }

           ++totalSkinnedMeshObjects;
           renderedSkinnedMeshObjects +=
              (visIdx < mainViewVisibleMeshIds.size() && mainViewVisibleMeshIds[visIdx] != 0u) ? 1u : 0u;
        }
        m_LastRuntimeStatsFrame.RenderedMeshObjects = renderedMeshObjects;
        m_LastRuntimeStatsFrame.CulledMeshObjects =
           static_cast<uint64_t>(m_ScratchVisibleMeshIds.size()) - renderedMeshObjects;
        m_LastRuntimeStatsFrame.RenderedSkinnedMeshObjects = renderedSkinnedMeshObjects;
        m_LastRuntimeStatsFrame.CulledSkinnedMeshObjects =
           (totalSkinnedMeshObjects > renderedSkinnedMeshObjects)
              ? (totalSkinnedMeshObjects - renderedSkinnedMeshObjects)
              : 0u;
     }
  }
  Profiler::Get().SetCounter(
     "Render/MeshCandidates",
     static_cast<uint64_t>(m_ScratchVisibleMeshIds.size()));
  Profiler::Get().SetCounter(
     "Render/MainViewVisibleMeshes",
     static_cast<uint64_t>(mainViewMeshEntityIds.size()));
  Profiler::Get().SetCounter(
     "Render/MainViewCulledMeshes",
     static_cast<uint64_t>(
        m_ScratchVisibleMeshIds.size() > mainViewMeshEntityIds.size()
           ? m_ScratchVisibleMeshIds.size() - mainViewMeshEntityIds.size()
           : 0u));

  const std::vector<EntityID>* skinningShadowEntities = &m_ScratchShadowMeshEntityIds;
  PrepareGpuSkinningAtlases(scene, m_ScratchVisibleMeshIds, skinningShadowEntities);
  PrepareGpuMaterializedSkinnedMeshes(scene, m_ScratchVisibleMeshIds, skinningShadowEntities);

  if (renderShadowMapThisFrame) {
     ScopedTimer t("Render/Shadows");
     RenderShadowMap(scene, renderCamera);
  } else {
     m_ShadowCascadeSubmitCounts = {0u, 0u, 0u, 0u};
  }

  if (supportsSkinnedOcclusionQueries && !skinnedOcclusionCandidates.empty()) {
     ScopedTimer t("Render/SkinnedOcclusion");
     static bgfx::ProgramHandle s_occlusionDepthProg = BGFX_INVALID_HANDLE;
     if (!bgfx::isValid(s_occlusionDepthProg)) {
        s_occlusionDepthProg = ShaderManager::Instance().LoadProgram("vs_depth", "fs_depth");
     }

     std::shared_ptr<Mesh> cubeMesh = StandardMeshManager::Instance().GetCubeMesh();
     if (cubeMesh &&
         bgfx::isValid(s_occlusionDepthProg) &&
         bgfx::isValid(cubeMesh->vbh) &&
         bgfx::isValid(cubeMesh->ibh)) {
        bgfx::setViewRect(kSkinnedOcclusionViewId, 0, 0, m_Width, m_Height);
        bgfx::setViewTransform(kSkinnedOcclusionViewId, m_view, m_proj);
        if (m_RenderToOffscreen) {
           bgfx::setViewFrameBuffer(kSkinnedOcclusionViewId, m_SceneFrameBuffer);
        } else {
           bgfx::setViewFrameBuffer(kSkinnedOcclusionViewId, BGFX_INVALID_HANDLE);
        }
        bgfx::setViewClear(kSkinnedOcclusionViewId, BGFX_CLEAR_NONE, 0, 1.0f, 0);
        bgfx::touch(kSkinnedOcclusionViewId);

        const glm::mat4 viewProj =
           glm::make_mat4(m_proj) * glm::make_mat4(m_view);
        bgfx::setUniform(u_LightViewProj, glm::value_ptr(viewProj));

        uint64_t queryCount = 0;
        uint64_t lastFrameVisibleCount = 0;
        uint64_t lastFrameInvisibleCount = 0;
        for (const auto& candidate : skinnedOcclusionCandidates) {
           if (candidate.skeletonRoot == INVALID_ENTITY_ID ||
               candidate.skeleton == nullptr) {
              continue;
           }

           const uint64_t queryKey = MakeSkinnedOcclusionKey(scene, candidate.skeletonRoot);
           auto& entry = m_SkinnedOcclusionQueries[queryKey];
           entry.LastTouchedFrame = m_SkinnedOcclusionFrame;
           if (!bgfx::isValid(entry.Handle)) {
              entry.Handle = bgfx::createOcclusionQuery();
              if (!bgfx::isValid(entry.Handle)) {
                 continue;
              }
           }

           const glm::vec3 center = 0.5f * (candidate.boundsMin + candidate.boundsMax);
           const glm::vec3 extent = glm::max(
              0.5f * (candidate.boundsMax - candidate.boundsMin),
              glm::vec3(0.05f));
           const glm::mat4 queryTransform =
              glm::translate(glm::mat4(1.0f), center) *
              glm::scale(glm::mat4(1.0f), extent);

           bgfx::setTransform(glm::value_ptr(queryTransform));
           bgfx::setVertexBuffer(0, cubeMesh->vbh);
           bgfx::setIndexBuffer(cubeMesh->ibh);
           bgfx::setState(BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_CULL_CW | BGFX_STATE_MSAA);
           bgfx::submit(kSkinnedOcclusionViewId, s_occlusionDepthProg, entry.Handle);

           ++queryCount;
           if (entry.LastVisible) {
              ++lastFrameVisibleCount;
           } else {
              ++lastFrameInvisibleCount;
           }
        }

        Profiler::Get().SetCounter("Render/SkinnedOcclusionQueries", queryCount);
        Profiler::Get().SetCounter("Render/SkinnedVisibleLastFrame", lastFrameVisibleCount);
        Profiler::Get().SetCounter("Render/SkinnedOccludedLastFrame", lastFrameInvisibleCount);
     }
  } else {
     Profiler::Get().SetCounter("Render/SkinnedOcclusionQueries", 0);
     Profiler::Get().SetCounter("Render/SkinnedVisibleLastFrame", 0);
     Profiler::Get().SetCounter("Render/SkinnedOccludedLastFrame", 0);
  }

  if (supportsSkinnedOcclusionQueries && (m_SkinnedOcclusionFrame & 127ULL) == 0ULL) {
     for (auto it = m_SkinnedOcclusionQueries.begin(); it != m_SkinnedOcclusionQueries.end();) {
        if (it->second.LastTouchedFrame + 512ULL < m_SkinnedOcclusionFrame) {
           if (bgfx::isValid(it->second.Handle)) {
              bgfx::destroy(it->second.Handle);
           }
           it = m_SkinnedOcclusionQueries.erase(it);
        } else {
           ++it;
        }
     }
  }

   // PERFORMANCE: Use member DrawItem struct and scratch buffer
   m_ScratchDraws.clear();
   if (m_ScratchDraws.capacity() < mainViewMeshEntityIds.size() * 2)
      m_ScratchDraws.reserve(mainViewMeshEntityIds.size() * 2);

  // PERF: Parallel DrawItem gathering with frustum culling
  // Use parallel processing when JobSystem is available and we have enough work
  const bool useParallel = cm::g_JobSystem != nullptr && mainViewMeshInputs.size() > 32;
  { ScopedTimer t("Render/MeshGather");
   
   if (useParallel) {
      // Process meshes in parallel chunks
      const size_t chunkSize = ComputeOptimalChunkSize(mainViewMeshInputs.size(),
                                                        cm::g_JobSystem->GetWorkerCount(), 32);
      const size_t chunkCount = (mainViewMeshInputs.size() + chunkSize - 1) / chunkSize;
      std::vector<std::vector<DrawItem>> chunkResults(chunkCount);
      
      parallel_for(*cm::g_JobSystem, size_t(0), mainViewMeshInputs.size(), chunkSize,
         [&](size_t start, size_t count) {
            // Per-thread vector for collecting DrawItems (allocated on stack, moved to results)
            std::vector<DrawItem> localDrawItems;
            localDrawItems.reserve(count * 2);
            
            // Process this chunk of entities
            for (size_t i = start; i < start + count && i < mainViewMeshInputs.size(); ++i) {
               const MainViewMeshInput& meshInput = mainViewMeshInputs[i];
               EntityID eid = meshInput.Entity;
               auto* data = meshInput.Data;
               if (!IsPresentationVisible(data) || !data->Active || !data->Mesh || !data->Mesh->mesh) continue;
               Mesh* meshPtr = meshInput.MeshAsset;
               if (!meshPtr) continue;

               DrawItem base{};
               base.mesh = meshPtr;
               base.material = data->Mesh->material.get();
               base.propertyBlocks.MeshBlock = &data->Mesh->PropertyBlock;
               base.entityId = eid;
               memcpy(base.transform, glm::value_ptr(data->Transform.WorldMatrix), sizeof(float) * 16);
               base.renderOnTop = data->Mesh->RenderOnTop;
               base.renderOrder = data->Mesh->RenderOrder;
               base.showBackfaces = data->Mesh->ShowBackfaces;
               
               // PERF: Check if mesh can be GPU instanced
               bool canInstance = data->Mesh->EnableInstancing && 
                                  !meshPtr->Dynamic && 
                                  !data->RenderOverrides &&
                                  !data->Mesh->RenderOnTop;
               
               // Track skinning component for per-entity bone binding
               if (meshInput.Skinning) {
                  base.skinning = meshInput.Skinning;
                  base.skeleton = meshInput.Skeleton;
                  base.blendShapes = data->BlendShapes.get();
                  canInstance = false;
                  const bool hasBlendShapes =
                     data->BlendShapes && !data->BlendShapes->Shapes.empty();
                  base.canSkinnedInstance =
                     meshInput.Skinning->HasGpuSkinningInstanceRecord() &&
                     (!meshPtr->Dynamic ||
                      bgfx::isValid(meshPtr->dvbh) ||
                      (hasBlendShapes &&
                       ShouldUseGpuMorphTargets(meshInput.Skinning, meshPtr, data->BlendShapes.get())));
               }
               base.canInstance = canInstance;
               
               {
                  glm::vec3 worldPos = glm::vec3(data->Transform.WorldMatrix[3]);
                  glm::vec3 d = worldPos - cameraPosition;
                  base.sortDepth = glm::dot(d, d);
               }
               base.isTransparent = base.material ? ((base.material->GetStateFlags() & BGFX_STATE_BLEND_ALPHA) != 0) : false;
               auto applyRenderOverrides = [&](DrawItem& item) {
                  if (!data->RenderOverrides) return;
                  const RenderOverridesComponent& ro = *data->RenderOverrides;
                  if (ro.ShowOnTop) item.renderOnTop = true;
                  item.receiveShadows = ro.ReceiveShadows;
                  if (ro.AlphaBlendEnabled) {
                     item.alphaOverride = true;
                     item.alphaEnable = true;
                     item.isTransparent = true;
                     item.alphaCutoutOverride = false;
                  }
                  if (ro.UseAlphaCutout) {
                     item.alphaCutoutOverride = true;
                     item.alphaCutoutThreshold = ro.AlphaCutoutThreshold;
                     item.alphaOverride = true;
                     item.alphaEnable = false;
                     item.isTransparent = false;
                  }
                  if (!ro.DepthWriteEnabled) {
                     item.depthWriteOverride = true;
                     item.depthWriteEnable = ro.DepthWriteEnabled;
                  }
                  if (!ro.DepthTestEnabled) {
                     item.depthTestOverride = true;
                     item.depthTestEnable = ro.DepthTestEnabled;
                  }
                  if (item.renderOnTop) {
                     item.canInstance = false;
                     item.canSkinnedInstance = false;
                  }
               };
               applyRenderOverrides(base);

               if (!meshPtr->Submeshes.empty() && !data->Mesh->materials.empty()) {
                  for (const auto& sm : meshPtr->Submeshes) {
                     DrawItem di = base;
                     const size_t slot = sm.materialSlot < data->Mesh->materials.size() ? sm.materialSlot : 0;
                     di.material = data->Mesh->materials[slot] ? data->Mesh->materials[slot].get() : data->Mesh->material.get();
                     di.isTransparent = di.material ? ((di.material->GetStateFlags() & BGFX_STATE_BLEND_ALPHA) != 0) : base.isTransparent;
                     di.indexStart = sm.indexStart;
                     di.indexCount = sm.indexCount;
                     MaterialPropertyBlockStack stack;
                     stack.MeshBlock = &data->Mesh->PropertyBlock;
                     if (sm.materialSlot < data->Mesh->SlotPropertyBlocks.size()) {
                        const MaterialPropertyBlock& slotPB = data->Mesh->SlotPropertyBlocks[sm.materialSlot];
                        if (!slotPB.Empty()) stack.SlotBlock = &slotPB;
                     }
                     // Proxy overrides (material + property block)
                     if (sm.materialSlot < data->Mesh->SubmeshOwners.size()) {
                        EntityID proxyId = data->Mesh->SubmeshOwners[sm.materialSlot];
                        if (proxyId != INVALID_ENTITY_ID) {
                           auto* proxyData = scene.GetEntityData(proxyId);
                           if (proxyData && proxyData->MeshProxy) {
                              const auto& proxyComp = *proxyData->MeshProxy;
                              auto it = proxyComp.SlotIndexLookup.find(sm.materialSlot);
                              if (it != proxyComp.SlotIndexLookup.end()) {
                                 size_t localIndex = it->second;
                                 if (localIndex < proxyComp.SlotMaterialOverrides.size()) {
                                    const auto& overrideMat = proxyComp.SlotMaterialOverrides[localIndex];
                                    if (overrideMat) {
                                       di.material = overrideMat.get();
                                       di.isTransparent = di.material ? ((di.material->GetStateFlags() & BGFX_STATE_BLEND_ALPHA) != 0) : di.isTransparent;
                                    }
                                 }
                                 if (localIndex < proxyComp.SlotPropertyBlocks.size()) {
                                    const MaterialPropertyBlock& proxySlotBlock = proxyComp.SlotPropertyBlocks[localIndex];
                                    if (!proxySlotBlock.Empty()) stack.ProxyBlock = &proxyComp.SlotPropertyBlocks[localIndex];
                                 }
                              }
                              if (!stack.ProxyBlock && !proxyComp.PropertyBlock.Empty()) {
                                 stack.ProxyBlock = &proxyComp.PropertyBlock;
                              }
                           }
                        }
                     }
                     di.propertyBlocks = stack;
                     applyRenderOverrides(di);
                     localDrawItems.push_back(di);
                  }
               } else {
                  DrawItem di = base;
                  di.indexStart = 0;
                  di.indexCount = meshPtr->numIndices;
                  MaterialPropertyBlockStack stack = base.propertyBlocks;
                  if (!data->Mesh->SlotPropertyBlocks.empty()) {
                     const MaterialPropertyBlock& slotPB = data->Mesh->SlotPropertyBlocks[0];
                     if (!slotPB.Empty()) stack.SlotBlock = &slotPB;
                  }
                  di.propertyBlocks = stack;
                  applyRenderOverrides(di);
                  localDrawItems.push_back(di);
               }
            }
            
            const size_t chunkIndex = start / chunkSize;
            if (chunkIndex < chunkResults.size()) {
               chunkResults[chunkIndex] = std::move(localDrawItems);
            }
         },
         JobSystem::Priority::High);
      
      // Merge chunk results sequentially without worker-side mutex contention.
      size_t totalDrawItems = 0;
      for (const auto& chunkResult : chunkResults) {
         totalDrawItems += chunkResult.size();
      }
      if (m_ScratchDraws.capacity() < totalDrawItems) {
         m_ScratchDraws.reserve(totalDrawItems);
      }
      for (auto& chunkResult : chunkResults) {
         m_ScratchDraws.insert(
            m_ScratchDraws.end(),
            std::make_move_iterator(chunkResult.begin()),
            std::make_move_iterator(chunkResult.end()));
      }
   } else {
      // Sequential fallback (original code path)
      // Gather
      for (size_t entityIndex = 0; entityIndex < mainViewMeshInputs.size(); ++entityIndex) {
      const MainViewMeshInput& meshInput = mainViewMeshInputs[entityIndex];
      EntityID eid = meshInput.Entity;
      auto* data = meshInput.Data;
      if (!IsPresentationVisible(data) || !data->Active || !data->Mesh || !data->Mesh->mesh) continue;
      Mesh* meshPtr = meshInput.MeshAsset;  // PERFORMANCE: raw ptr, guaranteed valid this frame
      if (!meshPtr) continue;

      DrawItem base{};
      base.mesh = meshPtr;  // PERFORMANCE: raw ptr instead of shared_ptr copy
      base.material = data->Mesh->material.get();  // PERFORMANCE: raw ptr, no ref-count
      base.propertyBlocks.MeshBlock = &data->Mesh->PropertyBlock;
      base.entityId = eid;
      memcpy(base.transform, glm::value_ptr(data->Transform.WorldMatrix), sizeof(float) * 16);
      base.renderOnTop = data->Mesh->RenderOnTop;
      base.renderOrder = data->Mesh->RenderOrder;
      base.showBackfaces = data->Mesh->ShowBackfaces;
      
      // PERF: Check if mesh can be GPU instanced
      // Must have EnableInstancing and no per-entity render override state.
      bool canInstance = data->Mesh->EnableInstancing && 
                         !meshPtr->Dynamic && 
                         !data->RenderOverrides &&
                         !data->Mesh->RenderOnTop;
      
      // Track skinning metadata for atlas-driven GPU skinning.
      // Each skinned mesh contributes draw-time remap/correction metadata while
      // the shared skeleton pose itself is uploaded once per skeleton.
      // NOTE: Check both SkeletonRoot (direct binding) and ResolvedSkeletonRoot
      // (for UseParentSkeleton armor meshes that inherit from parent skeleton)
      if (meshInput.Skinning) {
         base.skinning = meshInput.Skinning;
         base.skeleton = meshInput.Skeleton;
         base.blendShapes = data->BlendShapes.get();
         canInstance = false;  // Skinned meshes cannot be instanced
         const bool hasBlendShapes =
            data->BlendShapes && !data->BlendShapes->Shapes.empty();
         base.canSkinnedInstance =
            meshInput.Skinning->HasGpuSkinningInstanceRecord() &&
            (!meshPtr->Dynamic ||
             bgfx::isValid(meshPtr->dvbh) ||
             (hasBlendShapes &&
              ShouldUseGpuMorphTargets(meshInput.Skinning, meshPtr, data->BlendShapes.get())));
      }
      base.canInstance = canInstance;
      
      {
         glm::vec3 worldPos = glm::vec3(data->Transform.WorldMatrix[3]);
         glm::vec3 d = worldPos - cameraPosition;
         base.sortDepth = glm::dot(d, d);
      }
      base.isTransparent = base.material ? ((base.material->GetStateFlags() & BGFX_STATE_BLEND_ALPHA) != 0) : false;
      auto applyRenderOverrides = [&](DrawItem& item) {
         if (!data->RenderOverrides) return;
         const RenderOverridesComponent& ro = *data->RenderOverrides;
         if (ro.ShowOnTop) item.renderOnTop = true;
         item.receiveShadows = ro.ReceiveShadows;
         if (ro.AlphaBlendEnabled) {
            item.alphaOverride = true;
            item.alphaEnable = true;
            item.isTransparent = true;
            // Explicit blend overrides cutout
            item.alphaCutoutOverride = false;
         }
         if (ro.UseAlphaCutout) {
            item.alphaCutoutOverride = true;
            item.alphaCutoutThreshold = ro.AlphaCutoutThreshold;
            // Cutout is opaque; disable blending even if material requested it
            item.alphaOverride = true;
            item.alphaEnable = false;
            item.isTransparent = false;
         }
         if (!ro.DepthWriteEnabled) {
            item.depthWriteOverride = true;
            item.depthWriteEnable = ro.DepthWriteEnabled;
         }
         if (!ro.DepthTestEnabled) {
            item.depthTestOverride = true;
            item.depthTestEnable = ro.DepthTestEnabled;
         }
         if (item.renderOnTop) {
            item.canInstance = false;
            item.canSkinnedInstance = false;
         }
      };
      applyRenderOverrides(base);

      if (!meshPtr->Submeshes.empty() && !data->Mesh->materials.empty()) {
         for (const auto& sm : meshPtr->Submeshes) {
            DrawItem di = base;
            const size_t slot = sm.materialSlot < data->Mesh->materials.size() ? sm.materialSlot : 0;
            // PERFORMANCE: use raw ptr instead of shared_ptr
            di.material = data->Mesh->materials[slot] ? data->Mesh->materials[slot].get() : data->Mesh->material.get();
            // Recompute transparency based on the actual material bound for this submesh
            di.isTransparent = di.material ? ((di.material->GetStateFlags() & BGFX_STATE_BLEND_ALPHA) != 0) : base.isTransparent;
            di.indexStart = sm.indexStart;
            di.indexCount = sm.indexCount;
            MaterialPropertyBlockStack stack;
            stack.MeshBlock = &data->Mesh->PropertyBlock;
            if (sm.materialSlot < data->Mesh->SlotPropertyBlocks.size()) {
               const MaterialPropertyBlock& slotPB = data->Mesh->SlotPropertyBlocks[sm.materialSlot];
               if (!slotPB.Empty()) stack.SlotBlock = &slotPB;
            }
            // Proxy overrides (material + property block)
            if (sm.materialSlot < data->Mesh->SubmeshOwners.size()) {
               EntityID proxyId = data->Mesh->SubmeshOwners[sm.materialSlot];
               if (proxyId != INVALID_ENTITY_ID) {
                  auto* proxyData = scene.GetEntityData(proxyId);
                  if (proxyData && proxyData->MeshProxy) {
                     const auto& proxyComp = *proxyData->MeshProxy;
                     auto it = proxyComp.SlotIndexLookup.find(sm.materialSlot);
                     if (it != proxyComp.SlotIndexLookup.end()) {
                        size_t localIndex = it->second;
                        if (localIndex < proxyComp.SlotMaterialOverrides.size()) {
                           const auto& overrideMat = proxyComp.SlotMaterialOverrides[localIndex];
                           if (overrideMat) {
                              di.material = overrideMat.get();  // PERFORMANCE: raw ptr
                              di.isTransparent = di.material ? ((di.material->GetStateFlags() & BGFX_STATE_BLEND_ALPHA) != 0) : di.isTransparent;
                           }
                        }
                        if (localIndex < proxyComp.SlotPropertyBlocks.size()) {
                           const MaterialPropertyBlock& proxySlotBlock = proxyComp.SlotPropertyBlocks[localIndex];
                           if (!proxySlotBlock.Empty()) stack.ProxyBlock = &proxyComp.SlotPropertyBlocks[localIndex];
                        }
                     }
                     if (!stack.ProxyBlock && !proxyComp.PropertyBlock.Empty()) {
                        stack.ProxyBlock = &proxyComp.PropertyBlock;
                     }
                  }
               }
            }
            di.propertyBlocks = stack;
            applyRenderOverrides(di);
            m_ScratchDraws.push_back(di);  // PERFORMANCE: use scratch buffer
         }
      } else {
         DrawItem di = base;
         di.indexStart = 0;
         di.indexCount = meshPtr->numIndices;
         MaterialPropertyBlockStack stack = base.propertyBlocks;
         if (!data->Mesh->SlotPropertyBlocks.empty()) {
            const MaterialPropertyBlock& slotPB = data->Mesh->SlotPropertyBlocks[0];
            if (!slotPB.Empty()) stack.SlotBlock = &slotPB;
         }
         di.propertyBlocks = stack;
         applyRenderOverrides(di);
         m_ScratchDraws.push_back(di);  // PERFORMANCE: use scratch buffer
      }
   }
   }
  }

  { ScopedTimer t("Render/MeshFinalize");
  if (m_MaterialInstanceCache.size() < m_ScratchDraws.size()) {
     m_MaterialInstanceCache.reserve(m_ScratchDraws.size());
  }
  for (DrawItem& di : m_ScratchDraws) {
     di.materialInstance = resolveMaterialInstanceBinding(di.material, di.propertyBlocks);
     if (!di.alphaOverride) {
        glm::vec4 scalar1(0.0f);
        bool hasMaterialCutout = false;
        if (di.materialInstance &&
            di.materialInstance->HasPBRScalar1 &&
            di.materialInstance->PBRScalar1.y > 0.001f) {
           scalar1 = di.materialInstance->PBRScalar1;
           hasMaterialCutout = true;
        } else if (di.material && di.material->TryGetUniform(pbrScalar1Id, scalar1) && scalar1.y > 0.001f) {
           hasMaterialCutout = true;
        }
        if (hasMaterialCutout) {
           di.alphaCutoutOverride = true;
           di.alphaCutoutThreshold = scalar1.y;
           di.alphaOverride = true;
           di.alphaEnable = false;
           di.isTransparent = false;
        }
     }
     applyInstancingVariation(di);
     di.resolvedStateFlags = resolveDrawStateFlags(di);
     const MaterialBindingKeyInfo bindingInfo = getMaterialBindingKeyInfo(di.material);
     di.materialBindingKey = bindingInfo.key;
     di.allowEquivalentBindCache = bindingInfo.equivalentSafe && di.materialInstance == nullptr;
  }
  }

  // Split opaque/transparent
  // PERFORMANCE: Use scratch buffers instead of per-frame allocation
  { ScopedTimer t("Render/MeshSort");
  m_ScratchOpaque.clear();
  m_ScratchTransparent.clear();
  m_ScratchNonInstanced.clear();
  if (m_ScratchOpaque.capacity() < m_ScratchDraws.size()) m_ScratchOpaque.reserve(m_ScratchDraws.size());
  if (m_ScratchTransparent.capacity() < m_ScratchDraws.size()) m_ScratchTransparent.reserve(m_ScratchDraws.size());
  for (const auto& di : m_ScratchDraws) {
     if (di.isTransparent) m_ScratchTransparent.push_back(di); else m_ScratchOpaque.push_back(di);
  }
  auto sortCommon = [](const DrawItem& a, const DrawItem& b){
     if (a.renderOnTop != b.renderOnTop) return a.renderOnTop < b.renderOnTop; // non-overlay first
     if (a.renderOrder != b.renderOrder) return a.renderOrder < b.renderOrder;
     // Group by skeleton first to minimize bone uniform uploads (skinned mesh optimization)
     if (a.skeleton != b.skeleton) return a.skeleton < b.skeleton;
     const uint16_t ap = a.material ? a.material->GetProgram().idx : bgfx::kInvalidHandle;
     const uint16_t bp = b.material ? b.material->GetProgram().idx : bgfx::kInvalidHandle;
     if (ap != bp) return ap < bp;
     if (a.resolvedStateFlags != b.resolvedStateFlags) return a.resolvedStateFlags < b.resolvedStateFlags;
     if (a.materialBindingKey != b.materialBindingKey) return a.materialBindingKey < b.materialBindingKey;
     if (a.material != b.material) return a.material < b.material;
     auto av = a.mesh->Dynamic ? a.mesh->dvbh.idx : a.mesh->vbh.idx;
     auto bv = b.mesh->Dynamic ? b.mesh->dvbh.idx : b.mesh->vbh.idx;
     if (av != bv) return av < bv;
     return a.mesh->ibh.idx < b.mesh->ibh.idx;
  };
  std::sort(m_ScratchOpaque.begin(), m_ScratchOpaque.end(), sortCommon);
  std::sort(m_ScratchTransparent.begin(), m_ScratchTransparent.end(), [&](const DrawItem& a, const DrawItem& b){
     if (a.renderOnTop != b.renderOnTop) return a.renderOnTop < b.renderOnTop;
     if (a.renderOrder != b.renderOrder) return a.renderOrder < b.renderOrder;
     if (a.sortDepth != b.sortDepth) return a.sortDepth > b.sortDepth; // back-to-front
     return sortCommon(a, b);
  });
  Profiler::Get().SetCounter("Render/SubmeshPackets", static_cast<uint64_t>(m_ScratchDraws.size()));
  Profiler::Get().SetCounter("Render/MaterialInstanceCacheHits", materialInstanceCacheHitCount);
  Profiler::Get().SetCounter("Render/MaterialInstanceRebuilds", materialInstanceRebuildCount);
  Profiler::Get().SetCounter("Render/MaterialInstanceDirtyUploads", materialInstanceUploadCount);
  Profiler::Get().SetCounter("Render/MaterialInstanceCacheEntries", static_cast<uint64_t>(m_MaterialInstanceCache.size()));
  }

  std::unordered_map<const Material*, float> materialShadowReceiveCache;
  materialShadowReceiveCache.reserve(256);
  auto getMaterialShadowReceive = [&](const Material* mat) -> float {
     if (!mat) {
        return 1.0f;
     }
     auto it = materialShadowReceiveCache.find(mat);
     if (it != materialShadowReceiveCache.end()) {
        return it->second;
     }
     float receive = 1.0f;
     if (auto pbr = dynamic_cast<const PBRMaterial*>(mat)) {
        if (pbr->GetReceiveShadowsOverride()) {
           receive = pbr->GetReceiveShadows() ? 1.0f : 0.0f;
        }
     }
     materialShadowReceiveCache.emplace(mat, receive);
     return receive;
  };
  std::unordered_map<const Material*, float> materialEmissionStrengthCache;
  materialEmissionStrengthCache.reserve(256);
  auto getMaterialEmissionStrength = [&](const Material* mat) -> float {
     if (!mat) {
        return 0.0f;
     }
     auto it = materialEmissionStrengthCache.find(mat);
     if (it != materialEmissionStrengthCache.end()) {
        return it->second;
     }
     float emission = 0.0f;
     if (auto pbr = dynamic_cast<const PBRMaterial*>(mat)) {
        emission = pbr->GetEmissionStrength();
     }
     materialEmissionStrengthCache.emplace(mat, emission);
     return emission;
  };
  std::shared_ptr<Material> fallbackStaticMaterial;
  std::shared_ptr<Material> fallbackSkinnedMaterial;
  size_t fallbackMaterialsCreated = 0;
  size_t fallbackMaterialUses = 0;
  auto resolveProgramMaterial = [&](Material* source, bool needsSkinned) -> Material* {
     Material* resolved = ResolveMaterialWithValidProgram(
        scene,
        source,
        needsSkinned,
        fallbackStaticMaterial,
        fallbackSkinnedMaterial,
        &fallbackMaterialsCreated);
     if (resolved && resolved != source) {
        ++fallbackMaterialUses;
     }
     return resolved;
  };
  auto applyMaterialInstanceBinding = [&](Material* material, const MaterialInstanceBinding* binding) {
     if (!material || !binding) {
        return;
     }
     if (!binding->MeshOverrides.Empty()) {
        material->ApplyPackedPropertyOverrides(binding->MeshOverrides);
     }
     if (!binding->SlotOverrides.Empty()) {
        material->ApplyPackedPropertyOverrides(binding->SlotOverrides);
     }
     if (!binding->ProxyOverrides.Empty()) {
        material->ApplyPackedPropertyOverrides(binding->ProxyOverrides);
     }
  };

  // =========================================================================
  // PERF: GPU Instancing for opaque meshes with EnableInstancing flag
  // Group eligible meshes by mesh+material and submit as instanced batches
  // =========================================================================
  
  // IMPORTANT: Reset u_PBRScalar1 to ensure alpha cutout is disabled by default
  // This prevents any leftover state from the previous frame's instancer rendering
  // from affecting non-instanced meshes that don't explicitly set this uniform.
  {
      static bgfx::UniformHandle s_u_PBRScalar1 = bgfx::createUniform("u_PBRScalar1", bgfx::UniformType::Vec4);
      glm::vec4 defaultPBRScalar1(0.0f, 0.0f, 0.0f, 0.0f);  // y=0 disables alpha cutout
      bgfx::setUniform(s_u_PBRScalar1, &defaultPBRScalar1);
  }
  
  { ScopedTimer t("Render/MeshSubmit");
  m_InstanceManager.BeginFrame();
  m_ScratchNonInstanced.clear();
  const bgfx::ProgramHandle skinnedPbrProgram =
     ShaderManager::Instance().LoadProgram("vs_pbr_skinned", "fs_pbr_skinned");
  auto resolveSkinnedColorPath = [&](const DrawItem& di,
                                     Material* drawMat,
                                     bgfx::VertexBufferHandle& outVertexBuffer,
                                     bgfx::ProgramHandle& outInstancedProgram,
                                     bgfx::ProgramHandle& outSingleProgram,
                                     bool& outUsesGpuMorph,
                                     bool& outUsesMaterializedSkinning) -> bool {
     outVertexBuffer = BGFX_INVALID_HANDLE;
     outInstancedProgram = BGFX_INVALID_HANDLE;
     outSingleProgram = BGFX_INVALID_HANDLE;
     outUsesGpuMorph = false;
     outUsesMaterializedSkinning = false;

     if (!drawMat ||
         !di.skinning ||
         !di.mesh ||
         !di.skinning->HasGpuSkinningInstanceRecord() ||
         !bgfx::isValid(di.mesh->ibh)) {
        return false;
     }

     const bgfx::ProgramHandle baseProgram = drawMat->GetProgram();
     if (!bgfx::isValid(baseProgram)) {
        return false;
     }

     if (TryGetGpuMaterializedSkinnedVertexBuffer(scene, di.entityId, di.mesh, outVertexBuffer) &&
         ResolveGpuMaterializedSkinnedColorProgram(baseProgram, outSingleProgram)) {
        outUsesMaterializedSkinning = true;
        return bgfx::isValid(outVertexBuffer) && bgfx::isValid(outSingleProgram);
     }

     if (baseProgram.idx != skinnedPbrProgram.idx) {
        return false;
     }

     if (di.mesh->Dynamic) {
        if (!di.canSkinnedInstance) {
           return false;
        }
        if (ShouldUseGpuMorphTargets(di.skinning, di.mesh, di.blendShapes)) {
           outVertexBuffer = GetOrCreateGpuMorphVertexBuffer(di.mesh);
           outInstancedProgram = m_PBRSkinnedMorphInstancedProgram;
           outSingleProgram = m_PBRSkinnedMorphProgram;
           outUsesGpuMorph = true;
        } else {
           return false;
        }
        return bgfx::isValid(outVertexBuffer) &&
           bgfx::isValid(outInstancedProgram) &&
           bgfx::isValid(outSingleProgram);
     }

     outVertexBuffer = di.mesh->vbh;
     outInstancedProgram = m_PBRSkinnedInstancedProgram;
     outSingleProgram = baseProgram;
     return bgfx::isValid(outVertexBuffer) && bgfx::isValid(outInstancedProgram);
  };
  uint64_t skinnedColorBatchCandidates = 0;
  uint64_t skinnedColorBatchedInstances = 0;
  uint64_t skinnedColorGpuMorphBatchedInstances = 0;
  
  for (const auto& di : m_ScratchOpaque) {
     if (!di.material || !di.mesh) continue;

     if (di.canSkinnedInstance &&
         !di.isTransparent &&
         di.skinning != nullptr &&
         di.mesh != nullptr) {
        Material* drawMat = resolveProgramMaterial(di.material, true);
        if (!drawMat) {
           m_ScratchNonInstanced.push_back(di);
           continue;
        }

        bgfx::VertexBufferHandle resolvedVbh = BGFX_INVALID_HANDLE;
        bgfx::ProgramHandle instancedProgram = BGFX_INVALID_HANDLE;
        bgfx::ProgramHandle singleProgram = BGFX_INVALID_HANDLE;
        bool usesGpuMorph = false;
        bool usesMaterializedSkinning = false;
        if (!resolveSkinnedColorPath(
               di,
               drawMat,
               resolvedVbh,
               instancedProgram,
               singleProgram,
               usesGpuMorph,
               usesMaterializedSkinning)) {
           m_ScratchNonInstanced.push_back(di);
           continue;
        }
        if (!bgfx::isValid(instancedProgram)) {
           m_ScratchNonInstanced.push_back(di);
           continue;
        }

        cm::rendering::SkinnedInstanceData instance{};
        instance.SetTransform(di.transform);
        instance.SetMetadata(static_cast<float>(di.skinning->GpuInstanceAtlasRecordIndex));

        cm::rendering::InstanceKey key;
        key.vbh = resolvedVbh;
        key.ibh = di.mesh->ibh;
        key.program = instancedProgram;
        key.indexStart = di.indexStart;
        key.indexCount = di.indexCount;
        key.variationHash = HashCombine64(di.materialBindingKey, di.instancingVariationHash);

        const uint64_t stateFlags = di.resolvedStateFlags;
        auto& batch = m_InstanceManager.GetBatch(key, stateFlags, static_cast<uint16_t>(sizeof(instance)));
        batch.AddInstance(instance);
        ++skinnedColorBatchCandidates;
        ++skinnedColorBatchedInstances;
        if (usesGpuMorph) {
           ++skinnedColorGpuMorphBatchedInstances;
        }

        if (batch.instanceCount == 1u) {
           Material* mat = drawMat;
           const float receive = di.instancingHasShadowReceive
              ? glm::clamp(di.instancingShadowReceive, 0.0f, 1.0f)
              : getMaterialShadowReceive(mat);
           const bool hasPBRScalar1 = di.instancingHasPBRScalar1;
           const glm::vec4 pbrScalar1Value = di.instancingPBRScalar1;
           const bgfx::VertexBufferHandle vbh = resolvedVbh;
           const bgfx::IndexBufferHandle ibh = di.mesh->ibh;
           const uint32_t indexStart = di.indexStart;
           const uint32_t indexCount = di.indexCount;
           const SkinningComponent* skinning = di.skinning;
           const bgfx::ProgramHandle submitSingleProgram = instancedProgram;
           const char* submitSingleDebugName = usesGpuMorph
              ? "vs_pbr_skinned_morph_instanced+fs_pbr_skinned"
              : "vs_pbr_skinned_instanced+fs_pbr_skinned";
           const bgfx::ProgramHandle fallbackSingleProgram = singleProgram;
           const char* fallbackSingleDebugName = usesGpuMorph
              ? "vs_pbr_skinned_morph+fs_pbr_skinned"
              : "vs_pbr_skinned+fs_pbr_skinned";
           batch.bindBatch = [this, mat, receive, hasPBRScalar1, pbrScalar1Value]() {
              mat->BindUniforms();
              BindShadowUniforms();
              BindGpuSkinningAtlasGlobals();
              if (hasPBRScalar1) {
                 static bgfx::UniformHandle s_u_PBRScalar1 = bgfx::createUniform("u_PBRScalar1", bgfx::UniformType::Vec4);
                 bgfx::setUniform(s_u_PBRScalar1, &pbrScalar1Value);
              }
              glm::vec4 receiveVec(receive, 0.0f, 0.0f, 0.0f);
              bgfx::setUniform(u_ShadowReceive, &receiveVec);
              GlobalShaderProperties::Instance().Apply();
           };
           batch.submitSingle = [this, mat, skinning, usesGpuMorph, submitSingleProgram, submitSingleDebugName, fallbackSingleProgram, fallbackSingleDebugName, receive, hasPBRScalar1, pbrScalar1Value, stateFlags, vbh, ibh, indexStart, indexCount](uint16_t viewId, const uint8_t* instanceBytes) {
              const auto* inst = reinterpret_cast<const cm::rendering::SkinnedInstanceData*>(instanceBytes);
              if (!inst) {
                 return;
              }

              if (SubmitSingleSkinnedInstance(
                 viewId,
                 vbh,
                 ibh,
                 submitSingleProgram,
                 *inst,
                 stateFlags,
                 submitSingleDebugName,
                 [this, mat, receive, hasPBRScalar1, pbrScalar1Value]() {
                    mat->BindUniforms();
                    BindShadowUniforms();
                    BindGpuSkinningAtlasGlobals();
                    if (hasPBRScalar1) {
                       static bgfx::UniformHandle s_u_PBRScalar1 = bgfx::createUniform("u_PBRScalar1", bgfx::UniformType::Vec4);
                       bgfx::setUniform(s_u_PBRScalar1, &pbrScalar1Value);
                    }
                    glm::vec4 receiveVec(receive, 0.0f, 0.0f, 0.0f);
                    bgfx::setUniform(u_ShadowReceive, &receiveVec);
                    GlobalShaderProperties::Instance().Apply();
                 },
                 indexStart,
                 indexCount)) {
                 return;
              }

              bgfx::setTransform(inst->transform);
              bgfx::setVertexBuffer(0, vbh);
              bgfx::setIndexBuffer(ibh, indexStart, indexCount);
              SetNormalMatrixUniform(inst->transform);
              if (usesGpuMorph) {
                 BindSkinningInstanceRecord(static_cast<uint32_t>(glm::max(inst->metadata[0], 0.0f)));
              } else if (skinning) {
                 Renderer::SkinningBindCacheState singleSkinningCache{};
                 BindSkinningIfChanged(skinning, singleSkinningCache);
              }
              mat->BindUniforms();
              BindShadowUniforms();
              if (hasPBRScalar1) {
                 static bgfx::UniformHandle s_u_PBRScalar1 = bgfx::createUniform("u_PBRScalar1", bgfx::UniformType::Vec4);
                 bgfx::setUniform(s_u_PBRScalar1, &pbrScalar1Value);
              }
              glm::vec4 receiveVec(receive, 0.0f, 0.0f, 0.0f);
              bgfx::setUniform(u_ShadowReceive, &receiveVec);
              GlobalShaderProperties::Instance().Apply();
              bgfx::setState(stateFlags);
              SafeSubmit(viewId, fallbackSingleProgram, fallbackSingleDebugName);
           };
        }
        continue;
     }
     
     // Only instance if the mesh explicitly opts in and has no per-entity variations
     if (di.canInstance && !di.isTransparent) {
        Material* drawMat = resolveProgramMaterial(di.material, di.skinning != nullptr);
        if (!drawMat) {
           m_ScratchNonInstanced.push_back(di);
           continue;
        }
        bgfx::ProgramHandle prog = drawMat->GetProgram();
        if (!bgfx::isValid(prog)) {
           m_ScratchNonInstanced.push_back(di);
           continue;
        }
        
        cm::rendering::InstanceKey key;
        key.vbh = di.mesh->vbh;
        key.ibh = di.mesh->ibh;
        key.program = prog;
        key.indexStart = di.indexStart;
        key.indexCount = di.indexCount;
        key.variationHash = HashCombine64(di.materialBindingKey, di.instancingVariationHash);
        
        uint64_t stateFlags = di.resolvedStateFlags;
        auto& batch = m_InstanceManager.GetBatch(key, stateFlags, static_cast<uint16_t>(sizeof(cm::rendering::InstanceData)));
        batch.AddInstance(di.transform);
        
        // Capture material binding for this batch (only need to do once per batch)
        if (batch.instanceCount == 1u) {
           Material* mat = drawMat;
           const float receive = di.instancingHasShadowReceive
              ? glm::clamp(di.instancingShadowReceive, 0.0f, 1.0f)
              : getMaterialShadowReceive(mat);
           const bool hasPBRScalar1 = di.instancingHasPBRScalar1;
           const glm::vec4 pbrScalar1Value = di.instancingPBRScalar1;
           batch.bindBatch = [this, mat, receive, hasPBRScalar1, pbrScalar1Value]() {
              mat->BindUniforms();
              BindShadowUniforms();
              if (hasPBRScalar1) {
                 static bgfx::UniformHandle s_u_PBRScalar1 = bgfx::createUniform("u_PBRScalar1", bgfx::UniformType::Vec4);
                 bgfx::setUniform(s_u_PBRScalar1, &pbrScalar1Value);
              }
              glm::vec4 receiveVec(receive, 0.0f, 0.0f, 0.0f);
              bgfx::setUniform(u_ShadowReceive, &receiveVec);
              GlobalShaderProperties::Instance().Apply();
           };
        }
     } else {
        m_ScratchNonInstanced.push_back(di);
     }
  }
  
  // Submit instanced batches first (opaque only)
  m_InstanceManager.Submit(1);
  Profiler::Get().SetCounter("Render/SkinnedColorBatchCandidates", skinnedColorBatchCandidates);
  Profiler::Get().SetCounter("Render/SkinnedColorBatchedInstances", skinnedColorBatchedInstances);
  Profiler::Get().SetCounter("Render/SkinnedColorGpuMorphBatchedInstances", skinnedColorGpuMorphBatchedInstances);

  // Submit non-instanced items (opaque)
  uint64_t submittedDrawCount = 0;
  uint64_t materialBindCallCount = 0;
  uint64_t materialBindSkipCount = 0;
  uint64_t skinningBindCandidateCount = 0;
  uint64_t skinnedColorGpuMorphSingleDraws = 0;
  auto submitList = [&](const std::vector<DrawItem>& list){
  struct BindSignature {
      const Material* material = nullptr;
      uint64_t materialBindingKey = 0;
      bool equivalentBinding = false;
      uint16_t programIdx = bgfx::kInvalidHandle;
      uint64_t resolvedStateFlags = 0;
      uint16_t viewId = 1;
      bool valid = false;
  };
  struct GeometrySignature {
      const Mesh* mesh = nullptr;
      bool dynamic = false;
      uint16_t vertexBufferIdx = bgfx::kInvalidHandle;
      uint16_t indexBufferIdx = bgfx::kInvalidHandle;
      uint32_t dynamicVertexCount = 0;
      uint32_t indexStart = 0;
      uint32_t indexCount = 0;
      bool valid = false;
  };
  auto canCacheMaterialBinding = [&](const DrawItem& di, bool hasPropertyBlocks) -> bool {
      if (!enableMaterialBindCache) return false;
      if (!di.material) return false;
      if (hasPropertyBlocks) return false;
      if (di.alphaCutoutOverride) return false;
      // Conservative path: only trust explicit per-material cache safety.
      // Equivalent-binding reuse across distinct material instances can leak stale
      // texture/uniform state (observed as intermittent black meshes).
      return di.material->IsBindCacheSafe();
  };
  const bool enableGeometryStateCache = false;
  BindSignature lastBind{};
  GeometrySignature lastGeometry{};
  Renderer::SkinningBindCacheState lastSkinning{};
  uint32_t consecutiveBindSkips = 0;
  uint32_t consecutiveGeometrySkips = 0;
  constexpr uint32_t kMaxConsecutiveBindSkips = 16;
  constexpr uint32_t kMaxConsecutiveGeometrySkips = 32;
  for (const DrawItem& di : list) {
      if (!di.material) continue;
      Material* drawMat = resolveProgramMaterial(di.material, di.skinning != nullptr);
      if (!drawMat) continue;
      const bool usingFallbackMaterial = (drawMat != di.material);
      bgfx::ProgramHandle prog = drawMat->GetProgram();
      bgfx::VertexBufferHandle resolvedStaticVertexBuffer = BGFX_INVALID_HANDLE;
      bgfx::ProgramHandle ignoredInstancedProgram = BGFX_INVALID_HANDLE;
      bgfx::ProgramHandle resolvedSingleProgram = BGFX_INVALID_HANDLE;
      bool usesGpuMorph = false;
      bool usesMaterializedSkinning = false;
      if (di.skinning &&
          resolveSkinnedColorPath(
             di,
             drawMat,
             resolvedStaticVertexBuffer,
             ignoredInstancedProgram,
             resolvedSingleProgram,
             usesGpuMorph,
             usesMaterializedSkinning)) {
         prog = resolvedSingleProgram;
      } else {
         usesGpuMorph = false;
         usesMaterializedSkinning = false;
         resolvedStaticVertexBuffer = di.mesh->vbh;
      }
      if (!bgfx::isValid(prog)) continue;

      bgfx::setTransform(di.transform);
      const bool meshIsDynamic = di.mesh->Dynamic && !usesGpuMorph && !usesMaterializedSkinning;
      const uint16_t vertexBufferIdx = meshIsDynamic ? di.mesh->dvbh.idx : resolvedStaticVertexBuffer.idx;
      const uint16_t indexBufferIdx = di.mesh->ibh.idx;
      bool geometryChanged =
         !enableGeometryStateCache ||
         !lastGeometry.valid ||
         lastGeometry.mesh != di.mesh ||
         lastGeometry.dynamic != meshIsDynamic ||
         lastGeometry.vertexBufferIdx != vertexBufferIdx ||
         lastGeometry.indexBufferIdx != indexBufferIdx ||
         lastGeometry.indexStart != di.indexStart ||
         lastGeometry.indexCount != di.indexCount ||
         (meshIsDynamic && lastGeometry.dynamicVertexCount != di.mesh->numVertices);
      if (enableGeometryStateCache && !geometryChanged && consecutiveGeometrySkips >= kMaxConsecutiveGeometrySkips) {
         // Periodically refresh VB/IB binding even for identical draws.
         geometryChanged = true;
      }
      if (geometryChanged) {
         if (meshIsDynamic) {
            if (bgfx::isValid(di.mesh->dvbh)) {
               bgfx::setVertexBuffer(0, di.mesh->dvbh, 0, di.mesh->numVertices);
            } else {
               continue;
            }
         } else {
            if (!bgfx::isValid(resolvedStaticVertexBuffer)) {
               continue;
            }
            bgfx::setVertexBuffer(0, resolvedStaticVertexBuffer);
         }
         bgfx::setIndexBuffer(di.mesh->ibh, di.indexStart, di.indexCount);
         consecutiveGeometrySkips = 0;
      } else {
         ++consecutiveGeometrySkips;
      }

      // Normal matrix uniform (handles non-uniform scaling correctly)
      SetNormalMatrixUniform(di.transform);

      // Skinned mesh: bind atlas metadata for this draw.
      if (di.skinning) {
         ++skinningBindCandidateCount;
         if (usesGpuMorph) {
            ++skinnedColorGpuMorphSingleDraws;
            BindSkinningInstanceRecord(di.skinning->GpuInstanceAtlasRecordIndex);
            lastSkinning = {};
         } else if (!usesMaterializedSkinning) {
            BindSkinningIfChanged(di.skinning, lastSkinning);
         }
      }

      const MaterialPropertyBlock* meshBlock =
         (!di.materialInstance && di.propertyBlocks.MeshBlock && !di.propertyBlocks.MeshBlock->Empty()) ? di.propertyBlocks.MeshBlock : nullptr;
      const MaterialPropertyBlock* slotBlock =
         (!di.materialInstance && di.propertyBlocks.SlotBlock && !di.propertyBlocks.SlotBlock->Empty()) ? di.propertyBlocks.SlotBlock : nullptr;
      const MaterialPropertyBlock* proxyBlock =
         (!di.materialInstance && di.propertyBlocks.ProxyBlock && !di.propertyBlocks.ProxyBlock->Empty()) ? di.propertyBlocks.ProxyBlock : nullptr;
      const bool hasPropertyBlocks = di.materialInstance != nullptr || (meshBlock || slotBlock || proxyBlock);
      const bool cacheEligible = !usingFallbackMaterial && canCacheMaterialBinding(di, hasPropertyBlocks);
      const BindSignature currentBind{
         drawMat,
         di.materialBindingKey,
         di.allowEquivalentBindCache,
         prog.idx,
         di.resolvedStateFlags,
         1,
         cacheEligible
      };
      const bool sameBindingIdentity =
         currentBind.equivalentBinding
            ? (lastBind.equivalentBinding && lastBind.materialBindingKey == currentBind.materialBindingKey)
            : (!lastBind.equivalentBinding && lastBind.material == currentBind.material);
      const bool canSkipBindUniforms =
         cacheEligible &&
         lastBind.valid &&
         sameBindingIdentity &&
         lastBind.programIdx == currentBind.programIdx &&
         lastBind.resolvedStateFlags == currentBind.resolvedStateFlags &&
         lastBind.viewId == currentBind.viewId;
      const bool forceRefreshBind = (consecutiveBindSkips >= kMaxConsecutiveBindSkips);
      const bool skipMaterialBind = canSkipBindUniforms && !forceRefreshBind;

      // Bind common uniforms
      if (!skipMaterialBind) {
         drawMat->BindUniforms();
         consecutiveBindSkips = 0;
         ++materialBindCallCount;
      } else {
         ++consecutiveBindSkips;
         ++materialBindSkipCount;
      }
      // Shadow/global bindings are required for every draw submit.
      BindShadowUniforms();
      GlobalShaderProperties::Instance().Apply();
      if (di.materialInstance) {
         applyMaterialInstanceBinding(drawMat, di.materialInstance);
      } else {
         if (meshBlock) {
            drawMat->ApplyPropertyBlock(*meshBlock);
         }
         if (slotBlock) {
            drawMat->ApplyPropertyBlock(*slotBlock);
         }
         if (proxyBlock) {
            drawMat->ApplyPropertyBlock(*proxyBlock);
         }
      }
      {
         float receive = getMaterialShadowReceive(drawMat);
         if (di.materialInstance && di.materialInstance->HasShadowReceive) {
            receive = di.materialInstance->ShadowReceive;
         } else {
            auto applyShadowReceiveOverride = [&](const MaterialPropertyBlock* block) {
               if (!block) return;
               glm::vec4 value;
               if (block->TryGetVector(shadowReceiveId, value)) {
                  receive = value.x;
               }
            };
            applyShadowReceiveOverride(meshBlock);
            applyShadowReceiveOverride(slotBlock);
            applyShadowReceiveOverride(proxyBlock);
         }
         if (!di.receiveShadows) {
            receive = 0.0f;
         }
         receive = glm::clamp(receive, 0.0f, 1.0f);
         glm::vec4 receiveVec(receive, 0.0f, 0.0f, 0.0f);
         bgfx::setUniform(u_ShadowReceive, &receiveVec);
      }
      if (di.alphaCutoutOverride) {
         glm::vec4 scalar1(0.0f);
         if (di.materialInstance && di.materialInstance->HasPBRScalar1) {
            scalar1 = di.materialInstance->PBRScalar1;
         } else {
            scalar1.x = getMaterialEmissionStrength(drawMat);
            auto applyScalarFromBlock = [&](const MaterialPropertyBlock* block) {
               if (!block) return;
               glm::vec4 value;
               if (block->TryGetVector(pbrScalar1Id, value)) {
                  scalar1 = value;
               }
            };
            applyScalarFromBlock(meshBlock);
            applyScalarFromBlock(slotBlock);
            applyScalarFromBlock(proxyBlock);
         }
         scalar1.y = di.alphaCutoutThreshold;
         static bgfx::UniformHandle u_PBRScalar1 = bgfx::createUniform("u_PBRScalar1", bgfx::UniformType::Vec4);
         bgfx::setUniform(u_PBRScalar1, &scalar1);
      }
      bgfx::setState(di.resolvedStateFlags);
      bgfx::submit(1, prog);
      ++submittedDrawCount;
      lastGeometry.mesh = di.mesh;
      lastGeometry.dynamic = meshIsDynamic;
      lastGeometry.vertexBufferIdx = vertexBufferIdx;
      lastGeometry.indexBufferIdx = indexBufferIdx;
      lastGeometry.dynamicVertexCount = di.mesh->numVertices;
      lastGeometry.indexStart = di.indexStart;
      lastGeometry.indexCount = di.indexCount;
      lastGeometry.valid = true;
      if (cacheEligible) {
         lastBind = currentBind;
      } else {
         lastBind.valid = false;
      }
  }
  };
  // Submit non-instanced opaque items, then transparent
  submitList(m_ScratchNonInstanced);
  submitList(m_ScratchTransparent);
  Profiler::Get().SetCounter("Render/SubmittedDraws", submittedDrawCount);
  Profiler::Get().SetCounter("Render/MaterialBindCalls", materialBindCallCount);
  Profiler::Get().SetCounter("Render/MaterialBindSkips", materialBindSkipCount);
  Profiler::Get().SetCounter("Render/SkinningBindCandidates", skinningBindCandidateCount);
  Profiler::Get().SetCounter("Render/SkinnedColorGpuMorphSingleDraws", skinnedColorGpuMorphSingleDraws);
  if (fallbackMaterialUses > 0 && (s_RenderSceneFrameCounter % 120u) == 0u) {
     std::cout << "[Renderer] Material program fallback in use: draws=" << fallbackMaterialUses
               << " created=" << fallbackMaterialsCreated << std::endl;
  }
  }

  // --------------------------------------
  // Draw all terrains (with LOD and frustum culling)
  // --------------------------------------
  { ScopedTimer t("Render/Terrain");
  const Environment& terrainEnv = scene.GetEnvironment();
  const uint32_t terrainSamplerFlags =
      (terrainEnv.TextureFilter == Environment::TextureFilterMode::Point)
          ? (BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT)
          : (BGFX_SAMPLER_MIN_ANISOTROPIC | BGFX_SAMPLER_MAG_ANISOTROPIC);
  const uint32_t terrainDataSamplerFlags =
      BGFX_SAMPLER_MIN_POINT |
      BGFX_SAMPLER_MAG_POINT |
      BGFX_SAMPLER_MIP_POINT |
      BGFX_SAMPLER_U_CLAMP |
      BGFX_SAMPLER_V_CLAMP;

  for (size_t terrainIndex = 0; terrainIndex < m_ScratchTerrainEntityIds.size(); ++terrainIndex)
  {
      EntityID terrainEntityId = m_ScratchTerrainEntityIds[terrainIndex];
      auto* data = terrainIndex < m_ScratchTerrainData.size() ? m_ScratchTerrainData[terrainIndex] : nullptr;
      if (!IsPresentationVisible(data) || !data->Active || !data->Terrain) continue;

      TerrainComponent& terrain = *data->Terrain;
      float terrainReceive = 1.0f;
      if (data->RenderOverrides && !data->RenderOverrides->ReceiveShadows) {
         terrainReceive = 0.0f;
      }
      
      // Initialize or update clipmap system if enabled
      // When UseClipmaps is true, terrain uses geometry clipmaps for efficient LOD
      // This provides smooth LOD transitions without popping
      if (terrain.UseClipmaps && bgfx::isValid(m_ClipmapProgram))
      {
         const float desiredClipmapBaseScale =
            terrain.WorldSize.x / static_cast<float>(std::max(1u, terrain.GridResolution - 1));
         if (terrain.ClipmapSystem)
         {
            auto* existingClipSystem = static_cast<terrain::TerrainClipmapSystem*>(terrain.ClipmapSystem);
            const terrain::ClipmapConfig& existingConfig = existingClipSystem->GetConfig();
            const bool clipmapConfigChanged =
               existingConfig.LevelCount != terrain.ClipmapLevels ||
               existingConfig.GridSize != terrain.ClipmapGridSize ||
               existingConfig.EnableMorphing != terrain.ClipmapMorphing ||
               std::abs(existingConfig.BaseScale - desiredClipmapBaseScale) > 1e-4f;
            if (clipmapConfigChanged)
            {
               Terrain::DestroyClipmapSystem(terrain);
            }
         }

         // Lazy initialize clipmap system
         if (!terrain.ClipmapSystem)
         {
            auto* clipSystem = new terrain::TerrainClipmapSystem();
            terrain::ClipmapConfig config;
            config.LevelCount = terrain.ClipmapLevels;
            config.GridSize = terrain.ClipmapGridSize;
            config.BaseScale = desiredClipmapBaseScale;
            config.EnableMorphing = terrain.ClipmapMorphing;
            clipSystem->Init(config);
            terrain.ClipmapSystem = clipSystem;
         }
         
         // Update clipmap positions based on camera
         auto* clipSystem = static_cast<terrain::TerrainClipmapSystem*>(terrain.ClipmapSystem);
         clipSystem->Update(cameraPosition, terrain.WorldSize);
      }
      else if (!terrain.UseClipmaps && terrain.ClipmapSystem)
      {
         // Clipmaps disabled - clean up the system
         Terrain::DestroyClipmapSystem(terrain);
      }
      
      if (!terrain.UseChunkedTerrain && terrain.ChunkStreamingSystem)
      {
         Terrain::DestroyStreamingSystem(terrain);
         for (TerrainChunk& chunk : terrain.Chunks)
            chunk.StreamState = ChunkStreamState::Resident;
      }
      
      Terrain::PrepareForRendering(terrain);
      
      // Update physics body if terrain height data changed
      if (terrain.PhysicsDirty)
      {
         Terrain::UpdatePhysicsBody(terrain, data->Transform.WorldMatrix, terrainEntityId);
      }
      
      if (!bgfx::isValid(terrain.ChunkVB) || !bgfx::isValid(terrain.ChunkIB))
         continue;

      // Update LOD selection for terrain chunks (distance-based)
      Terrain::UpdateLOD(terrain, cameraPosition, data->Transform.WorldMatrix);
      
      // Perform per-chunk frustum culling and calculate minimum distance for LOD selection
      float minDistance = std::numeric_limits<float>::max();
      for (TerrainChunk& chunk : terrain.Chunks)
      {
         // Default to visible
         chunk.Visible = true;
         
         // Frustum cull each chunk if enabled
         if (doCull)
         {
            // Transform chunk AABB to world space
            const glm::vec3 corners[8] = {
               glm::vec3(chunk.WorldMin.x, chunk.WorldMin.y, chunk.WorldMin.z),
               glm::vec3(chunk.WorldMax.x, chunk.WorldMin.y, chunk.WorldMin.z),
               glm::vec3(chunk.WorldMin.x, chunk.WorldMax.y, chunk.WorldMin.z),
               glm::vec3(chunk.WorldMax.x, chunk.WorldMax.y, chunk.WorldMin.z),
               glm::vec3(chunk.WorldMin.x, chunk.WorldMin.y, chunk.WorldMax.z),
               glm::vec3(chunk.WorldMax.x, chunk.WorldMin.y, chunk.WorldMax.z),
               glm::vec3(chunk.WorldMin.x, chunk.WorldMax.y, chunk.WorldMax.z),
               glm::vec3(chunk.WorldMax.x, chunk.WorldMax.y, chunk.WorldMax.z)
            };
            
            glm::vec3 worldMin(std::numeric_limits<float>::max());
            glm::vec3 worldMax(std::numeric_limits<float>::lowest());
            
            for (int i = 0; i < 8; ++i)
            {
               const glm::vec4 wc = data->Transform.WorldMatrix * glm::vec4(corners[i], 1.0f);
               const glm::vec3 w = glm::vec3(wc);
               worldMin = glm::min(worldMin, w);
               worldMax = glm::max(worldMax, w);
            }
            
            chunk.Visible = AabbIntersectsFrustum(fr, worldMin, worldMax);
         }
         
         if (chunk.Visible)
         {
            minDistance = std::min(minDistance, chunk.DistanceToCamera);
         }
      }
      
      // Select LOD level based on minimum distance (or 0 if LOD disabled)
      int terrainLODLevel = 0;
      if (terrain.LODConfig.Enabled && terrain.LODLevelCount > 0)
      {
         for (uint32_t lod = 0; lod < terrain.LODLevelCount; ++lod)
         {
            if (minDistance < terrain.LODConfig.LODDistances[lod])
            {
               terrainLODLevel = static_cast<int>(lod);
               break;
            }
            terrainLODLevel = static_cast<int>(lod);
         }
      }
      
      // Select the index buffer based on LOD level
      // NOTE: LOD index buffers disabled for now - using original ChunkIB
      // TODO: Debug LOD index buffer generation before re-enabling
      bgfx::IndexBufferHandle activeIB = terrain.ChunkIB;
      // if (terrain.LODConfig.Enabled && 
      //     terrainLODLevel < static_cast<int>(terrain.LODLevelCount) &&
      //     bgfx::isValid(terrain.LODIndexBuffers[terrainLODLevel]))
      // {
      //    activeIB = terrain.LODIndexBuffers[terrainLODLevel];
      // }

      glm::vec2 cellSize = Terrain::GetCellSize(terrain);
      glm::vec4 heightParams(terrain.MaxHeight, cellSize.x, cellSize.y, 0.0f);

      // Rebuild texture arrays if dirty
      if (terrain.LayerTextureArraysDirty)
      {
         BuildTerrainTextureArrays(terrain);
      }

      // Build layer data for all 8 possible layers
      // With texture arrays, hasAlbedo/hasNormal is always 1.0 if we have layers (arrays are pre-built)
      const bool hasTextureArrays = bgfx::isValid(m_TerrainAlbedoArrayTex) && m_TerrainArrayLayerCount > 0;
      glm::vec4 layerTiling[kMaxTerrainLayers];
      glm::vec4 layerColors[kMaxTerrainLayers];
      for (int i = 0; i < kMaxTerrainLayers; ++i)
      {
         if (i < static_cast<int>(terrain.Layers.size()))
         {
            const TerrainLayerDesc& layer = terrain.Layers[i];
            const float tiling = glm::max(0.01f, layer.Tiling);
            // With texture arrays, textures are always present (fallback baked in)
            const float hasAlbedo = hasTextureArrays ? 1.0f : 0.0f;
            const float hasNormal = hasTextureArrays ? 1.0f : 0.0f;
            layerTiling[i] = glm::vec4(tiling, hasAlbedo, hasNormal, 0.0f);
            layerColors[i] = glm::vec4(layer.PlaceholderColor, 1.0f);
         }
         else
         {
            layerTiling[i] = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
            layerColors[i] = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
         }
      }
      
      // Check if terrain uses 8-layer mode (has second splatmap)
      const bool has8Layers = !terrain.SplatMap2.empty();
      
      // Layer count for shader
      const int numActiveLayers = static_cast<int>(terrain.Layers.size());

      const uint64_t terrainState =
         BGFX_STATE_WRITE_RGB |
         BGFX_STATE_WRITE_Z |
         BGFX_STATE_DEPTH_TEST_LESS |
         BGFX_STATE_MSAA |
         BGFX_STATE_CULL_CW;

      float transform[16];
      memcpy(transform, glm::value_ptr(data->Transform.WorldMatrix), sizeof(float) * 16);

      // ========================================================================
      // CLIPMAP RENDERING PATH (when terrain.UseClipmaps is enabled)
      // Uses geometry clipmaps for efficient LOD with smooth transitions
      // ========================================================================
      if (terrain.UseClipmaps && terrain.ClipmapSystem && bgfx::isValid(m_ClipmapProgram))
      {
         auto* clipSystem = static_cast<terrain::TerrainClipmapSystem*>(terrain.ClipmapSystem);
         const auto& levels = clipSystem->GetLevels();
         const auto& meshes = clipSystem->GetMeshes();
         const auto& config = clipSystem->GetConfig();
         
         // Need valid height texture from the first chunk
         if (!terrain.Chunks.empty() && bgfx::isValid(terrain.Chunks[0].HeightTexture))
         {
            bgfx::TextureHandle heightTex = terrain.Chunks[0].HeightTexture;
            bgfx::TextureHandle splatTex = terrain.Chunks[0].SplatTexture;
            bgfx::TextureHandle holeTex = bgfx::isValid(terrain.Chunks[0].HoleTexture) ? terrain.Chunks[0].HoleTexture : m_TerrainFallbackHole;
            
            // Render clipmap levels from outer to inner for proper depth ordering
            for (int lvl = static_cast<int>(levels.size()) - 1; lvl >= 0; --lvl)
            {
               const auto& level = levels[lvl];
               
               // Calculate morph factor for smooth LOD transition
               float morphFactor = 0.0f;
               if (config.EnableMorphing && lvl < static_cast<int>(levels.size()) - 1)
               {
                  const float levelExtent = level.Scale * static_cast<float>(config.GridSize);
                  const float morphStart = levelExtent * (1.0f - config.MorphRegion);
                  const glm::vec2 camXZ(cameraPosition.x, cameraPosition.z);
                  const float distFromCenter = glm::length(camXZ - level.SnapOffset);
                  morphFactor = glm::clamp((distFromCenter - morphStart) / (levelExtent - morphStart), 0.0f, 1.0f);
               }
               
               // Set per-level clipmap uniforms
               glm::vec4 clipParams(level.Scale, morphFactor, static_cast<float>(config.GridSize), static_cast<float>(lvl));
               bgfx::setUniform(u_ClipmapParams, glm::value_ptr(clipParams));
               
               glm::vec4 clipOffset(level.SnapOffset.x, level.SnapOffset.y, terrain.WorldSize.x, terrain.WorldSize.y);
               bgfx::setUniform(u_ClipmapOffset, glm::value_ptr(clipOffset));
               
               // Extract terrain world origin from transform matrix (column 3 = translation)
               // This allows clipmaps to work correctly when terrain is not at world origin
               glm::vec3 terrainOrigin = glm::vec3(data->Transform.WorldMatrix[3]);
               glm::vec4 originVec(terrainOrigin.x, terrainOrigin.y, terrainOrigin.z, 0.0f);
               bgfx::setUniform(u_TerrainOrigin, glm::value_ptr(originVec));
               
               // Set height and terrain params
               bgfx::setUniform(u_TerrainHeightParams, glm::value_ptr(heightParams));
               
               // Set normal matrix for proper normal transformation
               SetNormalMatrixUniform(transform);
               
               // Set camera position uniform (needed by clipmap shader)
               glm::vec4 camPosUniform(cameraPosition.x, cameraPosition.y, cameraPosition.z, 1.0f);
               bgfx::setUniform(u_cameraPos, glm::value_ptr(camPosUniform));
               
               // Bind textures
               bgfx::setTexture(0, s_TerrainHeightTexture, heightTex, terrainDataSamplerFlags);
               if (bgfx::isValid(splatTex))
                  bgfx::setTexture(1, s_TerrainSplatTexture, splatTex);
               
               // Bind second splatmap for 8-layer mode (slot 2)
               if (has8Layers && bgfx::isValid(terrain.Chunks[0].SplatTexture2))
                  bgfx::setTexture(2, s_TerrainSplatTexture2, terrain.Chunks[0].SplatTexture2);
               
               // Bind texture arrays (slot 3 = albedo array, slot 4 = normal array)
               if (bgfx::isValid(m_TerrainAlbedoArrayTex))
                  bgfx::setTexture(3, s_TerrainAlbedoArray, m_TerrainAlbedoArrayTex, terrainSamplerFlags);
               if (bgfx::isValid(m_TerrainNormalArrayTex))
                  bgfx::setTexture(4, s_TerrainNormalArray, m_TerrainNormalArrayTex, terrainSamplerFlags);
               if (bgfx::isValid(holeTex))
                  bgfx::setTexture(5, s_TerrainHoleTexture, holeTex, terrainDataSamplerFlags);

               // Set material and lighting uniforms
               bgfx::setUniform(u_TerrainLayerTiling, layerTiling, kMaxTerrainLayers);
               bgfx::setUniform(u_TerrainLayerColor, layerColors, kMaxTerrainLayers);
               
               // Set layer count uniform
               glm::vec4 layerCountVec(static_cast<float>(numActiveLayers), 0.0f, 0.0f, 0.0f);
               bgfx::setUniform(u_TerrainLayerCount, glm::value_ptr(layerCountVec));
               glm::vec4 terrainMaterial(0.0f, 0.7f, 1.0f, 1.0f);  // metallic, roughness, ao, normalStrength
               bgfx::setUniform(u_TerrainMaterial, glm::value_ptr(terrainMaterial));
               
               BindShadowUniforms();
               GlobalShaderProperties::Instance().Apply();
               glm::vec4 receiveVec(terrainReceive, 0.0f, 0.0f, 0.0f);
               bgfx::setUniform(u_ShadowReceive, &receiveVec);
               
               bgfx::setTransform(transform);
               
               // Level 0: render center mesh (full grid)
               // Other levels: render ring mesh (hollow center)
               if (lvl == 0)
               {
                  if (bgfx::isValid(meshes.CenterVB) && bgfx::isValid(meshes.CenterIB))
                  {
                     bgfx::setVertexBuffer(0, meshes.CenterVB);
                     bgfx::setIndexBuffer(meshes.CenterIB);
                     bgfx::setState(terrainState);
                     if (bgfx::isValid(m_ClipmapProgram)) {
                        bgfx::submit(1, m_ClipmapProgram);
                     }
                  }
               }
               else
               {
                  if (bgfx::isValid(meshes.RingVB) && bgfx::isValid(meshes.RingIB))
                  {
                     bgfx::setVertexBuffer(0, meshes.RingVB);
                     bgfx::setIndexBuffer(meshes.RingIB);
                     bgfx::setState(terrainState);
                     if (bgfx::isValid(m_ClipmapProgram)) {
                        bgfx::submit(1, m_ClipmapProgram);
                     }
                  }
               }
            }
         }
      }
      // ========================================================================
      // CHUNKED TERRAIN RENDERING PATH (Skyrim-style cells with unified textures)
      // Uses NxN chunks with per-chunk LOD, morphing, and parallel culling
      // ========================================================================
      else if (terrain.UseChunkedTerrain && bgfx::isValid(m_ChunkedTerrainProgram) && m_ChunkSystem)
      {
         // Initialize chunk system if needed
         bool chunkLayoutChanged = false;
         if (terrain.Chunks.size() != m_ChunkSystem->GetTotalChunks() || terrain.ChunkMeshDirty)
         {
            terrain::ChunkSystemConfig chunkConfig;
            chunkConfig.ChunkVertexSize = terrain.ChunkVertexSize;
            chunkConfig.EnableMorphing = terrain.ChunkMorphing;
            chunkConfig.MorphRegion = terrain.ChunkMorphRegion;
            // Copy LOD distances from terrain config
            for (uint32_t i = 0; i < terrain::ChunkSystemConfig::kMaxLODLevels; ++i)
            {
               chunkConfig.LODDistances[i] = terrain.LODConfig.LODDistances[i];
               chunkConfig.LODSteps[i] = terrain.LODConfig.LODSteps[i];
            }
            m_ChunkSystem->Init(terrain, chunkConfig);
            chunkLayoutChanged = true;
            terrain.RuntimeChunkPrepToken = 0;
         }

         const bool streamingEnabled = terrain.ChunkStreaming && (cm::g_JobSystem != nullptr);
         bool didStreamingUpdate = false;
         if (streamingEnabled)
         {
            if (!terrain.ChunkStreamingSystem)
            {
               auto* streaming = new terrain::TerrainStreamingSystem();
               terrain::StreamingConfig config;
               config.Enabled = true;
               config.LoadRadius = terrain.StreamingLoadRadius;
               config.UnloadRadius = terrain.StreamingUnloadRadius;
               streaming->Init(terrain, *cm::g_JobSystem, config);
               terrain.ChunkStreamingSystem = streaming;
               didStreamingUpdate = true;
            }

            auto* streaming = static_cast<terrain::TerrainStreamingSystem*>(terrain.ChunkStreamingSystem);
            if (streaming)
            {
               if (chunkLayoutChanged)
               {
                  terrain::StreamingConfig config = streaming->GetConfig();
                  config.Enabled = true;
                  config.LoadRadius = terrain.StreamingLoadRadius;
                  config.UnloadRadius = terrain.StreamingUnloadRadius;
                  streaming->Init(terrain, *cm::g_JobSystem, config);
                  didStreamingUpdate = true;
               }
               else
               {
                  auto& config = streaming->GetConfig();
                  config.Enabled = true;
                  config.LoadRadius = terrain.StreamingLoadRadius;
                  config.UnloadRadius = terrain.StreamingUnloadRadius;
               }

               if (chunkLayoutChanged || terrain.RuntimeChunkPrepToken != m_TerrainChunkPrepToken)
               {
                  streaming->Update(terrain, cameraPosition, data->Transform.WorldMatrix);
                  streaming->ProcessGpuUploads(terrain);
                  didStreamingUpdate = true;
               }
            }
         }
         else if (terrain.ChunkStreamingSystem)
         {
            Terrain::DestroyStreamingSystem(terrain);
            for (TerrainChunk& chunk : terrain.Chunks)
               chunk.StreamState = ChunkStreamState::Resident;
            didStreamingUpdate = true;
         }

         const bool needsChunkPrep = chunkLayoutChanged ||
                                     didStreamingUpdate ||
                                     (terrain.RuntimeChunkPrepToken != m_TerrainChunkPrepToken);
         if (needsChunkPrep)
         {
            Profiler::Get().AddCounter("Render/TerrainPrepRuns", 1);
            // Build frustum for culling
            terrain::ChunkFrustum chunkFrustum = terrain::ChunkFrustum::FromViewProj(proj * view);

            // Update chunk LOD selection with parallel culling when JobSystem is available
            if (cm::g_JobSystem != nullptr) {
               m_ChunkSystem->UpdateChunkLODParallel(terrain, cameraPosition, data->Transform.WorldMatrix, chunkFrustum, *cm::g_JobSystem);
            } else {
               m_ChunkSystem->UpdateChunkLOD(terrain, cameraPosition, data->Transform.WorldMatrix, chunkFrustum);
            }

            // Enforce restricted LOD (max difference of 1 between neighbors)
            m_ChunkSystem->EnforceRestrictedLOD(terrain);

            // Update morph factors and neighbor LOD info
            m_ChunkSystem->UpdateMorphFactors(terrain, cameraPosition);
            m_ChunkSystem->UpdateNeighborLODs(terrain);
            terrain.RuntimeChunkPrepToken = m_TerrainChunkPrepToken;
         }
         else
         {
            Profiler::Get().AddCounter("Render/TerrainPrepSkips", 1);
         }
         
         // Check we have valid unified textures
         if (terrain.Chunks.empty() || !bgfx::isValid(terrain.Chunks[0].HeightTexture))
            continue;
         
         bgfx::TextureHandle heightTex = terrain.Chunks[0].HeightTexture;
         bgfx::TextureHandle splatTex = terrain.Chunks[0].SplatTexture;
         bgfx::TextureHandle holeTex = bgfx::isValid(terrain.Chunks[0].HoleTexture) ? terrain.Chunks[0].HoleTexture : m_TerrainFallbackHole;
         
         // Set terrain size uniform
         glm::vec4 terrainSizeVec(terrain.WorldSize.x, terrain.WorldSize.y, 0.0f, 0.0f);
         const float invRes = 1.0f / static_cast<float>(std::max(1u, terrain.GridResolution));
         glm::vec4 chunkTexelSize(invRes, invRes, 0.0f, 0.0f);
         
         // =====================================================================
         // INSTANCED CHUNKED TERRAIN RENDERING PATH
         // Batches all visible chunks by LOD level into single draw calls
         // This reduces draw call overhead from O(N) to O(LOD_levels) = O(4)
         // =====================================================================
         if (bgfx::isValid(m_ChunkedTerrainInstancedProgram) && bgfx::isValid(terrain.SharedChunkVB))
         {
            // Prepare instance batches once per render token and reuse across terrain passes.
            auto& batchCache = m_TerrainChunkBatchCache[terrainEntityId];
            if (batchCache.token != m_TerrainChunkPrepToken)
            {
               batchCache.totalVisible = m_ChunkSystem->PrepareInstancedBatches(terrain, batchCache.batches);
               batchCache.token = m_TerrainChunkPrepToken;
            }
            uint32_t totalVisible = batchCache.totalVisible;
            
            if (totalVisible > 0)
            {
               Profiler::Get().AddCounter("Render/TerrainVisibleChunks", totalVisible);
               // Instance stride: 4 vec4s = 64 bytes (ChunkInstanceData)
               constexpr uint16_t instanceStride = sizeof(terrain::ChunkInstanceData);
               
               // Render each LOD batch with a single instanced draw call
               for (uint32_t lod = 0; lod < terrain::ChunkSystemConfig::kMaxLODLevels; ++lod)
               {
                  const auto& batch = batchCache.batches[lod];
                  if (batch.Empty())
                     continue;
                  
                  // Get index buffer for this LOD level
                  bgfx::IndexBufferHandle lodIB = m_ChunkSystem->GetLODIndexBuffer(terrain, lod);
                  uint32_t lodIndexCount = m_ChunkSystem->GetLODIndexCount(terrain, lod);
                  
                  if (!bgfx::isValid(lodIB) || lodIndexCount == 0)
                     continue;
                  
                  // Check available instance buffer space
                  uint32_t instanceCount = static_cast<uint32_t>(batch.Size());
                  uint32_t availableInstances = bgfx::getAvailInstanceDataBuffer(instanceCount, instanceStride);
                  
                  if (availableInstances == 0)
                     continue;
                  
                  if (instanceCount > availableInstances)
                     instanceCount = availableInstances;
                  
                  // Allocate and fill instance data buffer
                  bgfx::InstanceDataBuffer idb{};
                  bgfx::allocInstanceDataBuffer(&idb, instanceCount, instanceStride);
                  std::memcpy(idb.data, batch.Instances.data(), instanceCount * instanceStride);
                  
                  // Set transform, buffers, and instance data
                  bgfx::setTransform(transform);
                  bgfx::setVertexBuffer(0, terrain.SharedChunkVB);
                  bgfx::setIndexBuffer(lodIB, 0, lodIndexCount);
                  bgfx::setInstanceDataBuffer(&idb);
                  
                  // Bind unified textures (same for all chunks)
                  bgfx::setTexture(0, s_TerrainHeightTexture, heightTex, terrainDataSamplerFlags);
                  if (bgfx::isValid(splatTex))
                     bgfx::setTexture(1, s_TerrainSplatTexture, splatTex);
                  
                  // Bind second splatmap for 8-layer mode
                  if (has8Layers && bgfx::isValid(terrain.Chunks[0].SplatTexture2))
                     bgfx::setTexture(2, s_TerrainSplatTexture2, terrain.Chunks[0].SplatTexture2);
                  
                  // Bind texture arrays
                  if (bgfx::isValid(m_TerrainAlbedoArrayTex))
                     bgfx::setTexture(3, s_TerrainAlbedoArray, m_TerrainAlbedoArrayTex, terrainSamplerFlags);
                  if (bgfx::isValid(m_TerrainNormalArrayTex))
                     bgfx::setTexture(4, s_TerrainNormalArray, m_TerrainNormalArrayTex, terrainSamplerFlags);
                  if (bgfx::isValid(holeTex))
                     bgfx::setTexture(5, s_TerrainHoleTexture, holeTex, terrainDataSamplerFlags);
                  
                  // Set shared uniforms (not per-chunk - those are in instance data)
                  bgfx::setUniform(u_TerrainSize, glm::value_ptr(terrainSizeVec));
                  bgfx::setUniform(u_TerrainHeightParams, glm::value_ptr(heightParams));
                  bgfx::setUniform(u_TerrainTexelSize, glm::value_ptr(chunkTexelSize));
                  
                  // Set material and lighting uniforms
                  bgfx::setUniform(u_TerrainLayerTiling, layerTiling, kMaxTerrainLayers);
                  bgfx::setUniform(u_TerrainLayerColor, layerColors, kMaxTerrainLayers);
                  
                  glm::vec4 layerCountVec(static_cast<float>(numActiveLayers), 0.0f, 0.0f, 0.0f);
                  bgfx::setUniform(u_TerrainLayerCount, glm::value_ptr(layerCountVec));
                  
                  glm::vec4 terrainMaterial(0.0f, 0.7f, 1.0f, 1.0f);
                  bgfx::setUniform(u_TerrainMaterial, glm::value_ptr(terrainMaterial));
                  
                  // Camera position for shader
                  glm::vec4 camPosUniform(cameraPosition.x, cameraPosition.y, cameraPosition.z, 1.0f);
                  bgfx::setUniform(u_cameraPos, glm::value_ptr(camPosUniform));
                  
                  SetNormalMatrixUniform(transform);
                  BindShadowUniforms();
                  GlobalShaderProperties::Instance().Apply();
                  glm::vec4 receiveVec(terrainReceive, 0.0f, 0.0f, 0.0f);
                  bgfx::setUniform(u_ShadowReceive, &receiveVec);
                  
                  bgfx::setState(terrainState);
                  bgfx::submit(1, m_ChunkedTerrainInstancedProgram);
               }
            }
         }
         // =====================================================================
         // FALLBACK: Per-chunk rendering (if instanced shaders not available)
         // =====================================================================
         else
         {
            // Render each visible chunk individually
            for (const TerrainChunk& chunk : terrain.Chunks)
            {
               if (!chunk.Visible || chunk.CurrentLOD < 0)
                  continue;
               
               // Skip unloaded chunks (streaming)
               if (chunk.StreamState != ChunkStreamState::Resident)
                  continue;
               
               // Get LOD index buffer
               const uint32_t lodLevel = static_cast<uint32_t>(chunk.CurrentLOD);
               bgfx::IndexBufferHandle lodIB = m_ChunkSystem->GetLODIndexBuffer(terrain, lodLevel);
               uint32_t lodIndexCount = m_ChunkSystem->GetLODIndexCount(terrain, lodLevel);
               
               if (!bgfx::isValid(terrain.SharedChunkVB) || !bgfx::isValid(lodIB) || lodIndexCount == 0)
                  continue;
               
               // Set per-chunk uniforms
               glm::vec4 chunkParams(chunk.UVOffset.x, chunk.UVOffset.y,
                                      chunk.UVScale.x, chunk.UVScale.y);
               glm::vec4 chunkWorld(chunk.WorldOffset.x, chunk.WorldOffset.y,
                                     chunk.WorldExtent.x, chunk.WorldExtent.y);
               glm::vec4 morphParams(chunk.MorphFactor,
                                      static_cast<float>(chunk.CurrentLOD),
                                      static_cast<float>(terrain.ChunkVertexSize),
                                      0.0f);
               glm::vec4 neighborLODs(
                  static_cast<float>(chunk.NeighborLODs[0]),  // North
                  static_cast<float>(chunk.NeighborLODs[1]),  // East
                  static_cast<float>(chunk.NeighborLODs[2]),  // South
                  static_cast<float>(chunk.NeighborLODs[3])); // West
               
               bgfx::setTransform(transform);
               bgfx::setVertexBuffer(0, terrain.SharedChunkVB);
               bgfx::setIndexBuffer(lodIB, 0, lodIndexCount);
               
               // Bind unified textures
               bgfx::setTexture(0, s_TerrainHeightTexture, heightTex, terrainDataSamplerFlags);
               if (bgfx::isValid(splatTex))
                  bgfx::setTexture(1, s_TerrainSplatTexture, splatTex);
               
               // Bind second splatmap for 8-layer mode
               if (has8Layers && bgfx::isValid(terrain.Chunks[0].SplatTexture2))
                  bgfx::setTexture(2, s_TerrainSplatTexture2, terrain.Chunks[0].SplatTexture2);
               
               // Bind texture arrays
               if (bgfx::isValid(m_TerrainAlbedoArrayTex))
                  bgfx::setTexture(3, s_TerrainAlbedoArray, m_TerrainAlbedoArrayTex, terrainSamplerFlags);
               if (bgfx::isValid(m_TerrainNormalArrayTex))
                  bgfx::setTexture(4, s_TerrainNormalArray, m_TerrainNormalArrayTex, terrainSamplerFlags);
               if (bgfx::isValid(holeTex))
                  bgfx::setTexture(5, s_TerrainHoleTexture, holeTex, terrainDataSamplerFlags);
               
               // Set chunk uniforms
               bgfx::setUniform(u_ChunkParams, glm::value_ptr(chunkParams));
               bgfx::setUniform(u_ChunkWorld, glm::value_ptr(chunkWorld));
               bgfx::setUniform(u_MorphParams, glm::value_ptr(morphParams));
               bgfx::setUniform(u_NeighborLODs, glm::value_ptr(neighborLODs));
               bgfx::setUniform(u_TerrainSize, glm::value_ptr(terrainSizeVec));
               bgfx::setUniform(u_TerrainHeightParams, glm::value_ptr(heightParams));
               bgfx::setUniform(u_TerrainTexelSize, glm::value_ptr(chunkTexelSize));
               
               // Set material and lighting uniforms
               bgfx::setUniform(u_TerrainLayerTiling, layerTiling, kMaxTerrainLayers);
               bgfx::setUniform(u_TerrainLayerColor, layerColors, kMaxTerrainLayers);
               
               glm::vec4 layerCountVec(static_cast<float>(numActiveLayers), 0.0f, 0.0f, 0.0f);
               bgfx::setUniform(u_TerrainLayerCount, glm::value_ptr(layerCountVec));
               
               glm::vec4 terrainMaterial(0.0f, 0.7f, 1.0f, 1.0f);
               bgfx::setUniform(u_TerrainMaterial, glm::value_ptr(terrainMaterial));
               
               // Camera position for shader
               glm::vec4 camPosUniform(cameraPosition.x, cameraPosition.y, cameraPosition.z, 1.0f);
               bgfx::setUniform(u_cameraPos, glm::value_ptr(camPosUniform));
               
               SetNormalMatrixUniform(transform);
               BindShadowUniforms();
               GlobalShaderProperties::Instance().Apply();
               glm::vec4 receiveVec(terrainReceive, 0.0f, 0.0f, 0.0f);
               bgfx::setUniform(u_ShadowReceive, &receiveVec);
               
               bgfx::setState(terrainState);
               bgfx::submit(1, m_ChunkedTerrainProgram);
            }
         }
      }
      // ========================================================================
      // TRADITIONAL SINGLE-CHUNK RENDERING PATH (legacy fallback)
      // ========================================================================
      else
      {
         for (const TerrainChunk& chunk : terrain.Chunks)
         {
            // Skip chunks that are frustum culled
            if (!chunk.Visible)
               continue;
               
            if (!bgfx::isValid(chunk.HeightTexture) || !bgfx::isValid(chunk.SplatTexture))
               continue;

            const uint32_t chunkQuadsX = chunk.VertexCountX > 0 ? chunk.VertexCountX - 1 : 0;
            const uint32_t chunkQuadsZ = chunk.VertexCountZ > 0 ? chunk.VertexCountZ - 1 : 0;

            // Chunk layout in local space: start in XZ and physical size (in meters) along XZ.
            glm::vec4 chunkParams(
               chunk.Start.x * cellSize.x,
               chunk.Start.y * cellSize.y,
               glm::max(1u, chunkQuadsX) * cellSize.x,
               glm::max(1u, chunkQuadsZ) * cellSize.y);

            // Texel size is based on the actual height texture resolution: VertexCountX x VertexCountZ.
            // This matches UploadHeightTexture, which creates the texture with those dimensions.
            glm::vec4 texelSize(
               1.0f / glm::max(1u, chunk.VertexCountX),
               1.0f / glm::max(1u, chunk.VertexCountZ),
               0.0f,
               0.0f);

            bgfx::setTransform(transform);
            bgfx::setVertexBuffer(0, terrain.ChunkVB);
            bgfx::setIndexBuffer(activeIB);
            bgfx::setTexture(0, s_TerrainHeightTexture, chunk.HeightTexture, terrainDataSamplerFlags);
            bgfx::setTexture(1, s_TerrainSplatTexture, chunk.SplatTexture);
            bgfx::TextureHandle holeTex = bgfx::isValid(chunk.HoleTexture) ? chunk.HoleTexture : m_TerrainFallbackHole;
            
            // Bind second splatmap for 8-layer mode (slot 2)
            if (has8Layers && bgfx::isValid(chunk.SplatTexture2))
               bgfx::setTexture(2, s_TerrainSplatTexture2, chunk.SplatTexture2);

            // Bind texture arrays (slot 3 = albedo array, slot 4 = normal array)
            if (bgfx::isValid(m_TerrainAlbedoArrayTex))
               bgfx::setTexture(3, s_TerrainAlbedoArray, m_TerrainAlbedoArrayTex, terrainSamplerFlags);
            if (bgfx::isValid(m_TerrainNormalArrayTex))
               bgfx::setTexture(4, s_TerrainNormalArray, m_TerrainNormalArrayTex, terrainSamplerFlags);
            if (bgfx::isValid(holeTex))
               bgfx::setTexture(5, s_TerrainHoleTexture, holeTex, terrainDataSamplerFlags);

            bgfx::setUniform(u_TerrainChunkParams, glm::value_ptr(chunkParams));
            bgfx::setUniform(u_TerrainHeightParams, glm::value_ptr(heightParams));
            bgfx::setUniform(u_TerrainTexelSize, glm::value_ptr(texelSize));
            bgfx::setUniform(u_TerrainLayerTiling, layerTiling, kMaxTerrainLayers);
            bgfx::setUniform(u_TerrainLayerColor, layerColors, kMaxTerrainLayers);
            
            // Set layer count uniform
            glm::vec4 layerCountVec(static_cast<float>(numActiveLayers), 0.0f, 0.0f, 0.0f);
            bgfx::setUniform(u_TerrainLayerCount, glm::value_ptr(layerCountVec));
            
            // Terrain material properties: x=metallic, y=roughness, z=ao, w=normalStrength
            glm::vec4 terrainMaterial(0.0f, 0.7f, 1.0f, 1.0f); // Default: non-metallic, medium roughness, full AO, normal strength 1.0
            bgfx::setUniform(u_TerrainMaterial, glm::value_ptr(terrainMaterial));
            
            // Normal matrix for correct normal transformation (handles non-uniform scaling)
            SetNormalMatrixUniform(transform);

            BindShadowUniforms();
            GlobalShaderProperties::Instance().Apply();
            glm::vec4 receiveVec(terrainReceive, 0.0f, 0.0f, 0.0f);
            bgfx::setUniform(u_ShadowReceive, &receiveVec);
            bgfx::setState(terrainState);
            if (bgfx::isValid(m_TerrainProgram)) {
               bgfx::submit(1, m_TerrainProgram);
            }
         }
      }

      { ScopedTimer tGrass("Render/Grass");
      TerrainGrass::Render(terrain, data->Transform.WorldMatrix, 1, cameraPosition, view, proj, m_EnableFrustumCulling);
      }
      
      // Render resource layers (imposters and debug visualization)
      if (data->ResourceLayers) {
         cm::resourcelayer::ResourceLayerComponent& layers = *data->ResourceLayers;
         
         // Auto-regenerate if needed (e.g., after scene load or play mode entry)
         if (layers.NeedsFullRegeneration && data->Terrain) {
            layers.Regenerate(*data->Terrain, data->Transform.WorldMatrix);
            layers.NeedsFullRegeneration = false;
            layers.NeedsRegeneration = false;
         }
         
         glm::mat4 viewProj = proj * view;
         
         // Update visibility and distance for all resource instances
         cm::resourcelayer::ResourceLayerRenderer layerRenderer;
         layerRenderer.UpdateVisibility(layers, cameraPosition, viewProj);
         
         // Render imposters and debug markers
         uint64_t layerState = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | 
                               BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS |
                               BGFX_STATE_CULL_CW | BGFX_STATE_MSAA;
         layerRenderer.Render(1, layers, view, proj, cameraPosition, layerState);
         
         // ProximitySwapSystem: In play mode, swap imposters to full prefabs when camera is close
         if (scene.m_IsPlaying) {
            static cm::resourcelayer::ProximitySwapSystem s_SwapSystem;
            s_SwapSystem.SetMaxActivePrefabs(layers.MaxActivePrefabs);
            s_SwapSystem.Update(layers, scene, cameraPosition, 0.016f);
         }
      }
   }
   }

   // --------------------------------------
   // Draw instancers (optimized instanced meshes with prefab hot-swap)
   // Upload lighting uniforms once for all instancer draws
   // --------------------------------------
   { ScopedTimer t("Render/Instancers");
   {
      UploadLightsToShader(m_ScratchLights);
      UploadEnvironmentToShader(m_CachedEnvironment, m_CachedEditorLightingOverride);
      
      uint64_t instancerState = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | 
                                BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS |
                                BGFX_STATE_CULL_CW | BGFX_STATE_MSAA;
      cm::instancer::InstancerSystem::Instance().Render(
         1, scene, m_view, proj, cameraPosition, instancerState);
   }
   }

   // --------------------------------------
   // Draw particle emitters (new system)
   // Scene-filtered rendering ensures only emitters owned by this scene are drawn
   // --------------------------------------
   { ScopedTimer t("Render/Particles");
   bx::Vec3 eye = { camPos.x, camPos.y, camPos.z };
   ecs::ParticleEmitterSystem::Get().Render(1, m_view, eye, &scene);
   }

   // --------------------------------------
   // Debug drawing (editor mode, or allowed in play mode)
   // --------------------------------------
#ifndef CLAYMORE_RUNTIME
   const bool allowEditorDebugInPlay = Application::HasInstance() && Application::Get().m_RunEditorUI && m_DebugDrawInPlayMode;
#else
   const bool allowEditorDebugInPlay = false;
#endif
#ifndef CLAYMORE_RUNTIME
   if (!scene.m_IsPlaying || allowEditorDebugInPlay) {
      ScopedTimer t("Render/DebugDraw");
      // Colliders
      if (m_ShowColliders) {
         for (auto& entity : scene.GetEntities()) {
            auto* data = scene.GetEntityData(entity.GetID());
            if (!IsPresentationVisible(data)) continue;
            if (data->Collider) {
               const Mesh* mesh = (data->Mesh && data->Mesh->mesh) ? data->Mesh->mesh.get() : nullptr;
               DrawCollider(*data->Collider, data->Transform, mesh, data->RigidBody.get(), data->StaticBody.get());
            }
            if (data->CharacterController) {
               DrawCharacterController(*data->CharacterController, data->Transform);
            }
            // Debug draw Areas with overlap-based coloring
            // Red = no overlaps detected, Green = has overlaps (bodies or areas)
            if (data->Area) {
               bool hasOverlaps = false;
               {
                  std::lock_guard<std::mutex> lk(data->Area->Mutex);
                  hasOverlaps = !data->Area->OverlappingBodies.empty() || !data->Area->OverlappingAreas.empty();
               }
               // ABGR format: Green when overlapping, Red when not
               uint32_t abgrColor = hasOverlaps
                  ? ((uint32_t(255) << 24) | (uint32_t(0) << 16) | (uint32_t(255) << 8) | uint32_t(0))   // Bright green
                  : ((uint32_t(255) << 24) | (uint32_t(0) << 16) | (uint32_t(0) << 8) | uint32_t(255));  // Bright red
               DrawAreaCollider(*data->Area, data->Transform, abgrColor);
            }
            }
         }

      // Picking AABBs (world-space) around meshes
      if (m_ShowAABBs) {
         for (auto& entity : scene.GetEntities()) {
            auto* data = scene.GetEntityData(entity.GetID());
            if (!IsPresentationVisible(data) || !data->Mesh || !data->Mesh->mesh) continue;
            std::shared_ptr<Mesh> meshPtr = data->Mesh->mesh;
            if (!meshPtr) continue;
            // Transform local AABB to world-space by transforming the 8 corners and recomputing min/max
            const glm::vec3 lmin = meshPtr->BoundsMin;
            const glm::vec3 lmax = meshPtr->BoundsMax;
            glm::vec3 corners[8] = {
               {lmin.x, lmin.y, lmin.z}, {lmax.x, lmin.y, lmin.z}, {lmin.x, lmax.y, lmin.z}, {lmax.x, lmax.y, lmin.z},
               {lmin.x, lmin.y, lmax.z}, {lmax.x, lmin.y, lmax.z}, {lmin.x, lmax.y, lmax.z}, {lmax.x, lmax.y, lmax.z}
            };
            glm::vec3 wmin( std::numeric_limits<float>::max());
            glm::vec3 wmax(-std::numeric_limits<float>::max());
            for (int i = 0; i < 8; ++i) {
               glm::vec3 w = glm::vec3(data->Transform.WorldMatrix * glm::vec4(corners[i], 1.0f));
               wmin = glm::min(wmin, w);
               wmax = glm::max(wmax, w);
            }
            DrawAABB(wmin, wmax, 0);
         }
      }

      if (m_ShowCameraFrustums) {
         for (auto& entity : scene.GetEntities()) {
            auto* data = scene.GetEntityData(entity.GetID());
            if (!IsPresentationVisible(data) || !data->Camera) continue;
            DrawCameraFrustum(*data->Camera, data->Transform);
         }
      }

      // Physics debug visualization (raycasts/linecasts)
      if (PhysicsDebug::IsEnabled()) {
         DrawPhysicsDebugLines();
         // Clear single-frame debug lines after rendering
         PhysicsDebug::Tick(0.016f); // ~60fps delta, single-frame lines are cleared immediately
      }
   }
#endif // CLAYMORE_RUNTIME - debug drawing is editor-only

   // --------------------------------------
   // Draw text components (world or screen space)
   // --------------------------------------
   if (m_TextRenderer) {
      { ScopedTimer t("Render/Text");
   // Main scene ordering: meshes (1), world-space UI (2), screen-space UI (3)
   m_TextRenderer->RenderTexts(scene, m_view, m_proj, m_Width, m_Height, 1, kMainScreenUIViewId);
      }
   }

   std::unordered_set<uint16_t> currentUIOffscreenViewIds;
   bool allowWorldCanvasInput = true;
   { ScopedTimer t("Render/UIOverlay");
   RenderUIOverlay(
      scene,
      kMainScreenUIViewId,
      m_Width,
      m_Height,
      true,
      CanvasComponent::RenderSpace::ScreenSpace,
      INVALID_ENTITY_ID,
      &currentUIOffscreenViewIds,
      false,
      false);
   allowWorldCanvasInput = !m_UIInputConsumed;
   }
   { ScopedTimer t("Render/WorldUI");
   RenderWorldSpaceCanvases(
      scene,
      renderCamera,
      m_view,
      m_proj,
      kMainWorldUIViewId,
      GetSceneFrameBuffer(),
      m_Width,
      m_Height,
      allowWorldCanvasInput,
      currentUIOffscreenViewIds);
   }
   m_UIPrevMouseDown = Input::IsMouseButtonPressed(0);
   for (uint16_t viewId : m_UISceneCaptureViewIds) {
      if (currentUIOffscreenViewIds.find(viewId) == currentUIOffscreenViewIds.end()) {
         ReleaseOffscreenTarget(viewId);
      }
   }
   m_UISceneCaptureViewIds = std::move(currentUIOffscreenViewIds);

   DrawShadowDebugOverlay(renderCamera);

   // Restore shadow context so other viewports don't inherit it
   m_ShadowContextScene = prevShadowScene;
   m_ShadowContextEnabled = prevShadowEnabled;
   }

void Renderer::RenderUIOverlay(Scene& scene,
   uint16_t viewId,
   uint32_t width,
   uint32_t height,
   bool allowInput,
   CanvasComponent::RenderSpace renderSpace,
   EntityID targetCanvasEntity,
   std::unordered_set<uint16_t>* currentOffscreenViewIds,
   bool releaseUnusedOffscreenTargets,
   bool updateMouseState) {
   const uint16_t uiViewId = viewId;
   const uint32_t prevWidth = m_Width;
   const uint32_t prevHeight = m_Height;
   m_Width = width;
   m_Height = height;
   const bool restoreInput = !allowInput;
   const bool prevUIInputConsumed = m_UIInputConsumed;
   m_UIInputConsumed = false;

   // --------------------------------------
   // UI Rendering (Canvas/Panel/Button)
   // --------------------------------------
   if (m_ShowUIOverlay && bgfx::isValid(m_UIProgram)) {
      // Setup orthographic projection for the screen-space UI view (top-left origin)
      const bgfx::Caps* caps = bgfx::getCaps();
      float ortho[16];
      bx::mtxOrtho(ortho, 0.0f, float(m_Width), float(m_Height), 0.0f, 0.0f, 100.0f, 0.0f, caps->homogeneousDepth);
      float viewIdMat[16]; bx::mtxIdentity(viewIdMat);
      bgfx::setViewTransform(uiViewId, viewIdMat, ortho);
      bgfx::setViewRect(uiViewId, 0, 0, (uint16_t)m_Width, (uint16_t)m_Height);
      // Ensure painter's order for UI/text: honor submission order (z-sorted by us)
      bgfx::setViewMode(uiViewId, bgfx::ViewMode::Sequential);

      // Mouse for hit-testing (prefer viewport-reported framebuffer coords)
      float mx = 0.0f, my = 0.0f;
      if (allowInput) {
         if (m_UIMouseValid) {
            mx = m_UIMouseX;
            my = m_UIMouseY;
            }
         else {
            auto mp = Input::GetMousePosition();
            mx = mp.first; my = mp.second;
            }
         }
      else {
         mx = -FLT_MAX;
         my = -FLT_MAX;
         }
      bool mouseDown = allowInput && Input::IsMouseButtonPressed(0);
      const bool mousePressed = allowInput && mouseDown && !m_UIPrevMouseDown;
      const bool mouseReleased = allowInput && !mouseDown && m_UIPrevMouseDown;
      EntityID dropTargetCandidate = INVALID_ENTITY_ID;
      if (allowInput && m_UIDragSource != INVALID_ENTITY_ID && !scene.GetEntityData(m_UIDragSource)) {
         m_UIDragSource = INVALID_ENTITY_ID;
         m_UIDragActive = false;
      }
      m_UIInputConsumed = false;

      // Sorted UI draw: collect panels and screen-space texts, sort by canvas order then hierarchy then z
      const auto& entities = scene.GetEntities();
      m_ScratchUIItems.clear();
      m_ScratchUIDrawOrder.clear();
      if (m_ScratchUIItems.capacity() < entities.size()) {
         m_ScratchUIItems.reserve(entities.size());
      }
      if (m_ScratchUIDrawOrder.capacity() < entities.size()) {
         m_ScratchUIDrawOrder.reserve(entities.size());
      }

      std::unordered_map<EntityID, size_t> uiEntityOrder;
      uiEntityOrder.reserve(std::min<size_t>(entities.size(), 128));
      std::unordered_map<const CanvasComponent*, glm::vec2> canvasScaleCache;
      canvasScaleCache.reserve(16);
      
      // OPTIMIZATION: Helper to submit UI quad using transient buffers (no create/destroy per-element)
      // Transient buffers are automatically recycled at end of frame - no manual cleanup needed
      // scissorHandle: UINT16_MAX means no scissor, otherwise use cached scissor rect
      auto submitUIQuadTransient = [this, uiViewId](UIVertex* verts, uint16_t* indices, uint32_t vertCount, uint32_t indexCount, 
                                          bgfx::TextureHandle tex, uint16_t /*viewId*/ = 2, uint16_t scissorHandle = UINT16_MAX) {
         bgfx::TransientVertexBuffer tvb;
         bgfx::TransientIndexBuffer tib;
         
         if (!bgfx::allocTransientBuffers(&tvb, UIVertex::layout, vertCount, &tib, indexCount)) {
            return; // Failed to allocate - transient buffer pool exhausted
         }

         bgfx::TextureHandle boundTexture = bgfx::isValid(tex) ? tex : m_UIWhiteTex;
         if (!bgfx::isValid(boundTexture) || !bgfx::isValid(m_UISampler) || !bgfx::isValid(m_UIProgram)) {
            return;
         }
         
         memcpy(tvb.data, verts, vertCount * sizeof(UIVertex));
         memcpy(tib.data, indices, indexCount * sizeof(uint16_t));
         
         float identity[16]; bx::mtxIdentity(identity);
         bgfx::setTransform(identity);
         bgfx::setVertexBuffer(0, &tvb);
         bgfx::setIndexBuffer(&tib);
         bgfx::setTexture(0, m_UISampler, boundTexture);
         bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_ALWAYS | BGFX_STATE_BLEND_ALPHA);
         
         // Apply scissor if provided
         if (scissorHandle != UINT16_MAX) {
            bgfx::setScissor(scissorHandle);
         }
         
         bgfx::submit(uiViewId, m_UIProgram);
      };
      
      // Helper lambda to calculate scale factor from canvas reference resolution
      // Returns {scaleX, scaleY} where 1.0 means no scaling
      auto getCanvasScale = [this, &canvasScaleCache](CanvasComponent* canvas) -> glm::vec2 {
         auto cacheIt = canvasScaleCache.find(canvas);
         if (cacheIt != canvasScaleCache.end()) {
            return cacheIt->second;
         }

         glm::vec2 resolvedScale(1.0f, 1.0f);
         if (!canvas || canvas->ReferenceWidth <= 0 || canvas->ReferenceHeight <= 0) {
            canvasScaleCache[canvas] = resolvedScale;
            return resolvedScale;  // No reference resolution = no scaling
         }
         
         float refW = static_cast<float>(canvas->ReferenceWidth);
         float refH = static_cast<float>(canvas->ReferenceHeight);
         float scaleX = static_cast<float>(m_Width) / refW;
         float scaleY = static_cast<float>(m_Height) / refH;
         
         switch (canvas->ReferenceScaleMode) {
            case CanvasComponent::ScaleMode::ConstantPixelSize:
               resolvedScale = glm::vec2(1.0f, 1.0f);  // No scaling
               break;
            case CanvasComponent::ScaleMode::ScaleWithWidth:
               resolvedScale = glm::vec2(scaleX, scaleX);  // Use width scale for both axes
               break;
            case CanvasComponent::ScaleMode::ScaleWithHeight:
               resolvedScale = glm::vec2(scaleY, scaleY);  // Use height scale for both axes
               break;
            case CanvasComponent::ScaleMode::ScaleWithSmallest:
               {
                  float minScale = std::min(scaleX, scaleY);
                  resolvedScale = glm::vec2(minScale, minScale);
               }
               break;
            case CanvasComponent::ScaleMode::ScaleWithLargest:
               {
                  float maxScale = std::max(scaleX, scaleY);
                  resolvedScale = glm::vec2(maxScale, maxScale);
               }
               break;
            case CanvasComponent::ScaleMode::Expand:
               resolvedScale = glm::vec2(scaleX, scaleY);  // Independent scaling per axis
               break;
            default:
               break;
         }
         canvasScaleCache[canvas] = resolvedScale;
         return resolvedScale;
      };

      // Helper lambda to compute hierarchy depth (root = 0)
      // Safety limit prevents infinite loop if there's a cycle in parent chain
      auto computeHierarchyDepth = [&scene](EntityID id) -> int {
         int depth = 0;
         EntityID cur = id;
         constexpr int kMaxDepth = 1000;
         while (cur != INVALID_ENTITY_ID && depth < kMaxDepth) {
            auto* d = scene.GetEntityData(cur);
            if (!d || d->Parent == INVALID_ENTITY_ID) break;
            cur = d->Parent;
            ++depth;
         }
         if (depth >= kMaxDepth) {
            std::cerr << "[Renderer] WARNING: computeHierarchyDepth hit safety limit for entity " << id << " - possible parent cycle!" << std::endl;
         }
         return depth;
      };

      // Helper: check if any ancestor (or self) hides UI via visibility flags.
      // This ensures children don't render when a parent panel/scroll view is hidden.
      auto isHiddenByUIAncestor = [&scene](EntityID id) -> bool {
         EntityID cur = id;
         int safetyCounter = 0;
         constexpr int kMaxTraversal = 1000;
         while (cur != INVALID_ENTITY_ID && safetyCounter < kMaxTraversal) {
            ++safetyCounter;
            auto* d = scene.GetEntityData(cur);
            if (!d) break;
            if (!IsPresentationVisible(d)) return true;
            if (d->Panel && !d->Panel->Visible) return true;
            if (d->ScrollView && !d->ScrollView->Visible) return true;
            cur = d->Parent;
         }
         return false;
      };

      auto computeDrivenChildOpacity = [&scene](EntityID id) -> float {
         float opacity = 1.0f;
         auto* d = scene.GetEntityData(id);
         EntityID cur = d ? d->Parent : INVALID_ENTITY_ID;
         int safetyCounter = 0;
         constexpr int kMaxTraversal = 1000;
         while (cur != INVALID_ENTITY_ID && safetyCounter < kMaxTraversal) {
            ++safetyCounter;
            auto* pd = scene.GetEntityData(cur);
            if (!pd) break;
            if (pd->Panel && pd->Panel->DriveChildrenOpacity) {
               opacity *= std::clamp(pd->Panel->Opacity, 0.0f, 1.0f);
            }
            cur = pd->Parent;
         }
         return opacity;
      };

      // Collect all UI items
      size_t sceneEntityOrderCounter = 0;
      for (auto& e : entities) {
         const EntityID entityId = e.GetID();
         const size_t sceneEntityOrder = sceneEntityOrderCounter++;
         auto* d = scene.GetEntityData(entityId);
         if (!d) continue;

         const bool hasPanel = (d->Panel && d->Panel->Visible);
         const bool hasText = (d->Text && !d->Text->WorldSpace && d->Text->Visible);
         const bool hasLayoutGroupItem = (!hasPanel && d->LayoutGroup != nullptr);
         if (!hasPanel && !hasText && !hasLayoutGroupItem) {
            continue;
         }
         if (!IsPresentationVisible(d)) continue;
         if (isHiddenByUIAncestor(entityId)) continue;

         uiEntityOrder.emplace(entityId, sceneEntityOrder);
         // Find the nearest ancestor canvas and keep each subtree owned by that nearest canvas.
         CanvasComponent* canvas = nullptr;
         EntityID canvasEntityId = INVALID_ENTITY_ID;
         {
            EntityID cur = entityId;
            int safetyCounter = 0;
            constexpr int kMaxTraversal = 1000;
            while (cur != INVALID_ENTITY_ID && safetyCounter < kMaxTraversal) {
               ++safetyCounter;
               auto* d2 = scene.GetEntityData(cur);
               if (!d2) {
                  break;
               }
               if (d2->Canvas) {
                  canvas = d2->Canvas.get();
                  canvasEntityId = cur;
                  break;
               }
               cur = d2->Parent;
            }
            if (safetyCounter >= kMaxTraversal) {
               std::cerr << "[Renderer] WARNING: Canvas ancestor search hit safety limit for entity " << entityId << " - possible parent cycle!" << std::endl;
            }
         }

         const bool hasMatchingCanvas = canvas && canvas->Space == renderSpace
            && (targetCanvasEntity == INVALID_ENTITY_ID || canvasEntityId == targetCanvasEntity);
         const bool includeLegacyScreenItem = !canvas
            && renderSpace == CanvasComponent::RenderSpace::ScreenSpace
            && targetCanvasEntity == INVALID_ENTITY_ID;
         if (!hasMatchingCanvas && !includeLegacyScreenItem) {
            continue;
         }

         int corder = canvas ? canvas->SortOrder : 0;
         float copacity = canvas ? canvas->Opacity : 1.0f;
         copacity *= computeDrivenChildOpacity(entityId);
         int hdepth = computeHierarchyDepth(entityId);
         
         if (hasPanel)
         {
            m_ScratchUIItems.push_back(
               { corder, hdepth, d->Panel->ZOrder, copacity, UIItemType::Panel, d->Panel.get(), nullptr, d, entityId, canvas, {0,0,0,0} }
            );
         }
         if (hasText)
         {
            m_ScratchUIItems.push_back(
               { corder, hdepth, d->Text->ZOrder, copacity, UIItemType::Text, nullptr, d->Text.get(), d, entityId, canvas, {0,0,0,0} }
            );
         }
         if (hasLayoutGroupItem)
         {
            m_ScratchUIItems.push_back(
               { corder, hdepth, 0, copacity, UIItemType::LayoutGroup, nullptr, nullptr, d, entityId, canvas, {0,0,0,0} }
            );
         }
      }

      // Sort: canvas order, then hierarchy depth (parents first), then z-order
      std::sort(m_ScratchUIItems.begin(), m_ScratchUIItems.end(), [](const UIDrawItem& a, const UIDrawItem& b){
         if (a.canvasOrder != b.canvasOrder) 
         { 
            return a.canvasOrder < b.canvasOrder; 
         }
         // Parents before children (lower depth first)
         if (a.hierarchyDepth != b.hierarchyDepth) {
            return a.hierarchyDepth < b.hierarchyDepth;
         }
         return a.z < b.z;
      });
      
      // Build entity ID to item index map for quick parent lookup
      m_ScratchUIIndex.clear();
      m_ScratchUIIndex.reserve(m_ScratchUIItems.size());
      for (size_t i = 0; i < m_ScratchUIItems.size(); ++i) {
         m_ScratchUIIndex[m_ScratchUIItems[i].entityId] = i;
      }
      Profiler::Get().SetCounter("Render/UIItems", static_cast<uint64_t>(m_ScratchUIItems.size()));

      std::unordered_map<EntityID, size_t> uiRectSourceIndex;
      uiRectSourceIndex.reserve(m_ScratchUIItems.size());
      std::unordered_map<EntityID, size_t> panelItemIndex;
      panelItemIndex.reserve(m_ScratchUIItems.size());
      for (size_t i = 0; i < m_ScratchUIItems.size(); ++i) {
         const UIDrawItem& item = m_ScratchUIItems[i];
         if (item.type == UIItemType::Panel) {
            panelItemIndex[item.entityId] = i;
            if (uiRectSourceIndex.find(item.entityId) == uiRectSourceIndex.end()) {
               uiRectSourceIndex[item.entityId] = i;
            }
         } else if (item.type == UIItemType::LayoutGroup && uiRectSourceIndex.find(item.entityId) == uiRectSourceIndex.end()) {
            uiRectSourceIndex[item.entityId] = i;
         }
      }

      std::unordered_map<EntityID, EntityID> parentUIAncestorCache;
      parentUIAncestorCache.reserve(m_ScratchUIItems.size());
      std::unordered_map<EntityID, EntityID> nearestScrollViewCache;
      nearestScrollViewCache.reserve(m_ScratchUIItems.size());
      std::unordered_map<EntityID, std::vector<EntityID>> scrollAncestorChainCache;
      scrollAncestorChainCache.reserve(m_ScratchUIItems.size());
      
      // Helper to find parent UI rect (looks for nearest ancestor with Panel or LayoutGroup)
      // Safety limit prevents infinite loop if there's a cycle in parent chain
      auto resolveParentUIAncestor = [&](EntityID id) -> EntityID {
         if (auto cacheIt = parentUIAncestorCache.find(id); cacheIt != parentUIAncestorCache.end()) {
            return cacheIt->second;
         }

         auto* d = scene.GetEntityData(id);
         EntityID cur = d ? d->Parent : INVALID_ENTITY_ID;
         int safetyCounter = 0;
         constexpr int kMaxTraversal = 1000;
         while (cur != INVALID_ENTITY_ID && safetyCounter < kMaxTraversal) {
            ++safetyCounter;
            if (uiRectSourceIndex.find(cur) != uiRectSourceIndex.end()) {
               parentUIAncestorCache[id] = cur;
               return cur;
            }

            auto* pd = scene.GetEntityData(cur);
            if (!pd) {
               break;
            }
            cur = pd->Parent;
         }
         if (safetyCounter >= kMaxTraversal) {
            std::cerr << "[Renderer] WARNING: resolveParentUIAncestor hit safety limit for entity " << id << std::endl;
         }

         parentUIAncestorCache[id] = INVALID_ENTITY_ID;
         return INVALID_ENTITY_ID;
      };
      auto findParentUIRect = [&](EntityID id) -> glm::vec4 {
         const EntityID parentUiEntity = resolveParentUIAncestor(id);
         if (parentUiEntity != INVALID_ENTITY_ID) {
            auto itemIt = uiRectSourceIndex.find(parentUiEntity);
            if (itemIt != uiRectSourceIndex.end()) {
               return m_ScratchUIItems[itemIt->second].computedRect;
            }
         }
         return glm::vec4(0, 0, (float)m_Width, (float)m_Height); // Screen rect if no parent UI
      };
      
      // Helper to find cumulative scroll offset from ancestor ScrollView components
      // Returns the total (x, y) offset to subtract from child positions
      auto resolveScrollAncestors = [&](EntityID id) -> const std::vector<EntityID>& {
         if (auto cacheIt = scrollAncestorChainCache.find(id); cacheIt != scrollAncestorChainCache.end()) {
            return cacheIt->second;
         }

         std::vector<EntityID> ancestors;
         auto* d = scene.GetEntityData(id);
         EntityID cur = d ? d->Parent : INVALID_ENTITY_ID;
         int safetyCounter = 0;
         constexpr int kMaxTraversal = 1000;
         while (cur != INVALID_ENTITY_ID && safetyCounter < kMaxTraversal) {
            ++safetyCounter;
            auto* pd = scene.GetEntityData(cur);
            if (!pd) {
               break;
            }
            if (pd->ScrollView) {
               ancestors.push_back(cur);
            }
            cur = pd->Parent;
         }

         auto [it, _] = scrollAncestorChainCache.emplace(id, std::move(ancestors));
         return it->second;
      };
      auto findScrollOffset = [&](EntityID id) -> glm::vec2 {
         glm::vec2 offset(0.0f, 0.0f);
         for (EntityID ancestorId : resolveScrollAncestors(id)) {
            auto* pd = scene.GetEntityData(ancestorId);
            if (!pd) {
               continue;
            }
            if (pd->ScrollView && pd->ScrollView->Visible) {
               offset += pd->ScrollView->ContentOffset;
            }
         }
         return offset;
      };
      
      // Helper to find the nearest ancestor ScrollView for clipping purposes
      // Returns the entity ID and computed rect of the ScrollView, or INVALID_ENTITY_ID if none
      auto resolveNearestScrollView = [&](EntityID id) -> EntityID {
         if (auto cacheIt = nearestScrollViewCache.find(id); cacheIt != nearestScrollViewCache.end()) {
            return cacheIt->second;
         }

         auto* d = scene.GetEntityData(id);
         EntityID cur = d ? d->Parent : INVALID_ENTITY_ID;
         int safetyCounter = 0;
         constexpr int kMaxTraversal = 1000;
         while (cur != INVALID_ENTITY_ID && safetyCounter < kMaxTraversal) {
            ++safetyCounter;
            auto* pd = scene.GetEntityData(cur);
            if (!pd) {
               break;
            }
            if (pd->ScrollView) {
               nearestScrollViewCache[id] = cur;
               return cur;
            }
            cur = pd->Parent;
         }

         nearestScrollViewCache[id] = INVALID_ENTITY_ID;
         return INVALID_ENTITY_ID;
      };
      auto findParentScrollView = [&](EntityID id) -> std::pair<EntityID, glm::vec4> {
         const EntityID scrollViewEntity = resolveNearestScrollView(id);
         if (scrollViewEntity != INVALID_ENTITY_ID) {
            auto* pd = scene.GetEntityData(scrollViewEntity);
            if (pd && pd->ScrollView && pd->ScrollView->Visible) {
               auto it = panelItemIndex.find(scrollViewEntity);
               if (it != panelItemIndex.end()) {
                  return { scrollViewEntity, m_ScratchUIItems[it->second].computedRect };
               }
            }
         }
         return { INVALID_ENTITY_ID, glm::vec4(0) };
      };
      
      // Helper to check if a parent has a LayoutGroup component
      // Children of layout groups should not use anchor-based positioning
      auto parentHasLayoutGroup = [&](EntityID id) -> bool {
         auto* d = scene.GetEntityData(id);
         if (!d || d->Parent == INVALID_ENTITY_ID) return false;
         
         auto* parentData = scene.GetEntityData(d->Parent);
         return parentData && parentData->LayoutGroup != nullptr;
      };
      
      // Layout pass: compute screen rects for all items (in sorted order, parents first)
      for (auto& it : m_ScratchUIItems) {
         // Get scale factor from canvas reference resolution
         glm::vec2 scale = getCanvasScale(it.canvas);
         
         if (it.type == UIItemType::Panel) {
            PanelComponent& p = *it.panel;
            float ax = 0.0f, ay = 0.0f;
            
            // Check if using parent-relative anchoring (UIRect component)
            if (it.data->UIRect && it.data->UIRect->AnchorToParent) {
               UIRectComponent& rect = *it.data->UIRect;
               glm::vec4 parentRect = findParentUIRect(it.entityId);
               
               // Compute anchor position within parent (parent rect is already in screen space)
               float anchorX = parentRect.x + parentRect.z * rect.HorizontalAnchor;
               float anchorY = parentRect.y + parentRect.w * rect.VerticalAnchor;
               
               // Apply pivot offset (pivot determines which point of this element sits at anchor)
               // Scale size and offset by reference resolution scale factor
               float w = p.Size.x * p.Scale.x * scale.x;
               float h = p.Size.y * p.Scale.y * scale.y;
               ax = anchorX - w * rect.Pivot.x + rect.Offset.x * scale.x;
               ay = anchorY - h * rect.Pivot.y + rect.Offset.y * scale.y;
            }
            else if (p.AnchorEnabled) {
               glm::vec4 anchorRect = p.AnchorToParentUI
                  ? findParentUIRect(it.entityId)
                  : glm::vec4(0.0f, 0.0f, (float)m_Width, (float)m_Height);

               float anchorX = 0.0f, anchorY = 0.0f;
               switch (p.Anchor) {
                  case UIAnchorPreset::TopLeft:    anchorX = anchorRect.x;                  anchorY = anchorRect.y;                   break;
                  case UIAnchorPreset::Top:        anchorX = anchorRect.x + anchorRect.z * 0.5f; anchorY = anchorRect.y;                   break;
                  case UIAnchorPreset::TopRight:   anchorX = anchorRect.x + anchorRect.z;   anchorY = anchorRect.y;                   break;
                  case UIAnchorPreset::Left:       anchorX = anchorRect.x;                  anchorY = anchorRect.y + anchorRect.w * 0.5f; break;
                  case UIAnchorPreset::Center:     anchorX = anchorRect.x + anchorRect.z * 0.5f; anchorY = anchorRect.y + anchorRect.w * 0.5f; break;
                  case UIAnchorPreset::Right:      anchorX = anchorRect.x + anchorRect.z;   anchorY = anchorRect.y + anchorRect.w * 0.5f; break;
                  case UIAnchorPreset::BottomLeft: anchorX = anchorRect.x;                  anchorY = anchorRect.y + anchorRect.w;    break;
                  case UIAnchorPreset::Bottom:     anchorX = anchorRect.x + anchorRect.z * 0.5f; anchorY = anchorRect.y + anchorRect.w;    break;
                  case UIAnchorPreset::BottomRight:anchorX = anchorRect.x + anchorRect.z;   anchorY = anchorRect.y + anchorRect.w;    break;
               }
               // Scale the offset from the anchor point
               anchorX += p.AnchorOffset.x * scale.x;
               anchorY += p.AnchorOffset.y * scale.y;
               
               // Apply pivot - the anchor point should be at the pivot position of the panel
               // So we offset backward from anchor to get top-left
               float w = p.Size.x * p.Scale.x * scale.x;
               float h = p.Size.y * p.Scale.y * scale.y;
               ax = anchorX - w * p.Pivot.x;
               ay = anchorY - h * p.Pivot.y;
            }
            else {
               // Position-based - always relative to parent UI element if one exists
               // This ensures children move with their parent panels
               glm::vec4 parentRect = findParentUIRect(it.entityId);
               bool hasUIParent = (parentRect.z != (float)m_Width || parentRect.w != (float)m_Height || 
                                   parentRect.x != 0 || parentRect.y != 0);
               
               if (hasUIParent || (it.data->UIRect && it.data->UIRect->AnchorToParent)) {
                  ax = parentRect.x + p.Position.x * scale.x;
                  ay = parentRect.y + p.Position.y * scale.y;
               } else {
                  // Absolute position (no UI parent) - scale it
                  ax = p.Position.x * scale.x;
                  ay = p.Position.y * scale.y;
               }
            }
            
            // Scale size by reference resolution factor
            float w = p.Size.x * p.Scale.x * scale.x;
            float h = p.Size.y * p.Scale.y * scale.y;
            
            it.computedRect = glm::vec4(ax, ay, w, h);
            
            // Store computed rect in UIRect if present
            if (it.data->UIRect) {
               it.data->UIRect->_ComputedRect = it.computedRect;
               it.data->UIRect->_RectDirty = false;
            }
         }
         else if (it.type == UIItemType::Text) {
            TextRendererComponent& t = *it.text;
            float sx = it.data->Transform.Position.x;
            float sy = it.data->Transform.Position.y;
            
            // Scale text metrics by reference resolution factor
            float scaledPixelSize = t.PixelSize * scale.x;  // Use X scale for font size
            
            // Estimate multiline text metrics for layout bounds. The renderer uses
            // real glyph advances later; this keeps default text rects large enough
            // for explicit line breaks without requiring a manual UIRect.
            float textAscent = scaledPixelSize * 0.8f;
            float textDescent = scaledPixelSize * 0.2f;
            float lineHeight = scaledPixelSize * 1.1f;
            size_t lineCount = 1;
            size_t currentLineChars = 0;
            size_t maxLineChars = 0;
            for (size_t i = 0; i < t.Text.size(); ++i) {
               const char ch = t.Text[i];
               if (ch == '\r' || ch == '\n') {
                  maxLineChars = std::max(maxLineChars, currentLineChars);
                  currentLineChars = 0;
                  ++lineCount;
                  if (ch == '\r' && i + 1 < t.Text.size() && t.Text[i + 1] == '\n') {
                     ++i;
                  }
                  continue;
               }
               const unsigned char uch = static_cast<unsigned char>(ch);
               if (uch >= 32 && uch < 128) {
                  ++currentLineChars;
               }
            }
            maxLineChars = std::max(maxLineChars, currentLineChars);
            float textHeight = (textAscent + textDescent) + lineHeight * static_cast<float>(lineCount - 1);
            
            // Rough approximation - actual width depends on font metrics and is handled during rendering.
            float textWidth = scaledPixelSize * static_cast<float>(maxLineChars) * 0.5f;
            
            // Check if using parent-relative anchoring
            bool wantsParentAnchor = t.AnchorToParentUI ||
                                     (it.data->UIRect && it.data->UIRect->AnchorToParent);
            if (wantsParentAnchor) {
               // Use existing UIRect when present; otherwise synthesize a default
               // rect so AnchorToParentUI can work without requiring a UIRect.
               UIRectComponent defaultRect;
               defaultRect.AnchorToParent = true;
               UIRectComponent& rect = it.data->UIRect ? *it.data->UIRect : defaultRect;
               glm::vec4 parentRect = findParentUIRect(it.entityId);
               
               // Compute anchor position within parent (parent rect is already in screen space)
               float anchorX = parentRect.x + parentRect.z * rect.HorizontalAnchor;
               float anchorY = parentRect.y + parentRect.w * rect.VerticalAnchor;
               
               // Use UIRect size if specified (scaled), otherwise use estimated text bounds
               float boundsW = rect.Size.x > 0 ? rect.Size.x * scale.x : textWidth;
               float boundsH = rect.Size.y > 0 ? rect.Size.y * scale.y : textHeight;
               
               // Compute top-left of the text bounding box based on pivot
               // Pivot (0.5, 0.5) = center, (0,0) = top-left, (1,1) = bottom-right
               float boxLeft = anchorX - boundsW * rect.Pivot.x
                               + rect.Offset.x * scale.x
                               + t.AnchorOffset.x * scale.x;
               float boxTop = anchorY - boundsH * rect.Pivot.y
                              + rect.Offset.y * scale.y
                              + t.AnchorOffset.y * scale.y;
               
               // Text rendering uses pen position (baseline start), not bounding box top-left
               // The pen X is the left edge of text, pen Y is the baseline
               // To vertically center text within the bounding box:
               // - The visual center of the text is at baseline - ascent/2 + textHeight/2
               // - For true vertical centering, baseline = boxTop + (boundsH + textAscent - textDescent) / 2
               // Simplified: place baseline so text appears vertically centered in bounds
               sx = boxLeft;
               sy = boxTop + (boundsH - textHeight) * 0.5f + textAscent;
               
               // Explicit UIRect bounds are aligned by TextRenderer using TextAlignment.
               // Unbounded text keeps the historical pivot-based anchor behavior.
               if (rect.Size.x <= 0.0f && textWidth < boundsW) {
                  sx = boxLeft + (boundsW - textWidth) * rect.Pivot.x;
               }
            }
            else if (t.AnchorEnabled) {
               // Screen-space anchoring - anchor points are in actual screen coordinates
               switch (t.Anchor) {
                  case UIAnchorPreset::TopLeft: break;
                  case UIAnchorPreset::Top:        sx = m_Width * 0.5f; break;
                  case UIAnchorPreset::TopRight:   sx = (float)m_Width; break;
                  case UIAnchorPreset::Left:       sy = m_Height * 0.5f; break;
                  case UIAnchorPreset::Center:     sx = m_Width * 0.5f; sy = m_Height * 0.5f; break;
                  case UIAnchorPreset::Right:      sx = (float)m_Width; sy = m_Height * 0.5f; break;
                  case UIAnchorPreset::BottomLeft: sy = (float)m_Height; break;
                  case UIAnchorPreset::Bottom:     sx = m_Width * 0.5f; sy = (float)m_Height; break;
                  case UIAnchorPreset::BottomRight:sx = (float)m_Width; sy = (float)m_Height; break;
               }
               // Scale the offset from the anchor point
               sx += t.AnchorOffset.x * scale.x;
               sy += t.AnchorOffset.y * scale.y;
            }
            else {
               // Position-based - always relative to parent UI element if one exists
               // This ensures children move with their parent panels
               // Position is treated as top-left of text bounds (like panels), not baseline
               glm::vec4 parentRect = findParentUIRect(it.entityId);
               bool hasUIParent = (parentRect.z != (float)m_Width || parentRect.w != (float)m_Height || 
                                   parentRect.x != 0 || parentRect.y != 0);
               
               float topX, topY;  // Top-left of text bounds
               if (hasUIParent || (it.data->UIRect && it.data->UIRect->AnchorToParent)) {
                  topX = parentRect.x + it.data->Transform.Position.x * scale.x;
                  topY = parentRect.y + it.data->Transform.Position.y * scale.y;
               } else {
                  // Absolute position (no UI parent) - scale it
                  topX = it.data->Transform.Position.x * scale.x;
                  topY = it.data->Transform.Position.y * scale.y;
               }
               
               // Convert from top of bounds to baseline for rendering
               sx = topX;
               sy = topY + textAscent;
            }
            
            // Store computed rect (using estimated text bounds)
            // computedRect.y is top of text bounds
            float rectTop = sy - textAscent;
            it.computedRect = glm::vec4(sx, rectTop, textWidth, textHeight);
            
            if (it.data->UIRect) {
               it.data->UIRect->_ComputedRect = it.computedRect;
               it.data->UIRect->_RectDirty = false;
            }
         }
         else if (it.type == UIItemType::LayoutGroup) {
            // Layout-only entities can define an anchoring rect via UIRect
            float ax = 0.0f;
            float ay = 0.0f;
            float w = 0.0f;
            float h = 0.0f;
            
            if (it.data->UIRect && it.data->UIRect->AnchorToParent) {
               UIRectComponent& rect = *it.data->UIRect;
               glm::vec4 parentRect = findParentUIRect(it.entityId);
               
               float anchorX = parentRect.x + parentRect.z * rect.HorizontalAnchor;
               float anchorY = parentRect.y + parentRect.w * rect.VerticalAnchor;
               
               w = rect.Size.x * scale.x;
               h = rect.Size.y * scale.y;
               ax = anchorX - w * rect.Pivot.x + rect.Offset.x * scale.x;
               ay = anchorY - h * rect.Pivot.y + rect.Offset.y * scale.y;
            } else {
               // Position-based - always relative to parent UI element if one exists
               glm::vec4 parentRect = findParentUIRect(it.entityId);
               bool hasUIParent = (parentRect.z != (float)m_Width || parentRect.w != (float)m_Height || 
                                   parentRect.x != 0 || parentRect.y != 0);
               
               if (hasUIParent) {
                  ax = parentRect.x + it.data->Transform.Position.x * scale.x;
                  ay = parentRect.y + it.data->Transform.Position.y * scale.y;
               } else {
                  ax = it.data->Transform.Position.x * scale.x;
                  ay = it.data->Transform.Position.y * scale.y;
               }
               
               if (it.data->UIRect) {
                  w = it.data->UIRect->Size.x * scale.x;
                  h = it.data->UIRect->Size.y * scale.y;
               }
            }
            
            it.computedRect = glm::vec4(ax, ay, w, h);
            
            if (it.data->UIRect) {
               it.data->UIRect->_ComputedRect = it.computedRect;
               it.data->UIRect->_RectDirty = false;
            }
         }
      }
      
      auto resolveGridDims = [](const LayoutGroupComponent& lg, int childCount,
         int& outColumns, int& outRows, int& outUsedColumns, int& outUsedRows) {
         int columns = lg.Columns;
         int rows = lg.Rows;
         if (columns <= 0 && rows > 0) {
            columns = (childCount + rows - 1) / rows;
         }
         if (rows <= 0 && columns > 0) {
            rows = (childCount + columns - 1) / columns;
         }
         if (columns <= 0) columns = std::max(1, childCount);
         if (rows <= 0) rows = 1;

         int usedColumns = columns;
         int usedRows = rows;
         if (lg.Columns > 0 && lg.Rows <= 0) {
            usedRows = (childCount + columns - 1) / columns;
         } else if (lg.Columns <= 0 && lg.Rows > 0) {
            usedColumns = (childCount + rows - 1) / rows;
         } else {
            usedRows = std::min(rows, (childCount + columns - 1) / columns);
         }
         outColumns = columns;
         outRows = rows;
         outUsedColumns = std::max(1, usedColumns);
         outUsedRows = std::max(1, usedRows);
      };

      // LayoutGroup sizing pass: compute sizes for layout-only entities from children
      // Process deeper items first so child sizes are available
      for (auto it = m_ScratchUIItems.rbegin(); it != m_ScratchUIItems.rend(); ++it) {
         if (it->type != UIItemType::LayoutGroup || !it->data->LayoutGroup) continue;
         
         LayoutGroupComponent& lg = *it->data->LayoutGroup;
         glm::vec2 lgScale = getCanvasScale(it->canvas);
         
         float scaledPaddingL = lg.Padding.x * lgScale.x;
         float scaledPaddingT = lg.Padding.y * lgScale.y;
         float scaledPaddingR = lg.Padding.z * lgScale.x;
         float scaledPaddingB = lg.Padding.w * lgScale.y;
         const bool gridEnabled = (lg.Columns > 0 || lg.Rows > 0);
         const float scaledSpacing = lg.Spacing * (gridEnabled ? lgScale.x
            : (lg.Direction == LayoutGroupComponent::LayoutDirection::Vertical ? lgScale.y : lgScale.x));
         
         float contentW = 0.0f;
         float contentH = 0.0f;
         int childCount = 0;
         
         // Use authored child order to keep layout deterministic
         for (EntityID childId : it->data->Children) {
            auto itIndex = m_ScratchUIIndex.find(childId);
            if (itIndex == m_ScratchUIIndex.end()) continue; // child not visible or not a UI item
            ++childCount;
         }
         
         if (childCount == 0) continue;
         
         if (gridEnabled) {
            int columns = 0;
            int rows = 0;
            int usedColumns = 0;
            int usedRows = 0;
            resolveGridDims(lg, childCount, columns, rows, usedColumns, usedRows);
            
            float cellW = lg.CellSize.x * lgScale.x;
            float cellH = lg.CellSize.y * lgScale.y;
            float gridW = usedColumns * cellW + scaledSpacing * std::max(0, usedColumns - 1);
            float gridH = usedRows * cellH + scaledSpacing * std::max(0, usedRows - 1);
            contentW = gridW + scaledPaddingL + scaledPaddingR;
            contentH = gridH + scaledPaddingT + scaledPaddingB;
         } else {
            float totalMain = 0.0f;
            float maxCross = 0.0f;
            for (EntityID childId : it->data->Children) {
               auto itIndex = m_ScratchUIIndex.find(childId);
               if (itIndex == m_ScratchUIIndex.end()) continue;
               const UIDrawItem& child = m_ScratchUIItems[itIndex->second];
               
               const float childW = child.computedRect.z;
               const float childH = child.computedRect.w;
               if (lg.Direction == LayoutGroupComponent::LayoutDirection::Vertical) {
                  totalMain += childH;
                  maxCross = std::max(maxCross, childW);
               } else {
                  totalMain += childW;
                  maxCross = std::max(maxCross, childH);
               }
            }
            
            if (childCount > 1) {
               totalMain += scaledSpacing * (childCount - 1);
            }
            
            if (lg.Direction == LayoutGroupComponent::LayoutDirection::Vertical) {
               contentW = maxCross + scaledPaddingL + scaledPaddingR;
               contentH = totalMain + scaledPaddingT + scaledPaddingB;
            } else {
               contentW = totalMain + scaledPaddingL + scaledPaddingR;
               contentH = maxCross + scaledPaddingT + scaledPaddingB;
            }
         }
         
         if (it->computedRect.z <= 0.0f) it->computedRect.z = contentW;
         if (it->computedRect.w <= 0.0f) it->computedRect.w = contentH;
         
         if (it->data->UIRect) {
            it->data->UIRect->_ComputedRect = it->computedRect;
            it->data->UIRect->_RectDirty = false;
         }
      }
      
      // LayoutGroup pass: position children according to layout settings
      // This handles both Panel and Text children of layout groups
      for (auto& it : m_ScratchUIItems) {
         if (!it.data->LayoutGroup || it.type == UIItemType::Text) continue;
         
         LayoutGroupComponent& lg = *it.data->LayoutGroup;
         glm::vec4 parentRect = it.computedRect;
         glm::vec2 lgScale = getCanvasScale(it.canvas);
         
         // Gather direct children of this layout group in authored order
         std::vector<size_t> childIndices;
         childIndices.reserve(it.data->Children.size());
         for (EntityID childId : it.data->Children) {
            auto itIndex = m_ScratchUIIndex.find(childId);
            if (itIndex == m_ScratchUIIndex.end()) continue; // child not visible or not a UI item
            // Include both panels and text elements as layout children
            childIndices.push_back(itIndex->second);
         }
         
         if (childIndices.empty()) {
            float scaledPaddingL = lg.Padding.x * lgScale.x;
            float scaledPaddingT = lg.Padding.y * lgScale.y;
            float scaledPaddingR = lg.Padding.z * lgScale.x;
            float scaledPaddingB = lg.Padding.w * lgScale.y;
            lg._CalculatedContentSize = glm::vec2(
               scaledPaddingL + scaledPaddingR,
               scaledPaddingT + scaledPaddingB
            );
            continue;
         }
         
         // Reverse order if requested
         if (lg.ReverseOrder) {
            std::reverse(childIndices.begin(), childIndices.end());
         }

         const bool gridEnabled = (lg.Columns > 0 || lg.Rows > 0);
         if (gridEnabled) {
            float scaledPaddingL = lg.Padding.x * lgScale.x;
            float scaledPaddingT = lg.Padding.y * lgScale.y;
            float scaledPaddingR = lg.Padding.z * lgScale.x;
            float scaledPaddingB = lg.Padding.w * lgScale.y;
            float scaledSpacing = lg.Spacing * lgScale.x;

            const int childCount = static_cast<int>(childIndices.size());
            int columns = 0;
            int rows = 0;
            int usedColumns = 0;
            int usedRows = 0;
            resolveGridDims(lg, childCount, columns, rows, usedColumns, usedRows);

            float cellW = lg.CellSize.x * lgScale.x;
            float cellH = lg.CellSize.y * lgScale.y;
            float gridW = usedColumns * cellW + scaledSpacing * std::max(0, usedColumns - 1);
            float gridH = usedRows * cellH + scaledSpacing * std::max(0, usedRows - 1);

            float startX = parentRect.x + scaledPaddingL;
            float startY = parentRect.y + scaledPaddingT;
            float availableW = parentRect.z - scaledPaddingL - scaledPaddingR;
            float availableH = parentRect.w - scaledPaddingT - scaledPaddingB;

            float alignOffsetX = 0.0f;
            if (availableW > gridW) {
               switch (lg.ChildAlignment) {
                  case LayoutGroupComponent::Alignment::Start:  alignOffsetX = 0.0f; break;
                  case LayoutGroupComponent::Alignment::Center: alignOffsetX = (availableW - gridW) * 0.5f; break;
                  case LayoutGroupComponent::Alignment::End:    alignOffsetX = availableW - gridW; break;
               }
            }
            float alignOffsetY = 0.0f;
            if (availableH > gridH) {
               switch (lg.CrossAlignment) {
                  case LayoutGroupComponent::Alignment::Start:  alignOffsetY = 0.0f; break;
                  case LayoutGroupComponent::Alignment::Center: alignOffsetY = (availableH - gridH) * 0.5f; break;
                  case LayoutGroupComponent::Alignment::End:    alignOffsetY = availableH - gridH; break;
               }
            }

            startX += alignOffsetX;
            startY += alignOffsetY;

            for (int i = 0; i < childCount; ++i) {
               int row = (columns > 0) ? (i / columns) : 0;
               int col = (columns > 0) ? (i % columns) : i;
               if (lg.Rows > 0 && row >= rows) break;

               UIDrawItem& child = m_ScratchUIItems[childIndices[i]];
               child.computedRect.x = startX + col * (cellW + scaledSpacing);
               child.computedRect.y = startY + row * (cellH + scaledSpacing);
               child.computedRect.z = cellW;
               child.computedRect.w = cellH;

               if (child.data->UIRect) {
                  child.data->UIRect->_ComputedRect = child.computedRect;
                  child.data->UIRect->_RectDirty = false;
               }
            }

            lg._CalculatedContentSize = glm::vec2(
               gridW + scaledPaddingL + scaledPaddingR,
               gridH + scaledPaddingT + scaledPaddingB
            );
            continue;
         }
         
         // Calculate layout starting position (parent top-left + padding)
         // Scale padding by reference resolution factor
         float scaledPaddingL = lg.Padding.x * lgScale.x;
         float scaledPaddingT = lg.Padding.y * lgScale.y;
         float scaledPaddingR = lg.Padding.z * lgScale.x;
         float scaledPaddingB = lg.Padding.w * lgScale.y;
         float scaledSpacing = lg.Spacing * (lg.Direction == LayoutGroupComponent::LayoutDirection::Vertical ? lgScale.y : lgScale.x);
         
         float startX = parentRect.x + scaledPaddingL;
         float startY = parentRect.y + scaledPaddingT;
         float availableW = parentRect.z - scaledPaddingL - scaledPaddingR;
         float availableH = parentRect.w - scaledPaddingT - scaledPaddingB;
         
         float currentX = startX;
         float currentY = startY;
         float maxCrossSize = 0.0f; // Track max size perpendicular to layout direction
         
         for (size_t ci : childIndices) {
            UIDrawItem& child = m_ScratchUIItems[ci];
            float childW = child.computedRect.z;
            float childH = child.computedRect.w;
            
            // Apply uniform child sizing if requested (only for panels)
            if (child.type == UIItemType::Panel) {
               if (lg.ControlChildWidth && lg.Direction == LayoutGroupComponent::LayoutDirection::Vertical) {
                  childW = availableW;
                  child.computedRect.z = childW;
               }
               if (lg.ControlChildHeight && lg.Direction == LayoutGroupComponent::LayoutDirection::Horizontal) {
                  childH = availableH;
                  child.computedRect.w = childH;
               }
            }
            
            // Position child (layout group takes precedence over anchor-based positioning)
            child.computedRect.x = currentX;
            child.computedRect.y = currentY;
            
            // Apply cross-axis alignment
            if (lg.Direction == LayoutGroupComponent::LayoutDirection::Vertical) {
               // Horizontal alignment within vertical layout
               switch (lg.CrossAlignment) {
                  case LayoutGroupComponent::Alignment::Start:
                     child.computedRect.x = startX;
                     break;
                  case LayoutGroupComponent::Alignment::Center:
                     child.computedRect.x = startX + (availableW - childW) * 0.5f;
                     break;
                  case LayoutGroupComponent::Alignment::End:
                     child.computedRect.x = startX + availableW - childW;
                     break;
               }
               currentY += childH + scaledSpacing;
               maxCrossSize = std::max(maxCrossSize, childW);
            } else {
               // Vertical alignment within horizontal layout
               switch (lg.CrossAlignment) {
                  case LayoutGroupComponent::Alignment::Start:
                     child.computedRect.y = startY;
                     break;
                  case LayoutGroupComponent::Alignment::Center:
                     child.computedRect.y = startY + (availableH - childH) * 0.5f;
                     break;
                  case LayoutGroupComponent::Alignment::End:
                     child.computedRect.y = startY + availableH - childH;
                     break;
               }
               currentX += childW + scaledSpacing;
               maxCrossSize = std::max(maxCrossSize, childH);
            }
            
            // Update UIRect if present
            if (child.data->UIRect) {
               child.data->UIRect->_ComputedRect = child.computedRect;
               child.data->UIRect->_RectDirty = false;
            }
         }
         
         // Store calculated content size for ScrollView integration (in screen space)
         if (lg.Direction == LayoutGroupComponent::LayoutDirection::Vertical) {
            lg._CalculatedContentSize = glm::vec2(maxCrossSize + scaledPaddingL + scaledPaddingR,
                                                  currentY - startY - scaledSpacing + scaledPaddingT + scaledPaddingB);
         } else {
            lg._CalculatedContentSize = glm::vec2(currentX - startX - scaledSpacing + scaledPaddingL + scaledPaddingR,
                                                  maxCrossSize + scaledPaddingT + scaledPaddingB);
         }
      }
      
      // Post-LayoutGroup pass: update panels that use parent-relative anchoring
      // Skip this for direct children of layout groups (layout groups handle positioning)
      for (auto& it : m_ScratchUIItems) {
         if (it.type != UIItemType::Panel) continue;
         PanelComponent& p = *it.panel;
         bool wantsParentAnchor = (it.data->UIRect && it.data->UIRect->AnchorToParent) ||
                                  (p.AnchorEnabled && p.AnchorToParentUI);
         if (!wantsParentAnchor) continue;
         if (parentHasLayoutGroup(it.entityId)) continue;

         glm::vec2 scale = getCanvasScale(it.canvas);
         glm::vec4 parentRect = findParentUIRect(it.entityId);

         float ax = 0.0f, ay = 0.0f;
         float w = p.Size.x * p.Scale.x * scale.x;
         float h = p.Size.y * p.Scale.y * scale.y;

         if (it.data->UIRect && it.data->UIRect->AnchorToParent) {
            UIRectComponent& rect = *it.data->UIRect;
            float anchorX = parentRect.x + parentRect.z * rect.HorizontalAnchor;
            float anchorY = parentRect.y + parentRect.w * rect.VerticalAnchor;
            ax = anchorX - w * rect.Pivot.x + rect.Offset.x * scale.x;
            ay = anchorY - h * rect.Pivot.y + rect.Offset.y * scale.y;
         } else if (p.AnchorEnabled && p.AnchorToParentUI) {
            float anchorX = 0.0f, anchorY = 0.0f;
            switch (p.Anchor) {
               case UIAnchorPreset::TopLeft:    anchorX = parentRect.x;                  anchorY = parentRect.y;                   break;
               case UIAnchorPreset::Top:        anchorX = parentRect.x + parentRect.z * 0.5f; anchorY = parentRect.y;                   break;
               case UIAnchorPreset::TopRight:   anchorX = parentRect.x + parentRect.z;   anchorY = parentRect.y;                   break;
               case UIAnchorPreset::Left:       anchorX = parentRect.x;                  anchorY = parentRect.y + parentRect.w * 0.5f; break;
               case UIAnchorPreset::Center:     anchorX = parentRect.x + parentRect.z * 0.5f; anchorY = parentRect.y + parentRect.w * 0.5f; break;
               case UIAnchorPreset::Right:      anchorX = parentRect.x + parentRect.z;   anchorY = parentRect.y + parentRect.w * 0.5f; break;
               case UIAnchorPreset::BottomLeft: anchorX = parentRect.x;                  anchorY = parentRect.y + parentRect.w;    break;
               case UIAnchorPreset::Bottom:     anchorX = parentRect.x + parentRect.z * 0.5f; anchorY = parentRect.y + parentRect.w;    break;
               case UIAnchorPreset::BottomRight:anchorX = parentRect.x + parentRect.z;   anchorY = parentRect.y + parentRect.w;    break;
            }
            anchorX += p.AnchorOffset.x * scale.x;
            anchorY += p.AnchorOffset.y * scale.y;
            ax = anchorX - w * p.Pivot.x;
            ay = anchorY - h * p.Pivot.y;
         } else {
            continue;
         }

         it.computedRect = glm::vec4(ax, ay, w, h);
         if (it.data->UIRect) {
            it.data->UIRect->_ComputedRect = it.computedRect;
            it.data->UIRect->_RectDirty = false;
         }
      }

      // Post-LayoutGroup pass: update children of repositioned panels
      // This handles text elements that use parent-relative positioning
      // Skip this for children of layout groups (layout groups handle positioning)
      for (auto& it : m_ScratchUIItems) {
         if (it.type != UIItemType::Text) continue;
         TextRendererComponent& t = *it.text;
         bool wantsParentAnchor = t.AnchorToParentUI ||
                                  (it.data->UIRect && it.data->UIRect->AnchorToParent);
         if (!wantsParentAnchor) continue;
         
         // Skip anchor-based repositioning if parent has a layout group
         // Layout groups take precedence and handle positioning
         if (parentHasLayoutGroup(it.entityId)) continue;
         
         // Get scale factor for this text's canvas
         glm::vec2 textScale = getCanvasScale(it.canvas);
         
         // Re-find parent rect (which may have been updated by LayoutGroup)
         glm::vec4 parentRect = findParentUIRect(it.entityId);
         UIRectComponent defaultRect;
         defaultRect.AnchorToParent = true;
         UIRectComponent& rect = it.data->UIRect ? *it.data->UIRect : defaultRect;
         
         // Re-compute text metrics with scaling
         float scaledPixelSize = t.PixelSize * textScale.x;
         float textAscent = scaledPixelSize * 0.8f;
         float textDescent = scaledPixelSize * 0.2f;
         float textHeight = textAscent + textDescent;
         float textWidth = scaledPixelSize * t.Text.length() * 0.5f;
         
         // Re-compute anchor position within parent (parent rect is already in screen space)
         float anchorX = parentRect.x + parentRect.z * rect.HorizontalAnchor;
         float anchorY = parentRect.y + parentRect.w * rect.VerticalAnchor;
         
         // Use UIRect size if specified (scaled), otherwise use estimated text bounds
         float boundsW = rect.Size.x > 0 ? rect.Size.x * textScale.x : textWidth;
         float boundsH = rect.Size.y > 0 ? rect.Size.y * textScale.y : textHeight;
         
        // Compute top-left of the text bounding box based on pivot and offsets
        float boxLeft = anchorX - boundsW * rect.Pivot.x
                        + rect.Offset.x * textScale.x
                        + t.AnchorOffset.x * textScale.x;
        float boxTop = anchorY - boundsH * rect.Pivot.y
                       + rect.Offset.y * textScale.y
                       + t.AnchorOffset.y * textScale.y;
         
         // Position text within bounding box based on pivot
         float sx = boxLeft;
         float sy = boxTop + (boundsH - textHeight) * 0.5f + textAscent;
         
         // Explicit UIRect bounds are aligned by TextRenderer using TextAlignment.
         // Unbounded text keeps the historical pivot-based anchor behavior.
         if (rect.Size.x <= 0.0f && textWidth < boundsW) {
            sx = boxLeft + (boundsW - textWidth) * rect.Pivot.x;
         }
         
         // Update computed rect
         float rectTop = sy - textAscent;
         it.computedRect = glm::vec4(sx, rectTop, textWidth, textHeight);
         
         if (it.data->UIRect) {
            it.data->UIRect->_ComputedRect = it.computedRect;
            it.data->UIRect->_RectDirty = false;
         }
      }
      
      // FitToContent pass: resize panels to fit children (after all rects computed)
      for (auto& it : m_ScratchUIItems) {
         if (it.type == UIItemType::Panel && it.data->FitToContent && it.data->FitToContent->Enabled) {
            FitToContentComponent& ftc = *it.data->FitToContent;
            PanelComponent& p = *it.panel;
            
            // Find children bounds
            float minX = std::numeric_limits<float>::max();
            float minY = std::numeric_limits<float>::max();
            float maxX = std::numeric_limits<float>::lowest();
            float maxY = std::numeric_limits<float>::lowest();
            bool hasChildren = false;
            
            for (const auto& childIt : m_ScratchUIItems) {
               if (childIt.entityId == it.entityId) continue;
               
               // Check if this is a child (direct or descendant based on DirectChildrenOnly)
               bool isChild = false;
               if (ftc.DirectChildrenOnly) {
                  isChild = (childIt.data->Parent == it.entityId);
               } else {
                  // Check if it's a descendant
                  // Safety limit prevents infinite loop if there's a cycle in parent chain
                  EntityID cur = childIt.data->Parent;
                  int safetyCounter = 0;
                  constexpr int kMaxTraversal = 1000;
                  while (cur != INVALID_ENTITY_ID && safetyCounter < kMaxTraversal) {
                     ++safetyCounter;
                     if (cur == it.entityId) { isChild = true; break; }
                     auto* pd = scene.GetEntityData(cur);
                     if (!pd) break;
                     cur = pd->Parent;
                  }
               }
               
               if (isChild) {
                  hasChildren = true;
                  // Child rect relative to parent
                  float cx = childIt.computedRect.x;
                  float cy = childIt.computedRect.y;
                  float cw = childIt.computedRect.z;
                  float ch = childIt.computedRect.w;
                  minX = std::min(minX, cx);
                  minY = std::min(minY, cy);
                  maxX = std::max(maxX, cx + cw);
                  maxY = std::max(maxY, cy + ch);
               }
            }
            
            if (hasChildren) {
               // Convert to parent-relative coordinates
               float relMinX = minX - it.computedRect.x;
               float relMinY = minY - it.computedRect.y;
               float relMaxX = maxX - it.computedRect.x;
               float relMaxY = maxY - it.computedRect.y;
               
               // Compute new size with padding
               float newW = (relMaxX - relMinX) + ftc.Padding.x + ftc.Padding.z;
               float newH = (relMaxY - relMinY) + ftc.Padding.y + ftc.Padding.w;
               
               // Apply min/max constraints
               if (ftc.MinSize.x > 0) newW = std::max(newW, ftc.MinSize.x);
               if (ftc.MinSize.y > 0) newH = std::max(newH, ftc.MinSize.y);
               if (ftc.MaxSize.x > 0) newW = std::min(newW, ftc.MaxSize.x);
               if (ftc.MaxSize.y > 0) newH = std::min(newH, ftc.MaxSize.y);
               
               // Update panel size
               if (ftc.FitWidth) p.Size.x = newW / p.Scale.x;
               if (ftc.FitHeight) p.Size.y = newH / p.Scale.y;
               
               // Recompute this panel's rect
               float ax = it.computedRect.x;
               float ay = it.computedRect.y;
               it.computedRect.z = p.Size.x * p.Scale.x;
               it.computedRect.w = p.Size.y * p.Scale.y;
            }
         }
      }

      // Compatibility fix for same-entity world-space UI roots that were authored
      // while their components still held screen-space coordinates. If the root
      // visuals land entirely outside their own offscreen canvas, localize the
      // whole canvas subtree back to (0, 0) so legacy scenes still render.
      if (renderSpace == CanvasComponent::RenderSpace::WorldSpace && targetCanvasEntity != INVALID_ENTITY_ID) {
         float rootMinX = std::numeric_limits<float>::max();
         float rootMinY = std::numeric_limits<float>::max();
         float rootMaxX = std::numeric_limits<float>::lowest();
         float rootMaxY = std::numeric_limits<float>::lowest();
         bool hasCanvasRootVisual = false;

         for (const auto& it : m_ScratchUIItems) {
            if (it.entityId != targetCanvasEntity)
               continue;

            rootMinX = std::min(rootMinX, it.computedRect.x);
            rootMinY = std::min(rootMinY, it.computedRect.y);
            rootMaxX = std::max(rootMaxX, it.computedRect.x + it.computedRect.z);
            rootMaxY = std::max(rootMaxY, it.computedRect.y + it.computedRect.w);
            hasCanvasRootVisual = true;
         }

         const bool rootIsCompletelyOutsideCanvas = hasCanvasRootVisual
            && (rootMaxX <= 0.0f
               || rootMinX >= static_cast<float>(m_Width)
               || rootMaxY <= 0.0f
               || rootMinY >= static_cast<float>(m_Height));

         if (rootIsCompletelyOutsideCanvas) {
            const glm::vec2 rootOffset(rootMinX, rootMinY);
            for (auto& it : m_ScratchUIItems) {
               it.computedRect.x -= rootOffset.x;
               it.computedRect.y -= rootOffset.y;
               if (it.data->UIRect) {
                  it.data->UIRect->_ComputedRect = it.computedRect;
                  it.data->UIRect->_RectDirty = false;
               }
            }
         }
      }

      // UI Scene Capture pass: render scene views into panel textures
      std::unordered_set<uint16_t> localCaptureViewIds;
      std::unordered_set<uint16_t>& currentCaptureViewIds =
         currentOffscreenViewIds ? *currentOffscreenViewIds : localCaptureViewIds;
      const bgfx::Caps* uiCaps = bgfx::getCaps();
      const uint16_t maxViews = uiCaps ? (uint16_t)uiCaps->limits.maxViews : 256;
      const uint16_t captureViewMin = 60;
      const uint16_t captureViewMax = (uint16_t)std::min<int>(maxViews > 1 ? (maxViews - 2) : 0, 198);
      auto allocateCaptureViewId = [&](std::unordered_set<uint16_t>& used) -> uint16_t {
         if (captureViewMax < captureViewMin) return 0;
         for (uint16_t candidate = m_NextUISceneCaptureViewId; candidate <= captureViewMax; candidate = (uint16_t)(candidate + 2)) {
            if (used.find(candidate) == used.end()) {
               m_NextUISceneCaptureViewId = (uint16_t)(candidate + 2);
               return candidate;
            }
         }
         for (uint16_t candidate = captureViewMin; candidate <= captureViewMax; candidate = (uint16_t)(candidate + 2)) {
            if (used.find(candidate) == used.end()) {
               m_NextUISceneCaptureViewId = (uint16_t)(candidate + 2);
               return candidate;
            }
         }
         return 0;
      };
      auto computeBounds = [&](EntityID rootId, bool includeChildren, glm::vec3& outMin, glm::vec3& outMax) -> bool {
         outMin = glm::vec3(std::numeric_limits<float>::max());
         outMax = glm::vec3(std::numeric_limits<float>::lowest());
         bool hasBounds = false;
         std::vector<EntityID> stack;
         stack.push_back(rootId);
         while (!stack.empty()) {
            EntityID id = stack.back();
            stack.pop_back();
            auto* d = scene.GetEntityData(id);
            if (!IsPresentationVisible(d) || !d->Active) continue;
            if (d->Mesh && d->Mesh->mesh) {
               const glm::vec3 lmin = d->Mesh->mesh->BoundsMin;
               const glm::vec3 lmax = d->Mesh->mesh->BoundsMax;
               if (lmax.x > lmin.x && lmax.y > lmin.y && lmax.z > lmin.z) {
                  const glm::mat4& M = d->Transform.WorldMatrix;
                  const glm::vec3 corners[8] = {
                     {lmin.x, lmin.y, lmin.z}, {lmax.x, lmin.y, lmin.z},
                     {lmin.x, lmax.y, lmin.z}, {lmax.x, lmax.y, lmin.z},
                     {lmin.x, lmin.y, lmax.z}, {lmax.x, lmin.y, lmax.z},
                     {lmin.x, lmax.y, lmax.z}, {lmax.x, lmax.y, lmax.z}
                  };
                  for (const auto& c : corners) {
                     glm::vec3 w = glm::vec3(M * glm::vec4(c, 1.0f));
                     outMin = glm::min(outMin, w);
                     outMax = glm::max(outMax, w);
                  }
                  hasBounds = true;
               }
            }
            if (includeChildren) {
               for (EntityID child : d->Children) {
                  stack.push_back(child);
               }
            }
         }
         if (!hasBounds) {
            outMin = glm::vec3(-0.5f);
            outMax = glm::vec3(0.5f);
         }
         return hasBounds;
      };
      auto normalizeSafe = [](const glm::vec3& v, const glm::vec3& fallback) {
         if (glm::length2(v) < 1e-6f) return fallback;
         return glm::normalize(v);
      };

      uint64_t uiSceneCaptureCount = 0;
      for (auto& it : m_ScratchUIItems) {
         if (it.type != UIItemType::Panel) continue;
         EntityData* d = it.data;
         if (!d || !d->Panel || !d->UISceneCapture) continue;
         UISceneCaptureComponent& cap = *d->UISceneCapture;

         if (!cap.Enabled) {
            d->Panel->UseExternalTexture = false;
            d->Panel->ExternalTextureHandle = BGFX_INVALID_HANDLE;
            continue;
         }

         uint32_t renderW = cap.RenderWidth > 0
            ? static_cast<uint32_t>(cap.RenderWidth)
            : static_cast<uint32_t>(std::max(1.0f, it.computedRect.z));
         uint32_t renderH = cap.RenderHeight > 0
            ? static_cast<uint32_t>(cap.RenderHeight)
            : static_cast<uint32_t>(std::max(1.0f, it.computedRect.w));
         if (renderW == 0 || renderH == 0) {
            d->Panel->UseExternalTexture = false;
            d->Panel->ExternalTextureHandle = BGFX_INVALID_HANDLE;
            continue;
         }

         const bool viewIdOutOfRange = (cap._ViewIdBase < captureViewMin
            || cap._ViewIdBase > captureViewMax
            || cap._ViewIdBase + 1 >= maxViews);
         if (cap._ViewIdBase == 0 || viewIdOutOfRange || currentCaptureViewIds.count(cap._ViewIdBase) > 0) {
            cap._ViewIdBase = allocateCaptureViewId(currentCaptureViewIds);
            if (cap._ViewIdBase == 0) {
               d->Panel->UseExternalTexture = false;
               d->Panel->ExternalTextureHandle = BGFX_INVALID_HANDLE;
               continue;
            }
         }
         currentCaptureViewIds.insert(cap._ViewIdBase);

         EntityID targetId = cap.TargetEntity;
         if (targetId != INVALID_ENTITY_ID && !scene.GetEntityData(targetId)) {
            targetId = INVALID_ENTITY_ID;
         }
         if (targetId == INVALID_ENTITY_ID && (cap.TargetGuidHigh != 0 || cap.TargetGuidLow != 0)) {
            ClaymoreGUID guid{};
            guid.high = cap.TargetGuidHigh;
            guid.low = cap.TargetGuidLow;
            targetId = scene.FindEntityByGUID(guid);
            if (targetId != INVALID_ENTITY_ID) {
               cap.TargetEntity = targetId;
            }
         }
         if (targetId != INVALID_ENTITY_ID && (cap.TargetGuidHigh == 0 && cap.TargetGuidLow == 0)) {
            if (auto* td = scene.GetEntityData(targetId)) {
               cap.TargetGuidHigh = td->EntityGuid.high;
               cap.TargetGuidLow = td->EntityGuid.low;
            }
         }

         UISceneCaptureState& state = m_UISceneCaptureStates[it.entityId];
         const bool targetChanged = (targetId != state.lastTargetId)
            || (cap.TargetGuidHigh != state.lastTargetGuidHigh)
            || (cap.TargetGuidLow != state.lastTargetGuidLow);
         const bool shouldFrame = cap.AutoFrame || !state.initialized || targetChanged;

         const float aspect = renderH > 0 ? (float)renderW / (float)renderH : 1.0f;
         float nearClip = std::max(0.001f, cap.NearClip);
         float farClip = cap.FarClip;
         glm::vec3 viewDir = normalizeSafe(cap.ViewDirection, glm::vec3(0.0f, 0.0f, 1.0f));
         glm::vec3 upDir = normalizeSafe(cap.UpDirection, glm::vec3(0.0f, 1.0f, 0.0f));
         if (cap.LockViewToTarget && targetId != INVALID_ENTITY_ID) {
            if (auto* td = scene.GetEntityData(targetId)) {
               const glm::mat4& M = td->Transform.WorldMatrix;
               glm::vec3 right = glm::vec3(M[0]);
               glm::vec3 up = glm::vec3(M[1]);
               glm::vec3 forward = glm::vec3(M[2]);
               auto safeNorm = [](const glm::vec3& v, const glm::vec3& fallback) {
                  return (glm::length2(v) < 1e-6f) ? fallback : glm::normalize(v);
               };
               right = safeNorm(right, glm::vec3(1.0f, 0.0f, 0.0f));
               up = safeNorm(up, glm::vec3(0.0f, 1.0f, 0.0f));
               forward = safeNorm(forward, glm::vec3(0.0f, 0.0f, 1.0f));
               auto rotateLocal = [&](const glm::vec3& v) {
                  return right * v.x + up * v.y + forward * v.z;
               };
               viewDir = normalizeSafe(rotateLocal(cap.ViewDirection), viewDir);
               upDir = normalizeSafe(rotateLocal(cap.UpDirection), upDir);
            }
         }
         if (std::abs(glm::dot(viewDir, upDir)) > 0.98f) {
            upDir = glm::vec3(0.0f, 1.0f, 0.0f);
            if (std::abs(glm::dot(viewDir, upDir)) > 0.98f) {
               upDir = glm::vec3(1.0f, 0.0f, 0.0f);
            }
         }
         glm::vec3 right = glm::normalize(glm::cross(upDir, viewDir));
         upDir = glm::normalize(glm::cross(viewDir, right));

         if (shouldFrame && targetId != INVALID_ENTITY_ID) {
            glm::vec3 boundsMin, boundsMax;
            computeBounds(targetId, cap.IncludeChildren, boundsMin, boundsMax);
            glm::vec3 center = 0.5f * (boundsMin + boundsMax) + cap.FocusOffset;
            glm::vec3 extents = 0.5f * (boundsMax - boundsMin);
            extents = glm::max(extents, glm::vec3(0.001f));

            glm::vec3 absRight = glm::abs(right);
            glm::vec3 absUp = glm::abs(upDir);
            glm::vec3 absView = glm::abs(viewDir);
            float halfWidth = absRight.x * extents.x + absRight.y * extents.y + absRight.z * extents.z;
            float halfHeight = absUp.x * extents.x + absUp.y * extents.y + absUp.z * extents.z;
            float halfDepth = absView.x * extents.x + absView.y * extents.y + absView.z * extents.z;

            float fovRad = glm::radians(glm::clamp(cap.FieldOfView, 5.0f, 179.0f));
            float tanHalfFov = std::tan(fovRad * 0.5f);
            float distHeight = halfHeight / tanHalfFov;
            float distWidth = halfWidth / (tanHalfFov * aspect);
            float distance = std::max(distHeight, distWidth);
            float padding = std::max(0.1f, cap.BoundsPadding);
            distance *= padding;

            float suggestedFar = distance + halfDepth * 4.0f;
            if (farClip <= nearClip) {
               farClip = suggestedFar;
            } else {
               farClip = std::max(farClip, suggestedFar);
            }
            if (farClip <= nearClip + 0.01f) {
               farClip = nearClip + std::max(1.0f, distance);
            }

            state.camera.SetViewportSize((float)renderW, (float)renderH);
            state.camera.SetPerspective(cap.FieldOfView, aspect, nearClip, farClip);
            state.camera.SetPosition(center + viewDir * distance);
            state.camera.LookAt(center, upDir);
         } else if (!state.initialized) {
            glm::vec3 center = cap.FocusOffset;
            float distance = 5.0f;
            if (farClip <= nearClip) farClip = nearClip + 100.0f;
            state.camera.SetViewportSize((float)renderW, (float)renderH);
            state.camera.SetPerspective(cap.FieldOfView, aspect, nearClip, farClip);
            state.camera.SetPosition(center + viewDir * distance);
            state.camera.LookAt(center, upDir);
         } else {
            if (farClip <= nearClip) farClip = nearClip + 100.0f;
            state.camera.SetViewportSize((float)renderW, (float)renderH);
            state.camera.SetPerspective(cap.FieldOfView, aspect, nearClip, farClip);
         }

         state.lastTargetId = targetId;
         state.lastTargetGuidHigh = cap.TargetGuidHigh;
         state.lastTargetGuidLow = cap.TargetGuidLow;
         state.lastWidth = renderW;
         state.lastHeight = renderH;
         state.initialized = true;

        std::unordered_set<EntityID> allowedEntities;
        const std::unordered_set<EntityID>* allowedPtr = nullptr;
        if (targetId != INVALID_ENTITY_ID) {
           std::vector<EntityID> stack;
           stack.push_back(targetId);
           while (!stack.empty()) {
              EntityID id = stack.back();
              stack.pop_back();
              if (allowedEntities.insert(id).second && cap.IncludeChildren) {
                 if (auto* td = scene.GetEntityData(id)) {
                    for (EntityID child : td->Children) {
                       stack.push_back(child);
                    }
                 }
              }
           }
           allowedPtr = &allowedEntities;
        }

        bgfx::TextureHandle capTex = RenderSceneToTexture(&scene, renderW, renderH, &state.camera,
           cap._ViewIdBase, cap.ShowGrid, cap.ClearColor, false, allowedPtr, true);
         ++uiSceneCaptureCount;
         if (bgfx::isValid(capTex)) {
            d->Panel->UseExternalTexture = true;
            d->Panel->ExternalTextureHandle = capTex;
         } else {
            d->Panel->UseExternalTexture = false;
            d->Panel->ExternalTextureHandle = BGFX_INVALID_HANDLE;
         }
      }
      Profiler::Get().SetCounter("Render/UISceneCaptures", uiSceneCaptureCount);

      if (releaseUnusedOffscreenTargets) {
         for (uint16_t viewId : m_UISceneCaptureViewIds) {
            if (currentCaptureViewIds.find(viewId) == currentCaptureViewIds.end()) {
               ReleaseOffscreenTarget(viewId);
            }
         }
         m_UISceneCaptureViewIds = currentCaptureViewIds;
      }

      // Resolve ScrollView content bounds after layout has settled.
      // We only update runtime scroll metrics when the effective content or
      // viewport changes so layout-driven scroll views do not churn state.
      auto nearlyEqualFloat = [](float a, float b, float epsilon = 0.01f) {
         return std::abs(a - b) <= epsilon;
      };
      auto nearlyEqualVec2 = [&](const glm::vec2& a, const glm::vec2& b, float epsilon = 0.01f) {
         return nearlyEqualFloat(a.x, b.x, epsilon) && nearlyEqualFloat(a.y, b.y, epsilon);
      };
      constexpr float kScrollOverflowEpsilon = 0.5f;

      for (auto& it : m_ScratchUIItems) {
         if (it.type != UIItemType::Panel || !it.data || !it.data->ScrollView) {
            continue;
         }

         ScrollViewComponent& sv = *it.data->ScrollView;
         glm::vec2 viewportSize(
            std::max(0.0f, it.computedRect.z),
            std::max(0.0f, it.computedRect.w)
         );

         bool layoutDrivesContentSize = (it.data->LayoutGroup != nullptr);
         glm::vec2 resolvedContentSize(0.0f);
         if (layoutDrivesContentSize) {
            resolvedContentSize = it.data->LayoutGroup->_CalculatedContentSize;
         } else {
            glm::vec2 canvasScale = getCanvasScale(it.canvas);
            resolvedContentSize = glm::vec2(
               std::max(0.0f, sv.ContentSize.x * canvasScale.x),
               std::max(0.0f, sv.ContentSize.y * canvasScale.y)
            );
         }

         resolvedContentSize.x = std::max(0.0f, resolvedContentSize.x);
         resolvedContentSize.y = std::max(0.0f, resolvedContentSize.y);

         const bool hasHorizontalOverflow = sv.HorizontalScroll &&
            resolvedContentSize.x > viewportSize.x + kScrollOverflowEpsilon;
         const bool hasVerticalOverflow = sv.VerticalScroll &&
            resolvedContentSize.y > viewportSize.y + kScrollOverflowEpsilon;

         const bool metricsChanged =
            !nearlyEqualVec2(sv._ResolvedContentSizeScreen, resolvedContentSize) ||
            !nearlyEqualVec2(sv._ResolvedViewportSizeScreen, viewportSize) ||
            sv._HasHorizontalOverflow != hasHorizontalOverflow ||
            sv._HasVerticalOverflow != hasVerticalOverflow ||
            sv._LayoutGroupDrivesContentSize != layoutDrivesContentSize;

         if (!metricsChanged) {
            continue;
         }

         sv._ResolvedContentSizeScreen = resolvedContentSize;
         sv._ResolvedViewportSizeScreen = viewportSize;
         sv._HasHorizontalOverflow = hasHorizontalOverflow;
         sv._HasVerticalOverflow = hasVerticalOverflow;
         sv._LayoutGroupDrivesContentSize = layoutDrivesContentSize;

         const float maxScrollX = hasHorizontalOverflow
            ? std::max(0.0f, resolvedContentSize.x - viewportSize.x)
            : 0.0f;
         const float maxScrollY = hasVerticalOverflow
            ? std::max(0.0f, resolvedContentSize.y - viewportSize.y)
            : 0.0f;

         sv.ContentOffset.x = std::clamp(sv.ContentOffset.x, 0.0f, maxScrollX);
         sv.ContentOffset.y = std::clamp(sv.ContentOffset.y, 0.0f, maxScrollY);
      }

      // Build final draw order separately from layout order.
      // Layout needs parents computed before children, but rendering/input should
      // walk sibling subtrees in Z order so a higher-Z panel stays above another
      // panel and all of that panel's descendants.
      struct UICanvasDrawGroup {
         int CanvasOrder = 0;
         size_t FirstSceneOrder = std::numeric_limits<size_t>::max();
         std::unordered_map<EntityID, std::vector<size_t>> ItemsByEntity;
      };

      auto uiTypePriority = [](UIItemType type) -> int {
         switch (type) {
         case UIItemType::Panel:
            return 0;
         case UIItemType::LayoutGroup:
            return 1;
         case UIItemType::Text:
            return 2;
         default:
            return 3;
         }
      };

      auto itemIndexLess = [&](size_t aIndex, size_t bIndex) {
         const UIDrawItem& a = m_ScratchUIItems[aIndex];
         const UIDrawItem& b = m_ScratchUIItems[bIndex];
         if (a.z != b.z) {
            return a.z < b.z;
         }
         const int aPriority = uiTypePriority(a.type);
         const int bPriority = uiTypePriority(b.type);
         if (aPriority != bPriority) {
            return aPriority < bPriority;
         }
         return aIndex < bIndex;
      };

      auto entityLocalZ = [&](EntityID entityId, const std::unordered_map<EntityID, std::vector<size_t>>& itemsByEntity) -> int {
         auto it = itemsByEntity.find(entityId);
         if (it == itemsByEntity.end() || it->second.empty()) {
            return 0;
         }

         bool hasPanel = false;
         int preferredZ = 0;
         bool hasAny = false;
         int fallbackZ = 0;

         for (size_t itemIndex : it->second) {
            const UIDrawItem& item = m_ScratchUIItems[itemIndex];
            if (!hasAny) {
               fallbackZ = item.z;
               hasAny = true;
            }
            if (item.type == UIItemType::Panel) {
               if (!hasPanel) {
                  preferredZ = item.z;
                  hasPanel = true;
               } else {
                  preferredZ = std::min(preferredZ, item.z);
               }
            }
         }

         if (hasPanel) {
            return preferredZ;
         }

         return fallbackZ;
      };

      auto entityDrawLess = [&](EntityID a, EntityID b, const std::unordered_map<EntityID, std::vector<size_t>>& itemsByEntity) {
         const int aZ = entityLocalZ(a, itemsByEntity);
         const int bZ = entityLocalZ(b, itemsByEntity);
         if (aZ != bZ) {
            return aZ < bZ;
         }

         const size_t aOrder = uiEntityOrder.count(a) ? uiEntityOrder[a] : std::numeric_limits<size_t>::max();
         const size_t bOrder = uiEntityOrder.count(b) ? uiEntityOrder[b] : std::numeric_limits<size_t>::max();
         return aOrder < bOrder;
      };

      std::vector<UICanvasDrawGroup> uiDrawGroups;
      uiDrawGroups.reserve(m_ScratchUIItems.size());
      std::unordered_map<const CanvasComponent*, size_t> uiDrawGroupIndex;
      uiDrawGroupIndex.reserve(m_ScratchUIItems.size());

      for (size_t itemIndex = 0; itemIndex < m_ScratchUIItems.size(); ++itemIndex) {
         const UIDrawItem& item = m_ScratchUIItems[itemIndex];
         auto [groupIt, inserted] = uiDrawGroupIndex.emplace(item.canvas, uiDrawGroups.size());
         if (inserted) {
            UICanvasDrawGroup group;
            group.CanvasOrder = item.canvasOrder;
            uiDrawGroups.push_back(std::move(group));
         }

         UICanvasDrawGroup& group = uiDrawGroups[groupIt->second];
         group.CanvasOrder = item.canvasOrder;
         group.ItemsByEntity[item.entityId].push_back(itemIndex);

         auto orderIt = uiEntityOrder.find(item.entityId);
         if (orderIt != uiEntityOrder.end()) {
            group.FirstSceneOrder = std::min(group.FirstSceneOrder, orderIt->second);
         }
      }

      for (UICanvasDrawGroup& group : uiDrawGroups) {
         for (auto& [entityId, itemIndices] : group.ItemsByEntity) {
            std::sort(itemIndices.begin(), itemIndices.end(), itemIndexLess);
         }
      }

      std::sort(uiDrawGroups.begin(), uiDrawGroups.end(), [](const UICanvasDrawGroup& a, const UICanvasDrawGroup& b) {
         if (a.CanvasOrder != b.CanvasOrder) {
            return a.CanvasOrder < b.CanvasOrder;
         }
         return a.FirstSceneOrder < b.FirstSceneOrder;
      });

      auto collectDirectGroupChildren = [&](auto&& self, EntityID entityId,
                                            const std::unordered_map<EntityID, std::vector<size_t>>& itemsByEntity,
                                            std::vector<EntityID>& outChildren) -> void {
         auto* entityData = scene.GetEntityData(entityId);
         if (!entityData) {
            return;
         }

         for (EntityID childId : entityData->Children) {
            if (itemsByEntity.find(childId) != itemsByEntity.end()) {
               outChildren.push_back(childId);
            } else {
               self(self, childId, itemsByEntity, outChildren);
            }
         }
      };

      auto appendEntityDrawSubtree = [&](auto&& self, EntityID entityId,
                                         const std::unordered_map<EntityID, std::vector<size_t>>& itemsByEntity) -> void {
         auto itemIt = itemsByEntity.find(entityId);
         if (itemIt != itemsByEntity.end()) {
            for (size_t itemIndex : itemIt->second) {
               m_ScratchUIDrawOrder.push_back(itemIndex);
            }
         }

         std::vector<EntityID> childEntities;
         collectDirectGroupChildren(collectDirectGroupChildren, entityId, itemsByEntity, childEntities);
         std::sort(childEntities.begin(), childEntities.end(),
            [&](EntityID a, EntityID b) { return entityDrawLess(a, b, itemsByEntity); });

         for (EntityID childId : childEntities) {
            self(self, childId, itemsByEntity);
         }
      };

      for (const UICanvasDrawGroup& group : uiDrawGroups) {
         std::vector<EntityID> roots;
         roots.reserve(group.ItemsByEntity.size());

         for (const auto& entity : entities) {
            auto* entityData = scene.GetEntityData(entity.GetID());
            if (!entityData || entityData->Parent != INVALID_ENTITY_ID) {
               continue;
            }

            if (group.ItemsByEntity.find(entity.GetID()) != group.ItemsByEntity.end()) {
               roots.push_back(entity.GetID());
               continue;
            }

            collectDirectGroupChildren(collectDirectGroupChildren, entity.GetID(), group.ItemsByEntity, roots);
         }

         std::sort(roots.begin(), roots.end(),
            [&](EntityID a, EntityID b) { return entityDrawLess(a, b, group.ItemsByEntity); });

         for (EntityID rootId : roots) {
            appendEntityDrawSubtree(appendEntityDrawSubtree, rootId, group.ItemsByEntity);
         }
      }

      // Render pass: draw items using computed rects
      // Track current scissor state for ScrollView clipping
      uint16_t currentScissorHandle = UINT16_MAX; // UINT16_MAX means no scissor
      EntityID dropdownInputCaptureEntity = INVALID_ENTITY_ID;

      if (allowInput) {
         for (auto rit = m_ScratchUIDrawOrder.rbegin(); rit != m_ScratchUIDrawOrder.rend(); ++rit) {
            const UIDrawItem& captureItem = m_ScratchUIItems[*rit];
            if (captureItem.type != UIItemType::Panel || !captureItem.data || !captureItem.data->Dropdown) {
               continue;
            }

            DropdownComponent& captureDropdown = *captureItem.data->Dropdown;
            if (!captureDropdown.Interactable || !captureDropdown.IsOpen) {
               continue;
            }

            auto [captureScrollViewEntity, captureScrollViewRect] = findParentScrollView(captureItem.entityId);
            glm::vec2 captureScrollOffset(0.0f, 0.0f);
            if (captureScrollViewEntity != INVALID_ENTITY_ID) {
               captureScrollOffset = findScrollOffset(captureItem.entityId);
            }

            float captureX0 = captureItem.computedRect.x - captureScrollOffset.x;
            float captureY0 = captureItem.computedRect.y - captureScrollOffset.y;
            float captureX1 = captureX0 + captureItem.computedRect.z;
            float captureY1 = captureY0 + captureItem.computedRect.w;

            bool insideCaptureMain = (mx >= captureX0 && mx <= captureX1 && my >= captureY0 && my <= captureY1);
            if (insideCaptureMain && captureScrollViewEntity != INVALID_ENTITY_ID) {
               insideCaptureMain = (mx >= captureScrollViewRect.x && mx <= captureScrollViewRect.x + captureScrollViewRect.z &&
                                    my >= captureScrollViewRect.y && my <= captureScrollViewRect.y + captureScrollViewRect.w);
            }

            int visibleOptions = std::min((int)captureDropdown.Options.size(), captureDropdown.MaxVisibleOptions);
            float listHeight = visibleOptions * captureDropdown.OptionHeight;
            float listY0 = captureY1;
            float listY1 = listY0 + listHeight;
            bool insideCaptureList = visibleOptions > 0 &&
                                     (mx >= captureX0 && mx <= captureX1 && my >= listY0 && my <= listY1);

            if (insideCaptureMain || insideCaptureList) {
               dropdownInputCaptureEntity = captureItem.entityId;
               break;
            }
         }
      }
      
      for (size_t drawItemIndex : m_ScratchUIDrawOrder) {
         const UIDrawItem& it = m_ScratchUIItems[drawItemIndex];
         // Check if this item is inside a ScrollView
         auto [scrollViewEntity, scrollViewRect] = findParentScrollView(it.entityId);
         glm::vec2 scrollOffset(0.0f, 0.0f);
         uint16_t scissorHandle = UINT16_MAX;
         
         if (scrollViewEntity != INVALID_ENTITY_ID) {
            // Get scroll offset from the ScrollView
            scrollOffset = findScrollOffset(it.entityId);
            
            // Compute scissor rect (intersection of ScrollView bounds with screen)
            float sx0 = std::max(0.0f, scrollViewRect.x);
            float sy0 = std::max(0.0f, scrollViewRect.y);
            float sx1 = std::min((float)m_Width, scrollViewRect.x + scrollViewRect.z);
            float sy1 = std::min((float)m_Height, scrollViewRect.y + scrollViewRect.w);
            
            // Cache scissor rect for this draw
            if (sx1 > sx0 && sy1 > sy0) {
               scissorHandle = bgfx::setScissor(
                  (uint16_t)sx0, (uint16_t)sy0, 
                  (uint16_t)(sx1 - sx0), (uint16_t)(sy1 - sy0)
               );
            }
         }
         
         if (it.type == UIItemType::Panel) {
            EntityData* d = it.data;
            PanelComponent& p = *it.panel;
            glm::vec2 canvasScale = getCanvasScale(it.canvas);
            
            // Use pre-computed rect from layout pass, apply scroll offset
            float x0 = it.computedRect.x - scrollOffset.x;
            float y0 = it.computedRect.y - scrollOffset.y;
            float x1 = x0 + it.computedRect.z;
            float y1 = y0 + it.computedRect.w;

            if (allowInput) {
               bool insidePanel = (mx >= x0 && mx <= x1 && my >= y0 && my <= y1);
               if (insidePanel && scrollViewEntity != INVALID_ENTITY_ID) {
                  insidePanel = (mx >= scrollViewRect.x && mx <= scrollViewRect.x + scrollViewRect.z &&
                                 my >= scrollViewRect.y && my <= scrollViewRect.y + scrollViewRect.w);
               }

               p.Hovered = insidePanel;
               p.Pressed = insidePanel && mouseDown;
               p.DragStarted = false;
               p.DragEnded = false;
               p.Dropped = false;
               p.DropSourceEntity = -1;
               p.DropTargetEntity = -1;

               if (p.AllowDrag && insidePanel && mousePressed && m_UIDragSource == INVALID_ENTITY_ID) {
                  m_UIDragSource = it.entityId;
                  m_UIDragActive = true;
                  p.DragStarted = true;
               }
               p.Dragging = (m_UIDragSource == it.entityId && mouseDown);
               if (mouseReleased && m_UIDragSource == it.entityId) {
                  p.DragEnded = true;
               }

               if (mouseReleased && m_UIDragSource != INVALID_ENTITY_ID && p.AllowDrop && insidePanel) {
                  dropTargetCandidate = it.entityId;
               }
            }
            
            // Calculate pivot point for rotation
            float pivotX = x0 + (x1 - x0) * p.Pivot.x;
            float pivotY = y0 + (y1 - y0) * p.Pivot.y;
            
            // Helper function to rotate a 2D point around the pivot
            auto rotatePoint = [&](float x, float y) -> glm::vec2 {
               if (std::abs(p.Rotation) < 0.001f) {
                  return glm::vec2(x, y);
               }
               float rad = glm::radians(p.Rotation);
               float cosR = std::cos(rad);
               float sinR = std::sin(rad);
               float dx = x - pivotX;
               float dy = y - pivotY;
               return glm::vec2(
                  pivotX + dx * cosR - dy * sinR,
                  pivotY + dx * sinR + dy * cosR
               );
            };

            // Base tint: panel tint
            uint32_t abgr = 0xffffffffu;
            auto clamp01 = [](float v) { return std::max(0.0f, std::min(1.0f, v)); };
            glm::vec4 tint = p.TintColor;
            if (d->Button) 
            {
               if (d->Button->Pressed)       { tint *= d->Button->PressedTint; }
               else if (d->Button->Hovered)  { tint *= d->Button->HoverTint;   }
               else                          { tint *= d->Button->NormalTint;  }
            }
            
            // Dropdown visual state tinting (when used as main panel)
            if (d->Dropdown && d->Dropdown->Interactable)
            {
               if (d->Dropdown->IsOpen)         { tint *= glm::vec4(d->Dropdown->OptionSelectedColor); }
               else if (d->Dropdown->Hovered)   { tint *= glm::vec4(d->Dropdown->OptionHoverColor);    }
               else                             { tint *= glm::vec4(d->Dropdown->OptionNormalColor);   }
            }

            uint8_t r = (uint8_t)(clamp01(tint.r) * 255.0f);
            uint8_t g = (uint8_t)(clamp01(tint.g) * 255.0f);
            uint8_t b = (uint8_t)(clamp01(tint.b) * 255.0f);
            uint8_t a = (uint8_t)(clamp01(tint.a * p.Opacity * it.canvasOpacity) * 255.0f);
            abgr = (a << 24) | (b << 16) | (g << 8) | (r);

            UIVertex verts[4];
            uint16_t idx[6] = { 0,1,2, 0,2,3 };
            const bool forceExternalStretch = (d->UISceneCapture && p.UseExternalTexture);
            if (!forceExternalStretch && p.Mode == PanelComponent::FillMode::NineSlice && p.Texture.IsValid()) {
               float L = x0, T = y0, R = x1, B = y1;
               float w = (x1 - x0), h = (y1 - y0);
               float uL = p.UVRect.x, vT = p.UVRect.y, uR = p.UVRect.z, vB = p.UVRect.w;
               
               // Compute pixel border sizes with per-axis fallback
               // If SliceBorder[i] > 0, use it directly; otherwise derive from SliceUV[i]
               float du = (uR - uL);
               float dv = (vB - vT);
               
               // Per-axis calculation: pixel border if set, otherwise percentage of dimension from UV
               float lpx = (p.SliceBorder.x > 0) ? p.SliceBorder.x : 
                           ((du != 0.0f) ? w * (p.SliceUV.x / du) : 0.0f);
               float tpx = (p.SliceBorder.y > 0) ? p.SliceBorder.y : 
                           ((dv != 0.0f) ? h * (p.SliceUV.y / dv) : 0.0f);
               float rpx = (p.SliceBorder.z > 0) ? p.SliceBorder.z : 
                           ((du != 0.0f) ? w * (p.SliceUV.z / du) : 0.0f);
               float bpx = (p.SliceBorder.w > 0) ? p.SliceBorder.w : 
                           ((dv != 0.0f) ? h * (p.SliceUV.w / dv) : 0.0f);
               
               // Clamp borders so they don't exceed panel size
               if (lpx + rpx > w) { float scale = w / (lpx + rpx); lpx *= scale; rpx *= scale; }
               if (tpx + bpx > h) { float scale = h / (tpx + bpx); tpx *= scale; bpx *= scale; }

               float xL = L;
               float xM = L + lpx;
               float xR = R - rpx;
               float yT = T;
               float yM = T + tpx;
               float yB = B - bpx;

               float uL2 = uL + p.SliceUV.x;
               float uR2 = uR - p.SliceUV.z;
               float vT2 = vT + p.SliceUV.y;
               float vB2 = vB - p.SliceUV.w;

               // Resolve texture once for all 9 slices (moved outside lambda for efficiency)
               bgfx::TextureHandle nineSliceTex = m_UIWhiteTex;
               if (p.UseExternalTexture && bgfx::isValid(p.ExternalTextureHandle)) {
                  nineSliceTex = p.ExternalTextureHandle;
               }
               else if (p.Texture.IsValid()) {
#ifndef CLAYMORE_RUNTIME
                  if (auto* entry = AssetLibrary::Instance().GetAsset(p.Texture)) {
                     if (!entry->texture || !bgfx::isValid(*entry->texture)) {
                        auto tex = AssetLibrary::Instance().LoadTexture(p.Texture);
                        (void)tex;
                     }
                     if (entry->texture && bgfx::isValid(*entry->texture)) nineSliceTex = *entry->texture;
                  }
#else
                  // Runtime: resolve GUID to path and load texture
                  if (IAssetResolver* resolver = Assets::GetResolver()) {
                     std::string texPath = resolver->GetPathForGUID(p.Texture.guid);
                     if (!texPath.empty()) {
                        TextureSpecifier spec;
                        spec.Path = texPath;
                        p.CachedTextureHandle = AcquireTextureHandle(spec, TextureColorSpace::Linear);
                        if (bgfx::isValid(p.CachedTextureHandle)) nineSliceTex = p.CachedTextureHandle;
                     } else {
                        p.CachedTextureHandle = BGFX_INVALID_HANDLE;
                     }
                  }
#endif
               }
               
               // OPTIMIZATION: Use transient buffers instead of create/destroy per quad
               auto submitQuad = [&](float xa, float ya, float xb, float yb, float ua, float va, float ub, float vb) {
                  // Apply rotation to all four corners
                  glm::vec2 v0 = rotatePoint(xa, ya);
                  glm::vec2 v1 = rotatePoint(xb, ya);
                  glm::vec2 v2 = rotatePoint(xb, yb);
                  glm::vec2 v3 = rotatePoint(xa, yb);
                  
                  UIVertex vv[4] = {
                      { v0.x, v0.y, 0.0f, ua, va, abgr },
                      { v1.x, v1.y, 0.0f, ub, va, abgr },
                      { v2.x, v2.y, 0.0f, ub, vb, abgr },
                      { v3.x, v3.y, 0.0f, ua, vb, abgr }
                  };
                  uint16_t ii[6] = { 0,1,2, 0,2,3 };
                  submitUIQuadTransient(vv, ii, 4, 6, nineSliceTex, uiViewId);
               };

               submitQuad(xL, yT, xM, yM, uL, vT, uL2, vT2);
               submitQuad(xM, yT, xR, yM, uL2, vT, uR2, vT2);
               submitQuad(xR, yT, R, yM, uR2, vT, uR, vT2);
               submitQuad(xL, yM, xM, yB, uL, vT2, uL2, vB2);
               submitQuad(xM, yM, xR, yB, uL2, vT2, uR2, vB2);
               submitQuad(xR, yM, R, yB, uR2, vT2, uR, vB2);
               submitQuad(xL, yB, xM, B, uL, vB2, uL2, vB);
               submitQuad(xM, yB, xR, B, uL2, vB2, uR2, vB);
               submitQuad(xR, yB, R, B, uR2, vB2, uR, vB);
               // NineSlice already rendered - skip single-quad path but continue to progress bar
            }
            else {
               // Single-quad rendering for Tile and Stretch modes
               // Apply rotation to all four corners
               glm::vec2 v0 = rotatePoint(x0, y0);
               glm::vec2 v1 = rotatePoint(x1, y0);
               glm::vec2 v2 = rotatePoint(x1, y1);
               glm::vec2 v3 = rotatePoint(x0, y1);
               
               if (!forceExternalStretch && p.Mode == PanelComponent::FillMode::Tile) {
                  float u0 = p.UVRect.x, uv0 = p.UVRect.y;
                  float u1 = p.UVRect.z * p.TileRepeat.x, uv1 = p.UVRect.w * p.TileRepeat.y;
                  verts[0] = { v0.x, v0.y, 0.0f, u0, uv0, abgr };
                  verts[1] = { v1.x, v1.y, 0.0f, u1, uv0, abgr };
                  verts[2] = { v2.x, v2.y, 0.0f, u1, uv1, abgr };
                  verts[3] = { v3.x, v3.y, 0.0f, u0, uv1, abgr };
               }
               else {
                  verts[0] = { v0.x, v0.y, 0.0f, p.UVRect.x, p.UVRect.y, abgr };
                  verts[1] = { v1.x, v1.y, 0.0f, p.UVRect.z, p.UVRect.y, abgr };
                  verts[2] = { v2.x, v2.y, 0.0f, p.UVRect.z, p.UVRect.w, abgr };
                  verts[3] = { v3.x, v3.y, 0.0f, p.UVRect.x, p.UVRect.w, abgr };
               }
               // OPTIMIZATION: Use transient buffers instead of create/destroy per quad
               bgfx::TextureHandle th = m_UIWhiteTex;
               if (p.UseExternalTexture && bgfx::isValid(p.ExternalTextureHandle)) {
                  th = p.ExternalTextureHandle;
               }
               else if (p.Texture.IsValid()) {
#ifndef CLAYMORE_RUNTIME
                  if (auto* entry = AssetLibrary::Instance().GetAsset(p.Texture)) {
                     if (!entry->texture || !bgfx::isValid(*entry->texture)) {
                        auto tex = AssetLibrary::Instance().LoadTexture(p.Texture);
                        (void)tex;
                     }
                     if (entry->texture && bgfx::isValid(*entry->texture)) th = *entry->texture;
                  }
#else
                  // Runtime: resolve GUID to path and load texture
                  if (IAssetResolver* resolver = Assets::GetResolver()) {
                     std::string texPath = resolver->GetPathForGUID(p.Texture.guid);
                     if (!texPath.empty()) {
                        TextureSpecifier spec;
                        spec.Path = texPath;
                        p.CachedTextureHandle = AcquireTextureHandle(spec, TextureColorSpace::Linear);
                        if (bgfx::isValid(p.CachedTextureHandle)) th = p.CachedTextureHandle;
                     } else {
                        p.CachedTextureHandle = BGFX_INVALID_HANDLE;
                     }
                  }
#endif
               }
               submitUIQuadTransient(verts, idx, 4, 6, th, 2, scissorHandle);
            }

            // Button hit-testing overlay (use scroll-adjusted coordinates)
            if (d->Button && d->Button->Interactable) {
               bool inside = (mx >= x0 && mx <= x1 && my >= y0 && my <= y1);
               
               // If inside a ScrollView, also check if within scissor bounds
               if (inside && scrollViewEntity != INVALID_ENTITY_ID) {
                  inside = (mx >= scrollViewRect.x && mx <= scrollViewRect.x + scrollViewRect.z &&
                            my >= scrollViewRect.y && my <= scrollViewRect.y + scrollViewRect.w);
               }
               d->Button->Hovered = inside;
               
               if (inside) { m_UIInputConsumed = true; }
               
               bool wasPressed = d->Button->Pressed;
               
               d->Button->Pressed = inside && mouseDown;
               d->Button->Clicked = (!mouseDown && wasPressed && inside);
               
               if (d->Button->Toggle && d->Button->Clicked) 
               { 
                  d->Button->Toggled = !d->Button->Toggled; 
               }
            }

            // Slider rendering and interaction
            if (d->Slider && d->Slider->Visible) {
               SliderComponent& sl = *d->Slider;
               sl.ValueChanged = false;

               float range = sl.MaxValue - sl.MinValue;
               float normalizedValue = (range > 0.0001f) ? (sl.Value - sl.MinValue) / range : 0.0f;
               normalizedValue = std::clamp(normalizedValue, 0.0f, 1.0f);

               float handleW = std::max(8.0f, sl.HandleSize.x * canvasScale.x);
               float handleH = std::max(8.0f, sl.HandleSize.y * canvasScale.y);
               float padX = std::max(2.0f, 2.0f * canvasScale.x);
               float padY = std::max(2.0f, 2.0f * canvasScale.y);

               float trackX0 = x0 + padX;
               float trackY0 = y0 + padY;
               float trackX1 = x1 - padX;
               float trackY1 = y1 - padY;

               bool insideSlider = (mx >= x0 && mx <= x1 && my >= y0 && my <= y1);
               if (insideSlider && scrollViewEntity != INVALID_ENTITY_ID) {
                  insideSlider = (mx >= scrollViewRect.x && mx <= scrollViewRect.x + scrollViewRect.z &&
                                 my >= scrollViewRect.y && my <= scrollViewRect.y + scrollViewRect.w);
               }

               if (!mouseDown) {
                  sl.Dragging = false;
               }

               sl.Hovered = insideSlider;

               auto snapSliderValue = [&](float value) -> float {
                  float result = value;
                  if (sl.WholeNumbers) {
                     result = std::round(result);
                  } else if (sl.Step > 0.0001f) {
                     result = sl.MinValue + std::round((result - sl.MinValue) / sl.Step) * sl.Step;
                  }
                  return std::clamp(result, sl.MinValue, sl.MaxValue);
               };

               auto updateSliderValueFromMouse = [&](float mouseX, float mouseY) {
                  float newNormalized = normalizedValue;
                  if (sl.SliderDirection == SliderComponent::Direction::Horizontal) {
                     float usableStart = trackX0 + handleW * 0.5f;
                     float usableEnd = trackX1 - handleW * 0.5f;
                     if (usableEnd <= usableStart) {
                        usableStart = trackX0;
                        usableEnd = trackX1;
                     }
                     float denom = std::max(usableEnd - usableStart, 0.0001f);
                     newNormalized = (mouseX - usableStart) / denom;
                  } else {
                     float usableStart = trackY1 - handleH * 0.5f;
                     float usableEnd = trackY0 + handleH * 0.5f;
                     if (usableStart <= usableEnd) {
                        usableStart = trackY1;
                        usableEnd = trackY0;
                     }
                     float denom = std::max(usableStart - usableEnd, 0.0001f);
                     newNormalized = (usableStart - mouseY) / denom;
                  }

                  newNormalized = std::clamp(newNormalized, 0.0f, 1.0f);
                  float newValue = snapSliderValue(sl.MinValue + (sl.MaxValue - sl.MinValue) * newNormalized);
                  if (std::abs(newValue - sl.Value) > 0.0001f) {
                     sl.Value = newValue;
                     sl.ValueChanged = true;
                  }

                  normalizedValue = (range > 0.0001f) ? (sl.Value - sl.MinValue) / range : 0.0f;
                  normalizedValue = std::clamp(normalizedValue, 0.0f, 1.0f);
               };

               if (allowInput && sl.Interactable) {
                  if ((insideSlider && mousePressed) || (sl.Dragging && mouseDown)) {
                     sl.Dragging = mouseDown;
                     updateSliderValueFromMouse(mx, my);
                     m_UIInputConsumed = true;
                  } else if (insideSlider || sl.Dragging) {
                     m_UIInputConsumed = true;
                  }
               }

               if (sl.ShowFill) {
                  float fillX0 = trackX0;
                  float fillY0 = trackY0;
                  float fillX1 = trackX1;
                  float fillY1 = trackY1;

                  if (sl.SliderDirection == SliderComponent::Direction::Horizontal) {
                     float usableStart = trackX0 + handleW * 0.5f;
                     float usableEnd = trackX1 - handleW * 0.5f;
                     if (usableEnd <= usableStart) {
                        usableStart = trackX0;
                        usableEnd = trackX1;
                     }
                     float handleCenterX = usableStart + (usableEnd - usableStart) * normalizedValue;
                     fillX1 = std::clamp(handleCenterX, trackX0, trackX1);
                  } else {
                     float usableStart = trackY1 - handleH * 0.5f;
                     float usableEnd = trackY0 + handleH * 0.5f;
                     if (usableStart <= usableEnd) {
                        usableStart = trackY1;
                        usableEnd = trackY0;
                     }
                     float handleCenterY = usableStart - (usableStart - usableEnd) * normalizedValue;
                     fillY0 = std::clamp(handleCenterY, trackY0, trackY1);
                  }

                  glm::vec4 fillColor = sl.FillColor;
                  uint8_t fr = (uint8_t)(clamp01(fillColor.r) * 255.0f);
                  uint8_t fg = (uint8_t)(clamp01(fillColor.g) * 255.0f);
                  uint8_t fb = (uint8_t)(clamp01(fillColor.b) * 255.0f);
                  uint8_t fa = (uint8_t)(clamp01(fillColor.a * sl.Opacity * it.canvasOpacity) * 255.0f);
                  uint32_t fillABGR = (fa << 24) | (fb << 16) | (fg << 8) | fr;

                  UIVertex fillVerts[4] = {
                     { fillX0, fillY0, 0.0f, 0.0f, 0.0f, fillABGR },
                     { fillX1, fillY0, 0.0f, 1.0f, 0.0f, fillABGR },
                     { fillX1, fillY1, 0.0f, 1.0f, 1.0f, fillABGR },
                     { fillX0, fillY1, 0.0f, 0.0f, 1.0f, fillABGR }
                  };
                  uint16_t fillIdx[6] = { 0,1,2, 0,2,3 };

                  bgfx::TextureHandle fillTh = m_UIWhiteTex;
#ifndef CLAYMORE_RUNTIME
                  if (sl.FillTexture.IsValid()) {
                     if (auto* entry = AssetLibrary::Instance().GetAsset(sl.FillTexture)) {
                        if (!entry->texture || !bgfx::isValid(*entry->texture)) {
                           auto tex = AssetLibrary::Instance().LoadTexture(sl.FillTexture);
                           (void)tex;
                        }
                        if (entry->texture && bgfx::isValid(*entry->texture)) fillTh = *entry->texture;
                     }
                  }
#endif
                  submitUIQuadTransient(fillVerts, fillIdx, 4, 6, fillTh, 2, scissorHandle);
               }

               glm::vec4 handleTint = sl.Interactable
                  ? (sl.Dragging ? sl.HandlePressedTint : (sl.Hovered ? sl.HandleHoverTint : sl.HandleNormalTint))
                  : sl.HandleDisabledTint;

               float handleX0 = trackX0;
               float handleY0 = trackY0;
               float handleX1 = trackX0 + handleW;
               float handleY1 = trackY0 + handleH;

               if (sl.SliderDirection == SliderComponent::Direction::Horizontal) {
                  float usableStart = trackX0 + handleW * 0.5f;
                  float usableEnd = trackX1 - handleW * 0.5f;
                  if (usableEnd <= usableStart) {
                     usableStart = trackX0;
                     usableEnd = trackX1;
                  }
                  float centerX = usableStart + (usableEnd - usableStart) * normalizedValue;
                  float centerY = (trackY0 + trackY1) * 0.5f;
                  handleX0 = centerX - handleW * 0.5f;
                  handleX1 = centerX + handleW * 0.5f;
                  handleY0 = centerY - handleH * 0.5f;
                  handleY1 = centerY + handleH * 0.5f;
               } else {
                  float usableStart = trackY1 - handleH * 0.5f;
                  float usableEnd = trackY0 + handleH * 0.5f;
                  if (usableStart <= usableEnd) {
                     usableStart = trackY1;
                     usableEnd = trackY0;
                  }
                  float centerX = (trackX0 + trackX1) * 0.5f;
                  float centerY = usableStart - (usableStart - usableEnd) * normalizedValue;
                  handleX0 = centerX - handleW * 0.5f;
                  handleX1 = centerX + handleW * 0.5f;
                  handleY0 = centerY - handleH * 0.5f;
                  handleY1 = centerY + handleH * 0.5f;
               }

               uint8_t hr = (uint8_t)(clamp01(handleTint.r) * 255.0f);
               uint8_t hg = (uint8_t)(clamp01(handleTint.g) * 255.0f);
               uint8_t hb = (uint8_t)(clamp01(handleTint.b) * 255.0f);
               uint8_t ha = (uint8_t)(clamp01(handleTint.a * sl.Opacity * it.canvasOpacity) * 255.0f);
               uint32_t handleABGR = (ha << 24) | (hb << 16) | (hg << 8) | hr;

               UIVertex handleVerts[4] = {
                  { handleX0, handleY0, 0.0f, 0.0f, 0.0f, handleABGR },
                  { handleX1, handleY0, 0.0f, 1.0f, 0.0f, handleABGR },
                  { handleX1, handleY1, 0.0f, 1.0f, 1.0f, handleABGR },
                  { handleX0, handleY1, 0.0f, 0.0f, 1.0f, handleABGR }
               };
               uint16_t handleIdx[6] = { 0,1,2, 0,2,3 };

               bgfx::TextureHandle handleTh = m_UIWhiteTex;
#ifndef CLAYMORE_RUNTIME
               if (sl.HandleTexture.IsValid()) {
                  if (auto* entry = AssetLibrary::Instance().GetAsset(sl.HandleTexture)) {
                     if (!entry->texture || !bgfx::isValid(*entry->texture)) {
                        auto tex = AssetLibrary::Instance().LoadTexture(sl.HandleTexture);
                        (void)tex;
                     }
                     if (entry->texture && bgfx::isValid(*entry->texture)) handleTh = *entry->texture;
                  }
               }
#endif
               submitUIQuadTransient(handleVerts, handleIdx, 4, 6, handleTh, 2, scissorHandle);
            }
            
            // ScrollView input handling - consume scroll input when mouse is over the scroll view
            if (d->ScrollView && d->ScrollView->Visible) {
               ScrollViewComponent& sv = *d->ScrollView;
               bool inside = (mx >= x0 && mx <= x1 && my >= y0 && my <= y1);
               const bool canScrollHorizontally = sv.HorizontalScroll && sv._HasHorizontalOverflow;
               const bool canScrollVertically = sv.VerticalScroll && sv._HasVerticalOverflow;
               const bool canScroll = canScrollHorizontally || canScrollVertically;
               
               if (inside && canScroll) {
                  m_UIInputConsumed = true;
                  
                  // Get scroll delta and apply to ContentOffset
                  float scrollDelta = Input::GetScrollDelta();
                  if (scrollDelta != 0.0f) {
                     float sensitivity = sv.ScrollSensitivity;
                     float maxScrollX = canScrollHorizontally
                        ? std::max(0.0f, sv._ResolvedContentSizeScreen.x - sv._ResolvedViewportSizeScreen.x)
                        : 0.0f;
                     float maxScrollY = canScrollVertically
                        ? std::max(0.0f, sv._ResolvedContentSizeScreen.y - sv._ResolvedViewportSizeScreen.y)
                        : 0.0f;
                     
                     // Apply scroll with inversion (positive scroll = scroll up = negative offset)
                     if (canScrollVertically) {
                        sv.ContentOffset.y = std::clamp(
                           sv.ContentOffset.y - scrollDelta * sensitivity,
                           0.0f, maxScrollY
                        );
                     }
                     if (canScrollHorizontally) {
                        // For horizontal scroll, could use shift+scroll or horizontal scroll wheel
                        // For now, only apply to horizontal if vertical is disabled
                        if (!canScrollVertically) {
                           sv.ContentOffset.x = std::clamp(
                              sv.ContentOffset.x - scrollDelta * sensitivity,
                              0.0f, maxScrollX
                           );
                        }
                     }
                  }
               }
            }
            
            // Dropdown hit-testing and interaction
            if (d->Dropdown && d->Dropdown->Interactable) {
               DropdownComponent& dd = *d->Dropdown;
               
               // Hide sibling Text component - we render dropdown text ourselves
               if (d->Text) {
                  d->Text->Visible = false;
               }
               
               // Reset per-frame flags
               dd.ValueChanged = false;
               
               // Track if mouse was down last frame (using a static map keyed by entity GUID)
               static std::unordered_map<uint64_t, bool> s_DropdownWasPressed;
               bool wasPressed = s_DropdownWasPressed[d->EntityGuid.low];
               
               // Main dropdown area hit test
               bool insideMain = (mx >= x0 && mx <= x1 && my >= y0 && my <= y1);
               bool blockedByOpenDropdown = dropdownInputCaptureEntity != INVALID_ENTITY_ID &&
                                            dropdownInputCaptureEntity != it.entityId;
               if (blockedByOpenDropdown) {
                  insideMain = false;
               }
               dd.Hovered = insideMain;
               
               // Track if we clicked the main area (mouse released while inside)
               bool clickedMain = (!mouseDown && wasPressed && insideMain);
               
               // Calculate dropdown list bounds when open
               int visibleOptions = std::min((int)dd.Options.size(), dd.MaxVisibleOptions);
               float listHeight = visibleOptions * dd.OptionHeight;
               float listY0 = y1;  // Start below the main panel
               float listY1 = listY0 + listHeight;
               int hiddenOptionCount = std::max(0, (int)dd.Options.size() - visibleOptions);
               float maxDropdownScroll = hiddenOptionCount * dd.OptionHeight;
               dd._ScrollOffset = std::clamp(dd._ScrollOffset, 0.0f, maxDropdownScroll);
               
               // Check if mouse is inside the expanded options list
               bool insideList = !blockedByOpenDropdown &&
                                 dd.IsOpen &&
                                 (mx >= x0 && mx <= x1 && my >= listY0 && my <= listY1);
               
               // Consume input if hovering main or list
               if (insideMain || insideList) { m_UIInputConsumed = true; }

               // Scroll open dropdown option list with mouse wheel.
               if (dd.IsOpen && insideList) {
                  float dropdownScrollDelta = Input::GetScrollDelta();
                  if (dropdownScrollDelta != 0.0f && maxDropdownScroll > 0.0f) {
                     dd._ScrollOffset = std::clamp(
                        dd._ScrollOffset - (dropdownScrollDelta * dd.OptionHeight),
                        0.0f,
                        maxDropdownScroll
                     );
                  }
               }
               
               // Determine hovered option index when list is open
               dd.HoveredOptionIndex = -1;
               if (dd.IsOpen && insideList) {
                  float relY = my - listY0 + dd._ScrollOffset;
                  int idx = (int)(relY / dd.OptionHeight);
                  if (idx >= 0 && idx < (int)dd.Options.size()) {
                     dd.HoveredOptionIndex = idx;
                  }
               }
               
               // Handle clicks
               if (clickedMain) {
                  // Toggle dropdown when the main box is clicked.
                  dd.IsOpen = !dd.IsOpen;
                  if (dd.IsOpen) {
                     // Opening a dropdown should start from the top for deterministic behavior.
                     dd._ScrollOffset = 0.0f;
                  }
               }
               else if (!mouseDown && wasPressed && dd.IsOpen) {
                  if (dd.HoveredOptionIndex >= 0) {
                     // Select option
                     int oldIndex = dd.SelectedIndex;
                     dd.SelectedIndex = dd.HoveredOptionIndex;
                     dd.ValueChanged = (oldIndex != dd.SelectedIndex);
                     dd.IsOpen = false;
                  }
                  else if (!insideMain && !insideList) {
                     // Clicked outside - close dropdown
                     dd.IsOpen = false;
                  }
               }
               
               s_DropdownWasPressed[d->EntityGuid.low] = mouseDown && (insideMain || insideList);
               
               // Text size for dropdown labels (scaled to reference resolution)
               const float dropdownTextSize = 16.0f * canvasScale.x;
               
               // Render selected option text on main dropdown panel
               if (m_TextRenderer) {
                  std::string displayText;
                  if (dd.SelectedIndex >= 0 && dd.SelectedIndex < (int)dd.Options.size()) {
                     displayText = dd.Options[dd.SelectedIndex];
                  } else if (!dd.Caption.empty()) {
                     displayText = dd.Caption;
                  } else {
                     displayText = "Select...";
                  }
                  
                  float mainTextX = x0 + 8.0f;  // Left padding
                  float mainHeight = y1 - y0;
                  // Center text vertically: baseline = top + (height/2) + (textSize * 0.35) for approx centering
                  float mainTextY = y0 + mainHeight * 0.5f + dropdownTextSize * 0.35f;
                  
                  TextRendererComponent mainText;
                  mainText.Text = displayText;
                  mainText.PixelSize = dropdownTextSize;
                  mainText.ColorAbgr = 0xffffffffu;  // White text
                  mainText.WorldSpace = false;
                  mainText.Visible = true;
                  
                  std::vector<std::pair<const TextRendererComponent*, glm::vec2>> mainTextItems = {
                     { &mainText, glm::vec2{ mainTextX, mainTextY } }
                  };
                  m_TextRenderer->RenderScreenTexts(mainTextItems, it.canvasOpacity, m_Width, m_Height, uiViewId);
               }
               
               // Render dropdown options list when open
               if (dd.IsOpen && !dd.Options.empty()) {
                  for (int i = 0; i < visibleOptions; ++i) {
                     int optIdx = i + (int)(dd._ScrollOffset / dd.OptionHeight);
                     if (optIdx >= (int)dd.Options.size()) break;
                     
                     float optY0 = listY0 + i * dd.OptionHeight;
                     float optY1 = optY0 + dd.OptionHeight;
                     
                     // Determine option color
                     glm::vec4 optColor = dd.OptionNormalColor;
                     if (optIdx == dd.SelectedIndex) {
                        optColor = dd.OptionSelectedColor;
                     }
                     if (optIdx == dd.HoveredOptionIndex) {
                        optColor = dd.OptionHoverColor;
                     }
                     
                     // Pack color
                     uint8_t oR = (uint8_t)(clamp01(optColor.r) * 255.0f);
                     uint8_t oG = (uint8_t)(clamp01(optColor.g) * 255.0f);
                     uint8_t oB = (uint8_t)(clamp01(optColor.b) * 255.0f);
                     uint8_t oA = (uint8_t)(clamp01(optColor.a * it.canvasOpacity) * 255.0f);
                     uint32_t optAbgr = (oA << 24) | (oB << 16) | (oG << 8) | oR;
                     
                     // Create option quad - OPTIMIZATION: use transient buffers
                     UIVertex optVerts[4] = {
                        { x0, optY0, 0.0f, 0.0f, 0.0f, optAbgr },
                        { x1, optY0, 0.0f, 1.0f, 0.0f, optAbgr },
                        { x1, optY1, 0.0f, 1.0f, 1.0f, optAbgr },
                        { x0, optY1, 0.0f, 0.0f, 1.0f, optAbgr }
                     };
                     uint16_t optIdx6[6] = { 0,1,2, 0,2,3 };
                  submitUIQuadTransient(optVerts, optIdx6, 4, 6, m_UIWhiteTex, uiViewId);
                     
                     // Render option text - vertically centered
                     if (m_TextRenderer && optIdx < (int)dd.Options.size()) {
                        float textX = x0 + 8.0f;  // Left padding
                        // Center text vertically: baseline = top + (height/2) + (textSize * 0.35)
                        float textY = optY0 + dd.OptionHeight * 0.5f + dropdownTextSize * 0.35f;
                        
                        // Create temporary TextRendererComponent for the option text
                        TextRendererComponent optText;
                        optText.Text = dd.Options[optIdx];
                        optText.PixelSize = dropdownTextSize;
                        optText.ColorAbgr = 0xffffffffu;  // White text
                        optText.WorldSpace = false;
                        optText.Visible = true;
                        
                        std::vector<std::pair<const TextRendererComponent*, glm::vec2>> optTextItems = {
                           { &optText, glm::vec2{ textX, textY } }
                        };
                        m_TextRenderer->RenderScreenTexts(optTextItems, it.canvasOpacity, m_Width, m_Height, uiViewId);
                     }
                  }
               }
            }

            // Progress Bar fill rendering
            if (d->ProgressBar && d->ProgressBar->Visible) {
               ProgressBarComponent& pb = *d->ProgressBar;
               
               // Animate display value smoothly toward target
               if (pb.Animate) {
                  float target = pb.Value;
                  float diff = target - pb._DisplayValue;
                  float step = pb.AnimationSpeed * 0.016f; // Approx 60fps step
                  if (std::abs(diff) < step) pb._DisplayValue = target;
                  else pb._DisplayValue += (diff > 0 ? step : -step);
               } else {
                  pb._DisplayValue = pb.Value;
               }
               
               // Normalize value to 0-1 range
               float range = pb.MaxValue - pb.MinValue;
               float fillAmount = (range > 0.0001f) ? (pb._DisplayValue - pb.MinValue) / range : 0.0f;
               fillAmount = std::clamp(fillAmount, 0.0f, 1.0f);
               
               // Determine padding - use Panel's SliceBorder if flag is set
               glm::vec4 effectivePadding = pb.Padding;
               if (pb.UsePanelBorderAsPadding && it.panel) {
                  const PanelComponent& panelRef = *it.panel;
                  if (panelRef.Mode == PanelComponent::FillMode::NineSlice) {
                     // Per-axis calculation matching NineSlice border logic
                     float w = (x1 - x0), h = (y1 - y0);
                     float du = (panelRef.UVRect.z - panelRef.UVRect.x);
                     float dv = (panelRef.UVRect.w - panelRef.UVRect.y);
                     
                     // Per-axis: pixel border if set, otherwise percentage from UV
                     effectivePadding.x = (panelRef.SliceBorder.x > 0) ? panelRef.SliceBorder.x :
                                          ((du != 0.0f) ? w * (panelRef.SliceUV.x / du) : 0.0f);
                     effectivePadding.y = (panelRef.SliceBorder.y > 0) ? panelRef.SliceBorder.y :
                                          ((dv != 0.0f) ? h * (panelRef.SliceUV.y / dv) : 0.0f);
                     effectivePadding.z = (panelRef.SliceBorder.z > 0) ? panelRef.SliceBorder.z :
                                          ((du != 0.0f) ? w * (panelRef.SliceUV.z / du) : 0.0f);
                     effectivePadding.w = (panelRef.SliceBorder.w > 0) ? panelRef.SliceBorder.w :
                                          ((dv != 0.0f) ? h * (panelRef.SliceUV.w / dv) : 0.0f);
                  }
               }
               
               // Calculate fill rectangle with padding
               float fx0 = x0 + effectivePadding.x;  // left padding
               float fy0 = y0 + effectivePadding.y;  // top padding
               float fx1 = x1 - effectivePadding.z;  // right padding
               float fy1 = y1 - effectivePadding.w;  // bottom padding
               
               // Apply fill direction
               float fillX0 = fx0, fillY0 = fy0, fillX1 = fx1, fillY1 = fy1;
               float fillU0 = 0.0f, fillV0 = 0.0f, fillU1 = 1.0f, fillV1 = 1.0f;
               
               switch (pb.Direction) {
                  case ProgressBarComponent::FillDirection::LeftToRight:
                     fillX1 = fx0 + (fx1 - fx0) * fillAmount;
                     fillU1 = fillAmount;
                     break;
                  case ProgressBarComponent::FillDirection::RightToLeft:
                     fillX0 = fx1 - (fx1 - fx0) * fillAmount;
                     fillU0 = 1.0f - fillAmount;
                     break;
                  case ProgressBarComponent::FillDirection::BottomToTop:
                     fillY0 = fy1 - (fy1 - fy0) * fillAmount;
                     fillV0 = 1.0f - fillAmount;
                     break;
                  case ProgressBarComponent::FillDirection::TopToBottom:
                     fillY1 = fy0 + (fy1 - fy0) * fillAmount;
                     fillV1 = fillAmount;
                     break;
               }
               
               // Determine fill color (gradient or solid)
               glm::vec4 fillColor = pb.FillColor;
               if (pb.UseGradient) {
                  fillColor = glm::mix(pb.GradientLowColor, pb.GradientHighColor, fillAmount);
               }
               
               // Pack fill color with component opacity and canvas opacity
               uint8_t fr = (uint8_t)(fillColor.x * 255.0f);
               uint8_t fg = (uint8_t)(fillColor.y * 255.0f);
               uint8_t fb = (uint8_t)(fillColor.z * 255.0f);
               uint8_t fa = (uint8_t)(fillColor.w * pb.Opacity * it.canvasOpacity * 255.0f);
               uint32_t fillABGR = (fa << 24) | (fb << 16) | (fg << 8) | fr;
               
               // Create fill bar vertices - OPTIMIZATION: use transient buffers
               UIVertex fillVerts[4] = {
                  { fillX0, fillY0, 0.0f, fillU0, fillV0, fillABGR },
                  { fillX1, fillY0, 0.0f, fillU1, fillV0, fillABGR },
                  { fillX1, fillY1, 0.0f, fillU1, fillV1, fillABGR },
                  { fillX0, fillY1, 0.0f, fillU0, fillV1, fillABGR }
               };
               uint16_t fillIdx[6] = { 0,1,2, 0,2,3 };
               
               // Get fill texture or use white
               bgfx::TextureHandle fillTh = m_UIWhiteTex;
#ifndef CLAYMORE_RUNTIME
               if (pb.FillTexture.IsValid()) {
                  if (auto* entry = AssetLibrary::Instance().GetAsset(pb.FillTexture)) {
                     if (!entry->texture || !bgfx::isValid(*entry->texture)) {
                        auto tex = AssetLibrary::Instance().LoadTexture(pb.FillTexture);
                        (void)tex;
                     }
                     if (entry->texture && bgfx::isValid(*entry->texture)) fillTh = *entry->texture;
                  }
               }
#endif
                  submitUIQuadTransient(fillVerts, fillIdx, 4, 6, fillTh, uiViewId);
            }

            // Optional: debug rect outline
            if (m_ShowUIRects) {
               uint32_t dbg = 0x66ff8800u;
               // draw via bgfx line list: use a tiny transient buffer for the rectangle
               struct L { float x,y,z; };
               L v[8] = { {x0,y0,0},{x1,y0,0}, 
                          {x1,y0,0},{x1,y1,0}, 
                          {x1,y1,0},{x0,y1,0}, 
                          {x0,y1,0},{x0,y0,0} 
                        };
               const bgfx::Memory* mem = bgfx::copy(v, sizeof(v));
               
               bgfx::VertexLayout layout; layout.begin().add(bgfx::Attrib::Position,3,bgfx::AttribType::Float).end();
               bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(mem, layout);
               
               float idm[16]; bx::mtxIdentity(idm); bgfx::setTransform(idm);
               bgfx::setVertexBuffer(0, vbh);
               
#ifndef CLAYMORE_RUNTIME
               if (m_CachedDebugMaterial) {
                  m_CachedDebugMaterial->BindUniforms();
                  ApplyDefaultDebugLineColor();
                  bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_PT_LINES | BGFX_STATE_BLEND_ALPHA);
                  if (bgfx::isValid(m_CachedDebugMaterial->GetProgram())) {
                  bgfx::submit(uiViewId, m_CachedDebugMaterial->GetProgram());
                  }
               }
#endif
               bgfx::destroy(vbh);
            }
         } else if (it.type == UIItemType::Text) {
            // Check if this text is inside a ScrollView
            auto [textScrollViewEntity, textScrollViewRect] = findParentScrollView(it.entityId);
            glm::vec2 textScrollOffset(0.0f, 0.0f);
            uint16_t textScissorHandle = UINT16_MAX;
            float effectiveMaxWidth = 0.0f; // Will be set if inside ScrollView
            
            if (textScrollViewEntity != INVALID_ENTITY_ID) {
               // Get scroll offset from the ScrollView
               textScrollOffset = findScrollOffset(it.entityId);
               
               // Compute scissor rect
               float tsx0 = std::max(0.0f, textScrollViewRect.x);
               float tsy0 = std::max(0.0f, textScrollViewRect.y);
               float tsx1 = std::min((float)m_Width, textScrollViewRect.x + textScrollViewRect.z);
               float tsy1 = std::min((float)m_Height, textScrollViewRect.y + textScrollViewRect.w);
               
               // Calculate the actual available width for text wrapping
               // This accounts for canvas scaling that may differ from the C# design-space values
               float textLeftEdge = it.computedRect.x - textScrollOffset.x;
               effectiveMaxWidth = tsx1 - textLeftEdge - 4.0f; // 4px right margin
               
               if (tsx1 > tsx0 && tsy1 > tsy0) {
                  textScissorHandle = bgfx::setScissor(
                     (uint16_t)tsx0, (uint16_t)tsy0, 
                     (uint16_t)(tsx1 - tsx0), (uint16_t)(tsy1 - tsy0)
                  );
               }
            }
            
            // Use pre-computed position from layout pass, apply scroll offset
            // computedRect.y stores top of text bounds, but TextRenderer needs baseline position
            float sx = it.computedRect.x - textScrollOffset.x;
            float sy = it.computedRect.y - textScrollOffset.y;
            
            // Convert from top of bounds to baseline (ascent below top)
            glm::vec2 textScale = getCanvasScale(it.canvas);
            float scaledPixelSize = it.text->PixelSize * textScale.x;
            float textAscent = scaledPixelSize * 0.8f;
            sy += textAscent;
            
            if (m_TextRenderer) {
               // If inside a ScrollView, clamp the text wrap width to actual visible space
               // This fixes canvas scaling mismatches where design-space width != screen-space width
               TextRendererComponent textCopy = *it.text;
               textCopy.PixelSize = scaledPixelSize;
               if (it.data->UIRect && it.data->UIRect->Size.x > 0.0f && textCopy.RectSize.x <= 0.0f) {
                  textCopy.RectSize.x = it.data->UIRect->Size.x;
               }
               if (it.data->UIRect && it.data->UIRect->Size.y > 0.0f && textCopy.RectSize.y <= 0.0f) {
                  textCopy.RectSize.y = it.data->UIRect->Size.y;
               }
               if (textCopy.RectSize.x > 0.0f) textCopy.RectSize.x *= textScale.x;
               if (textCopy.RectSize.y > 0.0f) textCopy.RectSize.y *= textScale.y;
               if (effectiveMaxWidth > 0 && textCopy.RectSize.x > effectiveMaxWidth) {
                  textCopy.RectSize.x = effectiveMaxWidth;
               }
               
               std::vector<std::pair<const TextRendererComponent*, glm::vec2>> one = { { &textCopy, glm::vec2{sx, sy} } };
               m_TextRenderer->RenderScreenTexts(one, it.canvasOpacity, m_Width, m_Height, uiViewId, textScissorHandle);
            }
         }
      }

      goto ui_overlay_end;

      // Simple per-entity pass: draw panels; drive buttons; text already handled by TextRenderer screen path
      for (auto& e : scene.GetEntities()) {
         auto* d = scene.GetEntityData(e.GetID());
         if (!IsPresentationVisible(d))
         { 
            continue; 
         }

         // If entity has a Canvas and is screen space, it just acts as a scope for children; we currently use global backbuffer size
         
         // Draw Panel
         if (d->Panel && d->Panel->Visible) {
            PanelComponent& p = *d->Panel;
            // Compute anchor-based top-left position
            float ax = 0.0f, ay = 0.0f;
            if (p.AnchorEnabled) {
               switch (p.Anchor) {
                     case UIAnchorPreset::TopLeft:    ax = 0;              ay = 0;               break;
                     case UIAnchorPreset::Top:        ax = m_Width * 0.5f;   ay = 0;               break;
                     case UIAnchorPreset::TopRight:   ax = (float)m_Width;  ay = 0;               break;
                     case UIAnchorPreset::Left:       ax = 0;              ay = m_Height * 0.5f;  break;
                     case UIAnchorPreset::Center:     ax = m_Width * 0.5f;   ay = m_Height * 0.5f;  break;
                     case UIAnchorPreset::Right:      ax = (float)m_Width; ay = m_Height * 0.5f;  break;
                     case UIAnchorPreset::BottomLeft: ax = 0;              ay = (float)m_Height; break;
                     case UIAnchorPreset::Bottom:     ax = m_Width * 0.5f;   ay = (float)m_Height; break;
                     case UIAnchorPreset::BottomRight:ax = (float)m_Width; ay = (float)m_Height; break;
                  }
               ax += p.AnchorOffset.x;
               ay += p.AnchorOffset.y;
               }
            else {
               ax = p.Position.x;
               ay = p.Position.y;
               }
            float x0 = ax;
            float y0 = ay;
            float x1 = x0 + p.Size.x * p.Scale.x;
            float y1 = y0 + p.Size.y * p.Scale.y;
            
            // Calculate pivot point for rotation
            float pivotX = x0 + (x1 - x0) * p.Pivot.x;
            float pivotY = y0 + (y1 - y0) * p.Pivot.y;
            
            // Helper function to rotate a 2D point around the pivot
            auto rotatePoint = [&](float x, float y) -> glm::vec2 {
               if (std::abs(p.Rotation) < 0.001f) {
                  return glm::vec2(x, y);
               }
               float rad = glm::radians(p.Rotation);
               float cosR = std::cos(rad);
               float sinR = std::sin(rad);
               float dx = x - pivotX;
               float dy = y - pivotY;
               return glm::vec2(
                  pivotX + dx * cosR - dy * sinR,
                  pivotY + dx * sinR + dy * cosR
               );
            };

            // Base tint: panel tint
            uint32_t abgr = 0xffffffffu;
            // Pack tint including opacity
            auto clamp01 = [](float v) { return std::max(0.0f, std::min(1.0f, v)); };
            glm::vec4 tint = p.TintColor;
            // Apply button state tint if present
            if (d->Button) 
            {
               if (d->Button->Pressed)       { tint *= d->Button->PressedTint; }
               else if (d->Button->Hovered)  { tint *= d->Button->HoverTint;   }
               else                          { tint *= d->Button->NormalTint;  }
            }
            // Multiply by ancestor canvas opacity if under a screen-space canvas
            float canvasOpacity = 1.0f;
            {
               EntityID cur = e.GetID();
               while (cur != INVALID_ENTITY_ID) {
                  auto* d2 = scene.GetEntityData(cur);
                  if (!d2) 
                  { 
                     break; 
                  }

                  if (d2->Canvas && 
                      d2->Canvas->Space == CanvasComponent::RenderSpace::ScreenSpace) 
                  { 
                     canvasOpacity = d2->Canvas->Opacity; 
                     break; 
                  }
                  cur = d2->Parent;
               }
            }
            uint8_t r = (uint8_t)(clamp01(tint.r) * 255.0f);
            uint8_t g = (uint8_t)(clamp01(tint.g) * 255.0f);
            uint8_t b = (uint8_t)(clamp01(tint.b) * 255.0f);
            uint8_t a = (uint8_t)(clamp01(tint.a * p.Opacity * canvasOpacity) * 255.0f);
            abgr = (a << 24) | (b << 16) | (g << 8) | (r);

            UIVertex verts[4];
            uint16_t idx[6] = { 0,1,2, 0,2,3 };
            if (p.Mode == PanelComponent::FillMode::NineSlice &&
                p.Texture.IsValid()) 
            {
               float L = x0, T = y0, R = x1, B = y1;
               float w = (x1 - x0), h = (y1 - y0);
               float uL = p.UVRect.x, vT = p.UVRect.y, uR = p.UVRect.z, vB = p.UVRect.w;
               
               // Compute pixel border sizes with per-axis fallback
               float du = (uR - uL);
               float dv = (vB - vT);
               
               float lpx = (p.SliceBorder.x > 0) ? p.SliceBorder.x : 
                           ((du != 0.0f) ? w * (p.SliceUV.x / du) : 0.0f);
               float tpx = (p.SliceBorder.y > 0) ? p.SliceBorder.y : 
                           ((dv != 0.0f) ? h * (p.SliceUV.y / dv) : 0.0f);
               float rpx = (p.SliceBorder.z > 0) ? p.SliceBorder.z : 
                           ((du != 0.0f) ? w * (p.SliceUV.z / du) : 0.0f);
               float bpx = (p.SliceBorder.w > 0) ? p.SliceBorder.w : 
                           ((dv != 0.0f) ? h * (p.SliceUV.w / dv) : 0.0f);
               
               // Clamp borders so they don't exceed panel size
               if (lpx + rpx > w) { float scale = w / (lpx + rpx); lpx *= scale; rpx *= scale; }
               if (tpx + bpx > h) { float scale = h / (tpx + bpx); tpx *= scale; bpx *= scale; }

               float xL = L;
               float xM = L + lpx;
               float xR = R - rpx;
               float yT = T;
               float yM = T + tpx;
               float yB = B - bpx;

               // Compute UV splits using absolute slice margins inside the rect
               float uL2 = uL + p.SliceUV.x;
               float uR2 = uR - p.SliceUV.z;
               float vT2 = vT + p.SliceUV.y;
               float vB2 = vB - p.SliceUV.w;

               // Resolve texture once for all 9 slices (moved outside lambda for efficiency)
               bgfx::TextureHandle nineSliceTex2 = m_UIWhiteTex;
#ifndef CLAYMORE_RUNTIME
               if (p.Texture.IsValid()) {
                  if (auto* entry = AssetLibrary::Instance().GetAsset(p.Texture)) {
                     if (!entry->texture || !bgfx::isValid(*entry->texture)) {
                        auto tex = AssetLibrary::Instance().LoadTexture(p.Texture);
                        (void)tex;
                     }
                     if (entry->texture && bgfx::isValid(*entry->texture)) nineSliceTex2 = *entry->texture;
                  }
               }
#endif
               
               // OPTIMIZATION: Use transient buffers instead of create/destroy per quad
               auto submitQuad = [&](float xa, float ya, float xb, float yb, float ua, float va, float ub, float vb) {
                  // Apply rotation to all four corners
                  glm::vec2 v0 = rotatePoint(xa, ya);
                  glm::vec2 v1 = rotatePoint(xb, ya);
                  glm::vec2 v2 = rotatePoint(xb, yb);
                  glm::vec2 v3 = rotatePoint(xa, yb);
                  
                  UIVertex vv[4] = {
                      { v0.x, v0.y, 0.0f, ua, va, abgr },
                      { v1.x, v1.y, 0.0f, ub, va, abgr },
                      { v2.x, v2.y, 0.0f, ub, vb, abgr },
                      { v3.x, v3.y, 0.0f, ua, vb, abgr }
                  };
                  uint16_t ii[6] = { 0,1,2, 0,2,3 };
                  submitUIQuadTransient(vv, ii, 4, 6, nineSliceTex2, uiViewId);
               };

               submitQuad(xL, yT, xM, yM, uL, vT, uL2, vT2);
               submitQuad(xM, yT, xR, yM, uL2, vT, uR2, vT2);
               submitQuad(xR, yT, R, yM, uR2, vT, uR, vT2);
               submitQuad(xL, yM, xM, yB, uL, vT2, uL2, vB2);
               submitQuad(xM, yM, xR, yB, uL2, vT2, uR2, vB2);
               submitQuad(xR, yM, R, yB, uR2, vT2, uR, vB2);
               submitQuad(xL, yB, xM, B, uL, vB2, uL2, vB);
               submitQuad(xM, yB, xR, B, uL2, vB2, uR2, vB);
               submitQuad(xR, yB, R, B, uR2, vB2, uR, vB);
               // NineSlice already rendered - skip single-quad path but continue to progress bar
               }
            else {
               // Single-quad rendering for Tile and Stretch modes
               // Apply rotation to all four corners
               glm::vec2 v0 = rotatePoint(x0, y0);
               glm::vec2 v1 = rotatePoint(x1, y0);
               glm::vec2 v2 = rotatePoint(x1, y1);
               glm::vec2 v3 = rotatePoint(x0, y1);
               
               if (p.Mode == PanelComponent::FillMode::Tile) {
                  float u0 = p.UVRect.x, uv0 = p.UVRect.y;
                  float u1 = p.UVRect.z * p.TileRepeat.x, uv1 = p.UVRect.w * p.TileRepeat.y;
                  verts[0] = { v0.x, v0.y, 0.0f, u0, uv0, abgr };
                  verts[1] = { v1.x, v1.y, 0.0f, u1, uv0, abgr };
                  verts[2] = { v2.x, v2.y, 0.0f, u1, uv1, abgr };
                  verts[3] = { v3.x, v3.y, 0.0f, u0, uv1, abgr };
               }
               else {
                  verts[0] = { v0.x, v0.y, 0.0f, p.UVRect.x, p.UVRect.y, abgr };
                  verts[1] = { v1.x, v1.y, 0.0f, p.UVRect.z, p.UVRect.y, abgr };
                  verts[2] = { v2.x, v2.y, 0.0f, p.UVRect.z, p.UVRect.w, abgr };
                  verts[3] = { v3.x, v3.y, 0.0f, p.UVRect.x, p.UVRect.w, abgr };
               }
               // OPTIMIZATION: Use transient buffers instead of create/destroy per quad
               bgfx::TextureHandle th = m_UIWhiteTex;
#ifndef CLAYMORE_RUNTIME
               if (p.Texture.IsValid()) {
                  if (auto* entry = AssetLibrary::Instance().GetAsset(p.Texture)) {
                     if (!entry->texture || !bgfx::isValid(*entry->texture)) {
                        auto tex = AssetLibrary::Instance().LoadTexture(p.Texture);
                        (void)tex;
                     }
                     if (entry->texture && bgfx::isValid(*entry->texture)) th = *entry->texture;
                  }
               }
#endif
                  submitUIQuadTransient(verts, idx, 4, 6, th, uiViewId);
            }

            // Button hit-testing overlay
            if (d->Button && d->Button->Interactable) {
               bool inside = (mx >= x0 && mx <= x1 && my >= y0 && my <= y1);
               d->Button->Hovered = inside;
               if (inside) m_UIInputConsumed = true;
               bool wasPressed = d->Button->Pressed;
               d->Button->Pressed = inside && mouseDown;
               d->Button->Clicked = (!mouseDown && wasPressed && inside);
               if (d->Button->Toggle && d->Button->Clicked) d->Button->Toggled = !d->Button->Toggled;
               }

            // Progress Bar fill rendering (simple pass)
            if (d->ProgressBar && d->ProgressBar->Visible) {
               ProgressBarComponent& pb = *d->ProgressBar;
               
               // Animate display value
               if (pb.Animate) {
                  float target = pb.Value;
                  float diff = target - pb._DisplayValue;
                  float step = pb.AnimationSpeed * 0.016f;
                  if (std::abs(diff) < step) pb._DisplayValue = target;
                  else pb._DisplayValue += (diff > 0 ? step : -step);
               } else {
                  pb._DisplayValue = pb.Value;
               }
               
               // Normalize value
               float range = pb.MaxValue - pb.MinValue;
               float fillAmount = (range > 0.0001f) ? (pb._DisplayValue - pb.MinValue) / range : 0.0f;
               fillAmount = std::clamp(fillAmount, 0.0f, 1.0f);
               
               // Determine padding - use Panel's SliceBorder if flag is set
               glm::vec4 effectivePadding = pb.Padding;
               if (pb.UsePanelBorderAsPadding && p.Mode == PanelComponent::FillMode::NineSlice) {
                  // Per-axis calculation matching NineSlice border logic
                  float w = (x1 - x0), h = (y1 - y0);
                  float du = (p.UVRect.z - p.UVRect.x);
                  float dv = (p.UVRect.w - p.UVRect.y);
                  
                  // Per-axis: pixel border if set, otherwise percentage from UV
                  effectivePadding.x = (p.SliceBorder.x > 0) ? p.SliceBorder.x :
                                       ((du != 0.0f) ? w * (p.SliceUV.x / du) : 0.0f);
                  effectivePadding.y = (p.SliceBorder.y > 0) ? p.SliceBorder.y :
                                       ((dv != 0.0f) ? h * (p.SliceUV.y / dv) : 0.0f);
                  effectivePadding.z = (p.SliceBorder.z > 0) ? p.SliceBorder.z :
                                       ((du != 0.0f) ? w * (p.SliceUV.z / du) : 0.0f);
                  effectivePadding.w = (p.SliceBorder.w > 0) ? p.SliceBorder.w :
                                       ((dv != 0.0f) ? h * (p.SliceUV.w / dv) : 0.0f);
               }
               
               // Calculate fill rectangle with padding
               float fx0 = x0 + effectivePadding.x;
               float fy0 = y0 + effectivePadding.y;
               float fx1 = x1 - effectivePadding.z;
               float fy1 = y1 - effectivePadding.w;
               
               float fillX0 = fx0, fillY0 = fy0, fillX1 = fx1, fillY1 = fy1;
               float fillU0 = 0.0f, fillV0 = 0.0f, fillU1 = 1.0f, fillV1 = 1.0f;
               
               switch (pb.Direction) {
                  case ProgressBarComponent::FillDirection::LeftToRight:
                     fillX1 = fx0 + (fx1 - fx0) * fillAmount;
                     fillU1 = fillAmount;
                     break;
                  case ProgressBarComponent::FillDirection::RightToLeft:
                     fillX0 = fx1 - (fx1 - fx0) * fillAmount;
                     fillU0 = 1.0f - fillAmount;
                     break;
                  case ProgressBarComponent::FillDirection::BottomToTop:
                     fillY0 = fy1 - (fy1 - fy0) * fillAmount;
                     fillV0 = 1.0f - fillAmount;
                     break;
                  case ProgressBarComponent::FillDirection::TopToBottom:
                     fillY1 = fy0 + (fy1 - fy0) * fillAmount;
                     fillV1 = fillAmount;
                     break;
               }
               
               glm::vec4 fillColor = pb.FillColor;
               if (pb.UseGradient) {
                  fillColor = glm::mix(pb.GradientLowColor, pb.GradientHighColor, fillAmount);
               }
               
               // Pack fill color with component opacity
               uint8_t fr = (uint8_t)(fillColor.x * 255.0f);
               uint8_t fg = (uint8_t)(fillColor.y * 255.0f);
               uint8_t fb = (uint8_t)(fillColor.z * 255.0f);
               uint8_t fa = (uint8_t)(fillColor.w * pb.Opacity * 255.0f);
               uint32_t fillABGR = (fa << 24) | (fb << 16) | (fg << 8) | fr;
               
               // OPTIMIZATION: use transient buffers
               UIVertex fillVerts[4] = {
                  { fillX0, fillY0, 0.0f, fillU0, fillV0, fillABGR },
                  { fillX1, fillY0, 0.0f, fillU1, fillV0, fillABGR },
                  { fillX1, fillY1, 0.0f, fillU1, fillV1, fillABGR },
                  { fillX0, fillY1, 0.0f, fillU0, fillV1, fillABGR }
               };
               uint16_t fillIdx[6] = { 0,1,2, 0,2,3 };
               
               bgfx::TextureHandle fillTh = m_UIWhiteTex;
#ifndef CLAYMORE_RUNTIME
               if (pb.FillTexture.IsValid()) {
                  if (auto* entry = AssetLibrary::Instance().GetAsset(pb.FillTexture)) {
                     if (!entry->texture || !bgfx::isValid(*entry->texture)) {
                        auto tex = AssetLibrary::Instance().LoadTexture(pb.FillTexture);
                        (void)tex;
                     }
                     if (entry->texture && bgfx::isValid(*entry->texture)) fillTh = *entry->texture;
                  }
               }
#endif
                  submitUIQuadTransient(fillVerts, fillIdx, 4, 6, fillTh, uiViewId);
            }
            }
         }
      
      if (allowInput) {
         if (mouseReleased && m_UIDragSource != INVALID_ENTITY_ID) {
            EntityID sourceId = m_UIDragSource;
            if (dropTargetCandidate == sourceId) {
               dropTargetCandidate = INVALID_ENTITY_ID;
            }
            if (auto* sourceData = scene.GetEntityData(sourceId); sourceData && sourceData->Panel) {
               sourceData->Panel->DragEnded = true;
               sourceData->Panel->DropTargetEntity = dropTargetCandidate;
            }
            if (dropTargetCandidate != INVALID_ENTITY_ID) {
               if (auto* targetData = scene.GetEntityData(dropTargetCandidate); targetData && targetData->Panel) {
                  targetData->Panel->Dropped = true;
                  targetData->Panel->DropSourceEntity = sourceId;
               }
            }
            m_UIDragSource = INVALID_ENTITY_ID;
            m_UIDragActive = false;
         }
         if (updateMouseState) {
            m_UIPrevMouseDown = mouseDown;
         }
      }
   }

ui_overlay_end:
   m_Width = prevWidth;
   m_Height = prevHeight;
   if (restoreInput) {
      m_UIInputConsumed = prevUIInputConsumed;
   }
   }

void Renderer::RenderWorldSpaceCanvases(Scene& scene,
   Camera* renderCamera,
   const float* viewMtx,
   const float* projMtx,
   uint16_t worldViewId,
   bgfx::FrameBufferHandle framebuffer,
   uint32_t width,
   uint32_t height,
   bool allowInput,
   std::unordered_set<uint16_t>& currentOffscreenViewIds) {
   if (!renderCamera || width == 0 || height == 0 || !m_ShowUIOverlay || !bgfx::isValid(m_UIProgram)) {
      return;
   }

   struct WorldCanvasEntry {
      EntityID entityId = INVALID_ENTITY_ID;
      CanvasComponent* canvas = nullptr;
      glm::mat4 model = glm::mat4(1.0f);
      float quadWidth = 0.0f;
      float quadHeight = 0.0f;
      uint32_t renderWidth = 0;
      uint32_t renderHeight = 0;
      float hitDistance = std::numeric_limits<float>::max();
      float hitMouseX = 0.0f;
      float hitMouseY = 0.0f;
      bool hit = false;
   };

   std::vector<WorldCanvasEntry> canvases;
   canvases.reserve(scene.GetEntities().size());

   const glm::mat4 viewMatrix = glm::make_mat4(viewMtx);
   for (auto& entity : scene.GetEntities()) {
      EntityID entityId = entity.GetID();
      auto* data = scene.GetEntityData(entityId);
      if (!data || !data->Canvas || data->Canvas->Space != CanvasComponent::RenderSpace::WorldSpace) {
         continue;
      }
      if (!IsPresentationVisible(data) || !data->Active) {
         continue;
      }

      WorldCanvasEntry entry;
      entry.entityId = entityId;
      entry.canvas = data->Canvas.get();
      entry.renderWidth = ResolveWorldCanvasRenderDimension(entry.canvas->Width, entry.canvas->ReferenceWidth, width);
      entry.renderHeight = ResolveWorldCanvasRenderDimension(entry.canvas->Height, entry.canvas->ReferenceHeight, height);
      entry.quadWidth = static_cast<float>(entry.renderWidth) * 0.01f;
      entry.quadHeight = static_cast<float>(entry.renderHeight) * 0.01f;
      entry.model = entry.canvas->Billboard
         ? BuildCanvasBillboardMatrix(data->Transform.WorldMatrix, viewMatrix)
         : data->Transform.WorldMatrix;
      canvases.push_back(entry);
   }

   if (canvases.empty()) {
      return;
   }

   float mouseNX = 0.0f;
   float mouseNY = 0.0f;
   bool haveMouseNormalized = false;
   if (allowInput) {
      if (m_UIMouseNormalizedValid) {
         mouseNX = m_UIMouseNX;
         mouseNY = m_UIMouseNY;
         haveMouseNormalized = true;
      } else {
         auto mp = Input::GetMousePosition();
         if (width > 0 && height > 0) {
            mouseNX = mp.first / static_cast<float>(width);
            mouseNY = mp.second / static_cast<float>(height);
            haveMouseNormalized = true;
         }
      }
   }

   EntityID inputCanvasEntity = INVALID_ENTITY_ID;
   if (allowInput && haveMouseNormalized) {
      float clipX = mouseNX * 2.0f - 1.0f;
      float clipY = 1.0f - mouseNY * 2.0f;
      glm::vec4 rayClip(clipX, clipY, -1.0f, 1.0f);
      glm::mat4 invProj = glm::inverse(renderCamera->GetProjectionMatrix());
      glm::vec4 rayEye = invProj * rayClip;
      rayEye.z = -1.0f;
      rayEye.w = 0.0f;
      glm::mat4 invView = glm::inverse(renderCamera->GetViewMatrix());
      glm::vec3 rayDir = glm::normalize(glm::vec3(invView * rayEye));
      glm::vec3 rayOrigin = renderCamera->GetPosition();

      for (auto& canvasEntry : canvases) {
         if (canvasEntry.quadWidth <= 0.0f || canvasEntry.quadHeight <= 0.0f) {
            continue;
         }

         glm::mat4 invModel = glm::inverse(canvasEntry.model);
         glm::vec3 localOrigin = glm::vec3(invModel * glm::vec4(rayOrigin, 1.0f));
         glm::vec3 localDir = glm::vec3(invModel * glm::vec4(rayDir, 0.0f));
         float dirLenSq = glm::dot(localDir, localDir);
         if (dirLenSq < 1e-8f) {
            continue;
         }
         localDir /= std::sqrt(dirLenSq);
         if (std::abs(localDir.z) < 1e-6f) {
            continue;
         }

         float tLocal = -localOrigin.z / localDir.z;
         if (tLocal < 0.0f) {
            continue;
         }

         glm::vec3 localHit = localOrigin + localDir * tLocal;
         float halfW = canvasEntry.quadWidth * 0.5f;
         float halfH = canvasEntry.quadHeight * 0.5f;
         if (localHit.x < -halfW || localHit.x > halfW || localHit.y < -halfH || localHit.y > halfH) {
            continue;
         }

         glm::vec3 worldHit = glm::vec3(canvasEntry.model * glm::vec4(localHit, 1.0f));
         float hitDistance = glm::length(worldHit - rayOrigin);
         float localMouseX = ((localHit.x + halfW) / canvasEntry.quadWidth) * static_cast<float>(canvasEntry.renderWidth);
         float localMouseY = ((halfH - localHit.y) / canvasEntry.quadHeight) * static_cast<float>(canvasEntry.renderHeight);

         canvasEntry.hit = true;
         canvasEntry.hitDistance = hitDistance;
         canvasEntry.hitMouseX = glm::clamp(localMouseX, 0.0f, static_cast<float>(canvasEntry.renderWidth));
         canvasEntry.hitMouseY = glm::clamp(localMouseY, 0.0f, static_cast<float>(canvasEntry.renderHeight));

         auto shouldReplaceInputTarget = [&](const WorldCanvasEntry& current) {
            if (inputCanvasEntity == INVALID_ENTITY_ID) {
               return true;
            }
            if (hitDistance + 1e-4f < current.hitDistance) {
               return true;
            }
            return std::abs(hitDistance - current.hitDistance) <= 1e-4f
               && canvasEntry.canvas->SortOrder > current.canvas->SortOrder;
         };

         if (inputCanvasEntity == INVALID_ENTITY_ID) {
            inputCanvasEntity = canvasEntry.entityId;
         } else {
            auto it = std::find_if(canvases.begin(), canvases.end(), [&](const WorldCanvasEntry& candidate) {
               return candidate.entityId == inputCanvasEntity;
            });
            if (it != canvases.end() && shouldReplaceInputTarget(*it)) {
               inputCanvasEntity = canvasEntry.entityId;
            }
         }
      }
   }

   std::sort(canvases.begin(), canvases.end(), [](const WorldCanvasEntry& a, const WorldCanvasEntry& b) {
      if (a.canvas->SortOrder != b.canvas->SortOrder) {
         return a.canvas->SortOrder < b.canvas->SortOrder;
      }
      return a.entityId < b.entityId;
   });

   bgfx::setViewFrameBuffer(worldViewId, framebuffer);
   bgfx::setViewRect(worldViewId, 0, 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height));
   bgfx::setViewTransform(worldViewId, viewMtx, projMtx);
   bgfx::setViewMode(worldViewId, bgfx::ViewMode::Sequential);

   auto submitWorldCanvasQuad = [&](const WorldCanvasEntry& canvasEntry, bgfx::TextureHandle texture) {
      if (!bgfx::isValid(texture) || !bgfx::isValid(m_UISampler) || !bgfx::isValid(m_UIProgram)) {
         return;
      }

      bgfx::TransientVertexBuffer tvb;
      bgfx::TransientIndexBuffer tib;
      if (!bgfx::allocTransientBuffers(&tvb, UIVertex::layout, 4, &tib, 6)) {
         return;
      }

      const float halfW = canvasEntry.quadWidth * 0.5f;
      const float halfH = canvasEntry.quadHeight * 0.5f;
      const uint32_t white = 0xffffffffu;
      auto* verts = reinterpret_cast<UIVertex*>(tvb.data);
      verts[0] = { -halfW,  halfH, 0.0f, 0.0f, 0.0f, white };
      verts[1] = {  halfW,  halfH, 0.0f, 1.0f, 0.0f, white };
      verts[2] = {  halfW, -halfH, 0.0f, 1.0f, 1.0f, white };
      verts[3] = { -halfW, -halfH, 0.0f, 0.0f, 1.0f, white };

      auto* indices = reinterpret_cast<uint16_t*>(tib.data);
      indices[0] = 0; indices[1] = 1; indices[2] = 2;
      indices[3] = 0; indices[4] = 2; indices[5] = 3;

      float modelMtx[16];
      memcpy(modelMtx, glm::value_ptr(canvasEntry.model), sizeof(modelMtx));
      bgfx::setTransform(modelMtx);
      bgfx::setVertexBuffer(0, &tvb);
      bgfx::setIndexBuffer(&tib);
      bgfx::setTexture(0, m_UISampler, texture);
      bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_BLEND_ALPHA);
      bgfx::submit(worldViewId, m_UIProgram);
   };

   const bgfx::Caps* uiCaps = bgfx::getCaps();
   const uint16_t maxViews = uiCaps ? static_cast<uint16_t>(uiCaps->limits.maxViews) : 256;
   const uint16_t captureViewMin = 60;
   const uint16_t captureViewMax = static_cast<uint16_t>(std::min<int>(maxViews > 1 ? (maxViews - 2) : 0, 198));
   auto allocateCaptureViewId = [&](std::unordered_set<uint16_t>& used) -> uint16_t {
      if (captureViewMax < captureViewMin) return 0;
      for (uint16_t candidate = m_NextUISceneCaptureViewId; candidate <= captureViewMax; candidate = static_cast<uint16_t>(candidate + 2)) {
         if (used.find(candidate) == used.end()) {
            m_NextUISceneCaptureViewId = static_cast<uint16_t>(candidate + 2);
            return candidate;
         }
      }
      for (uint16_t candidate = captureViewMin; candidate <= captureViewMax; candidate = static_cast<uint16_t>(candidate + 2)) {
         if (used.find(candidate) == used.end()) {
            m_NextUISceneCaptureViewId = static_cast<uint16_t>(candidate + 2);
            return candidate;
         }
      }
      return 0;
   };

   const float savedMouseX = m_UIMouseX;
   const float savedMouseY = m_UIMouseY;
   const float savedMouseNX = m_UIMouseNX;
   const float savedMouseNY = m_UIMouseNY;
   const bool savedMouseValid = m_UIMouseValid;
   const bool savedMouseNormalizedValid = m_UIMouseNormalizedValid;

   for (auto& canvasEntry : canvases) {
      uint16_t& viewBase = m_UIWorldCanvasViewIds[canvasEntry.entityId];
      const bool viewIdOutOfRange = (viewBase < captureViewMin)
         || (viewBase > captureViewMax)
         || (viewBase + 1 >= maxViews);
      if (viewBase == 0 || viewIdOutOfRange || currentOffscreenViewIds.find(viewBase) != currentOffscreenViewIds.end()) {
         viewBase = allocateCaptureViewId(currentOffscreenViewIds);
      }
      if (viewBase == 0) {
         continue;
      }

      bgfx::FrameBufferHandle targetFramebuffer = BGFX_INVALID_HANDLE;
      bgfx::TextureHandle targetTexture = EnsureOffscreenTexture(
         viewBase,
         canvasEntry.renderWidth,
         canvasEntry.renderHeight,
         &targetFramebuffer);
      if (!bgfx::isValid(targetTexture) || !bgfx::isValid(targetFramebuffer)) {
         continue;
      }
      currentOffscreenViewIds.insert(viewBase);

      bgfx::setViewFrameBuffer(viewBase, targetFramebuffer);
      bgfx::setViewRect(viewBase, 0, 0,
         static_cast<uint16_t>(canvasEntry.renderWidth),
         static_cast<uint16_t>(canvasEntry.renderHeight));
      bgfx::setViewClear(viewBase, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x00000000, 1.0f, 0);
      bgfx::touch(viewBase);

      const bool canvasAllowInput = allowInput && canvasEntry.entityId == inputCanvasEntity && canvasEntry.hit;
      if (canvasAllowInput) {
         m_UIMouseX = canvasEntry.hitMouseX;
         m_UIMouseY = canvasEntry.hitMouseY;
         m_UIMouseNX = 0.0f;
         m_UIMouseNY = 0.0f;
         m_UIMouseValid = true;
         m_UIMouseNormalizedValid = false;
      }

      RenderUIOverlay(
         scene,
         viewBase,
         canvasEntry.renderWidth,
         canvasEntry.renderHeight,
         canvasAllowInput,
         CanvasComponent::RenderSpace::WorldSpace,
         canvasEntry.entityId,
         &currentOffscreenViewIds,
         false,
         false);

      if (canvasAllowInput) {
         m_UIMouseX = savedMouseX;
         m_UIMouseY = savedMouseY;
         m_UIMouseNX = savedMouseNX;
         m_UIMouseNY = savedMouseNY;
         m_UIMouseValid = savedMouseValid;
         m_UIMouseNormalizedValid = savedMouseNormalizedValid;
      }

      submitWorldCanvasQuad(canvasEntry, targetTexture);
   }

   m_UIMouseX = savedMouseX;
   m_UIMouseY = savedMouseY;
   m_UIMouseNX = savedMouseNX;
   m_UIMouseNY = savedMouseNY;
   m_UIMouseValid = savedMouseValid;
   m_UIMouseNormalizedValid = savedMouseNormalizedValid;
}

void Renderer::RenderScene(Scene& scene, uint16_t viewId)
   {
   // Prepare camera matrices (respect scene camera only while playing)
   Camera* renderCamera = scene.m_IsPlaying ? scene.GetActiveCamera() : nullptr;
   if (!renderCamera) renderCamera = m_RendererCamera;
   if (!renderCamera) {
      std::cerr << "[Renderer] RenderScene called with null camera!" << std::endl;
      return;
   }
   glm::mat4 view = renderCamera->GetViewMatrix();
   glm::mat4 proj = renderCamera->GetProjectionMatrix();
   bgfx::setViewTransform(viewId, glm::value_ptr(view), glm::value_ptr(proj));
   bgfx::touch(viewId);

   glm::vec4 camPos(renderCamera->GetPosition(), 1.0f);
   bgfx::setUniform(u_cameraPos, &camPos);

   uint32_t activeLayerMask = 0xFFFFFFFFu;
   bool enforceLayerMask = false;
   if (scene.m_IsPlaying) {
      for (const auto& eCam : scene.GetEntities()) {
         auto* dCam = scene.GetEntityData(eCam.GetID());
         if (dCam && dCam->Camera && &dCam->Camera->Camera == renderCamera) {
            activeLayerMask = dCam->Camera->LayerMask;
            enforceLayerMask = true;
            break;
         }
      }
   }

   const bool editorLightingOverride = ShouldApplyEditorLightingOverride(scene);
   UploadEnvironmentToShader(scene.GetEnvironment(), editorLightingOverride);

   Scene* prevShadowScene = m_ShadowContextScene;
   bool prevShadowEnabled = m_ShadowContextEnabled;
   // This render path does not generate shadow maps, so disable to avoid leakage.
   m_ShadowContextScene = &scene;
   m_ShadowContextEnabled = false;

   // PERFORMANCE: Use scratch buffer instead of per-frame allocation
   m_ScratchLights.clear();
   if (m_ScratchLights.capacity() < static_cast<size_t>(kMaxShaderLights)) {
      m_ScratchLights.reserve(static_cast<size_t>(kMaxShaderLights));
   }
   auto buildLightData = [&](const EntityData* data) -> LightData {
      LightData ld;
      ld.type = data->Light->Type;
      ld.color = data->Light->Color * data->Light->Intensity;
      ld.position = glm::vec3(data->Transform.WorldMatrix[3]);
      if (data->Light->Type == LightType::Directional) {
         ld.direction = LightDirectionFromTransform(data->Transform);
         ld.range = 0.0f; ld.constant = 1.0f; ld.linear = 0.0f; ld.quadratic = 0.0f;
      } else {
         ld.direction = glm::vec3(0.0f);
         ld.range = 50.0f; ld.constant = 1.0f; ld.linear = 0.09f; ld.quadratic = 0.032f;
      }
      return ld;
   };
   EntityID primaryDirectional = INVALID_ENTITY_ID;
   std::vector<std::pair<float, EntityID>> pointCandidates;
   pointCandidates.reserve(scene.GetEntities().size());
   const glm::vec3 lightSelectOrigin = renderCamera ? renderCamera->GetPosition() : glm::vec3(0.0f);
   for (auto& entity : scene.GetEntities()) {
      auto* data = scene.GetEntityData(entity.GetID());
      if (!data || !data->Light || !IsPresentationVisible(data)) continue;
      if (data->Light->Type == LightType::Directional) {
         if (primaryDirectional == INVALID_ENTITY_ID) primaryDirectional = entity.GetID();
         continue;
      }
      const glm::vec3 lp = glm::vec3(data->Transform.WorldMatrix[3]);
      const float distSq = glm::dot(lp - lightSelectOrigin, lp - lightSelectOrigin);
      pointCandidates.emplace_back(distSq, entity.GetID());
   }
   std::sort(pointCandidates.begin(), pointCandidates.end(),
      [](const auto& a, const auto& b) { return a.first < b.first; });
   if (primaryDirectional != INVALID_ENTITY_ID) {
      if (auto* d = scene.GetEntityData(primaryDirectional); d && d->Light) {
         m_ScratchLights.push_back(buildLightData(d));
      }
   }
   for (const auto& candidate : pointCandidates) {
      if (m_ScratchLights.size() >= static_cast<size_t>(kMaxShaderLights)) break;
      auto* d = scene.GetEntityData(candidate.second);
      if (!d || !d->Light) continue;
      m_ScratchLights.push_back(buildLightData(d));
   }

   if (editorLightingOverride) {
      m_ScratchLights.clear();
   }

#ifndef CLAYMORE_RUNTIME
   const bool showGridInEditor = Application::HasInstance() && Application::Get().m_RunEditorUI && !scene.m_IsPlaying && m_ShowGrid;
   if (showGridInEditor) {
      DrawGrid(viewId);
   }
#endif
   UploadLightsToShader(m_ScratchLights);
   std::shared_ptr<Material> fallbackStaticMaterial;
   std::shared_ptr<Material> fallbackSkinnedMaterial;
   auto resolveProgramMaterial = [&](Material* source, bool needsSkinned) -> Material* {
      return ResolveMaterialWithValidProgram(
         scene,
         source,
         needsSkinned,
         fallbackStaticMaterial,
         fallbackSkinnedMaterial);
    };
   const bgfx::ProgramHandle viewportSkinnedPbrProgram =
      ShaderManager::Instance().LoadProgram("vs_pbr_skinned", "fs_pbr_skinned");
   auto resolveViewportGpuMorphPath = [&](EntityID entityId,
                                          EntityData* data,
                                          Mesh* mesh,
                                          Material* drawMat,
                                          bgfx::VertexBufferHandle& outVertexBuffer,
                                          bgfx::ProgramHandle& outProgram,
                                          bool& outUsesGpuMorph,
                                          bool& outUsesMaterializedSkinning) -> bool {
      outVertexBuffer = BGFX_INVALID_HANDLE;
      outProgram = BGFX_INVALID_HANDLE;
      outUsesGpuMorph = false;
      outUsesMaterializedSkinning = false;

      if (!data || !data->Skinning || !mesh || !drawMat) {
         return false;
      }

      if (TryGetGpuMaterializedSkinnedVertexBuffer(scene, entityId, mesh, outVertexBuffer) &&
          ResolveGpuMaterializedSkinnedColorProgram(drawMat->GetProgram(), outProgram)) {
         outUsesMaterializedSkinning = true;
         return bgfx::isValid(outVertexBuffer) && bgfx::isValid(outProgram);
      }

      if (!mesh->Dynamic ||
          !data->Skinning->HasGpuSkinningInstanceRecord() ||
          !ShouldUseGpuMorphTargets(data->Skinning.get(), mesh, data->BlendShapes.get()) ||
          !bgfx::isValid(viewportSkinnedPbrProgram) ||
          !bgfx::isValid(m_PBRSkinnedMorphProgram) ||
          !bgfx::isValid(drawMat->GetProgram()) ||
          drawMat->GetProgram().idx != viewportSkinnedPbrProgram.idx) {
         return false;
      }

      outVertexBuffer = GetOrCreateGpuMorphVertexBuffer(mesh);
      outProgram = m_PBRSkinnedMorphProgram;
      outUsesGpuMorph = true;
      return bgfx::isValid(outVertexBuffer) && bgfx::isValid(outProgram);
   };

   // PERFORMANCE: Use scratch buffer instead of per-frame allocation
   m_ScratchVisibleMeshIds.clear();
   const size_t entityCount = scene.GetEntities().size();
   if (m_ScratchVisibleMeshIds.capacity() < entityCount) m_ScratchVisibleMeshIds.reserve(entityCount);
   for (const auto& eSnap : scene.GetEntities()) {
      EntityID eidSnap = eSnap.GetID();
      auto* dSnap = scene.GetEntityData(eidSnap);
      if (!IsPresentationVisible(dSnap) || !dSnap->Mesh || !dSnap->Mesh->mesh) continue;
      if (enforceLayerMask) {
         if (((activeLayerMask >> (dSnap->Layer & 31)) & 1u) == 0u) continue;
      }
      m_ScratchVisibleMeshIds.push_back(eidSnap);
   }

   PrepareGpuSkinningAtlases(scene, m_ScratchVisibleMeshIds);
   PrepareGpuMaterializedSkinnedMeshes(scene, m_ScratchVisibleMeshIds);

   // Build frustum from THIS render's camera matrices for proper culling in offscreen views
   // (Don't use m_view/m_proj which belong to the main viewport)
   float viewArr[16], projArr[16];
   memcpy(viewArr, glm::value_ptr(view), sizeof(float) * 16);
   memcpy(projArr, glm::value_ptr(proj), sizeof(float) * 16);
   
   Frustum fr;
   const bool doCull = m_EnableFrustumCulling;
   if (doCull) {
      fr = BuildFrustum(viewArr, projArr);
   }
   const cm::world::RuntimeWorld* runtimeWorld = scene.GetRuntimeWorld();
   auto* ragdollSystem = scene.m_IsPlaying ? cm::physics::GetRagdollSystem() : nullptr;
   std::unordered_map<EntityID, std::pair<glm::vec3, glm::vec3>> ragdollBoundsCache;
   ragdollBoundsCache.reserve(16);
   std::unordered_map<EntityID, bool> sharedViewCullCache;
   sharedViewCullCache.reserve(64);
   auto tryGetRagdollCullBounds = [&](const EntityData* data,
                                      const Mesh* mesh,
                                      glm::vec3& outMin,
                                      glm::vec3& outMax) -> bool {
      return TryGetSharedSkinnedCharacterBounds(
         scene,
         data,
         mesh,
         runtimeWorld,
         ragdollSystem,
         ragdollBoundsCache,
         outMin,
         outMax);
   };

   for (EntityID eid : m_ScratchVisibleMeshIds) {
      auto* data = scene.GetEntityData(eid);
      if (!IsPresentationVisible(data) || !data->Mesh || !data->Mesh->mesh) continue;
      std::shared_ptr<Mesh> meshPtr = data->Mesh->mesh; if (!meshPtr) continue;

      // CPU frustum culling using transformed local AABB
      // Skip culling if mesh has SkipFrustumCulling enabled (e.g. first-person arms)
      if (doCull && !data->Mesh->SkipFrustumCulling) {
         glm::vec3 wmin(0.0f);
         glm::vec3 wmax(0.0f);
         const bool haveSharedSkinnedBounds = tryGetRagdollCullBounds(data, meshPtr.get(), wmin, wmax);
         if (!haveSharedSkinnedBounds) {
            // Apply BoundsPadding multiplier for skinned/animated meshes
            const glm::vec3 center = (meshPtr->BoundsMin + meshPtr->BoundsMax) * 0.5f;
            const glm::vec3 extents = (meshPtr->BoundsMax - meshPtr->BoundsMin) * 0.5f * data->Mesh->BoundsPadding;
            const glm::vec3 lmin = center - extents;
            const glm::vec3 lmax = center + extents;
            glm::vec3 corners[8] = {
               {lmin.x, lmin.y, lmin.z}, {lmax.x, lmin.y, lmin.z}, {lmin.x, lmax.y, lmin.z}, {lmax.x, lmax.y, lmin.z},
               {lmin.x, lmin.y, lmax.z}, {lmax.x, lmin.y, lmax.z}, {lmin.x, lmax.y, lmax.z}, {lmax.x, lmax.y, lmax.z}
            };
            wmin = glm::vec3(std::numeric_limits<float>::max());
            wmax = glm::vec3(-std::numeric_limits<float>::max());
            for (int i = 0; i < 8; ++i) {
               glm::vec3 w = glm::vec3(data->Transform.WorldMatrix * glm::vec4(corners[i], 1.0f));
               wmin = glm::min(wmin, w);
               wmax = glm::max(wmax, w);
            }
         }
         bool visible = false;
         if (haveSharedSkinnedBounds) {
            const EntityID skelRoot = ResolveSkinningSkeletonRootEntity(data);
            if (skelRoot != INVALID_ENTITY_ID) {
               auto cacheIt = sharedViewCullCache.find(skelRoot);
               if (cacheIt == sharedViewCullCache.end()) {
                  cacheIt = sharedViewCullCache.emplace(skelRoot, AabbIntersectsFrustum(fr, wmin, wmax)).first;
               }
               visible = cacheIt->second;
            } else {
               visible = AabbIntersectsFrustum(fr, wmin, wmax);
            }
         } else {
            visible = AabbIntersectsFrustum(fr, wmin, wmax);
         }
         if (!visible) continue;
      }

      if (!HasRenderableVertexSource(*this, data, meshPtr.get())) continue;

      float transform[16]; memcpy(transform, glm::value_ptr(data->Transform.WorldMatrix), sizeof(float) * 16);

      if (!meshPtr->Submeshes.empty() && !data->Mesh->materials.empty()) {
         for (const auto& sm : meshPtr->Submeshes) {
            const size_t slot = sm.materialSlot < data->Mesh->materials.size() ? sm.materialSlot : 0;
            Material* sourceMat = data->Mesh->materials[slot] ? data->Mesh->materials[slot].get() : data->Mesh->material.get();
            if (!sourceMat) sourceMat = data->Mesh->material.get();
            Material* mat = resolveProgramMaterial(sourceMat, data->Skinning != nullptr);
            if (!mat) continue;
            bool usesGpuMorph = false;
            bool usesMaterializedSkinning = false;
            bgfx::ProgramHandle submitProgram = mat->GetProgram();
            bgfx::setTransform(transform);
            bgfx::VertexBufferHandle resolvedVbh = BGFX_INVALID_HANDLE;
            if (resolveViewportGpuMorphPath(
                   eid,
                   data,
                   meshPtr.get(),
                   mat,
                   resolvedVbh,
                   submitProgram,
                   usesGpuMorph,
                   usesMaterializedSkinning)) {
               bgfx::setVertexBuffer(0, resolvedVbh);
            } else if (meshPtr->Dynamic) {
               if (bgfx::isValid(meshPtr->dvbh)) {
                  bgfx::setVertexBuffer(0, meshPtr->dvbh, 0, meshPtr->numVertices);
               } else { continue; }
            } else {
               bgfx::setVertexBuffer(0, meshPtr->vbh);
            }
            bgfx::setIndexBuffer(meshPtr->ibh, sm.indexStart, sm.indexCount);
            // Normal matrix for this draw (handles non-uniform scaling correctly)
            SetNormalMatrixUniform(transform);
            mat->BindUniforms();
            // Ensure shadow sampler/texture are bound on slot 7 for this offscreen view
            BindShadowUniforms();
            // Apply globals for each submesh draw
            GlobalShaderProperties::Instance().Apply();
            // Apply per-slot property block if present; else fall back to component block
            const MaterialPropertyBlock* pb = nullptr;
            if (data->Mesh && sm.materialSlot < data->Mesh->SlotPropertyBlocks.size()) {
               if (!data->Mesh->SlotPropertyBlocks[sm.materialSlot].Empty()) pb = &data->Mesh->SlotPropertyBlocks[sm.materialSlot];
            }
            if (!pb && data->Mesh && !data->Mesh->PropertyBlock.Empty()) pb = &data->Mesh->PropertyBlock;
            if (pb) mat->ApplyPropertyBlock(*pb);
            {
               static const PropertyID shadowReceiveId = PropertyID::Get("u_shadowReceive");
               float receive = 1.0f;
               if (auto pbr = dynamic_cast<const PBRMaterial*>(mat)) {
                  if (pbr->GetReceiveShadowsOverride()) {
                     receive = pbr->GetReceiveShadows() ? 1.0f : 0.0f;
                  }
               }
               if (pb) {
                  glm::vec4 value;
                  if (pb->TryGetVector(shadowReceiveId, value)) {
                     receive = value.x;
                  }
               }
               if (data->RenderOverrides && !data->RenderOverrides->ReceiveShadows) {
                  receive = 0.0f;
               }
               receive = glm::clamp(receive, 0.0f, 1.0f);
               glm::vec4 receiveVec(receive, 0.0f, 0.0f, 0.0f);
               bgfx::setUniform(u_ShadowReceive, &receiveVec);
            }
            uint64_t stateFlags = sourceMat ? sourceMat->GetStateFlags() : mat->GetStateFlags();
            // Optional render-on-top override (for overlapping transparent faces, etc.)
            if (data->Mesh && data->Mesh->RenderOnTop) {
               stateFlags &= ~BGFX_STATE_WRITE_Z;
               stateFlags &= ~BGFX_STATE_DEPTH_TEST_MASK;
               stateFlags |= BGFX_STATE_DEPTH_TEST_ALWAYS;
            }
            bgfx::setState(stateFlags);
            if (data->Skinning) {
               if (usesGpuMorph) {
                  BindSkinningInstanceRecord(data->Skinning->GpuInstanceAtlasRecordIndex);
               } else if (!usesMaterializedSkinning) {
                  Renderer::SkinningBindCacheState singleSkinningCache{};
                  BindSkinningIfChanged(data->Skinning.get(), singleSkinningCache);
               }
            }
            if (usesGpuMorph) {
               SafeSubmit(viewId, submitProgram, "vs_pbr_skinned_morph+fs_pbr_skinned");
            } else {
               bgfx::submit(viewId, submitProgram);
            }
         }
      } else {
         const MaterialPropertyBlock* slotBlock = nullptr;
         if (data->Mesh && !data->Mesh->SlotPropertyBlocks.empty()) {
            const auto& slotPB = data->Mesh->SlotPropertyBlocks[0];
            if (!slotPB.Empty()) slotBlock = &slotPB;
         }
         const MaterialPropertyBlock* blockToUse = slotBlock;
         if (!blockToUse && data->Mesh && !data->Mesh->PropertyBlock.Empty()) {
            blockToUse = &data->Mesh->PropertyBlock;
         }
         if (!data->Mesh || !data->Mesh->material) {
            continue;
         }
         Material* drawMat = resolveProgramMaterial(data->Mesh->material.get(), data->Skinning != nullptr);
         if (!drawMat) {
            continue;
         }
         bgfx::VertexBufferHandle resolvedVbh = BGFX_INVALID_HANDLE;
         bgfx::ProgramHandle submitProgram = drawMat->GetProgram();
         bool usesGpuMorph = false;
         bool usesMaterializedSkinning = false;
         if (resolveViewportGpuMorphPath(
                eid,
                data,
                meshPtr.get(),
                drawMat,
                resolvedVbh,
                submitProgram,
                usesGpuMorph,
                usesMaterializedSkinning)) {
            bgfx::setTransform(transform);
            bgfx::setVertexBuffer(0, resolvedVbh);
            bgfx::setIndexBuffer(meshPtr->ibh);
            SetNormalMatrixUniform(transform);
            drawMat->BindUniforms();
            BindShadowUniforms();
            GlobalShaderProperties::Instance().Apply();
            if (blockToUse && !blockToUse->Empty()) {
               drawMat->ApplyPropertyBlock(*blockToUse);
            }
            {
               static const PropertyID shadowReceiveId = PropertyID::Get("u_shadowReceive");
               float receive = 1.0f;
               if (auto pbr = dynamic_cast<const PBRMaterial*>(drawMat)) {
                  if (pbr->GetReceiveShadowsOverride()) {
                     receive = pbr->GetReceiveShadows() ? 1.0f : 0.0f;
                  }
               }
               if (blockToUse) {
                  glm::vec4 value;
                  if (blockToUse->TryGetVector(shadowReceiveId, value)) {
                     receive = value.x;
                  }
               }
               receive = glm::clamp(receive, 0.0f, 1.0f);
               glm::vec4 receiveVec(receive, 0.0f, 0.0f, 0.0f);
               bgfx::setUniform(u_ShadowReceive, &receiveVec);
            }
            bgfx::setState(drawMat->GetStateFlags());
            if (usesGpuMorph) {
               BindSkinningInstanceRecord(data->Skinning->GpuInstanceAtlasRecordIndex);
            }
            SafeSubmit(
               viewId,
               submitProgram,
               usesGpuMorph ? "vs_pbr_skinned_morph+fs_pbr_skinned" : "vs_pbr+fs_pbr");
         } else {
            if (data->Skinning) {
               if (!usesMaterializedSkinning) {
                  Renderer::SkinningBindCacheState singleSkinningCache{};
                  BindSkinningIfChanged(data->Skinning.get(), singleSkinningCache);
               }
            }
            DrawMesh(*meshPtr.get(), transform, *drawMat, viewId, blockToUse);
         }
      }
      }

   // NOTE: Particles are NOT rendered in offscreen views (prefab editor).
   // The particle system is a global singleton and doesn't track scene ownership,
   // so rendering here would cause main scene particles to appear in prefab editor
   // and vice versa. A proper fix requires scene-aware particle management.

   // Restore shadow context so other viewports don't inherit it
   m_ShadowContextScene = prevShadowScene;
   m_ShadowContextEnabled = prevShadowEnabled;
   }


// ---------------- RenderScene with RenderContext ----------------
void Renderer::RenderScene(const RenderContext& ctx)
{
    if (!ctx.scene || !ctx.camera) {
        std::cerr << "[Renderer] RenderScene(RenderContext) called with null scene or camera!" << std::endl;
        return;
    }
    
    Scene& scene = *ctx.scene;
    Camera* renderCamera = ctx.camera;
    const uint16_t viewId = ctx.viewId;
    const uint16_t worldUIViewId = (ctx.uiViewId == viewId) ? static_cast<uint16_t>(viewId + 1) : ctx.uiViewId;
    const uint16_t screenUIViewId = static_cast<uint16_t>(worldUIViewId + 1);
    
    if (ctx.width == 0 || ctx.height == 0) {
        return;
    }
    
    // Use matrices from context (already computed via UpdateFromCamera)
    if (bgfx::isValid(ctx.framebuffer)) {
        bgfx::setViewFrameBuffer(viewId, ctx.framebuffer);
        bgfx::setViewFrameBuffer(worldUIViewId, ctx.framebuffer);
        bgfx::setViewFrameBuffer(screenUIViewId, ctx.framebuffer);
    } else {
        bgfx::setViewFrameBuffer(viewId, BGFX_INVALID_HANDLE);
        bgfx::setViewFrameBuffer(worldUIViewId, BGFX_INVALID_HANDLE);
        bgfx::setViewFrameBuffer(screenUIViewId, BGFX_INVALID_HANDLE);
    }
    bgfx::setViewRect(viewId, 0, 0, (uint16_t)ctx.width, (uint16_t)ctx.height);
    bgfx::setViewRect(worldUIViewId, 0, 0, (uint16_t)ctx.width, (uint16_t)ctx.height);
    bgfx::setViewRect(screenUIViewId, 0, 0, (uint16_t)ctx.width, (uint16_t)ctx.height);
    if (ctx.isOffscreen) {
        bgfx::setViewClear(viewId, ctx.clearFlags, ctx.clearColor, ctx.clearDepth, ctx.clearStencil);
    }
    bgfx::setViewClear(worldUIViewId, BGFX_CLEAR_NONE, 0, 1.0f, 0);
    bgfx::setViewClear(screenUIViewId, BGFX_CLEAR_NONE, 0, 1.0f, 0);
    bgfx::setViewTransform(viewId, ctx.view, ctx.proj);
    bgfx::setViewTransform(worldUIViewId, ctx.view, ctx.proj);
    bgfx::setViewMode(worldUIViewId, bgfx::ViewMode::Sequential);
    bgfx::setViewMode(screenUIViewId, bgfx::ViewMode::Sequential);
    bgfx::touch(viewId);
    bgfx::touch(worldUIViewId);
    
    glm::vec4 camPos(renderCamera->GetPosition(), 1.0f);
    bgfx::setUniform(u_cameraPos, &camPos);
    
    uint32_t activeLayerMask = 0xFFFFFFFFu;
    bool enforceLayerMask = false;
    if (scene.m_IsPlaying) {
        for (const auto& eCam : scene.GetEntities()) {
            auto* dCam = scene.GetEntityData(eCam.GetID());
            if (dCam && dCam->Camera && &dCam->Camera->Camera == renderCamera) {
                activeLayerMask = dCam->Camera->LayerMask;
                enforceLayerMask = true;
                break;
            }
        }
    }
    
    const Environment& env = scene.GetEnvironment();
    const bool editorLightingOverride = ShouldApplyEditorLightingOverride(scene);
    UploadEnvironmentToShader(env, editorLightingOverride || ctx.forceFogDisabled);

    Scene* prevShadowScene = m_ShadowContextScene;
    bool prevShadowEnabled = m_ShadowContextEnabled;
    m_ShadowContextScene = &scene;
    m_ShadowContextEnabled = (ctx.enableShadows && !editorLightingOverride && env.ShadowsEnabled);
    
    // PERFORMANCE: Use scratch buffer instead of per-frame allocation
    m_ScratchLights.clear();
    if (m_ScratchLights.capacity() < static_cast<size_t>(kMaxShaderLights)) {
        m_ScratchLights.reserve(static_cast<size_t>(kMaxShaderLights));
    }
    auto buildLightData = [&](const EntityData* data) -> LightData {
        LightData ld;
        ld.type = data->Light->Type;
        ld.color = data->Light->Color * data->Light->Intensity;
        ld.position = glm::vec3(data->Transform.WorldMatrix[3]);
        if (data->Light->Type == LightType::Directional) {
            ld.direction = LightDirectionFromTransform(data->Transform);
            ld.range = 0.0f; ld.constant = 1.0f; ld.linear = 0.0f; ld.quadratic = 0.0f;
        } else {
            ld.direction = glm::vec3(0.0f);
            ld.range = 50.0f; ld.constant = 1.0f; ld.linear = 0.09f; ld.quadratic = 0.032f;
        }
        return ld;
    };
    EntityID primaryDirectional = INVALID_ENTITY_ID;
    std::vector<std::pair<float, EntityID>> pointCandidates;
    pointCandidates.reserve(scene.GetEntities().size());
    const glm::vec3 lightSelectOrigin = renderCamera ? renderCamera->GetPosition() : glm::vec3(0.0f);
    for (auto& entity : scene.GetEntities()) {
        auto* data = scene.GetEntityData(entity.GetID());
        if (!data || !data->Light || !IsPresentationVisible(data) || !data->Active) continue;
        if (data->Light->Type == LightType::Directional) {
            if (primaryDirectional == INVALID_ENTITY_ID) primaryDirectional = entity.GetID();
            continue;
        }
        const glm::vec3 lp = glm::vec3(data->Transform.WorldMatrix[3]);
        const float distSq = glm::dot(lp - lightSelectOrigin, lp - lightSelectOrigin);
        pointCandidates.emplace_back(distSq, entity.GetID());
    }
    std::sort(pointCandidates.begin(), pointCandidates.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });
    if (primaryDirectional != INVALID_ENTITY_ID) {
        if (auto* d = scene.GetEntityData(primaryDirectional); d && d->Light) {
            m_ScratchLights.push_back(buildLightData(d));
        }
    }
    for (const auto& candidate : pointCandidates) {
        if (m_ScratchLights.size() >= static_cast<size_t>(kMaxShaderLights)) break;
        auto* d = scene.GetEntityData(candidate.second);
        if (!d || !d->Light) continue;
        m_ScratchLights.push_back(buildLightData(d));
    }
    
    if (editorLightingOverride) {
        m_ScratchLights.clear();
    }
    
#ifndef CLAYMORE_RUNTIME
    // Draw grid if requested (editor-only)
    if (ctx.showGrid && !scene.m_IsPlaying) {
        DrawGrid(viewId);
    }
#endif
    UploadLightsToShader(m_ScratchLights);
    std::shared_ptr<Material> fallbackStaticMaterial;
    std::shared_ptr<Material> fallbackSkinnedMaterial;
    auto resolveProgramMaterial = [&](Material* source, bool needsSkinned) -> Material* {
        return ResolveMaterialWithValidProgram(
            scene,
            source,
            needsSkinned,
            fallbackStaticMaterial,
            fallbackSkinnedMaterial);
    };
    const bgfx::ProgramHandle contextSkinnedPbrProgram =
        ShaderManager::Instance().LoadProgram("vs_pbr_skinned", "fs_pbr_skinned");
    auto resolveContextGpuMorphPath = [&](EntityID entityId,
                                          EntityData* data,
                                          Mesh* mesh,
                                          Material* drawMat,
                                          bgfx::VertexBufferHandle& outVertexBuffer,
                                          bgfx::ProgramHandle& outProgram,
                                          bool& outUsesGpuMorph,
                                          bool& outUsesMaterializedSkinning) -> bool {
        outVertexBuffer = BGFX_INVALID_HANDLE;
        outProgram = BGFX_INVALID_HANDLE;
        outUsesGpuMorph = false;
        outUsesMaterializedSkinning = false;

        if (!data || !data->Skinning || !mesh || !drawMat) {
            return false;
        }

        if (TryGetGpuMaterializedSkinnedVertexBuffer(scene, entityId, mesh, outVertexBuffer) &&
            ResolveGpuMaterializedSkinnedColorProgram(drawMat->GetProgram(), outProgram)) {
            outUsesMaterializedSkinning = true;
            return bgfx::isValid(outVertexBuffer) && bgfx::isValid(outProgram);
        }

        if (!mesh->Dynamic ||
            !data->Skinning->HasGpuSkinningInstanceRecord() ||
            !ShouldUseGpuMorphTargets(data->Skinning.get(), mesh, data->BlendShapes.get()) ||
            !bgfx::isValid(contextSkinnedPbrProgram) ||
            !bgfx::isValid(m_PBRSkinnedMorphProgram) ||
            !bgfx::isValid(drawMat->GetProgram()) ||
            drawMat->GetProgram().idx != contextSkinnedPbrProgram.idx) {
            return false;
        }

        outVertexBuffer = GetOrCreateGpuMorphVertexBuffer(mesh);
        outProgram = m_PBRSkinnedMorphProgram;
        outUsesGpuMorph = true;
        return bgfx::isValid(outVertexBuffer) && bgfx::isValid(outProgram);
    };
    
    // PERFORMANCE: Use scratch buffer instead of per-frame allocation
    m_ScratchVisibleMeshIds.clear();
    const size_t entityCount = scene.GetEntities().size();
    if (m_ScratchVisibleMeshIds.capacity() < entityCount) m_ScratchVisibleMeshIds.reserve(entityCount);
    for (const auto& eSnap : scene.GetEntities()) {
        EntityID eidSnap = eSnap.GetID();
        auto* dSnap = scene.GetEntityData(eidSnap);
        if (!IsPresentationVisible(dSnap) || !dSnap->Active || !dSnap->Mesh || !dSnap->Mesh->mesh) continue;
        if (ctx.allowedEntities && ctx.allowedEntities->find(eidSnap) == ctx.allowedEntities->end()) continue;
        if (enforceLayerMask) {
            if (((activeLayerMask >> (dSnap->Layer & 31)) & 1u) == 0u) continue;
        }
        m_ScratchVisibleMeshIds.push_back(eidSnap);
    }
    
    // Build frustum from context matrices for proper culling
    Frustum fr;
    const bool doCull = ctx.enableFrustumCulling && m_EnableFrustumCulling;
    if (doCull) {
        fr = BuildFrustum(ctx.view, ctx.proj);
    }
    const cm::world::RuntimeWorld* runtimeWorldCtx = scene.GetRuntimeWorld();
    auto* ragdollSystemCtx = scene.m_IsPlaying ? cm::physics::GetRagdollSystem() : nullptr;
    std::unordered_map<EntityID, std::pair<glm::vec3, glm::vec3>> ragdollBoundsCacheCtx;
    ragdollBoundsCacheCtx.reserve(16);
    std::unordered_map<EntityID, bool> sharedContextCullCache;
    sharedContextCullCache.reserve(64);
    auto tryGetRagdollCullBoundsCtx = [&](const EntityData* data,
                                          const Mesh* mesh,
                                          glm::vec3& outMin,
                                          glm::vec3& outMax) -> bool {
        return TryGetSharedSkinnedCharacterBounds(
            scene,
            data,
            mesh,
            runtimeWorldCtx,
            ragdollSystemCtx,
            ragdollBoundsCacheCtx,
            outMin,
            outMax);
    };

    PrepareGpuSkinningAtlases(scene, m_ScratchVisibleMeshIds);
    PrepareGpuMaterializedSkinnedMeshes(scene, m_ScratchVisibleMeshIds);
    
    // Render meshes
    { ScopedTimer t("Render/Viewport/Meshes");
    Renderer::SkinningBindCacheState lastSkinning{};
    for (EntityID eid : m_ScratchVisibleMeshIds) {
        auto* data = scene.GetEntityData(eid);
        if (!IsPresentationVisible(data) || !data->Active || !data->Mesh || !data->Mesh->mesh) continue;
        std::shared_ptr<Mesh> meshPtr = data->Mesh->mesh;
        if (!meshPtr) continue;
        
        // CPU frustum culling
        // Skip culling if mesh has SkipFrustumCulling enabled (e.g. first-person arms)
        if (doCull && !data->Mesh->SkipFrustumCulling) {
            glm::vec3 wmin(0.0f);
            glm::vec3 wmax(0.0f);
            const bool haveSharedSkinnedBounds = tryGetRagdollCullBoundsCtx(data, meshPtr.get(), wmin, wmax);
            if (!haveSharedSkinnedBounds) {
                // Apply BoundsPadding multiplier for skinned/animated meshes
                const glm::vec3 center = (meshPtr->BoundsMin + meshPtr->BoundsMax) * 0.5f;
                const glm::vec3 extents = (meshPtr->BoundsMax - meshPtr->BoundsMin) * 0.5f * data->Mesh->BoundsPadding;
                const glm::vec3 lmin = center - extents;
                const glm::vec3 lmax = center + extents;
                glm::vec3 corners[8] = {
                    {lmin.x, lmin.y, lmin.z}, {lmax.x, lmin.y, lmin.z}, {lmin.x, lmax.y, lmin.z}, {lmax.x, lmax.y, lmin.z},
                    {lmin.x, lmin.y, lmax.z}, {lmax.x, lmin.y, lmax.z}, {lmin.x, lmax.y, lmax.z}, {lmax.x, lmax.y, lmax.z}
                };
                wmin = glm::vec3(std::numeric_limits<float>::max());
                wmax = glm::vec3(-std::numeric_limits<float>::max());
                for (int i = 0; i < 8; ++i) {
                    glm::vec3 w = glm::vec3(data->Transform.WorldMatrix * glm::vec4(corners[i], 1.0f));
                    wmin = glm::min(wmin, w);
                    wmax = glm::max(wmax, w);
                }
            }
            bool visible = false;
            if (haveSharedSkinnedBounds) {
                const EntityID skelRoot = ResolveSkinningSkeletonRootEntity(data);
                if (skelRoot != INVALID_ENTITY_ID) {
                    auto cacheIt = sharedContextCullCache.find(skelRoot);
                    if (cacheIt == sharedContextCullCache.end()) {
                        cacheIt = sharedContextCullCache.emplace(skelRoot, AabbIntersectsFrustum(fr, wmin, wmax)).first;
                    }
                    visible = cacheIt->second;
                } else {
                    visible = AabbIntersectsFrustum(fr, wmin, wmax);
                }
            } else {
                visible = AabbIntersectsFrustum(fr, wmin, wmax);
            }
            if (!visible) continue;
        }
        
        if (!HasRenderableVertexSource(*this, data, meshPtr.get())) continue;
        
        float transform[16];
        memcpy(transform, glm::value_ptr(data->Transform.WorldMatrix), sizeof(float) * 16);
        
        if (!meshPtr->Submeshes.empty() && !data->Mesh->materials.empty()) {
            for (const auto& sm : meshPtr->Submeshes) {
                const size_t slot = sm.materialSlot < data->Mesh->materials.size() ? sm.materialSlot : 0;
                Material* sourceMat = data->Mesh->materials[slot] ? data->Mesh->materials[slot].get() : data->Mesh->material.get();
                if (!sourceMat) sourceMat = data->Mesh->material.get();
                Material* mat = resolveProgramMaterial(sourceMat, data->Skinning != nullptr);
                if (!mat) continue;
                bool usesGpuMorph = false;
                bool usesMaterializedSkinning = false;
                bgfx::ProgramHandle submitProgram = mat->GetProgram();
                bgfx::setTransform(transform);
                bgfx::VertexBufferHandle resolvedVbh = BGFX_INVALID_HANDLE;
                if (resolveContextGpuMorphPath(
                       eid,
                       data,
                       meshPtr.get(),
                       mat,
                       resolvedVbh,
                       submitProgram,
                       usesGpuMorph,
                       usesMaterializedSkinning)) {
                    bgfx::setVertexBuffer(0, resolvedVbh);
                } else if (meshPtr->Dynamic) {
                    if (bgfx::isValid(meshPtr->dvbh)) {
                        bgfx::setVertexBuffer(0, meshPtr->dvbh, 0, meshPtr->numVertices);
                    } else { continue; }
                } else {
                    bgfx::setVertexBuffer(0, meshPtr->vbh);
                }
                bgfx::setIndexBuffer(meshPtr->ibh, sm.indexStart, sm.indexCount);
                SetNormalMatrixUniform(transform);
                mat->BindUniforms();
                BindShadowUniforms();
                GlobalShaderProperties::Instance().Apply();
                const MaterialPropertyBlock* pb = nullptr;
                if (data->Mesh && sm.materialSlot < data->Mesh->SlotPropertyBlocks.size()) {
                    if (!data->Mesh->SlotPropertyBlocks[sm.materialSlot].Empty()) pb = &data->Mesh->SlotPropertyBlocks[sm.materialSlot];
                }
                if (!pb && data->Mesh && !data->Mesh->PropertyBlock.Empty()) pb = &data->Mesh->PropertyBlock;
                if (pb) mat->ApplyPropertyBlock(*pb);
                {
                    static const PropertyID shadowReceiveId = PropertyID::Get("u_shadowReceive");
                    float receive = 1.0f;
                    if (auto pbr = dynamic_cast<const PBRMaterial*>(mat)) {
                        if (pbr->GetReceiveShadowsOverride()) {
                            receive = pbr->GetReceiveShadows() ? 1.0f : 0.0f;
                        }
                    }
                    if (pb) {
                        glm::vec4 value;
                        if (pb->TryGetVector(shadowReceiveId, value)) {
                            receive = value.x;
                        }
                    }
                    if (data->RenderOverrides && !data->RenderOverrides->ReceiveShadows) {
                        receive = 0.0f;
                    }
                    receive = glm::clamp(receive, 0.0f, 1.0f);
                    glm::vec4 receiveVec(receive, 0.0f, 0.0f, 0.0f);
                    bgfx::setUniform(u_ShadowReceive, &receiveVec);
                }
                uint64_t stateFlags = sourceMat ? sourceMat->GetStateFlags() : mat->GetStateFlags();
                if (data->Mesh && data->Mesh->RenderOnTop) {
                    stateFlags &= ~BGFX_STATE_WRITE_Z;
                    stateFlags &= ~BGFX_STATE_DEPTH_TEST_MASK;
                    stateFlags |= BGFX_STATE_DEPTH_TEST_ALWAYS;
                }
                bgfx::setState(stateFlags);
                if (data->Skinning) {
                    if (usesGpuMorph) {
                        BindSkinningInstanceRecord(data->Skinning->GpuInstanceAtlasRecordIndex);
                        lastSkinning = {};
                    } else if (!usesMaterializedSkinning) {
                        BindSkinningIfChanged(data->Skinning.get(), lastSkinning);
                    }
                }
                if (usesGpuMorph) {
                    SafeSubmit(viewId, submitProgram, "vs_pbr_skinned_morph+fs_pbr_skinned");
                } else {
                    bgfx::submit(viewId, submitProgram);
                }
            }
        } else {
            const MaterialPropertyBlock* slotBlock = nullptr;
            if (data->Mesh && !data->Mesh->SlotPropertyBlocks.empty()) {
                const auto& slotPB = data->Mesh->SlotPropertyBlocks[0];
                if (!slotPB.Empty()) slotBlock = &slotPB;
            }
            const MaterialPropertyBlock* blockToUse = slotBlock;
            if (!blockToUse && data->Mesh && !data->Mesh->PropertyBlock.Empty()) {
                blockToUse = &data->Mesh->PropertyBlock;
            }
            if (!data->Mesh || !data->Mesh->material) {
                continue;
            }
          Material* drawMat = resolveProgramMaterial(data->Mesh->material.get(), data->Skinning != nullptr);
          if (!drawMat) {
             continue;
          }
          bgfx::VertexBufferHandle resolvedVbh = BGFX_INVALID_HANDLE;
          bgfx::ProgramHandle submitProgram = drawMat->GetProgram();
          bool usesGpuMorph = false;
          bool usesMaterializedSkinning = false;
          if (resolveContextGpuMorphPath(
                 eid,
                 data,
                 meshPtr.get(),
                 drawMat,
                 resolvedVbh,
                 submitProgram,
                 usesGpuMorph,
                 usesMaterializedSkinning)) {
             bgfx::setTransform(transform);
             bgfx::setVertexBuffer(0, resolvedVbh);
             bgfx::setIndexBuffer(meshPtr->ibh);
             SetNormalMatrixUniform(transform);
             drawMat->BindUniforms();
             BindShadowUniforms();
             GlobalShaderProperties::Instance().Apply();
             if (blockToUse && !blockToUse->Empty()) {
                drawMat->ApplyPropertyBlock(*blockToUse);
             }
             {
                static const PropertyID shadowReceiveId = PropertyID::Get("u_shadowReceive");
                float receive = 1.0f;
                if (auto pbr = dynamic_cast<const PBRMaterial*>(drawMat)) {
                   if (pbr->GetReceiveShadowsOverride()) {
                      receive = pbr->GetReceiveShadows() ? 1.0f : 0.0f;
                   }
                }
                if (blockToUse) {
                   glm::vec4 value;
                   if (blockToUse->TryGetVector(shadowReceiveId, value)) {
                      receive = value.x;
                   }
                }
                receive = glm::clamp(receive, 0.0f, 1.0f);
                glm::vec4 receiveVec(receive, 0.0f, 0.0f, 0.0f);
                bgfx::setUniform(u_ShadowReceive, &receiveVec);
             }
             bgfx::setState(drawMat->GetStateFlags());
             if (usesGpuMorph) {
                BindSkinningInstanceRecord(data->Skinning->GpuInstanceAtlasRecordIndex);
                lastSkinning = {};
             }
             SafeSubmit(
                viewId,
                submitProgram,
                usesGpuMorph ? "vs_pbr_skinned_morph+fs_pbr_skinned" : "vs_pbr+fs_pbr");
          } else {
             if (data->Skinning) {
                if (!usesMaterializedSkinning) {
                   BindSkinningIfChanged(data->Skinning.get(), lastSkinning);
                }
             }
             DrawMesh(*meshPtr.get(), transform, *drawMat, viewId, blockToUse);
          }
        }
    }
    }
    
    // Render particles filtered by scene
    { ScopedTimer t("Render/Viewport/Particles");
    bx::Vec3 eye = { camPos.x, camPos.y, camPos.z };
    ecs::ParticleEmitterSystem::Get().Render(viewId, ctx.view, eye, ctx.scene);
    }
    
    // Draw text components (world or screen space)
    if (m_TextRenderer) {
        m_TextRenderer->RenderTexts(scene, ctx.view, ctx.proj, ctx.width, ctx.height, viewId, screenUIViewId);
    }
    
    std::unordered_set<uint16_t> currentUIOffscreenViewIds;
    bool allowWorldCanvasInput = ctx.allowUIInput;
    // UI overlay
    if (ctx.renderUIOverlay) {
        RenderUIOverlay(
            scene,
            screenUIViewId,
            ctx.width,
            ctx.height,
            ctx.allowUIInput,
            CanvasComponent::RenderSpace::ScreenSpace,
            INVALID_ENTITY_ID,
            &currentUIOffscreenViewIds,
            false,
            false);
        allowWorldCanvasInput = ctx.allowUIInput && !m_UIInputConsumed;
        RenderWorldSpaceCanvases(
            scene,
            renderCamera,
            ctx.view,
            ctx.proj,
            worldUIViewId,
            ctx.framebuffer,
            ctx.width,
            ctx.height,
            allowWorldCanvasInput,
            currentUIOffscreenViewIds);
        if (ctx.allowUIInput) {
            m_UIPrevMouseDown = Input::IsMouseButtonPressed(0);
        }
        for (uint16_t offscreenViewId : m_UISceneCaptureViewIds) {
            if (currentUIOffscreenViewIds.find(offscreenViewId) == currentUIOffscreenViewIds.end()) {
                ReleaseOffscreenTarget(offscreenViewId);
            }
        }
        m_UISceneCaptureViewIds = std::move(currentUIOffscreenViewIds);
    }

    // Restore shadow context so other viewports don't inherit it
    m_ShadowContextScene = prevShadowScene;
    m_ShadowContextEnabled = prevShadowEnabled;
}


// ---------------- Mesh Submission ----------------

void Renderer::DrawMesh(const Mesh& mesh, const float* transform, const Material& material, const MaterialPropertyBlock* propertyBlock) {
   bgfx::setTransform(transform);
   if (mesh.Dynamic) {
      if (bgfx::isValid(mesh.dvbh)) 
         {
         bgfx::setVertexBuffer(0, mesh.dvbh, 0, mesh.numVertices);
         }
      else 
         {
         std::cerr << "[Renderer] Tried to draw with invalid dynamic VBO!\n";
         return;
         }
      }
   else 
      {
      bgfx::setVertexBuffer(0, mesh.vbh);
      }

   bgfx::setIndexBuffer(mesh.ibh);

   // Bind shared material defaults first, then overlay per-entity overrides
   // Provide normal matrix to shaders that use it (handles non-uniform scaling correctly)
   SetNormalMatrixUniform(transform);

   material.BindUniforms();
   GlobalShaderProperties::Instance().Apply();
   if (propertyBlock && !propertyBlock->Empty()) {
      material.ApplyPropertyBlock(*propertyBlock);
      }
   // Use the material's depth state; allow per-entity override to render on top
   uint64_t stateFlags2 = material.GetStateFlags();
   // No access to entity here; this path is used by callers who can preconfigure state in material
   bgfx::setState(stateFlags2);
   auto materialProgram = material.GetProgram();

   if (!bgfx::isValid(material.GetProgram())) {
      std::cerr << "Invalid PBR shader program!\n";
      return;
      }

   bgfx::submit(1, materialProgram);
   }

void Renderer::DrawMesh(const Mesh& mesh, const float* transform, const Material& material, uint16_t viewId, const MaterialPropertyBlock* propertyBlock) {
   bgfx::setTransform(transform);
   if (mesh.Dynamic) {
      if (bgfx::isValid(mesh.dvbh)) bgfx::setVertexBuffer(0, mesh.dvbh, 0, mesh.numVertices); else { std::cerr << "[Renderer] Invalid dynamic VBO" << std::endl; return; }
      }
   else {
      bgfx::setVertexBuffer(0, mesh.vbh);
      }
   bgfx::setIndexBuffer(mesh.ibh);

   // Bind shared material defaults then overlay overrides so they win
   // Note: normal matrix set below applies regardless (handles non-uniform scaling correctly)
   SetNormalMatrixUniform(transform);
   material.BindUniforms();
   // Bind shadow resources on slot 7 so shaders expecting comparison sampler don't trip D3D11 SDK Layers
   BindShadowUniforms();
   GlobalShaderProperties::Instance().Apply();
   
   if (propertyBlock && !propertyBlock->Empty()) 
      { 
      material.ApplyPropertyBlock(*propertyBlock); 
      }
   {
      static const PropertyID shadowReceiveId = PropertyID::Get("u_shadowReceive");
      float receive = 1.0f;
      if (auto pbr = dynamic_cast<const PBRMaterial*>(&material)) {
         if (pbr->GetReceiveShadowsOverride()) {
            receive = pbr->GetReceiveShadows() ? 1.0f : 0.0f;
         }
      }
      if (propertyBlock) {
         glm::vec4 value;
         if (propertyBlock->TryGetVector(shadowReceiveId, value)) {
            receive = value.x;
         }
      }
      receive = glm::clamp(receive, 0.0f, 1.0f);
      glm::vec4 receiveVec(receive, 0.0f, 0.0f, 0.0f);
      bgfx::setUniform(u_ShadowReceive, &receiveVec);
   }

   bgfx::setState(material.GetStateFlags());
   if (!bgfx::isValid(material.GetProgram())) 
      { 
      std::cerr << "Invalid material program" << std::endl;
      return; 
      }

   bgfx::submit(viewId, material.GetProgram());
   }


// ---------------- Light Management ----------------
void Renderer::UploadLightsToShader(const std::vector<LightData>&lights) {
   glm::vec4 colors[kMaxShaderLights], positions[kMaxShaderLights], params[kMaxShaderLights];

   for (int i = 0; i < kMaxShaderLights; ++i) {
      if (i < lights.size()) {
         const LightData& light = lights[i];

         // Color with intensity in alpha
         colors[i] = glm::vec4(light.color, 1.0f);

         if (light.type == LightType::Directional) {
            // For directional lights: xyz = direction, w = 0 (directional)
            positions[i] = glm::vec4(light.direction, 0.0f);
            }
         else {
            // For point lights: xyz = position, w = 1 (point)
            positions[i] = glm::vec4(light.position, 1.0f);
            }

         // Light parameters: x = range, y = constant, z = linear, w = quadratic
         params[i] = glm::vec4(light.range, light.constant, light.linear, light.quadratic);
         }
      else {
         // Disabled light
         colors[i] = glm::vec4(0.0f);
         positions[i] = glm::vec4(0.0f);
         params[i] = glm::vec4(0.0f);
         }
      }

   bgfx::setUniform(u_LightColors, colors, kMaxShaderLights);
   bgfx::setUniform(u_LightPositions, positions, kMaxShaderLights);
   bgfx::setUniform(u_LightParams, params, kMaxShaderLights);
   }

void Renderer::UploadEnvironmentToShader(const Environment & env, bool forceFogDisabled)
   {
   // Pack ambient color * intensity in xyz, w = flags (bit0: fog enabled)
   glm::vec3 ambient = env.AmbientColor * env.AmbientIntensity;
   bool fogEnabled = env.EnableFog && !forceFogDisabled;
   float flags = fogEnabled ? 1.0f : 0.0f;
   glm::vec4 ambientFog(ambient, flags);
   bgfx::setUniform(u_AmbientFog, &ambientFog);

   // Fog params: x = density, yzw = fog color
   float fogDensity = fogEnabled ? env.FogDensity : 0.0f;
   glm::vec4 fogParams(fogDensity, env.FogColor.r, env.FogColor.g, env.FogColor.b);
   bgfx::setUniform(u_FogParams, &fogParams);

   // Sky params: x = procedural sky active, y = skybox cubemap available.
   const bool skyboxAvailable = env.UseSkybox && env.SkyboxTexture && env.SkyboxTexture->IsValid();
   glm::vec4 skyParams(env.ProceduralSky ? 1.0f : 0.0f, skyboxAvailable ? 1.0f : 0.0f, 0.0f, 0.0f);
   bgfx::setUniform(u_SkyParams, &skyParams);
   
    // Unity-style sky parameters
   glm::vec4 skyTop(env.SkyTopColor, 1.0f);
   glm::vec4 horizon(env.SkyHorizonColor, 1.0f);
   glm::vec4 groundColor(env.SkyGroundColor, 1.0f);
   bgfx::setUniform(u_SkyTopColor, &skyTop);
   bgfx::setUniform(u_SkyHorizonColor, &horizon);
   bgfx::setUniform(u_GroundColor, &groundColor);
   
   // Sun parameters: x=SunSize (0..1 mapped to angular radius), y=SunSizeConvergence (edge hardness), z=SunIntensity, w unused.
   glm::vec4 sunParams(env.SunSize, env.SunSizeConvergence, env.SunIntensity, 0.0f);
   bgfx::setUniform(u_SkySunParams, &sunParams);
   
   // Atmosphere parameters: x=AtmosphereThickness, y=HorizonFade, z=SkyExposure multiplier, w unused.
   glm::vec4 atmosphereParams(env.AtmosphereThickness, env.HorizonFade, env.SkyExposure, 0.0f);
   bgfx::setUniform(u_SkyAtmosphereParams, &atmosphereParams);

   // Scene-wide color grade for active PBR shaders.
   // Keep always enabled by default to provide a Blender-like baseline look without extra setup.
   glm::vec4 sceneColorGrade(glm::max(env.Exposure, 0.0f), 1.05f, 1.0f, 1.0f);
   bgfx::setUniform(u_SceneColorGrade, &sceneColorGrade);
   }

#ifndef CLAYMORE_RUNTIME
void Renderer::DrawGrid() {
   // View setup
   bgfx::setViewTransform(0, m_view, m_proj);
   bgfx::setViewRect(0, 0, 0, m_Width, m_Height);

   // Fixed, angle-independent bounds: square around camera projection on ground
   Camera* cam = GetCamera();
   if (!cam) return;

   const EditorSettings& settings = EditorSettings::Get();

   glm::vec3 camPos = cam->GetPosition();
   
   // Safety check: don't draw grid if camera position is invalid (NaN, Inf, or extreme values)
   if (!std::isfinite(camPos.x) || !std::isfinite(camPos.y) || !std::isfinite(camPos.z)) return;
   if (std::abs(camPos.x) > 1e6f || std::abs(camPos.y) > 1e6f || std::abs(camPos.z) > 1e6f) return;
   
   glm::vec2 groundCenter = { camPos.x, camPos.z };
   float height = std::max(0.001f, fabs(camPos.y));
   float extent = std::clamp(height * 8.0f, 10.0f, 400.0f);

   float minX = groundCenter.x - extent;
   float maxX = groundCenter.x + extent;
   float minZ = groundCenter.y - extent;
   float maxZ = groundCenter.y + extent;

   // Pad a little
   const float padding = 1.0f;
   minX -= padding; maxX += padding; minZ -= padding; maxZ += padding;

   // Snap to step
   const float step = 1.0f;
   auto floorTo = [&](float v) { return std::floor(v / step) * step; };
   auto ceilTo = [&](float v) { return std::ceil(v / step) * step; };
   minX = floorTo(minX); maxX = ceilTo(maxX); minZ = floorTo(minZ); maxZ = ceilTo(maxZ);

   // Safety: cap maximum grid lines to prevent memory explosion
   const int maxLines = 2000;
   int estimatedLines = (int)((maxX - minX) / step + 1) + (int)((maxZ - minZ) / step + 1);
   if (estimatedLines > maxLines) return;

   // Build grid lines, separating regular, major, and axis lines
   std::vector<GridVertex> regularLines;
   std::vector<GridVertex> majorLines;
   std::vector<GridVertex> axisLinesX;  // Red - X axis
   std::vector<GridVertex> axisLinesZ;  // Blue - Z axis
   
   regularLines.reserve(estimatedLines * 2);
   
   int majorInterval = settings.GridMajorLineInterval;
   bool showAxisLines = settings.GridShowAxisLines;

   // Lines parallel to Z axis (varying X)
   for (float x = minX; x <= maxX + 1e-4f; x += step) {
      int xi = (int)std::round(x);
      
      if (showAxisLines && std::abs(x) < step * 0.5f) {
         axisLinesZ.push_back({ x, 0.01f, minZ });
         axisLinesZ.push_back({ x, 0.01f, maxZ });
      } else if (majorInterval > 0 && xi % majorInterval == 0) {
         majorLines.push_back({ x, 0.005f, minZ });
         majorLines.push_back({ x, 0.005f, maxZ });
      } else {
         regularLines.push_back({ x, 0.0f, minZ });
         regularLines.push_back({ x, 0.0f, maxZ });
      }
   }
   
   // Lines parallel to X axis (varying Z)
   for (float z = minZ; z <= maxZ + 1e-4f; z += step) {
      int zi = (int)std::round(z);
      
      if (showAxisLines && std::abs(z) < step * 0.5f) {
         axisLinesX.push_back({ minX, 0.01f, z });
         axisLinesX.push_back({ maxX, 0.01f, z });
      } else if (majorInterval > 0 && zi % majorInterval == 0) {
         majorLines.push_back({ minX, 0.005f, z });
         majorLines.push_back({ maxX, 0.005f, z });
      } else {
         regularLines.push_back({ minX, 0.0f, z });
         regularLines.push_back({ maxX, 0.0f, z });
      }
   }

   float identity[16];
   bx::mtxIdentity(identity);
   if (!m_CachedDebugMaterial) return;
   
   // Helper lambda to draw a line batch with color
   auto drawLineBatch = [&](const std::vector<GridVertex>& verts, const glm::vec4& color) {
      if (verts.empty()) return;
      
      const bgfx::Memory* mem = bgfx::copy(verts.data(), (uint32_t)(verts.size() * sizeof(GridVertex)));
      bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(mem, GridVertex::layout);
      
      bgfx::setTransform(identity);
      bgfx::setVertexBuffer(0, vbh);
      m_CachedDebugMaterial->BindUniforms();
      
      glm::vec4 abgrColor(color.z, color.y, color.x, color.w);
      bgfx::setUniform(u_DebugColor, &abgrColor);
      
      bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_PT_LINES | BGFX_STATE_BLEND_ALPHA);
      if (bgfx::isValid(m_CachedDebugMaterial->GetProgram())) {
         bgfx::submit(0, m_CachedDebugMaterial->GetProgram());
      }
      bgfx::destroy(vbh);
   };

   // Draw regular grid lines
   drawLineBatch(regularLines, settings.GridColor);
   
   // Draw major grid lines
   if (majorInterval > 0) {
      drawLineBatch(majorLines, settings.GridMajorColor);
   }
   
   // Draw axis lines on top
   if (showAxisLines) {
      drawLineBatch(axisLinesX, settings.GridAxisColorX);
      drawLineBatch(axisLinesZ, settings.GridAxisColorZ);
   }
}
#endif // CLAYMORE_RUNTIME - DrawGrid() is editor-only

#ifndef CLAYMORE_RUNTIME
void Renderer::DrawGrid(uint16_t viewId) {
   if (!bgfx::isValid(m_GridVB)) return;

   Camera* cam = GetCamera(); 
   if (!cam) return;
   
   const EditorSettings& settings = EditorSettings::Get();
   
   glm::vec3 camPos = cam->GetPosition();
   glm::vec2 groundCenter = { camPos.x, camPos.z };

   float height = std::max(0.001f, fabs(camPos.y));
   float extent = std::clamp(height * 8.0f, 10.0f, 400.0f);
   float minX = groundCenter.x - extent; float maxX = groundCenter.x + extent;
   float minZ = groundCenter.y - extent; float maxZ = groundCenter.y + extent;

   const float step = 1.0f;
   auto floorTo = [&](float v) { return std::floor(v / step) * step; };
   auto ceilTo = [&](float v) { return std::ceil(v / step) * step; };

   minX = floorTo(minX); maxX = ceilTo(maxX); minZ = floorTo(minZ); maxZ = ceilTo(maxZ);
   
   // Build grid lines, separating regular, major, and axis lines
   std::vector<GridVertex> regularLines;
   std::vector<GridVertex> majorLines;
   std::vector<GridVertex> axisLinesX;  // Red - X axis
   std::vector<GridVertex> axisLinesZ;  // Blue - Z axis
   
   int majorInterval = settings.GridMajorLineInterval;
   bool showAxisLines = settings.GridShowAxisLines;

   // Lines parallel to Z axis (varying X)
   for (float x = minX; x <= maxX + 1e-4f; x += step) {
      int xi = (int)std::round(x);
      
      if (showAxisLines && std::abs(x) < step * 0.5f) {
         // Z axis line (at X=0) - Blue
         axisLinesZ.push_back({ x, 0.01f, minZ });
         axisLinesZ.push_back({ x, 0.01f, maxZ });
      } else if (majorInterval > 0 && xi % majorInterval == 0) {
         majorLines.push_back({ x, 0.005f, minZ });
         majorLines.push_back({ x, 0.005f, maxZ });
      } else {
         regularLines.push_back({ x, 0.0f, minZ });
         regularLines.push_back({ x, 0.0f, maxZ });
      }
   }
   
   // Lines parallel to X axis (varying Z)
   for (float z = minZ; z <= maxZ + 1e-4f; z += step) {
      int zi = (int)std::round(z);
      
      if (showAxisLines && std::abs(z) < step * 0.5f) {
         // X axis line (at Z=0) - Red
         axisLinesX.push_back({ minX, 0.01f, z });
         axisLinesX.push_back({ maxX, 0.01f, z });
      } else if (majorInterval > 0 && zi % majorInterval == 0) {
         majorLines.push_back({ minX, 0.005f, z });
         majorLines.push_back({ maxX, 0.005f, z });
      } else {
         regularLines.push_back({ minX, 0.0f, z });
         regularLines.push_back({ maxX, 0.0f, z });
      }
   }

   float identity[16]; 
   bx::mtxIdentity(identity);
   if (!m_CachedDebugMaterial) return;
   
   // Helper lambda to draw a line batch with color
   auto drawLineBatch = [&](const std::vector<GridVertex>& verts, const glm::vec4& color, float lineWidth = 1.0f) {
      if (verts.empty()) return;
      
      const bgfx::Memory* mem = bgfx::copy(verts.data(), (uint32_t)(verts.size() * sizeof(GridVertex)));
      bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(mem, GridVertex::layout);
      
      bgfx::setTransform(identity);
      bgfx::setVertexBuffer(0, vbh);
      m_CachedDebugMaterial->BindUniforms();
      
      // Apply color
      glm::vec4 abgrColor(color.z, color.y, color.x, color.w); // Convert RGBA to ABGR for bgfx
      bgfx::setUniform(u_DebugColor, &abgrColor);
      
      uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_PT_LINES | BGFX_STATE_BLEND_ALPHA;
      bgfx::setState(state);
      if (bgfx::isValid(m_CachedDebugMaterial->GetProgram())) {
         bgfx::submit(viewId, m_CachedDebugMaterial->GetProgram());
      }
      bgfx::destroy(vbh);
   };

   // Draw regular grid lines
   drawLineBatch(regularLines, settings.GridColor);
   
   // Draw major grid lines (brighter/thicker)
   if (majorInterval > 0) {
      drawLineBatch(majorLines, settings.GridMajorColor);
   }
   
   // Draw axis lines on top
   if (showAxisLines) {
      drawLineBatch(axisLinesX, settings.GridAxisColorX);  // X axis - Red
      drawLineBatch(axisLinesZ, settings.GridAxisColorZ);  // Z axis - Blue
   }
}
#endif // CLAYMORE_RUNTIME - DrawGrid(viewId) is editor-only

void Renderer::SetDebugDrawInPlayMode(bool enabled) {
#ifndef CLAYMORE_RUNTIME
   if (!Application::HasInstance()) {
      m_DebugDrawInPlayMode = enabled;
      PhysicsDebug::SetEnabled(enabled);
      return;
   }

   if (!Application::Get().m_RunEditorUI) {
      m_DebugDrawInPlayMode = false;
      PhysicsDebug::SetEnabled(false);
      return;
   }

   m_DebugDrawInPlayMode = enabled;
   PhysicsDebug::SetEnabled(enabled);
#else
   // In runtime builds, debug draw is always disabled
   m_DebugDrawInPlayMode = false;
   PhysicsDebug::SetEnabled(false);
#endif
}

bool Renderer::GetDebugDrawInPlayMode() const {
#ifndef CLAYMORE_RUNTIME
   if (!Application::HasInstance()) {
      return m_DebugDrawInPlayMode;
   }

   if (!Application::Get().m_RunEditorUI) {
      return false;
   }

   return m_DebugDrawInPlayMode;
#else
   return false;  // Always false in runtime builds
#endif
}

void Renderer::DrawDebugRay(const glm::vec3 & origin, const glm::vec3 & dir, float length) {
   if (!bgfx::isValid(m_DebugLineProgram)) return;
   glm::vec3 a = origin;
   glm::vec3 b = origin + (glm::length(dir) > 1e-6f ? glm::normalize(dir) : dir) * length;
   GridVertex line[2] = { {a.x,a.y,a.z}, {b.x,b.y,b.z} };
   const bgfx::Memory* mem = bgfx::copy(line, (uint32_t)sizeof(line));
   bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(mem, GridVertex::layout);
   float identity[16]; bx::mtxIdentity(identity); bgfx::setTransform(identity);
   bgfx::setVertexBuffer(0, vbh);
   bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_PT_LINES);
   ApplyDefaultDebugLineColor();
   bgfx::submit(kDebugOverlayViewId, m_DebugLineProgram);
   bgfx::destroy(vbh);
}

void Renderer::DrawDebugLineColored(const glm::vec3& from, const glm::vec3& to, uint32_t abgrColor) {
   if (!bgfx::isValid(m_DebugLineProgram)) return;
   
   GridVertex line[2] = { {from.x, from.y, from.z}, {to.x, to.y, to.z} };
   const bgfx::Memory* mem = bgfx::copy(line, (uint32_t)sizeof(line));
   bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(mem, GridVertex::layout);
   
   float identity[16]; 
   bx::mtxIdentity(identity); 
   bgfx::setTransform(identity);
   bgfx::setVertexBuffer(0, vbh);
   bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_PT_LINES | BGFX_STATE_BLEND_ALPHA);
   
   // Convert ABGR uint32_t to vec4 (RGBA normalized)
   float a = ((abgrColor >> 24) & 0xFF) / 255.0f;
   float b = ((abgrColor >> 16) & 0xFF) / 255.0f;
   float g = ((abgrColor >> 8) & 0xFF) / 255.0f;
   float r = (abgrColor & 0xFF) / 255.0f;
   ApplyDebugLineColor(glm::vec4(r, g, b, a));
   
   bgfx::submit(kDebugOverlayViewId, m_DebugLineProgram);
   bgfx::destroy(vbh);
}

void Renderer::DrawPhysicsDebugLines() {
   if (!bgfx::isValid(m_DebugLineProgram)) return;
   
   const auto& lines = PhysicsDebug::GetLines();
   if (lines.empty()) return;
   
   float identity[16];
   bx::mtxIdentity(identity);
   
   for (const auto& line : lines) {
      GridVertex verts[2] = { 
         {line.from.x, line.from.y, line.from.z}, 
         {line.to.x, line.to.y, line.to.z} 
      };
      const bgfx::Memory* mem = bgfx::copy(verts, (uint32_t)sizeof(verts));
      bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(mem, GridVertex::layout);
      
      bgfx::setTransform(identity);
      bgfx::setVertexBuffer(0, vbh);
      bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_PT_LINES | BGFX_STATE_BLEND_ALPHA);
      
      // Convert ABGR uint32_t to vec4 (RGBA normalized)
      float a = ((line.color >> 24) & 0xFF) / 255.0f;
      float b = ((line.color >> 16) & 0xFF) / 255.0f;
      float g = ((line.color >> 8) & 0xFF) / 255.0f;
      float r = (line.color & 0xFF) / 255.0f;
      ApplyDebugLineColor(glm::vec4(r, g, b, a));
      
      bgfx::submit(kDebugOverlayViewId, m_DebugLineProgram);
      bgfx::destroy(vbh);
   }
}

#ifndef CLAYMORE_RUNTIME
void Renderer::DrawCollider(const ColliderComponent & collider, const TransformComponent & transform, const Mesh* mesh, const RigidBodyComponent* rigidBody, const StaticBodyComponent* staticBody) {
   if (!bgfx::isValid(m_DebugLineProgram)) return;

   glm::vec3 scale(1.0f), skew(0.0f), transformWorldPos = glm::vec3(transform.WorldMatrix[3]);
   glm::vec4 perspective(0.0f);
   glm::quat transformWorldRot = transform.UseQuatRotation ?
      glm::normalize(transform.RotationQ) : glm::quat(glm::radians(transform.Rotation));
   if (glm::decompose(transform.WorldMatrix, scale, transformWorldRot, transformWorldPos, skew, perspective))
   {
      transformWorldRot = glm::normalize(transformWorldRot);
   }
   glm::vec3 worldScale = glm::abs(scale);

   glm::vec3 finalPos = transformWorldPos;
   glm::quat finalRot = transformWorldRot;

   JPH::BodyID bodyId = JPH::BodyID();
   if (rigidBody && !rigidBody->BodyID.IsInvalid())
   {
      bodyId = rigidBody->BodyID;
   }
   else if (staticBody && !staticBody->BodyID.IsInvalid())
   {
      bodyId = staticBody->BodyID;
   }

   bool usedLiveBodyTransform = false;
   if (!bodyId.IsInvalid())
   {
      glm::mat4 physicsWorld = Physics::Get().GetBodyTransform(bodyId);
      if (physicsWorld != glm::mat4(0.0f))
      {
         glm::vec3 physicsScale(1.0f), physicsSkew(0.0f), physicsPos = glm::vec3(physicsWorld[3]);
         glm::vec4 physicsPerspective(0.0f);
         glm::quat physicsRot(1.0f, 0.0f, 0.0f, 0.0f);
         if (glm::decompose(physicsWorld, physicsScale, physicsRot, physicsPos, physicsSkew, physicsPerspective))
         {
            finalPos = physicsPos;
            finalRot = glm::normalize(physicsRot);
            usedLiveBodyTransform = true;
         }
         else
         {
            finalPos = glm::vec3(physicsWorld[3]);
            usedLiveBodyTransform = true;
         }
      }
   }

   if (!usedLiveBodyTransform)
   {
      finalPos += glm::vec3(transform.WorldMatrix * glm::vec4(collider.Offset, 1.0f)) - transformWorldPos;
   }
   
   // Build world transform matrix: translate to final position, then rotate by world rotation
   // Scale is already applied to shape vertices, so we don't need to scale the transform
   glm::mat4 worldTransform = glm::translate(glm::mat4(1.0f), finalPos) * glm::toMat4(finalRot);

   float transformMatrix[16];
   memcpy(transformMatrix, glm::value_ptr(worldTransform), sizeof(float) * 16);
   bgfx::setTransform(transformMatrix);

   // Set debug material state
   bgfx::setState(
      BGFX_STATE_WRITE_RGB |
      BGFX_STATE_WRITE_Z |
      BGFX_STATE_DEPTH_TEST_LEQUAL |
      BGFX_STATE_PT_LINES
   );

   // Draw different shapes based on collider type
   switch (collider.ShapeType) {
         case ColliderShape::Box: {
         // Create wireframe box vertices
         std::vector<GridVertex> boxVertices;
         float halfSizeX = collider.Size.x * worldScale.x * 0.5f;
         float halfSizeY = collider.Size.y * worldScale.y * 0.5f;
         float halfSizeZ = collider.Size.z * worldScale.z * 0.5f;

         // Front face
         boxVertices.push_back({ -halfSizeX, -halfSizeY, -halfSizeZ });
         boxVertices.push_back({ halfSizeX, -halfSizeY, -halfSizeZ });
         boxVertices.push_back({ halfSizeX, -halfSizeY, -halfSizeZ });
         boxVertices.push_back({ halfSizeX,  halfSizeY, -halfSizeZ });
         boxVertices.push_back({ halfSizeX,  halfSizeY, -halfSizeZ });
         boxVertices.push_back({ -halfSizeX,  halfSizeY, -halfSizeZ });
         boxVertices.push_back({ -halfSizeX,  halfSizeY, -halfSizeZ });
         boxVertices.push_back({ -halfSizeX, -halfSizeY, -halfSizeZ });

         // Back face
         boxVertices.push_back({ -halfSizeX, -halfSizeY,  halfSizeZ });
         boxVertices.push_back({ halfSizeX, -halfSizeY,  halfSizeZ });
         boxVertices.push_back({ halfSizeX, -halfSizeY,  halfSizeZ });
         boxVertices.push_back({ halfSizeX,  halfSizeY,  halfSizeZ });
         boxVertices.push_back({ halfSizeX,  halfSizeY,  halfSizeZ });
         boxVertices.push_back({ -halfSizeX,  halfSizeY,  halfSizeZ });
         boxVertices.push_back({ -halfSizeX,  halfSizeY,  halfSizeZ });
         boxVertices.push_back({ -halfSizeX, -halfSizeY,  halfSizeZ });

         // Connecting lines
         boxVertices.push_back({ -halfSizeX, -halfSizeY, -halfSizeZ });
         boxVertices.push_back({ -halfSizeX, -halfSizeY,  halfSizeZ });
         boxVertices.push_back({ halfSizeX, -halfSizeY, -halfSizeZ });
         boxVertices.push_back({ halfSizeX, -halfSizeY,  halfSizeZ });
         boxVertices.push_back({ halfSizeX,  halfSizeY, -halfSizeZ });
         boxVertices.push_back({ halfSizeX,  halfSizeY,  halfSizeZ });
         boxVertices.push_back({ -halfSizeX,  halfSizeY, -halfSizeZ });
         boxVertices.push_back({ -halfSizeX,  halfSizeY,  halfSizeZ });

         const bgfx::Memory* mem = bgfx::copy(boxVertices.data(), sizeof(GridVertex) * boxVertices.size());
         bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(mem, GridVertex::layout);
         bgfx::setVertexBuffer(0, vbh);
         ApplyDefaultDebugLineColor();
         bgfx::submit(kDebugOverlayViewId, m_DebugLineProgram);
         bgfx::destroy(vbh);
         break;
         }
         case ColliderShape::Capsule: {
         // Draw capsule wireframe matching Jolt CapsuleShape (centered at origin)
         // Jolt's CapsuleShape extends from -halfHeight to +halfHeight along Y axis, centered at origin
         // This matches how RigidBody capsules are created in BuildShape()
         
         // Match physics exactly: BuildShape uses worldScale from WorldMatrix (before offset)
         // Then CreatePhysicsBody applies offset to WorldMatrix
         // So we need to extract scale from WorldMatrix (before offset) for shape size
         glm::vec3 scale, skew;
         glm::quat rot;
         glm::vec3 pos;
         glm::vec4 perspective;
         glm::decompose(transform.WorldMatrix, scale, rot, pos, skew, perspective);
         glm::vec3 ws = glm::abs(scale);
         
         // Match physics calculation exactly: radius uses max of X/Z scale, height uses Y scale
         // This matches BuildShape() calculation
         const float r = glm::max(collider.Radius * glm::max(ws.x, ws.z), 0.05f);
         const float halfCyl = glm::max((collider.Height * ws.y) * 0.5f, 0.0f); // cylinder half-height (excluding hemispheres)
         // No feetToCenter offset - capsule is centered at origin (matching Jolt CapsuleShape)

         const int segments = 20;
         std::vector<GridVertex> verts;
         verts.reserve(segments * 24);

         auto addRing = [&](float y, float radius) {
            for (int i = 0; i < segments; ++i) {
               float a0 = (float)i / segments * 6.2831853f;
               float a1 = (float)(i + 1) / segments * 6.2831853f;
               float x0 = radius * cosf(a0), z0 = radius * sinf(a0);
               float x1 = radius * cosf(a1), z1 = radius * sinf(a1);
               verts.push_back({ x0, y, z0 }); // No offset - centered at origin
               verts.push_back({ x1, y, z1 });
            }
         };

         auto addVerticalMeridian = [&](float angle) {
            float x = cosf(angle), z = sinf(angle);
            // Cylinder segment (centered at origin)
            verts.push_back({ r * x, -halfCyl, r * z });
            verts.push_back({ r * x,  halfCyl, r * z });
            // Bottom hemisphere arc (latitude -pi/2..0)
            const int arcSeg = 12;
            const float pi = 3.14159265f;
            for (int i = 0; i < arcSeg; ++i) {
               float t0 = -(pi * 0.5f) + (float)i / arcSeg * (pi * 0.5f);
               float t1 = -(pi * 0.5f) + (float)(i + 1) / arcSeg * (pi * 0.5f);
               float y0 = -halfCyl + r * sinf(t0); // No offset - centered at origin
               float y1 = -halfCyl + r * sinf(t1);
               float rr0 = r * cosf(t0);
               float rr1 = r * cosf(t1);
               verts.push_back({ rr0 * x, y0, rr0 * z });
               verts.push_back({ rr1 * x, y1, rr1 * z });
            }
            // Top hemisphere arc (latitude 0..pi/2)
            for (int i = 0; i < arcSeg; ++i) {
               float t0 = (float)i / arcSeg * (pi * 0.5f);
               float t1 = (float)(i + 1) / arcSeg * (pi * 0.5f);
               float y0 = halfCyl + r * sinf(t0); // No offset - centered at origin
               float y1 = halfCyl + r * sinf(t1);
               float rr0 = r * cosf(t0);
               float rr1 = r * cosf(t1);
               verts.push_back({ rr0 * x, y0, rr0 * z });
               verts.push_back({ rr1 * x, y1, rr1 * z });
            }
         };

         // Seams (rings) at cylinder caps
         addRing( halfCyl, r);
         addRing(-halfCyl, r);

         // Vertical meridians (0, 90, 180, 270 degrees)
         const float pi = 3.14159265f;
         float baseAngles[2] = { 0.0f, pi * 0.5f };
         for (float a : baseAngles) {
            addVerticalMeridian(a);
            addVerticalMeridian(a + pi);
         }

         if (!verts.empty()) {
            const bgfx::Memory* mem = bgfx::copy(verts.data(), (uint32_t)(verts.size() * sizeof(GridVertex)));
            bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(mem, GridVertex::layout);
            bgfx::setVertexBuffer(0, vbh);
            ApplyDefaultDebugLineColor();
            bgfx::submit(kDebugOverlayViewId, m_DebugLineProgram);
            bgfx::destroy(vbh);
         }
         break;
         }
         case ColliderShape::Sphere: {
         // Draw sphere wireframe as three orthogonal circles
         std::vector<GridVertex> sphereVertices;
         float radius = collider.Radius;
         const int segments = 32;

         // XY plane circle (Z-axis view)
         for (int i = 0; i <= segments; ++i) {
            float angle = (float)i / segments * 2.0f * 3.14159f;
            float x = cos(angle) * radius;
            float y = sin(angle) * radius;
            sphereVertices.push_back({ x, y, 0.0f });
         }

         // XZ plane circle (Y-axis view)
         for (int i = 0; i <= segments; ++i) {
            float angle = (float)i / segments * 2.0f * 3.14159f;
            float x = cos(angle) * radius;
            float z = sin(angle) * radius;
            sphereVertices.push_back({ x, 0.0f, z });
         }

         // YZ plane circle (X-axis view)
         for (int i = 0; i <= segments; ++i) {
            float angle = (float)i / segments * 2.0f * 3.14159f;
            float y = cos(angle) * radius;
            float z = sin(angle) * radius;
            sphereVertices.push_back({ 0.0f, y, z });
         }

         const bgfx::Memory* mem = bgfx::copy(sphereVertices.data(), sizeof(GridVertex) * sphereVertices.size());
         bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(mem, GridVertex::layout);
         bgfx::setVertexBuffer(0, vbh);
         ApplyDefaultDebugLineColor();
         bgfx::submit(kDebugOverlayViewId, m_DebugLineProgram);
         bgfx::destroy(vbh);
         break;
         }
         case ColliderShape::Mesh: {
         if (!mesh || mesh->Vertices.empty()) {
            break;
            }

         std::vector<GridVertex> meshLines;
         const auto& verts = mesh->Vertices;
         const auto& indices = mesh->Indices;

         if (!indices.empty()) {
            meshLines.reserve((indices.size() / 3) * 6);
            for (size_t i = 0; i + 2 < indices.size(); i += 3) {
               const uint32_t i0 = indices[i + 0];
               const uint32_t i1 = indices[i + 1];
               const uint32_t i2 = indices[i + 2];
               if (i0 >= verts.size() || i1 >= verts.size() || i2 >= verts.size()) {
                  continue;
                  }

               const glm::vec3 v0 = verts[i0] * worldScale;
               const glm::vec3 v1 = verts[i1] * worldScale;
               const glm::vec3 v2 = verts[i2] * worldScale;

               meshLines.push_back({ v0.x, v0.y, v0.z });
               meshLines.push_back({ v1.x, v1.y, v1.z });
               meshLines.push_back({ v1.x, v1.y, v1.z });
               meshLines.push_back({ v2.x, v2.y, v2.z });
               meshLines.push_back({ v2.x, v2.y, v2.z });
               meshLines.push_back({ v0.x, v0.y, v0.z });
               }
            }
         else {
            meshLines.reserve((verts.size() / 3) * 6);
            for (size_t i = 0; i + 2 < verts.size(); i += 3) {
               const glm::vec3 v0 = verts[i + 0] * worldScale;
               const glm::vec3 v1 = verts[i + 1] * worldScale;
               const glm::vec3 v2 = verts[i + 2] * worldScale;

               meshLines.push_back({ v0.x, v0.y, v0.z });
               meshLines.push_back({ v1.x, v1.y, v1.z });
               meshLines.push_back({ v1.x, v1.y, v1.z });
               meshLines.push_back({ v2.x, v2.y, v2.z });
               meshLines.push_back({ v2.x, v2.y, v2.z });
               meshLines.push_back({ v0.x, v0.y, v0.z });
               }
            }

         if (!meshLines.empty()) {
            const bgfx::Memory* mem = bgfx::copy(meshLines.data(), sizeof(GridVertex) * meshLines.size());
            bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(mem, GridVertex::layout);
            bgfx::setVertexBuffer(0, vbh);
            ApplyDefaultDebugLineColor();
            bgfx::submit(kDebugOverlayViewId, m_DebugLineProgram);
            bgfx::destroy(vbh);
            }
         break;
         }
      }
   }
#endif // CLAYMORE_RUNTIME - DrawCollider is editor-only

#ifndef CLAYMORE_RUNTIME
void Renderer::DrawAABB(const glm::vec3& worldMin, const glm::vec3& worldMax, uint16_t viewId) {
   glm::vec3 v000 = {worldMin.x, worldMin.y, worldMin.z};
   glm::vec3 v100 = {worldMax.x, worldMin.y, worldMin.z};
   glm::vec3 v010 = {worldMin.x, worldMax.y, worldMin.z};
   glm::vec3 v110 = {worldMax.x, worldMax.y, worldMin.z};
   glm::vec3 v001 = {worldMin.x, worldMin.y, worldMax.z};
   glm::vec3 v101 = {worldMax.x, worldMin.y, worldMax.z};
   glm::vec3 v011 = {worldMin.x, worldMax.y, worldMax.z};
   glm::vec3 v111 = {worldMax.x, worldMax.y, worldMax.z};

   std::vector<GridVertex> lines = {
      {v000.x, v000.y, v000.z}, {v100.x, v100.y, v100.z},
      {v100.x, v100.y, v100.z}, {v110.x, v110.y, v110.z},
      {v110.x, v110.y, v110.z}, {v010.x, v010.y, v010.z},
      {v010.x, v010.y, v010.z}, {v000.x, v000.y, v000.z},

      {v001.x, v001.y, v001.z}, {v101.x, v101.y, v101.z},
      {v101.x, v101.y, v101.z}, {v111.x, v111.y, v111.z},
      {v111.x, v111.y, v111.z}, {v011.x, v011.y, v011.z},
      {v011.x, v011.y, v011.z}, {v001.x, v001.y, v001.z},

      {v000.x, v000.y, v000.z}, {v001.x, v001.y, v001.z},
      {v100.x, v100.y, v100.z}, {v101.x, v101.y, v101.z},
      {v110.x, v110.y, v110.z}, {v111.x, v111.y, v111.z},
      {v010.x, v010.y, v010.z}, {v011.x, v011.y, v011.z},
   };

   const bgfx::Memory* mem = bgfx::copy(lines.data(), (uint32_t)(lines.size() * sizeof(GridVertex)));
   bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(mem, GridVertex::layout);
   float identity[16]; bx::mtxIdentity(identity);
   bgfx::setTransform(identity);
   bgfx::setVertexBuffer(0, vbh);

   if (m_CachedDebugMaterial) {
      m_CachedDebugMaterial->BindUniforms();
      ApplyDefaultDebugLineColor();
      bgfx::setState(
         BGFX_STATE_WRITE_RGB |
         BGFX_STATE_DEPTH_TEST_LEQUAL |
         BGFX_STATE_PT_LINES |
         BGFX_STATE_BLEND_ALPHA
      );
      if (bgfx::isValid(m_CachedDebugMaterial->GetProgram())) {
         bgfx::submit(viewId, m_CachedDebugMaterial->GetProgram());
      }
   }
   bgfx::destroy(vbh);
}
#endif // CLAYMORE_RUNTIME - DrawAABB is editor-only

// These utility functions are needed for both editor and runtime (debug line drawing)
void Renderer::ApplyDebugLineColor(const glm::vec4& color) const
{
   if (bgfx::isValid(u_DebugColor)) {
      bgfx::setUniform(u_DebugColor, glm::value_ptr(color));
   }
}

void Renderer::ApplyDefaultDebugLineColor() const
{
   ApplyDebugLineColor(m_DefaultDebugLineColor);
}

#ifndef CLAYMORE_RUNTIME
void Renderer::DrawAreaCollider(const cm::physics::AreaComponent & area, const TransformComponent & transform, uint32_t abgrColor)
{
   if (!bgfx::isValid(m_DebugLineProgram)) return;

   glm::mat4 world = transform.WorldMatrix * glm::translate(glm::mat4(1.0f), area.Offset);
   float tm[16];
   memcpy(tm, glm::value_ptr(world), sizeof(float) * 16);
   bgfx::setTransform(tm);

   std::vector<PosColorVertex> lines;
   lines.reserve(96);
   
   auto pushLine = [&](const glm::vec3& a, const glm::vec3& b)
   {
      lines.push_back({ a.x, a.y, a.z, abgrColor });
      lines.push_back({ b.x, b.y, b.z, abgrColor });
   };

   switch (area.ShapeType)
   {
      case cm::physics::AreaShapeType::Box:
      {
         glm::vec3 halfExtents = glm::max(glm::abs(area.Size) * 0.5f, glm::vec3(0.0f));
         glm::vec3 he = halfExtents;
         glm::vec3 corners[8] = {
            {-he.x,-he.y,-he.z}, { he.x,-he.y,-he.z}, { he.x, he.y,-he.z}, {-he.x, he.y,-he.z},
            {-he.x,-he.y, he.z}, { he.x,-he.y, he.z}, { he.x, he.y, he.z}, {-he.x, he.y, he.z}
         };

         // bottom rectangle
         pushLine(corners[0], corners[1]);
         pushLine(corners[1], corners[2]);
         pushLine(corners[2], corners[3]);
         pushLine(corners[3], corners[0]);
         // top rectangle
         pushLine(corners[4], corners[5]);
         pushLine(corners[5], corners[6]);
         pushLine(corners[6], corners[7]);
         pushLine(corners[7], corners[4]);
         // vertical edges
         pushLine(corners[0], corners[4]);
         pushLine(corners[1], corners[5]);
         pushLine(corners[2], corners[6]);
         pushLine(corners[3], corners[7]);
         break;
      }
      case cm::physics::AreaShapeType::Capsule:
      {
         float radius = std::max(area.Radius, 0.0f);
         float halfHeight = std::max(area.Height * 0.5f, 0.0f);
         const int segments = 24;
         for (int i = 0; i < segments; ++i)
         {
            float a0 = (float)i / segments * glm::two_pi<float>();
            float a1 = (float)(i + 1) / segments * glm::two_pi<float>();
            float c0 = std::cos(a0), s0 = std::sin(a0);
            float c1 = std::cos(a1), s1 = std::sin(a1);
            glm::vec3 top0(radius * c0, halfHeight, radius * s0);
            glm::vec3 top1(radius * c1, halfHeight, radius * s1);
            glm::vec3 bottom0(radius * c0, -halfHeight, radius * s0);
            glm::vec3 bottom1(radius * c1, -halfHeight, radius * s1);
            pushLine(top0, top1);
            pushLine(bottom0, bottom1);
            pushLine(top0, bottom0);
         }

         if (radius > 0.0f)
         {
            const int hemiSegments = 12;
            auto addHemisphere = [&](bool top)
            {
               float baseY = top ? halfHeight : -halfHeight;
               float dir = top ? 1.0f : -1.0f;
               for (int i = 0; i < hemiSegments; ++i)
               {
                  float t0 = (float)i / hemiSegments * glm::half_pi<float>();
                  float t1 = (float)(i + 1) / hemiSegments * glm::half_pi<float>();
                  float ct0 = std::cos(t0), st0 = std::sin(t0);
                  float ct1 = std::cos(t1), st1 = std::sin(t1);

                  glm::vec3 xy0(radius * ct0, baseY + dir * radius * st0, 0.0f);
                  glm::vec3 xy1(radius * ct1, baseY + dir * radius * st1, 0.0f);
                  pushLine(xy0, xy1);
                  pushLine(glm::vec3(-xy0.x, xy0.y, 0.0f), glm::vec3(-xy1.x, xy1.y, 0.0f));

                  glm::vec3 zy0(0.0f, baseY + dir * radius * st0, radius * ct0);
                  glm::vec3 zy1(0.0f, baseY + dir * radius * st1, radius * ct1);
                  pushLine(zy0, zy1);
                  pushLine(glm::vec3(0.0f, zy0.y, -zy0.z), glm::vec3(0.0f, zy1.y, -zy1.z));
               }
            };
            addHemisphere(true);
            addHemisphere(false);
         }
         break;
      }
      case cm::physics::AreaShapeType::Sphere:
      {
         float radius = std::max(area.Radius, 0.0f);
         if (radius <= 0.0f) break;
         const int segments = 32;
         for (int i = 0; i < segments; ++i)
         {
            float a0 = (float)i / segments * glm::two_pi<float>();
            float a1 = (float)(i + 1) / segments * glm::two_pi<float>();
            float c0 = std::cos(a0), s0 = std::sin(a0);
            float c1 = std::cos(a1), s1 = std::sin(a1);
            pushLine({ radius * c0, radius * s0, 0.0f }, { radius * c1, radius * s1, 0.0f });
            pushLine({ radius * c0, 0.0f, radius * s0 }, { radius * c1, 0.0f, radius * s1 });
            pushLine({ 0.0f, radius * c0, radius * s0 }, { 0.0f, radius * c1, radius * s1 });
         }
         break;
      }
   }

   if (lines.empty()) return;
   PosColorVertex::Init();
   const bgfx::Memory* mem = bgfx::copy(lines.data(), (uint32_t)(lines.size() * sizeof(PosColorVertex)));
   bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(mem, PosColorVertex::layout);
   bgfx::setVertexBuffer(0, vbh);
   bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_PT_LINES | BGFX_STATE_BLEND_ALPHA | BGFX_STATE_LINEAA);
   
   // Convert ABGR to RGBA for the uniform (shader uses uniform color, not vertex color)
   float a = ((abgrColor >> 24) & 0xFF) / 255.0f;
   float b = ((abgrColor >> 16) & 0xFF) / 255.0f;
   float g = ((abgrColor >> 8) & 0xFF) / 255.0f;
   float r = (abgrColor & 0xFF) / 255.0f;
   ApplyDebugLineColor(glm::vec4(r, g, b, a));
   
   bgfx::submit(kDebugOverlayViewId, m_DebugLineProgram);
   bgfx::destroy(vbh);
}

void Renderer::DrawRing(const glm::vec3& center, const glm::vec3& normal, float radius, uint32_t abgrColor)
{
   if (!bgfx::isValid(m_DebugLineProgram)) return;
   glm::vec3 n = glm::length2(normal) > 1e-4f ? glm::normalize(normal) : glm::vec3(0.0f, 1.0f, 0.0f);
   glm::vec3 reference = std::abs(n.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
   glm::vec3 tangent = glm::normalize(glm::cross(reference, n));
   glm::vec3 bitangent = glm::normalize(glm::cross(n, tangent));
   // Offset significantly above the surface to ensure visibility (same as camera frustum style)
   const float hoverOffset = 0.1f;
   const glm::vec3 ringCenter = center + n * hoverOffset;

   const int segments = 64;
   std::vector<PosColorVertex> verts;
   verts.reserve(segments * 2);
   for (int i = 0; i < segments; ++i)
   {
      float a0 = (float)i / segments * glm::two_pi<float>();
      float a1 = (float)(i + 1) / segments * glm::two_pi<float>();
      glm::vec3 p0 = ringCenter + (tangent * std::cos(a0) + bitangent * std::sin(a0)) * radius;
      glm::vec3 p1 = ringCenter + (tangent * std::cos(a1) + bitangent * std::sin(a1)) * radius;
      verts.push_back({ p0.x, p0.y, p0.z, abgrColor });
      verts.push_back({ p1.x, p1.y, p1.z, abgrColor });
   }

   if (verts.empty()) return;
   PosColorVertex::Init();
   const bgfx::Memory* mem = bgfx::copy(verts.data(), uint32_t(verts.size() * sizeof(PosColorVertex)));
   bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(mem, PosColorVertex::layout);
   float identity[16]; bx::mtxIdentity(identity);
   bgfx::setTransform(identity);
   bgfx::setVertexBuffer(0, vbh);
   bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_PT_LINES | BGFX_STATE_BLEND_ALPHA | BGFX_STATE_LINEAA);
   // Use the same bright color as camera frustum for better visibility
   glm::vec4 frustumColor(0.95f, 0.95f, 1.0f, 0.85f);
   ApplyDebugLineColor(frustumColor);
   bgfx::submit(kDebugOverlayViewId, m_DebugLineProgram);
   ApplyDefaultDebugLineColor();
   bgfx::destroy(vbh);
}

void Renderer::DrawFilledCircle(const glm::vec3& center, const glm::vec3& normal, float radius, uint32_t abgrColor)
{
   if (!bgfx::isValid(m_DebugLineProgram)) return;
   glm::vec3 n = glm::length2(normal) > 1e-4f ? glm::normalize(normal) : glm::vec3(0.0f, 1.0f, 0.0f);
   glm::vec3 reference = std::abs(n.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
   glm::vec3 tangent = glm::normalize(glm::cross(reference, n));
   glm::vec3 bitangent = glm::normalize(glm::cross(n, tangent));
   const float hoverOffset = 0.05f;
   const glm::vec3 circleCenter = center + n * hoverOffset;

   const int segments = 32;
   std::vector<PosColorVertex> verts;
   verts.reserve(segments * 3);
   for (int i = 0; i < segments; ++i) {
      float a0 = (float)i / segments * glm::two_pi<float>();
      float a1 = (float)(i + 1) / segments * glm::two_pi<float>();
      glm::vec3 p0 = circleCenter + (tangent * std::cos(a0) + bitangent * std::sin(a0)) * radius;
      glm::vec3 p1 = circleCenter + (tangent * std::cos(a1) + bitangent * std::sin(a1)) * radius;
      verts.push_back({ circleCenter.x, circleCenter.y, circleCenter.z, abgrColor });
      verts.push_back({ p0.x, p0.y, p0.z, abgrColor });
      verts.push_back({ p1.x, p1.y, p1.z, abgrColor });
   }

   if (verts.empty()) return;
   PosColorVertex::Init();
   const bgfx::Memory* mem = bgfx::copy(verts.data(), uint32_t(verts.size() * sizeof(PosColorVertex)));
   bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(mem, PosColorVertex::layout);
   float identity[16]; bx::mtxIdentity(identity);
   bgfx::setTransform(identity);
   bgfx::setVertexBuffer(0, vbh);
   bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_BLEND_ALPHA);
   float r = ((abgrColor) & 0xFF) / 255.0f;
   float g = ((abgrColor >> 8) & 0xFF) / 255.0f;
   float b = ((abgrColor >> 16) & 0xFF) / 255.0f;
   float a = ((abgrColor >> 24) & 0xFF) / 255.0f;
   ApplyDebugLineColor(glm::vec4(r, g, b, a));
   bgfx::submit(kDebugOverlayViewId, m_DebugLineProgram);
   ApplyDefaultDebugLineColor();
   bgfx::destroy(vbh);
}

void Renderer::DrawCameraFrustum(const CameraComponent& camera, const TransformComponent& transform)
{
   if (!bgfx::isValid(m_DebugLineProgram)) return;

   constexpr float kMaxDebugFrustumDepth = 5000.0f;
   float nearClip = glm::max(0.001f, camera.NearClip);
   float farClip = camera.FarClip;
   if (!std::isfinite(farClip) || farClip <= nearClip) {
      farClip = nearClip + 0.1f;
   }
   float clampedFar = glm::min(farClip, nearClip + kMaxDebugFrustumDepth);
   clampedFar = glm::max(clampedFar, nearClip + 0.01f);

   auto isMatrixFinite = [](const glm::mat4& m) {
      for (int i = 0; i < 4; ++i) {
         if (!glm::all(glm::isfinite(glm::vec4(m[i])))) {
            return false;
         }
      }
      return true;
   };

   glm::mat4 cameraWorld = transform.WorldMatrix;
   if (!isMatrixFinite(cameraWorld)) {
      cameraWorld = glm::inverse(camera.Camera.GetViewMatrix());
   }
   if (!isMatrixFinite(cameraWorld)) {
      cameraWorld = glm::mat4(1.0f);
      cameraWorld[3] = glm::vec4(transform.Position, 1.0f);
   }

   auto safeNormal = [](const glm::vec3& v, const glm::vec3& fallback) {
      if (!glm::all(glm::isfinite(v))) return fallback;
      float len = glm::length(v);
      if (len < 1e-4f) return fallback;
      return v / len;
   };

   glm::vec3 position = glm::vec3(cameraWorld[3]);
   glm::vec3 right = safeNormal(glm::vec3(cameraWorld[0]), glm::vec3(1.0f, 0.0f, 0.0f));
   glm::vec3 up = safeNormal(glm::vec3(cameraWorld[1]), glm::vec3(0.0f, 1.0f, 0.0f));
   glm::vec3 forward = safeNormal(-glm::vec3(cameraWorld[2]), glm::vec3(0.0f, 0.0f, -1.0f));

   float nearHalfWidth = 0.1f;
   float nearHalfHeight = 0.1f;
   float farHalfWidth = 0.1f;
   float farHalfHeight = 0.1f;
   glm::mat4 proj = camera.Camera.GetProjectionMatrix();

   if (camera.IsPerspective) {
      float aspect = (m_Height > 0) ? (float)m_Width / (float)m_Height : 1.0f;
      float fov = camera.FieldOfView;
      if (!std::isfinite(fov)) fov = 60.0f;
      fov = glm::clamp(fov, 1.0f, 179.0f);
      float tanHalfFov = glm::tan(glm::radians(fov * 0.5f));
      if (!std::isfinite(tanHalfFov) || tanHalfFov < 1e-6f) {
         float projY = proj[1][1];
         tanHalfFov = (fabsf(projY) > 1e-6f) ? (1.0f / projY) : 0.0f;
      }
      if (!std::isfinite(tanHalfFov) || tanHalfFov < 1e-6f) tanHalfFov = glm::tan(glm::radians(30.0f));
      nearHalfHeight = nearClip * tanHalfFov;
      nearHalfWidth = nearHalfHeight * aspect;
      farHalfHeight = clampedFar * tanHalfFov;
      farHalfWidth = farHalfHeight * aspect;
   }
   else {
      float scaleX = proj[0][0];
      float scaleY = proj[1][1];
      float width = (fabsf(scaleX) > 1e-6f) ? (2.0f / scaleX) : 10.0f;
      float height = (fabsf(scaleY) > 1e-6f) ? (2.0f / scaleY) : 10.0f;
      nearHalfWidth = farHalfWidth = fabsf(width) * 0.5f;
      nearHalfHeight = farHalfHeight = fabsf(height) * 0.5f;
   }

   glm::vec3 nearCenter = position + forward * nearClip;
   glm::vec3 farCenter = position + forward * clampedFar;

   std::array<glm::vec3, 8> corners;
   enum Corner {
      NBL = 0, NBR, NTR, NTL,
      FBL, FBR, FTR, FTL
   };

   corners[NBL] = nearCenter - right * nearHalfWidth - up * nearHalfHeight;
   corners[NBR] = nearCenter + right * nearHalfWidth - up * nearHalfHeight;
   corners[NTR] = nearCenter + right * nearHalfWidth + up * nearHalfHeight;
   corners[NTL] = nearCenter - right * nearHalfWidth + up * nearHalfHeight;

   corners[FBL] = farCenter - right * farHalfWidth - up * farHalfHeight;
   corners[FBR] = farCenter + right * farHalfWidth - up * farHalfHeight;
   corners[FTR] = farCenter + right * farHalfWidth + up * farHalfHeight;
   corners[FTL] = farCenter - right * farHalfWidth + up * farHalfHeight;

   std::vector<GridVertex> lines;
   lines.reserve(28);
   auto pushEdge = [&](Corner a, Corner b) {
      lines.push_back({ corners[a].x, corners[a].y, corners[a].z });
      lines.push_back({ corners[b].x, corners[b].y, corners[b].z });
   };

   // Near face
   pushEdge(NBL, NBR);
   pushEdge(NBR, NTR);
   pushEdge(NTR, NTL);
   pushEdge(NTL, NBL);

   // Far face
   pushEdge(FBL, FBR);
   pushEdge(FBR, FTR);
   pushEdge(FTR, FTL);
   pushEdge(FTL, FBL);

   // Connect near/far
   pushEdge(NBL, FBL);
   pushEdge(NBR, FBR);
   pushEdge(NTR, FTR);
   pushEdge(NTL, FTL);

   if (lines.empty()) return;

   const uint32_t vertexCount = (uint32_t)lines.size();
   if (bgfx::getAvailTransientVertexBuffer(vertexCount, GridVertex::layout) < vertexCount) {
      // Not enough space in the transient buffer this frame; skip the draw instead of stalling.
      return;
      }

   bgfx::TransientVertexBuffer tvb;
   bgfx::allocTransientVertexBuffer(&tvb, vertexCount, GridVertex::layout);
   memcpy(tvb.data, lines.data(), vertexCount * sizeof(GridVertex));

   float identity[16]; bx::mtxIdentity(identity);
   bgfx::setTransform(identity);
   bgfx::setVertexBuffer(0, &tvb);
   glm::vec4 frustumColor(0.95f, 0.95f, 1.0f, 0.85f);
   ApplyDebugLineColor(frustumColor);
   bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_PT_LINES | BGFX_STATE_BLEND_ALPHA | BGFX_STATE_LINEAA);
   bgfx::submit(kDebugOverlayViewId, m_DebugLineProgram);
   ApplyDefaultDebugLineColor();
}

// --------------------------------------
// Draw simple wireframe outline around selected entity's mesh (editor only)
// --------------------------------------
void Renderer::RenderObjectIdScene(Scene& scene, uint16_t viewId) {
   if (!bgfx::isValid(m_ObjectIdFB)) {
      Resize(m_Width, m_Height);
   }
   if (!bgfx::isValid(m_ObjectIdFB)) {
      return;
   }

   bgfx::setViewRect(viewId, 0, 0, (uint16_t)m_Width, (uint16_t)m_Height);
   bgfx::setViewTransform(viewId, m_view, m_proj);
   bgfx::setViewFrameBuffer(viewId, m_ObjectIdFB);
   bgfx::setViewClear(viewId, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x00000000, 1.0f, 0);
   bgfx::touch(viewId);

   m_ScratchVisibleMeshIds.clear();
   m_ScratchVisibleMeshIds.reserve(scene.GetEntities().size());
   for (const auto& e : scene.GetEntities()) {
      auto* data = scene.GetEntityData(e.GetID());
      if (!IsPresentationVisible(data) || !data->Active || !data->Mesh || !data->Mesh->mesh) {
         continue;
      }
      m_ScratchVisibleMeshIds.push_back(e.GetID());
   }
   Profiler::Get().SetCounter(
      "Render/ObjectIdVisibleCandidates",
      static_cast<uint64_t>(m_ScratchVisibleMeshIds.size()));
   PrepareGpuSkinningAtlases(scene, m_ScratchVisibleMeshIds);
   PrepareGpuMaterializedSkinnedMeshes(scene, m_ScratchVisibleMeshIds);

   Renderer::SkinningBindCacheState skinningCache{};
   uint64_t objectIdSkinnedBatchedInstances = 0;
   auto resolveObjectIdSkinnedPath = [&](EntityData* data,
                                         EntityID entityId,
                                         Mesh* mesh,
                                         bgfx::VertexBufferHandle& outVertexBuffer,
                                         bgfx::ProgramHandle& outInstancedProgram,
                                         bgfx::ProgramHandle& outSingleProgram,
                                         bool& outUsesGpuMorph,
                                         bool& outUsesMaterializedSkinning) -> bool {
      outVertexBuffer = BGFX_INVALID_HANDLE;
      outInstancedProgram = BGFX_INVALID_HANDLE;
      outSingleProgram = BGFX_INVALID_HANDLE;
      outUsesGpuMorph = false;
      outUsesMaterializedSkinning = false;

      if (!data || !data->Skinning || !mesh || !bgfx::isValid(mesh->ibh)) {
         return false;
      }

      if (TryGetGpuMaterializedSkinnedVertexBuffer(scene, entityId, mesh, outVertexBuffer) &&
          bgfx::isValid(m_ObjectIdProgram)) {
         outSingleProgram = m_ObjectIdProgram;
         outUsesMaterializedSkinning = true;
         return bgfx::isValid(outVertexBuffer) && bgfx::isValid(outSingleProgram);
      }

      if (mesh->Dynamic) {
         if (!data->Skinning->HasGpuSkinningInstanceRecord()) {
            return false;
         }
         if (ShouldUseGpuMorphTargets(data->Skinning.get(), mesh, data->BlendShapes.get())) {
            outVertexBuffer = GetOrCreateGpuMorphVertexBuffer(mesh);
            outInstancedProgram = m_ObjectIdProgramSkinnedMorphInstanced;
            outSingleProgram = m_ObjectIdProgramSkinnedMorph;
            outUsesGpuMorph = true;
            return bgfx::isValid(outVertexBuffer) &&
               (bgfx::isValid(outInstancedProgram) || bgfx::isValid(outSingleProgram));
         }

         return false;
      }

      outVertexBuffer = mesh->vbh;
      outInstancedProgram = m_ObjectIdProgramSkinnedInstanced;
      outSingleProgram = m_ObjectIdProgramSkinned;
      return bgfx::isValid(outVertexBuffer) &&
         (bgfx::isValid(outInstancedProgram) || bgfx::isValid(outSingleProgram));
   };
   for (EntityID entityId : m_ScratchVisibleMeshIds) {
      auto* data = scene.GetEntityData(entityId);
      if (!IsPresentationVisible(data) || !data->Active || !data->Mesh || !data->Mesh->mesh) continue;
      std::shared_ptr<Mesh> meshPtr = data->Mesh->mesh;
      if (!HasRenderableVertexSource(*this, data, meshPtr.get())) continue;

      const bool isSkinned = UsesSkinnedObjectIdProgram(data);
      constexpr uint64_t objectIdState =
         BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LEQUAL;
      float transform[16];
      memcpy(transform, glm::value_ptr(data->Transform.WorldMatrix), sizeof(float) * 16);
      bgfx::ProgramHandle objectIdProgram = isSkinned ? m_ObjectIdProgramSkinned : m_ObjectIdProgram;
      bool usesGpuMorph = false;
      bool usesMaterializedSkinning = false;
      if (isSkinned && data->Skinning) {
         bgfx::VertexBufferHandle resolvedVbh = BGFX_INVALID_HANDLE;
         bgfx::ProgramHandle instancedProgram = BGFX_INVALID_HANDLE;
         if (resolveObjectIdSkinnedPath(
                data,
                entityId,
                meshPtr.get(),
                resolvedVbh,
                instancedProgram,
                objectIdProgram,
                usesGpuMorph,
                usesMaterializedSkinning)) {
            if (data->Skinning->HasGpuSkinningInstanceRecord() && bgfx::isValid(instancedProgram)) {
               cm::rendering::SkinnedInstanceData instance{};
               instance.SetTransform(transform);
               instance.SetMetadata(static_cast<float>(data->Skinning->GpuInstanceAtlasRecordIndex));

               const char* instancedDebugName = usesGpuMorph
                  ? "vs_pbr_skinned_morph_object_id_instanced+fs_object_id_instanced"
                  : "vs_pbr_skinned_object_id_instanced+fs_object_id_instanced";
               if (SubmitSingleSkinnedInstance(
                      viewId,
                      resolvedVbh,
                      meshPtr->ibh,
                      instancedProgram,
                      instance,
                      objectIdState,
                      instancedDebugName,
                      [this]() { BindGpuSkinningAtlasGlobals(); })) {
                  ++objectIdSkinnedBatchedInstances;
                  continue;
               }
            }
            bgfx::setVertexBuffer(0, resolvedVbh);
         } else if (meshPtr->Dynamic) {
            if (!bgfx::isValid(meshPtr->dvbh)) continue;
            bgfx::setVertexBuffer(0, meshPtr->dvbh, 0, meshPtr->numVertices);
         } else {
            bgfx::setVertexBuffer(0, meshPtr->vbh);
         }
      } else if (meshPtr->Dynamic) {
         if (!bgfx::isValid(meshPtr->dvbh)) continue;
         bgfx::setVertexBuffer(0, meshPtr->dvbh, 0, meshPtr->numVertices);
      } else {
         bgfx::setVertexBuffer(0, meshPtr->vbh);
      }
      bgfx::setTransform(transform);
      bgfx::setIndexBuffer(meshPtr->ibh);
      bgfx::setState(objectIdState);

      const uint32_t id = static_cast<uint32_t>(entityId) + 1u;
      const glm::vec4 packed(
         (float)(id & 255u) / 255.0f,
         (float)((id >> 8) & 255u) / 255.0f,
         (float)((id >> 16) & 255u) / 255.0f,
         0.0f);
      bgfx::setUniform(u_ObjectIdPacked, &packed);

      if (isSkinned && data->Skinning) {
         if (usesGpuMorph) {
            BindSkinningInstanceRecord(data->Skinning->GpuInstanceAtlasRecordIndex);
            skinningCache = {};
         } else if (!usesMaterializedSkinning) {
            BindSkinningIfChanged(data->Skinning.get(), skinningCache);
         }
      }

      const char* debugName = usesMaterializedSkinning
         ? "vs_pbr+fs_object_id"
         : (usesGpuMorph
         ? "vs_pbr_skinned_morph+fs_object_id"
         : (isSkinned ? "vs_pbr_skinned+fs_object_id" : "vs_pbr+fs_object_id"));
      SafeSubmit(viewId, objectIdProgram, debugName);
   }
   Profiler::Get().SetCounter("Render/ObjectIdSkinnedBatchedInstances", objectIdSkinnedBatchedInstances);
}

bool Renderer::SupportsObjectIdPicking() const {
   const bgfx::Caps* caps = bgfx::getCaps();
   return caps != nullptr &&
      (caps->supported & BGFX_CAPS_TEXTURE_BLIT) != 0 &&
      (caps->supported & BGFX_CAPS_TEXTURE_READ_BACK) != 0 &&
      bgfx::isValid(m_ObjectIdReadbackTex) &&
      bgfx::isValid(m_ObjectIdTex);
}

bool Renderer::RequestObjectIdPick(Scene& scene, float nx, float ny) {
   if (m_ObjectIdPickPending || m_Width == 0 || m_Height == 0) {
      return false;
   }

   const bgfx::Caps* caps = bgfx::getCaps();
   if (!SupportsObjectIdPicking()) {
      static bool s_ObjectIdPickUnsupportedWarned = false;
      if (!s_ObjectIdPickUnsupportedWarned) {
         s_ObjectIdPickUnsupportedWarned = true;
         std::cerr << "[Renderer] Object-id picking is unavailable on the current renderer/backend. Falling back to CPU picking." << std::endl;
      }
      return false;
   }

   const uint16_t objectIdPickView = 6;
   const uint16_t readbackBlitView = 7;
   RenderObjectIdScene(scene, objectIdPickView);

   const float clampedNx = glm::clamp(nx, 0.0f, 0.999999f);
   const float clampedNy = glm::clamp(ny, 0.0f, 0.999999f);
   const float sampleNy = caps->originBottomLeft ? (1.0f - clampedNy) : clampedNy;
   const uint16_t pixelX = static_cast<uint16_t>(clampedNx * float(m_Width));
   const uint16_t pixelY = static_cast<uint16_t>(glm::clamp(sampleNy, 0.0f, 0.999999f) * float(m_Height));
   bgfx::setViewRect(readbackBlitView, 0, 0, 1, 1);
   bgfx::touch(readbackBlitView);
   bgfx::blit(readbackBlitView, m_ObjectIdReadbackTex, 0, 0, m_ObjectIdTex, pixelX, pixelY, 1, 1);

   const uint32_t pendingFrame = bgfx::readTexture(m_ObjectIdReadbackTex, m_ObjectIdPickReadbackBuffer.data());
   if (pendingFrame == std::numeric_limits<uint32_t>::max()) {
      return false;
   }

   m_ObjectIdPickPending = true;
   m_ObjectIdPickPendingFrame = pendingFrame;
   return true;
}

bool Renderer::ConsumeObjectIdPickResult(EntityID& outId, bool& outHadHit) {
   if (m_ObjectIdPickPending && m_ObjectIdPickPendingFrame <= GetLastSubmittedFrame()) {
      const uint32_t packedId =
         uint32_t(m_ObjectIdPickReadbackBuffer[0]) |
         (uint32_t(m_ObjectIdPickReadbackBuffer[1]) << 8u) |
         (uint32_t(m_ObjectIdPickReadbackBuffer[2]) << 16u);
      m_ObjectIdPickResult = (packedId == 0u) ? INVALID_ENTITY_ID : static_cast<EntityID>(packedId - 1u);
      m_ObjectIdPickResultHadHit = packedId != 0u;
      m_ObjectIdPickResultReady = true;
      m_ObjectIdPickPending = false;
      m_ObjectIdPickPendingFrame = 0;
   }

   if (!m_ObjectIdPickResultReady) {
      return false;
   }

   outId = m_ObjectIdPickResult;
   outHadHit = m_ObjectIdPickResultHadHit;
   m_ObjectIdPickResultReady = false;
   return true;
}

void Renderer::DrawEntityOutline(Scene & scene, EntityID selectedEntity) {
   if (selectedEntity < 0 || scene.m_IsPlaying) return;
   auto* data = scene.GetEntityData(selectedEntity);
   if (!IsPresentationVisible(data) || !data->Active || !data->Mesh || !data->Mesh->mesh) return;

   const uint16_t kView_ObjectId = 210;
   const uint16_t kView_OutlineEdge = 211;
   const uint16_t kView_OutlineComposite = 212;

   if (!bgfx::isValid(m_ObjectIdFB) || !bgfx::isValid(m_EdgeMaskFB)) {
      Resize(m_Width, m_Height);
   }
   if (!bgfx::isValid(m_ObjectIdFB) || !bgfx::isValid(m_EdgeMaskFB)) {
      return;
   }

   {
      bgfx::setViewRect(kView_ObjectId, 0, 0, (uint16_t)m_Width, (uint16_t)m_Height);
      bgfx::setViewTransform(kView_ObjectId, m_view, m_proj);
      bgfx::setViewFrameBuffer(kView_ObjectId, m_ObjectIdFB);
      bgfx::setViewClear(kView_ObjectId, BGFX_CLEAR_COLOR, 0x00000000, 1.0f, 0);
      bgfx::touch(kView_ObjectId);

      const std::vector<EntityID> outlinedMeshIds{ selectedEntity };
      PrepareGpuSkinningAtlases(scene, outlinedMeshIds);
      PrepareGpuMaterializedSkinnedMeshes(scene, outlinedMeshIds);

      std::shared_ptr<Mesh> meshPtr = data->Mesh->mesh;
      if (HasRenderableVertexSource(*this, data, meshPtr.get())) {
         auto resolveObjectIdSkinnedPath = [&](Mesh* mesh,
                                               bgfx::VertexBufferHandle& outVertexBuffer,
                                               bgfx::ProgramHandle& outInstancedProgram,
                                               bgfx::ProgramHandle& outProgram,
                                               bool& outUsesGpuMorph,
                                               bool& outUsesMaterializedSkinning) -> bool {
            outVertexBuffer = BGFX_INVALID_HANDLE;
            outInstancedProgram = BGFX_INVALID_HANDLE;
            outProgram = BGFX_INVALID_HANDLE;
            outUsesGpuMorph = false;
            outUsesMaterializedSkinning = false;

            if (!data->Skinning || !mesh || !bgfx::isValid(mesh->ibh)) {
               return false;
            }

            if (TryGetGpuMaterializedSkinnedVertexBuffer(scene, selectedEntity, mesh, outVertexBuffer) &&
                bgfx::isValid(m_ObjectIdProgram)) {
               outProgram = m_ObjectIdProgram;
               outUsesMaterializedSkinning = true;
               return bgfx::isValid(outVertexBuffer) && bgfx::isValid(outProgram);
            }

            if (mesh->Dynamic) {
               if (!data->Skinning->HasGpuSkinningInstanceRecord()) {
                  return false;
               }
               if (ShouldUseGpuMorphTargets(data->Skinning.get(), mesh, data->BlendShapes.get())) {
                  outVertexBuffer = GetOrCreateGpuMorphVertexBuffer(mesh);
                  outInstancedProgram = m_ObjectIdProgramSkinnedMorphInstanced;
                  outProgram = m_ObjectIdProgramSkinnedMorph;
                  outUsesGpuMorph = true;
                  return bgfx::isValid(outVertexBuffer) &&
                     (bgfx::isValid(outInstancedProgram) || bgfx::isValid(outProgram));
               }

               return false;
            }

            outVertexBuffer = mesh->vbh;
            outInstancedProgram = m_ObjectIdProgramSkinnedInstanced;
            outProgram = m_ObjectIdProgramSkinned;
            return bgfx::isValid(outVertexBuffer) &&
               (bgfx::isValid(outInstancedProgram) || bgfx::isValid(outProgram));
         };

         const bool isSkinned = UsesSkinnedObjectIdProgram(data);
         bgfx::ProgramHandle objectIdProgram = isSkinned ? m_ObjectIdProgramSkinned : m_ObjectIdProgram;
         bool usesGpuMorph = false;
         bool usesMaterializedSkinning = false;
         bool submittedInstanced = false;
         if (isSkinned && data->Skinning) {
            bgfx::VertexBufferHandle resolvedVbh = BGFX_INVALID_HANDLE;
            bgfx::ProgramHandle instancedProgram = BGFX_INVALID_HANDLE;
            if (resolveObjectIdSkinnedPath(
                   meshPtr.get(),
                   resolvedVbh,
                   instancedProgram,
                   objectIdProgram,
                   usesGpuMorph,
                   usesMaterializedSkinning)) {
               if (data->Skinning->HasGpuSkinningInstanceRecord() && bgfx::isValid(instancedProgram)) {
                  cm::rendering::SkinnedInstanceData instance{};
                  instance.SetTransform(data->Transform.WorldMatrix);
                  instance.SetMetadata(static_cast<float>(data->Skinning->GpuInstanceAtlasRecordIndex));

                  const char* instancedDebugName = usesGpuMorph
                     ? "vs_pbr_skinned_morph_object_id_instanced+fs_object_id_instanced"
                     : "vs_pbr_skinned_object_id_instanced+fs_object_id_instanced";
                  submittedInstanced = SubmitSingleSkinnedInstance(
                     kView_ObjectId,
                     resolvedVbh,
                     meshPtr->ibh,
                     instancedProgram,
                     instance,
                     BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_LEQUAL,
                     instancedDebugName,
                     [this]() { BindGpuSkinningAtlasGlobals(); });
               }
               if (!submittedInstanced) {
                  bgfx::setVertexBuffer(0, resolvedVbh);
               }
            } else if (meshPtr->Dynamic) {
               if (!bgfx::isValid(meshPtr->dvbh)) {
                  return;
               }
               bgfx::setVertexBuffer(0, meshPtr->dvbh, 0, meshPtr->numVertices);
            } else {
               bgfx::setVertexBuffer(0, meshPtr->vbh);
            }
         } else if (meshPtr->Dynamic) {
            if (!bgfx::isValid(meshPtr->dvbh)) {
               return;
            }
            bgfx::setVertexBuffer(0, meshPtr->dvbh, 0, meshPtr->numVertices);
         } else {
            bgfx::setVertexBuffer(0, meshPtr->vbh);
         }

         if (!submittedInstanced) {
            float transform[16];
            memcpy(transform, glm::value_ptr(data->Transform.WorldMatrix), sizeof(float) * 16);
            bgfx::setTransform(transform);
            bgfx::setIndexBuffer(meshPtr->ibh);
            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_LEQUAL);

            const uint32_t packedId = static_cast<uint32_t>(selectedEntity) + 1u;
            const glm::vec4 packed(
               float(packedId & 255u) / 255.0f,
               float((packedId >> 8) & 255u) / 255.0f,
               float((packedId >> 16) & 255u) / 255.0f,
               0.0f);
            bgfx::setUniform(u_ObjectIdPacked, &packed);

            if (isSkinned && data->Skinning) {
               if (usesGpuMorph) {
                  BindSkinningInstanceRecord(data->Skinning->GpuInstanceAtlasRecordIndex);
               } else if (!usesMaterializedSkinning) {
                  Renderer::SkinningBindCacheState singleSkinningCache{};
                  BindSkinningIfChanged(data->Skinning.get(), singleSkinningCache);
               }
            }

            const char* debugName = usesMaterializedSkinning
               ? "vs_pbr+fs_object_id"
               : (usesGpuMorph
               ? "vs_pbr_skinned_morph+fs_object_id"
               : (isSkinned ? "vs_pbr_skinned+fs_object_id" : "vs_pbr+fs_object_id"));
            SafeSubmit(
               kView_ObjectId,
               objectIdProgram,
               debugName);
         }
      }
   }

   {
      bgfx::setViewRect(kView_OutlineEdge, 0, 0, (uint16_t)m_Width, (uint16_t)m_Height);
      bgfx::setViewFrameBuffer(kView_OutlineEdge, m_EdgeMaskFB);
      bgfx::setViewClear(kView_OutlineEdge, BGFX_CLEAR_COLOR, 0x00000000, 1.0f, 0);
      float idM[16]; bx::mtxIdentity(idM); bgfx::setTransform(idM);
      if (bgfx::isValid(m_FullscreenVB) && bgfx::isValid(m_FullscreenIB)) {
         bgfx::setVertexBuffer(0, m_FullscreenVB); bgfx::setIndexBuffer(m_FullscreenIB);
         bgfx::setTexture(0, s_ObjectId, m_ObjectIdTex);
         glm::vec4 texelSize(1.0f / float(m_Width), 1.0f / float(m_Height), 0.0f, 0.0f);
         bgfx::setUniform(u_TexelSize, &texelSize);
         const uint32_t id = static_cast<uint32_t>(selectedEntity) + 1u;
         glm::vec4 selPacked((float)(id & 255u) / 255.0f,
                             (float)((id >> 8) & 255u) / 255.0f,
                             (float)((id >> 16) & 255u) / 255.0f,
                             0.0f);
         bgfx::setUniform(u_SelectedIdPacked, &selPacked);
         glm::vec4 edgeParams(m_OutlineThicknessPx, 0.0f, 0.0f, 0.0f);
         bgfx::setUniform(u_OutlineParams, &edgeParams);
         bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
         if (bgfx::isValid(m_OutlineEdgeProgram)) {
            bgfx::submit(kView_OutlineEdge, m_OutlineEdgeProgram);
         }
      }
   }

   {
      bgfx::setViewRect(kView_OutlineComposite, 0, 0, (uint16_t)m_Width, (uint16_t)m_Height);
      if (m_RenderToOffscreen) bgfx::setViewFrameBuffer(kView_OutlineComposite, m_SceneFrameBuffer); else bgfx::setViewFrameBuffer(kView_OutlineComposite, BGFX_INVALID_HANDLE);
      float idM[16]; bx::mtxIdentity(idM); bgfx::setTransform(idM);
      if (bgfx::isValid(m_FullscreenVB) && bgfx::isValid(m_FullscreenIB)) {
         bgfx::setVertexBuffer(0, m_FullscreenVB); bgfx::setIndexBuffer(m_FullscreenIB);
         bgfx::setTexture(0, s_EdgeMask, m_EdgeMaskTex);
         glm::vec4 texelSize(1.0f / float(m_Width), 1.0f / float(m_Height), 0.0f, 0.0f);
         bgfx::setUniform(u_TexelSize, &texelSize);
         bgfx::setUniform(u_OutlineColor, &m_OutlineColor);
         bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA);
         if (bgfx::isValid(m_OutlineCompositeProgram2)) {
            bgfx::submit(kView_OutlineComposite, m_OutlineCompositeProgram2);
         }
      }
   }
}

// --------------------------------------
// Draw outline with custom color and thickness (for hover highlights, etc.)
// --------------------------------------
void Renderer::DrawEntityOutline(Scene& scene, EntityID entityId, const glm::vec4& color, float thickness) {
   if (entityId < 0 || scene.m_IsPlaying) return;
   auto* data = scene.GetEntityData(entityId);
   if (!IsPresentationVisible(data) || !data->Active || !data->Mesh || !data->Mesh->mesh) return;

   // Reuse the same outline pipeline with custom color/thickness
   const uint16_t kView_ObjectId = 216;
   const uint16_t kView_OutlineEdge = 217;
   const uint16_t kView_OutlineComposite = 218;

   if (!bgfx::isValid(m_ObjectIdFB) || !bgfx::isValid(m_EdgeMaskFB)) {
      Resize(m_Width, m_Height);
   }
   if (!bgfx::isValid(m_ObjectIdFB) || !bgfx::isValid(m_EdgeMaskFB)) {
      return;
   }

   // 1) ObjectID pass: render only the selected entity. This preserves the
   // old editor-outline cost profile while still using the authoritative
   // skinned/GPU-morph vertex path for the selected mesh.
   {
      bgfx::setViewRect(kView_ObjectId, 0, 0, (uint16_t)m_Width, (uint16_t)m_Height);
      bgfx::setViewTransform(kView_ObjectId, m_view, m_proj);
      bgfx::setViewFrameBuffer(kView_ObjectId, m_ObjectIdFB);
      bgfx::setViewClear(kView_ObjectId, BGFX_CLEAR_COLOR, 0x00000000, 1.0f, 0);
      bgfx::touch(kView_ObjectId);

      const std::vector<EntityID> outlinedMeshIds{ entityId };
      PrepareGpuSkinningAtlases(scene, outlinedMeshIds);
      PrepareGpuMaterializedSkinnedMeshes(scene, outlinedMeshIds);

      std::shared_ptr<Mesh> meshPtr = data->Mesh->mesh;
      if (HasRenderableVertexSource(*this, data, meshPtr.get())) {
         auto resolveObjectIdSkinnedPath = [&](Mesh* mesh,
                                               bgfx::VertexBufferHandle& outVertexBuffer,
                                               bgfx::ProgramHandle& outInstancedProgram,
                                               bgfx::ProgramHandle& outProgram,
                                               bool& outUsesGpuMorph,
                                               bool& outUsesMaterializedSkinning) -> bool {
            outVertexBuffer = BGFX_INVALID_HANDLE;
            outInstancedProgram = BGFX_INVALID_HANDLE;
            outProgram = BGFX_INVALID_HANDLE;
            outUsesGpuMorph = false;
            outUsesMaterializedSkinning = false;

            if (!data->Skinning || !mesh || !bgfx::isValid(mesh->ibh)) {
               return false;
            }

            if (TryGetGpuMaterializedSkinnedVertexBuffer(scene, entityId, mesh, outVertexBuffer) &&
                bgfx::isValid(m_ObjectIdProgram)) {
               outProgram = m_ObjectIdProgram;
               outUsesMaterializedSkinning = true;
               return bgfx::isValid(outVertexBuffer) && bgfx::isValid(outProgram);
            }

            if (mesh->Dynamic) {
               if (!data->Skinning->HasGpuSkinningInstanceRecord()) {
                  return false;
               }
               if (ShouldUseGpuMorphTargets(data->Skinning.get(), mesh, data->BlendShapes.get())) {
                  outVertexBuffer = GetOrCreateGpuMorphVertexBuffer(mesh);
                  outInstancedProgram = m_ObjectIdProgramSkinnedMorphInstanced;
                  outProgram = m_ObjectIdProgramSkinnedMorph;
                  outUsesGpuMorph = true;
                  return bgfx::isValid(outVertexBuffer) &&
                     (bgfx::isValid(outInstancedProgram) || bgfx::isValid(outProgram));
               }

               return false;
            }

            outVertexBuffer = mesh->vbh;
            outInstancedProgram = m_ObjectIdProgramSkinnedInstanced;
            outProgram = m_ObjectIdProgramSkinned;
            return bgfx::isValid(outVertexBuffer) &&
               (bgfx::isValid(outInstancedProgram) || bgfx::isValid(outProgram));
         };

         const bool isSkinned = UsesSkinnedObjectIdProgram(data);
         bgfx::ProgramHandle objectIdProgram = isSkinned ? m_ObjectIdProgramSkinned : m_ObjectIdProgram;
         bool usesGpuMorph = false;
         bool usesMaterializedSkinning = false;
         bool submittedInstanced = false;
         if (isSkinned && data->Skinning) {
            bgfx::VertexBufferHandle resolvedVbh = BGFX_INVALID_HANDLE;
            bgfx::ProgramHandle instancedProgram = BGFX_INVALID_HANDLE;
            if (resolveObjectIdSkinnedPath(
                   meshPtr.get(),
                   resolvedVbh,
                   instancedProgram,
                   objectIdProgram,
                   usesGpuMorph,
                   usesMaterializedSkinning)) {
               if (data->Skinning->HasGpuSkinningInstanceRecord() && bgfx::isValid(instancedProgram)) {
                  cm::rendering::SkinnedInstanceData instance{};
                  instance.SetTransform(data->Transform.WorldMatrix);
                  instance.SetMetadata(static_cast<float>(data->Skinning->GpuInstanceAtlasRecordIndex));

                  const char* instancedDebugName = usesGpuMorph
                     ? "vs_pbr_skinned_morph_object_id_instanced+fs_object_id_instanced"
                     : "vs_pbr_skinned_object_id_instanced+fs_object_id_instanced";
                  submittedInstanced = SubmitSingleSkinnedInstance(
                     kView_ObjectId,
                     resolvedVbh,
                     meshPtr->ibh,
                     instancedProgram,
                     instance,
                     BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_LEQUAL,
                     instancedDebugName,
                     [this]() { BindGpuSkinningAtlasGlobals(); });
               }
               if (!submittedInstanced) {
                  bgfx::setVertexBuffer(0, resolvedVbh);
               }
            } else if (meshPtr->Dynamic) {
               if (!bgfx::isValid(meshPtr->dvbh)) {
                  return;
               }
               bgfx::setVertexBuffer(0, meshPtr->dvbh, 0, meshPtr->numVertices);
            } else {
               bgfx::setVertexBuffer(0, meshPtr->vbh);
            }
         } else if (meshPtr->Dynamic) {
            if (!bgfx::isValid(meshPtr->dvbh)) {
               return;
            }
            bgfx::setVertexBuffer(0, meshPtr->dvbh, 0, meshPtr->numVertices);
         } else {
            bgfx::setVertexBuffer(0, meshPtr->vbh);
         }

         if (!submittedInstanced) {
            float transform[16];
            memcpy(transform, glm::value_ptr(data->Transform.WorldMatrix), sizeof(float) * 16);
            bgfx::setTransform(transform);
            bgfx::setIndexBuffer(meshPtr->ibh);
            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_LEQUAL);

            const uint32_t packedId = static_cast<uint32_t>(entityId) + 1u;
            const glm::vec4 packed(
               float(packedId & 255u) / 255.0f,
               float((packedId >> 8) & 255u) / 255.0f,
               float((packedId >> 16) & 255u) / 255.0f,
               0.0f);
            bgfx::setUniform(u_ObjectIdPacked, &packed);

            if (isSkinned && data->Skinning) {
               if (usesGpuMorph) {
                  BindSkinningInstanceRecord(data->Skinning->GpuInstanceAtlasRecordIndex);
               } else if (!usesMaterializedSkinning) {
                  Renderer::SkinningBindCacheState singleSkinningCache{};
                  BindSkinningIfChanged(data->Skinning.get(), singleSkinningCache);
               }
            }

            const char* debugName = usesMaterializedSkinning
               ? "vs_pbr+fs_object_id"
               : (usesGpuMorph
               ? "vs_pbr_skinned_morph+fs_object_id"
               : (isSkinned ? "vs_pbr_skinned+fs_object_id" : "vs_pbr+fs_object_id"));
            SafeSubmit(
               kView_ObjectId,
               objectIdProgram,
               debugName);
         }
      }
   }

   // 2) Edge pass
   {
      bgfx::setViewRect(kView_OutlineEdge, 0, 0, (uint16_t)m_Width, (uint16_t)m_Height);
      bgfx::setViewFrameBuffer(kView_OutlineEdge, m_EdgeMaskFB);
      bgfx::setViewClear(kView_OutlineEdge, BGFX_CLEAR_COLOR, 0x00000000, 1.0f, 0);
      float idM[16]; bx::mtxIdentity(idM); bgfx::setTransform(idM);
      if (bgfx::isValid(m_FullscreenVB) && bgfx::isValid(m_FullscreenIB)) {
         bgfx::setVertexBuffer(0, m_FullscreenVB); bgfx::setIndexBuffer(m_FullscreenIB);
         bgfx::setTexture(0, s_ObjectId, m_ObjectIdTex);
         glm::vec4 texelSize(1.0f / float(m_Width), 1.0f / float(m_Height), 0.0f, 0.0f);
         bgfx::setUniform(u_TexelSize, &texelSize);
         uint32_t id = (uint32_t)entityId + 1u;
         glm::vec4 selPacked((float)(id & 255u) / 255.0f,
                             (float)((id >> 8) & 255u) / 255.0f,
                             (float)((id >> 16) & 255u) / 255.0f,
                             0.0f);
         bgfx::setUniform(u_SelectedIdPacked, &selPacked);
         glm::vec4 edgeParams(thickness, 0.0f, 0.0f, 0.0f);
         bgfx::setUniform(u_OutlineParams, &edgeParams);
         bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
         if (bgfx::isValid(m_OutlineEdgeProgram)) {
            bgfx::submit(kView_OutlineEdge, m_OutlineEdgeProgram);
         }
      }
   }

   // 3) Composite pass with custom color
   {
      bgfx::setViewRect(kView_OutlineComposite, 0, 0, (uint16_t)m_Width, (uint16_t)m_Height);
      if (m_RenderToOffscreen) bgfx::setViewFrameBuffer(kView_OutlineComposite, m_SceneFrameBuffer); else bgfx::setViewFrameBuffer(kView_OutlineComposite, BGFX_INVALID_HANDLE);
      float idM[16]; bx::mtxIdentity(idM); bgfx::setTransform(idM);
      if (bgfx::isValid(m_FullscreenVB) && bgfx::isValid(m_FullscreenIB)) {
         bgfx::setVertexBuffer(0, m_FullscreenVB); bgfx::setIndexBuffer(m_FullscreenIB);
         bgfx::setTexture(0, s_EdgeMask, m_EdgeMaskTex);
         glm::vec4 texelSize(1.0f / float(m_Width), 1.0f / float(m_Height), 0.0f, 0.0f);
         bgfx::setUniform(u_TexelSize, &texelSize);
         bgfx::setUniform(u_OutlineColor, &color);
         bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA);
         if (bgfx::isValid(m_OutlineCompositeProgram2)) {
            bgfx::submit(kView_OutlineComposite, m_OutlineCompositeProgram2);
         }
      }
   }
}

void Renderer::DrawCharacterController(const CharacterControllerComponent& cc, const TransformComponent& transform) {
	if (!bgfx::isValid(m_DebugLineProgram)) return;

	// Mirror the exact geometry used by CharacterVirtual creation
	const float r = glm::max(0.05f, cc.Radius);
	const float halfCyl = glm::max(0.0f, cc.Height * 0.5f); // cylinder half-height (excluding hemispheres)
	glm::vec3 up = glm::normalize(cc.Up);
	if (!glm::all(glm::isfinite(up)) || glm::length(up) < 1e-4f) up = glm::vec3(0.0f, 1.0f, 0.0f);
	const float feetToCenter = halfCyl + r;
	
	// Extract position and rotation from WorldMatrix to match physics exactly
	glm::vec3 pos = glm::vec3(transform.WorldMatrix[3]);
	glm::quat rq = transform.UseQuatRotation ?
		transform.RotationQ : glm::quat(glm::radians(transform.Rotation));
	
	// Offset is in local space, rotate it by entity rotation (matches physics)
	glm::vec3 offsetWorld = rq * cc.Offset;
	// up is world-space direction, feetToCenter is along world up
	glm::vec3 capsuleCenter = pos + offsetWorld + up * feetToCenter;
	
	// Build transform: translate to capsule center, then apply rotation
	glm::mat4 world = glm::translate(glm::mat4(1.0f), capsuleCenter) * glm::toMat4(rq);

	const int segments = 20;
	std::vector<GridVertex> verts; verts.reserve(segments * 24);

	auto addRing = [&](float y, float radius){
		for (int i = 0; i < segments; ++i) {
			float a0 = (float)i / segments * 6.2831853f;
			float a1 = (float)(i + 1) / segments * 6.2831853f;
			float x0 = radius * cosf(a0), z0 = radius * sinf(a0);
			float x1 = radius * cosf(a1), z1 = radius * sinf(a1);
			verts.push_back({ x0, y, z0 }); verts.push_back({ x1, y, z1 });
		}
	};

	auto addVerticalMeridian = [&](float angle){
		float x = cosf(angle), z = sinf(angle);
		// Cylinder segment
		verts.push_back({ r * x, -halfCyl, r * z });
		verts.push_back({ r * x,  halfCyl, r * z });
		// Bottom hemisphere arc (latitude -pi/2..0)
		const int arcSeg = 12;
		for (int i = 0; i < arcSeg; ++i) {
			float t0 = -(3.14159265f * 0.5f) + (float)i / arcSeg * (3.14159265f * 0.5f);
			float t1 = -(3.14159265f * 0.5f) + (float)(i + 1) / arcSeg * (3.14159265f * 0.5f);
			float y0 = -halfCyl + r * sinf(t0);
			float y1 = -halfCyl + r * sinf(t1);
			float rr0 = r * cosf(t0);
			float rr1 = r * cosf(t1);
			verts.push_back({ rr0 * x, y0, rr0 * z }); verts.push_back({ rr1 * x, y1, rr1 * z });
		}
		// Top hemisphere arc (latitude 0..pi/2)
		for (int i = 0; i < arcSeg; ++i) {
			float t0 = (float)i / arcSeg * (3.14159265f * 0.5f);
			float t1 = (float)(i + 1) / arcSeg * (3.14159265f * 0.5f);
			float y0 = halfCyl + r * sinf(t0);
			float y1 = halfCyl + r * sinf(t1);
			float rr0 = r * cosf(t0);
			float rr1 = r * cosf(t1);
			verts.push_back({ rr0 * x, y0, rr0 * z }); verts.push_back({ rr1 * x, y1, rr1 * z });
		}
	};

	// Seams (rings) at cylinder caps
	addRing( halfCyl, r);
	addRing(-halfCyl, r);

	// Vertical meridians (0, 90, 45, 135 degrees)
	const float pi = 3.14159265f;
	float baseAngles[2] = { 0.0f, pi * 0.5f };
	for (float a : baseAngles) {
		addVerticalMeridian(a);
		addVerticalMeridian(a + pi); // mirrored opposite side so arcs are complete around
	}

	if (!verts.empty()) {
		const bgfx::Memory* mem = bgfx::copy(verts.data(), (uint32_t)(verts.size() * sizeof(GridVertex)));
		bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(mem, GridVertex::layout);
		float M[16]; memcpy(M, glm::value_ptr(world), sizeof(float)*16);
		bgfx::setTransform(M);
		bgfx::setVertexBuffer(0, vbh);
		bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_PT_LINES);
      ApplyDefaultDebugLineColor();
		bgfx::submit(kDebugOverlayViewId, m_DebugLineProgram);
		bgfx::destroy(vbh);
	}
}


// Draw a global screen-space outline for all visible meshes in the scene when enabled.
void Renderer::DrawSceneOutline(Scene & scene) {
   const Environment& env = scene.GetEnvironment();
   if (!env.OutlineEnabled) return;

   // Reuse the same pipeline as DrawEntityOutline but render ObjectID for all visible entities, then edge/composite without selection filtering
   const uint16_t kView_ObjectId = 213;
   const uint16_t kView_OutlineEdge = 214;
   const uint16_t kView_OutlineComposite = 215;

   if (!bgfx::isValid(m_ObjectIdFB) || !bgfx::isValid(m_EdgeMaskFB)) {
      Resize(m_Width, m_Height);
   }

   RenderObjectIdScene(scene, kView_ObjectId);

   // 2) Edge pass: detect edges between different object ids across entire buffer
   {
      bgfx::setViewRect(kView_OutlineEdge, 0, 0, (uint16_t)m_Width, (uint16_t)m_Height);
      bgfx::setViewFrameBuffer(kView_OutlineEdge, m_EdgeMaskFB);
      bgfx::setViewClear(kView_OutlineEdge, BGFX_CLEAR_COLOR, 0x00000000, 1.0f, 0);
      float idM[16]; bx::mtxIdentity(idM); bgfx::setTransform(idM);
      if (bgfx::isValid(m_FullscreenVB) && bgfx::isValid(m_FullscreenIB)) {
         bgfx::setVertexBuffer(0, m_FullscreenVB); bgfx::setIndexBuffer(m_FullscreenIB);
         bgfx::setTexture(0, s_ObjectId, m_ObjectIdTex);
         glm::vec4 texelSize(1.0f / float(m_Width), 1.0f / float(m_Height), 0.0f, 0.0f);
         bgfx::setUniform(u_TexelSize, &texelSize);
         // Global mode: set mode=1; selected id is ignored in shader
         glm::vec4 selPacked(0.0f, 0.0f, 0.0f, 0.0f);
         bgfx::setUniform(u_SelectedIdPacked, &selPacked);
         glm::vec4 edgeParams(std::clamp(env.OutlineThickness, 1.0f, 8.0f), 1.0f, 0.0f, 0.0f);
         bgfx::setUniform(u_OutlineParams, &edgeParams);
         bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
         bgfx::submit(kView_OutlineEdge, m_OutlineEdgeProgram);
      }
   }

   // 3) Composite pass
   {
      bgfx::setViewRect(kView_OutlineComposite, 0, 0, (uint16_t)m_Width, (uint16_t)m_Height);
      if (m_RenderToOffscreen) bgfx::setViewFrameBuffer(kView_OutlineComposite, m_SceneFrameBuffer); else bgfx::setViewFrameBuffer(kView_OutlineComposite, BGFX_INVALID_HANDLE);
      float idM[16]; bx::mtxIdentity(idM); bgfx::setTransform(idM);
      if (bgfx::isValid(m_FullscreenVB) && bgfx::isValid(m_FullscreenIB)) {
         bgfx::setVertexBuffer(0, m_FullscreenVB); bgfx::setIndexBuffer(m_FullscreenIB);
         bgfx::setTexture(0, s_EdgeMask, m_EdgeMaskTex);
         glm::vec4 texelSize(1.0f / float(m_Width), 1.0f / float(m_Height), 0.0f, 0.0f);
         bgfx::setUniform(u_TexelSize, &texelSize);
         glm::vec4 color(glm::vec3(env.OutlineColor), 1.0f);
         bgfx::setUniform(u_OutlineColor, &color);
         bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA);
         bgfx::submit(kView_OutlineComposite, m_OutlineCompositeProgram2);
      }
   }
}
#endif // CLAYMORE_RUNTIME - DrawSceneOutline and related debug functions are editor-only

#ifndef CLAYMORE_RUNTIME
void Renderer::InitGrid(float size, float step) {
   std::vector<GridVertex> vertices;
   float half = size / 2.0f;
   for (float i = -half; i <= half; i += step) {
      vertices.push_back({ i, 0.0f, -half });
      vertices.push_back({ i, 0.0f,  half });
      vertices.push_back({ -half, 0.0f, i });
      vertices.push_back({ half, 0.0f, i });
      }
   m_GridVertexCount = (uint32_t)vertices.size();

   const bgfx::Memory* mem = bgfx::copy(vertices.data(), sizeof(GridVertex) * vertices.size());
   m_GridVB = bgfx::createVertexBuffer(mem, GridVertex::layout);
   }
#endif // CLAYMORE_RUNTIME - InitGrid is editor-only

// --------------------------------------
// Fullscreen triangle helpers (reuse across passes)
// --------------------------------------
void Renderer::EnsureFullscreenTriangle() {
   if (bgfx::isValid(m_FullscreenVB) && bgfx::isValid(m_FullscreenIB)) return;
   struct Pos { float x,y,z; };
   const Pos verts[3] = { {-1.0f,-1.0f,0.0f}, {3.0f,-1.0f,0.0f}, {-1.0f,3.0f,0.0f} };
   const uint16_t idx[3] = {0,1,2};
   bgfx::VertexLayout layout; layout.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float).end();
   const bgfx::Memory* vmem = bgfx::copy(verts, sizeof(verts));
   const bgfx::Memory* imem = bgfx::copy(idx, sizeof(idx));
   m_FullscreenVB = bgfx::createVertexBuffer(vmem, layout);
   m_FullscreenIB = bgfx::createIndexBuffer(imem);
}

void Renderer::DestroyFullscreenTriangle() {
   if (bgfx::isValid(m_FullscreenVB)) { bgfx::destroy(m_FullscreenVB); m_FullscreenVB = BGFX_INVALID_HANDLE; }
   if (bgfx::isValid(m_FullscreenIB)) { bgfx::destroy(m_FullscreenIB); m_FullscreenIB = BGFX_INVALID_HANDLE; }
}

Renderer::Frustum Renderer::BuildFrustum(const float* view, const float* proj) const {
   // Combine view-projection
   glm::mat4 V = glm::make_mat4(view);
   glm::mat4 P = glm::make_mat4(proj);
   glm::mat4 VP = P * V;
   // Extract clip planes from VP.
   // Plane eq: ax + by + cz + d = 0, store as (a,b,c,d).
   // NOTE: Camera projection matrices are generated with glm::perspective/ortho
   // in this codebase, i.e. OpenGL-style clip-space depth (near = -w).
   // Using backend caps (homogeneousDepth) here can over-cull on D3D because the
   // CPU culling convention no longer matches the matrix convention.
   Renderer::Frustum f{};
   auto row = [&](int r){ return glm::vec4(VP[0][r], VP[1][r], VP[2][r], VP[3][r]); };
   glm::vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
   glm::vec4 planes[6] = {
      r3 + r0, // left
      r3 - r0, // right
      r3 + r1, // bottom
      r3 - r1, // top
      r3 + r2, // near
      r3 - r2  // far
   };
   for (int i = 0; i < 6; ++i) {
      glm::vec3 n(planes[i]);
      float len = glm::length(n);
      if (len > 1e-6f) planes[i] /= len;
      f.planes[i].p = planes[i];
   }
   return f;
}

bool Renderer::AabbIntersectsFrustum(const Frustum& f, const glm::vec3& wmin, const glm::vec3& wmax) const {
   // For each plane, compute the positive vertex; if outside, culled
   constexpr float kCullEpsilon = 1e-4f;
   for (int i = 0; i < 6; ++i) {
      const glm::vec4& pl = f.planes[i].p;
      glm::vec3 p;
      p.x = (pl.x >= 0.0f) ? wmax.x : wmin.x;
      p.y = (pl.y >= 0.0f) ? wmax.y : wmin.y;
      p.z = (pl.z >= 0.0f) ? wmax.z : wmin.z;
      float d = pl.x * p.x + pl.y * p.y + pl.z * p.z + pl.w;
      if (d < -kCullEpsilon) return false;
   }
   return true;
}


// ---------------- Shadows ----------------
void Renderer::InitShadowResources(uint32_t resolution)
{
   ShutdownShadowResources();
   // Clamp resolution to a sane, supported range
   const uint32_t kMinRes = 16;
   const uint32_t kMaxRes = 8192;
   if (resolution < kMinRes) resolution = 0; // treat invalid as disabled
   if (resolution > kMaxRes) resolution = kMaxRes;
   m_ShadowRes = resolution;
   if (m_ShadowRes == 0) {
      return;
   }
   // Note: do NOT mark WRITE_ONLY; we need to sample this texture in the lighting pass
   const bgfx::Caps* caps = bgfx::getCaps();
   const bool supportsShadowCompare = caps && 0 != (caps->supported & BGFX_CAPS_TEXTURE_COMPARE_LEQUAL);
   uint64_t flags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
   if (supportsShadowCompare) {
      flags |= BGFX_SAMPLER_COMPARE_LEQUAL;
   }
   // Create a single depth atlas shared by all cascades; 2x2 tiles.
   // Also attach an R8 color debug target so we can inspect shadow coverage on-screen.
   m_ShadowDepth = bgfx::createTexture2D((uint16_t)resolution, (uint16_t)resolution, false, 1, bgfx::TextureFormat::D16, flags);
   m_ShadowDebugColor = bgfx::createTexture2D(
      (uint16_t)resolution,
      (uint16_t)resolution,
      false,
      1,
      bgfx::TextureFormat::R8,
      BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
   );
   if (bgfx::isValid(m_ShadowDepth) && bgfx::isValid(m_ShadowDebugColor)) {
      bgfx::TextureHandle attachments[] = { m_ShadowDebugColor, m_ShadowDepth };
      m_ShadowFB = bgfx::createFrameBuffer(2, attachments, false);
   } else if (bgfx::isValid(m_ShadowDepth)) {
      m_ShadowFB = bgfx::createFrameBuffer(1, &m_ShadowDepth, false);
   }

   // Point-light omnidirectional shadow atlas (3x2 tiles, one tile per cubemap face).
   m_PointShadowFaceRes = glm::max<uint32_t>(64u, m_ShadowRes / 2u);
   m_PointShadowAtlasWidth = m_PointShadowFaceRes * 3u;
   m_PointShadowAtlasHeight = m_PointShadowFaceRes * 2u * static_cast<uint32_t>(kMaxPointShadowLights);
   m_PointShadowColor = bgfx::createTexture2D(
      (uint16_t)m_PointShadowAtlasWidth,
      (uint16_t)m_PointShadowAtlasHeight,
      false,
      1,
      bgfx::TextureFormat::R16F,
      BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
   );
   m_PointShadowDepth = bgfx::createTexture2D(
      (uint16_t)m_PointShadowAtlasWidth,
      (uint16_t)m_PointShadowAtlasHeight,
      false,
      1,
      bgfx::TextureFormat::D16,
      BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
   );
   if (bgfx::isValid(m_PointShadowColor) && bgfx::isValid(m_PointShadowDepth)) {
      bgfx::TextureHandle pointAttachments[] = { m_PointShadowColor, m_PointShadowDepth };
      m_PointShadowFB = bgfx::createFrameBuffer(2, pointAttachments, false);
   }
   m_PointShadowCount = 0;
   m_PointShadowLightSlots.fill(-1);
   m_PointShadowLightPosWS.fill(glm::vec3(0.0f));
   m_PointShadowRanges.fill(0.0f);
}

void Renderer::ShutdownShadowResources()
{
   if (bgfx::isValid(m_ShadowFB)) { bgfx::destroy(m_ShadowFB); m_ShadowFB.idx = bgfx::kInvalidHandle; }
   if (bgfx::isValid(m_ShadowDebugColor)) { bgfx::destroy(m_ShadowDebugColor); m_ShadowDebugColor.idx = bgfx::kInvalidHandle; }
   if (bgfx::isValid(m_ShadowDepth)) { bgfx::destroy(m_ShadowDepth); m_ShadowDepth.idx = bgfx::kInvalidHandle; }
   if (bgfx::isValid(m_PointShadowFB)) { bgfx::destroy(m_PointShadowFB); m_PointShadowFB.idx = bgfx::kInvalidHandle; }
   if (bgfx::isValid(m_PointShadowColor)) { bgfx::destroy(m_PointShadowColor); m_PointShadowColor.idx = bgfx::kInvalidHandle; }
   if (bgfx::isValid(m_PointShadowDepth)) { bgfx::destroy(m_PointShadowDepth); m_PointShadowDepth.idx = bgfx::kInvalidHandle; }
   m_ShadowRes = 0;
   m_PointShadowFaceRes = 0;
   m_PointShadowAtlasWidth = 0;
   m_PointShadowAtlasHeight = 0;
   m_PointShadowCount = 0;
   m_PointShadowLightSlots.fill(-1);
   m_PointShadowLightPosWS.fill(glm::vec3(0.0f));
   m_PointShadowRanges.fill(0.0f);
}

static glm::mat4 OrthoOffCenter(float left, float right, float bottom, float top, float znear, float zfar)
{
   glm::mat4 m(1.0f);
   m[0][0] = 2.0f / (right - left);
   m[1][1] = 2.0f / (top - bottom);
   m[2][2] = -2.0f / (zfar - znear);
   m[3][0] = -(right + left) / (right - left);
   m[3][1] = -(top + bottom) / (top - bottom);
   m[3][2] = -(zfar + znear) / (zfar - znear);
   return m;
}

void Renderer::RenderShadowMap(Scene& scene, const Camera* camera)
{
   const bool hasDirectionalShadowTarget = bgfx::isValid(m_ShadowFB);
   const bool hasPointShadowTarget = bgfx::isValid(m_PointShadowFB) && bgfx::isValid(m_PointShadowColor);
   m_PointShadowCount = 0;
   m_PointShadowLightSlots.fill(-1);
   m_PointShadowLightPosWS.fill(glm::vec3(0.0f));
   m_PointShadowRanges.fill(0.0f);
   if (!hasDirectionalShadowTarget && !hasPointShadowTarget) {
      m_ShadowCascadeSubmitCounts = {0u, 0u, 0u, 0u};
      return;
   }

   // Find first directional light
   glm::vec3 lightDir = glm::normalize(glm::vec3(-0.5f, -1.0f, -0.2f));
   for (auto& entity : scene.GetEntities()) {
      auto* d = scene.GetEntityData(entity.GetID());
      if (!d || !d->Light || !IsPresentationVisible(d)) continue;
      if (d->Light->Type == LightType::Directional) {
         lightDir = LightDirectionFromTransform(d->Transform);
         break;
      }
   }
   m_ShadowDirWS = lightDir;

   // Cascaded shadow splits (4 cascades)
   glm::mat4 Vc = camera ? camera->GetViewMatrix() : glm::mat4(1.0f);
   glm::mat4 Pc = camera ? camera->GetProjectionMatrix() : glm::mat4(1.0f);
   float shadowDist = glm::max(1.0f, scene.GetEnvironment().ShadowDistance);
   m_CascadeCount = 4;
   float camNear = camera ? camera->GetNearClip() : 0.1f;
   float camFar = camera ? camera->GetFarClip() : shadowDist;
   float shadowFar = glm::min(camFar, shadowDist);
   if (shadowFar <= camNear + 0.001f) shadowFar = camNear + 0.001f;
   float lambda = 0.7f;
   for (int i = 0; i < m_CascadeCount; ++i) {
      float id = float(i + 1) / float(m_CascadeCount);
      float logd = camNear * pow(shadowFar / camNear, id);
      float unif = camNear + (shadowFar - camNear) * id;
      m_CascadeSplits[i] = glm::mix(unif, logd, lambda);
   }
   // Render cascades into 2x2 atlas
   glm::vec3 camPos = camera ? camera->GetPosition() : glm::vec3(0.0f);
   glm::mat4 invView = glm::inverse(Vc);
   glm::vec3 camRight = glm::normalize(glm::vec3(invView[0]));
   glm::vec3 camUp = glm::normalize(glm::vec3(invView[1]));
   glm::vec3 camFwd = glm::normalize(-glm::vec3(invView[2]));
   float fov = camera ? glm::radians(camera->GetFieldOfView()) : glm::radians(60.0f);
   float aspect = camera ? camera->GetAspectRatio() : 1.0f;
   float tanHalfFov = tan(fov * 0.5f);
   const bgfx::Caps* caps = bgfx::getCaps();
   int tiles = 2;
   float tileSize = (float)m_ShadowRes / (float)tiles;
   auto buildSliceCorners = [&](float nearDist, float farDist, std::array<glm::vec3, 8>& outCorners) {
      float nearHeight = tanHalfFov * nearDist;
      float nearWidth = nearHeight * aspect;
      float farHeight = tanHalfFov * farDist;
      float farWidth = farHeight * aspect;
      glm::vec3 nearCenter = camPos + camFwd * nearDist;
      glm::vec3 farCenter = camPos + camFwd * farDist;
      glm::vec3 nearUp = camUp * nearHeight;
      glm::vec3 nearRight = camRight * nearWidth;
      glm::vec3 farUp = camUp * farHeight;
      glm::vec3 farRight = camRight * farWidth;
      outCorners[0] = nearCenter - nearRight + nearUp;
      outCorners[1] = nearCenter + nearRight + nearUp;
      outCorners[2] = nearCenter + nearRight - nearUp;
      outCorners[3] = nearCenter - nearRight - nearUp;
      outCorners[4] = farCenter - farRight + farUp;
      outCorners[5] = farCenter + farRight + farUp;
      outCorners[6] = farCenter + farRight - farUp;
      outCorners[7] = farCenter - farRight - farUp;
   };
   struct ShadowCascadeCullData {
      glm::mat4 View = glm::mat4(1.0f);
      glm::vec3 Min = glm::vec3(0.0f);
      glm::vec3 Max = glm::vec3(0.0f);
      bool Valid = false;
   };
   std::array<float, 4> cascadeWorldRadii = { shadowFar, shadowFar, shadowFar, shadowFar };
   std::array<ShadowCascadeCullData, 4> cascadeCullData{};
   for (int ci = 0; ci < m_CascadeCount; ++ci) {
      float prevSplit = (ci == 0) ? camNear : m_CascadeSplits[ci - 1];
      float currSplit = m_CascadeSplits[ci];
      std::array<glm::vec3, 8> sliceCorners{};
      buildSliceCorners(prevSplit, currSplit, sliceCorners);

      // Camera-centered clipmap-style cascades:
      // 1) anchor each cascade around the camera instead of the slice centroid so
      //    rotation does not drag the projection around the scene,
      // 2) derive the extent from the farthest slice corner so coverage stays
      //    conservative for the current split,
      // 3) snap the projected center to texels to keep sampling stable.
      const glm::vec3 center = camPos;
      float radius = 0.0f;
      for (const auto& c : sliceCorners) {
         radius = glm::max(radius, glm::length(c - center));
      }
      radius = glm::max(radius, glm::max(currSplit, 0.5f));
      radius = std::ceil(radius * 16.0f) / 16.0f;
      if (ci >= 0 && ci < static_cast<int>(cascadeWorldRadii.size())) {
         cascadeWorldRadii[ci] = radius;
      }

      glm::vec3 up(0.0f, 1.0f, 0.0f);
      if (fabs(glm::dot(up, lightDir)) > 0.95f) up = glm::vec3(0.0f, 0.0f, 1.0f);
      glm::vec3 eye = center - lightDir * (radius + 100.0f);
      float vbx[16]; float pbx[16];
      bx::mtxLookAt(vbx, bx::Vec3{eye.x, eye.y, eye.z}, bx::Vec3{center.x, center.y, center.z}, bx::Vec3{up.x, up.y, up.z});
      glm::mat4 view = glm::make_mat4(vbx);

      glm::vec3 centerLS = glm::vec3(view * glm::vec4(center, 1.0f));
      const float texelWorld = (2.0f * radius) / glm::max(tileSize, 1.0f);
      if (texelWorld > 0.0f) {
         centerLS.x = std::floor(centerLS.x / texelWorld + 0.5f) * texelWorld;
         centerLS.y = std::floor(centerLS.y / texelWorld + 0.5f) * texelWorld;
      }

      float minZ = std::numeric_limits<float>::max();
      float maxZ = -std::numeric_limits<float>::max();
      for (const auto& c : sliceCorners) {
         glm::vec3 ls = glm::vec3(view * glm::vec4(c, 1.0f));
         minZ = glm::min(minZ, ls.z);
         maxZ = glm::max(maxZ, ls.z);
      }
      const float left = centerLS.x - radius;
      const float right = centerLS.x + radius;
      const float bottom = centerLS.y - radius;
      const float top = centerLS.y + radius;
      const float zPadding = glm::max(50.0f, radius * 2.0f);
      const float cullPadding = glm::max(texelWorld * 2.0f, 0.5f);
      bx::mtxOrtho(pbx, left, right, bottom, top, minZ - zPadding, maxZ + zPadding, 0.0f, caps->homogeneousDepth);
      glm::mat4 proj = glm::make_mat4(pbx);
      m_CascadeMatrices[ci] = proj * view;
      cascadeCullData[ci].View = view;
      cascadeCullData[ci].Min = glm::vec3(
         left - cullPadding,
         bottom - cullPadding,
         (minZ - zPadding) - cullPadding);
      cascadeCullData[ci].Max = glm::vec3(
         right + cullPadding,
         top + cullPadding,
         (maxZ + zPadding) + cullPadding);
      cascadeCullData[ci].Valid = true;
      // Atlas placement
      int tx = ci % tiles; int ty = ci / tiles;
      glm::vec2 scale = glm::vec2(1.0f / tiles);
      glm::vec2 bias = glm::vec2((float)tx / tiles, (float)ty / tiles);
      m_CascadeScaleBias[ci] = glm::vec4(scale, bias);
   }
   m_LightViewProj = m_CascadeMatrices[0];

   // Godot-style cascade culling is done against the cascade's stabilized light-space
   // coverage, not by guessing at clip-space conventions from the final matrix.
   auto aabbIntersectsCascade = [&](const ShadowCascadeCullData& cull,
                                    const glm::vec3& boundsMin,
                                    const glm::vec3& boundsMax) -> bool {
      if (!cull.Valid) {
         return true;
      }

      const glm::vec3 center = (boundsMin + boundsMax) * 0.5f;
      const glm::vec3 extents = (boundsMax - boundsMin) * 0.5f;
      const glm::vec3 centerLS = glm::vec3(cull.View * glm::vec4(center, 1.0f));
      const glm::mat3 basis(cull.View);
      glm::vec3 extentsLS;
      extentsLS.x =
         std::abs(basis[0][0]) * extents.x +
         std::abs(basis[1][0]) * extents.y +
         std::abs(basis[2][0]) * extents.z;
      extentsLS.y =
         std::abs(basis[0][1]) * extents.x +
         std::abs(basis[1][1]) * extents.y +
         std::abs(basis[2][1]) * extents.z;
      extentsLS.z =
         std::abs(basis[0][2]) * extents.x +
         std::abs(basis[1][2]) * extents.y +
         std::abs(basis[2][2]) * extents.z;
      const glm::vec3 minLS = centerLS - extentsLS;
      const glm::vec3 maxLS = centerLS + extentsLS;

      return maxLS.x >= cull.Min.x &&
         minLS.x <= cull.Max.x &&
         maxLS.y >= cull.Min.y &&
         minLS.y <= cull.Max.y &&
         maxLS.z >= cull.Min.z &&
         minLS.z <= cull.Max.z;
   };

   // Setup one shadow view per cascade tile (bgfx view rect performs atlas placement).
   // Scissor-only atlas packing clips, but does not remap NDC into each tile.
   float id[16]; bx::mtxIdentity(id);
   const uint16_t tileExtent = (uint16_t)tileSize;
   if (hasDirectionalShadowTarget) {
      for (int ci = 0; ci < m_CascadeCount; ++ci) {
         const uint16_t cascadeViewId = uint16_t(m_ShadowViewId + ci);
         const uint16_t x = (uint16_t)((ci % tiles) * tileSize);
         const uint16_t y = (uint16_t)((ci / tiles) * tileSize);
         bgfx::setViewFrameBuffer(cascadeViewId, m_ShadowFB);
         bgfx::setViewRect(cascadeViewId, x, y, tileExtent, tileExtent);
         bgfx::setViewTransform(cascadeViewId, id, id);
         const uint16_t clearFlags = bgfx::isValid(m_ShadowDebugColor) ? (BGFX_CLEAR_DEPTH | BGFX_CLEAR_COLOR) : BGFX_CLEAR_DEPTH;
         bgfx::setViewClear(cascadeViewId, clearFlags, 0x000000ff, 1.0f, 0);
         bgfx::touch(cascadeViewId);
      }
   }

   static bgfx::ProgramHandle s_depthProg = BGFX_INVALID_HANDLE;
   static bgfx::ProgramHandle s_depthProgSkinned = BGFX_INVALID_HANDLE;
   static bgfx::ProgramHandle s_depthProgSkinnedInstanced = BGFX_INVALID_HANDLE;
   static bgfx::ProgramHandle s_depthProgSkinnedMorph = BGFX_INVALID_HANDLE;
   static bgfx::ProgramHandle s_depthProgSkinnedMorphInstanced = BGFX_INVALID_HANDLE;
   static bgfx::UniformHandle u_ShadowPsxWorld = BGFX_INVALID_HANDLE;
   if (!bgfx::isValid(s_depthProg)) s_depthProg = ShaderManager::Instance().LoadProgram("vs_depth", "fs_depth");
   if (!bgfx::isValid(s_depthProgSkinned)) s_depthProgSkinned = ShaderManager::Instance().LoadProgram("vs_depth_skinned", "fs_depth");
   if (!bgfx::isValid(s_depthProgSkinnedInstanced)) s_depthProgSkinnedInstanced = ShaderManager::Instance().LoadProgram("vs_depth_skinned_instanced", "fs_depth");
   if (!bgfx::isValid(s_depthProgSkinnedMorph)) s_depthProgSkinnedMorph = ShaderManager::Instance().LoadProgram("vs_depth_skinned_morph", "fs_depth");
   if (!bgfx::isValid(s_depthProgSkinnedMorphInstanced)) s_depthProgSkinnedMorphInstanced = ShaderManager::Instance().LoadProgram("vs_depth_skinned_morph_instanced", "fs_depth");
   if (!bgfx::isValid(u_ShadowPsxWorld)) {
      u_ShadowPsxWorld = bgfx::createUniform("u_psxWorld", bgfx::UniformType::Vec4);
   }
   const glm::vec4 shadowPsxWorld(0.0f, 0.25f, 0.0f, 0.0f);
   auto resolveShadowSkinnedPath = [&](const EntityData* data,
                                       EntityID entityId,
                                       const Mesh* mesh,
                                       bgfx::VertexBufferHandle& outVertexBuffer,
                                       bgfx::ProgramHandle& outInstancedProgram,
                                       bgfx::ProgramHandle& outSingleProgram,
                                       bool& outUsesGpuMorph,
                                       bool& outUsesMaterializedSkinning) -> bool {
      outVertexBuffer = BGFX_INVALID_HANDLE;
      outInstancedProgram = BGFX_INVALID_HANDLE;
      outSingleProgram = BGFX_INVALID_HANDLE;
      outUsesGpuMorph = false;
      outUsesMaterializedSkinning = false;

      if (!data || !data->Skinning || !mesh || !bgfx::isValid(mesh->ibh)) {
         return false;
      }

      if (TryGetGpuMaterializedSkinnedVertexBuffer(scene, entityId, mesh, outVertexBuffer) &&
          bgfx::isValid(s_depthProg)) {
         outSingleProgram = s_depthProg;
         outUsesMaterializedSkinning = true;
         return bgfx::isValid(outVertexBuffer) && bgfx::isValid(outSingleProgram);
      }

      if (mesh->Dynamic) {
         if (!data->Skinning->HasGpuSkinningInstanceRecord()) {
            return false;
         }
         if (ShouldUseGpuMorphTargets(data->Skinning.get(), mesh, data->BlendShapes.get())) {
            outVertexBuffer = GetOrCreateGpuMorphVertexBuffer(mesh);
            outInstancedProgram = s_depthProgSkinnedMorphInstanced;
            outSingleProgram = s_depthProgSkinnedMorph;
            outUsesGpuMorph = true;
            return bgfx::isValid(outVertexBuffer) &&
               bgfx::isValid(outInstancedProgram);
         }

         return false;
      }

      outVertexBuffer = mesh->vbh;
      outInstancedProgram = s_depthProgSkinnedInstanced;
      outSingleProgram = s_depthProgSkinned;
      return bgfx::isValid(outVertexBuffer) &&
         bgfx::isValid(outInstancedProgram);
   };

   uint64_t shadowCastersScanned = 0;
   uint64_t shadowCascadeCulled = 0;
   uint64_t shadowSkinnedCascadeSkipped = 0;
   uint64_t shadowSkinnedOffscreenSkipped = 0;
   uint64_t shadowMainViewCulled = 0;
   uint64_t shadowCascadeSubmits = 0;
   uint64_t shadowSkinnedBatchedInstances = 0;
  std::array<uint32_t, 4> cascadeSubmitCounts = {0u, 0u, 0u, 0u};
   const cm::world::RuntimeWorld* runtimeWorld = scene.GetRuntimeWorld();
   auto* shadowRagdollSystem = scene.m_IsPlaying ? cm::physics::GetRagdollSystem() : nullptr;
   std::unordered_map<EntityID, std::pair<glm::vec3, glm::vec3>> shadowRagdollBoundsCache;
   shadowRagdollBoundsCache.reserve(16);
   std::vector<std::unordered_map<EntityID, bool>> shadowCascadeVisibilityCache(
      std::max(0, m_CascadeCount));
   for (auto& cache : shadowCascadeVisibilityCache) {
      cache.reserve(64);
   }
   auto tryGetShadowRagdollBounds = [&](const EntityData* data,
                                        const Mesh* mesh,
                                        glm::vec3& outMin,
                                        glm::vec3& outMax) -> bool {
      return TryGetSharedSkinnedCharacterBounds(
         scene,
         data,
         mesh,
         runtimeWorld,
         shadowRagdollSystem,
         shadowRagdollBoundsCache,
         outMin,
         outMax);
   };
   auto tryGetRuntimeShadowBounds = [&](size_t shadowIndex,
                                        glm::vec3& outMin,
                                        glm::vec3& outMax) -> bool {
      if (!runtimeWorld || shadowIndex >= m_ScratchShadowMeshHandles.size()) {
         return false;
      }

      const cm::world::RuntimeBounds* bounds =
         runtimeWorld->TryGetBounds(m_ScratchShadowMeshHandles[shadowIndex]);
      if (!bounds || !bounds->Valid) {
         return false;
      }

      outMin = bounds->WorldMin;
      outMax = bounds->WorldMax;
      return true;
   };

   // =========================================================================
   // PERF: Shadow LOD - skip small or distant objects from shadow maps
   // This significantly reduces shadow pass draw calls for complex scenes.
   // Objects are culled based on:
   //   1. Distance from camera (skip objects beyond shadow distance)
   //   2. Projected screen size (skip very small objects that won't cast visible shadows)
   // =========================================================================
   const float minShadowScreenSize = 0.005f;  // Skip objects smaller than 0.5% of screen
   std::array<Renderer::SkinningBindCacheState, 4> cascadeSkinningCache{};
   for (auto& manager : m_ShadowInstanceManagers) {
      manager.BeginFrame();
   }
   cm::instancer::ShadowRenderParams instancerShadowParams{};
   instancerShadowParams.CameraPosition = camPos;
   instancerShadowParams.CameraNear = camNear;
   instancerShadowParams.ShadowDistance = shadowFar;
   instancerShadowParams.CascadeCount = hasDirectionalShadowTarget ? m_CascadeCount : 0;
   instancerShadowParams.FirstCascadeViewId = m_ShadowViewId;
   for (int ci = 0; ci < instancerShadowParams.CascadeCount && ci < cm::instancer::kShadowCascadeCount; ++ci) {
      instancerShadowParams.CascadeSplits[ci] = m_CascadeSplits[ci];
      instancerShadowParams.CascadeMatrices[ci] = m_CascadeMatrices[ci];
   }
   cm::instancer::ShadowRenderStats instancerShadowStats{};
   instancerShadowParams.DirectionalStateFlags =
      BGFX_STATE_WRITE_Z |
      BGFX_STATE_DEPTH_TEST_LESS |
      BGFX_STATE_MSAA |
      BGFX_STATE_CULL_CW;
   if (bgfx::isValid(m_ShadowDebugColor)) {
      instancerShadowParams.DirectionalStateFlags |= BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A;
   }
   
   // Submit all opaque meshes depth-only to each cascade tile
   if (hasDirectionalShadowTarget) {
      for (size_t shadowIndex = 0; shadowIndex < m_ScratchShadowMeshEntityIds.size(); ++shadowIndex) {
      ++shadowCastersScanned;
      auto* d = shadowIndex < m_ScratchShadowMeshData.size() ? m_ScratchShadowMeshData[shadowIndex] : nullptr;
      if (!IsPresentationVisible(d) || !d->Active || !d->Mesh || !d->Mesh->mesh) continue;
      std::shared_ptr<Mesh> meshPtr = d->Mesh->mesh; if (!meshPtr) continue;
      if (!HasRenderableVertexSource(*this, d, meshPtr.get())) continue;

      glm::vec3 shadowCullMin(0.0f);
      glm::vec3 shadowCullMax(0.0f);
      const bool haveSharedSkinnedBounds =
         tryGetShadowRagdollBounds(d, meshPtr.get(), shadowCullMin, shadowCullMax);
      if (!haveSharedSkinnedBounds && !tryGetRuntimeShadowBounds(shadowIndex, shadowCullMin, shadowCullMax)) {
         const glm::vec3 lcenter = (meshPtr->BoundsMin + meshPtr->BoundsMax) * 0.5f;
         const glm::vec3 lextents =
            (meshPtr->BoundsMax - meshPtr->BoundsMin) * 0.5f * std::max(0.01f, d->Mesh->BoundsPadding);
         const glm::mat4& MW = d->Transform.WorldMatrix;
         const glm::vec3 worldCenter = glm::vec3(MW * glm::vec4(lcenter, 1.0f));
         glm::vec3 ex;
         ex.x = std::abs(MW[0][0]) * lextents.x + std::abs(MW[1][0]) * lextents.y + std::abs(MW[2][0]) * lextents.z;
         ex.y = std::abs(MW[0][1]) * lextents.x + std::abs(MW[1][1]) * lextents.y + std::abs(MW[2][1]) * lextents.z;
         ex.z = std::abs(MW[0][2]) * lextents.x + std::abs(MW[1][2]) * lextents.y + std::abs(MW[2][2]) * lextents.z;
         shadowCullMin = worldCenter - ex;
         shadowCullMax = worldCenter + ex;
      }

      // PERF: Shadow LOD culling
      // Use the most stable bounds we have for split-body skinned characters so
      // body-part meshes do not make conflicting shadow decisions.
      const glm::vec3 shadowCullCenter = (shadowCullMin + shadowCullMax) * 0.5f;
      const float shadowCullRadius = 0.5f * glm::length(shadowCullMax - shadowCullMin);
      const float distToCam = glm::length(shadowCullCenter - camPos);
      if ((distToCam - shadowCullRadius) > shadowFar * 1.2f) {
         continue;
      }

      if (shadowCullRadius > 0.001f) {
         const float screenSize = shadowCullRadius / glm::max(1.0f, distToCam);
         if (screenSize < minShadowScreenSize) continue;
      }

      const EntityID skelRoot = haveSharedSkinnedBounds ? ResolveSkinningSkeletonRootEntity(d) : INVALID_ENTITY_ID;
      const bool isSkinnedCaster = meshPtr->HasSkinning();
      const bool gpuMorphSkinnedCaster =
         isSkinnedCaster &&
         d->Skinning &&
         d->Skinning->HasGpuSkinningInstanceRecord() &&
         meshPtr->Dynamic &&
         ShouldUseGpuMorphTargets(d->Skinning.get(), meshPtr.get(), d->BlendShapes.get());

      uint64_t state = BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_MSAA;
      if (bgfx::isValid(m_ShadowDebugColor)) {
         state |= BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A;
      }
      if (!d->Mesh->ShowBackfaces) {
         state |= BGFX_STATE_CULL_CW;
      }
      float M[16]; memcpy(M, glm::value_ptr(d->Transform.WorldMatrix), sizeof(M));
      const bool decimateSkinnedShadows = scene.m_IsPlaying && !gpuMorphSkinnedCaster;
      const float skinnedCasterRadius =
         isSkinnedCaster
            ? (0.5f * glm::length(shadowCullMax - shadowCullMin))
            : 0.0f;
      for (int ci = 0; ci < m_CascadeCount; ++ci) {
         bool visibleInCascade = false;
         if (skelRoot != INVALID_ENTITY_ID) {
            auto& cache = shadowCascadeVisibilityCache[ci];
            auto cacheIt = cache.find(skelRoot);
            if (cacheIt == cache.end()) {
               cacheIt = cache.emplace(
                  skelRoot,
                  aabbIntersectsCascade(cascadeCullData[ci], shadowCullMin, shadowCullMax)).first;
            }
            visibleInCascade = cacheIt->second;
         } else {
            visibleInCascade = aabbIntersectsCascade(cascadeCullData[ci], shadowCullMin, shadowCullMax);
         }

         if (!visibleInCascade) {
            ++shadowCascadeCulled;
            continue;
         }
         if (decimateSkinnedShadows && isSkinnedCaster) {
            const float cascadeNear = (ci == 0) ? camNear : m_CascadeSplits[ci - 1];
            const float cascadeFar = (ci >= 0 && ci < static_cast<int>(m_CascadeSplits.size()))
               ? m_CascadeSplits[ci]
               : shadowFar;
            const float cascadeMargin = glm::clamp(skinnedCasterRadius, 1.0f, 8.0f);
            if ((distToCam + cascadeMargin) < cascadeNear ||
                (distToCam - cascadeMargin) > cascadeFar) {
               ++shadowSkinnedCascadeSkipped;
               continue;
            }
         }
         const uint16_t cascadeViewId = uint16_t(m_ShadowViewId + ci);
         if (isSkinnedCaster &&
             d->Skinning &&
             d->Skinning->HasGpuSkinningInstanceRecord()) {
            bgfx::VertexBufferHandle resolvedVbh = BGFX_INVALID_HANDLE;
            bgfx::ProgramHandle instancedProgram = BGFX_INVALID_HANDLE;
            bgfx::ProgramHandle singleProgram = BGFX_INVALID_HANDLE;
            bool usesGpuMorph = false;
            bool usesMaterializedSkinning = false;
            if (resolveShadowSkinnedPath(
                   d,
                   m_ScratchShadowMeshEntityIds[shadowIndex],
                   meshPtr.get(),
                   resolvedVbh,
                   instancedProgram,
                   singleProgram,
                   usesGpuMorph,
                   usesMaterializedSkinning)) {
               if (!bgfx::isValid(instancedProgram)) {
                  // Fall back to the single-draw path below while reusing the
                  // materialized vertex buffer through a second resolve call.
               } else {
               cm::rendering::SkinnedInstanceData instance{};
               instance.SetTransform(M);
               instance.SetMetadata(static_cast<float>(d->Skinning->GpuInstanceAtlasRecordIndex));

               cm::rendering::InstanceKey key;
               key.vbh = resolvedVbh;
               key.ibh = meshPtr->ibh;
               key.program = instancedProgram;
               key.indexStart = 0;
               key.indexCount = meshPtr->numIndices;
               key.variationHash = 0;

               auto& batch = m_ShadowInstanceManagers[ci].GetBatch(
                  key,
                  state,
                  static_cast<uint16_t>(sizeof(instance)));
               batch.AddInstance(instance);
               ++shadowSkinnedBatchedInstances;

               if (batch.instanceCount == 1u) {
                  const bgfx::VertexBufferHandle vbh = resolvedVbh;
                  const bgfx::IndexBufferHandle ibh = meshPtr->ibh;
                  const bgfx::ProgramHandle submitProgram = instancedProgram;
                  const glm::mat4 cascadeMatrix = m_CascadeMatrices[ci];
                  const char* debugName = usesGpuMorph
                     ? "vs_depth_skinned_morph_instanced+fs_depth"
                     : "vs_depth_skinned_instanced+fs_depth";
                  batch.bindBatch = [this, cascadeMatrix, shadowPsxWorld]() {
                     BindGpuSkinningAtlasGlobals();
                     bgfx::setUniform(u_ShadowPsxWorld, &shadowPsxWorld);
                     bgfx::setUniform(u_LightViewProj, glm::value_ptr(cascadeMatrix));
                  };
                  const SkinningComponent* skinning = d->Skinning.get();
                  const bgfx::ProgramHandle fallbackProgram = usesGpuMorph ? s_depthProgSkinnedMorph : s_depthProgSkinned;
                  const char* fallbackDebugName = usesGpuMorph
                     ? "vs_depth_skinned_morph+fs_depth"
                     : "vs_depth_skinned+fs_depth";
                  batch.submitSingle = [this, vbh, ibh, skinning, usesGpuMorph, submitProgram, debugName, fallbackProgram, fallbackDebugName, state, cascadeMatrix, shadowPsxWorld](uint16_t fallbackViewId, const uint8_t* instanceBytes) {
                     const auto* inst = reinterpret_cast<const cm::rendering::SkinnedInstanceData*>(instanceBytes);
                     if (!inst) {
                        return;
                     }

                     if (SubmitSingleSkinnedInstance(
                        fallbackViewId,
                        vbh,
                        ibh,
                        submitProgram,
                        *inst,
                        state,
                        debugName,
                        [this, cascadeMatrix, shadowPsxWorld]() {
                           BindGpuSkinningAtlasGlobals();
                           bgfx::setUniform(u_ShadowPsxWorld, &shadowPsxWorld);
                           bgfx::setUniform(u_LightViewProj, glm::value_ptr(cascadeMatrix));
                        })) {
                        return;
                     }

                     bgfx::setTransform(inst->transform);
                     bgfx::setVertexBuffer(0, vbh);
                     bgfx::setIndexBuffer(ibh);
                     if (usesGpuMorph) {
                        BindSkinningInstanceRecord(static_cast<uint32_t>(glm::max(inst->metadata[0], 0.0f)));
                     } else if (skinning) {
                        Renderer::SkinningBindCacheState singleSkinningCache{};
                        BindSkinningIfChanged(skinning, singleSkinningCache);
                     }
                     bgfx::setUniform(u_ShadowPsxWorld, &shadowPsxWorld);
                     bgfx::setUniform(u_LightViewProj, glm::value_ptr(cascadeMatrix));
                     bgfx::setState(state);
                     SafeSubmit(fallbackViewId, fallbackProgram, fallbackDebugName);
                  };
               }
               continue;
               }
            }
         }
         // bgfx consumes and clears draw state on submit, so re-bind per cascade.
         bgfx::setTransform(M);
         bgfx::ProgramHandle skinnedDepthProgram = s_depthProgSkinned;
         bool usesGpuMorph = false;
         bool usesMaterializedSkinning = false;
         if (isSkinnedCaster && d->Skinning) {
            bgfx::VertexBufferHandle resolvedVbh = BGFX_INVALID_HANDLE;
            bgfx::ProgramHandle ignoredInstancedProgram = BGFX_INVALID_HANDLE;
            if (resolveShadowSkinnedPath(
                   d,
                   m_ScratchShadowMeshEntityIds[shadowIndex],
                   meshPtr.get(),
                   resolvedVbh,
                   ignoredInstancedProgram,
                   skinnedDepthProgram,
                   usesGpuMorph,
                   usesMaterializedSkinning)) {
               bgfx::setVertexBuffer(0, resolvedVbh);
            } else if (meshPtr->Dynamic) {
               if (bgfx::isValid(meshPtr->dvbh)) {
                  bgfx::setVertexBuffer(0, meshPtr->dvbh, 0, meshPtr->numVertices);
               } else {
                  continue;
               }
            } else {
               bgfx::setVertexBuffer(0, meshPtr->vbh);
            }
         } else if (meshPtr->Dynamic) {
            if (bgfx::isValid(meshPtr->dvbh)) {
               bgfx::setVertexBuffer(0, meshPtr->dvbh, 0, meshPtr->numVertices);
            } else {
               continue;
            }
         } else {
            bgfx::setVertexBuffer(0, meshPtr->vbh);
         }
         bgfx::setIndexBuffer(meshPtr->ibh);
         bgfx::setState(state);
         bgfx::setUniform(u_ShadowPsxWorld, &shadowPsxWorld);
         bgfx::setUniform(u_LightViewProj, glm::value_ptr(m_CascadeMatrices[ci]));
         if (isSkinnedCaster) {
            // Bind per-entity bones from SkinningComponent (not material)
            if (d->Skinning) {
               if (usesGpuMorph) {
                  BindSkinningInstanceRecord(d->Skinning->GpuInstanceAtlasRecordIndex);
                  cascadeSkinningCache[ci] = {};
               } else if (!usesMaterializedSkinning) {
                  BindSkinningIfChanged(d->Skinning.get(), cascadeSkinningCache[ci]);
               }
            }
            SafeSubmit(
               cascadeViewId,
               skinnedDepthProgram,
               usesMaterializedSkinning
                  ? "vs_depth+fs_depth"
                  : (usesGpuMorph ? "vs_depth_skinned_morph+fs_depth" : "vs_depth_skinned+fs_depth"));
         } else {
            SafeSubmit(cascadeViewId, s_depthProg, "vs_depth+fs_depth");
         }
         ++shadowCascadeSubmits;
         if (ci >= 0 && ci < 4) {
            ++cascadeSubmitCounts[ci];
         }
      }
   }
   }

   if (hasDirectionalShadowTarget) {
      for (int ci = 0; ci < m_CascadeCount && ci < static_cast<int>(m_ShadowInstanceManagers.size()); ++ci) {
         const uint32_t estimatedSubmits = static_cast<uint32_t>(m_ShadowInstanceManagers[ci].EstimateSubmitCount());
         if (estimatedSubmits == 0u) {
            continue;
         }
         const uint16_t cascadeViewId = uint16_t(m_ShadowViewId + ci);
         m_ShadowInstanceManagers[ci].Submit(cascadeViewId);
         shadowCascadeSubmits += estimatedSubmits;
         if (ci >= 0 && ci < 4) {
            cascadeSubmitCounts[ci] += estimatedSubmits;
         }
      }
   }

   // -------------------------------------------------------------------------
   // Terrain shadow casters (clipmap, chunked, or legacy)
   // -------------------------------------------------------------------------
   uint64_t terrainShadowState =
      BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_MSAA | BGFX_STATE_CULL_CW;
   const uint32_t terrainShadowSamplerFlags =
      BGFX_SAMPLER_MIN_POINT |
      BGFX_SAMPLER_MAG_POINT |
      BGFX_SAMPLER_MIP_POINT |
      BGFX_SAMPLER_U_CLAMP |
      BGFX_SAMPLER_V_CLAMP;
   if (bgfx::isValid(m_ShadowDebugColor)) {
      terrainShadowState |= BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A;
   }
   if (hasDirectionalShadowTarget) {
      for (size_t terrainIndex = 0; terrainIndex < m_ScratchShadowTerrainEntityIds.size(); ++terrainIndex) {
      const EntityID shadowTerrainId = m_ScratchShadowTerrainEntityIds[terrainIndex];
      auto* data = terrainIndex < m_ScratchShadowTerrainData.size() ? m_ScratchShadowTerrainData[terrainIndex] : nullptr;
      if (!IsPresentationVisible(data) || !data->Active || !data->Terrain) continue;

      TerrainComponent& terrain = *data->Terrain;

      // Initialize or update clipmap system if enabled (mirrors main render path)
      if (terrain.UseClipmaps && bgfx::isValid(m_ClipmapDepthProgram)) {
         const float desiredClipmapBaseScale =
            terrain.WorldSize.x / static_cast<float>(std::max(1u, terrain.GridResolution - 1));
         if (terrain.ClipmapSystem) {
            auto* existingClipSystem = static_cast<terrain::TerrainClipmapSystem*>(terrain.ClipmapSystem);
            const terrain::ClipmapConfig& existingConfig = existingClipSystem->GetConfig();
            const bool clipmapConfigChanged =
               existingConfig.LevelCount != terrain.ClipmapLevels ||
               existingConfig.GridSize != terrain.ClipmapGridSize ||
               existingConfig.EnableMorphing != terrain.ClipmapMorphing ||
               std::abs(existingConfig.BaseScale - desiredClipmapBaseScale) > 1e-4f;
            if (clipmapConfigChanged) {
               Terrain::DestroyClipmapSystem(terrain);
            }
         }
         if (!terrain.ClipmapSystem) {
            auto* clipSystem = new terrain::TerrainClipmapSystem();
            terrain::ClipmapConfig config;
            config.LevelCount = terrain.ClipmapLevels;
            config.GridSize = terrain.ClipmapGridSize;
            config.BaseScale = desiredClipmapBaseScale;
            config.EnableMorphing = terrain.ClipmapMorphing;
            clipSystem->Init(config);
            terrain.ClipmapSystem = clipSystem;
         }
         auto* clipSystem = static_cast<terrain::TerrainClipmapSystem*>(terrain.ClipmapSystem);
         clipSystem->Update(camPos, terrain.WorldSize);
      } else if (!terrain.UseClipmaps && terrain.ClipmapSystem) {
         Terrain::DestroyClipmapSystem(terrain);
      }

      if (!terrain.UseChunkedTerrain && terrain.ChunkStreamingSystem) {
         Terrain::DestroyStreamingSystem(terrain);
         for (TerrainChunk& chunk : terrain.Chunks) {
            chunk.StreamState = ChunkStreamState::Resident;
         }
      }

      Terrain::PrepareForRendering(terrain);

      if (!bgfx::isValid(terrain.ChunkVB) || !bgfx::isValid(terrain.ChunkIB)) {
         continue;
      }

      glm::vec2 cellSize = Terrain::GetCellSize(terrain);
      glm::vec4 heightParams(terrain.MaxHeight, cellSize.x, cellSize.y, 0.0f);
      float transform[16];
      memcpy(transform, glm::value_ptr(data->Transform.WorldMatrix), sizeof(float) * 16);

      // Prefer clipmaps when enabled
      if (terrain.UseClipmaps && terrain.ClipmapSystem && bgfx::isValid(m_ClipmapDepthProgram)) {
         if (terrain.Chunks.empty() || !bgfx::isValid(terrain.Chunks[0].HeightTexture)) {
            continue;
         }
         auto* clipSystem = static_cast<terrain::TerrainClipmapSystem*>(terrain.ClipmapSystem);
         const auto& levels = clipSystem->GetLevels();
         const auto& meshes = clipSystem->GetMeshes();
         const auto& config = clipSystem->GetConfig();
         bgfx::TextureHandle heightTex = terrain.Chunks[0].HeightTexture;
         bgfx::TextureHandle holeTex = bgfx::isValid(terrain.Chunks[0].HoleTexture) ? terrain.Chunks[0].HoleTexture : m_TerrainFallbackHole;

         for (int ci = 0; ci < m_CascadeCount; ++ci) {
            const uint16_t cascadeViewId = uint16_t(m_ShadowViewId + ci);
            bgfx::setUniform(u_LightViewProj, glm::value_ptr(m_CascadeMatrices[ci]));

            for (int lvl = static_cast<int>(levels.size()) - 1; lvl >= 0; --lvl) {
               const auto& level = levels[lvl];
               float morphFactor = 0.0f;
               if (config.EnableMorphing && lvl < static_cast<int>(levels.size()) - 1) {
                  const float levelExtent = level.Scale * static_cast<float>(config.GridSize);
                  const float morphStart = levelExtent * (1.0f - config.MorphRegion);
                  const glm::vec2 camXZ(camPos.x, camPos.z);
                  const float distFromCenter = glm::length(camXZ - level.SnapOffset);
                  morphFactor = glm::clamp((distFromCenter - morphStart) / (levelExtent - morphStart), 0.0f, 1.0f);
               }

               glm::vec4 clipParams(level.Scale, morphFactor, static_cast<float>(config.GridSize), static_cast<float>(lvl));
               bgfx::setUniform(u_ClipmapParams, glm::value_ptr(clipParams));
               glm::vec4 clipOffset(level.SnapOffset.x, level.SnapOffset.y, terrain.WorldSize.x, terrain.WorldSize.y);
               bgfx::setUniform(u_ClipmapOffset, glm::value_ptr(clipOffset));
               glm::vec3 terrainOrigin = glm::vec3(data->Transform.WorldMatrix[3]);
               glm::vec4 originVec(terrainOrigin.x, terrainOrigin.y, terrainOrigin.z, 0.0f);
               bgfx::setUniform(u_TerrainOrigin, glm::value_ptr(originVec));
               bgfx::setUniform(u_TerrainHeightParams, glm::value_ptr(heightParams));

               bgfx::setTransform(transform);
               bgfx::setTexture(0, s_TerrainHeightTexture, heightTex, terrainShadowSamplerFlags);
               if (bgfx::isValid(holeTex))
                  bgfx::setTexture(5, s_TerrainHoleTexture, holeTex, terrainShadowSamplerFlags);

               if (lvl == 0) {
                  if (bgfx::isValid(meshes.CenterVB) && bgfx::isValid(meshes.CenterIB)) {
                     bgfx::setVertexBuffer(0, meshes.CenterVB);
                     bgfx::setIndexBuffer(meshes.CenterIB);
                     bgfx::setState(terrainShadowState);
                     bgfx::submit(cascadeViewId, m_ClipmapDepthProgram);
                     ++shadowCascadeSubmits;
                     if (ci >= 0 && ci < 4) ++cascadeSubmitCounts[ci];
                  }
               } else {
                  if (bgfx::isValid(meshes.RingVB) && bgfx::isValid(meshes.RingIB)) {
                     bgfx::setVertexBuffer(0, meshes.RingVB);
                     bgfx::setIndexBuffer(meshes.RingIB);
                     bgfx::setState(terrainShadowState);
                     bgfx::submit(cascadeViewId, m_ClipmapDepthProgram);
                     ++shadowCascadeSubmits;
                     if (ci >= 0 && ci < 4) ++cascadeSubmitCounts[ci];
                  }
               }
            }
         }
         continue;
      }

      // Chunked terrain path
      if (terrain.UseChunkedTerrain && m_ChunkSystem) {
         bool chunkLayoutChanged = false;
         if (terrain.Chunks.size() != m_ChunkSystem->GetTotalChunks() || terrain.ChunkMeshDirty) {
            terrain::ChunkSystemConfig chunkConfig;
            chunkConfig.ChunkVertexSize = terrain.ChunkVertexSize;
            chunkConfig.EnableMorphing = terrain.ChunkMorphing;
            chunkConfig.MorphRegion = terrain.ChunkMorphRegion;
            for (uint32_t i = 0; i < terrain::ChunkSystemConfig::kMaxLODLevels; ++i) {
               chunkConfig.LODDistances[i] = terrain.LODConfig.LODDistances[i];
               chunkConfig.LODSteps[i] = terrain.LODConfig.LODSteps[i];
            }
            m_ChunkSystem->Init(terrain, chunkConfig);
            chunkLayoutChanged = true;
            terrain.RuntimeChunkPrepToken = 0;
         }

         const bool streamingEnabled = terrain.ChunkStreaming && (cm::g_JobSystem != nullptr);
         bool didStreamingUpdate = false;
         if (streamingEnabled)
         {
            if (!terrain.ChunkStreamingSystem)
            {
               auto* streaming = new terrain::TerrainStreamingSystem();
               terrain::StreamingConfig config;
               config.Enabled = true;
               config.LoadRadius = terrain.StreamingLoadRadius;
               config.UnloadRadius = terrain.StreamingUnloadRadius;
               streaming->Init(terrain, *cm::g_JobSystem, config);
               terrain.ChunkStreamingSystem = streaming;
               didStreamingUpdate = true;
            }

            auto* streaming = static_cast<terrain::TerrainStreamingSystem*>(terrain.ChunkStreamingSystem);
            if (streaming)
            {
               if (chunkLayoutChanged)
               {
                  terrain::StreamingConfig config = streaming->GetConfig();
                  config.Enabled = true;
                  config.LoadRadius = terrain.StreamingLoadRadius;
                  config.UnloadRadius = terrain.StreamingUnloadRadius;
                  streaming->Init(terrain, *cm::g_JobSystem, config);
                  didStreamingUpdate = true;
               }
               else
               {
                  auto& config = streaming->GetConfig();
                  config.Enabled = true;
                  config.LoadRadius = terrain.StreamingLoadRadius;
                  config.UnloadRadius = terrain.StreamingUnloadRadius;
               }

               if (chunkLayoutChanged || terrain.RuntimeChunkPrepToken != m_TerrainChunkPrepToken)
               {
                  streaming->Update(terrain, camPos, data->Transform.WorldMatrix);
                  streaming->ProcessGpuUploads(terrain);
                  didStreamingUpdate = true;
               }
            }
         }
         else if (terrain.ChunkStreamingSystem)
         {
            Terrain::DestroyStreamingSystem(terrain);
            for (TerrainChunk& chunk : terrain.Chunks)
               chunk.StreamState = ChunkStreamState::Resident;
            didStreamingUpdate = true;
         }

         const bool needsChunkPrep = chunkLayoutChanged ||
                                     didStreamingUpdate ||
                                     (terrain.RuntimeChunkPrepToken != m_TerrainChunkPrepToken);
         if (needsChunkPrep)
         {
            Profiler::Get().AddCounter("Render/TerrainPrepRuns", 1);
            terrain::ChunkFrustum chunkFrustum = terrain::ChunkFrustum::FromViewProj(Pc * Vc);
            if (cm::g_JobSystem != nullptr) {
               m_ChunkSystem->UpdateChunkLODParallel(terrain, camPos, data->Transform.WorldMatrix, chunkFrustum, *cm::g_JobSystem);
            } else {
               m_ChunkSystem->UpdateChunkLOD(terrain, camPos, data->Transform.WorldMatrix, chunkFrustum);
            }
            m_ChunkSystem->EnforceRestrictedLOD(terrain);
            m_ChunkSystem->UpdateMorphFactors(terrain, camPos);
            m_ChunkSystem->UpdateNeighborLODs(terrain);
            terrain.RuntimeChunkPrepToken = m_TerrainChunkPrepToken;
         }
         else
         {
            Profiler::Get().AddCounter("Render/TerrainPrepSkips", 1);
         }

         if (terrain.Chunks.empty() || !bgfx::isValid(terrain.Chunks[0].HeightTexture)) {
            continue;
         }
         bgfx::TextureHandle heightTex = terrain.Chunks[0].HeightTexture;
         bgfx::TextureHandle holeTex = bgfx::isValid(terrain.Chunks[0].HoleTexture) ? terrain.Chunks[0].HoleTexture : m_TerrainFallbackHole;
         const float invRes = 1.0f / static_cast<float>(std::max(1u, terrain.GridResolution));
         glm::vec4 chunkTexelSize(invRes, invRes, 0.0f, 0.0f);
         const float lightVertical = glm::max(glm::abs(lightDir.y), 0.25f);
         const float lightHorizontal = glm::length(glm::vec2(lightDir.x, lightDir.z));
         const float terrainCasterReach = glm::min(shadowFar, terrain.MaxHeight * lightHorizontal / lightVertical);
         const float terrainShadowPadding = glm::max(glm::max(32.0f, shadowFar * 0.25f), terrainCasterReach);
         const auto shadowChunkDistanceLimit = [&](int cascadeIndex) {
            const int safeIndex = glm::clamp(cascadeIndex, 0, 3);
            return cascadeWorldRadii[static_cast<size_t>(safeIndex)] + terrainShadowPadding;
         };

         if (bgfx::isValid(m_ChunkedTerrainDepthInstancedProgram) && bgfx::isValid(terrain.SharedChunkVB)) {
            auto& batchCache = m_TerrainChunkShadowBatchCache[shadowTerrainId];
            if (batchCache.token != m_TerrainChunkPrepToken)
            {
               batchCache.totalVisible = 0;
               for (int ci = 0; ci < 4; ++ci) {
                  auto& cascadeCache = batchCache.cascades[static_cast<size_t>(ci)];
                  cascadeCache.distanceLimit = shadowChunkDistanceLimit(ci);
                  cascadeCache.totalVisible =
                     m_ChunkSystem->PrepareInstancedBatchesNearCamera(terrain, cascadeCache.distanceLimit, cascadeCache.batches);
                  if (ci < m_CascadeCount) {
                     batchCache.totalVisible += cascadeCache.totalVisible;
                  }
               }
               batchCache.token = m_TerrainChunkPrepToken;
            }
            if (batchCache.totalVisible > 0) {
               Profiler::Get().AddCounter("Render/TerrainShadowVisibleChunks", batchCache.totalVisible);
               constexpr uint16_t instanceStride = sizeof(terrain::ChunkInstanceData);
               for (int ci = 0; ci < m_CascadeCount; ++ci) {
                  const auto& cascadeCache = batchCache.cascades[static_cast<size_t>(ci)];
                  if (cascadeCache.totalVisible == 0) continue;
                  const uint16_t cascadeViewId = uint16_t(m_ShadowViewId + ci);
                  bgfx::setUniform(u_LightViewProj, glm::value_ptr(m_CascadeMatrices[ci]));

                  for (uint32_t lod = 0; lod < terrain::ChunkSystemConfig::kMaxLODLevels; ++lod) {
                     const auto& batch = cascadeCache.batches[lod];
                     if (batch.Empty()) continue;

                     bgfx::IndexBufferHandle lodIB = m_ChunkSystem->GetLODIndexBuffer(terrain, lod);
                     uint32_t lodIndexCount = m_ChunkSystem->GetLODIndexCount(terrain, lod);
                     if (!bgfx::isValid(lodIB) || lodIndexCount == 0) continue;

                     uint32_t instanceCount = static_cast<uint32_t>(batch.Size());
                     uint32_t availableInstances = bgfx::getAvailInstanceDataBuffer(instanceCount, instanceStride);
                     if (availableInstances == 0) continue;
                     if (instanceCount > availableInstances) instanceCount = availableInstances;

                     bgfx::InstanceDataBuffer idb{};
                     bgfx::allocInstanceDataBuffer(&idb, instanceCount, instanceStride);
                     std::memcpy(idb.data, batch.Instances.data(), instanceCount * instanceStride);

                     bgfx::setTransform(transform);
                     bgfx::setVertexBuffer(0, terrain.SharedChunkVB);
                     bgfx::setIndexBuffer(lodIB, 0, lodIndexCount);
                     bgfx::setInstanceDataBuffer(&idb);
                     bgfx::setTexture(0, s_TerrainHeightTexture, heightTex, terrainShadowSamplerFlags);
                     if (bgfx::isValid(holeTex))
                        bgfx::setTexture(5, s_TerrainHoleTexture, holeTex, terrainShadowSamplerFlags);
                     bgfx::setUniform(u_TerrainHeightParams, glm::value_ptr(heightParams));
                     bgfx::setUniform(u_TerrainTexelSize, glm::value_ptr(chunkTexelSize));
                     bgfx::setState(terrainShadowState);
                     bgfx::submit(cascadeViewId, m_ChunkedTerrainDepthInstancedProgram);
                     ++shadowCascadeSubmits;
                     if (ci >= 0 && ci < 4) ++cascadeSubmitCounts[ci];
                  }
               }
            }
         } else if (bgfx::isValid(m_ChunkedTerrainDepthProgram)) {
            for (int ci = 0; ci < m_CascadeCount; ++ci) {
               const float distanceLimit = shadowChunkDistanceLimit(ci);
               const uint16_t cascadeViewId = uint16_t(m_ShadowViewId + ci);
               bgfx::setUniform(u_LightViewProj, glm::value_ptr(m_CascadeMatrices[ci]));

               for (const TerrainChunk& chunk : terrain.Chunks) {
                  if (!chunk.Visible || chunk.CurrentLOD < 0) continue;
                  if (chunk.StreamState != ChunkStreamState::Resident) continue;
                  if (chunk.DistanceToCamera > distanceLimit) continue;

                  const uint32_t lodLevel = static_cast<uint32_t>(chunk.CurrentLOD);
                  bgfx::IndexBufferHandle lodIB = m_ChunkSystem->GetLODIndexBuffer(terrain, lodLevel);
                  uint32_t lodIndexCount = m_ChunkSystem->GetLODIndexCount(terrain, lodLevel);
                  if (!bgfx::isValid(terrain.SharedChunkVB) || !bgfx::isValid(lodIB) || lodIndexCount == 0) continue;

                  glm::vec4 chunkParams(chunk.UVOffset.x, chunk.UVOffset.y, chunk.UVScale.x, chunk.UVScale.y);
                  glm::vec4 chunkWorld(chunk.WorldOffset.x, chunk.WorldOffset.y, chunk.WorldExtent.x, chunk.WorldExtent.y);
                  glm::vec4 morphParams(chunk.MorphFactor, static_cast<float>(chunk.CurrentLOD), static_cast<float>(terrain.ChunkVertexSize), 0.0f);
                  glm::vec4 neighborLODs(
                     static_cast<float>(chunk.NeighborLODs[0]),
                     static_cast<float>(chunk.NeighborLODs[1]),
                     static_cast<float>(chunk.NeighborLODs[2]),
                     static_cast<float>(chunk.NeighborLODs[3]));

                  bgfx::setTransform(transform);
                  bgfx::setVertexBuffer(0, terrain.SharedChunkVB);
                  bgfx::setIndexBuffer(lodIB, 0, lodIndexCount);
                  bgfx::setTexture(0, s_TerrainHeightTexture, heightTex, terrainShadowSamplerFlags);
                  if (bgfx::isValid(holeTex))
                     bgfx::setTexture(5, s_TerrainHoleTexture, holeTex, terrainShadowSamplerFlags);
                  bgfx::setUniform(u_ChunkParams, glm::value_ptr(chunkParams));
                  bgfx::setUniform(u_ChunkWorld, glm::value_ptr(chunkWorld));
                  bgfx::setUniform(u_MorphParams, glm::value_ptr(morphParams));
                  bgfx::setUniform(u_NeighborLODs, glm::value_ptr(neighborLODs));
                  bgfx::setUniform(u_TerrainHeightParams, glm::value_ptr(heightParams));
                  bgfx::setUniform(u_TerrainTexelSize, glm::value_ptr(chunkTexelSize));
                  bgfx::setState(terrainShadowState);
                  bgfx::submit(cascadeViewId, m_ChunkedTerrainDepthProgram);
                  ++shadowCascadeSubmits;
                  if (ci >= 0 && ci < 4) ++cascadeSubmitCounts[ci];
               }
            }
         }
         continue;
      }

      // Legacy non-chunked terrain path
      if (!terrain.Chunks.empty() && bgfx::isValid(m_TerrainDepthProgram)) {
         for (int ci = 0; ci < m_CascadeCount; ++ci) {
            const uint16_t cascadeViewId = uint16_t(m_ShadowViewId + ci);
            bgfx::setUniform(u_LightViewProj, glm::value_ptr(m_CascadeMatrices[ci]));

            for (const TerrainChunk& chunk : terrain.Chunks) {
               if (!bgfx::isValid(chunk.HeightTexture)) continue;
               const uint32_t chunkQuadsX = chunk.VertexCountX > 0 ? chunk.VertexCountX - 1 : 0;
               const uint32_t chunkQuadsZ = chunk.VertexCountZ > 0 ? chunk.VertexCountZ - 1 : 0;
               glm::vec4 chunkParams(
                  chunk.Start.x * cellSize.x,
                  chunk.Start.y * cellSize.y,
                  glm::max(1u, chunkQuadsX) * cellSize.x,
                  glm::max(1u, chunkQuadsZ) * cellSize.y);
               glm::vec4 texelSize(
                  1.0f / glm::max(1u, chunk.VertexCountX),
                  1.0f / glm::max(1u, chunk.VertexCountZ),
                  0.0f,
                  0.0f);

               bgfx::setTransform(transform);
               bgfx::setVertexBuffer(0, terrain.ChunkVB);
               bgfx::setIndexBuffer(terrain.ChunkIB);
               bgfx::setTexture(0, s_TerrainHeightTexture, chunk.HeightTexture, terrainShadowSamplerFlags);
               bgfx::TextureHandle holeTex = bgfx::isValid(chunk.HoleTexture) ? chunk.HoleTexture : m_TerrainFallbackHole;
               if (bgfx::isValid(holeTex))
                  bgfx::setTexture(5, s_TerrainHoleTexture, holeTex, terrainShadowSamplerFlags);
               bgfx::setUniform(u_TerrainChunkParams, glm::value_ptr(chunkParams));
               bgfx::setUniform(u_TerrainHeightParams, glm::value_ptr(heightParams));
               bgfx::setUniform(u_TerrainTexelSize, glm::value_ptr(texelSize));
               bgfx::setState(terrainShadowState);
               bgfx::submit(cascadeViewId, m_TerrainDepthProgram);
               ++shadowCascadeSubmits;
               if (ci >= 0 && ci < 4) ++cascadeSubmitCounts[ci];
            }
         }
      }
      }
   }

   // -------------------------------------------------------------------------
   // Point-light shadow caster pass (budgeted multi-light, spatially gated)
   // -------------------------------------------------------------------------
   const bgfx::Caps* shadowCaps = bgfx::getCaps();
   const uint16_t maxViews = shadowCaps ? static_cast<uint16_t>(shadowCaps->limits.maxViews) : 256u;
   const bool pointViewRangeValid = (m_PointShadowViewId + static_cast<uint16_t>(6 * kMaxPointShadowLights)) <= maxViews;
   if (hasPointShadowTarget && pointViewRangeValid && bgfx::isValid(s_depthProg) && bgfx::isValid(s_depthProgSkinned)) {
      static bgfx::ProgramHandle s_pointDepthProg = BGFX_INVALID_HANDLE;
      static bgfx::ProgramHandle s_pointDepthProgSkinned = BGFX_INVALID_HANDLE;
      static bgfx::ProgramHandle s_pointDepthProgSkinnedInstanced = BGFX_INVALID_HANDLE;
      static bgfx::ProgramHandle s_pointDepthProgSkinnedMorph = BGFX_INVALID_HANDLE;
      static bgfx::ProgramHandle s_pointDepthProgSkinnedMorphInstanced = BGFX_INVALID_HANDLE;
      static bgfx::UniformHandle u_PointShadowLightPosRangeDepth = BGFX_INVALID_HANDLE;
      if (!bgfx::isValid(s_pointDepthProg)) s_pointDepthProg = ShaderManager::Instance().LoadProgram("vs_depth", "fs_point_shadow_depth");
      if (!bgfx::isValid(s_pointDepthProgSkinned)) s_pointDepthProgSkinned = ShaderManager::Instance().LoadProgram("vs_depth_skinned", "fs_point_shadow_depth");
      if (!bgfx::isValid(s_pointDepthProgSkinnedInstanced)) {
         s_pointDepthProgSkinnedInstanced =
            ShaderManager::Instance().LoadProgram("vs_depth_skinned_instanced", "fs_point_shadow_depth");
      }
      if (!bgfx::isValid(s_pointDepthProgSkinnedMorph)) {
         s_pointDepthProgSkinnedMorph =
            ShaderManager::Instance().LoadProgram("vs_depth_skinned_morph", "fs_point_shadow_depth");
      }
      if (!bgfx::isValid(s_pointDepthProgSkinnedMorphInstanced)) {
         s_pointDepthProgSkinnedMorphInstanced =
            ShaderManager::Instance().LoadProgram("vs_depth_skinned_morph_instanced", "fs_point_shadow_depth");
      }
      if (!bgfx::isValid(u_PointShadowLightPosRangeDepth)) {
         u_PointShadowLightPosRangeDepth = bgfx::createUniform("u_pointShadowLightPosRangeDepth", bgfx::UniformType::Vec4);
      }
      if (bgfx::isValid(s_pointDepthProg) && bgfx::isValid(s_pointDepthProgSkinned) && bgfx::isValid(u_PointShadowLightPosRangeDepth)) {
         auto resolvePointShadowSkinnedPath = [&](const EntityData* data,
                                                  EntityID entityId,
                                                  const Mesh* mesh,
                                                  bgfx::VertexBufferHandle& outVertexBuffer,
                                                  bgfx::ProgramHandle& outInstancedProgram,
                                                  bgfx::ProgramHandle& outSingleProgram,
                                                  bool& outUsesGpuMorph,
                                                  bool& outUsesMaterializedSkinning) -> bool {
            outVertexBuffer = BGFX_INVALID_HANDLE;
            outInstancedProgram = BGFX_INVALID_HANDLE;
            outSingleProgram = BGFX_INVALID_HANDLE;
            outUsesGpuMorph = false;
            outUsesMaterializedSkinning = false;

            if (!data || !data->Skinning || !mesh || !bgfx::isValid(mesh->ibh)) {
               return false;
            }

            if (TryGetGpuMaterializedSkinnedVertexBuffer(scene, entityId, mesh, outVertexBuffer) &&
                bgfx::isValid(s_pointDepthProg)) {
               outSingleProgram = s_pointDepthProg;
               outUsesMaterializedSkinning = true;
               return bgfx::isValid(outVertexBuffer) && bgfx::isValid(outSingleProgram);
            }

            if (mesh->Dynamic) {
               if (!data->Skinning->HasGpuSkinningInstanceRecord()) {
                  return false;
               }
               if (ShouldUseGpuMorphTargets(data->Skinning.get(), mesh, data->BlendShapes.get())) {
                  outVertexBuffer = GetOrCreateGpuMorphVertexBuffer(mesh);
                  outInstancedProgram = s_pointDepthProgSkinnedMorphInstanced;
                  outSingleProgram = s_pointDepthProgSkinnedMorph;
                  outUsesGpuMorph = true;
                  return bgfx::isValid(outVertexBuffer) &&
                     bgfx::isValid(outInstancedProgram);
               }

               return false;
            }

            outVertexBuffer = mesh->vbh;
            outInstancedProgram = s_pointDepthProgSkinnedInstanced;
            outSingleProgram = s_pointDepthProgSkinned;
            return bgfx::isValid(outVertexBuffer) &&
               bgfx::isValid(outInstancedProgram);
         };
         for (auto& manager : m_PointShadowInstanceManagers) {
            manager.BeginFrame();
         }
         // Mirror shader light selection policy so point-shadow slot mapping is stable:
         // include the primary directional, then nearest point lights to camera.
         std::vector<EntityID> selectedShaderLights;
         selectedShaderLights.reserve(kMaxShaderLights);
         EntityID primaryDirectional = INVALID_ENTITY_ID;
         std::vector<std::pair<float, EntityID>> pointCandidates;
         pointCandidates.reserve(m_ScratchLightEntityIds.size());
         for (EntityID lightEntityId : m_ScratchLightEntityIds) {
            auto* ld = scene.GetEntityData(lightEntityId);
            if (!IsPresentationVisible(ld) || !ld->Active || !ld->Light) continue;
            if (ld->Light->Type == LightType::Directional) {
               if (primaryDirectional == INVALID_ENTITY_ID) primaryDirectional = lightEntityId;
               continue;
            }
            const glm::vec3 lp = glm::vec3(ld->Transform.WorldMatrix[3]);
            const float distSq = glm::dot(lp - camPos, lp - camPos);
            pointCandidates.emplace_back(distSq, lightEntityId);
         }
         std::sort(pointCandidates.begin(), pointCandidates.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
         if (primaryDirectional != INVALID_ENTITY_ID) selectedShaderLights.push_back(primaryDirectional);
         for (const auto& c : pointCandidates) {
            if (selectedShaderLights.size() >= static_cast<size_t>(kMaxShaderLights)) break;
            selectedShaderLights.push_back(c.second);
         }

         struct PointShadowCandidate {
            EntityID entity = INVALID_ENTITY_ID;
            int shaderSlot = -1;
            glm::vec3 position = glm::vec3(0.0f);
            float range = 50.0f;
            float cameraDistSq = 0.0f;
         };
         std::vector<PointShadowCandidate> pointShadowCandidates;
         pointShadowCandidates.reserve(kMaxShaderLights);
         for (size_t slot = 0; slot < selectedShaderLights.size(); ++slot) {
            auto* d = scene.GetEntityData(selectedShaderLights[slot]);
            if (!d || !d->Light || d->Light->Type != LightType::Point || !d->Light->PointShadowsEnabled) continue;
            PointShadowCandidate c;
            c.entity = selectedShaderLights[slot];
            c.shaderSlot = static_cast<int>(slot);
            c.position = glm::vec3(d->Transform.WorldMatrix[3]);
            c.range = 50.0f;
            c.cameraDistSq = glm::dot(c.position - camPos, c.position - camPos);
            // Coarse gate: skip lights far from camera where shadows are unlikely visible.
            const float maxShadowDist = shadowFar + c.range;
            if (c.cameraDistSq > (maxShadowDist * maxShadowDist)) {
               continue;
            }
            pointShadowCandidates.push_back(c);
         }
         std::sort(pointShadowCandidates.begin(), pointShadowCandidates.end(),
            [](const PointShadowCandidate& a, const PointShadowCandidate& b) { return a.cameraDistSq < b.cameraDistSq; });

         const glm::vec3 faceDirs[6] = {
            glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f),
            glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f),
            glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, -1.0f)
         };
         const glm::vec3 faceUps[6] = {
            glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f),
            glm::vec3(0.0f, 0.0f, 1.0f),  glm::vec3(0.0f, 0.0f, -1.0f),
            glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)
         };
         const uint64_t pointShadowState = BGFX_STATE_WRITE_Z | BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                                           BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_MSAA | BGFX_STATE_CULL_CW;
         instancerShadowParams.PointStateFlags = pointShadowState;
         const uint16_t faceRes = static_cast<uint16_t>(glm::max(1u, m_PointShadowFaceRes));
         const float pointNear = 0.1f;

         m_PointShadowCount = 0;
         for (int shadowIdx = 0; shadowIdx < kMaxPointShadowLights; ++shadowIdx) {
            m_PointShadowLightSlots[shadowIdx] = -1;
            m_PointShadowLightPosWS[shadowIdx] = glm::vec3(0.0f);
            m_PointShadowRanges[shadowIdx] = 0.0f;
         }

        int acceptedPointLights = 0;
        for (const PointShadowCandidate& candidate : pointShadowCandidates) {
           if (acceptedPointLights >= kMaxPointShadowLights) break;

           // Spatial gate: skip if no caster intersects light influence sphere.
           bool hasPotentialCaster = false;
           for (size_t shadowIndex = 0; shadowIndex < m_ScratchShadowMeshEntityIds.size(); ++shadowIndex) {
               auto* d = shadowIndex < m_ScratchShadowMeshData.size() ? m_ScratchShadowMeshData[shadowIndex] : nullptr;
               if (!IsPresentationVisible(d) || !d->Active || !d->Mesh || !d->Mesh->mesh) continue;
               std::shared_ptr<Mesh> meshPtr = d->Mesh->mesh;
               if (!meshPtr) continue;
               glm::vec3 worldMin(0.0f);
               glm::vec3 worldMax(0.0f);
               if (!tryGetRuntimeShadowBounds(shadowIndex, worldMin, worldMax)) {
                  const glm::vec3 meshCenterLS = (meshPtr->BoundsMin + meshPtr->BoundsMax) * 0.5f;
                  glm::vec3 extents = (meshPtr->BoundsMax - meshPtr->BoundsMin) * 0.5f;
                  float maxScale = glm::max(
                     glm::max(std::abs(d->Transform.Scale.x), std::abs(d->Transform.Scale.y)),
                     std::abs(d->Transform.Scale.z));
                  const glm::vec3 meshCenterWSFallback =
                     glm::vec3(d->Transform.WorldMatrix * glm::vec4(meshCenterLS, 1.0f));
                  const float radiusFallback = glm::length(extents) * glm::max(0.01f, maxScale);
                  worldMin = meshCenterWSFallback - glm::vec3(radiusFallback);
                  worldMax = meshCenterWSFallback + glm::vec3(radiusFallback);
               }
               glm::vec3 meshCenterWS = 0.5f * (worldMin + worldMax);
               float radius = 0.5f * glm::length(worldMax - worldMin);
               const bool isSkinnedCaster = meshPtr->HasSkinning();
               if (isSkinnedCaster && tryGetShadowRagdollBounds(d, meshPtr.get(), worldMin, worldMax)) {
                  meshCenterWS = 0.5f * (worldMin + worldMax);
                  radius = 0.5f * glm::length(worldMax - worldMin);
               }
               float maxReach = candidate.range + radius;
               if (glm::length2(meshCenterWS - candidate.position) <= maxReach * maxReach) {
                  hasPotentialCaster = true;
                  break;
               }
            }
            if (!hasPotentialCaster) {
               hasPotentialCaster = cm::instancer::InstancerSystem::Instance().HasPointShadowCaster(
                  scene,
                  candidate.position,
                  candidate.range,
                  camPos,
                  shadowFar);
            }
            if (!hasPotentialCaster) continue;

            const float pointFar = glm::max(candidate.range, pointNear + 0.01f);
            glm::vec4 pointDepthLightPosRange(candidate.position, pointFar);
            bgfx::setUniform(u_PointShadowLightPosRangeDepth, &pointDepthLightPosRange);

            const int atlasBlockRow = acceptedPointLights * 2;
            for (int face = 0; face < 6; ++face) {
               Renderer::SkinningBindCacheState pointSkinningCache{};
               const uint16_t viewId = static_cast<uint16_t>(m_PointShadowViewId + acceptedPointLights * 6 + face);
               const size_t pointBatchManagerIndex = static_cast<size_t>(acceptedPointLights * 6 + face);
               auto& pointBatchManager = m_PointShadowInstanceManagers[pointBatchManagerIndex];
               const uint16_t tileX = static_cast<uint16_t>((face % 3) * faceRes);
               const uint16_t tileY = static_cast<uint16_t>((atlasBlockRow + (face / 3)) * faceRes);

               float viewMtx[16];
               float projMtx[16];
               bx::mtxLookAt(
                  viewMtx,
                  bx::Vec3{ candidate.position.x, candidate.position.y, candidate.position.z },
                  bx::Vec3{
                     candidate.position.x + faceDirs[face].x,
                     candidate.position.y + faceDirs[face].y,
                     candidate.position.z + faceDirs[face].z
                  },
                  bx::Vec3{ faceUps[face].x, faceUps[face].y, faceUps[face].z }
               );
               bx::mtxProj(projMtx, 90.0f, 1.0f, pointNear, pointFar, shadowCaps ? shadowCaps->homogeneousDepth : false);
               const glm::mat4 pointLightViewProj = glm::make_mat4(projMtx) * glm::make_mat4(viewMtx);
               if (acceptedPointLights < cm::instancer::kPointShadowLightCount) {
                  auto& instancerFace = instancerShadowParams.PointLights[acceptedPointLights].Faces[face];
                  instancerFace.ViewId = viewId;
                  instancerFace.LightViewProj = pointLightViewProj;
               }

               bgfx::setViewFrameBuffer(viewId, m_PointShadowFB);
               bgfx::setViewRect(viewId, tileX, tileY, faceRes, faceRes);
               bgfx::setViewTransform(viewId, id, id);
               bgfx::setViewClear(viewId, BGFX_CLEAR_DEPTH | BGFX_CLEAR_COLOR, 0xffffffff, 1.0f, 0);
               bgfx::touch(viewId);

               for (size_t shadowIndex = 0; shadowIndex < m_ScratchShadowMeshEntityIds.size(); ++shadowIndex) {
                  auto* d = shadowIndex < m_ScratchShadowMeshData.size() ? m_ScratchShadowMeshData[shadowIndex] : nullptr;
                  if (!IsPresentationVisible(d) || !d->Active || !d->Mesh || !d->Mesh->mesh) continue;
                  std::shared_ptr<Mesh> meshPtr = d->Mesh->mesh; if (!meshPtr) continue;
                  if (!HasRenderableVertexSource(*this, d, meshPtr.get())) continue;

                  // Fine spatial gate per light: skip casters outside influence sphere.
                  glm::vec3 worldMin(0.0f);
                  glm::vec3 worldMax(0.0f);
                  if (!tryGetRuntimeShadowBounds(shadowIndex, worldMin, worldMax)) {
                     const glm::vec3 meshCenterLS = (meshPtr->BoundsMin + meshPtr->BoundsMax) * 0.5f;
                     glm::vec3 extents = (meshPtr->BoundsMax - meshPtr->BoundsMin) * 0.5f;
                     float maxScale = glm::max(
                        glm::max(std::abs(d->Transform.Scale.x), std::abs(d->Transform.Scale.y)),
                        std::abs(d->Transform.Scale.z));
                     const glm::vec3 meshCenterWSFallback =
                        glm::vec3(d->Transform.WorldMatrix * glm::vec4(meshCenterLS, 1.0f));
                     const float radiusFallback = glm::length(extents) * glm::max(0.01f, maxScale);
                     worldMin = meshCenterWSFallback - glm::vec3(radiusFallback);
                     worldMax = meshCenterWSFallback + glm::vec3(radiusFallback);
                  }
                  glm::vec3 meshCenterWS = 0.5f * (worldMin + worldMax);
                  float radius = 0.5f * glm::length(worldMax - worldMin);
                  const bool isSkinnedCaster = meshPtr->HasSkinning();
                  if (isSkinnedCaster && tryGetShadowRagdollBounds(d, meshPtr.get(), worldMin, worldMax)) {
                     meshCenterWS = 0.5f * (worldMin + worldMax);
                     radius = 0.5f * glm::length(worldMax - worldMin);
                  }
                  float maxReach = pointFar + radius;
                  if (glm::length2(meshCenterWS - candidate.position) > maxReach * maxReach) continue;

                  float M[16]; memcpy(M, glm::value_ptr(d->Transform.WorldMatrix), sizeof(M));
                  if (isSkinnedCaster &&
                      d->Skinning &&
                      d->Skinning->HasGpuSkinningInstanceRecord()) {
                     bgfx::VertexBufferHandle resolvedVbh = BGFX_INVALID_HANDLE;
                     bgfx::ProgramHandle instancedProgram = BGFX_INVALID_HANDLE;
                     bgfx::ProgramHandle singleProgram = BGFX_INVALID_HANDLE;
                     bool usesGpuMorph = false;
                     bool usesMaterializedSkinning = false;
                     if (resolvePointShadowSkinnedPath(
                            d,
                            m_ScratchShadowMeshEntityIds[shadowIndex],
                            meshPtr.get(),
                            resolvedVbh,
                            instancedProgram,
                            singleProgram,
                            usesGpuMorph,
                            usesMaterializedSkinning)) {
                        if (!bgfx::isValid(instancedProgram)) {
                           // Fall back to the single-draw path below.
                        } else {
                        cm::rendering::SkinnedInstanceData instance{};
                        instance.SetTransform(M);
                        instance.SetMetadata(static_cast<float>(d->Skinning->GpuInstanceAtlasRecordIndex));

                        cm::rendering::InstanceKey key;
                        key.vbh = resolvedVbh;
                        key.ibh = meshPtr->ibh;
                        key.program = instancedProgram;
                        key.indexStart = 0;
                        key.indexCount = meshPtr->numIndices;
                        key.variationHash = 0;

                        auto& batch = pointBatchManager.GetBatch(
                           key,
                           pointShadowState,
                           static_cast<uint16_t>(sizeof(instance)));
                        batch.AddInstance(instance);
                        ++shadowSkinnedBatchedInstances;

                        if (batch.instanceCount == 1u) {
                           const bgfx::VertexBufferHandle vbh = resolvedVbh;
                           const bgfx::IndexBufferHandle ibh = meshPtr->ibh;
                           const bgfx::ProgramHandle submitProgram = instancedProgram;
                           const glm::mat4 faceViewProj = pointLightViewProj;
                           const glm::vec4 lightPosRangeDepth = pointDepthLightPosRange;
                           const char* debugName = usesGpuMorph
                              ? "vs_depth_skinned_morph_instanced+fs_point_shadow_depth"
                              : "vs_depth_skinned_instanced+fs_point_shadow_depth";
                           batch.bindBatch = [this, faceViewProj, lightPosRangeDepth, shadowPsxWorld]() {
                              BindGpuSkinningAtlasGlobals();
                              bgfx::setUniform(u_ShadowPsxWorld, &shadowPsxWorld);
                              bgfx::setUniform(u_LightViewProj, glm::value_ptr(faceViewProj));
                              bgfx::setUniform(u_PointShadowLightPosRangeDepth, &lightPosRangeDepth);
                           };
                           const SkinningComponent* skinning = d->Skinning.get();
                           const bgfx::ProgramHandle fallbackProgram = usesGpuMorph ? s_pointDepthProgSkinnedMorph : s_pointDepthProgSkinned;
                           const char* fallbackDebugName = usesGpuMorph
                              ? "vs_depth_skinned_morph+fs_point_shadow_depth"
                              : "vs_depth_skinned+fs_point_shadow_depth";
                           batch.submitSingle = [this, vbh, ibh, skinning, usesGpuMorph, submitProgram, debugName, fallbackProgram, fallbackDebugName, pointShadowState, faceViewProj, lightPosRangeDepth, shadowPsxWorld](uint16_t fallbackViewId, const uint8_t* instanceBytes) {
                              const auto* inst = reinterpret_cast<const cm::rendering::SkinnedInstanceData*>(instanceBytes);
                              if (!inst) {
                                 return;
                              }

                              if (SubmitSingleSkinnedInstance(
                                 fallbackViewId,
                                 vbh,
                                 ibh,
                                 submitProgram,
                                 *inst,
                                 pointShadowState,
                                 debugName,
                                 [this, faceViewProj, lightPosRangeDepth, shadowPsxWorld]() {
                                    BindGpuSkinningAtlasGlobals();
                                    bgfx::setUniform(u_ShadowPsxWorld, &shadowPsxWorld);
                                    bgfx::setUniform(u_LightViewProj, glm::value_ptr(faceViewProj));
                                    bgfx::setUniform(u_PointShadowLightPosRangeDepth, &lightPosRangeDepth);
                                 })) {
                                 return;
                              }

                              bgfx::setTransform(inst->transform);
                              bgfx::setVertexBuffer(0, vbh);
                              bgfx::setIndexBuffer(ibh);
                              if (usesGpuMorph) {
                                 BindSkinningInstanceRecord(static_cast<uint32_t>(glm::max(inst->metadata[0], 0.0f)));
                              } else if (skinning) {
                                 Renderer::SkinningBindCacheState singleSkinningCache{};
                                 BindSkinningIfChanged(skinning, singleSkinningCache);
                              }
                              bgfx::setUniform(u_ShadowPsxWorld, &shadowPsxWorld);
                              bgfx::setUniform(u_LightViewProj, glm::value_ptr(faceViewProj));
                              bgfx::setUniform(u_PointShadowLightPosRangeDepth, &lightPosRangeDepth);
                              bgfx::setState(pointShadowState);
                              SafeSubmit(fallbackViewId, fallbackProgram, fallbackDebugName);
                           };
                        }
                        continue;
                        }
                     }
                  }

                  bgfx::setTransform(M);
                  bgfx::ProgramHandle pointSkinnedProgram = s_pointDepthProgSkinned;
                  bool usesGpuMorph = false;
                  bool usesMaterializedSkinning = false;
                  if (isSkinnedCaster && d->Skinning) {
                     bgfx::VertexBufferHandle resolvedVbh = BGFX_INVALID_HANDLE;
                     bgfx::ProgramHandle ignoredInstancedProgram = BGFX_INVALID_HANDLE;
                     if (resolvePointShadowSkinnedPath(
                            d,
                            m_ScratchShadowMeshEntityIds[shadowIndex],
                            meshPtr.get(),
                            resolvedVbh,
                            ignoredInstancedProgram,
                            pointSkinnedProgram,
                            usesGpuMorph,
                            usesMaterializedSkinning)) {
                        bgfx::setVertexBuffer(0, resolvedVbh);
                     } else if (meshPtr->Dynamic) {
                        if (!bgfx::isValid(meshPtr->dvbh)) continue;
                        bgfx::setVertexBuffer(0, meshPtr->dvbh, 0, meshPtr->numVertices);
                     } else {
                        bgfx::setVertexBuffer(0, meshPtr->vbh);
                     }
                  } else if (meshPtr->Dynamic) {
                     if (!bgfx::isValid(meshPtr->dvbh)) continue;
                     bgfx::setVertexBuffer(0, meshPtr->dvbh, 0, meshPtr->numVertices);
                  } else {
                     bgfx::setVertexBuffer(0, meshPtr->vbh);
                  }
                  bgfx::setIndexBuffer(meshPtr->ibh);
                  bgfx::setState(pointShadowState);
                  bgfx::setUniform(u_ShadowPsxWorld, &shadowPsxWorld);
                  bgfx::setUniform(u_LightViewProj, glm::value_ptr(pointLightViewProj));
                  if (isSkinnedCaster && d->Skinning) {
                     if (usesGpuMorph) {
                        BindSkinningInstanceRecord(d->Skinning->GpuInstanceAtlasRecordIndex);
                     } else if (!usesMaterializedSkinning) {
                        BindSkinningIfChanged(d->Skinning.get(), pointSkinningCache);
                     }
                     SafeSubmit(
                        viewId,
                        pointSkinnedProgram,
                        usesMaterializedSkinning
                           ? "vs_depth+fs_point_shadow_depth"
                           : (usesGpuMorph ? "vs_depth_skinned_morph+fs_point_shadow_depth" : "vs_depth_skinned+fs_point_shadow_depth"));
                  } else if (!isSkinnedCaster) {
                     SafeSubmit(viewId, s_pointDepthProg, "vs_depth+fs_point_shadow_depth");
                  }
               }

               if (pointBatchManager.EstimateSubmitCount() > 0u) {
                  pointBatchManager.Submit(viewId);
               }
            }

            m_PointShadowLightSlots[acceptedPointLights] = candidate.shaderSlot;
            m_PointShadowLightPosWS[acceptedPointLights] = candidate.position;
            m_PointShadowRanges[acceptedPointLights] = pointFar;
            if (acceptedPointLights < cm::instancer::kPointShadowLightCount) {
               auto& instancerPointLight = instancerShadowParams.PointLights[acceptedPointLights];
               instancerPointLight.Position = candidate.position;
               instancerPointLight.Range = pointFar;
            }
            ++acceptedPointLights;
         }
         m_PointShadowCount = acceptedPointLights;
         instancerShadowParams.PointLightCount = acceptedPointLights;
      }
   }

   cm::instancer::InstancerSystem::Instance().RenderShadows(
      scene,
      instancerShadowParams,
      &instancerShadowStats);
   shadowCascadeSubmits += instancerShadowStats.DirectionalSubmits;
   for (int ci = 0; ci < cm::instancer::kShadowCascadeCount; ++ci) {
      cascadeSubmitCounts[ci] += instancerShadowStats.CascadeSubmits[ci];
   }

   m_ShadowCascadeSubmitCounts = cascadeSubmitCounts;
   auto& profiler = Profiler::Get();
   profiler.SetCounter("Render/ShadowCastersScanned", shadowCastersScanned);
   profiler.SetCounter("Render/ShadowCascadeCulled", shadowCascadeCulled);
   profiler.SetCounter("Render/ShadowSkinnedCascadeSkipped", shadowSkinnedCascadeSkipped);
   profiler.SetCounter("Render/ShadowSkinnedOffscreenSkipped", shadowSkinnedOffscreenSkipped);
   profiler.SetCounter("Render/ShadowMainViewCulled", shadowMainViewCulled);
   profiler.SetCounter("Render/ShadowSkinnedBatchedInstances", shadowSkinnedBatchedInstances);
   profiler.SetCounter("Render/ShadowCascadeSubmits", shadowCascadeSubmits);
   profiler.SetCounter("Render/InstancerShadowDirectionalSubmits", instancerShadowStats.DirectionalSubmits);
   profiler.SetCounter("Render/InstancerShadowPointSubmits", instancerShadowStats.PointSubmits);
}

void Renderer::DrawShadowDebugOverlay(const Camera* camera)
{
   if (!m_ShowShadowDebugOverlay) return;
   if (!m_ShadowContextEnabled) return;
   if (!bgfx::isValid(m_ShadowDebugProgram) || !bgfx::isValid(s_ShadowDebug) || !bgfx::isValid(u_ShadowDebugParams)) return;
   if (!bgfx::isValid(m_ShadowDebugColor) || !bgfx::isValid(m_FullscreenVB) || !bgfx::isValid(m_FullscreenIB)) return;
   if (m_Width == 0 || m_Height == 0) return;

   const uint16_t kView_ShadowDebug = 219;
   const uint16_t pad = 12;
   const uint16_t overlaySize = (uint16_t)glm::clamp<int>(int(glm::min(m_Width, m_Height) / 4u), 140, 360);
   const uint16_t x = (m_Width > overlaySize + pad) ? (uint16_t)(m_Width - overlaySize - pad) : 0;
   const uint16_t y = pad;

   bgfx::setViewRect(kView_ShadowDebug, x, y, overlaySize, overlaySize);
   if (m_RenderToOffscreen) {
      bgfx::setViewFrameBuffer(kView_ShadowDebug, m_SceneFrameBuffer);
   } else {
      bgfx::setViewFrameBuffer(kView_ShadowDebug, BGFX_INVALID_HANDLE);
   }
   bgfx::setViewClear(kView_ShadowDebug, BGFX_CLEAR_NONE, 0, 1.0f, 0);

   float id[16]; bx::mtxIdentity(id);
   bgfx::setTransform(id);
   bgfx::setVertexBuffer(0, m_FullscreenVB);
   bgfx::setIndexBuffer(m_FullscreenIB);
   bgfx::setTexture(0, s_ShadowDebug, m_ShadowDebugColor);

   // Pick a representative cascade using nearest visible mesh view-depth.
   // This matches cascade split space (camera-forward depth), not radial distance.
   float testDepth = 0.0f;
   if (camera && m_ShadowContextScene) {
      const glm::mat4 view = camera->GetViewMatrix();
      float nearest = std::numeric_limits<float>::max();
      for (EntityID eid : m_ScratchVisibleMeshIds) {
         auto* d = m_ShadowContextScene->GetEntityData(eid);
         if (!IsPresentationVisible(d) || !d->Active || !d->Mesh || !d->Mesh->mesh) continue;
         const glm::vec3 wp = glm::vec3(d->Transform.WorldMatrix[3]);
         const glm::vec4 vp = view * glm::vec4(wp, 1.0f);
         const float depth = glm::max(-vp.z, 0.0f);
         nearest = glm::min(nearest, depth);
      }
      if (nearest < std::numeric_limits<float>::max()) {
         testDepth = nearest;
      }
   }
   int selectedCascade = 0;
   if (m_CascadeCount > 1) {
      if (testDepth > m_CascadeSplits[0]) selectedCascade = 1;
      if (testDepth > m_CascadeSplits[1]) selectedCascade = 2;
      if (testDepth > m_CascadeSplits[2]) selectedCascade = 3;
      selectedCascade = glm::clamp(selectedCascade, 0, glm::max(0, m_CascadeCount - 1));
   }

   const bgfx::Caps* caps = bgfx::getCaps();
   const bool supportsShadowCompare = caps && 0 != (caps->supported & BGFX_CAPS_TEXTURE_COMPARE_LEQUAL);
   const bool shadowFbValid = bgfx::isValid(m_ShadowFB) && bgfx::isValid(m_ShadowDepth);
   const int cascadeCountDbg = glm::clamp(m_CascadeCount, 0, 4);
   glm::vec4 dbgParams(
      (caps && caps->originBottomLeft) ? 1.0f : 0.0f,
      (float)selectedCascade,
      2.0f,
      0.0f
   );
   bgfx::setUniform(u_ShadowDebugParams, &dbgParams);
   bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
   bgfx::submit(kView_ShadowDebug, m_ShadowDebugProgram);

   bgfx::dbgTextPrintf(1, 1, 0x0e, "Shadow Atlas %ux%u", m_ShadowRes, m_ShadowRes);
   bgfx::dbgTextPrintf(1, 2, 0x0f, "CSM count:%d  splits: %.1f %.1f %.1f", cascadeCountDbg, m_CascadeSplits[0], m_CascadeSplits[1], m_CascadeSplits[2]);
   bgfx::dbgTextPrintf(1, 3, 0x0f, "Debug cascade: %d  testDepth: %.1f", selectedCascade, testDepth);
   bgfx::dbgTextPrintf(1, 4, 0x0f, "ShadowCaps compare:%d  fb:%d  ctx:%d", supportsShadowCompare ? 1 : 0, shadowFbValid ? 1 : 0, m_ShadowContextEnabled ? 1 : 0);
   bgfx::dbgTextPrintf(1, 5, 0x0f, "Submits c0:%u c1:%u c2:%u c3:%u", m_ShadowCascadeSubmitCounts[0], m_ShadowCascadeSubmitCounts[1], m_ShadowCascadeSubmitCounts[2], m_ShadowCascadeSubmitCounts[3]);
}

void Renderer::BindShadowUniforms()
{
   const Scene* contextScene = m_ShadowContextScene ? m_ShadowContextScene : Scene::CurrentScene;
   Environment env = contextScene ? contextScene->GetEnvironment() : Environment{};
   const bgfx::Caps* caps = bgfx::getCaps();
   const bool supportsShadowCompare = caps && 0 != (caps->supported & BGFX_CAPS_TEXTURE_COMPARE_LEQUAL);
   const bool shadowsEnabled = m_ShadowContextEnabled && env.ShadowsEnabled && bgfx::isValid(m_ShadowDepth) && supportsShadowCompare;
   const bool pointShadowsEnabled = m_ShadowContextEnabled && env.ShadowsEnabled &&
                                    (m_PointShadowCount > 0) &&
                                    bgfx::isValid(m_PointShadowColor);

   if (!shadowsEnabled) {
      env.ShadowStrength = 0.0f;
      env.ShadowSamples = 0;
      env.ShadowSoftness = 0.0f;
   }

   // Upload uniforms regardless so shaders can branch off strength / texel size
   int samples = shadowsEnabled ? env.ShadowSamples : 0;
   if (samples != 1 && samples != 4 && samples != 9 && samples != 16) {
      samples = shadowsEnabled ? 9 : 0;
   }
   const float originBottomLeft = (caps && caps->originBottomLeft) ? 1.0f : 0.0f;
   glm::vec4 texel(0.0f, 0.0f, (float)samples, originBottomLeft);
   if (shadowsEnabled && m_ShadowRes > 0) {
      texel.x = 1.0f / (float)m_ShadowRes;
      texel.y = 1.0f / (float)m_ShadowRes;
   }

   glm::mat4 lightViewProj = shadowsEnabled ? m_LightViewProj : glm::mat4(1.0f);
   bgfx::setUniform(u_LightViewProj, glm::value_ptr(lightViewProj));
   if (shadowsEnabled) {
      // Upload cascade matrices and atlas transform
      bgfx::setUniform(u_LightViewProjCSM, m_CascadeMatrices.data(), 4);
      bgfx::setUniform(u_CascadeScaleBias, m_CascadeScaleBias.data(), 4);
      glm::vec4 splits(0.0f);
      splits.x = (m_CascadeCount > 0) ? m_CascadeSplits[0] : 0.0f;
      splits.y = (m_CascadeCount > 1) ? m_CascadeSplits[1] : 0.0f;
      splits.z = (m_CascadeCount > 2) ? m_CascadeSplits[2] : 0.0f;
      splits.w = (float)glm::max(1, m_CascadeCount);
      bgfx::setUniform(u_CascadeSplits, &splits);
   } else {
      static const std::array<glm::mat4, 4> kIdentityCSM = {
         glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f)
      };
      static const std::array<glm::vec4, 4> kZeroScaleBias = {
         glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f)
      };
      bgfx::setUniform(u_LightViewProjCSM, kIdentityCSM.data(), 4);
      bgfx::setUniform(u_CascadeScaleBias, kZeroScaleBias.data(), 4);
      glm::vec4 splits(0.0f);
      bgfx::setUniform(u_CascadeSplits, &splits);
   }
   bgfx::setUniform(u_ShadowTexelSize, &texel);
   glm::vec4 sp(env.ShadowBias, env.ShadowNormalBias, env.ShadowSoftness, env.ShadowStrength);
   bgfx::setUniform(u_ShadowParams, &sp);
   glm::vec4 receiveDefault(1.0f, 0.0f, 0.0f, 0.0f);
   bgfx::setUniform(u_ShadowReceive, &receiveDefault);
   glm::vec4 ldir(shadowsEnabled ? m_ShadowDirWS : glm::vec3(0.0f), 0.0f);
   bgfx::setUniform(u_ShadowLightDir, &ldir);
   std::array<glm::vec4, kMaxPointShadowLights> pointMeta{};
   std::array<glm::vec4, kMaxPointShadowLights> pointPos{};
   for (int i = 0; i < kMaxPointShadowLights; ++i) {
      const bool active = pointShadowsEnabled && i < m_PointShadowCount && m_PointShadowLightSlots[i] >= 0;
      pointMeta[i] = glm::vec4(
         active ? 1.0f : 0.0f,
         active ? static_cast<float>(m_PointShadowLightSlots[i]) : -1.0f,
         active ? m_PointShadowRanges[i] : 0.0f,
         env.ShadowBias
      );
      pointPos[i] = glm::vec4(active ? m_PointShadowLightPosWS[i] : glm::vec3(0.0f), 0.0f);
   }
   bgfx::setUniform(u_PointShadowMeta, pointMeta.data(), kMaxPointShadowLights);
   bgfx::setUniform(u_PointShadowLightPos, pointPos.data(), kMaxPointShadowLights);
   glm::vec4 pointAtlas(3.0f, static_cast<float>(2 * kMaxPointShadowLights), 0.0f, 0.0f);
   if (pointShadowsEnabled && m_PointShadowAtlasWidth > 0u && m_PointShadowAtlasHeight > 0u) {
      pointAtlas.z = 1.0f / static_cast<float>(m_PointShadowAtlasWidth);
      pointAtlas.w = 1.0f / static_cast<float>(m_PointShadowAtlasHeight);
   }
   bgfx::setUniform(u_PointShadowAtlas, &pointAtlas);

   const uint32_t smpCompare = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_COMPARE_LEQUAL;
   if (shadowsEnabled && bgfx::isValid(m_ShadowDepth)) {
      bgfx::setTexture(7, s_ShadowMap, m_ShadowDepth, smpCompare);
   } else {
      // Fallback: bind a tiny dummy depth texture so slot 7 always has a sampler bound
      // Must use depth format (D16) to support comparison sampling, not R8
      static bgfx::TextureHandle s_dummy = BGFX_INVALID_HANDLE;
      if (!bgfx::isValid(s_dummy)) {
         const uint32_t flags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
         // D16 format requires 2 bytes per pixel (16 bits)
         const bgfx::Memory* mem = bgfx::alloc(2);
         // Set to maximum depth (0xFFFF) so all shadow tests pass (no shadows)
         mem->data[0] = 0xFF;
         mem->data[1] = 0xFF;
         s_dummy = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::D16, flags, mem);
      }
      bgfx::setTexture(7, s_ShadowMap, s_dummy, smpCompare);
   }

   const uint32_t pointShadowSamplerFlags = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT;
   if (pointShadowsEnabled && bgfx::isValid(m_PointShadowColor)) {
      bgfx::setTexture(8, s_PointShadowMap, m_PointShadowColor, pointShadowSamplerFlags);
   } else {
      static bgfx::TextureHandle s_pointDummy = BGFX_INVALID_HANDLE;
      if (!bgfx::isValid(s_pointDummy)) {
         const uint8_t white[4] = { 255, 255, 255, 255 };
         const bgfx::Memory* mem = bgfx::copy(white, sizeof(white));
         s_pointDummy = bgfx::createTexture2D(
            1, 1, false, 1, bgfx::TextureFormat::RGBA8,
            BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, mem
         );
      }
      bgfx::setTexture(8, s_PointShadowMap, s_pointDummy, pointShadowSamplerFlags);
   }

   const uint32_t skyboxSamplerFlags =
      BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_W_CLAMP |
      BGFX_SAMPLER_MIN_ANISOTROPIC | BGFX_SAMPLER_MAG_ANISOTROPIC;
   if (env.UseSkybox && env.SkyboxTexture && env.SkyboxTexture->IsValid()) {
      bgfx::setTexture(9, s_Skybox, env.SkyboxTexture->GetHandle(), skyboxSamplerFlags);
   } else {
      bgfx::TextureHandle s_skyDummy = GetDummySkyboxTexture();
      if (bgfx::isValid(s_skyDummy)) {
         bgfx::setTexture(9, s_Skybox, s_skyDummy, skyboxSamplerFlags);
      }
   }
}

void Renderer::BindLightingUniforms()
{
   // Re-upload cached lighting uniforms for external systems
   // This ensures lights and environment are set for each draw call
   Camera* cam = GetCamera();
   if (cam) {
      glm::vec4 camPos(cam->GetPosition(), 1.0f);
      bgfx::setUniform(u_cameraPos, &camPos);
   }
   UploadLightsToShader(m_ScratchLights);
   UploadEnvironmentToShader(m_CachedEnvironment, m_CachedEditorLightingOverride);
}

// Helper function to compute and set normal matrix uniform
void Renderer::SetNormalMatrixUniform(const float* transform) {
   // Compute transpose(inverse(mat3(model))) to handle non-uniform scaling correctly
   glm::mat4 modelMtx = glm::make_mat4(transform);
   glm::mat3 n3 = glm::transpose(glm::inverse(glm::mat3(modelMtx)));
   glm::mat4 normalMat4(1.0f);
   normalMat4[0] = glm::vec4(n3[0], 0.0f);
   normalMat4[1] = glm::vec4(n3[1], 0.0f);
   normalMat4[2] = glm::vec4(n3[2], 0.0f);
   bgfx::setUniform(u_normalMat, glm::value_ptr(normalMat4));
}

glm::vec3 Renderer::ComputePrimarySunDirection(Scene& scene) const
{
   glm::vec3 sunDir(0.0f, 1.0f, 0.0f);
   for (auto& entity : scene.GetEntities()) {
      auto* data = scene.GetEntityData(entity.GetID());
      if (!data || !data->Light || !IsPresentationVisible(data) || !data->Active) continue;
      if (data->Light->Type == LightType::Directional) {
         sunDir = LightDirectionFromTransform(data->Transform);
         break;
      }
   }
   return sunDir;
}

void Renderer::SetEditorLightingOverride(bool disabled)
{
   m_EditorLightingOverride = disabled;
}

bool Renderer::ShouldApplyEditorLightingOverride(const Scene& scene) const
{
#ifndef CLAYMORE_RUNTIME
   if (!m_EditorLightingOverride)
      return false;
   if (!Application::HasInstance() || !Application::Get().m_RunEditorUI)
      return false;
   if (scene.m_IsPlaying)
      return false;
   return true;
#else
   // No editor lighting override in runtime builds
   (void)scene;
   return false;
#endif
}

// ============================================================================
// Terrain Texture Array System
// ============================================================================

void Renderer::DestroyTerrainTextureArrays()
{
   if (bgfx::isValid(m_TerrainAlbedoArrayTex))
   {
      bgfx::destroy(m_TerrainAlbedoArrayTex);
      m_TerrainAlbedoArrayTex = BGFX_INVALID_HANDLE;
   }
   if (bgfx::isValid(m_TerrainNormalArrayTex))
   {
      bgfx::destroy(m_TerrainNormalArrayTex);
      m_TerrainNormalArrayTex = BGFX_INVALID_HANDLE;
   }
   m_TerrainArrayResolution = 0;
   m_TerrainArrayLayerCount = 0;
}

namespace
{
   // CPU image resize with selectable filter
   void ResizeImageNearest(const uint8_t* src, int srcW, int srcH,
                           uint8_t* dst, int dstW, int dstH)
   {
      for (int y = 0; y < dstH; ++y)
      {
         int srcY = y * srcH / dstH;
         for (int x = 0; x < dstW; ++x)
         {
            int srcX = x * srcW / dstW;
            const uint8_t* sp = src + (srcY * srcW + srcX) * 4;
            uint8_t* dp = dst + (y * dstW + x) * 4;
            dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = sp[3];
         }
      }
   }

   void ResizeImageBilinear(const uint8_t* src, int srcW, int srcH,
                            uint8_t* dst, int dstW, int dstH)
   {
      float xRatio = static_cast<float>(srcW) / dstW;
      float yRatio = static_cast<float>(srcH) / dstH;

      for (int y = 0; y < dstH; ++y)
      {
         float srcYf = y * yRatio;
         int y0 = static_cast<int>(srcYf);
         int y1 = std::min(y0 + 1, srcH - 1);
         float fy = srcYf - y0;

         for (int x = 0; x < dstW; ++x)
         {
            float srcXf = x * xRatio;
            int x0 = static_cast<int>(srcXf);
            int x1 = std::min(x0 + 1, srcW - 1);
            float fx = srcXf - x0;

            const uint8_t* p00 = src + (y0 * srcW + x0) * 4;
            const uint8_t* p10 = src + (y0 * srcW + x1) * 4;
            const uint8_t* p01 = src + (y1 * srcW + x0) * 4;
            const uint8_t* p11 = src + (y1 * srcW + x1) * 4;

            uint8_t* dp = dst + (y * dstW + x) * 4;
            for (int c = 0; c < 4; ++c)
            {
               float v = p00[c] * (1 - fx) * (1 - fy)
                       + p10[c] * fx * (1 - fy)
                       + p01[c] * (1 - fx) * fy
                       + p11[c] * fx * fy;
               dp[c] = static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f));
            }
         }
      }
   }

   // Bicubic (Catmull-Rom) kernel
   inline float CubicWeight(float x)
   {
      const float a = -0.5f;
      x = std::abs(x);
      if (x <= 1.0f)
         return (a + 2.0f) * x * x * x - (a + 3.0f) * x * x + 1.0f;
      else if (x < 2.0f)
         return a * x * x * x - 5.0f * a * x * x + 8.0f * a * x - 4.0f * a;
      return 0.0f;
   }

   void ResizeImageBicubic(const uint8_t* src, int srcW, int srcH,
                           uint8_t* dst, int dstW, int dstH)
   {
      float xRatio = static_cast<float>(srcW) / dstW;
      float yRatio = static_cast<float>(srcH) / dstH;

      for (int y = 0; y < dstH; ++y)
      {
         float srcYf = y * yRatio;
         int yBase = static_cast<int>(srcYf);
         float fy = srcYf - yBase;

         for (int x = 0; x < dstW; ++x)
         {
            float srcXf = x * xRatio;
            int xBase = static_cast<int>(srcXf);
            float fx = srcXf - xBase;

            float accum[4] = { 0, 0, 0, 0 };
            float wSum = 0;

            for (int j = -1; j <= 2; ++j)
            {
               int sampleY = std::clamp(yBase + j, 0, srcH - 1);
               float wy = CubicWeight(fy - j);

               for (int i = -1; i <= 2; ++i)
               {
                  int sampleX = std::clamp(xBase + i, 0, srcW - 1);
                  float wx = CubicWeight(fx - i);
                  float w = wx * wy;

                  const uint8_t* sp = src + (sampleY * srcW + sampleX) * 4;
                  for (int c = 0; c < 4; ++c)
                     accum[c] += sp[c] * w;
                  wSum += w;
               }
            }

            uint8_t* dp = dst + (y * dstW + x) * 4;
            for (int c = 0; c < 4; ++c)
               dp[c] = static_cast<uint8_t>(std::clamp(accum[c] / wSum, 0.0f, 255.0f));
         }
      }
   }

   void ResizeImage(const uint8_t* src, int srcW, int srcH,
                    uint8_t* dst, int dstW, int dstH,
                    TerrainTextureFilter filter)
   {
      switch (filter)
      {
      case TerrainTextureFilter::Nearest:
         ResizeImageNearest(src, srcW, srcH, dst, dstW, dstH);
         break;
      case TerrainTextureFilter::Bicubic:
         ResizeImageBicubic(src, srcW, srcH, dst, dstW, dstH);
         break;
      case TerrainTextureFilter::Bilinear:
      default:
         ResizeImageBilinear(src, srcW, srcH, dst, dstW, dstH);
         break;
      }
   }

   // Load image from path, resize to target resolution, return RGBA pixels
   // Returns empty vector on failure
   std::vector<uint8_t> LoadAndResizeImage(const std::string& path, int targetRes,
                                           TerrainTextureFilter filter)
   {
      stbi_set_flip_vertically_on_load(false);

      int w = 0, h = 0, c = 0;
      std::vector<uint8_t> fileData;
      stbi_uc* pixels = nullptr;

      // Try VFS first
      if (VFS::Get() && VFS::Get()->ReadFile(path, fileData))
         pixels = stbi_load_from_memory(fileData.data(), static_cast<int>(fileData.size()), &w, &h, &c, 4);

      // Fallback to FileSystem
      if (!pixels && FileSystem::Instance().ReadFile(path, fileData))
         pixels = stbi_load_from_memory(fileData.data(), static_cast<int>(fileData.size()), &w, &h, &c, 4);

#ifndef CLAYMORE_RUNTIME
      // Direct disk fallback for editor only
      if (!pixels)
         pixels = stbi_load(path.c_str(), &w, &h, &c, 4);
#endif

      if (!pixels)
         return {};

      std::vector<uint8_t> result(targetRes * targetRes * 4);
      if (w == targetRes && h == targetRes)
      {
         std::memcpy(result.data(), pixels, result.size());
      }
      else
      {
         ResizeImage(pixels, w, h, result.data(), targetRes, targetRes, filter);
      }

      stbi_image_free(pixels);
      return result;
   }

} // anonymous namespace

void Renderer::BuildTerrainTextureArrays(TerrainComponent& terrain)
{
   // Early-out if no layers
   if (terrain.Layers.empty())
   {
      DestroyTerrainTextureArrays();
      terrain.LayerTextureArraysDirty = false;
      return;
   }

   const uint32_t targetRes = terrain.LayerTextureResolution;
   const uint32_t layerCount = static_cast<uint32_t>(terrain.Layers.size());
   const TerrainTextureFilter filter = terrain.LayerResizeFilter;

   // Destroy old arrays
   DestroyTerrainTextureArrays();

   // Prepare storage for all layers
   const size_t sliceSize = targetRes * targetRes * 4;
   std::vector<uint8_t> albedoData(sliceSize * layerCount);
   std::vector<uint8_t> normalData(sliceSize * layerCount);

   // Default flat normal (0.5, 0.5, 1.0) -> (128, 128, 255, 255)
   std::vector<uint8_t> defaultNormal(sliceSize);
   for (size_t i = 0; i < sliceSize; i += 4)
   {
      defaultNormal[i + 0] = 128;
      defaultNormal[i + 1] = 128;
      defaultNormal[i + 2] = 255;
      defaultNormal[i + 3] = 255;
   }

   // Default white albedo
   std::vector<uint8_t> defaultAlbedo(sliceSize);
   for (size_t i = 0; i < sliceSize; i += 4)
   {
      defaultAlbedo[i + 0] = 255;
      defaultAlbedo[i + 1] = 255;
      defaultAlbedo[i + 2] = 255;
      defaultAlbedo[i + 3] = 255;
   }

   // Load and resize each layer in parallel
   std::vector<std::future<void>> futures;
   futures.reserve(layerCount * 2);

   for (uint32_t i = 0; i < layerCount; ++i)
   {
      const auto& layer = terrain.Layers[i];
      uint8_t* albedoDst = albedoData.data() + i * sliceSize;
      uint8_t* normalDst = normalData.data() + i * sliceSize;

      // Albedo loading
      futures.push_back(std::async(std::launch::async, [&layer, albedoDst, &defaultAlbedo, targetRes, filter, sliceSize]() {
         if (!layer.AlbedoPath.empty())
         {
            auto pixels = LoadAndResizeImage(layer.AlbedoPath, targetRes, filter);
            if (!pixels.empty())
            {
               std::memcpy(albedoDst, pixels.data(), sliceSize);
               return;
            }
         }
         // Use placeholder color instead of white
         for (size_t j = 0; j < sliceSize; j += 4)
         {
            albedoDst[j + 0] = static_cast<uint8_t>(layer.PlaceholderColor.r * 255.0f);
            albedoDst[j + 1] = static_cast<uint8_t>(layer.PlaceholderColor.g * 255.0f);
            albedoDst[j + 2] = static_cast<uint8_t>(layer.PlaceholderColor.b * 255.0f);
            albedoDst[j + 3] = 255;
         }
      }));

      // Normal loading
      futures.push_back(std::async(std::launch::async, [&layer, normalDst, &defaultNormal, targetRes, filter, sliceSize]() {
         if (!layer.NormalPath.empty())
         {
            auto pixels = LoadAndResizeImage(layer.NormalPath, targetRes, filter);
            if (!pixels.empty())
            {
               std::memcpy(normalDst, pixels.data(), sliceSize);
               return;
            }
         }
         std::memcpy(normalDst, defaultNormal.data(), sliceSize);
      }));
   }

   // Wait for all loads to complete
   for (auto& f : futures)
      f.get();

   // Create texture arrays
   // Note: BGFX Texture2DArray uses numLayers parameter in createTexture2D
   const uint16_t res16 = static_cast<uint16_t>(targetRes);
   const uint16_t layers16 = static_cast<uint16_t>(layerCount);

   // Albedo array (sRGB for color textures)
   {
      const bgfx::Memory* mem = bgfx::copy(albedoData.data(), static_cast<uint32_t>(albedoData.size()));
      m_TerrainAlbedoArrayTex = bgfx::createTexture2D(
         res16, res16,
         false,           // hasMips
         layers16,        // numLayers (makes it an array)
         bgfx::TextureFormat::RGBA8,
         BGFX_TEXTURE_SRGB,
         mem
      );
   }

   // Normal array (linear for data textures)
   {
      const bgfx::Memory* mem = bgfx::copy(normalData.data(), static_cast<uint32_t>(normalData.size()));
      m_TerrainNormalArrayTex = bgfx::createTexture2D(
         res16, res16,
         false,           // hasMips
         layers16,        // numLayers (makes it an array)
         bgfx::TextureFormat::RGBA8,
         BGFX_TEXTURE_NONE,
         mem
      );
   }

   m_TerrainArrayResolution = targetRes;
   m_TerrainArrayLayerCount = layerCount;
   terrain.LayerTextureArraysDirty = false;

   std::cout << "[Terrain] Built texture arrays: " << layerCount << " layers @ " << targetRes << "x" << targetRes << "\n";
}

