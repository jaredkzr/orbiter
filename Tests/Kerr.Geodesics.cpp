// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Rotating black holes (Src/Orbiter/Kerr.cpp): the textbook radii, the
// metric against Boyer-Lindquist's, Hamilton's equations against the
// Hamiltonian's gradient, circular orbits (period, stability about the
// ISCO), the capture of light at the photon orbits' impact parameters,
// weak deflection, conserved quantities (Carter's constant), free fall
// through the horizon, observers' frames.

#include "Kerr.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <random>

using namespace kerr;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {
	const double PI = 3.14159265358979323846;

	double Hamiltonian (const Hole &h, const double *y)
	{
		return 0.5 * h.Norm (y + 1, y + 4);
	}

	// inverse Boyer-Lindquist metric, (t, r, theta, phi)
	void InverseBL (const Hole &h, double r, double th, double g[4][4])
	{
		double M = h.M, a = h.a, s2 = std::sin (th) * std::sin (th), c2 = 1.0 - s2;
		double Sg = r * r + a * a * c2, D = r * r - 2.0 * M * r + a * a, ra = r * r + a * a;
		for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) g[i][j] = 0.0;
		g[0][0] = -(ra * ra - a * a * D * s2) / (Sg * D);
		g[0][3] = g[3][0] = -2.0 * M * a * r / (Sg * D);
		g[3][3] = (D - a * a * s2) / (Sg * D * s2);
		g[1][1] = D / Sg;
		g[2][2] = 1.0 / Sg;
	}

	// a photon far out at (x0, y0, 0) heading along +x with energy 1
	State Photon (const Hole &h, double x0, double y0)
	{
		State s;
		s.x[1] = x0; s.x[2] = y0;
		double f, l[4];
		h.NullForm (s.x + 1, f, l);
		// -1 + px^2 - f (1 + l1 px)^2 = 0
		double A = 1.0 - f * l[1] * l[1], B = -2.0 * f * l[1], C = -(1.0 + f);
		s.p[0] = -1.0;
		s.p[1] = (-B + std::sqrt (B * B - 4.0 * A * C)) / (2.0 * A);
		return s;
	}

	// photon with axial angular momentum L (E = 1) coming in along +x
	State PhotonL (const Hole &h, double x0, double L)
	{
		double y = -L;
		for (int i = 0; i < 4; i++) y = -L / Photon (h, x0, y).p[1];
		return Photon (h, x0, y);
	}

	// true if a photon is captured (crosses the horizon), false if it gets
	// back out to rOut
	bool Captured (const Hole &h, State s, double rOut)
	{
		Integrator opt;
		opt.tol = 1e-12;
		double rh = h.Horizon ();
		bool in = false;
		opt.stop = [&] (const State &st) {
			double r = h.R (st.x + 1);
			if (r < rh * 0.999) { in = true; return true; }
			return r > rOut && st.x[1] > 0.0;
		};
		double step = 0.0;
		Integrate (h, s, 1e9, opt, step);
		return in;
	}
}

