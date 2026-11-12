// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "XRSoundShim.h"
#include "XRSound.h"
#include "AudioSystem.h"
#include "XrConfig.h"
#include "Orbiter.h"
#include "Vessel.h"
#include "Psys.h"
#include "Log.h"
#include "TimeData.h"
#include <algorithm>
#include <cctype>
#include <memory>

extern Orbiter *g_pOrbiter;
extern PlanetarySystem *g_psys;
extern TimeData td;

namespace audio {

namespace {
	using PT = XRSound::PlaybackType;

	/// What a default sound is in Orbiter's sound
	enum Kind { LINE, GROUP, ENGINE_MAIN, ENGINE_RETRO, ENGINE_HOVER, RCS, AIR, CONTACT, OTHER };
	struct DefaultSound { int id; const char *key; Kind kind; };
	const DefaultSound defaultSounds[] = {
		{XRSound::AirConditioning, "AirConditioning", OTHER},
		{XRSound::LandedWind, "LandedWind", OTHER},
		{XRSound::AudioGreeting, "AudioGreeting", LINE},
		{XRSound::MainEngines, "MainEngines", ENGINE_MAIN},
		{XRSound::RetroEngines, "RetroEngines", ENGINE_RETRO},
		{XRSound::HoverEngines, "HoverEngines", ENGINE_HOVER},
		{XRSound::RCSSustain, "RCSSustain", RCS},
		{XRSound::SwitchOn, "SwitchOn", LINE},
		{XRSound::SwitchOff, "SwitchOff", LINE},
		{XRSound::Rotation, "RCSRotation", LINE},
		{XRSound::Translation, "RCSTranslation", LINE},
		{XRSound::Off, "RCSOff", LINE},
		{XRSound::CustomEngines, "CustomEngines", OTHER},
		{XRSound::AFPitch, "AFPitch", LINE},
		{XRSound::AFOn, "AFOn", LINE},
		{XRSound::AFOff, "AFOff", LINE},
		{XRSound::Crash, "Crash", OTHER},
		{XRSound::MetalCrunch, "MetalCrunch", CONTACT},
		{XRSound::WheelChirp, "WheelChirp", CONTACT},
		{XRSound::Touchdown, "Touchdown", LINE},
		{XRSound::WheelStop, "WheelStop", LINE},
		{XRSound::TiresRolling, "TiresRolling", CONTACT},
		{XRSound::OneHundredKnots, "OneHundredKnots", LINE},
		{XRSound::Liftoff, "Liftoff", LINE},
		{XRSound::WarningGearIsUp, "WarningGearIsUp", LINE},
		{XRSound::YouAreClearedToLand, "YouAreClearedToLand", LINE},
		{XRSound::Docking, "Docking", CONTACT},
		{XRSound::DockingCallout, "DockingCallout", LINE},
		{XRSound::Undocking, "Undocking", CONTACT},
		{XRSound::UndockingCallout, "UndockingCallout", LINE},
		{XRSound::Wheekbrakes, "Wheelbrakes", CONTACT},
		{XRSound::DockingRadarBeep, "DockingRadarBeep", LINE},
		{XRSound::FlightWind, "FlightWind", AIR},
		{XRSound::ReentryPlasma, "ReentryPlasma", OTHER},
		{XRSound::SonicBoom, "SonicBoom", OTHER},
		{XRSound::AutopilotOn, "AutopilotOn", LINE},
		{XRSound::AutopilotOff, "AutopilotOff", LINE},
		{XRSound::SubsonicCallout, "SubsonicCallout", LINE},
		{XRSound::RCSAttackPlusX, "RCSAttackPlusX", RCS},
		{XRSound::RCSAttackPlusY, "RCSAttackPlusY", RCS},
		{XRSound::RCSAttackPlusZ, "RCSAttackPlusZ", RCS},
		{XRSound::RCSAttackMinusX, "RCSAttackMinusX", RCS},
		{XRSound::RCSAttackMinusY, "RCSAttackMinusY", RCS},
		{XRSound::RCSAttackMinusZ, "RCSAttackMinusZ", RCS},
		{XRSound::RCSClosePlusX, "RCSClosePlusX", RCS},
		{XRSound::RCSClosePlusY, "RCSClosePlusY", RCS},
		{XRSound::RCSClosePlusZ, "RCSClosePlusZ", RCS},
		{XRSound::RCSCloseMinusX, "RCSCloseMinusX", RCS},
		{XRSound::RCSCloseMinusY, "RCSCloseMinusY", RCS},
		{XRSound::RCSCloseMinusZ, "RCSCloseMinusZ", RCS},
		{XRSound::RadioATCGroup, "RadioATCGroup", GROUP},
		{XRSound::CabinAmbienceGroup, "CabinAmbienceGroup", GROUP},
		{XRSound::MachCalloutsGroup, "MachCalloutsGroup", GROUP},
		{XRSound::AltitudeCalloutsGroup, "AltitudeCalloutsGroup", GROUP},
		{XRSound::DockingDistanceCalloutsGroup, "DockingDistanceCalloutsGroup", GROUP},
		{XRSound::MusicFolder, "MusicFolder", GROUP},
	};

