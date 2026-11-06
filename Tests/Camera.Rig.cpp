// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// The external camera's geometry (Src/Orbiter/CamRig.cpp): rotations and
// angles, frames from up and forward, the pitch limit (the horizon never
// turns over), parallel transport of a frame (level, no turn about up),
// the Locked mode's trackball (a full loop without a jump), flyby
// placement.

#include "CamRig.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <algorithm>
#include <random>

using namespace camrig;
using Catch::Matchers::WithinAbs;

namespace {
	double Det (const Matrix &M)
	{
		return dotp (Col (M, 0), crossp (Col (M, 1), Col (M, 2)));
	}
	double Orthogonality (const Matrix &M) // largest deviation of M^T M from I
	{
		Matrix P (transp (M) * M);
		double e = 0.0;
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				e = std::max (e, fabs (P(i, j) - (i == j ? 1.0 : 0.0)));
		return e;
	}
	double MaxDiff (const Matrix &A, const Matrix &B)
	{
		double e = 0.0;
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				e = std::max (e, fabs (A(i, j) - B(i, j)));
		return e;
	}
	Vector RandomUnit (std::mt19937 &rng)
	{
		std::normal_distribution<double> n;
		Vector v (n (rng), n (rng), n (rng));
		return v.unit();
	}
}

TEST_CASE ("YawPitch matches the camera's convention and round-trips", "[rig]")
{
	// yaw 0, pitch 0: looking along +z
	Matrix R (YawPitch (0, 0));
	REQUIRE_THAT (MaxDiff (R, IMatrix()), WithinAbs (0.0, 1e-15));
	// positive pitch: looking up (camera below its target)
	REQUIRE (Col (YawPitch (0, 0.3), 2).y > 0.0);
	for (double yaw = -3.0; yaw < 3.0; yaw += 0.37)
		for (double pitch = -1.5; pitch <= 1.5; pitch += 0.29) {
			Matrix M (YawPitch (yaw, pitch));
			REQUIRE_THAT (Det (M), WithinAbs (1.0, 1e-12));
			REQUIRE_THAT (Orthogonality (M), WithinAbs (0.0, 1e-12));
			double y, p;
			YawPitchOf (Col (M, 2), y, p);
			REQUIRE_THAT (Wrap (y - yaw), WithinAbs (0.0, 1e-9));
			REQUIRE_THAT (p, WithinAbs (pitch, 1e-9));
			// the camera's up is never below the frame's horizon (|pitch| < 90)
			REQUIRE (Col (M, 1).y > 0.0);
		}
}

TEST_CASE ("The pitch limit stops short of the poles", "[rig]")
{
	double pitch = 0.0;
	for (int i = 0; i < 1000; i++) pitch = ClampPitch (pitch + 0.01); // a long drag up
	REQUIRE_THAT (pitch, WithinAbs (PITCH_LIMIT, 1e-12));
	// over the limit the view is still the right way up, and dragging
	// sideways still turns the same way
	Matrix M (YawPitch (0.2, pitch)), N (YawPitch (0.25, pitch));
	REQUIRE (Col (M, 1).y > 0.0);
	double y0, y1, p;
	YawPitchOf (Col (M, 2), y0, p);
	YawPitchOf (Col (N, 2), y1, p);
	REQUIRE (y1 > y0);
}

TEST_CASE ("Frames from up and forward", "[rig]")
{
	std::mt19937 rng (7);
	for (int i = 0; i < 200; i++) {
		Vector up (RandomUnit (rng)), fwd (RandomUnit (rng) * 3.0);
		Matrix F;
		if (!FrameUpFwd (up, fwd, F)) continue;
		REQUIRE_THAT (Det (F), WithinAbs (1.0, 1e-12));
		REQUIRE_THAT (Orthogonality (F), WithinAbs (0.0, 1e-12));
		REQUIRE_THAT ((Col (F, 1) - up).length(), WithinAbs (0.0, 1e-12));
		REQUIRE (dotp (Col (F, 2), fwd) > 0.0);
	}
	Matrix F;
	REQUIRE_FALSE (FrameUpFwd (Vector (0, 1, 0), Vector (0, 2, 0), F));
}

TEST_CASE ("Parallel transport keeps a frame level without turning it about up", "[rig]")
{
	// carried once around a great circle and back, a frame comes back as
	// it was (no drift about up), and its up is the given one throughout
	Matrix F0;
	REQUIRE (FrameUpFwd (Vector (0, 1, 0), Vector (0.3, 0, 1), F0));
	Matrix F (F0);
	const int N = 720;
	for (int i = 1; i <= N; i++) {
		double a = Pi2 * i / N;
		Vector up (sin (a), cos (a), 0.0); // over the poles of the x axis
		F = Transport (F, up);
		REQUIRE_THAT ((Col (F, 1) - up).length(), WithinAbs (0.0, 1e-9));
		REQUIRE_THAT (Orthogonality (F), WithinAbs (0.0, 1e-9));
	}
	REQUIRE_THAT (MaxDiff (F, F0), WithinAbs (0.0, 1e-6));

	// an up that turns through the frame's forward (straight over the top)
	F = F0;
	for (int i = 1; i <= 180; i++) {
		double a = Pi * i / 180;
		F = Transport (F, Vector (0, cos (a), sin (a)));
		REQUIRE_THAT (Det (F), WithinAbs (1.0, 1e-9));
	}
}

