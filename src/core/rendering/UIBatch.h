#pragma once
#include <bgfx/bgfx.h>
#include <vector>
#include "VertexTypes.h"

// Batched UI Rendering System for Claymore Engine
// Eliminates per-frame buffer creation/destruction for massive performance gains
// Target: Sub-millisecond UI rendering at 60fps

namespace cm {
namespace rendering {

// Batched UI vertex/index buffer manager
// Uses dynamic buffers that are updated each frame instead of created/destroyed
class UIBatch {
public:
    static constexpr uint32_t kInitialVertexCapacity = 4096;
    static constexpr uint32_t kInitialIndexCapacity = 8192;
    static constexpr uint32_t kMaxVertices = 65536;
    static constexpr uint32_t kMaxIndices = 131072;

    UIBatch() = default;
    ~UIBatch() { Shutdown(); }

    // Initialize/shutdown GPU resources
    void Init() {
        if (!m_initialized) {
            m_vertices.reserve(kInitialVertexCapacity);
            m_indices.reserve(kInitialIndexCapacity);
            m_vertexCapacity = kInitialVertexCapacity;
            m_indexCapacity = kInitialIndexCapacity;
            
            // Create initial dynamic buffers
            CreateBuffers(m_vertexCapacity, m_indexCapacity);
            m_initialized = true;
        }
    }

    void Shutdown() {
        if (bgfx::isValid(m_dvbh)) {
            bgfx::destroy(m_dvbh);
            m_dvbh = BGFX_INVALID_HANDLE;
        }
        if (bgfx::isValid(m_dibh)) {
            bgfx::destroy(m_dibh);
            m_dibh = BGFX_INVALID_HANDLE;
        }
        m_vertices.clear();
        m_indices.clear();
        m_initialized = false;
    }

    // Begin a new frame batch
    void Begin() {
        m_vertices.clear();
        m_indices.clear();
        m_drawCalls.clear();
        m_currentVertexOffset = 0;
    }

    // Add a quad to the batch
    void AddQuad(float x0, float y0, float x1, float y1,
                 float u0, float v0, float u1, float v1,
                 uint32_t abgr,
                 bgfx::TextureHandle texture = BGFX_INVALID_HANDLE) {
        
        if (m_vertices.size() + 4 > kMaxVertices) return;
        if (m_indices.size() + 6 > kMaxIndices) return;

        uint16_t baseVertex = static_cast<uint16_t>(m_vertices.size());

        m_vertices.push_back({ x0, y0, 0.0f, u0, v0, abgr });
        m_vertices.push_back({ x1, y0, 0.0f, u1, v0, abgr });
        m_vertices.push_back({ x1, y1, 0.0f, u1, v1, abgr });
        m_vertices.push_back({ x0, y1, 0.0f, u0, v1, abgr });

        m_indices.push_back(baseVertex + 0);
        m_indices.push_back(baseVertex + 1);
        m_indices.push_back(baseVertex + 2);
        m_indices.push_back(baseVertex + 0);
        m_indices.push_back(baseVertex + 2);
        m_indices.push_back(baseVertex + 3);

        // Record draw call for this quad
        if (m_drawCalls.empty() || m_drawCalls.back().texture.idx != texture.idx) {
            DrawCall dc;
            dc.texture = texture;
            dc.indexStart = static_cast<uint32_t>(m_indices.size() - 6);
            dc.indexCount = 6;
            m_drawCalls.push_back(dc);
        } else {
            // Merge with previous draw call
            m_drawCalls.back().indexCount += 6;
        }
    }

    // Add 9-slice quad efficiently (9 quads in one call)
    void AddNineSlice(float L, float T, float R, float B,
                      float lpx, float rpx, float tpx, float bpx,
                      float uL, float vT, float uR, float vB,
                      float uL2, float vT2, float uR2, float vB2,
                      uint32_t abgr,
                      bgfx::TextureHandle texture) {
        
        float xL = L;
        float xM = L + lpx;
        float xR = R - rpx;
        float yT = T;
        float yM = T + tpx;
        float yB = B - bpx;

        // 9 quads: top-left, top-center, top-right, mid-left, mid-center, mid-right, bot-left, bot-center, bot-right
        AddQuad(xL, yT, xM, yM, uL,  vT,  uL2, vT2, abgr, texture);
        AddQuad(xM, yT, xR, yM, uL2, vT,  uR2, vT2, abgr, texture);
        AddQuad(xR, yT, R,  yM, uR2, vT,  uR,  vT2, abgr, texture);
        AddQuad(xL, yM, xM, yB, uL,  vT2, uL2, vB2, abgr, texture);
        AddQuad(xM, yM, xR, yB, uL2, vT2, uR2, vB2, abgr, texture);
        AddQuad(xR, yM, R,  yB, uR2, vT2, uR,  vB2, abgr, texture);
        AddQuad(xL, yB, xM, B,  uL,  vB2, uL2, vB,  abgr, texture);
        AddQuad(xM, yB, xR, B,  uL2, vB2, uR2, vB,  abgr, texture);
        AddQuad(xR, yB, R,  B,  uR2, vB2, uR,  vB,  abgr, texture);
    }