TEST_CASE ("Radii", "[radii]")
{
	Hole s (1.0, 0.0);
	CHECK_THAT (s.Horizon (), WithinRel (2.0, 1e-15));
	CHECK_THAT (s.ISCO (true), WithinRel (6.0, 1e-14));
	CHECK_THAT (s.ISCO (false), WithinRel (6.0, 1e-14));
	CHECK_THAT (s.PhotonOrbit (true), WithinRel (3.0, 1e-14));
	CHECK_THAT (s.PhotonImpact (true), WithinRel (std::sqrt (27.0), 1e-14));
	CHECK_THAT (s.Ergosphere (0.3), WithinRel (2.0, 1e-15));

	Hole x (1.0, 1.0); // extremal
	CHECK_THAT (x.Horizon (), WithinRel (1.0, 1e-15));
	CHECK_THAT (x.ISCO (true), WithinAbs (1.0, 1e-6));
	CHECK_THAT (x.ISCO (false), WithinRel (9.0, 1e-14));
	CHECK_THAT (x.PhotonOrbit (true), WithinAbs (1.0, 1e-6));
	CHECK_THAT (x.PhotonOrbit (false), WithinRel (4.0, 1e-14));
	CHECK_THAT (x.PhotonImpact (false), WithinRel (-7.0, 1e-12)); // (-a + 6 cos (acos (a)/3)... = -7 at a = 1)

	Hole k (1.0, 0.9);
	CHECK_THAT (k.Horizon (), WithinRel (1.0 + std::sqrt (0.19), 1e-15));
	CHECK_THAT (k.InnerHorizon (), WithinRel (1.0 - std::sqrt (0.19), 1e-14));
	CHECK_THAT (k.ISCO (true), WithinAbs (2.3209, 1e-4));
	CHECK_THAT (k.ISCO (false), WithinAbs (8.7174, 1e-4));
	CHECK_THAT (k.Ergosphere (0.0), WithinRel (2.0, 1e-15));
	// the photon orbits' impact parameters (Chandrasekhar): b = -a +- 6 M cos (acos (-+a/M) / 3)
	CHECK_THAT (k.PhotonImpact (true), WithinRel (-0.9 + 6.0 * std::cos (std::acos (-0.9) / 3.0), 1e-12));
	CHECK_THAT (k.PhotonImpact (false), WithinRel (-0.9 - 6.0 * std::cos (std::acos (0.9) / 3.0), 1e-12));

	// circular orbits: the marginally stable one is where dE/dr = 0
	for (double a : {0.0, 0.5, 0.9, 0.998})
		for (bool pro : {true, false}) {
			Hole h (1.0, a);
			double r = h.ISCO (pro), E0, E1, L, W;
			REQUIRE (h.CircularOrbit (r * 0.999, pro, E0, L, W));
			REQUIRE (h.CircularOrbit (r * 1.001, pro, E1, L, W));
			CHECK (std::fabs (E1 - E0) < 1e-6);
		}

	Hole si = Hole::FromSI (1.32712440018e20 / 6.67430e-11, 0.0); // (the Sun: GM)
	CHECK_THAT (si.Horizon (), WithinRel (2953.25, 1e-5)); // its Schwarzschild radius [m]
}

TEST_CASE ("Metric", "[metric]")
{
	std::mt19937 rng (3);
	std::uniform_real_distribution<double> U (-6.0, 6.0);
	for (double a : {0.0, 0.6, 0.9, 1.0}) {
		Hole h (1.0, a);
		for (int n = 0; n < 200; n++) {
			double x[3] = {U (rng), U (rng), U (rng)};
			if (h.R (x) < 0.05) continue;
			double g[4][4], gi[4][4];
			h.Metric (x, g);
			h.InverseMetric (x, gi);
			for (int i = 0; i < 4; i++)
				for (int j = 0; j < 4; j++) {
					double s = 0.0;
					for (int k = 0; k < 4; k++) s += g[i][k] * gi[k][j];
					CHECK (std::fabs (s - (i == j ? 1.0 : 0.0)) < 1e-9);
				}
			// the spheroidal radius's defining relation
			double r = h.R (x);
			CHECK (std::fabs ((x[0] * x[0] + x[1] * x[1]) / (r * r + a * a) + x[2] * x[2] / (r * r) - 1.0) < 1e-9);

			// Hamilton's equations against the Hamiltonian's gradient
			double y[8] = {0, x[0], x[1], x[2], -1.0, U (rng) * 0.2, U (rng) * 0.2, U (rng) * 0.2}, dy[8];
			h.Derivs (y, dy);
			for (int i = 0; i < 4; i++) {
				const double e = 1e-6;
				double yp[8], ym[8];
				for (int k = 0; k < 8; k++) yp[k] = ym[k] = y[k];
				// dx/dlambda = dH/dp
				yp[4 + i] += e; ym[4 + i] -= e;
				double dHdp = (Hamiltonian (h, yp) - Hamiltonian (h, ym)) / (2.0 * e);
				CHECK (std::fabs (dHdp - dy[i]) < 1e-6 * (1.0 + std::fabs (dy[i])));
				if (i == 0) continue;
				for (int k = 0; k < 8; k++) yp[k] = ym[k] = y[k];
				yp[i] += e; ym[i] -= e;
				double dHdx = (Hamiltonian (h, yp) - Hamiltonian (h, ym)) / (2.0 * e);
				CHECK (std::fabs (-dHdx - dy[4 + i]) < 1e-5 * (1.0 + std::fabs (dy[4 + i])));
			}
		}

		// Boyer-Lindquist states keep their norm
		std::uniform_real_distribution<double> R (1.0, 12.0), T (0.2, 2.9), P (-1.0, 1.0);
		for (int n = 0; n < 200; n++) {
			double r = h.Horizon () + R (rng), th = T (rng), ph = P (rng) * PI;
			double p[4] = {-1.0 + 0.1 * P (rng), P (rng), P (rng) * 3.0, P (rng) * 4.0};
			double gi[4][4], nBL = 0.0;
			InverseBL (h, r, th, gi);
			for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) nBL += gi[i][j] * p[i] * p[j];
			State s = h.FromBoyerLindquist (r, th, ph, p);
			CHECK_THAT (h.R (s.x + 1), WithinRel (r, 1e-12));
			CHECK (std::fabs (h.Norm (s.x + 1, s.p) - nBL) < 1e-9 * (1.0 + std::fabs (nBL)));
			CHECK_THAT (h.AxialMomentum (s), WithinAbs (p[3], 1e-9));
		}
	}
}

