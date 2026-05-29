#include "River.h"
#include "core/ecs/Components.h"
#include "core/ecs/Scene.h"
#include "core/ecs/EntityData.h"
#include "core/rendering/Mesh.h"
#include "core/rendering/VertexTypes.h"
#include "core/rendering/MaterialManager.h"
#include "core/rendering/PBRMaterial.h"
#include "core/vfs/FileSystem.h"
#include <bgfx/bgfx.h>

#include <fstream>
#include <filesystem>
#include <iostream>
#include <cstring>

namespace fs = std::filesystem;

namespace
{
    // River mesh asset format
    static constexpr char kRiverMeshMagic[4] = { 'R', 'V', 'M', 'S' };
    static constexpr uint32_t kRiverMeshVersion = 1;
    
    struct RiverMeshHeader
    {
        char Magic[4];
        uint32_t Version;
        uint32_t VertexCount;
        uint32_t IndexCount;
        float BoundsMin[3];
        float BoundsMax[3];
    };
    
    // Catmull-Rom spline interpolation
    glm::vec3 CatmullRom(const glm::vec3& p0, const glm::vec3& p1, 
                         const glm::vec3& p2, const glm::vec3& p3, float t)
    {
        float t2 = t * t;
        float t3 = t2 * t;
        
        return 0.5f * ((2.0f * p1) +
                       (-p0 + p2) * t +
                       (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                       (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
    }
}

bool River::SaveRiverMeshAsset(RiverComponent& river,
                                const std::vector<glm::vec3>& vertices,
                                const std::vector<glm::vec3>& normals,
                                const std::vector<glm::vec2>& uvs,
                                const std::vector<uint32_t>& indices)
{
    if (vertices.empty() || indices.empty())
        return false;
    
    // Build default path if not set
    if (river.MeshAssetPath.empty())
    {
        river.MeshAssetPath = RiverComponent::BuildDefaultMeshAssetPath(river.MeshAssetGuid);
    }
    
    // Resolve path relative to project root
    fs::path diskPath(river.MeshAssetPath);
    if (diskPath.is_relative())
    {
        const fs::path& projectRoot = FileSystem::Instance().GetProjectRoot();
        if (!projectRoot.empty())
        {
            diskPath = projectRoot / diskPath;
        }
    }
    
    // Create directories
    if (diskPath.has_parent_path())
    {
        std::error_code ec;
        fs::create_directories(diskPath.parent_path(), ec);
    }
    
    std::ofstream stream(diskPath, std::ios::binary | std::ios::trunc);
    if (!stream.is_open())
    {
        std::cerr << "[River] Failed to open river mesh asset for writing: " << diskPath.string() << std::endl;
        return false;
    }
    
    // Compute bounds
    glm::vec3 boundsMin(std::numeric_limits<float>::max());
    glm::vec3 boundsMax(std::numeric_limits<float>::lowest());
    for (const auto& v : vertices)
    {
        boundsMin = glm::min(boundsMin, v);
        boundsMax = glm::max(boundsMax, v);
    }
    
    // Write header
    RiverMeshHeader header{};
    std::memcpy(header.Magic, kRiverMeshMagic, sizeof(header.Magic));
    header.Version = kRiverMeshVersion;
    header.VertexCount = static_cast<uint32_t>(vertices.size());
    header.IndexCount = static_cast<uint32_t>(indices.size());
    header.BoundsMin[0] = boundsMin.x;
    header.BoundsMin[1] = boundsMin.y;
    header.BoundsMin[2] = boundsMin.z;
    header.BoundsMax[0] = boundsMax.x;
    header.BoundsMax[1] = boundsMax.y;
    header.BoundsMax[2] = boundsMax.z;
    
    stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
    
    // Write vertex data (positions, normals, uvs)
    stream.write(reinterpret_cast<const char*>(vertices.data()), 
                 vertices.size() * sizeof(glm::vec3));
    stream.write(reinterpret_cast<const char*>(normals.data()), 
                 normals.size() * sizeof(glm::vec3));
    stream.write(reinterpret_cast<const char*>(uvs.data()), 
                 uvs.size() * sizeof(glm::vec2));
    
    // Write indices
    stream.write(reinterpret_cast<const char*>(indices.data()), 
                 indices.size() * sizeof(uint32_t));
    
    if (!stream.good())
    {
        std::cerr << "[River] Failed while writing river mesh asset: " << diskPath.string() << std::endl;
        return false;
    }
    
    stream.close();
    river.MeshAssetDirty = false;
    
    std::cout << "[River] Saved river mesh asset: " << diskPath.string() 
              << " (" << vertices.size() << " verts, " << indices.size() << " indices)" << std::endl;
    
    return true;
}

bool River::LoadRiverMeshAsset(const std::string& assetPath, std::shared_ptr<Mesh>& outMesh)
{
    if (assetPath.empty())
        return false;
    
    // Try virtual filesystem first (for pak-mounted files), then disk
    std::vector<uint8_t> fileData;
    bool loaded = FileSystem::Instance().ReadFile(assetPath, fileData);
    
    // Fallback to direct disk access
    if (!loaded)
    {
        fs::path diskPath(assetPath);
        if (diskPath.is_relative())
        {
            const fs::path& projectRoot = FileSystem::Instance().GetProjectRoot();
            if (!projectRoot.empty())
            {
                diskPath = projectRoot / diskPath;
            }
        }
        
        std::ifstream stream(diskPath, std::ios::binary);
        if (!stream.is_open())
        {
            std::cerr << "[River] Failed to open river mesh asset for reading: " << diskPath.string() << std::endl;
            return false;
        }
        
        stream.seekg(0, std::ios::end);
        size_t size = static_cast<size_t>(stream.tellg());
        stream.seekg(0, std::ios::beg);
        fileData.resize(size);
        stream.read(reinterpret_cast<char*>(fileData.data()), size);
        
        if (!stream.good())
        {
            std::cerr << "[River] Failed to read river mesh asset: " << diskPath.string() << std::endl;
            return false;
        }
    }
    
    // Parse header
    size_t readPos = 0;
    auto readBytes = [&](void* dst, size_t count) -> bool {
        if (readPos + count > fileData.size()) return false;
        std::memcpy(dst, fileData.data() + readPos, count);
        readPos += count;
        return true;
    };
    
    RiverMeshHeader header{};
    if (!readBytes(&header, sizeof(header)))
        return false;
    
    // Validate header
    if (std::memcmp(header.Magic, kRiverMeshMagic, sizeof(header.Magic)) != 0)
    {
        std::cerr << "[River] Invalid river mesh asset magic" << std::endl;
        return false;
    }
    
    if (header.Version != kRiverMeshVersion)
    {
        std::cerr << "[River] Unsupported river mesh asset version: " << header.Version << std::endl;
        return false;
    }
    
    // Read vertex data
    std::vector<glm::vec3> vertices(header.VertexCount);
    std::vector<glm::vec3> normals(header.VertexCount);
    std::vector<glm::vec2> uvs(header.VertexCount);
    std::vector<uint32_t> indices(header.IndexCount);
    
    if (!readBytes(vertices.data(), vertices.size() * sizeof(glm::vec3)) ||
        !readBytes(normals.data(), normals.size() * sizeof(glm::vec3)) ||
        !readBytes(uvs.data(), uvs.size() * sizeof(glm::vec2)) ||
        !readBytes(indices.data(), indices.size() * sizeof(uint32_t)))
    {
        std::cerr << "[River] Failed to read river mesh data" << std::endl;
        return false;
    }
    
    // Create mesh
    auto mesh = std::make_shared<Mesh>();
    mesh->Vertices = std::move(vertices);
    mesh->Normals = std::move(normals);
    mesh->UVs = std::move(uvs);
    mesh->Indices = std::move(indices);
    mesh->numVertices = header.VertexCount;
    mesh->numIndices = header.IndexCount;
    mesh->BoundsMin = glm::vec3(header.BoundsMin[0], header.BoundsMin[1], header.BoundsMin[2]);
    mesh->BoundsMax = glm::vec3(header.BoundsMax[0], header.BoundsMax[1], header.BoundsMax[2]);
    
    // Create GPU buffers
    PBRVertex::Init();
    std::vector<PBRVertex> pbrVertices(mesh->numVertices);
    for (uint32_t i = 0; i < mesh->numVertices; ++i)
    {
        pbrVertices[i].x = mesh->Vertices[i].x;
        pbrVertices[i].y = mesh->Vertices[i].y;
        pbrVertices[i].z = mesh->Vertices[i].z;
        pbrVertices[i].nx = mesh->Normals[i].x;
        pbrVertices[i].ny = mesh->Normals[i].y;
        pbrVertices[i].nz = mesh->Normals[i].z;
        pbrVertices[i].u = mesh->UVs[i].x;
        pbrVertices[i].v = mesh->UVs[i].y;
    }
    
    const bgfx::Memory* vMem = bgfx::copy(pbrVertices.data(), 
        static_cast<uint32_t>(pbrVertices.size() * sizeof(PBRVertex)));
    mesh->vbh = bgfx::createVertexBuffer(vMem, PBRVertex::layout);
    
    const bgfx::Memory* iMem = bgfx::copy(mesh->Indices.data(), 
        static_cast<uint32_t>(mesh->Indices.size() * sizeof(uint32_t)));
    mesh->ibh = bgfx::createIndexBuffer(iMem, BGFX_BUFFER_INDEX32);
    
    outMesh = mesh;
    
    std::cout << "[River] Loaded river mesh asset: " << assetPath 
              << " (" << mesh->numVertices << " verts, " << mesh->numIndices << " indices)" << std::endl;
    
    return true;
}

std::shared_ptr<Mesh> River::GenerateMeshFromPath(
    const RiverComponent& river,
    float terrainMaxHeight,
    const std::function<float(float, float)>& sampleTerrainHeight)
{
    if (river.PathPoints.size() < 2)
        return nullptr;
    
    // Generate spline path from control points
    std::vector<glm::vec3> splinePositions;
    std::vector<glm::vec3> splineNormals;
    
    const auto& pathPoints = river.PathPoints;
    
    if (pathPoints.size() < 4)
    {
        // Linear interpolation for fewer points
        for (size_t i = 0; i < pathPoints.size() - 1; ++i)
        {
            for (int j = 0; j <= river.SplineSubdivision; ++j)
            {
                float t = static_cast<float>(j) / static_cast<float>(river.SplineSubdivision);
                splinePositions.push_back(glm::mix(pathPoints[i].Position, pathPoints[i + 1].Position, t));
                splineNormals.push_back(glm::normalize(glm::mix(pathPoints[i].Normal, pathPoints[i + 1].Normal, t)));
            }
        }
    }
    else
    {
        // Catmull-Rom spline
        for (size_t i = 0; i < pathPoints.size() - 1; ++i)
        {
            size_t i0 = (i == 0) ? 0 : i - 1;
            size_t i1 = i;
            size_t i2 = i + 1;
            size_t i3 = std::min(i + 2, pathPoints.size() - 1);
            
            for (int j = 0; j <= river.SplineSubdivision; ++j)
            {
                if (j == river.SplineSubdivision && i < pathPoints.size() - 2)
                    continue;
                
                float t = static_cast<float>(j) / static_cast<float>(river.SplineSubdivision);
                splinePositions.push_back(CatmullRom(
                    pathPoints[i0].Position, pathPoints[i1].Position,
                    pathPoints[i2].Position, pathPoints[i3].Position, t));
                splineNormals.push_back(glm::normalize(CatmullRom(
                    pathPoints[i0].Normal, pathPoints[i1].Normal,
                    pathPoints[i2].Normal, pathPoints[i3].Normal, t)));
            }
        }
    }
    
    if (splinePositions.size() < 2)
        return nullptr;
    
    // Build mesh data
    std::vector<glm::vec3> vertices;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> uvs;
    std::vector<uint32_t> indices;
    
    float totalLength = 0.0f;
    std::vector<float> segmentLengths;
    segmentLengths.push_back(0.0f);
    
    for (size_t i = 1; i < splinePositions.size(); ++i)
    {
        float segLen = glm::length(splinePositions[i] - splinePositions[i - 1]);
        totalLength += segLen;
        segmentLengths.push_back(totalLength);
    }
    
    float brushRadius = river.Width;
    
    for (size_t i = 0; i < splinePositions.size(); ++i)
    {
        const glm::vec3& pos = splinePositions[i];
        
        // Calculate tangent
        glm::vec3 tangent;
        if (i == 0)
            tangent = glm::normalize(splinePositions[1] - splinePositions[0]);
        else if (i == splinePositions.size() - 1)
            tangent = glm::normalize(splinePositions[i] - splinePositions[i - 1]);
        else
            tangent = glm::normalize(splinePositions[i + 1] - splinePositions[i - 1]);
        
        glm::vec3 up(0.0f, 1.0f, 0.0f);
        glm::vec3 perpendicular = glm::normalize(glm::cross(up, tangent));
        
        // Sample terrain height
        float terrainHeight = sampleTerrainHeight ? sampleTerrainHeight(pos.x, pos.z) : pos.y;
        float waterY = terrainHeight + river.WaterHeight;
        
        glm::vec3 leftPos = pos + perpendicular * brushRadius;
        glm::vec3 rightPos = pos - perpendicular * brushRadius;
        leftPos.y = waterY;
        rightPos.y = waterY;
        
        vertices.push_back(leftPos);
        vertices.push_back(rightPos);
        normals.push_back(up);
        normals.push_back(up);
        
        float v = (totalLength > 0.0f) ? segmentLengths[i] / totalLength : 0.0f;
        float vScaled = v * (totalLength / brushRadius);
        uvs.push_back(glm::vec2(0.0f, vScaled));
        uvs.push_back(glm::vec2(1.0f, vScaled));
    }
    
    // Generate indices
    for (size_t i = 0; i < splinePositions.size() - 1; ++i)
    {
        uint32_t bl = static_cast<uint32_t>(i * 2);
        uint32_t br = static_cast<uint32_t>(i * 2 + 1);
        uint32_t tl = static_cast<uint32_t>((i + 1) * 2);
        uint32_t tr = static_cast<uint32_t>((i + 1) * 2 + 1);
        
        indices.push_back(bl);
        indices.push_back(tl);
        indices.push_back(br);
        indices.push_back(br);
        indices.push_back(tl);
        indices.push_back(tr);
    }
    
    if (vertices.empty() || indices.empty())
        return nullptr;
    
    // Create mesh
    auto mesh = std::make_shared<Mesh>();
    mesh->Vertices = vertices;
    mesh->Normals = normals;
    mesh->UVs = uvs;
    mesh->Indices = indices;
    mesh->numVertices = static_cast<uint32_t>(vertices.size());
    mesh->numIndices = static_cast<uint32_t>(indices.size());
    mesh->ComputeBounds();
    
    // Create GPU buffers
    PBRVertex::Init();
    std::vector<PBRVertex> pbrVertices(mesh->numVertices);
    for (uint32_t i = 0; i < mesh->numVertices; ++i)
    {
        pbrVertices[i].x = vertices[i].x;
        pbrVertices[i].y = vertices[i].y;
        pbrVertices[i].z = vertices[i].z;
        pbrVertices[i].nx = normals[i].x;
        pbrVertices[i].ny = normals[i].y;
        pbrVertices[i].nz = normals[i].z;
        pbrVertices[i].u = uvs[i].x;
        pbrVertices[i].v = uvs[i].y;
    }
    
    const bgfx::Memory* vMem = bgfx::copy(pbrVertices.data(), 
        static_cast<uint32_t>(pbrVertices.size() * sizeof(PBRVertex)));
    mesh->vbh = bgfx::createVertexBuffer(vMem, PBRVertex::layout);
    
    const bgfx::Memory* iMem = bgfx::copy(mesh->Indices.data(), 
        static_cast<uint32_t>(mesh->Indices.size() * sizeof(uint32_t)));
    mesh->ibh = bgfx::createIndexBuffer(iMem, BGFX_BUFFER_INDEX32);
    
    return mesh;
}

void River::RestoreRiverMeshes(Scene& scene)
{
    // Find all entities with River components and restore their meshes
    for (auto& entity : scene.GetEntities())
    {
        EntityData* data = scene.GetEntityData(entity.GetID());
        if (!data || !data->River)
            continue;
        
        RiverComponent& river = *data->River;
        
        // Skip if no mesh was generated or no asset path
        if (!river.MeshGenerated || river.MeshAssetPath.empty())
            continue;
        
        // Find the mesh entity
        EntityID meshEntityId = river.MeshEntity;
        if (meshEntityId == INVALID_ENTITY_ID)
        {
            // Look for child entity with river mesh
            for (EntityID childId : data->Children)
            {
                EntityData* childData = scene.GetEntityData(childId);
                if (childData && childData->Name.find("_River") != std::string::npos)
                {
                    meshEntityId = childId;
                    river.MeshEntity = meshEntityId;
                    break;
                }
            }
        }
        
        if (meshEntityId == INVALID_ENTITY_ID)
        {
            std::cerr << "[River] No mesh entity found for river on entity: " << data->Name << std::endl;
            continue;
        }
        
        EntityData* meshData = scene.GetEntityData(meshEntityId);
        if (!meshData)
        {
            std::cerr << "[River] Mesh entity not found: " << meshEntityId << std::endl;
            continue;
        }
        
        // Load the mesh from asset
        std::shared_ptr<Mesh> loadedMesh;
        if (!LoadRiverMeshAsset(river.MeshAssetPath, loadedMesh) || !loadedMesh)
        {
            std::cerr << "[River] Failed to load river mesh asset: " << river.MeshAssetPath << std::endl;
            river.NeedsRegeneration = true;
            continue;
        }
        
        // Create or update mesh component
        if (!meshData->Mesh)
        {
            auto material = MaterialManager::Instance().CreateDefaultPBRMaterial();
            meshData->Mesh = std::make_unique<MeshComponent>(loadedMesh, "RiverMesh", material);
            meshData->Mesh->ShowBackfaces = true;
        }
        else
        {
            // Destroy old GPU resources if any
            if (meshData->Mesh->mesh)
            {
                if (bgfx::isValid(meshData->Mesh->mesh->vbh))
                    bgfx::destroy(meshData->Mesh->mesh->vbh);
                if (bgfx::isValid(meshData->Mesh->mesh->ibh))
                    bgfx::destroy(meshData->Mesh->mesh->ibh);
            }
            meshData->Mesh->mesh = loadedMesh;
        }
        
        river.NeedsRegeneration = false;
        
        std::cout << "[River] Restored river mesh for entity: " << data->Name << std::endl;
    }
}

