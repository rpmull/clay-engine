using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Runtime.CompilerServices;
using System.Reflection;
using System.Numerics;
using System.Text.Json;

namespace ClaymoreEngine
{
    public static class InteropExports
    {
        private static readonly Dictionary<IntPtr, GCHandle> _liveHandles = new();
        private static readonly object _liveHandlesLock = new();
        private static readonly HashSet<IntPtr> _faultedOnUpdateHandles = new();
        private const bool DisableFaultingOnUpdateScripts = true;

        private static bool TryGetLiveTarget(IntPtr handle, out object? target)
        {
            target = null;
            if (handle == IntPtr.Zero) return false;
            lock (_liveHandlesLock)
            {
                if (!_liveHandles.TryGetValue(handle, out var gch))
                    return false;
                try
                {
                    target = gch.Target;
                    return target != null;
                }
                catch
                {
                    return false;
                }
            }
        }

        private static bool TryRemoveLiveHandle(IntPtr handle, out GCHandle gch)
        {
            gch = default;
            if (handle == IntPtr.Zero) return false;
            lock (_liveHandlesLock)
            {
                if (!_liveHandles.TryGetValue(handle, out gch))
                    return false;
                _liveHandles.Remove(handle);
                return true;
            }
        }

        private static bool IsOnUpdateFaulted(IntPtr handle)
        {
            if (handle == IntPtr.Zero) return false;
            lock (_liveHandlesLock)
            {
                return _faultedOnUpdateHandles.Contains(handle);
            }
        }

        private static void MarkOnUpdateFaulted(IntPtr handle)
        {
            if (handle == IntPtr.Zero) return;
            lock (_liveHandlesLock)
            {
                _faultedOnUpdateHandles.Add(handle);
            }
        }

        private static void ClearOnUpdateFault(IntPtr handle)
        {
            if (handle == IntPtr.Zero) return;
            lock (_liveHandlesLock)
            {
                _faultedOnUpdateHandles.Remove(handle);
            }
        }

        private static void ClearAllOnUpdateFaults()
        {
            lock (_liveHandlesLock)
            {
                _faultedOnUpdateHandles.Clear();
            }
        }

        private static void SafeManagedLogError(string message)
        {
            try
            {
                ManagedLogger.LogError(message);
            }
            catch (Exception logEx)
            {
                try
                {
                    Console.WriteLine($"[C#] [ManagedLoggerFallback] {message}");
                    Console.WriteLine($"[C#] [ManagedLoggerFallback] logging failed: {logEx.GetType().Name}: {logEx.Message}");
                }
                catch
                {
                    // Intentionally swallow as a last resort.
                }
            }
        }

        private static void SafeManagedLogException(string context, Exception ex)
        {
            SafeManagedLogError($"{context}: {ex.GetType().Name}: {ex.Message}");
            SafeManagedLogError($"Exception detail:\n{ex}");
        }

        // ------------------ Reflection interop ------------------
        private enum NativePropertyType
        {
            Int = 0,
            Float = 1,
            Bool = 2,
            String = 3,
            Vector3 = 4,
            Entity = 5,
            ComponentRef = 6,
            ScriptRef = 7,
            Prefab = 8,          // Asset reference to .prefab files (stored as GUID string)
            Enum = 9,            // Enum type (stores int value, displays dropdown)
            List = 10,           // List<T> (expandable list UI)
            Struct = 11,         // Serializable struct (expandable fields)
            ClayObject = 12,     // Reference to a ClayScriptableObject asset
            Mesh = 13,           // Asset reference to mesh files (stored as "GUID:fileID" string)
            Dictionary = 14,     // Dictionary<TKey, TValue> (table UI)
            DialogueLibrary = 15, // Reference to dialogue library asset (.dlglib files)
            AnimationController = 16, // Asset reference to .animctrl files (stored as VFS path)
            AnimationControllerOverride = 17, // Asset reference to .animoverride files (stored as VFS path)
            Texture = 18,        // Asset reference to texture files (stored as "GUID:fileID" string)
            Audio = 19           // Asset reference to audio files (stored as GUID string)
        }

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void RegisterPropDelegate([MarshalAs(UnmanagedType.LPStr)] string klass,
                                                  [MarshalAs(UnmanagedType.LPStr)] string field,
                                                  int propType,
                                                  IntPtr boxedDefault,
                                                  [MarshalAs(UnmanagedType.LPStr)] string? auxTypeFullName);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate bool GetFieldDelegate(IntPtr handle, [MarshalAs(UnmanagedType.LPStr)] string field, int propertyType, IntPtr boxedOut);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void RegisterPropExtendedDelegate([MarshalAs(UnmanagedType.LPStr)] string klass,
                                                          [MarshalAs(UnmanagedType.LPStr)] string field,
                                                          int propType,
                                                          IntPtr boxedDefault,
                                                          [MarshalAs(UnmanagedType.LPStr)] string? auxTypeFullName,
                                                          [MarshalAs(UnmanagedType.LPStr)] string? enumNames,
                                                          [MarshalAs(UnmanagedType.LPStr)] string? enumValues,
                                                          int listElementType,
                                                          [MarshalAs(UnmanagedType.LPStr)] string? listElementTypeName,
                                                          [MarshalAs(UnmanagedType.LPStr)] string? structFieldsJson,
                                                          [MarshalAs(UnmanagedType.U1)] bool populateFromResources,
                                                          [MarshalAs(UnmanagedType.U1)] bool selectFromResources);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void ClearScriptPropertiesDelegate([MarshalAs(UnmanagedType.LPStr)] string className);

        // Native side passes pointer to this delegate so managed can invoke it without P/Invoke
        private static RegisterPropDelegate? _registerPropNative;

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetFieldDelegate(IntPtr scriptHandle,
                                              [MarshalAs(UnmanagedType.LPStr)] string field,
                                              IntPtr boxedValue);

        private static NativePropertyType ToNative(Type t)
        {
            if (t == typeof(int)) return NativePropertyType.Int;
            if (t == typeof(float)) return NativePropertyType.Float;
            if (t == typeof(bool)) return NativePropertyType.Bool;
            if (t == typeof(string)) return NativePropertyType.String;
            if (t == typeof(System.Numerics.Vector3)) return NativePropertyType.Vector3;
            if (t == typeof(Entity)) return NativePropertyType.Entity;
            if (t == typeof(Prefab)) return NativePropertyType.Prefab;
            if (t == typeof(Mesh)) return NativePropertyType.Mesh;
            if (t == typeof(Texture)) return NativePropertyType.Texture;
            if (t == typeof(AudioClip)) return NativePropertyType.Audio;
            if (t == typeof(AnimationController)) return NativePropertyType.AnimationController;
            if (t == typeof(AnimationControllerOverride)) return NativePropertyType.AnimationControllerOverride;
            // DialogueLibraryRef - special struct for dialogue library references
            if (t.Name == "DialogueLibraryRef" || t.FullName == "Dialogue.DialogueLibraryRef")
                return NativePropertyType.DialogueLibrary;
            // Component reference: any type derived from ComponentBase
            if (typeof(ComponentBase).IsAssignableFrom(t)) return NativePropertyType.ComponentRef;
            // Script reference: any type derived from ScriptComponent
            if (typeof(global::ScriptComponent).IsAssignableFrom(t)) return NativePropertyType.ScriptRef;
            // ResourceRef<T> - treated as ClayObject with dropdown UI
            if (t.IsGenericType && t.GetGenericTypeDefinition() == typeof(ResourceRef<>))
                return NativePropertyType.ClayObject;
            // ClayScriptableObject asset reference
            if (typeof(Scripting.Scriptable.ClayScriptableObject).IsAssignableFrom(t)) return NativePropertyType.ClayObject;
            // Enum types
            if (t.IsEnum) return NativePropertyType.Enum;
            // Generic List<T>
            if (t.IsGenericType && t.GetGenericTypeDefinition() == typeof(List<>)) return NativePropertyType.List;
            // Serializable struct (value type with SerializableStruct attribute or public fields)
            if (t.IsValueType && !t.IsPrimitive && t != typeof(decimal) && t != typeof(System.Numerics.Vector3))
            {
                return NativePropertyType.Struct;
            }
            return NativePropertyType.Int;
        }
        
        // Get the element type for List<T> or null if not a list
        private static Type? GetListElementType(Type t)
        {
            if (t.IsGenericType && t.GetGenericTypeDefinition() == typeof(List<>))
                return t.GetGenericArguments()[0];
            return null;
        }
        
        private static IEnumerable<FieldInfo> GetSerializableStructFields(Type t)
        {
            foreach (var field in t.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic))
            {
                if (field.IsPublic || field.GetCustomAttribute<SerializeField>() != null)
                {
                    yield return field;
                }
            }
        }

        private static bool IsStructuredListElementType(Type elementType)
        {
            return ToNative(elementType) == NativePropertyType.Struct;
        }

        private static string BuildStructFieldsJson(Type structType)
        {
            return JsonSerializer.Serialize(BuildStructFieldArray(structType));
        }

