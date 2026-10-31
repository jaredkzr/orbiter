// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Transfer.h"
#include "Routes.h"
#include "Trajectory.h"
#include "Orbiter.h"
#include "Celbody.h"
#include "Vessel.h"
#include "Element.h"
#include "Psys.h"
#include "Planet.h"
#include "Log.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <limits>

extern TimeData td;
extern PlanetarySystem *g_psys;

namespace transfer {

namespace {
	const float NaNf = std::numeric_limits<float>::quiet_NaN();
	const double DAY = 86400.0;
}

// =======================================================================
// Snapshot of the bodies of a transfer (taken on the simulation thread)

struct BodySnap {
	const CelestialBody *body = nullptr;
	double mu = 0.0, mass = 0.0, radius = 0.0, soi = 1e100;
	double parentMass = 0.0;
	bool moonOfPlanet = false;   // (its parent orbits something)
	std::shared_ptr<Elements> els;
	Vector pole;                 // rotation axis, in the sense of the body's orbital motion
};

struct Bodies {
	Bodies () = default;
	Bodies (const Bodies &o) { *this = o; }
	Bodies &operator= (const Bodies &o)
	{
		kind = o.kind, ref = o.ref, target = o.target, central = o.central;
		mjdRef = o.mjdRef, mjd0 = o.mjd0, r0 = o.r0, v0 = o.v0, nref = o.nref;
		objective = o.objective, capAlt = o.capAlt, capApo = o.capApo;
		landed = o.landed, padLocal = o.padLocal, parkR = o.parkR, ascentTime = o.ascentTime, minCoast = o.minCoast;
		tabRef = o.tabRef, tabTgt = o.tabTgt;
		tabRef.b = &ref, tabTgt.b = &target; // (the tables point at members)
		fly = o.fly, flyMinAlt = o.flyMinAlt, tabFly = o.tabFly, strict = o.strict;
		for (size_t i = 0; i < tabFly.size() && i < fly.size(); i++) tabFly[i].b = &fly[i];
		other = o.other, tabOther = o.tabOther;
		for (size_t i = 0; i < tabOther.size() && i < other.size(); i++) tabOther[i].b = &other[i];
		return *this;
	}
	Kind kind = KIND_NONE;
	BodySnap ref, target, central;
	double mjdRef = 0.0;         // MJD of simulation time 0
	double mjd0 = 0.0;           // now
	Vector r0, v0;               // vessel relative to ref (global orientation)
	Vector nref;                 // prograde sense of the transfer conic
	int objective = OBJ_TOTAL;
	double capAlt = 0.0, capApo = 0.0;
	// launch from the surface: the vessel is landed; searches assume a circular
	// parking orbit of radius parkR in the plane the transfer needs
	bool landed = false;
	Vector padLocal;             // launch site in the departure body's frame
	double parkR = 0.0;
	double ascentTime = 600.0, minCoast = 1200.0; // [s]

	// launch site at a time (global orientation, relative to the body's centre)
	Vector Pad (double mjd) const
	{
		Matrix R;
		ref.body->GetRotation ((mjd - mjdRef) * DAY, R);
		return mul (R, padLocal);
	}

	// state tables of the departure and target bodies relative to their parents (Hermite)
	struct Table {
		const BodySnap *b = nullptr;
		double t0 = 0.0, h = 1.0; // [MJD], [days]
		std::vector<Vector> p, v;
	};
	Table tabRef, tabTgt;
	bool strict = false;          // states from the tables only (threads of a search)
	// routes: the flyby bodies (the encounters: the flybys, then the target)
	std::vector<BodySnap> fly;
	std::vector<double> flyMinAlt;  // least periapsis altitude of each [m]
	std::vector<Table> tabFly;
	// routes: the bodies about the central body the legs keep clear of
	std::vector<BodySnap> other;
	std::vector<Table> tabOther;
	int Encounters () const { return (int)fly.size() + 1; }
	const BodySnap &Enc (int i) const { return (i >= 0 && i < (int)fly.size()) ? fly[i] : target; }
	const Table &EncTab (int i) const { return (i >= 0 && i < (int)fly.size()) ? tabFly[i] : tabTgt; }
	bool EncState (int i, double mjd, Vector &p, Vector &v) const { return State (EncTab (i), mjd, p, v); }

	bool Raw (const BodySnap &b, double mjd, Vector &p, Vector &v) const
	{
		CelestialBody::EphemState s;
		if (!b.body->EphemerisAtTime ((mjd - mjdRef) * DAY, s, b.els.get())) return false;
		if (s.havetrue) p = s.cpos, v = s.cvel;
		else if (s.havebary) p = s.bpos, v = s.bvel;
		else return false;
		if (s.parentbary && b.moonOfPlanet && b.parentMass > 0.0) {
			// relative to the barycentre of the planet and (mostly) this moon
			double f = 1.0 + b.mass / b.parentMass;
			p *= f, v *= f;
		}
		return true;
	}

	void MakeTable (Table &tab, const BodySnap &b, double t0, double t1, double h)
	{
		tab.b = &b;
		tab.t0 = t0, tab.h = h;
		size_t n = (size_t)ceil ((t1 - t0) / h) + 2;
		tab.p.resize (n), tab.v.resize (n);
		for (size_t i = 0; i < n; i++) {
			Vector p, v;
			if (!Raw (b, t0 + i * h, p, v)) { tab.p.clear (), tab.v.clear (); return; }
			tab.p[i] = p, tab.v[i] = v * DAY; // (velocity per day for the interpolation)
		}
	}

	bool State (const Table &tab, double mjd, Vector &p, Vector &v) const
	{
		double s = (mjd - tab.t0) / tab.h;
		long k = (long)floor (s);
		if (tab.p.empty() || k < 0 || k + 1 >= (long)tab.p.size()) return !strict && Raw (*tab.b, mjd, p, v);
		s -= k;
		double h = tab.h, s2 = s*s, s3 = s2*s;
		double h00 = 2*s3 - 3*s2 + 1, h10 = s3 - 2*s2 + s, h01 = -2*s3 + 3*s2, h11 = s3 - s2;
		double d00 = 6*s2 - 6*s, d10 = 3*s2 - 4*s + 1, d01 = -6*s2 + 6*s, d11 = 3*s2 - 2*s;
		const Vector &p0 = tab.p[k], &p1 = tab.p[k+1], &v0 = tab.v[k], &v1 = tab.v[k+1];
		p = p0 * h00 + v0 * (h10 * h) + p1 * h01 + v1 * (h11 * h);
		v = ((p0 * d00 + p1 * d01) / h + v0 * d10 + v1 * d11) / DAY;
		return true;
	}

	bool RefState (double mjd, Vector &p, Vector &v) const { return State (tabRef, mjd, p, v); }
	bool TargetState (double mjd, Vector &p, Vector &v) const { return State (tabTgt, mjd, p, v); }

	double ParkRadius () const { return landed ? parkR : r0.length(); }
	double CapPeri () const { return target.radius + capAlt; }
	double CapApo () const { return target.radius + std::max (capApo, capAlt); }

	double CaptureDv (double vinf) const
	{
		double rp = CapPeri (), ra = std::min (CapApo (), 0.9 * target.soi);
		ra = std::max (ra, rp);
		double vp = sqrt (vinf*vinf + 2.0 * target.mu / rp);
		double vc = sqrt (target.mu * (2.0 / rp - 2.0 / (rp + ra)));
		return std::max (0.0, vp - vc);
	}

	// patched-conic transfer for a departure date and time of flight
	bool Evaluate (double dep, double tof, Solution &s) const
	{
		s.valid = false;
		double tofs = tof * DAY;
		if (tofs <= 0.0) return false;
		Vector p2, w2;
		if (!TargetState (dep + tof, p2, w2)) return false;
		if (kind == KIND_SIBLING) {
			Vector p1, w1;
			if (!RefState (dep, p1, w1)) return false;
			if (!Lambert (p1, p2, tofs, central.mu, nref, false, s.v1, s.v2)) return false;
			s.r1 = p1, s.r2 = p2;
			s.vinfDepVec = s.v1 - w1;
			s.vinfDep = s.vinfDepVec.length();
			s.c3 = s.vinfDep * s.vinfDep;
			double rp = ParkRadius ();
			s.dvEject = sqrt (s.c3 + 2.0 * ref.mu / rp) - sqrt (ref.mu / rp);
		} else if (kind == KIND_MOON && landed) {
			// from the parking orbit in the plane of the transfer: the best point to leave it
			double tofs2 = tofs, mu = ref.mu, rp = parkR, vc = sqrt (mu / rp);
			Vector r2h (p2.unit()), nrm (crossp (p2, w2).unit());
			Vector q (crossp (nrm, r2h).unit());
			auto Cost = [&](double th, Vector *pv1, Vector *pv2, Vector *pr1) {
				Vector r1 ((r2h * cos (th) - q * sin (th)) * rp), v1, v2;
				if (!Lambert (r1, p2, tofs2, mu, nrm, false, v1, v2)) return 1e30;
				Vector vcirc (crossp (nrm, r1.unit()).unit() * vc);
				if (pv1) *pv1 = v1, *pv2 = v2, *pr1 = r1;
				return (v1 - vcirc).length();
			};
			double bt = 2.8, bc = 1e30;
			for (int i = 0; i <= 24; i++) {
				double th = (100.0 + 79.5 * i / 24.0) * RAD, c = Cost (th, 0, 0, 0);
				if (c < bc) bc = c, bt = th;
			}
			double lo = bt - 3.4 * RAD, hi = std::min (bt + 3.4 * RAD, 179.7 * RAD);
			for (int it = 0; it < 30; it++) {
				double m1 = lo + (hi - lo) / 3.0, m2 = hi - (hi - lo) / 3.0;
				if (Cost (m1, 0, 0, 0) < Cost (m2, 0, 0, 0)) hi = m2; else lo = m1;
			}
			if (Cost (0.5 * (lo + hi), &s.v1, &s.v2, &s.r1) >= 1e29) return false;
			s.r2 = p2;
			s.dvEject = Cost (0.5 * (lo + hi), 0, 0, 0);
			s.vinfDepVec = s.v1 - crossp (nrm, s.r1.unit()).unit() * vc;
			s.vinfDep = 0.0, s.c3 = 0.0;
		} else if (kind == KIND_MOON) {
			Vector rv, vv;
			if (!maneuver::Kepler (r0, v0, ref.mu, (dep - mjd0) * DAY, rv, vv)) return false;
			if (!Lambert (rv, p2, tofs, ref.mu, nref, false, s.v1, s.v2)) return false;
			s.r1 = rv, s.r2 = p2;
			s.vinfDepVec = s.v1 - vv;
			s.dvEject = s.vinfDepVec.length();
			s.vinfDep = 0.0, s.c3 = 0.0;
		} else return false;
		s.vinfArrVec = s.v2 - w2;
		s.vinfArr = s.vinfArrVec.length();
		s.dvCapture = CaptureDv (s.vinfArr);
		s.dvTotal = s.dvEject + (objective == OBJ_TOTAL ? s.dvCapture : 0.0);
		s.mu = central.mu;
		s.dep = dep, s.tof = tof;
		s.valid = std::isfinite (s.dvTotal);
		return s.valid;
	}

	// the ejection burn on the parking orbit for a transfer
	bool Ejection (Solution &s, double earliest) const
	{
		s.burnValid = false;
		if (!s.valid || landed) return false; // (landed: see the launch opportunities)
		if (kind == KIND_MOON) {
			Vector rv, vv;
			if (!maneuver::Kepler (r0, v0, ref.mu, (s.dep - mjd0) * DAY, rv, vv)) return false;
			Vector dvg (s.v1 - vv);
			s.burnMjd = s.dep;
			s.burnDv = maneuver::FromGlobal (dvg, rv, vv);
			s.burnPos = rv;
			s.burnDvMag = dvg.length();
			s.decl = asin (std::clamp (fabs (dotp (dvg.unit(), crossp (rv, vv).unit())), 0.0, 1.0));
			s.burnValid = s.dep > earliest;
			return s.burnValid;
		}
		// on the parking orbit, within half a period of the departure date:
		// the point needing the least velocity change for the departure asymptote
		double r = r0.length(), v2 = dotp (v0, v0);
		double a = 1.0 / (2.0 / r - v2 / ref.mu);
		double P = (a > 0.0) ? Pi2 * sqrt (a*a*a / ref.mu) / DAY : 0.1; // [days]
		double t0 = std::max (earliest, s.dep - 0.5 * P);
		auto Cost = [&](double t, Vector *dvg, Vector *rr, Vector *vv) {
			Vector rb, vb;
			if (!maneuver::Kepler (r0, v0, ref.mu, (t - mjd0) * DAY, rb, vb)) return 1e30;
			Vector d (DepartureVelocity (rb, s.vinfDepVec, ref.mu) - vb);
			if (dvg) *dvg = d, *rr = rb, *vv = vb;
			return d.length();
		};
		const int N = 120;
		double bt = t0, bc = 1e30;
		for (int i = 0; i < N; i++) {
			double t = t0 + P * i / N, c = Cost (t, nullptr, nullptr, nullptr);
			if (c < bc) bc = c, bt = t;
		}
		// golden section about the best sample
		double lo = std::max (t0, bt - P / N), hi = bt + P / N;
		const double g = 0.6180339887498949;
		double x1 = hi - g * (hi - lo), x2 = lo + g * (hi - lo);
		double f1 = Cost (x1, nullptr, nullptr, nullptr), f2 = Cost (x2, nullptr, nullptr, nullptr);
		for (int it = 0; it < 40; it++) {
			if (f1 < f2) { hi = x2; x2 = x1; f2 = f1; x1 = hi - g * (hi - lo); f1 = Cost (x1, nullptr, nullptr, nullptr); }
			else         { lo = x1; x1 = x2; f1 = f2; x2 = lo + g * (hi - lo); f2 = Cost (x2, nullptr, nullptr, nullptr); }
		}
		double tb = 0.5 * (lo + hi);
		Vector dvg, rb, vb;
		if (Cost (tb, &dvg, &rb, &vb) >= 1e29) return false;
		s.burnMjd = tb;
		s.burnDv = maneuver::FromGlobal (dvg, rb, vb);
		s.burnPos = rb;
		s.burnDvMag = dvg.length();
		s.decl = asin (std::clamp (fabs (dotp (s.vinfDepVec.unit(), crossp (r0, v0).unit())), 0.0, 1.0));
		s.burnValid = true;
		return true;
	}
};

namespace {

	BodySnap Snap (const CelestialBody *cb)
	{
		BodySnap b;
		b.body = cb;
		b.mass = cb->Mass();
		b.mu = Ggrav * b.mass;
		b.radius = cb->Size();
		const CelestialBody *p = cb->Primary();
		if (p) {
			b.parentMass = p->Mass();
			b.moonOfPlanet = p->Primary() != nullptr;
			double d = (cb->GPos() - p->GPos()).length();
			b.soi = d * pow (b.mass / p->Mass(), 0.4);
			Vector h (crossp (cb->GPos() - p->GPos(), cb->GVel() - p->GVel()));
			b.pole = cb->RotAxis();
			if (dotp (b.pole, h) < 0.0) b.pole = -b.pole;
		} else {
			b.pole = cb->RotAxis();
		}
		if (!cb->HasEphemerisModule() && cb->ElementsPtr())
			b.els = std::make_shared<Elements> (*cb->ElementsPtr());
		return b;
	}

