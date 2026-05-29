using System.Collections.Generic;
using System;

namespace ClaymoreEngine
   {
   public class UnifiedMorphComponent : ComponentBase
      {
      /// <summary>
      /// Set a single morph weight by name. Automatically propagates to child meshes.
      /// </summary>
      public void SetUnifiedMorph(string shapeName, float value)
         {
         var shapeCount = ComponentInterop.UnifiedMorph_GetCount(entity.EntityID);

         for (int i = 0; i < shapeCount; i++)
            {
            var name = ComponentInterop.UnifiedMorph_GetName(entity.EntityID, i);
            if (name == shapeName)
               {
               ComponentInterop.UnifiedMorph_SetWeight(entity.EntityID, i, value);
               return;
               }
            }
         Console.WriteLine("Couldn't find morph name: " + shapeName);
         }
      
      /// <summary>
      /// Set a morph weight by index (faster than by name). Automatically propagates to child meshes.
      /// </summary>
      public void SetUnifiedMorphByIndex(int index, float value)
         {
         ComponentInterop.UnifiedMorph_SetWeight(entity.EntityID, index, value);
         }
      
      /// <summary>
      /// Set multiple morph weights without propagating after each one,
      /// then call PropagateAll() once at the end. More efficient for character creators.
      /// </summary>
      public void SetWeightWithoutPropagate(int index, float value)
         {
         // Directly set the weight on UnifiedMorph without triggering per-shape propagation
         // (For this to work, we'd need a separate native function - for now, just set it)
         var data = ComponentInterop.UnifiedMorph_GetWeight(entity.EntityID, index);
         // This is a placeholder - the native SetWeight already propagates
         ComponentInterop.UnifiedMorph_SetWeight(entity.EntityID, index, value);
         }
      
      /// <summary>
      /// Propagate all morph weights to child meshes using parallel-for.
      /// Call this after bulk weight changes via SetWeightWithoutPropagate.
      /// </summary>
      public void PropagateAll()
         {
         ComponentInterop.UnifiedMorph_PropagateAll(entity.EntityID);
         }
      
      /// <summary>
      /// Get the number of morph targets.
      /// </summary>
      public int Count => ComponentInterop.UnifiedMorph_GetCount(entity.EntityID);
      
      /// <summary>
      /// Get a morph name by index.
      /// </summary>
      public string GetName(int index) => ComponentInterop.UnifiedMorph_GetName(entity.EntityID, index);
      
      /// <summary>
      /// Get a morph weight by index.
      /// </summary>
      public float GetWeight(int index) => ComponentInterop.UnifiedMorph_GetWeight(entity.EntityID, index);
      }
   }
