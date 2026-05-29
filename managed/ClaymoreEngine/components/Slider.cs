using System;

namespace ClaymoreEngine
{
    /// <summary>
    /// UI Slider component for interactive range selection.
    /// Access via entity.GetComponent&lt;Slider&gt;().
    /// </summary>
    public sealed class Slider : ComponentBase
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

        public float Value
        {
            get => entity.IsValid && ComponentInterop.UI_Slider_GetValue != null
                ? ComponentInterop.UI_Slider_GetValue(entity.EntityID)
                : 0f;
            set
            {
                if (!entity.IsValid)
                    return;

                ComponentInterop.UI_Slider_SetValue?.Invoke(entity.EntityID, value);
            }
        }

        public float MinValue
        {
            get => entity.IsValid && ComponentInterop.UI_Slider_GetMinValue != null
                ? ComponentInterop.UI_Slider_GetMinValue(entity.EntityID)
                : 0f;
            set
            {
                if (!entity.IsValid)
                    return;

                ComponentInterop.UI_Slider_SetMinValue?.Invoke(entity.EntityID, value);
            }
        }

        public float MaxValue
        {
            get => entity.IsValid && ComponentInterop.UI_Slider_GetMaxValue != null
                ? ComponentInterop.UI_Slider_GetMaxValue(entity.EntityID)
                : 1f;
            set
            {
                if (!entity.IsValid)
                    return;

                ComponentInterop.UI_Slider_SetMaxValue?.Invoke(entity.EntityID, value);
            }
        }

        public float NormalizedValue
        {
            get
            {
                float min = MinValue;
                float max = MaxValue;
                float range = max - min;
                if (range <= 0f)
                    return 0f;

                return (Value - min) / range;
            }
            set
            {
                float clamped = Math.Clamp(value, 0f, 1f);
                float min = MinValue;
                float max = MaxValue;
                Value = min + (max - min) * clamped;
            }
        }

        public bool hovered => entity.IsValid && ComponentInterop.UI_Slider_IsHovered != null
            ? ComponentInterop.UI_Slider_IsHovered(entity.EntityID)
            : false;

        public bool dragging => entity.IsValid && ComponentInterop.UI_Slider_IsDragging != null
            ? ComponentInterop.UI_Slider_IsDragging(entity.EntityID)
            : false;

        public bool valueChanged => entity.IsValid && ComponentInterop.UI_Slider_ValueChanged != null
            ? ComponentInterop.UI_Slider_ValueChanged(entity.EntityID)
            : false;

        public void SetRange(float min, float max)
        {
            MinValue = min;
            MaxValue = max;
        }

        public void SetValueClamped(float value)
        {
            float min = MinValue;
            float max = MaxValue;
            Value = Math.Clamp(value, min, max);
        }
    }
}
