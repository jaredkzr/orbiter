// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// The audio engine, offline: buses, volumes, the simulation fade, and the
// WAV files renders are written to.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "Audio/AudioEngine.h"
#include "Audio/WavWriter.h"
#include "Audio/Ears.h"
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

Engine::Options Offline (bool ears = false)
{
	Engine::Options o;
	o.mode = Engine::OFFLINE;
	o.ears = ears; // (the gains exactly: no compression)
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

TEST_CASE ("Ears: quiet as it is, loud compressed, peaks held", "[audio]")
{
	Ears::Settings set;
	CHECK (Ears::GainDb (-40.0f, set) == 0.0f);
	CHECK (Ears::GainDb (-25.0f, set) == 0.0f);
	CHECK_THAT (Ears::GainDb (-5.0f, set), WithinAbs (-10.0f, 1e-4f)); // (15 dB over: 5 dB over)
	// in the mix: a tone at +20 dBFS comes out held below full scale, a
	// quiet one as it is
	for (double amp : {10.0, 0.005}) {
		Engine e;
		REQUIRE (e.Init (Offline (true)));
		Tone t (e, EXTERIOR, 220.0, amp);
		RenderRms (e, 2.0); // (the ears settle)
		double rms = RenderRms (e, 0.5);
		uint64_t n = 4800;
		std::vector<float> buf (n * 2);
		e.Render (buf.data (), n);
		float peak = 0.0f;
		for (float v : buf) peak = std::max (peak, std::fabs (v));
		if (amp > 1.0) {
			CHECK (peak <= 0.9f);
			CHECK (rms > 0.05);
			CHECK (e.EarsReduction () < -20.0f);
		} else {
			CHECK_THAT (rms, WithinRel (amp / std::sqrt (2.0) / std::sqrt (2.0) * std::sqrt (2.0), 0.6));
			CHECK (e.EarsReduction () > -0.5f);
		}
	}
	// music is not heard through the ears
	Engine e;
	REQUIRE (e.Init (Offline (true)));
	Tone m (e, MUSIC, 220.0, 0.5);
	RenderRms (e, 1.0);
	CHECK (e.EarsReduction () == 0.0f);
}

TEST_CASE ("Ears: the limiter holds peaks without distorting", "[audio]")
{
	// a 220 Hz tone 6 dB over full scale: out below the ceiling, its
	// harmonics (from clipping) at least 40 dB down
	Engine e;
	REQUIRE (e.Init (Offline (true)));
	Tone t (e, EXTERIOR, 220.0, 2.0);
	RenderRms (e, 2.0);
	const uint64_t n = 48000;
	std::vector<float> buf (n * 2);
	e.Render (buf.data (), n);
	float peak = 0.0f;
	for (float v : buf) peak = std::max (peak, std::fabs (v));
	CHECK (peak <= 0.9f);
	auto amp = [&](double f) {
		double re = 0.0, im = 0.0;
		for (uint64_t i = 0; i < n; i++) re += buf[2*i] * std::cos (2.0 * M_PI * f * i / 48000.0), im += buf[2*i] * std::sin (2.0 * M_PI * f * i / 48000.0);
		return std::sqrt (re * re + im * im);
	};
	double a1 = amp (220.0), a3 = amp (660.0), a2 = amp (440.0);
	INFO ("harmonics: 2nd " << 20.0 * std::log10 (a2 / a1) << " dB, 3rd " << 20.0 * std::log10 (a3 / a1) << " dB");
	CHECK (a3 < a1 * 0.01);
	CHECK (a2 < a1 * 0.01);
}

TEST_CASE ("Ears: the listening level raises a quiet scene, ducks for a loud one, recovers slowly", "[audio]")
{
	Engine::Options o = Offline (true);
	o.adapt = true;
	Engine e;
	REQUIRE (e.Init (o));
	// a quiet scene: a cabin's 56 dB, -74 dBFS - raised the most (45 dB)
	{
		Tone quiet (e, EXTERIOR, 220.0, 2.8e-4);
		RenderRms (e, 4.0);
		const double rms = RenderRms (e, 1.0);
		INFO ("quiet: " << 20.0 * std::log10 (rms) << " dBFS out, gain " << e.EarsAdapt () << " dB");
		CHECK (e.EarsAdapt () > 40.0f);
		CHECK (20.0 * std::log10 (rms) > -40.0);
		// something loud starts (-10 dBFS before the gain): down within a
		// fraction of a second
		{
			Tone loud (e, EXTERIOR, 110.0, 0.3);
			RenderRms (e, 0.3);
			INFO ("loud: gain " << e.EarsAdapt () << " dB");
			CHECK (e.EarsAdapt () < 10.0f);
			RenderRms (e, 2.0);
		}
		// quiet again: recovering over seconds, not at once
		RenderRms (e, 1.0);
		const float g1 = e.EarsAdapt ();
		RenderRms (e, 20.0);
		const float g2 = e.EarsAdapt ();
		INFO ("recovering: " << g1 << " dB after 1 s, " << g2 << " dB after 21 s");
		CHECK (g1 < 30.0f);
		CHECK (g2 > 40.0f);
	}
}

TEST_CASE ("Ears: the dynamic range setting's levels", "[audio]")
{
	const Ears::Settings off = Ears::ForCompression (Ears::COMP_OFF), light = Ears::ForCompression (Ears::COMP_LIGHT);
	const Ears::Settings med = Ears::ForCompression (Ears::COMP_MEDIUM), strong = Ears::ForCompression (Ears::COMP_STRONG);
	// off: no compression (the limiter alone), no listening level
	CHECK (Ears::GainDb (-5.0f, off) == 0.0f);
	CHECK (off.adaptMax == 0.0f);
	// medium: the defaults
	CHECK (Ears::GainDb (-5.0f, med) == Ears::GainDb (-5.0f, Ears::Settings ()));
	// harder as it goes: a loud scene brought down more, a quiet one raised more
	CHECK (Ears::GainDb (-5.0f, light) > Ears::GainDb (-5.0f, med));
	CHECK (Ears::GainDb (-5.0f, strong) < Ears::GainDb (-5.0f, med));
	CHECK (light.adaptMax < med.adaptMax);
	CHECK (strong.adaptMax > med.adaptMax);
	// live: set on a running engine
	Engine::Options o = Offline (true);
	o.adapt = true;
	Engine e;
	REQUIRE (e.Init (o));
	e.SetCompression (Ears::COMP_OFF);
	Tone loud (e, EXTERIOR, 220.0, 0.3);
	RenderRms (e, 1.0);
	INFO ("off: reduction " << e.EarsReduction () << " dB, listening level " << e.EarsAdapt () << " dB");
	CHECK (e.EarsReduction () > -0.5f);
	CHECK (e.EarsAdapt () < 0.5f);
}
