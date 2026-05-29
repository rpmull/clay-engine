using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Drawing;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text.Json;
using ClaymoreEngine; // For Mesh type

namespace ClaymoreEngine.Scripting.Scriptable
{
    public abstract class ClayScriptableObject
    {
        internal IntPtr __native;
        
        /// <summary>
        /// The GUID of this asset. Set when loaded from a .clayobj file.
        /// </summary>
        public string? Guid { get; internal set; }
        
        protected virtual void OnEnable() { }
        protected virtual void OnDisable() { }
    }
    
    /// <summary>
    /// Cache and loader for ClayScriptableObject instances.
    /// </summary>
    public static class ClayScriptableObjectLoader
    {
        private static readonly Dictionary<string, ClayScriptableObject> _cache = new();
        private static Func<string, string?>? _getPathForGuid;
        private static Func<string, string?>? _readFileContents;
        
        /// <summary>
        /// Initialize the loader with the native GetPathForGUID function.
        /// </summary>
        public static void Initialize(Func<string, string?> getPathForGuid)
        {
            _getPathForGuid = getPathForGuid;
        }

        /// <summary>
        /// Resolve a resource path from a GUID using the native resource map.
        /// Returns null when the GUID is unknown or the resolver is unavailable.
        /// </summary>
        public static string? GetPathForGuid(string guid)
        {
            if (string.IsNullOrWhiteSpace(guid))
                return null;
            return _getPathForGuid?.Invoke(guid);
        }
        
        /// <summary>
        /// Initialize VFS file reading (for PAK support at runtime).
        /// </summary>
        public static void InitializeVFS(Func<string, string?> readFileContents)
        {
            _readFileContents = readFileContents;
        }
        
        /// <summary>
        /// Load or get cached ClayScriptableObject by GUID.
        /// </summary>
        public static ClayScriptableObject? Load(string guid, Type expectedType)
        {
            if (string.IsNullOrEmpty(guid)) return null;
            
            // Validate GUID format - should be hex characters and dashes only
            // Valid formats: "abc123..." (32 chars) or "abc-123-..." (with dashes)
            if (!IsValidGuidFormat(guid))
            {
                Console.WriteLine($"[ClayScriptableObjectLoader] Invalid GUID format (len={guid.Length}): {guid.Substring(0, Math.Min(20, guid.Length))}...");
                return null;
            }
            
            // Check cache first
            if (_cache.TryGetValue(guid, out var cached))
            {
                Console.WriteLine($"[ClayScriptableObjectLoader] Returning cached instance for {guid}");
                if (expectedType.IsAssignableFrom(cached.GetType()))
                    return cached;
            }
            
            // Get path from GUID via interop
            string? path = _getPathForGuid?.Invoke(guid);
            if (string.IsNullOrEmpty(path))
            {
                Console.WriteLine($"[ClayScriptableObjectLoader] No path found for GUID: {guid}");
                return null;
            }
            
            // Load from file
            var loaded = LoadFromFile(path, expectedType);
            if (loaded != null)
            {
                loaded.Guid = guid;
                _cache[guid] = loaded;
            }
            return loaded;
        }
        
