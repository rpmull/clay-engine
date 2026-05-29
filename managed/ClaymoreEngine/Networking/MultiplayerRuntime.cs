using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Numerics;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Channels;
using System.Threading.Tasks;
using ClaymoreEngine.Multiplayer;

namespace ClaymoreEngine.Networking
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void MultiplayerUpdateDelegate(float dt);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void MultiplayerVoidDelegate();

    public static class MultiplayerRuntime
    {
        private const int ProtocolVersion = 1;
        private const float SnapshotIntervalSeconds = 1.0f / 20.0f;

        private enum PacketType : byte
        {
            Hello = 1,
            Welcome = 2,
            ClientReady = 3,
            PeerJoined = 4,
            PeerLeft = 5,
            Spawn = 6,
            Despawn = 7,
            OwnershipChanged = 8,
            Rpc = 9,
            VariableDelta = 10,
            StateBaseline = 11,
            UdpHello = 12,
            Snapshot = 13,
            Disconnect = 14
        }

        private enum WireValueType : byte
        {
            Null = 0,
            Bool = 1,
            Int32 = 2,
            UInt64 = 3,
            Single = 4,
            String = 5,
            Vector2 = 6,
            Vector3 = 7,
            Quaternion = 8
        }

        private sealed class PeerConnection : IDisposable
        {
            public ulong PeerId;
            public TcpClient Tcp = null!;
            public NetworkStream Stream = null!;
            public Channel<byte[]> Outgoing = Channel.CreateUnbounded<byte[]>(
                new UnboundedChannelOptions { SingleReader = true, SingleWriter = false });
            public CancellationTokenSource Cancel = new();
            public Task? ReadTask;
            public Task? WriteTask;
            public IPEndPoint? UdpEndpoint;
            public string UdpToken = string.Empty;
            public bool IsReady;

            public void Dispose()
            {
                try { Cancel.Cancel(); } catch { }
                try { Tcp.Close(); } catch { }
                try { Stream.Dispose(); } catch { }
                try { Cancel.Dispose(); } catch { }
            }
        }

        private sealed class PendingSpawnContext
        {
            public ulong NetworkId;
            public ulong OwnerPeerId;
            public string PrefabGuid = string.Empty;
            public bool Dynamic;
            public bool Announce;
        }

        private sealed class PendingRemoteSpawn
        {
            public ulong NetworkId;
            public ulong OwnerPeerId;
            public string PrefabGuid = string.Empty;
            public Vector3 Position;
            public Quaternion Rotation = Quaternion.Identity;
            public Vector3 Scale = Vector3.One;
            public int AttemptCount;
        }

        private sealed class RpcDescriptor
        {
            public required RpcAttribute Attribute;
            public required MethodInfo Method;
            public required ulong MethodId;
            public required ParameterInfo[] Parameters;
        }

        private sealed class NetworkVariableDescriptor
        {
            public required FieldInfo Field;
            public required Type ValueType;
        }

        private sealed class BehaviourMetadata
        {
            public required string TypeKey;
            public required ulong TypeHash;
            public Dictionary<string, RpcDescriptor> RpcByName = new(StringComparer.Ordinal);
            public Dictionary<ulong, RpcDescriptor> RpcById = new();
            public Dictionary<string, NetworkVariableDescriptor> VariablesByName = new(StringComparer.Ordinal);
            public List<NetworkVariableDescriptor> Variables = new();
        }

        private sealed class NetworkObjectState
        {
            public ulong NetworkId;
            public int EntityId;
            public string EntityGuid = string.Empty;
            public string ScenePath = string.Empty;
            public string PrefabGuid = string.Empty;
            public ulong OwnerPeerId = 1;
            public bool Dynamic;
            public bool SpawnAnnouncementPending;
            public bool ReplicateTransform;
            public bool ReplicateScale;
            public bool InterpolateRemoteTransform;
            public float InterpolationSpeed = 15.0f;
            public bool HasLastSentTransform;
            public Vector3 LastSentPosition;
            public Quaternion LastSentRotation = Quaternion.Identity;
            public Vector3 LastSentScale = Vector3.One;
            public bool HasTargetTransform;
            public Vector3 TargetPosition;
            public Quaternion TargetRotation = Quaternion.Identity;
            public Vector3 TargetScale = Vector3.One;
            public readonly List<NetworkBehaviour> Behaviours = new();

            public NetworkBehaviour? PrimaryBehaviour => Behaviours.Count > 0 ? Behaviours[0] : null;
        }

        private static readonly object SyncRoot = new();
        private static readonly ConcurrentQueue<Action> MainThreadActions = new();
        private static readonly Dictionary<ulong, PeerConnection> Peers = new();
        private static readonly Dictionary<ulong, NetworkObjectState> ObjectsByNetworkId = new();
        private static readonly Dictionary<int, NetworkObjectState> ObjectsByEntityId = new();
        private static readonly Dictionary<Type, BehaviourMetadata> MetadataCache = new();
        private static readonly List<PendingRemoteSpawn> PendingRemoteSpawns = new();

        private static TcpListener? s_tcpListener;
        private static UdpClient? s_udpSocket;
        private static CancellationTokenSource? s_lifetime;
        private static IPAddress? s_serverAddress;
        private static IPEndPoint? s_serverUdpEndpoint;
        private static NetworkSessionMode s_mode = NetworkSessionMode.Offline;
        private static ulong s_localPeerId;
        private static ulong s_nextPeerId = 2;
        private static ulong s_nextDynamicNetworkId = 1;
        private static int s_udpPort;
        private static float s_snapshotAccumulator;
        private static PendingSpawnContext? s_pendingSpawnContext;
        private static bool s_clientReadyPending;
        private static int s_clientReadyCountdown;
        private static string s_expectedScenePath = string.Empty;
        private static bool s_shuttingDown;

        public static event Action<ulong>? PeerConnected;
        public static event Action<ulong>? PeerDisconnected;
        public static event Action<string>? ConnectionError;

        public static NetworkSessionMode Mode => s_mode;
        public static ulong LocalPeerId => s_localPeerId;
        public static bool IsSessionActive => s_mode != NetworkSessionMode.Offline;
        public static bool IsServer => s_mode == NetworkSessionMode.Server || s_mode == NetworkSessionMode.Host;
        public static bool IsClient => s_mode == NetworkSessionMode.Client || s_mode == NetworkSessionMode.Host;
        public static bool IsHost => s_mode == NetworkSessionMode.Host;

        public static IReadOnlyCollection<ulong> GetConnectedPeers()
        {
            lock (SyncRoot)
            {
                return Peers.Keys.ToArray();
            }
        }

        public static void PreUpdateExport(float dt)
        {
            PreUpdate(dt);
        }

        public static void PostUpdateExport(float dt) => PostUpdate(dt);
        public static void ShutdownExport() => Shutdown();

        public static Task<bool> StartHostAsync(int port, int maxConnections)
        {
            if (!StartServerInternal(port, maxConnections))
                return Task.FromResult(false);

            s_mode = NetworkSessionMode.Host;
            s_localPeerId = 1;
            ActivateRegisteredBehaviours();
            return Task.FromResult(true);
        }

        public static Task<bool> StartServerAsync(int port, int maxConnections)
        {
            if (!StartServerInternal(port, maxConnections))
                return Task.FromResult(false);

            s_mode = NetworkSessionMode.Server;
            s_localPeerId = 1;
            ActivateRegisteredBehaviours();
            return Task.FromResult(true);
        }

        public static async Task<bool> StartClientAsync(string host, int port)
        {
            if (IsSessionActive)
                return false;

            try
            {
                s_lifetime = new CancellationTokenSource();
                s_mode = NetworkSessionMode.Client;
                s_localPeerId = 0;
                s_udpPort = port;
                s_snapshotAccumulator = 0.0f;
                s_shuttingDown = false;

                TcpClient tcp = new TcpClient();
                await tcp.ConnectAsync(host, port);
                tcp.NoDelay = true;
                s_serverAddress = ((IPEndPoint)tcp.Client.RemoteEndPoint!).Address;

                var peer = new PeerConnection
                {
                    PeerId = 1,
                    Tcp = tcp,
                    Stream = tcp.GetStream()
                };

                lock (SyncRoot)
                {
                    Peers[peer.PeerId] = peer;
                }

                peer.ReadTask = Task.Run(() => ReliableReadLoop(peer, peer.Cancel.Token));
                peer.WriteTask = Task.Run(() => ReliableWriteLoop(peer, peer.Cancel.Token));

                s_udpSocket = new UdpClient(0);
                _ = Task.Run(() => UdpReceiveLoop(s_udpSocket, s_lifetime.Token));

                SendReliable(peer.PeerId, BuildPacket(PacketType.Hello, writer =>
                {
                    writer.Write(ProtocolVersion);
                    writer.Write(SceneManager.CurrentScenePath ?? string.Empty);
                }));

                return true;
            }
            catch (Exception ex)
            {
                ReportError($"Failed to start client: {ex.Message}");
                Shutdown();
                return false;
            }
        }

        public static void Shutdown()
        {
            NetworkSessionMode previousMode = s_mode;
            bool hasTransportResources;
            lock (SyncRoot)
            {
                hasTransportResources = Peers.Count > 0;
            }

            if (previousMode == NetworkSessionMode.Offline &&
                !s_shuttingDown &&
                s_lifetime == null &&
                s_tcpListener == null &&
                s_udpSocket == null &&
                !hasTransportResources)
            {
                return;
            }

            s_shuttingDown = true;

            lock (SyncRoot)
            {
                foreach (var state in ObjectsByNetworkId.Values.ToArray())
                {
                    foreach (var behaviour in state.Behaviours.ToArray())
                    {
                        DeactivateBehaviour(behaviour);
                    }
                }
            }

            try { s_lifetime?.Cancel(); } catch { }

            lock (SyncRoot)
            {
                foreach (var peer in Peers.Values.ToArray())
                {
                    peer.Dispose();
                }
                Peers.Clear();
            }

            try { s_tcpListener?.Stop(); } catch { }
            s_tcpListener = null;

            try { s_udpSocket?.Dispose(); } catch { }
            s_udpSocket = null;

            s_lifetime?.Dispose();
            s_lifetime = null;
            s_serverAddress = null;
            s_serverUdpEndpoint = null;
            s_mode = NetworkSessionMode.Offline;
            s_localPeerId = 0;
            s_nextPeerId = 2;
            s_snapshotAccumulator = 0.0f;
            s_pendingSpawnContext = null;
            PendingRemoteSpawns.Clear();
            s_clientReadyPending = false;
            s_clientReadyCountdown = 0;
            s_expectedScenePath = string.Empty;
            s_shuttingDown = false;

            if (previousMode == NetworkSessionMode.Client)
            {
                lock (SyncRoot)
                {
                    foreach (var dynamicState in ObjectsByNetworkId.Values.Where(static s => s.Dynamic).ToArray())
                    {
                        if (dynamicState.EntityId > 0)
                        {
                            EntityInterop.DestroyEntity(dynamicState.EntityId);
                        }
                    }
                }
            }
        }

        public static void RegisterBehaviour(NetworkBehaviour behaviour)
        {
            if (behaviour == null || !behaviour.self.IsValid)
                return;

            string entityGuid = NetworkingInterop.GetEntityGuid(behaviour.EntityID);
            if (string.IsNullOrWhiteSpace(entityGuid))
            {
                Console.WriteLine($"[Multiplayer] Cannot register entity {behaviour.EntityID}: GUID unavailable.");
                return;
            }

            lock (SyncRoot)
            {
                PendingSpawnContext? spawnContext = s_pendingSpawnContext;
                ulong networkId = spawnContext != null
                    ? spawnContext.NetworkId
                    : ComputeSceneObjectId(SceneManager.CurrentScenePath, entityGuid);

                if (!ObjectsByNetworkId.TryGetValue(networkId, out NetworkObjectState? state))
                {
                    state = new NetworkObjectState
                    {
                        NetworkId = networkId
                    };
                    ObjectsByNetworkId[networkId] = state;
                }

                state.EntityId = behaviour.EntityID;
                state.EntityGuid = entityGuid;
                state.ScenePath = SceneManager.CurrentScenePath ?? string.Empty;

                if (!state.Behaviours.Contains(behaviour))
                {
                    state.Behaviours.Add(behaviour);
                }

                if (spawnContext != null)
                {
                    state.Dynamic = spawnContext.Dynamic;
                    state.PrefabGuid = spawnContext.PrefabGuid;
                    state.OwnerPeerId = spawnContext.OwnerPeerId == 0 ? 1UL : spawnContext.OwnerPeerId;
                    state.SpawnAnnouncementPending = spawnContext.Announce;
                }
                else if (state.OwnerPeerId == 0)
                {
                    state.OwnerPeerId = 1;
                }

                ApplyBehaviourConfiguration(state);
                ObjectsByEntityId[behaviour.EntityID] = state;

                BehaviourMetadata metadata = GetMetadata(behaviour.GetType());
                AttachNetworkVariables(behaviour, metadata);
                ApplyBehaviourState(behaviour, state);
                ActivateBehaviourIfNeeded(behaviour);
            }
        }

        public static void UnregisterBehaviour(NetworkBehaviour behaviour)
        {
            if (behaviour == null)
                return;

            lock (SyncRoot)
            {
                if (!ObjectsByEntityId.TryGetValue(behaviour.EntityID, out NetworkObjectState? state))
                    return;

                state.Behaviours.Remove(behaviour);
                DeactivateBehaviour(behaviour);

                if (state.Behaviours.Count == 0)
                {
                    ObjectsByEntityId.Remove(behaviour.EntityID);
                    if (state.Dynamic)
                    {
                        ObjectsByNetworkId.Remove(state.NetworkId);
                    }
                }
            }
        }

        public static void NotifyVariableDirty(NetworkBehaviour behaviour)
        {
            _ = behaviour;
        }

        public static bool CanWriteVariable(NetworkBehaviour behaviour, NetworkVariableWritePermission writePermission)
        {
            if (!IsSessionActive)
                return true;

            if (writePermission == NetworkVariableWritePermission.Server)
                return HasStateAuthority(behaviour);

            return HasInputAuthority(behaviour);
        }

        public static bool HasStateAuthority(NetworkBehaviour behaviour)
        {
            if (behaviour == null)
                return false;

            return !IsSessionActive || IsServer;
        }

        public static bool HasInputAuthority(NetworkBehaviour behaviour)
        {
            if (behaviour == null || !IsSessionActive)
                return false;

            return behaviour.OwnerPeerId != 0 && behaviour.OwnerPeerId == s_localPeerId;
        }

        public static Entity SpawnPrefab(Prefab prefab, Vector3 position, ulong ownerPeerId)
        {
            if (!IsServer || !prefab.IsValid)
                return new Entity(-1);

            ulong networkId = AllocateDynamicNetworkId();
            s_pendingSpawnContext = new PendingSpawnContext
            {
                NetworkId = networkId,
                OwnerPeerId = ownerPeerId == 0 ? 1UL : ownerPeerId,
                PrefabGuid = prefab.Guid ?? string.Empty,
                Dynamic = true,
                Announce = true
            };

            try
            {
                Entity entity = prefab.Instantiate(position);
                return entity;
            }
            finally
            {
                s_pendingSpawnContext = null;
            }
        }

        public static bool Despawn(NetworkBehaviour behaviour)
        {
            if (!IsServer || behaviour == null)
                return false;

            lock (SyncRoot)
            {
                if (!ObjectsByNetworkId.TryGetValue(behaviour.NetworkId, out NetworkObjectState? state))
                    return false;

                BroadcastReliable(BuildPacket(PacketType.Despawn, writer =>
                {
                    writer.Write(state.NetworkId);
                }));

                foreach (var entry in state.Behaviours.ToArray())
                {
                    DeactivateBehaviour(entry);
                }

                ObjectsByNetworkId.Remove(state.NetworkId);
                ObjectsByEntityId.Remove(state.EntityId);
            }

            EntityInterop.DestroyEntity(behaviour.EntityID);
            return true;
        }

        public static bool AssignOwnership(NetworkBehaviour behaviour, ulong ownerPeerId)
        {
            if (!IsServer || behaviour == null)
                return false;

            lock (SyncRoot)
            {
                if (!ObjectsByNetworkId.TryGetValue(behaviour.NetworkId, out NetworkObjectState? state))
                    return false;

                ulong previousOwner = state.OwnerPeerId;
                state.OwnerPeerId = ownerPeerId == 0 ? 1UL : ownerPeerId;
                foreach (var entry in state.Behaviours)
                {
                    ulong before = entry.InternalOwnerPeerId;
                    entry.InternalOwnerPeerId = state.OwnerPeerId;
                    entry.OnOwnershipChanged(before, state.OwnerPeerId);
                }

                BroadcastReliable(BuildPacket(PacketType.OwnershipChanged, writer =>
                {
                    writer.Write(state.NetworkId);
                    writer.Write(state.OwnerPeerId);
                }));

                return previousOwner != state.OwnerPeerId;
            }
        }

        public static void SendRpc(NetworkBehaviour behaviour, string methodName, params object[] args)
        {
            if (behaviour == null || string.IsNullOrWhiteSpace(methodName))
                return;

            BehaviourMetadata metadata = GetMetadata(behaviour.GetType());
            if (!metadata.RpcByName.TryGetValue(methodName, out RpcDescriptor? descriptor))
            {
                Console.WriteLine($"[Multiplayer] RPC '{methodName}' is not registered on {metadata.TypeKey}.");
                return;
            }

            if (!IsSessionActive)
            {
                InvokeRpcLocal(behaviour.NetworkId, metadata.TypeHash, descriptor.MethodId, args.Cast<object?>().ToArray());
                return;
            }

            byte[] payload = BuildPacket(PacketType.Rpc, writer =>
            {
                writer.Write(behaviour.NetworkId);
                writer.Write(metadata.TypeHash);
                writer.Write(descriptor.MethodId);
                writer.Write(args.Length);
                foreach (object? arg in args)
                {
                    WriteValue(writer, arg?.GetType(), arg);
                }
            });

            switch (descriptor.Attribute.Targets)
            {
                case RpcTargets.Server:
                    if (IsServer)
                    {
                        InvokeRpcLocal(behaviour.NetworkId, metadata.TypeHash, descriptor.MethodId, args.Cast<object?>().ToArray());
                    }
                    else
                    {
                        SendReliable(1, payload);
                    }
                    break;

                case RpcTargets.Owner:
                    if (!IsServer)
                        return;
                    if (behaviour.OwnerPeerId == s_localPeerId)
                    {
                        InvokeRpcLocal(behaviour.NetworkId, metadata.TypeHash, descriptor.MethodId, args.Cast<object?>().ToArray());
                    }
                    else
                    {
                        SendReliable(behaviour.OwnerPeerId, payload);
                    }
                    break;

                case RpcTargets.Observers:
                    if (!IsServer)
                        return;
                    BroadcastReliable(payload);
                    break;

                case RpcTargets.Everyone:
                    if (!IsServer)
                        return;
                    BroadcastReliable(payload);
                    InvokeRpcLocal(behaviour.NetworkId, metadata.TypeHash, descriptor.MethodId, args.Cast<object?>().ToArray());
                    break;
            }
        }

        public static void PreUpdate(float dt)
        {
            while (MainThreadActions.TryDequeue(out Action? action))
            {
                action();
            }

            RetryPendingRemoteSpawns(dt);

            if (s_mode == NetworkSessionMode.Client &&
                s_clientReadyPending &&
                !SceneManager.IsLoading &&
                SceneManager.IsLoaded)
            {
                if (s_clientReadyCountdown > 0)
                {
                    s_clientReadyCountdown--;
                }
                else
                {
                    SendReliable(1, BuildPacket(PacketType.ClientReady, static _ => { }));
                    s_clientReadyPending = false;
                }
            }
        }

        public static void PostUpdate(float dt)
        {
            if (!IsSessionActive)
                return;

            lock (SyncRoot)
            {
                ApplyInterpolatedTransforms(dt);
                FlushPendingSpawns();
                FlushDirtyNetworkVariables();

                s_snapshotAccumulator += dt;
                if (s_snapshotAccumulator >= SnapshotIntervalSeconds)
                {
                    s_snapshotAccumulator = 0.0f;
                    SendSnapshots();
                }
            }
        }
        private static bool StartServerInternal(int port, int maxConnections)
        {
            if (IsSessionActive)
                return false;

            try
            {
                s_lifetime = new CancellationTokenSource();
                s_tcpListener = new TcpListener(IPAddress.Any, port);
                s_tcpListener.Start(maxConnections);
                s_udpSocket = new UdpClient(port);
                s_udpPort = port;
                s_snapshotAccumulator = 0.0f;
                s_shuttingDown = false;

                _ = Task.Run(() => AcceptLoop(s_lifetime.Token));
                _ = Task.Run(() => UdpReceiveLoop(s_udpSocket, s_lifetime.Token));
                return true;
            }
            catch (Exception ex)
            {
                ReportError($"Failed to start server: {ex.Message}");
                Shutdown();
                return false;
            }
        }

        private static async Task AcceptLoop(CancellationToken cancellationToken)
        {
            while (!cancellationToken.IsCancellationRequested && s_tcpListener != null)
            {
                try
                {
                    TcpClient tcp = await s_tcpListener.AcceptTcpClientAsync(cancellationToken);
                    tcp.NoDelay = true;

                    var peer = new PeerConnection
                    {
                        PeerId = s_nextPeerId++,
                        Tcp = tcp,
                        Stream = tcp.GetStream(),
                        UdpToken = Guid.NewGuid().ToString("N")
                    };

                    lock (SyncRoot)
                    {
                        Peers[peer.PeerId] = peer;
                    }

                    peer.ReadTask = Task.Run(() => ReliableReadLoop(peer, peer.Cancel.Token));
                    peer.WriteTask = Task.Run(() => ReliableWriteLoop(peer, peer.Cancel.Token));
                }
                catch (OperationCanceledException)
                {
                    break;
                }
                catch (Exception ex)
                {
                    ReportError($"Accept loop error: {ex.Message}");
                    await Task.Delay(50, cancellationToken);
                }
            }
        }

        private static async Task ReliableReadLoop(PeerConnection peer, CancellationToken cancellationToken)
        {
            try
            {
                while (!cancellationToken.IsCancellationRequested)
                {
                    byte[] lengthBuffer = new byte[sizeof(int)];
                    await peer.Stream.ReadExactlyAsync(lengthBuffer, cancellationToken);
                    int length = BitConverter.ToInt32(lengthBuffer, 0);
                    if (length <= 0 || length > 1024 * 1024)
                    {
                        throw new InvalidDataException("Invalid packet size.");
                    }

                    byte[] payload = new byte[length];
                    await peer.Stream.ReadExactlyAsync(payload, cancellationToken);
                    MainThreadActions.Enqueue(() => ProcessReliablePacket(peer.PeerId, payload));
                }
            }
            catch (OperationCanceledException)
            {
            }
            catch (Exception)
            {
                MainThreadActions.Enqueue(() => HandlePeerDisconnected(peer.PeerId));
            }
        }

        private static async Task ReliableWriteLoop(PeerConnection peer, CancellationToken cancellationToken)
        {
            try
            {
                await foreach (byte[] payload in peer.Outgoing.Reader.ReadAllAsync(cancellationToken))
                {
                    byte[] lengthBuffer = BitConverter.GetBytes(payload.Length);
                    await peer.Stream.WriteAsync(lengthBuffer, cancellationToken);
                    await peer.Stream.WriteAsync(payload, cancellationToken);
                    await peer.Stream.FlushAsync(cancellationToken);
                }
            }
            catch (OperationCanceledException)
            {
            }
            catch (Exception)
            {
                MainThreadActions.Enqueue(() => HandlePeerDisconnected(peer.PeerId));
            }
        }

        private static async Task UdpReceiveLoop(UdpClient udpClient, CancellationToken cancellationToken)
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                try
                {
                    UdpReceiveResult result = await udpClient.ReceiveAsync(cancellationToken);
                    byte[] payload = result.Buffer;
                    IPEndPoint remoteEndPoint = result.RemoteEndPoint;
                    MainThreadActions.Enqueue(() => ProcessUdpPacket(remoteEndPoint, payload));
                }
                catch (OperationCanceledException)
                {
                    break;
                }
                catch (ObjectDisposedException)
                {
                    break;
                }
                catch (Exception ex)
                {
                    ReportError($"UDP receive error: {ex.Message}");
                }
            }
        }

        private static void ProcessReliablePacket(ulong sourcePeerId, byte[] payload)
        {
            using var stream = new MemoryStream(payload);
            using var reader = new BinaryReader(stream, Encoding.UTF8, leaveOpen: false);
            PacketType packetType = (PacketType)reader.ReadByte();

            switch (packetType)
            {
                case PacketType.Hello:
                    HandleHello(sourcePeerId, reader);
                    break;
                case PacketType.Welcome:
                    HandleWelcome(reader);
                    break;
                case PacketType.ClientReady:
                    HandleClientReady(sourcePeerId);
                    break;
                case PacketType.PeerJoined:
                    RaisePeerConnected(reader.ReadUInt64());
                    break;
                case PacketType.PeerLeft:
                    RaisePeerDisconnected(reader.ReadUInt64());
                    break;
                case PacketType.Spawn:
                    HandleSpawn(reader);
                    break;
                case PacketType.Despawn:
                    HandleDespawn(reader.ReadUInt64());
                    break;
                case PacketType.OwnershipChanged:
                    HandleOwnershipChanged(reader.ReadUInt64(), reader.ReadUInt64());
                    break;
                case PacketType.Rpc:
                    HandleRpc(sourcePeerId, reader);
                    break;
                case PacketType.VariableDelta:
                    HandleVariableDelta(sourcePeerId, reader);
                    break;
                case PacketType.StateBaseline:
                    HandleStateBaseline(reader);
                    break;
                case PacketType.Snapshot:
                    HandleSnapshot(sourcePeerId, reader);
                    break;
                case PacketType.Disconnect:
                    if (!IsServer && stream.Position < stream.Length)
                    {
                        string reason = reader.ReadString();
                        if (!string.IsNullOrWhiteSpace(reason))
                        {
                            ReportError(reason);
                        }
                    }
                    HandlePeerDisconnected(sourcePeerId);
                    break;
            }
        }

        private static void ProcessUdpPacket(IPEndPoint remoteEndPoint, byte[] payload)
        {
            using var stream = new MemoryStream(payload);
            using var reader = new BinaryReader(stream, Encoding.UTF8, leaveOpen: false);
            PacketType packetType = (PacketType)reader.ReadByte();

            switch (packetType)
            {
                case PacketType.UdpHello:
                    HandleUdpHello(remoteEndPoint, reader);
                    break;
                case PacketType.Snapshot:
                    if (!TryResolveUdpPeer(remoteEndPoint, out ulong sourcePeerId))
                        return;
                    HandleSnapshot(sourcePeerId, reader);
                    break;
            }
        }

        private static void HandleHello(ulong sourcePeerId, BinaryReader reader)
        {
            if (!IsServer)
                return;

            int version = reader.ReadInt32();
            string requestedScene = reader.ReadString();
            if (version != ProtocolVersion)
            {
                SendReliable(sourcePeerId, BuildPacket(PacketType.Disconnect, writer => writer.Write("Protocol mismatch")));
                return;
            }

            string scenePath = SceneManager.CurrentScenePath ?? requestedScene ?? string.Empty;
            lock (SyncRoot)
            {
                if (!Peers.TryGetValue(sourcePeerId, out PeerConnection? peer))
                    return;

                SendReliable(sourcePeerId, BuildPacket(PacketType.Welcome, writer =>
                {
                    writer.Write(sourcePeerId);
                    writer.Write(scenePath);
                    writer.Write(s_udpPort);
                    writer.Write(peer.UdpToken ?? string.Empty);
                }));
            }
        }

        private static void HandleWelcome(BinaryReader reader)
        {
            if (IsServer)
                return;

            s_localPeerId = reader.ReadUInt64();
            s_expectedScenePath = reader.ReadString();
            int udpPort = reader.ReadInt32();
            string udpToken = reader.ReadString();
            if (s_serverAddress != null)
            {
                s_serverUdpEndpoint = new IPEndPoint(s_serverAddress, udpPort);
            }

            lock (SyncRoot)
            {
                if (Peers.TryGetValue(1, out PeerConnection? serverPeer))
                {
                    serverPeer.IsReady = true;
                }
            }

            RaisePeerConnected(1);

            if (s_udpSocket != null && s_serverUdpEndpoint != null)
            {
                byte[] hello = BuildPacket(PacketType.UdpHello, writer =>
                {
                    writer.Write(s_localPeerId);
                    writer.Write(udpToken);
                });
                _ = s_udpSocket.SendAsync(hello, hello.Length, s_serverUdpEndpoint);
            }

            if (!string.IsNullOrWhiteSpace(s_expectedScenePath) &&
                !string.Equals(SceneManager.CurrentScenePath, s_expectedScenePath, StringComparison.OrdinalIgnoreCase))
            {
                SceneManager.LoadSync(s_expectedScenePath);
            }

            s_clientReadyPending = true;
            s_clientReadyCountdown = 2;
            ActivateRegisteredBehaviours();
        }

        private static void HandleClientReady(ulong sourcePeerId)
        {
            if (!IsServer)
                return;

            lock (SyncRoot)
            {
                if (!Peers.TryGetValue(sourcePeerId, out PeerConnection? peer))
                    return;

                peer.IsReady = true;

                foreach (ulong peerId in Peers.Keys.Where(id => id != sourcePeerId).ToArray())
                {
                    SendReliable(sourcePeerId, BuildPacket(PacketType.PeerJoined, writer => writer.Write(peerId)));
                    SendReliable(peerId, BuildPacket(PacketType.PeerJoined, writer => writer.Write(sourcePeerId)));
                }

                foreach (NetworkObjectState state in ObjectsByNetworkId.Values.ToArray())
                {
                    if (state.Dynamic)
                    {
                        SendReliable(sourcePeerId, BuildSpawnPacket(state));
                    }

                    SendReliable(sourcePeerId, BuildBaselinePacket(state, sourcePeerId));
                }
            }

            RaisePeerConnected(sourcePeerId);
        }

        private static void HandleUdpHello(IPEndPoint remoteEndPoint, BinaryReader reader)
        {
            if (!IsServer)
                return;

            ulong peerId = reader.ReadUInt64();
            string token = reader.ReadString();

            lock (SyncRoot)
            {
                if (Peers.TryGetValue(peerId, out PeerConnection? peer) &&
                    string.Equals(peer.UdpToken, token, StringComparison.Ordinal))
                {
                    peer.UdpEndpoint = remoteEndPoint;
                }
            }
        }

        private static bool TryResolveUdpPeer(IPEndPoint remoteEndPoint, out ulong peerId)
        {
            lock (SyncRoot)
            {
                foreach (var pair in Peers)
                {
                    if (pair.Value.UdpEndpoint != null &&
                        pair.Value.UdpEndpoint.Equals(remoteEndPoint))
                    {
                        peerId = pair.Key;
                        return true;
                    }
                }
            }

            peerId = 0;
            return false;
        }

        private static void HandleSpawn(BinaryReader reader)
        {
            ulong networkId = reader.ReadUInt64();
            string prefabGuid = reader.ReadString();
            ulong ownerPeerId = reader.ReadUInt64();
            Vector3 position = ReadVector3(reader);
            Quaternion rotation = ReadQuaternion(reader);
            Vector3 scale = ReadVector3(reader);

            if (TryInstantiateRemoteSpawn(networkId, prefabGuid, ownerPeerId, position, rotation, scale))
            {
                RemovePendingRemoteSpawn(networkId);
                return;
            }

            QueuePendingRemoteSpawn(networkId, prefabGuid, ownerPeerId, position, rotation, scale);
        }

        private static void HandleDespawn(ulong networkId)
        {
            lock (SyncRoot)
            {
                if (!ObjectsByNetworkId.TryGetValue(networkId, out NetworkObjectState? state))
                {
                    RemovePendingRemoteSpawn(networkId);
                    return;
                }

                foreach (var behaviour in state.Behaviours.ToArray())
                {
                    DeactivateBehaviour(behaviour);
                }

                ObjectsByEntityId.Remove(state.EntityId);
                ObjectsByNetworkId.Remove(networkId);
                if (state.EntityId > 0)
                {
                    EntityInterop.DestroyEntity(state.EntityId);
                }
            }

            RemovePendingRemoteSpawn(networkId);
        }

        private static void HandleOwnershipChanged(ulong networkId, ulong ownerPeerId)
        {
            lock (SyncRoot)
            {
                if (!ObjectsByNetworkId.TryGetValue(networkId, out NetworkObjectState? state))
                    return;

                ulong previousOwner = state.OwnerPeerId;
                state.OwnerPeerId = ownerPeerId;
                foreach (var behaviour in state.Behaviours)
                {
                    behaviour.InternalOwnerPeerId = ownerPeerId;
                    behaviour.OnOwnershipChanged(previousOwner, ownerPeerId);
                }
            }
        }

        private static void RetryPendingRemoteSpawns(float dt)
        {
            _ = dt;

            if (!IsClient || PendingRemoteSpawns.Count == 0)
                return;

            for (int i = PendingRemoteSpawns.Count - 1; i >= 0; i--)
            {
                PendingRemoteSpawn pending = PendingRemoteSpawns[i];
                if (TryInstantiateRemoteSpawn(pending.NetworkId,
                                              pending.PrefabGuid,
                                              pending.OwnerPeerId,
                                              pending.Position,
                                              pending.Rotation,
                                              pending.Scale))
                {
                    PendingRemoteSpawns.RemoveAt(i);
                    continue;
                }

                pending.AttemptCount++;
                if (pending.AttemptCount == 1 || pending.AttemptCount % 30 == 0)
                {
                    Console.WriteLine(
                        $"[Multiplayer] Waiting for prefab {pending.PrefabGuid} to resolve for network object {pending.NetworkId} (attempt {pending.AttemptCount}).");
                }

                if (pending.AttemptCount >= 300)
                {
                    Console.WriteLine(
                        $"[Multiplayer] Dropping remote spawn for network object {pending.NetworkId} after {pending.AttemptCount} attempts. Prefab GUID: {pending.PrefabGuid}");
                    PendingRemoteSpawns.RemoveAt(i);
                }
            }
        }

        private static bool TryInstantiateRemoteSpawn(ulong networkId,
                                                      string prefabGuid,
                                                      ulong ownerPeerId,
                                                      Vector3 position,
                                                      Quaternion rotation,
                                                      Vector3 scale)
        {
            lock (SyncRoot)
            {
                if (ObjectsByNetworkId.ContainsKey(networkId))
                    return true;
            }

            var prefab = new Prefab { Guid = prefabGuid };
            if (!prefab.IsValid)
            {
                Console.WriteLine(
                    $"[Multiplayer] Received spawn for network object {networkId} with invalid prefab GUID '{prefabGuid}'.");
                return false;
            }

            s_pendingSpawnContext = new PendingSpawnContext
            {
                NetworkId = networkId,
                OwnerPeerId = ownerPeerId,
                PrefabGuid = prefabGuid,
                Dynamic = true,
                Announce = false
            };

            try
            {
                Entity entity = prefab.Instantiate(position);
                if (!entity.IsValid)
                    return false;

                EntityInterop.SetRotationQuat(entity.EntityID, rotation);
                EntityInterop.SetScale(entity.EntityID, scale);
                return true;
            }
            finally
            {
                s_pendingSpawnContext = null;
            }
        }

        private static void QueuePendingRemoteSpawn(ulong networkId,
                                                    string prefabGuid,
                                                    ulong ownerPeerId,
                                                    Vector3 position,
                                                    Quaternion rotation,
                                                    Vector3 scale)
        {
            PendingRemoteSpawn? existing = PendingRemoteSpawns.FirstOrDefault(pending => pending.NetworkId == networkId);
            if (existing != null)
            {
                existing.OwnerPeerId = ownerPeerId;
                existing.PrefabGuid = prefabGuid;
                existing.Position = position;
                existing.Rotation = rotation;
                existing.Scale = scale;
                return;
            }

            PendingRemoteSpawns.Add(new PendingRemoteSpawn
            {
                NetworkId = networkId,
                OwnerPeerId = ownerPeerId,
                PrefabGuid = prefabGuid,
                Position = position,
                Rotation = rotation,
                Scale = scale,
                AttemptCount = 0
            });

            Console.WriteLine(
                $"[Multiplayer] Deferred remote spawn for network object {networkId} until prefab {prefabGuid} is available locally.");
        }

        private static void RemovePendingRemoteSpawn(ulong networkId)
        {
            for (int i = PendingRemoteSpawns.Count - 1; i >= 0; i--)
            {
                if (PendingRemoteSpawns[i].NetworkId == networkId)
                {
                    PendingRemoteSpawns.RemoveAt(i);
                }
            }
        }

        private static void HandleRpc(ulong sourcePeerId, BinaryReader reader)
        {
            ulong networkId = reader.ReadUInt64();
            ulong behaviourTypeHash = reader.ReadUInt64();
            ulong rpcId = reader.ReadUInt64();
            int argCount = reader.ReadInt32();
            object?[] args = new object?[argCount];
            for (int i = 0; i < argCount; i++)
            {
                args[i] = ReadValue(reader);
            }

            lock (SyncRoot)
            {
                if (!ObjectsByNetworkId.TryGetValue(networkId, out NetworkObjectState? state))
                    return;

                if (!TryGetBehaviourForType(state, behaviourTypeHash, out NetworkBehaviour? behaviour, out BehaviourMetadata? metadata))
                    return;

                if (!metadata!.RpcById.TryGetValue(rpcId, out RpcDescriptor? descriptor))
                    return;

                if (IsServer && sourcePeerId != 1)
                {
                    if (descriptor.Attribute.Targets != RpcTargets.Server)
                        return;

                    if (state.OwnerPeerId != sourcePeerId)
                        return;
                }

                descriptor.Method.Invoke(behaviour, CoerceRpcArguments(descriptor, args));
            }
        }

        private static void HandleVariableDelta(ulong sourcePeerId, BinaryReader reader)
        {
            ulong networkId = reader.ReadUInt64();
            ulong typeHash = reader.ReadUInt64();
            string fieldName = reader.ReadString();
            object? value = ReadValue(reader);

            lock (SyncRoot)
            {
                if (!ObjectsByNetworkId.TryGetValue(networkId, out NetworkObjectState? state))
                    return;

                if (!TryGetBehaviourForType(state, typeHash, out NetworkBehaviour? behaviour, out BehaviourMetadata? metadata))
                    return;

                if (!metadata!.VariablesByName.TryGetValue(fieldName, out NetworkVariableDescriptor? descriptor))
                    return;

                if (descriptor.Field.GetValue(behaviour) is not INetworkVariable variable)
                    return;

                if (IsServer && sourcePeerId != 1 && !CanRemoteWriteVariable(sourcePeerId, state, variable))
                    return;

                variable.ApplyRemote(value);
                variable.ClearDirty();

                if (IsServer && sourcePeerId != 1)
                {
                    byte[] packet = BuildPacket(PacketType.VariableDelta, writer =>
                    {
                        writer.Write(networkId);
                        writer.Write(typeHash);
                        writer.Write(fieldName);
                        WriteValue(writer, descriptor.ValueType, value);
                    });

                    foreach (ulong peerId in Peers.Keys.ToArray())
                    {
                        if (peerId == sourcePeerId)
                            continue;

                        if (!Peers.TryGetValue(peerId, out PeerConnection? peer) || !peer.IsReady)
                            continue;

                        if (!CanPeerReadVariable(peerId, state, variable))
                            continue;

                        SendReliable(peerId, packet);
                    }
                }
            }
        }

        private static void HandleStateBaseline(BinaryReader reader)
        {
            ulong networkId = reader.ReadUInt64();
            ulong ownerPeerId = reader.ReadUInt64();
            bool dynamic = reader.ReadBoolean();
            string scenePath = reader.ReadString();
            string entityGuid = reader.ReadString();
            bool replicateTransform = reader.ReadBoolean();
            bool replicateScale = reader.ReadBoolean();
            bool interpolate = reader.ReadBoolean();
            float interpolationSpeed = reader.ReadSingle();
            Vector3 position = ReadVector3(reader);
            Quaternion rotation = ReadQuaternion(reader);
            Vector3 scale = ReadVector3(reader);
            int variableCount = reader.ReadInt32();

            lock (SyncRoot)
            {
                if (!ObjectsByNetworkId.TryGetValue(networkId, out NetworkObjectState? state))
                {
                    if (!dynamic && !string.IsNullOrWhiteSpace(entityGuid))
                    {
                        int entityId = NetworkingInterop.FindEntityByGuid(entityGuid);
                        if (entityId > 0 && ObjectsByEntityId.TryGetValue(entityId, out NetworkObjectState? resolved))
                        {
                            state = resolved;
                        }
                    }

                    if (state == null)
                    {
                        for (int i = 0; i < variableCount; i++)
                        {
                            _ = reader.ReadUInt64();
                            _ = reader.ReadString();
                            _ = ReadValue(reader);
                        }
                        return;
                    }

                    RebindNetworkId(state, networkId);
                }

                state.OwnerPeerId = ownerPeerId;
                state.Dynamic = dynamic;
                state.ScenePath = scenePath;
                state.EntityGuid = entityGuid;
                state.ReplicateTransform = replicateTransform;
                state.ReplicateScale = replicateScale;
                state.InterpolateRemoteTransform = interpolate;
                state.InterpolationSpeed = interpolationSpeed;
                ApplyBehaviourConfiguration(state);
                ApplyRemoteTransform(state, position, rotation, scale, preferInterpolation: false);

                for (int i = 0; i < variableCount; i++)
                {
                    ulong typeHash = reader.ReadUInt64();
                    string fieldName = reader.ReadString();
                    object? value = ReadValue(reader);

                    if (!TryGetBehaviourForType(state, typeHash, out NetworkBehaviour? behaviour, out BehaviourMetadata? metadata))
                        continue;

                    if (!metadata!.VariablesByName.TryGetValue(fieldName, out NetworkVariableDescriptor? descriptor))
                        continue;

                    if (descriptor.Field.GetValue(behaviour) is INetworkVariable variable)
                    {
                        variable.ApplyRemote(value);
                        variable.ClearDirty();
                    }
                }

                foreach (var behaviour in state.Behaviours)
                {
                    behaviour.InternalOwnerPeerId = ownerPeerId;
                    ActivateBehaviourIfNeeded(behaviour);
                }
            }
        }

        private static void HandleSnapshot(ulong sourcePeerId, BinaryReader reader)
        {
            int count = reader.ReadInt32();
            for (int i = 0; i < count; i++)
            {
                ulong networkId = reader.ReadUInt64();
                Vector3 position = ReadVector3(reader);
                Quaternion rotation = ReadQuaternion(reader);
                Vector3 scale = ReadVector3(reader);

                lock (SyncRoot)
                {
                    if (!ObjectsByNetworkId.TryGetValue(networkId, out NetworkObjectState? state))
                        continue;

                    if (IsServer && sourcePeerId != 1)
                    {
                        NetworkBehaviour? primary = state.PrimaryBehaviour;
                        if (primary == null ||
                            !state.ReplicateTransform ||
                            !primary.AllowOwnerSimulation ||
                            state.OwnerPeerId != sourcePeerId)
                        {
                            continue;
                        }

                        ApplyOwnerSimulatedTransformOnServer(state, position, rotation, scale);
                        continue;
                    }

                    ApplyRemoteTransform(state, position, rotation, scale, preferInterpolation: true);
                }
            }
        }

        private static void HandlePeerDisconnected(ulong peerId)
        {
            bool shouldShutdownClientSession = false;
            lock (SyncRoot)
            {
                if (Peers.TryGetValue(peerId, out PeerConnection? peer))
                {
                    peer.Dispose();
                    Peers.Remove(peerId);
                }

                if (IsServer)
                {
                    BroadcastReliable(BuildPacket(PacketType.PeerLeft, writer => writer.Write(peerId)), exceptPeerId: peerId);
                }
                else if (peerId == 1)
                {
                    shouldShutdownClientSession = true;
                }
            }

            RaisePeerDisconnected(peerId);

            if (shouldShutdownClientSession)
            {
                ReportError("Disconnected from server.");
                Shutdown();
            }
        }
        private static void FlushPendingSpawns()
        {
            if (!IsServer)
                return;

            foreach (NetworkObjectState state in ObjectsByNetworkId.Values.Where(static s => s.Dynamic && s.SpawnAnnouncementPending).ToArray())
            {
                foreach (ulong peerId in Peers.Keys.ToArray())
                {
                    if (!Peers.TryGetValue(peerId, out PeerConnection? peer) || !peer.IsReady)
                        continue;

                    SendReliable(peerId, BuildSpawnPacket(state));
                    SendReliable(peerId, BuildBaselinePacket(state, peerId));
                }

                state.SpawnAnnouncementPending = false;
            }
        }

        private static void FlushDirtyNetworkVariables()
        {
            foreach (NetworkObjectState state in ObjectsByNetworkId.Values.ToArray())
            {
                foreach (NetworkBehaviour behaviour in state.Behaviours.ToArray())
                {
                    BehaviourMetadata metadata = GetMetadata(behaviour.GetType());
                    foreach (NetworkVariableDescriptor descriptor in metadata.Variables)
                    {
                        if (descriptor.Field.GetValue(behaviour) is not INetworkVariable variable || !variable.Dirty)
                            continue;

                        byte[] packet = BuildPacket(PacketType.VariableDelta, writer =>
                        {
                            writer.Write(state.NetworkId);
                            writer.Write(metadata.TypeHash);
                            writer.Write(descriptor.Field.Name);
                            WriteValue(writer, descriptor.ValueType, variable.BoxedValue);
                        });

                        if (IsServer)
                        {
                            foreach (ulong peerId in Peers.Keys.ToArray())
                            {
                                if (!Peers.TryGetValue(peerId, out PeerConnection? peer) || !peer.IsReady)
                                    continue;

                                if (!CanPeerReadVariable(peerId, state, variable))
                                    continue;

                                SendReliable(peerId, packet);
                            }
                        }
                        else if (HasInputAuthority(behaviour))
                        {
                            SendReliable(1, packet);
                        }

                        variable.ClearDirty();
                    }
                }
            }
        }

        private static void SendSnapshots()
        {
            if (IsServer)
            {
                foreach (ulong peerId in Peers.Keys.ToArray())
                {
                    if (!Peers.TryGetValue(peerId, out PeerConnection? peer) || !peer.IsReady)
                        continue;

                    List<NetworkObjectState> states = new();
                    foreach (NetworkObjectState state in ObjectsByNetworkId.Values.ToArray())
                    {
                        if (!ShouldSendTransformToPeer(state, peerId))
                            continue;

                        if (TryCaptureTransform(state, out Vector3 position, out Quaternion rotation, out Vector3 scale))
                        {
                            state.LastSentPosition = position;
                            state.LastSentRotation = rotation;
                            state.LastSentScale = scale;
                            state.HasLastSentTransform = true;
                            states.Add(state);
                        }
                    }

                    if (states.Count == 0)
                        continue;

                    byte[] packet = BuildPacket(PacketType.Snapshot, writer =>
                    {
                        writer.Write(states.Count);
                        foreach (NetworkObjectState state in states)
                        {
                            writer.Write(state.NetworkId);
                            WriteVector3(writer, state.LastSentPosition);
                            WriteQuaternion(writer, state.LastSentRotation);
                            WriteVector3(writer, state.LastSentScale);
                        }
                    });

                    SendDatagram(peerId, packet);
                }
            }
            else if (IsClient)
            {
                List<NetworkObjectState> ownedStates = new();
                foreach (NetworkObjectState state in ObjectsByNetworkId.Values.ToArray())
                {
                    NetworkBehaviour? primary = state.PrimaryBehaviour;
                    if (primary == null ||
                        !state.ReplicateTransform ||
                        !primary.AllowOwnerSimulation ||
                        !HasInputAuthority(primary))
                    {
                        continue;
                    }

                    if (TryCaptureTransform(state, out Vector3 position, out Quaternion rotation, out Vector3 scale))
                    {
                        state.LastSentPosition = position;
                        state.LastSentRotation = rotation;
                        state.LastSentScale = scale;
                        state.HasLastSentTransform = true;
                        ownedStates.Add(state);
                    }
                }

                if (ownedStates.Count > 0)
                {
                    byte[] packet = BuildPacket(PacketType.Snapshot, writer =>
                    {
                        writer.Write(ownedStates.Count);
                        foreach (NetworkObjectState state in ownedStates)
                        {
                            writer.Write(state.NetworkId);
                            WriteVector3(writer, state.LastSentPosition);
                            WriteQuaternion(writer, state.LastSentRotation);
                            WriteVector3(writer, state.LastSentScale);
                        }
                    });

                    // Client-owned gameplay snapshots need to survive mixed LAN/firewall setups
                    // where TCP succeeds but client->server UDP silently drops.
                    SendReliable(1, packet);
                }
            }
        }

        private static bool ShouldSendTransformToPeer(NetworkObjectState state, ulong peerId)
        {
            if (!state.ReplicateTransform || state.EntityId <= 0)
                return false;

            NetworkBehaviour? primary = state.PrimaryBehaviour;
            if (primary == null)
                return false;

            if (primary.AllowOwnerSimulation && state.OwnerPeerId == peerId)
                return false;

            return true;
        }

        private static bool TryCaptureTransform(NetworkObjectState state, out Vector3 position, out Quaternion rotation, out Vector3 scale)
        {
            if (state.EntityId <= 0)
            {
                position = Vector3.Zero;
                rotation = Quaternion.Identity;
                scale = Vector3.One;
                return false;
            }

            position = EntityInterop.GetPosition(state.EntityId);
            rotation = EntityInterop.GetRotationQuat(state.EntityId);
            scale = EntityInterop.GetScale(state.EntityId);

            NetworkBehaviour? primary = state.PrimaryBehaviour;
            if (primary == null)
                return false;

            if (!state.HasLastSentTransform)
                return true;

            bool positionChanged = Vector3.DistanceSquared(position, state.LastSentPosition) >= primary.PositionThreshold * primary.PositionThreshold;
            bool rotationChanged = QuaternionAngleDegrees(rotation, state.LastSentRotation) >= primary.RotationThresholdDegrees;
            bool scaleChanged = state.ReplicateScale &&
                                Vector3.DistanceSquared(scale, state.LastSentScale) >= primary.ScaleThreshold * primary.ScaleThreshold;

            return positionChanged || rotationChanged || scaleChanged;
        }

        private static void ApplyInterpolatedTransforms(float dt)
        {
            foreach (NetworkObjectState state in ObjectsByNetworkId.Values.ToArray())
            {
                if (!state.HasTargetTransform || state.EntityId <= 0)
                    continue;

                NetworkBehaviour? primary = state.PrimaryBehaviour;
                if (primary == null || HasStateAuthority(primary))
                    continue;

                // Owner-simulated local objects should follow local input, not drift
                // back toward the last server-authored target every frame.
                if (primary.AllowOwnerSimulation && HasInputAuthority(primary))
                    continue;

                if (!state.InterpolateRemoteTransform)
                {
                    ApplyTransformImmediately(state, state.TargetPosition, state.TargetRotation, state.TargetScale);
                    continue;
                }

                float t = Math.Clamp(dt * state.InterpolationSpeed, 0.0f, 1.0f);
                Vector3 currentPosition = EntityInterop.GetPosition(state.EntityId);
                Quaternion currentRotation = EntityInterop.GetRotationQuat(state.EntityId);
                Vector3 currentScale = EntityInterop.GetScale(state.EntityId);

                Vector3 nextPosition = Vector3.Lerp(currentPosition, state.TargetPosition, t);
                Quaternion nextRotation = Quaternion.Slerp(currentRotation, state.TargetRotation, t);
                Vector3 nextScale = state.ReplicateScale ? Vector3.Lerp(currentScale, state.TargetScale, t) : currentScale;

                ApplyTransformImmediately(state, nextPosition, nextRotation, nextScale);
            }
        }

        private static void ApplyRemoteTransform(NetworkObjectState state,
                                                 Vector3 position,
                                                 Quaternion rotation,
                                                 Vector3 scale,
                                                 bool preferInterpolation)
        {
            NetworkBehaviour? primary = state.PrimaryBehaviour;
            if (primary == null || state.EntityId <= 0)
                return;

            if (HasStateAuthority(primary))
                return;

            // The owning client predicts and drives this transform locally.
            if (primary.AllowOwnerSimulation && HasInputAuthority(primary))
            {
                state.HasTargetTransform = false;
                return;
            }

            state.TargetPosition = position;
            state.TargetRotation = rotation;
            state.TargetScale = scale;
            state.HasTargetTransform = true;

            if (!preferInterpolation || !state.InterpolateRemoteTransform)
            {
                ApplyTransformImmediately(state, position, rotation, scale);
            }
        }

        private static void ApplyTransformImmediately(NetworkObjectState state, Vector3 position, Quaternion rotation, Vector3 scale)
        {
            EntityInterop.SetPosition(state.EntityId, position);
            EntityInterop.SetRotationQuat(state.EntityId, rotation);
            if (state.ReplicateScale)
            {
                EntityInterop.SetScale(state.EntityId, scale);
            }
        }

        private static void ApplyOwnerSimulatedTransformOnServer(NetworkObjectState state,
                                                                 Vector3 position,
                                                                 Quaternion rotation,
                                                                 Vector3 scale)
        {
            if (state.EntityId <= 0)
                return;

            state.TargetPosition = position;
            state.TargetRotation = rotation;
            state.TargetScale = scale;
            state.HasTargetTransform = true;
            ApplyTransformImmediately(state, position, rotation, scale);
        }

        private static void ActivateRegisteredBehaviours()
        {
            lock (SyncRoot)
            {
                foreach (NetworkObjectState state in ObjectsByNetworkId.Values.ToArray())
                {
                    foreach (NetworkBehaviour behaviour in state.Behaviours.ToArray())
                    {
                        ApplyBehaviourState(behaviour, state);
                        ActivateBehaviourIfNeeded(behaviour);
                    }
                }
            }
        }

        private static void ApplyBehaviourState(NetworkBehaviour behaviour, NetworkObjectState state)
        {
            behaviour.InternalNetworkId = state.NetworkId;
            behaviour.InternalOwnerPeerId = state.OwnerPeerId;
            behaviour.InternalDynamicSpawn = state.Dynamic;
        }

        private static void ActivateBehaviourIfNeeded(NetworkBehaviour behaviour)
        {
            if (!IsSessionActive || behaviour.InternalSpawnCallbackInvoked)
                return;

            behaviour.InternalSpawned = true;
            behaviour.InternalSpawnCallbackInvoked = true;
            behaviour.OnNetworkSpawn();
        }

        private static void DeactivateBehaviour(NetworkBehaviour behaviour)
        {
            if (!behaviour.InternalSpawnCallbackInvoked)
                return;

            behaviour.InternalSpawned = false;
            behaviour.InternalSpawnCallbackInvoked = false;
            behaviour.OnNetworkDespawn();
        }

        private static void ApplyBehaviourConfiguration(NetworkObjectState state)
        {
            NetworkBehaviour? primary = state.PrimaryBehaviour;
            state.ReplicateTransform = state.Behaviours.Any(static b => b.SynchronizeTransform);
            state.ReplicateScale = primary?.SynchronizeScale ?? false;
            state.InterpolateRemoteTransform = primary?.InterpolateRemoteTransform ?? true;
            state.InterpolationSpeed = primary?.InterpolationSpeed ?? 15.0f;
        }

        private static bool TryGetBehaviourForType(NetworkObjectState state,
                                                   ulong typeHash,
                                                   out NetworkBehaviour? behaviour,
                                                   out BehaviourMetadata? metadata)
        {
            foreach (NetworkBehaviour candidate in state.Behaviours)
            {
                BehaviourMetadata candidateMetadata = GetMetadata(candidate.GetType());
                if (candidateMetadata.TypeHash == typeHash)
                {
                    behaviour = candidate;
                    metadata = candidateMetadata;
                    return true;
                }
            }

            behaviour = null;
            metadata = null;
            return false;
        }

        private static void RebindNetworkId(NetworkObjectState state, ulong networkId)
        {
            if (state.NetworkId == networkId)
                return;

            ulong previousNetworkId = state.NetworkId;
            if (previousNetworkId != 0 &&
                ObjectsByNetworkId.TryGetValue(previousNetworkId, out NetworkObjectState? existing) &&
                ReferenceEquals(existing, state))
            {
                ObjectsByNetworkId.Remove(previousNetworkId);
            }

            state.NetworkId = networkId;
            ObjectsByNetworkId[networkId] = state;

            foreach (NetworkBehaviour behaviour in state.Behaviours)
            {
                behaviour.InternalNetworkId = networkId;
            }
        }

        private static bool CanRemoteWriteVariable(ulong sourcePeerId, NetworkObjectState state, INetworkVariable variable)
        {
            if (sourcePeerId == 1)
                return true;

            if (variable.WritePermission == NetworkVariableWritePermission.Server)
                return false;

            return state.OwnerPeerId == sourcePeerId;
        }

        private static bool CanPeerReadVariable(ulong peerId, NetworkObjectState state, INetworkVariable variable)
        {
            if (peerId == 1)
                return true;

            if (variable.ReadPermission == NetworkVariableReadPermission.Everyone)
                return true;

            return state.OwnerPeerId == peerId;
        }

        private static void InvokeRpcLocal(ulong networkId, ulong typeHash, ulong rpcId, object?[] args)
        {
            lock (SyncRoot)
            {
                if (!ObjectsByNetworkId.TryGetValue(networkId, out NetworkObjectState? state))
                    return;

                if (!TryGetBehaviourForType(state, typeHash, out NetworkBehaviour? behaviour, out BehaviourMetadata? metadata))
                    return;

                if (!metadata!.RpcById.TryGetValue(rpcId, out RpcDescriptor? descriptor))
                    return;

                descriptor.Method.Invoke(behaviour, CoerceRpcArguments(descriptor, args));
            }
        }

        private static object?[] CoerceRpcArguments(RpcDescriptor descriptor, object?[] args)
        {
            if (descriptor.Parameters.Length == 0 || args.Length == 0)
                return args;

            object?[] coerced = new object?[descriptor.Parameters.Length];
            for (int i = 0; i < descriptor.Parameters.Length; i++)
            {
                object? value = i < args.Length ? args[i] : null;
                coerced[i] = CoerceValue(value, descriptor.Parameters[i].ParameterType);
            }

            return coerced;
        }

        private static object? CoerceValue(object? value, Type targetType)
        {
            if (value == null)
                return targetType.IsValueType ? Activator.CreateInstance(targetType) : null;

            Type effectiveType = Nullable.GetUnderlyingType(targetType) ?? targetType;
            if (effectiveType.IsInstanceOfType(value))
                return value;

            if (effectiveType.IsEnum)
            {
                if (value is string enumName)
                    return Enum.Parse(effectiveType, enumName, ignoreCase: true);

                return Enum.ToObject(effectiveType, Convert.ToInt32(value));
            }

            return Convert.ChangeType(value, effectiveType);
        }

        private static void AttachNetworkVariables(NetworkBehaviour behaviour, BehaviourMetadata metadata)
        {
            foreach (NetworkVariableDescriptor descriptor in metadata.Variables)
            {
                object? instance = descriptor.Field.GetValue(behaviour);
                if (instance == null)
                {
                    instance = Activator.CreateInstance(descriptor.Field.FieldType);
                    descriptor.Field.SetValue(behaviour, instance);
                }

                if (instance is INetworkVariable variable)
                {
                    variable.Attach(behaviour, descriptor.Field.Name);
                }
            }
        }

        private static BehaviourMetadata GetMetadata(Type type)
        {
            if (MetadataCache.TryGetValue(type, out BehaviourMetadata? cached))
                return cached;

            var metadata = new BehaviourMetadata
            {
                TypeKey = type.FullName ?? type.Name,
                TypeHash = Hash64(type.FullName ?? type.Name)
            };

            foreach (MethodInfo method in type.GetMethods(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic))
            {
                RpcAttribute? attribute = method.GetCustomAttributes(true).OfType<RpcAttribute>().FirstOrDefault();
                if (attribute == null)
                    continue;

                var descriptor = new RpcDescriptor
                {
                    Attribute = attribute,
                    Method = method,
                    MethodId = Hash64($"{metadata.TypeKey}::{method.Name}"),
                    Parameters = method.GetParameters()
                };

                metadata.RpcByName[method.Name] = descriptor;
                metadata.RpcById[descriptor.MethodId] = descriptor;
            }

            foreach (FieldInfo field in type.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic))
            {
                if (!field.FieldType.IsGenericType ||
                    field.FieldType.GetGenericTypeDefinition() != typeof(NetworkVariable<>))
                {
                    continue;
                }

                var variableDescriptor = new NetworkVariableDescriptor
                {
                    Field = field,
                    ValueType = field.FieldType.GetGenericArguments()[0]
                };

                metadata.Variables.Add(variableDescriptor);
                metadata.VariablesByName[field.Name] = variableDescriptor;
            }

            MetadataCache[type] = metadata;
            return metadata;
        }

        private static byte[] BuildSpawnPacket(NetworkObjectState state)
        {
            TryCaptureTransform(state, out Vector3 position, out Quaternion rotation, out Vector3 scale);
            return BuildPacket(PacketType.Spawn, writer =>
            {
                writer.Write(state.NetworkId);
                writer.Write(state.PrefabGuid ?? string.Empty);
                writer.Write(state.OwnerPeerId);
                WriteVector3(writer, position);
                WriteQuaternion(writer, rotation);
                WriteVector3(writer, scale);
            });
        }

        private static byte[] BuildBaselinePacket(NetworkObjectState state, ulong targetPeerId)
        {
            TryCaptureTransform(state, out Vector3 position, out Quaternion rotation, out Vector3 scale);
            return BuildPacket(PacketType.StateBaseline, writer =>
            {
                writer.Write(state.NetworkId);
                writer.Write(state.OwnerPeerId);
                writer.Write(state.Dynamic);
                writer.Write(state.ScenePath ?? string.Empty);
                writer.Write(state.EntityGuid ?? string.Empty);
                writer.Write(state.ReplicateTransform);
                writer.Write(state.ReplicateScale);
                writer.Write(state.InterpolateRemoteTransform);
                writer.Write(state.InterpolationSpeed);
                WriteVector3(writer, position);
                WriteQuaternion(writer, rotation);
                WriteVector3(writer, scale);

                var variables = new List<(ulong TypeHash, string FieldName, Type ValueType, object? Value)>();
                foreach (NetworkBehaviour behaviour in state.Behaviours)
                {
                    BehaviourMetadata metadata = GetMetadata(behaviour.GetType());
                    foreach (NetworkVariableDescriptor descriptor in metadata.Variables)
                    {
                        if (descriptor.Field.GetValue(behaviour) is INetworkVariable variable &&
                            CanPeerReadVariable(targetPeerId, state, variable))
                        {
                            variables.Add((metadata.TypeHash, descriptor.Field.Name, descriptor.ValueType, variable.BoxedValue));
                        }
                    }
                }

                writer.Write(variables.Count);
                foreach (var variable in variables)
                {
                    writer.Write(variable.TypeHash);
                    writer.Write(variable.FieldName);
                    WriteValue(writer, variable.ValueType, variable.Value);
                }
            });
        }

        private static byte[] BuildPacket(PacketType packetType, Action<BinaryWriter> writePayload)
        {
            using var stream = new MemoryStream();
            using var writer = new BinaryWriter(stream, Encoding.UTF8, leaveOpen: true);
            writer.Write((byte)packetType);
            writePayload(writer);
            writer.Flush();
            return stream.ToArray();
        }

        private static void SendReliable(ulong peerId, byte[] payload)
        {
            if (peerId == 0)
                return;

            if (peerId == s_localPeerId && IsHost)
            {
                MainThreadActions.Enqueue(() => ProcessReliablePacket(peerId, payload));
                return;
            }

            lock (SyncRoot)
            {
                if (Peers.TryGetValue(peerId, out PeerConnection? peer))
                {
                    peer.Outgoing.Writer.TryWrite(payload);
                }
            }
        }

        private static void BroadcastReliable(byte[] payload, ulong exceptPeerId = 0)
        {
            PeerConnection[] targets;
            lock (SyncRoot)
            {
                targets = Peers
                    .Where(pair => pair.Key != exceptPeerId && pair.Value.IsReady)
                    .Select(pair => pair.Value)
                    .ToArray();
            }

            foreach (PeerConnection peer in targets)
            {
                peer.Outgoing.Writer.TryWrite(payload);
            }
        }

        private static void SendDatagram(ulong peerId, byte[] payload)
        {
            if (peerId == s_localPeerId && IsHost)
            {
                MainThreadActions.Enqueue(() => ProcessReliablePacket(peerId, payload));
                return;
            }

            if (s_udpSocket == null)
            {
                SendReliable(peerId, payload);
                return;
            }

            if (IsServer)
            {
                PeerConnection? peer;
                lock (SyncRoot)
                {
                    Peers.TryGetValue(peerId, out peer);
                }

                if (peer == null || peer.UdpEndpoint == null)
                {
                    SendReliable(peerId, payload);
                    return;
                }

                _ = s_udpSocket.SendAsync(payload, payload.Length, peer.UdpEndpoint);
            }
            else if (s_serverUdpEndpoint != null)
            {
                _ = s_udpSocket.SendAsync(payload, payload.Length, s_serverUdpEndpoint);
            }
            else
            {
                SendReliable(peerId, payload);
            }
        }

        private static void RaisePeerConnected(ulong peerId) => PeerConnected?.Invoke(peerId);
        private static void RaisePeerDisconnected(ulong peerId) => PeerDisconnected?.Invoke(peerId);

        private static void ReportError(string message)
        {
            Console.WriteLine($"[Multiplayer] {message}");
            ConnectionError?.Invoke(message);
        }

        private static ulong AllocateDynamicNetworkId()
        {
            ulong value = s_nextDynamicNetworkId++;
            return 0x8000_0000_0000_0000UL | value;
        }

        private static ulong ComputeSceneObjectId(string? scenePath, string entityGuid)
        {
            return Hash64($"{scenePath ?? string.Empty}|{entityGuid}");
        }

        private static ulong Hash64(string value)
        {
            const ulong offset = 14695981039346656037UL;
            const ulong prime = 1099511628211UL;
            ulong hash = offset;
            foreach (byte b in Encoding.UTF8.GetBytes(value))
            {
                hash ^= b;
                hash *= prime;
            }
            return hash;
        }

        private static void WriteValue(BinaryWriter writer, Type? valueType, object? value)
        {
            if (value == null || valueType == null)
            {
                writer.Write((byte)WireValueType.Null);
                return;
            }

            if (valueType.IsEnum)
            {
                writer.Write((byte)WireValueType.Int32);
                writer.Write(Convert.ToInt32(value));
                return;
            }

            if (valueType == typeof(bool))
            {
                writer.Write((byte)WireValueType.Bool);
                writer.Write((bool)value);
                return;
            }

            if (valueType == typeof(int))
            {
                writer.Write((byte)WireValueType.Int32);
                writer.Write((int)value);
                return;
            }

            if (valueType == typeof(ulong))
            {
                writer.Write((byte)WireValueType.UInt64);
                writer.Write((ulong)value);
                return;
            }

            if (valueType == typeof(float))
            {
                writer.Write((byte)WireValueType.Single);
                writer.Write((float)value);
                return;
            }

            if (valueType == typeof(string))
            {
                writer.Write((byte)WireValueType.String);
                writer.Write((string)value);
                return;
            }

            if (valueType == typeof(Vector2))
            {
                writer.Write((byte)WireValueType.Vector2);
                var vector = (Vector2)value;
                writer.Write(vector.X);
                writer.Write(vector.Y);
                return;
            }

            if (valueType == typeof(Vector3))
            {
                writer.Write((byte)WireValueType.Vector3);
                WriteVector3(writer, (Vector3)value);
                return;
            }

            if (valueType == typeof(Quaternion))
            {
                writer.Write((byte)WireValueType.Quaternion);
                WriteQuaternion(writer, (Quaternion)value);
                return;
            }

            throw new NotSupportedException($"Unsupported network value type: {valueType.FullName}");
        }

        private static object? ReadValue(BinaryReader reader)
        {
            WireValueType type = (WireValueType)reader.ReadByte();
            return type switch
            {
                WireValueType.Null => null,
                WireValueType.Bool => reader.ReadBoolean(),
                WireValueType.Int32 => reader.ReadInt32(),
                WireValueType.UInt64 => reader.ReadUInt64(),
                WireValueType.Single => reader.ReadSingle(),
                WireValueType.String => reader.ReadString(),
                WireValueType.Vector2 => new Vector2(reader.ReadSingle(), reader.ReadSingle()),
                WireValueType.Vector3 => ReadVector3(reader),
                WireValueType.Quaternion => ReadQuaternion(reader),
                _ => null
            };
        }

        private static void WriteVector3(BinaryWriter writer, Vector3 value)
        {
            writer.Write(value.X);
            writer.Write(value.Y);
            writer.Write(value.Z);
        }

        private static Vector3 ReadVector3(BinaryReader reader)
        {
            return new Vector3(reader.ReadSingle(), reader.ReadSingle(), reader.ReadSingle());
        }

        private static void WriteQuaternion(BinaryWriter writer, Quaternion value)
        {
            writer.Write(value.X);
            writer.Write(value.Y);
            writer.Write(value.Z);
            writer.Write(value.W);
        }

        private static Quaternion ReadQuaternion(BinaryReader reader)
        {
            return Quaternion.Normalize(new Quaternion(reader.ReadSingle(), reader.ReadSingle(), reader.ReadSingle(), reader.ReadSingle()));
        }

        private static float QuaternionAngleDegrees(Quaternion a, Quaternion b)
        {
            float dot = Math.Abs(Quaternion.Dot(Quaternion.Normalize(a), Quaternion.Normalize(b)));
            dot = Math.Clamp(dot, 0.0f, 1.0f);
            return 2.0f * (float)(Math.Acos(dot) * (180.0 / Math.PI));
        }
    }
}
