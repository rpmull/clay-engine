#include "DeferredGPUBuffers.h"
#include "Mesh.h"
#include "VertexTypes.h"
#include "core/debug/PrefabLog.h"

#include <vector>
#include <mutex>

namespace DeferredGPU {

// Thread-safe queue of meshes pending GPU buffer creation
static std::mutex s_queueMutex;
static std::vector<std::shared_ptr<Mesh>> s_pendingMeshes;

void QueueMesh(std::shared_ptr<Mesh> mesh) {
    if (!mesh || !mesh->pendingBuffer) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(s_queueMutex);
    s_pendingMeshes.push_back(mesh);
}

size_t FlushPendingBuffers() {
    std::vector<std::shared_ptr<Mesh>> meshesToProcess;
    
    // Take all pending meshes under lock
    {
        std::lock_guard<std::mutex> lock(s_queueMutex);
        meshesToProcess = std::move(s_pendingMeshes);
        s_pendingMeshes.clear();
    }
    
    if (meshesToProcess.empty()) {
        return 0;
    }
    
    PREFAB_LOG("DeferredGPU: Creating GPU buffers for " << meshesToProcess.size() << " meshes");
    
    size_t processed = 0;
    for (auto& mesh : meshesToProcess) {
        if (!mesh || !mesh->pendingBuffer) {
            continue;
        }
        
        auto& pending = *mesh->pendingBuffer;
        
        // Create vertex buffer
        if (!pending.vertexData.empty()) {
            const bgfx::Memory* vbMem = bgfx::copy(pending.vertexData.data(), 
                                                    static_cast<uint32_t>(pending.vertexData.size()));
            
            if (pending.isDynamic) {
                // Create dynamic vertex buffer
                if (pending.isSkinned) {
                    mesh->dvbh = bgfx::createDynamicVertexBuffer(vbMem, SkinnedPBRVertex::layout);
                } else {
                    mesh->dvbh = bgfx::createDynamicVertexBuffer(vbMem, PBRVertex::layout);
                }
                mesh->Dynamic = true;
            } else {
                // Create static vertex buffer
                if (pending.isSkinned) {
                    mesh->vbh = bgfx::createVertexBuffer(vbMem, SkinnedPBRVertex::layout);
                } else {
                    mesh->vbh = bgfx::createVertexBuffer(vbMem, PBRVertex::layout);
                }
            }
        }
        
        // Create index buffer
        if (!pending.indexData.empty()) {
            const bgfx::Memory* ibMem = bgfx::copy(pending.indexData.data(), 
                                                   static_cast<uint32_t>(pending.indexData.size()));
            
            if (pending.is32BitIndices) {
                mesh->ibh = bgfx::createIndexBuffer(ibMem, BGFX_BUFFER_INDEX32);
            } else {
                mesh->ibh = bgfx::createIndexBuffer(ibMem);
            }
        }
        
        // Clear pending buffer data to free memory
        mesh->pendingBuffer.reset();
        processed++;
    }
    
    PREFAB_LOG("DeferredGPU: Created " << processed << " GPU buffer pairs");
    
    return processed;
}

size_t GetPendingCount() {
    std::lock_guard<std::mutex> lock(s_queueMutex);
    return s_pendingMeshes.size();
}

void ClearPendingQueue() {
    std::lock_guard<std::mutex> lock(s_queueMutex);
    
    // Clear pending buffers on all queued meshes
    for (auto& mesh : s_pendingMeshes) {
        if (mesh && mesh->pendingBuffer) {
            mesh->pendingBuffer.reset();
        }
    }
    
    s_pendingMeshes.clear();
}

} // namespace DeferredGPU
