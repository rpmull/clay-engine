
#include "core/navigation/NavJobs.h"
#include "core/navigation/NavMesh.h"
#include "core/navigation/NavSerialization.h"
#include "core/ecs/Scene.h"
#include "core/ecs/Components.h"
#include "core/rendering/Terrain.h"
#include "core/vfs/FileSystem.h"

#include <Recast.h>
#include <DetourAlloc.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>

#include <array>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>
#include <atomic>
#include <regex>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace nav;

namespace
{
static constexpr uint16_t kMeshAreaId = 63; // Reserve top area id for mesh triangles.

class RecastCtx final : public rcContext
{
public:
    explicit RecastCtx(bool state) : rcContext(state) {}
};

static bool BoundsOverlapXZ(const Bounds& a, const Bounds& b)
{
    if (a.max.x < b.min.x || a.min.x > b.max.x) return false;
    if (a.max.z < b.min.z || a.min.z > b.max.z) return false;
    return true;
}
struct MeshNameRegexFilter
{
    bool enabled = false;
    std::string pattern;
    std::regex regex;
};

static MeshNameRegexFilter BuildMeshNameRegexFilter(const NavMeshComponent& comp)
{
    MeshNameRegexFilter filter{};
    filter.enabled = comp.GeometryIncludeRegexEnabled && !comp.GeometryIncludeRegexPattern.empty();
    filter.pattern = comp.GeometryIncludeRegexPattern;
    if (!filter.enabled) return filter;

    try {
        filter.regex = std::regex(
            filter.pattern,
            std::regex_constants::ECMAScript | std::regex_constants::icase
        );
    } catch (const std::regex_error& ex) {
        std::cerr << "[Nav] Invalid geometry regex pattern \"" << filter.pattern
                  << "\": " << ex.what() << ". Ignoring filter." << std::endl;
        filter.enabled = false;
    }
    return filter;
}

static bool MeshNamePassesRegexFilter(const MeshNameRegexFilter& filter, const std::string& name)
{
    if (!filter.enabled) return true;
    return std::regex_search(name, filter.regex);
}

static bool ValidatePolyMeshTopology(const rcPolyMesh& mesh, const char* label)
{
    if (!mesh.polys || mesh.nvp <= 0 || mesh.npolys <= 0 || mesh.nverts <= 0) {
        std::cerr << "[Nav] Recast: invalid polymesh buffers (" << (label ? label : "unknown") << ")." << std::endl;
        return false;
    }

    int invalidPolys = 0;
    for (int i = 0; i < mesh.npolys; ++i) {
        const unsigned short* poly = &mesh.polys[(size_t)i * (size_t)mesh.nvp * 2];
        int vcount = 0;
        bool bad = false;
        unsigned short seen[DT_VERTS_PER_POLYGON] = {};
        int seenCount = 0;
        for (int j = 0; j < mesh.nvp; ++j) {
            const unsigned short vi = poly[j];
            if (vi == RC_MESH_NULL_IDX) break;
            if ((int)vi >= mesh.nverts) { bad = true; break; }
            for (int k = 0; k < seenCount; ++k) {
                if (seen[k] == vi) { bad = true; break; }
            }
            if (bad) break;
            seen[seenCount++] = vi;
            ++vcount;
        }
        if (vcount < 3) bad = true;
        if (bad) ++invalidPolys;
    }

    if (invalidPolys > 0) {
        std::cerr << "[Nav] Recast: invalid polygon topology in " << invalidPolys
                  << "/" << mesh.npolys << " polys (" << (label ? label : "unknown") << ")." << std::endl;
        return false;
    }
    return true;
}

static bool SafeBuildPolyMeshDetail(rcContext* ctx, const rcPolyMesh& mesh, const rcCompactHeightfield& chf,
                                    float sampleDist, float sampleMaxError, rcPolyMeshDetail& dmesh)
{
#if defined(_MSC_VER)
    __try {
        return rcBuildPolyMeshDetail(ctx, mesh, chf, sampleDist, sampleMaxError, dmesh);
    } __except (1) {
        std::cerr << "[Nav] Recast: exception in rcBuildPolyMeshDetail (access violation)." << std::endl;
        return false;
    }
#else
    return rcBuildPolyMeshDetail(ctx, mesh, chf, sampleDist, sampleMaxError, dmesh);
#endif
}

static bool BuildDetourData(const std::vector<glm::vec3>& verts,
                            const std::vector<uint32_t>& tris,
                            const std::vector<uint16_t>& areas,
                            const Bounds& bounds,
                            const NavBakeSettings& bake,
                            std::vector<uint8_t>& outNavData,
                            bool logGrid = true,
                            bool buildDetail = true,
                            bool asDetourTile = false,
                            int tileX = 0,
                            int tileY = 0);

static double EstimateGridCellsXZ(const Bounds& b, float cellSize)
{
    if (cellSize <= 0.0f) return 0.0;
    const double spanX = std::max(0.0, (double)b.max.x - (double)b.min.x);
    const double spanZ = std::max(0.0, (double)b.max.z - (double)b.min.z);
    return (spanX / cellSize) * (spanZ / cellSize);
}

static bool TriIntersectsAabb(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const Bounds& box)
{
    const glm::vec3 triMin = glm::min(a, glm::min(b, c));
    const glm::vec3 triMax = glm::max(a, glm::max(b, c));
    if (triMax.x < box.min.x || triMin.x > box.max.x) return false;
    if (triMax.y < box.min.y || triMin.y > box.max.y) return false;
    if (triMax.z < box.min.z || triMin.z > box.max.z) return false;
    return true;
}

static void ApplyPerPolyCostsFromTerrain(Scene* scene, NavMeshRuntime& rt);

static bool BuildWorldGridChunkedNavPack(const std::vector<glm::vec3>& verts,
                                         const std::vector<uint32_t>& tris,
                                         const std::vector<uint16_t>& areas,
                                         const Bounds& sceneBounds,
                                         Scene* scene,
                                         const NavMeshComponent& comp,
                                         const std::array<float, 64>& areaCosts,
                                         uint64_t bakeHash,
                                         const std::filesystem::path& packPath,
                                         std::shared_ptr<NavMeshRuntime>& previewRuntime)
{
    if (verts.empty() || tris.empty() || tris.size() % 3 != 0) return false;

    const float chunkSize = std::max(32.0f, comp.ChunkWorldSize);
    const float pad = std::max(0u, comp.ChunkBakePadding) * chunkSize;
    const float spanX = std::max(1.0f, sceneBounds.max.x - sceneBounds.min.x);
    const float spanZ = std::max(1.0f, sceneBounds.max.z - sceneBounds.min.z);
    const uint32_t chunksX = std::max(1u, (uint32_t)std::ceil(spanX / chunkSize));
    const uint32_t chunksZ = std::max(1u, (uint32_t)std::ceil(spanZ / chunkSize));

    nav::io::NavPackMeta meta{};
    meta.chunksX = chunksX;
    meta.chunksZ = chunksZ;
    meta.sceneGuidHigh = comp.NavMeshDataGuid.high;
    meta.sceneGuidLow = comp.NavMeshDataGuid.low;
    meta.bakeHash = bakeHash;

    const size_t totalChunks = (size_t)chunksX * (size_t)chunksZ;
    std::vector<nav::io::NavPackChunk> entries(totalChunks);
    std::vector<std::vector<uint8_t>> payloads(totalChunks);

    const size_t triCount = tris.size() / 3;
    const int padChunks = (int)std::max(0u, comp.ChunkBakePadding);

    // Spatially bin triangles by chunk coverage so chunk assembly is O(local), not O(allTriangles).
    std::vector<std::vector<uint32_t>> triBins(totalChunks);
    std::vector<uint8_t> occupied(totalChunks, 0);
    for (size_t t = 0; t < triCount; ++t) {
        const uint32_t gi0 = tris[t * 3 + 0];
        const uint32_t gi1 = tris[t * 3 + 1];
        const uint32_t gi2 = tris[t * 3 + 2];
        if (gi0 >= verts.size() || gi1 >= verts.size() || gi2 >= verts.size()) continue;
        const glm::vec3& a = verts[gi0];
        const glm::vec3& b = verts[gi1];
        const glm::vec3& c = verts[gi2];
        const glm::vec3 triMin = glm::min(a, glm::min(b, c));
        const glm::vec3 triMax = glm::max(a, glm::max(b, c));

        const int minX = std::clamp((int)std::floor((triMin.x - sceneBounds.min.x) / chunkSize), 0, (int)chunksX - 1);
        const int maxX = std::clamp((int)std::floor((triMax.x - sceneBounds.min.x) / chunkSize), 0, (int)chunksX - 1);
        const int minZ = std::clamp((int)std::floor((triMin.z - sceneBounds.min.z) / chunkSize), 0, (int)chunksZ - 1);
        const int maxZ = std::clamp((int)std::floor((triMax.z - sceneBounds.min.z) / chunkSize), 0, (int)chunksZ - 1);

        for (int z = minZ; z <= maxZ; ++z) {
            for (int x = minX; x <= maxX; ++x) {
                const size_t bi = (size_t)z * chunksX + (size_t)x;
                triBins[bi].push_back((uint32_t)t);
                occupied[bi] = 1;
            }
        }
    }

    // Mark active chunks as those near occupied chunks (respecting padding).
    std::vector<uint8_t> active(totalChunks, 0);
    for (uint32_t z = 0; z < chunksZ; ++z) {
        for (uint32_t x = 0; x < chunksX; ++x) {
            const size_t oi = (size_t)z * chunksX + (size_t)x;
            if (!occupied[oi]) continue;
            const int minZ = std::max(0, (int)z - padChunks);
            const int maxZ = std::min((int)chunksZ - 1, (int)z + padChunks);
            const int minX = std::max(0, (int)x - padChunks);
            const int maxX = std::min((int)chunksX - 1, (int)x + padChunks);
            for (int zz = minZ; zz <= maxZ; ++zz) {
                for (int xx = minX; xx <= maxX; ++xx) {
                    active[(size_t)zz * chunksX + (size_t)xx] = 1;
                }
            }
        }
    }

    size_t activeChunks = 0;
    for (uint8_t v : active) activeChunks += (v != 0);
    std::cout << "[Nav] Chunked bake active chunks: " << activeChunks << "/" << totalChunks << std::endl;

    std::vector<size_t> activeIndices;
    activeIndices.reserve(activeChunks);
    for (uint32_t z = 0; z < chunksZ; ++z) {
        for (uint32_t x = 0; x < chunksX; ++x) {
            const size_t ci = (size_t)z * chunksX + (size_t)x;
            nav::io::NavPackChunk& entry = entries[ci];
            entry.gridX = (int32_t)x;
            entry.gridZ = (int32_t)z;
            entry.bounds.min = glm::vec3(sceneBounds.min.x + x * chunkSize, sceneBounds.min.y, sceneBounds.min.z + z * chunkSize);
            entry.bounds.max = glm::vec3(sceneBounds.min.x + (x + 1) * chunkSize, sceneBounds.max.y, sceneBounds.min.z + (z + 1) * chunkSize);
            if (!active[ci]) {
                entry.hash = 0;
                entry.size = 0;
                continue;
            }
            activeIndices.push_back(ci);
        }
    }

    auto resolveLocalViewCameraPos = [&](bool& usedActiveCamera) -> glm::vec3 {
        usedActiveCamera = false;
        if (!scene) return glm::vec3(0.0f);
        auto viewportPos = [&]() -> glm::vec3 {
            if (!scene->HasEditorViewportState()) return glm::vec3(0.0f);
            const Scene::EditorViewportState& s = scene->GetEditorViewportState();
            const float yaw = glm::radians(s.Yaw);
            const float pitch = glm::radians(s.Pitch);
            const float cosPitch = std::cos(pitch);
            glm::vec3 forward(cosPitch * std::sin(yaw), std::sin(pitch), cosPitch * std::cos(yaw));
            if (glm::length2(forward) < 1e-8f) forward = glm::vec3(0.0f, 0.0f, -1.0f);
            forward = glm::normalize(forward);
            return s.Target - forward * std::max(0.01f, s.Distance);
        };

        if (scene->m_IsPlaying) {
            // Play mode: local-view should track active in-scene gameplay camera.
            if (Camera* cam = scene->GetActiveCamera()) {
                usedActiveCamera = true;
                return cam->GetPosition();
            }
            // Fallback for scenes without a runtime camera.
            if (scene->HasEditorViewportState()) return viewportPos();
            return glm::vec3(0.0f);
        }

        // Editor mode: local-view follows editor viewport camera.
        if (scene->HasEditorViewportState()) return viewportPos();
        if (Camera* cam = scene->GetActiveCamera()) {
            usedActiveCamera = true;
            return cam->GetPosition();
        }
        return glm::vec3(0.0f);
    };
    auto distanceToAabbXZ = [&](const glm::vec3& p, const Bounds& b) -> float {
        const float cx = std::clamp(p.x, b.min.x, b.max.x);
        const float cz = std::clamp(p.z, b.min.z, b.max.z);
        const float dx = p.x - cx;
        const float dz = p.z - cz;
        return std::sqrt(dx * dx + dz * dz);
    };

    std::vector<size_t> bakeIndices = activeIndices;
    if (comp.BakeVisibleChunksOnly) {
        bool usedActiveCamera = false;
        const glm::vec3 camPos = resolveLocalViewCameraPos(usedActiveCamera);
        const float localRadius = comp.ChunkStreamRadius > 0.0f
                                ? comp.ChunkStreamRadius
                                : std::max(chunkSize * 4.0f, 256.0f);
        std::vector<uint8_t> selected(totalChunks, 0);
        size_t selectedCount = 0;
        for (size_t ci : activeIndices) {
            if (distanceToAabbXZ(camPos, entries[ci].bounds) <= localRadius) {
                selected[ci] = 1;
                ++selectedCount;
            }
        }

        // Fallback: if radius excluded all chunks, include nearest active chunk.
        if (selectedCount == 0 && !activeIndices.empty()) {
            size_t nearest = activeIndices[0];
            float bestDist = distanceToAabbXZ(camPos, entries[nearest].bounds);
            for (size_t ci : activeIndices) {
                const float d = distanceToAabbXZ(camPos, entries[ci].bounds);
                if (d < bestDist) {
                    bestDist = d;
                    nearest = ci;
                }
            }
            selected[nearest] = 1;
        }

        const int viewPad = (int)std::max(0u, comp.BakeVisibleChunkPadding);
        if (viewPad > 0) {
            std::vector<uint8_t> expanded = selected;
            for (size_t ci : activeIndices) {
                if (!selected[ci]) continue;
                const int cz = (int)(ci / chunksX);
                const int cx = (int)(ci % chunksX);
                const int minZ = std::max(0, cz - viewPad);
                const int maxZ = std::min((int)chunksZ - 1, cz + viewPad);
                const int minX = std::max(0, cx - viewPad);
                const int maxX = std::min((int)chunksX - 1, cx + viewPad);
                for (int zz = minZ; zz <= maxZ; ++zz) {
                    for (int xx = minX; xx <= maxX; ++xx) {
                        expanded[(size_t)zz * chunksX + (size_t)xx] = 1;
                    }
                }
            }
            selected.swap(expanded);
        }

        bakeIndices.clear();
        bakeIndices.reserve(activeIndices.size());
        for (size_t ci : activeIndices) {
            if (selected[ci]) bakeIndices.push_back(ci);
        }
        std::cout << "[Nav] Local-view proximity selection: " << bakeIndices.size()
                  << "/" << activeIndices.size()
                  << " radius=" << localRadius
                  << " source=" << (usedActiveCamera ? "active-scene-camera" : "editor-viewport")
                  << std::endl;
    }

    if (!bakeIndices.empty() && (comp.BakeVisibleChunksOnly || comp.BakeMissingChunksOnly)) {
        Bounds selectedBounds;
        selectedBounds.min = glm::vec3(FLT_MAX);
        selectedBounds.max = glm::vec3(-FLT_MAX);
        int minChunkX = std::numeric_limits<int>::max();
        int minChunkZ = std::numeric_limits<int>::max();
        int maxChunkX = std::numeric_limits<int>::min();
        int maxChunkZ = std::numeric_limits<int>::min();
        for (size_t ci : bakeIndices) {
            const auto& e = entries[ci];
            selectedBounds.expand(e.bounds.min);
            selectedBounds.expand(e.bounds.max);
            minChunkX = std::min(minChunkX, e.gridX);
            minChunkZ = std::min(minChunkZ, e.gridZ);
            maxChunkX = std::max(maxChunkX, e.gridX);
            maxChunkZ = std::max(maxChunkZ, e.gridZ);
        }

        bool usedActiveCamera = false;
        const glm::vec3 camPos = resolveLocalViewCameraPos(usedActiveCamera);
        std::cout << "[Nav][Diag] Bake camera: source=" << (usedActiveCamera ? "active-scene-camera" : "editor-viewport")
                  << " pos=(" << camPos.x << "," << camPos.y << "," << camPos.z << ")" << std::endl;
        std::cout << "[Nav][Diag] Target chunk window: x=[" << minChunkX << "," << maxChunkX << "]"
                  << " z=[" << minChunkZ << "," << maxChunkZ << "] size=" << chunkSize
                  << " selected=" << bakeIndices.size() << std::endl;
        std::cout << "[Nav][Diag] Target world bounds: min=(" << selectedBounds.min.x << "," << selectedBounds.min.y << "," << selectedBounds.min.z
                  << ") max=(" << selectedBounds.max.x << "," << selectedBounds.max.y << "," << selectedBounds.max.z << ")" << std::endl;

        std::cout << "[Nav][Diag] Sample target chunks:";
        const size_t sampleChunks = std::min<size_t>(bakeIndices.size(), 16);
        for (size_t i = 0; i < sampleChunks; ++i) {
            const auto& e = entries[bakeIndices[i]];
            std::cout << " (" << e.gridX << "," << e.gridZ << ")";
        }
        if (bakeIndices.size() > sampleChunks) std::cout << " ...";
        std::cout << std::endl;

        size_t includedMeshEntities = 0;
        size_t includedMeshTris = 0;
        size_t printedMeshes = 0;
        size_t consideredMeshes = 0;
        size_t regexFilteredMeshes = 0;
        const MeshNameRegexFilter meshNameFilter = BuildMeshNameRegexFilter(comp);
        if (scene) {
            for (const auto& ent : scene->GetEntities()) {
                const EntityID id = ent.GetID();
                const EntityData* d = scene->GetEntityData(id);
                if (!d || !d->Mesh || !d->Mesh->mesh) continue;
                const Mesh& m = *d->Mesh->mesh;
                if (m.Vertices.empty()) continue;
                ++consideredMeshes;
                if (!MeshNamePassesRegexFilter(meshNameFilter, d->Name)) {
                    ++regexFilteredMeshes;
                    continue;
                }

                Bounds meshBounds;
                meshBounds.min = glm::vec3(FLT_MAX);
                meshBounds.max = glm::vec3(-FLT_MAX);
                const glm::mat4& M = d->Transform.WorldMatrix;
                for (const glm::vec3& v : m.Vertices) {
                    meshBounds.expand(glm::vec3(M * glm::vec4(v, 1.0f)));
                }
                if (!BoundsOverlapXZ(meshBounds, selectedBounds)) continue;

                ++includedMeshEntities;
                const size_t triEstimate = !m.Indices.empty() ? (m.Indices.size() / 3) : (m.Vertices.size() / 3);
                includedMeshTris += triEstimate;
                if (printedMeshes < 20) {
                    std::cout << "[Nav][Diag] Mesh source: id=" << id
                              << " name=\"" << d->Name << "\" tris~" << triEstimate
                              << " boundsMin=(" << meshBounds.min.x << "," << meshBounds.min.y << "," << meshBounds.min.z << ")"
                              << " boundsMax=(" << meshBounds.max.x << "," << meshBounds.max.y << "," << meshBounds.max.z << ")"
                              << std::endl;
                    ++printedMeshes;
                }
            }
        }
        std::cout << "[Nav][Diag] Included mesh entities: " << includedMeshEntities
                  << " (tris~" << includedMeshTris << ")" << std::endl;
        if (meshNameFilter.enabled) {
            std::cout << "[Nav][Diag] Mesh regex filter: considered=" << consideredMeshes
                      << " filtered=" << regexFilteredMeshes
                      << " mode=include-only-matches"
                      << " pattern=\"" << meshNameFilter.pattern << "\""
                      << std::endl;
        }

        size_t terrainEntities = 0;
        size_t includedTerrainChunks = 0;
        size_t printedTerrainChunks = 0;
        if (scene) {
            for (const auto& ent : scene->GetEntities()) {
                const EntityID id = ent.GetID();
                const EntityData* d = scene->GetEntityData(id);
                if (!d || !d->Terrain) continue;
                ++terrainEntities;
                const TerrainComponent& t = *d->Terrain;
                if (t.UseChunkedTerrain && !t.Chunks.empty()) {
                    for (const auto& tc : t.Chunks) {
                        Bounds tb;
                        tb.min = glm::vec3(tc.WorldMin.x, tc.WorldMin.y, tc.WorldMin.z);
                        tb.max = glm::vec3(tc.WorldMax.x, tc.WorldMax.y, tc.WorldMax.z);
                        if (!BoundsOverlapXZ(tb, selectedBounds)) continue;
                        ++includedTerrainChunks;
                        if (printedTerrainChunks < 24) {
                            std::cout << "[Nav][Diag] Terrain chunk: entity=" << id
                                      << " name=\"" << d->Name << "\""
                                      << " grid=(" << tc.GridX << "," << tc.GridZ << ")"
                                      << " boundsMin=(" << tb.min.x << "," << tb.min.y << "," << tb.min.z << ")"
                                      << " boundsMax=(" << tb.max.x << "," << tb.max.y << "," << tb.max.z << ")"
                                      << std::endl;
                            ++printedTerrainChunks;
                        }
                    }
                } else {
                    std::cout << "[Nav][Diag] Terrain source (non-chunked): entity=" << id
                              << " name=\"" << d->Name << "\""
                              << " gridRes=" << t.GridResolution << std::endl;
                }
            }
        }
        std::cout << "[Nav][Diag] Terrain entities: " << terrainEntities
                  << ", overlapping terrain chunks: " << includedTerrainChunks << std::endl;
    }

    nav::io::NavPackMeta existingMeta{};
    std::vector<nav::io::NavPackChunk> existingChunks;
    bool hasExistingPack = false;
    if (std::filesystem::exists(packPath) && nav::io::LoadNavPackIndex(packPath.string(), existingMeta, existingChunks)) {
        hasExistingPack = existingMeta.chunksX == meta.chunksX && existingMeta.chunksZ == meta.chunksZ;
    }

    if (comp.BakeMissingChunksOnly && hasExistingPack) {
        std::vector<size_t> filtered;
        filtered.reserve(bakeIndices.size());
        for (size_t ci : bakeIndices) {
            if (ci < existingChunks.size() && existingChunks[ci].size > 0) continue;
            filtered.push_back(ci);
        }
        bakeIndices.swap(filtered);
        std::cout << "[Nav] Missing-chunk filter: " << bakeIndices.size()
                  << " chunks still need bake." << std::endl;
    }

    std::sort(bakeIndices.begin(), bakeIndices.end());
    if (bakeIndices.empty()) {
        std::cout << "[Nav] Chunk bake skipped: no chunks selected for baking." << std::endl;
        return true;
    }

    const size_t targetChunks = bakeIndices.size();
    std::cout << "[Nav] Chunk bake target chunks: " << targetChunks << std::endl;

    std::atomic<size_t> nextWork{0};
    std::atomic<size_t> processedActive{0};
    std::atomic<size_t> bakedChunksAtomic{0};
    std::atomic<int> chunkDiagPrinted{0};
    std::mutex logMutex;
    std::vector<std::shared_ptr<NavMeshRuntime>> previewCandidates(totalChunks);

    const unsigned hw = std::thread::hardware_concurrency();
    size_t workerCount = hw == 0 ? 8u : (size_t)hw;
    workerCount = std::max<size_t>(1, std::min(workerCount, targetChunks));
    std::cout << "[Nav] Chunk bake workers: " << workerCount << std::endl;

    auto bakeChunkByIndex = [&](size_t ci) {
        const uint32_t z = (uint32_t)(ci / (size_t)chunksX);
        const uint32_t x = (uint32_t)(ci % (size_t)chunksX);
        nav::io::NavPackChunk& entry = entries[ci];

        auto reportProgress = [&]() {
            const size_t p = processedActive.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((p % 256) == 0 || p == targetChunks) {
                std::lock_guard<std::mutex> lk(logMutex);
                std::cout << "[Nav] Chunk bake progress: " << p << "/" << targetChunks
                          << " (baked=" << bakedChunksAtomic.load(std::memory_order_relaxed) << ")" << std::endl;
            }
        };

        Bounds query = entry.bounds;
        query.min.x -= pad; query.min.z -= pad;
        query.max.x += pad; query.max.z += pad;

        std::unordered_map<uint32_t, uint32_t> remap;
        remap.reserve(4096);
        std::vector<glm::vec3> cVerts;
        std::vector<uint32_t> cTris;
        std::vector<uint16_t> cAreas;
        Bounds cBounds;
        cBounds.min = glm::vec3(FLT_MAX);
        cBounds.max = glm::vec3(-FLT_MAX);

        const int gatherMinZ = std::max(0, (int)z - padChunks);
        const int gatherMaxZ = std::min((int)chunksZ - 1, (int)z + padChunks);
        const int gatherMinX = std::max(0, (int)x - padChunks);
        const int gatherMaxX = std::min((int)chunksX - 1, (int)x + padChunks);

        size_t expectedCandidates = 0;
        for (int zz = gatherMinZ; zz <= gatherMaxZ; ++zz) {
            for (int xx = gatherMinX; xx <= gatherMaxX; ++xx) {
                const size_t bi = (size_t)zz * (size_t)chunksX + (size_t)xx;
                expectedCandidates += triBins[bi].size();
            }
        }

        std::unordered_set<uint32_t> seen;
        const size_t reserveHint = std::min<size_t>(expectedCandidates, 1u << 20);
        seen.reserve(reserveHint);
        std::vector<uint32_t> candidates;
        candidates.reserve(reserveHint);
        for (int zz = gatherMinZ; zz <= gatherMaxZ; ++zz) {
            for (int xx = gatherMinX; xx <= gatherMaxX; ++xx) {
                const size_t bi = (size_t)zz * (size_t)chunksX + (size_t)xx;
                for (uint32_t t : triBins[bi]) {
                    if (seen.insert(t).second) {
                        candidates.push_back(t);
                    }
                }
            }
        }

        for (uint32_t t : candidates) {
            const uint32_t gi0 = tris[t * 3 + 0];
            const uint32_t gi1 = tris[t * 3 + 1];
            const uint32_t gi2 = tris[t * 3 + 2];
            if (gi0 >= verts.size() || gi1 >= verts.size() || gi2 >= verts.size()) continue;
            const glm::vec3& a = verts[gi0];
            const glm::vec3& b = verts[gi1];
            const glm::vec3& c = verts[gi2];
            if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(a.z) ||
                !std::isfinite(b.x) || !std::isfinite(b.y) || !std::isfinite(b.z) ||
                !std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z)) {
                continue;
            }
            if (!TriIntersectsAabb(a, b, c, query)) continue;
            const glm::vec3 n = glm::cross(b - a, c - a);
            if (glm::dot(n, n) < 1e-12f) continue;

            auto mapIndex = [&](uint32_t gi) -> uint32_t {
                auto it = remap.find(gi);
                if (it != remap.end()) return it->second;
                const uint32_t li = (uint32_t)cVerts.size();
                remap.emplace(gi, li);
                cVerts.push_back(verts[gi]);
                cBounds.expand(verts[gi]);
                return li;
            };

            cTris.push_back(mapIndex(gi0));
            cTris.push_back(mapIndex(gi1));
            cTris.push_back(mapIndex(gi2));
            cAreas.push_back(t < areas.size() ? areas[t] : 1);
        }

