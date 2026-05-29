using System;
using System.Numerics;

namespace ClaymoreEngine
{
    /// <summary>
    /// Attaches an entity to a bone in a skeleton hierarchy without direct parenting.
    /// Useful for weapons, accessories, and other items that need to follow bone transforms.
    /// </summary>
    public class BoneAttachment : ComponentBase
    {
        /// <summary>
        /// Gets whether the entity has a BoneAttachment component.
        /// </summary>
        public bool HasComponent => ComponentInterop.BoneAttachment_HasComponent(entity.EntityID);

        /// <summary>
        /// Gets or sets whether the bone attachment is enabled.
        /// </summary>
        public bool Enabled
        {
            get => ComponentInterop.BoneAttachment_GetEnabled(entity.EntityID);
            set => ComponentInterop.BoneAttachment_SetEnabled(entity.EntityID, value);
        }

        /// <summary>
        /// Gets or sets the target bone name.
        /// Setting this will invalidate the current resolution, triggering a new bone search.
        /// </summary>
        public string BoneName
        {
            get => ComponentInterop.BoneAttachment_GetBoneName(entity.EntityID);
            set => ComponentInterop.BoneAttachment_SetBoneName(entity.EntityID, value ?? string.Empty);
        }

        /// <summary>
        /// Gets or sets the local position offset relative to the target bone.
        /// </summary>
        public Vector3 LocalPosition
        {
            get
            {
                ComponentInterop.BoneAttachment_GetLocalPosition(entity.EntityID, out float x, out float y, out float z);
                return new Vector3(x, y, z);
            }
            set => ComponentInterop.BoneAttachment_SetLocalPosition(entity.EntityID, value.X, value.Y, value.Z);
        }

        /// <summary>
        /// Gets or sets the local rotation offset relative to the target bone (in Euler degrees).
        /// </summary>
        public Vector3 LocalRotation
        {
            get
            {
                ComponentInterop.BoneAttachment_GetLocalRotation(entity.EntityID, out float x, out float y, out float z);
                return new Vector3(x, y, z);
            }
            set => ComponentInterop.BoneAttachment_SetLocalRotation(entity.EntityID, value.X, value.Y, value.Z);
        }

        /// <summary>
        /// Gets or sets the local scale relative to the target bone.
        /// </summary>
        public Vector3 LocalScale
        {
            get
            {
                ComponentInterop.BoneAttachment_GetLocalScale(entity.EntityID, out float x, out float y, out float z);
                return new Vector3(x, y, z);
            }
            set => ComponentInterop.BoneAttachment_SetLocalScale(entity.EntityID, value.X, value.Y, value.Z);
        }

        /// <summary>
        /// Gets or sets whether the attachment inherits the bone's rotation.
        /// </summary>
        public bool InheritRotation
        {
            get => ComponentInterop.BoneAttachment_GetInheritRotation(entity.EntityID);
            set => ComponentInterop.BoneAttachment_SetInheritRotation(entity.EntityID, value);
        }

        /// <summary>
        /// Gets or sets whether the attachment inherits the bone's scale.
        /// </summary>
        public bool InheritScale
        {
            get => ComponentInterop.BoneAttachment_GetInheritScale(entity.EntityID);
            set => ComponentInterop.BoneAttachment_SetInheritScale(entity.EntityID, value);
        }

        /// <summary>
        /// Gets whether the bone attachment has been successfully resolved to a bone entity.
        /// </summary>
        public bool IsResolved => ComponentInterop.BoneAttachment_IsResolved(entity.EntityID);

        /// <summary>
        /// Gets or sets the explicit skeleton entity reference.
        /// When set, bypasses the automatic skeleton detection (parent/sibling walking).
        /// Setting this invalidates resolution, triggering a new bone search on the specified skeleton.
        /// Use this for reliable same-frame initialization when instantiating and configuring BoneAttachment.
        /// </summary>
        public Entity? SkeletonEntity
        {
            get
            {
                int skelId = ComponentInterop.BoneAttachment_GetSkeletonEntity(entity.EntityID);
                return skelId > 0 ? new Entity(skelId) : null;
            }
            set
            {
                int skelId = value?.EntityID ?? -1;
                ComponentInterop.BoneAttachment_SetSkeletonEntity(entity.EntityID, skelId);
            }
        }

        /// <summary>
        /// Invalidates the current bone resolution, forcing a new search on the next update.
        /// Call this after changing the BoneName or when the skeleton hierarchy changes.
        /// </summary>
        public void InvalidateResolution()
        {
            ComponentInterop.BoneAttachment_InvalidateResolution(entity.EntityID);
        }

        /// <summary>
        /// Sets the target bone by name and the local transform offset in one call.
        /// </summary>
        /// <param name="boneName">The name of the target bone.</param>
        /// <param name="position">Local position offset relative to the bone.</param>
        /// <param name="rotationEuler">Local rotation offset in Euler degrees.</param>
        /// <param name="scale">Local scale relative to the bone.</param>
        public void SetBoneWithOffset(string boneName, Vector3 position, Vector3 rotationEuler, Vector3 scale)
        {
            BoneName = boneName;
            LocalPosition = position;
            LocalRotation = rotationEuler;
            LocalScale = scale;
        }

        /// <summary>
        /// Sets the target bone by name with default local offset (identity transform).
        /// </summary>
        /// <param name="boneName">The name of the target bone.</param>
        public void SetBone(string boneName)
        {
            BoneName = boneName;
        }

        /// <summary>
        /// Configures the bone attachment with an explicit skeleton reference for reliable same-frame initialization.
        /// Use this method when instantiating an entity and configuring BoneAttachment in the same frame.
        /// </summary>
        /// <param name="skeletonEntity">The entity containing the SkeletonComponent (skeleton root)</param>
        /// <param name="boneName">The name of the target bone to attach to</param>
        /// <param name="localPosition">Optional local position offset</param>
        /// <param name="localRotation">Optional local rotation offset in Euler degrees</param>
        /// <param name="localScale">Optional local scale</param>
        public void Configure(Entity skeletonEntity, string boneName, Vector3? localPosition = null, Vector3? localRotation = null, Vector3? localScale = null)
        {
            // Set skeleton first to bypass auto-detection
            SkeletonEntity = skeletonEntity;
            BoneName = boneName;
            if (localPosition.HasValue)
                LocalPosition = localPosition.Value;
            if (localRotation.HasValue)
                LocalRotation = localRotation.Value;
            if (localScale.HasValue)
                LocalScale = localScale.Value;
        }
    }
}

