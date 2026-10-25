// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "ContactSolver.h"
#include <algorithm>
#include <cmath>

namespace contact {

	namespace {
		// (Orbiter's frames are left-handed: the velocity of a point r of a
		// body turning at w is crossp(r, w), and the moment of a force F
		// applied at r is crossp(F, r); rotations preserve both)

		Vector InvInertia (const Body &b, const Vector &L)
		{
			Vector l (tmul (b.R, L));
			return mul (b.R, Vector (l.x / (b.mass * b.pmi.x), l.y / (b.mass * b.pmi.y), l.z / (b.mass * b.pmi.z)));
		}

		// inverse effective mass of a body at r along d
		double InvMass (const Body &b, const Vector &r, const Vector &d)
		{
			return 1.0 / b.mass + dotp (d, crossp (r, InvInertia (b, crossp (d, r))));
		}

		Vector PointVel (const Body &b, const Vector &r)
		{
			return b.v + crossp (r, b.w);
		}

		struct Solver {
			std::vector<Body> &B;
			std::vector<Vector> *P, *L;

			double K (const Point &p, const Vector &d) const
			{
				double k = InvMass (B[p.a], p.ra, d);
				if (p.b >= 0) k += InvMass (B[p.b], p.rb, d);
				return k;
			}
			Vector RelVel (const Point &p) const
			{
				return PointVel (B[p.a], p.ra) - (p.b >= 0 ? PointVel (B[p.b], p.rb) : p.vs);
			}
			double RelPseudoVel (const Point &p) const
			{
				double u = dotp (B[p.a].vp + crossp (p.ra, B[p.a].wp), p.n);
				if (p.b >= 0) u -= dotp (B[p.b].vp + crossp (p.rb, B[p.b].wp), p.n);
				return u;
			}
			void ApplyPseudo (const Point &p, double l)
			{
				for (int k = 0; k < 2; k++) {
					int i = (k ? p.b : p.a);
					if (i < 0) continue;
					Vector J (p.n * (k ? -l : l)), r (k ? p.rb : p.ra);
					B[i].vp += J / B[i].mass;
					B[i].wp += InvInertia (B[i], crossp (J, r));
				}
			}
			void Apply (Body &b, int i, const Vector &r, const Vector &J)
			{
				Vector m (crossp (J, r));
				b.v += J / b.mass;
				b.w += InvInertia (b, m);
				if (P) (*P)[i] += J;
				if (L) (*L)[i] += m;
			}
			void Apply (const Point &p, const Vector &J)
			{
				Apply (B[p.a], p.a, p.ra, J);
				if (p.b >= 0) Apply (B[p.b], p.b, p.rb, -J);
			}
		};

		const int ITERATIONS = 24;
		const double MAX_PUSHOUT = 2.0;     // [m/s] largest speed at which a penetrating point is pushed out
		const double RIGID_BAUMGARTE = 0.2; // fraction of a rigid contact's penetration removed per step
		const double RIGID_SLOP = 0.005;    // [m] penetration of a rigid contact left alone (no jitter)
	}

	void Tangents (const Vector &n, const Vector &d, Vector &t1, Vector &t2)
	{
		t1 = d - n * dotp (d, n);
		if (t1.length() < 1e-6 * std::max (d.length(), 1e-30))
			t1 = crossp (n, fabs (n.x) < 0.9 ? Vector(1,0,0) : Vector(0,0,1));
		t1.unify();
		t2 = crossp (n, t1);
	}

	void Solve (std::vector<Point> &pts, std::vector<Body> &bodies, double h,
		std::vector<Vector> *P, std::vector<Vector> *L)
	{
		if (P) P->assign (bodies.size(), Vector(0,0,0));
		if (L) L->assign (bodies.size(), Vector(0,0,0));
		for (Body &b : bodies) b.vp.Set (0,0,0), b.wp.Set (0,0,0);
		if (pts.empty() || h <= 0.0) return;
		Solver S{bodies, P, L};

		for (Point &p : pts) {
			p.Kn = S.K (p, p.n);
			p.K1 = S.K (p, p.t1);
			p.K2 = S.K (p, p.t2);
			if (p.k > 0.0) {
				// spring-damper, implicit in the step (soft constraint): pushes
				// only if the point ends up below the surface (also when it is
				// above it at the start)
				double s = p.c + h * p.k;
				p.gamma = 1.0 / (h * s);
				p.bias = std::max (p.k * p.C / s, -MAX_PUSHOUT);
			} else if (p.C > 0.0) {
				// rigid, not touching yet: may approach the surface up to it
				// within the step
				p.gamma = 0.0;
				p.bias = p.C / h;
			} else {
				// rigid, penetrating: no approach; the penetration is removed by
				// moving the bodies apart (below), not by a velocity
				p.gamma = 0.0;
				p.bias = 0.0;
			}
			p.pbias = (p.k > 0.0 || p.C > 0.0) ? 0.0 :
				std::max (RIGID_BAUMGARTE * std::min (p.C + RIGID_SLOP, 0.0) / h, -MAX_PUSHOUT);
			p.lp = 0.0;
			// warm start (only points already in contact: a point that just
			// touched starts from zero)
			if (p.warm && p.C <= 0.0) {
				p.ln = std::max (p.warm[0], 0.0);
				p.l1 = std::clamp (p.warm[1], -p.mu1 * p.ln, p.mu1 * p.ln);
				p.l2 = std::clamp (p.warm[2], -p.mu2 * p.ln, p.mu2 * p.ln);
				S.Apply (p, p.n * p.ln + p.t1 * p.l1 + p.t2 * p.l2);
			} else {
				p.ln = p.l1 = p.l2 = 0.0;
			}
		}

		for (int it = 0; it < ITERATIONS; it++) {
			for (Point &p : pts) {
				// normal
				double u = dotp (S.RelVel (p), p.n);
				double dl = -(u + p.bias + p.gamma * p.ln) / (p.Kn + p.gamma);
				double ln = std::max (p.ln + dl, 0.0);
				dl = ln - p.ln;
				p.ln = ln;
				if (dl) S.Apply (p, p.n * dl);

				// friction (bounded by the normal impulse)
				Vector vt (S.RelVel (p));
				double u1 = dotp (vt, p.t1), u2 = dotp (vt, p.t2);
				double l1 = std::clamp (p.l1 - u1 / p.K1, -p.mu1 * p.ln, p.mu1 * p.ln);
				double l2 = std::clamp (p.l2 - u2 / p.K2, -p.mu2 * p.ln, p.mu2 * p.ln);
				Vector J (p.t1 * (l1 - p.l1) + p.t2 * (l2 - p.l2));
				p.l1 = l1, p.l2 = l2;
				if (J.x || J.y || J.z) S.Apply (p, J);
			}
		}

		// position correction of penetrating rigid contacts (pseudo-velocities)
		for (Body &b : bodies) b.vp.Set (0,0,0), b.wp.Set (0,0,0);
		for (int it = 0; it < ITERATIONS; it++)
			for (Point &p : pts) {
				if (!p.pbias) continue;
				double dl = -(S.RelPseudoVel (p) + p.pbias) / p.Kn;
				double lp = std::max (p.lp + dl, 0.0);
				S.ApplyPseudo (p, lp - p.lp);
				p.lp = lp;
			}

		for (Point &p : pts)
			if (p.warm) p.warm[0] = p.ln, p.warm[1] = p.l1, p.warm[2] = p.l2;
	}

} // namespace contact
