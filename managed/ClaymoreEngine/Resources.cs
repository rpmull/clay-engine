using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using ClaymoreEngine.Scripting.Scriptable;

namespace ClaymoreEngine
{
    /// <summary>
    /// Type of change that occurred to a resource
    /// </summary>
    public enum ResourceChangeType
    {
        Added = 0,
        Modified = 1,
        Removed = 2
    }

    /// <summary>
    /// Information about a resource change event
    /// </summary>
    public readonly struct ResourceChangeEvent
    {
        public readonly ResourceChangeType Type;
        public readonly string TypeName;
        public readonly string Name;
        public readonly string Guid;

        public ResourceChangeEvent(ResourceChangeType type, string typeName, string name, string guid)
        {
            Type = type;
            TypeName = typeName;
            Name = name;
            Guid = guid;
        }
    }

    /// <summary>
    /// Static class providing access to assets in the resources/ folder.
    /// Resources are ClayScriptableObjects that can be loaded by type and name at runtime.
    /// </summary>
    public static class Resources
    {
        // Interop delegates
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GetResourceCountDelegate([MarshalAs(UnmanagedType.LPStr)] string typeName);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GetResourceGUIDsDelegate(
            [MarshalAs(UnmanagedType.LPStr)] string typeName,
            IntPtr outGuids,
            int maxCount);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate IntPtr GetResourceByNameDelegate(
            [MarshalAs(UnmanagedType.LPStr)] string typeName,
            [MarshalAs(UnmanagedType.LPStr)] string name);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GetResourceNamesDelegate(
            [MarshalAs(UnmanagedType.LPStr)] string typeName,
            IntPtr outNames,
            int maxCount);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate bool IsInitializedDelegate();

        // Change notification delegates
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void SubscribeToTypeDelegate([MarshalAs(UnmanagedType.LPStr)] string typeName);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void UnsubscribeFromTypeDelegate([MarshalAs(UnmanagedType.LPStr)] string typeName);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate bool HasResourcesFolderDelegate();

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate bool TryDiscoverFolderDelegate([MarshalAs(UnmanagedType.LPStr)] string projectRoot);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int FlushPendingChangesDelegate();

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GetPendingChangeCountDelegate();

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GetPendingChangesDelegate(
            IntPtr outTypes,
            IntPtr outTypeNames,
            IntPtr outNames,
            IntPtr outGuids,
            int maxCount);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate IntPtr GetGuidFromPathDelegate([MarshalAs(UnmanagedType.LPStr)] string vfsPath);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GetAssetCountByTypeDelegate([MarshalAs(UnmanagedType.LPStr)] string typeName);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GetAssetGUIDsByTypeDelegate(
            [MarshalAs(UnmanagedType.LPStr)] string typeName,
            IntPtr outGuids,
            int maxCount);
        
        // Cached delegates
        private static GetResourceCountDelegate? _getResourceCount;
        private static GetResourceGUIDsDelegate? _getResourceGUIDs;
        private static GetResourceByNameDelegate? _getResourceByName;
        private static GetResourceNamesDelegate? _getResourceNames;
        private static IsInitializedDelegate? _isInitialized;
        private static SubscribeToTypeDelegate? _subscribeToType;
        private static UnsubscribeFromTypeDelegate? _unsubscribeFromType;
        private static HasResourcesFolderDelegate? _hasResourcesFolder;
        private static TryDiscoverFolderDelegate? _tryDiscoverFolder;
        private static FlushPendingChangesDelegate? _flushPendingChanges;
        private static GetPendingChangeCountDelegate? _getPendingChangeCount;
        private static GetPendingChangesDelegate? _getPendingChanges;
        private static GetGuidFromPathDelegate? _getGuidFromPath;
        private static GetAssetCountByTypeDelegate? _getAssetCountByType;
        private static GetAssetGUIDsByTypeDelegate? _getAssetGUIDsByType;
        
