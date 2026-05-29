$input v_color0, v_texcoord0

/*
 * Copyright 2011-2025 Branimir Karadzic. All rights reserved.
 * License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE
 */

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
uniform vec4 u_particleParams; // x = blend mode, yzw reserved

void main()
{
    vec4 tex = texture2D(s_texColor, v_texcoord0.xy);

    // Color modulation by per-particle vertex color; use texture alpha for transparency.
    // For multiply, treat alpha as a lerp to white so transparent areas don't darken the scene.
    if (u_particleParams.x > 1.5)
    {
        tex.rgb = mix(vec3(1.0, 1.0, 1.0), tex.rgb, tex.a);
    }
    vec4 outCol = tex * v_color0;
    // Per-particle fade factor in v_texcoord0.z (0..1), multiply both rgb and alpha.
    float blend = clamp(v_texcoord0.z, 0.0, 1.0);
    outCol.rgb *= blend;
    outCol.a *= blend;
    
    // Premultiply for alpha/additive to avoid color bleed on soft edges.
    if (u_particleParams.x < 1.5)
    {
        outCol.rgb *= outCol.a;
    }
    
    // Discard nearly transparent fragments to avoid outline artifacts
    if (outCol.a < 0.01)
    {
        discard;
    }
    
    gl_FragColor = outCol;
}
