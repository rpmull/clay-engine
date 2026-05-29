using System.Numerics;

namespace ClaymoreEngine
{
    /// <summary>
    /// Renders a scene view into a UI panel using a render texture.
    /// Attach to an entity with a Panel component.
    /// </summary>
    public sealed class UISceneCapture : ComponentBase
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

        public bool enabled
        {
            get => entity.IsValid && ComponentInterop.UI_SceneCapture_GetEnabled != null
                ? ComponentInterop.UI_SceneCapture_GetEnabled(entity.EntityID)
                : true;
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_SceneCapture_SetEnabled?.Invoke(entity.EntityID, value);
            }
        }

        public bool autoFrame
        {
            get => entity.IsValid && ComponentInterop.UI_SceneCapture_GetAutoFrame != null
                ? ComponentInterop.UI_SceneCapture_GetAutoFrame(entity.EntityID)
                : true;
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_SceneCapture_SetAutoFrame?.Invoke(entity.EntityID, value);
            }
        }

        public bool includeChildren
        {
            get => entity.IsValid && ComponentInterop.UI_SceneCapture_GetIncludeChildren != null
                ? ComponentInterop.UI_SceneCapture_GetIncludeChildren(entity.EntityID)
                : true;
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_SceneCapture_SetIncludeChildren?.Invoke(entity.EntityID, value);
            }
        }

        public float boundsPadding
        {
            get => entity.IsValid && ComponentInterop.UI_SceneCapture_GetBoundsPadding != null
                ? ComponentInterop.UI_SceneCapture_GetBoundsPadding(entity.EntityID)
                : 1.15f;
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_SceneCapture_SetBoundsPadding?.Invoke(entity.EntityID, value);
            }
        }

        public float fieldOfView
        {
            get => entity.IsValid && ComponentInterop.UI_SceneCapture_GetFieldOfView != null
                ? ComponentInterop.UI_SceneCapture_GetFieldOfView(entity.EntityID)
                : 60.0f;
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_SceneCapture_SetFieldOfView?.Invoke(entity.EntityID, value);
            }
        }

        public float nearClip
        {
            get => entity.IsValid && ComponentInterop.UI_SceneCapture_GetNearClip != null
                ? ComponentInterop.UI_SceneCapture_GetNearClip(entity.EntityID)
                : 0.1f;
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_SceneCapture_SetNearClip?.Invoke(entity.EntityID, value);
            }
        }

        public float farClip
        {
            get => entity.IsValid && ComponentInterop.UI_SceneCapture_GetFarClip != null
                ? ComponentInterop.UI_SceneCapture_GetFarClip(entity.EntityID)
                : 500.0f;
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_SceneCapture_SetFarClip?.Invoke(entity.EntityID, value);
            }
        }

        public Vector3 viewDirection
        {
            get
            {
                if (!entity.IsValid || ComponentInterop.UI_SceneCapture_GetViewDirection == null)
                    return new Vector3(0.0f, 0.0f, 1.0f);
                ComponentInterop.UI_SceneCapture_GetViewDirection(entity.EntityID, out float x, out float y, out float z);
                return new Vector3(x, y, z);
            }
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_SceneCapture_SetViewDirection?.Invoke(entity.EntityID, value.X, value.Y, value.Z);
            }
        }

        public Vector3 upDirection
        {
            get
            {
                if (!entity.IsValid || ComponentInterop.UI_SceneCapture_GetUpDirection == null)
                    return new Vector3(0.0f, 1.0f, 0.0f);
                ComponentInterop.UI_SceneCapture_GetUpDirection(entity.EntityID, out float x, out float y, out float z);
                return new Vector3(x, y, z);
            }
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_SceneCapture_SetUpDirection?.Invoke(entity.EntityID, value.X, value.Y, value.Z);
            }
        }

        public Vector3 focusOffset
        {
            get
            {
                if (!entity.IsValid || ComponentInterop.UI_SceneCapture_GetFocusOffset == null)
                    return Vector3.Zero;
                ComponentInterop.UI_SceneCapture_GetFocusOffset(entity.EntityID, out float x, out float y, out float z);
                return new Vector3(x, y, z);
            }
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_SceneCapture_SetFocusOffset?.Invoke(entity.EntityID, value.X, value.Y, value.Z);
            }
        }

        public int targetEntityId
        {
            get => entity.IsValid && ComponentInterop.UI_SceneCapture_GetTargetEntity != null
                ? ComponentInterop.UI_SceneCapture_GetTargetEntity(entity.EntityID)
                : -1;
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_SceneCapture_SetTargetEntity?.Invoke(entity.EntityID, value);
            }
        }

        public Entity target
        {
            get => new Entity(targetEntityId);
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_SceneCapture_SetTargetEntity?.Invoke(entity.EntityID, value.EntityID);
            }
        }

        public Vector2 renderSize
        {
            get
            {
                if (!entity.IsValid || ComponentInterop.UI_SceneCapture_GetRenderSize == null)
                    return Vector2.Zero;
                ComponentInterop.UI_SceneCapture_GetRenderSize(entity.EntityID, out int w, out int h);
                return new Vector2(w, h);
            }
            set
            {
                if (!entity.IsValid) return;
                int w = value.X <= 0 ? 0 : (int)value.X;
                int h = value.Y <= 0 ? 0 : (int)value.Y;
                ComponentInterop.UI_SceneCapture_SetRenderSize?.Invoke(entity.EntityID, w, h);
            }
        }

        public Vector4 clearColor
        {
            get
            {
                if (!entity.IsValid || ComponentInterop.UI_SceneCapture_GetClearColor == null)
                    return Vector4.Zero;
                ComponentInterop.UI_SceneCapture_GetClearColor(entity.EntityID, out float r, out float g, out float b, out float a);
                return new Vector4(r, g, b, a);
            }
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_SceneCapture_SetClearColor?.Invoke(entity.EntityID, value.X, value.Y, value.Z, value.W);
            }
        }

        public bool showGrid
        {
            get => entity.IsValid && ComponentInterop.UI_SceneCapture_GetShowGrid != null
                ? ComponentInterop.UI_SceneCapture_GetShowGrid(entity.EntityID)
                : false;
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_SceneCapture_SetShowGrid?.Invoke(entity.EntityID, value);
            }
        }

        /// <summary>
        /// When enabled, view/up directions are interpreted in target-local space
        /// and rotated with the target's world transform.
        /// </summary>
        public bool lockViewToTarget
        {
            get => entity.IsValid && ComponentInterop.UI_SceneCapture_GetLockViewToTarget != null
                ? ComponentInterop.UI_SceneCapture_GetLockViewToTarget(entity.EntityID)
                : false;
            set
            {
                if (!entity.IsValid) return;
                ComponentInterop.UI_SceneCapture_SetLockViewToTarget?.Invoke(entity.EntityID, value);
            }
        }
    }
}
