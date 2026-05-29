#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// ----------------------------------------
// Native <-> Managed Input Interop exports
// ----------------------------------------
// Returns 1 if the given key is currently held, 0 otherwise.
__declspec(dllexport) int IsKeyHeld(int key);
// Returns 1 only on the frame the key transitioned to pressed.
__declspec(dllexport) int IsKeyDown(int key);
// Returns 1 only on the frame the mouse button transitioned to pressed.
__declspec(dllexport) int IsMouseDown(int button);
// Returns 1 if the given mouse button is currently held, 0 otherwise.
__declspec(dllexport) int IsMouseHeld(int button);
// Returns current mouse delta since last frame.
__declspec(dllexport) void GetMouseDelta(float* deltaX, float* deltaY);
// Returns mouse scroll wheel delta since last frame.
__declspec(dllexport) float GetScrollDelta();
// Writes a message to the engine logger.
__declspec(dllexport) void DebugLog(const char* msg);
// Set mouse cursor mode: 0 = Free, 1 = Captured/Relative
__declspec(dllexport) void SetMouseMode(int mode);

// ----------------------------------------
// Gamepad Input
// ----------------------------------------
// Returns 1 if a gamepad is connected at the given index (0-3).
__declspec(dllexport) int IsGamepadConnected(int gamepadIndex);
// Returns 1 if the specified button is held on the gamepad.
__declspec(dllexport) int IsGamepadButtonHeld(int button, int gamepadIndex);
// Returns 1 only on the frame the button was pressed.
__declspec(dllexport) int IsGamepadButtonDown(int button, int gamepadIndex);
// Returns axis value: sticks in [-1,1], triggers in [0,1].
__declspec(dllexport) float GetGamepadAxis(int axis, int gamepadIndex);

#ifdef __cplusplus
}
#endif

// --------------------------------------------------------------------------------------
// Function pointer typedefs & extern declarations
// --------------------------------------------------------------------------------------
using IsKeyHeld_fn = int(*)(int key);
using IsKeyDown_fn     = int(*)(int key);
using IsMouseDown_fn   = int(*)(int button);
using IsMouseHeld_fn   = int(*)(int button);
using GetMouseDelta_fn = void(*)(float* dx, float* dy);
using GetScrollDelta_fn = float(*)();
using SetMouseMode_fn = void(*)(int mode);

extern IsKeyHeld_fn      IsKeyHeldPtr;
extern IsKeyDown_fn      IsKeyDownPtr;
extern IsMouseDown_fn    IsMouseDownPtr;
extern IsMouseHeld_fn    IsMouseHeldPtr;
extern GetMouseDelta_fn  GetMouseDeltaPtr;
extern GetScrollDelta_fn GetScrollDeltaPtr;
extern SetMouseMode_fn   SetMouseModePtr;

// Logger
using DebugLog_fn = void(*)(const char* msg);
extern DebugLog_fn DebugLogPtr;

// Gamepad
using IsGamepadConnected_fn  = int(*)(int gamepadIndex);
using IsGamepadButtonHeld_fn = int(*)(int button, int gamepadIndex);
using IsGamepadButtonDown_fn = int(*)(int button, int gamepadIndex);
using GetGamepadAxis_fn      = float(*)(int axis, int gamepadIndex);

extern IsGamepadConnected_fn  IsGamepadConnectedPtr;
extern IsGamepadButtonHeld_fn IsGamepadButtonHeldPtr;
extern IsGamepadButtonDown_fn IsGamepadButtonDownPtr;
extern GetGamepadAxis_fn      GetGamepadAxisPtr;

