// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Special relativity for relativistic flight (see Relativity.h)
// =======================================================================

#include "Relativity.h"
#include <algorithm>
#include <cmath>

namespace relativity {

	namespace {
		const double C2 = C * C;

		/// (gamma - 1) / v^2 for proper velocity u: gamma^2 / (c^2 (gamma + 1)), regular at v = 0
		inline double BoostCoef (double gamma) { return gamma * gamma / (C2 * (gamma + 1.0)); }
	}

	// ===================================================================
	// Speeds

	double Gamma (double v)
	{
		double b2 = v * v / C2;
		return b2 < 1.0 ? 1.0 / std::sqrt (1.0 - b2) : HUGE_VAL;
	}

	double GammaOfU (const Vector &u)
	{
		return std::sqrt (1.0 + u.length2 () / C2);
	}

	double GammaMinus1OfU (const Vector &u)
	{
		double q = u.length2 () / C2;
		return q / (std::sqrt (1.0 + q) + 1.0);
	}

	Vector VelocityOfU (const Vector &u)
	{
		return u / GammaOfU (u);
	}

	Vector UOfVelocity (const Vector &v)
	{
		return v * Gamma (v.length ());
	}

	double Rapidity (double v)
	{
		return std::atanh (v / C);
	}

	double SpeedOfRapidity (double eta)
	{
		return C * std::tanh (eta);
	}

	Vector AddVelocity (const Vector &v, const Vector &w)
	{
		double g = Gamma (v.length ());
		double vw = dotp (v, w);
		return (v + w / g + v * (BoostCoef (g) / g * vw)) / (1.0 + vw / C2);
	}

	void Boost (const Vector &v, double &t, Vector &x)
	{
		double g = Gamma (v.length ());
		double vx = dotp (v, x);
		double t1 = g * (t - vx / C2);
		x = x + v * (BoostCoef (g) * vx - g * t);
		t = t1;
	}

	// ===================================================================
	// Light

	Vector Aberrate (const Vector &n, const Vector &v)
	{
		// (the photon arriving from n, travelling along -n, boosted)
		double g = Gamma (v.length ());
		double nv = dotp (n, v);
		return (n + v * (BoostCoef (g) * nv + g / C)) / (g * (1.0 + nv / C));
	}

	double Doppler (const Vector &n, const Vector &v)
	{
		return Gamma (v.length ()) * (1.0 + dotp (n, v) / C);
	}

	double EmissionPos (const Vector &p, const Vector &v, const Vector *pp, const Vector *vp, const Vector &obs, Vector &pe)
	{
		// (about the parent: its angular velocity w = r x v / r^2 and the
		// radial rate; dt = |pe - obs| / c iterated, converging as fast as
		// v / c)
		Vector r, w;
		double radial = 0.0;
		if (pp && vp) {
			r = p - *pp;
			Vector vr = v - *vp;
			double r2 = r.length2 ();
			if (r2 > 0.0) w = crossp (r, vr) / r2, radial = dotp (r, vr) / r2;
			else pp = nullptr;
		}
		double dt = (p - obs).length () / C;
		for (int i = 0; i < 16; i++) {
			if (pp && vp) {
				double wl = w.length (), a = -wl * dt;
				Vector rr = r;
				if (wl > 0.0) {
					Vector k = w / wl;
					rr = r * std::cos (a) + crossp (k, r) * std::sin (a) + k * (dotp (k, r) * (1.0 - std::cos (a)));
				}
				pe = *pp - *vp * dt + rr * (1.0 - radial * dt);
			} else pe = p - v * dt;
			double dn = (pe - obs).length () / C;
			bool done = std::fabs (dn - dt) <= 1e-12 * dn;
			dt = dn;
			if (done) break;
		}
		return dt;
	}

	// ===================================================================
	// Gravity and clocks

	Vector PNAccel (const Vector &g, double phi, const Vector &v)
	{
		return g * (1.0 + (v.length2 () + 4.0 * phi) / C2) - v * (4.0 * dotp (v, g) / C2);
	}

