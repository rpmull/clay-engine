#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include "Entity.h"

class Scene;

namespace cm::npc {

enum class ScalabilityTier : uint8_t {
    Hero = 0,
    Relevant,
    Background,
    Filler,
    Dormant
};

enum class ScalabilityRepresentation : uint8_t {
    HeroCharacter = 0,
    BudgetedCharacter,
    DormantCharacter
};

enum ScalabilityReason : uint32_t {
    Reason_None = 0,
    Reason_NoCamera = 1u << 0,
    Reason_PlayerOwnedCritical = 1u << 1,
    Reason_MotionCritical = 1u << 2,
    Reason_Visible = 1u << 3,
    Reason_Offscreen = 1u << 4,
    Reason_ShadowRelevant = 1u << 5,
    Reason_CrowdEligible = 1u << 6,
    Reason_CrowdThrottled = 1u << 7,
    Reason_BeyondNear = 1u << 8,
    Reason_BeyondMedium = 1u << 9,
    Reason_BeyondFar = 1u << 10,
};

struct ScalabilityState {
    bool Participates = false;
    ScalabilityTier Tier = ScalabilityTier::Relevant;
    ScalabilityRepresentation Representation = ScalabilityRepresentation::BudgetedCharacter;
    uint32_t ReasonFlags = Reason_None;
    float CameraDistance = 0.0f;
    bool VisualVisible = true;
    bool ShadowRelevant = false;
    bool PlayerOwnedCritical = false;
    bool MotionCritical = false;
    bool CrowdEligible = false;
    bool CrowdThrottled = false;
    uint32_t CrowdRank = std::numeric_limits<uint32_t>::max();
    uint32_t VisibleCrowdCount = 0;
    bool StateOnlyWhenScheduled = false;
    float AnimationUpdateInterval = 0.0f;
    float ScriptUpdateInterval = 0.0f;
    float NavigationRepathInterval = 0.0f;
    uint64_t EvaluatedFrame = 0;

    void ResetRuntime()
    {
        Participates = false;
        Tier = ScalabilityTier::Relevant;
        Representation = ScalabilityRepresentation::BudgetedCharacter;
        ReasonFlags = Reason_None;
        CameraDistance = 0.0f;
        VisualVisible = true;
        ShadowRelevant = false;
        PlayerOwnedCritical = false;
        MotionCritical = false;
        CrowdEligible = false;
        CrowdThrottled = false;
        CrowdRank = std::numeric_limits<uint32_t>::max();
        VisibleCrowdCount = 0;
        StateOnlyWhenScheduled = false;
        AnimationUpdateInterval = 0.0f;
        ScriptUpdateInterval = 0.0f;
        NavigationRepathInterval = 0.0f;
    }

    [[nodiscard]] bool WantsSkinningWork() const
    {
        return PlayerOwnedCritical || VisualVisible || ShadowRelevant;
    }

    [[nodiscard]] bool IsDormant() const
    {
        return Tier == ScalabilityTier::Dormant;
    }
};

void UpdateScalability(Scene& scene, float deltaTime);
EntityID ResolveActiveCameraOwnerEntity(Scene& scene);
bool IsEntityDescendantOf(Scene& scene, EntityID entityId, EntityID ancestorId);
const char* TierToString(ScalabilityTier tier);
std::string DescribeReasonFlags(uint32_t reasonFlags);

} // namespace cm::npc
