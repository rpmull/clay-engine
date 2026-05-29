using System;
using System.Numerics;
using System.Reflection;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public unsafe delegate void TweenInteropInitDelegate(IntPtr* functionPointers, int count);

    public static unsafe class Tween
    {
        // The first three values keep their legacy numeric layout so existing
        // native registrations remain compatible. New values are managed-only.
        public enum Easing
        {
            Linear = 0,
            EaseInOutQuad = 1,
            EaseOutCubic = 2,
            EaseInQuad = 3,
            EaseOutQuad = 4,
            EaseInCubic = 5,
            EaseInOutCubic = 6,
            EaseInBack = 7,
            EaseOutBack = 8,
            EaseOutElastic = 9,
            EaseOutBounce = 10,

            Quad = EaseOutQuad,
            Cubic = EaseOutCubic
        }

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void TweenPositionFn(int entityId, float x, float y, float z, float duration, int easing);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void TweenRotationEulerFn(int entityId, float x, float y, float z, float duration, int easing);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void TweenScaleFn(int entityId, float x, float y, float z, float duration, int easing);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void TweenLightIntensityFn(int entityId, float to, float duration, int easing);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void TweenManagedFloatFn(int entityId, string className, string field, float to, float duration, int easing);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void TweenManagedVec3Fn(int entityId, string className, string field, float x, float y, float z, float duration, int easing);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void TweenSetFinishedCallbackFn(IntPtr cb);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void TweenFinishedManagedFn(int entityId, string tag);

        private static TweenPositionFn? _nativePosition;
        private static TweenRotationEulerFn? _nativeRotationEuler;
        private static TweenScaleFn? _nativeScale;
        private static TweenLightIntensityFn? _nativeLightIntensity;
        private static TweenManagedFloatFn? _nativeManagedFloat;
        private static TweenManagedVec3Fn? _nativeManagedVec3;
        private static TweenSetFinishedCallbackFn? _setFinished;

        private static TweenFinishedManagedFn? s_onFinishedThunk;

        public static event Action<int, string>? OnFinished;

        public static void InitializeInteropExport(IntPtr* ptrs, int count)
        {
            if (ptrs == null || count < 7)
                return;

            int i = 0;
            _nativePosition = Marshal.GetDelegateForFunctionPointer<TweenPositionFn>(ptrs[i++]);
            _nativeRotationEuler = Marshal.GetDelegateForFunctionPointer<TweenRotationEulerFn>(ptrs[i++]);
            _nativeScale = Marshal.GetDelegateForFunctionPointer<TweenScaleFn>(ptrs[i++]);
            _nativeLightIntensity = Marshal.GetDelegateForFunctionPointer<TweenLightIntensityFn>(ptrs[i++]);
            _nativeManagedFloat = Marshal.GetDelegateForFunctionPointer<TweenManagedFloatFn>(ptrs[i++]);
            _nativeManagedVec3 = Marshal.GetDelegateForFunctionPointer<TweenManagedVec3Fn>(ptrs[i++]);
            _setFinished = Marshal.GetDelegateForFunctionPointer<TweenSetFinishedCallbackFn>(ptrs[i++]);

            s_onFinishedThunk ??= OnFinishedNative;
            _setFinished?.Invoke(Marshal.GetFunctionPointerForDelegate(s_onFinishedThunk));
        }

        private static void OnFinishedNative(int entityId, string tag)
            => OnFinished?.Invoke(entityId, tag ?? string.Empty);

        internal static void ResetRuntimeState()
        {
            OnFinished = null;
            TweenRuntime.Reset();
        }

        internal static void UpdateRuntime(float dt)
        {
            TweenRuntime.Update(dt);
        }

        public static int ActiveCount => TweenRuntime.ActiveCount;

        public static void CancelAll(int entityId) => TweenRuntime.CancelAll(entityId);
        public static void CancelAll() => TweenRuntime.CancelAll();

        public static TweenHandle Wait(float duration)
            => TweenRuntime.Add(new NoOpTweenHandle().SetDelay(duration));

        /// <summary>
        /// Loops a sequence of tween-building steps indefinitely.
        /// Each step may create one tween or multiple tweens in parallel.
        /// </summary>
        public static TweenHandle Loop(params Action[] steps)
            => TweenRuntime.CreateLoop(steps);

        public static TweenHandle To(Func<float> getter, Action<float> setter, float to, float duration)
            => To(getter, setter, to, duration, TweenMath.Lerp);

        public static TweenHandle To(Func<Vector2> getter, Action<Vector2> setter, Vector2 to, float duration)
            => To(getter, setter, to, duration, TweenMath.Lerp);

        public static TweenHandle To(Func<Vector3> getter, Action<Vector3> setter, Vector3 to, float duration)
            => To(getter, setter, to, duration, TweenMath.Lerp);

        public static TweenHandle To(Func<Vector4> getter, Action<Vector4> setter, Vector4 to, float duration)
            => To(getter, setter, to, duration, TweenMath.Lerp);

        public static TweenHandle To(Func<Quaternion> getter, Action<Quaternion> setter, Quaternion to, float duration)
            => To(getter, setter, to, duration, TweenMath.Lerp);

        public static TweenHandle To<T>(
            Func<T> getter,
            Action<T> setter,
            T to,
            float duration,
            Func<T, T, float, T> interpolator)
        {
            if (getter == null || setter == null || interpolator == null)
                return new NoOpTweenHandle();

            T from;
            try
            {
                from = getter();
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[Tween] Failed to read tween start value: {ex}");
                return new NoOpTweenHandle();
            }

            return TweenRuntime.Add(new PropertyTweenHandle<T>(from, to, duration, setter, interpolator));
        }

        public static TweenHandle FromTo<T>(
            Action<T> setter,
            T from,
            T to,
            float duration,
            Func<T, T, float, T> interpolator)
        {
            if (setter == null || interpolator == null)
                return new NoOpTweenHandle();

            return TweenRuntime.Add(new PropertyTweenHandle<T>(from, to, duration, setter, interpolator));
        }

        public static TweenHandle AlongCurve(Action<Vector3> setter, Curve curve, float duration, Easing easing = Easing.Linear)
            => AlongCurve(setter, curve, duration, 0f, 1f, easing);

        public static TweenHandle AlongCurve(Action<Vector3> setter, Curve curve, float duration, float from, float to, Easing easing = Easing.Linear)
        {
            if (setter == null || curve == null)
                return new NoOpTweenHandle();

            return TweenRuntime.Add(new CurveTweenHandle(curve, duration, setter, from, to)
                .SetEase(easing));
        }

        public static TweenHandle Position(int entityId, Vector3 to, float duration, Easing easing = Easing.Linear)
        {
            if (!TweenGuards.IsEntityAlive(entityId))
                return new NoOpTweenHandle();

            Transform transform = new Transform(entityId);
            TweenHandle tween = To(() => transform.position, value => transform.position = value, to, duration)
                .SetEase(easing)
                .SetTargetEntity(entityId)
                .SetTargetAlivePredicate(() => TweenGuards.IsEntityAlive(entityId))
                .SetReplaceKey((entityId, "Transform.Position"));

            tween.OnComplete += () => OnFinished?.Invoke(entityId, "position");
            return tween;
        }

        public static TweenHandle RotationEuler(int entityId, Vector3 to, float duration, Easing easing = Easing.Linear)
        {
            if (!TweenGuards.IsEntityAlive(entityId))
                return new NoOpTweenHandle();

            Transform transform = new Transform(entityId);
            TweenHandle tween = To(() => transform.eulerAngles, value => transform.eulerAngles = value, to, duration)
                .SetEase(easing)
                .SetTargetEntity(entityId)
                .SetTargetAlivePredicate(() => TweenGuards.IsEntityAlive(entityId))
                .SetReplaceKey((entityId, "Transform.Rotation"));

            tween.OnComplete += () => OnFinished?.Invoke(entityId, "rotation");
            return tween;
        }

        public static TweenHandle Scale(int entityId, Vector3 to, float duration, Easing easing = Easing.Linear)
        {
            if (!TweenGuards.IsEntityAlive(entityId))
                return new NoOpTweenHandle();

            Transform transform = new Transform(entityId);
            TweenHandle tween = To(() => transform.scale, value => transform.scale = value, to, duration)
                .SetEase(easing)
                .SetTargetEntity(entityId)
                .SetTargetAlivePredicate(() => TweenGuards.IsEntityAlive(entityId))
                .SetReplaceKey((entityId, "Transform.Scale"));

            tween.OnComplete += () => OnFinished?.Invoke(entityId, "scale");
            return tween;
        }

        public static TweenHandle LightIntensity(int entityId, float to, float duration, Easing easing = Easing.Linear)
        {
            Entity entity = new Entity(entityId);
            LightComponent? light = entity.GetComponent<LightComponent>();
            if (light == null)
                return new NoOpTweenHandle();

            TweenHandle tween = To(() => light.Intensity, value => light.Intensity = value, to, duration)
                .SetEase(easing)
                .SetTargetEntity(entityId)
                .SetTargetAlivePredicate(() => TweenGuards.IsComponentAlive(light))
                .SetReplaceKey((entityId, "Light.Intensity"));

            tween.OnComplete += () => OnFinished?.Invoke(entityId, "light");
            return tween;
        }

        public static TweenHandle ManagedFloat(int entityId, string field, float to, float duration, Easing easing = Easing.Linear, string className = null)
        {
            return CreateManagedMemberTween(entityId, field, className, to, duration, easing, (from, target, t) => TweenMath.Lerp(from, target, t));
        }

        public static TweenHandle ManagedVec3(int entityId, string field, Vector3 to, float duration, Easing easing = Easing.Linear, string className = null)
        {
            return CreateManagedMemberTween(entityId, field, className, to, duration, easing, (from, target, t) => TweenMath.Lerp(from, target, t));
        }

        internal static float EvaluateEasing(float t, Easing easing)
        {
            t = Math.Clamp(t, 0f, 1f);
            return easing switch
            {
                Easing.EaseInOutQuad => t < 0.5f
                    ? 2f * t * t
                    : 1f - MathF.Pow(-2f * t + 2f, 2f) * 0.5f,
                Easing.EaseInQuad => t * t,
                Easing.EaseOutQuad => 1f - (1f - t) * (1f - t),
                Easing.EaseOutCubic => 1f - MathF.Pow(1f - t, 3f),
                Easing.EaseInCubic => t * t * t,
                Easing.EaseInOutCubic => t < 0.5f
                    ? 4f * t * t * t
                    : 1f - MathF.Pow(-2f * t + 2f, 3f) * 0.5f,
                Easing.EaseInBack =>
                    2.70158f * t * t * t - 1.70158f * t * t,
                Easing.EaseOutBack =>
                    1f + 2.70158f * MathF.Pow(t - 1f, 3f) + 1.70158f * MathF.Pow(t - 1f, 2f),
                Easing.EaseOutElastic when t == 0f => 0f,
                Easing.EaseOutElastic when t == 1f => 1f,
                Easing.EaseOutElastic =>
                    MathF.Pow(2f, -10f * t) * MathF.Sin((t * 10f - 0.75f) * (2f * MathF.PI / 3f)) + 1f,
                Easing.EaseOutBounce => EaseOutBounce(t),
                _ => t
            };
        }

        private static float EaseOutBounce(float t)
        {
            const float n1 = 7.5625f;
            const float d1 = 2.75f;

            if (t < 1f / d1)
                return n1 * t * t;

            if (t < 2f / d1)
            {
                t -= 1.5f / d1;
                return n1 * t * t + 0.75f;
            }

            if (t < 2.5f / d1)
            {
                t -= 2.25f / d1;
                return n1 * t * t + 0.9375f;
            }

            t -= 2.625f / d1;
            return n1 * t * t + 0.984375f;
        }

        private static TweenHandle CreateManagedMemberTween<T>(
            int entityId,
            string memberName,
            string? className,
            T targetValue,
            float duration,
            Easing easing,
            Func<T, T, float, T> interpolator)
        {
            if (string.IsNullOrWhiteSpace(memberName) ||
                !TryResolveManagedMember(entityId, className, memberName, out Func<T>? getter, out Action<T>? setter, out string resolvedClass))
                return new NoOpTweenHandle();

            string tagPrefix = typeof(T) == typeof(float)
                ? "mfloat"
                : typeof(T) == typeof(Vector3)
                    ? "mvec3"
                    : "mvalue";
            string tag = $"{tagPrefix}:{resolvedClass}.{memberName}";

            TweenHandle tween = To(getter, setter, targetValue, duration, interpolator)
                .SetEase(easing)
                .SetTargetEntity(entityId)
                .SetTargetAlivePredicate(() => TweenGuards.IsEntityAlive(entityId))
                .SetReplaceKey((entityId, $"{resolvedClass}.{memberName}"));

            tween.OnComplete += () => OnFinished?.Invoke(entityId, tag);
            return tween;
        }

        private static bool TryResolveManagedMember<T>(
            int entityId,
            string? className,
            string memberName,
            out Func<T>? getter,
            out Action<T>? setter,
            out string resolvedClass)
        {
            getter = null;
            setter = null;
            resolvedClass = string.IsNullOrWhiteSpace(className) ? "?" : className;

            foreach (ScriptComponent script in ScriptRegistry.Enumerate(entityId, className))
            {
                if (TryBindManagedMember(script, memberName, out getter, out setter))
                {
                    resolvedClass = script.GetType().FullName ?? script.GetType().Name;
                    return true;
                }
            }

            return false;
        }

        private static bool TryBindManagedMember<T>(
            ScriptComponent script,
            string memberName,
            out Func<T>? getter,
            out Action<T>? setter)
        {
            getter = null;
            setter = null;

            const BindingFlags flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
            Type scriptType = script.GetType();

            FieldInfo? field = scriptType.GetField(memberName, flags);
            if (field != null && field.FieldType == typeof(T))
            {
                getter = () => (T)field.GetValue(script)!;
                setter = value => field.SetValue(script, value);
                return true;
            }

            PropertyInfo? property = scriptType.GetProperty(memberName, flags);
            if (property != null &&
                property.PropertyType == typeof(T) &&
                property.CanRead &&
                property.CanWrite)
            {
                getter = () => (T)property.GetValue(script)!;
                setter = value => property.SetValue(script, value);
                return true;
            }

            return false;
        }
    }
}
