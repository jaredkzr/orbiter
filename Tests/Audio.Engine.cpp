// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// The audio engine, offline: buses, volumes, the simulation fade, and the
// WAV files renders are written to.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "Audio/AudioEngine.h"
#include "Audio/WavWriter.h"
#include "miniaudio.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

using namespace audio;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

/// A sine on a bus (not spatialized)
struct Tone {
	ma_waveform wave;
	ma_sound sound;
	Tone (Engine &e, Bus b, double freq, double amp) {
		ma_waveform_config c = ma_waveform_config_init (ma_format_f32, 1, e.SampleRate (), ma_waveform_type_sine, amp, freq);
		REQUIRE (ma_waveform_init (&c, &wave) == MA_SUCCESS);
		REQUIRE (ma_sound_init_from_data_source (e.Ma (), &wave, MA_SOUND_FLAG_NO_SPATIALIZATION, e.Group (b), &sound) == MA_SUCCESS);
		ma_sound_start (&sound);
	}
	~Tone () { ma_sound_uninit (&sound); ma_waveform_uninit (&wave); }
};

/// RMS of the left channel over `seconds` of the mix
double RenderRms (Engine &e, double seconds)
{
	uint64_t n = (uint64_t)(seconds * e.SampleRate ());
	std::vector<float> buf (n * e.Channels ());
	REQUIRE (e.Render (buf.data (), n) == n);
	double s = 0.0;
	for (uint64_t i = 0; i < n; i++) s += (double)buf[i * e.Channels ()] * buf[i * e.Channels ()];
	return std::sqrt (s / n);
}

Engine::Options Offline ()
{
	Engine::Options o;
	o.mode = Engine::OFFLINE;
	return o;
}

} // namespace

TEST_CASE ("Offline engine starts", "[audio]")
{
	Engine e;
	REQUIRE (e.Init (Offline ()));
	CHECK (e.GetMode () == Engine::OFFLINE);
	CHECK (e.SampleRate () == 48000);
	CHECK (e.Channels () == 2);
	for (int b = 0; b < NBUS; b++) CHECK (e.Group ((Bus)b) != nullptr);
}

TEST_CASE ("Silence without sources, clock advances", "[audio]")
{
	Engine e;
	REQUIRE (e.Init (Offline ()));
	CHECK (RenderRms (e, 0.5) == 0.0);
	CHECK (e.Time () == 24000);
}

TEST_CASE ("Bus and master gains scale the mix", "[audio]")
{
	Engine e;
	REQUIRE (e.Init (Offline ()));
	Tone t (e, EXTERIOR, 440.0, 0.25);
	RenderRms (e, 0.05); // (settle)
	double ref = RenderRms (e, 0.5);
	CHECK (ref > 0.1);
	e.SetBusGain (EXTERIOR, 0.5f);
	RenderRms (e, 0.05);
	CHECK_THAT (RenderRms (e, 0.5), WithinRel (ref * 0.5, 0.02));
	e.SetMasterGain (0.5f);
	RenderRms (e, 0.05);
	CHECK_THAT (RenderRms (e, 0.5), WithinRel (ref * 0.25, 0.02));
	e.SetBusGain (EXTERIOR, 0.0f);
	RenderRms (e, 0.05);
	CHECK (RenderRms (e, 0.5) < 1e-6);
}

TEST_CASE ("Simulation fade: simulation buses only", "[audio]")
{
	Engine e;
	REQUIRE (e.Init (Offline ()));
	Tone sim (e, EXTERIOR, 440.0, 0.25);
	RenderRms (e, 0.1);
	double ref = RenderRms (e, 0.5);

	// paused: the outside fades out over 0.15 s
	e.SetSimFade (0.0f, 0.15);
	RenderRms (e, 0.2);
	CHECK (RenderRms (e, 0.5) < 1e-4);

	// music plays on
	Tone music (e, MUSIC, 330.0, 0.25);
	RenderRms (e, 0.05);
	CHECK_THAT (RenderRms (e, 0.5), WithinRel (ref, 0.05));
}

TEST_CASE ("Simulation fade back in", "[audio]")
{
	Engine e;
	REQUIRE (e.Init (Offline ()));
	Tone sim (e, VOICE, 440.0, 0.25);
	RenderRms (e, 0.1);
	double ref = RenderRms (e, 0.5);
	e.SetSimFade (0.0f, 0.1);
	RenderRms (e, 0.3);
	e.SetSimFade (1.0f, 0.4);
	double mid = RenderRms (e, 0.1);
	CHECK (mid > 0.0);
	CHECK (mid < ref);
	RenderRms (e, 0.4);
	CHECK_THAT (RenderRms (e, 0.5), WithinRel (ref, 0.02));
}

TEST_CASE ("Volume curve", "[audio]")
{
	CHECK (VolumeToGain (0.0f) == 0.0f);
	CHECK (VolumeToGain (1.0f) == 1.0f);
	CHECK_THAT (VolumeToGain (0.5f), WithinRel (0.1f, 1e-4f)); // -20 dB
	CHECK (VolumeToGain (0.3f) < VolumeToGain (0.31f));
}

TEST_CASE ("WAV file: header and samples", "[audio]")
{
	auto path = std::filesystem::temp_directory_path () / "orbiter_audio_test.wav";
	{
		WavWriter w;
		REQUIRE (w.Open (path.string (), 48000, 2));
		std::vector<float> d (2 * 1000);
		for (size_t i = 0; i < d.size (); i++) d[i] = (float)i / d.size ();
		w.Write (d.data (), 600);
		w.Flush ();
		w.Write (d.data () + 1200, 400);
		CHECK (w.Frames () == 1000);
	}
	FILE *f = fopen (path.string ().c_str (), "rb");
	REQUIRE (f);
	std::vector<unsigned char> b (std::filesystem::file_size (path));
	REQUIRE (fread (b.data (), 1, b.size (), f) == b.size ());
	fclose (f);
	auto u16 = [&](size_t o) { return (unsigned)b[o] | (unsigned)b[o+1] << 8; };
	auto u32 = [&](size_t o) { return (uint32_t)b[o] | (uint32_t)b[o+1] << 8 | (uint32_t)b[o+2] << 16 | (uint32_t)b[o+3] << 24; };
	CHECK (memcmp (b.data (), "RIFF", 4) == 0);
	CHECK (u32 (4) == b.size () - 8);
	CHECK (memcmp (b.data () + 8, "WAVE", 4) == 0);
	CHECK (u16 (20) == 3);      // float
	CHECK (u16 (22) == 2);
	CHECK (u32 (24) == 48000);
	CHECK (u16 (34) == 32);
	CHECK (memcmp (b.data () + 50, "data", 4) == 0);
	CHECK (u32 (54) == 1000 * 2 * 4);
	float last;
	memcpy (&last, b.data () + b.size () - 4, 4);
	CHECK (last == (float)1999 / 2000);
	std::filesystem::remove (path);
}
