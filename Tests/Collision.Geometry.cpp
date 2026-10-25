// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Unit tests of the convex geometry for collisions (Src/Orbiter/ConvexGeometry)

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "ConvexGeometry.h"
#include <cmath>
#include <random>

using Catch::Matchers::WithinAbs;

static convex::Polytope Box (double sx, double sy, double sz)
{
	std::vector<Vector> p;
	for (int k = 0; k < 8; k++)
		p.push_back (Vector ((k & 1) ? sx : -sx, (k & 2) ? sy : -sy, (k & 4) ? sz : -sz) * 0.5);
	convex::Polytope P;
	convex::Build (p, P);
	return P;
}

static convex::Placed Place (const convex::Polytope &P, const Vector &pos, const Matrix &R = Matrix(1,0,0, 0,1,0, 0,0,1), double skin = 0.0)
{
	return { &P, R, pos, skin };
}

static Matrix RotZ (double a) { return Matrix (cos(a), -sin(a), 0, sin(a), cos(a), 0, 0, 0, 1); }
static Matrix RotX (double a) { return Matrix (1, 0, 0, 0, cos(a), -sin(a), 0, sin(a), cos(a)); }

TEST_CASE("Hull of a box", "[collision]")
{
	convex::Polytope P = Box (1, 2, 3);
	REQUIRE(P.v.size() == 8);
	REQUIRE(P.f.size() == 6);
	for (const auto &f : P.f) {
		REQUIRE(f.idx.size() == 4);
		// (counter-clockwise around the outward normal)
		Vector a = P.v[f.idx[0]], b = P.v[f.idx[1]], c = P.v[f.idx[2]];
		REQUIRE(dotp (crossp (b - a, c - a), f.n) > 0.0);
		for (int i : f.idx) REQUIRE_THAT(dotp (f.n, P.v[i]) - f.d, WithinAbs(0.0, 1e-9));
	}
	REQUIRE_THAT(P.volume, WithinAbs(6.0, 1e-9));
}

TEST_CASE("Hull of points inside points", "[collision]")
{
	std::mt19937 rng (1);
	std::uniform_real_distribution<double> u (-1.0, 1.0);
	std::vector<Vector> p;
	for (int i = 0; i < 2000; i++) {
		Vector q (u(rng), u(rng), u(rng));
		p.push_back (q.unit() * (i % 4 ? 0.5 * q.length() : 1.0)); // (points on and inside a sphere)
	}
	convex::Polytope P;
	REQUIRE(convex::Build (p, P));
	// every point inside or on every face (large sets are reduced to their
	// extreme points: within 1% of the size)
	for (const Vector &q : p)
		for (const auto &f : P.f) REQUIRE(dotp (f.n, q) - f.d < 0.01);
	// a closed convex surface: V - E + F = 2, every face convex
	size_t E = 0;
	for (const auto &f : P.f) E += f.idx.size();
	REQUIRE(E % 2 == 0);
	REQUIRE((long)P.v.size() - (long)(E / 2) + (long)P.f.size() == 2);
	for (const auto &f : P.f)
		for (int i : f.idx) REQUIRE(dotp (f.n, P.v[i]) - f.d < 1e-9);
	REQUIRE(P.volume > 3.5);
	REQUIRE(P.volume < 4.0 * 3.14159265 / 3.0 + 1e-6);
}

TEST_CASE("Hull of a flat set has a little thickness", "[collision]")
{
	std::vector<Vector> p = { {0,0,0}, {2,0,0}, {2,0,1}, {0,0,1} };
	convex::Polytope P;
	REQUIRE(convex::Build (p, P));
	REQUIRE(P.f.size() == 6);
	REQUIRE(P.volume > 0.0);
	REQUIRE(P.volume < 2.0 * 0.0011);
}

TEST_CASE("Distance between separated boxes", "[collision]")
{
	convex::Polytope P = Box (1, 1, 1);
	Vector pa, pb, n;
	double d = convex::Distance (Place (P, {3,0,0}), Place (P, {0,0,0}), pa, pb, n);
	REQUIRE_THAT(d, WithinAbs(2.0, 1e-9));
	REQUIRE_THAT(n.x, WithinAbs(1.0, 1e-9));
	d = convex::Distance (Place (P, {3,0,0}, RotZ(0), 0.1), Place (P, {0,0,0}, RotZ(0), 0.2), pa, pb, n);
	REQUIRE_THAT(d, WithinAbs(1.7, 1e-9));
	// corner towards a face: a box turned by 45 degrees
	d = convex::Distance (Place (P, {0,2,0}, RotZ(M_PI/4)), Place (P, {0,0,0}), pa, pb, n);
	REQUIRE_THAT(d, WithinAbs(2.0 - 0.5 - std::sqrt(0.5), 1e-9));
}

