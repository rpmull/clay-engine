using System;
using System.Numerics;

namespace ClaymoreEngine
{
    public enum TintBlendMode
    {
        Normal = 0,
        Multiply = 1,
        Overlay = 2,
        Add = 3,
        Screen = 4,
        SoftLight = 5,
        ColorDodge = 6,
        ColorBurn = 7,
        Difference = 8,
        Detail = 9
    }

    /// <summary>
    /// Controls tint/color of child meshes based on a name pattern.
    /// Allows simultaneously changing material colors on multiple meshes that match the pattern.
    /// </summary>
    public class TintController : ComponentBase
    {
        /// <summary>
        /// Gets whether the entity has a TintController component.
        /// </summary>
        public bool HasComponent => ComponentInterop.TintController_HasComponent(entity.EntityID);

        /// <summary>
        /// Gets or sets the name pattern for matching child meshes (e.g., "Body*").
        /// </summary>
        public string NamePattern
        {
            get => ComponentInterop.TintController_GetNamePattern(entity.EntityID);
            set => ComponentInterop.TintController_SetNamePattern(entity.EntityID, value);
        }

        /// <summary>
        /// Gets or sets whether to use the multi-channel tint mask mode.
        /// When true, uses TintColor0-3. When false, uses BaseTint.
        /// </summary>
        public bool UseTintMask
        {
            get => ComponentInterop.TintController_GetUseTintMask(entity.EntityID);
            set => ComponentInterop.TintController_SetUseTintMask(entity.EntityID, value);
        }

        /// <summary>
        /// Gets or sets whether PBR scalar overrides are enabled for tinted meshes.
        /// When enabled, metallic/roughness/emission values override the mesh material.
        /// </summary>
        public bool UsePbrOverrides
        {
            get => ComponentInterop.TintController_GetUsePbrOverrides(entity.EntityID);
            set => ComponentInterop.TintController_SetUsePbrOverrides(entity.EntityID, value);
        }

        /// <summary>Override metallic (0-1) when UsePbrOverrides is enabled.</summary>
        public float Metallic
        {
            get => ComponentInterop.TintController_GetPbrMetallic(entity.EntityID);
            set => ComponentInterop.TintController_SetPbrMetallic(entity.EntityID, value);
        }

        /// <summary>Override roughness (0-1) when UsePbrOverrides is enabled.</summary>
        public float Roughness
        {
            get => ComponentInterop.TintController_GetPbrRoughness(entity.EntityID);
            set => ComponentInterop.TintController_SetPbrRoughness(entity.EntityID, value);
        }

        /// <summary>Override emission strength when UsePbrOverrides is enabled.</summary>
        public float EmissionStrength
        {
            get => ComponentInterop.TintController_GetPbrEmissionStrength(entity.EntityID);
            set => ComponentInterop.TintController_SetPbrEmissionStrength(entity.EntityID, value);
        }

        /// <summary>
        /// Default blend mode used by targets that do not specify their own logic elsewhere.
        /// </summary>
        public TintBlendMode GlobalBlendMode
        {
            get => (TintBlendMode)ComponentInterop.TintController_GetGlobalBlendMode(entity.EntityID);
            set => ComponentInterop.TintController_SetGlobalBlendMode(entity.EntityID, (int)value);
        }

        /// <summary>
        /// Controls whether runtime-parented skinned meshes are auto-added as tint targets.
        /// Disable this for character creators that rebuild targets explicitly.
        /// </summary>
        public bool AutoIncludeParentedSkinnedMeshes
        {
            get => ComponentInterop.TintController_GetAutoIncludeParentedSkinnedMeshes(entity.EntityID);
            set => ComponentInterop.TintController_SetAutoIncludeParentedSkinnedMeshes(entity.EntityID, value);
        }

        /// <summary>Override emission color when UsePbrOverrides is enabled.</summary>
        public Vector3 EmissionColor
        {
            get
            {
                ComponentInterop.TintController_GetPbrEmissionColor(entity.EntityID, out float r, out float g, out float b);
                return new Vector3(r, g, b);
            }
            set => ComponentInterop.TintController_SetPbrEmissionColor(entity.EntityID, value.X, value.Y, value.Z);
        }

        /// <summary>
        /// Gets or sets the base tint color (used when UseTintMask is false).
        /// </summary>
        public Vector4 BaseTint
        {
            get
            {
                ComponentInterop.TintController_GetBaseTint(entity.EntityID, out float r, out float g, out float b, out float a);
                return new Vector4(r, g, b, a);
            }
            set => ComponentInterop.TintController_SetBaseTint(entity.EntityID, value.X, value.Y, value.Z, value.W);
        }

        /// <summary>
        /// Gets the tint color for a specific channel (0-3).
        /// </summary>
        public Vector4 GetTintColor(int channel)
        {
            if (channel < 0 || channel > 3)
                throw new ArgumentOutOfRangeException(nameof(channel), "Channel must be 0-3");
            ComponentInterop.TintController_GetTintColor(entity.EntityID, channel, out float r, out float g, out float b, out float a);
            return new Vector4(r, g, b, a);
        }

