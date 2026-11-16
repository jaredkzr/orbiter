// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "VesselSounds.h"
#include "DebugEnv.h"
#include "Ears.h"
#include "RocketSynth.h"
#include "RcsSynth.h"
#include "WindSynth.h"
#include "ContactSynth.h"
#include "ContactVoices.h"
#include "AudioSystem.h"
#include "Vessel.h"
#include "Psys.h"
#include "Camera.h"
#include "VesselAPI.h"
#include "TimeData.h"
#include "Planet.h"
#include "Astro.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>

extern PlanetarySystem *g_psys;
extern Camera *g_camera;
extern TimeData td;

namespace audio {

namespace {
	/// The synthesizers' RMS at full level (Tests/Audio.Synth.cpp holds them to it)
	constexpr double ROCKET_RMS = 0.3, RCS_RMS = 0.3, WIND_RMS = 0.3;
	/// The exhaust's speed counted at most this (a chemical rocket's) [m/s]
	constexpr double VE_MAX = 4500.0, VE_RCS_MAX = 3000.0;
	/// In the cockpit, through the structure: this much of the pressure at
	/// the source's reference distance (a DG's main engines: about 115 dB)
	constexpr double HULL_ENGINE = 0.01, HULL_RCS = 0.02;
	/// The air over the hull: radiated p(r0) = WIND_RAD q (U / 300 m/s);
	/// on the hull (in the cockpit) WIND_WALL q, through it WIND_TRANS of that;
	/// both times Lowson's 1 / (1 + 0.14 M^2) (quieter at high Mach)
	constexpr double WIND_RAD = 0.0004, WIND_WALL = 0.004, WIND_TRANS = 0.018;
	/// Exhaust ahead of a rocket: this much (-20 dB)
	constexpr float ROCKET_FRONT = 0.1f;
	/// Clusters per vessel at most
	constexpr size_t MAX_CLUSTERS = 6;
	/// Contacts: rolling at full (rollAmp 1) is CONTACT_P [Pa] at the
	/// vessel's size (about 100 dB at 10 m, for 15 t: louder heavier); in the
	/// cockpit, through the structure, CONTACT_HULL of it
	constexpr double CONTACT_P = 2.0, CONTACT_RMS = 0.3, CONTACT_HULL = 0.4;
	/// Blows: closing speeds that strike one [m/s] (above the step's
	/// gravity), and the speeds of a full one (a hard landing, a crash, a bump)
	constexpr double BLOW_MIN[3] = {0.35, 0.4, 0.04}, BLOW_FULL[3] = {3.0, 3.5, 0.5};
	constexpr double BLOW_GAP = 0.3; // [s] between blows of a kind
	/// The new contacts (ContactVoices): of a blow's energy (1/2 m v^2) this
	/// much becomes sound - the sprung gear's, rigid metal's (the struts:
	/// -6 dB under the gear's blow); closing speeds of the hardest tier
	constexpr double ETA_GEAR = 1e-5, ETA_METAL = 1e-4, ETA_STRUT = 0.25e-5;
	constexpr double VFULL_GEAR = 3.0, VFULL_METAL = 3.5, VFULL_VESSEL = 0.5;
	/// The cabin's rattle: from RATTLE_A [m/s^2] of deceleration (0.3 g),
	/// RATTLE_P [Pa] at ten times it; not again within RATTLE_GAP [s] unless
	/// twice as hard
	constexpr double RATTLE_A = 0.3 * 9.81, RATTLE_P = 0.4, RATTLE_GAP = 0.3;
	/// A chirp: of the friction's work spinning a wheel up, this much
	/// becomes sound (with the wheel guess of Chirps, a delta-glider's
	/// touchdown at 95 m/s ~0.1 MJ: ~8 dB more energy than its gear's thump -
	/// louder outside, as a real one); in the cabin CHIRP_CABIN of it,
	/// dulled below CHIRP_CABIN_FC (a zip)
	constexpr double ETA_CHIRP = 3e-6, CHIRP_CABIN = 0.056;   // (-25 dB)
	constexpr float CHIRP_CABIN_FC = 1500.0f;
	/// The beds' parameters as pushed (rolling gain, rate, braking gain, rate,
	/// scraping gain, rate, its low-pass)
	using BedParams = Floats<11>;   // (stage 4: then the two motor beds' gain, rate)
	/// The scrape in the cabin: this much louder than outside's share (the
	/// grind carried through the hull; dulled there, ContactVoices)
	constexpr double SCRAPE_CABIN = 1.6;
	/// A tear's, a clatter's, a crash's one-shots: low-passed so (the tear's
	/// in the cabin: dulled with the rest)
	constexpr float TEAR_FC = 12000.0f, CLATTER_FC = 10000.0f, CRASH_FC = 16000.0f;

	V3 ToV3 (const VECTOR3 &v) { return V3 (v.x, v.y, v.z); }

