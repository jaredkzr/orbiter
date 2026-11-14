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

#include "World.h"
#include "FlightState.h"
#include <deque>
#include <map>
#include <initializer_list>
#include <string>

class Vessel;
class CelestialBody;
class VESSEL;

namespace audio {

class System;
struct XrConfig;

class Callouts {
public:
	explicit Callouts (System &s): sys (s) {}

	enum Event { EV_DOCKED, EV_UNDOCKED, EV_SPLASHDOWN, EV_PART_LOST, EV_HEAT_DAMAGE, EV_AFLOAT, EV_DOCK_REFUSED };

	/// Each time step: the vessel in focus and its flight this step (the
	/// events FlightState published, its sample); now: the clock of what is
	/// heard (the lines' waits and the voice's pacing: held in a pause, not
	/// hurried by time acceleration)
	void Step (const Vessel *focus, const FlightState &flight, double now, double warp);
	/// Something happened to a vessel (the focus vessel's are called:
	/// true - it's the one followed)
	bool OnEvent (const Vessel *v, Event e);
	/// A new session, or the focus moved: start afresh
	void Reset ();
	/// Silent (the setting off): the flight still followed (the radio's replies)
	void SetMuted (bool m) { muted = m; if (m) queue.clear (); }

	/// Lines said so far (tests); the n-th newest's file (null: none)
	int Said () const { return said; }
	const char *Recent (int n) const { return n >= 0 && n < (int)recent.size () ? recent[n].c_str () : nullptr; }
	/// A line spoken elsewhere (an XRSound cfg's animation callout): noted
	void Note (const std::string &file, double now);

	/// A crew line for another part of the sound (the radio's entry interface, the drive): its key
	/// (Sound/Voice/callouts.cfg; a vessel's XRSound cfg or code may give its own file)
	void Crew (const char *key);

private:
	enum Prio { LOW = 0, NORMAL = 1, WARNING = 2 };
	/// Say a line (a key of the XRSound cfgs, or a file), if it's on; it
	/// waits at most maxAge [s] for its turn
	bool Say (const std::string &file, Prio prio = NORMAL, double maxAge = 2.0, float gain = 1.0f);
	/// A line by its key: its file (Line), priority, wait and gain (the table)
	bool SayKey (const char *key);
	/// A group's line (altitudes, Mach, distances), as the group's key has it
	bool SayGroup (const char *group, const std::string &base);
	/// The file for an XRSound key (its default if the cfgs don't name it;
	/// empty: off)
	std::string Line (const char *key) const;
	/// A file in a group's folder (altitude, Mach, distance callouts)
	std::string GroupLine (const char *group, const std::string &base) const;
	void Speak (double now);

	// the flight's moments and crossings, said (in the order the flight's
	// steps had them: heat, flight, docking, modes, ascent, fuel)
	void FlightLines (const FlightState &f, double now);
	void DockingLines (const FlightState &f, double now);
	void Lines (const FlightState &f, std::initializer_list<FlightEvent::Kind> kinds, double now);
	void Said1 (const FlightEvent &e, double now);

	System &sys;
	const Vessel *vessel = nullptr;
	BodyRef vref;           // (its identity: a new vessel at its address is another)
	const XrConfig *cfg = nullptr;
	struct Queued { std::string file; Prio prio; double until; bool cued = false; float gain = 1.0f; };
	std::deque<Queued> queue;
	int speaking = 0;       // the line playing (a handle)
	int beep = 0;           // the docking radar's beep playing
	int said = 0;
	bool muted = false;
	std::deque<std::string> recent; // (newest first; kept across focus changes)
	double t = 0.0;

	/// The voice's own pacing (afresh for each vessel)
	struct Pacing {
		double nextAlt = 0.0, nextGearWarn = 0.0, nextTerrain = 0.0;  // (not again before these)
		double nextDist = 0.0, nextBeep = 0.0;
		double undockAt = -1.0;  // ("undocking confirmed" a moment after)
	} pace;
	bool greeted = false;   // (the session's first vessel welcomed aboard)
	std::map<std::string, double> lastSaid;   // (lines with a cooldown: when said)
	int lastChatter[2] = { -1, -1 };   // (the line said last: sunrise, sunset)
	double chatterAt = -1e9;            // (the last chatter: none again within CHATTER_GAP)
};

} // namespace audio

#endif // !__CALLOUTS_H