        if (cTris.empty()) {
            entry.hash = 0;
            entry.size = 0;
            reportProgress();
            return;
        }

        const bool chunkHasMesh = std::any_of(
            cAreas.begin(), cAreas.end(),
            [](const uint16_t area) { return area == kMeshAreaId; });

        const int diagSlot = chunkDiagPrinted.fetch_add(1, std::memory_order_relaxed);
        if (diagSlot < 24) {
            std::lock_guard<std::mutex> lk(logMutex);
            std::cout << "[Nav][Diag] Chunk input: (" << x << "," << z << ")"
                      << " gatherRangeX=[" << gatherMinX << "," << gatherMaxX << "]"
                      << " gatherRangeZ=[" << gatherMinZ << "," << gatherMaxZ << "]"
                      << " candidateTris=" << candidates.size()
                      << " acceptedTris=" << (cTris.size() / 3)
                      << " verts=" << cVerts.size()
                      << " mesh=" << (chunkHasMesh ? "yes" : "terrain-only")
                      << std::endl;
        }

        NavBakeSettings chunkBake = comp.Bake;
        // Keep chunked bake conservative/stable for very large worlds.
        chunkBake.vertsPerPoly = 3.0f;
        chunkBake.detailSampleDist = 0.0f;
        if (!chunkHasMesh) {
            // Use a coarser profile for terrain-only chunks to keep sparse large worlds tractable.
            const float coarseCs = std::max(comp.Bake.cellSize * 4.0f, 0.75f);
            chunkBake.cellSize = coarseCs;
            chunkBake.cellHeight = std::max(comp.Bake.cellHeight, coarseCs * 0.25f);
            chunkBake.detailSampleMaxError = std::max(comp.Bake.detailSampleMaxError, 2.0f);
            chunkBake.regionMinSize = std::max(comp.Bake.regionMinSize, 4.0f);
            chunkBake.regionMergeSize = std::max(comp.Bake.regionMergeSize, 64.0f);
        }