	double Period (double a, double mu) { return Pi2 * sqrt (a*a*a / mu); }

	// 2D Nelder-Mead minimisation of f(x, y)
	template<class F> void Minimise (F f, double &x, double &y, double sx, double sy, int iter = 120)
	{
		double px[3] = { x, x + sx, x }, py[3] = { y, y, y + sy }, fv[3];
		for (int i = 0; i < 3; i++) fv[i] = f (px[i], py[i]);
		for (int it = 0; it < iter; it++) {
			int lo = 0, hi = 0;
			for (int i = 1; i < 3; i++) { if (fv[i] < fv[lo]) lo = i; if (fv[i] > fv[hi]) hi = i; }
			int mid = 3 - lo - hi;
			double cx = 0.5 * (px[lo] + px[mid]), cy = 0.5 * (py[lo] + py[mid]);
			double rx = 2*cx - px[hi], ry = 2*cy - py[hi], fr = f (rx, ry);
			if (fr < fv[lo]) {
				double ex = 3*cx - 2*px[hi], ey = 3*cy - 2*py[hi], fe = f (ex, ey);
				if (fe < fr) px[hi] = ex, py[hi] = ey, fv[hi] = fe;
				else px[hi] = rx, py[hi] = ry, fv[hi] = fr;
			} else if (fr < fv[mid]) {
				px[hi] = rx, py[hi] = ry, fv[hi] = fr;
			} else {
				double kx = 0.5 * (cx + px[hi]), ky = 0.5 * (cy + py[hi]), fk = f (kx, ky);
				if (fk < fv[hi]) px[hi] = kx, py[hi] = ky, fv[hi] = fk;
				else for (int i = 0; i < 3; i++) if (i != lo) {
					px[i] = 0.5 * (px[i] + px[lo]), py[i] = 0.5 * (py[i] + py[lo]);
					fv[i] = f (px[i], py[i]);
				}
			}
		}
		int lo = 0;
		for (int i = 1; i < 3; i++) if (fv[i] < fv[lo]) lo = i;
		x = px[lo], y = py[lo];
	}

	// ----------------------------------------------------------------------
	// Arrival of a predicted trajectory at the target

	struct Arrival {
		bool soi = false;          // enters the target's sphere of influence
		bool closest = false;      // has a closest approach to the target
		double res[2] = { 0, 0 };  // residual [m]: B-plane (soi) or miss vector
		Vector B, Bt;              // B vector, target
		Vector S;                  // incoming asymptote
		double vinf = 0.0;
		double entryMjd = 0.0;
		double periMjd = 0.0, periAlt = 0.0;
		double periSpeed = 0.0;    // at the periapsis (from the trajectory near it)
		bool peri = false;
		double incl = 0.0, inclMin = 0.0;
		double miss = 1e30;
		double bt = 0, br = 0, btT = 0, brT = 0;
	};

