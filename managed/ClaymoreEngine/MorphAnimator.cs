using System;
using System.Collections.Generic;

namespace ClaymoreEngine
{
    /// <summary>
    /// Easing functions for morph tweens
    /// </summary>
    public enum MorphEasing
    {
        Linear,
        EaseInQuad,
        EaseOutQuad,
        EaseInOutQuad,
        EaseInCubic,
        EaseOutCubic,
        EaseInOutCubic,
        EaseInBack,      // Slight overshoot at start
        EaseOutBack,     // Slight overshoot at end
        EaseOutElastic,  // Bouncy effect
        EaseOutBounce    // Bounce at end
    }

    /// <summary>
    /// Configuration for a single morph tween (one-shot interpolation)
    /// </summary>
    public class MorphTween
    {
        public string MorphName { get; set; }
        public int MorphIndex { get; set; } = -1;
        public float FromValue { get; set; } = 0f;
        public float ToValue { get; set; } = 1f;
        public float Duration { get; set; } = 0.3f;
        public MorphEasing Easing { get; set; } = MorphEasing.EaseOutQuad;
        public Action OnComplete { get; set; }
        
        internal float ElapsedTime { get; set; } = 0f;
        internal bool IsComplete { get; set; } = false;
        internal bool UseCurrentAsFrom { get; set; } = true; // Start from current value
    }

    /// <summary>
    /// Oscillation pattern for morph animations
    /// </summary>
    public enum MorphOscillationPattern
    {
        Sine,           // Smooth sine wave (good for breathing, subtle movements)
        Triangle,       // Linear up/down (good for basic mouth movements)
        Noise,          // Random noise (good for natural variation)
        PingPong,       // Linear with hold at peaks (good for expressive talking)
        Pulse           // Quick attack, slow decay (good for emphasis)
    }

    /// <summary>
    /// Configuration for a single morph oscillation
    /// </summary>
    public class MorphOscillation
    {
        public string MorphName { get; set; }
        public int MorphIndex { get; set; } = -1; // Cached index for performance
        public float MinValue { get; set; } = 0f;
        public float MaxValue { get; set; } = 1f;
        public float Speed { get; set; } = 1f; // Oscillations per second
        public float PhaseOffset { get; set; } = 0f; // 0-1 phase offset
        public MorphOscillationPattern Pattern { get; set; } = MorphOscillationPattern.Sine;
        public bool Enabled { get; set; } = true;
        
        internal float CurrentPhase { get; set; } = 0f;
        internal float NoiseValue { get; set; } = 0f;
        internal float NoiseTarget { get; set; } = 0f;
    }

    /// <summary>
    /// Preset configurations for common animations
    /// </summary>
    public static class MorphPresets
    {
        /// <summary>
        /// Basic talking animation - oscillates jaw/mouth open morphs
        /// </summary>
        public static MorphOscillation[] TalkingBasic(params string[] mouthMorphNames)
        {
            var oscillations = new MorphOscillation[mouthMorphNames.Length];
            for (int i = 0; i < mouthMorphNames.Length; i++)
            {
                oscillations[i] = new MorphOscillation
                {
                    MorphName = mouthMorphNames[i],
                    MinValue = 0f,
                    MaxValue = 0.7f,
                    Speed = 4f + (i * 0.5f), // Slight variation
                    Pattern = MorphOscillationPattern.Noise,
                    PhaseOffset = i * 0.25f
                };
            }
            return oscillations;
        }

        /// <summary>
        /// Expressive talking with faster variation
        /// </summary>
        public static MorphOscillation[] TalkingExpressive(string jawMorph, string lipsMorph = null)
        {
            var list = new List<MorphOscillation>
            {
                new MorphOscillation
                {
                    MorphName = jawMorph,
                    MinValue = 0f,
                    MaxValue = 0.6f,
                    Speed = 5f,
                    Pattern = MorphOscillationPattern.Noise
                }
            };

            if (!string.IsNullOrEmpty(lipsMorph))
            {
                list.Add(new MorphOscillation
                {
                    MorphName = lipsMorph,
                    MinValue = 0f,
                    MaxValue = 0.4f,
                    Speed = 7f,
                    Pattern = MorphOscillationPattern.Noise,
                    PhaseOffset = 0.3f
                });
            }

            return list.ToArray();
        }

