using System;
using System.Collections.Concurrent;
using System.Numerics;
using ClaymoreEngine.Physics;

namespace ClaymoreEngine
{
    public class RigidBodyComponent : ComponentBase
    {
        // Backing fields for events
        private Action<int, CollisionOtherKind>? _onCollisionEnter;
        private Action<int, CollisionOtherKind>? _onCollisionExit;
        private bool _registered;

        // Instance-scoped events with auto-registration
        public event Action<int, CollisionOtherKind> OnCollisionEnter
        {
            add { EnsureRegistered(); _onCollisionEnter += value; }
            remove { _onCollisionEnter -= value; }
        }
        public event Action<int, CollisionOtherKind> OnCollisionExit
        {
            add { EnsureRegistered(); _onCollisionExit += value; }
            remove { _onCollisionExit -= value; }
        }

        // Static registry to route global CollisionInterop events to the correct instance
        private static readonly ConcurrentDictionary<int, WeakReference<RigidBodyComponent>> s_Instances
            = new ConcurrentDictionary<int, WeakReference<RigidBodyComponent>>();

        private static bool s_Initialized;

        private static void EnsureInitialized()
        {
            if (s_Initialized) return;
            s_Initialized = true;

            CollisionInterop.Entered += (selfEntity, otherEntity, otherKind) =>
            {
                if (TryGet(selfEntity, out var inst)) inst.RaiseCollisionEnter(otherEntity, otherKind);
            };
            CollisionInterop.Exited += (selfEntity, otherEntity, otherKind) =>
            {
                if (TryGet(selfEntity, out var inst)) inst.RaiseCollisionExit(otherEntity, otherKind);
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
            s_Instances[id] = new WeakReference<RigidBodyComponent>(this);
        }

        /// <summary>
        /// Registers this RigidBodyComponent instance to receive collision events.
        /// </summary>
        public void RegisterInstance()
        {
            EnsureRegistered();
        }

        /// <summary>
        /// Unregisters this RigidBodyComponent instance from receiving collision events.
        /// </summary>
        public void UnregisterInstance()
        {
            if (!entity.IsValid) return;
            int id = entity.EntityID;
            s_Instances.TryRemove(id, out _);
            _registered = false;
        }

        private static bool TryGet(int entityId, out RigidBodyComponent instance)
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

        private void RaiseCollisionEnter(int other, CollisionOtherKind otherKind) => _onCollisionEnter?.Invoke(other, otherKind);
        private void RaiseCollisionExit(int other, CollisionOtherKind otherKind) => _onCollisionExit?.Invoke(other, otherKind);

        public float Mass
        {
            get => ComponentInterop.GetRigidBodyMass(entity.EntityID);
            set => ComponentInterop.SetRigidBodyMass(entity.EntityID, value);
        }

        public bool IsKinematic
        {
            get => ComponentInterop.GetRigidBodyIsKinematic(entity.EntityID);
            set => ComponentInterop.SetRigidBodyIsKinematic(entity.EntityID, value);
        }

        public bool UseGravity
        {
            get => ComponentInterop.GetRigidBodyUseGravity(entity.EntityID);
            set => ComponentInterop.SetRigidBodyUseGravity(entity.EntityID, value);
        }

        public uint CollisionMask
        {
            get => ComponentInterop.GetRigidBodyCollisionMask(entity.EntityID);
            set => ComponentInterop.SetRigidBodyCollisionMask(entity.EntityID, value);
        }

        public void SetCollisionMask(params string[] layerNames)
        {
            CollisionMask = LayerMask.Get(layerNames);
        }

        public void SetCollisionLayerEnabled(string layerName, bool enabled)
        {
            uint layerMask = LayerMask.Get(layerName);
            CollisionMask = enabled ? CollisionMask | layerMask : CollisionMask & ~layerMask;
        }

        public void IgnoreCollisionLayer(string layerName, bool ignore = true)
        {
            SetCollisionLayerEnabled(layerName, !ignore);
        }

        public bool CollidesWithLayer(string layerName)
        {
            return LayerMask.Includes(CollisionMask, layerName);
        }

        /// <summary>
        /// Sets the physics layer this body belongs to (the layer it is filtered by
        /// in raycasts, spherecasts and contacts). Keeps the collider and body layer
        /// in sync. The layer must already be registered (see PhysicsLayer.Register).
        /// </summary>
        /// <param name="layerName">The registered physics layer name, e.g. "Ignore".</param>
        /// <returns>True if the layer was found and applied.</returns>
        public bool SetPhysicsLayer(string layerName)
        {
            return ComponentInterop.SetRigidBodyPhysicsLayer(entity.EntityID, layerName);
        }

        public Vector3 LinearVelocity
        {
            get
            {
                ComponentInterop.GetRigidBodyLinearVelocity(entity.EntityID, out float x, out float y, out float z);
                return new Vector3(x, y, z);
            }
            set => ComponentInterop.SetRigidBodyLinearVelocity(entity.EntityID, value.X, value.Y, value.Z);
        }

        public Vector3 AngularVelocity
        {
            get
            {
                ComponentInterop.GetRigidBodyAngularVelocity(entity.EntityID, out float x, out float y, out float z);
                return new Vector3(x, y, z);
            }
            set => ComponentInterop.SetRigidBodyAngularVelocity(entity.EntityID, value.X, value.Y, value.Z);
        }

        public string DebugSummary => ComponentInterop.RigidBody_GetDebugSummary(entity.EntityID);

        public void ApplyForce(Vector3 force)
        {
            ComponentInterop.ApplyRigidBodyForce(entity.EntityID, force.X, force.Y, force.Z);
        }

        public void ApplyTorque(Vector3 torque)
        {
            ComponentInterop.ApplyRigidBodyTorque(entity.EntityID, torque.X, torque.Y, torque.Z);
        }

        public void ApplyImpulse(Vector3 impulse)
        {
            ComponentInterop.ApplyRigidBodyImpulse(entity.EntityID, impulse.X, impulse.Y, impulse.Z);
        }

        public void ApplyAngularImpulse(Vector3 impulse)
        {
            ComponentInterop.ApplyRigidBodyAngularImpulse(entity.EntityID, impulse.X, impulse.Y, impulse.Z);
        }
    }
}
