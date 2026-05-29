#pragma once
#include <unordered_map>
#include <utility>
#include <array>
#include "core/platform/KeyCodes.h"

// Gamepad button indices (XInput layout)
enum class GamepadButton {
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
   A = 12,
   B,
   X,
   Y,
   Count = 16
};

// Gamepad axis indices
enum class GamepadAxis {
   LeftX = 0,
   LeftY,
   RightX,
   RightY,
   LeftTrigger,
   RightTrigger,
   Count = 6
};

class Input {
public:
   static void Init();
   static void Update();
   static void SetRelativeMode(bool enabled) { s_RelativeMode = enabled; }
   static bool IsRelativeMode() { return s_RelativeMode; }

   // When true, all input query functions report no input (used by console overlay)
   static void SetBlocked(bool blocked) { s_Blocked = blocked; }
   static bool IsBlocked() { return s_Blocked; }

   static bool IsKeyPressed(int key);
   static bool IsKeyPressed(KeyCode key) { return IsKeyPressed(static_cast<int>(key)); }

   static bool WasKeyPressedThisFrame(int key);
   static bool WasKeyPressedThisFrame(KeyCode key) { return WasKeyPressedThisFrame(static_cast<int>(key)); }

   static bool IsMouseButtonPressed(int button);
   static bool IsMouseButtonPressed(MouseButton button) { return IsMouseButtonPressed(static_cast<int>(button)); }
   
   static bool WasMouseButtonPressedThisFrame(int button);
   static bool WasMouseButtonPressedThisFrame(MouseButton button) { return WasMouseButtonPressedThisFrame(static_cast<int>(button)); }
   
   static std::pair<float, float> GetMouseDelta();
   static float GetScrollDelta();
   static std::pair<float, float> GetMousePosition() {
      if (s_RelativeMode) {
         return { s_LockedCenterX, s_LockedCenterY };
      }
      return { static_cast<float>(s_LastMouseX), static_cast<float>(s_LastMouseY) };
   }
   static void SetLockedCenter(float x, float y) { s_LockedCenterX = x; s_LockedCenterY = y; }

   // =========================================================================
   // Gamepad API
   // =========================================================================
   static constexpr int MaxGamepads = 4;
   
   // Returns true if a gamepad is connected at the given index (0-3)
   static bool IsGamepadConnected(int index = 0);
   
   // Returns true if the specified button is currently pressed
   static bool IsGamepadButtonPressed(GamepadButton button, int gamepadIndex = 0);
   static bool IsGamepadButtonPressed(int button, int gamepadIndex = 0);
   
   // Returns true if the button was pressed this frame (edge detection)
   static bool WasGamepadButtonPressedThisFrame(GamepadButton button, int gamepadIndex = 0);
   static bool WasGamepadButtonPressedThisFrame(int button, int gamepadIndex = 0);
   
   // Returns axis value in range [-1, 1] for sticks, [0, 1] for triggers
   static float GetGamepadAxis(GamepadAxis axis, int gamepadIndex = 0);
   static float GetGamepadAxis(int axis, int gamepadIndex = 0);
   
   // Deadzone for stick axes (applied automatically)
   static void SetGamepadDeadzone(float deadzone) { s_GamepadDeadzone = deadzone; }
   static float GetGamepadDeadzone() { return s_GamepadDeadzone; }
   
   // =========================================================================
   // Event handlers
   // =========================================================================
   static void OnKey(int key, int action) {
      bool isPress = (action != 0);
      if (isPress && !s_Keys[key]) s_KeyDownEdge[key] = true;
      s_Keys[key] = isPress;            // zero = release
      }
   static void OnKey(KeyCode key, InputAction action) {
      OnKey(static_cast<int>(key), static_cast<int>(action));
      }

   static void OnMouseButton(int button, int action) {
      bool isPress = (action != 0);
      if (isPress && !s_MouseButtons[button]) s_MouseButtonDownEdge[button] = true;
      s_MouseButtons[button] = isPress;
      }
   static void OnMouseButton(MouseButton button, InputAction action) {
      OnMouseButton(static_cast<int>(button), static_cast<int>(action));
      }

   static void OnMouseMove(double xpos, double ypos) {
      if (s_RelativeMode) {
         // Treat inputs as deltas; accumulate for this frame
         s_MouseDeltaX += static_cast<float>(xpos);
         s_MouseDeltaY += static_cast<float>(ypos);
      } else {
         s_MouseDeltaX = static_cast<float>(xpos - s_LastMouseX);
         s_MouseDeltaY = static_cast<float>(ypos - s_LastMouseY);
         s_LastMouseX = xpos;
         s_LastMouseY = ypos;
      }
      }

   static void OnScroll(double yoffset) {
      s_ScrollDelta = static_cast<float>(yoffset);
      }

private:
   // Keyboard/Mouse state
   static std::unordered_map<int, bool> s_Keys;
   static std::unordered_map<int, bool> s_KeyDownEdge;
   static std::unordered_map<int, bool> s_MouseButtons;
   static std::unordered_map<int, bool> s_MouseButtonDownEdge;

   static double s_LastMouseX;
   static double s_LastMouseY;

   static float s_ScrollDelta;
   static float s_MouseDeltaX;
   static float s_MouseDeltaY;
   static bool  s_RelativeMode;
   static float s_LockedCenterX;
   static float s_LockedCenterY;
   static bool  s_Blocked;
   
   // Gamepad state (per controller)
   struct GamepadState {
      bool connected = false;
      std::array<bool, static_cast<size_t>(GamepadButton::Count)> buttons{};
      std::array<bool, static_cast<size_t>(GamepadButton::Count)> buttonEdges{};
      std::array<float, static_cast<size_t>(GamepadAxis::Count)> axes{};
   };
   static std::array<GamepadState, MaxGamepads> s_Gamepads;
   static float s_GamepadDeadzone;
   
   // Internal: poll XInput state
   static void PollGamepads();
   static float ApplyDeadzone(float value, float deadzone);
   };
