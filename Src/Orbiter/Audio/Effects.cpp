// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Effects.h"
#include "DebugEnv.h"
#include "BlastSynth.h"
#include "FireSynth.h"
#include "SplashSynth.h"
#include "ThunderSynth.h"
#include "CreakSynth.h"
#include "AudioSystem.h"
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
extern Vessel *g_focusobj;

namespace audio {

namespace {
	/// A blast unheard this long after it went off is dropped (beyond the
	/// world's reach)
	constexpr double BLAST_MAX_AGE = 240.0;
	constexpr double SEA_P = 101325.0, SEA_C = 340.3;
	/// The structure's creaks in its cabin: at the most this [Pa] at their
	/// peak (subtle: a moderate load's some 6 dB over the cabin's air
	/// handling - 56 dB - near the limit 80 dB at the peak; lost under the
	/// air's rush), carried through the structure
	constexpr double CREAK_P = 0.2, CREAK_HULL = 50.0;
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
	b.em = world.Add (d, std::move (s));
	Emitter *em = world.Get (b.em);
	if (!em) return;
	Floats<1> p;   // (gone off: 1)
	em->Push (b.t0 - 1e-3, p);
	p[0] = 1.0f;
	em->Push (b.t0, p);
	LOGOUT ("Audio: explosion of %s: %.0f kg propellant, %.0f kg TNT%s, air %.3g kg/m^3", v->Name (), propellant, b.W, alt < fireball ? " (surface burst)" : "", a.rho);
	blasts.push_back (b);
}

void Effects::Lightning (const Planet *pl, const Vector &start, const Vector &dir, double length, double El, int strokes, double duration, uint32_t seed)
{
	if (!pl || !engine.Active () || !g_camera) return;
	// (everything in the planet's frame: it turns while the sound comes)
	const Vector s = tmul (pl->GRot (), start - pl->GPos ()), d = tmul (pl->GRot (), dir);
	const Vector L = tmul (pl->GRot (), g_camera->GPos () - pl->GPos ());
	const double gs[3] = {start.x, start.y, start.z};
	const Air a = World::AirAt (pl, gs);
	const bool cinematic = world.GetRealism () != PHYSICAL;
	if (!a.Present () && !cinematic) return;
	const double ps[3] = {s.x, s.y, s.z}, pd[3] = {d.x, d.y, d.z}, pL[3] = {L.x, L.y, L.z};
	// (segments of 20 m near, longer far off: the detail a distant listener
	// can't tell apart costs time)
	const double seg = std::clamp ((s - L).length () / 500.0, 20.0, 100.0);
	ThunderSynth::Channel ch = ThunderSynth::Make (ps, pd, length, seg, seed);
	std::vector<double> st;
	for (int k = 0; k < std::max (strokes, 1); k++) st.push_back (duration * k / std::max (strokes, 1));
	double dmin = 0.0;
	const double p = cinematic ? std::max (a.p, CINEMATIC_RHO * SEA_P) : a.p;
	std::vector<float> buf = ThunderSynth::Render (ch, pL, st, El, p, a.c > 0.0 ? a.c : SEA_C, engine.SampleRate (), dmin);
	// the emitter at the channel's nearest point (its first sound): the buffer
	// is the pressure there already (spreading 1 at refDist = its distance;
	// the world's coupling at the source divided out)
	size_t inear = 0;
	double best = 1e30;
	for (size_t i = 0; i < ch.x.size (); i++) {
		double dd = std::hypot (ch.x[i] - L.x, ch.y[i] - L.y, ch.z[i] - L.z);
		if (dd < best) best = dd, inear = i;
	}
	Thunder th;
	th.t0 = td.SimT1;
	EmitterDesc ed;
	ed.body = pl;
	ed.offset = V3 (ch.x[inear], ch.y[inear], ch.z[inear]);
	ed.refDist = (float)std::max (1.0, best);
	ed.level = (float)(1.0 / P_FULL / std::max (Coupling (Effective (a, world.GetRealism ())), 1e-6));
	ed.name = "thunder";
	const size_t nbuf = buf.size ();
	auto syn = ThunderSynth::Make (engine, std::move (buf));
	if (!syn) return;
	th.em = world.Add (ed, std::move (syn));
	Emitter *tem = world.Get (th.em);
	if (!tem) return;
	Floats<1> pr;   // (flashed: 1)
	tem->Push (th.t0 - 1e-3, pr);
	pr[0] = 1.0f;
	tem->Push (th.t0, pr);
	LOGOUT ("Audio: lightning %.0f m long, %.2g J/m, %d strokes; thunder from %.0f m, %.1f s long (air %.0f Pa, c %.0f m/s)",
		length, El, strokes, best, nbuf / (double)engine.SampleRate (), p, a.c);
	thunders.push_back (th);
}

void Effects::Update (double t)
{
	if (!g_camera) return;
	for (auto it = thunders.begin (); it != thunders.end (); ) {
		Thunder &th = *it;
		Emitter *em = world.Get (th.em);
		ThunderSynth *synth = world.Synth<ThunderSynth> (th.em);
		if (!em || !synth) { it = thunders.erase (it); continue; } // (its planet gone)
		if (!th.fired) {
			Floats<1> pr;
			pr[0] = 1.0f;
			em->Push (t, pr);
			if (em->Heard ().images > 0 && em->Recalled<Floats<1>> ()[0] > 0.5f) {
				synth->Fire ();
				th.fired = true;
				const bool log = Dbg ().log > 0;
				if (log) printf ("Audio: thunder heard at %.3f s\n", t), fflush (stdout);
			}
		}
		if ((th.fired && synth->Done ()) || t - th.t0 > BLAST_MAX_AGE || t < th.t0 - 1.0) { world.Remove (th.em); it = thunders.erase (it); }
		else ++it;
	}
	UpdateFires (t);
	UpdateWater (t);
	UpdateCreaks (t);
	const Vector &cam = g_camera->GPos ();
	for (auto it = blasts.begin (); it != blasts.end (); ) {
		Blast &b = *it;
		Emitter *em = world.Get (b.em);
		BlastSynth *synth = world.Synth<BlastSynth> (b.em);
		if (!em || !synth) { it = blasts.erase (it); continue; } // (its planet gone)
		const Vector src = b.planet->GPos () + mul (b.planet->GRot (), Vector (em->Desc ().offset.x, em->Desc ().offset.y, em->Desc ().offset.z));
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
			const double refDist = em->Desc ().refDist;
			em->SetLevel ((float)(dP * std::max (R, refDist) / refDist / P_FULL / std::max (b.coupling, 1e-6)));
			synth->Set (R, tdv);
			Floats<1> p;
			p[0] = 1.0f;
			em->Push (t, p);
			// (heard: once the world has found its path - its travel time; before
			// that its history would be recalled with none)
			if (em->Heard ().images > 0 && em->Recalled<Floats<1>> ()[0] > 0.5f) {
				synth->Fire ();
				b.fired = true;
				LOGOUT ("Audio: blast heard at %.3f s, %.0f m from it: %.0f Pa (%.0f dB), %.1f ms (air %.0f Pa)", t, R, dP, 20.0 * std::log10 (std::max (dP, 1e-12) / 2e-5), 1e3 * tdv, b.p);
				const bool log = Dbg ().log > 0;
				if (log) printf ("Audio: blast heard at %.3f s, %.0f m from it: %.0f Pa\n", t, R, dP), fflush (stdout);
			}
		}
		if ((b.fired && synth->Done ()) || t - b.t0 > BLAST_MAX_AGE || t < b.t0 - 1.0) {
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
		const BodyRef dr = world.Ref (d);
		if (std::any_of (fires.begin (), fires.end (), [dr](const Fire &f) { return f.debris == dr; })) continue;
		auto s = FireSynth::Make (engine, seed++);
		if (!s) continue;
		Fire f;
		f.debris = dr;
		const double Q = d->Fuel () * FireSynth::HEAT / std::max (d->BurnTime (), 1.0);
		EmitterDesc ed;
		ed.body = d;
		ed.refDist = (float)std::max (1.0, 0.5 * FireSynth::DiameterFor (Q));
		// (p at refDist from the acoustic power, in sea-level air)
		ed.level = (float)(VesselSounds::PressureAt (FireSynth::EFFICIENCY * Q, ed.refDist) / P_FULL / FireSynth::FIRE_RMS);
		ed.name = std::string ("fire of ") + d->Name ();
		f.em = world.Add (ed, std::move (s));
		Emitter *fem = world.Get (f.em);
		if (!fem) continue;
		fem->Push (t - 1e-3, Floats<3>{});   // (its level, peak, size)
		LOGOUT ("Audio: %s burning: %.0f kg over %.0f s, %.2g W heat, %.2g W of sound, peak %.0f Hz, %.0f m across",
			d->Name (), d->Fuel (), d->BurnTime (), Q, FireSynth::EFFICIENCY * Q, FireSynth::PeakFor (Q), FireSynth::DiameterFor (Q));
		fires.push_back (f);
	}
	for (auto it = fires.begin (); it != fires.end (); ) {
		Fire &f = *it;
		// (its wreckage gone - deleted: its emitter with it)
		const Debris *d = static_cast<const Debris*> (world.Get (f.debris));
		Emitter *em = world.Get (f.em);
		FireSynth *synth = world.Synth<FireSynth> (f.em);
		if (!d || !em || !synth) { world.Remove (f.em); it = fires.erase (it); continue; }
		const double left = d->BurnLeft (), T = std::max (d->BurnTime (), 1.0);
		const double Q = d->Fuel () * FireSynth::HEAT / T;
		// (dying down over its last fifth)
		const double level = left > 0.0 ? std::sqrt (std::min (1.0, left / (0.2 * T))) : 0.0;
		Floats<3> p;
		p[0] = (float)level, p[1] = (float)FireSynth::PeakFor (Q), p[2] = (float)FireSynth::DiameterFor (Q);
		em->Push (t, p);
		// (as heard: from when the sound now arriving left it; nothing until
		// the world has found its path)
		const Floats<3> h = em->Heard ().images > 0 ? em->Recalled<Floats<3>> () : Floats<3>{};
		synth->Set (h[1] > 0.0f ? h[1] : p[1], h[2] > 0.0f ? h[2] : p[2], h[0]);
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
		const BodyRef vr = world.Ref (v);
		auto it = std::find_if (waters.begin (), waters.end (), [vr](const Water &w) { return w.v == vr; });
		if (it == waters.end ()) {
			if (!ws.nearWater && !ws.wet) continue;
			// (near the water: an emitter, its history from now)
			auto s = SplashSynth::Make (engine, seed++);
			if (!s) continue;
			Water w;
			w.v = vr;
			EmitterDesc d;
			d.body = v;
			d.refDist = (float)std::max (2.0, 0.5 * v->Size ());
			d.level = 0.0f;
			d.hull = 0.3f; // (in its cabin: the slam through the structure)
			d.name = std::string (v->Name ()) + " water";
			w.em = world.Add (d, std::move (s));
			if (!world.Get (w.em)) continue;
			world.Get (w.em)->Push (t - 1e-3, Floats<5>{});   // (entries, the last's strength, size, speed; the wake)
			waters.push_back (w);
			it = waters.end () - 1;
		}
		Water &w = *it;
		Emitter *em = world.Get (w.em);
		SplashSynth *synth = world.Synth<SplashSynth> (w.em);
		if (!em || !synth) continue;
		if (ws.nearWater || ws.wet) w.lastNear = t;
		// an entry: wet anew. Its sound: a part of its kinetic energy - the
		// entry speed of the hull's fastest panel - over about the spray's
		// time, as a pressure at refDist (sea-level air)
		const double size = v->Size ();
		Floats<5> p;
		em->Latest (p);
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
			const double r0 = em->Desc ().refDist;
			const double prms = std::sqrt (E / T * RHO0 * C0 / (4.0 * 3.14159265 * r0 * r0));
			w.entries++;
			p[0] = (float)w.entries, p[1] = (float)(prms / P_FULL / SplashSynth::SPLASH_RMS), p[2] = (float)size, p[3] = (float)vin;
			LOGOUT ("Audio: %s into the water at %.1f m/s (%.0f m/s along): %.2g J of sound, %.2f Pa at %.0f m", v->Name (), vin, w.vAlong, E, prms, r0);
			const bool log = Dbg ().log > 0;
			if (log) printf ("Audio: %s into the water at %.3f s, %.1f m/s\n", v->Name (), t, vin), fflush (stdout);
		}
		w.wet = ws.wet;
		w.vDown = vDown, w.vAlong = vAlong;
		// the wake: its speed through the water (wet), at a fixed level -
		// a body moving through water at a few m/s: tens of dB at its side
		p[4] = ws.wet ? (float)ws.speed : 0.0f;
		em->Push (t, p);
		if (ws.wet) {
			const VECTOR3 &c = ws.contact;
			const Vector off = tmul (v->GRot (), Vector (c.x, c.y, c.z) - v->GPos ());
			em->SetOffset (V3 (off.x, off.y, off.z));
		}
		// as heard: entries counted, the strength of the last
		const Floats<5> h = em->Heard ().images > 0 ? em->Recalled<Floats<5>> () : Floats<5>{};
		const int n = (int)h[0];
		if (n > w.heard) {
			// (the emitter's level: this entry's; the synth's own strength 1)
			em->SetLevel (h[1]);
			synth->Entry (1.0f, h[2], h[3]);
			w.heard = n;
		} else if (w.heard == 0) em->SetLevel ((float)WAKE_LEVEL);
		// the wake: a level of its own (a body moving through water at a few
		// m/s: some 0.02 Pa at its side), whatever the last entry's
		synth->Set (h[4], (float)size, (float)(WAKE_LEVEL / std::max ((double)em->Desc ().level, 1e-9)));
	}
	// (far from the water a while, or its vessel gone: gone)
	for (auto it = waters.begin (); it != waters.end (); ) {
		if (t - it->lastNear > 60.0 || !world.Get (it->em)) { world.Remove (it->em); it = waters.erase (it); }
		else ++it;
	}
}

