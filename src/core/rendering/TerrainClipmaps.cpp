#include "TerrainClipmaps.h"
#include "VertexTypes.h"
#include "core/ecs/Components.h"
#include <algorithm>
#include <cmath>
#include <glm/gtc/type_ptr.hpp>

namespace terrain {

namespace {
    // Simple vertex for clipmap meshes - just position (x,z), UVs computed in shader
    struct ClipmapVertex {
        float x, z;  // Local grid position, centered at (0,0)
        
        static bgfx::VertexLayout layout;
        static void initLayout() {
            layout.begin()
                .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
                .end();
        }
    };
    bgfx::VertexLayout ClipmapVertex::layout;
    static bool s_layoutInitialized = false;
    
    void EnsureLayoutInitialized() {
        if (!s_layoutInitialized) {
            ClipmapVertex::initLayout();
            s_layoutInitialized = true;
        }
    }
}

void TerrainClipmapSystem::Init(const ClipmapConfig& config) {
    if (m_Initialized) {
        Shutdown();
    }
    
    EnsureLayoutInitialized();
    
    m_Config = config;
    m_Config.GridSize = std::max(8u, m_Config.GridSize);
    // Ensure power of 2
    uint32_t pot = 1;
    while (pot < m_Config.GridSize) pot *= 2;
    m_Config.GridSize = pot;
    
    // Initialize levels
    m_Levels.resize(m_Config.LevelCount);
    float scale = m_Config.BaseScale;
    for (uint32_t i = 0; i < m_Config.LevelCount; ++i) {
        m_Levels[i].Scale = scale;
        m_Levels[i].SnapOffset = glm::vec2(0.0f);
        m_Levels[i].PrevSnapOffset = glm::vec2(0.0f);
        m_Levels[i].NeedsUpdate = true;
        scale *= 2.0f;  // Each level is 2x coarser
    }
    
    BuildMeshes();
    m_Initialized = true;
}

void TerrainClipmapSystem::Shutdown() {
    // Destroy all mesh resources
    if (bgfx::isValid(m_Meshes.BlockVB)) bgfx::destroy(m_Meshes.BlockVB);
    if (bgfx::isValid(m_Meshes.BlockIB)) bgfx::destroy(m_Meshes.BlockIB);
    if (bgfx::isValid(m_Meshes.RingVB)) bgfx::destroy(m_Meshes.RingVB);
    if (bgfx::isValid(m_Meshes.RingIB)) bgfx::destroy(m_Meshes.RingIB);
    if (bgfx::isValid(m_Meshes.TrimHorizVB)) bgfx::destroy(m_Meshes.TrimHorizVB);
    if (bgfx::isValid(m_Meshes.TrimHorizIB)) bgfx::destroy(m_Meshes.TrimHorizIB);
    if (bgfx::isValid(m_Meshes.TrimVertVB)) bgfx::destroy(m_Meshes.TrimVertVB);
    if (bgfx::isValid(m_Meshes.TrimVertIB)) bgfx::destroy(m_Meshes.TrimVertIB);
    if (bgfx::isValid(m_Meshes.CrossVB)) bgfx::destroy(m_Meshes.CrossVB);
    if (bgfx::isValid(m_Meshes.CrossIB)) bgfx::destroy(m_Meshes.CrossIB);
    if (bgfx::isValid(m_Meshes.CenterVB)) bgfx::destroy(m_Meshes.CenterVB);
    if (bgfx::isValid(m_Meshes.CenterIB)) bgfx::destroy(m_Meshes.CenterIB);
    
    m_Meshes = ClipmapMeshes{};
    m_Levels.clear();
    m_Initialized = false;
}

bool TerrainClipmapSystem::Update(const glm::vec3& cameraPos, const glm::vec2& terrainWorldSize) {
    if (!m_Initialized || m_Levels.empty()) return false;
    
    bool anyUpdated = false;
    
    // CRITICAL FIX: All levels must share the SAME center position so the nested
    // ring meshes align properly. The center is snapped to the coarsest level's grid
    // to ensure all levels can align (finer grids are subsets of coarser grids).
    //
    // The coarsest level (highest index) has the largest snap size. By snapping
    // to that grid, we ensure all levels are properly centered together.
    
    // Calculate the snap size based on the coarsest level
    const float coarsestScale = m_Levels.back().Scale;
    const float snapSize = coarsestScale * 2.0f;  // Snap to every 2 vertices at coarsest scale
    
    // Calculate the shared center position (snapped to coarsest grid)
    glm::vec2 camXZ(cameraPos.x, cameraPos.z);
    glm::vec2 sharedCenter;
    sharedCenter.x = std::floor(camXZ.x / snapSize) * snapSize;
    sharedCenter.y = std::floor(camXZ.y / snapSize) * snapSize;
    
    // Clamp to terrain bounds (use coarsest level's extent for margin)
    const float maxExtent = coarsestScale * m_Config.GridSize;
    sharedCenter.x = glm::clamp(sharedCenter.x, maxExtent * 0.5f, terrainWorldSize.x - maxExtent * 0.5f);
    sharedCenter.y = glm::clamp(sharedCenter.y, maxExtent * 0.5f, terrainWorldSize.y - maxExtent * 0.5f);
    
    // Apply the same center to all levels
    for (uint32_t i = 0; i < m_Levels.size(); ++i) {
        ClipmapLevel& level = m_Levels[i];
        
        // Check if position changed
        if (sharedCenter != level.SnapOffset) {
            level.PrevSnapOffset = level.SnapOffset;
            level.SnapOffset = sharedCenter;
            level.NeedsUpdate = true;
            anyUpdated = true;
        } else {
            level.NeedsUpdate = false;
        }
    }
    
    m_LastCameraPos = cameraPos;
    return anyUpdated;
}

void TerrainClipmapSystem::BuildMeshes() {
    m_Meshes.GridSize = m_Config.GridSize;
    
    BuildBlockMesh();
    BuildRingMesh();
    BuildTrimMeshes();
    BuildCenterMesh();
}

void TerrainClipmapSystem::BuildBlockMesh() {
    // Build a simple NxN grid mesh positioned at the origin
    // This is used as the basic building block
    const uint32_t n = m_Config.GridSize;
    const uint32_t vertCount = n * n;
    const uint32_t quadCount = (n - 1) * (n - 1);
    const uint32_t indexCount = quadCount * 6;
    
    std::vector<ClipmapVertex> vertices(vertCount);
    std::vector<uint16_t> indices(indexCount);
    
    // Center at origin
    const float halfSize = static_cast<float>(n - 1) * 0.5f;
    
    // Generate vertices
    for (uint32_t z = 0; z < n; ++z) {
        for (uint32_t x = 0; x < n; ++x) {
            ClipmapVertex& v = vertices[z * n + x];
            v.x = static_cast<float>(x) - halfSize;
            v.z = static_cast<float>(z) - halfSize;
        }
    }
    
    // Generate indices
    uint32_t idx = 0;
    for (uint32_t z = 0; z < n - 1; ++z) {
        for (uint32_t x = 0; x < n - 1; ++x) {
            const uint16_t base = static_cast<uint16_t>(z * n + x);
            indices[idx++] = base;
            indices[idx++] = base + static_cast<uint16_t>(n);
            indices[idx++] = base + 1;
            indices[idx++] = base + 1;
            indices[idx++] = base + static_cast<uint16_t>(n);
            indices[idx++] = base + static_cast<uint16_t>(n) + 1;
        }
    }
    
    const bgfx::Memory* vMem = bgfx::copy(vertices.data(), static_cast<uint32_t>(vertices.size() * sizeof(ClipmapVertex)));
    const bgfx::Memory* iMem = bgfx::copy(indices.data(), static_cast<uint32_t>(indices.size() * sizeof(uint16_t)));
    
    m_Meshes.BlockVB = bgfx::createVertexBuffer(vMem, ClipmapVertex::layout);
    m_Meshes.BlockIB = bgfx::createIndexBuffer(iMem);
    m_Meshes.BlockIndexCount = indexCount;
}

void TerrainClipmapSystem::BuildRingMesh() {
    // Build a hollow ring mesh for outer clipmap levels
    // 
    // The ring uses the same NxN vertex grid as the center mesh, but with the center
    // portion (the "hole") excluded from the triangle indices.
    //
    // The hole is sized so when the ring is rendered at scale S, its hole exactly
    // matches where the center mesh at scale S/2 would be rendered.
    //
    // For proper nesting:
    // - Center mesh: spans local coords from -halfSize to +halfSize (halfSize = (N-1)/2)
    // - At scale 1: covers world -halfSize to +halfSize
    // - Ring at scale 2: hole should cover world -halfSize to +halfSize
    // - Therefore ring's local hole spans from -halfSize/2 to +halfSize/2
    //
    // We use integer vertex indices to define the hole boundary:
    // - holeMin = N/4 (start index of hole in each dimension)
    // - holeMax = 3*N/4 (end index of hole, exclusive)
    // This gives a centered hole that's approximately half the width/height.
    
    const uint32_t n = m_Config.GridSize;
    const float halfSize = static_cast<float>(n - 1) * 0.5f;
    
    // Create full NxN vertex grid (same as center mesh)
    std::vector<ClipmapVertex> vertices(n * n);
    for (uint32_t z = 0; z < n; ++z) {
        for (uint32_t x = 0; x < n; ++x) {
            ClipmapVertex& v = vertices[z * n + x];
            v.x = static_cast<float>(x) - halfSize;
            v.z = static_cast<float>(z) - halfSize;
        }
    }
    
    // Define hole boundaries using integer indices
    // The hole is centered and sized to be approximately half the grid
    // holeStart and holeEnd define the vertex INDEX range of the hole
    const uint32_t holeStart = n / 4;          // e.g., 16 for n=64
    const uint32_t holeEnd = n - n / 4;        // e.g., 48 for n=64
    
    // Generate indices, skipping quads that fall entirely within the hole
    std::vector<uint16_t> indices;
    indices.reserve((n - 1) * (n - 1) * 6);  // Upper bound, will use less
    
    for (uint32_t z = 0; z < n - 1; ++z) {
        for (uint32_t x = 0; x < n - 1; ++x) {
            // Check if this quad's lower-left corner is inside the hole
            // A quad is inside the hole if ALL its vertices are in the hole region
            const bool xInHole = (x >= holeStart && x + 1 < holeEnd);
            const bool zInHole = (z >= holeStart && z + 1 < holeEnd);
            
            if (xInHole && zInHole) {
                // This quad is entirely inside the hole - skip it
                continue;
            }
            
            // Add this quad
            const uint16_t base = static_cast<uint16_t>(z * n + x);
            indices.push_back(base);
            indices.push_back(base + static_cast<uint16_t>(n));
            indices.push_back(base + 1);
            indices.push_back(base + 1);
            indices.push_back(base + static_cast<uint16_t>(n));
            indices.push_back(base + static_cast<uint16_t>(n) + 1);
        }
    }
    
    const bgfx::Memory* vMem = bgfx::copy(vertices.data(), static_cast<uint32_t>(vertices.size() * sizeof(ClipmapVertex)));
    const bgfx::Memory* iMem = bgfx::copy(indices.data(), static_cast<uint32_t>(indices.size() * sizeof(uint16_t)));
    
    m_Meshes.RingVB = bgfx::createVertexBuffer(vMem, ClipmapVertex::layout);
    m_Meshes.RingIB = bgfx::createIndexBuffer(iMem);
    m_Meshes.RingIndexCount = static_cast<uint32_t>(indices.size());
}

void TerrainClipmapSystem::BuildTrimMeshes() {
    // Trim meshes handle the 2:1 vertex ratio at LOD boundaries
    // They're thin strips that stitch different resolution edges together
    
    const uint32_t n = m_Config.GridSize;
    
    // Horizontal trim: n vertices wide, 2 vertices tall with special indexing
    {
        std::vector<ClipmapVertex> vertices;
        std::vector<uint16_t> indices;
        
        // Top row: full resolution
        for (uint32_t x = 0; x <= n; ++x) {
            ClipmapVertex v;
            v.x = static_cast<float>(x);
            v.z = 0.0f;
            vertices.push_back(v);
        }
        // Bottom row: half resolution (every other vertex)
        for (uint32_t x = 0; x <= n; x += 2) {
            ClipmapVertex v;
            v.x = static_cast<float>(x);
            v.z = 1.0f;
            vertices.push_back(v);
        }
        
        // Generate triangles connecting full-res top to half-res bottom
        const uint32_t topCount = n + 1;
        uint32_t topIdx = 0;
        uint32_t botIdx = topCount;
        
        for (uint32_t i = 0; i < n / 2; ++i) {
            // Two top vertices connect to one bottom vertex
            uint16_t t0 = static_cast<uint16_t>(topIdx);
            uint16_t t1 = static_cast<uint16_t>(topIdx + 1);
            uint16_t t2 = static_cast<uint16_t>(topIdx + 2);
            uint16_t b0 = static_cast<uint16_t>(botIdx);
            uint16_t b1 = static_cast<uint16_t>(botIdx + 1);
            
            // First triangle
            indices.push_back(t0);
            indices.push_back(b0);
            indices.push_back(t1);
            // Second triangle
            indices.push_back(t1);
            indices.push_back(b0);
            indices.push_back(b1);
            // Third triangle
            indices.push_back(t1);
            indices.push_back(b1);
            indices.push_back(t2);
            
            topIdx += 2;
            botIdx += 1;
        }
        
        const bgfx::Memory* vMem = bgfx::copy(vertices.data(), static_cast<uint32_t>(vertices.size() * sizeof(ClipmapVertex)));
        const bgfx::Memory* iMem = bgfx::copy(indices.data(), static_cast<uint32_t>(indices.size() * sizeof(uint16_t)));
        
        m_Meshes.TrimHorizVB = bgfx::createVertexBuffer(vMem, ClipmapVertex::layout);
        m_Meshes.TrimHorizIB = bgfx::createIndexBuffer(iMem);
        m_Meshes.TrimHorizIndexCount = static_cast<uint32_t>(indices.size());
    }
    
    // Vertical trim: same but rotated 90 degrees
    {
        std::vector<ClipmapVertex> vertices;
        std::vector<uint16_t> indices;
        
        // Left column: full resolution
        for (uint32_t z = 0; z <= n; ++z) {
            ClipmapVertex v;
            v.x = 0.0f;
            v.z = static_cast<float>(z);
            vertices.push_back(v);
        }
        // Right column: half resolution
        for (uint32_t z = 0; z <= n; z += 2) {
            ClipmapVertex v;
            v.x = 1.0f;
            v.z = static_cast<float>(z);
            vertices.push_back(v);
        }
        
        const uint32_t leftCount = n + 1;
        uint32_t leftIdx = 0;
        uint32_t rightIdx = leftCount;
        
        for (uint32_t i = 0; i < n / 2; ++i) {
            uint16_t l0 = static_cast<uint16_t>(leftIdx);
            uint16_t l1 = static_cast<uint16_t>(leftIdx + 1);
            uint16_t l2 = static_cast<uint16_t>(leftIdx + 2);
            uint16_t r0 = static_cast<uint16_t>(rightIdx);
            uint16_t r1 = static_cast<uint16_t>(rightIdx + 1);
            
            indices.push_back(l0);
            indices.push_back(l1);
            indices.push_back(r0);
            indices.push_back(l1);
            indices.push_back(r1);
            indices.push_back(r0);
            indices.push_back(l1);
            indices.push_back(l2);
            indices.push_back(r1);
            
            leftIdx += 2;
            rightIdx += 1;
        }
        
        const bgfx::Memory* vMem = bgfx::copy(vertices.data(), static_cast<uint32_t>(vertices.size() * sizeof(ClipmapVertex)));
        const bgfx::Memory* iMem = bgfx::copy(indices.data(), static_cast<uint32_t>(indices.size() * sizeof(uint16_t)));
        
        m_Meshes.TrimVertVB = bgfx::createVertexBuffer(vMem, ClipmapVertex::layout);
        m_Meshes.TrimVertIB = bgfx::createIndexBuffer(iMem);
        m_Meshes.TrimVertIndexCount = static_cast<uint32_t>(indices.size());
    }
}

void TerrainClipmapSystem::BuildCenterMesh() {
    // Center mesh fills the innermost area (level 0)
    // It's a standard NxN grid centered at the origin
    
    const uint32_t n = m_Config.GridSize;
    const uint32_t vertCount = n * n;
    const uint32_t quadCount = (n - 1) * (n - 1);
    const uint32_t indexCount = quadCount * 6;
    
    std::vector<ClipmapVertex> vertices(vertCount);
    std::vector<uint16_t> indices(indexCount);
    
    // Center the mesh around origin
    const float halfSize = static_cast<float>(n - 1) * 0.5f;
    
    for (uint32_t z = 0; z < n; ++z) {
        for (uint32_t x = 0; x < n; ++x) {
            ClipmapVertex& v = vertices[z * n + x];
            v.x = static_cast<float>(x) - halfSize;
            v.z = static_cast<float>(z) - halfSize;
        }
    }
    
    uint32_t idx = 0;
    for (uint32_t z = 0; z < n - 1; ++z) {
        for (uint32_t x = 0; x < n - 1; ++x) {
            const uint16_t base = static_cast<uint16_t>(z * n + x);
            indices[idx++] = base;
            indices[idx++] = base + static_cast<uint16_t>(n);
            indices[idx++] = base + 1;
            indices[idx++] = base + 1;
            indices[idx++] = base + static_cast<uint16_t>(n);
            indices[idx++] = base + static_cast<uint16_t>(n) + 1;
        }
    }
    
    const bgfx::Memory* vMem = bgfx::copy(vertices.data(), static_cast<uint32_t>(vertices.size() * sizeof(ClipmapVertex)));
    const bgfx::Memory* iMem = bgfx::copy(indices.data(), static_cast<uint32_t>(indices.size() * sizeof(uint16_t)));
    
    m_Meshes.CenterVB = bgfx::createVertexBuffer(vMem, ClipmapVertex::layout);
    m_Meshes.CenterIB = bgfx::createIndexBuffer(iMem);
    m_Meshes.CenterIndexCount = indexCount;
}

// Render function implementation - renders terrain using clipmaps
void RenderTerrainClipmaps(
    TerrainComponent& terrain,
    TerrainClipmapSystem& clipmaps,
    const glm::mat4& worldMatrix,
    const glm::vec3& cameraPos,
    uint16_t viewId,
    bgfx::ProgramHandle program,
    const ClipmapRenderParams& params)
{
    if (!clipmaps.IsInitialized()) return;
    
    const auto& config = clipmaps.GetConfig();
    const auto& levels = clipmaps.GetLevels();
    const auto& meshes = clipmaps.GetMeshes();
    
    if (levels.empty()) return;
    
    // Set common state
    const uint64_t state = 
        BGFX_STATE_WRITE_RGB |
        BGFX_STATE_WRITE_Z |
        BGFX_STATE_DEPTH_TEST_LESS |
        BGFX_STATE_MSAA |
        BGFX_STATE_CULL_CW;
    
    float transform[16];
    memcpy(transform, glm::value_ptr(worldMatrix), sizeof(float) * 16);
    
    // Bind height and splat textures
    bgfx::setTexture(0, params.s_heightTexture, params.heightTexture);
    bgfx::setTexture(1, params.s_splatTexture, params.splatTexture);
    
    // Height params for the shader
    glm::vec4 heightParams(terrain.MaxHeight, 0.0f, 0.0f, 0.0f);
    bgfx::setUniform(params.u_heightParams, glm::value_ptr(heightParams));
    
    // Render each clipmap level from outer to inner (back to front)
    // This ensures proper depth ordering
    for (int i = static_cast<int>(levels.size()) - 1; i >= 0; --i) {
        const ClipmapLevel& level = levels[i];
        
        // Calculate morph factor for this level (smooth LOD transition)
        float morphFactor = 0.0f;
        if (config.EnableMorphing && i < static_cast<int>(levels.size()) - 1) {
            const float levelExtent = level.Scale * config.GridSize;
            const float morphStart = levelExtent * (1.0f - config.MorphRegion);
            const glm::vec2 camXZ(cameraPos.x, cameraPos.z);
            const float distFromCenter = glm::length(camXZ - level.SnapOffset);
            morphFactor = glm::clamp((distFromCenter - morphStart) / (levelExtent - morphStart), 0.0f, 1.0f);
        }
        
        // Set per-level uniforms
        glm::vec4 clipmapParams(level.Scale, morphFactor, static_cast<float>(config.GridSize), static_cast<float>(i));
        bgfx::setUniform(params.u_clipmapParams, glm::value_ptr(clipmapParams));
        
        glm::vec4 clipmapOffset(level.SnapOffset.x, level.SnapOffset.y, terrain.WorldSize.x, terrain.WorldSize.y);
        bgfx::setUniform(params.u_clipmapOffset, glm::value_ptr(clipmapOffset));
        
        if (i == 0) {
            // Level 0: render full center mesh
            if (bgfx::isValid(meshes.CenterVB) && bgfx::isValid(meshes.CenterIB) && bgfx::isValid(program)) {
                bgfx::setTransform(transform);
                bgfx::setVertexBuffer(0, meshes.CenterVB);
                bgfx::setIndexBuffer(meshes.CenterIB);
                bgfx::setState(state);
                bgfx::submit(viewId, program);
            }
        } else {
            // Outer levels: render ring mesh (hollow center)
            if (bgfx::isValid(meshes.RingVB) && bgfx::isValid(meshes.RingIB) && bgfx::isValid(program)) {
                bgfx::setTransform(transform);
                bgfx::setVertexBuffer(0, meshes.RingVB);
                bgfx::setIndexBuffer(meshes.RingIB);
                bgfx::setState(state);
                bgfx::submit(viewId, program);
            }
        }
    }
}

} // namespace terrain

