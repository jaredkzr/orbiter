// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Relativistic Travel MFD: the ideal drive (see RelTravel.h). Pages:
// Target (body or vessel, arrival at rest, in orbit or passing, altitude),
// Trip (acceleration, timing, the plan: arrival on both clocks, top speed,
// speed plotted against Solar System and ship time; engage), Flight
// (speed, Lorentz factor, rapidity, the drive and its g-load, the ship's
// clock, distance in both frames, time to arrival; a manual drive), Cost
// (the rocket a real trip would need: photon rocket, other exhaust speeds).
// Drawn with the MFD widget toolkit (pointer input).
// =======================================================================

#ifndef __MFDRELTRAVEL_H
#define __MFDRELTRAVEL_H

#include "Mfd.h"

namespace reltravel { class Controller; }

class Instrument_RelTravel: public Instrument {
public:
	Instrument_RelTravel (Pane *_pane, INT_PTR _id, const Spec &spec, Vessel *_vessel);
	virtual ~Instrument_RelTravel ();
	int Type () const { return MFD_RELTRAVEL; }
	char ModeSelKey () const { return 'R'; }
	void UpdateDraw (oapi::Sketchpad *skp);
	bool Update (double upDTscale);
	bool KeyBuffered (DWORD key);
	bool ProcessButton (int bt, int event);
	const char *BtnLabel (int bt) const;
	int BtnMenu (const MFDBUTTONMENU **menu) const;

	enum Page { PG_TARGET, PG_TRIP, PG_FLIGHT, PG_COST, NPAGE };

protected:
	bool ReadParams (std::ifstream &ifs);
	void WriteParams (std::ostream &ofs) const;

private:
	reltravel::Controller *Trip () const;
	void DrawTarget (const struct RLayout &L);
	void DrawTrip (const struct RLayout &L);
	void DrawFlight (const struct RLayout &L);
	void DrawCost (const struct RLayout &L);
	void CycleTarget (int dir);
	void Action (int what); // the buttons' and keys' functions

	int page;
	double exhaust = 0.1;  // Cost: the exhaust speed compared [c]
	double manualG = 1.0;  // Flight: the manual drive [g]
	bool manualRev = false;

	static struct SavePrm {
		Vessel *usr = nullptr;
		int page = PG_TARGET;
	} saveprm;
};

#endif // !__MFDRELTRAVEL_H
