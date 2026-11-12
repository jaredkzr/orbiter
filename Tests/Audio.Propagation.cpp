// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// How sound travels: the air's loudness, absorption and Doppler factor
// (Audio/Medium), and what a listener hears of a moving source: travel
// time, Doppler, the shock cone of a supersonic source (Audio/Track).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "Audio/Medium.h"
#include "Audio/Track.h"
#include <cmath>

using namespace audio;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {
	double c_dist (const Track::Image &i, const V3 &L) { return (L - i.pos).Len (); }
	double Db (double g) { return 20.0 * std::log10 (g); }

	/// Mars near the surface: CO2 at 210 K, 0.020 kg/m^3
	Air Mars () { return Air::From (0.020, 210.0, 610.0, 188.9, 1.29); }

	/// A track of a source in uniform motion from p0 with velocity v, from t0 to t1
	Track Line (const V3 &p0, const V3 &v, double t0, double t1, double dt = 0.02)
	{
		Track tr;
		for (double t = t0; t <= t1 + 1e-9; t += dt) tr.Add (t, p0 + v * (t - t0));
		return tr;
	}
}

// ---- the air ------------------------------------------------------------

TEST_CASE ("Speed of sound from the gas", "[audio][medium]")
{
	Air a = Air::From (1.225, 288.15, 101325.0, 287.05, 1.4);
	CHECK_THAT (a.c, WithinRel (340.3, 0.002));
	CHECK_THAT (Mars ().c, WithinRel (226.0, 0.02)); // (CO2 at 210 K: ~ 225-240 m/s)
}

TEST_CASE ("Loudness in thin air and vacuum", "[audio][medium]")
{
	Air sea = Air::SeaLevel ();
	CHECK_THAT (Coupling (sea), WithinRel (1.0, 1e-9));
	// one air at both ends: sqrt(rho c / rho0 c0); on Mars about -20 dB
	double mars = Coupling (Mars ()) * Coupling (Mars ());
	CHECK (Db (mars) < -18.0);
	CHECK (Db (mars) > -23.0);
	// vacuum at either end: nothing
	CHECK (Coupling (Air ()) == 0.0);
	// cinematic: vacuum carries sound (thin reference air), the real air stays
	Air e = Effective (Air (), CINEMATIC);
	CHECK (e.Present ());
	CHECK (Coupling (e) > 0.5);
	CHECK (Effective (sea, CINEMATIC).rho == sea.rho);
	CHECK (!Effective (Air (), PHYSICAL).Present ());
}

TEST_CASE ("Air absorption", "[audio][medium]")
{
	Air sea = Air::SeaLevel ();
	CHECK_THAT (AbsorptionDb (sea, 1.0, 4000.0), WithinRel (0.028, 1e-6));
	CHECK_THAT (AbsorptionDb (sea, 100.0, 8000.0), WithinRel (4.0 * 2.8, 1e-6));
	// 1 km: highs gone, a cutoff of about 1.3 kHz
	CHECK_THAT (AbsorptionCutoff (sea, 1000.0), WithinRel (1309.0, 0.01));
	// near: everything
	CHECK (AbsorptionCutoff (sea, 1.0) == 20000.0);
	// thin air absorbs more over the same distance
	CHECK (AbsorptionCutoff (Mars (), 100.0) < AbsorptionCutoff (sea, 100.0) / 5.0);
}

TEST_CASE ("Spreading", "[audio][medium]")
{
	CHECK (Spreading (0.5, 2.0) == 1.0);
	CHECK_THAT (Spreading (20.0, 2.0), WithinRel (0.1, 1e-12));
	CHECK_THAT (Db (Spreading (200.0, 100.0)), WithinAbs (-6.02, 0.01));
}