        private static List<Dictionary<string, object?>> BuildStructFieldArray(Type structType)
        {
            var fields = new List<Dictionary<string, object?>>();
            foreach (var field in GetSerializableStructFields(structType))
            {
                Type fieldType = field.FieldType;
                NativePropertyType nativeType = ToNative(fieldType);

                var entry = new Dictionary<string, object?>
                {
                    ["name"] = field.Name,
                    ["type"] = (int)nativeType,
                    ["aux"] = GetAuxTypeName(fieldType, nativeType)
                };

                if (nativeType == NativePropertyType.Enum)
                {
                    entry["enumNames"] = Enum.GetNames(fieldType);
                    entry["enumValues"] = Enum.GetValues(fieldType).Cast<int>().ToArray();
                }

                if (nativeType == NativePropertyType.List)
                {
                    Type? elementType = GetListElementType(fieldType);
                    if (elementType != null)
                    {
                        NativePropertyType elementNativeType = ToNative(elementType);
                        entry["listElementType"] = (int)elementNativeType;
                        entry["listElementTypeName"] = elementType.FullName;

                        if (elementNativeType == NativePropertyType.Enum)
                        {
                            entry["enumNames"] = Enum.GetNames(elementType);
                            entry["enumValues"] = Enum.GetValues(elementType).Cast<int>().ToArray();
                        }

                        if (elementNativeType == NativePropertyType.Struct)
                        {
                            entry["structFields"] = BuildStructFieldArray(elementType);
                        }
                    }
                }
                else if (nativeType == NativePropertyType.Struct)
                {
                    entry["structFields"] = BuildStructFieldArray(fieldType);
                }

                if (field.GetCustomAttribute<PopulateFromResourcesAttribute>() != null ||
                    field.GetCustomAttribute<PopulateFromResourceAttribute>() != null)
                {
                    entry["populateFromResources"] = true;
                }

                bool isResourceRef = fieldType.IsGenericType && fieldType.GetGenericTypeDefinition() == typeof(ResourceRef<>);
                if (isResourceRef ||
                    field.GetCustomAttribute<SelectFromResourcesAttribute>() != null ||
                    field.GetCustomAttribute<SelectFromResourceAttribute>() != null)
                {
                    entry["selectFromResources"] = true;
                }

                fields.Add(entry);
            }

            return fields;
        }

        private static string? GetAuxTypeName(Type type, NativePropertyType nativeType)
        {
            if (nativeType == NativePropertyType.Enum ||
                nativeType == NativePropertyType.ComponentRef ||
                nativeType == NativePropertyType.ScriptRef ||
                nativeType == NativePropertyType.Struct ||
                nativeType == NativePropertyType.DialogueLibrary ||
                nativeType == NativePropertyType.AnimationController ||
                nativeType == NativePropertyType.AnimationControllerOverride)
            {
                return type.FullName;
            }

            if (type.IsGenericType && type.GetGenericTypeDefinition() == typeof(ResourceRef<>))
            {
                return type.GetGenericArguments()[0].FullName;
            }

            if (nativeType == NativePropertyType.ClayObject)
            {
                return type.FullName;
            }

            return null;
        }
        
        // Get enum values and names for dropdown display
        private static (string[] names, int[] values) GetEnumInfo(Type t)
        {
            var names = Enum.GetNames(t);
            var values = Enum.GetValues(t).Cast<int>().ToArray();
            return (names, values);
        }

        // Recursively registers each public field of a serializable struct as a
        // flat "prefix.leaf" property. Handles nested structs (and Vector2, which
        // is a struct of X/Y floats) by recursing. Boxed defaults are read from the
        // supplied struct value so field initializers are preserved.
        private static void RegisterStructLeaves(string className, string prefix,
            Type structType, object? structValue, int depth)
        {
            if (depth > 6) return; // guard against pathological/self-referential structs
            foreach (var sub in structType.GetFields(BindingFlags.Instance | BindingFlags.Public))
            {
                Type ft = sub.FieldType;
                NativePropertyType nType = ToNative(ft);
                object? def = null;
                try { def = structValue != null ? sub.GetValue(structValue) : null; } catch { def = null; }
                string fullName = prefix + "." + sub.Name;

                if (nType == NativePropertyType.Struct)
                {
                    object? nestedVal = def;
                    if (nestedVal == null && ft.IsValueType)
                    {
                        try { nestedVal = Activator.CreateInstance(ft); } catch { nestedVal = null; }
                    }
                    RegisterStructLeaves(className, fullName, ft, nestedVal, depth + 1);
                    continue;
                }

                IntPtr boxedPtr = BoxLeafDefault(ft, def, nType,
                    out string? aux, out string? enumNames, out string? enumValues);
                if (_registerPropExtendedNative != null)
                {
                    _registerPropExtendedNative(className, fullName, (int)nType, boxedPtr, aux,
                        enumNames, enumValues, 0, null, null, false, false);
                }
                else
                {
                    _registerPropNative?.Invoke(className, fullName, (int)nType, boxedPtr, aux);
                }
                if (boxedPtr != IntPtr.Zero) Marshal.FreeHGlobal(boxedPtr);
            }
        }

        // Boxes a primitive leaf default to unmanaged memory for registration.
        // Mirrors the inline boxing used for top-level fields, for the common leaf
        // types found inside structs. Returns IntPtr.Zero for unsupported types.
        private static IntPtr BoxLeafDefault(Type ft, object? def, NativePropertyType nType,
            out string? aux, out string? enumNames, out string? enumValues)
        {
            aux = null; enumNames = null; enumValues = null;
            try
            {
                if (ft == typeof(int)) { IntPtr p = Marshal.AllocHGlobal(sizeof(int)); Marshal.WriteInt32(p, def is int i ? i : 0); return p; }
                if (ft == typeof(float)) { IntPtr p = Marshal.AllocHGlobal(sizeof(float)); float f = def is float fv ? fv : 0f; Marshal.StructureToPtr(f, p, false); return p; }
                if (ft == typeof(bool)) { IntPtr p = Marshal.AllocHGlobal(1); Marshal.WriteByte(p, (byte)((def is bool b && b) ? 1 : 0)); return p; }
                if (ft == typeof(string)) { return Marshal.StringToHGlobalAnsi(def as string ?? string.Empty); }
                if (ft == typeof(System.Numerics.Vector3)) { IntPtr p = Marshal.AllocHGlobal(Marshal.SizeOf<System.Numerics.Vector3>()); var v = def is System.Numerics.Vector3 vv ? vv : default; Marshal.StructureToPtr(v, p, false); return p; }
                if (ft == typeof(Entity)) { IntPtr p = Marshal.AllocHGlobal(sizeof(int)); Marshal.WriteInt32(p, def is Entity e ? e.EntityID : -1); return p; }
                if (ft.IsEnum) { aux = ft.FullName; enumNames = string.Join("|", Enum.GetNames(ft)); enumValues = string.Join("|", Enum.GetValues(ft).Cast<int>()); IntPtr p = Marshal.AllocHGlobal(sizeof(int)); Marshal.WriteInt32(p, def != null ? (int)def : 0); return p; }
            }
            catch { }
            return IntPtr.Zero;
        }

