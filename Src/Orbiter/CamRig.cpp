// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "CamRig.h"
#include <algorithm>
#include <cmath>

namespace camrig {

double Wrap (double a)
{
	a = fmod (a, Pi2);
	return a >= Pi ? a - Pi2 : a < -Pi ? a + Pi2 : a;
}

double ClampPitch (double pitch)
{
	return std::clamp (pitch, -PITCH_LIMIT, PITCH_LIMIT);
}

Matrix RotY (double a)
{
	double s = sin (a), c = cos (a);
	return Matrix (c, 0, -s,  0, 1, 0,  s, 0, c);
}

Matrix RotX (double a)
{
	double s = sin (a), c = cos (a);
	return Matrix (1, 0, 0,  0, c, s,  0, -s, c);
}

void YawPitchOf (const Vector &dir, double &yaw, double &pitch)
{
	pitch = asin (std::clamp (dir.y, -1.0, 1.0));
	yaw = (fabs (dir.x) + fabs (dir.z) > 1e-12 ? atan2 (-dir.x, dir.z) : 0.0);
}

Matrix AxisAngle (const Vector &ax, double angle)
{
	double s = sin (angle), c = cos (angle), t = 1.0 - c;
	return Matrix (
		c + ax.x*ax.x*t,      ax.x*ax.y*t - ax.z*s, ax.x*ax.z*t + ax.y*s,
		ax.y*ax.x*t + ax.z*s, c + ax.y*ax.y*t,      ax.y*ax.z*t - ax.x*s,
		ax.z*ax.x*t - ax.y*s, ax.z*ax.y*t + ax.x*s, c + ax.z*ax.z*t);
}

Matrix RotationBetween (const Vector &a, const Vector &b)
{
	Vector ax (crossp (a, b));
	double s = ax.length(), c = dotp (a, b);
	if (s > 1e-12) return AxisAngle (ax / s, atan2 (s, c));
	if (c > 0.0) return IMatrix ();
	// opposite: half a turn about any axis normal to a
	Vector n (fabs (a.x) < 0.9 ? Vector (1, 0, 0) : Vector (0, 1, 0));
	n = crossp (a, n).unit();
	return AxisAngle (n, Pi);
}

bool FrameUpFwd (const Vector &up, const Vector &fwd, Matrix &F, double minSin)
{
	Vector y (up.unit());
	Vector z (fwd - y * dotp (fwd, y));
	double l = z.length(), lf = fwd.length();
	if (lf <= 0.0 || l < minSin * lf) return false;
	z /= l;
	Vector x (crossp (y, z));
	F = Basis (x, y, z);
	return true;
}

Matrix Orthonormalise (const Matrix &F)
{
	Matrix R;
	Vector y (Col (F, 1));
	if (!FrameUpFwd (y, Col (F, 2), R)) {
		// (forward along up: forward from x)
		y = y.unit();
		Vector z (crossp (Col (F, 0), y).unit());
		R = Basis (crossp (y, z), y, z);
	}
	return R;
}

Matrix Transport (const Matrix &F, const Vector &up)
{
	return Orthonormalise (RotationBetween (Col (F, 1).unit(), up.unit()) * F);
}

Matrix LookLevel (const Vector &dir, const Vector &up, const Matrix &prev)
{
	Vector z (dir.unit());
	Vector x (crossp (up, z));
	double l = x.length();
	if (l < 1e-3) { // (looking straight up or down: the previous up, normal to dir)
		Vector y (Col (prev, 1));
		y = y - z * dotp (y, z);
		if (y.length() < 1e-6) y = Col (prev, 2) * (dotp (z, up) > 0 ? -1.0 : 1.0);
		y = y.unit();
		return Basis (crossp (y, z), y, z);
	}
	x /= l;
	return Basis (x, crossp (z, x), z);
}

Matrix Slerp (const Matrix &A, const Matrix &B, double k)
{
	if (k >= 1.0) return B;
	if (k <= 0.0) return A;
	Quaternion qa (A), qb (B), q;
	q.interp (qa, qb, k);
	Matrix R;
	R.Set (q);
	return R;
}

bool AutoOrbital (bool wasOrbital, double alt, double R, double atmTop)
{
	double h = std::max (atmTop, 0.03 * R);
	return wasOrbital ? alt > 0.9 * h : alt > 1.1 * h;
}

Vector FlybyOffset (const Vector &vdir, const Vector &up, double speed, double size, int side, double &ahead)
{
	// a few seconds ahead, near enough to fill much of the view as it passes
	const double LEAD = 3.0; // [s]
	ahead = std::clamp (speed * LEAD, 8.0 * size, 3000.0);
	double aside = std::max (2.0 * size, 0.06 * ahead);
	Vector s (crossp (up, vdir));
	if (s.length() < 1e-3) s = crossp (Vector (0, 1, 0), vdir);
	if (s.length() < 1e-3) s = crossp (Vector (1, 0, 0), vdir);
	s = s.unit() * (side < 0 ? -1.0 : 1.0);
	Vector u (up - vdir * dotp (up, vdir));
	u = (u.length() > 1e-3 ? u.unit() : Vector (0, 0, 0));
	return vdir * ahead + s * aside + u * (0.3 * aside);
}

bool FlybyPassed (const Vector &offset, const Vector &vdir, double ahead)
{
	double d = offset.length();
	if (d > 3.0 * ahead) return true;
	return dotp (offset, vdir) < 0.0 && d > 1.1 * ahead;
}

}
