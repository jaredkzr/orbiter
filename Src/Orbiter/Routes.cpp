// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Routes.h"
#include "Conics.h"
#include "Maneuver.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace transfer {
namespace route {

namespace {
	const double DAY = 86400.0;
	const double BAD = 1e30;
	const int MAXSOL = 9;     // Lambert solutions per leg (0 revolutions, then two per count)
	const double TOF_PENALTY = 50.0; // [m/s per day] beyond the longest flight

	double DepCost (const Problem &p, const Vector &vinf)
	{
		double v = vinf.length();
		switch (p.dep) {
		case DEP_PARK: {
			double mu = p.depBody.mu, rp = p.parkR;
			return sqrt (v*v + 2.0 * mu / rp) - sqrt (mu / rp);
		}
		default: return v; // (DEP_VINF; DEP_ORBIT: the burn)
		}
	}

	double ArrCost (const Problem &p, const Vector &vinf)
	{
		double v = vinf.length();
		switch (p.arr) {
		case ARR_CAPTURE: {
			const Body &t = p.enc.back();
			double rp = p.capRp, ra = std::max (p.capRa, rp);
			double vp = sqrt (v*v + 2.0 * t.mu / rp);
			double vc = sqrt (t.mu * (2.0 / rp - 2.0 / (rp + ra)));
			return std::max (0.0, vp - vc);
		}
		case ARR_VINF: return v;
		default: return 0.0;
		}
	}

	// the departure point: the departure body, or the vessel's orbit
	bool Start (const Problem &p, double mjd, Vector &r, Vector &v)
	{
		if (p.dep == DEP_ORBIT) return maneuver::Kepler (p.r0, p.v0, p.mu, (mjd - p.mjd0) * DAY, r, v);
		return p.depBody.state && p.depBody.state (mjd, r, v);
	}

	// frame of a departure excess velocity: along the velocity, normal, in between
	void DepFrame (const Vector &r, const Vector &v, Vector &i, Vector &j, Vector &k)
	{
		i = v.unit();
		k = crossp (r, v).unit();
		j = crossp (k, i).unit();
	}

	// the Lambert solutions of a leg
	int Solutions (const Problem &p, const Vector &r1, const Vector &r2, double tof, Vector *v1, Vector *v2, int *revs)
	{
		int n = 0;
		if (Lambert (r1, r2, tof, p.mu, p.nref, false, 0, 0, v1[n], v2[n])) revs[n++] = 0;
		for (int N = 1; N <= p.maxRevs && n + 2 <= MAXSOL; N++)
			for (int br = 0; br < 2; br++)
				if (Lambert (r1, r2, tof, p.mu, p.nref, false, N, br, v1[n], v2[n])) revs[n++] = N;
		return n;
	}

	// powered flyby with the body's limit on the periapsis
	double Flyby (const Body &b, const Vector &vin, const Vector &vout, double &rp, double &dp, double &dt)
	{
		if (b.rpSoft <= 0.0) return PoweredFlyby (vin, vout, b.mu, b.rpMin, rp, dp, dt);
		PoweredFlyby (vin, vout, b.mu, 0.0, rp, dp, dt);
		dt = (rp < b.rpMin) ? b.rpSoft * (b.rpMin - rp) : 0.0;
		return dp + dt;
	}

	double Penalty (const Problem &p, double tof)
	{
		return (p.maxTof > 0.0 && tof > p.maxTof) ? TOF_PENALTY * (tof - p.maxTof) : 0.0;
	}

	// the closest pass of a leg to the bodies to avoid, as a fraction of the
	// distance to keep (the leg's own bodies near its ends excepted): the conic
	// (r0, v0) from t0, after tm the conic (rm, vm)
	const double PASS_PENALTY = 3000.0; // [m/s] for a pass through a body's centre