        /// <summary>
        /// Blinking animation
        /// </summary>
        public static MorphOscillation BlinkAnimation(string blinkMorph, float blinkInterval = 3f)
        {
            return new MorphOscillation
            {
                MorphName = blinkMorph,
                MinValue = 0f,
                MaxValue = 1f,
                Speed = 1f / blinkInterval,
                Pattern = MorphOscillationPattern.Pulse
            };
        }

        /// <summary>
        /// Breathing animation (subtle chest/body movement)
        /// </summary>
        public static MorphOscillation BreathingAnimation(string breathMorph)
        {
            return new MorphOscillation
            {
                MorphName = breathMorph,
                MinValue = 0f,
                MaxValue = 0.3f,
                Speed = 0.25f, // ~4 second breath cycle
                Pattern = MorphOscillationPattern.Sine
            };
        }
    }

    /// <summary>
    /// Animates unified morph blend shapes with oscillation patterns and one-shot tweens.
    /// Useful for procedural animations like talking, breathing, blinking, and expressions.
    /// </summary>
    public class MorphAnimator
    {
        private Entity _entity;
        private UnifiedMorphComponent _morphComponent;
        private List<MorphOscillation> _oscillations = new List<MorphOscillation>();
        private List<MorphTween> _activeTweens = new List<MorphTween>();
        private bool _isPlaying = false;
        private Random _random = new Random();
        private float _globalSpeed = 1f;
        private float _intensity = 1f;

        /// <summary>
        /// Whether the animator is currently playing
        /// </summary>
        public bool IsPlaying => _isPlaying;

        /// <summary>
        /// Global speed multiplier for all oscillations
        /// </summary>
        public float GlobalSpeed
        {
            get => _globalSpeed;
            set => _globalSpeed = Math.Max(0f, value);
        }

        /// <summary>
        /// Intensity multiplier (0-1) that scales all oscillation ranges
        /// </summary>
        public float Intensity
        {
            get => _intensity;
            set => _intensity = Math.Clamp(value, 0f, 1f);
        }

        /// <summary>
        /// Create a morph animator for an entity
        /// </summary>
        public MorphAnimator(Entity entity)
        {
            _entity = entity;
            _morphComponent = entity.GetComponent<UnifiedMorphComponent>();
            if (_morphComponent == null)
            {
                Console.WriteLine($"[MorphAnimator] Warning: Entity {entity.EntityID} has no UnifiedMorphComponent");
            }
        }

        /// <summary>
        /// Add an oscillation configuration
        /// </summary>
        public MorphAnimator AddOscillation(MorphOscillation oscillation)
        {
            // Cache the morph index for better performance
            if (oscillation.MorphIndex < 0 && _morphComponent != null)
            {
                oscillation.MorphIndex = FindMorphIndex(oscillation.MorphName);
            }
            oscillation.CurrentPhase = oscillation.PhaseOffset;
            oscillation.NoiseTarget = (float)_random.NextDouble();
            _oscillations.Add(oscillation);
            return this;
        }

        /// <summary>
        /// Add multiple oscillation configurations
        /// </summary>
        public MorphAnimator AddOscillations(params MorphOscillation[] oscillations)
        {
            foreach (var osc in oscillations)
            {
                AddOscillation(osc);
            }
            return this;
        }

        /// <summary>
        /// Remove all oscillations
        /// </summary>
        public MorphAnimator ClearOscillations()
        {
            _oscillations.Clear();
            return this;
        }

        #region Tweening API

        /// <summary>
        /// Smoothly tween a morph to a target value over a duration.
        /// Starts from the current value of the morph.
        /// </summary>
        /// <param name="morphName">Name of the morph target</param>
        /// <param name="toValue">Target value (0-1 typically)</param>
        /// <param name="duration">Duration in seconds</param>
        /// <param name="easing">Easing function</param>
        /// <param name="onComplete">Optional callback when tween completes</param>
        public MorphAnimator TweenTo(string morphName, float toValue, float duration = 0.3f, 
            MorphEasing easing = MorphEasing.EaseOutQuad, Action onComplete = null)
        {
            var tween = new MorphTween
            {
                MorphName = morphName,
                ToValue = toValue,
                Duration = duration,
                Easing = easing,
                OnComplete = onComplete,
                UseCurrentAsFrom = true
            };
            
            // Cache morph index
            if (_morphComponent != null)
            {
                tween.MorphIndex = FindMorphIndex(morphName);
            }
            
            // Remove any existing tween for this morph
            _activeTweens.RemoveAll(t => t.MorphName == morphName || t.MorphIndex == tween.MorphIndex);
            
            _activeTweens.Add(tween);
            return this;
        }

