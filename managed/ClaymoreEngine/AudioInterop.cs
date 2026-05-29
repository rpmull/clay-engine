using System;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public unsafe delegate void AudioInteropInitDelegate(IntPtr* functionPointers, int count);

    public static unsafe class AudioInterop
    {
        public const int ExpectedFunctionCount = 32;

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate IntPtr GetAssetNameByGuidFn(ulong guidHigh, ulong guidLow);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate IntPtr GetGuidFromPathFn([MarshalAs(UnmanagedType.LPStr)] string vfsPath);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void GetClipReferenceFn(int entityID, out ulong guidHigh, out ulong guidLow);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetClipReferenceFn(int entityID, ulong guidHigh, ulong guidLow);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate IntPtr GetStringFn(int entityID);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetStringFn(int entityID, [MarshalAs(UnmanagedType.LPStr)] string value);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate float GetFloatFn(int entityID);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetFloatFn(int entityID, float value);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate bool GetBoolFn(int entityID);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetBoolFn(int entityID, [MarshalAs(UnmanagedType.I1)] bool value);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void CommandFn(int entityID);

        public static GetAssetNameByGuidFn? GetAssetNameByGuid;
        public static GetGuidFromPathFn? GetGuidFromPath;
        public static GetClipReferenceFn? GetClipReference;
        public static SetClipReferenceFn? SetClipReference;
        public static GetStringFn? GetAudioPath;
        public static SetStringFn? SetAudioPath;
        public static GetFloatFn? GetVolume;
        public static SetFloatFn? SetVolume;
        public static GetFloatFn? GetPitch;
        public static SetFloatFn? SetPitch;
        public static GetBoolFn? GetLoop;
        public static SetBoolFn? SetLoop;
        public static GetBoolFn? GetPlayOnAwake;
        public static SetBoolFn? SetPlayOnAwake;
        public static GetBoolFn? GetMute;
        public static SetBoolFn? SetMute;
        public static GetBoolFn? GetSpatial;
        public static SetBoolFn? SetSpatial;
        public static GetFloatFn? GetMinDistance;
        public static SetFloatFn? SetMinDistance;
        public static GetFloatFn? GetMaxDistance;
        public static SetFloatFn? SetMaxDistance;
        public static GetFloatFn? GetDopplerFactor;
        public static SetFloatFn? SetDopplerFactor;
        public static GetFloatFn? GetRolloff;
        public static SetFloatFn? SetRolloff;
        public static GetBoolFn? GetIsPlaying;
        public static GetBoolFn? GetIsPaused;
        public static CommandFn? Play;
        public static CommandFn? Stop;
        public static CommandFn? Pause;
        public static CommandFn? Resume;

        public static void InitializeInteropExport(IntPtr* ptrs, int count) => InitializeInterop(ptrs, count);

        public static void InitializeInterop(IntPtr* ptrs, int count)
        {
            if (ptrs == null || count <= 0)
            {
                Console.WriteLine("[AudioInterop] Invalid init args.");
                return;
            }

            try
            {
                if (count != ExpectedFunctionCount)
                {
                    Console.WriteLine($"[AudioInterop] Expected {ExpectedFunctionCount} functions but received {count}. Some audio bindings may be unavailable.");
                }

                int i = 0;
                if (count > i) GetAssetNameByGuid = Marshal.GetDelegateForFunctionPointer<GetAssetNameByGuidFn>(ptrs[i++]);
                if (count > i) GetGuidFromPath = Marshal.GetDelegateForFunctionPointer<GetGuidFromPathFn>(ptrs[i++]);
                if (count > i) GetClipReference = Marshal.GetDelegateForFunctionPointer<GetClipReferenceFn>(ptrs[i++]);
                if (count > i) SetClipReference = Marshal.GetDelegateForFunctionPointer<SetClipReferenceFn>(ptrs[i++]);
                if (count > i) GetAudioPath = Marshal.GetDelegateForFunctionPointer<GetStringFn>(ptrs[i++]);
                if (count > i) SetAudioPath = Marshal.GetDelegateForFunctionPointer<SetStringFn>(ptrs[i++]);
                if (count > i) GetVolume = Marshal.GetDelegateForFunctionPointer<GetFloatFn>(ptrs[i++]);
                if (count > i) SetVolume = Marshal.GetDelegateForFunctionPointer<SetFloatFn>(ptrs[i++]);
                if (count > i) GetPitch = Marshal.GetDelegateForFunctionPointer<GetFloatFn>(ptrs[i++]);
                if (count > i) SetPitch = Marshal.GetDelegateForFunctionPointer<SetFloatFn>(ptrs[i++]);
                if (count > i) GetLoop = Marshal.GetDelegateForFunctionPointer<GetBoolFn>(ptrs[i++]);
                if (count > i) SetLoop = Marshal.GetDelegateForFunctionPointer<SetBoolFn>(ptrs[i++]);
                if (count > i) GetPlayOnAwake = Marshal.GetDelegateForFunctionPointer<GetBoolFn>(ptrs[i++]);
                if (count > i) SetPlayOnAwake = Marshal.GetDelegateForFunctionPointer<SetBoolFn>(ptrs[i++]);
                if (count > i) GetMute = Marshal.GetDelegateForFunctionPointer<GetBoolFn>(ptrs[i++]);
                if (count > i) SetMute = Marshal.GetDelegateForFunctionPointer<SetBoolFn>(ptrs[i++]);
                if (count > i) GetSpatial = Marshal.GetDelegateForFunctionPointer<GetBoolFn>(ptrs[i++]);
                if (count > i) SetSpatial = Marshal.GetDelegateForFunctionPointer<SetBoolFn>(ptrs[i++]);
                if (count > i) GetMinDistance = Marshal.GetDelegateForFunctionPointer<GetFloatFn>(ptrs[i++]);
                if (count > i) SetMinDistance = Marshal.GetDelegateForFunctionPointer<SetFloatFn>(ptrs[i++]);
                if (count > i) GetMaxDistance = Marshal.GetDelegateForFunctionPointer<GetFloatFn>(ptrs[i++]);
                if (count > i) SetMaxDistance = Marshal.GetDelegateForFunctionPointer<SetFloatFn>(ptrs[i++]);
                if (count > i) GetDopplerFactor = Marshal.GetDelegateForFunctionPointer<GetFloatFn>(ptrs[i++]);
                if (count > i) SetDopplerFactor = Marshal.GetDelegateForFunctionPointer<SetFloatFn>(ptrs[i++]);
                if (count > i) GetRolloff = Marshal.GetDelegateForFunctionPointer<GetFloatFn>(ptrs[i++]);
                if (count > i) SetRolloff = Marshal.GetDelegateForFunctionPointer<SetFloatFn>(ptrs[i++]);
                if (count > i) GetIsPlaying = Marshal.GetDelegateForFunctionPointer<GetBoolFn>(ptrs[i++]);
                if (count > i) GetIsPaused = Marshal.GetDelegateForFunctionPointer<GetBoolFn>(ptrs[i++]);
                if (count > i) Play = Marshal.GetDelegateForFunctionPointer<CommandFn>(ptrs[i++]);
                if (count > i) Stop = Marshal.GetDelegateForFunctionPointer<CommandFn>(ptrs[i++]);
                if (count > i) Pause = Marshal.GetDelegateForFunctionPointer<CommandFn>(ptrs[i++]);
                if (count > i) Resume = Marshal.GetDelegateForFunctionPointer<CommandFn>(ptrs[i++]);
                Console.WriteLine($"[Managed] AudioInterop delegates initialized ({i} functions).");
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[AudioInterop] Initialization failed: {ex}");
            }
        }
    }
}
