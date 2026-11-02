// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// A black hole: a root body with a Kerr spacetime (BlackHole.h)

#include "Orbiter.h"
#include "BlackHole.h"
#include "Log.h"
#include <algorithm>
#include <cmath>
#include <fstream>

extern Orbiter *g_pOrbiter;

BlackHole::BlackHole (char *fname)
: Star (fname)
{
	double chi = 0.0;
	std::ifstream ifs (g_pOrbiter->ConfigPath (fname));
	if (ifs) {
		GetItemReal (ifs, "Spin", chi);
		GetItemReal (ifs, "AccretionRate", discRate);
		GetItemReal (ifs, "DiscOuter", discOuter);
		GetItemReal (ifs, "DiscGlow", discGlow);
	}
	chi = std::clamp (chi, -MAX_SPIN, MAX_SPIN);
	hole = kerr::Hole::FromSI (mass, chi);
	size = hole.Horizon ();
	SetFrame ();
	LOGOUT ("Black hole %s: %.4g kg (%.4g Msun), spin %.3f, horizon %.4g km, ISCO %.4g km", Name(),
		mass, mass / 1.98847e30, chi, size * 1e-3, hole.ISCO (true) * 1e-3);
	if (discRate > 0.0) {
		// the Eddington luminosity 4 pi G M m_p c / sigma_T; the disc's flux
		// F = 3 Mdot c^2 / (8 pi) DiscFluxShape (r) (metres), sigma T^4 = F
		const double G = 6.67430e-11, C = 299792458.0, MP = 1.67262192e-27, ST = 6.6524587e-29, SB = 5.670374e-8, PI = 3.14159265358979323846;
		double E, L, W;
		hole.CircularOrbit (hole.ISCO (true), true, E, L, W);
		double eta = 1.0 - E, LEdd = 4.0 * PI * G * mass * MP * C / ST;
		double mdot = discRate * LEdd / (eta * C * C);
		discTK = std::pow (3.0 * mdot * C * C / (8.0 * PI * hole.M * hole.M * SB), 0.25);
		discL = eta * mdot * C * C;
		for (double r = hole.ISCO (true); r < 10.0 * hole.ISCO (true); r *= 1.01)
			discTmax = std::max (discTmax, std::pow (3.0 * mdot * C * C / (8.0 * PI) * hole.DiscFluxShape (r) / SB, 0.25));
		LOGOUT ("Black hole %s: accretion disc at %.3g Eddington (%.3g kg/s), out to %.0f M: hottest %.4g K, %.4g W (%.3g Lsun)", Name(),
			discRate, mdot, discOuter, discTmax, discL, discL / 3.828e26);
	}
}

void BlackHole::SetFrame ()
{
	// (right-handed: swap y and z of Orbiter's left-handed vectors)
	auto rh = [] (const Vector &v) { return Vector (v.x, v.z, v.y); };
	axis = RotAxis ();
	if (axis.length2 () < 0.5) axis = Vector (0, 1, 0);
	Vector n (rh (axis));
	Vector ref (std::fabs (n.x) < 0.9 ? Vector (1, 0, 0) : Vector (0, 1, 0));
	Vector x (ref - n * dotp (ref, n));
	x.unify ();
	Vector y (crossp (n, x)); // (right-handed: n x x; Vecmat's crossp is the right-handed formula)
	e1 = rh (x), e2 = rh (y);
}

void BlackHole::Update (bool force)
{
	Star::Update (force);
}

void BlackHole::ToKerr (const Vector &v, double *k) const
{
	// (the dot product doesn't care about handedness)
	k[0] = dotp (v, e1);
	k[1] = dotp (v, e2);
	k[2] = dotp (v, axis);
}

Vector BlackHole::FromKerr (const double *k) const
{
	return e1 * k[0] + e2 * k[1] + axis * k[2];
}

bool BlackHole::HoverAccel (const Vector &rpos, Vector &alpha) const
{
	// the static observer's 4-acceleration: a_m = d_m ln V, V = sqrt (-g_tt);
	// on the boost frame of the observer at rest
	double X[3], u[4], e[4][4];
	ToKerr (rpos, X);
	if (!hole.StaticObserver (X, u)) return false;
	double r = std::max (hole.R (X), hole.M), eps = 1e-6 * r;
	auto lnV = [&] (const double *x) { double g[4][4]; hole.Metric (x, g); return 0.5 * std::log (-g[0][0]); };
	double a[4] = {0, 0, 0, 0}, av[4];
	for (int i = 0; i < 3; i++) {
		double xp[3] = {X[0], X[1], X[2]}, xm[3] = {X[0], X[1], X[2]};
		xp[i] += eps, xm[i] -= eps;
		a[1 + i] = (lnV (xp) - lnV (xm)) / (2.0 * eps);
	}
	hole.Raise (X, a, av);
	hole.BoostFrame (X, u, e);
	const double C2 = 299792458.0 * 299792458.0;
	double A[3];
	for (int i = 0; i < 3; i++) A[i] = hole.Dot (X, av, e[1 + i]) * C2;
	alpha = FromKerr (A);
	return true;
}

