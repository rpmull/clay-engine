$input v_texcoord0, v_color0

#include <bgfx_shader.sh>

SAMPLER2D(s_text, 0);
uniform vec4 u_textParams; // x: 0 = fill, 1 = outline; yz: outline radius in atlas UVs

float sampleTextAlpha(vec2 uv)
{
    return texture2D(s_text, clamp(uv, vec2(0.0, 0.0), vec2(1.0, 1.0))).r;
}

float dilateTextAlpha(vec2 uv, vec2 radius)
{
    vec2 diag = radius * 0.70710678;
    float a = sampleTextAlpha(uv);
    a = max(a, sampleTextAlpha(uv + vec2( radius.x, 0.0)));
    a = max(a, sampleTextAlpha(uv + vec2(-radius.x, 0.0)));
    a = max(a, sampleTextAlpha(uv + vec2(0.0,  radius.y)));
    a = max(a, sampleTextAlpha(uv + vec2(0.0, -radius.y)));
    a = max(a, sampleTextAlpha(uv + vec2( diag.x,  diag.y)));
    a = max(a, sampleTextAlpha(uv + vec2(-diag.x,  diag.y)));
    a = max(a, sampleTextAlpha(uv + vec2( diag.x, -diag.y)));
    a = max(a, sampleTextAlpha(uv + vec2(-diag.x, -diag.y)));
    return a;
}

void main()
{
    float center = sampleTextAlpha(v_texcoord0.xy);
    float a = center;
    if (u_textParams.x > 0.5)
    {
        vec2 radius = u_textParams.yz;
        float dilated = dilateTextAlpha(v_texcoord0.xy, radius);
        dilated = max(dilated, dilateTextAlpha(v_texcoord0.xy, radius * 0.66));
        dilated = max(dilated, dilateTextAlpha(v_texcoord0.xy, radius * 0.33));
        a = max(dilated - center, 0.0);
    }
    gl_FragColor = vec4(v_color0.rgb, v_color0.a * a);
}