	// enc: the encounter of a route (-1: the target); hd: the angular momentum
	// direction its flyby is to have (null: the target's arrival setting)
	Arrival Arrive (const trajectory::Prediction &p, const Bodies &b, const Settings &set, double afterSimT, bool wantSoi, double rpAim,
		int enc = -1, const Vector *hd = nullptr)
	{
		Arrival a;
		const BodySnap &tb = b.Enc (enc);
		const CelestialBody *tgt = tb.body;
		double mu = tb.mu;
		const Vector &k = tb.pole;
		// sphere of influence entry
		const trajectory::Event *entry = nullptr;
		for (const auto &e : p.events)
			if (e.type == trajectory::Event::SOI_ENTER && e.ref == tgt && e.t > afterSimT) { entry = &e; break; }
		if (entry && wantSoi && entry->seg >= 0 && entry->seg < (int)p.segs.size() && !p.segs[entry->seg].pts.empty()) {
			// the hyperbola where the body dominates: well inside its sphere of
			// influence (at its edge the Sun pulls as hard; Jupiter's is 0.3 AU)
			// (interpolated at that distance: the B vector must not jump from one
			// point of the path to the next as the burn changes)
			const auto &sp = p.segs[entry->seg].pts;
			double rEval = std::max (0.15 * tb.soi, 3.0 * rpAim);
			trajectory::Point pt = sp.front();
			for (size_t i = 1; i < sp.size(); i++) {
				const trajectory::Point &q0 = sp[i-1], &q1 = sp[i];
				if (dotp (q0.r, q0.v) > 0.0) break; // (past the periapsis)
				double r0 = q0.r.length(), r1 = q1.r.length();
				if (r0 > rEval && r1 <= rEval) {
					double hh = q1.t - q0.t, s = (r0 - rEval) / (r0 - r1), s2 = s*s, s3 = s2*s;
					for (int it = 0; it < 3; it++) { // (the radius along the cubic: a few secant steps)
						Vector r (q0.r * (2*s3 - 3*s2 + 1) + q0.v * ((s3 - 2*s2 + s) * hh) + q1.r * (-2*s3 + 3*s2) + q1.v * ((s3 - s2) * hh));
						double rs = r.length();
						s = std::clamp (s + (rs - rEval) / std::max (r0 - r1, 1.0), 0.0, 1.0), s2 = s*s, s3 = s2*s;
					}
					pt.t = q0.t + s * hh;
					pt.r = q0.r * (2*s3 - 3*s2 + 1) + q0.v * ((s3 - 2*s2 + s) * hh) + q1.r * (-2*s3 + 3*s2) + q1.v * ((s3 - s2) * hh);
					pt.v = (q0.r * (6*s2 - 6*s) + q1.r * (-6*s2 + 6*s)) / hh + q0.v * (3*s2 - 4*s + 1) + q1.v * (3*s2 - 2*s);
					break;
				}
			}
			a.B = BVector (pt.r, pt.v, mu, a.S, a.vinf);
			a.entryMjd = b.mjdRef + pt.t / DAY;
			Vector h (crossp (pt.r, pt.v));
			a.incl = acos (std::clamp (dotp (h.unit(), k), -1.0, 1.0));
			double rp = rpAim;
			double Bm = rp * sqrt (1.0 + 2.0 * mu / (rp * a.vinf * a.vinf));
			a.inclMin = asin (std::clamp (fabs (dotp (a.S, k)), 0.0, 1.0));
			Vector Bd;
			if (hd) { // (a flyby: the plane that turns the excess velocity as planned)
				Vector h (*hd - a.S * dotp (*hd, a.S));
				if (h.length() < 1e-6) h = crossp (a.S, Vector (0, 1, 0));
				Bd = crossp (a.S, h.unit()).unit();
			} else if (set.arrival == ARR_FREE && a.B.length() > 1.0) Bd = a.B.unit();
			else {
				Vector hd (k - a.S * dotp (k, a.S));
				if (hd.length() < 1e-6) hd = crossp (a.S, Vector (0, 1, 0));
				Bd = crossp (a.S, hd.unit()).unit();
			}
			a.Bt = Bd * Bm;
			Vector T (crossp (a.S, k));
			if (T.length() < 1e-6) T = crossp (a.S, Vector (1, 0, 0));
			T.unify();
			Vector R (crossp (a.S, T).unit());
			a.bt = dotp (a.B, T), a.br = dotp (a.B, R);
			a.btT = dotp (a.Bt, T), a.brT = dotp (a.Bt, R);
			a.res[0] = a.bt - a.btT, a.res[1] = a.br - a.brT;
			a.soi = true;
			for (const auto &e : p.events)
				if (e.type == trajectory::Event::PERIAPSIS && e.ref == tgt && e.t >= entry->t) {
					a.peri = true, a.periMjd = b.mjdRef + e.t / DAY, a.periAlt = e.value;
					// speed at the periapsis: the energy of the nearest point carried to the
					// periapsis radius (the other bodies' pull changes it along the way in)
					double rp = tb.radius + e.value;
					if (e.seg >= 0 && e.seg < (int)p.segs.size()) {
						const trajectory::Point *q = nullptr;
						for (const auto &pt : p.segs[e.seg].pts)
							if (!q || fabs (pt.t - e.t) < fabs (q->t - e.t)) q = &pt;
						if (q) a.periSpeed = sqrt (std::max (0.0, dotp (q->v, q->v) + 2.0 * mu * (1.0 / rp - 1.0 / q->r.length())));
					}
					if (a.periSpeed <= 0.0) a.periSpeed = sqrt (a.vinf * a.vinf + 2.0 * mu / rp);
					break;
				}
			if (!a.peri) // (impact)
				for (const auto &e : p.events)
					if (e.type == trajectory::Event::IMPACT && e.ref == tgt) { a.periMjd = b.mjdRef + e.t / DAY, a.periAlt = 0.0; break; }
			a.miss = a.peri ? tb.radius + a.periAlt : 0.0;
			return a;
		}
		// otherwise the closest approach: the miss vector across the relative velocity
		const trajectory::Event *best = nullptr;
		for (const auto &e : p.events)
			if (e.type == trajectory::Event::CLOSEST && e.t > afterSimT && (!best || e.value < best->value)) best = &e;
		if (!best || best->seg < 0 || best->seg >= (int)p.segs.size()) return a;
		const auto &seg = p.segs[best->seg];
		// vessel state at the event (nearest point of its segment, advanced linearly)
		const trajectory::Point *q = nullptr;
		for (const auto &pt : seg.pts) if (!q || fabs (pt.t - best->t) < fabs (q->t - best->t)) q = &pt;
		if (!q) return a;
		Vector rv (q->r + q->v * (best->t - q->t)), vv (q->v);
		double mjd = b.mjdRef + best->t / DAY;
		Vector rt, vt;
		if (seg.ref == tgt) rt = Vector (), vt = Vector ();
		else if (seg.ref == tgt->Primary()) { if (!b.EncState (enc, mjd, rt, vt)) return a; }
		else return a;
		Vector m (rv - rt), w (vv - vt);
		Vector T (crossp (w.unit(), k));
		if (T.length() < 1e-6) T = crossp (w.unit(), Vector (1, 0, 0));
		T.unify();
		Vector R (crossp (w.unit(), T).unit());
		a.res[0] = dotp (m, T), a.res[1] = dotp (m, R);
		a.miss = m.length();
		a.closest = true;
		a.entryMjd = mjd;
		return a;
	}
}

// =======================================================================
// Planner

namespace { // (routes: below)
	route::Problem RouteProblem (const Bodies &b, const Settings &set, const std::vector<int> &seq, double now, double span);
	Route ToRoute (const Bodies &b, const std::vector<int> &seq, const route::Problem &p, const route::Result &r);
	Solution RouteSolution (const Bodies &b, const route::Result &r);
	void Avoid (const Bodies &b, route::Problem &p);
}

Planner::Planner (Vessel *v): vessel(v) {}

Planner::~Planner ()
{
	Abort ();
}

void Planner::Abort ()
{
	abort = true;
	Join ();
	abort = false;
}

void Planner::Join ()
{
	if (worker.joinable()) worker.join();
	busy = false;
}

void Planner::Start (Status::Job job, std::function<void()> work)
{
	Abort ();
	{
		std::lock_guard<std::mutex> lock (mtx);
		status.job = job;
		status.progress = 0.0f;
		status.message.clear ();
	}
	busy = true;
	worker = std::thread ([this, work]() {
		work ();
		std::lock_guard<std::mutex> lock (mtx);
		if (!status.message.empty() && !abort)
			oapiWriteLogV ("Transfer planner (%s): %s", vessel->Name(), status.message.c_str());
		if (!planPending) { // (else when the simulation thread has applied the plan)
			status.job = Status::IDLE;
			status.serial++;
		}
		busy = false;
	});
}

std::vector<const CelestialBody*> Planner::Targets () const
{
	std::vector<const CelestialBody*> t;
	const CelestialBody *ref = vessel->ElRef();
	if (!ref) return t;
	if (const CelestialBody *p = ref->Primary())
		for (DWORD i = 0; i < p->nSecondary(); i++)
			if (p->Secondary(i) != ref) t.push_back (p->Secondary(i));
	for (DWORD i = 0; i < ref->nSecondary(); i++)
		t.push_back (ref->Secondary(i));
	return t;
}

Kind Planner::KindOf (const CelestialBody *target) const
{
	const CelestialBody *ref = vessel->ElRef();
	if (!ref || !target || target == ref) return KIND_NONE;
	if (target->Primary() == ref) return KIND_MOON;
	if (ref->Primary() && target->Primary() == ref->Primary()) return KIND_SIBLING;
	return KIND_NONE;
}

double Planner::DefaultParkAlt (const CelestialBody *body)
{
	if (!body) return 2e5;
	// with an atmosphere: 4 percent of the radius, 150 to 600 km (Earth: 255 km);
	// without: 5 percent, 20 to 1000 km (the atmospheres' altitude limits are
	// set for drag, far out: Earth 3000 km)
	if (const Planet *p = dynamic_cast<const Planet*> (body))
		if (p->HasAtmosphere ()) return std::clamp (0.04 * body->Size (), 1.5e5, 6e5);
	return std::clamp (0.05 * body->Size (), 2e4, 1e6);
}

double Planner::DefaultCaptureAlt (const CelestialBody *target)
{
	if (!target) return 0.0;
	return std::clamp (0.06 * target->Size(), 50e3, 2e7);
}

std::shared_ptr<Bodies> Planner::Snapshot (const CelestialBody *target) const
{
	Kind kind = KindOf (target);
	if (kind == KIND_NONE) return nullptr;
	auto b = std::make_shared<Bodies>();
	const CelestialBody *ref = vessel->ElRef();
	b->kind = kind;
	b->ref = Snap (ref);
	b->target = Snap (target);
	b->tabRef.b = &b->ref, b->tabTgt.b = &b->target; // (no tables yet: direct ephemeris)
	b->central = (kind == KIND_SIBLING) ? Snap (ref->Primary()) : b->ref;
	b->mjdRef = td.MJD_ref;
	b->mjd0 = td.MJD1;
	b->r0 = vessel->GPos() - ref->GPos();
	b->v0 = vessel->GVel() - ref->GVel();
	b->landed = vessel->GetStatus () == FLIGHTSTATUS_LANDED || vessel->GroundContact ();
	b->parkR = ref->Size () + (settings.parkAlt > 0.0 ? settings.parkAlt : DefaultParkAlt (ref));
	b->ascentTime = settings.ascentTime, b->minCoast = settings.minCoast;
	if (b->landed) {
		Matrix R;
		ref->GetRotation (td.SimT1, R);
		b->padLocal = tmul (R, b->r0);
	}
	b->objective = settings.objective;
	b->capAlt = settings.capAlt > 0.0 ? settings.capAlt : DefaultCaptureAlt (target);
	b->capApo = settings.capApo;
	if (kind == KIND_SIBLING) // (prograde about the parent: the departure body's sense)
		b->nref = crossp (ref->GPos() - ref->Primary()->GPos(), ref->GVel() - ref->Primary()->GVel()).unit();
	else if (!b->landed)
		b->nref = crossp (b->r0, b->v0).unit();
	else // (moon from the surface: the sense of the moon's orbit)
		b->nref = crossp (target->GPos() - ref->GPos(), target->GVel() - ref->GVel()).unit();
	if (settings.UsesRoute ()) {
		// the flyby bodies: with the target's parent
		for (size_t i = 0; i < settings.flybys.size(); i++) {
			const CelestialBody *f = settings.flybys[i];
			if (!f || f->Primary () != b->central.body) return nullptr;
			b->fly.push_back (Snap (f));
			double alt = settings.FlybyAlt (i);
			b->flyMinAlt.push_back (alt > 0.0 ? alt : DefaultFlybyAlt (f));
		}
		b->tabFly.resize (b->fly.size ());
		for (size_t i = 0; i < b->fly.size(); i++) b->tabFly[i].b = &b->fly[i];
		// (the bodies large enough to bend a trajectory: as FlybyBodies, with the target)
		const CelestialBody *c = b->central.body;
		for (DWORD i = 0; i < c->nSecondary (); i++) {
			const CelestialBody *o = c->Secondary (i);
			if (o->Size () > 0.0 && sqrt (2.0 * Ggrav * o->Mass () / o->Size ()) >= 500.0) b->other.push_back (Snap (o));
		}
		b->tabOther.resize (b->other.size ());
		for (size_t i = 0; i < b->other.size(); i++) b->tabOther[i].b = &b->other[i];
	}
	return b;
}

bool Planner::Search ()
{
	auto b = Snapshot (settings.target);
	if (!b) {
		std::lock_guard<std::mutex> lock (mtx);
		status.message = settings.UsesRoute () ? "No route from here: the flybys must orbit the target's parent" : "No transfer from here to this target";
		return false;
	}
	if (settings.UsesRoute ()) {
		if (b->landed && b->kind != KIND_SIBLING) {
			std::lock_guard<std::mutex> lock (mtx);
			status.message = "Routes to moons start in orbit: launch first";
			return false;
		}
		Settings set = settings;
		std::vector<double> seed;
		seed.swap (routeSeed);
		Start (Status::SEARCH, [this, b, set, seed]() { DoRoute (b, set, seed); });
		return true;
	}
	Start (Status::SEARCH, [this, b]() { DoSearch (b, -1.0); });
	return true;
}

bool Planner::SearchAround (double dep)
{
	if (settings.UsesRoute ()) return Search ();
	auto b = Snapshot (settings.target);
	if (!b) return false;
	Start (Status::SEARCH, [this, b, dep]() { DoSearch (b, dep); });
	return true;
}

void Planner::DoSearch (std::shared_ptr<Bodies> b, double around)
{
	auto Progress = [&](float f) { std::lock_guard<std::mutex> lock (mtx); status.progress = f; };
	const double now = b->mjd0 + 600.0 / DAY; // (departures from 10 min ahead)

	// characteristic times: synodic period, Hohmann time of flight, periods
	Vector p, v;
	double muC = b->central.mu;
	double a1, a2, H, S, Pv = 0.0;
	if (!b->Raw (b->target, now, p, v)) { std::lock_guard<std::mutex> lock (mtx); status.message = "No ephemeris of the target"; return; }
	a2 = 1.0 / (2.0 / p.length() - dotp (v, v) / (b->kind == KIND_SIBLING ? muC : b->ref.mu));
	if (b->kind == KIND_SIBLING) {
		Vector pr, vr;
		if (!b->Raw (b->ref, now, pr, vr)) return;
		a1 = 1.0 / (2.0 / pr.length() - dotp (vr, vr) / muC);
	} else if (b->landed) {
		a1 = b->parkR;
		Pv = Period (a1, b->ref.mu) / DAY;
		muC = b->ref.mu;
	} else {
		double r = b->r0.length();
		a1 = 1.0 / (2.0 / r - dotp (b->v0, b->v0) / b->ref.mu);
		Pv = Period (std::max (a1, 1.0), b->ref.mu) / DAY;
		muC = b->ref.mu;
	}
	if (a1 <= 0.0 || a2 <= 0.0) { std::lock_guard<std::mutex> lock (mtx); status.message = "Not in orbit"; return; }
	double T1 = Period (a1, muC) / DAY, T2 = Period (a2, muC) / DAY;
	S = 1.0 / std::max (fabs (1.0 / T1 - 1.0 / T2), 1e-6);
	H = 0.5 * Period (0.5 * (a1 + a2), muC) / DAY;
	double tofLo, tofHi;
	if (b->kind == KIND_SIBLING) tofLo = 0.35 * H, tofHi = 1.8 * H;
	else tofLo = 0.45 * H, tofHi = 2.2 * H;

	// windows over the next years (siblings) or the target's period (moons)
	double span;
	if (b->kind == KIND_SIBLING) span = std::clamp (2.5 * S, 1.2 * S, 15.0 * 365.25);
	else span = std::max (T2, 2.0 * Pv);
	span = std::min (span, 15.0 * 365.25);
	double tabEnd = now + span + tofHi + 2.0;
	b->MakeTable (b->tabTgt, b->target, now - 1.0, tabEnd, std::clamp (T2 / 2000.0, 0.01, 1.0));
	if (b->kind == KIND_SIBLING) b->MakeTable (b->tabRef, b->ref, now - 1.0, tabEnd, std::clamp (T1 / 2000.0, 0.01, 1.0));
	if (abort) return;

	auto Total = [&](double dep, double tof) -> double {
		Solution s;
		return b->Evaluate (dep, tof, s) ? s.dvTotal : 1e30;
	};
	auto curveOut = std::make_shared<Curve>();
	double center = around;
	if (around < 0.0) {
		const int N = 360, NT = 36;
		int nphase = (b->kind == KIND_MOON && !b->landed) ? 8 : 1;
		curveOut->dep0 = now, curveOut->dep1 = now + span;
		curveOut->dv.assign (N, NaNf), curveOut->tof.assign (N, NaNf);
		for (int i = 0; i < N && !abort; i++) {
			double dep = now + span * i / (N - 1);
			double bestDv = 1e30, bestTof = 0.0;
			for (int ph = 0; ph < nphase; ph++) {
				double d = dep + (nphase > 1 ? Pv * ph / nphase : 0.0);
				for (int j = 0; j < NT; j++) {
					double tof = tofLo + (tofHi - tofLo) * j / (NT - 1);
					double c = Total (d, tof);
					if (c < bestDv) bestDv = c, bestTof = tof;
				}
			}
			if (bestDv < 1e29) curveOut->dv[i] = (float)bestDv, curveOut->tof[i] = (float)bestTof;
			if (i % 8 == 0) Progress (0.5f * i / N);
		}
		// windows: local minima, best first
		std::vector<std::pair<float, double>> mins;
		for (int i = 0; i < N; i++) {
			float c = curveOut->dv[i];
			if (!std::isfinite (c)) continue;
			bool lo = true;
			for (int k = std::max (0, i - 6); k <= std::min (N - 1, i + 6); k++)
				if (k != i && std::isfinite (curveOut->dv[k]) && curveOut->dv[k] < c) { lo = false; break; }
			if (lo) mins.push_back ({ c, now + span * i / (N - 1) });
		}
		std::sort (mins.begin(), mins.end());
		for (auto &m : mins) curveOut->windows.push_back (m.second);
		// the porkchop around the best window within the first synodic period
		// (moons: the target's period)
		double first = (b->kind == KIND_SIBLING) ? S : span;
		center = -1.0;
		for (auto &m : mins) if (m.second <= now + first) { center = m.second; break; }
		if (center < 0.0) center = mins.empty() ? now : mins.front().second;
		std::lock_guard<std::mutex> lock (mtx);
		curve = curveOut;
	}
	if (abort) return;

	// porkchop
	auto g = std::make_shared<Grid>();
	double half = (b->kind == KIND_SIBLING) ? 0.15 * S : b->landed ? 0.25 * T2 : std::max (Pv, 0.05);
	g->dep0 = std::max (now, center - half), g->dep1 = g->dep0 + 2.0 * half;
	g->tof0 = tofLo, g->tof1 = tofHi;
	g->nx = (b->kind == KIND_SIBLING) ? 120 : 160, g->ny = 90;
	size_t n = (size_t)g->nx * g->ny;
	g->total.assign (n, NaNf), g->eject.assign (n, NaNf), g->capture.assign (n, NaNf);
	double bestC = 1e30, bx = center, by = H;
	for (int j = 0; j < g->ny && !abort; j++) {
		double tof = g->tof0 + (g->tof1 - g->tof0) * j / (g->ny - 1);
		for (int i = 0; i < g->nx; i++) {
			double dep = g->dep0 + (g->dep1 - g->dep0) * i / (g->nx - 1);
			Solution s;
			if (!b->Evaluate (dep, tof, s)) continue;
			size_t k = (size_t)j * g->nx + i;
			g->total[k] = (float)s.dvTotal, g->eject[k] = (float)s.dvEject, g->capture[k] = (float)s.dvCapture;
			if (s.dvTotal < bestC) bestC = s.dvTotal, bx = dep, by = tof;
		}
		if (j % 6 == 0) Progress (0.5f + 0.45f * j / g->ny);
	}
	if (abort) return;
	g->zmin = (float)bestC;

	// the optimum
	double dx = (g->dep1 - g->dep0) / (g->nx - 1), dy = (g->tof1 - g->tof0) / (g->ny - 1);
	Minimise ([&](double x, double y) {
		if (x < now || y < 0.2 * tofLo) return 1e30;
		return Total (x, y);
	}, bx, by, dx, dy);
	Solution opt;
	b->Evaluate (bx, by, opt);
	b->Ejection (opt, b->mjd0 + 120.0 / DAY);
	LaunchPlan lp;
	Launches (*b, opt, lp);

	std::lock_guard<std::mutex> lock (mtx);
	grid = g;
	best = opt;
	selected = opt;
	route = Route ();
	curveX.clear ();
	launch = lp;
	bodies = b;
	status.progress = 1.0f;
	char buf[160];
	snprintf (buf, sizeof (buf), "Optimum: %.0f m/s (ejection %.0f, capture %.0f)", opt.dvTotal, opt.dvEject, opt.dvCapture);
	status.message = buf;
}

void Planner::Select (double dep, double tof)
{
	std::lock_guard<std::mutex> lock (mtx);
	if (!bodies) return;
	if (route.valid) {
		// the route's departure date: the curve's nearest sample
		if (dep < 0.0 || !curve || curveX.empty()) return;
		const Curve &c = *curve;
		int n = (int)curveX.size(), bi = -1;
		for (int i = 0; i < n; i++) {
			if (curveX[i].empty()) continue;
			double d = c.dep0 + (c.dep1 - c.dep0) * i / std::max (n - 1, 1);
			if (bi < 0 || fabs (d - dep) < fabs (c.dep0 + (c.dep1 - c.dep0) * bi / std::max (n - 1, 1) - dep)) bi = i;
		}
		if (bi < 0) return;
		std::vector<int> seq;
		for (int i = 0; i < (int)bodies->fly.size(); i++) seq.push_back (i);
		Settings set = settings;
		set.dsm = route.dsm;
		route::Problem p = RouteProblem (*bodies, set, seq, bodies->mjd0 + 600.0 / DAY, c.dep1 - bodies->mjd0 - 600.0 / DAY);
		p.t0lo = p.t0hi = curveX[bi][0];
		route::Result r;
		Avoid (*bodies, p);
		if (route::Evaluate (p, curveX[bi].data(), &r) >= 1e29 || !r.valid) return;
		route::CheckPasses (p, r);
		route = ToRoute (*bodies, seq, p, r);
		Solution s = RouteSolution (*bodies, r);
		bodies->Ejection (s, td.MJD1 + 120.0 / DAY);
		selected = s;
		Launches (*bodies, s, launch);
		status.serial++;
		return;
	}
	if (dep < 0.0) { selected = best; return; }
	Solution s;
	if (bodies->Evaluate (dep, tof, s)) {
		bodies->Ejection (s, td.MJD1 + 120.0 / DAY);
		selected = s;
		Launches (*bodies, s, launch);
		status.serial++;
	}
}

void Planner::UpdateLaunch ()
{
	std::lock_guard<std::mutex> lock (mtx);
	if (!bodies || !bodies->landed || busy) return;
	// the new launch settings, the search's transfer
	bodies->parkR = bodies->ref.radius + (settings.parkAlt > 0.0 ? settings.parkAlt : DefaultParkAlt (bodies->ref.body));
	bodies->ascentTime = settings.ascentTime, bodies->minCoast = settings.minCoast;
	Solution s;
	if (!route.valid && selected.valid && bodies->Evaluate (selected.dep, selected.tof, s)) selected = s;
	Launches (*bodies, selected, launch);
	status.serial++;
}

void Planner::SelectLaunch (int i)
{
	std::lock_guard<std::mutex> lock (mtx);
	if (i >= 0 && i < (int)launch.opp.size()) launch.sel = i, status.serial++;
}

LaunchPlan Planner::Launch () const { std::lock_guard<std::mutex> lock (mtx); return launch; }

// -----------------------------------------------------------------------
// Launch opportunities: the parking orbit must contain the direction the
// transfer leaves in (the departure asymptote, or the moon at arrival). As
// the site turns with the body, the plane through that direction and the
// site turns too: the opportunities are the times when launching into it
// costs least (the site passing under the least inclined such plane).

void Planner::Launches (const Bodies &b, const Solution &s, LaunchPlan &lp)
{
	lp = LaunchPlan ();
	if (!b.landed || !s.valid) return;
	lp.landed = true;
	const double mu = b.ref.mu, rp = b.parkR, vc = sqrt (mu / rp);
	const double now = b.mjd0;
	lp.parkAlt = rp - b.ref.radius;
	Vector pl (b.padLocal.unit());
	lp.lat = asin (std::clamp (pl.y, -1.0, 1.0)), lp.lng = atan2 (pl.z, pl.x);
	// the body's spin axis (its frame's y axis), in the sense the site moves
	Vector pa (b.Pad (now)), pb (b.Pad (now + 60.0 / DAY));
	Matrix R0;
	b.ref.body->GetRotation ((now - b.mjdRef) * DAY, R0);
	Vector w (mul (R0, Vector (0, 1, 0)).unit());
	if (dotp (crossp (w, pa), pb - pa) < 0.0) w = -w;
	double omega = Pi2 / b.ref.body->RotT ();
	double vrot = omega * crossp (w, pa).length ();
	lp.vAscent = vc - vrot;
	Vector d ((b.kind == KIND_SIBLING ? s.vinfDepVec : s.r2).unit());

	auto Geo = [&](double t, Vector &p, Vector &n, Vector &v, Vector &e) {
		p = b.Pad (t).unit();
		n = crossp (d, p);
		if (n.length() < 1e-9) n = crossp (w, p);
		n.unify();
		if (dotp (n, w) < 0.0) n = -n; // (prograde)
		v = crossp (n, p).unit();      // direction of flight at the site
		e = crossp (w, p).unit();      // east
		return (v * vc - e * vrot).length() - (vc - vrot);
	};
	double t0 = std::max (now + 120.0 / DAY, s.dep - 1.25), t1 = s.dep + 0.25;
	if (t1 <= t0) return;
	const double step = 60.0 / DAY;
	int n = (int)((t1 - t0) / step) + 1;
	std::vector<double> pen (n);
	Vector p, nn, v, e;
	for (int i = 0; i < n; i++) pen[i] = Geo (t0 + i * step, p, nn, v, e);
	for (int i = 1; i + 1 < n; i++) {
		if (!(pen[i] <= pen[i-1] && pen[i] < pen[i+1])) continue;
		double lo = t0 + (i - 1) * step, hi = t0 + (i + 1) * step;
		for (int it = 0; it < 30; it++) {
			double m1 = lo + (hi - lo) / 3.0, m2 = hi - (hi - lo) / 3.0;
			if (Geo (m1, p, nn, v, e) < Geo (m2, p, nn, v, e)) hi = m2; else lo = m1;
		}
		LaunchOpportunity o;
		o.launchMjd = 0.5 * (lo + hi);
		o.dvPenalty = Geo (o.launchMjd, p, nn, v, e);
		Vector north (w - p * dotp (w, p));
		north.unify();
		o.azimuth = posangle (atan2 (dotp (v, e), dotp (v, north)));
		o.incl = acos (std::clamp (dotp (nn, w), -1.0, 1.0));
		{
			Matrix R;
			b.ref.body->GetRotation ((o.launchMjd - b.mjdRef) * DAY, R);
			Vector node (tmul (R, crossp (w, nn)));
			o.lan = posangle (atan2 (node.z, node.x));
		}
		// insertion after the ascent, downrange along the plane (at half the orbital speed on average)
		double th = 0.5 * vc * b.ascentTime / rp;
		o.insMjd = o.launchMjd + b.ascentTime / DAY;
		o.rIns = (p * cos (th) + v * sin (th)) * rp;
		o.vIns = (v * cos (th) - p * sin (th)) * vc;
		// the ejection from that orbit, keeping the arrival
		Bodies c (b);
		c.landed = false;
		c.r0 = o.rIns, c.v0 = o.vIns, c.mjd0 = o.insMjd;
		c.nref = (b.kind == KIND_SIBLING) ? b.nref : crossp (o.rIns, o.vIns).unit();
		double earliest = o.insMjd + b.minCoast / DAY;
		double arr = s.dep + s.tof;
		double P = Period (rp, mu) / DAY;
		Solution q;
		if (b.kind == KIND_SIBLING) {
			double dep = std::max (s.dep, earliest + 0.5 * P);
			if (!c.Evaluate (dep, arr - dep, q) || !c.Ejection (q, earliest)) continue;
		} else {
			double bt = earliest, bc = 1e30;
			for (int k = 0; k < 90; k++) {
				double tb = earliest + P * k / 90.0;
				Solution x;
				if (c.Evaluate (tb, arr - tb, x) && x.dvEject < bc) bc = x.dvEject, bt = tb;
			}
			double lo2 = bt - P / 90.0, hi2 = bt + P / 90.0;
			for (int it = 0; it < 30; it++) {
				double m1 = lo2 + (hi2 - lo2) / 3.0, m2 = hi2 - (hi2 - lo2) / 3.0;
				Solution x1, x2;
				double c1 = c.Evaluate (m1, arr - m1, x1) ? x1.dvEject : 1e30, c2 = c.Evaluate (m2, arr - m2, x2) ? x2.dvEject : 1e30;
				if (c1 < c2) hi2 = m2; else lo2 = m1;
			}
			double tb = std::max (earliest, 0.5 * (lo2 + hi2));
			if (!c.Evaluate (tb, arr - tb, q) || !c.Ejection (q, earliest - 1e-6)) continue;
		}
		o.burnMjd = q.burnMjd;
		o.burnDv = q.burnDv;
		o.burnPos = q.burnPos;
		o.coast = (q.burnMjd - o.insMjd) * DAY;
		o.dvEject = q.burnDvMag;
		o.dvCapture = q.dvCapture;
		o.tof = arr - q.burnMjd;
		o.dvTotal = o.dvPenalty + o.dvEject + (b.objective == OBJ_TOTAL ? o.dvCapture : 0.0);
		lp.opp.push_back (o);
	}
	// the cheapest by default
	for (size_t i = 0; i < lp.opp.size(); i++)
		if (lp.sel < 0 || lp.opp[i].dvTotal < lp.opp[lp.sel].dvTotal) lp.sel = (int)i;
}

// -----------------------------------------------------------------------
// Numerical refinement: Newton iterations on the velocity change of one
// node, the residual being the B-plane miss at the target (or the miss
// vector of the closest approach while the trajectory doesn't reach the
// target's sphere of influence)

namespace {
	struct Shooter {
		trajectory::Sandbox *sb;
		const Bodies *b;
		const Settings *set;
		const std::atomic<bool> *abort;
		double days;
		double afterSimT;
		int enc = -1;               // route: the encounter targeted (-1: the target)
		const Vector *hd = nullptr; // route flyby: the angular momentum direction wanted
		double rpWant = 0.0;        // periapsis radius wanted (0: the capture periapsis)
		double tFix = 0.0;          // [MJD] outside the sphere of influence: aim at the body's centre at this time (0: closest approach)

