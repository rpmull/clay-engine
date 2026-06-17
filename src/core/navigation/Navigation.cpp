// Ensure Windows macro pollution is disabled before any system/third-party includes
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "core/navigation/Navigation.h"
#include "core/navigation/NavPathfinder.h"
#include "core/utils/PrefabPerfDiagnostics.h"
#include "core/utils/Profiler.h"
#include "core/jobs/ParallelFor.h"
#include "core/jobs/Jobs.h"
#include "core/navigation/NavMesh.h"
#include "core/navigation/NavAgent.h"
#include "core/navigation/NavQueries.h"
#include "core/navigation/NavDebugDraw.h"
#include "core/ecs/Scene.h"
#include "core/ecs/Components.h"
#include "core/ecs/NpcScalability.h"
#include "core/navigation/NavInterop.h"
#include "core/physics/Physics.h"
#include <atomic>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <cfloat>
#include <chrono>

// After all includes, ensure min/max macros aren't defined
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif


using namespace nav;

// Set to true for nav debugging; false in production to avoid hot-path I/O
static constexpr bool kNavVerbose = false;

namespace {
    inline glm::vec3 ToNavQuerySpace(const glm::vec3& placedPos, float agentPlacementOffset)
    {
        glm::vec3 queryPos = placedPos;
        queryPos.y += agentPlacementOffset;
        return queryPos;
    }

    inline glm::vec3 ToPlacedSpace(const glm::vec3& rawNavPos, float agentPlacementOffset)
    {
        glm::vec3 placedPos = rawNavPos;
        placedPos.y -= agentPlacementOffset;
        return placedPos;
    }

    bool NearestPlacedPoint(const NavMeshRuntime& runtime,
                            float agentPlacementOffset,
                            const glm::vec3& placedPos,
                            float maxDist,
                            glm::vec3& outPlaced,
                            glm::vec3* outRaw = nullptr)
    {
        glm::vec3 rawOnMesh(0.0f);
        if (!runtime.NearestPoint(ToNavQuerySpace(placedPos, agentPlacementOffset), maxDist, rawOnMesh)) {
            return false;
        }

        if (outRaw) {
            *outRaw = rawOnMesh;
        }
        outPlaced = ToPlacedSpace(rawOnMesh, agentPlacementOffset);
        return true;
    }

    void ApplyPlacementOffset(NavPath& path, float agentPlacementOffset)
    {
        if (std::abs(agentPlacementOffset) <= 1e-6f) {
            return;
        }

        for (glm::vec3& point : path.points) {
            point.y -= agentPlacementOffset;
        }
    }

    void AppendPlacedPathPoints(NavPath& out,
                                const NavPath& src,
                                float agentPlacementOffset,
                                bool skipFirstPoint)
    {
        if (src.points.empty()) {
            return;
        }

        size_t begin = skipFirstPoint ? 1u : 0u;
        if (begin >= src.points.size()) {
            return;
        }

        out.points.reserve(out.points.size() + (src.points.size() - begin));
        for (size_t i = begin; i < src.points.size(); ++i) {
            out.points.push_back(ToPlacedSpace(src.points[i], agentPlacementOffset));
        }
        out.valid = !out.points.empty();
    }

    bool SampleNavmeshHeight(const NavMeshRuntime& runtime, const glm::vec3& pos, float range, float& outY)
    {
        if (range <= 0.0f) return false;
        const glm::vec3 up(0.0f, 1.0f, 0.0f);
        const glm::vec3 start = pos + up * range;
        const glm::vec3 end = pos - up * range;
        float tHit = 0.0f;
        glm::vec3 hitNormal(0.0f);
        if (!runtime.Raycast(start, end, tHit, hitNormal)) return false;
        const glm::vec3 hit = start + (end - start) * tHit;
        outY = hit.y;
        return true;
    }

    struct BoundaryEdge {
        glm::vec3 a{0.0f};
        glm::vec3 b{0.0f};
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
    };

    static inline uint64_t EdgeKey(uint32_t a, uint32_t b) {
        uint32_t lo = std::min(a, b);
        uint32_t hi = std::max(a, b);
        return (uint64_t(hi) << 32) | uint64_t(lo);
    }

    static inline uint64_t HashCombine64(uint64_t h, uint64_t k) {
        h ^= k + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }

