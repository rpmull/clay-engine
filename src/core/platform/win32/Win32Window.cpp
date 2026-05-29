#include "Win32Window.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <cstdio>
#include <algorithm>
#include <filesystem>
#include <vector>
#include <string>
#include "core/input/Input.h"
#include "core/platform/KeyCodes.h"

// ImGui input handling for editor
#include <imgui.h>
#include "backends/imgui_impl_win32.h"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#pragma comment(lib, "dwmapi.lib")

static Win32Window* g_WindowInstance = nullptr; 

static KeyCode TranslateVirtualKey(int vk);
static KeyCode MapVirtualKeyToKeyCode(WPARAM wParam, LPARAM lParam);

namespace {

constexpr WORD kClaymoreAppIconResourceId = 101;

HICON LoadClaymoreWindowIcon(HINSTANCE instance, int size)
{
	UINT resourceFlags = LR_DEFAULTCOLOR;
	if (size <= 0) {
		resourceFlags |= LR_DEFAULTSIZE;
	}

	HICON icon = static_cast<HICON>(::LoadImageW(
		instance,
		MAKEINTRESOURCEW(kClaymoreAppIconResourceId),
		IMAGE_ICON,
		size,
		size,
		resourceFlags));
	if (icon) {
		return icon;
	}

	const std::wstring fallbackPath = (std::filesystem::current_path() / "assets" / "icons" / "claymore_icon.ico").wstring();
	UINT fileFlags = LR_LOADFROMFILE;
	if (size <= 0) {
		fileFlags |= LR_DEFAULTSIZE;
	}

	return static_cast<HICON>(::LoadImageW(
		nullptr,
		fallbackPath.c_str(),
		IMAGE_ICON,
		size,
		size,
		fileFlags));
}

} // namespace

float Win32Window::GetDPIScale() const {
	if (!m_HighDPI || m_hWnd == nullptr) return 1.0f;
	UINT dpi = 96;
	// GetDpiForWindow is Win10+ 
	HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
	if (hUser32) {
		auto pGetDpiForWindow = reinterpret_cast<UINT(WINAPI*)(HWND)>(GetProcAddress(hUser32, "GetDpiForWindow"));
		if (pGetDpiForWindow) dpi = pGetDpiForWindow(m_hWnd);
	}
	return std::max(1.0f, static_cast<float>(dpi) / 96.0f);
}

