using System;
using System.Collections.Concurrent;
using System.Numerics;

namespace ClaymoreEngine
{
    public sealed class PortalComponent : ComponentBase
    {
        private Action<int, bool>? _onPortalCrossed;
        private bool _registered;

        public event Action<int, bool> PortalCrossed
        {
            add { EnsureRegistered(); _onPortalCrossed += value; }
            remove { _onPortalCrossed -= value; }
        }

        private static readonly ConcurrentDictionary<int, WeakReference<PortalComponent>> s_Instances
            = new ConcurrentDictionary<int, WeakReference<PortalComponent>>();

        private static bool s_Initialized;

        private static void EnsureInitialized()
        {
            if (s_Initialized) return;
            s_Initialized = true;

            PortalInterop.PortalCrossed += (portalEntity, otherEntity, entering) =>
            {
                if (TryGet(portalEntity, out var inst)) inst.RaisePortalCrossed(otherEntity, entering);
            };
        }

        private void EnsureRegistered()
        {
            if (_registered) return;
            _registered = true;
            EnsureInitialized();
            int id = entity.EntityID;
            s_Instances[id] = new WeakReference<PortalComponent>(this);
        }

        public void RegisterInstance()
        {
            EnsureRegistered();
        }

        public void UnregisterInstance()
        {
            if (entity == null) return;
            int id = entity.EntityID;
            s_Instances.TryRemove(id, out _);
            _registered = false;
        }

        private static bool TryGet(int entityId, out PortalComponent instance)
        {
            instance = null;
            if (s_Instances.TryGetValue(entityId, out var weak))
            {
                if (weak.TryGetTarget(out var inst))
                {
                    instance = inst;
                    return true;
                }
                s_Instances.TryRemove(entityId, out _);
            }
            return false;
        }

        private void RaisePortalCrossed(int other, bool entering) => _onPortalCrossed?.Invoke(other, entering);

        public bool Enabled
        {
            get => ComponentInterop.Portal_GetEnabled != null && ComponentInterop.Portal_GetEnabled(entity.EntityID);
            set => ComponentInterop.Portal_SetEnabled?.Invoke(entity.EntityID, value);
        }

        public string TargetScenePath
        {
            get => ComponentInterop.Portal_GetTargetScenePath(entity.EntityID);
            set => ComponentInterop.Portal_SetTargetScenePath?.Invoke(entity.EntityID, value ?? string.Empty);
        }

        public string TargetPortalGuid
        {
            get => ComponentInterop.Portal_GetTargetPortalGuid(entity.EntityID);
            set => ComponentInterop.Portal_SetTargetPortalGuid?.Invoke(entity.EntityID, value ?? string.Empty);
        }

        public string TargetPortalPath
        {
            get => ComponentInterop.Portal_GetTargetPortalPath(entity.EntityID);
            set => ComponentInterop.Portal_SetTargetPortalPath?.Invoke(entity.EntityID, value ?? string.Empty);
        }

        public Vector3 EntryOffset
        {
            get
            {
                float x = 0, y = 0, z = 0;
                ComponentInterop.Portal_GetEntryOffset?.Invoke(entity.EntityID, out x, out y, out z);
                return new Vector3(x, y, z);
            }
            set => ComponentInterop.Portal_SetEntryOffset?.Invoke(entity.EntityID, value.X, value.Y, value.Z);
        }

        public Vector3 ExitOffset
        {
            get
            {
                float x = 0, y = 0, z = 0;
                ComponentInterop.Portal_GetExitOffset?.Invoke(entity.EntityID, out x, out y, out z);
                return new Vector3(x, y, z);
            }
            set => ComponentInterop.Portal_SetExitOffset?.Invoke(entity.EntityID, value.X, value.Y, value.Z);
        }

        public bool AutoDetect
        {
            get => ComponentInterop.Portal_GetAutoDetect != null && ComponentInterop.Portal_GetAutoDetect(entity.EntityID);
            set => ComponentInterop.Portal_SetAutoDetect?.Invoke(entity.EntityID, value);
        }

        public float TriggerRadius
        {
            get => ComponentInterop.Portal_GetTriggerRadius != null ? ComponentInterop.Portal_GetTriggerRadius(entity.EntityID) : 0f;
            set => ComponentInterop.Portal_SetTriggerRadius?.Invoke(entity.EntityID, value);
        }

        public bool FireExitEvents
        {
            get => ComponentInterop.Portal_GetFireExitEvents != null && ComponentInterop.Portal_GetFireExitEvents(entity.EntityID);
            set => ComponentInterop.Portal_SetFireExitEvents?.Invoke(entity.EntityID, value);
        }

        /// <summary>
        /// Returns the scene path at the other end of this portal (the target scene).
        /// </summary>
        public string GetConnectedScene() => TargetScenePath ?? string.Empty;
    }
}
