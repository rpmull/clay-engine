$input a_position, a_texcoord0, i_data0, i_data1, i_data2, i_data3
$output v_worldPos, v_texcoord0, v_color0

#include <bgfx_shader.sh>

uniform vec4 u_GrassLayerParams;   // x=mode, y=wind strength, z=fixed billboard flag, w=alpha cutout
uniform vec4 u_GrassWindDir;       // xy=normalized wind direction (world XZ), zw=unused
uniform vec4 u_GrassBaseColor;
uniform vec4 u_GrassColorVariance;
uniform vec4 u_GrassTime;
uniform vec4 u_GrassTerrainInfo;   // xy=terrain world size, zw=terrain world origin (XZ)
uniform vec4 u_GrassDeformParams;  // x=max deformation, yzw=unused

SAMPLER2D(s_deformationTex, 4);    // Deformation texture: RG=direction*strength

void main()
{
    vec3 side     = i_data0.xyz;
    vec3 up       = i_data1.xyz;
    vec3 forward  = i_data2.xyz;
    vec3 basePos  = i_data3.xyz;

    float randColor = i_data0.w;
    float windPhase = i_data3.w;
    float windStrength = u_GrassLayerParams.y;
    float isFixedBillboard = u_GrassLayerParams.z;
    float deformationEnabled = u_GrassDeformParams.x > 0.0001 ? 1.0 : 0.0;

    // Wind sway - apply horizontal offset based on height (a_position.y)
    float windTime = u_GrassTime.x * 2.0 + windPhase;
    float swayAmount = sin(windTime) * windStrength * 0.15 * a_position.y;
    
    // For fixed billboard mode, use world-space wind direction for consistent sway
    // For regular billboard, use the blade's side vector (which faces camera)
    vec3 swayDir = mix(side, vec3(u_GrassWindDir.x, 0.0, u_GrassWindDir.y), isFixedBillboard);
    
    // Calculate world position with wind offset
    vec3 worldPos = basePos
        + side * a_position.x
        + up   * a_position.y
        + swayDir * swayAmount;

    // Sample and apply grass deformation from player/NPC movement
    if (deformationEnabled > 0.5)
    {
        // Calculate UV in terrain space for deformation sampling
        vec2 terrainUV = (basePos.xz - u_GrassTerrainInfo.zw) / u_GrassTerrainInfo.xy;
        terrainUV = clamp(terrainUV, vec2(0.0, 0.0), vec2(1.0, 1.0));
        
        // Sample deformation texture (RG contains direction * strength)
        // Use texture2DLod with LOD 0 since vertex shaders don't support Sample()
        vec4 deform = texture2DLod(s_deformationTex, terrainUV, 0.0);
        vec2 deformDir = deform.xy;
        float deformStrength = length(deformDir);
        
        if (deformStrength > 0.001)
        {
            // Normalize direction and apply deformation based on vertex height
            // Bottom of grass stays planted, top bends most
            float heightFactor = a_position.y;  // 0 at base, ~1 at tip
            float bendAmount = deformStrength * u_GrassDeformParams.x * heightFactor;
            
            // Apply horizontal displacement in deformation direction
            vec3 deformOffset = vec3(deformDir.x, 0.0, deformDir.y) * bendAmount;
            worldPos += deformOffset;
        }
    }

    vec4 clipPos = mul(u_viewProj, vec4(worldPos, 1.0));

    // Apply base color with per-instance variance
    vec3 baseColor = u_GrassBaseColor.rgb;
    vec3 variance  = u_GrassColorVariance.rgb;
    vec3 finalColor = clamp(baseColor + variance * (randColor * 2.0 - 1.0), 0.0, 1.0);

    v_worldPos  = worldPos;
    v_texcoord0 = vec4(a_texcoord0.xy, 0.0, 0.0);
    v_color0    = vec4(finalColor, 1.0);

    gl_Position = clipPos;
}
