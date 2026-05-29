using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    /// <summary>
    /// Delegate signature for animation event callbacks.
    /// </summary>
    /// <param name="eventName">The name of the event that was fired</param>
    /// <param name="className">The script class name from the animation event</param>
    /// <param name="methodName">The method name from the animation event</param>
    /// <param name="payloadJson">JSON string containing the event payload data</param>
    public delegate void AnimationEventHandler(string eventName, string className, string methodName, string payloadJson);
    
    /// <summary>
    /// Delegate type for the Initialize method (used by native interop bootstrapping).
    /// </summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public unsafe delegate void AnimationEventInteropInitDelegate(IntPtr* functionPointers, int count);
    
    /// <summary>
    /// Native callback signature for animation events from C++.
    /// </summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void NativeAnimationEventCallback(
        int entityId, 
        [MarshalAs(UnmanagedType.LPStr)] string eventName,
        [MarshalAs(UnmanagedType.LPStr)] string className,
        [MarshalAs(UnmanagedType.LPStr)] string methodName,
        [MarshalAs(UnmanagedType.LPStr)] string payloadJson);
    
    /// <summary>
    /// Native function signatures for registering/clearing event callbacks.
    /// </summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void SetAnimationEventCallbackFn(NativeAnimationEventCallback callback);
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void Animator_SetEventCallbackFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string eventName, IntPtr callbackHandle);
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void Animator_ClearEventCallbackFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string eventName);
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void Animator_ClearAllEventCallbacksFn(int entityId);
    
    /// <summary>
    /// Interop bridge for animation events between native C++ and managed C# code.
    /// Allows scripts to register callbacks for specific animation events by name.
    /// </summary>
    public static unsafe class AnimationEventInterop
    {
        // Native function pointers
        internal static SetAnimationEventCallbackFn? SetAnimationEventCallback;
        internal static Animator_SetEventCallbackFn? Animator_SetEventCallback;
        internal static Animator_ClearEventCallbackFn? Animator_ClearEventCallback;
        internal static Animator_ClearAllEventCallbacksFn? Animator_ClearAllEventCallbacks;
        
        // Keep the callback delegate alive to prevent GC collection
        private static NativeAnimationEventCallback? s_NativeCallback;
        
        // Registry of event handlers per entity per event name
        // Key: entityId, Value: Dictionary<eventName, List<handlers>>
        private static readonly Dictionary<int, Dictionary<string, List<AnimationEventHandler>>> s_EventHandlers = new();
        
        // Lock for thread-safe access to handlers
        private static readonly object s_Lock = new();
        
        /// <summary>
        /// Initialize the interop system with function pointers from native code.
        /// </summary>
        public static void Initialize(IntPtr* ptrs, int count)
        {
            Console.WriteLine($"[AnimationEventInterop] Initialize called with count={count}");
            
            if (count < 4)
            {
                Console.WriteLine($"[AnimationEventInterop] Expected at least 4 function pointers, got {count}");
                return;
            }
            
            int i = 0;
            SetAnimationEventCallback = Marshal.GetDelegateForFunctionPointer<SetAnimationEventCallbackFn>(ptrs[i++]);
            Animator_SetEventCallback = Marshal.GetDelegateForFunctionPointer<Animator_SetEventCallbackFn>(ptrs[i++]);
            Animator_ClearEventCallback = Marshal.GetDelegateForFunctionPointer<Animator_ClearEventCallbackFn>(ptrs[i++]);
            Animator_ClearAllEventCallbacks = Marshal.GetDelegateForFunctionPointer<Animator_ClearAllEventCallbacksFn>(ptrs[i++]);
            
            // Create and register our callback with native code
            s_NativeCallback = OnAnimationEvent;
            SetAnimationEventCallback?.Invoke(s_NativeCallback);
            
            Console.WriteLine("[AnimationEventInterop] Initialize complete");
        }
        
        /// <summary>
        /// Called by native code when an animation event fires.
        /// Dispatches to all registered handlers for that entity and event name.
        /// </summary>
        private static void OnAnimationEvent(int entityId, string eventName, string className, string methodName, string payloadJson)
        {
            List<AnimationEventHandler>? handlers = null;
            
            lock (s_Lock)
            {
                if (s_EventHandlers.TryGetValue(entityId, out var entityHandlers))
                {
                    if (entityHandlers.TryGetValue(eventName, out var eventHandlers))
                    {
                        // Copy the list to avoid holding the lock during invocation
                        handlers = new List<AnimationEventHandler>(eventHandlers);
                    }
                }
            }
            
            if (handlers != null)
            {
                foreach (var handler in handlers)
                {
                    try
                    {
                        handler(eventName, className, methodName, payloadJson);
                    }
                    catch (Exception ex)
                    {
                        Console.WriteLine($"[AnimationEventInterop] Handler exception: {ex}");
                    }
                }
            }
        }
        
        /// <summary>
        /// Register a handler for a specific animation event on an entity's animator.
        /// </summary>
        /// <param name="entityId">The entity ID that has the animator component</param>
        /// <param name="eventName">The event name to listen for (matches className in animation event)</param>
        /// <param name="handler">The callback to invoke when the event fires</param>
        public static void AddEventHandler(int entityId, string eventName, AnimationEventHandler handler)
        {
            if (string.IsNullOrEmpty(eventName) || handler == null) return;
            
            lock (s_Lock)
            {
                if (!s_EventHandlers.TryGetValue(entityId, out var entityHandlers))
                {
                    entityHandlers = new Dictionary<string, List<AnimationEventHandler>>();
                    s_EventHandlers[entityId] = entityHandlers;
                }
                
                if (!entityHandlers.TryGetValue(eventName, out var eventHandlers))
                {
                    eventHandlers = new List<AnimationEventHandler>();
                    entityHandlers[eventName] = eventHandlers;
                }
                
                eventHandlers.Add(handler);
            }
        }
        
        /// <summary>
        /// Remove a specific handler for an animation event.
        /// </summary>
        public static void RemoveEventHandler(int entityId, string eventName, AnimationEventHandler handler)
        {
            if (string.IsNullOrEmpty(eventName) || handler == null) return;
            
            lock (s_Lock)
            {
                if (s_EventHandlers.TryGetValue(entityId, out var entityHandlers))
                {
                    if (entityHandlers.TryGetValue(eventName, out var eventHandlers))
                    {
                        eventHandlers.Remove(handler);
                        
                        // Clean up empty collections
                        if (eventHandlers.Count == 0)
                        {
                            entityHandlers.Remove(eventName);
                            if (entityHandlers.Count == 0)
                            {
                                s_EventHandlers.Remove(entityId);
                            }
                        }
                    }
                }
            }
        }
        
        /// <summary>
        /// Remove all handlers for a specific event on an entity.
        /// </summary>
        public static void ClearEventHandlers(int entityId, string eventName)
        {
            if (string.IsNullOrEmpty(eventName)) return;
            
            lock (s_Lock)
            {
                if (s_EventHandlers.TryGetValue(entityId, out var entityHandlers))
                {
                    entityHandlers.Remove(eventName);
                    if (entityHandlers.Count == 0)
                    {
                        s_EventHandlers.Remove(entityId);
                    }
                }
            }
        }
        
        /// <summary>
        /// Remove all handlers for an entity (call when entity is destroyed).
        /// </summary>
        public static void ClearAllEntityHandlers(int entityId)
        {
            lock (s_Lock)
            {
                s_EventHandlers.Remove(entityId);
            }
        }
    }
}