        std::vector<uint8_t> navData;
        if (!BuildDetourData(cVerts, cTris, cAreas, cBounds, chunkBake, navData, false, false, true, (int)x, (int)z)) {
            {
                std::lock_guard<std::mutex> lk(logMutex);
                std::cerr << "[Nav] Chunk bake failed at (" << x << "," << z << ")" << std::endl;
            }
            entry.hash = 0;
            entry.size = 0;
            reportProgress();
            return;
        }

        auto rt = std::make_shared<NavMeshRuntime>();
        rt->m_AreaCost = areaCosts;
        if (!rt->SetDetourNavData(navData)) {
            {
                std::lock_guard<std::mutex> lk(logMutex);
                std::cerr << "[Nav] Chunk runtime init failed at (" << x << "," << z << ")" << std::endl;
            }
            entry.hash = 0;
            entry.size = 0;
            reportProgress();
            return;
        }
        rt->m_Bounds = cBounds;
        if (scene) ApplyPerPolyCostsFromTerrain(scene, *rt);

        std::vector<uint8_t> payload;
        if (!nav::io::BuildNavPayload(*rt, bakeHash, payload)) {
            {
                std::lock_guard<std::mutex> lk(logMutex);
                std::cerr << "[Nav] Chunk payload build failed at (" << x << "," << z << ")" << std::endl;
            }
            entry.hash = 0;
            entry.size = 0;
            reportProgress();
            return;
        }

