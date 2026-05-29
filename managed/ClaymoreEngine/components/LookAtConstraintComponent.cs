using System;

namespace ClaymoreEngine
{
    /// <summary>
    /// Component for controlling LookAt/Aim constraints on skeletal meshes.
    /// This rotates bones toward a target entity without moving them.
    /// Works in a layered pipeline: Animation → LookAt → IK → Skinning.
    /// </summary>
    /// <remarks>
    /// For spine look-at (torso rotation toward camera):
    /// - Attach to skeleton root entity
    /// - Set target to camera or aim pivot
    /// - Rotation is distributed across spine bones
    /// 
    /// For head tracking:
    /// - Use separate constraint on head/neck bones
    /// - Higher smoothing for natural movement
    /// 
    /// Mode selection:
    /// - LookAtPosition: For third-person cameras, NPCs - rotates to face target position
    /// - MatchRotation: For FPS cameras - copies target's yaw/pitch rotation directly
    /// </remarks>
    public sealed class LookAtConstraintComponent : ComponentBase
    {
        /// <summary>
        /// Gets or sets whether the constraint is enabled.
        /// When disabled, no rotation is applied.
        /// </summary>
        public bool Enabled
        {
            get => LookAtInterop.GetEnabled?.Invoke(entity.EntityID) ?? false;
            set => LookAtInterop.SetEnabled?.Invoke(entity.EntityID, value);
        }

        /// <summary>
        /// Gets or sets the constraint mode.
        /// - LookAtPosition: Rotate to face target's world position (3rd person, NPCs)
        /// - MatchRotation: Copy target's yaw/pitch rotation (FPS cameras)
        /// </summary>
        public LookAtMode Mode
        {
            get => (LookAtMode)(LookAtInterop.GetMode?.Invoke(entity.EntityID) ?? 0);
            set => LookAtInterop.SetMode?.Invoke(entity.EntityID, (int)value);
        }

        /// <summary>
        /// DEPRECATED: No longer needed. MatchRotation mode now automatically uses
        /// the camera convention (-Z forward). This property is kept for compatibility
        /// but has no effect.
        /// </summary>
        [Obsolete("MatchRotation mode now automatically handles camera forward convention. This property is no longer needed.")]
        public bool TargetUsesNegativeZForward
        {
            get => LookAtInterop.GetTargetUsesNegativeZForward?.Invoke(entity.EntityID) ?? false;
            set => LookAtInterop.SetTargetUsesNegativeZForward?.Invoke(entity.EntityID, value);
        }

        /// <summary>
        /// Gets or sets the blend weight [0-1].
        /// 0 = no effect, 1 = full rotation toward target.
        /// </summary>
        public float Weight
        {
            get => LookAtInterop.GetWeight?.Invoke(entity.EntityID) ?? 0f;
            set => LookAtInterop.SetWeight?.Invoke(entity.EntityID, value);
        }

        /// <summary>
        /// Sets the look target entity.
        /// The constraint will rotate bones to face this entity.
        /// </summary>
        /// <param name="target">Target entity (camera, aim pivot, another character, etc.)</param>
        public void SetTarget(Entity target)
        {
            LookAtInterop.SetTarget?.Invoke(entity.EntityID, target.EntityID);
        }

        /// <summary>
        /// Sets the look target by entity ID.
        /// </summary>
        /// <param name="targetEntityId">Entity ID of the target</param>
        public void SetTarget(int targetEntityId)
        {
            LookAtInterop.SetTarget?.Invoke(entity.EntityID, targetEntityId);
        }

        /// <summary>
        /// Sets the smoothing speed.
        /// Higher values = faster response, lower values = smoother/slower.
        /// 0 = instant (no smoothing).
        /// </summary>
        /// <param name="speed">Smoothing speed (recommended: 5-15)</param>
        public void SetSmoothingSpeed(float speed)
        {
            LookAtInterop.SetSmoothingSpeed?.Invoke(entity.EntityID, speed);
        }

        /// <summary>
        /// Sets the maximum rotation angles (in degrees).
        /// Prevents over-rotation that would look unnatural.
        /// </summary>
        /// <param name="maxYaw">Maximum left/right rotation (default: 70°)</param>
        /// <param name="maxPitch">Maximum up/down rotation (default: 45°)</param>
        /// <param name="maxRoll">Maximum tilt rotation (default: 15°)</param>
        public void SetMaxAngles(float maxYaw, float maxPitch, float maxRoll = 15f)
        {
            LookAtInterop.SetMaxAngles?.Invoke(entity.EntityID, maxYaw, maxPitch, maxRoll);
        }

        /// <summary>
        /// Convenience method to temporarily disable the constraint.
        /// Equivalent to setting Enabled = false.
        /// </summary>
        public void Disable() => Enabled = false;

        /// <summary>
        /// Convenience method to enable the constraint.
        /// Equivalent to setting Enabled = true.
        /// </summary>
        public void Enable() => Enabled = true;

        /// <summary>
        /// Set mode to LookAtPosition - rotate to face target's world position.
        /// Use for third-person cameras, NPCs, etc.
        /// </summary>
        public void UseLookAtPosition() => Mode = LookAtMode.LookAtPosition;

        /// <summary>
        /// Set mode to MatchRotation - copy target's yaw/pitch rotation.
        /// Use for FPS cameras where spine should rotate with camera facing.
        /// </summary>
        public void UseMatchRotation() => Mode = LookAtMode.MatchRotation;

        /// <summary>
        /// Convenience method to set up for camera targets.
        /// Sets MatchRotation mode. The camera forward convention (-Z) is handled automatically.
        /// </summary>
        public void UseMatchRotationForCamera()
        {
            Mode = LookAtMode.MatchRotation;
            // Convention handling is now baked into the native constraint system
        }

        /// <summary>
        /// Smoothly blend the weight over time.
        /// Use in Update() for smooth transitions.
        /// </summary>
        /// <param name="targetWeight">Target weight to blend toward</param>
        /// <param name="speed">Blend speed (0-1 per second at speed=1)</param>
        /// <param name="deltaTime">Time since last frame</param>
        public void BlendWeight(float targetWeight, float speed, float deltaTime)
        {
            float current = Weight;
            float diff = targetWeight - current;
            float step = speed * deltaTime;
            
            if (System.Math.Abs(diff) <= step)
                Weight = targetWeight;
            else
                Weight = current + System.Math.Sign(diff) * step;
        }
    }
}
