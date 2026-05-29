#include "ArmorWrapSystem.h"
#include "ArmorFitComponent.h"
#include "ArmorWrapTypes.h"
#include "ArmorWrapLoader.h"
#include "core/ecs/Scene.h"
#include "core/ecs/Components.h"
#include "core/ecs/AnimationComponents.h"
#include "core/rendering/VertexTypes.h"
#include "core/rendering/ShaderManager.h"
#include "core/jobs/JobSystem.h"
#include "core/jobs/ParallelFor.h"
#include "core/jobs/Jobs.h"
#include <iostream>
#include <algorithm>

namespace cm { namespace deformation {

// ============================================================================
// Static Resources - Persistent scratch buffers to avoid per-frame allocations
// ============================================================================
static struct ArmorWrapResources
{
    bool Initialized = false;
    bool ComputeAvailable = false;
    bool ForceGpuDisabled = false;
    
    // Compute shader program
    bgfx::ProgramHandle ComputeProgram = BGFX_INVALID_HANDLE;
    
    // Uniforms
    bgfx::UniformHandle u_WrapParams = BGFX_INVALID_HANDLE;
    
    // ========================================================================
    // PERFORMANCE: Scratch buffers persisted across frames
    // These are .clear()'d each frame but retain capacity to avoid reallocation
    // ========================================================================
    struct WorkItem
    {
        EntityID armorId;
        ArmorFitComponent* fit;
        MeshComponent* mesh;
        EntityData* armorData;
        Mesh* armorMeshPtr;
        size_t vertexCount;
    };
    std::vector<WorkItem> ScratchWorkItems;
    
    // Per-mesh output buffer (reused across meshes within a frame)
    std::vector<glm::vec3> ScratchOutputPositions;
    
