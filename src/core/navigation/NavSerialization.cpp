#include "core/navigation/NavSerialization.h"
#include "core/navigation/NavMesh.h"
#include "core/utils/LZ4.h"
#include "core/utils/MemoryMappedFile.h"
#include "core/vfs/FileSystem.h"
#include <fstream>
#include <iostream>
#include <vector>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <filesystem>

using namespace nav;
using namespace nav::io;

namespace {
    // Lightweight CRC32 (IEEE 802.3) implementation
    static uint32_t crc32_table[256];
    static bool crc32_init = false;
    static void init_crc32() {
        if (crc32_init) return;
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            crc32_table[i] = c;
        }
        crc32_init = true;
    }
    static uint32_t crc32_buf(const uint8_t* data, size_t len) {
        init_crc32();
        uint32_t c = 0xFFFFFFFFu;
        for (size_t i = 0; i < len; ++i) c = crc32_table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }
}

// ============================================================================
// Version 3 Layout (with LZ4 compression):
// ============================================================================
// [magic u32][version u32][flags u32][uncompressedSize u64]
// [compressedPayload...]
// [CRC32 u32] (of everything before CRC)
//
// The payload (uncompressed) contains:
// - INFO: bounds, counts
// - VERT: vertices (bulk memcpy)
// - POLY: polygons (bulk memcpy)  
// - ACST: area costs (64 floats)
// - PCST: per-poly costs (float per polygon)
// - LINK: off-mesh links
// - BVHN: BVH nodes (bulk memcpy)
// - BVHI: BVH indices (bulk memcpy)
// - HASH: bake hash
//
// Key optimizations:
// - LZ4 compression typically achieves 2-4x compression on navmesh data
// - Memory-mapped loading for large files
// - Bulk memcpy for all arrays (no per-element parsing)
// - Optional CRC verification (off by default)
// ============================================================================

// File flags
constexpr uint32_t FLAG_COMPRESSED = 0x1;

// Chunk IDs (little-endian)
constexpr uint32_t CHUNK_INFO = 'OFNI';
constexpr uint32_t CHUNK_VERT = 'TREV';
constexpr uint32_t CHUNK_POLY = 'YLOP';
constexpr uint32_t CHUNK_LINK = 'KNIL';
constexpr uint32_t CHUNK_BVHN = 'NHVB';
constexpr uint32_t CHUNK_BVHI = 'IHVB';
constexpr uint32_t CHUNK_ADJC = 'CJDA';  // Adjacency data for pathfinding
constexpr uint32_t CHUNK_ACST = 'TSCA';  // Area costs (64 floats)
constexpr uint32_t CHUNK_PCST = 'TSCP';  // Per-poly costs (float per poly)
constexpr uint32_t CHUNK_DTNM = 'MNTD';  // Native Detour navmesh data blob
constexpr uint32_t CHUNK_HASH = 'HSAH';
constexpr uint32_t CHUNK_CRC  = 'CRCC';

struct ChunkHeader { uint32_t id; uint32_t size; };

// Helper functions for writing
static void write_u32(std::vector<uint8_t>& buf, uint32_t v) { 
    size_t o = buf.size(); buf.resize(o+4); memcpy(buf.data()+o, &v, 4);
}
static void write_u64(std::vector<uint8_t>& buf, uint64_t v) { 
    size_t o = buf.size(); buf.resize(o+8); memcpy(buf.data()+o, &v, 8);
}
static void write_f32(std::vector<uint8_t>& buf, float v) { 
    size_t o = buf.size(); buf.resize(o+4); memcpy(buf.data()+o, &v, 4);
}
static void write_vec3(std::vector<uint8_t>& buf, const glm::vec3& v) { 
    write_f32(buf, v.x); write_f32(buf, v.y); write_f32(buf, v.z);
}
static void write_bulk(std::vector<uint8_t>& buf, const void* data, size_t size) {
    size_t o = buf.size(); buf.resize(o + size); memcpy(buf.data() + o, data, size);
}
static bool read_file(std::istream& f, void* dst, size_t size) {
    f.read(reinterpret_cast<char*>(dst), size);
    return f.good();
}
static bool write_file(std::ostream& f, const void* src, size_t size) {
    if (size == 0) return true;
    f.write(reinterpret_cast<const char*>(src), static_cast<std::streamsize>(size));
    return f.good();
}