void Effects::UpdateCreaks (double t)
{
	// (the vessel in focus only: heard in its cabin)
	const Vessel *v = g_focusobj && !g_focusobj->Destroyed () ? g_focusobj : nullptr;
	const BodyRef vr = world.Ref (v);
	if (creak.v != vr || (v && !world.Get (creak.em))) {
		world.Remove (creak.em);
		world.Remove (creak.bed);
		creak = Creak ();
		if (!v) return;
		auto s = CreakSynth::Make (engine, seed++);
		if (!s) return;
		creak.v = vr;
		EmitterDesc d;
		d.body = v;
		// (through the structure, in the cabin: CREAK_P at the most; outside,
		// a tiny source - refDist 0.1 m - nothing heard)
		d.refDist = 0.1f;
		d.level = (float)(CREAK_P / P_FULL / CREAK_HULL);
		d.hull = (float)CREAK_HULL;
		d.name = std::string (v->Name ()) + " structure";
		creak.em = world.Add (d, std::move (s));
		if (!world.Get (creak.em)) { creak = Creak (); return; }
		world.Get (creak.em)->Push (t - 1e-3, Floats<4>{});   // (creaks, groans counted; their strengths)
		creak.t = t;
	}
	if (!v) return; // (none in focus, or its wreck: nothing to creak)
	VESSEL *vi = const_cast<Vessel*>(v)->GetModuleInterface ();
	if (!vi) return;
	// its stress (0: none, 1: at its limit): its joints' worst load (the
	// structure's model, with damage on), else from its load factor (its
	// non-gravitational acceleration; 6 g: a limit) and, half as much, the
	// air's dynamic pressure on its panels (40 kPa)
	double stress = 0.0;
	const DWORD np = vi->GetPartCount ();
	for (DWORD i = 1; i < np; i++) {
		PARTINFO pi;
		if (vi->GetPartInfo (i, &pi) && !(pi.flags & PART_LOST)) stress = std::max (stress, pi.load);
	}
	if (stress <= 0.0) {
		VECTOR3 F, W;
		double gl = 0.0;
		if (vi->GetForceVector (F) && vi->GetWeightVector (W)) gl = length (F - W) / std::max (v->Mass (), 1.0) / 9.81;
		const double q = v->GetSurfParam () ? v->GetSurfParam ()->dynp : 0.0;
		stress = std::max (gl / 6.0, 0.5 * q / 40e3);
	}
	// (a black hole's tides on the core: heard as they near what tears it)
	const double tide = std::clamp (v->TidalLoad (), 0.0, 1.5);
	stress = std::max (stress, tide);
	// tides near tearing it: the whole structure groaning under the creaks, from half the limit
	// (a generated bed, looped, through the hull: Sound/Generated/creak_1)
	const double bedGain = std::pow (std::clamp ((tide - 0.5) / 0.5, 0.0, 1.0), 1.5);
	if (bedGain > 0.0 && !world.Get (creak.bed)) {
		const std::string f = System::FindSound ("Sound/Generated/creak_1.flac");
		if (!f.empty ()) {
			EmitterDesc d;
			d.body = v, d.refDist = 0.1f, d.hull = (float)CREAK_HULL, d.level = 0.0f;
			d.name = std::string (v->Name ()) + " structure groaning";
			creak.bed = world.Add (d, MakeFileSource (engine, f, true));
			const bool log = Dbg ().log > 0;
			if (log) printf ("Audio: structure groaning at %.2f s (tides %.2f of tearing)\n", t, tide), fflush (stdout);
		}
	}
	if (Emitter *bed = world.Get (creak.bed)) bed->SetLevel ((float)(2.0 * CREAK_P / P_FULL / CREAK_HULL * bedGain));
	stress = std::clamp (stress, 0.0, 1.5);
	const double dt = t - creak.t;
	creak.t = t;
	Floats<4> p;
	p[0] = (float)creak.count[0], p[1] = (float)creak.count[1];
	// (a jump - the vessel set somewhere, a first step: not a load changing)
	if (creak.stress >= 0.0 && dt > 0.0 && dt < 1.0 && std::fabs (stress - creak.stress) < 0.3) {
		// creaks at random: more often the faster the stress changes, and
		// the higher it is; groans when a high stress holds
		const double rate = 12.0 * std::fabs (stress - creak.stress) / dt * std::sqrt (stress) + 0.3 * std::pow (stress, 4.0)
			+ 20.0 * std::pow (tide, 3.0);   // (tides near tearing it: creaking all over)
		creak.debt += rate * dt;
		if (creak.debt > 1.0 || CreakRand () < rate * dt * 0.3) {
			creak.debt = std::max (0.0, creak.debt - 1.0);
			int k = (std::fabs (stress - creak.stress) / dt < 0.05 && stress > 0.4) ? CreakSynth::GROAN : CreakSynth::CREAK;
			if (tide > 0.6 && CreakRand () < tide - 0.3) k = CreakSynth::GROAN;   // (heavier the nearer it tears)
			creak.count[k]++;
			p[k] = (float)creak.count[k];
			p[2 + k] = (float)std::min (tide > 0.7 ? 1.5 : 1.0, stress * (0.5 + 0.5 * CreakRand ()) * (1.0 + 0.5 * std::max (0.0, tide - 0.7) / 0.3));
		}
	}
	creak.stress = stress;
	Emitter *cem = world.Get (creak.em);
	CreakSynth *csynth = world.Synth<CreakSynth> (creak.em);
	if (!cem || !csynth) return;
	cem->Push (t, p);
	const Floats<4> h = cem->Recalled<Floats<4>> ();
	for (int k = 0; k < 2; k++) {
		if ((int)h[k] > creak.heard[k]) {
			csynth->Strike ((CreakSynth::Kind)k, h[2 + k] > 0.0f ? h[2 + k] : 0.5f);
			const bool log = Dbg ().log > 0;
			if (log) printf ("Audio: %s at %.2f s (stress %.2f)\n", k ? "groan" : "creak", t, creak.stress), fflush (stdout);
			creak.heard[k] = (int)h[k];
		}
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
	for (auto &th : thunders) world.Remove (th.em);
	thunders.clear ();
	world.Remove (creak.em);
	world.Remove (creak.bed);
	creak = Creak ();
}

} // namespace audio
