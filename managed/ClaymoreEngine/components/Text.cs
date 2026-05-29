using System.Numerics;

namespace ClaymoreEngine
{
    /// <summary>
    /// UI anchor preset for text positioning.
    /// </summary>
    public enum UIAnchorPreset
    {
        TopLeft = 0,
        Top = 1,
        TopRight = 2,
        Left = 3,
        Center = 4,
        Right = 5,
        BottomLeft = 6,
        Bottom = 7,
        BottomRight = 8
    }

    public enum TextAlignment
    {
        Left = 0,
        Center = 1,
        Right = 2
    }

    public sealed class Text : ComponentBase, IUIOpacity, IUIPositionable
    {
        /// <summary>
        /// The text content to display.
        /// </summary>
        public string text
        {
            get
            {
                if (!entity.IsValid || ComponentInterop.UI_Text_GetText == null) return string.Empty;
                ComponentInterop.UI_Text_GetText(entity.EntityID, out var ptr);
                return ptr != System.IntPtr.Zero ? System.Runtime.InteropServices.Marshal.PtrToStringAnsi(ptr) : string.Empty;
            }
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_Text_SetText?.Invoke(entity.EntityID, value ?? string.Empty);
            }
        }

        /// <summary>
        /// Additional opacity multiplier (0..1), applied on top of color alpha.
        /// </summary>
        public float opacity
        {
            get => entity.IsValid && ComponentInterop.UI_Text_GetOpacity != null ? ComponentInterop.UI_Text_GetOpacity(entity.EntityID) : 1.0f;
            set { if (entity.IsValid) ComponentInterop.UI_Text_SetOpacity?.Invoke(entity.EntityID, value); }
        }

        /// <summary>
        /// Whether the text is visible.
        /// </summary>
        public bool visible
        {
            get => entity.IsValid && ComponentInterop.UI_Text_GetVisible != null ? ComponentInterop.UI_Text_GetVisible(entity.EntityID) : true;
            set { if (entity.IsValid) ComponentInterop.UI_Text_SetVisible?.Invoke(entity.EntityID, value); }
        }