        entry.hash = bakeHash ^ (((uint64_t)x << 32) | (uint64_t)z);
        entry.size = payload.size();
        payloads[ci] = std::move(payload);
        bakedChunksAtomic.fetch_add(1, std::memory_order_relaxed);
        previewCandidates[ci] = rt;

        reportProgress();
    };

    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (size_t i = 0; i < workerCount; ++i) {
        workers.emplace_back([&]() {
            for (;;) {
                const size_t wi = nextWork.fetch_add(1, std::memory_order_relaxed);
                if (wi >= targetChunks) break;
                bakeChunkByIndex(bakeIndices[wi]);
            }
        });
    }
    for (std::thread& t : workers) {
        if (t.joinable()) t.join();
    }

    const size_t bakedChunks = bakedChunksAtomic.load(std::memory_order_relaxed);
    for (size_t ci : bakeIndices) {
        if (previewCandidates[ci]) {
            previewRuntime = previewCandidates[ci];
            break;
        }
    }

    if (bakedChunks == 0) {
        std::cerr << "[Nav] Chunked bake produced zero valid chunks." << std::endl;
        return false;
    }

    std::filesystem::create_directories(packPath.parent_path());
    const bool incrementalWrite = comp.BakeVisibleChunksOnly || comp.BakeMissingChunksOnly;
    if (incrementalWrite) {
        if (!hasExistingPack) {
            std::vector<std::vector<uint8_t>> emptyPayloads(totalChunks);
            if (!nav::io::WriteNavPack(packPath.string(), meta, entries, emptyPayloads)) {
                std::cerr << "[Nav] Failed to initialize navpack for incremental bake: " << packPath.string() << std::endl;
                return false;
            }
        }

        std::vector<nav::io::NavPackChunk> updateEntries;
        std::vector<std::vector<uint8_t>> updatePayloads;
        updateEntries.reserve(bakeIndices.size());
        updatePayloads.reserve(bakeIndices.size());
        for (size_t ci : bakeIndices) {
            if (payloads[ci].empty()) continue;
            updateEntries.push_back(entries[ci]);
            updatePayloads.push_back(std::move(payloads[ci]));
        }
        if (!updateEntries.empty()) {
            if (!nav::io::UpsertNavPackChunks(packPath.string(), meta, updateEntries, updatePayloads)) {
                std::cerr << "[Nav] Failed to upsert navpack chunk batch." << std::endl;
                return false;
            }
        }
        const size_t written = updateEntries.size();
        std::cout << "[Nav] Chunked navpack updated: " << written << " chunks (target=" << targetChunks
                  << ", active=" << activeChunks << ", total=" << totalChunks << ")" << std::endl;
    } else {
        if (!nav::io::WriteNavPack(packPath.string(), meta, entries, payloads)) {
            std::cerr << "[Nav] Failed to write navpack: " << packPath.string() << std::endl;
            return false;
        }
        std::cout << "[Nav] Chunked navpack written: " << bakedChunks << "/" << totalChunks << " chunks" << std::endl;
    }
    return true;
}

