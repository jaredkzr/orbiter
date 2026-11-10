// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// What a body shows the water (Src/Orbiter/HullWater.cpp): the volume of the
// union of its hulls (overlaps counted once), its submerged volume and
// centre of buoyancy, and the panels of its outside (faces inside another
// hull left out).

#include "HullWater.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>

using namespace hydro;
using Catch::Matchers::WithinRel;
using Catch::Matchers::WithinAbs;

namespace {
	const double PI = 3.14159265358979323846;
	convex::Polytope Box (const Vector &lo, const Vector &hi)
	{
		std::vector<Vector> p;
		for (int i = 0; i < 8; i++)
			p.push_back (Vector (i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z));
		convex::Polytope P;
		convex::Build (p, P);
		return P;
	}
	convex::Polytope Ball (const Vector &c, double r)
	{
		std::vector<Vector> p;
		const int n = 24;
		for (int i = 0; i <= n; i++)
			for (int j = 0; j < 2 * n; j++) {
				double th = PI * i / n, ph = PI * j / n;
				p.push_back (c + Vector (sin (th) * cos (ph), cos (th), sin (th) * sin (ph)) * r);
			}
		convex::Polytope P;
		convex::Build (p, P);
		return P;
	}
	// the panels' area projected onto a direction (what pressure along it acts on)
	double PanelArea (const Model &m, const Vector &dir)
	{
		double a = 0.0;
		for (const Panel &p : m.panels) a += p.area * std::max (0.0, dotp (p.n, dir));
		return a;
	}
}

TEST_CASE ("A box's volume, centroid and outside", "[Water.Volume]")
{
	Model m = Build ({ Box (Vector (-1, -0.5, -2), Vector (1, 0.5, 2)) }, nullptr);
	CHECK_THAT (m.volume, WithinRel (8.0, 0.03));
	CHECK (m.centroid.length () < 0.05);
	CHECK ((int)m.cells.size () <= MAX_CELLS);
	// (the panels are the grid's outside, its edges rounded by the smoothed normals)
	CHECK_THAT (m.area, WithinRel (28.0, 0.04));
	CHECK_THAT (PanelArea (m, Vector (0, -1, 0)), WithinRel (8.0, 0.05)); // (the bottom)
	CHECK_THAT (PanelArea (m, Vector (0, 0, 1)), WithinRel (2.0, 0.12));  // (the front: half a cell out on a small face)
	CHECK ((int)m.panels.size () <= MAX_PANELS);
}

TEST_CASE ("Overlapping hulls count once", "[Water.Volume]")
{
	Model m = Build ({ Box (Vector (0, 0, 0), Vector (2, 1, 4)), Box (Vector (1, 0, 0), Vector (3, 1, 4)) }, nullptr);
	CHECK_THAT (m.volume, WithinRel (12.0, 0.03));
	// (the outside of the union: the faces inside the other box are not the outside)
	CHECK_THAT (m.area, WithinRel (2.0 * (3.0 * 1.0 + 3.0 * 4.0 + 1.0 * 4.0), 0.04));
	CHECK_THAT (m.centroid.x, WithinAbs (1.5, 0.05));
}

TEST_CASE ("Submerged volume and centre of buoyancy", "[Water.Volume]")
{
	Model m = Build ({ Box (Vector (-1, -0.5, -2), Vector (1, 0.5, 2)) }, nullptr);
	Vector cb;
	// (waterline 0.3 m above the bottom)
	double v = Submerged (m, Vector (0, -0.2, 0), Vector (0, 1, 0), &cb);
	CHECK_THAT (v, WithinRel (2.0 * 0.3 * 4.0, 0.04));
	CHECK_THAT (cb.y, WithinAbs (-0.35, 0.02));
	// (all in, none)
	CHECK_THAT (Submerged (m, Vector (0, 5, 0), Vector (0, 1, 0)), WithinRel (m.volume, 1e-9));
	CHECK (Submerged (m, Vector (0, -5, 0), Vector (0, 1, 0)) == 0.0);
	// (heeled 30 deg: half under water through the centre is half the volume, its centre off to the low side)
	Vector up (sin (PI / 6.0), cos (PI / 6.0), 0.0);
	v = Submerged (m, Vector (0, 0, 0), up, &cb);
	CHECK_THAT (v, WithinRel (4.0, 0.03));
	CHECK (cb.x < -0.1);
}

