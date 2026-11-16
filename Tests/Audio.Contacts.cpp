// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// What a vessel's contacts do (ContactModel), from samples, without Orbiter:
// a touchdown - the main gear, then the nose; a contact kept doesn't strike
// again unless it closes hard; a point that left strikes anew, one missed
// step doesn't make it leave; the hull's points strike as one blow; the
// levels of rolling, braking and scraping; docking's energy; the log's lines

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "Audio/Contacts.h"
#include <algorithm>
#include <cmath>

using namespace audio;
using Catch::Matchers::WithinRel;
using Catch::Matchers::ContainsSubstring;

namespace {

ContactSample Wheel (int id, double vn, double Fn = 5e4, double roll = 0.0)
{
	ContactSample s;
	s.id = id, s.kind = ContactKind::WHEEL, s.vn = vn, s.Fn = Fn, s.slipLong = roll, s.meff = 6000.0;
	return s;
}

ContactSample Hull (double vn, double meff = 1e4)
{
	ContactSample s;
	s.kind = ContactKind::HULL, s.vn = vn, s.Fn = 1e5, s.meff = meff;
	return s;
}

const double H = 0.02;

} // namespace

TEST_CASE ("Contacts: the main gear touches down, then the nose", "[contacts]")
{
	ContactModel m;
	m.Step (1.00, H, {Wheel (1, 2.0), Wheel (2, 2.0)}, SurfaceKind::PAVED);
	m.Step (1.02, H, {Wheel (1, 0.1), Wheel (2, 0.1)}, SurfaceKind::PAVED);
	m.Step (1.04, H, {Wheel (1, 0.0), Wheel (2, 0.0), Wheel (0, 1.5)}, SurfaceKind::PAVED);
	const auto &e = m.Events ();
	REQUIRE (e.size () == 3);
	CHECK (e[0].id == 1);
	CHECK (e[1].id == 2);
	CHECK (e[2].id == 0);
	CHECK (e[2].t == 1.04);
	for (const auto &x : e) {
		CHECK (x.type == ContactEvent::IMPACT);
		CHECK (x.kind == ContactKind::WHEEL);
		CHECK (x.surface == SurfaceKind::PAVED);
	}
	CHECK_THAT (e[0].energy, WithinRel (0.5 * 6000.0 * 4.0, 1e-9));
	CHECK_THAT (e[2].speed, WithinRel (1.5, 1e-9));
}

TEST_CASE ("Contacts: a contact kept strikes again only closing hard", "[contacts]")
{
	ContactModel m;
	double t = 1.0;
	m.Step (t, H, {Wheel (1, 1.0)}, SurfaceKind::PAVED);
	REQUIRE (m.Events ().size () == 1);
	m.ClearEvents ();
	// (rolling on: small closings, a bump at 0.4 m/s - below RESTRIKE_V)
	for (int i = 0; i < 50; i++) m.Step (t += H, H, {Wheel (1, i == 20 ? 0.4 : 0.02)}, SurfaceKind::PAVED);
	CHECK (m.Events ().empty ());
	// a hard bump: struck again; once within STRIKE_GAP
	m.Step (t += H, H, {Wheel (1, 0.8)}, SurfaceKind::PAVED);
	m.Step (t += H, H, {Wheel (1, 0.8)}, SurfaceKind::PAVED);
	CHECK (m.Events ().size () == 1);
}

TEST_CASE ("Contacts: a strut compressing is one strike; a bounce in contact another", "[contacts]")
{
	ContactModel m;
	double t = 1.0;
	// (the wheel down at 2 m/s, its strut taking it over a quarter second: closing ever more slowly)
	for (double v : {2.0, 1.9, 1.8, 1.77, 1.6, 1.4, 1.2, 1.0, 0.8, 0.6, 0.4, 0.2, 0.0}) m.Step (t += H, H, {Wheel (1, v)}, SurfaceKind::PAVED);
	CHECK (m.Events ().size () == 1);
	m.ClearEvents ();
	// (rebounding - opening, still touching - then down again hard)
	for (double v : {-0.3, -0.5, -0.2, 0.3, 0.9}) m.Step (t += H, H, {Wheel (1, v)}, SurfaceKind::PAVED);
	REQUIRE (m.Events ().size () == 1);
	CHECK_THAT (m.Events ()[0].speed, WithinRel (0.9, 1e-9));
}

TEST_CASE ("Contacts: a point that left strikes anew; a missed step isn't leaving", "[contacts]")
{
	ContactModel m;
	double t = 1.0;
	m.Step (t, H, {Wheel (0, 1.0)}, SurfaceKind::SOIL);
	m.ClearEvents ();
	// (one step without it: within LEFT_AFTER - touching still, a slow closing)
	t += 2 * H;
	m.Step (t, H, {Wheel (0, 0.2)}, SurfaceKind::SOIL);
	CHECK (m.Events ().empty ());
	// gone for a quarter second (a bounce): back, it strikes (above STRIKE_V)
	t += 0.25;
	m.Step (t, H, {Wheel (0, 0.2)}, SurfaceKind::SOIL);
	REQUIRE (m.Events ().size () == 1);
	CHECK (m.Events ()[0].surface == SurfaceKind::SOIL);
	m.ClearEvents ();
	// ... not when it merely rests down again
	t += 0.25;
	m.Step (t, H, {Wheel (0, 0.01)}, SurfaceKind::SOIL);
	CHECK (m.Events ().empty ());
}

