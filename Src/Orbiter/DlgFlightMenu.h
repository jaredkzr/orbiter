// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The flight menu (Esc): the simulation pauses and a menu opens over the
// view: resume, save, quick save, settings, help, quit to the launchpad,
// exit.
// =======================================================================

#ifndef __DLGFLIGHTMENU_H
#define __DLGFLIGHTMENU_H

#include "OrbiterAPI.h"

class DlgFlightMenu : public ImGuiDialog {
public:
	DlgFlightMenu ();
	/// Open the menu (pausing), or close it (resuming as before)
	void Toggle ();
	void Display () override;
	void OnDraw () override {}
	void OnClose () override;

private:
	void Close ();
	bool wasRunning = false; // the simulation ran when the menu opened
	int openFrame = 0;       // (the Esc that opened it is not the one that closes it)
	bool saving = false;     // the "save as" field shown
	char saveName[128] = "";
};

#endif // !__DLGFLIGHTMENU_H
