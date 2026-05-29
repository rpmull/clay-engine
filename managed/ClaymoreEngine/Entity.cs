using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Text.RegularExpressions;

namespace ClaymoreEngine
{
    public struct Entity
    {
        public int EntityID { get; private set; }

        public string Name => EntityInterop.GetName(EntityID);

        // Treat IDs <= 0 as invalid; runtime IDs start at 1
        public bool IsValid => EntityID > 0;

        public Entity(int entityID)
        {
            EntityID = entityID;
        }

        public Transform transform => new Transform(EntityID);
        public NpcScalabilityInfo Scalability => NpcScalability.Get(this);

        public static List<Entity> Find(string name, bool useRegex = false, List<Entity> presetList = null, bool traverseDeepChildren = false)
        {
            return FindAllWithName(name, useRegex, presetList, traverseDeepChildren);
        }

        /// <summary>
        /// Finds the first entity in the scene with the given name.
        /// </summary>
        public static Entity FindFirst(string name)
        {
            return new Entity(EntityInterop.FindByName(name));
        }

        /// <summary>
        /// Finds the first entity ID in the scene with the given name.
        /// </summary>
        public static int FindFirstId(string name)
        {
            return EntityInterop.FindByName(name);
        }

        /// <summary>
        /// Finds all entities in the scene that match the given name.
        /// </summary>
        /// <param name="name">The name to search for</param>
        /// <param name="useRegex">If true, treats the name as a regular expression pattern</param>
        /// <returns>A list of all entities matching the name</returns>
        public static List<Entity> FindAllWithName(string name, bool useRegex = false, List<Entity> presetList = null, bool traverseDeepChildren = false)
        {


            List<Entity> results = new List<Entity>();
            if (string.IsNullOrEmpty(name))
                return results;

            int[] entityIds;

            if (presetList != null)
            {
                if (traverseDeepChildren)
                {
                    HashSet<int> visited = new HashSet<int>();
                    Queue<int> queue = new Queue<int>();
                    for (int i = 0; i < presetList.Count; i++)
                    {
                        int id = presetList[i].EntityID;
                        if (id <= 0) continue;
                        if (visited.Add(id)) queue.Enqueue(id);
                    }

                    while (queue.Count > 0)
                    {
                        int current = queue.Dequeue();
                        int[] childIds = EntityInterop.GetChildren(current);
                        for (int i = 0; i < childIds.Length; i++)
                        {
                            int childId = childIds[i];
                            if (childId <= 0) continue;
                            if (visited.Add(childId)) queue.Enqueue(childId);
                        }
                    }

                    entityIds = new int[visited.Count];
                    visited.CopyTo(entityIds);
                }
                else
                {
                    entityIds = new int[presetList.Count];
                    for (int i = 0; i < presetList.Count; i++)
                    {
                        entityIds[i] = presetList[i].EntityID;
                    }
                }
            }
            else
            {
                entityIds = EntityInterop.GetEntities();
            }

            if (useRegex)
            {
                Regex regex;
                try
                {
                    regex = new Regex(name);
                }
                catch
                {
                    // Invalid regex pattern, return empty list
                    return results;
                }

                foreach (var entityId in entityIds)
                {
                    string entityName = EntityInterop.GetName(entityId);
                    if (!string.IsNullOrEmpty(entityName) && regex.IsMatch(entityName))
                    {
                        results.Add(new Entity(entityId));
                    }
                }
            }
            else
            {
                foreach (var entityId in entityIds)
                {
                    string entityName = EntityInterop.GetName(entityId);
                    if (entityName == name)
                    {
                        results.Add(new Entity(entityId));
                    }
                }
            }

            return results;
        }