// Build uncompressed payload
static std::vector<uint8_t> BuildPayload(const NavMeshRuntime& rt, uint64_t bakeHash) {
    std::vector<uint8_t> buf;
    buf.reserve(
        64 +                                          // INFO
        rt.m_Vertices.size() * 12 +                  // VERT
        rt.m_Polys.size() * 16 +                     // POLY
        sizeof(float) * 64 + 8 +                    // ACST
        rt.m_Polys.size() * sizeof(float) + 8 +     // PCST
        rt.m_Links.size() * 36 +                     // LINK
        rt.m_BVH.size() * 40 + 8 +                   // BVHN
        rt.m_BVHIndices.size() * 4 + 8 +             // BVHI
        16                                            // HASH
    );
    
    // INFO chunk
    {
        size_t at = buf.size();
        write_u32(buf, CHUNK_INFO); write_u32(buf, 0); // placeholder size
        write_vec3(buf, rt.m_Bounds.min);
        write_vec3(buf, rt.m_Bounds.max);
        write_u32(buf, (uint32_t)rt.m_Vertices.size());
        write_u32(buf, (uint32_t)rt.m_Polys.size());
        write_u32(buf, (uint32_t)rt.m_Links.size());
        write_u32(buf, (uint32_t)rt.m_BVH.size());
        write_u32(buf, (uint32_t)rt.m_BVHIndices.size());
        uint32_t sz = (uint32_t)(buf.size() - at - 8);
        memcpy(buf.data() + at + 4, &sz, 4);
    }
    
    // VERT chunk - bulk write
    {
        size_t at = buf.size();
        write_u32(buf, CHUNK_VERT); write_u32(buf, 0);
        size_t dataSize = rt.m_Vertices.size() * sizeof(glm::vec3);
        write_bulk(buf, rt.m_Vertices.data(), dataSize);
        uint32_t sz = (uint32_t)(buf.size() - at - 8);
        memcpy(buf.data() + at + 4, &sz, 4);
    }
    
    // POLY chunk - need to pack area/flags
    {
        size_t at = buf.size();
        write_u32(buf, CHUNK_POLY); write_u32(buf, 0);
        for (const auto& p : rt.m_Polys) {
            write_u32(buf, p.i0);
            write_u32(buf, p.i1);
            write_u32(buf, p.i2);
            write_u32(buf, (uint32_t)p.area | (p.flags << 16));
        }
        uint32_t sz = (uint32_t)(buf.size() - at - 8);
        memcpy(buf.data() + at + 4, &sz, 4);
    }
    
    // ACST chunk (area costs)
    {
        size_t at = buf.size();
        write_u32(buf, CHUNK_ACST); write_u32(buf, 0);
        write_bulk(buf, rt.m_AreaCost.data(), sizeof(float) * rt.m_AreaCost.size());
        uint32_t sz = (uint32_t)(buf.size() - at - 8);
        memcpy(buf.data() + at + 4, &sz, 4);
    }

    // PCST chunk (per-poly costs)
    {
        size_t at = buf.size();
        write_u32(buf, CHUNK_PCST); write_u32(buf, 0);
        for (const auto& p : rt.m_Polys) {
            write_f32(buf, p.cost);
        }
        uint32_t sz = (uint32_t)(buf.size() - at - 8);
        memcpy(buf.data() + at + 4, &sz, 4);
    }

    // LINK chunk
    {
        size_t at = buf.size();
        write_u32(buf, CHUNK_LINK); write_u32(buf, 0);
        for (const auto& l : rt.m_Links) {
            write_vec3(buf, l.a);
            write_vec3(buf, l.b);
            write_f32(buf, l.radius);
            write_u32(buf, l.flags);
            write_u32(buf, (uint32_t)l.bidir);
        }
        uint32_t sz = (uint32_t)(buf.size() - at - 8);
        memcpy(buf.data() + at + 4, &sz, 4);
    }
    
    // BVHN chunk - bulk write (BVNode is POD)
    {
        size_t at = buf.size();
        write_u32(buf, CHUNK_BVHN); write_u32(buf, 0);
        // BVNode layout: Bounds(24) + left(4) + right(4) + start(4) + count(4) = 40 bytes
        // But Bounds contains 2 vec3s which are 12 bytes each = 24 bytes
        // So we write each node explicitly to ensure correct layout
        for (const auto& node : rt.m_BVH) {
            write_vec3(buf, node.b.min);
            write_vec3(buf, node.b.max);
            write_u32(buf, node.left);
            write_u32(buf, node.right);
            write_u32(buf, node.start);
            write_u32(buf, node.count);
        }
        uint32_t sz = (uint32_t)(buf.size() - at - 8);
        memcpy(buf.data() + at + 4, &sz, 4);
    }
    
    // BVHI chunk - bulk write
    {
        size_t at = buf.size();
        write_u32(buf, CHUNK_BVHI); write_u32(buf, 0);
        size_t dataSize = rt.m_BVHIndices.size() * sizeof(uint32_t);
        write_bulk(buf, rt.m_BVHIndices.data(), dataSize);
        uint32_t sz = (uint32_t)(buf.size() - at - 8);
        memcpy(buf.data() + at + 4, &sz, 4);
    }
    
    // ADJC chunk - adjacency data for pathfinding (CRITICAL for A*)
    {
        size_t at = buf.size();
        write_u32(buf, CHUNK_ADJC); write_u32(buf, 0);
        write_u32(buf, (uint32_t)rt.m_Adjacency.size());
        for (const auto& neighbors : rt.m_Adjacency) {
            write_u32(buf, (uint32_t)neighbors.size());
            for (uint32_t n : neighbors) {
                write_u32(buf, n);
            }
        }
        uint32_t sz = (uint32_t)(buf.size() - at - 8);
        memcpy(buf.data() + at + 4, &sz, 4);
    }
    
    // HASH chunk
    {
        write_u32(buf, CHUNK_HASH); write_u32(buf, 8);
        write_u64(buf, bakeHash);
    }

    // DTNM chunk (native Detour navmesh bytes)
    if (!rt.GetDetourNavData().empty()) {
        size_t at = buf.size();
        write_u32(buf, CHUNK_DTNM); write_u32(buf, 0);
        write_bulk(buf, rt.GetDetourNavData().data(), rt.GetDetourNavData().size());
        uint32_t sz = (uint32_t)(buf.size() - at - 8);
        memcpy(buf.data() + at + 4, &sz, 4);
    }
    
    return buf;
}