        /// <summary>
        /// Smoothly tween a morph from one value to another over a duration.
        /// </summary>
        public MorphAnimator TweenFromTo(string morphName, float fromValue, float toValue, float duration = 0.3f,
            MorphEasing easing = MorphEasing.EaseOutQuad, Action onComplete = null)
        {
            var tween = new MorphTween
            {
                MorphName = morphName,
                FromValue = fromValue,
                ToValue = toValue,
                Duration = duration,
                Easing = easing,
                OnComplete = onComplete,
                UseCurrentAsFrom = false
            };
            
            // Cache morph index
            if (_morphComponent != null)
            {
                tween.MorphIndex = FindMorphIndex(morphName);
            }
            
            // Remove any existing tween for this morph
            _activeTweens.RemoveAll(t => t.MorphName == morphName || t.MorphIndex == tween.MorphIndex);
            
            _activeTweens.Add(tween);
            return this;
        }

        /// <summary>
        /// Tween a morph by index (faster, no name lookup)
        /// </summary>
        public MorphAnimator TweenToByIndex(int morphIndex, float toValue, float duration = 0.3f,
            MorphEasing easing = MorphEasing.EaseOutQuad, Action onComplete = null)
        {
            var tween = new MorphTween
            {
                MorphIndex = morphIndex,
                ToValue = toValue,
                Duration = duration,
                Easing = easing,
                OnComplete = onComplete,
                UseCurrentAsFrom = true
            };
            
            // Remove any existing tween for this morph
            _activeTweens.RemoveAll(t => t.MorphIndex == morphIndex);
            
            _activeTweens.Add(tween);
            return this;
        }

        /// <summary>
        /// Cancel all active tweens, optionally completing them instantly
        /// </summary>
        public MorphAnimator CancelTweens(bool completeInstantly = false)
        {
            if (completeInstantly && _morphComponent != null)
            {
                foreach (var tween in _activeTweens)
                {
                    if (tween.MorphIndex >= 0)
                    {
                        _morphComponent.SetUnifiedMorphByIndex(tween.MorphIndex, tween.ToValue);
                        tween.OnComplete?.Invoke();
                    }
                }
            }
            _activeTweens.Clear();
            return this;
        }

        /// <summary>
        /// Cancel tween for a specific morph
        /// </summary>
        public MorphAnimator CancelTween(string morphName, bool completeInstantly = false)
        {
            var tween = _activeTweens.Find(t => t.MorphName == morphName);
            if (tween != null)
            {
                if (completeInstantly && _morphComponent != null && tween.MorphIndex >= 0)
                {
                    _morphComponent.SetUnifiedMorphByIndex(tween.MorphIndex, tween.ToValue);
                    tween.OnComplete?.Invoke();
                }
                _activeTweens.Remove(tween);
            }
            return this;
        }

        /// <summary>
        /// Check if any tweens are currently active
        /// </summary>
        public bool HasActiveTweens => _activeTweens.Count > 0;

        #endregion

        /// <summary>
        /// Start the oscillation animation
        /// </summary>
        public void Play()
        {
            _isPlaying = true;
        }

        /// <summary>
        /// Stop the oscillation animation and optionally reset morphs to zero
        /// </summary>
        public void Stop(bool resetToZero = true)
        {
            _isPlaying = false;
            
            if (resetToZero && _morphComponent != null)
            {
                foreach (var osc in _oscillations)
                {
                    if (osc.MorphIndex >= 0)
                    {
                        _morphComponent.SetUnifiedMorphByIndex(osc.MorphIndex, 0f);
                    }
                }
            }
        }

        /// <summary>
        /// Pause without resetting morph values
        /// </summary>
        public void Pause()
        {
            _isPlaying = false;
        }

        /// <summary>
        /// Call this every frame (from OnUpdate) to animate the morphs
        /// </summary>
        public void Update(float deltaTime)
        {
            if (_morphComponent == null)
                return;

            // Process tweens (always, even when oscillations are paused)
            UpdateTweens(deltaTime);

            // Process oscillations (only when playing)
            if (_isPlaying)
            {
                float scaledDelta = deltaTime * _globalSpeed;

                foreach (var osc in _oscillations)
                {
                    if (!osc.Enabled || osc.MorphIndex < 0)
                        continue;

                    // Update phase
                    osc.CurrentPhase += scaledDelta * osc.Speed;
                    if (osc.CurrentPhase > 1f)
                        osc.CurrentPhase -= 1f;

                    // Calculate oscillation value based on pattern
                    float t = EvaluatePattern(osc);
                    
                    // Apply intensity and calculate final value
                    float range = (osc.MaxValue - osc.MinValue) * _intensity;
                    float value = osc.MinValue + t * range;

                    // Set the morph weight
                    _morphComponent.SetUnifiedMorphByIndex(osc.MorphIndex, value);
                }
            }
        }

