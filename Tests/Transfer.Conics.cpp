// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Conics of transfer planning (Src/Orbiter/Conics.cpp): Lambert's problem
// against a published solution and against Kepler propagation, the
// departure hyperbola from a point, the B vector.

#include "Conics.h"
#include "Maneuver.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <random>

using namespace transfer;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {
	const double MU_EARTH = 3.986004418e14;
	const double MU_SUN = 1.32712440018e20;
}

TEST_CASE ("Lambert: Vallado example 7-5", "[lambert]")
{
	// D. Vallado, Fundamentals of Astrodynamics and Applications, example 7-5
	Vector r1 (15945.34e3, 0, 0), r2 (12214.83899e3, 10249.46731e3, 0), v1, v2;
	REQUIRE (Lambert (r1, r2, 76.0 * 60.0, MU_EARTH, Vector (0, 0, 1), false, v1, v2));
	CHECK_THAT (v1.x, WithinAbs (2058.913, 0.5));
	CHECK_THAT (v1.y, WithinAbs (2915.965, 0.5));
	CHECK_THAT (v1.z, WithinAbs (0.0, 1e-6));
	CHECK_THAT (v2.x, WithinAbs (-3451.565, 0.5));
	CHECK_THAT (v2.y, WithinAbs (910.315, 0.5));
}

TEST_CASE ("Lambert: solutions reach the target (Kepler propagation)", "[lambert]")
{
	std::mt19937 rng (1234);
	std::uniform_real_distribution<double> U (-1.0, 1.0);
	int n = 0;
	for (int k = 0; k < 400; k++) {
		// heliocentric transfers 0.4 - 6 AU, flight times 20 days - 4 years, both senses
		double AU = 1.495978707e11;
		Vector r1 (U (rng), 0.15 * U (rng), U (rng)), r2 (U (rng), 0.15 * U (rng), U (rng));
		if (r1.length() < 0.1 || r2.length() < 0.1) continue;
		r1 = r1.unit() * (AU * (0.4 + 5.6 * 0.5 * (U (rng) + 1.0)));
		r2 = r2.unit() * (AU * (0.4 + 5.6 * 0.5 * (U (rng) + 1.0)));
		double tof = 86400.0 * (20.0 + 1440.0 * 0.5 * (U (rng) + 1.0));
		Vector nref (0, (k % 2) ? 1.0 : -1.0, 0);
		Vector v1, v2;
		if (!Lambert (r1, r2, tof, MU_SUN, nref, false, v1, v2)) continue;
		Vector r, v;
		REQUIRE (maneuver::Kepler (r1, v1, MU_SUN, tof, r, v));
		CHECK ((r - r2).length() < 1e-6 * r2.length());
		CHECK ((v - v2).length() < 1e-6 * v2.length());
		// the sense of motion follows nref
		CHECK (dotp (crossp (r1, v1), nref) > 0.0);
		n++;
	}
	CHECK (n > 350);
}

TEST_CASE ("Departure from a point onto a hyperbola", "[departure]")
{
	std::mt19937 rng (99);
	std::uniform_real_distribution<double> U (-1.0, 1.0);
	for (int k = 0; k < 200; k++) {
		Vector r (U (rng), U (rng), U (rng)), s (U (rng), U (rng), U (rng));
		if (r.length() < 0.1 || s.length() < 0.1) continue;
		r = r.unit() * 6.7e6;
		s = s.unit();
		if (dotp (r.unit(), s) < -0.9) continue; // (nearly behind: degenerate)
		double vinf = 500.0 + 4500.0 * 0.5 * (U (rng) + 1.0);
		Vector v (DepartureVelocity (r, s * vinf, MU_EARTH));
		// the energy of the hyperbola
		CHECK_THAT (0.5 * dotp (v, v) - MU_EARTH / r.length(), WithinRel (0.5 * vinf * vinf, 1e-9));
		// the outgoing asymptote: -1/e P + sqrt (1 - 1/e^2) Q
		double rl = r.length(), vv = dotp (v, v);
		Vector e ((r * (vv - MU_EARTH / rl) - v * dotp (r, v)) / MU_EARTH);
		double el = e.length();
		Vector P (e / el), Q (crossp (crossp (r, v), P).unit());
		Vector out (P * (-1.0 / el) + Q * sqrt (1.0 - 1.0 / (el * el)));
		CHECK ((out - s).length() < 1e-9);
		// and the velocity tends to it
		Vector rf, vf;
		REQUIRE (maneuver::Kepler (r, v, MU_EARTH, 3.0e8, rf, vf));
		CHECK (dotp (vf.unit(), s) > dotp (v.unit(), s) - 1e-12);
	}
}

TEST_CASE ("B vector of a hyperbola", "[bplane]")
{
	// a hyperbola from far away: B is the miss distance of the incoming asymptote
	Vector S (0.6, 0.0, 0.8), off (0.8, 0.0, -0.6); // (orthogonal unit vectors)
	double b = 9.0e6, vinf = 3000.0, mu = 4.2828e13;
	Vector r (S * -3.0e9 + off * b), v (S * vinf);
	// (the state at 3e9 m is nearly on the asymptote; correct its energy)
	double v2 = vinf * vinf + 2.0 * mu / r.length();
	v = S * sqrt (v2);
	Vector Sc;
	double vi;
	Vector B (BVector (r, v, mu, Sc, vi));
	CHECK_THAT (vi, WithinRel (vinf, 1e-6));
	CHECK ((Sc - S).length() < 1e-3);
	CHECK_THAT (B.length(), WithinRel (b, 2e-3));
	CHECK (dotp (B.unit(), off) > 0.999);
}

