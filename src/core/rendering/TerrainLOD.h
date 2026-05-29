#pragma once
#include <bgfx/bgfx.h>
#include <glm/glm.hpp>
#include <vector>
#include <array>

// High-Performance Terrain LOD System
// Implements clipmap-style LOD with GPU-friendly chunk management
// Target: Large terrains (8km+) at 60fps with minimal CPU overhead

namespace cm {
namespace rendering {

// LOD level configuration
struct TerrainLODLevel {
    float maxDistance;      // Max camera distance for this LOD
    uint32_t gridStep;      // Vertex skip factor (1 = full res, 2 = half, 4 = quarter, etc.)
    uint32_t indexOffset;   // Start index in shared index buffer
    uint32_t indexCount;    // Number of indices for this LOD
};

// Chunk for spatial organization and culling
struct TerrainChunkLOD {
    glm::vec2 worldMin;     // XZ min bounds in world space
    glm::vec2 worldMax;     // XZ max bounds in world space  
    float minHeight;        // Y min for AABB culling
    float maxHeight;        // Y max for AABB culling
    uint32_t gridX, gridZ;  // Grid position in chunk array
    int currentLOD;         // Current LOD level (-1 = culled)
    bool visible;           // Frustum visibility
    float distanceToCamera; // For LOD selection
};

// Configuration for terrain LOD system
struct TerrainLODConfig {
    // LOD distances (world units)
    float lodDistances[4] = { 50.0f, 150.0f, 400.0f, 1000.0f };
    
    // Chunk size in vertices (must be power of 2 + 1, e.g., 33, 65, 129)
    uint32_t chunkSize = 65;
    
    // Enable morphing between LODs (reduces popping)
    bool enableMorphing = true;
    
    // Morph region as fraction of LOD distance
    float morphRegion = 0.2f;
    
    // Maximum LOD levels
    static constexpr uint32_t kMaxLODLevels = 4;
};

// Frustum planes for culling
struct Frustum {
    glm::vec4 planes[6]; // Left, Right, Bottom, Top, Near, Far
    
    static Frustum FromViewProj(const glm::mat4& viewProj);
    bool ContainsAABB(const glm::vec3& min, const glm::vec3& max) const;
};

// Main terrain LOD manager
class TerrainLODSystem {
public:
    TerrainLODSystem() = default;
    ~TerrainLODSystem() { Shutdown(); }

    // Initialize for a terrain with given dimensions
    void Init(uint32_t terrainResolution, const glm::vec2& worldSize, float maxHeight,
              const TerrainLODConfig& config = TerrainLODConfig{});
    
    void Shutdown();

    // Update LOD selection based on camera
    // Call once per frame before rendering
    void Update(const glm::vec3& cameraPos, const glm::mat4& viewProj);

    // Get visible chunks for rendering
    const std::vector<TerrainChunkLOD>& GetVisibleChunks() const { return m_visibleChunks; }
    
    // Get LOD index buffer for a given level
    bgfx::IndexBufferHandle GetLODIndexBuffer(uint32_t lodLevel) const;
    
    // Get vertex buffer (shared across all chunks)
    bgfx::VertexBufferHandle GetVertexBuffer() const { return m_vertexBuffer; }
    
    // Stats for profiling
    uint32_t GetVisibleChunkCount() const { return static_cast<uint32_t>(m_visibleChunks.size()); }
    uint32_t GetTotalChunkCount() const { return static_cast<uint32_t>(m_allChunks.size()); }
    uint32_t GetCulledChunkCount() const { return m_culledCount; }
    
    // LOD level info
    const TerrainLODLevel& GetLODLevel(uint32_t level) const { return m_lodLevels[level]; }
    uint32_t GetLODLevelCount() const { return m_lodLevelCount; }

private:
    void BuildChunks(uint32_t terrainRes, const glm::vec2& worldSize, float maxHeight);
    void BuildLODBuffers(uint32_t chunkSize);
    int SelectLOD(float distance) const;
    
    TerrainLODConfig m_config;
    
    // All terrain chunks
    std::vector<TerrainChunkLOD> m_allChunks;
    
    // Visible chunks after culling (updated each frame)
    std::vector<TerrainChunkLOD> m_visibleChunks;
    
    // LOD level definitions
    std::array<TerrainLODLevel, TerrainLODConfig::kMaxLODLevels> m_lodLevels;
    uint32_t m_lodLevelCount = 0;
    
    // Shared vertex buffer for chunk grid
    bgfx::VertexBufferHandle m_vertexBuffer = BGFX_INVALID_HANDLE;
    
    // Per-LOD index buffers
    std::array<bgfx::IndexBufferHandle, TerrainLODConfig::kMaxLODLevels> m_lodIndexBuffers;
    
    // Stats
    uint32_t m_culledCount = 0;
    
