// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// The flight as the sound follows it (FlightState): its events from samples,
// without Orbiter -
//  - traces recorded in the game (ORBITER_AUDIO_FLIGHT_TRACE: the Shuttle's
//    liftoff, Tests/Audio/Ascent.trace; the DG's takeoff and modes,
//    Tests/Audio/Callouts.trace): their events, in order, at their times
//  - tables of samples for the rest: Max Q, MECO, orbit, a deorbit burn, a
//    capture, an escape, a powered descent, the roll program, fuel, heat,
//    the structure, sunset, a new sphere of influence, a landing on another
//    world and one at home, time acceleration

#include <catch2/catch_test_macros.hpp>
#include "Audio/FlightState.h"
#include "Audio/CrewLines.h"
#include "Audio/DebugEnv.h"
#include <map>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

using namespace audio;
using E = FlightEvent;

namespace {

/// The events, one line each: "<name> <value>[ <text>] at <t>"
std::string Run (FlightState &f, const std::vector<FlightSample> &samples, std::function<bool (int)> quiet = nullptr)
{
	std::ostringstream out;
	for (size_t i = 0; i < samples.size (); i++) {
		for (const FlightEvent &e : f.Step (1, &samples[i], quiet && quiet ((int)i))) {
			char line[128];
			snprintf (line, sizeof (line), "%s %g%s%s at %.2f\n", FlightEventName (e.kind), e.value, e.text.empty () ? "" : " ", e.text.c_str (), samples[i].t);
			out << line;
		}
	}
	return out.str ();
}

std::vector<FlightSample> Trace (const char *file)
{
	std::vector<FlightSample> v;
	std::ifstream in (file);
	std::string line;
	FlightSample s;
	while (std::getline (in, line)) if (ReadSample (line, s)) v.push_back (s);
	return v;
}

/// A table's samples: from a start, each step changed by a function of its time
std::vector<FlightSample> Table (FlightSample s, double t1, std::function<void (FlightSample&)> at, double dt = 0.1)
{
	std::vector<FlightSample> v;
	s.dt = dt;
	for (int i = 0; i * dt <= t1 + 1e-9; i++) {
		FlightSample x = s;
		x.t = i * dt;
		at (x);
		v.push_back (x);
	}
	return v;
}

FlightSample InOrbit ()
{
	FlightSample s;
	s.alt = s.altitude = 300e3;
	s.airspeed = s.groundspeed = 7700.0;
	s.hasMain = true, s.hasRef = true, s.orbit = true;
	s.elRef = &s, s.elRefName = "Earth", s.surfaceName = "Earth";
	s.mainFuel = s.rcsFuel = 0.9;
	return s;
}

} // namespace

TEST_CASE ("Recorded: the Shuttle's liftoff", "[audio][flight]")
{
	auto tr = Trace (TESTS_DIR "/Audio/Ascent.trace");
	REQUIRE (tr.size () == 1500);
	FlightState f;
	CHECK (Run (f, tr) ==
		"ignition 0 at 2.02\n"
		"liftoff 1 at 2.54\n");
	CHECK (f.Flying ());
	CHECK (f.Ascending ());
}

TEST_CASE ("Recorded: the DG's modes, takeoff, climb", "[audio][flight]")
{
	auto tr = Trace (TESTS_DIR "/Audio/Callouts.trace");
	REQUIRE (tr.size () > 3000);
	FlightState f;
	CHECK (Run (f, tr) ==
		"rcs-mode 2 at 1.02\n"
		"rcs-mode 1 at 2.52\n"
		"nav 2 at 4.02\n"
		"nav 0 at 5.54\n"
		"af-mode 2 at 7.04\n"
		"af-mode 0 at 8.54\n"
		"liftoff 0 at 30.48\n"
		"positive-rate 0 at 30.74\n"
		"meco 0 at 59.18\n"
		"orbit 0 at 62.70\n");
}