// Forward declaration
static bool ReadNavbinLegacy(const uint8_t* data, size_t size, 
                              std::shared_ptr<NavMeshRuntime>& out, uint64_t& outHash,
                              const LoadOptions& opts);

// Parse payload into runtime
static bool ParsePayload(const uint8_t* data, size_t size, NavMeshRuntime& rt, uint64_t& outHash, bool& hasBVH) {
    size_t p = 0;
    hasBVH = false;
    bool hasPolyCost = false;
    std::vector<uint8_t> detourData;
    
    while (p + 8 <= size) {
        uint32_t id, csz;
        memcpy(&id, data + p, 4);
        memcpy(&csz, data + p + 4, 4);
        p += 8;
        
        if (p + csz > size) return false;
        const uint8_t* chunk = data + p;
        
        switch (id) {
            case CHUNK_INFO: {
                size_t q = 0;
                rt.m_Bounds.min = *(const glm::vec3*)(chunk + q); q += 12;
                rt.m_Bounds.max = *(const glm::vec3*)(chunk + q); q += 12;
                // Counts stored for reference but we use chunk sizes
                break;
            }
            case CHUNK_VERT: {
                size_t n = csz / sizeof(glm::vec3);
                rt.m_Vertices.resize(n);
                memcpy(rt.m_Vertices.data(), chunk, csz);
                break;
            }
            case CHUNK_POLY: {
                size_t stride = 16;
                size_t n = csz / stride;
                rt.m_Polys.resize(n);
                for (size_t i = 0; i < n; ++i) {
                    const uint8_t* rec = chunk + i * stride;
                    auto& poly = rt.m_Polys[i];
                    memcpy(&poly.i0, rec + 0, 4);
                    memcpy(&poly.i1, rec + 4, 4);
                    memcpy(&poly.i2, rec + 8, 4);
                    uint32_t af;
                    memcpy(&af, rec + 12, 4);
                    poly.area = (uint16_t)(af & 0xFFFF);
                    poly.flags = (af >> 16);
                }
                break;
            }
            case CHUNK_LINK: {
                size_t stride = 36; // 2*vec3 + 3*u32
                size_t n = csz / stride;
                rt.m_Links.resize(n);
                const uint8_t* r = chunk;
                for (size_t i = 0; i < n; ++i) {
                    auto& l = rt.m_Links[i];
                    l.a = *(const glm::vec3*)r; r += 12;
                    l.b = *(const glm::vec3*)r; r += 12;
                    memcpy(&l.radius, r, 4); r += 4;
                    memcpy(&l.flags, r, 4); r += 4;
                    uint32_t bd; memcpy(&bd, r, 4); r += 4;
                    l.bidir = (uint8_t)bd;
                }
                break;
            }
            case CHUNK_PCST: {
                if (rt.m_Polys.empty()) break;
                size_t count = csz / sizeof(float);
                if (count > rt.m_Polys.size()) count = rt.m_Polys.size();
                const float* costs = reinterpret_cast<const float*>(chunk);
                for (size_t i = 0; i < count; ++i) {
                    rt.m_Polys[i].cost = std::max(costs[i], 0.01f);
                }
                hasPolyCost = true;
                break;
            }
            case CHUNK_ACST: {
                const size_t expectedBytes = sizeof(float) * rt.m_AreaCost.size();
                if (csz >= expectedBytes) {
                    memcpy(rt.m_AreaCost.data(), chunk, expectedBytes);
                } else {
                    // Backward-compatible partial chunk handling.
                    const size_t count = csz / sizeof(float);
                    if (count > 0) {
                        memcpy(rt.m_AreaCost.data(), chunk, count * sizeof(float));
                    }
                }
                break;
            }
            case CHUNK_BVHN: {
                size_t stride = 40; // 2*vec3 + 4*u32
                size_t n = csz / stride;
                rt.m_BVH.resize(n);
                const uint8_t* r = chunk;
                for (size_t i = 0; i < n; ++i) {
                    auto& node = rt.m_BVH[i];
                    node.b.min = *(const glm::vec3*)r; r += 12;
                    node.b.max = *(const glm::vec3*)r; r += 12;
                    memcpy(&node.left, r, 4); r += 4;
                    memcpy(&node.right, r, 4); r += 4;
                    memcpy(&node.start, r, 4); r += 4;
                    memcpy(&node.count, r, 4); r += 4;
                }
                hasBVH = true;
                break;
            }
            case CHUNK_BVHI: {
                size_t n = csz / sizeof(uint32_t);
                rt.m_BVHIndices.resize(n);
                memcpy(rt.m_BVHIndices.data(), chunk, csz);
                break;
            }
            case CHUNK_ADJC: {
                // Read adjacency data - critical for pathfinding
                const uint8_t* cur = chunk;
                uint32_t adjCount;
                memcpy(&adjCount, cur, 4); cur += 4;
                rt.m_Adjacency.resize(adjCount);
                for (uint32_t i = 0; i < adjCount && (size_t)(cur - chunk) < csz; ++i) {
                    uint32_t neighborCount;
                    memcpy(&neighborCount, cur, 4); cur += 4;
                    rt.m_Adjacency[i].resize(neighborCount);
                    for (uint32_t j = 0; j < neighborCount && (size_t)(cur - chunk) < csz; ++j) {
                        memcpy(&rt.m_Adjacency[i][j], cur, 4); cur += 4;
                    }
                }
                break;
            }
            case CHUNK_HASH: {
                memcpy(&outHash, chunk, 8);
                break;
            }
            case CHUNK_DTNM: {
                detourData.assign(chunk, chunk + csz);
                break;
            }
        }
        p += csz;
    }

    if (!hasPolyCost) {
        for (auto& p : rt.m_Polys) {
            float fallback = 1.0f;
            if (p.area < rt.m_AreaCost.size()) fallback = rt.m_AreaCost[p.area];
            p.cost = std::max(fallback, 0.01f);
        }
    }

    if (!detourData.empty()) {
        if (!rt.SetDetourNavData(detourData)) {
            std::cerr << "[Nav] ParsePayload: invalid Detour nav data chunk." << std::endl;
            return false;
        }
    }
    return true;
}