        private void UpdateTweens(float deltaTime)
        {
            for (int i = _activeTweens.Count - 1; i >= 0; i--)
            {
                var tween = _activeTweens[i];
                
                if (tween.MorphIndex < 0)
                {
                    _activeTweens.RemoveAt(i);
                    continue;
                }

                // Initialize from current value on first frame if needed
                if (tween.ElapsedTime == 0f && tween.UseCurrentAsFrom)
                {
                    tween.FromValue = _morphComponent.GetWeight(tween.MorphIndex);
                }

                tween.ElapsedTime += deltaTime;
                float t = Math.Clamp(tween.ElapsedTime / tween.Duration, 0f, 1f);
                
                // Apply easing
                float easedT = EvaluateEasing(t, tween.Easing);
                
                // Interpolate value
                float value = Lerp(tween.FromValue, tween.ToValue, easedT);
                _morphComponent.SetUnifiedMorphByIndex(tween.MorphIndex, value);

                // Check completion
                if (t >= 1f)
                {
                    tween.IsComplete = true;
                    tween.OnComplete?.Invoke();
                    _activeTweens.RemoveAt(i);
                }
            }
        }

        private float EvaluatePattern(MorphOscillation osc)
        {
            float phase = osc.CurrentPhase;

            switch (osc.Pattern)
            {
                case MorphOscillationPattern.Sine:
                    return (float)(Math.Sin(phase * Math.PI * 2) * 0.5 + 0.5);

                case MorphOscillationPattern.Triangle:
                    return phase < 0.5f ? phase * 2f : 2f - phase * 2f;

                case MorphOscillationPattern.Noise:
                    // Smooth noise interpolation
                    if (phase < 0.1f && osc.NoiseValue != osc.NoiseTarget)
                    {
                        osc.NoiseValue = osc.NoiseTarget;
                        osc.NoiseTarget = (float)_random.NextDouble();
                    }
                    return Lerp(osc.NoiseValue, osc.NoiseTarget, phase);

                case MorphOscillationPattern.PingPong:
                    // Linear with slight hold at peaks
                    if (phase < 0.4f)
                        return phase / 0.4f;
                    else if (phase < 0.5f)
                        return 1f;
                    else if (phase < 0.9f)
                        return 1f - (phase - 0.5f) / 0.4f;
                    else
                        return 0f;

                case MorphOscillationPattern.Pulse:
                    // Quick attack, slow decay
                    if (phase < 0.1f)
                        return phase / 0.1f;
                    else
                        return 1f - (phase - 0.1f) / 0.9f;

                default:
                    return 0f;
            }
        }

        private int FindMorphIndex(string morphName)
        {
            if (_morphComponent == null || string.IsNullOrEmpty(morphName))
                return -1;

            int count = _morphComponent.Count;
            for (int i = 0; i < count; i++)
            {
                if (_morphComponent.GetName(i) == morphName)
                    return i;
            }

            Console.WriteLine($"[MorphAnimator] Warning: Morph '{morphName}' not found");
            return -1;
        }

        private static float Lerp(float a, float b, float t)
        {
            return a + (b - a) * t;
        }

        private static float EvaluateEasing(float t, MorphEasing easing)
        {
            switch (easing)
            {
                case MorphEasing.Linear:
                    return t;

                case MorphEasing.EaseInQuad:
                    return t * t;

                case MorphEasing.EaseOutQuad:
                    return 1f - (1f - t) * (1f - t);

                case MorphEasing.EaseInOutQuad:
                    return t < 0.5f ? 2f * t * t : 1f - (float)Math.Pow(-2f * t + 2f, 2) / 2f;

                case MorphEasing.EaseInCubic:
                    return t * t * t;

                case MorphEasing.EaseOutCubic:
                    return 1f - (float)Math.Pow(1f - t, 3);

                case MorphEasing.EaseInOutCubic:
                    return t < 0.5f ? 4f * t * t * t : 1f - (float)Math.Pow(-2f * t + 2f, 3) / 2f;

                case MorphEasing.EaseInBack:
                    const float c1 = 1.70158f;
                    const float c3 = c1 + 1f;
                    return c3 * t * t * t - c1 * t * t;

                case MorphEasing.EaseOutBack:
                    const float c1b = 1.70158f;
                    const float c3b = c1b + 1f;
                    return 1f + c3b * (float)Math.Pow(t - 1f, 3) + c1b * (float)Math.Pow(t - 1f, 2);

                case MorphEasing.EaseOutElastic:
                    if (t == 0f) return 0f;
                    if (t == 1f) return 1f;
                    const float c4 = (float)(2f * Math.PI) / 3f;
                    return (float)Math.Pow(2f, -10f * t) * (float)Math.Sin((t * 10f - 0.75f) * c4) + 1f;

                case MorphEasing.EaseOutBounce:
                    return EaseOutBounce(t);

                default:
                    return t;
            }
        }

