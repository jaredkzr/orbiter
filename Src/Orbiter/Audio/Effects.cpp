// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Effects.h"
#include "BlastSynth.h"
#include "FireSynth.h"
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
}

void Effects::Explosion (const Vessel *v, double propellant)
{
	if (!v || !engine.Active () || propellant < 1.0) return;
	const Planet *pl = v->ProxyPlanet ();
	if (!pl) return;
	const Vector &g = v->GPos ();
	const double gp[3] = { g.x, g.y, g.z };
	const Air a = World::AirAt (pl, gp);
	if (!a.Present ()) {
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
	b.sachsR = std::cbrt (SEA_P / std::max (a.p, 1.0));
	b.sachsT = b.sachsR * SEA_C / std::max (a.c, 1.0);
	// an emitter on the planet where it went off; its history: nothing
	// before, the blast from t0
	EmitterDesc d;
	d.body = pl;
	Vector off = tmul (pl->GRot (), g - pl->GPos ());
	d.offset = V3 (off.x, off.y, off.z);
	d.refDist = (float)std::max (2.0, 2.0 * std::cbrt (b.W) * b.sachsR); // (Z = 2: the near field)
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
	const Vector &cam = g_camera->GPos ();
	for (auto it = blasts.begin (); it != blasts.end (); ) {
		Blast &b = *it;
		const Vector src = b.planet->GPos () + mul (b.planet->GRot (), Vector (b.em->Desc ().offset.x, b.em->Desc ().offset.y, b.em->Desc ().offset.z));
		const double R = std::max (1.0, (cam - src).length ());
		if (!b.fired) {
			// its strength and length here (the listener moves while it comes):
			// the world spreads it as 1/r from refDist, so the level there is
			// dP(R) R / refDist
			const double W3 = std::cbrt (b.W);
			const double Z = R * b.sachsR / W3;
			const double dP = BlastSynth::Overpressure (Z);
			const double refDist = b.em->Desc ().refDist;
			b.em->SetLevel ((float)(dP * std::max (R, refDist) / refDist / P_FULL));
			b.synth->Set (R, BlastSynth::Duration (Z, b.W) * b.sachsT);
			Emitter::Params p{};
			p[0] = 1.0f;
			b.em->Push (t, p);
			// (heard: once the world has found its path - its travel time; before
			// that its history would be recalled with none)
			if (b.em->Heard ().images > 0 && b.em->Recalled ()[0] > 0.5f) {
				b.synth->Fire ();
				b.fired = true;
				LOGOUT ("Audio: blast heard at %.3f s, %.0f m from it: %.0f Pa (%.0f dB), %.1f ms", t, R, dP, 20.0 * std::log10 (dP / 2e-5), 1e3 * BlastSynth::Duration (Z, b.W) * b.sachsT);
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

void Effects::Forget (const Body *body)
{
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
}

} // namespace audio
