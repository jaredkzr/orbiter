// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Special relativity of relativistic flight (Src/Orbiter/Relativity.cpp):
// composition and aberration against boosted worldlines and light rays,
// hyperbolic motion (closed form, integrated, driven by a force),
// textbook trips, trips to moving targets checked by flying them, the
// relativistic rocket.

#include "Relativity.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <algorithm>
#include <cmath>
#include <random>

using namespace relativity;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {
	const double YEAR = 365.25 * 86400.0;
	const double AU = 1.495978707e11;

	Vector RandomUnit (std::mt19937 &rng)
	{
		std::normal_distribution<double> N;
		return Vector (N (rng), N (rng), N (rng)).unit ();
	}
}

TEST_CASE ("Speeds and velocity composition", "[speeds]")
{
	CHECK_THAT (Gamma (0.6 * C), WithinRel (1.25, 1e-14));
	CHECK_THAT (GammaMinus1OfU (Vector (1.0, 0, 0)), WithinRel (0.5 / (C * C), 1e-9));
	Vector a = AddVelocity (Vector (0.5 * C, 0, 0), Vector (0.5 * C, 0, 0));
	CHECK_THAT (a.x, WithinRel (0.8 * C, 1e-14));
	Vector b = AddVelocity (Vector (0.6 * C, 0, 0), Vector (0, 0.6 * C, 0));
	CHECK_THAT (b.x, WithinRel (0.6 * C, 1e-14));
	CHECK_THAT (b.y, WithinRel (0.48 * C, 1e-14));
	CHECK_THAT (Rapidity (SpeedOfRapidity (3.7)), WithinRel (3.7, 1e-12));

	// composition = the boosted worldline of an object moving at w in S'
	std::mt19937 rng (7);
	std::uniform_real_distribution<double> U (0.0, 0.999);
	for (int i = 0; i < 200; i++) {
		Vector v = RandomUnit (rng) * (U (rng) * C), w = RandomUnit (rng) * (U (rng) * C);
		double t = 1.0;
		Vector x = w; // (the event at t' = 1 s on the object's worldline in S')
		Boost (-v, t, x);
		Vector ref = x / t, add = AddVelocity (v, w);
		CHECK ((add - ref).length () < 1e-6);
		CHECK (add.length () < C);
		Vector u = UOfVelocity (add);
		CHECK ((VelocityOfU (u) - add).length () < 1e-6);
	}
}

TEST_CASE ("Lorentz boosts", "[boost]")
{
	double t = 1.0;
	Vector x;
	Boost (Vector (0.6 * C, 0, 0), t, x);
	CHECK_THAT (t, WithinRel (1.25, 1e-14));
	CHECK_THAT (x.x, WithinRel (-0.75 * C, 1e-14));

	std::mt19937 rng (11);
	std::uniform_real_distribution<double> U (0.0, 0.999), T (-10.0, 10.0);
	for (int i = 0; i < 200; i++) {
		Vector v = RandomUnit (rng) * (U (rng) * C);
		double t0 = T (rng), t1 = t0;
		Vector x0 = RandomUnit (rng) * (T (rng) * C), x1 = x0;
		Boost (v, t1, x1);
		// the interval is invariant
		double s0 = C * C * t0 * t0 - x0.length2 (), s1 = C * C * t1 * t1 - x1.length2 ();
		CHECK (std::fabs (s1 - s0) < 1e-9 * (C * C * 100.0));
		// boosting back returns the event
		Boost (-v, t1, x1);
		CHECK_THAT (t1, WithinAbs (t0, 1e-9));
		CHECK ((x1 - x0).length () < 1e-9 * C);
	}
}