static void GatherMeshGeometry(Scene& scene, const NavMeshComponent& comp,
                               std::vector<glm::vec3>& outVerts,
                               std::vector<uint32_t>& outTris,
                               std::vector<uint16_t>& outAreas,
                               Bounds& outBounds)
{
    const MeshNameRegexFilter meshNameFilter = BuildMeshNameRegexFilter(comp);
    size_t consideredMeshes = 0;
    size_t filteredMeshes = 0;

    for (const auto& e : scene.GetEntities()) {
        const EntityID id = e.GetID();
        const EntityData* d = scene.GetEntityData(id);
        if (!d || !d->Mesh || !d->Mesh->mesh) continue;
        const Mesh& m = *d->Mesh->mesh;
        if (m.Vertices.empty()) continue;
        ++consideredMeshes;
        if (!MeshNamePassesRegexFilter(meshNameFilter, d->Name)) {
            ++filteredMeshes;
            continue;
        }

        const glm::mat4& M = d->Transform.WorldMatrix;
        const bool flipWinding = glm::determinant(glm::mat3(M)) < 0.0f;
        const uint32_t base = (uint32_t)outVerts.size();
        outVerts.reserve(outVerts.size() + m.Vertices.size());
        for (const glm::vec3& v : m.Vertices) {
            const glm::vec3 w = glm::vec3(M * glm::vec4(v, 1.0f));
            outVerts.push_back(w);
            outBounds.expand(w);
        }

        if (!m.Indices.empty()) {
            for (size_t i = 0; i + 2 < m.Indices.size(); i += 3) {
                const uint32_t i0 = m.Indices[i + 0];
                const uint32_t i1 = m.Indices[i + 1];
                const uint32_t i2 = m.Indices[i + 2];
                if (i0 >= m.Vertices.size() || i1 >= m.Vertices.size() || i2 >= m.Vertices.size()) continue;
                outTris.push_back(base + i0);
                outTris.push_back(base + (flipWinding ? i2 : i1));
                outTris.push_back(base + (flipWinding ? i1 : i2));
                outAreas.push_back(kMeshAreaId);
            }
        } else {
            for (size_t i = 0; i + 2 < m.Vertices.size(); i += 3) {
                outTris.push_back(base + (uint32_t)i + 0);
                outTris.push_back(base + (flipWinding ? (uint32_t)i + 2 : (uint32_t)i + 1));
                outTris.push_back(base + (flipWinding ? (uint32_t)i + 1 : (uint32_t)i + 2));
                outAreas.push_back(kMeshAreaId);
            }
        }
    }

    if (meshNameFilter.enabled) {
        const size_t keptMeshes = consideredMeshes >= filteredMeshes ? (consideredMeshes - filteredMeshes) : 0;
        std::cout << "[Nav][Diag] Mesh regex filter: considered=" << consideredMeshes
                  << " kept=" << keptMeshes
                  << " filtered=" << filteredMeshes
                  << " mode=include-only-matches"
                  << " pattern=\"" << meshNameFilter.pattern << "\""
                  << std::endl;
    }
}

static void GatherTerrainGeometry(Scene& scene, const NavMeshComponent& comp,
                                  std::vector<glm::vec3>& outVerts,
                                  std::vector<uint32_t>& outTris,
                                  std::vector<uint16_t>& outAreas,
                                  std::array<float, 64>& areaCosts,
                                  Bounds& outBounds)
{
    const uint32_t step = std::max(1u, comp.TerrainSampleStep);
    for (const auto& e : scene.GetEntities()) {
        const EntityID id = e.GetID();
        const EntityData* d = scene.GetEntityData(id);
        if (!d || !d->Terrain) continue;
        const TerrainComponent& t = *d->Terrain;
        if (t.GridResolution < 2 || t.HeightMap.empty()) continue;

        const glm::mat4& M = d->Transform.WorldMatrix;
        const glm::vec2 cell = Terrain::GetCellSize(t);
        if (cell.x <= 0.0f || cell.y <= 0.0f) continue;

        auto sample = [&](uint32_t x, uint32_t z) -> glm::vec3 {
            x = std::min(x, t.GridResolution - 1);
            z = std::min(z, t.GridResolution - 1);
            const size_t idx = (size_t)z * t.GridResolution + x;
            const float y = idx < t.HeightMap.size() ? ((float)t.HeightMap[idx] / 65535.0f) * t.MaxHeight : 0.0f;
            return glm::vec3(M * glm::vec4((float)x * cell.x, y, (float)z * cell.y, 1.0f));
        };
        auto sampleHole = [&](uint32_t x, uint32_t z) -> bool {
            x = std::min(x, t.GridResolution - 1);
            z = std::min(z, t.GridResolution - 1);
            return Terrain::IsHoleAtLocal(t, (float)x * cell.x, (float)z * cell.y);
        };

        auto sampleArea = [&](uint32_t x, uint32_t z) -> uint16_t {
            if (t.SplatMap.empty()) return 1;
            const size_t idx = (size_t)std::min(z, t.GridResolution - 1) * t.GridResolution + std::min(x, t.GridResolution - 1);
            if (idx >= t.SplatMap.size()) return 1;
            const glm::u8vec4 w = t.SplatMap[idx];
            uint8_t best = w.r;
            int layer = 0;
            if (w.g > best) { best = w.g; layer = 1; }
            if (w.b > best) { best = w.b; layer = 2; }
            if (w.a > best) { best = w.a; layer = 3; }
            layer = std::clamp(layer, 0, 61); // +1 below keeps terrain in [1,62], reserving 63 for meshes.
            if (!t.Layers.empty()) {
                const int li = std::min(layer, (int)t.Layers.size() - 1);
                areaCosts[(size_t)layer + 1] = std::max(0.01f, t.Layers[(size_t)li].NavCost);
            }
            return (uint16_t)(layer + 1);
        };

        for (uint32_t z = 0; z + step < t.GridResolution; z += step) {
            for (uint32_t x = 0; x + step < t.GridResolution; x += step) {
                const glm::vec3 p00 = sample(x, z);
                const glm::vec3 p10 = sample(x + step, z);
                const glm::vec3 p01 = sample(x, z + step);
                const glm::vec3 p11 = sample(x + step, z + step);
                const uint16_t area = sampleArea(x, z);
                const bool h00 = sampleHole(x, z);
                const bool h10 = sampleHole(x + step, z);
                const bool h01 = sampleHole(x, z + step);
                const bool h11 = sampleHole(x + step, z + step);

                // Use winding that produces upward normals in Y-up space.
                if (!h00 && !h01 && !h10) {
                    const uint32_t base0 = (uint32_t)outVerts.size();
                    outVerts.push_back(p00); outVerts.push_back(p01); outVerts.push_back(p10);
                    outTris.push_back(base0 + 0); outTris.push_back(base0 + 1); outTris.push_back(base0 + 2);
                    outAreas.push_back(area);
                    outBounds.expand(p00); outBounds.expand(p01); outBounds.expand(p10);
                }

                if (!h10 && !h01 && !h11) {
                    const uint32_t base1 = (uint32_t)outVerts.size();
                    outVerts.push_back(p10); outVerts.push_back(p01); outVerts.push_back(p11);
                    outTris.push_back(base1 + 0); outTris.push_back(base1 + 1); outTris.push_back(base1 + 2);
                    outAreas.push_back(area);
                    outBounds.expand(p10); outBounds.expand(p01); outBounds.expand(p11);
                }
            }
        }
    }
}

// Option 3: Per-poly costs from terrain splatmap. Samples at centroid + 3 vertices so
// large polys spanning multiple layers get a cost that reflects their composition.
// Polys not on terrain keep cost 1.0f. Used by NavPathfinder and PCST chunk.
static void ApplyPerPolyCostsFromTerrain(Scene* scene, NavMeshRuntime& rt)
{
    if (!scene || rt.m_Polys.empty() || rt.m_Vertices.empty()) return;

    for (auto& poly : rt.m_Polys) {
        if (poly.i0 >= rt.m_Vertices.size() || poly.i1 >= rt.m_Vertices.size() || poly.i2 >= rt.m_Vertices.size()) {
            poly.cost = 1.0f;
            continue;
        }
        const glm::vec3& v0 = rt.m_Vertices[poly.i0];
        const glm::vec3& v1 = rt.m_Vertices[poly.i1];
        const glm::vec3& v2 = rt.m_Vertices[poly.i2];
        const glm::vec3 centroid = (v0 + v1 + v2) / 3.0f;

        // Find terrain containing centroid; use same terrain for all samples (poly is on one terrain)
        const TerrainComponent* terrain = nullptr;
        const EntityData* terrainEntity = nullptr;
        for (const auto& e : scene->GetEntities()) {
            const EntityData* d = scene->GetEntityData(e.GetID());
            if (!d || !d->Terrain) continue;
            const TerrainComponent& t = *d->Terrain;
            if (t.GridResolution < 2 || t.SplatMap.empty() || t.Layers.empty()) continue;

            const glm::vec4 local4 = glm::inverse(d->Transform.WorldMatrix) * glm::vec4(centroid, 1.0f);
            if (local4.x >= 0.0f && local4.x <= t.WorldSize.x && local4.z >= 0.0f && local4.z <= t.WorldSize.y) {
                terrain = &t;
                terrainEntity = d;
                break;
            }
        }
        if (!terrain || !terrainEntity) {
            poly.cost = 1.0f;
            continue;
        }

        const glm::mat4 invM = glm::inverse(terrainEntity->Transform.WorldMatrix);
        const glm::vec2 cell = Terrain::GetCellSize(*terrain);
        if (cell.x <= 0.0f || cell.y <= 0.0f) {
            poly.cost = 1.0f;
            continue;
        }

        const float maxCoord = static_cast<float>(terrain->GridResolution - 1);
        auto sampleLayer = [&](const glm::vec3& worldPos) -> int {
            const glm::vec4 local4 = invM * glm::vec4(worldPos, 1.0f);
            const float lx = local4.x, lz = local4.z;
            if (lx < 0.0f || lx > terrain->WorldSize.x || lz < 0.0f || lz > terrain->WorldSize.y)
                return -1;
            if (Terrain::IsHoleAtLocal(*terrain, lx, lz))
                return -1;
            const uint32_t gx = static_cast<uint32_t>(std::clamp(std::floor(lx / cell.x), 0.0f, maxCoord));
            const uint32_t gz = static_cast<uint32_t>(std::clamp(std::floor(lz / cell.y), 0.0f, maxCoord));
            const size_t idx = (size_t)gz * terrain->GridResolution + gx;
            if (idx >= terrain->SplatMap.size()) return -1;
            const glm::u8vec4 w = terrain->SplatMap[idx];
            uint8_t best = w.r;
            int layer = 0;
            if (w.g > best) { best = w.g; layer = 1; }
            if (w.b > best) { best = w.b; layer = 2; }
            if (w.a > best) { best = w.a; layer = 3; }
            return layer;
        };

        // Sample 4 points: centroid + 3 vertices. Count which layer each hits.
        int layerCount[4] = {0, 0, 0, 0};
        const glm::vec3 samples[4] = {centroid, v0, v1, v2};
        for (int i = 0; i < 4; ++i) {
            const int layer = sampleLayer(samples[i]);
            if (layer >= 0) layerCount[layer]++;
        }

        int bestLayer = 0;
        int bestCount = 0;
        for (int i = 0; i < 4; ++i) {
            if (layerCount[i] > bestCount) {
                bestCount = layerCount[i];
                bestLayer = i;
            }
        }
        if (bestCount == 0) {
            poly.cost = 1.0f;
            continue;
        }

        const int li = std::clamp(bestLayer, 0, (int)terrain->Layers.size() - 1);
        poly.cost = std::max(0.01f, terrain->Layers[(size_t)li].NavCost);
    }
}

