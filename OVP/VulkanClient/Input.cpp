// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Input.h"
#include "OrbiterAPI.h"
#include "posix/OrbiterInput.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

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

	// ------------------------------------------------------------------
	// Game controllers: gamepads (standard layout) and other joysticks, in
	// the order they were connected

	struct Controller {
		SDL_JoystickID id;
		SDL_Gamepad *pad = nullptr;
		SDL_Joystick *joy = nullptr;
		std::string name;
	};
	std::vector<Controller> controllers;
	bool controllersChanged = true;

	// virtual test gamepad (AttachTestGamepad): its state is set again before
	// each update, since SDL resets triggers and buttons when it is opened
	struct {
		SDL_Joystick *joy = nullptr;
		Sint16 axis[6];
		unsigned int buttons = 0;
	} testPad;

	void UpdateTestGamepad ()
	{
		if (!testPad.joy) return;
		for (int a = 0; a < 6; a++)
			SDL_SetJoystickVirtualAxis (testPad.joy, a, testPad.axis[a]);
		for (int b = 0; b < OAPI_PAD_NBUTTON; b++)
			SDL_SetJoystickVirtualButton (testPad.joy, b, (testPad.buttons >> b) & 1);
	}

	void AddController (SDL_JoystickID id)
	{
		for (auto &c : controllers) if (c.id == id) return;
		Controller c;
		c.id = id;
		if (SDL_IsGamepad (id)) {
			if (!(c.pad = SDL_OpenGamepad (id))) return;
			const char *n = SDL_GetGamepadName (c.pad);
			c.name = n ? n : "Gamepad";
		} else {
			if (!(c.joy = SDL_OpenJoystick (id))) return;
			const char *n = SDL_GetJoystickName (c.joy);
			c.name = n ? n : "Joystick";
		}
		oapiWriteLogV ("VulkanClient: controller connected: %s (%s)", c.name.c_str(), c.pad ? "gamepad" : "joystick");
		controllers.push_back (c);
		controllersChanged = true;
	}

	void RemoveController (SDL_JoystickID id)
	{
		for (auto it = controllers.begin(); it != controllers.end(); ++it) {
			if (it->id != id) continue;
			oapiWriteLogV ("VulkanClient: controller disconnected: %s", it->name.c_str());
			if (it->pad) SDL_CloseGamepad (it->pad);
			if (it->joy) SDL_CloseJoystick (it->joy);
			controllers.erase (it);
			controllersChanged = true;
			return;
		}
	}

	// Gamepad cursor: the right stick's button switches it on and off; while
	// on, the right stick moves the mouse pointer, A and B are the left and
	// right mouse buttons and the d-pad's up and down turn the wheel. These
	// don't reach Orbiter's gamepad functions meanwhile (camera, A/B, d-pad).
	struct PadCursor {
		bool on = false;
		bool prevToggle = false, prevA = false, prevB = false, prevUp = false, prevDown = false;
		float x = -1.0f, y = -1.0f;
		Uint64 lastTicks = 0;
	} padCursor;

	void PushMotion (SDL_Window *window, float x, float y, float dx, float dy)
	{
		SDL_Event e;
		SDL_zero (e);
		e.type = SDL_EVENT_MOUSE_MOTION;
		e.motion.timestamp = SDL_GetTicksNS ();
		e.motion.windowID = SDL_GetWindowID (window);
		e.motion.x = x, e.motion.y = y;
		e.motion.xrel = dx, e.motion.yrel = dy;
		SDL_PushEvent (&e);
	}

	void PushButton (SDL_Window *window, Uint8 button, bool down, float x, float y)
	{
		SDL_Event e;
		SDL_zero (e);
		e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
		e.button.timestamp = SDL_GetTicksNS ();
		e.button.windowID = SDL_GetWindowID (window);
		e.button.button = button;
		e.button.down = down;
		e.button.clicks = 1;
		e.button.x = x, e.button.y = y;
		SDL_PushEvent (&e);
	}

	void PushWheel (SDL_Window *window, float w, float x, float y)
	{
		SDL_Event e;
		SDL_zero (e);
		e.type = SDL_EVENT_MOUSE_WHEEL;
		e.wheel.timestamp = SDL_GetTicksNS ();
		e.wheel.windowID = SDL_GetWindowID (window);
		e.wheel.y = w;
		e.wheel.mouse_x = x, e.wheel.mouse_y = y;
		SDL_PushEvent (&e);
	}

	void UpdatePadCursor (SDL_Window *window, OAPI_CONTROLLERSTATE &s)
	{
		PadCursor &c = padCursor;
		auto Btn = [&](int b) { return (s.buttons[b >> 5] >> (b & 31)) & 1u; };
		auto Clear = [&](int b) { s.buttons[b >> 5] &= ~(1u << (b & 31)); };
		bool toggle = Btn (OAPI_PAD_RSTICK);
		if (toggle && !c.prevToggle) {
			c.on = !c.on;
			if (c.on) {
				int w, h;
				SDL_GetWindowSize (window, &w, &h);
				float mx, my;
				SDL_GetMouseState (&mx, &my);
				c.x = (mx > 0.0f || my > 0.0f) ? mx : 0.5f * w;
				c.y = (mx > 0.0f || my > 0.0f) ? my : 0.5f * h;
				c.lastTicks = SDL_GetTicks ();
			}
			oapiWriteLogV ("VulkanClient: gamepad cursor %s", c.on ? "on" : "off");
		}
		c.prevToggle = toggle;
		Clear (OAPI_PAD_RSTICK);
		if (!c.on) return;

		Uint64 now = SDL_GetTicks ();
		float dt = std::min (0.1f, (now - c.lastTicks) * 1e-3f);
		c.lastTicks = now;
		auto Axis = [](float v) { float a = std::fabs (v); return a < 0.15f ? 0.0f : std::copysign ((a - 0.15f) / 0.85f, v); };
		float ax = Axis (s.axis[OAPI_PAD_RIGHTX]), ay = Axis (s.axis[OAPI_PAD_RIGHTY]);
		if (ax || ay) {
			int w, h;
			SDL_GetWindowSize (window, &w, &h);
			const float speed = 1.2f * h; // [pixels/s] at full deflection (squared response)
			float dx = ax * std::fabs (ax) * speed * dt, dy = ay * std::fabs (ay) * speed * dt;
			float nx = std::clamp (c.x + dx, 0.0f, (float)w - 1.0f), ny = std::clamp (c.y + dy, 0.0f, (float)h - 1.0f);
			if (nx != c.x || ny != c.y) {
				if (SDL_GetWindowFlags (window) & SDL_WINDOW_INPUT_FOCUS) SDL_WarpMouseInWindow (window, nx, ny); // (the visible cursor)
				else PushMotion (window, nx, ny, nx - c.x, ny - c.y);
				c.x = nx, c.y = ny;
			}
		}
		bool a = Btn (OAPI_PAD_A), b = Btn (OAPI_PAD_B), up = Btn (OAPI_PAD_DPAD_UP), down = Btn (OAPI_PAD_DPAD_DOWN);
		if (a != c.prevA) PushButton (window, SDL_BUTTON_LEFT, a, c.x, c.y);
		if (b != c.prevB) PushButton (window, SDL_BUTTON_RIGHT, b, c.x, c.y);
		if (up && !c.prevUp) PushWheel (window, 1.0f, c.x, c.y);
		if (down && !c.prevDown) PushWheel (window, -1.0f, c.x, c.y);
		c.prevA = a, c.prevB = b, c.prevUp = up, c.prevDown = down;
		// (hidden from Orbiter's gamepad functions)
		s.axis[OAPI_PAD_RIGHTX] = s.axis[OAPI_PAD_RIGHTY] = 0.0f;
		Clear (OAPI_PAD_A), Clear (OAPI_PAD_B), Clear (OAPI_PAD_DPAD_UP), Clear (OAPI_PAD_DPAD_DOWN);
	}

	void ReportControllers (SDL_Window *window)
	{
		if (controllersChanged) {
			std::vector<const char*> names;
			for (auto &c : controllers) names.push_back (c.name.c_str());
			oapiInputControllers ((int)names.size(), names.data());
			controllersChanged = false;
		}
		for (size_t i = 0; i < controllers.size(); i++) {
			const Controller &c = controllers[i];
			OAPI_CONTROLLERSTATE s{};
			s.hat = -1;
			auto SetButton = [&](int b, bool down) { if (down) s.buttons[b >> 5] |= 1u << (b & 31); };
			if (c.pad) {
				static const SDL_GamepadButton buttons[OAPI_PAD_NBUTTON] = {
					SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
					SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_START,
					SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
					SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
					SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
					SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
				};
				static const SDL_GamepadAxis axes[6] = {
					SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX,
					SDL_GAMEPAD_AXIS_RIGHTY, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
				};
				s.gamepad = true;
				s.naxis = 6;
				for (int a = 0; a < 6; a++)
					s.axis[a] = std::max (-1.0f, SDL_GetGamepadAxis (c.pad, axes[a]) / 32767.0f);
				for (int b = 0; b < OAPI_PAD_NBUTTON; b++)
					SetButton (b, SDL_GetGamepadButton (c.pad, buttons[b]));
				if (i == 0) UpdatePadCursor (window, s);
			} else {
				s.naxis = std::min (8, SDL_GetNumJoystickAxes (c.joy));
				for (int a = 0; a < s.naxis; a++)
					s.axis[a] = std::max (-1.0f, SDL_GetJoystickAxis (c.joy, a) / 32767.0f);
				int nb = std::min (128, SDL_GetNumJoystickButtons (c.joy));
				for (int b = 0; b < nb; b++)
					SetButton (b, SDL_GetJoystickButton (c.joy, b));
				if (SDL_GetNumJoystickHats (c.joy) > 0) {
					static const struct { Uint8 mask; int dir; } hats[] = {
						{SDL_HAT_UP, 0}, {SDL_HAT_RIGHTUP, 4500}, {SDL_HAT_RIGHT, 9000}, {SDL_HAT_RIGHTDOWN, 13500},
						{SDL_HAT_DOWN, 18000}, {SDL_HAT_LEFTDOWN, 22500}, {SDL_HAT_LEFT, 27000}, {SDL_HAT_LEFTUP, 31500},
					};
					Uint8 h = SDL_GetJoystickHat (c.joy, 0);
					for (auto &x : hats) if (h == x.mask) s.hat = x.dir;
				}
			}
			oapiInputControllerState ((int)i, &s);
		}
	}

} // namespace

