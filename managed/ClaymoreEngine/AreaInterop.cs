using System;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public unsafe delegate void AreaInteropInitDelegate(IntPtr* functionPointers, int count);

    public static unsafe class AreaInterop
    {
        // Native callback setter signatures (provided by native in InitializeInteropExport)
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void SetBodyEventFn(AreaBodyEventFn fn);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void SetAreaEventFn(AreaAreaEventFn fn);

        // Native -> Managed callback signatures
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void AreaBodyEventFn(int areaEntity, int otherEntity);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void AreaAreaEventFn(int areaEntity, int otherEntity);

        // Keep delegates alive so the GC doesn't collect them after registration
        private static AreaBodyEventFn s_BodyEnteredCb = OnBodyEntered;
        private static AreaBodyEventFn s_BodyExitedCb  = OnBodyExited;
        private static AreaAreaEventFn s_AreaEnteredCb = OnAreaEntered;
        private static AreaAreaEventFn s_AreaExitedCb  = OnAreaExited;

        // Public C# events for scripts
        public static event Action<int,int>? BodyEntered;
        public static event Action<int,int>? BodyExited;
        public static event Action<int,int>? AreaEntered;
        public static event Action<int,int>? AreaExited;

        // Managed static handlers invoked from native
        private static void OnBodyEntered(int areaEntity, int otherEntity) => BodyEntered?.Invoke(areaEntity, otherEntity);
        private static void OnBodyExited (int areaEntity, int otherEntity) => BodyExited?.Invoke(areaEntity, otherEntity);
        private static void OnAreaEntered(int areaEntity, int otherEntity) => AreaEntered?.Invoke(areaEntity, otherEntity);
        private static void OnAreaExited (int areaEntity, int otherEntity) => AreaExited?.Invoke(areaEntity, otherEntity);

        public static void InitializeInteropExport(IntPtr* ptrs, int count) => InitializeInterop(ptrs, count);

        public static unsafe void InitializeInterop(IntPtr* ptrs, int count)
        {
            if (count < 4)
            {
                Console.WriteLine($"[AreaInterop] Expected 4 pointers, got {count}.");
                return;
            }
            int i = 0;
            var setBodyEntered = Marshal.GetDelegateForFunctionPointer<SetBodyEventFn>(ptrs[i++]);
            var setBodyExited  = Marshal.GetDelegateForFunctionPointer<SetBodyEventFn>(ptrs[i++]);
            var setAreaEntered = Marshal.GetDelegateForFunctionPointer<SetAreaEventFn>(ptrs[i++]);
            var setAreaExited  = Marshal.GetDelegateForFunctionPointer<SetAreaEventFn>(ptrs[i++]);

            // Register managed callbacks with native
            setBodyEntered(s_BodyEnteredCb);
            setBodyExited (s_BodyExitedCb);
            setAreaEntered(s_AreaEnteredCb);
            setAreaExited (s_AreaExitedCb);

            Console.WriteLine("[Managed] AreaInterop delegates initialized.");
        }
    }
}
