#pragma once

#include <memory>
#include <vector>
#include <cstdint>

struct Mesh;

/**
 * PendingGPUBuffer - Stores vertex/index data for deferred GPU buffer creation
 * 
 * This allows mesh loading to defer synchronous GPU operations (bgfx::createVertexBuffer,
 * bgfx::createIndexBuffer) until after all meshes are loaded. This batches the GPU
 * operations together, reducing main thread blocking from 50-500ms to near-instant.
 */
struct PendingGPUBuffer {
    std::vector<uint8_t> vertexData;
    std::vector<uint8_t> indexData;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t vertexStride = 0;  // 32 for PBRVertex, 52 for SkinnedPBRVertex
    bool isSkinned = false;
    bool is32BitIndices = false;
    bool isDynamic = false;
};

/**
 * DeferredGPU - Deferred GPU buffer creation manager
 * 
 * Performance Impact:
 * - Batches all GPU buffer creation to a single point
 * - Eliminates 1-10ms blocking per mesh during instantiation
 * - For complex prefabs with 10-50 meshes, saves 50-500ms total
 * 
 * Usage:
 * 1. During mesh loading, call QueueMesh() instead of bgfx::createVertexBuffer
 * 2. After all meshes are loaded, call FlushPendingBuffers()
 * 3. GPU buffers are created in batch, minimizing driver overhead
 */
namespace DeferredGPU {

/**
 * Queue a mesh for deferred GPU buffer creation.
 * The mesh must have its pendingBuffer populated.
 * 
 * @param mesh Shared pointer to mesh with pending buffer data
 */
void QueueMesh(std::shared_ptr<Mesh> mesh);

/**
 * Create all pending GPU buffers.
 * Call this at the end of prefab instantiation, after all meshes are loaded.
 * This is the only point where bgfx::createVertexBuffer is called.
 * 
 * @return Number of meshes processed
 */
size_t FlushPendingBuffers();

/**
 * Get count of pending buffer operations.
 * Useful for logging/debugging.
 */
size_t GetPendingCount();

/**
 * Clear pending queue without creating buffers.
 * Call on error/abort to avoid memory leaks.
 */
void ClearPendingQueue();

} // namespace DeferredGPU