bool nav::io::WriteNavbin(const NavMeshRuntime& rt, uint64_t bakeHash, const std::string& filePath,
                          const WriteOptions& opts)
{
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // Validate data before serializing
    size_t vertCount = rt.m_Vertices.size();
    for (size_t i = 0; i < rt.m_Polys.size(); ++i) {
        const auto& p = rt.m_Polys[i];
        if (p.i0 >= vertCount || p.i1 >= vertCount || p.i2 >= vertCount) {
            std::cerr << "[Nav] WriteNavbin: Invalid polygon[" << i << "] with indices ("
                      << p.i0 << ", " << p.i1 << ", " << p.i2 << ") but only "
                      << vertCount << " vertices. Aborting save." << std::endl;
            return false;
        }
    }
    
    // Build payload
    std::vector<uint8_t> payload = BuildPayload(rt, bakeHash);
    size_t uncompressedSize = payload.size();
    
    // Compress if requested
    std::vector<uint8_t> compressed;
    bool useCompression = opts.compress && payload.size() > 1024; // Only compress if > 1KB
    
    if (useCompression) {
        compressed = lz4::CompressVec(payload);
        // Only use compression if it actually saves space
        if (compressed.empty() || compressed.size() >= payload.size() * 0.95f) {
            useCompression = false;
        }
    }
    
    // Build final file
    std::vector<uint8_t> file;
    file.reserve(20 + (useCompression ? compressed.size() : payload.size()) + 4);
    
    // Header
    write_u32(file, NAVBIN_MAGIC);
    write_u32(file, NAVBIN_VERSION);
    write_u32(file, useCompression ? FLAG_COMPRESSED : 0);
    write_u64(file, uncompressedSize);
    
    // Data
    if (useCompression) {
        write_bulk(file, compressed.data(), compressed.size());
    } else {
        write_bulk(file, payload.data(), payload.size());
    }
    
    // CRC footer
    uint32_t crc = crc32_buf(file.data(), file.size());
    write_u32(file, crc);
    
    // Ensure parent directory exists
    std::filesystem::path fp(filePath);
    if (fp.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(fp.parent_path(), ec);
        // Ignore errors - directory might already exist
    }
    
    // Write to disk
    std::ofstream f(filePath, std::ios::binary | std::ios::trunc);
    if (!f) {
        std::cerr << "[Nav] WriteNavbin: Failed to open file for writing: " << filePath << std::endl;
        return false;
    }
    f.write((const char*)file.data(), file.size());
    f.close();
    
    if (!f.good()) {
        std::cerr << "[Nav] WriteNavbin: Error writing to file: " << filePath << std::endl;
        return false;
    }
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    
    float ratio = useCompression ? (float)compressed.size() / payload.size() : 1.0f;
    std::cout << "[Nav] Saved navmesh: " << rt.m_Polys.size() << " tris, " 
              << (file.size() / 1024) << " KB" 
              << (useCompression ? " (LZ4 " + std::to_string((int)(ratio * 100)) + "%)" : "")
              << " in " << duration.count() << "ms" << std::endl;
    
    return true;
}