        // Cached resources (invalidated on changes)
        private static readonly Dictionary<string, List<ClayScriptableObject>> _cachedByType = new();
        private static readonly Dictionary<string, ClayScriptableObject> _cachedByName = new();
        
        // Subscription tracking - maps type name to list of callbacks
        private static readonly Dictionary<string, List<Action<ResourceChangeEvent>>> _changeCallbacks = new();
        private static readonly HashSet<string> _subscribedTypes = new();
        
        // Track objects with PopulateFromResources for automatic updates
        private static readonly Dictionary<string, List<WeakReference<object>>> _populatedFieldOwners = new();

        /// <summary>
        /// Event raised when resources change. Subscribe for global notifications.
        /// </summary>
        public static event Action<ResourceChangeEvent>? OnResourceChanged;
        
        /// <summary>
        /// Initialize the Resources system with native function pointers.
        /// Called from interop initialization.
        /// </summary>
        [System.Runtime.InteropServices.UnmanagedCallersOnly]
        public static unsafe void Initialize(void** ptrs, int count)
        {
            if (count < 15)
            {
                Console.WriteLine($"[Resources] Expected at least 15 function pointers, got {count}");
                return;
            }
            
            int i = 0;
            _getResourceCount = Marshal.GetDelegateForFunctionPointer<GetResourceCountDelegate>((IntPtr)ptrs[i++]);
            _getResourceGUIDs = Marshal.GetDelegateForFunctionPointer<GetResourceGUIDsDelegate>((IntPtr)ptrs[i++]);
            _getResourceByName = Marshal.GetDelegateForFunctionPointer<GetResourceByNameDelegate>((IntPtr)ptrs[i++]);
            _getResourceNames = Marshal.GetDelegateForFunctionPointer<GetResourceNamesDelegate>((IntPtr)ptrs[i++]);
            _isInitialized = Marshal.GetDelegateForFunctionPointer<IsInitializedDelegate>((IntPtr)ptrs[i++]);
            _subscribeToType = Marshal.GetDelegateForFunctionPointer<SubscribeToTypeDelegate>((IntPtr)ptrs[i++]);
            _unsubscribeFromType = Marshal.GetDelegateForFunctionPointer<UnsubscribeFromTypeDelegate>((IntPtr)ptrs[i++]);
            _hasResourcesFolder = Marshal.GetDelegateForFunctionPointer<HasResourcesFolderDelegate>((IntPtr)ptrs[i++]);
            _tryDiscoverFolder = Marshal.GetDelegateForFunctionPointer<TryDiscoverFolderDelegate>((IntPtr)ptrs[i++]);
            _flushPendingChanges = Marshal.GetDelegateForFunctionPointer<FlushPendingChangesDelegate>((IntPtr)ptrs[i++]);
            _getPendingChangeCount = Marshal.GetDelegateForFunctionPointer<GetPendingChangeCountDelegate>((IntPtr)ptrs[i++]);
            _getPendingChanges = Marshal.GetDelegateForFunctionPointer<GetPendingChangesDelegate>((IntPtr)ptrs[i++]);
            _getGuidFromPath = Marshal.GetDelegateForFunctionPointer<GetGuidFromPathDelegate>((IntPtr)ptrs[i++]);
            _getAssetCountByType = Marshal.GetDelegateForFunctionPointer<GetAssetCountByTypeDelegate>((IntPtr)ptrs[i++]);
            _getAssetGUIDsByType = Marshal.GetDelegateForFunctionPointer<GetAssetGUIDsByTypeDelegate>((IntPtr)ptrs[i++]);
            
            Console.WriteLine("[Resources] Initialized with native function pointers (including asset loading)");
        }
        
        /// <summary>
        /// Check if the Resources system is ready.
        /// </summary>
        public static bool IsInitialized => _isInitialized?.Invoke() ?? false;

        /// <summary>
        /// Check if a resources folder exists in the project.
        /// </summary>
        public static bool HasResourcesFolder => _hasResourcesFolder?.Invoke() ?? false;

