using System;
using System.Numerics;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    public enum MaterialKind
    {
        None = 0,
        PBR = 1,
        PSX = 2,
        ShaderGraph = 3,
        Custom = 4,
    }

    internal enum PbrMaterialScalar
    {
        Metallic = 0,
        Roughness = 1,
        NormalScale = 2,
        AmbientOcclusion = 3,
        EmissionStrength = 4,
        DisplacementScale = 5,
    }

    public sealed class Material
    {
        private readonly Entity entity;

        internal Material(Entity entity, int slotIndex)
        {
            this.entity = entity;
            SlotIndex = slotIndex;
        }

        public Entity Entity => entity;
        public int SlotIndex { get; }

        public bool IsValid
        {
            get
            {
                if (!entity.IsValid || SlotIndex < 0)
                {
                    return false;
                }

                int slotCount = MaterialInterop.GetSlotCount?.Invoke(entity.EntityID) ?? 0;
                return SlotIndex < slotCount && Type != MaterialKind.None;
            }
        }

        public MaterialKind Type =>
            (MaterialKind)(MaterialInterop.GetMaterialTypeSlot?.Invoke(
                entity.EntityID, SlotIndex) ?? 0);

        public string Name => ReadSlotString(
            MaterialInterop.GetMaterialNameSlot, string.Empty);

        public string AssetPath
        {
            get => ReadSlotString(MaterialInterop.GetMaterialAssetPathSlot,
                string.Empty);
            set => MaterialInterop.SetMaterialAssetPathSlot?.Invoke(
                entity.EntityID, SlotIndex, value ?? string.Empty);
        }

        public Vector4 ColorTint
        {
            get => GetVector4(MaterialProperties.ColorTint,
                defaultValue: Vector4.One);
            set => SetVector4(MaterialProperties.ColorTint, value);
        }

        public string AlbedoPath
        {
            get => GetTexturePath(MaterialProperties.Albedo);
            set => SetTexture(MaterialProperties.Albedo, value);
        }

        public string MetallicRoughnessPath
        {
            get => GetTexturePath(MaterialProperties.MetallicRoughness);
            set => SetTexture(MaterialProperties.MetallicRoughness, value);
        }

        public string NormalPath
        {
            get => GetTexturePath(MaterialProperties.NormalMap);
            set => SetTexture(MaterialProperties.NormalMap, value);
        }

        public string AmbientOcclusionPath
        {
            get => GetTexturePath(MaterialProperties.AO);
            set => SetTexture(MaterialProperties.AO, value);
        }

        public string EmissionPath
        {
            get => GetTexturePath(MaterialProperties.Emission);
            set => SetTexture(MaterialProperties.Emission, value);
        }

        public string DisplacementPath
        {
            get => GetTexturePath(MaterialProperties.Displacement);
            set => SetTexture(MaterialProperties.Displacement, value);
        }

        public string TintMaskPath
        {
            get => GetTexturePath(MaterialProperties.TintMask);
            set => SetTexture(MaterialProperties.TintMask, value);
        }

        public float Metallic
        {
            get => GetPbrScalar(PbrMaterialScalar.Metallic);
            set => SetPbrScalar(PbrMaterialScalar.Metallic, value);
        }

        public float Roughness
        {
            get => GetPbrScalar(PbrMaterialScalar.Roughness);
            set => SetPbrScalar(PbrMaterialScalar.Roughness, value);
        }

        public float NormalScale
        {
            get => GetPbrScalar(PbrMaterialScalar.NormalScale);
            set => SetPbrScalar(PbrMaterialScalar.NormalScale, value);
        }

        public float AmbientOcclusion
        {
            get => GetPbrScalar(PbrMaterialScalar.AmbientOcclusion);
            set => SetPbrScalar(PbrMaterialScalar.AmbientOcclusion, value);
        }

        public float EmissionStrength
        {
            get => GetPbrScalar(PbrMaterialScalar.EmissionStrength);
            set => SetPbrScalar(PbrMaterialScalar.EmissionStrength, value);
        }

        public float DisplacementScale
        {
            get => GetPbrScalar(PbrMaterialScalar.DisplacementScale);
            set => SetPbrScalar(PbrMaterialScalar.DisplacementScale, value);
        }

        public Vector3 EmissionColor
        {
            get
            {
                if (MaterialInterop.GetPbrEmissionColorSlot == null)
                {
                    return Vector3.One;
                }

                MaterialInterop.GetPbrEmissionColorSlot(entity.EntityID, SlotIndex,
                    out float x, out float y, out float z);
                return new Vector3(x, y, z);
            }
            set => MaterialInterop.SetPbrEmissionColorSlot?.Invoke(
                entity.EntityID, SlotIndex, value.X, value.Y, value.Z);
        }

        public Vector4 UVTransform
        {
            get
            {
                if (MaterialInterop.GetPbrUVTransformSlot == null)
                {
                    return new Vector4(1f, 1f, 0f, 0f);
                }

                MaterialInterop.GetPbrUVTransformSlot(entity.EntityID, SlotIndex,
                    out float scaleX, out float scaleY, out float offsetX,
                    out float offsetY);
                return new Vector4(scaleX, scaleY, offsetX, offsetY);
            }
            set => MaterialInterop.SetPbrUVTransformSlot?.Invoke(
                entity.EntityID, SlotIndex, value.X, value.Y, value.Z, value.W);
        }

        public bool ReceiveShadowsOverride
        {
            get => MaterialInterop.GetPbrReceiveShadowsOverrideSlot?.Invoke(
                entity.EntityID, SlotIndex) ?? false;
            set => MaterialInterop.SetPbrReceiveShadowsOverrideSlot?.Invoke(
                entity.EntityID, SlotIndex, value);
        }

        public bool ReceiveShadows
        {
            get => MaterialInterop.GetPbrReceiveShadowsSlot?.Invoke(
                entity.EntityID, SlotIndex) ?? false;
            set => MaterialInterop.SetPbrReceiveShadowsSlot?.Invoke(
                entity.EntityID, SlotIndex, value);
        }

        public bool HasProperty(string propertyName)
        {
            return MaterialInterop.HasMaterialPropertySlot?.Invoke(entity.EntityID,
                SlotIndex, propertyName) ?? false;
        }

        public Vector4 GetVector4(string propertyName, Vector4? defaultValue = null)
        {
            if (MaterialInterop.GetMaterialVector4Slot == null)
            {
                return defaultValue ?? Vector4.Zero;
            }

            if (!HasProperty(propertyName))
            {
                return defaultValue ?? Vector4.Zero;
            }

            MaterialInterop.GetMaterialVector4Slot(entity.EntityID, SlotIndex,
                propertyName, out float x, out float y, out float z, out float w);
            return new Vector4(x, y, z, w);
        }

        public void SetVector4(string propertyName, Vector4 value)
        {
            MaterialInterop.SetMaterialVector4Slot?.Invoke(entity.EntityID,
                SlotIndex, propertyName, value.X, value.Y, value.Z, value.W);
        }

        public string GetTexturePath(string propertyName)
        {
            if (MaterialInterop.GetMaterialTexturePathSlot == null)
            {
                return string.Empty;
            }

            IntPtr ptr = MaterialInterop.GetMaterialTexturePathSlot(
                entity.EntityID, SlotIndex, propertyName);
            return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) ??
                string.Empty : string.Empty;
        }

        public void SetTexture(string propertyName, string assetPath)
        {
            MaterialInterop.SetMaterialTexturePathSlot?.Invoke(entity.EntityID,
                SlotIndex, propertyName, assetPath ?? string.Empty);
        }

        private float GetPbrScalar(PbrMaterialScalar scalar)
        {
            return MaterialInterop.GetPbrScalarSlot?.Invoke(entity.EntityID,
                SlotIndex, (int)scalar) ?? 0f;
        }

        private void SetPbrScalar(PbrMaterialScalar scalar, float value)
        {
            MaterialInterop.SetPbrScalarSlot?.Invoke(entity.EntityID, SlotIndex,
                (int)scalar, value);
        }

        private string ReadSlotString(MaterialSlotStringFn? getter,
            string fallback)
        {
            if (getter == null)
            {
                return fallback;
            }

            IntPtr ptr = getter(entity.EntityID, SlotIndex);
            return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) ?? fallback :
                fallback;
        }
    }
}
