using System.Numerics;

namespace ClaymoreEngine
{
    /// <summary>
    /// Scrollable container for UI elements.
    /// Requires a sibling PanelComponent for the viewport mask.
    /// </summary>
    public sealed class ScrollView : ComponentBase, IUIOpacity
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
        /// The opacity of the scrollbar elements (0-1).
        /// </summary>
        public float opacity
        {
            get => entity.IsValid && ComponentInterop.UI_ScrollView_GetOpacity != null
                ? ComponentInterop.UI_ScrollView_GetOpacity(entity.EntityID)
                : 1.0f;
            set { if (entity.IsValid) ComponentInterop.UI_ScrollView_SetOpacity?.Invoke(entity.EntityID, value); }
        }

        /// <summary>
        /// Whether the scrollbar elements are visible.
        /// </summary>
        public bool visible
        {
            get => entity.IsValid && ComponentInterop.UI_ScrollView_GetVisible != null
                ? ComponentInterop.UI_ScrollView_GetVisible(entity.EntityID)
                : true;
            set { if (entity.IsValid) ComponentInterop.UI_ScrollView_SetVisible?.Invoke(entity.EntityID, value); }
        }

        /// <summary>
        /// Current scroll offset of the content.
        /// </summary>
        public Vector2 ContentOffset
        {
            get
            {
                if (!entity.IsValid || ComponentInterop.UI_ScrollView_GetContentOffset == null)
                    return Vector2.Zero;
                ComponentInterop.UI_ScrollView_GetContentOffset(entity.EntityID, out float x, out float y);
                return new Vector2(x, y);
            }
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_ScrollView_SetContentOffset?.Invoke(entity.EntityID, value.X, value.Y);
            }
        }

        /// <summary>
        /// Total size of the scrollable content area.
        /// </summary>
        public Vector2 ContentSize
        {
            get
            {
                if (!entity.IsValid || ComponentInterop.UI_ScrollView_GetContentSize == null)
                    return Vector2.Zero;
                ComponentInterop.UI_ScrollView_GetContentSize(entity.EntityID, out float w, out float h);
                return new Vector2(w, h);
            }
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_ScrollView_SetContentSize?.Invoke(entity.EntityID, value.X, value.Y);
            }
        }

        /// <summary>
        /// Scrolls to make the specified position visible.
        /// </summary>
        public void ScrollTo(Vector2 position)
        {
            ContentOffset = position;
        }

        /// <summary>
        /// Resets scroll position to top-left.
        /// </summary>
        public void ResetScroll()
        {
            ContentOffset = Vector2.Zero;
        }
    }
}

