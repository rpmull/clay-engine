#ifndef CLAYMORE_LIB_SHADERGRAPH_SH
#define CLAYMORE_LIB_SHADERGRAPH_SH

// Helpers shared by generated shader-graph fragment shaders.
// Kept dependency-free so it can be included by any generated graph.

// RGB <-> HSV conversions (Sam Hocevar / Inigo Quilez, branch-free).
vec3 sg_rgb2hsv(vec3 c)
{
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

vec3 sg_hsv2rgb(vec3 c)
{
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

// Interpolate hue (0..1, wrapping) honouring a Blender-style hue path:
// mode 0 = Near (shortest), 1 = Far (longest), 2 = CW, 3 = CCW.
float sg_mixHue(float a, float b, float t, int mode)
{
    float diff = b - a;
    if (mode == 0) {          // Near
        if (diff > 0.5)  diff -= 1.0;
        if (diff < -0.5) diff += 1.0;
    } else if (mode == 1) {   // Far
        if (diff > 0.0 && diff < 0.5) diff -= 1.0;
        if (diff < 0.0 && diff > -0.5) diff += 1.0;
    } else if (mode == 2) {   // Clockwise (decreasing hue)
        if (diff > 0.0) diff -= 1.0;
    } else {                  // Counter-clockwise (increasing hue)
        if (diff < 0.0) diff += 1.0;
    }
    return fract(a + diff * t);
}

// Interpolate two RGB colours through HSV space (used by ColorRamp HSV mode).
vec3 sg_mixHSV(vec3 ca, vec3 cb, float t, int hueMode)
{
    vec3 a = sg_rgb2hsv(ca);
    vec3 b = sg_rgb2hsv(cb);
    float h = sg_mixHue(a.x, b.x, t, hueMode);
    float s = mix(a.y, b.y, t);
    float v = mix(a.z, b.z, t);
    return sg_hsv2rgb(vec3(h, s, v));
}

#endif // CLAYMORE_LIB_SHADERGRAPH_SH