        private static string NormalizeResourcePath(string path)
        {
            if (string.IsNullOrWhiteSpace(path))
                return string.Empty;

            string normalized = path.Replace('\\', '/').Trim();
            if (normalized.Length == 0)
                return string.Empty;

            if (!normalized.StartsWith("resources/", StringComparison.OrdinalIgnoreCase))
            {
                int idx = normalized.IndexOf("resources/", StringComparison.OrdinalIgnoreCase);
                if (idx >= 0)
                {
                    normalized = normalized.Substring(idx);
                }
                else
                {
                    normalized = $"resources/{normalized}";
                }
            }

            while (normalized.StartsWith("/"))
                normalized = normalized.Substring(1);

            return normalized;
        }

        internal static string? ResolveGuidFromPath(string vfsPath)
        {
            if (_getGuidFromPath == null) return null;
            string normalized = NormalizeResourcePath(vfsPath);
            if (string.IsNullOrEmpty(normalized)) return null;
            IntPtr guidPtr = _getGuidFromPath(normalized);
            if (guidPtr == IntPtr.Zero) return null;
            string? guid = Marshal.PtrToStringAnsi(guidPtr);
            return string.IsNullOrEmpty(guid) ? null : guid;
        }

        private static ClayScriptableObject? GetResourceByTypeAndName(Type type, string name)
        {
            if (string.IsNullOrEmpty(name)) return null;

            string typeName = type.FullName ?? type.Name;
            string cacheKey = $"{typeName}:{name}";

            if (_cachedByName.TryGetValue(cacheKey, out var cached))
            {
                return cached;
            }

            if (_getResourceByName == null)
            {
                Console.WriteLine("[Resources] Not initialized - cannot get resource");
                return null;
            }

            IntPtr guidPtr = _getResourceByName(typeName, name);
            if (guidPtr == IntPtr.Zero) return null;

            string? guid = Marshal.PtrToStringAnsi(guidPtr);
            if (string.IsNullOrEmpty(guid)) return null;

            var resource = ClayScriptableObjectLoader.Load(guid, type) as ClayScriptableObject;
            if (resource != null)
            {
                _cachedByName[cacheKey] = resource;
            }

            return resource;
        }

        private static List<ClayScriptableObject> GetResourcesByType(Type type)
        {
            string typeName = type.FullName ?? type.Name;

            if (_cachedByType.TryGetValue(typeName, out var cachedList))
            {
                return new List<ClayScriptableObject>(cachedList);
            }

            var result = new List<ClayScriptableObject>();

            if (_getResourceCount == null || _getResourceGUIDs == null)
            {
                Console.WriteLine("[Resources] Not initialized - cannot get resources");
                return result;
            }

            int count = _getResourceCount(typeName);
            if (count <= 0) return result;

            IntPtr[] guidPtrs = new IntPtr[count];
            GCHandle handle = GCHandle.Alloc(guidPtrs, GCHandleType.Pinned);
            try
            {
                int actualCount = _getResourceGUIDs(typeName, handle.AddrOfPinnedObject(), count);
                for (int i = 0; i < actualCount; i++)
                {
                    string? guid = Marshal.PtrToStringAnsi(guidPtrs[i]);
                    if (string.IsNullOrEmpty(guid)) continue;

                    var resource = ClayScriptableObjectLoader.Load(guid, type) as ClayScriptableObject;
                    if (resource != null)
                    {
                        result.Add(resource);
                    }
                }
            }
            finally
            {
                handle.Free();
            }

            _cachedByType[typeName] = new List<ClayScriptableObject>(result);
            return result;
        }
        