	Vector CoordAccel (const Vector &alpha, const Vector &v)
	{
		Vector u = UOfVelocity (v);
		double g = GammaOfU (u);
		Vector du = BoostedAcceleration (alpha, u) / g;  // du/dt
		double dg = dotp (u, du) / (C2 * g);             // dgamma/dt
		return (du - v * dg) / g;
	}

	Vector ProperOfCoordAccel (const Vector &a, const Vector &v)
	{
		double s = v.length();
		if (s <= 0.0) return a;
		double g = Gamma (s);
		Vector n (v / s);
		Vector par (n * dotp (a, n));
		return par * (g * g * g) + (a - par) * (g * g);
	}

	Vector URate (const Vector &u, const Vector &a, const Vector &alpha)
	{
		double g = GammaOfU (u);
		Vector v = u / g;
		// d(gamma v)/dt = gamma a + gamma^3 (v.a) v / c^2, and the proper acceleration's
		return a * g + v * (g * g * g * dotp (v, a) / C2) + BoostedAcceleration (alpha, u) / g;
	}

	double ClockRateMinus1 (double v2, double phi)
	{
		double q = (2.0 * phi - v2) / C2;
		return q / (std::sqrt (1.0 + q) + 1.0);
	}

	// ===================================================================
	// Motion

	Vector BoostedAcceleration (const Vector &alpha, const Vector &u)
	{
		double g = GammaOfU (u);
		return alpha + u * (dotp (alpha, u) / (C2 * (g + 1.0)));
	}

	namespace {
		struct Deriv { double dt; Vector dx, du; };

		inline Deriv Rate (double t, const Vector &x, const Vector &u, const Vector &alpha, const Field *field)
		{
			double g = GammaOfU (u);
			Deriv d;
			d.dt = g;
			d.dx = u;
			d.du = BoostedAcceleration (alpha, u);
			if (field) d.du += (*field) (t, x, u / g) * g;
			return d;
		}
	}

	void Propagate (State &s, const Vector &alpha, double dtau, const Field *field, int steps)
	{
		if (dtau <= 0.0) return;
		if (steps <= 0)
			steps = std::max (1, (int)std::ceil (alpha.length () * dtau / C / 0.01));
		double h = dtau / steps;
		for (int i = 0; i < steps; i++) {
			Deriv k1 = Rate (s.t, s.x, s.u, alpha, field);
			Deriv k2 = Rate (s.t + 0.5 * h * k1.dt, s.x + k1.dx * (0.5 * h), s.u + k1.du * (0.5 * h), alpha, field);
			Deriv k3 = Rate (s.t + 0.5 * h * k2.dt, s.x + k2.dx * (0.5 * h), s.u + k2.du * (0.5 * h), alpha, field);
			Deriv k4 = Rate (s.t + h * k3.dt, s.x + k3.dx * h, s.u + k3.du * h, alpha, field);
			s.t += h / 6.0 * (k1.dt + 2.0 * k2.dt + 2.0 * k3.dt + k4.dt);
			s.x += (k1.dx + k2.dx * 2.0 + k3.dx * 2.0 + k4.dx) * (h / 6.0);
			s.u += (k1.du + k2.du * 2.0 + k3.du * 2.0 + k4.du) * (h / 6.0);
		}
		s.tau += dtau;
	}

	void Hyperbolic (double a, double tau, double &t, double &x, double &v)
	{
		if (a <= 0.0) { t = tau, x = 0.0, v = 0.0; return; }
		double eta = a * tau / C;
		double sh = std::sinh (0.5 * eta);
		t = C / a * std::sinh (eta);
		x = 2.0 * C2 / a * sh * sh; // (c^2/a (cosh eta - 1), accurate at small eta)
		v = C * std::tanh (eta);
	}

