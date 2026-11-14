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
