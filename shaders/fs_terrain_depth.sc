$input v_heightUV

#include <bgfx_shader.sh>

SAMPLER2D(s_holeTexture, 5);

void main()
{
    if (texture2D(s_holeTexture, v_heightUV).r > 0.5)
    {
        discard;
    }

    float d = clamp(gl_FragCoord.z, 0.0, 1.0);
    gl_FragColor = vec4(d, d, d, 1.0);
}