	void FlipAndBurn (double d, double a, double &tau, double &t, double &vmax)
	{
		// each half: c^2/a (cosh eta - 1) = d/2, i.e. sinh (eta/2) = sqrt (a d / (4 c^2))
		double eta = 2.0 * std::asinh (std::sqrt (a * d / (4.0 * C2)));
		tau = 2.0 * C / a * eta;
		t = 2.0 * C / a * std::sinh (eta);
		vmax = C * std::tanh (eta);
	}

	namespace {
		/// Rapidity of nb burns (1: accelerate then coast; 2: accelerate,
		/// coast, decelerate) covering distance d in coordinate time t, or 0
		/// if t is too short. The distance grows with eta (its derivative is
		/// c sech^2 eta times the coast's time), so bisection.
		double CoastEta (double d, double a, double t, int nb)
		{
			if (d <= 0.0 || t <= 0.0 || a <= 0.0) return 0.0;
			auto dist = [&](double eta) {
				double coast = t - nb * C / a * std::sinh (eta);
				double sh = std::sinh (0.5 * eta);
				return nb * 2.0 * C2 / a * sh * sh + C * std::tanh (eta) * coast;
			};
			double hi = std::asinh (a * t / (nb * C));
			if (dist (hi) < d) return 0.0;
			double lo = 0.0;
			for (int i = 0; i < 200 && hi - lo > 1e-15 * hi; i++) {
				double mid = 0.5 * (lo + hi);
				(dist (mid) < d ? lo : hi) = mid;
			}
			return 0.5 * (lo + hi);
		}
	}

	double CoastRapidity (double d, double a, double t, double &tauBurn)
	{
		double eta = CoastEta (d, a, t, 2);
		tauBurn = eta > 0.0 ? C / a * eta : 0.0;
		return eta;
	}

	// ===================================================================
	// Trips

	namespace {
		/// Unit vectors perpendicular to d (and each other)
		void Perp (const Vector &d, Vector &e1, Vector &e2)
		{
			Vector ref = std::fabs (d.x) < 0.6 ? Vector (1, 0, 0) : Vector (0, 1, 0);
			e1 = crossp (d, ref).unit ();
			e2 = crossp (d, e1);
		}

		/// Solve the n x n system A x = b in place (partial pivoting);
		/// false if singular
		bool SolveLinear (int n, double A[6][6], double b[6])
		{
			for (int k = 0; k < n; k++) {
				int p = k;
				for (int i = k + 1; i < n; i++) if (std::fabs (A[i][k]) > std::fabs (A[p][k])) p = i;
				if (std::fabs (A[p][k]) < 1e-300) return false;
				if (p != k) { std::swap (A[p], A[k]); std::swap (b[p], b[k]); }
				for (int i = k + 1; i < n; i++) {
					double f = A[i][k] / A[k][k];
					for (int j = k; j < n; j++) A[i][j] -= f * A[k][j];
					b[i] -= f * b[k];
				}
			}
			for (int k = n - 1; k >= 0; k--) {
				double s = b[k];
				for (int j = k + 1; j < n; j++) s -= A[k][j] * b[j];
				b[k] = s / A[k][k];
			}
			return true;
		}

		/// The trip's unknowns: burn directions (as offsets in the planes
		/// perpendicular to the current ones) and burn lengths
		struct Unknowns {
			Vector dir[2];
			double tau[2] = { 0.0, 0.0 };
		};

		struct Evaluator {
			const TripSpec &spec;
			int nb;            // burns
			double L, Vs;      // scales of position and velocity residuals
			int steps[2];      // Runge-Kutta steps of each burn (fixed within an iteration)