bool Win32Window::Create(const wchar_t* title, int width, int height, bool resizable, bool highDPI, bool maximize, bool center) {
	m_HighDPI = highDPI;
	m_hInstance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{ sizeof(wc) };
	wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
	wc.lpfnWndProc = &Win32Window::WndProc;
	wc.hInstance = m_hInstance;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hIcon = LoadClaymoreWindowIcon(m_hInstance, GetSystemMetrics(SM_CXICON));
	wc.hIconSm = LoadClaymoreWindowIcon(m_hInstance, GetSystemMetrics(SM_CXSMICON));
	wc.lpszClassName = L"ClaymoreWin32Window";
	if (!RegisterClassExW(&wc)) {
		DWORD err = GetLastError();
		if (err != ERROR_CLASS_ALREADY_EXISTS) return false;
	}

	DWORD style = WS_OVERLAPPEDWINDOW;
	if (!resizable) style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
	RECT r{ 0, 0, width, height };
	AdjustWindowRect(&r, style, FALSE);

    // Set global instance BEFORE creating window so early messages (e.g., WM_SIZE) route to the correct object
	g_WindowInstance = this;

	m_hWnd = CreateWindowExW(0, wc.lpszClassName, title, style,
		CW_USEDEFAULT, CW_USEDEFAULT,
		r.right - r.left, r.bottom - r.top,
		nullptr, nullptr, m_hInstance, nullptr);
	if (!m_hWnd) return false;

    // Ensure icon applied to existing window handles
    if (wc.hIcon) SendMessageW(m_hWnd, WM_SETICON, ICON_BIG, (LPARAM)wc.hIcon);
    if (wc.hIconSm) SendMessageW(m_hWnd, WM_SETICON, ICON_SMALL, (LPARAM)wc.hIconSm);

	// Set border and titlebar to match Claymore engine color (bg0: RGB(36, 36, 36))
	COLORREF claymoreColor = RGB(36, 36, 36);
	DwmSetWindowAttribute(m_hWnd, DWMWA_BORDER_COLOR, &claymoreColor, sizeof(claymoreColor));
	DwmSetWindowAttribute(m_hWnd, DWMWA_CAPTION_COLOR, &claymoreColor, sizeof(claymoreColor));
	// Refresh window frame to apply DWM changes
	SetWindowPos(m_hWnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

	if (maximize) {
		ShowWindow(m_hWnd, SW_SHOWMAXIMIZED);
		if (!IsZoomed(m_hWnd)) {
			ShowWindow(m_hWnd, SW_MAXIMIZE);
		}
	} else {
		ShowWindow(m_hWnd, SW_SHOW);
	}
	UpdateWindow(m_hWnd);
	SetForegroundWindow(m_hWnd);

	// Optionally center the window when not maximized
	if (center && !maximize) {
		RECT rc; GetWindowRect(m_hWnd, &rc);
		int winW = rc.right - rc.left;
		int winH = rc.bottom - rc.top;
		RECT wa{};
		SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
		int x = wa.left + ((wa.right - wa.left) - winW) / 2;
		int y = wa.top + ((wa.bottom - wa.top) - winH) / 2;
		SetWindowPos(m_hWnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	}

	RECT clientRect{};
	if (GetClientRect(m_hWnd, &clientRect)) {
		m_Width = clientRect.right - clientRect.left;
		m_Height = clientRect.bottom - clientRect.top;
	} else {
		m_Width = width;
		m_Height = height;
	}
	m_Minimized = false;
	m_ShouldClose = false;
	return true;
}

void Win32Window::Destroy() {
	if (m_hWnd) {
		DestroyWindow(m_hWnd);
		m_hWnd = nullptr;
	}
    if (g_WindowInstance == this) {
        g_WindowInstance = nullptr;
    }
}

void Win32Window::PumpEvents() {
	MSG msg;
	while (PeekMessageW(&msg, nullptr, 0u, 0u, PM_REMOVE)) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
}

void Win32Window::SetCursorCaptured(bool captured) {
	if (m_Captured == captured) return;
	m_Captured = captured;
	if (captured) {
		// Hide cursor and confine it to the window; use relative mouse input
		ShowCursor(FALSE);
		RECT rect; GetClientRect(m_hWnd, &rect);
		POINT tl = { rect.left, rect.top };
		POINT br = { rect.right, rect.bottom };
		ClientToScreen(m_hWnd, &tl);
		ClientToScreen(m_hWnd, &br);
		RECT clip = { tl.x, tl.y, br.x, br.y };
		ClipCursor(&clip);
		// Center the cursor in window
		POINT center = { (tl.x + br.x) / 2, (tl.y + br.y) / 2 };
		SetCursorPos(center.x, center.y);
		// Remember logical center in client coords for Input::GetMousePosition
		float cx = (float)(rect.right - rect.left) * 0.5f;
		float cy = (float)(rect.bottom - rect.top) * 0.5f;
		Input::SetLockedCenter(cx, cy);
		// Enable raw input for high-precision relative motion
		RAWINPUTDEVICE rid{}; rid.usUsagePage = 0x01; rid.usUsage = 0x02; rid.dwFlags = RIDEV_INPUTSINK; rid.hwndTarget = m_hWnd;
		RegisterRawInputDevices(&rid, 1, sizeof(rid));
	} else {
		ClipCursor(nullptr);
		ShowCursor(TRUE);
		// Optional: unregister raw input
		RAWINPUTDEVICE rid{}; rid.usUsagePage = 0x01; rid.usUsage = 0x02; rid.dwFlags = RIDEV_REMOVE; rid.hwndTarget = nullptr;
		RegisterRawInputDevices(&rid, 1, sizeof(rid));
	}
}

void Win32Window::CenterCursorIfCaptured() {
	if (!m_Captured || !m_hWnd) return;
	
	// Re-center cursor each frame to prevent drift and ensure proper relative mode behavior
	RECT rect; GetClientRect(m_hWnd, &rect);
	POINT tl = { rect.left, rect.top };
	POINT br = { rect.right, rect.bottom };
	ClientToScreen(m_hWnd, &tl);
	ClientToScreen(m_hWnd, &br);
	
	// Center the cursor in window
	POINT center = { (tl.x + br.x) / 2, (tl.y + br.y) / 2 };
	SetCursorPos(center.x, center.y);
}

static KeyCode TranslateVirtualKey(int vk) {
	if (vk >= 'A' && vk <= 'Z')
		return static_cast<KeyCode>(vk);
	if (vk >= '0' && vk <= '9')
		return static_cast<KeyCode>(static_cast<int>(KeyCode::Digit0) + (vk - '0'));

	switch (vk) {
	case VK_SPACE:      return KeyCode::Space;
	case VK_OEM_1:      return KeyCode::Semicolon;
	case VK_OEM_PLUS:   return KeyCode::Equal;
	case VK_OEM_COMMA:  return KeyCode::Comma;
	case VK_OEM_MINUS:  return KeyCode::Minus;
	case VK_OEM_PERIOD: return KeyCode::Period;
	case VK_OEM_2:      return KeyCode::Slash;
	case VK_OEM_3:      return KeyCode::GraveAccent;
	case VK_OEM_4:      return KeyCode::LeftBracket;
	case VK_OEM_5:      return KeyCode::Backslash;
	case VK_OEM_6:      return KeyCode::RightBracket;
	case VK_OEM_7:      return KeyCode::Apostrophe;
	case VK_OEM_102:    return KeyCode::World2;

	case VK_ESCAPE:     return KeyCode::Escape;
	case VK_RETURN:     return KeyCode::Enter;
	case VK_TAB:        return KeyCode::Tab;
	case VK_BACK:       return KeyCode::Backspace;
	case VK_INSERT:     return KeyCode::Insert;
	case VK_DELETE:     return KeyCode::Delete;
	case VK_RIGHT:      return KeyCode::Right;
	case VK_LEFT:       return KeyCode::Left;
	case VK_DOWN:       return KeyCode::Down;
	case VK_UP:         return KeyCode::Up;
	case VK_PRIOR:      return KeyCode::PageUp;
	case VK_NEXT:       return KeyCode::PageDown;
	case VK_HOME:       return KeyCode::Home;
	case VK_END:        return KeyCode::End;
	case VK_CAPITAL:    return KeyCode::CapsLock;
	case VK_SCROLL:     return KeyCode::ScrollLock;
	case VK_NUMLOCK:    return KeyCode::NumLock;
	case VK_SNAPSHOT:   return KeyCode::PrintScreen;
	case VK_PAUSE:      return KeyCode::Pause;

	case VK_F1:  case VK_F2:  case VK_F3:  case VK_F4:
	case VK_F5:  case VK_F6:  case VK_F7:  case VK_F8:
	case VK_F9:  case VK_F10: case VK_F11: case VK_F12:
	case VK_F13: case VK_F14: case VK_F15: case VK_F16:
	case VK_F17: case VK_F18: case VK_F19: case VK_F20:
	case VK_F21: case VK_F22: case VK_F23: case VK_F24: {
		int offset = vk - VK_F1;
		return static_cast<KeyCode>(static_cast<int>(KeyCode::F1) + offset);
	}

	case VK_NUMPAD0: case VK_NUMPAD1: case VK_NUMPAD2: case VK_NUMPAD3:
	case VK_NUMPAD4: case VK_NUMPAD5: case VK_NUMPAD6: case VK_NUMPAD7:
	case VK_NUMPAD8: case VK_NUMPAD9: {
		int offset = vk - VK_NUMPAD0;
		return static_cast<KeyCode>(static_cast<int>(KeyCode::KP_0) + offset);
	}
	case VK_DECIMAL:   return KeyCode::KP_Decimal;
	case VK_DIVIDE:    return KeyCode::KP_Divide;
	case VK_MULTIPLY:  return KeyCode::KP_Multiply;
	case VK_SUBTRACT:  return KeyCode::KP_Subtract;
	case VK_ADD:       return KeyCode::KP_Add;
	case VK_SEPARATOR: return KeyCode::KP_Enter;
	case VK_OEM_NEC_EQUAL: return KeyCode::KP_Equal;

	case VK_LSHIFT:   return KeyCode::LeftShift;
	case VK_RSHIFT:   return KeyCode::RightShift;
	case VK_LCONTROL: return KeyCode::LeftControl;
	case VK_RCONTROL: return KeyCode::RightControl;
	case VK_LMENU:    return KeyCode::LeftAlt;
	case VK_RMENU:    return KeyCode::RightAlt;
	case VK_LWIN:     return KeyCode::LeftSuper;
	case VK_RWIN:     return KeyCode::RightSuper;
	case VK_APPS:     return KeyCode::Menu;
	default:          return KeyCode::Unknown;
	}
}

static KeyCode MapVirtualKeyToKeyCode(WPARAM wParam, LPARAM lParam) {
	int vk = static_cast<int>(wParam);
	UINT sc = static_cast<UINT>((lParam & 0x00FF0000) >> 16);
	bool extended = (lParam & (1u << 24)) != 0;

	if (vk == VK_SHIFT) {
		vk = MapVirtualKeyW(sc, MAPVK_VSC_TO_VK_EX);
	} else if (vk == VK_CONTROL) {
		vk = extended ? VK_RCONTROL : VK_LCONTROL;
	} else if (vk == VK_MENU) {
		vk = extended ? VK_RMENU : VK_LMENU;
	} else if (vk == VK_RETURN && extended) {
		vk = VK_SEPARATOR; // distinguish keypad enter
	}

	return TranslateVirtualKey(vk);
}

LRESULT CALLBACK Win32Window::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	// Forward input to ImGui
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
		return 1;

	switch (msg) {
		case WM_INPUT: {
			if (g_WindowInstance && g_WindowInstance->m_Captured) {
				UINT size = 0;
				GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
				if (size > 0) {
					std::vector<BYTE> buffer(size);
					if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER)) == size) {
						RAWINPUT* ri = reinterpret_cast<RAWINPUT*>(buffer.data());
						if (ri->header.dwType == RIM_TYPEMOUSE) {
							LONG dx = ri->data.mouse.lLastX;
							LONG dy = ri->data.mouse.lLastY;
							if (dx != 0 || dy != 0) {
								Input::OnMouseMove((double)dx, (double)dy);
							}
						}
					}
				}
				return 0;
			}
			break;
		}
		case WM_SIZE: {
			if (g_WindowInstance) {
				g_WindowInstance->m_Minimized = (wParam == SIZE_MINIMIZED);
				g_WindowInstance->m_Width = LOWORD(lParam);
				g_WindowInstance->m_Height = HIWORD(lParam);
				if (g_WindowInstance->m_OnResize) {
					g_WindowInstance->m_OnResize(g_WindowInstance->m_Width, g_WindowInstance->m_Height, g_WindowInstance->m_Minimized);
				}
			}
			return 0;
		}
		case WM_DPICHANGED: {
			RECT* const rc = reinterpret_cast<RECT*>(lParam);
			SetWindowPos(hWnd, nullptr, rc->left, rc->top,
					 rc->right - rc->left, rc->bottom - rc->top,
					 SWP_NOZORDER | SWP_NOACTIVATE);
			return 0;
		}
		case WM_MOUSEMOVE: {
			if (g_WindowInstance && g_WindowInstance->m_Captured) {
				// Ignore absolute mouse moves in captured mode; WM_INPUT provides deltas
				return 0;
			}
			int x = GET_X_LPARAM(lParam);
			int y = GET_Y_LPARAM(lParam);
			Input::OnMouseMove((double)x, (double)y);
			return 0;
		}
		case WM_LBUTTONDOWN: case WM_LBUTTONUP:
		case WM_RBUTTONDOWN: case WM_RBUTTONUP:
		case WM_MBUTTONDOWN: case WM_MBUTTONUP: {
			MouseButton button = MouseButton::Left;
			if (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) button = MouseButton::Right;
			else if (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP) button = MouseButton::Middle;
			InputAction action = (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN)
				? InputAction::Press
				: InputAction::Release;
			Input::OnMouseButton(button, action);
			return 0;
		}
		case WM_MOUSEWHEEL: {
			float dy = (float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA;
			Input::OnScroll(dy);
			return 0;
		}
		case WM_KEYDOWN: case WM_SYSKEYDOWN:
		case WM_KEYUP:   case WM_SYSKEYUP: {
			bool down = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
			KeyCode key = MapVirtualKeyToKeyCode(wParam, lParam);
			if (key != KeyCode::Unknown) {
				Input::OnKey(key, down ? InputAction::Press : InputAction::Release);
			}
			return 0;
		}
		case WM_CHAR: {
			// If needed later, route text input to ImGui or custom text system
			return 0;
		}
		case WM_CLOSE: {
			if (g_WindowInstance) g_WindowInstance->m_ShouldClose = true;
			PostQuitMessage(0);
			return 0;
		}
	}
	return DefWindowProcW(hWnd, msg, wParam, lParam);
}


