using System;
using System.Numerics;
using System.Runtime.InteropServices;
using System.Threading.Tasks;

namespace ClaymoreEngine
{
    public struct Prefab
    {
        // 32-char hex guid or null/empty
        public string? Guid;

        /// <summary>
        /// Returns true if this Prefab reference has a valid GUID assigned.
        /// Use this instead of == null checks since Prefab is a struct.
        /// </summary>
        public bool IsValid => !string.IsNullOrWhiteSpace(Guid) && Guid!.Length == 32;

        /// <summary>
        /// Gets the name of the prefab asset that would be instantiated.
        /// This is the name of the root entity that would be created when calling Instantiate().
        /// </summary>
        public string name
        {
            get
            {
                if (!IsValid || !TryParseGuid(Guid, out ulong hi, out ulong lo))
                    return string.Empty;
                
                if (PrefabInterop.GetAssetNameByGuid == null)
                    return string.Empty;
                
                IntPtr ptr = PrefabInterop.GetAssetNameByGuid(hi, lo);
                return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) ?? string.Empty : string.Empty;
            }
        }

        private static bool TryParseGuid(string? guid, out ulong high, out ulong low)
        {
            high = 0; low = 0;
            if (string.IsNullOrWhiteSpace(guid) || guid!.Length != 32)
                return false;
            try
            {
                high = Convert.ToUInt64(guid.Substring(0, 16), 16);
                low  = Convert.ToUInt64(guid.Substring(16, 16), 16);
                return true;
            }
            catch { return false; }
        }

        private enum PrefabAsyncStatus
        {
            Pending = 0,
            Ready = 1,
            Failed = 2,
            NotFound = 3
        }

        /// <summary>
        /// Instantiate this prefab into the scene. Returns the root entity or an invalid entity if failed.
        /// </summary>
        public Entity Instantiate(Vector3? position = null)
        {
            return Instantiate(this, position);
        }

        /// <summary>
        /// Instantiate this prefab into the scene using an existing entity as the root.
        /// </summary>
        public Entity Instantiate(Entity root, Vector3? position = null, bool useRootAsModelRoot = false)
        {
            return Instantiate(this, root, position, useRootAsModelRoot);
        }

        public Task<Entity> InstantiateAsync(Vector3? position = null)
        {
            return InstantiateAsync(this, position);
        }

        public Entity InstantiatePrepared(Vector3? position = null,
            Func<Entity, Task<bool>>? onReadyAsync = null)
        {
            return InstantiatePrepared(this, position, onReadyAsync);
        }

        public Entity InstantiateNonBlocking(Vector3? position = null)
        {
            return InstantiateNonBlocking(this, position);
        }

        public Entity InstantiateNonBlocking(Entity root, Vector3? position =
            null, bool useRootAsModelRoot = false)
        {
            return InstantiateNonBlocking(this, root, position,
                useRootAsModelRoot);
        }

        public Task<Entity> InstantiateAsync(Entity root, Vector3? position = null, bool useRootAsModelRoot = false)
        {
            return InstantiateAsync(this, root, position, useRootAsModelRoot);
        }

        public Entity InstantiatePrepared(Entity root, Vector3? position =
            null, bool useRootAsModelRoot = false, Func<Entity, Task<bool>>?
            onReadyAsync = null)
        {
            return InstantiatePrepared(this, root, position,
                useRootAsModelRoot, onReadyAsync);
        }

        /// <summary>
        /// Instantiate a prefab into the scene. Returns the root entity or an invalid entity if failed.
        /// </summary>
        public static Entity Instantiate(Prefab p, Vector3? position = null)
        {
            if (!TryParseGuid(p.Guid, out ulong hi, out ulong lo))
                return new Entity(-1);
            int id = PrefabInterop.InstantiateBlocking != null
                ? PrefabInterop.InstantiateBlocking(hi, lo)
                : (PrefabInterop.Instantiate != null ? PrefabInterop.Instantiate(hi, lo) : -1);
            if (id <= 0) return new Entity(-1);
            var e = new Entity(id);
            if (position.HasValue)
                EntityInterop.SetPosition(id, position.Value);
            return e;
        }

