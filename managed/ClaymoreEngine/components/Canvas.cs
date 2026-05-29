namespace ClaymoreEngine
{
    public enum CanvasRenderSpace
    {
        ScreenSpace = 0,
        WorldSpace = 1
    }

    public sealed class Canvas : ComponentBase, IUIOpacity
    {
        public float opacity
        {
            get => ComponentInterop.UI_Canvas_GetOpacity != null ? ComponentInterop.UI_Canvas_GetOpacity(entity.EntityID) : 1.0f;
            set => ComponentInterop.UI_Canvas_SetOpacity?.Invoke(entity.EntityID, value);
        }

        public CanvasRenderSpace renderSpace
        {
            get => entity.IsValid && ComponentInterop.UI_Canvas_GetRenderSpace != null
                ? (CanvasRenderSpace)ComponentInterop.UI_Canvas_GetRenderSpace(entity.EntityID)
                : CanvasRenderSpace.ScreenSpace;
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_Canvas_SetRenderSpace?.Invoke(entity.EntityID, (int)value);
            }
        }

        public bool billboard
        {
            get => entity.IsValid && ComponentInterop.UI_Canvas_GetBillboard != null
                ? ComponentInterop.UI_Canvas_GetBillboard(entity.EntityID)
                : true;
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_Canvas_SetBillboard?.Invoke(entity.EntityID, value);
            }
        }

        public bool worldSpace
        {
            get => renderSpace == CanvasRenderSpace.WorldSpace;
            set => renderSpace = value ? CanvasRenderSpace.WorldSpace : CanvasRenderSpace.ScreenSpace;
        }

        /// <summary>
        /// Whether the canvas is visible. Canvas doesn't have native visible flag,
        /// so we use opacity 0 as invisible.
        /// </summary>
        public bool visible
        {
            get => opacity > 0f;
            set { if (!value) opacity = 0f; }
        }
    }
}


