// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "FlightState.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace audio {

namespace {
	constexpr double AIRBORNE_AFTER = 0.5;          // [s] off the ground: flying (not a bounce)
	constexpr double RUNWAY_SPEED = 30.0;           // [m/s] a takeoff run (not a hover's liftoff)
	constexpr double MAXQ_MIN = 5000.0;             // [Pa] a Max Q worth calling
	constexpr double LOW_FUEL = 0.05;               // fuel low below 5 %
	constexpr double FUEL_STEPS[] = { 0.20, 0.10 }; // and a word at these
}

const char *FlightEventName (FlightEvent::Kind k)
{
	static const char *names[] = { "touchdown", "surface", "rollout", "landed", "liftoff", "positive-rate", "approach",
		"nav", "rcs-mode", "af-mode", "ignition", "deorbit", "capture", "escape", "descent", "roll-program", "maxq", "meco", "orbit",
		"main-fuel", "rcs-fuel", "heat", "structural", "light", "soi", "deepspace" };
	return k >= 0 && k < (int)(sizeof (names) / sizeof (names[0])) ? names[k] : "?";
}

void FlightState::Reset ()
{
	st = State ();
	dark = -1, lightChangedAt = -1.0;
	vessel = 0;
	events.clear ();
	branch = NONE;
	prevDock = -1.0;
}

const std::vector<FlightEvent> &FlightState::Step (uint64_t v, const FlightSample *sp, bool quiet)
{
	if (v != vessel) { Reset (); vessel = v; }
	events.clear ();
	branch = NONE;
	sampled = vessel && sp;
	if (!sampled) return events;
	const FlightSample &s = *sp;
	now = s;
	wasFirst = st.first;
	// (time acceleration: nothing published, the state kept up)
	if (!quiet || st.first) {
		Heat (s);
		Flight (s);
		Docking (s);
		Modes (s);
		Ascent (s);
		Fuel (s);
	}
	Daylight (s, quiet);
	if (quiet) { st.first = true; events.clear (); return events; }
	st.first = false;
	Soi (s);
	return events;
}

void FlightState::Flight (const FlightSample &s)
{
	const double simt = s.t;
	const double alt = s.alt, airspeed = s.airspeed, ground = s.groundspeed, vs = s.vs;
	// on the ground: touching it, landed; or (not flying yet) standing still, as a rocket held on its
	// pad. A vessel "flying" standing still, its engines off, for a second: held - on a pad, a ground
	// it doesn't touch: on the ground, quietly; hovering takes thrust
	const bool held = ground < 0.1 && std::fabs (vs) < 0.05;
	if (held && st.flying && s.main <= 0.0 && s.hover <= 0.0 && s.retro <= 0.0) {
		if (st.heldSince < 0.0) st.heldSince = simt;
		else if (simt - st.heldSince >= 1.0) st.flying = false, st.rolledOut = true, st.airborneAt = 0.0;
	} else st.heldSince = -1.0;
	const bool contact = s.contact || alt <= 0.0 || (held && !st.flying);
	const double mach = s.atmP > ATM_P ? s.mach : 0.0;

	before = { st.prevAlt, st.prevVs, st.prevAirspeed, st.prevMach };
	if (st.first) {
		branch = FIRST;
		st.prevAlt = alt, st.prevVs = vs, st.prevAirspeed = airspeed, st.prevMach = mach;
		st.flying = !contact;
		st.rolledOut = !st.flying && ground < 0.5;
		st.airborneAt = st.flying ? simt - AIRBORNE_AFTER : 0.0;
		return;
	}

	if (contact) {
		st.airborneAt = 0.0;
		if (st.flying) {
			branch = TOUCHDOWN_STEP;
			const double sink = std::max (-vs, -st.prevVs);
			st.flying = false;
			st.rolledOut = false;
			st.ascent = false;
			Emit (FlightEvent::TOUCHDOWN, sink);
			st.padIgnition = st.runway = false;
			// down on another world, from orbit
			if (st.orbited && !s.surfaceName.empty () && s.surfaceName != "Earth") Emit (FlightEvent::SURFACE, 0.0, s.surfaceName);
			st.orbited = false;
		} else {
			branch = ROLLING;
			if (!st.rolledOut && ground < 0.2) {
				st.rolledOut = true;
				Emit (FlightEvent::ROLLOUT);
				if (st.flewHigh) Emit (FlightEvent::LANDED); // (home from a flight to space)
				st.flewHigh = false;
			}
		}
	} else if (!st.flying) {
		branch = LEAVING;
		// off the ground: flying after a moment (not a bounce)
		if (st.airborneAt == 0.0) st.airborneAt = simt;
		else if (simt - st.airborneAt >= AIRBORNE_AFTER) {
			st.flying = true;
			st.rolledOut = true;
			// (a rocket's - lit on its pad, or its engines pointing up - not a plane's or a hover's)
			const bool rocket = st.padIgnition || (s.main > 0.0 && s.mainUp);
			Emit (FlightEvent::LIFTOFF, rocket ? 1.0 : 0.0);
			st.liftoffAt = simt, st.rollSaid = false;
			st.runway = !rocket && ground > RUNWAY_SPEED, st.posRateSaid = false;
			st.ascent = true, st.maxqSaid = st.mecoSaid = st.orbitSaid = false, st.qMax = 0.0, st.mainOffAt = -1.0;
		}
	} else {
		branch = FLYING;
		// a runway takeoff climbing away (once; said in its first half minute)
		if (st.runway && !st.posRateSaid && vs > 2.0) {
			st.posRateSaid = true;
			if (simt - st.airborneAt < 30.0) Emit (FlightEvent::POSITIVE_RATE);
		}
	}
	st.prevAlt = alt, st.prevVs = vs, st.prevAirspeed = airspeed, st.prevMach = mach;
}