	const DefaultSound *Default (int id)
	{
		for (auto &d : defaultSounds) if (d.id == id) return &d;
		return nullptr;
	}

	bool SameKey (const std::string &a, const char *b)
	{
		size_t n = strlen (b);
		return a.size () == n && std::equal (a.begin (), a.end (), b,
			[](char x, char y) { return std::tolower ((unsigned char)x) == std::tolower ((unsigned char)y); });
	}

	System *Sys () { return g_pOrbiter ? g_pOrbiter->Audio () : nullptr; }

	/// Where a playback type's sound goes, and its level there
	struct Route { bool onVessel; Bus bus; double level; };
	Route RouteFor (PT t, bool module)
	{
		if (module) return {false, UI, 1.0};
		switch (t) {
		case PT::BothViewFar:    return {true, EXTERIOR, 1.0};
		case PT::BothViewMedium: return {true, EXTERIOR, 0.5};
		case PT::BothViewClose:  return {true, EXTERIOR, 0.25};
		case PT::InternalOnly:   return {false, INTERIOR, 1.0};
		case PT::Radio:          return {false, VOICE, 1.0};
		case PT::Wind:           return {false, EXTERIOR, 1.0};
		default:                 return {false, UI, 1.0};
		}
	}

	PT TypeNamed (const std::string &s, PT def)
	{
		static const std::pair<const char*, PT> names[] = {
			{"InternalOnly", PT::InternalOnly}, {"BothViewFar", PT::BothViewFar}, {"BothViewMedium", PT::BothViewMedium},
			{"BothViewClose", PT::BothViewClose}, {"Radio", PT::Radio}, {"Wind", PT::Wind}, {"Global", PT::Global}};
		for (auto &n : names) if (SameKey (s, n.first)) return n.second;
		return def;
	}
}

// =======================================================================
// An instance (a vessel's, a module's)

class XrInstance : public XRSound {
public:
	explicit XrInstance (const Vessel *v): vessel (v) { Instances ().push_back (this); }
	explicit XrInstance (const std::string &m): module (m) { Instances ().push_back (this); }
	~XrInstance () override
	{
		auto &all = Instances ();
		all.erase (std::remove (all.begin (), all.end (), this), all.end ());
		if (System *s = Sys ()) for (auto &sl : slots) if (sl.second.h) s->StopSound (sl.second.h);
	}

	static std::vector<XrInstance*> &Instances () { static std::vector<XrInstance*> v; return v; }
	static XrInstance *For (const Vessel *v)
	{
		for (auto *i : Instances ()) if (i->vessel == v && v) return i;
		return nullptr;
	}

	bool IsPresent () const override { return true; }
	float GetVersion () const override { return 3.0f; }

	bool LoadWav (const int id, const char *file, const PlaybackType type) override
	{
		if (!file || !*file || System::FindSound (file).empty ()) return false;
		Slot &s = slots[id];
		if (s.h) if (System *sys = Sys ()) sys->StopSound (s.h);
		s = Slot ();
		s.file = file, s.type = type;
		return true;
	}

