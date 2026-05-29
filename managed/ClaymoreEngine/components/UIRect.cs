using System.Numerics;

namespace ClaymoreEngine
{
    /// <summary>
    /// Enables parent-relative anchoring for UI elements.
    /// Requires the parent to have a Panel or UIRect with a computed rect.
    /// </summary>
    public sealed class UIRect : ComponentBase
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

        public bool anchorToParent
        {
            get => entity.IsValid && ComponentInterop.UI_Rect_GetAnchorToParent != null
                ? ComponentInterop.UI_Rect_GetAnchorToParent(entity.EntityID)
                : false;
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_Rect_SetAnchorToParent?.Invoke(entity.EntityID, value);
            }
        }

        public float horizontalAnchor
        {
            get => entity.IsValid && ComponentInterop.UI_Rect_GetHorizontalAnchor != null
                ? ComponentInterop.UI_Rect_GetHorizontalAnchor(entity.EntityID)
                : 0.5f;
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_Rect_SetHorizontalAnchor?.Invoke(entity.EntityID, value);
            }
        }

        public float verticalAnchor
        {
            get => entity.IsValid && ComponentInterop.UI_Rect_GetVerticalAnchor != null
                ? ComponentInterop.UI_Rect_GetVerticalAnchor(entity.EntityID)
                : 0.5f;
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_Rect_SetVerticalAnchor?.Invoke(entity.EntityID, value);
            }
        }

        public Vector2 pivot
        {
            get
            {
                if (!entity.IsValid || ComponentInterop.UI_Rect_GetPivot == null)
                    return new Vector2(0.5f, 0.5f);
                ComponentInterop.UI_Rect_GetPivot(entity.EntityID, out float x, out float y);
                return new Vector2(x, y);
            }
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_Rect_SetPivot?.Invoke(entity.EntityID, value.X, value.Y);
            }
        }

        public Vector2 offset
        {
            get
            {
                if (!entity.IsValid || ComponentInterop.UI_Rect_GetOffset == null)
                    return Vector2.Zero;
                ComponentInterop.UI_Rect_GetOffset(entity.EntityID, out float x, out float y);
                return new Vector2(x, y);
            }
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_Rect_SetOffset?.Invoke(entity.EntityID, value.X, value.Y);
            }
        }

        public Vector2 size
        {
            get
            {
                if (!entity.IsValid || ComponentInterop.UI_Rect_GetSize == null)
                    return Vector2.Zero;
                ComponentInterop.UI_Rect_GetSize(entity.EntityID, out float w, out float h);
                return new Vector2(w, h);
            }
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_Rect_SetSize?.Invoke(entity.EntityID, value.X, value.Y);
            }
        }
    }
}

