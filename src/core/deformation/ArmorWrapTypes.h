#pragma once
#include <cstdint>

// ============================================================================
// Armor Wrap Deformation Types
// ============================================================================
// This header defines the binary-compatible struct layout for armor wrap data.
// The struct is used by both:
//   - EDITOR: to write .wrapbin files from imported JSON
//   - RUNTIME: to load .wrapbin directly into memory without parsing
//
// The layout is FIXED and must remain stable across versions.
// ============================================================================

namespace cm { namespace deformation {

// ============================================================================
// ArmorWrapInfluence - Native runtime struct (12 bytes, tightly packed)
// ============================================================================
// Each armor vertex has exactly one of these structs describing how it should
// follow the body mesh surface after body morphs are applied.
//
// The barycentric coordinates (w0, w1) are quantized to uint16 (0-65535 = 0.0-1.0).
// w2 is computed as 1.0 - w0 - w1 at runtime to avoid redundant storage.
//
// Layout:
//   [0-3]   triIndex    : Body triangle index
//   [4-5]   w0          : Barycentric weight for vertex 0 (quantized)
//   [6-7]   w1          : Barycentric weight for vertex 1 (quantized)
//   [8-9]   wrapWeight  : Blend strength 0-65535 = 0.0-1.0
//   [10-11] flags       : Bitfield for future use (rigid/soft/etc)
// ============================================================================

#pragma pack(push, 1)
struct ArmorWrapInfluence
{
    uint32_t triIndex;      // Body triangle index (into body index buffer / 3)
    uint16_t w0;            // Barycentric weight for first triangle vertex (0-65535)
    uint16_t w1;            // Barycentric weight for second triangle vertex (0-65535)
    uint16_t wrapWeight;    // Per-vertex wrap strength (0-65535 = 0.0-1.0)
    uint16_t flags;         // Bitfield: bit 0 = rigid (skip skinned offset), etc.
};
#pragma pack(pop)

static_assert(sizeof(ArmorWrapInfluence) == 12, "ArmorWrapInfluence must be exactly 12 bytes");

// Flag bits for ArmorWrapInfluence::flags
namespace WrapFlags {
    constexpr uint16_t None    = 0x0000;
    constexpr uint16_t Rigid   = 0x0001;  // Vertex follows body rigidly (no blend with skinned pos)
    constexpr uint16_t Soft    = 0x0002;  // Vertex uses softened falloff
    constexpr uint16_t NoWrap  = 0x0004;  // Skip wrap entirely for this vertex
}

// ============================================================================
// .wrapbin File Format
// ============================================================================
// The .wrapbin file is a minimal fixed-layout binary:
//
//   [Header]          (16 bytes)
//     magic           (4 bytes) : 'WRPB' (0x42505257)
//     version         (4 bytes) : Format version (currently 1)
//     vertexCount     (4 bytes) : Number of ArmorWrapInfluence entries
//     flags           (4 bytes) : Reserved for future use
//
//   [Data]            (vertexCount * 12 bytes)
//     ArmorWrapInfluence[vertexCount]
//
// Runtime can load this with a single fread() directly into an ArmorWrapInfluence*.
// ============================================================================

constexpr uint32_t WRAPBIN_MAGIC   = 'W' | ('R' << 8) | ('P' << 16) | ('B' << 24); // 'WRPB'
constexpr uint32_t WRAPBIN_VERSION = 1;

#pragma pack(push, 1)
struct WrapBinHeader
{
    uint32_t magic;         // Must be WRAPBIN_MAGIC
    uint32_t version;       // Must match WRAPBIN_VERSION
    uint32_t vertexCount;   // Number of ArmorWrapInfluence entries
    uint32_t flags;         // Reserved (set to 0)
};
#pragma pack(pop)

static_assert(sizeof(WrapBinHeader) == 16, "WrapBinHeader must be exactly 16 bytes");

// ============================================================================
// Quantization Utilities
// ============================================================================

inline uint16_t QuantizeWeight(float w)
{
    // Clamp to [0, 1] and scale to uint16
    if (w <= 0.0f) return 0;
    if (w >= 1.0f) return 65535;
    return static_cast<uint16_t>(w * 65535.0f + 0.5f);
}

inline float DequantizeWeight(uint16_t q)
{
    return static_cast<float>(q) / 65535.0f;
}

}} // namespace cm::deformation

