$input a_position, a_normal, a_texcoord0
$output v_worldPos, v_normal, v_texcoord0, v_viewDir

#include <bgfx_shader.sh>

uniform vec4 u_psxParams; // x=jitter_amp_px, y=affine_factor [0..1], z=light_influence [0..1], w=normalPerturbAmp
uniform vec4 u_psxWorld;  // x=vertexWorldAmp (meters), y=tileSize (meters), z=unused, w=unused
uniform vec4 u_cameraPos;
uniform mat4 u_normalMat; // transpose(inverse(mat3(model)))

void main()
{
    vec4 worldPos = mul(u_model[0], vec4(a_position, 1.0));
    vec3 worldNormal = normalize(mul((mat3)u_normalMat, a_normal));

    // Planar-preserving world-space offset: per-tile constant along normal
    float worldAmp = max(u_psxWorld.x, 0.0);
    float tileSize = max(u_psxWorld.y, 1e-4);
    if (worldAmp > 0.0) {
        vec3 key = floor(worldPos.xyz / tileSize);
        float n = fract(sin(dot(key, vec3(12.9898, 78.233, 45.164))) * 43758.5453);
        float offset = (n - 0.5) * worldAmp;
        worldPos.xyz += worldNormal * offset;
    }

    v_worldPos = worldPos.xyz;
    v_normal = worldNormal;

    // Keep existing screen-space jitter control for optional subtle wobble (px == 0 disables)
    vec4 clip = mul(u_viewProj, worldPos);
    float px = max(u_psxParams.x, 0.0); // pixels
    if (px > 0.0) {
        // project to ndc
        vec2 ndc = clip.xy / max(clip.w, 1e-6);
        // assume 1080p-ish scaling; bgfx doesn't expose resolution here, so leave in NDC units via a heuristic
        float step = max(px / 540.0, 1e-6);
        ndc = floor(ndc / step + 0.5) * step;
        clip.xy = ndc * clip.w;
    }

    v_texcoord0.xy = a_texcoord0.xy;
    v_viewDir = normalize(u_cameraPos.xyz - worldPos.xyz);
    gl_Position = clip;
}

