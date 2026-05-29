using System;
using System.Collections.Generic;
using System.Numerics;

namespace ClaymoreEngine
{
    public sealed class Curve
    {
        public const int DefaultResolution = 96;

        private readonly Func<float, Vector3> _evaluator;
        private readonly int _resolution;
        private bool _arcLengthReady;
        private float[] _sampleParameters = Array.Empty<float>();
        private float[] _cumulativeLengths = Array.Empty<float>();
        private float _length;

        public Curve(Func<float, Vector3> evaluator, bool closed = false, int resolution = DefaultResolution)
        {
            _evaluator = evaluator ?? throw new ArgumentNullException(nameof(evaluator));
            Closed = closed;
            _resolution = Math.Max(2, resolution);
        }

        public bool Closed { get; }

        public float Length
        {
            get
            {
                EnsureArcLengthTable();
                return _length;
            }
        }

        public Vector3 Start => Evaluate(0f);
        public Vector3 End => Evaluate(1f);

        public Vector3 Evaluate(float t)
            => _evaluator(NormalizeParameter(t, Closed));

        public Vector3 EvaluateByDistance(float normalizedDistance)
        {
            EnsureArcLengthTable();

            float u = Closed
                ? Wrap01(normalizedDistance)
                : Clamp01(normalizedDistance);

            if (_length <= float.Epsilon || _cumulativeLengths.Length == 0)
                return Evaluate(u);

            float targetLength = u * _length;
            if (targetLength <= 0f)
                return Evaluate(0f);

            if (targetLength >= _length)
                return Evaluate(1f);

            int upper = Array.BinarySearch(_cumulativeLengths, targetLength);
            if (upper >= 0)
                return Evaluate(_sampleParameters[upper]);

            upper = ~upper;
            int lower = Math.Max(0, upper - 1);

            float lowerLength = _cumulativeLengths[lower];
            float upperLength = _cumulativeLengths[upper];
            float span = upperLength - lowerLength;
            float segmentT = span > float.Epsilon
                ? (targetLength - lowerLength) / span
                : 0f;

            float parameter = TweenMath.Lerp(_sampleParameters[lower], _sampleParameters[upper], segmentT);
            return Evaluate(parameter);
        }

        public Vector3 TangentByDistance(float normalizedDistance, float delta = 0.001f)
        {
            delta = MathF.Max(0.00001f, delta);
            float before = Closed
                ? normalizedDistance - delta
                : MathF.Max(0f, normalizedDistance - delta);
            float after = Closed
                ? normalizedDistance + delta
                : MathF.Min(1f, normalizedDistance + delta);

            Vector3 tangent = EvaluateByDistance(after) - EvaluateByDistance(before);
            if (tangent.LengthSquared() <= float.Epsilon)
                return Vector3.Zero;

            return Vector3.Normalize(tangent);
        }

        public static Curve Line(Vector3 start, Vector3 end, int resolution = 2)
            => new(t => TweenMath.Lerp(start, end, t), false, resolution);

        public static Curve Bezier(Vector3 start, Vector3 control, Vector3 end, int resolution = DefaultResolution)
            => QuadraticBezier(start, control, end, resolution);

        public static Curve QuadraticBezier(Vector3 start, Vector3 control, Vector3 end, int resolution = DefaultResolution)
            => new(t =>
            {
                float u = 1f - t;
                return (u * u * start) + (2f * u * t * control) + (t * t * end);
            }, false, resolution);

        public static Curve CubicBezier(Vector3 start, Vector3 controlA, Vector3 controlB, Vector3 end, int resolution = DefaultResolution)
            => new(t =>
            {
                float u = 1f - t;
                float uu = u * u;
                float tt = t * t;
                return (uu * u * start)
                    + (3f * uu * t * controlA)
                    + (3f * u * tt * controlB)
                    + (tt * t * end);
            }, false, resolution);

