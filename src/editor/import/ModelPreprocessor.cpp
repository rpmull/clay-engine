#include "ModelPreprocessor.h"
#include "core/rendering/VertexTypes.h"
#include <bgfx/bgfx.h>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtx/norm.hpp>
#include <unordered_map>
#include <cmath>

#include "core/utils/DebugModelDump.h"

namespace
{
struct CombinedBlendContext
{
    size_t TotalVertices = 0;
    std::unordered_map<std::string, size_t> NameToIndex;
    BlendShapeComponent Result;

    BlendShape& EnsureShape(const std::string& name)
    {
        auto it = NameToIndex.find(name);
        if (it != NameToIndex.end())
        {
            return Result.Shapes[it->second];
        }

        BlendShape shape;
        shape.Name = name;
        // During merge, use dense storage (we're combining from multiple meshes)
        shape.IsSparse = false;
        shape.DeltaPos.assign(TotalVertices, glm::vec3(0.0f));
        shape.DeltaNormal.assign(TotalVertices, glm::vec3(0.0f));
        shape.Weight = 0.0f;
        Result.Shapes.push_back(std::move(shape));
        size_t idx = Result.Shapes.size() - 1;
        NameToIndex[name] = idx;
        return Result.Shapes[idx];
    }
    
    // After all merging is complete, convert to sparse representation
    void FinalizeToSparse()
    {
        for (auto& shape : Result.Shapes)
        {
            shape.MakeSparse();
        }
    }
};

} // namespace