	// times [s after the state] within dtmax at which the conic (r0, v0) is at distance R
	int Crossings (const Vector &r0, const Vector &v0, double mu, double R, double dtmax, double *dt, int nmax)
	{
		double r = r0.length(), v2 = dotp (v0, v0);
		Vector h (crossp (r0, v0));
		Vector ev ((r0 * (v2 - mu / r) - v0 * dotp (r0, v0)) / mu);
		double e = ev.length(), p = dotp (h, h) / mu;
		if (e < 1e-6) return 0; // (circular: at one distance)
		double c = (p / R - 1.0) / e;
		if (c < -1.0 || c > 1.0) return 0;
		double alpha = 2.0 / r - v2 / mu; // 1/a
		double nu0 = atan2 (dotp (crossp (ev, r0), h) / h.length(), dotp (ev, r0));
		int n = 0;
		if (alpha > 0.0 && e < 1.0) {
			double a = 1.0 / alpha, nm = sqrt (mu * alpha * alpha * alpha), P = Pi2 / nm;
			auto M = [&](double nu) { double E = 2.0 * atan (sqrt ((1.0 - e) / (1.0 + e)) * tan (0.5 * nu)); return E - e * sin (E); };
			(void)a;
			double M0 = M (nu0);
			for (double s : { 1.0, -1.0 }) {
				double d = fmod (M (s * acos (c)) - M0, Pi2);
				if (d < 0.0) d += Pi2;
				for (double t = d / nm; t <= dtmax && n < nmax; t += P) dt[n++] = t;
			}
		} else if (e > 1.0) {
			double nm = sqrt (mu * -alpha * -alpha * -alpha);
			auto M = [&](double nu) { double H = 2.0 * atanh (sqrt ((e - 1.0) / (e + 1.0)) * tan (0.5 * nu)); return e * sinh (H) - H; };
			double M0 = M (nu0);
			for (double s : { 1.0, -1.0 }) {
				double t = (M (s * acos (c)) - M0) / nm;
				if (t > 0.0 && t <= dtmax && n < nmax) dt[n++] = t;
			}
		}
		return n;
	}

	// the closest pass of a leg to the bodies to avoid, as a fraction of the
	// distance to keep (the leg's own bodies near its ends excepted): where
	// the leg crosses each body's distance from the central body (and, with
	// n > 0, at n samples). The conic (r0, v0) from t0, after tm (rm, vm).
	double LegPass (const Problem &p, double t0, double t1, const Vector &r0, const Vector &v0, double tm, const Vector &rm, const Vector &vm,
		const Body *a, const Body *b, int n, int *nearBody = nullptr, double *nearMjd = nullptr)
	{
		double best = 1e30, T = t1 - t0;
		auto Check = [&](size_t k, double t, const Vector &r) {
			const Body &o = p.avoid[k];
			// (the leg's own bodies, told by their parameter: not near its ends,
			// while on the way out of or into their distance to keep)
			if (o.radius <= 0.0) return;
			bool own = (a && o.mu == a->mu) || (b && o.mu == b->mu);
			if (own) {
				Vector q, w;
				double vrel = o.state (t0, q, w) ? std::max ((v0 - w).length(), 300.0) : 3000.0;
				double skip = std::max (0.08 * T, 1.5 * o.radius / vrel / DAY);
				if ((a && o.mu == a->mu && t - t0 < skip) || (b && o.mu == b->mu && t1 - t < skip)) return;
			}
			Vector q, w;
			if (!o.state (t, q, w)) return;
			double f = (r - q).length() / o.radius;
			if (f < best) {
				best = f;
				if (nearBody) *nearBody = (int)k, *nearMjd = t;
			}
		};
		for (size_t k = 0; k < p.avoid.size(); k++) {
			Vector q, w;
			if (p.avoid[k].radius <= 0.0 || !p.avoid[k].state (t0, q, w)) continue;
			// both conics of the leg, each over its time
			for (int part = 0; part < 2; part++) {
				double ta = part ? tm : t0, tb = part ? t1 : std::min (tm, t1);
				if (tb <= ta) continue;
				const Vector &rs = part ? rm : r0, &vs = part ? vm : v0;
				double R = q.length(), dt[16];
				for (int it = 0; it < 2; it++) { // (the body's distance at the crossing, again)
					int nc = Crossings (rs, vs, p.mu, R, (tb - ta) * DAY, dt, 16);
					for (int i = 0; i < nc; i++) {
						Vector r, v;
						if (!maneuver::Kepler (rs, vs, p.mu, dt[i], r, v)) continue;
						double t = ta + dt[i] / DAY;
						Check (k, t, r);
						if (it == 0 && i == 0 && p.avoid[k].state (t, q, w)) R = q.length();
					}
				}
			}
		}
		for (int i = 1; i < n; i++) {
			double t = t0 + T * i / n;
			Vector r, v;
			bool ok = (t < tm) ? maneuver::Kepler (r0, v0, p.mu, (t - t0) * DAY, r, v) : maneuver::Kepler (rm, vm, p.mu, (t - tm) * DAY, r, v);
			if (ok) for (size_t k = 0; k < p.avoid.size(); k++) Check (k, t, r);
		}
		return best;
	}

