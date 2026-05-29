namespace ClaymoreEngine
{
    /// <summary>
    /// UI Toggle component for on/off state and radio-style groups.
    /// </summary>
    public sealed class Toggle : ComponentBase
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

        public bool IsOn
        {
            get => entity.IsValid && ComponentInterop.UI_Toggle_GetIsOn != null
                ? ComponentInterop.UI_Toggle_GetIsOn(entity.EntityID)
                : false;
            set
            {
                if (!entity.IsValid)
                    return;

                ComponentInterop.UI_Toggle_SetIsOn?.Invoke(entity.EntityID, value);
            }
        }

        public bool IsHovered => entity.IsValid && ComponentInterop.UI_Toggle_IsHovered != null
            ? ComponentInterop.UI_Toggle_IsHovered(entity.EntityID)
            : false;

        public bool IsPressed => entity.IsValid && ComponentInterop.UI_Toggle_IsPressed != null
            ? ComponentInterop.UI_Toggle_IsPressed(entity.EntityID)
            : false;

        public bool ValueChanged => entity.IsValid && ComponentInterop.UI_Toggle_ValueChanged != null
            ? ComponentInterop.UI_Toggle_ValueChanged(entity.EntityID)
            : false;
    }
}
