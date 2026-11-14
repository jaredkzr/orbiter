// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Callouts.h"
#include "DebugEnv.h"
#include "CrewLines.h"
#include "AudioSystem.h"
#include "XrConfig.h"
#include "XRSoundShim.h"
#include "Vessel.h"
#include "Celbody.h"
#include "Orbiter.h"
#include "Log.h"
#include <algorithm>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>

extern Orbiter *g_pOrbiter;

namespace audio {

namespace {
	constexpr double KNOTS100 = 100.0 * 0.514444;   // [m/s]
	constexpr double MIN_GAP = 1.0;                 // [s] between altitude or distance callouts
	constexpr double CLEARED_ALT = 1500.0;          // [m] "cleared to land"
	constexpr double BEEP_RANGE = 500.0;            // [m] the docking radar beeps within this
	constexpr double CHATTER_GAP = 300.0;           // [s] the crew's chatter: none again within five minutes
	/// Before a warning: a caution chirp; before damage, the master alarm (generated: make_sfx.py)
	const char *CAUTION = "Sound/Generated/caution.flac", *MASTER_ALARM = "Sound/Generated/master_alarm.flac";
	constexpr double ATM_P = FlightState::ATM_P;    // [Pa] Mach callouts above this pressure

	const int heights[] = { 5000, 4000, 3000, 2000, 1000, 900, 800, 700, 600, 500, 400, 300, 200, 100,
		75, 50, 40, 30, 20, 15, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1 };

}

std::string Callouts::Line (const char *key) const
{
	std::string own;
	if (xr::LineFor (vessel, key, own)) return own; // (the vessel's code's: XRSound)
	if (cfg) {
		std::string lk (key);
		for (auto &c : lk) c = (char)std::tolower ((unsigned char)c);
		auto it = cfg->files.find (lk);
		if (it != cfg->files.end ()) return cfg->File (key); // (named: that, or none)
	}
	const CrewLine *c = CrewLines::Game ().Find (key);   // (the table: Sound/Voice/callouts.cfg)
	return c ? c->file : std::string ();
}

bool Callouts::SayKey (const char *key)
{
	const CrewLine *c = CrewLines::Game ().Find (key);
	if (!c) return false;
	// (its cooldown: not again so soon)
	if (c->cooldown > 0.0) {
		auto it = lastSaid.find (c->key);
		if (it != lastSaid.end () && t - it->second < c->cooldown) return false;
	}
	if (!Say (Line (key), c->warning ? WARNING : NORMAL, c->wait, (float)c->gain)) return false;
	if (c->cooldown > 0.0) lastSaid[c->key] = t;
	return true;
}

bool Callouts::SayGroup (const char *group, const std::string &base)
{
	const CrewLine *c = CrewLines::Game ().Find (group);
	if (!c) return false;
	return Say (GroupLine (group, base), c->warning ? WARNING : NORMAL, c->wait, (float)c->gain);
}

std::string Callouts::GroupLine (const char *group, const std::string &base) const
{
	std::string g (group);
	for (auto &c : g) c = (char)std::tolower ((unsigned char)c);
	std::string dir;
	if (xr::LineFor (vessel, group, dir)) { if (dir.empty ()) return std::string (); }
	else {
		dir = cfg ? cfg->Group (group) : std::string ();
		if (dir.empty () && cfg && cfg->groups.count (g)) return std::string (); // (switched off)
	}
	if (dir.empty ()) if (const CrewLine *c = CrewLines::Game ().Find (group)) dir = c->file;
	if (dir.empty ()) return std::string ();
	for (const char *ext : { ".wav", ".ogg", ".flac", ".mp3" }) {
		std::string f = dir + "/" + base + ext;
		if (!System::FindSound (xr::Voice (f)).empty ()) return f;
	}
	return std::string ();
}

void Callouts::Reset ()
{
	if (speaking) sys.StopSound (speaking);
	if (beep) sys.StopSound (beep);
	speaking = beep = 0;
	queue.clear ();
	pace = Pacing ();
	vessel = nullptr;
	vref = BodyRef ();
	cfg = nullptr;
}

bool Callouts::Say (const std::string &f, Prio prio, double maxAge, float gain)
{
	if (f.empty () || muted) return false;
	std::string file (f);
	std::replace (file.begin (), file.end (), '\\', '/');
	// (the same line waiting already: once)
	for (auto &q : queue) if (q.file == file) { q.until = t + maxAge; return true; }
	Queued q{file, prio, t + maxAge};
	q.gain = gain;
	// warnings go before the others waiting
	auto it = queue.begin ();
	while (it != queue.end () && it->prio >= prio) ++it;
	queue.insert (it, q);
	return true;
}

void Callouts::Speak (double now)
{
	if (speaking && !sys.Playing (speaking)) speaking = 0;
	while (!queue.empty () && queue.front ().until < now) queue.pop_front (); // (too late now)
	if (speaking || queue.empty ()) return;
	Queued q = queue.front ();
	queue.pop_front ();
	// a warning: its cue first (the line after it: back to the queue's front)
	if (q.prio == WARNING && !q.cued) {
		const bool damage = q.file == Line ("WarningDamage") || q.file == Line ("WarningHeatDamage");
		const char *cue = damage ? MASTER_ALARM : CAUTION;
		if (!System::FindSound (cue).empty () && (speaking = sys.PlayOn (VOICE, cue, 1.0, false, false))) {
			q.cued = true, q.until = std::max (q.until, now + 1.0);
			queue.push_front (q);
			const bool log = Dbg ().log > 0;
			if (log) printf ("Audio: cue %s at %.2f\n", cue, now), fflush (stdout);
			return;
		}
	}
	speaking = sys.PlayOn (VOICE, xr::Voice (q.file), q.gain, false); // (XRSound's line: in Orbiter's voice, as set)
	if (!speaking) return;
	said++;
	Note (q.file, now);
}

void Callouts::Note (const std::string &f, double now)
{
	std::string file (f);
	std::replace (file.begin (), file.end (), '\\', '/');
	recent.push_front (file);
	if (recent.size () > 64) recent.pop_back ();
	// (and the file that plays, where it isn't the one named: Orbiter's voice for XRSound's line)
	std::string plays = System::FindSound (xr::Voice (file));
	std::replace (plays.begin (), plays.end (), '\\', '/');
	const bool other = !plays.empty () && plays.find (file) == std::string::npos;
	LOGOUT ("Audio: callout %s at %.2f%s%s", file.c_str (), now, other ? " plays " : "", other ? plays.c_str () : "");
	const bool log = Dbg ().log > 0;
	if (log) printf ("Audio: callout %s at %.2f%s%s\n", file.c_str (), now, other ? " plays " : "", other ? plays.c_str () : ""), fflush (stdout);
}

bool Callouts::OnEvent (const Vessel *v, Event e)
{
	if (!v || sys.GetWorld ().Known (v) != vref) return false;
	switch (e) {
	case EV_DOCKED:      SayKey ("DockingCallout"); pace.undockAt = -1.0; break;
	case EV_UNDOCKED:    pace.undockAt = t + 0.667; break; // ("undocking confirmed" a moment later)
	case EV_SPLASHDOWN:  SayKey ("Splashdown"); break;
	case EV_PART_LOST:   SayKey ("WarningDamage"); break;
	case EV_HEAT_DAMAGE: SayKey ("WarningHeatDamage"); break;
	case EV_AFLOAT:      SayKey ("Afloat"); break;
	case EV_DOCK_REFUSED: SayKey ("DockingAborted"); break;
	}
	return true;
}

void Callouts::Step (const Vessel *focus, const FlightState &f, double now, double warp)
{
	const BodyRef fr = sys.GetWorld ().Ref (focus);
	if (fr != vref) {
		Reset ();
		vessel = focus;
		vref = fr;
		cfg = focus ? &XrConfig::For (focus->ClassName () ? focus->ClassName () : "") : nullptr;
		// the session's first vessel: welcomed aboard (XRSound's AudioGreeting; not in a render: the tests)
		if (focus && !greeted && !sys.Rendering ()) { greeted = true; SayKey ("AudioGreeting"); }
	}
	t = now;
	if (!vessel || vessel->Destroyed ()) { queue.clear (); return; }
	if (!f.Sampled ()) return; // (no module: nothing followed)
	// (time acceleration: nothing called)
	const bool quiet = warp > 10.0;
	if (quiet) queue.clear ();
	if (!quiet || f.First ()) {
		Lines (f, { FlightEvent::HEAT, FlightEvent::STRUCTURAL }, now);
		FlightLines (f, now);
		DockingLines (f, now);
		Lines (f, { FlightEvent::NAV, FlightEvent::RCS_MODE, FlightEvent::AF_MODE }, now);
		Lines (f, { FlightEvent::IGNITION, FlightEvent::CAPTURE, FlightEvent::ESCAPE, FlightEvent::ROLL_PROGRAM,
			FlightEvent::MAXQ, FlightEvent::MECO, FlightEvent::ORBIT }, now);
		Lines (f, { FlightEvent::MAIN_FUEL, FlightEvent::RCS_FUEL }, now);
	}
	Lines (f, { FlightEvent::LIGHT }, now);
	if (quiet) return;
	Lines (f, { FlightEvent::SOI }, now);
	if (pace.undockAt >= 0.0 && now >= pace.undockAt) { SayKey ("UndockingCallout"); pace.undockAt = -1.0; }
	Speak (now);
}

void Callouts::Lines (const FlightState &f, std::initializer_list<FlightEvent::Kind> kinds, double now)
{
	for (const FlightEvent &e : f.Events ())
		for (FlightEvent::Kind k : kinds)
			if (e.kind == k) { Said1 (e, now); break; }
}

void Callouts::Said1 (const FlightEvent &e, double now)
{
	using E = FlightEvent;
	switch (e.kind) {
	case E::HEAT:       SayKey (e.value >= 2.0 ? "WarningHullTemperatureCritical" : "WarningHullTemperature"); break;
	case E::STRUCTURAL: SayKey ("WarningStructuralLimit"); break;
	case E::NAV: {
		// a mode on: its name ("Prograde."; else XRSound's autopilot tone); off: the tone
		static const char *navKey[8] = { "", "NavKillrot", "NavHlevel", "NavPrograde", "NavRetrograde", "NavNormal", "NavAntinormal", "NavHoldalt" };
		const int nav = (int)e.value;
		const std::string named = nav > 0 && nav < 8 ? Line (navKey[nav]) : std::string ();
		SayKey (!named.empty () && !System::FindSound (xr::Voice (named)).empty () ? navKey[nav] : nav ? "AutopilotOn" : "AutopilotOff");
		break; }
	case E::RCS_MODE:   SayKey (e.value == 1 ? "RCSRotation" : e.value == 2 ? "RCSTranslation" : "RCSOff"); break;
	case E::AF_MODE:    SayKey (e.value == 0 ? "AFOff" : e.value == 1 ? "AFPitch" : "AFOn"); break;
	case E::IGNITION:   SayKey ("Ignition"); break;
	case E::CAPTURE:    SayKey ("OrbitInsertion"); break;
	case E::ESCAPE:     SayKey ("EscapeTrajectory"); break;
	case E::ROLL_PROGRAM: SayKey ("RollProgram"); break;
	case E::MAXQ:       SayKey ("MaxQ"); break;
	case E::MECO:       SayKey ("MECO"); break;
	case E::ORBIT:      SayKey ("Orbit"); break;
	case E::MAIN_FUEL:
	case E::RCS_FUEL: {
		const bool m = e.kind == E::MAIN_FUEL;
		if (e.value == 0.0) SayKey (m ? "WarningMainFuelDepleted" : "WarningRCSFuelDepleted");
		else if (e.value == 1.0) SayKey (m ? "WarningMainFuelLow" : "WarningRCSFuelLow");
		else SayKey (e.value > 15.0 ? (m ? "MainFuel20" : "RCSFuel20") : (m ? "MainFuel10" : "RCSFuel10"));
		break; }
	case E::LIGHT: {
		// sunrise, sunset above the air: the commander's chatter - one of the lines there are, not the last
		// one again; casual: quieter than a callout (-4.4 dB); not again within five minutes
		if (now - chatterAt < CHATTER_GAP) break;
		const int dark = e.value > 0.5 ? 1 : 0;
		const CrewLine *c = CrewLines::Game ().Find (dark ? "ChatterSunset" : "ChatterSunrise");
		if (!c) break;
		std::vector<std::string> lines;
		const size_t at = c->file.find ("{n}");
		for (int n = 1; n <= 9 && at != std::string::npos; n++) {
			std::string fl = c->file;
			fl.replace (at, 3, std::to_string (n));
			if (!System::FindSound (fl).empty ()) lines.push_back (fl);
		}
		if (lines.empty ()) break;
		int &last = lastChatter[dark];
		int k = (int)(std::fmod (now * 7.31, (double)lines.size ()));
		if (lines.size () > 1 && k == last) k = (k + 1) % (int)lines.size ();
		last = k;
		if (Say (lines[k], c->warning ? WARNING : NORMAL, c->wait, (float)c->gain)) chatterAt = now;
		break; }
	case E::SOI: {
		// "Entering the sphere of influence of the Moon." (Sound/Voice/SOI <body>.flac; else a general line)
		const std::string fl = "Sound/Voice/SOI " + e.text + ".flac";
		const CrewLine *c = CrewLines::Game ().Find ("SoiChanged");
		if (c) Say (System::FindSound (fl).empty () ? Line ("SoiChanged") : fl, c->warning ? WARNING : NORMAL, c->wait, (float)c->gain);
		break; }
	default: break;
	}
}

void Callouts::FlightLines (const FlightState &f, double now)
{
	// (after XRSound's TakeoffAndLandingCallouts, AltitudeCallouts, MachCallouts)
	const FlightSample &s = f.Now ();
	const double alt = s.alt, airspeed = s.airspeed, vs = s.vs;
	const double gear = s.gear;   // (< 0: not known)
	const bool gearDown = gear >= 0.999;
	const bool atm = s.atmP > ATM_P;
	const double mach = s.mach;
	const double prevAlt = f.PrevAlt (), prevAirspeed = f.PrevAirspeed (), prevMach = f.PrevMach ();
	switch (f.StepBranch ()) {
	case FlightState::TOUCHDOWN_STEP:
		// touchdown (a hard one: the crunch says it, no voice)
		for (const FlightEvent &e : f.Events ())
			if (e.kind == FlightEvent::TOUCHDOWN && e.value < 4.0 && (gear < 0.0 || gearDown)) SayKey ("Touchdown");
		break;
	case FlightState::ROLLING:
		// rolling: "100 knots" through it either way; a takeoff run: V1 and rotate, where the
		// vessel's cfg gives its speeds (accelerating through them); at a stop, "wheel stop"
		if (prevAirspeed >= 0.0 && ((airspeed >= KNOTS100) != (prevAirspeed >= KNOTS100)) && (gear < 0.0 || gearDown))
			SayKey ("OneHundredKnots");
		if (cfg && prevAirspeed >= 0.0 && airspeed > prevAirspeed) {
			const double v1 = cfg->Number ("V1Speed", 0.0), vr = cfg->Number ("RotateSpeed", 0.0);
			if (v1 > 0.0 && prevAirspeed < v1 && airspeed >= v1) SayKey ("V1");
			if (vr > 0.0 && prevAirspeed < vr && airspeed >= vr) SayKey ("Rotate");
		}
		for (const FlightEvent &e : f.Events ())
			if (e.kind == FlightEvent::ROLLOUT && gearDown) SayKey ("WheelStop");
		break;
	case FlightState::LEAVING:
		for (const FlightEvent &e : f.Events ())
			if (e.kind == FlightEvent::LIFTOFF) SayKey ("Liftoff");
		break;
	case FlightState::FLYING: {
		for (const FlightEvent &e : f.Events ())
			if (e.kind == FlightEvent::POSITIVE_RATE) SayKey ("PositiveRate");
		// closing on the ground: "terrain" (an impact within 15 s, coming down faster than 10 m/s; the
		// gear up); low and sinking fast: "sink rate" (below 150 m, faster than 6 m/s); each 5 s at most
		const double sink = -vs;
		if (now >= pace.nextTerrain && alt > 0.0) {
			// (not with the gear down: a landing - a glider's steep final approach comes down fast)
			if (!gearDown && sink > 10.0 && alt / sink < 15.0) { SayKey ("WarningTerrain"); pace.nextTerrain = now + 5.0; }
			else if (alt < 150.0 && sink > 6.0) { SayKey ("WarningSinkRate"); pace.nextTerrain = now + 5.0; }
		}
		// descending: the gear, clearance, the height above the ground
		const double gearAlt = cfg ? cfg->Number ("WarningGearIsUpAltitude", 275.0) : 275.0;
		if (gear >= 0.0 && !gearDown && alt < gearAlt && alt < prevAlt && vs <= -0.25 && alt > 0.0 && now >= pace.nextGearWarn) {
			if (SayKey ("WarningGearIsUp")) pace.nextGearWarn = now + 4.0;
		} else if (now >= pace.nextAlt && vs < 0.0) {
			if (atm && gear >= 0.0 && prevAlt > CLEARED_ALT && alt <= CLEARED_ALT && vs > -150.0) {
				if (SayKey ("YouAreClearedToLand")) pace.nextAlt = now + MIN_GAP;
			} else if (alt <= heights[0]) {
				for (int h : heights) {
					if (prevAlt > h && alt <= h) {
						if (SayGroup ("AltitudeCalloutsGroup", std::to_string (h))) pace.nextAlt = now + MIN_GAP;
						break;
					}
				}
			}
		}
		break; }
	default: return; // (the first step: state only)
	}
	// Mach: through Mach 1 up, subsonic down, each whole number either way (off the ground)
	const FlightState::Branch b = f.StepBranch ();
	if ((b == FlightState::LEAVING || b == FlightState::FLYING) && atm && prevMach > 0.0 && mach > 0.0) {
		if (prevMach >= 1.0 && mach < 1.0) SayKey ("SubsonicCallout");
		else if (prevMach < 1.0 && mach >= 1.0) SayGroup ("MachCalloutsGroup", "Mach 1");
		else if (prevMach < 27.0 && mach >= 27.0) SayGroup ("MachCalloutsGroup", "Mach 27 Plus");
		else {
			for (int m = 2; m < 27; m++) {
				if ((prevMach < m && mach >= m) || (prevMach > m && mach <= m)) {
					SayGroup ("MachCalloutsGroup", "Mach " + std::to_string (m));
					break;
				}
			}
		}
	}
}

void Callouts::DockingLines (const FlightState &f, double now)
{
	// (after XRSound's DockingCallouts and DockingRadar: the distance from our first port to the
	// nearest IDS signal tuned)
	const FlightSample &s = f.Now ();
	if (f.First () || s.docked) { if (beep) sys.StopSound (beep), beep = 0; return; }
	const double dist = s.dockDist, prevDist = f.PrevDock ();
	if (dist >= 0.0 && prevDist >= 0.0 && now >= pace.nextDist && dist <= heights[0]) {
		for (int h : heights) {
			if (prevDist > h && dist <= h) {
				if (SayGroup ("DockingDistanceCalloutsGroup", std::to_string (h))) pace.nextDist = now + MIN_GAP;
				break;
			}
		}
	}
	// the radar's beep: faster as the port nears (every 4 s at 500 m, 0.5 s close)
	if (dist > 0.0 && dist <= BEEP_RANGE && !muted) {
		if (now >= pace.nextBeep) {
			if (beep) sys.StopSound (beep);
			const std::string fl = Line ("DockingRadarBeep");
			const CrewLine *c = CrewLines::Game ().Find ("DockingRadarBeep");
			beep = fl.empty () ? 0 : sys.PlayOn (VOICE, xr::Voice (fl), c ? c->gain : 0.4, false, false); // (a beep, not speech)
			pace.nextBeep = now + 0.5 + 3.5 * (dist / BEEP_RANGE);
		}
	} else {
		if (beep) sys.StopSound (beep), beep = 0;
		pace.nextBeep = 0.0;
	}
}

void Callouts::Crew (const char *key)
{
	if (vessel) SayKey (key);
}

} // namespace audio
