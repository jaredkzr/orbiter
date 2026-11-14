// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// The world (World.h) without Orbiter: a scene of our own (Sim.h) - bodies
// moving along given paths, a frame with air or without - and what is heard
// of each emitter: travel time, Doppler, the shock cone and its boom, the
// cockpit's hull and structure, vacuum; the handles and identities of
// emitters and bodies.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "Audio/AudioEngine.h"
#include "Audio/World.h"
#include <cmath>
#include <functional>
#include <map>

using namespace audio;
using Catch::Matchers::WithinRel;
using Catch::Matchers::WithinAbs;

namespace {

/// Bodies are only addresses to the world: these stand for them
int bodyA, bodyB, bodyC, planet;
const Body *A = reinterpret_cast<const Body*> (&bodyA);
const Body *B = reinterpret_cast<const Body*> (&bodyB);
const Body *C = reinterpret_cast<const Body*> (&bodyC);
const void *PLANET = &planet;

/// A scene of our own: each body's position a function of time; the frame
/// a planet at the origin, not turning, its air sea level's (or none)
struct TestScene : Scene {
	double t = 0.0;
	std::map<const Body*, std::function<V3 (double)>> path;
	std::map<const Body*, const void*> structure;
	bool air = true;

	bool PoseOf (const Body *b, Pose &p) const override
	{
		auto it = path.find (b);
		if (it == path.end ()) return false;
		p.pos = it->second (t);
		return true;
	}
	const void *StructureOf (const Body *b) const override
	{
		auto it = structure.find (b);
		return it == structure.end () ? b : it->second;
	}
	double SizeOf (const Body *) const override { return 5.0; }
	Air AirAt (const void *frame, const V3 &) const override { return frame && air ? Air::SeaLevel () : Air (); }

	Snapshot At (double time, const V3 &ear, bool inside = false, const void *of = nullptr)
	{
		t = time;
		Snapshot s;
		s.t = s.playT = time;
		s.listener = true;
		s.ear.pos = ear;
		s.frame = PLANET;
		s.air = air ? Air::SeaLevel () : Air ();
		s.inside = inside, s.insideOf = of;
		return s;
	}
};

struct Fixture {
	Engine e;
	Fixture () { Engine::Options o; o.mode = Engine::OFFLINE; e.Init (o); }
};

EmitterId AddTone (World &w, Engine &e, const Body *b, float hull = 0.0f, bool boom = false)
{
	EmitterDesc d;
	d.body = b;
	d.level = 1.0f;
	d.refDist = 10.0f;
	d.hull = hull;
	d.boom = boom;
	d.name = "tone";
	return w.Add (d, MakeToneSource (e, 440.0, 0.5));
}

} // namespace

TEST_CASE ("A subsonic fly-by: travel time, Doppler, spreading", "[audio][world]")
{
	Fixture f;
	World w (f.e);
	TestScene sc;
	// (100 m/s along x, 300 m abeam of the listener at the origin; overhead at t = 10 s)
	sc.path[A] = [] (double t) { return V3 (100.0 * (t - 10.0), 300.0, 0.0); };
	EmitterId id = AddTone (w, f.e, A);
	REQUIRE (w.Get (id));
	double dopIn = 0.0, dopOut = 0.0;
	for (int k = 0; k <= 2000; k++) {
		const double t = k * 0.01;
		w.Update (sc.At (t, V3 ()), sc);
		const Emitter::State &h = w.Get (id)->Heard ();
		if (k == 500) {
			// approaching: heard from where it was a travel time ago
			REQUIRE (h.images == 1);
			const V3 heardFrom (100.0 * (t - h.delay - 10.0), 300.0, 0.0);
			CHECK_THAT (h.delay, WithinRel (heardFrom.Len () / C0, 1e-3));
			CHECK_THAT (h.dist, WithinRel (heardFrom.Len (), 1e-3));
			dopIn = h.doppler;
			// spreading from refDist (sea level at both ends: coupling 1)
			CHECK_THAT (h.air, WithinRel (10.0 / h.dist, 0.02));
		}
		if (k == 1500) dopOut = w.Get (id)->Heard ().doppler;
	}
	CHECK (dopIn > 1.05);
	CHECK (dopOut < 0.95);
	CHECK (dopIn < 1.0 / (1.0 - 100.0 / C0)); // (at most head-on)
}