	bool IsRcs (VESSEL *vi, THRUSTER_HANDLE th)
	{
		for (int g = THGROUP_ATT_PITCHUP; g <= THGROUP_ATT_BACK; g++) {
			DWORD n = vi->GetGroupThrusterCount ((THGROUP_TYPE)g);
			for (DWORD i = 0; i < n; i++)
				if (vi->GetGroupThruster ((THGROUP_TYPE)g, i) == th) return true;
		}
		return false;
	}
}

VesselSounds::VesselSounds (Engine &e, World &w): engine (e), world (w)
{
}

VesselSounds::~VesselSounds ()
{
	Clear ();
}

void VesselSounds::Drop (PerVessel &pv)
{
	for (auto *cl : { &pv.engines, &pv.rcs })
		for (auto &c : *cl) world.Remove (c.em);
	pv.engines.clear (), pv.rcs.clear ();
	world.Remove (pv.wind), pv.wind = EmitterId ();
	world.Remove (pv.contact), pv.contact = EmitterId ();
	world.Remove (pv.contactAir), pv.contactAir = EmitterId ();
	world.Remove (pv.contactStruct), pv.contactStruct = EmitterId ();
	for (bool &b : pv.bedTakes) b = false;
	pv.motorSlot[0] = pv.motorSlot[1] = -1;
}

bool VesselSounds::ContactBank ()
{
	if (bankState == 0) {
		const std::string f = System::FindSound ("Sound/Contacts/contacts.txt");
		bankState = !f.empty () && bank.Load (f, engine.SampleRate ()) ? 1 : -1;
		if (bankState < 0) fprintf (stderr, "Audio: no Sound/Contacts/contacts.txt: the contacts are silent\n");
	}
	return bankState > 0;
}

void VesselSounds::Clear ()
{
	for (auto &pv : vessels) Drop (*pv);
	vessels.clear ();
	for (auto &d : detached) world.Remove (d.em);
	detached.clear ();
}

void VesselSounds::Build (PerVessel &pv)
{
	Drop (pv);
	VESSEL *vi = pv.v->GetModuleInterface ();
	if (!vi) return;
	const Tune &tune = Config (pv.v).tune;
	const double size = pv.v->Size ();
	const DWORD nth = vi->GetThrusterCount ();
	pv.nth = nth;

	// clusters: engines by place and direction, attitude thrusters by place
	struct Proto { std::vector<int> th; V3 pos; V3 dir; double maxth = 0.0, isp = 0.0; };
	std::vector<Proto> eng, att;
	const double engR = std::max (3.0, 0.15 * size), attR = std::max (2.5, 0.25 * size);
	for (DWORD i = 0; i < nth; i++) {
		THRUSTER_HANDLE th = vi->GetThrusterHandleByIndex (i);
		VECTOR3 r, d;
		vi->GetThrusterRef (th, r);
		vi->GetThrusterDir (th, d);
		double F = vi->GetThrusterMax0 (th);
		if (F <= 0.0) continue;
		double isp = vi->GetThrusterIsp0 (th);
		bool rcs = IsRcs (vi, th);
		auto &list = rcs ? att : eng;
		double R = rcs ? attR : engR;
		Proto *into = nullptr;
		for (auto &p : list) {
			V3 c = p.pos * (1.0 / p.maxth);
			if ((c - ToV3 (r)).Len () > R) continue;
			if (!rcs && (p.dir * (1.0 / p.maxth)).Dot (ToV3 (d)) < 0.9) continue;
			into = &p;
			break;
		}
		if (!into && list.size () >= MAX_CLUSTERS) {
			// (too many: into the nearest)
			double best = 1e30;
			for (auto &p : list) {
				double dd = (p.pos * (1.0 / p.maxth) - ToV3 (r)).Len ();
				if (dd < best) best = dd, into = &p;
			}
		}
		if (!into) list.push_back (Proto ()), into = &list.back ();
		into->th.push_back ((int)i);
		into->pos = into->pos + ToV3 (r) * F;
		into->dir = into->dir + ToV3 (d) * F;
		into->isp += isp * F;
		into->maxth += F;
	}

	for (auto &p : eng) {
		Cluster c;
		c.th = p.th, c.maxth = p.maxth;
		auto synth = RocketSynth::Make (engine, seed++);
		if (!synth) continue;
		double ve = std::min (p.isp / p.maxth, VE_MAX);
		double D = 0.5 * std::sqrt (p.maxth / 1e5);     // (the nozzle: 0.5 m at 100 kN)
		EmitterDesc d;
		d.body = pv.v;
		d.offset = p.pos * (1.0 / p.maxth);
		V3 dir = p.dir * (1.0 / p.maxth);
		d.dir = dir * -1.0;                               // (the exhaust: against the thrust)
		d.front = ROCKET_FRONT;
		d.refDist = (float)std::max (3.0, 5.0 * D);
		double p0 = PressureAt (JetPower (p.maxth, ve), d.refDist);
		c.p0 = p0;
		d.level = (float)(tune.engineLevel * p0 / P_FULL / ROCKET_RMS);
		d.hull = (float)HULL_ENGINE;
		d.name = std::string (pv.v->Name ()) + " engine " + std::to_string (pv.engines.size ());
		RocketSynth::Params rp;
		rp.peak = RocketSynth::PeakFor (p.maxth);
		rp.crackle = 0.3f;
		rp.rumble = RocketSynth::RumbleFor (p.maxth / p.th.size ());
		rp.pump = RocketSynth::PumpFor (p.maxth / p.th.size ());
		synth->Set (rp);
		c.em = world.Add (d, std::move (synth));
		if (c.em) pv.engines.push_back (c);
	}
	for (auto &p : att) {
		Cluster c;
		c.th = p.th, c.maxth = p.maxth;
		auto synth = RcsSynth::Make (engine, seed++);
		if (!synth) continue;
		double ve = std::min (p.isp / p.maxth, VE_RCS_MAX);
		EmitterDesc d;
		d.body = pv.v;
		d.offset = p.pos * (1.0 / p.maxth);
		d.refDist = 1.5f;
		// (the largest single thruster sets the cluster's loudness: they rarely all fire)
		double Fmax = 0.0;
		VESSEL *vi2 = vi;
		for (int i : p.th) Fmax = std::max (Fmax, vi2->GetThrusterMax0 (vi2->GetThrusterHandleByIndex (i)));
		double p0 = PressureAt (2.0 * JetPower (Fmax, ve), d.refDist);
		c.p0 = p0;
		d.level = (float)(tune.rcsLevel * p0 / P_FULL / RCS_RMS);
		d.hull = (float)HULL_RCS;
		d.name = std::string (pv.v->Name ()) + " rcs " + std::to_string (pv.rcs.size ());
		synth->Set (0.0f, (float)(tune.rcsPitch * RocketSynth::PeakFor (Fmax)));
		c.em = world.Add (d, std::move (synth));
		if (c.em) pv.rcs.push_back (c);
	}

	// its contacts (wheels, hull, docking, animations' motors): off for now,
	// to be reworked (user review 2026-11-12); ORBITER_AUDIO_CONTACTS=1 for
	// development and tests
	const bool contactsOn = Dbg ().contacts;
	auto cs = contactsOn ? ContactSynth::Make (engine, seed++) : nullptr;
	if (cs) {
		EmitterDesc d;
		d.body = pv.v;
		d.refDist = (float)std::max (3.0, 0.5 * size);
		double m = std::max (pv.v->Mass (), 100.0);
		pv.contactMass = std::clamp (std::sqrt (m / 15000.0), 0.3, 5.0);
		d.level = (float)(tune.contactLevel * CONTACT_P * pv.contactMass / P_FULL / CONTACT_RMS);
		d.hull = (float)CONTACT_HULL;
		d.name = std::string (pv.v->Name ()) + " contacts";
		pv.contact = world.Add (d, std::move (cs));
	}

	// (ORBITER_AUDIO_CONTACTS=2) the new contacts: heard outside (its blows'
	// energy at the reference distance: levels set per one-shot), and
	// through its structure in its cabin (one-shots' full scale P_FULL)
	if (Dbg ().contactVoices && ContactBank ()) {
		auto air = ContactVoices::Make (engine, ContactVoices::AIR, size, seed++);
		auto st = ContactVoices::Make (engine, ContactVoices::STRUCTURE, size, seed++);
		if (air && st) {
			EmitterDesc d;
			d.body = pv.v;
			d.refDist = (float)(pv.contactRef = std::max (3.0, 0.5 * size));
			d.level = (float)tune.contactLevel;
			d.name = std::string (pv.v->Name ()) + " contacts";
			pv.contactAir = world.Add (d, std::move (air));
			pv.mechRng = Uniforms (seed++);
			pv.contactStruct = world.Add (StructureEmitter (pv.v, P_FULL * tune.contactLevel, std::string (pv.v->Name ()) + " contacts (structure)"), std::move (st));
		}
	}

	// the air over it
	auto ws = WindSynth::Make (engine, seed++);
	if (ws) {
		EmitterDesc d;
		d.body = pv.v;
		d.refDist = (float)std::max (3.0, 0.5 * size);
		d.level = 0.0f;
		d.boom = true;
		d.length = (float)(2.0 * size);
		d.name = std::string (pv.v->Name ()) + " air";
		pv.wind = world.Add (d, std::move (ws));
	}
}

void VesselSounds::UpdateVessel (PerVessel &pv)
{
	const double t = now;
	VESSEL *vi = pv.v->GetModuleInterface ();
	if (!vi) return;
	if (vi->GetThrusterCount () != pv.nth) Build (pv);
	VesselConfig &cfg = Config (pv.v);
	const Tune &tune = cfg.tune;
	// the motors of animations with a sound: running while they move, a
	// latch or lock at their ends
	for (int k = 0; k < 3; k++) pv.motor[k] = 0.0f;
	for (AnimSound &a : cfg.anims) {
		double st = vi->GetAnimation (a.anim);
		if (a.last >= 0.0 && dtNow > 0.0) {
			double speed = std::fabs (st - a.last) / dtNow;      // [1/s]: a full travel in 1/speed s
			int mk = a.kind == ANIM_GEAR ? ContactSynth::GEAR : a.kind == ANIM_DOOR ? ContactSynth::DOOR : ContactSynth::SERVO;
			bool moving = speed > 1e-4;
			if (moving) pv.motor[mk] = std::max (pv.motor[mk], (float)(a.level * std::min (1.0, 0.4 + 4.0 * speed)));
			// (stopped at an end: it closed, locked)
			if (a.moving && !moving && (st < 1e-6 || st > 1.0 - 1e-6)) {
				int b = a.kind == ANIM_GEAR ? ContactSynth::GEARLOCK : ContactSynth::LATCH;
				pv.blows[b]++, pv.blowStrength[b] = (float)(a.level * (a.kind == ANIM_MOTOR ? 0.3 : 0.8));
			}
			a.moving = moving;
		}
		a.last = st;
	}

	for (auto &c : pv.engines) {
		double lv = 0.0;
		for (int i : c.th) {
			THRUSTER_HANDLE th = vi->GetThrusterHandleByIndex (i);
			lv += vi->GetThrusterLevel (th) * vi->GetThrusterMax0 (th);
		}
		Emitter *em = world.Get (c.em);
		RocketSynth *rocket = world.Synth<RocketSynth> (c.em);
		if (!em || !rocket) continue;
		// (as heard: when the sound now arriving left it)
		em->Push (t, Floats<1>{{(float)(lv / c.maxth)}});   // (the throttle)
		RocketSynth::Params rp;
		rp.level = em->Recalled<Floats<1>> ()[0];
		rp.peak = (float)(tune.enginePitch * RocketSynth::PeakFor (c.maxth));
		rp.crackle = (float)(0.3 * tune.crackle);
		rp.rumble = (float)(tune.rumble * RocketSynth::RumbleFor (c.maxth / c.th.size ()));
		rp.whine = (float)(0.15 * tune.whine);
		rp.pump = (float)(tune.enginePitch * RocketSynth::PumpFor (c.maxth / c.th.size ()));
		rocket->Set (rp);
		// (its tune, on or off: as they are now)
		em->SetLevel (cfg.on[SOUND_ENGINES] ? (float)(tune.engineLevel * c.p0 / P_FULL / ROCKET_RMS) : 0.0f);
	}
	for (auto &c : pv.rcs) {
		double lv = 0.0, Fmax = 0.0;
		for (int i : c.th) {
			THRUSTER_HANDLE th = vi->GetThrusterHandleByIndex (i);
			double F = vi->GetThrusterMax0 (th);
			lv = std::max (lv, vi->GetThrusterLevel (th));
			Fmax = std::max (Fmax, F);
		}
		Emitter *em = world.Get (c.em);
		RcsSynth *rcs = world.Synth<RcsSynth> (c.em);
		if (!em || !rcs) continue;
		em->Push (t, Floats<1>{{(float)lv}});
		rcs->Set (em->Recalled<Floats<1>> ()[0], (float)(tune.rcsPitch * RocketSynth::PeakFor (Fmax)));
		em->SetLevel (cfg.on[SOUND_RCS] ? (float)(tune.rcsLevel * c.p0 / P_FULL / RCS_RMS) : 0.0f);
	}
	Emitter *wind = world.Get (pv.wind);
	WindSynth *windSynth = world.Synth<WindSynth> (pv.wind);
	if (wind && windSynth) {
		double q = 0.0, M = 0.0;
		pv.v->DynPressure (q);
		pv.v->MachNumber (M);
		const SurfParam *sp = pv.v->GetSurfParam ();
		double U = sp ? sp->airspd : 0.0;
		if (replaying) {
			// (the state shown: the air's speed from the velocity over the turning
			// planet, its density there; the surface parameters are the live ones)
			q = M = U = 0.0;
			if (const Planet *pl = pv.v->ProxyPlanet ()) {
				Vector r (pv.v->GPos () - pl->GPos ()), v (pv.v->GVel () - pl->GVel ());
				Vector w (mul (pl->GRot (), Vector (0, Pi2 / pl->RotT (), 0)));
				Vector va (v - crossp (r, w));        // (left-handed: the ground's velocity is r x w)
				U = va.length ();
				double g[3] = {pv.v->GPos ().x, pv.v->GPos ().y, pv.v->GPos ().z};
				Air a = World::AirAt (pl, g);
				q = 0.5 * a.rho * U * U;
				M = a.c > 0.0 ? U / a.c : 0.0;
			}
		}
		wind->Push (t, Floats<4>{{(float)U, WindSynth::BuffetFor (vi->GetAOA (), M), (float)q, (float)M}}); // (speed, buffet, q, Mach)
		const Floats<4> h = wind->Recalled<Floats<4>> ();
		WindSynth::Params wp;
		wp.speed = h[0];
		wp.buffet = h[1];
		windSynth->Set (wp);
		double qh = h[2], Uh = h[0];
		// (the turbulent boundary layer's pressure fluctuations fall off at
		// high Mach: Lowson's p_rms / q = 0.006 / (1 + 0.14 M^2) for an attached
		// layer - at Mach 20, a 57th of the subsonic: a reentry's first part is
		// quiet in the cabin, its loud part lower and slower)
		const double lowson = 1.0 / (1.0 + 0.14 * (double)h[3] * (double)h[3]);
		double p0 = WIND_RAD * qh * (Uh / 300.0) * lowson;
		wind->SetLevel (cfg.on[SOUND_AIR] ? (float)(tune.airLevel * p0 / P_FULL / WIND_RMS) : 0.0f);
		double cabin = WIND_WALL * qh * lowson * WIND_TRANS;
		wind->SetHull (p0 > 1e-9 ? (float)(cabin / p0) : 0.0f);
	}
	if (Emitter *ca = world.Get (pv.contactAir)) ca->SetLevel (cfg.on[SOUND_CONTACTS] ? (float)tune.contactLevel : 0.0f);
	if (Dbg ().contactVoices) UpdateMotors (pv, cfg);
	UpdateContactBeds (pv);
	if (Emitter *cs = world.Get (pv.contactStruct))
		cs->SetLevel (cfg.on[SOUND_CONTACTS] ? StructureEmitter (pv.v, P_FULL * tune.contactLevel, "").level : 0.0f);
	if (Emitter *ce = world.Get (pv.contact)) {
		ce->SetLevel (cfg.on[SOUND_CONTACTS] ? (float)(tune.contactLevel * CONTACT_P * pv.contactMass / P_FULL / CONTACT_RMS) : 0.0f);
		UpdateContacts (pv, t);
	}
}

bool VesselSounds::Tune::Set (const std::string &key, double v)
{
	static const struct { const char *k; double Tune::*m; } keys[] = {
		{"engine_level", &Tune::engineLevel}, {"engine_pitch", &Tune::enginePitch}, {"rumble", &Tune::rumble},
		{"whine", &Tune::whine}, {"crackle", &Tune::crackle}, {"rcs_level", &Tune::rcsLevel},
		{"rcs_pitch", &Tune::rcsPitch}, {"air_level", &Tune::airLevel}, {"contact_level", &Tune::contactLevel},
	};
	for (auto &k : keys) {
		if (key != k.k) continue;
		this->*k.m = std::clamp (v, 0.0, 10.0);
		return true;
	}
	return false;
}

VesselSounds::VesselConfig &VesselSounds::Config (const Vessel *v)
{
	const BodyRef r = world.Ref (v);
	auto it = configs.find (r.serial);
	if (it != configs.end ()) return it->second;
	VesselConfig &c = configs[r.serial];
	c.ref = r;
	// Config/Sound/<class>.cfg: "key = value" lines (Tune), and
	// "Anim = <index> <motor|door|gear> [level]"; ';' starts a comment
	const char *cls = v->ClassName ();
	if (!cls || !*cls) return c;
	std::ifstream f (std::string ("Config/Sound/") + cls + ".cfg");
	std::string line;
	while (std::getline (f, line)) {
		size_t sc = line.find (';');
		if (sc != std::string::npos) line.resize (sc);
		size_t eq = line.find ('=');
		if (eq == std::string::npos) continue;
		auto trim = [](std::string x) {
			size_t a = x.find_first_not_of (" \t\r"), b = x.find_last_not_of (" \t\r");
			return a == std::string::npos ? std::string () : x.substr (a, b - a + 1);
		};
		std::string key = trim (line.substr (0, eq)), val = trim (line.substr (eq + 1));
		for (auto &ch : key) ch = (char)std::tolower ((unsigned char)ch);
		if (key == "anim") {
			unsigned idx; char kind[32] = ""; double level = 1.0;
			if (sscanf (val.c_str (), "%u %31s %lf", &idx, kind, &level) >= 2) {
				std::string k (kind);
				int kk = k == "gear" ? ANIM_GEAR : k == "door" ? ANIM_DOOR : k == "motor" ? ANIM_MOTOR : ANIM_NONE;
				if (kk) c.anims.push_back ({idx, kk, (float)level});
			}
		} else {
			c.tune.Set (key, atof (val.c_str ()));
		}
	}
	return c;
}

bool VesselSounds::TuneVessel (const Vessel *v, const std::string &key, double value)
{
	return Config (v).tune.Set (key, value);   // (heard from the next step: levels and pitches follow it)
}

bool VesselSounds::Enable (const Vessel *v, int sound, bool on)
{
	if (!v || sound < 0 || sound >= NSOUND) return false;
	Config (v).on[sound] = on;
	return true;
}

void VesselSounds::Animation (const Vessel *v, unsigned anim, int kind, double level)
{
	auto &anims = Config (v).anims;
	anims.erase (std::remove_if (anims.begin (), anims.end (), [anim](const AnimSound &a) { return a.anim == anim; }), anims.end ());
	if (kind > ANIM_NONE && kind <= ANIM_GEAR) anims.push_back ({anim, kind, (float)std::clamp (level, 0.0, 4.0)});
}

int VesselSounds::GearAnim (const Vessel *v) const
{
	auto it = configs.find (world.Known (v).serial);
	if (it == configs.end ()) return -1;
	for (auto &a : it->second.anims) if (a.kind == ANIM_GEAR) return (int)a.anim;
	return -1;
}

VesselSounds::PerVessel *VesselSounds::Find (const Vessel *v)
{
	const BodyRef r = world.Known (v);
	if (!r) return nullptr;
	for (auto &p : vessels) if (p->ref == r) return p.get ();
	return nullptr;
}

void VesselSounds::Contacts (const Vessel *v, const std::vector<ContactSample> &pts, SurfaceKind surface, double t, double h)
{
	PerVessel *pv = Find (v);
	if (!pv || pts.empty ()) return;
	pv->contacts.Step (t, h, pts, surface);
	PlayContacts (*pv, pts, t);
	if (Emitter *ca = world.Get (pv->contactAir)) {
		// the beds: their pressures at 10 m as at the reference distance (spherical)
		const ContactLevels &lv = pv->contacts.Levels ();
		const ContactBeds b = BedsFor (lv);
		const double k = 10.0 / pv->contactRef / P_FULL;
		const double burst = pv->scrapeBurst.Step (t, lv.scrapePower + lv.vesselScrape);
		BedParams bp;
		bp[0] = (float)(b.rollP * k), bp[1] = (float)b.rollRate, bp[2] = (float)(b.brakeP * k), bp[3] = (float)b.brakeRate;
		bp[4] = (float)(b.scrapeP * burst * k), bp[5] = (float)b.scrapeRate, bp[6] = (float)b.scrapeCutoff;
		for (int i = 0; i < 4; i++) bp[7 + i] = pv->motorAir[i];
		ca->Push (t, bp);
		bp[4] = (float)(bp[4] * SCRAPE_CABIN);
		for (int i = 0; i < 4; i++) bp[7 + i] = pv->motorCab[i];
		if (Emitter *cs = world.Get (pv->contactStruct)) cs->Push (t, bp);
		pv->bedPushT = t;
	}
	LogContacts (v, pv->contacts);
	// (ContactSynth's: the step summed)
	const ContactLevels &l = pv->contacts.Levels ();
	ContactStep &s = pv->step;
	s.wheelLoad += l.wheelLoad;
	s.squealPower += l.scrubPower + 0.3 * l.brakePower;
	s.scrapePower += l.scrapePower;
	s.scrapeSpeed = std::max (s.scrapeSpeed, l.scrapeSpeed);
	V3 wpos;
	double wsum = 0.0;
	for (const ContactSample &p : pts) {
		double &blow = p.kind == ContactKind::WHEEL ? s.blowWheel : p.kind == ContactKind::VESSEL ? s.blowVessel : s.blowHull;
		blow = std::max (blow, p.vn);
		double w = p.Fn + std::hypot (p.Flong, p.Flat);
		wpos = wpos + p.pos * w, wsum += w;
	}
	if (wsum > 0.0) s.where = wpos * (1.0 / wsum);
	s.any = true;
}

void VesselSounds::PlayContacts (PerVessel &pv, const std::vector<ContactSample> &pts, double t)
{
	Emitter *air = world.Get (pv.contactAir), *st = world.Get (pv.contactStruct);
	if (!air || !st) return;
	const double m = std::max (pv.v->Mass (), 100.0);
	const float rate = (float)std::pow (m / 15000.0, -0.12);   // (heavier: lower)
	// a take of a layer at a blow's energy E [J] (eta of it sound), on both:
	// its gain for the energy at the reference distance (p^2 integrated:
	// eta E rho c / 4 pi r0^2), its tier's share
	auto play = [&](int kind, const char *layer, SampleBank::Tier tier, float share, double E, double eta, float cutoff, const V3 &pos, double tAt) {
		SampleBank::Take k = bank.Pick (layer, tier);
		if (!k || k.energy <= 0.0) return;
		const double p2 = eta * E * RHO0 * C0 / (4.0 * 3.14159265358979 * pv.contactRef * pv.contactRef);
		const float g = (float)(std::sqrt (p2 * share / k.energy) / P_FULL) * k.gain;
		const OneShot s = ContactVoices::Shot (kind, k.clip.get (), g, k.rate * rate, cutoff, 1.0f);
		air->SetOffset (pos);
		air->Schedule (tAt, s);
		st->Schedule (tAt, s);
	};
	for (const ContactEvent &e : pv.contacts.Events ()) {
		if (e.type != ContactEvent::IMPACT) continue;
		const bool gear = e.kind == ContactKind::WHEEL || e.kind == ContactKind::LEG;
		const double vfull = gear ? VFULL_GEAR : e.kind == ContactKind::VESSEL ? VFULL_VESSEL : VFULL_METAL;
		const float s = (float)std::clamp (e.speed / vfull, 0.0, 1.0);
		const float cutoff = (float)(1500.0 * std::pow (8.0, s));   // (1.5 kHz soft .. 12 kHz hard)
		const SampleBank::Blend b = SampleBank::ForStrength (s);
		const char *layer = gear ? "impact_gear" : "impact_metal";
		const double eta = gear ? ETA_GEAR : ETA_METAL;
		play (ContactVoices::IMPACT, layer, b.tier, 1.0f - b.w, e.energy, eta, cutoff, e.pos, e.t);
		if (b.w > 0.05f) play (ContactVoices::IMPACT, layer, b.next, b.w, e.energy, eta, cutoff, e.pos, e.t);
		if (gear) play (ContactVoices::STRUT, "strut", SampleBank::ANY, 1.0f, e.energy, ETA_STRUT, 12000.0f, e.pos, e.t);
	}
	// the tyres' chirps: a wheel touching down sliding (Chirps), the take
	// played the spin-up long, its level from the friction's work
	for (const Chirp &c : pv.chirps.Step (t, pts, pv.contacts.Events (), m)) {
		SampleBank::Take k = bank.Pick ("chirp");
		if (!k || k.energy <= 0.0 || !k.clip->frames) continue;
		const double dur = (double)k.clip->frames / k.clip->rate;
		const double len = std::min (c.spinUp, dur);
		const double played = k.energy * (len / dur) * 0.8;   // (the energy played: so much of it, its fade)
		const double p2 = ETA_CHIRP * c.work * RHO0 * C0 / (4.0 * 3.14159265358979 * pv.contactRef * pv.contactRef);
		const float g = (float)(std::sqrt (p2 / played) / P_FULL) * k.gain;
		// (faster: a little higher)
		const float rate = k.rate * (float)(CHIRP_RATE0 + (CHIRP_RATE1 - CHIRP_RATE0) * std::clamp ((c.speed - 15.0) / (CHIRP_RATE_V - 15.0), 0.0, 1.0));
		V3 pos;
		for (const ContactSample &p : pts) if (p.id == c.id) pos = p.pos;
		air->SetOffset (pos);
		air->Schedule (c.t, ContactVoices::Shot (ContactVoices::CHIRP, k.clip.get (), g, rate, 16000.0f, 0.0f, (float)len));
		st->Schedule (c.t, ContactVoices::Shot (ContactVoices::CHIRP, k.clip.get (), g * (float)CHIRP_CABIN, rate, CHIRP_CABIN_FC, 0.0f, (float)len));
		if (Dbg ().log > 0)
			printf ("Audio: contact of %s: chirp wheel %d at %.3f s: %.1f m/s, spin-up %.3f s, %.3g J\n", pv.v->Name (), c.id, c.t, c.speed, c.spinUp, c.work), fflush (stdout);
	}
	// the cabin's rattle: decelerated over RATTLE_A (the contacts' force
	// beyond holding its weight up; braking), as loud as log a - of the
	// blow's peak: rattled when it falls off it (or holds, braking), at
	// that moment; again only after it has eased, or twice as hard
	double Fn = 0.0, Ft = 0.0;
	for (const ContactSample &p : pts) Fn += p.Fn, Ft += std::fabs (p.Flong);
	double g = 0.0;
	if (const CelestialBody *pb = pv.v->ProxyBody ())
		g = Ggrav * pb->Mass () / std::max (pv.v->GPos ().dist2 (pb->GPos ()), 1.0);
	const double a = std::max (Fn / m - g, Ft / m);
	if (t - pv.lastContactT > 0.1) pv.rattleOn = pv.rattleFired = false;   // (touching again after a while: from rest)
	pv.lastContactT = t;
	if (a > RATTLE_A) {
		if (!pv.rattleOn || a > pv.peakA) pv.peakA = a, pv.peakT = t;
		pv.rattleOn = true;
		if (pv.rattleFired && a > 2.0 * pv.rattleA && t - pv.lastRattle > RATTLE_GAP) pv.rattleFired = false, pv.peakA = a, pv.peakT = t;
	}
	if (pv.rattleOn && !pv.rattleFired && (a < 0.8 * pv.peakA || t - pv.peakT > 0.1)) {
		SampleBank::Take k = bank.Pick ("rattle");
		if (k && k.energy > 0.0 && k.clip->frames > 0) {
			const double p = RATTLE_P * std::clamp (std::log10 (pv.peakA / RATTLE_A), 0.1, 1.0);
			const double rms = std::sqrt (k.energy / ((double)k.clip->frames / k.clip->rate));
			st->Schedule (pv.peakT, ContactVoices::Shot (ContactVoices::RATTLE, k.clip.get (), (float)(p / P_FULL / rms) * k.gain, k.rate, 12000.0f, 0.3f));
			if (Dbg ().log > 0) printf ("Audio: contact of %s: rattle at %.3f s: %.2f g\n", pv.v->Name (), pv.peakT, pv.peakA / 9.81), fflush (stdout);
		}
		pv.rattleFired = true, pv.rattleA = pv.peakA, pv.lastRattle = t;
	}
	if (a <= RATTLE_A) pv.rattleOn = pv.rattleFired = false;
}

void VesselSounds::UpdateContactBeds (PerVessel &pv)
{
	Emitter *air = world.Get (pv.contactAir), *st = world.Get (pv.contactStruct);
	ContactVoices *va = world.Synth<ContactVoices> (pv.contactAir), *vs = world.Synth<ContactVoices> (pv.contactStruct);
	if (!air || !st || !va || !vs) return;
	// their takes (two of each, at unit RMS), once there
	static const char *layer[ContactVoices::BEDS] = {"rolling", "brake", "scrape", "motor", "motor"};
	for (int b = 0; b < ContactVoices::BEDS; b++) {
		if (pv.bedTakes[b]) continue;
		const Clip *c[2] = {};
		float g[2] = {1.0f, 1.0f};
		int n = 0;
		const int takes = std::min (2, std::max (1, bank.Count (layer[b])));   // (none there: one Pick - its one warning)
		for (int i = 0; i < takes; i++) {
			SampleBank::Take k = bank.Pick (layer[b]);
			if (!k || k.energy <= 0.0 || !k.clip->frames) continue;
			if (n && k.clip.get () == c[0]) continue;
			c[n] = k.clip.get (), g[n] = (float)(1.0 / std::sqrt (k.energy / ((double)k.clip->frames / k.clip->rate))), n++;
		}
		if (!n) continue;
		if (va->SetBedTakes (b, c[0], g[0], c[1], g[1]) && vs->SetBedTakes (b, c[0], g[0], c[1], g[1])) pv.bedTakes[b] = true;
	}
	// (no contacts this while: silence, once)
	if (now - pv.bedPushT > 0.1) {
		BedParams z, zc;
		z[1] = z[3] = z[5] = 1.0f;
		for (int i = 0; i < 4; i++) z[7 + i] = pv.motorAir[i];
		zc = z;
		for (int i = 0; i < 4; i++) zc[7 + i] = pv.motorCab[i];
		BedParams last;
		bool same = air->Latest (last);
		for (int i = 0; i < 11 && same; i++) same = last[i] == z[i];
		BedParams lastc;
		bool samec = st->Latest (lastc);
		for (int i = 0; i < 11 && samec; i++) samec = lastc[i] == zc[i];
		if (!same || !samec) air->Push (now, z), st->Push (now, zc);
	}
	// as heard now (the travel time ago outside; at once in its cabin)
	const BedParams a = air->Recalled<BedParams> (), s = st->Recalled<BedParams> ();
	va->SetBed (ContactVoices::ROLL, a[0], a[1]), va->SetBed (ContactVoices::BRAKE, a[2], a[3]);
	vs->SetBed (ContactVoices::ROLL, s[0], s[1]), vs->SetBed (ContactVoices::BRAKE, s[2], s[3]);
	va->SetBed (ContactVoices::SCRAPE, a[4], a[5], a[6]), vs->SetBed (ContactVoices::SCRAPE, s[4], s[5], s[6]);
	va->SetBed (ContactVoices::MOTOR_A, a[7], a[8] > 0.0f ? a[8] : 1.0f), vs->SetBed (ContactVoices::MOTOR_A, s[7], s[8] > 0.0f ? s[8] : 1.0f);
	va->SetBed (ContactVoices::MOTOR_B, a[9], a[10] > 0.0f ? a[10] : 1.0f), vs->SetBed (ContactVoices::MOTOR_B, s[9], s[10] > 0.0f ? s[10] : 1.0f);
	// (the log, with the sources' states: twice a second while either sounds - at the reference
	// distance as wanted, [Pa], and as playing outside, eased, [Pa])
	if (Dbg ().logStates && (s[7] > 0.0f || s[9] > 0.0f || vs->BedLevel (ContactVoices::MOTOR_A) > 0.0f || vs->BedLevel (ContactVoices::MOTOR_B) > 0.0f) && now - pv.motorLogT >= 0.5) {
		pv.motorLogT = now;
		printf ("Audio: t %.2f %s motors in the cabin: %.3g Pa (playing %.3g, rate %.2f), %.3g Pa (playing %.3g, rate %.2f)\n",
			now, pv.v->Name (), s[7] * P_FULL, vs->BedLevel (ContactVoices::MOTOR_A) * P_FULL, s[8], s[9] * P_FULL, vs->BedLevel (ContactVoices::MOTOR_B) * P_FULL, s[10]);
		fflush (stdout);
	}
	if (Dbg ().logStates && (a[0] > 0.0f || a[2] > 0.0f || a[4] > 0.0f || va->BedLevel (0) > 0.0f || va->BedLevel (1) > 0.0f || va->BedLevel (2) > 0.0f) && now - pv.bedLogT >= 0.5) {
		pv.bedLogT = now;
		printf ("Audio: t %.2f %s contact beds: rolling %.3g Pa (playing %.3g, rate %.2f), braking %.3g Pa (playing %.3g, rate %.2f), scraping %.3g Pa (playing %.3g, rate %.2f, below %.0f Hz) at %.0f m\n",
			now, pv.v->Name (), a[0] * P_FULL, va->BedLevel (0) * P_FULL, a[1], a[2] * P_FULL, va->BedLevel (1) * P_FULL, a[3],
			a[4] * P_FULL, va->BedLevel (2) * P_FULL, a[5], a[6], pv.contactRef);
		fflush (stdout);
	}
}

void VesselSounds::LogContacts (const Vessel *v, ContactModel &m)
{
	if (Dbg ().log > 0)
		for (const ContactEvent &e : m.Events ())
			printf ("Audio: contact of %s: %s\n", v->Name (), Describe (e).c_str ()), fflush (stdout);
	m.ClearEvents ();
}

void VesselSounds::Docked (const Vessel *v, const V3 &port, bool dock, double speed, double mred)
{
	PerVessel *pv = Find (v);
	if (!pv) return;
	if (dock) pv->contacts.Dock (now, port, speed, mred);
	else pv->contacts.Undock (now, port, speed);
	LogContacts (v, pv->contacts);
	// (stage 4) the mechanisms: docking's blow, the capture latch, the hooks
	// closing, the seal; undocking's release, the hooks opening, the push-off
	// - in its cabin (its structure: both vessels' while docked) at fixed
	// levels, outside less (in vacuum not at all)
	if (Dbg ().contactVoices && ContactBank ()) {
		const double t = now;
		const double m = std::max (v->Mass (), 100.0);
		const auto tl = dock ? DockTimeline (speed, mred, pv->mechRng) : UndockTimeline (speed, m, pv->mechRng);
		int n = 0, hooks = 0;
		double tSeal = -1.0, tHook0 = -1.0, tHook1 = -1.0;
		for (const MechShot &s : tl) {
			bool ok = false;
			switch (s.what) {
			case MechShot::BLOW:
				ok = PlayBlow (*pv, "impact_metal", std::clamp (speed / VFULL_VESSEL, 0.0, 1.0), s.level, ContactVoices::IMPACT, t + s.t, port);
				break;
			case MechShot::PUSH:
				ok = PlayBlow (*pv, "impact_metal", 0.0, s.level, ContactVoices::IMPACT, t + s.t, port);
				break;
			case MechShot::LATCH:
				ok = PlayFixed (*pv, "latch", SampleBank::ANY, s.level, MECH_AIR, ContactVoices::LATCH, t + s.t, (float)s.rate, 16000.0f, port);
				break;
			case MechShot::HOOK:
				ok = PlayFixed (*pv, "hook", SampleBank::ANY, s.level, MECH_AIR, ContactVoices::HOOK, t + s.t, (float)s.rate, 16000.0f, port);
				if (ok) { if (!hooks) tHook0 = s.t; tHook1 = s.t; hooks++; }
				break;
			case MechShot::RELEASE:
				ok = PlayFixed (*pv, "release", SampleBank::ANY, s.level, MECH_AIR, ContactVoices::RELEASE, t + s.t, (float)s.rate, 16000.0f, port);
				break;
			case MechShot::SEAL:
				ok = PlayFixed (*pv, "seal", SampleBank::ANY, s.level, MECH_AIR, ContactVoices::SEAL, t + s.t, (float)s.rate, 16000.0f, port);
				if (ok) tSeal = s.t;
				break;
			}
			n += ok ? 1 : 0;
		}
		if (Dbg ().log > 0) {
			printf ("Audio: contact of %s: %s heard: %d one-shots, %d hooks", v->Name (), dock ? "docking" : "undocking", n, hooks);
			if (hooks) printf (" %.2f-%.2f s on", tHook0, tHook1);
			if (tSeal >= 0.0) printf (", the seal %.2f s on", tSeal);
			printf ("\n"), fflush (stdout);
		}
	}
	Emitter *contact = world.Get (pv->contact);
	if (!contact) return;
	int k = dock ? ContactSynth::DOCK : ContactSynth::UNDOCK;
	pv->blows[k]++, pv->blowStrength[k] = 1.0f;
	const bool log = Dbg ().log > 0;
	if (log) printf ("Audio: %s of %s\n", dock ? "docking" : "undocking", v->Name ()), fflush (stdout);
	contact->SetOffset (port);
}

bool VesselSounds::PlayFixed (PerVessel &pv, const char *layer, int tier, double p, double airShare, int kind, double tAt, float rate, float cutoff, const V3 &pos)
{
	Emitter *air = world.Get (pv.contactAir), *st = world.Get (pv.contactStruct);
	if ((!air && !st) || p <= 0.0) return false;
	SampleBank::Take k = bank.Pick (layer, (SampleBank::Tier)tier);
	if (!k || k.energy <= 0.0 || !k.clip->frames) return false;
	// (its RMS over its length the level: p in the cabin, airShare of it outside at the reference distance)
	const double rms = std::sqrt (k.energy / ((double)k.clip->frames / k.clip->rate));
	const float g = (float)(p / P_FULL / rms) * k.gain;
	bool ok = false;
	if (st) ok = st->Schedule (tAt, ContactVoices::Shot (kind, k.clip.get (), g, k.rate * rate, cutoff, 0.0f)) || ok;
	if (air && airShare > 0.0) {
		air->SetOffset (pos);
		ok = air->Schedule (tAt, ContactVoices::Shot (kind, k.clip.get (), g * (float)airShare, k.rate * rate, cutoff, 0.0f)) || ok;
	}
	return ok;
}

bool VesselSounds::PlayBlow (PerVessel &pv, const char *layer, double strength, double E, int kind, double tAt, const V3 &pos)
{
	Emitter *air = world.Get (pv.contactAir), *st = world.Get (pv.contactStruct);
	if ((!air && !st) || E <= 0.0) return false;
	const float s = (float)std::clamp (strength, 0.0, 1.0);
	const float cutoff = (float)(1500.0 * std::pow (8.0, s));   // (as the impacts: 1.5 kHz soft .. 12 kHz hard)
	const SampleBank::Blend b = SampleBank::ForStrength (s);
	const double p2 = E * RHO0 * C0 / (4.0 * 3.14159265358979 * pv.contactRef * pv.contactRef);
	bool ok = false;
	for (int i = 0; i < 2; i++) {
		const float share = i ? b.w : 1.0f - b.w;
		if (share <= 0.05f) continue;
		SampleBank::Take k = bank.Pick (layer, i ? b.next : b.tier);
		if (!k || k.energy <= 0.0) continue;
		const float g = (float)(std::sqrt (p2 * share / k.energy) / P_FULL) * k.gain;
		const OneShot sh = ContactVoices::Shot (kind, k.clip.get (), g, k.rate, cutoff, 1.0f);
		if (air) { air->SetOffset (pos); ok = air->Schedule (tAt, sh) || ok; }
		if (st) ok = st->Schedule (tAt, sh) || ok;
	}
	return ok;
}

void VesselSounds::UpdateMotors (PerVessel &pv, VesselConfig &cfg)
{
	for (int i = 0; i < 4; i++) pv.motorAir[i] = pv.motorCab[i] = (i % 2) ? 1.0f : 0.0f;
	VESSEL *vi = pv.v->GetModuleInterface ();
	if (!vi) return;
	const double t = now;
	static const char *kindName[4] = {"", "motor", "door", "gear"};
	// the moments: started; stopped at an end (the lock, the gear's thud, a door's seal); stopped on the way
	for (AnimSound &a : cfg.anims) {
		if (a.phase == 0.0) a.phase = 6.28318530718 * pv.mechRng () + 1e-3;
		const MotorTrack::Event e = a.track.Step (t, dtNow, vi->GetAnimation (a.anim));
		const int k = std::clamp (a.kind - 1, 0, 2);
		if (e == MotorTrack::END) {
			PlayFixed (pv, "latch", SampleBank::ANY, LOCK_P[k] * a.level, MECH_AIR, ContactVoices::LOCK, t + LOCK_T, 1.0f, 16000.0f, V3 ());
			if (a.kind == ANIM_GEAR) PlayFixed (pv, "impact_gear", SampleBank::SOFT, GEAR_THUD_P * a.level, MECH_AIR, ContactVoices::THUD, t + LOCK_T, 1.0f, 4000.0f, V3 ());
			if (a.kind == ANIM_DOOR) PlayFixed (pv, "seal", SampleBank::ANY, SEAL_DOOR_P * a.level, MECH_AIR, ContactVoices::SEAL, t + DOOR_SEAL_T, 1.0f, 16000.0f, V3 ());
		}
		if (e != MotorTrack::NONE && Dbg ().log > 0)
			printf ("Audio: motor of %s: animation %u (%s) %s at %.3f s\n", pv.v->Name (), a.anim, kindName[std::clamp (a.kind, 0, 3)],
				e == MotorTrack::START ? "starts" : e == MotorTrack::END ? "stops at its end" : "stops on the way", t), fflush (stdout);
	}
	// the two beds: one playing keeps its animation while it moves; a free one
	// takes the moving one first by kind (gear, door, motor), then level
	auto find = [&](int anim) -> AnimSound * {
		for (AnimSound &a : cfg.anims) if ((int)a.anim == anim) return &a;
		return nullptr;
	};
	for (int &s : pv.motorSlot) {
		AnimSound *a = s >= 0 ? find (s) : nullptr;
		if (!a || !a->track.Moving ()) s = -1;
	}
	for (int s = 0; s < 2; s++) {
		if (pv.motorSlot[s] >= 0) continue;
		AnimSound *best = nullptr;
		for (AnimSound &a : cfg.anims) {
			if (!a.track.Moving () || (int)a.anim == pv.motorSlot[1 - s]) continue;
			if (!best || a.kind > best->kind || (a.kind == best->kind && a.level > best->level)) best = &a;
		}
		if (best) pv.motorSlot[s] = (int)best->anim;
	}
	for (int s = 0; s < 2; s++) {
		AnimSound *a = pv.motorSlot[s] >= 0 ? find (pv.motorSlot[s]) : nullptr;
		if (!a) continue;
		const MotorSound ms = MotorFor (a->kind - 1, a->level, a->track.Speed (), t, a->phase);
		pv.motorCab[2 * s] = (float)(ms.p / P_FULL), pv.motorCab[2 * s + 1] = (float)ms.rate;
		pv.motorAir[2 * s] = (float)(ms.p * MOTOR_AIR / P_FULL), pv.motorAir[2 * s + 1] = (float)ms.rate;
	}
}

Emitter *VesselSounds::Detach (PerVessel &pv, const V3 &pos, double dur, const char *what)
{
	const Planet *pl = pv.v->ProxyPlanet ();
	if (!pl) return nullptr;
	auto cv = ContactVoices::Make (engine, ContactVoices::AIR, pv.v->Size (), seed++);
	if (!cv) return nullptr;
	// where it happened, in the planet's frame (it turns while the sound comes)
	const Vector g = pv.v->GPos () + mul (pv.v->GRot (), Vector (pos.x, pos.y, pos.z));
	const Vector off = tmul (pl->GRot (), g - pl->GPos ());
	EmitterDesc d;
	d.body = pl;
	d.offset = V3 (off.x, off.y, off.z);
	d.refDist = (float)pv.contactRef;
	d.level = (float)Config (pv.v).tune.contactLevel;
	d.name = std::string (what) + " of " + pv.v->Name ();
	const EmitterId id = world.Add (d, std::move (cv));
	Emitter *em = world.Get (id);
	if (!em) return nullptr;
	// (heard out: its length, then the farthest heard's travel time)
	detached.push_back ({id, td.SimT1 + dur + RANGE / 300.0});
	return em;
}

bool VesselSounds::PlayEnergy (Emitter *em, Emitter *em2, const char *layer, double E, double r0, float rate, float cutoff, double tAt, int kind)
{
	if ((!em && !em2) || E <= 0.0) return false;
	SampleBank::Take k = bank.Pick (layer);
	if (!k || k.energy <= 0.0) return false;
	// (as the blows: p^2 integrated at r0, E rho c / 4 pi r0^2)
	const double p2 = E * RHO0 * C0 / (4.0 * 3.14159265358979 * r0 * r0);
	const float g = (float)(std::sqrt (p2 / k.energy) / P_FULL) * k.gain;
	const OneShot s = ContactVoices::Shot (kind, k.clip.get (), g, k.rate * rate, cutoff, 0.0f);
	bool ok = false;
	if (em) ok = em->Schedule (tAt, s) || ok;
	if (em2) ok = em2->Schedule (tAt, s) || ok;
	return ok;
}

void VesselSounds::PartBroke (const Vessel *v, const V3 &pos, double mass, double hit)
{
	PerVessel *pv = Find (v);
	if (!pv) return;
	pv->contacts.Break (now, pos, mass, hit);
	LogContacts (v, pv->contacts);
	if (!Dbg ().contactVoices || !ContactBank ()) return;
	// a part torn off: its tear, the debris' clatter after it - outside from
	// where it broke (outliving the vessel: it may be breaking up); in its
	// cabin through its structure, the tear as loud
	const BreakSound b = BreakFor (mass, hit, Rand01 ());
	const double t = td.SimT1;
	Emitter *out = Detach (*pv, pos, 3.0, "a part torn off");
	Emitter *st = world.Get (pv->contactStruct);
	PlayEnergy (out, st, "tear", b.tearE, pv->contactRef, (float)b.rate, TEAR_FC, t, ContactVoices::TEAR);
	PlayEnergy (out, nullptr, "clatter", b.clatterE, pv->contactRef, 1.0f, CLATTER_FC, t + b.clatterDelay, ContactVoices::CLATTER);
	if (Dbg ().log > 0)
		printf ("Audio: contact of %s: tear at %.3f s: %.3g J of sound (rate %.2f), clatter %.3f s on\n", v->Name (), t, b.tearE, b.rate, b.clatterDelay), fflush (stdout);
}

void VesselSounds::Crash (const Vessel *v, double speed, double mass, double energy)
{
	if (Dbg ().log > 0) {
		ContactEvent e;
		e.type = ContactEvent::CRASH;
		e.t = now, e.speed = speed, e.mass = mass, e.energy = energy;
		printf ("Audio: contact of %s: %s\n", v->Name (), Describe (e).c_str ()), fflush (stdout);
	}
	PerVessel *pv = Find (v);
	if (!pv || !Dbg ().contactVoices || !ContactBank ()) return;
	// the crash: its take, tears staggered, the debris - from where it
	// struck, on the planet (the vessel's own sounds go with it now)
	Emitter *em = Detach (*pv, V3 (), 6.0, "crash");
	if (!em) return;
	const double t = td.SimT1;
	static const char *layer[3] = {"crash", "tear", "clatter"};
	static const int kind[3] = {ContactVoices::CRASH, ContactVoices::TEAR, ContactVoices::CLATTER};
	static const float fc[3] = {CRASH_FC, TEAR_FC, CLATTER_FC};
	int n = 0;
	for (const CrashPart &c : CrashFor (energy, mass, Rand01 (), Rand01 ()))
		n += PlayEnergy (em, nullptr, layer[c.layer], c.energy, pv->contactRef, (float)c.rate, fc[c.layer], t + c.t, kind[c.layer]) ? 1 : 0;
	if (Dbg ().log > 0)
		printf ("Audio: contact of %s: crash heard: %d one-shots, %.3g J of sound\n", v->Name (), n, ETA_CRASH * energy), fflush (stdout);
}

void VesselSounds::UpdateContacts (PerVessel &pv, double t)
{
	ContactStep &c = pv.step;
	Emitter *contact = world.Get (pv.contact);
	ContactSynth *contactSynth = world.Synth<ContactSynth> (pv.contact);
	if (!contact || !contactSynth) return;
	const double m = std::max (pv.v->Mass (), 100.0);
	const double g = 9.81; // (normalizing only)
	const double mg = m * g;
	const SurfParam *sp = pv.v->GetSurfParam ();
	const double gs = sp ? sp->groundspd : 0.0;
	// blows (once per kind within BLOW_GAP)
	const double vb[3] = {c.blowWheel, c.blowHull, c.blowVessel};
	const int kinds[3] = {ContactSynth::TOUCHDOWN, ContactSynth::HULL, ContactSynth::CLANG};
	for (int k = 0; k < 3; k++) {
		if (vb[k] < BLOW_MIN[k] || t - pv.lastBlow[k] < BLOW_GAP) continue;
		pv.lastBlow[k] = t;
		pv.blows[kinds[k]]++;
		pv.blowStrength[kinds[k]] = (float)std::min (1.5, vb[k] / BLOW_FULL[k]);
		const bool log = Dbg ().log > 0;
		static const char *names[3] = {"touchdown", "hull", "vessel"};
		if (log) printf ("Audio: %s blow of %s at %.3f s (%.2f m/s)\n", names[k], pv.v->Name (), t, vb[k]), fflush (stdout);
	}
	if (c.any) contact->SetOffset (c.where);
	Floats<22> p;   // (rolling, its load, squeal, scrape, its speed; the blows counted, their strengths; the motors)
	p[0] = c.wheelLoad > 0.0 ? (float)gs : 0.0f;                 // rolling speed
	p[1] = (float)(c.wheelLoad / mg);                             // the load on the wheels
	p[2] = (float)std::min (1.0, std::sqrt (c.squealPower / (4.0 * mg)));
	p[3] = (float)std::min (1.0, std::sqrt (c.scrapePower / (5.0 * mg)));
	p[4] = (float)c.scrapeSpeed;
	for (int k = 0; k < ContactSynth::NBLOW; k++) p[5 + k] = (float)pv.blows[k], p[12 + k] = pv.blowStrength[k];
	for (int k = 0; k < ContactSynth::NMOTOR; k++) p[19 + k] = pv.motor[k];
	contact->Push (t, p);
	c = ContactStep ();

	// as heard: the sound now arriving
	const Floats<22> h = contact->Recalled<Floats<22>> ();
	ContactSynth::Params cp;
	cp.rollSpeed = h[0], cp.rollLoad = h[1], cp.squeal = h[2], cp.scrape = h[3], cp.scrapeSpeed = h[4];
	for (int k = 0; k < ContactSynth::NMOTOR; k++) cp.motor[k] = h[19 + k];
	contactSynth->Set (cp);
	for (int k = 0; k < ContactSynth::NBLOW; k++) {
		int n = (int)h[5 + k];
		if (n > pv.heardBlows[k]) contactSynth->Strike ((ContactSynth::Blow)k, h[12 + k]);
		pv.heardBlows[k] = std::max (pv.heardBlows[k], n);
	}
}

void VesselSounds::Update (double t, bool replay)
{
	if (!g_psys || !g_camera || !engine.Active ()) return;
	dtNow = t > now ? t - now : 0.0;
	now = t;
	replaying = replay;
	for (auto &pv : vessels) pv->seen = false;
	const Vector &cam = g_camera->GPos ();
	for (size_t i = 0; i < g_psys->nVessel (); i++) {
		Vessel *v = g_psys->GetVessel (i);
		if (!v || v->Destroyed ()) continue;
		if ((v->GPos () - cam).length () > RANGE + v->Size ()) continue;
		const BodyRef ref = world.Ref (v);
		PerVessel *pv = nullptr;
		for (auto &p : vessels) if (p->ref == ref) { pv = p.get (); break; }
		if (!pv) {
			vessels.push_back (std::make_unique<PerVessel>());
			pv = vessels.back ().get ();
			pv->v = v, pv->ref = ref;
			Build (*pv);
		}
		pv->seen = true;
		UpdateVessel (*pv);
	}
	// sounds outliving their vessels, heard out: gone
	for (auto it = detached.begin (); it != detached.end (); ) {
		if (now > it->until || !world.Get (it->em)) { world.Remove (it->em); it = detached.erase (it); }
		else ++it;
	}
	// out of range (or destroyed, or deleted): their sounds go
	for (auto it = vessels.begin (); it != vessels.end (); ) {
		if (!(*it)->seen) { Drop (**it); it = vessels.erase (it); }
		else ++it;
	}
	// (the configurations of vessels deleted: gone too)
	for (auto it = configs.begin (); it != configs.end (); )
		it = world.Alive (it->second.ref) ? std::next (it) : configs.erase (it);
}

} // namespace audio
