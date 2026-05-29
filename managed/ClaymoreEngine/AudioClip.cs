using System;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    /// <summary>
    /// Reference to an audio asset. Use this in [SerializeField] fields and ClayObjects to
    /// assign project audio files that can later be played by an AudioSourceComponent.
    /// </summary>
    public struct AudioClip
    {
        /// <summary>
        /// 32-character hex GUID of the audio asset.
        /// </summary>
        public string? Guid;

        /// <summary>
        /// Returns true if this reference has a valid GUID assigned.
        /// </summary>
        public bool IsValid => !string.IsNullOrWhiteSpace(Guid) && Guid!.Length == 32;

        /// <summary>
        /// Gets the asset name for display/debugging purposes.
        /// </summary>
        public string name
        {
            get
            {
                if (!IsValid || !TryParseGuid(out ulong hi, out ulong lo))
                    return string.Empty;

                if (AudioInterop.GetAssetNameByGuid == null)
                    return string.Empty;

                IntPtr ptr = AudioInterop.GetAssetNameByGuid(hi, lo);
                return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) ?? string.Empty : string.Empty;
            }
        }

        /// <summary>
        /// Creates an AudioClip reference from a GUID string.
        /// </summary>
        public static AudioClip FromGuid(string guid)
        {
            return new AudioClip { Guid = guid };
        }

        /// <summary>
        /// Creates an AudioClip reference from a project path.
        /// </summary>
        public static AudioClip FromPath(string vfsPath)
        {
            if (string.IsNullOrWhiteSpace(vfsPath))
                return default;

            if (AudioInterop.GetGuidFromPath != null)
            {
                IntPtr guidPtr = AudioInterop.GetGuidFromPath(vfsPath);
                if (guidPtr != IntPtr.Zero)
                {
                    string? guid = Marshal.PtrToStringAnsi(guidPtr);
                    if (!string.IsNullOrWhiteSpace(guid) && guid!.Length == 32)
                    {
                        return new AudioClip { Guid = guid };
                    }
                }
            }

            string? resourceGuid = Resources.ResolveGuidFromPath(vfsPath);
            if (!string.IsNullOrWhiteSpace(resourceGuid) && resourceGuid!.Length == 32)
            {
                return new AudioClip { Guid = resourceGuid };
            }

            return default;
        }

        internal bool TryParseGuid(out ulong high, out ulong low)
        {
            high = 0;
            low = 0;
            if (string.IsNullOrWhiteSpace(Guid) || Guid!.Length != 32)
                return false;

            try
            {
                high = Convert.ToUInt64(Guid.Substring(0, 16), 16);
                low = Convert.ToUInt64(Guid.Substring(16, 16), 16);
                return true;
            }
            catch
            {
                return false;
            }
        }

        /// <summary>
        /// Serializes the audio reference to a string for interop.
        /// </summary>
        public string ToInteropString()
        {
            return Guid ?? string.Empty;
        }

        /// <summary>
        /// Parses an audio reference from its interop string.
        /// </summary>
        public static AudioClip FromInteropString(string? interopStr)
        {
            if (string.IsNullOrWhiteSpace(interopStr))
                return default;

            int colonIndex = interopStr!.IndexOf(':');
            if (colonIndex >= 0)
            {
                interopStr = interopStr.Substring(0, colonIndex);
            }

            return new AudioClip { Guid = interopStr };
        }

        public override string ToString()
        {
            if (!IsValid) return "(None)";
            return $"AudioClip({Guid?.Substring(0, 8)}...)";
        }
    }
}
