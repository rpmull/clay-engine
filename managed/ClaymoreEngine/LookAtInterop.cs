using System;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    /// <summary>
    /// Delegate type for LookAt interop initialization from native code.
    /// </summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public unsafe delegate void LookAtInteropInitDelegate(IntPtr* functionPointers, int count);

    /// <summary>
    /// LookAt constraint mode - how the constraint derives rotation.
    /// </summary>
    public enum LookAtMode
    {
        /// <summary>
        /// Rotate bones to face target entity's world position.
        /// Use for third-person cameras, NPCs looking at player, etc.
        /// </summary>
        LookAtPosition = 0,
        
        /// <summary>
        /// Copy target entity's yaw/pitch rotation directly.
        /// Use for FPS cameras where spine should rotate with camera facing.
        /// </summary>
        MatchRotation = 1
    }

    /// <summary>
    /// Interop bindings for LookAt/Aim constraint control from managed scripts.
    /// Allows scripts to dynamically control look-at behavior at runtime.
    /// </summary>
    public static unsafe class LookAtInterop
    {
        // Native function delegate types
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetEnabledFn(int entity, bool enabled);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetWeightFn(int entity, float weight);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetTargetFn(int entity, int targetEntity);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetSmoothingSpeedFn(int entity, float speed);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetMaxAnglesFn(int entity, float maxYaw, float maxPitch, float maxRoll);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate bool GetEnabledFn(int entity);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate float GetWeightFn(int entity);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetModeFn(int entity, int mode);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate int GetModeFn(int entity);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetTargetUsesNegativeZForwardFn(int entity, [MarshalAs(UnmanagedType.I1)] bool value);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate bool GetTargetUsesNegativeZForwardFn(int entity);

        // Static delegate instances (populated by native bootstrap)
        public static SetEnabledFn SetEnabled;
        public static SetWeightFn SetWeight;
        public static SetTargetFn SetTarget;
        public static SetSmoothingSpeedFn SetSmoothingSpeed;
        public static SetMaxAnglesFn SetMaxAngles;
        public static GetEnabledFn GetEnabled;
        public static GetWeightFn GetWeight;
        public static SetModeFn SetMode;
        public static GetModeFn GetMode;
        public static SetTargetUsesNegativeZForwardFn SetTargetUsesNegativeZForward;
        public static GetTargetUsesNegativeZForwardFn GetTargetUsesNegativeZForward;

        /// <summary>
        /// Called by native code during DotNetHost initialization.
        /// Must match the order of pointers passed from C++.
        /// </summary>
        public static void InitializeInteropExport(IntPtr* ptrs, int count)
        {
            const int ExpectedCount = 11;
            if (count < ExpectedCount)
            {
                Console.WriteLine($"[LookAtInterop] Expected {ExpectedCount} pointers, got {count}");
                return;
            }

            int i = 0;
            SetEnabled        = Marshal.GetDelegateForFunctionPointer<SetEnabledFn>(ptrs[i++]);
            SetWeight         = Marshal.GetDelegateForFunctionPointer<SetWeightFn>(ptrs[i++]);
            SetTarget         = Marshal.GetDelegateForFunctionPointer<SetTargetFn>(ptrs[i++]);
            SetSmoothingSpeed = Marshal.GetDelegateForFunctionPointer<SetSmoothingSpeedFn>(ptrs[i++]);
            SetMaxAngles      = Marshal.GetDelegateForFunctionPointer<SetMaxAnglesFn>(ptrs[i++]);
            GetEnabled        = Marshal.GetDelegateForFunctionPointer<GetEnabledFn>(ptrs[i++]);
            GetWeight         = Marshal.GetDelegateForFunctionPointer<GetWeightFn>(ptrs[i++]);
            SetMode           = Marshal.GetDelegateForFunctionPointer<SetModeFn>(ptrs[i++]);
            GetMode           = Marshal.GetDelegateForFunctionPointer<GetModeFn>(ptrs[i++]);
            SetTargetUsesNegativeZForward = Marshal.GetDelegateForFunctionPointer<SetTargetUsesNegativeZForwardFn>(ptrs[i++]);
            GetTargetUsesNegativeZForward = Marshal.GetDelegateForFunctionPointer<GetTargetUsesNegativeZForwardFn>(ptrs[i++]);

            Console.WriteLine("[Managed] LookAtInterop delegates initialized.");
        }
    }
}
