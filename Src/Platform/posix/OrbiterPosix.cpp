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
#include "LaunchpadPosix.h"
#include "DlgMgr.h"
#include "Keymap.h"
#include "Replay.h"
#include "imgui.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>

extern const TCHAR *MasterConfigFile;
extern Orbiter *g_pOrbiter;
extern Camera *g_camera;
extern Vessel *g_focusobj;
extern Select *g_select;
extern InputBox *g_input;
extern TimeData td;

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

	// the launchpad (no scenario on the command line) needs a graphics client
	if (!gclient && pConfig->CfgCmdlinePrm.LaunchScenario.empty())
		LoadModule ("Modules\\Plugin", "VulkanClient");

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
	// Without a scenario on the command line, the launchpad selects one (in
	// the graphics client's window); sessions then return to it.
	std::string scenario = pConfig->CfgCmdlinePrm.LaunchScenario;
	bool launchpad = scenario.empty();
	if (launchpad && !gclient) {
		fprintf (stderr, "Orbiter: no scenario specified. Use -s <scenario> or --scenario=<scenario>,\n"
		                 "or load a graphics client (-p VulkanClient) for the launchpad.\n"
		                 "Run with --help for a list of options.\n");
		return 1;
	}

	memset (platformKstate, 0, sizeof(platformKstate));
	bPlatformClose = false;

	int nsession = 0;
	do {
		if (launchpad && !RunLaunchpad (scenario)) break;
		RunSession (scenario);
		if (launchpad) printf ("Launchpad: session %d closed\n", ++nsession);
	} while (launchpad && !bFastExit);

	if (m_pPosixLaunchpad) {
		delete m_pPosixLaunchpad;
		m_pPosixLaunchpad = nullptr;
	}
	pConfig->Write ();
	return 0;
}

