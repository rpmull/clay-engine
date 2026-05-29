using System;
using System.Runtime.InteropServices;
using System.Collections.Generic;

public enum PinKind { Input, Output }
public enum ValueType { Bool, Int, Float, String, Entity, Any }
public record PinDesc(string Name, PinKind Kind, ValueType Type);
public record FieldDesc(string Name, ValueType Type, float Min = 0, float Max = 0, string[]? Options = null);

// Minimal placeholder for now; will be populated with Get/Set delegates later
public sealed class EvaluatorIO { }

// The node "class" is just a struct with a payload + callbacks
public sealed class ManagedNodeType
   {
   public uint TypeId;             // hash("Quest.HasFlag")
   public string Name;             // "Has Flag"
   public Func<IntPtr, IntPtr> Create;                     // user -> payloadHandle
   public Action<IntPtr, IntPtr> Destroy;                  // (user, payload)
   public Func<IntPtr, (FieldDesc[], PinDesc[])> Describe; // schema
   public Func<IntPtr, IntPtr, int, object?> GetField;     // (user, payload, idx)
   public Action<IntPtr, IntPtr, int, object?> SetField;   // ...
   public Func<IntPtr, IntPtr, byte[]> Serialize;          // payload -> bytes
   public Func<IntPtr, byte[], IntPtr> Deserialize;        // bytes -> payload
   public Action<IntPtr, IntPtr, EvaluatorIO> Evaluate;    // main meat
   }

namespace ClaymoreEngine
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void GetManagedNodeGraphAPIDelegate(ref NativeNodeGraphAPI native, out ManagedNodeGraphAPI managedOut);

    [StructLayout(LayoutKind.Sequential)]
    public struct NativeNodeGraphAPI
    {
        public IntPtr user;
        public IntPtr RegisterNodeType; // function pointer
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct ManagedNodeGraphAPI
    {
        public IntPtr EnumerateNodeTypes; // function pointer
    }

    public static class NodeGraphInteropExports
    {
        private static NativeNodeGraphAPI _native;
        private static ManagedNodeGraphAPI _managed;

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void RegisterNodeTypeDelegate(IntPtr descPtr);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void EnumerateDelegate(IntPtr user);

        private static RegisterNodeTypeDelegate? _registerNodeType;
        private static EnumerateDelegate? _enumerate;

        // Simple registrar list; game scripts add to this before engine bootstraps
        private static readonly List<ManagedNodeType> _registered = new();

        public static void Register(ManagedNodeType type)
        {
            _registered.Add(type);
        }

        // Managed-side API filler for native to call back
        public static void GetNodeGraphAPI(ref NativeNodeGraphAPI native, out ManagedNodeGraphAPI managedOut)
        {
            _native = native;
            _registerNodeType = Marshal.GetDelegateForFunctionPointer<RegisterNodeTypeDelegate>(native.RegisterNodeType);
            _enumerate = EnumerateAll;
            _managed = new ManagedNodeGraphAPI
            {
                EnumerateNodeTypes = Marshal.GetFunctionPointerForDelegate(_enumerate)
            };
            managedOut = _managed;
        }

        private static void EnumerateAll(IntPtr user)
        {
            if (_registerNodeType == null) return;
            foreach (var t in _registered)
            {
                // Marshal minimal desc for now (name + typeId). Fields/pins can be extended later.
                using var nameUtf8 = new Utf8String(t.Name);
                // Layout: TypeId(uint32), name(char*), fields/pins omitted for MVP
                unsafe
                {
                    // stack allocate a small struct compatible with native (TypeId + const char*)
                    var desc = new NodeTypeDescNativeSimple { typeId = t.TypeId, name = nameUtf8.IntPtr };
                    IntPtr ptr = new IntPtr(&desc);
                    _registerNodeType(ptr);
                }
            }
        }

        // Simple native-compatible struct for MVP
        private unsafe struct NodeTypeDescNativeSimple
        {
            public uint typeId;
            public IntPtr name;
        }
    }

    // Helper for pinvoke UTF-8 strings
    internal sealed class Utf8String : IDisposable
    {
        public IntPtr IntPtr { get; }
        public Utf8String(string s)
        {
            var bytes = System.Text.Encoding.UTF8.GetBytes(s + "\0");
            IntPtr = Marshal.AllocHGlobal(bytes.Length);
            Marshal.Copy(bytes, 0, IntPtr, bytes.Length);
        }
        public void Dispose() => Marshal.FreeHGlobal(IntPtr);
    }
}
