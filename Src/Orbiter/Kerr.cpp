// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Rotating black holes: the Kerr spacetime in Kerr-Schild coordinates (Kerr.h)

#include "Kerr.h"
#include <algorithm>
#include <cmath>

namespace kerr {

namespace {
	const double G_SI = 6.67430e-11;   // [m^3 kg^-1 s^-2] (CODATA 2018)
	const double C_SI = 299792458.0;   // [m/s]

	// the spheroidal radius and its gradient: r^4 - w r^2 - a^2 z^2 = 0,
	// w = x^2 + y^2 + z^2 - a^2; D = 2 r^2 - w = sqrt (w^2 + 4 a^2 z^2)
	struct Radius {
		double r, D;
		double dr[3]; // dr/dx, dr/dy, dr/dz
	};

	Radius Spheroidal (double a, const double *x)
	{
		Radius s;
		double a2 = a * a, z2 = x[2] * x[2];
		double w = x[0] * x[0] + x[1] * x[1] + z2 - a2;
		double D = std::sqrt (w * w + 4.0 * a2 * z2);
		// (each form where it does not cancel)
		double r2 = w >= 0.0 ? 0.5 * (w + D) : (D - w > 0.0 ? 2.0 * a2 * z2 / (D - w) : 0.0);
		s.r = std::sqrt (r2);
		s.D = D;
		if (s.r > 0.0 && D > 0.0) {
			s.dr[0] = s.r * x[0] / D;
			s.dr[1] = s.r * x[1] / D;
			s.dr[2] = (r2 + a2) * x[2] / (s.r * D);
		} else
			s.dr[0] = s.dr[1] = s.dr[2] = 0.0; // (the ring singularity)
		return s;
	}
}

Hole Hole::FromSI (double m, double chi)
{
	double M = G_SI * m / (C_SI * C_SI);
	return Hole (M, chi * M);
}

double Hole::R (const double *x3) const
{
	return Spheroidal (a, x3).r;
}

double Hole::Horizon () const
{
	return M + std::sqrt (std::max (0.0, M * M - a * a));
}

double Hole::InnerHorizon () const
{
	return M - std::sqrt (std::max (0.0, M * M - a * a));
}

double Hole::Ergosphere (double cosTheta) const
{
	return M + std::sqrt (std::max (0.0, M * M - a * a * cosTheta * cosTheta));
}

double Hole::ISCO (bool prograde) const
{
	double chi = std::min (1.0, std::fabs (a) / M);
	double z1 = 1.0 + std::cbrt (1.0 - chi * chi) * (std::cbrt (1.0 + chi) + std::cbrt (1.0 - chi));
	double z2 = std::sqrt (3.0 * chi * chi + z1 * z1);
	double d = std::sqrt (std::max (0.0, (3.0 - z1) * (3.0 + z1 + 2.0 * z2)));
	return M * (3.0 + z2 + (prograde ? -d : d));
}

double Hole::PhotonOrbit (bool prograde) const
{
	double chi = std::min (1.0, std::fabs (a) / M);
	return 2.0 * M * (1.0 + std::cos (2.0 / 3.0 * std::acos (prograde ? -chi : chi)));
}

double Hole::PhotonImpact (bool prograde) const
{
	// L/E of the circular photon orbit (Bardeen, Press & Teukolsky 1972:
	// the ratio of their E and L), for the orbit's spin ab, signed by the
	// orbit's direction
	double r = PhotonOrbit (prograde);
	double dir = (a >= 0.0 ? 1.0 : -1.0) * (prograde ? 1.0 : -1.0);
	double ab = prograde ? std::fabs (a) : -std::fabs (a);
	double sM = std::sqrt (M), sr = std::sqrt (r);
	return dir * sM * (r * r - 2.0 * ab * sM * sr + ab * ab) / (r * sr - 2.0 * M * sr + ab * sM);
}

bool Hole::CircularOrbit (double r, bool prograde, double &E, double &L, double &Omega) const
{
	double dir = (a >= 0.0 ? 1.0 : -1.0) * (prograde ? 1.0 : -1.0);
	double ab = prograde ? std::fabs (a) : -std::fabs (a);
	double sM = std::sqrt (M), sr = std::sqrt (r), r32 = r * sr;
	double q = r32 - 3.0 * M * sr + 2.0 * ab * sM;
	if (q <= 0.0) return false;
	double den = std::pow (r, 0.75) * std::sqrt (q);
	E = (r32 - 2.0 * M * sr + ab * sM) / den;
	L = dir * sM * (r * r - 2.0 * ab * sM * sr + ab * ab) / den;
	Omega = dir * sM / (r32 + ab * sM);
	return true;
}

double Hole::DiscFluxShape (double r) const
{
	// x = sqrt (r/M); the roots of x^3 - 3x + 2a* = 0; Page & Thorne's
	// integral of the energy and angular momentum carried in
	double as = a / M, x = std::sqrt (r / M), x0 = std::sqrt (ISCO (true) / M);
	if (x <= x0) return 0.0;
	double ac = std::acos (std::clamp (as, -1.0, 1.0)), PI = 3.14159265358979323846;
	double x1 = 2.0 * std::cos ((ac - PI) / 3.0), x2 = 2.0 * std::cos ((ac + PI) / 3.0), x3 = -2.0 * std::cos (ac / 3.0);
	auto term = [&] (double xi, double xj, double xk) {
		return 3.0 * (xi - as) * (xi - as) / (xi * (xi - xj) * (xi - xk)) * std::log ((x - xi) / (x0 - xi));
	};
	double br = x - x0 - 1.5 * as * std::log (x / x0) - term (x1, x2, x3) - term (x2, x1, x3) - term (x3, x1, x2);
	return br / (x * x * x * x * (x * x * x - 3.0 * x + 2.0 * as)) / (M * M);
}

// =======================================================================
// The metric

void Hole::NullForm (const double *x3, double &f, double l[4]) const
{
	Radius s = Spheroidal (a, x3);
	double r = s.r, r2 = r * r, ra = r2 + a * a, z = x3[2];
	f = 2.0 * M * r2 * r / (r2 * r2 + a * a * z * z);
	l[0] = 1.0;
	l[1] = (r * x3[0] + a * x3[1]) / ra;
	l[2] = (r * x3[1] - a * x3[0]) / ra;
	l[3] = r > 0.0 ? z / r : 0.0;
}

void Hole::Metric (const double *x3, double g[4][4]) const
{
	double f, l[4];
	NullForm (x3, f, l);
	for (int m = 0; m < 4; m++)
		for (int n = 0; n < 4; n++)
			g[m][n] = (m == n ? (m == 0 ? -1.0 : 1.0) : 0.0) + f * l[m] * l[n];
}

void Hole::InverseMetric (const double *x3, double g[4][4]) const
{
	double f, l[4];
	NullForm (x3, f, l);
	double lu[4] = {-1.0, l[1], l[2], l[3]};
	for (int m = 0; m < 4; m++)
		for (int n = 0; n < 4; n++)
			g[m][n] = (m == n ? (m == 0 ? -1.0 : 1.0) : 0.0) - f * lu[m] * lu[n];
}

double Hole::Norm (const double *x3, const double *p) const
{
	double f, l[4];
	NullForm (x3, f, l);
	double L = -p[0] + l[1] * p[1] + l[2] * p[2] + l[3] * p[3];
	return -p[0] * p[0] + p[1] * p[1] + p[2] * p[2] + p[3] * p[3] - f * L * L;
}

void Hole::Raise (const double *x3, const double *p, double *v) const
{
	double f, l[4];
	NullForm (x3, f, l);
	double L = -p[0] + l[1] * p[1] + l[2] * p[2] + l[3] * p[3];
	v[0] = -p[0] + f * L;
	for (int i = 1; i < 4; i++) v[i] = p[i] - f * L * l[i];
}

void Hole::Lower (const double *x3, const double *v, double *p) const
{
	double f, l[4];
	NullForm (x3, f, l);
	double L = v[0] + l[1] * v[1] + l[2] * v[2] + l[3] * v[3];
	p[0] = -v[0] + f * L;
	for (int i = 1; i < 4; i++) p[i] = v[i] + f * L * l[i];
}

double Hole::Dot (const double *x3, const double *v, const double *w) const
{
	double p[4];
	Lower (x3, v, p);
	return p[0] * w[0] + p[1] * w[1] + p[2] * w[2] + p[3] * w[3];
}

// =======================================================================
// Geodesics

void Hole::Derivs (const double *y, double *dy) const
{
	const double *x = y + 1, *p = y + 4;
	Radius s = Spheroidal (a, x);
	double r = s.r, r2 = r * r, a2 = a * a, ra = r2 + a2, z = x[2];
	double q = r2 * r2 + a2 * z * z;
	double f = 2.0 * M * r2 * r / q;
	double l1 = (r * x[0] + a * x[1]) / ra, l2 = (r * x[1] - a * x[0]) / ra, l3 = z / r;
	double L = -p[0] + l1 * p[1] + l2 * p[2] + l3 * p[3];

	// dx^m/dlambda = g^mn p_n
	dy[0] = -p[0] + f * L;
	dy[1] = p[1] - f * L * l1;
	dy[2] = p[2] - f * L * l2;
	dy[3] = p[3] - f * L * l3;

	// dp_i/dlambda = d_i (f L^2) / 2 = (d_i f) L^2 / 2 + f L (d_i l) . p
	double dfdr = 2.0 * M * r2 * (3.0 * a2 * z * z - r2 * r2) / (q * q);
	double dfdz = -4.0 * M * r2 * r * a2 * z / (q * q);
	for (int i = 0; i < 3; i++) {
		double dr = s.dr[i];
		double dl1 = ((dr * x[0] + (i == 0 ? r : 0.0) + (i == 1 ? a : 0.0)) * ra - (r * x[0] + a * x[1]) * 2.0 * r * dr) / (ra * ra);
		double dl2 = ((dr * x[1] + (i == 1 ? r : 0.0) - (i == 0 ? a : 0.0)) * ra - (r * x[1] - a * x[0]) * 2.0 * r * dr) / (ra * ra);
		double dl3 = (i == 2 ? 1.0 / r : 0.0) - z * dr / r2;
		double df = dfdr * dr + (i == 2 ? dfdz : 0.0);
		dy[5 + i] = 0.5 * df * L * L + f * L * (dl1 * p[1] + dl2 * p[2] + dl3 * p[3]);
	}
	dy[4] = 0.0; // (stationary: p_t conserved)
}

double Hole::Carter (const State &st) const
{
	const double *x = st.x + 1, *p = st.p;
	double r = R (x);
	double ct = r > 0.0 ? x[2] / r : 0.0, st2 = std::max (1e-300, 1.0 - ct * ct), sn = std::sqrt (st2);
	double pth = ct / sn * (x[0] * p[1] + x[1] * p[2]) - r * sn * p[3];
	double mu2 = -Norm (x, p), E = -p[0], Lz = AxialMomentum (st);
	return pth * pth + ct * ct * (a * a * (mu2 - E * E) + Lz * Lz / st2);
}

State Hole::FromBoyerLindquist (double r, double theta, double phi, const double *pBL) const
{
	State s;
	double sn = std::sin (theta), cs = std::cos (theta), sp = std::sin (phi), cp = std::cos (phi);
	s.x[0] = 0.0;
	s.x[1] = sn * (r * cp - a * sp);
	s.x[2] = sn * (r * sp + a * cp);
	s.x[3] = r * cs;
	// covector: t_BL = t - T(r), phi_BL = phi - Phi(r), T' = 2Mr/Delta,
	// Phi' = a/Delta; then from (r, theta, phi) to (x, y, z)
	double delta = r * r - 2.0 * M * r + a * a;
	double pr = pBL[1] - (2.0 * M * r * pBL[0] + a * pBL[3]) / delta;
	Radius rs = Spheroidal (a, s.x + 1);
	double rho2 = s.x[1] * s.x[1] + s.x[2] * s.x[2], ra = r * r + a * a;
	double dth[3] = {0, 0, 0};
	if (std::fabs (sn) > 1e-12)
		for (int i = 0; i < 3; i++)
			dth[i] = -((i == 2 ? 1.0 / r : 0.0) - s.x[3] * rs.dr[i] / (r * r)) / sn;
	double dph[3] = {-s.x[2] / rho2 + a / ra * rs.dr[0], s.x[1] / rho2 + a / ra * rs.dr[1], a / ra * rs.dr[2]};
	s.p[0] = pBL[0];
	for (int i = 0; i < 3; i++)
		s.p[1 + i] = pr * rs.dr[i] + pBL[2] * dth[i] + pBL[3] * dph[i];
	return s;
}

// =======================================================================
// Observers

void Hole::Tetrad (const double *x3, const double *u, double e[4][4]) const
{
	for (int m = 0; m < 4; m++) e[0][m] = u[m];
	for (int k = 1; k < 4; k++) {
		double v[4] = {0, 0, 0, 0};
		v[k] = 1.0;
		for (int b = 0; b < k; b++) {
			double d = Dot (x3, v, e[b]) * (b == 0 ? -1.0 : 1.0);
			for (int m = 0; m < 4; m++) v[m] -= d * e[b][m];
		}
		double n = std::sqrt (Dot (x3, v, v));
		for (int m = 0; m < 4; m++) e[k][m] = v[m] / n;
	}
}

bool Hole::StaticObserver (const double *x3, double *u) const
{
	double f, l[4];
	NullForm (x3, f, l);
	if (f >= 1.0) return false; // (g_tt = f - 1)
	u[0] = 1.0 / std::sqrt (1.0 - f);
	u[1] = u[2] = u[3] = 0.0;
	return true;
}

void Hole::NormalObserver (const double *x3, double *u) const
{
	double f, l[4];
	NullForm (x3, f, l);
	double p[4] = {-1.0 / std::sqrt (1.0 + f), 0, 0, 0};
	Raise (x3, p, u);
}

bool Hole::CoordinateVelocity (const double *x3, const double *v3, double *u) const
{
	double V[4] = {1.0, v3[0], v3[1], v3[2]};
	double n = -Dot (x3, V, V);
	if (n <= 0.0) return false;
	double g = 1.0 / std::sqrt (n);
	for (int m = 0; m < 4; m++) u[m] = g * V[m];
	return true;
}

void Hole::BoostFrame (const double *x3, const double *u, double e[4][4]) const
{
	double n[4], E[4][4];
	NormalObserver (x3, n);
	Tetrad (x3, n, E);
	// the velocity relative to the normal observer: gamma, beta on its axes
	double g = -Dot (x3, u, n), b[3];
	for (int i = 0; i < 3; i++) b[i] = Dot (x3, u, E[1 + i]) / g;
	// pure boost: e_i = E_i + b_i (g E_0 + g^2 / (1 + g) b.E)
	for (int m = 0; m < 4; m++) e[0][m] = u[m];
	for (int i = 0; i < 3; i++)
		for (int m = 0; m < 4; m++) {
			double bE = b[0] * E[1][m] + b[1] * E[2][m] + b[2] * E[3][m];
			e[1 + i][m] = E[1 + i][m] + b[i] * (g * E[0][m] + g * g / (1.0 + g) * bE);
		}
}

void Hole::Christoffel (const double *x3, double G[4][4][4]) const
{
	// dg[d][m][n] = d_d g_mn (d_t: 0), central differences
	double r = std::max (R (x3), M), eps = 1e-5 * r;
	double dg[4][4][4] = {}, gp[4][4], gm[4][4], gi[4][4];
	for (int d = 1; d < 4; d++) {
		double xp[3] = {x3[0], x3[1], x3[2]}, xm[3] = {x3[0], x3[1], x3[2]};
		xp[d - 1] += eps, xm[d - 1] -= eps;
		Metric (xp, gp);
		Metric (xm, gm);
		for (int m = 0; m < 4; m++)
			for (int n = 0; n < 4; n++) dg[d][m][n] = (gp[m][n] - gm[m][n]) / (2.0 * eps);
	}
	InverseMetric (x3, gi);
	for (int a = 0; a < 4; a++)
		for (int b = 0; b < 4; b++)
			for (int c = b; c < 4; c++) {
				double s = 0.0;
				for (int d = 0; d < 4; d++) s += gi[a][d] * (dg[b][d][c] + dg[c][d][b] - dg[d][b][c]);
				G[a][b][c] = G[a][c][b] = 0.5 * s;
			}
}

void Hole::Tidal (const double *x3, const double e[4][4], double T[3][3]) const
{
	// R^a_bcd = d_c G^a_db - d_d G^a_cb + G^a_ce G^e_db - G^a_de G^e_cb
	double r = std::max (R (x3), M), eps = 1e-4 * r;
	static thread_local double dG[4][4][4][4]; // [d][a][b][c] = d_d Gamma^a_bc
	double G[4][4][4], Gp[4][4][4], Gm[4][4][4];
	Christoffel (x3, G);
	for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++) for (int c = 0; c < 4; c++) dG[0][a][b][c] = 0.0;
	for (int d = 1; d < 4; d++) {
		double xp[3] = {x3[0], x3[1], x3[2]}, xm[3] = {x3[0], x3[1], x3[2]};
		xp[d - 1] += eps, xm[d - 1] -= eps;
		Christoffel (xp, Gp);
		Christoffel (xm, Gm);
		for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++) for (int c = 0; c < 4; c++)
			dG[d][a][b][c] = (Gp[a][b][c] - Gm[a][b][c]) / (2.0 * eps);
	}
	// R^a_b c d contracted: Ru[a][c] = R^a_bcd u^b u^d
	const double *u = e[0];
	double Ru[4][4];
	for (int a = 0; a < 4; a++)
		for (int c = 0; c < 4; c++) {
			double s = 0.0;
			for (int b = 0; b < 4; b++)
				for (int d = 0; d < 4; d++) {
					double w = u[b] * u[d];
					if (w == 0.0) continue;
					double R = dG[c][a][d][b] - dG[d][a][c][b];
					for (int k = 0; k < 4; k++) R += G[a][c][k] * G[k][d][b] - G[a][d][k] * G[k][c][b];
					s += R * w;
				}
			Ru[a][c] = s;
		}
	// T_ij = -R_{i0j0} = -(g e_i)_a Ru^a_c e_j^c
	for (int i = 0; i < 3; i++) {
		double ei[4];
		Lower (x3, e[1 + i], ei);
		for (int j = 0; j < 3; j++) {
			double s = 0.0;
			for (int a = 0; a < 4; a++)
				for (int c = 0; c < 4; c++) s += ei[a] * Ru[a][c] * e[1 + j][c];
			T[i][j] = -s;
		}
	}
}