//-----------------------------------------------------------------------------
// Name: RunSession()
// Desc: Run a simulation session until it is closed. Without a graphics
//       client, the console controls the session.
//-----------------------------------------------------------------------------
void Orbiter::RunSession (const std::string &scenario)
{
	Launch (scenario.c_str());

	while (bSession) {
		if (bAllowInput) bActive = true, bAllowInput = false;
		if (BeginTimeStep (bRunning)) {
			UpdateWorld();
			EndTimeStep (bRunning); // the graphics client processes window events in clbkUpdate
			if (!bSession) break;   // (closed on reaching a session limit)
			if (bVisible) {
				if (bActive) UserInput ();
				Render3DEnvironment (replay && replay->Exporting()); // (no user interface in exported frames)
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
}

//-----------------------------------------------------------------------------
// Name: RunLaunchpad()
// Desc: Show the launchpad until the user launches a scenario (returned in
//       `scenario`) or exits (returns false)
//-----------------------------------------------------------------------------
namespace {
	// the launchpad as a dialog of the dialog manager, which runs the ImGui frame
	class LaunchpadDlg : public ImGuiDialog {
	public:
		LaunchpadDlg (PosixLaunchpad *lp): ImGuiDialog ("Launchpad"), lp(lp) { active = true; }
		void Display () override { action = lp->Draw (); }
		void OnDraw () override {}
		PosixLaunchpad *lp;
		PosixLaunchpad::Action action = PosixLaunchpad::NONE;
	};
}

bool Orbiter::RunLaunchpad (std::string &scenario)
{
	if (!m_pPosixLaunchpad) m_pPosixLaunchpad = new PosixLaunchpad (this);
	else m_pPosixLaunchpad->Refresh ();

	DialogManager *dlg = new DialogManager (this, NULL); // (ImGui context)
	LaunchpadDlg lpdlg (m_pPosixLaunchpad);
	dlg->AddEntry (&lpdlg);
	PosixLaunchpad::Action action = PosixLaunchpad::NONE;
	auto next = std::chrono::steady_clock::now();
	while (action == PosixLaunchpad::NONE) {
		gclient->clbkUpdate (false); // window events
		platformKbuf.clear();
		if (bPlatformClose) { // window closed
			bPlatformClose = false;
			action = PosixLaunchpad::EXIT;
			break;
		}
		dlg->ImGuiNewFrame ();
		action = lpdlg.action;
		gclient->clbkRenderScene ();
		gclient->clbkImGuiRenderDrawData ();
		gclient->clbkDisplayFrame ();
		// (at most 60 frames per second, e.g. without vertical sync)
		next += std::chrono::microseconds (16667);
		auto now = std::chrono::steady_clock::now();
		if (next > now) std::this_thread::sleep_until (next);
		else next = now;
	}
	m_pPosixLaunchpad->ReleaseResources ();
	dlg->DelEntry (&lpdlg);
	delete dlg;

	if (action != PosixLaunchpad::LAUNCH) return false;
	scenario = m_pPosixLaunchpad->Scenario ();

	// video settings for the render window
	const CFG_DEVPRM &d = pConfig->CfgDevPrm;
	gclient->VideoData.fullscreen = d.bFullscreen;
	gclient->VideoData.winw = (int)d.WinW;
	gclient->VideoData.winh = (int)d.WinH;
	return true;
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

	// joystick input
	DIJOYSTATE2 js;
	if (pDI->PollJoystick (&js)) {
		UserJoyInput_System (&js);                  // general joystick functions
		if (bRunning) UserJoyInput_OnRunning (&js); // joystick vessel control functions
		for (i = 0; i < 15; i++) ctrlTotal[i] += ctrlJoystick[i]; // update thrust requests
	}
	GamepadInput ();

	g_camera->UpdateMouse();

	// apply manual attitude control
	g_focusobj->ApplyUserAttitudeControls (ctrlTotal);

	return S_OK;
}

//-----------------------------------------------------------------------------
// Name: GamepadInput()
// Desc: Gamepad functions beyond the joystick axes (see DInput::PollJoystick:
//       left stick pitch and bank, shoulder buttons yaw): the right stick
//       rotates the camera, the triggers set the main engine level and the
//       buttons select functions, as their keys in the key map would.
//-----------------------------------------------------------------------------
void Orbiter::GamepadInput ()
{
	const OAPI_CONTROLLERSTATE *pad = pDI->Gamepad ();
	if (!pad) {
		padButtons = 0;
		return;
	}
	auto Axis = [&](int i, double dz) {
		double v = pad->axis[i], a = fabs (v);
		return a <= dz ? 0.0 : copysign ((std::min (a, 1.0) - dz) / (1.0 - dz), v);
	};

	// right stick: camera rotation (up to 1.5 rad/s)
	double rx = Axis (OAPI_PAD_RIGHTX, 0.15), ry = Axis (OAPI_PAD_RIGHTY, 0.15);
	if (rx || ry)
		g_camera->Rotate (1.5 * rx * fabs (rx) * td.SysDT, -1.5 * ry * fabs (ry) * td.SysDT, true);

	// triggers: main (and retro) engine level, up to 0.5 per second
	double th = Axis (OAPI_PAD_RTRIGGER, 0.05) - Axis (OAPI_PAD_LTRIGGER, 0.05);
	if (th && bRunning)
		g_focusobj->IncMainRetroLevel (0.5 * th * td.SimDT);

	// buttons: functions on press
	static const struct { int button, lkey; } map[] = {
		{ OAPI_PAD_A,          OAPI_LKEY_NMKillrot },
		{ OAPI_PAD_B,          OAPI_LKEY_RCSMode },
		{ OAPI_PAD_X,          OAPI_LKEY_HUDMode },
		{ OAPI_PAD_Y,          OAPI_LKEY_ToggleCamInternal },
		{ OAPI_PAD_START,      OAPI_LKEY_Pause },
		{ OAPI_PAD_BACK,       OAPI_LKEY_MainMenu },
		{ OAPI_PAD_LSTICK,     OAPI_LKEY_NMHLevel },
		{ OAPI_PAD_RSTICK,     OAPI_LKEY_CockpitResetCam },
		{ OAPI_PAD_DPAD_UP,    OAPI_LKEY_IncSimSpeed },
		{ OAPI_PAD_DPAD_DOWN,  OAPI_LKEY_DecSimSpeed },
		{ OAPI_PAD_DPAD_LEFT,  OAPI_LKEY_NMPrograde },
		{ OAPI_PAD_DPAD_RIGHT, OAPI_LKEY_NMRetrograde },
	};
	unsigned int pressed = pad->buttons[0] & ~padButtons;
	padButtons = pad->buttons[0];
	for (const auto &m : map)
		if (pressed & (1u << m.button))
			SendLogicalKey (m.lkey);
}

void Orbiter::SendLogicalKey (int lfunc)
{
	WORD k = keymap.Key (lfunc);
	DWORD mod[3], nmod = 0;
	if (k & KMOD_CTRL)  mod[nmod++] = (k & KMOD_LCTRL)  ? OAPI_KEY_LCONTROL : OAPI_KEY_RCONTROL;
	if (k & KMOD_SHIFT) mod[nmod++] = (k & KMOD_LSHIFT) ? OAPI_KEY_LSHIFT   : OAPI_KEY_RSHIFT;
	if (k & KMOD_ALT)   mod[nmod++] = (k & KMOD_LALT)   ? OAPI_KEY_LALT     : OAPI_KEY_RALT;
	SendKbdBuffered (k & 0xFF, mod, nmod, false);
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
