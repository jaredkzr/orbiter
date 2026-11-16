// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The flight menu (Esc): the simulation pauses and a menu opens over the
// view: resume, save, quick save, settings, help, quit to the launchpad,
// exit. With the focus vessel lost it opens as "flight over" (restart,
// load the latest quicksave, switch vessel, keep watching) - only on Esc:
// a crash doesn't pause the simulation, the wreck plays on
// =======================================================================

#ifndef __DLGFLIGHTMENU_H
#define __DLGFLIGHTMENU_H

#include "OrbiterAPI.h"
#include <string>

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
	bool over = false;       // opened as "flight over"
	std::string overTitle, overReason;
};

#endif // !__DLGFLIGHTMENU_H
