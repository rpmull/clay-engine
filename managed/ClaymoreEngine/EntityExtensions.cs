using ClaymoreEngine;
using System;
using System.Collections.Generic;
using System.Reflection;

namespace ClaymoreEngine
{
   public static class EntityExtensions
   {
      private static readonly Dictionary<(int, Type), object> _componentCache = new Dictionary<(int, Type), object>();
      private static readonly Dictionary<(int, Type), object> _moduleComponentCache = new Dictionary<(int, Type), object>();

      /// <summary>
      /// Clears all cached component references. MUST be called when starting/stopping play mode
      /// to prevent stale references to destroyed runtime scene entities.
      /// </summary>
      public static void ClearAllCaches()
      {
         Console.WriteLine($"[EntityExtensions] Clearing component caches ({_componentCache.Count} components, {_moduleComponentCache.Count} modules)");

         // Unregister any AreaComponents / RigidBodyComponents before clearing
         foreach (var kvp in _componentCache)
         {
            if (kvp.Value is AreaComponent area)
            {
               area.UnregisterInstance();
            }
            else if (kvp.Value is RigidBodyComponent rb)
            {
               rb.UnregisterInstance();
            }
            else if (kvp.Value is PortalComponent portal)
            {
               portal.UnregisterInstance();
            }
         }

         _componentCache.Clear();
         _moduleComponentCache.Clear();
         Button.ClearRegistry();
         NavAgentComponent.ClearRegistry();
      }

      public static T AddComponent<T>(this Entity entity) where T : ComponentBase, new()
      {
         var componentName = typeof(T).Name;
         if (ComponentInterop.HasComponent(entity.EntityID, componentName))
         {
            return GetComponent<T>(entity);
         }

         ComponentInterop.AddComponent(entity.EntityID, componentName);
         return GetComponent<T>(entity);
      }

      public static T AddScript<T>(this Entity entity) where T : ScriptComponent
      {
         T existing = ScriptRegistry.Get<T>(entity.EntityID);
         if (existing != null)
         {
            return existing;
         }

         string scriptName = typeof(T).FullName ?? typeof(T).Name;
         if (string.IsNullOrEmpty(scriptName))
         {
            Console.WriteLine("[EntityExtensions] Script type name is null/empty.");
            return null;
         }

         ComponentInterop.AddScript?.Invoke(entity.EntityID, scriptName);
         T script = ScriptRegistry.Get<T>(entity.EntityID);
         if (script == null)
         {
            Console.WriteLine($"[EntityExtensions] Failed to add script '{scriptName}' to entity {entity.EntityID}.");
         }
         return script;
      }

      public static T GetComponent<T>(this Entity entity) where T : ComponentBase, new()
      {
         var key = (entity.EntityID, typeof(T));
         if (_componentCache.TryGetValue(key, out var cachedComponent))
         {
            return (T)cachedComponent;
         }

         var componentName = typeof(T).Name;
         if (!ComponentInterop.HasComponent(entity.EntityID, componentName))
         {
            return null;
         }

         var component = new T { entity = entity };
         if (component is AreaComponent area)
         {
            area.RegisterInstance();
         }
         else if (component is RigidBodyComponent rb)
         {
            rb.RegisterInstance();
         }
         if (component is Button button)
         {
            Button.Register(button);
         }
         _componentCache[key] = component;
         return component;
      }

      public static void RemoveComponent<T>(this Entity entity) where T : ComponentBase
      {
         var key = (entity.EntityID, typeof(T));
         if (_componentCache.ContainsKey(key))
         {
            if (_componentCache[key] is AreaComponent area) area.UnregisterInstance();
            else if (_componentCache[key] is RigidBodyComponent rb) rb.UnregisterInstance();
            else if (_componentCache[key] is PortalComponent portal) portal.UnregisterInstance();
            else if (_componentCache[key] is Button button) Button.Unregister(button);
            _componentCache.Remove(key);
         }

         var componentName = typeof(T).Name;
         ComponentInterop.RemoveComponent(entity.EntityID, componentName);
      }

      /// <summary>
      /// Adds a module component of type T to the entity.
      /// Module components are dynamically loaded from external DLLs and stored in native ModuleComponent storage.
      /// </summary>
      public static T AddModuleComponent<T>(this Entity entity) where T : ModuleComponentBase, new()
      {
         var typeName = typeof(T).FullName;
         if (string.IsNullOrEmpty(typeName))
         {
            Console.WriteLine("[EntityExtensions] Module component type name is null/empty.");
            return null;
         }
         if (ModuleComponentInterop.HasModuleComponent(entity.EntityID, typeName))
         {
            return GetModuleComponent<T>(entity);
         }

         ModuleComponentInterop.AddModuleComponent(entity.EntityID, typeName);
         return GetModuleComponent<T>(entity);
      }