		// the vessel relative to the encounter body at the fixed time
		bool MissAt (const trajectory::Prediction &p, Vector &m) const
		{
			double tf = (tFix - b->mjdRef) * DAY;
			const CelestialBody *body = b->Enc (enc).body;
			for (const auto &seg : p.segs) {
				if (seg.pts.size() < 2 || tf < seg.pts.front().t || tf > seg.pts.back().t) continue;
				size_t i = 1;
				while (i + 1 < seg.pts.size() && seg.pts[i].t < tf) i++;
				const trajectory::Point &p0 = seg.pts[i-1], &p1 = seg.pts[i];
				double hh = p1.t - p0.t, s = (hh > 0.0) ? (tf - p0.t) / hh : 0.0, s2 = s*s, s3 = s2*s;
				Vector r (p0.r * (2*s3 - 3*s2 + 1) + p0.v * ((s3 - 2*s2 + s) * hh) + p1.r * (-2*s3 + 3*s2) + p1.v * ((s3 - s2) * hh));
				if (seg.ref == body) { m = r; return true; }
				if (seg.ref != body->Primary ()) return false;
				Vector pb, vb;
				if (!b->EncState (enc, tFix, pb, vb)) return false;
				m = r - pb;
				return true;
			}
			return false;
		}

		bool Run (std::vector<maneuver::Node> &nodes, int k, Arrival &a, std::function<void(float)> progress, std::string &msg)
		{
			const bool moon = b->kind == KIND_MOON;
			const double h = moon ? 0.01 : 0.02;  // [m/s] finite differences
			const double rpT = rpWant > 0.0 ? rpWant : b->CapPeri ();
			const double R = b->Enc (enc).radius;
			const double tol = std::max (1000.0, 2e-4 * rpT);
			const double tolAlt = std::max (1000.0, 0.001 * rpT);
			double rpAim = rpT;
			int outer = 0;
			for (int it = 0; it < 40; it++) {
				if (*abort) return false;
				auto p = sb->Predict (nodes, days, abort);
				if (!p) return false;
				a = Arrive (*p, *b, *set, afterSimT, true, rpAim, enc, hd);
				Vector mf;
				if (!a.soi && !a.closest && !(tFix > 0.0 && MissAt (*p, mf))) {
					msg = "The trajectory doesn't approach the target";
					return false;
				}
				double rn = hypot (a.res[0], a.res[1]);
				if (getenv ("ORBITER_TRANSFER_DEBUG"))
					oapiWriteLogV ("Shooter enc %d it %d: soi %d closest %d res %.0f km, dv (%.2f %.2f %.2f), peri %.0f km, %zu events; B (%.0f %.0f) target (%.0f %.0f) km, vinf %.0f",
						enc, it, a.soi, a.closest, rn * 1e-3, nodes[k].dv.x, nodes[k].dv.y, nodes[k].dv.z, a.periAlt * 1e-3, p->events.size (),
						a.bt * 1e-3, a.br * 1e-3, a.btT * 1e-3, a.brT * 1e-3, a.vinf);
				if (a.soi && rn < tol) {
					// (the two-body B-plane misses the periapsis by the other bodies' pull
					// inside the sphere of influence: aim again by the difference)
					double rpAct = R + a.periAlt;
					if (!a.peri || fabs (rpAct - rpT) < tolAlt || ++outer > 6) return true;
					rpAim = std::max (0.5 * R, rpAim + (rpT - rpAct));
					continue;
				}
				progress (std::min (0.95f, 0.1f + 0.04f * it));
				Vector m0;
				if (!a.soi && tFix > 0.0 && MissAt (*p, m0)) {
					// far out: Newton on the position at the encounter time (well posed,
					// where the closest approach jumps between passes)
					// (larger differences than near the body: the integration's noise
					// over a long arc is not small against a few cm/s)
					const double hf = moon ? 0.1 : 0.5;
					double J[3][3];
					bool okJ = true;
					for (int c = 0; c < 3 && okJ; c++) {
						std::vector<maneuver::Node> pn (nodes);
						double comp[3] = { pn[k].dv.x, pn[k].dv.y, pn[k].dv.z };
						comp[c] += hf;
						pn[k].dv = Vector (comp[0], comp[1], comp[2]);
						auto pp = sb->Predict (pn, days, abort);
						if (!pp) return false;
						Vector m1;
						if (!MissAt (*pp, m1)) { okJ = false; break; }
						Vector d ((m1 - m0) / hf);
						J[0][c] = d.x, J[1][c] = d.y, J[2][c] = d.z;
					}
					if (okJ) {
						double det = J[0][0]*(J[1][1]*J[2][2] - J[1][2]*J[2][1]) - J[0][1]*(J[1][0]*J[2][2] - J[1][2]*J[2][0]) + J[0][2]*(J[1][0]*J[2][1] - J[1][1]*J[2][0]);
						if (fabs (det) > 1e-30) {
							double rhs[3] = { -m0.x, -m0.y, -m0.z }, x[3];
							for (int c = 0; c < 3; c++) { // (Cramer)
								double M[3][3];
								for (int r = 0; r < 3; r++) for (int q = 0; q < 3; q++) M[r][q] = (q == c) ? rhs[r] : J[r][q];
								x[c] = (M[0][0]*(M[1][1]*M[2][2] - M[1][2]*M[2][1]) - M[0][1]*(M[1][0]*M[2][2] - M[1][2]*M[2][0]) + M[0][2]*(M[1][0]*M[2][1] - M[1][1]*M[2][0])) / det;
							}
							Vector step (x[0], x[1], x[2]);
							double smax = moon ? 20.0 : 40.0;
							if (step.length() > smax) step *= smax / step.length();
							// backtracking: other bodies on the way (a pass near a planet
							// between the encounters) make the miss far from linear
							Vector base (nodes[k].dv);
							double best = m0.length(), bf = 0.0;
							for (double f = 1.0; f > 0.04; f *= 0.5) {
								std::vector<maneuver::Node> pn (nodes);
								pn[k].dv = base + step * f;
								auto pp = sb->Predict (pn, days, abort);
								if (!pp) return false;
								Vector m1;
								if (MissAt (*pp, m1) && m1.length() < best) { best = m1.length(), bf = f; break; }
							}
							if (getenv ("ORBITER_TRANSFER_DEBUG")) oapiWriteLogV ("  fixed-time miss %.0f km -> %.0f km (step %.2f m/s x %.2f)", m0.length() * 1e-3, best * 1e-3, step.length(), bf);
							if (bf == 0.0) { msg = "Targeting failed: no correction reduces the miss"; return false; }
							nodes[k].dv = base + step * bf;
							continue;
						}
					}
				}
				bool soi = a.soi;
				double J[2][3];
				for (int c = 0; c < 3; c++) {
					std::vector<maneuver::Node> pn (nodes);
					double comp[3] = { pn[k].dv.x, pn[k].dv.y, pn[k].dv.z };
					comp[c] += h;
					pn[k].dv = Vector (comp[0], comp[1], comp[2]);
					auto pp = sb->Predict (pn, days, abort);
					if (!pp) return false;
					Arrival ap = Arrive (*pp, *b, *set, afterSimT, soi, rpAim, enc, hd);
					if (ap.soi != soi || (!ap.soi && !ap.closest)) { msg = "Unstable targeting"; return false; }
					J[0][c] = (ap.res[0] - a.res[0]) / h;
					J[1][c] = (ap.res[1] - a.res[1]) / h;
				}
				// least-norm step: dv = J^T (J J^T)^-1 (-res)
				double A = J[0][0]*J[0][0] + J[0][1]*J[0][1] + J[0][2]*J[0][2];
				double B = J[0][0]*J[1][0] + J[0][1]*J[1][1] + J[0][2]*J[1][2];
				double C = J[1][0]*J[1][0] + J[1][1]*J[1][1] + J[1][2]*J[1][2];
				double det = A*C - B*B;
				if (fabs (det) < 1e-30) { msg = "Targeting failed (singular)"; return false; }
				double y0 = (-C * a.res[0] + B * a.res[1]) / det, y1 = (B * a.res[0] - A * a.res[1]) / det;
				Vector step (J[0][0]*y0 + J[1][0]*y1, J[0][1]*y0 + J[1][1]*y1, J[0][2]*y0 + J[1][2]*y1);
				double smax = soi ? (moon ? 5.0 : 10.0) : (moon ? 20.0 : 40.0);
				if (step.length() > smax) step *= smax / step.length();
				// backtracking: other bodies near the encounter (a planet's moons)
				// make the miss far from linear
				Vector base (nodes[k].dv);
				double bf = 0.0;
				for (double f = 1.0; f > 0.02; f *= 0.5) {
					std::vector<maneuver::Node> pn (nodes);
					pn[k].dv = base + step * f;
					auto pp = sb->Predict (pn, days, abort);
					if (!pp) return false;
					Arrival at = Arrive (*pp, *b, *set, afterSimT, soi, rpAim, enc, hd);
					if ((at.soi && !soi) || ((at.soi || at.closest) && at.soi == soi && hypot (at.res[0], at.res[1]) < rn)) { bf = f; break; }
				}
				if (bf == 0.0) {
					if (getenv ("ORBITER_TRANSFER_DEBUG")) oapiWriteLogV ("  no reduction along the step (%.2f %.2f %.2f), J (%.3g %.3g %.3g / %.3g %.3g %.3g) km per m/s",
						step.x, step.y, step.z, J[0][0] * 1e-3, J[0][1] * 1e-3, J[0][2] * 1e-3, J[1][0] * 1e-3, J[1][1] * 1e-3, J[1][2] * 1e-3);
					msg = "Targeting did not converge";
					return false; // (a: the best so far; the caller checks a.soi)
				}
				nodes[k].dv = base + step * bf;
			}
			msg = "Targeting did not converge";
			return false;
		}
	};
}

bool Planner::MakeNodes (bool arm)
{
	auto b = Snapshot (settings.target);
	if (!b) return false;
	Solution sol = Selected ();
	if (!sol.valid) return false;
	if (b->landed) {
		std::lock_guard<std::mutex> lock (mtx);
		status.message = "On the surface: launch first (Launch page), then make the nodes in orbit";
		return false;
	}
	Route rt = GetRoute ();
	if (rt.valid) {
		// the route searched must be the one set up
		std::vector<const CelestialBody*> want (settings.UsesRoute () ? settings.flybys : std::vector<const CelestialBody*> ());
		want.push_back (settings.target);
		if (want != rt.bodies) {
			std::lock_guard<std::mutex> lock (mtx);
			status.message = "The route has changed: search again";
			return false;
		}
		if (!b->Ejection (sol, b->mjd0 + 300.0 / DAY)) {
			std::lock_guard<std::mutex> lock (mtx);
			status.message = "The departure is past: search again";
			return false;
		}
		double days = (rt.dep + rt.tof - b->mjd0) * 1.15 + 8.0;
		auto sb = std::make_shared<trajectory::Sandbox> (vessel, rt.bodies[0], days);
		if (!sb->Valid ()) return false;
		Settings set = settings;
		Start (Status::REFINE, [this, b, sb, rt, sol, set, arm]() { DoRefineRoute (b, sb, rt, sol, set, arm); });
		return true;
	}
	// the ejection again from the current state (the vessel has moved on)
	{
		b->Evaluate (sol.dep, sol.tof, sol);
		if (!b->Ejection (sol, b->mjd0 + 300.0 / DAY)) {
			std::lock_guard<std::mutex> lock (mtx);
			status.message = "The departure is past: search again";
			return false;
		}
	}
	double days = (sol.dep + sol.tof - b->mjd0) * 1.3 + 3.0;
	auto sb = std::make_shared<trajectory::Sandbox> (vessel, settings.target, days);
	if (!sb->Valid ()) return false;
	Settings set = settings;
	Start (Status::REFINE, [this, b, sb, sol, set, arm]() { DoRefine (b, sb, sol, set, arm); });
	return true;
}

void Planner::DoRefine (std::shared_ptr<Bodies> b, std::shared_ptr<trajectory::Sandbox> sb, Solution sol, Settings set, bool arm)
{
	auto Progress = [&](float f) { std::lock_guard<std::mutex> lock (mtx); status.progress = f; };
	std::vector<maneuver::Node> nodes (1);
	nodes[0].mjd = sol.burnMjd;
	nodes[0].dv = sol.burnDv;
	nodes[0].autoburn = arm;
	sb->Advance (sol.burnMjd - 60.0 / DAY, &abort); // (the coast in the parking orbit once)
	double days = (sol.dep + sol.tof - b->mjd0) * 1.3 + 3.0;
	Shooter sh{ sb.get(), b.get(), &set, &abort, days, (sol.burnMjd - b->mjdRef) * DAY };
	Arrival a;
	std::string msg;
	bool ok = sh.Run (nodes, 0, a, Progress, msg);
	if (abort) return;

	Plan pl;
	if (!a.soi) {
		std::lock_guard<std::mutex> lock (mtx);
		status.message = msg.empty() ? "Targeting failed" : msg;
		return;
	}
	pl.valid = true;
	pl.nodes.push_back ({ ROLE_EJECTION, nodes[0].mjd, nodes[0].dv, true, true });
	double tEnd = a.peri ? a.periMjd : a.entryMjd;
	for (int m = 0; m < 2; m++)
		if (set.useMcc[m])
			pl.nodes.push_back ({ ROLE_MCC, nodes[0].mjd + set.mcc[m] * (tEnd - nodes[0].mjd), Vector (), false, false });
	if (set.objective == OBJ_TOTAL && a.peri) {
		double rp = b->target.radius + a.periAlt;
		double ra = std::max (rp, std::min (b->CapApo (), 0.9 * b->target.soi));
		double vp = a.periSpeed;
		double vc = sqrt (b->target.mu * (2.0 / rp - 2.0 / (rp + ra)));
		pl.nodes.push_back ({ ROLE_CAPTURE, a.periMjd, Vector (-(vp - vc), 0.0, 0.0), true, true });
	}
	pl.arrMjd = a.periMjd;
	pl.periAlt = a.periAlt;
	pl.periTarget = b->capAlt;
	pl.incl = a.incl;
	pl.inclMin = a.inclMin;
	pl.bt = a.bt, pl.br = a.br, pl.btT = a.btT, pl.brT = a.brT;
	pl.arrives = true;
	pl.armed = arm;

	std::lock_guard<std::mutex> lock (mtx);
	pendingPlan = pl;
	planPending = true;
	pendingCorrection = false;
	status.progress = 1.0f;
	char buf[160];
	snprintf (buf, sizeof (buf), "%s: periapsis %.0f km (target %.0f km)", ok ? "Targeted" : "Partly targeted",
		a.periAlt * 1e-3, b->capAlt * 1e-3);
	status.message = buf;
}

bool Planner::Correct (bool ejection)
{
	if (busy) return false;
	Plan pl = GetPlan ();
	if (!pl.valid || !settings.target) return false;
	auto b = Snapshot (settings.target);
	if (!b) return false;
	double now = td.MJD1;
	if (!pl.route.empty ()) {
		// a route: the nodes before the encounter the correction aims at
		if ((int)b->fly.size () + 1 != (int)pl.route.size ()) return false;
		int idx = -1;
		for (size_t i = 0; i < pl.nodes.size(); i++) {
			const PlanNode &pn = pl.nodes[i];
			if (ejection ? pn.role == ROLE_EJECTION
				: ((pn.role == ROLE_MCC || pn.role == ROLE_DSM) && !pn.done && pn.mjd > now + 60.0 / DAY)) { idx = (int)i; break; }
		}
		if (ejection && idx < 0) return false;
		double mjd = (idx >= 0) ? pl.nodes[idx].mjd : now + 600.0 / DAY;
		int aim = (idx >= 0) ? pl.nodes[idx].aim : -1;
		if (aim < 0) {
			aim = (int)pl.route.size () - 1;
			for (size_t i = 0; i < pl.flybys.size(); i++) if (pl.flybys[i].mjd > mjd) { aim = (int)i; break; }
		}
		double tAim = (aim < (int)pl.flybys.size()) ? pl.flybys[aim].mjd : pl.arrMjd;
		std::vector<maneuver::Node> nodes;
		int refine = -1;
		for (const auto &n : vessel->ManeuverNodes ()) {
			if (n.mjd < now || n.mjd >= tAim - 0.01) continue;
			if (n.mjd == mjd) refine = (int)nodes.size ();
			nodes.push_back (n);
		}
		auto sb = std::make_shared<trajectory::Sandbox> (vessel, pl.route[aim], (tAim - now) * 1.2 + 3.0);
		if (!sb->Valid ()) return false;
		Settings set = settings;
		bool arm = pl.armed;
		Start (Status::CORRECT, [this, b, sb, nodes, refine, mjd, idx, set, arm]() { DoCorrectRoute (b, sb, nodes, refine, mjd, idx, set, arm); });
		return true;
	}
	// the vessel's nodes, without the capture node (placed again afterwards)
	std::vector<maneuver::Node> nodes;
	double capMjd = -1.0, ejMjd = -1.0;
	for (const auto &pn : pl.nodes) {
		if (pn.role == ROLE_CAPTURE) capMjd = pn.mjd;
		if (pn.role == ROLE_EJECTION) ejMjd = pn.mjd;
	}
	for (const auto &n : vessel->ManeuverNodes ()) {
		if (n.mjd == capMjd) continue;
		if (n.mjd >= now) nodes.push_back (n);
	}
	int idx = -1, refine = -1;
	double mjd = 0.0;
	if (ejection) {
		// the ejection again
		for (size_t i = 0; i < nodes.size(); i++) if (nodes[i].mjd == ejMjd) refine = (int)i;
		for (size_t i = 0; i < pl.nodes.size(); i++) if (pl.nodes[i].role == ROLE_EJECTION) idx = (int)i;
		if (refine < 0 || idx < 0) return false;
		mjd = ejMjd;
	} else {
		// the next correction not computed yet (or a new one now)
		for (size_t i = 0; i < pl.nodes.size(); i++)
			if (pl.nodes[i].role == ROLE_MCC && !pl.nodes[i].done && pl.nodes[i].mjd > now + 60.0 / DAY) { idx = (int)i; break; }
		mjd = (idx >= 0) ? pl.nodes[idx].mjd : now + 600.0 / DAY;
	}
	int cap = refine;
	double days = (pl.arrMjd - now) * 1.2 + 3.0;
	auto sb = std::make_shared<trajectory::Sandbox> (vessel, settings.target, days);
	if (!sb->Valid ()) return false;
	Settings set = settings;
	bool arm = pl.armed;
	Start (Status::CORRECT, [this, b, sb, nodes, cap, mjd, idx, set, arm]() { DoCorrect (b, sb, nodes, cap, mjd, idx, set, arm); });
	return true;
}

void Planner::DoCorrect (std::shared_ptr<Bodies> b, std::shared_ptr<trajectory::Sandbox> sb, std::vector<maneuver::Node> nodes,
	int refineIndex, double mccMjd, int planIndex, Settings set, bool arm)
{
	auto Progress = [&](float f) { std::lock_guard<std::mutex> lock (mtx); status.progress = f; };
	int k = refineIndex;
	if (k < 0) { // a new correction node
		maneuver::Node mcc;
		mcc.mjd = mccMjd;
		mcc.dv = Vector ();
		mcc.autoburn = arm;
		auto it = std::upper_bound (nodes.begin(), nodes.end(), mcc, [](const maneuver::Node &x, const maneuver::Node &y) { return x.mjd < y.mjd; });
		k = (int)(it - nodes.begin());
		nodes.insert (it, mcc);
	}
	Plan pl = GetPlan ();
	double days = (pl.arrMjd - b->mjd0) * 1.2 + 3.0;
	Shooter sh{ sb.get(), b.get(), &set, &abort, days, (mccMjd - b->mjdRef) * DAY };
	Arrival a;
	std::string msg;
	bool ok = sh.Run (nodes, k, a, Progress, msg);
	if (abort) return;
	if (!a.soi) {
		std::lock_guard<std::mutex> lock (mtx);
		status.message = msg.empty() ? "Correction failed" : msg;
		return;
	}
	// the plan with the correction, and the capture at the new periapsis
	for (auto &pn : pl.nodes) if (pn.role == ROLE_CAPTURE) {
		double rp = b->target.radius + a.periAlt;
		double ra = std::max (rp, std::min (b->CapApo (), 0.9 * b->target.soi));
		double vp = a.periSpeed;
		double vc = sqrt (b->target.mu * (2.0 / rp - 2.0 / (rp + ra)));
		pn.mjd = a.periMjd;
		pn.dv = Vector (-(vp - vc), 0.0, 0.0);
	}
	PlanNode corr{ ROLE_MCC, mccMjd, nodes[k].dv, nodes[k].dv.length() >= 0.05, true };
	if (refineIndex >= 0) corr = PlanNode{ ROLE_EJECTION, mccMjd, nodes[k].dv, true, true }, pl.ejRetargeted = true;
	if (planIndex >= 0 && planIndex < (int)pl.nodes.size()) pl.nodes[planIndex] = corr;
	else {
		auto pit = std::upper_bound (pl.nodes.begin(), pl.nodes.end(), corr, [](const PlanNode &x, const PlanNode &y) { return x.mjd < y.mjd; });
		planIndex = (int)(pit - pl.nodes.begin());
		pl.nodes.insert (pit, corr);
	}
	pl.arrMjd = a.periMjd;
	pl.periAlt = a.periAlt;
	pl.incl = a.incl;
	pl.inclMin = a.inclMin;
	pl.bt = a.bt, pl.br = a.br, pl.btT = a.btT, pl.brT = a.brT;

	std::lock_guard<std::mutex> lock (mtx);
	pendingPlan = pl;
	planPending = true;
	pendingCorrection = true;
	pendingPlanIndex = planIndex;
	status.progress = 1.0f;
	char buf[160];
	snprintf (buf, sizeof (buf), "%s: %.2f m/s, periapsis %.0f km", refineIndex >= 0 ? "Ejection targeted again" : ok ? "Correction" : "Partial correction",
		nodes[k].dv.length(), a.periAlt * 1e-3);
	status.message = buf;
}

// =======================================================================
// Routes with flybys: the patched-conic optimum (Routes.h), the best
// velocity change by departure date near it, suggestions

double Planner::DefaultFlybyAlt (const CelestialBody *body)
{
	return DefaultParkAlt (body);
}

std::vector<const CelestialBody*> Planner::FlybyBodies () const
{
	std::vector<const CelestialBody*> f;
	const CelestialBody *ref = vessel->ElRef ();
	Kind kind = KindOf (settings.target);
	const CelestialBody *central = (kind == KIND_SIBLING) ? ref->Primary () : (kind == KIND_MOON) ? ref : nullptr;
	if (!central) return f;
	for (DWORD i = 0; i < central->nSecondary (); i++) {
		const CelestialBody *c = central->Secondary (i);
		if (c == settings.target) continue;
		// (too small to turn a trajectory: escape speed below 500 m/s)
		if (c->Size () <= 0.0 || sqrt (2.0 * Ggrav * c->Mass () / c->Size ()) < 500.0) continue;
		f.push_back (c);
	}
	return f;
}

namespace {
	double SemiMajor (const Vector &p, const Vector &v, double mu) { return 1.0 / (2.0 / p.length() - dotp (v, v) / mu); }

