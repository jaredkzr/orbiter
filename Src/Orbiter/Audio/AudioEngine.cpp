// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "AudioEngine.h"
#include "miniaudio.h"
#include "Ears.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace audio {

namespace {
	const char *BUS_NAMES[NBUS] = { "exterior", "interior", "voice", "radio", "music", "ui" };
}

const char *BusName (Bus b)
{
	return b >= 0 && b < NBUS ? BUS_NAMES[b] : "";
}

float VolumeToGain (float v)
{
	if (v <= 0.0f) return 0.0f;
	if (v >= 1.0f) return 1.0f;
	return std::pow (10.0f, 2.0f * (v - 1.0f)); // -40 dB .. 0 dB
}

struct Engine::Impl {
	ma_sound_group group[NBUS];
	bool groupInit[NBUS] = {};
	Ears ears;
};

Engine::Engine () = default;

Engine::~Engine ()
{
	Shutdown ();
}

bool Engine::Init (const Options &opt)
{
	Shutdown ();
	if (opt.mode == OFF) return false;

	engine = std::make_unique<ma_engine>();
	impl = std::make_unique<Impl>();
	ma_engine_config cfg = ma_engine_config_init ();
	cfg.channels = opt.channels;
	cfg.sampleRate = opt.sampleRate;
	if (opt.mode == OFFLINE) {
		cfg.noDevice = MA_TRUE; // (mixed by Render)
	}
	ma_result r = ma_engine_init (&cfg, engine.get());
	if (r != MA_SUCCESS) {
		fprintf (stderr, "Audio: no output (%s)\n", ma_result_description (r));
		engine.reset ();
		impl.reset ();
		return false;
	}
	mode = opt.mode;
	channels = ma_engine_get_channels (engine.get());
	sampleRate = ma_engine_get_sample_rate (engine.get());
	deviceName.clear ();
	if (mode == DEVICE) {
		if (ma_device *dev = ma_engine_get_device (engine.get())) {
			char name[256] = "";
			if (ma_device_get_name (dev, ma_device_type_playback, name, sizeof(name), NULL) == MA_SUCCESS)
				deviceName = name;
		}
	}

	for (int b = 0; b < NBUS; b++) {
		if (ma_sound_group_init (engine.get(), 0, NULL, &impl->group[b]) == MA_SUCCESS)
			impl->groupInit[b] = true;
	}
	if (opt.ears && impl->ears.Init (engine.get())) {
		for (Bus b : { EXTERIOR, INTERIOR })
			if (impl->groupInit[b]) ma_node_attach_output_bus (&impl->group[b], 0, impl->ears.Node (), 0);
	}
	simFade = 1.0f;
	duck = 1.0f;
	return true;
}

void Engine::Shutdown ()
{
	if (!engine) return;
	for (int b = 0; b < NBUS; b++)
		if (impl->groupInit[b]) ma_sound_group_uninit (&impl->group[b]), impl->groupInit[b] = false;
	impl->ears.Uninit ();
	ma_engine_uninit (engine.get());
	engine.reset ();
	impl.reset ();
	mode = OFF;
}

ma_sound_group *Engine::Group (Bus b)
{
	if (!engine || b < 0 || b >= NBUS || !impl->groupInit[b]) return nullptr;
	return &impl->group[b];
}

void Engine::SetMasterGain (float g)
{
	if (engine) ma_engine_set_volume (engine.get(), std::max (0.0f, g));
}

void Engine::SetBusGain (Bus b, float g)
{
	if (b < 0 || b >= NBUS) return;
	busGain[b] = std::max (0.0f, g);
	if (ma_sound_group *grp = Group (b)) ma_sound_group_set_volume (grp, busGain[b] * (Ducked (b) ? duck : 1.0f));
}

void Engine::SetDuck (float g)
{
	g = std::clamp (g, 0.0f, 1.0f);
	if (g == duck) return;
	duck = g;
	for (int b = 0; b < NBUS; b++)
		if (Ducked ((Bus)b)) SetBusGain ((Bus)b, busGain[b]);
}

void Engine::SetSimFade (float target, double seconds)
{
	target = std::clamp (target, 0.0f, 1.0f);
	if (!engine || target == simFade) return;
	simFade = target;
	ma_uint64 ms = (ma_uint64)std::max (0.0, seconds * 1000.0);
	for (int b = 0; b < NBUS; b++) {
		if (!SimBus ((Bus)b)) continue;
		// (from the current level: a fade reversed half way doesn't jump)
		if (ma_sound_group *grp = Group ((Bus)b)) ma_sound_group_set_fade_in_milliseconds (grp, -1.0f, target, ms);
	}
}

uint64_t Engine::Render (float *out, uint64_t frames)
{
	if (mode != OFFLINE || !frames) return 0;
	ma_uint64 done = 0;
	ma_engine_read_pcm_frames (engine.get(), out, frames, &done);
	// (the engine fills what it doesn't mix with silence, but be sure)
	if (done < frames)
		std::fill (out + done * channels, out + frames * channels, 0.0f);
	return frames;
}

float Engine::EarsReduction () const
{
	return impl ? impl->ears.Reduction () : 0.0f;
}

uint64_t Engine::Time () const
{
	return engine ? ma_engine_get_time_in_pcm_frames (engine.get()) : 0;
}

} // namespace audio
