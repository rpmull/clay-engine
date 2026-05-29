$input v_texcoord1

#include <bgfx_shader.sh>

void main()
{
    gl_FragColor = vec4(v_texcoord1.rgb, 1.0);
}