TEST_CASE ("A rocket to orbit: ignition, liftoff, roll, Max Q, MECO, orbit", "[audio][flight]")
{
	FlightSample s;
	s.contact = true, s.hasMain = true, s.mainUp = true, s.hasRef = true;
	s.atmP = 101325.0, s.elRef = &s, s.elRefName = "Earth", s.surfaceName = "Earth";
	FlightState f;
	auto tr = Table (s, 200.0, [] (FlightSample &x) {
		const double t = x.t;
		if (t >= 1.0 && t < 150.0) x.main = 1.0;                  // (lit at 1 s, cut at 150 s)
		if (t >= 2.0) {                                           // (off the pad at 2 s)
			x.contact = false;
			x.alt = x.altitude = 50.0 * (t - 2.0) * (t - 2.0);
			x.vs = x.airspeed = 100.0 * (t - 2.0);
			x.groundspeed = x.airspeed;
			x.q = 30000.0 * std::exp (-std::pow ((t - 60.0) / 20.0, 2.0)); // (Max Q at 60 s)
			x.atmP = 101325.0 * std::exp (-x.altitude / 8000.0);
		}
		if (t >= 10.0 && t < 14.0) x.rollRate = 0.1;              // (the roll program)
		if (t >= 180.0) x.orbit = true;
	});
	const std::string ev = Run (f, tr);
	CHECK (ev ==
		"ignition 0 at 1.00\n"
		"liftoff 1 at 2.60\n"            // (at 2 s still on the ground: altitude 0)
		"roll-program 0 at 11.90\n"      // (2 s of rolling from 10 s)
		"maxq 0 at 64.60\n"
		"meco 0 at 150.50\n"
		"orbit 0 at 180.00\n");
	CHECK (!f.Ascending ());
	CHECK (f.InOrbit ());
}

TEST_CASE ("In orbit: a capture, a deorbit, a powered descent, an escape", "[audio][flight]")
{
	FlightState f;
	// (a burn ending in orbit about a body - not a launch's: a capture)
	FlightSample s = InOrbit ();
	auto capture = Table (s, 20.0, [] (FlightSample &x) {
		x.orbit = x.t > 10.0;
		if (x.t >= 5.0 && x.t < 12.0) x.main = 1.0;
	});
	CHECK (Run (f, capture) == "capture 0 at 12.00\n");
	// (a burn in orbit ending with the periapsis in the ground: a deorbit; airless; braking down below
	// 15 km five minutes later: a powered descent)
	f.Reset ();
	s.airless = true;
	auto down = Table (s, 700.0, [] (FlightSample &x) {
		const double t = x.t;
		if (t >= 10.0 && t < 20.0) x.retro = 1.0;
		x.orbit = t < 15.0;
		if (t >= 400.0) x.altitude = x.alt = 12e3, x.hvUp = -20.0, x.hover = 0.5;
	}, 0.5);
	CHECK (Run (f, down) == "deorbit 0 at 20.00\ndescent 0 at 400.00\n");
	// (a burn onto an open path)
	f.Reset ();
	s = InOrbit ();
	auto out = Table (s, 30.0, [] (FlightSample &x) {
		if (x.t >= 5.0 && x.t < 20.0) x.main = 1.0;
		if (x.t >= 15.0) x.escape = true, x.orbit = false;
	});
	CHECK (Run (f, out) == "escape 0 at 20.00\n");
}

TEST_CASE ("Fuel, heat, the structure: once each way, with hysteresis", "[audio][flight]")
{
	FlightState f;
	FlightSample s = InOrbit ();
	auto tr = Table (s, 100.0, [] (FlightSample &x) {
		const double t = x.t;
		x.mainFuel = std::max (0.0, 0.305 - 0.004 * t);           // (20 %, 10 %, low, empty)
		x.heat = t < 40.0 ? (t + 0.5) / 40.0 : t < 60.0 ? 0.5 : 0.85; // (hot, critical; cooled; hot again)
		x.load = t >= 30.0 && t < 35.0 ? 0.9 : 0.1;
	}, 1.0);
	const std::string ev = Run (f, tr);
	CHECK (ev ==
		"main-fuel 20 at 27.00\n"
		"structural 0 at 30.00\n"
		"heat 1 at 32.00\n"
		"heat 2 at 38.00\n"
		"main-fuel 10 at 52.00\n"
		"heat 1 at 60.00\n"
		"main-fuel 1 at 64.00\n"
		"main-fuel 0 at 77.00\n");
}