void FlightState::Docking (const FlightSample &s)
{
	const double dist = s.docked ? -1.0 : s.dockDist;
	prevDock = st.prevDist;
	if (st.first || s.docked) { st.prevDist = s.docked ? -1.0 : s.dockDist; prevDock = -1.0; return; }
	// inside a kilometre of the port (once; again after backing out past 2 km)
	if (dist >= 0.0 && st.prevDist > 1000.0 && dist <= 1000.0 && !st.approachSaid) { Emit (FlightEvent::APPROACH); st.approachSaid = true; }
	if (dist > 2000.0) st.approachSaid = false;
	st.prevDist = dist;
}

void FlightState::Modes (const FlightSample &s)
{
	if (!st.first) {
		if ((s.nav != 0) != (st.prevNav != 0) || (s.nav && s.nav != st.prevNav)) Emit (FlightEvent::NAV, s.nav);
		if (s.rcs != st.prevRcs) Emit (FlightEvent::RCS_MODE, s.rcs);
		if (s.af != st.prevAf) Emit (FlightEvent::AF_MODE, s.af);
	}
	st.prevNav = s.nav, st.prevRcs = s.rcs, st.prevAf = s.af;
}

void FlightState::Ascent (const FlightSample &s)
{
	const double simt = s.t, main = s.main;
	const bool contact = !st.flying; // (Flight: on the ground, or held on a pad)
	if (st.first) { st.prevMain = main; return; }

	// ignition: the main engines lit on the ground, pointing up (a rocket on its pad, not a plane on
	// its runway)
	if (contact && st.prevMain <= 0.0 && main > 0.0 && s.hasMain && s.mainUp) { Emit (FlightEvent::IGNITION); st.padIgnition = true; }

	const bool orbit = s.hasRef && s.orbit && !s.escape;   // (a flyby's periapsis may be high: not an orbit)
	const bool escape = s.hasRef && s.escape;
	if (orbit) st.orbited = true;
	// deorbit: a burn (main or retro engines) begun in orbit that ends with the periapsis in the
	// atmosphere or the ground
	const bool burning = main > 0.0 || s.retro > 0.0;
	if (burning && !st.prevBurning) st.orbitAtBurn = orbit, st.escapeAtBurn = escape;
	if (!burning && st.prevBurning && s.hasRef && !contact) {
		if (st.orbitAtBurn && !orbit && !escape) { Emit (FlightEvent::DEORBIT); st.deorbitAt = simt; }
		// into orbit about a body (not a launch's: "orbit achieved"), or onto an escape path
		else if (!st.orbitAtBurn && orbit && !st.ascent) Emit (FlightEvent::CAPTURE);
		else if (!st.escapeAtBurn && escape) Emit (FlightEvent::ESCAPE);
	}
	st.prevBurning = burning;
	st.inOrbit = orbit;
	// a powered descent on an airless world (the Moon): from orbit, braking down below 15 km - once,
	// not right after the deorbit burn (the same moment)
	if (s.hasRef && st.orbited && !st.descentSaid && !contact) {
		const bool braking = s.hover > 0.2 || s.retro > 0.2 || main > 0.2;
		if (s.airless && braking && s.hvUp < -1.0 && s.altitude < 15e3 && simt - st.deorbitAt > 300.0) {
			Emit (FlightEvent::DESCENT);
			st.descentSaid = true;
		}
	}
	if (contact || s.altitude > 30e3) st.descentSaid = false;

	// a rocket's roll program: rolling about its thrust axis (faster than 2 deg/s, for 2 s - not a
	// lurch as the engines light) from 5 s to 40 s off the pad
	const double sinceLiftoff = simt - st.liftoffAt;
	if (st.padIgnition && st.flying && !st.rollSaid && sinceLiftoff > 5.0 && sinceLiftoff < 40.0 && main > 0.0 && s.hasMain) {
		st.rolling = s.rollRate > 0.035 ? st.rolling + s.dt : 0.0;
		if (st.rolling >= 2.0) { Emit (FlightEvent::ROLL_PROGRAM); st.rollSaid = true; }
	}

	if (st.ascent) {
		// Max Q: the dynamic pressure past its peak (5 % down from it), in a powered climb (not a
		// plane slowing down)
		const bool climbing = main > 0.0 && s.vs > 0.0;
		const double q = s.q;
		if (!climbing) st.qMax = 0.0;
		else if (q > st.qMax) st.qMax = q;
		if (!st.maxqSaid && climbing && st.qMax > MAXQ_MIN && q < 0.95 * st.qMax) {
			Emit (FlightEvent::MAXQ);
			st.maxqSaid = true;
		}
		// MECO: the main engines off (half a second) on the way up: off the ground, above 10 km or out
		// of the atmosphere
		if (main > 0.0) st.mainOffAt = -1.0;
		else if (st.prevMain > 0.0) st.mainOffAt = simt;
		if (!st.mecoSaid && st.mainOffAt >= 0.0 && simt - st.mainOffAt >= 0.5 && (s.altitude > 10e3 || s.atmP < ATM_P)) {
			Emit (FlightEvent::MECO);
			st.mecoSaid = true;
		}
		if (!st.orbitSaid && orbit) {
			Emit (FlightEvent::ORBIT);
			st.orbitSaid = true;
			st.flewHigh = true;
			st.ascent = false;
		}
	}
	st.prevMain = main;
}

