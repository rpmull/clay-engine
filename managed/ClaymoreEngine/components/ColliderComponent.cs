using System.Numerics;

namespace ClaymoreEngine
{
    /// <summary>
    /// Collider component for physics simulation.
    /// When added, defaults to a sphere shape with radius 0.5.
    /// A RigidBodyComponent or StaticBodyComponent is needed to activate physics simulation.
    /// </summary>
    public class ColliderComponent : ComponentBase
    {
        public Vector3 Offset
        {
            get
            {
                if (ComponentInterop.Collider_GetOffset == null)
                {
                    return Vector3.Zero;
                }

                ComponentInterop.Collider_GetOffset(entity.EntityID, out float x, out float y, out float z);
                return new Vector3(x, y, z);
            }
        }
    }
}

