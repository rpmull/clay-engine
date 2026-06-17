using ClaymoreEngine.Physics;

namespace ClaymoreEngine
{
    /// <summary>
    /// Managed handle for an entity's static physics body. Static bodies don't move
    /// under simulation but still participate in collisions and queries, so the most
    /// common runtime operation is changing which physics layer they belong to.
    /// </summary>
    public class StaticBodyComponent : ComponentBase
    {
        /// <summary>
        /// Sets the physics layer this body belongs to (the layer it is filtered by in
        /// raycasts, spherecasts and contacts). Keeps the collider and body layer in
        /// sync. The layer must already be registered (see <see cref="PhysicsLayer.Register"/>).
        /// </summary>
        /// <param name="layerName">The registered physics layer name, e.g. "Ignore".</param>
        /// <returns>True if the layer was found and applied.</returns>
        public bool SetPhysicsLayer(string layerName)
        {
            // Shared native entry point: it resolves the layer once and applies it to
            // the collider plus whichever body (rigid or static) the entity owns, so
            // their inspector layer dropdowns stay matched.
            return ComponentInterop.SetRigidBodyPhysicsLayer(entity.EntityID, layerName);
        }
    }
}