      /// <summary>
      /// Gets a module component of type T from the entity.
      /// Returns the managed instance that synchronizes with native ModuleComponent storage.
      /// </summary>
      public static T GetModuleComponent<T>(this Entity entity) where T : ModuleComponentBase, new()
      {
         var key = (entity.EntityID, typeof(T));
         if (_moduleComponentCache.TryGetValue(key, out var cachedComponent))
         {
            return (T)cachedComponent;
         }

         var typeName = typeof(T).FullName;
         if (string.IsNullOrEmpty(typeName))
         {
            Console.WriteLine("[EntityExtensions] Module component type name is null/empty.");
            return null;
         }
         IntPtr nativePtr = IntPtr.Zero;

         // First try the normal TypeId-based lookup
         if (ModuleComponentInterop.HasModuleComponent(entity.EntityID, typeName))
         {
            nativePtr = ModuleComponentInterop.GetModuleComponent(entity.EntityID, typeName);
         }

         // If that failed, try the fallback by full name
         if (nativePtr == IntPtr.Zero)
         {
            Console.WriteLine($"[EntityExtensions] TypeId lookup failed for {typeName}, trying full name fallback...");
            nativePtr = ModuleComponentInterop.GetModuleComponentByFullName(entity.EntityID, typeName);
         }

         if (nativePtr == IntPtr.Zero)
         {
            Console.WriteLine($"[EntityExtensions] Module Component Not Found: {typeName}");
            return null;
         }

         var component = new T();
         component.InternalInitialize(entity);
         _moduleComponentCache[key] = component;

         return component;
      }

      /// <summary>
      /// Gets a module component by type name. This allows getting components from loaded modules
      /// without knowing the exact type at compile time.
      /// </summary>
      public static ModuleComponentBase GetModuleComponent(this Entity entity, string typeName)
      {
         if (string.IsNullOrEmpty(typeName))
         {
            Console.WriteLine("[EntityExtensions] Module component type name is null/empty.");
            return null;
         }
         // Try to find the type in loaded assemblies
         Type componentType = null;
         foreach (var assembly in AppDomain.CurrentDomain.GetAssemblies())
         {
            componentType = assembly.GetType(typeName);
            if (componentType != null && typeof(ModuleComponentBase).IsAssignableFrom(componentType))
               break;
         }

         if (componentType == null)
         {
            Console.WriteLine($"[EntityExtensions] Could not find module component type: {typeName}");
            return null;
         }

         var key = (entity.EntityID, componentType);
         if (_moduleComponentCache.TryGetValue(key, out var cachedComponent))
         {
            return (ModuleComponentBase)cachedComponent;
         }

         if (!ModuleComponentInterop.HasModuleComponent(entity.EntityID, typeName))
         {
            return null;
         }

         var component = (ModuleComponentBase)Activator.CreateInstance(componentType);
         component.InternalInitialize(entity);
         _moduleComponentCache[key] = component;

         return component;
      }

      /// <summary>
      /// Removes a module component of type T from the entity.
      /// </summary>
      public static void RemoveModuleComponent<T>(this Entity entity) where T : ModuleComponentBase
      {
         var key = (entity.EntityID, typeof(T));
         if (_moduleComponentCache.ContainsKey(key))
         {
            _moduleComponentCache.Remove(key);
         }

         var typeName = typeof(T).FullName;
         if (string.IsNullOrEmpty(typeName))
         {
            Console.WriteLine("[EntityExtensions] Module component type name is null/empty.");
            return;
         }
         ModuleComponentInterop.RemoveModuleComponent(entity.EntityID, typeName);
      }

      /// <summary>
      /// Finds the first component of type T on this entity's children.
      /// </summary>
      public static T FindComponentInChildren<T>(this Entity entity, bool includeSelf = false, bool traverseDeepChildren = false)
         where T : ComponentBase, new()
      {
         if (!entity.IsValid)
            return null;

         if (includeSelf)
         {
            var selfComponent = entity.GetComponent<T>();
            if (selfComponent != null)
               return selfComponent;
         }

         if (!traverseDeepChildren)
         {
            foreach (var child in entity.GetChildren())
            {
               if (!child.IsValid)
                  continue;
               var component = child.GetComponent<T>();
               if (component != null)
                  return component;
            }

            return null;
         }

         var queue = new Queue<Entity>();
         foreach (var child in entity.GetChildren())
            queue.Enqueue(child);

         while (queue.Count > 0)
         {
            var current = queue.Dequeue();
            if (!current.IsValid)
               continue;

            var component = current.GetComponent<T>();
            if (component != null)
               return component;

            foreach (var child in current.GetChildren())
               queue.Enqueue(child);
         }

         return null;
      }

      /// <summary>
      /// Removes a module component by type name.
      /// </summary>
      public static void RemoveModuleComponent(this Entity entity, string typeName)
      {
         if (string.IsNullOrEmpty(typeName))
         {
            Console.WriteLine("[EntityExtensions] Module component type name is null/empty.");
            return;
         }
         // Find and remove from cache
         var keysToRemove = new List<(int, Type)>();
         foreach (var kvp in _moduleComponentCache)
         {
            if (kvp.Key.Item1 == entity.EntityID && kvp.Key.Item2.FullName == typeName)
            {
               keysToRemove.Add(kvp.Key);
            }
         }

         foreach (var key in keysToRemove)
         {
            _moduleComponentCache.Remove(key);
         }

         ModuleComponentInterop.RemoveModuleComponent(entity.EntityID, typeName);
      }

      public static void SetVisible(this Entity entity, bool isVisible)
      {
         EntityInterop.SetVisible(entity.EntityID, isVisible);
      }

      public static void SetPresentationHidden(this Entity entity, bool hidden)
      {
         EntityInterop.SetPresentationHidden(entity.EntityID, hidden);
      }
   }
}