        private static bool IsValidGuidFormat(string guid)
        {
            if (string.IsNullOrEmpty(guid)) return false;
            // GUIDs should be reasonable length (32 hex chars, possibly with dashes)
            if (guid.Length < 16 || guid.Length > 40) return false;
            // Check for valid hex/dash characters
            foreach (char c in guid)
            {
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == '-'))
                    return false;
            }
            return true;
        }
        
        /// <summary>
        /// Load a ClayScriptableObject from a .clayobj JSON file.
        /// Uses VFS when available (for PAK support at runtime).
        /// </summary>
        private static ClayScriptableObject? LoadFromFile(string path, Type expectedType)
        {
            string? json = null;

            try
            {
                // Try VFS first (works with PAK files at runtime)
                if (_readFileContents != null)
                {
                    json = _readFileContents(path);
                }

                // Fallback to direct file access (editor mode)
                if (string.IsNullOrEmpty(json))
                {
                    if (!File.Exists(path))
                    {
                        Console.WriteLine($"[ClayScriptableObjectLoader] File not found: {path}");
                        return null;
                    }
                    json = File.ReadAllText(path);
                }

                if (string.IsNullOrEmpty(json))
                {
                    Console.WriteLine($"[ClayScriptableObjectLoader] Failed to read file: {path}");
                    return null;
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[ClayScriptableObjectLoader] Read failed for {path}: {ex.Message}");
                return null;
            }

            try
            {
                using var doc = JsonDocument.Parse(json);
                var root = doc.RootElement;
                
                // Get type name from JSON
                string typeName = "";
                if (root.TryGetProperty("typeName", out var typeNameProp))
                {
                    typeName = typeNameProp.GetString() ?? "";
                }
                
                if (string.IsNullOrEmpty(typeName))
                {
                    Console.WriteLine($"[ClayScriptableObjectLoader] No typeName in asset: {path}");
                    return null;
                }
                
                // Find the type
                Type? assetType = FindType(typeName);
                if (assetType == null)
                {
                    Console.WriteLine($"[ClayScriptableObjectLoader] Type not found: {typeName}");
                    return null;
                }
                
                // Verify type compatibility
                if (!expectedType.IsAssignableFrom(assetType))
                {
                    Console.WriteLine($"[ClayScriptableObjectLoader] Type mismatch: expected {expectedType.FullName}, got {typeName}");
                    return null;
                }
                
                // Create instance
                var instance = Activator.CreateInstance(assetType) as ClayScriptableObject;
                if (instance == null)
                {
                    Console.WriteLine($"[ClayScriptableObjectLoader] Failed to create instance of {typeName}");
                    return null;
                }
                
                // Deserialize fields from JSON "fields" object
                if (root.TryGetProperty("fields", out var fieldsObj))
                {
                    DeserializeFields(instance, assetType, fieldsObj);
                }
                
                return instance;
            }
            catch (JsonException)
            {
                // If JSON parsing fails (often due to encoding mismatches when using VFS),
                // try a direct disk read fallback in editor mode before giving up.
                try
                {
                    if (File.Exists(path))
                    {
                        string diskJson = File.ReadAllText(path);
                        using var doc = JsonDocument.Parse(diskJson);
                        var root = doc.RootElement;

                        string typeName = "";
                        if (root.TryGetProperty("typeName", out var typeNameProp))
                        {
                            typeName = typeNameProp.GetString() ?? "";
                        }

                        if (string.IsNullOrEmpty(typeName))
                        {
                            Console.WriteLine($"[ClayScriptableObjectLoader] No typeName in asset: {path}");
                            return null;
                        }

                        Type? assetType = FindType(typeName);
                        if (assetType == null)
                        {
                            Console.WriteLine($"[ClayScriptableObjectLoader] Type not found: {typeName}");
                            return null;
                        }

                        if (!expectedType.IsAssignableFrom(assetType))
                        {
                            Console.WriteLine($"[ClayScriptableObjectLoader] Type mismatch: expected {expectedType.FullName}, got {typeName}");
                            return null;
                        }

                        var instance = Activator.CreateInstance(assetType) as ClayScriptableObject;
                        if (instance == null)
                        {
                            Console.WriteLine($"[ClayScriptableObjectLoader] Failed to create instance of {typeName}");
                            return null;
                        }

                        if (root.TryGetProperty("fields", out var fieldsObj))
                        {
                            DeserializeFields(instance, assetType, fieldsObj);
                        }

                        return instance;
                    }
                }
                catch (Exception fallbackEx)
                {
                    Console.WriteLine($"[ClayScriptableObjectLoader] Fallback load failed for {path}: {fallbackEx.Message}");
                }

                Console.WriteLine($"[ClayScriptableObjectLoader] Load failed for {path}: invalid JSON content.");
                return null;
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[ClayScriptableObjectLoader] Load failed for {path}: {ex.Message}");
                return null;
            }
        }
        
        private static void DeserializeFields(ClayScriptableObject instance, Type type, JsonElement fields)
        {
            foreach (var field in type.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic))
            {
                bool isSerializable = field.IsPublic || field.GetCustomAttribute<SerializeField>() != null;
                if (!isSerializable) continue;
                
                if (!fields.TryGetProperty(field.Name, out var value)) continue;
                
                try
                {
                    object? deserializedValue = DeserializeValue(field.FieldType, value);
                    if (deserializedValue != null)
                    {
                        field.SetValue(instance, deserializedValue);
                    }
                }
                catch (Exception ex)
                {
                    Console.WriteLine($"[ClayScriptableObjectLoader] Field {field.Name} deserialize failed: {ex.Message}");
                }
            }
        }
        
        private static object? DeserializeValue(Type targetType, JsonElement element)
        {
            if (targetType == typeof(int))
                return element.ValueKind == JsonValueKind.Null ? 0 : element.GetInt32();
            if (targetType == typeof(float))
            {
                if (element.ValueKind == JsonValueKind.Null) return 0f;
                if (element.ValueKind == JsonValueKind.Number) return element.GetSingle();
                if (element.ValueKind == JsonValueKind.String &&
                    float.TryParse(element.GetString(), System.Globalization.NumberStyles.Float,
                        System.Globalization.CultureInfo.InvariantCulture, out float parsedFloat))
                {
                    return parsedFloat;
                }
                return 0f;
            }
            if (targetType == typeof(double))
            {
                if (element.ValueKind == JsonValueKind.Null) return 0d;
                if (element.ValueKind == JsonValueKind.Number) return element.GetDouble();
                if (element.ValueKind == JsonValueKind.String &&
                    double.TryParse(element.GetString(), System.Globalization.NumberStyles.Float,
                        System.Globalization.CultureInfo.InvariantCulture, out double parsedDouble))
                {
                    return parsedDouble;
                }
                return 0d;
            }
            if (targetType == typeof(bool))
            {
                if (element.ValueKind == JsonValueKind.Null) return false;
                if (element.ValueKind == JsonValueKind.True || element.ValueKind == JsonValueKind.False)
                    return element.GetBoolean();
                if (element.ValueKind == JsonValueKind.Number)
                    return element.GetInt32() != 0;
                if (element.ValueKind == JsonValueKind.String &&
                    bool.TryParse(element.GetString(), out bool parsedBool))
                {
                    return parsedBool;
                }
                return false;
            }
            if (targetType == typeof(string))
                return element.GetString();
            if (targetType == typeof(long))
                return element.GetInt64();
            
            // Enum support - convert int value to enum
            if (targetType.IsEnum)
            {
                if (element.ValueKind == JsonValueKind.Null)
                    return Enum.ToObject(targetType, 0);

                int enumValue = 0;
                if (element.ValueKind == JsonValueKind.Number)
                {
                    enumValue = element.GetInt32();
                }
                else if (element.ValueKind == JsonValueKind.String)
                {
                    string? enumText = element.GetString();
                    if (!string.IsNullOrEmpty(enumText))
                    {
                        if (int.TryParse(enumText, out int parsed))
                        {
                            enumValue = parsed;
                        }
                        else
                        {
                            try
                            {
                                return Enum.Parse(targetType, enumText, ignoreCase: true);
                            }
                            catch
                            {
                                enumValue = 0;
                            }
                        }
                    }
                }
                return Enum.ToObject(targetType, enumValue);
            }
            
            // Mesh reference type
            if (targetType == typeof(Mesh))
            {
                return DeserializeMesh(element);
            }
            
            // Texture reference type
            if (targetType == typeof(Texture))
            {
                return DeserializeTexture(element);
            }
            
            // Prefab reference type
            if (targetType == typeof(Prefab))
            {
                return DeserializePrefab(element);
            }

            if (targetType == typeof(AudioClip))
            {
                return DeserializeAudioClip(element);
            }

            if (targetType == typeof(AnimationController))
            {
                if (element.ValueKind == JsonValueKind.String)
                {
                    return new AnimationController { Path = element.GetString() ?? string.Empty };
                }
                if (element.ValueKind == JsonValueKind.Object &&
                    element.TryGetProperty("path", out var pathProp) &&
                    pathProp.ValueKind == JsonValueKind.String)
                {
                    return new AnimationController { Path = pathProp.GetString() ?? string.Empty };
                }
                return new AnimationController();
            }

            if (targetType == typeof(AnimationControllerOverride))
            {
                if (element.ValueKind == JsonValueKind.String)
                {
                    return new AnimationControllerOverride { Path = element.GetString() ?? string.Empty };
                }
                if (element.ValueKind == JsonValueKind.Object &&
                    element.TryGetProperty("path", out var pathProp) &&
                    pathProp.ValueKind == JsonValueKind.String)
                {
                    return new AnimationControllerOverride { Path = pathProp.GetString() ?? string.Empty };
                }
                return new AnimationControllerOverride();
            }

            // Dialogue library reference
            if (targetType == typeof(DialogueLibraryRef))
            {
                if (element.ValueKind == JsonValueKind.String)
                {
                    return new DialogueLibraryRef(element.GetString() ?? string.Empty);
                }
            }
            
            // Vector3 support
            if (targetType == typeof(System.Numerics.Vector3))
            {
                return DeserializeVector3(element);
            }

            // Color support
            if (targetType == typeof(Color))
            {
                return DeserializeColor(element);
            }
            
            // ResourceRef<T> support - deserialize from GUID string
            if (targetType.IsGenericType && targetType.GetGenericTypeDefinition() == typeof(ResourceRef<>))
            {
                return DeserializeResourceRef(targetType, element);
            }
            
            // Nested ClayScriptableObject reference (by GUID)
            if (typeof(ClayScriptableObject).IsAssignableFrom(targetType))
            {
                // Element should be a GUID string
                string? guid = element.ValueKind == JsonValueKind.String ? element.GetString() : null;
                if (!string.IsNullOrEmpty(guid))
                {
                    return Load(guid, targetType);
                }
                return null;
            }
            
            // Generic List support
            if (targetType.IsGenericType && targetType.GetGenericTypeDefinition() == typeof(List<>))
            {
                return DeserializeList(targetType, element);
            }

            // Serializable struct (value type)
            if (targetType.IsValueType && !targetType.IsPrimitive && !targetType.IsEnum && targetType != typeof(decimal))
            {
                return DeserializeStruct(targetType, element);
            }
            
            return null;
        }
        
        private static Mesh DeserializeMesh(JsonElement element)
        {
            var mesh = new Mesh();
            
            // Handle object format: { "guid": "...", "fileID": 0 }
            if (element.ValueKind == JsonValueKind.Object)
            {
                if (element.TryGetProperty("guid", out var guidProp))
                    mesh.Guid = guidProp.GetString();
                if (element.TryGetProperty("fileID", out var fileIdProp))
                    mesh.FileID = fileIdProp.GetInt32();
            }
            // Handle string format: "guid:fileID" or just "guid"
            else if (element.ValueKind == JsonValueKind.String)
            {
                mesh = Mesh.FromInteropString(element.GetString());
            }
            
            return mesh;
        }
        
        private static Texture DeserializeTexture(JsonElement element)
        {
            var texture = new Texture();
            
            // Handle object format: { "guid": "...", "fileID": 0 }
            if (element.ValueKind == JsonValueKind.Object)
            {
                if (element.TryGetProperty("guid", out var guidProp))
                    texture.Guid = guidProp.GetString();
                if (element.TryGetProperty("fileID", out var fileIdProp))
                    texture.FileID = fileIdProp.GetInt32();
            }
            // Handle string format: "guid:fileID" or just "guid"
            else if (element.ValueKind == JsonValueKind.String)
            {
                texture = Texture.FromInteropString(element.GetString());
            }
            
            return texture;
        }
        
        private static Prefab DeserializePrefab(JsonElement element)
        {
            var prefab = new Prefab();
            
            // Handle string format: GUID string
            if (element.ValueKind == JsonValueKind.String)
            {
                prefab.Guid = element.GetString();
            }
            // Handle object format: { "guid": "..." }
            else if (element.ValueKind == JsonValueKind.Object)
            {
                if (element.TryGetProperty("guid", out var guidProp))
                    prefab.Guid = guidProp.GetString();
            }
            
            return prefab;
        }

        private static AudioClip DeserializeAudioClip(JsonElement element)
        {
            var clip = new AudioClip();

            if (element.ValueKind == JsonValueKind.Object)
            {
                if (element.TryGetProperty("guid", out var guidProp))
                    clip.Guid = guidProp.GetString();
            }
            else if (element.ValueKind == JsonValueKind.String)
            {
                clip = AudioClip.FromInteropString(element.GetString());
            }

            return clip;
        }
        
        private static object? DeserializeResourceRef(Type resourceRefType, JsonElement element)
        {
            // Get the generic argument type (T in ResourceRef<T>)
            Type[] genericArgs = resourceRefType.GetGenericArguments();
            if (genericArgs.Length != 1) return null;
            Type targetType = genericArgs[0];
            
            // Create instance of ResourceRef<T>
            object? resourceRefInstance = Activator.CreateInstance(resourceRefType);
            if (resourceRefInstance == null) return null;
            
            // Handle string format: GUID string
            string? guid = null;
            if (element.ValueKind == JsonValueKind.String)
            {
                guid = element.GetString();
            }
            // Handle object format: { "guid": "..." } or { "_guid": "..." }
            else if (element.ValueKind == JsonValueKind.Object)
            {
                if (element.TryGetProperty("guid", out var guidProp))
                    guid = guidProp.GetString();
                else if (element.TryGetProperty("_guid", out var guidProp2))
                    guid = guidProp2.GetString();
            }
            
            // Set the GUID property on ResourceRef<T>
            if (!string.IsNullOrEmpty(guid))
            {
                PropertyInfo? guidProperty = resourceRefType.GetProperty("Guid");
                if (guidProperty != null && guidProperty.CanWrite)
                {
                    guidProperty.SetValue(resourceRefInstance, guid);
                }
            }
            
            return resourceRefInstance;
        }
        
        private static System.Numerics.Vector3 DeserializeVector3(JsonElement element)
        {
            if (element.ValueKind == JsonValueKind.Object)
            {
                float x = element.TryGetProperty("x", out var xp) ? xp.GetSingle() : 0f;
                float y = element.TryGetProperty("y", out var yp) ? yp.GetSingle() : 0f;
                float z = element.TryGetProperty("z", out var zp) ? zp.GetSingle() : 0f;
                return new System.Numerics.Vector3(x, y, z);
            }
            else if (element.ValueKind == JsonValueKind.Array)
            {
                var arr = element.EnumerateArray().ToArray();
                float x = arr.Length > 0 ? arr[0].GetSingle() : 0f;
                float y = arr.Length > 1 ? arr[1].GetSingle() : 0f;
                float z = arr.Length > 2 ? arr[2].GetSingle() : 0f;
                return new System.Numerics.Vector3(x, y, z);
            }
            return System.Numerics.Vector3.Zero;
        }

        private static Color DeserializeColor(JsonElement element)
        {
            float r = 1f, g = 1f, b = 1f, a = 1f;
            bool hasAny = false;

            if (element.ValueKind == JsonValueKind.Array)
            {
                var arr = element.EnumerateArray().ToArray();
                if (arr.Length >= 4)
                {
                    r = arr[0].GetSingle();
                    g = arr[1].GetSingle();
                    b = arr[2].GetSingle();
                    a = arr[3].GetSingle();
                    hasAny = true;
                }
            }
            else if (element.ValueKind == JsonValueKind.Object)
            {
                bool found = TryGetFloat(element, "r", out r) || TryGetFloat(element, "R", out r);
                found |= TryGetFloat(element, "g", out g) || TryGetFloat(element, "G", out g);
                found |= TryGetFloat(element, "b", out b) || TryGetFloat(element, "B", out b);
                found |= TryGetFloat(element, "a", out a) || TryGetFloat(element, "A", out a);
                hasAny = found;
            }
            else if (element.ValueKind == JsonValueKind.Number)
            {
                try
                {
                    return Color.FromArgb(element.GetInt32());
                }
                catch
                {
                    return Color.Empty;
                }
            }

            if (!hasAny) return Color.Empty;

            r = NormalizeColorComponent(r);
            g = NormalizeColorComponent(g);
            b = NormalizeColorComponent(b);
            a = NormalizeColorComponent(a);

            int ri = (int)Math.Round(r * 255f);
            int gi = (int)Math.Round(g * 255f);
            int bi = (int)Math.Round(b * 255f);
            int ai = (int)Math.Round(a * 255f);

            return Color.FromArgb(ai, ri, gi, bi);
        }

        private static bool TryGetFloat(JsonElement obj, string key, out float value)
        {
            if (obj.TryGetProperty(key, out var prop) && prop.ValueKind == JsonValueKind.Number)
            {
                value = (float)prop.GetDouble();
                return true;
            }
            value = 0f;
            return false;
        }

        private static float NormalizeColorComponent(float value)
        {
            if (value > 1f) value /= 255f;
            if (value < 0f) return 0f;
            if (value > 1f) return 1f;
            return value;
        }
        
        private static object? DeserializeList(Type listType, JsonElement element)
        {
            if (element.ValueKind != JsonValueKind.Array)
                return null;
            
            var elementType = listType.GetGenericArguments()[0];
            var listInstance = Activator.CreateInstance(listType) as System.Collections.IList;
            if (listInstance == null) return null;
            
            foreach (var item in element.EnumerateArray())
            {
                var deserializedItem = DeserializeValue(elementType, item);
                if (deserializedItem != null)
                {
                    listInstance.Add(deserializedItem);
                }
            }
            
            return listInstance;
        }

        private static object? DeserializeStruct(Type structType, JsonElement element)
        {
            if (element.ValueKind == JsonValueKind.Null)
                return Activator.CreateInstance(structType);
            if (element.ValueKind != JsonValueKind.Object) return null;

            object boxed = Activator.CreateInstance(structType);
            if (boxed == null) return null;

            foreach (var field in structType.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic))
            {
                bool serializable = field.IsPublic || field.GetCustomAttribute<SerializeField>() != null;
                if (!serializable) continue;
                if (!element.TryGetProperty(field.Name, out var subValue)) continue;

                try
                {
                    var deserialized = DeserializeValue(field.FieldType, subValue);
                    if (deserialized != null)
                    {
                        field.SetValue(boxed, deserialized);
                    }
                }
                catch (Exception ex)
                {
                    Console.WriteLine($"[ClayScriptableObjectLoader] Struct field {structType.Name}.{field.Name} deserialize failed: {ex.Message}");
                }
            }

            return boxed;
        }
        
        private static Type? FindType(string fullName)
        {
            // First, try to resolve from the hot-reloadable scripts assembly (GameScripts.dll).
            // This is critical because after hot-reload, user types like 'Item' are in the new
            // ScriptDomain assembly which may not appear in AppDomain.CurrentDomain.GetAssemblies()
            // when using collectible AssemblyLoadContexts.
            var scriptType = ScriptDomain.ResolveType(fullName);
            if (scriptType != null) return scriptType;
            
            // Fall back to searching all loaded assemblies for engine/system types
            foreach (var asm in AppDomain.CurrentDomain.GetAssemblies())
            {
                var type = asm.GetType(fullName);
                if (type != null) return type;
            }
            return null;
        }
        
        /// <summary>
        /// Clear the cache (e.g., on hot reload).
        /// </summary>
        public static void ClearCache()
        {
            _cache.Clear();
        }
        
        /// <summary>
        /// Invalidate a specific GUID from the cache (e.g., when a clay object is saved).
        /// </summary>
        public static void InvalidateCache(string guid)
        {
            if (string.IsNullOrEmpty(guid)) return;
            _cache.Remove(guid);
        }
        
        /// <summary>
        /// Check if a type is assignable from another type by full name.
        /// Used for type validation in editor.
        /// </summary>
        public static bool IsTypeAssignable(string assetTypeName, string expectedTypeName)
        {
            if (assetTypeName == expectedTypeName) return true;
            
            Type? assetType = FindType(assetTypeName);
            Type? expectedType = FindType(expectedTypeName);
            
            if (assetType == null || expectedType == null) return false;
            
            return expectedType.IsAssignableFrom(assetType);
        }
    }
}


