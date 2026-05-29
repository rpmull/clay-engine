#include "MeshBinaryLoader.h"
#include "core/vfs/FileSystem.h"
#include "core/vfs/VirtualFS.h"
#include "core/rendering/Mesh.h"
#include "core/rendering/VertexTypes.h"
#include "core/rendering/DeferredGPUBuffers.h"
#include "core/ecs/Components.h"
#include "core/debug/PrefabLog.h"
#include <glm/glm.hpp>
#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>

namespace MeshBinaryLoader {

// Meshbin file structures (must match editor's format)
struct Header {
    uint32_t magic;
    uint32_t version;
    uint32_t submeshCount;
};

struct SubmeshDesc {
    uint32_t vertexCount;
    uint32_t indexCount;
    uint32_t vertexStride;
    uint32_t indexSize;
    uint32_t hasSkinning;
    uint32_t vbOffset;
    uint32_t ibOffset;
    glm::vec3 bmin;
    glm::vec3 bmax;
    uint32_t nameOffset;
    uint32_t nameLength;
    uint32_t texOffset;
    uint32_t texSize;
    uint32_t extrasOffset;
    uint32_t extrasSize;
    uint32_t quantInfoOffset;
    uint32_t quantInfoSize;
    uint32_t xformOffset;
    uint32_t xformSize;
    uint32_t submeshOffset;
    uint32_t submeshSize;
    uint32_t blendOffset;
    uint32_t blendSize;
};

// Packed submesh structure (matches editor format)
struct PackedSubmesh {
    uint32_t indexStart = 0;
    uint32_t indexCount = 0;
    uint32_t baseVertex = 0;
    uint32_t materialSlot = 0;
};

// Simple memory stream for reading
struct MemStream {
    const uint8_t* data;
    size_t size;
    size_t pos;
    
    bool read(void* dst, size_t count) {
        if (pos + count > size) return false;
        memcpy(dst, data + pos, count);
        pos += count;
        return true;
    }
    
    bool seek(size_t offset) {
        if (offset > size) return false;
        pos = offset;
        return true;
    }
    
    template<typename T>
    bool read(T& out) { return read(&out, sizeof(T)); }
};

// Read file from VFS
// Note: Path normalization (including .., prefix stripping, case-insensitive fallback)
// is now handled centrally by VFS::NormalizePath and PakReader::FindEntry.
// No need for per-loader path variant guessing.
static bool ReadMeshBinFile(const std::string& path, std::vector<uint8_t>& outData) {
    // Normalize path first (resolves .., collapses slashes)
    std::string normalized = IVirtualFS::NormalizePath(path);
    
    // Try global VFS first (PAK files at runtime)
    if (VFS::Get() && VFS::Get()->ReadFile(normalized, outData)) {
        return true;
    }
    
    // Fall back to FileSystem (handles VFS delegation + disk fallback)
    if (FileSystem::Instance().ReadFile(normalized, outData)) {
        return true;
    }
    
    // If normalized path differs, try original as last resort
    if (normalized != path) {
        if (VFS::Get() && VFS::Get()->ReadFile(path, outData)) {
            return true;
        }
        if (FileSystem::Instance().ReadFile(path, outData)) {
            return true;
        }
    }
    
    return false;
}

// Read header and descriptors
static bool ReadHeaderAndDescs(const std::vector<uint8_t>& fileData, Header& h, std::vector<SubmeshDesc>& descs) {
    if (fileData.size() < sizeof(Header)) return false;
    
    MemStream ms{fileData.data(), fileData.size(), 0};
    
    if (!ms.read(h)) return false;
    if (h.magic != MESH_BIN_MAGIC) {
        MESH_LOG_ERROR("Invalid magic in meshbin");
        return false;
    }
    if (h.version > MESH_BIN_VERSION) {
        MESH_LOG_ERROR("Unsupported meshbin version: " << h.version);
        return false;
    }
    
    descs.resize(h.submeshCount);
    for (uint32_t i = 0; i < h.submeshCount; i++) {
        if (!ms.read(descs[i])) return false;
    }
    
    return true;
}

// Read submesh chunk from binary data
// This reads the Submeshes array that maps submesh ranges to material slots
static void ReadSubmeshChunk(MemStream& ms, const SubmeshDesc& desc, std::vector<Mesh::Submesh>& out) {
    if (desc.submeshOffset == 0 || desc.submeshSize == 0) {
        out.clear();
        return;
    }
    
    size_t savedPos = ms.pos;
    if (!ms.seek(desc.submeshOffset)) {
        MESH_LOG_WARN("Failed to seek to submesh offset " << desc.submeshOffset);
        out.clear();
        return;
    }
    
    uint32_t count = 0;
    if (!ms.read(count) || count == 0) {
        out.clear();
        ms.pos = savedPos;
        return;
    }
    
    out.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        PackedSubmesh packed{};
        if (!ms.read(packed)) {
            MESH_LOG_ERROR("Failed to read packed submesh " << i << " of " << count);
            out.clear();
            ms.pos = savedPos;
            return;
        }
        out[i].indexStart = packed.indexStart;
        out[i].indexCount = packed.indexCount;
        out[i].baseVertex = packed.baseVertex;
        out[i].materialSlot = packed.materialSlot;
    }
    
