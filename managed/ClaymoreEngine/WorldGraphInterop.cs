using System;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public unsafe delegate void WorldGraphInteropInitDelegate(IntPtr* functionPointers, int count);

    public static unsafe class WorldGraphInterop
    {
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool WorldGraph_LoadProjectFn();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool WorldGraph_IsLoadedFn();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int WorldGraph_GetPortalCountFn();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr WorldGraph_GetPortalScenePathFn(int index);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr WorldGraph_GetPortalGuidFn(int index);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr WorldGraph_GetPortalTargetScenePathFn(int index);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr WorldGraph_GetPortalTargetGuidFn(int index);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr WorldGraph_GetPortalPathFn(int index);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr WorldGraph_GetPortalTargetPathFn(int index);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void WorldGraph_GetPortalVec3Fn(int index, out float x, out float y, out float z);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool WorldGraph_IsPortalResolvedFn(int index);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float WorldGraph_GetPortalDistanceFn(int index);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int WorldGraph_FindPortalIndexFn([MarshalAs(UnmanagedType.LPStr)] string scenePath,
                                                                                                          [MarshalAs(UnmanagedType.LPStr)] string portalGuid);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int WorldGraph_GetPoiCountFn();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr WorldGraph_GetPoiStringFn(int index);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool WorldGraph_GetPoiIsPortalFn(int index);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int WorldGraph_FindPoiIndexFn([MarshalAs(UnmanagedType.LPStr)] string scenePath,
                                                                                                            [MarshalAs(UnmanagedType.LPStr)] string poiGuid);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float WorldGraph_GetPoiPortalDistanceFn([MarshalAs(UnmanagedType.LPStr)] string scenePath,
                                                                                                                      [MarshalAs(UnmanagedType.LPStr)] string poiGuid,
                                                                                                                      [MarshalAs(UnmanagedType.LPStr)] string portalGuid);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float WorldGraph_GetPortalPoiDistanceFn([MarshalAs(UnmanagedType.LPStr)] string scenePath,
                                                                                                                     [MarshalAs(UnmanagedType.LPStr)] string portalGuid,
                                                                                                                     [MarshalAs(UnmanagedType.LPStr)] string poiGuid);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float WorldGraph_GetPortalPortalDistanceFn([MarshalAs(UnmanagedType.LPStr)] string scenePath,
                                                                                                                         [MarshalAs(UnmanagedType.LPStr)] string fromPortalGuid,
                                                                                                                         [MarshalAs(UnmanagedType.LPStr)] string toPortalGuid);

        public static WorldGraph_LoadProjectFn LoadProject;
        public static WorldGraph_IsLoadedFn IsLoaded;
        public static WorldGraph_GetPortalCountFn GetPortalCount;
        public static WorldGraph_GetPortalScenePathFn GetPortalScenePathInternal;
        public static WorldGraph_GetPortalGuidFn GetPortalGuidInternal;
        public static WorldGraph_GetPortalTargetScenePathFn GetPortalTargetScenePathInternal;
        public static WorldGraph_GetPortalTargetGuidFn GetPortalTargetGuidInternal;
        public static WorldGraph_GetPortalPathFn GetPortalPathInternal;
        public static WorldGraph_GetPortalTargetPathFn GetPortalTargetPathInternal;
        public static WorldGraph_GetPortalVec3Fn GetPortalEntryPosition;
        public static WorldGraph_GetPortalVec3Fn GetPortalExitPosition;
        public static WorldGraph_IsPortalResolvedFn IsPortalResolved;
        public static WorldGraph_GetPortalDistanceFn GetPortalDistance;
        public static WorldGraph_FindPortalIndexFn FindPortalIndex;
        public static WorldGraph_GetPoiCountFn GetPoiCount;
        public static WorldGraph_GetPoiStringFn GetPoiScenePathInternal;
        public static WorldGraph_GetPoiStringFn GetPoiGuidInternal;
        public static WorldGraph_GetPoiStringFn GetPoiPathInternal;
        public static WorldGraph_GetPoiStringFn GetPoiScriptClassInternal;
        public static WorldGraph_GetPoiStringFn GetPoiNodeNameInternal;
        public static WorldGraph_GetPoiStringFn GetPoiNodeTypeInternal;
        public static WorldGraph_GetPoiIsPortalFn GetPoiIsPortal;
        public static WorldGraph_GetPortalVec3Fn GetPoiPosition;
        public static WorldGraph_FindPoiIndexFn FindPoiIndex;
        public static WorldGraph_GetPoiPortalDistanceFn GetPoiToPortalDistance;
        public static WorldGraph_GetPortalPoiDistanceFn GetPortalToPoiDistance;
        public static WorldGraph_GetPortalPortalDistanceFn GetPortalToPortalDistance;

        public static void InitializeInteropExport(IntPtr* ptrs, int count) => InitializeInterop(ptrs, count);

        public static unsafe void InitializeInterop(IntPtr* ptrs, int count)
        {
            if (count < 27)
            {
                Console.WriteLine($"[WorldGraphInterop] Expected 27 pointers, got {count}.");
                return;
            }

            int i = 0;
            LoadProject = Marshal.GetDelegateForFunctionPointer<WorldGraph_LoadProjectFn>(ptrs[i++]);
            IsLoaded = Marshal.GetDelegateForFunctionPointer<WorldGraph_IsLoadedFn>(ptrs[i++]);
            GetPortalCount = Marshal.GetDelegateForFunctionPointer<WorldGraph_GetPortalCountFn>(ptrs[i++]);
            GetPortalScenePathInternal = Marshal.GetDelegateForFunctionPointer<WorldGraph_GetPortalScenePathFn>(ptrs[i++]);
            GetPortalGuidInternal = Marshal.GetDelegateForFunctionPointer<WorldGraph_GetPortalGuidFn>(ptrs[i++]);
            GetPortalTargetScenePathInternal = Marshal.GetDelegateForFunctionPointer<WorldGraph_GetPortalTargetScenePathFn>(ptrs[i++]);
            GetPortalTargetGuidInternal = Marshal.GetDelegateForFunctionPointer<WorldGraph_GetPortalTargetGuidFn>(ptrs[i++]);
            GetPortalPathInternal = Marshal.GetDelegateForFunctionPointer<WorldGraph_GetPortalPathFn>(ptrs[i++]);
            GetPortalTargetPathInternal = Marshal.GetDelegateForFunctionPointer<WorldGraph_GetPortalTargetPathFn>(ptrs[i++]);
            GetPortalEntryPosition = Marshal.GetDelegateForFunctionPointer<WorldGraph_GetPortalVec3Fn>(ptrs[i++]);
            GetPortalExitPosition = Marshal.GetDelegateForFunctionPointer<WorldGraph_GetPortalVec3Fn>(ptrs[i++]);
            IsPortalResolved = Marshal.GetDelegateForFunctionPointer<WorldGraph_IsPortalResolvedFn>(ptrs[i++]);
            GetPortalDistance = Marshal.GetDelegateForFunctionPointer<WorldGraph_GetPortalDistanceFn>(ptrs[i++]);
            FindPortalIndex = Marshal.GetDelegateForFunctionPointer<WorldGraph_FindPortalIndexFn>(ptrs[i++]);
            GetPoiCount = Marshal.GetDelegateForFunctionPointer<WorldGraph_GetPoiCountFn>(ptrs[i++]);
            GetPoiScenePathInternal = Marshal.GetDelegateForFunctionPointer<WorldGraph_GetPoiStringFn>(ptrs[i++]);
            GetPoiGuidInternal = Marshal.GetDelegateForFunctionPointer<WorldGraph_GetPoiStringFn>(ptrs[i++]);
            GetPoiPathInternal = Marshal.GetDelegateForFunctionPointer<WorldGraph_GetPoiStringFn>(ptrs[i++]);
            GetPoiScriptClassInternal = Marshal.GetDelegateForFunctionPointer<WorldGraph_GetPoiStringFn>(ptrs[i++]);
            GetPoiNodeNameInternal = Marshal.GetDelegateForFunctionPointer<WorldGraph_GetPoiStringFn>(ptrs[i++]);
            GetPoiNodeTypeInternal = Marshal.GetDelegateForFunctionPointer<WorldGraph_GetPoiStringFn>(ptrs[i++]);
            GetPoiIsPortal = Marshal.GetDelegateForFunctionPointer<WorldGraph_GetPoiIsPortalFn>(ptrs[i++]);
            GetPoiPosition = Marshal.GetDelegateForFunctionPointer<WorldGraph_GetPortalVec3Fn>(ptrs[i++]);
            FindPoiIndex = Marshal.GetDelegateForFunctionPointer<WorldGraph_FindPoiIndexFn>(ptrs[i++]);
            GetPoiToPortalDistance = Marshal.GetDelegateForFunctionPointer<WorldGraph_GetPoiPortalDistanceFn>(ptrs[i++]);
            GetPortalToPoiDistance = Marshal.GetDelegateForFunctionPointer<WorldGraph_GetPortalPoiDistanceFn>(ptrs[i++]);
            GetPortalToPortalDistance = Marshal.GetDelegateForFunctionPointer<WorldGraph_GetPortalPortalDistanceFn>(ptrs[i++]);

            Console.WriteLine("[Managed] WorldGraphInterop delegates initialized.");
        }

        public static string GetPortalScenePath(int index)
        {
            if (GetPortalScenePathInternal == null) return string.Empty;
            IntPtr ptr = GetPortalScenePathInternal(index);
            return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) : string.Empty;
        }

        public static string GetPortalGuid(int index)
        {
            if (GetPortalGuidInternal == null) return string.Empty;
            IntPtr ptr = GetPortalGuidInternal(index);
            return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) : string.Empty;
        }

        public static string GetPortalTargetScenePath(int index)
        {
            if (GetPortalTargetScenePathInternal == null) return string.Empty;
            IntPtr ptr = GetPortalTargetScenePathInternal(index);
            return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) : string.Empty;
        }

        public static string GetPortalTargetGuid(int index)
        {
            if (GetPortalTargetGuidInternal == null) return string.Empty;
            IntPtr ptr = GetPortalTargetGuidInternal(index);
            return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) : string.Empty;
        }

        public static string GetPortalPath(int index)
        {
            if (GetPortalPathInternal == null) return string.Empty;
            IntPtr ptr = GetPortalPathInternal(index);
            return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) : string.Empty;
        }

        public static string GetPortalTargetPath(int index)
        {
            if (GetPortalTargetPathInternal == null) return string.Empty;
            IntPtr ptr = GetPortalTargetPathInternal(index);
            return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) : string.Empty;
        }

        public static string GetPoiScenePath(int index)
        {
            if (GetPoiScenePathInternal == null) return string.Empty;
            IntPtr ptr = GetPoiScenePathInternal(index);
            return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) : string.Empty;
        }

        public static string GetPoiGuid(int index)
        {
            if (GetPoiGuidInternal == null) return string.Empty;
            IntPtr ptr = GetPoiGuidInternal(index);
            return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) : string.Empty;
        }

        public static string GetPoiPath(int index)
        {
            if (GetPoiPathInternal == null) return string.Empty;
            IntPtr ptr = GetPoiPathInternal(index);
            return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) : string.Empty;
        }

        public static string GetPoiScriptClass(int index)
        {
            if (GetPoiScriptClassInternal == null) return string.Empty;
            IntPtr ptr = GetPoiScriptClassInternal(index);
            return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) : string.Empty;
        }

        public static string GetPoiNodeName(int index)
        {
            if (GetPoiNodeNameInternal == null) return string.Empty;
            IntPtr ptr = GetPoiNodeNameInternal(index);
            return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) : string.Empty;
        }

        public static string GetPoiNodeType(int index)
        {
            if (GetPoiNodeTypeInternal == null) return string.Empty;
            IntPtr ptr = GetPoiNodeTypeInternal(index);
            return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) : string.Empty;
        }
    }
}
