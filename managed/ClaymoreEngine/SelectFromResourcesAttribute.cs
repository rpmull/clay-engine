using System;

namespace ClaymoreEngine
{
    /// <summary>
    /// Marks a ClayScriptableObject field to display as a dropdown selector populated with all resources of that type.
    /// The field type must derive from ClayScriptableObject.
    /// 
    /// <example>
    /// Usage:
    /// <code>
    /// [SelectFromResources]
    /// [SerializeField]
    /// public ItemClass itemClass;
    /// </code>
    /// 
    /// In the inspector, this will display as a dropdown menu containing all ItemClass resources
    /// found in the resources/ folder, allowing you to select one.
    /// </example>
    /// </summary>
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class SelectFromResourcesAttribute : Attribute
    {
        /// <summary>
        /// If true, includes a "None" option at the top of the dropdown.
        /// Default is true.
        /// </summary>
        public bool AllowNone { get; set; } = true;
        
        /// <summary>
        /// Optional filter pattern for resource names (e.g., "Sword*" to only include items starting with "Sword").
        /// Default is null (no filter).
        /// </summary>
        public string? NameFilter { get; set; }
        
        /// <summary>
        /// If true, sort the dropdown options by resource name alphabetically.
        /// Default is true.
        /// </summary>
        public bool SortByName { get; set; } = true;
    }
    
    /// <summary>
    /// Alias for SelectFromResourcesAttribute (singular form accepted for convenience).
    /// </summary>
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class SelectFromResourceAttribute : Attribute
    {
        public bool AllowNone { get; set; } = true;
        public string? NameFilter { get; set; }
        public bool SortByName { get; set; } = true;
    }
}