TEST_CASE ("Contacts: the hull's points strike as one blow", "[contacts]")
{
	ContactModel m;
	m.Step (2.0, H, {Hull (3.0), Hull (4.0), Hull (1.0, 2e4)}, SurfaceKind::PAVED);
	REQUIRE (m.Events ().size () == 1);
	const ContactEvent &e = m.Events ()[0];
	CHECK (e.kind == ContactKind::HULL);
	CHECK (e.id == -1);
	CHECK (e.points == 3);
	CHECK_THAT (e.speed, WithinRel (4.0, 1e-9));
	CHECK_THAT (e.energy, WithinRel (0.5 * 1e4 * 16.0, 1e-9)); // (the largest: not their sum)
	// wheels and the hull apart: each its own
	m.ClearEvents ();
	m.Step (5.0, H, {Hull (1.0), Wheel (1, 1.0)}, SurfaceKind::PAVED);
	CHECK (m.Events ().size () == 2);
}

TEST_CASE ("Contacts: the levels of rolling, braking, scraping", "[contacts]")
{
	ContactModel m;
	ContactSample a = Wheel (1, 0.0, 4e4, 30.0), b = Wheel (2, 0.0, 6e4, 31.0);
	b.brake = 1.0, b.Flong = -1e4;      // (braked: friction along)
	a.Flat = 2e3, a.slipLat = 0.5;      // (a sideways slip)
	ContactSample h = Hull (0.0);
	h.Flong = 3e4, h.slipLong = 10.0, h.slipLat = 0.0;
	m.Step (3.0, H, {a, b, h}, SurfaceKind::PAVED);
	const ContactLevels &l = m.Levels ();
	CHECK (l.any);
	CHECK_THAT (l.wheelLoad, WithinRel (1e5, 1e-9));
	CHECK_THAT (l.rollSpeed, WithinRel (31.0, 1e-9));
	CHECK_THAT (l.scrubPower, WithinRel (2e3 * 0.5, 1e-9));
	CHECK_THAT (l.brakePower, WithinRel (1e4 * 31.0, 1e-9)); // (only the braked wheel's)
	CHECK_THAT (l.scrapePower, WithinRel (3e4 * 10.0, 1e-9));
	CHECK_THAT (l.scrapeSpeed, WithinRel (10.0, 1e-9));
	// a step without contacts: nothing
	m.Step (3.02, H, {}, SurfaceKind::PAVED);
	CHECK_FALSE (m.Levels ().any);
}

TEST_CASE ("Contacts: docking, undocking, a part breaking off", "[contacts]")
{
	ContactModel m;
	m.Dock (10.0, V3 (0, 0, 5), 0.2, 1.2e4);
	m.Undock (20.0, V3 (0, 0, 5), 0.1);
	m.Break (30.0, V3 (1, 0, -3), 850.0, 12.0);
	const auto &e = m.Events ();
	REQUIRE (e.size () == 3);
	CHECK (e[0].type == ContactEvent::DOCK);
	CHECK_THAT (e[0].energy, WithinRel (0.5 * 1.2e4 * 0.04, 1e-9));
	CHECK (e[1].type == ContactEvent::UNDOCK);
	CHECK (e[2].type == ContactEvent::BREAK);
	CHECK (e[2].mass == 850.0);
	// the log's lines
	CHECK_THAT (Describe (e[0]), ContainsSubstring ("dock at 10.000 s: 0.200 m/s"));
	CHECK_THAT (Describe (e[2]), ContainsSubstring ("break at 30.000 s: 850 kg, 12.00 m/s"));
	ContactModel w;
	w.Step (1.0, H, {Wheel (2, 1.25)}, SurfaceKind::REGOLITH);
	CHECK (Describe (w.Events ()[0]) == "impact wheel 2 at 1.000 s: 1.25 m/s, 4688 J, regolith");
	ContactEvent c;
	c.type = ContactEvent::CRASH, c.t = 5.0, c.speed = 50.0, c.mass = 1e4, c.energy = 1.25e7;
	CHECK (Describe (c) == "crash at 5.000 s: 50.00 m/s, 1e+04 kg, 1.25e+07 J");
}