    ms.pos = savedPos;
    
    if (!out.empty()) {
        MESH_LOG("Read " << out.size() << " submeshes with material slot mappings");
    }
}

static void CompactSkinningPalette(Mesh& mesh, std::vector<uint8_t>& vbData, uint32_t siblingSkinnedMeshCount)
{
    mesh.SkinningBoneRemap.clear();

    if (!mesh.HasSkinning() || mesh.BoneIndices.empty() || mesh.BoneWeights.empty()) {
        return;
    }

    int maxBoneIndex = -1;
    for (size_t i = 0; i < mesh.BoneIndices.size() && i < mesh.BoneWeights.size(); ++i) {
        const glm::ivec4& bi = mesh.BoneIndices[i];
        const glm::vec4& bw = mesh.BoneWeights[i];
        for (int c = 0; c < 4; ++c) {
            if (bw[c] <= 1e-5f || bi[c] < 0) {
                continue;
            }
            maxBoneIndex = std::max(maxBoneIndex, bi[c]);
        }
    }

    if (maxBoneIndex <= 0) {
        return;
    }

    std::vector<uint8_t> used(static_cast<size_t>(maxBoneIndex + 1), 0);
    size_t uniqueCount = 0;
    for (size_t i = 0; i < mesh.BoneIndices.size() && i < mesh.BoneWeights.size(); ++i) {
        const glm::ivec4& bi = mesh.BoneIndices[i];
        const glm::vec4& bw = mesh.BoneWeights[i];
        for (int c = 0; c < 4; ++c) {
            const int boneIndex = bi[c];
            if (bw[c] <= 1e-5f || boneIndex < 0) {
                continue;
            }
            uint8_t& flag = used[static_cast<size_t>(boneIndex)];
            if (flag == 0) {
                flag = 1;
                ++uniqueCount;
            }
        }
    }

    if (uniqueCount == 0 || uniqueCount >= used.size()) {
        return;
    }

    const bool forceCompactionForVertexFormat =
        maxBoneIndex >= static_cast<int>(std::numeric_limits<uint8_t>::max() + 1u);
    if (forceCompactionForVertexFormat &&
        uniqueCount > static_cast<size_t>(std::numeric_limits<uint8_t>::max() + 1u)) {
        std::cerr << "[MeshBinaryLoader] WARNING: Mesh requires more than 256 unique skinning bones in a single draw. "
                  << "The current vertex format cannot represent that mesh without authoring splits." << std::endl;
        return;
    }

    // Split characters such as Player/base_human_96 benefit more from the shared
    // skeleton palette fast path than from per-mesh compaction. Preserve original
    // bone indices for substantial multi-part skinned assets and reserve compaction
    // for tiny palette slices where the per-draw savings are still meaningful.
    if (!forceCompactionForVertexFormat && siblingSkinnedMeshCount > 1 && uniqueCount > 4) {
        return;
    }

    std::vector<int> oldToNew(used.size(), -1);
    mesh.SkinningBoneRemap.reserve(uniqueCount);
    for (size_t oldIndex = 0; oldIndex < used.size(); ++oldIndex) {
        if (used[oldIndex] == 0) {
            continue;
        }
        oldToNew[oldIndex] = static_cast<int>(mesh.SkinningBoneRemap.size());
        mesh.SkinningBoneRemap.push_back(static_cast<uint16_t>(oldIndex));
    }

    for (size_t i = 0; i < mesh.BoneIndices.size() && i < mesh.BoneWeights.size(); ++i) {
        glm::ivec4& bi = mesh.BoneIndices[i];
        const glm::vec4& bw = mesh.BoneWeights[i];
        for (int c = 0; c < 4; ++c) {
            const int boneIndex = bi[c];
            if (bw[c] <= 1e-5f || boneIndex < 0 || boneIndex >= static_cast<int>(oldToNew.size())) {
                bi[c] = 0;
                continue;
            }
            const int compactIndex = oldToNew[static_cast<size_t>(boneIndex)];
            bi[c] = compactIndex >= 0 ? compactIndex : 0;
        }
    }

    if (vbData.size() >= mesh.BoneIndices.size() * sizeof(SkinnedPBRVertex)) {
        auto* skinnedVertices = reinterpret_cast<SkinnedPBRVertex*>(vbData.data());
        for (size_t i = 0; i < mesh.BoneIndices.size(); ++i) {
            const glm::ivec4& bi = mesh.BoneIndices[i];
            skinnedVertices[i].i0 = static_cast<uint8_t>(bi.x);
            skinnedVertices[i].i1 = static_cast<uint8_t>(bi.y);
            skinnedVertices[i].i2 = static_cast<uint8_t>(bi.z);
            skinnedVertices[i].i3 = static_cast<uint8_t>(bi.w);
        }
    }

    MESH_LOG("Compacted skinning palette to " << uniqueCount << " bones");
}

