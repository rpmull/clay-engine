using System;

namespace ClaymoreEngine
{
    /// <summary>
    /// UI Progress Bar component for displaying progress values.
    /// Access via entity.GetComponent&lt;ProgressBar&gt;().
    /// </summary>
    public sealed class ProgressBar : ComponentBase, IUIOpacity
    {
        public bool worldSpace
        {
            get => UIRenderSpaceUtility.GetWorldSpace(entity);
            set => UIRenderSpaceUtility.SetWorldSpace(entity, value);
        }

        public bool billboard
        {
            get => UIRenderSpaceUtility.GetBillboard(entity);
            set => UIRenderSpaceUtility.SetBillboard(entity, value);
        }

        /// <summary>
        /// The opacity of the progress bar fill (0-1).
        /// </summary>
        public float opacity
        {
            get => entity.IsValid && ComponentInterop.UI_ProgressBar_GetOpacity != null
                ? ComponentInterop.UI_ProgressBar_GetOpacity(entity.EntityID)
                : 1.0f;
            set { if (entity.IsValid) ComponentInterop.UI_ProgressBar_SetOpacity?.Invoke(entity.EntityID, value); }
        }

        /// <summary>
        /// Whether the progress bar fill is visible.
        /// </summary>
        public bool visible
        {
            get => entity.IsValid && ComponentInterop.UI_ProgressBar_GetVisible != null
                ? ComponentInterop.UI_ProgressBar_GetVisible(entity.EntityID)
                : true;
            set { if (entity.IsValid) ComponentInterop.UI_ProgressBar_SetVisible?.Invoke(entity.EntityID, value); }
        }

        /// <summary>
        /// The current value of the progress bar.
        /// Managed writes update immediately; use TweenValue for eased transitions.
        /// </summary>
        public float Value
        {
            get => entity.IsValid && ComponentInterop.UI_ProgressBar_GetValue != null 
                ? ComponentInterop.UI_ProgressBar_GetValue(entity.EntityID) 
                : 0f;
            set { if (entity.IsValid) ComponentInterop.UI_ProgressBar_SetValue?.Invoke(entity.EntityID, value); }
        }

        /// <summary>
        /// The minimum value of the progress bar.
        /// </summary>
        public float MinValue
        {
            get => entity.IsValid && ComponentInterop.UI_ProgressBar_GetMinValue != null 
                ? ComponentInterop.UI_ProgressBar_GetMinValue(entity.EntityID) 
                : 0f;
            set { if (entity.IsValid) ComponentInterop.UI_ProgressBar_SetMinValue?.Invoke(entity.EntityID, value); }
        }

        /// <summary>
        /// The maximum value of the progress bar.
        /// </summary>
        public float MaxValue
        {
            get => entity.IsValid && ComponentInterop.UI_ProgressBar_GetMaxValue != null 
                ? ComponentInterop.UI_ProgressBar_GetMaxValue(entity.EntityID) 
                : 1f;
            set { if (entity.IsValid) ComponentInterop.UI_ProgressBar_SetMaxValue?.Invoke(entity.EntityID, value); }
        }

        /// <summary>
        /// Gets or sets the value as a normalized 0-1 range.
        /// Managed writes update immediately; use TweenNormalizedValue for eased transitions.
        /// </summary>
        public float NormalizedValue
        {
            get
            {
                float min = MinValue;
                float max = MaxValue;
                float range = max - min;
                if (range <= 0f) return 0f;
                return (Value - min) / range;
            }
            set
            {
                float min = MinValue;
                float max = MaxValue;
                Value = min + value * (max - min);
            }
        }

        /// <summary>
        /// Sets the value, clamped to the min/max range.
        /// </summary>
        /// <param name="value">The value to set.</param>
        public void SetValueClamped(float value)
        {
            float min = MinValue;
            float max = MaxValue;
            Value = MathF.Max(min, MathF.Min(max, value));
        }

        /// <summary>
        /// Sets the progress bar range (min and max values).
        /// </summary>
        /// <param name="min">The minimum value.</param>
        /// <param name="max">The maximum value.</param>
        public void SetRange(float min, float max)
        {
            MinValue = min;
            MaxValue = max;
        }

        /// <summary>
        /// Increments the value by a delta amount, clamped to max.
        /// </summary>
        /// <param name="delta">The amount to add to the current value.</param>
        public void Increment(float delta = 1f)
        {
            SetValueClamped(Value + delta);
        }

        /// <summary>
        /// Decrements the value by a delta amount, clamped to min.
        /// </summary>
        /// <param name="delta">The amount to subtract from the current value.</param>
        public void Decrement(float delta = 1f)
        {
            SetValueClamped(Value - delta);
        }
    }
}

