// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "TestInput.h"
#include "OrbiterAPI.h"
#include "posix/OrbiterInput.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

OAPIFUNC void oapiExitOrbiter (int code); // implemented in the Orbiter core

namespace {

	std::string Trim (const std::string &s)
	{
		size_t a = s.find_first_not_of (" \t\r\n"), b = s.find_last_not_of (" \t\r\n");
		return a == std::string::npos ? std::string() : s.substr (a, b - a + 1);
	}

	// split off the first word
	std::string Word (std::string &s)
	{
		s = Trim (s);
		size_t e = s.find_first_of (" \t");
		std::string w = s.substr (0, e);
		s = (e == std::string::npos ? std::string() : Trim (s.substr (e)));
		return w;
	}

	// "x,y" or "@anchor"
	bool Term (const std::string &t, float &x, float &y)
	{
		if (t.empty()) return false;
		if (t[0] == '@') return oapiInputUiAnchor (t.c_str() + 1, &x, &y);
		return sscanf (t.c_str(), "%f,%f", &x, &y) == 2;
	}

	// Lua helpers of the test interpreter
	const char *luaPrelude =
		"__tfail = 0 "
		"function check (name, cond, msg) "
		"  oapi.write_log (string.format ('TestInput: %s %s %s', cond and 'PASS' or 'FAIL', name, msg or '')) "
		"  if not cond then __tfail = __tfail + 1 end "
		"end";

} // namespace

bool TestInput::Load (const char *path)
{
	std::ifstream f (path);
	if (!f) {
		oapiWriteLogError ("TestInput: cannot read %s", path);
		return false;
	}
	std::string l;
	while (std::getline (f, l)) {
		size_t c = l.find ('#');
		if (c != std::string::npos) l.erase (c);
		l = Trim (l);
		if (!l.empty()) lines.push_back (l);
	}
	oapiWriteLogV ("TestInput: %d commands from %s", (int)lines.size(), path);
	return true;
}

std::string TestInput::TakeScreenshotRequest ()
{
	std::string s;
	s.swap (screenshot);
	return s;
}

bool TestInput::Resolve (const std::string &spec, float &x, float &y) const
{
	// offset "+dx,dy" (after the last term)
	std::string s = spec;
	float dx = 0, dy = 0;
	size_t arrow = s.find ("->");
	size_t from = (arrow == std::string::npos ? 1 : arrow + 3);
	size_t off = s.find_first_of ("+", from);
	if (off != std::string::npos) {
		if (sscanf (s.c_str() + off + 1, "%f,%f", &dx, &dy) != 2) return false;
		s.erase (off);
	}
	if (arrow == std::string::npos) {
		if (!Term (s, x, y)) return false;
	} else {
		// "a->b*f"
		std::string a = s.substr (0, arrow), b = s.substr (arrow + 2);
		float f = 1.0f;
		size_t star = b.find ('*');
		if (star != std::string::npos) f = (float)atof (b.c_str() + star + 1), b.erase (star);
		float xa, ya, xb, yb;
		if (!Term (a, xa, ya) || !Term (b, xb, yb)) return false;
		x = xa + (xb - xa) * f, y = ya + (yb - ya) * f;
	}
	x += dx, y += dy;
	return true;
}

bool TestInput::Lua (const std::string &code)
{
	if (!interp) {
		interp = oapiCreateInterpreter ();
		if (!interp) {
			Fail ("no script interpreter");
			return false;
		}
		oapiExecScriptCmd (interp, luaPrelude);
	}
	// (errors are logged and count as failures)
	std::string chunk = "local ok, err = pcall (function () " + code + "\nend) "
		"if not ok then oapi.write_log ('TestInput: FAIL lua ' .. tostring (err)); __tfail = __tfail + 1 end";
	oapiExecScriptCmd (interp, chunk.c_str());
	return true;
}

void TestInput::Fail (const std::string &msg)
{
	oapiWriteLogError ("TestInput: FAIL %s", msg.c_str());
	oapiExitOrbiter (1);
}

void TestInput::MouseMove (SDL_Window *window, float x, float y)
{
	SDL_Event e;
	SDL_zero (e);
	e.type = SDL_EVENT_MOUSE_MOTION;
	e.motion.timestamp = SDL_GetTicksNS ();
	e.motion.windowID = SDL_GetWindowID (window);
	e.motion.x = x, e.motion.y = y;
	e.motion.xrel = x - mx, e.motion.yrel = y - my;
	SDL_PushEvent (&e);
	mx = x, my = y;
}

void TestInput::MouseButton (SDL_Window *window, int button, bool down)
{
	SDL_Event e;
	SDL_zero (e);
	e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
	e.button.timestamp = SDL_GetTicksNS ();
	e.button.windowID = SDL_GetWindowID (window);
	e.button.button = (Uint8)button;
	e.button.down = down;
	e.button.clicks = 1;
	e.button.x = mx, e.button.y = my;
	SDL_PushEvent (&e);
}