	// semi-major axis of the departure (the departure body's or the vessel's orbit about the central body)
	double DepartureA (const Bodies &b, double now)
	{
		Vector p, v;
		if (b.kind == KIND_SIBLING) return b.RefState (now, p, v) ? SemiMajor (p, v, b.central.mu) : 0.0;
		return b.landed ? b.parkR : SemiMajor (b.r0, b.v0, b.ref.mu);
	}

	// default window of departures: the synodic period of the departure and
	// the first body, half again (vessels about the central body: the bodies' periods)
	double RouteSpan (const Bodies &b, const Settings &set, const std::vector<int> &seq, double now)
	{
		if (set.routeSpan > 0.0) return set.routeSpan;
		double mu = b.central.mu, a0 = DepartureA (b, now);
		Vector p, v;
		const BodySnap &first = seq.empty() ? b.target : b.fly[seq[0]];
		if (!b.Raw (first, now, p, v) || a0 <= 0.0) return 365.0;
		double a1 = SemiMajor (p, v, mu);
		double T0 = Period (a0, mu) / DAY, T1 = Period (std::max (a1, 1.0), mu) / DAY;
		if (b.kind == KIND_SIBLING) {
			double S = 1.0 / std::max (fabs (1.0 / T0 - 1.0 / T1), 1e-6);
			return std::clamp (1.5 * S, 200.0, 1500.0);
		}
		double Tmax = std::max (T0, T1);
		for (int i : seq) if (b.Raw (b.fly[i], now, p, v)) Tmax = std::max (Tmax, Period (std::max (SemiMajor (p, v, mu), 1.0), mu) / DAY);
		return 1.5 * Tmax;
	}

