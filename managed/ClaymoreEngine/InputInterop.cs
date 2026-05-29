using System;
using System.Numerics;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public unsafe delegate void InputInteropInitDelegate(IntPtr* functionPointers, int count);

    /// <summary>
    /// Managed side wrapper around native InputInterop functions.
    /// Mirrors the pattern used by <see cref="EntityInterop"/>.
    /// </summary>
    public static unsafe class InputInterop
    {
        // ---------------------------------------------------------------------------------
        // Delegate definitions (must match native signatures & calling convention order)
        // ---------------------------------------------------------------------------------
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int  IsKeyHeldFn(int key);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int  IsKeyDownFn(int key);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int  IsMouseDownFn(int button);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int  IsMouseHeldFn(int button);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void GetMouseDeltaFn(out float dx, out float dy);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float GetScrollDeltaFn();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void SetMouseModeFn(int mode);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void LogFn([MarshalAs(UnmanagedType.LPStr)] string message);
        
        // Gamepad delegates
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int IsGamepadConnectedFn(int gamepadIndex);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int IsGamepadButtonHeldFn(int button, int gamepadIndex);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int IsGamepadButtonDownFn(int button, int gamepadIndex);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float GetGamepadAxisFn(int axis, int gamepadIndex);

        // ------------------------------------------------------------------------------
        // Resolved function pointers
        // ------------------------------------------------------------------------------
        public static IsKeyHeldFn     IsKeyHeld;
        public static IsKeyDownFn     IsKeyDown;
        public static IsMouseDownFn   IsMouseDown;
        public static IsMouseHeldFn   IsMouseHeld;
        public static GetMouseDeltaFn GetMouseDelta;
        public static GetScrollDeltaFn GetScrollDelta;
        public static SetMouseModeFn  SetMouseMode;
        public static LogFn           Log;
        
        // Gamepad function pointers
        public static IsGamepadConnectedFn  IsGamepadConnected;
        public static IsGamepadButtonHeldFn IsGamepadButtonHeld;
        public static IsGamepadButtonDownFn IsGamepadButtonDown;
        public static GetGamepadAxisFn      GetGamepadAxis;

        // ------------------------------------------------------------------------------
        // Initialization from native side.
        // ------------------------------------------------------------------------------
        public static void InitializeInteropExport(IntPtr* ptrs, int count) => InitializeInterop(ptrs, count);

        public static unsafe void InitializeInterop(IntPtr* ptrs, int count)
        {
            if (count < 12)
            {
                Console.WriteLine($"[InputInterop] Expected >=12 function pointers, received {count}.");
                return;
            }

            int i = 0;
            IsKeyHeld   = Marshal.GetDelegateForFunctionPointer<IsKeyHeldFn>(ptrs[i++]);
            IsKeyDown   = Marshal.GetDelegateForFunctionPointer<IsKeyDownFn>(ptrs[i++]);
            IsMouseDown = Marshal.GetDelegateForFunctionPointer<IsMouseDownFn>(ptrs[i++]);
            IsMouseHeld = Marshal.GetDelegateForFunctionPointer<IsMouseHeldFn>(ptrs[i++]);
            GetMouseDelta = Marshal.GetDelegateForFunctionPointer<GetMouseDeltaFn>(ptrs[i++]);
            GetScrollDelta = Marshal.GetDelegateForFunctionPointer<GetScrollDeltaFn>(ptrs[i++]);
            Log          = Marshal.GetDelegateForFunctionPointer<LogFn>(ptrs[i++]);
            SetMouseMode = Marshal.GetDelegateForFunctionPointer<SetMouseModeFn>(ptrs[i++]);
            
            // Gamepad
            IsGamepadConnected  = Marshal.GetDelegateForFunctionPointer<IsGamepadConnectedFn>(ptrs[i++]);
            IsGamepadButtonHeld = Marshal.GetDelegateForFunctionPointer<IsGamepadButtonHeldFn>(ptrs[i++]);
            IsGamepadButtonDown = Marshal.GetDelegateForFunctionPointer<IsGamepadButtonDownFn>(ptrs[i++]);
            GetGamepadAxis      = Marshal.GetDelegateForFunctionPointer<GetGamepadAxisFn>(ptrs[i++]);

            Console.WriteLine("[Managed] InputInterop delegates initialized (with gamepad and scroll support).");
        }
    }

    // -------------------------------------------------------------------------------------
    // Gamepad button and axis enums (matches native side)
    // -------------------------------------------------------------------------------------
    public enum GamepadButton
    {
        DPadUp = 0,
        DPadDown,
        DPadLeft,
        DPadRight,
        Start,
        Back,
        LeftThumb,
        RightThumb,
        LeftShoulder,
        RightShoulder,
        // 10, 11 reserved
        A = 12,
        B,
        X,
        Y
    }

    public enum GamepadAxis
    {
        LeftX = 0,
        LeftY,
        RightX,
        RightY,
        LeftTrigger,
        RightTrigger
    }

    // -------------------------------------------------------------------------------------
    // Convenience API exposed to user scripts
    // -------------------------------------------------------------------------------------
    public static class Input
    {
        public enum MouseMode { Free = 0, Captured = 1 }
        
        /// <summary>
        /// Returns true the frame the key became pressed.
        /// </summary>
        public static bool GetKeyDown(KeyCode key)  => InputInterop.IsKeyDown((int)key) != 0;
        
        /// <summary>
        /// Returns true if the key is currently held.
        /// </summary>
        public static bool GetKey(KeyCode key)      => InputInterop.IsKeyHeld((int)key) != 0;

        /// <summary>
        /// Returns true only on the frame the mouse button was pressed (0 = left, 1 = right ...).
        /// </summary>
        public static bool GetMouseDown(int button) => InputInterop.IsMouseDown(button) != 0;

        /// <summary>
        /// Returns true if specified mouse button is currently held (0 = left, 1 = right ...).
        /// Use this for continuous actions like dragging or holding to charge.
        /// </summary>
        public static bool GetMouse(int button) => InputInterop.IsMouseHeld(button) != 0;

        public static Vector2 GetMouseDelta()
        {
            InputInterop.GetMouseDelta(out float dx, out float dy);
            return new Vector2(dx, dy);
        }

        /// <summary>
        /// Returns the mouse scroll wheel delta for the current frame.
        /// Positive values indicate scrolling up/forward, negative values indicate scrolling down/backward.
        /// </summary>
        public static float GetMouseScrollDelta()
        {
            return InputInterop.GetScrollDelta?.Invoke() ?? 0f;
        }

        public static void SetMouseMode(MouseMode mode)
        {
            InputInterop.SetMouseMode((int)mode);
        }
        
        // ---------------------------------------------------------------------------------
        // Gamepad API
        // ---------------------------------------------------------------------------------
        
        /// <summary>
        /// Returns true if a gamepad is connected at the specified index (0-3).
        /// </summary>
        public static bool IsGamepadConnected(int gamepadIndex = 0)
        {
            return InputInterop.IsGamepadConnected?.Invoke(gamepadIndex) == 1;
        }
        
        /// <summary>
        /// Returns true if the specified gamepad button is currently held.
        /// </summary>
        public static bool GetGamepadButton(GamepadButton button, int gamepadIndex = 0)
        {
            return InputInterop.IsGamepadButtonHeld?.Invoke((int)button, gamepadIndex) == 1;
        }
        
        /// <summary>
        /// Returns true the frame the gamepad button was pressed.
        /// </summary>
        public static bool GetGamepadButtonDown(GamepadButton button, int gamepadIndex = 0)
        {
            return InputInterop.IsGamepadButtonDown?.Invoke((int)button, gamepadIndex) == 1;
        }
        
        /// <summary>
        /// Returns the value of a gamepad axis. Sticks: [-1, 1], Triggers: [0, 1].
        /// </summary>
        public static float GetGamepadAxis(GamepadAxis axis, int gamepadIndex = 0)
        {
            return InputInterop.GetGamepadAxis?.Invoke((int)axis, gamepadIndex) ?? 0f;
        }
        
        /// <summary>
        /// Returns left stick as a Vector2 (X, Y).
        /// </summary>
        public static Vector2 GetLeftStick(int gamepadIndex = 0)
        {
            float x = GetGamepadAxis(GamepadAxis.LeftX, gamepadIndex);
            float y = GetGamepadAxis(GamepadAxis.LeftY, gamepadIndex);
            return new Vector2(x, y);
        }
        
        /// <summary>
        /// Returns right stick as a Vector2 (X, Y).
        /// </summary>
        public static Vector2 GetRightStick(int gamepadIndex = 0)
        {
            float x = GetGamepadAxis(GamepadAxis.RightX, gamepadIndex);
            float y = GetGamepadAxis(GamepadAxis.RightY, gamepadIndex);
            return new Vector2(x, y);
        }
        
        /// <summary>
        /// Returns left trigger value [0, 1].
        /// </summary>
        public static float GetLeftTrigger(int gamepadIndex = 0)
        {
            return GetGamepadAxis(GamepadAxis.LeftTrigger, gamepadIndex);
        }
        
        /// <summary>
        /// Returns right trigger value [0, 1].
        /// </summary>
        public static float GetRightTrigger(int gamepadIndex = 0)
        {
            return GetGamepadAxis(GamepadAxis.RightTrigger, gamepadIndex);
        }
    }

    // -------------------------------------------------------------------------------------
    // Simple Debug.Log wrapper
    // -------------------------------------------------------------------------------------
    public static class Debug
    {
        public static void Log(string message)
        {
            InputInterop.Log(message);
        }
    }

    // -------------------------------------------------------------------------------------
    // Minimal subset of GLFW key codes exposed to scripts.
    // Extend as required.
    // -------------------------------------------------------------------------------------
    public enum KeyCode
    {
        Unknown      = -1,
        Space        = 32,
        Apostrophe   = 39,
        Comma        = 44,
        Minus        = 45,
        Period       = 46,
        Slash        = 47,
        Digit0       = 48,
        Digit1       = 49,
        Digit2       = 50,
        Digit3       = 51,
        Digit4       = 52,
        Digit5       = 53,
        Digit6       = 54,
        Digit7       = 55,
        Digit8       = 56,
        Digit9       = 57,
        Semicolon    = 59,
        Equal        = 61,
        A            = 65,
        B            = 66,
        C            = 67,
        D            = 68,
        E            = 69,
        F            = 70,
        G            = 71,
        H            = 72,
        I            = 73,
        J            = 74,
        K            = 75,
        L            = 76,
        M            = 77,
        N            = 78,
        O            = 79,
        P            = 80,
        Q            = 81,
        R            = 82,
        S            = 83,
        T            = 84,
        U            = 85,
        V            = 86,
        W            = 87,
        X            = 88,
        Y            = 89,
        Z            = 90,
        LeftBracket  = 91,
        Backslash    = 92,
        RightBracket = 93,
        GraveAccent  = 96,
        World1       = 161,
        World2       = 162,

        Escape      = 256,
        Enter       = 257,
        Tab         = 258,
        Backspace   = 259,
        Insert      = 260,
        Delete      = 261,
        Right       = 262,
        Left        = 263,
        Down        = 264,
        Up          = 265,
        PageUp      = 266,
        PageDown    = 267,
        Home        = 268,
        End         = 269,
        CapsLock    = 280,
        ScrollLock  = 281,
        NumLock     = 282,
        PrintScreen = 283,
        Pause       = 284,
        F1          = 290,
        F2          = 291,
        F3          = 292,
        F4          = 293,
        F5          = 294,
        F6          = 295,
        F7          = 296,
        F8          = 297,
        F9          = 298,
        F10         = 299,
        F11         = 300,
        F12         = 301,
        F13         = 302,
        F14         = 303,
        F15         = 304,
        F16         = 305,
        F17         = 306,
        F18         = 307,
        F19         = 308,
        F20         = 309,
        F21         = 310,
        F22         = 311,
        F23         = 312,
        F24         = 313,
        F25         = 314,

        KP_0       = 320,
        KP_1       = 321,
        KP_2       = 322,
        KP_3       = 323,
        KP_4       = 324,
        KP_5       = 325,
        KP_6       = 326,
        KP_7       = 327,
        KP_8       = 328,
        KP_9       = 329,
        KP_Decimal = 330,
        KP_Divide  = 331,
        KP_Multiply= 332,
        KP_Subtract= 333,
        KP_Add     = 334,
        KP_Enter   = 335,
        KP_Equal   = 336,

        LeftShift   = 340,
        LeftControl = 341,
        LeftAlt     = 342,
        LeftSuper   = 343,
        RightShift   = 344,
        RightControl = 345,
        RightAlt     = 346,
        RightSuper   = 347,
        Menu         = 348,
    }
}
