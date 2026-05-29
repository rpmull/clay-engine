#include "TerrainPainter.h"
#include "core/rendering/Renderer.h"
#include "core/rendering/TerrainGrass.h"
#ifndef CLAYMORE_CORE
#include <imgui.h>
#endif
#include "core/ecs/Components.h"
#include "core/ecs/EntityData.h"
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <bx/math.h>
#include <limits>
#include <cmath>
#include <algorithm>
#include <iostream>
#include "core/rendering/Terrain.h"
#include "editor/rendering/Picking.h"
#include "editor/undo/EditorSceneUndoStack.h"
#include "core/platform/KeyCodes.h"

namespace
{
   struct TerrainHit
   {
      glm::vec3 LocalPosition{ 0.0f };
      glm::vec3 WorldPosition{ 0.0f };
      glm::vec3 LocalNormal{ 0.0f, 1.0f, 0.0f };
      glm::vec3 WorldNormal{ 0.0f, 1.0f, 0.0f };
   };

   struct TerrainPainterState
   {
      bool CursorValid = false;
      bool PaintingActive = false;
      bool BrushModeEnabled = false;
      glm::vec3 CursorWorldPos{ 0.0f };
      glm::vec3 CursorWorldNormal{ 0.0f, 1.0f, 0.0f };
      bool BrushHeldLastFrame = false;
      float StampHeightNormalized = 0.0f;
      bool StampHeightValid = false;
      EntityID StampEntity = INVALID_ENTITY_ID;
      EntityID PaintEntity = INVALID_ENTITY_ID;
   };

   TerrainPainterState& GetPainterState()
   {
      static TerrainPainterState state;
      return state;
   }

   bool ComputeTerrainHit(const TransformComponent& transform, TerrainComponent& terrain, const glm::vec3& rayOriginWorld, const glm::vec3& rayDirWorld, TerrainHit& outHit)
   {
      glm::vec3 worldPos, worldNormal, localPos, localNormal;
      if (!Terrain::Raycast(transform, terrain, rayOriginWorld, rayDirWorld, &worldPos, &worldNormal, &localPos, &localNormal))
         return false;
      outHit.LocalPosition = localPos;
      outHit.LocalNormal = localNormal;
      outHit.WorldPosition = worldPos;
      outHit.WorldNormal = worldNormal;
      return true;
   }

   bool ComputeTerrainHit(const TransformComponent& transform,
      TerrainComponent& terrain,
      const glm::vec3& rayOriginWorld,
      const glm::vec3& rayDirWorld,
      TerrainHit& outHit,
      bool ignoreHoles)
   {
      glm::vec3 worldPos, worldNormal, localPos, localNormal;
      if (!Terrain::Raycast(transform, terrain, rayOriginWorld, rayDirWorld, &worldPos, &worldNormal, &localPos, &localNormal, ignoreHoles))
         return false;
      outHit.LocalPosition = localPos;
      outHit.LocalNormal = localNormal;
      outHit.WorldPosition = worldPos;
      outHit.WorldNormal = worldNormal;
      return true;
   }

