// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "FrameState.h"
#include <algorithm>
#include <cmath>

namespace vkframe {

Snapshot Sample (const VECTOR3 &cam, const MATRIX3 &camRot, uint32_t epoch)
{
	Snapshot s;
	s.valid = true;
	s.t = oapiGetSimTime ();
	s.cam = cam, s.camRot = camRot;
	s.epoch = epoch;
	oapiGetGlobalOrigin (&s.origin);
	if (OBJHANDLE hp = oapiCameraProxyGbody ()) {
		BodyPose &b = s.body;
		b.h = hp;
		oapiGetGlobalPos (hp, &b.pos);
		oapiGetGlobalVel (hp, &b.vel);
		oapiGetRotationMatrix (hp, &b.rot);
		b.radius = oapiGetSize (hp);
		double T = oapiGetPlanetPeriod (hp);
		// (as the particles' ground velocity: the body's y axis, PI2 / T)
		if (T != 0.0) b.omega = mul (b.rot, _V(0, 1, 0)) * (PI2 / T);
	}
	return s;
}

CarryFrame FrameOf (const Snapshot &a, const Snapshot &b)
{
	if (!a.valid || !b.valid || a.epoch != b.epoch || !a.body.h || a.body.h != b.body.h) return CarryFrame::Global;
	return length (b.cam - b.body.pos) < GROUND_RANGE * b.body.radius ? CarryFrame::Ground : CarryFrame::Body;
}

Carry CarryBetween (const Snapshot &a, const Snapshot &b)
{
	Carry c;
	c.Q = _M(1, 0, 0, 0, 1, 0, 0, 0, 1);
	if (!a.valid || !b.valid) return c;
	// (a's G positions in b's G: an origin shift between them taken out)
	VECTOR3 dOrg = b.origin - a.origin;
	c.shift = (a.cam - dOrg) - b.cam;
	c.continuous = a.epoch == b.epoch;
	c.T = c.shift, c.dist = length (c.T);
	CarryFrame f = FrameOf (a, b);
	if (f == CarryFrame::Global) return c;
	// (camera relative to the body's centre: independent of G's origin and
	// its motion - exact in double)
	VECTOR3 ra = a.cam - a.body.pos, rb = b.cam - b.body.pos;
	if (f == CarryFrame::Ground) {
		c.frame = CarryFrame::Ground;
		// Q = R_b R_a^T: a body-fixed point's turn from a to b
		const MATRIX3 &A = a.body.rot, &B = b.body.rot;
		const double *ad = A.data, *bd = B.data;
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++) {
				double s = 0.0;
				for (int k = 0; k < 3; k++) s += bd[3 * i + k] * ad[3 * j + k];
				c.Q.data[3 * i + j] = s;
			}
		// (its antisymmetric part: sin (angle) times the axis, for the
		// standard cross product - no handedness assumed)
		c.v = _V(c.Q.m32 - c.Q.m23, c.Q.m13 - c.Q.m31, c.Q.m21 - c.Q.m12) * 0.5;
	} else c.frame = CarryFrame::Body;
	c.T = mul (c.Q, ra) - rb;
	c.D0 = c.T - c.shift;
	c.dist = length (c.T);
	return c;
}

void FrameState::Advance (const VECTOR3 &cam, const MATRIX3 &camRot)
{
	prev = cur;
	cur = Sample (cam, camRot, epoch);
}

VECTOR3 FrameState::CarryPrevToNow (const VECTOR3 &g) const
{
	switch (FrameOf (prev, cur)) {
	case CarryFrame::Ground: return cur.body.pos + mul (cur.body.rot, tmul (prev.body.rot, g - prev.body.pos));
	case CarryFrame::Body: return cur.body.pos + (g - prev.body.pos);
	default: return PrevToNowG (g);
	}
}

VECTOR3 FrameState::FrameVel (const VECTOR3 &g) const
{
	switch (FrameOf (prev, cur)) {
	case CarryFrame::Ground: return GroundVel (g);
	case CarryFrame::Body: return cur.body.vel;
	default: return _V(0, 0, 0);
	}
}

VECTOR3 FrameState::GroundVel (const VECTOR3 &g) const
{
	if (!cur.body.h) return _V(0, 0, 0);
	return cur.body.vel + crossp (g - cur.body.pos, cur.body.omega);
}

} // namespace vkframe
