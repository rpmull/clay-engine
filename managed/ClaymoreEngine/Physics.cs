using System;
using System.Numerics;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    /// <summary>
    /// Provides managed access to global physics parameters such as gravity.
    /// </summary>
    public static class PhysicsGlobals
    {
        // Delegate types for function pointer interop
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate void Physics_SetGravityFn(float x, float y, float z);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate void Physics_GetGravityFn(out float x, out float y, out float z);

        internal static Physics_SetGravityFn? _setGravity;
        internal static Physics_GetGravityFn? _getGravity;

        /// <summary>
        /// Gets or sets the world gravity vector in meters per second squared.
        /// </summary>
        public static Vector3 Gravity
        {
            get
            {
                if (_getGravity == null) return new Vector3(0, -9.81f, 0);
                _getGravity(out float x, out float y, out float z);
                return new Vector3(x, y, z);
            }
            set
            {
                _setGravity?.Invoke(value.X, value.Y, value.Z);
            }
        }

        /// <summary>
        /// Convenience helper for setting gravity without touching the property.
        /// </summary>
        public static void SetGravity(Vector3 gravity) => Gravity = gravity;

        /// <summary>
        /// Convenience helper for retrieving gravity without touching the property.
        /// </summary>
        public static Vector3 GetGravity() => Gravity;
    }
}

namespace ClaymoreEngine.Physics
{
    // Delegate for interop initialization from native
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public unsafe delegate void PhysicsInteropInitDelegate(IntPtr* functionPointers, int count);

    /// <summary>
    /// Physics layer management. Layers are user-defined strings mapped to bit indices.
    /// Use LayerMask methods to query and combine layer masks.
    /// </summary>
    public static class PhysicsLayer
    {
        // Delegate types for layer management
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate uint RegisterLayerFn([MarshalAs(UnmanagedType.LPStr)] string name);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate int GetLayerIndexFn([MarshalAs(UnmanagedType.LPStr)] string name);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate uint GetLayerMaskFn([MarshalAs(UnmanagedType.LPStr)] string name);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate uint GetLayerCountFn();

        internal static RegisterLayerFn? _registerLayer;
        internal static GetLayerIndexFn? _getLayerIndex;
        internal static GetLayerMaskFn? _getLayerMask;
        internal static GetLayerCountFn? _getLayerCount;

        /// <summary>
        /// Register a new physics layer by name. Returns the layer index (0-31).
        /// If the layer already exists, returns its existing index.
        /// </summary>
        public static uint Register(string name)
        {
            if (_registerLayer == null)
            {
                Console.WriteLine("[PhysicsLayer] Interop not initialized");
                return 0;
            }
            return _registerLayer(name);
        }

        /// <summary>
        /// Get the index of a layer by name. Returns -1 if not found.
        /// </summary>
        public static int GetIndex(string name)
        {
            if (_getLayerIndex == null) return -1;
            return _getLayerIndex(name);
        }

        /// <summary>
        /// Get the bitmask for a single layer by name. Returns 0 if not found.
        /// </summary>
        public static uint GetMask(string name)
        {
            if (_getLayerMask == null) return 0;
            return _getLayerMask(name);
        }

        /// <summary>
        /// Get the number of registered layers.
        /// </summary>
        public static uint GetCount()
        {
            if (_getLayerCount == null) return 0;
            return _getLayerCount();
        }

        /// <summary>
        /// Gets a layer mask from a layer index.
        /// </summary>
        public static uint IndexToMask(uint layerIndex) => 1u << (int)layerIndex;
        
        /// <summary>
        /// Combines multiple layer names into a single mask.
        /// </summary>
        public static uint GetMask(params string[] layerNames)
        {
            uint mask = 0;
            foreach (var name in layerNames)
                mask |= GetMask(name);
            return mask;
        }
    }

    /// <summary>
    /// Helper class for working with layer masks.
    /// </summary>
    public static class LayerMask
    {
        /// <summary>All layers.</summary>
        public const uint All = 0xFFFFFFFF;
        
