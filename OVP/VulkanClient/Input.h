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

} // namespace input

#endif // !__INPUT_H_VKCLIENT
