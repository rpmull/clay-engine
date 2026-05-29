// GameScripts/ScriptComponent.cs

using ClaymoreEngine;
using ClaymoreEngine.Scripting.Scriptable;
using System;
using System.Collections;
using System.Collections.Generic;
using System.Reflection;

public abstract class ScriptComponent
{
   public int EntityID { get; internal set; }
   public Entity self { get; private set; }

   private Transform? _transform;
   public Transform transform => _transform ??= new Transform(EntityID);

   public virtual void OnCreate() { }
   public virtual void OnUpdate(float dt) { }
   public virtual void OnDestroy() { }
   
   /// <summary>
   /// Called by the inspector when any serialized field is modified.
   /// Override this to respond to inspector changes immediately.
   /// </summary>
   public virtual void OnValidate() { }

   public virtual void Bind(Entity entity)
   {
      EntityID = entity.EntityID;
      self = entity;
      ScriptRegistry.Register(this);
      
      // Process PopulateFromResources attributes
      PopulateResourceFields();
   }
   
   /// <summary>
   /// Populates all List fields marked with [PopulateFromResources] attribute.
   /// </summary>
   private void PopulateResourceFields()
   {
      var type = GetType();
      var fields = type.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
      
      foreach (var field in fields)
      {
         // Check for both plural and singular forms of the attribute
         var populateAttr = field.GetCustomAttribute<PopulateFromResourcesAttribute>();
         var populateAttrSingular = field.GetCustomAttribute<PopulateFromResourceAttribute>();
         
         // Use whichever attribute is present
         bool hasAttribute = populateAttr != null || populateAttrSingular != null;
         string? nameFilter = populateAttr?.NameFilter ?? populateAttrSingular?.NameFilter;
         bool sortByName = populateAttr?.SortByName ?? populateAttrSingular?.SortByName ?? false;
         
         if (!hasAttribute) continue;
         
         // Verify field is a List<T>
         if (!field.FieldType.IsGenericType || field.FieldType.GetGenericTypeDefinition() != typeof(List<>))
         {
            Console.WriteLine($"[ScriptComponent] Warning: {type.Name}.{field.Name} has [PopulateFromResources] but is not a List<T>");
            continue;
         }
         
         // Get element type
         var elementType = field.FieldType.GetGenericArguments()[0];
         
         // Verify element type derives from ClayScriptableObject
         if (!typeof(ClayScriptableObject).IsAssignableFrom(elementType))
         {
            Console.WriteLine($"[ScriptComponent] Warning: {type.Name}.{field.Name} element type {elementType.Name} does not derive from ClayScriptableObject");
            continue;
         }
         
         // Get or create the list instance
         var list = field.GetValue(this) as IList;
         if (list == null)
         {
            // Create new list instance
            list = Activator.CreateInstance(field.FieldType) as IList;
            field.SetValue(this, list);
         }
         
         // Populate from resources
         try
         {
            Resources.PopulateList(list, elementType);
            
            // Apply name filter if specified
            if (!string.IsNullOrEmpty(nameFilter))
            {
               ApplyNameFilter(list, nameFilter);
            }
            
            // Sort if requested
            if (sortByName)
            {
               SortListByName(list);
            }
            
            // Register for automatic updates when resources change
            Resources.RegisterPopulatedField(this, elementType.FullName!, list);
            
            Console.WriteLine($"[ScriptComponent] Populated {type.Name}.{field.Name} with {list.Count} resources");
         }
         catch (Exception ex)
         {
            Console.WriteLine($"[ScriptComponent] Error populating {type.Name}.{field.Name}: {ex.Message}");
         }
      }
   }
   
   private void ApplyNameFilter(IList list, string pattern)
   {
      // Simple wildcard matching: "*pattern*" contains, "pattern*" starts with, "*pattern" ends with
      bool startWildcard = pattern.StartsWith("*");
      bool endWildcard = pattern.EndsWith("*");
      string search = pattern.Trim('*');
      
      for (int i = list.Count - 1; i >= 0; i--)
      {
         if (list[i] is not ClayScriptableObject obj) continue;
         
         // Get resource name from GUID (stored in asset)
         string? name = obj.Guid;
         if (string.IsNullOrEmpty(name)) continue;
         
         bool match = false;
         if (startWildcard && endWildcard)
         {
            match = name.Contains(search, StringComparison.OrdinalIgnoreCase);
         }
         else if (startWildcard)
         {
            match = name.EndsWith(search, StringComparison.OrdinalIgnoreCase);
         }
         else if (endWildcard)
         {
            match = name.StartsWith(search, StringComparison.OrdinalIgnoreCase);
         }
         else
         {
            match = name.Equals(search, StringComparison.OrdinalIgnoreCase);
         }
         
         if (!match)
         {
            list.RemoveAt(i);
         }
      }
   }
   
   private void SortListByName(IList list)
   {
      // Convert to array for sorting
      var array = new object[list.Count];
      list.CopyTo(array, 0);
      
      Array.Sort(array, (a, b) =>
      {
         var nameA = (a as ClayScriptableObject)?.Guid ?? "";
         var nameB = (b as ClayScriptableObject)?.Guid ?? "";
         return string.Compare(nameA, nameB, StringComparison.OrdinalIgnoreCase);
      });
      
      // Clear and repopulate
      list.Clear();
      foreach (var item in array)
      {
         list.Add(item);
      }
   }
}