	bool PlayWav (const int id, const bool loop, const float volume) override
	{
		System *sys = Sys ();
		if (!sys) return false;
		auto it = slots.find (id);
		if (it == slots.end ()) {
			// (a default sound not loaded: the cfgs' file)
			const DefaultSound *d = Default (id);
			if (!d || !Enabled (id)) return false;
			const XrConfig &c = XrConfig::For (ClassName ());
			std::string f = c.File (d->key);
			if (f.empty () || !LoadWav (id, f.c_str (), d->kind == LINE ? PT::Radio : PT::BothViewClose)) return false;
			it = slots.find (id);
		}
		if (Default (id) && !Enabled (id)) return false;
		Slot &s = it->second;
		s.volume = std::clamp ((double)volume, 0.0, 1.0);
		if (s.h && sys->Playing (s.h)) {
			// (playing: its volume changes; it goes on)
			sys->SetSound (s.h, s.volume * RouteFor (s.type, !vessel).level, -1.0);
			s.loop = loop;
			return true;
		}
		Route r = RouteFor (s.type, !vessel);
		s.loop = loop;
		s.h = r.onVessel ? sys->Play (vessel, s.file, V3 (), s.volume * r.level, loop)
		                 : sys->PlayOn (r.bus, s.file, s.volume * r.level, loop);
		if (!s.h) return false;
		Gate (*sys, s, Sys () ? lastFocus : nullptr, lastCockpit);
		return true;
	}

	bool StopWav (const int id) override
	{
		auto it = slots.find (id);
		if (it == slots.end ()) return false;
		if (it->second.h) if (System *sys = Sys ()) sys->StopSound (it->second.h);
		it->second.h = 0;
		return true;
	}

	bool IsWavPlaying (const int id) const override
	{
		const Slot *s = Find (id);
		System *sys = Sys ();
		return s && s->h && sys && sys->Playing (s->h) && !sys->Paused (s->h);
	}

	bool SetPaused (const int id, const bool pause) override
	{
		Slot *s = Find (id);
		System *sys = Sys ();
		return s && s->h && sys && sys->Pause (s->h, pause);
	}

	bool IsPaused (const int id) const override
	{
		const Slot *s = Find (id);
		System *sys = Sys ();
		return s && s->h && sys && sys->Paused (s->h);
	}

	bool SetDefaultSoundEnabled (const DefaultSoundID id, const bool on) override
	{
		const DefaultSound *d = Default (id);
		if (!d || !vessel) return false;
		enabled[id] = on;
		if (System *sys = Sys ()) {
			// (a synthesizer: on or off for the vessel)
			const char *tune = d->kind == ENGINE_MAIN ? "engine_level" : d->kind == RCS ? "rcs_level"
				: d->kind == AIR ? "air_level" : d->kind == CONTACT ? "contact_level" : nullptr;
			if (tune) sys->Tune (vessel, tune, on ? 1.0 : 0.0);
			if (!on) StopWav (id);
		}
		return true;
	}

	bool GetDefaultSoundEnabled (const DefaultSoundID id) const override
	{
		return Default (id) && Enabled (id);
	}

	bool SetDefaultSoundGroupFolder (const DefaultSoundID id, const char *folder) override
	{
		const DefaultSound *d = Default (id);
		if (!d || d->kind != GROUP || !folder || !*folder || !vessel) return false;
		folders[id] = folder;
		return true;
	}

	const char *GetDefaultSoundGroupFolder (const DefaultSoundID id) const override
	{
		const DefaultSound *d = Default (id);
		if (!d || d->kind != GROUP) return nullptr;
		auto it = folders.find (id);
		if (it != folders.end ()) return it->second.c_str ();
		static std::string g;
		g = XrConfig::For (ClassName ()).Group (d->key);
		return g.empty () ? nullptr : g.c_str ();
	}

	bool SetPan (const int id, const float pan) override
	{
		Slot *s = Find (id);
		System *sys = Sys ();
		return s && s->h && sys && sys->SetPan (s->h, pan);
	}

