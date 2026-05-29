namespace ClaymoreEngine
{
    /// <summary>
    /// UI Input Field component for editable text entry.
    /// </summary>
    public sealed class InputField : ComponentBase
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

        public string Text
        {
            get => entity.IsValid ? ComponentInterop.UI_InputField_GetText(entity.EntityID) : string.Empty;
            set
            {
                if (!entity.IsValid)
                    return;

                ComponentInterop.UI_InputField_SetText?.Invoke(entity.EntityID, value ?? string.Empty);
            }
        }

        public string PlaceholderText
        {
            get => entity.IsValid ? ComponentInterop.UI_InputField_GetPlaceholder(entity.EntityID) : string.Empty;
            set
            {
                if (!entity.IsValid)
                    return;

                ComponentInterop.UI_InputField_SetPlaceholder?.Invoke(entity.EntityID, value ?? string.Empty);
            }
        }

        public bool IsFocused => entity.IsValid && ComponentInterop.UI_InputField_IsFocused != null
            ? ComponentInterop.UI_InputField_IsFocused(entity.EntityID)
            : false;

        public bool TextChanged => entity.IsValid && ComponentInterop.UI_InputField_TextChanged != null
            ? ComponentInterop.UI_InputField_TextChanged(entity.EntityID)
            : false;
    }
}
