#include "TerrainGrass.h"

#include "core/ecs/Components.h"
#include "core/ecs/Scene.h"
#include "core/ecs/EntityData.h"
#ifndef CLAYMORE_RUNTIME
#include "editor/pipeline/AssetLibrary.h"
#endif
#include "core/rendering/Renderer.h"
#include "core/rendering/MaterialCache.h"
#include "core/rendering/ShaderManager.h"
#include "core/rendering/Terrain.h"
#include "core/rendering/TerrainChunks.h"
#include "core/rendering/TextureLoader.h"
#include "core/jobs/Jobs.h"

#include <bx/timer.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <utility>
#include <atomic>
#include <unordered_set>

// CM_DEBUG_GRASS requires ImGui which is editor-only
// Disable grass debug in core library builds
#if defined(CLAYMORE_CORE)
#undef CM_DEBUG_GRASS
#define CM_DEBUG_GRASS 0
#endif

#if CM_DEBUG_GRASS
#include <imgui.h>
#endif

namespace TerrainGrass
   {
   namespace
      {
      constexpr uint32_t kGrassComputeGroupSize = 8;
      constexpr uint32_t kMaxBladesPerTexel = 4;
      constexpr uint32_t kCpuMinBladesPerTexel = 1;
      constexpr uint32_t kCpuMaxBladesPerTexel = 6;
      constexpr uint32_t kCpuMaxInstancesPerLayer = 65536;  // Reduced to stay within bgfx transient buffer limits
      constexpr float kCpuDensityThreshold = 0.01f;
      // bgfx uses one compute binding table for buffers and images, so every binding
      // used by a dispatch must have its own stage index.
      constexpr uint8_t kGrassResetCounterStage = 1;
      constexpr uint8_t kGrassResetIndirectArgsStage = 2;
      constexpr uint8_t kGrassGenerateInstanceStage = 0;
      constexpr uint8_t kGrassGenerateCounterStage = 1;
      constexpr uint8_t kGrassGenerateDensityStage = 2;
      constexpr uint8_t kGrassGenerateHeightStage = 3;
      constexpr uint8_t kGrassGenerateSplatStage = 4;
      constexpr uint8_t kGrassGenerateHoleStage = 5;
      constexpr uint8_t kGrassFinalizeCounterStage = 1;
      constexpr uint8_t kGrassFinalizeIndirectArgsStage = 2;
#if CM_DEBUG_GRASS
      constexpr uint8_t kGrassFinalizeDebugStage = 3;
#endif
      constexpr uint8_t kGrassGenerateRequiredBindings = kGrassGenerateHoleStage + 1;

#if CM_DEBUG_GRASS
      constexpr uint32_t kMaxDebugChunks = 16;
      constexpr uint32_t kMaxDebugLayersPerChunk = 32;
      constexpr uint32_t kMaxDebugEntries = kMaxDebugChunks * kMaxDebugLayersPerChunk;
      constexpr uint32_t kDebugTextureWidth = 64;
      constexpr uint32_t kDebugTextureHeight = (kMaxDebugEntries + kDebugTextureWidth - 1) / kDebugTextureWidth;

      struct GrassLayerDebugSample
         {
         ClaymoreGUID Guid{};
         std::string Name;
         uint32_t LayerIndex = 0;
         uint32_t ChunkIndex = 0;
         glm::ivec2 ChunkStart = glm::ivec2(0);
         glm::ivec2 ChunkSize = glm::ivec2(0);
         float DensitySample = 0.0f;
         float MaskSample = 1.0f;
         float ApproxBladesPerCell = 0.0f;
         float EstimatedInstances = 0.0f;
         uint32_t InstanceCount = 0;
         bool Submitted = false;
         bool ExpectVisible = false;
         bool TelemetryAvailable = false;
         bool DistanceGateActive = false;
         bool DistanceFullyRejected = false;
         bool DistancePartiallyRejected = false;
         bool SplatAvailable = false;
         GrassMaskSource MaskMode = GrassMaskSource::None;
         float MinDistance = 0.0f;
         float MaxDistance = 0.0f;
         glm::vec2 HeightRange = glm::vec2(0.0f);
         float SlopeLimit = 0.0f;
         glm::vec2 DistanceRange = glm::vec2(0.0f);
         float CenterDistance = 0.0f;
         };

      struct GrassDebugState
         {
         bool Enabled = false;
         bool FocusCameraChunkOnly = true;
         bool DumpRequested = false;
         bool TelemetrySupported = true;
         uint32_t FrameCounter = 0;
         uint32_t CameraChunkIndex = 0;
         uint32_t ReadbackPeriod = 12;
         bool PendingReadback = false;
         uint32_t PendingFrame = std::numeric_limits<uint32_t>::max();
         std::vector<uint8_t> ReadbackBuffer;
         std::vector<uint32_t> InstanceCounters;
         std::vector<GrassLayerDebugSample> Samples;
         uint32_t LastDumpFrame = 0;
         };

      GrassDebugState& GetDebugState()
         {
         static GrassDebugState s_State;
         if (s_State.InstanceCounters.size() != kMaxDebugEntries)
            {
            s_State.InstanceCounters.assign(kMaxDebugEntries, 0u);
            }
         return s_State;
         }

      uint32_t EncodeDebugSlot(uint32_t chunkIndex, uint32_t layerIndex)
         {
         if (chunkIndex >= kMaxDebugChunks || layerIndex >= kMaxDebugLayersPerChunk)
            return std::numeric_limits<uint32_t>::max();
         return chunkIndex * kMaxDebugLayersPerChunk + layerIndex;
         }
#endif

      struct GrassRendererResources
         {
         bgfx::VertexBufferHandle BillboardVB = BGFX_INVALID_HANDLE;
         bgfx::IndexBufferHandle BillboardIB = BGFX_INVALID_HANDLE;
         bgfx::ProgramHandle BillboardProgram = BGFX_INVALID_HANDLE;
         bgfx::ProgramHandle MeshProgram = BGFX_INVALID_HANDLE;
         bgfx::ProgramHandle ComputeResetProgram = BGFX_INVALID_HANDLE;
         bgfx::ProgramHandle ComputeGenerateProgram = BGFX_INVALID_HANDLE;
         bgfx::ProgramHandle ComputeFinalizeProgram = BGFX_INVALID_HANDLE;
         bgfx::UniformHandle s_Albedo = BGFX_INVALID_HANDLE;
         bgfx::UniformHandle s_GrassDensity = BGFX_INVALID_HANDLE; // Sampler for density texture in compute
         bgfx::UniformHandle u_LayerParams = BGFX_INVALID_HANDLE;
         bgfx::UniformHandle u_WindDir = BGFX_INVALID_HANDLE;
         bgfx::UniformHandle u_FadeParams = BGFX_INVALID_HANDLE;
         bgfx::UniformHandle u_Time = BGFX_INVALID_HANDLE;
         bgfx::UniformHandle u_BaseColor = BGFX_INVALID_HANDLE;
         bgfx::UniformHandle u_ColorVariance = BGFX_INVALID_HANDLE;
         bgfx::UniformHandle u_GrassTerrain0 = BGFX_INVALID_HANDLE;
         bgfx::UniformHandle u_GrassTerrain1 = BGFX_INVALID_HANDLE;
         bgfx::UniformHandle u_GrassLayer0 = BGFX_INVALID_HANDLE;
         bgfx::UniformHandle u_GrassLayer1 = BGFX_INVALID_HANDLE;
         bgfx::UniformHandle u_GrassMaskParams = BGFX_INVALID_HANDLE;
         bgfx::UniformHandle u_GrassCameraPos = BGFX_INVALID_HANDLE;
         bgfx::UniformHandle u_GrassCameraRight = BGFX_INVALID_HANDLE;
         bgfx::UniformHandle u_GrassCameraForward = BGFX_INVALID_HANDLE;
         bgfx::UniformHandle u_GrassWorld = BGFX_INVALID_HANDLE;
         bgfx::UniformHandle u_GrassNormal = BGFX_INVALID_HANDLE;
         bgfx::UniformHandle u_GrassDispatch = BGFX_INVALID_HANDLE;
         bgfx::UniformHandle u_GrassDrawArgs = BGFX_INVALID_HANDLE;
         bgfx::UniformHandle u_GrassSeedTime = BGFX_INVALID_HANDLE;
         bgfx::UniformHandle u_GrassTerrainInfo = BGFX_INVALID_HANDLE;
         bgfx::UniformHandle u_GrassDeformParams = BGFX_INVALID_HANDLE;
         bgfx::UniformHandle s_DeformationTex = BGFX_INVALID_HANDLE;
#if CM_DEBUG_GRASS
         bgfx::UniformHandle u_GrassDebugInfo = BGFX_INVALID_HANDLE;
         bgfx::TextureHandle DebugTexture = BGFX_INVALID_HANDLE;
         bgfx::TextureFormat::Enum DebugTextureFormat = bgfx::TextureFormat::RGBA32F;
         uint32_t DebugTextureWidth = kDebugTextureWidth;
         uint32_t DebugTextureHeight = kDebugTextureHeight;
#endif
         bgfx::TextureHandle WhiteTexture = BGFX_INVALID_HANDLE;
         bgfx::TextureHandle ComputeWhiteTexture = BGFX_INVALID_HANDLE;
         bgfx::TextureHandle ComputeSolidHoleTexture = BGFX_INVALID_HANDLE;
         bgfx::VertexLayout BillboardLayout;
         bgfx::VertexLayout InstanceLayout;
         bgfx::VertexLayoutHandle InstanceLayoutHandle = BGFX_INVALID_HANDLE;
         bool Initialized = false;
         };

      GrassRendererResources& GetResources()
         {
         static GrassRendererResources s_Resources;
         return s_Resources;
         }

      std::atomic<GrassGenerationMode>& GetGenerationModeAtomic()
         {
         static std::atomic<GrassGenerationMode> s_Mode{ GrassGenerationMode::Compute };
         return s_Mode;
         }

#if CM_DEBUG_GRASS
      glm::ivec2 ChunkCenterCell(const TerrainChunk& chunk)
         {
         const int spanX = std::max(1u, chunk.VertexCountX);
         const int spanZ = std::max(1u, chunk.VertexCountZ);
         return glm::ivec2(chunk.Start.x + spanX / 2, chunk.Start.y + spanZ / 2);
         }

      void ComputeChunkLocalBounds(const TerrainChunk& chunk, const glm::vec2& cellSize, glm::vec2& outMin, glm::vec2& outMax)
         {
         const glm::vec2 start(chunk.Start.x * cellSize.x, chunk.Start.y * cellSize.y);
         const int spanX = std::max(0, static_cast<int>(std::max(1u, chunk.VertexCountX)) - 1);
         const int spanZ = std::max(0, static_cast<int>(std::max(1u, chunk.VertexCountZ)) - 1);
         const glm::vec2 extent(spanX * cellSize.x, spanZ * cellSize.y);

         outMin = start;
         outMax = start + extent;
         }

      glm::vec3 SampleWorldPosition(const TerrainComponent& terrain, const glm::mat4& worldTransform, float localX, float localZ)
         {
         const float height = Terrain::SampleHeightWorld(terrain, localX, localZ);
         const glm::vec4 localPos(localX, height, localZ, 1.0f);
         const glm::vec4 worldPos = worldTransform * localPos;
         return glm::vec3(worldPos);
         }

      glm::vec3 SampleWorldPosition(const TerrainComponent& terrain, const glm::mat4& worldTransform, const glm::vec2& localXZ)
         {
         return SampleWorldPosition(terrain, worldTransform, localXZ.x, localXZ.y);
         }

      void ComputeChunkDistanceRange(const TerrainComponent& terrain,
         const TerrainChunk& chunk,
         const glm::mat4& worldTransform,
         const glm::vec3& cameraPos,
         glm::vec2& outRange,
         float& outCenterDistance)
         {
         const glm::vec2 cellSize = Terrain::GetCellSize(terrain);
         if (cellSize.x <= 0.0f || cellSize.y <= 0.0f)
            {
            outRange = glm::vec2(0.0f);
            outCenterDistance = 0.0f;
            return;
            }

         glm::vec2 chunkMin(0.0f);
         glm::vec2 chunkMax(0.0f);
         ComputeChunkLocalBounds(chunk, cellSize, chunkMin, chunkMax);
         const glm::vec2 chunkCenter = (chunkMin + chunkMax) * 0.5f;

         const glm::vec3 centerWorld = SampleWorldPosition(terrain, worldTransform, chunkCenter);
         float minDistance = glm::distance(cameraPos, centerWorld);
         float maxDistance = minDistance;

         const glm::vec2 corners[] = {
            glm::vec2(chunkMin.x, chunkMin.y),
            glm::vec2(chunkMax.x, chunkMin.y),
            glm::vec2(chunkMin.x, chunkMax.y),
            glm::vec2(chunkMax.x, chunkMax.y)
            };

         for (const glm::vec2& corner : corners)
            {
            const glm::vec3 cornerWorld = SampleWorldPosition(terrain, worldTransform, corner);
            const float d = glm::distance(cameraPos, cornerWorld);
            minDistance = std::min(minDistance, d);
            maxDistance = std::max(maxDistance, d);
            }

         outRange = glm::vec2(minDistance, maxDistance);
         outCenterDistance = glm::distance(cameraPos, centerWorld);
         }
#endif // CM_DEBUG_GRASS - end block to expose utility functions

      // These utility functions are needed by both debug and CPU generation code
      float SamplePaintDensity(const TerrainComponent& terrain, const TerrainGrassLayerDesc& layer, glm::ivec2 cell)
         {
         if (terrain.GridResolution == 0 || layer.PaintedMask.empty())
            return 0.0f;

         const int grid = static_cast<int>(terrain.GridResolution);
         cell = glm::clamp(cell, glm::ivec2(0), glm::ivec2(grid - 1));
         const size_t idx = static_cast<size_t>(cell.y) * terrain.GridResolution + static_cast<size_t>(cell.x);
         if (idx >= layer.PaintedMask.size())
            return 0.0f;
         return static_cast<float>(layer.PaintedMask[idx]) / 255.0f;
         }

      // Bilinear-interpolated paint density sampling at normalized UV coordinates [0,1]
      float SamplePaintDensityBilinear(const TerrainComponent& terrain, const TerrainGrassLayerDesc& layer, float u, float v)
         {
         if (terrain.GridResolution < 2 || layer.PaintedMask.empty())
            return 0.0f;

         const uint32_t res = terrain.GridResolution;
         const float maxCoord = static_cast<float>(res - 1);
         const float xf = glm::clamp(u * maxCoord, 0.0f, maxCoord);
         const float yf = glm::clamp(v * maxCoord, 0.0f, maxCoord);
         
         const uint32_t x0 = static_cast<uint32_t>(std::floor(xf));
         const uint32_t y0 = static_cast<uint32_t>(std::floor(yf));
         const uint32_t x1 = std::min(x0 + 1, res - 1);
         const uint32_t y1 = std::min(y0 + 1, res - 1);
         const float tx = xf - static_cast<float>(x0);
         const float ty = yf - static_cast<float>(y0);
         
         const float v00 = static_cast<float>(layer.PaintedMask[y0 * res + x0]) / 255.0f;
         const float v10 = static_cast<float>(layer.PaintedMask[y0 * res + x1]) / 255.0f;
         const float v01 = static_cast<float>(layer.PaintedMask[y1 * res + x0]) / 255.0f;
         const float v11 = static_cast<float>(layer.PaintedMask[y1 * res + x1]) / 255.0f;
         
         const float vx0 = glm::mix(v00, v10, tx);
         const float vx1 = glm::mix(v01, v11, tx);
         return glm::mix(vx0, vx1, ty);
         }

      glm::u8vec4 SampleSplatValue(const TerrainComponent& terrain, glm::ivec2 cell)
         {
         if (terrain.GridResolution == 0 || terrain.SplatMap.empty())
            return glm::u8vec4(255);

         const int grid = static_cast<int>(terrain.GridResolution);
         cell = glm::clamp(cell, glm::ivec2(0), glm::ivec2(grid - 1));
         const size_t idx = static_cast<size_t>(cell.y) * terrain.GridResolution + static_cast<size_t>(cell.x);
         if (idx >= terrain.SplatMap.size())
            return glm::u8vec4(255);
         return terrain.SplatMap[idx];
         }

      // Bilinear-interpolated splat sampling at normalized UV coordinates [0,1]
      glm::vec4 SampleSplatValueBilinear(const TerrainComponent& terrain, float u, float v)
         {
         if (terrain.GridResolution < 2 || terrain.SplatMap.empty())
            return glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);

         const uint32_t res = terrain.GridResolution;
         const float maxCoord = static_cast<float>(res - 1);
         const float xf = glm::clamp(u * maxCoord, 0.0f, maxCoord);
         const float yf = glm::clamp(v * maxCoord, 0.0f, maxCoord);
         
         const uint32_t x0 = static_cast<uint32_t>(std::floor(xf));
         const uint32_t y0 = static_cast<uint32_t>(std::floor(yf));
         const uint32_t x1 = std::min(x0 + 1, res - 1);
         const uint32_t y1 = std::min(y0 + 1, res - 1);
         const float tx = xf - static_cast<float>(x0);
         const float ty = yf - static_cast<float>(y0);
         
         const glm::vec4 v00 = glm::vec4(terrain.SplatMap[y0 * res + x0]) / 255.0f;
         const glm::vec4 v10 = glm::vec4(terrain.SplatMap[y0 * res + x1]) / 255.0f;
         const glm::vec4 v01 = glm::vec4(terrain.SplatMap[y1 * res + x0]) / 255.0f;
         const glm::vec4 v11 = glm::vec4(terrain.SplatMap[y1 * res + x1]) / 255.0f;
         
         const glm::vec4 vx0 = glm::mix(v00, v10, tx);
         const glm::vec4 vx1 = glm::mix(v01, v11, tx);
         return glm::mix(vx0, vx1, ty);
         }

      float MaskSampleValue(GrassMaskSource mode, const glm::u8vec4& splat, bool hasSplat, float densitySample)
         {
         switch (mode)
            {
            case GrassMaskSource::SplatR: return hasSplat ? splat.r / 255.0f : 1.0f;
            case GrassMaskSource::SplatG: return hasSplat ? splat.g / 255.0f : 1.0f;
            case GrassMaskSource::SplatB: return hasSplat ? splat.b / 255.0f : 1.0f;
            case GrassMaskSource::SplatA: return hasSplat ? splat.a / 255.0f : 1.0f;
            case GrassMaskSource::Painted: return densitySample;
            case GrassMaskSource::None:
            default:
               return 1.0f;
            }
         }

      // Overload for pre-normalized vec4 splat values (from bilinear sampling)
      float MaskSampleValueNormalized(GrassMaskSource mode, const glm::vec4& splat, bool hasSplat, float densitySample)
         {
         switch (mode)
            {
            case GrassMaskSource::SplatR: return hasSplat ? splat.r : 1.0f;
            case GrassMaskSource::SplatG: return hasSplat ? splat.g : 1.0f;
            case GrassMaskSource::SplatB: return hasSplat ? splat.b : 1.0f;
            case GrassMaskSource::SplatA: return hasSplat ? splat.a : 1.0f;
            case GrassMaskSource::Painted: return densitySample;
            case GrassMaskSource::None:
            default:
               return 1.0f;
            }
         }

      // EnsureDebugTexture and other debug functions are inside CM_DEBUG_GRASS block below
#if CM_DEBUG_GRASS
      bool EnsureDebugTexture(GrassRendererResources& res)
         {
         const bgfx::Caps* caps = bgfx::getCaps();
         if (!caps || !(caps->supported & BGFX_CAPS_COMPUTE))
            {
            return false;
            }
         if (bgfx::isValid(res.DebugTexture))
            return true;

         const uint64_t textureFlags = BGFX_TEXTURE_COMPUTE_WRITE | BGFX_TEXTURE_READ_BACK | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
         const bgfx::TextureFormat::Enum candidates[] = {
            bgfx::TextureFormat::RGBA32F,
            bgfx::TextureFormat::RGBA16F,
            bgfx::TextureFormat::RGBA32U
            };

         bgfx::TextureFormat::Enum selected = bgfx::TextureFormat::Count;
         for (bgfx::TextureFormat::Enum fmt : candidates)
            {
            if (bgfx::isTextureValid(0, false, 1, fmt, textureFlags))
               {
               selected = fmt;
               break;
               }
            }

         if (selected == bgfx::TextureFormat::Count)
            {
            std::cerr << "[TerrainGrass] Grass debug telemetry unavailable: required texture format not supported on this device." << std::endl;
            return false;
            }

         res.DebugTextureFormat = selected;
         res.DebugTexture = bgfx::createTexture2D(
            static_cast<uint16_t>(res.DebugTextureWidth),
            static_cast<uint16_t>(res.DebugTextureHeight),
            false,
            1,
            selected,
            textureFlags);

         if (!bgfx::isValid(res.DebugTexture))
            {
            std::cerr << "[TerrainGrass] Failed to allocate grass debug texture (format " << selected << "). Telemetry disabled." << std::endl;
            return false;
            }
         return true;
         }

      void ProcessDebugReadback(GrassRendererResources& res, GrassDebugState& state)
         {
         if (!state.PendingReadback)
            return;

         if (state.PendingFrame > Renderer::Get().GetLastSubmittedFrame())
            return;

         state.PendingReadback = false;
         const size_t pixelCount = static_cast<size_t>(res.DebugTextureWidth) * static_cast<size_t>(res.DebugTextureHeight);
         const size_t expectedBytes = pixelCount * sizeof(float) * 4;
         if (state.ReadbackBuffer.size() < expectedBytes)
            return;

         const float* samples = reinterpret_cast<const float*>(state.ReadbackBuffer.data());
         for (size_t i = 0; i < std::min(pixelCount, state.InstanceCounters.size()); ++i)
            {
            const float instanceValue = samples[i * 4 + 0];
            state.InstanceCounters[i] = instanceValue > 0.0f ? static_cast<uint32_t>(instanceValue + 0.5f) : 0u;
            }
         }

      void RequestDebugReadback(GrassRendererResources& res, GrassDebugState& state, uint32_t usedEntries)
         {
         if (usedEntries == 0 || state.PendingReadback)
            return;
         if (state.ReadbackPeriod == 0)
            state.ReadbackPeriod = 1;
         if ((state.FrameCounter % state.ReadbackPeriod) != 0)
            return;

         const size_t pixelCount = static_cast<size_t>(res.DebugTextureWidth) * static_cast<size_t>(res.DebugTextureHeight);
         state.ReadbackBuffer.resize(pixelCount * sizeof(float) * 4);
         state.PendingFrame = bgfx::readTexture(res.DebugTexture, state.ReadbackBuffer.data());
         state.PendingReadback = state.PendingFrame != std::numeric_limits<uint32_t>::max();
         }

      const char* MaskSourceToString(GrassMaskSource source)
         {
         switch (source)
            {
            case GrassMaskSource::None:   return "None";
            case GrassMaskSource::SplatR: return "Splat R";
            case GrassMaskSource::SplatG: return "Splat G";
            case GrassMaskSource::SplatB: return "Splat B";
            case GrassMaskSource::SplatA: return "Splat A";
            case GrassMaskSource::Painted:return "Painted";
            default: return "Unknown";
            }
         }
#endif // CM_DEBUG_GRASS - pause for shared utility functions

      // These functions are used by both debug and CPU grass generation
      bool UsesSplatMask(GrassMaskSource source)
         {
         switch (source)
            {
            case GrassMaskSource::SplatR:
            case GrassMaskSource::SplatG:
            case GrassMaskSource::SplatB:
            case GrassMaskSource::SplatA:
               return true;
            default:
               return false;
            }
         }

      uint32_t Hash32(uint32_t value)
         {
         value ^= value >> 16;
         value *= 0x7feb352d;
         value ^= value >> 15;
         value *= 0x846ca68b;
         value ^= value >> 16;
         return value;
         }

      float RandomFloat01(uint32_t& seed)
         {
         seed = Hash32(seed);
         return static_cast<float>(seed) / 4294967295.0f;
         }

      float SmoothStep(float t)
         {
         return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
         }

      float HashToFloat01(uint32_t value)
         {
         return static_cast<float>(value) / 4294967295.0f;
         }

      float ValueNoise2D(const glm::vec2& p, uint32_t seed)
         {
         const int x0 = static_cast<int>(std::floor(p.x));
         const int y0 = static_cast<int>(std::floor(p.y));
         const float fx = p.x - static_cast<float>(x0);
         const float fy = p.y - static_cast<float>(y0);

         auto corner = [&](int x, int y)
            {
            const uint32_t hx = static_cast<uint32_t>(x) * 0x8da6b343u;
            const uint32_t hy = static_cast<uint32_t>(y) * 0xd8163841u;
            return HashToFloat01(Hash32(hx ^ hy ^ seed));
            };

         const float a = corner(x0, y0);
         const float b = corner(x0 + 1, y0);
         const float c = corner(x0, y0 + 1);
         const float d = corner(x0 + 1, y0 + 1);

         const float u = SmoothStep(fx);
         const float v = SmoothStep(fy);

         const float ab = glm::mix(a, b, u);
         const float cd = glm::mix(c, d, u);
         return glm::mix(ab, cd, v);
         }

      float SampleSplatNoise(const glm::vec2& pos, float scale, float strength, uint32_t seed)
         {
         if (strength <= 0.0f || scale <= 0.0f)
            return 1.0f;
         const float noise = ValueNoise2D(pos * scale, seed);
         const float t = glm::clamp(strength, 0.0f, 1.0f);
         return glm::mix(1.0f, noise, t);
         }

      bool SphereInFrustum(const terrain::ChunkFrustum& frustum, const glm::vec3& center, float radius)
         {
         for (int i = 0; i < 6; ++i)
            {
            const glm::vec4& p = frustum.Planes[i];
            if (glm::dot(glm::vec3(p), center) + p.w < -radius)
               return false;
            }
         return true;
         }

#if CM_DEBUG_GRASS // Resume debug-only code
      std::string DescribeMaskSource(const GrassLayerDebugSample& sample)
         {
         const char* label = MaskSourceToString(sample.MaskMode);
         if (sample.MaskMode == GrassMaskSource::Painted)
            return "Painted";
         if (sample.MaskMode == GrassMaskSource::None)
            return "None";
         if (UsesSplatMask(sample.MaskMode))
            {
            if (sample.SplatAvailable)
               {
               return label;
               }
            std::string missing = label;
            missing += " missing";
            return missing;
            }
         return std::string(label ? label : "Unknown");
         }

      const char* DescribeDistanceState(const GrassLayerDebugSample& sample)
         {
         if (!sample.DistanceGateActive)
            return "Off";
         if (sample.DistanceFullyRejected)
            return "Cull";
         if (sample.DistancePartiallyRejected)
            return "Clip";
         return "InRange";
         }

      std::string BuildSampleNotes(const GrassLayerDebugSample& sample)
         {
         std::ostringstream notes;
         notes << (sample.Submitted ? "Draw" : "Skipped");
         notes << " | " << (sample.TelemetryAvailable ? "GPU counter" : "Estimate");
         notes << " | Mask: " << DescribeMaskSource(sample);
         notes << " | Dist: " << DescribeDistanceState(sample);
         if (sample.TelemetryAvailable)
            {
            notes << " | Instances: " << sample.InstanceCount;
            if (sample.ExpectVisible && sample.InstanceCount == 0)
               {
               notes << " | Zero output";
               }
            }
         else
            {
            notes << " | Est: " << static_cast<uint32_t>(std::round(sample.EstimatedInstances));
            notes << " | No telemetry";
            }
         return notes.str();
         }

      uint32_t FindCameraChunkIndex(const TerrainComponent& terrain, const glm::vec3& cameraPos, const glm::vec2& cellSize)
         {
         if (terrain.Chunks.empty())
            return 0;

         for (uint32_t i = 0; i < terrain.Chunks.size(); ++i)
            {
            const TerrainChunk& chunk = terrain.Chunks[i];
            const glm::vec2 origin(chunk.Start.x * cellSize.x, chunk.Start.y * cellSize.y);
            const glm::vec2 extent(
               static_cast<float>(std::max(1u, chunk.VertexCountX)) * cellSize.x,
               static_cast<float>(std::max(1u, chunk.VertexCountZ)) * cellSize.y);

            if (cameraPos.x >= origin.x && cameraPos.x <= origin.x + extent.x &&
               cameraPos.z >= origin.y && cameraPos.z <= origin.y + extent.y)
               {
               return i;
               }
            }
         return 0;
         }

      void PushLayerSample(const TerrainComponent& terrain,
         const TerrainGrassLayerDesc& layer,
         const TerrainChunk& chunk,
         uint32_t layerIndex,
         uint32_t chunkIndex,
         uint32_t debugSlot,
         bool drawSubmitted,
         const glm::mat4& worldTransform,
         const glm::vec3& cameraPos,
         bool telemetryAvailable)
         {
         GrassDebugState& state = GetDebugState();
         GrassLayerDebugSample sample;
         sample.Guid = layer.Guid;
         sample.Name = layer.Name;
         sample.LayerIndex = layerIndex;
         sample.ChunkIndex = chunkIndex;
         sample.ChunkStart = chunk.Start;
         sample.ChunkSize = glm::ivec2(chunk.VertexCountX, chunk.VertexCountZ);
         const glm::ivec2 cell = ChunkCenterCell(chunk);
         sample.DensitySample = SamplePaintDensity(terrain, layer, cell);
         const glm::u8vec4 splat = SampleSplatValue(terrain, cell);
         const bool splatAvailable = bgfx::isValid(chunk.SplatTexture);
         sample.MaskSample = MaskSampleValue(layer.Mask, splat, splatAvailable, sample.DensitySample);
         const glm::vec2 cellSize = Terrain::GetCellSize(terrain);
         const float cellArea = std::max(cellSize.x * cellSize.y, 1e-6f);
         sample.ApproxBladesPerCell = sample.DensitySample * layer.DensityPerSquareMeter * cellArea;
         const bool canReadTelemetry = telemetryAvailable && debugSlot < state.InstanceCounters.size();
         sample.InstanceCount = canReadTelemetry ? state.InstanceCounters[debugSlot] : 0u;
         sample.TelemetryAvailable = canReadTelemetry;
         sample.Submitted = drawSubmitted;
         glm::vec2 chunkDistanceRange(0.0f);
         float chunkCenterDistance = 0.0f;
         ComputeChunkDistanceRange(terrain, chunk, worldTransform, cameraPos, chunkDistanceRange, chunkCenterDistance);
         sample.DistanceRange = chunkDistanceRange;
         sample.CenterDistance = chunkCenterDistance;
         sample.DistanceGateActive = (layer.MaxDistance > 0.0f) && (layer.MaxDistance > layer.MinDistance + 0.001f);
         const bool beyondMax = sample.DistanceGateActive && (chunkDistanceRange.x > layer.MaxDistance);
         const bool beforeMin = sample.DistanceGateActive && (chunkDistanceRange.y < layer.MinDistance);
         sample.DistanceFullyRejected = sample.DistanceGateActive && (beyondMax || beforeMin);
         const bool straddlesMin = sample.DistanceGateActive && chunkDistanceRange.x < layer.MinDistance;
         const bool straddlesMax = sample.DistanceGateActive && chunkDistanceRange.y > layer.MaxDistance;
         sample.DistancePartiallyRejected = sample.DistanceGateActive && !sample.DistanceFullyRejected && (straddlesMin || straddlesMax);
         const bool densityOk = sample.DensitySample > 0.01f;
         const bool maskOk = sample.MaskSample > 0.01f;
         const bool distanceOk = !sample.DistanceGateActive || !sample.DistanceFullyRejected;
         sample.ExpectVisible = drawSubmitted && densityOk && maskOk && distanceOk;
         sample.SplatAvailable = splatAvailable;
         sample.MaskMode = layer.Mask;
         sample.MinDistance = layer.MinDistance;
         sample.MaxDistance = layer.MaxDistance;
         sample.HeightRange = layer.HeightRange;
         sample.SlopeLimit = layer.MaxSlopeDegrees;
         const float chunkCellCount = static_cast<float>(std::max(1u, chunk.VertexCountX)) * static_cast<float>(std::max(1u, chunk.VertexCountZ));
         sample.EstimatedInstances = sample.ApproxBladesPerCell * chunkCellCount;
         state.Samples.push_back(std::move(sample));
         }

      void DumpGrassDebugToLog(const GrassDebugState& state)
         {
         for (const GrassLayerDebugSample& sample : state.Samples)
            {
            std::cout << "[GrassDebug] chunk=" << sample.ChunkIndex
               << " layer=" << sample.LayerIndex
               << " density=" << sample.DensitySample
               << " mask=" << sample.MaskSample
               << " approxBlades=" << sample.ApproxBladesPerCell
               << " estInstances=" << sample.EstimatedInstances
               << " instances=" << sample.InstanceCount
               << " telemetry=" << (sample.TelemetryAvailable ? "gpu" : "est")
               << " distRange=" << sample.MinDistance << "-" << sample.MaxDistance
               << " chunkDist=" << sample.DistanceRange.x << "-" << sample.DistanceRange.y
               << " distState=" << (sample.DistanceGateActive
                  ? (sample.DistanceFullyRejected ? "culled" : (sample.DistancePartiallyRejected ? "clip" : "ok"))
                  : "off")
               << " heightRange=" << sample.HeightRange.x << "-" << sample.HeightRange.y
               << " slope<=" << sample.SlopeLimit
               << " maskMode=" << MaskSourceToString(sample.MaskMode)
               << " submitted=" << (sample.Submitted ? "true" : "false")
               << std::endl;
            }
         }
#endif // CM_DEBUG_GRASS

      struct BillboardVertex
         {
         float x, y, z;
         float u, v;
         };

      float GetTimeSeconds()
         {
         static uint64_t startCounter = bx::getHPCounter();
         const uint64_t now = bx::getHPCounter();
         const double freq = double(bx::getHPFrequency());
         return static_cast<float>((double(now - startCounter) / freq));
         }

      glm::ivec2 ClampCell(const glm::ivec2& cell, int maxValue)
         {
         const glm::ivec2 minBound(0);
         const glm::ivec2 maxBound(maxValue);
         return glm::clamp(cell, minBound, maxBound);
         }

      void EnsureLayerHandles(TerrainGrassLayerDesc& layer)
         {
         if (layer.BillboardTexturePath.empty())
            {
            layer.BillboardTexture = BGFX_INVALID_HANDLE;
            }
         else
            {
            try
               {
               TextureSpecifier spec;
               spec.Path = layer.BillboardTexturePath;
               layer.BillboardTexture = AcquireTextureHandle(spec, TextureColorSpace::Linear);
               }
            catch (...)
               {
               layer.BillboardTexture = BGFX_INVALID_HANDLE;
               }
            }

         if (layer.RenderMode == GrassRenderMode::Mesh && layer.MeshAsset.IsValid())
            {
#ifndef CLAYMORE_RUNTIME
            layer.Mesh = AssetLibrary::Instance().LoadMesh(layer.MeshAsset);
            if (layer.MeshAsset.IsValid() && layer.MeshPath.empty())
               {
               if (AssetEntry* entry = AssetLibrary::Instance().GetAsset(layer.MeshAsset))
                  {
                  layer.MeshPath = entry->path;
                  }
               }
#endif
            }
         else if (layer.RenderMode != GrassRenderMode::Billboard && layer.RenderMode != GrassRenderMode::BillboardFixed)
            {
            // Only mesh mode, but no valid asset
            layer.Mesh.reset();
            }
         }

      void UploadDensityTexture(TerrainComponent& terrain, TerrainGrassLayerDesc& layer)
         {
         // Early exit if density is not dirty - prevents per-frame texture destruction/recreation
         // and bgfx::copy() allocations that cause memory exhaustion over time
         if (!layer.DensityDirty && !terrain.GrassMasksDirty && bgfx::isValid(layer.DensityTexture))
            {
            return;
            }
            
         layer.EnsureMaskSize(terrain.GridResolution);
         const uint32_t resolution = std::max(terrain.GridResolution, 2u);
         const size_t expected = static_cast<size_t>(resolution) * resolution;
         if (layer.PaintedMask.size() != expected)
            {
            layer.PaintedMask.assign(expected, 0);
            }

         // Validate that we have valid data before attempting to upload
         if (layer.PaintedMask.empty())
            {
            std::cerr << "[TerrainGrass] PaintedMask is empty, cannot upload density texture." << std::endl;
            return;
            }

         // Validate size fits in uint32_t (BGFX requirement)
         constexpr size_t maxUint32 = static_cast<size_t>(std::numeric_limits<uint32_t>::max());
         if (layer.PaintedMask.size() > maxUint32)
            {
            std::cerr << "[TerrainGrass] PaintedMask size (" << layer.PaintedMask.size() 
                      << ") exceeds maximum (" << maxUint32 << ")." << std::endl;
            return;
            }

         // Validate resolution is within reasonable limits (BGFX texture size limits)
         // Most GPUs support up to 16384x16384, but we'll be conservative
         if (resolution > 8192u)
            {
            std::cerr << "[TerrainGrass] Resolution (" << resolution << ") exceeds maximum (8192)." << std::endl;
            return;
            }

         // Always destroy and recreate the texture with data to ensure the upload works
         // reliably across all bgfx backends. Using updateTexture2D can be unreliable
         // for textures used in compute shaders.
         const uint64_t textureFlags =
            BGFX_TEXTURE_COMPUTE_WRITE |
            BGFX_SAMPLER_U_CLAMP |
            BGFX_SAMPLER_V_CLAMP;

         if (bgfx::isValid(layer.DensityTexture))
            {
            bgfx::destroy(layer.DensityTexture);
            layer.DensityTexture = BGFX_INVALID_HANDLE;
            }

         // Create texture WITH initial data (more reliable than updateTexture2D)
         // Validate data pointer is valid before calling bgfx::copy
         const void* maskData = layer.PaintedMask.data();
         if (!maskData)
            {
            std::cerr << "[TerrainGrass] PaintedMask.data() returned nullptr." << std::endl;
            return;
            }

         const uint32_t maskSize = static_cast<uint32_t>(layer.PaintedMask.size());
         if (maskSize == 0)
            {
            std::cerr << "[TerrainGrass] PaintedMask size is 0, cannot upload." << std::endl;
            return;
            }

         const bgfx::Memory* mem = bgfx::copy(maskData, maskSize);
         if (!mem)
            {
            std::cerr << "[TerrainGrass] bgfx::copy failed for density texture (" << resolution << "x" << resolution 
                      << ", size=" << maskSize << " bytes)." << std::endl;
            return;
            }

         layer.DensityTexture = bgfx::createTexture2D(
            resolution,
            resolution,
            false,
            1,
            bgfx::TextureFormat::R8,
            textureFlags,
            mem);
            
         if (!bgfx::isValid(layer.DensityTexture))
            {
            std::cerr << "[TerrainGrass] Failed to create density texture (" << resolution << "x" << resolution << ")." << std::endl;
            return;
            }
         layer.DensityTextureResolution = resolution;

         layer.DensityDirty = false;
         layer.DensityDirtyMin = glm::ivec2(0);
         layer.DensityDirtyMax = glm::ivec2(static_cast<int>(resolution) - 1);
         }

      GrassLayerRuntime* EnsureRuntimeBuffers(TerrainGrassLayerDesc& layer, uint32_t capacity, const bgfx::VertexLayout& layout)
         {
         capacity = std::max(capacity, 1u);
         if (!layer.Runtime)
            {
            layer.Runtime = std::make_unique<GrassLayerRuntime>();
            }

         GrassLayerRuntime& runtime = *layer.Runtime;
         if (runtime.Capacity == capacity &&
            bgfx::isValid(runtime.InstanceBuffer) &&
            bgfx::isValid(runtime.CounterTexture) &&
            bgfx::isValid(runtime.IndirectArgs))
            {
            return &runtime;
            }

         runtime.Reset();
         runtime.Capacity = capacity;

         runtime.InstanceBuffer = bgfx::createDynamicVertexBuffer(
            capacity,
            layout,
            BGFX_BUFFER_COMPUTE_WRITE | BGFX_BUFFER_ALLOW_RESIZE);

         // Create counter as 1x1 R32U texture for atomic counter UAV
         // Using texture instead of buffer because DynamicIndexBuffer UAV binding has issues
         runtime.CounterTexture = bgfx::createTexture2D(
            1, 1, false, 1,
            bgfx::TextureFormat::R32U,
            BGFX_TEXTURE_COMPUTE_WRITE | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);

         runtime.IndirectArgs = bgfx::createIndirectBuffer(1);

         if (!bgfx::isValid(runtime.InstanceBuffer) ||
            !bgfx::isValid(runtime.CounterTexture) ||
            !bgfx::isValid(runtime.IndirectArgs))
            {
            runtime.Reset();
            std::cerr << "[TerrainGrass] Failed to allocate grass compute buffers." << std::endl;
            return nullptr;
            }

         return &runtime;
         }

      void DispatchReset(uint16_t viewId, GrassRendererResources& res, GrassLayerRuntime& runtime)
         {
         // Validate buffers before dispatch
         if (!bgfx::isValid(runtime.CounterTexture) || !bgfx::isValid(runtime.IndirectArgs))
            {
            static bool s_WarnedReset = false;
            if (!s_WarnedReset)
               {
               s_WarnedReset = true;
               std::cerr << "[TerrainGrass] DispatchReset: Invalid buffer handles. Counter=" 
                  << bgfx::isValid(runtime.CounterTexture) << " Indirect=" << bgfx::isValid(runtime.IndirectArgs) << std::endl;
               }
            return;
            }
         
         // Use unique compute stages per dispatch so bgfx doesn't overwrite bindings.
         bgfx::setImage(kGrassResetCounterStage, runtime.CounterTexture, 0, bgfx::Access::ReadWrite, bgfx::TextureFormat::R32U);
         bgfx::setBuffer(kGrassResetIndirectArgsStage, runtime.IndirectArgs, bgfx::Access::ReadWrite);
         const glm::vec4 drawArgs(6.0f, 0.0f, 0.0f, 0.0f);
         bgfx::setUniform(res.u_GrassDrawArgs, &drawArgs.x);
         bgfx::dispatch(viewId, res.ComputeResetProgram, 1, 1, 1);
         }

      void DispatchFinalize(uint16_t viewId,
         GrassRendererResources& res,
         GrassLayerRuntime& runtime,
         uint32_t capacity
#if CM_DEBUG_GRASS
         , int debugSlot
         , bool debugEnabled
#endif
      )
         {
         if (!bgfx::isValid(runtime.CounterTexture) || !bgfx::isValid(runtime.IndirectArgs))
            {
            return;
            }
         bgfx::setImage(kGrassFinalizeCounterStage, runtime.CounterTexture, 0, bgfx::Access::ReadWrite, bgfx::TextureFormat::R32U);
         bgfx::setBuffer(kGrassFinalizeIndirectArgsStage, runtime.IndirectArgs, bgfx::Access::ReadWrite);
         const glm::vec4 drawArgs(6.0f, static_cast<float>(capacity), 0.0f, 0.0f);
         bgfx::setUniform(res.u_GrassDrawArgs, &drawArgs.x);
#if CM_DEBUG_GRASS
         glm::vec4 debugInfo(-1.0f, 0.0f, 0.0f, 0.0f);
         if (debugEnabled && debugSlot >= 0 && bgfx::isValid(res.DebugTexture) && bgfx::isValid(res.u_GrassDebugInfo))
            {
            bgfx::setImage(kGrassFinalizeDebugStage, res.DebugTexture, 0, bgfx::Access::Write, bgfx::TextureFormat::RGBA32F);
            debugInfo = glm::vec4(
               static_cast<float>(debugSlot),
               static_cast<float>(res.DebugTextureWidth),
               1.0f,
               0.0f);
            }
         if (bgfx::isValid(res.u_GrassDebugInfo))
            {
            bgfx::setUniform(res.u_GrassDebugInfo, &debugInfo.x);
            }
#endif
         bgfx::dispatch(viewId, res.ComputeFinalizeProgram, 1, 1, 1);
         }

      void DispatchGenerate(uint16_t viewId,
         GrassRendererResources& res,
         const TerrainComponent& terrain,
         const TerrainGrassLayerDesc& layer,
         GrassLayerRuntime& runtime,
         const glm::mat4& worldTransform,
         const glm::mat3& normalMatrix,
         const glm::vec3& cameraPos,
         const glm::vec3& cameraRight,
         const glm::vec3& cameraForward,
         const TerrainChunk& chunk)
         {
         if (!bgfx::isValid(layer.DensityTexture) || !bgfx::isValid(runtime.InstanceBuffer))
            return;
         if (!bgfx::isValid(runtime.CounterTexture) || !bgfx::isValid(runtime.IndirectArgs))
            return;

         // bgfx stores compute bindings in one stage table, so every image/buffer bound
         // by this dispatch must use a unique slot.
         bgfx::setBuffer(kGrassGenerateInstanceStage, runtime.InstanceBuffer, bgfx::Access::Write);
         bgfx::setImage(kGrassGenerateCounterStage, runtime.CounterTexture, 0, bgfx::Access::ReadWrite, bgfx::TextureFormat::R32U);
         bgfx::setImage(kGrassGenerateDensityStage, layer.DensityTexture, 0, bgfx::Access::Read, bgfx::TextureFormat::R8);
         bgfx::setImage(kGrassGenerateHeightStage, chunk.HeightTexture, 0, bgfx::Access::Read, bgfx::TextureFormat::R32F);

         bgfx::TextureHandle splatHandle = chunk.SplatTexture;
         if (!bgfx::isValid(splatHandle))
            {
            splatHandle = GetResources().ComputeWhiteTexture;
            }
         bgfx::setImage(kGrassGenerateSplatStage, splatHandle, 0, bgfx::Access::Read, bgfx::TextureFormat::RGBA8);

         bgfx::TextureHandle holeHandle = chunk.HoleTexture;
         if (!bgfx::isValid(holeHandle))
            {
            holeHandle = GetResources().ComputeSolidHoleTexture;
            }
         bgfx::setImage(kGrassGenerateHoleStage, holeHandle, 0, bgfx::Access::Read, bgfx::TextureFormat::R8);

         // Apply grass sampling multiplier for denser grass on low-res terrains
         const uint32_t samplingMult = std::clamp(terrain.GrassSamplingMultiplier, 1u, 16u);
         const uint32_t grassGridResolution = terrain.GridResolution * samplingMult;
         const glm::vec2 terrainCellSize = Terrain::GetCellSize(terrain);
         const glm::vec2 cellSize = terrainCellSize / static_cast<float>(samplingMult);
         const float cellArea = cellSize.x * cellSize.y;
         const glm::vec4 terrain0(cellSize.x, cellSize.y, cellArea, terrain.MaxHeight);
         const glm::vec4 terrain1(
            static_cast<float>(grassGridResolution),
            grassGridResolution > 1 ? 1.0f / static_cast<float>(grassGridResolution - 1) : 1.0f,
            static_cast<float>(runtime.Capacity),
            static_cast<float>(kMaxBladesPerTexel));

         bgfx::setUniform(res.u_GrassTerrain0, &terrain0.x);
         bgfx::setUniform(res.u_GrassTerrain1, &terrain1.x);

         const glm::vec4 layer0(
            layer.DensityPerSquareMeter,
            layer.HeightRange.x,
            layer.HeightRange.y,
            glm::radians(layer.MaxSlopeDegrees));

         const glm::vec4 layer1(
            layer.ScaleRange.x,
            layer.ScaleRange.y,
            glm::radians(layer.RandomYawDegrees),
            layer.WindStrength);

         bgfx::setUniform(res.u_GrassLayer0, &layer0.x);
         bgfx::setUniform(res.u_GrassLayer1, &layer1.x);

         // Set layer params for compute shader (billboard vs mesh mode)
         const bool isBillboardType = (layer.RenderMode == GrassRenderMode::Billboard || layer.RenderMode == GrassRenderMode::BillboardFixed);
         const glm::vec4 layerParams(
            isBillboardType ? 0.0f : 1.0f,
            layer.WindStrength,
            layer.RenderMode == GrassRenderMode::BillboardFixed ? 1.0f : 0.0f,  // flag for fixed billboard
            isBillboardType ? 0.35f : 0.0f);
         bgfx::setUniform(res.u_LayerParams, &layerParams.x);

         // Calculate wind direction from angle (0 = +X, 90 = +Z)
         const float windAngleRad = glm::radians(layer.WindDirectionDegrees);
         const glm::vec4 windDir(std::cos(windAngleRad), std::sin(windAngleRad), 0.0f, 0.0f);
         bgfx::setUniform(res.u_WindDir, &windDir.x);

         const glm::vec4 maskParams(
            static_cast<float>(layer.Mask),
            layer.MinDistance,
            layer.MaxDistance,
            bgfx::isValid(chunk.SplatTexture) ? 1.0f : 0.0f);
         bgfx::setUniform(res.u_GrassMaskParams, &maskParams.x);

         const glm::vec4 camPos(cameraPos, 0.0f);
         const glm::vec4 camRight(cameraRight, 0.0f);
         const glm::vec4 camForward(cameraForward, 0.0f);
         bgfx::setUniform(res.u_GrassCameraPos, &camPos.x);
         bgfx::setUniform(res.u_GrassCameraRight, &camRight.x);
         bgfx::setUniform(res.u_GrassCameraForward, &camForward.x);

         bgfx::setUniform(res.u_GrassWorld, glm::value_ptr(worldTransform), 4);

         std::array<float, 12> normalData{};
         const float* normalPtr = glm::value_ptr(normalMatrix);
         for (int row = 0; row < 3; ++row)
            {
            normalData[row * 4 + 0] = normalPtr[row * 3 + 0];
            normalData[row * 4 + 1] = normalPtr[row * 3 + 1];
            normalData[row * 4 + 2] = normalPtr[row * 3 + 2];
            normalData[row * 4 + 3] = 0.0f;
            }
         bgfx::setUniform(res.u_GrassNormal, normalData.data(), 3);

         const uint16_t groupsX = static_cast<uint16_t>((grassGridResolution + kGrassComputeGroupSize - 1) / kGrassComputeGroupSize);
         const uint16_t groupsY = static_cast<uint16_t>((grassGridResolution + kGrassComputeGroupSize - 1) / kGrassComputeGroupSize);
         // dispatch.w = terrain's actual grid resolution (for texture sampling when grass grid is multiplied)
         const glm::vec4 dispatch(groupsX, groupsY, static_cast<float>(grassGridResolution), static_cast<float>(terrain.GridResolution));
         bgfx::setUniform(res.u_GrassDispatch, &dispatch.x);

         const float timeSec = GetTimeSeconds();
         const uint32_t seed = layer.Guid.low ^ layer.Guid.high ^ grassGridResolution;
         const glm::vec4 seedTime(static_cast<float>(seed & 0x00FFFFFFu), timeSec, layer.ColorVariance.x, layer.ColorVariance.y);
         bgfx::setUniform(res.u_GrassSeedTime, &seedTime.x);

         bgfx::dispatch(viewId, res.ComputeGenerateProgram, groupsX, groupsY, 1);
         }

      void SetCommonLayerUniforms(const TerrainGrassLayerDesc& layer, const TerrainComponent& terrain, 
                                    const glm::mat4& worldTransform, GrassRendererResources& res)
         {
          const bool isBillboardType = (layer.RenderMode == GrassRenderMode::Billboard || layer.RenderMode == GrassRenderMode::BillboardFixed);
          const bool deformationEnabled = terrain.DeformationConfig.Enabled && bgfx::isValid(terrain.DeformationTexture);
         const float alphaCutout = isBillboardType ? 0.35f : 0.0f;
          
          const glm::vec4 layerParams(
             isBillboardType ? 0.0f : 1.0f,
             layer.WindStrength,
             layer.RenderMode == GrassRenderMode::BillboardFixed ? 1.0f : 0.0f,
             alphaCutout);

         // Calculate wind direction from angle (0 = +X, 90 = +Z)
         const float windAngleRad = glm::radians(layer.WindDirectionDegrees);
         const glm::vec4 windDir(std::cos(windAngleRad), std::sin(windAngleRad), 0.0f, 0.0f);

         const float invRange = (layer.MaxDistance - layer.MinDistance) > 0.0f
            ? 1.0f / (layer.MaxDistance - layer.MinDistance)
            : 0.0f;
         const glm::vec4 fadeParams(layer.MinDistance, layer.MaxDistance, invRange, 0.0f);
         const glm::vec4 baseColor(layer.BaseColor, 1.0f);
         const glm::vec4 colorVariance(layer.ColorVariance, 0.0f);
         
         // Terrain info for deformation UV calculation (world size, world origin)
         const glm::vec3 terrainOrigin = glm::vec3(worldTransform[3]);
         const glm::vec4 terrainInfo(terrain.WorldSize.x, terrain.WorldSize.y, terrainOrigin.x, terrainOrigin.z);
         
         // Deformation parameters
         const glm::vec4 deformParams(
            deformationEnabled ? terrain.DeformationConfig.MaxDeformation : 0.0f,
            0.0f,
            0.0f,
            0.0f);

         bgfx::setUniform(res.u_LayerParams, &layerParams.x);
         bgfx::setUniform(res.u_WindDir, &windDir.x);
         bgfx::setUniform(res.u_FadeParams, &fadeParams.x);
         bgfx::setUniform(res.u_BaseColor, &baseColor.x);
         bgfx::setUniform(res.u_ColorVariance, &colorVariance.x);
         bgfx::setUniform(res.u_GrassTerrainInfo, &terrainInfo.x);
         bgfx::setUniform(res.u_GrassDeformParams, &deformParams.x);
         
         // Bind deformation texture
         if (deformationEnabled)
            {
            bgfx::setTexture(4, res.s_DeformationTex, terrain.DeformationTexture);
            }
         }

      std::vector<GrassInstanceData>& GetCpuInstanceScratch()
         {
         static thread_local std::vector<GrassInstanceData> s_Scratch;
         return s_Scratch;
         }

      bool ShouldLogCpuClampWarning(const ClaymoreGUID& guid)
         {
         static std::unordered_set<uint64_t> s_WarnedLayers;
         const uint64_t key = guid.high ^ (guid.low << 1);
         const auto result = s_WarnedLayers.insert(key);
         return result.second;
         }

      struct GrassPatchContext
         {
         glm::mat4 WorldTransform{};
         glm::mat3 NormalMatrix{};
         glm::vec3 CameraPos{};
         glm::vec3 CameraRight{};
         glm::vec3 CameraForward{};
         glm::vec2 CellSize{};
         glm::vec2 PatchWorldSize{};
         uint32_t GridResolution = 0;
         uint32_t PatchSizeCells = 0;
         uint32_t PatchCountX = 0;
         uint32_t PatchCountZ = 0;
         float PatchRadius = 0.0f;
         float SlopeLimit = 0.0f;
         float YawLimit = 0.0f;
         float CellArea = 0.0f;
         bool ProceduralSplat = false;
         bool SplatAvailable = false;
         bool DistanceGateActive = false;
         float MinDistance = 0.0f;
         float MaxDistance = 0.0f;
         uint32_t LayerSeed = 0;
         uint32_t NoiseSeed = 0;
         };

      bool BuildGrassPatchContext(const TerrainComponent& terrain,
         const TerrainGrassLayerDesc& layer,
         const glm::mat4& worldTransform,
         const glm::mat3& normalMatrix,
         const glm::vec3& cameraPos,
         const glm::vec3& cameraRight,
         const glm::vec3& cameraForward,
         GrassPatchContext& outCtx)
         {
         outCtx = GrassPatchContext{};
         outCtx.WorldTransform = worldTransform;
         outCtx.NormalMatrix = normalMatrix;
         outCtx.CameraPos = cameraPos;
         outCtx.CameraRight = cameraRight;
         outCtx.CameraForward = cameraForward;

         const bool usesSplatMask = UsesSplatMask(layer.Mask);
         outCtx.ProceduralSplat = usesSplatMask;
         outCtx.SplatAvailable = usesSplatMask && !terrain.SplatMap.empty();

         if (terrain.GridResolution == 0)
            return false;
         if (!outCtx.ProceduralSplat && layer.PaintedMask.empty())
            return false;
         if (outCtx.ProceduralSplat && !outCtx.SplatAvailable)
            return false;

         const glm::vec2 terrainCellSize = Terrain::GetCellSize(terrain);
         if (terrainCellSize.x <= 0.0f || terrainCellSize.y <= 0.0f)
            return false;

         const uint32_t samplingMult = std::clamp(terrain.GrassSamplingMultiplier, 1u, 16u);
         outCtx.GridResolution = terrain.GridResolution * samplingMult;
         outCtx.CellSize = terrainCellSize / static_cast<float>(samplingMult);

         uint32_t patchSizeCells = std::max(8u, terrain.GrassChunkResolution * samplingMult);
         patchSizeCells = std::min(std::max(1u, patchSizeCells), std::max(1u, outCtx.GridResolution));
         outCtx.PatchSizeCells = patchSizeCells;
         outCtx.PatchCountX = (outCtx.GridResolution + patchSizeCells - 1u) / patchSizeCells;
         outCtx.PatchCountZ = outCtx.PatchCountX;
         outCtx.PatchWorldSize = outCtx.CellSize * static_cast<float>(patchSizeCells);
         outCtx.PatchRadius = 0.5f * glm::length(outCtx.PatchWorldSize);
         outCtx.SlopeLimit = glm::radians(layer.MaxSlopeDegrees);
         outCtx.YawLimit = glm::radians(layer.RandomYawDegrees);
         outCtx.DistanceGateActive = (layer.MaxDistance > 0.0f) && (layer.MaxDistance > (layer.MinDistance + 0.001f));
         outCtx.MinDistance = layer.MinDistance;
         outCtx.MaxDistance = layer.MaxDistance;
         outCtx.CellArea = outCtx.CellSize.x * outCtx.CellSize.y;

         uint32_t guidSeed =
            static_cast<uint32_t>(layer.Guid.low) ^
            static_cast<uint32_t>(layer.Guid.low >> 32) ^
            static_cast<uint32_t>(layer.Guid.high) ^
            static_cast<uint32_t>(layer.Guid.high >> 32);
         outCtx.LayerSeed = outCtx.ProceduralSplat ? layer.SplatSeed : guidSeed;
         outCtx.NoiseSeed = Hash32(outCtx.LayerSeed ^ 0x68bc21ebu);

         return true;
         }

      bool GenerateCpuInstancesForPatch(const TerrainComponent& terrain,
         const TerrainGrassLayerDesc& layer,
         const GrassPatchContext& ctx,
         uint32_t patchX,
         uint32_t patchZ,
         std::vector<GrassInstanceData>& outInstances,
         size_t maxInstances)
         {
         if (ctx.GridResolution == 0 || ctx.PatchSizeCells == 0)
            return false;

         const glm::ivec2 startCell(
            static_cast<int>(patchX * ctx.PatchSizeCells),
            static_cast<int>(patchZ * ctx.PatchSizeCells));
         const glm::ivec2 endCell(
            static_cast<int>(std::min(ctx.GridResolution, (patchX + 1) * ctx.PatchSizeCells)),
            static_cast<int>(std::min(ctx.GridResolution, (patchZ + 1) * ctx.PatchSizeCells)));

         if (startCell.x >= endCell.x || startCell.y >= endCell.y)
            return false;

         const float patchMinX = static_cast<float>(startCell.x) * ctx.CellSize.x;
         const float patchMinZ = static_cast<float>(startCell.y) * ctx.CellSize.y;
         const float patchMaxX = static_cast<float>(endCell.x) * ctx.CellSize.x;
         const float patchMaxZ = static_cast<float>(endCell.y) * ctx.CellSize.y;
         const float centerLocalX = (patchMinX + patchMaxX) * 0.5f;
         const float centerLocalZ = (patchMinZ + patchMaxZ) * 0.5f;
         const float centerHeight = Terrain::SampleHeightWorld(terrain, centerLocalX, centerLocalZ);
         const glm::vec3 patchCenterWorld = glm::vec3(ctx.WorldTransform * glm::vec4(centerLocalX, centerHeight, centerLocalZ, 1.0f));

         if (ctx.DistanceGateActive)
            {
            const float patchDistance = glm::distance(ctx.CameraPos, patchCenterWorld);
            if (patchDistance - ctx.PatchRadius > ctx.MaxDistance)
               return false;
            if (patchDistance + ctx.PatchRadius < ctx.MinDistance)
               return false;
            }

         uint32_t patchSeed = ctx.LayerSeed ^
            Hash32(static_cast<uint32_t>(patchX) * 73856093u) ^
            Hash32(static_cast<uint32_t>(patchZ) * 19349663u);

         const float uvDenom = static_cast<float>(ctx.GridResolution > 1 ? ctx.GridResolution - 1 : 1);

         for (int cellY = startCell.y; cellY < endCell.y; ++cellY)
            {
            for (int cellX = startCell.x; cellX < endCell.x; ++cellX)
               {
               const float u = static_cast<float>(cellX) / uvDenom;
               const float v = static_cast<float>(cellY) / uvDenom;

               const float centerLocalXCell = (static_cast<float>(cellX) + 0.5f) * ctx.CellSize.x;
               const float centerLocalZCell = (static_cast<float>(cellY) + 0.5f) * ctx.CellSize.y;
               if (Terrain::IsHoleAtLocal(terrain, centerLocalXCell, centerLocalZCell))
                  continue;

               float densityWeight = 1.0f;
               if (ctx.ProceduralSplat)
                  {
                  const glm::vec4 splatValue = SampleSplatValueBilinear(terrain, u, v);
                  const float maskFactor = MaskSampleValueNormalized(layer.Mask, splatValue, ctx.SplatAvailable, 1.0f);
                  // Use layer-specific threshold for splatmap channels to allow clean separation from other textures
                  const float splatThreshold = layer.SplatThreshold;
                  if (maskFactor <= splatThreshold)
                     continue;

                  const float noiseWeight = SampleSplatNoise(glm::vec2(centerLocalXCell, centerLocalZCell),
                     layer.SplatNoiseScale, layer.SplatNoiseStrength, ctx.NoiseSeed);
                  densityWeight = glm::clamp(maskFactor * noiseWeight, 0.0f, 1.0f);
                  // After noise multiplication, use a lower threshold to avoid over-filtering
                  if (densityWeight <= kCpuDensityThreshold)
                     continue;
                  }
               else
                  {
                  const float densitySample = SamplePaintDensityBilinear(terrain, layer, u, v);
                  if (densitySample <= kCpuDensityThreshold)
                     continue;
                  densityWeight = glm::clamp(densitySample, 0.0f, 1.0f);
                  if (densityWeight <= kCpuDensityThreshold)
                     continue;
                  }

               const float baseHeight = Terrain::SampleHeightWorld(terrain, centerLocalXCell, centerLocalZCell);
               if (baseHeight < layer.HeightRange.x || baseHeight > layer.HeightRange.y)
                  continue;

               glm::vec3 normalLocal = Terrain::SampleNormal(terrain, centerLocalXCell, centerLocalZCell);
               if (!std::isfinite(normalLocal.x) || !std::isfinite(normalLocal.y) || !std::isfinite(normalLocal.z))
                  normalLocal = glm::vec3(0.0f, 1.0f, 0.0f);
               const float slope = std::acos(glm::clamp(normalLocal.y, 0.0f, 1.0f));
               if (slope > ctx.SlopeLimit)
                  continue;

               glm::vec3 sideLocal = glm::normalize(glm::cross(normalLocal, glm::vec3(0.0f, 0.0f, 1.0f)));
               if (!std::isfinite(sideLocal.x) || !std::isfinite(sideLocal.y) || !std::isfinite(sideLocal.z) || glm::length2(sideLocal) < 1e-4f)
                  {
                  sideLocal = glm::normalize(glm::cross(normalLocal, glm::vec3(1.0f, 0.0f, 0.0f)));
                  }
               glm::vec3 forwardLocal = glm::normalize(glm::cross(sideLocal, normalLocal));

               const float scaledDensity = layer.DensityPerSquareMeter * 0.1f;
               const float bladesExact = densityWeight * scaledDensity * ctx.CellArea;

               uint32_t cellSeed = patchSeed ^
                  Hash32(static_cast<uint32_t>(cellX) * 83492791u) ^
                  Hash32(static_cast<uint32_t>(cellY) * 181783497u);

               int bladeCount = static_cast<int>(bladesExact);
               const float fractional = bladesExact - static_cast<float>(bladeCount);
               if (RandomFloat01(cellSeed) < fractional)
                  ++bladeCount;

               bladeCount = std::clamp(bladeCount, 0, static_cast<int>(kCpuMaxBladesPerTexel));
               if (bladeCount <= 0)
                  continue;

               for (int blade = 0; blade < bladeCount; ++blade)
                  {
                  const float offsetX = RandomFloat01(cellSeed);
                  const float offsetZ = RandomFloat01(cellSeed);
                  const float localX = (static_cast<float>(cellX) + offsetX) * ctx.CellSize.x;
                  const float localZ = (static_cast<float>(cellY) + offsetZ) * ctx.CellSize.y;
                  if (Terrain::IsHoleAtLocal(terrain, localX, localZ))
                     continue;

                  const float bladeHeight = Terrain::SampleHeightWorld(terrain, localX, localZ);

                  const float scale = glm::mix(layer.ScaleRange.x, layer.ScaleRange.y, RandomFloat01(cellSeed));
                  float yawAngle = 0.0f;
                  if (ctx.YawLimit > 0.0f)
                     {
                     yawAngle = (RandomFloat01(cellSeed) * 2.0f - 1.0f) * ctx.YawLimit;
                     }

                  const float randColor = RandomFloat01(cellSeed);
                  const float windPhase = RandomFloat01(cellSeed);

                  const float sinYaw = std::sin(yawAngle);
                  const float cosYaw = std::cos(yawAngle);
                  glm::vec3 rotatedSide = glm::normalize(sideLocal * cosYaw + forwardLocal * sinYaw);
                  glm::vec3 rotatedForward = glm::normalize(glm::cross(rotatedSide, normalLocal));

                  glm::vec3 upWorldRaw = ctx.NormalMatrix * normalLocal;
                  glm::vec3 sideWorldRaw = ctx.NormalMatrix * rotatedSide;
                  glm::vec3 forwardWorldRaw = ctx.NormalMatrix * rotatedForward;

                  if (glm::length2(upWorldRaw) < 1e-8f)
                     upWorldRaw = glm::vec3(0.0f, 1.0f, 0.0f);
                  if (glm::length2(sideWorldRaw) < 1e-8f)
                     sideWorldRaw = glm::vec3(1.0f, 0.0f, 0.0f);
                  if (glm::length2(forwardWorldRaw) < 1e-8f)
                     forwardWorldRaw = glm::vec3(0.0f, 0.0f, 1.0f);

                  glm::vec3 upWorld = glm::normalize(upWorldRaw) * scale;
                  glm::vec3 sideWorld = glm::normalize(sideWorldRaw) * scale;
                  glm::vec3 forwardWorld = glm::normalize(forwardWorldRaw) * scale;

                  const glm::vec3 worldPos = glm::vec3(ctx.WorldTransform * glm::vec4(localX, bladeHeight, localZ, 1.0f));

                  if (layer.RenderMode == GrassRenderMode::Billboard)
                     {
                     glm::vec3 toCameraFlat = ctx.CameraPos - worldPos;
                     toCameraFlat.y = 0.0f;
                     if (glm::length2(toCameraFlat) < 1e-8f)
                        toCameraFlat = glm::vec3(0.0f, 0.0f, 1.0f);
                     toCameraFlat = glm::normalize(toCameraFlat);

                     glm::vec3 billboardSideRaw = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), toCameraFlat);
                     if (glm::length2(billboardSideRaw) < 1e-8f)
                        billboardSideRaw = glm::vec3(1.0f, 0.0f, 0.0f);
                     sideWorld = glm::normalize(billboardSideRaw) * scale;
                     forwardWorld = toCameraFlat * scale;
                     }

                  const bool validSide = std::isfinite(sideWorld.x) && std::isfinite(sideWorld.y) && std::isfinite(sideWorld.z);
                  const bool validUp = std::isfinite(upWorld.x) && std::isfinite(upWorld.y) && std::isfinite(upWorld.z);
                  const bool validPos = std::isfinite(worldPos.x) && std::isfinite(worldPos.y) && std::isfinite(worldPos.z);
                  if (!validSide || !validUp || !validPos)
                     continue;
                  if (ctx.DistanceGateActive)
                     {
                     const float dist = glm::distance(worldPos, ctx.CameraPos);
                     if (dist < ctx.MinDistance || dist > ctx.MaxDistance)
                        continue;
                     }

                  GrassInstanceData instance{};
                  instance.Data[0] = glm::vec4(sideWorld, randColor);
                  instance.Data[1] = glm::vec4(upWorld, glm::length(upWorld));
                  instance.Data[2] = glm::vec4(forwardWorld, 0.0f);
                  instance.Data[3] = glm::vec4(worldPos, windPhase);

                  outInstances.push_back(instance);

                  if (outInstances.size() >= maxInstances)
                     {
                     return true;
                     }
                  }
               }
            }

         return false;
         }

      bool GenerateCpuInstancesForLayer(const TerrainComponent& terrain,
         const TerrainGrassLayerDesc& layer,
         const glm::mat4& worldTransform,
         const glm::mat3& normalMatrix,
         const glm::vec3& cameraPos,
         const glm::vec3& cameraLocalPos,
         const glm::vec3& cameraRight,
         const glm::vec3& cameraForward,
         const terrain::ChunkFrustum* frustum,
         float frustumPadding,
         std::vector<GrassInstanceData>& outInstances)
         {
         outInstances.clear();

         GrassPatchContext ctx{};
         if (!BuildGrassPatchContext(terrain, layer, worldTransform, normalMatrix, cameraPos, cameraRight, cameraForward, ctx))
            return false;

         const bool useFrustum = frustum != nullptr;

         uint32_t patchStartX = 0;
         uint32_t patchEndX = ctx.PatchCountX > 0 ? ctx.PatchCountX - 1u : 0u;
         uint32_t patchStartZ = 0;
         uint32_t patchEndZ = ctx.PatchCountZ > 0 ? ctx.PatchCountZ - 1u : 0u;
         if (ctx.MaxDistance > 0.0f && ctx.PatchWorldSize.x > 0.0f && ctx.PatchWorldSize.y > 0.0f)
            {
            const float maxRadius = ctx.MaxDistance + ctx.PatchRadius;
            const float minLocalX = cameraLocalPos.x - maxRadius;
            const float maxLocalX = cameraLocalPos.x + maxRadius;
            const float minLocalZ = cameraLocalPos.z - maxRadius;
            const float maxLocalZ = cameraLocalPos.z + maxRadius;

            const int minPatchX = static_cast<int>(std::floor(minLocalX / ctx.PatchWorldSize.x));
            const int maxPatchX = static_cast<int>(std::floor(maxLocalX / ctx.PatchWorldSize.x));
            const int minPatchZ = static_cast<int>(std::floor(minLocalZ / ctx.PatchWorldSize.y));
            const int maxPatchZ = static_cast<int>(std::floor(maxLocalZ / ctx.PatchWorldSize.y));

            patchStartX = static_cast<uint32_t>(std::clamp(minPatchX, 0, static_cast<int>(ctx.PatchCountX) - 1));
            patchEndX = static_cast<uint32_t>(std::clamp(maxPatchX, 0, static_cast<int>(ctx.PatchCountX) - 1));
            patchStartZ = static_cast<uint32_t>(std::clamp(minPatchZ, 0, static_cast<int>(ctx.PatchCountZ) - 1));
            patchEndZ = static_cast<uint32_t>(std::clamp(maxPatchZ, 0, static_cast<int>(ctx.PatchCountZ) - 1));
            }

         float heightSpan = layer.HeightRange.y - layer.HeightRange.x;
         if (!std::isfinite(heightSpan) || heightSpan <= 0.0f)
            heightSpan = terrain.MaxHeight;
         heightSpan = glm::clamp(heightSpan, 0.0f, terrain.MaxHeight);
         const float halfHeight = 0.5f * heightSpan;
         const float baseFrustumRadius = glm::length(glm::vec3(ctx.PatchWorldSize.x * 0.5f, halfHeight, ctx.PatchWorldSize.y * 0.5f));

         if (patchStartX > patchEndX || patchStartZ > patchEndZ)
            return false;

         bool limitReached = false;
         for (uint32_t patchZ = patchStartZ; patchZ <= patchEndZ; ++patchZ)
            {
            for (uint32_t patchX = patchStartX; patchX <= patchEndX; ++patchX)
               {
               if (useFrustum)
                  {
                  const float patchMinX = static_cast<float>(patchX * ctx.PatchSizeCells) * ctx.CellSize.x;
                  const float patchMinZ = static_cast<float>(patchZ * ctx.PatchSizeCells) * ctx.CellSize.y;
                  const float patchMaxX = static_cast<float>(std::min(ctx.GridResolution, (patchX + 1) * ctx.PatchSizeCells)) * ctx.CellSize.x;
                  const float patchMaxZ = static_cast<float>(std::min(ctx.GridResolution, (patchZ + 1) * ctx.PatchSizeCells)) * ctx.CellSize.y;
                  const float centerLocalX = (patchMinX + patchMaxX) * 0.5f;
                  const float centerLocalZ = (patchMinZ + patchMaxZ) * 0.5f;
                  const float centerHeight = Terrain::SampleHeightWorld(terrain, centerLocalX, centerLocalZ);
                  const glm::vec3 patchCenterWorld = glm::vec3(worldTransform * glm::vec4(centerLocalX, centerHeight, centerLocalZ, 1.0f));
                  const float sphereRadius = baseFrustumRadius + frustumPadding;
                  if (!SphereInFrustum(*frustum, patchCenterWorld, sphereRadius))
                     continue;
                  }

               if (GenerateCpuInstancesForPatch(terrain, layer, ctx, patchX, patchZ, outInstances, kCpuMaxInstancesPerLayer))
                  {
                  limitReached = true;
                  return limitReached;
                  }
               }
            }

         return limitReached;
         }

      void RenderCpuPipeline(const TerrainComponent& terrain,
         const glm::mat4& worldTransform,
         uint16_t viewId,
         const glm::vec3& cameraPos,
         const glm::mat4& viewMatrix,
         const glm::mat4& projMatrix,
         bool enableFrustumCulling,
         GrassRendererResources& res)
         {
         if (terrain.GrassLayers.empty())
            return;

         const glm::mat4 invView = glm::inverse(viewMatrix);
         // GLM stores matrices in column-major order; columns 0 and 2 encode the camera right/forward axes.
         glm::vec3 cameraRight(invView[0][0], invView[0][1], invView[0][2]);
         glm::vec3 cameraForward = -glm::vec3(invView[2][0], invView[2][1], invView[2][2]);
         if (glm::length2(cameraRight) > 1e-6f) cameraRight = glm::normalize(cameraRight);
         else cameraRight = glm::vec3(1.0f, 0.0f, 0.0f);
         if (glm::length2(cameraForward) > 1e-6f) cameraForward = glm::normalize(cameraForward);
         else cameraForward = glm::vec3(0.0f, 0.0f, -1.0f);

        const glm::vec2 terrainCellSize = Terrain::GetCellSize(terrain);
        const uint32_t samplingMult = std::clamp(terrain.GrassSamplingMultiplier, 1u, 16u);
        const glm::vec2 grassCellSize = terrainCellSize / static_cast<float>(samplingMult);
        const uint32_t patchSizeCells = std::max(8u, terrain.GrassChunkResolution * samplingMult);
        const glm::vec2 patchWorldSize = grassCellSize * static_cast<float>(patchSizeCells);
        const float patchStep = glm::max(patchWorldSize.x, patchWorldSize.y);
        const float cameraUpdateThreshold = glm::max(0.5f, patchStep * 0.25f);
        const float cpuRebuildCooldown = 0.1f;
        const float frustumPadding = glm::max(0.5f, glm::min(grassCellSize.x, grassCellSize.y) * 2.0f);

         const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(worldTransform)));
        const glm::mat4 invWorld = glm::inverse(worldTransform);
        const glm::vec3 cameraLocalPos = glm::vec3(invWorld * glm::vec4(cameraPos, 1.0f));
      glm::vec3 cameraForwardLocal = glm::vec3(invWorld * glm::vec4(cameraForward, 0.0f));
      if (glm::length2(cameraForwardLocal) > 1e-6f)
         cameraForwardLocal = glm::normalize(cameraForwardLocal);
      else
         cameraForwardLocal = glm::vec3(0.0f, 0.0f, -1.0f);
         const float timeSec = GetTimeSeconds();
         const glm::vec4 timeUniform(timeSec, 0.0f, 0.0f, 0.0f);
         bgfx::setUniform(res.u_Time, &timeUniform.x);

        terrain::ChunkFrustum frustum{};
        const terrain::ChunkFrustum* frustumPtr = nullptr;
        if (enableFrustumCulling)
           {
           frustum = terrain::ChunkFrustum::FromViewProj(projMatrix * viewMatrix);
           frustumPtr = &frustum;
           }

