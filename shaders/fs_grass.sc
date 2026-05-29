$input v_worldPos, v_texcoord0, v_color0

#include <bgfx_shader.sh>

SAMPLER2D(s_GrassTex, 0);

uniform vec4 u_GrassLayerParams; // x=mode, y=wind strength, z=unused, w=alpha cutout
uniform vec4 u_GrassFadeParams;  // x=min, y=max, z=inv range, w=unused
uniform vec4 u_cameraPos;
uniform vec4 u_ambientFog;       // xyz = ambient color, w = fog enabled flag
uniform vec4 u_fogParams;        // x = fog density, yzw = fog color

void main()
{
    vec4 albedo = texture2D(s_GrassTex, v_texcoord0.xy);
    float alpha = albedo.a;

    if (u_GrassLayerParams.x < 0.5)
    {
        if (alpha < u_GrassLayerParams.w)
        {
            discard;
        }
    }

    vec3 color = albedo.rgb * v_color0.rgb;

    float dist = length(v_worldPos - u_cameraPos.xyz);
    float fade = clamp(1.0 - (dist - u_GrassFadeParams.x) * u_GrassFadeParams.z, 0.0, 1.0);
    color *= fade;

    // Apply exponential fog (matches fs_pbr.sc)
    if (u_ambientFog.w > 0.5)
    {
        float fogFactor = 1.0 - clamp(exp(-u_fogParams.x * dist), 0.0, 1.0);
        vec3 fogColor = u_fogParams.yzw;
        color = mix(color, fogColor, fogFactor);
    }

    float outAlpha = (u_GrassLayerParams.x < 0.5) ? alpha * fade : 1.0;
    gl_FragColor = vec4(color, outAlpha);
}
