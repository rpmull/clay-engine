#include "bgfx_compute.sh"

#ifndef CM_DEBUG_GRASS
#define CM_DEBUG_GRASS 0
#endif

// Counter as 1x1 R32U texture for atomic operations
IMAGE2D_RW(u_CounterTexture, r32ui, 1);
// Use uint buffer for indexed draw indirect args (5 values):
// [0] indexCount, [1] instanceCount, [2] firstIndex, [3] baseVertex, [4] baseInstance
BUFFER_RW(u_IndirectBuffer, uint, 2);

uniform vec4 u_GrassDrawArgs; // x = index count, y = capacity
#if CM_DEBUG_GRASS
IMAGE2D_RW(u_DebugTexture, rgba32f, 3);
uniform vec4 u_GrassDebugInfo; // x=index, y=width, z=enabled
#endif

NUM_THREADS(1, 1, 1)

void main()
{
    // Read counter from texture
    uint instanceCount = imageLoad(u_CounterTexture, ivec2(0, 0)).x;
    const uint capacity = uint(u_GrassDrawArgs.y + 0.5);
    if (instanceCount > capacity)
    {
        instanceCount = capacity;
    }

    // Write indexed indirect draw args (DrawElementsIndirectCommand):
    u_IndirectBuffer[0] = uint(u_GrassDrawArgs.x + 0.5); // indexCount (6 for billboard)
    u_IndirectBuffer[1] = instanceCount;                  // instanceCount
    u_IndirectBuffer[2] = 0u;                             // firstIndex
    u_IndirectBuffer[3] = 0u;                             // baseVertex (signed int, but 0 is safe)
    u_IndirectBuffer[4] = 0u;                             // baseInstance

#if CM_DEBUG_GRASS
    if (u_GrassDebugInfo.z > 0.5)
    {
        const int rawIndex = int(u_GrassDebugInfo.x + 0.5);
        const int width = max(1, int(u_GrassDebugInfo.y + 0.5));
        if (rawIndex >= 0)
        {
            ivec2 coord = ivec2(rawIndex % width, rawIndex / width);
            imageStore(u_DebugTexture, coord, vec4(float(instanceCount), float(capacity), 0.0, 0.0));
        }
    }
#endif
}

