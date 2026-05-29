using System;
using System.Collections.Generic;
using System.Numerics;

namespace ClaymoreEngine
{
    internal enum TweenState
    {
        Playing,
        Paused,
        Completed,
        Killed
    }

    public abstract class TweenHandle
    {
        private TweenState _state = TweenState.Playing;
        private float _delayRemaining;
        private bool _hasStarted;
        private bool _completionNotified;
        private bool _killNotified;

        protected TweenHandle(float delaySeconds = 0f)
        {
            _delayRemaining = MathF.Max(0f, delaySeconds);
        }

        internal object? ReplaceKey { get; private set; }
        internal int TargetEntityId { get; private set; }
        internal Func<bool>? TargetAlivePredicate { get; private set; }

        public event Action? OnStart;
        public event Action<float>? OnStep;
        public event Action? OnComplete;
        public event Action? OnKill;

        public bool IsPlaying => _state == TweenState.Playing;
        public bool IsPaused => _state == TweenState.Paused;
        public bool IsComplete => _state == TweenState.Completed;
        public bool IsKilled => _state == TweenState.Killed;

        protected bool HasDelayRemaining => _delayRemaining > 0f;
        protected Tween.Easing Ease { get; private set; } = Tween.Easing.Linear;

        public TweenHandle SetEase(Tween.Easing easing)
        {
            Ease = easing;
            return this;
        }

        public TweenHandle SetDelay(float seconds)
        {
            _delayRemaining = MathF.Max(0f, seconds);
            return this;
        }

        public TweenHandle Pause()
        {
            if (_state == TweenState.Playing)
                _state = TweenState.Paused;

            return this;
        }

        public TweenHandle Play()
        {
            if (_state == TweenState.Paused)
                _state = TweenState.Playing;

            return this;
        }

        public void Complete()
        {
            if (_state == TweenState.Completed || _state == TweenState.Killed)
                return;

            ApplyFinalValue();
            MarkComplete();
        }

        public void Kill(bool complete = false)
        {
            if (_state == TweenState.Completed || _state == TweenState.Killed)
                return;

            if (complete)
            {
                Complete();
                return;
            }

            _state = TweenState.Killed;
            OnKilled();
            NotifyKilled();
        }

        internal TweenHandle SetReplaceKey(object? key)
        {
            ReplaceKey = key;
            TweenRuntime.ReconcileReplaceKey(this);
            return this;
        }

        internal TweenHandle SetTargetEntity(int entityId)
        {
            TargetEntityId = entityId;
            return this;
        }

        internal TweenHandle SetTargetAlivePredicate(Func<bool>? predicate)
        {
            TargetAlivePredicate = predicate;
            return this;
        }

        internal virtual bool ConflictsWithReplaceKey(object? key)
            => ReplaceKey != null && Equals(ReplaceKey, key);

        internal virtual bool IsMeaningful => true;

        internal bool Update(float dt)
        {
            if (_state == TweenState.Killed || _state == TweenState.Completed)
                return false;

            if (_state == TweenState.Paused)
                return true;

            if (TargetAlivePredicate != null && !TargetAlivePredicate())
            {
                _state = TweenState.Killed;
                OnKilled();
                NotifyKilled();
                return false;
            }

            if (_delayRemaining > 0f)
            {
                _delayRemaining -= dt;
                if (_delayRemaining > 0f)
                    return true;

                dt = MathF.Max(0f, -_delayRemaining);
                _delayRemaining = 0f;
            }

            if (!_hasStarted)
            {
                _hasStarted = true;
                SafeInvoke(OnStart, "OnStart");
            }

            bool shouldKeepRunning = UpdateTween(dt);
            if (!shouldKeepRunning)
            {
                MarkComplete();
                return false;
            }

            return _state == TweenState.Playing;
        }

        protected float ApplyEasing(float normalizedTime)
            => Tween.EvaluateEasing(normalizedTime, Ease);

        protected void RaiseStep(float normalizedTime)
            => SafeInvoke(OnStep, normalizedTime, "OnStep");

        protected virtual void OnKilled()
        {
        }

        protected abstract bool UpdateTween(float dt);
        protected abstract void ApplyFinalValue();

        private void MarkComplete()
        {
            if (_state == TweenState.Completed)
                return;

            _state = TweenState.Completed;
            if (_completionNotified)
                return;

            _completionNotified = true;
            SafeInvoke(OnComplete, "OnComplete");
        }

