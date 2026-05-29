#include "bgfx_compute.sh"

// Counter as 1x1 R32U texture for atomic operations
IMAGE2D_RW(u_CounterTexture, r32ui, 1);
// Use uint buffer for indexed draw indirect args (5 values):
// [0] indexCount, [1] instanceCount, [2] firstIndex, [3] baseVertex, [4] baseInstance
BUFFER_RW(u_IndirectBuffer, uint, 2);

uniform vec4 u_GrassDrawArgs; // x = index count per instance, y = capacity

NUM_THREADS(1, 1, 1)

void main()
{
    // Reset counter to 0
    imageStore(u_CounterTexture, ivec2(0, 0), uvec4(0u, 0u, 0u, 0u));
    
    // Initialize indexed indirect draw args (DrawElementsIndirectCommand):
    u_IndirectBuffer[0] = uint(u_GrassDrawArgs.x + 0.5); // indexCount (6 for billboard)
    u_IndirectBuffer[1] = 0u;                             // instanceCount (will be set by finalize)
    u_IndirectBuffer[2] = 0u;                             // firstIndex
    u_IndirectBuffer[3] = 0u;                             // baseVertex
    u_IndirectBuffer[4] = 0u;                             // baseInstance
}

