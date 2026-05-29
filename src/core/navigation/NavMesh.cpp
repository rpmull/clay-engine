#include "core/navigation/NavMesh.h"
#include "core/ecs/Scene.h"
#include "core/ecs/Components.h"
#include "core/rendering/Terrain.h"
#include "core/vfs/FileSystem.h"
#include "core/navigation/NavQueries.h"
#include "core/navigation/NavSerialization.h"
#include "core/navigation/NavJobs.h"
#include "core/rendering/Camera.h"
#include <cstring>
#include <algorithm>
#include <functional>
#include <vector>
#include <unordered_set>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cfloat>
#include <atomic>
#include <queue>
#include <limits>
#include <cmath>

#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>
#include <DetourNavMeshBuilder.h>
#include <DetourAlloc.h>

using namespace nav;

static uint64_t fnv1a64(const void* data, size_t len, uint64_t seed)
{
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ULL ^ seed;
    for (size_t i = 0; i < len; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

static std::string EnsureNavPackFileOnDisk(const std::string& filePath)
{
    namespace fs = std::filesystem;

    if (filePath.empty()) {
        return {};
    }

    fs::path diskPath(filePath);
    const fs::path& projectRoot = FileSystem::Instance().GetProjectRoot();
    if (diskPath.is_relative() && !projectRoot.empty()) {
        diskPath = projectRoot / diskPath;
    }
    diskPath = diskPath.lexically_normal();
    if (fs::exists(diskPath)) {
        return diskPath.string();
    }

    std::string vfsPath = filePath;
    std::replace(vfsPath.begin(), vfsPath.end(), '\\', '/');
    std::vector<uint8_t> data;
    if (!FileSystem::Instance().ReadFile(vfsPath, data)) {
        return diskPath.string();
    }

    fs::path cacheDir = fs::temp_directory_path() / "claymore_pak_cache" / "nav";
    std::error_code ec;
    fs::create_directories(cacheDir, ec);

    const size_t hashValue = std::hash<std::string>{}(vfsPath);
    fs::path extractedPath = cacheDir / ("navpack_" + std::to_string(hashValue) + ".navpack");
    if (!fs::exists(extractedPath)) {
        std::ofstream out(extractedPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return diskPath.string();
        }
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        out.close();
    }

    return extractedPath.string();
}
static uint64_t HashCombine(uint64_t h, uint64_t k) { h ^= k + 0x9e3779b97f4a7c15ULL + (h<<6) + (h>>2); return h; }

static uint32_t CountIslands(const NavMeshRuntime& rt)
{
    if (rt.m_Polys.empty() || rt.m_Adjacency.empty()) return 0;
    std::vector<int> component(rt.m_Polys.size(), -1);
    uint32_t numComponents = 0;
    for (uint32_t startPoly = 0; startPoly < rt.m_Polys.size(); ++startPoly) {
        if (component[startPoly] >= 0) continue;
        std::vector<uint32_t> queue;
        queue.push_back(startPoly);
        component[startPoly] = (int)numComponents;
        while (!queue.empty()) {
            uint32_t cur = queue.back();
            queue.pop_back();
            if (cur >= rt.m_Adjacency.size()) continue;
            for (uint32_t neighbor : rt.m_Adjacency[cur]) {
                if (neighbor >= rt.m_Polys.size()) continue;
                if (component[neighbor] < 0) {
                    component[neighbor] = (int)numComponents;
                    queue.push_back(neighbor);
                }
            }
        }
        numComponents++;
    }
    return numComponents;
}

static void UpdateBakeStatsFromRuntime(NavMeshComponent& comp)
{
    if (!comp.Runtime) return;
    float costMin = FLT_MAX;
    float costMax = 0.0f;
    for (const auto& p : comp.Runtime->m_Polys) {
        costMin = std::min(costMin, p.cost);
        costMax = std::max(costMax, p.cost);
    }
    if (comp.Runtime->m_Polys.empty()) { costMin = 1.0f; costMax = 1.0f; }
    comp.LastBakeCostMin.store(costMin);
    comp.LastBakeCostMax.store(costMax);
    comp.LastBakeIslands.store(CountIslands(*comp.Runtime));
    comp.LastBakeRemovedIslands.store(0);
}

void NavMeshComponent::GetEffectiveSources(Scene& scene, std::vector<EntityID>& out) const
{
    out.clear();
    out.reserve(scene.GetEntities().size());
    for (const auto& e : scene.GetEntities()) {
        const EntityID id = e.GetID();
        const auto* d = scene.GetEntityData(id);
        if (!d || !d->Mesh || !d->Mesh->mesh) continue;
        out.push_back(id);
    }
}

void NavMeshComponent::GetTerrainSources(Scene& scene, std::vector<EntityID>& out) const
{
    out.clear();
    out.reserve(scene.GetEntities().size());
    for (const auto& e : scene.GetEntities()) {
        const EntityID id = e.GetID();
        const auto* d = scene.GetEntityData(id);
        if (!d || !d->Terrain) continue;
        out.push_back(id);
    }
}

uint64_t NavMeshComponent::ComputeBakeHash(Scene& scene) const
{
    uint64_t h = 0xcbf29ce484222325ULL;
    // Include bake settings
    const uint64_t bakeHash = fnv1a64(&Bake, sizeof(Bake), 0x1234);
    h = HashCombine(h, bakeHash);
    
    // Include terrain-specific settings
    h = HashCombine(h, (uint64_t)TerrainSampleStep);
    h = HashCombine(h, (uint64_t)GeometryIncludeRegexEnabled);
    if (!GeometryIncludeRegexPattern.empty()) {
        h = HashCombine(h, fnv1a64(GeometryIncludeRegexPattern.data(), GeometryIncludeRegexPattern.size(), 0x6A6A));
    }
    h = HashCombine(h, (uint64_t)BakeVisibleChunksOnly);
    h = HashCombine(h, (uint64_t)BakeVisibleChunkPadding);
    h = HashCombine(h, (uint64_t)BakeMissingChunksOnly);

    // Stitching settings
    h = HashCombine(h, (uint64_t)EnableStitching);
    h = HashCombine(h, fnv1a64(&StitchEpsilon, sizeof(StitchEpsilon), 0x4747));
    h = HashCombine(h, fnv1a64(&StitchMaxNormalAngleDeg, sizeof(StitchMaxNormalAngleDeg), 0x4848));
    h = HashCombine(h, fnv1a64(&StitchMaxHeight, sizeof(StitchMaxHeight), 0x4949));
    h = HashCombine(h, fnv1a64(&StitchMaxXZ, sizeof(StitchMaxXZ), 0x4A4A));

    // Path smoothing settings
    h = HashCombine(h, (uint64_t)CostAwareSmoothing);

    // Use GetEffectiveSources which now includes siblings
    std::vector<EntityID> effectiveSources;
    GetEffectiveSources(scene, effectiveSources);

    // Include source meshes CPU vertex/index + world transform hash
    for (EntityID id : effectiveSources) {
        auto* d = scene.GetEntityData(id);
        if (!d || !d->Mesh || !d->Mesh->mesh) continue;
        const Mesh& m = *d->Mesh->mesh;
        uint64_t vhash = fnv1a64(m.Vertices.data(), m.Vertices.size() * sizeof(glm::vec3), 0x1111);
        uint64_t ihash = fnv1a64(m.Indices.data(), m.Indices.size() * sizeof(uint32_t), 0x2222);
        uint64_t thash = fnv1a64(&d->Transform.WorldMatrix, sizeof(glm::mat4), 0x3333);
        h = HashCombine(h, vhash);
        h = HashCombine(h, ihash);
        h = HashCombine(h, thash);
    }
    
    // Include terrain heightmap data in hash
    std::vector<EntityID> terrainSources;
    GetTerrainSources(scene, terrainSources);
    for (EntityID id : terrainSources) {
        auto* d = scene.GetEntityData(id);
        if (!d || !d->Terrain) continue;
        const TerrainComponent& t = *d->Terrain;
        if (!t.HeightMap.empty()) {
            h = HashCombine(h, fnv1a64(t.HeightMap.data(), t.HeightMap.size() * sizeof(uint16_t), 0x5555));
        }
        if (!t.SplatMap.empty()) {
            h = HashCombine(h, fnv1a64(t.SplatMap.data(), t.SplatMap.size() * sizeof(glm::u8vec4), 0x5A5A));
        }
        if (!t.SplatMap2.empty()) {
            h = HashCombine(h, fnv1a64(t.SplatMap2.data(), t.SplatMap2.size() * sizeof(glm::u8vec4), 0x5B5B));
        }
        if (!t.HoleMask.empty()) {
            h = HashCombine(h, fnv1a64(t.HoleMask.data(), t.HoleMask.size() * sizeof(uint8_t), 0x5D5D));
        }
        h = HashCombine(h, static_cast<uint64_t>(t.Layers.size()));
        for (const auto& layer : t.Layers) {
            h = HashCombine(h, fnv1a64(&layer.NavCost, sizeof(layer.NavCost), 0x5C5C));
        }
        h = HashCombine(h, fnv1a64(&t.GridResolution, sizeof(t.GridResolution), 0x6666));
        h = HashCombine(h, fnv1a64(&t.WorldSize, sizeof(t.WorldSize), 0x7777));
        h = HashCombine(h, fnv1a64(&t.MaxHeight, sizeof(t.MaxHeight), 0x8888));
        h = HashCombine(h, fnv1a64(&d->Transform.WorldMatrix, sizeof(glm::mat4), 0x9999));
    }
    
    return h;
}

void NavMeshComponent::RequestBake(Scene& scene)
{
    if (Baking.exchange(true)) return; // already baking
    BakingCancel.store(false);
    BakingProgress.store(0.0f);
    BakingStage.store(0);
    // Job dispatched via NavJobs (implemented in NavJobs.cpp)
    namespace jobs = nav::jobs;
    extern void SubmitBake(NavMeshComponent* comp, Scene* scene);
    jobs::SubmitBake(this, &scene);
}

void NavMeshComponent::CancelBake()
{
    BakingCancel.store(true);
    BakingStage.store(0);
}

std::string NavMeshComponent::BuildDefaultAssetPath(const ClaymoreGUID& guid)
{
    // Use filesystem path to ensure correct separators on all platforms
    std::filesystem::path p = ".bin";
    p /= "nav";
    p /= "navmesh_" + guid.ToString() + ".navbin";
    return p.string();
}

void NavMeshComponent::EnsureGuid()
{
    if (NavMeshDataGuid == ClaymoreGUID()) {
        NavMeshDataGuid = ClaymoreGUID::Generate();
        AssetPath = BuildDefaultAssetPath(NavMeshDataGuid);
    }
}

bool NavMeshComponent::EnsureRuntimeLoaded()
{
    if (Runtime) return true;
    if (ChunkedNavEnabled) {
        return false;
    }
    
    // Don't retry if we already failed
    if (LoadAttempted) return false;
    LoadAttempted = true;
    
    // Try loading from new asset path first
    if (!AssetPath.empty()) {
        namespace fs = std::filesystem;
        
        // Normalize path separators for VFS lookup
        std::string vfsPath = AssetPath;
        std::replace(vfsPath.begin(), vfsPath.end(), '\\', '/');
        
        // Try VFS first (supports PAK files in runtime)
        std::vector<uint8_t> data;
        if (FileSystem::Instance().ReadFile(vfsPath, data)) {
            uint64_t fileHash = 0;
            std::shared_ptr<NavMeshRuntime> loaded;
            if (nav::io::LoadNavMeshFromMemory(data.data(), data.size(), loaded, fileHash)) {
                std::atomic_store(&Runtime, loaded);
                Runtime->UseCostAwareSmoothing = CostAwareSmoothing;
                UpdateBakeStatsFromRuntime(*this);
                BakeHash = fileHash;
                std::cout << "[Nav] Loaded navmesh: " << Runtime->m_Polys.size() << " polys from VFS: " 
                          << vfsPath << std::endl;
                return true;
            } else {
                std::cerr << "[Nav] Failed to parse navmesh from VFS: " << vfsPath << std::endl;
            }
        } else {
            // Fallback to disk for editor mode
            fs::path diskPath(AssetPath);
            const fs::path& projectRoot = FileSystem::Instance().GetProjectRoot();
            
            if (diskPath.is_relative() && !projectRoot.empty()) {
                diskPath = projectRoot / diskPath;
            }
            
            // Normalize path to ensure consistent separators
            diskPath = diskPath.lexically_normal();
            
            if (fs::exists(diskPath)) {
                uint64_t fileHash = 0;
                std::shared_ptr<NavMeshRuntime> loaded;
                if (nav::io::LoadNavMeshFromFile(diskPath.string(), loaded, fileHash)) {
                    std::atomic_store(&Runtime, loaded);
                    Runtime->UseCostAwareSmoothing = CostAwareSmoothing;
                    UpdateBakeStatsFromRuntime(*this);
                    BakeHash = fileHash;
                    std::cout << "[Nav] Loaded navmesh: " << Runtime->m_Polys.size() << " polys from " 
                              << diskPath.string() << std::endl;
                    return true;
                } else {
                    std::cerr << "[Nav] Failed to load navmesh from " << diskPath.string() << std::endl;
                }
            } else {
                std::cerr << "[Nav] Navmesh file not found: " << vfsPath << " (VFS) or " << diskPath.string() << " (disk)" << std::endl;
            }
        }
    }
    
    return false;
}

bool NavMeshComponent::EnsureRuntimeLoaded(Scene& scene)
{
    if (ChunkedNavEnabled) {
        std::shared_ptr<NavChunkManager> manager = ChunkManager;
        if (!manager) {
            manager = std::make_shared<NavChunkManager>();
            ChunkManager = manager;
        }
        std::string path = NavPackPath.empty() ? AssetPath : NavPackPath;
        if (path.empty()) return false;
        // Resolve relative paths against project root (LoadPack uses std::ifstream which needs absolute path)
        {
            namespace fs = std::filesystem;
            fs::path diskPath(path);
            const fs::path& projectRoot = FileSystem::Instance().GetProjectRoot();
            if (diskPath.is_relative() && !projectRoot.empty()) {
                diskPath = projectRoot / diskPath;
            }
            diskPath = diskPath.lexically_normal();
            // For chunked nav, LoadPack expects .navpack; if path was .navbin, use .navpack equivalent
            if (diskPath.extension() == ".navbin") {
                diskPath.replace_extension(".navpack");
            }
            path = diskPath.string();
        }
        path = EnsureNavPackFileOnDisk(path);
        if (!manager->LoadPack(path)) return false;
        manager->Update(scene, *this);
        Runtime = manager->GetStitchedRuntime();
        return Runtime != nullptr;
    }
    if (Runtime) return true;
    return EnsureRuntimeLoaded();
}

NavMeshRuntime::NavMeshRuntime()
{
    m_AreaCost.fill(1.0f);
    UseCostAwareSmoothing = true;
}

NavMeshRuntime::~NavMeshRuntime()
{
    ClearDetour();
}

bool NavMeshRuntime::HasDetour() const
{
    return m_DetourMesh != nullptr && m_DetourQuery != nullptr;
}

void NavMeshRuntime::ClearDetour()
{
    if (m_DetourQuery) {
        dtFreeNavMeshQuery(m_DetourQuery);
        m_DetourQuery = nullptr;
    }
    if (m_DetourMesh) {
        dtFreeNavMesh(m_DetourMesh);
        m_DetourMesh = nullptr;
    }
    m_DetourNavData.clear();
}

bool NavMeshRuntime::SetDetourNavData(const uint8_t* data, size_t size)
{
    if (!data || size == 0 || size > (size_t)std::numeric_limits<int>::max()) {
        return false;
    }

    {
        std::unique_lock lk(m_Lock);
        ClearDetour();

        unsigned char* navData = (unsigned char*)dtAlloc((int)size, DT_ALLOC_PERM);
        if (!navData) {
            return false;
        }
        std::memcpy(navData, data, size);

        m_DetourMesh = dtAllocNavMesh();
        if (!m_DetourMesh) {
            dtFree(navData);
            return false;
        }

        dtStatus meshStatus = m_DetourMesh->init(navData, (int)size, DT_TILE_FREE_DATA);
        if (dtStatusFailed(meshStatus)) {
            dtFreeNavMesh(m_DetourMesh);
            m_DetourMesh = nullptr;
            return false;
        }

        m_DetourQuery = dtAllocNavMeshQuery();
        if (!m_DetourQuery) {
            dtFreeNavMesh(m_DetourMesh);
            m_DetourMesh = nullptr;
            return false;
        }

        dtStatus queryStatus = m_DetourQuery->init(m_DetourMesh, 2048);
        if (dtStatusFailed(queryStatus)) {
            dtFreeNavMeshQuery(m_DetourQuery);
            m_DetourQuery = nullptr;
            dtFreeNavMesh(m_DetourMesh);
            m_DetourMesh = nullptr;
            return false;
        }

        m_DetourNavData.assign(data, data + size);
    }

    return SyncLegacyGeometryFromDetour();
}

bool NavMeshRuntime::SetDetourNavData(std::vector<uint8_t> data)
{
    if (!SetDetourNavData(data.data(), data.size())) {
        return false;
    }
    std::unique_lock lk(m_Lock);
    m_DetourNavData = std::move(data);
    return true;
}

bool NavMeshRuntime::InitDetourTileWorld(const glm::vec3& origin, float tileWidth, float tileHeight, int maxTiles, int maxPolysPerTile)
{
    if (tileWidth <= 0.0f || tileHeight <= 0.0f || maxTiles <= 0 || maxPolysPerTile <= 0) {
        return false;
    }

    std::unique_lock lk(m_Lock);
    ClearDetour();

    m_DetourMesh = dtAllocNavMesh();
    if (!m_DetourMesh) return false;

    dtNavMeshParams params{};
    params.orig[0] = origin.x;
    params.orig[1] = origin.y;
    params.orig[2] = origin.z;
    params.tileWidth = tileWidth;
    params.tileHeight = tileHeight;
    params.maxTiles = maxTiles;
    params.maxPolys = maxPolysPerTile;

    dtStatus meshStatus = m_DetourMesh->init(&params);
    if (dtStatusFailed(meshStatus)) {
        std::cerr << "[Nav] Detour tile world init failed: status=0x"
                  << std::hex << (uint32_t)meshStatus << std::dec
                  << " origin=(" << origin.x << "," << origin.y << "," << origin.z << ")"
                  << " tile=(" << tileWidth << "," << tileHeight << ")"
                  << " maxTiles=" << maxTiles
                  << " maxPolysPerTile=" << maxPolysPerTile
                  << std::endl;
        dtFreeNavMesh(m_DetourMesh);
        m_DetourMesh = nullptr;
        return false;
    }

    m_DetourQuery = dtAllocNavMeshQuery();
    if (!m_DetourQuery) {
        dtFreeNavMesh(m_DetourMesh);
        m_DetourMesh = nullptr;
        return false;
    }

    dtStatus queryStatus = m_DetourQuery->init(m_DetourMesh, 2048);
    if (dtStatusFailed(queryStatus)) {
        std::cerr << "[Nav] Detour nav query init failed: status=0x"
                  << std::hex << (uint32_t)queryStatus << std::dec
                  << " maxNodes=2048"
                  << std::endl;
        dtFreeNavMeshQuery(m_DetourQuery);
        m_DetourQuery = nullptr;
        dtFreeNavMesh(m_DetourMesh);
        m_DetourMesh = nullptr;
        return false;
    }

    m_DetourNavData.clear();
    m_Bounds.min = glm::vec3(0.0f);
    m_Bounds.max = glm::vec3(0.0f);
    return true;
}

bool NavMeshRuntime::AddDetourTileCopy(const uint8_t* data, size_t size, uint64_t& outTileRef,
                                       int forcedTileX, int forcedTileY)
{
    outTileRef = 0;
    if (!data || size == 0 || size > (size_t)std::numeric_limits<int>::max()) return false;

    std::unique_lock lk(m_Lock);
    if (!m_DetourMesh || !m_DetourQuery) return false;

    unsigned char* tileData = (unsigned char*)dtAlloc((int)size, DT_ALLOC_PERM);
    if (!tileData) return false;
    std::memcpy(tileData, data, size);
    const int noOverride = std::numeric_limits<int>::min();
    if (forcedTileX != noOverride && forcedTileY != noOverride && size >= sizeof(dtMeshHeader)) {
        dtMeshHeader* hdr = reinterpret_cast<dtMeshHeader*>(tileData);
        if (hdr->magic == DT_NAVMESH_MAGIC && hdr->version == DT_NAVMESH_VERSION) {
            hdr->x = forcedTileX;
            hdr->y = forcedTileY;
            hdr->layer = 0;
            hdr->userId = (uint32_t)(((uint32_t)forcedTileY << 16) ^ ((uint32_t)forcedTileX & 0xFFFFu));
        }
    }

    dtTileRef tileRef = 0;
    dtStatus addStatus = m_DetourMesh->addTile(tileData, (int)size, DT_TILE_FREE_DATA, 0, &tileRef);
    if (dtStatusFailed(addStatus) || tileRef == 0) {
        dtFree(tileData);
        return false;
    }

    outTileRef = (uint64_t)tileRef;
    return true;
}

bool NavMeshRuntime::RemoveDetourTile(uint64_t tileRef)
{
    if (tileRef == 0) return true;
    std::unique_lock lk(m_Lock);
    if (!m_DetourMesh) return false;

    unsigned char* tileData = nullptr;
    int tileDataSize = 0;
    dtStatus removeStatus = m_DetourMesh->removeTile((dtTileRef)tileRef, &tileData, &tileDataSize);
    if (dtStatusFailed(removeStatus)) {
        return false;
    }
    if (tileData) {
        dtFree(tileData);
    }
    return true;
}

void NavMeshRuntime::GetDetourDebugStats(uint32_t& outTiles, uint32_t& outPolys) const
{
    outTiles = 0;
    outPolys = 0;
    std::shared_lock lk(m_Lock);
    if (!m_DetourMesh) return;
    const dtNavMesh* detourMesh = static_cast<const dtNavMesh*>(m_DetourMesh);
    const int maxTiles = detourMesh->getMaxTiles();
    for (int i = 0; i < maxTiles; ++i) {
        const dtMeshTile* tile = detourMesh->getTile(i);
        if (!tile || !tile->header) continue;
        ++outTiles;
        outPolys += (uint32_t)tile->header->polyCount;
    }
}

bool NavMeshRuntime::SyncLegacyGeometryFromDetour()
{
    if (!m_DetourMesh) return false;

    m_Vertices.clear();
    m_Polys.clear();
    m_Adjacency.clear();
    m_BVH.clear();
    m_BVHIndices.clear();
    m_Bounds.min = glm::vec3(FLT_MAX);
    m_Bounds.max = glm::vec3(-FLT_MAX);

    const dtNavMesh* detourMesh = static_cast<const dtNavMesh*>(m_DetourMesh);
    const int maxTiles = detourMesh->getMaxTiles();
    for (int ti = 0; ti < maxTiles; ++ti) {
        const dtMeshTile* tile = detourMesh->getTile(ti);
        if (!tile || !tile->header || !tile->polys || !tile->verts) continue;

        const int vertBase = (int)m_Vertices.size();
        const int vertCount = tile->header->vertCount;
        for (int i = 0; i < vertCount; ++i) {
            const float* v = &tile->verts[i * 3];
            glm::vec3 out(v[0], v[1], v[2]);
            m_Vertices.push_back(out);
            m_Bounds.expand(out);
        }

        const int polyCount = tile->header->polyCount;
        for (int pi = 0; pi < polyCount; ++pi) {
            const dtPoly& poly = tile->polys[pi];
            if (poly.getType() != DT_POLYTYPE_GROUND || poly.vertCount < 3) {
                continue;
            }

            for (uint8_t k = 1; k + 1 < poly.vertCount; ++k) {
                NavMeshRuntime::Poly out{};
                out.i0 = (uint32_t)(vertBase + poly.verts[0]);
                out.i1 = (uint32_t)(vertBase + poly.verts[k]);
                out.i2 = (uint32_t)(vertBase + poly.verts[k + 1]);
                out.area = (uint16_t)poly.getArea();
                out.flags = (uint32_t)poly.flags;
                float areaCost = (out.area < m_AreaCost.size()) ? m_AreaCost[out.area] : 1.0f;
                out.cost = std::max(0.01f, areaCost);
                m_Polys.push_back(out);
            }
        }
    }

    if (m_Vertices.empty()) {
        m_Bounds.min = glm::vec3(0.0f);
        m_Bounds.max = glm::vec3(0.0f);
    }

    // Build compatibility structures used by existing debug draw/chunk stitch paths.
    RebuildBVH();
    return true;
}

bool NavMeshRuntime::FindPath(const glm::vec3& start, const glm::vec3& end, NavPath& out, const NavAgentParams& params, NavFlags include, NavFlags exclude) const
{
    std::shared_lock lk(m_Lock);
    return queries::FindPath(*this, start, end, params, include, exclude, out);
}

bool NavMeshRuntime::Raycast(const glm::vec3& start, const glm::vec3& end, float& tHit, glm::vec3& hitNormal) const
{
    std::shared_lock lk(m_Lock);
    return queries::RaycastPolyMesh(*this, start, end, tHit, hitNormal);
}

bool NavMeshRuntime::NearestPoint(const glm::vec3& pos, float maxDist, glm::vec3& outOnMesh) const
{
    std::shared_lock lk(m_Lock);
    return queries::NearestPointOnNavmesh(*this, pos, maxDist, outOnMesh);
}

static uint64_t ChunkKey64(int32_t x, int32_t z)
{
    return (uint64_t)(uint32_t)x << 32 | (uint32_t)z;
}

// Distance from point to nearest edge of AABB in XZ. Returns 0 if outside or invalid.
static float DistanceToNearestEdgeXZ(const glm::vec3& p, const Bounds& b)
{
    if (!std::isfinite(p.x) || !std::isfinite(p.z) ||
        !std::isfinite(b.min.x) || !std::isfinite(b.max.x) ||
        !std::isfinite(b.min.z) || !std::isfinite(b.max.z)) {
        return 0.0f;
    }
    const float toWest = p.x - b.min.x;
    const float toEast = b.max.x - p.x;
    const float toSouth = p.z - b.min.z;
    const float toNorth = b.max.z - p.z;
    const float minX = std::min(toWest, toEast);
    const float minZ = std::min(toSouth, toNorth);
    if (minX < 0.0f || minZ < 0.0f) return 0.0f; // outside bounds
    return std::min(minX, minZ);
}

static float DistanceToAabbXZ(const glm::vec3& p, const Bounds& b)
{
    if (!std::isfinite(p.x) || !std::isfinite(p.z)) {
        return std::numeric_limits<float>::infinity();
    }
    if (!std::isfinite(b.min.x) || !std::isfinite(b.max.x) ||
        !std::isfinite(b.min.z) || !std::isfinite(b.max.z)) {
        return std::numeric_limits<float>::infinity();
    }

    const float minX = std::min(b.min.x, b.max.x);
    const float maxX = std::max(b.min.x, b.max.x);
    const float minZ = std::min(b.min.z, b.max.z);
    const float maxZ = std::max(b.min.z, b.max.z);

    const float cx = std::clamp(p.x, minX, maxX);
    const float cz = std::clamp(p.z, minZ, maxZ);
    const float dx = p.x - cx;
    const float dz = p.z - cz;
    return std::sqrt(dx * dx + dz * dz);
}

static float DistanceToAabbXZSq(const glm::vec3& p, const Bounds& b)
{
    if (!std::isfinite(p.x) || !std::isfinite(p.z)) {
        return std::numeric_limits<float>::infinity();
    }
    if (!std::isfinite(b.min.x) || !std::isfinite(b.max.x) ||
        !std::isfinite(b.min.z) || !std::isfinite(b.max.z)) {
        return std::numeric_limits<float>::infinity();
    }

    const float minX = std::min(b.min.x, b.max.x);
    const float maxX = std::max(b.min.x, b.max.x);
    const float minZ = std::min(b.min.z, b.max.z);
    const float maxZ = std::max(b.min.z, b.max.z);

    const float cx = std::clamp(p.x, minX, maxX);
    const float cz = std::clamp(p.z, minZ, maxZ);
    const float dx = p.x - cx;
    const float dz = p.z - cz;
    return dx * dx + dz * dz;
}

static glm::vec3 ResolveStreamingCameraPos(Scene& scene)
{
    auto viewportCameraPos = [&scene]() -> glm::vec3 {
        if (!scene.HasEditorViewportState()) return glm::vec3(0.0f);
        const Scene::EditorViewportState& s = scene.GetEditorViewportState();
        const float yaw = glm::radians(s.Yaw);
        const float pitch = glm::radians(s.Pitch);
        const float cosPitch = std::cos(pitch);
        glm::vec3 forward(
            cosPitch * std::sin(yaw),
            std::sin(pitch),
            cosPitch * std::cos(yaw)
        );
        if (glm::length2(forward) < 1e-8f) {
            forward = glm::vec3(0.0f, 0.0f, -1.0f);
        } else {
            forward = glm::normalize(forward);
        }
        return s.Target - forward * std::max(0.01f, s.Distance);
    };

    if (scene.m_IsPlaying) {
        // Play mode: follow active in-scene camera.
        if (Camera* cam = scene.GetActiveCamera()) return cam->GetPosition();
        // Fallback if no runtime camera exists.
        if (scene.HasEditorViewportState()) return viewportCameraPos();
        return glm::vec3(0.0f);
    }

    // Editor mode: follow editor viewport camera.
    if (scene.HasEditorViewportState()) return viewportCameraPos();
    if (Camera* cam = scene.GetActiveCamera()) return cam->GetPosition();
    return glm::vec3(0.0f);
}

NavChunkManager::~NavChunkManager()
{
    std::lock_guard lk(m_StateMutex);
    StopLoadWorker();
}

void NavChunkManager::StartLoadWorker()
{
    if (m_LoadWorkerRunning) return;
    m_LoadWorkerStop = false;
    m_LoadWorkerRunning = true;
    m_LoadWorker = std::thread([this]() {
        for (;;) {
            PendingChunkLoad req;
            {
                std::unique_lock lk(m_LoadMutex);
                m_LoadCv.wait(lk, [this]() { return m_LoadWorkerStop || !m_LoadQueue.empty(); });
                if (m_LoadWorkerStop && m_LoadQueue.empty()) break;
                req = std::move(m_LoadQueue.front());
                m_LoadQueue.pop_front();
            }

            CompletedChunkLoad result;
            result.key = req.key;
            result.success = false;
            std::shared_ptr<NavMeshRuntime> loaded;
            uint64_t fileHash = 0;
            nav::io::NavPackChunk entry;
            entry.gridX = req.gridX;
            entry.gridZ = req.gridZ;
            entry.bounds = req.bounds;
            entry.hash = req.hash;
            entry.offset = req.offset;
            entry.size = req.size;
            if (nav::io::ReadNavPackChunk(req.path, entry, loaded, fileHash)) {
                result.success = true;
                result.fileHash = fileHash;
                result.runtime = std::move(loaded);
            }

            {
                std::lock_guard lk(m_LoadMutex);
                m_LoadResults.push_back(std::move(result));
            }
        }
    });
}

void NavChunkManager::StopLoadWorker()
{
    {
        std::lock_guard lk(m_LoadMutex);
        m_LoadWorkerStop = true;
        m_LoadQueue.clear();
        m_LoadResults.clear();
        m_LoadInFlight.clear();
    }
    m_LoadCv.notify_all();
    if (m_LoadWorker.joinable()) {
        m_LoadWorker.join();
    }
    {
        std::lock_guard lk(m_LoadMutex);
        m_LoadQueue.clear();
        m_LoadResults.clear();
        m_LoadInFlight.clear();
    }
    m_LoadWorkerRunning = false;
    m_LoadWorkerStop = false;
}

bool NavChunkManager::QueueChunkLoad(uint64_t key, const NavChunkRecord& rec)
{
    if (!m_LoadWorkerRunning) return false;
    std::lock_guard lk(m_LoadMutex);
    if (m_LoadInFlight.find(key) != m_LoadInFlight.end()) return false;
    PendingChunkLoad req;
    req.key = key;
    req.path = m_Path;
    req.gridX = rec.key.x;
    req.gridZ = rec.key.z;
    req.bounds = rec.bounds;
    req.hash = rec.hash;
    req.offset = rec.offset;
    req.size = rec.size;
    m_LoadInFlight.insert(key);
    m_LoadQueue.push_back(std::move(req));
    m_LoadCv.notify_one();
    return true;
}

bool NavChunkManager::IsChunkLoadInFlight(uint64_t key)
{
    std::lock_guard lk(m_LoadMutex);
    return m_LoadInFlight.find(key) != m_LoadInFlight.end();
}

bool NavChunkManager::TryPopChunkLoadResult(CompletedChunkLoad& out)
{
    std::lock_guard lk(m_LoadMutex);
    if (m_LoadResults.empty()) return false;
    out = std::move(m_LoadResults.front());
    m_LoadResults.pop_front();
    m_LoadInFlight.erase(out.key);
    return true;
}

std::shared_ptr<NavMeshRuntime> NavChunkManager::GetStitchedRuntime() const
{
    std::lock_guard lk(m_StateMutex);
    return m_Stitched;
}

bool NavChunkManager::LoadPack(std::string path)
{
    std::lock_guard lk(m_StateMutex);
    const std::string& packPath = path;
    if (!m_Path.empty() && m_Path == packPath && !m_Chunks.empty()) {
        // Throttle filesystem check: only re-check every 60 frames (~1s)
        static thread_local uint32_t s_fsCheckCounter = 0;
        if ((++s_fsCheckCounter % 60u) != 0) {
            return true;
        }
        uint64_t writeTimeTicks = 0;
        {
            std::error_code ec;
            const auto writeTime = std::filesystem::last_write_time(std::filesystem::path(packPath), ec);
            if (!ec) {
                writeTimeTicks = (uint64_t)writeTime.time_since_epoch().count();
            }
        }
        if (writeTimeTicks == 0 || writeTimeTicks == m_LastPackWriteTimeTicks) {
            return true;
        }
        std::cout << "[Nav] Detected updated navpack on disk. Reloading chunk index." << std::endl;
    }
    uint64_t writeTimeTicks = 0;
    {
        std::error_code ec;
        const auto writeTime = std::filesystem::last_write_time(std::filesystem::path(packPath), ec);
        if (!ec) {
            writeTimeTicks = (uint64_t)writeTime.time_since_epoch().count();
        }
    }
    // Full load path (first load or pack changed)
    StopLoadWorker();
    nav::io::NavPackMeta meta;
    std::vector<nav::io::NavPackChunk> entries;
    if (!nav::io::LoadNavPackIndex(packPath, meta, entries)) {
        return false;
    }
    m_Path = packPath;
    m_LastPackWriteTimeTicks = writeTimeTicks;
    m_ChunksX = meta.chunksX;
    m_ChunksZ = meta.chunksZ;
    m_BakeHash = meta.bakeHash;
    m_DetourWorldReady = false;
    m_DetourWorldOrigin = glm::vec3(0.0f);
    m_DetourTileWidth = 0.0f;
    m_DetourTileHeight = 0.0f;
    m_Stitched.reset();
    m_DetourWorldInitFailed = false;
    m_StreamRevision = 0;
    m_Chunks.clear();
    m_LoadedChunkCount = 0;
    m_LoadedBoundsValid = false;
    m_LoadedBoundsDirty = false;
    m_LoadedBounds.min = glm::vec3(0.0f);
    m_LoadedBounds.max = glm::vec3(0.0f);
    m_Chunks.reserve(entries.size());
    size_t invalidBoundsCount = 0;
    for (const auto& e : entries) {
        NavChunkRecord rec;
        rec.key = { e.gridX, e.gridZ };
        rec.bounds = e.bounds;
        if (rec.bounds.min.x > rec.bounds.max.x) std::swap(rec.bounds.min.x, rec.bounds.max.x);
        if (rec.bounds.min.y > rec.bounds.max.y) std::swap(rec.bounds.min.y, rec.bounds.max.y);
        if (rec.bounds.min.z > rec.bounds.max.z) std::swap(rec.bounds.min.z, rec.bounds.max.z);
        const bool finiteBounds =
            std::isfinite(rec.bounds.min.x) && std::isfinite(rec.bounds.min.y) && std::isfinite(rec.bounds.min.z) &&
            std::isfinite(rec.bounds.max.x) && std::isfinite(rec.bounds.max.y) && std::isfinite(rec.bounds.max.z);
        rec.hash = e.hash;
        rec.offset = e.offset;
        rec.size = e.size;
        rec.loaded = false;
        rec.missingPayload = (e.size == 0) || !finiteBounds;
        rec.loadFailStreak = rec.missingPayload ? 255u : 0u;
        if (!finiteBounds) {
            ++invalidBoundsCount;
        }
        m_Chunks[ChunkKey64(e.gridX, e.gridZ)] = rec;
    }
    if (invalidBoundsCount > 0) {
        std::cerr << "[Nav] Navpack index contains " << invalidBoundsCount
                  << " chunk(s) with invalid bounds metadata; those chunks are skipped. "
                  << "Please Rebuild/Rebake navigation."
                  << std::endl;
    }
    StartLoadWorker();
    m_StitchDirty = true;
    return true;
}

void NavChunkManager::GetLoadedChunkRuntimes(std::vector<LoadedChunkRuntime>& out) const
{
    std::lock_guard lk(m_StateMutex);
    out.clear();
    out.reserve(m_Chunks.size());
    for (const auto& kv : m_Chunks) {
        const NavChunkRecord& rec = kv.second;
        if (!rec.loaded || !rec.runtime) continue;
        LoadedChunkRuntime e;
        e.key = rec.key;
        e.bounds = rec.bounds;
        e.runtime = rec.runtime;
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const LoadedChunkRuntime& a, const LoadedChunkRuntime& b) {
        if (a.key.z != b.key.z) return a.key.z < b.key.z;
        return a.key.x < b.key.x;
    });
}

bool NavChunkManager::EnsureDetourTileWorld(const NavMeshComponent& comp)
{
    if (m_DetourWorldInitFailed) {
        return false;
    }
    if (m_DetourWorldReady && m_Stitched && m_Stitched->HasDetour()) {
        return true;
    }
    if (m_Chunks.empty()) return false;

    Bounds worldBounds;
    worldBounds.min = glm::vec3(FLT_MAX);
    worldBounds.max = glm::vec3(-FLT_MAX);
    bool hasValidChunkBounds = false;
    for (const auto& kv : m_Chunks) {
        const Bounds& b = kv.second.bounds;
        if (!std::isfinite(b.min.x) || !std::isfinite(b.min.y) || !std::isfinite(b.min.z) ||
            !std::isfinite(b.max.x) || !std::isfinite(b.max.y) || !std::isfinite(b.max.z)) {
            continue;
        }
        glm::vec3 bmin(
            std::min(b.min.x, b.max.x),
            std::min(b.min.y, b.max.y),
            std::min(b.min.z, b.max.z)
        );
        glm::vec3 bmax(
            std::max(b.min.x, b.max.x),
            std::max(b.min.y, b.max.y),
            std::max(b.min.z, b.max.z)
        );
        worldBounds.expand(bmin);
        worldBounds.expand(bmax);
        hasValidChunkBounds = true;
    }
    if (!hasValidChunkBounds || worldBounds.min.x > worldBounds.max.x) {
        m_DetourWorldInitFailed = true;
        std::cerr << "[Nav] Chunked navpack has invalid bounds metadata. "
                  << "Skipping navpack load; please Rebuild/Rebake navigation."
                  << std::endl;
        return false;
    }

    float tileWidth = comp.ChunkWorldSize > 0.0f ? comp.ChunkWorldSize : 0.0f;
    float tileHeight = comp.ChunkWorldSize > 0.0f ? comp.ChunkWorldSize : 0.0f;
    if (tileWidth <= 0.0f || tileHeight <= 0.0f || !std::isfinite(tileWidth) || !std::isfinite(tileHeight)) {
        tileWidth = 0.0f;
        tileHeight = 0.0f;
        for (const auto& kv : m_Chunks) {
            const Bounds& b = kv.second.bounds;
            if (!std::isfinite(b.min.x) || !std::isfinite(b.min.z) ||
                !std::isfinite(b.max.x) || !std::isfinite(b.max.z)) {
                continue;
            }
            const float bw = std::max(b.min.x, b.max.x) - std::min(b.min.x, b.max.x);
            const float bh = std::max(b.min.z, b.max.z) - std::min(b.min.z, b.max.z);
            if (bw > 0.0f && bh > 0.0f) {
                tileWidth = bw;
                tileHeight = bh;
                break;
            }
        }
        if (tileWidth <= 0.0f || tileHeight <= 0.0f || !std::isfinite(tileWidth) || !std::isfinite(tileHeight)) {
            m_DetourWorldInitFailed = true;
            std::cerr << "[Nav] Chunked navpack has invalid tile dimensions. "
                      << "Skipping navpack load; please Rebuild/Rebake navigation."
                      << std::endl;
            return false;
        }
        tileWidth = std::max(1.0f, tileWidth);
        tileHeight = std::max(1.0f, tileHeight);
    }

    auto rt = std::make_shared<NavMeshRuntime>();
    if (comp.Runtime) {
        rt->m_AreaCost = comp.Runtime->m_AreaCost;
    } else {
        rt->m_AreaCost.fill(1.0f);
    }

    int estimatedVisibleTiles = 0;
    if (comp.ChunkStreamRadius > 0.0f && tileWidth > 0.0f && tileHeight > 0.0f) {
        const float streamArea = 3.1415926535f * comp.ChunkStreamRadius * comp.ChunkStreamRadius;
        const float tileArea = std::max(1.0f, tileWidth * tileHeight);
        estimatedVisibleTiles = (int)std::ceil(streamArea / tileArea);
    }
    int maxTiles = std::max(64, estimatedVisibleTiles * 4 + 64);
    maxTiles = std::min(maxTiles, 1 << 16); // Keep tile bits bounded for 32-bit poly refs.

    auto nextPow2 = [](unsigned int v) -> unsigned int {
        if (v <= 1u) return 1u;
        --v;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        return v + 1;
    };
    auto ilog2 = [](unsigned int v) -> int {
        int r = 0;
        while (v > 1u) { v >>= 1; ++r; }
        return r;
    };
    const int tileBits = ilog2(nextPow2((unsigned int)std::max(1, maxTiles)));
    // Detour requires at least 10 salt bits for 32-bit refs, so tileBits+polyBits must be <= 22.
    const int maxRefBitsForTileAndPoly = 22;
    const int desiredPolyBits = 14;
    const int polyBitsBudget = maxRefBitsForTileAndPoly - tileBits;
    if (polyBitsBudget <= 0) {
        m_DetourWorldInitFailed = true;
        std::cerr << "[Nav] Chunked navpack incompatible with Detour salt-bit limits (tileBits="
                  << tileBits << ", requires tileBits+polyBits<=22). "
                  << "Skipping navpack load; please reduce streaming tile budget or rebake with coarser chunks."
                  << std::endl;
        return false;
    }
    const int polyBits = std::min(desiredPolyBits, polyBitsBudget);
    const int maxPolysPerTile = 1 << polyBits;
    if (tileBits + polyBits > maxRefBitsForTileAndPoly) {
        m_DetourWorldInitFailed = true;
        std::cerr << "[Nav] Chunked navpack incompatible with Detour limits (tileBits="
                  << tileBits << ", polyBits=" << polyBits
                  << ", requires <=22). Skipping navpack load; please Rebuild/Rebake navigation."
                  << std::endl;
        return false;
    }

    if (!rt->InitDetourTileWorld(worldBounds.min, tileWidth, tileHeight, maxTiles, maxPolysPerTile)) {
        m_DetourWorldInitFailed = true;
        std::cerr << "[Nav] Failed to initialize chunked Detour tile world. "
                  << "Skipping navpack load; please Rebuild/Rebake navigation."
                  << std::endl;
        return false;
    }

    m_Stitched = rt;
    m_DetourWorldOrigin = worldBounds.min;
    m_DetourTileWidth = tileWidth;
    m_DetourTileHeight = tileHeight;
    m_DetourWorldReady = true;
    return true;
}

void NavChunkManager::UpdateDetourWorldBounds()
{
    if (!m_Stitched) return;

    if (m_LoadedBoundsDirty) {
        Bounds recomputed;
        recomputed.min = glm::vec3(FLT_MAX);
        recomputed.max = glm::vec3(-FLT_MAX);
        size_t loadedCount = 0;
        for (const auto& kv : m_Chunks) {
            const NavChunkRecord& rec = kv.second;
            if (!rec.loaded) continue;
            recomputed.expand(rec.bounds.min);
            recomputed.expand(rec.bounds.max);
            ++loadedCount;
        }
        m_LoadedChunkCount = loadedCount;
        if (loadedCount > 0) {
            m_LoadedBounds = recomputed;
            m_LoadedBoundsValid = true;
        } else {
            m_LoadedBoundsValid = false;
            m_LoadedBounds.min = glm::vec3(0.0f);
            m_LoadedBounds.max = glm::vec3(0.0f);
        }
        m_LoadedBoundsDirty = false;
    }

    Bounds b = m_LoadedBounds;
    if (!m_LoadedBoundsValid) {
        b.min = m_DetourWorldOrigin;
        b.max = m_DetourWorldOrigin;
    }
    std::unique_lock lk(m_Stitched->m_Lock);
    m_Stitched->m_Bounds = b;
}

void NavChunkManager::Update(Scene& scene, NavMeshComponent& comp)
{
    std::lock_guard lk(m_StateMutex);
    if (m_Path.empty()) return;
    if (m_Chunks.empty()) return;

    glm::vec3 camPos = ResolveStreamingCameraPos(scene);

    // Throttle: only update when camera nears the bounds of currently loaded nav, or chunks are loading
    size_t inFlight = 0;
    {
        std::lock_guard loadLk(m_LoadMutex);
        inFlight = m_LoadInFlight.size();
    }
    const glm::vec3 camDelta = camPos - m_LastStreamCameraPos;
    const float camMoveSq = glm::dot(camDelta, camDelta);
    const float minMoveForRescan = std::max(1.0f, comp.ChunkWorldSize * 0.2f);
    const float minMoveForRescanSq = minMoveForRescan * minMoveForRescan;
    const bool cameraMovedEnough = camMoveSq >= minMoveForRescanSq;
    if (cameraMovedEnough) {
        m_StreamIdleFrameCounter = 0;
    } else {
        ++m_StreamIdleFrameCounter;
        const uint32_t idleIntervalFrames = scene.m_IsPlaying ? 3u : 2u;
        if (inFlight == 0 && (m_StreamIdleFrameCounter % idleIntervalFrames) != 0u) {
            return;
        }
    }

    if (inFlight == 0 && m_Stitched && m_Stitched->HasDetour()) {
        if (m_LoadedBoundsDirty) {
            Bounds recomputed;
            recomputed.min = glm::vec3(FLT_MAX);
            recomputed.max = glm::vec3(-FLT_MAX);
            size_t loadedCount = 0;
            for (const auto& kv : m_Chunks) {
                if (!kv.second.loaded) continue;
                recomputed.expand(kv.second.bounds.min);
                recomputed.expand(kv.second.bounds.max);
                ++loadedCount;
            }
            m_LoadedChunkCount = loadedCount;
            if (loadedCount > 0) {
                m_LoadedBounds = recomputed;
                m_LoadedBoundsValid = true;
            } else {
                m_LoadedBoundsValid = false;
            }
            m_LoadedBoundsDirty = false;
        }
        if (m_LoadedBoundsValid) {
            const float distToEdge = DistanceToNearestEdgeXZ(camPos, m_LoadedBounds);
            const float margin = std::max(comp.ChunkWorldSize * 0.5f, comp.ChunkStreamRadius * 0.15f);
            if (distToEdge > margin) {
                m_LastStreamCameraPos = camPos;
                return;
            }
        }
    }
    m_LastStreamCameraPos = camPos;

    std::vector<EntityID> terrainSources;
    comp.GetTerrainSources(scene, terrainSources);
    TerrainComponent* terrain = nullptr;
    if (!terrainSources.empty()) {
        if (auto* d = scene.GetEntityData(terrainSources[0])) {
            if (d->Terrain) terrain = d->Terrain.get();
        }
    }

    std::vector<uint8_t> want;
    want.assign((size_t)m_ChunksX * m_ChunksZ, 0);
    const float streamRadiusSq = (comp.ChunkStreamRadius > 0.0f)
        ? (comp.ChunkStreamRadius * comp.ChunkStreamRadius)
        : 0.0f;
    if (terrain && comp.ChunkingMode == NavChunkingMode::Terrain && terrain->UseChunkedTerrain && !terrain->Chunks.empty()) {
        for (const auto& chunk : terrain->Chunks) {
            if (chunk.GridX >= m_ChunksX || chunk.GridZ >= m_ChunksZ) continue;
            if (chunk.Visible) {
                want[(size_t)chunk.GridZ * m_ChunksX + chunk.GridX] = 1;
            } else if (comp.ChunkStreamRadius > 0.0f) {
                Bounds b;
                b.min = glm::vec3(chunk.WorldMin.x, chunk.WorldMin.y, chunk.WorldMin.z);
                b.max = glm::vec3(chunk.WorldMax.x, chunk.WorldMax.y, chunk.WorldMax.z);
                if (DistanceToAabbXZSq(camPos, b) <= streamRadiusSq) {
                    want[(size_t)chunk.GridZ * m_ChunksX + chunk.GridX] = 1;
                }
            }
        }
    } else {
        for (auto& kv : m_Chunks) {
            const NavChunkRecord& rec = kv.second;
            const float distSq = DistanceToAabbXZSq(camPos, rec.bounds);
            if (comp.ChunkStreamRadius <= 0.0f || distSq <= streamRadiusSq) {
                size_t idx = (size_t)rec.key.z * m_ChunksX + (size_t)rec.key.x;
                if (idx < want.size()) want[idx] = 1;
            }
        }
    }

    // Padding
    const uint32_t pad = comp.BakeVisibleChunkPadding;
    if (pad > 0 && !want.empty()) {
        std::vector<uint8_t> padded = want;
        for (uint32_t z = 0; z < m_ChunksZ; ++z) {
            for (uint32_t x = 0; x < m_ChunksX; ++x) {
                if (!want[(size_t)z * m_ChunksX + x]) continue;
                for (int dz = -(int)pad; dz <= (int)pad; ++dz) {
                    for (int dx = -(int)pad; dx <= (int)pad; ++dx) {
                        int nx = (int)x + dx;
                        int nz = (int)z + dz;
                        if (nx < 0 || nz < 0 || nx >= (int)m_ChunksX || nz >= (int)m_ChunksZ) continue;
                        padded[(size_t)nz * m_ChunksX + nx] = 1;
                    }
                }
            }
        }
        want.swap(padded);
    }

    size_t wantCount = 0;
    for (uint8_t v : want) wantCount += (v != 0);

    const size_t loadedBefore = m_LoadedChunkCount;
    const bool coldStart = (loadedBefore == 0);
    const bool stillWarming = loadedBefore < wantCount;
    const size_t maxLoadsPerUpdate = coldStart ? 32u : (stillWarming ? 16u : (scene.m_IsPlaying ? 8u : 16u));
    const size_t maxUnloadsPerUpdate = stillWarming ? 0u : (scene.m_IsPlaying ? 8u : 16u);
    const float unloadHysteresis = std::max(16.0f, comp.ChunkWorldSize * 0.5f);
    const float unloadRadius = comp.ChunkStreamRadius > 0.0f ? (comp.ChunkStreamRadius + unloadHysteresis) : comp.ChunkStreamRadius;
    const float unloadRadiusSq = unloadRadius > 0.0f ? (unloadRadius * unloadRadius) : 0.0f;

    struct ChunkOpCandidate {
        uint64_t key = 0;
        float distanceSq = 0.0f;
    };
    std::vector<ChunkOpCandidate> loadCandidates;
    std::vector<ChunkOpCandidate> unloadCandidates;
    loadCandidates.reserve(64);
    unloadCandidates.reserve(64);

    bool changed = false;
    size_t loadedNow = 0;
    size_t unloadedNow = 0;
    size_t loadReadFail = 0;
    for (auto& kv : m_Chunks) {
        NavChunkRecord& rec = kv.second;
        const size_t idx = (size_t)rec.key.z * m_ChunksX + (size_t)rec.key.x;
        const bool wantedByRadius = idx < want.size() ? (want[idx] != 0) : false;
        const float distSq = DistanceToAabbXZSq(camPos, rec.bounds);
        const bool keepLoaded = rec.loaded && unloadRadiusSq > 0.0f && distSq <= unloadRadiusSq;
        const bool shouldLoad = wantedByRadius || keepLoaded;

        if (shouldLoad && !rec.loaded) {
            if (!rec.missingPayload && rec.loadFailStreak == 0) {
                loadCandidates.push_back({ kv.first, distSq });
            }
        } else if (!shouldLoad && rec.loaded) {
            unloadCandidates.push_back({ kv.first, distSq });
        } else if (!wantedByRadius && !rec.missingPayload && rec.loadFailStreak != 0 && rec.loadFailStreak != 255) {
            // Retry a previously failed chunk when it re-enters the wanted set.
            rec.loadFailStreak = 0;
        }
    }

    const size_t loadBudget = std::min(maxLoadsPerUpdate, loadCandidates.size());
    const size_t unloadBudget = std::min(maxUnloadsPerUpdate, unloadCandidates.size());
    if (loadBudget > 0) {
        auto loadCmp = [](const ChunkOpCandidate& a, const ChunkOpCandidate& b) {
            return a.distanceSq < b.distanceSq;
        };
        if (loadCandidates.size() > loadBudget) {
            std::nth_element(loadCandidates.begin(), loadCandidates.begin() + loadBudget, loadCandidates.end(), loadCmp);
            std::sort(loadCandidates.begin(), loadCandidates.begin() + loadBudget, loadCmp);
        } else {
            std::sort(loadCandidates.begin(), loadCandidates.end(), loadCmp);
        }
    }
    if (unloadBudget > 0) {
        auto unloadCmp = [](const ChunkOpCandidate& a, const ChunkOpCandidate& b) {
            return a.distanceSq > b.distanceSq;
        };
        if (unloadCandidates.size() > unloadBudget) {
            std::nth_element(unloadCandidates.begin(), unloadCandidates.begin() + unloadBudget, unloadCandidates.end(), unloadCmp);
            std::sort(unloadCandidates.begin(), unloadCandidates.begin() + unloadBudget, unloadCmp);
        } else {
            std::sort(unloadCandidates.begin(), unloadCandidates.end(), unloadCmp);
        }
    }

    const bool needsDetourWorld = (loadBudget > 0) || (loadedBefore > 0);
    if (needsDetourWorld && !EnsureDetourTileWorld(comp)) {
        if (!m_DetourWorldInitFailed) {
            std::cerr << "[Nav] Failed to initialize global Detour tile world for chunk streaming." << std::endl;
        }
        return;
    }

    size_t queuedNow = 0;
    for (size_t i = 0; i < loadBudget; ++i) {
        auto it = m_Chunks.find(loadCandidates[i].key);
        if (it == m_Chunks.end()) continue;
        NavChunkRecord& rec = it->second;
        if (rec.loaded || rec.missingPayload || rec.loadFailStreak != 0) continue;
        if (IsChunkLoadInFlight(loadCandidates[i].key)) continue;

        if (QueueChunkLoad(loadCandidates[i].key, rec)) {
            ++queuedNow;
        }
    }

    const size_t applyBudget = std::max<size_t>(1u, maxLoadsPerUpdate);
    for (size_t applied = 0; applied < applyBudget; ++applied) {
        CompletedChunkLoad result;
        if (!TryPopChunkLoadResult(result)) break;

        auto it = m_Chunks.find(result.key);
        if (it == m_Chunks.end()) continue;
        NavChunkRecord& rec = it->second;
        if (rec.loaded) continue;

        const size_t idx = (size_t)rec.key.z * m_ChunksX + (size_t)rec.key.x;
        const bool wantedByRadius = idx < want.size() ? (want[idx] != 0) : false;
        const float distSq = DistanceToAabbXZSq(camPos, rec.bounds);
        const bool keepLoaded = rec.loaded && unloadRadiusSq > 0.0f && distSq <= unloadRadiusSq;
        const bool shouldLoadNow = wantedByRadius || keepLoaded;
        if (!shouldLoadNow) {
            if (rec.loadFailStreak != 255) {
                rec.loadFailStreak = 0;
            }
            continue;
        }

        if (!result.success || !result.runtime || !result.runtime->HasDetour() || result.runtime->GetDetourNavData().empty()) {
            rec.loadFailStreak = 255;
            std::cerr << "[Nav] Chunk (" << rec.key.x << "," << rec.key.z
                      << ") has invalid/legacy nav data. Skipping chunk load; please Rebuild/Rebake navigation."
                      << std::endl;
            ++loadReadFail;
            continue;
        }

        if (m_Stitched) {
            std::unique_lock lk(m_Stitched->m_Lock);
            m_Stitched->m_AreaCost = result.runtime->m_AreaCost;
        }

        uint64_t tileRef = 0;
        if (!m_Stitched || !m_Stitched->AddDetourTileCopy(result.runtime->GetDetourNavData().data(),
                                                           result.runtime->GetDetourNavData().size(),
                                                           tileRef,
                                                           rec.key.x,
                                                           rec.key.z)) {
            rec.loadFailStreak = 255;
            std::cerr << "[Nav] Chunk (" << rec.key.x << "," << rec.key.z
                      << ") failed Detour tile import. Skipping chunk load; please Rebuild/Rebake navigation."
                      << std::endl;
            ++loadReadFail;
            continue;
        }

        rec.runtime = std::move(result.runtime);
        rec.loaded = true;
        rec.detourTileRef = tileRef;
        rec.loadFailStreak = 0;
        ++m_LoadedChunkCount;
        if (!m_LoadedBoundsValid) {
            m_LoadedBounds = rec.bounds;
            m_LoadedBoundsValid = true;
            m_LoadedBoundsDirty = false;
        } else {
            m_LoadedBounds.expand(rec.bounds.min);
            m_LoadedBounds.expand(rec.bounds.max);
        }
        changed = true;
        ++loadedNow;
    }

    for (size_t i = 0; i < unloadBudget; ++i) {
        auto it = m_Chunks.find(unloadCandidates[i].key);
        if (it == m_Chunks.end()) continue;
        NavChunkRecord& rec = it->second;
        if (!rec.loaded) continue;
        if (rec.detourTileRef != 0 && m_Stitched) {
            m_Stitched->RemoveDetourTile(rec.detourTileRef);
            rec.detourTileRef = 0;
        }
        rec.runtime.reset();
        rec.loaded = false;
        if (m_LoadedChunkCount > 0) {
            --m_LoadedChunkCount;
        }
        if (m_LoadedChunkCount == 0) {
            m_LoadedBoundsValid = false;
            m_LoadedBoundsDirty = false;
            m_LoadedBounds.min = glm::vec3(0.0f);
            m_LoadedBounds.max = glm::vec3(0.0f);
        } else {
            m_LoadedBoundsDirty = true;
        }
        changed = true;
        ++unloadedNow;
    }

    if (changed) {
        UpdateDetourWorldBounds();
        m_StitchDirty = false;
        ++m_StreamRevision;
    }

    if (changed) {
#if 0 // Disable hot-path diagnostic output (enable for debugging)
        size_t loadedTotal = 0;
        for (const auto& kv : m_Chunks) loadedTotal += (kv.second.loaded ? 1u : 0u);
        size_t inFlightLoads = 0;
        {
            std::lock_guard lk(m_LoadMutex);
            inFlightLoads = m_LoadInFlight.size();
        }
        std::cout << "[Nav][Diag] Stream update: cam=(" << camPos.x << "," << camPos.y << "," << camPos.z << ")"
                  << " radius=" << comp.ChunkStreamRadius
                  << " want=" << wantCount
                  << " loadedNow=" << loadedNow
                  << " unloadedNow=" << unloadedNow
                  << " queuedNow=" << queuedNow
                  << " inFlight=" << inFlightLoads
                  << " readFail=" << loadReadFail
                  << " loadedTotal=" << loadedTotal
                  << " detourWorld=" << ((m_Stitched && m_Stitched->HasDetour()) ? 1 : 0)
                  << std::endl;
#endif
    }
}

void NavChunkManager::RebuildStitched(const NavMeshComponent& comp)
{
    m_StitchDirty = false;
    std::vector<glm::vec3> verts;
    std::vector<NavMeshRuntime::Poly> polys;
    std::vector<uint16_t> areas;
    std::vector<float> costs;

    for (const auto& kv : m_Chunks) {
        const NavChunkRecord& rec = kv.second;
        if (!rec.loaded || !rec.runtime) continue;
        uint32_t base = (uint32_t)verts.size();
        verts.insert(verts.end(), rec.runtime->m_Vertices.begin(), rec.runtime->m_Vertices.end());
        for (const auto& p : rec.runtime->m_Polys) {
            NavMeshRuntime::Poly np;
            np.i0 = base + p.i0;
            np.i1 = base + p.i1;
            np.i2 = base + p.i2;
            np.area = p.area;
            np.flags = p.flags;
            np.cost = p.cost;
            polys.push_back(np);
        }
    }
    auto rt = std::make_shared<NavMeshRuntime>();
    rt->m_Vertices = std::move(verts);
    rt->m_Polys = std::move(polys);
    rt->m_AreaCost = comp.Runtime ? comp.Runtime->m_AreaCost : std::array<float, 64>{};
    if (!comp.Runtime) rt->m_AreaCost.fill(1.0f);

    if (!comp.EnableStitching) {
        // In non-stitch mode, keep stitched runtime lightweight for debug/queries:
        // skip expensive cross-chunk edge matching and rebuild only bounds + BVH.
        rt->m_Adjacency.clear();
        if (!rt->m_Vertices.empty()) {
            Bounds bounds;
            bounds.min = glm::vec3(FLT_MAX);
            bounds.max = glm::vec3(-FLT_MAX);
            for (const auto& v : rt->m_Vertices) bounds.expand(v);
            rt->m_Bounds = bounds;
        }
        rt->RebuildBVH();
        m_Stitched = std::move(rt);
        return;
    }

    // Build adjacency by edge matching (stitch across chunks)
    rt->m_Adjacency.assign(rt->m_Polys.size(), {});
    const float eps = std::max(1e-4f, comp.StitchEpsilon);
    const float cosMaxAngle = std::cos(glm::radians(comp.StitchMaxNormalAngleDeg));
    struct QuantPos { int x, y, z; };
    struct EdgeKey { QuantPos a, b; };
    struct EdgeRec { uint32_t tri; glm::vec3 aPos; glm::vec3 bPos; };
    auto quantize = [&](const glm::vec3& v) -> QuantPos {
        return { (int)std::floor(v.x / eps + 0.5f),
                 (int)std::floor(v.y / eps + 0.5f),
                 (int)std::floor(v.z / eps + 0.5f) };
    };
    auto lessPos = [](const QuantPos& a, const QuantPos& b) {
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        return a.z < b.z;
    };
    auto makeKey = [&](const glm::vec3& va, const glm::vec3& vb) -> EdgeKey {
        QuantPos qa = quantize(va);
        QuantPos qb = quantize(vb);
        if (lessPos(qb, qa)) std::swap(qa, qb);
        return { qa, qb };
    };
    struct EdgeKeyEq {
        bool operator()(const EdgeKey& x, const EdgeKey& y) const {
            return x.a.x == y.a.x && x.a.y == y.a.y && x.a.z == y.a.z &&
                   x.b.x == y.b.x && x.b.y == y.b.y && x.b.z == y.b.z;
        }
    };
    struct EdgeKeyHash {
        size_t operator()(const EdgeKey& k) const {
            const uint64_t h1 = (uint64_t)(k.a.x * 73856093) ^ (uint64_t)(k.a.y * 19349663) ^ (uint64_t)(k.a.z * 83492791);
            const uint64_t h2 = (uint64_t)(k.b.x * 73856093) ^ (uint64_t)(k.b.y * 19349663) ^ (uint64_t)(k.b.z * 83492791);
            return (size_t)(h1 ^ (h2 << 1));
        }
    };
    std::vector<glm::vec3> polyNormals(rt->m_Polys.size(), glm::vec3(0.0f, 1.0f, 0.0f));
    for (uint32_t t = 0; t < (uint32_t)rt->m_Polys.size(); ++t) {
        const auto& p = rt->m_Polys[t];
        if (p.i0 >= rt->m_Vertices.size() || p.i1 >= rt->m_Vertices.size() || p.i2 >= rt->m_Vertices.size()) continue;
        const glm::vec3& a = rt->m_Vertices[p.i0];
        const glm::vec3& b = rt->m_Vertices[p.i1];
        const glm::vec3& c = rt->m_Vertices[p.i2];
        glm::vec3 n = glm::cross(b - a, c - a);
        float len = glm::length(n);
        if (len > 1e-6f) polyNormals[t] = n / len;
    }
    auto normalsCompatible = [&](uint32_t a, uint32_t b) {
        return glm::dot(polyNormals[a], polyNormals[b]) >= cosMaxAngle;
    };
    const float epsSq = eps * eps;
    auto endpointsMatch = [&](const EdgeRec& e1, const EdgeRec& e2) {
        float d00 = glm::distance2(e1.aPos, e2.aPos);
        float d11 = glm::distance2(e1.bPos, e2.bPos);
        float d01 = glm::distance2(e1.aPos, e2.bPos);
        float d10 = glm::distance2(e1.bPos, e2.aPos);
        return (d00 <= epsSq && d11 <= epsSq) || (d01 <= epsSq && d10 <= epsSq);
    };
    std::unordered_map<EdgeKey, std::vector<EdgeRec>, EdgeKeyHash, EdgeKeyEq> buckets;
    buckets.reserve(rt->m_Polys.size() * 2);
    for (uint32_t t = 0; t < (uint32_t)rt->m_Polys.size(); ++t) {
        const auto& p = rt->m_Polys[t];
        const glm::vec3& v0 = rt->m_Vertices[p.i0];
        const glm::vec3& v1 = rt->m_Vertices[p.i1];
        const glm::vec3& v2 = rt->m_Vertices[p.i2];
        buckets[makeKey(v0, v1)].push_back({ t, v0, v1 });
        buckets[makeKey(v1, v2)].push_back({ t, v1, v2 });
        buckets[makeKey(v2, v0)].push_back({ t, v2, v0 });
    }
    for (auto& kv : buckets) {
        auto& list = kv.second;
        if (list.size() < 2) continue;
        for (size_t i = 0; i < list.size(); ++i) {
            for (size_t j = i + 1; j < list.size(); ++j) {
                uint32_t a = list[i].tri;
                uint32_t b = list[j].tri;
                if (a == b) continue;
                if (!endpointsMatch(list[i], list[j])) continue;
                if (!normalsCompatible(a, b)) continue;
                rt->m_Adjacency[a].push_back(b);
                rt->m_Adjacency[b].push_back(a);
            }
        }
    }
    for (auto& adj : rt->m_Adjacency) {
        std::sort(adj.begin(), adj.end());
        adj.erase(std::unique(adj.begin(), adj.end()), adj.end());
    }

    // Bounds + BVH
    if (!rt->m_Vertices.empty()) {
        Bounds bounds;
        bounds.min = glm::vec3(FLT_MAX);
        bounds.max = glm::vec3(-FLT_MAX);
        for (const auto& v : rt->m_Vertices) bounds.expand(v);
        rt->m_Bounds = bounds;
    }
    rt->RebuildBVH();
    m_Stitched = std::move(rt);
}

// Helper: compute bounds for a range of triangles
static Bounds ComputePolyBounds(const std::vector<glm::vec3>& verts, const std::vector<NavMeshRuntime::Poly>& polys, 
                                const std::vector<uint32_t>& indices, uint32_t start, uint32_t count)
{
    Bounds b; b.min = glm::vec3(FLT_MAX); b.max = glm::vec3(-FLT_MAX);
    for (uint32_t i = start; i < start + count; ++i) {
        if (i >= indices.size()) continue;
        uint32_t polyIdx = indices[i];
        if (polyIdx >= polys.size()) continue;
        const auto& p = polys[polyIdx];
        if (p.i0 < verts.size()) b.expand(verts[p.i0]);
        if (p.i1 < verts.size()) b.expand(verts[p.i1]);
        if (p.i2 < verts.size()) b.expand(verts[p.i2]);
    }
    return b;
}

// Helper: compute centroid of a triangle
static glm::vec3 PolyCentroid(const std::vector<glm::vec3>& verts, const NavMeshRuntime::Poly& p)
{
    if (p.i0 >= verts.size() || p.i1 >= verts.size() || p.i2 >= verts.size()) {
        return glm::vec3(0.0f);
    }
    return (verts[p.i0] + verts[p.i1] + verts[p.i2]) / 3.0f;
}

static float PointAABBDistSq(const glm::vec3& p, const Bounds& b)
{
    float dx = std::max(std::max(b.min.x - p.x, 0.0f), p.x - b.max.x);
    float dy = std::max(std::max(b.min.y - p.y, 0.0f), p.y - b.max.y);
    float dz = std::max(std::max(b.min.z - p.z, 0.0f), p.z - b.max.z);
    return dx * dx + dy * dy + dz * dz;
}

static uint32_t FindNearestPolyIndex(const NavMeshRuntime& nm, const glm::vec3& pos)
{
    if (nm.m_Polys.empty()) return UINT32_MAX;
    if (nm.m_BVH.empty()) {
        float bestDist = FLT_MAX;
        uint32_t bestIdx = UINT32_MAX;
        for (uint32_t i = 0; i < nm.m_Polys.size(); ++i) {
            const auto& p = nm.m_Polys[i];
            if (p.i0 >= nm.m_Vertices.size() || p.i1 >= nm.m_Vertices.size() || p.i2 >= nm.m_Vertices.size()) continue;
            glm::vec3 c = PolyCentroid(nm.m_Vertices, p);
            glm::vec3 d = pos - c;
            float distSq = glm::dot(d, d);
            if (distSq < bestDist) { bestDist = distSq; bestIdx = i; }
        }
        return bestIdx;
    }

    struct Entry { uint32_t nodeIdx; float distSq; };
    auto cmp = [](const Entry& a, const Entry& b) { return a.distSq > b.distSq; };
    std::priority_queue<Entry, std::vector<Entry>, decltype(cmp)> pq(cmp);
    pq.push({0, PointAABBDistSq(pos, nm.m_BVH[0].b)});

    float bestDistSq = FLT_MAX;
    uint32_t bestIdx = UINT32_MAX;
    while (!pq.empty()) {
        Entry e = pq.top(); pq.pop();
        if (e.distSq > bestDistSq) continue;
        const auto& node = nm.m_BVH[e.nodeIdx];
        if (node.count > 0) {
            for (uint32_t i = node.start; i < node.start + node.count && i < nm.m_BVHIndices.size(); ++i) {
                uint32_t polyIdx = nm.m_BVHIndices[i];
                if (polyIdx >= nm.m_Polys.size()) continue;
                const auto& p = nm.m_Polys[polyIdx];
                if (p.i0 >= nm.m_Vertices.size() || p.i1 >= nm.m_Vertices.size() || p.i2 >= nm.m_Vertices.size()) continue;
                glm::vec3 c = PolyCentroid(nm.m_Vertices, p);
                glm::vec3 d = pos - c;
                float distSq = glm::dot(d, d);
                if (distSq < bestDistSq) { bestDistSq = distSq; bestIdx = polyIdx; }
            }
        } else {
            if (node.left < nm.m_BVH.size()) {
                float ld = PointAABBDistSq(pos, nm.m_BVH[node.left].b);
                if (ld < bestDistSq) pq.push({node.left, ld});
            }
            if (node.right < nm.m_BVH.size()) {
                float rd = PointAABBDistSq(pos, nm.m_BVH[node.right].b);
                if (rd < bestDistSq) pq.push({node.right, rd});
            }
        }
    }
    return bestIdx;
}

static void BuildLinkAdjacencyLocked(NavMeshRuntime& nm)
{
    nm.m_LinkAdjacency.clear();
    nm.m_LinkAdjacency.resize(nm.m_Polys.size());
    if (nm.m_Polys.empty() || nm.m_BVH.empty()) return;

    auto addLink = [&](const OffMeshLink& link) {
        float radius = std::max(link.radius, 0.0f);
        glm::vec3 aOn, bOn;
        if (!queries::NearestPointOnNavmesh(nm, link.a, radius, aOn)) return;
        if (!queries::NearestPointOnNavmesh(nm, link.b, radius, bOn)) return;
        uint32_t aPoly = FindNearestPolyIndex(nm, aOn);
        uint32_t bPoly = FindNearestPolyIndex(nm, bOn);
        if (aPoly == UINT32_MAX || bPoly == UINT32_MAX || aPoly == bPoly) return;
        const float cost = std::max(link.cost, 0.01f);
        nm.m_LinkAdjacency[aPoly].push_back({ bPoly, aOn, bOn, cost, link.flags });
        if (link.bidir) {
            nm.m_LinkAdjacency[bPoly].push_back({ aPoly, bOn, aOn, cost, link.flags });
        }
    };

    for (const auto& link : nm.m_Links) {
        addLink(link);
    }
    for (const auto& link : nm.m_ExternalLinks) {
        addLink(link);
    }
}

void NavMeshRuntime::RebuildBVH()
{
    std::unique_lock lk(m_Lock);
    m_BVH.clear();
    m_BVHIndices.clear();
    
    if (m_Polys.empty()) {
        m_Bounds.min = m_Bounds.max = glm::vec3(0.0f);
        return;
    }
    
    // Validate all polygon indices before building BVH
    size_t vertCount = m_Vertices.size();
    size_t invalidCount = 0;
    for (size_t i = 0; i < m_Polys.size(); ++i) {
        const auto& p = m_Polys[i];
        if (p.i0 >= vertCount || p.i1 >= vertCount || p.i2 >= vertCount) {
            invalidCount++;
            if (invalidCount <= 5) {
                std::cerr << "[Nav] RebuildBVH: Invalid polygon[" << i << "] indices: (" 
                          << p.i0 << ", " << p.i1 << ", " << p.i2 << ") >= " << vertCount << std::endl;
            }
        }
    }
    if (invalidCount > 0) {
        std::cerr << "[Nav] RebuildBVH: Found " << invalidCount << " invalid polygons out of " << m_Polys.size() 
                  << ". Aborting BVH build." << std::endl;
        return;
    }
    
    // Initialize indices
    m_BVHIndices.resize(m_Polys.size());
    for (uint32_t i = 0; i < (uint32_t)m_Polys.size(); ++i) m_BVHIndices[i] = i;
    
    // Compute overall bounds
    m_Bounds.min = glm::vec3(FLT_MAX); m_Bounds.max = glm::vec3(-FLT_MAX);
    for (const auto& v : m_Vertices) m_Bounds.expand(v);
    
    // Build BVH recursively using median split
    const uint32_t kMaxLeafSize = 16;  // Max triangles per leaf for chunked queries
    
    struct BuildTask { uint32_t nodeIdx; uint32_t start; uint32_t count; };
    std::vector<BuildTask> stack;
    
    // Create root node
    BVNode root;
    root.b = ComputePolyBounds(m_Vertices, m_Polys, m_BVHIndices, 0, (uint32_t)m_Polys.size());
    root.start = 0;
    root.count = (uint32_t)m_Polys.size();
    m_BVH.push_back(root);
    
    stack.push_back({0, 0, (uint32_t)m_Polys.size()});
    
    while (!stack.empty()) {
        BuildTask task = stack.back();
        stack.pop_back();
        
        BVNode& node = m_BVH[task.nodeIdx];
        
        // If small enough, keep as leaf
        if (task.count <= kMaxLeafSize) {
            node.start = task.start;
            node.count = task.count;
            continue;
        }
        
        // Find best axis to split (longest extent)
        glm::vec3 extent = node.b.max - node.b.min;
        int axis = (extent.x >= extent.y && extent.x >= extent.z) ? 0 : 
                   (extent.y >= extent.z) ? 1 : 2;
        
        // Sort triangles by centroid along chosen axis
        std::sort(m_BVHIndices.begin() + task.start, 
                  m_BVHIndices.begin() + task.start + task.count,
                  [&](uint32_t a, uint32_t b) {
                      glm::vec3 ca = PolyCentroid(m_Vertices, m_Polys[a]);
                      glm::vec3 cb = PolyCentroid(m_Vertices, m_Polys[b]);
                      return ca[axis] < cb[axis];
                  });
        
        // Split at median
        uint32_t mid = task.count / 2;
        uint32_t leftCount = mid;
        uint32_t rightCount = task.count - mid;
        
        // Create child nodes
        BVNode leftNode;
        leftNode.b = ComputePolyBounds(m_Vertices, m_Polys, m_BVHIndices, task.start, leftCount);
        leftNode.start = task.start;
        leftNode.count = leftCount;
        
        BVNode rightNode;
        rightNode.b = ComputePolyBounds(m_Vertices, m_Polys, m_BVHIndices, task.start + mid, rightCount);
        rightNode.start = task.start + mid;
        rightNode.count = rightCount;
        
        uint32_t leftIdx = (uint32_t)m_BVH.size();
        uint32_t rightIdx = leftIdx + 1;
        
        m_BVH.push_back(leftNode);
        m_BVH.push_back(rightNode);
        
        // Update parent to internal node
        m_BVH[task.nodeIdx].left = leftIdx;
        m_BVH[task.nodeIdx].right = rightIdx;
        m_BVH[task.nodeIdx].count = 0; // Mark as internal node
        
        // Add children to stack for further processing
        stack.push_back({leftIdx, task.start, leftCount});
        stack.push_back({rightIdx, task.start + mid, rightCount});
    }

    BuildLinkAdjacencyLocked(*this);
}

void NavMeshRuntime::RebuildLinkAdjacency()
{
    std::unique_lock lk(m_Lock);
    BuildLinkAdjacencyLocked(*this);
}

void NavMeshRuntime::SetExternalLinks(std::vector<OffMeshLink> links)
{
    std::unique_lock lk(m_Lock);
    m_ExternalLinks = std::move(links);
    BuildLinkAdjacencyLocked(*this);
}


