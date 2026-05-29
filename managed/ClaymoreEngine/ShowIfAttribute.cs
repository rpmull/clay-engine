using System;

namespace ClaymoreEngine
{
    /// <summary>
    /// Shows this field in the ClayScriptableObject inspector only when the named field equals one of the given values.
    /// Supports enum (compared by integer), bool, int, float, and string.
    /// Use multiple arguments for OR semantics, e.g. [ShowIf("itemType", ItemType.Armor, ItemType.Weapon)].
    /// </summary>
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class ShowIfAttribute : Attribute
    {
        public string FieldName { get; }
        public object?[] Values { get; }

        public ShowIfAttribute(string fieldName, object? value)
        {
            FieldName = fieldName ?? string.Empty;
            Values = new[] { value };
        }

        /// <summary>Show when the named field equals any of the given values (OR).</summary>
        public ShowIfAttribute(string fieldName, params object?[] values)
        {
            FieldName = fieldName ?? string.Empty;
            Values = values ?? Array.Empty<object?>();
        }
    }

    /// <summary>
    /// Hides this field in the ClayScriptableObject inspector when the named field equals one of the given values.
    /// Use multiple arguments for OR semantics, e.g. [HideIf("itemType", ItemType.Consumable)].
    /// </summary>
    [AttributeUsage(AttributeTargets.Field)]
    public sealed class HideIfAttribute : Attribute
    {
        public string FieldName { get; }
        public object?[] Values { get; }

        public HideIfAttribute(string fieldName, object? value)
        {
            FieldName = fieldName ?? string.Empty;
            Values = new[] { value };
        }

        /// <summary>Hide when the named field equals any of the given values (OR).</summary>
        public HideIfAttribute(string fieldName, params object?[] values)
        {
            FieldName = fieldName ?? string.Empty;
            Values = values ?? Array.Empty<object?>();
        }
    }
}
