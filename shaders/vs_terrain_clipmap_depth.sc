$input a_position
$output v_heightUV

#include <bgfx_shader.sh>

// Height texture sampler
SAMPLER2D(s_heightTexture, 0);

// Clipmap parameters
uniform vec4 u_clipmapParams;   // x=level scale, y=morph factor, z=grid size, w=level index
uniform vec4 u_clipmapOffset;   // xy=level snap offset in world, zw=terrain world size
uniform vec4 u_heightParams;    // x=MaxHeight, yzw unused
uniform vec4 u_terrainOrigin;   // xyz=terrain world origin (translation from transform)
uniform mat4 u_lightViewProj;

void main()
{
    float levelScale = u_clipmapParams.x;
    float morphFactor = u_clipmapParams.y;
    float gridSize = u_clipmapParams.z;
    float halfGridSize = (gridSize - 1.0) * 0.5;
    
    vec2 terrainSize = u_clipmapOffset.zw;
    vec2 levelOffset = u_clipmapOffset.xy;
    vec2 terrainOriginXZ = u_terrainOrigin.xz;  // Terrain world origin (XZ plane)
    
    // a_position.xy contains local grid coordinates centered at origin
    vec2 localXZ = a_position.xy;
    
    // Convert to integer grid coordinates (0 to gridSize-1)
    vec2 gridPos = localXZ + halfGridSize;
    
    // Transform to world XZ position
    vec2 worldXZ = localXZ * levelScale + levelOffset;
    
    // Apply morphing to smooth LOD transitions
    if (morphFactor > 0.001)
    {
        vec2 fractPart = fract(gridPos * 0.5);
        vec2 morphOffset = vec2(0.0, 0.0);
        
        if (fractPart.x > 0.25)
        {
            morphOffset.x = -levelScale;
        }
        
        if (fractPart.y > 0.25)
        {
            morphOffset.y = -levelScale;
        }
        
        float isOdd = step(0.25, max(fractPart.x, fractPart.y));
        worldXZ = worldXZ + morphOffset * morphFactor * isOdd;
    }
    
    // Convert world XZ to terrain-local coordinates for UV calculation
    vec2 localTerrainXZ = worldXZ - terrainOriginXZ;
    
    // Clamp to terrain bounds (in terrain-local space)
    localTerrainXZ = clamp(localTerrainXZ, vec2(0.0, 0.0), terrainSize);
    
    // Calculate UV for height texture sampling (0-1 across entire terrain)
    vec2 heightUV = localTerrainXZ / max(terrainSize, vec2(1.0, 1.0));
    
    // Sample height from terrain height texture
    float h = texture2DLod(s_heightTexture, heightUV, 0.0).x;
    float worldY = h * u_heightParams.x + u_terrainOrigin.y;  // Add terrain Y origin
    
    // Build final world position
    vec3 localPos = vec3(worldXZ.x, worldY, worldXZ.y);
    
    // Transform to world space
    vec4 worldPos4 = mul(u_model[0], vec4(localPos, 1.0));
    v_heightUV = heightUV;

    // Output position using light view-projection for shadow map
    gl_Position = mul(u_lightViewProj, worldPos4);
}