PreparedModel BuildPreparedModel(const Model& model)
{
    PreparedModel prepared;
    prepared.RootLocal = model.RootLocal;
    prepared.Skeleton.HasSkeleton = !model.BoneNames.empty();
    prepared.Skeleton.BoneNames = model.BoneNames;
    prepared.Skeleton.InverseBindPoses = model.InverseBindPoses;
    if (!model.BoneParents.empty())
    {
        prepared.Skeleton.BoneParents = model.BoneParents;
    }
    else
    {
        prepared.Skeleton.BoneParents.assign(prepared.Skeleton.BoneNames.size(), -1);
    }
    PBRVertex::Init();
    SkinnedPBRVertex::Init();

    // Group original meshes by node/entity name
    std::unordered_map<std::string, std::vector<size_t>> groupByNode;
    std::vector<std::string> groupOrder;
    groupByNode.reserve(model.Meshes.size());
    for (size_t i = 0; i < model.Meshes.size(); ++i)
    {
        std::string key;
        if (i < model.MeshEntityNames.size() && !model.MeshEntityNames[i].empty())
            key = model.MeshEntityNames[i];
        else
            key = std::string("Mesh_") + std::to_string(i);

        auto& bucket = groupByNode[key];
        if (bucket.empty())
        {
            groupOrder.push_back(key);
        }
        bucket.push_back(i);
    }

    prepared.Meshes.reserve(groupOrder.size());
    prepared.Proxies.clear();

    for (const std::string& nodeName : groupOrder)
    {
        const auto it = groupByNode.find(nodeName);
        if (it == groupByNode.end()) continue;

        const std::vector<size_t>& indices = it->second;
        if (indices.empty()) continue;
        std::vector<size_t> filtered;
        filtered = indices;

        size_t primaryIndex = filtered.front();
        std::shared_ptr<Mesh> reference = model.Meshes[primaryIndex];
        if (!reference) continue;

        bool groupIsSkinned = reference->HasSkinning();
        bool mixedSkin = false;
        for (size_t idx : filtered)
        {
            auto mesh = model.Meshes[idx];
            if (!mesh) continue;
            if (mesh->HasSkinning() != groupIsSkinned)
            {
                mixedSkin = true;
                break;

            }
        }

        std::vector<size_t> workList = mixedSkin ? std::vector<size_t>{primaryIndex} : filtered;

        PreparedMeshEntry entry;
        entry.NodeName = nodeName;
        entry.Skinned = groupIsSkinned;
        entry.SourceMeshIndices.reserve(workList.size());
        for (size_t idx : workList) entry.SourceMeshIndices.push_back(static_cast<int>(idx));
        glm::mat4 nodeTransform = glm::mat4(1.0f);
        if (primaryIndex < model.MeshTransforms.size())
            nodeTransform = model.MeshTransforms[primaryIndex];
        entry.LocalTransform = nodeTransform;

        // Build combined mesh
        std::shared_ptr<Mesh> combined = std::make_shared<Mesh>();
        combined->Dynamic = false;
        combined->Submeshes.clear();
        combined->SkinnedLayout = groupIsSkinned;

        uint32_t vertexBase = 0;
        uint32_t indexBase = 0;
        size_t totalVertexCount = 0;
        for (size_t idx : workList)
        {
            if (auto mesh = model.Meshes[idx])
                totalVertexCount += mesh->Vertices.size();
        }

        CombinedBlendContext blendCtx;
        blendCtx.TotalVertices = totalVertexCount;

        for (size_t slot = 0; slot < workList.size(); ++slot)
        {
            size_t idx = workList[slot];
            auto src = model.Meshes[idx];
            if (!src) continue;

        combined->Vertices.insert(combined->Vertices.end(), src->Vertices.begin(), src->Vertices.end());
        combined->Normals.insert(combined->Normals.end(), src->Normals.begin(), src->Normals.end());
        combined->UVs.insert(combined->UVs.end(), src->UVs.begin(), src->UVs.end());

            if (groupIsSkinned)
            {
                combined->BoneWeights.insert(combined->BoneWeights.end(), src->BoneWeights.begin(), src->BoneWeights.end());
                combined->BoneIndices.insert(combined->BoneIndices.end(), src->BoneIndices.begin(), src->BoneIndices.end());
            }

            uint32_t thisBase = vertexBase;
            vertexBase += static_cast<uint32_t>(src->Vertices.size());

            Mesh::Submesh sm;
            sm.baseVertex = thisBase;
            sm.indexStart = indexBase;
            sm.indexCount = static_cast<uint32_t>(src->Indices.size());
            sm.materialSlot = static_cast<uint32_t>(slot);
            combined->Submeshes.push_back(sm);

            for (uint32_t idxVal : src->Indices)
            {
                combined->Indices.push_back(idxVal + thisBase);
            }
            indexBase += sm.indexCount;

            // Capture material source for this slot
            if (idx < model.Materials.size())
                entry.Materials.push_back(model.Materials[idx]);
            else
                entry.Materials.push_back(MaterialSource{});

            if (idx < model.MaterialSlotNames.size())
                entry.MaterialSlotNames.push_back(model.MaterialSlotNames[idx]);
            else
                entry.MaterialSlotNames.emplace_back();

            // Merge blendshapes (handles both sparse and dense input)
            if (idx < model.BlendShapes.size())
            {
                const auto& srcBlend = model.BlendShapes[idx];
                if (!srcBlend.Shapes.empty())
                {
                    size_t offset = sm.baseVertex;
                    for (const auto& shape : srcBlend.Shapes)
                    {
                        BlendShape& dstShape = blendCtx.EnsureShape(shape.Name);
                        
                        if (shape.IsSparse)
                        {
                            // Handle sparse input - copy indexed deltas
                            for (size_t i = 0; i < shape.SparseIndices.size(); ++i)
                            {
                                size_t target = offset + shape.SparseIndices[i];
                                if (target >= dstShape.DeltaPos.size()) continue;
                                dstShape.DeltaPos[target] = shape.SparseDeltaPos[i];
                                if (i < shape.SparseDeltaNorm.size())
                                {
                                    dstShape.DeltaNormal[target] = shape.SparseDeltaNorm[i];
                                }
                            }
                        }
                        else
                        {
                            // Handle dense input (legacy path)
                            if (shape.DeltaPos.size() != src->Vertices.size()) continue;
                            for (size_t v = 0; v < shape.DeltaPos.size(); ++v)
                            {
                                size_t target = offset + v;
                                if (target >= dstShape.DeltaPos.size()) continue;
                                dstShape.DeltaPos[target] = shape.DeltaPos[v];
                            }
                            if (shape.DeltaNormal.size() == src->Vertices.size())
                            {
                                for (size_t v = 0; v < shape.DeltaNormal.size(); ++v)
                                {
                                    size_t target = offset + v;
                                    if (target >= dstShape.DeltaNormal.size()) continue;
                                    dstShape.DeltaNormal[target] = shape.DeltaNormal[v];
                                }
                            }
                        }
                    }
                }
            }
        }
        
        // Convert merged blendshapes to sparse representation for memory efficiency
        blendCtx.FinalizeToSparse();

        combined->ComputeBounds();

        const bgfx::VertexLayout* layoutPtr = nullptr;
        const bgfx::Memory* vbMem = nullptr;
        if (groupIsSkinned)
        {
            std::vector<SkinnedPBRVertex> verts;
            verts.reserve(combined->Vertices.size());
            for (size_t i = 0; i < combined->Vertices.size(); ++i)
            {
                const glm::vec3& p = combined->Vertices[i];
                const glm::vec3& n = (i < combined->Normals.size()) ? combined->Normals[i] : glm::vec3(0, 1, 0);
                const glm::vec2& uv = (i < combined->UVs.size()) ? combined->UVs[i] : glm::vec2(0, 0);
                glm::ivec4 bi = (i < combined->BoneIndices.size()) ? combined->BoneIndices[i] : glm::ivec4(0);
                glm::vec4 bw = (i < combined->BoneWeights.size()) ? combined->BoneWeights[i] : glm::vec4(1, 0, 0, 0);
                verts.push_back({ p.x,p.y,p.z, n.x,n.y,n.z, uv.x,uv.y,
                    (uint8_t)bi.x,(uint8_t)bi.y,(uint8_t)bi.z,(uint8_t)bi.w,
                    bw.x,bw.y,bw.z,bw.w });
            }
            vbMem = bgfx::copy(verts.data(), static_cast<uint32_t>(verts.size() * sizeof(SkinnedPBRVertex)));
            layoutPtr = &SkinnedPBRVertex::layout;
        }
        else
        {
            std::vector<PBRVertex> verts;
            verts.reserve(combined->Vertices.size());
            for (size_t i = 0; i < combined->Vertices.size(); ++i)
            {
                const glm::vec3& p = combined->Vertices[i];
                const glm::vec3& n = (i < combined->Normals.size()) ? combined->Normals[i] : glm::vec3(0, 1, 0);
                const glm::vec2& uv = (i < combined->UVs.size()) ? combined->UVs[i] : glm::vec2(0, 0);
                verts.push_back({ p.x,p.y,p.z, n.x,n.y,n.z, uv.x, uv.y });
            }
            vbMem = bgfx::copy(verts.data(), static_cast<uint32_t>(verts.size() * sizeof(PBRVertex)));
            layoutPtr = &PBRVertex::layout;
        }

        if (groupIsSkinned && !blendCtx.Result.Shapes.empty())
        {
            combined->dvbh = bgfx::createDynamicVertexBuffer(vbMem, *layoutPtr);
            combined->Dynamic = true;
        }
        else if (!blendCtx.Result.Shapes.empty())
        {
            combined->dvbh = bgfx::createDynamicVertexBuffer(vbMem, *layoutPtr);
            combined->Dynamic = true;
        }
        else
        {
            combined->vbh = bgfx::createVertexBuffer(vbMem, *layoutPtr);
            combined->Dynamic = false;
        }

        uint32_t maxIndex = 0;
        for (uint32_t v : combined->Indices) maxIndex = std::max(maxIndex, v);
        if (maxIndex >= 65536u)
        {
            const bgfx::Memory* imem = bgfx::copy(combined->Indices.data(), static_cast<uint32_t>(combined->Indices.size() * sizeof(uint32_t)));
            combined->ibh = bgfx::createIndexBuffer(imem, BGFX_BUFFER_INDEX32);
        }
        else
        {
            std::vector<uint16_t> idx16;
            idx16.reserve(combined->Indices.size());
            for (uint32_t v : combined->Indices) idx16.push_back(static_cast<uint16_t>(v));
            const bgfx::Memory* imem = bgfx::copy(idx16.data(), static_cast<uint32_t>(idx16.size() * sizeof(uint16_t)));
            combined->ibh = bgfx::createIndexBuffer(imem);
        }

        combined->numVertices = static_cast<uint32_t>(combined->Vertices.size());
        combined->numIndices = static_cast<uint32_t>(combined->Indices.size());

        entry.MeshData = combined;
        entry.BlendShapes = std::move(blendCtx.Result);

        size_t meshEntryIndex = prepared.Meshes.size();
        prepared.Meshes.push_back(std::move(entry));

        if (groupIsSkinned)
        {
            PreparedProxyEntry proxy;
            proxy.NodeName = nodeName;
            proxy.DisplayName = nodeName;
            proxy.MeshEntryIndex = meshEntryIndex;
            proxy.Skinned = true;
            proxy.LocalTransform = nodeTransform;
            proxy.OriginalMeshIndex = prepared.Meshes[meshEntryIndex].SourceMeshIndices.empty()
                ? -1
                : prepared.Meshes[meshEntryIndex].SourceMeshIndices.front();
            proxy.SubmeshSlots.clear();
            if (prepared.Meshes[meshEntryIndex].MeshData && !prepared.Meshes[meshEntryIndex].MeshData->Submeshes.empty())
            {
                proxy.SubmeshSlots.reserve(prepared.Meshes[meshEntryIndex].MeshData->Submeshes.size());
                for (uint32_t slot = 0;
                     slot < prepared.Meshes[meshEntryIndex].MeshData->Submeshes.size();
                     ++slot)
                {
                    proxy.SubmeshSlots.push_back(slot);
                }
            }
            prepared.Proxies.push_back(std::move(proxy));
        }
    }

    DebugModelDump::DumpPreparedModel(model, prepared, "BuildPreparedModel");
    return prepared;
}

