$input a_position
$output v_position, v_texcoord0, v_heightUV, v_worldPos, v_normal

#include <bgfx_shader.sh>

// Height texture sampler
SAMPLER2D(s_heightTexture, 0);

// Clipmap parameters
uniform vec4 u_clipmapParams;   // x=level scale, y=morph factor, z=grid size, w=level index
uniform vec4 u_clipmapOffset;   // xy=level snap offset in world, zw=terrain world size
uniform vec4 u_heightParams;    // x=MaxHeight, yzw unused
uniform vec4 u_cameraPos;       // xyz=camera position
uniform vec4 u_terrainOrigin;   // xyz=terrain world origin (translation from transform)

// Normal matrix for proper normal transformation
uniform mat4 u_normalMat;

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
    // Range: approximately -halfGridSize to +halfGridSize
    vec2 localXZ = a_position.xy;
    
    // Convert to integer grid coordinates (0 to gridSize-1)
    // Adding halfGridSize shifts from [-half, +half] to [0, gridSize-1]
    vec2 gridPos = localXZ + halfGridSize;
    
    // Transform to world XZ position
    vec2 worldXZ = localXZ * levelScale + levelOffset;
    
    // Apply morphing to smooth LOD transitions
    // Odd-indexed vertices morph toward their even-indexed neighbors
    if (morphFactor > 0.001)
    {
        // Check if this vertex is at an odd grid position (should morph)
        // An odd vertex has fract(gridPos * 0.5) != 0
        vec2 fractPart = fract(gridPos * 0.5);
        
        // For each axis, if fractPart is ~0.5, the vertex is odd on that axis
        // We morph odd vertices toward the previous even position
        vec2 morphOffset = vec2(0.0, 0.0);
        
        // X-axis: if odd, morph toward x-1 (which is even)
        if (fractPart.x > 0.25)
        {
            morphOffset.x = -levelScale;  // Move one grid step back
        }
        
        // Z-axis: if odd, morph toward z-1 (which is even)
        if (fractPart.y > 0.25)
        {
            morphOffset.y = -levelScale;  // Move one grid step back
        }
        
        // Only morph if this vertex is actually odd on at least one axis
        float isOdd = step(0.25, max(fractPart.x, fractPart.y));
        worldXZ = worldXZ + morphOffset * morphFactor * isOdd;
    }
    
    // Convert world XZ to terrain-local coordinates for UV calculation
    // This accounts for terrain not being at world origin
    vec2 localTerrainXZ = worldXZ - terrainOriginXZ;
    
    // Clamp to terrain bounds (in terrain-local space)
    localTerrainXZ = clamp(localTerrainXZ, vec2(0.0, 0.0), terrainSize);
    
    // Calculate UV for height texture sampling (0-1 across entire terrain)
    // UV is relative to terrain's own coordinate space, not world space
    vec2 heightUV = localTerrainXZ / max(terrainSize, vec2(1.0, 1.0));
    
    // Sample height from terrain height texture
    float h = texture2DLod(s_heightTexture, heightUV, 0.0).x;
    float worldY = h * u_heightParams.x + u_terrainOrigin.y;  // Add terrain Y origin
    
    // Build final world position (use world XZ, which includes terrain origin implicitly via levelOffset)
    // Note: worldXZ is in world space, heightUV samples from terrain-local space
    vec3 localPos = vec3(worldXZ.x, worldY, worldXZ.y);
    
    // Compute normal from height gradient (finite differences)
    // Use world-space step size for proper normal calculation
    // The step in UV space corresponds to one vertex spacing in world space
    float worldStep = levelScale;  // One vertex = levelScale world units
    vec2 uvStep = vec2(worldStep, worldStep) / max(terrainSize, vec2(1.0, 1.0));
    
    vec2 uvL = clamp(heightUV + vec2(-uvStep.x, 0.0), vec2(0.0, 0.0), vec2(1.0, 1.0));
    vec2 uvR = clamp(heightUV + vec2( uvStep.x, 0.0), vec2(0.0, 0.0), vec2(1.0, 1.0));
    vec2 uvD = clamp(heightUV + vec2(0.0, -uvStep.y), vec2(0.0, 0.0), vec2(1.0, 1.0));
    vec2 uvU = clamp(heightUV + vec2(0.0,  uvStep.y), vec2(0.0, 0.0), vec2(1.0, 1.0));
    
    float hL = texture2DLod(s_heightTexture, uvL, 0.0).x * u_heightParams.x;
    float hR = texture2DLod(s_heightTexture, uvR, 0.0).x * u_heightParams.x;
    float hD = texture2DLod(s_heightTexture, uvD, 0.0).x * u_heightParams.x;
    float hU = texture2DLod(s_heightTexture, uvU, 0.0).x * u_heightParams.x;
    
    // Central differences: spacing is 2 * worldStep
    float sampleSpacing = worldStep * 2.0;
    float dhdx = (hR - hL) / max(sampleSpacing, 0.001);
    float dhdz = (hU - hD) / max(sampleSpacing, 0.001);
    
    vec3 normal = normalize(vec3(-dhdx, 1.0, -dhdz));
    
    // Transform to world space
    vec4 worldPos4 = mul(u_model[0], vec4(localPos, 1.0));
    v_worldPos = worldPos4.xyz;
    v_position = localPos;
    
    // For layer texture tiling, use normalized UVs (0-1 across terrain)
    // This matches the regular terrain shader behavior where a_texcoord0 is 0-1
    // The fragment shader applies tiling: texture2D(s_layer, layerUV * tiling)
    v_texcoord0 = vec4(heightUV.x, heightUV.y, 0.0, 0.0);
    v_heightUV = heightUV;
    
    // Transform normal
    v_normal = normalize(mul((mat3)u_normalMat, normal));
    
    gl_Position = mul(u_modelViewProj, vec4(localPos, 1.0));
}