        /// <summary>No layers.</summary>
        public const uint None = 0;

        /// <summary>
        /// Get mask for a single layer by name.
        /// </summary>
        public static uint Get(string layerName) => PhysicsLayer.GetMask(layerName);

        /// <summary>
        /// Get combined mask for multiple layers by name.
        /// </summary>
        public static uint Get(params string[] layerNames) => PhysicsLayer.GetMask(layerNames);

        /// <summary>
        /// Get mask that includes all layers EXCEPT the specified ones.
        /// </summary>
        public static uint AllExcept(params string[] excludeLayerNames)
        {
            return All & ~PhysicsLayer.GetMask(excludeLayerNames);
        }

        /// <summary>
        /// Check if a mask includes a specific layer.
        /// </summary>
        public static bool Includes(uint mask, string layerName)
        {
            return (mask & PhysicsLayer.GetMask(layerName)) != 0;
        }

        /// <summary>
        /// Add a layer to a mask.
        /// </summary>
        public static uint Add(uint mask, string layerName)
        {
            return mask | PhysicsLayer.GetMask(layerName);
        }

        /// <summary>
        /// Remove a layer from a mask.
        /// </summary>
        public static uint Remove(uint mask, string layerName)
        {
            return mask & ~PhysicsLayer.GetMask(layerName);
        }
    }

    /// <summary>
    /// Contains information about a raycast hit.
    /// </summary>
    public struct RaycastHit
    {
        /// <summary>
        /// The point in world space where the ray hit the collider.
        /// </summary>
        public Vector3 Point;

        /// <summary>
        /// The normal of the surface the ray hit.
        /// </summary>
        public Vector3 Normal;

        /// <summary>
        /// The distance from the ray origin to the hit point.
        /// </summary>
        public float Distance;

        /// <summary>
        /// The entity ID of the hit object, or 0 if no entity is associated.
        /// </summary>
        public int EntityId;

        /// <summary>
        /// Gets the Entity that was hit. Returns an invalid entity if EntityId is 0.
        /// </summary>
        public Entity Entity => new Entity(EntityId);
    }

    // Delegate types for function pointer interop (with layer mask)
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate bool Physics_RaycastFn(
        float originX, float originY, float originZ,
        float dirX, float dirY, float dirZ,
        float maxDistance,
        uint layerMask,
        out float hitPointX, out float hitPointY, out float hitPointZ,
        out float hitNormalX, out float hitNormalY, out float hitNormalZ,
        out float hitDistance,
        out int hitEntityId);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate bool Physics_RaycastPointsFn(
        float fromX, float fromY, float fromZ,
        float toX, float toY, float toZ,
        uint layerMask,
        out float hitPointX, out float hitPointY, out float hitPointZ,
        out float hitNormalX, out float hitNormalY, out float hitNormalZ,
        out float hitDistance,
        out int hitEntityId);

    /// <summary>
    /// Provides physics raycast functionality.
    /// </summary>
    public static unsafe class Physics
    {
        private static Physics_RaycastFn? _raycast;
        private static Physics_RaycastPointsFn? _raycastPoints;
        private static bool _initialized = false;
        
        // Expected number of function pointers (must match native kPhysicsInteropCount)
        private const int kExpectedCount = 8;

