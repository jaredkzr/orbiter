// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "AudioSystem.h"
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
#include <cmath>
#include <cstdlib>

extern TimeData td;
extern PlanetarySystem *g_psys;
extern Vessel *g_focusobj;
extern Camera *g_camera;

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

std::string System::FindSound (const std::string &file)
{
	if (file.empty ()) return std::string ();
	std::string s = ResolveCase (file);
	if (!s.empty ()) return s;
	return ResolveCase ("Sound/" + file);
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

int System::PlayOn (Bus bus, const std::string &file, double level, bool loop)
{
	if (!engine.Active () || bus < 0 || bus >= NBUS) return 0;
	std::string path = FindSound (file);
	if (path.empty ()) { LOGOUT ("Audio: no sound file %s", file.c_str ()); return 0; }
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
}

void System::EndSession ()
{
	callouts.Reset ();
	xr::Clear ();
	for (auto &p : played) if (p.second.flat) ma_sound_uninit (p.second.flat.get ());
	played.clear ();
	vessels.Clear ();
	effects.Clear ();
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
	// (a render is mixed at full volume, whatever the player's settings)
	if (Rendering ()) {
		engine.SetMasterGain (1.0f);
		for (int b = 0; b < NBUS; b++) engine.SetBusGain ((Bus)b, 1.0f);
		return;
	}
	engine.SetMasterGain (s.bEnabled ? VolumeToGain (s.Master) : 0.0f);
	for (int b = 0; b < NBUS; b++) engine.SetBusGain ((Bus)b, VolumeToGain (s.Bus[b]));
}

void System::SettingsChanged ()
{
	const CFG_SOUNDPRM &s = orbiter->Cfg()->CfgSoundPrm;
	if (!engine.Active () && s.bEnabled && client && !forcedOff) {
		Engine::Options opt;
		opt.mode = Engine::DEVICE;
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
		world.Update (t);
		// the voice, XRSound's sounds (not in the replay: it isn't flown)
		const bool cockpit = g_camera && g_camera->IsInternal ();
		if (!replay) {
			if (CalloutsOn ()) callouts.Step (g_focusobj, td.SimT1, td.SimDT, td.Warp ());
			else callouts.Reset ();
			xr::Step (*this, g_focusobj, cockpit, td.SimT1);
		}
		UpdatePlayed ();
		Duck (td.UiDT ());
		static const bool log = getenv ("ORBITER_AUDIO_LOG") != nullptr;
		if (log && (logT < 0.0 || t - logT >= 0.5 || t < logT)) {
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
