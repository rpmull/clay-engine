#pragma once
#include <bgfx/bgfx.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cstring>
#include <functional>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <memory>

// GPU Instancing System for Claymore Engine
// Reduces draw calls for duplicate meshes by batching transforms into instance buffers
// Designed to match Unity/Unreal instancing performance

namespace cm {
namespace rendering {

// Instance data layout: 4x float4 = world matrix (64 bytes per instance)
// Matches bgfx instancing convention: TEXCOORD7-4 (i_data0..i_data3)
struct InstanceData {
    float transform[16];  // Column-major 4x4 matrix
    
    void SetTransform(const glm::mat4& m) {
        memcpy(transform, glm::value_ptr(m), sizeof(transform));
    }
    
    void SetTransform(const float* m) {
        memcpy(transform, m, sizeof(transform));
    }
};

// Skinned instance data extends the world transform with one extra vec4 of
// per-instance metadata. bgfx exposes this as i_data4 : TEXCOORD3.
struct SkinnedInstanceData {
    float transform[16];   // Column-major 4x4 world matrix
    float metadata[4];     // x = skinning instance record index, yzw reserved

    void SetTransform(const glm::mat4& m) {
        memcpy(transform, glm::value_ptr(m), sizeof(transform));
    }

    void SetTransform(const float* m) {
        memcpy(transform, m, sizeof(transform));
    }

    void SetMetadata(float x, float y = 0.0f, float z = 0.0f, float w = 0.0f) {
        metadata[0] = x;
        metadata[1] = y;
        metadata[2] = z;
        metadata[3] = w;
    }
};

// Key for identifying unique mesh+material combinations
struct InstanceKey {
    bgfx::VertexBufferHandle vbh;
    bgfx::IndexBufferHandle ibh;
    bgfx::ProgramHandle program;
    uint32_t indexStart;
    uint32_t indexCount;
    uint64_t variationHash = 0;
    uint64_t stateFlags = 0;
    