        private void NotifyKilled()
        {
            if (_killNotified)
                return;

            _killNotified = true;
            SafeInvoke(OnKill, "OnKill");
        }

        private static void SafeInvoke(Action? callback, string callbackName)
        {
            if (callback == null)
                return;

            try
            {
                callback();
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[Tween] {callbackName} callback exception: {ex}");
            }
        }

        private static void SafeInvoke(Action<float>? callback, float value, string callbackName)
        {
            if (callback == null)
                return;

            try
            {
                callback(value);
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[Tween] {callbackName} callback exception: {ex}");
            }
        }
    }

    internal sealed class NoOpTweenHandle : TweenHandle
    {
        internal override bool IsMeaningful => HasDelayRemaining;

        protected override void ApplyFinalValue()
        {
        }

        protected override bool UpdateTween(float dt)
        {
            return false;
        }
    }

    internal sealed class PropertyTweenHandle<T> : TweenHandle
    {
        private readonly T _from;
        private readonly T _to;
        private readonly float _duration;
        private readonly Action<T> _setter;
        private readonly Func<T, T, float, T> _interpolator;
        private float _elapsed;

        internal PropertyTweenHandle(
            T from,
            T to,
            float duration,
            Action<T> setter,
            Func<T, T, float, T> interpolator)
        {
            _from = from;
            _to = to;
            _duration = MathF.Max(0f, duration);
            _setter = setter;
            _interpolator = interpolator;
        }

        protected override bool UpdateTween(float dt)
        {
            if (_duration <= 0f)
            {
                _setter(_to);
                RaiseStep(1f);
                return false;
            }

            _elapsed = MathF.Min(_elapsed + dt, _duration);
            float normalizedTime = Math.Clamp(_elapsed / _duration, 0f, 1f);
            float easedTime = ApplyEasing(normalizedTime);
            _setter(_interpolator(_from, _to, easedTime));
            RaiseStep(normalizedTime);
            return _elapsed < _duration;
        }

        protected override void ApplyFinalValue()
        {
            _setter(_to);
            RaiseStep(1f);
        }
    }

    internal sealed class CurveTweenHandle : TweenHandle
    {
        private readonly Curve _curve;
        private readonly float _duration;
        private readonly Action<Vector3> _setter;
        private readonly float _from;
        private readonly float _to;
        private float _elapsed;

        internal CurveTweenHandle(Curve curve, float duration, Action<Vector3> setter, float from = 0f, float to = 1f)
        {
            _curve = curve ?? throw new ArgumentNullException(nameof(curve));
            _duration = MathF.Max(0f, duration);
            _setter = setter ?? throw new ArgumentNullException(nameof(setter));
            _from = from;
            _to = to;
        }

        protected override bool UpdateTween(float dt)
        {
            if (_duration <= 0f)
            {
                ApplyAt(1f);
                return false;
            }

            _elapsed = MathF.Min(_elapsed + dt, _duration);
            float normalizedTime = Math.Clamp(_elapsed / _duration, 0f, 1f);
            ApplyAt(normalizedTime);
            return _elapsed < _duration;
        }

        protected override void ApplyFinalValue()
        {
            ApplyAt(1f);
        }

        private void ApplyAt(float normalizedTime)
        {
            float easedTime = ApplyEasing(normalizedTime);
            float curvePosition = TweenMath.Lerp(_from, _to, easedTime);
            _setter(_curve.EvaluateByDistance(curvePosition));
            RaiseStep(normalizedTime);
        }
    }

    internal sealed class TweenGroupHandle : TweenHandle
    {
        private readonly List<TweenHandle> _children;

        internal TweenGroupHandle(List<TweenHandle> children)
        {
            _children = children ?? new List<TweenHandle>();

            foreach (TweenHandle child in _children)
            {
                if (child.TargetEntityId > 0)
                {
                    SetTargetEntity(child.TargetEntityId);
                    if (child.TargetAlivePredicate != null)
                        SetTargetAlivePredicate(child.TargetAlivePredicate);
                    break;
                }
            }
        }

        internal bool HasChildren => _children.Count > 0;

        internal override bool ConflictsWithReplaceKey(object? key)
        {
            for (int i = 0; i < _children.Count; i++)
            {
                if (_children[i].ConflictsWithReplaceKey(key))
                    return true;
            }

            return false;
        }