	double PassPenalty (double f) { return (f < 1.0) ? PASS_PENALTY * (1.0 - f) : 0.0; }

	// -------------------------------------------------------------------
	// MGA: Lambert legs, powered flybys; the legs' solutions (revolutions,
	// branches) chosen by dynamic programming

	double EvaluateMga (const Problem &p, const double *x, Result *res)
	{
		const int n = p.Legs ();
		if (n < 1) return BAD;
		std::vector<double> t (n + 1);
		t[0] = x[0];
		for (int k = 0; k < n; k++) t[k+1] = t[k] + x[1+k];
		Vector rA, vA;
		if (!Start (p, t[0], rA, vA)) return BAD;
		std::vector<Vector> rB (n), vB (n);
		for (int k = 0; k < n; k++)
			if (!p.enc[k].state (t[k+1], rB[k], vB[k])) return BAD;
		// solutions
		std::vector<Vector> V1 (n * MAXSOL), V2 (n * MAXSOL);
		std::vector<int> R (n * MAXSOL), ns (n);
		for (int k = 0; k < n; k++) {
			const Vector &r1 = (k == 0) ? rA : rB[k-1];
			ns[k] = Solutions (p, r1, rB[k], x[1+k] * DAY, &V1[k*MAXSOL], &V2[k*MAXSOL], &R[k*MAXSOL]);
			if (!ns[k]) return BAD;
		}
		// dynamic programming over the legs
		std::vector<double> cost (n * MAXSOL, BAD);
		std::vector<int> from (n * MAXSOL, -1);
		for (int s = 0; s < ns[0]; s++)
			cost[s] = DepCost (p, V1[s] - vA);
		for (int k = 1; k < n; k++) {
			const Body &fb = p.enc[k-1];
			for (int s2 = 0; s2 < ns[k]; s2++) {
				Vector vout (V1[k*MAXSOL + s2] - vB[k-1]);
				for (int s1 = 0; s1 < ns[k-1]; s1++) {
					double c0 = cost[(k-1)*MAXSOL + s1];
					if (c0 >= BAD) continue;
					Vector vin (V2[(k-1)*MAXSOL + s1] - vB[k-1]);
					double rp, dp, dt;
					double c = c0 + Flyby (fb, vin, vout, rp, dp, dt);
					if (c < cost[k*MAXSOL + s2]) cost[k*MAXSOL + s2] = c, from[k*MAXSOL + s2] = s1;
				}
			}
		}
		double best = BAD;
		int bs = -1;
		for (int s = 0; s < ns[n-1]; s++) {
			double c0 = cost[(n-1)*MAXSOL + s];
			if (c0 >= BAD) continue;
			double c = c0 + ArrCost (p, V2[(n-1)*MAXSOL + s] - vB[n-1]);
			if (c < best) best = c, bs = s;
		}
		if (bs < 0) return BAD;
		double tof = t[n] - t[0];
		best += Penalty (p, tof);
		std::vector<int> sel (n);
		sel[n-1] = bs;
		for (int k = n - 1; k > 0; k--) sel[k-1] = from[k*MAXSOL + sel[k]];
		if (!p.avoid.empty()) // (the chosen legs clear of the other bodies)
			for (int k = 0; k < n; k++) {
				const Vector &r1 = (k == 0) ? rA : rB[k-1];
				best += PassPenalty (LegPass (p, t[k], t[k+1], r1, V1[k*MAXSOL + sel[k]], 1e30, r1, Vector (),
					k > 0 ? &p.enc[k-1] : (p.dep != DEP_ORBIT ? &p.depBody : nullptr), &p.enc[k], 0));
			}
		if (!res) return best;

		// the route
		Result &o = *res;
		o = Result ();
		o.x.assign (x, x + Dimension (p));
		o.dep = t[0], o.tof = tof;
		o.legs.resize (n), o.enc.resize (n);
		for (int k = 0; k < n; k++) {
			int s = k*MAXSOL + sel[k];
			Leg &L = o.legs[k];
			L.t0 = t[k], L.t1 = t[k+1];
			L.r0 = (k == 0) ? rA : rB[k-1];
			L.v0 = V1[s];
			L.r1 = rB[k], L.v1 = V2[s];
			L.revs = R[s];
			Encounter &E = o.enc[k];
			E.mjd = t[k+1];
			E.vinfIn = V2[s] - vB[k];
		}
		o.vinfDep = o.legs[0].v0 - vA;
		o.dvDep = DepCost (p, o.vinfDep);
		for (int k = 0; k + 1 < n; k++) {
			Encounter &E = o.enc[k];
			E.vinfOut = o.legs[k+1].v0 - vB[k];
			Flyby (p.enc[k], E.vinfIn, E.vinfOut, E.rp, E.dvPeri, E.dvTurn);
			E.turn = acos (std::clamp (dotp (E.vinfIn.unit(), E.vinfOut.unit()), -1.0, 1.0));
			if (E.rp < p.enc[k].rpMin && p.enc[k].rpSoft <= 0.0) E.rp = p.enc[k].rpMin;
			o.dvFlyby += E.dvPeri + E.dvTurn;
		}
		o.dvArr = ArrCost (p, o.enc[n-1].vinfIn);
		o.dv = best;
		o.valid = true;
		return best;
	}

