using System.Collections.Generic;
using System.Numerics;

namespace ClaymoreEngine
{
    public class SplineComponent : ComponentBase
    {
        public int GetControlPointCount()
        {
            if (ComponentInterop.Spline_GetControlPointCount == null) return 0;
            return ComponentInterop.Spline_GetControlPointCount(entity.EntityID);
        }

        public bool TryGetControlPoint(int index, out Vector3 worldPos)
        {
            worldPos = Vector3.Zero;
            if (ComponentInterop.Spline_GetControlPoint == null) return false;
            if (!ComponentInterop.Spline_GetControlPoint(entity.EntityID, index, out float x, out float y, out float z))
                return false;
            worldPos = new Vector3(x, y, z);
            return true;
        }

        public int GetSampledPointCount()
        {
            if (ComponentInterop.Spline_GetSampledPointCount == null) return 0;
            return ComponentInterop.Spline_GetSampledPointCount(entity.EntityID);
        }

        public bool TryGetSampledPoint(int index, out Vector3 worldPos)
        {
            worldPos = Vector3.Zero;
            if (ComponentInterop.Spline_GetSampledPoint == null) return false;
            if (!ComponentInterop.Spline_GetSampledPoint(entity.EntityID, index, out float x, out float y, out float z))
                return false;
            worldPos = new Vector3(x, y, z);
            return true;
        }

        public IEnumerable<Vector3> GetSampledPoints()
        {
            int count = GetSampledPointCount();
            for (int i = 0; i < count; i++)
            {
                if (TryGetSampledPoint(i, out Vector3 p))
                    yield return p;
            }
        }

        public bool TryGetNearestPoint(Vector3 worldPos, out Vector3 nearest, out float distance)
        {
            nearest = Vector3.Zero;
            distance = 0.0f;
            if (ComponentInterop.Spline_GetNearestPoint == null) return false;
            if (!ComponentInterop.Spline_GetNearestPoint(entity.EntityID, worldPos.X, worldPos.Y, worldPos.Z,
                    out float x, out float y, out float z, out float d))
                return false;
            nearest = new Vector3(x, y, z);
            distance = d;
            return true;
        }

        /// <summary>
        /// Get a point on the spline at a normalized position along its length (arc-length parameterization).
        /// t=0 → start, t=1 → end, t=0.5 → midpoint by distance.
        /// </summary>
        public bool TryGetPointAtNormalized(float t, out Vector3 worldPos)
        {
            worldPos = Vector3.Zero;
            if (ComponentInterop.Spline_GetPointAtNormalized == null) return false;
            if (!ComponentInterop.Spline_GetPointAtNormalized(entity.EntityID, t, out float x, out float y, out float z))
                return false;
            worldPos = new Vector3(x, y, z);
            return true;
        }
    }
}
