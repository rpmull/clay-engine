using System;

namespace ClaymoreEngine
{
    public class CameraComponent : ComponentBase
    {
        /// <summary>
        /// Layer mask for culling (bit i corresponds to EntityData::Layer == i)
        /// </summary>
        public uint LayerMask
        {
            get => ComponentInterop.GetCameraLayerMask(entity.EntityID);
            set => ComponentInterop.SetCameraLayerMask(entity.EntityID, value);
        }

        /// <summary>
        /// Whether this camera is active and considered for rendering
        /// </summary>
        public bool Active
        {
            get => ComponentInterop.GetCameraActive?.Invoke(entity.EntityID) ?? false;
            set => ComponentInterop.SetCameraActive?.Invoke(entity.EntityID, value);
        }

        /// <summary>
        /// Camera priority - lower values render first, higher values render last
        /// </summary>
        public int Priority
        {
            get => ComponentInterop.GetCameraPriority?.Invoke(entity.EntityID) ?? 0;
            set => ComponentInterop.SetCameraPriority?.Invoke(entity.EntityID, value);
        }

        /// <summary>
        /// Field of view in degrees (for perspective projection)
        /// </summary>
        public float FieldOfView
        {
            get => ComponentInterop.GetCameraFieldOfView?.Invoke(entity.EntityID) ?? 60.0f;
            set => ComponentInterop.SetCameraFieldOfView?.Invoke(entity.EntityID, value);
        }

        /// <summary>
        /// Near clipping plane distance
        /// </summary>
        public float NearClip
        {
            get => ComponentInterop.GetCameraNearClip?.Invoke(entity.EntityID) ?? 0.1f;
            set => ComponentInterop.SetCameraNearClip?.Invoke(entity.EntityID, value);
        }

        /// <summary>
        /// Far clipping plane distance
        /// </summary>
        public float FarClip
        {
            get => ComponentInterop.GetCameraFarClip?.Invoke(entity.EntityID) ?? 1000.0f;
            set => ComponentInterop.SetCameraFarClip?.Invoke(entity.EntityID, value);
        }

        /// <summary>
        /// True for perspective projection, false for orthographic
        /// </summary>
        public bool IsPerspective
        {
            get => ComponentInterop.GetCameraIsPerspective?.Invoke(entity.EntityID) ?? true;
            set => ComponentInterop.SetCameraIsPerspective?.Invoke(entity.EntityID, value);
        }

        /// <summary>
        /// Enable/disable a single camera mask bit by project layer name
        /// </summary>
        public void SetLayerMask(string layerName, bool enable)
        {
            ComponentInterop.Camera_SetLayerMaskByName?.Invoke(entity.EntityID, layerName, enable);
        }
    }
}


