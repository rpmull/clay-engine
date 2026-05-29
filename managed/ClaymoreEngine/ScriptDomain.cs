using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.Loader;
using System.Runtime.InteropServices;
using System.Numerics;

namespace ClaymoreEngine
   {
   public static class ScriptDomain
      {
      private static AssemblyLoadContext? _alc;
      private static Assembly? _scriptsAsm;

      public static readonly List<Type> _allScriptTypes = new();
      public static IEnumerable<Type> AllScriptTypes => _allScriptTypes;

      // -------------------------------------------------------------
      // Reflection property registration (managed -> native)
      // -------------------------------------------------------------
      private enum PropertyType
      {
         Int = 0,
         Float = 1,
         Bool = 2,
         String = 3,
         Vector3 = 4
      }

      [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
      private delegate void RegisterScriptPropertyDelegate(
         [MarshalAs(UnmanagedType.LPStr)] string scriptClass,
         [MarshalAs(UnmanagedType.LPStr)] string propName,
         int propertyType,
         [MarshalAs(UnmanagedType.LPStr)] string defaultValue);

      private static RegisterScriptPropertyDelegate? _registerScriptProperty;

      private static void EnsureRegisterScriptProperty()
      {
         if (_registerScriptProperty != null)
            return;

         if (!NativeLibrary.TryLoad("ClaymoreEngine", out var libHandle))
         {
            Console.WriteLine("[C#] Failed to load ClaymoreEngine.dll for property registration.");
            return;
         }

         if (!NativeLibrary.TryGetExport(libHandle, "RegisterScriptProperty", out var fnPtr))
         {
            Console.WriteLine("[C#] RegisterScriptProperty export not found in native library.");
            return;
         }

         _registerScriptProperty = Marshal.GetDelegateForFunctionPointer<RegisterScriptPropertyDelegate>(fnPtr);
      }

      private static void RegisterSerializedFields(Type scriptType)
      {
         EnsureRegisterScriptProperty();
         if (_registerScriptProperty == null)
            return;

         const BindingFlags flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
         foreach (var field in scriptType.GetFields(flags))
         {
            if (field.GetCustomAttribute<SerializeField>() == null)
               continue;

            PropertyType pType;
            string defaultVal = "";

            // Try to instantiate a temporary instance to read the authored default initializer
            object? tmpInstance = null;
            try { tmpInstance = Activator.CreateInstance(scriptType); }
            catch { tmpInstance = null; }

            if (field.FieldType == typeof(int))
            {
               pType = PropertyType.Int;
               if (tmpInstance != null)
               {
                  try { defaultVal = ((int)(field.GetValue(tmpInstance) ?? 0)).ToString(System.Globalization.CultureInfo.InvariantCulture); }
                  catch { defaultVal = "0"; }
               }
            }
            else if (field.FieldType == typeof(float))
            {
               pType = PropertyType.Float;
               if (tmpInstance != null)
               {
                  try { defaultVal = ((float)(field.GetValue(tmpInstance) ?? 0f)).ToString(System.Globalization.CultureInfo.InvariantCulture); }
                  catch { defaultVal = "0"; }
               }
            }
            else if (field.FieldType == typeof(bool))
            {
               pType = PropertyType.Bool;
               if (tmpInstance != null)
               {
                  try { defaultVal = (((bool)(field.GetValue(tmpInstance) ?? false)) ? "true" : "false"); }
                  catch { defaultVal = "false"; }
               }
            }
            else if (field.FieldType == typeof(string))
            {
               pType = PropertyType.String;
               if (tmpInstance != null)
               {
                  try { defaultVal = (field.GetValue(tmpInstance) as string) ?? string.Empty; }
                  catch { defaultVal = string.Empty; }
               }
            }
            else if (field.FieldType == typeof(Vector3))
            {
               pType = PropertyType.Vector3;
               if (tmpInstance != null)
               {
                  try {
                     var v = (Vector3)(field.GetValue(tmpInstance) ?? default(Vector3));
                     defaultVal = string.Create(System.Globalization.CultureInfo.InvariantCulture, $"{v.X},{v.Y},{v.Z}");
                  } catch { defaultVal = "0,0,0"; }
               }
            }
            else
               continue; // Unsupported type

            _registerScriptProperty(scriptType.FullName!, field.Name, (int)pType, defaultVal);
         }
      }

      /// <summary>
      /// Loads the specified GameScripts DLL into a collectible context and registers all valid ScriptComponent types.
      /// </summary>
      public static void LoadScripts(string dllPath, RegisterScriptCallback? registerCallback = null)
         {
         if (!File.Exists(dllPath))
            throw new FileNotFoundException("GameScripts.dll not found", dllPath);

         // Unload old domain if present
         UnloadDomain();

         string scriptsDir = Path.GetDirectoryName(dllPath)!;
         _alc = new AssemblyLoadContext("GameScriptsDomain", isCollectible: true);

         // Resolve missing dependencies from the GameScripts folder
         _alc.Resolving += (context, asmName) =>
         {
            string candidate = Path.Combine(scriptsDir, $"{asmName.Name}.dll");
            if (File.Exists(candidate))
               {
               Console.WriteLine($"[C#] Resolving: {asmName} => {candidate}");
               return context.LoadFromAssemblyPath(candidate);
               }

            Console.WriteLine($"[C#] Failed to resolve: {asmName}");
            return null;
         };

         // Make sure ClaymoreEngine can be reused from default load context
         AssemblyLoadContext.Default.Resolving += ResolveFromDefault;

         try
            {
            string fullPath = Path.GetFullPath(dllPath);
            string pdbPath = Path.ChangeExtension(fullPath, ".pdb");
            using FileStream dllStream = new FileStream(fullPath,
                                                        FileMode.Open,
                                                        FileAccess.Read,
                                                        FileShare.ReadWrite | FileShare.Delete);

            if (File.Exists(pdbPath))
               {
               try
                  {
                  using FileStream pdbStream = new FileStream(pdbPath,
                                                              FileMode.Open,
                                                              FileAccess.Read,
                                                              FileShare.ReadWrite | FileShare.Delete);
                  _scriptsAsm = _alc.LoadFromStream(dllStream, pdbStream);
                  Console.WriteLine($"[C#] Loaded scripts with symbols: {Path.GetFileName(pdbPath)}");
                  }
               catch (Exception symbolEx)
                  {
                  Console.WriteLine($"[C#] Failed to load symbols from {pdbPath}: {symbolEx.Message}");
                  dllStream.Position = 0;
                  _scriptsAsm = _alc.LoadFromStream(dllStream);
                  }
               }
            else
               {
               _scriptsAsm = _alc.LoadFromStream(dllStream);
               }


            Console.WriteLine($"[C#] Loaded scripts: {_scriptsAsm.FullName}");

         _allScriptTypes.Clear();

         foreach (Type type in _scriptsAsm.GetTypes())
            {
            if (type.IsAbstract || !typeof(ScriptComponent).IsAssignableFrom(type))
               continue;

            Console.WriteLine($"[C#] Registering: {type.FullName}");
            _allScriptTypes.Add(type);

            // -------------------------------------------------------------
            // 1. Notify native about the new script type (with [Priority] for OnCreate ordering)
            // -------------------------------------------------------------
            registerCallback?.Invoke(type.FullName!, GetEffectivePriority(type));
            uint flags = (uint)ScriptTypeFlags.None;
            if (HasDontDestroyOnLoadAttribute(type))
               flags |= (uint)ScriptTypeFlags.DontDestroyOnLoad;
            if (HasBakeInWorldGraphAttribute(type))
               flags |= (uint)ScriptTypeFlags.BakeInWorldGraph;
            if (HasPreAnimationUpdateAttribute(type))
               flags |= (uint)ScriptTypeFlags.PreAnimationUpdate;
            if (flags != (uint)ScriptTypeFlags.None)
               InteropProcessor.RegisterScriptFlags(type.FullName!, flags);

            // -------------------------------------------------------------
            // 2. Scan for [SerializeField] fields and register them so that
            //    the native Inspector can expose them.
            // -------------------------------------------------------------
            RegisterSerializedFields(type);
            }

         Console.WriteLine($"[C#] Total script types loaded: {_allScriptTypes.Count}");
            }
         catch (ReflectionTypeLoadException rtle)
            {
            Console.WriteLine($"[C#] ReflectionTypeLoadException: {rtle.Message}");
            foreach (var ex in rtle.LoaderExceptions)
               {
               Console.WriteLine($"  [LoaderException] {ex?.Message}");
               }

            throw;
            }
         catch (Exception ex)
            {
            Console.WriteLine($"[C#] Failed to load GameScripts: {ex}");
            throw;
            }
         }

      public static void ReapplyScriptFlags()
         {
         foreach (Type type in _allScriptTypes)
            {
            if (type == null || type.FullName == null)
               continue;

            uint flags = (uint)ScriptTypeFlags.None;
            if (HasDontDestroyOnLoadAttribute(type))
               flags |= (uint)ScriptTypeFlags.DontDestroyOnLoad;
            if (HasBakeInWorldGraphAttribute(type))
               flags |= (uint)ScriptTypeFlags.BakeInWorldGraph;
            if (HasPreAnimationUpdateAttribute(type))
               flags |= (uint)ScriptTypeFlags.PreAnimationUpdate;
            if (flags != (uint)ScriptTypeFlags.None)
               InteropProcessor.RegisterScriptFlags(type.FullName, flags);
            }
         }

      private static bool HasDontDestroyOnLoadAttribute(Type type)
         {
         if (type.GetCustomAttribute<DontDestroyOnLoadAttribute>() != null)
            return true;

         // Handle cases where ClaymoreEngine is loaded in another ALC
         foreach (var attr in type.GetCustomAttributes())
            {
            var attrType = attr.GetType();
            if (attrType.Name == nameof(DontDestroyOnLoadAttribute))
               return true;
            if (attrType.FullName == "ClaymoreEngine.DontDestroyOnLoadAttribute")
               return true;
            }

         return false;
         }

      private static bool HasBakeInWorldGraphAttribute(Type type)
         {
         if (type.GetCustomAttribute<BakeInWorldGraphAttribute>() != null)
            return true;

         foreach (var attr in type.GetCustomAttributes())
            {
            var attrType = attr.GetType();
            if (attrType.Name == nameof(BakeInWorldGraphAttribute))
               return true;
            if (attrType.FullName == "ClaymoreEngine.BakeInWorldGraphAttribute")
               return true;
            }

         return false;
         }

      private static bool HasPreAnimationUpdateAttribute(Type type)
         {
         if (type.GetCustomAttribute<PreAnimationUpdateAttribute>() != null)
            return true;

         foreach (var attr in type.GetCustomAttributes())
            {
            var attrType = attr.GetType();
            if (attrType.Name == nameof(PreAnimationUpdateAttribute))
               return true;
            if (attrType.FullName == "ClaymoreEngine.PreAnimationUpdateAttribute")
               return true;
            }

         return false;
         }

      /// <summary>
      /// Attempt to resolve shared assemblies from default context (e.g., ClaymoreEngine).
      /// </summary>
      private static Assembly? ResolveFromDefault(AssemblyLoadContext context, AssemblyName assemblyName)
         {
         if (assemblyName.Name == "ClaymoreEngine")
            {
            var loaded = AppDomain.CurrentDomain.GetAssemblies()
                .FirstOrDefault(a => a.GetName().Name == "ClaymoreEngine");

            if (loaded != null)
               {
               Console.WriteLine($"[C#] Resolved ClaymoreEngine from default context");
               return loaded;
               }

            Console.WriteLine($"[C#] Failed to resolve ClaymoreEngine from default context");
            }

         return null;
         }

      /// <summary>
      /// Clean up previously loaded context and assemblies.
      /// </summary>
      private static void UnloadDomain()
         {
         if (_alc != null)
            {
            Console.WriteLine("[C#] Unloading previous script domain...");
            _scriptsAsm = null;

            // Clear ClayScriptableObject cache - cached objects have types from the old assembly
            // and won't be compatible with the new assembly's types
            Scripting.Scriptable.ClayScriptableObjectLoader.ClearCache();
            Console.WriteLine("[C#] Cleared ClayScriptableObject cache for hot reload.");

            _alc.Unload();
            _alc = null;

            GC.Collect();
            GC.WaitForPendingFinalizers();
            Console.WriteLine("[C#] Domain unloaded.");
            }
         }

      /// <summary>
      /// Gets the effective [Priority] for OnCreate ordering. Derived type wins; otherwise inherits from base.
      /// </summary>
      public static int GetEffectivePriority(Type scriptType)
         {
         if (scriptType == null || !typeof(ScriptComponent).IsAssignableFrom(scriptType))
            return 0;
         var attr = scriptType.GetCustomAttribute<PriorityAttribute>(inherit: true);
         if (attr != null)
            return attr.Value;
         if (scriptType.BaseType != null && typeof(ScriptComponent).IsAssignableFrom(scriptType.BaseType))
            return GetEffectivePriority(scriptType.BaseType);
         return 0;
         }

      /// <summary>
      /// Attempts to resolve a script type by full name.
      /// </summary>
      public static Type? ResolveType(string name)
         {
         // Try exact full name first
         var type = _scriptsAsm?.GetType(name, throwOnError: false, ignoreCase: false);
         if (type != null)
            return type;

         // Fallback: try to find by short name (case-insensitive)
         return _allScriptTypes.FirstOrDefault(t => string.Equals(t.Name, name, StringComparison.OrdinalIgnoreCase));
         }
      }
      }
