$input a_position, a_normal, a_texcoord0, i_data0, i_data1, i_data2, i_data3
$output v_worldPos, v_normal, v_texcoord0

#include <bgfx_shader.sh>

uniform mat4 u_lightViewProj;
uniform vec4 u_UVTransform;

void main()
{
    vec3 basisX = i_data0.xyz;
    vec3 basisY = i_data1.xyz;
    vec3 basisZ = i_data2.xyz;
    vec3 translation = i_data3.xyz;

    vec3 worldPos = translation
                  + basisX * a_position.x
                  + basisY * a_position.y
                  + basisZ * a_position.z;

    vec3 c0 = cross(basisY, basisZ);
    vec3 c1 = cross(basisZ, basisX);
    vec3 c2 = cross(basisX, basisY);
    float det = dot(basisX, c0);
    float invDet = (abs(det) > 1e-8) ? (1.0 / det) : 1.0;
    vec3 worldNormal = normalize(
        (c0 * a_normal.x + c1 * a_normal.y + c2 * a_normal.z) * invDet
    );

    v_worldPos = worldPos;
    v_normal = worldNormal;
    v_texcoord0.xy = a_texcoord0.xy * u_UVTransform.xy + u_UVTransform.zw;
    v_texcoord0.zw = vec2(0.0, 0.0);
    gl_Position = mul(u_lightViewProj, vec4(worldPos, 1.0));
}
