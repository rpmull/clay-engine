$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_shadowDebug, 0);
uniform vec4 u_shadowDebugParams; // x=originBottomLeft, y=selectedCascade, z=tiles

void main()
{
    vec2 uv = v_texcoord0.xy;
    if (u_shadowDebugParams.x < 0.5) {
        uv.y = 1.0 - uv.y;
    }

    float depth = texture2D(s_shadowDebug, uv).r;
    vec3 color = vec3(depth, depth, depth);

    float tiles = max(u_shadowDebugParams.z, 1.0);
    vec2 tiled = uv * tiles;
    vec2 fracUV = fract(tiled);
    float edgeDist = min(min(fracUV.x, fracUV.y), min(1.0 - fracUV.x, 1.0 - fracUV.y));
    float grid = (edgeDist < 0.01) ? 1.0 : 0.0;

    float tileIndex = floor(tiled.x) + floor(tiled.y) * tiles;
    float selectedCascade = floor(u_shadowDebugParams.y + 0.5);
    if (u_shadowDebugParams.x < 0.5) {
        float sx = mod(selectedCascade, tiles);
        float sy = floor(selectedCascade / tiles);
        sy = (tiles - 1.0) - sy;
        selectedCascade = sy * tiles + sx;
    }
    float selected = (abs(tileIndex - selectedCascade) < 0.25) ? 1.0 : 0.0;

    color = mix(color, vec3(0.0, 0.8, 0.1), grid * 0.8);
    float selectedBorder = (edgeDist < 0.03) ? 1.0 : 0.0;
    color = mix(color, vec3(0.9, 0.2, 0.1), selected * selectedBorder * 0.9);

    gl_FragColor = vec4(color, 1.0);
}
