$input  a_position, a_normal, a_indices, a_weight
$output v_worldPos, v_normal

#include <bgfx_shader.sh>
#include "lib_skinning.sh"

uniform vec4 u_psxWorld;  // x=vertexWorldAmp (meters), y=tileSize (meters), z=unused, w=unused
uniform mat4 u_lightViewProj;

void main()
{
	ivec4 idx = ivec4(a_indices);
	vec4  w   = a_weight;
	float s = max(w.x + w.y + w.z + w.w, 1e-6);
	w /= s;
	mat4 skin = ComputeSkinMatrix(idx, w);
	vec4 worldPos = mul(mul(u_model[0], skin), vec4(a_position, 1.0));
	vec3 worldNormal = normalize(mul((mat3)u_model[0], mul((mat3)skin, a_normal)));

	// Mirror planar-preserving world-space offset used in color pass
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
	gl_Position = mul(u_lightViewProj, worldPos);
}