	// -------------------------------------------------------------------
	// MGA-1DSM

	double EvaluateDsm (const Problem &p, const double *x, Result *res)
	{
		const int n = p.Legs ();
		if (n < 1) return BAD;
		double t = x[0];
		Vector rA, vA;
		if (!Start (p, t, rA, vA)) return BAD;
		Vector ei, ej, ek;
		DepFrame (rA, vA, ei, ej, ek);
		double theta = Pi2 * x[2], phi = acos (std::clamp (2.0 * x[3] - 1.0, -1.0, 1.0)) - 0.5 * Pi;
		Vector vinf ((ei * (cos (theta) * cos (phi)) + ej * (sin (theta) * cos (phi)) + ek * sin (phi)) * x[1]);
		double dvDep = DepCost (p, vinf);
		double total = dvDep, dvDsm = 0.0;
		Vector r (rA), v (vA + vinf);
		if (res) {
			*res = Result ();
			res->legs.resize (n), res->enc.resize (n);
			res->vinfDep = vinf, res->dvDep = dvDep, res->dep = t;
		}
		const double t0 = t;
		Vector V1[MAXSOL], V2[MAXSOL];
		int R[MAXSOL];
		for (int k = 0; k < n; k++) {
			double eta, T;
			if (k == 0) eta = x[4], T = x[5];
			else { int b = 6 + 4 * (k - 1); eta = x[b+2], T = x[b+3]; }
			Vector rd, vd;
			if (!maneuver::Kepler (r, v, p.mu, eta * T * DAY, rd, vd)) return BAD;
			Vector rB, vB;
			if (!p.enc[k].state (t + T, rB, vB)) return BAD;
			int ns = Solutions (p, rd, rB, (1.0 - eta) * T * DAY, V1, V2, R);
			if (!ns) return BAD;
			int bs = 0;
			double bc = BAD;
			for (int s = 0; s < ns; s++) {
				double c = (V1[s] - vd).length();
				if (c < bc) bc = c, bs = s;
			}
			dvDsm += bc;
			total += bc;
			if (!p.avoid.empty())
				total += PassPenalty (LegPass (p, t, t + T, r, v, t + eta * T, rd, V1[bs],
					k > 0 ? &p.enc[k-1] : (p.dep != DEP_ORBIT ? &p.depBody : nullptr), &p.enc[k], 0));
			Vector vin (V2[bs] - vB);
			if (res) {
				Leg &L = res->legs[k];
				L.t0 = t, L.t1 = t + T;
				L.r0 = r, L.v0 = v;
				L.dsmMjd = t + eta * T, L.dsmR = rd, L.dsmDv = V1[bs] - vd;
				L.r1 = rB, L.v1 = V2[bs];
				L.revs = R[bs];
				res->enc[k].mjd = t + T;
				res->enc[k].vinfIn = vin;
			}
			t += T;
			if (k + 1 < n) {
				int b = 6 + 4 * k;
				const Body &fb = p.enc[k];
				double rp = fb.rpMin * exp (x[b+1]);
				Vector vout (FlybyOut (vin, fb.mu, rp, x[b], vB));
				if (res) {
					Encounter &E = res->enc[k];
					E.vinfOut = vout, E.rp = rp, E.beta = x[b];
					E.turn = FlybyTurn (vin.length(), rp, fb.mu);
				}
				r = rB, v = vB + vout;
			} else {
				double ca = ArrCost (p, vin);
				total += ca;
				if (res) res->dvArr = ca;
			}
		}
		total += Penalty (p, t - t0);
		if (res) {
			res->dvDsm = dvDsm;
			res->tof = t - t0;
			res->dv = total;
			res->x.assign (x, x + Dimension (p));
			res->valid = std::isfinite (total);
		}
		return std::isfinite (total) ? total : BAD;
	}
}

double CheckPasses (const Problem &p, Result &r, int samples)
{
	double least = 1e30;
	if (p.avoid.empty() || !r.valid) return least;
	for (size_t k = 0; k < r.legs.size(); k++) {
		Leg &L = r.legs[k];
		const Body *a = k > 0 ? &p.enc[k-1] : (p.dep != DEP_ORBIT ? &p.depBody : nullptr);
		double tm = (L.dsmMjd > 0.0) ? L.dsmMjd : 1e30;
		// (after a manoeuvre: the conic from it)
		Vector rm (L.dsmR), vmm;
		if (L.dsmMjd > 0.0) {
			Vector r, v;
			maneuver::Kepler (L.r0, L.v0, p.mu, (L.dsmMjd - L.t0) * DAY, r, v);
			vmm = v + L.dsmDv;
		}
		L.nearBody = -1;
		L.nearDist = LegPass (p, L.t0, L.t1, L.r0, L.v0, tm, rm, vmm, a, &p.enc[k], samples, &L.nearBody, &L.nearMjd);
		least = std::min (least, L.nearDist);
	}
	return least;
}

int Dimension (const Problem &p)
{
	int n = p.Legs ();
	return p.dsm ? 6 + 4 * (n - 1) : 1 + n;
}

void Bounds (const Problem &p, std::vector<double> &lo, std::vector<double> &hi)
{
	int n = p.Legs ();
	lo.clear (), hi.clear ();
	lo.push_back (p.t0lo), hi.push_back (p.t0hi);
	if (!p.dsm) {
		for (int k = 0; k < n; k++) lo.push_back (p.tlo[k]), hi.push_back (p.thi[k]);
		return;
	}
	auto Add = [&](double a, double b) { lo.push_back (a), hi.push_back (b); };
	Add (0.0, p.vinfMax);
	Add (0.0, 1.0), Add (0.0, 1.0);
	Add (0.02, 0.98), Add (p.tlo[0], p.thi[0]);
	for (int k = 1; k < n; k++) {
		Add (-Pi, Pi);
		Add (0.0, log (std::max (1.01, p.rpMaxFactor)));
		Add (0.02, 0.98);
		Add (p.tlo[k], p.thi[k]);
	}
}

double Evaluate (const Problem &p, const double *x, Result *res)
{
	return p.dsm ? EvaluateDsm (p, x, res) : EvaluateMga (p, x, res);
}

bool ToDsm (const Problem &p, const Result &mga, std::vector<double> &x)
{
	int n = p.Legs ();
	if (!mga.valid || (int)mga.legs.size() != n || (int)mga.enc.size() != n) return false;
	std::vector<double> lo, hi;
	Bounds (p, lo, hi);
	x.assign (Dimension (p), 0.0);
	x[0] = mga.dep;
	Vector rA, vA;
	if (!Start (p, mga.dep, rA, vA)) return false;
	Vector ei, ej, ek;
	DepFrame (rA, vA, ei, ej, ek);
	double vinf = mga.vinfDep.length();
	if (vinf <= 0.0) return false;
	Vector c (mga.vinfDep / vinf);
	double phi = asin (std::clamp (dotp (c, ek), -1.0, 1.0));
	double theta = atan2 (dotp (c, ej), dotp (c, ei));
	if (theta < 0.0) theta += Pi2;
	x[1] = vinf, x[2] = theta / Pi2, x[3] = 0.5 * (cos (phi + 0.5 * Pi) + 1.0);
	x[4] = 0.5, x[5] = mga.legs[0].t1 - mga.legs[0].t0;
	for (int k = 1; k < n; k++) {
		int b = 6 + 4 * (k - 1);
		const Encounter &E = mga.enc[k-1];
		Vector rB, vB;
		if (!p.enc[k-1].state (E.mjd, rB, vB)) return false;
		// the B-plane angle of the outgoing excess velocity (as FlybyOut measures it)
		Vector i (E.vinfIn.unit()), j (crossp (i, vB).unit()), kk (crossp (i, j).unit());
		x[b] = atan2 (dotp (E.vinfOut, kk), dotp (E.vinfOut, j));
		double rp = std::max (E.rp, p.enc[k-1].rpMin);
		x[b+1] = log (rp / p.enc[k-1].rpMin);
		x[b+2] = 0.5;
		x[b+3] = mga.legs[k].t1 - mga.legs[k].t0;
	}
	for (size_t d = 0; d < x.size(); d++) x[d] = std::clamp (x[d], lo[d], hi[d]);
	// each manoeuvre where it costs least: (a Lambert arc near 180 degrees from it
	// to the next body has an ill-defined plane)
	for (int k = 0; k < n; k++) {
		int i = (k == 0) ? 4 : 6 + 4 * (k - 1) + 2;
		double be = x[i], bf = Evaluate (p, x.data());
		for (int j = 1; j < 20; j++) {
			x[i] = 0.05 * j;
			double f = Evaluate (p, x.data());
			if (f < bf) bf = f, be = x[i];
		}
		x[i] = be;
	}
	return true;
}

// =======================================================================
// Optimisation in the unit cube

namespace {
	struct Scaled {
		const Problem &p;
		std::vector<double> lo, hi;
		mutable std::vector<double> x;
		mutable long evals = 0;
		explicit Scaled (const Problem &pr): p(pr) { Bounds (p, lo, hi); x.resize (lo.size()); }
		int D () const { return (int)lo.size(); }
		void ToX (const double *u, double *xo) const
		{
			for (size_t d = 0; d < lo.size(); d++) xo[d] = lo[d] + (hi[d] - lo[d]) * std::clamp (u[d], 0.0, 1.0);
		}
		void ToU (const double *xi, double *u) const
		{
			for (size_t d = 0; d < lo.size(); d++)
				u[d] = (hi[d] > lo[d]) ? std::clamp ((xi[d] - lo[d]) / (hi[d] - lo[d]), 0.0, 1.0) : 0.0;
		}
		double operator() (const double *u) const
		{
			ToX (u, x.data());
			evals++;
			return Evaluate (p, x.data());
		}
	};

