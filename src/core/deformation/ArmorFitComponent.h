#pragma once
#include <vector>
#include <memory>
#include <string>
#include <bgfx/bgfx.h>
#include <glm/glm.hpp>
#include "ArmorWrapTypes.h"
#include "core/ecs/Entity.h"

namespace cm { namespace deformation {

// ============================================================================
// ArmorWrapData - Runtime wrap binding loaded from .wrapbin
// ============================================================================
// Holds the array of ArmorWrapInfluence structs for a single armor mesh.
// This is loaded once at asset load time and shared across instances.
// ============================================================================

struct ArmorWrapData
{
    std::vector<ArmorWrapInfluence> Influences;
    
    // Asset path this was loaded from (for debugging/reloading)
    std::string SourcePath;
    
    // Quick validation
    bool IsValid() const { return !Influences.empty(); }
    size_t VertexCount() const { return Influences.size(); }
};

// ============================================================================
// ArmorFitComponent - Per-entity armor wrap fitting configuration
// ============================================================================
// Attach this component to an armor mesh entity to enable wrap deformation.
// The component pairs the armor with a body entity and controls the wrap blend.
// ============================================================================

struct ArmorFitComponent
{
    // The entity containing the body mesh (must have MeshComponent + optionally SkinningComponent)
    EntityID BodyEntity = INVALID_ENTITY_ID;
    
    // Global wrap weight multiplier (0.0 = no wrap, 1.0 = full wrap)
    // This blends between the armor's skinned position and the wrapped position.
    float GlobalWrapWeight = 1.0f;
    
    // Optional: per-bone weight overrides (bone index -> weight scale)
    // If empty, all bones use GlobalWrapWeight
    std::vector<float> BoneWeightOverrides;
    
    // Wrap data loaded from .wrapbin (shared_ptr allows asset sharing)
    std::shared_ptr<ArmorWrapData> WrapData;
    
    // Path to the .wrapbin file (for serialization/reloading)
    std::string WrapBinPath;
    
    // ========================================================================
    // GPU Resources (runtime-only, NOT serialized)
    // ========================================================================
    
    // SSBO containing the wrap influence buffer (uploaded from WrapData)
    bgfx::DynamicVertexBufferHandle WrapInfluenceBuffer = BGFX_INVALID_HANDLE;
    
    // SSBO for output wrapped positions
    bgfx::DynamicVertexBufferHandle WrapOutputBuffer = BGFX_INVALID_HANDLE;
    
    // Cache to avoid redundant GPU uploads
    bool GpuBuffersDirty = true;
    uint32_t CachedVertexCount = 0;
    
    // ========================================================================
    // Runtime State
    // ========================================================================
    
    // If true, use GPU compute shader for wrap deformation
    // If false (or compute unavailable), use CPU fallback
    bool UseGPU = true;
    
    // Validation state - set once after wrap data is loaded and validated
    // Avoids per-frame validation overhead
    bool WrapDataValidated = false;
    
    // Cached pointer to body mesh final positions (updated each frame)
    // This is set by the wrap system during update
    const glm::vec3* BodyFinalPositions = nullptr;
    const uint32_t* BodyIndexBuffer = nullptr;
    uint32_t BodyVertexCount = 0;
    uint32_t BodyIndexCount = 0;
    
    // ========================================================================
    // Lifecycle
    // ========================================================================
    
    ArmorFitComponent() = default;
    ~ArmorFitComponent() { ReleaseGpuResources(); }
    
    // Prevent copy to avoid double-destroying GPU handles
    ArmorFitComponent(const ArmorFitComponent&) = delete;
    ArmorFitComponent& operator=(const ArmorFitComponent&) = delete;
    
    ArmorFitComponent(ArmorFitComponent&& other) noexcept
        : BodyEntity(other.BodyEntity)
        , GlobalWrapWeight(other.GlobalWrapWeight)
        , BoneWeightOverrides(std::move(other.BoneWeightOverrides))
        , WrapData(std::move(other.WrapData))
        , WrapBinPath(std::move(other.WrapBinPath))
        , WrapInfluenceBuffer(other.WrapInfluenceBuffer)
        , WrapOutputBuffer(other.WrapOutputBuffer)
        , GpuBuffersDirty(other.GpuBuffersDirty)
        , CachedVertexCount(other.CachedVertexCount)
        , UseGPU(other.UseGPU)
        , WrapDataValidated(other.WrapDataValidated)
        , BodyFinalPositions(other.BodyFinalPositions)
        , BodyIndexBuffer(other.BodyIndexBuffer)
        , BodyVertexCount(other.BodyVertexCount)
        , BodyIndexCount(other.BodyIndexCount)
    {
        other.WrapInfluenceBuffer = BGFX_INVALID_HANDLE;
        other.WrapOutputBuffer = BGFX_INVALID_HANDLE;
    }
    
    ArmorFitComponent& operator=(ArmorFitComponent&& other) noexcept
    {
        if (this != &other)
        {
            ReleaseGpuResources();
            BodyEntity = other.BodyEntity;
            GlobalWrapWeight = other.GlobalWrapWeight;
            BoneWeightOverrides = std::move(other.BoneWeightOverrides);
            WrapData = std::move(other.WrapData);
            WrapBinPath = std::move(other.WrapBinPath);
            WrapInfluenceBuffer = other.WrapInfluenceBuffer;
            WrapOutputBuffer = other.WrapOutputBuffer;
            GpuBuffersDirty = other.GpuBuffersDirty;
            CachedVertexCount = other.CachedVertexCount;
            UseGPU = other.UseGPU;
            WrapDataValidated = other.WrapDataValidated;
            BodyFinalPositions = other.BodyFinalPositions;
            BodyIndexBuffer = other.BodyIndexBuffer;
            BodyVertexCount = other.BodyVertexCount;
            BodyIndexCount = other.BodyIndexCount;
            other.WrapInfluenceBuffer = BGFX_INVALID_HANDLE;
            other.WrapOutputBuffer = BGFX_INVALID_HANDLE;
        }
        return *this;
    }
    
    void ReleaseGpuResources()
    {
        if (bgfx::isValid(WrapInfluenceBuffer))
        {
            bgfx::destroy(WrapInfluenceBuffer);
            WrapInfluenceBuffer = BGFX_INVALID_HANDLE;
        }
        if (bgfx::isValid(WrapOutputBuffer))
        {
            bgfx::destroy(WrapOutputBuffer);
            WrapOutputBuffer = BGFX_INVALID_HANDLE;
        }
        GpuBuffersDirty = true;
        CachedVertexCount = 0;
    }
    
    // Mark buffers as needing re-upload (e.g., after loading new wrap data)
    void MarkDirty()
    {
        GpuBuffersDirty = true;
    }
};

}} // namespace cm::deformation