TEST_CASE ("A supersonic pass: silence ahead of the cone, then one boom", "[audio][world]")
{
	Fixture f;
	World w (f.e);
	TestScene sc;
	// (Mach 2, 500 m overhead)
	sc.path[A] = [] (double t) { return V3 (2.0 * C0 * (t - 10.0), 500.0, 0.0); };
	int booms = 0;
	double boomT = -1.0, mach = 0.0;
	w.OnBoom ([&] (const World::Boom &b) { booms++; boomT = b.t; mach = b.mach; });
	EmitterId id = AddTone (w, f.e, A, 0.0f, true);
	bool silentAhead = true;
	for (int k = 0; k <= 2500; k++) {
		const double t = k * 0.01;
		w.Update (sc.At (t, V3 ()), sc);
		if (t > 4.0 && t < 9.0 && w.Get (id)->Heard ().images > 0) silentAhead = false;
	}
	CHECK (silentAhead);
	CHECK (booms == 1);
	CHECK_THAT (mach, WithinRel (2.0, 0.02));
	// (the cone, half-angle asin (1/M), arrives a moment after it passes overhead)
	const double expect = 10.0 + 500.0 * std::sqrt (2.0 * 2.0 - 1.0) / (2.0 * C0);
	CHECK_THAT (boomT, WithinAbs (expect, 0.05));
}

TEST_CASE ("In the cockpit: its own vessel through the structure, others muffled", "[audio][world]")
{
	Fixture f;
	World w (f.e);
	TestScene sc;
	int ship;
	sc.path[A] = [] (double) { return V3 (0.0, 0.0, 5.0); };   // (on the listener's ship)
	sc.path[B] = [] (double) { return V3 (0.0, 0.0, 15.0); };  // (docked to it: one structure)
	sc.path[C] = [] (double) { return V3 (200.0, 0.0, 0.0); }; // (another)
	sc.structure[A] = &ship, sc.structure[B] = &ship;
	EmitterId a = AddTone (w, f.e, A, 0.5f), b = AddTone (w, f.e, B, 0.5f), c = AddTone (w, f.e, C, 0.5f);
	for (int k = 0; k < 100; k++) w.Update (sc.At (k * 0.01, V3 (), true, &ship), sc);
	CHECK (w.Get (a)->Heard ().hull > 0.0f);
	CHECK (w.Get (b)->Heard ().hull > 0.0f);
	CHECK (w.Get (c)->Heard ().hull == 0.0f);
	// (through the hull, the outside world muffled: -20 dB, no highs)
	CHECK (w.Get (c)->Heard ().cutoff <= 700.0f);
	CHECK (w.Get (c)->Heard ().air < 0.1f * 10.0f / 200.0f * 1.01f);
	// the same from outside: no structure path, the air's highs
	for (int k = 100; k < 200; k++) w.Update (sc.At (k * 0.01, V3 ()), sc);
	CHECK (w.Get (a)->Heard ().hull == 0.0f);
	CHECK (w.Get (c)->Heard ().cutoff > 2000.0f); // (the air's own absorption over 200 m: some 3 kHz)
}

TEST_CASE ("Vacuum: nothing through it, the structure still carries", "[audio][world]")
{
	Fixture f;
	World w (f.e);
	TestScene sc;
	sc.air = false;
	int ship;
	sc.path[A] = [] (double) { return V3 (0.0, 0.0, 5.0); };
	sc.path[C] = [] (double) { return V3 (50.0, 0.0, 0.0); };
	sc.structure[A] = &ship;
	EmitterId a = AddTone (w, f.e, A, 0.5f), c = AddTone (w, f.e, C, 0.5f);
	for (int k = 0; k < 100; k++) w.Update (sc.At (k * 0.01, V3 (), true, &ship), sc);
	CHECK (w.Get (c)->Heard ().images == 0);
	CHECK (w.Get (c)->Heard ().air == 0.0f);
	CHECK (w.Get (a)->Heard ().air == 0.0f);
	CHECK (w.Get (a)->Heard ().hull > 0.0f);
	// cinematic: heard anyway, at once
	w.SetRealism (CINEMATIC);
	for (int k = 100; k < 200; k++) w.Update (sc.At (k * 0.01, V3 (), false), sc);
	CHECK (w.Get (c)->Heard ().images == 1);
	CHECK (w.Get (c)->Heard ().air > 0.0f);
	CHECK (w.Get (c)->Heard ().delay == 0.0);
}

