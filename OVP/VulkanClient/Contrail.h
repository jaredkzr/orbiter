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
#include "Particles.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>

namespace contrail {

/// air temperature [K] at global position p over the planet hp (0: no air
/// there); its pressure [Pa] in *pres if given
inline double AirTemperature (OBJHANDLE hp, const VECTOR3 &p, double *pres = nullptr)
{
	if (pres) *pres = 0.0;
	if (!hp || !oapiGetPlanetAtmConstants (hp)) return 0.0;
	double lng, lat, rad;
	oapiGlobalToEqu (hp, p, &lng, &lat, &rad);
	ATMPARAM prm{};
	// (by altitude and position: the radius-only overload gave rho 0 once)
	oapiGetPlanetAtmParams (hp, rad - oapiGetSize (hp), lng, lat, &prm);
	if (pres && prm.rho > 0.0) *pres = prm.p;
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

/// T: the air's temperature [K]; p: its pressure [Pa]; alt: altitude [m];
/// rocket: a rocket engine's exhaust (more water, hotter: forms higher);
/// humidity: 0..1 (SessionHumidity)
inline Gate Evaluate (double T, double p, double alt, bool rocket, double humidity)
{
	Gate g;
	if (T <= 0.0) return g;
	// (thin air: the exhaust's water expands and freezes out unseen - no
	// trail below some 2 kPa, whatever the cold: Mars' 0.6 kPa at 210 K
	// drew white bands behind a hovering DG, f5. Earth's trails at 8-25 km
	// are in 2.5-35 kPa)
	double thin = Smooth (2e3, 4e3, p);
	// (Schmidt-Appleman threshold, about -40 C for jets; rockets' exhaust
	// condenses a little warmer)
	double tc = rocket ? 238.0 : 233.0;
	g.form = Smooth (tc + 5.0, tc - 5.0, T) * thin;
	// (the air above the tropopause is dry: jets' trails short and thin
	// there; a rocket's carries its own water up to the stratosphere)
	if (rocket) g.form *= 1.0 - Smooth (22e3, 30e3, alt);
	else g.form *= (1.0 - 0.6 * Smooth (13e3, 16e3, alt)) * (1.0 - Smooth (20e3, 26e3, alt)); // (a jet's little water: none in the dry upper stratosphere)
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

/// The visible trail of one exhaust: a thin line of lit puffs at rest in
/// the air behind the source (class Contrail, sprites: the volume would
/// draw a thin line in beads), living the gate's life up to 30 s and
/// spreading slowly; where the air is ice-supersaturated (Gate::persist) it
/// is carried on by larger puffs of the long-lived tier (the trail flag:
/// wind drift, fading in as the line fades out), spreading into a band for
/// up to 15 minutes
struct Emitter {
	ParticleRing head, line, band;
	double accH = 0.0, accL = 0.0, accB = 0.0;
	/// the line's texture: a smooth puff (lit, without billows or erosion:
	/// the smoke texture's billows broke a thin line into beads); null:
	/// the smoke texture
	VkTexture *tex = nullptr;
	bool havePrev = false;
	VECTOR3 prev{};

	void Reset () { havePrev = false; }
	/// src: where it shows (global), vel: the source's velocity (global),
	/// air: the source's velocity relative to the air (global), width: the
	/// line's width where it shows [m], strength: 0..1 (engine level);
	/// band: this one lays the persistent band (one exhaust of a vessel:
	/// the engines' bands merge anyway)
	void Emit (ParticleSystem *sys, const VECTOR3 &src, const VECTOR3 &vel, const VECTOR3 &air, const Gate &g,
		double width, double strength, double dt, bool withBand = true)
	{
		double a = std::clamp (g.form * strength, 0.0, 1.0);
		if (!sys || !sys->enabled || dt <= 0.0 || a < 0.01) { havePrev = false; return; }
		VECTOR3 p0 = havePrev ? prev : src - vel * dt;
		prev = src, havePrev = true;
		double v = length (air);
		double D = sys->density, sz = std::pow (D, -1.0 / 3.0);
		ParticleSystem::Emission e{};
		e.p0 = p0, e.p1 = src;
		e.vel = vel - air; // (at rest in the air)
		e.dir = v > 1.0 ? air / v : _V(0, 0, 0);
		e.v0 = 0.0, e.dt = dt;
		e.slowdown = 0.5;
		e.tinted = true, e.cold = true;
		e.albedo[0] = e.albedo[1] = e.albedo[2] = 0.95f;
		e.cls = ParticleSystem::Class::Contrail;
		// the line: a puff for every half of its width along the path
		// (capped); its first seconds by short-lived puffs (particles fade in
		// over 8 % of their life: a 30 s line alone showed only from some
		// 600 m behind a jet)
		double life = std::min (g.life, 30.0);
		// (the head dense: seen close, behind the engines; the line has
		// spread by the time it takes over, sparser)
		double hrate = std::clamp (v / (0.25 * width), 4.0, 200.0) * D;
		double rate = std::clamp (v / (0.5 * width), 2.0, 40.0) * D;
		e.spread = 0.3;
		// (at night fainter, ParticleSystem::contrailNight: against the sky
		// a trail would show as a band)
		const double an = a * sys->contrailNight;
		e.alpha = 0.9 * an * std::min (1.0, 1.3 / (D * sz * sz));
		double hlife = std::min (4.0, life);
		VkTexture *lt = tex ? tex : sys->SmokeTexture ();
		if (sys->Reserve (head, hrate, hlife, ParticleSystem::Class::Contrail, 0.0, lt, false)) {
			accH += hrate * dt;
			uint32_t n = (uint32_t)accH;
			accH -= n;
			if (n) {
				e.lifetime = hlife;
				e.size = width * sz, e.growth = width / 3.0 * sz;
				sys->Emit (head, e, n);
			}
		}
		if (life > hlife && sys->Reserve (line, rate, life, ParticleSystem::Class::Contrail, 0.0, lt, false)) {
			accL += rate * dt;
			uint32_t n = (uint32_t)accL;
			accL -= n;
			if (n) {
				e.lifetime = life;
				e.size = width * sz, e.growth = (1.0 + 1.5 * g.persist) * width / 3.0 * sz;
				sys->Emit (line, e, n);
			}
		}
		// the persistent band (the trail tier): a few large puffs a second
		// (each lives up to 15 minutes: a launch's three engines at 20 a
		// second filled the pool by T+120, f9)
		if (withBand && g.persist > 0.3 && g.life > 60.0) {
			double S0 = std::max (8.0 * width, 30.0);
			double rb = std::clamp (v / S0, 0.25, 4.0);
			if (sys->Reserve (band, rb, g.life, ParticleSystem::Class::Contrail, 0.0, sys->SmokeTexture (), false)) {
				accB += rb * dt;
				uint32_t n = (uint32_t)accB;
				accB -= n;
				if (n) {
					e.spread = 0.5;
					e.lifetime = g.life;
					e.size = S0, e.growth = 1.0;
					e.alpha = 0.35 * an * std::clamp ((g.persist - 0.3) / 0.4, 0.0, 1.0);
					e.trail = true;
					sys->Emit (band, e, n);
				}
			}
		}
	}
	void Draw (ParticleSystem *sys, VkDescriptorSet frameSet) const
	{
		if (!sys) return;
		VkTexture *lt = tex ? tex : sys->SmokeTexture ();
		sys->Draw (head, lt, false, frameSet);
		sys->Draw (line, lt, false, frameSet);
		sys->Draw (band, sys->SmokeTexture (), false, frameSet);
	}
	/// the emitter is gone (its particles live on)
	void Release (ParticleSystem *sys)
	{
		for (ParticleRing *r : { &head, &line, &band })
			if (sys && r->sys == sys) sys->Release (*r);
	}
};

} // namespace contrail

#endif // !__CONTRAIL_H