Vector BlackHole::FreeAccel (const Vector &rpos, const Vector &rvel) const
{
	// (the velocity's change over a short free flight)
	const double C = 299792458.0;
	double X[3];
	ToKerr (rpos, X);
	double dt = 1e-4 * std::max (hole.R (X), hole.M) / C;
	Vector p (rpos), v (rvel);
	if (!Drift (p, v, dt)) return Vector ();
	return (v - rvel) / dt;
}

bool BlackHole::CircularVelocity (const Vector &rpos, Vector &rvel) const
{
	double X[3];
	ToKerr (rpos, X);
	double r = hole.R (X), E, L, W;
	if (!hole.CircularOrbit (r, true, E, L, W)) return false;
	// (turning about the axis at Omega: the coordinate velocity Omega z x x)
	const double C = 299792458.0;
	double V[3] = {-W * X[1] * C, W * X[0] * C, 0.0};
	rvel = FromKerr (V);
	return true;
}

bool BlackHole::LocalState (const Vector &rpos, const Vector &rvel, Local &loc) const
{
	const double C = 299792458.0, C2 = C * C;
	double X[3], V[3], u[4], e[4][4], T[3][3];
	ToKerr (rpos, X);
	ToKerr (rvel / C, V);
	loc = Local ();
	loc.r = hole.R (X);
	loc.inside = loc.r < hole.Horizon ();
	if (!hole.CoordinateVelocity (X, V, u)) return false;
	loc.clockRate = 1.0 / u[0];
	hole.BoostFrame (X, u, e);
	hole.Tidal (X, e, T);
	// (on the boost frame's axes, which are Kerr's axes carried over: back
	// to the global axes)
	for (int i = 0; i < 3; i++) {
		double col[3] = {T[0][i], T[1][i], T[2][i]};
		Vector c (FromKerr (col));
		double ci[3] = {c.x, c.y, c.z};
		for (int k = 0; k < 3; k++) T[k][i] = ci[k];
	}
	for (int k = 0; k < 3; k++) {
		double row[3] = {T[k][0], T[k][1], T[k][2]};
		Vector rr (FromKerr (row));
		loc.tidal[k][0] = rr.x * C2, loc.tidal[k][1] = rr.y * C2, loc.tidal[k][2] = rr.z * C2;
	}
	// largest eigenvalue's size: the power iteration's bound, the Frobenius
	// norm over sqrt (3/2) (traceless: exact for 2:-1:-1)
	double f = 0.0;
	for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) f += loc.tidal[i][j] * loc.tidal[i][j];
	loc.tidalMax = std::sqrt (f / 1.5);
	double us[4];
	if ((loc.canHover = hole.StaticObserver (X, us))) {
		HoverAccel (rpos, loc.hover);
		double g = -hole.Dot (X, u, us);
		loc.speedStatic = C * std::sqrt (std::max (0.0, 1.0 - 1.0 / (g * g)));
	}
	return true;
}

bool BlackHole::Drift (Vector &rpos, Vector &rvel, double dt, const Vector &alpha) const
{
	const double C = 299792458.0;
	kerr::State s;
	double X[3], V[3], u[4];
	ToKerr (rpos, X);
	ToKerr (rvel / C, V);
	if (!hole.CoordinateVelocity (X, V, u)) return false;
	for (int i = 0; i < 3; i++) s.x[1 + i] = X[i];
	hole.Lower (X, u, s.p);
	kerr::Integrator opt;
	opt.tol = 1e-11;
	double h = 0.0, dtau, A[3];
	ToKerr (alpha / (C * C), A);
	kerr::Thrust thrust = [&] (double, const double *, const double *, double *o) { o[0] = A[0]; o[1] = A[1]; o[2] = A[2]; };
	if (kerr::Fly (hole, s, C * dt, opt, h, alpha.length2 () > 0.0 ? thrust : nullptr, dtau) != kerr::GEO_DONE) return false;
	hole.Raise (s.x + 1, s.p, u);
	double v3[3] = {u[1] / u[0] * C, u[2] / u[0] * C, u[3] / u[0] * C};
	rpos = FromKerr (s.x + 1);
	rvel = FromKerr (v3);
	return true;
}
