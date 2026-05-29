$input v_worldPos, v_normal, v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_albedo, 0);
uniform vec4 u_PBRScalar1;

void main()
{
    float alphaCutoff = u_PBRScalar1.y;
    if (alphaCutoff > 0.001 && texture2D(s_albedo, v_texcoord0.xy).a < alphaCutoff) {
        discard;
    }

    float d = clamp(gl_FragCoord.z, 0.0, 1.0);
    gl_FragColor = vec4(d, d, d, 1.0);
}
