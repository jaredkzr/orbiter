// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Input.h"
#include "OrbiterAPI.h"
#include "posix/OrbiterInput.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include <SDL3/SDL.h>

namespace {

	// SDL scancodes (USB HID usage) -> Orbiter key codes (PC set 1 scancodes)
	DWORD MapScancode (SDL_Scancode sc)
	{
		switch (sc) {
		case SDL_SCANCODE_ESCAPE: return OAPI_KEY_ESCAPE;
		case SDL_SCANCODE_1: return OAPI_KEY_1;
		case SDL_SCANCODE_2: return OAPI_KEY_2;
		case SDL_SCANCODE_3: return OAPI_KEY_3;
		case SDL_SCANCODE_4: return OAPI_KEY_4;
		case SDL_SCANCODE_5: return OAPI_KEY_5;
		case SDL_SCANCODE_6: return OAPI_KEY_6;
		case SDL_SCANCODE_7: return OAPI_KEY_7;
		case SDL_SCANCODE_8: return OAPI_KEY_8;
		case SDL_SCANCODE_9: return OAPI_KEY_9;
		case SDL_SCANCODE_0: return OAPI_KEY_0;
		case SDL_SCANCODE_MINUS: return OAPI_KEY_MINUS;
		case SDL_SCANCODE_EQUALS: return OAPI_KEY_EQUALS;
		case SDL_SCANCODE_BACKSPACE: return OAPI_KEY_BACK;
		case SDL_SCANCODE_TAB: return OAPI_KEY_TAB;
		case SDL_SCANCODE_Q: return OAPI_KEY_Q;
		case SDL_SCANCODE_W: return OAPI_KEY_W;
		case SDL_SCANCODE_E: return OAPI_KEY_E;
		case SDL_SCANCODE_R: return OAPI_KEY_R;
		case SDL_SCANCODE_T: return OAPI_KEY_T;
		case SDL_SCANCODE_Y: return OAPI_KEY_Y;
		case SDL_SCANCODE_U: return OAPI_KEY_U;
		case SDL_SCANCODE_I: return OAPI_KEY_I;
		case SDL_SCANCODE_O: return OAPI_KEY_O;
		case SDL_SCANCODE_P: return OAPI_KEY_P;
		case SDL_SCANCODE_LEFTBRACKET: return OAPI_KEY_LBRACKET;
		case SDL_SCANCODE_RIGHTBRACKET: return OAPI_KEY_RBRACKET;
		case SDL_SCANCODE_RETURN: return OAPI_KEY_RETURN;
		case SDL_SCANCODE_LCTRL: return OAPI_KEY_LCONTROL;
		case SDL_SCANCODE_A: return OAPI_KEY_A;
		case SDL_SCANCODE_S: return OAPI_KEY_S;
		case SDL_SCANCODE_D: return OAPI_KEY_D;
		case SDL_SCANCODE_F: return OAPI_KEY_F;
		case SDL_SCANCODE_G: return OAPI_KEY_G;
		case SDL_SCANCODE_H: return OAPI_KEY_H;
		case SDL_SCANCODE_J: return OAPI_KEY_J;
		case SDL_SCANCODE_K: return OAPI_KEY_K;
		case SDL_SCANCODE_L: return OAPI_KEY_L;
		case SDL_SCANCODE_SEMICOLON: return OAPI_KEY_SEMICOLON;
		case SDL_SCANCODE_APOSTROPHE: return OAPI_KEY_APOSTROPHE;
		case SDL_SCANCODE_GRAVE: return OAPI_KEY_GRAVE;
		case SDL_SCANCODE_LSHIFT: return OAPI_KEY_LSHIFT;
		case SDL_SCANCODE_BACKSLASH: return OAPI_KEY_BACKSLASH;
		case SDL_SCANCODE_Z: return OAPI_KEY_Z;
		case SDL_SCANCODE_X: return OAPI_KEY_X;
		case SDL_SCANCODE_C: return OAPI_KEY_C;
		case SDL_SCANCODE_V: return OAPI_KEY_V;
		case SDL_SCANCODE_B: return OAPI_KEY_B;
		case SDL_SCANCODE_N: return OAPI_KEY_N;
		case SDL_SCANCODE_M: return OAPI_KEY_M;
		case SDL_SCANCODE_COMMA: return OAPI_KEY_COMMA;
		case SDL_SCANCODE_PERIOD: return OAPI_KEY_PERIOD;
		case SDL_SCANCODE_SLASH: return OAPI_KEY_SLASH;
		case SDL_SCANCODE_RSHIFT: return OAPI_KEY_RSHIFT;
		case SDL_SCANCODE_KP_MULTIPLY: return OAPI_KEY_MULTIPLY;
		case SDL_SCANCODE_LALT: return OAPI_KEY_LALT;
		case SDL_SCANCODE_SPACE: return OAPI_KEY_SPACE;
		case SDL_SCANCODE_CAPSLOCK: return OAPI_KEY_CAPITAL;
		case SDL_SCANCODE_F1: return OAPI_KEY_F1;
		case SDL_SCANCODE_F2: return OAPI_KEY_F2;
		case SDL_SCANCODE_F3: return OAPI_KEY_F3;
		case SDL_SCANCODE_F4: return OAPI_KEY_F4;
		case SDL_SCANCODE_F5: return OAPI_KEY_F5;
		case SDL_SCANCODE_F6: return OAPI_KEY_F6;
		case SDL_SCANCODE_F7: return OAPI_KEY_F7;
		case SDL_SCANCODE_F8: return OAPI_KEY_F8;
		case SDL_SCANCODE_F9: return OAPI_KEY_F9;
		case SDL_SCANCODE_F10: return OAPI_KEY_F10;
		case SDL_SCANCODE_NUMLOCKCLEAR: return OAPI_KEY_NUMLOCK;
		case SDL_SCANCODE_SCROLLLOCK: return OAPI_KEY_SCROLL;
		case SDL_SCANCODE_KP_7: return OAPI_KEY_NUMPAD7;
		case SDL_SCANCODE_KP_8: return OAPI_KEY_NUMPAD8;
		case SDL_SCANCODE_KP_9: return OAPI_KEY_NUMPAD9;
		case SDL_SCANCODE_KP_MINUS: return OAPI_KEY_SUBTRACT;
		case SDL_SCANCODE_KP_4: return OAPI_KEY_NUMPAD4;
		case SDL_SCANCODE_KP_5: return OAPI_KEY_NUMPAD5;
		case SDL_SCANCODE_KP_6: return OAPI_KEY_NUMPAD6;
		case SDL_SCANCODE_KP_PLUS: return OAPI_KEY_ADD;
		case SDL_SCANCODE_KP_1: return OAPI_KEY_NUMPAD1;
		case SDL_SCANCODE_KP_2: return OAPI_KEY_NUMPAD2;
		case SDL_SCANCODE_KP_3: return OAPI_KEY_NUMPAD3;
		case SDL_SCANCODE_KP_0: return OAPI_KEY_NUMPAD0;
		case SDL_SCANCODE_KP_PERIOD: return OAPI_KEY_DECIMAL;
		case SDL_SCANCODE_NONUSBACKSLASH: return OAPI_KEY_OEM_102;
		case SDL_SCANCODE_F11: return OAPI_KEY_F11;
		case SDL_SCANCODE_F12: return OAPI_KEY_F12;
		case SDL_SCANCODE_KP_ENTER: return OAPI_KEY_NUMPADENTER;
		case SDL_SCANCODE_RCTRL: return OAPI_KEY_RCONTROL;
		case SDL_SCANCODE_KP_DIVIDE: return OAPI_KEY_DIVIDE;
		case SDL_SCANCODE_PRINTSCREEN: return OAPI_KEY_SYSRQ;
		case SDL_SCANCODE_RALT: return OAPI_KEY_RALT;
		case SDL_SCANCODE_PAUSE: return OAPI_KEY_PAUSE;
		case SDL_SCANCODE_HOME: return OAPI_KEY_HOME;
		case SDL_SCANCODE_UP: return OAPI_KEY_UP;
		case SDL_SCANCODE_PAGEUP: return OAPI_KEY_PRIOR;
		case SDL_SCANCODE_LEFT: return OAPI_KEY_LEFT;
		case SDL_SCANCODE_RIGHT: return OAPI_KEY_RIGHT;
		case SDL_SCANCODE_END: return OAPI_KEY_END;
		case SDL_SCANCODE_DOWN: return OAPI_KEY_DOWN;
		case SDL_SCANCODE_PAGEDOWN: return OAPI_KEY_NEXT;
		case SDL_SCANCODE_INSERT: return OAPI_KEY_INSERT;
		case SDL_SCANCODE_DELETE: return OAPI_KEY_DELETE;
		default: return 0;
		}
	}

