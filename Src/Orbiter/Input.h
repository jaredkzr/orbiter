// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// DirectInput user interface class
// =======================================================================

#ifndef __INPUT_H
#define __INPUT_H

#include "Di7frame.h"
#ifndef _WIN32
#include "posix/OrbiterInput.h"
#include <vector>
#endif

class Orbiter;

class DInput {
	friend class Orbiter;

public:
	DInput (Orbiter *pOrbiter);
	~DInput ();

	HRESULT Create (HINSTANCE hInst);
	void Destroy ();

	void SetRenderWindow(HWND hWnd);

	bool CreateKbdDevice();
	bool CreateJoyDevice ();
	void DestroyDevices ();

	inline CDIFramework7 *GetDIFrame() const { return diframe; }
	inline const LPDIRECTINPUTDEVICE8 GetKbdDevice() const { return diframe->GetKbdDevice(); }
	inline const LPDIRECTINPUTDEVICE8 GetJoyDevice() const { return diframe->GetJoyDevice(); }

	void OptionChanged(DWORD cat, DWORD item);

	bool PollJoystick (DIJOYSTATE2 *js);

	struct JoyProp {
		bool bThrottle;  // joystick has throttle control
		bool bRudder;    // joystick has rudder control
		int ThrottleOfs; // throttle data offset
	};

#ifndef _WIN32
	// Game controllers reported by the graphics client (posix/OrbiterInput.h)
	void GetJoysticks (DIDEVICEINSTANCE **dev, DWORD *count);
	DWORD NumJoysticks () const { return (DWORD)devices.size(); }
	void SetControllers (int n, const char *const *names);
	void SetControllerState (int idx, const OAPI_CONTROLLERSTATE *state);
	/// the selected controller, if it is a connected gamepad
	const OAPI_CONTROLLERSTATE *Gamepad () const;
#endif

protected:
	HRESULT SetJoystickProperties ();

private:
	Orbiter *orbiter;
	CDIFramework7 *diframe;
	JoyProp joyprop;
	HWND m_hWnd;
#ifndef _WIN32
	const OAPI_CONTROLLERSTATE *Selected () const;
	std::vector<DIDEVICEINSTANCE> devices;
	std::vector<OAPI_CONTROLLERSTATE> states;
	std::vector<bool> stateValid;
#endif
};

#endif // !__INPUT_H