	// Nelder-Mead (adaptive parameters, Gao and Han 2012) in the unit cube
	double NelderMead (const Scaled &f, std::vector<double> &u, double step, long maxEvals, const std::atomic<bool> *abort)
	{
		const int D = f.D();
		const double al = 1.0, be = 1.0 + 2.0 / D, ga = 0.75 - 0.5 / D, de = 1.0 - 1.0 / D;
		std::vector<std::vector<double>> s (D + 1, u);
		std::vector<double> fv (D + 1);
		for (int i = 0; i < D; i++) {
			double &c = s[i+1][i];
			c = (c + step <= 1.0) ? c + step : c - step;
		}
		long e0 = f.evals;
		for (int i = 0; i <= D; i++) fv[i] = f (s[i].data());
		std::vector<int> idx (D + 1);
		std::vector<double> cen (D), xr (D), xe (D), xc (D);
		auto Clamp = [](std::vector<double> &v) { for (auto &c : v) c = std::clamp (c, 0.0, 1.0); };
		while (f.evals - e0 < maxEvals && !(abort && *abort)) {
			for (int i = 0; i <= D; i++) idx[i] = i;
			std::sort (idx.begin(), idx.end(), [&](int a, int b) { return fv[a] < fv[b]; });
			int b = idx[0], w = idx[D], sw = idx[D-1];
			// converged: the simplex is tiny
			double size = 0.0;
			for (int i = 0; i <= D; i++) for (int d = 0; d < D; d++) size = std::max (size, fabs (s[i][d] - s[b][d]));
			if (size < 1e-9 || fabs (fv[w] - fv[b]) < 1e-9 * (1.0 + fabs (fv[b]))) break;
			std::fill (cen.begin(), cen.end(), 0.0);
			for (int i = 0; i <= D; i++) if (i != w) for (int d = 0; d < D; d++) cen[d] += s[i][d] / D;
			for (int d = 0; d < D; d++) xr[d] = cen[d] + al * (cen[d] - s[w][d]);
			Clamp (xr);
			double fr = f (xr.data());
			if (fr < fv[b]) {
				for (int d = 0; d < D; d++) xe[d] = cen[d] + be * (xr[d] - cen[d]);
				Clamp (xe);
				double fe = f (xe.data());
				if (fe < fr) s[w] = xe, fv[w] = fe; else s[w] = xr, fv[w] = fr;
			} else if (fr < fv[sw]) {
				s[w] = xr, fv[w] = fr;
			} else {
				bool outside = fr < fv[w];
				for (int d = 0; d < D; d++) xc[d] = outside ? cen[d] + ga * (xr[d] - cen[d]) : cen[d] - ga * (cen[d] - s[w][d]);
				Clamp (xc);
				double fc = f (xc.data());
				if (fc < std::min (fr, fv[w])) s[w] = xc, fv[w] = fc;
				else for (int i = 0; i <= D; i++) if (i != b) {
					for (int d = 0; d < D; d++) s[i][d] = s[b][d] + de * (s[i][d] - s[b][d]);
					fv[i] = f (s[i].data());
				}
			}
		}
		int b = 0;
		for (int i = 1; i <= D; i++) if (fv[i] < fv[b]) b = i;
		u = s[b];
		return fv[b];
	}
}

double Polish (const Problem &p, std::vector<double> &x, int evals, const std::atomic<bool> *abort)
{
	Scaled f (p);
	std::vector<double> u (f.D());
	f.ToU (x.data(), u.data());
	double best = f (u.data());
	// restarted from the result while that improves (Nelder-Mead stalls on narrow valleys)
	for (int r = 0; r < 6 && f.evals < evals; r++) {
		std::vector<double> v (u);
		double c = NelderMead (f, v, r == 0 ? 0.01 : 0.002, evals - f.evals, abort);
		if (c < best - 1e-6) best = c, u = v; else break;
	}
	f.ToX (u.data(), x.data());
	return best;
}

Result Optimise (const Problem &p, const Options &o)
{
	Scaled f (p);
	const int D = f.D();
	std::mt19937 rng (o.seed);
	std::uniform_real_distribution<double> U (0.0, 1.0);
	const int NP = std::clamp (10 * D, 30, 120);
	const int stall = 60 + 6 * D;
	const int mbhFails = 20 + 2 * D;
	std::vector<double> bestU (D);
	double bestF = BAD;
	auto Abort = [&]() { return o.abort && *o.abort; };

	std::vector<std::vector<double>> pop (NP, std::vector<double> (D)), trial (NP, std::vector<double> (D));
	std::vector<double> fit (NP), F (NP), CR (NP);
	for (int restart = 0; f.evals < o.evals && !Abort(); restart++) {
		// population: random (independent of the earlier ones), the seeds in the first one
		for (int i = 0; i < NP; i++) {
			for (int d = 0; d < D; d++) pop[i][d] = U (rng);
			F[i] = 0.5, CR[i] = 0.9;
		}
		int k = 0;
		if (restart == 0)
			for (const auto &s : o.seeds) if ((int)s.size() == D && k < NP) f.ToU (s.data(), pop[k++].data());
		for (int i = 0; i < NP; i++) fit[i] = f (pop[i].data());
		double genBest = *std::min_element (fit.begin(), fit.end());
		int since = 0;
		while (since < stall && f.evals < o.evals && !Abort()) {
			// jDE: self-adaptive F and CR, DE/rand/1/bin
			for (int i = 0; i < NP; i++) {
				double Fi = (U (rng) < 0.1) ? 0.1 + 0.9 * U (rng) : F[i];
				double CRi = (U (rng) < 0.1) ? U (rng) : CR[i];
				int a, b, c;
				do a = (int)(U (rng) * NP); while (a == i || a >= NP);
				do b = (int)(U (rng) * NP); while (b == i || b == a || b >= NP);
				do c = (int)(U (rng) * NP); while (c == i || c == a || c == b || c >= NP);
				int jr = (int)(U (rng) * D);
				for (int d = 0; d < D; d++) {
					double v = (d == jr || U (rng) < CRi) ? pop[a][d] + Fi * (pop[b][d] - pop[c][d]) : pop[i][d];
					if (v < 0.0) v = U (rng) * pop[i][d];                  // (back between the parent and the bound)
					else if (v > 1.0) v = pop[i][d] + U (rng) * (1.0 - pop[i][d]);
					trial[i][d] = v;
				}
				double ft = f (trial[i].data());
				if (ft <= fit[i]) pop[i].swap (trial[i]), fit[i] = ft, F[i] = Fi, CR[i] = CRi;
			}
			double gb = *std::min_element (fit.begin(), fit.end());
			if (gb < genBest - 1e-4 * (1.0 + fabs (genBest))) genBest = gb, since = 0;
			else since++;
			if (o.progress) o.progress ((float)std::min (1.0, (double)f.evals / o.evals));
		}
		int ib = (int)(std::min_element (fit.begin(), fit.end()) - fit.begin());
		std::vector<double> u (pop[ib]);
		double fu = fit[ib];
		// polish the population's best
		for (int r = 0; r < 4 && !Abort(); r++) {
			std::vector<double> v (u);
			double c = NelderMead (f, v, r == 0 ? 0.01 : 0.002, 300 * D, o.abort);
			if (c < fu - 1e-6) fu = c, u = v; else break;
		}
		// monotonic basin hopping from there: local searches from random
		// neighbours, moving on when one is better (the global optimum of
		// these problems is often a narrow basin next to a broad one)
		int fails = 0;
		std::vector<double> y (D);
		while (fails < mbhFails && f.evals < o.evals && !Abort()) {
			double rho = (U (rng) < 0.5) ? 0.03 : 0.12;
			for (int d = 0; d < D; d++) y[d] = std::clamp (u[d] + rho * (2.0 * U (rng) - 1.0), 0.0, 1.0);
			double fy = NelderMead (f, y, 0.01, 200 * D, o.abort);
			if (fy < fu - 1e-3) fu = fy, u = y, fails = 0;
			else fails++;
			if (o.progress) o.progress ((float)std::min (1.0, (double)f.evals / o.evals));
		}
		// and to the bottom of that basin (restarted: the simplex collapses in narrow valleys)
		for (int r = 0; r < 8 && !Abort(); r++) {
			std::vector<double> v (u);
			double c = NelderMead (f, v, (r % 2) ? 0.002 : 0.02, 400 * D, o.abort);
			if (c < fu - 1e-4) fu = c, u = v; else if (r > 1) break;
		}
		if (fu < bestF) bestF = fu, bestU = u;
	}
	Result res;
	if (bestF < BAD) {
		std::vector<double> x (D);
		f.ToX (bestU.data(), x.data());
		Evaluate (p, x.data(), &res);
	}
	return res;
}

} // namespace route
} // namespace transfer