        /// <summary>
        /// Sets the tint color for a specific channel (0-3).
        /// </summary>
        public void SetTintColor(int channel, Vector4 color)
        {
            if (channel < 0 || channel > 3)
                throw new ArgumentOutOfRangeException(nameof(channel), "Channel must be 0-3");
            ComponentInterop.TintController_SetTintColor(entity.EntityID, channel, color.X, color.Y, color.Z, color.W);
        }

        /// <summary>
        /// Gets or sets TintColor0 (red channel mask).
        /// </summary>
        public Vector4 TintColor0
        {
            get => GetTintColor(0);
            set => SetTintColor(0, value);
        }

        /// <summary>
        /// Gets or sets TintColor1 (green channel mask).
        /// </summary>
        public Vector4 TintColor1
        {
            get => GetTintColor(1);
            set => SetTintColor(1, value);
        }

        /// <summary>
        /// Gets or sets TintColor2 (blue channel mask).
        /// </summary>
        public Vector4 TintColor2
        {
            get => GetTintColor(2);
            set => SetTintColor(2, value);
        }

        /// <summary>
        /// Gets or sets TintColor3 (alpha channel mask).
        /// </summary>
        public Vector4 TintColor3
        {
            get => GetTintColor(3);
            set => SetTintColor(3, value);
        }

        /// <summary>
        /// Number of explicitly tracked tint targets on this controller.
        /// </summary>
        public int TrackedTargetCount => ComponentInterop.TintController_GetTrackedTargetCount(entity.EntityID);

        /// <summary>
        /// Gets a tracked target entity ID by target index.
        /// </summary>
        public int GetTrackedTargetEntityId(int index)
        {
            int count = TrackedTargetCount;
            if (index < 0 || index >= count)
                throw new ArgumentOutOfRangeException(nameof(index), $"Index must be in [0, {count - 1}]");
            return ComponentInterop.TintController_GetTrackedTargetEntity(entity.EntityID, index);
        }

        /// <summary>
        /// Non-alloc copy of tracked target entity IDs into a caller-provided span.
        /// Returns the number of IDs written.
        /// </summary>
        public int CopyTrackedTargetEntityIds(Span<int> destination)
        {
            int count = TrackedTargetCount;
            int written = Math.Min(count, destination.Length);
            for (int i = 0; i < written; i++)
                destination[i] = ComponentInterop.TintController_GetTrackedTargetEntity(entity.EntityID, i);
            return written;
        }

        /// <summary>
        /// Allocating convenience getter for tracked target entity IDs.
        /// Prefer CopyTrackedTargetEntityIds for tight loops.
        /// </summary>
        public int[] TrackedTargetEntityIds
        {
            get
            {
                int count = TrackedTargetCount;
                if (count <= 0) return Array.Empty<int>();
                int[] ids = new int[count];
                CopyTrackedTargetEntityIds(ids);
                return ids;
            }
        }

        /// <summary>
        /// Removes all explicit targets from this controller.
        /// </summary>
        public void ClearTargets()
        {
            ComponentInterop.TintController_ClearTargets(entity.EntityID);
        }

        /// <summary>
        /// Removes all explicit targets pointing at the given entity.
        /// </summary>
        public void RemoveTargetsForEntity(Entity target)
        {
            if (!target.IsValid)
                return;

            ComponentInterop.TintController_RemoveTargetsForEntity(entity.EntityID, target.EntityID);
        }

        /// <summary>
        /// Adds a new explicit tint target.
        /// </summary>
        public void AddTarget(
            Entity target,
            int materialSlot = -1,
            TintBlendMode blendMode = TintBlendMode.Normal,
            bool useTargetColor = false,
            Vector4? color = null)
        {
            if (!target.IsValid)
                throw new ArgumentException("Target entity must be valid.", nameof(target));

            Vector4 resolvedColor = color ?? Vector4.One;
            ComponentInterop.TintController_AddTarget(
                entity.EntityID,
                target.EntityID,
                materialSlot,
                (int)blendMode,
                useTargetColor,
                resolvedColor.X,
                resolvedColor.Y,
                resolvedColor.Z,
                resolvedColor.W);
        }

        /// <summary>
        /// Allocating convenience getter for tracked target entities.
        /// Prefer TrackedTargetEntityIds/CopyTrackedTargetEntityIds for performance-sensitive code.
        /// </summary>
        public Entity[] TrackedChildren
        {
            get
            {
                int[] ids = TrackedTargetEntityIds;
                if (ids.Length == 0) return Array.Empty<Entity>();
                Entity[] entities = new Entity[ids.Length];
                for (int i = 0; i < ids.Length; i++)
                    entities[i] = new Entity(ids[i]);
                return entities;
            }
        }

        /// <summary>
        /// Refreshes the list of matching meshes based on the current NamePattern.
        /// Call this after changing the NamePattern to update which meshes are affected.
        /// </summary>
        public void Refresh()
        {
            ComponentInterop.TintController_Refresh(entity.EntityID);
        }
    }
}