        /// <summary>
        /// Get a single resource by name.
        /// </summary>
        /// <typeparam name="T">The ClayScriptableObject type</typeparam>
        /// <param name="name">Resource name (filename without extension)</param>
        /// <returns>The resource, or null if not found</returns>
        public static T? Get<T>(string name) where T : ClayScriptableObject
        {
            if (string.IsNullOrEmpty(name)) return null;
            
            string typeName = typeof(T).FullName!;
            string cacheKey = $"{typeName}:{name}";
            
            // Check cache first
            if (_cachedByName.TryGetValue(cacheKey, out var cached))
            {
                return cached as T;
            }
            
            // Get GUID from native
            if (_getResourceByName == null)
            {
                Console.WriteLine("[Resources] Not initialized - cannot get resource");
                return null;
            }
            
            IntPtr guidPtr = _getResourceByName(typeName, name);
            if (guidPtr == IntPtr.Zero) return null;
            
            string? guid = Marshal.PtrToStringAnsi(guidPtr);
            if (string.IsNullOrEmpty(guid)) return null;
            
            // Load via ClayScriptableObjectLoader
            var resource = ClayScriptableObjectLoader.Load(guid, typeof(T)) as T;
            if (resource != null)
            {
                _cachedByName[cacheKey] = resource;
            }
            
            return resource;
        }

        /// <summary>
        /// Load an asset from the resources/ VFS path.
        /// </summary>
        public static T Load<T>(string path)
        {
            if (string.IsNullOrEmpty(path)) return default!;

            Type type = typeof(T);
            if (typeof(ClayScriptableObject).IsAssignableFrom(type))
            {
                string? guid = ResolveGuidFromPath(path);
                if (string.IsNullOrEmpty(guid)) return default!;
                return ClayScriptableObjectLoader.Load(guid, type) is T typed ? typed : default!;
            }

            string? assetGuid = ResolveGuidFromPath(path);
            if (string.IsNullOrEmpty(assetGuid)) return default!;

            if (type == typeof(Texture))
                return (T)(object)Texture.FromGuid(assetGuid);
            if (type == typeof(Mesh))
                return (T)(object)Mesh.FromGuid(assetGuid);
            if (type == typeof(Prefab))
                return (T)(object)new Prefab { Guid = assetGuid };
            if (type == typeof(AudioClip))
                return (T)(object)AudioClip.FromGuid(assetGuid);

            return default!;
        }

        /// <summary>
        /// Load an asset by name from the resources/ path.
        /// </summary>
        public static T LoadByName<T>(string name)
        {
            if (string.IsNullOrEmpty(name)) return default!;

            Type type = typeof(T);
            if (typeof(ClayScriptableObject).IsAssignableFrom(type))
            {
                var resource = GetResourceByTypeAndName(type, name);
                return resource is T typed ? typed : default!;
            }

            if (!name.Contains("."))
            {
                if (type == typeof(Texture))
                {
                    string[] exts = { ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".dds", ".hdr", ".ktx", ".ktx2" };
                    foreach (string ext in exts)
                    {
                        var tex = Load<Texture>(name + ext);
                        if (tex.IsValid) return (T)(object)tex;
                    }
                }
                else if (type == typeof(Mesh))
                {
                    string[] exts = { ".meshbin", ".fbx", ".gltf", ".glb", ".obj" };
                    foreach (string ext in exts)
                    {
                        var mesh = Load<Mesh>(name + ext);
                        if (mesh.IsValid) return (T)(object)mesh;
                    }
                }
                else if (type == typeof(Prefab))
                {
                    string[] exts = { ".prefab", ".prefabb" };
                    foreach (string ext in exts)
                    {
                        var prefab = Load<Prefab>(name + ext);
                        if (prefab.IsValid) return (T)(object)prefab;
                    }
                }
                else if (type == typeof(AudioClip))
                {
                    string[] exts = { ".wav", ".mp3", ".ogg", ".flac" };
                    foreach (string ext in exts)
                    {
                        var clip = Load<AudioClip>(name + ext);
                        if (clip.IsValid) return (T)(object)clip;
                    }
                }
            }

            return Load<T>(name);
        }