        /// <summary>
        /// Initialize physics interop from native function pointers.
        /// Called from native during startup.
        /// </summary>
        public static void InitializeInterop(IntPtr* ptrs, int count)
        {
            if (ptrs == null || count < kExpectedCount)
            {
                Console.WriteLine($"[PhysicsInterop] Invalid pointer table or insufficient count (got {count}, need {kExpectedCount}).");
                return;
            }

            int i = 0;
            // Gravity functions (0-1)
            PhysicsGlobals._setGravity = Marshal.GetDelegateForFunctionPointer<PhysicsGlobals.Physics_SetGravityFn>(ptrs[i++]);
            PhysicsGlobals._getGravity = Marshal.GetDelegateForFunctionPointer<PhysicsGlobals.Physics_GetGravityFn>(ptrs[i++]);
            // Raycast functions (2-3)
            _raycast = Marshal.GetDelegateForFunctionPointer<Physics_RaycastFn>(ptrs[i++]);
            _raycastPoints = Marshal.GetDelegateForFunctionPointer<Physics_RaycastPointsFn>(ptrs[i++]);
            // Layer management functions (4-7)
            PhysicsLayer._registerLayer = Marshal.GetDelegateForFunctionPointer<PhysicsLayer.RegisterLayerFn>(ptrs[i++]);
            PhysicsLayer._getLayerIndex = Marshal.GetDelegateForFunctionPointer<PhysicsLayer.GetLayerIndexFn>(ptrs[i++]);
            PhysicsLayer._getLayerMask = Marshal.GetDelegateForFunctionPointer<PhysicsLayer.GetLayerMaskFn>(ptrs[i++]);
            PhysicsLayer._getLayerCount = Marshal.GetDelegateForFunctionPointer<PhysicsLayer.GetLayerCountFn>(ptrs[i++]);
            
            _initialized = true;
            Console.WriteLine($"[PhysicsInterop] Physics delegates initialized ({count} functions, {PhysicsLayer.GetCount()} layers).");
        }

        /// <summary>
        /// Casts a ray from origin along direction up to maxDistance.
        /// </summary>
        /// <param name="origin">The starting point of the ray in world space.</param>
        /// <param name="direction">The direction of the ray (will be normalized internally).</param>
        /// <param name="maxDistance">The maximum distance the ray should travel.</param>
        /// <param name="hit">If true is returned, contains information about the hit.</param>
        /// <param name="layerMask">Bitmask of layers to include. Use LayerMask constants.</param>
        /// <returns>True if the ray hit something, false otherwise.</returns>
        public static bool Raycast(Vector3 origin, Vector3 direction, float maxDistance, out RaycastHit hit, uint layerMask = LayerMask.All)
        {
            hit = default;
            
            if (!_initialized || _raycast == null)
            {
                Console.WriteLine("[Physics] Raycast not initialized - native interop not set up.");
                return false;
            }

            bool result = _raycast(
                origin.X, origin.Y, origin.Z,
                direction.X, direction.Y, direction.Z,
                maxDistance,
                layerMask,
                out float px, out float py, out float pz,
                out float nx, out float ny, out float nz,
                out float dist,
                out int entityId);

            hit = new RaycastHit
            {
                Point = new Vector3(px, py, pz),
                Normal = new Vector3(nx, ny, nz),
                Distance = dist,
                EntityId = entityId
            };

            return result;
        }

        /// <summary>
        /// Casts a ray between two points (line segment cast).
        /// </summary>
        /// <param name="from">The starting point in world space.</param>
        /// <param name="to">The end point in world space.</param>
        /// <param name="hit">If true is returned, contains information about the hit.</param>
        /// <param name="layerMask">Bitmask of layers to include. Use LayerMask constants.</param>
        /// <returns>True if the ray hit something between the two points, false otherwise.</returns>
        public static bool Linecast(Vector3 from, Vector3 to, out RaycastHit hit, uint layerMask = LayerMask.All)
        {
            hit = default;
            
            if (!_initialized || _raycastPoints == null)
            {
                Console.WriteLine("[Physics] Linecast not initialized - native interop not set up.");
                return false;
            }

            bool result = _raycastPoints(
                from.X, from.Y, from.Z,
                to.X, to.Y, to.Z,
                layerMask,
                out float px, out float py, out float pz,
                out float nx, out float ny, out float nz,
                out float dist,
                out int entityId);

            hit = new RaycastHit
            {
                Point = new Vector3(px, py, pz),
                Normal = new Vector3(nx, ny, nz),
                Distance = dist,
                EntityId = entityId
            };

            return result;
        }
    }
}





