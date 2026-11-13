// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Effects.h"
#include "BlastSynth.h"
#include "FireSynth.h"
#include "SplashSynth.h"
#include "VesselSounds.h"
#include "Debris.h"
#include "Psys.h"
#include "Medium.h"
#include "Ears.h"
#include "Vessel.h"
#include "Planet.h"
#include "Camera.h"
#include "Log.h"
#include "TimeData.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

extern Camera *g_camera;
extern TimeData td;
extern PlanetarySystem *g_psys;

namespace audio {

namespace {
	/// A blast unheard this long after it went off is dropped (beyond the
	/// world's reach)
	constexpr double BLAST_MAX_AGE = 240.0;
	constexpr double SEA_P = 101325.0, SEA_C = 340.3;
	/// The wake's level (0.02 Pa at refDist)
	const double WAKE_LEVEL = 0.02 / P_FULL / 0.3;
}

void Effects::Explosion (const Vessel *v, double propellant)
{
	if (!v || !engine.Active () || propellant < 1.0) return;
	const Planet *pl = v->ProxyPlanet ();
	if (!pl) return;
	const Vector &g = v->GPos ();
	const double gp[3] = { g.x, g.y, g.z };
	const Air a = World::AirAt (pl, gp);
	const bool cinematic = world.GetRealism () != PHYSICAL;
	if (!a.Present () && !cinematic) {
		LOGOUT ("Audio: %s broke up in vacuum: no blast heard", v->Name ());
		return;
	}
	// a surface burst near the ground (within about the fireball's size)
	const double alt = v->GetSurfParam () ? v->GetSurfParam ()->alt : 1e9;
	const double fireball = 3.86 * std::cbrt (propellant); // [m] (a propellant fireball's radius)
	Blast b;
	b.W = BlastSynth::TntEquivalent (propellant, alt < fireball);
	b.planet = pl;
	b.t0 = td.SimT1;
	// (cinematic: the air there at least a quarter of sea level's)
	b.p = cinematic ? std::max (a.p, CINEMATIC_RHO * SEA_P) : a.p;
	b.c = a.c > 0.0 ? a.c : SEA_C;
	b.coupling = Coupling (Effective (a, world.GetRealism ())); // (what the world multiplies it by at the source)
	// an emitter on the planet where it went off; its history: nothing
	// before, the blast from t0
	EmitterDesc d;
	d.body = pl;
	Vector off = tmul (pl->GRot (), g - pl->GPos ());
	d.offset = V3 (off.x, off.y, off.z);
	d.refDist = (float)std::max (2.0, 2.0 * std::cbrt (b.W) * std::cbrt (SEA_P / b.p)); // (Z = 2: the near field)
	d.level = 0.0f;
	d.name = std::string ("explosion of ") + v->Name ();
	auto s = BlastSynth::Make (engine, seed++);
	if (!s) return;
	b.synth = s.get ();
	b.em = world.Add (d, std::move (s));
	if (!b.em) return;
	Emitter::Params p{};
	b.em->Push (b.t0 - 1e-3, p);
	p[0] = 1.0f;
	b.em->Push (b.t0, p);
	LOGOUT ("Audio: explosion of %s: %.0f kg propellant, %.0f kg TNT%s, air %.3g kg/m^3", v->Name (), propellant, b.W, alt < fireball ? " (surface burst)" : "", a.rho);
	blasts.push_back (b);
}

void Effects::Update (double t)
{
	if (!g_camera) return;
	UpdateFires (t);
	UpdateWater (t);
	const Vector &cam = g_camera->GPos ();
	for (auto it = blasts.begin (); it != blasts.end (); ) {
		Blast &b = *it;
		const Vector src = b.planet->GPos () + mul (b.planet->GRot (), Vector (b.em->Desc ().offset.x, b.em->Desc ().offset.y, b.em->Desc ().offset.z));
		const double R = std::max (1.0, (cam - src).length ());
		if (!b.fired) {
			// its strength and length here (the listener moves while it comes):
			// the world spreads it as 1/r from refDist, so the level there is
			// dP(R) R / refDist
			// (Sachs-scaled for the air at the source; the world multiplies every
			// source by its coupling into the air there, (rho c / rho0 c0)^1/4 -
			// here the scaling already says what the blast is, so that is
			// divided out; the listener's coupling is the listener's own air)
			double dP, tdv;
			BlastSynth::Sachs (R, b.W, b.p, b.c, dP, tdv);
			const double refDist = b.em->Desc ().refDist;
			b.em->SetLevel ((float)(dP * std::max (R, refDist) / refDist / P_FULL / std::max (b.coupling, 1e-6)));
			b.synth->Set (R, tdv);
			Emitter::Params p{};
			p[0] = 1.0f;
			b.em->Push (t, p);
			// (heard: once the world has found its path - its travel time; before
			// that its history would be recalled with none)
			if (b.em->Heard ().images > 0 && b.em->Recalled ()[0] > 0.5f) {
				b.synth->Fire ();
				b.fired = true;
				LOGOUT ("Audio: blast heard at %.3f s, %.0f m from it: %.0f Pa (%.0f dB), %.1f ms (air %.0f Pa)", t, R, dP, 20.0 * std::log10 (std::max (dP, 1e-12) / 2e-5), 1e3 * tdv, b.p);
				static const bool log = getenv ("ORBITER_AUDIO_LOG") != nullptr;
				if (log) printf ("Audio: blast heard at %.3f s, %.0f m from it: %.0f Pa\n", t, R, dP), fflush (stdout);
			}
		}
		if ((b.fired && b.synth->Done ()) || t - b.t0 > BLAST_MAX_AGE || t < b.t0 - 1.0) {
			world.Remove (b.em);
			it = blasts.erase (it);
		} else ++it;
	}
}

void Effects::UpdateFires (double t)
{
	if (!g_psys) return;
	// fires lit: an emitter each, its history from the moment it was lit
	for (size_t i = 0; i < g_psys->nDebris (); i++) {
		Debris *d = g_psys->GetDebris (i);
		if (!d || d->BurnLeft () <= 0.0 || d->Fuel () <= 0.0) continue;
		if (std::any_of (fires.begin (), fires.end (), [d](const Fire &f) { return f.debris == d; })) continue;
		auto s = FireSynth::Make (engine, seed++);
		if (!s) continue;
		Fire f;
		f.debris = d;
		f.synth = s.get ();
		const double Q = d->Fuel () * FireSynth::HEAT / std::max (d->BurnTime (), 1.0);
		EmitterDesc ed;
		ed.body = d;
		ed.refDist = (float)std::max (1.0, 0.5 * FireSynth::DiameterFor (Q));
		// (p at refDist from the acoustic power, in sea-level air)
		ed.level = (float)(VesselSounds::PressureAt (FireSynth::EFFICIENCY * Q, ed.refDist) / P_FULL / FireSynth::FIRE_RMS);
		ed.name = std::string ("fire of ") + d->Name ();
		f.em = world.Add (ed, std::move (s));
		if (!f.em) continue;
		f.em->Push (t - 1e-3, Emitter::Params{});
		LOGOUT ("Audio: %s burning: %.0f kg over %.0f s, %.2g W heat, %.2g W of sound, peak %.0f Hz, %.0f m across",
			d->Name (), d->Fuel (), d->BurnTime (), Q, FireSynth::EFFICIENCY * Q, FireSynth::PeakFor (Q), FireSynth::DiameterFor (Q));
		fires.push_back (f);
	}
	for (auto it = fires.begin (); it != fires.end (); ) {
		Fire &f = *it;
		const Debris *d = static_cast<const Debris*>(f.debris);
		const double left = d->BurnLeft (), T = std::max (d->BurnTime (), 1.0);
		const double Q = d->Fuel () * FireSynth::HEAT / T;
		// (dying down over its last fifth)
		const double level = left > 0.0 ? std::sqrt (std::min (1.0, left / (0.2 * T))) : 0.0;
		Emitter::Params p{};
		p[0] = (float)level, p[1] = (float)FireSynth::PeakFor (Q), p[2] = (float)FireSynth::DiameterFor (Q);
		f.em->Push (t, p);
		// (as heard: from when the sound now arriving left it; nothing until
		// the world has found its path)
		const Emitter::Params h = f.em->Heard ().images > 0 ? f.em->Recalled () : Emitter::Params{};
		f.synth->Set (h[1] > 0.0f ? h[1] : p[1], h[2] > 0.0f ? h[2] : p[2], h[0]);
		// out: gone a minute after it stopped burning (its last sound arrived)
		if (left <= 0.0 && f.out < 0.0) f.out = t;
		if (f.out >= 0.0 && t - f.out > 60.0) { world.Remove (f.em); it = fires.erase (it); }
		else ++it;
	}
}

void Effects::UpdateWater (double t)
{
	if (!g_psys) return;
	for (DWORD i = 0; i < g_psys->nVessel (); i++) {
		Vessel *v = g_psys->GetVessel (i);
		if (!v || v->Destroyed ()) continue;
		VESSEL *vi = v->GetModuleInterface ();
		if (!vi || vi->Version () < 2) continue;
		WATERSTATE ws;
		if (!vi->GetWaterState (&ws)) continue;
		auto it = std::find_if (waters.begin (), waters.end (), [v](const Water &w) { return w.v == v; });
		if (it == waters.end ()) {
			if (!ws.nearWater && !ws.wet) continue;
			// (near the water: an emitter, its history from now)
			auto s = SplashSynth::Make (engine, seed++);
			if (!s) continue;
			Water w;
			w.v = v;
			w.synth = s.get ();
			EmitterDesc d;
			d.body = v;
			d.refDist = (float)std::max (2.0, 0.5 * v->Size ());
			d.level = 0.0f;
			d.hull = 0.3f; // (in its cabin: the slam through the structure)
			d.name = std::string (v->Name ()) + " water";
			w.em = world.Add (d, std::move (s));
			if (!w.em) continue;
			w.em->Push (t - 1e-3, Emitter::Params{});
			waters.push_back (w);
			it = waters.end () - 1;
		}
		Water &w = *it;
		if (ws.nearWater || ws.wet) w.lastNear = t;
		// an entry: wet anew. Its sound: a part of its kinetic energy - the
		// entry speed of the hull's fastest panel - over about the spray's
		// time, as a pressure at refDist (sea-level air)
		const double size = v->Size ();
		Emitter::Params p = w.em->History ().empty () ? Emitter::Params{} : w.em->History ().back ().second;
		// (the entry's speeds: into the water - the hull's own, the step before
		// it touched (the water's measure of it comes later) - and along it)
		VECTOR3 gv;
		vi->GetGroundspeedVector (FRAME_HORIZON, gv);
		const double vDown = std::max (-gv.y, 0.0), vAlong = std::hypot (gv.x, gv.z);
		const bool newEntry = ws.wet && !w.wet && t - w.dryAt > 0.5; // (a skip, a bounce: one entry)
		if (!ws.wet && w.wet) w.dryAt = t;
		if (newEntry) {
			const double vin = std::max ({ws.entrySpeed, w.vDown, 0.5});
			// its energy: the fall into the water, and a part of a skim's along
			// it (a ditching: the spray sheet it throws up as it slows)
			const double E = SplashSynth::SPLASH_EFFICIENCY * 0.5 * v->Mass () * (vin * vin + 0.05 * w.vAlong * w.vAlong);
			const double T = std::clamp (0.6 * vin / 9.81, 0.3, 4.0) + 0.3;
			const double r0 = w.em->Desc ().refDist;
			const double prms = std::sqrt (E / T * RHO0 * C0 / (4.0 * 3.14159265 * r0 * r0));
			w.entries++;
			p[0] = (float)w.entries, p[1] = (float)(prms / P_FULL / SplashSynth::SPLASH_RMS), p[2] = (float)size, p[3] = (float)vin;
			LOGOUT ("Audio: %s into the water at %.1f m/s (%.0f m/s along): %.2g J of sound, %.2f Pa at %.0f m", v->Name (), vin, w.vAlong, E, prms, r0);
			static const bool log = getenv ("ORBITER_AUDIO_LOG") != nullptr;
			if (log) printf ("Audio: %s into the water at %.3f s, %.1f m/s\n", v->Name (), t, vin), fflush (stdout);
		}
		w.wet = ws.wet;
		w.vDown = vDown, w.vAlong = vAlong;
		// the wake: its speed through the water (wet), at a fixed level -
		// a body moving through water at a few m/s: tens of dB at its side
		p[4] = ws.wet ? (float)ws.speed : 0.0f;
		w.em->Push (t, p);
		if (ws.wet) {
			const VECTOR3 &c = ws.contact;
			const Vector off = tmul (v->GRot (), Vector (c.x, c.y, c.z) - v->GPos ());
			w.em->SetOffset (V3 (off.x, off.y, off.z));
		}
		// as heard: entries counted, the strength of the last
		const Emitter::Params h = w.em->Heard ().images > 0 ? w.em->Recalled () : Emitter::Params{};
		const int n = (int)h[0];
		if (n > w.heard) {
			// (the emitter's level: this entry's; the synth's own strength 1)
			w.em->SetLevel (h[1]);
			w.synth->Entry (1.0f, h[2], h[3]);
			w.heard = n;
		} else if (w.heard == 0) w.em->SetLevel ((float)WAKE_LEVEL);
		// the wake: a level of its own (a body moving through water at a few
		// m/s: some 0.02 Pa at its side), whatever the last entry's
		w.synth->Set (h[4], (float)size, (float)(WAKE_LEVEL / std::max ((double)w.em->Desc ().level, 1e-9)));
	}
	// (far from the water a while: gone)
	for (auto it = waters.begin (); it != waters.end (); ) {
		if (t - it->lastNear > 60.0) { world.Remove (it->em); it = waters.erase (it); }
		else ++it;
	}
}

void Effects::Forget (const Body *body)
{
	for (auto it = waters.begin (); it != waters.end (); ) {
		if ((const Body*)it->v == body) { world.Remove (it->em); it = waters.erase (it); }
		else ++it;
	}
	for (auto it = fires.begin (); it != fires.end (); ) {
		if (it->debris == body) { world.Remove (it->em); it = fires.erase (it); }
		else ++it;
	}
	for (auto it = blasts.begin (); it != blasts.end (); ) {
		if (it->planet == body) { world.Remove (it->em); it = blasts.erase (it); }
		else ++it;
	}
}

void Effects::Clear ()
{
	for (auto &b : blasts) world.Remove (b.em);
	blasts.clear ();
	for (auto &f : fires) world.Remove (f.em);
	fires.clear ();
	for (auto &w : waters) world.Remove (w.em);
	waters.clear ();
}

} // namespace audio
