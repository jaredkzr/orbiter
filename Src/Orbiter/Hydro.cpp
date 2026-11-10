// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Hydro.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace hydro {

int debugPanels = 0;

namespace {

	const double PI_ = 3.14159265358979323846;

	// skin friction coefficient (ITTC-57; laminar below Re 1e5)
	double Cf (double Re)
	{
		if (Re < 1e3) return 1.328 / sqrt (1e3);
		if (Re < 1e5) return 1.328 / sqrt (Re);
		double l = log10 (Re) - 2.0;
		return 0.075 / (l * l);
	}

	// rotation by the vector a (axis, angle |a|)
	Matrix Rot (const Vector &a)
	{
		double th = a.length();
		if (th < 1e-12) return Matrix (1, 0, 0, 0, 1, 0, 0, 0, 1);
		Vector k (a / th);
		double c = cos (th), s = sin (th), c1 = 1.0 - c;
		return Matrix (
			c + c1 * k.x * k.x,       c1 * k.x * k.y - s * k.z, c1 * k.x * k.z + s * k.y,
			c1 * k.x * k.y + s * k.z, c + c1 * k.y * k.y,       c1 * k.y * k.z - s * k.x,
			c1 * k.x * k.z - s * k.y, c1 * k.y * k.z + s * k.x, c + c1 * k.z * k.z);
	}

	// re-orthonormalise (drift over many sub-steps)
	void Orthonormalise (Matrix &R)
	{
		Vector x (R.m11, R.m21, R.m31), y (R.m12, R.m22, R.m32);
		x.unify();
		Vector z (crossp (x, y));
		z.unify();
		y = crossp (z, x);
		R = Matrix (x.x, y.x, z.x, x.y, y.y, z.y, x.z, y.z, z.z);
	}

}

void Result::Clear (int parts)
{
	*this = Result();
	if (parts > 0) partF.assign (parts, Vector (0, 0, 0));
}

double Surface::Height (double x, double z, double *sx, double *sz) const
{
	if (n < 2) {
		if (sx) *sx = 0.0;
		if (sz) *sz = 0.0;
		return 0.0;
	}
	double u = std::clamp ((x - x0) / ds, 0.0, n - 1.000001), v = std::clamp ((z - z0) / ds, 0.0, n - 1.000001);
	int i = (int)u, j = (int)v;
	double fu = u - i, fv = v - j;
	const double *r0 = &h[(size_t)j * n + i], *r1 = r0 + n;
	double a = r0[0] + (r0[1] - r0[0]) * fu, b = r1[0] + (r1[1] - r1[0]) * fu;
	if (sx) *sx = ((r0[1] - r0[0]) * (1.0 - fv) + (r1[1] - r1[0]) * fv) / ds;
	if (sz) *sz = (b - a) / ds;
	return a + (b - a) * fv;
}

Vector Surface::Vel (double x, double z, double d) const
{
	if (n < 2) return Vector (0, 0, 0);
	double u = std::clamp ((x - x0) / ds, 0.0, n - 1.000001), v = std::clamp ((z - z0) / ds, 0.0, n - 1.000001);
	int i = (int)u, j = (int)v;
	double fu = u - i, fv = v - j;
	const Vector *r0 = &vel[(size_t)j * n + i], *r1 = r0 + n;
	Vector a (r0[0] + (r0[1] - r0[0]) * fu), b (r1[0] + (r1[1] - r1[0]) * fu);
	return (a + (b - a) * fv) * exp (-kp * std::max (d, 0.0));
}