bool nav::io::ReadNavbin(const std::string& filePath, std::shared_ptr<NavMeshRuntime>& out, uint64_t& outHash,
                         const LoadOptions& opts)
{
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // Use memory mapping for larger files if enabled
    claymore::MemoryMappedFile mmf;
    std::vector<uint8_t> fileBuf;
    const uint8_t* fileData = nullptr;
    size_t fileSize = 0;
    
    if (opts.useMemoryMapping && !FileSystem::Instance().IsRuntimeMode()) {
        if (mmf.Open(filePath)) {
            fileData = mmf.Data();
            fileSize = mmf.Size();
        }
    }
    
    // Fallback to regular file read
    if (!fileData) {
        if (!FileSystem::Instance().ReadFile(filePath, fileBuf)) {
            std::cerr << "[Nav] ReadNavbin: Failed to read file: " << filePath << std::endl;
            return false;
        }
        fileData = fileBuf.data();
        fileSize = fileBuf.size();
    }
    
    // Validate header
    if (fileSize < 24) {
        std::cerr << "[Nav] ReadNavbin: File too small (" << fileSize << " bytes)" << std::endl;
        return false;
    }
    
    uint32_t magic, version, flags;
    uint64_t uncompressedSize;
    memcpy(&magic, fileData, 4);
    memcpy(&version, fileData + 4, 4);
    memcpy(&flags, fileData + 8, 4);
    memcpy(&uncompressedSize, fileData + 12, 8);
    
    if (magic != NAVBIN_MAGIC) {
        std::cerr << "[Nav] ReadNavbin: Bad magic (got 0x" << std::hex << magic 
                  << ", expected 0x" << NAVBIN_MAGIC << std::dec << ")" << std::endl;
        return false;
    }
    
    // Support current and prior versions.
    if (version != NAVBIN_VERSION && version != 4 && version != 3) {
        std::cerr << "[Nav] ReadNavbin: Unsupported version " << version 
                  << " (expected " << NAVBIN_VERSION << "). Please rebake the navmesh." << std::endl;
        return false;
    }
    
    std::cout << "[Nav] ReadNavbin: v" << version << " flags=0x" << std::hex << flags 
              << std::dec << " uncompSize=" << uncompressedSize << " fileSize=" << fileSize << std::endl;
    
    // CRC check (optional)
    if (opts.verifyCRC) {
        uint32_t storedCRC, calcCRC;
        memcpy(&storedCRC, fileData + fileSize - 4, 4);
        calcCRC = crc32_buf(fileData, fileSize - 4);
        if (storedCRC != calcCRC) {
            std::cerr << "[Nav] ReadNavbin: CRC mismatch (stored=0x" << std::hex << storedCRC 
                      << " calculated=0x" << calcCRC << std::dec << ")" << std::endl;
            return false;
        }
    }
    
    // Extract payload
    const uint8_t* payloadData = fileData + 20;
    size_t payloadSize = fileSize - 20 - 4; // Header + CRC
    
    std::vector<uint8_t> decompressed;
    if (flags & FLAG_COMPRESSED) {
        decompressed.resize((size_t)uncompressedSize);
        size_t decSize = lz4::Decompress(payloadData, payloadSize, decompressed.data(), decompressed.size());
        if (decSize != uncompressedSize) {
            std::cerr << "[Nav] ReadNavbin: LZ4 decompression failed (got " << decSize 
                      << ", expected " << uncompressedSize << ")" << std::endl;
            return false;
        }
        payloadData = decompressed.data();
        payloadSize = decompressed.size();
    }
    
    // Parse payload
    auto rt = std::make_shared<NavMeshRuntime>();
    bool hasBVH = false;
    outHash = 0;
    
    if (!ParsePayload(payloadData, payloadSize, *rt, outHash, hasBVH)) {
        std::cerr << "[Nav] ReadNavbin: Failed to parse payload (" << payloadSize << " bytes)" << std::endl;
        return false;
    }
    
    // Rebuild BVH only if not present or explicitly requested
    if (!hasBVH || opts.rebuildBVH) {
        rt->RebuildBVH();
    }
    
    out = std::move(rt);
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    std::cout << "[Nav] Loaded navmesh: " << out->m_Polys.size() << " tris in " << duration.count() << "ms" << std::endl;
    
    return true;
}

// Legacy reader for v1/v2 files
static bool ReadNavbinLegacy(const uint8_t* data, size_t size, 
                              std::shared_ptr<NavMeshRuntime>& out, uint64_t& outHash,
                              const LoadOptions& opts)
{
    if (size < 8) return false;
    
    uint32_t magic, ver;
    memcpy(&magic, data, 4);
    memcpy(&ver, data + 4, 4);
    
    if (magic != NAVBIN_MAGIC) return false;
    if (ver != 1 && ver != 2) return false;
    
    auto rt = std::make_shared<NavMeshRuntime>();
    outHash = 0;
    bool hasBVH = false;
    
    size_t p = 8;
    while (p + 8 <= size) {
        uint32_t id, csz;
        memcpy(&id, data + p, 4);
        memcpy(&csz, data + p + 4, 4);
        p += 8;
        
        if (id == CHUNK_CRC) break;
        if (p + csz > size) return false;
        const uint8_t* chunk = data + p;
        
        switch (id) {
            case 'OFNI': {
                size_t q = 24; // Skip old cell params
                rt->m_Bounds.min = *(const glm::vec3*)(chunk + q); q += 12;
                rt->m_Bounds.max = *(const glm::vec3*)(chunk + q);
                break;
            }
            case 'TREV': {
                size_t n = csz / 12;
                rt->m_Vertices.resize(n);
                memcpy(rt->m_Vertices.data(), chunk, csz);
                break;
            }
            case 'YLOP': {
                size_t stride = 16;
                size_t n = csz / stride;
                rt->m_Polys.resize(n);
                for (size_t i = 0; i < n; ++i) {
                    const uint8_t* rec = chunk + i * stride;
                    auto& poly = rt->m_Polys[i];
                    memcpy(&poly.i0, rec, 4);
                    memcpy(&poly.i1, rec + 4, 4);
                    memcpy(&poly.i2, rec + 8, 4);
                    uint32_t af; memcpy(&af, rec + 12, 4);
                    poly.area = (uint16_t)(af & 0xFFFF);
                    poly.flags = (af >> 16);
                }
                break;
            }
            case 'KNIL': {
                size_t stride = 36;
                size_t n = csz / stride;
                rt->m_Links.resize(n);
                const uint8_t* r = chunk;
                for (size_t i = 0; i < n; ++i) {
                    auto& l = rt->m_Links[i];
                    l.a = *(const glm::vec3*)r; r += 12;
                    l.b = *(const glm::vec3*)r; r += 12;
                    memcpy(&l.radius, r, 4); r += 4;
                    memcpy(&l.flags, r, 4); r += 4;
                    uint32_t bd; memcpy(&bd, r, 4); r += 4;
                    l.bidir = (uint8_t)bd;
                }
                break;
            }
            case 'NHVB': {
                uint32_t nodeCount;
                memcpy(&nodeCount, chunk, 4);
                rt->m_BVH.resize(nodeCount);
                const uint8_t* r = chunk + 4;
                for (uint32_t i = 0; i < nodeCount; ++i) {
                    auto& node = rt->m_BVH[i];
                    node.b.min = *(const glm::vec3*)r; r += 12;
                    node.b.max = *(const glm::vec3*)r; r += 12;
                    memcpy(&node.left, r, 4); r += 4;
                    memcpy(&node.right, r, 4); r += 4;
                    memcpy(&node.start, r, 4); r += 4;
                    memcpy(&node.count, r, 4); r += 4;
                }
                hasBVH = true;
                break;
            }
            case 'IHVB': {
                uint32_t indexCount;
                memcpy(&indexCount, chunk, 4);
                rt->m_BVHIndices.resize(indexCount);
                memcpy(rt->m_BVHIndices.data(), chunk + 4, indexCount * 4);
                break;
            }
            case 'HSAH': {
                memcpy(&outHash, chunk, 8);
                break;
            }
        }
        p += csz;
    }
    
    if (!hasBVH || opts.rebuildBVH) {
        rt->RebuildBVH();
    }
    
    out = std::move(rt);
    return true;
}

