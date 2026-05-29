#pragma once

#include <bgfx/bgfx.h>
#include <glm/glm.hpp>
#include <vector>
#include <array>

// Forward declarations
struct TerrainComponent;

namespace terrain {

// Clipmap configuration
struct ClipmapConfig {
    uint32_t LevelCount = 4;           // Number of clipmap levels (rings)
    uint32_t GridSize = 64;            // Vertices per side for each clipmap block (power of 2)
    float BaseScale = 1.0f;            // World units per vertex at finest level
    bool EnableMorphing = true;        // Smooth transitions between LOD levels
    float MorphRegion = 0.3f;          // Fraction of ring where morphing occurs (0-1)
};

// A single clipmap ring level
struct ClipmapLevel {
    float Scale;                       // World units per vertex at this level
    glm::vec2 SnapOffset;              // Grid-snapped offset from camera
    glm::vec2 PrevSnapOffset;          // Previous frame for smooth updates
    bool NeedsUpdate = true;
};

// Pre-built mesh blocks that form the clipmap rings
// Each ring is composed of these blocks arranged around the center
struct ClipmapMeshes {
    // Block types:
    // - Block: Square NxN grid
    // - Ring: L-shaped piece (4 of these form a ring around inner level)
    // - Trim: Fills seams between LOD levels (handles 2:1 vertex ratio)
    // - Cross: Fills the center cross gap when levels don't align
    
    bgfx::VertexBufferHandle BlockVB = BGFX_INVALID_HANDLE;      // Standard square block
    bgfx::IndexBufferHandle BlockIB = BGFX_INVALID_HANDLE;
    uint32_t BlockIndexCount = 0;
    
    bgfx::VertexBufferHandle RingVB = BGFX_INVALID_HANDLE;       // L-shaped ring piece
    bgfx::IndexBufferHandle RingIB = BGFX_INVALID_HANDLE;
    uint32_t RingIndexCount = 0;
    
    // Interior trim pieces (handle LOD transitions)
    bgfx::VertexBufferHandle TrimHorizVB = BGFX_INVALID_HANDLE;  // Horizontal trim strip
    bgfx::IndexBufferHandle TrimHorizIB = BGFX_INVALID_HANDLE;
    uint32_t TrimHorizIndexCount = 0;
    
    bgfx::VertexBufferHandle TrimVertVB = BGFX_INVALID_HANDLE;   // Vertical trim strip
    bgfx::IndexBufferHandle TrimVertIB = BGFX_INVALID_HANDLE;
    uint32_t TrimVertIndexCount = 0;
    
    // Degenerate triangle strip to fill cross-shaped gap
    bgfx::VertexBufferHandle CrossVB = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle CrossIB = BGFX_INVALID_HANDLE;
    uint32_t CrossIndexCount = 0;
    
    // Center fill for innermost level
    bgfx::VertexBufferHandle CenterVB = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle CenterIB = BGFX_INVALID_HANDLE;
    uint32_t CenterIndexCount = 0;
    
    uint32_t GridSize = 0;  // Grid size these meshes were built for
};

// Main clipmap system for a terrain
class TerrainClipmapSystem {
public:
    TerrainClipmapSystem() = default;
    ~TerrainClipmapSystem() { Shutdown(); }
    
    // Initialize clipmap system for a terrain
    void Init(const ClipmapConfig& config);
    
    // Shutdown and release GPU resources
    void Shutdown();
    
    // Update clipmap positions based on camera
    // Returns true if any level snapped to new position
    bool Update(const glm::vec3& cameraPos, const glm::vec2& terrainWorldSize);
    
    // Get the clipmap configuration
    const ClipmapConfig& GetConfig() const { return m_Config; }
    
    // Get level data for rendering
    const std::vector<ClipmapLevel>& GetLevels() const { return m_Levels; }
    
    // Get shared meshes
    const ClipmapMeshes& GetMeshes() const { return m_Meshes; }
    
    // Check if initialized
    bool IsInitialized() const { return m_Initialized; }
    
private:
    void BuildMeshes();
    void BuildBlockMesh();
    void BuildRingMesh();
    void BuildTrimMeshes();
    void BuildCenterMesh();
    
    ClipmapConfig m_Config;
    std::vector<ClipmapLevel> m_Levels;
    ClipmapMeshes m_Meshes;
    bool m_Initialized = false;
    
    glm::vec3 m_LastCameraPos{0.0f};
};

// Render a terrain using clipmaps
// This is called from Renderer::RenderScene when clipmaps are enabled
void RenderTerrainClipmaps(
    TerrainComponent& terrain,
    TerrainClipmapSystem& clipmaps,
    const glm::mat4& worldMatrix,
    const glm::vec3& cameraPos,
    uint16_t viewId,
    bgfx::ProgramHandle program,
    const struct ClipmapRenderParams& params
);

// Parameters passed to clipmap rendering
struct ClipmapRenderParams {
    bgfx::UniformHandle u_clipmapParams;      // (level scale, morph factor, grid size, 0)
    bgfx::UniformHandle u_clipmapOffset;      // (offsetX, offsetZ, terrainSizeX, terrainSizeZ)
    bgfx::UniformHandle u_heightParams;       // (maxHeight, cellSizeX, cellSizeZ, 0)
    bgfx::TextureHandle heightTexture;
    bgfx::TextureHandle splatTexture;
    bgfx::UniformHandle s_heightTexture;
    bgfx::UniformHandle s_splatTexture;
};

} // namespace terrain