TEST_CASE ("Contacts: rolling rises with speed and load, silent at rest; braking by its power", "[contacts]")
{
	ContactLevels l;
	l.wheelLoad = ROLL_LOAD, l.rollSpeed = ROLL_V;
	CHECK_THAT (BedsFor (l).rollP, WithinRel (ROLL_P, 1e-9));
	// v^1.5, sqrt(load)
	l.rollSpeed = 2.25 * ROLL_V;
	CHECK_THAT (BedsFor (l).rollP, WithinRel (3.375 * ROLL_P, 1e-9));
	l.wheelLoad = 0.25 * ROLL_LOAD;
	CHECK_THAT (BedsFor (l).rollP, WithinRel (0.5 * 3.375 * ROLL_P, 1e-9));
	// (no faster than ROLL_VMAX)
	ContactLevels vmax = l;
	vmax.rollSpeed = 2.0 * ROLL_VMAX;
	CHECK_THAT (BedsFor (vmax).rollP, WithinRel (ROLL_P * std::pow (ROLL_VMAX / ROLL_V, 1.5) * 0.5, 1e-9));
	// faster: higher (ROLL_RATE0 at rest .. ROLL_RATE1)
	ContactLevels slow = l, fast = l;
	slow.rollSpeed = 10.0, fast.rollSpeed = 90.0;
	CHECK (BedsFor (slow).rollRate < BedsFor (fast).rollRate);
	CHECK (BedsFor (fast).rollRate <= ROLL_RATE1);
	// at rest, or nothing on the wheels: silent
	l.rollSpeed = 0.0;
	CHECK (BedsFor (l).rollP == 0.0);
	l.rollSpeed = 50.0, l.wheelLoad = 0.0;
	CHECK (BedsFor (l).rollP == 0.0);
	// braking: as the root of its power (and the tyres' scrub), capped; none: silent
	ContactLevels b;
	b.rollSpeed = 30.0, b.wheelLoad = ROLL_LOAD;
	CHECK (BedsFor (b).brakeP == 0.0);
	b.brakePower = BRAKE_W;
	CHECK_THAT (BedsFor (b).brakeP, WithinRel (BRAKE_P, 1e-9));
	b.brakePower = 2.0 * BRAKE_W, b.scrubPower = 2.0 * BRAKE_W;
	CHECK_THAT (BedsFor (b).brakeP, WithinRel (2.0 * BRAKE_P, 1e-9));
	b.brakePower = 1e3 * BRAKE_W;
	CHECK_THAT (BedsFor (b).brakeP, WithinRel (4.0 * BRAKE_P, 1e-9));
	ContactLevels bs = b;
	bs.rollSpeed = 3.0;
	CHECK (BedsFor (bs).brakeRate < BedsFor (b).brakeRate);
}

namespace {
ContactSample Rolling (int id, double vn, double ground, double Fn = 4e4)
{
	ContactSample s = Wheel (id, vn, Fn, ground);
	return s;
}
}

TEST_CASE ("Contacts: a wheel chirps touching down over 15 m/s, once per touchdown", "[contacts]")
{
	ContactModel m;
	Chirps c;
	const double M = 12000.0;
	double t = 1.0;
	auto step = [&](std::vector<ContactSample> pts) {
		m.ClearEvents ();
		m.Step (t, H, pts, SurfaceKind::PAVED);
		auto r = c.Step (t, pts, m.Events (), M);
		t += H;
		return r;
	};
	// the mains down at 90 m/s: each chirps once, at its touchdown
	auto r = step ({Rolling (1, 1.2, 90.0), Rolling (2, 1.2, 90.0)});
	REQUIRE (r.size () == 2);
	CHECK (r[0].id == 1);
	CHECK (r[0].t == 1.0);
	// its spin-up: 0.6 m v / (mu Fn), the wheel 1 % of its static load (M g / 3)
	const double mw = Chirps::WHEEL_MASS * M / 3.0;
	CHECK_THAT (r[0].spinUp, WithinRel (std::clamp (0.6 * mw * 90.0 / (Chirps::MU_TYRE * 4e4), 0.04, 0.4), 1e-9));
	CHECK_THAT (r[0].work, WithinRel (0.3 * mw * 90.0 * 90.0, 1e-6));
	// rolling on, a bump struck again: no chirp (spun up)
	for (int i = 0; i < 20; i++) CHECK (step ({Rolling (1, 0.02, 89.0), Rolling (2, 0.02, 89.0)}).empty ());
	CHECK (step ({Rolling (1, 1.5, 88.0), Rolling (2, 0.02, 88.0)}).empty ());
	CHECK (m.Events ().size () == 1);   // (struck: an impact - but no chirp)
	// the nose down later: it chirps
	r = step ({Rolling (1, 0.0, 88.0), Rolling (2, 0.0, 88.0), Rolling (0, 1.0, 88.0)});
	REQUIRE (r.size () == 1);
	CHECK (r[0].id == 0);
	// a bounce: off 0.3 s, down again within 0.5 s of its chirp - none
	t += 0.3;
	CHECK (step ({Rolling (0, 1.0, 87.0)}).empty ());
	// slow (a hop at taxiing speed): none
	t += 2.0;
	CHECK (step ({Rolling (0, 1.0, 10.0)}).empty ());
	// down again after a long hop, fast: again
	t += 2.0;
	CHECK (step ({Rolling (0, 1.0, 60.0)}).size () == 1);
}
