// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Conics.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace transfer {

// =======================================================================
// Lambert's problem: D. Izzo, "Revisiting Lambert's problem", Celestial
// Mechanics and Dynamical Astronomy 121 (2015): Householder iterations on
// x, time of flight by Lagrange, Battin or Lancaster expressions. With N
// full revolutions there are two solutions (left and right of the least
// time of flight xM) if the time of flight is long enough.

namespace {

	double Hypergeometric (double z)
	{
		double s = 1.0, c = 1.0, err = 1.0;
		for (int j = 0; err > 1e-11 && j < 1000; j++) {
			c = c * (3.0 + j) * (1.0 + j) / (2.5 + j) * z / (j + 1.0);
			s += c;
			err = fabs (c);
		}
		return s;
	}

	struct Izzo {
		double lambda;
		int N = 0;

		double TofLagrange (double x) const
		{
			double a = 1.0 / (1.0 - x*x);
			if (a > 0.0) {
				double alfa = 2.0 * acos (x);
				double beta = 2.0 * asin (sqrt (lambda*lambda / a));
				if (lambda < 0.0) beta = -beta;
				return a * sqrt (a) * ((alfa - sin (alfa)) - (beta - sin (beta)) + Pi2 * N) / 2.0;
			}
			double alfa = 2.0 * acosh (x);
			double beta = 2.0 * asinh (sqrt (-lambda*lambda / a));
			if (lambda < 0.0) beta = -beta;
			return -a * sqrt (-a) * ((beta - sinh (beta)) - (alfa - sinh (alfa))) / 2.0;
		}

		double Tof (double x) const
		{
			double dist = fabs (x - 1.0);
			if (dist < 0.2 && dist > 0.01) return TofLagrange (x);
			double K = lambda*lambda, E = x*x - 1.0, rho = fabs (E), z = sqrt (1.0 + K*E);
			if (dist < 0.01) { // Battin series
				double eta = z - lambda * x;
				double S1 = 0.5 * (1.0 - lambda - x * eta);
				double Q = 4.0 / 3.0 * Hypergeometric (S1);
				return (eta*eta*eta * Q + 4.0 * lambda * eta) / 2.0 + N * Pi / pow (rho, 1.5);
			}
			double y = sqrt (rho), g = x * z - lambda * E, d;
			if (E < 0.0) d = N * Pi + acos (std::clamp (g, -1.0, 1.0));
			else d = log (y * (z - lambda * x) + g);
			return (x - lambda * z - d / y) / E;
		}

		void Derivs (double x, double T, double &DT, double &DDT, double &DDDT) const
		{
			double l2 = lambda*lambda, l3 = l2*lambda;
			double umx2 = 1.0 - x*x;
			double y = sqrt (1.0 - l2 * umx2), y2 = y*y, y3 = y2*y;
			DT = 1.0 / umx2 * (3.0 * T * x - 2.0 + 2.0 * l3 * x / y);
			DDT = 1.0 / umx2 * (3.0 * T + 5.0 * x * DT + 2.0 * (1.0 - l2) * l3 / y3);
			DDDT = 1.0 / umx2 * (7.0 * x * DDT + 8.0 * DT - 6.0 * (1.0 - l2) * l2 * l3 * x / y3 / y2);
		}

		double Householder (double T, double x, double tol, int maxit) const
		{
			for (int it = 0; it < maxit; it++) {
				double tof = Tof (x), DT, DDT, DDDT;
				Derivs (x, tof, DT, DDT, DDDT);
				double delta = tof - T, DT2 = DT*DT;
				double xn = x - delta * (DT2 - delta * DDT / 2.0) / (DT * (DT2 - delta * DDT) + DDDT * delta*delta / 6.0);
				if (!std::isfinite (xn)) return std::numeric_limits<double>::quiet_NaN();
				double err = fabs (x - xn);
				x = xn;
				if (err < tol) break;
			}
			return x;
		}

