// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "AudioSystem.h"
#include "Loudness.h"
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

	const char *render = getenv ("ORBITER_AUDIO_RENDER");
	const char *force = getenv ("ORBITER_AUDIO");
	const CFG_SOUNDPRM &s = orbiter->Cfg()->CfgSoundPrm;

	Engine::Options opt;
	// the listening level (Ears.h): on for the speakers; in a render (the
	// tests' physical levels) only if ORBITER_AUDIO_ADAPT=1 (listening reels)
	const char *adapt = getenv ("ORBITER_AUDIO_ADAPT");
	opt.adapt = adapt && *adapt ? *adapt != '0' : !(render && *render);
	if (render && *render) {
		opt.mode = Engine::OFFLINE; // (renders regardless of the settings: tests)
	} else if (force && *force) {
		opt.mode = *force != '0' ? Engine::DEVICE : Engine::OFF;
		forcedOff = opt.mode == Engine::OFF;
		client = !forcedOff;
	} else {
		// (offscreen runs, tests and recordings, keep off the speakers)
		const char *offscreen = getenv ("ORBITER_VK_OFFSCREEN");
		if (offscreen && *offscreen && *offscreen != '0') client = false;
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
	ApplySettings (s);
	world.OnBoom ([this](const World::Boom &b) { Boom (b); });
}

void System::Boom (const World::Boom &b)
{
	double dp, T;
	const Air &la = world.ListenerAir ();
	BoomSynth::Strength (b.emitter->Desc ().length, b.mach, b.height, b.sAir.p, la.p, b.sAir.c, dp, T);
	LOGOUT ("Audio: boom from %s at %.3f s: Mach %.2f, %.0f m, %.1f Pa, %.0f ms", b.emitter->Desc ().name.c_str (), b.t, b.mach, b.height, dp, T * 1e3);
	static const bool log = getenv ("ORBITER_AUDIO_LOG") != nullptr;
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

namespace {
	/// A path as given, its backslashes as slashes, its case as on the disk
	/// (files named in Windows' manner: add-ons' cfgs)
	std::string ResolveCase (std::string f)
	{
		std::replace (f.begin (), f.end (), '\\', '/');
		std::error_code ec;
		if (f.empty () || std::filesystem::exists (f, ec)) return f;
		namespace fs = std::filesystem;
		fs::path in (f), out;
		for (const fs::path &part : in) {
			fs::path next = out.empty () ? part : out / part;
			if (!fs::exists (next, ec)) {
				bool found = false;
				std::string want = part.string ();
				for (const auto &de : fs::directory_iterator (out.empty () ? fs::path (".") : out, ec)) {
					std::string name = de.path ().filename ().string ();
					if (name.size () == want.size () && std::equal (name.begin (), name.end (), want.begin (),
						[](char a, char b) { return std::tolower ((unsigned char)a) == std::tolower ((unsigned char)b); })) {
						next = out.empty () ? fs::path (name) : out / name;
						found = true;
						break;
					}
				}
				if (!found) return std::string ();
			}
			out = next;
		}
		return out.string ();
	}
}

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
		static const char *env = getenv ("ORBITER_AUDIO_VOICE");
		if (env && *env) return std::tolower ((unsigned char)*env) != 'x';
		return !g_pOrbiter || g_pOrbiter->Cfg()->CfgSoundPrm.CalloutVoice == 0;
	}
}

std::string System::FindSound (const std::string &file)
{
	if (file.empty ()) return std::string ();
	// XRSound's default voice lines in Orbiter's voice: the one place every lookup passes
	// (the callouts, XRSound configs, the XRSound shim), so XR vessels and add-ons that name
	// the defaults speak with it too. A default with no Orbiter line (a sound, music) is XRSound's.
	std::string xr = XrDefaultName (file);
	if (!xr.empty () && OrbiterVoice ()) {
		std::string v = ResolveCase ("Sound/Voice/" + xr + ".flac");
		if (!v.empty ()) return v;
	}
	std::string s = ResolveCase (file);
	if (!s.empty ()) return s;
	return ResolveCase ("Sound/" + file);
}

