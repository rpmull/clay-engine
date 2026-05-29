using System.Numerics;

namespace ClaymoreEngine
{
    public class CharacterControllerComponent : ComponentBase
    {
        /// <summary>
        /// The desired horizontal velocity for this frame.
        /// Set each frame based on input; the physics system handles movement.
        /// </summary>
        public Vector3 DesiredVelocity
        {
            get
            {
                if (ComponentInterop.CC_GetDesiredVelocity == null) return Vector3.Zero;
                ComponentInterop.CC_GetDesiredVelocity(entity.EntityID, out float x, out float y, out float z);
                return new Vector3(x, y, z);
            }
            set
            {
                if (ComponentInterop.CC_SetDesiredVelocity == null) return;
                ComponentInterop.CC_SetDesiredVelocity(entity.EntityID, value.X, value.Y, value.Z);
            }
        }
        
        /// <summary>
        /// Direct control over vertical velocity. Useful for climbing, flying, or custom gravity.
        /// Bypasses normal gravity accumulation when set.
        /// </summary>
        public float VerticalVelocity
        {
            get
            {
                if (ComponentInterop.CC_GetVerticalVelocity == null) return 0f;
                return ComponentInterop.CC_GetVerticalVelocity(entity.EntityID);
            }
            set
            {
                if (ComponentInterop.CC_SetVerticalVelocity == null) return;
                ComponentInterop.CC_SetVerticalVelocity(entity.EntityID, value);
            }
        }

        public void Jump(float speed)
        {
            if (ComponentInterop.CC_Jump == null) return;
            ComponentInterop.CC_Jump(entity.EntityID, speed);
        }

        public bool IsGrounded
        {
            get
            {
                if (ComponentInterop.CC_IsGrounded == null) return false;
                return ComponentInterop.CC_IsGrounded(entity.EntityID);
            }
        }

        public void SetPosition(Vector3 position)
        {
            if (ComponentInterop.CC_SetPosition == null) return;
            ComponentInterop.CC_SetPosition(entity.EntityID, position.X, position.Y, position.Z);
        }
    }
}


