#include "Input.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <Xinput.h>
#include <cmath>

// Link XInput library
#pragma comment(lib, "xinput.lib")

// Static member definitions
// No GLFW state in Win32 backend path

std::unordered_map<int, bool> Input::s_Keys;
std::unordered_map<int, bool> Input::s_MouseButtons;
std::unordered_map<int, bool> Input::s_KeyDownEdge;
std::unordered_map<int, bool> Input::s_MouseButtonDownEdge;
 
double Input::s_LastMouseX = 0.0;
double Input::s_LastMouseY = 0.0;

float Input::s_ScrollDelta = 0.0f;
float Input::s_MouseDeltaX = 0.0f;
float Input::s_MouseDeltaY = 0.0f;
bool  Input::s_RelativeMode = false;
float Input::s_LockedCenterX = 0.0f;
float Input::s_LockedCenterY = 0.0f;
bool  Input::s_Blocked = false;

// Gamepad state
std::array<Input::GamepadState, Input::MaxGamepads> Input::s_Gamepads{};
float Input::s_GamepadDeadzone = 0.15f;

void Input::Init() {
   // Initialize gamepad state
   for (auto& gp : s_Gamepads) {
      gp.connected = false;
      gp.buttons.fill(false);
      gp.buttonEdges.fill(false);
      gp.axes.fill(0.0f);
   }
}

void Input::Update() {
   // Clear edge presses at frame start
   s_KeyDownEdge.clear();
   s_MouseButtonDownEdge.clear();

   s_ScrollDelta = 0.0f; // Reset scroll after each frame
   s_MouseDeltaX = 0.0f;
   s_MouseDeltaY = 0.0f;
   
   // Poll gamepads
   PollGamepads();
}

float Input::ApplyDeadzone(float value, float deadzone) {
   if (std::abs(value) < deadzone) return 0.0f;
   // Rescale from [deadzone, 1] to [0, 1] preserving sign
   float sign = value > 0 ? 1.0f : -1.0f;
   return sign * (std::abs(value) - deadzone) / (1.0f - deadzone);
}

void Input::PollGamepads() {
   for (int i = 0; i < MaxGamepads; ++i) {
      XINPUT_STATE state{};
      DWORD result = XInputGetState(i, &state);
      
      auto& gp = s_Gamepads[i];
      bool wasConnected = gp.connected;
      gp.connected = (result == ERROR_SUCCESS);
      
      if (!gp.connected) {
         // Clear state when disconnected
         if (wasConnected) {
            gp.buttons.fill(false);
            gp.buttonEdges.fill(false);
            gp.axes.fill(0.0f);
         }
         continue;
      }
      
      // Clear button edges for this frame
      gp.buttonEdges.fill(false);
      
      // Map XInput buttons to our enum
      const WORD buttons = state.Gamepad.wButtons;
      auto updateButton = [&](GamepadButton btn, WORD mask) {
         int idx = static_cast<int>(btn);
         bool pressed = (buttons & mask) != 0;
         if (pressed && !gp.buttons[idx]) {
            gp.buttonEdges[idx] = true;
         }
         gp.buttons[idx] = pressed;
      };
      
      updateButton(GamepadButton::DPadUp, XINPUT_GAMEPAD_DPAD_UP);
      updateButton(GamepadButton::DPadDown, XINPUT_GAMEPAD_DPAD_DOWN);
      updateButton(GamepadButton::DPadLeft, XINPUT_GAMEPAD_DPAD_LEFT);
      updateButton(GamepadButton::DPadRight, XINPUT_GAMEPAD_DPAD_RIGHT);
      updateButton(GamepadButton::Start, XINPUT_GAMEPAD_START);
      updateButton(GamepadButton::Back, XINPUT_GAMEPAD_BACK);
      updateButton(GamepadButton::LeftThumb, XINPUT_GAMEPAD_LEFT_THUMB);
      updateButton(GamepadButton::RightThumb, XINPUT_GAMEPAD_RIGHT_THUMB);
      updateButton(GamepadButton::LeftShoulder, XINPUT_GAMEPAD_LEFT_SHOULDER);
      updateButton(GamepadButton::RightShoulder, XINPUT_GAMEPAD_RIGHT_SHOULDER);
      updateButton(GamepadButton::A, XINPUT_GAMEPAD_A);
      updateButton(GamepadButton::B, XINPUT_GAMEPAD_B);
      updateButton(GamepadButton::X, XINPUT_GAMEPAD_X);
      updateButton(GamepadButton::Y, XINPUT_GAMEPAD_Y);
      
      // Update axes
      // Left stick: -32768 to 32767
      float lx = static_cast<float>(state.Gamepad.sThumbLX) / 32767.0f;
      float ly = static_cast<float>(state.Gamepad.sThumbLY) / 32767.0f;
      // Right stick
      float rx = static_cast<float>(state.Gamepad.sThumbRX) / 32767.0f;
      float ry = static_cast<float>(state.Gamepad.sThumbRY) / 32767.0f;
      // Triggers: 0 to 255
      float lt = static_cast<float>(state.Gamepad.bLeftTrigger) / 255.0f;
      float rt = static_cast<float>(state.Gamepad.bRightTrigger) / 255.0f;
      
      // Apply deadzone to sticks
      gp.axes[static_cast<int>(GamepadAxis::LeftX)] = ApplyDeadzone(lx, s_GamepadDeadzone);
      gp.axes[static_cast<int>(GamepadAxis::LeftY)] = ApplyDeadzone(ly, s_GamepadDeadzone);
      gp.axes[static_cast<int>(GamepadAxis::RightX)] = ApplyDeadzone(rx, s_GamepadDeadzone);
      gp.axes[static_cast<int>(GamepadAxis::RightY)] = ApplyDeadzone(ry, s_GamepadDeadzone);
      // Triggers don't need deadzone (already 0 when not pressed)
      gp.axes[static_cast<int>(GamepadAxis::LeftTrigger)] = lt;
      gp.axes[static_cast<int>(GamepadAxis::RightTrigger)] = rt;
   }
}