static bool BuildDetourData(const std::vector<glm::vec3>& verts,
                            const std::vector<uint32_t>& tris,
                            const std::vector<uint16_t>& areas,
                            const Bounds& bounds,
                            const NavBakeSettings& bake,
                            std::vector<uint8_t>& outNavData,
                            bool logGrid,
                            bool buildDetail,
                            bool asDetourTile,
                            int tileX,
                            int tileY)
{
    if (verts.empty() || tris.empty() || (tris.size() % 3) != 0) {
        std::cerr << "[Nav] Recast: invalid geometry buffers (verts=" << verts.size()
                  << ", trisIdx=" << tris.size() << ")." << std::endl;
        return false;
    }

    RecastCtx ctx(true);
    auto fail = [&](const char* stage) {
        std::cerr << "[Nav] Recast failed at stage: " << stage << std::endl;
        return false;
    };
    const int nverts = (int)verts.size();
    const int ntris = (int)(tris.size() / 3);

    std::vector<float> rcVerts((size_t)nverts * 3);
    std::vector<int> rcTris((size_t)ntris * 3);
    for (int i = 0; i < nverts; ++i) {
        rcVerts[(size_t)i * 3 + 0] = verts[(size_t)i].x;
        rcVerts[(size_t)i * 3 + 1] = verts[(size_t)i].y;
        rcVerts[(size_t)i * 3 + 2] = verts[(size_t)i].z;
    }
    for (int i = 0; i < ntris; ++i) {
        rcTris[(size_t)i * 3 + 0] = (int)tris[(size_t)i * 3 + 0];
        rcTris[(size_t)i * 3 + 1] = (int)tris[(size_t)i * 3 + 1];
        rcTris[(size_t)i * 3 + 2] = (int)tris[(size_t)i * 3 + 2];
    }

    std::vector<unsigned char> triAreas((size_t)ntris, 0);
    rcMarkWalkableTriangles(&ctx, bake.agentMaxSlopeDeg, rcVerts.data(), nverts, rcTris.data(), ntris, triAreas.data());
    int walkableCount = 0;
    for (int i = 0; i < ntris; ++i) {
        if (triAreas[(size_t)i] != RC_NULL_AREA) {
            ++walkableCount;
        }
    }
    if (areas.size() == (size_t)ntris) {
        for (int i = 0; i < ntris; ++i) {
            if (triAreas[(size_t)i] != RC_NULL_AREA) {
                const uint16_t srcArea = areas[(size_t)i];
                if (srcArea != 0) {
                    triAreas[(size_t)i] = (unsigned char)std::clamp<uint16_t>(srcArea, 1, 63);
                }
            }
        }
    }
    if (walkableCount == 0) {
        const float cosSlope = std::cos(glm::radians(std::clamp(bake.agentMaxSlopeDeg, 0.0f, 89.0f)));
        std::cerr << "[Nav] Recast: 0 walkable triangles after slope filtering."
                  << " tris=" << ntris
                  << " slopeDeg=" << bake.agentMaxSlopeDeg
                  << " cosSlope=" << cosSlope
                  << " boundsMin=(" << bounds.min.x << "," << bounds.min.y << "," << bounds.min.z << ")"
                  << " boundsMax=(" << bounds.max.x << "," << bounds.max.y << "," << bounds.max.z << ")"
                  << std::endl;
        return false;
    }

    rcConfig cfg{};
    const float baseCs = std::max(0.02f, bake.cellSize);
    const float baseCh = std::max(0.02f, bake.cellHeight);
    cfg.cs = baseCs;
    cfg.ch = baseCh;
    cfg.walkableSlopeAngle = std::clamp(bake.agentMaxSlopeDeg, 0.0f, 89.0f);
    cfg.walkableHeight = std::max(1, (int)std::ceil(bake.agentHeight / cfg.ch));
    cfg.walkableClimb = std::max(0, (int)std::floor(bake.agentMaxClimb / cfg.ch));
    cfg.maxSimplificationError = std::max(0.1f, bake.edgeMaxError);
    cfg.minRegionArea = std::max(0, (int)rcSqr(bake.regionMinSize));
    cfg.mergeRegionArea = std::max(cfg.minRegionArea, (int)rcSqr(bake.regionMergeSize));
    cfg.maxVertsPerPoly = std::clamp((int)std::round(bake.vertsPerPoly), 3, (int)DT_VERTS_PER_POLYGON);
    cfg.detailSampleMaxError = cfg.ch * std::max(0.0f, bake.detailSampleMaxError);
    rcVcopy(cfg.bmin, &bounds.min.x);
    rcVcopy(cfg.bmax, &bounds.max.x);
    auto recalcGridForCellSize = [&](float cs) {
        cfg.cs = std::max(0.02f, cs);

        // If we upscale cell size to fit huge worlds, also coarsen height quantization.
        // Keeping ultra-fine ch with huge cs over-filters ledges and often yields 0 polys.
        if (cfg.cs > baseCs * 2.0f) {
            cfg.ch = std::max(baseCh, cfg.cs * 0.25f);
        } else {
            cfg.ch = baseCh;
        }

        cfg.walkableHeight = std::max(1, (int)std::ceil(bake.agentHeight / cfg.ch));
        cfg.walkableClimb = std::max(0, (int)std::floor(bake.agentMaxClimb / cfg.ch));
        cfg.walkableRadius = std::max(0, (int)std::ceil(bake.agentRadius / cfg.cs));
        if (cfg.cs > bake.agentRadius * 4.0f) {
            cfg.walkableRadius = 0;
        }
        cfg.maxEdgeLen = std::max(0, (int)std::round(bake.edgeMaxLen / cfg.cs));
        cfg.detailSampleDist = bake.detailSampleDist < 0.9f ? 0.0f : cfg.cs * bake.detailSampleDist;
        cfg.detailSampleMaxError = cfg.ch * std::max(0.0f, bake.detailSampleMaxError);
        rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);
    };
    recalcGridForCellSize(cfg.cs);
    if (cfg.width <= 0 || cfg.height <= 0) {
        std::cerr << "[Nav] Recast: invalid grid size width=" << cfg.width << " height=" << cfg.height << std::endl;
        return false;
    }

    // Guard against runaway heightfield allocation on very large worlds.
    static constexpr int64_t kMaxGridCells = 12000000; // ~12M cells
    int64_t gridCells = (int64_t)cfg.width * (int64_t)cfg.height;
    int upscalePass = 0;
    while (gridCells > kMaxGridCells && upscalePass < 8) {
        recalcGridForCellSize(cfg.cs * 1.5f);
        gridCells = (int64_t)cfg.width * (int64_t)cfg.height;
        ++upscalePass;
    }
    if (gridCells > kMaxGridCells) {
        std::cerr << "[Nav] Recast: world too large for single-tile bake. grid="
                  << cfg.width << "x" << cfg.height << " (" << gridCells
                  << " cells) at cs=" << cfg.cs << std::endl;
        return false;
    }
    if (logGrid) {
        std::cout << "[Nav] Recast grid: " << cfg.width << "x" << cfg.height
                  << " (" << gridCells << " cells), cs=" << cfg.cs
                  << ", ch=" << cfg.ch
                  << ", walkHeight=" << cfg.walkableHeight
                  << ", walkClimb=" << cfg.walkableClimb
                  << ", walkRadius=" << cfg.walkableRadius
                  << std::endl;
    }

    rcHeightfield* hf = rcAllocHeightfield();
    rcCompactHeightfield* chf = rcAllocCompactHeightfield();
    rcContourSet* cset = rcAllocContourSet();
    rcPolyMesh* pmesh = rcAllocPolyMesh();
    rcPolyMeshDetail* dmesh = buildDetail ? rcAllocPolyMeshDetail() : nullptr;
    unsigned char* navData = nullptr;
    int navDataSize = 0;

    auto cleanup = [&]() {
        if (dmesh) rcFreePolyMeshDetail(dmesh);
        if (pmesh) rcFreePolyMesh(pmesh);
        if (cset) rcFreeContourSet(cset);
        if (chf) rcFreeCompactHeightfield(chf);
        if (hf) rcFreeHeightField(hf);
        if (navData) dtFree(navData);
    };

    if (!hf || !chf || !cset || !pmesh || (buildDetail && !dmesh)) { cleanup(); return fail("alloc objects"); }
    bool createdHeightfield = rcCreateHeightfield(&ctx, *hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch);
    int createRetry = 0;
    while (!createdHeightfield && createRetry < 4) {
        recalcGridForCellSize(cfg.cs * 1.5f);
        gridCells = (int64_t)cfg.width * (int64_t)cfg.height;
        std::cerr << "[Nav] Recast: retrying create heightfield with cs=" << cfg.cs
                  << " grid=" << cfg.width << "x" << cfg.height << std::endl;
        createdHeightfield = rcCreateHeightfield(&ctx, *hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch);
        ++createRetry;
    }
    if (!createdHeightfield) { cleanup(); return fail("create heightfield"); }
    if (!rcRasterizeTriangles(&ctx, rcVerts.data(), nverts, rcTris.data(), triAreas.data(), ntris, *hf, cfg.walkableClimb)) { cleanup(); return fail("rasterize"); }
    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *hf);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *hf);
    if (!rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf, *chf)) { cleanup(); return fail("build compact heightfield"); }
    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf)) { cleanup(); return fail("erode walkable area"); }
    if (!rcBuildDistanceField(&ctx, *chf)) { cleanup(); return fail("build distance field"); }
    if (!rcBuildRegions(&ctx, *chf, 0, cfg.minRegionArea, cfg.mergeRegionArea)) { cleanup(); return fail("build regions"); }
    if (!rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset, RC_CONTOUR_TESS_WALL_EDGES)) { cleanup(); return fail("build contours"); }
    if (!rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh)) { cleanup(); return fail("build polymesh"); }
    if (pmesh->npolys <= 0) {
        // Interior/fine scenes can be over-pruned by erosion/region thresholds.
        // Retry once with a relaxed profile aligned with common Recast demo troubleshooting.
        std::cerr << "[Nav] Recast: no polygons with primary config, retrying relaxed build."
                  << " walkRadius=" << cfg.walkableRadius
                  << " minRegionArea=" << cfg.minRegionArea
                  << " mergeRegionArea=" << cfg.mergeRegionArea
                  << " walkHeight=" << cfg.walkableHeight
                  << std::endl;

        cleanup();

        hf = rcAllocHeightfield();
        chf = rcAllocCompactHeightfield();
        cset = rcAllocContourSet();
        pmesh = rcAllocPolyMesh();
        dmesh = buildDetail ? rcAllocPolyMeshDetail() : nullptr;
        if (!hf || !chf || !cset || !pmesh || (buildDetail && !dmesh)) { cleanup(); return fail("alloc objects (relaxed)"); }

        const int relaxedWalkHeight = std::max(1, cfg.walkableHeight / 2);
        const int relaxedWalkRadius = 0;
        const int relaxedMinRegion = 0;
        const int relaxedMergeRegion = 0;

        if (!rcCreateHeightfield(&ctx, *hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) { cleanup(); return fail("create heightfield (relaxed)"); }
        if (!rcRasterizeTriangles(&ctx, rcVerts.data(), nverts, rcTris.data(), triAreas.data(), ntris, *hf, cfg.walkableClimb)) { cleanup(); return fail("rasterize (relaxed)"); }
        rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *hf);
        rcFilterLedgeSpans(&ctx, relaxedWalkHeight, cfg.walkableClimb, *hf);
        rcFilterWalkableLowHeightSpans(&ctx, relaxedWalkHeight, *hf);
        if (!rcBuildCompactHeightfield(&ctx, relaxedWalkHeight, cfg.walkableClimb, *hf, *chf)) { cleanup(); return fail("build compact heightfield (relaxed)"); }
        if (!rcErodeWalkableArea(&ctx, relaxedWalkRadius, *chf)) { cleanup(); return fail("erode walkable area (relaxed)"); }
        if (!rcBuildDistanceField(&ctx, *chf)) { cleanup(); return fail("build distance field (relaxed)"); }
        if (!rcBuildRegions(&ctx, *chf, 0, relaxedMinRegion, relaxedMergeRegion)) { cleanup(); return fail("build regions (relaxed)"); }
        if (!rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset, RC_CONTOUR_TESS_WALL_EDGES)) { cleanup(); return fail("build contours (relaxed)"); }
        if (!rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh)) { cleanup(); return fail("build polymesh (relaxed)"); }
        if (pmesh->npolys <= 0) { cleanup(); return fail("no output polygons"); }

        std::cerr << "[Nav] Recast: relaxed profile succeeded."
                  << " walkHeight=" << relaxedWalkHeight
                  << " walkRadius=" << relaxedWalkRadius
                  << " minRegionArea=" << relaxedMinRegion
                  << std::endl;
    }

    if (!ValidatePolyMeshTopology(*pmesh, "final")) { cleanup(); return fail("invalid polymesh topology"); }
    if (buildDetail) {
        if (!dmesh) { cleanup(); return fail("alloc detail mesh"); }
        if (!SafeBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh)) {
            cleanup();
            return fail("build polymesh detail");
        }
    }

    std::vector<unsigned short> polyFlags((size_t)pmesh->npolys, 1);
    dtNavMeshCreateParams params{};
    params.verts = pmesh->verts;
    params.vertCount = pmesh->nverts;
    params.polys = pmesh->polys;
    params.polyAreas = pmesh->areas;
    params.polyFlags = polyFlags.data();
    params.polyCount = pmesh->npolys;
    params.nvp = pmesh->nvp;
    params.detailMeshes = buildDetail ? dmesh->meshes : nullptr;
    params.detailVerts = buildDetail ? dmesh->verts : nullptr;
    params.detailVertsCount = buildDetail ? dmesh->nverts : 0;
    params.detailTris = buildDetail ? dmesh->tris : nullptr;
    params.detailTriCount = buildDetail ? dmesh->ntris : 0;
    params.walkableHeight = bake.agentHeight;
    params.walkableRadius = bake.agentRadius;
    params.walkableClimb = bake.agentMaxClimb;
    rcVcopy(params.bmin, pmesh->bmin);
    rcVcopy(params.bmax, pmesh->bmax);
    params.cs = cfg.cs;
    params.ch = cfg.ch;
    params.buildBvTree = true;
    if (asDetourTile) {
        params.tileX = tileX;
        params.tileY = tileY;
        params.tileLayer = 0;
        params.userId = (uint32_t)(((uint32_t)tileY << 16) ^ ((uint32_t)tileX & 0xFFFFu));
    }

    if (!dtCreateNavMeshData(&params, &navData, &navDataSize) || navDataSize <= 0) {
        cleanup();
        return fail("dtCreateNavMeshData");
    }

    outNavData.assign(navData, navData + navDataSize);
    cleanup();
    return true;
}
} // namespace

