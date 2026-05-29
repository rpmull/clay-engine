using System;
using System.Numerics;

namespace ClaymoreEngine
{
    public interface IUIOpacity
    {
        float opacity { get; set; }
        bool visible { get; set; }
        Entity Entity { get; }
    }

    public interface IUIPositionable
    {
        Vector2 anchorOffset { get; set; }
        Entity Entity { get; }
    }

    public static class UITween
    {
        // Legacy no-op kept for compile compatibility. UI tweens are now driven
        // by the engine-owned managed tween runtime instead of script OnUpdate.
        public static void Update(float dt)
        {
        }

        public static void CancelAll(int entityId) => Tween.CancelAll(entityId);
        public static void CancelAll() => Tween.CancelAll();
        public static int ActiveCount => Tween.ActiveCount;

        internal static void ResetRuntimeState()
        {
        }
    }

    public static class UIOpacityTweenExtensions
    {
        private static bool CanTween(IUIOpacity component)
        {
            if (component is null)
                return false;

            return TweenGuards.IsEntityAlive(component.Entity.EntityID);
        }

        public static TweenHandle TweenOpacity<T>(this T component, float targetOpacity, float duration, Tween.Easing easing = Tween.Easing.EaseOutCubic)
            where T : IUIOpacity
        {
            if (!CanTween(component))
                return new NoOpTweenHandle();

            int entityId = component.Entity.EntityID;
            if (targetOpacity > 0f)
                component.visible = true;

            return Tween.To(
                    () => component.opacity,
                    value =>
                    {
                        if (CanTween(component))
                            component.opacity = value;
                    },
                    targetOpacity,
                    duration)
                .SetEase(easing)
                .SetTargetEntity(entityId)
                .SetTargetAlivePredicate(() => CanTween(component))
                .SetReplaceKey((entityId, typeof(T).FullName ?? typeof(T).Name, "opacity"));
        }

        public static T TweenOpacityIn<T>(this T component, float duration, Tween.Easing easing = Tween.Easing.EaseOutCubic, Action? onComplete = null)
            where T : IUIOpacity
        {
            if (!CanTween(component))
                return component;

            component.opacity = 0f;
            component.visible = true;
            TweenHandle tween = component.TweenOpacity(1f, duration, easing);
            if (onComplete != null)
                tween.OnComplete += onComplete;

            return component;
        }

        public static T TweenOpacityOut<T>(this T component, float duration, Tween.Easing easing = Tween.Easing.EaseOutCubic, Action? onComplete = null)
            where T : IUIOpacity
        {
            if (!CanTween(component))
                return component;

            TweenHandle tween = component.TweenOpacity(0f, duration, easing);
            tween.OnComplete += () =>
            {
                if (CanTween(component))
                    component.visible = false;

                onComplete?.Invoke();
            };
            return component;
        }

        public static T TweenOpacityTo<T>(this T component, float targetOpacity, float duration, Tween.Easing easing = Tween.Easing.EaseOutCubic, Action? onComplete = null)
            where T : IUIOpacity
        {
            if (!CanTween(component))
                return component;

            TweenHandle tween = component.TweenOpacity(targetOpacity, duration, easing);
            if (onComplete != null)
                tween.OnComplete += onComplete;

            return component;
        }

        [Obsolete("Use TweenOpacityIn instead.")]
        public static T FadeIn<T>(this T component, float duration, Tween.Easing easing = Tween.Easing.EaseOutCubic, Action? onComplete = null)
            where T : IUIOpacity
            => component.TweenOpacityIn(duration, easing, onComplete);

        [Obsolete("Use TweenOpacityOut instead.")]
        public static T FadeOut<T>(this T component, float duration, Tween.Easing easing = Tween.Easing.EaseOutCubic, Action? onComplete = null)
            where T : IUIOpacity
            => component.TweenOpacityOut(duration, easing, onComplete);

        [Obsolete("Use TweenOpacity or TweenOpacityTo instead.")]
        public static T FadeTo<T>(this T component, float targetOpacity, float duration, Tween.Easing easing = Tween.Easing.EaseOutCubic, Action? onComplete = null)
            where T : IUIOpacity
            => component.TweenOpacityTo(targetOpacity, duration, easing, onComplete);
    }

    public static class UIPositionTweenExtensions
    {
        private static bool CanTween(IUIPositionable component)
        {
            if (component is null)
                return false;

            return TweenGuards.IsEntityAlive(component.Entity.EntityID);
        }