        protected override bool UpdateTween(float dt)
        {
            if (_children.Count == 0)
            {
                RaiseStep(1f);
                return false;
            }

            for (int i = _children.Count - 1; i >= 0; i--)
            {
                TweenHandle child = _children[i];
                bool keepChild = false;

                try
                {
                    keepChild = child.Update(dt);
                }
                catch (Exception ex)
                {
                    Console.WriteLine($"[Tween] Child tween update exception: {ex}");
                    child.Kill();
                }

                if (!keepChild || child.IsComplete || child.IsKilled)
                    _children.RemoveAt(i);
            }

            if (_children.Count == 0)
            {
                RaiseStep(1f);
                return false;
            }

            return true;
        }

        protected override void ApplyFinalValue()
        {
            for (int i = _children.Count - 1; i >= 0; i--)
            {
                _children[i].Complete();
            }

            _children.Clear();
            RaiseStep(1f);
        }

        protected override void OnKilled()
        {
            for (int i = _children.Count - 1; i >= 0; i--)
            {
                _children[i].Kill();
            }

            _children.Clear();
        }
    }

    internal sealed class TweenLoopHandle : TweenHandle
    {
        private readonly Action[] _steps;
        private TweenGroupHandle? _currentStep;
        private int _nextStepIndex;

        internal TweenLoopHandle(Action[] steps)
        {
            _steps = steps ?? Array.Empty<Action>();
        }

        internal bool Initialize()
            => TryStartNextStep();

        internal override bool ConflictsWithReplaceKey(object? key)
            => _currentStep != null && _currentStep.ConflictsWithReplaceKey(key);

        protected override bool UpdateTween(float dt)
        {
            if (_steps.Length == 0)
            {
                RaiseStep(1f);
                return false;
            }

            float stepDt = dt;
            int transitions = 0;

            while (true)
            {
                if (_currentStep == null && !TryStartNextStep())
                {
                    RaiseStep(1f);
                    return false;
                }

                bool keepStep = _currentStep!.Update(stepDt);
                if (keepStep && !_currentStep.IsComplete && !_currentStep.IsKilled)
                    return true;

                _currentStep = null;
                stepDt = 0f;
                transitions++;

                if (transitions > Math.Max(1, _steps.Length))
                {
                    Console.WriteLine("[Tween] Loop contained no lasting tweens; stopping to avoid a tight update loop.");
                    RaiseStep(1f);
                    return false;
                }
            }
        }

        protected override void ApplyFinalValue()
        {
            _currentStep?.Complete();
            _currentStep = null;
            RaiseStep(1f);
        }

        protected override void OnKilled()
        {
            _currentStep?.Kill();
            _currentStep = null;
        }

        private bool TryStartNextStep()
        {
            if (_steps.Length == 0)
                return false;

            for (int attempts = 0; attempts < _steps.Length; attempts++)
            {
                Action? step = _steps[_nextStepIndex];
                _nextStepIndex = (_nextStepIndex + 1) % _steps.Length;

                TweenGroupHandle group = TweenRuntime.CaptureGroup(step);
                if (!group.HasChildren)
                    continue;

                if (group.TargetEntityId > 0)
                    SetTargetEntity(group.TargetEntityId);
                if (group.TargetAlivePredicate != null)
                    SetTargetAlivePredicate(group.TargetAlivePredicate);

                _currentStep = group;
                return true;
            }

            return false;
        }
    }

    internal sealed class TweenCaptureContext
    {
        private readonly List<TweenHandle> _captured = new();

        internal TweenHandle Capture(TweenHandle tween)
        {
            if (tween == null)
                return new NoOpTweenHandle();

            if (!tween.IsMeaningful)
                return tween;

            if (tween.ReplaceKey != null)
            {
                for (int i = _captured.Count - 1; i >= 0; i--)
                {
                    TweenHandle existing = _captured[i];
                    if (existing.IsComplete || existing.IsKilled)
                        continue;

                    if (existing.ConflictsWithReplaceKey(tween.ReplaceKey))
                    {
                        existing.Kill();
                        _captured.RemoveAt(i);
                    }
                }
            }

            _captured.Add(tween);
            TweenRuntime.ReconcileReplaceKey(tween);
            return tween;
        }

        internal List<TweenHandle> Release()
            => _captured;
    }

    internal static class TweenRuntime
    {
        private static readonly List<TweenHandle> s_ActiveTweens = new();
        [ThreadStatic] private static Stack<TweenCaptureContext>? s_CaptureStack;

        public static int ActiveCount => s_ActiveTweens.Count;