        /// <summary>
        /// Instantiate a prefab into the scene using an existing entity as the root.
        /// </summary>
        public static Entity Instantiate(Prefab p, Entity root, Vector3? position = null, bool useRootAsModelRoot = false)
        {
            if (!TryParseGuid(p.Guid, out ulong hi, out ulong lo))
                return new Entity(-1);

            bool reuseRoot = useRootAsModelRoot && root.IsValid && PrefabInterop.InstantiateWithRoot != null;
            int id = reuseRoot
                ? PrefabInterop.InstantiateWithRoot!(hi, lo, root.EntityID, true)
                : (PrefabInterop.InstantiateBlocking != null
                    ? PrefabInterop.InstantiateBlocking(hi, lo)
                    : (PrefabInterop.Instantiate != null ? PrefabInterop.Instantiate(hi, lo) : -1));

            if (id <= 0) return new Entity(-1);
            var e = new Entity(id);
            if (root.IsValid && !reuseRoot)
                e.SetParent(root);
            if (position.HasValue)
                EntityInterop.SetPosition(id, position.Value);
            return e;
        }

        /// <summary>
        /// Instantiate a prefab without waiting for async population to finish.
        /// Returns the placeholder root immediately.
        /// </summary>
        public static Entity InstantiateNonBlocking(Prefab p, Vector3?
            position = null)
        {
            if (!TryParseGuid(p.Guid, out ulong hi, out ulong lo))
                return new Entity(-1);

            int id = PrefabInterop.Instantiate != null
                ? PrefabInterop.Instantiate(hi, lo)
                : (PrefabInterop.InstantiateBlocking != null
                    ? PrefabInterop.InstantiateBlocking(hi, lo)
                    : -1);
            if (id <= 0) return new Entity(-1);

            var e = new Entity(id);
            if (position.HasValue)
                EntityInterop.SetPosition(id, position.Value);
            return e;
        }

        /// <summary>
        /// Instantiate a prefab without waiting for async population to finish.
        /// Returns the placeholder root immediately.
        /// </summary>
        public static Entity InstantiateNonBlocking(Prefab p, Entity root,
            Vector3? position = null, bool useRootAsModelRoot = false)
        {
            if (!TryParseGuid(p.Guid, out ulong hi, out ulong lo))
                return new Entity(-1);

            bool reuseRoot = useRootAsModelRoot && root.IsValid &&
                PrefabInterop.InstantiateWithRoot != null;
            int id = reuseRoot
                ? PrefabInterop.InstantiateWithRoot!(hi, lo, root.EntityID,
                    true)
                : (PrefabInterop.Instantiate != null
                    ? PrefabInterop.Instantiate(hi, lo)
                    : (PrefabInterop.InstantiateBlocking != null
                        ? PrefabInterop.InstantiateBlocking(hi, lo)
                        : -1));

            if (id <= 0) return new Entity(-1);

            var e = new Entity(id);
            if (root.IsValid && !reuseRoot)
                e.SetParent(root);
            if (position.HasValue)
                EntityInterop.SetPosition(id, position.Value);
            return e;
        }

        /// <summary>
        /// Await until an entity is ready for use.
        /// Async prefab placeholders will wait for completion; ordinary
        /// entities return immediately.
        /// </summary>
        public static async Task<Entity> WaitUntilReadyAsync(Entity entity)
        {
            if (!entity.IsValid)
                return new Entity(-1);

            if (PrefabInterop.GetAsyncStatus == null)
                return entity;

            while (true)
            {
                int status = PrefabInterop.GetAsyncStatus(entity.EntityID);
                if (status == (int)PrefabAsyncStatus.Ready)
                    return entity;
                if (status == (int)PrefabAsyncStatus.Failed ||
                    status == (int)PrefabAsyncStatus.NotFound)
                {
                    return new Entity(-1);
                }

                await Task.Yield();
            }
        }

        /// <summary>
        /// Instantiate a prefab asynchronously and await until it is fully ready.
        /// </summary>
        public static async Task<Entity> InstantiateAsync(Prefab p, Vector3? position = null)
        {
            return await WaitUntilReadyAsync(InstantiateNonBlocking(p,
                position));
        }

        public static async Task<Entity> InstantiateAsync(Prefab p, Entity root, Vector3? position = null, bool useRootAsModelRoot = false)
        {
            return await WaitUntilReadyAsync(InstantiateNonBlocking(p, root,
                position, useRootAsModelRoot));
        }

