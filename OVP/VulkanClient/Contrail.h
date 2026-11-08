// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Contrails: where the water of an exhaust condenses into a visible trail.
// A gate by the air's temperature (Schmidt-Appleman, roughly: colder than
// about -40 C; on Earth from some 8 km up), fading where the air is too dry
// (jets: above the tropopause; rockets, whose exhaust carries far more
// water: up to the stratosphere, fading above ~25 km), and whether the
// trail persists (ice-supersaturated air: minutes, spreading) or dissolves
// within a minute, by a humidity drawn per session and planet.
// Emitters (VVessel, legacy contrail streams) use these; see
// out/fx-audit/A-run2-notes.md for the wiring.
// =======================================================================

#ifndef __CONTRAIL_H
#define __CONTRAIL_H

#include "OrbiterAPI.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>

namespace contrail {

/// air temperature [K] at global position p over the planet hp (0: no air there)
inline double AirTemperature (OBJHANDLE hp, const VECTOR3 &p)
{
	if (!hp || !oapiGetPlanetAtmConstants (hp)) return 0.0;
	double lng, lat, rad;
	oapiGlobalToEqu (hp, p, &lng, &lat, &rad);
	ATMPARAM prm{};
	// (by altitude and position: the radius-only overload gave rho 0 once)
	oapiGetPlanetAtmParams (hp, rad - oapiGetSize (hp), lng, lat, &prm);
	return prm.rho > 0.0 ? prm.T : 0.0;
}

/// a contrail at a place: how much of the exhaust's water shows (0..1),
/// how far it persists (0: dissolves within the short life, 1: the long),
/// its life [s]
struct Gate {
	double form = 0.0;
	double persist = 0.0;
	double life = 0.0;
};

inline double Smooth (double e0, double e1, double x)
{
	double t = std::clamp ((x - e0) / (e1 - e0), 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}

/// T: the air's temperature [K]; alt: altitude [m]; rocket: a rocket
/// engine's exhaust (more water, hotter: forms higher); humidity: 0..1
/// (SessionHumidity)
inline Gate Evaluate (double T, double alt, bool rocket, double humidity)
{
	Gate g;
	if (T <= 0.0) return g;
	// (Schmidt-Appleman threshold, about -40 C for jets; rockets' exhaust
	// condenses a little warmer)
	double tc = rocket ? 238.0 : 233.0;
	g.form = Smooth (tc + 5.0, tc - 5.0, T);
	// (the air above the tropopause is dry: jets' trails short and thin
	// there; a rocket's carries its own water up to the stratosphere)
	if (rocket) g.form *= 1.0 - Smooth (22e3, 30e3, alt);
	else g.form *= 1.0 - 0.6 * Smooth (13e3, 16e3, alt);
	if (g.form <= 0.0) return g;
	// persistent in ice-supersaturated air: the upper troposphere on a
	// humid day; a rocket's in the stratosphere (its water and the cold:
	// launch trails stand for many minutes at 15-30 km)
	double tropo = Smooth (0.4, 0.75, humidity) * Smooth (7e3, 9e3, alt) * (1.0 - Smooth (12.5e3, 14e3, alt));
	double strato = rocket ? Smooth (14e3, 18e3, alt) * (1.0 - Smooth (28e3, 34e3, alt)) * (0.5 + 0.5 * humidity) : 0.0;
	g.persist = std::max (tropo, strato);
	const double SHORT = 30.0, LONG = 900.0; // [s] (the long: the trail tier's)
	g.life = SHORT + (LONG - SHORT) * g.persist;
	return g;
}

/// the humidity of the upper air on the planet hp in this session, 0
/// (dry: contrails short) .. 1 (persistent ones): drawn from the planet,
/// the day of the session's start and its wind (the same all session,
/// another on another day); ORBITER_VK_HUMIDITY overrides it
inline double SessionHumidity (OBJHANDLE hp)
{
	static std::map<OBJHANDLE, double> cache;
	auto it = cache.find (hp);
	if (it != cache.end ()) return it->second;
	double h;
	if (const char *e = getenv ("ORBITER_VK_HUMIDITY")) h = std::clamp (atof (e), 0.0, 1.0);
	else {
		char name[64] = "";
		if (hp) oapiGetObjectName (hp, name, 64);
		uint32_t x = 2166136261u;
		for (const char *c = name; *c; c++) x = (x ^ (uint8_t)*c) * 16777619u;
		x = (x ^ (uint32_t)(int64_t)std::floor (oapiGetSimMJD ())) * 16777619u;
		if (hp) {
			VECTOR3 w = oapiGetModelWind (hp, 0.0, 0.0, 11e3);
			x = (x ^ (uint32_t)(int32_t)std::lround (w.x * 100.0)) * 16777619u;
			x = (x ^ (uint32_t)(int32_t)std::lround (w.z * 100.0)) * 16777619u;
		}
		x ^= x >> 15, x *= 0x2c1b3c6du, x ^= x >> 12;
		h = (x & 0xffff) / 65535.0;
		if (strcmp (name, "Earth")) h *= 0.3; // (other worlds: drier)
	}
	cache[hp] = h;
	return h;
}

} // namespace contrail

#endif // !__CONTRAIL_H