uint32_t GetSubmeshCount(const std::string& meshBinPath) {
    std::vector<uint8_t> fileData;
    if (!ReadMeshBinFile(meshBinPath, fileData)) {
        return 0;
    }
    
    Header h;
    std::vector<SubmeshDesc> descs;
    if (!ReadHeaderAndDescs(fileData, h, descs)) {
        return 0;
    }
    
    return h.submeshCount;
}

bool IsValidMeshBin(const std::string& meshBinPath) {
    std::vector<uint8_t> fileData;
    if (!ReadMeshBinFile(meshBinPath, fileData)) {
        return false;
    }
    
    if (fileData.size() < sizeof(Header)) return false;
    
    const Header* h = reinterpret_cast<const Header*>(fileData.data());
    return h->magic == MESH_BIN_MAGIC && h->version <= MESH_BIN_VERSION;
}

std::shared_ptr<Mesh> LoadMesh(const std::string& meshBinPath, uint32_t submeshIndex, bool* outSkinned) {
    MESH_LOG("Loading mesh from: " << meshBinPath << " (submesh " << submeshIndex << ")");
    
    std::vector<uint8_t> fileData;
    if (!ReadMeshBinFile(meshBinPath, fileData)) {
        MESH_LOG_ERROR("Failed to read meshbin: " << meshBinPath);
        return nullptr;
    }
    
    MESH_LOG("Read " << fileData.size() << " bytes");
    
    Header h;
    std::vector<SubmeshDesc> descs;
    if (!ReadHeaderAndDescs(fileData, h, descs)) {
        MESH_LOG_ERROR("Failed to parse header/descriptors");
        return nullptr;
    }
    
    MESH_LOG("Found " << h.submeshCount << " submeshes, version " << h.version);
    
    if (submeshIndex >= h.submeshCount) {
        MESH_LOG_ERROR("Submesh index " << submeshIndex << " out of range (count: " << h.submeshCount << ")");
        return nullptr;
    }
    
    const SubmeshDesc& desc = descs[submeshIndex];
    
    MESH_LOG("Submesh " << submeshIndex << ": " << desc.vertexCount << " verts, " << desc.indexCount << " indices, "
             << "stride=" << desc.vertexStride << ", indexSize=" << desc.indexSize << ", skinned=" << desc.hasSkinning);
    
    // Validate offsets and sizes
    if (desc.vbOffset >= fileData.size() || desc.ibOffset >= fileData.size()) {
        MESH_LOG_ERROR("Invalid buffer offsets (vb=" << desc.vbOffset << ", ib=" << desc.ibOffset << ", fileSize=" << fileData.size() << ")");
        return nullptr;
    }
    
    uint64_t expectedVbEnd = static_cast<uint64_t>(desc.vbOffset) +
                             static_cast<uint64_t>(desc.vertexCount) * desc.vertexStride;
    uint64_t expectedIbEnd = static_cast<uint64_t>(desc.ibOffset) +
                             static_cast<uint64_t>(desc.indexCount) * desc.indexSize;
    if (expectedVbEnd > fileData.size() || expectedIbEnd > fileData.size()) {
        MESH_LOG_ERROR("Buffer data extends past end of file (vbEnd=" << expectedVbEnd << ", ibEnd=" << expectedIbEnd << ", fileSize=" << fileData.size() << ")");
        return nullptr;
    }
    bool isSkinned = desc.hasSkinning != 0;
    
    if (outSkinned) {
        *outSkinned = isSkinned;
    }
    
    // Create mesh
    auto mesh = std::make_shared<Mesh>();
    mesh->SkinnedLayout = isSkinned;
    mesh->numVertices = desc.vertexCount;
    mesh->numIndices = desc.indexCount;
    mesh->BoundsMin = desc.bmin;
    mesh->BoundsMax = desc.bmax;
    
    // Read vertex data
    MemStream ms{fileData.data(), fileData.size(), 0};
    if (!ms.seek(desc.vbOffset)) {
        MESH_LOG_ERROR("Invalid vertex buffer offset");
        return nullptr;
    }
    
    size_t vbSize = static_cast<size_t>(expectedVbEnd - desc.vbOffset);
    std::vector<uint8_t> vbData(vbSize);
    if (!ms.read(vbData.data(), vbSize)) {
        MESH_LOG_ERROR("Failed to read vertex data");
        return nullptr;
    }
    
    // Read index data
    if (!ms.seek(desc.ibOffset)) {
        MESH_LOG_ERROR("Invalid index buffer offset");
        return nullptr;
    }
    
    size_t ibSize = static_cast<size_t>(expectedIbEnd - desc.ibOffset);
    std::vector<uint8_t> ibData(ibSize);
    if (!ms.read(ibData.data(), ibSize)) {
        MESH_LOG_ERROR("Failed to read index data");
        return nullptr;
    }
    
    // Parse vertices directly from the binary data
    // The meshbin format uses exactly PBRVertex (32 bytes) or SkinnedPBRVertex (52 bytes)
    mesh->Vertices.resize(desc.vertexCount);
    mesh->Normals.resize(desc.vertexCount);
    mesh->UVs.resize(desc.vertexCount);
    
    if (isSkinned) {
        mesh->BoneIndices.resize(desc.vertexCount);
        mesh->BoneWeights.resize(desc.vertexCount);
    }
    
    // Expected strides: PBRVertex=32, SkinnedPBRVertex=52
    const uint32_t expectedStride = isSkinned ? 52 : 32;
    if (desc.vertexStride != expectedStride) {
        MESH_LOG_WARN("Unexpected vertex stride " << desc.vertexStride << " (expected " << expectedStride << ")");
    }
    
    if (isSkinned) {
        // Parse as SkinnedPBRVertex directly
        const SkinnedPBRVertex* vertices = reinterpret_cast<const SkinnedPBRVertex*>(vbData.data());
        for (uint32_t i = 0; i < desc.vertexCount; i++) {
            const auto& v = vertices[i];
            mesh->Vertices[i] = glm::vec3(v.x, v.y, v.z);
            mesh->Normals[i] = glm::vec3(v.nx, v.ny, v.nz);
            mesh->UVs[i] = glm::vec2(v.u, v.v);
            mesh->BoneIndices[i] = glm::ivec4(v.i0, v.i1, v.i2, v.i3);
            mesh->BoneWeights[i] = glm::vec4(v.w0, v.w1, v.w2, v.w3);
        }
        uint32_t siblingSkinnedMeshCount = 0;
        for (const auto& entryDesc : descs) {
            siblingSkinnedMeshCount += (entryDesc.hasSkinning != 0) ? 1u : 0u;
        }
        CompactSkinningPalette(*mesh, vbData, siblingSkinnedMeshCount);
    } else {
        // Parse as PBRVertex directly
        const PBRVertex* vertices = reinterpret_cast<const PBRVertex*>(vbData.data());
        for (uint32_t i = 0; i < desc.vertexCount; i++) {
            const auto& v = vertices[i];
            mesh->Vertices[i] = glm::vec3(v.x, v.y, v.z);
            mesh->Normals[i] = glm::vec3(v.nx, v.ny, v.nz);
            mesh->UVs[i] = glm::vec2(v.u, v.v);
        }
    }
    
    // Parse indices
    mesh->Indices.resize(desc.indexCount);
    if (desc.indexSize == 4) {
        memcpy(mesh->Indices.data(), ibData.data(), ibSize);
    } else if (desc.indexSize == 2) {
        const uint16_t* src = reinterpret_cast<const uint16_t*>(ibData.data());
        for (uint32_t i = 0; i < desc.indexCount; i++) {
            mesh->Indices[i] = src[i];
        }
    }
    
    // Read submesh chunk (maps submesh ranges to material slots)
    // This is critical for multi-material meshes - without it, all submeshes use slot 0
    ReadSubmeshChunk(ms, desc, mesh->Submeshes);
    
    // If no submeshes were read, create a default one covering the entire mesh (slot 0)
    // This matches the editor behavior for backward compatibility
    if (mesh->Submeshes.empty()) {
        Mesh::Submesh sm;
        sm.indexStart = 0;
        sm.indexCount = desc.indexCount;
        sm.baseVertex = 0;
        sm.materialSlot = 0;
        mesh->Submeshes = { sm };
        MESH_LOG("No submesh chunk found, created default submesh covering entire mesh (slot 0)");
    }
    
    // Sanity check before GPU buffer creation
    if (desc.vertexCount == 0 || desc.indexCount == 0) {
        MESH_LOG_ERROR("Empty mesh (verts=" << desc.vertexCount << ", indices=" << desc.indexCount << ")");
        return nullptr;
    }
    
    // Very large meshes might indicate corrupt data
    if (desc.vertexCount > 10000000 || desc.indexCount > 30000000) {
        MESH_LOG_ERROR("Suspiciously large mesh (verts=" << desc.vertexCount << ", indices=" << desc.indexCount << ")");
        return nullptr;
    }
    
    MESH_LOG("Queueing GPU buffer creation (deferred)...");
    
    // PERFORMANCE OPTIMIZATION: Defer GPU buffer creation to batch all operations
    // This avoids synchronous bgfx calls (1-10ms each) during mesh loading
    // Buffers will be created when DeferredGPU::FlushPendingBuffers() is called
    mesh->pendingBuffer = std::make_unique<PendingGPUBuffer>();
    mesh->pendingBuffer->vertexData = std::move(vbData);
    mesh->pendingBuffer->indexData = std::move(ibData);
    mesh->pendingBuffer->vertexCount = desc.vertexCount;
    mesh->pendingBuffer->indexCount = desc.indexCount;
    mesh->pendingBuffer->vertexStride = desc.vertexStride;
    mesh->pendingBuffer->isSkinned = isSkinned;
    mesh->pendingBuffer->is32BitIndices = (desc.indexSize == 4);
    mesh->pendingBuffer->isDynamic = false;  // Static by default, can be upgraded later if blend shapes are needed
    
    // Queue mesh for deferred GPU buffer creation
    DeferredGPU::QueueMesh(mesh);
    
    MESH_LOG("Loaded mesh (GPU deferred): " << desc.vertexCount << " verts, " << desc.indexCount << " indices" << (isSkinned ? " (skinned)" : ""));
    
    return mesh;
}

