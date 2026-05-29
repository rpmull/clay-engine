using System;

namespace ClaymoreEngine.Multiplayer
{
    public abstract class NetworkBehaviour : ScriptComponent
    {
        internal ulong InternalNetworkId;
        internal ulong InternalOwnerPeerId;
        internal bool InternalSpawned;
        internal bool InternalDynamicSpawn;
        internal bool InternalSpawnCallbackInvoked;

        public ulong NetworkId => InternalNetworkId;
        public ulong OwnerPeerId => InternalOwnerPeerId;
        public bool IsSpawned => InternalSpawned;
        public bool IsDynamicSpawn => InternalDynamicSpawn;

        public bool SynchronizeTransform { get; protected set; } = true;
        public bool SynchronizeScale { get; protected set; } = false;
        public bool InterpolateRemoteTransform { get; protected set; } = true;
        public bool RunUpdateOnProxy { get; protected set; } = false;
        public bool AllowOwnerSimulation { get; protected set; } = false;
        public float PositionThreshold { get; protected set; } = 0.001f;
        public float RotationThresholdDegrees { get; protected set; } = 0.25f;
        public float ScaleThreshold { get; protected set; } = 0.001f;
        public float InterpolationSpeed { get; protected set; } = 15.0f;

        public bool IsServer => NetworkManager.IsServer;
        public bool IsClient => NetworkManager.IsClient;
        public bool IsHost => NetworkManager.IsHost;
        public ulong LocalPeerId => NetworkManager.LocalPeerId;
        public bool HasStateAuthority => NetworkManager.HasStateAuthority(this);
        public bool HasInputAuthority => NetworkManager.HasInputAuthority(this);

        public bool ShouldProcessLocalUpdate =>
            !NetworkManager.IsSessionActive ||
            RunUpdateOnProxy ||
            HasStateAuthority ||
            (AllowOwnerSimulation && HasInputAuthority);

        public sealed override void Bind(Entity entity)
        {
            base.Bind(entity);
            ClaymoreEngine.Networking.MultiplayerRuntime.RegisterBehaviour(this);
        }

        public virtual void OnNetworkSpawn() { }
        public virtual void OnNetworkDespawn() { }
        public virtual void OnOwnershipChanged(ulong previousOwnerPeerId, ulong newOwnerPeerId) { }

        protected void SendRpc(string methodName, params object[] args) => NetworkManager.SendRpc(this, methodName, args);
        protected void SendServerRpc(string methodName, params object[] args) => NetworkManager.SendRpc(this, methodName, args);
        protected void SendObserversRpc(string methodName, params object[] args) => NetworkManager.SendRpc(this, methodName, args);
        protected void SendOwnerRpc(string methodName, params object[] args) => NetworkManager.SendRpc(this, methodName, args);
    }
}
