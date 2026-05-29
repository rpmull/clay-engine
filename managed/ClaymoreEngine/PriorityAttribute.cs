using System;

namespace ClaymoreEngine
{
    /// <summary>
    /// Controls the order in which script OnCreate is invoked during scene/prefab initialization.
    /// Lower value runs earlier. Default is 0 when not specified.
    /// </summary>
    [AttributeUsage(AttributeTargets.Class, Inherited = true, AllowMultiple = false)]
    public sealed class PriorityAttribute : Attribute
    {
        public int Value { get; }

        public PriorityAttribute(int value)
        {
            Value = value;
        }
    }
}