        /// <summary>
        /// Load all resources of the given type from the resources/ path.
        /// </summary>
        public static List<T> LoadAll<T>()
        {
            Type type = typeof(T);
            if (typeof(ClayScriptableObject).IsAssignableFrom(type))
            {
                var resources = GetResourcesByType(type);
                var typed = new List<T>(resources.Count);
                foreach (var res in resources)
                {
                    if (res is T casted)
                        typed.Add(casted);
                }
                return typed;
            }

            var result = new List<T>();
            if (_getAssetCountByType == null || _getAssetGUIDsByType == null)
            {
                Console.WriteLine("[Resources] Asset listing not available");
                return result;
            }

            int count = _getAssetCountByType(type.FullName ?? type.Name);
            if (count <= 0) return result;

            IntPtr[] guidPtrs = new IntPtr[count];
            GCHandle handle = GCHandle.Alloc(guidPtrs, GCHandleType.Pinned);
            try
            {
                int actualCount = _getAssetGUIDsByType(type.FullName ?? type.Name, handle.AddrOfPinnedObject(), count);
                for (int i = 0; i < actualCount; i++)
                {
                    string? guid = Marshal.PtrToStringAnsi(guidPtrs[i]);
                    if (string.IsNullOrEmpty(guid)) continue;
                    if (type == typeof(Texture))
                        result.Add((T)(object)Texture.FromGuid(guid));
                    else if (type == typeof(Mesh))
                        result.Add((T)(object)Mesh.FromGuid(guid));
                    else if (type == typeof(Prefab))
                        result.Add((T)(object)new Prefab { Guid = guid });
                    else if (type == typeof(AudioClip))
                        result.Add((T)(object)AudioClip.FromGuid(guid));
                }
            }
            finally
            {
                handle.Free();
            }

            return result;
        }
        
        /// <summary>
        /// Get all resources of a specific type.
        /// </summary>
        /// <typeparam name="T">The ClayScriptableObject type</typeparam>
        /// <returns>List of all resources of that type</returns>
        public static List<T> GetResources<T>() where T : ClayScriptableObject
        {
            string typeName = typeof(T).FullName!;
            
            // Check cache
            if (_cachedByType.TryGetValue(typeName, out var cachedList))
            {
                return cachedList.ConvertAll(x => (T)x);
            }
            
            var result = new List<T>();
            
            if (_getResourceCount == null || _getResourceGUIDs == null)
            {
                Console.WriteLine("[Resources] Not initialized - cannot get resources");
                return result;
            }
            
            int count = _getResourceCount(typeName);
            if (count <= 0) return result;
            
            // Allocate array for GUIDs
            IntPtr[] guidPtrs = new IntPtr[count];
            GCHandle handle = GCHandle.Alloc(guidPtrs, GCHandleType.Pinned);
            
            try
            {
                int actualCount = _getResourceGUIDs(typeName, handle.AddrOfPinnedObject(), count);
                
                for (int i = 0; i < actualCount; i++)
                {
                    string? guid = Marshal.PtrToStringAnsi(guidPtrs[i]);
                    if (string.IsNullOrEmpty(guid)) continue;
                    
                    var resource = ClayScriptableObjectLoader.Load(guid, typeof(T)) as T;
                    if (resource != null)
                    {
                        result.Add(resource);
                    }
                }
            }
            finally
            {
                handle.Free();
            }
            
            // Cache the result
            _cachedByType[typeName] = result.ConvertAll(x => (ClayScriptableObject)x);
            
            return result;
        }
        
        /// <summary>
        /// Alias for GetResources - get all resources of a type.
        /// </summary>
        public static List<T> GetAll<T>() where T : ClayScriptableObject => GetResources<T>();
        
        /// <summary>
        /// Get all resource names for a specific type.
        /// Useful for editor dropdowns.
        /// </summary>
        public static List<string> GetResourceNames<T>() where T : ClayScriptableObject
        {
            return GetResourceNames(typeof(T).FullName!);
        }
        
