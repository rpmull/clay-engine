using System;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    public enum CollisionOtherKind : byte
    {
        Unknown = 0,
        RigidBody = 1,
        StaticBody = 2,
        CharacterController = 3
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public unsafe delegate void CollisionInteropInitDelegate(IntPtr* functionPointers, int count);

    public static unsafe class CollisionInterop
    {
        // Native callback setter signatures
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetCollisionEventFn(CollisionEventFn fn);

        // Native -> Managed callback signature
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void CollisionEventFn(int selfEntity, int otherEntity, CollisionOtherKind otherKind);

        // Keep delegates alive so the GC doesn't collect them after registration
        private static CollisionEventFn s_OnEnterCb = OnEnter;
        private static CollisionEventFn s_OnExitCb  = OnExit;

        // Public C# events for scripts
        public static event Action<int, int, CollisionOtherKind>? Entered;
        public static event Action<int, int, CollisionOtherKind>? Exited;

        // Managed static handlers invoked from native
        private static void OnEnter(int selfEntity, int otherEntity, CollisionOtherKind otherKind) => Entered?.Invoke(selfEntity, otherEntity, otherKind);
        private static void OnExit(int selfEntity, int otherEntity, CollisionOtherKind otherKind) => Exited?.Invoke(selfEntity, otherEntity, otherKind);

        public static void InitializeInteropExport(IntPtr* ptrs, int count) => InitializeInterop(ptrs, count);

        // Expects pointer order: [SetOnEnter, SetOnExit]
        public static void InitializeInterop(IntPtr* ptrs, int count)
        {
            if (ptrs == null || count < 2)
            {
                Console.WriteLine($"[CollisionInterop] Expected 2 pointers, got {count}.");
                return;
            }

            int i = 0;
            var setOnEnter = Marshal.GetDelegateForFunctionPointer<SetCollisionEventFn>(ptrs[i++]);
            var setOnExit = Marshal.GetDelegateForFunctionPointer<SetCollisionEventFn>(ptrs[i++]);

            setOnEnter(s_OnEnterCb);
            setOnExit(s_OnExitCb);

            Console.WriteLine("[Managed] CollisionInterop delegates initialized.");
        }
    }
}