bool nav::io::ReadNavbinMapped(const std::string& filePath, std::shared_ptr<NavMeshRuntime>& out, uint64_t& outHash)
{
    LoadOptions opts;
    opts.useMemoryMapping = true;
    opts.verifyCRC = false;
    opts.rebuildBVH = false;
    return ReadNavbin(filePath, out, outHash, opts);
}

bool nav::io::LoadNavMeshFromFile(const std::string& path, std::shared_ptr<NavMeshRuntime>& out, uint64_t& outHash)
{
    // Use optimal settings for runtime loading
    LoadOptions opts;
    opts.useMemoryMapping = true;
    opts.verifyCRC = false;
    opts.rebuildBVH = false;
    return ReadNavbin(path, out, outHash, opts);
}

bool nav::io::ReadNavbinFromMemory(const uint8_t* fileData, size_t fileSize, std::shared_ptr<NavMeshRuntime>& out, 
                                    uint64_t& outHash, const LoadOptions& opts)
{
    // Validate header
    if (fileSize < 24) {
        std::cerr << "[Nav] ReadNavbinFromMemory: Data too small (" << fileSize << " bytes)" << std::endl;
        return false;
    }
    
    uint32_t magic, version, flags;
    uint64_t uncompressedSize;
    memcpy(&magic, fileData, 4);
    memcpy(&version, fileData + 4, 4);
    memcpy(&flags, fileData + 8, 4);
    memcpy(&uncompressedSize, fileData + 12, 8);
    
    if (magic != NAVBIN_MAGIC) {
        std::cerr << "[Nav] ReadNavbinFromMemory: Bad magic (got 0x" << std::hex << magic 
                  << ", expected 0x" << NAVBIN_MAGIC << std::dec << ")" << std::endl;
        return false;
    }
    
    if (version != NAVBIN_VERSION && version != 4 && version != 3) {
        std::cerr << "[Nav] ReadNavbinFromMemory: Unsupported version " << version 
                  << " (expected " << NAVBIN_VERSION << "). Please rebake the navmesh." << std::endl;
        return false;
    }
    
    // CRC check (optional)
    if (opts.verifyCRC) {
        uint32_t storedCRC, calcCRC;
        memcpy(&storedCRC, fileData + fileSize - 4, 4);
        calcCRC = crc32_buf(fileData, fileSize - 4);
        if (storedCRC != calcCRC) {
            std::cerr << "[Nav] ReadNavbinFromMemory: CRC mismatch" << std::endl;
            return false;
        }
    }
    
    // Extract payload
    const uint8_t* payloadData = fileData + 20;
    size_t payloadSize = fileSize - 20 - 4; // Header + CRC
    
    std::vector<uint8_t> decompressed;
    if (flags & FLAG_COMPRESSED) {
        decompressed.resize((size_t)uncompressedSize);
        size_t decSize = lz4::Decompress(payloadData, payloadSize, decompressed.data(), decompressed.size());
        if (decSize != uncompressedSize) {
            std::cerr << "[Nav] ReadNavbinFromMemory: LZ4 decompression failed" << std::endl;
            return false;
        }
        payloadData = decompressed.data();
        payloadSize = decompressed.size();
    }
    
    // Parse payload
    auto rt = std::make_shared<NavMeshRuntime>();
    bool hasBVH = false;
    outHash = 0;
    
    if (!ParsePayload(payloadData, payloadSize, *rt, outHash, hasBVH)) {
        std::cerr << "[Nav] ReadNavbinFromMemory: Failed to parse payload" << std::endl;
        return false;
    }
    
    // Rebuild BVH only if not present or explicitly requested
    if (!hasBVH || opts.rebuildBVH) {
        rt->RebuildBVH();
    }
    
    out = std::move(rt);
    return true;
}