TEST_CASE ("Lambert: multi-revolution solutions reach the target", "[lambert]")
{
	std::mt19937 rng (4321);
	std::uniform_real_distribution<double> U (-1.0, 1.0);
	int found[4] = { 0, 0, 0, 0 };
	for (int k = 0; k < 300; k++) {
		double AU = 1.495978707e11;
		Vector r1 (U (rng), 0.1 * U (rng), U (rng)), r2 (U (rng), 0.1 * U (rng), U (rng));
		if (r1.length() < 0.1 || r2.length() < 0.1) continue;
		r1 = r1.unit() * (AU * (0.6 + 0.9 * 0.5 * (U (rng) + 1.0)));
		r2 = r2.unit() * (AU * (0.6 + 0.9 * 0.5 * (U (rng) + 1.0)));
		double tof = 86400.0 * (400.0 + 1500.0 * 0.5 * (U (rng) + 1.0));
		Vector nref (0, 1, 0);
		for (int N = 1; N <= 3; N++)
			for (int br = 0; br < 2; br++) {
				Vector v1, v2;
				if (!Lambert (r1, r2, tof, MU_SUN, nref, false, N, br, v1, v2)) continue;
				// elliptic, N full revolutions within the flight
				double a = 1.0 / (2.0 / r1.length() - dotp (v1, v1) / MU_SUN);
				REQUIRE (a > 0.0);
				double P = 2.0 * Pi * sqrt (a*a*a / MU_SUN);
				CHECK (tof > N * P);
				CHECK (tof < (N + 1) * P);
				Vector r, v;
				REQUIRE (maneuver::Kepler (r1, v1, MU_SUN, tof, r, v));
				CHECK ((r - r2).length() < 1e-6 * r2.length());
				CHECK ((v - v2).length() < 1e-6 * v2.length());
				CHECK (dotp (crossp (r1, v1), nref) > 0.0);
				found[N]++;
			}
	}
	CHECK (found[1] > 200);
	CHECK (found[2] > 50);
}

TEST_CASE ("Lambert: the two branches of a revolution differ, and vanish below the least time", "[lambert]")
{
	double AU = 1.495978707e11;
	Vector r1 (AU, 0, 0), r2 (0, 0, 1.2 * AU), nref (0, 1, 0);
	// with one revolution: about 1.3 periods of a 1.1 AU orbit
	Vector a1, a2, b1, b2;
	REQUIRE (Lambert (r1, r2, 1.6 * 365.25 * 86400.0, MU_SUN, nref, false, 1, 0, a1, a2));
	REQUIRE (Lambert (r1, r2, 1.6 * 365.25 * 86400.0, MU_SUN, nref, false, 1, 1, b1, b2));
	CHECK ((a1 - b1).length() > 100.0);
	// too short for a full revolution
	CHECK_FALSE (Lambert (r1, r2, 0.5 * 365.25 * 86400.0, MU_SUN, nref, false, 1, 0, a1, a2));
	CHECK_FALSE (Lambert (r1, r2, 0.5 * 365.25 * 86400.0, MU_SUN, nref, false, 1, 1, a1, a2));
}

TEST_CASE ("Flybys: turn angle, periapsis, powered flyby", "[flyby]")
{
	const double mu = 3.24859e14; // Venus
	double vinf = 5000.0, rp = 6.4e6;
	double turn = FlybyTurn (vinf, rp, mu);
	CHECK_THAT (FlybyPeriapsis (vinf, turn, mu), WithinRel (rp, 1e-9));
	// an unpowered flyby: equal speeds, the turn of that periapsis
	Vector vin (vinf, 0, 0), vout (vinf * cos (turn), 0, vinf * sin (turn));
	double r, dp, dt;
	double dv = PoweredFlyby (vin, vout, mu, 6.1e6, r, dp, dt);
	CHECK_THAT (r, WithinRel (rp, 1e-6));
	CHECK (dv < 1e-3);
	// a change of speed only: the burn at the periapsis
	Vector vout2 (vout * 1.1);
	dv = PoweredFlyby (vin, vout2, mu, 5.0e6, r, dp, dt);
	double vp1 = sqrt (vinf*vinf + 2.0 * mu / r), vp2 = sqrt (1.21 * vinf*vinf + 2.0 * mu / r);
	CHECK_THAT (dp, WithinRel (vp2 - vp1, 1e-6));
	CHECK (dt == 0.0);
	CHECK (dp < 0.1 * vinf); // (cheaper at the periapsis than far out)
	// more turn than the body gives above its least periapsis: the rest by a burn
	dv = PoweredFlyby (vin, Vector (-vinf, 0, 1.0), mu, 6.1e6, r, dp, dt);
	CHECK (r < 6.1e6);
	CHECK (dt > 0.0);
	// FlybyOut: the turn and speed of the hyperbola, about the incoming direction
	Vector vb (0, 0, 35000.0);
	for (double beta = -3.0; beta < 3.1; beta += 0.5) {
		Vector out (FlybyOut (vin, mu, rp, beta, vb));
		CHECK_THAT (out.length(), WithinRel (vinf, 1e-12));
		CHECK_THAT (acos (dotp (out.unit(), vin.unit())), WithinAbs (turn, 1e-9));
	}
}
