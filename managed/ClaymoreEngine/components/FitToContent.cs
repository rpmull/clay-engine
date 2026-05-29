using System.Numerics;

namespace ClaymoreEngine
{
    /// <summary>
    /// Makes a Panel auto-size to fit its children.
    /// Attach to an entity with a PanelComponent to automatically adjust
    /// the panel's Size based on the bounding box of its children.
    /// </summary>
    public sealed class FitToContent : ComponentBase
    {
        public bool worldSpace
        {
            get => UIRenderSpaceUtility.GetWorldSpace(entity);
            set => UIRenderSpaceUtility.SetWorldSpace(entity, value);
        }

        public bool billboard
        {
            get => UIRenderSpaceUtility.GetBillboard(entity);
            set => UIRenderSpaceUtility.SetBillboard(entity, value);
        }

        /// <summary>
        /// Enable/disable auto-sizing (useful for temporarily disabling).
        /// </summary>
        public bool enabled
        {
            get => entity.IsValid && ComponentInterop.UI_FitToContent_GetEnabled != null
                ? ComponentInterop.UI_FitToContent_GetEnabled(entity.EntityID)
                : true;
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_FitToContent_SetEnabled?.Invoke(entity.EntityID, value);
            }
        }

        /// <summary>
        /// Whether to auto-fit the width.
        /// </summary>
        public bool fitWidth
        {
            get => entity.IsValid && ComponentInterop.UI_FitToContent_GetFitWidth != null
                ? ComponentInterop.UI_FitToContent_GetFitWidth(entity.EntityID)
                : true;
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_FitToContent_SetFitWidth?.Invoke(entity.EntityID, value);
            }
        }

        /// <summary>
        /// Whether to auto-fit the height.
        /// </summary>
        public bool fitHeight
        {
            get => entity.IsValid && ComponentInterop.UI_FitToContent_GetFitHeight != null
                ? ComponentInterop.UI_FitToContent_GetFitHeight(entity.EntityID)
                : true;
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_FitToContent_SetFitHeight?.Invoke(entity.EntityID, value);
            }
        }

        /// <summary>
        /// Padding around children content (left, top, right, bottom).
        /// </summary>
        public Vector4 padding
        {
            get
            {
                if (!entity.IsValid || ComponentInterop.UI_FitToContent_GetPadding == null)
                    return new Vector4(10f, 10f, 10f, 10f);
                ComponentInterop.UI_FitToContent_GetPadding(entity.EntityID, out float l, out float t, out float r, out float b);
                return new Vector4(l, t, r, b);
            }
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_FitToContent_SetPadding?.Invoke(entity.EntityID, value.X, value.Y, value.Z, value.W);
            }
        }

        /// <summary>
        /// Minimum size constraints (0 = no minimum).
        /// </summary>
        public Vector2 minSize
        {
            get
            {
                if (!entity.IsValid || ComponentInterop.UI_FitToContent_GetMinSize == null)
                    return Vector2.Zero;
                ComponentInterop.UI_FitToContent_GetMinSize(entity.EntityID, out float w, out float h);
                return new Vector2(w, h);
            }
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_FitToContent_SetMinSize?.Invoke(entity.EntityID, value.X, value.Y);
            }
        }

        /// <summary>
        /// Maximum size constraints (0 = no maximum).
        /// </summary>
        public Vector2 maxSize
        {
            get
            {
                if (!entity.IsValid || ComponentInterop.UI_FitToContent_GetMaxSize == null)
                    return Vector2.Zero;
                ComponentInterop.UI_FitToContent_GetMaxSize(entity.EntityID, out float w, out float h);
                return new Vector2(w, h);
            }
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_FitToContent_SetMaxSize?.Invoke(entity.EntityID, value.X, value.Y);
            }
        }

        /// <summary>
        /// If true, include only direct children. If false, include all descendants.
        /// </summary>
        public bool directChildrenOnly
        {
            get => entity.IsValid && ComponentInterop.UI_FitToContent_GetDirectChildrenOnly != null
                ? ComponentInterop.UI_FitToContent_GetDirectChildrenOnly(entity.EntityID)
                : true;
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_FitToContent_SetDirectChildrenOnly?.Invoke(entity.EntityID, value);
            }
        }
    }
}

