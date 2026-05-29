using System;
using System.Numerics;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    /// <summary>
    /// Component for rendering 3D meshes. Provides access to mesh data, 
    /// rendering properties, and the ability to swap meshes at runtime.
    /// </summary>
    public class MeshComponent : ComponentBase
    {
        /// <summary>
        /// Gets or sets the mesh asset reference. Setting this will load and display a new mesh.
        /// </summary>
        public Mesh MeshReference
        {
            get
            {
                if (MeshInterop.GetReference == null) return default;
                MeshInterop.GetReference(entity.EntityID, out ulong hi, out ulong lo, out int fileID);
                if (hi == 0 && lo == 0) return default;
                
                // Convert to hex GUID string
                string guid = hi.ToString("x16") + lo.ToString("x16");
                return new Mesh { Guid = guid, FileID = fileID };
            }
            set
            {
                if (!value.IsValid) return;
                value.SetOnEntity(entity);
            }
        }

        /// <summary>
        /// The display name of the mesh (read-only).
        /// </summary>
        public string Name
        {
            get
            {
                if (MeshInterop.GetName == null) return string.Empty;
                IntPtr ptr = MeshInterop.GetName(entity.EntityID);
                return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) ?? string.Empty : string.Empty;
            }
        }

        /// <summary>
        /// Number of vertices in the mesh (read-only).
        /// </summary>
        public int VertexCount => MeshInterop.GetVertexCount?.Invoke(entity.EntityID) ?? 0;

        /// <summary>
        /// Number of indices in the mesh (read-only).
        /// </summary>
        public int IndexCount => MeshInterop.GetIndexCount?.Invoke(entity.EntityID) ?? 0;

        /// <summary>
        /// Number of submeshes/material slots (read-only).
        /// </summary>
        public int SubmeshCount => MeshInterop.GetSubmeshCount?.Invoke(entity.EntityID) ?? 0;

        /// <summary>
        /// Number of material slots (read-only).
        /// </summary>
        public int MaterialSlotCount => MeshInterop.GetMaterialSlotCount?.Invoke(entity.EntityID) ?? SubmeshCount;

        public readonly struct MaterialSlotInfo
        {
            public MaterialSlotInfo(int index, string name)
            {
                Index = index;
                Name = name ?? string.Empty;
            }

            public int Index { get; }
            public string Name { get; }
            public string name => Name;
        }

        /// <summary>
        /// Managed view of mesh material slots with authored slot names.
        /// </summary>
        public MaterialSlotInfo[] MaterialSlots
        {
            get
            {
                int count = MaterialSlotCount;
                if (count <= 0) return Array.Empty<MaterialSlotInfo>();

                var result = new MaterialSlotInfo[count];
                for (int i = 0; i < count; ++i)
                {
                    result[i] = new MaterialSlotInfo(i, GetMaterialSlotName(i));
                }
                return result;
            }
        }

        /// <summary>
        /// Managed material wrappers for each material slot.
        /// </summary>
        public Material[] Materials
        {
            get
            {
                int count = MaterialSlotCount;
                if (count <= 0) return Array.Empty<Material>();

                var result = new Material[count];
                for (int i = 0; i < count; ++i)
                {
                    result[i] = new Material(entity, i);
                }
                return result;
            }
        }

        // Unity-style aliases.
        public Material[] materials => Materials;
        public MaterialSlotInfo[] materialSlots => MaterialSlots;

        /// <summary>
        /// Convenience wrapper for slot 0.
        /// </summary>
        public Material Material => GetMaterial(0);

        public Material material => Material;

        public Material GetMaterial(int slotIndex)
        {
            return new Material(entity, slotIndex);
        }

        public string GetMaterialSlotName(int slotIndex)
        {
            if (slotIndex < 0) return string.Empty;
            if (MeshInterop.GetMaterialSlotName == null) return $"Slot {slotIndex}";
            IntPtr ptr = MeshInterop.GetMaterialSlotName(entity.EntityID, slotIndex);
            if (ptr == IntPtr.Zero) return $"Slot {slotIndex}";
            string name = Marshal.PtrToStringAnsi(ptr) ?? string.Empty;
            return string.IsNullOrWhiteSpace(name) ? $"Slot {slotIndex}" : name;
        }

        /// <summary>
        /// Returns the first slot index whose name contains the provided text (case-insensitive).
        /// Returns -1 when no slot matches.
        /// </summary>
        public int FindMaterialSlotIndexByNameContains(string text)
        {
            if (string.IsNullOrWhiteSpace(text)) return -1;
            int count = MaterialSlotCount;
            for (int i = 0; i < count; ++i)
            {
                string slotName = GetMaterialSlotName(i);
                if (slotName.IndexOf(text, StringComparison.OrdinalIgnoreCase) >= 0)
                {
                    return i;
                }
            }
            return -1;
        }

        /// <summary>
        /// Returns all slot indices whose names contain the provided text (case-insensitive).
        /// </summary>
        public int[] FindMaterialSlotIndicesByNameContains(string text)
        {
            if (string.IsNullOrWhiteSpace(text)) return Array.Empty<int>();
            int count = MaterialSlotCount;
            var matches = new System.Collections.Generic.List<int>(Math.Min(count, 4));
            for (int i = 0; i < count; ++i)
            {
                string slotName = GetMaterialSlotName(i);
                if (slotName.IndexOf(text, StringComparison.OrdinalIgnoreCase) >= 0)
                {
                    matches.Add(i);
                }
            }
            return matches.Count == 0 ? Array.Empty<int>() : matches.ToArray();
        }

        /// <summary>
        /// Minimum corner of the mesh's axis-aligned bounding box (read-only).
        /// </summary>
        public Vector3 BoundsMin
        {
            get
            {
                if (MeshInterop.GetBoundsMin == null) return Vector3.Zero;
                MeshInterop.GetBoundsMin(entity.EntityID, out float x, out float y, out float z);
                return new Vector3(x, y, z);
            }
        }

        /// <summary>
        /// Maximum corner of the mesh's axis-aligned bounding box (read-only).
        /// </summary>
        public Vector3 BoundsMax
        {
            get
            {
                if (MeshInterop.GetBoundsMax == null) return Vector3.Zero;
                MeshInterop.GetBoundsMax(entity.EntityID, out float x, out float y, out float z);
                return new Vector3(x, y, z);
            }
        }

        /// <summary>
        /// Center of the mesh's bounding box (read-only).
        /// </summary>
        public Vector3 BoundsCenter => (BoundsMin + BoundsMax) * 0.5f;

        /// <summary>
        /// Size of the mesh's bounding box (read-only).
        /// </summary>
        public Vector3 BoundsSize => BoundsMax - BoundsMin;

        /// <summary>
        /// Whether this mesh has skinning/skeletal animation data (read-only).
        /// </summary>
        public bool HasSkinning => MeshInterop.HasSkinning?.Invoke(entity.EntityID) ?? false;

        /// <summary>
        /// Multiplier applied to the mesh AABB during frustum culling.
        /// Increase this for animated/skinned meshes whose authored bounds are too tight.
        /// </summary>
        public float BoundsPadding
        {
            get => MeshInterop.GetBoundsPadding?.Invoke(entity.EntityID) ?? 1.0f;
            set => MeshInterop.SetBoundsPadding?.Invoke(entity.EntityID, value);
        }

        /// <summary>
        /// When true, renders this mesh on top of others (disables Z-write).
        /// Useful for weapons, UI elements, or effects that should always be visible.
        /// </summary>
        public bool RenderOnTop
        {
            get => MeshInterop.GetRenderOnTop?.Invoke(entity.EntityID) ?? false;
            set => MeshInterop.SetRenderOnTop?.Invoke(entity.EntityID, value);
        }

        /// <summary>
        /// When true, disables backface culling to show both sides of polygons.
        /// Useful for flat/paper objects, foliage, or cloth.
        /// </summary>
        public bool ShowBackfaces
        {
            get => MeshInterop.GetShowBackfaces?.Invoke(entity.EntityID) ?? false;
            set => MeshInterop.SetShowBackfaces?.Invoke(entity.EntityID, value);
        }

        /// <summary>
        /// When true, bypasses CPU frustum culling for this mesh.
        /// Useful for first-person meshes or authored cases whose animated bounds are unreliable.
        /// </summary>
        public bool SkipFrustumCulling
        {
            get => MeshInterop.GetSkipFrustumCulling?.Invoke(entity.EntityID) ?? false;
            set => MeshInterop.SetSkipFrustumCulling?.Invoke(entity.EntityID, value);
        }

        /// <summary>
        /// Render order for sorting within the same render pass.
        /// Higher values render later (on top).
        /// </summary>
        public int RenderOrder
        {
            get => MeshInterop.GetRenderOrder?.Invoke(entity.EntityID) ?? 0;
            set => MeshInterop.SetRenderOrder?.Invoke(entity.EntityID, value);
        }

        /// <summary>
        /// When true, this mesh has a unique cloned material instance
        /// that can be modified without affecting other meshes using the same material.
        /// </summary>
        public bool UniqueMaterial
        {
            get => MeshInterop.GetUniqueMaterial?.Invoke(entity.EntityID) ?? false;
            set => MeshInterop.SetUniqueMaterial?.Invoke(entity.EntityID, value);
        }

        /// <summary>
        /// Sets a new mesh on this component from a Mesh asset reference.
        /// </summary>
        /// <param name="mesh">The mesh asset to display</param>
        /// <returns>True if the mesh was successfully set</returns>
        public bool SetMesh(Mesh mesh)
        {
            return mesh.SetOnEntity(entity);
        }
    }
}