TEST_CASE ("Emitters and bodies: handles and identities", "[audio][world]")
{
	Fixture f;
	World w (f.e);
	TestScene sc;
	sc.path[A] = [] (double) { return V3 (10.0, 0.0, 0.0); };
	sc.path[B] = [] (double) { return V3 (20.0, 0.0, 0.0); };
	EmitterId a = AddTone (w, f.e, A), b = AddTone (w, f.e, B);
	REQUIRE (a);
	REQUIRE (b);
	CHECK (a != b);
	const BodyRef ra = w.Ref (A), rb = w.Ref (B);
	CHECK (ra != rb);
	CHECK (w.Ref (A) == ra);
	// removed: its handle dead, even once its slot is used again
	w.Remove (a);
	CHECK (!w.Get (a));
	EmitterId a2 = AddTone (w, f.e, A);
	CHECK (a2.slot == a.slot);
	CHECK (!w.Get (a));
	CHECK (w.Get (a2));
	w.Remove (a); // (a dead handle: nothing)
	CHECK (w.Get (a2));
	// a body forgotten: its emitters and identity gone, the other's not
	w.Forget (A);
	CHECK (!w.Get (a2));
	CHECK (w.Get (b));
	CHECK (!w.Alive (ra));
	CHECK (w.Alive (rb));
	// (a new body at its address is another)
	const BodyRef ra2 = w.Ref (A);
	CHECK (ra2 != ra);
	CHECK (w.Alive (ra2));
	// a body no longer in the scene: silent, not a crash
	sc.path.erase (B);
	w.Update (sc.At (0.0, V3 ()), sc);
	CHECK (w.Get (b)->Heard ().air == 0.0f);
	w.Clear ();
	CHECK (!w.Get (b));
	CHECK (!w.Alive (rb));
}

TEST_CASE ("An emitter's parameters, as heard: a travel time ago, kept once while they hold", "[audio][world]")
{
	Fixture f;
	World w (f.e);
	TestScene sc;
	// (340.3 m off: a second's travel)
	sc.path[A] = [] (double) { return V3 (C0, 0.0, 0.0); };
	EmitterId id = AddTone (w, f.e, A);
	Emitter *e = w.Get (id);
	REQUIRE (e);
	// a throttle: 0 until 5 s, then 1; held for a minute
	for (int k = 0; k <= 6500; k++) {
		const double t = k * 0.01;
		e->Push (t, Floats<1>{{t >= 5.0 ? 1.0f : 0.0f}});
		w.Update (sc.At (t, V3 ()), sc);
		if (k == 550) CHECK (e->Recalled<Floats<1>> ()[0] == 0.0f);   // (5.5 s: what left it at 4.5 s)
		if (k == 610) CHECK (e->Recalled<Floats<1>> ()[0] == 1.0f);   // (6.1 s: at 5.1 s)
	}
	// (two values, each kept once - not 6500 moments)
	CHECK (e->HistoryLength () <= 2);
	// a value changing every step: a minute of it, no more
	for (int k = 6501; k <= 20000; k++) e->Push (k * 0.01, Floats<1>{{(float)k}});
	CHECK (e->HistoryLength () <= 6200);
	CHECK (e->HistoryLength () >= 6000);
	// time going back (a replay): afresh
	e->Push (10.0, Floats<1>{{7.0f}});
	CHECK (e->HistoryLength () == 1);
	Floats<1> last;
	REQUIRE (e->Latest (last));
	CHECK (last[0] == 7.0f);
}

TEST_CASE ("A recording on a body far off: heard when the sound of its start arrives", "[audio][world]")
{
	Fixture f;
	World w (f.e);
	TestScene sc;
	// (3 km off: 8.8 s)
	sc.path[A] = [] (double) { return V3 (3000.0, 0.0, 0.0); };
	sc.path[B] = [] (double) { return V3 (30.0, 0.0, 0.0); };
	EmitterDesc d;
	d.body = A, d.delayed = true, d.name = "far";
	EmitterId far = w.Add (d, MakeToneSource (f.e, 440.0, 0.5));
	d.body = B, d.name = "near";
	EmitterId near = w.Add (d, MakeToneSource (f.e, 440.0, 0.5));
	double farAt = -1.0, nearAt = -1.0;
	for (int k = 0; k <= 1200; k++) {
		const double t = k * 0.01;
		w.Update (sc.At (t, V3 ()), sc);
		if (farAt < 0.0 && w.Get (far)->Begun ()) farAt = t;
		if (nearAt < 0.0 && w.Get (near)->Begun ()) nearAt = t;
	}
	CHECK_THAT (farAt, WithinAbs (3000.0 / C0, 0.05));
	CHECK (nearAt < 0.2);
	// held: not begun while held, begun when let go (once its sound has come)
	d.body = A, d.name = "held";
	EmitterId held = w.Add (d, MakeToneSource (f.e, 440.0, 0.5));
	w.Get (held)->Hold (true);
	for (int k = 1201; k <= 2400; k++) w.Update (sc.At (k * 0.01, V3 ()), sc);
	CHECK (!w.Get (held)->Begun ());
	w.Get (held)->Hold (false);
	w.Update (sc.At (24.01, V3 ()), sc);
	CHECK (w.Get (held)->Begun ());
	// vacuum: no way for it - begun at once (unheard; it ends)
	sc.air = false;
	d.body = A, d.name = "vacuum";
	EmitterId vac = w.Add (d, MakeToneSource (f.e, 440.0, 0.5));
	w.Update (sc.At (24.02, V3 ()), sc);
	CHECK (w.Get (vac)->Begun ());
}