TEST_CASE ("Aberration and Doppler shift", "[light]")
{
	const double beta = 0.8;
	Vector v (beta * C, 0, 0);
	// a source abeam is seen ahead at cos theta' = beta
	Vector n = Aberrate (Vector (0, 1, 0), v);
	CHECK_THAT (n.x, WithinRel (beta, 1e-14));
	CHECK_THAT (n.length (), WithinRel (1.0, 1e-14));
	// head-on and receding
	CHECK_THAT (Doppler (Vector (1, 0, 0), v), WithinRel (std::sqrt ((1 + beta) / (1 - beta)), 1e-13));
	CHECK_THAT (Doppler (Vector (-1, 0, 0), v), WithinRel (std::sqrt ((1 - beta) / (1 + beta)), 1e-13));

	// against boosted light rays: a photon from direction n travels along
	// -n; the event at t = 1 s on its path, boosted, gives the direction
	// it is seen from and (time component) the Doppler factor
	std::mt19937 rng (13);
	std::uniform_real_distribution<double> U (0.0, 0.9999);
	for (int i = 0; i < 200; i++) {
		Vector vo = RandomUnit (rng) * (U (rng) * C), d = RandomUnit (rng);
		double t = 1.0;
		Vector x = -d * C;
		Boost (vo, t, x);
		Vector ref = -x / (C * t), ab = Aberrate (d, vo);
		CHECK ((ab - ref).length () < 1e-9);
		CHECK_THAT (Doppler (d, vo), WithinRel (t, 1e-9));
		// and back: S moves at -vo relative to the observer
		Vector back = Aberrate (ab, -vo);
		CHECK ((back - d).length () < 1e-9);
		CHECK_THAT (Doppler (d, vo) * Doppler (ab, -vo), WithinRel (1.0, 1e-9));
	}
}

TEST_CASE ("Light-time", "[light]")
{
	// straight: the light left where the body was dt ago, dt = |pe - obs| / c
	Vector p (3e8, 4e8, 0), v (0.3 * C, 0, 0), obs, pe;
	double dt = EmissionPos (p, v, nullptr, nullptr, obs, pe);
	CHECK ((pe - (p - v * dt)).length () < 1e-3);
	CHECK_THAT ((pe - obs).length (), WithinRel (C * dt, 1e-10));
	// in a circle about a parent (itself moving): the body's place dt ago on
	// its circle, against the motion
	const double R = 3e10, W = 0.07 * C / R;
	Vector pp (1e9, -2e9, 5e8), vp (2e4, 1e4, 0);
	for (double ang : { 0.0, 1.0, 2.5 }) {
		Vector q = pp + Vector (std::cos (ang), std::sin (ang), 0) * R, vq = vp + Vector (-std::sin (ang), std::cos (ang), 0) * (W * R);
		Vector o = pp + Vector (std::cos (ang + 0.3), std::sin (ang + 0.3), 0.2) * (25.0 * R);
		dt = EmissionPos (q, vq, &pp, &vp, o, pe);
		double a = ang - W * dt;
		Vector ref = pp - vp * dt + Vector (std::cos (a), std::sin (a), 0) * R;
		CHECK ((pe - ref).length () < 1e-6 * R);
		CHECK_THAT ((pe - o).length (), WithinRel (C * dt, 1e-9));
	}
}

TEST_CASE ("Hyperbolic motion", "[motion]")
{
	// a year at 1 g from rest: closed form, integrated thrust, and a
	// constant force d(gamma v)/dt (the same motion)
	double t, x, v;
	Hyperbolic (G0, YEAR, t, x, v);
	State s;
	Propagate (s, Vector (G0, 0, 0), YEAR);
	// (Runge-Kutta 4 in steps of rapidity 0.01: ~3e-11 after rapidity 1)
	CHECK_THAT (s.t, WithinRel (t, 1e-10));
	CHECK_THAT (s.x.x, WithinRel (x, 1e-10));
	CHECK_THAT (s.V ().x, WithinRel (v, 1e-10));
	CHECK_THAT (s.tau, WithinRel (YEAR, 1e-15));
	CHECK (std::fabs (s.x.y) + std::fabs (s.x.z) == 0.0);

	Field f = [](double, const Vector &, const Vector &) { return Vector (G0, 0, 0); };
	State sf;
	Propagate (sf, Vector (), YEAR, &f, 2000);
	CHECK_THAT (sf.t, WithinRel (t, 1e-10));
	CHECK_THAT (sf.x.x, WithinRel (x, 1e-10));

	// a sideways burn from speed: a pure turn-and-speed-up, no closed form;
	// converged (step halving changes little) and below c
	State a, b;
	a.u = b.u = UOfVelocity (Vector (0.9 * C, 0, 0));
	Propagate (a, Vector (0, 100.0 * G0, 0), 30.0 * 86400.0);
	Propagate (b, Vector (0, 100.0 * G0, 0), 30.0 * 86400.0, nullptr, 4000);
	CHECK ((a.x - b.x).length () < 1e-9 * a.x.length ());
	CHECK (a.V ().length () < C);
}