TEST_CASE ("A ball", "[Water.Volume]")
{
	Model m = Build ({ Ball (Vector (0, 0, 0), 1.5) }, nullptr);
	CHECK_THAT (m.volume, WithinRel (4.0 / 3.0 * PI * 1.5 * 1.5 * 1.5, 0.03));
	// (the grid's outside lies up to half a cell out: a little more area on a curved body)
	CHECK_THAT (m.area, WithinRel (4.0 * PI * 1.5 * 1.5, 0.12));
	// (normals point out)
	for (const Panel &p : m.panels) CHECK (dotp (p.n, p.c) > 0.0);
}

TEST_CASE ("A long body keeps to the cell budget", "[Water.Volume]")
{
	Model m = Build ({ Box (Vector (-50, -5, -5), Vector (50, 5, 5)) }, nullptr);
	CHECK ((int)m.cells.size () <= MAX_CELLS);
	CHECK_THAT (m.volume, WithinRel (10000.0, 0.06));
	CHECK ((int)m.panels.size () <= MAX_PANELS);
}

TEST_CASE ("Parts of cells and panels", "[Water.Volume]")
{
	auto partAt = [](const Vector &p) { return p.z > 0.0 ? 1 : 0; };
	Model m = Build ({ Box (Vector (-1, -0.5, -2), Vector (1, 0.5, 2)) }, partAt);
	double n[2] = { 0, 0 };
	for (const Cell &c : m.cells) n[c.part] += c.w;
	CHECK_THAT (n[0], WithinRel (n[1], 0.1)); // (the middle slab of cells goes to one side)
	for (const Panel &p : m.panels) CHECK (p.part == (p.c.z > 0.0 ? 1 : 0));
}

TEST_CASE ("A symmetric body cut up unevenly comes out symmetric", "[Water.Volume]")
{
	// a box 4 m wide about x = 0, its hulls cut off-centre (as a mesh's convex decomposition may)
	Model m = Build ({ Box (Vector (-2, -0.5, -3), Vector (0.7, 0.5, 3)), Box (Vector (0.7, -0.5, -3), Vector (2, 0.5, 3)),
		Box (Vector (-2, -0.6, -1), Vector (-0.5, 0.0, 1)) }, nullptr); // (and a bulge on the left only, inside mostly)
	double l = 0.0, r = 0.0;
	for (const Panel &p : m.panels) {
		double a = p.area * std::max (0.0, -p.n.y);
		if (fabs (p.c.x) < 1e-6) l += 0.5 * a, r += 0.5 * a; // (on the plane)
		else if (p.c.x < 0.0) l += a; else r += a;
	}
	CHECK (m.symmetric);
	CHECK_THAT (l, WithinRel (r, 0.02));
	CHECK_THAT (m.centroid.x, WithinAbs (0.0, 1e-9));
}

TEST_CASE ("A hollow hull of thin walls is sealed", "[Water.Volume]")
{
	// six plates 5 cm thick closing a box 2 x 1 x 4: the inside counts as displacement
	double t = 0.05;
	Model m = Build ({ Box (Vector (-1, -0.5, -2), Vector (1, -0.5 + t, 2)), Box (Vector (-1, 0.5 - t, -2), Vector (1, 0.5, 2)),
		Box (Vector (-1, -0.5, -2), Vector (-1 + t, 0.5, 2)), Box (Vector (1 - t, -0.5, -2), Vector (1, 0.5, 2)),
		Box (Vector (-1, -0.5, -2), Vector (1, 0.5, -2 + t)), Box (Vector (-1, -0.5, 2 - t), Vector (1, 0.5, 2)) }, nullptr);
	CHECK_THAT (m.volume, WithinRel (8.0, 0.08)); // (the walls' cells count whole)
	// (its outside only: no panels on the inner faces of the walls)
	CHECK_THAT (m.area, WithinRel (28.0, 0.06));
}
