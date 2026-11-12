// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "VesselSounds.h"
#include "Ears.h"
#include "RocketSynth.h"
#include "RcsSynth.h"
#include "WindSynth.h"
#include "ContactSynth.h"
#include "Vessel.h"
#include "Psys.h"
#include "Camera.h"
#include "VesselAPI.h"
#include "TimeData.h"
#include "Planet.h"
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
	/// on the hull (in the cockpit) WIND_WALL q, through it WIND_TRANS of that
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
		for (auto &c : *cl) if (c.em) world.Remove (c.em);
	pv.engines.clear (), pv.rcs.clear ();
	if (pv.wind) world.Remove (pv.wind);
	pv.wind = nullptr, pv.windSynth = nullptr;
	if (pv.contact) world.Remove (pv.contact);
	pv.contact = nullptr, pv.contactSynth = nullptr;
}

void VesselSounds::Clear ()
{
	for (auto &pv : vessels) Drop (*pv);
	vessels.clear ();
}

void VesselSounds::Forget (const Body *body)
{
	for (auto it = configs.begin (); it != configs.end (); )
		it = (const Body*)it->first == body ? configs.erase (it) : std::next (it);
	// (the world forgets the emitters itself)
	vessels.erase (std::remove_if (vessels.begin (), vessels.end (), [body](std::unique_ptr<PerVessel> &pv) {
		return pv->v == body;
	}), vessels.end ());
}

void VesselSounds::Build (PerVessel &pv)
{
	Drop (pv);
	pv.rebuild = false;
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
		c.rocket = synth.get ();
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
		c.rcs = synth.get ();
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
		d.level = (float)(tune.rcsLevel * p0 / P_FULL / RCS_RMS);
		d.hull = (float)HULL_RCS;
		d.name = std::string (pv.v->Name ()) + " rcs " + std::to_string (pv.rcs.size ());
		c.rcs->Set (0.0f, (float)(tune.rcsPitch * RocketSynth::PeakFor (Fmax)));
		c.em = world.Add (d, std::move (synth));
		if (c.em) pv.rcs.push_back (c);
	}

	// its contacts
	auto cs = ContactSynth::Make (engine, seed++);
	if (cs) {
		pv.contactSynth = cs.get ();
		EmitterDesc d;
		d.body = pv.v;
		d.refDist = (float)std::max (3.0, 0.5 * size);
		double m = std::max (pv.v->Mass (), 100.0);
		d.level = (float)(tune.contactLevel * CONTACT_P * std::clamp (std::sqrt (m / 15000.0), 0.3, 5.0) / P_FULL / CONTACT_RMS);
		d.hull = (float)CONTACT_HULL;
		d.name = std::string (pv.v->Name ()) + " contacts";
		pv.contact = world.Add (d, std::move (cs));
		if (!pv.contact) pv.contactSynth = nullptr;
	}

	// the air over it
	auto ws = WindSynth::Make (engine, seed++);
	if (ws) {
		pv.windSynth = ws.get ();
		EmitterDesc d;
		d.body = pv.v;
		d.refDist = (float)std::max (3.0, 0.5 * size);
		d.level = 0.0f;
		d.boom = true;
		d.length = (float)(2.0 * size);
		d.name = std::string (pv.v->Name ()) + " air";
		pv.wind = world.Add (d, std::move (ws));
		if (!pv.wind) pv.windSynth = nullptr;
	}
}

void VesselSounds::UpdateVessel (PerVessel &pv)
{
	const double t = now;
	VESSEL *vi = pv.v->GetModuleInterface ();
	if (!vi) return;
	if (vi->GetThrusterCount () != pv.nth || pv.rebuild) Build (pv);
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
		// (as heard: when the sound now arriving left it)
		c.em->Push (t, {(float)(lv / c.maxth), 0.0f, 0.0f, 0.0f});
		RocketSynth::Params rp;
		rp.level = c.em->Recalled ()[0];
		rp.peak = (float)(tune.enginePitch * RocketSynth::PeakFor (c.maxth));
		rp.crackle = (float)(0.3 * tune.crackle);
		rp.rumble = (float)(tune.rumble * RocketSynth::RumbleFor (c.maxth / c.th.size ()));
		rp.whine = (float)(0.15 * tune.whine);
		rp.pump = (float)(tune.enginePitch * RocketSynth::PumpFor (c.maxth / c.th.size ()));
		c.rocket->Set (rp);
	}
	for (auto &c : pv.rcs) {
		double lv = 0.0, Fmax = 0.0;
		for (int i : c.th) {
			THRUSTER_HANDLE th = vi->GetThrusterHandleByIndex (i);
			double F = vi->GetThrusterMax0 (th);
			lv = std::max (lv, vi->GetThrusterLevel (th));
			Fmax = std::max (Fmax, F);
		}
		c.em->Push (t, {(float)lv, 0.0f, 0.0f, 0.0f});
		c.rcs->Set (c.em->Recalled ()[0], (float)(tune.rcsPitch * RocketSynth::PeakFor (Fmax)));
	}
	if (pv.wind) {
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
		pv.wind->Push (t, {(float)U, WindSynth::BuffetFor (vi->GetAOA (), M), (float)q, 0.0f});
		Emitter::Params h = pv.wind->Recalled ();
		WindSynth::Params wp;
		wp.speed = h[0];
		wp.buffet = h[1];
		pv.windSynth->Set (wp);
		double qh = h[2], Uh = h[0];
		double p0 = WIND_RAD * qh * (Uh / 300.0);
		pv.wind->SetLevel ((float)(tune.airLevel * p0 / P_FULL / WIND_RMS));
		double cabin = WIND_WALL * qh * WIND_TRANS;
		pv.wind->SetHull (p0 > 1e-9 ? (float)(cabin / p0) : 0.0f);
	}
	if (pv.contact) UpdateContacts (pv, t);
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
	auto it = configs.find (v);
	if (it != configs.end ()) return it->second;
	VesselConfig &c = configs[v];
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
	if (!Config (v).tune.Set (key, value)) return false;
	if (PerVessel *pv = Find (v)) pv->rebuild = true;
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
	auto it = configs.find (v);
	if (it == configs.end ()) return -1;
	for (auto &a : it->second.anims) if (a.kind == ANIM_GEAR) return (int)a.anim;
	return -1;
}