// =======================================================================
// Integration

namespace {
	// Dormand-Prince 5(4)
	const double C2 = 1.0 / 5, C3 = 3.0 / 10, C4 = 4.0 / 5, C5 = 8.0 / 9;
	const double A21 = 1.0 / 5;
	const double A31 = 3.0 / 40, A32 = 9.0 / 40;
	const double A41 = 44.0 / 45, A42 = -56.0 / 15, A43 = 32.0 / 9;
	const double A51 = 19372.0 / 6561, A52 = -25360.0 / 2187, A53 = 64448.0 / 6561, A54 = -212.0 / 729;
	const double A61 = 9017.0 / 3168, A62 = -355.0 / 33, A63 = 46732.0 / 5247, A64 = 49.0 / 176, A65 = -5103.0 / 18656;
	const double B1 = 35.0 / 384, B3 = 500.0 / 1113, B4 = 125.0 / 192, B5 = -2187.0 / 6784, B6 = 11.0 / 84;
	const double E1 = 71.0 / 57600, E3 = -71.0 / 16695, E4 = 71.0 / 1920, E5 = -17253.0 / 339200, E6 = 22.0 / 525, E7 = -1.0 / 40;
}

Result Integrate (const Hole &hole, State &s, double dlambda, Integrator &opt, double &h)
{
	double y[8], k1[8], k2[8], k3[8], k4[8], k5[8], k6[8], k7[8], yt[8], y5[8];
	for (int i = 0; i < 4; i++) { y[i] = s.x[i]; y[4 + i] = s.p[i]; }
	double dir = dlambda >= 0.0 ? 1.0 : -1.0, left = std::fabs (dlambda);
	opt.steps = 0;
	hole.Derivs (y, k1);

	auto limit = [&] (const double *yy, const double *dd) {
		double r = std::max (hole.R (yy + 1), 1e-3 * hole.M);
		// (the rate of the coordinates, time included: a particle at rest moves in t)
		double v = std::sqrt (dd[0] * dd[0] + dd[1] * dd[1] + dd[2] * dd[2] + dd[3] * dd[3]);
		double hl = v > 0.0 ? opt.rFrac * r / v : left;
		if (opt.hmax > 0.0) hl = std::min (hl, opt.hmax);
		return std::make_pair (hl, r / std::max (v, 1e-300));
	};
	auto lim = limit (y, k1);
	if (h <= 0.0) h = 0.1 * lim.first;

	while (left > 0.0) {
		if (opt.steps >= opt.maxSteps) return GEO_STEPS;
		double hs = std::min ({h, lim.first, left});
		if (hs < opt.hmin * lim.second) return GEO_STUCK;
		double hh = dir * hs;
		for (int i = 0; i < 8; i++) yt[i] = y[i] + hh * A21 * k1[i];
		hole.Derivs (yt, k2);
		for (int i = 0; i < 8; i++) yt[i] = y[i] + hh * (A31 * k1[i] + A32 * k2[i]);
		hole.Derivs (yt, k3);
		for (int i = 0; i < 8; i++) yt[i] = y[i] + hh * (A41 * k1[i] + A42 * k2[i] + A43 * k3[i]);
		hole.Derivs (yt, k4);
		for (int i = 0; i < 8; i++) yt[i] = y[i] + hh * (A51 * k1[i] + A52 * k2[i] + A53 * k3[i] + A54 * k4[i]);
		hole.Derivs (yt, k5);
		for (int i = 0; i < 8; i++) yt[i] = y[i] + hh * (A61 * k1[i] + A62 * k2[i] + A63 * k3[i] + A64 * k4[i] + A65 * k5[i]);
		hole.Derivs (yt, k6);
		for (int i = 0; i < 8; i++) y5[i] = y[i] + hh * (B1 * k1[i] + B3 * k3[i] + B4 * k4[i] + B5 * k5[i] + B6 * k6[i]);
		hole.Derivs (y5, k7);

		// error, each component against its scale: positions the distance
		// from the hole, momenta the momentum's size
		double r = std::max (hole.R (y + 1), hole.M);
		double pn = std::sqrt (y[4] * y[4] + y[5] * y[5] + y[6] * y[6] + y[7] * y[7]);
		double err = 0.0;
		for (int i = 0; i < 8; i++) {
			double e = hh * (E1 * k1[i] + E3 * k3[i] + E4 * k4[i] + E5 * k5[i] + E6 * k6[i] + E7 * k7[i]);
			double sc = i < 4 ? std::max (std::fabs (y[i]), r) : std::max (std::fabs (y[i]), pn);
			err = std::max (err, std::fabs (e) / (opt.tol * sc));
		}
		if (!(err <= 1.0)) {
			h = hs * std::max (0.1, 0.9 * std::pow (err > 0.0 ? err : 1e10, -0.2));
			if (!(err == err)) h = 0.1 * hs; // (NaN: far too large)
			continue;
		}
		for (int i = 0; i < 8; i++) { y[i] = y5[i]; k1[i] = k7[i]; }
		left -= hs;
		opt.steps++;
		h = hs * std::min (5.0, 0.9 * std::pow (std::max (err, 1e-10), -0.2));
		lim = limit (y, k1);
		if (opt.stop) {
			for (int i = 0; i < 4; i++) { s.x[i] = y[i]; s.p[i] = y[4 + i]; }
			if (opt.stop (s)) return GEO_STOPPED;
		}
	}
	for (int i = 0; i < 4; i++) { s.x[i] = y[i]; s.p[i] = y[4 + i]; }
	return GEO_DONE;
}

