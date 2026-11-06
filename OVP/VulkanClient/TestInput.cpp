// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "TestInput.h"
#include "OrbiterAPI.h"
#include "posix/OrbiterInput.h"
#include <cstdlib>
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

	// split off the first word ("a quoted one" may have spaces)
	std::string Word (std::string &s)
	{
		s = Trim (s);
		if (!s.empty() && s[0] == '"') {
			size_t q = s.find ('"', 1);
			if (q != std::string::npos) {
				std::string w = s.substr (1, q - 1);
				s = Trim (s.substr (q + 1));
				return w;
			}
		}
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

	// Lua helpers of the test interpreter: check (tests), __tojson and
	// __state (interactive: results as JSON, the flight's state)
	const char *luaPrelude = R"LUA(
__tfail = 0
function check (name, cond, msg)
	oapi.write_log (string.format ('TestInput: %s %s %s', cond and 'PASS' or 'FAIL', name, msg or ''))
	if not cond then __tfail = __tfail + 1 end
end
function __tojson (v)
	local t = type (v)
	if v == nil then return 'null'
	elseif t == 'boolean' then return tostring (v)
	elseif t == 'number' then
		if v ~= v or v == math.huge or v == -math.huge then return 'null' end
		return string.format ('%.10g', v)
	elseif t == 'string' then
		local esc = {['"'] = '\\"', ['\\'] = '\\\\', ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t'}
		return '"' .. (v:gsub ('[%c"\\]', function (c) return esc[c] or string.format ('\\u%04x', c:byte ()) end)) .. '"'
	elseif t == 'table' then
		local n = #v
		local a = {}
		if n > 0 then
			for i = 1, n do a[i] = __tojson (v[i]) end
			return '[' .. table.concat (a, ',') .. ']'
		end
		for k, x in pairs (v) do a[#a + 1] = __tojson (tostring (k)) .. ':' .. __tojson (x) end
		table.sort (a)
		return '{' .. table.concat (a, ',') .. '}'
	end
	return __tojson (tostring (v))
end
function __state ()
	local function try (f, ...) local ok, r = pcall (f, ...) if ok then return r end end
	local s = {}
	s.simt = try (oapi.get_simtime); s.mjd = try (oapi.get_simmjd)
	s.tacc = try (oapi.get_tacc); s.paused = try (oapi.get_pause)
	local ok, m, cam = pcall (oapi.get_cameramode); if ok then s.camera = cam end
	s.cockpit = try (oapi.cockpit_mode); s.mapview = try (oapi.get_mapview)
	s.hud = try (oapi.get_hudmode)
	s.mfd = {left = try (oapi.get_mfdmode, 0), right = try (oapi.get_mfdmode, 1)}
	local v = vessel.get_focusinterface ()
	if v then
		local d = {}
		d.name = try (v.get_name, v); d.class = try (v.get_classname, v)
		d.altitude = try (v.get_altitude, v); d.airspeed = try (v.get_airspeed, v)
		d.groundspeed = try (v.get_groundspeed, v)
		local h = try (v.get_horizonairspeedvector, v); if h then d.vspeed = h.y end
		local p, b, y = try (v.get_pitch, v), try (v.get_bank, v), try (v.get_yaw, v)
		if p then d.pitch = math.deg (p) end; if b then d.bank = math.deg (b) end; if y then d.yaw = math.deg (y) end
		local fs = try (v.get_flightstatus, v); if fs then d.landed = (fs % 2 == 1) end
		if THGROUP then
			d.main = try (v.get_thrustergrouplevel, v, THGROUP.MAIN)
			d.retro = try (v.get_thrustergrouplevel, v, THGROUP.RETRO)
			d.hover = try (v.get_thrustergrouplevel, v, THGROUP.HOVER)
		end
		local n, pm, px = try (v.get_propellantcount, v) or 0, 0, 0
		for i = 0, n - 1 do
			local ph = v:get_propellanthandle (i)
			pm = pm + (try (v.get_propellantmass, v, ph) or 0); px = px + (try (v.get_propellantmaxmass, v, ph) or 0)
		end
		d.propellant = pm; d.propellant_max = px; d.mass = try (v.get_mass, v)
		local g = try (v.get_gravityref, v); if g then d.gravity_ref = oapi.get_objname (g) end
		local sr = try (v.get_surfaceref, v); if sr then d.surface_ref = oapi.get_objname (sr) end
		d.rcs = try (v.get_rcsmode, v); d.adc = try (v.get_adcmode, v)
		local nav = {}
		for name, id in pairs (NAVMODE or {}) do if try (v.get_navmode, v, id) then nav[#nav + 1] = name end end
		table.sort (nav); d.navmodes = nav
		s.vessel = d
	end
	return s
end
)LUA";

	// a JSON string
	std::string Json (const std::string &s)
	{
		std::string r = "\"";
		for (unsigned char c : s) {
			if (c == '"' || c == '\\') r += '\\', r += (char)c;
			else if (c == '\n') r += "\\n";
			else if (c < 0x20) { char b[8]; snprintf (b, sizeof (b), "\\u%04x", c); r += b; }
			else r += (char)c;
		}
		return r + "\"";
	}

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
	// offset "+dx,dy" (after the last term); quotes around it dropped
	std::string s = Trim (spec);
	if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr (1, s.size() - 2);
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
	if (reply) { // (interactive: answered with the error, the rest of the command skipped)
		if (error.empty()) error = msg;
		skip = true;
		return;
	}
	oapiWriteLogError ("TestInput: FAIL %s", msg.c_str());
	oapiExitOrbiter (1);
}

void TestInput::SetInteractive (Reply r, const std::string &dir)
{
	reply = r;
	evalPath = dir + "/lua_result.txt";
	timeout = 5.0;
}

void TestInput::Append (const std::string &line, int id)
{
	std::string l = Trim (line);
	if (!l.empty()) lines.push_back (l);
	lines.push_back ("__reply " + std::to_string (id));
}

bool TestInput::Exec (const std::string &code, std::string &out)
{
	if (!interp) {
		interp = oapiCreateInterpreter ();
		if (!interp) { Fail ("no script interpreter"); return false; }
		oapiExecScriptCmd (interp, luaPrelude);
	}
	remove (evalPath.c_str());
	std::string chunk = "local __f = io.open (" + Json (evalPath) + ", 'w') "
		"local ok, r = pcall (function () " + code + "\nend) "
		"if ok then __f:write ('1', __tojson (r)) else __f:write ('0', tostring (r)) end "
		"__f:close ()";
	oapiExecScriptCmd (interp, chunk.c_str());
	std::ifstream f (evalPath);
	std::stringstream ss;
	if (f) ss << f.rdbuf();
	std::string s = ss.str();
	if (s.empty()) { Fail ("Lua: the code did not compile (see Orbiter.log)"); return false; }
	if (s[0] != '1') { Fail ("Lua: " + s.substr (1)); return false; }
	out = s.substr (1);
	return true;
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

void TestInput::Step (SDL_Window *window, bool instantOnly)
{
	if (done) return;
	if (waitFrames > 0) {
		if (!instantOnly) waitFrames--;
		return;
	}
	while (pc < lines.size()) {
		if (skip) { // (interactive: the rest of a failed command, up to its reply)
			skip = false;
			blockedSince = 0;
			while (pc < lines.size() && lines[pc].compare (0, 8, "__reply ")) pc++;
			continue;
		}
		std::string args = lines[pc];
		std::string cmd = Word (args);
		if (instantOnly && cmd != "__reply" && cmd != "lua" && cmd != "eval" && cmd != "state"
			&& cmd != "anchors" && cmd != "widgets" && cmd != "timeout")
			return; // (the rest in the frame)

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
			result = Json (args);
			pc++;
			return;
		} else if (cmd == "lua") {
			if (reply) { if (!Exec (args, result)) continue; }
			else Lua (args);
		} else if (cmd == "eval") {
			if (!Exec ("return " + args, result)) continue;
		} else if (cmd == "state") {
			if (!Exec ("return __state ()", result)) continue;
		} else if (cmd == "anchors") {
			int n = oapiInputUiAnchorList (args.c_str(), nullptr, 0);
			std::string list (n + 1, '\0');
			oapiInputUiAnchorList (args.c_str(), &list[0], n + 1);
			list.resize (n);
			// {"name": [x, y], ...}
			std::string j = "{";
			std::istringstream in (list);
			std::string name;
			float x, y;
			while (in >> name >> x >> y) {
				char b[64];
				snprintf (b, sizeof (b), ":[%.0f,%.0f]", x, y);
				j += (j.size() > 1 ? "," : "") + Json (name) + b;
			}
			result = j + "}";
		} else if (cmd == "widgets") {
			int n = oapiInputUiWidgetList (args.c_str(), nullptr, 0);
			std::string list (n + 1, '\0');
			oapiInputUiWidgetList (args.c_str(), &list[0], n + 1);
			list.resize (n);
			// [{"label":..., "window":..., "x":..., "y":..., "w":..., "h":..., "state":...}, ...]
			std::string j = "[";
			std::istringstream in (list);
			std::string line;
			while (std::getline (in, line)) {
				size_t t1 = line.find ('\t'), t2 = line.find ('\t', t1 + 1), t3 = line.find ('\t', t2 + 1);
				if (t3 == std::string::npos) continue;
				float x, y, w, h;
				if (sscanf (line.c_str(), "%f %f %f %f", &x, &y, &w, &h) != 4) continue;
				char b[96];
				snprintf (b, sizeof (b), ",\"x\":%.0f,\"y\":%.0f,\"w\":%.0f,\"h\":%.0f", x, y, w, h);
				std::string state = line.substr (t3 + 1);
				j += (j.size() > 1 ? "," : "") + std::string ("{\"label\":") + Json (line.substr (t2 + 1, t3 - t2 - 1))
					+ ",\"window\":" + Json (line.substr (t1 + 1, t2 - t1 - 1)) + b
					+ (state.empty() ? std::string() : ",\"state\":" + Json (state)) + "}";
			}
			result = j + "]";
		} else if (cmd == "__reply") {
			if (reply) reply (atoi (args.c_str()), error.empty(), error.empty() ? result : error);
			result.clear(), error.clear();
		} else if (cmd == "exit") {
			Lua ("oapi.exit (__tfail > 0 and 1 or 0)");
		} else {
			Fail ("unknown command '" + lines[pc] + "'");
			return;
		}
		pc++, blockedSince = 0;
	}
	if (reply) return; // (interactive: more to come)
	oapiWriteLogV ("TestInput: script finished");
	done = true;
}