        public static TweenHandle TweenAnchorOffset<T>(this T component, Vector2 targetPosition, float duration, Tween.Easing easing = Tween.Easing.EaseInOutQuad)
            where T : IUIPositionable
        {
            if (!CanTween(component))
                return new NoOpTweenHandle();

            int entityId = component.Entity.EntityID;
            return Tween.To(
                    () => component.anchorOffset,
                    value =>
                    {
                        if (CanTween(component))
                            component.anchorOffset = value;
                    },
                    targetPosition,
                    duration)
                .SetEase(easing)
                .SetTargetEntity(entityId)
                .SetTargetAlivePredicate(() => CanTween(component))
                .SetReplaceKey((entityId, typeof(T).FullName ?? typeof(T).Name, "anchorOffset"));
        }

        public static T TweenPosition<T>(this T component, Vector2 targetPosition, float duration, Tween.Easing easing = Tween.Easing.EaseInOutQuad, Action? onComplete = null)
            where T : IUIPositionable
        {
            if (!CanTween(component))
                return component;

            TweenHandle tween = component.TweenAnchorOffset(targetPosition, duration, easing);
            if (onComplete != null)
                tween.OnComplete += onComplete;

            return component;
        }
    }

    public static class UITweenCancelExtensions
    {
        public static T CancelTweens<T>(this T component) where T : ComponentBase
        {
            UITween.CancelAll(component.Entity.EntityID);
            return component;
        }
    }

    public static class UIRectTweenExtensions
    {
        private static bool CanTween(UIRect rect)
        {
            if (rect is null)
                return false;

            return TweenGuards.IsEntityAlive(rect.Entity.EntityID);
        }

        public static TweenHandle TweenRectOffset(this UIRect rect, Vector2 targetOffset, float duration, Tween.Easing easing = Tween.Easing.EaseInOutQuad)
        {
            if (!CanTween(rect))
                return new NoOpTweenHandle();

            int entityId = rect.Entity.EntityID;
            return Tween.To(
                    () => rect.offset,
                    value =>
                    {
                        if (CanTween(rect))
                            rect.offset = value;
                    },
                    targetOffset,
                    duration)
                .SetEase(easing)
                .SetTargetEntity(entityId)
                .SetTargetAlivePredicate(() => CanTween(rect))
                .SetReplaceKey((entityId, typeof(UIRect).FullName ?? nameof(UIRect), "offset"));
        }

        public static UIRect TweenOffset(this UIRect rect, Vector2 targetOffset, float duration, Tween.Easing easing = Tween.Easing.EaseInOutQuad, Action? onComplete = null)
        {
            if (!CanTween(rect))
                return rect;

            TweenHandle tween = rect.TweenRectOffset(targetOffset, duration, easing);
            if (onComplete != null)
                tween.OnComplete += onComplete;

            return rect;
        }
    }

    public static class ProgressBarTweenExtensions
    {
        private static bool CanTween(ProgressBar progressBar)
        {
            if (progressBar is null)
                return false;

            return TweenGuards.IsComponentAlive(progressBar);
        }

        /// <summary>
        /// Tweens the progress bar's raw value over time using the managed tween runtime.
        /// </summary>
        public static TweenHandle TweenValue(this ProgressBar progressBar, float targetValue, float duration, Tween.Easing easing = Tween.Easing.EaseOutCubic)
            => TweenValueCore(progressBar, () => progressBar.Value, value => progressBar.Value = value, targetValue, duration, easing);

        /// <summary>
        /// Tweens the progress bar's normalized 0-1 value over time using the managed tween runtime.
        /// </summary>
        public static TweenHandle TweenNormalizedValue(this ProgressBar progressBar, float targetValue, float duration, Tween.Easing easing = Tween.Easing.EaseOutCubic)
            => TweenValueCore(progressBar, () => progressBar.NormalizedValue, value => progressBar.NormalizedValue = value, targetValue, duration, easing);

        private static TweenHandle TweenValueCore(
            ProgressBar progressBar,
            Func<float> getter,
            Action<float> setter,
            float targetValue,
            float duration,
            Tween.Easing easing)
        {
            if (!CanTween(progressBar))
                return new NoOpTweenHandle();

            int entityId = progressBar.Entity.EntityID;
            return Tween.To(
                    getter,
                    value =>
                    {
                        if (CanTween(progressBar))
                            setter(value);
                    },
                    targetValue,
                    duration)
                .SetEase(easing)
                .SetTargetEntity(entityId)
                .SetTargetAlivePredicate(() => CanTween(progressBar))
                .SetReplaceKey((entityId, typeof(ProgressBar).FullName ?? nameof(ProgressBar), nameof(ProgressBar.Value)));
        }
    }
}
