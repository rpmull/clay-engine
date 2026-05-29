using System;

namespace ClaymoreEngine
{
    [AttributeUsage(AttributeTargets.Class, Inherited = true, AllowMultiple = false)]
    public sealed class PreAnimationUpdateAttribute : Attribute
    {
    }
}
