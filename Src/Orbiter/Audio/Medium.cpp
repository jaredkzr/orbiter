// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Medium.h"
#include <algorithm>
#include <cmath>

namespace audio {

namespace {
	/// Absorption at sea level [dB/m/Hz^2]: 0.028 dB/m at 4 kHz
	constexpr double ABS_K = 0.028 / (4000.0 * 4000.0);
	/// (the Doppler factor's limits: a source closing at the speed of sound)
	constexpr double DOPPLER_MAX = 8.0;
}

Air Air::From (double rho, double T, double p, double R, double gamma)
{
	Air a;
	a.rho = rho, a.T = T, a.p = p;
	a.c = (T > 0.0 && R > 0.0 && gamma > 0.0) ? std::sqrt (gamma * R * T) : 0.0;
	return a;
}

Air Air::SeaLevel ()
{
	Air a;
	a.rho = RHO0, a.T = 288.15, a.p = 101325.0, a.c = C0;
	return a;
}

Air Effective (const Air &a, Realism r)
{
	if (r == PHYSICAL) return a;
	Air e = a;
	double rmin = CINEMATIC_RHO * RHO0;
	if (!a.Present () || a.rho < rmin) {
		e.rho = rmin;
		if (!(e.c > 0.0)) e.c = C0;
		if (!(e.T > 0.0)) e.T = 288.15;
	}
	return e;
}

double Coupling (const Air &a)
{
	if (!a.Present ()) return 0.0;
	return std::pow ((a.rho * a.c) / (RHO0 * C0), 0.25);
}

double Spreading (double dist, double refDist)
{
	refDist = std::max (refDist, 1e-3);
	return dist <= refDist ? 1.0 : refDist / dist;
}

double AbsorptionDb (const Air &a, double dist, double freq)
{
	if (!a.Present ()) return INFINITY;
	return ABS_K * freq * freq * (RHO0 / a.rho) * std::max (0.0, dist);
}

double AbsorptionCutoff (const Air &a, double dist, double fmax)
{
	if (!a.Present ()) return 0.0;
	double k = ABS_K * (RHO0 / a.rho) * dist;
	if (k <= 0.0) return fmax;
	return std::min (fmax, std::sqrt (3.0 / k));
}

double DopplerFactor (const double n[3], const double vs[3], const double vl[3], double c)
{
	if (!(c > 0.0)) return 1.0;
	double ns = n[0]*vs[0] + n[1]*vs[1] + n[2]*vs[2]; // source speed towards the listener
	double nl = n[0]*vl[0] + n[1]*vl[1] + n[2]*vl[2]; // listener speed away from the source
	double den = c - ns, num = c - nl;
	// (both negative: listener and source together faster than sound, the
	// listener overtaking the sound: the ratio holds. One negative: the
	// sound heard backwards, as fast as it would be forwards)
	if (std::fabs (den) < std::fabs (num) / DOPPLER_MAX) return DOPPLER_MAX;
	return std::clamp (std::fabs (num / den), 1.0 / DOPPLER_MAX, DOPPLER_MAX);
}

} // namespace audio
