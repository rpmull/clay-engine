using System;
using System.Numerics;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    /// <summary>
    /// Delegate for initializing particle interop function pointers from native code.
    /// </summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public unsafe delegate void ParticleInteropInitDelegate(IntPtr* functionPointers, int count);

    #region Delegate Signatures

    // Core
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate bool Particle_GetEnabledFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_SetEnabledFn(int entityId, bool enabled);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_PlayFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_StopFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_RestartFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate bool Particle_IsPlayingFn(int entityId);

    // Space & Shape
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int Particle_GetSimulationSpaceFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_SetSimulationSpaceFn(int entityId, int space);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int Particle_GetShapeFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_SetShapeFn(int entityId, int shape);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate float Particle_GetShapeRadiusFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_SetShapeRadiusFn(int entityId, float radius);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate float Particle_GetShapeAngleFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_SetShapeAngleFn(int entityId, float angle);

    // Start Values
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_GetStartSpeedFn(int entityId, out float min, out float max);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_SetStartSpeedFn(int entityId, float min, float max);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_GetStartSizeFn(int entityId, out float min, out float max);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_SetStartSizeFn(int entityId, float min, float max);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_GetStartColorFn(int entityId, out float r, out float g, out float b, out float a);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_SetStartColorFn(int entityId, float r, float g, float b, float a);

    // Emission
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate float Particle_GetEmissionRateFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_SetEmissionRateFn(int entityId, float rate);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate bool Particle_GetLoopingFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_SetLoopingFn(int entityId, bool looping);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate float Particle_GetDurationFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_SetDurationFn(int entityId, float duration);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_GetLifetimeFn(int entityId, out float min, out float max);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_SetLifetimeFn(int entityId, float min, float max);

    // Physics
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate float Particle_GetGravityModifierFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_SetGravityModifierFn(int entityId, float gravity);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int Particle_GetMaxParticlesFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_SetMaxParticlesFn(int entityId, int maxParticles);

    // Module Enables
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate bool Particle_GetSizeOverLifetimeEnabledFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_SetSizeOverLifetimeEnabledFn(int entityId, bool enabled);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate bool Particle_GetColorOverLifetimeEnabledFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_SetColorOverLifetimeEnabledFn(int entityId, bool enabled);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate bool Particle_GetVelocityOverLifetimeEnabledFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_SetVelocityOverLifetimeEnabledFn(int entityId, bool enabled);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate bool Particle_GetRotationOverLifetimeEnabledFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_SetRotationOverLifetimeEnabledFn(int entityId, bool enabled);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate bool Particle_GetAlignWithTrajectoryFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_SetAlignWithTrajectoryFn(int entityId, bool enabled);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate bool Particle_GetBurstEnabledFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_SetBurstEnabledFn(int entityId, bool enabled);

    // Size Over Lifetime
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_GetSizeOverLifetimeFn(int entityId, out float start, out float end, out int curveType);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_SetSizeOverLifetimeFn(int entityId, float start, float end, int curveType);

    // Color Gradient
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int Particle_GetColorGradientKeyCountFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_GetColorGradientKeyFn(int entityId, int index, out float time, out float r, out float g, out float b, out float a);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_SetColorGradientKeyFn(int entityId, int index, float time, float r, float g, float b, float a);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_ClearColorGradientFn(int entityId);

    // Burst
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int Particle_GetBurstCountFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Particle_SetBurstCountFn(int entityId, int count);

    #endregion

    /// <summary>
    /// Static interop class for particle emitter operations.
    /// </summary>
    public static class ParticleInterop
    {
        // Core
        public static Particle_GetEnabledFn? GetEnabled;
        public static Particle_SetEnabledFn? SetEnabled;
        public static Particle_PlayFn? Play;
        public static Particle_StopFn? Stop;
        public static Particle_RestartFn? Restart;
        public static Particle_IsPlayingFn? IsPlaying;

        // Space & Shape
        public static Particle_GetSimulationSpaceFn? GetSimulationSpace;
        public static Particle_SetSimulationSpaceFn? SetSimulationSpace;
        public static Particle_GetShapeFn? GetShape;
        public static Particle_SetShapeFn? SetShape;
        public static Particle_GetShapeRadiusFn? GetShapeRadius;
        public static Particle_SetShapeRadiusFn? SetShapeRadius;
        public static Particle_GetShapeAngleFn? GetShapeAngle;
        public static Particle_SetShapeAngleFn? SetShapeAngle;

        // Start Values
        public static Particle_GetStartSpeedFn? GetStartSpeed;
        public static Particle_SetStartSpeedFn? SetStartSpeed;
        public static Particle_GetStartSizeFn? GetStartSize;
        public static Particle_SetStartSizeFn? SetStartSize;
        public static Particle_GetStartColorFn? GetStartColor;
        public static Particle_SetStartColorFn? SetStartColor;

        // Emission
        public static Particle_GetEmissionRateFn? GetEmissionRate;
        public static Particle_SetEmissionRateFn? SetEmissionRate;
        public static Particle_GetLoopingFn? GetLooping;
        public static Particle_SetLoopingFn? SetLooping;
        public static Particle_GetDurationFn? GetDuration;
        public static Particle_SetDurationFn? SetDuration;
        public static Particle_GetLifetimeFn? GetLifetime;
        public static Particle_SetLifetimeFn? SetLifetime;

        // Physics
        public static Particle_GetGravityModifierFn? GetGravityModifier;
        public static Particle_SetGravityModifierFn? SetGravityModifier;
        public static Particle_GetMaxParticlesFn? GetMaxParticles;
        public static Particle_SetMaxParticlesFn? SetMaxParticles;

        // Module Enables
        public static Particle_GetSizeOverLifetimeEnabledFn? GetSizeOverLifetimeEnabled;
        public static Particle_SetSizeOverLifetimeEnabledFn? SetSizeOverLifetimeEnabled;
        public static Particle_GetColorOverLifetimeEnabledFn? GetColorOverLifetimeEnabled;
        public static Particle_SetColorOverLifetimeEnabledFn? SetColorOverLifetimeEnabled;
        public static Particle_GetVelocityOverLifetimeEnabledFn? GetVelocityOverLifetimeEnabled;
        public static Particle_SetVelocityOverLifetimeEnabledFn? SetVelocityOverLifetimeEnabled;
        public static Particle_GetRotationOverLifetimeEnabledFn? GetRotationOverLifetimeEnabled;
        public static Particle_SetRotationOverLifetimeEnabledFn? SetRotationOverLifetimeEnabled;
        public static Particle_GetAlignWithTrajectoryFn? GetAlignWithTrajectory;
        public static Particle_SetAlignWithTrajectoryFn? SetAlignWithTrajectory;
        public static Particle_GetBurstEnabledFn? GetBurstEnabled;
        public static Particle_SetBurstEnabledFn? SetBurstEnabled;

        // Size Over Lifetime
        public static Particle_GetSizeOverLifetimeFn? GetSizeOverLifetime;
        public static Particle_SetSizeOverLifetimeFn? SetSizeOverLifetime;

        // Color Gradient
        public static Particle_GetColorGradientKeyCountFn? GetColorGradientKeyCount;
        public static Particle_GetColorGradientKeyFn? GetColorGradientKey;
        public static Particle_SetColorGradientKeyFn? SetColorGradientKey;
        public static Particle_ClearColorGradientFn? ClearColorGradient;

        // Burst
        public static Particle_GetBurstCountFn? GetBurstCount;
        public static Particle_SetBurstCountFn? SetBurstCount;

        /// <summary>
        /// Number of function pointers expected for particle interop.
        /// MUST match the native side count.
        /// </summary>
        public const int PARTICLE_INTEROP_COUNT = 52;

        public static unsafe void Initialize(IntPtr* ptrs, int count)
        {
            if (count < PARTICLE_INTEROP_COUNT)
            {
                Console.WriteLine($"[ParticleInterop] Warning: expected {PARTICLE_INTEROP_COUNT} pointers, got {count}");
                return;
            }

            int i = 0;

            // Core (6 functions)
            GetEnabled = Marshal.GetDelegateForFunctionPointer<Particle_GetEnabledFn>(ptrs[i++]);
            SetEnabled = Marshal.GetDelegateForFunctionPointer<Particle_SetEnabledFn>(ptrs[i++]);
            Play = Marshal.GetDelegateForFunctionPointer<Particle_PlayFn>(ptrs[i++]);
            Stop = Marshal.GetDelegateForFunctionPointer<Particle_StopFn>(ptrs[i++]);
            Restart = Marshal.GetDelegateForFunctionPointer<Particle_RestartFn>(ptrs[i++]);
            IsPlaying = Marshal.GetDelegateForFunctionPointer<Particle_IsPlayingFn>(ptrs[i++]);

            // Space & Shape (8 functions)
            GetSimulationSpace = Marshal.GetDelegateForFunctionPointer<Particle_GetSimulationSpaceFn>(ptrs[i++]);
            SetSimulationSpace = Marshal.GetDelegateForFunctionPointer<Particle_SetSimulationSpaceFn>(ptrs[i++]);
            GetShape = Marshal.GetDelegateForFunctionPointer<Particle_GetShapeFn>(ptrs[i++]);
            SetShape = Marshal.GetDelegateForFunctionPointer<Particle_SetShapeFn>(ptrs[i++]);
            GetShapeRadius = Marshal.GetDelegateForFunctionPointer<Particle_GetShapeRadiusFn>(ptrs[i++]);
            SetShapeRadius = Marshal.GetDelegateForFunctionPointer<Particle_SetShapeRadiusFn>(ptrs[i++]);
            GetShapeAngle = Marshal.GetDelegateForFunctionPointer<Particle_GetShapeAngleFn>(ptrs[i++]);
            SetShapeAngle = Marshal.GetDelegateForFunctionPointer<Particle_SetShapeAngleFn>(ptrs[i++]);

            // Start Values (6 functions)
            GetStartSpeed = Marshal.GetDelegateForFunctionPointer<Particle_GetStartSpeedFn>(ptrs[i++]);
            SetStartSpeed = Marshal.GetDelegateForFunctionPointer<Particle_SetStartSpeedFn>(ptrs[i++]);
            GetStartSize = Marshal.GetDelegateForFunctionPointer<Particle_GetStartSizeFn>(ptrs[i++]);
            SetStartSize = Marshal.GetDelegateForFunctionPointer<Particle_SetStartSizeFn>(ptrs[i++]);
            GetStartColor = Marshal.GetDelegateForFunctionPointer<Particle_GetStartColorFn>(ptrs[i++]);
            SetStartColor = Marshal.GetDelegateForFunctionPointer<Particle_SetStartColorFn>(ptrs[i++]);

            // Emission (8 functions)
            GetEmissionRate = Marshal.GetDelegateForFunctionPointer<Particle_GetEmissionRateFn>(ptrs[i++]);
            SetEmissionRate = Marshal.GetDelegateForFunctionPointer<Particle_SetEmissionRateFn>(ptrs[i++]);
            GetLooping = Marshal.GetDelegateForFunctionPointer<Particle_GetLoopingFn>(ptrs[i++]);
            SetLooping = Marshal.GetDelegateForFunctionPointer<Particle_SetLoopingFn>(ptrs[i++]);
            GetDuration = Marshal.GetDelegateForFunctionPointer<Particle_GetDurationFn>(ptrs[i++]);
            SetDuration = Marshal.GetDelegateForFunctionPointer<Particle_SetDurationFn>(ptrs[i++]);
            GetLifetime = Marshal.GetDelegateForFunctionPointer<Particle_GetLifetimeFn>(ptrs[i++]);
            SetLifetime = Marshal.GetDelegateForFunctionPointer<Particle_SetLifetimeFn>(ptrs[i++]);

            // Physics (4 functions)
            GetGravityModifier = Marshal.GetDelegateForFunctionPointer<Particle_GetGravityModifierFn>(ptrs[i++]);
            SetGravityModifier = Marshal.GetDelegateForFunctionPointer<Particle_SetGravityModifierFn>(ptrs[i++]);
            GetMaxParticles = Marshal.GetDelegateForFunctionPointer<Particle_GetMaxParticlesFn>(ptrs[i++]);
            SetMaxParticles = Marshal.GetDelegateForFunctionPointer<Particle_SetMaxParticlesFn>(ptrs[i++]);

            // Module Enables (12 functions)
            GetSizeOverLifetimeEnabled = Marshal.GetDelegateForFunctionPointer<Particle_GetSizeOverLifetimeEnabledFn>(ptrs[i++]);
            SetSizeOverLifetimeEnabled = Marshal.GetDelegateForFunctionPointer<Particle_SetSizeOverLifetimeEnabledFn>(ptrs[i++]);
            GetColorOverLifetimeEnabled = Marshal.GetDelegateForFunctionPointer<Particle_GetColorOverLifetimeEnabledFn>(ptrs[i++]);
            SetColorOverLifetimeEnabled = Marshal.GetDelegateForFunctionPointer<Particle_SetColorOverLifetimeEnabledFn>(ptrs[i++]);
            GetVelocityOverLifetimeEnabled = Marshal.GetDelegateForFunctionPointer<Particle_GetVelocityOverLifetimeEnabledFn>(ptrs[i++]);
            SetVelocityOverLifetimeEnabled = Marshal.GetDelegateForFunctionPointer<Particle_SetVelocityOverLifetimeEnabledFn>(ptrs[i++]);
            GetRotationOverLifetimeEnabled = Marshal.GetDelegateForFunctionPointer<Particle_GetRotationOverLifetimeEnabledFn>(ptrs[i++]);
            SetRotationOverLifetimeEnabled = Marshal.GetDelegateForFunctionPointer<Particle_SetRotationOverLifetimeEnabledFn>(ptrs[i++]);
            GetAlignWithTrajectory = Marshal.GetDelegateForFunctionPointer<Particle_GetAlignWithTrajectoryFn>(ptrs[i++]);
            SetAlignWithTrajectory = Marshal.GetDelegateForFunctionPointer<Particle_SetAlignWithTrajectoryFn>(ptrs[i++]);
            GetBurstEnabled = Marshal.GetDelegateForFunctionPointer<Particle_GetBurstEnabledFn>(ptrs[i++]);
            SetBurstEnabled = Marshal.GetDelegateForFunctionPointer<Particle_SetBurstEnabledFn>(ptrs[i++]);

            // Size Over Lifetime (2 functions)
            GetSizeOverLifetime = Marshal.GetDelegateForFunctionPointer<Particle_GetSizeOverLifetimeFn>(ptrs[i++]);
            SetSizeOverLifetime = Marshal.GetDelegateForFunctionPointer<Particle_SetSizeOverLifetimeFn>(ptrs[i++]);

            // Color Gradient (4 functions)
            GetColorGradientKeyCount = Marshal.GetDelegateForFunctionPointer<Particle_GetColorGradientKeyCountFn>(ptrs[i++]);
            GetColorGradientKey = Marshal.GetDelegateForFunctionPointer<Particle_GetColorGradientKeyFn>(ptrs[i++]);
            SetColorGradientKey = Marshal.GetDelegateForFunctionPointer<Particle_SetColorGradientKeyFn>(ptrs[i++]);
            ClearColorGradient = Marshal.GetDelegateForFunctionPointer<Particle_ClearColorGradientFn>(ptrs[i++]);

            // Burst (2 functions)
            GetBurstCount = Marshal.GetDelegateForFunctionPointer<Particle_GetBurstCountFn>(ptrs[i++]);
            SetBurstCount = Marshal.GetDelegateForFunctionPointer<Particle_SetBurstCountFn>(ptrs[i++]);

            Console.WriteLine($"[ParticleInterop] Initialized with {i} functions");
        }
    }

    /// <summary>
    /// Particle simulation space enum (matches native ParticleSimulationSpace).
    /// </summary>
    public enum ParticleSimulationSpace
    {
        Local = 0,
        World = 1
    }

    /// <summary>
    /// Particle emission shape enum (matches native ParticleEmissionShape).
    /// </summary>
    public enum ParticleEmissionShape
    {
        Point = 0,
        Sphere,
        Hemisphere,
        Cone,
        Box,
        Circle,
        Disc,
        Edge,
        Rectangle
    }

    /// <summary>
    /// Particle curve type for over-lifetime effects.
    /// </summary>
    public enum ParticleCurveType
    {
        Constant = 0,
        Linear,
        EaseIn,
        EaseOut,
        EaseInOut,
        Custom
    }

    /// <summary>
    /// Managed component for controlling particle emitters on entities.
    /// </summary>
    public sealed class ParticleEmitter : ComponentBase
    {
        #region Core

        /// <summary>
        /// Whether the particle emitter is enabled.
        /// </summary>
        public bool Enabled
        {
            get => ParticleInterop.GetEnabled?.Invoke(Entity.EntityID) ?? false;
            set => ParticleInterop.SetEnabled?.Invoke(Entity.EntityID, value);
        }

        /// <summary>
        /// Start/resume particle emission.
        /// </summary>
        public void Play() => ParticleInterop.Play?.Invoke(Entity.EntityID);

        /// <summary>
        /// Stop particle emission (particles already spawned continue).
        /// </summary>
        public void Stop() => ParticleInterop.Stop?.Invoke(Entity.EntityID);

        /// <summary>
        /// Restart emission from the beginning.
        /// </summary>
        public void Restart() => ParticleInterop.Restart?.Invoke(Entity.EntityID);

        /// <summary>
        /// Whether the emitter is currently playing.
        /// </summary>
        public bool IsPlaying => ParticleInterop.IsPlaying?.Invoke(Entity.EntityID) ?? false;

        #endregion

        #region Space & Shape

        /// <summary>
        /// Simulation space (Local = follow emitter, World = stay in world space).
        /// </summary>
        public ParticleSimulationSpace SimulationSpace
        {
            get => (ParticleSimulationSpace)(ParticleInterop.GetSimulationSpace?.Invoke(Entity.EntityID) ?? 1);
            set => ParticleInterop.SetSimulationSpace?.Invoke(Entity.EntityID, (int)value);
        }

        /// <summary>
        /// Emission shape type.
        /// </summary>
        public ParticleEmissionShape Shape
        {
            get => (ParticleEmissionShape)(ParticleInterop.GetShape?.Invoke(Entity.EntityID) ?? 3);
            set => ParticleInterop.SetShape?.Invoke(Entity.EntityID, (int)value);
        }

        /// <summary>
        /// Shape radius (for sphere, cone, circle, disc shapes).
        /// </summary>
        public float ShapeRadius
        {
            get => ParticleInterop.GetShapeRadius?.Invoke(Entity.EntityID) ?? 1f;
            set => ParticleInterop.SetShapeRadius?.Invoke(Entity.EntityID, value);
        }

        /// <summary>
        /// Shape angle in degrees (for cone shape).
        /// </summary>
        public float ShapeAngle
        {
            get => ParticleInterop.GetShapeAngle?.Invoke(Entity.EntityID) ?? 25f;
            set => ParticleInterop.SetShapeAngle?.Invoke(Entity.EntityID, value);
        }

        #endregion

        #region Start Values

        /// <summary>
        /// Minimum start speed.
        /// </summary>
        public float StartSpeedMin
        {
            get
            {
                if (ParticleInterop.GetStartSpeed == null) return 2f;
                ParticleInterop.GetStartSpeed(Entity.EntityID, out float min, out _);
                return min;
            }
            set
            {
                if (ParticleInterop.GetStartSpeed == null || ParticleInterop.SetStartSpeed == null) return;
                ParticleInterop.GetStartSpeed(Entity.EntityID, out _, out float max);
                ParticleInterop.SetStartSpeed(Entity.EntityID, value, max);
            }
        }

        /// <summary>
        /// Maximum start speed.
        /// </summary>
        public float StartSpeedMax
        {
            get
            {
                if (ParticleInterop.GetStartSpeed == null) return 5f;
                ParticleInterop.GetStartSpeed(Entity.EntityID, out _, out float max);
                return max;
            }
            set
            {
                if (ParticleInterop.GetStartSpeed == null || ParticleInterop.SetStartSpeed == null) return;
                ParticleInterop.GetStartSpeed(Entity.EntityID, out float min, out _);
                ParticleInterop.SetStartSpeed(Entity.EntityID, min, value);
            }
        }

        /// <summary>
        /// Set start speed range.
        /// </summary>
        public void SetStartSpeed(float min, float max)
        {
            ParticleInterop.SetStartSpeed?.Invoke(Entity.EntityID, min, max);
        }

        /// <summary>
        /// Minimum start size.
        /// </summary>
        public float StartSizeMin
        {
            get
            {
                if (ParticleInterop.GetStartSize == null) return 0.1f;
                ParticleInterop.GetStartSize(Entity.EntityID, out float min, out _);
                return min;
            }
            set
            {
                if (ParticleInterop.GetStartSize == null || ParticleInterop.SetStartSize == null) return;
                ParticleInterop.GetStartSize(Entity.EntityID, out _, out float max);
                ParticleInterop.SetStartSize(Entity.EntityID, value, max);
            }
        }

        /// <summary>
        /// Maximum start size.
        /// </summary>
        public float StartSizeMax
        {
            get
            {
                if (ParticleInterop.GetStartSize == null) return 0.3f;
                ParticleInterop.GetStartSize(Entity.EntityID, out _, out float max);
                return max;
            }
            set
            {
                if (ParticleInterop.GetStartSize == null || ParticleInterop.SetStartSize == null) return;
                ParticleInterop.GetStartSize(Entity.EntityID, out float min, out _);
                ParticleInterop.SetStartSize(Entity.EntityID, min, value);
            }
        }

        /// <summary>
        /// Set start size range.
        /// </summary>
        public void SetStartSize(float min, float max)
        {
            ParticleInterop.SetStartSize?.Invoke(Entity.EntityID, min, max);
        }

        /// <summary>
        /// Start color (RGBA).
        /// </summary>
        public Vector4 StartColor
        {
            get
            {
                if (ParticleInterop.GetStartColor == null) return Vector4.One;
                ParticleInterop.GetStartColor(Entity.EntityID, out float r, out float g, out float b, out float a);
                return new Vector4(r, g, b, a);
            }
            set
            {
                var normalized = NormalizeColorInput(value);
                ParticleInterop.SetStartColor?.Invoke(Entity.EntityID, normalized.X, normalized.Y, normalized.Z, normalized.W);
            }
        }

        #endregion

        private static Vector4 NormalizeColorInput(Vector4 value)
        {
            bool anyOverOne = value.X > 1f || value.Y > 1f || value.Z > 1f || value.W > 1f;
            if (!anyOverOne) return value;
            if (value.X > 255f || value.Y > 255f || value.Z > 255f || value.W > 255f) return value;
            if (!IsWhole(value.X) || !IsWhole(value.Y) || !IsWhole(value.Z) || !IsWhole(value.W)) return value;
            return value / 255f;
        }

        private static bool IsWhole(float value)
        {
            return MathF.Abs(value - MathF.Round(value)) < 0.0001f;
        }

        #region Emission

        /// <summary>
        /// Particles emitted per second.
        /// </summary>
        public float EmissionRate
        {
            get => ParticleInterop.GetEmissionRate?.Invoke(Entity.EntityID) ?? 100f;
            set => ParticleInterop.SetEmissionRate?.Invoke(Entity.EntityID, value);
        }

        /// <summary>
        /// Whether emission loops.
        /// </summary>
        public bool Looping
        {
            get => ParticleInterop.GetLooping?.Invoke(Entity.EntityID) ?? true;
            set => ParticleInterop.SetLooping?.Invoke(Entity.EntityID, value);
        }

        /// <summary>
        /// Duration of emission in seconds.
        /// </summary>
        public float Duration
        {
            get => ParticleInterop.GetDuration?.Invoke(Entity.EntityID) ?? 5f;
            set => ParticleInterop.SetDuration?.Invoke(Entity.EntityID, value);
        }

        /// <summary>
        /// Minimum particle lifetime.
        /// </summary>
        public float LifetimeMin
        {
            get
            {
                if (ParticleInterop.GetLifetime == null) return 3f;
                ParticleInterop.GetLifetime(Entity.EntityID, out float min, out _);
                return min;
            }
            set
            {
                if (ParticleInterop.GetLifetime == null || ParticleInterop.SetLifetime == null) return;
                ParticleInterop.GetLifetime(Entity.EntityID, out _, out float max);
                ParticleInterop.SetLifetime(Entity.EntityID, value, max);
            }
        }

        /// <summary>
        /// Maximum particle lifetime.
        /// </summary>
        public float LifetimeMax
        {
            get
            {
                if (ParticleInterop.GetLifetime == null) return 5f;
                ParticleInterop.GetLifetime(Entity.EntityID, out _, out float max);
                return max;
            }
            set
            {
                if (ParticleInterop.GetLifetime == null || ParticleInterop.SetLifetime == null) return;
                ParticleInterop.GetLifetime(Entity.EntityID, out float min, out _);
                ParticleInterop.SetLifetime(Entity.EntityID, min, value);
            }
        }

        /// <summary>
        /// Set particle lifetime range.
        /// </summary>
        public void SetLifetime(float min, float max)
        {
            ParticleInterop.SetLifetime?.Invoke(Entity.EntityID, min, max);
        }

        #endregion

        #region Physics

        /// <summary>
        /// Gravity multiplier applied to particles.
        /// </summary>
        public float GravityModifier
        {
            get => ParticleInterop.GetGravityModifier?.Invoke(Entity.EntityID) ?? 0f;
            set => ParticleInterop.SetGravityModifier?.Invoke(Entity.EntityID, value);
        }

        /// <summary>
        /// Maximum number of particles.
        /// </summary>
        public int MaxParticles
        {
            get => ParticleInterop.GetMaxParticles?.Invoke(Entity.EntityID) ?? 1024;
            set => ParticleInterop.SetMaxParticles?.Invoke(Entity.EntityID, value);
        }

        #endregion

        #region Module Enables

        /// <summary>
        /// Enable size over lifetime effect.
        /// </summary>
        public bool SizeOverLifetimeEnabled
        {
            get => ParticleInterop.GetSizeOverLifetimeEnabled?.Invoke(Entity.EntityID) ?? true;
            set => ParticleInterop.SetSizeOverLifetimeEnabled?.Invoke(Entity.EntityID, value);
        }

        /// <summary>
        /// Enable color over lifetime effect.
        /// </summary>
        public bool ColorOverLifetimeEnabled
        {
            get => ParticleInterop.GetColorOverLifetimeEnabled?.Invoke(Entity.EntityID) ?? true;
            set => ParticleInterop.SetColorOverLifetimeEnabled?.Invoke(Entity.EntityID, value);
        }

        /// <summary>
        /// Enable velocity over lifetime effect.
        /// </summary>
        public bool VelocityOverLifetimeEnabled
        {
            get => ParticleInterop.GetVelocityOverLifetimeEnabled?.Invoke(Entity.EntityID) ?? false;
            set => ParticleInterop.SetVelocityOverLifetimeEnabled?.Invoke(Entity.EntityID, value);
        }

        /// <summary>
        /// Enable rotation over lifetime effect.
        /// </summary>
        public bool RotationOverLifetimeEnabled
        {
            get => ParticleInterop.GetRotationOverLifetimeEnabled?.Invoke(Entity.EntityID) ?? false;
            set => ParticleInterop.SetRotationOverLifetimeEnabled?.Invoke(Entity.EntityID, value);
        }

        /// <summary>
        /// Rotate particles to face their direction of travel.
        /// </summary>
        public bool AlignWithTrajectory
        {
            get => ParticleInterop.GetAlignWithTrajectory?.Invoke(Entity.EntityID) ?? false;
            set => ParticleInterop.SetAlignWithTrajectory?.Invoke(Entity.EntityID, value);
        }

        /// <summary>
        /// Enable burst emission.
        /// </summary>
        public bool BurstEnabled
        {
            get => ParticleInterop.GetBurstEnabled?.Invoke(Entity.EntityID) ?? false;
            set => ParticleInterop.SetBurstEnabled?.Invoke(Entity.EntityID, value);
        }

        #endregion

        #region Size Over Lifetime

        /// <summary>
        /// Size multiplier at start of lifetime.
        /// </summary>
        public float SizeOverLifetimeStart
        {
            get
            {
                if (ParticleInterop.GetSizeOverLifetime == null) return 1f;
                ParticleInterop.GetSizeOverLifetime(Entity.EntityID, out float start, out _, out _);
                return start;
            }
            set
            {
                if (ParticleInterop.GetSizeOverLifetime == null || ParticleInterop.SetSizeOverLifetime == null) return;
                ParticleInterop.GetSizeOverLifetime(Entity.EntityID, out _, out float end, out int curve);
                ParticleInterop.SetSizeOverLifetime(Entity.EntityID, value, end, curve);
            }
        }

        /// <summary>
        /// Size multiplier at end of lifetime.
        /// </summary>
        public float SizeOverLifetimeEnd
        {
            get
            {
                if (ParticleInterop.GetSizeOverLifetime == null) return 0f;
                ParticleInterop.GetSizeOverLifetime(Entity.EntityID, out _, out float end, out _);
                return end;
            }
            set
            {
                if (ParticleInterop.GetSizeOverLifetime == null || ParticleInterop.SetSizeOverLifetime == null) return;
                ParticleInterop.GetSizeOverLifetime(Entity.EntityID, out float start, out _, out int curve);
                ParticleInterop.SetSizeOverLifetime(Entity.EntityID, start, value, curve);
            }
        }

        /// <summary>
        /// Curve type for size over lifetime interpolation.
        /// </summary>
        public ParticleCurveType SizeOverLifetimeCurve
        {
            get
            {
                if (ParticleInterop.GetSizeOverLifetime == null) return ParticleCurveType.Linear;
                ParticleInterop.GetSizeOverLifetime(Entity.EntityID, out _, out _, out int curve);
                return (ParticleCurveType)curve;
            }
            set
            {
                if (ParticleInterop.GetSizeOverLifetime == null || ParticleInterop.SetSizeOverLifetime == null) return;
                ParticleInterop.GetSizeOverLifetime(Entity.EntityID, out float start, out float end, out _);
                ParticleInterop.SetSizeOverLifetime(Entity.EntityID, start, end, (int)value);
            }
        }

        /// <summary>
        /// Set size over lifetime parameters.
        /// </summary>
        public void SetSizeOverLifetime(float start, float end, ParticleCurveType curve = ParticleCurveType.Linear)
        {
            ParticleInterop.SetSizeOverLifetime?.Invoke(Entity.EntityID, start, end, (int)curve);
        }

        #endregion

        #region Color Gradient

        /// <summary>
        /// Number of keys in the color gradient.
        /// </summary>
        public int ColorGradientKeyCount => ParticleInterop.GetColorGradientKeyCount?.Invoke(Entity.EntityID) ?? 0;

        /// <summary>
        /// Get a color gradient key by index.
        /// </summary>
        public (float time, Vector4 color) GetColorGradientKey(int index)
        {
            if (ParticleInterop.GetColorGradientKey == null) return (0f, Vector4.One);
            ParticleInterop.GetColorGradientKey(Entity.EntityID, index, out float time, out float r, out float g, out float b, out float a);
            return (time, new Vector4(r, g, b, a));
        }

        /// <summary>
        /// Set a color gradient key (expands gradient if index is at end).
        /// </summary>
        public void SetColorGradientKey(int index, float time, Vector4 color)
        {
            ParticleInterop.SetColorGradientKey?.Invoke(Entity.EntityID, index, time, color.X, color.Y, color.Z, color.W);
        }

        /// <summary>
        /// Clear the color gradient and reset to default.
        /// </summary>
        public void ClearColorGradient()
        {
            ParticleInterop.ClearColorGradient?.Invoke(Entity.EntityID);
        }

        #endregion

        #region Burst

        /// <summary>
        /// Number of particles emitted per burst.
        /// </summary>
        public int BurstCount
        {
            get => ParticleInterop.GetBurstCount?.Invoke(Entity.EntityID) ?? 10;
            set => ParticleInterop.SetBurstCount?.Invoke(Entity.EntityID, value);
        }

        #endregion
    }
}