			/// The arrival of a trip: coast's coordinate time, arrival time
			/// and state; residual vector (scaled)
			int Residual (const Unknowns &k, double *r, double *coastDt = nullptr, State *arrive = nullptr) const
			{
				State s = spec.start;
				Propagate (s, k.dir[0] * spec.accel, k.tau[0], nullptr, steps[0]);
				Vector xf, vf;
				double tA, tc;
				if (nb == 2) {
					State b; // (the second burn, from the origin: without gravity it doesn't depend on where)
					b.u = s.u;
					Propagate (b, k.dir[1] * spec.accel, k.tau[1], nullptr, steps[1]);
					tc = spec.tArrive > 0.0 ? spec.tArrive - s.t - b.t : 0.0;
					tA = s.t + tc + b.t;
					xf = s.x + s.V () * tc + b.x;
					vf = b.V ();
					if (arrive) arrive->t = tA, arrive->tau = s.tau + tc / s.Gamma () + b.tau, arrive->x = xf, arrive->u = b.u;
				} else {
					tc = spec.tArrive > 0.0 ? spec.tArrive - s.t : 0.0;
					tA = s.t + tc;
					xf = s.x + s.V () * tc;
					vf = s.V ();
					if (arrive) arrive->t = tA, arrive->tau = s.tau + tc / s.Gamma (), arrive->x = xf, arrive->u = s.u;
				}
				if (coastDt) *coastDt = tc;
				Vector xt, vt;
				spec.target (tA, xt, vt);
				Vector dx = (xf - xt) / L;
				r[0] = dx.x, r[1] = dx.y, r[2] = dx.z;
				if (nb == 2) {
					Vector dv = (vf - vt) / Vs;
					r[3] = dv.x, r[4] = dv.y, r[5] = dv.z;
					return 6;
				}
				return 3;
			}
		};

		double Norm (const double *r, int n)
		{
			double s = 0.0;
			for (int i = 0; i < n; i++) s += r[i] * r[i];
			return std::sqrt (s);
		}

		/// Unknowns after a step: angles (p, q) about each direction, scaled burn lengths
		Unknowns Apply (const Unknowns &k, const double *d, int nb, const double *tauScale)
		{
			Unknowns n = k;
			for (int b = 0; b < nb; b++) {
				Vector e1, e2;
				Perp (k.dir[b], e1, e2);
				n.dir[b] = (k.dir[b] + e1 * d[2 * b] + e2 * d[2 * b + 1]).unit ();
			}
			for (int b = 0; b < nb; b++)
				n.tau[b] = k.tau[b] + d[2 * nb + b] * tauScale[b];
			return n;
		}
	}

