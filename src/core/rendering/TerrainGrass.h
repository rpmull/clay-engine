#pragma once

// CM_DEBUG_GRASS requires ImGui which is editor-only.
// Since TerrainGrass.cpp is compiled in ClaymoreCore (which doesn't have ImGui),
// the debug window function cannot be compiled there. Disable for now.
// TODO: Move grass debug UI to an editor-specific file if needed.
#ifndef CM_DEBUG_GRASS
#define CM_DEBUG_GRASS 0
#endif

#include <glm/glm.hpp>
#include <bgfx/bgfx.h>
#include <vector>
#include <cstdint>

struct TerrainComponent;
class Scene;

namespace TerrainGrass
{
   enum class GrassGenerationMode : uint8_t
      {
      Compute = 0,
      CpuDensity = 1
      };

   void InitializeRendererResources();
   void ShutdownRendererResources();

   void SetGenerationMode(GrassGenerationMode mode);
   GrassGenerationMode GetGenerationMode();
   const char* GetGenerationModeLabel(GrassGenerationMode mode);

   // Ensure textures/meshes referenced by layers are resident.
   void SyncLayerResources(TerrainComponent& terrain);

   // Rebuild per-chunk instance data if dirty.
   void EnsureChunksUpToDate(TerrainComponent& terrain);

   // Apply brush painting to a specific layer (deltaSign = +/-1).
   void ApplyPaint(TerrainComponent& terrain, size_t layerIndex, const glm::vec2& centerXZ, float radius, float deltaSign, float falloffPower);

   // Render all grass for the given terrain using provided view/projection data.
   void Render(const TerrainComponent& terrain,
      const glm::mat4& worldTransform,
      uint16_t viewId,
      const glm::vec3& cameraPos,
      const glm::mat4& viewMatrix,
      const glm::mat4& projMatrix,
      bool enableFrustumCulling);

   // Helper to force rebuild of runtime data.
   void MarkAllDirty(TerrainComponent& terrain);

   // Grass Deformation System
   // Initializes the deformation texture for a terrain
   void InitializeDeformation(TerrainComponent& terrain);
   
   // Destroys the deformation texture
   void DestroyDeformation(TerrainComponent& terrain);
   
   // Updates deformation: applies decay and uploads texture if dirty
   void UpdateDeformation(TerrainComponent& terrain, float deltaTime);
   
   // Applies a deformer at the given world position
   void ApplyDeformer(TerrainComponent& terrain,
                      const glm::mat4& terrainWorldMatrix,
                      const glm::vec3& deformerWorldPos,
                      const glm::vec3& velocity,
                      float radius,
                      float strength);
   
   // Returns the deformation texture handle (for shader binding)
   bgfx::TextureHandle GetDeformationTexture(const TerrainComponent& terrain);

   // Updates grass deformation for all terrains in the scene
   // Call this once per frame before rendering
   // - Collects all GrassDeformerComponents and applies their deformation
   // - Updates velocity tracking for movement-based deformation
   // - Applies time-based decay to existing deformations
   void UpdateDeformationSystem(Scene& scene, float deltaTime);

#if CM_DEBUG_GRASS
   // Runtime debug hooks.
   void SetDebugOverlayEnabled(bool enabled);
   bool IsDebugOverlayEnabled();
   void RenderDebugWindow();
   void RequestDebugDump();
#endif
}

