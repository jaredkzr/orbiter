// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Window events: passes SDL input to ImGui and the Orbiter core
// =======================================================================

#ifndef __INPUT_H_VKCLIENT
#define __INPUT_H_VKCLIENT

struct SDL_Window;

namespace input {

	/// Process all pending window events. Returns false if the window was closed.
	bool ProcessEvents (SDL_Window *window);

	/// Attach a virtual gamepad held in a fixed state (tests):
	/// "lx,ly,rx,ry,lt,rt[,buttons]" (axes -1 to 1, triggers 0 to 1; buttons
	/// as a bit mask of OAPI_PAD_xxx)
	void AttachTestGamepad (const char *spec);

} // namespace input

#endif // !__INPUT_H_VKCLIENT
