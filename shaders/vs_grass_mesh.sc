$input a_position, a_normal, a_texcoord0, i_data0, i_data1, i_data2, i_data3
$output v_worldPos, v_normal, v_texcoord0, v_color0

#include <bgfx_shader.sh>

uniform vec4 u_GrassLayerParams;   // x=mode, y=wind strength, z=fixed billboard flag, w=alpha cutout
uniform vec4 u_GrassBaseColor;
uniform vec4 u_GrassColorVariance;
uniform vec4 u_GrassTerrainInfo;   // xy=terrain world size, zw=terrain world origin (XZ)
uniform vec4 u_GrassDeformParams;  // x=max deformation, yzw=unused

SAMPLER2D(s_deformationTex, 4);    // Deformation texture: RG=direction*strength

void main()
{
    vec4 inst0 = i_data0;
    vec4 inst1 = i_data1;
    vec4 inst2 = i_data2;
    vec4 inst3 = i_data3;

    vec3 basisX = inst0.xyz;
    vec3 basisY = inst1.xyz;
    vec3 basisZ = inst2.xyz;
    float randColor = inst0.w;
    float deformationEnabled = u_GrassDeformParams.x > 0.0001 ? 1.0 : 0.0;

    vec3 lp = a_position;
    vec3 basePos = inst3.xyz;
    vec3 worldPos = basePos
                  + basisX * lp.x
                  + basisY * lp.y
                  + basisZ * lp.z;

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
            // Use local Y position as height factor (assumes grass mesh is upright)
            float heightFactor = max(0.0, lp.y);
            float bendAmount = deformStrength * u_GrassDeformParams.x * heightFactor;
            
            // Apply horizontal displacement in deformation direction
            vec3 deformOffset = vec3(deformDir.x, 0.0, deformDir.y) * bendAmount;
            worldPos += deformOffset;
        }
    }

    vec3 n = normalize(a_normal);
    vec3 worldNormal = normalize(
        basisX * n.x +
        basisY * n.y +
        basisZ * n.z
    );

    vec4 worldPos4 = vec4(worldPos, 1.0);
    gl_Position = mul(u_viewProj, worldPos4);

    vec3 baseColor = u_GrassBaseColor.rgb;
    vec3 variance  = u_GrassColorVariance.rgb;
    vec3 color     = clamp(baseColor + variance * (randColor * 2.0 - 1.0), 0.0, 1.0);

    v_worldPos  = worldPos;
    v_normal    = worldNormal;
    v_texcoord0 = vec4(a_texcoord0.xy, inst3.w, 0.0);
    v_color0    = vec4(color, 0.0);
}