	// the patched-conic problem of a route: the flybys seq (indices into b.fly), then the target
	route::Problem RouteProblem (const Bodies &b, const Settings &set, const std::vector<int> &seq, double now, double span)
	{
		route::Problem p;
		p.mu = b.central.mu;
		p.nref = b.nref;
		const Bodies *bp = &b;
		if (b.kind == KIND_SIBLING) {
			p.dep = route::DEP_PARK;
			p.depBody.mu = b.ref.mu, p.depBody.radius = b.ref.radius;
			p.depBody.state = [bp](double mjd, Vector &r, Vector &v) { return bp->RefState (mjd, r, v); };
			p.parkR = b.ParkRadius ();
		} else {
			p.dep = route::DEP_ORBIT;
			p.r0 = b.r0, p.v0 = b.v0, p.mjd0 = b.mjd0;
		}
		std::vector<int> enc (seq);
		enc.push_back (-1); // (the target)
		double aPrev = DepartureA (b, now);
		const CelestialBody *prev = (b.kind == KIND_SIBLING) ? b.ref.body : nullptr;
		for (size_t i = 0; i < enc.size(); i++) {
			const BodySnap &s = (enc[i] >= 0) ? b.fly[enc[i]] : b.target;
			route::Body e;
			e.mu = s.mu, e.radius = s.radius;
			e.rpMin = s.radius + (enc[i] >= 0 ? b.flyMinAlt[enc[i]] : 0.0);
			int k = enc[i];
			e.state = [bp, k](double mjd, Vector &r, Vector &v) { return bp->EncState (k, mjd, r, v); };
			p.enc.push_back (e);
			// the leg's time: about the Hohmann time between the orbits (returns to
			// the same body: its period), with room for the full revolutions
			Vector q, w;
			double a = b.EncState (k, now, q, w) ? SemiMajor (q, w, p.mu) : aPrev; // (from the tables: threads)
			if (!(a > 0.0)) a = aPrev;
			double H = 0.5 * Period (0.5 * (std::max (aPrev, 1.0) + std::max (a, 1.0)), p.mu) / DAY;
			bool same = s.body == prev;
			double lo = (same ? 0.6 : 0.3) * H;
			double hi = (same ? 2.2 : 1.8) * H * (1.0 + set.maxRevs);
			if (set.maxTof > 0.0) hi = std::min (hi, set.maxTof), lo = std::min (lo, 0.5 * hi);
			p.tlo.push_back (lo), p.thi.push_back (hi);
			aPrev = a, prev = s.body;
		}
		if (b.objective == OBJ_TOTAL) {
			p.arr = route::ARR_CAPTURE;
			p.capRp = b.CapPeri ();
			p.capRa = std::max (p.capRp, std::min (b.CapApo (), 0.9 * b.target.soi));
		} else p.arr = route::ARR_NONE;
		p.t0lo = now, p.t0hi = now + span;
		p.maxTof = set.maxTof;
		p.maxRevs = std::clamp (set.maxRevs, 0, 3);
		p.dsm = set.dsm;
		double aDep = DepartureA (b, now);
		p.vinfMax = (aDep > 0.0) ? 0.3 * sqrt (p.mu / aDep) : 6000.0;
		return p;
	}

	// the longest a route can take (for the ephemeris tables)
	double RouteLength (const route::Problem &p)
	{
		double t = 0.0;
		for (double h : p.thi) t += h;
		return (p.maxTof > 0.0) ? std::min (t, p.maxTof) : t;
	}

	Route ToRoute (const Bodies &b, const std::vector<int> &seq, const route::Problem &p, const route::Result &r)
	{
		Route rt;
		if (!r.valid) return rt;
		rt.valid = true;
		rt.dsm = p.dsm;
		for (int i : seq) rt.bodies.push_back (b.fly[i].body);
		rt.bodies.push_back (b.target.body);
		rt.dep = r.dep, rt.tof = r.tof;
		rt.dvTotal = r.dv;
		rt.dvEject = r.dvDep, rt.dvFlyby = r.dvFlyby, rt.dvDsm = r.dvDsm;
		rt.vinfArr = r.enc.back().vinfIn.length();
		rt.dvCapture = b.CaptureDv (rt.vinfArr);
		rt.vinfDep = (b.kind == KIND_SIBLING) ? r.vinfDep.length() : 0.0;
		for (size_t k = 0; k + 1 < r.enc.size(); k++) {
			const route::Encounter &E = r.enc[k];
			RouteFlyby f;
			f.body = b.fly[seq[k]].body;
			f.mjd = E.mjd;
			f.alt = E.rp - b.fly[seq[k]].radius;
			f.minAlt = b.flyMinAlt[seq[k]];
			f.turn = E.turn;
			f.vinfIn = E.vinfIn.length(), f.vinfOut = E.vinfOut.length();
			f.dv = E.dvPeri, f.dvTurn = E.dvTurn;
			f.vinfInVec = E.vinfIn, f.vinfOutVec = E.vinfOut;
			rt.flybys.push_back (f);
		}
		for (const auto &L : r.legs) {
			RouteLeg l;
			if (L.nearBody >= 0 && L.nearDist < 1.0 && L.nearBody < (int)b.other.size()) {
				l.near = b.other[L.nearBody].body;
				l.nearDist = L.nearDist * 3.0 * b.other[L.nearBody].soi;
				l.nearMjd = L.nearMjd;
			}
			l.t0 = L.t0, l.t1 = L.t1, l.revs = L.revs;
			l.r0 = L.r0, l.v0 = L.v0;
			l.dsmMjd = L.dsmMjd;
			l.dsmDvVec = L.dsmDv, l.dsmDv = L.dsmDv.length(), l.dsmPos = L.dsmR;
			rt.legs.push_back (l);
		}
		rt.x = r.x;
		return rt;
	}

	// the route's departure and arrival as a direct transfer's solution (the
	// ejection, launch opportunities and the Burn page use it)
	Solution RouteSolution (const Bodies &b, const route::Result &r)
	{
		Solution s;
		if (!r.valid || r.legs.empty()) return s;
		s.dep = r.dep, s.tof = r.tof;
		s.r1 = r.legs[0].r0, s.v1 = r.legs[0].v0;
		s.r2 = r.legs.back().r1, s.v2 = r.legs.back().v1;
		s.mu = b.central.mu;
		s.vinfDepVec = r.vinfDep;
		if (b.kind == KIND_SIBLING) s.vinfDep = r.vinfDep.length(), s.c3 = s.vinfDep * s.vinfDep;
		s.dvEject = r.dvDep;
		s.vinfArrVec = r.enc.back().vinfIn;
		s.vinfArr = s.vinfArrVec.length();
		s.dvCapture = b.CaptureDv (s.vinfArr);
		s.dvTotal = r.dv;
		s.valid = true;
		return s;
	}

	// the ephemeris tables of the bodies of routes from now for this long
	void RouteTables (Bodies &b, double now, double days, const std::vector<bool> *use = nullptr)
	{
		double t1 = now + days + 2.0;
		auto Step = [&](const BodySnap &s) {
			Vector p, v;
			if (!b.Raw (s, now, p, v)) return 1.0;
			double a = SemiMajor (p, v, b.central.mu);
			return std::clamp ((a > 0.0 ? Period (a, b.central.mu) / DAY : 365.0) / 2000.0, 0.01, 1.0);
		};
		b.MakeTable (b.tabTgt, b.target, now - 1.0, t1, Step (b.target));
		if (b.kind == KIND_SIBLING) b.MakeTable (b.tabRef, b.ref, now - 1.0, t1, Step (b.ref));
		for (size_t i = 0; i < b.fly.size(); i++)
			if (!use || (*use)[i]) b.MakeTable (b.tabFly[i], b.fly[i], now - 1.0, t1, Step (b.fly[i]));
		for (size_t i = 0; i < b.other.size(); i++) b.MakeTable (b.tabOther[i], b.other[i], now - 1.0, t1, Step (b.other[i]));
	}

	// the bodies a route's legs keep clear of (three times their sphere of influence:
	// the corrections after flybys must not bring them closer)
	void Avoid (const Bodies &b, route::Problem &p)
	{
		p.avoid.clear ();
		const Bodies *bp = &b;
		for (size_t i = 0; i < b.other.size(); i++) {
			route::Body o;
			o.mu = b.other[i].mu;
			o.radius = 3.0 * std::min (b.other[i].soi, 1e12);
			o.state = [bp, i](double mjd, Vector &r, Vector &v) { return bp->State (bp->tabOther[i], mjd, r, v); };
			p.avoid.push_back (o);
		}
	}