	UINT MouseButtonEvent (Uint8 button, bool down)
	{
		switch (button) {
		case SDL_BUTTON_LEFT:  return down ? WM_LBUTTONDOWN : WM_LBUTTONUP;
		case SDL_BUTTON_RIGHT: return down ? WM_RBUTTONDOWN : WM_RBUTTONUP;
		default:               return 0;
		}
	}

} // namespace

bool input::ProcessEvents (SDL_Window *window)
{
	bool open = true;

	// cursor capture while Orbiter is in camera rotation mode
	bool capture = oapiInputMouseCaptured ();
	if (capture != SDL_GetWindowRelativeMouseMode (window))
		SDL_SetWindowRelativeMouseMode (window, capture);

	SDL_Event ev;
	while (SDL_PollEvent (&ev)) {
		if (ImGui::GetCurrentContext())
			ImGui_ImplSDL3_ProcessEvent (&ev);

		switch (ev.type) {
		case SDL_EVENT_QUIT:
		case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
			oapiInputCloseRequest ();
			open = false;
			break;
		case SDL_EVENT_WINDOW_FOCUS_GAINED:
			oapiInputFocus (true);
			break;
		case SDL_EVENT_WINDOW_FOCUS_LOST:
			oapiInputFocus (false);
			break;
		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP:
			if (!ev.key.repeat) {
				DWORD key = MapScancode (ev.key.scancode);
				if (key) oapiInputKey (key, ev.type == SDL_EVENT_KEY_DOWN);
			}
			break;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP: {
			UINT msg = MouseButtonEvent (ev.button.button, ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
			if (msg) oapiInputMouseButton (msg, (int)ev.button.x, (int)ev.button.y);
			} break;
		case SDL_EVENT_MOUSE_MOTION: {
			// relative motion is fractional; carry the remainder so slow drags aren't lost
			static float remX = 0.0f, remY = 0.0f;
			remX += ev.motion.xrel, remY += ev.motion.yrel;
			int dx = (int)remX, dy = (int)remY;
			remX -= dx, remY -= dy;
			oapiInputMouseMove ((int)ev.motion.x, (int)ev.motion.y, dx, dy);
			} break;
		case SDL_EVENT_MOUSE_WHEEL: {
			float mx, my;
			SDL_GetMouseState (&mx, &my);
			oapiInputMouseWheel ((int)(ev.wheel.y * 120.0f), (int)mx, (int)my);
			} break;
		}
	}
	return open;
}
