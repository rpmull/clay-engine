$input a_position
$output v_heightUV

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
uniform vec4 u_texelSize;       // xy=1/heightmap resolution
uniform mat4 u_lightViewProj;

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
    float edgeMorph = 0.0;
    if (onNorthEdge && u_neighborLODs.x > myLOD) edgeMorph = max(edgeMorph, 1.0);
    if (onEastEdge && u_neighborLODs.y > myLOD) edgeMorph = max(edgeMorph, 1.0);
    if (onSouthEdge && u_neighborLODs.z > myLOD) edgeMorph = max(edgeMorph, 1.0);
    if (onWestEdge && u_neighborLODs.w > myLOD) edgeMorph = max(edgeMorph, 1.0);
    
    // Apply vertex morphing
    vec2 morphedUV = localUV;
    float effectiveMorph = max(morphFactor, edgeMorph);
    
    if (effectiveMorph > 0.001)
    {
        vec2 fractPart = fract(gridPos * 0.5);
        vec2 morphOffset = vec2(0.0, 0.0);
        float uvStep = 1.0 / (gridSize - 1.0);
        
        if (fractPart.x > 0.25) morphOffset.x = -uvStep;
        if (fractPart.y > 0.25) morphOffset.y = -uvStep;
        
        float isOdd = step(0.25, max(fractPart.x, fractPart.y));
        morphedUV = localUV + morphOffset * effectiveMorph * isOdd;
        morphedUV = clamp(morphedUV, vec2(0.0, 0.0), vec2(1.0, 1.0));
    }
    
    // Calculate UV into unified heightmap
    vec2 heightUV = u_chunkParams.xy + morphedUV * u_chunkParams.zw;
    heightUV = clamp(heightUV, vec2(0.0, 0.0), vec2(1.0, 1.0));
    
    // Sample at texel centers to match CPU heightfield
    vec2 texelSize = max(u_texelSize.xy, vec2(1e-6, 1e-6));
    vec2 texelCenterOffset = texelSize * 0.5;
    vec2 sampleUV = heightUV * (vec2(1.0, 1.0) - texelSize) + texelCenterOffset;

    // Sample height
    float h = texture2DLod(s_heightTexture, sampleUV, 0.0).x;
    float worldY = h * u_heightParams.x;
    
    // Calculate world XZ position
    vec2 worldXZ = u_chunkWorld.xy + morphedUV * u_chunkWorld.zw;
    
    // Build terrain-local position, then transform to world to match color pass.
    vec3 localPos = vec3(worldXZ.x, worldY, worldXZ.y);
    vec4 worldPos = mul(u_model[0], vec4(localPos, 1.0));
    v_heightUV = sampleUV;
    
    gl_Position = mul(u_lightViewProj, worldPos);
}