   void ComputeBrushPlaneBasis(const glm::vec3& normal, glm::vec3& outNormal, glm::vec3& outTangent, glm::vec3& outBitangent)
   {
      outNormal = glm::dot(normal, normal) > 1e-4f ? glm::normalize(normal) : glm::vec3(0.0f, 1.0f, 0.0f);
      const glm::vec3 reference = std::abs(outNormal.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
      outTangent = glm::normalize(glm::cross(reference, outNormal));
      outBitangent = glm::normalize(glm::cross(outNormal, outTangent));
   }

   void DrawBrushSquare(Renderer& renderer, const glm::vec3& center, const glm::vec3& normal, float halfExtent, uint32_t abgrColor)
   {
      glm::vec3 planeNormal, tangent, bitangent;
      ComputeBrushPlaneBasis(normal, planeNormal, tangent, bitangent);

      const float hoverOffset = 0.1f;
      const glm::vec3 squareCenter = center + planeNormal * hoverOffset;
      const glm::vec3 xAxis = tangent * halfExtent;
      const glm::vec3 zAxis = bitangent * halfExtent;
      const glm::vec3 corners[4] = {
         squareCenter - xAxis - zAxis,
         squareCenter + xAxis - zAxis,
         squareCenter + xAxis + zAxis,
         squareCenter - xAxis + zAxis
      };

      for (int i = 0; i < 4; ++i)
      {
         renderer.DrawDebugLineColored(corners[i], corners[(i + 1) % 4], abgrColor);
      }
   }

   bool HasHeightmapStampData(const TerrainBrush& brush)
   {
      return brush.HeightmapStampWidth > 0 &&
             brush.HeightmapStampHeight > 0 &&
             brush.HeightmapStampSamples.size() == static_cast<size_t>(brush.HeightmapStampWidth) * static_cast<size_t>(brush.HeightmapStampHeight);
   }

   float SampleHeightmapStamp(const TerrainBrush& brush, float u, float v)
   {
      if (!HasHeightmapStampData(brush))
         return 0.0f;

      u = glm::clamp(u, 0.0f, 1.0f);
      v = glm::clamp(v, 0.0f, 1.0f);

      const float sampleX = u * static_cast<float>(brush.HeightmapStampWidth - 1);
      const float sampleY = v * static_cast<float>(brush.HeightmapStampHeight - 1);
      const int x0 = static_cast<int>(std::floor(sampleX));
      const int y0 = static_cast<int>(std::floor(sampleY));
      const int x1 = std::min(x0 + 1, brush.HeightmapStampWidth - 1);
      const int y1 = std::min(y0 + 1, brush.HeightmapStampHeight - 1);
      const float tx = sampleX - static_cast<float>(x0);
      const float ty = sampleY - static_cast<float>(y0);

      auto sampleAt = [&brush](int x, int y) {
         const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(brush.HeightmapStampWidth) + static_cast<size_t>(x);
         return idx < brush.HeightmapStampSamples.size() ? brush.HeightmapStampSamples[idx] : 0.0f;
      };

      const float v00 = sampleAt(x0, y0);
      const float v10 = sampleAt(x1, y0);
      const float v01 = sampleAt(x0, y1);
      const float v11 = sampleAt(x1, y1);
      const float a = glm::mix(v00, v10, tx);
      const float b = glm::mix(v01, v11, tx);
      return glm::mix(a, b, ty);
   }

   void ApplyHeightPaint(TerrainComponent& terrain, const glm::vec2& centerXZ, float radius, float direction, glm::ivec2& minDirty, glm::ivec2& maxDirty)
   {
      glm::vec2 cell = Terrain::GetCellSize(terrain);
      cell.x = glm::max(cell.x, 0.0001f);
      cell.y = glm::max(cell.y, 0.0001f);
      int minX = glm::max(0, (int)glm::floor((centerXZ.x - radius) / cell.x));
      int maxX = glm::min((int)terrain.GridResolution - 1, (int)glm::ceil((centerXZ.x + radius) / cell.x));
      int minZ = glm::max(0, (int)glm::floor((centerXZ.y - radius) / cell.y));
      int maxZ = glm::min((int)terrain.GridResolution - 1, (int)glm::ceil((centerXZ.y + radius) / cell.y));
      bool changed = false;

      float radiusSq = radius * radius;
      float falloffPower = glm::max(0.01f, terrain.Brush.Falloff);
      float heightScale = glm::max(0.0001f, terrain.MaxHeight);
      for (int z = minZ; z <= maxZ; ++z)
      {
         float sampleZ = z * cell.y;
         for (int x = minX; x <= maxX; ++x)
         {
            float sampleX = x * cell.x;
            float dx = sampleX - centerXZ.x;
            float dz = sampleZ - centerXZ.y;
            float distSq = dx * dx + dz * dz;
            if (distSq > radiusSq)
               continue;
            float dist = std::sqrt(distSq);
            float falloff = 1.0f - (dist / radius);
            falloff = glm::pow(glm::clamp(falloff, 0.0f, 1.0f), falloffPower);

            size_t idx = static_cast<size_t>(z) * terrain.GridResolution + x;
            float normalized = terrain.HeightMap[idx] / 65535.0f;
            float delta = (terrain.Brush.Strength / heightScale) * direction * falloff;
            normalized = glm::clamp(normalized + delta, 0.0f, 1.0f);
            uint16_t newValue = static_cast<uint16_t>(glm::clamp(normalized * 65535.0f, 0.0f, 65535.0f));
            if (terrain.HeightMap[idx] != newValue)
            {
               terrain.HeightMap[idx] = newValue;
               changed = true;
               minDirty.x = glm::min(minDirty.x, x);
               minDirty.y = glm::min(minDirty.y, z);
               maxDirty.x = glm::max(maxDirty.x, x);
               maxDirty.y = glm::max(maxDirty.y, z);
            }
         }
      }

      if (changed)
      {
         Terrain::MarkHeightRegionDirty(terrain, minDirty, maxDirty);
      }
   }

   void ApplyInstancerPaint(TerrainComponent& terrain, size_t layerIndex, const glm::vec2& centerXZ, float radius, float amount, float falloffPower)
   {
      if (layerIndex >= terrain.InstancerLayers.size())
         return;

      TerrainInstancerLayerDesc& layer = terrain.InstancerLayers[layerIndex];
      layer.EnsureMaskSize(terrain.GridResolution);
      if (layer.PaintedMask.empty() || terrain.GridResolution < 2)
         return;

      glm::vec2 cell = Terrain::GetCellSize(terrain);
      cell.x = glm::max(cell.x, 0.0001f);
      cell.y = glm::max(cell.y, 0.0001f);
      const int minX = glm::max(0, (int)glm::floor((centerXZ.x - radius) / cell.x));
      const int maxX = glm::min((int)terrain.GridResolution - 1, (int)glm::ceil((centerXZ.x + radius) / cell.x));
      const int minZ = glm::max(0, (int)glm::floor((centerXZ.y - radius) / cell.y));
      const int maxZ = glm::min((int)terrain.GridResolution - 1, (int)glm::ceil((centerXZ.y + radius) / cell.y));

      const float radiusSq = radius * radius;
      const float falloff = glm::max(0.01f, falloffPower);
      bool changed = false;
      glm::ivec2 dirtyMin(std::numeric_limits<int>::max());
      glm::ivec2 dirtyMax(std::numeric_limits<int>::min());

      for (int z = minZ; z <= maxZ; ++z)
      {
         const float sampleZ = z * cell.y;
         for (int x = minX; x <= maxX; ++x)
         {
            const float sampleX = x * cell.x;
            const float dx = sampleX - centerXZ.x;
            const float dz = sampleZ - centerXZ.y;
            const float distSq = dx * dx + dz * dz;
            if (distSq > radiusSq)
               continue;

            const float dist = std::sqrt(distSq);
            float weight = 1.0f - (dist / radius);
            weight = glm::pow(glm::clamp(weight, 0.0f, 1.0f), falloff);

            const size_t idx = static_cast<size_t>(z) * terrain.GridResolution + x;
            if (idx >= layer.PaintedMask.size())
               continue;

            float density = static_cast<float>(layer.PaintedMask[idx]) / 255.0f;
            density = glm::clamp(density + amount * weight, 0.0f, 1.0f);
            const uint8_t newValue = static_cast<uint8_t>(glm::round(density * 255.0f));
            if (layer.PaintedMask[idx] != newValue)
            {
               layer.PaintedMask[idx] = newValue;
               changed = true;
               dirtyMin.x = glm::min(dirtyMin.x, x);
               dirtyMin.y = glm::min(dirtyMin.y, z);
               dirtyMax.x = glm::max(dirtyMax.x, x);
               dirtyMax.y = glm::max(dirtyMax.y, z);
            }
         }
      }

      if (changed)
      {
         layer.Mask = TerrainInstancerMaskSource::Painted;
         layer.InvalidatePaintedMaskBounds();
         if (layer.RuntimeDirty || layer.RuntimeRebuildInProgress || layer.Instancer.NeedsRegeneration)
            terrain.MarkInstancerLayerDirty(layerIndex);
         else
            terrain.MarkInstancerLayerRegionDirty(layerIndex, dirtyMin, dirtyMax);
         terrain.AssetDirty = true;
      }
   }

   void NormalizeWeights(glm::vec4& weights, int lockedIndex)
   {
      weights = glm::max(weights, glm::vec4(0.0f));
      float sum = weights.x + weights.y + weights.z + weights.w;
      if (sum <= 1e-4f)
      {
         weights = glm::vec4(0.25f);
         sum = 1.0f;
      }
      if (lockedIndex >= 0)
      {
         float locked = glm::clamp(weights[lockedIndex], 0.0f, 1.0f);
         float remaining = glm::max(0.0f, 1.0f - locked);
         glm::vec4 others = weights;
         others[lockedIndex] = 0.0f;
         float otherSum = others.x + others.y + others.z + others.w;
         if (otherSum > 1e-4f)
         {
            others *= remaining / otherSum;
            weights = others;
            weights[lockedIndex] = locked;
            return;
         }
      }
      weights /= sum;
   }

   // Normalize weights across all 8 layers
   void NormalizeWeights8(float weights[8], int lockedIndex)
   {
      float sum = 0.0f;
      for (int i = 0; i < 8; ++i) sum += weights[i];
      if (sum < 1e-4f) {
         weights[0] = 1.0f;
         for (int i = 1; i < 8; ++i) weights[i] = 0.0f;
         return;
      }
      if (lockedIndex >= 0 && lockedIndex < 8) {
         float locked = glm::clamp(weights[lockedIndex], 0.0f, 1.0f);
         float remaining = glm::max(0.0f, 1.0f - locked);
         float otherSum = sum - locked;
         if (otherSum > 1e-4f) {
            float scale = remaining / otherSum;
            for (int i = 0; i < 8; ++i) {
               if (i != lockedIndex) weights[i] *= scale;
            }
            weights[lockedIndex] = locked;
            return;
         }
      }
      for (int i = 0; i < 8; ++i) weights[i] /= sum;
   }

   void ApplyTexturePaint(TerrainComponent& terrain, const glm::vec2& centerXZ, float radius, float deltaSign, glm::ivec2& minDirty, glm::ivec2& maxDirty)
   {
      glm::vec2 cell = Terrain::GetCellSize(terrain);
      cell.x = glm::max(cell.x, 0.0001f);
      cell.y = glm::max(cell.y, 0.0001f);
      int minX = glm::max(0, (int)glm::floor((centerXZ.x - radius) / cell.x));
      int maxX = glm::min((int)terrain.GridResolution - 1, (int)glm::ceil((centerXZ.x + radius) / cell.x));
      int minZ = glm::max(0, (int)glm::floor((centerXZ.y - radius) / cell.y));
      int maxZ = glm::min((int)terrain.GridResolution - 1, (int)glm::ceil((centerXZ.y + radius) / cell.y));
      int maxLayer = terrain.Layers.empty() ? -1 : static_cast<int>(terrain.Layers.size()) - 1;
      if (maxLayer < 0)
         return;
      int layerIndex = glm::clamp(terrain.Brush.ActiveLayer, 0, maxLayer);
      
      // Ensure SplatMap2 exists when painting layers 4-7
      if (layerIndex >= 4) {
         terrain.EnsureSplatMap2();
      }
      
      bool changed = false;

      float radiusSq = radius * radius;
      float falloffPower = glm::max(0.01f, terrain.Brush.Falloff);
      for (int z = minZ; z <= maxZ; ++z)
      {
         float sampleZ = z * cell.y;
         for (int x = minX; x <= maxX; ++x)
         {
            float sampleX = x * cell.x;
            float dx = sampleX - centerXZ.x;
            float dz = sampleZ - centerXZ.y;
            float distSq = dx * dx + dz * dz;
            if (distSq > radiusSq)
               continue;
            float dist = std::sqrt(distSq);
            float falloff = 1.0f - (dist / radius);
            falloff = glm::pow(glm::clamp(falloff, 0.0f, 1.0f), falloffPower);

            size_t idx = static_cast<size_t>(z) * terrain.GridResolution + x;
            
            // Read weights from both splatmaps
            float weights[8] = {0.0f};
            glm::vec4 splat0 = glm::vec4(terrain.SplatMap[idx]) / 255.0f;
            weights[0] = splat0.r;
            weights[1] = splat0.g;
            weights[2] = splat0.b;
            weights[3] = splat0.a;
            
            if (!terrain.SplatMap2.empty()) {
               glm::vec4 splat1 = glm::vec4(terrain.SplatMap2[idx]) / 255.0f;
               weights[4] = splat1.r;
               weights[5] = splat1.g;
               weights[6] = splat1.b;
               weights[7] = splat1.a;
            }
            
            // Apply paint
            float delta = terrain.Brush.TextureStrength * deltaSign * falloff;
            weights[layerIndex] = glm::clamp(weights[layerIndex] + delta, 0.0f, 1.0f);
            NormalizeWeights8(weights, layerIndex);

            // Pack back into splatmaps
            glm::u8vec4 packed0;
            packed0.x = static_cast<uint8_t>(glm::clamp(weights[0], 0.0f, 1.0f) * 255.0f);
            packed0.y = static_cast<uint8_t>(glm::clamp(weights[1], 0.0f, 1.0f) * 255.0f);
            packed0.z = static_cast<uint8_t>(glm::clamp(weights[2], 0.0f, 1.0f) * 255.0f);
            packed0.w = static_cast<uint8_t>(glm::clamp(weights[3], 0.0f, 1.0f) * 255.0f);
            
            bool changed0 = (terrain.SplatMap[idx] != packed0);
            if (changed0) {
               terrain.SplatMap[idx] = packed0;
            }
            
            bool changed1 = false;
            if (!terrain.SplatMap2.empty()) {
               glm::u8vec4 packed1;
               packed1.x = static_cast<uint8_t>(glm::clamp(weights[4], 0.0f, 1.0f) * 255.0f);
               packed1.y = static_cast<uint8_t>(glm::clamp(weights[5], 0.0f, 1.0f) * 255.0f);
               packed1.z = static_cast<uint8_t>(glm::clamp(weights[6], 0.0f, 1.0f) * 255.0f);
               packed1.w = static_cast<uint8_t>(glm::clamp(weights[7], 0.0f, 1.0f) * 255.0f);
               changed1 = (terrain.SplatMap2[idx] != packed1);
               if (changed1) {
                  terrain.SplatMap2[idx] = packed1;
               }
            }
            
            if (changed0 || changed1) {
               changed = true;
               minDirty.x = glm::min(minDirty.x, x);
               minDirty.y = glm::min(minDirty.y, z);
               maxDirty.x = glm::max(maxDirty.x, x);
               maxDirty.y = glm::max(maxDirty.y, z);
            }
         }
      }

      if (changed)
      {
         std::cout << "[TerrainPainter] ApplyTexturePaint changed terrain @" << (void*)&terrain
                   << " layer=" << layerIndex
                   << " dirty region: (" << minDirty.x << "," << minDirty.y << ") to (" << maxDirty.x << "," << maxDirty.y << ")" << std::endl;
         Terrain::MarkSplatRegionDirty(terrain, minDirty, maxDirty);
         std::cout << "[TerrainPainter] After MarkSplatRegionDirty: SplatDataDirty=" << terrain.SplatDataDirty
                   << " chunk.SplatDirty=" << terrain.Chunks[0].SplatDirty << std::endl;
      }
   }

   void ApplyHolePaint(TerrainComponent& terrain, const glm::vec2& centerXZ, float radius, float direction, glm::ivec2& minDirty, glm::ivec2& maxDirty)
   {
      if (terrain.HoleMask.empty())
         return;

      glm::vec2 cell = Terrain::GetCellSize(terrain);
      cell.x = glm::max(cell.x, 0.0001f);
      cell.y = glm::max(cell.y, 0.0001f);
      int minX = glm::max(0, (int)glm::floor((centerXZ.x - radius) / cell.x));
      int maxX = glm::min((int)terrain.GridResolution - 1, (int)glm::ceil((centerXZ.x + radius) / cell.x));
      int minZ = glm::max(0, (int)glm::floor((centerXZ.y - radius) / cell.y));
      int maxZ = glm::min((int)terrain.GridResolution - 1, (int)glm::ceil((centerXZ.y + radius) / cell.y));
      bool changed = false;

      const float radiusSq = radius * radius;
      const float falloffPower = glm::max(0.01f, terrain.Brush.Falloff);
      const float paintStrength = glm::clamp(terrain.Brush.TextureStrength, 0.0f, 1.0f);
      for (int z = minZ; z <= maxZ; ++z)
      {
         const float sampleZ = z * cell.y;
         for (int x = minX; x <= maxX; ++x)
         {
            const float sampleX = x * cell.x;
            const float dx = sampleX - centerXZ.x;
            const float dz = sampleZ - centerXZ.y;
            const float distSq = dx * dx + dz * dz;
            if (distSq > radiusSq)
               continue;

            const float dist = std::sqrt(distSq);
            float falloff = 1.0f - (dist / radius);
            falloff = glm::pow(glm::clamp(falloff, 0.0f, 1.0f), falloffPower);

            const size_t idx = static_cast<size_t>(z) * terrain.GridResolution + x;
            const float normalized = terrain.HoleMask[idx] / 255.0f;
            const float delta = paintStrength * direction * falloff;
            const float newNormalized = glm::clamp(normalized + delta, 0.0f, 1.0f);
            const uint8_t newValue = static_cast<uint8_t>(glm::clamp(newNormalized * 255.0f, 0.0f, 255.0f));
            if (terrain.HoleMask[idx] != newValue)
            {
               terrain.HoleMask[idx] = newValue;
               changed = true;
               minDirty.x = glm::min(minDirty.x, x);
               minDirty.y = glm::min(minDirty.y, z);
               maxDirty.x = glm::max(maxDirty.x, x);
               maxDirty.y = glm::max(maxDirty.y, z);
            }
         }
      }

      if (changed)
      {
         Terrain::MarkHoleRegionDirty(terrain, minDirty, maxDirty);
      }
   }

   float SampleHeightNormalized(const TerrainComponent& terrain, int x, int z)
   {
      x = glm::clamp(x, 0, static_cast<int>(terrain.GridResolution) - 1);
      z = glm::clamp(z, 0, static_cast<int>(terrain.GridResolution) - 1);
      size_t idx = static_cast<size_t>(z) * terrain.GridResolution + x;
      if (idx >= terrain.HeightMap.size()) return 0.0f;
      return terrain.HeightMap[idx] / 65535.0f;
   }

   void ApplySmoothPaint(TerrainComponent& terrain, const glm::vec2& centerXZ, float radius, glm::ivec2& minDirty, glm::ivec2& maxDirty)
   {
      if (terrain.HeightMap.empty()) return;
      glm::vec2 cell = Terrain::GetCellSize(terrain);
      cell.x = glm::max(cell.x, 0.0001f);
      cell.y = glm::max(cell.y, 0.0001f);
      int minX = glm::max(0, (int)glm::floor((centerXZ.x - radius) / cell.x));
      int maxX = glm::min((int)terrain.GridResolution - 1, (int)glm::ceil((centerXZ.x + radius) / cell.x));
      int minZ = glm::max(0, (int)glm::floor((centerXZ.y - radius) / cell.y));
      int maxZ = glm::min((int)terrain.GridResolution - 1, (int)glm::ceil((centerXZ.y + radius) / cell.y));
      float radiusSq = radius * radius;
      float falloffPower = glm::max(0.01f, terrain.Brush.Falloff);
      float smoothStrength = glm::clamp(terrain.Brush.Strength, 0.0f, 1.0f);
      bool changed = false;

      for (int z = minZ; z <= maxZ; ++z)
      {
         float sampleZ = z * cell.y;
         for (int x = minX; x <= maxX; ++x)
         {
            float sampleX = x * cell.x;
            float dx = sampleX - centerXZ.x;
            float dz = sampleZ - centerXZ.y;
            float distSq = dx * dx + dz * dz;
            if (distSq > radiusSq)
               continue;
            float dist = std::sqrt(distSq);
            float falloff = 1.0f - (dist / radius);
            falloff = glm::pow(glm::clamp(falloff, 0.0f, 1.0f), falloffPower);

            float current = SampleHeightNormalized(terrain, x, z);
            float neighborSum = 0.0f;
            int neighborCount = 0;
            for (int nz = -1; nz <= 1; ++nz)
            {
               for (int nx = -1; nx <= 1; ++nx)
               {
                  neighborSum += SampleHeightNormalized(terrain, x + nx, z + nz);
                  neighborCount++;
               }
            }
            float average = (neighborCount > 0) ? neighborSum / (float)neighborCount : current;
            float lerpT = smoothStrength * falloff;
            float newValue = glm::mix(current, average, glm::clamp(lerpT, 0.0f, 1.0f));
            uint16_t packed = static_cast<uint16_t>(glm::clamp(newValue * 65535.0f, 0.0f, 65535.0f));
            size_t idx = static_cast<size_t>(z) * terrain.GridResolution + x;
            if (terrain.HeightMap[idx] != packed)
            {
               terrain.HeightMap[idx] = packed;
               changed = true;
               minDirty.x = glm::min(minDirty.x, x);
               minDirty.y = glm::min(minDirty.y, z);
               maxDirty.x = glm::max(maxDirty.x, x);
               maxDirty.y = glm::max(maxDirty.y, z);
            }
         }
      }

      if (changed)
      {
         Terrain::MarkHeightRegionDirty(terrain, minDirty, maxDirty);
      }
   }

   void ApplyStampPaint(TerrainComponent& terrain, const glm::vec2& centerXZ, float radius, float targetNormalized, glm::ivec2& minDirty, glm::ivec2& maxDirty)
   {
      if (terrain.HeightMap.empty()) return;
      glm::vec2 cell = Terrain::GetCellSize(terrain);
      cell.x = glm::max(cell.x, 0.0001f);
      cell.y = glm::max(cell.y, 0.0001f);
      int minX = glm::max(0, (int)glm::floor((centerXZ.x - radius) / cell.x));
      int maxX = glm::min((int)terrain.GridResolution - 1, (int)glm::ceil((centerXZ.x + radius) / cell.x));
      int minZ = glm::max(0, (int)glm::floor((centerXZ.y - radius) / cell.y));
      int maxZ = glm::min((int)terrain.GridResolution - 1, (int)glm::ceil((centerXZ.y + radius) / cell.y));
      float radiusSq = radius * radius;
      float falloffPower = glm::max(0.01f, terrain.Brush.Falloff);
      float stampStrength = glm::clamp(terrain.Brush.Strength, 0.0f, 1.0f);
      bool changed = false;

      for (int z = minZ; z <= maxZ; ++z)
      {
         float sampleZ = z * cell.y;
         for (int x = minX; x <= maxX; ++x)
         {
            float sampleX = x * cell.x;
            float dx = sampleX - centerXZ.x;
            float dz = sampleZ - centerXZ.y;
            float distSq = dx * dx + dz * dz;
            if (distSq > radiusSq)
               continue;
            float dist = std::sqrt(distSq);
            float falloff = 1.0f - (dist / radius);
            falloff = glm::pow(glm::clamp(falloff, 0.0f, 1.0f), falloffPower);
            float current = SampleHeightNormalized(terrain, x, z);
            float lerpT = glm::clamp(stampStrength * falloff, 0.0f, 1.0f);
            float newValue = glm::mix(current, targetNormalized, lerpT);
            uint16_t packed = static_cast<uint16_t>(glm::clamp(newValue * 65535.0f, 0.0f, 65535.0f));
            size_t idx = static_cast<size_t>(z) * terrain.GridResolution + x;
            if (terrain.HeightMap[idx] != packed)
            {
               terrain.HeightMap[idx] = packed;
               changed = true;
               minDirty.x = glm::min(minDirty.x, x);
               minDirty.y = glm::min(minDirty.y, z);
               maxDirty.x = glm::max(maxDirty.x, x);
               maxDirty.y = glm::max(maxDirty.y, z);
            }
         }
      }

      if (changed)
      {
         Terrain::MarkHeightRegionDirty(terrain, minDirty, maxDirty);
      }
   }

   void ApplyHeightmapStampPaint(TerrainComponent& terrain, const glm::vec2& centerXZ, float halfExtent, glm::ivec2& minDirty, glm::ivec2& maxDirty)
   {
      if (terrain.HeightMap.empty() || !HasHeightmapStampData(terrain.Brush))
         return;

      glm::vec2 cell = Terrain::GetCellSize(terrain);
      cell.x = glm::max(cell.x, 0.0001f);
      cell.y = glm::max(cell.y, 0.0001f);
      halfExtent = glm::max(0.1f, halfExtent);

      const float minWorldX = centerXZ.x - halfExtent;
      const float maxWorldX = centerXZ.x + halfExtent;
      const float minWorldZ = centerXZ.y - halfExtent;
      const float maxWorldZ = centerXZ.y + halfExtent;

      const int minX = glm::max(0, static_cast<int>(std::floor(minWorldX / cell.x)));
      const int maxX = glm::min(static_cast<int>(terrain.GridResolution) - 1, static_cast<int>(std::ceil(maxWorldX / cell.x)));
      const int minZ = glm::max(0, static_cast<int>(std::floor(minWorldZ / cell.y)));
      const int maxZ = glm::min(static_cast<int>(terrain.GridResolution) - 1, static_cast<int>(std::ceil(maxWorldZ / cell.y)));
      const float squareSize = glm::max(halfExtent * 2.0f, 0.0001f);
      const float maxTerrainHeight = glm::max(0.0001f, terrain.MaxHeight);
      const float replaceBlend = glm::clamp(terrain.Brush.Strength, 0.0f, 1.0f);
      const float deltaScale = glm::max(0.0f, terrain.Brush.Strength);

      float minY = glm::clamp(terrain.Brush.HeightmapStampMinY, 0.0f, maxTerrainHeight);
      float maxY = glm::clamp(terrain.Brush.HeightmapStampMaxY, minY, maxTerrainHeight);
      float baselineY = glm::clamp(terrain.Brush.HeightmapStampBaselineY, minY, maxY);
      const bool additive = terrain.Brush.HeightmapStampAdditive && !terrain.Brush.HeightmapStampSubtractive;
      const bool subtractive = terrain.Brush.HeightmapStampSubtractive;
      bool changed = false;

      for (int z = minZ; z <= maxZ; ++z)
      {
         const float sampleZ = z * cell.y;
         const float v = (sampleZ - minWorldZ) / squareSize;
         if (v < 0.0f || v > 1.0f)
            continue;

         for (int x = minX; x <= maxX; ++x)
         {
            const float sampleX = x * cell.x;
            const float u = (sampleX - minWorldX) / squareSize;
            if (u < 0.0f || u > 1.0f)
               continue;

            const float stampSample = SampleHeightmapStamp(terrain.Brush, u, v);
            const float stampWorldY = glm::mix(minY, maxY, stampSample);
            const size_t idx = static_cast<size_t>(z) * terrain.GridResolution + x;
            const float currentNormalized = terrain.HeightMap[idx] / 65535.0f;

            float nextNormalized = currentNormalized;
            if (additive || subtractive)
            {
               float deltaWorldY = (stampWorldY - baselineY) * deltaScale;
               if (subtractive)
                  deltaWorldY = -deltaWorldY;
               const float nextWorldY = currentNormalized * maxTerrainHeight + deltaWorldY;
               nextNormalized = glm::clamp(nextWorldY / maxTerrainHeight, 0.0f, 1.0f);
            }
            else
            {
               const float targetNormalized = glm::clamp(stampWorldY / maxTerrainHeight, 0.0f, 1.0f);
               nextNormalized = glm::mix(currentNormalized, targetNormalized, replaceBlend);
            }

            const uint16_t packed = static_cast<uint16_t>(glm::clamp(nextNormalized * 65535.0f, 0.0f, 65535.0f));
            if (terrain.HeightMap[idx] != packed)
            {
               terrain.HeightMap[idx] = packed;
               changed = true;
               minDirty.x = glm::min(minDirty.x, x);
               minDirty.y = glm::min(minDirty.y, z);
               maxDirty.x = glm::max(maxDirty.x, x);
               maxDirty.y = glm::max(maxDirty.y, z);
            }
         }
      }

      if (changed)
      {
         Terrain::MarkHeightRegionDirty(terrain, minDirty, maxDirty);
      }
   }

   // Simple hash functions for noise generation
   inline float NoiseHash(float n)
   {
      return glm::fract(std::sin(n) * 43758.5453123f);
   }

   inline float NoiseHash2D(const glm::vec2& p)
   {
      return NoiseHash(glm::dot(p, glm::vec2(12.9898f, 78.233f)));
   }

   // Smooth interpolation (quintic Hermite)
   inline float SmoothStep(float t)
   {
      return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
   }

   // Value noise implementation (simpler than Perlin, no static tables)
   float ValueNoise2D(const glm::vec2& p)
   {
      glm::vec2 i = glm::floor(p);
      glm::vec2 f = glm::fract(p);
      
      // Four corners
      float a = NoiseHash2D(i);
      float b = NoiseHash2D(i + glm::vec2(1.0f, 0.0f));
      float c = NoiseHash2D(i + glm::vec2(0.0f, 1.0f));
      float d = NoiseHash2D(i + glm::vec2(1.0f, 1.0f));
      
      // Smooth interpolation
      float u = SmoothStep(f.x);
      float v = SmoothStep(f.y);
      
      // Bilinear interpolation
      return glm::mix(glm::mix(a, b, u), glm::mix(c, d, u), v);
   }

   // Generate fractal brownian motion noise (multi-octave value noise)
   float FBMNoise(const glm::vec2& pos, float scale, int octaves, float persistence)
   {
      float value = 0.0f;
      float amplitude = 1.0f;
      float frequency = scale;
      float maxValue = 0.0f;

      for (int i = 0; i < octaves; ++i)
      {
         // ValueNoise2D returns [0, 1], convert to [-1, 1]
         float n = ValueNoise2D(pos * frequency) * 2.0f - 1.0f;
         value += n * amplitude;
         maxValue += amplitude;
         amplitude *= persistence;
         frequency *= 2.0f;
      }

      // Normalize to [-1, 1] range
      return value / maxValue;
   }

   void ApplyErosionNoisePaint(TerrainComponent& terrain, const glm::vec2& centerXZ, float radius, float direction, glm::ivec2& minDirty, glm::ivec2& maxDirty)
   {
      if (terrain.HeightMap.empty()) return;
      
      glm::vec2 cell = Terrain::GetCellSize(terrain);
      cell.x = glm::max(cell.x, 0.0001f);
      cell.y = glm::max(cell.y, 0.0001f);
      
      int minX = glm::max(0, (int)glm::floor((centerXZ.x - radius) / cell.x));
      int maxX = glm::min((int)terrain.GridResolution - 1, (int)glm::ceil((centerXZ.x + radius) / cell.x));
      int minZ = glm::max(0, (int)glm::floor((centerXZ.y - radius) / cell.y));
      int maxZ = glm::min((int)terrain.GridResolution - 1, (int)glm::ceil((centerXZ.y + radius) / cell.y));
      
      float radiusSq = radius * radius;
      float falloffPower = glm::max(0.01f, terrain.Brush.Falloff);
      float heightScale = glm::max(0.0001f, terrain.MaxHeight);
      float noiseScale = terrain.Brush.ErosionNoiseScale;
      int octaves = glm::clamp(terrain.Brush.ErosionNoiseOctaves, 1, 8);
      float persistence = glm::clamp(terrain.Brush.ErosionNoisePersistence, 0.1f, 0.9f);
      float noiseStrength = terrain.Brush.ErosionNoiseStrength;
      
      bool changed = false;
      
      for (int z = minZ; z <= maxZ; ++z)
      {
         float sampleZ = z * cell.y;
         for (int x = minX; x <= maxX; ++x)
         {
            float sampleX = x * cell.x;
            float dx = sampleX - centerXZ.x;
            float dz = sampleZ - centerXZ.y;
            float distSq = dx * dx + dz * dz;
            if (distSq > radiusSq)
               continue;
               
            float dist = std::sqrt(distSq);
            float falloff = 1.0f - (dist / radius);
            falloff = glm::pow(glm::clamp(falloff, 0.0f, 1.0f), falloffPower);
            
            // Sample noise at this position
            glm::vec2 noisePos(sampleX, sampleZ);
            float noise = FBMNoise(noisePos, noiseScale, octaves, persistence);
            
            // Convert noise from [-1, 1] to erosion effect
            // Negative values erode, positive values can deposit (based on direction)
            float erosionAmount = noise * noiseStrength * direction * falloff;
            
            size_t idx = static_cast<size_t>(z) * terrain.GridResolution + x;
            float normalized = terrain.HeightMap[idx] / 65535.0f;
            float delta = (terrain.Brush.Strength / heightScale) * erosionAmount;
            normalized = glm::clamp(normalized + delta, 0.0f, 1.0f);
            uint16_t newValue = static_cast<uint16_t>(glm::clamp(normalized * 65535.0f, 0.0f, 65535.0f));
            
            if (terrain.HeightMap[idx] != newValue)
            {
               terrain.HeightMap[idx] = newValue;
               changed = true;
               minDirty.x = glm::min(minDirty.x, x);
               minDirty.y = glm::min(minDirty.y, z);
               maxDirty.x = glm::max(maxDirty.x, x);
               maxDirty.y = glm::max(maxDirty.y, z);
            }
         }
      }
      
      if (changed)
      {
         Terrain::MarkHeightRegionDirty(terrain, minDirty, maxDirty);
      }
   }

   // Ridge noise for cliffs and mountains - creates sharp directional features
   float RidgeNoise(const glm::vec2& pos, float scale, int octaves, float persistence)
   {
      float value = 0.0f;
      float amplitude = 1.0f;
      float frequency = scale;
      float maxValue = 0.0f;

      for (int i = 0; i < octaves; ++i)
      {
         float n = ValueNoise2D(pos * frequency) * 2.0f - 1.0f;
         // Ridge transformation: fold the noise to create sharp ridges
         n = 1.0f - std::abs(n);
         n = n * n; // Square to sharpen ridges
         value += n * amplitude;
         maxValue += amplitude;
         amplitude *= persistence;
         frequency *= 2.0f;
      }

      return value / maxValue;
   }

   void ApplyCliffStampPaint(TerrainComponent& terrain, const glm::vec2& centerXZ, float radius, float direction, glm::ivec2& minDirty, glm::ivec2& maxDirty)
   {
      if (terrain.HeightMap.empty()) return;
      
      glm::vec2 cell = Terrain::GetCellSize(terrain);
      cell.x = glm::max(cell.x, 0.0001f);
      cell.y = glm::max(cell.y, 0.0001f);
      
      int minX = glm::max(0, (int)glm::floor((centerXZ.x - radius) / cell.x));
      int maxX = glm::min((int)terrain.GridResolution - 1, (int)glm::ceil((centerXZ.x + radius) / cell.x));
      int minZ = glm::max(0, (int)glm::floor((centerXZ.y - radius) / cell.y));
      int maxZ = glm::min((int)terrain.GridResolution - 1, (int)glm::ceil((centerXZ.y + radius) / cell.y));
      
      float radiusSq = radius * radius;
      float falloffPower = glm::max(0.01f, terrain.Brush.Falloff);
      float heightScale = glm::max(0.0001f, terrain.MaxHeight);
      float cliffHeight = terrain.Brush.CliffHeight;
      float roughness = terrain.Brush.CliffRoughness;
      float layering = terrain.Brush.CliffLayering;
      
      bool changed = false;
      
      for (int z = minZ; z <= maxZ; ++z)
      {
         float sampleZ = z * cell.y;
         for (int x = minX; x <= maxX; ++x)
         {
            float sampleX = x * cell.x;
            float dx = sampleX - centerXZ.x;
            float dz = sampleZ - centerXZ.y;
            float distSq = dx * dx + dz * dz;
            if (distSq > radiusSq)
               continue;
               
            float dist = std::sqrt(distSq);
            float falloff = 1.0f - (dist / radius);
            falloff = glm::pow(glm::clamp(falloff, 0.0f, 1.0f), falloffPower);
            
            glm::vec2 noisePos(sampleX, sampleZ);
            
            // Base cliff shape - steep at edges
            float cliffShape = glm::pow(falloff, 0.3f); // Steep walls
            
            // Ridge noise for jagged rock appearance
            float ridgeDetail = RidgeNoise(noisePos, 0.4f, 4, 0.6f);
            float jaggedNoise = FBMNoise(noisePos, 0.25f, 3, 0.5f);
            
            // Horizontal layering effect (sedimentary bands)
            size_t idx = static_cast<size_t>(z) * terrain.GridResolution + x;
            float currentHeight = terrain.HeightMap[idx] / 65535.0f * heightScale;
            float layerNoise = std::sin(currentHeight * 2.0f + noisePos.x * 0.1f) * 0.5f + 0.5f;
            layerNoise = layerNoise * layerNoise; // Sharpen bands
            
            // Combine effects
            float rockDetail = glm::mix(jaggedNoise, ridgeDetail, 0.6f) * roughness;
            float layerEffect = layerNoise * layering * 0.3f;
            
            float cliffEffect = cliffShape * (1.0f + rockDetail * 0.4f + layerEffect);
            float heightDelta = (cliffHeight / heightScale) * cliffEffect * direction * falloff;
            
            float normalized = terrain.HeightMap[idx] / 65535.0f;
            normalized = glm::clamp(normalized + heightDelta * terrain.Brush.Strength, 0.0f, 1.0f);
            uint16_t newValue = static_cast<uint16_t>(glm::clamp(normalized * 65535.0f, 0.0f, 65535.0f));
            
            if (terrain.HeightMap[idx] != newValue)
            {
               terrain.HeightMap[idx] = newValue;
               changed = true;
               minDirty.x = glm::min(minDirty.x, x);
               minDirty.y = glm::min(minDirty.y, z);
               maxDirty.x = glm::max(maxDirty.x, x);
               maxDirty.y = glm::max(maxDirty.y, z);
            }
         }
      }
      
      if (changed)
      {
         Terrain::MarkHeightRegionDirty(terrain, minDirty, maxDirty);
      }
   }

   void ApplyMountainStampPaint(TerrainComponent& terrain, const glm::vec2& centerXZ, float radius, float direction, glm::ivec2& minDirty, glm::ivec2& maxDirty)
   {
      if (terrain.HeightMap.empty()) return;
      
      glm::vec2 cell = Terrain::GetCellSize(terrain);
      cell.x = glm::max(cell.x, 0.0001f);
      cell.y = glm::max(cell.y, 0.0001f);
      
      int minX = glm::max(0, (int)glm::floor((centerXZ.x - radius) / cell.x));
      int maxX = glm::min((int)terrain.GridResolution - 1, (int)glm::ceil((centerXZ.x + radius) / cell.x));
      int minZ = glm::max(0, (int)glm::floor((centerXZ.y - radius) / cell.y));
      int maxZ = glm::min((int)terrain.GridResolution - 1, (int)glm::ceil((centerXZ.y + radius) / cell.y));
      
      float radiusSq = radius * radius;
      float falloffPower = glm::max(0.01f, terrain.Brush.Falloff);
      float heightScale = glm::max(0.0001f, terrain.MaxHeight);
      float peakHeight = terrain.Brush.MountainHeight;
      float ridgeScale = terrain.Brush.MountainRidgeScale;
      float rockiness = terrain.Brush.MountainRockiness;
      float steepness = terrain.Brush.MountainSteepness;
      
      bool changed = false;
      
      for (int z = minZ; z <= maxZ; ++z)
      {
         float sampleZ = z * cell.y;
         for (int x = minX; x <= maxX; ++x)
         {
            float sampleX = x * cell.x;
            float dx = sampleX - centerXZ.x;
            float dz = sampleZ - centerXZ.y;
            float distSq = dx * dx + dz * dz;
            if (distSq > radiusSq)
               continue;
               
            float dist = std::sqrt(distSq);
            float normalizedDist = dist / radius;
            
            glm::vec2 noisePos(sampleX, sampleZ);
            
            // Mountain peak shape - steeper at center, gradual at edges
            float mountainShape = 1.0f - glm::pow(normalizedDist, steepness);
            mountainShape = glm::max(0.0f, mountainShape);
            
            // Ridge features radiating from peak
            float angle = std::atan2(dz, dx);
            float ridgePattern = RidgeNoise(glm::vec2(angle * 3.0f, normalizedDist * 8.0f), ridgeScale * 10.0f, 3, 0.5f);
            
            // Add variation to ridge heights based on position
            float ridgeVariation = FBMNoise(noisePos, ridgeScale, 4, 0.5f) * 0.5f + 0.5f;
            ridgePattern *= ridgeVariation;
            
            // Surface rock detail
            float rockDetail = RidgeNoise(noisePos, 0.5f, 4, 0.6f);
            float fineDetail = FBMNoise(noisePos, 0.8f, 3, 0.4f);
            float surfaceRock = glm::mix(fineDetail, rockDetail, 0.7f) * rockiness;
            
            // Combine: base shape + ridges (stronger near peak) + rock detail
            float ridgeInfluence = ridgePattern * 0.3f * (1.0f - normalizedDist * 0.5f);
            float totalShape = mountainShape * (1.0f + ridgeInfluence + surfaceRock * 0.2f);
            
            // Apply brush falloff
            float falloff = 1.0f - normalizedDist;
            falloff = glm::pow(glm::clamp(falloff, 0.0f, 1.0f), falloffPower);
            
            float heightDelta = (peakHeight / heightScale) * totalShape * direction * falloff;
            
            size_t idx = static_cast<size_t>(z) * terrain.GridResolution + x;
            float normalized = terrain.HeightMap[idx] / 65535.0f;
            normalized = glm::clamp(normalized + heightDelta * terrain.Brush.Strength, 0.0f, 1.0f);
            uint16_t newValue = static_cast<uint16_t>(glm::clamp(normalized * 65535.0f, 0.0f, 65535.0f));
            
            if (terrain.HeightMap[idx] != newValue)
            {
               terrain.HeightMap[idx] = newValue;
               changed = true;
               minDirty.x = glm::min(minDirty.x, x);
               minDirty.y = glm::min(minDirty.y, z);
               maxDirty.x = glm::max(maxDirty.x, x);
               maxDirty.y = glm::max(maxDirty.y, z);
            }
         }
      }
      
      if (changed)
      {
         Terrain::MarkHeightRegionDirty(terrain, minDirty, maxDirty);
      }
   }
}

void TerrainPainter::Update(Scene& scene, EntityID selectedEntity, bool playMode, bool viewportHovered, bool allowSelectionChanges, Camera* viewportCamera)
{
   TerrainPainterState& state = GetPainterState();
   EditorSceneUndoStack& undoStack = EditorSceneUndoStack::Get();
   auto endStroke = [&]() {
      if (state.BrushHeldLastFrame) {
         if (state.PaintEntity != INVALID_ENTITY_ID && state.PaintEntity != 0) {
            if (EntityData* paintedData = scene.GetEntityData(state.PaintEntity)) {
               if (paintedData->Terrain) {
                  paintedData->Terrain->EndDeferredInstancerDirty();
               }
            }
         }
         undoStack.EndScopedAction(&scene);
         state.BrushHeldLastFrame = false;
      }
      state.StampHeightValid = false;
      state.StampEntity = INVALID_ENTITY_ID;
      state.PaintEntity = INVALID_ENTITY_ID;
   };

   state.CursorValid = false;
   state.PaintingActive = false;
   bool leftMouseDown = Input::IsMouseButtonPressed(MouseButton::Left);
   if (!leftMouseDown) {
      endStroke();
   }

   if (!state.BrushModeEnabled)
   {
      endStroke();
#ifndef CLAYMORE_CORE
      ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
#endif
      return;
   }

   if (!viewportHovered)
   {
#ifndef CLAYMORE_CORE
      ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
#endif
      return;
   }

#ifndef CLAYMORE_CORE
   ImGui::SetMouseCursor(ImGuiMouseCursor_None);
#endif

   if (playMode || selectedEntity == 0) {
      endStroke();
      return;
   }
   auto* data = scene.GetEntityData(selectedEntity);
   if (!data || !data->Terrain) {
      endStroke();
      return;
   }

   TerrainComponent& terrain = *data->Terrain;
   Terrain::EnsureChunkLayout(terrain);
   if (state.StampEntity != selectedEntity) {
      state.StampHeightValid = false;
   }

   Renderer& renderer = Renderer::Get();
   float mouseNX = 0.0f;
   float mouseNY = 0.0f;
   if (!renderer.GetUIMouseNormalized(mouseNX, mouseNY))
      return;

   Camera* cam = viewportCamera ? viewportCamera : renderer.GetCamera();
   if (!cam)
      return;

   Ray ray = Picking::ScreenPointToRay(mouseNX, mouseNY, cam);
   glm::vec3 rayOrigin = ray.Origin;
   glm::vec3 rayDir = ray.Direction;

   TerrainHit hit;
   const bool ignoreHoleHits = (terrain.Brush.Mode == TerrainBrushMode::Hole);
   if (!ComputeTerrainHit(data->Transform, terrain, rayOrigin, rayDir, hit, ignoreHoleHits))
      return;

   bool paintEnabled = (selectedEntity != 0) && data && data->Terrain.get() == &terrain;
   uint32_t ringColor = paintEnabled ? 0xFF00FFFFu : 0x8800FFFFu;
   glm::vec3 ringNormal = terrain.Brush.AlignToNormal ? hit.WorldNormal : glm::vec3(0.0f, 1.0f, 0.0f);
   state.CursorValid = true;
   state.CursorWorldPos = hit.WorldPosition;
   state.CursorWorldNormal = ringNormal;
   const bool isHeightmapStampMode = (terrain.Brush.Mode == TerrainBrushMode::HeightmapStamp);
   const bool hasHeightmapStampData = HasHeightmapStampData(terrain.Brush);
   if (isHeightmapStampMode)
   {
      DrawBrushSquare(renderer, state.CursorWorldPos, state.CursorWorldNormal, terrain.Brush.Radius, ringColor);
   }
   else
   {
      renderer.DrawRing(state.CursorWorldPos, state.CursorWorldNormal, terrain.Brush.Radius, ringColor);
   }

  bool shiftPressed = Input::IsKeyPressed(KeyCode::LeftShift) || Input::IsKeyPressed(KeyCode::RightShift);
   bool brushDown = leftMouseDown;

   if (!paintEnabled || (isHeightmapStampMode && !hasHeightmapStampData)) {
      endStroke();
      return;
   }

   if (!brushDown)
      return;

   state.PaintingActive = true;

#ifndef CLAYMORE_CORE
   if (!allowSelectionChanges)
      ImGui::SetNextItemAllowOverlap();
#endif

   glm::vec4 worldToLocal = glm::inverse(data->Transform.WorldMatrix) * glm::vec4(hit.WorldPosition, 1.0f);
   glm::vec3 localBrushPos = glm::vec3(worldToLocal);
   glm::vec2 brushCenter(localBrushPos.x, localBrushPos.z);
   static int s_WorldLocPrints = 0;
   if (s_WorldLocPrints < 16)
   {
      std::cout << "[GrassBrush] worldPos=(" << hit.WorldPosition.x << ", " << hit.WorldPosition.y << ", " << hit.WorldPosition.z
         << ") local=(" << localBrushPos.x << ", " << localBrushPos.y << ", " << localBrushPos.z << ")" << std::endl;
      ++s_WorldLocPrints;
   }
   glm::ivec2 minDirty(std::numeric_limits<int>::max());
   glm::ivec2 maxDirty(std::numeric_limits<int>::min());

   // Shift+Left Click decreases height/texture, Left Click increases
   float direction = shiftPressed ? -1.0f : 1.0f;

  if (!state.BrushHeldLastFrame) {
      undoStack.BeginScopedAction(&scene, "Paint Terrain");
      state.BrushHeldLastFrame = true;
      state.PaintEntity = selectedEntity;
      terrain.BeginDeferredInstancerDirty();
      if (terrain.Brush.Mode == TerrainBrushMode::StampHeight) {
          glm::vec2 cell = Terrain::GetCellSize(terrain);
          cell.x = glm::max(cell.x, 0.0001f);
          cell.y = glm::max(cell.y, 0.0001f);
          int gridX = glm::clamp((int)glm::round(localBrushPos.x / cell.x), 0, (int)terrain.GridResolution - 1);
          int gridZ = glm::clamp((int)glm::round(localBrushPos.z / cell.y), 0, (int)terrain.GridResolution - 1);
          size_t idx = static_cast<size_t>(gridZ) * terrain.GridResolution + gridX;
          if (idx < terrain.HeightMap.size()) {
              state.StampHeightNormalized = terrain.HeightMap[idx] / 65535.0f;
              state.StampHeightValid = true;
              state.StampEntity = selectedEntity;
          }
      }
  }
  else if (isHeightmapStampMode)
  {
      return;
  }
  if (terrain.Brush.Mode == TerrainBrushMode::Height)
  {
      ApplyHeightPaint(terrain, brushCenter, glm::max(0.1f, terrain.Brush.Radius), direction, minDirty, maxDirty);
  }
  else if (terrain.Brush.Mode == TerrainBrushMode::Texture)
  {
      ApplyTexturePaint(terrain, brushCenter, glm::max(0.1f, terrain.Brush.Radius), direction, minDirty, maxDirty);
  }
  else if (terrain.Brush.Mode == TerrainBrushMode::Hole)
  {
      ApplyHolePaint(terrain, brushCenter, glm::max(0.1f, terrain.Brush.Radius), direction, minDirty, maxDirty);
  }
  else if (terrain.Brush.Mode == TerrainBrushMode::SmoothHeight)
  {
      ApplySmoothPaint(terrain, brushCenter, glm::max(0.1f, terrain.Brush.Radius), minDirty, maxDirty);
  }
  else if (terrain.Brush.Mode == TerrainBrushMode::StampHeight)
  {
      float target = state.StampHeightNormalized;
      if (!state.StampHeightValid) {
          float worldHeight = Terrain::SampleHeightWorld(terrain, brushCenter.x, brushCenter.y);
          float maxHeight = glm::max(0.0001f, terrain.MaxHeight);
          target = glm::clamp(worldHeight / maxHeight, 0.0f, 1.0f);
      }
      ApplyStampPaint(terrain, brushCenter, glm::max(0.1f, terrain.Brush.Radius), target, minDirty, maxDirty);
  }
  else if (terrain.Brush.Mode == TerrainBrushMode::HeightmapStamp)
  {
      ApplyHeightmapStampPaint(terrain, brushCenter, glm::max(0.1f, terrain.Brush.Radius), minDirty, maxDirty);
  }
  else if (terrain.Brush.Mode == TerrainBrushMode::ErosionNoise)
  {
      ApplyErosionNoisePaint(terrain, brushCenter, glm::max(0.1f, terrain.Brush.Radius), direction, minDirty, maxDirty);
  }
  else if (terrain.Brush.Mode == TerrainBrushMode::FlattenHeight)
  {
      float maxHeight = glm::max(0.0001f, terrain.MaxHeight);
      float targetNormalized = glm::clamp(terrain.Brush.FlattenTargetHeight / maxHeight, 0.0f, 1.0f);
      ApplyStampPaint(terrain, brushCenter, glm::max(0.1f, terrain.Brush.Radius), targetNormalized, minDirty, maxDirty);
  }
  else if (terrain.Brush.Mode == TerrainBrushMode::CliffStamp)
  {
      ApplyCliffStampPaint(terrain, brushCenter, glm::max(0.1f, terrain.Brush.Radius), direction, minDirty, maxDirty);
  }
  else if (terrain.Brush.Mode == TerrainBrushMode::MountainStamp)
  {
      ApplyMountainStampPaint(terrain, brushCenter, glm::max(0.1f, terrain.Brush.Radius), direction, minDirty, maxDirty);
  }
  else if (terrain.Brush.Mode == TerrainBrushMode::Instancer)
  {
      if (!terrain.InstancerLayers.empty())
      {
         int activeLayer = glm::clamp(terrain.Brush.ActiveInstancerLayer, 0, static_cast<int>(terrain.InstancerLayers.size()) - 1);
         ApplyInstancerPaint(
            terrain,
            static_cast<size_t>(activeLayer),
            brushCenter,
            glm::max(0.1f, terrain.Brush.Radius),
            direction * glm::max(0.01f, terrain.Brush.InstancerStrength),
            glm::max(0.01f, terrain.Brush.Falloff));
      }
  }
  else
  {
      if (!terrain.GrassLayers.empty())
      {
         int activeLayer = glm::clamp(terrain.Brush.ActiveGrassLayer, 0, static_cast<int>(terrain.GrassLayers.size()) - 1);
         TerrainGrass::ApplyPaint(
            terrain,
            static_cast<size_t>(activeLayer),
            brushCenter,
            glm::max(0.1f, terrain.Brush.Radius),
            direction * glm::max(0.01f, terrain.Brush.GrassStrength),
            glm::max(0.01f, terrain.Brush.Falloff));
      }
  }
}

bool TerrainPainter::IsPainting()
{
   return GetPainterState().PaintingActive;
}

void TerrainPainter::SetBrushModeEnabled(bool enabled)
{
   GetPainterState().BrushModeEnabled = enabled;
}

bool TerrainPainter::IsBrushModeEnabled()
{
   return GetPainterState().BrushModeEnabled;
}

bool TerrainPainter::ToggleBrushMode()
{
   TerrainPainterState& state = GetPainterState();
   state.BrushModeEnabled = !state.BrushModeEnabled;
   return state.BrushModeEnabled;
}
