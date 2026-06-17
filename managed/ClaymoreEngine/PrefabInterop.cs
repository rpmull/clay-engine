using System;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public unsafe delegate void PrefabInteropInitDelegate(IntPtr* functionPointers, int count);

    // Managed-side bindings for native prefab functions.
    public static unsafe class PrefabInterop
    {
        // Native: int InstantiatePrefabByGuid(ulong hi, ulong lo)
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int InstantiatePrefabFn(ulong hi, ulong lo);
        
        // Native: int InstantiatePrefabByGuidBlocking(ulong hi, ulong lo)
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int InstantiatePrefabBlockingFn(ulong hi, ulong lo);

        // Native: int InstantiatePrefabByGuidWithRoot(ulong hi, ulong lo, int rootEntityId, bool useExistingRoot)
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int InstantiatePrefabWithRootFn(ulong hi, ulong lo, int rootEntityId, [MarshalAs(UnmanagedType.I1)] bool useExistingRoot);
        
        // Native: int GetPrefabAsyncStatus(int entityId)
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int GetAsyncStatusFn(int entityId);
        
        // Native: const char* GetAssetNameByGuid(ulong hi, ulong lo)
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr GetAssetNameByGuidFn(ulong hi, ulong lo);

        // Native: int PreloadPrefabByGuid(ulong hi, ulong lo)
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int PreloadPrefabFn(ulong hi, ulong lo);

        // Bound by native during startup
        public static InstantiatePrefabFn? Instantiate;
        public static InstantiatePrefabBlockingFn? InstantiateBlocking;
        public static InstantiatePrefabWithRootFn? InstantiateWithRoot;
        public static GetAsyncStatusFn? GetAsyncStatus;
        public static GetAssetNameByGuidFn? GetAssetNameByGuid;
        public static PreloadPrefabFn? Preload;

        public static void InitializeInteropExport(IntPtr* ptrs, int count) => InitializeInterop(ptrs, count);

        public static void InitializeInterop(IntPtr* ptrs, int count)
        {
            if (ptrs == null || count <= 0)
            {
                Console.WriteLine("[PrefabInterop] Invalid init args.");
                return;
            }

            try
            {
                int i = 0;
                if (count > i) Instantiate = Marshal.GetDelegateForFunctionPointer<InstantiatePrefabFn>(ptrs[i++]);
                if (count > i) InstantiateBlocking = Marshal.GetDelegateForFunctionPointer<InstantiatePrefabBlockingFn>(ptrs[i++]);
                if (count > i) GetAsyncStatus = Marshal.GetDelegateForFunctionPointer<GetAsyncStatusFn>(ptrs[i++]);
                if (count > i) GetAssetNameByGuid = Marshal.GetDelegateForFunctionPointer<GetAssetNameByGuidFn>(ptrs[i++]);
                if (count > i) InstantiateWithRoot = Marshal.GetDelegateForFunctionPointer<InstantiatePrefabWithRootFn>(ptrs[i++]);
                if (count > i) Preload = Marshal.GetDelegateForFunctionPointer<PreloadPrefabFn>(ptrs[i++]);
                Console.WriteLine($"[Managed] PrefabInterop delegates initialized ({i} functions).");
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[PrefabInterop] Initialization failed: {ex}");
            }
        }
    }
}