        public static TweenHandle Add(TweenHandle tween)
        {
            if (tween == null)
                return new NoOpTweenHandle();

            if (s_CaptureStack != null && s_CaptureStack.Count > 0)
                return s_CaptureStack.Peek().Capture(tween);

            s_ActiveTweens.Add(tween);
            ReconcileReplaceKey(tween);
            return tween;
        }

        public static TweenHandle CreateLoop(Action[] steps)
        {
            if (steps == null || steps.Length == 0)
                return new NoOpTweenHandle();

            TweenLoopHandle loop = new TweenLoopHandle(steps);
            if (!loop.Initialize())
                return new NoOpTweenHandle();

            return Add(loop);
        }

        public static TweenGroupHandle CaptureGroup(Action? buildAction)
        {
            TweenCaptureContext context = new TweenCaptureContext();
            s_CaptureStack ??= new Stack<TweenCaptureContext>();
            s_CaptureStack.Push(context);

            try
            {
                buildAction?.Invoke();
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[Tween] Failed to build tween group: {ex}");
            }
            finally
            {
                s_CaptureStack.Pop();
            }

            return new TweenGroupHandle(context.Release());
        }

        public static void ReconcileReplaceKey(TweenHandle tween)
        {
            if (tween.ReplaceKey == null)
                return;

            for (int i = s_ActiveTweens.Count - 1; i >= 0; i--)
            {
                TweenHandle existing = s_ActiveTweens[i];
                if (ReferenceEquals(existing, tween))
                    continue;

                if (existing.IsComplete || existing.IsKilled)
                    continue;

                if (existing.ConflictsWithReplaceKey(tween.ReplaceKey))
                {
                    existing.Kill();
                    s_ActiveTweens.RemoveAt(i);
                }
            }
        }

        public static void Update(float dt)
        {
            dt = MathF.Max(0f, dt);

            for (int i = s_ActiveTweens.Count - 1; i >= 0; i--)
            {
                TweenHandle tween = s_ActiveTweens[i];
                bool keepTween = false;

                try
                {
                    keepTween = tween.Update(dt);
                }
                catch (Exception ex)
                {
                    Console.WriteLine($"[Tween] Tween update exception: {ex}");
                    tween.Kill();
                }

                if (!keepTween || tween.IsComplete || tween.IsKilled)
                {
                    if (i < s_ActiveTweens.Count && ReferenceEquals(s_ActiveTweens[i], tween))
                    {
                        s_ActiveTweens.RemoveAt(i);
                    }
                    else
                    {
                        s_ActiveTweens.Remove(tween);
                    }
                }
            }
        }

        public static void CancelAll(int entityId)
        {
            if (entityId <= 0)
                return;

            for (int i = s_ActiveTweens.Count - 1; i >= 0; i--)
            {
                if (s_ActiveTweens[i].TargetEntityId == entityId)
                {
                    s_ActiveTweens[i].Kill();
                    s_ActiveTweens.RemoveAt(i);
                }
            }
        }

        public static void CancelAll()
        {
            foreach (TweenHandle tween in s_ActiveTweens)
            {
                tween.Kill();
            }

            s_ActiveTweens.Clear();
        }

        public static void Reset()
        {
            s_ActiveTweens.Clear();
        }
    }

    internal static class TweenGuards
    {
        public static bool IsEntityAlive(int entityId)
        {
            if (entityId <= 0)
                return false;

            if (EntityInterop.IsSceneBeingDestroyedFunc != null && EntityInterop.IsSceneBeingDestroyedFunc())
                return false;

            // Treat any positive lookup result as alive. Requiring an exact ID echo here
            // is brittle and can turn valid tweens into silent no-ops if the interop layer
            // is in a transient or remapped state.
            if (EntityInterop.GetEntityByID != null)
            {
                try
                {
                    if (EntityInterop.GetEntityByID(entityId) <= 0)
                        return false;
                }
                catch
                {
                    // Fall back to the caller-provided entity id. Native setters/getters
                    // already guard missing entities, so a false positive here is safer
                    // than silently rejecting a valid tween.
                }
            }

            return true;
        }

        public static bool IsComponentAlive(ComponentBase? component)
            => component != null && IsEntityAlive(component.Entity.EntityID);
    }

    internal static class TweenMath
    {
        public static float Lerp(float from, float to, float t) => from + (to - from) * t;
        public static Vector2 Lerp(Vector2 from, Vector2 to, float t) => Vector2.Lerp(from, to, t);
        public static Vector3 Lerp(Vector3 from, Vector3 to, float t) => Vector3.Lerp(from, to, t);
        public static Vector4 Lerp(Vector4 from, Vector4 to, float t) => Vector4.Lerp(from, to, t);