TEST_CASE ("Sunset in orbit, a new sphere of influence, deep space", "[audio][flight]")
{
	FlightState f;
	FlightSample s = InOrbit ();
	int moon, sun;
	auto tr = Table (s, 400.0, [&] (FlightSample &x) {
		const double t = x.t;
		x.dark = t >= 50.0 && t < 100.0;       // (dark at 50 s: told after a minute of it; light again before)
		if (t >= 200.0) x.dark = true;          // (told at 260 s)
		if (t >= 120.0) x.elRef = &moon, x.elRefName = "Moon";
		if (t >= 300.0) x.elRef = &sun, x.elRefName = "Sun";
	}, 1.0);
	CHECK (Run (f, tr) ==
		"soi 0 Moon at 120.00\n"
		"light 1 at 260.00\n"
		"soi 0 Sun at 300.00\n");
	// (from Earth's straight into the Sun's: the replies will lag)
	FlightState g;
	auto away = Table (s, 100.0, [&] (FlightSample &x) { if (x.t >= 70.0) x.elRef = &sun, x.elRefName = "Sun"; }, 1.0);
	CHECK (Run (g, away) == "deepspace 0 at 70.00\nsoi 0 Sun at 70.00\n");
}

TEST_CASE ("Landings: on another world from orbit, at home from space", "[audio][flight]")
{
	FlightState f;
	FlightSample s = InOrbit ();
	s.surfaceName = "Moon";
	auto tr = Table (s, 40.0, [] (FlightSample &x) {
		const double t = x.t;
		x.orbit = t < 5.0;
		x.alt = x.altitude = std::max (0.0, 1000.0 - 50.0 * t);
		x.vs = t < 20.0 ? -2.0 : 0.0;
		x.contact = t >= 20.0;
		x.groundspeed = x.airspeed = t < 20.0 ? 5.0 : std::max (0.0, 5.0 - (t - 20.0));
	}, 0.5);
	CHECK (Run (f, tr) == "touchdown 2 at 20.00\nsurface 0 Moon at 20.00\nrollout 0 at 25.00\n");
	// (home: after an ascent to orbit, down again - the ground hears of the rollout)
	FlightState g;
	FlightSample h;
	h.hasRef = true, h.elRef = &h, h.elRefName = "Earth", h.surfaceName = "Earth", h.hasMain = true, h.mainUp = true;
	h.contact = true;
	auto home = Table (h, 60.0, [] (FlightSample &x) {
		const double t = x.t;
		x.main = t >= 1.0 && t < 10.0 ? 1.0 : 0.0;
		const bool up = t >= 2.0 && t < 40.0;
		x.contact = !up;
		x.alt = x.altitude = up ? 1000.0 : 0.0;
		x.vs = up ? (t < 30.0 ? 10.0 : -1.0) : 0.0;
		x.orbit = t >= 20.0 && t < 30.0;
		x.groundspeed = x.airspeed = up ? 50.0 : 0.0;
	}, 0.5);
	CHECK (Run (g, home) == "ignition 0 at 1.00\nliftoff 1 at 2.50\nmeco 0 at 10.50\norbit 0 at 20.00\n"
		"touchdown 1 at 40.00\nrollout 0 at 40.50\nlanded 0 at 40.50\n");
}

TEST_CASE ("Time acceleration: followed, nothing told; afresh after it", "[audio][flight]")
{
	FlightState f;
	FlightSample s = InOrbit ();
	auto tr = Table (s, 60.0, [] (FlightSample &x) {
		x.rcs = x.t >= 10.0 ? 1 : 0;           // (changed while accelerated)
		x.mainFuel = x.t < 30.0 ? 0.3 : 0.15;  // (20 % passed while accelerated)
		x.nav = x.t >= 50.0 ? 3 : 0;           // (after it: told)
	}, 1.0);
	CHECK (Run (f, tr, [] (int i) { return i >= 5 && i < 40; }) == "nav 3 at 50.00\n");
}

