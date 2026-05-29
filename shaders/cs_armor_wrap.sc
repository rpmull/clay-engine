// ============================================================================
// Armor Wrap Deformation Compute Shader
// ============================================================================
// Applies body-surface wrap deformation to armor mesh vertices.
// Each armor vertex is bound to a body triangle via barycentric coordinates.
//
// Input SSBOs:
//   - BodyPositions[]  : Final morphed/skinned body vertex positions
//   - BodyIndices[]    : Body mesh index buffer
//   - WrapInfluences[] : Per-armor-vertex wrap binding data
//   - ArmorSkinned[]   : Armor skinned positions (before wrap)
//
// Output SSBO:
//   - ArmorOutput[]    : Final armor positions (after wrap blend)
// ============================================================================

#include "bgfx_compute.sh"

// ============================================================================
// Struct matching ArmorWrapInfluence (12 bytes packed)
// ============================================================================
struct WrapInfluence
{
    uint triIndex;      // Body triangle index
    uint w0_w1;         // Packed: low 16 bits = w0, high 16 bits = w1
    uint wrapWeight_flags; // Packed: low 16 bits = wrapWeight, high 16 bits = flags
};

// Flag bits
#define FLAG_RIGID   0x0001
#define FLAG_SOFT    0x0002
#define FLAG_NO_WRAP 0x0004

// ============================================================================
// Buffer Bindings
// ============================================================================
#if BGFX_SHADER_LANGUAGE_HLSL
StructuredBuffer<float3> u_BodyPositions : REGISTER(t, 0);
StructuredBuffer<uint> u_BodyIndices : REGISTER(t, 1);
StructuredBuffer<WrapInfluence> u_WrapInfluences : REGISTER(t, 2);
StructuredBuffer<float3> u_ArmorSkinned : REGISTER(t, 3);
RWStructuredBuffer<float3> u_ArmorOutput : REGISTER(u, 0);
#else
BUFFER_RO(u_BodyPositions, vec4, 0);    // Actually vec3 but padded
BUFFER_RO(u_BodyIndices, uint, 1);
BUFFER_RO(u_WrapInfluences, vec4, 2);   // Reinterpreted as WrapInfluence
BUFFER_RO(u_ArmorSkinned, vec4, 3);     // Actually vec3 but padded
BUFFER_RW(u_ArmorOutput, vec4, 4);      // Actually vec3 but padded
#endif

// ============================================================================
// Uniforms
// ============================================================================
uniform vec4 u_WrapParams;  // x = globalWrapWeight, y = vertexCount, z/w = reserved

// ============================================================================
// Helper: Dequantize uint16 weight to float
// ============================================================================
float DequantizeWeight(uint quantized)
{
    return float(quantized) / 65535.0;
}

// ============================================================================
// Helper: Unpack WrapInfluence from buffer
// ============================================================================
#if !BGFX_SHADER_LANGUAGE_HLSL
WrapInfluence UnpackInfluence(uint index)
{
    // Each WrapInfluence is 12 bytes = 3 uints
    // Buffer is vec4 so we need to compute proper offset
    uint baseIdx = index * 3u;
    uint word0 = floatBitsToUint(u_WrapInfluences[baseIdx / 4u][baseIdx % 4u]);
    uint word1 = floatBitsToUint(u_WrapInfluences[(baseIdx + 1u) / 4u][(baseIdx + 1u) % 4u]);
    uint word2 = floatBitsToUint(u_WrapInfluences[(baseIdx + 2u) / 4u][(baseIdx + 2u) % 4u]);
    
    WrapInfluence w;
    w.triIndex = word0;
    w.w0_w1 = word1;
    w.wrapWeight_flags = word2;
    return w;
}
#endif

// ============================================================================
// Main Compute Kernel
// ============================================================================
NUM_THREADS(64, 1, 1)
void main()
{
    uint vertexId = gl_GlobalInvocationID.x;
    uint vertexCount = uint(u_WrapParams.y + 0.5);
    
    if (vertexId >= vertexCount)
        return;
    
    // Get wrap influence for this vertex
#if BGFX_SHADER_LANGUAGE_HLSL
    WrapInfluence w = u_WrapInfluences[vertexId];
#else
    WrapInfluence w = UnpackInfluence(vertexId);
#endif
    
    // Extract packed values
    uint w0_quant = w.w0_w1 & 0xFFFFu;
    uint w1_quant = (w.w0_w1 >> 16u) & 0xFFFFu;
    uint wrapWeight_quant = w.wrapWeight_flags & 0xFFFFu;
    uint flags = (w.wrapWeight_flags >> 16u) & 0xFFFFu;
    
    // Get armor skinned position
#if BGFX_SHADER_LANGUAGE_HLSL
    float3 armorSkinned = u_ArmorSkinned[vertexId];
#else
    vec3 armorSkinned = u_ArmorSkinned[vertexId].xyz;
#endif
    
    // Skip if no wrap flag set
    if ((flags & FLAG_NO_WRAP) != 0u)
    {
#if BGFX_SHADER_LANGUAGE_HLSL
        u_ArmorOutput[vertexId] = armorSkinned;
#else
        u_ArmorOutput[vertexId] = vec4(armorSkinned, 0.0);
#endif
        return;
    }
    
    // Get body triangle indices
    uint tri = w.triIndex;
#if BGFX_SHADER_LANGUAGE_HLSL
    uint i0 = u_BodyIndices[tri * 3u + 0u];
    uint i1 = u_BodyIndices[tri * 3u + 1u];
    uint i2 = u_BodyIndices[tri * 3u + 2u];
#else
    uint i0 = u_BodyIndices[tri * 3u + 0u];
    uint i1 = u_BodyIndices[tri * 3u + 1u];
    uint i2 = u_BodyIndices[tri * 3u + 2u];
#endif
    
    // Get body triangle vertex positions
#if BGFX_SHADER_LANGUAGE_HLSL
    float3 a = u_BodyPositions[i0];
    float3 b = u_BodyPositions[i1];
    float3 c = u_BodyPositions[i2];
#else
    vec3 a = u_BodyPositions[i0].xyz;
    vec3 b = u_BodyPositions[i1].xyz;
    vec3 c = u_BodyPositions[i2].xyz;
#endif
    
    // Dequantize barycentric weights
    float fw0 = DequantizeWeight(w0_quant);
    float fw1 = DequantizeWeight(w1_quant);
    float fw2 = 1.0 - fw0 - fw1;
    
    // Compute wrapped position on body surface
#if BGFX_SHADER_LANGUAGE_HLSL
    float3 wrapped = a * fw0 + b * fw1 + c * fw2;
#else
    vec3 wrapped = a * fw0 + b * fw1 + c * fw2;
#endif
    
    // Compute final blend weight
    float globalWeight = u_WrapParams.x;
    float perVertexWeight = DequantizeWeight(wrapWeight_quant);
    float totalWeight = globalWeight * perVertexWeight;
    
    // Compute final position
#if BGFX_SHADER_LANGUAGE_HLSL
    float3 finalPos;
#else
    vec3 finalPos;
#endif
    
    if ((flags & FLAG_RIGID) != 0u)
    {
        // Rigid: use wrapped position directly
        finalPos = wrapped;
    }
    else
    {
        // Blend between skinned and wrapped
        finalPos = mix(armorSkinned, wrapped, totalWeight);
    }
    
    // Write output
#if BGFX_SHADER_LANGUAGE_HLSL
    u_ArmorOutput[vertexId] = finalPos;
#else
    u_ArmorOutput[vertexId] = vec4(finalPos, 0.0);
#endif
}