#if CM_DEBUG_GRASS
         GrassDebugState& debugState = GetDebugState();
         if (debugState.Enabled)
            {
            debugState.TelemetrySupported = false;
            debugState.Samples.clear();
            }
#endif

         std::vector<GrassInstanceData>& scratch = GetCpuInstanceScratch();
      static uint32_t s_FrameCounter = 0;
      ++s_FrameCounter;
      const bool hasJobSystem = (cm::g_JobSystem != nullptr);

         for (const TerrainGrassLayerDesc& layerConst : terrain.GrassLayers)
            {
            // Skip disabled layers, layers without masks, and layers using GPU mode
            const bool usesSplatMask = UsesSplatMask(layerConst.Mask);
            if (!layerConst.Enabled || layerConst.UseGPU)
               continue;
            if (!usesSplatMask && layerConst.PaintedMask.empty())
               continue;

            SetCommonLayerUniforms(layerConst, terrain, worldTransform, res);
            
            // Ensure runtime exists for caching
            TerrainGrassLayerDesc& layer = const_cast<TerrainGrassLayerDesc&>(layerConst);
            if (!layer.Runtime)
               layer.Runtime = std::make_unique<GrassLayerRuntime>();
            GrassLayerRuntime* runtime = layer.Runtime.get();
            if (!runtime)
               continue;

            if (layer.RuntimeDirty || layer.DensityDirty)
               {
               runtime->PatchCache.clear();
               runtime->PatchInFlight.clear();
               {
                  std::lock_guard<std::mutex> lock(runtime->PatchResultMutex);
                  runtime->PatchResults.clear();
               }
               runtime->CpuInstanceCache.clear();
               runtime->CpuCacheValid = false;
               layer.DensityDirty = false;
               layer.RuntimeDirty = false;
               }

            std::vector<GrassPatchJobResult> completedResults;
            {
               std::lock_guard<std::mutex> lock(runtime->PatchResultMutex);
               if (!runtime->PatchResults.empty())
                  completedResults.swap(runtime->PatchResults);
            }
            for (GrassPatchJobResult& result : completedResults)
               {
               runtime->PatchInFlight.erase(result.Key);
               GrassPatchCacheEntry& entry = runtime->PatchCache[result.Key];
               entry.Instances = std::move(result.Instances);
               entry.Ready = true;
               entry.LastUsedFrame = s_FrameCounter;
               }

            GrassPatchContext ctx{};
            if (!BuildGrassPatchContext(terrain, layerConst, worldTransform, normalMatrix, cameraPos, cameraRight, cameraForward, ctx))
               continue;

            uint32_t patchStartX = 0;
            uint32_t patchEndX = ctx.PatchCountX > 0 ? ctx.PatchCountX - 1u : 0u;
            uint32_t patchStartZ = 0;
            uint32_t patchEndZ = ctx.PatchCountZ > 0 ? ctx.PatchCountZ - 1u : 0u;
            if (ctx.MaxDistance > 0.0f && ctx.PatchWorldSize.x > 0.0f && ctx.PatchWorldSize.y > 0.0f)
               {
               const float maxRadius = ctx.MaxDistance + ctx.PatchRadius;
               const float minLocalX = cameraLocalPos.x - maxRadius;
               const float maxLocalX = cameraLocalPos.x + maxRadius;
               const float minLocalZ = cameraLocalPos.z - maxRadius;
               const float maxLocalZ = cameraLocalPos.z + maxRadius;

               const int minPatchX = static_cast<int>(std::floor(minLocalX / ctx.PatchWorldSize.x));
               const int maxPatchX = static_cast<int>(std::floor(maxLocalX / ctx.PatchWorldSize.x));
               const int minPatchZ = static_cast<int>(std::floor(minLocalZ / ctx.PatchWorldSize.y));
               const int maxPatchZ = static_cast<int>(std::floor(maxLocalZ / ctx.PatchWorldSize.y));

               patchStartX = static_cast<uint32_t>(std::clamp(minPatchX, 0, static_cast<int>(ctx.PatchCountX) - 1));
               patchEndX = static_cast<uint32_t>(std::clamp(maxPatchX, 0, static_cast<int>(ctx.PatchCountX) - 1));
               patchStartZ = static_cast<uint32_t>(std::clamp(minPatchZ, 0, static_cast<int>(ctx.PatchCountZ) - 1));
               patchEndZ = static_cast<uint32_t>(std::clamp(maxPatchZ, 0, static_cast<int>(ctx.PatchCountZ) - 1));
               }

            const uint32_t prefetchPatches = 1;
            if (prefetchPatches > 0 && ctx.PatchCountX > 0)
               {
               if (cameraForwardLocal.x > 0.2f)
                  patchEndX = std::min(patchEndX + prefetchPatches, ctx.PatchCountX - 1u);
               else if (cameraForwardLocal.x < -0.2f)
                  patchStartX = static_cast<uint32_t>(std::max(0, static_cast<int>(patchStartX) - static_cast<int>(prefetchPatches)));
               if (cameraForwardLocal.z > 0.2f)
                  patchEndZ = std::min(patchEndZ + prefetchPatches, ctx.PatchCountZ - 1u);
               else if (cameraForwardLocal.z < -0.2f)
                  patchStartZ = static_cast<uint32_t>(std::max(0, static_cast<int>(patchStartZ) - static_cast<int>(prefetchPatches)));
               }

            if (patchStartX > patchEndX || patchStartZ > patchEndZ)
               continue;

            bool forceRefresh = false;
            const bool trackCameraRefresh = (layerConst.RenderMode == GrassRenderMode::Billboard) || ctx.DistanceGateActive;
            if (trackCameraRefresh)
               {
               if (!runtime->CpuCacheValid)
                  {
                  runtime->CpuCacheValid = true;
                  runtime->CachedCameraPos = cameraPos;
                  runtime->LastCpuUpdateTime = timeSec;
                  }
               const glm::vec2 delta(cameraPos.x - runtime->CachedCameraPos.x, cameraPos.z - runtime->CachedCameraPos.z);
               if (glm::length2(delta) > cameraUpdateThreshold * cameraUpdateThreshold &&
                   (timeSec - runtime->LastCpuUpdateTime) >= cpuRebuildCooldown)
                  {
                  runtime->CachedCameraPos = cameraPos;
                  runtime->LastCpuUpdateTime = timeSec;
                  forceRefresh = true;
                  }
               }

            float heightSpan = layer.HeightRange.y - layer.HeightRange.x;
            if (!std::isfinite(heightSpan) || heightSpan <= 0.0f)
               heightSpan = terrain.MaxHeight;
            heightSpan = glm::clamp(heightSpan, 0.0f, terrain.MaxHeight);
            const float halfHeight = 0.5f * heightSpan;
            const float baseFrustumRadius = glm::length(glm::vec3(ctx.PatchWorldSize.x * 0.5f, halfHeight, ctx.PatchWorldSize.y * 0.5f));

            scratch.clear();
            scratch.reserve(std::min(static_cast<size_t>(8192), static_cast<size_t>(kCpuMaxInstancesPerLayer)));

            const uint32_t syncPatchLimit = 1;
            uint32_t syncPatchBudget = syncPatchLimit;

            struct PatchRequest
               {
               GrassPatchKey Key{};
               float DistanceSq = 0.0f;
               };
            std::vector<PatchRequest> requests;
            requests.reserve((patchEndX - patchStartX + 1u) * (patchEndZ - patchStartZ + 1u));

            bool reachedLimit = false;
            for (uint32_t patchZ = patchStartZ; patchZ <= patchEndZ && !reachedLimit; ++patchZ)
               {
               for (uint32_t patchX = patchStartX; patchX <= patchEndX && !reachedLimit; ++patchX)
                  {
                  const float patchMinX = static_cast<float>(patchX * ctx.PatchSizeCells) * ctx.CellSize.x;
                  const float patchMinZ = static_cast<float>(patchZ * ctx.PatchSizeCells) * ctx.CellSize.y;
                  const float patchMaxX = static_cast<float>(std::min(ctx.GridResolution, (patchX + 1) * ctx.PatchSizeCells)) * ctx.CellSize.x;
                  const float patchMaxZ = static_cast<float>(std::min(ctx.GridResolution, (patchZ + 1) * ctx.PatchSizeCells)) * ctx.CellSize.y;
                  const float centerLocalX = (patchMinX + patchMaxX) * 0.5f;
                  const float centerLocalZ = (patchMinZ + patchMaxZ) * 0.5f;
                  const float centerHeight = Terrain::SampleHeightWorld(terrain, centerLocalX, centerLocalZ);
                  const glm::vec3 patchCenterWorld = glm::vec3(worldTransform * glm::vec4(centerLocalX, centerHeight, centerLocalZ, 1.0f));

                  const float patchDistance = glm::distance(cameraPos, patchCenterWorld);
                  if (ctx.DistanceGateActive)
                     {
                     if (patchDistance - ctx.PatchRadius > ctx.MaxDistance)
                        continue;
                     if (patchDistance + ctx.PatchRadius < ctx.MinDistance)
                        continue;
                     }

                  bool renderPatch = true;
                  if (frustumPtr)
                     {
                     const float sphereRadius = baseFrustumRadius + frustumPadding;
                     renderPatch = SphereInFrustum(*frustumPtr, patchCenterWorld, sphereRadius);
                     }

                  const GrassPatchKey key{ patchX, patchZ };
                  auto cacheIt = runtime->PatchCache.find(key);
                  if (renderPatch && (cacheIt == runtime->PatchCache.end() || !cacheIt->second.Ready) && syncPatchBudget > 0)
                     {
                     GrassPatchCacheEntry& entry = runtime->PatchCache[key];
                     entry.Instances.clear();
                     GenerateCpuInstancesForPatch(terrain, layerConst, ctx, patchX, patchZ, entry.Instances, 4096);
                     entry.Ready = true;
                     entry.LastUsedFrame = s_FrameCounter;
                     cacheIt = runtime->PatchCache.find(key);
                     if (cacheIt != runtime->PatchCache.end() && !cacheIt->second.Instances.empty())
                        {
                        const size_t remaining = kCpuMaxInstancesPerLayer - scratch.size();
                        const size_t copyCount = std::min(remaining, cacheIt->second.Instances.size());
                        if (copyCount > 0)
                           {
                           scratch.insert(scratch.end(), cacheIt->second.Instances.begin(), cacheIt->second.Instances.begin() + copyCount);
                           }
                        }
                     syncPatchBudget = 0;
                     }
                  if (cacheIt != runtime->PatchCache.end())
                     {
                     cacheIt->second.LastUsedFrame = s_FrameCounter;
                     if (renderPatch && cacheIt->second.Ready)
                        {
                        const size_t remaining = kCpuMaxInstancesPerLayer - scratch.size();
                        if (remaining == 0)
                           {
                           reachedLimit = true;
                           continue;
                           }
                        const size_t copyCount = std::min(remaining, cacheIt->second.Instances.size());
                        scratch.insert(scratch.end(), cacheIt->second.Instances.begin(), cacheIt->second.Instances.begin() + copyCount);
                        if (scratch.size() >= kCpuMaxInstancesPerLayer)
                           reachedLimit = true;
                        }
                     }

                  const bool missing = (cacheIt == runtime->PatchCache.end()) || !cacheIt->second.Ready;
                  if ((missing || forceRefresh) && runtime->PatchInFlight.find(key) == runtime->PatchInFlight.end())
                     {
                     requests.push_back({ key, patchDistance * patchDistance });
                     }
                  }
               }

            const uint32_t maxJobsPerFrame = usesSplatMask ? 4u : 1u;
            if (!requests.empty())
               {
               std::sort(requests.begin(), requests.end(),
                  [](const PatchRequest& a, const PatchRequest& b) { return a.DistanceSq < b.DistanceSq; });
               }

            uint32_t jobsSubmitted = 0;
            for (const PatchRequest& req : requests)
               {
               if (jobsSubmitted >= maxJobsPerFrame)
                  break;
               if (runtime->PatchInFlight.find(req.Key) != runtime->PatchInFlight.end())
                  continue;

               runtime->PatchInFlight.insert(req.Key);
               const uint32_t patchX = req.Key.X;
               const uint32_t patchZ = req.Key.Z;
               if (usesSplatMask && hasJobSystem)
                  {
                  const GrassPatchContext ctxCopy = ctx;
                  const TerrainComponent* terrainPtr = &terrain;
                  const TerrainGrassLayerDesc* layerPtr = &layerConst;
                  GrassLayerRuntime* runtimePtr = runtime;
                  const bool queued = cm::g_JobSystem->Enqueue([terrainPtr, layerPtr, runtimePtr, ctxCopy, req, patchX, patchZ]()
                     {
                     std::vector<GrassInstanceData> instances;
                     instances.reserve(256);
                     GenerateCpuInstancesForPatch(*terrainPtr, *layerPtr, ctxCopy, patchX, patchZ, instances, kCpuMaxInstancesPerLayer);
                     GrassPatchJobResult result{};
                     result.Key = req.Key;
                     result.Instances = std::move(instances);
                     std::lock_guard<std::mutex> lock(runtimePtr->PatchResultMutex);
                     runtimePtr->PatchResults.push_back(std::move(result));
                     }, JobSystem::Priority::Low);
                  if (!queued)
                     {
                     runtime->PatchInFlight.erase(req.Key);
                     }
                  }
               else
                  {
                  GrassPatchCacheEntry& entry = runtime->PatchCache[req.Key];
                  entry.Instances.clear();
                  GenerateCpuInstancesForPatch(terrain, layerConst, ctx, patchX, patchZ, entry.Instances, kCpuMaxInstancesPerLayer);
                  entry.Ready = true;
                  entry.LastUsedFrame = s_FrameCounter;
                  runtime->PatchInFlight.erase(req.Key);
                  }

               ++jobsSubmitted;
               }

            if (scratch.empty())
               {
               if (!runtime->CpuInstanceCache.empty())
                  {
                  scratch = runtime->CpuInstanceCache;
                  }
               else if (!requests.empty())
                  {
                  const uint32_t fallbackPatchLimit = 4u;
                  uint32_t fallbackCount = 0;
                  for (const PatchRequest& req : requests)
                     {
                     if (fallbackCount >= fallbackPatchLimit)
                        break;
                     if (runtime->PatchInFlight.find(req.Key) != runtime->PatchInFlight.end())
                        continue;

                     GrassPatchCacheEntry& entry = runtime->PatchCache[req.Key];
                     entry.Instances.clear();
                     GenerateCpuInstancesForPatch(terrain, layerConst, ctx, req.Key.X, req.Key.Z, entry.Instances, 4096);
                     entry.Ready = true;
                     entry.LastUsedFrame = s_FrameCounter;
                     if (!entry.Instances.empty())
                        {
                        scratch = entry.Instances;
                        break;
                        }
                     ++fallbackCount;
                     }
                  }
               }
            if (!scratch.empty())
               {
               runtime->CpuInstanceCache = scratch;
               }

            const uint32_t keepFrames = 30;
            const size_t desiredCount = static_cast<size_t>(patchEndX - patchStartX + 1u) * static_cast<size_t>(patchEndZ - patchStartZ + 1u);
            const size_t maxCacheEntries = desiredCount + 8;
            for (auto it = runtime->PatchCache.begin(); it != runtime->PatchCache.end();)
               {
               const uint32_t age = s_FrameCounter - it->second.LastUsedFrame;
               if (age > keepFrames && runtime->PatchCache.size() > maxCacheEntries)
                  it = runtime->PatchCache.erase(it);
               else
                  ++it;
               }
            while (runtime->PatchCache.size() > maxCacheEntries)
               {
               auto oldest = runtime->PatchCache.begin();
               for (auto it = runtime->PatchCache.begin(); it != runtime->PatchCache.end(); ++it)
                  {
                  if (it->second.LastUsedFrame < oldest->second.LastUsedFrame)
                     oldest = it;
                  }
               runtime->PatchCache.erase(oldest);
               }
            
            if (scratch.empty())
               continue;

            uint32_t instanceCount = static_cast<uint32_t>(scratch.size());
            const uint16_t stride = res.InstanceLayout.getStride();
            if (stride != sizeof(GrassInstanceData))
               {
               std::cerr << "[TerrainGrass] CPU grass instance stride mismatch. Expected "
                  << sizeof(GrassInstanceData) << " bytes but layout uses " << stride << " bytes." << std::endl;
               continue;
               }

            // Check available transient buffer space and clamp instance count
            const uint32_t availableInstances = bgfx::getAvailInstanceDataBuffer(instanceCount, stride);
            if (availableInstances == 0)
               {
               continue;
               }
            if (instanceCount > availableInstances)
               {
               instanceCount = availableInstances;
               }

            bgfx::InstanceDataBuffer idb{};
            bgfx::allocInstanceDataBuffer(&idb, instanceCount, stride);
            const size_t copyBytes = static_cast<size_t>(instanceCount) * sizeof(GrassInstanceData);
            std::memcpy(idb.data, scratch.data(), copyBytes);

            bgfx::TextureHandle albedo = bgfx::isValid(layerConst.BillboardTexture) ? layerConst.BillboardTexture : res.WhiteTexture;
            bgfx::setTexture(0, res.s_Albedo, albedo);
            bgfx::setInstanceDataBuffer(&idb);

            uint64_t state =
               BGFX_STATE_WRITE_RGB |
               BGFX_STATE_WRITE_A |
               BGFX_STATE_WRITE_Z |
               BGFX_STATE_DEPTH_TEST_LESS |
               BGFX_STATE_MSAA;

            bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
            if (layerConst.RenderMode == GrassRenderMode::Billboard || layerConst.RenderMode == GrassRenderMode::BillboardFixed)
               {
               program = res.BillboardProgram;
               state |= BGFX_STATE_BLEND_ALPHA;
               state &= ~BGFX_STATE_WRITE_Z;
               state &= ~BGFX_STATE_DEPTH_TEST_MASK;
               state |= BGFX_STATE_DEPTH_TEST_LEQUAL;
               state &= ~(BGFX_STATE_CULL_CW | BGFX_STATE_CULL_CCW);
               bgfx::setVertexBuffer(0, res.BillboardVB);
               bgfx::setIndexBuffer(res.BillboardIB);
               }
            else
               {
               if (!layerConst.Mesh)
                  {
                  std::cerr << "[TerrainGrass] Missing mesh asset for grass layer '" << layerConst.Name << "'." << std::endl;
                  continue;
                  }
               program = res.MeshProgram;
               state &= ~BGFX_STATE_BLEND_ALPHA;
               state |= BGFX_STATE_CULL_CW;

               if (layerConst.Mesh->Dynamic)
                  {
                  bgfx::setVertexBuffer(0, layerConst.Mesh->dvbh, 0, layerConst.Mesh->numVertices);
                  }
               else
                  {
                  bgfx::setVertexBuffer(0, layerConst.Mesh->vbh);
                  }
               bgfx::setIndexBuffer(layerConst.Mesh->ibh);
               }

            if (!bgfx::isValid(program))
               continue;

            bgfx::setState(state);
            bgfx::submit(viewId, program);

            // Warn if instance count was capped
            if (scratch.size() >= kCpuMaxInstancesPerLayer && ShouldLogCpuClampWarning(layerConst.Guid))
               {
               std::cerr << "[TerrainGrass] CPU grass layer '" << layerConst.Name << "' reached the instance cap ("
                  << kCpuMaxInstancesPerLayer << "). Output was clamped." << std::endl;
               }
            }
         }

      } // namespace

   void SetGenerationMode(GrassGenerationMode mode)
      {
      GetGenerationModeAtomic().store(mode, std::memory_order_relaxed);
      }

   GrassGenerationMode GetGenerationMode()
      {
      return GetGenerationModeAtomic().load(std::memory_order_relaxed);
      }

   const char* GetGenerationModeLabel(GrassGenerationMode mode)
      {
      switch (mode)
         {
         case GrassGenerationMode::CpuDensity: return "CPU Density";
         case GrassGenerationMode::Compute:
         default: return "GPU Compute";
         }
      }

   void InitializeRendererResources()
      {
      GrassRendererResources& res = GetResources();
      if (res.Initialized)
         return;

      BillboardVertex vertices[] = {
         { -0.5f, 0.0f, 0.0f, 0.0f, 1.0f },
         {  0.5f, 0.0f, 0.0f, 1.0f, 1.0f },
         { -0.5f, 1.0f, 0.0f, 0.0f, 0.0f },
         {  0.5f, 1.0f, 0.0f, 1.0f, 0.0f },
         };
      const uint16_t indices[] = { 0, 1, 2, 1, 3, 2 };

      res.BillboardLayout.begin()
         .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
         .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
         .end();

      res.InstanceLayout.begin()
         .add(bgfx::Attrib::TexCoord7, 4, bgfx::AttribType::Float)
         .add(bgfx::Attrib::TexCoord6, 4, bgfx::AttribType::Float)
         .add(bgfx::Attrib::TexCoord5, 4, bgfx::AttribType::Float)
         .add(bgfx::Attrib::TexCoord4, 4, bgfx::AttribType::Float)
         .end();
      
      // Create a layout handle for explicit layout binding when using instance data
      res.InstanceLayoutHandle = bgfx::createVertexLayout(res.InstanceLayout);

      const bgfx::Memory* vMem = bgfx::copy(vertices, sizeof(vertices));
      const bgfx::Memory* iMem = bgfx::copy(indices, sizeof(indices));
      res.BillboardVB = bgfx::createVertexBuffer(vMem, res.BillboardLayout);
      res.BillboardIB = bgfx::createIndexBuffer(iMem);

      res.BillboardProgram = ShaderManager::Instance().LoadProgram("vs_grass_billboard", "fs_grass");
      res.MeshProgram = ShaderManager::Instance().LoadProgram("vs_grass_mesh", "fs_grass");

      bgfx::ShaderHandle csr = ShaderManager::Instance().LoadShader("cs_grass_reset", ShaderType::Compute);
      bgfx::ShaderHandle csg = ShaderManager::Instance().LoadShader("cs_grass_generate", ShaderType::Compute);
      bgfx::ShaderHandle csf = ShaderManager::Instance().LoadShader("cs_grass_finalize", ShaderType::Compute);
      if (bgfx::isValid(csr)) res.ComputeResetProgram = bgfx::createProgram(csr, true);
      if (bgfx::isValid(csg)) res.ComputeGenerateProgram = bgfx::createProgram(csg, true);
      if (bgfx::isValid(csf)) res.ComputeFinalizeProgram = bgfx::createProgram(csf, true);

      res.s_Albedo = bgfx::createUniform("s_GrassTex", bgfx::UniformType::Sampler);
      res.s_GrassDensity = bgfx::createUniform("s_DensityTexture", bgfx::UniformType::Sampler);
      res.u_LayerParams = bgfx::createUniform("u_GrassLayerParams", bgfx::UniformType::Vec4);
      res.u_WindDir = bgfx::createUniform("u_GrassWindDir", bgfx::UniformType::Vec4);
      res.u_FadeParams = bgfx::createUniform("u_GrassFadeParams", bgfx::UniformType::Vec4);
      res.u_Time = bgfx::createUniform("u_GrassTime", bgfx::UniformType::Vec4);
      res.u_BaseColor = bgfx::createUniform("u_GrassBaseColor", bgfx::UniformType::Vec4);
      res.u_ColorVariance = bgfx::createUniform("u_GrassColorVariance", bgfx::UniformType::Vec4);
      res.u_GrassTerrain0 = bgfx::createUniform("u_GrassTerrain0", bgfx::UniformType::Vec4);
      res.u_GrassTerrain1 = bgfx::createUniform("u_GrassTerrain1", bgfx::UniformType::Vec4);
      res.u_GrassLayer0 = bgfx::createUniform("u_GrassLayer0", bgfx::UniformType::Vec4);
      res.u_GrassLayer1 = bgfx::createUniform("u_GrassLayer1", bgfx::UniformType::Vec4);
      res.u_GrassMaskParams = bgfx::createUniform("u_GrassMaskParams", bgfx::UniformType::Vec4);
      res.u_GrassCameraPos = bgfx::createUniform("u_GrassCameraPos", bgfx::UniformType::Vec4);
      res.u_GrassCameraRight = bgfx::createUniform("u_GrassCameraRight", bgfx::UniformType::Vec4);
      res.u_GrassCameraForward = bgfx::createUniform("u_GrassCameraForward", bgfx::UniformType::Vec4);
      res.u_GrassWorld = bgfx::createUniform("u_GrassWorld", bgfx::UniformType::Vec4, 4);
      res.u_GrassNormal = bgfx::createUniform("u_GrassNormal", bgfx::UniformType::Vec4, 3);
      res.u_GrassDispatch = bgfx::createUniform("u_GrassDispatch", bgfx::UniformType::Vec4);
      res.u_GrassDrawArgs = bgfx::createUniform("u_GrassDrawArgs", bgfx::UniformType::Vec4);
      res.u_GrassSeedTime = bgfx::createUniform("u_GrassSeedTime", bgfx::UniformType::Vec4);
      res.u_GrassTerrainInfo = bgfx::createUniform("u_GrassTerrainInfo", bgfx::UniformType::Vec4);
      res.u_GrassDeformParams = bgfx::createUniform("u_GrassDeformParams", bgfx::UniformType::Vec4);
      res.s_DeformationTex = bgfx::createUniform("s_deformationTex", bgfx::UniformType::Sampler);
#if CM_DEBUG_GRASS
      res.u_GrassDebugInfo = bgfx::createUniform("u_GrassDebugInfo", bgfx::UniformType::Vec4);
      if (!EnsureDebugTexture(res))
         {
         std::cerr << "[TerrainGrass] Grass debug telemetry disabled (texture format unavailable)." << std::endl;
         }
#endif

      const uint8_t white[4] = { 255, 255, 255, 255 };
      res.WhiteTexture = TextureLoader::Load2DFromRGBA(white, 1, 1, false);
      {
         const bgfx::Memory* whiteMem = bgfx::copy(white, sizeof(white));
         res.ComputeWhiteTexture = bgfx::createTexture2D(
            1,
            1,
            false,
            1,
            bgfx::TextureFormat::RGBA8,
            BGFX_TEXTURE_COMPUTE_WRITE | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
            whiteMem);
      }
      {
         const uint8_t solidHole = 0;
         const bgfx::Memory* holeMem = bgfx::copy(&solidHole, sizeof(solidHole));
         res.ComputeSolidHoleTexture = bgfx::createTexture2D(
            1,
            1,
            false,
            1,
            bgfx::TextureFormat::R8,
            BGFX_TEXTURE_COMPUTE_WRITE | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
            holeMem);
      }

      res.Initialized = true;
      }

   void ShutdownRendererResources()
      {
      GrassRendererResources& res = GetResources();
      if (!res.Initialized)
         return;

      if (bgfx::isValid(res.BillboardVB)) bgfx::destroy(res.BillboardVB);
      if (bgfx::isValid(res.BillboardIB)) bgfx::destroy(res.BillboardIB);
      if (bgfx::isValid(res.BillboardProgram)) bgfx::destroy(res.BillboardProgram);
      if (bgfx::isValid(res.MeshProgram)) bgfx::destroy(res.MeshProgram);
      if (bgfx::isValid(res.ComputeResetProgram)) bgfx::destroy(res.ComputeResetProgram);
      if (bgfx::isValid(res.ComputeGenerateProgram)) bgfx::destroy(res.ComputeGenerateProgram);
      if (bgfx::isValid(res.ComputeFinalizeProgram)) bgfx::destroy(res.ComputeFinalizeProgram);
      if (bgfx::isValid(res.InstanceLayoutHandle)) bgfx::destroy(res.InstanceLayoutHandle);

      if (bgfx::isValid(res.s_Albedo)) bgfx::destroy(res.s_Albedo);
      if (bgfx::isValid(res.s_GrassDensity)) bgfx::destroy(res.s_GrassDensity);
      if (bgfx::isValid(res.u_LayerParams)) bgfx::destroy(res.u_LayerParams);
      if (bgfx::isValid(res.u_WindDir)) bgfx::destroy(res.u_WindDir);
      if (bgfx::isValid(res.u_FadeParams)) bgfx::destroy(res.u_FadeParams);
      if (bgfx::isValid(res.u_Time)) bgfx::destroy(res.u_Time);
      if (bgfx::isValid(res.u_BaseColor)) bgfx::destroy(res.u_BaseColor);
      if (bgfx::isValid(res.u_ColorVariance)) bgfx::destroy(res.u_ColorVariance);
      if (bgfx::isValid(res.u_GrassTerrain0)) bgfx::destroy(res.u_GrassTerrain0);
      if (bgfx::isValid(res.u_GrassTerrain1)) bgfx::destroy(res.u_GrassTerrain1);
      if (bgfx::isValid(res.u_GrassLayer0)) bgfx::destroy(res.u_GrassLayer0);
      if (bgfx::isValid(res.u_GrassLayer1)) bgfx::destroy(res.u_GrassLayer1);
      if (bgfx::isValid(res.u_GrassMaskParams)) bgfx::destroy(res.u_GrassMaskParams);
      if (bgfx::isValid(res.u_GrassCameraPos)) bgfx::destroy(res.u_GrassCameraPos);
      if (bgfx::isValid(res.u_GrassCameraRight)) bgfx::destroy(res.u_GrassCameraRight);
      if (bgfx::isValid(res.u_GrassCameraForward)) bgfx::destroy(res.u_GrassCameraForward);
      if (bgfx::isValid(res.u_GrassWorld)) bgfx::destroy(res.u_GrassWorld);
      if (bgfx::isValid(res.u_GrassNormal)) bgfx::destroy(res.u_GrassNormal);
      if (bgfx::isValid(res.u_GrassDispatch)) bgfx::destroy(res.u_GrassDispatch);
      if (bgfx::isValid(res.u_GrassDrawArgs)) bgfx::destroy(res.u_GrassDrawArgs);
      if (bgfx::isValid(res.u_GrassSeedTime)) bgfx::destroy(res.u_GrassSeedTime);
      if (bgfx::isValid(res.WhiteTexture)) bgfx::destroy(res.WhiteTexture);
      if (bgfx::isValid(res.ComputeWhiteTexture)) bgfx::destroy(res.ComputeWhiteTexture);
      if (bgfx::isValid(res.ComputeSolidHoleTexture)) bgfx::destroy(res.ComputeSolidHoleTexture);
#if CM_DEBUG_GRASS
      if (bgfx::isValid(res.u_GrassDebugInfo)) bgfx::destroy(res.u_GrassDebugInfo);
      if (bgfx::isValid(res.DebugTexture)) bgfx::destroy(res.DebugTexture);
#endif

      res = GrassRendererResources{};
      }

   void SyncLayerResources(TerrainComponent& terrain)
      {
      for (TerrainGrassLayerDesc& layer : terrain.GrassLayers)
         {
         EnsureLayerHandles(layer);
         UploadDensityTexture(terrain, layer);
         }
      }

   void EnsureChunksUpToDate(TerrainComponent& terrain)
      {
      // Only sync resources if grass data is dirty - prevents unnecessary per-frame work
      if (terrain.GrassStructureDirty || terrain.GrassMasksDirty)
         {
         SyncLayerResources(terrain);
         terrain.GrassStructureDirty = false;
         terrain.GrassMasksDirty = false;
         }
      }

   void ApplyPaint(TerrainComponent& terrain, size_t layerIndex, const glm::vec2& centerXZ, float radius, float deltaSign, float falloffPower)
      {
      if (terrain.GrassLayers.empty())
         return;

      layerIndex = std::min(layerIndex, terrain.GrassLayers.size() - 1);
      TerrainGrassLayerDesc& layer = terrain.GrassLayers[layerIndex];
      layer.EnsureMaskSize(terrain.GridResolution);

      glm::vec2 cell = Terrain::GetCellSize(terrain);
      cell = glm::max(cell, glm::vec2(0.0001f));

      const int minX = std::max(0, static_cast<int>(std::floor((centerXZ.x - radius) / cell.x)));
      const int maxX = std::min(static_cast<int>(terrain.GridResolution) - 1, static_cast<int>(std::ceil((centerXZ.x + radius) / cell.x)));
      const int minZ = std::max(0, static_cast<int>(std::floor((centerXZ.y - radius) / cell.y)));
      const int maxZ = std::min(static_cast<int>(terrain.GridResolution) - 1, static_cast<int>(std::ceil((centerXZ.y + radius) / cell.y)));

      // Debug paint parameters (always log for first 20 calls to ensure we see them)
      static int s_PaintDebugCount = 0;
      if (s_PaintDebugCount < 20)
         {
         ++s_PaintDebugCount;
         std::cout << "[TerrainGrass::ApplyPaint] CALLED! centerXZ=(" << centerXZ.x << "," << centerXZ.y << ")"
            << " radius=" << radius << " deltaSign=" << deltaSign
            << " cell=(" << cell.x << "," << cell.y << ")"
            << " gridRes=" << terrain.GridResolution
            << " range=[" << minX << "-" << maxX << ", " << minZ << "-" << maxZ << "]"
            << " maskSize=" << layer.PaintedMask.size() << std::endl;
         }

      const float radiusSq = radius * radius;
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

            float dist = std::sqrt(distSq);
            float falloff = 1.0f - (dist / radius);
            falloff = std::pow(glm::clamp(falloff, 0.0f, 1.0f), falloffPower);
            const float delta = deltaSign * falloff * 255.0f;
            const size_t idx = static_cast<size_t>(z) * terrain.GridResolution + x;

            float value = static_cast<float>(layer.PaintedMask[idx]);
            value = glm::clamp(value + delta, 0.0f, 255.0f);
            const uint8_t quantized = static_cast<uint8_t>(value);
            if (layer.PaintedMask[idx] != quantized)
               {
               layer.PaintedMask[idx] = quantized;
               changed = true;
               dirtyMin.x = std::min(dirtyMin.x, x);
               dirtyMin.y = std::min(dirtyMin.y, z);
               dirtyMax.x = std::max(dirtyMax.x, x);
               dirtyMax.y = std::max(dirtyMax.y, z);
               }
            }
         }

      if (changed)
         {
         layer.DensityDirty = true;
         if (layer.DensityDirtyMin.x == 0 && layer.DensityDirtyMax.x == 0)
            {
            layer.DensityDirtyMin = dirtyMin;
            layer.DensityDirtyMax = dirtyMax;
            }
         else
            {
            layer.DensityDirtyMin = glm::min(layer.DensityDirtyMin, dirtyMin);
            layer.DensityDirtyMax = glm::max(layer.DensityDirtyMax, dirtyMax);
            }

         UploadDensityTexture(terrain, layer);
         layer.RuntimeDirty = true;
         terrain.GrassMasksDirty = true;
         terrain.AssetDirty = true;

         // Debug: show paint result
         static int s_PaintResultCount = 0;
         if (s_PaintResultCount < 5)
            {
            ++s_PaintResultCount;
            uint32_t nonZero = 0;
            for (uint8_t v : layer.PaintedMask)
               if (v > 0) ++nonZero;
            std::cout << "[TerrainGrass::ApplyPaint] Changed! dirtyRange=[" << dirtyMin.x << "-" << dirtyMax.x
               << ", " << dirtyMin.y << "-" << dirtyMax.y << "] nonZeroAfter=" << nonZero << std::endl;
            }
         }
      }

   void Render(const TerrainComponent& terrain,
      const glm::mat4& worldTransform,
      uint16_t viewId,
      const glm::vec3& cameraPos,
      const glm::mat4& viewMatrix,
      const glm::mat4& projMatrix,
      bool enableFrustumCulling)
      {
      GrassRendererResources& res = GetResources();
      if (!res.Initialized || terrain.GrassLayers.empty())
         return;

      // Check if any layers want GPU mode
      bool hasGpuLayers = false;
      bool hasCpuLayers = false;
      for (const auto& layer : terrain.GrassLayers)
         {
         if (!layer.Enabled) continue;
         if (layer.UseGPU) hasGpuLayers = true;
         else hasCpuLayers = true;
         }

      // Always render CPU layers first (most common case)
      if (hasCpuLayers)
         {
         RenderCpuPipeline(terrain, worldTransform, viewId, cameraPos, viewMatrix, projMatrix, enableFrustumCulling, res);
         }

      // Skip GPU path if no layers want it
      if (!hasGpuLayers)
         return;

      const bgfx::Caps* caps = bgfx::getCaps();
      if (!(caps->supported & BGFX_CAPS_COMPUTE))
         {
         std::cerr << "[TerrainGrass] Compute shaders are not supported by the active renderer." << std::endl;
         return;
         }
      static bool s_ComputeBindingWarned = false;
      if (caps->limits.maxComputeBindings < kGrassGenerateRequiredBindings)
         {
         if (!s_ComputeBindingWarned)
            {
            s_ComputeBindingWarned = true;
            std::cerr << "[TerrainGrass] GPU mode requires at least "
               << unsigned(kGrassGenerateRequiredBindings)
               << " compute bindings, but the active renderer reports "
               << caps->limits.maxComputeBindings << "." << std::endl;
            }
         return;
         }
      
      // Check for indirect draw support
      static bool s_IndirectWarned = false;
      const bool hasIndirectSupport = (caps->supported & BGFX_CAPS_DRAW_INDIRECT) != 0;
      if (!hasIndirectSupport && !s_IndirectWarned)
         {
         s_IndirectWarned = true;
         std::cerr << "[TerrainGrass] WARNING: BGFX_CAPS_DRAW_INDIRECT not supported. GPU grass may not work correctly." << std::endl;
         }

      if (terrain.Chunks.empty())
         return;
      const TerrainChunk& chunk = terrain.Chunks.front();
      if (!bgfx::isValid(chunk.HeightTexture))
         return;
#if CM_DEBUG_GRASS
      GrassDebugState& debugState = GetDebugState();
      bool debugActive = debugState.Enabled;
      bool telemetryReady = false;
      if (debugActive)
         {
         telemetryReady = EnsureDebugTexture(res);
         debugState.TelemetrySupported = telemetryReady;
         if (telemetryReady)
            {
            debugState.FrameCounter++;
            debugState.Samples.clear();
            ProcessDebugReadback(res, debugState);
            }
         else
            {
            debugState.Samples.clear();
            }
         }
      else
         {
         debugState.Samples.clear();
         }
      const glm::vec2 terrainCellSize = Terrain::GetCellSize(terrain);
      if (debugActive)
         {
         debugState.CameraChunkIndex = FindCameraChunkIndex(terrain, cameraPos, terrainCellSize);
         }
      const uint32_t chunkIndex = 0;
      uint32_t debugEntriesUsed = 0;
#endif

      const glm::mat4 invView = glm::inverse(viewMatrix);
      glm::vec3 cameraRight = glm::vec3(invView[0][0], invView[0][1], invView[0][2]);
      glm::vec3 cameraForward = -glm::vec3(invView[2][0], invView[2][1], invView[2][2]);
      if (glm::length2(cameraRight) > 1e-6f) cameraRight = glm::normalize(cameraRight);
      else cameraRight = glm::vec3(1.0f, 0.0f, 0.0f);
      if (glm::length2(cameraForward) > 1e-6f) cameraForward = glm::normalize(cameraForward);
      else cameraForward = glm::vec3(0.0f, 0.0f, -1.0f);

      const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(worldTransform)));

      const float timeSec = GetTimeSeconds();
      const glm::vec4 timeUniform(timeSec, 0.0f, 0.0f, 0.0f);
      bgfx::setUniform(res.u_Time, &timeUniform.x);

      // Debug: Check compute program validity once
      static bool s_DebugComputeLogged = false;
      if (!s_DebugComputeLogged)
         {
         s_DebugComputeLogged = true;
         std::cout << "[TerrainGrass] Compute programs valid: Reset=" << bgfx::isValid(res.ComputeResetProgram)
            << " Generate=" << bgfx::isValid(res.ComputeGenerateProgram)
            << " Finalize=" << bgfx::isValid(res.ComputeFinalizeProgram) << std::endl;
         std::cout << "[TerrainGrass] Billboard/Mesh programs valid: Billboard=" << bgfx::isValid(res.BillboardProgram)
            << " Mesh=" << bgfx::isValid(res.MeshProgram) << std::endl;
         std::cout << "[TerrainGrass] Density sampler valid: " << bgfx::isValid(res.s_GrassDensity) << std::endl;
         }

      for (size_t layerIndex = 0; layerIndex < terrain.GrassLayers.size(); ++layerIndex)
         {
         const TerrainGrassLayerDesc& layerConst = terrain.GrassLayers[layerIndex];
         // Only render layers that have UseGPU enabled
         if (!layerConst.Enabled || layerConst.PaintedMask.empty() || !layerConst.UseGPU)
            continue;

         TerrainGrassLayerDesc& layer = const_cast<TerrainGrassLayerDesc&>(layerConst);

         // Validate required resources for GPU path
         if (!bgfx::isValid(layer.DensityTexture))
            {
            static bool s_WarnedDensity = false;
            if (!s_WarnedDensity)
               {
               s_WarnedDensity = true;
               std::cerr << "[TerrainGrass] GPU mode requires density texture for layer '" << layer.Name << "'. Skipping." << std::endl;
               }
            continue;
            }
         if (!bgfx::isValid(chunk.HeightTexture))
            {
            static bool s_WarnedHeight = false;
            if (!s_WarnedHeight)
               {
               s_WarnedHeight = true;
               std::cerr << "[TerrainGrass] GPU mode requires height texture. Skipping." << std::endl;
               }
            continue;
            }
         if (!bgfx::isValid(res.ComputeGenerateProgram) || !bgfx::isValid(res.ComputeResetProgram) || !bgfx::isValid(res.ComputeFinalizeProgram))
            {
            static bool s_WarnedCompute = false;
            if (!s_WarnedCompute)
               {
               s_WarnedCompute = true;
               std::cerr << "[TerrainGrass] GPU mode requires valid compute programs. Skipping." << std::endl;
               }
            continue;
            }
         if (!bgfx::isValid(res.BillboardProgram))
            {
            static bool s_WarnedProgram = false;
            if (!s_WarnedProgram)
               {
               s_WarnedProgram = true;
               std::cerr << "[TerrainGrass] GPU mode requires valid billboard program. Skipping." << std::endl;
               }
            continue;
            }

         // Debug: Log layer state periodically and when mask changes
         static std::unordered_map<uint64_t, int> s_LastLoggedMask;
         const uint64_t layerKey = layer.Guid.low ^ layer.Guid.high;
         const int currentMask = static_cast<int>(layer.Mask);
         auto it = s_LastLoggedMask.find(layerKey);
         const bool maskChanged = (it == s_LastLoggedMask.end()) || (it->second != currentMask);
         if (maskChanged)
            {
            s_LastLoggedMask[layerKey] = currentMask;
            uint32_t nonZeroCount = 0;
            uint32_t maxVal = 0;
            for (uint8_t v : layer.PaintedMask)
               {
               if (v > 0) ++nonZeroCount;
               if (v > maxVal) maxVal = v;
               }
            std::cout << "[TerrainGrass] Layer '" << layer.Name << "' mask: size=" << layer.PaintedMask.size()
               << " nonZero=" << nonZeroCount << " maxVal=" << maxVal
               << " densityTex=" << bgfx::isValid(layer.DensityTexture)
               << " mask=" << currentMask << " (maskChanged!)" << std::endl;
            }

         const uint32_t samplingMult = std::clamp(terrain.GrassSamplingMultiplier, 1u, 16u);
         const uint32_t grassGridRes = terrain.GridResolution * samplingMult;
         const uint32_t capacity = std::max<uint32_t>(grassGridRes * grassGridRes * kMaxBladesPerTexel, 1u);
         GrassLayerRuntime* runtime = EnsureRuntimeBuffers(layer, capacity, res.InstanceLayout);
         if (!runtime)
            continue;

         // Note: SetCommonLayerUniforms is called later, right before submit,
         // because compute dispatches will consume any state set here.

         DispatchReset(viewId, res, *runtime);
         
         // Debug: Log compute dispatch parameters once per layer
         static std::unordered_set<uint64_t> s_DispatchLogged;
         if (s_DispatchLogged.find(layerKey) == s_DispatchLogged.end())
            {
            s_DispatchLogged.insert(layerKey);
            uint32_t nonZero = 0;
            for (uint8_t v : layer.PaintedMask) if (v > 0) ++nonZero;
            std::cout << "[TerrainGrass::Dispatch] Layer '" << layer.Name << "'"
               << " capacity=" << capacity
               << " gridRes=" << terrain.GridResolution
               << " heightRange=[" << layer.HeightRange.x << "," << layer.HeightRange.y << "]"
               << " maxSlope=" << layer.MaxSlopeDegrees
               << " distRange=[" << layer.MinDistance << "," << layer.MaxDistance << "]"
               << " density=" << layer.DensityPerSquareMeter
               << " nonZeroPaint=" << nonZero
               << " heightTexValid=" << bgfx::isValid(chunk.HeightTexture)
               << " maxTerrainHeight=" << terrain.MaxHeight
               << std::endl;
            }
         
         DispatchGenerate(viewId, res, terrain, layer, *runtime, worldTransform, normalMatrix, cameraPos, cameraRight, cameraForward, chunk);
#if CM_DEBUG_GRASS
         uint32_t debugSlotValue = std::numeric_limits<uint32_t>::max();
         bool debugSlotValid = false;
         if (telemetryReady)
            {
            debugSlotValue = EncodeDebugSlot(chunkIndex, static_cast<uint32_t>(layerIndex));
            debugSlotValid = debugSlotValue != std::numeric_limits<uint32_t>::max();
            }
#endif
         DispatchFinalize(viewId, res, *runtime, capacity
#if CM_DEBUG_GRASS
            , debugSlotValid ? static_cast<int>(debugSlotValue) : -1
            , telemetryReady && debugSlotValid
#endif
         );

         // Validate albedo texture
         bgfx::TextureHandle albedo = bgfx::isValid(layer.BillboardTexture) ? layer.BillboardTexture : res.WhiteTexture;
         if (!bgfx::isValid(albedo))
            {
            static bool s_WarnedAlbedo = false;
            if (!s_WarnedAlbedo)
               {
               s_WarnedAlbedo = true;
               std::cerr << "[TerrainGrass] GPU mode: no valid albedo texture for layer '" << layer.Name << "'." << std::endl;
               }
            continue;
            }

         // Validate instance buffer before binding
         if (!bgfx::isValid(runtime->InstanceBuffer))
            {
            static bool s_WarnedInstance = false;
            if (!s_WarnedInstance)
               {
               s_WarnedInstance = true;
               std::cerr << "[TerrainGrass] GPU mode: invalid instance buffer for layer '" << layer.Name << "'." << std::endl;
               }
            continue;
            }

         uint64_t state =
            BGFX_STATE_WRITE_RGB |
            BGFX_STATE_WRITE_A |
            BGFX_STATE_WRITE_Z |
            BGFX_STATE_DEPTH_TEST_LESS |
            BGFX_STATE_MSAA;

         bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;

         if (layer.RenderMode == GrassRenderMode::Billboard || layer.RenderMode == GrassRenderMode::BillboardFixed)
            {
            program = res.BillboardProgram;
            state |= BGFX_STATE_BLEND_ALPHA;
            state &= ~BGFX_STATE_WRITE_Z;
            state &= ~BGFX_STATE_DEPTH_TEST_MASK;
            state |= BGFX_STATE_DEPTH_TEST_LEQUAL;
            state &= ~(BGFX_STATE_CULL_CW | BGFX_STATE_CULL_CCW);
            bgfx::setVertexBuffer(0, res.BillboardVB);
            bgfx::setIndexBuffer(res.BillboardIB);
            }
         else
            {
            if (!layer.Mesh)
               {
               std::cerr << "[TerrainGrass] Missing mesh asset for grass layer '" << layer.Name << "'." << std::endl;
               continue;
               }
            program = res.MeshProgram;
            state &= ~BGFX_STATE_BLEND_ALPHA;
            state |= BGFX_STATE_CULL_CW;

            if (layer.Mesh->Dynamic)
               {
               bgfx::setVertexBuffer(0, layer.Mesh->dvbh, 0, layer.Mesh->numVertices);
               }
            else
               {
               bgfx::setVertexBuffer(0, layer.Mesh->vbh);
               }
            bgfx::setIndexBuffer(layer.Mesh->ibh);
            }
         
         // Bind instance buffer (matches bgfx example 48-drawindirect pattern)
         bgfx::setInstanceDataBuffer(runtime->InstanceBuffer, 0, runtime->Capacity);
         
         // Set uniforms for the render pass (must be after compute dispatches, before submit)
         // This includes deformation texture, wind params, colors, etc.
         SetCommonLayerUniforms(layer, terrain, worldTransform, res);
         
         // Set time uniform (needed for wind animation in vertex shader)
         const float timeSec = GetTimeSeconds();
         const glm::vec4 gpuTimeUniform(timeSec, 0.0f, 0.0f, 0.0f);
         bgfx::setUniform(res.u_Time, &gpuTimeUniform.x);
         
         // Bind albedo texture
         bgfx::setTexture(0, res.s_Albedo, albedo);

         if (!bgfx::isValid(program))
            continue;

         bgfx::setState(state);
         
         // Validate indirect buffer before submit
         if (!bgfx::isValid(runtime->IndirectArgs))
            {
            static bool s_WarnedIndirect = false;
            if (!s_WarnedIndirect)
               {
               s_WarnedIndirect = true;
               std::cerr << "[TerrainGrass] GPU mode: invalid indirect args buffer. Skipping draw." << std::endl;
               }
            continue;
            }
         
         // Use indirect draw - the instance count comes from IndirectArgs buffer
         // which was populated by the finalize compute shader.
         // Note: bgfx indirect draws with DynamicVertexBuffer instance data require
         // the instance buffer to be bound via setInstanceDataBuffer (done above).
         bgfx::submit(viewId, program, runtime->IndirectArgs, 0, 1, 0, BGFX_DISCARD_NONE);
#if CM_DEBUG_GRASS
         const bool drawSubmitted = true;
         if (debugActive)
            {
            if (telemetryReady)
               {
               const bool telemetryForSample = telemetryReady && debugSlotValid;
               if (debugSlotValid)
                  {
                  debugEntriesUsed = std::max(debugEntriesUsed, debugSlotValue + 1);
                  PushLayerSample(terrain, layer, chunk, static_cast<uint32_t>(layerIndex), chunkIndex, debugSlotValue, drawSubmitted, worldTransform, cameraPos, telemetryForSample);
                  }
               else
                  {
                  PushLayerSample(terrain, layer, chunk, static_cast<uint32_t>(layerIndex), chunkIndex, std::numeric_limits<uint32_t>::max(), drawSubmitted, worldTransform, cameraPos, telemetryForSample);
                  }
               }
            else
               {
               // Still record that this layer attempted to draw even without telemetry support.
               PushLayerSample(terrain, layer, chunk, static_cast<uint32_t>(layerIndex), chunkIndex, std::numeric_limits<uint32_t>::max(), drawSubmitted, worldTransform, cameraPos, false);
               }
            }
#endif
         }