		// x of the solution: zero revolutions, or N with branch 0 (left) or 1 (right);
		// NaN if there is none
		double Solve (double T, int branch) const
		{
			const double NaN = std::numeric_limits<double>::quiet_NaN();
			double l2 = lambda*lambda, l3 = l2*lambda;
			double T00 = acos (lambda) + lambda * sqrt (1.0 - l2);
			if (N == 0) {
				double T1 = 2.0 / 3.0 * (1.0 - l3);
				double x;
				if (T >= T00) x = -(T - T00) / (T - T00 + 4.0);
				else if (T <= T1) x = T1 * (T1 - T) / (0.4 * (1.0 - l2 * l3) * T) + 1.0;
				else x = pow (T / T00, 0.69314718055994529 / log (T1 / T00)) - 1.0;
				return Householder (T, x, 1e-11, 30);
			}
			if (T < N * Pi) return NaN;
			// the least time of flight with N revolutions (Halley iterations on dT/dx = 0)
			double T0 = T00 + N * Pi;
			if (T < T0) {
				double x = 0.0, Tmin = T0;
				for (int it = 0; it < 12; it++) {
					double DT, DDT, DDDT;
					Derivs (x, Tmin, DT, DDT, DDDT);
					if (DT == 0.0) break;
					double xn = x - DT * DDT / (DDT * DDT - DT * DDDT / 2.0);
					if (!std::isfinite (xn) || fabs (xn) >= 1.0) break;
					double err = fabs (x - xn);
					x = xn;
					Tmin = Tof (x);
					if (err < 1e-13) break;
				}
				if (Tmin > T) return NaN;
			}
			double x;
			if (branch == 0) {
				double t = pow ((N * Pi + Pi) / (8.0 * T), 2.0 / 3.0);
				x = (t - 1.0) / (t + 1.0);
			} else {
				double t = pow ((8.0 * T) / (N * Pi), 2.0 / 3.0);
				x = (t - 1.0) / (t + 1.0);
			}
			x = Householder (T, x, 1e-9, 20);
			if (!(fabs (x) < 1.0)) return NaN; // (elliptic only)
			return x;
		}
	};

	inline Vector Cross (const Vector &a, const Vector &b) { return crossp (a, b); }
}

bool Lambert (const Vector &R1, const Vector &R2, double tof, double mu, const Vector &nref, bool retro,
	Vector &v1, Vector &v2)
{
	return Lambert (R1, R2, tof, mu, nref, retro, 0, 0, v1, v2);
}

bool Lambert (const Vector &R1, const Vector &R2, double tof, double mu, const Vector &nref, bool retro,
	int revs, int branch, Vector &v1, Vector &v2)
{
	if (tof <= 0.0 || mu <= 0.0 || revs < 0) return false;
	double r1 = R1.length(), r2 = R2.length();
	Vector cv (R2 - R1);
	double c = cv.length();
	if (r1 <= 0.0 || r2 <= 0.0 || c <= 0.0) return false;
	double s = 0.5 * (r1 + r2 + c);
	Vector ir1 (R1 / r1), ir2 (R2 / r2);
	Vector ih (Cross (ir1, ir2));
	double lh = ih.length();
	if (lh < 1e-12) return false; // (collinear: the plane is undefined)
	ih /= lh;
	double lambda2 = 1.0 - c / s;
	double lambda = sqrt (std::max (0.0, lambda2));
	Vector it1, it2;
	if (dotp (ih, nref) < 0.0) { // transfer angle > 180 deg in the sense of nref
		lambda = -lambda;
		it1 = Cross (ir1, ih), it2 = Cross (ir2, ih);
	} else {
		it1 = Cross (ih, ir1), it2 = Cross (ih, ir2);
	}
	it1.unify(), it2.unify();
	if (retro) {
		lambda = -lambda;
		it1 = -it1, it2 = -it2;
	}
	double T = sqrt (2.0 * mu / (s*s*s)) * tof;
	Izzo iz{ lambda, revs };
	double x = iz.Solve (T, branch);
	if (!std::isfinite (x)) return false;
	double gamma = sqrt (mu * s / 2.0);
	double rho = (r1 - r2) / c;
	double sigma = sqrt (std::max (0.0, 1.0 - rho*rho));
	double y = sqrt (1.0 - lambda2 + lambda2 * x*x);
	double vr1 = gamma * ((lambda * y - x) - rho * (lambda * y + x)) / r1;
	double vr2 = -gamma * ((lambda * y - x) + rho * (lambda * y + x)) / r2;
	double vt = gamma * sigma * (y + lambda * x);
	v1 = ir1 * vr1 + it1 * (vt / r1);
	v2 = ir2 * vr2 + it2 * (vt / r2);
	return std::isfinite (v1.x + v1.y + v1.z + v2.x + v2.y + v2.z);
}

