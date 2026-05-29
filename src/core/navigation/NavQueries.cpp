#include "core/navigation/NavQueries.h"
#include "core/navigation/NavMesh.h"

#include <DetourNavMeshQuery.h>
#include <DetourStatus.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <thread>

using namespace nav;

namespace nav::queries
{
    static void LogDetourStatus(const char* stage, dtStatus status)
    {
        std::cerr << "[Nav][PathDiag] " << stage
                  << " status=0x" << std::hex << (unsigned int)status << std::dec
                  << " succeed=" << (dtStatusSucceed(status) ? "yes" : "no")
                  << " failed=" << (dtStatusFailed(status) ? "yes" : "no")
                  << " inProgress=" << (dtStatusInProgress(status) ? "yes" : "no");
#if defined(DT_PARTIAL_RESULT)
        if (dtStatusDetail(status, DT_PARTIAL_RESULT)) std::cerr << " detail=partial-result";
#endif
#if defined(DT_BUFFER_TOO_SMALL)
        if (dtStatusDetail(status, DT_BUFFER_TOO_SMALL)) std::cerr << " detail=buffer-too-small";
#endif
#if defined(DT_OUT_OF_NODES)
        if (dtStatusDetail(status, DT_OUT_OF_NODES)) std::cerr << " detail=out-of-nodes";
#endif
        std::cerr << std::endl;
    }

    // Startup-stable RNG for Detour callbacks.
    // Avoid std::random_device/std::mt19937 initialization here because this code can run
    // on worker threads very early in app startup.
    static std::atomic<uint64_t> g_DetourSeedCounter{ 0x8f4d2a6bc31e9475ull };
    static thread_local uint64_t g_DetourRngState = 0ull;

    static uint64_t NextDetourRandU64()
    {
        uint64_t state = g_DetourRngState;
        if (state == 0ull) {
            const uint64_t counter = g_DetourSeedCounter.fetch_add(0x9e3779b97f4a7c15ull, std::memory_order_relaxed);
            const uint64_t tidHash = static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
            state = counter ^ (tidHash + 0x9e3779b97f4a7c15ull);
            if (state == 0ull) {
                state = 0x4d595df4d0f33173ull;
            }
        }
        // xorshift64*
        state ^= (state >> 12);
        state ^= (state << 25);
        state ^= (state >> 27);
        g_DetourRngState = state;
        return state * 2685821657736338717ull;
    }

    static float DetourFrand()
    {
        // Convert high bits to [0, 1).
        constexpr float kInv2Pow24 = 1.0f / 16777216.0f;
        const uint32_t bits24 = static_cast<uint32_t>((NextDetourRandU64() >> 40) & 0xFFFFFFu);
        return static_cast<float>(bits24) * kInv2Pow24;
    }

    static bool BuildFilter(const NavMeshRuntime& nm, NavFlags include, NavFlags exclude, dtQueryFilter& out)
    {
        if (!nm.m_DetourQuery || !nm.m_DetourMesh) return false;
        out.setIncludeFlags(include.mask == 0 ? 0xFFFFu : (unsigned short)include.mask);
        out.setExcludeFlags((unsigned short)exclude.mask);
        for (int i = 0; i < DT_MAX_AREAS; ++i) {
            float cost = 1.0f;
            if ((size_t)i < nm.m_AreaCost.size()) {
                cost = std::max(0.01f, nm.m_AreaCost[(size_t)i]);
            }
            out.setAreaCost(i, cost);
        }
        return true;
    }

    static bool FindNearest(const NavMeshRuntime& nm, const glm::vec3& pos, const NavAgentParams& params,
                            const dtQueryFilter& filter, dtPolyRef& outRef, float outPos[3],
                            dtStatus* outStatus = nullptr, float outExt[3] = nullptr)
    {
        const float ext[3] = {
            std::max(1.0f, params.radius * 4.0f),
            std::max(2.0f, params.height),
            std::max(1.0f, params.radius * 4.0f)
        };
        if (outExt) {
            outExt[0] = ext[0];
            outExt[1] = ext[1];
            outExt[2] = ext[2];
        }
        const float p[3] = { pos.x, pos.y, pos.z };
        dtStatus s = nm.m_DetourQuery->findNearestPoly(p, ext, &filter, &outRef, outPos);
        if (outStatus) {
            *outStatus = s;
        }
        return dtStatusSucceed(s) && outRef != 0;
    }

