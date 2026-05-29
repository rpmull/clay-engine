#include "InputInterop.h"
#include "core/input/Input.h"

#ifdef CLAYMORE_EDITOR
#include "editor/ui/Logger.h"
#include "editor/application.h"
#include <imgui.h>
#include <imgui_internal.h>
#else
#include "core/RuntimeApplication.h"
#include "core/platform/win32/Win32Window.h"
#include <iostream>
#endif

// --------------------------------------------------------------------------------------
// Function pointer typedefs matching managed delegate signatures.
// --------------------------------------------------------------------------------------
using IsKeyHeld_fn      = int(*)(int key);
using IsKeyDown_fn      = int(*)(int key);
using IsMouseDown_fn    = int(*)(int button);
using IsMouseHeld_fn    = int(*)(int button);
using GetMouseDelta_fn  = void(*)(float* dx, float* dy);
using GetScrollDelta_fn = float(*)();
using DebugLog_fn       = void(*)(const char* msg);
using SetMouseMode_fn   = void(*)(int mode);

// Gamepad function pointers
using IsGamepadConnected_fn     = int(*)(int gamepadIndex);
using IsGamepadButtonHeld_fn    = int(*)(int button, int gamepadIndex);
using IsGamepadButtonDown_fn    = int(*)(int button, int gamepadIndex);
using GetGamepadAxis_fn         = float(*)(int axis, int gamepadIndex);

// --------------------------------------------------------------------------------------
// Pointer instances passed to managed side (set up in DotNetHost.cpp)
// --------------------------------------------------------------------------------------
IsKeyHeld_fn     IsKeyHeldPtr     = &IsKeyHeld;
IsKeyDown_fn     IsKeyDownPtr     = &IsKeyDown;
IsMouseDown_fn   IsMouseDownPtr   = &IsMouseDown;
IsMouseHeld_fn   IsMouseHeldPtr   = &IsMouseHeld;
GetMouseDelta_fn GetMouseDeltaPtr = &GetMouseDelta;
GetScrollDelta_fn GetScrollDeltaPtr = &GetScrollDelta;
DebugLog_fn      DebugLogPtr      = &DebugLog;
SetMouseMode_fn  SetMouseModePtr  = &SetMouseMode;

// Gamepad function pointers
IsGamepadConnected_fn  IsGamepadConnectedPtr  = &IsGamepadConnected;
IsGamepadButtonHeld_fn IsGamepadButtonHeldPtr = &IsGamepadButtonHeld;
IsGamepadButtonDown_fn IsGamepadButtonDownPtr = &IsGamepadButtonDown;
GetGamepadAxis_fn      GetGamepadAxisPtr      = &GetGamepadAxis;

#ifdef CLAYMORE_EDITOR
static bool EditorAllowsGameMouseInput()
{
    if (!Application::HasInstance()) {
        return true;
    }
    return Application::Get().IsGameViewportMouseInputAllowed();
}
#endif

// --------------------------------------------------------------------------------------
// Exported implementation
// --------------------------------------------------------------------------------------
extern "C" {

__declspec(dllexport) int IsKeyHeld(int key)
{
    return Input::IsKeyPressed(key) ? 1 : 0;
}

__declspec(dllexport) int IsKeyDown(int key)
{
    return Input::WasKeyPressedThisFrame(key) ? 1 : 0;
}

__declspec(dllexport) int IsMouseDown(int button)
{
#ifdef CLAYMORE_EDITOR
    if (!EditorAllowsGameMouseInput()) {
        return 0;
    }
#endif
    return Input::WasMouseButtonPressedThisFrame(button) ? 1 : 0;
}

__declspec(dllexport) int IsMouseHeld(int button)
{
#ifdef CLAYMORE_EDITOR
    if (!EditorAllowsGameMouseInput()) {
        return 0;
    }
#endif
    return Input::IsMouseButtonPressed(button) ? 1 : 0;
}

__declspec(dllexport) void GetMouseDelta(float* deltaX, float* deltaY)
{
    if (!deltaX || !deltaY)
        return;
#ifdef CLAYMORE_EDITOR
    if (!EditorAllowsGameMouseInput()) {
        *deltaX = 0.0f;
        *deltaY = 0.0f;
        return;
    }
#endif
    auto d = Input::GetMouseDelta();
    *deltaX = d.first;
    *deltaY = d.second;
}

__declspec(dllexport) float GetScrollDelta()
{
#ifdef CLAYMORE_EDITOR
    if (!EditorAllowsGameMouseInput()) {
        return 0.0f;
    }
#endif
    return Input::GetScrollDelta();
}

__declspec(dllexport) void DebugLog(const char* msg)
{
    if(msg) {
#ifdef CLAYMORE_EDITOR
        Logger::Log(msg);
#else
        std::cout << "[Script] " << msg << std::endl;
#endif
    }
}

__declspec(dllexport) void SetMouseMode(int mode)
{
    // 0 = free, 1 = captured/relative
    bool capture = (mode == 1);
    
#ifdef CLAYMORE_EDITOR
    Application& app = Application::Get();
    const bool allowEditorCapture = !app.m_RunEditorUI || app.IsPlaying();

    // Never allow gameplay scripts to capture the editor mouse while not in play mode.
    // Edit-mode scripts can still request free mode, which also clears any stale capture.
    if (capture && !allowEditorCapture) {
        app.SetMouseCaptured(false);
        return;
    }

    // Toggle platform capture and input relative mode via editor Application
    app.SetMouseCaptured(capture);

    // In editor, make sure ImGui doesn't capture/hover when captured
    // Skip ImGui calls in game mode where no context exists
    if (capture && app.m_RunEditorUI) {
        ImGuiIO& io = ImGui::GetIO();
        io.WantCaptureMouse = false;
        io.WantCaptureKeyboard = false;
        io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
        ImGui::ClearActiveID();
    }
#else
    // Runtime: use Win32Window directly
    if (RuntimeApplication::HasInstance()) {
        Win32Window* window = RuntimeApplication::Get().GetWin32Window();
        if (window) {
            window->SetCursorCaptured(capture);
        }
    }
    Input::SetRelativeMode(capture);
#endif
}

// --------------------------------------------------------------------------------------
// Gamepad functions
// --------------------------------------------------------------------------------------
__declspec(dllexport) int IsGamepadConnected(int gamepadIndex)
{
    return Input::IsGamepadConnected(gamepadIndex) ? 1 : 0;
}

__declspec(dllexport) int IsGamepadButtonHeld(int button, int gamepadIndex)
{
    return Input::IsGamepadButtonPressed(button, gamepadIndex) ? 1 : 0;
}

__declspec(dllexport) int IsGamepadButtonDown(int button, int gamepadIndex)
{
    return Input::WasGamepadButtonPressedThisFrame(button, gamepadIndex) ? 1 : 0;
}

__declspec(dllexport) float GetGamepadAxis(int axis, int gamepadIndex)
{
    return Input::GetGamepadAxis(axis, gamepadIndex);
}
 
}
 
