#include "NpcScalability.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <sstream>
#include <vector>

#include "EntityData.h"
#include "Scene.h"
#include "core/animation/AnimationPlayerComponent.h"
#include "core/rendering/Environment.h"
#include "core/utils/Profiler.h"
#include "core/world/RuntimeWorld.h"

namespace cm::npc {
namespace {

struct LodPlane {
    glm::vec4 P{ 0.0f };
};

struct LodFrustum {
    std::array<LodPlane, 6> Planes{};
};

struct DistanceBands {
    float NearSq = 20.0f * 20.0f;
    float MediumSq = 50.0f * 50.0f;
    float FarSq = 100.0f * 100.0f;
};

struct CrowdBudget {
    uint32_t FullRateCount = 0;
    uint32_t ThirtyHzCount = 0;
    uint32_t FifteenHzCount = 0;
    float OverflowInterval = 0.10000000f;
};

struct CrowdCandidate {
    EntityID Entity = INVALID_ENTITY_ID;
    float DistSq = 0.0f;
};

LodFrustum BuildFrustum(const glm::mat4& view, const glm::mat4& proj)
{
    const glm::mat4 vp = proj * view;
    auto row = [&](int r) { return glm::vec4(vp[0][r], vp[1][r], vp[2][r], vp[3][r]); };
    const glm::vec4 r0 = row(0);
    const glm::vec4 r1 = row(1);
    const glm::vec4 r2 = row(2);
    const glm::vec4 r3 = row(3);

    LodFrustum frustum{};
    std::array<glm::vec4, 6> planes = {
        r3 + r0,
        r3 - r0,
        r3 + r1,
        r3 - r1,
        r3 + r2,
        r3 - r2
    };
    for (size_t i = 0; i < planes.size(); ++i) {
        const glm::vec3 n(planes[i]);
        const float len = glm::length(n);
        if (len > 1e-6f) {
            planes[i] /= len;
        }
        frustum.Planes[i].P = planes[i];
    }
    return frustum;
}

bool AabbVisible(const LodFrustum& frustum, const glm::vec3& worldMin, const glm::vec3& worldMax)
{
    for (const LodPlane& plane : frustum.Planes) {
        glm::vec3 p;
        p.x = (plane.P.x >= 0.0f) ? worldMax.x : worldMin.x;
        p.y = (plane.P.y >= 0.0f) ? worldMax.y : worldMin.y;
        p.z = (plane.P.z >= 0.0f) ? worldMax.z : worldMin.z;
        const float d = plane.P.x * p.x + plane.P.y * p.y + plane.P.z * p.z + plane.P.w;
        if (d < 0.0f) {
            return false;
        }
    }
    return true;
}

bool PointVisible(const LodFrustum& frustum, const glm::vec3& center, float radius)
{
    const glm::vec3 extents(radius);
    return AabbVisible(frustum, center - extents, center + extents);
}

DistanceBands ResolveDistanceBands(const EntityData& data)
{
    if (data.AnimationPlayer) {
        const auto& player = *data.AnimationPlayer;
        const float nearDist = std::max(0.0f, player.LODNearDistance);
        const float mediumDist = std::max(nearDist, player.LODMediumDistance);
        const float farDist = std::max(mediumDist, player.LODFarDistance);
        return {
            nearDist * nearDist,
            mediumDist * mediumDist,
            farDist * farDist
        };
    }

    constexpr float kScriptNear = 20.0f;
    constexpr float kScriptMedium = 50.0f;
    constexpr float kScriptFar = 100.0f;
    return {
        kScriptNear * kScriptNear,
        kScriptMedium * kScriptMedium,
        kScriptFar * kScriptFar
    };
}

float ResolveVisibleAnimationInterval(const EntityData& data, float distSq)
{
    const auto* player = data.AnimationPlayer.get();
    if (!player || !player->LODEnabled) {
        return 0.0f;
    }

    const DistanceBands bands = ResolveDistanceBands(data);
    if (distSq < bands.NearSq) {
        return 0.0f;
    }
    if (distSq < bands.MediumSq) {
        return std::max(0.0f, player->LODMediumInterval);
    }
    if (distSq < bands.FarSq) {
        return std::max(0.0f, player->LODFarInterval);
    }
    return std::max(0.0f, player->LODVeryFarInterval);
}

float ResolveOffscreenAnimationInterval(const EntityData& data, float distSq)
{
    const auto* player = data.AnimationPlayer.get();
    if (!player || !player->LODEnabled) {
        return 0.0f;
    }

    const DistanceBands bands = ResolveDistanceBands(data);
    if (distSq < bands.NearSq) {
        return std::max(0.0f, player->OffscreenNearInterval);
    }
    if (distSq < bands.MediumSq) {
        return std::max(0.0f, player->OffscreenMediumInterval);
    }
    if (distSq < bands.FarSq) {
        return std::max(0.0f, player->OffscreenFarInterval);
    }
    return std::max(0.0f, player->OffscreenVeryFarInterval);
}

float ResolveScriptInterval(const EntityData& data)
{
    if (data.ScriptLodForceDisabled) {
        return 0.0f;
    }

    const bool autoEligible =
        data.AnimationPlayer != nullptr ||
        data.NavAgent != nullptr ||
        data.Skeleton != nullptr;
    if (!data.ScriptLodEnabled && !autoEligible) {
        return 0.0f;
    }

    switch (data.NpcScalability.Tier) {
        case ScalabilityTier::Hero:
        case ScalabilityTier::Relevant:
            return 0.0f;
        case ScalabilityTier::Background:
            return 0.03333334f;
        case ScalabilityTier::Filler:
            return 0.06666667f;
        case ScalabilityTier::Dormant:
            return 0.13333334f;
    }
    return 0.0f;
}

float ResolveNavigationInterval(const EntityData& data)
{
    const auto* agent = data.NavAgent.get();
    if (!agent || !agent->Enabled) {
        return 0.0f;
    }

    const float baseInterval = std::max(0.0f, agent->RepathInterval);
    switch (data.NpcScalability.Tier) {
        case ScalabilityTier::Hero:
        case ScalabilityTier::Relevant:
            return baseInterval;
        case ScalabilityTier::Background:
            return std::max(baseInterval, 0.75f);
        case ScalabilityTier::Filler:
            return std::max(baseInterval, 1.0f);
        case ScalabilityTier::Dormant:
            return std::max(baseInterval, 1.5f);
    }
    return baseInterval;
}

bool HasActiveMotionDriver(const EntityData& data)
{
    constexpr float kMotionEpsilonSq = 0.0025f;

    if (data.NavAgent && data.NavAgent->Enabled) {
        const float currentSpeedSq = glm::dot(
            data.NavAgent->CurrentVelocity,
            data.NavAgent->CurrentVelocity);
        const float smoothedSpeedSq = glm::dot(
            data.NavAgent->SmoothedVelocity,
            data.NavAgent->SmoothedVelocity);
        if (currentSpeedSq > kMotionEpsilonSq || smoothedSpeedSq > kMotionEpsilonSq) {
            return true;
        }
        if (data.NavAgent->HasDestination ||
            data.NavAgent->PathRequested ||
            data.NavAgent->HasPath()) {
            return true;
        }
    }

    if (data.CharacterController) {
        const float desiredSpeedSq = glm::dot(
            data.CharacterController->DesiredVelocity,
            data.CharacterController->DesiredVelocity);
        if (desiredSpeedSq > kMotionEpsilonSq ||
            std::abs(data.CharacterController->VerticalVelocity) > 0.01f ||
            data.CharacterController->JumpRequested) {
            return true;
        }
    }

    if (data.RigidBody) {
        const float linearSpeedSq = glm::dot(
            data.RigidBody->LinearVelocity,
            data.RigidBody->LinearVelocity);
        if (linearSpeedSq > kMotionEpsilonSq) {
            return true;
        }
    }

    return false;
}

bool TryResolveWorldBounds(const cm::world::RuntimeWorld* runtimeWorld,
                           EntityID entityId,
                           const EntityData& data,
                           glm::vec3& outMin,
                           glm::vec3& outMax)
{
    if (runtimeWorld != nullptr && data.Skeleton != nullptr) {
        if (const auto* cache = runtimeWorld->TryGetSkinningGroupCache(entityId)) {
            if (runtimeWorld->TryGetSkinningGroupWorldBounds(*cache, outMin, outMax)) {
                constexpr float kConservativePadding = 0.35f;
                outMin -= glm::vec3(kConservativePadding);
                outMax += glm::vec3(kConservativePadding);
                return true;
            }
        }
    }

    if (runtimeWorld != nullptr) {
        const cm::world::RuntimeEntityHandle handle = runtimeWorld->TryGetHandle(entityId);
        if (handle.IsValid()) {
            if (const auto* bounds = runtimeWorld->TryGetBounds(handle);
                bounds != nullptr && bounds->Valid) {
                outMin = bounds->WorldMin;
                outMax = bounds->WorldMax;
                return true;
            }
        }
    }

    return false;
}

bool ShouldParticipate(const EntityData& data)
{
    if (!data.Active) {
        return false;
    }

    return data.AnimationPlayer != nullptr ||
           data.NavAgent != nullptr ||
           !data.Scripts.empty() ||
           data.Skeleton != nullptr;
}

CrowdBudget ResolveCrowdBudget(size_t visibleCount)
{
    CrowdBudget budget{};
    budget.OverflowInterval = (visibleCount >= 48) ? 0.13333334f : 0.10000000f;

    if (visibleCount == 0) {
        return budget;
    }
    if (visibleCount <= 10) {
        budget.FullRateCount = static_cast<uint32_t>(visibleCount);
        return budget;
    }

    budget.FullRateCount = 10;
    if (visibleCount <= 16) {
        budget.ThirtyHzCount = static_cast<uint32_t>(visibleCount) - budget.FullRateCount;
        return budget;
    }

    budget.ThirtyHzCount = 6;
    if (visibleCount <= 24) {
        budget.FifteenHzCount = static_cast<uint32_t>(visibleCount) - budget.FullRateCount - budget.ThirtyHzCount;
        return budget;
    }

    if (visibleCount <= 40) {
        budget.FullRateCount = 8;
        budget.ThirtyHzCount = 8;
        budget.FifteenHzCount = 12;
        return budget;
    }

    budget.FullRateCount = 6;
    budget.ThirtyHzCount = 10;
    budget.FifteenHzCount = 16;
    return budget;
}

float ResolveCrowdInterval(uint32_t rank, size_t visibleCount, bool* throttled = nullptr)
{
    const CrowdBudget budget = ResolveCrowdBudget(visibleCount);
    if (rank < budget.FullRateCount) {
        if (throttled) {
            *throttled = false;
        }
        return 0.0f;
    }

    if (throttled) {
        *throttled = true;
    }
    if (rank < budget.FullRateCount + budget.ThirtyHzCount) {
        return 0.03333334f;
    }
    if (rank < budget.FullRateCount + budget.ThirtyHzCount + budget.FifteenHzCount) {
        return 0.06666667f;
    }
    return budget.OverflowInterval;
}

} // namespace

EntityID ResolveActiveCameraOwnerEntity(Scene& scene)
{
    const EntityID activeCameraEntity = scene.GetActiveCameraEntityID();
    if (activeCameraEntity == INVALID_ENTITY_ID) {
        return INVALID_ENTITY_ID;
    }

    EntityData* cameraData = scene.GetEntityData(activeCameraEntity);
    if (!cameraData) {
        return activeCameraEntity;
    }

    return cameraData->Parent != INVALID_ENTITY_ID
        ? cameraData->Parent
        : activeCameraEntity;
}

bool IsEntityDescendantOf(Scene& scene, EntityID entityId, EntityID ancestorId)
{
    if (entityId == INVALID_ENTITY_ID || ancestorId == INVALID_ENTITY_ID) {
        return false;
    }

    EntityID current = entityId;
    while (current != INVALID_ENTITY_ID) {
        if (current == ancestorId) {
            return true;
        }

        EntityData* data = scene.GetEntityData(current);
        if (!data) {
            break;
        }
        current = data->Parent;
    }

    return false;
}

const char* TierToString(ScalabilityTier tier)
{
    switch (tier) {
        case ScalabilityTier::Hero: return "Hero";
        case ScalabilityTier::Relevant: return "Relevant";
        case ScalabilityTier::Background: return "Background";
        case ScalabilityTier::Filler: return "Filler";
        case ScalabilityTier::Dormant: return "Dormant";
    }
    return "Unknown";
}

std::string DescribeReasonFlags(uint32_t reasonFlags)
{
    if (reasonFlags == Reason_None) {
        return "None";
    }

    std::ostringstream builder;
    bool hasAny = false;
    auto append = [&](const char* label) {
        if (hasAny) {
            builder << ", ";
        }
        builder << label;
        hasAny = true;
    };

    if ((reasonFlags & Reason_NoCamera) != 0u) append("NoCamera");
    if ((reasonFlags & Reason_PlayerOwnedCritical) != 0u) append("PlayerOwned");
    if ((reasonFlags & Reason_MotionCritical) != 0u) append("MotionCritical");
    if ((reasonFlags & Reason_Visible) != 0u) append("Visible");
    if ((reasonFlags & Reason_Offscreen) != 0u) append("Offscreen");
    if ((reasonFlags & Reason_ShadowRelevant) != 0u) append("ShadowRelevant");
    if ((reasonFlags & Reason_CrowdEligible) != 0u) append("CrowdEligible");
    if ((reasonFlags & Reason_CrowdThrottled) != 0u) append("CrowdThrottled");
    if ((reasonFlags & Reason_BeyondNear) != 0u) append("BeyondNear");
    if ((reasonFlags & Reason_BeyondMedium) != 0u) append("BeyondMedium");
    if ((reasonFlags & Reason_BeyondFar) != 0u) append("BeyondFar");

    return builder.str();
}

void UpdateScalability(Scene& scene, float deltaTime)
{
    (void)deltaTime;

    static uint64_t s_FrameCounter = 0;
    ++s_FrameCounter;

    if (!scene.m_IsPlaying) {
        for (const Entity& entity : scene.GetEntities()) {
            if (EntityData* data = scene.GetEntityData(entity.GetID())) {
                data->NpcScalability.ResetRuntime();
                data->NpcScalability.EvaluatedFrame = s_FrameCounter;
            }
        }
        return;
    }

    if (!scene.GetRuntimeWorld() ||
        (scene.HasPendingRuntimeWorldStructuralSyncWork() && !scene.IsRuntimeWorldFrameSyncLocked())) {
        scene.SyncRuntimeWorld(false);
    }

    const cm::world::RuntimeWorld* runtimeWorld = scene.GetRuntimeWorld();
    Camera* activeCamera = scene.GetActiveCamera();
    const EntityID activeCameraOwnerEntity = ResolveActiveCameraOwnerEntity(scene);
    const bool haveCamera = activeCamera != nullptr;

    glm::vec3 cameraPosition(0.0f);
    LodFrustum frustum{};
    if (activeCamera != nullptr) {
        cameraPosition = activeCamera->GetPosition();
        frustum = BuildFrustum(activeCamera->GetViewMatrix(), activeCamera->GetProjectionMatrix());
    }

    const Environment& environment = scene.GetEnvironment();
    const float shadowDistance =
        environment.ShadowsEnabled ? std::max(0.0f, environment.ShadowDistance) : 0.0f;

    constexpr float kCrowdNearDistanceSq = 28.0f * 28.0f;
    std::vector<CrowdCandidate> crowdCandidates;
    crowdCandidates.reserve(64);

    uint64_t participantCount = 0;
    uint64_t visibleCount = 0;
    uint64_t heroCount = 0;
    uint64_t relevantCount = 0;
    uint64_t backgroundCount = 0;
    uint64_t fillerCount = 0;
    uint64_t dormantCount = 0;

    for (const Entity& entity : scene.GetEntities()) {
        EntityData* data = scene.GetEntityData(entity.GetID());
        if (!data) {
            continue;
        }

        ScalabilityState& state = data->NpcScalability;
        state.ResetRuntime();
        state.EvaluatedFrame = s_FrameCounter;

        if (!ShouldParticipate(*data)) {
            continue;
        }

        ++participantCount;
        state.Participates = true;

        const DistanceBands bands = ResolveDistanceBands(*data);
        const glm::vec3 worldPos = glm::vec3(data->Transform.WorldMatrix[3]);
        float distSq = 0.0f;
        bool visualVisible = true;
        bool shadowRelevant = false;
        bool playerOwnedCritical = false;
        bool motionCritical = false;
        uint32_t reasonFlags = Reason_None;

        if (!haveCamera) {
            reasonFlags |= Reason_NoCamera | Reason_Visible;
        } else {
            const glm::vec3 toCamera = worldPos - cameraPosition;
            distSq = glm::dot(toCamera, toCamera);
            state.CameraDistance = std::sqrt(distSq);

            playerOwnedCritical =
                IsEntityDescendantOf(scene, entity.GetID(), activeCameraOwnerEntity) ||
                IsEntityDescendantOf(scene, activeCameraOwnerEntity, entity.GetID());
            motionCritical =
                !playerOwnedCritical &&
                distSq <= std::max(bands.NearSq, 32.0f * 32.0f) &&
                HasActiveMotionDriver(*data);

            visualVisible = playerOwnedCritical;
            glm::vec3 worldMin(0.0f);
            glm::vec3 worldMax(0.0f);
            float boundsRadius = 0.0f;
            if (!visualVisible) {
                if (TryResolveWorldBounds(runtimeWorld, entity.GetID(), *data, worldMin, worldMax)) {
                    visualVisible = AabbVisible(frustum, worldMin, worldMax);
                    boundsRadius = 0.5f * glm::length(worldMax - worldMin);
                } else {
                    visualVisible = PointVisible(frustum, worldPos, 1.75f);
                    boundsRadius = 1.75f;
                }
            } else if (TryResolveWorldBounds(runtimeWorld, entity.GetID(), *data, worldMin, worldMax)) {
                boundsRadius = 0.5f * glm::length(worldMax - worldMin);
            }

            if (shadowDistance > 0.0f) {
                const float shadowReach = shadowDistance + boundsRadius;
                shadowRelevant = distSq <= shadowReach * shadowReach;
            }

            if (playerOwnedCritical) reasonFlags |= Reason_PlayerOwnedCritical;
            if (motionCritical) reasonFlags |= Reason_MotionCritical;
            if (visualVisible) reasonFlags |= Reason_Visible;
            else reasonFlags |= Reason_Offscreen;
            if (shadowRelevant) reasonFlags |= Reason_ShadowRelevant;
            if (distSq >= bands.NearSq) reasonFlags |= Reason_BeyondNear;
            if (distSq >= bands.MediumSq) reasonFlags |= Reason_BeyondMedium;
            if (distSq >= bands.FarSq) reasonFlags |= Reason_BeyondFar;
        }

        state.PlayerOwnedCritical = playerOwnedCritical;
        state.MotionCritical = motionCritical;
        state.VisualVisible = visualVisible;
        state.ShadowRelevant = shadowRelevant;
        if (!haveCamera) {
            state.CameraDistance = 0.0f;
        }

        if (!haveCamera || playerOwnedCritical) {
            state.Tier = ScalabilityTier::Hero;
            state.Representation = ScalabilityRepresentation::HeroCharacter;
            ++heroCount;
        } else if (motionCritical || distSq <= bands.NearSq) {
            state.Tier = ScalabilityTier::Relevant;
            state.Representation = ScalabilityRepresentation::BudgetedCharacter;
            ++relevantCount;
        } else if (distSq <= bands.MediumSq) {
            state.Tier = ScalabilityTier::Background;
            state.Representation = ScalabilityRepresentation::BudgetedCharacter;
            ++backgroundCount;
        } else if (visualVisible || distSq <= bands.FarSq) {
            state.Tier = ScalabilityTier::Filler;
            state.Representation = ScalabilityRepresentation::BudgetedCharacter;
            ++fillerCount;
        } else {
            state.Tier = ScalabilityTier::Dormant;
            state.Representation = ScalabilityRepresentation::DormantCharacter;
            ++dormantCount;
        }

        if (visualVisible) {
            ++visibleCount;
        }

        state.ReasonFlags = reasonFlags;
        state.StateOnlyWhenScheduled = false;
        state.AnimationUpdateInterval = 0.0f;
        if (data->AnimationPlayer) {
            if (!data->AnimationPlayer->LODEnabled || playerOwnedCritical || motionCritical || !haveCamera) {
                state.AnimationUpdateInterval = 0.0f;
            } else if (visualVisible) {
                state.AnimationUpdateInterval = ResolveVisibleAnimationInterval(*data, distSq);
            } else {
                state.AnimationUpdateInterval = ResolveOffscreenAnimationInterval(*data, distSq);
                state.StateOnlyWhenScheduled = true;
            }
        }

        state.ScriptUpdateInterval = ResolveScriptInterval(*data);
        state.NavigationRepathInterval = ResolveNavigationInterval(*data);

        const bool crowdEligible =
            data->AnimationPlayer != nullptr &&
            data->AnimationPlayer->CrowdThrottleEnabled &&
            data->AnimationPlayer->LODEnabled &&
            visualVisible &&
            !playerOwnedCritical &&
            distSq <= kCrowdNearDistanceSq;
        state.CrowdEligible = crowdEligible;
        if (crowdEligible) {
            state.ReasonFlags |= Reason_CrowdEligible;
            crowdCandidates.push_back({ entity.GetID(), distSq });
        }
    }

    std::sort(crowdCandidates.begin(), crowdCandidates.end(),
        [](const CrowdCandidate& lhs, const CrowdCandidate& rhs) {
            return lhs.DistSq < rhs.DistSq;
        });

    for (uint32_t rank = 0; rank < crowdCandidates.size(); ++rank) {
        EntityData* data = scene.GetEntityData(crowdCandidates[rank].Entity);
        if (!data) {
            continue;
        }

        ScalabilityState& state = data->NpcScalability;
        state.CrowdRank = rank;
        state.VisibleCrowdCount = static_cast<uint32_t>(crowdCandidates.size());

        bool throttled = false;
        const float crowdInterval = ResolveCrowdInterval(rank, crowdCandidates.size(), &throttled);
        if (throttled) {
            state.CrowdThrottled = true;
            state.ReasonFlags |= Reason_CrowdThrottled;
            state.AnimationUpdateInterval = std::max(state.AnimationUpdateInterval, crowdInterval);
        }
    }

    Profiler::Get().SetCounter("NpcScalability/Participants", participantCount);
    Profiler::Get().SetCounter("NpcScalability/Visible", visibleCount);
    Profiler::Get().SetCounter("NpcScalability/Hero", heroCount);
    Profiler::Get().SetCounter("NpcScalability/Relevant", relevantCount);
    Profiler::Get().SetCounter("NpcScalability/Background", backgroundCount);
    Profiler::Get().SetCounter("NpcScalability/Filler", fillerCount);
    Profiler::Get().SetCounter("NpcScalability/Dormant", dormantCount);
    Profiler::Get().SetCounter("NpcScalability/CrowdCandidates", static_cast<uint64_t>(crowdCandidates.size()));
}

} // namespace cm::npc