bool System::VesselEvent (const Vessel *v, const std::string &event)
{
	// a separation: the pyro's bang on the vessel (as its other sounds: through the air, the hull;
	// not across vacuum), the commander's call, the ground's reply (the vessel in focus)
	struct Ev { const char *name, *line; int radio; };
	static const Ev evs[] = {
		{"BoosterSeparation", "Sound/Voice/Booster Separation.flac", Radio::BOOSTER_SEP},
		{"TankSeparation", "Sound/Voice/Tank Separation.flac", Radio::TANK_SEP},
	};
	for (const Ev &e : evs) {
		if (event != e.name) continue;
		if (!FindSound ("Sound/Generated/pyro.flac").empty ()) Play (v, "Sound/Generated/pyro.flac", V3 (0, 0, 0), 10.0, false);   // (the bolts and separation motors: unmistakable over the engines - measured in a launch, -21 dB in the mix against the roar at -18)
		if (v == g_focusobj) {
			callouts.Crew (e.line, 3.0);
			radio.OnEvent ((Radio::Event)e.radio);
		}
		static const bool log = getenv ("ORBITER_AUDIO_LOG") != nullptr;
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
	p.loop = loop, p.level = level;
	EmitterDesc d;
	d.body = v;
	d.offset = pos;
	d.level = (float)level;
	d.refDist = 2.0f;
	d.hull = 0.5f;
	d.name = file;
	p.em = world.Add (d, MakeFileSource (engine, path, loop));
	if (!p.em) return 0;
	int h = nextHandle++;
	played[h] = std::move (p);
	return h;
}

int System::PlayOn (Bus bus, const std::string &file, double level, bool loop, bool speech)
{
	if (!engine.Active () || bus < 0 || bus >= NBUS) return 0;
	std::string path = FindSound (file);
	if (path.empty ()) { LOGOUT ("Audio: no sound file %s", file.c_str ()); return 0; }
	if (speech && (bus == VOICE || bus == RADIO)) level *= LoudnessGain (path, SPEECH_LUFS);
	Played p;
	p.loop = loop, p.level = level, p.bus = bus;
	p.flat = std::make_unique<ma_sound>();
	ma_uint32 flags = MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_DECODE;
	if (ma_sound_init_from_file (engine.Ma (), path.c_str (), flags, engine.Group (bus), NULL, p.flat.get ()) != MA_SUCCESS) return 0;
	ma_sound_set_looping (p.flat.get (), loop ? MA_TRUE : MA_FALSE);
	ma_sound_set_volume (p.flat.get (), (float)level);
	ma_sound_start (p.flat.get ());
	int h = nextHandle++;
	played[h] = std::move (p);
	return h;
}

bool System::StopSound (int h)
{
	auto it = played.find (h);
	if (it == played.end ()) return false;
	if (it->second.em) world.Remove (it->second.em);
	if (it->second.flat) ma_sound_uninit (it->second.flat.get ());
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
		if (p.em) p.em->SetPitch ((float)pitch);
		if (p.flat) ma_sound_set_pitch (p.flat.get (), (float)pitch);
	}
	return true;
}

void System::ApplyLevel (Played &p)
{
	const float g = (float)(p.level * p.gate);
	if (p.em) p.em->SetLevel (g);
	if (p.flat) ma_sound_set_volume (p.flat.get (), g);
}

bool System::Playing (int h) const
{
	return played.count (h) > 0;
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
	if (p->flat) { if (pause) ma_sound_stop (p->flat.get ()); else ma_sound_start (p->flat.get ()); }
	if (p->em && p->em->GetSource ()) p->em->GetSource ()->Pause (pause);
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
	if (!p || !p->flat) return false; // (sounds on a vessel are where they are)
	p->pan = std::clamp (pan, -1.0f, 1.0f);
	ma_sound_set_pan (p->flat.get (), p->pan);
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
	if (p->flat) return ma_sound_seek_to_second (p->flat.get (), (float)std::max (0.0, s)) == MA_SUCCESS;
	return p->em && p->em->GetSource () && p->em->GetSource ()->Seek (s);
}

double System::Cursor (int h) const
{
	const Played *p = Find (h);
	if (!p) return -1.0;
	if (p->flat) { float c = 0.0f; return ma_sound_get_cursor_in_seconds (p->flat.get (), &c) == MA_SUCCESS ? c : -1.0; }
	return p->em && p->em->GetSource () ? p->em->GetSource ()->Cursor () : -1.0;
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
	for (auto &p : played)
		if (p.second.bus == VOICE && p.second.flat && !p.second.paused && p.second.gate > 0.0 && p.second.level > 0.0
			&& ma_sound_is_playing (p.second.flat.get ())) return true;
	return false;
}

