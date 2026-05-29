using System;
using System.Collections.Generic;

namespace ClaymoreEngine
{
    [Flags]
    public enum NpcScalabilityReason : uint
    {
        None = 0,
        NoCamera = 1u << 0,
        PlayerOwnedCritical = 1u << 1,
        MotionCritical = 1u << 2,
        Visible = 1u << 3,
        Offscreen = 1u << 4,
        ShadowRelevant = 1u << 5,
        CrowdEligible = 1u << 6,
        CrowdThrottled = 1u << 7,
        BeyondNear = 1u << 8,
        BeyondMedium = 1u << 9,
        BeyondFar = 1u << 10,
    }

    public enum NpcScalabilityTier
    {
        Hero = 0,
        Relevant = 1,
        Background = 2,
        Filler = 3,
        Dormant = 4,
    }

    public enum NpcScalabilityRepresentation
    {
        HeroCharacter = 0,
        BudgetedCharacter = 1,
        DormantCharacter = 2,
    }

    public readonly struct NpcScalabilityInfo
    {
        public readonly bool Participates;
        public readonly NpcScalabilityTier Tier;
        public readonly NpcScalabilityRepresentation Representation;
        public readonly NpcScalabilityReason ReasonFlags;
        public readonly float CameraDistance;
        public readonly bool Visible;
        public readonly int CrowdRank;
        public readonly int CrowdCount;
        public readonly float AnimationUpdateInterval;
        public readonly float ScriptUpdateInterval;
        public readonly float NavigationRepathInterval;

        internal NpcScalabilityInfo(
            bool participates,
            NpcScalabilityTier tier,
            NpcScalabilityRepresentation representation,
            NpcScalabilityReason reasonFlags,
            float cameraDistance,
            bool visible,
            int crowdRank,
            int crowdCount,
            float animationUpdateInterval,
            float scriptUpdateInterval,
            float navigationRepathInterval)
        {
            Participates = participates;
            Tier = tier;
            Representation = representation;
            ReasonFlags = reasonFlags;
            CameraDistance = cameraDistance;
            Visible = visible;
            CrowdRank = crowdRank;
            CrowdCount = crowdCount;
            AnimationUpdateInterval = animationUpdateInterval;
            ScriptUpdateInterval = scriptUpdateInterval;
            NavigationRepathInterval = navigationRepathInterval;
        }

        public bool CrowdEligible => (ReasonFlags & NpcScalabilityReason.CrowdEligible) != 0;
        public bool CrowdThrottled => (ReasonFlags & NpcScalabilityReason.CrowdThrottled) != 0;
        public bool PlayerOwnedCritical => (ReasonFlags & NpcScalabilityReason.PlayerOwnedCritical) != 0;
        public bool MotionCritical => (ReasonFlags & NpcScalabilityReason.MotionCritical) != 0;
        public string DescribeReasons() => NpcScalability.DescribeReasons(ReasonFlags);
    }

    public static class NpcScalability
    {
        public static NpcScalabilityInfo Get(Entity entity) => Get(entity.EntityID);

        public static NpcScalabilityInfo Get(int entityId)
        {
            if (entityId <= 0 ||
                ComponentInterop.NpcScalability_GetParticipates == null ||
                ComponentInterop.NpcScalability_GetTier == null ||
                ComponentInterop.NpcScalability_GetRepresentation == null ||
                ComponentInterop.NpcScalability_GetReasonFlags == null ||
                ComponentInterop.NpcScalability_GetCameraDistance == null ||
                ComponentInterop.NpcScalability_GetVisible == null ||
                ComponentInterop.NpcScalability_GetCrowdRank == null ||
                ComponentInterop.NpcScalability_GetCrowdCount == null ||
                ComponentInterop.NpcScalability_GetAnimationUpdateInterval == null ||
                ComponentInterop.NpcScalability_GetScriptUpdateInterval == null ||
                ComponentInterop.NpcScalability_GetNavigationRepathInterval == null)
            {
                return default;
            }

            return new NpcScalabilityInfo(
                ComponentInterop.NpcScalability_GetParticipates(entityId),
                (NpcScalabilityTier)ComponentInterop.NpcScalability_GetTier(entityId),
                (NpcScalabilityRepresentation)ComponentInterop.NpcScalability_GetRepresentation(entityId),
                (NpcScalabilityReason)ComponentInterop.NpcScalability_GetReasonFlags(entityId),
                ComponentInterop.NpcScalability_GetCameraDistance(entityId),
                ComponentInterop.NpcScalability_GetVisible(entityId),
                ComponentInterop.NpcScalability_GetCrowdRank(entityId),
                ComponentInterop.NpcScalability_GetCrowdCount(entityId),
                ComponentInterop.NpcScalability_GetAnimationUpdateInterval(entityId),
                ComponentInterop.NpcScalability_GetScriptUpdateInterval(entityId),
                ComponentInterop.NpcScalability_GetNavigationRepathInterval(entityId));
        }

        public static string DescribeReasons(NpcScalabilityReason reasonFlags)
        {
            if (reasonFlags == NpcScalabilityReason.None)
            {
                return "None";
            }

            List<string> labels = new List<string>(8);
            if ((reasonFlags & NpcScalabilityReason.NoCamera) != 0) labels.Add("NoCamera");
            if ((reasonFlags & NpcScalabilityReason.PlayerOwnedCritical) != 0) labels.Add("PlayerOwned");
            if ((reasonFlags & NpcScalabilityReason.MotionCritical) != 0) labels.Add("MotionCritical");
            if ((reasonFlags & NpcScalabilityReason.Visible) != 0) labels.Add("Visible");
            if ((reasonFlags & NpcScalabilityReason.Offscreen) != 0) labels.Add("Offscreen");
            if ((reasonFlags & NpcScalabilityReason.ShadowRelevant) != 0) labels.Add("ShadowRelevant");
            if ((reasonFlags & NpcScalabilityReason.CrowdEligible) != 0) labels.Add("CrowdEligible");
            if ((reasonFlags & NpcScalabilityReason.CrowdThrottled) != 0) labels.Add("CrowdThrottled");
            if ((reasonFlags & NpcScalabilityReason.BeyondNear) != 0) labels.Add("BeyondNear");
            if ((reasonFlags & NpcScalabilityReason.BeyondMedium) != 0) labels.Add("BeyondMedium");
            if ((reasonFlags & NpcScalabilityReason.BeyondFar) != 0) labels.Add("BeyondFar");
            return string.Join(", ", labels);
        }
    }
}
