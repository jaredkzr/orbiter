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

#endif // !_WIN32

#endif // !__ORBITERINPUT_H
