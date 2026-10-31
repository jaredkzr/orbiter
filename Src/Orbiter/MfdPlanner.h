// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Transfer Planner MFD: transfers to other planets and to moons (see
// Transfer.h). Pages: Setup (target, objective, arrival orbit), Chart
// (porkchop plot of the velocity change over departure date and time of
// flight), Windows (launch windows over the next years), Burn (the
// ejection: when, how much, which way), Cruise (corrections and the
// predicted arrival), Budget (the plan against the vessel's velocity
// change); with gravity assists (Setup > Advanced) also Route (the flyby
// bodies, suggestions, the route's legs and flybys). Drawn with the MFD
// widget toolkit (pointer input).
// =======================================================================

#ifndef __MFDPLANNER_H
#define __MFDPLANNER_H

#include "Mfd.h"

namespace transfer { class Planner; }

class Instrument_Planner: public Instrument {
public:
	Instrument_Planner (Pane *_pane, INT_PTR _id, const Spec &spec, Vessel *_vessel);
	virtual ~Instrument_Planner ();
	int Type () const { return MFD_PLANNER; }
	char ModeSelKey () const { return 'P'; }
	void UpdateDraw (oapi::Sketchpad *skp);
	bool Update (double upDTscale);
	bool KeyBuffered (DWORD key);
	bool ProcessButton (int bt, int event);
	const char *BtnLabel (int bt) const;
	int BtnMenu (const MFDBUTTONMENU **menu) const;
	int ProcessMessage (int msg, void *data);

	enum Page { PG_SETUP, PG_CHART, PG_WINDOWS, PG_LAUNCH, PG_BURN, PG_CRUISE, PG_BUDGET, PG_ROUTE, NPAGE };

protected:
	bool ReadParams (std::ifstream &ifs);
	void WriteParams (std::ostream &ofs) const;

private:
	transfer::Planner *Planner () const;
	void DrawSetup (const struct Layout &L);
	void DrawChart (const struct Layout &L);
	void DrawWindows (const struct Layout &L);
	void DrawBurn (const struct Layout &L);
	void DrawCruise (const struct Layout &L);
	void DrawBudget (const struct Layout &L);
	void DrawLaunch (const struct Layout &L);
	void DrawRoute (const struct Layout &L);
	void CycleTarget (int dir);
	void Action (int what); // the buttons' and keys' functions

	int page;
	unsigned int lastSerial = 0;   // planner results seen
	int lastJob = 0;
	bool hover = false;
	double hoverX = 0, hoverY = 0; // hovered plot point
	int flySel = -1;               // Route: the selected flyby
	int addSel = 0;                // Route: the body to add
	int routeView = 0;             // Route: 0 the route, 1 the suggestions

	static struct SavePrm {
		Vessel *usr = nullptr;
		int page = PG_SETUP;
	} saveprm;
};

#endif // !__MFDPLANNER_H