        /// <summary>
        /// Instantiate a prefab nonblocking, keep it presentation-hidden,
        /// run setup once ready, then reveal it if setup succeeds.
        /// </summary>
        public static Entity InstantiatePrepared(Prefab p, Vector3? position =
            null, Func<Entity, Task<bool>>? onReadyAsync = null)
        {
            Entity entity = InstantiateNonBlocking(p, position);
            return StartPreparedInstantiation(entity, onReadyAsync,
                DescribePrefab(p));
        }

        /// <summary>
        /// Instantiate a prefab nonblocking with an explicit root, keep it
        /// presentation-hidden, run setup once ready, then reveal it if setup
        /// succeeds.
        /// </summary>
        public static Entity InstantiatePrepared(Prefab p, Entity root,
            Vector3? position = null, bool useRootAsModelRoot = false,
            Func<Entity, Task<bool>>? onReadyAsync = null)
        {
            Entity entity = InstantiateNonBlocking(p, root, position,
                useRootAsModelRoot);
            return StartPreparedInstantiation(entity, onReadyAsync,
                DescribePrefab(p));
        }

        private static Entity StartPreparedInstantiation(Entity entity,
            Func<Entity, Task<bool>>? onReadyAsync, string description)
        {
            if (!entity.IsValid)
                return new Entity(-1);

            entity.SetPresentationHidden(true);
            RevealPreparedUiSubtree(entity);
            _ = FinalizePreparedInstantiationAsync(entity, onReadyAsync,
                description);
            return entity;
        }

        private static void RevealPreparedUiSubtree(Entity root)
        {
            foreach (Entity child in root.GetChildren())
            {
                RevealPreparedUiSubtreeRecursive(child, false);
            }
        }

        private static void RevealPreparedUiSubtreeRecursive(Entity entity,
            bool uiAncestorVisible)
        {
            bool isUiNode = uiAncestorVisible || HasPreparedUiComponent(entity);
            if (isUiNode)
            {
                entity.SetPresentationHidden(false);
            }

            foreach (Entity child in entity.GetChildren())
            {
                RevealPreparedUiSubtreeRecursive(child, isUiNode);
            }
        }

        private static bool HasPreparedUiComponent(Entity entity)
        {
            return entity.GetComponent<Canvas>() != null ||
                   entity.GetComponent<Panel>() != null ||
                   entity.GetComponent<Text>() != null ||
                   entity.GetComponent<Button>() != null ||
                   entity.GetComponent<Dropdown>() != null ||
                   entity.GetComponent<FitToContent>() != null ||
                   entity.GetComponent<LayoutGroup>() != null ||
                   entity.GetComponent<ProgressBar>() != null ||
                   entity.GetComponent<ScrollView>() != null ||
                   entity.GetComponent<Slider>() != null ||
                   entity.GetComponent<UIRect>() != null ||
                   entity.GetComponent<UISceneCapture>() != null;
        }

        private static async Task FinalizePreparedInstantiationAsync(Entity
            entity, Func<Entity, Task<bool>>? onReadyAsync, string
            description)
        {
            try
            {
                Entity readyEntity = await WaitUntilReadyAsync(entity);
                if (!readyEntity.IsValid || !entity.IsValid)
                {
                    bool wasStillValid = entity.IsValid;
                    DestroyIfValid(entity);
                    if (wasStillValid)
                    {
                        Console.WriteLine(
                            $"[Prefab] Prepared instantiation for " +
                            $"'{description}' did not reach ready state.");
                    }
                    return;
                }

                bool shouldReveal = true;
                if (onReadyAsync != null)
                {
                    shouldReveal = await onReadyAsync(entity);
                }

                if (!entity.IsValid)
                    return;

                if (!shouldReveal)
                {
                    DestroyIfValid(entity);
                    return;
                }

                entity.active = true;
                entity.SetPresentationHidden(false);
            }
            catch (Exception ex)
            {
                DestroyIfValid(entity);
                Console.WriteLine(
                    $"[Prefab] Prepared instantiation for '{description}' " +
                    $"failed: {ex.Message}");
            }
        }

        private static void DestroyIfValid(Entity entity)
        {
            if (entity.IsValid)
            {
                entity.Destroy();
            }
        }

        private static string DescribePrefab(Prefab p)
        {
            string prefabName = p.name;
            if (!string.IsNullOrWhiteSpace(prefabName))
            {
                return prefabName;
            }

            if (!string.IsNullOrWhiteSpace(p.Guid))
            {
                return p.Guid!;
            }

            return "prefab";
        }
    }
}



