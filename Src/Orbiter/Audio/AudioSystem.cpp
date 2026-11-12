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
#include "miniaudio.h"
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <cstdlib>

extern TimeData td;
extern PlanetarySystem *g_psys;

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
	static uint32_t seed = 7;
	auto bs = BoomSynth::Make (engine, (float)T, seed++);
	if (!bs) return;
	double len = bs->Length ();
	world.PlayShot (std::move (bs), b.dir, (float)(dp / P_FULL), len);
}

namespace {
	std::string FindSound (const std::string &file)
	{
		std::error_code ec;
		if (std::filesystem::exists (file, ec)) return file;
		std::string s = "Sound/" + file;
		if (std::filesystem::exists (s, ec)) return s;
		return std::string ();
	}
}

int System::Play (const Vessel *v, const std::string &file, const V3 &pos, double level, bool loop)
{
	if (!engine.Active ()) return 0;
	std::string path = FindSound (file);
	if (path.empty ()) { LOGOUT ("Audio: no sound file %s", file.c_str ()); return 0; }
	Played p;
	p.loop = loop, p.level = level;
	if (v) {
		EmitterDesc d;
		d.body = v;
		d.offset = pos;
		d.level = (float)level;
		d.refDist = 2.0f;
		d.hull = 0.5f;
		d.name = file;
		p.em = world.Add (d, MakeFileSource (engine, path, loop));
		if (!p.em) return 0;
	} else {
		p.ui = std::make_unique<ma_sound>();
		ma_uint32 flags = MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_DECODE;
		if (ma_sound_init_from_file (engine.Ma (), path.c_str (), flags, engine.Group (UI), NULL, p.ui.get ()) != MA_SUCCESS) return 0;
		ma_sound_set_looping (p.ui.get (), loop ? MA_TRUE : MA_FALSE);
		ma_sound_set_volume (p.ui.get (), (float)level);
		ma_sound_start (p.ui.get ());
	}
	int h = nextHandle++;
	played[h] = std::move (p);
	return h;
}

bool System::StopSound (int h)
{
	auto it = played.find (h);
	if (it == played.end ()) return false;
	if (it->second.em) world.Remove (it->second.em);
	if (it->second.ui) ma_sound_uninit (it->second.ui.get ());
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
		if (p.em) p.em->SetLevel ((float)level);
		if (p.ui) ma_sound_set_volume (p.ui.get (), (float)level);
	}
	if (pitch > 0.0) {
		if (p.em) p.em->SetPitch ((float)pitch);
		if (p.ui) ma_sound_set_pitch (p.ui.get (), (float)pitch);
	}
	return true;
}

bool System::Playing (int h) const
{
	return played.count (h) > 0;
}

void System::UpdatePlayed ()
{
	// (sounds not looped go when they have played)
	for (auto it = played.begin (); it != played.end (); ) {
		Played &p = it->second;
		bool done = !p.loop && ((p.em && p.em->SourceDone ()) || (p.ui && ma_sound_at_end (p.ui.get ())));
		if (done) {
			if (p.em) world.Remove (p.em);
			if (p.ui) ma_sound_uninit (p.ui.get ());
			it = played.erase (it);
		} else ++it;
	}
}

void System::EndSession ()
{
	for (auto &p : played) if (p.second.ui) ma_sound_uninit (p.second.ui.get ());
	played.clear ();
	vessels.Clear ();
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
	world.Forget (body);
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
	for (auto &p : played) if (p.second.ui) ma_sound_uninit (p.second.ui.get ());
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
		world.Update (t);
		UpdatePlayed ();
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
