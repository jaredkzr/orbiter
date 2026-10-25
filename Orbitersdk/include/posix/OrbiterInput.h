// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// User input interface for graphics clients on non-Windows platforms.
//
// On Windows, Orbiter reads the keyboard and joystick via DirectInput and
// receives mouse events through the render window's message procedure.
// Elsewhere, the graphics client owns the render window and its event
// queue, and passes user input to Orbiter with these functions. They must
// be called from the simulation thread (e.g. from clbkUpdate).
// =======================================================================

#ifndef __ORBITERINPUT_H
#define __ORBITERINPUT_H

#include "OrbiterAPI.h"

#ifndef _WIN32

/**
 * \brief Report a key press or release.
 * \param key key code (OAPI_KEY_xxx)
 * \param down true if the key was pressed, false if released
 */
OAPIFUNC void oapiInputKey (DWORD key, bool down);

/**
 * \brief Report a mouse button event.
 * \param event WM_LBUTTONDOWN, WM_LBUTTONUP, WM_RBUTTONDOWN or WM_RBUTTONUP
 * \param x, y mouse position in the render window [pixel]
 */
OAPIFUNC void oapiInputMouseButton (UINT event, int x, int y);

/**
 * \brief Report mouse motion.
 * \param x, y new mouse position in the render window [pixel]
 * \param dx, dy relative motion [pixel]. This is also reported while the
 *   cursor is captured (camera rotation mode), when x and y are fixed.
 */
OAPIFUNC void oapiInputMouseMove (int x, int y, int dx, int dy);

/**
 * \brief Report mouse wheel rotation.
 * \param delta rotation in Windows units (120 per notch, positive away from the user)
 * \param x, y mouse position in the render window [pixel]
 */
OAPIFUNC void oapiInputMouseWheel (int delta, int x, int y);

/**
 * \brief Report a change of the render window's input focus.
 */
OAPIFUNC void oapiInputFocus (bool active);

/**
 * \brief Request the end of the simulation session (e.g. the render window was closed).
 */
OAPIFUNC void oapiInputCloseRequest ();

/**
 * \brief Returns true while Orbiter wants the mouse captured, i.e. the
 *   cursor hidden and relative motion reported (camera rotation mode).
 */
OAPIFUNC bool oapiInputMouseCaptured ();

/// Game controller buttons (OAPI_CONTROLLERSTATE::buttons of a gamepad)
enum {
	OAPI_PAD_A, OAPI_PAD_B, OAPI_PAD_X, OAPI_PAD_Y,
	OAPI_PAD_BACK, OAPI_PAD_GUIDE, OAPI_PAD_START,
	OAPI_PAD_LSTICK, OAPI_PAD_RSTICK, OAPI_PAD_LSHOULDER, OAPI_PAD_RSHOULDER,
	OAPI_PAD_DPAD_UP, OAPI_PAD_DPAD_DOWN, OAPI_PAD_DPAD_LEFT, OAPI_PAD_DPAD_RIGHT,
	OAPI_PAD_NBUTTON
};

/// Gamepad axes (OAPI_CONTROLLERSTATE::axis of a gamepad)
enum {
	OAPI_PAD_LEFTX, OAPI_PAD_LEFTY, OAPI_PAD_RIGHTX, OAPI_PAD_RIGHTY,
	OAPI_PAD_LTRIGGER, OAPI_PAD_RTRIGGER
};

/// State of a game controller
struct OAPI_CONTROLLERSTATE {
	/// A gamepad (standard layout: axes and buttons OAPI_PAD_xxx), or a
	/// joystick (axes X, Y, Z, Rx, Ry, Rz, slider 0, slider 1 and numbered buttons)
	bool gamepad;
	/// Axis positions, -1 to 1 (gamepad triggers 0 to 1; stick Y axes
	/// positive towards the user)
	float axis[8];
	int naxis;
	/// Button states, one bit per button
	unsigned int buttons[4];
	/// Hat switch: -1 centred, else the direction in hundredths of a
	/// degree clockwise from forward
	int hat;
};

/**
 * \brief Report the connected game controllers (on startup and whenever
 *   one is added or removed). Their order defines the joystick device
 *   index of the configuration (index i is device i+1).
 * \param n number of controllers
 * \param names product names
 */
OAPIFUNC void oapiInputControllers (int n, const char *const *names);

/**
 * \brief Report the state of a game controller (once per frame).
 * \param idx controller index (see oapiInputControllers)
 */
OAPIFUNC void oapiInputControllerState (int idx, const OAPI_CONTROLLERSTATE *state);

#endif // !_WIN32

#endif // !__ORBITERINPUT_H
