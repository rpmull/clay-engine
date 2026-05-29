using System;
using System.Numerics;
using System.Runtime.InteropServices;

namespace ClaymoreEngine.Physics
{
    // Delegate type for interop initialization
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public unsafe delegate void RagdollInteropInitDelegate(void** ptrs, int count);

    /// <summary>
    /// Ragdoll physics - creates physics bodies for skeleton bones
    /// allowing characters to react to physics on death or impact.
    /// </summary>
    public static class Ragdoll
    {
        // Native function pointers (set during interop initialization)
        internal static Ragdoll_Create_Delegate? Create_Native;
        internal static Ragdoll_Destroy_Delegate? Destroy_Native;
        internal static Ragdoll_Has_Delegate? Has_Native;
        internal static Ragdoll_Activate_Delegate? Activate_Native;
        internal static Ragdoll_Deactivate_Delegate? Deactivate_Native;
        internal static Ragdoll_ApplyImpulse_Delegate? ApplyImpulse_Native;
        internal static Ragdoll_ApplyImpulseToAll_Delegate? ApplyImpulseToAll_Native;
        internal static Ragdoll_SetPhysicsLayer_Delegate? SetPhysicsLayer_Native;
        internal static Ragdoll_GetOwnerFromBone_Delegate? GetOwnerFromBone_Native;
        
        // Delegate types
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate bool Ragdoll_Create_Delegate(int entityId, [MarshalAs(UnmanagedType.I1)] bool includeFingersAndToes, uint physicsLayer);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate void Ragdoll_Destroy_Delegate(int entityId);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate bool Ragdoll_Has_Delegate(int entityId);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate void Ragdoll_Activate_Delegate(int entityId);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate void Ragdoll_Deactivate_Delegate(int entityId);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate void Ragdoll_ApplyImpulse_Delegate(int entityId, int boneIndex, float x, float y, float z);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate void Ragdoll_ApplyImpulseToAll_Delegate(int entityId, float x, float y, float z);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate void Ragdoll_SetPhysicsLayer_Delegate(int entityId, uint layer);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate int Ragdoll_GetOwnerFromBone_Delegate(int boneEntityId);
        
        /// <summary>
        /// Create a ragdoll for an entity with a skeleton.
        /// Physics bodies are created for each bone in parallel for performance.
        /// </summary>
        /// <param name="entity">Entity with a skeleton component (or child with skeleton)</param>
        /// <param name="includeFingersAndToes">If false, skips small bones for better performance</param>
        /// <param name="physicsLayer">Physics layer for ragdoll bones (default: 1)</param>
        /// <returns>True if ragdoll was created successfully</returns>
        public static bool Create(Entity entity, bool includeFingersAndToes = false, uint physicsLayer = 1)
        {
            if (!entity.IsValid) return false;
            return Create_Native?.Invoke(entity.EntityID, includeFingersAndToes, physicsLayer) ?? false;
        }
        
        /// <summary>
        /// Destroy the ragdoll for an entity, removing all physics bodies.
        /// </summary>
        public static void Destroy(Entity entity)
        {
            if (!entity.IsValid) return;
            Destroy_Native?.Invoke(entity.EntityID);
        }
        
        /// <summary>
        /// Check if an entity has a ragdoll.
        /// </summary>
        public static bool Has(Entity entity)
        {
            if (!entity.IsValid) return false;
            return Has_Native?.Invoke(entity.EntityID) ?? false;
        }
        
        /// <summary>
        /// Activate ragdoll physics - bones will respond to physics forces.
        /// You should also disable animation when activating ragdoll.
        /// </summary>
        public static void Activate(Entity entity)
        {
            if (!entity.IsValid) return;
            Activate_Native?.Invoke(entity.EntityID);
        }
        
        /// <summary>
        /// Deactivate ragdoll - makes bone bodies kinematic.
        /// </summary>
        public static void Deactivate(Entity entity)
        {
            if (!entity.IsValid) return;
            Deactivate_Native?.Invoke(entity.EntityID);
        }
        