TEST_CASE ("Doppler factor", "[audio][medium]")
{
	double n[3] = {1, 0, 0}, zero[3] = {0, 0, 0};
	double vs[3] = {100, 0, 0};   // source towards the listener
	double vl[3] = {100, 0, 0};   // listener away from the source
	CHECK_THAT (DopplerFactor (n, vs, zero, 340.0), WithinRel (340.0 / 240.0, 1e-12));
	CHECK_THAT (DopplerFactor (n, zero, vl, 340.0), WithinRel (240.0 / 340.0, 1e-12));
	CHECK_THAT (DopplerFactor (n, vs, vl, 340.0), WithinRel (1.0, 1e-12)); // (moving together)
	double away[3] = {-100, 0, 0};
	CHECK_THAT (DopplerFactor (n, away, zero, 340.0), WithinRel (340.0 / 440.0, 1e-12));
}

// ---- what is heard --------------------------------------------------------

TEST_CASE ("Stationary source: heard after d/c", "[audio][track]")
{
	Track tr = Line (V3 (0, 0, 0), V3 (), 0.0, 10.0);
	Track::Image im[4];
	int n = tr.Heard (10.0, V3 (680.0, 0, 0), 340.0, im, 4);
	REQUIRE (n == 1);
	CHECK_THAT (im[0].tau, WithinAbs (8.0, 1e-9));
	// from before the track: held at its first point
	n = tr.Heard (10.0, V3 (6800.0, 0, 0), 340.0, im, 4);
	REQUIRE (n == 1);
	CHECK_THAT (im[0].tau, WithinAbs (-10.0, 1e-9));
	// restarted (it appeared there): nothing from before
	Track r;
	r.Restart ();
	for (double t = 0.0; t <= 10.0; t += 0.02) r.Add (t, V3 ());
	CHECK (r.Heard (10.0, V3 (6800.0, 0, 0), 340.0, im, 4) == 0);
	CHECK (r.Heard (10.0, V3 (680.0, 0, 0), 340.0, im, 4) == 1);
}

TEST_CASE ("Before the track: going on as it started", "[audio][track]")
{
	// a track starting at t = 10 with a source at 100 m/s: before, it was
	// on the same line
	Track tr;
	for (double t = 10.0; t <= 20.0; t += 0.02) tr.Add (t, V3 (100.0 * t, 0, 0));
	Track::Image im[4];
	// a listener far ahead hears the source from before the track
	const V3 L (10000.0, 500.0, 0);
	int n = tr.Heard (20.0, L, 340.0, im, 4);
	REQUIRE (n == 1);
	CHECK (im[0].tau < 10.0);
	CHECK_THAT (im[0].pos.x, WithinAbs (100.0 * im[0].tau, 1e-6));
	CHECK_THAT (c_dist (im[0], L), WithinAbs (340.0 * (20.0 - im[0].tau), 1e-6));
}

TEST_CASE ("Subsonic fly-by: one image, delay and Doppler", "[audio][track]")
{
	// a source at 170 m/s (Mach 0.5) along x, 100 m past a listener at the origin
	const double c = 340.0, v = 170.0;
	Track tr = Line (V3 (-3000, 100, 0), V3 (v, 0, 0), 0.0, 40.0);
	const V3 L (0, 0, 0);
	for (double t = 10.0; t < 35.0; t += 1.37) { // (from 10 s: sound that left after the track began)
		Track::Image im[4];
		int n = tr.Heard (t, L, c, im, 4);
		REQUIRE (n == 1);
		// it left at tau, from there; c (t - tau) is its distance then
		double d = (L - im[0].pos).Len ();
		CHECK_THAT (c * (t - im[0].tau), WithinAbs (d, 1e-6));
		CHECK_THAT (im[0].pos.x, WithinAbs (-3000.0 + v * im[0].tau, 1e-6));
		CHECK_THAT (im[0].vel.x, WithinAbs (v, 1e-9));
		// the Doppler factor against the classic formula, and against dtau/dt
		V3 nv = (L - im[0].pos) * (1.0 / d);
		double nn[3] = {nv.x, nv.y, nv.z}, vs[3] = {v, 0, 0}, vl[3] = {0, 0, 0};
		double f = DopplerFactor (nn, vs, vl, c);
		Track::Image im2[4];
		REQUIRE (tr.Heard (t + 1e-3, L, c, im2, 4) == 1);
		CHECK_THAT ((im2[0].tau - im[0].tau) / 1e-3, WithinRel (f, 1e-3));
	}
	// approaching: higher (up to 2x at Mach 0.5), receding: lower (down to 2/3)
	Track::Image a[1], r[1];
	tr.Heard (10.0, L, c, a, 1);
	tr.Heard (35.0, L, c, r, 1);
	double na[3] = {1, 0, 0}, nr[3] = {-1, 0, 0}, vs[3] = {v, 0, 0}, z[3] = {0, 0, 0};
	CHECK (DopplerFactor (na, vs, z, c) > 1.9);
	CHECK (DopplerFactor (nr, vs, z, c) < 0.7);
}

