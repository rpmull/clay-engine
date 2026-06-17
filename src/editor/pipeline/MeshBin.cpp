#include "MeshBin.h"
#include <fstream>
#include <algorithm>
#include <functional>
#include <filesystem>
#include <memory>
#include "jobs/JobSystem.h"
#include "jobs/Jobs.h"
#include "core/rendering/VertexTypes.h"
#include <bgfx/bgfx.h>
#include "editor/import/ModelPreprocessor.h"
#include "core/rendering/MaterialSource.h"
#include "core/vfs/FileSystem.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include "editor/Project.h"

namespace meshbin {

// Helper: Ensures file is available on disk for reading.
// If file doesn't exist but is in a mounted pak, extracts to temp cache.
// Returns path to use for ifstream (may be original or temp path).
static std::string EnsureFileOnDisk(const std::string& filePath) {
    namespace fs = std::filesystem;
    
    // If file exists on disk, use it directly
    if (fs::exists(filePath)) {
        return filePath;
    }
    
    // Try to extract from virtual filesystem (pak)
    std::vector<uint8_t> data;
    if (FileSystem::Instance().ReadFile(filePath, data)) {
        // Extract to temp cache
        fs::path cacheDir = fs::temp_directory_path() / "claymore_pak_cache";
        std::error_code ec;
        fs::create_directories(cacheDir, ec);
        
        // Create a unique cache path based on the original path
        size_t h = std::hash<std::string>{}(filePath);
        std::string ext = fs::path(filePath).extension().string();
        fs::path cachePath = cacheDir / ("cache_" + std::to_string(h) + ext);
        
        std::ofstream out(cachePath, std::ios::binary | std::ios::trunc);
        if (out.is_open() && !data.empty()) {
            out.write(reinterpret_cast<const char*>(data.data()), data.size());
            out.close();
            return cachePath.string();
        }
    }
    
    // Return original path (will fail to open if doesn't exist)
    return filePath;
}
struct Header { uint32_t magic; uint32_t version; uint32_t submeshCount; };
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
}