        public static List<Entity> FindAllWithComponent(
        string componentTypeName,
        List<Entity> presetList = null,
        bool traverseDeepChildren = false)
        {
            var results = new List<Entity>();

            if (string.IsNullOrEmpty(componentTypeName))
                return results;

            int[] entityIds;

            if (presetList != null)
            {
                if (traverseDeepChildren)
                {
                    var visited = new HashSet<int>();
                    var queue = new Queue<int>();

                    // seed from preset list
                    for (int i = 0; i < presetList.Count; i++)
                    {
                        int id = presetList[i].EntityID;
                        if (id <= 0) continue;
                        if (visited.Add(id)) queue.Enqueue(id);
                    }

                    // BFS all descendants
                    while (queue.Count > 0)
                    {
                        int current = queue.Dequeue();
                        int[] childIds = EntityInterop.GetChildren(current);
                        for (int i = 0; i < childIds.Length; i++)
                        {
                            int childId = childIds[i];
                            if (childId <= 0) continue;
                            if (visited.Add(childId)) queue.Enqueue(childId);
                        }
                    }

                    entityIds = new int[visited.Count];
                    visited.CopyTo(entityIds);
                }
                else
                {
                    entityIds = new int[presetList.Count];
                    for (int i = 0; i < presetList.Count; i++)
                        entityIds[i] = presetList[i].EntityID;
                }
            }
            else
            {
                entityIds = EntityInterop.GetEntities();
            }

            for (int i = 0; i < entityIds.Length; i++)
            {
                int entityId = entityIds[i];
                if (entityId <= 0) continue;

                if (ComponentInterop.HasComponent(entityId, componentTypeName))
                    results.Add(new Entity(entityId));
            }

            return results;
        }


        public static List<Entity> FindAllWithComponent<T>(
    List<Entity> presetList = null,
    bool traverseDeepChildren = false) where T : ComponentBase
        {
            return FindAllWithComponent(typeof(T).Name, presetList, traverseDeepChildren);
        }



        /// <summary>
        /// Creates a new entity with the given name.
        /// </summary>
        /// <param name="name">Name for the entity</param>
        /// <returns>The newly created entity</returns>
        public static Entity Create(string name)
        {
            int id = EntityInterop.CreateEntity(name);
            return new Entity(id);
        }

        /// <summary>
        /// Creates a new entity with the given name and optional parent.
        /// </summary>
        /// <param name="name">Name for the entity</param>
        /// <param name="parent">Optional parent entity to attach to</param>
        /// <returns>The newly created entity</returns>
        public static Entity Create(string name, Entity parent)
        {
            int id = EntityInterop.CreateWithParent(name, parent.EntityID);
            return new Entity(id);
        }

        public static Entity GetEntity(int id)
        {
            int entityId = EntityInterop.GetEntityByID(id);
            return new Entity(entityId);
        }

        // ---------------------- Parenting ----------------------

        /// <summary>
        /// Sets the parent of this entity.
        /// </summary>
        /// <param name="parent">The new parent entity</param>
        /// <param name="preserveWorldTransform">If true, maintains world position/rotation/scale</param>
        public void SetParent(Entity parent, bool preserveWorldTransform = false)
        {
            if (!IsValid) return;
            EntityInterop.SetParent(EntityID, parent.EntityID, preserveWorldTransform);
        }

        /// <summary>
        /// Sets the parent of this entity. Passing null moves this entity to the scene root.
        /// </summary>
        /// <param name="parent">The new parent entity, or null to move this entity to the scene root</param>
        /// <param name="preserveWorldTransform">If true, maintains world position/rotation/scale</param>
        public void SetParent(Entity? parent, bool preserveWorldTransform = false)
        {
            if (!IsValid) return;
            int parentId = parent.HasValue && parent.Value.IsValid ? parent.Value.EntityID : -1;
            EntityInterop.SetParent(EntityID, parentId, preserveWorldTransform);
        }

        /// <summary>
        /// Gets the parent of this entity.
        /// </summary>
        /// <returns>The parent entity, or an invalid entity if no parent</returns>
        public Entity GetParent()
        {
            if (!IsValid) return new Entity(-1);
            int parentId = EntityInterop.GetParent(EntityID);
            return new Entity(parentId);
        }