void System::UpdatePlayed ()
{
	// (sounds not looped go when they have played)
	for (auto it = played.begin (); it != played.end (); ) {
		Played &p = it->second;
		bool done = !p.loop && !p.paused && ((p.em && p.em->SourceDone ()) || (p.flat && ma_sound_at_end (p.flat.get ())));
		if (done) {
			if (p.em) world.Remove (p.em);
			if (p.flat) ma_sound_uninit (p.flat.get ());
			it = played.erase (it);
		} else ++it;
	}
}

bool System::CalloutsOn () const
{
	static const char *env = getenv ("ORBITER_AUDIO_CALLOUTS"); // (tests: 0 or 1)
	if (env && *env) return *env != '0';
	return orbiter->Cfg()->CfgSoundPrm.bCallouts;
}

bool System::MusicOn () const
{
	static const char *env = getenv ("ORBITER_AUDIO_MUSIC"); // (0 or 1)
	if (env && *env) return *env != '0';
	if (Rendering ()) return false; // (the tests' renders: the world alone)
	return orbiter->Cfg()->CfgSoundPrm.bMusic;
}

bool System::HrtfOn () const
{
	static const char *env = getenv ("ORBITER_AUDIO_HRTF"); // (0 or 1)
	if (env && *env) return *env != '0';
	return orbiter->Cfg()->CfgSoundPrm.bHrtf;
}

void System::Explosion (const Vessel *v, double propellant)
{
	effects.Explosion (v, propellant);
	if (v && v == g_focusobj) music.Alarm ();
}

void System::Event (const Vessel *v, Callouts::Event e)
{
	callouts.OnEvent (v, e);
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
		if (getenv ("ORBITER_AUDIO_LOG")) printf ("Audio: click %s (interface)\n", f), fflush (stdout);
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
	static const bool log = getenv ("ORBITER_AUDIO_LOG") != nullptr;
	if (log) printf ("Audio: click %s (%s)\n", f, cabin ? "cabin" : "interface"), fflush (stdout);
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
	// the voice speaking: the rest down 9 dB, quickly (80 ms); back slowly
	// (0.5 s) after it
	const float target = Speaking () ? 0.355f : 1.0f;
	const float d = engine.Duck ();
	const double tau = target < d ? 0.08 : 0.5;
	const float k = (float)(1.0 - std::exp (-std::max (dt, 0.0) / tau));
	float nd = d + (target - d) * k;
	if (std::fabs (nd - target) < 1e-3f) nd = target;
	engine.SetDuck (nd);
	// a transmission heard: the world and the music down 6 dB (a headset's
	// comms over the cabin), as quickly, back as slowly
	const float rt = radio.Transmitting () ? 0.5f : 1.0f;
	const float rd = engine.RadioDuck ();
	const float rk = (float)(1.0 - std::exp (-std::max (dt, 0.0) / (rt < rd ? 0.08 : 0.5)));
	float nr = rd + (rt - rd) * rk;
	if (std::fabs (nr - rt) < 1e-3f) nr = rt;
	engine.SetRadioDuck (nr);
}

void System::EndSession ()
{
	callouts.Reset ();
	xr::Clear ();
	warpDrive.Clear ();
	for (auto &p : played) if (p.second.flat) ma_sound_uninit (p.second.flat.get ());
	played.clear ();
	vessels.Clear ();
	effects.Clear ();
	ambience.Clear ();
	radio.Clear ();
	world.Clear ();
	testTone = nullptr;
	logT = -1.0;
}

void System::Forget (const Body *body)
{
	if (testTone && testTone->Desc ().body == body) testTone = nullptr;
	for (auto it = played.begin (); it != played.end (); )
		it = (it->second.em && it->second.em->Desc ().body == body) ? played.erase (it) : std::next (it);
	vessels.Forget (body);
	effects.Forget (body);
	world.Forget (body);
	xr::Forget (body);
}