bool nav::io::LoadNavMeshFromMemory(const uint8_t* data, size_t size, std::shared_ptr<NavMeshRuntime>& out, uint64_t& outHash)
{
    // Use optimal settings for runtime loading
    LoadOptions opts;
    opts.verifyCRC = false;
    opts.rebuildBVH = false;
    return ReadNavbinFromMemory(data, size, out, outHash, opts);
}

bool nav::io::BuildNavPayload(const NavMeshRuntime& rt, uint64_t bakeHash, std::vector<uint8_t>& outPayload)
{
    outPayload = BuildPayload(rt, bakeHash);
    return true;
}

// ============================================================================
// NavPack container (chunked navmesh)
// ============================================================================
namespace {
    struct NavPackHeader {
        uint32_t magic = NAVPACK_MAGIC;
        uint32_t version = NAVPACK_VERSION;
        uint32_t chunksX = 0;
        uint32_t chunksZ = 0;
        uint64_t sceneGuidHigh = 0;
        uint64_t sceneGuidLow = 0;
        uint64_t bakeHash = 0;
    };
    static constexpr size_t NavPackEntrySize =
        sizeof(int32_t) * 2 + sizeof(float) * 6 + sizeof(uint64_t) * 3;
    static size_t NavPackTableCount(const NavPackMeta& meta) {
        return (size_t)meta.chunksX * (size_t)meta.chunksZ;
    }
}

bool nav::io::WriteNavPack(const std::string& path, const NavPackMeta& meta,
                           const std::vector<NavPackChunk>& chunks,
                           const std::vector<std::vector<uint8_t>>& payloads)
{
    if (chunks.size() != payloads.size()) return false;
    const size_t expected = NavPackTableCount(meta);
    if (chunks.size() != expected) return false;

    NavPackHeader header;
    header.chunksX = meta.chunksX;
    header.chunksZ = meta.chunksZ;
    header.sceneGuidHigh = meta.sceneGuidHigh;
    header.sceneGuidLow = meta.sceneGuidLow;
    header.bakeHash = meta.bakeHash;

    uint64_t dataOffset = (uint64_t)(sizeof(NavPackHeader) + expected * NavPackEntrySize);
    uint64_t cursor = dataOffset;
    std::vector<NavPackChunk> table = chunks;
    for (size_t i = 0; i < table.size(); ++i) {
        if (!payloads[i].empty()) {
            const uint64_t payloadSize = (uint64_t)payloads[i].size();
            if (cursor + payloadSize < cursor) return false; // overflow guard
            table[i].offset = cursor;
            table[i].size = payloadSize;
            cursor += payloadSize;
        } else {
            table[i].offset = 0;
            table[i].size = 0;
        }
    }

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;

    if (!write_file(f, &header, sizeof(header))) return false;

    for (const auto& c : table) {
        if (!write_file(f, &c.gridX, sizeof(int32_t))) return false;
        if (!write_file(f, &c.gridZ, sizeof(int32_t))) return false;
        if (!write_file(f, &c.bounds.min.x, sizeof(float))) return false;
        if (!write_file(f, &c.bounds.min.y, sizeof(float))) return false;
        if (!write_file(f, &c.bounds.min.z, sizeof(float))) return false;
        if (!write_file(f, &c.bounds.max.x, sizeof(float))) return false;
        if (!write_file(f, &c.bounds.max.y, sizeof(float))) return false;
        if (!write_file(f, &c.bounds.max.z, sizeof(float))) return false;
        if (!write_file(f, &c.offset, sizeof(uint64_t))) return false;
        if (!write_file(f, &c.size, sizeof(uint64_t))) return false;
        if (!write_file(f, &c.hash, sizeof(uint64_t))) return false;
    }

    for (const auto& payload : payloads) {
        if (!payload.empty()) {
            if (!write_file(f, payload.data(), payload.size())) return false;
        }
    }

    f.close();
    return f.good();
}

bool nav::io::LoadNavPackIndex(const std::string& path, NavPackMeta& outMeta,
                               std::vector<NavPackChunk>& outChunks)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    NavPackHeader header;
    if (!read_file(f, &header, sizeof(header))) return false;
    if (header.magic != NAVPACK_MAGIC || header.version != NAVPACK_VERSION) return false;

    outMeta.chunksX = header.chunksX;
    outMeta.chunksZ = header.chunksZ;
    outMeta.sceneGuidHigh = header.sceneGuidHigh;
    outMeta.sceneGuidLow = header.sceneGuidLow;
    outMeta.bakeHash = header.bakeHash;

    const size_t count = (size_t)header.chunksX * (size_t)header.chunksZ;
    outChunks.clear();
    outChunks.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        NavPackChunk c;
        if (!read_file(f, &c.gridX, sizeof(int32_t))) return false;
        if (!read_file(f, &c.gridZ, sizeof(int32_t))) return false;
        if (!read_file(f, &c.bounds.min.x, sizeof(float))) return false;
        if (!read_file(f, &c.bounds.min.y, sizeof(float))) return false;
        if (!read_file(f, &c.bounds.min.z, sizeof(float))) return false;
        if (!read_file(f, &c.bounds.max.x, sizeof(float))) return false;
        if (!read_file(f, &c.bounds.max.y, sizeof(float))) return false;
        if (!read_file(f, &c.bounds.max.z, sizeof(float))) return false;
        if (!read_file(f, &c.offset, sizeof(uint64_t))) return false;
        if (!read_file(f, &c.size, sizeof(uint64_t))) return false;
        if (!read_file(f, &c.hash, sizeof(uint64_t))) return false;
        outChunks.push_back(c);
    }
    return true;
}

