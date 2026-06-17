using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    /// <summary>
    /// Native callback signature for animation state transitions from C++.
    /// phase: 1 = entered, 0 = exited. otherStateName is the previous state on enter,
    /// or the next state on exit.
    /// </summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void NativeAnimationStateCallback(
        int entityId,
        [MarshalAs(UnmanagedType.LPStr)] string stateName,
        [MarshalAs(UnmanagedType.LPStr)] string otherStateName,
        int phase);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void SetAnimationStateCallbackFn(NativeAnimationStateCallback callback);

    /// <summary>
    /// Interop bridge for animation state enter/exit transitions between native C++ and
    /// managed C# code. Native dispatches every committed state transition; this class
    /// routes them to the per-(entity, state-name) handlers registered through
    /// <see cref="AnimatorState"/>.
    ///
    /// Performance: native only dispatches on actual transitions (never per frame), and
    /// the dispatch is a single atomic load + null check when nothing is subscribed.
    /// </summary>
    public static unsafe class AnimationStateInterop
    {
        internal static SetAnimationStateCallbackFn? SetAnimationStateCallback;

        // Keep the callback delegate alive to prevent GC collection.
        private static NativeAnimationStateCallback? s_NativeCallback;

        // entityId -> stateName -> handlers
        private static readonly Dictionary<int, Dictionary<string, List<Action>>> s_EnterHandlers = new();
        private static readonly Dictionary<int, Dictionary<string, List<Action>>> s_ExitHandlers = new();
        private static readonly object s_Lock = new();

        /// <summary>
        /// Initialize the interop system with function pointers from native code.
        /// Reuses the AnimationEventInteropInitDelegate signature (IntPtr*, int).
        /// </summary>
        public static void Initialize(IntPtr* ptrs, int count)
        {
            Console.WriteLine($"[AnimationStateInterop] Initialize called with count={count}");
            if (count < 1)
            {
                Console.WriteLine($"[AnimationStateInterop] Expected at least 1 function pointer, got {count}");
                return;
            }

            SetAnimationStateCallback = Marshal.GetDelegateForFunctionPointer<SetAnimationStateCallbackFn>(ptrs[0]);

            s_NativeCallback = OnAnimationStateEvent;
            SetAnimationStateCallback?.Invoke(s_NativeCallback);

            Console.WriteLine("[AnimationStateInterop] Initialize complete");
        }

        private static void OnAnimationStateEvent(int entityId, string stateName, string otherStateName, int phase)
        {
            var map = (phase == 1) ? s_EnterHandlers : s_ExitHandlers;

            List<Action>? handlers = null;
            lock (s_Lock)
            {
                if (map.TryGetValue(entityId, out var byState) &&
                    byState.TryGetValue(stateName, out var list))
                {
                    // Copy so we don't hold the lock during invocation.
                    handlers = new List<Action>(list);
                }
            }

            if (handlers != null)
            {
                foreach (var handler in handlers)
                {
                    try
                    {
                        handler();
                    }
                    catch (Exception ex)
                    {
                        Console.WriteLine($"[AnimationStateInterop] Handler exception: {ex}");
                    }
                }
            }
        }

        private static void Add(Dictionary<int, Dictionary<string, List<Action>>> map, int entityId, string stateName, Action handler)
        {
            if (string.IsNullOrEmpty(stateName) || handler == null) return;
            lock (s_Lock)
            {
                if (!map.TryGetValue(entityId, out var byState))
                {
                    byState = new Dictionary<string, List<Action>>();
                    map[entityId] = byState;
                }
                if (!byState.TryGetValue(stateName, out var list))
                {
                    list = new List<Action>();
                    byState[stateName] = list;
                }
                list.Add(handler);
            }
        }

        private static void Remove(Dictionary<int, Dictionary<string, List<Action>>> map, int entityId, string stateName, Action handler)
        {
            if (string.IsNullOrEmpty(stateName) || handler == null) return;
            lock (s_Lock)
            {
                if (map.TryGetValue(entityId, out var byState) &&
                    byState.TryGetValue(stateName, out var list))
                {
                    list.Remove(handler);
                    if (list.Count == 0)
                    {
                        byState.Remove(stateName);
                        if (byState.Count == 0) map.Remove(entityId);
                    }
                }
            }
        }

        internal static void AddEnterHandler(int entityId, string stateName, Action handler) => Add(s_EnterHandlers, entityId, stateName, handler);
        internal static void RemoveEnterHandler(int entityId, string stateName, Action handler) => Remove(s_EnterHandlers, entityId, stateName, handler);
        internal static void AddExitHandler(int entityId, string stateName, Action handler) => Add(s_ExitHandlers, entityId, stateName, handler);
        internal static void RemoveExitHandler(int entityId, string stateName, Action handler) => Remove(s_ExitHandlers, entityId, stateName, handler);

        /// <summary>
        /// Remove all state handlers for an entity (call when the entity is destroyed).
        /// </summary>
        public static void ClearAllEntityHandlers(int entityId)
        {
            lock (s_Lock)
            {
                s_EnterHandlers.Remove(entityId);
                s_ExitHandlers.Remove(entityId);
            }
        }
    }
}
