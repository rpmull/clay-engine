using System.Numerics;

namespace ClaymoreEngine
{
    /// <summary>
    /// Layout direction for LayoutGroup.
    /// </summary>
    public enum LayoutDirection
    {
        Horizontal = 0,
        Vertical = 1
    }

    /// <summary>
    /// Alignment options for layout children.
    /// </summary>
    public enum LayoutAlignment
    {
        Start = 0,
        Center = 1,
        End = 2
    }

    /// <summary>
    /// Automatic layout component that arranges child elements horizontally or vertically.
    /// Children are positioned according to padding, spacing, and alignment settings.
    /// </summary>
    public sealed class LayoutGroup : ComponentBase
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
        /// Layout direction (Horizontal or Vertical).
        /// </summary>
        public LayoutDirection direction
        {
            get => entity.IsValid && ComponentInterop.UI_LayoutGroup_GetDirection != null
                ? (LayoutDirection)ComponentInterop.UI_LayoutGroup_GetDirection(entity.EntityID)
                : LayoutDirection.Vertical;
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_LayoutGroup_SetDirection?.Invoke(entity.EntityID, (int)value);
            }
        }

        /// <summary>
        /// Padding around the layout area (left, top, right, bottom).
        /// </summary>
        public Vector4 padding
        {
            get
            {
                if (!entity.IsValid || ComponentInterop.UI_LayoutGroup_GetPadding == null)
                    return new Vector4(10f, 10f, 10f, 10f);
                ComponentInterop.UI_LayoutGroup_GetPadding(entity.EntityID, out float l, out float t, out float r, out float b);
                return new Vector4(l, t, r, b);
            }
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_LayoutGroup_SetPadding?.Invoke(entity.EntityID, value.X, value.Y, value.Z, value.W);
            }
        }

        /// <summary>
        /// Spacing between child elements in pixels.
        /// </summary>
        public float spacing
        {
            get => entity.IsValid && ComponentInterop.UI_LayoutGroup_GetSpacing != null
                ? ComponentInterop.UI_LayoutGroup_GetSpacing(entity.EntityID)
                : 5f;
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_LayoutGroup_SetSpacing?.Invoke(entity.EntityID, value);
            }
        }

        /// <summary>
        /// Alignment of children along the layout axis (Start, Center, End).
        /// </summary>
        public LayoutAlignment childAlignment
        {
            get => entity.IsValid && ComponentInterop.UI_LayoutGroup_GetChildAlignment != null
                ? (LayoutAlignment)ComponentInterop.UI_LayoutGroup_GetChildAlignment(entity.EntityID)
                : LayoutAlignment.Start;
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_LayoutGroup_SetChildAlignment?.Invoke(entity.EntityID, (int)value);
            }
        }

        /// <summary>
        /// Alignment of children perpendicular to the layout axis (Start, Center, End).
        /// </summary>
        public LayoutAlignment crossAlignment
        {
            get => entity.IsValid && ComponentInterop.UI_LayoutGroup_GetCrossAlignment != null
                ? (LayoutAlignment)ComponentInterop.UI_LayoutGroup_GetCrossAlignment(entity.EntityID)
                : LayoutAlignment.Start;
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_LayoutGroup_SetCrossAlignment?.Invoke(entity.EntityID, (int)value);
            }
        }

        /// <summary>
        /// If true, forces all children to have the same width (in vertical layout).
        /// </summary>
        public bool controlChildWidth
        {
            get => entity.IsValid && ComponentInterop.UI_LayoutGroup_GetControlChildWidth != null
                ? ComponentInterop.UI_LayoutGroup_GetControlChildWidth(entity.EntityID)
                : false;
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_LayoutGroup_SetControlChildWidth?.Invoke(entity.EntityID, value);
            }
        }

        /// <summary>
        /// If true, forces all children to have the same height (in horizontal layout).
        /// </summary>
        public bool controlChildHeight
        {
            get => entity.IsValid && ComponentInterop.UI_LayoutGroup_GetControlChildHeight != null
                ? ComponentInterop.UI_LayoutGroup_GetControlChildHeight(entity.EntityID)
                : false;
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_LayoutGroup_SetControlChildHeight?.Invoke(entity.EntityID, value);
            }
        }

        /// <summary>
        /// If true, reverses the layout order of children.
        /// </summary>
        public bool reverseOrder
        {
            get => entity.IsValid && ComponentInterop.UI_LayoutGroup_GetReverseOrder != null
                ? ComponentInterop.UI_LayoutGroup_GetReverseOrder(entity.EntityID)
                : false;
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_LayoutGroup_SetReverseOrder?.Invoke(entity.EntityID, value);
            }
        }
    }
}
