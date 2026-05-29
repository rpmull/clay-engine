using System;
using System.Reflection;
using System.Collections.Generic;

namespace ClaymoreEngine
{
    /// <summary>
    /// Base class for all module-defined components that are loaded from external DLLs.
    /// Module components store their data in native ModuleComponent storage and sync with managed instances.
    /// </summary>
    public abstract class ModuleComponentBase
    {
        internal Entity entity;
        
        /// <summary>
        /// The TypeId used to identify this component type in the native system.
        /// This is automatically generated from the component's full type name.
        /// </summary>
        public virtual string TypeName => GetType().FullName;
        
        /// <summary>
        /// Called when the component is first created or retrieved from native storage.
        /// Override this to initialize the component after data has been loaded.
        /// </summary>
        protected virtual void OnInitialize() { }
        
        /// <summary>
        /// Called to synchronize managed property values to native storage.
        /// This is automatically called when properties change if auto-sync is enabled.
        /// </summary>
        protected virtual void SyncToNative() 
        {
            ModuleComponentInterop.SyncComponentToNative(entity.EntityID, TypeName, this);
        }
        
        /// <summary>
        /// Called to synchronize native storage values to managed properties.
        /// This is automatically called when the component is first retrieved.
        /// </summary>
        protected virtual void SyncFromNative() 
        {
            ModuleComponentInterop.SyncComponentFromNative(entity.EntityID, TypeName, this);
        }
        
        /// <summary>
        /// Override this to provide custom field synchronization behavior.
        /// By default, all public properties with getters and setters are synchronized.
        /// </summary>
        /// <returns>Dictionary of field name to property info for synchronization</returns>
        public virtual Dictionary<string, PropertyInfo> GetSyncableProperties()
        {
            var properties = new Dictionary<string, PropertyInfo>();
            var type = GetType();
            
            foreach (var prop in type.GetProperties(BindingFlags.Public | BindingFlags.Instance))
            {
                // Skip properties that can't be read or written
                if (!prop.CanRead || !prop.CanWrite)
                    continue;
                    
                // Skip the entity and TypeName properties
                if (prop.Name == nameof(entity) || prop.Name == nameof(TypeName))
                    continue;
                    
                // Only sync supported types
                if (IsSupportedType(prop.PropertyType))
                {
                    properties[prop.Name] = prop;
                }
            }
            
            return properties;
        }
        
        private static bool IsSupportedType(Type type)
        {
            return type == typeof(bool) ||
                   type == typeof(int) ||
                   type == typeof(long) ||
                   type == typeof(float) ||
                   type == typeof(double) ||
                   type == typeof(string) ||
                   type == typeof(System.Numerics.Vector2) ||
                   type == typeof(System.Numerics.Vector3) ||
                   type == typeof(System.Numerics.Vector4) ||
                   type == typeof(System.Numerics.Quaternion);
        }
        
        /// <summary>
        /// Internal method called by the interop system to initialize this component instance.
        /// </summary>
        internal void InternalInitialize(Entity entity)
        {
            this.entity = entity;
            SyncFromNative();
            OnInitialize();
        }
    }
}
