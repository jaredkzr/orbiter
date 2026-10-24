// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// User input device class for non-Windows platforms.
// DirectInput is not available, so no keyboard or joystick devices are
// created. The simulation still receives key events that are injected
// through the Orbiter API (e.g. from scripts).
// =======================================================================

#include "Input.h"
#include "Orbiter.h"

DInput::DInput (Orbiter *pOrbiter)
{
	orbiter = pOrbiter;
	diframe = NULL;
	m_hWnd = NULL;
	joyprop.bThrottle = false;
	joyprop.bRudder = false;
	joyprop.ThrottleOfs = 0;
}

DInput::~DInput ()
{
}

HRESULT DInput::Create (HINSTANCE)
{
	return S_OK;
}

void DInput::Destroy ()
{
}

void DInput::SetRenderWindow (HWND hWnd)
{
	m_hWnd = hWnd;
}

bool DInput::CreateKbdDevice ()
{
	return false;
}

bool DInput::CreateJoyDevice ()
{
	return false;
}

void DInput::DestroyDevices ()
{
}

void DInput::OptionChanged (DWORD, DWORD)
{
}

bool DInput::PollJoystick (DIJOYSTATE2 *)
{
	return false;
}

HRESULT DInput::SetJoystickProperties ()
{
	return S_OK;
}
