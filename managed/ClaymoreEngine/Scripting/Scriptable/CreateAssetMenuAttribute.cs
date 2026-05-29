using System;

namespace ClaymoreEngine.Scripting.Scriptable
{
    /// <summary>
    /// Specifies menu path and default filename for ClayScriptableObject assets in the Create menu.
    /// </summary>
    [AttributeUsage(AttributeTargets.Class, Inherited = false, AllowMultiple = false)]
    public sealed class CreateAssetMenuAttribute : Attribute
    {
        public string MenuPath { get; }
        public string FileName { get; }
        public int Order { get; }
        public CreateAssetMenuAttribute(string menuPath, string fileName = "NewAsset", int order = 0)
        {
            MenuPath = menuPath; FileName = fileName; Order = order;
        }
    }
    
    /// <summary>
    /// Defines the subpath in the project panel context menu where this ClayObject type appears.
    /// Example: [AssetPath("examples/example_objects")] will place the type under Create > ClayObject > examples > example_objects
    /// </summary>
    [AttributeUsage(AttributeTargets.Class, Inherited = false, AllowMultiple = false)]
    public sealed class AssetPathAttribute : Attribute
    {
        public string Path { get; }
        public string DefaultFileName { get; }
        public int Order { get; }
        
        public AssetPathAttribute(string path, string defaultFileName = "NewAsset", int order = 0)
        {
            Path = path;
            DefaultFileName = defaultFileName;
            Order = order;
        }
    }
}