	// the route's velocity change by departure date: the optimum carried to
	// each date (its departure fixed, the rest polished)
	void RouteCurve (const route::Problem &p, const route::Result &r, Curve &c, std::vector<std::vector<double>> &xs,
		const std::atomic<bool> &abort, std::function<void(float)> progress)
	{
		const int N = 90;
		c.dep0 = p.t0lo, c.dep1 = p.t0hi;
		c.dv.assign (N, NaNf), c.tof.assign (N, NaNf);
		xs.assign (N, std::vector<double> ());
		if (!r.valid || c.dep1 <= c.dep0) return;
		int i0 = std::clamp ((int)floor ((r.dep - c.dep0) / (c.dep1 - c.dep0) * (N - 1) + 0.5), 0, N - 1);
		int done = 0;
		for (int dir = 1; dir >= -1; dir -= 2) {
			std::vector<double> x (r.x);
			for (int i = (dir > 0 ? i0 : i0 - 1); i >= 0 && i < N && !abort; i += dir) {
				double d = c.dep0 + (c.dep1 - c.dep0) * i / (N - 1);
				route::Problem q (p);
				q.t0lo = q.t0hi = d;
				x[0] = d;
				double f = route::Polish (q, x, p.dsm ? 2500 : 1200, &abort);
				if (f < 1e29) {
					route::Result rr;
					route::Evaluate (q, x.data(), &rr);
					c.dv[i] = (float)f, c.tof[i] = (float)rr.tof;
					xs[i] = x;
				} else x = r.x;
				if (++done % 6 == 0) progress ((float)done / N);
			}
		}
		std::vector<std::pair<float, double>> mins;
		for (int i = 0; i < N; i++) {
			if (!std::isfinite (c.dv[i])) continue;
			bool lo = true;
			for (int k = std::max (0, i - 4); k <= std::min (N - 1, i + 4); k++)
				if (k != i && std::isfinite (c.dv[k]) && c.dv[k] < c.dv[i]) { lo = false; break; }
			if (lo) mins.push_back ({ c.dv[i], c.dep0 + (c.dep1 - c.dep0) * i / (N - 1) });
		}
		std::sort (mins.begin(), mins.end());
		for (auto &m : mins) c.windows.push_back (m.second);
	}
}

bool Planner::RouteMode () const { return route.valid; }

void Planner::DoRoute (std::shared_ptr<Bodies> b, Settings set, std::vector<double> seed)
{
	auto Progress = [&](float f) { std::lock_guard<std::mutex> lock (mtx); status.progress = f; };
	auto Fail = [&](const char *msg) { std::lock_guard<std::mutex> lock (mtx); status.message = msg; };
	const double now = b->mjd0 + 600.0 / DAY;
	std::vector<int> seq;
	for (int i = 0; i < (int)b->fly.size(); i++) seq.push_back (i);
	double span = RouteSpan (*b, set, seq, now);
	route::Problem p = RouteProblem (*b, set, seq, now, span);
	RouteTables (*b, now, span + RouteLength (p));
	if (abort) return;

	// the optimum: independent searches on the threads, the best kept (one
	// search can end in a broad basin next to the best one); deep-space
	// manoeuvres: seeded with the powered-flyby optimum
	const int legs = p.Legs ();
	const int nt = std::clamp ((int)std::thread::hardware_concurrency () - 1, 1, 6);
	b->strict = true; // (the threads use the tables only)
	auto Multi = [&](const route::Problem &q, long evals, const std::vector<std::vector<double>> &seeds, float p0, float p1) {
		std::vector<route::Result> res (nt);
		std::vector<std::thread> th;
		for (int t = 0; t < nt; t++)
			th.emplace_back ([&, t]() {
				route::Options o;
				o.seed = 1 + t;
				o.abort = &abort;
				o.evals = evals;
				o.seeds = seeds;
				if (t == 0) o.progress = [&](float f) { Progress (p0 + (p1 - p0) * f); };
				res[t] = route::Optimise (q, o);
			});
		for (auto &x : th) x.join ();
		route::Result best;
		for (auto &x : res) if (x.valid && (!best.valid || x.dv < best.dv)) best = x;
		return best;
	};
	std::vector<std::vector<double>> seeds, none;
	route::Result r;
	if (p.dsm) {
		route::Problem q (p);
		q.dsm = false;
		if ((int)seed.size () == route::Dimension (p)) seeds.push_back (seed);
		route::Result m = Multi (q, 30000 + 20000 * legs, (int)seed.size () == route::Dimension (q) ? std::vector<std::vector<double>> (1, seed) : none, 0.0f, 0.2f);
		std::vector<double> x;
		if (m.valid && route::ToDsm (p, m, x)) seeds.push_back (x);
		r = Multi (p, 100000 + 60000 * legs, seeds, 0.2f, 0.7f);
	} else {
		if ((int)seed.size () == route::Dimension (p)) seeds.push_back (seed);
		r = Multi (p, 40000 + 40000 * legs, seeds, 0.0f, 0.7f);
	}
	if (abort) return;
	if (!r.valid) { b->strict = false; Fail ("No route found"); return; }
	// a leg passing close to another body (the patched conics don't see it, and
	// the flight can't be targeted through it): again, keeping clear of them
	route::Problem pa (p);
	Avoid (*b, pa);
	double pass = route::CheckPasses (pa, r);
	if (getenv ("ORBITER_TRANSFER_DEBUG"))
		for (const auto &L : r.legs) oapiWriteLogV ("Route leg: closest pass %s at %.2f of the distance to keep, MJD %.1f",
			L.nearBody >= 0 ? b->other[L.nearBody].body->Name () : "-", L.nearDist, L.nearMjd);
	if (pass < 1.0) {
		route::Result ra = Multi (pa, p.dsm ? 100000 + 60000 * legs : 40000 + 40000 * legs, std::vector<std::vector<double>> (1, r.x), 0.7f, 0.8f);
		if (abort) return;
		if (ra.valid) {
			route::CheckPasses (pa, ra);
			r = ra, p = pa;
		}
	}

	auto c = std::make_shared<Curve> ();
	std::vector<std::vector<double>> xs;
	RouteCurve (p, r, *c, xs, abort, [&](float f) { Progress (0.8f + 0.18f * f); });
	if (abort) return;

	b->strict = false;
	Route rt = ToRoute (*b, seq, p, r);
	Solution sol = RouteSolution (*b, r);
	b->Ejection (sol, b->mjd0 + 120.0 / DAY);
	LaunchPlan lp;
	Launches (*b, sol, lp);

	std::lock_guard<std::mutex> lock (mtx);
	grid.reset ();
	curve = c;
	curveX = xs;
	route = rt;
	best = selected = sol;
	launch = lp;
	bodies = b;
	status.progress = 1.0f;
	std::string seqs;
	for (const auto *fb : rt.bodies) { if (!seqs.empty()) seqs += "-"; seqs += fb->Name (); }
	char buf[256];
	snprintf (buf, sizeof (buf), "Route %s: %.0f m/s (ejection %.0f, flybys %.0f, manoeuvres %.0f, capture %.0f), %.0f days",
		seqs.c_str(), rt.dvTotal, rt.dvEject, rt.dvFlyby, rt.dvDsm, set.objective == OBJ_TOTAL ? rt.dvCapture : 0.0, rt.tof);
	status.message = buf;
}

bool Planner::Suggest ()
{
	Settings set = settings;
	set.flybys = FlybyBodies ();
	set.flybyAlt.clear ();
	set.assists = true;
	if (set.flybys.empty ()) {
		std::lock_guard<std::mutex> lock (mtx);
		status.message = "No bodies to fly by";
		return false;
	}
	// (the snapshot takes all the candidates as flyby bodies)
	Settings keep = settings;
	settings = set;
	auto b = Snapshot (set.target);
	settings = keep;
	if (!b) return false;
	Start (Status::SUGGEST, [this, b, set]() { DoSuggest (b, set); });
	return true;
}

void Planner::DoSuggest (std::shared_ptr<Bodies> b, Settings set)
{
	auto Progress = [&](float f) { std::lock_guard<std::mutex> lock (mtx); status.progress = f; };
	const double now = b->mjd0 + 600.0 / DAY;
	const int m = (int)b->fly.size ();
	// candidates by energy: between half the smaller and 1.6 times the larger
	// of the departure's and the target's orbits
	Vector p, v;
	double aDep = DepartureA (*b, now);
	double aTgt = b->Raw (b->target, now, p, v) ? SemiMajor (p, v, b->central.mu) : aDep;
	double lo = 0.5 * std::min (aDep, aTgt), hi = 1.6 * std::max (aDep, aTgt);
	std::vector<int> cand;
	std::vector<bool> use (m, false);
	for (int i = 0; i < m; i++) {
		double a = b->Raw (b->fly[i], now, p, v) ? SemiMajor (p, v, b->central.mu) : 0.0;
		if (a >= lo && a <= hi) cand.push_back (i), use[i] = true;
	}
	// without a limit: up to 2.5 times the Hohmann time to the target
	if (set.maxTof <= 0.0) set.maxTof = std::max (2.5 * 0.5 * Period (0.5 * (aDep + aTgt), b->central.mu) / DAY, 30.0);
	double span = RouteSpan (*b, set, std::vector<int> (), now);
	RouteTables (*b, now, span + set.maxTof + 2.0, &use);
	b->strict = true; // (the threads use the tables only)
	if (abort) return;

	struct Cand { std::vector<int> seq; double dv = 1e30, dep = 0.0, tof = 0.0; route::Result r; };
	auto Run = [&](std::vector<Cand> &cs, long evals, bool dsm, bool avoid, float p0, float p1) {
		std::atomic<int> next (0), done (0);
		int nt = std::clamp ((int)std::thread::hardware_concurrency () - 1, 1, 8);
		std::vector<std::thread> th;
		for (int t = 0; t < nt; t++)
			th.emplace_back ([&]() {
				for (int i; (i = next++) < (int)cs.size () && !abort; ) {
					Settings s2 (set);
					s2.dsm = dsm;
					route::Problem q = RouteProblem (*b, s2, cs[i].seq, now, span);
					if (avoid) Avoid (*b, q);
					route::Options o;
					o.seed = 1 + i;
					o.evals = evals;
					o.abort = &abort;
					if (dsm && cs[i].r.valid) {
						std::vector<double> x;
						if (route::ToDsm (q, cs[i].r, x)) o.seeds.push_back (x);
					}
					if (avoid && cs[i].r.valid && (int)cs[i].r.x.size () == route::Dimension (q)) { // (from the earlier optimum, now with the penalty)
						o.seeds.push_back (cs[i].r.x);
						cs[i].dv = route::Evaluate (q, cs[i].r.x.data(), &cs[i].r);
					}
					route::Result r = route::Optimise (q, o);
					if (r.valid && r.dv < cs[i].dv) cs[i].dv = r.dv, cs[i].dep = r.dep, cs[i].tof = r.tof, cs[i].r = r;
					done++;
				}
			});
		while (done < (int)cs.size () && !abort) {
			std::this_thread::sleep_for (std::chrono::milliseconds (50));
			Progress (p0 + (p1 - p0) * done / std::max<size_t> (cs.size (), 1));
		}
		for (auto &t : th) t.join ();
	};
	auto ByDv = [](const Cand &a, const Cand &c) { return a.dv < c.dv; };

	// direct, one and two flybys
	std::vector<Cand> cs (1);
	for (int i : cand) {
		cs.push_back (Cand{ { i } });
		for (int j : cand) cs.push_back (Cand{ { i, j } });
	}
	Run (cs, 12000, false, false, 0.0f, 0.45f);
	if (abort) return;
	// three: the best two-flyby routes and one more
	std::vector<Cand> two;
	for (auto &c : cs) if (c.seq.size () == 2 && c.dv < 1e29) two.push_back (c);
	std::sort (two.begin (), two.end (), ByDv);
	std::vector<Cand> three;
	for (size_t k = 0; k < two.size () && k < 6; k++)
		for (int j : cand) { Cand c; c.seq = two[k].seq; c.seq.push_back (j); three.push_back (c); }
	Run (three, 15000, false, false, 0.45f, 0.75f);
	if (abort) return;
	cs.insert (cs.end (), three.begin (), three.end ());
	// the best few again, longer (and with deep-space manoeuvres if allowed)
	std::sort (cs.begin (), cs.end (), ByDv);
	std::vector<Cand> top;
	bool direct = false;
	for (auto &c : cs) {
		if (c.dv >= 1e29) break;
		if (c.seq.empty ()) { if (!direct) top.push_back (c), direct = true; continue; }
		if (top.size () < 6) top.push_back (c);
		if (top.size () >= 6 && direct) break;
	}
	Run (top, 60000, false, true, 0.75f, set.dsm ? 0.85f : 0.98f);
	if (set.dsm && !abort) Run (top, 120000, true, true, 0.85f, 0.98f);
	if (abort) return;
	std::sort (top.begin (), top.end (), ByDv);
	std::vector<Suggestion> out;
	for (auto &c : top) {
		Suggestion sg;
		for (int i : c.seq) sg.flybys.push_back (b->fly[i].body);
		sg.dv = c.dv, sg.dep = c.dep, sg.tof = c.tof;
		sg.x = c.r.x, sg.dsm = (int)c.r.x.size () > 2 + (int)c.seq.size ();
		out.push_back (sg);
	}
	std::lock_guard<std::mutex> lock (mtx);
	suggestions = out;
	status.progress = 1.0f;
	char buf[256] = "No route found";
	for (auto &sg : out)
		if (!sg.flybys.empty ()) {
			std::string seqs;
			for (const auto *fb : sg.flybys) { seqs += fb->Name (); seqs += "-"; }
			seqs += b->target.body->Name ();
			snprintf (buf, sizeof (buf), "Best route: %s, %.0f m/s, %.0f days (%d found)", seqs.c_str (), sg.dv, sg.tof, (int)out.size ());
			break;
		}
	status.message = buf;
}

std::vector<Suggestion> Planner::Suggestions () const { std::lock_guard<std::mutex> lock (mtx); return suggestions; }

bool Planner::UseSuggestion (int i)
{
	std::vector<Suggestion> sg = Suggestions ();
	if (i < 0 || i >= (int)sg.size ()) return false;
	settings.flybys = sg[i].flybys;
	settings.flybyAlt.assign (settings.flybys.size (), 0.0);
	settings.assists = true;
	if (!sg[i].flybys.empty ()) settings.dsm = sg[i].dsm;
	routeSeed = sg[i].x;
	return Search ();
}
Route Planner::GetRoute () const { std::lock_guard<std::mutex> lock (mtx); return route; }

// -----------------------------------------------------------------------
// Routes in full gravity: each encounter targeted in turn by the last
// controllable node before it (the ejection, a leg's deep-space manoeuvre,
// or the correction after the previous flyby), the earlier ones kept

namespace {
	// the angular momentum direction of a planned flyby (null: no turn to speak of)
	const Vector *FlybyPlane (const RouteFlyby &f, Vector &h)
	{
		h = crossp (f.vinfInVec, f.vinfOutVec);
		if (f.turn < 0.5 * RAD || h.length() <= 0.0) return nullptr;
		h.unify ();
		return &h;
	}

	void SortPlan (Plan &pl)
	{
		std::stable_sort (pl.nodes.begin(), pl.nodes.end(), [](const PlanNode &a, const PlanNode &b) { return a.mjd < b.mjd; });
	}

	// insert a node in time order; returns its index
	int InsertNode (std::vector<maneuver::Node> &nodes, const maneuver::Node &n)
	{
		auto it = std::upper_bound (nodes.begin(), nodes.end(), n, [](const maneuver::Node &x, const maneuver::Node &y) { return x.mjd < y.mjd; });
		int k = (int)(it - nodes.begin());
		nodes.insert (it, n);
		return k;
	}

