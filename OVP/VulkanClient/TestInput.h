// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Scripted input for tests (ORBITER_VK_TEST_INPUT=<file>): mouse and
// keyboard events are pushed into the SDL event queue, so they take the
// same path as real input (ImGui and the Orbiter core), aimed at named
// elements of the user interface (oapiInputUiAnchor). Checks are Lua
// commands run in an interpreter of the session (they also run while the
// simulation is paused).
//
// One command per line ('#' starts a comment):
//   wait <frames>              let frames pass
//   waitfor <pos>              until the position can be resolved
//   timeout <seconds>          time to wait for a position (default 30)
//   move <pos>                 mouse motion
//   down|up [left|right]       mouse button
//   click <pos> [left|right]   move, press, release
//   drag <pos> <pos> [frames]  press at the first position, move to the
//                              second in steps, release
//   wheel <notches>            mouse wheel at the cursor
//   key <name>                 key press and release (SDL key names: M, F5, Delete, ...)
//   keydown|keyup <name>       (modifiers: Left Shift, Left Ctrl)
//   screenshot <file>          capture the next frame
//   lua <code>                 run Lua code; check(name, cond, msg) logs
//                              PASS/FAIL and counts failures
//   exit                       end Orbiter: exit code 1 if a check failed
//
// Each mouse or key event takes a frame. A position is "x,y" [pixels],
// "@anchor", "@a->@b*f" (from a towards b, f times the distance), each
// with an optional offset "+dx,dy". A position that cannot be resolved
// (the element is not drawn) holds the script until it can, or fails the
// test after the timeout.
// =======================================================================

#ifndef __TESTINPUT_H_VKCLIENT
#define __TESTINPUT_H_VKCLIENT

#include <string>
#include <vector>

struct SDL_Window;

class TestInput {
public:
	/// Load the script; false if it can't be read
	bool Load (const char *path);
	/// Run the script up to the next frame (before the window events are processed)
	void Step (SDL_Window *window);
	/// A screenshot was requested for this frame (path, else empty); clears the request
	std::string TakeScreenshotRequest ();
	/// End of the session: the interpreter is gone
	void SessionEnd () { interp = nullptr; }

private:
	bool Resolve (const std::string &spec, float &x, float &y) const;
	bool Lua (const std::string &code);
	void Fail (const std::string &msg);
	void MouseMove (SDL_Window *window, float x, float y);
	void MouseButton (SDL_Window *window, int button, bool down);
	void Key (SDL_Window *window, const std::string &name, bool down);

	std::vector<std::string> lines;
	size_t pc = 0;          // next line
	int waitFrames = 0;
	unsigned long long blockedSince = 0; // the current line waits for a position since [ms] (0: not waiting)
	double timeout = 30.0;   // [s]
	float mx = 0, my = 0;   // mouse position
	void *interp = nullptr;
	std::string screenshot;
	bool done = false;
};

#endif // !__TESTINPUT_H_VKCLIENT