void Forces (const Model &m, const Surface &s, const Body &b, Result &r, double dt)
{
	const int parts = (int)r.partF.size();
	const double M = b.mass;
	const Vector I (b.pmi * M);
	auto IinvL = [&](const Vector &t) { // (local frame)
		Vector tb (tmul (b.R, t));
		return mul (b.R, Vector (tb.x / I.x, tb.y / I.y, tb.z / I.z));
	};
	const Vector xcg (b.x + mul (b.R, b.cg));
	// (within a sub-step (dt > 0) the velocities the forces see are those the
	// forces before them left: each push is applied as an impulse before the
	// next is found, as the contact solver does, so that together they can't
	// reverse the body's motion through the water)
	Vector vv (b.v), wl (mul (b.R, b.w));
	const double hc = m.h, vc = hc * hc * hc;
	Vector F, T; // (local frame; T about the CG)
	auto add = [&](const Vector &f, const Vector &p, int part, bool drag) {
		F += f;
		Vector t (crossp (p - xcg, f));
		T += t;
		if (part >= 0 && part < parts) r.partF[part] += tmul (b.R, f);
		if (drag && dt > 0.0) vv += f * (dt / M), wl += IinvL (t) * dt;
	};
	// (a point's response to a push along a direction: its inverse effective mass)
	auto kInv = [&](const Vector &rc, const Vector &dir) {
		return 1.0 / M + dotp (crossp (IinvL (crossp (rc, dir)), rc), dir);
	};

	// cells: buoyancy, and the waterline's damping
	double awp = 0.0, sub = 0.0;
	static thread_local std::vector<double> depth;
	depth.resize (m.cells.size());
	for (size_t i = 0; i < m.cells.size(); i++) {
		const Cell &c = m.cells[i];
		Vector p (b.x + mul (b.R, c.c));
		double d = s.Height (p.x, p.z) - p.y;
		depth[i] = d;
		double f = std::clamp (0.5 + d / hc, 0.0, 1.0);
		if (f <= 0.0) continue;
		if (f < 1.0) awp += c.w * hc * hc;
		sub += f * c.w * vc;
		add (Vector (0.0, s.rho * s.g * f * c.w * vc, 0.0), p, c.part, false);
	}
	if (awp > 0.0) {
		// (heave damping per unit waterplane area: ZETA of critical for the heave)
		double ca = 2.0 * ZETA * sqrt (s.rho * s.g * M / awp);
		for (size_t i = 0; i < m.cells.size(); i++) {
			double f = 0.5 + depth[i] / hc;
			if (f <= 0.0 || f >= 1.0) continue;
			const Cell &c = m.cells[i];
			Vector p (b.x + mul (b.R, c.c));
			Vector rc (p - xcg);
			double vr = (vv + crossp (wl, rc)).y - s.Vel (p.x, p.z, 0.0).y;
			double fd = -ca * c.w * hc * hc * vr;
			if (dt > 0.0) { // (no reversal within the sub-step)
				double lim = fabs (vr) / (kInv (rc, Vector (0, 1, 0)) * dt);
				fd = std::clamp (fd, -lim, lim);
			}
			add (Vector (0.0, fd, 0.0), p, c.part, true);
		}
	}

	// panels: pressure, slamming, friction. The laws give each panel's push
	// from the velocities at the start; within a sub-step (dt > 0) these are
	// the bounds of its impulses, which are then found by projected
	// Gauss-Seidel passes as the contact solver's: no panel is pushed beyond
	// stopping it relative to the water, together (a body under water at
	// speed is stopped, not thrown back by the pushes on its first panels)
	struct PS { Vector p, n, rc, vw, dt; double jn, jt, ln, lt, kn, kt; int part; };
	static thread_local std::vector<PS> ps;
	ps.clear();
	double wetted = 0.0;
	for (const Panel &q : m.panels) {
		Vector p (b.x + mul (b.R, q.c));
		Vector n (mul (b.R, q.n));
		double sx, sz;
		double d = s.Height (p.x, p.z, &sx, &sz) - p.y;
		double ext = std::max ({ q.size * sqrt (std::max (0.0, 1.0 - n.y * n.y)), 0.1 * q.size, 0.02 });
		double wf = std::clamp (0.5 + d / ext, 0.0, 1.0);
		Vector rc (p - xcg);
		Vector vw (s.Vel (p.x, p.z, std::max (d, 0.0)));
		Vector u (vv + crossp (wl, rc) - vw);
		Vector ns (Vector (-sx, 1.0, -sz).unit());
		double w = dotp (u, ns); // (< 0: going down into the water)
		// (dry, and not reaching the water within the sub-step)
		if (wf <= 0.0 && (dt <= 0.0 || d - w * dt <= -0.5 * ext)) continue;
		double A = q.area * wf;
		wetted += A;
		double U = u.length();
		double un = dotp (u, n);
		double pn = 0.0;
		if (un > 0.0 && U > 0.0) {
			double sa = un / U;
			pn = 0.5 * s.rho * U * U * std::min (PI_ * sa, CN_MAX);
		}
		// slamming: wetted from below, facing down into the surface. Wagner's
		// pressure over the panel's wetting, as the impulse of the part of its
		// wetting band it goes through in the sub-step (the integral of the
		// pressure over the entry, for any step)
		double jslam = 0.0, cb = -dotp (n, ns);
		if (cb > 0.0 && w < 0.0 && wf < 1.0) {
			double sb = sqrt (std::max (0.0, 1.0 - cb * cb));
			double Cs = std::min (PI_ * cb / std::max (sb, 1e-6), CS_MAX);
			double psl = 0.5 * s.rho * w * w * Cs;
			r.slamMax = std::max (r.slamMax, psl);
			r.entrySpeed = std::max (r.entrySpeed, -w);
			if (dt > 0.0) {
				// (the band: depth -ext/2 .. ext/2; the sub-step's stretch: d .. d - w dt)
				double a0 = std::max (d, -0.5 * ext), a1 = std::min (d - w * dt, 0.5 * ext);
				if (a1 > a0) jslam = 0.5 * s.rho * (-w) * Cs * q.area * (a1 - a0);
			} else {
				pn += psl;
			}
		}
		r.pressureMax = std::max (r.pressureMax, pn);
		// skin friction
		Vector ut (u - n * un);
		double Ut = ut.length(), ft = 0.0;
		Vector dirT (0, 0, 0);
		if (Ut > 1e-6) {
			ft = 0.5 * s.rho * Cf (Ut * std::max (m.length, 0.1) / s.nu) * Ut * Ut * A;
			dirT = ut / Ut;
		}
		if (pn <= 0.0 && ft <= 0.0 && jslam <= 0.0) continue;
		PS x;
		x.p = p, x.n = n, x.rc = rc, x.vw = vw, x.dt = dirT, x.part = q.part;
		x.ln = x.lt = 0.0;
		if (dt > 0.0) {
			x.jn = pn * A * dt + jslam, x.jt = ft * dt;
			x.kn = kInv (rc, n), x.kt = ft > 0.0 ? kInv (rc, dirT) : 1.0;
		} else {
			x.ln = pn * A, x.lt = ft; // (the laws' forces as they are)
		}
		ps.push_back (x);
	}
	if (dt > 0.0) {
		for (int it = 0; it < 8; it++) {
			double moved = 0.0;
			// (sweeps forward and back in turn: no bias to the panels met first)
			for (size_t q = 0; q < ps.size(); q++) {
				PS &x = ps[it % 2 ? ps.size() - 1 - q : q];
				Vector u (vv + crossp (wl, x.rc) - x.vw);
				double dn = std::clamp (dotp (u, x.n) / x.kn, -x.ln, x.jn - x.ln);
				if (dn != 0.0) {
					x.ln += dn;
					Vector J (x.n * -dn);
					vv += J / M, wl += IinvL (crossp (x.rc, J));
					u = vv + crossp (wl, x.rc) - x.vw;
				}
				double dtg = x.jt > 0.0 ? std::clamp (dotp (u, x.dt) / x.kt, -x.lt, x.jt - x.lt) : 0.0;
				if (dtg != 0.0) {
					x.lt += dtg;
					Vector J (x.dt * -dtg);
					vv += J / M, wl += IinvL (crossp (x.rc, J));
				}
				moved = std::max (moved, std::max (fabs (dn), fabs (dtg)));
			}
			if (moved * dt <= 1e-12) break;
		}
	}
	if (debugPanels > 0 && !ps.empty()) {
		std::vector<const PS*> top;
		for (const PS &x : ps) top.push_back (&x);
		std::sort (top.begin(), top.end(), [](const PS *a, const PS *b) { return a->ln > b->ln; });
		for (int q = 0; q < std::min ((int)top.size(), debugPanels); q++) {
			Vector cb (tmul (b.R, top[q]->rc + xcg - b.x)), nb (tmul (b.R, top[q]->n));
			fprintf (stderr, "hydro panel c (%.2f %.2f %.2f) n (%.2f %.2f %.2f) Jn %.0f Jt %.0f\n", cb.x, cb.y, cb.z, nb.x, nb.y, nb.z, top[q]->ln, top[q]->lt);
		}
		fprintf (stderr, "--\n");
	}
	for (const PS &x : ps) {
		double sc = dt > 0.0 ? 1.0 / dt : 1.0;
		Vector f (x.n * (-x.ln * sc) + x.dt * (-x.lt * sc));
		F += f;
		T += crossp (x.rc, f);
		if (x.part >= 0 && x.part < parts) r.partF[x.part] += tmul (b.R, f);
	}
	r.F = F;
	r.T = tmul (b.R, T);
	r.submerged = sub;
	r.wetted = wetted;
	r.waterline = awp;
}