TEST_CASE ("Circular orbits", "[orbits]")
{
	for (double a : {0.0, 0.9})
		for (bool pro : {true, false})
			for (double rf : {1.05, 2.0}) {
				Hole h (1.0, a);
				double r = h.ISCO (pro) * rf, E, L, W;
				REQUIRE (h.CircularOrbit (r, pro, E, L, W));
				double pBL[4] = {-E, 0.0, 0.0, L};
				State s = h.FromBoyerLindquist (r, PI / 2, 0.0, pBL);
				CHECK_THAT (h.Norm (s.x + 1, s.p), WithinAbs (-1.0, 1e-12));
				double u[4];
				h.Raise (s.x + 1, s.p, u);
				// five orbits in proper time: the coordinate time and the
				// azimuth come back round (constant offsets from BL's)
				const int N = 5;
				double T = N * 2.0 * PI / std::fabs (W);
				double phi0 = std::atan2 (s.x[2], s.x[1]);
				Integrator opt;
				opt.tol = 1e-12;
				double rmin = 1e9, rmax = 0.0;
				opt.stop = [&] (const State &st) {
					double rr = h.R (st.x + 1);
					rmin = std::min (rmin, rr); rmax = std::max (rmax, rr);
					return false;
				};
				double step = 0.0;
				REQUIRE (Integrate (h, s, T / u[0], opt, step) == GEO_DONE);
				CHECK_THAT (s.x[0], WithinRel (T, 1e-8));
				double dphi = std::remainder (std::atan2 (s.x[2], s.x[1]) - phi0, 2.0 * PI);
				CHECK (std::fabs (dphi) < 1e-6);
				CHECK ((rmax - rmin) / r < 1e-7);
				CHECK (std::fabs (s.x[3]) < 1e-9);
				CHECK_THAT (h.Energy (s), WithinRel (E, 1e-12));
				CHECK_THAT (h.AxialMomentum (s), WithinRel (L, 1e-9));
				CHECK_THAT (h.Norm (s.x + 1, s.p), WithinAbs (-1.0, 1e-9));
			}
}

