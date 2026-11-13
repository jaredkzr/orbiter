// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Music.h"
#include "AudioSystem.h"
#include "Vessel.h"
#include "Psys.h"
#include "Log.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

extern Vessel *g_focusobj;
extern PlanetarySystem *g_psys;

namespace audio {

using namespace music;

Music::Music (System &s): sys (s)
{
}

Music::~Music ()
{
	Clear ();
}

void Music::Clear ()
{
	synth.reset ();
	mood = candidate = SILENT;
	since = alarm = off = 0.0;
	focus = nullptr;
}

void Music::Alarm ()
{
	alarm = ALARM;
}

Mood Music::MoodFor (const Situation &s)
{
	if (s.menu) return MENU;
	if (!s.vessel) return ORBIT;
	if (s.destroyed) return SILENT;
	if (s.ascent) return ASCENT;
	if (s.reentry) return REENTRY;
	if (s.landed || s.air) return GROUND;
	if (!s.docked && s.target >= 0.0 && s.target < APPROACH_DIST) return APPROACH;
	if (s.deep) return DEEP;
	return ORBIT;
}

Music::Situation Music::Observe (bool menu) const
{
	Situation s;
	s.menu = menu;
	if (menu || !g_psys || !g_focusobj) return s;
	s.vessel = true;
	if (g_focusobj->Destroyed ()) { s.destroyed = true; return s; }
	VESSEL *v = const_cast<Vessel*> (g_focusobj)->GetModuleInterface ();
	if (!v) return s;
	Callouts &c = sys.GetCallouts ();
	const double p = v->GetAtmPressure ();
	s.landed = !c.Flying ();
	s.air = p > 10.0;
	VECTOR3 as;
	v->GetAirspeedVector (FRAME_HORIZON, as);
	s.ascent = c.Ascending () && !s.landed && v->GetThrusterGroupLevel (THGROUP_MAIN) > 0.05 && as.y > 2.0;
	s.reentry = sys.GetRadio ().Blackout () || (!s.landed && !s.ascent && p > 1.0 && v->GetAirspeed () > REENTRY_V && as.y < 0.0);
	for (UINT i = 0; i < v->DockCount () && !s.docked; i++) s.docked = v->DockingStatus (i) != 0;

	// the docking target; another vessel closing in (or very near): not
	// one attached to this, nor a stage drifting away
	s.target = c.DockDistance ();
	VECTOR3 gp, gv;
	v->GetGlobalPos (gp), v->GetGlobalVel (gv);
	const OBJHANDLE self = v->GetHandle ();
	auto attached = [v] (OBJHANDLE h) {
		for (int tp = 0; tp < 2; tp++)
			for (DWORD i = 0; i < v->AttachmentCount (tp != 0); i++)
				if (v->GetAttachmentStatus (v->GetAttachmentHandle (tp != 0, i)) == h) return true;
		return false;
	};
	for (DWORD i = 0; i < oapiGetVesselCount (); i++) {
		OBJHANDLE h = oapiGetVesselByIndex (i);
		if (!h || h == self) continue;
		VECTOR3 op, ov;
		oapiGetGlobalPos (h, &op), oapiGetGlobalVel (h, &ov);
		const VECTOR3 d = op - gp;
		const double r = length (d);
		if (r >= APPROACH_DIST || (s.target >= 0.0 && r >= s.target)) continue;
		const double rate = dotp (ov - gv, d) / std::max (r, 1e-3); // (< 0: closing)
		if ((rate < -0.05 || r < 200.0) && !attached (h)) s.target = r;
	}

	// far from any body
	if (OBJHANDLE ref = v->GetGravityRef ()) {
		VECTOR3 rp;
		oapiGetGlobalPos (ref, &rp);
		const double r = length (gp - rp);
		s.deep = oapiGetObjectType (ref) == OBJTP_STAR || r > DEEP_RADII * oapiGetSize (ref);
		if (s.ascent) {
			// (the way to orbit: half the height of the sensible air, half
			// the speed of a low orbit)
			VECTOR3 rv;
			v->GetRelativeVel (ref, rv);
			const double vc = std::sqrt (GGRAV * oapiGetMass (ref) / std::max (r, 1.0));
			s.progress = 0.5 * std::min (1.0, v->GetAltitude () / 120e3) + 0.5 * std::min (1.0, length (rv) / vc);
		}
	}
	if (!s.ascent && s.target >= 0.0) s.progress = std::clamp (1.0 - s.target / APPROACH_DIST, 0.0, 1.0);
	return s;
}

void Music::Update (double dt, bool menu)
{
	Engine &e = sys.GetEngine ();
	if (!e.Active ()) return;
	static const bool log = getenv ("ORBITER_AUDIO_LOG") != nullptr;
	if (!sys.MusicOn ()) {
		// (faded out, then gone)
		if (synth) {
			synth->SetMood (SILENT);
			if ((off += dt) > 5.0) synth.reset ();
		}
		mood = candidate = SILENT, since = 0.0;
		return;
	}
	off = 0.0;
	if (!synth) {
		// (a new tune each time; a render's the same each time)
		const uint32_t seed = sys.Rendering () ? 1u : (uint32_t)std::chrono::steady_clock::now ().time_since_epoch ().count ();
		synth = MusicSynth::Make (e, seed);
		if (!synth || !synth->Attach (e.Group (MUSIC))) { synth.reset (); return; }
		synth->Start ();
	}
	if (g_focusobj != focus) focus = g_focusobj, alarm = 0.0;
	alarm = std::max (0.0, alarm - dt);
	const Situation s = Observe (menu);
	const Mood m = alarm > 0.0 ? SILENT : MoodFor (s);
	if (m != candidate) candidate = m, since = 0.0;
	else since += dt;
	// (a new situation holds a moment first; silence and the menu at once)
	if (candidate != mood && (since >= HOLD || candidate == SILENT || candidate == MENU)) {
		mood = candidate;
		if (log) printf ("Audio: music: %s\n", Name (mood)), fflush (stdout);
		LOGOUT_FINE ("Audio: music: %s", Name (mood));
	}
	synth->SetMood (mood);
	synth->SetIntensity ((float)s.progress);
}

} // namespace audio