int Substeps (const Model &m, const Surface &s, const Body &b, double h, const Vector &aExt)
{
	if (h <= 0.0) return 1;
	// the buoyancy's stiffness: rho g A over the mass (A: a generous waterplane)
	double A = 2.0 * pow (std::max (m.volume, 1e-6), 2.0 / 3.0);
	double wn = sqrt (s.rho * s.g * A / std::max (b.mass, 1e-3));
	double dt = 0.3 / wn;
	// the speed over the cells
	double speed = std::max (b.v.length(), (b.v + aExt * h).length()) + b.w.length() * m.radius;
	if (speed > 0.0) dt = std::min (dt, 0.5 * std::max (m.h, 0.02) / speed);
	return std::clamp ((int)ceil (h / dt), 1, MAX_SUBSTEPS);
}

void Step (const Model &m, const Surface &s, Body &b, const Vector &aExt, const Vector &alphaExt, double h, int parts, Result &r)
{
	r.Clear (parts);
	int n = Substeps (m, s, b, h, aExt);
	double dt = h / n;
	r.substeps = n;
	Result ri;
	Vector xcg (b.x + mul (b.R, b.cg));
	for (int i = 0; i < n; i++) {
		ri.Clear (parts);
		Forces (m, s, b, ri, dt);
		Vector aw (ri.F / b.mass);
		r.accMax = std::max (r.accMax, aw.length());
		b.v += (aExt + aw) * dt;
		xcg += b.v * dt;
		Vector tb (ri.T);
		Vector I (b.pmi * b.mass);
		b.w += (alphaExt + Vector (tb.x / I.x, tb.y / I.y, tb.z / I.z)) * dt;
		b.R = b.R * Rot (b.w * dt);
		b.x = xcg - mul (b.R, b.cg);
		r.F += ri.F / n;
		r.T += ri.T / n;
		for (int j = 0; j < parts; j++) r.partF[j] += ri.partF[j] / n;
		r.slamMax = std::max (r.slamMax, ri.slamMax);
		r.pressureMax = std::max (r.pressureMax, ri.pressureMax);
		r.entrySpeed = std::max (r.entrySpeed, ri.entrySpeed);
	}
	Orthonormalise (b.R);
	b.x = xcg - mul (b.R, b.cg);
	r.submerged = ri.submerged;
	r.wetted = ri.wetted;
	r.waterline = ri.waterline;
}

