// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// The sea's swell (Src/Orbiter/SeaSwell.cpp): the Pierson-Moskowitz sea of
// the wind (significant height and peak period), the discretised variance
// as measured over the surface, determinism, continuity across the anchor
// cells' borders, slopes and the water's velocity consistent with the
// height (finite differences in space and time).

#include "SeaSwell.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>

using namespace sea;
using Catch::Matchers::WithinRel;
using Catch::Matchers::WithinAbs;

namespace {
	const double PI = 3.14159265358979323846;
	const double R = 6.371e6;
	Vector Unit (double latDeg, double lngDeg)
	{
		double la = latDeg * PI / 180.0, lo = lngDeg * PI / 180.0;
		return Vector (cos (la) * cos (lo), sin (la), cos (la) * sin (lo));
	}
	// a point d metres east and n metres north of (lat, lng)
	Vector Offset (double latDeg, double lngDeg, double e, double n)
	{
		return Unit (latDeg + n / R * 180.0 / PI, lngDeg + e / (R * cos (latDeg * PI / 180.0)) * 180.0 / PI);
	}
	Swell Make (double wind, uint32_t seed = 7)
	{
		SwellParams p;
		p.wind = wind, p.bearing = 0.6, p.g = 9.81, p.radius = R, p.seed = seed, p.lambdaMin = 4.0;
		Swell s;
		s.Setup (p);
		return s;
	}
}

TEST_CASE ("The swell's sea follows Pierson-Moskowitz", "[Water.Swell]")
{
	for (double U : { 5.0, 7.0, 12.0, 20.0 }) {
		Swell s = Make (U);
		REQUIRE_FALSE (s.Calm ());
		double U195 = 1.06 * U;
		CHECK_THAT (s.HsFull (), WithinRel (0.209 * U195 * U195 / 9.81, 0.02));
		CHECK_THAT (s.Tp (), WithinRel (2.0 * PI * U195 / (0.877 * 9.81), 0.01));
		// (the band down to 4 m holds nearly all of it)
		CHECK (s.Hs () <= s.HsFull ());
		CHECK (s.Hs () > 0.93 * s.HsFull ());
		CHECK (s.CrestMax () > s.Hs ());
	}
	CHECK (Make (0.5).Calm ());   // (glassy)
	CHECK (Make (1.5).Hs () < 0.02); // (nearly all waves shorter than 4 m)
}

TEST_CASE ("The surface's variance is the spectrum's", "[Water.Swell]")
{
	Swell s = Make (8.0);
	// many points over a few cells and times
	double sum = 0.0, sum2 = 0.0;
	int n = 0;
	for (int i = 0; i < 400; i++)
		for (int j = 0; j < 10; j++) {
			Vector u = Offset (20.0 + 0.013 * j, -40.0 + 0.0137 * i, 0.0, 0.0);
			double h = s.Height (u, 1000.0 + 37.0 * j);
			sum += h, sum2 += h * h, n++;
		}
	double mean = sum / n, sd = sqrt (sum2 / n - mean * mean);
	CHECK (fabs (mean) < 0.05 * s.Hs ());
	CHECK_THAT (4.0 * sd, WithinRel (s.Hs (), 0.08));
}

TEST_CASE ("The swell is deterministic and seeded", "[Water.Swell]")
{
	Swell a = Make (7.0, 11), b = Make (7.0, 11), c = Make (7.0, 12);
	Vector u = Unit (28.0, -70.0);
	SwellSample sa, sb, sc;
	a.Sample (u, 7.25e8, -1.0, sa);
	b.Sample (u, 7.25e8, -1.0, sb);
	c.Sample (u, 7.25e8, -1.0, sc);
	CHECK (sa.h == sb.h);
	CHECK (sa.vel.x == sb.vel.x);
	CHECK (sa.h != sc.h);
}

TEST_CASE ("No seam at the anchor cells' borders", "[Water.Swell]")
{
	Swell s = Make (10.0);
	// along a line of latitude through several cells' borders (and a row border), small steps:
	// the height changes no more than its slope allows
	double t = 5.0e7;
	for (double lat : { 0.0, 27.5, 28.0, 63.0 }) {
		double prev, maxJump = 0.0;
		const double step = 0.5; // [m]
		// fine steps across a border: lng -71.0 is a column border of the 1 deg rows at low latitude
		for (double lng0 : { -71.0 }) {
			prev = s.Height (Offset (lat, lng0, -600.0, 0.0), t);
			for (int i = 1; i <= 2400; i++) {
				double h = s.Height (Offset (lat, lng0, -600.0 + i * step, 0.0), t);
				maxJump = std::max (maxJump, fabs (h - prev));
				prev = h;
			}
		}
		// (slopes stay below ~0.5: 0.25 m per 0.5 m step is a jump)
		CHECK (maxJump < 0.25);
	}
}

TEST_CASE ("Slopes and velocities agree with the height", "[Water.Swell]")
{
	Swell s = Make (9.0);
	const double t = 3.3e6, d = 0.01, dt = 1e-3;
	for (int k = 0; k < 20; k++) {
		double lat = 10.0 + 3.1 * k, lng = -50.0 + 2.7 * k;
		SwellSample c;
		s.Sample (Offset (lat, lng, 0.0, 0.0), t, 0.0, c);
		double he = s.Height (Offset (lat, lng, d, 0.0), t) - s.Height (Offset (lat, lng, -d, 0.0), t);
		double hn = s.Height (Offset (lat, lng, 0.0, d), t) - s.Height (Offset (lat, lng, 0.0, -d), t);
		CHECK_THAT (c.sx, WithinAbs (he / (2.0 * d), 2e-3));
		CHECK_THAT (c.sz, WithinAbs (hn / (2.0 * d), 2e-3));
		// (the surface rises with the water's vertical velocity there)
		double ht = (s.Height (Offset (lat, lng, 0.0, 0.0), t + dt) - s.Height (Offset (lat, lng, 0.0, 0.0), t - dt)) / (2.0 * dt);
		CHECK_THAT (c.vel.y, WithinAbs (ht, 2e-3));
		// (deeper: slower)
		SwellSample deep;
		s.Sample (Offset (lat, lng, 0.0, 0.0), t, -30.0, deep);
		CHECK (deep.vel.length () < c.vel.length () + 1e-9);
	}
}
