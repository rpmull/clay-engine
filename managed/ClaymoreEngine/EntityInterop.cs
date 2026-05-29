using System;
using System.Collections.Generic;
using System.Numerics;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public unsafe delegate void EntityInteropInitDelegate(IntPtr* functionPointers, int count);

    // -----------------------------------------------------------------------------
    // Managed side wrapper around native EntityInterop functions.
    // The order of delegates MUST match the order of function pointers passed from
    // the native side (see DotNetHost.cpp::SetupEntityInterop).
    // -----------------------------------------------------------------------------
    /// <summary>
    /// Managed bindings to native entity functions. The pointer layout is order-sensitive.
    /// Do not reorder delegates without updating both managed and native sides.
    /// </summary>
    public static unsafe class EntityInterop
    {
        private const int EntityCoreCount = 35;   // core entity and transform/physics + vis/presentation/active + local pos + parenting + IsSceneBeingDestroyed + GetEntityName + DuplicateEntity + GetUIMousePosition
        private const int TweenCount      = 7;    // tween block preceding components
        private const int ComponentInteropCount = ComponentInterop.InteropCount;
        private const int SceneInteropCount = 7;  // LoadScene, UnloadScene, LoadSceneEx, LoadProgress, IsLoading, IsLoaded, GetCurrentScenePath

        // ---------------------- Core Transform ----------------------
        // World position (Unity-style: transform.position = world position)
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void GetEntityWorldPositionFn(int entityID, out float x, out float y, out float z);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void SetEntityWorldPositionFn(int entityID, float x, float y, float z);
        // Local position (direct Transform.Position)
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void GetEntityLocalPositionFn(int entityID, out float x, out float y, out float z);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void SetEntityLocalPositionFn(int entityID, float x, float y, float z);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int  FindEntityByNameFn([MarshalAs(UnmanagedType.LPStr)] string name);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr GetEntityNameFn(int entityID);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr  GetEntitiesFn();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int  GetEntityCountFn();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int  DuplicateEntityFn(int entityID);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool GetUIMousePositionFn(out float x, out float y);

      // Entity management
      [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int  CreateEntityFn([MarshalAs(UnmanagedType.LPStr)] string name);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void DestroyEntityFn(int entityID);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int  GetEntityByIDFn(int entityID);

        // Rotation / Scale
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void GetRotationFn(int entityID, out float x, out float y, out float z);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void SetRotationFn(int entityID, float x, float y, float z);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void GetRotationQuatFn(int entityID, out float x, out float y, out float z, out float w);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void SetRotationQuatFn(int entityID, float x, float y, float z, float w);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void GetScaleFn(int entityID, out float x, out float y, out float z);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void SetScaleFn(int entityID, float x, float y, float z);

        // Physics
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void SetLinearVelocityFn(int entityID, float x, float y, float z);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void SetAngularVelocityFn(int entityID, float x, float y, float z);

        // Visibility/Active
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void SetEntityVisibleFn(int entityID, [MarshalAs(UnmanagedType.I1)] bool visible);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool GetEntityVisibleFn(int entityID);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void SetEntityPresentationHiddenFn(int entityID, [MarshalAs(UnmanagedType.I1)] bool hidden);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool GetEntityPresentationHiddenFn(int entityID);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void SetEntityActiveFn(int entityID, [MarshalAs(UnmanagedType.I1)] bool active);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool GetEntityActiveFn(int entityID);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool IsSceneBeingDestroyedFn();

        // Parenting
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void SetEntityParentFn(int childID, int parentID, [MarshalAs(UnmanagedType.I1)] bool preserveWorldTransform);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int GetEntityParentFn(int entityID);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr GetEntityChildrenFn(int entityID);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int GetEntityChildCountFn(int entityID);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int FindChildByNameFn(int parentID, [MarshalAs(UnmanagedType.LPStr)] string name);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int FindDescendantByNameFn(int rootID, [MarshalAs(UnmanagedType.LPStr)] string name);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int CreateEntityWithParentFn([MarshalAs(UnmanagedType.LPStr)] string name, int parentID);

        // ---------------------- Delegate instances ----------------------
        public static GetEntityWorldPositionFn  GetEntityWorldPosition;
        public static SetEntityWorldPositionFn  SetEntityWorldPosition;
        public static GetEntityLocalPositionFn GetEntityLocalPosition;
        public static SetEntityLocalPositionFn SetEntityLocalPosition;
        public static FindEntityByNameFn   FindEntityByName;
        public static GetEntityNameFn      GetEntityName;
        public static GetEntitiesFn        GetEntitiesRaw;
        public static GetEntityCountFn     GetEntityCount;
      public static CreateEntityFn         CreateEntity;
        public static DestroyEntityFn      DestroyEntity;
        public static GetEntityByIDFn      GetEntityByID;

        public static GetRotationFn        GetEntityRotation;
        public static SetRotationFn        SetEntityRotation;
        public static GetRotationQuatFn    GetEntityRotationQuat;
        public static SetRotationQuatFn    SetEntityRotationQuat;
        public static GetScaleFn           GetEntityScale;
        public static SetScaleFn           SetEntityScale;

        public static SetLinearVelocityFn  SetLinearVelocity;
        public static SetAngularVelocityFn SetAngularVelocity;
        public static SetEntityVisibleFn   SetEntityVisible;
        public static GetEntityVisibleFn   GetEntityVisible;
        public static SetEntityPresentationHiddenFn SetEntityPresentationHidden;
        public static GetEntityPresentationHiddenFn GetEntityPresentationHidden;
        public static SetEntityActiveFn    SetEntityActive;
        public static GetEntityActiveFn    GetEntityActive;
        public static IsSceneBeingDestroyedFn IsSceneBeingDestroyedFunc;
        
        // Parenting
        public static SetEntityParentFn     SetEntityParent;
        public static GetEntityParentFn     GetEntityParent;
        public static GetEntityChildrenFn   GetEntityChildrenRaw;
        public static GetEntityChildCountFn GetEntityChildCount;
        public static FindChildByNameFn     FindChildByName;
        public static FindDescendantByNameFn FindDescendantByName;
        public static CreateEntityWithParentFn CreateEntityWithParent;
        public static DuplicateEntityFn     DuplicateEntity;
        public static GetUIMousePositionFn GetUIMousePosition;

        // -----------------------------------------------------------------
        // Initialization from native side.  The native code passes an array
        // of function pointers in the exact order defined above.
        // -----------------------------------------------------------------
        /// <summary>
        /// Entry called from native host to bind function pointers to managed delegates.
        /// </summary>
        public static void InitializeInteropExport(IntPtr* ptrs, int count) => InitializeInterop(ptrs, count);

        /// <summary>
        /// Binds the native function-pointer table to managed delegates.
        /// Expects the following contiguous layout: [EntityCore][Tween][ComponentInterop][ModuleComponentInterop...]
        /// </summary>
        private static T SafeGetDelegate<T>(IntPtr ptr, int index, string name) where T : Delegate
        {
            if (ptr == IntPtr.Zero)
            {
                Console.WriteLine($"[EntityInterop] WARNING: Null pointer at index {index} for {name}");
                return null;
            }
            try
            {
                return Marshal.GetDelegateForFunctionPointer<T>(ptr);
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[EntityInterop] ERROR creating delegate at index {index} for {name}: {ex.Message}");
                return null;
            }
        }

        public static unsafe void InitializeInterop(IntPtr* ptrs, int count)
        {
            Console.WriteLine($"[EntityInterop] InitializeInterop called with count={count}");
            
            if (ptrs == null || count <= 0)
            {
                Console.WriteLine("[EntityInterop] Invalid interop pointer table.");
                return;
            }

            if (count < EntityCoreCount)
            {
                Console.WriteLine($"[EntityInterop] Expected >={EntityCoreCount} core function pointers, received {count}.");
                return;
            }

            int i = 0;
            GetEntityWorldPosition = SafeGetDelegate<GetEntityWorldPositionFn>(ptrs[i], i, "GetEntityWorldPosition"); i++;
            SetEntityWorldPosition = SafeGetDelegate<SetEntityWorldPositionFn>(ptrs[i], i, "SetEntityWorldPosition"); i++;
            FindEntityByName   = SafeGetDelegate<FindEntityByNameFn>(ptrs[i], i, "FindEntityByName"); i++;
            GetEntityName      = SafeGetDelegate<GetEntityNameFn>(ptrs[i], i, "GetEntityName"); i++;
            GetEntitiesRaw     = SafeGetDelegate<GetEntitiesFn>(ptrs[i], i, "GetEntitiesRaw"); i++;
            GetEntityCount     = SafeGetDelegate<GetEntityCountFn>(ptrs[i], i, "GetEntityCount"); i++;

            CreateEntity       = SafeGetDelegate<CreateEntityFn>(ptrs[i], i, "CreateEntity"); i++;
            DestroyEntity      = SafeGetDelegate<DestroyEntityFn>(ptrs[i], i, "DestroyEntity"); i++;
            GetEntityByID      = SafeGetDelegate<GetEntityByIDFn>(ptrs[i], i, "GetEntityByID"); i++;

            GetEntityRotation  = SafeGetDelegate<GetRotationFn>(ptrs[i], i, "GetEntityRotation"); i++;
            SetEntityRotation  = SafeGetDelegate<SetRotationFn>(ptrs[i], i, "SetEntityRotation"); i++;
            GetEntityRotationQuat = SafeGetDelegate<GetRotationQuatFn>(ptrs[i], i, "GetEntityRotationQuat"); i++;
            SetEntityRotationQuat = SafeGetDelegate<SetRotationQuatFn>(ptrs[i], i, "SetEntityRotationQuat"); i++;
            GetEntityScale     = SafeGetDelegate<GetScaleFn>(ptrs[i], i, "GetEntityScale"); i++;
            SetEntityScale     = SafeGetDelegate<SetScaleFn>(ptrs[i], i, "SetEntityScale"); i++;

            SetLinearVelocity  = SafeGetDelegate<SetLinearVelocityFn>(ptrs[i], i, "SetLinearVelocity"); i++;
            SetAngularVelocity = SafeGetDelegate<SetAngularVelocityFn>(ptrs[i], i, "SetAngularVelocity"); i++;
            SetEntityVisible   = SafeGetDelegate<SetEntityVisibleFn>(ptrs[i], i, "SetEntityVisible"); i++;
            GetEntityVisible   = SafeGetDelegate<GetEntityVisibleFn>(ptrs[i], i, "GetEntityVisible"); i++;
            SetEntityPresentationHidden = SafeGetDelegate<SetEntityPresentationHiddenFn>(ptrs[i], i, "SetEntityPresentationHidden"); i++;
            GetEntityPresentationHidden = SafeGetDelegate<GetEntityPresentationHiddenFn>(ptrs[i], i, "GetEntityPresentationHidden"); i++;
            SetEntityActive    = SafeGetDelegate<SetEntityActiveFn>(ptrs[i], i, "SetEntityActive"); i++;
            GetEntityActive    = SafeGetDelegate<GetEntityActiveFn>(ptrs[i], i, "GetEntityActive"); i++;
            IsSceneBeingDestroyedFunc = SafeGetDelegate<IsSceneBeingDestroyedFn>(ptrs[i], i, "IsSceneBeingDestroyed"); i++;
            GetEntityLocalPosition = SafeGetDelegate<GetEntityLocalPositionFn>(ptrs[i], i, "GetEntityLocalPosition"); i++;
            SetEntityLocalPosition = SafeGetDelegate<SetEntityLocalPositionFn>(ptrs[i], i, "SetEntityLocalPosition"); i++;
            // Parenting
            SetEntityParent = SafeGetDelegate<SetEntityParentFn>(ptrs[i], i, "SetEntityParent"); i++;
            GetEntityParent = SafeGetDelegate<GetEntityParentFn>(ptrs[i], i, "GetEntityParent"); i++;
            GetEntityChildrenRaw = SafeGetDelegate<GetEntityChildrenFn>(ptrs[i], i, "GetEntityChildren"); i++;
            GetEntityChildCount = SafeGetDelegate<GetEntityChildCountFn>(ptrs[i], i, "GetEntityChildCount"); i++;
            FindChildByName = SafeGetDelegate<FindChildByNameFn>(ptrs[i], i, "FindChildByName"); i++;
            FindDescendantByName = SafeGetDelegate<FindDescendantByNameFn>(ptrs[i], i, "FindDescendantByName"); i++;
            CreateEntityWithParent = SafeGetDelegate<CreateEntityWithParentFn>(ptrs[i], i, "CreateEntityWithParent"); i++;
            DuplicateEntity = SafeGetDelegate<DuplicateEntityFn>(ptrs[i], i, "DuplicateEntity"); i++;
            GetUIMousePosition = SafeGetDelegate<GetUIMousePositionFn>(ptrs[i], i, "GetUIMousePosition"); i++;
            
            Console.WriteLine($"[EntityInterop] Core delegates initialized (i={i})");

            // Initialize ComponentInterop with the remaining pointers (includes Tween block preceding it)
            // Skip the fixed Tween block before ComponentInterop
            Console.WriteLine($"[EntityInterop] Before ComponentInterop: i={i}, TweenCount={TweenCount}");
            var componentInteropPtrs = (void**)(ptrs + i + TweenCount);
            var remainingCount = count - (i + TweenCount);
            Console.WriteLine($"[EntityInterop] Calling ComponentInterop.Initialize with remainingCount={remainingCount}");
            ComponentInterop.Initialize(componentInteropPtrs, remainingCount);
            Console.WriteLine("[EntityInterop] ComponentInterop.Initialize returned");
            
            // We passed a fixed-size header before ComponentInterop: Tween
            i += TweenCount;
            // Component interop fixed-size block
            i += ComponentInteropCount;
            Console.WriteLine($"[EntityInterop] After component blocks: i={i}, count={count}");
            
            // Initialize ModuleComponentInterop with the remaining pointers
            if (count > i)
            {
                Console.WriteLine($"[EntityInterop] Calling ModuleComponentInterop.Initialize with {count - i} pointers");
                var moduleComponentInteropPtrs = (void**)(ptrs + i);
                var moduleRemainingCount = count - i;
                ModuleComponentInterop.Initialize(moduleComponentInteropPtrs, moduleRemainingCount);
                Console.WriteLine("[EntityInterop] ModuleComponentInterop.Initialize returned");
            }
            else
            {
                Console.WriteLine($"[EntityInterop] Skipping ModuleComponentInterop (count={count} <= i={i})");
            }

            // Attempt to bind scene interop functions appended near the end if present
            Console.WriteLine("[EntityInterop] Binding SceneInterop...");
            try
            {
                // Scene interop is appended at the end of the layout
                if (count >= SceneInteropCount && SceneInterop.LoadScene == null)
                {
                    int baseIndex = count - SceneInteropCount;
                    var loadPtr = ptrs[baseIndex + 0];
                    var unloadPtr = ptrs[baseIndex + 1];
                    var loadExPtr = ptrs[baseIndex + 2];
                    var progressPtr = ptrs[baseIndex + 3];
                    var isLoadingPtr = ptrs[baseIndex + 4];
                    var isLoadedPtr = ptrs[baseIndex + 5];
                    var getCurrentScenePathPtr = ptrs[baseIndex + 6];
                    Console.WriteLine($"[EntityInterop] SceneInterop pointers: load={loadPtr != IntPtr.Zero}, unload={unloadPtr != IntPtr.Zero}, loadEx={loadExPtr != IntPtr.Zero}, progress={progressPtr != IntPtr.Zero}, isLoading={isLoadingPtr != IntPtr.Zero}, isLoaded={isLoadedPtr != IntPtr.Zero}, getCurrentScenePath={getCurrentScenePathPtr != IntPtr.Zero}");
                    if (loadPtr != IntPtr.Zero) {
                        SceneInterop.LoadScene = Marshal.GetDelegateForFunctionPointer<SceneInterop.Scene_LoadSceneFn>(loadPtr);
                    }
                    if (unloadPtr != IntPtr.Zero) {
                        SceneInterop.UnloadScene = Marshal.GetDelegateForFunctionPointer<SceneInterop.Scene_UnloadSceneFn>(unloadPtr);
                    }
                    if (loadExPtr != IntPtr.Zero) {
                        SceneInterop.LoadSceneEx = Marshal.GetDelegateForFunctionPointer<SceneInterop.Scene_LoadSceneExFn>(loadExPtr);
                    }
                    if (progressPtr != IntPtr.Zero) {
                        SceneInterop.GetLoadProgress = Marshal.GetDelegateForFunctionPointer<SceneInterop.Scene_GetLoadProgressFn>(progressPtr);
                    }
                    if (isLoadingPtr != IntPtr.Zero) {
                        SceneInterop.IsSceneLoading = Marshal.GetDelegateForFunctionPointer<SceneInterop.Scene_IsSceneLoadingFn>(isLoadingPtr);
                    }
                    if (isLoadedPtr != IntPtr.Zero) {
                        SceneInterop.IsSceneLoaded = Marshal.GetDelegateForFunctionPointer<SceneInterop.Scene_IsSceneLoadedFn>(isLoadedPtr);
                    }
                    if (getCurrentScenePathPtr != IntPtr.Zero) {
                        SceneInterop.GetCurrentScenePath = Marshal.GetDelegateForFunctionPointer<SceneInterop.Scene_GetCurrentScenePathFn>(getCurrentScenePathPtr);
                    }
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[EntityInterop] SceneInterop binding failed: {ex.Message}");
            }

            Console.WriteLine("[Managed] EntityInterop delegates initialized.");
        }

        // ---------------------- Convenience Wrappers ----------------------
        /// <summary>
        /// Gets the world position of the entity (extracted from WorldMatrix).
        /// </summary>
        public static Vector3 GetPosition(int entityID)
        {
            if (GetEntityWorldPosition == null) return Vector3.Zero;
            GetEntityWorldPosition(entityID, out float x, out float y, out float z);
            return new Vector3(x, y, z);
        }

        /// <summary>
        /// Sets the world position of the entity (converts to local position internally).
        /// </summary>
        public static void SetPosition(int entityID, Vector3 position)
        {
            if (SetEntityWorldPosition == null) return;
            SetEntityWorldPosition(entityID, position.X, position.Y, position.Z);
        }

        public static Vector3 GetLocalPosition(int entityID)
        {
            if (GetEntityLocalPosition == null) return Vector3.Zero;
            GetEntityLocalPosition(entityID, out float x, out float y, out float z);
            return new Vector3(x, y, z);
        }

        public static void SetLocalPosition(int entityID, Vector3 position)
        {
            if (SetEntityLocalPosition == null) return;
            SetEntityLocalPosition(entityID, position.X, position.Y, position.Z);
        }

        public static int FindByName(string name)
        {
            if (FindEntityByName == null) return -1;
            return FindEntityByName(name);
        }

        public static string GetName(int entityID)
        {
            if (GetEntityName == null) return string.Empty;
            IntPtr ptr = GetEntityName(entityID);
            return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) ?? string.Empty : string.Empty;
        }

        public static Vector3 GetRotation(int entityID)
        {
            if (GetEntityRotation == null) return Vector3.Zero;
            GetEntityRotation(entityID, out float x, out float y, out float z);
            return new Vector3(x, y, z);
        }

        public static void SetRotation(int entityID, Vector3 rot)
        {
            if (SetEntityRotation == null) return;
            SetEntityRotation(entityID, rot.X, rot.Y, rot.Z);
        }

        public static Quaternion GetRotationQuat(int entityID)
        {
            if (GetEntityRotationQuat == null) return Quaternion.Identity;
            GetEntityRotationQuat(entityID, out float x, out float y, out float z, out float w);
            return new Quaternion(x, y, z, w);
        }

        public static void SetRotationQuat(int entityID, Quaternion q)
        {
            if (SetEntityRotationQuat == null) return;
            SetEntityRotationQuat(entityID, q.X, q.Y, q.Z, q.W);
        }

        public static Vector3 GetScale(int entityID)
        {
            if (GetEntityScale == null) return Vector3.One;
            GetEntityScale(entityID, out float x, out float y, out float z);
            return new Vector3(x, y, z);
        }
        
        public static void SetScale(int entityID, Vector3 scale)
        {
            if (SetEntityScale == null) return;
            SetEntityScale(entityID, scale.X, scale.Y, scale.Z);
        }

        public static int[] GetEntities()
        {
            if (GetEntitiesRaw == null || GetEntityCount == null) return Array.Empty<int>();
            IntPtr ptr = GetEntitiesRaw();
            int count = GetEntityCount();
            if (ptr == IntPtr.Zero || count <= 0) return Array.Empty<int>();
            int[] ids = new int[count];
            Marshal.Copy(ptr, ids, 0, count);
            return ids;
        }

        // Convenience wrappers
        public static void SetVisible(int entityID, bool visible)
        {
            if (SetEntityVisible == null) return;
            SetEntityVisible(entityID, visible);
        }
        
        public static bool IsVisible(int entityID)
        {
            if (GetEntityVisible == null) return true;
            return GetEntityVisible(entityID);
        }

        public static void SetPresentationHidden(int entityID, bool hidden)
        {
            if (SetEntityPresentationHidden == null) return;
            SetEntityPresentationHidden(entityID, hidden);
        }

        public static bool IsPresentationHidden(int entityID)
        {
            if (GetEntityPresentationHidden == null) return false;
            return GetEntityPresentationHidden(entityID);
        }
        
        public static void SetActive(int entityID, bool active)
        {
            if (SetEntityActive == null) return;
            SetEntityActive(entityID, active);
        }
        
        public static bool IsActive(int entityID)
        {
            if (GetEntityActive == null) return true;
            return GetEntityActive(entityID);
        }
        
        /// <summary>
        /// Returns true if the scene is currently being destroyed.
        /// Scripts should NOT destroy entities when this returns true.
        /// </summary>
        public static bool IsSceneBeingDestroyed()
        {
            if (IsSceneBeingDestroyedFunc == null) return false;
            return IsSceneBeingDestroyedFunc();
        }

        // ---------------------- Parenting Wrappers ----------------------
        public static void SetParent(int childID, int parentID, bool preserveWorldTransform = false)
        {
            if (SetEntityParent == null) return;
            SetEntityParent(childID, parentID, preserveWorldTransform);
        }

        public static int GetParent(int entityID)
        {
            if (GetEntityParent == null) return -1;
            return GetEntityParent(entityID);
        }

        public static int[] GetChildren(int entityID)
        {
            if (GetEntityChildrenRaw == null || GetEntityChildCount == null) return Array.Empty<int>();
            int count = GetEntityChildCount(entityID);
            if (count <= 0) return Array.Empty<int>();
            IntPtr ptr = GetEntityChildrenRaw(entityID);
            if (ptr == IntPtr.Zero) return Array.Empty<int>();
            int[] ids = new int[count];
            Marshal.Copy(ptr, ids, 0, count);
            return ids;
        }

        public static int FindChild(int parentID, string name)
        {
            if (FindChildByName == null) return -1;
            return FindChildByName(parentID, name);
        }

        public static int FindDescendant(int rootID, string name)
        {
            if (FindDescendantByName == null) return -1;
            return FindDescendantByName(rootID, name);
        }

        public static int CreateWithParent(string name, int parentID)
        {
            if (CreateEntityWithParent == null) return -1;
            return CreateEntityWithParent(name, parentID);
        }

        public static int Duplicate(int entityID)
        {
            if (DuplicateEntity == null) return -1;
            return DuplicateEntity(entityID);
        }

        public static bool GetUIMousePos(out float x, out float y)
        {
            x = 0;
            y = 0;
            if (GetUIMousePosition == null) return false;
            return GetUIMousePosition(out x, out y);
        }

    }
}
