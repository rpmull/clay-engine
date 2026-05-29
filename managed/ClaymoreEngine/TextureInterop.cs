using System;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    /// <summary>
    /// Interop delegates for texture operations
    /// </summary>
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate IntPtr Texture_GetAssetNameByGuidFn(ulong guidHigh, ulong guidLow);
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate IntPtr Texture_GetGuidFromPathFn([MarshalAs(UnmanagedType.LPStr)] string vfsPath);
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void UI_Panel_SetTextureFn(int entityId, ulong guidHigh, ulong guidLow, int fileId);
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void UI_Panel_GetTextureFn(int entityId, out ulong guidHigh, out ulong guidLow, out int fileId);
    
    public static class TextureInterop
    {
        public static Texture_GetAssetNameByGuidFn? GetAssetNameByGuid;
        public static Texture_GetGuidFromPathFn? GetGuidFromPath;
        public static UI_Panel_SetTextureFn? UI_Panel_SetTexture;
        public static UI_Panel_GetTextureFn? UI_Panel_GetTexture;
        
        /// <summary>
        /// Initialize texture interop from function pointer array.
        /// Called during ComponentInterop initialization.
        /// </summary>
        public static void Initialize(IntPtr getAssetName, IntPtr getGuidFromPath, IntPtr panelSetTexture, IntPtr panelGetTexture)
        {
            if (getAssetName != IntPtr.Zero)
                GetAssetNameByGuid = Marshal.GetDelegateForFunctionPointer<Texture_GetAssetNameByGuidFn>(getAssetName);
            if (getGuidFromPath != IntPtr.Zero)
                GetGuidFromPath = Marshal.GetDelegateForFunctionPointer<Texture_GetGuidFromPathFn>(getGuidFromPath);
            if (panelSetTexture != IntPtr.Zero)
                UI_Panel_SetTexture = Marshal.GetDelegateForFunctionPointer<UI_Panel_SetTextureFn>(panelSetTexture);
            if (panelGetTexture != IntPtr.Zero)
                UI_Panel_GetTexture = Marshal.GetDelegateForFunctionPointer<UI_Panel_GetTextureFn>(panelGetTexture);
        }
    }
}