	Trip SolveTrip (const TripSpec &spec)
	{
		Trip trip;
		const double a = spec.accel;
		if (a <= 0.0 || !spec.target) return trip;
		const int nb = spec.mode == TRIP_RENDEZVOUS ? 2 : 1;
		const bool fixedT = spec.tArrive > 0.0;
		const State &s0 = spec.start;
		const Vector x0 = s0.x, v0 = s0.V ();
		if (fixedT && spec.tArrive <= s0.t) return trip;

		// guess: straight-line trips to where the target will be
		Unknowns k;
		double T = 0.0;
		Vector r, xt, vt;
		spec.target (s0.t, xt, vt);
		{
			double tau, vmax;
			if (fixedT) T = spec.tArrive - s0.t;
			else if (nb == 2) FlipAndBurn ((xt - x0).length (), a, tau, T, vmax);
			else {
				double d = (xt - x0).length ();
				T = C / a * std::sqrt (std::pow (1.0 + a * d / C2, 2) - 1.0);
			}
			for (int it = 0; it < (fixedT ? 1 : 12); it++) {
				spec.target (s0.t + T, xt, vt);
				r = nb == 2 ? xt - x0 - (v0 + vt) * (0.5 * T) : xt - x0 - v0 * T;
				if (fixedT) break;
				double d = r.length (), Tn;
				if (nb == 2) {
					FlipAndBurn (d, a, tau, Tn, vmax);
					Tn = std::max (Tn, (vt - v0).length () / a);
				} else Tn = C / a * std::sqrt (std::pow (1.0 + a * d / C2, 2) - 1.0);
				if (std::fabs (Tn - T) < 1e-9 * T) { T = Tn; break; }
				T = Tn;
			}
		}
		double d = r.length ();
		Vector rhat = d > 0.0 ? r / d : (vt - v0).length () > 0.0 ? (vt - v0).unit () : Vector (1, 0, 0);
		double eta;
		if (fixedT) {
			eta = CoastEta (d, a, T, nb);
			if (eta <= 0.0) eta = nb == 2 ? 2.0 * std::asinh (std::sqrt (a * d / (4.0 * C2))) : std::asinh (a * T / C);
		} else eta = nb == 2 ? 2.0 * std::asinh (std::sqrt (a * d / (4.0 * C2))) : std::asinh (a * T / C);
		double tauGuess = C / a * eta;
		if (nb == 2) {
			Vector dv = (vt - v0) / (a * std::max (T, 1e-3));
			k.dir[0] = (rhat + dv).unit ();
			k.dir[1] = (-rhat + dv).unit ();
			k.tau[0] = k.tau[1] = std::max (tauGuess, 1e-3);
		} else {
			k.dir[0] = rhat;
			k.tau[0] = std::max (tauGuess, 1e-3);
		}
		// warm start: an earlier solution of the trip, on from where the
		// start state is along it (a guess better than straight lines, above
		// all with a velocity to cancel)
		if (spec.warm && spec.warm->ok && (int)spec.warm->burns.size() == nb) {
			const std::vector<Burn> &wb = spec.warm->burns;
			double e = std::max (spec.warmElapsed, 0.0), tiny = 1e-3;
			k.dir[0] = wb[0].dir;
			k.tau[0] = std::max (wb[0].dtau - e, tiny);
			if (nb == 2) {
				k.dir[1] = wb[1].dir;
				k.tau[1] = e <= wb[1].tau0 ? wb[1].dtau : std::max (wb[1].dtau - (e - wb[1].tau0), tiny);
			}
		}

		Evaluator ev{ spec, nb };
		ev.L = std::max (d, (xt - x0).length ());
		ev.L = std::max (ev.L, 1.0);
		ev.Vs = ev.L / std::max (T, 1e-3);
		const int n = 3 * nb; // unknowns: 2 angles and a length per burn
		double res[6], best;

		// Newton iterations, finite-difference Jacobian, backtracking
		for (int it = 0; it < 80; it++) {
			for (int b = 0; b < nb; b++)
				ev.steps[b] = std::max (4, (int)std::ceil (a * k.tau[b] / C / 0.005));
			int m = ev.Residual (k, res);
			best = Norm (res, m);
			trip.iterations = it;
			if (best < 1e-12) break;
			// (lengths scaled by the trip's: a burn nearly done stays well conditioned)
			double tsum = std::max (k.tau[0] + (nb == 2 ? k.tau[1] : 0.0), 1e-3);
			double tauScale[2] = { std::max (k.tau[0], 1e-3 * tsum), std::max (k.tau[1], 1e-3 * tsum) };
			double J[6][6], rhs[6];
			for (int j = 0; j < n; j++) {
				double h[6] = {}, rp[6];
				h[j] = 1e-7;
				ev.Residual (Apply (k, h, nb, tauScale), rp);
				for (int i = 0; i < m; i++) J[i][j] = (rp[i] - res[i]) / h[j];
			}
			for (int i = 0; i < m; i++) rhs[i] = -res[i];
			if (!SolveLinear (n, J, rhs)) break;
			// limit the step: angles to 0.5 rad, lengths to stay positive
			double f = 1.0;
			for (int b = 0; b < nb; b++) {
				double ang = std::hypot (rhs[2 * b], rhs[2 * b + 1]);
				if (ang * f > 0.5) f = 0.5 / ang;
				double dl = rhs[2 * nb + b] * tauScale[b]; // (length change: to at most a tenth of the length)
				if (dl * f < -0.9 * k.tau[b]) f = -0.9 * k.tau[b] / dl;
			}
			bool improved = false;
			for (int ls = 0; ls < 30; ls++, f *= 0.5) {
				double step[6], rt[6];
				for (int j = 0; j < n; j++) step[j] = rhs[j] * f;
				Unknowns kt = Apply (k, step, nb, tauScale);
				if (Norm (rt, ev.Residual (kt, rt)) < best) { k = kt; improved = true; break; }
			}
			if (!improved) break;
		}

		// the trip
		for (int b = 0; b < nb; b++)
			ev.steps[b] = std::max (4, (int)std::ceil (a * k.tau[b] / C / 0.005));
		State arr;
		double coast;
		int m = ev.Residual (k, res, &coast, &arr);
		Vector xa, va;
		spec.target (arr.t, xa, va);
		trip.missX = (arr.x - xa).length ();
		trip.missV = nb == 2 ? (arr.V () - va).length () : 0.0;
		trip.coastDt = coast;
		trip.tArrive = arr.t;
		trip.tauArrive = arr.tau - s0.tau;
		Burn b0;
		b0.tau0 = 0.0, b0.dtau = k.tau[0], b0.dir = k.dir[0];
		trip.burns.push_back (b0);
		if (nb == 2) {
			State s1 = s0; // (after the first burn: the coast's proper time)
			Propagate (s1, k.dir[0] * a, k.tau[0], nullptr, ev.steps[0]);
			Burn b1;
			b1.tau0 = k.tau[0] + coast / s1.Gamma ();
			b1.dtau = k.tau[1], b1.dir = k.dir[1];
			trip.burns.push_back (b1);
		}
		trip.properDv = a * (k.tau[0] + (nb == 2 ? k.tau[1] : 0.0));
		trip.ok = Norm (res, m) < 1e-8 && coast >= -1e-9 * std::max (T, 1.0) && k.tau[0] >= 0.0 && (nb == 1 || k.tau[1] >= 0.0);
		// top speed: sampled along the burns
		trip.vmax = v0.length ();
		for (int i = 1; i <= 32; i++) {
			State s = TripState (spec, trip, trip.tauArrive * i / 32.0);
			trip.vmax = std::max (trip.vmax, s.V ().length ());
		}
		for (const Burn &b : trip.burns)
			trip.vmax = std::max (trip.vmax, TripState (spec, trip, b.tau0 + b.dtau).V ().length ());
		return trip;
	}