TEST_CASE ("The Locked trackball goes all the way round without a jump", "[rig]")
{
	// dragging up for a full turn: each step turns the camera by the
	// same small angle, and the camera's x axis (the drag's left-right)
	// never flips
	Matrix Q (YawPitch (0.4, 0.1));
	Vector x0 (Col (Q, 0));
	const double step = 0.01;
	int n = (int)(Pi2 / step);
	for (int i = 0; i < n; i++) {
		Matrix Qn (Q * RotX (step));
		double turn = acos (std::clamp (dotp (Col (Q, 2), Col (Qn, 2)), -1.0, 1.0));
		REQUIRE_THAT (turn, WithinAbs (step, 1e-9));
		REQUIRE (dotp (Col (Qn, 0), x0) > 0.999);
		Q = Qn;
	}
	// and sideways at any orientation: about the camera's own up
	Matrix Qs (Q * RotY (0.05));
	REQUIRE_THAT ((Col (Qs, 1) - Col (Q, 1)).length(), WithinAbs (0.0, 1e-12));
}

TEST_CASE ("Slerp eases one rotation towards another", "[rig]")
{
	Matrix A (YawPitch (0.1, 0.2)), B (YawPitch (1.4, -0.6));
	REQUIRE_THAT (MaxDiff (Slerp (A, B, 0.0), A), WithinAbs (0.0, 1e-12));
	REQUIRE_THAT (MaxDiff (Slerp (A, B, 1.0), B), WithinAbs (0.0, 1e-12));
	Matrix H (Slerp (A, B, 0.5));
	REQUIRE_THAT (Orthogonality (H), WithinAbs (0.0, 1e-9));
	double ta = acos (std::clamp ((H(0,0)*A(0,0) + H(1,0)*A(1,0) + H(2,0)*A(2,0) + H(0,1)*A(0,1) + H(1,1)*A(1,1) + H(2,1)*A(2,1) + H(0,2)*A(0,2) + H(1,2)*A(1,2) + H(2,2)*A(2,2) - 1.0) / 2.0, -1.0, 1.0));
	double tb = acos (std::clamp ((H(0,0)*B(0,0) + H(1,0)*B(1,0) + H(2,0)*B(2,0) + H(0,1)*B(0,1) + H(1,1)*B(1,1) + H(2,1)*B(2,1) + H(0,2)*B(0,2) + H(1,2)*B(1,2) + H(2,2)*B(2,2) - 1.0) / 2.0, -1.0, 1.0));
	REQUIRE_THAT (ta, WithinAbs (tb, 1e-6)); // half way
}

TEST_CASE ("LookLevel keeps the horizon level", "[rig]")
{
	Vector up (0, 1, 0);
	Matrix prev (IMatrix());
	std::mt19937 rng (3);
	for (int i = 0; i < 200; i++) {
		Vector d (RandomUnit (rng));
		Matrix R (LookLevel (d, up, prev));
		REQUIRE_THAT (Det (R), WithinAbs (1.0, 1e-9));
		REQUIRE_THAT ((Col (R, 2) - d).length(), WithinAbs (0.0, 1e-9));
		if (fabs (d.y) < 0.99) {
			REQUIRE_THAT (Col (R, 0).y, WithinAbs (0.0, 1e-9)); // no roll
			REQUIRE (Col (R, 1).y > 0.0);
		}
	}
	// straight down: still a rotation, looking down
	Matrix D (LookLevel (Vector (0, -1, 0), up, prev));
	REQUIRE_THAT (Det (D), WithinAbs (1.0, 1e-9));
}

TEST_CASE ("Auto switches with hysteresis", "[rig]")
{
	const double R = 6.371e6, atm = 2e5;
	REQUIRE_FALSE (AutoOrbital (false, 1e5, R, atm));
	REQUIRE (AutoOrbital (false, 3e5, R, atm));
	REQUIRE (AutoOrbital (true, 2e5, R, atm));   // (stays orbital just below the edge)
	REQUIRE_FALSE (AutoOrbital (false, 2e5, R, atm));
	// no atmosphere: a few per cent of the radius
	REQUIRE_FALSE (AutoOrbital (false, 2e4, 1.737e6, 0.0));
	REQUIRE (AutoOrbital (false, 1e5, 1.737e6, 0.0));
}

TEST_CASE ("Flyby places the camera ahead, to the side and above", "[rig]")
{
	Vector vdir (0, 0, 1), up (0, 1, 0);
	double ahead;
	Vector o (FlybyOffset (vdir, up, 100.0, 10.0, 1, ahead));
	REQUIRE_THAT (ahead, WithinAbs (300.0, 1e-9));
	REQUIRE_THAT (dotp (o, vdir), WithinAbs (ahead, 1e-9));
	REQUIRE (o.y > 0.0);
	REQUIRE (fabs (o.x) >= 20.0);
	Vector o2 (FlybyOffset (vdir, up, 100.0, 10.0, -1, ahead));
	REQUIRE (o.x * o2.x < 0.0); // the other side
	// slow or parked: still a few sizes away; fast: no further than 30 sizes
	// (farther, the vessel is a speck as it comes)
	FlybyOffset (vdir, up, 0.0, 10.0, 1, ahead);
	REQUIRE_THAT (ahead, WithinAbs (80.0, 1e-9));
	FlybyOffset (vdir, up, 7700.0, 10.0, 1, ahead);
	REQUIRE_THAT (ahead, WithinAbs (300.0, 1e-9));
	// travelling straight up: a side is still found
	Vector v (FlybyOffset (up, up, 100.0, 10.0, 1, ahead));
	REQUIRE (std::hypot (v.x, v.z) > 1.0);

	// passed: behind the target (relative to its travel) and as far as it was ahead
	REQUIRE_FALSE (FlybyPassed (Vector (20, 6, 300), vdir, 300.0));
	REQUIRE_FALSE (FlybyPassed (Vector (20, 6, -100), vdir, 300.0));
	REQUIRE (FlybyPassed (Vector (20, 6, -340), vdir, 300.0));
	REQUIRE (FlybyPassed (Vector (1000, 0, 0), vdir, 300.0)); // (turned away)
}
