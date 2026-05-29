#pragma once

#include <glm/glm.hpp>
#include <Jolt/Jolt.h>  // Must be included before other Jolt headers
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <string>
#include <vector>
#include "core/ecs/Entity.h"

// Forward declarations to avoid circular dependency with Components.h
struct TerrainComponent;
struct TransformComponent;

class Terrain
{
public:
   static void EnsureChunkLayout(TerrainComponent& terrain);
   static glm::vec2 GetCellSize(const TerrainComponent& terrain);
   static float SampleHeightWorld(const TerrainComponent& terrain, float localX, float localZ);
   static glm::vec3 SampleNormal(const TerrainComponent& terrain, float localX, float localZ);
   static float SampleHoleMaskNormalized(const TerrainComponent& terrain, float localX, float localZ);
   static bool IsHoleAtLocal(const TerrainComponent& terrain, float localX, float localZ, float threshold = 0.5f);
   static bool Raycast(const TransformComponent& transform,
                       TerrainComponent& terrain,
                       const glm::vec3& rayOriginWorld,
                       const glm::vec3& rayDirWorld,
                       glm::vec3* outWorldPos = nullptr,
                       glm::vec3* outWorldNormal = nullptr,
                       glm::vec3* outLocalPos = nullptr,
                       glm::vec3* outLocalNormal = nullptr,
                       bool ignoreHoles = false);
   static void MarkHeightRegionDirty(TerrainComponent& terrain, const glm::ivec2& minCell, const glm::ivec2& maxCell);
   static void MarkSplatRegionDirty(TerrainComponent& terrain, const glm::ivec2& minCell, const glm::ivec2& maxCell);
   static void MarkHoleRegionDirty(TerrainComponent& terrain, const glm::ivec2& minCell, const glm::ivec2& maxCell);
   static void PrepareForRendering(TerrainComponent& terrain);
   
   // LOD System
   // Update chunk bounds after height data changes
   static void UpdateChunkBoundsFromHeightChange(TerrainComponent& terrain);
   // Update per-frame LOD selection based on camera distance
   static void UpdateLOD(TerrainComponent& terrain, const glm::vec3& cameraPos, const glm::mat4& worldMatrix);
   
   // Physics: Build heightfield collision shape from terrain height data
   // Returns a Jolt HeightFieldShape that matches the rendered terrain geometry
   static JPH::RefConst<JPH::Shape> BuildHeightFieldShape(const TerrainComponent& terrain);
   
   // Physics: Create or update the terrain's physics body
   // Should be called when terrain is created or when height data changes
   static void UpdatePhysicsBody(TerrainComponent& terrain, const glm::mat4& worldTransform, EntityID ownerEntity);
   
   // Physics: Destroy the terrain's physics body
   static void DestroyPhysicsBody(TerrainComponent& terrain);

   // Binary asset serialization
   static bool SaveTerrainAsset(TerrainComponent& terrain, bool forceWrite = false);
   static bool LoadTerrainAsset(const std::string& assetPath, TerrainComponent& terrain);
   
   // Heightmap image import/export (16-bit grayscale PNG)
   static bool ExportHeightmap(const TerrainComponent& terrain, const std::string& filePath);
   static bool ImportHeightmap(TerrainComponent& terrain, const std::string& filePath);
   static bool LoadHeightmapTextureSamples(const std::string& filePath, std::vector<float>& outSamples, int& outWidth, int& outHeight);
   
   // Splatmap image import/export (RGBA PNG - each channel is a layer weight)
   static bool ExportSplatmap(const TerrainComponent& terrain, const std::string& filePath);
   static bool ImportSplatmap(TerrainComponent& terrain, const std::string& filePath);
   
   // Clipmap system cleanup - call when terrain is destroyed or clipmaps are disabled
   // This destroys the runtime clipmap system stored in terrain.ClipmapSystem
   static void DestroyClipmapSystem(TerrainComponent& terrain);
   
   // Chunk streaming cleanup - call when streaming is disabled or terrain is destroyed
   // This destroys the runtime streaming system stored in terrain.ChunkStreamingSystem
   static void DestroyStreamingSystem(TerrainComponent& terrain);
};
