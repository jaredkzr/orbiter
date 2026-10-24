// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// ==================================================================================
// Custom dialog controls (gauges, switches) for non-Windows platforms.
// These controls only exist in Win32 dialog boxes, which are not available
// here, so no control can ever be passed to these functions. They are
// provided so that modules using the DlgCtrl API can be built and loaded.
// ==================================================================================

#include "DlgCtrl.h"

void oapiRegisterCustomControls (HINSTANCE hInst)
{
}

void oapiUnregisterCustomControls (HINSTANCE hInst)
{
}

LRESULT FAR PASCAL MsgProc_Gauge (HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	return 0;
}

LRESULT FAR PASCAL MsgProc_Switch (HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	return 0;
}

void oapiSetGaugeParams (HWND hCtrl, GAUGEPARAM *gp, bool redraw)
{
}

void oapiSetGaugeRange (HWND hCtrl, int rmin, int rmax, bool redraw)
{
}

int oapiSetGaugePos (HWND hCtrl, int pos, bool redraw)
{
	return 0;
}

int oapiIncGaugePos (HWND hCtrl, int dpos, bool redraw)
{
	return 0;
}

int oapiGetGaugePos (HWND hCtrl)
{
	return 0;
}

void oapiSetSwitchParams (HWND hCtrl, SWITCHPARAM *sp, bool redraw)
{
}

int oapiSetSwitchState (HWND hCtrl, int state, bool redraw)
{
	return 0;
}

int oapiGetSwitchState (HWND hCtrl)
{
	return 0;
}