        public static Curve CubicSpline(IReadOnlyList<Vector3> points, bool closed = false, int resolution = DefaultResolution)
        {
            Vector3[] controlPoints = CopyAndValidatePoints(points, closed);
            int segmentCount = closed ? controlPoints.Length : controlPoints.Length - 1;
            int curveResolution = Math.Max(2, resolution * segmentCount);

            return new Curve(t =>
            {
                if (closed)
                    t = Wrap01(t);
                else
                    t = Clamp01(t);

                if (!closed && t >= 1f)
                    return controlPoints[controlPoints.Length - 1];

                float scaled = t * segmentCount;
                int segment = Math.Min((int)MathF.Floor(scaled), segmentCount - 1);
                float localT = scaled - segment;

                Vector3 p0;
                Vector3 p1;
                Vector3 p2;
                Vector3 p3;

                if (closed)
                {
                    int count = controlPoints.Length;
                    p0 = controlPoints[(segment - 1 + count) % count];
                    p1 = controlPoints[segment % count];
                    p2 = controlPoints[(segment + 1) % count];
                    p3 = controlPoints[(segment + 2) % count];
                }
                else
                {
                    p1 = controlPoints[segment];
                    p2 = controlPoints[segment + 1];
                    p0 = segment > 0 ? controlPoints[segment - 1] : p1;
                    p3 = segment + 2 < controlPoints.Length ? controlPoints[segment + 2] : p2;
                }

                return CatmullRom(p0, p1, p2, p3, localT);
            }, closed, curveResolution);
        }

        public static Curve ThroughPoints(IReadOnlyList<Vector3> points, bool closed = false, int resolution = DefaultResolution)
            => CubicSpline(points, closed, resolution);

        public static Curve Circle(
            Vector3 center,
            float radius,
            Vector3? normal = null,
            Vector3? startDirection = null,
            int resolution = DefaultResolution)
        {
            radius = MathF.Max(0f, radius);
            Vector3 planeNormal = SafeNormalize(normal ?? Vector3.UnitY, Vector3.UnitY);
            Vector3 tangent = startDirection ?? Vector3.UnitX;
            tangent -= Vector3.Dot(tangent, planeNormal) * planeNormal;

            if (tangent.LengthSquared() <= 0.000001f)
                tangent = MathF.Abs(Vector3.Dot(planeNormal, Vector3.UnitX)) < 0.95f
                    ? Vector3.UnitX
                    : Vector3.UnitZ;

            tangent = Vector3.Normalize(tangent - Vector3.Dot(tangent, planeNormal) * planeNormal);
            Vector3 bitangent = Vector3.Normalize(Vector3.Cross(planeNormal, tangent));

            return new Curve(t =>
            {
                float angle = t * MathF.PI * 2f;
                return center + radius * ((MathF.Cos(angle) * tangent) + (MathF.Sin(angle) * bitangent));
            }, true, resolution);
        }

        private void EnsureArcLengthTable()
        {
            if (_arcLengthReady)
                return;

            int sampleCount = _resolution + 1;
            _sampleParameters = new float[sampleCount];
            _cumulativeLengths = new float[sampleCount];

            Vector3 previous = Evaluate(0f);
            _sampleParameters[0] = 0f;
            _cumulativeLengths[0] = 0f;
            _length = 0f;

            for (int i = 1; i < sampleCount; i++)
            {
                float t = i / (float)(sampleCount - 1);
                Vector3 current = Evaluate(t);
                _length += Vector3.Distance(previous, current);
                _sampleParameters[i] = t;
                _cumulativeLengths[i] = _length;
                previous = current;
            }

            _arcLengthReady = true;
        }

        private static Vector3[] CopyAndValidatePoints(IReadOnlyList<Vector3> points, bool closed)
        {
            if (points == null)
                throw new ArgumentNullException(nameof(points));

            int minimum = closed ? 3 : 2;
            if (points.Count < minimum)
                throw new ArgumentException($"A {(closed ? "closed" : "open")} spline requires at least {minimum} points.", nameof(points));

            Vector3[] copy = new Vector3[points.Count];
            for (int i = 0; i < points.Count; i++)
                copy[i] = points[i];

            return copy;
        }

        private static Vector3 CatmullRom(Vector3 p0, Vector3 p1, Vector3 p2, Vector3 p3, float t)
        {
            float tt = t * t;
            float ttt = tt * t;
            return 0.5f * ((2f * p1)
                + ((-p0 + p2) * t)
                + (((2f * p0) - (5f * p1) + (4f * p2) - p3) * tt)
                + ((-p0 + (3f * p1) - (3f * p2) + p3) * ttt));
        }

        private static float NormalizeParameter(float t, bool closed)
            => closed ? Wrap01(t) : Clamp01(t);

        private static float Clamp01(float value)
        {
            if (float.IsNaN(value))
                return 0f;

            return Math.Clamp(value, 0f, 1f);
        }

        private static float Wrap01(float value)
        {
            if (float.IsNaN(value) || float.IsInfinity(value))
                return 0f;

            return value - MathF.Floor(value);
        }

        private static Vector3 SafeNormalize(Vector3 value, Vector3 fallback)
        {
            if (value.LengthSquared() <= 0.000001f)
                return fallback;

            return Vector3.Normalize(value);
        }
    }
}