        public static Quaternion Lerp(Quaternion from, Quaternion to, float t)
            => Quaternion.Normalize(Quaternion.Slerp(from, to, t));

        public static Quaternion EulerDegreesToQuaternion(Vector3 eulerDegrees)
        {
            float yaw = eulerDegrees.Y * (MathF.PI / 180f);
            float pitch = eulerDegrees.X * (MathF.PI / 180f);
            float roll = eulerDegrees.Z * (MathF.PI / 180f);
            return Quaternion.CreateFromYawPitchRoll(yaw, pitch, roll);
        }
    }

    internal static class ManagedFrameServices
    {
        public static void Update(float dt)
        {
            Tween.UpdateRuntime(dt);
        }
    }

    public static class TransformTweenExtensions
    {
        public static TweenHandle TweenPosition(this Transform transform, Vector3 targetPosition, float duration, Tween.Easing easing = Tween.Easing.Linear)
        {
            if (transform == null || !TweenGuards.IsEntityAlive(transform.EntityID))
                return new NoOpTweenHandle();

            int entityId = transform.EntityID;
            return Tween.To(() => transform.position, value => transform.position = value, targetPosition, duration)
                .SetEase(easing)
                .SetTargetEntity(entityId)
                .SetTargetAlivePredicate(() => TweenGuards.IsEntityAlive(entityId))
                .SetReplaceKey((entityId, "Transform.Position"));
        }

        public static TweenHandle TweenPositionAlong(this Transform transform, Curve curve, float duration, Tween.Easing easing = Tween.Easing.Linear)
        {
            if (transform == null || curve == null || !TweenGuards.IsEntityAlive(transform.EntityID))
                return new NoOpTweenHandle();

            int entityId = transform.EntityID;
            return Tween.AlongCurve(value => transform.position = value, curve, duration, easing)
                .SetTargetEntity(entityId)
                .SetTargetAlivePredicate(() => TweenGuards.IsEntityAlive(entityId))
                .SetReplaceKey((entityId, "Transform.Position"));
        }

        public static TweenHandle TweenPositionAlong(this Transform transform, Curve curve, float duration, float from, float to, Tween.Easing easing = Tween.Easing.Linear)
        {
            if (transform == null || curve == null || !TweenGuards.IsEntityAlive(transform.EntityID))
                return new NoOpTweenHandle();

            int entityId = transform.EntityID;
            return Tween.AlongCurve(value => transform.position = value, curve, duration, from, to, easing)
                .SetTargetEntity(entityId)
                .SetTargetAlivePredicate(() => TweenGuards.IsEntityAlive(entityId))
                .SetReplaceKey((entityId, "Transform.Position"));
        }

        public static TweenHandle TweenLocalPosition(this Transform transform, Vector3 targetPosition, float duration, Tween.Easing easing = Tween.Easing.Linear)
        {
            if (transform == null || !TweenGuards.IsEntityAlive(transform.EntityID))
                return new NoOpTweenHandle();

            int entityId = transform.EntityID;
            return Tween.To(() => transform.localPosition, value => transform.localPosition = value, targetPosition, duration)
                .SetEase(easing)
                .SetTargetEntity(entityId)
                .SetTargetAlivePredicate(() => TweenGuards.IsEntityAlive(entityId))
                .SetReplaceKey((entityId, "Transform.LocalPosition"));
        }

        public static TweenHandle TweenLocalPositionAlong(this Transform transform, Curve curve, float duration, Tween.Easing easing = Tween.Easing.Linear)
        {
            if (transform == null || curve == null || !TweenGuards.IsEntityAlive(transform.EntityID))
                return new NoOpTweenHandle();

            int entityId = transform.EntityID;
            return Tween.AlongCurve(value => transform.localPosition = value, curve, duration, easing)
                .SetTargetEntity(entityId)
                .SetTargetAlivePredicate(() => TweenGuards.IsEntityAlive(entityId))
                .SetReplaceKey((entityId, "Transform.LocalPosition"));
        }

        public static TweenHandle TweenLocalPositionAlong(this Transform transform, Curve curve, float duration, float from, float to, Tween.Easing easing = Tween.Easing.Linear)
        {
            if (transform == null || curve == null || !TweenGuards.IsEntityAlive(transform.EntityID))
                return new NoOpTweenHandle();

            int entityId = transform.EntityID;
            return Tween.AlongCurve(value => transform.localPosition = value, curve, duration, from, to, easing)
                .SetTargetEntity(entityId)
                .SetTargetAlivePredicate(() => TweenGuards.IsEntityAlive(entityId))
                .SetReplaceKey((entityId, "Transform.LocalPosition"));
        }