TEST_CASE ("Supersonic: silent ahead of the cone, a pair behind, the boom", "[audio][track]")
{
	// Mach 2 along x, 1000 m above the listener; passes overhead at t = 20 s
	const double c = 340.0, v = 680.0;
	// (from t = -80 s: the path's start is pruned, no sound from before it)
	Track tr;
	for (double t = -80.0; t <= 60.0 + 1e-9; t += 0.02) {
		tr.Add (t, V3 (-20.0 * v + v * t, 0, 1000));
	}
	const V3 L (0, 0, 0);
	Track::Image im[4];

	// overhead and shortly after: the cone hasn't reached the listener
	CHECK (tr.Heard (20.0, L, c, im, 4) == 0);
	CHECK (tr.Heard (21.0, L, c, im, 4) == 0);

	// the cone (half angle asin(1/2) = 30 deg) reaches the ground
	// 1000 tan(60 deg) = 1732 m behind the source: at t = 20 + 1732/680 s
	double tBoom = 20.0 + 1000.0 * std::tan (60.0 * M_PI / 180.0) / v;
	CHECK (tr.Heard (tBoom - 0.01, L, c, im, 4) == 0);
	int n = tr.Heard (tBoom + 0.01, L, c, im, 4);
	REQUIRE (n == 2);
	// both images left near where the cone's line from the listener meets the path
	// (at x = 1000 / tan(60 deg) before the overhead point... the pair splits from one moment)
	CHECK (std::fabs (im[0].tau - im[1].tau) < 0.5);
	CHECK (im[0].tau > im[1].tau); // (newest first)

	// long after: two images, the older from the approach (heard backwards)
	n = tr.Heard (40.0, L, c, im, 4);
	REQUIRE (n == 2);
	CHECK (im[1].pos.x < 0.0);
	CHECK (im[0].pos.x > 0.0);
	for (int k = 0; k < 2; k++)
		CHECK_THAT (c * (40.0 - im[k].tau), WithinAbs ((L - im[k].pos).Len (), 1e-6));
}

TEST_CASE ("Moving listener: Doppler with the listener's speed", "[audio][track]")
{
	// a stationary source, a listener driving away at 34 m/s: heard 10 % lower
	Track tr = Line (V3 (0, 0, 0), V3 (), 0.0, 30.0);
	auto L = [](double t) { return V3 (100.0 + 34.0 * t, 0, 0); };
	Track::Image a[1], b[1];
	REQUIRE (tr.Heard (20.0, L (20.0), 340.0, a, 1) == 1);
	REQUIRE (tr.Heard (20.01, L (20.01), 340.0, b, 1) == 1);
	CHECK_THAT ((b[0].tau - a[0].tau) / 0.01, WithinRel (0.9, 1e-6));
}

TEST_CASE ("Track keeps a minute, thins close points", "[audio][track]")
{
	Track tr (60.0, 0.05);
	for (int i = 0; i <= 120 * 100; i++) tr.Add (i * 0.01, V3 (i * 0.01, 0, 0));
	// 60 s at 20 points per second (and one older than the minute)
	CHECK (tr.Size () <= 60 * 20 + 3);
	CHECK (tr.Size () >= 60 * 16);
	// time going back: a new path
	tr.Add (5.0, V3 (0, 0, 0));
	CHECK (tr.Size () == 1);
}