TEST_CASE ("The crew's lines: every one there, as the table says", "[audio][lines]")
{
	CrewLines t;
	REQUIRE (t.Read (SOURCE_DIR "/Sound/Voice/callouts.cfg"));
	CHECK (t.Size () > 60);
	// (each file: Orbiter's; XRSound's default recordings in XRSound's folder; a group's folder;
	// numbered variants, at least the first)
	auto there = [] (std::string f) {
		const size_t n = f.find ("{n}");
		if (n != std::string::npos) f.replace (n, 3, "1");
		const std::string xr = "XRSound/Default";
		if (f.compare (0, xr.size (), xr) == 0) f = "Sound/XRSound/assets/" + f;
		return std::filesystem::exists (SOURCE_DIR "/" + f);
	};
	for (const char *key : { "Touchdown", "Liftoff", "MaxQ", "MECO", "Orbit", "WarningGearIsUp", "AltitudeCalloutsGroup",
		"ChatterSunrise", "DriveEngaged", "CommBlackout", "DockingRadarBeep", "RollProgram" }) {
		const CrewLine *c = t.Find (key);
		REQUIRE (c);
		INFO (key << ": " << c->file);
		CHECK (there (c->file));
	}
	// (keys in any case; the table's columns)
	REQUIRE (t.Find ("warninggearisup"));
	CHECK (t.Find ("warninggearisup")->warning);
	CHECK (t.Find ("WarningGearIsUp")->wait == 2.5);
	CHECK (t.Find ("DockingRadarBeep")->gain == 0.4);
	CHECK (t.Find ("ChatterSunset")->gain == 0.6);
	CHECK (!t.Find ("NoSuchLine"));
	// every line's file
	std::ifstream in (SOURCE_DIR "/Sound/Voice/callouts.cfg");
	std::string line;
	int checked = 0;
	while (std::getline (in, line)) {
		const size_t bar = line.find ('|');
		if (line.empty () || line[0] == '#' || bar == std::string::npos) continue;
		std::string key = line.substr (0, bar);
		key.erase (key.find_last_not_of (" \t") + 1);
		const CrewLine *c = t.Find (key);
		REQUIRE (c);
		INFO (key << ": " << c->file);
		CHECK (there (c->file));
		checked++;
	}
	CHECK (checked == (int)t.Size ());
}

namespace {
	std::map<std::string, std::string> fakeEnv;
	const char *FakeGet (const char *k) { auto it = fakeEnv.find (k); return it == fakeEnv.end () ? nullptr : it->second.c_str (); }
}

TEST_CASE ("The sound's environment options: read as they always were", "[audio][env]")
{
	fakeEnv.clear ();
	DebugEnv d = DebugEnv::Read (FakeGet);
	CHECK (d.render.empty ());
	CHECK (d.device == -1);
	CHECK (d.log == 0);
	CHECK (d.callouts == -1);
	CHECK (d.vessels);
	CHECK (d.radio);
	CHECK (!d.contacts);
	CHECK (d.voices == 0);
	CHECK (d.radioCodec);
	fakeEnv = { {"ORBITER_AUDIO_RENDER", "x.wav"}, {"ORBITER_AUDIO", "0"}, {"ORBITER_AUDIO_LOG", ""}, {"ORBITER_AUDIO_CALLOUTS", "1"},
		{"ORBITER_AUDIO_MUSIC", "0"}, {"ORBITER_AUDIO_REALISM", "Cinematic"}, {"ORBITER_AUDIO_VOICE", "xrsound"},
		{"ORBITER_AUDIO_CAPCOM", "2"}, {"ORBITER_AUDIO_VOICES", "0"}, {"ORBITER_AUDIO_VESSELS", "0"}, {"ORBITER_AUDIO_CONTACTS", "1"},
		{"ORBITER_AUDIO_ASYNC", "1"}, {"ORBITER_RADIO_CODEC", "0"}, {"ORBITER_VK_OFFSCREEN", "1"}, {"ORBITER_AUDIO_SOLO", "voice,radio"} };
	d = DebugEnv::Read (FakeGet);
	CHECK (d.render == "x.wav");
	CHECK (d.device == 0);
	CHECK (d.log == 1);            // (set at all: on)
	CHECK (d.logStates);
	CHECK (d.callouts == 1);
	CHECK (d.music == 0);
	CHECK (d.realism == 1);
	CHECK (d.voice == 0);
	CHECK (d.capcom == 2);
	CHECK (d.voices == 1);         // (at least one)
	CHECK (!d.vessels);
	CHECK (d.contacts);
	CHECK (d.async);
	CHECK (!d.radioCodec);
	CHECK (d.offscreen);
	CHECK (d.solo == "voice,radio");
	fakeEnv = { {"ORBITER_AUDIO_LOG", "2"}, {"ORBITER_AUDIO_REALISM", "physical"}, {"ORBITER_AUDIO_VOICE", "orbiter"}, {"ORBITER_VK_OFFSCREEN", "0"}, {"ORBITER_AUDIO_CAPCOM", "3"} };
	d = DebugEnv::Read (FakeGet);
	CHECK (d.log == 2);
	CHECK (d.logStates);
	CHECK (d.realism == 0);
	CHECK (d.voice == 1);
	CHECK (!d.offscreen);
	CHECK (d.capcom == 0);
}