void TestInput::Key (SDL_Window *window, const std::string &name, bool down)
{
	SDL_Scancode sc = SDL_GetScancodeFromName (name.c_str());
	if (sc == SDL_SCANCODE_UNKNOWN) {
		Fail ("unknown key " + name);
		return;
	}
	static SDL_Keymod mod = SDL_KMOD_NONE;
	SDL_Keymod m = 0;
	switch (sc) {
	case SDL_SCANCODE_LSHIFT: m = SDL_KMOD_LSHIFT; break;
	case SDL_SCANCODE_RSHIFT: m = SDL_KMOD_RSHIFT; break;
	case SDL_SCANCODE_LCTRL:  m = SDL_KMOD_LCTRL; break;
	case SDL_SCANCODE_RCTRL:  m = SDL_KMOD_RCTRL; break;
	case SDL_SCANCODE_LALT:   m = SDL_KMOD_LALT; break;
	case SDL_SCANCODE_RALT:   m = SDL_KMOD_RALT; break;
	default: break;
	}
	if (down) mod |= m; else mod &= ~m;
	SDL_Event e;
	SDL_zero (e);
	e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
	e.key.timestamp = SDL_GetTicksNS ();
	e.key.windowID = SDL_GetWindowID (window);
	e.key.scancode = sc;
	e.key.key = SDL_GetKeyFromScancode (sc, SDL_KMOD_NONE, false);
	e.key.mod = mod;
	e.key.down = down;
	SDL_PushEvent (&e);
}

void TestInput::Step (SDL_Window *window)
{
	if (done) return;
	if (waitFrames > 0) {
		waitFrames--;
		return;
	}
	while (pc < lines.size()) {
		std::string args = lines[pc];
		std::string cmd = Word (args);

		// commands that need positions wait until they can be resolved
		auto Pos = [&](const std::string &spec, float &x, float &y) {
			if (Resolve (spec, x, y)) return true;
			Uint64 now = SDL_GetTicks ();
			if (!blockedSince) blockedSince = now;
			else if (now - blockedSince > (Uint64)(timeout * 1000.0)) Fail ("line '" + lines[pc] + "': " + spec + " not found");
			return false;
		};
		auto Button = [&](const std::string &b) { return b == "right" ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT; };

		if (cmd == "wait") {
			pc++;
			waitFrames = std::max (0, atoi (args.c_str()) - 1);
			return;
		} else if (cmd == "timeout") {
			timeout = atof (args.c_str());
		} else if (cmd == "waitfor") {
			float x, y;
			if (!Pos (args, x, y)) return;
		} else if (cmd == "move") {
			float x, y;
			if (!Pos (args, x, y)) return;
			MouseMove (window, x, y);
			pc++, blockedSince = 0;
			return;
		} else if (cmd == "down" || cmd == "up") {
			MouseButton (window, Button (args), cmd == "down");
			pc++;
			return;
		} else if (cmd == "click") {
			std::string spec = Word (args);
			float x, y;
			if (!Pos (spec, x, y)) return;
			std::string b = args.empty() ? "left" : args;
			char mv[64];
			snprintf (mv, sizeof(mv), "move %g,%g", x, y);
			lines[pc] = mv;
			lines.insert (lines.begin() + pc + 1, {"down " + b, "up " + b});
			continue;
		} else if (cmd == "drag") {
			std::string s0 = Word (args), s1 = Word (args);
			int n = args.empty() ? 20 : std::max (1, atoi (args.c_str()));
			float x0, y0, x1, y1;
			if (!Pos (s0, x0, y0) || !Pos (s1, x1, y1)) return;
			std::vector<std::string> seq;
			char mv[64];
			snprintf (mv, sizeof(mv), "move %g,%g", x0, y0);
			seq.push_back (mv);
			seq.push_back ("down left");
			for (int i = 1; i <= n; i++) {
				snprintf (mv, sizeof(mv), "move %g,%g", x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n);
				seq.push_back (mv);
			}
			seq.push_back ("up left");
			lines.erase (lines.begin() + pc);
			lines.insert (lines.begin() + pc, seq.begin(), seq.end());
			continue;
		} else if (cmd == "wheel") {
			SDL_Event e;
			SDL_zero (e);
			e.type = SDL_EVENT_MOUSE_WHEEL;
			e.wheel.timestamp = SDL_GetTicksNS ();
			e.wheel.windowID = SDL_GetWindowID (window);
			e.wheel.y = (float)atof (args.c_str());
			e.wheel.mouse_x = mx, e.wheel.mouse_y = my;
			SDL_PushEvent (&e);
			pc++;
			return;
		} else if (cmd == "key") {
			Key (window, args, true);
			lines[pc] = "keyup " + args;
			return;
		} else if (cmd == "keydown" || cmd == "keyup") {
			Key (window, args, cmd == "keydown");
			pc++;
			return;
		} else if (cmd == "screenshot") {
			screenshot = args;
			pc++;
			return;
		} else if (cmd == "lua") {
			Lua (args);
		} else if (cmd == "exit") {
			Lua ("oapi.exit (__tfail > 0 and 1 or 0)");
		} else {
			Fail ("unknown command '" + lines[pc] + "'");
			return;
		}
		pc++, blockedSince = 0;
	}
	oapiWriteLogV ("TestInput: script finished");
	done = true;
}