        public static TweenHandle TweenScale(this Transform transform, Vector3 targetScale, float duration, Tween.Easing easing = Tween.Easing.Linear)
        {
            if (transform == null || !TweenGuards.IsEntityAlive(transform.EntityID))
                return new NoOpTweenHandle();

            int entityId = transform.EntityID;
            return Tween.To(() => transform.scale, value => transform.scale = value, targetScale, duration)
                .SetEase(easing)
                .SetTargetEntity(entityId)
                .SetTargetAlivePredicate(() => TweenGuards.IsEntityAlive(entityId))
                .SetReplaceKey((entityId, "Transform.Scale"));
        }

        public static TweenHandle TweenRotation(this Transform transform, Vector3 targetEulerAngles, float duration, Tween.Easing easing = Tween.Easing.Linear)
        {
            if (transform == null || !TweenGuards.IsEntityAlive(transform.EntityID))
                return new NoOpTweenHandle();

            int entityId = transform.EntityID;
            return Tween.To(
                    () => transform.eulerAngles,
                    value => transform.eulerAngles = value,
                    targetEulerAngles,
                    duration)
                .SetEase(easing)
                .SetTargetEntity(entityId)
                .SetTargetAlivePredicate(() => TweenGuards.IsEntityAlive(entityId))
                .SetReplaceKey((entityId, "Transform.Rotation"));
        }

        public static TweenHandle TweenRotation(this Transform transform, Quaternion targetRotation, float duration, Tween.Easing easing = Tween.Easing.Linear)
        {
            if (transform == null || !TweenGuards.IsEntityAlive(transform.EntityID))
                return new NoOpTweenHandle();

            int entityId = transform.EntityID;
            return Tween.To(
                    () => transform.rotation,
                    value => transform.rotation = value,
                    targetRotation,
                    duration,
                    TweenMath.Lerp)
                .SetEase(easing)
                .SetTargetEntity(entityId)
                .SetTargetAlivePredicate(() => TweenGuards.IsEntityAlive(entityId))
                .SetReplaceKey((entityId, "Transform.Rotation"));
        }
    }

    public static class EntityTweenExtensions
    {
        public static TweenHandle TweenPosition(this Entity entity, Vector3 targetPosition, float duration, Tween.Easing easing = Tween.Easing.Linear)
            => entity.transform.TweenPosition(targetPosition, duration, easing);

        public static TweenHandle TweenLocalPosition(this Entity entity, Vector3 targetPosition, float duration, Tween.Easing easing = Tween.Easing.Linear)
            => entity.transform.TweenLocalPosition(targetPosition, duration, easing);

        public static TweenHandle TweenScale(this Entity entity, Vector3 targetScale, float duration, Tween.Easing easing = Tween.Easing.Linear)
            => entity.transform.TweenScale(targetScale, duration, easing);

        public static TweenHandle TweenRotation(this Entity entity, Vector3 targetEulerAngles, float duration, Tween.Easing easing = Tween.Easing.Linear)
            => entity.transform.TweenRotation(targetEulerAngles, duration, easing);

        public static TweenHandle TweenRotation(this Entity entity, Quaternion targetRotation, float duration, Tween.Easing easing = Tween.Easing.Linear)
            => entity.transform.TweenRotation(targetRotation, duration, easing);
    }

    public static class RigidBodyTweenExtensions
    {
        public static TweenHandle TweenLinearVelocity(this RigidBodyComponent rigidBody, Vector3 targetVelocity, float duration, Tween.Easing easing = Tween.Easing.Linear)
        {
            if (!TweenGuards.IsComponentAlive(rigidBody))
                return new NoOpTweenHandle();

            int entityId = rigidBody.Entity.EntityID;
            return Tween.To(() => rigidBody.LinearVelocity, value => rigidBody.LinearVelocity = value, targetVelocity, duration)
                .SetEase(easing)
                .SetTargetEntity(entityId)
                .SetTargetAlivePredicate(() => TweenGuards.IsComponentAlive(rigidBody))
                .SetReplaceKey((entityId, "RigidBody.LinearVelocity"));
        }