TEST_CASE ("Textbook trips at 1 g", "[trips]")
{
	// J. Baez et al., "The relativistic rocket" (Usenet Physics FAQ):
	// flip-and-burn at 1 g to Alpha Centauri (4.3 ly): 3.6 years on board,
	// 5.9 years on Earth; to the Galactic centre (30,000 ly): 20 years
	// on board; to Andromeda (2 million ly): 28 years on board (the FAQ's
	// table rounds to whole years: 28.2)
	double tau, t, vmax;
	FlipAndBurn (4.3 * LY, G0, tau, t, vmax);
	CHECK_THAT (tau / YEAR, WithinAbs (3.6, 0.05));
	CHECK_THAT (t / YEAR, WithinAbs (5.9, 0.05));
	FlipAndBurn (30000.0 * LY, G0, tau, t, vmax);
	CHECK_THAT (tau / YEAR, WithinAbs (20.0, 0.1));
	FlipAndBurn (2e6 * LY, G0, tau, t, vmax);
	CHECK_THAT (tau / YEAR, WithinAbs (28.0, 0.5));

	// interplanetary at 1 g is Newtonian: 2 sqrt (d/a)
	FlipAndBurn (0.52 * AU, G0, tau, t, vmax);
	CHECK_THAT (t, WithinRel (2.0 * std::sqrt (0.52 * AU / G0), 1e-5));
	// Earth to Mars at their closest at 1000 g: ~1.6 h, ~0.09 c
	FlipAndBurn (0.52 * AU, 1000.0 * G0, tau, t, vmax);
	CHECK_THAT (t / 3600.0, WithinAbs (1.57, 0.01));
	CHECK_THAT (vmax / C, WithinAbs (0.092, 0.001));

	// accelerate, coast, decelerate, arriving later: covers the distance
	double d = 4.3 * LY, tb;
	FlipAndBurn (d, G0, tau, t, vmax);
	CHECK (CoastRapidity (d, G0, 0.99 * t, tb) == 0.0);
	double eta = CoastRapidity (d, G0, 1.5 * t, tb);
	REQUIRE (eta > 0.0);
	double t1, x1, v1;
	Hyperbolic (G0, tb, t1, x1, v1);
	CHECK_THAT (2.0 * x1 + v1 * (1.5 * t - 2.0 * t1), WithinRel (d, 1e-9));
}

namespace {
	/// A target on a circular orbit about the origin (planet-like)
	TargetFunc Circular (double R, double period, double phase)
	{
		return [=](double t, Vector &x, Vector &v) {
			double w = 2.0 * M_PI / period, a = w * t + phase;
			x = Vector (R * std::cos (a), 0.0, R * std::sin (a));
			v = Vector (-R * w * std::sin (a), 0.0, R * w * std::cos (a));
		};
	}

	/// Fly the trip and compare with the target at arrival
	void CheckArrival (const TripSpec &spec, const Trip &trip, double tolX, double tolV)
	{
		State s = TripState (spec, trip, trip.tauArrive);
		Vector xt, vt;
		spec.target (trip.tArrive, xt, vt);
		CHECK_THAT (s.t, WithinRel (trip.tArrive, 1e-12));
		CHECK ((s.x - xt).length () < tolX);
		if (spec.mode == TRIP_RENDEZVOUS) CHECK ((s.V () - vt).length () < tolV);
		if (spec.tArrive > 0.0) CHECK_THAT (trip.tArrive, WithinRel (spec.tArrive, 1e-12));
	}
}

