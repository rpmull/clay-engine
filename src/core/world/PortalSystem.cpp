#include "PortalSystem.h"
#include "PortalInterop.h"
#include "WorldGraph.h"
#include "core/ecs/EntityData.h"
#include "core/vfs/VirtualFS.h"
#include "core/rendering/Renderer.h"
#include "core/utils/Profiler.h"
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <algorithm>
#include <unordered_set>

namespace cm::world {

static glm::vec3 GetWorldPosition(const TransformComponent& t, const glm::vec3& localOffset)
{
    glm::vec4 world = t.WorldMatrix * glm::vec4(localOffset, 1.0f);
    return glm::vec3(world.x, world.y, world.z);
}

static void DrawPortalDebugSphere(const glm::vec3& center, float radius, uint32_t abgrColor)
{
    if (radius <= 0.0f) radius = 0.25f;
    Renderer::Get().DrawRing(center, glm::vec3(1.0f, 0.0f, 0.0f), radius, abgrColor);
    Renderer::Get().DrawRing(center, glm::vec3(0.0f, 1.0f, 0.0f), radius, abgrColor);
    Renderer::Get().DrawRing(center, glm::vec3(0.0f, 0.0f, 1.0f), radius, abgrColor);
}

static std::string BuildEntityPath(Scene& scene, EntityID id)
{
    std::vector<std::string> parts;
    EntityID cur = id;
    while (cur != INVALID_ENTITY_ID) {
        auto* data = scene.GetEntityData(cur);
        if (!data) break;
        parts.push_back(data->Name);
        cur = data->Parent;
    }
    std::reverse(parts.begin(), parts.end());
    std::string path;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) path += "/";
        path += parts[i];
    }
    return path;
}