    static inline uint64_t fnv1a64_local(const void* data, size_t len, uint64_t seed)
    {
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(data);
        uint64_t hash = seed;
        for (size_t i = 0; i < len; ++i) {
            hash ^= (uint64_t)ptr[i];
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    static inline float SegmentDistanceSq2D(const glm::vec2& a0, const glm::vec2& a1,
                                            const glm::vec2& b0, const glm::vec2& b1)
    {
        const glm::vec2 da = a1 - a0;
        const glm::vec2 db = b1 - b0;
        const glm::vec2 r = a0 - b0;
        const float a = glm::dot(da, da);
        const float e = glm::dot(db, db);
        const float f = glm::dot(db, r);
        float s = 0.0f;
        float t = 0.0f;
        if (a <= 1e-6f && e <= 1e-6f) {
            return glm::dot(r, r);
        }
        if (a <= 1e-6f) {
            s = 0.0f;
            t = std::clamp(f / e, 0.0f, 1.0f);
        } else {
            float c = glm::dot(da, r);
            if (e <= 1e-6f) {
                t = 0.0f;
                s = std::clamp(-c / a, 0.0f, 1.0f);
            } else {
                float b = glm::dot(da, db);
                float denom = a * e - b * b;
                if (denom != 0.0f) {
                    s = std::clamp((b * f - c * e) / denom, 0.0f, 1.0f);
                } else {
                    s = 0.0f;
                }
                t = (b * s + f) / e;
                if (t < 0.0f) {
                    t = 0.0f;
                    s = std::clamp(-c / a, 0.0f, 1.0f);
                } else if (t > 1.0f) {
                    t = 1.0f;
                    s = std::clamp((b - c) / a, 0.0f, 1.0f);
                }
            }
        }
        const glm::vec2 c1 = a0 + da * s;
        const glm::vec2 c2 = b0 + db * t;
        const glm::vec2 d = c1 - c2;
        return glm::dot(d, d);
    }

    static inline float PointAABBDistSq(const glm::vec3& p, const Bounds& b)
    {
        float dx = std::max(std::max(b.min.x - p.x, 0.0f), p.x - b.max.x);
        float dy = std::max(std::max(b.min.y - p.y, 0.0f), p.y - b.max.y);
        float dz = std::max(std::max(b.min.z - p.z, 0.0f), p.z - b.max.z);
        return dx * dx + dy * dy + dz * dz;
    }

    static float PathLength(const NavPath& path)
    {
        float sum = 0.0f;
        for (size_t i = 1; i < path.points.size(); ++i) {
            sum += glm::length(path.points[i] - path.points[i - 1]);
        }
        return sum;
    }

    static void BuildBoundaryEdges(const NavMeshRuntime& nm, std::vector<BoundaryEdge>& outEdges)
    {
        outEdges.clear();
        struct EdgeInfo { uint32_t count = 0; uint32_t poly = 0; uint32_t a = 0; uint32_t b = 0; };
        std::unordered_map<uint64_t, EdgeInfo> edgeMap;
        edgeMap.reserve(nm.m_Polys.size() * 3);
        for (uint32_t i = 0; i < nm.m_Polys.size(); ++i) {
            const auto& p = nm.m_Polys[i];
            const uint32_t v[3] = { p.i0, p.i1, p.i2 };
            for (int e = 0; e < 3; ++e) {
                uint32_t a = v[e];
                uint32_t b = v[(e + 1) % 3];
                uint64_t key = EdgeKey(a, b);
                auto& info = edgeMap[key];
                if (info.count == 0) {
                    info.poly = i;
                    info.a = a;
                    info.b = b;
                }
                info.count++;
            }
        }
        outEdges.reserve(edgeMap.size());
        for (const auto& kv : edgeMap) {
            const EdgeInfo& info = kv.second;
            if (info.count != 1) continue;
            if (info.a >= nm.m_Vertices.size() || info.b >= nm.m_Vertices.size()) continue;
            const auto& p = nm.m_Polys[info.poly];
            const glm::vec3& pa = nm.m_Vertices[p.i0];
            const glm::vec3& pb = nm.m_Vertices[p.i1];
            const glm::vec3& pc = nm.m_Vertices[p.i2];
            glm::vec3 n = glm::cross(pb - pa, pc - pa);
            float len = glm::length(n);
            if (len > 1e-6f) n /= len;
            BoundaryEdge e;
            e.a = nm.m_Vertices[info.a];
            e.b = nm.m_Vertices[info.b];
            e.normal = n;
            outEdges.push_back(e);
        }
    }
}

Navigation& Navigation::Get()
{
    static Navigation s; return s;
}

void Navigation::SetDebugMask(NavDrawMask mask) { m_DebugMask = mask; debug::SetMask(mask); }
NavDrawMask Navigation::GetDebugMask() const { return m_DebugMask; }

bool Navigation::FindPath(Scene& scene, uint32_t navMeshEntity, const glm::vec3& start, const glm::vec3& end,
                          const NavAgentParams& p, NavFlags include, NavFlags exclude, NavPath& out)
{
    if (navMeshEntity == 0 || navMeshEntity == INVALID_ENTITY_ID) {
        BuildNavMeshGraph(scene, true);
        return FindPathAcrossMeshes(scene, start, end, p, include, exclude, out, nullptr, false);
    }
    auto* data = scene.GetEntityData(navMeshEntity);
    if (!data || !data->Navigation) return false;
    auto& comp = *data->Navigation; // added to Components.h later
    if (comp.ChunkedNavEnabled) {
        BuildNavMeshGraph(scene, true);
        return FindPathAcrossMeshes(scene, start, end, p, include, exclude, out, nullptr, false);
    }
    if (!comp.Runtime && !comp.EnsureRuntimeLoaded(scene)) return false;
    BuildNavMeshGraph(scene, false);
    const glm::vec3 rawStart = ToNavQuerySpace(start, comp.AgentPlacementOffset);
    const glm::vec3 rawEnd = ToNavQuerySpace(end, comp.AgentPlacementOffset);
    const bool found = comp.Runtime->FindPath(rawStart, rawEnd, out, p, include, exclude);
    if (found && out.valid) {
        ApplyPlacementOffset(out, comp.AgentPlacementOffset);
    }
    return found;
}

bool Navigation::Raycast(Scene& scene, uint32_t navMeshEntity, const glm::vec3& start, const glm::vec3& end, float& tHit, glm::vec3& hitNormal)
{
    auto* data = scene.GetEntityData(navMeshEntity);
    if (!data || !data->Navigation) return false;
    auto& comp = *data->Navigation;
    if (!comp.Runtime && !comp.EnsureRuntimeLoaded(scene)) return false;
    return comp.Runtime->Raycast(start, end, tHit, hitNormal);
}

bool Navigation::NearestPoint(Scene& scene, uint32_t navMeshEntity, const glm::vec3& pos, float maxDist, glm::vec3& outOnMesh)
{
    auto* data = scene.GetEntityData(navMeshEntity);
    if (!data || !data->Navigation) return false;
    auto& comp = *data->Navigation;
    if (!comp.Runtime && !comp.EnsureRuntimeLoaded(scene)) return false;
    return NearestPlacedPoint(*comp.Runtime, comp.AgentPlacementOffset, pos, maxDist, outOnMesh);
}

bool Navigation::BuildNavMeshGraph(Scene& scene, bool loadRuntime)
{
    std::vector<NavMeshEntry> entries;
    entries.reserve(scene.GetEntities().size());
    std::vector<OffMeshLink> navLinks;
    navLinks.reserve(scene.GetEntities().size());
    std::unordered_map<EntityID, uint64_t> observedStreamRevisions;
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (const auto& e : scene.GetEntities()) {
        auto* d = scene.GetEntityData(e.GetID());
        if (!d || !d->Navigation) continue;
        auto& comp = *d->Navigation;
        if (!comp.Enabled) continue;
        if (loadRuntime) {
            if (comp.ChunkedNavEnabled) {
                if (!comp.EnsureRuntimeLoaded(scene)) continue;
            } else if (!comp.Runtime && !comp.EnsureRuntimeLoaded(scene)) {
                continue;
            }
        }
        if (comp.ChunkedNavEnabled && comp.ChunkManager) {
            auto runtime = comp.ChunkManager->GetStitchedRuntime();
            if (!runtime || !runtime->HasDetour()) continue;

            NavMeshEntry entry;
            entry.entity = e.GetID();
            entry.runtime = runtime;
            entry.bounds = comp.AABB;
            entry.domainId = comp.DomainId;
            entry.domainPriority = comp.DomainPriority;
            entry.autoPortalEnabled = comp.AutoPortalEnabled;
            entry.autoPortalMaxXZ = comp.AutoPortalMaxXZ;
            entry.autoPortalMaxHeight = comp.AutoPortalMaxHeight;
            entry.maxNormalAngleDeg = comp.StitchMaxNormalAngleDeg;
            entry.agentPlacementOffset = comp.AgentPlacementOffset;
            entry.fromChunkedPack = false;
            entry.chunkX = 0;
            entry.chunkZ = 0;
            entries.push_back(entry);

            hash = HashCombine64(hash, (uint64_t)entry.entity);
            hash = HashCombine64(hash, (uint64_t)reinterpret_cast<uintptr_t>(runtime.get()));
            hash = HashCombine64(hash, (uint64_t)entry.domainId);
            hash = HashCombine64(hash, (uint64_t)entry.domainPriority);
            hash = HashCombine64(hash, (uint64_t)entry.autoPortalEnabled);
            hash = HashCombine64(hash, fnv1a64_local(&entry.autoPortalMaxXZ, sizeof(entry.autoPortalMaxXZ), 0x1212));
            hash = HashCombine64(hash, fnv1a64_local(&entry.autoPortalMaxHeight, sizeof(entry.autoPortalMaxHeight), 0x1313));
            hash = HashCombine64(hash, fnv1a64_local(&entry.agentPlacementOffset, sizeof(entry.agentPlacementOffset), 0x1337));
            hash = HashCombine64(hash, fnv1a64_local(&entry.bounds.min, sizeof(entry.bounds.min), 0x1414));
            hash = HashCombine64(hash, fnv1a64_local(&entry.bounds.max, sizeof(entry.bounds.max), 0x1515));
            const uint64_t streamRevision = comp.ChunkManager->GetStreamRevision();
            hash = HashCombine64(hash, streamRevision);
            observedStreamRevisions[e.GetID()] = streamRevision;
            continue;
        }

        auto runtime = std::atomic_load(&comp.Runtime);
        if (!runtime) continue;

        NavMeshEntry entry;
        entry.entity = e.GetID();
        entry.runtime = runtime;
        entry.bounds = comp.AABB;
        entry.domainId = comp.DomainId;
        entry.domainPriority = comp.DomainPriority;
        entry.autoPortalEnabled = comp.AutoPortalEnabled;
        entry.autoPortalMaxXZ = comp.AutoPortalMaxXZ;
        entry.autoPortalMaxHeight = comp.AutoPortalMaxHeight;
        entry.maxNormalAngleDeg = comp.StitchMaxNormalAngleDeg;
        entry.agentPlacementOffset = comp.AgentPlacementOffset;
        entry.fromChunkedPack = false;
        entry.chunkX = 0;
        entry.chunkZ = 0;
        entries.push_back(entry);

        hash = HashCombine64(hash, (uint64_t)entry.entity);
        hash = HashCombine64(hash, (uint64_t)reinterpret_cast<uintptr_t>(runtime.get()));
        hash = HashCombine64(hash, (uint64_t)entry.domainId);
        hash = HashCombine64(hash, (uint64_t)entry.domainPriority);
        hash = HashCombine64(hash, (uint64_t)entry.autoPortalEnabled);
        hash = HashCombine64(hash, fnv1a64_local(&entry.autoPortalMaxXZ, sizeof(entry.autoPortalMaxXZ), 0x1212));
        hash = HashCombine64(hash, fnv1a64_local(&entry.autoPortalMaxHeight, sizeof(entry.autoPortalMaxHeight), 0x1313));
        hash = HashCombine64(hash, fnv1a64_local(&entry.agentPlacementOffset, sizeof(entry.agentPlacementOffset), 0x1337));
        hash = HashCombine64(hash, fnv1a64_local(&entry.bounds.min, sizeof(entry.bounds.min), 0x1414));
        hash = HashCombine64(hash, fnv1a64_local(&entry.bounds.max, sizeof(entry.bounds.max), 0x1515));
    }

    for (const auto& e : scene.GetEntities()) {
        auto* d = scene.GetEntityData(e.GetID());
        if (!d || !d->NavLink) continue;
        const auto& linkComp = *d->NavLink;
        if (!linkComp.Enabled) continue;

        glm::vec3 start = linkComp.Start;
        glm::vec3 end = linkComp.End;
        if (!linkComp.UseWorldSpace) {
            const glm::mat4& world = d->Transform.WorldMatrix;
            start = glm::vec3(world * glm::vec4(start, 1.0f));
            end = glm::vec3(world * glm::vec4(end, 1.0f));
        }

        OffMeshLink link;
        link.a = start;
        link.b = end;
        link.radius = linkComp.Radius;
        link.cost = linkComp.Cost;
        link.flags = linkComp.Flags;
        link.bidir = linkComp.Bidirectional ? 1 : 0;
        navLinks.push_back(link);

        hash = HashCombine64(hash, fnv1a64_local(&link.a, sizeof(link.a), 0x1616));
        hash = HashCombine64(hash, fnv1a64_local(&link.b, sizeof(link.b), 0x1717));
        hash = HashCombine64(hash, fnv1a64_local(&link.radius, sizeof(link.radius), 0x1818));
        hash = HashCombine64(hash, fnv1a64_local(&link.cost, sizeof(link.cost), 0x1919));
        hash = HashCombine64(hash, fnv1a64_local(&link.flags, sizeof(link.flags), 0x1A1A));
        hash = HashCombine64(hash, (uint64_t)link.bidir);
    }

    if (hash == m_GraphHash && entries.size() == m_GraphMeshes.size()) {
        return false;
    }

    m_GraphHash = hash;
    m_GraphMeshes = std::move(entries);
    m_GraphPortals.assign(m_GraphMeshes.size(), {});
    m_ObservedStreamRevisions = std::move(observedStreamRevisions);

    if (!m_GraphMeshes.empty()) {
        std::vector<std::vector<OffMeshLink>> linksPerMesh(m_GraphMeshes.size());
        auto assignLink = [&](const OffMeshLink& link) {
            float bestScore = FLT_MAX;
            size_t bestIdx = m_GraphMeshes.size();
            const float maxRadius = std::max(link.radius, 0.0f);
            const float maxRadiusSq = maxRadius * maxRadius;
            for (size_t i = 0; i < m_GraphMeshes.size(); ++i) {
                const Bounds& b = m_GraphMeshes[i].bounds;
                float distA = PointAABBDistSq(link.a, b);
                float distB = PointAABBDistSq(link.b, b);
                if (distA > maxRadiusSq || distB > maxRadiusSq) continue;
                float score = distA + distB;
                if (score < bestScore) {
                    bestScore = score;
                    bestIdx = i;
                }
            }
            if (bestIdx != m_GraphMeshes.size()) {
                linksPerMesh[bestIdx].push_back(link);
            }
        };
        for (const auto& link : navLinks) {
            assignLink(link);
        }
        for (size_t i = 0; i < m_GraphMeshes.size(); ++i) {
            if (m_GraphMeshes[i].runtime) {
                m_GraphMeshes[i].runtime->SetExternalLinks(std::move(linksPerMesh[i]));
            }
        }
    }

    GetPathfinder().ClearCache();

    if (m_GraphMeshes.size() < 2) return true;

    const bool useFastPortalBuild = (m_GraphMeshes.size() >= 48);
    std::vector<std::vector<BoundaryEdge>> boundaryEdges(m_GraphMeshes.size());
    std::vector<uint8_t> boundaryEdgesBuilt(m_GraphMeshes.size(), 0);
    auto ensureBoundaryEdges = [&](size_t idx) -> const std::vector<BoundaryEdge>& {
        if (idx >= m_GraphMeshes.size()) {
            static const std::vector<BoundaryEdge> kEmpty;
            return kEmpty;
        }
        if (!boundaryEdgesBuilt[idx]) {
            boundaryEdgesBuilt[idx] = 1;
            if (m_GraphMeshes[idx].runtime) {
                BuildBoundaryEdges(*m_GraphMeshes[idx].runtime, boundaryEdges[idx]);
            }
        }
        return boundaryEdges[idx];
    };

    size_t portalCount = 0;
    const size_t maxPortalsPerPair = 32;
    float globalPortalMaxXZ = 0.0f;
    for (const auto& mesh : m_GraphMeshes) {
        if (!mesh.autoPortalEnabled) continue;
        globalPortalMaxXZ = std::max(globalPortalMaxXZ, mesh.autoPortalMaxXZ);
    }
    std::vector<uint32_t> sortedMeshIndices(m_GraphMeshes.size());
    std::iota(sortedMeshIndices.begin(), sortedMeshIndices.end(), 0u);
    std::sort(sortedMeshIndices.begin(), sortedMeshIndices.end(),
              [this](uint32_t lhs, uint32_t rhs) {
                  return m_GraphMeshes[lhs].bounds.min.x < m_GraphMeshes[rhs].bounds.min.x;
              });

    for (size_t ai = 0; ai < sortedMeshIndices.size(); ++ai) {
        const size_t a = sortedMeshIndices[ai];
        const auto& A = m_GraphMeshes[a];
        const float maxSearchX = A.bounds.max.x + std::max(0.0f, globalPortalMaxXZ);
        for (size_t bi = ai + 1; bi < sortedMeshIndices.size(); ++bi) {
            const size_t b = sortedMeshIndices[bi];
            const auto& B = m_GraphMeshes[b];
            if (B.bounds.min.x > maxSearchX) {
                break;
            }
            if (!A.autoPortalEnabled || !B.autoPortalEnabled) continue;

            const bool fromSameChunkedPack = A.fromChunkedPack && B.fromChunkedPack && A.entity == B.entity;
            const int chunkDx = std::abs(A.chunkX - B.chunkX);
            const int chunkDz = std::abs(A.chunkZ - B.chunkZ);
            const bool chunkNeighbors = fromSameChunkedPack && chunkDx <= 1 && chunkDz <= 1 && (chunkDx + chunkDz) > 0;
            // For chunked navpacks, only adjacent chunk pairs are valid portal candidates.
            if (fromSameChunkedPack && !chunkNeighbors) continue;

            const float maxXZ = std::max(A.autoPortalMaxXZ, B.autoPortalMaxXZ);
            const float maxHeight = std::max(A.autoPortalMaxHeight, B.autoPortalMaxHeight);
            const float cosMaxAngle = std::cos(glm::radians(std::min(A.maxNormalAngleDeg, B.maxNormalAngleDeg)));
            if (maxXZ <= 0.0f || maxHeight <= 0.0f) continue;

            float dx = 0.0f;
            if (A.bounds.max.x < B.bounds.min.x) dx = B.bounds.min.x - A.bounds.max.x;
            else if (A.bounds.min.x > B.bounds.max.x) dx = A.bounds.min.x - B.bounds.max.x;
            float dz = 0.0f;
            if (A.bounds.max.z < B.bounds.min.z) dz = B.bounds.min.z - A.bounds.max.z;
            else if (A.bounds.min.z > B.bounds.max.z) dz = A.bounds.min.z - B.bounds.max.z;
            float dy = 0.0f;
            if (A.bounds.max.y < B.bounds.min.y) dy = B.bounds.min.y - A.bounds.max.y;
            else if (A.bounds.min.y > B.bounds.max.y) dy = A.bounds.min.y - B.bounds.max.y;
            float dxz = std::sqrt(dx * dx + dz * dz);
            if (dxz > maxXZ || dy > maxHeight) continue;

            auto appendFastPortal = [&]() -> bool {
                const glm::vec3 aCenter = (A.bounds.min + A.bounds.max) * 0.5f;
                const glm::vec3 bCenter = (B.bounds.min + B.bounds.max) * 0.5f;
                const glm::vec3 aProbe(
                    std::clamp(bCenter.x, A.bounds.min.x, A.bounds.max.x),
                    std::clamp(bCenter.y, A.bounds.min.y, A.bounds.max.y),
                    std::clamp(bCenter.z, A.bounds.min.z, A.bounds.max.z)
                );
                const glm::vec3 bProbe(
                    std::clamp(aCenter.x, B.bounds.min.x, B.bounds.max.x),
                    std::clamp(aCenter.y, B.bounds.min.y, B.bounds.max.y),
                    std::clamp(aCenter.z, B.bounds.min.z, B.bounds.max.z)
                );
                const float search = std::max(1.0f, std::max(maxXZ, maxHeight) * 4.0f);
                glm::vec3 aOn, bOn;
                if (!A.runtime->NearestPoint(aProbe, search, aOn)) return false;
                if (!B.runtime->NearestPoint(bProbe, search, bOn)) return false;

                const float sepXZ = glm::distance(glm::vec2(aOn.x, aOn.z), glm::vec2(bOn.x, bOn.z));
                const float sepY = std::abs(aOn.y - bOn.y);
                if (sepXZ > std::max(1.0f, maxXZ * 3.0f) || sepY > std::max(1.0f, maxHeight * 3.0f)) return false;

                const float bridgeCost = std::max(0.01f, glm::distance(aOn, bOn));
                NavPortal pAB;
                pAB.from = (uint32_t)a;
                pAB.to = (uint32_t)b;
                pAB.fromPos = aOn;
                pAB.toPos = bOn;
                pAB.cost = bridgeCost;
                m_GraphPortals[a].push_back(pAB);

                NavPortal pBA;
                pBA.from = (uint32_t)b;
                pBA.to = (uint32_t)a;
                pBA.fromPos = bOn;
                pBA.toPos = aOn;
                pBA.cost = bridgeCost;
                m_GraphPortals[b].push_back(pBA);
                portalCount += 2;
                return true;
            };

            auto appendDetailedPortals = [&]() -> bool {
                const auto& edgesA = ensureBoundaryEdges(a);
                const auto& edgesB = ensureBoundaryEdges(b);
                if (edgesA.empty() || edgesB.empty()) return false;

                const float maxXZSq = maxXZ * maxXZ;
                const float cellSize = std::max(0.1f, maxXZ);
                auto cellKey = [&](int cx, int cz) -> int64_t {
                    return (int64_t(cx) << 32) ^ (int64_t(cz) & 0xFFFFFFFF);
                };

                struct EdgeRef { BoundaryEdge edge; };
                std::unordered_map<int64_t, std::vector<EdgeRef>> buckets;
                buckets.reserve(edgesB.size() * 2);
                for (const auto& e : edgesB) {
                    float minX = std::min(e.a.x, e.b.x) - maxXZ;
                    float maxX = std::max(e.a.x, e.b.x) + maxXZ;
                    float minZ = std::min(e.a.z, e.b.z) - maxXZ;
                    float maxZ = std::max(e.a.z, e.b.z) + maxXZ;
                    int cx0 = (int)std::floor(minX / cellSize);
                    int cx1 = (int)std::floor(maxX / cellSize);
                    int cz0 = (int)std::floor(minZ / cellSize);
                    int cz1 = (int)std::floor(maxZ / cellSize);
                    for (int cz = cz0; cz <= cz1; ++cz) {
                        for (int cx = cx0; cx <= cx1; ++cx) {
                            buckets[cellKey(cx, cz)].push_back({ e });
                        }
                    }
                }

                struct PortalCandidate {
                    glm::vec3 fromPos;
                    glm::vec3 toPos;
                    float score;
                };
                std::vector<PortalCandidate> candidates;
                candidates.reserve(edgesA.size());
                for (const auto& e : edgesA) {
                    float minX = std::min(e.a.x, e.b.x) - maxXZ;
                    float maxX = std::max(e.a.x, e.b.x) + maxXZ;
                    float minZ = std::min(e.a.z, e.b.z) - maxXZ;
                    float maxZ = std::max(e.a.z, e.b.z) + maxXZ;
                    int cx0 = (int)std::floor(minX / cellSize);
                    int cx1 = (int)std::floor(maxX / cellSize);
                    int cz0 = (int)std::floor(minZ / cellSize);
                    int cz1 = (int)std::floor(maxZ / cellSize);
                    bool found = false;
                    float bestScore = FLT_MAX;
                    glm::vec3 bestFrom{};
                    glm::vec3 bestTo{};
                    for (int cz = cz0; cz <= cz1; ++cz) {
                        for (int cx = cx0; cx <= cx1; ++cx) {
                            auto it = buckets.find(cellKey(cx, cz));
                            if (it == buckets.end()) continue;
                            for (const auto& ref : it->second) {
                                const auto& eb = ref.edge;
                                if (glm::dot(e.normal, eb.normal) < cosMaxAngle) continue;
                                float dist2 = SegmentDistanceSq2D(glm::vec2(e.a.x, e.a.z), glm::vec2(e.b.x, e.b.z),
                                                                 glm::vec2(eb.a.x, eb.a.z), glm::vec2(eb.b.x, eb.b.z));
                                if (dist2 > maxXZSq) continue;
                                float minH = std::min({ std::abs(e.a.y - eb.a.y),
                                                        std::abs(e.a.y - eb.b.y),
                                                        std::abs(e.b.y - eb.a.y),
                                                        std::abs(e.b.y - eb.b.y) });
                                if (minH > maxHeight) continue;
                                float score = dist2 + minH * minH;
                                if (score < bestScore) {
                                    bestScore = score;
                                    bestFrom = (e.a + e.b) * 0.5f;
                                    bestTo = (eb.a + eb.b) * 0.5f;
                                    found = true;
                                }
                            }
                        }
                    }
                    if (found) {
                        candidates.push_back({ bestFrom, bestTo, bestScore });
                    }
                }

                if (candidates.empty()) return false;
                std::sort(candidates.begin(), candidates.end(),
                          [](const PortalCandidate& x, const PortalCandidate& y){ return x.score < y.score; });
                if (candidates.size() > maxPortalsPerPair) candidates.resize(maxPortalsPerPair);
                for (const auto& c : candidates) {
                    NavPortal pAB;
                    pAB.from = (uint32_t)a;
                    pAB.to = (uint32_t)b;
                    pAB.fromPos = c.fromPos;
                    pAB.toPos = c.toPos;
                    pAB.cost = std::sqrt(c.score);
                    m_GraphPortals[a].push_back(pAB);

                    NavPortal pBA;
                    pBA.from = (uint32_t)b;
                    pBA.to = (uint32_t)a;
                    pBA.fromPos = c.toPos;
                    pBA.toPos = c.fromPos;
                    pBA.cost = std::sqrt(c.score);
                    m_GraphPortals[b].push_back(pBA);
                    portalCount += 2;
                }
                return true;
            };

            // Chunked nav should avoid expensive boundary matching in steady-state streaming.
            // Neighbor chunk pairs use a lightweight bridge portal (or none if probe fails).
            if (chunkNeighbors) {
                appendFastPortal();
                continue;
            }

            if (useFastPortalBuild) {
                appendFastPortal();
            } else {
                appendDetailedPortals();
            }
        }
    }

    if (portalCount > 0 && kNavVerbose) {
        const char* modeLabel = useFastPortalBuild ? "fast" : "detailed";
        std::cout << "[Nav] Auto portals built: meshes=" << m_GraphMeshes.size()
                  << " portals=" << portalCount
                  << " mode=" << modeLabel
                  << std::endl;
    }
    return true;
}

bool Navigation::FindPathAcrossMeshes(Scene& scene, const glm::vec3& start, const glm::vec3& end,
                                      const NavAgentParams& p, NavFlags include, NavFlags exclude, NavPath& out,
                                      std::vector<EntityID>* outMeshChain, bool ensureGraphBuilt)
{
    if (ensureGraphBuilt) {
        BuildNavMeshGraph(scene, true);
    }
    if (m_GraphMeshes.empty()) return false;
    if (m_GraphMeshes.size() == 1) {
        auto& entry = m_GraphMeshes.front();
        const glm::vec3 rawStart = ToNavQuerySpace(start, entry.agentPlacementOffset);
        const glm::vec3 rawEnd = ToNavQuerySpace(end, entry.agentPlacementOffset);
        const bool found = entry.runtime->FindPath(rawStart, rawEnd, out, p, include, exclude);
        if (found && out.valid) {
            ApplyPlacementOffset(out, entry.agentPlacementOffset);
        }
        return found;
    }

    struct Candidate {
        uint32_t idx = 0;
        glm::vec3 rawOnMesh{0.0f};
        float dist2 = FLT_MAX;
    };
    auto findNearestMesh = [&](const glm::vec3& pos, Candidate& outCand) -> bool {
        bool found = false;
        for (uint32_t i = 0; i < (uint32_t)m_GraphMeshes.size(); ++i) {
            const auto& m = m_GraphMeshes[i];
            glm::vec3 placedOnMesh(0.0f);
            glm::vec3 rawOnMesh(0.0f);
            if (!NearestPlacedPoint(*m.runtime, m.agentPlacementOffset, pos,
                                    std::max(1.0f, p.height + p.maxStep), placedOnMesh, &rawOnMesh)) {
                continue;
            }
            float d2 = glm::distance2(pos, placedOnMesh);
            if (d2 < outCand.dist2) {
                outCand.idx = i;
                outCand.rawOnMesh = rawOnMesh;
                outCand.dist2 = d2;
                found = true;
            }
        }
        return found;
    };

    Candidate startCand, endCand;
    if (!findNearestMesh(start, startCand)) return false;
    if (!findNearestMesh(end, endCand)) return false;

    if (startCand.idx == endCand.idx) {
        NavPath rawPath;
        if (m_GraphMeshes[startCand.idx].runtime->FindPath(startCand.rawOnMesh, endCand.rawOnMesh, rawPath, p, include, exclude)) {
            out = std::move(rawPath);
            ApplyPlacementOffset(out, m_GraphMeshes[startCand.idx].agentPlacementOffset);
            if (outMeshChain) {
                outMeshChain->clear();
                outMeshChain->push_back(m_GraphMeshes[startCand.idx].entity);
            }
            return true;
        }
        // If direct path fails (disconnected islands), try a two-hop route via another mesh.
        for (uint32_t mid = 0; mid < (uint32_t)m_GraphMeshes.size(); ++mid) {
            if (mid == startCand.idx) continue;
            const auto& portalsOut = m_GraphPortals[startCand.idx];
            const auto& portalsBack = m_GraphPortals[mid];
            for (const auto& outPortal : portalsOut) {
                if (outPortal.to != mid) continue;
                NavPath segA;
                if (!m_GraphMeshes[startCand.idx].runtime->FindPath(startCand.rawOnMesh, outPortal.fromPos, segA, p, include, exclude)) continue;
                for (const auto& backPortal : portalsBack) {
                    if (backPortal.to != startCand.idx) continue;
                    NavPath segB;
                    if (!m_GraphMeshes[mid].runtime->FindPath(outPortal.toPos, backPortal.fromPos, segB, p, include, exclude)) continue;
                    NavPath segC;
                    if (!m_GraphMeshes[startCand.idx].runtime->FindPath(backPortal.toPos, endCand.rawOnMesh, segC, p, include, exclude)) continue;
                    NavPath result;
                    AppendPlacedPathPoints(result, segA, m_GraphMeshes[startCand.idx].agentPlacementOffset, false);
                    AppendPlacedPathPoints(result, segB, m_GraphMeshes[mid].agentPlacementOffset, true);
                    AppendPlacedPathPoints(result, segC, m_GraphMeshes[startCand.idx].agentPlacementOffset, true);
                    result.valid = !result.points.empty();
                    out = std::move(result);
                    if (outMeshChain) {
                        outMeshChain->clear();
                        outMeshChain->push_back(m_GraphMeshes[startCand.idx].entity);
                        outMeshChain->push_back(m_GraphMeshes[mid].entity);
                        outMeshChain->push_back(m_GraphMeshes[startCand.idx].entity);
                    }
                    return out.valid;
                }
            }
        }
        return false;
    }

    // Fast path: try direct portals between start and end meshes.
    {
        const auto& portals = m_GraphPortals[startCand.idx];
        float bestLen = FLT_MAX;
        NavPath bestPath;
        bool found = false;
        for (const auto& portal : portals) {
            if (portal.to != endCand.idx) continue;
            NavPath segA;
            if (!m_GraphMeshes[startCand.idx].runtime->FindPath(startCand.rawOnMesh, portal.fromPos, segA, p, include, exclude)) continue;
            NavPath segB;
            if (!m_GraphMeshes[endCand.idx].runtime->FindPath(portal.toPos, endCand.rawOnMesh, segB, p, include, exclude)) continue;
            NavPath merged;
            AppendPlacedPathPoints(merged, segA, m_GraphMeshes[startCand.idx].agentPlacementOffset, false);
            AppendPlacedPathPoints(merged, segB, m_GraphMeshes[endCand.idx].agentPlacementOffset, true);
            merged.valid = !merged.points.empty();
            float len = PathLength(merged);
            if (merged.valid && len < bestLen) {
                bestLen = len;
                bestPath = std::move(merged);
                found = true;
            }
        }
        if (found) {
            out = std::move(bestPath);
            if (outMeshChain) {
                outMeshChain->clear();
                outMeshChain->push_back(m_GraphMeshes[startCand.idx].entity);
                outMeshChain->push_back(m_GraphMeshes[endCand.idx].entity);
            }
            return out.valid;
        }
    }

    const uint32_t n = (uint32_t)m_GraphMeshes.size();
    int32_t maxPriority = 0;
    for (const auto& m : m_GraphMeshes) {
        maxPriority = std::max(maxPriority, m.domainPriority);
    }
    std::vector<float> dist(n, FLT_MAX);
    std::vector<int32_t> prev(n, -1);
    std::vector<NavPortal> prevPortal(n);
    std::vector<uint8_t> visited(n, 0);
    dist[startCand.idx] = 0.0f;
    for (uint32_t iter = 0; iter < n; ++iter) {
        float best = FLT_MAX;
        int32_t u = -1;
        for (uint32_t i = 0; i < n; ++i) {
            if (visited[i]) continue;
            if (dist[i] < best) { best = dist[i]; u = (int32_t)i; }
        }
        if (u < 0) break;
        if ((uint32_t)u == endCand.idx) break;
        visited[u] = 1;
        for (const auto& edge : m_GraphPortals[u]) {
            uint32_t v = edge.to;
            if (visited[v]) continue;
            float cost = edge.cost;
            const auto& targetMesh = m_GraphMeshes[v];
            if (p.preferredDomainId != 0 && targetMesh.domainId != p.preferredDomainId) {
                cost += 5.0f;
            }
            if (maxPriority > 0) {
                cost += std::max(0, maxPriority - targetMesh.domainPriority) * 0.1f;
            }
            float nd = dist[u] + cost;
            if (nd < dist[v]) {
                dist[v] = nd;
                prev[v] = u;
                prevPortal[v] = edge;
            }
        }
    }

    if (prev[endCand.idx] == -1) return false;

    std::vector<NavPortal> chain;
    for (int32_t at = endCand.idx; at != (int32_t)startCand.idx; at = prev[at]) {
        chain.push_back(prevPortal[at]);
    }
    std::reverse(chain.begin(), chain.end());

    NavPath result;
    if (outMeshChain) {
        outMeshChain->clear();
        outMeshChain->push_back(m_GraphMeshes[startCand.idx].entity);
    }
    glm::vec3 curStart = startCand.rawOnMesh;
    uint32_t curMesh = startCand.idx;
    for (const auto& portal : chain) {
        if (curMesh != portal.from) break;
        NavPath seg;
        if (!m_GraphMeshes[curMesh].runtime->FindPath(curStart, portal.fromPos, seg, p, include, exclude)) return false;
        AppendPlacedPathPoints(result, seg, m_GraphMeshes[curMesh].agentPlacementOffset, !result.points.empty());
        curStart = portal.toPos;
        curMesh = portal.to;
        if (outMeshChain) outMeshChain->push_back(m_GraphMeshes[curMesh].entity);
    }
    NavPath tail;
    if (!m_GraphMeshes[curMesh].runtime->FindPath(curStart, endCand.rawOnMesh, tail, p, include, exclude)) return false;
    AppendPlacedPathPoints(result, tail, m_GraphMeshes[curMesh].agentPlacementOffset, !result.points.empty());
    result.valid = !result.points.empty();
    out = std::move(result);
    return out.valid;
}

void Navigation::RefreshEntityCaches(Scene& scene)
{
    const uint64_t sceneRevision = scene.GetDirtyRevision();
    const size_t entityCount = scene.GetEntities().size();
    if (m_CachedEntityRevision == sceneRevision &&
        m_CachedEntityCount == entityCount) {
        return;
    }

    m_CachedNavAgentEntities.clear();
    m_CachedNavMeshEntities.clear();
    m_CachedNavAgentEntities.reserve(entityCount);
    m_CachedNavMeshEntities.reserve(entityCount);

    for (const auto& e : scene.GetEntities()) {
        auto* d = scene.GetEntityData(e.GetID());
        if (!d) continue;
        if (d->NavAgent) {
            m_CachedNavAgentEntities.push_back(e.GetID());
        }
        if (d->Navigation) {
            m_CachedNavMeshEntities.push_back(e.GetID());
        }
    }
    m_CachedEntityRevision = sceneRevision;
    m_CachedEntityCount = entityCount;
}

void Navigation::Update(Scene& scene, float dt)
{
    auto& profiler = Profiler::Get();
    profiler.SetCounter("Nav/PendingQueue", static_cast<uint64_t>(GetPathfinder().GetPendingRequestCount()));
    profiler.SetCounter("Nav/CompletedQueue", static_cast<uint64_t>(GetPathfinder().GetCompletedResultCount()));
    profiler.SetCounter("Nav/PendingBudgetQueue", static_cast<uint64_t>(m_PendingPathRequests.size()));

    uint64_t entitiesScanned = 0;
    uint64_t agentsCollected = 0;
    uint64_t needsPathCount = 0;
    uint64_t autoRepathCount = 0;
    uint64_t pathRequestsAsync = 0;
    uint64_t pathRequestsSync = 0;
    uint64_t pathResultsProcessed = 0;
    uint64_t pathResultsStale = 0;
    uint64_t agentMoves = 0;
    const bool collectDetailedPrefabPerf = cm::debug::PrefabPerfDetailedTimingsEnabled();
    struct PrefabNavPerfSample {
        double TotalMs = 0.0;
        uint64_t AgentsCollected = 0;
        uint64_t NeedsPath = 0;
        uint64_t AsyncRequests = 0;
        uint64_t SyncRequests = 0;
        uint64_t PathResults = 0;
        uint64_t AgentMoves = 0;
    };
    std::unordered_map<EntityID, PrefabNavPerfSample> prefabNavPerf;
    if (collectDetailedPrefabPerf) {
        prefabNavPerf.reserve(16);
    }

    RefreshEntityCaches(scene);

    // Update chunked navmesh streaming (camera-driven)
    { ScopedTimer t("Nav/ChunkedStreaming");
    for (EntityID navEntityId : m_CachedNavMeshEntities) {
        ++entitiesScanned;
        auto* d = scene.GetEntityData(navEntityId);
        if (!d || !d->Navigation) continue;
        auto& navComp = *d->Navigation;
        if (!navComp.ChunkedNavEnabled) continue;
        navComp.EnsureRuntimeLoaded(scene);
    }

    // Process any completed async path requests (on main thread - thread safe)
    auto completedResults = GetPathfinder().ProcessCompletedRequests();
    for (const auto& result : completedResults) {
        ++pathResultsProcessed;
        // userData contains the agent entity ID
        EntityID agentEntity = static_cast<EntityID>(result.userData);
        auto* d = scene.GetEntityData(agentEntity);
        if (d && d->NavAgent) {
            const EntityID prefabPerfRootId = collectDetailedPrefabPerf
                ? cm::debug::ResolveOwningPrefabRoot(scene, agentEntity)
                : INVALID_ENTITY_ID;
            if (prefabPerfRootId != INVALID_ENTITY_ID) {
                ++prefabNavPerf[prefabPerfRootId].PathResults;
            }
            // Drop stale results from an old navmesh runtime (e.g., request queued before rebake).
            const NavMeshRuntime* currentRuntime = nullptr;
            if (d->NavAgent->NavMeshEntity != 0) {
                auto* meshData = scene.GetEntityData(d->NavAgent->NavMeshEntity);
                if (meshData && meshData->Navigation) {
                    auto& navComp = *meshData->Navigation;
                    if (navComp.Runtime || navComp.EnsureRuntimeLoaded(scene)) {
                        currentRuntime = navComp.Runtime.get();
                    }
                }
            }
            if (result.navmeshPtr != currentRuntime) {
                ++pathResultsStale;
                d->NavAgent->PathRequested = false;
                continue;
            }

            if (result.success && result.path.valid) {
                NavPath adjustedPath = result.path;
                if (d->NavAgent->NavMeshEntity != 0) {
                    auto* meshData = scene.GetEntityData(d->NavAgent->NavMeshEntity);
                    if (meshData && meshData->Navigation) {
                        ApplyPlacementOffset(adjustedPath, meshData->Navigation->AgentPlacementOffset);
                    }
                }
                d->NavAgent->CurrentPath = std::move(adjustedPath);
                d->NavAgent->PathCursor = 0;
                d->NavAgent->PathFailCount = 0; // Reset on success
                d->NavAgent->PathRetryTimer = 0.0f;
                if (kNavVerbose)
                    std::cout << "[Nav] Path found for agent " << agentEntity 
                              << ": " << result.path.points.size() << " waypoints, " 
                              << result.computeTimeMs << "ms" << std::endl;
            } else {
                d->NavAgent->PathFailCount++;
                d->NavAgent->PathRetryTimer = NavAgentComponent::kRetryDelay;
                if (kNavVerbose && d->NavAgent->PathFailCount <= 3)
                    std::cout << "[Nav] Path FAILED for agent " << agentEntity 
                              << " (attempt " << d->NavAgent->PathFailCount << ")" << std::endl;
            }
            d->NavAgent->PathRequested = false;
        }
    }
    }
    
    const uint64_t sceneRevision = scene.GetDirtyRevision();
    bool requiresGraphRefresh = m_GraphMeshes.empty() || (sceneRevision != m_LastObservedSceneRevision);
    if (!requiresGraphRefresh) {
        for (auto it = m_ObservedStreamRevisions.begin(); it != m_ObservedStreamRevisions.end(); ++it) {
            auto* meshData = scene.GetEntityData(it->first);
            if (!meshData || !meshData->Navigation || !meshData->Navigation->ChunkManager) {
                requiresGraphRefresh = true;
                break;
            }
            const uint64_t currentRevision = meshData->Navigation->ChunkManager->GetStreamRevision();
            if (currentRevision != it->second) {
                requiresGraphRefresh = true;
                break;
            }
        }
    }
    if (requiresGraphRefresh) {
        ScopedTimer t("Nav/BuildGraph");
        BuildNavMeshGraph(scene, false);
        m_LastObservedSceneRevision = sceneRevision;
    }
    const bool multiMeshActive = (m_GraphMeshes.size() > 1);

    // Collect agents that need path requests
    struct AgentUpdate {
        EntityID entityId;
        EntityID navMeshEntity;
        glm::vec3 position;
        glm::vec3 destination;
        NavAgentParams params;
        bool needsPath;
        NavAgentComponent* agent = nullptr;
    };
    std::vector<AgentUpdate> agentUpdates;
    agentUpdates.reserve(64); // Pre-allocate for typical agent count
    
    { ScopedTimer t("Nav/AgentCollect");
    // First pass: collect agent data (minimal locking)
    for (EntityID agentEntity : m_CachedNavAgentEntities) {
        ++entitiesScanned;
        auto* d = scene.GetEntityData(agentEntity); 
        if (!d || !d->NavAgent || !d->NavAgent->Enabled) continue;
        const EntityID prefabPerfRootId = collectDetailedPrefabPerf
            ? cm::debug::ResolveOwningPrefabRoot(scene, agentEntity)
            : INVALID_ENTITY_ID;
        const bool collectPrefabPerfForAgent =
            collectDetailedPrefabPerf && prefabPerfRootId != INVALID_ENTITY_ID;
        const auto prefabPerfAgentStart = collectPrefabPerfForAgent
            ? std::chrono::high_resolution_clock::now()
            : std::chrono::high_resolution_clock::time_point{};
        ++agentsCollected;
        
        NavAgentComponent& agent = *d->NavAgent;
        ::TransformComponent& tr = d->Transform;
        glm::vec3 position = glm::vec3(tr.WorldMatrix[3]);
        
        AgentUpdate update;
        update.entityId = agentEntity;
        update.position = position;
        update.needsPath = false;
        update.agent = &agent;
        
        auto findNearestNavMesh = [&](const glm::vec3& pos, float range, float maxYDelta, EntityID& outId) -> bool {
            float bestDist2 = FLT_MAX;
            EntityID best = 0;
            for (const auto& m : m_GraphMeshes) {
                glm::vec3 on;
                if (!NearestPlacedPoint(*m.runtime, m.agentPlacementOffset, pos, range, on)) continue;
                float dy = std::abs(on.y - pos.y);
                if (dy > maxYDelta) continue;
                float dx = on.x - pos.x;
                float dz = on.z - pos.z;
                float d2 = dx * dx + dz * dz;
                if (d2 < bestDist2) {
                    bestDist2 = d2;
                    best = m.entity;
                }
            }
            if (best != 0) {
                outId = best;
                return true;
            }
            return false;
        };

        // Auto-bind to navmesh (if not set OR if current reference is invalid)
        bool needsBinding = (agent.NavMeshEntity == 0);
        if (!needsBinding && agent.NavMeshEntity != 0) {
            // Check if current reference is still valid
            auto* meshData = scene.GetEntityData(agent.NavMeshEntity);
            if (!meshData || !meshData->Navigation) {
                if (kNavVerbose)
                    std::cout << "[Nav] Agent " << agentEntity << " has stale NavMeshEntity=" 
                              << agent.NavMeshEntity << ", re-binding...\n";
                agent.NavMeshEntity = 0;
                needsBinding = true;
            }
        }
        if (needsBinding) {
            // Find nearest navmesh
            EntityID best = 0;
            const float range = std::max(2.0f, agent.Params.height + agent.Params.maxStep);
            const float maxYDelta = std::max(0.5f, agent.Params.maxStep * 2.0f);
            if (multiMeshActive) {
                findNearestNavMesh(position, range, maxYDelta, best);
            } else {
                float bestDist2 = FLT_MAX;
                for (EntityID navEntityId : m_CachedNavMeshEntities) {
                    auto* d2 = scene.GetEntityData(navEntityId);
                    if (!d2 || !d2->Navigation) continue;
                    glm::vec3 c = (d2->Navigation->AABB.min + d2->Navigation->AABB.max) * 0.5f;
                    float dsq = glm::distance2(position, c);
                    if (dsq < bestDist2) { bestDist2 = dsq; best = navEntityId; }
                }
            }
            if (best != 0) {
                agent.NavMeshEntity = best;
                if (kNavVerbose)
                    std::cout << "[Nav] Agent " << agentEntity << " bound to NavMeshEntity=" << best << "\n";
            }
        } else if (multiMeshActive) {
            // Rebind if the agent has drifted onto a different mesh
            const float range = std::max(2.0f, agent.Params.height + agent.Params.maxStep);
            const float maxYDelta = std::max(0.5f, agent.Params.maxStep * 2.0f);
            EntityID best = agent.NavMeshEntity;
            if (findNearestNavMesh(position, range, maxYDelta, best) && best != agent.NavMeshEntity) {
                agent.NavMeshEntity = best;
            }
        }
        
        update.navMeshEntity = agent.NavMeshEntity;
        
        // Update retry timer
        if (agent.PathRetryTimer > 0.0f) {
            agent.PathRetryTimer -= dt;
        }
        if (agent.RepathTimer > 0.0f) {
            agent.RepathTimer -= dt;
        }

        float effectiveRepathInterval = std::max(0.0f, agent.RepathInterval);
        if (scene.m_IsPlaying) {
            const cm::npc::ScalabilityState& scalability = d->NpcScalability;
            if (scalability.Participates) {
                effectiveRepathInterval =
                    std::max(effectiveRepathInterval, scalability.NavigationRepathInterval);
            }
        }

        const bool canRetry =
            agent.PathFailCount < NavAgentComponent::kMaxRetries &&
            agent.PathRetryTimer <= 0.0f;
        const bool missingPath =
            agent.HasDestination &&
            !agent.HasPath() &&
            !agent.PathRequested &&
            canRetry;
        const bool needsPeriodicRepath =
            agent.AutoRepath &&
            agent.HasDestination &&
            agent.HasPath() &&
            !agent.PathRequested &&
            canRetry &&
            effectiveRepathInterval > 0.0f &&
            agent.RepathTimer <= 0.0f;

        if (missingPath || needsPeriodicRepath) {
            update.needsPath = true;
            ++needsPathCount;
            if (needsPeriodicRepath) {
                ++autoRepathCount;
            }
            if (collectPrefabPerfForAgent) {
                ++prefabNavPerf[prefabPerfRootId].NeedsPath;
            }
            update.destination = agent.Destination;
            update.params = agent.Params;
            agent.PathRequested = true;
            agent.RepathTimer = effectiveRepathInterval;
            if (m_PendingPathRequestEntities.insert(update.entityId).second) {
                PendingPathRequest req;
                req.entityId = update.entityId;
                req.navMeshEntity = agent.NavMeshEntity;
                req.position = update.position;
                req.destination = update.destination;
                req.params = update.params;
                m_PendingPathRequests.push_back(req);
            }
            if (kNavVerbose && agent.PathFailCount == 0) {
                std::cout << "[Nav] Agent " << update.entityId
                          << (needsPeriodicRepath ? " repathing to (" : " needs path to (")
                          << agent.Destination.x << "," << agent.Destination.y << "," << agent.Destination.z
                          << "), NavMeshEntity=" << agent.NavMeshEntity << std::endl;
            }
        }
        
        agentUpdates.push_back(update);
        if (collectPrefabPerfForAgent) {
            auto& sample = prefabNavPerf[prefabPerfRootId];
            sample.TotalMs += std::chrono::duration<double, std::milli>(
                std::chrono::high_resolution_clock::now() - prefabPerfAgentStart).count();
            ++sample.AgentsCollected;
        }
    }
    }
    
    // Submit budgeted path requests (async preferred; sync multi-mesh capped)
    { ScopedTimer t("Nav/Pathfinding");
    const bool useMultiMesh = (m_GraphMeshes.size() > 1);
    uint32_t totalProcessed = 0;
    uint32_t asyncProcessed = 0;
    uint32_t syncProcessed = 0;
    const size_t queueWindow = m_PendingPathRequests.size();
    for (size_t i = 0; i < queueWindow && totalProcessed < m_MaxPathRequestsPerFrame; ++i) {
        if (m_PendingPathRequests.empty()) break;
        PendingPathRequest req = m_PendingPathRequests.front();
        m_PendingPathRequests.pop_front();

        auto* d = scene.GetEntityData(req.entityId);
        if (!d || !d->NavAgent || !d->NavAgent->Enabled) {
            m_PendingPathRequestEntities.erase(req.entityId);
            continue;
        }

        NavAgentComponent& agent = *d->NavAgent;
        if (!agent.PathRequested || !agent.HasDestination) {
            agent.PathRequested = false;
            m_PendingPathRequestEntities.erase(req.entityId);
            continue;
        }

        req.navMeshEntity = agent.NavMeshEntity;
        req.position = glm::vec3(d->Transform.WorldMatrix[3]);
        req.destination = agent.Destination;
        req.params = agent.Params;

        if (useMultiMesh) {
            if (syncProcessed >= m_MaxSyncPathSolvesPerFrame) {
                m_PendingPathRequests.push_back(req);
                continue;
            }
            ++pathRequestsSync;
            const EntityID prefabPerfRootId = collectDetailedPrefabPerf
                ? cm::debug::ResolveOwningPrefabRoot(scene, req.entityId)
                : INVALID_ENTITY_ID;
            const auto prefabPerfSyncStart = (collectDetailedPrefabPerf && prefabPerfRootId != INVALID_ENTITY_ID)
                ? std::chrono::high_resolution_clock::now()
                : std::chrono::high_resolution_clock::time_point{};
            NavPath path;
            std::vector<EntityID> meshChain;
            bool success = FindPathAcrossMeshes(scene, req.position, req.destination,
                                                req.params, NavFlags{0}, NavFlags{0}, path, &meshChain, false);
            if (success && path.valid) {
                agent.CurrentPath = path;
                agent.PathCursor = 0;
                agent.PathFailCount = 0;
                agent.PathRetryTimer = 0.0f;
            } else {
                agent.PathFailCount++;
                agent.PathRetryTimer = NavAgentComponent::kRetryDelay;
            }
            agent.PathRequested = false;
            m_PendingPathRequestEntities.erase(req.entityId);
            if (collectDetailedPrefabPerf && prefabPerfRootId != INVALID_ENTITY_ID) {
                auto& sample = prefabNavPerf[prefabPerfRootId];
                sample.TotalMs += std::chrono::duration<double, std::milli>(
                    std::chrono::high_resolution_clock::now() - prefabPerfSyncStart).count();
                ++sample.SyncRequests;
            }
            ++syncProcessed;
            ++totalProcessed;
            continue;
        }

        if (asyncProcessed >= m_MaxAsyncPathRequestsPerFrame) {
            m_PendingPathRequests.push_back(req);
            continue;
        }

        auto* meshOwner = scene.GetEntityData(req.navMeshEntity);
        if (!meshOwner || !meshOwner->Navigation) {
            agent.PathRequested = false;
            m_PendingPathRequestEntities.erase(req.entityId);
            continue;
        }
        auto& navComp = *meshOwner->Navigation;
        if (!navComp.Runtime && !navComp.EnsureRuntimeLoaded(scene)) {
            agent.PathRequested = false;
            m_PendingPathRequestEntities.erase(req.entityId);
            continue;
        }

        glm::vec3 start = ToNavQuerySpace(req.position, navComp.AgentPlacementOffset);
        glm::vec3 end = ToNavQuerySpace(req.destination, navComp.AgentPlacementOffset);
        const float projectDist = (std::max)(1.0f, req.params.height + req.params.maxStep);
        glm::vec3 onMesh;
        if (navComp.Runtime->NearestPoint(start, projectDist, onMesh)) start = onMesh;
        if (navComp.Runtime->NearestPoint(end, projectDist, onMesh)) end = onMesh;

        GetPathfinder().RequestPathAsync(
            navComp.Runtime,
            start, end,
            req.params, NavFlags{0}, NavFlags{0},
            nullptr,
            static_cast<uint64_t>(req.entityId)
        );
        ++pathRequestsAsync;
        if (collectDetailedPrefabPerf) {
            const EntityID prefabPerfRootId =
                cm::debug::ResolveOwningPrefabRoot(scene, req.entityId);
            if (prefabPerfRootId != INVALID_ENTITY_ID) {
                ++prefabNavPerf[prefabPerfRootId].AsyncRequests;
            }
        }
        m_PendingPathRequestEntities.erase(req.entityId);
        ++asyncProcessed;
        ++totalProcessed;
    }
    profiler.SetCounter("Nav/PendingBudgetQueue", static_cast<uint64_t>(m_PendingPathRequests.size()));
    }
    
    // Second pass: update agent movement (can be parallelized)
    { ScopedTimer t("Nav/AgentMovement");
    for (const auto& collected : agentUpdates) {
        ++agentMoves;
        auto* d = scene.GetEntityData(collected.entityId);
        if (!d || !d->NavAgent || !d->NavAgent->Enabled) continue;
        const EntityID prefabPerfRootId = collectDetailedPrefabPerf
            ? cm::debug::ResolveOwningPrefabRoot(scene, collected.entityId)
            : INVALID_ENTITY_ID;
        const bool collectPrefabPerfForAgent =
            collectDetailedPrefabPerf && prefabPerfRootId != INVALID_ENTITY_ID;
        const auto prefabPerfMoveStart = collectPrefabPerfForAgent
            ? std::chrono::high_resolution_clock::now()
            : std::chrono::high_resolution_clock::time_point{};
        
        NavAgentComponent& agent = *d->NavAgent;
        ::TransformComponent& tr = d->Transform;
        glm::vec3 position = glm::vec3(tr.WorldMatrix[3]);
        
        glm::vec3 desiredVel{0};
        
        if (agent.HasPath()) {
            // Advance path cursor using progress-based rule (not just distance)
            // This prevents advancing when agent is "close enough" but hasn't actually passed the corner
            while (agent.PathCursor < agent.CurrentPath.points.size()) {
                bool isLastWaypoint = (agent.PathCursor == agent.CurrentPath.points.size() - 1);
                glm::vec3 waypoint = agent.CurrentPath.points[agent.PathCursor];
                
                // For final waypoint, use distance threshold
                if (isLastWaypoint) {
                    if (glm::distance(position, waypoint) < agent.ArriveThreshold) {
                        agent.PathCursor++;
                    } else {
                        break;
                    }
                } else {
                    // For intermediate waypoints, use progress-based advancement
                    // Check if agent has passed the waypoint plane (progressed past it along path direction)
                    glm::vec3 nextWaypoint = agent.CurrentPath.points[agent.PathCursor + 1];
                    glm::vec3 segmentDir = nextWaypoint - waypoint;
                    float segmentLen = glm::length(segmentDir);
                    
                    if (segmentLen > 1e-3f) {
                        segmentDir = segmentDir / segmentLen;
                        
                        // Project agent position onto the segment
                        glm::vec3 toAgent = position - waypoint;
                        float projection = glm::dot(toAgent, segmentDir);
                        
                        // Advance if: close enough AND has progressed past the waypoint plane
                        // Also check that waypoint is not behind agent relative to current path direction
                        float distToWaypoint = glm::length(toAgent);
                        float threshold = std::max(1.5f, agent.Params.radius * 3.0f);
                        
                        // Check if we've passed the waypoint (projection > 0) or are very close
                        bool hasProgressed = projection > -0.5f; // Small hysteresis: allow slight backtrack
                        bool isCloseEnough = distToWaypoint < threshold;
                        
                        if (isCloseEnough && hasProgressed) {
                            agent.PathCursor++;
                        } else {
                            break;
                        }
                    } else {
                        // Degenerate segment: just use distance
                        if (glm::distance(position, waypoint) < std::max(1.5f, agent.Params.radius * 3.0f)) {
                            agent.PathCursor++;
                        } else {
                            break;
                        }
                    }
                }
            }
            
            if (agent.PathCursor >= agent.CurrentPath.points.size()) {
                // Arrived at destination - keep the path valid!
                // This prevents immediate repathing when script calls SetDestination with similar dest
                desiredVel = glm::vec3(0);
                // Don't call Stop() - path stays valid, agent just stops moving
            } else {
                // Steer towards next waypoint
                glm::vec3 target = agent.CurrentPath.points[agent.PathCursor];
                glm::vec3 to = target - position;
                float dist = glm::length(to);
                
                if (dist > 1e-3f) {
                    glm::vec3 dir = to / dist;
                    float speed = agent.Params.maxSpeed;
                    
                    // Apply arrival slowdown when approaching final destination
                    if (agent.ArrivalSlowdownDist > 0.0f) {
                        float remainingDist = agent.RemainingDistance(position);
                        if (remainingDist < agent.ArrivalSlowdownDist) {
                            float slowdownFactor = remainingDist / agent.ArrivalSlowdownDist;
                            // Smooth slowdown curve (quadratic ease-out)
                            slowdownFactor = 1.0f - (1.0f - slowdownFactor) * (1.0f - slowdownFactor);
                            speed *= glm::max(0.1f, slowdownFactor); // Keep minimum 10% speed
                        }
                    }
                    
                    desiredVel = dir * speed;
                }
            }
        }
        
        // Apply acceleration-bounded velocity smoothing (not lerp-based)
        // This prevents overshoot and correction wobbles by enforcing physical acceleration limits
        glm::vec3 currentVel = agent.SmoothedVelocity;
        
        if (glm::length2(desiredVel) < 1e-6f) {
            // When stopping, decelerate smoothly using acceleration clamp
            glm::vec3 deltaVel = glm::vec3(0) - currentVel;
            float deltaLen = glm::length(deltaVel);
            if (deltaLen > 1e-6f) {
                float maxDelta = agent.Params.maxAccel * dt;
                if (deltaLen > maxDelta) {
                    deltaVel = (deltaVel / deltaLen) * maxDelta;
                }
                agent.SmoothedVelocity = currentVel + deltaVel;
            } else {
                agent.SmoothedVelocity = glm::vec3(0);
            }
        } else {
            // Compute desired velocity change
            glm::vec3 deltaVel = desiredVel - currentVel;
            float deltaLen = glm::length(deltaVel);
            
            if (deltaLen > 1e-6f) {
                // Clamp velocity change to max acceleration per frame
                float maxDelta = agent.Params.maxAccel * dt;
                if (deltaLen > maxDelta) {
                    deltaVel = (deltaVel / deltaLen) * maxDelta;
                }
                agent.SmoothedVelocity = currentVel + deltaVel;
            } else {
                agent.SmoothedVelocity = desiredVel;
            }
        }
        
        // Clamp to max speed
        float smoothedSpeed = glm::length(agent.SmoothedVelocity);
        if (smoothedSpeed > agent.Params.maxSpeed) {
            agent.SmoothedVelocity = (agent.SmoothedVelocity / smoothedSpeed) * agent.Params.maxSpeed;
        }
        
        // Apply movement using smoothed velocity
        glm::vec3 appliedVel = agent.SmoothedVelocity;
        NavMeshRuntime* runtime = nullptr;
        float agentPlacementOffset = 0.0f;
        if (agent.NavMeshEntity != 0) {
            auto* meshData = scene.GetEntityData(agent.NavMeshEntity);
            if (meshData && meshData->Navigation) {
                auto& navComp = *meshData->Navigation;
                if (navComp.Runtime || navComp.EnsureRuntimeLoaded(scene)) {
                    runtime = navComp.Runtime.get();
                    agentPlacementOffset = navComp.AgentPlacementOffset;
                }
            }
        }
        if (runtime && dt > 0.0f) {
            const float snapRange = std::max(1.0f, agent.Params.height + agent.Params.maxStep);
            const float maxXZSnap = std::max(0.25f, agent.Params.radius * 2.0f);
            const float maxYSnap = std::max(0.5f, agent.Params.maxStep * 2.0f);
            const glm::vec3 predictedPos = position + glm::vec3(appliedVel.x, 0.0f, appliedVel.z) * dt;
            glm::vec3 currentOnMesh;
            const bool hasCurrent = NearestPlacedPoint(*runtime, agentPlacementOffset, position, snapRange, currentOnMesh);
            glm::vec3 predictedOnMesh;
            if (NearestPlacedPoint(*runtime, agentPlacementOffset, predictedPos, snapRange, predictedOnMesh)) {
                const float dxz = glm::length(glm::vec2(predictedOnMesh.x - predictedPos.x,
                                                        predictedOnMesh.z - predictedPos.z));
                const float dy = hasCurrent ? std::abs(predictedOnMesh.y - currentOnMesh.y) : 0.0f;
                if (dxz <= maxXZSnap && (!hasCurrent || dy <= maxYSnap)) {
                    glm::vec3 desiredPos = predictedOnMesh;
                    glm::vec3 desiredVel = (desiredPos - position) / dt;
                    const float maxSpeed = std::max(1.0f, agent.Params.maxSpeed);
                    desiredVel.x = glm::clamp(desiredVel.x, -maxSpeed, maxSpeed);
                    desiredVel.y = glm::clamp(desiredVel.y, -maxSpeed, maxSpeed);
                    desiredVel.z = glm::clamp(desiredVel.z, -maxSpeed, maxSpeed);
                    appliedVel = desiredVel;
                    agent.SmoothedVelocity = appliedVel;
                } else if (hasCurrent) {
                    const float desiredYVel = (currentOnMesh.y - position.y) / dt;
                    const float maxYSpeed = std::max(1.0f, agent.Params.maxSpeed);
                    appliedVel.y = glm::clamp(desiredYVel, -maxYSpeed, maxYSpeed);
                    agent.SmoothedVelocity.y = appliedVel.y;
                }
            } else if (hasCurrent) {
                const float desiredYVel = (currentOnMesh.y - position.y) / dt;
                const float maxYSpeed = std::max(1.0f, agent.Params.maxSpeed);
                appliedVel.y = glm::clamp(desiredYVel, -maxYSpeed, maxYSpeed);
                agent.SmoothedVelocity.y = appliedVel.y;
            }
        }

        // Store current velocity for managed code access (use final applied velocity)
        agent.CurrentVelocity = appliedVel;
        if (d->RigidBody && !d->RigidBody->BodyID.IsInvalid()) {
            if (d->RigidBody->IsKinematic) {
                d->RigidBody->LinearVelocity = appliedVel;
            } else {
                ::Physics::Get().SetBodyLinearVelocity(d->RigidBody->BodyID, appliedVel);
            }
        } else {
            tr.Position += appliedVel * dt;
            scene.MarkTransformDirty(collected.entityId);
        }
        
        // Debug draw (only in editor mode)
        debug::DrawPath(agent.CurrentPath, 0);
        debug::DrawAgent(agent, position, desiredVel, 0);
        if (collectPrefabPerfForAgent) {
            auto& sample = prefabNavPerf[prefabPerfRootId];
            sample.TotalMs += std::chrono::duration<double, std::milli>(
                std::chrono::high_resolution_clock::now() - prefabPerfMoveStart).count();
            ++sample.AgentMoves;
        }
    }
    }

    profiler.SetCounter("Nav/EntitiesScanned", entitiesScanned);
    profiler.SetCounter("Nav/AgentsCollected", agentsCollected);
    profiler.SetCounter("Nav/NeedsPath", needsPathCount);
    profiler.SetCounter("Nav/AutoRepath", autoRepathCount);
    profiler.SetCounter("Nav/PathRequestsAsync", pathRequestsAsync);
    profiler.SetCounter("Nav/PathRequestsSync", pathRequestsSync);
    profiler.SetCounter("Nav/PathResultsProcessed", pathResultsProcessed);
    profiler.SetCounter("Nav/PathResultsStale", pathResultsStale);
    profiler.SetCounter("Nav/AgentMoves", agentMoves);
    profiler.SetCounter("Nav/PrefabRootsActive", static_cast<uint64_t>(prefabNavPerf.size()));
    if (collectDetailedPrefabPerf) {
        for (const auto& [prefabRootId, sample] : prefabNavPerf) {
            const auto label = cm::debug::DescribePrefabRoot(scene, prefabRootId);
            if (!label.IsValid()) {
                continue;
            }
            profiler.Record(cm::debug::MakePrefabProfilerSection("Nav/Prefab", label), sample.TotalMs);
        }

        if (cm::debug::PrefabPerfConsoleLoggingEnabled()) {
            static uint64_t s_PrefabNavLogFrame = 0;
            const uint32_t logInterval = cm::debug::PrefabPerfConsoleLogInterval();
            ++s_PrefabNavLogFrame;
            if (logInterval > 0 && (s_PrefabNavLogFrame % logInterval) == 0u && !prefabNavPerf.empty()) {
                std::vector<std::pair<EntityID, const PrefabNavPerfSample*>> ordered;
                ordered.reserve(prefabNavPerf.size());
                for (const auto& entry : prefabNavPerf) {
                    ordered.emplace_back(entry.first, &entry.second);
                }
                std::sort(ordered.begin(), ordered.end(),
                    [](const auto& lhs, const auto& rhs) {
                        return lhs.second->TotalMs > rhs.second->TotalMs;
                    });

                const size_t limit = std::min<size_t>(3, ordered.size());
                std::cout << "[PrefabPerf][Nav] Top prefab roots this frame:" << std::endl;
                for (size_t i = 0; i < limit; ++i) {
                    const EntityID prefabRootId = ordered[i].first;
                    const PrefabNavPerfSample& sample = *ordered[i].second;
                    const auto label = cm::debug::DescribePrefabRoot(scene, prefabRootId);
                    std::cout << "[PrefabPerf][Nav]   " << (i + 1) << ". "
                        << cm::debug::MakePrefabDebugLabel(label)
                        << " total=" << sample.TotalMs << "ms"
                        << " agents=" << sample.AgentsCollected
                        << " moves=" << sample.AgentMoves
                        << " needsPath=" << sample.NeedsPath
                        << " async=" << sample.AsyncRequests
                        << " sync=" << sample.SyncRequests
                        << " results=" << sample.PathResults
                        << std::endl;
                }
            }
        }
    }

    // Draw navmesh runtime when debug is enabled (editor only)
    if (!scene.m_IsPlaying && (uint32_t)m_DebugMask != 0) {
        // Get camera position for distance culling. Prefer active camera, then editor viewport.
        glm::vec3 cameraPos(0.0f);
        bool haveCameraPos = false;
        for (const auto& e : scene.GetEntities()) {
            auto* d = scene.GetEntityData(e.GetID());
            if (d && d->Camera && d->Camera->Active) {
                cameraPos = glm::vec3(d->Transform.WorldMatrix[3]);
                haveCameraPos = true;
                break;
            }
        }
        if (!haveCameraPos && scene.HasEditorViewportState()) {
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
            cameraPos = s.Target - forward * std::max(0.01f, s.Distance);
        }
        
        for (const auto& e : scene.GetEntities()) {
            auto* d = scene.GetEntityData(e.GetID()); if (!d || !d->Navigation) continue;
            auto& comp = *d->Navigation;
            if (comp.ChunkedNavEnabled && comp.ChunkManager) {
                if (!comp.EnsureRuntimeLoaded(scene)) {
                    continue;
                }
                std::vector<NavChunkManager::LoadedChunkRuntime> loadedChunks;
                comp.ChunkManager->GetLoadedChunkRuntimes(loadedChunks);
                if (loadedChunks.empty()) {
                    continue;
                }
                debug::SetOffset(comp.DebugDrawOffset);
                for (const auto& chunk : loadedChunks) {
                    if (!chunk.runtime) continue;
                    debug::DrawRuntime(*chunk.runtime, cameraPos, 3);
                }
                continue;
            }

            auto runtime = std::atomic_load(&comp.Runtime);
            if (!runtime && comp.EnsureRuntimeLoaded(scene)) {
                runtime = std::atomic_load(&comp.Runtime);
            }
            if (runtime) {
                debug::SetOffset(comp.DebugDrawOffset);
                debug::DrawRuntime(*runtime, cameraPos, 3); // View 3 = debug overlay
            }
        }

        const uint32_t mask = (uint32_t)m_DebugMask;
        if (mask & (uint32_t)NavDrawMask::Links) {
            BuildNavMeshGraph(scene, true);
            std::vector<std::pair<glm::vec3, glm::vec3>> portalLines;
            for (uint32_t i = 0; i < m_GraphPortals.size(); ++i) {
                for (const auto& p : m_GraphPortals[i]) {
                    if (p.from < p.to) {
                        portalLines.push_back({ p.fromPos, p.toPos });
                    }
                }
            }
            for (const auto& mesh : m_GraphMeshes) {
                if (!mesh.runtime) continue;
                for (const auto& link : mesh.runtime->m_Links) {
                    portalLines.push_back({ link.a, link.b });
                }
                for (const auto& link : mesh.runtime->m_ExternalLinks) {
                    portalLines.push_back({ link.a, link.b });
                }
            }
            if (!portalLines.empty()) {
                debug::DrawPortals(portalLines, 3);
            }
        }
    }
}