TEST_CASE("Penetration of overlapping boxes", "[collision]")
{
	convex::Polytope P = Box (1, 1, 1);
	Vector pa, pb, n;
	double d = convex::Distance (Place (P, {0.8,0,0}), Place (P, {0,0,0}), pa, pb, n);
	REQUIRE_THAT(d, WithinAbs(-0.2, 1e-6));
	REQUIRE_THAT(n.x, WithinAbs(1.0, 1e-6)); // (from B towards A)
	d = convex::Distance (Place (P, {0,0,-0.9}), Place (P, {0,0,0}), pa, pb, n);
	REQUIRE_THAT(d, WithinAbs(-0.1, 1e-6));
	REQUIRE_THAT(n.z, WithinAbs(-1.0, 1e-6));
}

TEST_CASE("Crossed edges", "[collision]")
{
	// two long thin boxes (wings), one along x and one along z, edge on:
	// turned by 45 degrees about their long axes, so their edges cross
	convex::Polytope wing = Box (10, 0.2, 0.2);
	convex::Polytope wing2 = Box (0.2, 0.2, 10);
	double half = 0.1 * std::sqrt (2.0); // (an edge's distance from the axis)
	Vector pa, pb, n;
	// 5 cm apart
	double d = convex::Distance (Place (wing, {0, 2*half + 0.05, 0}, RotX(M_PI/4)), Place (wing2, {0,0,0}, RotZ(M_PI/4)), pa, pb, n);
	REQUIRE_THAT(d, WithinAbs(0.05, 1e-6));
	// 5 cm into each other: detected (a test of the vertices alone would miss it)
	convex::Manifold m;
	REQUIRE(convex::Collide (Place (wing, {0, 2*half - 0.05, 0}, RotX(M_PI/4)), Place (wing2, {0,0,0}, RotZ(M_PI/4)), 0.0, m));
	REQUIRE(m.npt >= 1);
	REQUIRE_THAT(m.sep[0], WithinAbs(-0.05, 1e-6));
	REQUIRE_THAT(m.n.y, WithinAbs(1.0, 1e-6));
}

TEST_CASE("Box resting on a box: four points", "[collision]")
{
	convex::Polytope P = Box (1, 1, 1);
	convex::Manifold m;
	REQUIRE(convex::Collide (Place (P, {0.2, 0.99, 0.1}), Place (P, {0,0,0}), 0.01, m));
	REQUIRE(m.npt == 4);
	REQUIRE_THAT(m.n.y, WithinAbs(1.0, 1e-9));
	for (int i = 0; i < 4; i++) {
		REQUIRE_THAT(m.sep[i], WithinAbs(-0.01, 1e-9));
		REQUIRE_THAT(m.pt[i].y, WithinAbs(0.495, 1e-9));
	}
	// apart by more than the margin: no contact
	REQUIRE(!convex::Collide (Place (P, {0, 1.1, 0}), Place (P, {0,0,0}), 0.05, m));
	// within the margin: speculative contact
	REQUIRE(convex::Collide (Place (P, {0, 1.03, 0}), Place (P, {0,0,0}), 0.05, m));
	REQUIRE_THAT(m.sep[0], WithinAbs(0.03, 1e-9));
}

TEST_CASE("Distance agrees with sampling", "[collision]")
{
	std::mt19937 rng (7);
	std::uniform_real_distribution<double> u (-1.0, 1.0);
	for (int trial = 0; trial < 50; trial++) {
		std::vector<Vector> pa, pb;
		for (int i = 0; i < 30; i++) pa.push_back (Vector (u(rng), u(rng), u(rng)));
		for (int i = 0; i < 30; i++) pb.push_back (Vector (u(rng), u(rng), u(rng)) * 0.7);
		convex::Polytope A, B;
		convex::Build (pa, A); convex::Build (pb, B);
		Vector ofs (Vector (u(rng), u(rng), u(rng)).unit() * 3.0);
		Vector qa, qb, n;
		double d = convex::Distance (Place (A, ofs), Place (B, {0,0,0}), qa, qb, n);
		// (no pair of vertices is closer; the closest points are on the hulls)
		double vmin = 1e300;
		for (const Vector &a : A.v) for (const Vector &b : B.v) vmin = std::min (vmin, (a + ofs).dist (b));
		REQUIRE(d <= vmin + 1e-9);
		REQUIRE_THAT(qa.dist (qb), WithinAbs(d, 1e-6));
		for (const auto &f : A.f) REQUIRE(dotp (f.n, qa - ofs) - f.d < 1e-6);
		for (const auto &f : B.f) REQUIRE(dotp (f.n, qb) - f.d < 1e-6);
	}
}
