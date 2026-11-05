// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The external camera's geometry, without the simulation (unit-tested in
// Tests/Camera.Rig.cpp).
//
// An external camera orbits its target in a reference frame chosen by the
// camera mode: a rotation matrix whose columns are the frame's x axis, its
// "up" (y) and its forward direction (z), in global coordinates. In the
// frame the camera is turned by yaw (about up) and pitch (about its own x
// axis): it looks along z' = (-sin yaw cos pitch, sin pitch,
// cos yaw cos pitch) from the target's far side, so a positive pitch puts
// the camera below the target, looking up (Orbiter's convention for the
// external camera's polar angle). The pitch is limited short of straight
// up and down, so the frame's up stays up on the screen: the horizon never
// turns over and dragging never reverses. The Locked mode instead keeps a
// rotation of its own (trackball: turned about the camera's own axes).
// =======================================================================

#ifndef __CAMRIG_H
#define __CAMRIG_H

#include "Vecmat.h"

namespace camrig {

	/// The pitch limit [rad] of the modes with a horizon (89 degrees)
	constexpr double PITCH_LIMIT = 1.5533430342749532;

	/// Angle wrapped to [-pi, pi)
	double Wrap (double a);
	/// Pitch limited to +-PITCH_LIMIT
	double ClampPitch (double pitch);

	/// The columns of a matrix
	inline Vector Col (const Matrix &M, int i)
	{ return i == 0 ? Vector (M.m11, M.m21, M.m31) : i == 1 ? Vector (M.m12, M.m22, M.m32) : Vector (M.m13, M.m23, M.m33); }
	/// A matrix from its columns
	inline Matrix Basis (const Vector &x, const Vector &y, const Vector &z)
	{ return Matrix (x.x, y.x, z.x,  x.y, y.y, z.y,  x.z, y.z, z.z); }

	/// Rotations about the y axis (yaw) and the x axis (pitch), as the
	/// camera's: YawPitch (yaw, pitch) = RotY (yaw) * RotX (pitch)
	Matrix RotY (double a);
	Matrix RotX (double a);
	inline Matrix YawPitch (double yaw, double pitch) { return RotY (yaw) * RotX (pitch); }
	/// Yaw and pitch of a viewing direction (unit vector, frame coordinates)
	void YawPitchOf (const Vector &dir, double &yaw, double &pitch);

	/// The rotation about an axis (unit vector) by an angle
	Matrix AxisAngle (const Vector &axis, double angle);
	/// The smallest rotation turning direction a into direction b (unit vectors)
	Matrix RotationBetween (const Vector &a, const Vector &b);

	/// A frame with its up along `up` and its forward direction along
	/// `fwd` projected normal to up (unit or not); false if fwd is
	/// (nearly) along up
	bool FrameUpFwd (const Vector &up, const Vector &fwd, Matrix &F, double minSin = 1e-3);
	/// Frame F turned by the smallest rotation that takes its up to `up`
	/// (parallel transport: the horizon stays level, no turn about up)
	Matrix Transport (const Matrix &F, const Vector &up);
	/// Frame F made orthonormal (its up kept, then forward, then x)
	Matrix Orthonormalise (const Matrix &F);
	/// A camera rotation looking along `dir` with its up as near to `up`
	/// as possible (horizon level); `prev`'s up is used where dir is
	/// (nearly) along up
	Matrix LookLevel (const Vector &dir, const Vector &up, const Matrix &prev);

	/// The share of the remaining way a value eases towards its target in
	/// dt [s], with time constant tau [s]
	inline double Follow (double dt, double tau) { return tau > 0.0 ? 1.0 - exp (-dt / tau) : 1.0; }
	/// Rotation A eased towards B by the share k (slerp)
	Matrix Slerp (const Matrix &A, const Matrix &B, double k);
	/// Smooth step 0..1 over 0 <= u <= 1
	inline double SmoothStep (double u) { u = u < 0.0 ? 0.0 : u > 1.0 ? 1.0 : u; return u * u * (3.0 - 2.0 * u); }

	/// Auto mode: true for the orbital frame, false for the free (surface)
	/// one, at altitude alt [m] over a body of radius R [m] with an
	/// atmosphere up to atmTop [m] (0: none); with hysteresis about the
	/// last choice
	bool AutoOrbital (bool wasOrbital, double alt, double R, double atmTop);

	/// Flyby: where the camera is put relative to its target, ahead along
	/// the direction of travel `vdir` (unit) and to one side (side = +-1),
	/// a little above (up: unit vector); speed [m/s]: the target's speed
	/// past the camera; size [m]: the target's radius. Returns the offset
	/// [m] (global orientation); ahead: the distance ahead along vdir.
	Vector FlybyOffset (const Vector &vdir, const Vector &up, double speed, double size, int side, double &ahead);
	/// Flyby: true when the camera should be put ahead again: the target
	/// has passed it (offset: camera relative to the target; vdir: the
	/// target's direction of travel past the camera) and is as far behind
	/// as it was ahead, or much further away than that
	bool FlybyPassed (const Vector &offset, const Vector &vdir, double ahead);
}

#endif // !__CAMRIG_H