        /// <summary>
        /// Get all resource names for a specific type by type name.
        /// </summary>
        public static List<string> GetResourceNames(string typeName)
        {
            var result = new List<string>();
            
            if (_getResourceCount == null || _getResourceNames == null) return result;
            
            int count = _getResourceCount(typeName);
            if (count <= 0) return result;
            
            // Allocate array for names
            IntPtr[] namePtrs = new IntPtr[count];
            GCHandle handle = GCHandle.Alloc(namePtrs, GCHandleType.Pinned);
            
            try
            {
                int actualCount = _getResourceNames(typeName, handle.AddrOfPinnedObject(), count);
                
                for (int i = 0; i < actualCount; i++)
                {
                    string? name = Marshal.PtrToStringAnsi(namePtrs[i]);
                    if (!string.IsNullOrEmpty(name))
                    {
                        result.Add(name);
                    }
                }
            }
            finally
            {
                handle.Free();
            }
            
            return result;
        }
        
        /// <summary>
        /// Clear the resource cache. Called on hot reload or when resources change.
        /// </summary>
        public static void ClearCache()
        {
            _cachedByType.Clear();
            _cachedByName.Clear();
            Console.WriteLine("[Resources] Cache cleared");
        }

        /// <summary>
        /// Clear cache for a specific type only.
        /// </summary>
        public static void ClearCache(string typeName)
        {
            _cachedByType.Remove(typeName);
            
            // Remove any cached by name entries for this type
            var keysToRemove = new List<string>();
            foreach (var key in _cachedByName.Keys)
            {
                if (key.StartsWith(typeName + ":"))
                {
                    keysToRemove.Add(key);
                }
            }
            foreach (var key in keysToRemove)
            {
                _cachedByName.Remove(key);
            }
        }
        
        // ========== Change Notification System ==========

        /// <summary>
        /// Subscribe to changes for a specific type. The native side will track changes
        /// and emit events that can be polled.
        /// </summary>
        public static void SubscribeToType<T>() where T : ClayScriptableObject
        {
            SubscribeToType(typeof(T).FullName!);
        }

        /// <summary>
        /// Subscribe to changes for a specific type by name.
        /// </summary>
        public static void SubscribeToType(string typeName)
        {
            if (_subscribedTypes.Contains(typeName)) return;
            
            _subscribeToType?.Invoke(typeName);
            _subscribedTypes.Add(typeName);
            Console.WriteLine($"[Resources] Subscribed to changes for type: {typeName}");
        }

        /// <summary>
        /// Unsubscribe from changes for a specific type.
        /// </summary>
        public static void UnsubscribeFromType<T>() where T : ClayScriptableObject
        {
            UnsubscribeFromType(typeof(T).FullName!);
        }

        /// <summary>
        /// Unsubscribe from changes for a specific type by name.
        /// </summary>
        public static void UnsubscribeFromType(string typeName)
        {
            if (!_subscribedTypes.Contains(typeName)) return;
            
            _unsubscribeFromType?.Invoke(typeName);
            _subscribedTypes.Remove(typeName);
            Console.WriteLine($"[Resources] Unsubscribed from changes for type: {typeName}");
        }

        /// <summary>
        /// Register a callback for changes to a specific type.
        /// </summary>
        public static void OnTypeChanged<T>(Action<ResourceChangeEvent> callback) where T : ClayScriptableObject
        {
            OnTypeChanged(typeof(T).FullName!, callback);
        }

        /// <summary>
        /// Register a callback for changes to a specific type by name.
        /// </summary>
        public static void OnTypeChanged(string typeName, Action<ResourceChangeEvent> callback)
        {
            // Ensure subscribed at native level
            SubscribeToType(typeName);
            
            if (!_changeCallbacks.TryGetValue(typeName, out var list))
            {
                list = new List<Action<ResourceChangeEvent>>();
                _changeCallbacks[typeName] = list;
            }
            
            list.Add(callback);
        }

