using System.Numerics;

namespace ClaymoreEngine
{
    public sealed class Panel : ComponentBase, IUIOpacity, IUIPositionable
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

        public float opacity
        {
            get => ComponentInterop.UI_Panel_GetOpacity != null ? ComponentInterop.UI_Panel_GetOpacity(entity.EntityID) : 1.0f;
            set => ComponentInterop.UI_Panel_SetOpacity?.Invoke(entity.EntityID, value);
        }

        public bool driveChildrenOpacity
        {
            get => ComponentInterop.UI_Panel_GetDriveChildrenOpacity != null
                ? ComponentInterop.UI_Panel_GetDriveChildrenOpacity(entity.EntityID)
                : false;
            set => ComponentInterop.UI_Panel_SetDriveChildrenOpacity?.Invoke(entity.EntityID, value);
        }

        /// <summary>
        /// Gets or sets the texture displayed on this panel.
        /// </summary>
        public Texture texture
        {
            get
            {
                if (TextureInterop.UI_Panel_GetTexture == null)
                    return default;
                TextureInterop.UI_Panel_GetTexture(entity.EntityID, out ulong hi, out ulong lo, out int fileId);
                if (hi == 0 && lo == 0)
                    return default;
                string guid = hi.ToString("x16") + lo.ToString("x16");
                return new Texture { Guid = guid, FileID = fileId };
            }
            set
            {
                if (TextureInterop.UI_Panel_SetTexture == null || !value.IsValid)
                    return;
                if (!value.TryParseGuid(out ulong hi, out ulong lo))
                    return;
                TextureInterop.UI_Panel_SetTexture(entity.EntityID, hi, lo, value.FileID);
            }
        }
        
        public bool visible
        {
            get => ComponentInterop.UI_Panel_GetVisible != null ? ComponentInterop.UI_Panel_GetVisible(entity.EntityID) : true;
            set => ComponentInterop.UI_Panel_SetVisible?.Invoke(entity.EntityID, value);
        }

        public bool allowDrag
        {
            get => ComponentInterop.UI_Panel_GetAllowDrag != null ? ComponentInterop.UI_Panel_GetAllowDrag(entity.EntityID) : false;
            set => ComponentInterop.UI_Panel_SetAllowDrag?.Invoke(entity.EntityID, value);
        }

        public bool allowDrop
        {
            get => ComponentInterop.UI_Panel_GetAllowDrop != null ? ComponentInterop.UI_Panel_GetAllowDrop(entity.EntityID) : false;
            set => ComponentInterop.UI_Panel_SetAllowDrop?.Invoke(entity.EntityID, value);
        }
        
        public Vector2 size
        {
            get
            {
                if (ComponentInterop.UI_Panel_GetSize == null) return new Vector2(100, 100);
                ComponentInterop.UI_Panel_GetSize(entity.EntityID, out float w, out float h);
                return new Vector2(w, h);
            }
            set => ComponentInterop.UI_Panel_SetSize?.Invoke(entity.EntityID, value.X, value.Y);
        }
        
        public Vector4 tintColor
        {
            get
            {
                if (ComponentInterop.UI_Panel_GetTintColor == null) return Vector4.One;
                ComponentInterop.UI_Panel_GetTintColor(entity.EntityID, out float r, out float g, out float b, out float a);
                return new Vector4(r, g, b, a);
            }
            set => ComponentInterop.UI_Panel_SetTintColor?.Invoke(entity.EntityID, value.X, value.Y, value.Z, value.W);
        }
        
        public bool anchorEnabled
        {
            get => ComponentInterop.UI_Panel_GetAnchorEnabled != null ? ComponentInterop.UI_Panel_GetAnchorEnabled(entity.EntityID) : false;
            set => ComponentInterop.UI_Panel_SetAnchorEnabled?.Invoke(entity.EntityID, value);
        }
        
        public UIAnchorPreset anchor
        {
            get => ComponentInterop.UI_Panel_GetAnchor != null ? (UIAnchorPreset)ComponentInterop.UI_Panel_GetAnchor(entity.EntityID) : UIAnchorPreset.TopLeft;
            set => ComponentInterop.UI_Panel_SetAnchor?.Invoke(entity.EntityID, (int)value);
        }

        public bool anchorToParentUI
        {
            get => ComponentInterop.UI_Panel_GetAnchorToParent != null ? ComponentInterop.UI_Panel_GetAnchorToParent(entity.EntityID) : false;
            set => ComponentInterop.UI_Panel_SetAnchorToParent?.Invoke(entity.EntityID, value);
        }
        
        public Vector2 anchorOffset
        {
            get
            {
                if (ComponentInterop.UI_Panel_GetAnchorOffset == null) return Vector2.Zero;
                ComponentInterop.UI_Panel_GetAnchorOffset(entity.EntityID, out float x, out float y);
                return new Vector2(x, y);
            }
            set => ComponentInterop.UI_Panel_SetAnchorOffset?.Invoke(entity.EntityID, value.X, value.Y);
        }
        
        public int zOrder
        {
            get => ComponentInterop.UI_Panel_GetZOrder != null ? ComponentInterop.UI_Panel_GetZOrder(entity.EntityID) : 0;
            set => ComponentInterop.UI_Panel_SetZOrder?.Invoke(entity.EntityID, value);
        }

        public bool hovered => ComponentInterop.UI_Panel_IsHovered != null
            ? ComponentInterop.UI_Panel_IsHovered(entity.EntityID)
            : false;

        public bool pressed => ComponentInterop.UI_Panel_IsPressed != null
            ? ComponentInterop.UI_Panel_IsPressed(entity.EntityID)
            : false;

        public bool dragging => ComponentInterop.UI_Panel_IsDragging != null
            ? ComponentInterop.UI_Panel_IsDragging(entity.EntityID)
            : false;

        public bool dragStarted => ComponentInterop.UI_Panel_DragStarted != null
            ? ComponentInterop.UI_Panel_DragStarted(entity.EntityID)
            : false;

        public bool dragEnded => ComponentInterop.UI_Panel_DragEnded != null
            ? ComponentInterop.UI_Panel_DragEnded(entity.EntityID)
            : false;

        public bool wasDropped => ComponentInterop.UI_Panel_WasDropped != null
            ? ComponentInterop.UI_Panel_WasDropped(entity.EntityID)
            : false;

        public Entity dropSource => new Entity(ComponentInterop.UI_Panel_GetDropSource != null
            ? ComponentInterop.UI_Panel_GetDropSource(entity.EntityID)
            : -1);

        public Entity dropTarget => new Entity(ComponentInterop.UI_Panel_GetDropTarget != null
            ? ComponentInterop.UI_Panel_GetDropTarget(entity.EntityID)
            : -1);
    }
}