	float GetPan (const int id) override
	{
		Slot *s = Find (id);
		System *sys = Sys ();
		return s && s->h && sys && sys->Playing (s->h) ? sys->GetPan (s->h) : -100.0f;
	}

	bool SetPlaybackSpeed (const int id, const float speed) override
	{
		Slot *s = Find (id);
		System *sys = Sys ();
		return s && s->h && sys && speed > 0.0f && sys->SetSound (s->h, -1.0, speed);
	}

	float GetPlaybackSpeed (const int id) override
	{
		Slot *s = Find (id);
		System *sys = Sys ();
		return s && s->h && sys ? (float)sys->GetPitch (s->h) : 0.0f;
	}

	bool SetPlayPosition (const int id, const unsigned int ms) override
	{
		Slot *s = Find (id);
		System *sys = Sys ();
		return s && s->h && sys && sys->Seek (s->h, ms * 1e-3);
	}

	int GetPlayPosition (const int id) override
	{
		Slot *s = Find (id);
		System *sys = Sys ();
		double c = s && s->h && sys ? sys->Cursor (s->h) : -1.0;
		return c < 0.0 ? -1 : (int)(c * 1e3 + 0.5);
	}

	// ---- for the core ----------------------------------------------------

	struct Slot { std::string file; PT type = PT::Radio; int h = 0; double volume = 1.0; bool loop = false; };

	/// Where its sounds may be heard (each step)
	void Step (System &sys, const Vessel *focus, bool cockpit)
	{
		lastFocus = focus, lastCockpit = cockpit;
		for (auto &sl : slots) {
			Slot &s = sl.second;
			if (!s.h) continue;
			if (!sys.Playing (s.h)) { s.h = 0; continue; }
			Gate (sys, s, focus, cockpit);
		}
	}

	bool Enabled (int id) const
	{
		auto it = enabled.find (id);
		return it == enabled.end () || it->second;
	}

	/// A file its vessel's code gave a default sound (LoadWav with its ID)
	const Slot *Find (int id) const
	{
		auto it = slots.find (id);
		return it == slots.end () ? nullptr : &it->second;
	}
	Slot *Find (int id)
	{
		auto it = slots.find (id);
		return it == slots.end () ? nullptr : &it->second;
	}
	const std::map<int, std::string> &Folders () const { return folders; }

	const Vessel *vessel = nullptr;
	std::string module;

private:
	std::string ClassName () const { return vessel && vessel->ClassName () ? vessel->ClassName () : ""; }

	void Gate (System &sys, Slot &s, const Vessel *focus, bool cockpit)
	{
		double g = 1.0;
		if (vessel) {
			switch (s.type) {
			case PT::InternalOnly: g = vessel == focus && cockpit ? 1.0 : 0.0; break;
			case PT::Radio:        g = vessel == focus ? 1.0 : 0.0; break;
			case PT::Wind: {
				Vessel *v = const_cast<Vessel*>(vessel);
				VESSEL *vi = v->GetModuleInterface ();
				g = !cockpit && vi ? std::min (1.0, vi->GetAtmPressure () / 101325.0) : 0.0;
				break; }
			default: break;
			}
		}
		sys.SetGate (s.h, g);
	}

	std::map<int, Slot> slots;
	std::map<int, bool> enabled;
	std::map<int, std::string> folders;
	const Vessel *lastFocus = nullptr;
	bool lastCockpit = false;
};

// =======================================================================
// What a vessel class's XRSound cfg gives every vessel of the class: its
// animations' sounds, its engines' own files

namespace {
	struct AnimTrack {
		XrConfig::Anim a;
		double prev = -1.0;
		int oneShot = 0, moving = 0;
	};
	struct ClassSounds {
		const Vessel *v = nullptr;
		bool init = false;
		std::vector<AnimTrack> anims;
		std::string engine[3];      // own files for main, retro, hover (empty: synthesized)
		int engineH[3] = {};
		bool synthOff = false;
	};
	std::vector<std::unique_ptr<ClassSounds>> &Classes ()
	{
		static std::vector<std::unique_ptr<ClassSounds>> c;
		return c;
	}

