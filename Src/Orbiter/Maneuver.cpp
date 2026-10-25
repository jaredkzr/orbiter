// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Maneuver.h"
#include <cmath>

void maneuver::Frame (const Vector &r, const Vector &v, Vector &pro, Vector &nml, Vector &rad)
{
	pro = v.unit();
	nml = crossp (v, r).unit();
	rad = crossp (pro, nml); // (r - (r.pro) pro, normalised: outward, as nml = v x r in the left-handed frame)
	if (dotp (rad, r) < 0.0) rad = -rad;
}

Vector maneuver::ToGlobal (const Vector &dv, const Vector &r, const Vector &v)
{
	Vector pro, nml, rad;
	Frame (r, v, pro, nml, rad);
	return pro * dv.x + nml * dv.y + rad * dv.z;
}

Vector maneuver::FromGlobal (const Vector &dvg, const Vector &r, const Vector &v)
{
	Vector pro, nml, rad;
	Frame (r, v, pro, nml, rad);
	return Vector (dotp (dvg, pro), dotp (dvg, nml), dotp (dvg, rad));
}

namespace {
	// Stumpff functions
	double C2 (double psi)
	{
		if (psi > 1e-6) return (1.0 - cos (sqrt (psi))) / psi;
		if (psi < -1e-6) return (1.0 - cosh (sqrt (-psi))) / psi;
		return 0.5 - psi / 24.0;
	}
	double C3 (double psi)
	{
		if (psi > 1e-6) { double s = sqrt (psi); return (s - sin (s)) / (psi * s); }
		if (psi < -1e-6) { double s = sqrt (-psi); return (sinh (s) - s) / (-psi * s); }
		return 1.0 / 6.0 - psi / 120.0;
	}
}

bool maneuver::Kepler (const Vector &r0, const Vector &v0, double mu, double dt, Vector &r, Vector &v)
{
	r = r0, v = v0;
	if (dt == 0.0) return true;
	double r0l = r0.length();
	double smu = sqrt (mu);
	double rv = dotp (r0, v0) / smu;
	double alpha = 2.0 / r0l - dotp (v0, v0) / mu; // 1/a

	// initial guess of the universal anomaly
	double chi;
	if (alpha > 1e-12) chi = smu * dt * alpha;
	else if (fabs (alpha) <= 1e-12) {
		Vector h = crossp (r0, v0);
		double p = dotp (h, h) / mu;
		double s = 0.5 * atan (1.0 / (3.0 * sqrt (mu / (p*p*p)) * dt));
		double w = atan (cbrt (tan (s)));
		chi = sqrt (p) * 2.0 / tan (2.0 * w);
	} else {
		double a = 1.0 / alpha;
		double sgn = (dt >= 0.0 ? 1.0 : -1.0);
		chi = sgn * sqrt (-a) * log ((-2.0 * mu * alpha * dt) /
			(dotp (r0, v0) + sgn * sqrt (-mu * a) * (1.0 - r0l * alpha)));
		if (!std::isfinite (chi)) chi = sgn * smu * fabs (alpha) * dt;
	}

	// Newton iteration
	double psi = 0.0, c2 = 0.5, c3 = 1.0/6.0, rl = r0l;
	bool ok = false;
	for (int i = 0; i < 100; i++) {
		psi = chi * chi * alpha;
		c2 = C2 (psi), c3 = C3 (psi);
		rl = chi*chi*c2 + rv*chi*(1.0 - psi*c3) + r0l*(1.0 - psi*c2);
		double f = smu*dt - (chi*chi*chi*c3 + rv*chi*chi*c2 + r0l*chi*(1.0 - psi*c3));
		double dchi = f / rl;
		chi += dchi;
		if (fabs (dchi) < 1e-9 * (1.0 + fabs (chi))) { ok = true; break; }
	}
	psi = chi * chi * alpha;
	c2 = C2 (psi), c3 = C3 (psi);
	rl = chi*chi*c2 + rv*chi*(1.0 - psi*c3) + r0l*(1.0 - psi*c2);

	double f = 1.0 - chi*chi/r0l * c2;
	double g = dt - chi*chi*chi/smu * c3;
	double gd = 1.0 - chi*chi/rl * c2;
	double fd = smu/(rl*r0l) * chi * (psi*c3 - 1.0);
	r = r0 * f + v0 * g;
	v = r0 * fd + v0 * gd;
	return ok;
}

double maneuver::BurnTime (double dv, double F, double m, double ve)
{
	if (F <= 0.0 || m <= 0.0) return 0.0;
	if (ve <= 0.0) return dv * m / F;
	// m(t) = m - F/ve t; dv = ve ln (m / m(t))  =>  t = m ve / F (1 - exp (-dv/ve))
	return m * ve / F * (1.0 - exp (-dv / ve));
}