    // End batch and upload to GPU
    void End() {
        if (m_vertices.empty()) return;

        // Grow buffers if needed
        if (m_vertices.size() > m_vertexCapacity || m_indices.size() > m_indexCapacity) {
            uint32_t newVertexCap = std::max(static_cast<uint32_t>(m_vertices.size() * 1.5f), m_vertexCapacity);
            uint32_t newIndexCap = std::max(static_cast<uint32_t>(m_indices.size() * 1.5f), m_indexCapacity);
            newVertexCap = std::min(newVertexCap, kMaxVertices);
            newIndexCap = std::min(newIndexCap, kMaxIndices);
            
            if (newVertexCap > m_vertexCapacity || newIndexCap > m_indexCapacity) {
                if (bgfx::isValid(m_dvbh)) bgfx::destroy(m_dvbh);
                if (bgfx::isValid(m_dibh)) bgfx::destroy(m_dibh);
                m_vertexCapacity = newVertexCap;
                m_indexCapacity = newIndexCap;
                CreateBuffers(m_vertexCapacity, m_indexCapacity);
            }
        }

        // Update buffers
        const bgfx::Memory* vmem = bgfx::copy(m_vertices.data(), 
            static_cast<uint32_t>(m_vertices.size() * sizeof(UIVertex)));
        const bgfx::Memory* imem = bgfx::copy(m_indices.data(), 
            static_cast<uint32_t>(m_indices.size() * sizeof(uint16_t)));
        
        bgfx::update(m_dvbh, 0, vmem);
        bgfx::update(m_dibh, 0, imem);
    }

    // Submit all draw calls
    void Submit(uint16_t viewId, bgfx::ProgramHandle program, bgfx::UniformHandle sampler,
                bgfx::TextureHandle fallbackTexture, const float* orthoTransform) {
        if (m_vertices.empty() || m_drawCalls.empty()) return;
        if (!bgfx::isValid(program) || !bgfx::isValid(sampler)) return;

        for (const auto& dc : m_drawCalls) {
            bgfx::setTransform(orthoTransform);
            bgfx::setVertexBuffer(0, m_dvbh, 0, static_cast<uint32_t>(m_vertices.size()));
            bgfx::setIndexBuffer(m_dibh, dc.indexStart, dc.indexCount);
            
            bgfx::TextureHandle tex = bgfx::isValid(dc.texture) ? dc.texture : fallbackTexture;
            if (!bgfx::isValid(tex)) {
                continue;
            }
            bgfx::setTexture(0, sampler, tex);
            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_ALWAYS | BGFX_STATE_BLEND_ALPHA);
            bgfx::submit(viewId, program);
        }
    }

    // Stats
    size_t GetVertexCount() const { return m_vertices.size(); }
    size_t GetIndexCount() const { return m_indices.size(); }
    size_t GetDrawCallCount() const { return m_drawCalls.size(); }

private:
    struct DrawCall {
        bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
        uint32_t indexStart = 0;
        uint32_t indexCount = 0;
    };

    void CreateBuffers(uint32_t vertexCap, uint32_t indexCap) {
        UIVertex::Init();
        m_dvbh = bgfx::createDynamicVertexBuffer(vertexCap, UIVertex::layout, 
            BGFX_BUFFER_ALLOW_RESIZE);
        m_dibh = bgfx::createDynamicIndexBuffer(indexCap, 
            BGFX_BUFFER_ALLOW_RESIZE | BGFX_BUFFER_INDEX32);
    }

    std::vector<UIVertex> m_vertices;
    std::vector<uint16_t> m_indices;
    std::vector<DrawCall> m_drawCalls;
    
    bgfx::DynamicVertexBufferHandle m_dvbh = BGFX_INVALID_HANDLE;
    bgfx::DynamicIndexBufferHandle m_dibh = BGFX_INVALID_HANDLE;
    
    uint32_t m_vertexCapacity = 0;
    uint32_t m_indexCapacity = 0;
    uint32_t m_currentVertexOffset = 0;
    bool m_initialized = false;
};

} // namespace rendering
} // namespace cm