        /// <summary>
        /// Remove a change callback for a type.
        /// </summary>
        public static void RemoveTypeChangedCallback(string typeName, Action<ResourceChangeEvent> callback)
        {
            if (_changeCallbacks.TryGetValue(typeName, out var list))
            {
                list.Remove(callback);
                
                // If no more callbacks, consider unsubscribing (but keep if has populated fields)
                if (list.Count == 0 && !_populatedFieldOwners.ContainsKey(typeName))
                {
                    UnsubscribeFromType(typeName);
                    _changeCallbacks.Remove(typeName);
                }
            }
        }

        /// <summary>
        /// Poll for pending changes and dispatch them to callbacks.
        /// Should be called from main thread, typically in Update loop.
        /// </summary>
        public static void PollChanges()
        {
            if (_getPendingChanges == null) return;
            
            // First, flush any native pending changes
            _flushPendingChanges?.Invoke();
            
            const int maxChanges = 64;
            int[] types = new int[maxChanges];
            IntPtr[] typeNames = new IntPtr[maxChanges];
            IntPtr[] names = new IntPtr[maxChanges];
            IntPtr[] guids = new IntPtr[maxChanges];
            
            GCHandle typesHandle = GCHandle.Alloc(types, GCHandleType.Pinned);
            GCHandle typeNamesHandle = GCHandle.Alloc(typeNames, GCHandleType.Pinned);
            GCHandle namesHandle = GCHandle.Alloc(names, GCHandleType.Pinned);
            GCHandle guidsHandle = GCHandle.Alloc(guids, GCHandleType.Pinned);
            
            try
            {
                int count = _getPendingChanges(
                    typesHandle.AddrOfPinnedObject(),
                    typeNamesHandle.AddrOfPinnedObject(),
                    namesHandle.AddrOfPinnedObject(),
                    guidsHandle.AddrOfPinnedObject(),
                    maxChanges);
                
                for (int i = 0; i < count; i++)
                {
                    string? typeName = Marshal.PtrToStringAnsi(typeNames[i]);
                    string? name = Marshal.PtrToStringAnsi(names[i]);
                    string? guid = Marshal.PtrToStringAnsi(guids[i]);
                    
                    if (string.IsNullOrEmpty(typeName)) continue;
                    
                    var changeEvent = new ResourceChangeEvent(
                        (ResourceChangeType)types[i],
                        typeName!,
                        name ?? "",
                        guid ?? "");
                    
                    // Clear cache for this type
                    ClearCache(typeName!);
                    
                    // Dispatch to type-specific callbacks
                    if (_changeCallbacks.TryGetValue(typeName!, out var callbacks))
                    {
                        foreach (var callback in callbacks)
                        {
                            try
                            {
                                callback(changeEvent);
                            }
                            catch (Exception ex)
                            {
                                Console.WriteLine($"[Resources] Callback error: {ex.Message}");
                            }
                        }
                    }
                    
                    // Update any populated fields
                    UpdatePopulatedFields(typeName!, changeEvent);
                    
                    // Raise global event
                    OnResourceChanged?.Invoke(changeEvent);
                }
            }
            finally
            {
                typesHandle.Free();
                typeNamesHandle.Free();
                namesHandle.Free();
                guidsHandle.Free();
            }
        }
        
        // ========== PopulateFromResources Support ==========
        
        /// <summary>
        /// Register an object with a populated field for automatic updates.
        /// Called internally when PopulateFromResources processes a field.
        /// </summary>
        internal static void RegisterPopulatedField(object owner, string typeName, System.Collections.IList list)
        {
            // Subscribe to this type for changes
            SubscribeToType(typeName);
            
            if (!_populatedFieldOwners.TryGetValue(typeName, out var owners))
            {
                owners = new List<WeakReference<object>>();
                _populatedFieldOwners[typeName] = owners;
            }
            
            // Add weak reference to avoid preventing GC
            owners.Add(new WeakReference<object>(owner));
        }

