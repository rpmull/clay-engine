using System;
using System.Collections.Generic;
using ClaymoreEngine.Scripting.Scriptable;

namespace ClaymoreEngine
{
    /// <summary>
    /// A serializable reference to a ClayScriptableObject that provides
    /// resource selection methods and displays as a dropdown in the inspector.
    /// 
    /// <example>
    /// Usage:
    /// <code>
    /// [SerializeField]
    /// public ResourceRef&lt;Hair&gt; hair;
    /// 
    /// void Start()
    /// {
    ///     // Select by index
    ///     hair.SelectFromIndex(1);
    ///     
    ///     // Select by name
    ///     hair.SelectByName("Mohawk");
    ///     
    ///     // Access the value
    ///     Hair currentHair = hair.Value;
    ///     
    ///     // Or use implicit conversion
    ///     DoSomethingWithHair(hair);
    /// }
    /// </code>
    /// </example>
    /// </summary>
    /// <typeparam name="T">The ClayScriptableObject type to reference</typeparam>
    [Serializable]
    public class ResourceRef<T> where T : ClayScriptableObject
    {
        // The GUID of the referenced resource (serialized)
        private string _guid = "";
        
        // Cached value (loaded on demand)
        [NonSerialized]
        private T? _cachedValue;
        [NonSerialized]
        private bool _cacheValid;
        
        /// <summary>
        /// Default constructor.
        /// </summary>
        public ResourceRef()
        {
        }
        
        /// <summary>
        /// Construct with an initial value.
        /// </summary>
        public ResourceRef(T? value)
        {
            Value = value;
        }
        
        /// <summary>
        /// The currently selected resource, or null if none selected.
        /// </summary>
        public T? Value
        {
            get
            {
                if (!_cacheValid && !string.IsNullOrEmpty(_guid))
                {
                    _cachedValue = ClayScriptableObjectLoader.Load(_guid, typeof(T)) as T;
                    _cacheValid = true;
                }
                return _cachedValue;
            }
            set
            {
                _cachedValue = value;
                _guid = value?.Guid ?? "";
                _cacheValid = true;
            }
        }
        
        /// <summary>
        /// The GUID of the selected resource. Used for serialization.
        /// </summary>
        public string Guid
        {
            get => _guid;
            set
            {
                if (_guid != value)
                {
                    _guid = value ?? "";
                    _cacheValid = false;
                    _cachedValue = null;
                }
            }
        }
        
        /// <summary>
        /// Select a resource by its index in the available resources list.
        /// </summary>
        /// <param name="index">Zero-based index into the resources list</param>
        public void SelectFromIndex(int index)
        {
            var resources = Resources.GetAll<T>();
            if (index >= 0 && index < resources.Count)
            {
                Value = resources[index];
            }
            else
            {
                Value = null;
            }
        }
        
        /// <summary>
        /// Select a resource by name (filename without extension).
        /// </summary>
        /// <param name="name">The resource name to select</param>
        public void SelectByName(string name)
        {
            Value = Resources.Get<T>(name);
        }
        
        /// <summary>
        /// Get the names of all available resources of this type.
        /// </summary>
        /// <returns>List of resource names</returns>
        public List<string> GetAvailableNames()
        {
            return Resources.GetResourceNames<T>();
        }
        
        /// <summary>
        /// Get all available resources of this type.
        /// </summary>
        /// <returns>List of all resources</returns>
        public List<T> GetAll()
        {
            return Resources.GetAll<T>();
        }
        
        /// <summary>
        /// Get the count of available resources of this type.
        /// </summary>
        public int AvailableCount => Resources.GetAll<T>().Count;
        
        /// <summary>
        /// Check if a resource is currently selected.
        /// </summary>
        public bool HasValue => !string.IsNullOrEmpty(_guid);
        
        /// <summary>
        /// Clear the current selection.
        /// </summary>
        public void Clear()
        {
            Value = null;
        }
        
        /// <summary>
        /// Get the index of the currently selected resource in the available resources list.
        /// Returns -1 if nothing is selected or the resource is not found.
        /// </summary>
        public int GetCurrentIndex()
        {
            if (string.IsNullOrEmpty(_guid)) return -1;
            
            var resources = Resources.GetAll<T>();
            for (int i = 0; i < resources.Count; i++)
            {
                if (resources[i].Guid == _guid)
                    return i;
            }
            return -1;
        }
        
        /// <summary>
        /// Implicit conversion to the underlying type for convenience.
        /// </summary>
        public static implicit operator T?(ResourceRef<T>? r) => r?.Value;
        
        /// <summary>
        /// String representation showing the current selection.
        /// </summary>
        public override string ToString()
        {
            if (string.IsNullOrEmpty(_guid))
                return $"ResourceRef<{typeof(T).Name}>(None)";
            return $"ResourceRef<{typeof(T).Name}>({_guid})";
        }
    }
}

