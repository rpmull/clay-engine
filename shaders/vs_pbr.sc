$input a_position, a_normal, a_texcoord0
$output v_worldPos, v_normal, v_texcoord0, v_viewDir

#include <bgfx_shader.sh>

uniform vec4 u_cameraPos;
uniform vec4 u_UVTransform;
uniform vec4 u_DisplacementParams; // x=scale, y=bias
uniform mat4 u_normalMat; // transpose(inverse(mat3(model)))

SAMPLER2D(s_displacement, 6);

void main()
{
    vec4 worldPos = mul(u_model[0], vec4(a_position, 1.0)); // Object to World
    vec3 worldNormal = normalize(mul((mat3)u_normalMat, a_normal));

    vec2 scaledUV = a_texcoord0.xy * u_UVTransform.xy + u_UVTransform.zw;
    v_texcoord0.xy = scaledUV;
    v_texcoord0.zw = a_texcoord0.zw;

    if (abs(u_DisplacementParams.x) > 1e-6) {
        float h = texture2DLod(s_displacement, scaledUV, 0.0).r;
        float disp = (h - u_DisplacementParams.y) * u_DisplacementParams.x;
        worldPos.xyz += worldNormal * disp;
    }

    v_worldPos = worldPos.xyz;
    v_normal = worldNormal;
    v_viewDir  = normalize(u_cameraPos.xyz - worldPos.xyz);
    gl_Position = mul(u_viewProj, worldPos);  

}
