$input v_worldPos, v_normal

#include <bgfx_shader.sh>

void main()
{
	// Also write linearized debug color (R8 target attached in shadow pass).
	// Depth test/write still uses hardware depth buffer.
	float d = clamp(gl_FragCoord.z, 0.0, 1.0);
	gl_FragColor = vec4(d, d, d, 1.0);
}


