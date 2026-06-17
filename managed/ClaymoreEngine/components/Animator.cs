using System;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    /// <summary>
    /// Preset body masks for animation layering.
    /// These masks define which bones are affected by an animation layer.
    /// </summary>
    public enum BodyMask
    {
        /// <summary>All bones in the skeleton</summary>
        FullBody = 0,
        /// <summary>Spine, chest, arms, head, and neck</summary>
        UpperBody = 1,
        /// <summary>Hips and legs</summary>
        LowerBody = 2,
        /// <summary>Left shoulder through left hand and fingers</summary>
        LeftArm = 3,
        /// <summary>Right shoulder through right hand and fingers</summary>
        RightArm = 4,
        /// <summary>Neck, head, and eyes</summary>
        Head = 5,
        /// <summary>Spine, chest, and upper chest only</summary>
        Spine = 6,
        /// <summary>Left hand and fingers only</summary>
        LeftHand = 7,
        /// <summary>Right hand and fingers only</summary>
        RightHand = 8,
        /// <summary>Both arms (left and right)</summary>
        Arms = 9,
        /// <summary>Both legs (without hips)</summary>
        Legs = 10
    }

    /// <summary>
    /// How an animation layer blends with underlying layers.
    /// </summary>
    public enum AnimationBlendMode
    {
        /// <summary>Replace underlying animation (lerp based on weight)</summary>
        Override = 0,
        /// <summary>Add rotation/position deltas on top of underlying animation</summary>
        Additive = 1
    }

    /// <summary>
    /// Effective root motion handling for the currently evaluated base-layer clip or blend.
    /// Matches the native animation asset/runtime mode values.
    /// </summary>
    public enum RootMotionMode
    {
        None = 0,
        InPlace = 1,
        ApplyToEntity = 2
    }

    /// <summary>
    /// Represents an animation layer defined in the AnimatorController.
    /// Layers are authored in the editor - each layer has its own state machine.
    /// At runtime, you can control layer weights to enable/disable them.
    /// Use AnimatorController.GetLayer() to obtain an instance.
    /// </summary>
    public sealed class AnimationLayer
    {
        private readonly int _entityId;
        private readonly string _layerName;

        internal AnimationLayer(int entityId, string name)
        {
            _entityId = entityId;
            _layerName = name;
        }

        /// <summary>Layer name (as defined in the controller)</summary>
        public string Name => _layerName;

        /// <summary>
        /// Get the animation path of the current state in this layer.
        /// Animation states and transitions are defined in the controller - not settable at runtime.
        /// </summary>
        public string GetCurrentAnimation()
        {
            return ComponentInterop.AnimLayer_GetAnimation(_entityId, _layerName);
        }

        /// <summary>
        /// Get the body mask for this layer (as defined in the controller).
        /// </summary>
        public BodyMask GetMask()
        {
            return (BodyMask)(ComponentInterop.AnimLayer_GetMask?.Invoke(_entityId, _layerName) ?? 0);
        }

        /// <summary>
        /// Get the blend mode (as defined in the controller).
        /// </summary>
        public AnimationBlendMode GetBlendMode()
        {
            return (AnimationBlendMode)(ComponentInterop.AnimLayer_GetBlendMode?.Invoke(_entityId, _layerName) ?? 0);
        }

        /// <summary>
        /// Set the blend weight directly (0 = layer inactive, 1 = full override).
        /// This is the primary way to enable/disable layers at runtime.
        /// </summary>
        public AnimationLayer SetWeight(float weight)
        {
            ComponentInterop.AnimLayer_SetWeight?.Invoke(_entityId, _layerName, weight);
            return this;
        }

        /// <summary>
        /// Get the current blend weight.
        /// </summary>
        public float Weight
        {
            get => ComponentInterop.AnimLayer_GetWeight?.Invoke(_entityId, _layerName) ?? 0f;
            set => SetWeight(value);
        }

        /// <summary>
        /// Smoothly blend to a target weight over time.
        /// Use this to smoothly enable/disable layers.
        /// </summary>
        /// <param name="targetWeight">Target weight (0 = off, 1 = fully active)</param>
        /// <param name="blendSpeed">Weight change per second (default: 8)</param>
        public AnimationLayer BlendTo(float targetWeight, float blendSpeed = 8f)
        {
            ComponentInterop.AnimLayer_BlendTo?.Invoke(_entityId, _layerName, targetWeight, blendSpeed);
            return this;
        }

        /// <summary>
        /// Enable this layer by setting weight to 1.
        /// The layer's state machine will begin animating the masked bones.
        /// </summary>
        public AnimationLayer Enable()
        {
            ComponentInterop.AnimLayer_Play?.Invoke(_entityId, _layerName, true);
            return this;
        }

        /// <summary>
        /// Disable this layer by blending weight to 0.
        /// </summary>
        public AnimationLayer Disable()
        {
            ComponentInterop.AnimLayer_Stop?.Invoke(_entityId, _layerName);
            return this;
        }

        /// <summary>
        /// Check if this layer is currently active (weight > 0).
        /// </summary>
        public bool IsActive => ComponentInterop.AnimLayer_IsPlaying?.Invoke(_entityId, _layerName) ?? false;

        /// <summary>
        /// Get the playback speed of the current state (as defined in the controller).
        /// </summary>
        public float GetSpeed()
        {
            return ComponentInterop.AnimLayer_GetSpeed?.Invoke(_entityId, _layerName) ?? 1f;
        }

        /// <summary>
        /// Set current playback time within the layer's current animation.
        /// </summary>
        public AnimationLayer SetTime(float time)
        {
            ComponentInterop.AnimLayer_SetTime?.Invoke(_entityId, _layerName, time);
            return this;
        }

        /// <summary>
        /// Force this layer to an authored state by name and set its normalized playback time.
        /// </summary>
        public AnimationLayer SetState(string stateName, float normalizedTime = 0f, bool satisfyTransitionConditions = true)
        {
            if (string.IsNullOrEmpty(stateName)) return this;
            ComponentInterop.AnimLayer_SetStateByName?.Invoke(_entityId, _layerName, stateName, normalizedTime, satisfyTransitionConditions);
            return this;
        }

        /// <summary>
        /// Set current Blend2D parameters for this layer's active Blend2D state.
        /// </summary>
        public AnimationLayer SetBlend2D(float x, float y, bool clampToUnitRange = true)
        {
            ComponentInterop.AnimLayer_SetBlend2D?.Invoke(_entityId, _layerName, x, y, clampToUnitRange);
            return this;
        }

        /// <summary>
        /// Get current playback time within the layer's current animation.
        /// </summary>
        public float Time
        {
            get => ComponentInterop.AnimLayer_GetTime?.Invoke(_entityId, _layerName) ?? 0f;
            set => SetTime(value);
        }

        /// <summary>
        /// Get the duration of the current state's animation.
        /// </summary>
        public float Duration => ComponentInterop.AnimLayer_GetDuration?.Invoke(_entityId, _layerName) ?? 0f;

        /// <summary>
        /// Get whether the current state loops (as defined in the controller).
        /// </summary>
        public bool IsLooping => ComponentInterop.AnimLayer_GetLooping?.Invoke(_entityId, _layerName) ?? true;
    }

    /// <summary>
    /// Handle to a named state in the animator controller. Obtained via
    /// <see cref="AnimatorController.GetState(string)"/>. Lets scripts hook methods into
    /// state transitions:
    /// <code>
    /// var state = animator.GetController().GetState("Attack");
    /// state.OnStateEntered += () => { /* ... */ };
    /// state.OnStateExited  += () => { /* ... */ };
    /// </code>
    /// Callbacks fire on actual committed transitions (base or overlay layers), in both
    /// editor play mode and exported runtime. Handlers route through
    /// <see cref="AnimationStateInterop"/>; subscribing has no per-frame cost.
    /// </summary>
    public sealed class AnimatorState
    {
        private readonly int _entityId;
        private readonly string _stateName;

        internal AnimatorState(int entityId, string stateName)
        {
            _entityId = entityId;
            _stateName = stateName;
        }

        /// <summary>The authored state name this handle refers to.</summary>
        public string Name => _stateName;

        /// <summary>Raised when this state becomes the active state in its layer.</summary>
        public event Action OnStateEntered
        {
            add { AnimationStateInterop.AddEnterHandler(_entityId, _stateName, value); }
            remove { AnimationStateInterop.RemoveEnterHandler(_entityId, _stateName, value); }
        }

        /// <summary>Raised when this state stops being the active state in its layer.</summary>
        public event Action OnStateExited
        {
            add { AnimationStateInterop.AddExitHandler(_entityId, _stateName, value); }
            remove { AnimationStateInterop.RemoveExitHandler(_entityId, _stateName, value); }
        }

        /// <summary>True if this is the controller's current base-layer state.</summary>
        public bool IsActive =>
            _entityId > 0 && string.Equals(ComponentInterop.Animator_GetState(_entityId), _stateName, StringComparison.Ordinal);

        /// <summary>The base-layer state that was active immediately before the current one.</summary>
        public string PreviousState =>
            _entityId > 0 ? ComponentInterop.Animator_GetPreviousState(_entityId) : string.Empty;
    }

    // Thin managed wrapper over native Animator/AnimationPlayer component
    public sealed class Animator : ComponentBase
    {
        /// <summary>
        /// Gets or sets whether animation evaluation is enabled.
        /// Set to false when ragdoll takes over bone transforms.
        /// When disabled, animation system skips this entity but bone transforms
        /// are still uploaded for skinning (from ragdoll or last animated pose).
        /// </summary>
        public bool Enabled
        {
            get => entity.IsValid && ComponentInterop.Animator_GetEnabled != null 
                ? ComponentInterop.Animator_GetEnabled(entity.EntityID) 
                : false;
            set
            {
                if (!entity.IsValid || ComponentInterop.Animator_SetEnabled == null) return;
                ComponentInterop.Animator_SetEnabled(entity.EntityID, value);
            }
        }

        /// <summary>
        /// Gets the animator controller for this entity. 
        /// Always returns a controller object - check IsValid property to verify it's usable.
        /// Methods on an invalid controller will safely no-op.
        /// </summary>
        public AnimatorController GetController()
        {
            // Always return a controller - it handles invalid entity IDs gracefully
            return new AnimatorController(entity.IsValid ? entity.EntityID : -1);
        }

        /// <summary>
        /// Switch the active animator controller to the provided asset reference.
        /// </summary>
        /// <param name="controller">Animator controller asset reference</param>
        /// <param name="blendSeconds">Blend duration in seconds for a smooth switch</param>
        public void SetController(AnimationController controller, float blendSeconds = 0.15f)
        {
            if (!entity.IsValid || ComponentInterop.Animator_SetController == null) return;
            ComponentInterop.Animator_SetController(entity.EntityID, controller.Path ?? string.Empty, blendSeconds);
        }

        /// <summary>
        /// Switch the active animator controller by path.
        /// </summary>
        /// <param name="controllerPath">VFS-relative path to .animctrl</param>
        /// <param name="blendSeconds">Blend duration in seconds for a smooth switch</param>
        public void SetController(string controllerPath, float blendSeconds = 0.15f)
        {
            if (!entity.IsValid || ComponentInterop.Animator_SetController == null) return;
            ComponentInterop.Animator_SetController(entity.EntityID, controllerPath ?? string.Empty, blendSeconds);
        }

        /// <summary>
        /// Gets the effective root motion mode resolved for the current base-layer clip or blend.
        /// Updated during animation evaluation each frame.
        /// </summary>
        public RootMotionMode CurrentClipRootMotionMode
        {
            get => entity.IsValid && ComponentInterop.Animator_GetCurrentClipRootMotionMode != null
                ? (RootMotionMode)ComponentInterop.Animator_GetCurrentClipRootMotionMode(entity.EntityID)
                : RootMotionMode.None;
        }

        /// <summary>
        /// Returns true when the current clip/blend preserves authored root motion in some form.
        /// </summary>
        public bool CurrentClipHasRootMotion => CurrentClipRootMotionMode != RootMotionMode.None;

        /// <summary>
        /// Returns true when the current clip/blend is actively driving entity or physics motion.
        /// </summary>
        public bool CurrentClipAppliesRootMotion => CurrentClipRootMotionMode == RootMotionMode.ApplyToEntity;
    }

    public sealed class AnimatorController
    {
        private readonly int _entityId;
        internal AnimatorController(int entityId) { _entityId = entityId; }
        
        /// <summary>
        /// Returns true if this controller is attached to a valid entity.
        /// </summary>
        public bool IsValid => _entityId > 0;

        /// <summary>
        /// Gets the effective root motion mode resolved for the current base-layer clip or blend.
        /// Updated during animation evaluation each frame.
        /// </summary>
        public RootMotionMode CurrentClipRootMotionMode
        {
            get => IsValid && ComponentInterop.Animator_GetCurrentClipRootMotionMode != null
                ? (RootMotionMode)ComponentInterop.Animator_GetCurrentClipRootMotionMode(_entityId)
                : RootMotionMode.None;
        }

        /// <summary>
        /// Returns true when the current clip/blend preserves authored root motion in some form.
        /// </summary>
        public bool CurrentClipHasRootMotion => CurrentClipRootMotionMode != RootMotionMode.None;

        /// <summary>
        /// Returns true when the current clip/blend is actively driving entity or physics motion.
        /// </summary>
        public bool CurrentClipAppliesRootMotion => CurrentClipRootMotionMode == RootMotionMode.ApplyToEntity;

        /// <summary>
        /// Apply an animation controller override asset to the currently assigned controller.
        /// Matching clips are sampled from the override asset while states, parameters, and transitions stay on the base controller.
        /// </summary>
        public void SetOverride(AnimationControllerOverride overrideAsset)
        {
            if (!IsValid || ComponentInterop.Animator_SetOverride == null) return;
            ComponentInterop.Animator_SetOverride(_entityId, overrideAsset.Path ?? string.Empty);
        }

        /// <summary>
        /// Apply an animation controller override asset by path.
        /// </summary>
        public void SetOverride(string overridePath)
        {
            if (!IsValid || ComponentInterop.Animator_SetOverride == null) return;
            ComponentInterop.Animator_SetOverride(_entityId, overridePath ?? string.Empty);
        }

        /// <summary>
        /// Clear the active animation controller override.
        /// </summary>
        public void ClearOverride()
        {
            if (!IsValid || ComponentInterop.Animator_SetOverride == null) return;
            ComponentInterop.Animator_SetOverride(_entityId, string.Empty);
        }

        public void SetBool(string name, bool value)
        {
            if (!IsValid)
            {
                ManagedLogger.LogWarning($"AnimatorController.SetBool('{name}'): Controller is not valid (entityId={_entityId})");
                return;
            }
            if (ComponentInterop.Animator_SetBool == null)
            {
                ManagedLogger.LogError("AnimatorController.SetBool: delegate is null - interop not initialized");
                return;
            }
            ComponentInterop.Animator_SetBool(_entityId, name, value);
        }
        
        public void SetInt(string name, int value)
        {
            if (!IsValid) return;
            if (ComponentInterop.Animator_SetInt == null) return;
            ComponentInterop.Animator_SetInt(_entityId, name, value);
        }
        
        public void SetFloat(string name, float value)
        {
            if (!IsValid) return;
            if (ComponentInterop.Animator_SetFloat == null) return;
            ComponentInterop.Animator_SetFloat(_entityId, name, value);
        }
        
        public void SetTrigger(string name)
        {
            if (!IsValid) return;
            if (ComponentInterop.Animator_SetTrigger == null) return;
            ComponentInterop.Animator_SetTrigger(_entityId, name);
        }
        
        public void ResetTrigger(string name)
        {
            if (!IsValid) return;
            if (ComponentInterop.Animator_ResetTrigger == null) return;
            ComponentInterop.Animator_ResetTrigger(_entityId, name);
        }

        public bool GetBool(string name)
        {
            if (!IsValid || ComponentInterop.Animator_GetBool == null) return false;
            return ComponentInterop.Animator_GetBool(_entityId, name);
        }
        
        public int GetInt(string name)
        {
            if (!IsValid || ComponentInterop.Animator_GetInt == null) return 0;
            return ComponentInterop.Animator_GetInt(_entityId, name);
        }
        
        public float GetFloat(string name)
        {
            if (!IsValid || ComponentInterop.Animator_GetFloat == null) return 0f;
            return ComponentInterop.Animator_GetFloat(_entityId, name);
        }
        
        public bool GetTrigger(string name)
        {
            if (!IsValid || ComponentInterop.Animator_GetTrigger == null) return false;
            return ComponentInterop.Animator_GetTrigger(_entityId, name);
        }

        /// <summary>
        /// Gets the name of the current state in the animator controller.
        /// Returns an empty string if the entity is invalid or no state is active.
        /// </summary>
        public string GetState()
        {
            if (!IsValid) return string.Empty;
            return ComponentInterop.Animator_GetState(_entityId);
        }

        /// <summary>
        /// Get a handle to a named state so you can hook into its transitions, e.g.
        /// <c>GetState("Attack").OnStateEntered += () => {...}</c>. The handle is a thin
        /// value object; subscribing/unsubscribing is what registers the callback.
        /// Returns null if this controller is not attached to a valid entity.
        /// </summary>
        /// <param name="stateName">The authored state name (as shown in the controller).</param>
        public AnimatorState GetState(string stateName)
        {
            if (!IsValid || string.IsNullOrEmpty(stateName)) return null;
            return new AnimatorState(_entityId, stateName);
        }

        /// <summary>
        /// Get a handle to the controller's current base-layer state. Equivalent to
        /// <c>GetState(GetState())</c> but returns a usable <see cref="AnimatorState"/>
        /// handle so you can hook its transitions, e.g.
        /// <c>GetCurrentState().OnStateExited += () => {...}</c>.
        /// Returns null if the controller is invalid or has no active state.
        /// </summary>
        public AnimatorState GetCurrentState()
        {
            if (!IsValid) return null;
            string name = ComponentInterop.Animator_GetState(_entityId);
            if (string.IsNullOrEmpty(name)) return null;
            return new AnimatorState(_entityId, name);
        }

        /// <summary>
        /// Compute the state this controller will transition into next, based on the
        /// current base-layer state and the current parameter/trigger values, and return
        /// a handle to it. This mirrors the runtime's transition selection exactly, so it
        /// behaves identically in editor play mode and exported runtime.
        /// <para>
        /// If no transition currently qualifies (the state loops or simply has no pending
        /// exit), this returns a handle to the <em>current</em> state (self). If a
        /// crossfade is already in flight, it returns the transition's target state.
        /// </para>
        /// Because the query is non-destructive it does not consume triggers, so it is
        /// safe to call right after <see cref="SetTrigger(string)"/> to discover the state
        /// the trigger will lead into:
        /// <code>
        /// controller.SetTrigger(obj.Data.AnimTrigger);
        /// controller.GetNextState().OnStateExited += () => { obj.OnInteract(self); };
        /// </code>
        /// Returns null if the controller is invalid or has no resolvable state.
        /// </summary>
        public AnimatorState GetNextState()
        {
            if (!IsValid) return null;
            string name = ComponentInterop.Animator_GetNextState(_entityId);
            if (string.IsNullOrEmpty(name)) return null;
            return new AnimatorState(_entityId, name);
        }

        /// <summary>
        /// The base-layer state that was active immediately before the current state.
        /// Returns an empty string if there is no prior state yet.
        /// </summary>
        public string PreviousState
        {
            get
            {
                if (!IsValid) return string.Empty;
                return ComponentInterop.Animator_GetPreviousState(_entityId);
            }
        }

        /// <summary>
        /// Register a callback to be invoked when an animation event with the specified name fires.
        /// The event name corresponds to the "Script Class" field in the animation event editor.
        /// </summary>
        /// <param name="eventName">The event name to listen for</param>
        /// <param name="callback">The callback to invoke when the event fires</param>
        /// <example>
        /// // In your script's OnCreate:
        /// var animator = entity.GetComponent&lt;Animator&gt;();
        /// var controller = animator.GetController();
        /// controller.SetEventResponse("FootstepEvent", OnFootstep);
        /// 
        /// private void OnFootstep(string eventName, string className, string methodName, string payloadJson)
        /// {
        ///     // Play footstep sound
        /// }
        /// </example>
        public void SetEventResponse(string eventName, AnimationEventHandler callback)
        {
            if (!IsValid)
            {
                ManagedLogger.LogWarning($"AnimatorController.SetEventResponse('{eventName}'): Controller is not valid (entityId={_entityId})");
                return;
            }
            AnimationEventInterop.AddEventHandler(_entityId, eventName, callback);
        }
        
        /// <summary>
        /// Register a simple callback (Action) to be invoked when an animation event fires.
        /// This is a convenience overload that ignores the event parameters.
        /// </summary>
        /// <param name="eventName">The event name to listen for</param>
        /// <param name="callback">The callback to invoke when the event fires</param>
        public void SetEventResponse(string eventName, Action callback)
        {
            if (!IsValid)
            {
                ManagedLogger.LogWarning($"AnimatorController.SetEventResponse('{eventName}'): Controller is not valid (entityId={_entityId})");
                return;
            }
            AnimationEventInterop.AddEventHandler(_entityId, eventName, (_, _, _, _) => callback());
        }
        
        /// <summary>
        /// Remove a previously registered event callback.
        /// </summary>
        /// <param name="eventName">The event name</param>
        /// <param name="callback">The callback to remove</param>
        public void ClearEventResponse(string eventName, AnimationEventHandler callback)
        {
            if (!IsValid) return;
            AnimationEventInterop.RemoveEventHandler(_entityId, eventName, callback);
        }
        
        /// <summary>
        /// Remove all registered callbacks for a specific event.
        /// </summary>
        /// <param name="eventName">The event name</param>
        public void ClearEventResponse(string eventName)
        {
            if (!IsValid) return;
            AnimationEventInterop.ClearEventHandlers(_entityId, eventName);
        }

        // =========================================================================
        // Animation Layer Management
        // =========================================================================
        // Layers are defined in the AnimatorController asset, not created at runtime.
        // Each layer has its own state machine with states and transitions.
        // Use SetLayerWeight() to enable/disable layers.
        // =========================================================================

        /// <summary>
        /// Get an animation layer by name.
        /// Layers are defined in the AnimatorController - use SetLayerWeight to enable/disable them.
        /// </summary>
        /// <param name="name">Layer name as defined in the controller (e.g., "RightArm", "UpperBody")</param>
        /// <returns>The animation layer, or null if not found or entity is invalid</returns>
        /// <example>
        /// // Enable the right arm layer when sword is drawn
        /// var rightArmLayer = controller.GetLayer("RightArm");
        /// if (rightArmLayer != null)
        /// {
        ///     rightArmLayer.SetWeight(1.0f);  // Enable immediately
        ///     // Or blend smoothly:
        ///     rightArmLayer.BlendTo(1.0f, blendSpeed: 5f);
        /// }
        ///     
        /// // Later, when sword is sheathed:
        /// rightArmLayer?.BlendTo(0f, blendSpeed: 5f);
        /// </example>
        public AnimationLayer GetLayer(string name)
        {
            if (!IsValid) return null;
            int layerIndex = ComponentInterop.AnimLayer_GetOrCreate?.Invoke(_entityId, name, -1) ?? -1;
            if (layerIndex < 0) return null;
            return new AnimationLayer(_entityId, name);
        }

        /// <summary>
        /// Check if a layer with the given name exists in the controller.
        /// </summary>
        public bool HasLayer(string name)
        {
            if (!IsValid) return false;
            return ComponentInterop.AnimLayer_Has?.Invoke(_entityId, name) ?? false;
        }

        /// <summary>
        /// Get the number of layers defined in the controller.
        /// Layer 0 is always the base layer.
        /// </summary>
        public int LayerCount => IsValid ? (ComponentInterop.AnimLayer_GetCount?.Invoke(_entityId) ?? 0) : 0;

        /// <summary>
        /// Get a layer by its index in the controller.
        /// </summary>
        /// <param name="index">Layer index (0 = base layer)</param>
        public AnimationLayer GetLayerByIndex(int index)
        {
            if (!IsValid) return null;
            string name = ComponentInterop.AnimLayer_GetNameByIndex(_entityId, index);
            if (string.IsNullOrEmpty(name)) return null;
            return new AnimationLayer(_entityId, name);
        }

        /// <summary>
        /// Set the weight of a layer by name.
        /// This is a convenience method - equivalent to GetLayer(name)?.SetWeight(weight).
        /// </summary>
        /// <param name="layerName">Layer name</param>
        /// <param name="weight">Weight (0 = inactive, 1 = full)</param>
        public void SetLayerWeight(string layerName, float weight)
        {
            if (!IsValid) return;
            ComponentInterop.AnimLayer_SetWeight?.Invoke(_entityId, layerName, weight);
        }

        /// <summary>
        /// Get the weight of a layer by name.
        /// </summary>
        /// <param name="layerName">Layer name</param>
        /// <returns>Current weight (0-1), or 0 if layer not found</returns>
        public float GetLayerWeight(string layerName)
        {
            if (!IsValid) return 0f;
            return ComponentInterop.AnimLayer_GetWeight?.Invoke(_entityId, layerName) ?? 0f;
        }

        /// <summary>
        /// Smoothly blend a layer's weight to a target value.
        /// </summary>
        /// <param name="layerName">Layer name</param>
        /// <param name="targetWeight">Target weight (0-1)</param>
        /// <param name="blendSpeed">Weight change per second</param>
        public void BlendLayerWeight(string layerName, float targetWeight, float blendSpeed = 8f)
        {
            if (!IsValid) return;
            ComponentInterop.AnimLayer_BlendTo?.Invoke(_entityId, layerName, targetWeight, blendSpeed);
        }

        /// <summary>
        /// Force a specific controller state on a layer and optionally jump to a normalized time.
        /// Useful when switching controllers to preserve semantic pose (e.g. already-drawn weapon idle).
        /// </summary>
        /// <param name="layerName">Layer name in the controller (e.g. "Layer 1")</param>
        /// <param name="stateId">Target state id in that layer</param>
        /// <param name="normalizedTime">0..1 percentage into the state's evaluation time</param>
        public void SetLayerState(string layerName, int stateId, float normalizedTime = 0f)
        {
            if (!IsValid) return;
            ComponentInterop.AnimLayer_SetState?.Invoke(_entityId, layerName, stateId, normalizedTime);
        }

        /// <summary>
        /// Force a controller state by authored state name, with optional transition-condition simulation.
        /// When satisfyTransitionConditions is true, this method applies the incoming transition's condition
        /// values to animator parameters before forcing the state, which is useful for scene bootstrapping.
        /// </summary>
        public void SetLayerState(string layerName, string stateName, float normalizedTime = 0f, bool satisfyTransitionConditions = true)
        {
            if (!IsValid || string.IsNullOrEmpty(stateName)) return;
            ComponentInterop.AnimLayer_SetStateByName?.Invoke(_entityId, layerName, stateName, normalizedTime, satisfyTransitionConditions);
        }

        /// <summary>
        /// Set Blend2D X/Y coordinates for a specific layer's active Blend2D state.
        /// This writes directly to the state's configured Blend2D parameter names.
        /// </summary>
        public void SetLayerBlend2D(string layerName, float x, float y, bool clampToUnitRange = true)
        {
            if (!IsValid) return;
            ComponentInterop.AnimLayer_SetBlend2D?.Invoke(_entityId, layerName, x, y, clampToUnitRange);
        }

        /// <summary>
        /// Jump current layer state playback to a normalized time in [0,1].
        /// </summary>
        public void SetLayerNormalizedTime(string layerName, float normalizedTime)
        {
            if (!IsValid) return;
            ComponentInterop.AnimLayer_SetNormalizedTime?.Invoke(_entityId, layerName, normalizedTime);
        }
    }
}



