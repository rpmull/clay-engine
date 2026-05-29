$input a_position
$output v_texcoord0

#include <bgfx_shader.sh>

void main()
{
    // Fullscreen triangle already provides clip-space corners.
    gl_Position = vec4(a_position.x, a_position.y, 1.0, 1.0);
    // Pass clip-space XY directly so fs_sky can reconstruct camera rays via u_invViewProj.
    v_texcoord0.xy = a_position.xy;
}