TEST_CASE ("Stability about the ISCO", "[orbits]")
{
	// a circular orbit nudged inwards (p_r, the energy raised to keep the
	// mass): just outside the ISCO it stays, just inside it plunges
	for (double a : {0.0, 0.9}) {
		Hole h (1.0, a);
		for (double rf : {1.03, 0.97}) {
			double r = h.ISCO (true) * rf, E, L, W;
			REQUIRE (h.CircularOrbit (r, true, E, L, W));
			double pBL[4] = {0.0, -1e-4, 0.0, L};
			// p_t for the norm: g^tt pt^2 + 2 g^tphi pt L + g^phiphi L^2 + g^rr pr^2 = -1
			double gi[4][4];
			InverseBL (h, r, PI / 2, gi);
			double A = gi[0][0], B = 2.0 * gi[0][3] * L, C = gi[3][3] * L * L + gi[1][1] * pBL[1] * pBL[1] + 1.0;
			pBL[0] = (-B + std::sqrt (B * B - 4.0 * A * C)) / (2.0 * A);
			CHECK (-pBL[0] > E);
			State s = h.FromBoyerLindquist (r, PI / 2, 0.0, pBL);
			Integrator opt;
			opt.tol = 1e-11;
			double rh = h.Horizon (), rmin = r;
			opt.stop = [&] (const State &st) {
				double rr = h.R (st.x + 1);
				rmin = std::min (rmin, rr);
				return rr < rh;
			};
			double step = 0.0;
			Result res = Integrate (h, s, 20.0 * 2.0 * PI / W, opt, step);
			if (rf > 1.0) {
				CHECK (res == GEO_DONE);
				CHECK (rmin > 0.9 * r);
			} else
				CHECK (res == GEO_STOPPED);
		}
	}
}

TEST_CASE ("Capture of light", "[light]")
{
	// light with less than the photon orbit's impact parameter falls in,
	// with more it escapes (equatorial, with and against the spin)
	for (double a : {0.0, 0.9, 0.998}) {
		Hole h (1.0, a);
		for (bool pro : {true, false}) {
			double b = h.PhotonImpact (pro);
			CHECK (Captured (h, PhotonL (h, -1e4, b * 0.999), 2e4));
			CHECK_FALSE (Captured (h, PhotonL (h, -1e4, b * 1.001), 2e4));
		}
	}
}

TEST_CASE ("Weak deflection", "[light]")
{
	// equatorial light passing at b >> M: 4M/b + 15 pi M^2 / (4 b^2)
	// + 128 M^3 / (3 b^3) -+ 4 a M / b^2 (spin terms of third order left out)
	for (double a : {0.0, 0.9})
		for (int dir : {1, -1}) {
			Hole h (1.0, a);
			const double b = 1000.0, X = 1e7;
			State s = PhotonL (h, -X, dir * b);
			Integrator opt;
			opt.tol = 1e-13;
			opt.stop = [&] (const State &st) { return st.x[1] > X; };
			double step = 0.0;
			REQUIRE (Integrate (h, s, 1e9, opt, step) == GEO_STOPPED);
			double alpha = std::atan2 (dir * s.p[2], s.p[1]);
			double expect = 4.0 / b + 15.0 * PI / (4.0 * b * b) + 128.0 / (3.0 * b * b * b) - dir * 4.0 * a / (b * b);
			INFO ("a " << a << " dir " << dir << " alpha " << alpha << " expect " << expect);
			CHECK (std::fabs (alpha - expect) < (a == 0.0 ? 1e-9 : 1e-7));
		}
}

TEST_CASE ("Conserved quantities", "[orbits]")
{
	// an inclined, eccentric orbit about a fast hole: E, L_z, Carter's Q, H
	Hole h (1.0, 0.9);
	double x[3] = {8.0, 0.0, 1.0}, v[3] = {0.05, 0.28, 0.17}, u[4], p[4];
	REQUIRE (h.CoordinateVelocity (x, v, u));
	h.Lower (x, u, p);
	State s;
	for (int i = 0; i < 3; i++) s.x[1 + i] = x[i];
	for (int i = 0; i < 4; i++) s.p[i] = p[i];
	double E = h.Energy (s), L = h.AxialMomentum (s), Q = h.Carter (s);
	CHECK (Q > 1.0);
	Integrator opt;
	opt.tol = 1e-12;
	double qmax = 0.0, zmax = 0.0;
	opt.stop = [&] (const State &st) {
		if (std::fabs (st.x[3]) > 0.3)
			qmax = std::max (qmax, std::fabs (h.Carter (st) - Q) / Q);
		zmax = std::max (zmax, std::fabs (st.x[3]));
		return false;
	};
	double step = 0.0;
	REQUIRE (Integrate (h, s, 3000.0, opt, step) == GEO_DONE);
	CHECK (zmax > 3.0); // (it does leave the plane)
	CHECK_THAT (h.Energy (s), WithinRel (E, 1e-12));
	CHECK_THAT (h.AxialMomentum (s), WithinRel (L, 1e-8));
	CHECK (qmax < 1e-7);
	CHECK_THAT (h.Norm (s.x + 1, s.p), WithinAbs (-1.0, 1e-8));
}