    bool FindPath(const NavMeshRuntime& nm, const glm::vec3& start, const glm::vec3& end,
                  const NavAgentParams& params, NavFlags include, NavFlags exclude, NavPath& out)
    {
        out.points.clear();
        out.valid = false;
        if (!nm.HasDetour()) {
            std::cerr << "[Nav][PathDiag] FindPath aborted: runtime has no Detour navmesh/query." << std::endl;
            return false;
        }

        dtQueryFilter filter;
        if (!BuildFilter(nm, include, exclude, filter)) {
            std::cerr << "[Nav][PathDiag] BuildFilter failed for request start=("
                      << start.x << "," << start.y << "," << start.z << ") end=("
                      << end.x << "," << end.y << "," << end.z << ") include=0x"
                      << std::hex << include.mask << " exclude=0x" << exclude.mask << std::dec
                      << std::endl;
            return false;
        }

        dtPolyRef startRef = 0;
        dtPolyRef endRef = 0;
        float startNearest[3] = {};
        float endNearest[3] = {};
        float startExt[3] = {};
        float endExt[3] = {};
        dtStatus startNearestStatus = 0;
        dtStatus endNearestStatus = 0;
        if (!FindNearest(nm, start, params, filter, startRef, startNearest, &startNearestStatus, startExt)) {
            std::cerr << "[Nav][PathDiag] findNearest(start) failed: start=("
                      << start.x << "," << start.y << "," << start.z << ") ext=("
                      << startExt[0] << "," << startExt[1] << "," << startExt[2] << ")"
                      << " startRef=" << (uint64_t)startRef
                      << " include=0x" << std::hex << include.mask
                      << " exclude=0x" << exclude.mask << std::dec
                      << std::endl;
            LogDetourStatus("findNearest(start)", startNearestStatus);
            return false;
        }
        if (!FindNearest(nm, end, params, filter, endRef, endNearest, &endNearestStatus, endExt)) {
            std::cerr << "[Nav][PathDiag] findNearest(end) failed: end=("
                      << end.x << "," << end.y << "," << end.z << ") ext=("
                      << endExt[0] << "," << endExt[1] << "," << endExt[2] << ")"
                      << " endRef=" << (uint64_t)endRef
                      << " startNearest=(" << startNearest[0] << "," << startNearest[1] << "," << startNearest[2] << ")"
                      << " include=0x" << std::hex << include.mask
                      << " exclude=0x" << exclude.mask << std::dec
                      << std::endl;
            LogDetourStatus("findNearest(end)", endNearestStatus);
            return false;
        }

        constexpr int kMaxPolys = 8192;
        std::array<dtPolyRef, kMaxPolys> polyPath{};
        int polyCount = 0;

        const float startPos[3] = { start.x, start.y, start.z };
        const float endPos[3] = { end.x, end.y, end.z };
        dtStatus pathStatus = nm.m_DetourQuery->findPath(
            startRef, endRef, startPos, endPos, &filter, polyPath.data(), &polyCount, kMaxPolys
        );
        if (!dtStatusSucceed(pathStatus) || polyCount <= 0) {
            std::cerr << "[Nav][PathDiag] findPath failed: startRef=" << (uint64_t)startRef
                      << " endRef=" << (uint64_t)endRef
                      << " polyCount=" << polyCount
                      << " startNearest=(" << startNearest[0] << "," << startNearest[1] << "," << startNearest[2] << ")"
                      << " endNearest=(" << endNearest[0] << "," << endNearest[1] << "," << endNearest[2] << ")"
                      << std::endl;
            LogDetourStatus("findPath", pathStatus);
            return false;
        }

        constexpr int kMaxStraight = 8192;
        std::array<float, kMaxStraight * 3> straightPath{};
        std::array<unsigned char, kMaxStraight> straightFlags{};
        std::array<dtPolyRef, kMaxStraight> straightRefs{};
        int straightCount = 0;

        dtStatus straightStatus = nm.m_DetourQuery->findStraightPath(
            startPos, endPos, polyPath.data(), polyCount,
            straightPath.data(), straightFlags.data(), straightRefs.data(), &straightCount, kMaxStraight
        );
        if (!dtStatusSucceed(straightStatus) || straightCount <= 0) {
            std::cerr << "[Nav][PathDiag] findStraightPath failed: polyCount=" << polyCount
                      << " straightCount=" << straightCount
                      << " startRef=" << (uint64_t)startRef
                      << " endRef=" << (uint64_t)endRef
                      << std::endl;
            LogDetourStatus("findStraightPath", straightStatus);
            return false;
        }

        out.points.reserve((size_t)straightCount);
        for (int i = 0; i < straightCount; ++i) {
            const float* p = &straightPath[(size_t)i * 3];
            out.points.emplace_back(p[0], p[1], p[2]);
        }
        out.valid = out.points.size() >= 2;
        if (!out.valid) {
            std::cerr << "[Nav][PathDiag] Path rejected: straight path has " << out.points.size()
                      << " point(s), need at least 2." << std::endl;
        }
        return out.valid;
    }

