$input v_worldPos, v_normal, v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_albedo, 0);
uniform vec4 u_PBRScalar1;
uniform vec4 u_pointShadowLightPosRangeDepth; // xyz=light position, w=far range

void main()
{
    float alphaCutoff = u_PBRScalar1.y;
    if (alphaCutoff > 0.001 && texture2D(s_albedo, v_texcoord0.xy).a < alphaCutoff) {
        discard;
    }

    float farRange = max(u_pointShadowLightPosRangeDepth.w, 1e-4);
    float radialDepth = length(v_worldPos - u_pointShadowLightPosRangeDepth.xyz) / farRange;
    radialDepth = clamp(radialDepth, 0.0, 1.0);
    gl_FragColor = vec4(radialDepth, radialDepth, radialDepth, 1.0);
}