        public static TweenHandle TweenAngularVelocity(this RigidBodyComponent rigidBody, Vector3 targetVelocity, float duration, Tween.Easing easing = Tween.Easing.Linear)
        {
            if (!TweenGuards.IsComponentAlive(rigidBody))
                return new NoOpTweenHandle();

            int entityId = rigidBody.Entity.EntityID;
            return Tween.To(() => rigidBody.AngularVelocity, value => rigidBody.AngularVelocity = value, targetVelocity, duration)
                .SetEase(easing)
                .SetTargetEntity(entityId)
                .SetTargetAlivePredicate(() => TweenGuards.IsComponentAlive(rigidBody))
                .SetReplaceKey((entityId, "RigidBody.AngularVelocity"));
        }
    }

    public static class LightTweenExtensions
    {
        public static TweenHandle TweenIntensity(this LightComponent light, float targetIntensity, float duration, Tween.Easing easing = Tween.Easing.Linear)
        {
            if (!TweenGuards.IsComponentAlive(light))
                return new NoOpTweenHandle();

            int entityId = light.Entity.EntityID;
            return Tween.To(() => light.Intensity, value => light.Intensity = value, targetIntensity, duration)
                .SetEase(easing)
                .SetTargetEntity(entityId)
                .SetTargetAlivePredicate(() => TweenGuards.IsComponentAlive(light))
                .SetReplaceKey((entityId, "Light.Intensity"));
        }

        public static TweenHandle TweenColor(this LightComponent light, Vector3 targetColor, float duration, Tween.Easing easing = Tween.Easing.Linear)
        {
            if (!TweenGuards.IsComponentAlive(light))
                return new NoOpTweenHandle();

            int entityId = light.Entity.EntityID;
            return Tween.To(() => light.Color, value => light.Color = value, targetColor, duration)
                .SetEase(easing)
                .SetTargetEntity(entityId)
                .SetTargetAlivePredicate(() => TweenGuards.IsComponentAlive(light))
                .SetReplaceKey((entityId, "Light.Color"));
        }
    }

    public static class TintControllerTweenExtensions
    {
        public static TweenHandle TweenBaseTint(this TintController tintController, Vector4 targetTint, float duration, Tween.Easing easing = Tween.Easing.Linear)
        {
            if (!TweenGuards.IsComponentAlive(tintController))
                return new NoOpTweenHandle();

            int entityId = tintController.Entity.EntityID;
            return Tween.To(() => tintController.BaseTint, value => tintController.BaseTint = value, targetTint, duration)
                .SetEase(easing)
                .SetTargetEntity(entityId)
                .SetTargetAlivePredicate(() => TweenGuards.IsComponentAlive(tintController))
                .SetReplaceKey((entityId, "TintController.BaseTint"));
        }

        public static TweenHandle TweenTintColor(this TintController tintController, int channel, Vector4 targetTint, float duration, Tween.Easing easing = Tween.Easing.Linear)
        {
            if (!TweenGuards.IsComponentAlive(tintController))
                return new NoOpTweenHandle();

            int entityId = tintController.Entity.EntityID;
            return Tween.To(() => tintController.GetTintColor(channel), value => tintController.SetTintColor(channel, value), targetTint, duration)
                .SetEase(easing)
                .SetTargetEntity(entityId)
                .SetTargetAlivePredicate(() => TweenGuards.IsComponentAlive(tintController))
                .SetReplaceKey((entityId, $"TintController.Channel.{channel}"));
        }

        public static TweenHandle TweenEmissionStrength(this TintController tintController, float targetStrength, float duration, Tween.Easing easing = Tween.Easing.Linear)
        {
            if (!TweenGuards.IsComponentAlive(tintController))
                return new NoOpTweenHandle();

            int entityId = tintController.Entity.EntityID;
            return Tween.To(() => tintController.EmissionStrength, value => tintController.EmissionStrength = value, targetStrength, duration)
                .SetEase(easing)
                .SetTargetEntity(entityId)
                .SetTargetAlivePredicate(() => TweenGuards.IsComponentAlive(tintController))
                .SetReplaceKey((entityId, "TintController.EmissionStrength"));
        }

        public static TweenHandle TweenEmissionColor(this TintController tintController, Vector3 targetColor, float duration, Tween.Easing easing = Tween.Easing.Linear)
        {
            if (!TweenGuards.IsComponentAlive(tintController))
                return new NoOpTweenHandle();

            int entityId = tintController.Entity.EntityID;
            return Tween.To(() => tintController.EmissionColor, value => tintController.EmissionColor = value, targetColor, duration)
                .SetEase(easing)
                .SetTargetEntity(entityId)
                .SetTargetAlivePredicate(() => TweenGuards.IsComponentAlive(tintController))
                .SetReplaceKey((entityId, "TintController.EmissionColor"));
        }
    }

