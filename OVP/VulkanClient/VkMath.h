// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Minimal double-precision affine transformations.
// Orbiter's frames are left-handed with column vectors (v' = R v).
// Matrices are stored column-major, as expected by GLSL.
// =======================================================================

#ifndef __VKMATH_H
#define __VKMATH_H

#include "OrbiterAPI.h"
#include <cmath>

struct Mat4 {
	double m[16]; // column-major: m[col*4 + row]

	static Mat4 Identity ()
	{
		Mat4 r{};
		r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0;
		return r;
	}

	bool IsIdentity () const
	{
		for (int i = 0; i < 16; i++)
			if (m[i] != (i % 5 == 0 ? 1.0 : 0.0)) return false;
		return true;
	}

	/// Rotation (3x3 rotation matrix) followed by translation
	static Mat4 FromRotTrans (const MATRIX3 &R, const VECTOR3 &t)
	{
		Mat4 r = Identity();
		r.m[0] = R.m11; r.m[4] = R.m12; r.m[8]  = R.m13;
		r.m[1] = R.m21; r.m[5] = R.m22; r.m[9]  = R.m23;
		r.m[2] = R.m31; r.m[6] = R.m32; r.m[10] = R.m33;
		r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
		return r;
	}

	static Mat4 Translation (const VECTOR3 &t)
	{
		Mat4 r = Identity();
		r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
		return r;
	}

	static Mat4 Scale (double sx, double sy, double sz)
	{
		Mat4 r = Identity();
		r.m[0] = sx; r.m[5] = sy; r.m[10] = sz;
		return r;
	}

	/// Rotation by 'angle' about the unit vector 'axis' through point 'ref'
	static Mat4 RotationAbout (const VECTOR3 &ref, const VECTOR3 &axis, double angle)
	{
		double c = cos(angle), s = sin(angle), t = 1.0 - c;
		double x = axis.x, y = axis.y, z = axis.z;
		MATRIX3 R = {
			t*x*x + c,   t*x*y - s*z, t*x*z + s*y,
			t*x*y + s*z, t*y*y + c,   t*y*z - s*x,
			t*x*z - s*y, t*y*z + s*x, t*z*z + c
		};
		// p' = R (p - ref) + ref
		VECTOR3 Rref = mul (R, ref);
		return FromRotTrans (R, ref - Rref);
	}

	Mat4 operator* (const Mat4 &b) const
	{
		Mat4 r;
		for (int c = 0; c < 4; c++)
			for (int rr = 0; rr < 4; rr++) {
				double s = 0.0;
				for (int k = 0; k < 4; k++) s += m[k*4 + rr] * b.m[c*4 + k];
				r.m[c*4 + rr] = s;
			}
		return r;
	}

	VECTOR3 TransformPoint (const VECTOR3 &p) const
	{
		return _V(m[0]*p.x + m[4]*p.y + m[8]*p.z + m[12],
		          m[1]*p.x + m[5]*p.y + m[9]*p.z + m[13],
		          m[2]*p.x + m[6]*p.y + m[10]*p.z + m[14]);
	}

	void ToFloat (float *out) const
	{
		for (int i = 0; i < 16; i++) out[i] = (float)m[i];
	}

	static Mat4 FromFloat (const float *in)
	{
		Mat4 r;
		for (int i = 0; i < 16; i++) r.m[i] = in[i];
		return r;
	}

	/// Inverse of a rigid transform (rotation, optionally uniformly scaled, and translation)
	Mat4 RigidInverse () const
	{
		double s2 = m[0]*m[0] + m[1]*m[1] + m[2]*m[2]; // squared scale
		Mat4 r = Identity ();
		for (int c = 0; c < 3; c++)
			for (int rr = 0; rr < 3; rr++) r.m[c*4 + rr] = m[rr*4 + c] / s2;
		for (int rr = 0; rr < 3; rr++)
			r.m[12 + rr] = -(r.m[rr] * m[12] + r.m[4 + rr] * m[13] + r.m[8 + rr] * m[14]);
		return r;
	}
};

#endif // !__VKMATH_H