#if CM_DEBUG_GRASS
      if (telemetryReady)
         {
         RequestDebugReadback(res, debugState, debugEntriesUsed);
         }
#endif
      }

#if CM_DEBUG_GRASS
   void SetDebugOverlayEnabled(bool enabled)
      {
      GrassDebugState& state = GetDebugState();
      if (state.Enabled == enabled)
         return;
      state.Enabled = enabled;
      if (!enabled)
         {
         state.Samples.clear();
         state.PendingReadback = false;
         state.DumpRequested = false;
         }
      }

   bool IsDebugOverlayEnabled()
      {
      return GetDebugState().Enabled;
      }

   void RequestDebugDump()
      {
      GrassDebugState& state = GetDebugState();
      state.DumpRequested = true;
      }

   void RenderDebugWindow()
      {
      GrassDebugState& state = GetDebugState();
      if (!state.Enabled)
         return;

      bool open = state.Enabled;
      ImGui::SetNextWindowSize(ImVec2(580.0f, 420.0f), ImGuiCond_FirstUseEver);
      if (ImGui::Begin("Grass Debug", &open))
         {
         if (!state.TelemetrySupported)
            {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "GPU telemetry unavailable (missing compute/readback support).");
            }
         ImGui::Checkbox("Focus camera chunk", &state.FocusCameraChunkOnly);
         int period = static_cast<int>(state.ReadbackPeriod);
         if (ImGui::SliderInt("Readback period (frames)", &period, 1, 60))
            {
            state.ReadbackPeriod = static_cast<uint32_t>(period);
            }
         ImGui::SameLine();
         ImGui::TextUnformatted(state.PendingReadback ? "GPU counters: pending" : "GPU counters: ready");
         if (ImGui::Button("Dump summary to log"))
            {
            state.DumpRequested = true;
            }
         ImGui::SameLine();
         ImGui::Text("Entries: %zu", state.Samples.size());

         const ImGuiTableFlags tableFlags =
            ImGuiTableFlags_RowBg |
            ImGuiTableFlags_Borders |
            ImGuiTableFlags_Resizable |
            ImGuiTableFlags_ScrollY;
         if (ImGui::BeginTable("GrassDebugTable", 9, tableFlags))
            {
            ImGui::TableSetupColumn("Chunk", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Layer", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Density", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Mask", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Blades", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Instances", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Distance", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableSetupColumn("Height/Slope", ImGuiTableColumnFlags_WidthFixed, 140.0f);
            ImGui::TableSetupColumn("Notes", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (const GrassLayerDebugSample& sample : state.Samples)
               {
               if (state.FocusCameraChunkOnly && sample.ChunkIndex != state.CameraChunkIndex)
                  continue;

               const bool hasInstances = sample.TelemetryAvailable
                  ? (sample.InstanceCount > 0)
                  : (sample.EstimatedInstances > 0.1f);
               const bool expectVisible = sample.ExpectVisible;
               ImU32 rowColor = 0;
               if (hasInstances)
                  {
                  rowColor = ImGui::GetColorU32(ImVec4(0.2f, 0.52f, 0.2f, 0.15f));
                  }
               else if (expectVisible)
                  {
                  rowColor = ImGui::GetColorU32(ImVec4(0.8f, 0.65f, 0.15f, 0.15f));
                  }
               else
                  {
                  rowColor = ImGui::GetColorU32(ImVec4(0.7f, 0.2f, 0.2f, 0.15f));
                  }

               ImGui::TableNextRow();
               if (rowColor != 0)
                  {
                  ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, rowColor);
                  }

               ImGui::TableSetColumnIndex(0);
               ImGui::Text("%u (%d,%d)", sample.ChunkIndex, sample.ChunkStart.x, sample.ChunkStart.y);

               ImGui::TableSetColumnIndex(1);
               const char* displayName = sample.Name.empty() ? "Unnamed" : sample.Name.c_str();
               ImGui::Text("%u - %s", sample.LayerIndex, displayName);

               ImGui::TableSetColumnIndex(2);
               ImGui::Text("%.2f", sample.DensitySample);

               ImGui::TableSetColumnIndex(3);
               const std::string maskLabel = DescribeMaskSource(sample);
               ImGui::Text("%.2f (%s)", sample.MaskSample, maskLabel.c_str());

               ImGui::TableSetColumnIndex(4);
               ImGui::Text("%.2f", sample.ApproxBladesPerCell);

               ImGui::TableSetColumnIndex(5);
               if (sample.TelemetryAvailable)
                  {
                  ImGui::Text("%u", sample.InstanceCount);
                  }
               else
                  {
                  ImGui::Text("~%.0f", sample.EstimatedInstances);
                  }

               ImGui::TableSetColumnIndex(6);
               if (sample.DistanceGateActive)
                  {
                  ImGui::Text("%.1f - %.1f", sample.MinDistance, sample.MaxDistance);
                  ImGui::TextDisabled("chunk %.1f - %.1f", sample.DistanceRange.x, sample.DistanceRange.y);
                  }
               else
                  {
                  ImGui::TextUnformatted("Disabled");
                  }

               ImGui::TableSetColumnIndex(7);
               ImGui::Text("%.1f - %.1f / %.0f°", sample.HeightRange.x, sample.HeightRange.y, sample.SlopeLimit);

               ImGui::TableSetColumnIndex(8);
               const std::string notes = BuildSampleNotes(sample);
               ImGui::PushTextWrapPos(0.0f);
               ImGui::TextUnformatted(notes.c_str());
               ImGui::PopTextWrapPos();
               }
            ImGui::EndTable();
            }
         }
      ImGui::End();

      if (state.DumpRequested)
         {
         DumpGrassDebugToLog(state);
         state.DumpRequested = false;
         state.LastDumpFrame = state.FrameCounter;
         }

      state.Enabled = open;
      }