TEST_CASE ("Free fall through the horizon", "[horizon]")
{
	// from rest at R (Schwarzschild): r = R/2 (1 + cos eta),
	// tau = sqrt (R^3 / 8M) (eta + sin eta); the coordinates stay regular
	Hole h (1.0, 0.0);
	const double R0 = 10.0;
	double pBL[4] = {-std::sqrt (1.0 - 2.0 / R0), 0.0, 0.0, 0.0};
	State s = h.FromBoyerLindquist (R0, PI / 2, 0.0, pBL);
	for (double rt : {2.0, 0.5}) {
		State st = s;
		Integrator opt;
		opt.tol = 1e-12;
		opt.stop = [&] (const State &q) { return h.R (q.x + 1) <= rt; };
		double step = 0.0;
		// (affine parameter = proper time: integrate in small pieces to find tau at rt)
		double tau = 0.0, dt = 0.5;
		Result res = GEO_DONE;
		while (res == GEO_DONE) {
			State prev = st;
			res = Integrate (h, st, dt, opt, step);
			if (res == GEO_STOPPED) {
				// bisect within the last piece
				double lo = 0.0, hi = dt;
				for (int k = 0; k < 50; k++) {
					double mid = 0.5 * (lo + hi);
					State m = prev;
					Integrator o2; o2.tol = 1e-12;
					double st2 = 0.0;
					Integrate (h, m, mid, o2, st2);
					(h.R (m.x + 1) > rt ? lo : hi) = mid;
				}
				tau += 0.5 * (lo + hi);
			} else
				tau += dt;
			REQUIRE (tau < 100.0);
		}
		double eta = std::acos (2.0 * rt / R0 - 1.0);
		CHECK_THAT (tau, WithinRel (std::sqrt (R0 * R0 * R0 / 8.0) * (eta + std::sin (eta)), 1e-8));
		CHECK_THAT (h.Norm (st.x + 1, st.p), WithinAbs (-1.0, 1e-8));
	}
}

TEST_CASE ("Observers", "[frames]")
{
	Hole h (1.0, 0.9);
	double u[4];
	// (equatorial: r^2 = x^2 - a^2 there)
	double o2[3] = {std::sqrt (2.1 * 2.1 + 0.81), 0.0, 0.0}, e2[3] = {std::sqrt (1.9 * 1.9 + 0.81), 0.0, 0.0};
	CHECK (h.StaticObserver (o2, u));
	CHECK_FALSE (h.StaticObserver (e2, u));
	// frames orthonormal, outside, in the ergosphere, inside the horizon
	for (double r : {20.0, 1.9, 1.2, 0.5}) {
		double x[3] = {std::sqrt (r * r + 0.81) * 0.8, 0.3, 0.6 * r};
		double e[4][4];
		h.NormalObserver (x, u);
		double p[4];
		h.Lower (x, u, p);
		CHECK_THAT (h.Norm (x, p), WithinAbs (-1.0, 1e-12));
		h.Tetrad (x, u, e);
		for (int i = 0; i < 4; i++)
			for (int j = 0; j < 4; j++)
				CHECK (std::fabs (h.Dot (x, e[i], e[j]) - (i == j ? (i == 0 ? -1.0 : 1.0) : 0.0)) < 1e-12);
		CHECK (u[0] > 0.0); // (future pointing)
	}
}