	ClassSounds &ClassFor (const Vessel *v)
	{
		auto &all = Classes ();
		for (auto &c : all) if (c->v == v) return *c;
		all.push_back (std::make_unique<ClassSounds>());
		all.back ()->v = v;
		return *all.back ();
	}

	void Play1 (System &sys, const Vessel *v, const std::string &file, PT type, bool loop, int &h)
	{
		if (file.empty ()) return;
		Route r = RouteFor (type, false);
		if (r.bus == VOICE && !sys.CalloutsOn ()) return; // (a voice's line: the callouts switched off)
		if (h) sys.StopSound (h);
		h = r.onVessel ? sys.Play (v, file, V3 (), r.level, loop) : sys.PlayOn (r.bus, file, r.level, loop);
		// (the voice's: noted with the callouts)
		if (h && r.bus == VOICE && !loop) sys.GetCallouts ().Note (file, td.SimT1);
	}

	void StepClass (System &sys, ClassSounds &cs, const Vessel *focus, bool cockpit)
	{
		Vessel *v = const_cast<Vessel*>(cs.v);
		VESSEL *vi = v->GetModuleInterface ();
		if (!vi) return;
		const std::string cls = v->ClassName () ? v->ClassName () : "";
		if (!cs.init) {
			cs.init = true;
			const XrConfig &c = XrConfig::For (cls), &global = XrConfig::For ("");
			for (auto &a : c.anims) { AnimTrack t; t.a = a; cs.anims.push_back (t); }
			// an engine's file of its own: the class cfg's, differing from the
			// global one (the default: synthesized)
			static const char *keys[3] = {"MainEngines", "RetroEngines", "HoverEngines"};
			for (int k = 0; k < 3; k++) {
				std::string f = c.File (keys[k]);
				if (!f.empty () && f != global.File (keys[k])) cs.engine[k] = f;
			}
		}
		// (a vessel's code giving an engine a file: LoadWav with its ID)
		if (XrInstance *in = XrInstance::For (cs.v)) {
			static const int ids[3] = {XRSound::MainEngines, XRSound::RetroEngines, XRSound::HoverEngines};
			for (int k = 0; k < 3; k++) if (const XrInstance::Slot *s = in->Find (ids[k])) cs.engine[k] = s->file;
		}

		// engines with their own files: looped, as loud as the throttle; the
		// synthesizer off
		static const THGROUP_TYPE groups[3] = {THGROUP_MAIN, THGROUP_RETRO, THGROUP_HOVER};
		bool own = false;
		for (int k = 0; k < 3; k++) {
			if (cs.engine[k].empty ()) continue;
			own = true;
			double lvl = vi->GetThrusterGroupLevel (groups[k]);
			if (lvl > 0.01) {
				if (!cs.engineH[k] || !sys.Playing (cs.engineH[k])) cs.engineH[k] = sys.Play (cs.v, cs.engine[k], V3 (), lvl, true);
				else sys.SetSound (cs.engineH[k], lvl, -1.0);
			} else if (cs.engineH[k]) {
				sys.StopSound (cs.engineH[k]);
				cs.engineH[k] = 0;
			}
		}
		if (own && !cs.synthOff) { sys.Tune (cs.v, "engine_level", 0.0); cs.synthOff = true; }

		// animations: opening (from closed), moving, open (at its end),
		// closing (from open), closed (at its start)
		for (AnimTrack &t : cs.anims) {
			if (t.a.index < 0) continue;
			double st = vi->GetAnimation ((UINT)t.a.index);
			if (st < 0.0) continue; // (no such animation)
			PT type = TypeNamed (t.a.playback, PT::BothViewClose);
			if (t.prev >= 0.0 && st != t.prev) {
				if (t.prev <= 0.0 && st > 0.0) Play1 (sys, cs.v, t.a.opening, type, false, t.oneShot);
				else if (t.prev >= 1.0 && st < 1.0) Play1 (sys, cs.v, t.a.closing, type, false, t.oneShot);
				if (st >= 1.0) Play1 (sys, cs.v, t.a.open, type, false, t.oneShot);
				else if (st <= 0.0) Play1 (sys, cs.v, t.a.closed, type, false, t.oneShot);
				if (st > 0.0 && st < 1.0 && !t.a.moving.empty () && (!t.moving || !sys.Playing (t.moving)))
					Play1 (sys, cs.v, t.a.moving, type, true, t.moving);
			} else if (t.moving && (st == t.prev || st <= 0.0 || st >= 1.0)) {
				sys.StopSound (t.moving);
				t.moving = 0;
			}
			if ((st <= 0.0 || st >= 1.0) && t.moving) { sys.StopSound (t.moving); t.moving = 0; }
			// (heard as their type says)
			for (int h : {t.oneShot, t.moving}) {
				if (!h) continue;
				double g = type == PT::InternalOnly ? (cs.v == focus && cockpit ? 1.0 : 0.0) : type == PT::Radio ? (cs.v == focus ? 1.0 : 0.0) : 1.0;
				sys.SetGate (h, g);
			}
			t.prev = st;
		}
	}
}

namespace xr {

void Step (System &sys, const Vessel *focus, bool cockpit, double simt)
{
	for (auto *in : XrInstance::Instances ()) in->Step (sys, focus, cockpit);
	if (!g_psys) return;
	// (the vessels whose class cfg gives them sounds, or whose code does)
	for (DWORD i = 0; i < g_psys->nVessel (); i++) {
		const Vessel *v = g_psys->GetVessel (i);
		if (!v || v->Destroyed ()) continue;
		const XrConfig &c = XrConfig::For (v->ClassName () ? v->ClassName () : "");
		bool any = !c.anims.empty () || XrInstance::For (v);
		if (!any) {
			static const char *keys[3] = {"MainEngines", "RetroEngines", "HoverEngines"};
			const XrConfig &g = XrConfig::For ("");
			for (auto *k : keys) if (!c.File (k).empty () && c.File (k) != g.File (k)) any = true;
		}
		if (any) StepClass (sys, ClassFor (v), focus, cockpit);
	}
}

void Forget (const Body *body)
{
	auto &all = Classes ();
	all.erase (std::remove_if (all.begin (), all.end (), [body](const std::unique_ptr<ClassSounds> &c) { return (const Body*)c->v == body; }), all.end ());
	for (auto *in : XrInstance::Instances ()) if ((const Body*)in->vessel == body) in->vessel = nullptr;
}

void Clear ()
{
	Classes ().clear ();
	XrConfig::Reload ();
}

bool LineFor (const Vessel *v, const std::string &key, std::string &file)
{
	XrInstance *in = XrInstance::For (v);
	if (!in) return false;
	for (auto &d : defaultSounds) {
		if (!SameKey (key, d.key)) continue;
		if (!in->Enabled (d.id)) { file.clear (); return true; }
		if (d.kind == GROUP) {
			auto it = in->Folders ().find (d.id);
			if (it != in->Folders ().end ()) { file = it->second; return true; }
		} else if (const XrInstance::Slot *s = in->Find (d.id)) {
			file = s->file;
			return true;
		}
		return false;
	}
	return false;
}

} // namespace xr

} // namespace audio

// =======================================================================
// XRSound's factory: Orbiter's sound, not XRSound.dll

XRSound *XRSound::CreateInstance (VESSEL *pVessel)
{
	const Vessel *v = nullptr;
	if (pVessel) {
		const Body *b = (const Body*)pVessel->GetHandle ();
		if (b && b->Type () == OBJTP_VESSEL) v = (const Vessel*)b;
	}
	LOGOUT ("XRSound: vessel %s on Orbiter's sound", pVessel ? pVessel->GetName () : "(none)");
	return new audio::XrInstance (v);
}

XRSound *XRSound::CreateInstance (const char *pUniqueModuleName)
{
	LOGOUT ("XRSound: module %s on Orbiter's sound", pUniqueModuleName ? pUniqueModuleName : "(none)");
	return new audio::XrInstance (std::string (pUniqueModuleName ? pUniqueModuleName : ""));
}
