// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "AudioSystem.h"
#include "DebugEnv.h"
#include "Loudness.h"
#include "Assets.h"
#include "Orbiter.h"
#include "Config.h"
#include "TimeData.h"
#include "Log.h"
#include "Psys.h"
#include "Vessel.h"
#include "BoomSynth.h"
#include "Replay.h"
#include "Ears.h"
#include "XRSoundShim.h"
#include "OrbiterScene.h"
#include "FlightSampler.h"
#include "Camera.h"
#include "miniaudio.h"
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdlib>

extern TimeData td;
extern PlanetarySystem *g_psys;
extern Vessel *g_focusobj;
extern Camera *g_camera;
extern Orbiter *g_pOrbiter;

namespace audio {

System::System (Orbiter *o): orbiter (o)
{
}

System::~System ()
{
	Stop ();
}

float System::SimFadeFor (bool running, double warp)
{
	return running && warp <= WARP_MUTE ? 1.0f : 0.0f;
}

void System::Start (bool graphicsClient)
{
	if (started) return;
	started = true;
	client = graphicsClient;

	const DebugEnv &env = Dbg ();
	const char *render = env.render.c_str ();
	const CFG_SOUNDPRM &s = orbiter->Cfg()->CfgSoundPrm;

	Engine::Options opt;
	// the listening level (Ears.h): on for the speakers; in a render (the
	// tests' physical levels) only if ORBITER_AUDIO_ADAPT=1 (listening reels)
	opt.adapt = env.adapt >= 0 ? env.adapt != 0 : env.render.empty ();
	if (!env.render.empty ()) {
		opt.mode = Engine::OFFLINE; // (renders regardless of the settings: tests)
	} else if (env.device >= 0) {
		opt.mode = env.device ? Engine::DEVICE : Engine::OFF;
		forcedOff = opt.mode == Engine::OFF;
		client = !forcedOff;
	} else {
		// (offscreen runs, tests and recordings, keep off the speakers)
		if (env.offscreen) client = false;
		opt.mode = client && s.bEnabled ? Engine::DEVICE : Engine::OFF;
	}
	if (opt.mode == Engine::OFF) {
		LOGOUT ("Audio: off");
		return;
	}
	if (!engine.Init (opt)) {
		LOGOUT ("Audio: no output device, sound off");
		return;
	}
	if (opt.mode == Engine::OFFLINE) {
		if (!wav.Open (render, engine.SampleRate(), engine.Channels())) {
			LOGOUT ("Audio: can't write %s, sound off", render);
			engine.Shutdown ();
			return;
		}
		LOGOUT ("Audio: rendering to %s (%u Hz, %u channels)", render, engine.SampleRate(), engine.Channels());
	} else {
		LOGOUT ("Audio: %s (%u Hz, %u channels)", engine.DeviceName().empty() ? "default device" : engine.DeviceName().c_str(),
			engine.SampleRate(), engine.Channels());
	}
	renderTime = 0.0;
	flushed = 0;
	// files decoded in the background on the device (the simulation never
	// waits on a disk); at once in a render (repeatable to the sample)
	// (ORBITER_AUDIO_ASYNC=1: in the background in a render too - tests of the device's way)
	Assets::Get ().SetAsync (opt.mode == Engine::DEVICE || env.async);
	if (opt.mode == Engine::DEVICE) PrefetchClicks ();
	ApplySettings (s);
	world.OnBoom ([this](const World::Boom &b) { Boom (b); });
}

void System::Boom (const World::Boom &b)
{
	double dp, T;
	const Air &la = world.ListenerAir ();
	BoomSynth::Strength (b.emitter->Desc ().length, b.mach, b.height, b.sAir.p, la.p, b.sAir.c, dp, T);
	LOGOUT ("Audio: boom from %s at %.3f s: Mach %.2f, %.0f m, %.1f Pa, %.0f ms", b.emitter->Desc ().name.c_str (), b.t, b.mach, b.height, dp, T * 1e3);
	const bool log = Dbg ().log > 0;
	if (log) printf ("Audio: boom from %s at %.3f s: Mach %.2f, %.0f m, %.1f Pa, %.0f ms\n", b.emitter->Desc ().name.c_str (), b.t, b.mach, b.height, dp, T * 1e3), fflush (stdout);
	if (td.Warp () > WARP_MUTE) return;
	// (a source not faster than sound doesn't boom: its path jumped - a
	// vessel moved by a script, a first frame - and the jump looked
	// supersonic)
	if (b.mach < 1.0) { LOGOUT ("Audio: (not a boom: Mach %.2f, a jump)", b.mach); return; }
	static uint32_t seed = 7;
	auto bs = BoomSynth::Make (engine, (float)T, seed++);
	if (!bs) return;
	double len = bs->Length ();
	world.PlayShot (std::move (bs), b.dir, (float)(dp / P_FULL), len);
}

std::string System::FindSound (const std::string &file)
{
	if (file.empty ()) return std::string ();
	std::string s = Assets::Get ().Resolve (file);
	if (!s.empty ()) return s;
	return Assets::Get ().Resolve ("Sound/" + file);
}

bool System::VesselEvent (const Vessel *v, const std::string &event)
{
	// a separation: the pyro's bang on the vessel (as its other sounds: through the air, the hull;
	// not across vacuum), the commander's call, the ground's reply (the vessel in focus)
	struct Ev { const char *name, *line; int radio; };
	static const Ev evs[] = {
		{"BoosterSeparation", "BoosterSeparation", Radio::BOOSTER_SEP},
		{"TankSeparation", "TankSeparation", Radio::TANK_SEP},
	};
	for (const Ev &e : evs) {
		if (event != e.name) continue;
		if (!FindSound ("Sound/Generated/pyro.flac").empty ()) Play (v, "Sound/Generated/pyro.flac", V3 (0, 0, 0), 10.0, false);   // (the bolts and separation motors: unmistakable over the engines - measured in a launch, -21 dB in the mix against the roar at -18)
		if (v == g_focusobj) {
			callouts.Crew (e.line);
			radio.OnEvent ((Radio::Event)e.radio);
		}
		const bool log = Dbg ().log > 0;
		if (log) printf ("Audio: vessel event %s\n", e.name), fflush (stdout);
		return true;
	}
	return false;
}

int System::Play (const Vessel *v, const std::string &file, const V3 &pos, double level, bool loop)
{
	if (!v) return PlayOn (UI, file, level, loop);
	if (!engine.Active ()) return 0;
	std::string path = FindSound (file);
	if (path.empty ()) { LOGOUT ("Audio: no sound file %s", file.c_str ()); return 0; }
	Played p;
	p.loop = loop, p.level = level, p.path = path;
	p.desc.body = v;
	p.vessel = world.Ref (v);
	p.desc.offset = pos;
	p.desc.level = (float)level;
	p.desc.refDist = 2.0f;
	p.desc.hull = 0.5f;
	p.desc.name = file;
	p.desc.delayed = true;   // (heard when the sound of its start arrives: 3 km off, 9 s later)
	return Launch (std::move (p));
}

int System::PlayOn (Bus bus, const std::string &file, double level, bool loop, bool speech)
{
	if (!engine.Active () || bus < 0 || bus >= NBUS) return 0;
	std::string path = FindSound (file);
	if (path.empty ()) { LOGOUT ("Audio: no sound file %s", file.c_str ()); return 0; }
	Played p;
	p.loop = loop, p.level = level, p.bus = bus, p.path = path;
	p.speech = speech && (bus == VOICE || bus == RADIO);
	return Launch (std::move (p));
}

int System::Launch (Played &&p)
{
	// its file decoded (in the background on the device: it starts when
	// it's there, a frame or so later; at once in a render)
	Assets::Get ().Prefetch (p.path, engine.SampleRate (), 0, p.speech);
	const int h = nextHandle++;
	Played &q = played[h] = std::move (p);
	q.pending = true;
	if (!StartPending (q)) { played.erase (h); return 0; }
	return h;
}

bool System::StartPending (Played &p)
{
	if (!p.pending) return true;
	bool failed = false;
	ClipRef clip = Assets::Get ().Peek (p.path, engine.SampleRate (), 0, failed);
	if (failed) { LOGOUT ("Audio: can't read %s", p.path.c_str ()); return false; }
	if (!clip) return true; // (still loading)
	p.pending = false;
	// speech (the voice's, the radio's buses) brought to SPEECH_LUFS: recordings of all levels alike
	if (p.speech) p.level *= LoudnessGain (p.path, SPEECH_LUFS);
	if (p.desc.body) {
		if (!world.Alive (p.vessel)) return false; // (its vessel gone meanwhile)
		p.desc.level = (float)(p.level * p.gate);
		p.em = world.Add (p.desc, MakeClipSource (engine, clip, p.loop));
		Emitter *e = world.Get (p.em);
		if (!e) return false;
		if (p.pitch != 1.0) e->SetPitch ((float)p.pitch);
		if (p.seek >= 0.0 && e->GetSource ()) e->GetSource ()->Seek (p.seek);
		if (p.paused) e->Hold (true);
		return true;
	}
	p.flat = std::make_unique<ClipSound>();
	if (!p.flat->Init (engine.Ma (), clip, engine.Group (p.bus), 0, p.loop)) return false;
	ma_sound *s = p.flat->Sound ();
	ma_sound_set_volume (s, (float)(p.level * p.gate));
	if (p.pitch != 1.0) ma_sound_set_pitch (s, (float)p.pitch);
	if (p.pan != 0.0f) ma_sound_set_pan (s, p.pan);
	if (p.seek >= 0.0) ma_sound_seek_to_second (s, (float)p.seek);
	if (!p.paused) ma_sound_start (s);
	return true;
}

void System::Drop (Played &p)
{
	world.Remove (p.em), p.em = EmitterId ();
	p.flat.reset ();
}

bool System::StopSound (int h)
{
	auto it = played.find (h);
	if (it == played.end ()) return false;
	Drop (it->second);
	played.erase (it);
	return true;
}

bool System::SetSound (int h, double level, double pitch)
{
	auto it = played.find (h);
	if (it == played.end ()) return false;
	Played &p = it->second;
	if (level >= 0.0) {
		p.level = level;
		ApplyLevel (p);
	}
	if (pitch > 0.0) {
		p.pitch = pitch;
		if (Emitter *e = world.Get (p.em)) e->SetPitch ((float)pitch);
		if (p.flat) ma_sound_set_pitch (p.flat->Sound (), (float)pitch);
	}
	return true;
}

void System::ApplyLevel (Played &p)
{
	const float g = (float)(p.level * p.gate);
	if (Emitter *e = world.Get (p.em)) e->SetLevel (g);
	if (p.flat) ma_sound_set_volume (p.flat->Sound (), g);
}

bool System::Playing (int h) const
{
	return played.count (h) > 0;   // (waiting for its file too)
}

System::Played *System::Find (int h)
{
	auto it = played.find (h);
	return it == played.end () ? nullptr : &it->second;
}

const System::Played *System::Find (int h) const
{
	auto it = played.find (h);
	return it == played.end () ? nullptr : &it->second;
}

bool System::Pause (int h, bool pause)
{
	Played *p = Find (h);
	if (!p) return false;
	if (p->paused == pause) return true;
	p->paused = pause;
	if (p->flat) { if (pause) ma_sound_stop (p->flat->Sound ()); else ma_sound_start (p->flat->Sound ()); }
	if (Emitter *e = world.Get (p->em)) e->Hold (pause);
	return true;
}

bool System::Paused (int h) const
{
	const Played *p = Find (h);
	return p && p->paused;
}

bool System::SetPan (int h, float pan)
{
	Played *p = Find (h);
	if (!p || p->desc.body) return false; // (sounds on a vessel are where they are)
	p->pan = std::clamp (pan, -1.0f, 1.0f);
	if (p->flat) ma_sound_set_pan (p->flat->Sound (), p->pan);
	return true;
}

float System::GetPan (int h) const
{
	const Played *p = Find (h);
	return p ? p->pan : -100.0f;
}

double System::GetPitch (int h) const
{
	const Played *p = Find (h);
	return p ? p->pitch : 0.0;
}

bool System::Seek (int h, double s)
{
	Played *p = Find (h);
	if (!p) return false;
	if (p->pending) { p->seek = std::max (0.0, s); return true; }
	if (p->flat) return ma_sound_seek_to_second (p->flat->Sound (), (float)std::max (0.0, s)) == MA_SUCCESS;
	Emitter *e = world.Get (p->em);
	return e && e->GetSource () && e->GetSource ()->Seek (s);
}

double System::Cursor (int h) const
{
	const Played *p = Find (h);
	if (!p) return -1.0;
	if (p->pending) return std::max (0.0, p->seek);
	if (p->flat) { float c = 0.0f; return ma_sound_get_cursor_in_seconds (const_cast<ma_sound*> (p->flat->Sound ()), &c) == MA_SUCCESS ? c : -1.0; }
	Emitter *e = world.Get (p->em);
	return e && e->GetSource () ? e->GetSource ()->Cursor () : -1.0;
}

bool System::SetGate (int h, double g)
{
	Played *p = Find (h);
	if (!p) return false;
	g = std::clamp (g, 0.0, 1.0);
	if (g != p->gate) { p->gate = g; ApplyLevel (*p); }
	return true;
}

bool System::Speaking () const
{
	for (auto &p : played) {
		const Played &q = p.second;
		if (q.bus != VOICE || q.desc.body || q.paused || q.gate <= 0.0 || q.level <= 0.0) continue;
		if (q.pending || (q.flat && ma_sound_is_playing (q.flat->Sound ()))) return true; // (a line about to start: speaking)
	}
	return false;
}

void System::UpdatePlayed ()
{
	for (auto it = played.begin (); it != played.end (); ) {
		Played &p = it->second;
		// (waiting for its file: started when it's there; gone if it can't be read)
		if (p.pending && !StartPending (p)) { Drop (p); it = played.erase (it); continue; }
		// (on a vessel gone: gone with it)
		const Emitter *e = world.Get (p.em);
		if (p.desc.body && !p.pending && !e) { Drop (p); it = played.erase (it); continue; }
		// (sounds not looped go when they have played)
		bool done = !p.loop && !p.paused && !p.pending && ((e && e->SourceDone ()) || (p.flat && ma_sound_at_end (p.flat->Sound ())));
		if (done) {
			Drop (p);
			it = played.erase (it);
		} else ++it;
	}
}

bool System::CalloutsOn () const
{
	if (Dbg ().callouts >= 0) return Dbg ().callouts != 0; // (tests)
	return orbiter->Cfg()->CfgSoundPrm.bCallouts;
}

bool System::MusicOn () const
{
	if (Dbg ().music >= 0) return Dbg ().music != 0;
	if (Rendering ()) return false; // (the tests' renders: the world alone)
	return orbiter->Cfg()->CfgSoundPrm.bMusic;
}

bool System::HrtfOn () const
{
	if (Dbg ().hrtf >= 0) return Dbg ().hrtf != 0;
	return orbiter->Cfg()->CfgSoundPrm.bHrtf;
}

void System::Explosion (const Vessel *v, double propellant)
{
	effects.Explosion (v, propellant);
	if (v && v == g_focusobj) music.Alarm ();
}

void System::Event (const Vessel *v, Callouts::Event e)
{
	// (the vessel in focus: its crew says it, the ground hears of it)
	if (callouts.OnEvent (v, e)) {
		if (e == Callouts::EV_DOCKED) radio.OnEvent (Radio::DOCKED);
		else if (e == Callouts::EV_UNDOCKED) radio.OnEvent (Radio::UNDOCKED);
		else if (e == Callouts::EV_SPLASHDOWN) flight.Splashdown (), radio.OnEvent (Radio::SPLASHDOWN);
	}
	if (v && v == g_focusobj && (e == Callouts::EV_PART_LOST || e == Callouts::EV_HEAT_DAMAGE)) music.Alarm ();
}

void System::PlayClick ()
{
	const int k = pendingClick;
	pendingClick = -1;
	if (k < 0 || !engine.Active ()) return;
	clickRng ^= clickRng << 13, clickRng ^= clickRng >> 17, clickRng ^= clickRng << 5;
	char f[64];
	if (k == CLICK_UI) {
		snprintf (f, sizeof (f), "Clicks/ui_%u.flac", 1 + clickRng % 3);
		PlayOn (UI, f, UI_CLICK, false, false);
		if (Dbg ().log) printf ("Audio: click %s (interface)\n", f), fflush (stdout);
		return;
	}
	if (xr::HasInstance (g_focusobj)) return; // (an XRSound vessel clicks its own)
	if (k == CLICK_SWITCH) snprintf (f, sizeof (f), "Clicks/switch_%s_%u.flac", switchUp ? "on" : "off", 1 + clickRng % 4), switchUp = !switchUp;
	else snprintf (f, sizeof (f), "Clicks/button_%u.flac", 1 + clickRng % 4);
	// (the recordings peak at -1 dBFS: at CLICK_P in the cabin; paused or
	// time accelerated - the cockpit's bus faded - on the interface's)
	const bool cabin = engine.SimFadeTarget () > 0.5f;
	if (cabin) PlayOn (INTERIOR, f, CLICK_P / P_FULL / 0.891, false, false);
	else PlayOn (UI, f, 1.4 * UI_CLICK, false, false);
	const bool log = Dbg ().log > 0;
	if (log) printf ("Audio: click %s (%s)\n", f, cabin ? "cabin" : "interface"), fflush (stdout);
}

void System::TraceFlight (const FlightSample *s)
{
	// (ORBITER_AUDIO_FLIGHT_TRACE=<file>: the samples, for Tests/Audio.Flight.cpp)
	const std::string &path = Dbg ().flightTrace;
	if (path.empty () || !s) return;
	if (!flightTrace) flightTrace = fopen (path.c_str (), "w");
	if (flightTrace) WriteSample (flightTrace, *s);
}

void System::PrefetchClicks ()
{
	// (the controls' clicks: there at the first click)
	char f[64];
	for (unsigned i = 1; i <= 4; i++) {
		const char *kinds[] = { "Clicks/ui_%u.flac", "Clicks/switch_on_%u.flac", "Clicks/switch_off_%u.flac", "Clicks/button_%u.flac" };
		for (const char *k : kinds) {
			snprintf (f, sizeof (f), k, i);
			const std::string p = FindSound (f);
			if (!p.empty ()) Assets::Get ().Prefetch (p, engine.SampleRate ());
		}
	}
}

void System::Idle ()
{
	if (!engine.Active ()) return;
	ApplySettings (orbiter->Cfg()->CfgSoundPrm);
	const auto now = std::chrono::steady_clock::now ();
	const double dt = idleT.time_since_epoch ().count () ? std::min (0.25, std::chrono::duration<double> (now - idleT).count ()) : 0.0;
	idleT = now;
	music.Update (dt, true);
	PlayClick ();
	UpdatePlayed ();
}

void System::Duck (double dt)
{
	// the voice speaking: the rest down 5 dB, quickly (80 ms); back slowly
	// (0.5 s) after it
	const float target = Speaking () ? 0.562f : 1.0f;
	const float d = engine.Duck ();
	const double tau = target < d ? 0.08 : 0.5;
	const float k = (float)(1.0 - std::exp (-std::max (dt, 0.0) / tau));
	float nd = d + (target - d) * k;
	if (std::fabs (nd - target) < 1e-3f) nd = target;
	engine.SetDuck (nd);
	// a transmission heard: the world and the music down 4 dB (a headset's
	// comms over the cabin), as quickly, back as slowly
	const float rt = radio.Transmitting () ? 0.631f : 1.0f;
	const float rd = engine.RadioDuck ();
	const float rk = (float)(1.0 - std::exp (-std::max (dt, 0.0) / (rt < rd ? 0.08 : 0.5)));
	float nr = rd + (rt - rd) * rk;
	if (std::fabs (nr - rt) < 1e-3f) nr = rt;
	engine.SetRadioDuck (nr);
}

void System::EndSession ()
{
	callouts.Reset ();
	flight.Reset ();
	if (flightTrace) fclose (flightTrace), flightTrace = nullptr;
	xr::Clear ();
	warpDrive.Clear ();
	for (auto &p : played) Drop (p.second);
	played.clear ();
	vessels.Clear ();
	effects.Clear ();
	ambience.Clear ();
	radio.Clear ();
	world.Clear ();
	testTone = EmitterId ();
	logT = -1.0;
	Assets::Get ().Forget (); // (files may come or go between sessions)
}

void System::TestTone ()
{
	const std::string &spec = Dbg ().testTone;
	if (spec.empty () || world.Get (testTone) || !g_psys) return;
	std::string name (spec);
	double freq = 440.0, level = 1.0;
	size_t c = name.find (':');
	if (c != std::string::npos) {
		std::string rest = name.substr (c + 1);
		name.resize (c);
		sscanf (rest.c_str (), "%lf:%lf", &freq, &level);
	}
	Vessel *v = g_psys->GetVessel (name.c_str (), true);
	if (!v) return;
	EmitterDesc d;
	d.body = v;
	d.level = (float)level;
	d.refDist = 10.0f;
	d.hull = 0.5f;
	d.boom = true;
	d.length = (float)(2.0 * v->Size ());
	d.name = "test tone on " + name;
	testTone = world.Add (d, MakeToneSource (engine, freq, 0.5));
	if (testTone) LOGOUT ("Audio: test tone %.0f Hz on %s", freq, name.c_str ());
}

void System::Stop ()
{
	// every sound goes while the engine is there: their nodes and sounds
	// belong to its graph (then the engine, and last the HRTFs they read)
	EndSession ();
	music.Clear ();
	if (wav.IsOpen ()) {
		LOGOUT ("Audio: rendered %.2f s", (double)wav.Frames() / std::max (1u, engine.SampleRate()));
		wav.Close ();
	}
	engine.Shutdown ();
	PathNode::SetHrtf (nullptr);
	Assets::Get ().Shutdown ();
	started = false;
}

void System::ApplySettings (const CFG_SOUNDPRM &s)
{
	// binaural: the HRTFs loaded at the engine's rate the first time
	if (HrtfOn ()) {
		if ((!hrtf.Loaded () || hrtf.Rate () != engine.SampleRate ()) && !hrtfTried) {
			hrtfTried = true;
			const std::string f = FindSound ("HRTF/kemar.hrtf");
			if (!f.empty () && hrtf.Load (f, engine.SampleRate ())) LOGOUT ("Audio: binaural (%s)", f.c_str ());
			else LOGOUT ("Audio: no HRTFs (Sound/HRTF/kemar.hrtf): panning");
		}
		PathNode::SetHrtf (hrtf.Loaded () ? &hrtf : nullptr);
	} else PathNode::SetHrtf (nullptr);
	// (a render is mixed at full volume, whatever the player's settings - or
	// at them: ORBITER_AUDIO_RENDER_VOLUMES=1, listening reels. Either way
	// ORBITER_AUDIO_SOLO=<bus>: that bus alone - outside, cockpit, voice,
	// radio, music, interface; the world: outside,cockpit)
	const std::string &solo = Dbg ().solo;
	static const char *names[NBUS] = { "outside", "cockpit", "voice", "radio", "music", "interface" };
	auto heard = [&](int b) { return !Rendering () || solo.empty () || solo.find (names[b]) != std::string::npos; };
	if (Rendering () && !Dbg ().renderVolumes) {
		engine.SetMasterGain (1.0f);
		for (int b = 0; b < NBUS; b++) engine.SetBusGain ((Bus)b, heard (b) ? 1.0f : 0.0f);
		return;
	}
	engine.SetMasterGain (s.bEnabled ? VolumeToGain (s.Master) : 0.0f);
	for (int b = 0; b < NBUS; b++) engine.SetBusGain ((Bus)b, heard (b) ? VolumeToGain (s.Bus[b]) : 0.0f);
	// the dynamics' compression (the listening level with it: off with it)
	engine.SetCompression (s.Compression);
	engine.SetAdapt (Dbg ().adapt >= 0 ? Dbg ().adapt != 0 : s.Compression != Ears::COMP_OFF);
}

void System::SettingsChanged ()
{
	const CFG_SOUNDPRM &s = orbiter->Cfg()->CfgSoundPrm;
	if (!engine.Active () && s.bEnabled && client && !forcedOff) {
		Engine::Options opt;
		opt.mode = Engine::DEVICE;
		opt.adapt = true;
		if (engine.Init (opt))
			LOGOUT ("Audio: %s (%u Hz, %u channels)", engine.DeviceName().empty() ? "default device" : engine.DeviceName().c_str(),
				engine.SampleRate(), engine.Channels());
	}
	if (engine.Active ()) ApplySettings (s);
}

void System::Step ()
{
	if (!engine.Active ()) return;
	ApplySettings (orbiter->Cfg()->CfgSoundPrm);

	// the instant replay: heard at its time while it plays forwards, not too
	// fast (its contacts aren't: no physics then); entering and leaving it,
	// time jumps
	Replay *rp = orbiter->GetReplay ();
	const bool replay = rp && rp->Active ();
	if (replay != wasReplay) { world.ResetPaths (); wasReplay = replay; }
	const double t = replay ? rp->Time () : td.SimT1;

	// pause and time acceleration: quick out, a little slower back in
	float fade = replay ? (rp->Playing () && rp->Speed () > 0.0 && rp->Speed () <= 2.0 ? 1.0f : 0.0f)
	                    : SimFadeFor (orbiter->IsRunning (), td.Warp ());
	engine.SetSimFade (fade, fade < engine.SimFadeTarget () ? 0.15 : 0.4);
	const bool wasHeld = engine.Suspended ();
	engine.Suspend ();   // (faded out: held where they are, until the fade comes back)
	const bool logHold = Dbg ().log > 0;
	if (logHold && wasHeld != engine.Suspended ()) printf ("Audio: the simulation's sounds %s at %.2f\n", engine.Suspended () ? "held" : "going on", td.SimT1), fflush (stdout);
	// the clock of what is heard: real time while the simulation's sounds are (the voice's queue,
	// the radio's pauses, the voice cap; the simulation's time drives propagation and the flight)
	const double playDt = fade > 0.0f ? td.UiDT () : 0.0;
	playT += playDt;

	// the sounds of the world, as heard now
	if (g_psys) {
		TestTone ();
		const bool cinematic = Dbg ().realism >= 0 ? Dbg ().realism == 1 : orbiter->Cfg()->CfgSoundPrm.Realism == 1; // (tests: ORBITER_AUDIO_REALISM)
		world.SetRealism (cinematic ? CINEMATIC : PHYSICAL);
		if (Dbg ().vessels) vessels.Update (t, replay);
		if (!replay) effects.Update (t);
		ambience.Update (t, playT, td.UiDT ());
		world.Update (scene.Take (t, playT), scene);
		// the voice, XRSound's sounds (not in the replay: it isn't flown)
		const bool cockpit = g_camera && g_camera->IsInternal ();
		if (!replay) {
			// (the flight's events followed even with the callouts off: the
			// ground answers them on the radio)
			callouts.SetMuted (!CalloutsOn ());
			// the flight of the vessel in focus: its moments, said by the crew, answered by the ground
			FlightSample fs;
			const bool sampled = SampleFlight (*this, g_focusobj, td.SimT1, td.SimDT, fs);
			flight.Step (world.Ref (g_focusobj).serial, sampled ? &fs : nullptr, td.Warp () > 10.0);
			TraceFlight (sampled ? &fs : nullptr);
			const bool logFlight = Dbg ().log > 0;
			if (logFlight)
				for (const FlightEvent &e : flight.Events ())
					printf ("Audio: flight: %s %g%s%s at %.2f\n", FlightEventName (e.kind), e.value, e.text.empty () ? "" : " ", e.text.c_str (), td.SimT1), fflush (stdout);
			callouts.Step (g_focusobj, flight, playT, td.Warp ());
			radio.OnFlight (flight.Events ());
			if (!Dbg ().radio) radio.Clear (); else radio.Update (td.SimT1, playT); // (tests: ORBITER_AUDIO_RADIO=0)
			xr::Step (*this, g_focusobj, cockpit, td.SimT1);
			warpDrive.Step (g_focusobj, cockpit, playDt);
		}
		UpdatePlayed ();
		Duck (td.UiDT ());
		const bool log = Dbg ().logStates;   // (each source's state; not with ORBITER_AUDIO_LOG=events)
		if (log && (logT < 0.0 || t - logT >= 0.5 || t < logT)) {
			if (logT < 0.0 || t - logT >= 0.5) {
				static double loadT = -1e9;
				if (std::fabs (t - loadT) >= 5.0) {
					loadT = t;
					LOGOUT ("Audio: load %.1f %% (worst block %.1f %%), %d sources playing, %d stopped (not heard)",
						100.0 * engine.Load (), 100.0 * engine.PeakLoad (), world.Voices (), world.Culled ());
				}
			}
			logT = t;
			const Air &la = world.ListenerAir ();
			for (auto &e : world.Emitters ()) {
				const Emitter::State &h = e->Heard ();
				const bool all = Dbg ().log >= 2;
				if (!all && h.air < 1e-6f && h.hull < 1e-6f && e->Id () != testTone) continue; // (silent; ORBITER_AUDIO_LOG=2 logs them too)
				LOGOUT ("Audio: %st %.2f %s: images %d, dist %.0f m, delay %.3f s, doppler %.4f, air %.4f, hull %.4f, cutoff %.0f Hz (listener: rho %.4g, c %.1f%s)",
					replay ? "replay " : "", t, e->Desc ().name.c_str (), h.images, h.dist, h.delay, h.doppler, h.air, h.hull, h.cutoff,
					la.rho, la.c, world.ListenerInside () ? ", in the cockpit" : "");
			}
		}
	}

	// the music (the launchpad's, behind it a scene of its own)
	music.Update (td.UiDT (), orbiter->IsBackdrop ());
	idleT = {};
	PlayClick ();

	if (Rendering ()) RenderStep (td.UiDT ());
}

void System::RenderStep (double dt)
{
	// the frames up to the time asked for (rounded: no drift over a long render)
	const unsigned rate = engine.SampleRate (), nch = engine.Channels ();
	renderTime += std::max (0.0, dt);
	uint64_t want = (uint64_t)std::llround (renderTime * rate);
	uint64_t have = wav.Frames ();
	const uint64_t BLOCK = 1024;
	if (block.size () < BLOCK * nch) block.resize (BLOCK * nch);
	while (have < want) {
		uint64_t n = std::min (BLOCK, want - have);
		engine.Render (block.data(), n);
		wav.Write (block.data(), n);
		have += n;
	}
	if (have - flushed >= rate) { // (a readable file about every second)
		wav.Flush ();
		flushed = have;
	}
}

} // namespace audio
