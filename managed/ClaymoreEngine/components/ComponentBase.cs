namespace ClaymoreEngine
{
    public abstract class ComponentBase
    {
        internal Entity entity;
        
        /// <summary>
        /// Returns true if this component is attached to a valid entity.
        /// Use this instead of == null checks for safer validation.
        /// </summary>
        public bool IsValid => entity.IsValid;
        
        /// <summary>
        /// Gets the entity this component is attached to.
        /// </summary>
        public Entity Entity => entity;
    }
}