namespace {
std::string NormalizeTexturePath(const std::string& p)
{
    if (p.empty()) return p;
    std::string s = p;
    for (char& c : s) if (c == '\\') c = '/';
    try {
        std::error_code ec;
        auto proj = Project::GetProjectDirectory();
        if (!proj.empty()) {
            std::filesystem::path abs = s;
            if (abs.is_relative()) { abs = proj / s; }
            auto rel = std::filesystem::relative(abs, proj, ec);
            if (!ec) {
                std::string v = rel.string();
                for (char& c : v) if (c == '\\') c = '/';
                size_t pos = v.find("assets/");
                if (pos != std::string::npos) return v.substr(pos);
            }
        }
    } catch (...) {}
    return s;
}

void PopulateMaterialHints(const MaterialSource* source,
                           std::string& albedo,
                           std::string& mr,
                           std::string& normal,
                           uint32_t& extrasMask,
                           glm::vec4& extrasTint)
{
    if (!source) return;
    albedo = NormalizeTexturePath(source->Albedo.Path);
    mr     = NormalizeTexturePath(source->MetallicRoughness.Path);
    normal = NormalizeTexturePath(source->Normal.Path);
    if (source->HasTint) { extrasMask |= 1u; extrasTint = source->ColorTint; }
    if (source->TwoSided) extrasMask |= 2u;
}

struct PackedSubmesh
{
    uint32_t indexStart = 0;
    uint32_t indexCount = 0;
    uint32_t baseVertex = 0;
    uint32_t materialSlot = 0;
};

template<typename TStream>
void WriteVec3Array(TStream& out, const std::vector<glm::vec3>& values)
{
    for (const auto& v : values)
    {
        out.write(reinterpret_cast<const char*>(&v.x), sizeof(float));
        out.write(reinterpret_cast<const char*>(&v.y), sizeof(float));
        out.write(reinterpret_cast<const char*>(&v.z), sizeof(float));
    }
}

template<typename TStream>
void ReadVec3Array(TStream& in, std::vector<glm::vec3>& values)
{
    for (auto& v : values)
    {
        in.read(reinterpret_cast<char*>(&v.x), sizeof(float));
        in.read(reinterpret_cast<char*>(&v.y), sizeof(float));
        in.read(reinterpret_cast<char*>(&v.z), sizeof(float));
    }
}

uint32_t WriteSubmeshChunk(std::ostream& out, const std::vector<Mesh::Submesh>& submeshes)
{
    if (submeshes.empty())
    {
        return 0;
    }
    uint32_t count = static_cast<uint32_t>(submeshes.size());
    out.write(reinterpret_cast<const char*>(&count), sizeof(count));
    for (const auto& sm : submeshes)
    {
        PackedSubmesh packed{ sm.indexStart, sm.indexCount, sm.baseVertex, sm.materialSlot };
        out.write(reinterpret_cast<const char*>(&packed), sizeof(packed));
    }
    return sizeof(count) + static_cast<uint32_t>(submeshes.size() * sizeof(PackedSubmesh));
}

void ReadSubmeshChunk(std::istream& in, const meshbin::SubmeshDesc& desc, std::vector<Mesh::Submesh>& out)
{
    if (desc.submeshOffset == 0 || desc.submeshSize == 0)
    {
        out.clear();
        return;
    }
    std::streampos restore = in.tellg();
    in.seekg(desc.submeshOffset, std::ios::beg);
    uint32_t count = 0;
    in.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (!in || count == 0)
    {
        out.clear();
        in.seekg(restore, std::ios::beg);
        return;
    }
    out.resize(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        PackedSubmesh packed{};
        in.read(reinterpret_cast<char*>(&packed), sizeof(packed));
        if (!in)
        {
            out.clear();
            break;
        }
        out[i].indexStart = packed.indexStart;
        out[i].indexCount = packed.indexCount;
        out[i].baseVertex = packed.baseVertex;
        out[i].materialSlot = packed.materialSlot;
    }
    in.seekg(restore, std::ios::beg);
}


uint32_t WriteBlendShapesChunk(std::ostream& out, const BlendShapeComponent& blendComp)
{
    if (blendComp.Shapes.empty())
    {
        return 0;
    }
    uint32_t totalBytes = 0;
    uint32_t shapeCount = static_cast<uint32_t>(blendComp.Shapes.size());
    out.write(reinterpret_cast<const char*>(&shapeCount), sizeof(shapeCount));
    totalBytes += sizeof(shapeCount);
    for (const auto& shape : blendComp.Shapes)
    {
        uint32_t nameLen = static_cast<uint32_t>(shape.Name.size());
        out.write(reinterpret_cast<const char*>(&nameLen), sizeof(nameLen));
        totalBytes += sizeof(nameLen);
        if (nameLen)
        {
            out.write(shape.Name.data(), nameLen);
            totalBytes += nameLen;
        }
        out.write(reinterpret_cast<const char*>(&shape.Weight), sizeof(shape.Weight));
        totalBytes += sizeof(shape.Weight);
        
        // v9+: Write sparse flag and data
        uint8_t isSparse = shape.IsSparse ? 1 : 0;
        out.write(reinterpret_cast<const char*>(&isSparse), sizeof(isSparse));
        totalBytes += sizeof(isSparse);
        
        if (shape.IsSparse)
        {
            // Write sparse data: indices + deltas
            uint32_t sparseCount = static_cast<uint32_t>(shape.SparseIndices.size());
            out.write(reinterpret_cast<const char*>(&sparseCount), sizeof(sparseCount));
            totalBytes += sizeof(sparseCount);
            if (sparseCount)
            {
                out.write(reinterpret_cast<const char*>(shape.SparseIndices.data()), sparseCount * sizeof(uint32_t));
                totalBytes += sparseCount * sizeof(uint32_t);
                WriteVec3Array(out, shape.SparseDeltaPos);
                totalBytes += sparseCount * sizeof(float) * 3;
                WriteVec3Array(out, shape.SparseDeltaNorm);
                totalBytes += sparseCount * sizeof(float) * 3;
            }
        }
        else
        {
            // Write dense data (legacy fallback)
            uint32_t posCount = static_cast<uint32_t>(shape.DeltaPos.size());
            out.write(reinterpret_cast<const char*>(&posCount), sizeof(posCount));
            totalBytes += sizeof(posCount);
            if (posCount)
            {
                WriteVec3Array(out, shape.DeltaPos);
                totalBytes += posCount * sizeof(float) * 3;
            }
            uint32_t normalCount = static_cast<uint32_t>(shape.DeltaNormal.size());
            out.write(reinterpret_cast<const char*>(&normalCount), sizeof(normalCount));
            totalBytes += sizeof(normalCount);
            if (normalCount)
            {
                WriteVec3Array(out, shape.DeltaNormal);
                totalBytes += normalCount * sizeof(float) * 3;
            }
        }
    }
    return totalBytes;
}

std::unique_ptr<BlendShapeComponent> ReadBlendShapesChunk(std::istream& in, const meshbin::SubmeshDesc& desc, uint32_t binVersion)
{
    if (desc.blendOffset == 0 || desc.blendSize == 0)
    {
        return nullptr;
    }
    std::streampos restore = in.tellg();
    in.seekg(desc.blendOffset, std::ios::beg);
    uint32_t shapeCount = 0;
    in.read(reinterpret_cast<char*>(&shapeCount), sizeof(shapeCount));
    if (!in || shapeCount == 0)
    {
        in.seekg(restore, std::ios::beg);
        return nullptr;
    }
    auto component = std::make_unique<BlendShapeComponent>();
    component->Shapes.resize(shapeCount);
    for (uint32_t i = 0; i < shapeCount; ++i)
    {
        uint32_t nameLen = 0;
        in.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
        if (!in)
        {
            component.reset();
            break;
        }
        std::string name;
        name.resize(nameLen);
        if (nameLen)
        {
            in.read(name.data(), nameLen);
            if (!in)
            {
                component.reset();
                break;
            }
        }
        float weight = 0.0f;
        in.read(reinterpret_cast<char*>(&weight), sizeof(weight));
        if (!in)
        {
            component.reset();
            break;
        }
        
        auto& shape = component->Shapes[i];
        shape.Name = std::move(name);
        shape.Weight = weight;
        
        // v9+: Read sparse flag
        if (binVersion >= 9)
        {
            uint8_t isSparse = 0;
            in.read(reinterpret_cast<char*>(&isSparse), sizeof(isSparse));
            if (!in)
            {
                component.reset();
                break;
            }
            shape.IsSparse = (isSparse != 0);
            
            if (shape.IsSparse)
            {
                // Read sparse data
                uint32_t sparseCount = 0;
                in.read(reinterpret_cast<char*>(&sparseCount), sizeof(sparseCount));
                if (!in)
                {
                    component.reset();
                    break;
                }
                if (sparseCount)
                {
                    shape.SparseIndices.resize(sparseCount);
                    in.read(reinterpret_cast<char*>(shape.SparseIndices.data()), sparseCount * sizeof(uint32_t));
                    if (!in) { component.reset(); break; }
                    shape.SparseDeltaPos.resize(sparseCount);
                    ReadVec3Array(in, shape.SparseDeltaPos);
                    if (!in) { component.reset(); break; }
                    shape.SparseDeltaNorm.resize(sparseCount);
                    ReadVec3Array(in, shape.SparseDeltaNorm);
                    if (!in) { component.reset(); break; }
                }
            }
            else
            {
                // Read dense data
                uint32_t posCount = 0;
                in.read(reinterpret_cast<char*>(&posCount), sizeof(posCount));
                if (!in) { component.reset(); break; }
                shape.DeltaPos.resize(posCount);
                if (posCount)
                {
                    ReadVec3Array(in, shape.DeltaPos);
                    if (!in) { component.reset(); break; }
                }
                uint32_t normalCount = 0;
                in.read(reinterpret_cast<char*>(&normalCount), sizeof(normalCount));
                if (!in) { component.reset(); break; }
                shape.DeltaNormal.resize(normalCount);
                if (normalCount)
                {
                    ReadVec3Array(in, shape.DeltaNormal);
                    if (!in) { component.reset(); break; }
                }
            }
        }
        else
        {
            // v8 and earlier: Read dense data only, then convert to sparse
            uint32_t posCount = 0;
            in.read(reinterpret_cast<char*>(&posCount), sizeof(posCount));
            if (!in)
            {
                component.reset();
                break;
            }
            shape.DeltaPos.resize(posCount);
            if (posCount)
            {
                ReadVec3Array(in, shape.DeltaPos);
                if (!in)
                {
                    component.reset();
                    break;
                }
            }
            uint32_t normalCount = 0;
            in.read(reinterpret_cast<char*>(&normalCount), sizeof(normalCount));
            if (!in)
            {
                component.reset();
                break;
            }
            shape.DeltaNormal.resize(normalCount);
            if (normalCount)
            {
                ReadVec3Array(in, shape.DeltaNormal);
                if (!in)
                {
                    component.reset();
                    break;
                }
            }
            // Convert legacy dense data to sparse for runtime efficiency
            shape.MakeSparse();
        }
    }
    in.seekg(restore, std::ios::beg);
    return component;
}
} // namespace

