using System;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public unsafe delegate void PortalInteropInitDelegate(IntPtr* functionPointers, int count);

    public static unsafe class PortalInterop
    {
        // Native callback setter signature
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void SetPortalCrossedFn(PortalCrossedFn fn);

        // Native -> Managed callback signature
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void PortalCrossedFn(int portalEntity, int otherEntity, int entering);

        private static PortalCrossedFn s_CrossedCb = OnPortalCrossed;

        public static event Action<int, int, bool>? PortalCrossed;

        private static void OnPortalCrossed(int portalEntity, int otherEntity, int entering)
            => PortalCrossed?.Invoke(portalEntity, otherEntity, entering != 0);

        public static void InitializeInteropExport(IntPtr* ptrs, int count) => InitializeInterop(ptrs, count);

        public static unsafe void InitializeInterop(IntPtr* ptrs, int count)
        {
            if (count < 1)
            {
                Console.WriteLine($"[PortalInterop] Expected 1 pointer, got {count}.");
                return;
            }

            var setCrossed = Marshal.GetDelegateForFunctionPointer<SetPortalCrossedFn>(ptrs[0]);
            setCrossed(s_CrossedCb);

            Console.WriteLine("[Managed] PortalInterop delegates initialized.");
        }
    }
}
