using System;
using System.Runtime.InteropServices;

namespace ClaymoreEngine.Networking
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public unsafe delegate void NetworkingInteropInitDelegate(IntPtr* functionPointers, int count);

    public static unsafe class NetworkingInterop
    {
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate IntPtr GetEntityGuidFn(int entityId);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate int FindEntityByGuidFn([MarshalAs(UnmanagedType.LPStr)] string guid);

        public static GetEntityGuidFn? GetEntityGuidRaw;
        public static FindEntityByGuidFn? FindEntityByGuidRaw;

        public static void InitializeInteropExport(IntPtr* functionPointers, int count) => InitializeInterop(functionPointers, count);

        public static void InitializeInterop(IntPtr* functionPointers, int count)
        {
            if (functionPointers == null || count < 2)
            {
                Console.WriteLine("[NetworkingInterop] Expected 2 function pointers.");
                return;
            }

            GetEntityGuidRaw = Marshal.GetDelegateForFunctionPointer<GetEntityGuidFn>(functionPointers[0]);
            FindEntityByGuidRaw = Marshal.GetDelegateForFunctionPointer<FindEntityByGuidFn>(functionPointers[1]);
            Console.WriteLine("[NetworkingInterop] Native multiplayer identity hooks initialized.");
        }

        public static string GetEntityGuid(int entityId)
        {
            if (GetEntityGuidRaw == null)
                return string.Empty;

            IntPtr ptr = GetEntityGuidRaw(entityId);
            return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) ?? string.Empty : string.Empty;
        }

        public static int FindEntityByGuid(string guid)
        {
            if (FindEntityByGuidRaw == null || string.IsNullOrWhiteSpace(guid))
                return -1;

            return FindEntityByGuidRaw(guid);
        }
    }
}
