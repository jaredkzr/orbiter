// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// The water's forces and a body's motion in it (Src/Orbiter/Hydro.cpp):
// floating at the draft its weight sets, upright, settling; the same outcome
// for any step; a water entry stopped without the step's length mattering
// and without numerical blow-up; planing lift on a shallow bottom; a flat
// bottom slammed harder than a vee.

#include "Hydro.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>

using namespace hydro;
using Catch::Matchers::WithinRel;
using Catch::Matchers::WithinAbs;

namespace {
	const double PI = 3.14159265358979323846;
	const double G = 9.81, RHO = 1025.0;

	convex::Polytope Hull (const std::vector<Vector> &p)
	{
		convex::Polytope P;
		convex::Build (p, P);
		return P;
	}
	convex::Polytope Box (const Vector &lo, const Vector &hi)
	{
		std::vector<Vector> p;
		for (int i = 0; i < 8; i++) p.push_back (Vector (i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z));
		return Hull (p);
	}
	// a hull with a vee bottom (deadrise beta), 2 m wide, 4 m long, 1 m deep at the keel
	convex::Polytope Vee (double beta)
	{
		double rise = tan (beta) * 1.0;
		return Hull ({ Vector (0, 0, -2), Vector (0, 0, 2), Vector (-1, rise, -2), Vector (1, rise, -2), Vector (-1, rise, 2), Vector (1, rise, 2),
			Vector (-1, 1.0 + rise, -2), Vector (1, 1.0 + rise, -2), Vector (-1, 1.0 + rise, 2), Vector (1, 1.0 + rise, 2) });
	}
	Body MakeBody (const Model &m, double mass, double y)
	{
		Body b;
		b.mass = mass;
		// (a uniform box's moments per unit mass, from the model's extent: near enough)
		double L = m.length;
		b.pmi = Vector (L * L / 6.0, L * L / 6.0, L * L / 6.0);
		b.cg = m.centroid;
		b.R = Matrix (1, 0, 0, 0, 1, 0, 0, 0, 1);
		b.x = Vector (0, y, 0);
		return b;
	}
	Surface Calm () { Surface s; s.rho = RHO; s.g = G; return s; }

	// run for T seconds in steps of h; returns the lowest y of the cg's path and the largest water acceleration
	void Run (const Model &m, const Surface &s, Body &b, double h, double T, double *ymin = nullptr, double *amax = nullptr)
	{
		Result r;
		double lo = 1e9, am = 0.0;
		for (double t = 0.0; t < T - 1e-9; t += h) {
			Step (m, s, b, Vector (0, -G, 0), Vector (0, 0, 0), h, 0, r);
			Vector cg (b.x + mul (b.R, b.cg));
			lo = std::min (lo, cg.y);
			am = std::max (am, r.accMax);
			REQUIRE (std::isfinite (cg.y));
		}
		if (ymin) *ymin = lo;
		if (amax) *amax = am;
	}
}

TEST_CASE ("A box floats at the draft its weight sets, for any step", "[Water.Hydro]")
{
	Model m = Build ({ Box (Vector (-1, -0.5, -2), Vector (1, 0.5, 2)) }, nullptr);
	double draft = 0.3;
	double mass = RHO * m.volume / 1.0 * draft; // (the box is 1 m deep)
	for (double h : { 0.002, 0.02, 0.1 }) {
		Body b = MakeBody (m, mass, 0.5 - 0.6); // (its bottom 0.6 m under water: it rises)
		Run (m, Calm (), b, h, 60.0);
		double bottom = b.x.y - 0.5;
		CHECK_THAT (-bottom, WithinAbs (draft, 0.006));
		CHECK (b.v.length () < 0.01);
		CHECK (b.w.length () < 0.01);
		// (upright)
		CHECK (b.R.m22 > 0.999);
	}
}

TEST_CASE ("Heeled, a beamy box rights itself", "[Water.Hydro]")
{
	Model m = Build ({ Box (Vector (-2, -0.5, -4), Vector (2, 0.5, 4)) }, nullptr);
	double mass = RHO * 4.0 * 8.0 * 0.4;
	Body b = MakeBody (m, mass, 0.1);
	b.cg = Vector (0, -0.2, 0); // (low: stable)
	double a = 20.0 * PI / 180.0;
	b.R = Matrix (cos (a), -sin (a), 0, sin (a), cos (a), 0, 0, 0, 1);
	Run (m, Calm (), b, 0.02, 60.0);
	CHECK (b.R.m22 > 0.995); // (within ~6 deg of upright, settled)
	CHECK (b.w.length () < 0.02);
}