        /// <summary>
        /// Gets the parent entity, or null if no parent.
        /// </summary>
        public Entity? parent
        {
            get
            {
                if (!IsValid) return null;
                int parentId = EntityInterop.GetParent(EntityID);
                if (parentId <= 0) return null;
                return new Entity(parentId);
            }
            set
            {
                if (!IsValid) return;
                int parentId = value.HasValue && value.Value.IsValid ? value.Value.EntityID : -1;
                EntityInterop.SetParent(EntityID, parentId, false);
            }
        }

        /// <summary>
        /// Gets all direct children of this entity.
        /// </summary>
        /// <returns>Array of child entities</returns>
        public Entity[] GetChildren()
        {
            if (!IsValid) return Array.Empty<Entity>();
            int[] childIds = EntityInterop.GetChildren(EntityID);
            Entity[] children = new Entity[childIds.Length];
            for (int i = 0; i < childIds.Length; i++)
                children[i] = new Entity(childIds[i]);
            return children;
        }

        /// <summary>
        /// Gets the number of direct children.
        /// </summary>
        public int childCount
        {
            get
            {
                if (!IsValid) return 0;
                return EntityInterop.GetEntityChildCount?.Invoke(EntityID) ?? 0;
            }
        }

        /// <summary>
        /// Finds a direct child by name.
        /// </summary>
        /// <param name="name">Name of the child to find</param>
        /// <returns>The child entity, or an invalid entity if not found</returns>
        public Entity FindChild(string name)
        {
            if (!IsValid) return new Entity(-1);
            int childId = EntityInterop.FindChild(EntityID, name);
            return new Entity(childId);
        }

        /// <summary>
        /// Recursively searches for a descendant by name (depth-first).
        /// Useful for finding skeleton bones: entity.FindDescendant("Spine")
        /// </summary>
        /// <param name="name">Name of the descendant to find</param>
        /// <returns>The descendant entity, or an invalid entity if not found</returns>
        public Entity FindDescendant(string name)
        {
            if (!IsValid) return new Entity(-1);
            int descendantId = EntityInterop.FindDescendant(EntityID, name);
            return new Entity(descendantId);
        }

        /// <summary>
        /// Checks if this entity is a descendant of the given ancestor entity.
        /// </summary>
        /// <param name="ancestor">The potential ancestor entity</param>
        /// <returns>True if this entity is a descendant (child, grandchild, etc.) of the ancestor</returns>
        public bool IsDescendantOf(Entity ancestor)
        {
            if (!IsValid || !ancestor.IsValid) return false;

            Entity current = this;
            while (current.IsValid)
            {
                Entity? parent = current.parent;
                if (!parent.HasValue) return false;

                if (parent.Value.EntityID == ancestor.EntityID)
                    return true;

                current = parent.Value;
            }

            return false;
        }

        public static Entity FindFirstEntityByType<T>()
        {
            var typeName = typeof(T).Name;
            int[] entityIds = EntityInterop.GetEntities();
            foreach (var entityId in entityIds)
            {
                if (ComponentInterop.HasComponent(entityId, typeName))
                {
                    return new Entity(entityId);
                }
            }
            return new Entity(-1);
        }

        public static bool TryFindFirstEntityByType<T>(out Entity entity)
        {
            var typeName = typeof(T).Name;
            int[] entityIds = EntityInterop.GetEntities();
            foreach (var entityId in entityIds)
            {
                if (ComponentInterop.HasComponent(entityId, typeName))
                {
                    entity = new Entity(entityId);
                    return true;
                }
            }
            entity = new Entity(-1);
            return false;
        }

        public static Entity FindFirstEntityByScript<T>() where T : ScriptComponent
        {
            int[] entityIds = EntityInterop.GetEntities();
            foreach (var entityId in entityIds)
            {
                if (new Entity(entityId).GetScript<T>() != null)
                {
                    return new Entity(entityId);
                }
            }
            return new Entity(-1);
        }

        /// <summary>
        /// Finds and returns the first script of type T in the scene.
        /// </summary>
        /// <typeparam name="T">The script type to find</typeparam>
        /// <returns>The script instance, or null if not found</returns>
        public static T FindFirstScriptOfType<T>() where T : ScriptComponent
        {
            int[] entityIds = EntityInterop.GetEntities();
            foreach (var entityId in entityIds)
            {
                var script = new Entity(entityId).GetScript<T>();
                if (script != null)
                    return script;
            }
            return null;
        }

