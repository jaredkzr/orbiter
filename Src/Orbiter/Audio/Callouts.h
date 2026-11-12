// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The voice: callouts for the vessel in focus, in the headset (the voice
// bus: heard inside and outside, not through the air; the other sounds
// duck under it).
//
// XRSound's recorded lines, at XRSound's moments (its cfgs choose the
// files; a vessel class's own XRSound-<class>.cfg over the global one):
//   - takeoff and landing: "100 knots", liftoff, touchdown, wheel stop;
//     "warning, gear is up" below WarningGearIsUpAltitude, descending,
//     gear up; "cleared to land" through 1500 m; altitude above the
//     ground from 5000 m down (descending)
//   - Mach: each whole Mach number, "subsonic" below Mach 1
//   - docking: the distance to the docking target (an IDS signal) from
//     5000 m in; "contact", "undocking confirmed"; the docking radar's beep
//     within 500 m
//   - modes: autopilot on/off, RCS rotation/translation/off, aerodynamic
//     controls pitch/on/off
//   - warnings: main and RCS fuel low and depleted; damage (a part broken
//     off; heat damage)
// New lines (Sound/Voice/, made with Piper: Utils/Audio/make_voice.py):
//   - ignition (main engines lit on the ground, pointing up), max Q,
//     MECO (the ascent's main engine cutoff), orbit (periapsis above the
//     atmosphere, on the way up), splashdown
//
// One line at a time; lines waiting too long are dropped (an altitude
// called late is wrong).
// =======================================================================

#ifndef __CALLOUTS_H
#define __CALLOUTS_H

#include <deque>
#include <string>

class Vessel;
class VESSEL;

namespace audio {

class System;
struct XrConfig;

class Callouts {
public:
	explicit Callouts (System &s): sys (s) {}

	enum Event { EV_DOCKED, EV_UNDOCKED, EV_SPLASHDOWN, EV_PART_LOST, EV_HEAT_DAMAGE };

	/// Each time step: the vessel in focus (its state since the last step)
	void Step (const Vessel *focus, double simt, double simdt, double warp);
	/// Something happened to a vessel (the focus vessel's are called)
	void OnEvent (const Vessel *v, Event e);
	/// A new session, or the focus moved: start afresh
	void Reset ();

	/// Lines said so far (tests); the n-th newest's file (null: none)
	int Said () const { return said; }
	const char *Recent (int n) const { return n >= 0 && n < (int)recent.size () ? recent[n].c_str () : nullptr; }
	/// A line spoken elsewhere (an XRSound cfg's animation callout): noted
	void Note (const std::string &file, double simt);

	/// The gear's state (0 up .. 1 down; < 0: not known) of a vessel: the
	/// animation given the gear's sound, else the one its XRSound cfg names
	double GearState (const Vessel *v) const;

private:
	enum Prio { LOW = 0, NORMAL = 1, WARNING = 2 };
	/// Say a line (a key of the XRSound cfgs, or a file), if it's on; it
	/// waits at most maxAge [s] for its turn
	bool Say (const std::string &file, Prio prio = NORMAL, double maxAge = 2.0);
	/// The file for an XRSound key (its default if the cfgs don't name it;
	/// empty: off)
	std::string Line (const char *key) const;
	/// A file in a group's folder (altitude, Mach, distance callouts)
	std::string GroupLine (const char *group, const std::string &base) const;
	void Speak (double simt);

	void Flight (VESSEL *v, double simt);
	void Docking (VESSEL *v, double simt);
	void Modes (VESSEL *v);
	void Ascent (VESSEL *v, double simt);
	void Fuel (VESSEL *v);

	System &sys;
	const Vessel *vessel = nullptr;
	const XrConfig *cfg = nullptr;
	struct Queued { std::string file; Prio prio; double until; };
	std::deque<Queued> queue;
	int speaking = 0;       // the line playing (a handle)
	int beep = 0;           // the docking radar's beep playing
	int said = 0;
	std::deque<std::string> recent; // (newest first; kept across focus changes)
	double t = 0.0;

	/// What is followed of the vessel in focus (afresh for each)
	struct State {
		bool first = true;  // (the first step: state only)
		// takeoff and landing
		double prevAlt = -1.0, prevVs = 0.0, prevAirspeed = -1.0, prevMach = -1.0;
		double airborneAt = 0.0, nextAlt = 0.0, nextGearWarn = 0.0, heldSince = -1.0;
		bool flying = false, rolledOut = true;
		// docking
		double prevDist = -1.0, nextDist = 0.0, nextBeep = 0.0, undockAt = -1.0;
		// modes
		int prevNav = -1, prevRcs = -1, prevAf = -1;
		// ascent
		bool ascent = false, maxqSaid = false, mecoSaid = false, orbitSaid = false;
		double qMax = 0.0, mainOffAt = -1.0, prevMain = 0.0;
		// fuel: the fraction left of the main and RCS tanks
		double prevMainFuel = -1.0, prevRcsFuel = -1.0;
	} st;
};

} // namespace audio

#endif // !__CALLOUTS_H
