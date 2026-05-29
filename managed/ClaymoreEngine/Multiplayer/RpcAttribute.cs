using System;

namespace ClaymoreEngine.Multiplayer
{
    public enum RpcTargets
    {
        Server = 0,
        Owner = 1,
        Observers = 2,
        Everyone = 3
    }

    [AttributeUsage(AttributeTargets.Method, Inherited = true)]
    public class RpcAttribute : Attribute
    {
        public RpcTargets Targets { get; }
        public bool Reliable { get; }

        public RpcAttribute(RpcTargets targets, bool reliable = true)
        {
            Targets = targets;
            Reliable = reliable;
        }
    }

    [AttributeUsage(AttributeTargets.Method, Inherited = true)]
    public sealed class ServerRpcAttribute : RpcAttribute
    {
        public ServerRpcAttribute(bool reliable = true) : base(RpcTargets.Server, reliable) { }
    }

    [AttributeUsage(AttributeTargets.Method, Inherited = true)]
    public sealed class OwnerRpcAttribute : RpcAttribute
    {
        public OwnerRpcAttribute(bool reliable = true) : base(RpcTargets.Owner, reliable) { }
    }

    [AttributeUsage(AttributeTargets.Method, Inherited = true)]
    public sealed class ObserversRpcAttribute : RpcAttribute
    {
        public ObserversRpcAttribute(bool reliable = true) : base(RpcTargets.Observers, reliable) { }
    }

    [AttributeUsage(AttributeTargets.Method, Inherited = true)]
    public sealed class EveryoneRpcAttribute : RpcAttribute
    {
        public EveryoneRpcAttribute(bool reliable = true) : base(RpcTargets.Everyone, reliable) { }
    }
}