        /// <summary>
        /// Apply an impulse to a specific bone.
        /// </summary>
        /// <param name="entity">Entity with ragdoll</param>
        /// <param name="boneIndex">Index of the bone to apply impulse to</param>
        /// <param name="impulse">Impulse vector (world space)</param>
        public static void ApplyImpulse(Entity entity, int boneIndex, Vector3 impulse)
        {
            if (!entity.IsValid) return;
            ApplyImpulse_Native?.Invoke(entity.EntityID, boneIndex, impulse.X, impulse.Y, impulse.Z);
        }
        
        /// <summary>
        /// Apply an impulse to all bones (useful for explosions, etc.)
        /// </summary>
        /// <param name="entity">Entity with ragdoll</param>
        /// <param name="impulse">Impulse vector (world space)</param>
        public static void ApplyImpulseToAll(Entity entity, Vector3 impulse)
        {
            if (!entity.IsValid) return;
            ApplyImpulseToAll_Native?.Invoke(entity.EntityID, impulse.X, impulse.Y, impulse.Z);
        }
        
        /// <summary>
        /// Set physics layer for all bones in a ragdoll.
        /// This updates the collision layer for all ragdoll bone bodies.
        /// </summary>
        /// <param name="entity">Entity with ragdoll</param>
        /// <param name="layer">Physics layer index (0-31)</param>
        public static void SetPhysicsLayer(Entity entity, uint layer)
        {
            if (!entity.IsValid) return;
            SetPhysicsLayer_Native?.Invoke(entity.EntityID, layer);
        }
        
        /// <summary>
        /// Find the ragdoll owner entity from a bone entity.
        /// When a raycast hits a ragdoll bone, use this to find the NPC/character entity that owns the ragdoll.
        /// </summary>
        /// <param name="boneEntity">Bone entity that was hit by raycast</param>
        /// <returns>Owner entity ID, or invalid entity if not found</returns>
        public static Entity GetOwnerFromBone(Entity boneEntity)
        {
            if (!boneEntity.IsValid) return new Entity(-1);
            int ownerId = GetOwnerFromBone_Native?.Invoke(boneEntity.EntityID) ?? -1;
            return ownerId >= 0 ? new Entity(ownerId) : new Entity(-1);
        }
        
        /// <summary>
        /// Initialize ragdoll interop from native function pointers.
        /// Called during engine startup.
        /// </summary>
        public static unsafe void Initialize(void** ptrs, int count)
        {
            if (count < 9) return;
            
            int i = 0;
            Create_Native = Marshal.GetDelegateForFunctionPointer<Ragdoll_Create_Delegate>((IntPtr)ptrs[i++]);
            Destroy_Native = Marshal.GetDelegateForFunctionPointer<Ragdoll_Destroy_Delegate>((IntPtr)ptrs[i++]);
            Has_Native = Marshal.GetDelegateForFunctionPointer<Ragdoll_Has_Delegate>((IntPtr)ptrs[i++]);
            Activate_Native = Marshal.GetDelegateForFunctionPointer<Ragdoll_Activate_Delegate>((IntPtr)ptrs[i++]);
            Deactivate_Native = Marshal.GetDelegateForFunctionPointer<Ragdoll_Deactivate_Delegate>((IntPtr)ptrs[i++]);
            ApplyImpulse_Native = Marshal.GetDelegateForFunctionPointer<Ragdoll_ApplyImpulse_Delegate>((IntPtr)ptrs[i++]);
            ApplyImpulseToAll_Native = Marshal.GetDelegateForFunctionPointer<Ragdoll_ApplyImpulseToAll_Delegate>((IntPtr)ptrs[i++]);
            SetPhysicsLayer_Native = Marshal.GetDelegateForFunctionPointer<Ragdoll_SetPhysicsLayer_Delegate>((IntPtr)ptrs[i++]);
            GetOwnerFromBone_Native = Marshal.GetDelegateForFunctionPointer<Ragdoll_GetOwnerFromBone_Delegate>((IntPtr)ptrs[i++]);
            
            Console.WriteLine("[Ragdoll] Interop initialized");
        }
    }
}