TEST_CASE ("Trip solver: a straight line is flip-and-burn", "[trips]")
{
	TripSpec spec;
	spec.accel = 1000.0 * G0;
	spec.target = [](double, Vector &x, Vector &v) { x = Vector (AU, 0, 0), v = Vector (); };
	Trip trip = SolveTrip (spec);
	REQUIRE (trip.ok);
	double tau, t, vmax;
	FlipAndBurn (AU, spec.accel, tau, t, vmax);
	CHECK_THAT (trip.tauArrive, WithinRel (tau, 1e-8));
	CHECK_THAT (trip.tArrive, WithinRel (t, 1e-8));
	CHECK_THAT (trip.vmax, WithinRel (vmax, 1e-6));
	CHECK_THAT (trip.burns[0].dir.x, WithinRel (1.0, 1e-9));
	CHECK_THAT (trip.burns[1].dir.x, WithinRel (-1.0, 1e-9));
	CHECK_THAT (trip.coastDt, WithinAbs (0.0, 1e-9));

	// and between the stars (rapidity ~ 1.8 per burn)
	spec.accel = G0;
	spec.target = [](double, Vector &x, Vector &v) { x = Vector (0, 0, 4.3 * LY), v = Vector (); };
	trip = SolveTrip (spec);
	REQUIRE (trip.ok);
	FlipAndBurn (4.3 * LY, G0, tau, t, vmax);
	CHECK_THAT (trip.tauArrive, WithinRel (tau, 1e-8));
	CHECK_THAT (trip.tArrive, WithinRel (t, 1e-8));
}

TEST_CASE ("Trip solver: Earth to a Mars-like target", "[trips]")
{
	// from a circular orbit at 1 AU to a target on one at 1.52 AU, 60 deg ahead
	const double day = 86400.0;
	TargetFunc earth = Circular (AU, 365.25 * day, 0.0);
	TripSpec base;
	earth (0.0, base.start.x, base.start.u);
	base.start.u = UOfVelocity (base.start.u);
	base.target = Circular (1.524 * AU, 687.0 * day, M_PI / 3.0);

	for (double g : { 10.0, 1000.0, 1e5 }) {
		CAPTURE (g);
		TripSpec spec = base;
		spec.accel = g * G0;
		// rendezvous as soon as possible
		Trip fast = SolveTrip (spec);
		REQUIRE (fast.ok);
		CheckArrival (spec, fast, 10.0, 1e-3);
		// arriving later, coasting between the burns: cheaper
		TripSpec late = spec;
		late.tArrive = 1.5 * fast.tArrive;
		Trip slow = SolveTrip (late);
		REQUIRE (slow.ok);
		CheckArrival (late, slow, 10.0, 1e-3);
		CHECK (slow.coastDt > 0.0);
		CHECK (slow.properDv < fast.properDv);
		// flyby as soon as possible, and later
		TripSpec fly = spec;
		fly.mode = TRIP_FLYBY;
		Trip f = SolveTrip (fly);
		REQUIRE (f.ok);
		CheckArrival (fly, f, 10.0, 0.0);
		CHECK (f.tArrive < fast.tArrive);
		fly.tArrive = 1.5 * f.tArrive;
		Trip f2 = SolveTrip (fly);
		REQUIRE (f2.ok);
		CheckArrival (fly, f2, 10.0, 0.0);
		// relativistic at 100,000 g
		if (g >= 1e5) CHECK (fast.vmax > 0.5 * C);
	}
}

