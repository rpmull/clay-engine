$input a_position
$output v_position, v_texcoord0, v_heightUV, v_worldPos, v_normal

#include <bgfx_shader.sh>

// Height texture sampler (unified for entire terrain)
SAMPLER2D(s_heightTexture, 0);

// Per-chunk uniforms
uniform vec4 u_chunkParams;     // xy=UV offset into heightmap, zw=UV scale
uniform vec4 u_chunkWorld;      // xy=world XZ offset, zw=world XZ extent
uniform vec4 u_morphParams;     // x=morph factor, y=LOD level, z=grid size, w=unused
uniform vec4 u_neighborLODs;    // x=North LOD, y=East LOD, z=South LOD, w=West LOD

// Terrain uniforms
uniform vec4 u_heightParams;    // x=MaxHeight, y=cellSizeX, z=cellSizeZ, w=unused
uniform vec4 u_terrainSize;     // xy=terrain world size, zw=unused
uniform vec4 u_cameraPos;       // xyz=camera position
uniform vec4 u_texelSize;       // xy=1/heightmap resolution

// Normal matrix for proper normal transformation
uniform mat4 u_normalMat;

void main()
{
    // a_position.xy = local coordinates within chunk (0-1 range)
    vec2 localUV = a_position.xy;
    
    float gridSize = u_morphParams.z;
    float morphFactor = u_morphParams.x;
    float myLOD = u_morphParams.y;
    
    // Convert to grid position for morph calculation
    vec2 gridPos = localUV * (gridSize - 1.0);
    
    // Check if on chunk edges for neighbor LOD-aware morphing
    bool onNorthEdge = localUV.y > 0.99;
    bool onSouthEdge = localUV.y < 0.01;
    bool onEastEdge = localUV.x > 0.99;
    bool onWestEdge = localUV.x < 0.01;
    
    // Calculate edge morph factor based on neighbor LOD difference
    // If neighbor is coarser (higher LOD number), we need to morph edge vertices
    float edgeMorph = 0.0;
    if (onNorthEdge && u_neighborLODs.x > myLOD) edgeMorph = max(edgeMorph, 1.0);
    if (onEastEdge && u_neighborLODs.y > myLOD) edgeMorph = max(edgeMorph, 1.0);
    if (onSouthEdge && u_neighborLODs.z > myLOD) edgeMorph = max(edgeMorph, 1.0);
    if (onWestEdge && u_neighborLODs.w > myLOD) edgeMorph = max(edgeMorph, 1.0);
    
    // Apply vertex morphing for smooth LOD transitions
    // Odd-indexed vertices morph toward their even-indexed neighbors
    vec2 morphedUV = localUV;
    float effectiveMorph = max(morphFactor, edgeMorph);
    
    if (effectiveMorph > 0.001)
    {
        // Check if this vertex is at an odd grid position
        vec2 fractPart = fract(gridPos * 0.5);
        
        // Calculate morph offset (odd vertices snap to even neighbors)
        vec2 morphOffset = vec2(0.0, 0.0);
        float uvStep = 1.0 / (gridSize - 1.0);
        
        // X-axis: if odd, morph toward x-1 (which is even)
        if (fractPart.x > 0.25)
        {
            morphOffset.x = -uvStep;
        }
        
        // Z-axis: if odd, morph toward z-1 (which is even)
        if (fractPart.y > 0.25)
        {
            morphOffset.y = -uvStep;
        }
        
        // Only morph if this vertex is actually odd on at least one axis
        float isOdd = step(0.25, max(fractPart.x, fractPart.y));
        morphedUV = localUV + morphOffset * effectiveMorph * isOdd;
        
        // Clamp to valid UV range
        morphedUV = clamp(morphedUV, vec2(0.0, 0.0), vec2(1.0, 1.0));
    }
    
    // Calculate UV into unified heightmap
    vec2 heightUV = u_chunkParams.xy + morphedUV * u_chunkParams.zw;
    heightUV = clamp(heightUV, vec2(0.0, 0.0), vec2(1.0, 1.0));
    
    // Sample at texel centers to match CPU heightfield
    vec2 texelSize = max(u_texelSize.xy, vec2(1e-6, 1e-6));
    vec2 texelCenterOffset = texelSize * 0.5;
    vec2 sampleUV = heightUV * (vec2(1.0, 1.0) - texelSize) + texelCenterOffset;

    // Sample height from terrain height texture
    float h = texture2DLod(s_heightTexture, sampleUV, 0.0).x;
    float worldY = h * u_heightParams.x;
    
    // Calculate world XZ position (local to terrain)
    vec2 worldXZ = u_chunkWorld.xy + morphedUV * u_chunkWorld.zw;
    
    // Build local position
    vec3 localPos = vec3(worldXZ.x, worldY, worldXZ.y);
    
    // Compute normal from height gradient (finite differences)
    vec2 uvStep = texelSize;
    
    vec2 uvL = clamp(sampleUV + vec2(-uvStep.x, 0.0), vec2(0.0, 0.0), vec2(1.0, 1.0));
    vec2 uvR = clamp(sampleUV + vec2( uvStep.x, 0.0), vec2(0.0, 0.0), vec2(1.0, 1.0));
    vec2 uvD = clamp(sampleUV + vec2(0.0, -uvStep.y), vec2(0.0, 0.0), vec2(1.0, 1.0));
    vec2 uvU = clamp(sampleUV + vec2(0.0,  uvStep.y), vec2(0.0, 0.0), vec2(1.0, 1.0));
    
    float hL = texture2DLod(s_heightTexture, uvL, 0.0).x * u_heightParams.x;
    float hR = texture2DLod(s_heightTexture, uvR, 0.0).x * u_heightParams.x;
    float hD = texture2DLod(s_heightTexture, uvD, 0.0).x * u_heightParams.x;
    float hU = texture2DLod(s_heightTexture, uvU, 0.0).x * u_heightParams.x;
    
    // Central differences in world space
    vec2 sampleSpacing = max(u_heightParams.yz, vec2(1e-6, 1e-6));
    float dhdx = (hR - hL) / (2.0 * sampleSpacing.x);
    float dhdz = (hU - hD) / (2.0 * sampleSpacing.y);
    
    vec3 normal = normalize(vec3(-dhdx, 1.0, -dhdz));
    
    // Transform to world space
    vec4 worldPos4 = mul(u_model[0], vec4(localPos, 1.0));
    v_worldPos = worldPos4.xyz;
    v_position = localPos;
    
    // Output UVs for fragment shader
    v_texcoord0 = vec4(heightUV.x, heightUV.y, 0.0, 0.0);
    v_heightUV = sampleUV;
    
    // Transform normal
    v_normal = normalize(mul((mat3)u_normalMat, normal));
    
    gl_Position = mul(u_modelViewProj, vec4(localPos, 1.0));
}