// Helper to read vec3 array from memory stream
static void ReadVec3Array(MemStream& ms, std::vector<glm::vec3>& values) {
    for (auto& v : values) {
        if (!ms.read(v.x) || !ms.read(v.y) || !ms.read(v.z)) {
            break;
        }
    }
}

std::unique_ptr<BlendShapeComponent> LoadBlendShapes(const std::string& meshBinPath, uint32_t submeshIndex) {
    std::vector<uint8_t> fileData;
    if (!ReadMeshBinFile(meshBinPath, fileData)) {
        return nullptr;
    }
    
    Header h;
    std::vector<SubmeshDesc> descs;
    if (!ReadHeaderAndDescs(fileData, h, descs)) {
        return nullptr;
    }
    
    if (submeshIndex >= h.submeshCount) {
        return nullptr;
    }
    
    const SubmeshDesc& desc = descs[submeshIndex];
    
    // Check if blend shapes exist
    if (desc.blendOffset == 0 || desc.blendSize == 0) {
        return nullptr;
    }
    
    // Validate blend offset
    if (desc.blendOffset >= fileData.size() || 
        desc.blendOffset + desc.blendSize > fileData.size()) {
        MESH_LOG_ERROR("Invalid blend shape offset/size");
        return nullptr;
    }
    
    MemStream ms{fileData.data(), fileData.size(), 0};
    if (!ms.seek(desc.blendOffset)) {
        return nullptr;
    }
    
    uint32_t shapeCount = 0;
    if (!ms.read(shapeCount) || shapeCount == 0) {
        return nullptr;
    }
    
    auto component = std::make_unique<BlendShapeComponent>();
    component->Shapes.resize(shapeCount);
    
    for (uint32_t i = 0; i < shapeCount; ++i) {
        auto& shape = component->Shapes[i];
        
        uint32_t nameLen = 0;
        if (!ms.read(nameLen)) {
            component.reset();
            break;
        }
        
        if (nameLen > 0) {
            shape.Name.resize(nameLen);
            if (!ms.read(shape.Name.data(), nameLen)) {
                component.reset();
                break;
            }
        }
        shape.UpdateNameHash();
        
        float weight = 0.0f;
        if (!ms.read(weight)) {
            component.reset();
            break;
        }
        shape.Weight = weight;
        
        // v9+: Read sparse flag
        if (h.version >= 9) {
            uint8_t isSparse = 0;
            if (!ms.read(isSparse)) {
                component.reset();
                break;
            }
            shape.IsSparse = (isSparse != 0);
            
            if (shape.IsSparse) {
                // Read sparse data
                uint32_t sparseCount = 0;
                if (!ms.read(sparseCount)) {
                    component.reset();
                    break;
                }
                if (sparseCount > 0) {
                    shape.SparseIndices.resize(sparseCount);
                    if (!ms.read(shape.SparseIndices.data(), sparseCount * sizeof(uint32_t))) {
                        component.reset();
                        break;
                    }
                    shape.SparseDeltaPos.resize(sparseCount);
                    ReadVec3Array(ms, shape.SparseDeltaPos);
                    shape.SparseDeltaNorm.resize(sparseCount);
                    ReadVec3Array(ms, shape.SparseDeltaNorm);
                }
            } else {
                // Read dense data
                uint32_t posCount = 0;
                if (!ms.read(posCount)) {
                    component.reset();
                    break;
                }
                shape.DeltaPos.resize(posCount);
                if (posCount > 0) {
                    ReadVec3Array(ms, shape.DeltaPos);
                }
                
                uint32_t normalCount = 0;
                if (!ms.read(normalCount)) {
                    component.reset();
                    break;
                }
                shape.DeltaNormal.resize(normalCount);
                if (normalCount > 0) {
                    ReadVec3Array(ms, shape.DeltaNormal);
                }
            }
        } else {
            // v8 and earlier: Read dense data only
            uint32_t posCount = 0;
            if (!ms.read(posCount)) {
                component.reset();
                break;
            }
            shape.DeltaPos.resize(posCount);
            if (posCount > 0) {
                ReadVec3Array(ms, shape.DeltaPos);
            }
            
            uint32_t normalCount = 0;
            if (!ms.read(normalCount)) {
                component.reset();
                break;
            }
            shape.DeltaNormal.resize(normalCount);
            if (normalCount > 0) {
                ReadVec3Array(ms, shape.DeltaNormal);
            }
        }
    }
    
    if (component && !component->Shapes.empty()) {
        MESH_LOG("Loaded " << component->Shapes.size() << " blend shapes from mesh file");
    }
    
    return component;
}

} // namespace MeshBinaryLoader