    // Vertex buffer rebuild scratch (reused)
    std::vector<SkinnedPBRVertex> ScratchSkinnedVerts;
    std::vector<PBRVertex> ScratchPBRVerts;
    
} s_Resources;

// ============================================================================
// ArmorWrapSystem::Initialize
// ============================================================================
void ArmorWrapSystem::Initialize()
{
    if (s_Resources.Initialized)
        return;
    
    // Check if compute shaders are supported
    const bgfx::Caps* caps = bgfx::getCaps();
    s_Resources.ComputeAvailable = (caps->supported & BGFX_CAPS_COMPUTE) != 0;
    
    if (s_Resources.ComputeAvailable)
    {
        // Load compute shader
        bgfx::ShaderHandle cs = ShaderManager::Instance().LoadShader("cs_armor_wrap", ShaderType::Compute);
        if (bgfx::isValid(cs))
        {
            s_Resources.ComputeProgram = bgfx::createProgram(cs, true);
        }
        else
        {
            std::cerr << "[ArmorWrapSystem] Compute shader not found, using CPU fallback" << std::endl;
            s_Resources.ComputeAvailable = false;
        }
    }
    
    // Create uniforms
    s_Resources.u_WrapParams = bgfx::createUniform("u_WrapParams", bgfx::UniformType::Vec4);
    
    // Pre-allocate scratch buffers with reasonable initial capacity
    s_Resources.ScratchWorkItems.reserve(16);
    s_Resources.ScratchOutputPositions.reserve(65536);
    s_Resources.ScratchSkinnedVerts.reserve(65536);
    s_Resources.ScratchPBRVerts.reserve(65536);
    
    s_Resources.Initialized = true;
    
    std::cout << "[ArmorWrapSystem] Initialized (compute=" 
              << (s_Resources.ComputeAvailable ? "yes" : "no") << ")" << std::endl;
}

// ============================================================================
// ArmorWrapSystem::Shutdown
// ============================================================================
void ArmorWrapSystem::Shutdown()
{
    if (!s_Resources.Initialized)
        return;
    
    if (bgfx::isValid(s_Resources.ComputeProgram))
    {
        bgfx::destroy(s_Resources.ComputeProgram);
        s_Resources.ComputeProgram = BGFX_INVALID_HANDLE;
    }
    
    if (bgfx::isValid(s_Resources.u_WrapParams))
    {
        bgfx::destroy(s_Resources.u_WrapParams);
        s_Resources.u_WrapParams = BGFX_INVALID_HANDLE;
    }
    
    // Clear scratch buffers (release memory)
    s_Resources.ScratchWorkItems.clear();
    s_Resources.ScratchWorkItems.shrink_to_fit();
    s_Resources.ScratchOutputPositions.clear();
    s_Resources.ScratchOutputPositions.shrink_to_fit();
    s_Resources.ScratchSkinnedVerts.clear();
    s_Resources.ScratchSkinnedVerts.shrink_to_fit();
    s_Resources.ScratchPBRVerts.clear();
    s_Resources.ScratchPBRVerts.shrink_to_fit();
    
    s_Resources.Initialized = false;
    std::cout << "[ArmorWrapSystem] Shutdown" << std::endl;
}

// ============================================================================
// ArmorWrapSystem::IsComputeAvailable
// ============================================================================
bool ArmorWrapSystem::IsComputeAvailable()
{
    return s_Resources.ComputeAvailable && !s_Resources.ForceGpuDisabled;
}

// ============================================================================
// ArmorWrapSystem::SetForceGpuDisabled
// ============================================================================
void ArmorWrapSystem::SetForceGpuDisabled(bool disabled)
{
    s_Resources.ForceGpuDisabled = disabled;
}

// ============================================================================
// ArmorWrapSystem::Update
// ============================================================================
void ArmorWrapSystem::Update(Scene& scene)
{
    if (!s_Resources.Initialized)
    {
        Initialize();
    }
    
    // Choose CPU or GPU path based on availability and preference
    if (IsComputeAvailable())
    {
        UpdateGPU(scene);
    }
    else
    {
        UpdateCPU(scene);
    }
}

// ============================================================================
// CPU Deformation Kernel - Optimized for cache efficiency
// ============================================================================
struct WrapKernelArgs
{
    const ArmorWrapInfluence* wrap;
    const glm::vec3* bodyPos;
    const uint32_t* bodyIdx;
    const glm::vec3* armorSkinned;
    glm::vec3* armorOut;
    float globalWeight;
    uint32_t startVertex;
    uint32_t count;
};

static void WrapKernel(const WrapKernelArgs& args)
{
    const ArmorWrapInfluence* __restrict wrap = args.wrap;
    const glm::vec3* __restrict bodyPos = args.bodyPos;
    const uint32_t* __restrict bodyIdx = args.bodyIdx;
    const glm::vec3* __restrict armorSkinned = args.armorSkinned;
    glm::vec3* __restrict armorOut = args.armorOut;
    const float globalWeight = args.globalWeight;
    
    const uint32_t end = args.startVertex + args.count;
    for (uint32_t vi = args.startVertex; vi < end; ++vi)
    {
        const ArmorWrapInfluence& w = wrap[vi];
        
        // Skip if no wrap (fast path)
        if (w.flags & WrapFlags::NoWrap)
        {
            armorOut[vi] = armorSkinned[vi];
            continue;
        }
        
        // Get body triangle indices (single cache line for sequential triangles)
        const uint32_t triBase = w.triIndex * 3;
        const uint32_t i0 = bodyIdx[triBase + 0];
        const uint32_t i1 = bodyIdx[triBase + 1];
        const uint32_t i2 = bodyIdx[triBase + 2];
        
        // Load body triangle vertex positions
        const glm::vec3 a = bodyPos[i0];
        const glm::vec3 b = bodyPos[i1];
        const glm::vec3 c = bodyPos[i2];
        
        // Dequantize barycentric weights (constant folding friendly)
        constexpr float kInv65535 = 1.0f / 65535.0f;
        const float fw0 = static_cast<float>(w.w0) * kInv65535;
        const float fw1 = static_cast<float>(w.w1) * kInv65535;
        const float fw2 = 1.0f - fw0 - fw1;
        
        // Compute wrapped position on body surface
        const glm::vec3 wrapped = a * fw0 + b * fw1 + c * fw2;
        
        // Handle rigid flag (skip blend)
        if (w.flags & WrapFlags::Rigid)
        {
            armorOut[vi] = wrapped;
        }
        else
        {
            // Compute final blend weight and lerp
            const float perVertexWeight = static_cast<float>(w.wrapWeight) * kInv65535;
            const float totalWeight = globalWeight * perVertexWeight;
            armorOut[vi] = armorSkinned[vi] + (wrapped - armorSkinned[vi]) * totalWeight;
        }
    }
}

// ============================================================================
// ArmorWrapSystem::UpdateCPU - Optimized to minimize allocations
// ============================================================================
void ArmorWrapSystem::UpdateCPU(Scene& scene)
{
    auto& entities = scene.GetEntities();
    
    // Reuse scratch work items vector (clear but retain capacity)
    auto& workItems = s_Resources.ScratchWorkItems;
    workItems.clear();
    
    // Collect all entities with valid ArmorFitComponent
    for (const auto& ent : entities)
    {
        EntityData* data = scene.GetEntityData(ent.GetID());
        if (!data || !data->ArmorFit || !data->Mesh)
            continue;
        
        ArmorFitComponent* fit = data->ArmorFit.get();
        if (!fit->WrapData || !fit->WrapData->IsValid())
            continue;
        
        // Skip if wrap weight is zero (early out)
        if (fit->GlobalWrapWeight <= 0.0f)
            continue;
        
        // Validate body entity
        EntityData* bodyData = scene.GetEntityData(fit->BodyEntity);
        if (!bodyData || !bodyData->Mesh || !bodyData->Mesh->mesh)
            continue;
        
        Mesh* bodyMesh = bodyData->Mesh->mesh.get();
        if (bodyMesh->Vertices.empty() || bodyMesh->Indices.empty())
            continue;
        
        Mesh* armorMesh = data->Mesh->mesh.get();
        if (!armorMesh)
            continue;
        
        const size_t armorVertexCount = armorMesh->Vertices.size();
        
        // Validate vertex count matches wrap data (this check is fast - just size comparison)
        if (armorVertexCount != fit->WrapData->VertexCount())
            continue;
        
        // Cache body mesh data pointers (avoid repeated lookups)
        fit->BodyFinalPositions = bodyMesh->Vertices.data();
        fit->BodyIndexBuffer = bodyMesh->Indices.data();
        fit->BodyVertexCount = static_cast<uint32_t>(bodyMesh->Vertices.size());
        fit->BodyIndexCount = static_cast<uint32_t>(bodyMesh->Indices.size());
        
        workItems.push_back({ 
            ent.GetID(), 
            fit, 
            data->Mesh.get(), 
            data, 
            armorMesh,
            armorVertexCount 
        });
    }
    
    if (workItems.empty())
        return;
    
    // Reuse scratch buffers
    auto& outputPositions = s_Resources.ScratchOutputPositions;
    auto& skinnedVerts = s_Resources.ScratchSkinnedVerts;
    auto& pbrVerts = s_Resources.ScratchPBRVerts;
    
    // Process each armor mesh
    for (const auto& work : workItems)
    {
        ArmorFitComponent* fit = work.fit;
        Mesh* armorMesh = work.armorMeshPtr;
        const size_t armorVertexCount = work.vertexCount;
        
        // Resize output buffer (only allocates if capacity insufficient)
        outputPositions.resize(armorVertexCount);
        
        // Parallel wrap deformation with optimal chunk size for cache lines
        // 256 vertices * 12 bytes = 3KB per chunk, fits in L1 cache
        constexpr size_t kChunkSize = 256;
        parallel_for(Jobs(), size_t{0}, armorVertexCount, kChunkSize,
            [&](size_t start, size_t count)
            {
                WrapKernelArgs args{};
                args.wrap = fit->WrapData->Influences.data();
                args.bodyPos = fit->BodyFinalPositions;
                args.bodyIdx = fit->BodyIndexBuffer;
                args.armorSkinned = armorMesh->Vertices.data();
                args.armorOut = outputPositions.data();
                args.globalWeight = fit->GlobalWrapWeight;
                args.startVertex = static_cast<uint32_t>(start);
                args.count = static_cast<uint32_t>(count);
                WrapKernel(args);
            });
        
        // Update mesh vertices in-place
        std::memcpy(armorMesh->Vertices.data(), outputPositions.data(), 
                    armorVertexCount * sizeof(glm::vec3));
        
        // Update GPU vertex buffer if present
        if (bgfx::isValid(armorMesh->dvbh))
        {
            const bool skinned = armorMesh->HasSkinning();
            
            if (skinned)
            {
                skinnedVerts.resize(armorVertexCount);
                
                // Vectorizable loop - compiler can auto-vectorize
                for (size_t vi = 0; vi < armorVertexCount; ++vi)
                {
                    SkinnedPBRVertex& v = skinnedVerts[vi];
                    const glm::vec3& pos = outputPositions[vi];
                    const glm::vec3& nrm = armorMesh->Normals[vi];
                    
                    v.x = pos.x; v.y = pos.y; v.z = pos.z;
                    v.nx = nrm.x; v.ny = nrm.y; v.nz = nrm.z;
                    
                    if (vi < armorMesh->UVs.size())
                    {
                        v.u = armorMesh->UVs[vi].x;
                        v.v = armorMesh->UVs[vi].y;
                    }
                    else
                    {
                        v.u = 0.0f; v.v = 0.0f;
                    }
                    
                    if (vi < armorMesh->BoneIndices.size())
                    {
                        const glm::ivec4& bi = armorMesh->BoneIndices[vi];
                        v.i0 = static_cast<uint8_t>(bi.x);
                        v.i1 = static_cast<uint8_t>(bi.y);
                        v.i2 = static_cast<uint8_t>(bi.z);
                        v.i3 = static_cast<uint8_t>(bi.w);
                    }
                    else
                    {
                        v.i0 = v.i1 = v.i2 = v.i3 = 0;
                    }
                    
                    if (vi < armorMesh->BoneWeights.size())
                    {
                        const glm::vec4& bw = armorMesh->BoneWeights[vi];
                        v.w0 = bw.x; v.w1 = bw.y; v.w2 = bw.z; v.w3 = bw.w;
                    }
                    else
                    {
                        v.w0 = 1.0f; v.w1 = v.w2 = v.w3 = 0.0f;
                    }
                }
                
                const bgfx::Memory* mem = bgfx::copy(skinnedVerts.data(), 
                    static_cast<uint32_t>(armorVertexCount * sizeof(SkinnedPBRVertex)));
                bgfx::update(armorMesh->dvbh, 0, mem);
            }
            else
            {
                pbrVerts.resize(armorVertexCount);
                
                for (size_t vi = 0; vi < armorVertexCount; ++vi)
                {
                    PBRVertex& v = pbrVerts[vi];
                    const glm::vec3& pos = outputPositions[vi];
                    const glm::vec3& nrm = armorMesh->Normals[vi];
                    
                    v.x = pos.x; v.y = pos.y; v.z = pos.z;
                    v.nx = nrm.x; v.ny = nrm.y; v.nz = nrm.z;
                    
                    if (vi < armorMesh->UVs.size())
                    {
                        v.u = armorMesh->UVs[vi].x;
                        v.v = armorMesh->UVs[vi].y;
                    }
                    else
                    {
                        v.u = 0.0f; v.v = 0.0f;
                    }
                }
                
                const bgfx::Memory* mem = bgfx::copy(pbrVerts.data(),
                    static_cast<uint32_t>(armorVertexCount * sizeof(PBRVertex)));
                bgfx::update(armorMesh->dvbh, 0, mem);
            }
        }
    }
}

// ============================================================================
// ArmorWrapSystem::UpdateGPU - TODO: Implement full GPU path
// ============================================================================
void ArmorWrapSystem::UpdateGPU(Scene& scene)
{
    // GPU path placeholder - currently falls back to optimized CPU path
    // 
    // Full GPU implementation would:
    // 1. Upload body positions to compute SSBO (once per body per frame)
    // 2. Upload body indices to SSBO (once at load time, static)
    // 3. Upload wrap influences to SSBO (once at load time, static)
    // 4. Armor skinned positions already in GPU from skinning pass
    // 5. Dispatch compute shader: ceil(vertexCount / 64) workgroups
    // 6. Use output SSBO directly for rendering (no readback)
    //
    // Benefits:
    // - No CPU->GPU transfer for armor positions
    // - Parallel processing of thousands of vertices
    // - Can chain with skinning compute if available
    
    UpdateCPU(scene);
}

}} // namespace cm::deformation
