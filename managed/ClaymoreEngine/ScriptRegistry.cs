// New: ScriptRegistry.cs
using System;
using System.Collections.Generic;

namespace ClaymoreEngine;

public static class ScriptRegistry
   {
   private static readonly Dictionary<(int, Type), ScriptComponent> _byEntityType = new();

   public static void Register(ScriptComponent s) =>
       _byEntityType[(s.EntityID, s.GetType())] = s;

   public static T Get<T>(int entityId) where T : ScriptComponent =>
       _byEntityType.TryGetValue((entityId, typeof(T)), out var s) ? (T)s : null;

   // Non-generic lookup used by interop to resolve by runtime Type
   public static ScriptComponent? Get(Type t, int entityId) =>
        _byEntityType.TryGetValue((entityId, t), out var s) ? s : null;

   public static ScriptComponent? Get(int entityId, string? className)
   {
      foreach (ScriptComponent script in Enumerate(entityId, className))
         return script;

      return null;
   }

   public static IEnumerable<ScriptComponent> Enumerate(int entityId, string? className = null)
   {
      foreach (KeyValuePair<(int, Type), ScriptComponent> entry in _byEntityType)
      {
         if (entry.Key.Item1 != entityId)
            continue;

         Type scriptType = entry.Key.Item2;
         if (!string.IsNullOrWhiteSpace(className) &&
             !string.Equals(scriptType.Name, className, StringComparison.Ordinal) &&
             !string.Equals(scriptType.FullName, className, StringComparison.Ordinal))
         {
            continue;
         }

         yield return entry.Value;
      }
   }

   public static void Unregister(ScriptComponent? s)
   {
      if (s == null)
         return;

      _byEntityType.Remove((s.EntityID, s.GetType()));
   }

   public static void Clear()
   {
      _byEntityType.Clear();
   }

   }

   // New: EntityExtensions for scripts
   public static class ScriptLookupExtensions
      {
      public static T GetScript<T>(this Entity e) where T : ScriptComponent =>
          ScriptRegistry.Get<T>(e.EntityID);
      }
   
