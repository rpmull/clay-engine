using System;
using System.Numerics;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    /// <summary>
    /// Reference to a Texture asset. Use this in [SerializeField] fields to allow
    /// dragging texture files (PNG, JPG, etc.) from the Project panel.
    /// 
    /// At runtime, the reference resolves to the loaded texture.
    /// </summary>
    public struct Texture
    {
        /// <summary>
        /// 32-character hex GUID of the texture asset (points to .meta file).
        /// </summary>
        public string? Guid;

        /// <summary>
        /// File ID (typically 0 for textures).
        /// </summary>
        public int FileID;

        /// <summary>
        /// Returns true if this Texture reference has a valid GUID assigned.
        /// Use this instead of == null checks since Texture is a struct.
        /// </summary>
        public bool IsValid => !string.IsNullOrWhiteSpace(Guid) && Guid!.Length == 32;

        /// <summary>
        /// Gets the name of the texture asset.
        /// </summary>
        public string name
        {
            get
            {
                if (!IsValid || !TryParseGuid(out ulong hi, out ulong lo))
                    return string.Empty;
                
                if (TextureInterop.GetAssetNameByGuid == null)
                    return string.Empty;
                
                IntPtr ptr = TextureInterop.GetAssetNameByGuid(hi, lo);
                return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) ?? string.Empty : string.Empty;
            }
        }

        /// <summary>
        /// Creates a Texture reference from a GUID string.
        /// </summary>
        public static Texture FromGuid(string guid, int fileId = 0)
        {
            return new Texture { Guid = guid, FileID = fileId };
        }

        /// <summary>
        /// Creates a Texture reference from a VFS path (e.g., "assets/icons/sword.png").
        /// The path is resolved to a GUID at runtime via AssetLibrary lookup.
        /// </summary>
        public static Texture FromPath(string vfsPath)
        {
            if (string.IsNullOrWhiteSpace(vfsPath))
                return default;
            
            // First try the texture-specific interop function if available
            if (TextureInterop.GetGuidFromPath != null)
            {
                IntPtr guidPtr = TextureInterop.GetGuidFromPath(vfsPath);
                if (guidPtr != IntPtr.Zero)
                {
                    string? guid = Marshal.PtrToStringAnsi(guidPtr);
                    if (!string.IsNullOrWhiteSpace(guid) && guid!.Length == 32)
                    {
                        return new Texture { Guid = guid, FileID = 0 };
                    }
                }
            }

            // Fallback: use the Resources interop to resolve a VFS path
            string? resourceGuid = Resources.ResolveGuidFromPath(vfsPath);
            if (!string.IsNullOrWhiteSpace(resourceGuid) && resourceGuid!.Length == 32)
            {
                return new Texture { Guid = resourceGuid, FileID = 0 };
            }

            return default;
        }

        /// <summary>
        /// Parses the GUID string into high/low 64-bit components.
        /// </summary>
        internal bool TryParseGuid(out ulong high, out ulong low)
        {
            high = 0; low = 0;
            if (string.IsNullOrWhiteSpace(Guid) || Guid!.Length != 32)
                return false;
            try
            {
                high = Convert.ToUInt64(Guid.Substring(0, 16), 16);
                low = Convert.ToUInt64(Guid.Substring(16, 16), 16);
                return true;
            }
            catch { return false; }
        }

        /// <summary>
        /// Serializes the texture reference to a combined string format for interop.
        /// Format: "GUID:FileID" (e.g., "abc123...def456:0")
        /// </summary>
        public string ToInteropString()
        {
            if (string.IsNullOrEmpty(Guid))
                return string.Empty;
            return $"{Guid}:{FileID}";
        }

        /// <summary>
        /// Parses a texture reference from the interop string format.
        /// </summary>
        public static Texture FromInteropString(string? interopStr)
        {
            if (string.IsNullOrWhiteSpace(interopStr))
                return default;

            var parts = interopStr!.Split(':');
            if (parts.Length >= 2 && int.TryParse(parts[1], out int fileId))
            {
                return new Texture { Guid = parts[0], FileID = fileId };
            }
            // Fallback: treat entire string as GUID with fileID 0
            return new Texture { Guid = interopStr, FileID = 0 };
        }

        public override string ToString()
        {
            if (!IsValid) return "(None)";
            return $"Texture({Guid?.Substring(0, 8)}..., FileID={FileID})";
        }
    }
}
