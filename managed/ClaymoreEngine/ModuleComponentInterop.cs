using System;
using System.Collections.Generic;
using System.Numerics;
using System.Reflection;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    // Delegates for module component interop
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate bool HasModuleComponentFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string typeName);
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void AddModuleComponentFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string typeName);
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void RemoveModuleComponentFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string typeName);
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate IntPtr GetModuleComponentFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string typeName);
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate IntPtr GetModuleComponentByFullNameFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string fullName);
    
    // Field value getters
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate bool GetModuleFieldBoolFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string typeName, [MarshalAs(UnmanagedType.LPStr)] string fieldName);
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate int GetModuleFieldIntFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string typeName, [MarshalAs(UnmanagedType.LPStr)] string fieldName);
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate long GetModuleFieldInt64Fn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string typeName, [MarshalAs(UnmanagedType.LPStr)] string fieldName);
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate float GetModuleFieldFloatFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string typeName, [MarshalAs(UnmanagedType.LPStr)] string fieldName);
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate double GetModuleFieldDoubleFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string typeName, [MarshalAs(UnmanagedType.LPStr)] string fieldName);
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate IntPtr GetModuleFieldStringFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string typeName, [MarshalAs(UnmanagedType.LPStr)] string fieldName);
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void GetModuleFieldVec2Fn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string typeName, [MarshalAs(UnmanagedType.LPStr)] string fieldName, out float x, out float y);
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void GetModuleFieldVec3Fn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string typeName, [MarshalAs(UnmanagedType.LPStr)] string fieldName, out float x, out float y, out float z);
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void GetModuleFieldVec4Fn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string typeName, [MarshalAs(UnmanagedType.LPStr)] string fieldName, out float x, out float y, out float z, out float w);
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void GetModuleFieldQuatFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string typeName, [MarshalAs(UnmanagedType.LPStr)] string fieldName, out float x, out float y, out float z, out float w);
    
    // Field value setters
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void SetModuleFieldBoolFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string typeName, [MarshalAs(UnmanagedType.LPStr)] string fieldName, [MarshalAs(UnmanagedType.I1)] bool value);
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void SetModuleFieldIntFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string typeName, [MarshalAs(UnmanagedType.LPStr)] string fieldName, int value);
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void SetModuleFieldInt64Fn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string typeName, [MarshalAs(UnmanagedType.LPStr)] string fieldName, long value);
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void SetModuleFieldFloatFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string typeName, [MarshalAs(UnmanagedType.LPStr)] string fieldName, float value);
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void SetModuleFieldDoubleFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string typeName, [MarshalAs(UnmanagedType.LPStr)] string fieldName, double value);
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void SetModuleFieldStringFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string typeName, [MarshalAs(UnmanagedType.LPStr)] string fieldName, [MarshalAs(UnmanagedType.LPStr)] string value);
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void SetModuleFieldVec2Fn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string typeName, [MarshalAs(UnmanagedType.LPStr)] string fieldName, float x, float y);
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void SetModuleFieldVec3Fn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string typeName, [MarshalAs(UnmanagedType.LPStr)] string fieldName, float x, float y, float z);
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void SetModuleFieldVec4Fn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string typeName, [MarshalAs(UnmanagedType.LPStr)] string fieldName, float x, float y, float z, float w);
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void SetModuleFieldQuatFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string typeName, [MarshalAs(UnmanagedType.LPStr)] string fieldName, float x, float y, float z, float w);

    internal static unsafe class ModuleComponentInterop
    {
        // Function pointers
        public static HasModuleComponentFn HasModuleComponent;
        public static AddModuleComponentFn AddModuleComponent;
        public static RemoveModuleComponentFn RemoveModuleComponent;
        public static GetModuleComponentFn GetModuleComponent;
        public static GetModuleComponentByFullNameFn GetModuleComponentByFullName;
        
        // Getters
        public static GetModuleFieldBoolFn GetModuleFieldBool;
        public static GetModuleFieldIntFn GetModuleFieldInt;
        public static GetModuleFieldInt64Fn GetModuleFieldInt64;
        public static GetModuleFieldFloatFn GetModuleFieldFloat;
        public static GetModuleFieldDoubleFn GetModuleFieldDouble;
        public static GetModuleFieldStringFn GetModuleFieldStringInternal;
        public static GetModuleFieldVec2Fn GetModuleFieldVec2;
        public static GetModuleFieldVec3Fn GetModuleFieldVec3;
        public static GetModuleFieldVec4Fn GetModuleFieldVec4;
        public static GetModuleFieldQuatFn GetModuleFieldQuat;
        
        // Setters
        public static SetModuleFieldBoolFn SetModuleFieldBool;
        public static SetModuleFieldIntFn SetModuleFieldInt;
        public static SetModuleFieldInt64Fn SetModuleFieldInt64;
        public static SetModuleFieldFloatFn SetModuleFieldFloat;
        public static SetModuleFieldDoubleFn SetModuleFieldDouble;
        public static SetModuleFieldStringFn SetModuleFieldString;
        public static SetModuleFieldVec2Fn SetModuleFieldVec2;
        public static SetModuleFieldVec3Fn SetModuleFieldVec3;
        public static SetModuleFieldVec4Fn SetModuleFieldVec4;
        public static SetModuleFieldQuatFn SetModuleFieldQuat;

        public static string GetModuleFieldString(int entityId, string typeName, string fieldName)
        {
            IntPtr ptr = GetModuleFieldStringInternal(entityId, typeName, fieldName);
            return Marshal.PtrToStringAnsi(ptr);
        }

        private static T SafeGetDelegate<T>(IntPtr ptr, int index, string name) where T : Delegate
        {
            if (ptr == IntPtr.Zero)
            {
                Console.WriteLine($"[ModuleComponentInterop] WARNING: Null pointer at index {index} for {name}");
                return null;
            }
            try
            {
                return Marshal.GetDelegateForFunctionPointer<T>(ptr);
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[ModuleComponentInterop] ERROR creating delegate at index {index} for {name}: {ex.Message}");
                return null;
            }
        }

        public static void Initialize(void** ptrs, int count)
        {
            Console.WriteLine($"[ModuleComponentInterop] Initialize called with count={count}");
            
            if (count < 25)
            {
                Console.WriteLine($"[ModuleComponentInterop] Expected at least 25 function pointers, but got {count}.");
                return;
            }

            int i = 0;
            HasModuleComponent = SafeGetDelegate<HasModuleComponentFn>((IntPtr)ptrs[i], i, "HasModuleComponent"); i++;
            AddModuleComponent = SafeGetDelegate<AddModuleComponentFn>((IntPtr)ptrs[i], i, "AddModuleComponent"); i++;
            RemoveModuleComponent = SafeGetDelegate<RemoveModuleComponentFn>((IntPtr)ptrs[i], i, "RemoveModuleComponent"); i++;
            GetModuleComponent = SafeGetDelegate<GetModuleComponentFn>((IntPtr)ptrs[i], i, "GetModuleComponent"); i++;
            GetModuleComponentByFullName = SafeGetDelegate<GetModuleComponentByFullNameFn>((IntPtr)ptrs[i], i, "GetModuleComponentByFullName"); i++;
            Console.WriteLine($"[ModuleComponentInterop] After core funcs i={i}");
            
            GetModuleFieldBool = SafeGetDelegate<GetModuleFieldBoolFn>((IntPtr)ptrs[i], i, "GetModuleFieldBool"); i++;
            GetModuleFieldInt = SafeGetDelegate<GetModuleFieldIntFn>((IntPtr)ptrs[i], i, "GetModuleFieldInt"); i++;
            GetModuleFieldInt64 = SafeGetDelegate<GetModuleFieldInt64Fn>((IntPtr)ptrs[i], i, "GetModuleFieldInt64"); i++;
            GetModuleFieldFloat = SafeGetDelegate<GetModuleFieldFloatFn>((IntPtr)ptrs[i], i, "GetModuleFieldFloat"); i++;
            GetModuleFieldDouble = SafeGetDelegate<GetModuleFieldDoubleFn>((IntPtr)ptrs[i], i, "GetModuleFieldDouble"); i++;
            GetModuleFieldStringInternal = SafeGetDelegate<GetModuleFieldStringFn>((IntPtr)ptrs[i], i, "GetModuleFieldStringInternal"); i++;
            GetModuleFieldVec2 = SafeGetDelegate<GetModuleFieldVec2Fn>((IntPtr)ptrs[i], i, "GetModuleFieldVec2"); i++;
            GetModuleFieldVec3 = SafeGetDelegate<GetModuleFieldVec3Fn>((IntPtr)ptrs[i], i, "GetModuleFieldVec3"); i++;
            GetModuleFieldVec4 = SafeGetDelegate<GetModuleFieldVec4Fn>((IntPtr)ptrs[i], i, "GetModuleFieldVec4"); i++;
            GetModuleFieldQuat = SafeGetDelegate<GetModuleFieldQuatFn>((IntPtr)ptrs[i], i, "GetModuleFieldQuat"); i++;
            Console.WriteLine($"[ModuleComponentInterop] After getters i={i}");
            
            SetModuleFieldBool = SafeGetDelegate<SetModuleFieldBoolFn>((IntPtr)ptrs[i], i, "SetModuleFieldBool"); i++;
            SetModuleFieldInt = SafeGetDelegate<SetModuleFieldIntFn>((IntPtr)ptrs[i], i, "SetModuleFieldInt"); i++;
            SetModuleFieldInt64 = SafeGetDelegate<SetModuleFieldInt64Fn>((IntPtr)ptrs[i], i, "SetModuleFieldInt64"); i++;
            SetModuleFieldFloat = SafeGetDelegate<SetModuleFieldFloatFn>((IntPtr)ptrs[i], i, "SetModuleFieldFloat"); i++;
            SetModuleFieldDouble = SafeGetDelegate<SetModuleFieldDoubleFn>((IntPtr)ptrs[i], i, "SetModuleFieldDouble"); i++;
            SetModuleFieldString = SafeGetDelegate<SetModuleFieldStringFn>((IntPtr)ptrs[i], i, "SetModuleFieldString"); i++;
            SetModuleFieldVec2 = SafeGetDelegate<SetModuleFieldVec2Fn>((IntPtr)ptrs[i], i, "SetModuleFieldVec2"); i++;
            SetModuleFieldVec3 = SafeGetDelegate<SetModuleFieldVec3Fn>((IntPtr)ptrs[i], i, "SetModuleFieldVec3"); i++;
            SetModuleFieldVec4 = SafeGetDelegate<SetModuleFieldVec4Fn>((IntPtr)ptrs[i], i, "SetModuleFieldVec4"); i++;
            SetModuleFieldQuat = SafeGetDelegate<SetModuleFieldQuatFn>((IntPtr)ptrs[i], i, "SetModuleFieldQuat"); i++;
            Console.WriteLine($"[ModuleComponentInterop] Initialize complete i={i}");
        }
        
        /// <summary>
        /// Synchronizes managed component property values to native ModuleComponent storage
        /// </summary>
        internal static void SyncComponentToNative(int entityId, string typeName, ModuleComponentBase component)
        {
            var properties = component.GetSyncableProperties();
            
            foreach (var kvp in properties)
            {
                var fieldName = kvp.Key;
                var property = kvp.Value;
                var value = property.GetValue(component);
                
                if (value == null) continue;
                
                SetFieldValue(entityId, typeName, fieldName, value, property.PropertyType);
            }
        }
        
        /// <summary>
        /// Synchronizes native ModuleComponent storage values to managed component properties
        /// </summary>
        internal static void SyncComponentFromNative(int entityId, string typeName, ModuleComponentBase component)
        {
            var properties = component.GetSyncableProperties();
            
            foreach (var kvp in properties)
            {
                var fieldName = kvp.Key;
                var property = kvp.Value;
                
                var value = GetFieldValue(entityId, typeName, fieldName, property.PropertyType);
                if (value != null)
                {
                    property.SetValue(component, value);
                }
            }
        }
        
        private static void SetFieldValue(int entityId, string typeName, string fieldName, object value, Type type)
        {
            if (type == typeof(bool))
                SetModuleFieldBool(entityId, typeName, fieldName, (bool)value);
            else if (type == typeof(int))
                SetModuleFieldInt(entityId, typeName, fieldName, (int)value);
            else if (type == typeof(long))
                SetModuleFieldInt64(entityId, typeName, fieldName, (long)value);
            else if (type == typeof(float))
                SetModuleFieldFloat(entityId, typeName, fieldName, (float)value);
            else if (type == typeof(double))
                SetModuleFieldDouble(entityId, typeName, fieldName, (double)value);
            else if (type == typeof(string))
                SetModuleFieldString(entityId, typeName, fieldName, (string)value);
            else if (type == typeof(Vector2))
            {
                var v = (Vector2)value;
                SetModuleFieldVec2(entityId, typeName, fieldName, v.X, v.Y);
            }
            else if (type == typeof(Vector3))
            {
                var v = (Vector3)value;
                SetModuleFieldVec3(entityId, typeName, fieldName, v.X, v.Y, v.Z);
            }
            else if (type == typeof(Vector4))
            {
                var v = (Vector4)value;
                SetModuleFieldVec4(entityId, typeName, fieldName, v.X, v.Y, v.Z, v.W);
            }
            else if (type == typeof(Quaternion))
            {
                var q = (Quaternion)value;
                SetModuleFieldQuat(entityId, typeName, fieldName, q.X, q.Y, q.Z, q.W);
            }
        }
        
        private static object GetFieldValue(int entityId, string typeName, string fieldName, Type type)
        {
            if (type == typeof(bool))
                return GetModuleFieldBool(entityId, typeName, fieldName);
            else if (type == typeof(int))
                return GetModuleFieldInt(entityId, typeName, fieldName);
            else if (type == typeof(long))
                return GetModuleFieldInt64(entityId, typeName, fieldName);
            else if (type == typeof(float))
                return GetModuleFieldFloat(entityId, typeName, fieldName);
            else if (type == typeof(double))
                return GetModuleFieldDouble(entityId, typeName, fieldName);
            else if (type == typeof(string))
                return GetModuleFieldString(entityId, typeName, fieldName);
            else if (type == typeof(Vector2))
            {
                GetModuleFieldVec2(entityId, typeName, fieldName, out float x, out float y);
                return new Vector2(x, y);
            }
            else if (type == typeof(Vector3))
            {
                GetModuleFieldVec3(entityId, typeName, fieldName, out float x, out float y, out float z);
                return new Vector3(x, y, z);
            }
            else if (type == typeof(Vector4))
            {
                GetModuleFieldVec4(entityId, typeName, fieldName, out float x, out float y, out float z, out float w);
                return new Vector4(x, y, z, w);
            }
            else if (type == typeof(Quaternion))
            {
                GetModuleFieldQuat(entityId, typeName, fieldName, out float x, out float y, out float z, out float w);
                return new Quaternion(x, y, z, w);
            }
            
            return null;
        }
    }
}