#ifndef CLAYMORE_RUNTIME
void nav::jobs::SubmitBake(NavMeshComponent* comp, Scene* scene)
{
    if (!comp || !scene) return;
    const std::filesystem::path projectRoot = FileSystem::Instance().GetProjectRoot();

    std::thread([comp, scene, projectRoot]() {
        try {
        comp->BakingStage.store(1);
        comp->BakingProgress.store(0.05f);

        std::vector<glm::vec3> verts;
        std::vector<uint32_t> tris;
        std::vector<uint16_t> areas;
        std::array<float, 64> areaCosts{};
        areaCosts.fill(1.0f);
        Bounds bounds;

        GatherMeshGeometry(*scene, *comp, verts, tris, areas, bounds);
        GatherTerrainGeometry(*scene, *comp, verts, tris, areas, areaCosts, bounds);
        std::cout << "[Nav] Recast sources: verts=" << verts.size()
                  << " tris=" << (tris.size() / 3) << std::endl;
        if (verts.empty() || tris.empty()) {
            std::cerr << "[Nav] Bake aborted: no terrain/model geometry found in scene."
                      << std::endl;
            comp->Baking.store(false);
            comp->BakingStage.store(0);
            return;
        }
        if (bounds.min.x > bounds.max.x || bounds.min.y > bounds.max.y || bounds.min.z > bounds.max.z) {
            std::cerr << "[Nav] Bake aborted: invalid source bounds." << std::endl;
            comp->Baking.store(false);
            comp->BakingStage.store(0);
            return;
        }

        if (comp->BakingCancel.load()) {
            comp->Baking.store(false);
            comp->BakingStage.store(0);
            return;
        }
        comp->BakingProgress.store(0.25f);

        comp->EnsureGuid();
        if (comp->AssetPath.empty()) {
            comp->AssetPath = NavMeshComponent::BuildDefaultAssetPath(comp->NavMeshDataGuid);
        }

        const uint64_t bakeHash = comp->ComputeBakeHash(*scene);
        comp->BakeHash = bakeHash;

        // Resolve path: convert machine-specific absolute paths to project-relative for portability
        std::filesystem::path navPath(comp->AssetPath);
        if (navPath.is_absolute() && !projectRoot.empty()) {
            std::error_code ec;
            auto rel = std::filesystem::relative(navPath, projectRoot, ec);
            // If path is outside project or parent doesn't exist (e.g. from another machine), use default
            if (ec || rel.empty() || rel.string().find("..") != std::string::npos ||
                !std::filesystem::exists(navPath.parent_path(), ec)) {
                comp->AssetPath = NavMeshComponent::BuildDefaultAssetPath(comp->NavMeshDataGuid);
                navPath = projectRoot / comp->AssetPath;
            } else {
                comp->AssetPath = rel.string();
                navPath = projectRoot / comp->AssetPath;
            }
        } else if (navPath.is_relative()) {
            navPath = projectRoot / navPath;
        }
        navPath = navPath.lexically_normal();
        std::error_code ec;
        std::filesystem::create_directories(navPath.parent_path(), ec);
        if (ec) {
            std::cerr << "[Nav] Bake failed: cannot create directory " << navPath.parent_path().string()
                      << ": " << ec.message() << std::endl;
            comp->Baking.store(false);
            comp->BakingStage.store(0);
            return;
        }

        // Professional large-world behavior: auto-switch to chunked navpack when single-tile grid explodes.
        const double estCells = EstimateGridCellsXZ(bounds, std::max(0.02f, comp->Bake.cellSize));
        const bool hugeWorld = estCells > 12000000.0;
        const bool useChunked = comp->ChunkedNavEnabled || hugeWorld;

        if (useChunked) {
            if (hugeWorld && !comp->ChunkedNavEnabled) {
                std::cout << "[Nav] Auto-enabling chunked nav bake due to large world (est cells="
                          << (uint64_t)estCells << ")." << std::endl;
                comp->ChunkedNavEnabled = true;
            }

            if (comp->NavPackPath.empty()) {
                std::filesystem::path pack = navPath;
                pack.replace_extension(".navpack");
                comp->NavPackPath = (!projectRoot.empty())
                    ? std::filesystem::relative(pack, projectRoot).string()
                    : pack.string();
            }
            std::filesystem::path packPath(comp->NavPackPath);
            if (packPath.is_absolute() && !projectRoot.empty()) {
                std::error_code ec;
                auto rel = std::filesystem::relative(packPath, projectRoot, ec);
                if (ec || rel.empty() || rel.string().find("..") != std::string::npos ||
                    !std::filesystem::exists(packPath.parent_path(), ec)) {
                    comp->NavPackPath = std::filesystem::path(comp->AssetPath).replace_extension(".navpack").string();
                    packPath = projectRoot / comp->NavPackPath;
                } else {
                    comp->NavPackPath = rel.string();
                    packPath = projectRoot / comp->NavPackPath;
                }
            } else if (packPath.is_relative()) {
                packPath = projectRoot / packPath;
            }
            packPath = packPath.lexically_normal();

            std::shared_ptr<NavMeshRuntime> previewRt;
            if (!BuildWorldGridChunkedNavPack(verts, tris, areas, bounds, scene, *comp, areaCosts, bakeHash, packPath, previewRt)) {
                std::cerr << "[Nav] Recast chunked build failed." << std::endl;
                comp->Baking.store(false);
                comp->BakingStage.store(0);
                return;
            }
            comp->BakingProgress.store(0.85f);

            // Chunked streaming runtime is refreshed from disk timestamp changes in main-thread update.
            // Do not reset shared runtime/manager pointers here from the bake worker thread.
            comp->AABB = bounds;

            comp->LoadAttempted = true;
            comp->AssetDirty = false;
            comp->LastBakeCostMin.store(1.0f);
            comp->LastBakeCostMax.store(1.0f);
            comp->LastBakeIslands.store(previewRt && !previewRt->m_Polys.empty() ? 1u : 0u);
            comp->LastBakeRemovedIslands.store(0u);
            comp->BakingProgress.store(1.0f);
            comp->BakingStage.store(0);
            comp->Baking.store(false);
            return;
        }

        std::vector<uint8_t> navData;
        if (!BuildDetourData(verts, tris, areas, bounds, comp->Bake, navData)) {
            std::cerr << "[Nav] Recast build failed." << std::endl;
            comp->Baking.store(false);
            comp->BakingStage.store(0);
            return;
        }

        auto rt = std::make_shared<NavMeshRuntime>();
        rt->m_AreaCost = areaCosts;
        if (!rt->SetDetourNavData(navData)) {
            std::cerr << "[Nav] Detour runtime init failed." << std::endl;
            comp->Baking.store(false);
            comp->BakingStage.store(0);
            return;
        }
        rt->m_Bounds = bounds;
        comp->AABB = bounds;
        ApplyPerPolyCostsFromTerrain(scene, *rt);
        comp->BakingProgress.store(0.75f);

        if (!nav::io::WriteNavbin(*rt, bakeHash, navPath.string())) {
            std::cerr << "[Nav] Failed to write navbin: " << navPath.string() << std::endl;
            comp->Baking.store(false);
            comp->BakingStage.store(0);
            return;
        }

        if (comp->ChunkedNavEnabled) {
            nav::io::NavPackMeta meta{};
            meta.chunksX = 1;
            meta.chunksZ = 1;
            meta.sceneGuidHigh = comp->NavMeshDataGuid.high;
            meta.sceneGuidLow = comp->NavMeshDataGuid.low;
            meta.bakeHash = bakeHash;

            std::vector<uint8_t> payload;
            if (nav::io::BuildNavPayload(*rt, bakeHash, payload)) {
                nav::io::NavPackChunk chunk{};
                chunk.gridX = 0;
                chunk.gridZ = 0;
                chunk.bounds = bounds;
                chunk.size = payload.size();
                chunk.hash = bakeHash;

                std::vector<nav::io::NavPackChunk> chunks{chunk};
                std::vector<std::vector<uint8_t>> payloads{std::move(payload)};

                if (comp->NavPackPath.empty()) {
                    std::filesystem::path pack = navPath;
                    pack.replace_extension(".navpack");
                    comp->NavPackPath = (!projectRoot.empty())
                        ? std::filesystem::relative(pack, projectRoot).string()
                        : pack.string();
                }

                std::filesystem::path packPath(comp->NavPackPath);
                if (packPath.is_absolute() && !projectRoot.empty()) {
                    std::error_code ec;
                    auto rel = std::filesystem::relative(packPath, projectRoot, ec);
                    if (ec || rel.empty() || rel.string().find("..") != std::string::npos ||
                        !std::filesystem::exists(packPath.parent_path(), ec)) {
                        comp->NavPackPath = std::filesystem::path(comp->AssetPath).replace_extension(".navpack").string();
                        packPath = projectRoot / comp->NavPackPath;
                    } else {
                        comp->NavPackPath = rel.string();
                        packPath = projectRoot / comp->NavPackPath;
                    }
                } else if (packPath.is_relative()) {
                    packPath = projectRoot / packPath;
                }
                packPath = packPath.lexically_normal();
                std::error_code ec;
                std::filesystem::create_directories(packPath.parent_path(), ec);
                if (ec) {
                    std::cerr << "[Nav] Bake failed: cannot create navpack directory " << packPath.parent_path().string()
                              << ": " << ec.message() << std::endl;
                    comp->Baking.store(false);
                    comp->BakingStage.store(0);
                    return;
                }
                nav::io::WriteNavPack(packPath.string(), meta, chunks, payloads);
            }
        }

        comp->Runtime = rt;
        comp->LoadAttempted = true;
        comp->AssetDirty = false;
        comp->LastBakeCostMin.store(1.0f);
        comp->LastBakeCostMax.store(1.0f);
        comp->LastBakeIslands.store(rt->m_Polys.empty() ? 0u : 1u);
        comp->LastBakeRemovedIslands.store(0u);
        comp->BakingProgress.store(1.0f);
        comp->BakingStage.store(0);
        comp->Baking.store(false);
        } catch (const std::exception& ex) {
            std::cerr << "[Nav] Bake thread exception: " << ex.what() << std::endl;
            comp->Baking.store(false);
            comp->BakingStage.store(0);
        } catch (...) {
            std::cerr << "[Nav] Bake thread exception: unknown" << std::endl;
            comp->Baking.store(false);
            comp->BakingStage.store(0);
        }
    }).detach();
}
#else
void nav::jobs::SubmitBake(NavMeshComponent* comp, Scene* scene)
{
    (void)comp;
    (void)scene;
}
#endif
