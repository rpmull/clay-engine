using System;
using System.Numerics;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public unsafe delegate void MaterialInteropInitDelegate(IntPtr* functionPointers,
        int count);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Material_SetVector4Fn(int entityId,
        [MarshalAs(UnmanagedType.LPStr)] string propertyName, float x, float y,
        float z, float w);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Material_GetVector4Fn(int entityId,
        [MarshalAs(UnmanagedType.LPStr)] string propertyName, out float x,
        out float y, out float z, out float w);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate bool Material_HasPropertyFn(int entityId,
        [MarshalAs(UnmanagedType.LPStr)] string propertyName);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Material_RemovePropertyFn(int entityId,
        [MarshalAs(UnmanagedType.LPStr)] string propertyName);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Material_ClearAllFn(int entityId);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Material_SetVector4SlotFn(int entityId, int slotIndex,
        [MarshalAs(UnmanagedType.LPStr)] string propertyName, float x, float y,
        float z, float w);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Material_GetVector4SlotFn(int entityId, int slotIndex,
        [MarshalAs(UnmanagedType.LPStr)] string propertyName, out float x,
        out float y, out float z, out float w);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate bool Material_HasPropertySlotFn(int entityId, int slotIndex,
        [MarshalAs(UnmanagedType.LPStr)] string propertyName);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Material_RemovePropertySlotFn(int entityId,
        int slotIndex, [MarshalAs(UnmanagedType.LPStr)] string propertyName);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Material_ClearSlotFn(int entityId, int slotIndex);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int Material_GetSlotCountFn(int entityId);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Material_SetTexturePathFn(int entityId,
        [MarshalAs(UnmanagedType.LPStr)] string propertyName,
        [MarshalAs(UnmanagedType.LPStr)] string assetPath);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Material_SetTexturePathSlotFn(int entityId,
        int slotIndex, [MarshalAs(UnmanagedType.LPStr)] string propertyName,
        [MarshalAs(UnmanagedType.LPStr)] string assetPath);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int Material_GetMaterialTypeSlotFn(int entityId,
        int slotIndex);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate IntPtr MaterialSlotStringFn(int entityId, int slotIndex);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate bool Material_SetMaterialAssetPathSlotFn(int entityId,
        int slotIndex, [MarshalAs(UnmanagedType.LPStr)] string assetPath);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate bool Material_HasMaterialPropertySlotFn(int entityId,
        int slotIndex, [MarshalAs(UnmanagedType.LPStr)] string propertyName);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate IntPtr Material_GetMaterialTexturePathSlotFn(int entityId,
        int slotIndex, [MarshalAs(UnmanagedType.LPStr)] string propertyName);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate float Material_GetPbrScalarSlotFn(int entityId,
        int slotIndex, int scalarProperty);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Material_SetPbrScalarSlotFn(int entityId,
        int slotIndex, int scalarProperty, float value);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Material_GetPbrEmissionColorSlotFn(int entityId,
        int slotIndex, out float x, out float y, out float z);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Material_SetPbrEmissionColorSlotFn(int entityId,
        int slotIndex, float x, float y, float z);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Material_GetPbrUVTransformSlotFn(int entityId,
        int slotIndex, out float scaleX, out float scaleY, out float offsetX,
        out float offsetY);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Material_SetPbrUVTransformSlotFn(int entityId,
        int slotIndex, float scaleX, float scaleY, float offsetX,
        float offsetY);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate bool Material_GetPbrBoolSlotFn(int entityId,
        int slotIndex);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Material_SetPbrBoolSlotFn(int entityId,
        int slotIndex, [MarshalAs(UnmanagedType.I1)] bool value);

    public static class MaterialInterop
    {
        public static Material_SetVector4Fn? SetVector4;
        public static Material_GetVector4Fn? GetVector4;
        public static Material_HasPropertyFn? HasProperty;
        public static Material_RemovePropertyFn? RemoveProperty;
        public static Material_ClearAllFn? ClearAll;
        public static Material_SetTexturePathFn? SetTexturePath;

        public static Material_SetVector4SlotFn? SetVector4Slot;
        public static Material_GetVector4SlotFn? GetVector4Slot;
        public static Material_HasPropertySlotFn? HasPropertySlot;
        public static Material_RemovePropertySlotFn? RemovePropertySlot;
        public static Material_ClearSlotFn? ClearSlot;
        public static Material_GetSlotCountFn? GetSlotCount;
        public static Material_SetTexturePathSlotFn? SetTexturePathSlot;

        public static Material_GetMaterialTypeSlotFn? GetMaterialTypeSlot;
        public static MaterialSlotStringFn? GetMaterialNameSlot;
        public static MaterialSlotStringFn? GetMaterialAssetPathSlot;
        public static Material_SetMaterialAssetPathSlotFn?
            SetMaterialAssetPathSlot;
        public static Material_SetVector4SlotFn? SetMaterialVector4Slot;
        public static Material_GetVector4SlotFn? GetMaterialVector4Slot;
        public static Material_HasMaterialPropertySlotFn?
            HasMaterialPropertySlot;
        public static Material_SetTexturePathSlotFn?
            SetMaterialTexturePathSlot;
        public static Material_GetMaterialTexturePathSlotFn?
            GetMaterialTexturePathSlot;
        public static Material_GetPbrScalarSlotFn? GetPbrScalarSlot;
        public static Material_SetPbrScalarSlotFn? SetPbrScalarSlot;
        public static Material_GetPbrEmissionColorSlotFn?
            GetPbrEmissionColorSlot;
        public static Material_SetPbrEmissionColorSlotFn?
            SetPbrEmissionColorSlot;
        public static Material_GetPbrUVTransformSlotFn? GetPbrUVTransformSlot;
        public static Material_SetPbrUVTransformSlotFn? SetPbrUVTransformSlot;
        public static Material_GetPbrBoolSlotFn?
            GetPbrReceiveShadowsOverrideSlot;
        public static Material_SetPbrBoolSlotFn?
            SetPbrReceiveShadowsOverrideSlot;
        public static Material_GetPbrBoolSlotFn? GetPbrReceiveShadowsSlot;
        public static Material_SetPbrBoolSlotFn? SetPbrReceiveShadowsSlot;

        private const int MaterialInteropCount = 32;

        public static unsafe void Initialize(IntPtr* ptrs, int count)
        {
            if (count < MaterialInteropCount)
            {
                Console.WriteLine($"[MaterialInterop] Warning: expected " +
                    $"{MaterialInteropCount} pointers, got {count}");
                return;
            }

            int i = 0;
            SetVector4 = Marshal.GetDelegateForFunctionPointer<
                Material_SetVector4Fn>(ptrs[i++]);
            GetVector4 = Marshal.GetDelegateForFunctionPointer<
                Material_GetVector4Fn>(ptrs[i++]);
            HasProperty = Marshal.GetDelegateForFunctionPointer<
                Material_HasPropertyFn>(ptrs[i++]);
            RemoveProperty = Marshal.GetDelegateForFunctionPointer<
                Material_RemovePropertyFn>(ptrs[i++]);
            ClearAll = Marshal.GetDelegateForFunctionPointer<
                Material_ClearAllFn>(ptrs[i++]);
            SetTexturePath = Marshal.GetDelegateForFunctionPointer<
                Material_SetTexturePathFn>(ptrs[i++]);
            SetVector4Slot = Marshal.GetDelegateForFunctionPointer<
                Material_SetVector4SlotFn>(ptrs[i++]);
            GetVector4Slot = Marshal.GetDelegateForFunctionPointer<
                Material_GetVector4SlotFn>(ptrs[i++]);
            HasPropertySlot = Marshal.GetDelegateForFunctionPointer<
                Material_HasPropertySlotFn>(ptrs[i++]);
            RemovePropertySlot = Marshal.GetDelegateForFunctionPointer<
                Material_RemovePropertySlotFn>(ptrs[i++]);
            ClearSlot = Marshal.GetDelegateForFunctionPointer<
                Material_ClearSlotFn>(ptrs[i++]);
            GetSlotCount = Marshal.GetDelegateForFunctionPointer<
                Material_GetSlotCountFn>(ptrs[i++]);
            SetTexturePathSlot = Marshal.GetDelegateForFunctionPointer<
                Material_SetTexturePathSlotFn>(ptrs[i++]);
            GetMaterialTypeSlot = Marshal.GetDelegateForFunctionPointer<
                Material_GetMaterialTypeSlotFn>(ptrs[i++]);
            GetMaterialNameSlot = Marshal.GetDelegateForFunctionPointer<
                MaterialSlotStringFn>(ptrs[i++]);
            GetMaterialAssetPathSlot = Marshal.GetDelegateForFunctionPointer<
                MaterialSlotStringFn>(ptrs[i++]);
            SetMaterialAssetPathSlot = Marshal.GetDelegateForFunctionPointer<
                Material_SetMaterialAssetPathSlotFn>(ptrs[i++]);
            SetMaterialVector4Slot = Marshal.GetDelegateForFunctionPointer<
                Material_SetVector4SlotFn>(ptrs[i++]);
            GetMaterialVector4Slot = Marshal.GetDelegateForFunctionPointer<
                Material_GetVector4SlotFn>(ptrs[i++]);
            HasMaterialPropertySlot = Marshal.GetDelegateForFunctionPointer<
                Material_HasMaterialPropertySlotFn>(ptrs[i++]);
            SetMaterialTexturePathSlot =
                Marshal.GetDelegateForFunctionPointer<
                    Material_SetTexturePathSlotFn>(ptrs[i++]);
            GetMaterialTexturePathSlot =
                Marshal.GetDelegateForFunctionPointer<
                    Material_GetMaterialTexturePathSlotFn>(ptrs[i++]);
            GetPbrScalarSlot = Marshal.GetDelegateForFunctionPointer<
                Material_GetPbrScalarSlotFn>(ptrs[i++]);
            SetPbrScalarSlot = Marshal.GetDelegateForFunctionPointer<
                Material_SetPbrScalarSlotFn>(ptrs[i++]);
            GetPbrEmissionColorSlot = Marshal.GetDelegateForFunctionPointer<
                Material_GetPbrEmissionColorSlotFn>(ptrs[i++]);
            SetPbrEmissionColorSlot = Marshal.GetDelegateForFunctionPointer<
                Material_SetPbrEmissionColorSlotFn>(ptrs[i++]);
            GetPbrUVTransformSlot = Marshal.GetDelegateForFunctionPointer<
                Material_GetPbrUVTransformSlotFn>(ptrs[i++]);
            SetPbrUVTransformSlot = Marshal.GetDelegateForFunctionPointer<
                Material_SetPbrUVTransformSlotFn>(ptrs[i++]);
            GetPbrReceiveShadowsOverrideSlot =
                Marshal.GetDelegateForFunctionPointer<
                    Material_GetPbrBoolSlotFn>(ptrs[i++]);
            SetPbrReceiveShadowsOverrideSlot =
                Marshal.GetDelegateForFunctionPointer<
                    Material_SetPbrBoolSlotFn>(ptrs[i++]);
            GetPbrReceiveShadowsSlot = Marshal.GetDelegateForFunctionPointer<
                Material_GetPbrBoolSlotFn>(ptrs[i++]);
            SetPbrReceiveShadowsSlot =
                Marshal.GetDelegateForFunctionPointer<
                    Material_SetPbrBoolSlotFn>(ptrs[i++]);

            Console.WriteLine($"[MaterialInterop] Initialized with {i} " +
                "functions");
        }
    }

    public static class MaterialProperties
    {
        public const string Albedo = "s_albedo";
        public const string MetallicRoughness = "s_metallicRoughness";
        public const string NormalMap = "s_normalMap";
        public const string AO = "s_ao";
        public const string Emission = "s_emission";
        public const string TintMask = "s_tintMask";
        public const string Displacement = "s_displacement";

        public const string ColorTint = "u_ColorTint";
        public const string TintParams = "u_TintParams";
        public const string TextureUsage = "u_TextureUsage";
        public const string UVTransform = "u_UVTransform";
        public const string UVScaleOffset = UVTransform;
        public const string EmissionColor = "u_EmissionColor";
        public const string PBRScalar0 = "u_PBRScalar0";
        public const string PBRScalar1 = "u_PBRScalar1";
        public const string DisplacementParams = "u_DisplacementParams";

        public const string PSXParams = "u_psxParams";
        public const string PSXWorld = "u_psxWorld";
        public const string ToonParams = "u_toonParams";
        public const string Posterize = "u_posterize";
        public const string PSXShadowParams = "u_psxShadowParams";
        public const string PSXEmission = "u_psxEmission";
    }

    public sealed class MaterialComponent : ComponentBase
    {
        private static Vector4 GetDefaultVector4Value(string propertyName)
        {
            return propertyName == MaterialProperties.ColorTint
                ? Vector4.One
                : Vector4.Zero;
        }

        public int SlotCount => MaterialInterop.GetSlotCount?.Invoke(Entity.EntityID)
            ?? 0;

        public void SetVector4(string propertyName, Vector4 value)
        {
            MaterialInterop.SetVector4?.Invoke(Entity.EntityID, propertyName,
                value.X, value.Y, value.Z, value.W);
        }

        public void SetFloat(string propertyName, float value)
        {
            SetVector4(propertyName, new Vector4(value, 0, 0, 0));
        }

        public void SetColor(string propertyName, Vector4 color)
        {
            SetVector4(propertyName, color);
        }

        public void SetVector2(string propertyName, Vector2 value)
        {
            SetVector4(propertyName, new Vector4(value.X, value.Y, 0, 0));
        }

        public void SetVector3(string propertyName, Vector3 value)
        {
            SetVector4(propertyName, new Vector4(value.X, value.Y, value.Z, 0));
        }

        public Vector4 GetVector4(string propertyName)
        {
            if (MaterialInterop.GetVector4 == null || !HasProperty(propertyName))
            {
                return GetDefaultVector4Value(propertyName);
            }

            MaterialInterop.GetVector4(Entity.EntityID, propertyName, out float x,
                out float y, out float z, out float w);
            return new Vector4(x, y, z, w);
        }

        public float GetFloat(string propertyName) => GetVector4(propertyName).X;

        public bool HasProperty(string propertyName)
        {
            return MaterialInterop.HasProperty?.Invoke(Entity.EntityID,
                propertyName) ?? false;
        }

        public void RemoveProperty(string propertyName)
        {
            MaterialInterop.RemoveProperty?.Invoke(Entity.EntityID, propertyName);
        }

        public void ClearAll()
        {
            MaterialInterop.ClearAll?.Invoke(Entity.EntityID);
        }

        public void SetTexture(string propertyName, string assetPath)
        {
            MaterialInterop.SetTexturePath?.Invoke(Entity.EntityID, propertyName,
                assetPath);
        }

        public void SetVector4(int slot, string propertyName, Vector4 value)
        {
            MaterialInterop.SetVector4Slot?.Invoke(Entity.EntityID, slot,
                propertyName, value.X, value.Y, value.Z, value.W);
        }

        public void SetFloat(int slot, string propertyName, float value)
        {
            SetVector4(slot, propertyName, new Vector4(value, 0, 0, 0));
        }

        public void SetColor(int slot, string propertyName, Vector4 color)
        {
            SetVector4(slot, propertyName, color);
        }

        public Vector4 GetVector4(int slot, string propertyName)
        {
            if (MaterialInterop.GetVector4Slot == null || !HasProperty(slot,
                propertyName))
            {
                return GetDefaultVector4Value(propertyName);
            }

            MaterialInterop.GetVector4Slot(Entity.EntityID, slot, propertyName,
                out float x, out float y, out float z, out float w);
            return new Vector4(x, y, z, w);
        }

        public bool HasProperty(int slot, string propertyName)
        {
            return MaterialInterop.HasPropertySlot?.Invoke(Entity.EntityID, slot,
                propertyName) ?? false;
        }

        public void RemoveProperty(int slot, string propertyName)
        {
            MaterialInterop.RemovePropertySlot?.Invoke(Entity.EntityID, slot,
                propertyName);
        }

        public void ClearSlot(int slot)
        {
            MaterialInterop.ClearSlot?.Invoke(Entity.EntityID, slot);
        }

        public void SetTexture(int slot, string propertyName, string assetPath)
        {
            MaterialInterop.SetTexturePathSlot?.Invoke(Entity.EntityID, slot,
                propertyName, assetPath);
        }

        public Vector4 ColorTint
        {
            get => GetVector4(MaterialProperties.ColorTint);
            set => SetVector4(MaterialProperties.ColorTint, value);
        }

        public Vector4 UVScaleOffset
        {
            get => GetVector4(MaterialProperties.UVScaleOffset);
            set => SetVector4(MaterialProperties.UVScaleOffset, value);
        }
    }
}
