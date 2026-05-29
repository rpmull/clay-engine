using System;
using System.Numerics;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public unsafe delegate void NavigationInteropInitDelegate(IntPtr* functionPointers, int count);

    public static unsafe class NavigationInterop
    {
        // Delegate types - use separate floats for vectors to avoid ABI issues
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool FindPathFn(int navMeshEntity, float startX, float startY, float startZ, float endX, float endY, float endZ, NavAgentParams p, uint include, uint exclude, IntPtr outPath);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void AgentSetDestFn(int agentEntity, float destX, float destY, float destZ);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void AgentStopFn(int agentEntity);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void AgentWarpFn(int agentEntity, float posX, float posY, float posZ);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float AgentRemainingFn(int agentEntity);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void OnPathCompleteFn(ulong managedHandle, bool success);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool AgentGetBoolFn(int agentEntity);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float AgentGetFloatFn(int agentEntity);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void AgentSetFloatFn(int agentEntity, float value);

        // Original function pointers
        public static FindPathFn FindPath;
        public static AgentSetDestFn AgentSetDestination;
        public static AgentStopFn AgentStop;
        public static AgentWarpFn AgentWarp;
        public static AgentRemainingFn AgentRemainingDistance;
        public static OnPathCompleteFn OnPathComplete;

        // State query function pointers
        public static AgentGetBoolFn AgentIsStopped;
        public static AgentGetBoolFn AgentIsMoving;
        public static AgentGetBoolFn AgentHasPath;

        // Parameter get/set function pointers
        public static AgentGetFloatFn AgentGetSpeed;
        public static AgentSetFloatFn AgentSetSpeed;
        public static AgentGetFloatFn AgentGetAcceleration;
        public static AgentSetFloatFn AgentSetAcceleration;
        public static AgentGetFloatFn AgentGetRadius;
        public static AgentSetFloatFn AgentSetRadius;
        public static AgentGetFloatFn AgentGetHeight;
        public static AgentSetFloatFn AgentSetHeight;
        public static AgentGetFloatFn AgentGetStoppingDistance;
        public static AgentSetFloatFn AgentSetStoppingDistance;
        
        // Velocity getters (read-only, computed by navigation system)
        public static AgentGetFloatFn AgentGetVelocityX;
        public static AgentGetFloatFn AgentGetVelocityY;
        public static AgentGetFloatFn AgentGetVelocityZ;

        public static void InitializeInteropExport(IntPtr* ptrs, int count)
        {
            if (count < 22) { Console.WriteLine($"[NavigationInterop] Expected 22 pointers, got {count}"); return; }
            int i = 0;
            FindPath = Marshal.GetDelegateForFunctionPointer<FindPathFn>(ptrs[i++]);
            AgentSetDestination = Marshal.GetDelegateForFunctionPointer<AgentSetDestFn>(ptrs[i++]);
            AgentStop = Marshal.GetDelegateForFunctionPointer<AgentStopFn>(ptrs[i++]);
            AgentWarp = Marshal.GetDelegateForFunctionPointer<AgentWarpFn>(ptrs[i++]);
            AgentRemainingDistance = Marshal.GetDelegateForFunctionPointer<AgentRemainingFn>(ptrs[i++]);
            OnPathComplete = Marshal.GetDelegateForFunctionPointer<OnPathCompleteFn>(ptrs[i++]);
            AgentIsStopped = Marshal.GetDelegateForFunctionPointer<AgentGetBoolFn>(ptrs[i++]);
            AgentIsMoving = Marshal.GetDelegateForFunctionPointer<AgentGetBoolFn>(ptrs[i++]);
            AgentHasPath = Marshal.GetDelegateForFunctionPointer<AgentGetBoolFn>(ptrs[i++]);
            AgentGetSpeed = Marshal.GetDelegateForFunctionPointer<AgentGetFloatFn>(ptrs[i++]);
            AgentSetSpeed = Marshal.GetDelegateForFunctionPointer<AgentSetFloatFn>(ptrs[i++]);
            AgentGetAcceleration = Marshal.GetDelegateForFunctionPointer<AgentGetFloatFn>(ptrs[i++]);
            AgentSetAcceleration = Marshal.GetDelegateForFunctionPointer<AgentSetFloatFn>(ptrs[i++]);
            AgentGetRadius = Marshal.GetDelegateForFunctionPointer<AgentGetFloatFn>(ptrs[i++]);
            AgentSetRadius = Marshal.GetDelegateForFunctionPointer<AgentSetFloatFn>(ptrs[i++]);
            AgentGetHeight = Marshal.GetDelegateForFunctionPointer<AgentGetFloatFn>(ptrs[i++]);
            AgentSetHeight = Marshal.GetDelegateForFunctionPointer<AgentSetFloatFn>(ptrs[i++]);
            AgentGetStoppingDistance = Marshal.GetDelegateForFunctionPointer<AgentGetFloatFn>(ptrs[i++]);
            AgentSetStoppingDistance = Marshal.GetDelegateForFunctionPointer<AgentSetFloatFn>(ptrs[i++]);
            AgentGetVelocityX = Marshal.GetDelegateForFunctionPointer<AgentGetFloatFn>(ptrs[i++]);
            AgentGetVelocityY = Marshal.GetDelegateForFunctionPointer<AgentGetFloatFn>(ptrs[i++]);
            AgentGetVelocityZ = Marshal.GetDelegateForFunctionPointer<AgentGetFloatFn>(ptrs[i++]);
            Console.WriteLine("[Managed] NavigationInterop delegates initialized.");
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct NavAgentParams
        {
            public float radius, height, maxSlopeDeg, maxStep, maxSpeed, maxAccel;
            public int preferredDomainId;
        }
    }
}