        /// <summary>
        /// Text color as RGBA (0-1 range).
        /// </summary>
        public Vector4 color
        {
            get
            {
                if (!entity.IsValid || ComponentInterop.UI_Text_GetColor == null) return new Vector4(1, 1, 1, 1);
                ComponentInterop.UI_Text_GetColor(entity.EntityID, out float r, out float g, out float b, out float a);
                return new Vector4(r, g, b, a);
            }
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_Text_SetColor?.Invoke(entity.EntityID, value.X, value.Y, value.Z, value.W);
            }
        }

        /// <summary>
        /// Enables or disables text outline rendering.
        /// </summary>
        public bool outlineEnabled
        {
            get => entity.IsValid && ComponentInterop.UI_Text_GetOutlineEnabled != null
                ? ComponentInterop.UI_Text_GetOutlineEnabled(entity.EntityID)
                : false;
            set { if (entity.IsValid) ComponentInterop.UI_Text_SetOutlineEnabled?.Invoke(entity.EntityID, value); }
        }

        /// <summary>
        /// Outline color as RGBA (0-1 range).
        /// </summary>
        public Vector4 outlineColor
        {
            get
            {
                if (!entity.IsValid || ComponentInterop.UI_Text_GetOutlineColor == null) return new Vector4(0, 0, 0, 1);
                ComponentInterop.UI_Text_GetOutlineColor(entity.EntityID, out float r, out float g, out float b, out float a);
                return new Vector4(r, g, b, a);
            }
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_Text_SetOutlineColor?.Invoke(entity.EntityID, value.X, value.Y, value.Z, value.W);
            }
        }

        /// <summary>
        /// Outline thickness in pixels.
        /// </summary>
        public float outlineThickness
        {
            get => entity.IsValid && ComponentInterop.UI_Text_GetOutlineThickness != null
                ? ComponentInterop.UI_Text_GetOutlineThickness(entity.EntityID)
                : 1.0f;
            set { if (entity.IsValid) ComponentInterop.UI_Text_SetOutlineThickness?.Invoke(entity.EntityID, value); }
        }

        /// <summary>
        /// Enables or disables text drop shadow rendering.
        /// </summary>
        public bool shadowEnabled
        {
            get => entity.IsValid && ComponentInterop.UI_Text_GetShadowEnabled != null
                ? ComponentInterop.UI_Text_GetShadowEnabled(entity.EntityID)
                : false;
            set { if (entity.IsValid) ComponentInterop.UI_Text_SetShadowEnabled?.Invoke(entity.EntityID, value); }
        }

        /// <summary>
        /// Drop shadow color as RGBA (0-1 range).
        /// </summary>
        public Vector4 shadowColor
        {
            get
            {
                if (!entity.IsValid || ComponentInterop.UI_Text_GetShadowColor == null) return new Vector4(0, 0, 0, 0.5f);
                ComponentInterop.UI_Text_GetShadowColor(entity.EntityID, out float r, out float g, out float b, out float a);
                return new Vector4(r, g, b, a);
            }
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_Text_SetShadowColor?.Invoke(entity.EntityID, value.X, value.Y, value.Z, value.W);
            }
        }

        /// <summary>
        /// Drop shadow offset in pixels.
        /// </summary>
        public Vector2 shadowOffset
        {
            get
            {
                if (!entity.IsValid || ComponentInterop.UI_Text_GetShadowOffset == null) return new Vector2(2.0f, 2.0f);
                ComponentInterop.UI_Text_GetShadowOffset(entity.EntityID, out float x, out float y);
                return new Vector2(x, y);
            }
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_Text_SetShadowOffset?.Invoke(entity.EntityID, value.X, value.Y);
            }
        }

        /// <summary>
        /// Font pixel size (approximate height in pixels).
        /// </summary>
        public float pixelSize
        {
            get => entity.IsValid && ComponentInterop.UI_Text_GetPixelSize != null ? ComponentInterop.UI_Text_GetPixelSize(entity.EntityID) : 32.0f;
            set { if (entity.IsValid) ComponentInterop.UI_Text_SetPixelSize?.Invoke(entity.EntityID, value); }
        }

        /// <summary>
        /// Font size alias for pixelSize.
        /// </summary>
        public float fontSize
        {
            get => pixelSize;
            set => pixelSize = value;
        }

        public TextAlignment alignment
        {
            get => entity.IsValid && ComponentInterop.UI_Text_GetAlignment != null
                ? (TextAlignment)ComponentInterop.UI_Text_GetAlignment(entity.EntityID)
                : TextAlignment.Left;
            set { if (entity.IsValid) ComponentInterop.UI_Text_SetAlignment?.Invoke(entity.EntityID, (int)value); }
        }

        /// <summary>
        /// Z-order for sorting within a canvas (lower renders first).
        /// </summary>
        public int zOrder
        {
            get => entity.IsValid && ComponentInterop.UI_Text_GetZOrder != null ? ComponentInterop.UI_Text_GetZOrder(entity.EntityID) : 0;
            set { if (entity.IsValid) ComponentInterop.UI_Text_SetZOrder?.Invoke(entity.EntityID, value); }
        }

        /// <summary>
        /// Optional TTF font path from asset registry. Empty uses default font.
        /// </summary>
        public string fontPath
        {
            get => entity.IsValid && ComponentInterop.UI_Text_GetFontPathInternal != null ? ComponentInterop.UI_Text_GetFontPath(entity.EntityID) : string.Empty;
            set { if (entity.IsValid) ComponentInterop.UI_Text_SetFontPath?.Invoke(entity.EntityID, value ?? string.Empty); }
        }

        /// <summary>
        /// Whether UI anchoring is enabled for screen-space text.
        /// </summary>
        public bool anchorEnabled
        {
            get => entity.IsValid && ComponentInterop.UI_Text_GetAnchorEnabled != null ? ComponentInterop.UI_Text_GetAnchorEnabled(entity.EntityID) : false;
            set { if (entity.IsValid) ComponentInterop.UI_Text_SetAnchorEnabled?.Invoke(entity.EntityID, value); }
        }

        /// <summary>
        /// The anchor preset for positioning.
        /// </summary>
        public UIAnchorPreset anchor
        {
            get => entity.IsValid && ComponentInterop.UI_Text_GetAnchor != null ? (UIAnchorPreset)ComponentInterop.UI_Text_GetAnchor(entity.EntityID) : UIAnchorPreset.TopLeft;
            set { if (entity.IsValid) ComponentInterop.UI_Text_SetAnchor?.Invoke(entity.EntityID, (int)value); }
        }

        /// <summary>
        /// Additional pixel offset from anchor position.
        /// </summary>
        public Vector2 anchorOffset
        {
            get
            {
                if (!entity.IsValid || ComponentInterop.UI_Text_GetAnchorOffset == null) return Vector2.Zero;
                ComponentInterop.UI_Text_GetAnchorOffset(entity.EntityID, out float x, out float y);
                return new Vector2(x, y);
            }
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_Text_SetAnchorOffset?.Invoke(entity.EntityID, value.X, value.Y);
            }
        }

        /// <summary>
        /// Whether word wrapping is enabled (requires rectSize.X > 0).
        /// </summary>
        public bool wordWrap
        {
            get => entity.IsValid && ComponentInterop.UI_Text_GetWordWrap != null ? ComponentInterop.UI_Text_GetWordWrap(entity.EntityID) : false;
            set { if (entity.IsValid) ComponentInterop.UI_Text_SetWordWrap?.Invoke(entity.EntityID, value); }
        }

        /// <summary>
        /// Text bounding rectangle size in screen pixels. Used for wrapping when wordWrap is enabled.
        /// </summary>
        public Vector2 rectSize
        {
            get
            {
                if (!entity.IsValid || ComponentInterop.UI_Text_GetRectSize == null) return Vector2.Zero;
                ComponentInterop.UI_Text_GetRectSize(entity.EntityID, out float w, out float h);
                return new Vector2(w, h);
            }
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_Text_SetRectSize?.Invoke(entity.EntityID, value.X, value.Y);
            }
        }

        /// <summary>
        /// Effective UI canvas render space for this text when it is used as a UI element.
        /// This is separate from the standalone worldSpace text renderer flag below.
        /// </summary>
        public bool uiWorldSpace
        {
            get => UIRenderSpaceUtility.GetWorldSpace(entity);
            set => UIRenderSpaceUtility.SetWorldSpace(entity, value);
        }

        /// <summary>
        /// Whether a world-space UI text element should billboard toward the active camera.
        /// </summary>
        public bool uiBillboard
        {
            get => UIRenderSpaceUtility.GetBillboard(entity);
            set => UIRenderSpaceUtility.SetBillboard(entity, value);
        }

        /// <summary>
        /// Whether this text should face the active camera when rendered in world space.
        /// For canvas-backed UI text, this forwards to the nearest world-space canvas.
        /// </summary>
        public bool billboard
        {
            get
            {
                if (!entity.IsValid)
                    return true;

                if (worldSpace)
                {
                    return ComponentInterop.UI_Text_GetBillboard != null
                        ? ComponentInterop.UI_Text_GetBillboard(entity.EntityID)
                        : true;
                }

                return UIRenderSpaceUtility.GetBillboard(entity);
            }
            set
            {
                if (!entity.IsValid)
                    return;

                if (worldSpace)
                {
                    ComponentInterop.UI_Text_SetBillboard?.Invoke(entity.EntityID, value);
                    return;
                }

                UIRenderSpaceUtility.SetBillboard(entity, value);
            }
        }

        /// <summary>
        /// Render using the standalone 3D text renderer instead of the canvas-backed UI path.
        /// This cannot be enabled while the entity participates in canvas-backed UI.
        /// </summary>
        public bool worldSpace
        {
            get => entity.IsValid && ComponentInterop.UI_Text_GetWorldSpace != null ? ComponentInterop.UI_Text_GetWorldSpace(entity.EntityID) : true;
            set { if (entity.IsValid) ComponentInterop.UI_Text_SetWorldSpace?.Invoke(entity.EntityID, value); }
        }
    }
}


