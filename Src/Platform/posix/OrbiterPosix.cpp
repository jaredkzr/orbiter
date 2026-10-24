// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Platform-specific parts of class Orbiter for non-Windows platforms.
//
// Without a graphics client Orbiter runs as a headless server: the
// scenario given on the command line (-s / --scenario) is launched
// directly, and the session is controlled through the console (stdin).
// =======================================================================

#include "Orbiter.h"
#include "Log.h"
#include "State.h"
#include "Script.h"
#include "Mfd.h"
#include "Memstat.h"
#include "console_ng.h"
#include <cstdio>
#include <string>
#include <thread>

extern const TCHAR *MasterConfigFile;

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

	// The console allows to control the session, since there is no launchpad
	Launch (pConfig->CfgCmdlinePrm.LaunchScenario.c_str());

	while (bSession) {
		if (BeginTimeStep (bRunning)) {
			UpdateWorld();
			EndTimeStep (bRunning);
		}
		if (m_pConsole)
			m_pConsole->ParseCmd();
		if (!bRunning) // don't spin while paused
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
// Desc: There are no input devices on this platform. Only reset the per-frame
//       key and attitude control states; key events can still be injected
//       through SendKbdBuffered/SendKbdImmediate.
//-----------------------------------------------------------------------------
HRESULT Orbiter::UserInput ()
{
	memset (simkstate, 0, 256);
	for (int i = 0; i < 15; i++) {
		ctrlKeyboard[i] = ctrlJoystick[i] = 0;
		ctrlTotal[i] = 0;
	}
	return S_OK;
}

bool Orbiter::ActivateRoughType ()
{
	return false;
}

bool Orbiter::DeactivateRoughType ()
{
	return false;
}
