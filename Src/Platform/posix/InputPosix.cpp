// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// User input device class for non-Windows platforms.
// DirectInput is not available: the graphics client owns the devices and
// passes their input to Orbiter (posix/OrbiterInput.h). Keyboard events
// arrive through Orbiter::PlatformKey; game controllers are reported here
// and presented to the core as the DirectInput joystick state it expects
// (axes -1000 to 1000, throttle -1000 to 0).
// =======================================================================

#include "Input.h"
#include "Orbiter.h"
#include "UiAnchor.h"
#include <cmath>
#include <cstring>

extern Orbiter *g_pOrbiter;

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
	return true; // keyboard input is passed in by the graphics client
}

bool DInput::CreateJoyDevice ()
{
	SetJoystickProperties ();
	return Selected () != NULL;
}

void DInput::DestroyDevices ()
{
}

void DInput::OptionChanged (DWORD cat, DWORD)
{
	if (cat == OPTCAT_JOYSTICK)
		SetJoystickProperties ();
}

// =======================================================================
// Game controllers

void DInput::GetJoysticks (DIDEVICEINSTANCE **dev, DWORD *count)
{
	*dev = devices.data();
	*count = (DWORD)devices.size();
}

void DInput::SetControllers (int n, const char *const *names)
{
	devices.assign (n, DIDEVICEINSTANCE{});
	for (int i = 0; i < n; i++) {
		devices[i].dwSize = sizeof(DIDEVICEINSTANCE);
		strncpy (devices[i].tszProductName, names[i], MAX_PATH-1);
		strncpy (devices[i].tszInstanceName, names[i], MAX_PATH-1);
	}
	states.assign (n, OAPI_CONTROLLERSTATE{});
	stateValid.assign (n, false);
	SetJoystickProperties ();
}

void DInput::SetControllerState (int idx, const OAPI_CONTROLLERSTATE *state)
{
	if (idx < 0 || idx >= (int)states.size() || !state) return;
	states[idx] = *state;
	if (!stateValid[idx]) {
		stateValid[idx] = true;
		SetJoystickProperties (); // (axis count known now)
	}
}

const OAPI_CONTROLLERSTATE *DInput::Selected () const
{
	DWORD idx = orbiter->Cfg()->CfgJoystickPrm.Joy_idx;
	if (idx < 1 || idx > states.size() || !stateValid[idx-1]) return NULL;
	return &states[idx-1];
}

const OAPI_CONTROLLERSTATE *DInput::Gamepad () const
{
	const OAPI_CONTROLLERSTATE *s = Selected ();
	return (s && s->gamepad) ? s : NULL;
}

HRESULT DInput::SetJoystickProperties ()
{
	const OAPI_CONTROLLERSTATE *s = Selected ();
	const CFG_JOYSTICKPRM &prm = orbiter->Cfg()->CfgJoystickPrm;
	joyprop.bThrottle = false;
	joyprop.bRudder = false;
	joyprop.ThrottleOfs = 0;
	if (!s || s->gamepad) return S_OK; // (gamepad throttle: triggers, see Orbiter::GamepadInput)

	DIJOYSTATE2 js;
	static const int thAxis[4] = { -1, 2, 6, 7 }; // keyboard only, Z, slider 0, slider 1
	int ax = (prm.ThrottleAxis < 4 ? thAxis[prm.ThrottleAxis] : -1);
	if (ax >= 0 && ax < s->naxis) {
		joyprop.bThrottle = true;
		joyprop.ThrottleOfs = (ax == 2 ? (BYTE*)&js.lZ : (BYTE*)&js.rglSlider[ax-6]) - (BYTE*)&js;
	}
	joyprop.bRudder = (s->naxis > 5);
	return S_OK;
}

bool DInput::PollJoystick (DIJOYSTATE2 *js)
{
	const OAPI_CONTROLLERSTATE *s = Selected ();
	if (!s) return false;
	const CFG_JOYSTICKPRM &prm = orbiter->Cfg()->CfgJoystickPrm;

	// axes with the configured dead zone, rescaled to the DirectInput range
	double dz = std::min (0.95, prm.Deadzone * 1e-4);
	auto Axis = [&](int i) -> LONG {
		if (i >= s->naxis) return 0;
		double v = s->axis[i], a = fabs (v);
		if (a <= dz) return 0;
		return (LONG)lround (copysign ((std::min (a, 1.0) - dz) / (1.0 - dz), v) * 1000.0);
	};
	auto Button = [&](int i) { return (s->buttons[i >> 5] >> (i & 31)) & 1u; };

	memset (js, 0, sizeof(DIJOYSTATE2));
	for (int i = 0; i < 4; i++) js->rgdwPOV[i] = 0xFFFFFFFF; // centred

	if (s->gamepad) {
		// left stick: pitch and bank; shoulder buttons: yaw. The right stick,
		// triggers and buttons are handled by Orbiter::GamepadInput.
		js->lX = Axis (OAPI_PAD_LEFTX);
		js->lY = Axis (OAPI_PAD_LEFTY);
		js->lRz = (LONG)(Button (OAPI_PAD_RSHOULDER) - Button (OAPI_PAD_LSHOULDER)) * 1000;
		return true;
	}

	js->lX = Axis (0), js->lY = Axis (1), js->lZ = Axis (2);
	js->lRx = Axis (3), js->lRy = Axis (4), js->lRz = Axis (5);
	js->rglSlider[0] = Axis (6), js->rglSlider[1] = Axis (7);
	if (joyprop.bThrottle) {
		// throttle range -1000 (full) to 0, the first `saturation` of the
		// travel covering the full range
		LONG *th = (LONG*)((BYTE*)js + joyprop.ThrottleOfs);
		int ax = (th == &js->lZ ? 2 : th == &js->rglSlider[0] ? 6 : 7);
		double sat = std::max (0.05, prm.ThrottleSaturation * 1e-4);
		double f = std::min (1.0, (1.0 - s->axis[ax]) * 0.5 / sat); // 1 at the forward stop
		*th = -(LONG)lround (f * 1000.0);
	}
	for (int i = 0; i < 128; i++)
		js->rgbButtons[i] = Button (i) ? 0x80 : 0;
	if (s->hat >= 0) js->rgdwPOV[0] = (DWORD)s->hat;
	return true;
}

DLLEXPORT void oapiInputControllers (int n, const char *const *names)
{
	g_pOrbiter->GetInput()->SetControllers (n, names);
}

DLLEXPORT void oapiInputControllerState (int idx, const OAPI_CONTROLLERSTATE *state)
{
	g_pOrbiter->GetInput()->SetControllerState (idx, state);
}

DLLEXPORT bool oapiInputUiAnchor (const char *name, float *x, float *y)
{
	return uianchor::Get (name, *x, *y);
}
