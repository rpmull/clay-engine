$input v_worldPos, v_normal

#include <bgfx_shader.sh>

uniform vec4 u_pointShadowLightPosRangeDepth; // xyz=light position, w=far range

void main()
{
	float farRange = max(u_pointShadowLightPosRangeDepth.w, 1e-4);
	float radialDepth = length(v_worldPos - u_pointShadowLightPosRangeDepth.xyz) / farRange;
	radialDepth = clamp(radialDepth, 0.0, 1.0);
	gl_FragColor = vec4(radialDepth, radialDepth, radialDepth, 1.0);
}