	State TripState (const TripSpec &spec, const Trip &trip, double tau)
	{
		State s = spec.start;
		double t0 = s.tau;
		auto steps = [&](double dtau) {
			return std::max (4, (int)std::ceil (spec.accel * dtau / C / 0.005));
		};
		auto coast = [&](double dtau) {
			// (proper time dtau at constant velocity)
			double g = s.Gamma ();
			s.x += s.u * dtau;
			s.t += g * dtau;
			s.tau += dtau;
		};
		tau = std::max (tau, 0.0);
		for (size_t i = 0; i < trip.burns.size (); i++) {
			const Burn &b = trip.burns[i];
			double start = t0 + b.tau0;
			if (s.tau < start) coast (std::min (start, t0 + tau) - s.tau);
			if (t0 + tau <= start) return s;
			double dt = std::min (b.dtau, t0 + tau - start);
			Propagate (s, b.dir * spec.accel, dt, nullptr, steps (dt));
			if (dt < b.dtau) return s;
		}
		if (t0 + tau > s.tau) coast (t0 + tau - s.tau);
		return s;
	}

	// ===================================================================
	// Rocket cost

	RocketCost Rocket (double properDv, double w, double m1)
	{
		RocketCost rc;
		if (w <= 0.0) return rc;
		rc.massRatio = std::exp (properDv / w);
		rc.propellant = m1 * (rc.massRatio - 1.0);
		double k = w >= C ? 1.0 : 1.0 - std::sqrt (1.0 - w * w / C2);
		rc.energy = rc.propellant * C2 * k;
		return rc;
	}

} // namespace relativity
