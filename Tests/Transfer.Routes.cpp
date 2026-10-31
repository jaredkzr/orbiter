// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Gravity-assist routes (Src/Orbiter/Routes.cpp) against the GTOP benchmark
// "Cassini1" (ESA Advanced Concepts Team, Global Trajectory Optimisation
// Problems): Earth - Venus - Venus - Earth - Jupiter - Saturn, powered
// flybys, capture at Saturn; best known 4.9307 km/s (M. Stickel, 2006).
// The planets move on the benchmark's mean elements (the polynomials of
// its analytical ephemeris, values only), and periapses below the
// benchmark's limits cost its linear penalty.

#include "Routes.h"
#include "Conics.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>

using namespace transfer;
using namespace transfer::route;

namespace {
	const double MU_SUN = 1.32712428e20;
	const double AU = 1.4959787066e11;
	const double MJD2000 = 51544.0;      // MJD of 2000-01-01 0h

	// a [AU], e, i, node, argument of periapsis [deg] as polynomials in T
	// (centuries from 1900); mean anomaly M0 + (m0 + m1 T + m2 T^2) T [deg]
	struct Elems { double a, e[4], i[4], node[4], arg[4], M0, m[3]; };
	const Elems VENUS   = { 0.72333160, { 0.006820690, -0.000047740, 0.0000000910, 0 },
		{ 3.393630555555555560, 1.00583333333333333e-3, -9.72222222222222222e-7, 0 },
		{ 7.57796472222222222e+1, 8.9985e-1, 4.1e-4, 0 },
		{ 5.43841861111111111e+1, 5.08186111111111111e-1, -1.38638888888888889e-3, 0 },
		2.12603219444444444e2, { 5.8517803875e+4, 1.28605555555555556e-3, 0 } };
	const Elems EARTH   = { 1.000000230, { 0.016751040, -0.000041800, -0.0000001260, 0 },
		{ 0, 0, 0, 0 }, { 0, 0, 0, 0 },
		{ 1.01220833333333333e+2, 1.7191750, 4.52777777777777778e-4, 3.33333333333333333e-6 },
		3.58475844444444444e2, { 3.599904975e+4, -1.50277777777777778e-4, -3.33333333333333333e-6 } };
	const Elems JUPITER = { 5.2025610, { 0.048334750, 0.000164180, -0.00000046760, -0.00000000170 },
		{ 1.308736111111111110, -5.69611111111111111e-3, 3.88888888888888889e-6, 0 },
		{ 9.94433861111111111e+1, 1.010530, 3.52222222222222222e-4, -8.51111111111111111e-6 },
		{ 2.73277541666666667e+2, 5.99431666666666667e-1, 7.0405e-4, 5.07777777777777778e-6 },
		2.25328327777777778e2, { 3.03469202388888889e+3, -7.21588888888888889e-4, 1.78444444444444444e-6 } };
	const Elems SATURN  = { 9.5547470, { 0.055892320, -0.00034550, -0.0000007280, 0.000000000740 },
		{ 2.492519444444444440, -3.91888888888888889e-3, -1.54888888888888889e-5, 4.44444444444444444e-8 },
		{ 1.12790388888888889e+2, 8.73195138888888889e-1, -1.52180555555555556e-4, -5.30555555555555556e-6 },
		{ 3.38307772222222222e+2, 1.085220694444444440, 9.78541666666666667e-4, 9.91666666666666667e-6 },
		1.75466216666666667e2, { 1.22155146777777778e+3, -5.01819444444444444e-4, -5.19444444444444444e-6 } };

	double Poly (const double *c, double T) { return c[0] + T * (c[1] + T * (c[2] + T * c[3])); }

	bool State (const Elems &E, double mjd, Vector &r, Vector &v)
	{
		double T = (mjd - MJD2000 + 36525.0) / 36525.0;
		double a = E.a * AU, e = Poly (E.e, T), I = Poly (E.i, T) * _RAD_;
		double O = Poly (E.node, T) * _RAD_, w = Poly (E.arg, T) * _RAD_;
		double M = fmod ((E.M0 + (E.m[0] + T * (E.m[1] + T * E.m[2])) * T) * _RAD_, Pi2);
		double Ea = M;
		for (int it = 0; it < 50; it++) Ea -= (Ea - e * sin (Ea) - M) / (1.0 - e * cos (Ea));
		double n = sqrt (MU_SUN / (a*a*a)), Ed = n / (1.0 - e * cos (Ea)), q = sqrt (1.0 - e*e);
		double x = a * (cos (Ea) - e), y = a * q * sin (Ea);
		double vx = -a * sin (Ea) * Ed, vy = a * q * cos (Ea) * Ed;
		double cw = cos (w), sw = sin (w), cO = cos (O), sO = sin (O), cI = cos (I), sI = sin (I);
		double P[3] = { cw*cO - sw*sO*cI, cw*sO + sw*cO*cI, sw*sI };
		double Q[3] = { -sw*cO - cw*sO*cI, -sw*sO + cw*cO*cI, cw*sI };
		r = Vector (P[0]*x + Q[0]*y, P[1]*x + Q[1]*y, P[2]*x + Q[2]*y);
		v = Vector (P[0]*vx + Q[0]*vy, P[1]*vx + Q[1]*vy, P[2]*vx + Q[2]*vy);
		return true;
	}

	// rpSoft: the benchmark's penalty per metre below rpMin [m/s per m]
	Body Planet (const Elems &E, double mu, double rpMin, double rpSoft = 0.0)
	{
		Body b;
		b.mu = mu, b.rpMin = rpMin, b.rpSoft = rpSoft;
		b.state = [&E](double mjd, Vector &r, Vector &v) { return State (E, mjd, r, v); };
		return b;
	}