    bool operator==(const InstanceKey& other) const {
        return vbh.idx == other.vbh.idx &&
               ibh.idx == other.ibh.idx &&
               program.idx == other.program.idx &&
               indexStart == other.indexStart &&
               indexCount == other.indexCount &&
               variationHash == other.variationHash &&
               stateFlags == other.stateFlags;
    }
};

struct InstanceKeyHash {
    size_t operator()(const InstanceKey& k) const {
        size_t h = 0;
        h ^= std::hash<uint16_t>{}(k.vbh.idx) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint16_t>{}(k.ibh.idx) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint16_t>{}(k.program.idx) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>{}(k.indexStart) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>{}(k.indexCount) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint64_t>{}(k.variationHash) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint64_t>{}(k.stateFlags) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

// Per-batch data for instanced rendering
struct InstanceBatch {
    InstanceKey key;
    std::vector<uint8_t> instanceBytes;
    uint16_t instanceStride = 0;
    uint32_t instanceCount = 0;
    uint64_t stateFlags;
    
    // Batch binding callback (called before each submit)
    std::function<void()> bindBatch;
    // Optional per-instance fallback submit path for batches that cannot use an
    // instance buffer (for example, skinned batches falling back to legacy
    // per-draw submission while preserving per-instance metadata).
    std::function<void(uint16_t viewId, const uint8_t* instanceBytes)> submitSingle;
    
    void Reserve(size_t count) {
        if (instanceStride == 0) {
            return;
        }
        instanceBytes.reserve(count * instanceStride);
    }
    
    void AddInstance(const glm::mat4& transform) {
        InstanceData instance{};
        instance.SetTransform(transform);
        AddInstanceBytes(&instance, sizeof(instance));
    }
    
    void AddInstance(const float* transform) {
        InstanceData instance{};
        instance.SetTransform(transform);
        AddInstanceBytes(&instance, sizeof(instance));
    }

    template<typename T>
    void AddInstance(const T& instance) {
        AddInstanceBytes(&instance, static_cast<uint16_t>(sizeof(T)));
    }

    void AddInstanceBytes(const void* bytes, uint16_t stride) {
        if (stride == 0 || bytes == nullptr) {
            return;
        }
        if (instanceStride == 0) {
            instanceStride = stride;
        }
        if (instanceStride != stride) {
            return;
        }

        const size_t oldSize = instanceBytes.size();
        instanceBytes.resize(oldSize + stride);
        std::memcpy(instanceBytes.data() + oldSize, bytes, stride);
        ++instanceCount;
    }

    const uint8_t* GetInstanceBytes(size_t index) const {
        if (instanceStride == 0 || index >= instanceCount) {
            return nullptr;
        }
        return instanceBytes.data() + index * instanceStride;
    }
    
    void Clear() {
        instanceBytes.clear();
        instanceCount = 0;
        instanceStride = 0;
        bindBatch = nullptr;
        submitSingle = nullptr;
    }
};

// Instancing Manager - collects and batches draw calls
class InstanceManager {
public:
    static constexpr size_t kMinInstancesForBatching = 4;  // Below this, individual draws are more efficient
    static constexpr size_t kMaxInstancesPerBatch = 256;   // GPU instance buffer limit
    
    InstanceManager() = default;
    
    // Begin a new frame - clears all batches
    void BeginFrame() {
        m_batches.clear();
        m_batchMap.clear();
        m_currentBatchIndex = 0;
    }
    
    // Get or create a batch for the given key
    InstanceBatch& GetBatch(const InstanceKey& key, uint64_t stateFlags, uint16_t instanceStride) {
        InstanceKey lookupKey = key;
        lookupKey.stateFlags = stateFlags;

        auto it = m_batchMap.find(lookupKey);
        if (it != m_batchMap.end()) {
            return m_batches[it->second];
        }
        
        size_t idx = m_batches.size();
        m_batches.emplace_back();
        m_batches.back().key = lookupKey;
        m_batches.back().stateFlags = stateFlags;
        m_batches.back().instanceStride = instanceStride;
        m_batchMap[lookupKey] = idx;
        return m_batches.back();
    }
    
    // Submit all instanced batches to bgfx
    void Submit(uint16_t viewId) {
        for (auto& batch : m_batches) {
            if (batch.instanceCount == 0 || batch.instanceStride == 0) continue;

            auto submitIndividual = [&](size_t beginIndex, size_t endIndex) {
                for (size_t i = beginIndex; i < endIndex; ++i) {
                    const uint8_t* instanceData = batch.GetInstanceBytes(i);
                    if (!instanceData) {
                        continue;
                    }

                    if (batch.submitSingle) {
                        if (batch.bindBatch) batch.bindBatch();
                        batch.submitSingle(viewId, instanceData);
                        continue;
                    }

                    if (batch.instanceStride < sizeof(InstanceData)) {
                        continue;
                    }

                    const auto* inst = reinterpret_cast<const InstanceData*>(instanceData);
                    bgfx::setTransform(inst->transform);
                    bgfx::setVertexBuffer(0, batch.key.vbh);
                    bgfx::setIndexBuffer(batch.key.ibh, batch.key.indexStart, batch.key.indexCount);
                    if (batch.bindBatch) batch.bindBatch();
                    bgfx::setState(batch.stateFlags);
                    bgfx::submit(viewId, batch.key.program);
                }
            };

            // For small batches, bgfx overhead may exceed gain; submit individually
            if (batch.instanceCount < kMinInstancesForBatching) {
                submitIndividual(0, batch.instanceCount);
                continue;
            }

            // Split into sub-batches if exceeding max
            size_t offset = 0;
            while (offset < batch.instanceCount) {
                size_t count = std::min(batch.instanceCount - offset, kMaxInstancesPerBatch);
                
                uint32_t numInstances = bgfx::getAvailInstanceDataBuffer(
                    static_cast<uint32_t>(count), batch.instanceStride);
                
                if (numInstances == 0) {
                    submitIndividual(offset, offset + count);
                    offset += count;
                } else {
                    bgfx::InstanceDataBuffer idb;
                    bgfx::allocInstanceDataBuffer(&idb, numInstances, batch.instanceStride);
                    
                    std::memcpy(
                        idb.data,
                        batch.instanceBytes.data() + offset * batch.instanceStride,
                        numInstances * batch.instanceStride);
                    
                    bgfx::setVertexBuffer(0, batch.key.vbh);
                    bgfx::setIndexBuffer(batch.key.ibh, batch.key.indexStart, batch.key.indexCount);
                    bgfx::setInstanceDataBuffer(&idb);
                    if (batch.bindBatch) batch.bindBatch();
                    bgfx::setState(batch.stateFlags);
                    bgfx::submit(viewId, batch.key.program);
                    offset += numInstances;
                }
            }
        }
    }
    
    // Stats for debugging
    size_t GetBatchCount() const { return m_batches.size(); }
    size_t GetTotalInstances() const {
        size_t total = 0;
        for (const auto& b : m_batches) total += b.instanceCount;
        return total;
    }
    size_t EstimateSubmitCount() const {
        size_t total = 0;
        for (const auto& batch : m_batches) {
            if (batch.instanceCount == 0) {
                continue;
            }
            if (batch.instanceCount < kMinInstancesForBatching) {
                total += batch.instanceCount;
                continue;
            }
            total += (batch.instanceCount + kMaxInstancesPerBatch - 1) / kMaxInstancesPerBatch;
        }
        return total;
    }
    
private:
    std::vector<InstanceBatch> m_batches;
    std::unordered_map<InstanceKey, size_t, InstanceKeyHash> m_batchMap;
    size_t m_currentBatchIndex = 0;
};

} // namespace rendering
} // namespace cm

