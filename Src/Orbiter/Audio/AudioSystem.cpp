// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "AudioSystem.h"
#include "Orbiter.h"
#include "Config.h"
#include "TimeData.h"
#include "Log.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

extern TimeData td;

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
}

void System::Stop ()
{
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

	// pause and time acceleration: quick out, a little slower back in
	float fade = SimFadeFor (orbiter->IsRunning (), td.Warp ());
	engine.SetSimFade (fade, fade < engine.SimFadeTarget () ? 0.15 : 0.4);

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
