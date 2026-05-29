using System;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    public class AudioSourceComponent : ComponentBase
    {
        public global::ClaymoreEngine.AudioClip AudioClip
        {
            get
            {
                if (AudioInterop.GetClipReference == null)
                    return default;

                AudioInterop.GetClipReference(entity.EntityID, out ulong hi, out ulong lo);
                if (hi == 0 && lo == 0)
                    return default;

                string guid = $"{hi:x16}{lo:x16}";
                return global::ClaymoreEngine.AudioClip.FromGuid(guid);
            }
            set
            {
                if (AudioInterop.SetClipReference == null)
                    return;

                if (!value.TryParseGuid(out ulong hi, out ulong lo))
                {
                    hi = 0;
                    lo = 0;
                }
                AudioInterop.SetClipReference(entity.EntityID, hi, lo);
            }
        }

        public string AudioPath
        {
            get
            {
                IntPtr ptr = AudioInterop.GetAudioPath?.Invoke(entity.EntityID) ?? IntPtr.Zero;
                return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) ?? string.Empty : string.Empty;
            }
            set => AudioInterop.SetAudioPath?.Invoke(entity.EntityID, value ?? string.Empty);
        }

        public float Volume
        {
            get => AudioInterop.GetVolume?.Invoke(entity.EntityID) ?? 1.0f;
            set => AudioInterop.SetVolume?.Invoke(entity.EntityID, value);
        }

        public float Pitch
        {
            get => AudioInterop.GetPitch?.Invoke(entity.EntityID) ?? 1.0f;
            set => AudioInterop.SetPitch?.Invoke(entity.EntityID, value);
        }

        public bool Loop
        {
            get => AudioInterop.GetLoop?.Invoke(entity.EntityID) ?? false;
            set => AudioInterop.SetLoop?.Invoke(entity.EntityID, value);
        }

        public bool PlayOnAwake
        {
            get => AudioInterop.GetPlayOnAwake?.Invoke(entity.EntityID) ?? true;
            set => AudioInterop.SetPlayOnAwake?.Invoke(entity.EntityID, value);
        }

        public bool Mute
        {
            get => AudioInterop.GetMute?.Invoke(entity.EntityID) ?? false;
            set => AudioInterop.SetMute?.Invoke(entity.EntityID, value);
        }

        public bool Spatial
        {
            get => AudioInterop.GetSpatial?.Invoke(entity.EntityID) ?? true;
            set => AudioInterop.SetSpatial?.Invoke(entity.EntityID, value);
        }

        public float MinDistance
        {
            get => AudioInterop.GetMinDistance?.Invoke(entity.EntityID) ?? 1.0f;
            set => AudioInterop.SetMinDistance?.Invoke(entity.EntityID, value);
        }

        public float MaxDistance
        {
            get => AudioInterop.GetMaxDistance?.Invoke(entity.EntityID) ?? 50.0f;
            set => AudioInterop.SetMaxDistance?.Invoke(entity.EntityID, value);
        }

        public float DopplerFactor
        {
            get => AudioInterop.GetDopplerFactor?.Invoke(entity.EntityID) ?? 1.0f;
            set => AudioInterop.SetDopplerFactor?.Invoke(entity.EntityID, value);
        }

        public float Rolloff
        {
            get => AudioInterop.GetRolloff?.Invoke(entity.EntityID) ?? 1.0f;
            set => AudioInterop.SetRolloff?.Invoke(entity.EntityID, value);
        }

        public bool IsPlaying => AudioInterop.GetIsPlaying?.Invoke(entity.EntityID) ?? false;

        public bool IsPaused => AudioInterop.GetIsPaused?.Invoke(entity.EntityID) ?? false;

        public void Play() => AudioInterop.Play?.Invoke(entity.EntityID);

        public void Stop() => AudioInterop.Stop?.Invoke(entity.EntityID);

        public void Pause() => AudioInterop.Pause?.Invoke(entity.EntityID);

        public void Resume() => AudioInterop.Resume?.Invoke(entity.EntityID);
    }
}
