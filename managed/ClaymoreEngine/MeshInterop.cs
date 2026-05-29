using System;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public unsafe delegate void MeshInteropInitDelegate(IntPtr* functionPointers, int count);

    /// <summary>
    /// Managed-side bindings for native mesh component functions.
    /// </summary>
    public static unsafe class MeshInterop
    {
        public const int ExpectedFunctionCount = 27;

        // Mesh instantiation and assignment
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate int InstantiateMeshFn(ulong hi, ulong lo, int fileID, [MarshalAs(UnmanagedType.LPStr)] string? entityName);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate int InstantiateMeshWithRootFn(ulong hi, ulong lo, int fileID, [MarshalAs(UnmanagedType.LPStr)] string? entityName, int rootEntityId, [MarshalAs(UnmanagedType.I1)] bool useExistingRoot);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate bool SetMeshFn(int entityID, ulong hi, ulong lo, int fileID);

        // Component queries
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate bool HasComponentFn(int entityID);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void GetReferenceFn(int entityID, out ulong hi, out ulong lo, out int fileID);

        // Mesh statistics (read-only)
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate int GetIntFn(int entityID);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate IntPtr GetIndexedStringFn(int entityID, int index);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void GetVec3Fn(int entityID, out float x, out float y, out float z);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate float GetFloatFn(int entityID);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetFloatFn(int entityID, float value);

        // Boolean properties
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate bool GetBoolFn(int entityID);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetBoolFn(int entityID, [MarshalAs(UnmanagedType.I1)] bool value);

        // Int properties
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetIntFn(int entityID, int value);

        // String properties
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate IntPtr GetStringFn(int entityID);
        
        // Asset name by GUID
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate IntPtr GetAssetNameByGuidFn(ulong hi, ulong lo);

        // Bound delegates
        public static InstantiateMeshFn? Instantiate;
        public static InstantiateMeshWithRootFn? InstantiateWithRoot;
        public static SetMeshFn? SetMesh;
        public static HasComponentFn? HasComponent;
        public static GetReferenceFn? GetReference;
        public static GetIntFn? GetVertexCount;
        public static GetIntFn? GetIndexCount;
        public static GetIntFn? GetSubmeshCount;
        public static GetIntFn? GetMaterialSlotCount;
        public static GetIndexedStringFn? GetMaterialSlotName;
        public static GetVec3Fn? GetBoundsMin;
        public static GetVec3Fn? GetBoundsMax;
        public static GetFloatFn? GetBoundsPadding;
        public static SetFloatFn? SetBoundsPadding;
        public static GetBoolFn? GetRenderOnTop;
        public static SetBoolFn? SetRenderOnTop;
        public static GetBoolFn? GetShowBackfaces;
        public static SetBoolFn? SetShowBackfaces;
        public static GetBoolFn? GetSkipFrustumCulling;
        public static SetBoolFn? SetSkipFrustumCulling;
        public static GetIntFn? GetRenderOrder;
        public static SetIntFn? SetRenderOrder;
        public static GetBoolFn? GetUniqueMaterial;
        public static SetBoolFn? SetUniqueMaterial;
        public static GetBoolFn? HasSkinning;
        public static GetStringFn? GetName;
        public static GetAssetNameByGuidFn? GetAssetNameByGuid;

        public static void InitializeInteropExport(IntPtr* ptrs, int count) => InitializeInterop(ptrs, count);

        public static void InitializeInterop(IntPtr* ptrs, int count)
        {
            if (ptrs == null || count <= 0)
            {
                Console.WriteLine("[MeshInterop] Invalid init args.");
                return;
            }

            try
            {
                if (count != ExpectedFunctionCount)
                {
                    Console.WriteLine($"[MeshInterop] Expected {ExpectedFunctionCount} functions but received {count}. Some mesh bindings may be unavailable.");
                }

                int i = 0;
                if (count > i) Instantiate = Marshal.GetDelegateForFunctionPointer<InstantiateMeshFn>(ptrs[i++]);
                if (count > i) SetMesh = Marshal.GetDelegateForFunctionPointer<SetMeshFn>(ptrs[i++]);
                if (count > i) HasComponent = Marshal.GetDelegateForFunctionPointer<HasComponentFn>(ptrs[i++]);
                if (count > i) GetReference = Marshal.GetDelegateForFunctionPointer<GetReferenceFn>(ptrs[i++]);
                if (count > i) GetVertexCount = Marshal.GetDelegateForFunctionPointer<GetIntFn>(ptrs[i++]);
                if (count > i) GetIndexCount = Marshal.GetDelegateForFunctionPointer<GetIntFn>(ptrs[i++]);
                if (count > i) GetSubmeshCount = Marshal.GetDelegateForFunctionPointer<GetIntFn>(ptrs[i++]);
                if (count > i) GetMaterialSlotCount = Marshal.GetDelegateForFunctionPointer<GetIntFn>(ptrs[i++]);
                if (count > i) GetMaterialSlotName = Marshal.GetDelegateForFunctionPointer<GetIndexedStringFn>(ptrs[i++]);
                if (count > i) GetBoundsMin = Marshal.GetDelegateForFunctionPointer<GetVec3Fn>(ptrs[i++]);
                if (count > i) GetBoundsMax = Marshal.GetDelegateForFunctionPointer<GetVec3Fn>(ptrs[i++]);
                if (count > i) GetBoundsPadding = Marshal.GetDelegateForFunctionPointer<GetFloatFn>(ptrs[i++]);
                if (count > i) SetBoundsPadding = Marshal.GetDelegateForFunctionPointer<SetFloatFn>(ptrs[i++]);
                if (count > i) GetRenderOnTop = Marshal.GetDelegateForFunctionPointer<GetBoolFn>(ptrs[i++]);
                if (count > i) SetRenderOnTop = Marshal.GetDelegateForFunctionPointer<SetBoolFn>(ptrs[i++]);
                if (count > i) GetShowBackfaces = Marshal.GetDelegateForFunctionPointer<GetBoolFn>(ptrs[i++]);
                if (count > i) SetShowBackfaces = Marshal.GetDelegateForFunctionPointer<SetBoolFn>(ptrs[i++]);
                if (count > i) GetSkipFrustumCulling = Marshal.GetDelegateForFunctionPointer<GetBoolFn>(ptrs[i++]);
                if (count > i) SetSkipFrustumCulling = Marshal.GetDelegateForFunctionPointer<SetBoolFn>(ptrs[i++]);
                if (count > i) GetRenderOrder = Marshal.GetDelegateForFunctionPointer<GetIntFn>(ptrs[i++]);
                if (count > i) SetRenderOrder = Marshal.GetDelegateForFunctionPointer<SetIntFn>(ptrs[i++]);
                if (count > i) GetUniqueMaterial = Marshal.GetDelegateForFunctionPointer<GetBoolFn>(ptrs[i++]);
                if (count > i) SetUniqueMaterial = Marshal.GetDelegateForFunctionPointer<SetBoolFn>(ptrs[i++]);
                if (count > i) HasSkinning = Marshal.GetDelegateForFunctionPointer<GetBoolFn>(ptrs[i++]);
                if (count > i) GetName = Marshal.GetDelegateForFunctionPointer<GetStringFn>(ptrs[i++]);
                if (count > i) GetAssetNameByGuid = Marshal.GetDelegateForFunctionPointer<GetAssetNameByGuidFn>(ptrs[i++]);
                if (count > i) InstantiateWithRoot = Marshal.GetDelegateForFunctionPointer<InstantiateMeshWithRootFn>(ptrs[i++]);
                Console.WriteLine($"[Managed] MeshInterop delegates initialized ({i} functions).");
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[MeshInterop] Initialization failed: {ex}");
            }
        }
    }
}
