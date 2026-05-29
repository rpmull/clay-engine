using System;
using System.Numerics;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    /// <summary>
    /// Reference to a Mesh asset. Use this in [SerializeField] fields to allow
    /// dragging mesh files (FBX, GLB, etc.) or specific submeshes from the Project panel.
    /// 
    /// At runtime, the reference resolves to the mesh's binary data (.meshbin).
    /// </summary>
    public struct Mesh
    {
        /// <summary>
        /// 32-character hex GUID of the mesh asset (points to .meta file).
        /// </summary>
        public string? Guid;

        /// <summary>
        /// File ID for submesh selection within a multi-mesh model.
        /// Use 0 for the first/default mesh, or a specific ID for armor/equipment pieces.
        /// </summary>
        public int FileID;

        /// <summary>
        /// Returns true if this Mesh reference has a valid GUID assigned.
        /// Use this instead of == null checks since Mesh is a struct.
        /// </summary>
        public bool IsValid => !string.IsNullOrWhiteSpace(Guid) && Guid!.Length == 32;

        /// <summary>
        /// Gets the name of the mesh asset that would be instantiated.
        /// This is the name of the entity that would be created when calling Instantiate().
        /// </summary>
        public string name
        {
            get
            {
                if (!IsValid || !TryParseGuid(out ulong hi, out ulong lo))
                    return string.Empty;
                
                if (MeshInterop.GetAssetNameByGuid == null)
                    return string.Empty;
                
                IntPtr ptr = MeshInterop.GetAssetNameByGuid(hi, lo);
                return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) ?? string.Empty : string.Empty;
            }
        }

        /// <summary>
        /// Creates a Mesh reference from a GUID string.
        /// </summary>
        public static Mesh FromGuid(string guid, int fileId = 0)
        {
            return new Mesh { Guid = guid, FileID = fileId };
        }

        /// <summary>
        /// Instantiate this mesh as a new entity in the scene.
        /// </summary>
        /// <param name="entityName">Optional name for the created entity</param>
        /// <param name="position">Optional world position for the entity</param>
        /// <returns>The newly created entity, or an invalid entity if instantiation failed</returns>
        public Entity Instantiate(string? entityName = null, Vector3? position = null)
        {
            if (!TryParseGuid(out ulong hi, out ulong lo))
                return new Entity(-1);
            
            if (MeshInterop.Instantiate == null)
                return new Entity(-1);
            
            int id = MeshInterop.Instantiate(hi, lo, FileID, entityName);
            
            if (id <= 0) return new Entity(-1);
            
            var entity = new Entity(id);
            if (position.HasValue)
                EntityInterop.SetPosition(id, position.Value);
            
            return entity;
        }

        /// <summary>
        /// Instantiate this mesh as a new entity and parent it to another entity.
        /// Useful for attaching armor/equipment to skeleton bones.
        /// </summary>
        /// <param name="parent">The parent entity (e.g., a skeleton bone)</param>
        /// <param name="entityName">Optional name for the created entity</param>
        /// <param name="localPosition">Optional local position relative to parent</param>
        /// <param name="useParentAsModelRoot">If true, reuse the parent as the model root</param>
        /// <returns>The newly created entity, or an invalid entity if instantiation failed</returns>
        public Entity Instantiate(Entity parent, string? entityName = null, Vector3? localPosition = null, bool useParentAsModelRoot = false)
        {
            if (!TryParseGuid(out ulong hi, out ulong lo))
                return new Entity(-1);

            bool reuseRoot = useParentAsModelRoot && parent.IsValid && MeshInterop.InstantiateWithRoot != null;
            int id = reuseRoot
                ? MeshInterop.InstantiateWithRoot!(hi, lo, FileID, entityName, parent.EntityID, true)
                : (MeshInterop.Instantiate != null ? MeshInterop.Instantiate(hi, lo, FileID, entityName) : -1);
            
            if (id <= 0) return new Entity(-1);
            
            var entity = new Entity(id);
            
            // Set parent first (skip if we reused the parent as the model root)
            if (parent.IsValid && !reuseRoot)
                entity.SetParent(parent);
            
            // Then set local position if provided
            if (localPosition.HasValue)
                EntityInterop.SetLocalPosition(id, localPosition.Value);
            
            return entity;
        }

        /// <summary>
        /// Set this mesh on an existing entity, creating or replacing its MeshComponent.
        /// </summary>
        /// <param name="entity">The entity to set the mesh on</param>
        /// <returns>True if the mesh was successfully set</returns>
        public bool SetOnEntity(Entity entity)
        {
            if (!entity.IsValid || !TryParseGuid(out ulong hi, out ulong lo))
                return false;
            
            return MeshInterop.SetMesh != null && MeshInterop.SetMesh(entity.EntityID, hi, lo, FileID);
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
        /// Serializes the mesh reference to a combined string format for interop.
        /// Format: "GUID:FileID" (e.g., "abc123...def456:3")
        /// </summary>
        public string ToInteropString()
        {
            if (string.IsNullOrEmpty(Guid))
                return string.Empty;
            return $"{Guid}:{FileID}";
        }

        /// <summary>
        /// Parses a mesh reference from the interop string format.
        /// </summary>
        public static Mesh FromInteropString(string? interopStr)
        {
            if (string.IsNullOrWhiteSpace(interopStr))
                return default;

            var parts = interopStr!.Split(':');
            if (parts.Length >= 2 && int.TryParse(parts[1], out int fileId))
            {
                return new Mesh { Guid = parts[0], FileID = fileId };
            }
            // Fallback: treat entire string as GUID with fileID 0
            return new Mesh { Guid = interopStr, FileID = 0 };
        }

        public override string ToString()
        {
            if (!IsValid) return "(None)";
            return $"Mesh({Guid?.Substring(0, 8)}..., FileID={FileID})";
        }
    }
}