namespace meshbin {

bool WriteMeshBinFromModel(const Model& model, const std::string& filePath) {
    try {
        std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        Header h{ MESH_BIN_MAGIC, MESH_BIN_VERSION, (uint32_t)model.Meshes.size() };
        out.write((const char*)&h, sizeof(h));

        // Reserve space for descriptors
        std::vector<SubmeshDesc> descs(model.Meshes.size());
        size_t tablePos = (size_t)out.tellp();
        out.write((const char*)descs.data(), (std::streamsize)(descs.size() * sizeof(SubmeshDesc)));

        // Write VB/IB blobs sequentially
        std::vector<size_t> vbPos(model.Meshes.size(), 0), ibPos(model.Meshes.size(), 0), namePos(model.Meshes.size(), 0), texPos(model.Meshes.size(), 0), texSize(model.Meshes.size(), 0), extrasPos(model.Meshes.size(), 0), extrasSize(model.Meshes.size(), 0);
        for (size_t i = 0; i < model.Meshes.size(); ++i) {
            const auto& m = model.Meshes[i]; if (!m) { descs[i] = {}; continue; }
            const bool skinned = m->HasSkinning();
            // Interleave into PBR or SkinnedPBR layout
            if (skinned) {
                std::vector<SkinnedPBRVertex> verts; verts.reserve(m->Vertices.size());
                for (size_t v=0; v<m->Vertices.size(); ++v) {
                    glm::vec3 p = m->Vertices[v];
                    glm::vec3 n = (v < m->Normals.size()? m->Normals[v] : glm::vec3(0,1,0));
                    glm::vec2 uv = (v < m->UVs.size()? m->UVs[v] : glm::vec2(0,0));
                    glm::ivec4 bi = (v < m->BoneIndices.size()? m->BoneIndices[v] : glm::ivec4(0));
                    glm::vec4  bw = (v < m->BoneWeights.size()? m->BoneWeights[v] : glm::vec4(1,0,0,0));
                    verts.push_back({ p.x,p.y,p.z, n.x,n.y,n.z, uv.x,uv.y, (uint8_t)bi.x,(uint8_t)bi.y,(uint8_t)bi.z,(uint8_t)bi.w, bw.x,bw.y,bw.z,bw.w });
                }
                vbPos[i] = (size_t)out.tellp();
                out.write((const char*)verts.data(), (std::streamsize)(verts.size() * sizeof(SkinnedPBRVertex)));
            } else {
                std::vector<PBRVertex> verts; verts.reserve(m->Vertices.size());
                for (size_t v=0; v<m->Vertices.size(); ++v) {
                    glm::vec3 p = m->Vertices[v];
                    glm::vec3 n = (v < m->Normals.size()? m->Normals[v] : glm::vec3(0,1,0));
                    glm::vec2 uv = (v < m->UVs.size()? m->UVs[v] : glm::vec2(0,0));
                    verts.push_back({ p.x,p.y,p.z, n.x,n.y,n.z, uv.x,uv.y });
                }
                vbPos[i] = (size_t)out.tellp();
                out.write((const char*)verts.data(), (std::streamsize)(verts.size() * sizeof(PBRVertex)));
            }
            // Indices: prefer 16-bit if possible
            bool use16 = true; for (uint32_t idx : m->Indices) { if (idx >= 65536u) { use16 = false; break; } }
            ibPos[i] = (size_t)out.tellp();
            if (use16) {
                std::vector<uint16_t> idx16; idx16.reserve(m->Indices.size());
                for (uint32_t idx : m->Indices) idx16.push_back((uint16_t)idx);
                out.write((const char*)idx16.data(), (std::streamsize)(idx16.size() * sizeof(uint16_t)));
            } else {
                out.write((const char*)m->Indices.data(), (std::streamsize)(m->Indices.size() * sizeof(uint32_t)));
            }

            SubmeshDesc d{};
            d.vertexCount = (uint32_t)m->Vertices.size();
            d.indexCount = (uint32_t)m->Indices.size();
            d.vertexStride = (uint32_t)(skinned ? sizeof(SkinnedPBRVertex) : sizeof(PBRVertex));
            d.indexSize = use16 ? 2u : 4u;
            d.hasSkinning = skinned ? 1u : 0u;
            d.vbOffset = (uint32_t)vbPos[i];
            d.ibOffset = (uint32_t)ibPos[i];
            d.bmin = m->BoundsMin;
            d.bmax = m->BoundsMax;
            // Submesh name: try Model.MeshEntityNames if available
            std::string nameStr;
            if (i < model.MeshEntityNames.size() && !model.MeshEntityNames[i].empty()) nameStr = model.MeshEntityNames[i];
            if (nameStr.empty()) nameStr = std::string("Mesh_") + std::to_string(i);
            namePos[i] = (size_t)out.tellp();
            uint32_t nameLen = (uint32_t)nameStr.size();
            out.write((const char*)&nameLen, sizeof(nameLen));
            if (nameLen) out.write(nameStr.data(), nameLen);
            d.nameOffset = (uint32_t)namePos[i]; d.nameLength = nameLen;

            // Texture hints: pull from Model.Materials PBR paths when possible
            std::string albedo, mr, normal;
            const MaterialSource* matSrc = (i < model.Materials.size()) ? &model.Materials[i] : nullptr;
            uint32_t extrasMask = 0;
            glm::vec4 extrasTint(1.0f);
            PopulateMaterialHints(matSrc, albedo, mr, normal, extrasMask, extrasTint);
            std::string packed;
            // Serialize as length-prefixed strings: [len][bytes]*3
            auto appendStr = [&](const std::string& s){ uint32_t L=(uint32_t)s.size(); packed.append((const char*)&L, sizeof(L)); if(L) packed.append(s.data(), L); };
            appendStr(albedo); appendStr(mr); appendStr(normal);
            texPos[i] = (size_t)out.tellp();
            if (!packed.empty()) out.write(packed.data(), (std::streamsize)packed.size());
            texSize[i] = (uint32_t)packed.size();
            d.texOffset = (uint32_t)texPos[i]; d.texSize = (uint32_t)texSize[i];
            // Material extras (tint/twoSided)
            struct Extras { uint32_t mask; glm::vec4 tint; } ex{extrasMask, extrasTint};
            extrasPos[i] = (size_t)out.tellp();
            out.write((const char*)&ex, sizeof(ex));
            extrasSize[i] = (uint32_t)sizeof(ex);
            d.extrasOffset = (uint32_t)extrasPos[i]; d.extrasSize = (uint32_t)extrasSize[i];

            // Quantization info placeholder (future use): for now write zeros so v2 readers skip gracefully
            struct QuantInfo { uint32_t flags; glm::vec3 posMin; glm::vec3 posMax; } qi{0, m->BoundsMin, m->BoundsMax};
            size_t qpos = (size_t)out.tellp();
            out.write((const char*)&qi, sizeof(qi));
            d.quantInfoOffset = (uint32_t)qpos; d.quantInfoSize = (uint32_t)sizeof(qi);

            // Local transform: bake from Model.MeshTransforms if available
            struct PackedXform { glm::vec3 t; glm::quat r; glm::vec3 s; } px{ glm::vec3(0), glm::quat(1,0,0,0), glm::vec3(1) };
            if (i < model.MeshTransforms.size()) {
                glm::vec3 t, s, skew; glm::vec4 persp; glm::quat rq;
                glm::decompose(model.MeshTransforms[i], s, rq, t, skew, persp);
                px.t = t; px.r = rq; px.s = s;
            }
            size_t xfpos = (size_t)out.tellp();
            out.write((const char*)&px, sizeof(px));
            d.xformOffset = (uint32_t)xfpos; d.xformSize = (uint32_t)sizeof(px);

            size_t subPos = (size_t)out.tellp();
            uint32_t subBytes = WriteSubmeshChunk(out, m->Submeshes);
            d.submeshOffset = subBytes ? (uint32_t)subPos : 0u;
            d.submeshSize = subBytes;

            size_t blendPos = (size_t)out.tellp();
            uint32_t blendBytes = 0;
            if (i < model.BlendShapes.size())
            {
                blendBytes = WriteBlendShapesChunk(out, model.BlendShapes[i]);
            }
            d.blendOffset = blendBytes ? (uint32_t)blendPos : 0u;
            d.blendSize = blendBytes;

            descs[i] = d;
        }

        // Write descriptor table
        size_t endPos = (size_t)out.tellp();
        out.seekp((std::streamoff)tablePos, std::ios::beg);
        out.write((const char*)descs.data(), (std::streamsize)(descs.size() * sizeof(SubmeshDesc)));
        out.seekp((std::streamoff)endPos, std::ios::beg);
        return true;
    } catch (...) {
        return false;
    }
}

bool WriteMeshBinFromPrepared(const PreparedModel& prepared, const std::string& filePath)
{
    try {
        std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        Header h{ MESH_BIN_MAGIC, MESH_BIN_VERSION, (uint32_t)prepared.Meshes.size() };
        out.write((const char*)&h, sizeof(h));

        std::vector<SubmeshDesc> descs(prepared.Meshes.size());
        size_t tablePos = (size_t)out.tellp();
        out.write((const char*)descs.data(), (std::streamsize)(descs.size() * sizeof(SubmeshDesc)));

        std::vector<size_t> vbPos(prepared.Meshes.size(), 0), ibPos(prepared.Meshes.size(), 0), namePos(prepared.Meshes.size(), 0), texPos(prepared.Meshes.size(), 0), texSize(prepared.Meshes.size(), 0), extrasPos(prepared.Meshes.size(), 0), extrasSize(prepared.Meshes.size(), 0);
        for (size_t i = 0; i < prepared.Meshes.size(); ++i) {
            const auto& entry = prepared.Meshes[i];
            const auto& m = entry.MeshData;
            if (!m) { descs[i] = {}; continue; }
            const bool skinned = entry.Skinned || m->HasSkinning();

            if (skinned) {
                std::vector<SkinnedPBRVertex> verts; verts.reserve(m->Vertices.size());
                for (size_t v=0; v<m->Vertices.size(); ++v) {
                    glm::vec3 p = m->Vertices[v];
                    glm::vec3 n = (v < m->Normals.size()? m->Normals[v] : glm::vec3(0,1,0));
                    glm::vec2 uv = (v < m->UVs.size()? m->UVs[v] : glm::vec2(0,0));
                    glm::ivec4 bi = (v < m->BoneIndices.size()? m->BoneIndices[v] : glm::ivec4(0));
                    glm::vec4  bw = (v < m->BoneWeights.size()? m->BoneWeights[v] : glm::vec4(1,0,0,0));
                    verts.push_back({ p.x,p.y,p.z, n.x,n.y,n.z, uv.x,uv.y, (uint8_t)bi.x,(uint8_t)bi.y,(uint8_t)bi.z,(uint8_t)bi.w, bw.x,bw.y,bw.z,bw.w });
                }
                vbPos[i] = (size_t)out.tellp();
                out.write((const char*)verts.data(), (std::streamsize)(verts.size() * sizeof(SkinnedPBRVertex)));
            } else {
                std::vector<PBRVertex> verts; verts.reserve(m->Vertices.size());
                for (size_t v=0; v<m->Vertices.size(); ++v) {
                    glm::vec3 p = m->Vertices[v];
                    glm::vec3 n = (v < m->Normals.size()? m->Normals[v] : glm::vec3(0,1,0));
                    glm::vec2 uv = (v < m->UVs.size()? m->UVs[v] : glm::vec2(0,0));
                    verts.push_back({ p.x,p.y,p.z, n.x,n.y,n.z, uv.x,uv.y });
                }
                vbPos[i] = (size_t)out.tellp();
                out.write((const char*)verts.data(), (std::streamsize)(verts.size() * sizeof(PBRVertex)));
            }

            bool use16 = true; for (uint32_t idx : m->Indices) { if (idx >= 65536u) { use16 = false; break; } }
            ibPos[i] = (size_t)out.tellp();
            if (use16) {
                std::vector<uint16_t> idx16; idx16.reserve(m->Indices.size());
                for (uint32_t idx : m->Indices) idx16.push_back((uint16_t)idx);
                out.write((const char*)idx16.data(), (std::streamsize)(idx16.size() * sizeof(uint16_t)));
            } else {
                out.write((const char*)m->Indices.data(), (std::streamsize)(m->Indices.size() * sizeof(uint32_t)));
            }

            SubmeshDesc d{};
            d.vertexCount = (uint32_t)m->Vertices.size();
            d.indexCount = (uint32_t)m->Indices.size();
            d.vertexStride = (uint32_t)(skinned ? sizeof(SkinnedPBRVertex) : sizeof(PBRVertex));
            d.indexSize = use16 ? 2u : 4u;
            d.hasSkinning = skinned ? 1u : 0u;
            d.vbOffset = (uint32_t)vbPos[i];
            d.ibOffset = (uint32_t)ibPos[i];
            d.bmin = m->BoundsMin;
            d.bmax = m->BoundsMax;

            std::string nameStr = entry.NodeName.empty() ? std::string("Mesh_") + std::to_string(i) : entry.NodeName;
            namePos[i] = (size_t)out.tellp();
            uint32_t nameLen = (uint32_t)nameStr.size();
            out.write((const char*)&nameLen, sizeof(nameLen));
            if (nameLen) out.write(nameStr.data(), nameLen);
            d.nameOffset = (uint32_t)namePos[i]; d.nameLength = nameLen;

            std::string albedo, mr, normal;
            const MaterialSource* matSrcPrepared = entry.Materials.empty() ? nullptr : &entry.Materials.front();
            uint32_t extrasMaskPrepared = 0;
            glm::vec4 extrasTintPrepared(1.0f);
            PopulateMaterialHints(matSrcPrepared, albedo, mr, normal, extrasMaskPrepared, extrasTintPrepared);
            std::string packed;
            auto appendStr = [&](const std::string& s){ uint32_t L=(uint32_t)s.size(); packed.append((const char*)&L, sizeof(L)); if(L) packed.append(s.data(), L); };
            appendStr(albedo); appendStr(mr); appendStr(normal);
            texPos[i] = (size_t)out.tellp();
            if (!packed.empty()) out.write(packed.data(), (std::streamsize)packed.size());
            texSize[i] = (uint32_t)packed.size();
            d.texOffset = (uint32_t)texPos[i]; d.texSize = (uint32_t)texSize[i];

            struct Extras { uint32_t mask; glm::vec4 tint; } ex{extrasMaskPrepared, extrasTintPrepared};
            extrasPos[i] = (size_t)out.tellp();
            out.write((const char*)&ex, sizeof(ex));
            extrasSize[i] = (uint32_t)sizeof(ex);
            d.extrasOffset = (uint32_t)extrasPos[i]; d.extrasSize = (uint32_t)extrasSize[i];

            struct QuantInfo { uint32_t flags; glm::vec3 posMin; glm::vec3 posMax; } qi{0, m->BoundsMin, m->BoundsMax};
            size_t qpos = (size_t)out.tellp();
            out.write((const char*)&qi, sizeof(qi));
            d.quantInfoOffset = (uint32_t)qpos; d.quantInfoSize = (uint32_t)sizeof(qi);

            struct PackedMatrix { float values[16]; } px{};
            {
                for (int c = 0; c < 4; ++c) {
                    for (int r = 0; r < 4; ++r) {
                        px.values[c * 4 + r] = entry.LocalTransform[c][r];
                    }
                }
            }
            size_t xfpos = (size_t)out.tellp();
            out.write((const char*)&px, sizeof(px));
            d.xformOffset = (uint32_t)xfpos; d.xformSize = (uint32_t)sizeof(px);

            size_t subPos = (size_t)out.tellp();
            uint32_t subBytes = WriteSubmeshChunk(out, m->Submeshes);
            d.submeshOffset = subBytes ? (uint32_t)subPos : 0u;
            d.submeshSize = subBytes;

            size_t blendPos = (size_t)out.tellp();
            uint32_t blendBytes = WriteBlendShapesChunk(out, entry.BlendShapes);
            d.blendOffset = blendBytes ? (uint32_t)blendPos : 0u;
            d.blendSize = blendBytes;

            descs[i] = d;
        }

        size_t endPos = (size_t)out.tellp();
        out.seekp((std::streamoff)tablePos, std::ios::beg);
        out.write((const char*)descs.data(), (std::streamsize)(descs.size() * sizeof(SubmeshDesc)));
        out.seekp((std::streamoff)endPos, std::ios::beg);
        return true;
    } catch (...) {
        return false;
    }
}

uint32_t GetSubmeshCount(const std::string& filePath) {
    std::string diskPath = EnsureFileOnDisk(filePath);
    std::ifstream in(diskPath, std::ios::binary); if (!in.is_open()) return 0;
    Header h{}; in.read((char*)&h, sizeof(h));
    if (h.magic != MESH_BIN_MAGIC || h.version != MESH_BIN_VERSION) return 0;
    return h.submeshCount;
}

std::shared_ptr<Mesh> ReadMeshFromBin(const std::string& filePath,
                                      uint32_t submeshIndex,
                                      bool& outSkinned,
                                      std::unique_ptr<BlendShapeComponent>* outBlendShapes) {
    outSkinned = false;
    if (outBlendShapes)
    {
        outBlendShapes->reset();
    }
    // Ensure vertex layouts are initialized even if this runs on a worker thread
    static std::once_flag s_once;
    std::call_once(s_once, [](){ PBRVertex::Init(); SkinnedPBRVertex::Init(); });
    std::string diskPath = EnsureFileOnDisk(filePath);
    std::ifstream in(diskPath, std::ios::binary | std::ios::ate); if (!in.is_open()) return nullptr;
    size_t sz = (size_t)in.tellg(); in.seekg(0);
    if (sz < sizeof(Header)) return nullptr;
    Header h{}; in.read((char*)&h, sizeof(h));
    // Accept current version and v8 (pre-sparse blendshapes) for backward compatibility
    if (h.magic != MESH_BIN_MAGIC || (h.version != MESH_BIN_VERSION && h.version != 8)) return nullptr;
    std::vector<SubmeshDesc> descs(h.submeshCount);
    in.read((char*)descs.data(), (std::streamsize)(descs.size() * sizeof(SubmeshDesc)));
    if (submeshIndex >= descs.size()) return nullptr;
    const SubmeshDesc& d = descs[submeshIndex];
    outSkinned = (d.hasSkinning != 0);

    // Read VB
    std::vector<uint8_t> vb(d.vertexCount * d.vertexStride);
    in.seekg(d.vbOffset, std::ios::beg);
    in.read((char*)vb.data(), (std::streamsize)vb.size());
    // Read IB
    std::vector<uint8_t> ib(d.indexCount * d.indexSize);
    in.seekg(d.ibOffset, std::ios::beg);
    in.read((char*)ib.data(), (std::streamsize)ib.size());

    auto mesh = std::make_shared<Mesh>();
    mesh->numVertices = d.vertexCount;
    mesh->numIndices = d.indexCount;
    mesh->BoundsMin = d.bmin;
    mesh->BoundsMax = d.bmax;

    // Reconstruct CPU-side vertex data for parity with slow path
    mesh->Vertices.resize(d.vertexCount);
    mesh->Normals.resize(d.vertexCount);
    mesh->UVs.resize(d.vertexCount);

    if (outSkinned) {
        const SkinnedPBRVertex* verts = reinterpret_cast<const SkinnedPBRVertex*>(vb.data());
        mesh->BoneIndices.resize(d.vertexCount);
        mesh->BoneWeights.resize(d.vertexCount);
        for (uint32_t i = 0; i < d.vertexCount; ++i) {
            const SkinnedPBRVertex& v = verts[i];
            mesh->Vertices[i] = glm::vec3(v.x, v.y, v.z);
            mesh->Normals[i]  = glm::vec3(v.nx, v.ny, v.nz);
            mesh->UVs[i]      = glm::vec2(v.u, v.v);
            mesh->BoneIndices[i] = glm::ivec4((int)v.i0, (int)v.i1, (int)v.i2, (int)v.i3);
            mesh->BoneWeights[i] = glm::vec4(v.w0, v.w1, v.w2, v.w3);
        }
    } else {
        const PBRVertex* verts = reinterpret_cast<const PBRVertex*>(vb.data());
        for (uint32_t i = 0; i < d.vertexCount; ++i) {
            const PBRVertex& v = verts[i];
            mesh->Vertices[i] = glm::vec3(v.x, v.y, v.z);
            mesh->Normals[i]  = glm::vec3(v.nx, v.ny, v.nz);
            mesh->UVs[i]      = glm::vec2(v.u, v.v);
        }
    }

    // Reconstruct CPU-side indices
    mesh->Indices.resize(d.indexCount);
    if (d.indexSize == 2) {
        const uint16_t* idx = reinterpret_cast<const uint16_t*>(ib.data());
        for (uint32_t i = 0; i < d.indexCount; ++i) {
            mesh->Indices[i] = (uint32_t)idx[i];
        }
    } else {
        const uint32_t* idx = reinterpret_cast<const uint32_t*>(ib.data());
        for (uint32_t i = 0; i < d.indexCount; ++i) {
            mesh->Indices[i] = idx[i];
        }
    }

    const bgfx::Memory* vbMem = bgfx::copy(vb.data(), (uint32_t)vb.size());
    if (outSkinned) {
        mesh->SkinnedLayout = true;
        // Only skinned meshes that deform on the CPU (morph targets / blend
        // shapes) need a dynamic vertex buffer. Plain skinned meshes are
        // deformed entirely on the GPU from a static bind-pose buffer plus the
        // bone palette, exactly like meshes imported through ModelLoader.
        // Previously every skinned mesh was forced Dynamic with no static vbh,
        // which left the depth/shadow path with only the dvbh branch: any part
        // lacking a live GPU skinning record (or an up-to-date dvbh) was
        // silently skipped, so a multi-part character cast only one part's
        // shadow. Mirror ModelLoader's rule and key Dynamic off real morphs.
        const bool hasMorphTargets = (d.blendSize > 0);
        if (hasMorphTargets) {
            mesh->vbh = BGFX_INVALID_HANDLE;
            mesh->dvbh = bgfx::createDynamicVertexBuffer(vbMem, SkinnedPBRVertex::layout);
            mesh->Dynamic = true;
        } else {
            mesh->vbh = bgfx::createVertexBuffer(vbMem, SkinnedPBRVertex::layout);
            mesh->dvbh = BGFX_INVALID_HANDLE;
            mesh->Dynamic = false;
        }
    } else {
        mesh->vbh = bgfx::createVertexBuffer(vbMem, PBRVertex::layout);
        mesh->Dynamic = false;
        mesh->SkinnedLayout = false;
    }

    const bgfx::Memory* ibMem = bgfx::copy(ib.data(), (uint32_t)ib.size());
    if (d.indexSize == 2)
        mesh->ibh = bgfx::createIndexBuffer(ibMem);
    else
        mesh->ibh = bgfx::createIndexBuffer(ibMem, BGFX_BUFFER_INDEX32);

    ReadSubmeshChunk(in, d, mesh->Submeshes);
    if (mesh->Submeshes.empty())
    {
        Mesh::Submesh sm;
        sm.indexStart = 0;
        sm.indexCount = d.indexCount;
        sm.baseVertex = 0;
        sm.materialSlot = 0;
        mesh->Submeshes = { sm };
    }

    if (outBlendShapes)
    {
        *outBlendShapes = ReadBlendShapesChunk(in, d, h.version);
    }
    return mesh;
}

std::unique_ptr<BlendShapeComponent> ReadBlendShapesFromBin(const std::string& filePath,
                                                            uint32_t submeshIndex)
{
    std::ifstream in(filePath, std::ios::binary);
    if (!in.is_open())
    {
        return nullptr;
    }
    Header h{};
    in.read(reinterpret_cast<char*>(&h), sizeof(h));
    // Accept current version and v8 (pre-sparse blendshapes) for backward compatibility
    if (h.magic != MESH_BIN_MAGIC || (h.version != MESH_BIN_VERSION && h.version != 8))
    {
        return nullptr;
    }
    std::vector<SubmeshDesc> descs(h.submeshCount);
    in.read(reinterpret_cast<char*>(descs.data()), (std::streamsize)(descs.size() * sizeof(SubmeshDesc)));
    if (submeshIndex >= descs.size())
    {
        return nullptr;
    }
    return ReadBlendShapesChunk(in, descs[submeshIndex], h.version);
}

}