TEST_CASE ("A water entry is stopped alike for any step", "[Water.Hydro]")
{
	Model m = Build ({ Box (Vector (-1, -0.5, -2), Vector (1, 0.5, 2)) }, nullptr);
	double mass = RHO * m.volume * 0.4;
	double lows[3], acc[3];
	int k = 0;
	for (double h : { 0.002, 0.01, 0.05 }) {
		Body b = MakeBody (m, mass, 2.0);
		b.v = Vector (0, -9.0, 0);
		Run (m, Calm (), b, h, 20.0, &lows[k], &acc[k]);
		// (afloat in the end)
		CHECK (b.v.length () < 0.05);
		k++;
	}
	CHECK_THAT (lows[1], WithinAbs (lows[0], 0.1));
	CHECK_THAT (lows[2], WithinAbs (lows[0], 0.15));
	// (its centre floats at +0.1 m: it dips well below that, but not deep)
	CHECK (lows[0] < -0.1);
	CHECK (lows[0] > -3.0);
}

TEST_CASE ("A fast light body is stopped without blowing up", "[Water.Hydro]")
{
	Model m = Build ({ Box (Vector (-0.15, -0.15, -0.15), Vector (0.15, 0.15, 0.15)) }, nullptr);
	for (double h : { 0.02, 0.2 }) {
		Body b = MakeBody (m, 5.0, 1.0);
		b.v = Vector (20.0, -300.0, 0.0);
		b.w = Vector (3.0, 0.0, 1.0);
		Run (m, Calm (), b, h, 10.0);
		CHECK (std::isfinite (b.x.y));
		CHECK (b.v.length () < 1.0);
	}
}

TEST_CASE ("A shallow bottom planes", "[Water.Hydro]")
{
	// a flat plate-like hull skimming at 25 m/s, its nose up 4 deg, touching the water
	Model m = Build ({ Box (Vector (-1, 0, -3), Vector (1, 0.2, 3)) }, nullptr);
	double mass = 500.0;
	Body b = MakeBody (m, mass, 0.0);
	double a = 4.0 * PI / 180.0;
	b.R = Matrix (1, 0, 0, 0, cos (a), sin (a), 0, -sin (a), cos (a)); // (z forward, nose up)
	b.x = Vector (0, 0.05, 0);
	b.v = Vector (0, -0.5, 25.0);
	Result r;
	r.Clear (0);
	Forces (m, Calm (), b, r);
	// (lift above its weight; drag much less than the lift)
	CHECK (r.F.y > mass * G);
	CHECK (-r.F.z < 0.5 * r.F.y);
}

TEST_CASE ("A flat bottom slams harder than a vee", "[Water.Hydro]")
{
	double peak[2];
	int k = 0;
	for (double beta : { 2.0, 25.0 }) {
		Model m = Build ({ Vee (beta * PI / 180.0) }, nullptr);
		Body b = MakeBody (m, 3000.0, 0.6);
		b.cg = Vector (0, 0.8, 0);
		b.v = Vector (0, -8.0, 0);
		double amax;
		Run (m, Calm (), b, 0.01, 2.0, nullptr, &amax);
		peak[k++] = amax;
	}
	CHECK (peak[0] > 1.5 * peak[1]);
}

TEST_CASE ("Floating at rest: draft and trim", "[Water.Hydro]")
{
	Model m = Build ({ Box (Vector (-1, -0.5, -2), Vector (1, 0.5, 2)) }, nullptr);
	double y, th, ph;
	// (centred: level, 0.3 m draft -> origin 0.2 m above the water)
	REQUIRE (Equilibrium (m, RHO * 8.0 * 0.3, Vector (0, 0, 0), RHO, y, th, ph));
	CHECK_THAT (y, WithinAbs (0.2, 0.01));
	CHECK_THAT (th, WithinAbs (0.0, 1e-3));
	CHECK_THAT (ph, WithinAbs (0.0, 1e-3));
	// (centre of gravity forward: trims bow down; the centre of buoyancy comes under it)
	REQUIRE (Equilibrium (m, RHO * 8.0 * 0.3, Vector (0, -0.2, 0.3), RHO, y, th, ph));
	Matrix R (TrimMatrix (th, ph));
	Vector up (tmul (R, Vector (0, 1, 0)));
	Vector cb;
	Submerged (m, up * -y, up, &cb);
	Vector d (mul (R, cb - Vector (0, -0.2, 0.3)));
	CHECK (fabs (d.x) < 0.01);
	CHECK (fabs (d.z) < 0.01);
	CHECK (mul (R, Vector (0, 0, 1)).y < 0.0); // (bow down)
	// (too heavy: no)
	CHECK_FALSE (Equilibrium (m, RHO * 8.5, Vector (0, 0, 0), RHO, y, th, ph));
}
