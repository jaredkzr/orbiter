// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "GroundContact.h"
#include <algorithm>
#include <cmath>

namespace contact {

	namespace {
		// (Orbiter's frames are left-handed: the velocity of a point r of a
		// body turning at w is crossp(r, w), and the moment of a force F
		// applied at r is crossp(F, r))

		Vector InvInertia (const Body &b, const Vector &L)
		{
			return Vector (L.x / (b.mass * b.pmi.x), L.y / (b.mass * b.pmi.y), L.z / (b.mass * b.pmi.z));
		}

		// inverse effective mass of the body at r along d
		double InvMass (const Body &b, const Vector &r, const Vector &d)
		{
			return 1.0 / b.mass + dotp (d, crossp (r, InvInertia (b, crossp (d, r))));
		}

		Vector PointVel (const Body &b, const Point &p)
		{
			return b.v + crossp (p.r, b.w) - p.vg;
		}

		void ApplyImpulse (Body &b, const Vector &r, const Vector &J, Vector &P, Vector &L)
		{
			Vector m (crossp (J, r));
			b.v += J / b.mass;
			b.w += InvInertia (b, m);
			P += J;
			L += m;
		}

		const int ITERATIONS = 24;
		const double MAX_PUSHOUT = 2.0;   // [m/s] largest speed at which a penetrating point is pushed out
		const double RIGID_BAUMGARTE = 0.2; // fraction of a rigid point's penetration removed per step
	}

	void Solve (std::vector<Point> &pts, Body &b, double h, Vector &P, Vector &L)
	{
		P.Set (0, 0, 0);
		L.Set (0, 0, 0);
		if (pts.empty() || h <= 0.0) return;

		for (Point &p : pts) {
			p.Kn = InvMass (b, p.r, p.n);
			p.K1 = InvMass (b, p.r, p.t1);
			p.K2 = InvMass (b, p.r, p.t2);
			if (p.k > 0.0) {
				// spring-damper, implicit in the step (soft constraint): pushes
				// only if the point ends up below the ground (also when it is
				// above it at the start)
				double s = p.c + h * p.k;
				p.gamma = 1.0 / (h * s);
				p.bias = std::max (p.k * p.C / s, -MAX_PUSHOUT);
			} else if (p.C > 0.0) {
				// rigid, not touching yet: may approach the ground up to it
				// within the step
				p.gamma = 0.0;
				p.bias = p.C / h;
			} else {
				p.gamma = 0.0;
				p.bias = std::max (RIGID_BAUMGARTE * p.C / h, -MAX_PUSHOUT);
			}
			// warm start (only points already compressed: a point that just
			// touched starts from zero)
			if (p.warm && p.C <= 0.0) {
				p.ln = std::max (p.warm[0], 0.0);
				p.l1 = std::clamp (p.warm[1], -p.mu1 * p.ln, p.mu1 * p.ln);
				p.l2 = std::clamp (p.warm[2], -p.mu2 * p.ln, p.mu2 * p.ln);
				ApplyImpulse (b, p.r, p.n * p.ln + p.t1 * p.l1 + p.t2 * p.l2, P, L);
			} else {
				p.ln = p.l1 = p.l2 = 0.0;
			}
		}

		for (int it = 0; it < ITERATIONS; it++) {
			for (Point &p : pts) {
				// normal
				double u = dotp (PointVel (b, p), p.n);
				double dl = -(u + p.bias + p.gamma * p.ln) / (p.Kn + p.gamma);
				double ln = std::max (p.ln + dl, 0.0);
				dl = ln - p.ln;
				p.ln = ln;
				if (dl) ApplyImpulse (b, p.r, p.n * dl, P, L);

				// friction (bounded by the normal impulse)
				Vector vt (PointVel (b, p));
				double u1 = dotp (vt, p.t1), u2 = dotp (vt, p.t2);
				double l1 = std::clamp (p.l1 - u1 / p.K1, -p.mu1 * p.ln, p.mu1 * p.ln);
				double l2 = std::clamp (p.l2 - u2 / p.K2, -p.mu2 * p.ln, p.mu2 * p.ln);
				Vector J (p.t1 * (l1 - p.l1) + p.t2 * (l2 - p.l2));
				p.l1 = l1, p.l2 = l2;
				ApplyImpulse (b, p.r, J, P, L);
			}
		}

		for (Point &p : pts)
			if (p.warm) p.warm[0] = p.ln, p.warm[1] = p.l1, p.warm[2] = p.l2;
	}

} // namespace contact