        private static float EaseOutBounce(float t)
        {
            const float n1 = 7.5625f;
            const float d1 = 2.75f;

            if (t < 1f / d1)
                return n1 * t * t;
            else if (t < 2f / d1)
                return n1 * (t -= 1.5f / d1) * t + 0.75f;
            else if (t < 2.5f / d1)
                return n1 * (t -= 2.25f / d1) * t + 0.9375f;
            else
                return n1 * (t -= 2.625f / d1) * t + 0.984375f;
        }
    }

    /// <summary>
    /// Extension methods for easy MorphAnimator usage
    /// </summary>
    public static class MorphAnimatorExtensions
    {
        /// <summary>
        /// Create a MorphAnimator for this entity
        /// </summary>
        public static MorphAnimator CreateMorphAnimator(this Entity entity)
        {
            return new MorphAnimator(entity);
        }
    }

    /// <summary>
    /// Standalone tween helper for one-off morph tweens without a full MorphAnimator.
    /// Useful when you just need to tween a single morph without continuous oscillations.
    /// </summary>
    public static class MorphTweenHelper
    {
        private static List<(MorphTween tween, UnifiedMorphComponent morph)> _globalTweens = 
            new List<(MorphTween, UnifiedMorphComponent)>();

        /// <summary>
        /// Tween a morph on an entity. Call UpdateGlobalTweens() each frame.
        /// </summary>
        public static void Tween(Entity entity, string morphName, float toValue, float duration = 0.3f,
            MorphEasing easing = MorphEasing.EaseOutQuad, Action onComplete = null)
        {
            var morph = entity.GetComponent<UnifiedMorphComponent>();
            if (morph == null) return;

            int index = -1;
            for (int i = 0; i < morph.Count; i++)
            {
                if (morph.GetName(i) == morphName)
                {
                    index = i;
                    break;
                }
            }
            if (index < 0) return;

            // Remove existing tween for this morph
            _globalTweens.RemoveAll(t => t.morph == morph && t.tween.MorphIndex == index);

            var tween = new MorphTween
            {
                MorphName = morphName,
                MorphIndex = index,
                FromValue = morph.GetWeight(index),
                ToValue = toValue,
                Duration = duration,
                Easing = easing,
                OnComplete = onComplete,
                UseCurrentAsFrom = false
            };

            _globalTweens.Add((tween, morph));
        }

        /// <summary>
        /// Call this every frame to process global tweens
        /// </summary>
        public static void UpdateGlobalTweens(float deltaTime)
        {
            for (int i = _globalTweens.Count - 1; i >= 0; i--)
            {
                var (tween, morph) = _globalTweens[i];
                
                tween.ElapsedTime += deltaTime;
                float t = Math.Clamp(tween.ElapsedTime / tween.Duration, 0f, 1f);
                
                // Apply easing using reflection to access private method
                float easedT = EvaluateEasingPublic(t, tween.Easing);
                
                float value = tween.FromValue + (tween.ToValue - tween.FromValue) * easedT;
                morph.SetUnifiedMorphByIndex(tween.MorphIndex, value);

                if (t >= 1f)
                {
                    tween.OnComplete?.Invoke();
                    _globalTweens.RemoveAt(i);
                }
            }
        }

        private static float EvaluateEasingPublic(float t, MorphEasing easing)
        {
            switch (easing)
            {
                case MorphEasing.EaseOutQuad:
                    return 1f - (1f - t) * (1f - t);
                case MorphEasing.EaseInOutQuad:
                    return t < 0.5f ? 2f * t * t : 1f - (float)Math.Pow(-2f * t + 2f, 2) / 2f;
                case MorphEasing.EaseOutCubic:
                    return 1f - (float)Math.Pow(1f - t, 3);
                default:
                    return t;
            }
        }
    }
}

