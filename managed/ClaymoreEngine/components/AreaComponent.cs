using System;
using System.Collections.Concurrent;

namespace ClaymoreEngine
{
    public sealed class AreaComponent : ComponentBase
    {
        // Backing fields for events
        private Action<int>? _onBodyEntered;
        private Action<int>? _onBodyExited;
        private Action<int>? _onAreaEntered;
        private Action<int>? _onAreaExited;
        private bool _registered;

        // Instance-scoped events with auto-registration
        public event Action<int> OnBodyEntered
        {
            add { EnsureRegistered(); _onBodyEntered += value; }
            remove { _onBodyEntered -= value; }
        }
        public event Action<int> OnBodyExited
        {
            add { EnsureRegistered(); _onBodyExited += value; }
            remove { _onBodyExited -= value; }
        }
        public event Action<int> OnAreaEntered
        {
            add { EnsureRegistered(); _onAreaEntered += value; }
            remove { _onAreaEntered -= value; }
        }
        public event Action<int> OnAreaExited
        {
            add { EnsureRegistered(); _onAreaExited += value; }
            remove { _onAreaExited -= value; }
        }

        // Static registry to route global AreaInterop events to the correct instance
        private static readonly ConcurrentDictionary<int, WeakReference<AreaComponent>> s_Instances
            = new ConcurrentDictionary<int, WeakReference<AreaComponent>>();

        private static bool s_Initialized;

        private static void EnsureInitialized()
        {
            if (s_Initialized) return;
            s_Initialized = true;

            AreaInterop.BodyEntered += (areaEntity, otherEntity) =>
            {
                if (TryGet(areaEntity, out var inst)) inst.RaiseBodyEntered(otherEntity);
            };
            AreaInterop.BodyExited += (areaEntity, otherEntity) =>
            {
                if (TryGet(areaEntity, out var inst)) inst.RaiseBodyExited(otherEntity);
            };
            AreaInterop.AreaEntered += (areaEntity, otherEntity) =>
            {
                if (TryGet(areaEntity, out var inst)) inst.RaiseAreaEntered(otherEntity);
            };
            AreaInterop.AreaExited += (areaEntity, otherEntity) =>
            {
                if (TryGet(areaEntity, out var inst)) inst.RaiseAreaExited(otherEntity);
            };
        }

        /// <summary>
        /// Ensures this instance is registered to receive events. Called automatically when subscribing to events.
        /// </summary>
        private void EnsureRegistered()
        {
            if (_registered) return;
            _registered = true;
            EnsureInitialized();
            int id = entity.EntityID;
            s_Instances[id] = new WeakReference<AreaComponent>(this);
        }

        /// <summary>
        /// Registers this AreaComponent instance to receive area events.
        /// Note: This is now called automatically when subscribing to events.
        /// </summary>
        public void RegisterInstance()
        {
            EnsureRegistered();
        }

        /// <summary>
        /// Unregisters this AreaComponent instance from receiving area events.
        /// </summary>
        public void UnregisterInstance()
        {
            if (entity == null) return;
            int id = entity.EntityID;
            s_Instances.TryRemove(id, out _);
            _registered = false;
        }

        private static bool TryGet(int entityId, out AreaComponent instance)
        {
            instance = null;
            if (s_Instances.TryGetValue(entityId, out var weak))
            {
                if (weak.TryGetTarget(out var inst))
                {
                    instance = inst;
                    return true;
                }
                else
                {
                    s_Instances.TryRemove(entityId, out _); // cleanup
                }
            }
            return false;
        }

        private void RaiseBodyEntered(int other) => _onBodyEntered?.Invoke(other);
        private void RaiseBodyExited(int other)  => _onBodyExited?.Invoke(other);
        private void RaiseAreaEntered(int other) => _onAreaEntered?.Invoke(other);
        private void RaiseAreaExited(int other)  => _onAreaExited?.Invoke(other);
    }
}


