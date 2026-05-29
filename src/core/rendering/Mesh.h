#pragma once
#include <bgfx/bgfx.h>
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <limits>
#include <iostream>
#include <cstdint>

#include "DeferredGPUBuffers.h"  // Full definition of PendingGPUBuffer (required for unique_ptr destructor)

struct Mesh {
    bgfx::VertexBufferHandle vbh = BGFX_INVALID_HANDLE; // may store static or dynamic handle casted
    bgfx::DynamicVertexBufferHandle dvbh = BGFX_INVALID_HANDLE; // valid when Dynamic=true
    bgfx::IndexBufferHandle ibh = BGFX_INVALID_HANDLE;
    uint32_t numVertices = 0;
    uint32_t numIndices = 0;
    bool Dynamic = false;

    // Whether this mesh uses a skinned vertex layout (reliable even when CPU bone weights are not populated)
    bool SkinnedLayout = false;

    // CPU-side data for bounds & picking / morph targets / skinning
    std::vector<glm::vec3> Vertices;
    std::vector<glm::vec3> Normals;
    std::vector<glm::vec2> UVs;
    std::vector<uint32_t> Indices;

    // Optional submesh ranges for multi-material draws on a single mesh
    struct Submesh {
        uint32_t indexStart = 0;   // starting index within Indices
        uint32_t indexCount = 0;   // number of indices in this submesh
        uint32_t baseVertex = 0;   // first vertex of this submesh in Vertices
        uint32_t materialSlot = 0; // material slot index to use for this submesh
    };
    std::vector<Submesh> Submeshes;

    // Skinning (optional)
    std::vector<glm::vec4> BoneWeights; // xyzw weight
    std::vector<glm::ivec4> BoneIndices;
    // Optional compact palette remap: compact bone index -> original skeleton bone index.
    // When populated, BoneIndices are rewritten to this compact range to reduce per-draw
    // skinning uploads for split character meshes that only use a subset of bones.
    std::vector<uint16_t> SkinningBoneRemap;
    mutable uint64_t CachedSkinningBoneRemapHash = 0;
    mutable bool CachedSkinningBoneRemapHashValid = false;
    mutable const uint16_t* CachedSkinningBoneRemapData = nullptr;
    mutable size_t CachedSkinningBoneRemapSize = 0;
    

    glm::vec3 BoundsMin;
    glm::vec3 BoundsMax;

    // Deferred GPU buffer creation - stores vertex/index data until FlushPendingBuffers() is called
    // This avoids blocking the main thread during prefab instantiation
    std::unique_ptr<PendingGPUBuffer> pendingBuffer;
    
    // Check if this mesh has pending GPU buffer creation
    bool HasPendingBuffers() const { return pendingBuffer != nullptr; }

    bool HasSkinning() const { return SkinnedLayout || !BoneWeights.empty(); }
    bool UsesCompactSkinningPalette() const { return !SkinningBoneRemap.empty(); }
    uint64_t GetCachedSkinningBoneRemapHash() const {
        const uint16_t* currentData = SkinningBoneRemap.empty()
            ? nullptr
            : SkinningBoneRemap.data();
        const size_t currentSize = SkinningBoneRemap.size();
        if (CachedSkinningBoneRemapHashValid &&
            CachedSkinningBoneRemapData == currentData &&
            CachedSkinningBoneRemapSize == currentSize) {
            return CachedSkinningBoneRemapHash;
        }
        if (currentSize == 0) {
            CachedSkinningBoneRemapHash = 0;
            CachedSkinningBoneRemapHashValid = true;
            CachedSkinningBoneRemapData = currentData;
            CachedSkinningBoneRemapSize = currentSize;
            return CachedSkinningBoneRemapHash;
        }

        constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
        constexpr uint64_t kFnvPrime = 1099511628211ULL;
        uint64_t hash = kFnvOffset;
        const auto* bytes = reinterpret_cast<const uint8_t*>(currentData);
        for (size_t i = 0; i < currentSize * sizeof(uint16_t); ++i) {
            hash ^= static_cast<uint64_t>(bytes[i]);
            hash *= kFnvPrime;
        }

        CachedSkinningBoneRemapHash = hash;
        CachedSkinningBoneRemapHashValid = true;
        CachedSkinningBoneRemapData = currentData;
        CachedSkinningBoneRemapSize = currentSize;
        return CachedSkinningBoneRemapHash;
    }

    void ComputeBounds() {
        if (Vertices.empty()) {
            BoundsMin = glm::vec3(0);
            BoundsMax = glm::vec3(0);
            return;
        }
        glm::vec3 min(std::numeric_limits<float>::max());
        glm::vec3 max(std::numeric_limits<float>::lowest());
        for (auto& v : Vertices) {
            min = glm::min(min, v);
            max = glm::max(max, v);
        }
        BoundsMin = min;
        BoundsMax = max;
    }

    // GPU resource lifetime for meshes is managed by owning systems (asset pipeline or managers).
    // Avoid destroying handles here to prevent use-after-free during in-flight frames.
};
