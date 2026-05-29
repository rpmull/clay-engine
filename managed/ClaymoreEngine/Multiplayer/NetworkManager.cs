using System;
using System.Collections.Generic;
using System.Numerics;
using System.Threading.Tasks;

namespace ClaymoreEngine.Multiplayer
{
    public enum NetworkSessionMode
    {
        Offline = 0,
        Client = 1,
        Server = 2,
        Host = 3
    }

    public static class NetworkManager
    {
        public static event Action<ulong>? PeerConnected
        {
            add => Networking.MultiplayerRuntime.PeerConnected += value;
            remove => Networking.MultiplayerRuntime.PeerConnected -= value;
        }

        public static event Action<ulong>? PeerDisconnected
        {
            add => Networking.MultiplayerRuntime.PeerDisconnected += value;
            remove => Networking.MultiplayerRuntime.PeerDisconnected -= value;
        }

        public static event Action<string>? ConnectionError
        {
            add => Networking.MultiplayerRuntime.ConnectionError += value;
            remove => Networking.MultiplayerRuntime.ConnectionError -= value;
        }

        public static NetworkSessionMode Mode => Networking.MultiplayerRuntime.Mode;
        public static ulong LocalPeerId => Networking.MultiplayerRuntime.LocalPeerId;
        public static bool IsSessionActive => Networking.MultiplayerRuntime.IsSessionActive;
        public static bool IsServer => Networking.MultiplayerRuntime.IsServer;
        public static bool IsClient => Networking.MultiplayerRuntime.IsClient;
        public static bool IsHost => Networking.MultiplayerRuntime.IsHost;
        public static IReadOnlyCollection<ulong> ConnectedPeers => Networking.MultiplayerRuntime.GetConnectedPeers();

        public static Task<bool> StartHostAsync(int port = 7777, int maxConnections = 16) =>
            Networking.MultiplayerRuntime.StartHostAsync(port, maxConnections);

        public static Task<bool> StartServerAsync(int port = 7777, int maxConnections = 16) =>
            Networking.MultiplayerRuntime.StartServerAsync(port, maxConnections);

        public static Task<bool> StartClientAsync(string host, int port = 7777) =>
            Networking.MultiplayerRuntime.StartClientAsync(host, port);

        public static void Shutdown() => Networking.MultiplayerRuntime.Shutdown();

        public static Entity SpawnPrefab(Prefab prefab, Vector3? position = null, ulong ownerPeerId = 0) =>
            Networking.MultiplayerRuntime.SpawnPrefab(prefab, position ?? Vector3.Zero, ownerPeerId);

        public static bool Despawn(NetworkBehaviour behaviour) => Networking.MultiplayerRuntime.Despawn(behaviour);

        public static bool AssignOwnership(NetworkBehaviour behaviour, ulong ownerPeerId) =>
            Networking.MultiplayerRuntime.AssignOwnership(behaviour, ownerPeerId);

        public static bool HasStateAuthority(NetworkBehaviour behaviour) =>
            Networking.MultiplayerRuntime.HasStateAuthority(behaviour);

        public static bool HasInputAuthority(NetworkBehaviour behaviour) =>
            Networking.MultiplayerRuntime.HasInputAuthority(behaviour);

        public static void SendRpc(NetworkBehaviour behaviour, string methodName, params object[] args) =>
            Networking.MultiplayerRuntime.SendRpc(behaviour, methodName, args);
    }
}