Vector DepartureVelocity (const Vector &r, const Vector &vinf, double mu)
{
	// the velocity at r of the hyperbola whose outgoing asymptote is vinf:
	// v = vinf/2 ((D+1) s + (D-1) rhat), D = sqrt (1 + 4 mu / (r vinf^2 (1 + cos psi)))
	double rl = r.length(), vi = vinf.length();
	if (rl <= 0.0 || vi <= 0.0) return vinf;
	Vector rh (r / rl), sh (vinf / vi);
	double cp = std::max (dotp (rh, sh), -1.0 + 1e-9);
	double D = sqrt (1.0 + 4.0 * mu / (rl * vi*vi * (1.0 + cp)));
	return (sh * (D + 1.0) + rh * (D - 1.0)) * (0.5 * vi);
}

Vector BVector (const Vector &r, const Vector &v, double mu, Vector &S, double &vinf)
{
	double rl = r.length(), v2 = dotp (v, v);
	double energy = 0.5 * v2 - mu / rl;
	Vector h (Cross (r, v));
	Vector e ((r * (v2 - mu / rl) - v * dotp (r, v)) / mu);
	double el = e.length();
	if (energy > 0.0 && el > 1.0) {
		vinf = sqrt (2.0 * energy);
		Vector P (e / el), Q (Cross (h, P).unit());
		S = P * (1.0 / el) + Q * sqrt (1.0 - 1.0 / (el*el)); // incoming asymptote
	} else { // (not hyperbolic: the current direction of motion)
		vinf = sqrt (v2);
		S = v / vinf;
	}
	return Cross (S, h) / vinf;
}

// =======================================================================
// Flybys

double FlybyTurn (double vinf, double rp, double mu)
{
	return 2.0 * asin (1.0 / (1.0 + rp * vinf*vinf / mu));
}

double FlybyPeriapsis (double vinf, double turn, double mu)
{
	double s = sin (0.5 * std::clamp (turn, 1e-9, Pi - 1e-9));
	return mu / (vinf*vinf) * (1.0 / s - 1.0);
}

double PoweredFlyby (const Vector &vin, const Vector &vout, double mu, double rpMin, double &rp, double &dvPeri, double &dvTurn)
{
	double a = vin.length(), b = vout.length();
	dvPeri = dvTurn = 0.0;
	rp = 1e30;
	if (a <= 0.0 || b <= 0.0) { dvPeri = fabs (a - b); return dvPeri; }
	double turn = acos (std::clamp (dotp (vin, vout) / (a * b), -1.0, 1.0));
	// the turn of the incoming and outgoing halves at periapsis rp
	auto Turn = [&](double r) {
		return asin (1.0 / (1.0 + r * a*a / mu)) + asin (1.0 / (1.0 + r * b*b / mu));
	};
	if (turn > 1e-9) {
		// (decreasing in rp: bisection on log rp)
		double lo = log (1e-6 * mu / (a*b)), hi = log (1e6 * mu / (a*b));
		for (int it = 0; it < 60; it++) {
			double m = 0.5 * (lo + hi);
			if (Turn (exp (m)) > turn) lo = m; else hi = m;
		}
		rp = exp (0.5 * (lo + hi));
	}
	double r = rp;
	if (r < rpMin) {
		// flown at the least periapsis: the rest of the turn by a burn far out
		r = rpMin;
		double excess = turn - Turn (rpMin);
		if (excess > 0.0) dvTurn = 2.0 * std::min (a, b) * sin (0.5 * excess);
	}
	if (r < 1e29) dvPeri = fabs (sqrt (b*b + 2.0 * mu / r) - sqrt (a*a + 2.0 * mu / r));
	else dvPeri = fabs (a - b);
	return dvPeri + dvTurn;
}

Vector FlybyOut (const Vector &vin, double mu, double rp, double beta, const Vector &vref)
{
	double v = vin.length();
	if (v <= 0.0) return vin;
	double turn = FlybyTurn (v, rp, mu);
	Vector i (vin / v), j (Cross (i, vref)), k;
	if (j.length() < 1e-12) j = Cross (i, Vector (0, 1, 0));
	if (j.length() < 1e-12) j = Cross (i, Vector (1, 0, 0));
	j.unify();
	k = Cross (i, j).unit();
	return (i * cos (turn) + j * (cos (beta) * sin (turn)) + k * (sin (beta) * sin (turn))) * v;
}

} // namespace transfer