TEST_CASE ("Trip solver: random problems", "[trips]")
{
	std::mt19937 rng (17);
	std::uniform_real_distribution<double> U (0.0, 1.0);
	int n = 0, ok = 0;
	for (int i = 0; i < 100; i++) {
		TripSpec spec;
		spec.start.x = RandomUnit (rng) * (3.0 * AU * U (rng));
		spec.start.u = UOfVelocity (RandomUnit (rng) * (50e3 * U (rng)));
		Vector xt0 = RandomUnit (rng) * (5.0 * AU * (0.05 + U (rng))), vt = RandomUnit (rng) * (50e3 * U (rng));
		spec.target = [=](double t, Vector &x, Vector &v) { x = xt0 + vt * t, v = vt; };
		spec.accel = G0 * std::pow (10.0, 1.0 + 4.0 * U (rng)); // 10 g .. 100,000 g
		spec.mode = i % 2 ? TRIP_FLYBY : TRIP_RENDEZVOUS;
		Trip fast = SolveTrip (spec);
		n++;
		if (fast.ok) {
			ok++;
			CheckArrival (spec, fast, 10.0, 1e-3);
			if (i % 4 < 2) {
				TripSpec late = spec;
				late.tArrive = (1.2 + U (rng)) * fast.tArrive;
				Trip slow = SolveTrip (late);
				CHECK (slow.ok);
				if (slow.ok) CheckArrival (late, slow, 10.0, 1e-3);
			}
		}
	}
	CHECK (ok == n);
}

TEST_CASE ("Relativistic rocket", "[rocket]")
{
	// photon rocket from rest to 0.6 c: mass ratio sqrt ((1 + b) / (1 - b)) = 2
	double dv = C * Rapidity (0.6 * C);
	RocketCost pr = Rocket (dv, C, 1000.0);
	CHECK_THAT (pr.massRatio, WithinRel (2.0, 1e-12));
	CHECK_THAT (pr.propellant, WithinRel (1000.0, 1e-12));
	CHECK_THAT (pr.energy, WithinRel (1000.0 * C * C, 1e-12));
	// exhaust at 0.1 c: ((1 + b) / (1 - b))^(c / 2w) = 4^5
	RocketCost r = Rocket (dv, 0.1 * C, 1000.0);
	CHECK_THAT (r.massRatio, WithinRel (1024.0, 1e-10));
	CHECK_THAT (r.energy, WithinRel (1023e3 * C * C * (1.0 - std::sqrt (0.99)), 1e-10));
	// a flip-and-burn to Alpha Centauri at 1 g by photon rocket: ~ e^3.68 = 40
	double tau, t, vmax;
	FlipAndBurn (4.3 * LY, G0, tau, t, vmax);
	CHECK_THAT (Rocket (G0 * tau, C, 1.0).massRatio, WithinRel (std::exp (G0 * tau / C), 1e-12));
	CHECK_THAT (Rocket (G0 * tau, C, 1.0).massRatio, WithinAbs (39.0, 2.0));
}

TEST_CASE ("Coordinate acceleration of a proper acceleration", "[gravity]")
{
	Vector v (0.8 * C, 0, 0);
	double g = Gamma (0.8 * C);
	// along the motion: alpha / gamma^3; across: alpha / gamma^2
	CHECK_THAT (CoordAccel (Vector (10.0, 0, 0), v).x, WithinRel (10.0 / (g * g * g), 1e-12));
	Vector t = CoordAccel (Vector (0, 10.0, 0), v);
	CHECK_THAT (t.y, WithinRel (10.0 / (g * g), 1e-12));
	CHECK_THAT (t.x, WithinAbs (0.0, 1e-12));
	// and back
	std::mt19937 rng (19);
	std::uniform_real_distribution<double> U (0.0, 0.999);
	for (int i = 0; i < 100; i++) {
		Vector vv = RandomUnit (rng) * (U (rng) * C), ac = RandomUnit (rng) * 100.0;
		CHECK ((CoordAccel (ProperOfCoordAccel (ac, vv), vv) - ac).length () < 1e-9);
	}
	// du/dt integrated in coordinate time: hyperbolic motion
	Vector x, u;
	const double a = 1000.0 * G0, T = 3.0 * 86400.0;
	int n = 30000;
	double h = T / n;
	auto f = [&](const Vector &uu) { return URate (uu, Vector (), Vector (a, 0, 0)); };
	for (int i = 0; i < n; i++) {
		Vector k1u = f (u), k1x = VelocityOfU (u);
		Vector k2u = f (u + k1u * (0.5 * h)), k2x = VelocityOfU (u + k1u * (0.5 * h));
		Vector k3u = f (u + k2u * (0.5 * h)), k3x = VelocityOfU (u + k2u * (0.5 * h));
		Vector k4u = f (u + k3u * h), k4x = VelocityOfU (u + k3u * h);
		u += (k1u + k2u * 2.0 + k3u * 2.0 + k4u) * (h / 6.0);
		x += (k1x + k2x * 2.0 + k3x * 2.0 + k4x) * (h / 6.0);
	}
	// x(t) = c^2/a (sqrt (1 + (a t/c)^2) - 1)
	CHECK_THAT (x.x, WithinRel (C * C / a * (std::sqrt (1.0 + std::pow (a * T / C, 2)) - 1.0), 1e-9));
}

