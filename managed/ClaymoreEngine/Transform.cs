using System;
using System.Numerics;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    public class Transform
    {
        private readonly int _entityID;

        internal int EntityID => _entityID;

        internal Transform(int entityID)
        {
            _entityID = entityID;
        }

        /// <summary>
        /// Gets or sets the parent transform. Setting to null removes the parent.
        /// </summary>
        public Transform? parent
        {
            get
            {
                int parentId = EntityInterop.GetParent(_entityID);
                if (parentId <= 0) return null;
                return new Transform(parentId);
            }
            set
            {
                if (value == null)
                {
                    // Setting parent to null means no parent (parent ID = -1)
                    EntityInterop.SetParent(_entityID, -1, false);
                }
                else
                {
                    EntityInterop.SetParent(_entityID, value._entityID, false);
                }
            }
        }

        /// <summary>
        /// Sets the parent of this transform.
        /// </summary>
        /// <param name="newParent">The new parent transform</param>
        /// <param name="worldPositionStays">If true, maintains world position</param>
        public void SetParent(Transform? newParent, bool worldPositionStays = true)
        {
            int parentId = newParent?._entityID ?? -1;
            EntityInterop.SetParent(_entityID, parentId, worldPositionStays);
        }

        /// <summary>
        /// Gets or sets the world position of the entity.
        /// This is the actual world-space position after all parent transforms are applied.
        /// Setting this will automatically convert to local position relative to the parent.
        /// </summary>
        public Vector3 position
        {
            get => EntityInterop.GetPosition(_entityID);
            set => EntityInterop.SetPosition(_entityID, value);
        }

        /// <summary>
        /// Gets or sets the local position relative to the parent entity.
        /// For root entities (no parent), this is the same as world position.
        /// </summary>
        public Vector3 localPosition
        {
            get => EntityInterop.GetLocalPosition(_entityID);
            set => EntityInterop.SetLocalPosition(_entityID, value);
        }

        // Euler rotation in degrees
        public Vector3 eulerAngles
        {
            get
            {
                Vector3 raw = EntityInterop.GetRotation(_entityID);
                return new Vector3(NormalizeDegrees(raw.X), NormalizeDegrees(raw.Y), NormalizeDegrees(raw.Z));
            }
            set
            {
                // Thin wrapper: convert degrees to quaternion and delegate to quaternion setter
                float radY = value.Y * (MathF.PI / 180f);
                float radX = value.X * (MathF.PI / 180f);
                float radZ = value.Z * (MathF.PI / 180f);
                Quaternion q = Quaternion.CreateFromYawPitchRoll(radY, radX, radZ);
                EntityInterop.SetRotationQuat(_entityID, q);
            }
        }

        /// <summary>
        /// The forward direction of the transform in world space.
        /// This is the direction the transform is "looking at" (-Z in local space).
        /// </summary>
        public Vector3 forward
        {
            get => Vector3.Normalize(Vector3.Transform(-Vector3.UnitZ, rotation));
        }

        /// <summary>
        /// The right direction of the transform in world space (+X in local space).
        /// </summary>
        public Vector3 right
        {
            get => Vector3.Normalize(Vector3.Transform(Vector3.UnitX, rotation));
        }

        /// <summary>
        /// The up direction of the transform in world space (+Y in local space).
        /// </summary>
        public Vector3 up
        {
            get => Vector3.Normalize(Vector3.Transform(Vector3.UnitY, rotation));
        }

        // Quaternion rotation
        public Quaternion rotation
        {
            get => EntityInterop.GetRotationQuat(_entityID);
            set => EntityInterop.SetRotationQuat(_entityID, value);
        }

        public Vector3 scale
        {
            get => EntityInterop.GetScale(_entityID);
            set => EntityInterop.SetScale(_entityID, value);
        }

        private static float NormalizeDegrees(float degrees)
        {
            float normalized = degrees % 360f;
            if (normalized < 0f)
                normalized += 360f;
            return normalized;
        }

        /// <summary>
        /// Rotates the transform so the forward vector points at the target's position.
        /// </summary>
        /// <param name="target">The position to look at in world space.</param>
        public void LookAt(Vector3 target)
        {
            LookAt(target, Vector3.UnitY);
        }

        /// <summary>
        /// Rotates the transform so the forward vector points at the target's position.
        /// </summary>
        /// <param name="target">The position to look at in world space.</param>
        /// <param name="worldUp">The up direction to use for orientation.</param>
        public void LookAt(Vector3 target, Vector3 worldUp)
        {
            Vector3 direction = target - position;
            
            // Skip if target is at same position
            if (direction.LengthSquared() < 0.0001f)
                return;

            // Calculate yaw and pitch directly from direction to avoid gimbal lock
            // This ensures zero roll for orbital camera behavior
            // Add PI to yaw to flip 180° (camera looks along -Z in local space)
            float yaw = MathF.Atan2(direction.X, direction.Z) + MathF.PI;
            float horizontalDist = MathF.Sqrt(direction.X * direction.X + direction.Z * direction.Z);
            float pitch = MathF.Atan2(direction.Y, horizontalDist);

            // Build rotation from yaw (Y) then pitch (X) - no roll
            Quaternion yawQuat = Quaternion.CreateFromAxisAngle(Vector3.UnitY, yaw);
            Quaternion pitchQuat = Quaternion.CreateFromAxisAngle(Vector3.UnitX, pitch);
            
            rotation = yawQuat * pitchQuat;
        }

        /// <summary>
        /// Rotates the transform so the forward vector points at the target entity.
        /// </summary>
        /// <param name="target">The entity to look at.</param>
        public void LookAt(Entity target)
        {
            if (!target.IsValid) return;
            LookAt(target.transform.position, Vector3.UnitY);
        }

        /// <summary>
        /// Applies an incremental rotation to the transform using euler angles (in degrees).
        /// This is gimbal-lock safe because the rotation is converted to quaternion and
        /// multiplied with the current rotation.
        /// </summary>
        /// <param name="eulerDelta">The rotation increment in degrees (pitch, yaw, roll).</param>
        public void Rotate(Vector3 eulerDelta)
        {
            Rotate(eulerDelta, Space.Self);
        }

        /// <summary>
        /// Applies an incremental rotation to the transform using euler angles (in degrees).
        /// </summary>
        /// <param name="eulerDelta">The rotation increment in degrees (pitch, yaw, roll).</param>
        /// <param name="relativeTo">The coordinate space to rotate in.</param>
        public void Rotate(Vector3 eulerDelta, Space relativeTo)
        {
            // Convert delta euler to quaternion
            float radY = eulerDelta.Y * (MathF.PI / 180f);
            float radX = eulerDelta.X * (MathF.PI / 180f);
            float radZ = eulerDelta.Z * (MathF.PI / 180f);
            Quaternion deltaQuat = Quaternion.CreateFromYawPitchRoll(radY, radX, radZ);

            if (relativeTo == Space.Self)
            {
                // Local space: current rotation * delta
                rotation = rotation * deltaQuat;
            }
            else
            {
                // World space: delta * current rotation
                rotation = deltaQuat * rotation;
            }
        }

        /// <summary>
        /// Rotates the transform around an axis passing through a world-space point.
        /// </summary>
        /// <param name="point">The world-space point to rotate around.</param>
        /// <param name="axis">The axis to rotate around (should be normalized).</param>
        /// <param name="angle">The angle in degrees.</param>
        public void RotateAround(Vector3 point, Vector3 axis, float angle)
        {
            // Create rotation quaternion
            float radians = angle * (MathF.PI / 180f);
            Quaternion rotQuat = Quaternion.CreateFromAxisAngle(axis, radians);

            // Get offset from pivot point
            Vector3 offset = position - point;

            // Rotate the offset
            offset = Vector3.Transform(offset, rotQuat);

            // Apply new position
            position = point + offset;

            // Also rotate the transform's orientation
            rotation = rotQuat * rotation;
        }

        /// <summary>
        /// Rotates the transform by an angle around an axis.
        /// </summary>
        /// <param name="axis">The axis to rotate around (should be normalized).</param>
        /// <param name="angle">The angle in degrees.</param>
        public void Rotate(Vector3 axis, float angle)
        {
            Rotate(axis, angle, Space.Self);
        }

        /// <summary>
        /// Rotates the transform by an angle around an axis.
        /// </summary>
        /// <param name="axis">The axis to rotate around (should be normalized).</param>
        /// <param name="angle">The angle in degrees.</param>
        /// <param name="relativeTo">The coordinate space to rotate in.</param>
        public void Rotate(Vector3 axis, float angle, Space relativeTo)
        {
            float radians = angle * (MathF.PI / 180f);
            Quaternion deltaQuat = Quaternion.CreateFromAxisAngle(axis, radians);

            if (relativeTo == Space.Self)
            {
                rotation = rotation * deltaQuat;
            }
            else
            {
                rotation = deltaQuat * rotation;
            }
        }
    }

    /// <summary>
    /// Specifies the coordinate space for transform operations.
    /// </summary>
    public enum Space
    {
        /// <summary>Local coordinate space (relative to the transform itself).</summary>
        Self,
        /// <summary>World coordinate space.</summary>
        World
    }
}