VesselSounds::PerVessel *VesselSounds::Find (const Vessel *v)
{
	for (auto &p : vessels) if (p->v == v) return p.get ();
	return nullptr;
}

void VesselSounds::Contacts (const Vessel *v, const ContactStep &c)
{
	PerVessel *pv = Find (v);
	if (!pv || !c.any) return;
	ContactStep &s = pv->step;
	s.wheelLoad += c.wheelLoad;
	s.squealPower += c.squealPower;
	s.scrapePower += c.scrapePower;
	s.scrapeSpeed = std::max (s.scrapeSpeed, c.scrapeSpeed);
	s.blowWheel = std::max (s.blowWheel, c.blowWheel);
	s.blowHull = std::max (s.blowHull, c.blowHull);
	s.blowVessel = std::max (s.blowVessel, c.blowVessel);
	s.where = c.where;
	s.any = true;
}

void VesselSounds::Docked (const Vessel *v, const V3 &port, bool dock)
{
	PerVessel *pv = Find (v);
	if (!pv || !pv->contact) return;
	int k = dock ? ContactSynth::DOCK : ContactSynth::UNDOCK;
	pv->blows[k]++, pv->blowStrength[k] = 1.0f;
	static const bool log = getenv ("ORBITER_AUDIO_LOG") != nullptr;
	if (log) printf ("Audio: %s of %s\n", dock ? "docking" : "undocking", v->Name ()), fflush (stdout);
	pv->contact->SetOffset (port);
}

void VesselSounds::UpdateContacts (PerVessel &pv, double t)
{
	ContactStep &c = pv.step;
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
		static const bool log = getenv ("ORBITER_AUDIO_LOG") != nullptr;
		static const char *names[3] = {"touchdown", "hull", "vessel"};
		if (log) printf ("Audio: %s blow of %s at %.3f s (%.2f m/s)\n", names[k], pv.v->Name (), t, vb[k]), fflush (stdout);
	}
	if (c.any) pv.contact->SetOffset (c.where);
	Emitter::Params p{};
	p[0] = c.wheelLoad > 0.0 ? (float)gs : 0.0f;                 // rolling speed
	p[1] = (float)(c.wheelLoad / mg);                             // the load on the wheels
	p[2] = (float)std::min (1.0, std::sqrt (c.squealPower / (4.0 * mg)));
	p[3] = (float)std::min (1.0, std::sqrt (c.scrapePower / (5.0 * mg)));
	p[4] = (float)c.scrapeSpeed;
	for (int k = 0; k < ContactSynth::NBLOW; k++) p[5 + k] = (float)pv.blows[k], p[12 + k] = pv.blowStrength[k];
	for (int k = 0; k < ContactSynth::NMOTOR; k++) p[19 + k] = pv.motor[k];
	pv.contact->Push (t, p);
	c = ContactStep ();

	// as heard: the sound now arriving
	Emitter::Params h = pv.contact->Recalled ();
	ContactSynth::Params cp;
	cp.rollSpeed = h[0], cp.rollLoad = h[1], cp.squeal = h[2], cp.scrape = h[3], cp.scrapeSpeed = h[4];
	for (int k = 0; k < ContactSynth::NMOTOR; k++) cp.motor[k] = h[19 + k];
	pv.contactSynth->Set (cp);
	for (int k = 0; k < ContactSynth::NBLOW; k++) {
		int n = (int)h[5 + k];
		if (n > pv.heardBlows[k]) pv.contactSynth->Strike ((ContactSynth::Blow)k, h[12 + k]);
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
		PerVessel *pv = nullptr;
		for (auto &p : vessels) if (p->v == v) { pv = p.get (); break; }
		if (!pv) {
			vessels.push_back (std::make_unique<PerVessel>());
			pv = vessels.back ().get ();
			pv->v = v;
			Build (*pv);
		}
		pv->seen = true;
		UpdateVessel (*pv);
	}
	// out of range (or destroyed): their sounds go
	for (auto it = vessels.begin (); it != vessels.end (); ) {
		if (!(*it)->seen) { Drop (**it); it = vessels.erase (it); }
		else ++it;
	}
}

} // namespace audio
