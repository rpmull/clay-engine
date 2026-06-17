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

        // x=tint blend mode (0-8), y=mask threshold. Material-level like ColorTint.
        public Vector4 TintParams
        {
            get => GetVector4(MaterialProperties.TintParams,
                defaultValue: new Vector4(0f, 0.5f, 0f, 0f));
            set => SetVector4(MaterialProperties.TintParams, value);
        }

        /// <summary>Tint mask blend mode 0-8 (u_TintParams.x), Blender-style blend modes.</summary>
        public float TintBlendMode
        {
            get => TintParams.X;
            set => TintParams = TintParams with { X = value };
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

        // UV transform (xy = scale, zw = offset). Routed through the per-entity
        // property block rather than the PBR material slot: the property-block path
        // does not clone/canonicalize the material, so per-frame updates (e.g. UV
        // scrolling) are cheap and don't spawn runaway "_Clone" materials. The
        // override is applied after global shader properties and reaches PSX and PBR
        // shaders alike (both read u_UVTransform). Default is identity (1,1,0,0).
        public Vector4 UVTransform
        {
            get => GetEffectiveVector4(MaterialProperties.UVTransform, new Vector4(1f, 1f, 0f, 0f));
            set => SetOverrideVector4(MaterialProperties.UVTransform, value);
        }

        // Convenience helpers over UVTransform (xy = scale, zw = offset). These
        // work for PSX as well as PBR materials.
        public Vector2 UVScale
        {
            get { var t = UVTransform; return new Vector2(t.X, t.Y); }
            set { var t = UVTransform; UVTransform = new Vector4(value.X, value.Y, t.Z, t.W); }
        }

        public Vector2 UVOffset
        {
            get { var t = UVTransform; return new Vector2(t.Z, t.W); }
            set { var t = UVTransform; UVTransform = new Vector4(t.X, t.Y, value.X, value.Y); }
        }

        // PSX shader parameters. These are commonly set scene-wide in
        // Environment > Global Shader Properties, which binds them as GLOBAL
        // uniforms that override per-material values each draw. To make a
        // per-entity override actually take effect, these write to the per-entity
        // property block, which is applied after globals and therefore wins.
        // The getter reads the effective value (per-entity override first, then
        // the material's own value, then the default) so read-modify-write of a
        // single component preserves the others.
        // x=jitter(px), y=affine[0..1], z=lightInfluence[0..1], w=normalPerturb.
        public Vector4 PsxParams
        {
            get => GetEffectiveVector4(MaterialProperties.PSXParams, Vector4.Zero);
            set => SetOverrideVector4(MaterialProperties.PSXParams, value);
        }

        // x=vertexWorldAmp(m), y=tileSize(m).
        public Vector4 PsxWorld
        {
            get => GetEffectiveVector4(MaterialProperties.PSXWorld, new Vector4(0f, 0.25f, 0f, 0f));
            set => SetOverrideVector4(MaterialProperties.PSXWorld, value);
        }

        // x=bands(>=1), y=softness[0..1].
        public Vector4 ToonParams
        {
            get => GetEffectiveVector4(MaterialProperties.ToonParams, new Vector4(3f, 1f, 0f, 0f));
            set => SetOverrideVector4(MaterialProperties.ToonParams, value);
        }

        // x=levels (0 = off).
        public Vector4 Posterize
        {
            get => GetEffectiveVector4(MaterialProperties.Posterize, Vector4.Zero);
            set => SetOverrideVector4(MaterialProperties.Posterize, value);
        }

        // x=enable (0/1), y=strength [0..1] for unlit PSX shadow receive.
        public Vector4 PsxShadowParams
        {
            get => GetEffectiveVector4(MaterialProperties.PSXShadowParams, new Vector4(0f, 1f, 0f, 0f));
            set => SetOverrideVector4(MaterialProperties.PSXShadowParams, value);
        }

        // xyz=emission color, w=emission strength (0 = off).
        public Vector4 PsxEmission
        {
            get => GetEffectiveVector4(MaterialProperties.PSXEmission, new Vector4(1f, 1f, 1f, 0f));
            set => SetOverrideVector4(MaterialProperties.PSXEmission, value);
        }

        // ====================================================================
        // Named PSX parameters
        //
        // Readable accessors for the individual components of the PSX vec4
        // uniforms. Prefer these over manually rebuilding Vector4 values
        // ("vector masking"):
        //
        //   material.PsxJitter = 2f;            // instead of:
        //   // material.PsxParams = new Vector4(2f, p.Y, p.Z, p.W);
        //
        // Each setter reads the current effective value (override > material >
        // default), changes only its component, and writes a per-entity
        // override, so the other components are always preserved.
        // ====================================================================

        /// <summary>Vertex jitter amplitude in pixels (u_psxParams.x). 0 disables.</summary>
        public float PsxJitter
        {
            get => PsxParams.X;
            set => PsxParams = PsxParams with { X = value };
        }

        /// <summary>Affine texture warp factor 0..1 (u_psxParams.y).</summary>
        public float PsxAffineWarp
        {
            get => PsxParams.Y;
            set => PsxParams = PsxParams with { Y = value };
        }

        /// <summary>Lighting influence 0 (unlit) .. 1 (fully lit) (u_psxParams.z).</summary>
        public float PsxLightInfluence
        {
            get => PsxParams.Z;
            set => PsxParams = PsxParams with { Z = value };
        }

        /// <summary>Normal perturbation amplitude for PSX shimmer (u_psxParams.w).</summary>
        public float PsxNormalPerturb
        {
            get => PsxParams.W;
            set => PsxParams = PsxParams with { W = value };
        }

        /// <summary>World-space vertex snap amplitude in meters (u_psxWorld.x). 0 disables.</summary>
        public float PsxVertexWorldAmplitude
        {
            get => PsxWorld.X;
            set => PsxWorld = PsxWorld with { X = value };
        }

        /// <summary>World-space vertex snap tile size in meters (u_psxWorld.y).</summary>
        public float PsxVertexTileSize
        {
            get => PsxWorld.Y;
            set => PsxWorld = PsxWorld with { Y = value };
        }

        /// <summary>Toon shading band count, >= 1 (u_toonParams.x).</summary>
        public float ToonBands
        {
            get => ToonParams.X;
            set => ToonParams = ToonParams with { X = value };
        }

        /// <summary>Toon band softness 0 (hard bands) .. 1 (smooth) (u_toonParams.y).</summary>
        public float ToonSoftness
        {
            get => ToonParams.Y;
            set => ToonParams = ToonParams with { Y = value };
        }

        /// <summary>Albedo posterization levels (u_posterize.x). 0 disables.</summary>
        public float PosterizeLevels
        {
            get => Posterize.X;
            set => Posterize = Posterize with { X = value };
        }

        /// <summary>Whether the unlit PSX surface receives directional shadows (u_psxShadowParams.x).</summary>
        public bool PsxShadowsEnabled
        {
            get => PsxShadowParams.X > 0.5f;
            set => PsxShadowParams = PsxShadowParams with { X = value ? 1f : 0f };
        }

        /// <summary>Strength of PSX shadow receive 0..1 (u_psxShadowParams.y).</summary>
        public float PsxShadowStrength
        {
            get => PsxShadowParams.Y;
            set => PsxShadowParams = PsxShadowParams with { Y = value };
        }

        /// <summary>Emission color for PSX materials (u_psxEmission.xyz).</summary>
        public Vector3 PsxEmissionColor
        {
            get { var e = PsxEmission; return new Vector3(e.X, e.Y, e.Z); }
            set { var e = PsxEmission; PsxEmission = new Vector4(value.X, value.Y, value.Z, e.W); }
        }

        /// <summary>Emission strength for PSX materials (u_psxEmission.w). 0 = off.</summary>
        public float PsxEmissionStrength
        {
            get => PsxEmission.W;
            set => PsxEmission = PsxEmission with { W = value };
        }

        // Writes a per-entity property-block override (applied after global shader
        // properties, so it wins). Use for uniforms that may also be set globally.
        private void SetOverrideVector4(string propertyName, Vector4 value)
        {
            MaterialInterop.SetVector4Slot?.Invoke(entity.EntityID, SlotIndex,
                propertyName, value.X, value.Y, value.Z, value.W);
        }

        // Reads the effective value: per-entity property-block override if present,
        // otherwise the material's own uniform, otherwise the supplied default.
        private Vector4 GetEffectiveVector4(string propertyName, Vector4 defaultValue)
        {
            var getSlot = MaterialInterop.GetVector4Slot;
            if (getSlot != null &&
                MaterialInterop.HasPropertySlot?.Invoke(entity.EntityID, SlotIndex,
                    propertyName) == true)
            {
                getSlot(entity.EntityID, SlotIndex, propertyName,
                    out float x, out float y, out float z, out float w);
                return new Vector4(x, y, z, w);
            }
            var getMat = MaterialInterop.GetMaterialVector4Slot;
            if (getMat != null && HasProperty(propertyName))
            {
                getMat(entity.EntityID, SlotIndex, propertyName,
                    out float x, out float y, out float z, out float w);
                return new Vector4(x, y, z, w);
            }
            return defaultValue;
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