Matrix TrimMatrix (double pitch, double roll)
{
	double cp = cos (pitch), sp = sin (pitch), cr = cos (roll), sr = sin (roll);
	Matrix Rx (1, 0, 0, 0, cp, -sp, 0, sp, cp), Rz (cr, -sr, 0, sr, cr, 0, 0, 0, 1);
	return Rx * Rz;
}

bool Equilibrium (const Model &m, double mass, const Vector &cg, double rho, double &y, double &pitch, double &roll)
{
	if (m.Empty() || mass <= 0.0 || rho <= 0.0 || mass / rho >= m.volume) return false;
	const double L = std::max (m.length, 0.1);
	// residuals: displacement over the needed, and the centre of buoyancy's
	// horizontal offset from the centre of gravity (over the length)
	auto F = [&](double yo, double th, double ph, double r[3]) {
		Matrix R (TrimMatrix (th, ph));
		Vector up (tmul (R, Vector (0, 1, 0)));
		Vector cb;
		double v = Submerged (m, up * -yo, up, &cb);
		Vector d (mul (R, cb - cg));
		r[0] = v * rho / mass - 1.0, r[1] = d.x / L, r[2] = d.z / L;
	};
	// the draft upright first (bisection on the height)
	double lo = -m.radius - m.h, hi = m.radius + m.h;
	for (int i = 0; i < 60; i++) {
		double mid = 0.5 * (lo + hi), r[3];
		F (mid, 0.0, 0.0, r);
		(r[0] > 0.0 ? lo : hi) = mid; // (too deep: displaces too much -> higher)
	}
	double u[3] = { 0.5 * (lo + hi), 0.0, 0.0 };
	// then Newton on height, pitch and roll (Jacobian by differences, steps limited)
	for (int it = 0; it < 40; it++) {
		double r[3], J[3][3];
		F (u[0], u[1], u[2], r);
		if (fabs (r[0]) < 1e-4 && fabs (r[1]) < 1e-5 && fabs (r[2]) < 1e-5) break;
		const double d[3] = { 1e-3 * L, 1e-3, 1e-3 };
		for (int k = 0; k < 3; k++) {
			double v[3] = { u[0], u[1], u[2] }, rk[3];
			v[k] += d[k];
			F (v[0], v[1], v[2], rk);
			for (int q = 0; q < 3; q++) J[q][k] = (rk[q] - r[q]) / d[k];
		}
		// solve J du = -r (Cramer)
		auto det3 = [](double a[3][3]) {
			return a[0][0]*(a[1][1]*a[2][2]-a[1][2]*a[2][1]) - a[0][1]*(a[1][0]*a[2][2]-a[1][2]*a[2][0]) + a[0][2]*(a[1][0]*a[2][1]-a[1][1]*a[2][0]);
		};
		double D = det3 (J);
		if (fabs (D) < 1e-18) break;
		double du[3];
		for (int k = 0; k < 3; k++) {
			double A[3][3];
			for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) A[a][b] = b == k ? -r[a] : J[a][b];
			du[k] = det3 (A) / D;
		}
		// (no more than a tenth of the length, 10 deg a step; pitch and roll within 60 deg)
		du[0] = std::clamp (du[0], -0.1 * L, 0.1 * L);
		du[1] = std::clamp (du[1], -0.17, 0.17);
		du[2] = std::clamp (du[2], -0.17, 0.17);
		u[0] += du[0];
		u[1] = std::clamp (u[1] + du[1], -1.05, 1.05);
		u[2] = std::clamp (u[2] + du[2], -1.05, 1.05);
	}
	y = u[0], pitch = u[1], roll = u[2];
	double r[3];
	F (y, pitch, roll, r);
	return fabs (r[0]) < 0.02;
}

} // namespace hydro
