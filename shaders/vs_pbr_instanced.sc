$input a_position, a_normal, a_texcoord0, i_data0, i_data1, i_data2, i_data3
$output v_worldPos, v_normal, v_texcoord0, v_viewDir

#include <bgfx_shader.sh>

uniform vec4 u_cameraPos;
uniform vec4 u_UVTransform;
uniform vec4 u_DisplacementParams; // x=scale, y=bias

SAMPLER2D(s_displacement, 6);

void main()
{
    // Instance data: i_data0..i_data3 are the 4 COLUMNS of the model matrix (from glm column-major)
    // We use them as basis vectors like the grass shader does, NOT as matrix rows
    vec3 basisX = i_data0.xyz;  // Column 0: X axis
    vec3 basisY = i_data1.xyz;  // Column 1: Y axis  
    vec3 basisZ = i_data2.xyz;  // Column 2: Z axis
    vec3 translation = i_data3.xyz;  // Column 3: translation
    
    // Transform position manually using columns as basis vectors
    // worldPos = translation + basisX*pos.x + basisY*pos.y + basisZ*pos.z
    vec3 worldPos = translation
                  + basisX * a_position.x
                  + basisY * a_position.y
                  + basisZ * a_position.z;
    
    // Transform normal with inverse-transpose of per-instance basis (adjugate/determinant).
    // This preserves correct lighting under non-uniform and mirrored instance scales.
    vec3 c0 = cross(basisY, basisZ);
    vec3 c1 = cross(basisZ, basisX);
    vec3 c2 = cross(basisX, basisY);
    float det = dot(basisX, c0);
    float invDet = (abs(det) > 1e-8) ? (1.0 / det) : 1.0;
    vec3 worldNormal = normalize(
        (c0 * a_normal.x + c1 * a_normal.y + c2 * a_normal.z) * invDet
    );
    vec2 scaledUV = a_texcoord0.xy * u_UVTransform.xy + u_UVTransform.zw;
    v_texcoord0.xy = scaledUV;
    v_texcoord0.zw = vec2(0.0, 0.0);
    
    if (abs(u_DisplacementParams.x) > 1e-6) {
        float h = texture2DLod(s_displacement, scaledUV, 0.0).r;
        float disp = (h - u_DisplacementParams.y) * u_DisplacementParams.x;
        worldPos += worldNormal * disp;
    }

    v_worldPos = worldPos;
    v_normal = worldNormal;
    
    v_viewDir = normalize(u_cameraPos.xyz - worldPos);
    
    gl_Position = mul(u_viewProj, vec4(worldPos, 1.0));
}

