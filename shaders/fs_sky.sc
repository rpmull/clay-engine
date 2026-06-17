$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLERCUBE(s_skybox, 0);


// u_skyParams.x = use procedural sky
// u_skyParams.y = use cubemap skybox
// u_skyParams.z = max cubemap mip level
// u_skyParams.w = apply gamma (linear -> sRGB) on output
uniform vec4 u_skyParams;

uniform vec4 u_cameraPos;

uniform vec4 u_skyTopColor;       // rgb = top/zenith color
uniform vec4 u_skyHorizonColor;   // rgb = horizon band color
uniform vec4 u_groundColor;       // rgb = ground/under-hemisphere color

uniform vec4 u_sunDirection;      // xyz = sun direction (world space, normalized)
uniform vec4 u_skySunParams;      // x = sun size slider [0..1], y = convergence, z = intensity
uniform vec4 u_skyAtmosphereParams; // x = thickness, y = horizon fade, z = exposure

float saturate(float v) { return clamp(v, 0.0, 1.0); }

// Simple linear → sRGB for optional gamma encode on final color.
vec3 linearToSrgb(vec3 c)
{
    vec3 r;
    r.x = (c.x <= 0.0031308) ? c.x * 12.92 : 1.055 * pow(c.x, 1.0 / 2.4) - 0.055;
    r.y = (c.y <= 0.0031308) ? c.y * 12.92 : 1.055 * pow(c.y, 1.0 / 2.4) - 0.055;
    r.z = (c.z <= 0.0031308) ? c.z * 12.92 : 1.055 * pow(c.z, 1.0 / 2.4) - 0.055;
    return r;
}

// Rebuild a world-space view direction from clip-space XY and u_invViewProj/u_cameraPos.
vec3 reconstructViewDir(vec2 clipXY)
{
    // v_texcoord0.xy is already in clip space [-1,1].
    vec4 clip = vec4(clipXY, 1.0, 1.0);
    vec4 world = mul(u_invViewProj, clip);

    // Homogeneous divide to get a world-space position on the far plane.
    float w = max(world.w, 1e-6);
    vec3 worldPos = world.xyz / w;

    // Ray from camera position toward that world point.
    return normalize(worldPos - u_cameraPos.xyz);
}

// Hemispherical procedural sky: top / horizon / ground + sun disk.
vec3 EvaluateProceduralSky(vec3 viewDir)
{
    vec3 topColor     = u_skyTopColor.rgb;
    vec3 horizonColor = u_skyHorizonColor.rgb;
    vec3 groundColor  = u_groundColor.rgb;

    // y = -1 bottom, 0 horizon, +1 zenith
    float y = clamp(viewDir.y, -1.0, 1.0);
    float up   = saturate( y);   // 0..1 above horizon
    float down = saturate(-y);   // 0..1 below horizon

    // Simple, clean hemispheres:
    //  - Above horizon: Horizon -> Top
    //  - Below horizon: Ground  -> Horizon
    vec3 upper = mix(horizonColor, topColor,   up);
    vec3 lower = mix(groundColor,  horizonColor, down);

    vec3 sky = (y >= 0.0) ? upper : lower;

    // --- NARROW horizon band ---
    // We want a thin ring where HorizonColor dominates, not a big wash.
    float a = abs(y);          // 0 at horizon, 1 at poles
    float inner = 0.02;        // start of band (very close to horizon)
    float outer = 0.08;        // end of band (thickness)
    float band = smoothstep(outer, inner, a);  // 1 at y≈0, 0 past outer

    float horizonFade = clamp(u_skyAtmosphereParams.y, 0.0, 1.0);
    sky = mix(sky, horizonColor, band * horizonFade);

    // --- (OPTIONAL) paste your existing sun code here, BEFORE exposure ---
    // sky += ... sun contribution ...

    // Exposure (keep simple for now)
    float exposure = max(u_skyAtmosphereParams.z, 0.0);
    sky *= exposure;

    return max(sky, vec3(0.0,0.0,0.0));
}


void main()
{
    vec3 viewDir = reconstructViewDir(v_texcoord0.xy);
    vec3 baseColor = vec3(0.0,0.0,0.0);

    // Sample cubemap sky, procedural sky, or discard if nothing is active.
    if (u_skyParams.y > 0.5)
    {
        baseColor = textureCube(s_skybox, viewDir).rgb;
    }
    else if (u_skyParams.x > 0.5)
    {
        baseColor = EvaluateProceduralSky(viewDir);
    }
    else
    {
        discard;
    }

    // Optional gamma encode to sRGB if requested.
    if (u_skyParams.w > 0.5)
    {
        baseColor = linearToSrgb(baseColor);
    }

    gl_FragColor = vec4(baseColor, 1.0);
}
