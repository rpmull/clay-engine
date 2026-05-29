using System;

namespace ClaymoreEngine
{
    /// <summary>
    /// Marks a List field to be automatically populated with all resources of its element type.
    /// The field must be a List&lt;T&gt; where T derives from ClayScriptableObject.
    /// 
    /// <example>
    /// Usage:
    /// <code>
    /// [PopulateFromResources]
    /// [SerializeField]
    /// public List&lt;Item&gt; allItems;
    /// </code>
    /// 
    /// When the script is initialized, allItems will be populated with all Item resources
    /// found in the resources/ folder.
    /// </example>
    /// </summary>
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class PopulateFromResourcesAttribute : Attribute
    {
        /// <summary>
        /// If true, the list will be repopulated every time OnCreate is called (e.g., after hot reload).
        /// Default is true.
        /// </summary>
        public bool RefreshOnReload { get; set; } = true;
        
        /// <summary>
        /// Optional filter pattern for resource names (e.g., "Sword*" to only include items starting with "Sword").
        /// Default is null (no filter).
        /// </summary>
        public string? NameFilter { get; set; }
        
        /// <summary>
        /// If true, sort the populated list by resource name alphabetically.
        /// Default is false.
        /// </summary>
        public bool SortByName { get; set; } = false;
    }
    
    /// <summary>
    /// Alias for PopulateFromResourcesAttribute (singular form accepted for convenience).
    /// </summary>
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class PopulateFromResourceAttribute : Attribute
    {
        public bool RefreshOnReload { get; set; } = true;
        public string? NameFilter { get; set; }
        public bool SortByName { get; set; } = false;
    }
}