void System::TestTone ()
{
	static const char *spec = getenv ("ORBITER_AUDIO_TEST_TONE");
	if (!spec || !*spec || testTone || !g_psys) return;
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
	PathNode::SetHrtf (nullptr);
	music.Clear ();
	ambience.Clear ();
	radio.Clear ();
	warpDrive.Clear ();
	for (auto &p : played) if (p.second.flat) ma_sound_uninit (p.second.flat.get ());
	played.clear ();
	if (wav.IsOpen ()) {
		LOGOUT ("Audio: rendered %.2f s", (double)wav.Frames() / std::max (1u, engine.SampleRate()));
		wav.Close ();
	}
	engine.Shutdown ();
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
	// (a render is mixed at full volume, whatever the player's settings)
	static const bool renderVolumes = getenv ("ORBITER_AUDIO_RENDER_VOLUMES") && *getenv ("ORBITER_AUDIO_RENDER_VOLUMES") == '1'; // (listening reels)
	if (Rendering () && !renderVolumes) {
		// (ORBITER_AUDIO_SOLO=<bus>: that bus alone - outside, cockpit,
		// voice, radio, music, interface; the world: outside,cockpit)
		static const char *solo = getenv ("ORBITER_AUDIO_SOLO");
		static const char *names[NBUS] = { "outside", "cockpit", "voice", "radio", "music", "interface" };
		engine.SetMasterGain (1.0f);
		for (int b = 0; b < NBUS; b++) engine.SetBusGain ((Bus)b, !solo || !*solo || strstr (solo, names[b]) ? 1.0f : 0.0f);
		return;
	}
	engine.SetMasterGain (s.bEnabled ? VolumeToGain (s.Master) : 0.0f);
	for (int b = 0; b < NBUS; b++) engine.SetBusGain ((Bus)b, VolumeToGain (s.Bus[b]));
	// the dynamics' compression (the listening level with it: off with it)
	engine.SetCompression (s.Compression);
	static const char *adaptEnv = getenv ("ORBITER_AUDIO_ADAPT");
	engine.SetAdapt (adaptEnv && *adaptEnv ? *adaptEnv != '0' : s.Compression != Ears::COMP_OFF);
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

	// the sounds of the world, as heard now
	if (g_psys) {
		TestTone ();
		static const char *realismEnv = getenv ("ORBITER_AUDIO_REALISM"); // (tests: physical|cinematic)
		bool cinematic = realismEnv && *realismEnv ? (realismEnv[0] == 'c' || realismEnv[0] == 'C') : orbiter->Cfg()->CfgSoundPrm.Realism == 1;
		world.SetRealism (cinematic ? CINEMATIC : PHYSICAL);
		static const bool noVessels = getenv ("ORBITER_AUDIO_VESSELS") && *getenv ("ORBITER_AUDIO_VESSELS") == '0';
		if (!noVessels) vessels.Update (t, replay);
		if (!replay) effects.Update (t);
		ambience.Update (t, td.UiDT ());
		world.Update (t);
		// the voice, XRSound's sounds (not in the replay: it isn't flown)
		const bool cockpit = g_camera && g_camera->IsInternal ();
		if (!replay) {
			// (the flight's events followed even with the callouts off: the
			// ground answers them on the radio)
			callouts.SetMuted (!CalloutsOn ());
			callouts.Step (g_focusobj, td.SimT1, td.SimDT, td.Warp ());
			static const bool radioOff = getenv ("ORBITER_AUDIO_RADIO") && *getenv ("ORBITER_AUDIO_RADIO") == '0'; // (tests)
			if (radioOff) radio.Clear (); else radio.Update (td.SimT1);
			xr::Step (*this, g_focusobj, cockpit, td.SimT1);
			warpDrive.Step (g_focusobj, cockpit, td.UiDT ());
		}
		UpdatePlayed ();
		Duck (td.UiDT ());
		static const bool log = getenv ("ORBITER_AUDIO_LOG") != nullptr;
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
				static const bool all = getenv ("ORBITER_AUDIO_LOG") && atoi (getenv ("ORBITER_AUDIO_LOG")) >= 2;
				if (!all && h.air < 1e-6f && h.hull < 1e-6f && e.get () != testTone) continue; // (silent; ORBITER_AUDIO_LOG=2 logs them too)
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