using namespace meshbin;

static bool readHeaderAndDescs(const std::string& filePath, Header& h, std::vector<SubmeshDesc>& descs) {
    std::string diskPath = meshbin::EnsureFileOnDisk(filePath);
    std::ifstream in(diskPath, std::ios::binary); if (!in.is_open()) return false;
    in.read((char*)&h, sizeof(h)); if (h.magic != MESH_BIN_MAGIC) return false;
    descs.resize(h.submeshCount); in.read((char*)descs.data(), (std::streamsize)(descs.size() * sizeof(SubmeshDesc)));
    return true;
}

namespace meshbin {

bool HasCurrentVersion(const std::string& filePath) {
    std::ifstream in(filePath, std::ios::binary);
    if (!in.is_open()) return false;
    Header h{};
    in.read((char*)&h, sizeof(h));
    if (!in) return false;
    return h.magic == MESH_BIN_MAGIC && h.version == MESH_BIN_VERSION;
}

std::string GetSubmeshName(const std::string& filePath, uint32_t submeshIndex) {
    Header h{}; std::vector<SubmeshDesc> d; if (!readHeaderAndDescs(filePath, h, d) || submeshIndex >= d.size()) return {};
    std::ifstream in(filePath, std::ios::binary); if (!in.is_open()) return {};
    const SubmeshDesc& sd = d[submeshIndex];
    if (sd.nameLength == 0) return {};
    in.seekg(sd.nameOffset, std::ios::beg);
    uint32_t len=0; in.read((char*)&len, sizeof(len)); if (len == 0) return {};
    std::string s; s.resize(len); in.read(s.data(), len); return s;
}

TextureHints GetSubmeshTextureHints(const std::string& filePath, uint32_t submeshIndex) {
    Header h{}; std::vector<SubmeshDesc> d; if (!readHeaderAndDescs(filePath, h, d) || submeshIndex >= d.size()) return {};
    std::ifstream in(filePath, std::ios::binary); if (!in.is_open()) return {};
    const SubmeshDesc& sd = d[submeshIndex]; TextureHints th{};
    if (sd.texSize == 0) return th;
    in.seekg(sd.texOffset, std::ios::beg);
    auto readStr = [&](std::string& out){ uint32_t L=0; in.read((char*)&L, sizeof(L)); out.resize(L); if (L) in.read(out.data(), L); };
    readStr(th.albedo); readStr(th.metallicRoughness); readStr(th.normal);
    return th;
}

MaterialExtras GetSubmeshMaterialExtras(const std::string& filePath, uint32_t submeshIndex) {
    Header h{}; std::vector<SubmeshDesc> d; if (!readHeaderAndDescs(filePath, h, d) || submeshIndex >= d.size()) return {};
    std::ifstream in(filePath, std::ios::binary); if (!in.is_open()) return {};
    const SubmeshDesc& sd = d[submeshIndex]; MaterialExtras me{};
    if (sd.extrasSize == 0) return me;
    in.seekg(sd.extrasOffset, std::ios::beg);
    struct Extras { uint32_t mask; glm::vec4 tint; } ex{0, glm::vec4(1.0f)};
    in.read((char*)&ex, sizeof(ex));
    if (ex.mask & 1u) { me.hasTint = true; me.tint = ex.tint; }
    if (ex.mask & 2u) { me.twoSided = true; }
    return me;
}

TransformInfo GetSubmeshLocalTransform(const std::string& filePath, uint32_t submeshIndex) {
    Header h{}; std::vector<SubmeshDesc> d; if (!readHeaderAndDescs(filePath, h, d) || submeshIndex >= d.size()) return {};
    std::ifstream in(filePath, std::ios::binary); if (!in.is_open()) return {};
    const SubmeshDesc& sd = d[submeshIndex]; TransformInfo ti{};
    if (sd.xformSize == 0) return ti;
    in.seekg(sd.xformOffset, std::ios::beg);
    struct PackedMatrix { float values[16]; } px{};
    in.read((char*)&px, sizeof(px));
    ti.has = true;
    glm::mat4 mat(1.0f);
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            mat[c][r] = px.values[c * 4 + r];
        }
    }
    ti.matrix = mat;
    glm::vec3 t(0.0f), s(1.0f), skew(0.0f);
    glm::vec4 persp(0.0f);
    glm::quat rq(1.0f, 0.0f, 0.0f, 0.0f);
    glm::decompose(mat, s, rq, t, skew, persp);
    ti.t = t;
    ti.r = glm::normalize(rq);
    ti.s = s;
    return ti;
}

}

namespace meshbin {

void PrefetchMeshesAsync(const PrefetchRequest& req, std::function<void(PrefetchResult&&)> onComplete) {
    // Snapshot paths/indices for the background job
    PrefetchRequest copy = req;
    Jobs().Enqueue([copy, onComplete] {
        PrefetchResult r;
        r.meshes.reserve(copy.submeshIndices.size());
        r.blendShapes.reserve(copy.submeshIndices.size());
        for (uint32_t i : copy.submeshIndices) {
            bool s=false;
            std::unique_ptr<BlendShapeComponent> blend;
            auto m = ReadMeshFromBin(copy.path, i, s, &blend);
            r.meshes.push_back(std::move(m));
            r.blendShapes.push_back(std::move(blend));
        }
        // Fire callback on completion (note: current design calls from worker thread)
        onComplete(std::move(r));
    });
}

}