    // Terrain dimensions
    uint32_t m_terrainResolution = 0;
    glm::vec2 m_worldSize = glm::vec2(0.0f);
    float m_maxHeight = 0.0f;
    uint32_t m_chunksX = 0;
    uint32_t m_chunksZ = 0;
};

// ============================================================================
// Implementation
// ============================================================================

inline Frustum Frustum::FromViewProj(const glm::mat4& vp) {
    Frustum f;
    // Extract frustum planes from view-projection matrix (Gribb-Hartmann method)
    // Left
    f.planes[0] = glm::vec4(
        vp[0][3] + vp[0][0],
        vp[1][3] + vp[1][0],
        vp[2][3] + vp[2][0],
        vp[3][3] + vp[3][0]);
    // Right
    f.planes[1] = glm::vec4(
        vp[0][3] - vp[0][0],
        vp[1][3] - vp[1][0],
        vp[2][3] - vp[2][0],
        vp[3][3] - vp[3][0]);
    // Bottom
    f.planes[2] = glm::vec4(
        vp[0][3] + vp[0][1],
        vp[1][3] + vp[1][1],
        vp[2][3] + vp[2][1],
        vp[3][3] + vp[3][1]);
    // Top
    f.planes[3] = glm::vec4(
        vp[0][3] - vp[0][1],
        vp[1][3] - vp[1][1],
        vp[2][3] - vp[2][1],
        vp[3][3] - vp[3][1]);
    // Near
    f.planes[4] = glm::vec4(
        vp[0][3] + vp[0][2],
        vp[1][3] + vp[1][2],
        vp[2][3] + vp[2][2],
        vp[3][3] + vp[3][2]);
    // Far
    f.planes[5] = glm::vec4(
        vp[0][3] - vp[0][2],
        vp[1][3] - vp[1][2],
        vp[2][3] - vp[2][2],
        vp[3][3] - vp[3][2]);
    
    // Normalize planes
    for (int i = 0; i < 6; ++i) {
        float len = glm::length(glm::vec3(f.planes[i]));
        if (len > 0.0001f) f.planes[i] /= len;
    }
    return f;
}

inline bool Frustum::ContainsAABB(const glm::vec3& min, const glm::vec3& max) const {
    for (int i = 0; i < 6; ++i) {
        const glm::vec4& p = planes[i];
        // Find the positive vertex (furthest along plane normal)
        glm::vec3 pVertex(
            p.x >= 0 ? max.x : min.x,
            p.y >= 0 ? max.y : min.y,
            p.z >= 0 ? max.z : min.z);
        
        // If positive vertex is behind plane, AABB is outside
        if (glm::dot(glm::vec3(p), pVertex) + p.w < 0) {
            return false;
        }
    }
    return true;
}

inline void TerrainLODSystem::Init(uint32_t terrainResolution, const glm::vec2& worldSize, 
                                    float maxHeight, const TerrainLODConfig& config) {
    Shutdown();
    
    m_config = config;
    m_terrainResolution = terrainResolution;
    m_worldSize = worldSize;
    m_maxHeight = maxHeight;
    
    // Calculate chunk grid dimensions
    uint32_t chunkVertices = config.chunkSize;
    m_chunksX = (terrainResolution + chunkVertices - 2) / (chunkVertices - 1);
    m_chunksZ = m_chunksX;
    
    BuildChunks(terrainResolution, worldSize, maxHeight);
    BuildLODBuffers(config.chunkSize);
}

inline void TerrainLODSystem::Shutdown() {
    if (bgfx::isValid(m_vertexBuffer)) {
        bgfx::destroy(m_vertexBuffer);
        m_vertexBuffer = BGFX_INVALID_HANDLE;
    }
    for (auto& ib : m_lodIndexBuffers) {
        if (bgfx::isValid(ib)) {
            bgfx::destroy(ib);
            ib = BGFX_INVALID_HANDLE;
        }
    }
    m_allChunks.clear();
    m_visibleChunks.clear();
}

inline void TerrainLODSystem::BuildChunks(uint32_t terrainRes, const glm::vec2& worldSize, float maxHeight) {
    m_allChunks.clear();
    m_allChunks.reserve(m_chunksX * m_chunksZ);
    
    float chunkWorldSizeX = worldSize.x / m_chunksX;
    float chunkWorldSizeZ = worldSize.y / m_chunksZ;
    
    for (uint32_t z = 0; z < m_chunksZ; ++z) {
        for (uint32_t x = 0; x < m_chunksX; ++x) {
            TerrainChunkLOD chunk;
            chunk.gridX = x;
            chunk.gridZ = z;
            chunk.worldMin = glm::vec2(x * chunkWorldSizeX, z * chunkWorldSizeZ);
            chunk.worldMax = glm::vec2((x + 1) * chunkWorldSizeX, (z + 1) * chunkWorldSizeZ);
            chunk.minHeight = 0.0f;
            chunk.maxHeight = maxHeight;
            chunk.currentLOD = -1;
            chunk.visible = false;
            chunk.distanceToCamera = 0.0f;
            m_allChunks.push_back(chunk);
        }
    }
}

inline void TerrainLODSystem::BuildLODBuffers(uint32_t chunkSize) {
    // Build shared vertex buffer for a single chunk
    std::vector<float> vertices;
    vertices.reserve(chunkSize * chunkSize * 5); // x, z, u, v (y comes from height texture)
    
    for (uint32_t z = 0; z < chunkSize; ++z) {
        for (uint32_t x = 0; x < chunkSize; ++x) {
            float u = static_cast<float>(x) / (chunkSize - 1);
            float v = static_cast<float>(z) / (chunkSize - 1);
            vertices.push_back(u);  // Local X (0-1)
            vertices.push_back(v);  // Local Z (0-1)
            vertices.push_back(u);  // Texture U
            vertices.push_back(v);  // Texture V
        }
    }
    
    bgfx::VertexLayout layout;
    layout.begin()
        .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)  // Local XZ
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float) // UV
        .end();
    