bool Input::IsGamepadConnected(int index) {
   if (index < 0 || index >= MaxGamepads) return false;
   return s_Gamepads[index].connected;
}

bool Input::IsGamepadButtonPressed(GamepadButton button, int gamepadIndex) {
   return IsGamepadButtonPressed(static_cast<int>(button), gamepadIndex);
}

bool Input::IsGamepadButtonPressed(int button, int gamepadIndex) {
   if (s_Blocked) return false;
   if (gamepadIndex < 0 || gamepadIndex >= MaxGamepads) return false;
   if (button < 0 || button >= static_cast<int>(GamepadButton::Count)) return false;
   if (!s_Gamepads[gamepadIndex].connected) return false;
   return s_Gamepads[gamepadIndex].buttons[button];
}

bool Input::WasGamepadButtonPressedThisFrame(GamepadButton button, int gamepadIndex) {
   return WasGamepadButtonPressedThisFrame(static_cast<int>(button), gamepadIndex);
}

bool Input::WasGamepadButtonPressedThisFrame(int button, int gamepadIndex) {
   if (s_Blocked) return false;
   if (gamepadIndex < 0 || gamepadIndex >= MaxGamepads) return false;
   if (button < 0 || button >= static_cast<int>(GamepadButton::Count)) return false;
   if (!s_Gamepads[gamepadIndex].connected) return false;
   return s_Gamepads[gamepadIndex].buttonEdges[button];
}

float Input::GetGamepadAxis(GamepadAxis axis, int gamepadIndex) {
   return GetGamepadAxis(static_cast<int>(axis), gamepadIndex);
}

float Input::GetGamepadAxis(int axis, int gamepadIndex) {
   if (s_Blocked) return 0.0f;
   if (gamepadIndex < 0 || gamepadIndex >= MaxGamepads) return 0.0f;
   if (axis < 0 || axis >= static_cast<int>(GamepadAxis::Count)) return 0.0f;
   if (!s_Gamepads[gamepadIndex].connected) return 0.0f;
   return s_Gamepads[gamepadIndex].axes[axis];
}

bool Input::IsKeyPressed(int key) {
   if (s_Blocked && key != '/' && key != '`') return false;
   return s_Keys[key];
}

bool Input::WasKeyPressedThisFrame(int key) {
   if (s_Blocked && key != '/' && key != '`') return false;
   return s_KeyDownEdge[key];
}

bool Input::IsMouseButtonPressed(int button) {
   if (s_Blocked) return false;
   return s_MouseButtons[button];
}

bool Input::WasMouseButtonPressedThisFrame(int button) {
   if (s_Blocked) return false;
   return s_MouseButtonDownEdge[button];
}

std::pair<float, float> Input::GetMouseDelta() {
   if (s_Blocked) return {0.0f, 0.0f};
   return { s_MouseDeltaX, s_MouseDeltaY };
   }

float Input::GetScrollDelta() {
   if (s_Blocked) return 0.0f;
   return s_ScrollDelta;
   }