	// a node's velocity change in its frame from a global one (the state
	// there from a prediction with the node in place)
	bool NodeFrameDv (trajectory::Sandbox &sb, std::vector<maneuver::Node> &nodes, int k, const Vector &dvg, double days, const std::atomic<bool> *abort)
	{
		Vector keep (nodes[k].dv);
		nodes[k].dv = Vector ();
		auto p = sb.Predict (nodes, days, abort);
		if (!p || k >= (int)p->nodes.size()) { nodes[k].dv = keep; return false; }
		const trajectory::NodeState &ns = p->nodes[k];
		nodes[k].dv = maneuver::FromGlobal (dvg, ns.r, ns.v);
		return true;
	}
}

void Planner::DoRefineRoute (std::shared_ptr<Bodies> b, std::shared_ptr<trajectory::Sandbox> sb, Route rt, Solution sol, Settings set, bool arm)
{
	const int n = (int)rt.bodies.size(), nfly = n - 1;
	auto Progress = [&](int stage, float f) { std::lock_guard<std::mutex> lock (mtx); status.progress = (stage + f) / n; };
	auto Fail = [&](const std::string &m) { std::lock_guard<std::mutex> lock (mtx); status.message = m; };
	std::vector<maneuver::Node> nodes;
	Plan pl;
	pl.valid = true;
	pl.route = rt.bodies;
	pl.armed = arm;

	maneuver::Node ej;
	ej.mjd = sol.burnMjd, ej.dv = sol.burnDv, ej.autoburn = arm;
	nodes.push_back (ej);
	sb->Advance (ej.mjd - 60.0 / DAY, &abort); // (the coast in the parking orbit once)
	pl.nodes.push_back ({ ROLE_EJECTION, ej.mjd, ej.dv, true, true, 0 });
	int ctl = 0;
	auto DaysTo = [&](double mjd) { return (mjd - b->mjd0) * 1.15 + 5.0; };
	auto AddDsm = [&](int leg) -> bool {
		const RouteLeg &L = rt.legs[leg];
		if (!rt.dsm || L.dsmMjd <= 0.0 || L.dsmDv < 0.5) return false;
		maneuver::Node d;
		d.mjd = L.dsmMjd, d.autoburn = arm;
		int k = InsertNode (nodes, d);
		NodeFrameDv (*sb, nodes, k, L.dsmDvVec, DaysTo (L.dsmMjd) , &abort);
		ctl = k;
		pl.nodes.push_back ({ ROLE_DSM, d.mjd, nodes[k].dv, true, false, leg });
		return true;
	};
	AddDsm (0);
	Arrival a;
	std::string partly; // (encounters not targeted to the tolerance)
	for (int i = 0; i < n; i++) {
		if (abort) return;
		const CelestialBody *body = rt.bodies[i];
		const BodySnap &bs = b->Enc (i);
		sb->SetTarget (body);
		Vector h;
		const Vector *hd = (i < nfly) ? FlybyPlane (rt.flybys[i], h) : nullptr;
		double tCtl = nodes[ctl].mjd;
		Shooter sh{ sb.get(), b.get(), &set, &abort, DaysTo (rt.legs[i].t1), (tCtl - b->mjdRef) * DAY };
		sh.enc = (i < nfly) ? i : -1;
		sh.hd = hd;
		sh.rpWant = (i < nfly) ? bs.radius + rt.flybys[i].alt : 0.0;
		sh.tFix = (i < nfly) ? rt.flybys[i].mjd : rt.legs[i].t1;
		std::string msg;
		bool ok = sh.Run (nodes, ctl, a, [&](float f) { Progress (i, f); }, msg);
		if (abort) return;
		if (!a.soi) {
			Fail (std::string ("Targeting ") + body->Name () + " failed" + (msg.empty() ? "" : ": " + msg));
			return;
		}
		if (!ok) partly += std::string (partly.empty () ? "" : ", ") + body->Name ();
		pl.nodes[0].dv = nodes[0].dv; // (the ejection, if it was the one targeted)
		for (auto &pn : pl.nodes) if (pn.role == ROLE_DSM && FindNode (pn.mjd) < 0)
			for (const auto &nd : nodes) if (nd.mjd == pn.mjd) pn.dv = nd.dv;
		double tEnc = a.peri ? a.periMjd : a.entryMjd;
		// corrections on the way (computed when due)
		for (int m = 0; m < 2; m++)
			if (set.useMcc[m])
				pl.nodes.push_back ({ ROLE_MCC, tCtl + set.mcc[m] * (tEnc - tCtl), Vector (), false, false, i });
		if (i < nfly) {
			RouteFlyby f = rt.flybys[i];
			f.mjd = tEnc, f.alt = a.periAlt;
			pl.flybys.push_back (f);
			pl.flybyTarget.push_back (rt.flybys[i].alt);
			// powered flyby: the burn at the periapsis
			if (!rt.dsm && rt.flybys[i].dv >= 0.5 && a.peri) {
				maneuver::Node fn;
				fn.mjd = a.periMjd, fn.autoburn = arm;
				fn.dv = Vector ((rt.flybys[i].vinfOut >= rt.flybys[i].vinfIn ? 1.0 : -1.0) * rt.flybys[i].dv, 0.0, 0.0);
				InsertNode (nodes, fn);
				pl.nodes.push_back ({ ROLE_FLYBY, fn.mjd, fn.dv, true, true, i });
			}
			// the next leg's control: its deep-space manoeuvre, or a correction
			// after this flyby (out of the sphere of influence)
			if (!AddDsm (i + 1)) {
				double vout = std::max (rt.flybys[i].vinfOut, 100.0);
				double tExit = tEnc + std::min (1.5 * bs.soi / vout / DAY, 0.2 * (rt.legs[i+1].t1 - rt.legs[i+1].t0));
				maneuver::Node pc;
				pc.mjd = tExit, pc.autoburn = arm;
				ctl = InsertNode (nodes, pc);
				pl.nodes.push_back ({ ROLE_MCC, pc.mjd, Vector (), true, false, i + 1 });
			} else {
				for (size_t k = 0; k < nodes.size(); k++) if (nodes[k].mjd == rt.legs[i+1].dsmMjd) ctl = (int)k;
			}
		}
	}
	// the arrival
	if (set.objective == OBJ_TOTAL && a.peri) {
		double rp = b->target.radius + a.periAlt;
		double ra = std::max (rp, std::min (b->CapApo (), 0.9 * b->target.soi));
		double vc = sqrt (b->target.mu * (2.0 / rp - 2.0 / (rp + ra)));
		pl.nodes.push_back ({ ROLE_CAPTURE, a.periMjd, Vector (-(a.periSpeed - vc), 0.0, 0.0), true, true, n - 1 });
	}
	// the computed nodes' velocity changes (the targeting changed them)
	for (auto &pn : pl.nodes)
		if (pn.isNode && pn.role != ROLE_CAPTURE)
			for (const auto &nd : nodes) if (nd.mjd == pn.mjd) pn.dv = nd.dv;
	SortPlan (pl);
	pl.arrMjd = a.periMjd;
	pl.periAlt = a.periAlt;
	pl.periTarget = b->capAlt;
	pl.incl = a.incl, pl.inclMin = a.inclMin;
	pl.bt = a.bt, pl.br = a.br, pl.btT = a.btT, pl.brT = a.brT;
	pl.arrives = true;

	std::lock_guard<std::mutex> lock (mtx);
	pendingPlan = pl;
	planPending = true;
	pendingCorrection = false;
	status.progress = 1.0f;
	std::string fl;
	for (const auto &f : pl.flybys) {
		char t[96];
		snprintf (t, sizeof (t), "%s%s %.0f km", fl.empty() ? "" : ", ", f.body->Name (), f.alt * 1e-3);
		fl += t;
	}
	char buf[400];
	if (partly.empty ())
		snprintf (buf, sizeof (buf), "Route targeted: flybys %s; periapsis %.0f km (target %.0f km)", fl.c_str(), a.periAlt * 1e-3, b->capAlt * 1e-3);
	else // (the corrections on the way aim again, closer in)
		snprintf (buf, sizeof (buf), "Route partly targeted (%s: the corrections on the way aim again): flybys %s; periapsis %.0f km (target %.0f km)",
			partly.c_str (), fl.c_str(), a.periAlt * 1e-3, b->capAlt * 1e-3);
	status.message = buf;
}

void Planner::DoCorrectRoute (std::shared_ptr<Bodies> b, std::shared_ptr<trajectory::Sandbox> sb, std::vector<maneuver::Node> nodes,
	int refineIndex, double mjd, int planIndex, Settings set, bool arm)
{
	Plan pl = GetPlan ();
	Route rt = GetRoute ();
	const int n = (int)pl.route.size(), nfly = n - 1;
	int k = refineIndex;
	if (k < 0) {
		maneuver::Node c;
		c.mjd = mjd, c.autoburn = arm;
		k = InsertNode (nodes, c);
	}
	// the encounter it aims at
	int aim = (planIndex >= 0 && planIndex < (int)pl.nodes.size()) ? pl.nodes[planIndex].aim : -1;
	if (aim < 0) {
		aim = n - 1;
		for (int i = 0; i < nfly && i < (int)pl.flybys.size(); i++) if (pl.flybys[i].mjd > mjd) { aim = i; break; }
	}
	double tAim = (aim < nfly && aim < (int)pl.flybys.size()) ? pl.flybys[aim].mjd : pl.arrMjd;
	sb->SetTarget (pl.route[aim]);
	Vector h;
	const Vector *hd = (aim < nfly && aim < (int)rt.flybys.size()) ? FlybyPlane (rt.flybys[aim], h) : nullptr;
	Shooter sh{ sb.get(), b.get(), &set, &abort, (tAim - b->mjd0) * 1.2 + 3.0, (mjd - b->mjdRef) * DAY };
	sh.enc = (aim < nfly) ? aim : -1;
	sh.hd = hd;
	sh.rpWant = (aim < nfly && aim < (int)pl.flybyTarget.size()) ? b->Enc (aim).radius + pl.flybyTarget[aim] : 0.0;
	sh.tFix = tAim;
	Arrival a;
	std::string msg;
	bool ok = sh.Run (nodes, k, a, [&](float f) { std::lock_guard<std::mutex> lock (mtx); status.progress = f; }, msg);
	if (abort) return;
	if (!a.soi) {
		std::lock_guard<std::mutex> lock (mtx);
		status.message = msg.empty() ? "Correction failed" : msg;
		return;
	}
	double tEnc = a.peri ? a.periMjd : a.entryMjd;
	// the burns at that encounter move with it
	for (auto &pn : pl.nodes) {
		if (pn.aim != aim) continue;
		if (pn.role == ROLE_FLYBY) pn.mjd = tEnc;
		if (pn.role == ROLE_CAPTURE && a.peri) {
			double rp = b->target.radius + a.periAlt;
			double ra = std::max (rp, std::min (b->CapApo (), 0.9 * b->target.soi));
			double vc = sqrt (b->target.mu * (2.0 / rp - 2.0 / (rp + ra)));
			pn.mjd = a.periMjd;
			pn.dv = Vector (-(a.periSpeed - vc), 0.0, 0.0);
		}
	}
	if (aim < nfly && aim < (int)pl.flybys.size()) pl.flybys[aim].mjd = tEnc, pl.flybys[aim].alt = a.periAlt;
	else {
		pl.arrMjd = a.periMjd, pl.periAlt = a.periAlt;
		pl.incl = a.incl, pl.inclMin = a.inclMin;
		pl.bt = a.bt, pl.br = a.br, pl.btT = a.btT, pl.brT = a.brT;
	}
	PlanNode corr{ ROLE_MCC, mjd, nodes[k].dv, refineIndex >= 0 || nodes[k].dv.length() >= 0.05, true, aim };
	if (planIndex >= 0 && planIndex < (int)pl.nodes.size()) {
		corr.role = pl.nodes[planIndex].role;
		if (corr.role == ROLE_EJECTION) pl.ejRetargeted = true;
		pl.nodes[planIndex] = corr;
	} else {
		pl.nodes.push_back (corr);
	}
	SortPlan (pl);
	planIndex = -1;
	for (size_t i = 0; i < pl.nodes.size(); i++) if (pl.nodes[i].mjd == mjd && pl.nodes[i].done) planIndex = (int)i;

	std::lock_guard<std::mutex> lock (mtx);
	pendingPlan = pl;
	planPending = true;
	pendingCorrection = true;
	pendingPlanIndex = planIndex;
	status.progress = 1.0f;
	char buf[200];
	const char *what = corr.role == ROLE_EJECTION ? "Ejection targeted again" : corr.role == ROLE_DSM ? "Manoeuvre targeted" : "Correction";
	snprintf (buf, sizeof (buf), "%s%s: %.2f m/s, %s periapsis %.0f km", what, ok ? "" : " (partly)", nodes[k].dv.length(),
		pl.route[aim]->Name (), a.periAlt * 1e-3);
	status.message = buf;
}

int Planner::FindNode (double mjd) const
{
	const auto &n = vessel->ManeuverNodes ();
	for (size_t i = 0; i < n.size(); i++)
		if (fabs (n[i].mjd - mjd) < 1e-9) return (int)i;
	return -1;
}

void Planner::Arm (bool on)
{
	std::lock_guard<std::mutex> lock (mtx);
	if (!plan.valid) return;
	plan.armed = on;
	for (const auto &pn : plan.nodes) {
		if (!pn.isNode) continue;
		int i = FindNode (pn.mjd);
		if (i < 0) continue;
		maneuver::Node n = vessel->ManeuverNodes ()[i];
		n.autoburn = on;
		vessel->SetManeuverNode ((DWORD)i, n);
	}
}

void Planner::Clear ()
{
	Abort ();
	std::lock_guard<std::mutex> lock (mtx);
	for (const auto &pn : plan.nodes) {
		if (!pn.isNode) continue;
		int i = FindNode (pn.mjd);
		if (i >= 0) vessel->DelManeuverNode ((DWORD)i);
	}
	plan = Plan ();
	status.serial++;
}

void Planner::Update ()
{
	{
		std::lock_guard<std::mutex> lock (mtx);
		if (planPending) {
			planPending = false;
			if (!pendingCorrection) {
				// a new plan: its nodes replace the vessel's
				vessel->ClearManeuverNodes ();
				for (const auto &pn : pendingPlan.nodes)
					if (pn.isNode) {
						maneuver::Node n;
						n.mjd = pn.mjd, n.dv = pn.dv, n.autoburn = pendingPlan.armed;
						vessel->AddManeuverNode (n);
					}
			} else {
				// a correction: its node (if needed), the capture node (and a route's
				// powered flyby at that encounter) moved
				for (size_t i = 0; i < plan.nodes.size(); i++) {
					const PlanNode &old = plan.nodes[i];
					if ((old.role != ROLE_CAPTURE && old.role != ROLE_FLYBY) || !old.isNode) continue;
					int k = FindNode (old.mjd);
					for (const auto &pn : pendingPlan.nodes)
						if (pn.role == old.role && pn.aim == old.aim) {
							maneuver::Node n;
							if (k >= 0) n = vessel->ManeuverNodes ()[k];
							n.mjd = pn.mjd, n.dv = pn.dv, n.autoburn = pendingPlan.armed;
							if (k >= 0) vessel->SetManeuverNode ((DWORD)k, n);
							else vessel->AddManeuverNode (n);
						}
				}
				if (pendingPlanIndex >= 0 && pendingPlanIndex < (int)pendingPlan.nodes.size()) {
					const PlanNode &c = pendingPlan.nodes[pendingPlanIndex];
					int k;
					if ((c.role == ROLE_EJECTION || !pendingPlan.route.empty ()) && (k = FindNode (c.mjd)) >= 0) {
						maneuver::Node n = vessel->ManeuverNodes ()[k];
						n.dv = c.dv;
						vessel->SetManeuverNode ((DWORD)k, n);
					} else if (c.isNode) {
						maneuver::Node n;
						n.mjd = c.mjd, n.dv = c.dv, n.autoburn = pendingPlan.armed;
						vessel->AddManeuverNode (n);
					}
				}
			}
			plan = pendingPlan;
			status.job = Status::IDLE;
			status.serial++;
		}
	}

	// a correction due soon: refine it now (with the trajectory as flown)
	if (busy || !settings.autoCorrect) return;
	if (td.SysT1 - lastPlanCheck < 0.5) return;
	lastPlanCheck = td.SysT1;
	Plan pl = GetPlan ();
	if (!pl.valid) return;
	double now = td.MJD1;
	double tEj = -1.0;
	for (const auto &pn : pl.nodes) if (pn.role == ROLE_EJECTION) tEj = pn.mjd;
	if (!pl.ejRetargeted && tEj > now && FindNode (tEj) >= 0) {
		// the ejection again shortly before it (two orbits): a long coast in a low
		// orbit drifts from its prediction, which the ejection amplifies
		const CelestialBody *ref = vessel->ElRef ();
		const Elements *el = vessel->Els ();
		double P = (el && el->e < 1.0) ? el->OrbitT () : 3600.0;
		double lead = std::clamp (2.0 * P, 1800.0, 6.0 * 3600.0);
		double tdue = (tEj - now) * DAY;
		if (ref && tdue < lead && tdue > 0.25 * P) Correct (true);
		return;
	}
	if (tEj < 0.0 || now < tEj) return;              // (before the ejection)
	if (FindNode (tEj) >= 0 && now < tEj + 2.0 / 24.0) return; // (ejection still being flown)
	double cruise = (pl.arrMjd - tEj) * DAY;
	double lead = std::clamp (0.03 * cruise, 600.0, 6.0 * 3600.0);
	for (const auto &pn : pl.nodes)
		if ((pn.role == ROLE_MCC || pn.role == ROLE_DSM) && !pn.done && pn.mjd > now && (pn.mjd - now) * DAY < lead) {
			Correct ();
			break;
		}
}

std::shared_ptr<const Grid> Planner::Porkchop () const { std::lock_guard<std::mutex> lock (mtx); return grid; }
std::shared_ptr<const Curve> Planner::Windows () const { std::lock_guard<std::mutex> lock (mtx); return curve; }
Solution Planner::Best () const { std::lock_guard<std::mutex> lock (mtx); return best; }
Solution Planner::Selected () const { std::lock_guard<std::mutex> lock (mtx); return selected; }
Plan Planner::GetPlan () const { std::lock_guard<std::mutex> lock (mtx); return plan; }
Status Planner::GetStatus () const { std::lock_guard<std::mutex> lock (mtx); return status; }
Kind Planner::CurrentKind () const { std::lock_guard<std::mutex> lock (mtx); return bodies ? bodies->kind : KIND_NONE; }
const CelestialBody *Planner::Departure () const { std::lock_guard<std::mutex> lock (mtx); return bodies ? bodies->ref.body : nullptr; }
const CelestialBody *Planner::Central () const { std::lock_guard<std::mutex> lock (mtx); return bodies ? bodies->central.body : nullptr; }

double Planner::AvailableDv () const
{
	double thrust, ve;
	Vector dir;
	if (!vessel->MainEngines (thrust, ve, dir)) return 0.0;
	const ThrustGroupSpec *tgs = vessel->GetThrusterGroup (THGROUP_MAIN);
	std::vector<const void*> tanks;
	double mp = 0.0;
	for (const ThrustSpec *ts : tgs->ts)
		if (ts->tank && std::find (tanks.begin(), tanks.end(), (const void*)ts->tank) == tanks.end()) {
			tanks.push_back (ts->tank);
			mp += ts->tank->mass;
		}
	double m0 = vessel->Mass ();
	if (mp <= 0.0 || mp >= m0) return 0.0;
	return ve * log (m0 / (m0 - mp));
}

} // namespace transfer
