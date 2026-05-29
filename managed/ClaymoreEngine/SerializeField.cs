using System;
namespace ClaymoreEngine
{
    /// <summary>
    /// Marks a field to be serialized and visible in the inspector.
    /// Supports primitives, strings, Vector3, Entity, Prefab, Mesh, AnimationController, AnimationControllerOverride, enums, structs, and List&lt;T&gt;.
    /// </summary>
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class SerializeField : Attribute { }
    
    /// <summary>
    /// Marks a struct as serializable for inspector display.
    /// Fields within the struct will be shown as expandable sub-properties.
    /// </summary>
    [AttributeUsage(AttributeTargets.Struct)]
    public sealed class SerializableStruct : Attribute { }
}
