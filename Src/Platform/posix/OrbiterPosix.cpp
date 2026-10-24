// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Platform-specific parts of class Orbiter for non-Windows platforms.
//
// There is no launchpad: the scenario given on the command line
// (-s / --scenario) is launched directly. With a graphics client (loaded
// as a plugin, e.g. -p VulkanClient), the client owns the render window
// and passes user input to Orbiter (posix/OrbiterInput.h). Without one,
// Orbiter runs as a headless server controlled through the console.
// =======================================================================

#include "Orbiter.h"
#include "Log.h"
#include "State.h"
#include "Script.h"
#include "Mfd.h"
#include "Memstat.h"
#include "console_ng.h"
#include "Camera.h"
#include "Select.h"
#include "Vessel.h"
#include "Platform.h"
#include "posix/OrbiterInput.h"
#include "imgui.h"
#include <cstdio>
#include <string>
#include <thread>

extern const TCHAR *MasterConfigFile;
extern Orbiter *g_pOrbiter;
extern Camera *g_camera;
extern Vessel *g_focusobj;
extern Select *g_select;
extern InputBox *g_input;

// =======================================================================

HRESULT Orbiter::Create (HINSTANCE hInstance)
{
	hInst = hInstance;
	bWINEenv = false;

	// parameter manager - parses from master config file
	pConfig->Load (MasterConfigFile);
	strcpy (cfgpath, pConfig->CfgDirPrm.ConfigDir);   cfglen = strlen (cfgpath);

	HRESULT hr;
	if (FAILED (hr = pDI->Create (hInstance))) return hr;

	// Read key mapping from file (or write default keymap)
	if (!keymap.Read ("keymap.cfg")) keymap.Write ("keymap.cfg");

	pState = new State(); TRACENEW

	if (pConfig->CfgCmdlinePrm.bFastExit)
		SetFastExit (true);

	Instrument::RegisterBuiltinModes();

	script = new ScriptInterface(this); TRACENEW

	// preload modules from command line requests
	LoadModules ("Modules\\Plugin", pConfig->CfgCmdlinePrm.LoadPlugins);

	// preload active plugin modules
	LoadModules ("Modules\\Plugin", pConfig->GetActiveModules());

	// preload startup plugin modules
	LoadStartupModules();

	bSysClearType = false;
	memstat = new MemStat;

	return S_OK;
}

//-----------------------------------------------------------------------------
// Name: Run()
// Desc: Run the simulation session for the scenario requested on the command
//       line until the session is closed.
//-----------------------------------------------------------------------------
INT Orbiter::Run ()
{
	if (pConfig->CfgCmdlinePrm.LaunchScenario.empty()) {
		fprintf (stderr, "Orbiter: no scenario specified. Use -s <scenario> or --scenario=<scenario>.\n"
		                 "Run with --help for a list of options.\n");
		return 1;
	}

	memset (platformKstate, 0, sizeof(platformKstate));
	bPlatformClose = false;

	// Without a graphics client, the console controls the session (there is no launchpad)
	Launch (pConfig->CfgCmdlinePrm.LaunchScenario.c_str());

	while (bSession) {
		if (bAllowInput) bActive = true, bAllowInput = false;
		if (BeginTimeStep (bRunning)) {
			UpdateWorld();
			EndTimeStep (bRunning); // the graphics client processes window events in clbkUpdate
			if (bVisible) {
				if (bActive) UserInput ();
				Render3DEnvironment ();
			}
			if (bRunning && bCapture) {
				CaptureVideoFrame ();
			}
		}
		if (m_pConsole)
			m_pConsole->ParseCmd();
		if (bPlatformClose) { // render window closed
			bPlatformClose = false;
			PreCloseSession ();
			CloseSession ();
		} else if (!bRunning && !bVisible) // don't spin while paused
			std::this_thread::sleep_for (std::chrono::milliseconds (10));
	}
	return 0;
}

void Orbiter::ScreenToClient (POINT *pt) const
{
}

void Orbiter::TerminateOnError ()
{
	LogOut (">>> TERMINATING <<<");
	fprintf (stderr, "Orbiter: terminating after critical error. See Orbiter.log for details.\n");
	exit (1);
}

void Orbiter::UpdateServerWnd (HWND hWnd)
{
}

void Orbiter::InitRotationMode ()
{
	bKeepFocus = true;
}

void Orbiter::ExitRotationMode ()
{
	bKeepFocus = false;
}

