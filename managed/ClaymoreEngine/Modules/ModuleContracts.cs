using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;

namespace ClaymoreEngine.Modules
{
    [AttributeUsage(AttributeTargets.Class)]
    public sealed class ClayModuleAttribute : Attribute { }

    public interface IClayModule
    {
        string Name { get; }
        Version Version { get; }
        void Initialize(NativeAPIs native, ManagedAPIs managed);
        void Shutdown();
    }

    // Mirrors native structs for module registration
    [StructLayout(LayoutKind.Sequential)]
    public struct TypeId { public ulong High; public ulong Low; }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct InteropFieldDesc
    {
        [MarshalAs(UnmanagedType.LPStr)] public string name;
        public uint type;    // ValueType
        public uint flags;
        public int arrayRank;
        [MarshalAs(UnmanagedType.LPStr)] public string? enumType;
    }

    // Managed-friendly DTO used by module authors (fields as managed array)
    public struct InteropComponentDesc
    {
        public TypeId typeId;
        public string fullName;
        public string menuPath;
        public uint version;
        public InteropFieldDesc[] fields;
        public int order;
        public IntPtr Upgrade;
        public IntPtr CustomInspector;
    }

    // Native-compatible layout: pointer + count
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct InteropComponentDescNative
    {
        public TypeId typeId;
        [MarshalAs(UnmanagedType.LPStr)] public string fullName;
        [MarshalAs(UnmanagedType.LPStr)] public string menuPath;
        public uint version;
        public IntPtr fields; // InteropFieldDesc*
        public int fieldCount;
        public int order;
        public IntPtr Upgrade;
        public IntPtr CustomInspector;
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate bool RegisterComponentDelegate(InteropComponentDescNative desc);

    [StructLayout(LayoutKind.Sequential)]
    public struct NativeAPIs
    {
        public RegisterComponentDelegate RegisterComponent;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct ManagedAPIs
    {
        public IntPtr EnumerateComponents; // Function pointer to delegate
        // Optional hooks to be added later
    }

    // Attribute for declaring a module-provided component type
    [AttributeUsage(AttributeTargets.Class, Inherited = false)]
    public sealed class ClayComponentAttribute : Attribute
    {
        public string MenuPath { get; }
        public int Order { get; }
        public uint Version { get; }
        public ClayComponentAttribute(string menuPath, int order = 0, uint version = 1)
        { MenuPath = menuPath; Order = order; Version = version; }
    }

    // Attribute for declaring an exposed field on a component
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class ClayFieldAttribute : Attribute
    {
        public uint Flags { get; }
        public int ArrayRank { get; }
        public string? EnumType { get; }
        public ClayFieldAttribute(uint flags = 0, int arrayRank = 0, string? enumType = null)
        { Flags = flags; ArrayRank = arrayRank; EnumType = enumType; }
    }

    public static class ModuleRuntime
    {
        // Convention for TypeId hashing, stable across processes
        private static ulong Hash64(string s, ulong seed)
        {
            unchecked
            {
                const ulong PRIME64_1 = 11400714785074694791UL;
                const ulong PRIME64_2 = 14029467366897019727UL;
                const ulong PRIME64_3 = 1609587929392839161UL;
                const ulong PRIME64_4 = 9650029242287828579UL;
                const ulong PRIME64_5 = 2870177450012600261UL;
                var data = System.Text.Encoding.UTF8.GetBytes(s);
                ulong h64 = seed + PRIME64_5 + (ulong)data.Length;
                int index = 0;
                while (index + 8 <= data.Length)
                {
                    ulong k1 = BitConverter.ToUInt64(data, index);
                    k1 *= PRIME64_2; k1 = (k1 << 31) | (k1 >> 33); k1 *= PRIME64_1; h64 ^= k1;
                    h64 = ((h64 << 27) | (h64 >> 37)) * PRIME64_1 + PRIME64_4;
                    index += 8;
                }
                if (index + 4 <= data.Length)
                {
                    h64 ^= (ulong)BitConverter.ToUInt32(data, index) * PRIME64_1;
                    h64 = ((h64 << 23) | (h64 >> 41)) * PRIME64_2 + PRIME64_3; index += 4;
                }
                while (index < data.Length)
                {
                    h64 ^= ((ulong)data[index]) * PRIME64_5; h64 = ((h64 << 11) | (h64 >> 53)) * PRIME64_1; index++;
                }
                h64 ^= h64 >> 33; h64 *= PRIME64_2; h64 ^= h64 >> 29; h64 *= PRIME64_3; h64 ^= h64 >> 32; return h64;
            }
        }

        private static TypeId MakeTypeId(string fullName)
        {
            return new TypeId
            {
                High = Hash64(fullName, 0xC2B2AE3D27D4EB4FUL),
                Low = Hash64(fullName, 0x9E3779B97F4A7C15UL)
            };
        }

        public static TypeId ComputeTypeId(string fullName) => MakeTypeId(fullName);

        public static bool RegisterComponent(ref NativeAPIs native, InteropComponentDesc desc)
        {
            IntPtr fieldsPtr = IntPtr.Zero;
            try
            {
                int count = desc.fields?.Length ?? 0;
                if (count > 0)
                {
                    int sz = Marshal.SizeOf<InteropFieldDesc>();
                    fieldsPtr = Marshal.AllocHGlobal(sz * count);
                    for (int i = 0; i < count; ++i)
                    {
                        Marshal.StructureToPtr(desc.fields[i], fieldsPtr + i * sz, false);
                    }
                }
                var nativeDesc = new InteropComponentDescNative
                {
                    typeId = desc.typeId,
                    fullName = desc.fullName,
                    menuPath = desc.menuPath,
                    version = desc.version,
                    fields = fieldsPtr,
                    fieldCount = desc.fields?.Length ?? 0,
                    order = desc.order,
                    Upgrade = desc.Upgrade,
                    CustomInspector = desc.CustomInspector
                };
                return native.RegisterComponent(nativeDesc);
            }
            finally
            {
                if (fieldsPtr != IntPtr.Zero) Marshal.FreeHGlobal(fieldsPtr);
            }
        }

        private static uint MapValueType(Type t)
        {
            // Must match native cm::ValueType assignment in ModuleComponent.h
            if (t == typeof(bool)) return 0;        // Bool
            if (t == typeof(int)) return 1;         // Int
            if (t == typeof(long)) return 2;        // Int64
            if (t == typeof(float)) return 3;       // Float
            if (t == typeof(double)) return 4;      // Double
            if (t == typeof(string)) return 5;      // String
            if (t.FullName == "System.Numerics.Vector2") return 6; // Vec2
            if (t.FullName == "System.Numerics.Vector3") return 7; // Vec3
            if (t.FullName == "System.Numerics.Vector4") return 8; // Vec4
            if (t.FullName == "System.Numerics.Quaternion") return 9; // Quat
            if (t.FullName == "ClaymoreEngine.ColorRGBA") return 10; // Color (placeholder if wrapped)
            if (t.FullName == "ClaymoreEngine.EnumValue") return 12; // Enum
            // Guid maps to string by default
            return 5;
        }

        public static void EnumerateAllModulesAndRegister(ref NativeAPIs native)
        {
            try
            {
                Console.WriteLine("[C#] EnumerateAllModulesAndRegister starting...");
                var modules = new List<(IClayModule module, Assembly asm)>();
                
                var assemblies = AppDomain.CurrentDomain.GetAssemblies();
                Console.WriteLine($"[C#] Found {assemblies.Length} assemblies to scan");
                
                foreach (var asm in assemblies)
                {
                    try
                    {
                        Console.WriteLine($"[C#] Scanning assembly: {asm.FullName}");
                        var types = asm.GetTypes();
                        Console.WriteLine($"[C#] Found {types.Length} types in {asm.GetName().Name}");
                        
                        foreach (var t in types)
                        {
                            try
                            {
                                if (t.GetCustomAttribute<ClayModuleAttribute>() == null) continue;
                                if (!typeof(IClayModule).IsAssignableFrom(t)) continue;
                                if (t.IsAbstract) continue;
                                
                                Console.WriteLine($"[C#] Found module class: {t.FullName}");
                                var mod = (IClayModule?)Activator.CreateInstance(t);
                                if (mod != null) 
                                {
                                    modules.Add((mod, asm));
                                    Console.WriteLine($"[C#] Created module instance: {mod.Name}");
                                }
                            }
                            catch (Exception ex)
                            {
                                Console.WriteLine($"[C#] Error processing type {t.FullName}: {ex.Message}");
                            }
                        }
                    }
                    catch (Exception ex)
                    {
                        Console.WriteLine($"[C#] Error scanning assembly {asm.FullName}: {ex.Message}");
                    }
                }
                
                Console.WriteLine($"[C#] Found {modules.Count} modules to register");

                foreach (var (module, asm) in modules)
            {
                try
                {
                    // Let module customize if needed
                    module.Initialize(native, new ManagedAPIs());

                    // Discover component classes in the same assembly
                    foreach (var ct in asm.GetTypes())
                    {
                        var compAttr = ct.GetCustomAttribute<ClayComponentAttribute>();
                        if (compAttr == null) continue;
                        string fullName = ct.FullName ?? ct.Name;
                        var id = MakeTypeId(fullName);

                        var fieldList = new List<InteropFieldDesc>();
                        foreach (var f in ct.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic))
                        {
                            // Require ClayField or [SerializeField] for exposure
                            var fa = f.GetCustomAttribute<ClayFieldAttribute>();
                            var ser = f.GetCustomAttribute<System.SerializableAttribute>();
                            if (fa == null && ser == null) continue;

                            fieldList.Add(new InteropFieldDesc
                            {
                                name = f.Name,
                                type = MapValueType(f.FieldType),
                                flags = fa?.Flags ?? 0,
                                arrayRank = fa?.ArrayRank ?? 0,
                                enumType = fa?.EnumType
                            });
                        }

                        // Marshal fields array manually to native-compatible layout
                        IntPtr fieldsPtr = IntPtr.Zero;
                        try
                        {
                            int sz = Marshal.SizeOf<InteropFieldDesc>();
                            fieldsPtr = Marshal.AllocHGlobal(sz * fieldList.Count);
                            for (int i = 0; i < fieldList.Count; ++i)
                            {
                                IntPtr slot = fieldsPtr + i * sz;
                                Marshal.StructureToPtr(fieldList[i], slot, false);
                            }
                            var nativeDesc = new InteropComponentDescNative
                            {
                                typeId = id,
                                fullName = fullName,
                                menuPath = compAttr.MenuPath,
                                version = compAttr.Version,
                                fields = fieldsPtr,
                                fieldCount = fieldList.Count,
                                order = compAttr.Order,
                                Upgrade = IntPtr.Zero,
                                CustomInspector = IntPtr.Zero
                            };
                            native.RegisterComponent(nativeDesc);
                        }
                        finally
                        {
                            if (fieldsPtr != IntPtr.Zero)
                                Marshal.FreeHGlobal(fieldsPtr);
                        }
                    }
                }
                catch (Exception ex)
                {
                    Console.WriteLine($"[C#] Module registration failed for {module.Name}: {ex.Message}");
                }
            }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[C#] EnumerateAllModulesAndRegister failed: {ex}");
            }
        }
    }
}