	Problem Cassini1 ()
	{
		Problem p;
		p.mu = MU_SUN;
		p.nref = Vector (0, 0, 1); // (ecliptic north: these elements are right-handed)
		p.dep = DEP_VINF;
		p.depBody = Planet (EARTH, 3.9860119e14, 0.0);
		p.enc = { Planet (VENUS, 3.24860e14, 6351.8e3, 0.01), Planet (VENUS, 3.24860e14, 6351.8e3, 0.01),
			Planet (EARTH, 3.9860119e14, 6778.1e3, 0.01), Planet (JUPITER, 1.267e17, 600000e3, 0.001),
			Planet (SATURN, 3.79e16, 0.0) };
		p.arr = ARR_CAPTURE;
		p.capRp = 108950e3;
		double e = 0.98;
		p.capRa = p.capRp * (1.0 + e) / (1.0 - e);
		p.t0lo = MJD2000 - 1000.0, p.t0hi = MJD2000;
		p.tlo = { 30, 100, 30, 400, 1000 };
		p.thi = { 400, 470, 400, 2000, 6000 };
		return p;
	}
}

TEST_CASE ("Cassini1: the best known route", "[route]")
{
	Problem p = Cassini1 ();
	double x[6] = { MJD2000 - 789.8117, 158.302027105278, 449.385873819743, 54.7489684339665, 1024.36205846918, 4552.30796805542 };
	Result r;
	double f = Evaluate (p, x, &r);
	printf ("Cassini1 at the best known solution: %.4f km/s (departure %.3f, flybys %.3f, arrival %.3f)\n",
		f * 1e-3, r.dvDep * 1e-3, r.dvFlyby * 1e-3, r.dvArr * 1e-3);
	for (int k = 0; k + 1 < (int)r.enc.size(); k++)
		printf ("  flyby %d: rp %.0f km (min %.0f), turn %.1f deg, dv %.1f + %.1f m/s\n", k + 1, r.enc[k].rp * 1e-3,
			p.enc[k].rpMin * 1e-3, r.enc[k].turn * _DEG_, r.enc[k].dvPeri, r.enc[k].dvTurn);
	REQUIRE (r.valid);
	CHECK_THAT (f, Catch::Matchers::WithinRel (4930.7, 0.001));
	CHECK (r.legs.size() == 5);
	// the legs end where the bodies are
	for (int k = 0; k < 5; k++) {
		Vector rb, vb;
		p.enc[k].state (r.legs[k].t1, rb, vb);
		CHECK ((r.legs[k].r1 - rb).length() < 1.0);
	}
}

TEST_CASE ("Cassini1: the optimiser finds the best known route", "[route]")
{
	Problem p = Cassini1 ();
	Options o;
	o.seed = 7; // (8 of seeds 1-10 reach 4.93-5.00 within this budget, two stay in the 5.30 basin)
	o.evals = 600000;
	auto t0 = std::chrono::steady_clock::now();
	Result r = Optimise (p, o);
	double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
	REQUIRE (r.valid);
	printf ("Cassini1 optimised: %.4f km/s in %.1f s: departure MJD2000 %.1f, legs", r.dv * 1e-3, sec, r.dep - MJD2000);
	for (auto &L : r.legs) printf (" %.1f", L.t1 - L.t0);
	printf (" days\n");
	CHECK (r.dv < 4930.7 * 1.03);
}

TEST_CASE ("MGA-1DSM: from an MGA route, and a little better after polishing", "[route]")
{
	Problem p = Cassini1 ();
	double x[6] = { MJD2000 - 789.8117, 158.302027105278, 449.385873819743, 54.7489684339665, 1024.36205846918, 4552.30796805542 };
	Result mga;
	Evaluate (p, x, &mga);
	REQUIRE (mga.valid);
	Problem q (p);
	q.dsm = true;
	q.dep = DEP_VINF;
	std::vector<double> xd;
	REQUIRE (ToDsm (q, mga, xd));
	REQUIRE ((int)xd.size() == Dimension (q));
	Result d;
	double f0 = Evaluate (q, xd.data(), &d);
	REQUIRE (d.valid);
	// the same departure: the first leg's manoeuvre vanishes
	CHECK (d.legs[0].dsmDv.length() < 1.0);
	double f1 = Polish (q, xd, 20000);
	printf ("1DSM from Cassini1's route: %.4f km/s, polished %.4f km/s (MGA %.4f)\n", f0 * 1e-3, f1 * 1e-3, mga.dv * 1e-3);
	CHECK (f1 <= f0);
	CHECK (f1 < 1.3 * mga.dv);
}

TEST_CASE ("Multi-revolution legs: an Earth-Earth resonant leg", "[route]")
{
	// Earth to Earth in two years: needs a full revolution
	Problem p;
	p.mu = MU_SUN;
	p.nref = Vector (0, 0, 1);
	p.dep = DEP_VINF;
	p.depBody = Planet (EARTH, 3.9860119e14, 0.0);
	p.enc = { Planet (EARTH, 3.9860119e14, 6778.1e3) };
	p.arr = ARR_VINF;
	p.t0lo = MJD2000, p.t0hi = MJD2000 + 365.0;
	p.tlo = { 700 }, p.thi = { 760 };
	double x[2] = { MJD2000 + 100.0, 740.0 };
	p.maxRevs = 0;
	double f0 = Evaluate (p, x);
	p.maxRevs = 1;
	Result r;
	double f1 = Evaluate (p, x, &r);
	REQUIRE (r.valid);
	CHECK (r.legs[0].revs == 1);
	// (two years for a little more than a revolution: about 5 km/s at each end)
	CHECK (f1 < 0.5 * f0);
	CHECK (f1 < 12000.0);
}