//-----------------------------------------------------------------------------
// Name: UserInput()
// Desc: Process keyboard input passed in by the graphics client, in the same
//       way as DirectInput keyboard data on Windows. Mouse events are handled
//       when they are reported (PlatformMouse).
//-----------------------------------------------------------------------------
HRESULT Orbiter::UserInput ()
{
	DWORD i;
	bool skipkbd = false;

	memset(simkstate, 0, 256);
	for (i = 0; i < 15; i++) ctrlKeyboard[i] = ctrlJoystick[i] = 0; // reset keyboard and joystick attitude requests

	// skip keyboard if dialogs are open
	if ((g_input && g_input->IsActive()) ||
	    (g_select && g_select->IsActive())) skipkbd = true;

	ImGuiIO& io = ImGui::GetIO();
	bool imguiWantsKeyboard = io.WantCaptureKeyboard &&
		(pConfig->CfgUIPrm.MouseFocusMode == 0 || io.WantCaptureMouse);

	// immediate key interpretation
	char buffer[256];
	memcpy (buffer, platformKstate, 256);
	if (!imguiWantsKeyboard)
		for (i = 0; i < 256; i++)
			simkstate[i] |= buffer[i];
	bool consume = BroadcastImmediateKeyboardEvent (simkstate);
	if (!skipkbd && !consume) {
		KbdInputImmediate_System (simkstate);
		if (bRunning) KbdInputImmediate_OnRunning (simkstate);
	}

	// buffered key events
	DWORD nItems = (DWORD)platformKbuf.size();
	if (nItems && !imguiWantsKeyboard) {
		BroadcastBufferedKeyboardEvent (buffer, platformKbuf.data(), nItems);
		if (!skipkbd) {
			KbdInputBuffered_System (buffer, platformKbuf.data(), nItems);
			if (bRunning) KbdInputBuffered_OnRunning (buffer, platformKbuf.data(), nItems);
		}
	}
	platformKbuf.clear();

	for (i = 0; i < 15; i++) ctrlTotal[i] = ctrlKeyboard[i]; // update attitude requests

	g_camera->UpdateMouse();

	// apply manual attitude control
	g_focusobj->ApplyUserAttitudeControls (ctrlTotal);

	return S_OK;
}

// =======================================================================
// User input passed in by the graphics client

void Orbiter::PlatformKey (DWORD key, bool down)
{
	if (key >= 256) return;
	platformKstate[key] = (down ? (char)0x80 : 0);
	DIDEVICEOBJECTDATA dod = {};
	dod.dwOfs = key;
	dod.dwData = (down ? 0x80 : 0);
	dod.dwTimeStamp = timeGetTime();
	platformKbuf.push_back (dod);
}

void Orbiter::PlatformMouse (UINT event, DWORD state, int x, int y)
{
	if (!bSession) return;
	if (event != WM_MOUSEWHEEL)
		orbiter::platform::SetMousePos (x, y);
	ImGuiIO& io = ImGui::GetIO();
	if (io.WantCaptureMouse && !bKeepFocus) return;
	MouseEvent (event, state, x, y);
}

DLLEXPORT void oapiInputKey (DWORD key, bool down)
{
	g_pOrbiter->PlatformKey (key, down);
}

DLLEXPORT void oapiInputMouseButton (UINT event, int x, int y)
{
	g_pOrbiter->PlatformMouse (event, 0, x, y);
}

DLLEXPORT void oapiInputMouseMove (int x, int y, int dx, int dy)
{
	orbiter::platform::AddMouseMotion (dx, dy);
	g_pOrbiter->PlatformMouse (WM_MOUSEMOVE, 0, x, y);
}

DLLEXPORT void oapiInputMouseWheel (int delta, int x, int y)
{
	g_pOrbiter->PlatformMouse (WM_MOUSEWHEEL, MAKEWPARAM (0, (WORD)(short)delta), x, y);
}

DLLEXPORT void oapiInputFocus (bool active)
{
	g_pOrbiter->PlatformFocus (active);
}

DLLEXPORT void oapiInputCloseRequest ()
{
	g_pOrbiter->PlatformCloseRequest ();
}

DLLEXPORT bool oapiInputMouseCaptured ()
{
	return g_pOrbiter->PlatformMouseCaptured ();
}

bool Orbiter::ActivateRoughType ()
{
	return false;
}

bool Orbiter::DeactivateRoughType ()
{
	return false;
}