void FlightState::Fuel (const FlightSample &s)
{
	const double fm = s.mainFuel, fr = s.rcsFuel;
	if (!st.first) {
		// (the main engines on another vessel's tank - the Shuttle's external tank, docked - one meant to
		// run dry and be dropped: no main fuel warnings from it)
		if (fm >= 0.0 && st.prevMainFuel >= 0.0 && !s.mainExternal) {
			if (st.prevMainFuel > 0.0 && fm <= 0.0) Emit (FlightEvent::MAIN_FUEL, 0.0);
			else if (st.prevMainFuel >= LOW_FUEL && fm < LOW_FUEL) Emit (FlightEvent::MAIN_FUEL, 1.0);
			else for (double f : FUEL_STEPS) if (st.prevMainFuel >= f && fm < f) Emit (FlightEvent::MAIN_FUEL, f > 0.15 ? 20.0 : 10.0);
		}
		if (fr >= 0.0 && st.prevRcsFuel >= 0.0) {
			if (st.prevRcsFuel > 0.0 && fr <= 0.0) Emit (FlightEvent::RCS_FUEL, 0.0);
			else if (st.prevRcsFuel >= LOW_FUEL && fr < LOW_FUEL) Emit (FlightEvent::RCS_FUEL, 1.0);
			else for (double f : FUEL_STEPS) if (st.prevRcsFuel >= f && fr < f) Emit (FlightEvent::RCS_FUEL, f > 0.15 ? 20.0 : 10.0);
		}
	}
	st.prevMainFuel = fm, st.prevRcsFuel = fr;
}

void FlightState::Heat (const FlightSample &s)
{
	// the hull heating up (an entry): at 80 % of what its hottest part stands, again at 95 %, before
	// parts burn off; again once it has cooled below 60 %
	const double f = s.heat;
	if (st.first) { st.heatSaid = f >= 0.95 ? 2 : f >= 0.8 ? 1 : 0; return; }
	if (f >= 0.95 && st.heatSaid < 2) { Emit (FlightEvent::HEAT, 2.0); st.heatSaid = 2; }
	else if (f >= 0.8 && st.heatSaid < 1) { Emit (FlightEvent::HEAT, 1.0); st.heatSaid = 1; }
	else if (f < 0.6) st.heatSaid = 0;
	// the structure loaded near what it stands (80 % of the weakest part's strength); again once
	// below half
	const double L = s.load;
	if (L >= 0.8 && !st.loadSaid) { Emit (FlightEvent::STRUCTURAL); st.loadSaid = true; }
	else if (L < 0.5) st.loadSaid = false;
}