    const bgfx::Memory* vmem = bgfx::copy(vertices.data(), 
        static_cast<uint32_t>(vertices.size() * sizeof(float)));
    m_vertexBuffer = bgfx::createVertexBuffer(vmem, layout);
    
    // Build index buffers for each LOD level
    m_lodLevelCount = TerrainLODConfig::kMaxLODLevels;
    uint32_t steps[4] = { 1, 2, 4, 8 }; // Vertex skip factors
    
    for (uint32_t lod = 0; lod < m_lodLevelCount; ++lod) {
        uint32_t step = steps[lod];
        uint32_t lodSize = (chunkSize - 1) / step + 1;
        
        std::vector<uint16_t> indices;
        indices.reserve((lodSize - 1) * (lodSize - 1) * 6);
        
        for (uint32_t z = 0; z < lodSize - 1; ++z) {
            for (uint32_t x = 0; x < lodSize - 1; ++x) {
                uint32_t v00 = (z * step) * chunkSize + (x * step);
                uint32_t v10 = (z * step) * chunkSize + ((x + 1) * step);
                uint32_t v01 = ((z + 1) * step) * chunkSize + (x * step);
                uint32_t v11 = ((z + 1) * step) * chunkSize + ((x + 1) * step);
                
                // First triangle
                indices.push_back(static_cast<uint16_t>(v00));
                indices.push_back(static_cast<uint16_t>(v01));
                indices.push_back(static_cast<uint16_t>(v10));
                // Second triangle
                indices.push_back(static_cast<uint16_t>(v10));
                indices.push_back(static_cast<uint16_t>(v01));
                indices.push_back(static_cast<uint16_t>(v11));
            }
        }
        
        m_lodLevels[lod].maxDistance = m_config.lodDistances[lod];
        m_lodLevels[lod].gridStep = step;
        m_lodLevels[lod].indexOffset = 0;
        m_lodLevels[lod].indexCount = static_cast<uint32_t>(indices.size());
        
        const bgfx::Memory* imem = bgfx::copy(indices.data(),
            static_cast<uint32_t>(indices.size() * sizeof(uint16_t)));
        m_lodIndexBuffers[lod] = bgfx::createIndexBuffer(imem);
    }
}

inline int TerrainLODSystem::SelectLOD(float distance) const {
    for (uint32_t i = 0; i < m_lodLevelCount; ++i) {
        if (distance <= m_lodLevels[i].maxDistance) {
            return static_cast<int>(i);
        }
    }
    return static_cast<int>(m_lodLevelCount - 1);
}

inline void TerrainLODSystem::Update(const glm::vec3& cameraPos, const glm::mat4& viewProj) {
    m_visibleChunks.clear();
    m_culledCount = 0;
    
    Frustum frustum = Frustum::FromViewProj(viewProj);
    
    for (auto& chunk : m_allChunks) {
        // Build AABB for frustum test
        glm::vec3 aabbMin(chunk.worldMin.x, chunk.minHeight, chunk.worldMin.y);
        glm::vec3 aabbMax(chunk.worldMax.x, chunk.maxHeight, chunk.worldMax.y);
        
        // Frustum cull
        if (!frustum.ContainsAABB(aabbMin, aabbMax)) {
            chunk.visible = false;
            chunk.currentLOD = -1;
            ++m_culledCount;
            continue;
        }
        
        // Calculate distance to chunk center
        glm::vec3 chunkCenter(
            (chunk.worldMin.x + chunk.worldMax.x) * 0.5f,
            (chunk.minHeight + chunk.maxHeight) * 0.5f,
            (chunk.worldMin.y + chunk.worldMax.y) * 0.5f);
        
        chunk.distanceToCamera = glm::length(cameraPos - chunkCenter);
        chunk.currentLOD = SelectLOD(chunk.distanceToCamera);
        chunk.visible = true;
        
        m_visibleChunks.push_back(chunk);
    }
    
    // Sort by LOD then by distance for efficient batching
    std::sort(m_visibleChunks.begin(), m_visibleChunks.end(),
        [](const TerrainChunkLOD& a, const TerrainChunkLOD& b) {
            if (a.currentLOD != b.currentLOD) return a.currentLOD < b.currentLOD;
            return a.distanceToCamera < b.distanceToCamera;
        });
}

inline bgfx::IndexBufferHandle TerrainLODSystem::GetLODIndexBuffer(uint32_t lodLevel) const {
    if (lodLevel >= m_lodLevelCount) lodLevel = m_lodLevelCount - 1;
    return m_lodIndexBuffers[lodLevel];
}

} // namespace rendering
} // namespace cm