namespace {
	/// Deflection of a particle passing the Sun at impact parameter b, speed
	/// v far away: integrated with the post-Newtonian acceleration
	double Deflection (double v, double b)
	{
		const double GM = 1.32712440018e20;
		Vector x (-1e4 * b, b, 0), u = UOfVelocity (Vector (v, 0, 0));
		Vector u0 = u;
		auto acc = [&](const Vector &xx, const Vector &uu) {
			double r = xx.length ();
			Vector g = xx * (-GM / (r * r * r));
			return URate (uu, PNAccel (g, -GM / r, VelocityOfU (uu)), Vector ());
		};
		while (x.x < 1e4 * b) {
			double h = 2e-3 * x.length () / v;
			Vector k1u = acc (x, u), k1x = VelocityOfU (u);
			Vector k2u = acc (x + k1x * (0.5 * h), u + k1u * (0.5 * h)), k2x = VelocityOfU (u + k1u * (0.5 * h));
			Vector k3u = acc (x + k2x * (0.5 * h), u + k2u * (0.5 * h)), k3x = VelocityOfU (u + k2u * (0.5 * h));
			Vector k4u = acc (x + k3x * h, u + k3u * h), k4x = VelocityOfU (u + k3u * h);
			u += (k1u + k2u * 2.0 + k3u * 2.0 + k4u) * (h / 6.0);
			x += (k1x + k2x * 2.0 + k3x * 2.0 + k4x) * (h / 6.0);
		}
		return std::acos (std::clamp (dotp (u.unit (), u0.unit ()), -1.0, 1.0));
	}
}

TEST_CASE ("Deflection by the Sun", "[gravity]")
{
	const double GM = 1.32712440018e20, R = 6.957e8;
	// light grazing the Sun: 4GM/(b c^2) = 1.75 arcsec (Eddington 1919);
	// the integration starts and ends 10^4 b out: the deflection beyond
	// (a fraction ~ b/L) is missing
	double light = Deflection (0.9999999 * C, R);
	CHECK_THAT (light / M_PI * 180.0 * 3600.0, WithinAbs (1.75, 0.01));
	CHECK_THAT (light, WithinRel (4.0 * GM / (R * C * C), 2e-3));
	// a particle at c/2: 2GM/(b v^2) (1 + v^2/c^2)
	double v = 0.5 * C;
	CHECK_THAT (Deflection (v, R), WithinRel (2.0 * GM / (R * v * v) * (1.0 + 0.25), 2e-3));
}

TEST_CASE ("Clock rates: GPS", "[clocks]")
{
	// a GPS satellite (a = 26561.75 km) against a clock on the geoid
	// (potential incl. rotation -6.96927e-10 c^2): the satellite's clock
	// runs fast by 4.4647e-10 (38.6 microseconds a day: N. Ashby, Living
	// Rev. Relativity 6 (2003) 1)
	const double GM = 3.986004418e14, a = 26561.75e3;
	double sat = ClockRateMinus1 (GM / a, -GM / a);
	double ground = ClockRateMinus1 (0.0, -6.96927e-10 * C * C);
	CHECK_THAT (sat - ground, WithinAbs (4.4647e-10, 2e-14));
	CHECK_THAT ((sat - ground) * 86400.0 * 1e6, WithinAbs (38.6, 0.05));
	// special relativity alone: 1/gamma
	CHECK_THAT (1.0 + ClockRateMinus1 (0.36 * C * C, 0.0), WithinRel (0.8, 1e-14));
}
