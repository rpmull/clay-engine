#pragma once

#include <string>
#include <memory>
#include <cstdint>
#include <vector>
#include "core/navigation/NavTypes.h"

namespace nav { class NavMeshRuntime; }

namespace nav::io
{
    // Version history:
    // v1: Basic format (deprecated)
    // v2: Pre-built BVH for instant loading
    // v3: LZ4 compression + memory-mapped loading support
    // v4: Per-poly costs + area costs chunk
    // v5: Native Detour nav data chunk
    static constexpr uint32_t NAVBIN_MAGIC = 'B' | ('V'<<8) | ('A'<<16) | ('N'<<24); // 'NAVB' little-endian
    static constexpr uint32_t NAVBIN_VERSION = 5;
    
    // Chunk flags
    static constexpr uint32_t CHUNK_FLAG_COMPRESSED = 0x80000000;
    
    // Load options for performance tuning
    struct LoadOptions {
        bool verifyCRC = false;       // Skip CRC by default for fast loading
        bool rebuildBVH = false;      // Use serialized BVH instead of rebuilding
        bool useMemoryMapping = true; // Use mmap for large files
    };
    
    // Write options
    struct WriteOptions {
        bool compress = false;        // LZ4 compression disabled (custom implementation has bugs)
        int compressionLevel = 1;     // 1 = fast, higher = better ratio
    };

    bool WriteNavbin(const NavMeshRuntime& rt, uint64_t bakeHash, const std::string& filePath, 
                     const WriteOptions& opts = {});
    bool ReadNavbin(const std::string& filePath, std::shared_ptr<NavMeshRuntime>& out, uint64_t& outHash, 
                    const LoadOptions& opts = {});
    
    // Memory-mapped fast loading (returns immediately, data accessed on-demand)
    bool ReadNavbinMapped(const std::string& filePath, std::shared_ptr<NavMeshRuntime>& out, uint64_t& outHash);

    // Load from memory buffer (for runtime PAK loading)
    bool ReadNavbinFromMemory(const uint8_t* data, size_t size, std::shared_ptr<NavMeshRuntime>& out, uint64_t& outHash,
                              const LoadOptions& opts = {});

    // Helper called by component EnsureRuntimeLoaded()
    bool LoadNavMeshFromFile(const std::string& path, std::shared_ptr<NavMeshRuntime>& out, uint64_t& outHash);
    bool LoadNavMeshFromMemory(const uint8_t* data, size_t size, std::shared_ptr<NavMeshRuntime>& out, uint64_t& outHash);
    bool BuildNavPayload(const NavMeshRuntime& rt, uint64_t bakeHash, std::vector<uint8_t>& outPayload);

    // --------------------------------------------------------------------
    // NavPack: scene-level container for chunked navmesh
    // --------------------------------------------------------------------
    static constexpr uint32_t NAVPACK_MAGIC = 'P' | ('V'<<8) | ('A'<<16) | ('N'<<24); // 'NAVP'
    static constexpr uint32_t NAVPACK_VERSION = 1;

    struct NavPackMeta {
        uint32_t chunksX = 0;
        uint32_t chunksZ = 0;
        uint64_t sceneGuidHigh = 0;
        uint64_t sceneGuidLow = 0;
        uint64_t bakeHash = 0;
    };

    struct NavPackChunk {
        int32_t gridX = 0;
        int32_t gridZ = 0;
        Bounds bounds;
        uint64_t offset = 0;
        uint64_t size = 0;
        uint64_t hash = 0;
    };

    bool WriteNavPack(const std::string& path, const NavPackMeta& meta,
                      const std::vector<NavPackChunk>& chunks,
                      const std::vector<std::vector<uint8_t>>& payloads);
    bool LoadNavPackIndex(const std::string& path, NavPackMeta& outMeta,
                          std::vector<NavPackChunk>& outChunks);
    bool ReadNavPackChunk(const std::string& path, const NavPackChunk& chunk,
                          std::shared_ptr<NavMeshRuntime>& out, uint64_t& outHash);
    bool UpsertNavPackChunk(const std::string& path, const NavPackMeta& meta,
                            const NavPackChunk& chunk, const std::vector<uint8_t>& payload);
    bool UpsertNavPackChunks(const std::string& path, const NavPackMeta& meta,
                             const std::vector<NavPackChunk>& chunks,
                             const std::vector<std::vector<uint8_t>>& payloads);
}