TEST_CASE ("Powered flight", "[flight]")
{
	// hovering (Schwarzschild): the static observer's proper acceleration
	// M / (r^2 sqrt (1 - 2M/r)) outward holds it; its clock runs at
	// sqrt (1 - 2M/r)
	{
		Hole h (1.0, 0.0);
		const double r = 10.0, acc = 1.0 / (r * r * std::sqrt (1.0 - 2.0 / r));
		State s;
		s.x[1] = r;
		double u[4];
		REQUIRE (h.StaticObserver (s.x + 1, u));
		h.Lower (s.x + 1, u, s.p);
		Integrator opt;
		opt.tol = 1e-12;
		double step = 0.0, dtau;
		Thrust hover = [&] (double, const double *, const double *, double *al) { al[0] = acc; al[1] = al[2] = 0.0; };
		REQUIRE (Fly (h, s, 1000.0, opt, step, hover, dtau) == GEO_DONE);
		CHECK_THAT (s.x[0], WithinRel (1000.0, 1e-12));
		CHECK (std::fabs (s.x[1] - r) < 1e-7);
		CHECK_THAT (dtau, WithinRel (1000.0 * std::sqrt (1.0 - 2.0 / r), 1e-9));
	}
	// flat space (a negligible hole far away): hyperbolic motion from rest,
	// x = (sqrt (1 + (a t)^2) - 1) / a, tau = asinh (a t) / a
	{
		Hole h (1e-12, 0.0);
		State s;
		s.x[1] = 1e3;
		s.p[0] = -1.0;
		Integrator opt;
		opt.tol = 1e-12;
		opt.rFrac = 1e9;
		double step = 0.0, dtau;
		Thrust burn = [&] (double, const double *, const double *, double *al) { al[0] = 1.0; al[1] = al[2] = 0.0; };
		REQUIRE (Fly (h, s, 5.0, opt, step, burn, dtau) == GEO_DONE);
		CHECK_THAT (s.x[1] - 1e3, WithinRel (std::sqrt (26.0) - 1.0, 1e-9));
		CHECK_THAT (dtau, WithinRel (std::asinh (5.0), 1e-9));
	}
	// across the motion at speed v: the coordinate acceleration is
	// alpha / gamma^2 (the boost frame's axes are the coordinate axes)
	{
		Hole h (1e-12, 0.0);
		State s;
		s.x[1] = 1e3;
		double v[3] = {0.8, 0.0, 0.0}, u[4];
		REQUIRE (h.CoordinateVelocity (s.x + 1, v, u));
		h.Lower (s.x + 1, u, s.p);
		Integrator opt;
		opt.tol = 1e-13;
		opt.rFrac = 1e9;
		double step = 0.0, dtau;
		Thrust side = [&] (double, const double *, const double *, double *al) { al[0] = 0.0; al[1] = 0.01; al[2] = 0.0; };
		const double T = 1e-2;
		REQUIRE (Fly (h, s, T, opt, step, side, dtau) == GEO_DONE);
		h.Raise (s.x + 1, s.p, u);
		CHECK_THAT (u[2] / u[0] / T, WithinRel (0.01 * 0.36, 1e-6));
		CHECK_THAT (u[1] / u[0], WithinRel (0.8, 1e-6));
	}
}