void Win32Window::EnterFullscreen() {
	if (m_Fullscreen || m_hWnd == nullptr) return;

	// Save current window state
	m_SavedMaximized = !!IsZoomed(m_hWnd);
	if (m_SavedMaximized) {
		SendMessage(m_hWnd, WM_SYSCOMMAND, SC_RESTORE, 0);
	}
	m_SavedStyle = GetWindowLong(m_hWnd, GWL_STYLE);
	m_SavedExStyle = GetWindowLong(m_hWnd, GWL_EXSTYLE);
	GetWindowRect(m_hWnd, &m_SavedWindowRect);

	// Remove window decorations
	DWORD newStyle = m_SavedStyle & ~(WS_CAPTION | WS_THICKFRAME);
	DWORD newExStyle = m_SavedExStyle & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
	SetWindowLong(m_hWnd, GWL_STYLE, newStyle);
	SetWindowLong(m_hWnd, GWL_EXSTYLE, newExStyle);

	// Resize to monitor bounds
	HMONITOR hmon = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi{ sizeof(mi) };
	if (GetMonitorInfo(hmon, &mi)) {
		SetWindowPos(m_hWnd, nullptr,
			mi.rcMonitor.left, mi.rcMonitor.top,
			mi.rcMonitor.right - mi.rcMonitor.left,
			mi.rcMonitor.bottom - mi.rcMonitor.top,
			SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
	}

	m_Fullscreen = true;
}

void Win32Window::ExitFullscreen() {
	if (!m_Fullscreen || m_hWnd == nullptr) return;

	// Restore styles
	SetWindowLong(m_hWnd, GWL_STYLE, m_SavedStyle);
	SetWindowLong(m_hWnd, GWL_EXSTYLE, m_SavedExStyle);

	// Restore window size/pos
	SetWindowPos(m_hWnd, nullptr,
		m_SavedWindowRect.left, m_SavedWindowRect.top,
		m_SavedWindowRect.right - m_SavedWindowRect.left,
		m_SavedWindowRect.bottom - m_SavedWindowRect.top,
		SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

	if (m_SavedMaximized) {
		SendMessage(m_hWnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
	}

	m_Fullscreen = false;
}

void Win32Window::ToggleFullscreen() {
	if (m_Fullscreen) ExitFullscreen();
	else EnterFullscreen();
}