namespace {
	// the flight's equations in coordinate time: y = (x[4], u_m[4], tau)
	void FlightDerivs (const Hole &hole, const Thrust &thrust, const double *y, double *dy)
	{
		hole.Derivs (y, dy); // (d/dtau)
		if (thrust) {
			double u[4], e[4][4], al[3] = {0, 0, 0};
			hole.Raise (y + 1, y + 4, u);
			thrust (y[0], y + 1, u, al);
			if (al[0] != 0.0 || al[1] != 0.0 || al[2] != 0.0) {
				hole.BoostFrame (y + 1, u, e);
				double a[4], al4[4];
				for (int m = 0; m < 4; m++) a[m] = al[0] * e[1][m] + al[1] * e[2][m] + al[2] * e[3][m];
				hole.Lower (y + 1, a, al4);
				for (int m = 0; m < 4; m++) dy[4 + m] += al4[m];
			}
		}
		double r = 1.0 / dy[0]; // (dtau/dt)
		for (int i = 0; i < 8; i++) dy[i] *= r;
		dy[8] = r;
	}
}

Result Fly (const Hole &hole, State &s, double dt, Integrator &opt, double &h, const Thrust &thrust, double &dtau)
{
	const int N = 9;
	double y[N], k1[N], k2[N], k3[N], k4[N], k5[N], k6[N], k7[N], yt[N], y5[N];
	for (int i = 0; i < 4; i++) { y[i] = s.x[i]; y[4 + i] = s.p[i]; }
	y[8] = 0.0;
	double left = dt;
	opt.steps = 0;
	dtau = 0.0;
	if (dt <= 0.0) return GEO_DONE;
	FlightDerivs (hole, thrust, y, k1);

	auto limit = [&] (const double *yy, const double *dd) {
		double r = std::max (hole.R (yy + 1), 1e-3 * hole.M);
		double v = std::sqrt (dd[1] * dd[1] + dd[2] * dd[2] + dd[3] * dd[3]);
		// (a step: a fraction of r at the speed, at most r/c; the time to
		// cross r: the scale of the smallest step)
		double tr = r / std::max (v, 0.1);
		double hl = opt.rFrac * tr;
		if (opt.hmax > 0.0) hl = std::min (hl, opt.hmax);
		return std::make_pair (hl, tr);
	};
	auto lims = limit (y, k1);
	double lim = lims.first;
	if (h <= 0.0) h = 0.1 * lim;

	while (left > 0.0) {
		if (opt.steps >= opt.maxSteps) break;
		double hs = std::min ({h, lim, left});
		if (hs < opt.hmin * lims.second && hs < left) { dtau = y[8]; return GEO_STUCK; }
		double hh = hs;
		for (int i = 0; i < N; i++) yt[i] = y[i] + hh * A21 * k1[i];
		FlightDerivs (hole, thrust, yt, k2);
		for (int i = 0; i < N; i++) yt[i] = y[i] + hh * (A31 * k1[i] + A32 * k2[i]);
		FlightDerivs (hole, thrust, yt, k3);
		for (int i = 0; i < N; i++) yt[i] = y[i] + hh * (A41 * k1[i] + A42 * k2[i] + A43 * k3[i]);
		FlightDerivs (hole, thrust, yt, k4);
		for (int i = 0; i < N; i++) yt[i] = y[i] + hh * (A51 * k1[i] + A52 * k2[i] + A53 * k3[i] + A54 * k4[i]);
		FlightDerivs (hole, thrust, yt, k5);
		for (int i = 0; i < N; i++) yt[i] = y[i] + hh * (A61 * k1[i] + A62 * k2[i] + A63 * k3[i] + A64 * k4[i] + A65 * k5[i]);
		FlightDerivs (hole, thrust, yt, k6);
		for (int i = 0; i < N; i++) y5[i] = y[i] + hh * (B1 * k1[i] + B3 * k3[i] + B4 * k4[i] + B5 * k5[i] + B6 * k6[i]);
		FlightDerivs (hole, thrust, y5, k7);

		double r = std::max (hole.R (y + 1), hole.M);
		double pn = std::sqrt (y[4] * y[4] + y[5] * y[5] + y[6] * y[6] + y[7] * y[7]);
		double err = 0.0;
		for (int i = 0; i < N; i++) {
			double e = hh * (E1 * k1[i] + E3 * k3[i] + E4 * k4[i] + E5 * k5[i] + E6 * k6[i] + E7 * k7[i]);
			double sc = i < 4 ? std::max (std::fabs (y[i]), r) : i < 8 ? std::max (std::fabs (y[i]), pn) : std::max (std::fabs (y[i]), hs);
			err = std::max (err, std::fabs (e) / (opt.tol * sc));
		}
		if (!(err <= 1.0)) {
			h = (err == err) ? hs * std::max (0.1, 0.9 * std::pow (err, -0.2)) : 0.1 * hs;
			continue;
		}
		if (opt.stop) {
			// (the stop found within the step: the step shortened until the
			// stop falls in its last 1e-6 of the crossing time)
			for (int i = 0; i < 4; i++) { s.x[i] = y5[i]; s.p[i] = y5[4 + i]; }
			if (opt.stop (s) && hs > 1e-6 * lims.second) { h = 0.5 * hs; continue; }
		}
		for (int i = 0; i < N; i++) y[i] = y5[i];
		// (keep the 4-velocity on the mass shell: the thrust's drift)
		double nrm = hole.Norm (y + 1, y + 4);
		if (nrm < 0.0) { double sc = 1.0 / std::sqrt (-nrm); for (int i = 4; i < 8; i++) y[i] *= sc; }
		FlightDerivs (hole, thrust, y, k1);
		left -= hs;
		opt.steps++;
		h = hs * std::min (5.0, 0.9 * std::pow (std::max (err, 1e-10), -0.2));
		lims = limit (y, k1);
		lim = lims.first;
		if (opt.stop) {
			for (int i = 0; i < 4; i++) { s.x[i] = y[i]; s.p[i] = y[4 + i]; }
			if (opt.stop (s)) { dtau = y[8]; return GEO_STOPPED; }
		}
	}
	for (int i = 0; i < 4; i++) { s.x[i] = y[i]; s.p[i] = y[4 + i]; }
	dtau = y[8];
	return left > 0.0 ? GEO_STEPS : GEO_DONE;
}

} // namespace kerr