#endif // CM_DEBUG_GRASS

   void MarkAllDirty(TerrainComponent& terrain)
      {
      terrain.GrassStructureDirty = true;
      terrain.GrassMasksDirty = true;
      for (TerrainGrassLayerDesc& layer : terrain.GrassLayers)
         {
         layer.RuntimeDirty = true;
         layer.DensityDirty = true;
         layer.DensityDirtyMin = glm::ivec2(0);
         layer.DensityDirtyMax = glm::ivec2(0);
         }
      }

   //=============================================================================
   // Grass Deformation System
   //=============================================================================

   void InitializeDeformation(TerrainComponent& terrain)
      {
      if (!terrain.DeformationConfig.Enabled)
         return;
         
      const uint32_t res = terrain.DeformationConfig.TextureResolution;
      const size_t texelCount = static_cast<size_t>(res) * res;
      
      // Initialize CPU buffer with zeros (no deformation)
      terrain.DeformationData.resize(texelCount, glm::vec4(0.0f));
      terrain.DeformationDirty = true;
      terrain.DeformationTime = 0.0f;
      
      // Create GPU texture if not already created. Provide initial data up front so
      // the texture is immediately sampleable and stays in sync with the CPU buffer.
      if (!bgfx::isValid(terrain.DeformationTexture))
         {
         // RGBA16F texture: RG = direction*strength, B = unused, A = timestamp for decay
         const uint64_t flags = 
            BGFX_TEXTURE_COMPUTE_WRITE |
            BGFX_SAMPLER_U_CLAMP |
            BGFX_SAMPLER_V_CLAMP |
            BGFX_SAMPLER_MIN_POINT |
            BGFX_SAMPLER_MAG_POINT;

         std::vector<uint16_t> halfData(texelCount * 4u, 0u);
         const bgfx::Memory* mem = bgfx::copy(
            halfData.data(),
            static_cast<uint32_t>(halfData.size() * sizeof(uint16_t)));
         
         terrain.DeformationTexture = bgfx::createTexture2D(
            static_cast<uint16_t>(res),
            static_cast<uint16_t>(res),
            false, // no mipmaps
            1,     // single layer
            bgfx::TextureFormat::RGBA16F,
            flags,
            mem);
         }
      }
   
   void DestroyDeformation(TerrainComponent& terrain)
      {
      if (bgfx::isValid(terrain.DeformationTexture))
         {
         bgfx::destroy(terrain.DeformationTexture);
         terrain.DeformationTexture = BGFX_INVALID_HANDLE;
         }
      terrain.DeformationData.clear();
      terrain.DeformationDirty = false;
      terrain.DeformationTime = 0.0f;
      }
   
   void UpdateDeformation(TerrainComponent& terrain, float deltaTime)
      {
      if (!terrain.DeformationConfig.Enabled || terrain.DeformationData.empty())
         return;
      
      terrain.DeformationTime += deltaTime;
      const float decayRate = terrain.DeformationConfig.DecayRate;
      const float decayAmount = decayRate * deltaTime;
      
      // Apply decay to all deformation values
      bool anyActive = false;
      for (glm::vec4& texel : terrain.DeformationData)
         {
         // texel.xy = direction * strength
         // texel.z = unused
         // texel.w = remaining strength before full decay
         
         const float currentStrength = glm::length(glm::vec2(texel.x, texel.y));
         if (currentStrength > 0.001f)
            {
            anyActive = true;
            const float newStrength = std::max(0.0f, currentStrength - decayAmount);
            if (newStrength > 0.001f)
               {
               // Normalize and rescale
               const float scale = newStrength / currentStrength;
               texel.x *= scale;
               texel.y *= scale;
               }
            else
               {
               texel.x = 0.0f;
               texel.y = 0.0f;
               }
            terrain.DeformationDirty = true;
            }
         }
      
      // Upload texture if dirty. Recreate instead of updateTexture2D because the
      // deformation texture is compute-capable and bgfx warns on immutable updates.
      if (terrain.DeformationDirty)
         {
         const uint32_t res = terrain.DeformationConfig.TextureResolution;
         
         // Convert glm::vec4 to half-float for RGBA16F
         // We'll use bgfx::copy with proper data format
         std::vector<uint16_t> halfData(res * res * 4);
         for (size_t i = 0; i < terrain.DeformationData.size(); ++i)
            {
            const glm::vec4& v = terrain.DeformationData[i];
            halfData[i * 4 + 0] = glm::packHalf1x16(v.x);
            halfData[i * 4 + 1] = glm::packHalf1x16(v.y);
            halfData[i * 4 + 2] = glm::packHalf1x16(v.z);
            halfData[i * 4 + 3] = glm::packHalf1x16(v.w);
            }
         
         const bgfx::Memory* mem = bgfx::copy(halfData.data(), static_cast<uint32_t>(halfData.size() * sizeof(uint16_t)));
         const uint64_t flags =
            BGFX_TEXTURE_COMPUTE_WRITE |
            BGFX_SAMPLER_U_CLAMP |
            BGFX_SAMPLER_V_CLAMP |
            BGFX_SAMPLER_MIN_POINT |
            BGFX_SAMPLER_MAG_POINT;

         if (bgfx::isValid(terrain.DeformationTexture))
            {
            bgfx::destroy(terrain.DeformationTexture);
            terrain.DeformationTexture = BGFX_INVALID_HANDLE;
            }
         terrain.DeformationTexture = bgfx::createTexture2D(
            static_cast<uint16_t>(res),
            static_cast<uint16_t>(res),
            false,
            1,
            bgfx::TextureFormat::RGBA16F,
            flags,
            mem);
         terrain.DeformationDirty = !bgfx::isValid(terrain.DeformationTexture);
         }
      }
   
   void ApplyDeformer(TerrainComponent& terrain,
                      const glm::mat4& terrainWorldMatrix,
                      const glm::vec3& deformerWorldPos,
                      const glm::vec3& velocity,
                      float radius,
                      float strength)
      {
      if (!terrain.DeformationConfig.Enabled || terrain.DeformationData.empty())
         return;
      
      // Initialize deformation if needed
      if (terrain.DeformationData.empty())
         {
         InitializeDeformation(terrain);
         if (terrain.DeformationData.empty())
            return;
         }
      
      // Transform deformer position to terrain local space
      const glm::mat4 invWorld = glm::inverse(terrainWorldMatrix);
      const glm::vec3 localPos = glm::vec3(invWorld * glm::vec4(deformerWorldPos, 1.0f));
      
      // Convert to UV coordinates (0-1 range within terrain)
      const float u = localPos.x / terrain.WorldSize.x;
      const float v = localPos.z / terrain.WorldSize.y;
      
      // Skip if outside terrain bounds
      if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
         return;
      
      const uint32_t res = terrain.DeformationConfig.TextureResolution;
      const float texelSizeWorld = terrain.WorldSize.x / static_cast<float>(res);
      const float radiusTexels = radius / texelSizeWorld;
      
      // Calculate affected texel range
      const int centerX = static_cast<int>(u * res);
      const int centerY = static_cast<int>(v * res);
      const int radiusInt = static_cast<int>(std::ceil(radiusTexels)) + 1;
      
      const int minX = std::max(0, centerX - radiusInt);
      const int maxX = std::min(static_cast<int>(res) - 1, centerX + radiusInt);
      const int minY = std::max(0, centerY - radiusInt);
      const int maxY = std::min(static_cast<int>(res) - 1, centerY + radiusInt);
      
      // Calculate deformation direction from velocity (or radial if no velocity)
      const float velLength = glm::length(glm::vec2(velocity.x, velocity.z));
      const bool useVelocityDir = velLength > 0.1f;
      
      // Transform velocity to local space for direction
      const glm::vec3 localVel = glm::vec3(invWorld * glm::vec4(velocity, 0.0f));
      const glm::vec2 velDir2D = useVelocityDir 
         ? glm::normalize(glm::vec2(localVel.x, localVel.z))
         : glm::vec2(0.0f);
      
      const float maxStrength = terrain.DeformationConfig.MaxDeformation * strength;
      
      // Apply deformation to affected texels
      for (int y = minY; y <= maxY; ++y)
         {
         for (int x = minX; x <= maxX; ++x)
            {
            // Calculate texel world position
            const float texelU = (static_cast<float>(x) + 0.5f) / static_cast<float>(res);
            const float texelV = (static_cast<float>(y) + 0.5f) / static_cast<float>(res);
            const glm::vec2 texelWorld(texelU * terrain.WorldSize.x, texelV * terrain.WorldSize.y);
            const glm::vec2 deformerLocal(localPos.x, localPos.z);
            
            // Distance from deformer center
            const float dist = glm::distance(texelWorld, deformerLocal);
            if (dist > radius)
               continue;
            
            // Falloff based on distance (smooth quadratic falloff)
            const float t = dist / radius;
            const float falloff = 1.0f - t * t;
            
            // Direction: either from velocity or radially outward
            glm::vec2 dir;
            if (useVelocityDir)
               {
               dir = velDir2D;
               }
            else
               {
               // Radial direction from center
               const glm::vec2 toTexel = texelWorld - deformerLocal;
               if (glm::length(toTexel) > 0.001f)
                  dir = glm::normalize(toTexel);
               else
                  dir = glm::vec2(1.0f, 0.0f);
               }
            
            // Calculate final deformation
            const glm::vec2 deformation = dir * maxStrength * falloff;
            
            // Update texel (take maximum deformation)
            const size_t idx = static_cast<size_t>(y) * res + x;
            glm::vec4& texel = terrain.DeformationData[idx];
            const glm::vec2 existing(texel.x, texel.y);
            const float existingStrength = glm::length(existing);
            const float newStrength = glm::length(deformation);
            
            if (newStrength > existingStrength)
               {
               texel.x = deformation.x;
               texel.y = deformation.y;
               terrain.DeformationDirty = true;
               }
            }
         }
      }
   
   bgfx::TextureHandle GetDeformationTexture(const TerrainComponent& terrain)
      {
      return terrain.DeformationTexture;
      }

   void UpdateDeformationSystem(Scene& scene, float deltaTime)
      {
      // Collect all terrain entities and all deformer entities
      struct TerrainInfo
         {
         EntityData* Data;
         TerrainComponent* Terrain;
         glm::mat4 WorldMatrix;
         };
      struct DeformerInfo
         {
         EntityData* Data;
         GrassDeformerComponent* Deformer;
         glm::vec3 WorldPosition;
         };

      std::vector<TerrainInfo> terrains;
      std::vector<DeformerInfo> deformers;

      // Iterate all entities in the scene
      for (const Entity& entity : scene.GetEntities())
         {
         EntityData* data = scene.GetEntityData(entity.GetID());
         if (!data)
            continue;

         // Collect terrain entities
         if (data->Terrain)
            {
            TerrainInfo info;
            info.Data = data;
            info.Terrain = data->Terrain.get();
            info.WorldMatrix = data->Transform.WorldMatrix;
            
            // Initialize deformation system if not already done
            if (info.Terrain->DeformationConfig.Enabled && info.Terrain->DeformationData.empty())
               {
               InitializeDeformation(*info.Terrain);
               }
            
            terrains.push_back(info);
            }

         // Collect grass deformer entities
         if (data->GrassDeformer && data->GrassDeformer->Enabled)
            {
            DeformerInfo info;
            info.Data = data;
            info.Deformer = data->GrassDeformer.get();
            info.WorldPosition = glm::vec3(data->Transform.WorldMatrix[3]) + 
                                 glm::vec3(0.0f, info.Deformer->HeightOffset, 0.0f);
            deformers.push_back(info);
            }
         }

      // Update deformer velocities
      for (DeformerInfo& deformerInfo : deformers)
         {
         GrassDeformerComponent* deformer = deformerInfo.Deformer;
         const glm::vec3 currentPos = deformerInfo.WorldPosition;
         
         // Calculate velocity if tracking is enabled
         if (deformer->UseVelocity && deltaTime > 0.0f)
            {
            deformer->Velocity = (currentPos - deformer->LastPosition) / deltaTime;
            }
         else
            {
            deformer->Velocity = glm::vec3(0.0f);
            }
         deformer->LastPosition = currentPos;
         }

      // Apply deformers to terrains
      for (TerrainInfo& terrainInfo : terrains)
         {
         TerrainComponent* terrain = terrainInfo.Terrain;
         if (!terrain->DeformationConfig.Enabled)
            continue;

         // Calculate terrain bounds in world space
         const glm::vec3 terrainOrigin = glm::vec3(terrainInfo.WorldMatrix[3]);
         const glm::vec3 terrainMax = terrainOrigin + glm::vec3(terrain->WorldSize.x, terrain->MaxHeight, terrain->WorldSize.y);

         // Apply each deformer to this terrain
         for (const DeformerInfo& deformerInfo : deformers)
            {
            const GrassDeformerComponent* deformer = deformerInfo.Deformer;
            const glm::vec3& pos = deformerInfo.WorldPosition;

            // Quick AABB check: skip if deformer is too far from terrain
            const float radius = deformer->Radius;
            if (pos.x + radius < terrainOrigin.x || pos.x - radius > terrainMax.x ||
                pos.z + radius < terrainOrigin.z || pos.z - radius > terrainMax.z)
               {
               continue;
               }

            // Apply deformation
            ApplyDeformer(*terrain, terrainInfo.WorldMatrix, pos, deformer->Velocity, deformer->Radius, deformer->Strength);
            }

         // Update decay and upload texture
         UpdateDeformation(*terrain, deltaTime);
         }
      }

   } // namespace TerrainGrass


