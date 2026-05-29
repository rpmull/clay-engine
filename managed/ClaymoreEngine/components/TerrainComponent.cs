using System;
using System.Numerics;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    public class TerrainComponent : ComponentBase
    {
        public bool TryGetHeight(Vector3 worldPos, out float height)
        {
            height = 0.0f;
            if (ComponentInterop.Terrain_GetHeightAtWorld == null) return false;
            return ComponentInterop.Terrain_GetHeightAtWorld(entity.EntityID, worldPos.X, worldPos.Z, out height);
        }

        public bool TryGetNormal(Vector3 worldPos, out Vector3 normal)
        {
            normal = Vector3.UnitY;
            if (ComponentInterop.Terrain_GetNormalAtWorld == null) return false;
            if (!ComponentInterop.Terrain_GetNormalAtWorld(entity.EntityID, worldPos.X, worldPos.Z, out float x, out float y, out float z))
                return false;
            normal = new Vector3(x, y, z);
            return true;
        }

        public bool TryGetNearestPoint(Vector3 worldPos, out Vector3 point)
        {
            point = Vector3.Zero;
            if (ComponentInterop.Terrain_GetNearestPoint == null) return false;
            if (!ComponentInterop.Terrain_GetNearestPoint(entity.EntityID, worldPos.X, worldPos.Z, out float x, out float y, out float z))
                return false;
            point = new Vector3(x, y, z);
            return true;
        }

        public bool Raycast(Vector3 origin, Vector3 direction, out Vector3 hitPos, out Vector3 hitNormal)
        {
            hitPos = Vector3.Zero;
            hitNormal = Vector3.UnitY;
            if (ComponentInterop.Terrain_Raycast == null) return false;
            if (!ComponentInterop.Terrain_Raycast(entity.EntityID,
                    origin.X, origin.Y, origin.Z,
                    direction.X, direction.Y, direction.Z,
                    out float hx, out float hy, out float hz,
                    out float nx, out float ny, out float nz))
            {
                return false;
            }
            hitPos = new Vector3(hx, hy, hz);
            hitNormal = new Vector3(nx, ny, nz);
            return true;
        }

        public bool TryGetDominantLayer(Vector3 worldPos, out int layerIndex, out float weight)
        {
            layerIndex = -1;
            weight = 0.0f;
            if (ComponentInterop.Terrain_GetDominantLayerAtWorld == null) return false;
            return ComponentInterop.Terrain_GetDominantLayerAtWorld(entity.EntityID, worldPos.X, worldPos.Z, out layerIndex, out weight);
        }

        public bool SetHeight(Vector3 worldPos, float worldHeight)
        {
            if (ComponentInterop.Terrain_SetHeightAtWorld == null) return false;
            return ComponentInterop.Terrain_SetHeightAtWorld(entity.EntityID, worldPos.X, worldPos.Z, worldHeight);
        }

        public bool ApplyHeightDelta(Vector3 worldPos, float radius, float deltaHeight, float falloff = 1.0f)
        {
            if (ComponentInterop.Terrain_ApplyHeightDelta == null) return false;
            return ComponentInterop.Terrain_ApplyHeightDelta(entity.EntityID, worldPos.X, worldPos.Z, radius, deltaHeight, falloff);
        }

        public int InstancerLayerCount
        {
            get
            {
                if (ComponentInterop.Terrain_GetInstancerLayerCount == null) return 0;
                return ComponentInterop.Terrain_GetInstancerLayerCount(entity.EntityID);
            }
        }

        public string GetInstancerLayerName(int layerIndex)
        {
            if (ComponentInterop.Terrain_GetInstancerLayerNameInternal == null) return string.Empty;
            IntPtr ptr = ComponentInterop.Terrain_GetInstancerLayerNameInternal(entity.EntityID, layerIndex);
            return ptr == IntPtr.Zero ? string.Empty : (Marshal.PtrToStringAnsi(ptr) ?? string.Empty);
        }

        public bool SetInstancerLayerEnabled(int layerIndex, bool enabled)
        {
            if (ComponentInterop.Terrain_SetInstancerLayerEnabled == null) return false;
            return ComponentInterop.Terrain_SetInstancerLayerEnabled(entity.EntityID, layerIndex, enabled);
        }

        public bool SetInstancerLayerDensity(int layerIndex, float density)
        {
            if (ComponentInterop.Terrain_SetInstancerLayerDensity == null) return false;
            return ComponentInterop.Terrain_SetInstancerLayerDensity(entity.EntityID, layerIndex, density);
        }

        public bool RegenerateInstancers()
        {
            if (ComponentInterop.Terrain_RegenerateInstancers == null) return false;
            return ComponentInterop.Terrain_RegenerateInstancers(entity.EntityID);
        }
    }
}