        /// <summary>
        /// Update all populated fields when a resource type changes.
        /// </summary>
        private static void UpdatePopulatedFields(string typeName, ResourceChangeEvent changeEvent)
        {
            if (!_populatedFieldOwners.TryGetValue(typeName, out var owners)) return;
            
            // Clean up dead references and update live ones
            for (int i = owners.Count - 1; i >= 0; i--)
            {
                if (!owners[i].TryGetTarget(out var owner))
                {
                    owners.RemoveAt(i);
                    continue;
                }
                
                // Find and update the populated field
                UpdateOwnerPopulatedFields(owner, typeName, changeEvent);
            }
            
            // If no more owners, consider unsubscribing
            if (owners.Count == 0 && !_changeCallbacks.ContainsKey(typeName))
            {
                UnsubscribeFromType(typeName);
                _populatedFieldOwners.Remove(typeName);
            }
        }

        /// <summary>
        /// Update populated fields on a specific owner object.
        /// </summary>
        private static void UpdateOwnerPopulatedFields(object owner, string typeName, ResourceChangeEvent changeEvent)
        {
            var type = owner.GetType();
            var fields = type.GetFields(
                System.Reflection.BindingFlags.Instance | 
                System.Reflection.BindingFlags.Public | 
                System.Reflection.BindingFlags.NonPublic);
            
            foreach (var field in fields)
            {
                // Check for PopulateFromResources attribute (both plural and singular forms)
                var attr = field.GetCustomAttributes(typeof(PopulateFromResourcesAttribute), false);
                var attrSingular = field.GetCustomAttributes(typeof(PopulateFromResourceAttribute), false);
                if (attr.Length == 0 && attrSingular.Length == 0) continue;
                
                // Check if this field's element type matches the changed type
                if (!field.FieldType.IsGenericType) continue;
                if (field.FieldType.GetGenericTypeDefinition() != typeof(List<>)) continue;
                
                var elementType = field.FieldType.GetGenericArguments()[0];
                if (elementType.FullName != typeName) continue;
                
                // Get the list and update it
                var list = field.GetValue(owner) as System.Collections.IList;
                if (list == null) continue;
                
                // Re-populate the list
                PopulateList(list, elementType);
                
                Console.WriteLine($"[Resources] Updated populated field '{field.Name}' on {type.Name} ({changeEvent.Type}: {changeEvent.Name})");
            }
        }
        
        /// <summary>
        /// Populate a list field with all resources of its element type.
        /// Used by PopulateFromResources attribute handler.
        /// </summary>
        internal static void PopulateList<T>(List<T> list) where T : ClayScriptableObject
        {
            list.Clear();
            list.AddRange(GetResources<T>());
        }
        
        /// <summary>
        /// Non-generic populate for reflection use.
        /// </summary>
        internal static void PopulateList(System.Collections.IList list, Type elementType)
        {
            if (!typeof(ClayScriptableObject).IsAssignableFrom(elementType))
            {
                Console.WriteLine($"[Resources] Cannot populate list - element type {elementType} is not a ClayScriptableObject");
                return;
            }
            
            list.Clear();
            
            string typeName = elementType.FullName!;
            
            if (_getResourceCount == null || _getResourceGUIDs == null) return;
            
            int count = _getResourceCount(typeName);
            if (count <= 0) return;
            
            IntPtr[] guidPtrs = new IntPtr[count];
            GCHandle handle = GCHandle.Alloc(guidPtrs, GCHandleType.Pinned);
            
            try
            {
                int actualCount = _getResourceGUIDs(typeName, handle.AddrOfPinnedObject(), count);
                
                for (int i = 0; i < actualCount; i++)
                {
                    string? guid = Marshal.PtrToStringAnsi(guidPtrs[i]);
                    if (string.IsNullOrEmpty(guid)) continue;
                    
                    var resource = ClayScriptableObjectLoader.Load(guid, elementType);
                    if (resource != null)
                    {
                        list.Add(resource);
                    }
                }
            }
            finally
            {
                handle.Free();
            }
        }
    }
}