bool nav::io::ReadNavPackChunk(const std::string& path, const NavPackChunk& chunk,
                               std::shared_ptr<NavMeshRuntime>& out, uint64_t& outHash)
{
    if (chunk.size == 0 || chunk.offset == 0) return false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg((std::streamoff)chunk.offset, std::ios::beg);
    std::vector<uint8_t> payload;
    payload.resize((size_t)chunk.size);
    if (!read_file(f, payload.data(), payload.size())) return false;
    auto rt = std::make_shared<NavMeshRuntime>();
    bool hasBVH = false;
    uint64_t hash = 0;
    if (!ParsePayload(payload.data(), payload.size(), *rt, hash, hasBVH)) return false;
    if (!hasBVH) rt->RebuildBVH();
    out = rt;
    outHash = hash;
    return true;
}

bool nav::io::UpsertNavPackChunk(const std::string& path, const NavPackMeta& meta,
                                 const NavPackChunk& chunk, const std::vector<uint8_t>& payload)
{
    std::vector<NavPackChunk> chunks{chunk};
    std::vector<std::vector<uint8_t>> payloads{payload};
    return UpsertNavPackChunks(path, meta, chunks, payloads);
}

bool nav::io::UpsertNavPackChunks(const std::string& path, const NavPackMeta& meta,
                                  const std::vector<NavPackChunk>& chunks,
                                  const std::vector<std::vector<uint8_t>>& payloads)
{
    if (chunks.size() != payloads.size()) return false;

    const size_t count = NavPackTableCount(meta);
    std::ifstream fcheck(path, std::ios::binary);
    if (!fcheck.good()) {
        std::vector<NavPackChunk> initChunks(count);
        for (uint32_t z = 0; z < meta.chunksZ; ++z) {
            for (uint32_t x = 0; x < meta.chunksX; ++x) {
                size_t i = (size_t)z * meta.chunksX + x;
                initChunks[i].gridX = (int32_t)x;
                initChunks[i].gridZ = (int32_t)z;
            }
        }
        std::vector<std::vector<uint8_t>> emptyPayloads(count);
        if (!WriteNavPack(path, meta, initChunks, emptyPayloads)) return false;
    }
    fcheck.close();

    NavPackMeta existingMeta;
    std::vector<NavPackChunk> existingChunks;
    if (!LoadNavPackIndex(path, existingMeta, existingChunks)) return false;
    if (existingMeta.chunksX != meta.chunksX || existingMeta.chunksZ != meta.chunksZ) return false;

    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!f) return false;

    f.seekp(0, std::ios::end);
    uint64_t appendCursor = (uint64_t)f.tellp();

    for (size_t i = 0; i < chunks.size(); ++i) {
        const auto& payload = payloads[i];
        if (payload.empty()) continue;

        const NavPackChunk& req = chunks[i];
        const size_t idx = (size_t)req.gridZ * meta.chunksX + (size_t)req.gridX;
        if (idx >= count) {
            f.close();
            return false;
        }

        const uint64_t sz = (uint64_t)payload.size();
        if (appendCursor + sz < appendCursor) {
            f.close();
            return false;
        }

        f.seekp(0, std::ios::end);
        f.write(reinterpret_cast<const char*>(payload.data()), (std::streamsize)payload.size());

        NavPackChunk updated = req;
        updated.offset = appendCursor;
        updated.size = sz;
        appendCursor += sz;

        const std::streamoff entryOffset = (std::streamoff)sizeof(NavPackHeader) + (std::streamoff)(idx * NavPackEntrySize);
        f.seekp(entryOffset, std::ios::beg);
        f.write(reinterpret_cast<const char*>(&updated.gridX), sizeof(int32_t));
        f.write(reinterpret_cast<const char*>(&updated.gridZ), sizeof(int32_t));
        f.write(reinterpret_cast<const char*>(&updated.bounds.min.x), sizeof(float));
        f.write(reinterpret_cast<const char*>(&updated.bounds.min.y), sizeof(float));
        f.write(reinterpret_cast<const char*>(&updated.bounds.min.z), sizeof(float));
        f.write(reinterpret_cast<const char*>(&updated.bounds.max.x), sizeof(float));
        f.write(reinterpret_cast<const char*>(&updated.bounds.max.y), sizeof(float));
        f.write(reinterpret_cast<const char*>(&updated.bounds.max.z), sizeof(float));
        f.write(reinterpret_cast<const char*>(&updated.offset), sizeof(uint64_t));
        f.write(reinterpret_cast<const char*>(&updated.size), sizeof(uint64_t));
        f.write(reinterpret_cast<const char*>(&updated.hash), sizeof(uint64_t));
    }

    NavPackHeader header;
    header.chunksX = meta.chunksX;
    header.chunksZ = meta.chunksZ;
    header.sceneGuidHigh = meta.sceneGuidHigh;
    header.sceneGuidLow = meta.sceneGuidLow;
    header.bakeHash = meta.bakeHash;
    f.seekp(0, std::ios::beg);
    f.write(reinterpret_cast<const char*>(&header), sizeof(header));

    f.close();
    return f.good();
}