    bool RaycastPolyMesh(const NavMeshRuntime& nm, const glm::vec3& start, const glm::vec3& end,
                         float& tHit, glm::vec3& hitNormal)
    {
        tHit = 1.0f;
        hitNormal = glm::vec3(0.0f, 1.0f, 0.0f);
        if (!nm.HasDetour()) return false;

        dtQueryFilter filter;
        if (!BuildFilter(nm, NavFlags{0xFFFFFFFFu}, NavFlags{0u}, filter)) return false;

        dtPolyRef startRef = 0;
        float nearest[3] = {};
        if (!FindNearest(nm, start, NavAgentParams{}, filter, startRef, nearest)) return false;

        const float s[3] = { start.x, start.y, start.z };
        const float e[3] = { end.x, end.y, end.z };
        float t = 0.0f;
        float nrm[3] = {0.0f, 1.0f, 0.0f};
        std::array<dtPolyRef, 256> visited{};
        int visitedCount = 0;
        dtStatus status = nm.m_DetourQuery->raycast(
            startRef, s, e, &filter, &t, nrm, visited.data(), &visitedCount, (int)visited.size()
        );
        if (!dtStatusSucceed(status)) return false;

        if (t > 1.0f) {
            // Reached end with no wall hit.
            return false;
        }

        tHit = std::clamp(t, 0.0f, 1.0f);
        hitNormal = glm::vec3(nrm[0], nrm[1], nrm[2]);
        return true;
    }

    bool NearestPointOnNavmesh(const NavMeshRuntime& nm, const glm::vec3& pos, float maxDist,
                               glm::vec3& outOnMesh)
    {
        if (!nm.HasDetour()) return false;

        dtQueryFilter filter;
        if (!BuildFilter(nm, NavFlags{0xFFFFFFFFu}, NavFlags{0u}, filter)) return false;

        const float ext[3] = { maxDist, std::max(2.0f, maxDist), maxDist };
        const float p[3] = { pos.x, pos.y, pos.z };
        dtPolyRef ref = 0;
        float closest[3] = {};
        dtStatus nearest = nm.m_DetourQuery->findNearestPoly(p, ext, &filter, &ref, closest);
        if (!dtStatusSucceed(nearest) || ref == 0) return false;

        float clamped[3] = {};
        dtStatus cp = nm.m_DetourQuery->closestPointOnPoly(ref, p, clamped, nullptr);
        if (!dtStatusSucceed(cp)) return false;

        outOnMesh = glm::vec3(clamped[0], clamped[1], clamped[2]);
        return true;
    }

    bool RandomPointInRadius(const NavMeshRuntime& nm, const glm::vec3& pos, float r,
                             glm::vec3& out)
    {
        if (!nm.HasDetour()) return false;
        dtQueryFilter filter;
        if (!BuildFilter(nm, NavFlags{0xFFFFFFFFu}, NavFlags{0u}, filter)) return false;

        dtPolyRef centerRef = 0;
        float nearest[3] = {};
        if (!FindNearest(nm, pos, NavAgentParams{}, filter, centerRef, nearest)) return false;

        dtPolyRef randomRef = 0;
        float randomPos[3] = {};
        dtStatus s = nm.m_DetourQuery->findRandomPointAroundCircle(
            centerRef, nearest, r, &filter, DetourFrand, &randomRef, randomPos
        );
        if (!dtStatusSucceed(s) || randomRef == 0) return false;
        out = glm::vec3(randomPos[0], randomPos[1], randomPos[2]);
        return true;
    }
}
