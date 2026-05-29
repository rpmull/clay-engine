using System;
using System.Collections.Generic;

namespace ClaymoreEngine
{
    /// <summary>
    /// UI Dropdown component for selecting one option from a list.
    /// Requires sibling Panel and Text components on the same entity.
    /// </summary>
    public sealed class Dropdown : ComponentBase
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

        public int SelectedIndex
        {
            get => entity.IsValid && ComponentInterop.UI_Dropdown_GetSelectedIndex != null
                ? ComponentInterop.UI_Dropdown_GetSelectedIndex(entity.EntityID)
                : 0;
            set
            {
                if (!entity.IsValid)
                    return;

                ComponentInterop.UI_Dropdown_SetSelectedIndex?.Invoke(entity.EntityID, value);
            }
        }

        public int OptionCount => entity.IsValid && ComponentInterop.UI_Dropdown_GetOptionCount != null
            ? ComponentInterop.UI_Dropdown_GetOptionCount(entity.EntityID)
            : 0;

        public bool isOpen => entity.IsValid && ComponentInterop.UI_Dropdown_IsOpen != null
            ? ComponentInterop.UI_Dropdown_IsOpen(entity.EntityID)
            : false;

        public bool selectionChanged => entity.IsValid && ComponentInterop.UI_Dropdown_SelectionChanged != null
            ? ComponentInterop.UI_Dropdown_SelectionChanged(entity.EntityID)
            : false;

        public string GetOption(int index)
        {
            if (!entity.IsValid || index < 0 || index >= OptionCount)
                return string.Empty;

            return ComponentInterop.UI_Dropdown_GetOption(entity.EntityID, index);
        }

        public void SetOption(int index, string text)
        {
            if (!entity.IsValid || index < 0)
                return;

            ComponentInterop.UI_Dropdown_SetOption?.Invoke(entity.EntityID, index, text ?? string.Empty);
        }

        public void AddOption(string text)
        {
            if (!entity.IsValid)
                return;

            ComponentInterop.UI_Dropdown_AddOption?.Invoke(entity.EntityID, text ?? string.Empty);
        }

        public void ClearOptions()
        {
            if (!entity.IsValid)
                return;

            ComponentInterop.UI_Dropdown_ClearOptions?.Invoke(entity.EntityID);
        }

        public void SetOptions(IEnumerable<string> options)
        {
            ClearOptions();
            if (options == null)
                return;

            foreach (string option in options)
            {
                AddOption(option);
            }
        }

        public string[] GetOptions()
        {
            int count = OptionCount;
            if (count <= 0)
                return Array.Empty<string>();

            string[] options = new string[count];
            for (int i = 0; i < count; i++)
            {
                options[i] = GetOption(i);
            }

            return options;
        }
    }
}