void FlightState::Daylight (const FlightSample &s, bool quiet)
{
	const int nowDark = s.dark ? 1 : 0;
	if (dark < 0 || quiet) { dark = nowDark; lightChangedAt = -1.0; return; }   // (followed, not said)
	if (nowDark == dark) { lightChangedAt = -1.0; return; }
	if (lightChangedAt < 0.0) { lightChangedAt = s.t; return; }
	if (s.t - lightChangedAt < 60.0) return;   // (the light changed for a minute)
	dark = nowDark, lightChangedAt = -1.0;
	// (above the air, flying: an orbit's sunrise and sunset, not a day on the ground)
	if (!st.flying || s.lowAir) return;
	Emit (FlightEvent::LIGHT, dark);
}

void FlightState::Soi (const FlightSample &s)
{
	// the body it orbits changed (the first one seen, and a vessel at a boundary flipping, not told)
	const void *ref = s.elRef;
	if (!ref) return;
	// (leaving Earth for the Sun)
	if (soiRef && ref != soiRef && soiName == "Earth" && s.elRefName == "Sun") Emit (FlightEvent::DEEPSPACE);
	if (soiRef && ref != soiRef && s.t - soiAt > 60.0) {
		Emit (FlightEvent::SOI, 0.0, s.elRefName);
		soiAt = s.t;
	}
	if (ref != soiRef && !soiRef) soiAt = s.t;
	soiRef = ref, soiName = s.elRefName;
}

// ---- traces ---------------------------------------------------------------------

void WriteSample (FILE *f, const FlightSample &s)
{
	fprintf (f, "%.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %d %.17g %.17g %.17g %.17g %d %d %.17g "
		"%d %d %d %d %p %.17g %d %d %d %d %.17g %.17g %d %.17g %.17g %d %d |%s|%s|\n",
		s.t, s.dt, s.alt, s.altitude, s.airspeed, s.groundspeed, s.vs, s.hvUp, s.atmP, s.mach, s.q, (int)s.contact, s.gear,
		s.main, s.hover, s.retro, (int)s.hasMain, (int)s.mainUp, s.rollRate,
		(int)s.hasRef, (int)s.orbit, (int)s.escape, (int)s.airless, s.elRef, s.dockDist, (int)s.docked, s.nav, s.rcs, s.af,
		s.mainFuel, s.rcsFuel, (int)s.mainExternal, s.heat, s.load, (int)s.dark, (int)s.lowAir,
		s.elRefName.c_str (), s.surfaceName.c_str ());
}

bool ReadSample (const std::string &line, FlightSample &s)
{
	s = FlightSample ();
	int contact, hasMain, mainUp, hasRef, orbit, escape, airless, docked, mainExternal, dark, lowAir;
	void *ref = nullptr;
	int n = sscanf (line.c_str (), "%lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %d %lf %lf %lf %lf %d %d %lf "
		"%d %d %d %d %p %lf %d %d %d %d %lf %lf %d %lf %lf %d %d",
		&s.t, &s.dt, &s.alt, &s.altitude, &s.airspeed, &s.groundspeed, &s.vs, &s.hvUp, &s.atmP, &s.mach, &s.q, &contact, &s.gear,
		&s.main, &s.hover, &s.retro, &hasMain, &mainUp, &s.rollRate,
		&hasRef, &orbit, &escape, &airless, &ref, &s.dockDist, &docked, &s.nav, &s.rcs, &s.af,
		&s.mainFuel, &s.rcsFuel, &mainExternal, &s.heat, &s.load, &dark, &lowAir);
	if (n != 36) return false;
	s.contact = contact, s.hasMain = hasMain, s.mainUp = mainUp, s.hasRef = hasRef, s.orbit = orbit, s.escape = escape;
	s.airless = airless, s.docked = docked, s.mainExternal = mainExternal, s.dark = dark, s.lowAir = lowAir;
	s.elRef = ref;
	const size_t a = line.find ('|');
	const size_t b = a == std::string::npos ? a : line.find ('|', a + 1);
	const size_t c = b == std::string::npos ? b : line.find ('|', b + 1);
	if (c == std::string::npos) return false;
	s.elRefName = line.substr (a + 1, b - a - 1);
	s.surfaceName = line.substr (b + 1, c - b - 1);
	return true;
}

} // namespace audio
