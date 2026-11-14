// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "XRSoundShim.h"
#include "DebugEnv.h"
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

	/// XRSound's volume 1 as a pressure: a file at full scale peaks at this [Pa]. In the cockpit
	/// (InternalOnly), and the wind at the ear: 1 Pa (94 dB) - a loud cockpit sound, well over the
	/// cabin's air (56 dB). On the vessel, at 2 m: Close 4 Pa (106 dB, a machine at hand; heard in
	/// its cockpit through the hull some 10 dB over the cabin's air), Medium 8, Far 16 (XRSound's
	/// steps of 6 dB). The voice's and the interface's buses: as a gain
	constexpr double XR_COCKPIT_PA = 1.0, XR_CLOSE_PA = 4.0;

	/// Where a playback type's sound goes, and its level there (oapiAudioPlay's: AUDIO_LEVEL_PA)
	struct Route { bool onVessel; Bus bus; double level; };
	Route RouteFor (PT t, bool module)
	{
		const double close = XR_CLOSE_PA * AUDIO_LEVEL_PA, inside = XR_COCKPIT_PA * AUDIO_LEVEL_PA;
		if (module) return {false, UI, 1.0};
		switch (t) {
		case PT::BothViewFar:    return {true, EXTERIOR, 4.0 * close};
		case PT::BothViewMedium: return {true, EXTERIOR, 2.0 * close};
		case PT::BothViewClose:  return {true, EXTERIOR, close};
		case PT::InternalOnly:   return {false, INTERIOR, inside};
		case PT::Radio:          return {false, VOICE, 1.0};
		case PT::Wind:           return {false, EXTERIOR, inside};
		default:                 return {false, UI, 1.0};
		}
	}

	/// A file played as a route has it (oapiAudio*: XRSound on the public API); 0: not played
	int PlayRouted (const Vessel *v, const std::string &file, const Route &r, double volume, bool loop)
	{
		const std::string plays = xr::Voice (file);   // (XRSound's default lines: in Orbiter's voice, as set)
		const VECTOR3 at = _V(0, 0, 0);
		return r.onVessel ? oapiAudioPlay ((OBJHANDLE)v, plays.c_str (), &at, volume * r.level, loop)
		                  : oapiAudioPlayOn (r.bus, plays.c_str (), volume * r.level, loop);
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
	explicit XrInstance (const Vessel *v): vessel (v), isModule (!v)   // (no vessel: as a module's)
	{
		if (System *s = Sys ()) ref = s->GetWorld ().Ref (v);
		Instances ().push_back (this);
	}
	explicit XrInstance (const std::string &m): module (m), isModule (true) { Instances ().push_back (this); }
	~XrInstance () override
	{
		auto &all = Instances ();
		all.erase (std::remove (all.begin (), all.end (), this), all.end ());
		for (auto &sl : slots) if (sl.second.h) oapiAudioStop (sl.second.h);
	}

	static std::vector<XrInstance*> &Instances () { static std::vector<XrInstance*> v; return v; }
	static XrInstance *For (const Vessel *v)
	{
		System *s = Sys ();
		const BodyRef r = s && v ? s->GetWorld ().Known (v) : BodyRef ();
		if (!r) return nullptr;
		for (auto *i : Instances ()) if (!i->isModule && i->ref == r) return i;
		return nullptr;
	}
	/// Its vessel, while it is there (deleted: null - its sounds can't play)
	const Vessel *V () const
	{
		System *s = Sys ();
		return s && s->GetWorld ().Alive (ref) ? vessel : nullptr;
	}

	bool IsPresent () const override { return true; }
	float GetVersion () const override { return 3.0f; }

	bool LoadWav (const int id, const char *file, const PlaybackType type) override
	{
		if (!file || !*file || !oapiAudioHasFile (xr::Voice (file).c_str ())) return false;
		Slot &s = slots[id];
		if (s.h) oapiAudioStop (s.h);
		s = Slot ();
		s.file = file, s.type = type;
		return true;
	}

	bool PlayWav (const int id, const bool loop, const float volume) override
	{
		if (!Sys ()) return false;
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
		if (s.h && oapiAudioPlaying (s.h)) {
			// (playing: its volume changes; it goes on)
			s.loop = loop;
			Apply (s);
			return true;
		}
		if (!isModule && !V ()) return false;   // (its vessel gone)
		s.loop = loop;
		s.h = PlayRouted (V (), s.file, RouteFor (s.type, isModule), s.volume * s.gate, loop);
		if (!s.h) return false;
		s.applied = s.gate;
		Gate (s, lastFocus, lastCockpit);
		return true;
	}

	bool StopWav (const int id) override
	{
		auto it = slots.find (id);
		if (it == slots.end ()) return false;
		if (it->second.h) oapiAudioStop (it->second.h);
		it->second.h = 0;
		return true;
	}

	bool IsWavPlaying (const int id) const override
	{
		const Slot *s = Find (id);
		return s && s->h && oapiAudioPlaying (s->h) && !oapiAudioPaused (s->h);
	}

	bool SetPaused (const int id, const bool pause) override
	{
		Slot *s = Find (id);
		return s && s->h && oapiAudioPause (s->h, pause);
	}

	bool IsPaused (const int id) const override
	{
		const Slot *s = Find (id);
		return s && s->h && oapiAudioPaused (s->h);
	}

	bool SetDefaultSoundEnabled (const DefaultSoundID id, const bool on) override
	{
		const DefaultSound *d = Default (id);
		if (!d || !V ()) return false;
		enabled[id] = on;
		// (a synthesizer: on or off for the vessel - its tune kept)
		const int sound = d->kind == ENGINE_MAIN ? AUDIO_SOUND_ENGINES : d->kind == RCS ? AUDIO_SOUND_RCS
			: d->kind == AIR ? AUDIO_SOUND_AIR : d->kind == CONTACT ? AUDIO_SOUND_CONTACTS : -1;
		if (sound >= 0) oapiAudioDefaultSound ((OBJHANDLE)V (), sound, on);
		if (!on) StopWav (id);
		return true;
	}

	bool GetDefaultSoundEnabled (const DefaultSoundID id) const override
	{
		return Default (id) && Enabled (id);
	}

	bool SetDefaultSoundGroupFolder (const DefaultSoundID id, const char *folder) override
	{
		const DefaultSound *d = Default (id);
		if (!d || d->kind != GROUP || !folder || !*folder || isModule) return false;
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
		return s && s->h && oapiAudioPan (s->h, pan);
	}

	float GetPan (const int id) override
	{
		Slot *s = Find (id);
		return s && s->h && oapiAudioPlaying (s->h) ? (float)oapiAudioGetPan (s->h) : -100.0f;
	}

	bool SetPlaybackSpeed (const int id, const float speed) override
	{
		Slot *s = Find (id);
		return s && s->h && speed > 0.0f && oapiAudioSet (s->h, -1.0, speed);
	}

	float GetPlaybackSpeed (const int id) override
	{
		Slot *s = Find (id);
		return s && s->h ? (float)oapiAudioGetPitch (s->h) : 0.0f;
	}

	bool SetPlayPosition (const int id, const unsigned int ms) override
	{
		Slot *s = Find (id);
		return s && s->h && oapiAudioSeek (s->h, ms * 1e-3);
	}

	int GetPlayPosition (const int id) override
	{
		Slot *s = Find (id);
		double c = s && s->h ? oapiAudioCursor (s->h) : -1.0;
		return c < 0.0 ? -1 : (int)(c * 1e3 + 0.5);
	}

	// ---- for the core ----------------------------------------------------

	/// (gate: where it may be heard - 0..1, by its type; applied: the gate its level has now)
	struct Slot { std::string file; PT type = PT::Radio; int h = 0; double volume = 1.0, gate = 1.0, applied = -1.0; bool loop = false; };

	/// Where its sounds may be heard (each step)
	void Step (const Vessel *focus, bool cockpit)
	{
		lastFocus = focus, lastCockpit = cockpit;
		for (auto &sl : slots) {
			Slot &s = sl.second;
			if (!s.h) continue;
			if (!oapiAudioPlaying (s.h)) { s.h = 0; continue; }
			Gate (s, focus, cockpit);
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

	std::string module;

private:
	const Vessel *vessel = nullptr;   // (V (): while it is there)
	BodyRef ref;
	bool isModule = false;
	std::string ClassName () const { const Vessel *v = V (); return v && v->ClassName () ? v->ClassName () : ""; }

	/// Its level: its volume at its route's level, times its gate
	void Apply (Slot &s)
	{
		oapiAudioSet (s.h, s.volume * RouteFor (s.type, isModule).level * s.gate, -1.0);
		s.applied = s.gate;
	}

	void Gate (Slot &s, const Vessel *focus, bool cockpit)
	{
		double g = 1.0;
		if (const Vessel *own = V ()) {
			switch (s.type) {
			case PT::InternalOnly: g = own == focus && cockpit ? 1.0 : 0.0; break;
			case PT::Radio:        g = own == focus ? 1.0 : 0.0; break;
			case PT::Wind: {
				Vessel *v = const_cast<Vessel*>(own);
				VESSEL *vi = v->GetModuleInterface ();
				g = !cockpit && vi ? std::min (1.0, vi->GetAtmPressure () / 101325.0) : 0.0;
				break; }
			default: break;
			}
		}
		s.gate = std::clamp (g, 0.0, 1.0);
		if (s.gate != s.applied) Apply (s);
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
		double gate[2] = {1.0, 1.0};   // (the gate each has now: one-shot, moving)
	};
	struct ClassSounds {
		const Vessel *v = nullptr;  // (while ref is alive)
		BodyRef ref;
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

	ClassSounds &ClassFor (System &sys, const Vessel *v)
	{
		auto &all = Classes ();
		const BodyRef r = sys.GetWorld ().Ref (v);
		for (auto &c : all) if (c->ref == r) return *c;
		all.push_back (std::make_unique<ClassSounds>());
		all.back ()->v = v, all.back ()->ref = r;
		return *all.back ();
	}

	void Play1 (System &sys, const Vessel *v, const std::string &file, PT type, bool loop, int &h, double &gate)
	{
		if (file.empty ()) return;
		Route r = RouteFor (type, false);
		if (r.bus == VOICE && !sys.CalloutsOn ()) return; // (a voice's line: the callouts switched off)
		if (h) oapiAudioStop (h);
		h = PlayRouted (v, file, r, 1.0, loop);
		gate = 1.0;
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
				// (XRSound's engines: BothViewFar)
				const Route r = RouteFor (PT::BothViewFar, false);
				if (!cs.engineH[k] || !oapiAudioPlaying (cs.engineH[k])) cs.engineH[k] = PlayRouted (cs.v, cs.engine[k], r, lvl, true);
				else oapiAudioSet (cs.engineH[k], lvl * r.level, -1.0);
			} else if (cs.engineH[k]) {
				oapiAudioStop (cs.engineH[k]);
				cs.engineH[k] = 0;
			}
		}
		if (own && !cs.synthOff) { oapiAudioDefaultSound ((OBJHANDLE)cs.v, AUDIO_SOUND_ENGINES, false); cs.synthOff = true; }

		// animations: opening (from closed), moving, open (at its end),
		// closing (from open), closed (at its start)
		for (AnimTrack &t : cs.anims) {
			if (t.a.index < 0) continue;
			double st = vi->GetAnimation ((UINT)t.a.index);
			if (st < 0.0) continue; // (no such animation)
			PT type = TypeNamed (t.a.playback, PT::BothViewClose);
			if (t.prev >= 0.0 && st != t.prev) {
				if (t.prev <= 0.0 && st > 0.0) Play1 (sys, cs.v, t.a.opening, type, false, t.oneShot, t.gate[0]);
				else if (t.prev >= 1.0 && st < 1.0) Play1 (sys, cs.v, t.a.closing, type, false, t.oneShot, t.gate[0]);
				if (st >= 1.0) Play1 (sys, cs.v, t.a.open, type, false, t.oneShot, t.gate[0]);
				else if (st <= 0.0) Play1 (sys, cs.v, t.a.closed, type, false, t.oneShot, t.gate[0]);
				if (st > 0.0 && st < 1.0 && !t.a.moving.empty () && (!t.moving || !oapiAudioPlaying (t.moving)))
					Play1 (sys, cs.v, t.a.moving, type, true, t.moving, t.gate[1]);
			} else if (t.moving && (st == t.prev || st <= 0.0 || st >= 1.0)) {
				oapiAudioStop (t.moving);
				t.moving = 0;
			}
			if ((st <= 0.0 || st >= 1.0) && t.moving) { oapiAudioStop (t.moving); t.moving = 0; }
			// (heard as their type says: its level at its route's, times where it may be heard)
			const double g = type == PT::InternalOnly ? (cs.v == focus && cockpit ? 1.0 : 0.0) : type == PT::Radio ? (cs.v == focus ? 1.0 : 0.0) : 1.0;
			int hs[2] = {t.oneShot, t.moving};
			for (int i = 0; i < 2; i++) {
				if (!hs[i] || g == t.gate[i]) continue;
				oapiAudioSet (hs[i], RouteFor (type, false).level * g, -1.0);
				t.gate[i] = g;
			}
			t.prev = st;
		}
	}
}

namespace xr {

namespace {
	/// <name> if the file is one of XRSound's default sounds ("XRSound/Default/<name>.<ext>",
	/// either slash, any case, under Sound/ or not), else empty
	std::string XrDefaultName (const std::string &file)
	{
		std::string f = file;
		std::replace (f.begin (), f.end (), '\\', '/');
		std::string low = f;
		std::transform (low.begin (), low.end (), low.begin (), [](unsigned char c) { return (char)std::tolower (c); });
		size_t at = std::string::npos;
		for (const char *pre : { "sound/xrsound/default/", "xrsound/default/" })
			if (low.rfind (pre, 0) == 0) { at = strlen (pre); break; }
		if (at == std::string::npos) return std::string ();
		std::string name = f.substr (at);
		if (name.empty () || name.find ('/') != std::string::npos) return std::string ();   // (not a subfolder's: chatter, music)
		size_t dot = name.rfind ('.');
		return dot == std::string::npos ? name : name.substr (0, dot);
	}

	/// Callouts in Orbiter's voice (Sound/Voice), not XRSound's recordings (the settings;
	/// ORBITER_AUDIO_VOICE=orbiter or xrsound)
	bool OrbiterVoice ()
	{
		if (Dbg ().voice >= 0) return Dbg ().voice != 0;
		return !g_pOrbiter || g_pOrbiter->Cfg()->CfgSoundPrm.CalloutVoice == 0;
	}
}

std::string Voice (const std::string &file)
{
	// XRSound's default voice lines in Orbiter's voice: XR vessels and add-ons that name the
	// defaults speak with it too. A default with no Orbiter line (a sound, music) stays XRSound's
	std::string xr = XrDefaultName (file);
	if (xr.empty () || !OrbiterVoice ()) return file;
	const std::string v = "Sound/Voice/" + xr + ".flac";
	return System::FindSound (v).empty () ? file : v;
}

void Step (System &sys, const Vessel *focus, bool cockpit, double simt)
{
	for (auto *in : XrInstance::Instances ()) in->Step (focus, cockpit);
	// (vessels deleted: their class's sounds gone - their handles died with them)
	auto &all = Classes ();
	all.erase (std::remove_if (all.begin (), all.end (), [&sys](const std::unique_ptr<ClassSounds> &c) { return !sys.GetWorld ().Alive (c->ref); }), all.end ());
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
		if (any) StepClass (sys, ClassFor (sys, v), focus, cockpit);
	}
}

void Clear ()
{
	Classes ().clear ();
	XrConfig::Reload ();
}

bool HasInstance (const Vessel *v)
{
	return v && XrInstance::For (v);
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