    public static class MeshMaterialTweenExtensions
    {
        public static TweenHandle TweenColorTint(this MeshComponent mesh, Vector4 targetTint, float duration, Tween.Easing easing = Tween.Easing.Linear)
        {
            if (!TweenGuards.IsComponentAlive(mesh))
                return new NoOpTweenHandle();

            MaterialComponent material = CreateMaterialComponent(mesh);
            // Single-slot primitives/default meshes typically store tint on slot 0,
            // so prefer that override channel for the common case.
            if (material.SlotCount == 1)
                return material.TweenMaterialVector4(0, MaterialProperties.ColorTint, targetTint, duration, easing);

            return material.TweenMaterialVector4(MaterialProperties.ColorTint, targetTint, duration, easing);
        }

        public static TweenHandle TweenColorTint(this MeshComponent mesh, int slot, Vector4 targetTint, float duration, Tween.Easing easing = Tween.Easing.Linear)
        {
            if (!TweenGuards.IsComponentAlive(mesh))
                return new NoOpTweenHandle();

            MaterialComponent material = CreateMaterialComponent(mesh);
            return material.TweenMaterialVector4(slot, MaterialProperties.ColorTint, targetTint, duration, easing);
        }

        private static MaterialComponent CreateMaterialComponent(MeshComponent mesh)
            => new MaterialComponent { entity = mesh.Entity };
    }

    public static class MaterialTweenExtensions
    {
        public static TweenHandle TweenMaterialFloat(this MaterialComponent material, string propertyName, float targetValue, float duration, Tween.Easing easing = Tween.Easing.Linear)
        {
            if (!TweenGuards.IsComponentAlive(material))
                return new NoOpTweenHandle();

            int entityId = material.Entity.EntityID;
            return Tween.To(() => material.GetFloat(propertyName), value => material.SetFloat(propertyName, value), targetValue, duration)
                .SetEase(easing)
                .SetTargetEntity(entityId)
                .SetTargetAlivePredicate(() => TweenGuards.IsComponentAlive(material))
                .SetReplaceKey((entityId, $"Material.Float.{propertyName}"));
        }

        public static TweenHandle TweenMaterialFloat(this MaterialComponent material, int slot, string propertyName, float targetValue, float duration, Tween.Easing easing = Tween.Easing.Linear)
        {
            if (!TweenGuards.IsComponentAlive(material))
                return new NoOpTweenHandle();

            int entityId = material.Entity.EntityID;
            return Tween.To(() => material.GetVector4(slot, propertyName).X, value => material.SetFloat(slot, propertyName, value), targetValue, duration)
                .SetEase(easing)
                .SetTargetEntity(entityId)
                .SetTargetAlivePredicate(() => TweenGuards.IsComponentAlive(material))
                .SetReplaceKey((entityId, $"Material.Slot.{slot}.Float.{propertyName}"));
        }

        public static TweenHandle TweenMaterialVector4(this MaterialComponent material, string propertyName, Vector4 targetValue, float duration, Tween.Easing easing = Tween.Easing.Linear)
        {
            if (!TweenGuards.IsComponentAlive(material))
                return new NoOpTweenHandle();

            int entityId = material.Entity.EntityID;
            return Tween.To(() => material.GetVector4(propertyName), value => material.SetVector4(propertyName, value), targetValue, duration)
                .SetEase(easing)
                .SetTargetEntity(entityId)
                .SetTargetAlivePredicate(() => TweenGuards.IsComponentAlive(material))
                .SetReplaceKey((entityId, $"Material.Vector4.{propertyName}"));
        }

        public static TweenHandle TweenMaterialVector4(this MaterialComponent material, int slot, string propertyName, Vector4 targetValue, float duration, Tween.Easing easing = Tween.Easing.Linear)
        {
            if (!TweenGuards.IsComponentAlive(material))
                return new NoOpTweenHandle();

            int entityId = material.Entity.EntityID;
            return Tween.To(() => material.GetVector4(slot, propertyName), value => material.SetVector4(slot, propertyName, value), targetValue, duration)
                .SetEase(easing)
                .SetTargetEntity(entityId)
                .SetTargetAlivePredicate(() => TweenGuards.IsComponentAlive(material))
                .SetReplaceKey((entityId, $"Material.Slot.{slot}.Vector4.{propertyName}"));
        }
    }
}
