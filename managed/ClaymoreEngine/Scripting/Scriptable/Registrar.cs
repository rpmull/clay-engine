using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text.Json;
using ClaymoreEngine; // For ResourceRef<T>

namespace ClaymoreEngine.Scripting.Scriptable
{
    [StructLayout(LayoutKind.Sequential)]
    public struct TypeId { public ulong High; public ulong Low; }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    internal struct FieldDesc
    {
        public IntPtr name;       // UTF8 cstr
        public uint type;         // ValueType
        public uint flags;
        public int arrayRank;
        public IntPtr enumType;   // UTF8 cstr
        public IntPtr enumNames;  // UTF8 pipe-separated names (e.g., "None|Option1|Option2")
        public IntPtr enumValues; // UTF8 pipe-separated values (e.g., "0|1|2")
        // Extended metadata
        public uint listElementType;
        public IntPtr listElementTypeName;
        public IntPtr auxType;
        public IntPtr structFieldsJson;
        public uint populateFromResources;
        public uint selectFromResources;
        // Conditional visibility (empty showIfField = no condition)
        public IntPtr showIfField;
        public IntPtr showIfValue;
        public uint showIfMode;   // 0 = show when equal, 1 = hide when equal
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct ScriptableTypeDesc
    {
        public TypeId id;
        public IntPtr fullName;   // UTF8
        public IntPtr menuPath;   // UTF8
        public IntPtr defaultFile;// UTF8
        public int order;
        public uint version;
        public IntPtr fields;     // FieldDesc*
        public int fieldCount;
        public IntPtr CreateNative; // fn ptr
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct ManagedValue
    {
        public uint type;
        public uint isArray;
        public IntPtr data;
        public uint count;
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate bool RegisterTypeDelegate(ref ScriptableTypeDesc desc);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate bool SetFieldDelegate(IntPtr nativeInstance, [MarshalAs(UnmanagedType.LPUTF8Str)] string field, ref ManagedValue value);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate bool GetFieldDelegate(IntPtr nativeInstance, [MarshalAs(UnmanagedType.LPUTF8Str)] string field, ref ManagedValue valueOut);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate bool MarkDirtyDelegate(IntPtr nativeInstance);

    [StructLayout(LayoutKind.Sequential)]
    public struct NativeScriptableAPI
    {
        // Field order must match C++ struct NativeScriptableAPI exactly
        // Function pointers from C++ are marshaled as IntPtr
        public IntPtr user;                    // Offset 0: void* user
        public IntPtr RegisterTypePtr;          // Offset 8: bool (*RegisterType)(...)
        public IntPtr SetFieldPtr;              // Offset 16: bool (*SetField)(...)
        public IntPtr GetFieldPtr;              // Offset 24: bool (*GetField)(...)
        public IntPtr MarkDirtyPtr;             // Offset 32: bool (*MarkDirty)(...)
        public IntPtr GetPathForGUIDPtr;        // Offset 40: const char* (*GetPathForGUID)(...)
        public IntPtr IsTypeAssignablePtr;      // Offset 48: bool (*IsTypeAssignable)(...)
        public IntPtr ReadFileContentsPtr;      // Offset 56: const char* (*ReadFileContents)(...)
        public IntPtr InvalidateCachePtr;       // Offset 64: void (*InvalidateCache)(...)
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct ManagedScriptableAPI
    {
        public IntPtr EnumerateTypes;
        public IntPtr CreateDefault;
        public IntPtr CustomInspector;
        public IntPtr Upgrade;
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void GetManagedScriptableAPIDelegate(ref NativeScriptableAPI nativeApi, out ManagedScriptableAPI managedOut);

    public static class ScriptableInteropExports
    {
        private static RegisterTypeDelegate? _registerType;
        private static SetFieldDelegate? _setField;
        private static GetFieldDelegate? _getField;
        private static MarkDirtyDelegate? _markDirty;

        // Keep delegates alive so their function pointers remain valid across interop
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void EnumerateTypesDelegate(IntPtr user);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate bool CreateDefaultDelegate(TypeId type, IntPtr native);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate bool CustomInspectorDelegate(TypeId type, IntPtr native, IntPtr drawerApi);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate bool UpgradeDelegate(TypeId type, uint fromVersion, IntPtr native);

        private static EnumerateTypesDelegate? _enumerateTypesDel;
        private static CreateDefaultDelegate? _createDefaultDel;
        private static CustomInspectorDelegate? _customInspectorDel;
        private static UpgradeDelegate? _upgradeDel;

        // Delegate for native GetPathForGUID function
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate IntPtr GetPathForGUIDDelegate([MarshalAs(UnmanagedType.LPStr)] string guidStr);
        private static GetPathForGUIDDelegate? _getPathForGUID;
        
        // Delegate for native ReadFileContents function (reads via VFS, works with PAK)
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate IntPtr ReadFileContentsDelegate([MarshalAs(UnmanagedType.LPStr)] string path);
        private static ReadFileContentsDelegate? _readFileContents;
        
        public static void GetScriptableAPI(ref NativeScriptableAPI native, out ManagedScriptableAPI managed)
        {
            try
            {
                _registerType = Marshal.GetDelegateForFunctionPointer<RegisterTypeDelegate>(native.RegisterTypePtr);
                _setField = Marshal.GetDelegateForFunctionPointer<SetFieldDelegate>(native.SetFieldPtr);
                _getField = Marshal.GetDelegateForFunctionPointer<GetFieldDelegate>(native.GetFieldPtr);
                _markDirty = Marshal.GetDelegateForFunctionPointer<MarkDirtyDelegate>(native.MarkDirtyPtr);
                
                // Initialize GetPathForGUID if available
                if (native.GetPathForGUIDPtr != IntPtr.Zero)
                {
                    _getPathForGUID = Marshal.GetDelegateForFunctionPointer<GetPathForGUIDDelegate>(native.GetPathForGUIDPtr);
                    // Initialize the ClayScriptableObjectLoader with the native function
                    ClayScriptableObjectLoader.Initialize((guid) => {
                        if (_getPathForGUID == null) return null;
                        IntPtr pathPtr = _getPathForGUID(guid);
                        return pathPtr == IntPtr.Zero ? null : Marshal.PtrToStringAnsi(pathPtr);
                    });
                    Console.WriteLine("[C#] ClayScriptableObjectLoader initialized with native GetPathForGUID");
                }
                
                // Initialize ReadFileContents if available (for VFS/PAK file access)
                if (native.ReadFileContentsPtr != IntPtr.Zero)
                {
                    _readFileContents = Marshal.GetDelegateForFunctionPointer<ReadFileContentsDelegate>(native.ReadFileContentsPtr);
                    ClayScriptableObjectLoader.InitializeVFS((path) => {
                        if (_readFileContents == null) return null;
                        IntPtr contentsPtr = _readFileContents(path);
                        return contentsPtr == IntPtr.Zero ? null : Marshal.PtrToStringAnsi(contentsPtr);
                    });
                    Console.WriteLine("[C#] ClayScriptableObjectLoader VFS initialized with native ReadFileContents");
                }

                // Root delegates to prevent GC
                _enumerateTypesDel = new EnumerateTypesDelegate(EnumerateTypesImpl);
                _createDefaultDel = new CreateDefaultDelegate(CreateDefaultImpl);
                _customInspectorDel = new CustomInspectorDelegate(CustomInspectorImpl);
                _upgradeDel = new UpgradeDelegate(UpgradeImpl);

                managed = new ManagedScriptableAPI
                {
                    EnumerateTypes = Marshal.GetFunctionPointerForDelegate(_enumerateTypesDel),
                    CreateDefault = Marshal.GetFunctionPointerForDelegate(_createDefaultDel),
                    CustomInspector = Marshal.GetFunctionPointerForDelegate(_customInspectorDel),
                    Upgrade = Marshal.GetFunctionPointerForDelegate(_upgradeDel)
                };
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[C#] GetScriptableAPI failed: {ex}");
                managed = default;
            }
        }

        private static void EnumerateTypesImpl(IntPtr user)
        {
            try
            {
                var types = AppDomain.CurrentDomain.GetAssemblies()
                    .SelectMany(a => a.GetExportedTypes())
                    .Where(t => typeof(ClayScriptableObject).IsAssignableFrom(t) && !t.IsAbstract)
                    .ToArray();

                foreach (var t in types)
                {
                    // Check for CreateAssetMenuAttribute first, then AssetPathAttribute
                    var menu = t.GetCustomAttribute<CreateAssetMenuAttribute>();
                    var assetPath = t.GetCustomAttribute<AssetPathAttribute>();
                    
                    string menuPath;
                    string fileName;
                    int order;
                    
                    if (menu != null)
                    {
                        menuPath = menu.MenuPath;
                        fileName = menu.FileName;
                        order = menu.Order;
                    }
                    else if (assetPath != null)
                    {
                        // AssetPath creates menu under ClayObject > {path}
                        menuPath = $"ClayObject/{assetPath.Path}/{t.Name}";
                        fileName = assetPath.DefaultFileName;
                        order = assetPath.Order;
                    }
                    else
                    {
                        menuPath = $"ClayObject/{t.Name}";
                        fileName = $"New{t.Name}";
                        order = 0;
                    }
                    
                    uint version = 1;

                    // Compute TypeId as 128-bit xxhash-like via two 64-bit hashes
                    var nameUtf8 = System.Text.Encoding.UTF8.GetBytes(t.FullName! + "\0");
                    ulong low = XXHash64(nameUtf8, 0x9E3779B97F4A7C15UL);
                    ulong high = XXHash64(nameUtf8, 0xC2B2AE3D27D4EB4FUL);
                    var id = new TypeId { High = high, Low = low };

                    var fields = new List<FieldDesc>();
                    foreach (var f in t.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic))
                    {
                        bool serializable = f.IsPublic || f.GetCustomAttribute<SerializeField>() != null;
                        if (!serializable) continue;
                        
                        BuildFieldDescriptors(f, fields);
                    }

                    // Pin the FieldDesc array during the native call
                    IntPtr fieldsPtr = IntPtr.Zero;
                    GCHandle handle = default;
                    var arr = fields.ToArray();
                    if (arr.Length > 0)
                    {
                        handle = GCHandle.Alloc(arr, GCHandleType.Pinned);
                        fieldsPtr = Marshal.UnsafeAddrOfPinnedArrayElement(arr, 0);
                    }

                    try
                    {
                        var desc = new ScriptableTypeDesc
                        {
                            id = id,
                            fullName = Marshal.StringToCoTaskMemUTF8(t.FullName!),
                            menuPath = Marshal.StringToCoTaskMemUTF8(menuPath),
                            defaultFile = Marshal.StringToCoTaskMemUTF8(fileName),
                            order = order,
                            version = version,
                            fields = fieldsPtr,
                            fieldCount = arr.Length,
                            CreateNative = IntPtr.Zero
                        };
                        _registerType?.Invoke(ref desc);
                    }
                    finally
                    {
                        if (handle.IsAllocated) handle.Free();
                    }
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[C#] EnumerateTypes failed: {ex}");
            }
        }

        private static uint MapType(Type t)
        {
            // Must match ValueType enum in ScriptableObject.h
            if (t == typeof(bool)) return 1;    // Bool
            if (t == typeof(int)) return 2;     // Int32
            if (t == typeof(long)) return 3;    // Int64
            if (t == typeof(float)) return 4;   // Float
            if (t == typeof(double)) return 5;  // Double
            if (t == typeof(string)) return 6;  // String
            if (t == typeof(System.Numerics.Vector2)) return 7; // Vec2
            if (t == typeof(System.Numerics.Vector3)) return 8; // Vec3
            if (t == typeof(System.Numerics.Vector4)) return 9; // Vec4
            if (t == typeof(System.Numerics.Quaternion)) return 10; // Quat
            if (t.FullName == "System.Drawing.Color") return 11; // Color
            if (t.IsEnum) return 13; // Enum
            if (t == typeof(Mesh)) return 14; // Mesh asset reference
            if (t == typeof(Prefab)) return 15; // Prefab asset reference
            if (t == typeof(Texture)) return 19; // Texture asset reference
            if (t == typeof(AnimationController)) return 20; // AnimatorController asset reference
            if (t == typeof(AnimationControllerOverride)) return 21; // AnimatorControllerOverride asset reference
            if (t == typeof(AudioClip)) return 22; // Audio asset reference
            if (IsStructType(t)) return 16; // Struct
            // ResourceRef<T> - treated as ClayObject with dropdown UI
            if (t.IsGenericType && t.GetGenericTypeDefinition() == typeof(ResourceRef<>)) return 17; // ClayObject
            if (typeof(ClaymoreEngine.Scripting.Scriptable.ClayScriptableObject).IsAssignableFrom(t)) return 17; // ClayObject
            if (t == typeof(DialogueLibraryRef)) return 18; // DialogueLibrary
            return 0; // None/Unknown
        }

        private static bool IsStructType(Type t)
        {
            if (t.FullName == "System.Drawing.Color") return false;
            return t.IsValueType && !t.IsPrimitive && !t.IsEnum && t != typeof(decimal);
        }

        private static void BuildFieldDescriptors(FieldInfo f, List<FieldDesc> fields)
        {
            const uint PopulateFlag = 1u << 3;
            const uint SelectFlag = 1u << 4;

            Type fieldType = f.FieldType;
            int arrayRank = 0;
            Type elementType = fieldType;

            if (fieldType.IsArray)
            {
                arrayRank = 1;
                elementType = fieldType.GetElementType()!;
            }
            else if (fieldType.IsGenericType && fieldType.GetGenericTypeDefinition() == typeof(List<>))
            {
                arrayRank = 1;
                elementType = fieldType.GetGenericArguments()[0];
            }

            bool populateFromResources = f.GetCustomAttribute<PopulateFromResourcesAttribute>() != null ||
                                         f.GetCustomAttribute<PopulateFromResourceAttribute>() != null;
            bool selectFromResources = f.GetCustomAttribute<SelectFromResourcesAttribute>() != null ||
                                       f.GetCustomAttribute<SelectFromResourceAttribute>() != null;

            var fd = CreateFieldDesc(f.Name, elementType, arrayRank);

            // Resource flags
            if (populateFromResources) { fd.populateFromResources = 1; fd.flags |= PopulateFlag; }
            if (selectFromResources || (elementType.IsGenericType && elementType.GetGenericTypeDefinition() == typeof(ResourceRef<>)))
            { fd.selectFromResources = 1; fd.flags |= SelectFlag; }

            // Conditional visibility
            var showIf = f.GetCustomAttribute<ShowIfAttribute>();
            var hideIf = f.GetCustomAttribute<HideIfAttribute>();
            if (showIf != null)
            {
                fd.showIfField = Marshal.StringToCoTaskMemUTF8(showIf.FieldName);
                fd.showIfValue = Marshal.StringToCoTaskMemUTF8(ValuesToString(showIf.Values));
                fd.showIfMode = 0;
            }
            else if (hideIf != null)
            {
                fd.showIfField = Marshal.StringToCoTaskMemUTF8(hideIf.FieldName);
                fd.showIfValue = Marshal.StringToCoTaskMemUTF8(ValuesToString(hideIf.Values));
                fd.showIfMode = 1;
            }

            fields.Add(fd);
        }

        private static string ValuesToString(object?[] values)
        {
            if (values == null || values.Length == 0) return string.Empty;
            var parts = new string[values.Length];
            for (int i = 0; i < values.Length; i++)
                parts[i] = ValueToString(values[i]);
            return string.Join("|", parts);
        }

        private static string ValueToString(object? value)
        {
            if (value == null) return string.Empty;
            if (value is Enum e) return Convert.ToInt32(e).ToString(System.Globalization.CultureInfo.InvariantCulture);
            if (value is bool b) return b ? "true" : "false";
            if (value is int or long or float or double)
                return Convert.ToString(value, System.Globalization.CultureInfo.InvariantCulture) ?? string.Empty;
            return value.ToString() ?? string.Empty;
        }

        private static FieldDesc CreateFieldDesc(string name, Type elementType, int arrayRank)
        {
            var fd = new FieldDesc
            {
                name = Marshal.StringToCoTaskMemUTF8(name),
                // For lists, type is still the element type; arrayRank > 0 indicates it's a list
                type = MapType(elementType),
                flags = 1u,
                arrayRank = arrayRank,
                enumType = elementType.IsEnum ? Marshal.StringToCoTaskMemUTF8(elementType.FullName!) : IntPtr.Zero,
                enumNames = IntPtr.Zero,
                enumValues = IntPtr.Zero,
                listElementType = arrayRank > 0 ? MapType(elementType) : 0,
                listElementTypeName = arrayRank > 0 ? Marshal.StringToCoTaskMemUTF8(elementType.FullName!) : IntPtr.Zero,
                auxType = IntPtr.Zero,
                structFieldsJson = IntPtr.Zero,
                populateFromResources = 0,
                selectFromResources = 0,
                showIfField = IntPtr.Zero,
                showIfValue = IntPtr.Zero,
                showIfMode = 0
            };

            // Enum metadata - set for both direct enum fields AND list elements that are enums
            if (elementType.IsEnum)
            {
                var names = Enum.GetNames(elementType);
                var values = Enum.GetValues(elementType).Cast<int>().ToArray();
                fd.enumNames = Marshal.StringToCoTaskMemUTF8(string.Join("|", names));
                fd.enumValues = Marshal.StringToCoTaskMemUTF8(string.Join("|", values));
            }

            // Aux type for clay objects / structs
            if (fd.type == 17 /* ClayObject */ || fd.type == 16 /* Struct */)
            {
                // For ResourceRef<T>, use the generic argument type (T) as auxType
                Type auxTypeToUse = elementType;
                if (elementType.IsGenericType && elementType.GetGenericTypeDefinition() == typeof(ResourceRef<>))
                {
                    auxTypeToUse = elementType.GetGenericArguments()[0];
                }
                fd.auxType = Marshal.StringToCoTaskMemUTF8(auxTypeToUse.FullName!);
            }

            // Struct metadata
            if (fd.type == 16 /* Struct */)
            {
                string json = BuildStructFieldsJson(elementType);
                fd.structFieldsJson = Marshal.StringToCoTaskMemUTF8(json);
            }

            return fd;
        }

        private static string BuildStructFieldsJson(Type structType)
        {
            var fieldArray = BuildStructFieldArray(structType);
            return System.Text.Json.JsonSerializer.Serialize(fieldArray);
        }

        private static List<Dictionary<string, object?>> BuildStructFieldArray(Type structType)
        {
            var list = new List<Dictionary<string, object?>>();

            foreach (var sf in structType.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic))
            {
                bool serializable = sf.IsPublic || sf.GetCustomAttribute<SerializeField>() != null;
                if (!serializable) continue;

                Type ft = sf.FieldType;
                int arrayRank = 0;
                Type elemType = ft;
                if (ft.IsArray)
                {
                    arrayRank = 1;
                    elemType = ft.GetElementType()!;
                }
                else if (ft.IsGenericType && ft.GetGenericTypeDefinition() == typeof(List<>))
                {
                    arrayRank = 1;
                    elemType = ft.GetGenericArguments()[0];
                }

                var entry = new Dictionary<string, object?>();
                entry["name"] = sf.Name;
                entry["type"] = MapType(elemType);
                entry["flags"] = 1u;
                entry["arrayRank"] = arrayRank;
                entry["enumType"] = elemType.IsEnum ? elemType.FullName : null;
                if (elemType.IsEnum)
                {
                    entry["enumNames"] = Enum.GetNames(elemType);
                    entry["enumValues"] = Enum.GetValues(elemType).Cast<int>().ToArray();
                }
                entry["listElementType"] = arrayRank > 0 ? MapType(elemType) : 0;
                entry["listElementTypeName"] = arrayRank > 0 ? elemType.FullName : null;
                entry["auxType"] = (elemType.IsEnum || IsStructType(elemType) || typeof(ClaymoreEngine.Scripting.Scriptable.ClayScriptableObject).IsAssignableFrom(elemType))
                    ? elemType.FullName
                    : null;

                if (IsStructType(elemType))
                {
                    entry["structFields"] = BuildStructFieldArray(elemType);
                }

                bool populateAttr = sf.GetCustomAttribute<PopulateFromResourcesAttribute>() != null ||
                                    sf.GetCustomAttribute<PopulateFromResourceAttribute>() != null;
                bool selectAttr = sf.GetCustomAttribute<SelectFromResourcesAttribute>() != null ||
                                  sf.GetCustomAttribute<SelectFromResourceAttribute>() != null;
                entry["populateFromResources"] = populateAttr;
                entry["selectFromResources"] = selectAttr;

                list.Add(entry);
            }

            return list;
        }

        private static bool CreateDefaultImpl(TypeId type, IntPtr native)
        {
            // Default no-op: fields default to CLR defaults; user may add static SetDefaults later
            return true;
        }

        private static bool CustomInspectorImpl(TypeId type, IntPtr native, IntPtr drawerApi)
        {
            return false;
        }

        private static bool UpgradeImpl(TypeId type, uint fromVersion, IntPtr native)
        {
            return false;
        }

        private static ulong XXHash64(byte[] data, ulong seed)
        {
            unchecked
            {
                const ulong PRIME64_1 = 11400714785074694791UL;
                const ulong PRIME64_2 = 14029467366897019727UL;
                const ulong PRIME64_3 = 1609587929392839161UL;
                const ulong PRIME64_4 = 9650029242287828579UL;
                const ulong PRIME64_5 = 2870177450012600261UL;
                ulong h64 = seed + PRIME64_5 + (ulong)data.Length;
                int index = 0;
                while (index + 8 <= data.Length)
                {
                    ulong k1 = BitConverter.ToUInt64(data, index);
                    k1 *= PRIME64_2; k1 = Rotl64(k1, 31); k1 *= PRIME64_1; h64 ^= k1;
                    h64 = Rotl64(h64, 27) * PRIME64_1 + PRIME64_4;
                    index += 8;
                }
                if (index + 4 <= data.Length)
                {
                    h64 ^= (ulong)BitConverter.ToUInt32(data, index) * PRIME64_1;
                    h64 = Rotl64(h64, 23) * PRIME64_2 + PRIME64_3; index += 4;
                }
                while (index < data.Length)
                {
                    h64 ^= ((ulong)data[index]) * PRIME64_5; h64 = Rotl64(h64, 11) * PRIME64_1; index++;
                }
                h64 ^= h64 >> 33; h64 *= PRIME64_2; h64 ^= h64 >> 29; h64 *= PRIME64_3; h64 ^= h64 >> 32; return h64;
            }
        }
        private static ulong Rotl64(ulong x, int r) => (x << r) | (x >> (64 - r));
    }
}