void input::AttachTestGamepad (const char *spec)
{
	float v[6] = {};
	unsigned int buttons = 0;
	sscanf (spec, "%f,%f,%f,%f,%f,%f,%u", v, v+1, v+2, v+3, v+4, v+5, &buttons);

	SDL_VirtualJoystickDesc desc;
	SDL_INIT_INTERFACE(&desc);
	desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
	desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
	desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
	desc.name = "Virtual test gamepad";
	SDL_JoystickID id = SDL_AttachVirtualJoystick (&desc);
	SDL_Joystick *joy = id ? SDL_OpenJoystick (id) : nullptr;
	if (!joy) {
		oapiWriteLogError ("VulkanClient: could not attach a virtual gamepad: %s", SDL_GetError());
		return;
	}
	// (raw axis values -32768 to 32767; the triggers' rest position is -32768)
	for (int a = 0; a < 6; a++) {
		float r = (a < 4) ? v[a] * 32767.0f : v[a] * 65535.0f - 32768.0f;
		testPad.axis[a] = (Sint16)std::clamp (r, -32768.0f, 32767.0f);
	}
	testPad.buttons = buttons;
	testPad.joy = joy; // (stays open)
	UpdateTestGamepad ();
	oapiWriteLogV ("VulkanClient: virtual test gamepad attached (%s)", spec);
}

bool input::ProcessEvents (SDL_Window *window)
{
	bool open = true;

	// cursor capture while Orbiter is in camera rotation mode
	bool capture = oapiInputMouseCaptured ();
	if (capture != SDL_GetWindowRelativeMouseMode (window))
		SDL_SetWindowRelativeMouseMode (window, capture);

	UpdateTestGamepad ();
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
			oapiInputMouseWheel ((int)(ev.wheel.y * 120.0f), (int)ev.wheel.mouse_x, (int)ev.wheel.mouse_y);
			} break;
		case SDL_EVENT_JOYSTICK_ADDED:
			AddController (ev.jdevice.which);
			break;
		case SDL_EVENT_JOYSTICK_REMOVED:
			RemoveController (ev.jdevice.which);
			break;
		}
	}
	ReportControllers (window);
	return open;
}