        /// <summary>
        /// Tries to find the first script of type T in the scene.
        /// </summary>
        /// <typeparam name="T">The script type to find</typeparam>
        /// <param name="script">The found script, or null</param>
        /// <returns>True if a script was found</returns>
        public static bool TryFindFirstScriptOfType<T>(out T script) where T : ScriptComponent
        {
            int[] entityIds = EntityInterop.GetEntities();
            foreach (var entityId in entityIds)
            {
                script = new Entity(entityId).GetScript<T>();
                if (script != null)
                    return true;
            }
            script = null;
            return false;
        }

        /// <summary>
        /// Finds all script instances of type T in the current scene.
        /// </summary>
        /// <typeparam name="T">The script component type to search for</typeparam>
        /// <returns>A list of all script instances of type T</returns>
        public static List<T> FindAllScriptOfType<T>(
           List<Entity> presetList = null,
           bool traverseDeepChildren = false) where T : ScriptComponent
        {
            var results = new List<T>();
            int[] entityIds;

            if (presetList != null)
            {
                if (traverseDeepChildren)
                {
                    var visited = new HashSet<int>();
                    var queue = new Queue<int>();

                    // seed from preset list
                    for (int i = 0; i < presetList.Count; i++)
                    {
                        int id = presetList[i].EntityID;
                        if (id <= 0) continue;
                        if (visited.Add(id)) queue.Enqueue(id);
                    }

                    // BFS all descendants
                    while (queue.Count > 0)
                    {
                        int current = queue.Dequeue();
                        int[] childIds = EntityInterop.GetChildren(current);
                        for (int i = 0; i < childIds.Length; i++)
                        {
                            int childId = childIds[i];
                            if (childId <= 0) continue;
                            if (visited.Add(childId)) queue.Enqueue(childId);
                        }
                    }

                    entityIds = new int[visited.Count];
                    visited.CopyTo(entityIds);
                }
                else
                {
                    entityIds = new int[presetList.Count];
                    for (int i = 0; i < presetList.Count; i++)
                        entityIds[i] = presetList[i].EntityID;
                }
            }
            else
            {
                entityIds = EntityInterop.GetEntities();
            }

            for (int i = 0; i < entityIds.Length; i++)
            {
                int entityId = entityIds[i];
                if (entityId <= 0)
                    continue;

                var script = new Entity(entityId).GetScript<T>();
                if (script != null)
                    results.Add(script);
            }

            return results;
        }

        // Visibility / Active wrappers
        public bool visible { get => EntityInterop.IsVisible(EntityID); set => EntityInterop.SetVisible(EntityID, value); }
        public bool presentationHidden { get => EntityInterop.IsPresentationHidden(EntityID); set => EntityInterop.SetPresentationHidden(EntityID, value); }
        public bool active { get => EntityInterop.IsActive(EntityID); set => EntityInterop.SetActive(EntityID, value); }

        public TaskAwaiter<Entity> GetAwaiter()
        {
            return Prefab.WaitUntilReadyAsync(this).GetAwaiter();
        }

        /// <summary>
        /// Destroys this entity and removes it from the scene.
        /// After calling this, the entity becomes invalid.
        /// </summary>
        public void Destroy()
        {
            if (!IsValid) return;
            if (EntityInterop.IsSceneBeingDestroyed()) return;
            EntityInterop.DestroyEntity(EntityID);
        }

        public override bool Equals(object? obj) => obj is Entity e && e.EntityID == EntityID;
        public override int GetHashCode() => EntityID;

        public static bool operator ==(Entity? left, Entity? right)
        {
            if (ReferenceEquals(left, right))
                return true;
            if (left is null || right is null)
                return false;

            return left?.EntityID == right?.EntityID;
        }

        public static bool operator !=(Entity? left, Entity? right) => !(left == right);

    }
}