        // Sets a nested struct field via a dotted path ("parent.leaf" or deeper).
        // Structs are value types, so the chain is read into boxed copies, the leaf
        // is set, and the copies are written back up to the owning instance.
        private static void SetNestedManagedField(object root, string path, IntPtr boxed)
        {
            string[] parts = path.Split('.');
            object?[] chain = new object?[parts.Length];
            FieldInfo?[] fields = new FieldInfo?[parts.Length];
            chain[0] = root;
            for (int i = 0; i < parts.Length - 1; i++)
            {
                if (chain[i] == null) return;
                fields[i] = chain[i]!.GetType().GetField(parts[i],
                    BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
                if (fields[i] == null) return;
                object? next = fields[i]!.GetValue(chain[i]);
                if (next == null)
                {
                    Type nt = fields[i]!.FieldType;
                    if (!nt.IsValueType) return;
                    next = Activator.CreateInstance(nt);
                }
                chain[i + 1] = next;
            }

            object? leafOwner = chain[parts.Length - 1];
            if (leafOwner == null) return;
            FieldInfo? leaf = leafOwner.GetType().GetField(parts[parts.Length - 1],
                BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
            if (leaf == null) return;
            object? leafValue = UnboxLeafValue(leaf.FieldType, boxed);
            if (leafValue == null) return;
            leaf.SetValue(leafOwner, leafValue);

            // Write boxed value-type copies back up the chain.
            for (int i = parts.Length - 1; i > 0; i--)
            {
                fields[i - 1]!.SetValue(chain[i - 1], chain[i]);
            }
        }

        // Reads a nested struct field via a dotted path. Read-only (no copy-back).
        private static object? GetNestedManagedFieldValue(object root, string path, out Type? leafType)
        {
            leafType = null;
            string[] parts = path.Split('.');
            object? cur = root;
            FieldInfo? fi = null;
            for (int i = 0; i < parts.Length; i++)
            {
                if (cur == null) return null;
                fi = cur.GetType().GetField(parts[i],
                    BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
                if (fi == null) return null;
                cur = fi.GetValue(cur);
            }
            leafType = fi?.FieldType;
            return cur;
        }

        // Converts a boxed native pointer to a managed leaf value for the given type.
        private static object? UnboxLeafValue(Type ft, IntPtr boxed)
        {
            if (ft == typeof(int)) return Marshal.ReadInt32(boxed);
            if (ft == typeof(float)) return Marshal.PtrToStructure<float>(boxed);
            if (ft == typeof(bool)) return Marshal.ReadByte(boxed) != 0;
            if (ft == typeof(string)) return Marshal.PtrToStringAnsi(boxed);
            if (ft == typeof(System.Numerics.Vector3)) return Marshal.PtrToStructure<System.Numerics.Vector3>(boxed);
            if (ft == typeof(Entity)) return new Entity(Marshal.ReadInt32(boxed));
            if (ft.IsEnum) return Enum.ToObject(ft, Marshal.ReadInt32(boxed));
            return null;
        }

        private static bool LooksLikeJsonPayload(string? value)
        {
            if (string.IsNullOrWhiteSpace(value))
            {
                return false;
            }

            for (int i = 0; i < value.Length; i++)
            {
                char c = value[i];
                if (char.IsWhiteSpace(c))
                {
                    continue;
                }

                return c == '[' || c == '{';
            }

            return false;
        }

        private static object? SerializeStructuredValue(object? value, Type declaredType)
        {
            if (value == null)
            {
                return null;
            }

            if (declaredType == typeof(int)) return (int)value;
            if (declaredType == typeof(float)) return (float)value;
            if (declaredType == typeof(bool)) return (bool)value;
            if (declaredType == typeof(string)) return (string?)value;
            if (declaredType == typeof(System.Numerics.Vector3))
            {
                var vector = (System.Numerics.Vector3)value;
                return new[] { vector.X, vector.Y, vector.Z };
            }
            if (declaredType == typeof(Entity)) return ((Entity)value).EntityID;
            if (typeof(ComponentBase).IsAssignableFrom(declaredType))
            {
                var component = value as ComponentBase;
                return component?.Entity.EntityID ?? -1;
            }
            if (typeof(global::ScriptComponent).IsAssignableFrom(declaredType))
            {
                var script = value as global::ScriptComponent;
                return script?.EntityID ?? -1;
            }
            if (declaredType == typeof(Prefab)) return ((Prefab)value).Guid ?? "";
            if (declaredType == typeof(Mesh)) return ((Mesh)value).ToInteropString();
            if (declaredType == typeof(Texture)) return ((Texture)value).ToInteropString();
            if (declaredType == typeof(AudioClip)) return ((AudioClip)value).ToInteropString();
            if (declaredType == typeof(AnimationController)) return ((AnimationController)value).Path ?? "";
            if (declaredType == typeof(AnimationControllerOverride)) return ((AnimationControllerOverride)value).Path ?? "";
            if (declaredType.Name == "DialogueLibraryRef" || declaredType.FullName == "Dialogue.DialogueLibraryRef")
            {
                var guidField = declaredType.GetField("guid", BindingFlags.Instance | BindingFlags.Public);
                return guidField?.GetValue(value) as string ?? "";
            }
            if (declaredType.IsGenericType && declaredType.GetGenericTypeDefinition() == typeof(ResourceRef<>))
            {
                return declaredType.GetProperty("Guid")?.GetValue(value) as string ?? "";
            }
            if (typeof(Scripting.Scriptable.ClayScriptableObject).IsAssignableFrom(declaredType))
            {
                return (value as Scripting.Scriptable.ClayScriptableObject)?.Guid ?? "";
            }
            if (declaredType.IsEnum) return Convert.ToInt32(value);
            if (declaredType.IsGenericType && declaredType.GetGenericTypeDefinition() == typeof(List<>))
            {
                Type elementType = declaredType.GetGenericArguments()[0];
                var list = value as System.Collections.IList;
                var result = new List<object?>();
                if (list != null)
                {
                    foreach (var element in list)
                    {
                        result.Add(SerializeStructuredValue(element, elementType));
                    }
                }
                return result;
            }

            if (ToNative(declaredType) == NativePropertyType.Struct)
            {
                var obj = new Dictionary<string, object?>();
                foreach (var field in GetSerializableStructFields(declaredType))
                {
                    obj[field.Name] = SerializeStructuredValue(field.GetValue(value), field.FieldType);
                }
                return obj;
            }

            return null;
        }

        private static object? GetComponentByType(Entity entity, Type componentType)
        {
            try
            {
                MethodInfo? method = typeof(EntityExtensions)
                    .GetMethods(BindingFlags.Public | BindingFlags.Static)
                    .FirstOrDefault(m => m.Name == nameof(EntityExtensions.GetComponent) &&
                                         m.IsGenericMethodDefinition &&
                                         m.GetParameters().Length == 1);
                if (method == null)
                {
                    return null;
                }

                return method.MakeGenericMethod(componentType).Invoke(null, new object[] { entity });
            }
            catch
            {
                return null;
            }
        }

        private static object? DeserializeStructuredValue(Type targetType, JsonElement element)
        {
            try
            {
                if (targetType == typeof(int))
                {
                    if (element.ValueKind == JsonValueKind.Number && element.TryGetInt32(out int intValue)) return intValue;
                    if (element.ValueKind == JsonValueKind.String && int.TryParse(element.GetString(), out int parsedInt)) return parsedInt;
                    return 0;
                }

                if (targetType == typeof(float))
                {
                    if (element.ValueKind == JsonValueKind.Number) return element.GetSingle();
                    if (element.ValueKind == JsonValueKind.String &&
                        float.TryParse(element.GetString(), System.Globalization.NumberStyles.Float,
                            System.Globalization.CultureInfo.InvariantCulture, out float parsedFloat))
                    {
                        return parsedFloat;
                    }
                    return 0f;
                }

                if (targetType == typeof(bool))
                {
                    if (element.ValueKind == JsonValueKind.True || element.ValueKind == JsonValueKind.False) return element.GetBoolean();
                    if (element.ValueKind == JsonValueKind.String && bool.TryParse(element.GetString(), out bool parsedBool)) return parsedBool;
                    return false;
                }

                if (targetType == typeof(string))
                {
                    return element.ValueKind == JsonValueKind.Null ? string.Empty : (element.GetString() ?? string.Empty);
                }

                if (targetType == typeof(System.Numerics.Vector3))
                {
                    if (element.ValueKind == JsonValueKind.Array && element.GetArrayLength() >= 3)
                    {
                        return new System.Numerics.Vector3(
                            element[0].GetSingle(),
                            element[1].GetSingle(),
                            element[2].GetSingle());
                    }

                    if (element.ValueKind == JsonValueKind.Object)
                    {
                        float x = element.TryGetProperty("X", out var xProp) ? xProp.GetSingle() : 0f;
                        float y = element.TryGetProperty("Y", out var yProp) ? yProp.GetSingle() : 0f;
                        float z = element.TryGetProperty("Z", out var zProp) ? zProp.GetSingle() : 0f;
                        return new System.Numerics.Vector3(x, y, z);
                    }

                    return default(System.Numerics.Vector3);
                }

                if (targetType == typeof(Entity))
                {
                    int id = element.ValueKind == JsonValueKind.Number ? element.GetInt32() : -1;
                    return new Entity(id);
                }

                if (typeof(ComponentBase).IsAssignableFrom(targetType))
                {
                    int id = element.ValueKind == JsonValueKind.Number ? element.GetInt32() : -1;
                    return GetComponentByType(new Entity(id), targetType);
                }

                if (typeof(global::ScriptComponent).IsAssignableFrom(targetType))
                {
                    int id = element.ValueKind == JsonValueKind.Number ? element.GetInt32() : -1;
                    return ScriptRegistry.Get(targetType, id);
                }

                if (targetType == typeof(Prefab)) return new Prefab { Guid = element.GetString() };
                if (targetType == typeof(Mesh)) return Mesh.FromInteropString(element.GetString());
                if (targetType == typeof(Texture)) return Texture.FromInteropString(element.GetString());
                if (targetType == typeof(AudioClip)) return AudioClip.FromInteropString(element.GetString());
                if (targetType == typeof(AnimationController)) return new AnimationController { Path = element.GetString() ?? "" };
                if (targetType == typeof(AnimationControllerOverride)) return new AnimationControllerOverride { Path = element.GetString() ?? "" };

                if (targetType.Name == "DialogueLibraryRef" || targetType.FullName == "Dialogue.DialogueLibraryRef")
                {
                    object? dialogueRef = Activator.CreateInstance(targetType);
                    var guidField = targetType.GetField("guid", BindingFlags.Instance | BindingFlags.Public);
                    if (dialogueRef != null && guidField != null)
                    {
                        guidField.SetValue(dialogueRef, element.GetString() ?? "");
                    }
                    return dialogueRef;
                }

                if (targetType.IsGenericType && targetType.GetGenericTypeDefinition() == typeof(ResourceRef<>))
                {
                    object? resourceRef = Activator.CreateInstance(targetType);
                    targetType.GetProperty("Guid")?.SetValue(resourceRef, element.GetString() ?? "");
                    return resourceRef;
                }

                if (typeof(Scripting.Scriptable.ClayScriptableObject).IsAssignableFrom(targetType))
                {
                    string guid = element.GetString() ?? "";
                    if (string.IsNullOrEmpty(guid))
                    {
                        return null;
                    }

                    return Scripting.Scriptable.ClayScriptableObjectLoader.Load(guid, targetType);
                }

                if (targetType.IsEnum)
                {
                    if (element.ValueKind == JsonValueKind.Number && element.TryGetInt32(out int enumValue))
                    {
                        return Enum.ToObject(targetType, enumValue);
                    }

                    if (element.ValueKind == JsonValueKind.String)
                    {
                        string? enumText = element.GetString();
                        if (int.TryParse(enumText, out int parsedEnum))
                        {
                            return Enum.ToObject(targetType, parsedEnum);
                        }

                        if (!string.IsNullOrEmpty(enumText))
                        {
                            return Enum.Parse(targetType, enumText, ignoreCase: true);
                        }
                    }

                    return Activator.CreateInstance(targetType);
                }

                if (targetType.IsGenericType && targetType.GetGenericTypeDefinition() == typeof(List<>))
                {
                    Type elementType = targetType.GetGenericArguments()[0];
                    var list = Activator.CreateInstance(targetType) as System.Collections.IList;
                    if (list == null || element.ValueKind != JsonValueKind.Array)
                    {
                        return list;
                    }

                    foreach (JsonElement item in element.EnumerateArray())
                    {
                        object? deserialized = DeserializeStructuredValue(elementType, item);
                        if (deserialized != null || !elementType.IsValueType)
                        {
                            list.Add(deserialized);
                        }
                    }

                    return list;
                }

                if (ToNative(targetType) == NativePropertyType.Struct)
                {
                    object? structValue = Activator.CreateInstance(targetType);
                    if (structValue == null || element.ValueKind != JsonValueKind.Object)
                    {
                        return structValue;
                    }

                    foreach (var field in GetSerializableStructFields(targetType))
                    {
                        if (element.TryGetProperty(field.Name, out JsonElement fieldJson))
                        {
                            field.SetValue(structValue, DeserializeStructuredValue(field.FieldType, fieldJson));
                        }
                    }

                    return structValue;
                }
            }
            catch
            {
            }

            return null;
        }

        // Native will grab pointer to this method
        public static void SetManagedField(IntPtr handle, string field, IntPtr boxed)
        {
            try
            {
                if (!TryGetLiveTarget(handle, out var inst) || inst == null) return;

                // Dotted path => nested struct field (e.g. "adjustmentData.JitterAmount").
                if (field.IndexOf('.') >= 0)
                {
                    SetNestedManagedField(inst, field, boxed);
                    return;
                }

                var fi = inst.GetType().GetField(field, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
                if (fi == null) return;

                Type ft = fi.FieldType;
                object? value = null;
                if (ft == typeof(int))
                {
                    value = Marshal.ReadInt32(boxed);
                }
                else if (ft == typeof(float))
                {
                    value = Marshal.PtrToStructure<float>(boxed);
                }
                else if (ft == typeof(bool))
                {
                    value = Marshal.ReadByte(boxed) != 0;
                }
                else if (ft == typeof(string))
                {
                    value = Marshal.PtrToStringAnsi(boxed);
                }
                else if (ft == typeof(System.Numerics.Vector3))
                {
                    value = Marshal.PtrToStructure<System.Numerics.Vector3>(boxed);
                }
                else if (ft == typeof(Entity))
                {
                    int id = Marshal.ReadInt32(boxed);
                    value = new Entity(id);
                }
                else if (typeof(ComponentBase).IsAssignableFrom(ft))
                {
                    // Boxed value is an int entity id. Resolve to the requested component on that entity.
                    int id = Marshal.ReadInt32(boxed);
                    var ent = new Entity(id);
                    var mi = typeof(EntityExtensions).GetMethod("GetComponent", BindingFlags.Public | BindingFlags.Static);
                    var comp = mi != null ? mi.MakeGenericMethod(ft).Invoke(null, new object[] { ent }) : null;
                    value = comp; // may be null if entity lacks component
                }
                else if (typeof(global::ScriptComponent).IsAssignableFrom(ft))
                {
                    // Boxed value is entity id; resolve live script instance if attached
                    int id = Marshal.ReadInt32(boxed);
                    value = ScriptRegistry.Get(ft, id);
                }
                else if (ft == typeof(Prefab))
                {
                    // Boxed value is a string pointer containing the prefab GUID
                    string? guid = Marshal.PtrToStringAnsi(boxed);
                    value = new Prefab { Guid = guid };
                }
                else if (ft == typeof(Mesh))
                {
                    // Boxed value is a string pointer containing "GUID:fileID" format
                    string? interopStr = Marshal.PtrToStringAnsi(boxed);
                    value = Mesh.FromInteropString(interopStr);
                }
                else if (ft == typeof(Texture))
                {
                    // Boxed value is a string pointer containing "GUID:fileID" format
                    string? interopStr = Marshal.PtrToStringAnsi(boxed);
                    value = Texture.FromInteropString(interopStr);
                }
                else if (ft == typeof(AudioClip))
                {
                    // Boxed value is a string pointer containing the audio GUID
                    string? interopStr = Marshal.PtrToStringAnsi(boxed);
                    value = AudioClip.FromInteropString(interopStr);
                }
                else if (ft == typeof(AnimationController))
                {
                    // Boxed value is a string pointer containing the controller path
                    string? path = Marshal.PtrToStringAnsi(boxed);
                    AnimationController ctrl = fi.GetValue(inst) is AnimationController existing
                        ? existing
                        : new AnimationController();
                    ctrl.Path = path ?? "";
                    fi.SetValue(inst, ctrl);
                }
                else if (ft == typeof(AnimationControllerOverride))
                {
                    // Boxed value is a string pointer containing the override path
                    string? path = Marshal.PtrToStringAnsi(boxed);
                    AnimationControllerOverride ctrlOverride = fi.GetValue(inst) is AnimationControllerOverride existing
                        ? existing
                        : new AnimationControllerOverride();
                    ctrlOverride.Path = path ?? "";
                    fi.SetValue(inst, ctrlOverride);
                }
                else if (ft.Name == "DialogueLibraryRef" || ft.FullName == "Dialogue.DialogueLibraryRef")
                {
                    // DialogueLibraryRef - boxed value is a string pointer containing the GUID
                    string? guid = Marshal.PtrToStringAnsi(boxed);
                    // Use reflection to create the struct and set its guid field
                    value = Activator.CreateInstance(ft);
                    var guidField = ft.GetField("guid", BindingFlags.Instance | BindingFlags.Public);
                    if (guidField != null && value != null)
                    {
                        guidField.SetValue(value, guid ?? "");
                    }
                }
                else if (ft.IsGenericType && ft.GetGenericTypeDefinition() == typeof(ResourceRef<>))
                {
                    // ResourceRef<T> - boxed value is a GUID string, set on the wrapper
                    string? guid = Marshal.PtrToStringAnsi(boxed);
                    // Get or create the ResourceRef instance
                    var resourceRef = fi.GetValue(inst);
                    if (resourceRef == null)
                    {
                        resourceRef = Activator.CreateInstance(ft);
                        fi.SetValue(inst, resourceRef);
                    }
                    // Set the Guid property
                    var guidProp = ft.GetProperty("Guid");
                    guidProp?.SetValue(resourceRef, guid ?? "");
                    value = null; // Already set via SetValue above
                }
                else if (typeof(Scripting.Scriptable.ClayScriptableObject).IsAssignableFrom(ft))
                {
                    // Boxed value is a string pointer containing the asset GUID
                    string? guid = Marshal.PtrToStringAnsi(boxed);
                    if (!string.IsNullOrEmpty(guid))
                    {
                        value = Scripting.Scriptable.ClayScriptableObjectLoader.Load(guid, ft);
                    }
                }
                else if (ft.IsEnum)
                {
                    // Enum types - boxed value is the integer value
                    int enumValue = Marshal.ReadInt32(boxed);
                    value = Enum.ToObject(ft, enumValue);
                }
                else if (ft.IsGenericType && ft.GetGenericTypeDefinition() == typeof(List<>))
                {
                    string? serialized = Marshal.PtrToStringAnsi(boxed);
                    Type elemType = ft.GetGenericArguments()[0];
                    
                    var list = Activator.CreateInstance(ft) as System.Collections.IList;
                    if (list != null && !string.IsNullOrEmpty(serialized))
                    {
                        if (IsStructuredListElementType(elemType) && LooksLikeJsonPayload(serialized))
                        {
                            using JsonDocument doc = JsonDocument.Parse(serialized);
                            if (doc.RootElement.ValueKind == JsonValueKind.Array)
                            {
                                foreach (JsonElement item in doc.RootElement.EnumerateArray())
                                {
                                    object? deserialized = DeserializeStructuredValue(elemType, item);
                                    if (deserialized != null || !elemType.IsValueType)
                                    {
                                        list.Add(deserialized);
                                    }
                                }
                            }
                        }
                        else
                        {
                            string[] parts = serialized.Split('|');
                            foreach (string part in parts)
                            {
                                if (string.IsNullOrEmpty(part)) continue;

                                object? elem = null;
                                if (typeof(Scripting.Scriptable.ClayScriptableObject).IsAssignableFrom(elemType))
                                {
                                    elem = Scripting.Scriptable.ClayScriptableObjectLoader.Load(part, elemType);
                                }
                                else if (elemType == typeof(int))
                                {
                                    if (int.TryParse(part, out int iv)) elem = iv;
                                }
                                else if (elemType == typeof(float))
                                {
                                    if (float.TryParse(part, System.Globalization.NumberStyles.Float,
                                        System.Globalization.CultureInfo.InvariantCulture, out float fv)) elem = fv;
                                }
                                else if (elemType == typeof(bool))
                                {
                                    elem = part == "true" || part == "1";
                                }
                                else if (elemType == typeof(string))
                                {
                                    elem = part;
                                }
                                else if (elemType == typeof(Prefab))
                                {
                                    elem = new Prefab { Guid = part };
                                }
                                else if (elemType == typeof(Mesh))
                                {
                                    elem = Mesh.FromInteropString(part);
                                }
                                else if (elemType == typeof(Texture))
                                {
                                    elem = Texture.FromInteropString(part);
                                }
                                else if (elemType == typeof(AudioClip))
                                {
                                    elem = AudioClip.FromInteropString(part);
                                }
                                else if (elemType.Name == "DialogueLibraryRef" || elemType.FullName == "Dialogue.DialogueLibraryRef")
                                {
                                    object? dialogueRef = Activator.CreateInstance(elemType);
                                    var guidField = elemType.GetField("guid", BindingFlags.Instance | BindingFlags.Public);
                                    if (dialogueRef != null && guidField != null)
                                    {
                                        guidField.SetValue(dialogueRef, part);
                                        elem = dialogueRef;
                                    }
                                }
                                else if (elemType == typeof(AnimationController))
                                {
                                    elem = new AnimationController { Path = part };
                                }
                                else if (elemType == typeof(AnimationControllerOverride))
                                {
                                    elem = new AnimationControllerOverride { Path = part };
                                }
                                else if (elemType == typeof(Entity))
                                {
                                    if (int.TryParse(part, out int entityId)) elem = new Entity(entityId);
                                }
                                else if (elemType.IsEnum)
                                {
                                    if (int.TryParse(part, out int ev)) elem = Enum.ToObject(elemType, ev);
                                }

                                if (elem != null)
                                {
                                    list.Add(elem);
                                }
                            }
                        }
                    }
                    value = list;
                }
                else
                {
                    // Unsupported type
                    Console.WriteLine($"[C#] Unsupported SetManagedField type: {ft}");
                }

                if (value != null)
                    fi.SetValue(inst, value);
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[C#] SetManagedField error: {ex}");
            }
        }

        // Native will grab pointer to this method
        // Reads a field value from a managed script instance and boxes it for native interop
        public static bool GetManagedField(IntPtr handle, string field, int propertyType, IntPtr boxedOut)
        {
            try
            {
                if (!TryGetLiveTarget(handle, out var inst) || inst == null) return false;

                // Dotted path => nested struct field. Supports the simple value-type
                // leaves used inside serializable structs.
                if (field.IndexOf('.') >= 0)
                {
                    object? nv = GetNestedManagedFieldValue(inst, field, out Type? lt);
                    if (nv == null || lt == null) return false;
                    if (lt == typeof(int)) { Marshal.WriteInt32(boxedOut, (int)nv); return true; }
                    if (lt == typeof(float)) { Marshal.StructureToPtr((float)nv, boxedOut, false); return true; }
                    if (lt == typeof(bool)) { Marshal.WriteByte(boxedOut, (byte)(((bool)nv) ? 1 : 0)); return true; }
                    if (lt == typeof(System.Numerics.Vector3)) { Marshal.StructureToPtr((System.Numerics.Vector3)nv, boxedOut, false); return true; }
                    if (lt == typeof(Entity)) { Marshal.WriteInt32(boxedOut, ((Entity)nv).EntityID); return true; }
                    if (lt.IsEnum) { Marshal.WriteInt32(boxedOut, Convert.ToInt32(nv)); return true; }
                    return false;
                }

                var fi = inst.GetType().GetField(field, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
                if (fi == null) return false;

                Type ft = fi.FieldType;
                object? value = fi.GetValue(inst);
                if (value == null) return false;

                NativePropertyType propType = (NativePropertyType)propertyType;

                // Box the value based on its type and write to the output pointer
                switch (propType)
                {
                    case NativePropertyType.Int:
                    case NativePropertyType.Entity:
                    case NativePropertyType.ComponentRef:
                    case NativePropertyType.ScriptRef:
                    case NativePropertyType.Enum:
                        if (ft.IsEnum)
                        {
                            int enumValue = Convert.ToInt32(value);
                            Marshal.WriteInt32(boxedOut, enumValue);
                        }
                        else if (ft == typeof(int))
                        {
                            Marshal.WriteInt32(boxedOut, (int)value);
                        }
                        else if (ft == typeof(Entity))
                        {
                            Entity e = (Entity)value;
                            Marshal.WriteInt32(boxedOut, e.EntityID);
                        }
                        else if (typeof(ComponentBase).IsAssignableFrom(ft))
                        {
                            ComponentBase? comp = value as ComponentBase;
                            int entityId = comp != null ? comp.Entity.EntityID : -1;
                            Marshal.WriteInt32(boxedOut, entityId);
                        }
                        else if (typeof(global::ScriptComponent).IsAssignableFrom(ft))
                        {
                            global::ScriptComponent? script = value as global::ScriptComponent;
                            int entityId = script != null ? script.EntityID : -1;
                            Marshal.WriteInt32(boxedOut, entityId);
                        }
                        else
                        {
                            return false;
                        }
                        return true;

                    case NativePropertyType.Float:
                        if (ft == typeof(float))
                        {
                            Marshal.StructureToPtr((float)value, boxedOut, false);
                            return true;
                        }
                        return false;

                    case NativePropertyType.Bool:
                        if (ft == typeof(bool))
                        {
                            Marshal.WriteByte(boxedOut, (byte)((bool)value ? 1 : 0));
                            return true;
                        }
                        return false;

                    case NativePropertyType.String:
                    case NativePropertyType.Prefab:
                    case NativePropertyType.ClayObject:
                    case NativePropertyType.Mesh:
                    case NativePropertyType.Texture:
                    case NativePropertyType.Audio:
                    case NativePropertyType.DialogueLibrary:
                    case NativePropertyType.AnimationController:
                    case NativePropertyType.AnimationControllerOverride:
                        string? strValue = null;
                        if (ft == typeof(string))
                        {
                            strValue = (string?)value;
                        }
                        else if (ft == typeof(Prefab))
                        {
                            Prefab p = (Prefab)value;
                            strValue = p.Guid;
                        }
                        else if (ft == typeof(Mesh))
                        {
                            Mesh m = (Mesh)value;
                            strValue = m.ToInteropString();
                        }
                        else if (ft == typeof(Texture))
                        {
                            Texture tex = (Texture)value;
                            strValue = tex.ToInteropString();
                        }
                        else if (ft == typeof(AudioClip))
                        {
                            AudioClip clip = (AudioClip)value;
                            strValue = clip.ToInteropString();
                        }
                        else if (ft.Name == "DialogueLibraryRef" || ft.FullName == "Dialogue.DialogueLibraryRef")
                        {
                            var guidField = ft.GetField("guid", BindingFlags.Instance | BindingFlags.Public);
                            strValue = guidField?.GetValue(value) as string ?? "";
                        }
                        else if (ft == typeof(AnimationController))
                        {
                            AnimationController ctrl = (AnimationController)value;
                            strValue = ctrl.Path ?? "";
                        }
                        else if (ft == typeof(AnimationControllerOverride))
                        {
                            AnimationControllerOverride ctrl = (AnimationControllerOverride)value;
                            strValue = ctrl.Path ?? "";
                        }
                        else if (ft.IsGenericType && ft.GetGenericTypeDefinition() == typeof(ResourceRef<>))
                        {
                            var guidProp = ft.GetProperty("Guid");
                            strValue = guidProp?.GetValue(value) as string ?? "";
                        }
                        else if (typeof(Scripting.Scriptable.ClayScriptableObject).IsAssignableFrom(ft))
                        {
                            var scriptableObj = value as Scripting.Scriptable.ClayScriptableObject;
                            strValue = scriptableObj?.Guid ?? "";
                        }
                        else
                        {
                            return false;
                        }
                        if (strValue != null)
                        {
                            // Allocate unmanaged memory for the string and copy it
                            IntPtr strPtr = Marshal.StringToHGlobalAnsi(strValue);
                            // Write the pointer to the location pointed to by boxedOut
                            // boxedOut is IntPtr* (pointer to IntPtr)
                            Marshal.WriteIntPtr(boxedOut, strPtr);
                            // Note: Native side is responsible for freeing this memory using LocalFree (Windows) or free (Unix)
                            return true;
                        }
                        return false;

                    case NativePropertyType.Vector3:
                        if (ft == typeof(System.Numerics.Vector3))
                        {
                            Marshal.StructureToPtr((System.Numerics.Vector3)value, boxedOut, false);
                            return true;
                        }
                        return false;

                    case NativePropertyType.List:
                        if (ft.IsGenericType && ft.GetGenericTypeDefinition() == typeof(List<>))
                        {
                            var list = value as System.Collections.IList;
                            if (list != null)
                            {
                                Type elemType = ft.GetGenericArguments()[0];
                                string listStr;

                                if (IsStructuredListElementType(elemType))
                                {
                                    var payload = new List<object?>(list.Count);
                                    foreach (var elem in list)
                                    {
                                        payload.Add(SerializeStructuredValue(elem, elemType));
                                    }
                                    listStr = JsonSerializer.Serialize(payload);
                                }
                                else
                                {
                                    var parts = new List<string>();
                                    foreach (var elem in list)
                                    {
                                        if (elem == null) continue;
                                        string? serialized = null;
                                        if (typeof(Scripting.Scriptable.ClayScriptableObject).IsAssignableFrom(elemType))
                                        {
                                            var so = elem as Scripting.Scriptable.ClayScriptableObject;
                                            serialized = so?.Guid ?? "";
                                        }
                                        else if (elemType == typeof(int))
                                        {
                                            serialized = ((int)elem).ToString();
                                        }
                                        else if (elemType == typeof(float))
                                        {
                                            serialized = ((float)elem).ToString(System.Globalization.CultureInfo.InvariantCulture);
                                        }
                                        else if (elemType == typeof(bool))
                                        {
                                            serialized = ((bool)elem) ? "true" : "false";
                                        }
                                        else if (elemType == typeof(string))
                                        {
                                            serialized = (string?)elem ?? "";
                                        }
                                        else if (elemType == typeof(Prefab))
                                        {
                                            serialized = ((Prefab)elem).Guid ?? "";
                                        }
                                        else if (elemType == typeof(Mesh))
                                        {
                                            serialized = ((Mesh)elem).ToInteropString();
                                        }
                                        else if (elemType == typeof(Texture))
                                        {
                                            serialized = ((Texture)elem).ToInteropString();
                                        }
                                        else if (elemType == typeof(AudioClip))
                                        {
                                            serialized = ((AudioClip)elem).ToInteropString();
                                        }
                                        else if (elemType.Name == "DialogueLibraryRef" || elemType.FullName == "Dialogue.DialogueLibraryRef")
                                        {
                                            var guidField = elemType.GetField("guid", BindingFlags.Instance | BindingFlags.Public);
                                            serialized = guidField?.GetValue(elem) as string ?? "";
                                        }
                                        else if (elemType == typeof(AnimationController))
                                        {
                                            serialized = ((AnimationController)elem).Path ?? "";
                                        }
                                        else if (elemType == typeof(AnimationControllerOverride))
                                        {
                                            serialized = ((AnimationControllerOverride)elem).Path ?? "";
                                        }
                                        else if (elemType.IsEnum)
                                        {
                                            serialized = Convert.ToInt32(elem).ToString();
                                        }
                                        if (serialized != null)
                                        {
                                            parts.Add(serialized);
                                        }
                                    }
                                    listStr = string.Join("|", parts);
                                }

                                IntPtr strPtr = Marshal.StringToHGlobalAnsi(listStr);
                                // Write the pointer to the location pointed to by boxedOut
                                Marshal.WriteIntPtr(boxedOut, strPtr);
                                // Note: Native side is responsible for freeing this memory
                                return true;
                            }
                        }
                        return false;

                    case NativePropertyType.Struct:
                        // Structs are serialized as JSON strings
                        // For now, return empty string - struct serialization would require more complex handling
                        // The native side can fall back to serialized data for structs
                        IntPtr emptyStrPtr = Marshal.StringToHGlobalAnsi("");
                        Marshal.WriteIntPtr(boxedOut, emptyStrPtr);
                        return false; // Indicate failure so native falls back to serialized data

                    case NativePropertyType.Dictionary:
                        // Dictionaries are serialized as JSON strings
                        // For now, return empty string - dictionary serialization would require more complex handling
                        // The native side can fall back to serialized data for dictionaries
                        IntPtr emptyDictPtr = Marshal.StringToHGlobalAnsi("");
                        Marshal.WriteIntPtr(boxedOut, emptyDictPtr);
                        return false; // Indicate failure so native falls back to serialized data

                    default:
                        return false;
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[C#] GetManagedField error: {ex}");
                return false;
            }
        }


        // Cached callback pointer so native can invoke C# types
        private static RegisterScriptCallback? _nativeCallback;

        // Exposed for manual call in EngineEntry
        public static IntPtr NativeCallbackPtr => Marshal.GetFunctionPointerForDelegate(_nativeCallback!);

        // Called from native to create a new script instance by class name
        public static IntPtr Script_Create(IntPtr classNamePtr)
        {
            try
            {
                if (classNamePtr == IntPtr.Zero)
                {
                    Console.WriteLine("[C#] Script_Create error: null classNamePtr received");
                    return IntPtr.Zero;
                }
                
                string className = Marshal.PtrToStringAnsi(classNamePtr)!;
                if (string.IsNullOrEmpty(className))
                {
                    Console.WriteLine("[C#] Script_Create error: empty className");
                    return IntPtr.Zero;
                }
                
                var instance = ScriptFactory.Create(className);
                if (instance == null)
                {
                    Console.WriteLine($"[C#] Script_Create error: ScriptFactory.Create returned null for '{className}'");
                    return IntPtr.Zero;
                }

                var handle = GCHandle.Alloc(instance, GCHandleType.Normal);
                var ptr = GCHandle.ToIntPtr(handle);
                lock (_liveHandlesLock)
                {
                    _liveHandles[ptr] = handle;
                    _faultedOnUpdateHandles.Remove(ptr);
                }
                return ptr;
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[C#] Script_Create failed: {ex.GetType().Name}: {ex.Message}");
                Console.WriteLine($"[C#] Stack trace: {ex.StackTrace}");
                return IntPtr.Zero;
            }
        }

        public static void Script_Destroy(IntPtr handle)
        {
            if (TryRemoveLiveHandle(handle, out var gch))
            {
                // Safety: if OnDestroy was skipped for any reason, ensure registry cleanup still happens.
                if (gch.Target is ScriptComponent script)
                {
                    if (script is ClaymoreEngine.Multiplayer.NetworkBehaviour networkBehaviour)
                    {
                        ClaymoreEngine.Networking.MultiplayerRuntime.UnregisterBehaviour(networkBehaviour);
                    }
                    ScriptRegistry.Unregister(script);
                }
                gch.Free();
            }
            ClearOnUpdateFault(handle);
        }


        /// <summary>
        /// Called by native when entering/exiting play mode to clear component caches.
        /// This prevents stale references to destroyed runtime scene entities.
        /// </summary>
        public static void ClearComponentCaches()
        {
            // Script lookup registry is keyed by (EntityID, Type). Runtime clone IDs are reused
            // across play sessions, so stale entries here can cause false-positive GetScript<T>().
            ScriptRegistry.Clear();
            EntityExtensions.ClearAllCaches();
            ClearAllOnUpdateFaults();
            EngineSyncContext.Clear();
            UITween.ResetRuntimeState();
            Tween.ResetRuntimeState();
            DialogueSystem.ResetRuntimeState();
            QuestSystem.ResetRuntimeState();
        }

        // Called FIRST to bind script to entity and register in ScriptRegistry
        // This must be called on ALL scripts BEFORE any OnCreate calls
        public static void Script_Bind(IntPtr handle, int entityId)
        {
            try
            {
                if (handle == IntPtr.Zero)
                {
                    Console.WriteLine("[C#] Script_Bind error: null handle received");
                    return;
                }

                if (!TryGetLiveTarget(handle, out var target) || target is not ScriptComponent script)
                {
                    Console.WriteLine($"[C#] Script_Bind error: handle 0x{handle:X} not in live handles");
                    return;
                }
                script.Bind(new Entity(entityId));
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[C#] Script_Bind error: {ex.GetType().Name}: {ex.Message}");
                Console.WriteLine($"[C#] Stack trace: {ex.StackTrace}");
            }
        }

        // Called AFTER all scripts have been bound - now GetScript<T>() works
        public static void Script_OnCreate(IntPtr handle, int entityId)
        {
            // Validate handle before using it to prevent access violations
            if (handle == IntPtr.Zero)
            {
                Console.WriteLine("[C#] Script_OnCreate error: null handle received");
                return;
            }
            
            ScriptComponent? script = null;
            string scriptName = "Unknown";
            
            try
            {
                if (!TryGetLiveTarget(handle, out var target) || target is not ScriptComponent sc)
                {
                    Console.WriteLine($"[C#] Script_OnCreate error: handle 0x{handle:X} not found in live handles - script may have been destroyed");
                    return;
                }
                script = sc;
                
                if (script == null)
                {
                    Console.WriteLine($"[C#] Script_OnCreate error: GCHandle target is null or not a ScriptComponent (handle=0x{handle:X})");
                    return;
                }
                
                scriptName = script.GetType().Name;
                
                // If Bind wasn't called separately (legacy path), call it now
                if (script.EntityID == 0)
                    script.Bind(new Entity(entityId));
                
                script.OnCreate();
                Console.WriteLine($"[C#] OnCreate thread: managed={Environment.CurrentManagedThreadId}, os={ThreadIds.OsTid()}");
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[C#] Script '{scriptName}' OnCreate EXCEPTION: {ex.GetType().Name}: {ex.Message}");
                Console.WriteLine($"[C#] Stack trace:\n{ex.StackTrace}");
                if (ex.InnerException != null)
                {
                    Console.WriteLine($"[C#] Inner exception: {ex.InnerException.GetType().Name}: {ex.InnerException.Message}");
                }
            }
        }

        // Called once per frame
        public static void Script_OnUpdate(IntPtr handle, float dt)
        {
            // Validate handle before using it to prevent access violations
            if (handle == IntPtr.Zero)
            {
                SafeManagedLogError("Script_OnUpdate: null handle received");
                return;
            }

            if (DisableFaultingOnUpdateScripts && IsOnUpdateFaulted(handle))
            {
                return;
            }
            
            ScriptComponent? script = null;
            string scriptName = "Unknown";
            
            try
            {
                if (!TryGetLiveTarget(handle, out var target) || target is not ScriptComponent sc)
                {
                    SafeManagedLogError($"Script_OnUpdate: handle 0x{handle:X} not found in live handles - script may have been destroyed");
                    return;
                }
                script = sc;
                
                if (script == null)
                {
                    SafeManagedLogError("Script_OnUpdate: GCHandle target is null or not a ScriptComponent");
                    return;
                }
                
                scriptName = script.GetType().FullName ?? script.GetType().Name;
                if (script is ClaymoreEngine.Multiplayer.NetworkBehaviour networkBehaviour &&
                    !networkBehaviour.ShouldProcessLocalUpdate)
                {
                    return;
                }

                script.OnUpdate(dt);
            }
            catch (Exception ex)
            {
                int entityId = script?.EntityID ?? -1;
                SafeManagedLogException(
                    $"Script '{scriptName}' OnUpdate EXCEPTION (entityId={entityId}, handle=0x{handle:X})",
                    ex
                );

                if (DisableFaultingOnUpdateScripts)
                {
                    MarkOnUpdateFaulted(handle);
                    SafeManagedLogError(
                        $"Script '{scriptName}' has been disabled for this play session after OnUpdate exception (entityId={entityId}, handle=0x{handle:X})"
                    );
                }
            }
        }

        // Called when script is being destroyed (before GCHandle is freed)
        public static void Script_OnDestroy(IntPtr handle)
        {
            if (handle == IntPtr.Zero)
                return;
            
            try
            {
                if (!TryGetLiveTarget(handle, out var target) || target is not ScriptComponent script)
                    return;
                
                if (script != null)
                {
                    script.OnDestroy();
                    if (script is ClaymoreEngine.Multiplayer.NetworkBehaviour networkBehaviour)
                    {
                        ClaymoreEngine.Networking.MultiplayerRuntime.UnregisterBehaviour(networkBehaviour);
                    }
                    ScriptRegistry.Unregister(script);
                }
                ClearOnUpdateFault(handle);
            }
            catch (Exception ex)
            {
                SafeManagedLogException($"Script OnDestroy EXCEPTION (handle=0x{handle:X})", ex);
            }
        }

        public static void Script_Invoke(IntPtr handle, string methodName)
        {
            try
            {
                if (!TryGetLiveTarget(handle, out var script) || script == null) return;
                var mi = script.GetType().GetMethod(methodName, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
                if (mi != null)
                {
                    mi.Invoke(script, Array.Empty<object>());
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[C#] Script_Invoke error: {ex}");
            }
        }

        // Native passes pointer to struct containing function pointers
        // Must match ScriptRegistrationInterop in DotNetHost.cpp
        [StructLayout(LayoutKind.Sequential)]
        private struct ScriptRegistrationInterop
        {
            public IntPtr RegisterScriptTypePtr;
            public IntPtr RegisterScriptFlagsPtr;
            public IntPtr RegisterScriptPropertyPtr;
            public IntPtr RegisterScriptPropertyExtendedPtr;
            public IntPtr ClearScriptPropertiesPtr;
        }
        
        private static RegisterScriptFlagsCallback? _registerScriptFlagsNative;
        private static RegisterPropExtendedDelegate? _registerPropExtendedNative;
        private static ClearScriptPropertiesDelegate? _clearScriptPropertiesNative;

        // Called from native to register all known script types (callback passed from C++)
        public static void RegisterAllScripts(IntPtr interopPtr)
        {
            Console.WriteLine("[C#] RegisterAllScripts invoked");

            var interop = Marshal.PtrToStructure<ScriptRegistrationInterop>(interopPtr);

            _nativeCallback = Marshal.GetDelegateForFunctionPointer<RegisterScriptCallback>(interop.RegisterScriptTypePtr);
            if (interop.RegisterScriptFlagsPtr != IntPtr.Zero)
            {
                _registerScriptFlagsNative = Marshal.GetDelegateForFunctionPointer<RegisterScriptFlagsCallback>(interop.RegisterScriptFlagsPtr);
            }
            _registerPropNative = Marshal.GetDelegateForFunctionPointer<RegisterPropDelegate>(interop.RegisterScriptPropertyPtr);
            _registerPropExtendedNative = Marshal.GetDelegateForFunctionPointer<RegisterPropExtendedDelegate>(interop.RegisterScriptPropertyExtendedPtr);
            _clearScriptPropertiesNative = Marshal.GetDelegateForFunctionPointer<ClearScriptPropertiesDelegate>(interop.ClearScriptPropertiesPtr);

            if (_nativeCallback != null)
                InteropProcessor.SetRegisterCallback(_nativeCallback);
            if (_registerScriptFlagsNative != null)
                InteropProcessor.SetRegisterFlagsCallback(_registerScriptFlagsNative);

            // Ensure DontDestroyOnLoad flags are registered after callbacks are set.
            ScriptDomain.ReapplyScriptFlags();

            if (_nativeCallback == null || _registerPropNative == null)
            {
                Console.WriteLine("[C#] Native callbacks are null!");
                return;
            }
            
            if (_registerPropExtendedNative == null || _clearScriptPropertiesNative == null)
            {
                Console.WriteLine("[C#] Extended native callbacks are null - extended property types may not work!");
            }

            // Register reflected fields before we notify native of script types
            foreach (var t in ScriptDomain.AllScriptTypes)
            {
                // Clear any previously registered properties for this type
                try { _clearScriptPropertiesNative?.Invoke(t.FullName!); } catch { }

                foreach (var field in t.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic))
                {
                    if (field.GetCustomAttribute<SerializeField>() == null) continue;

                    NativePropertyType nType = ToNative(field.FieldType);

                    // Serializable structs (including System.Numerics.Vector2) are
                    // expanded into flat "field.leaf" properties. Each leaf is a
                    // primitive that round-trips through the existing serialization,
                    // apply, and inspector paths; nested struct fields are reached
                    // via dotted paths in Get/SetManagedField.
                    if (nType == NativePropertyType.Struct)
                    {
                        object? structOwner = null;
                        try { structOwner = Activator.CreateInstance(t); } catch { structOwner = null; }
                        object? structDef = null;
                        try { structDef = structOwner != null ? field.GetValue(structOwner) : null; } catch { structDef = null; }
                        RegisterStructLeaves(t.FullName!, field.Name, field.FieldType, structDef, 0);
                        continue;
                    }

                    // Instantiate a temporary object to read field initializers as defaults
                    object? tmp = null; try { tmp = Activator.CreateInstance(t); } catch { tmp = null; }
                    object? def = null; try { def = tmp != null ? field.GetValue(tmp) : null; } catch { def = null; }

                    // Box default to unmanaged memory
                    IntPtr boxedPtr = IntPtr.Zero;
                    Type ft = field.FieldType;
                    try
                    {
                        if (ft == typeof(int)) { boxedPtr = Marshal.AllocHGlobal(sizeof(int)); Marshal.WriteInt32(boxedPtr, def is int i ? i : 0); }
                        else if (ft == typeof(float)) { boxedPtr = Marshal.AllocHGlobal(sizeof(float)); float f = def is float fv ? fv : 0f; Marshal.StructureToPtr(f, boxedPtr, false); }
                        else if (ft == typeof(bool)) { boxedPtr = Marshal.AllocHGlobal(1); Marshal.WriteByte(boxedPtr, (byte)((def is bool vb && vb) ? 1 : 0)); }
                        else if (ft == typeof(string)) { boxedPtr = Marshal.StringToHGlobalAnsi(def as string ?? string.Empty); }
                        else if (ft == typeof(System.Numerics.Vector3)) { boxedPtr = Marshal.AllocHGlobal(Marshal.SizeOf<System.Numerics.Vector3>()); var v = def is System.Numerics.Vector3 vv ? vv : default; Marshal.StructureToPtr(v, boxedPtr, false); }
                        else if (ft == typeof(Entity)) { boxedPtr = Marshal.AllocHGlobal(sizeof(int)); int id = (def is Entity e) ? e.EntityID : -1; Marshal.WriteInt32(boxedPtr, id); }
                        else if (ft == typeof(Prefab)) { string guid = (def is Prefab p) ? (p.Guid ?? string.Empty) : string.Empty; boxedPtr = Marshal.StringToHGlobalAnsi(guid); }
                        else if (ft == typeof(Mesh)) { string meshRef = (def is Mesh m) ? m.ToInteropString() : string.Empty; boxedPtr = Marshal.StringToHGlobalAnsi(meshRef); }
                        else if (ft == typeof(Texture)) { string textureRef = (def is Texture tex) ? tex.ToInteropString() : string.Empty; boxedPtr = Marshal.StringToHGlobalAnsi(textureRef); }
                        else if (ft == typeof(AudioClip)) { string audioRef = (def is AudioClip clip) ? clip.ToInteropString() : string.Empty; boxedPtr = Marshal.StringToHGlobalAnsi(audioRef); }
                        else if (ft == typeof(AnimationController)) { string path = (def is AnimationController c) ? (c.Path ?? string.Empty) : string.Empty; boxedPtr = Marshal.StringToHGlobalAnsi(path); }
                        else if (ft == typeof(AnimationControllerOverride)) { string path = (def is AnimationControllerOverride c) ? (c.Path ?? string.Empty) : string.Empty; boxedPtr = Marshal.StringToHGlobalAnsi(path); }
                        else if (ft.Name == "DialogueLibraryRef" || ft.FullName == "Dialogue.DialogueLibraryRef") {
                            // DialogueLibraryRef - extract guid field from struct
                            string guid = string.Empty;
                            if (def != null) {
                                var guidField = ft.GetField("guid", BindingFlags.Instance | BindingFlags.Public);
                                if (guidField != null) guid = guidField.GetValue(def) as string ?? string.Empty;
                            }
                            boxedPtr = Marshal.StringToHGlobalAnsi(guid);
                        }
                        else if (ft.IsEnum) { boxedPtr = Marshal.AllocHGlobal(sizeof(int)); int val = def != null ? (int)def : 0; Marshal.WriteInt32(boxedPtr, val); }
                        else if (ft.IsGenericType && ft.GetGenericTypeDefinition() == typeof(ResourceRef<>)) {
                            // ResourceRef<T> - extract GUID from the wrapper if it exists
                            string guid = string.Empty;
                            if (def != null) {
                                var guidProp = ft.GetProperty("Guid");
                                if (guidProp != null) guid = guidProp.GetValue(def) as string ?? string.Empty;
                            }
                            boxedPtr = Marshal.StringToHGlobalAnsi(guid);
                        }
                        else if (typeof(Scripting.Scriptable.ClayScriptableObject).IsAssignableFrom(ft)) { boxedPtr = Marshal.StringToHGlobalAnsi(string.Empty); }
                    }
                    catch { boxedPtr = IntPtr.Zero; }

                    string? aux = null;
                    if (nType == NativePropertyType.ComponentRef || nType == NativePropertyType.ScriptRef)
                        aux = ft.FullName;
                    // For ResourceRef<T>, aux should be the element type T, not ResourceRef<T>
                    else if (ft.IsGenericType && ft.GetGenericTypeDefinition() == typeof(ResourceRef<>))
                        aux = ft.GetGenericArguments()[0].FullName;
                    else if (nType == NativePropertyType.ClayObject)
                        aux = ft.FullName;

                    // Prepare extended metadata for complex types
                    string? enumNames = null;
                    string? enumValues = null;
                    int listElementType = 0;
                    string? listElementTypeName = null;
                    string? structFieldsJson = null;

                    // Handle Enum types
                    if (nType == NativePropertyType.Enum)
                    {
                        aux = ft.FullName;
                        var names = Enum.GetNames(ft);
                        var values = Enum.GetValues(ft).Cast<int>().ToArray();
                        enumNames = string.Join("|", names);
                        enumValues = string.Join("|", values);
                    }

                    // Handle List<T> types
                    if (nType == NativePropertyType.List)
                    {
                        var elemType = GetListElementType(ft);
                        if (elemType != null)
                        {
                            listElementType = (int)ToNative(elemType);
                            listElementTypeName = elemType.FullName;
                            
                            // If list element is an enum, provide enum metadata
                            if (elemType.IsEnum)
                            {
                                var names = Enum.GetNames(elemType);
                                var values = Enum.GetValues(elemType).Cast<int>().ToArray();
                                enumNames = string.Join("|", names);
                                enumValues = string.Join("|", values);
                            }

                            if (ToNative(elemType) == NativePropertyType.Struct)
                            {
                                structFieldsJson = BuildStructFieldsJson(elemType);
                            }
                        }
                    }

                    // Handle Struct types - serialize field info as JSON
                    if (nType == NativePropertyType.Struct)
                    {
                        aux = ft.FullName;
                        structFieldsJson = BuildStructFieldsJson(ft);
                    }

                    // Check for PopulateFromResources attribute
                    bool populateFromResources = field.GetCustomAttribute<PopulateFromResourcesAttribute>() != null ||
                                                  field.GetCustomAttribute<PopulateFromResourceAttribute>() != null;
                    
                    // Check for SelectFromResources attribute (for single ClayObject fields)
                    // ResourceRef<T> fields automatically enable selectFromResources
                    bool isResourceRef = ft.IsGenericType && ft.GetGenericTypeDefinition() == typeof(ResourceRef<>);
                    bool selectFromResources = isResourceRef ||
                                                field.GetCustomAttribute<SelectFromResourcesAttribute>() != null ||
                                                field.GetCustomAttribute<SelectFromResourceAttribute>() != null;
                    
                    // Use extended registration for complex types
                    if ((nType == NativePropertyType.Enum || nType == NativePropertyType.List || 
                        nType == NativePropertyType.Struct || nType == NativePropertyType.ClayObject ||
                        populateFromResources || selectFromResources) &&
                        _registerPropExtendedNative != null)
                    {
                        _registerPropExtendedNative(t.FullName!, field.Name, (int)nType, boxedPtr, aux,
                            enumNames, enumValues, listElementType, listElementTypeName, structFieldsJson, 
                            populateFromResources, selectFromResources);
                    }
                    else
                    {
                        _registerPropNative!(t.FullName!, field.Name, (int)nType, boxedPtr, aux);
                    }

                    // Free temp buffer
                    if (boxedPtr != IntPtr.Zero)
                    {
                        Marshal.FreeHGlobal(boxedPtr);
                    }
                }
            }
            
            foreach (var type in ScriptDomain.AllScriptTypes)
            {
                if (type.FullName != null)
                {
                    Console.WriteLine($"[C#] Registering: {type.FullName}");
                    _nativeCallback(type.FullName, ScriptDomain.GetEffectivePriority(type));
                }
            }
        }
    }

    // Module interop export shim: native resolves this from module assembly as needed
    public static class ModuleInteropExports
    {
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void GetManagedModuleAPIDelegate(IntPtr nativePtr, IntPtr managedPtr);

        // Loads a module assembly into the current AppDomain so it can be reflected and discovered.
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void LoadModuleAssemblyDelegate([MarshalAs(UnmanagedType.LPWStr)] string moduleAssemblyPath);

        public static void LoadModuleAssembly(string moduleAssemblyPath)
        {
            try { 
                if (!string.IsNullOrWhiteSpace(moduleAssemblyPath)) {
                    Console.WriteLine($"[C#] Loading module assembly: {moduleAssemblyPath}");
                    var asm = System.Reflection.Assembly.LoadFrom(moduleAssemblyPath);
                    Console.WriteLine($"[C#] Successfully loaded assembly: {asm.FullName}");
                }
            }
            catch (Exception ex) { Console.WriteLine($"[C#] LoadModuleAssembly failed: {ex.Message}"); }
        }

        // Non-generic delegate for EnumerateComponents - must be static to prevent GC
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void EnumerateComponentsDelegate(IntPtr context);
        
        private static EnumerateComponentsDelegate? _enumerateComponentsDelegate;
        private static Modules.NativeAPIs _cachedNativeAPIs;
        
        // Static method for EnumerateComponents - must be static for delegate
        private static void EnumerateComponentsStatic(IntPtr context)
        {
            try
            {
                Console.WriteLine("[C#] EnumerateComponents called - scanning for modules...");
                Modules.ModuleRuntime.EnumerateAllModulesAndRegister(ref _cachedNativeAPIs);
                Console.WriteLine("[C#] EnumerateComponents completed successfully");
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[C#] EnumerateComponents failed: {ex}");
            }
        }
        
        // Static fallback method
        private static void EnumerateComponentsFallback(IntPtr context)
        {
            Console.WriteLine("[C#] Fallback EnumerateComponents - no modules found");
        }
        
        public static void GetModuleAPI(IntPtr nativePtr, IntPtr managedPtr)
        {
            try
            {
                Console.WriteLine("[C#] GetModuleAPI called");
                
                // Marshal the native APIs from the pointer
                var native = Marshal.PtrToStructure<Modules.NativeAPIs>(nativePtr);
                _cachedNativeAPIs = native; // Cache for the static method
                
                // Create the delegate from the static method
                _enumerateComponentsDelegate = EnumerateComponentsStatic;
                
                // Create the managed APIs with function pointer
                var managed = new Modules.ManagedAPIs
                {
                    EnumerateComponents = Marshal.GetFunctionPointerForDelegate(_enumerateComponentsDelegate)
                };
                
                // Marshal the managed APIs back to the pointer
                Marshal.StructureToPtr(managed, managedPtr, false);
                
                Console.WriteLine("[C#] GetModuleAPI completed successfully");
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[C#] GetModuleAPI failed: {ex}");
                // Provide a safe fallback
                var fallbackDelegate = new EnumerateComponentsDelegate(EnumerateComponentsFallback);
                var fallbackManaged = new Modules.ManagedAPIs
                {
                    EnumerateComponents = Marshal.GetFunctionPointerForDelegate(fallbackDelegate)
                };
                Marshal.StructureToPtr(fallbackManaged, managedPtr, false);
            }
        }
    }

    // Delegates used in native interop
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate IntPtr Script_CreateDelegate(IntPtr className);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Script_BindDelegate(IntPtr handle, int entityId);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Script_OnCreateDelegate(IntPtr handle, int entityId);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Script_OnUpdateDelegate(IntPtr handle, float dt);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Script_OnDestroyDelegate(IntPtr handle);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void ClearComponentCachesDelegate();

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void RegisterScriptCallback(string className, int priority);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void RegisterScriptFlagsCallback(string className, uint flags);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void RegisterAllScriptsDelegate(IntPtr nativeCallback);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate bool HasBakeInWorldGraphScriptDelegate([MarshalAs(UnmanagedType.LPStr)] string className);

    // Top-level delegate for invoking arbitrary method calls on managed scripts
    // (Resolved by native via load_assembly_and_get_function_pointer)
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Script_InvokeDelegate(IntPtr handle, [MarshalAs(UnmanagedType.LPStr)] string methodName);

    // Destroy delegate so native can free GCHandles for managed script instances
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Script_DestroyDelegate(IntPtr handle);

    // Clay object cache invalidation delegate
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void InvalidateClayObjectCacheDelegate([MarshalAs(UnmanagedType.LPStr)] string guid);

    // Scriptable interop bootstrap declaration for native to resolve
    public static class ScriptableInteropExports
    {
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void GetManagedScriptableAPIDelegate(ref Scripting.Scriptable.NativeScriptableAPI native, out Scripting.Scriptable.ManagedScriptableAPI managedOut);

        // Native resolves pointer to this method via load_assembly_and_get_function_pointer
        public static void GetScriptableAPI(ref Scripting.Scriptable.NativeScriptableAPI native, out Scripting.Scriptable.ManagedScriptableAPI managed)
        {
            Scripting.Scriptable.ScriptableInteropExports.GetScriptableAPI(ref native, out managed);
        }
    }

    // Clay object cache invalidation export for native to call
    public static class ClayObjectCacheExports
    {
        // Native resolves pointer to this method via load_assembly_and_get_function_pointer
        public static void InvalidateCache([MarshalAs(UnmanagedType.LPStr)] string guid)
        {
            Scripting.Scriptable.ClayScriptableObjectLoader.InvalidateCache(guid);
        }
    }

    // World graph bake metadata helper for native editor bake pipeline.
    public static class WorldGraphBakeInteropExports
    {
        public static bool HasBakeInWorldGraphAttribute([MarshalAs(UnmanagedType.LPStr)] string className)
        {
            if (string.IsNullOrWhiteSpace(className)) return false;

            var type = ScriptDomain.ResolveType(className);
            if (type == null) return false;

            // Normal fast path.
            if (type.GetCustomAttribute<BakeInWorldGraphAttribute>(inherit: true) != null)
                return true;

            // Cross-ALC defensive fallback.
            foreach (var attr in type.GetCustomAttributes(inherit: true))
            {
                var attrType = attr.GetType();
                if (attrType.Name == nameof(BakeInWorldGraphAttribute))
                    return true;
                if (attrType.FullName == "ClaymoreEngine.BakeInWorldGraphAttribute")
                    return true;
            }

            return false;
        }
    }
}
