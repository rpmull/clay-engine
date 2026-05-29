using System;
using System.Collections.Generic;

namespace ClaymoreEngine.Multiplayer
{
    public enum NetworkVariableReadPermission
    {
        Everyone = 0,
        Owner = 1
    }

    public enum NetworkVariableWritePermission
    {
        Server = 0,
        Owner = 1
    }

    internal interface INetworkVariable
    {
        Type ValueType { get; }
        object? BoxedValue { get; }
        string FieldName { get; }
        bool Dirty { get; }
        NetworkVariableReadPermission ReadPermission { get; }
        NetworkVariableWritePermission WritePermission { get; }
        void Attach(NetworkBehaviour behaviour, string fieldName);
        void ClearDirty();
        void ApplyRemote(object? value);
    }

    public sealed class NetworkVariable<T> : INetworkVariable
    {
        private T _value;

        internal NetworkBehaviour? OwnerBehaviour;
        internal string InternalFieldName = string.Empty;

        public event Action<T, T>? OnValueChanged;

        public NetworkVariableReadPermission ReadPermission { get; }
        public NetworkVariableWritePermission WritePermission { get; }
        public bool Dirty { get; private set; }

        public T Value
        {
            get => _value;
            set => SetValue(value, markDirty: true);
        }

        public NetworkVariable(T initialValue = default!,
                               NetworkVariableReadPermission readPermission = NetworkVariableReadPermission.Everyone,
                               NetworkVariableWritePermission writePermission = NetworkVariableWritePermission.Server)
        {
            _value = initialValue;
            ReadPermission = readPermission;
            WritePermission = writePermission;
        }

        public Type ValueType => typeof(T);
        public object? BoxedValue => _value;
        public string FieldName => InternalFieldName;

        void INetworkVariable.Attach(NetworkBehaviour behaviour, string fieldName)
        {
            OwnerBehaviour = behaviour;
            InternalFieldName = fieldName;
        }

        void INetworkVariable.ClearDirty() => Dirty = false;

        void INetworkVariable.ApplyRemote(object? value)
        {
            if (value is T typed)
            {
                SetValue(typed, markDirty: false);
                return;
            }

            if (value == null)
            {
                SetValue(default!, markDirty: false);
                return;
            }

            Type targetType = typeof(T);
            Type effectiveType = Nullable.GetUnderlyingType(targetType) ?? targetType;
            object convertedValue;
            if (effectiveType.IsEnum)
            {
                convertedValue = value is string enumName
                    ? Enum.Parse(effectiveType, enumName, ignoreCase: true)
                    : Enum.ToObject(effectiveType, Convert.ToInt32(value));
            }
            else
            {
                convertedValue = Convert.ChangeType(value, effectiveType);
            }

            SetValue((T)convertedValue, markDirty: false);
        }

        private void SetValue(T value, bool markDirty)
        {
            if (markDirty &&
                OwnerBehaviour != null &&
                !ClaymoreEngine.Networking.MultiplayerRuntime.CanWriteVariable(OwnerBehaviour, WritePermission))
            {
                return;
            }

            if (EqualityComparer<T>.Default.Equals(_value, value))
                return;

            T previous = _value;
            _value = value;
            Dirty = markDirty && OwnerBehaviour != null && ClaymoreEngine.Multiplayer.NetworkManager.IsSessionActive;
            if (Dirty && OwnerBehaviour != null)
            {
                ClaymoreEngine.Networking.MultiplayerRuntime.NotifyVariableDirty(OwnerBehaviour);
            }

            OnValueChanged?.Invoke(previous, value);
        }
    }
}