void PortalSystem::Update(Scene& scene, float /*dt*/)
{
    if (!scene.m_IsPlaying) return;

    auto& graph = cm::world::WorldGraph::Get();
    graph.BeginRuntimeUpdate();

    static thread_local std::vector<EntityID> s_portals;
    static thread_local std::vector<EntityID> s_candidates;
    s_portals.clear();
    s_candidates.clear();
    bool needsCandidateOverlapChecks = false;

    for (const auto& e : scene.GetEntities()) {
        auto* d = scene.GetEntityData(e.GetID());
        if (!d || !d->Active) continue;
        if (d->Portal) {
            s_portals.push_back(e.GetID());
            if (d->Portal->Enabled && d->Portal->AutoDetect && d->Portal->TriggerRadius > 0.0f) {
                needsCandidateOverlapChecks = true;
            }
        }
        if (d->NavAgent || d->CharacterController) {
            s_candidates.push_back(e.GetID());
        }
    }

    if (s_portals.empty()) {
        auto& profiler = Profiler::Get();
        profiler.SetCounter("Portals/Portals", 0);
        profiler.SetCounter("Portals/Candidates", 0);
        profiler.SetCounter("Portals/OverlapChecks", 0);
        graph.FinalizeRuntimeUpdate();
        return;
    }

    if (!needsCandidateOverlapChecks) {
        s_candidates.clear();
    }

    uint64_t overlapChecks = 0;
    const bool drawPortalDebug = Renderer::Get().GetDebugDrawInPlayMode();

    for (EntityID portalEntityId : s_portals) {
        auto* d = scene.GetEntityData(portalEntityId);
        if (!d || !d->Active || !d->Portal) continue;

        auto& portal = *d->Portal;
        if (!portal.Enabled) {
            portal.Overlapping.clear();
            continue;
        }

        WorldGraphPortal runtimePortal;
        runtimePortal.ScenePath = std::string();
        runtimePortal.PortalGuid = d->EntityGuid;
        runtimePortal.PortalPath = BuildEntityPath(scene, portalEntityId);
        runtimePortal.EntryPosition = GetWorldPosition(d->Transform, portal.EntryOffset);
        runtimePortal.TargetScenePath = portal.TargetScenePath;
        if (!runtimePortal.TargetScenePath.empty()) {
            try { runtimePortal.TargetScenePath = IVirtualFS::NormalizePath(runtimePortal.TargetScenePath); } catch(...) {}
        }
        runtimePortal.TargetPortalGuid = portal.TargetPortalGuid;
        runtimePortal.TargetPortalPath = portal.TargetPortalPath;
        runtimePortal.ExitPosition = GetWorldPosition(d->Transform, portal.ExitOffset);
        graph.UpsertRuntimePortal(std::move(runtimePortal));

        if (!portal.AutoDetect || portal.TriggerRadius <= 0.0f) {
            if (drawPortalDebug) {
                DrawPortalDebugSphere(GetWorldPosition(d->Transform, portal.EntryOffset), portal.TriggerRadius, 0xFFFF00FF);
            }
            portal.Overlapping.clear();
            continue;
        }

        const glm::vec3 portalPos = GetWorldPosition(d->Transform, portal.EntryOffset);
        const float radius = std::max(0.0f, portal.TriggerRadius);
        const float radiusSq = radius * radius;
        if (drawPortalDebug) {
            DrawPortalDebugSphere(portalPos, radius, 0xFF00FF00);
        }

        std::unordered_set<EntityID> current;
        current.reserve(portal.Overlapping.size());

        for (EntityID agentId : s_candidates) {
            if (agentId == portalEntityId) continue;
            ++overlapChecks;
            auto* agentData = scene.GetEntityData(agentId);
            if (!agentData || !agentData->Active) continue;

            const glm::vec3 agentPos = glm::vec3(agentData->Transform.WorldMatrix[3]);
            const glm::vec3 delta = agentPos - portalPos;
            const float distSq = glm::dot(delta, delta);
            if (distSq <= radiusSq) {
                current.insert(agentId);
                if (portal.Overlapping.find(agentId) == portal.Overlapping.end()) {
                    PortalInterop_Dispatch((int)portalEntityId, (int)agentId, 1);
                }
            }
        }

        if (portal.FireExitEvents) {
            for (EntityID prev : portal.Overlapping) {
                if (current.find(prev) == current.end()) {
                    PortalInterop_Dispatch((int)portalEntityId, (int)prev, 0);
                }
            }
        }

        portal.Overlapping = std::move(current);
    }

    graph.FinalizeRuntimeUpdate();
    auto& profiler = Profiler::Get();
    profiler.SetCounter("Portals/Portals", static_cast<uint64_t>(s_portals.size()));
    profiler.SetCounter("Portals/Candidates", static_cast<uint64_t>(s_candidates.size()));
    profiler.SetCounter("Portals/OverlapChecks", overlapChecks);
}

void PortalSystem::RebuildRuntimePortals(Scene& scene)
{
    scene.UpdateTransforms();

    auto& graph = cm::world::WorldGraph::Get();
    graph.BeginRuntimeUpdate();

    for (const auto& e : scene.GetEntities()) {
        auto* d = scene.GetEntityData(e.GetID());
        if (!d || !d->Active || !d->Portal) continue;

        auto& portal = *d->Portal;
        if (!portal.Enabled) continue;

        WorldGraphPortal runtimePortal;
        runtimePortal.ScenePath = std::string();
        runtimePortal.PortalGuid = d->EntityGuid;
        runtimePortal.PortalPath = BuildEntityPath(scene, e.GetID());
        runtimePortal.EntryPosition = GetWorldPosition(d->Transform, portal.EntryOffset);
        runtimePortal.TargetScenePath = portal.TargetScenePath;
        if (!runtimePortal.TargetScenePath.empty()) {
            try { runtimePortal.TargetScenePath = IVirtualFS::NormalizePath(runtimePortal.TargetScenePath); } catch(...) {}
        }
        runtimePortal.TargetPortalGuid = portal.TargetPortalGuid;
        runtimePortal.TargetPortalPath = portal.TargetPortalPath;
        runtimePortal.ExitPosition = GetWorldPosition(d->Transform, portal.ExitOffset);
        graph.UpsertRuntimePortal(std::move(runtimePortal));
    }

    graph.FinalizeRuntimeUpdate();
}

} // namespace cm::world