TEST_CASE ("Tides", "[tides]")
{
	// at rest outside a hole not turning: 2M/r^3 along r, -M/r^3 across
	{
		Hole h (1.0, 0.0);
		const double r = 7.0;
		double x[3] = {r, 0.0, 0.0}, u[4], e[4][4], T[3][3];
		REQUIRE (h.StaticObserver (x, u));
		h.Tetrad (x, u, e);
		h.Tidal (x, e, T);
		CHECK_THAT (T[0][0], WithinRel (2.0 / (r * r * r), 1e-6));
		CHECK_THAT (T[1][1], WithinRel (-1.0 / (r * r * r), 1e-6));
		CHECK_THAT (T[2][2], WithinRel (-1.0 / (r * r * r), 1e-6));
		CHECK (std::fabs (T[0][1]) < 1e-9);
	}
	// at rest on the spin axis (the principal frame there): along the axis
	// 2M (r^3 - 3 r a^2) / (r^2 + a^2)^3 (-2 Re Psi2, Psi2 = -M / (r - i a)^3),
	// across it half that, negative
	{
		Hole h (1.0, 0.9);
		const double r = 6.0, a = 0.9;
		double x[3] = {0.0, 0.0, r}, u[4], e[4][4], T[3][3];
		REQUIRE (h.StaticObserver (x, u));
		h.Tetrad (x, u, e);
		h.Tidal (x, e, T);
		double ez = 2.0 * (r * r * r - 3.0 * r * a * a) / std::pow (r * r + a * a, 3.0);
		CHECK_THAT (T[2][2], WithinRel (ez, 1e-6));
		CHECK_THAT (T[0][0], WithinRel (-0.5 * ez, 1e-6));
		CHECK_THAT (T[1][1], WithinRel (-0.5 * ez, 1e-6));
	}
	// the tidal tensor is traceless in vacuum, for any observer, also
	// inside the horizon; falling radially (Schwarzschild) it is the same
	// as at rest (radial boosts leave it unchanged)
	{
		Hole h (1.0, 0.9);
		for (double r : {12.0, 2.5, 1.2, 0.8}) {
			double x[3] = {r * 0.6, r * 0.5, r * 0.62}, u[4], e[4][4], T[3][3];
			h.NormalObserver (x, u);
			h.Tetrad (x, u, e);
			h.Tidal (x, e, T);
			double tr = T[0][0] + T[1][1] + T[2][2], sc = std::fabs (T[0][0]) + std::fabs (T[1][1]) + std::fabs (T[2][2]);
			CHECK (std::fabs (tr) < 1e-6 * sc);
			CHECK (std::fabs (T[0][1] - T[1][0]) < 1e-6 * sc);
		}
		Hole s (1.0, 0.0);
		const double r = 5.0;
		double x[3] = {r, 0.0, 0.0}, e[4][4], T[3][3];
		// (falling from rest at 10 M, at r = 5: E fixed, p_r from the norm)
		double E = std::sqrt (1.0 - 2.0 / 10.0);
		double prBL = -std::sqrt (E * E - (1.0 - 2.0 / r)) / (1.0 - 2.0 / r);
		double pb[4] = {-E, prBL, 0.0, 0.0};
		State st = s.FromBoyerLindquist (r, PI / 2, 0.0, pb);
		double u[4];
		s.Raise (st.x + 1, st.p, u);
		s.BoostFrame (x, u, e);
		s.Tidal (x, e, T);
		CHECK_THAT (T[0][0], WithinRel (2.0 / (r * r * r), 1e-5));
		CHECK_THAT (T[1][1], WithinRel (-1.0 / (r * r * r), 1e-5));
	}
}

TEST_CASE ("Thin disc", "[disc]")
{
	// far out, Newton's: F -> 3 Mdot M / (8 pi r^3) (the shape -> 1/r^3, as
	// 1 - sqrt (r_ISCO / r)); none within the ISCO
	for (double a : {0.0, 0.9}) {
		Hole h (1.0, a);
		double r = 1e8;
		CHECK_THAT (h.DiscFluxShape (r), WithinRel (1.0 / (r * r * r), 1e-3));
		CHECK (h.DiscFluxShape (h.ISCO () * 0.99) == 0.0);
		CHECK (h.DiscFluxShape (h.ISCO () * 1.5) > 0.0);
	}
	// the luminosity at infinity, both faces, each element's light carried
	// out with its energy E (int 4 pi r F E dr), is the binding energy of the
	// ISCO per unit of mass accreted: 1 - E_ISCO (5.72%, a = 0.9: 15.6%)
	for (double a : {0.0, 0.9}) {
		Hole h (1.0, a);
		double E0, L0, W0;
		h.CircularOrbit (h.ISCO (), true, E0, L0, W0);
		double sum = 0.0, d = 1e-3;
		for (double lr = std::log (h.ISCO ()); lr < std::log (1e7); lr += d) {
			double r = std::exp (lr), r1 = std::exp (lr + d), rm = 0.5 * (r + r1), E, L, W;
			h.CircularOrbit (rm, true, E, L, W);
			sum += 4.0 * PI * rm * 3.0 / (8.0 * PI) * h.DiscFluxShape (rm) * E * (r1 - r);
		}
		INFO ("a " << a << " efficiency " << sum << " expected " << 1.0 - E0);
		CHECK (std::fabs (sum - (1.0 - E0)) < 1e-3 * (1.0 - E0));
	}
}
