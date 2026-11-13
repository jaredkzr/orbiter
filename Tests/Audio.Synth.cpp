// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// The synthesizers, offline: their level at full (the calibration
// VesselSounds counts on), how their sound follows their parameters, the
// strength of sonic booms and rocket noise.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "Audio/AudioEngine.h"
#include "Audio/PathNode.h"
#include "Audio/RocketSynth.h"
#include "Audio/RcsSynth.h"
#include "Audio/WindSynth.h"
#include "Audio/BoomSynth.h"
#include "Audio/ContactSynth.h"
#include "Audio/BlastSynth.h"
#include "Audio/FireSynth.h"
#include "Audio/SplashSynth.h"
#include "Audio/ThunderSynth.h"
#include "Audio/CreakSynth.h"
#include "Audio/AmbienceSynth.h"
#include "Audio/MusicSynth.h"
#include "Audio/Hrtf.h"
#include "Audio/Loudness.h"
#include "Audio/WavWriter.h"
#include "Audio/VesselSounds.h"
#include "miniaudio.h"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

using namespace audio;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

/// A synth heard at unit gain, centred (equal-power: each channel 0.707)
struct Rig {
	Engine e;
	PathNode node;
	Rig () {
		Engine::Options o;
		o.mode = Engine::OFFLINE;
		o.ears = false;
		REQUIRE (e.Init (o));
		REQUIRE (node.Init (e.Ma (), 0.0f));
		ma_node_attach_output_bus (node.Node (), PathNode::AIR, e.Group (EXTERIOR), 0);
		PathNode::Target t;
		t.gain = 1.0f;
		node.Set (PathNode::AIR, t);
	}
	void Attach (Source &s) { REQUIRE (s.Attach (node.Node ())); s.Start (); }
	/// the synth's own signal (left channel / 0.707) over `seconds`
	std::vector<float> Take (double seconds) {
		uint64_t n = (uint64_t)(seconds * e.SampleRate ());
		std::vector<float> buf (n * 2), mono (n);
		e.Render (buf.data (), n);
		for (uint64_t i = 0; i < n; i++) mono[i] = buf[2*i] / 0.70710678f;
		return mono;
	}
};

double Rms (const std::vector<float> &x, size_t i0 = 0, size_t i1 = 0)
{
	if (!i1) i1 = x.size ();
	double s = 0.0;
	for (size_t i = i0; i < i1; i++) s += (double)x[i] * x[i];
	return std::sqrt (s / std::max<size_t> (1, i1 - i0));
}

/// Spectral centroid [Hz] of the power spectrum (a plain DFT of the last
/// 4096 samples, Hann window; enough for a test)
double Centroid (const std::vector<float> &x, double rate)
{
	const size_t N = 4096;
	size_t start = x.size () > N ? x.size () - N : 0;
	std::vector<double> v (N, 0.0);
	for (size_t i = 0; i < N && start + i < x.size (); i++)
		v[i] = x[start + i] * (0.5 - 0.5 * std::cos (2.0 * M_PI * i / N));
	double num = 0.0, den = 0.0;
	for (size_t k = 1; k < N / 2; k++) {
		double re = 0.0, im = 0.0, w = 2.0 * M_PI * k / N;
		for (size_t i = 0; i < N; i++) re += v[i] * std::cos (w * i), im -= v[i] * std::sin (w * i);
		double pw = re * re + im * im;
		num += pw * k * rate / N, den += pw;
	}
	return den > 0.0 ? num / den : 0.0;
}

double Db (double x) { return 20.0 * std::log10 (x); }
double Peak (const std::vector<float> &x) { double m = 0.0; for (float v : x) m = std::max (m, (double)std::fabs (v)); return m; }

} // namespace

TEST_CASE ("Rocket: level at full, silent off, ignition", "[audio][synth]")
{
	Rig r;
	auto s = RocketSynth::Make (r.e, 1);
	REQUIRE (s);
	RocketSynth::Params p;
	p.level = 0.0f;
	p.peak = RocketSynth::PeakFor (2e5);
	s->Set (p);
	r.Attach (*s);
	CHECK (Rms (r.Take (0.3)) < 1e-6);
	p.level = 1.0f;
	s->Set (p);
	auto ign = r.Take (0.25);
	auto run = r.Take (2.0);
	double rms = Rms (run);
	INFO ("rocket RMS at full " << rms);
	CHECK_THAT (Db (rms), WithinAbs (Db (0.3), 3.0));   // (VesselSounds: ROCKET_RMS)
	// the ignition: louder than the run, at first
	CHECK (Rms (ign, 0, 4800) > Rms (run));
	// off: silent after the chuff, and the pumps wound down
	p.level = 0.0f;
	s->Set (p);
	r.Take (8.0);
	CHECK (Rms (r.Take (0.5)) < 1e-3);
}

TEST_CASE ("Rocket: bigger engines lower", "[audio][synth]")
{
	CHECK (RocketSynth::PeakFor (3.5e7) < 25.0f);
	CHECK (RocketSynth::PeakFor (1e5) > 150.0f);
	CHECK (RocketSynth::PeakFor (1e5) < 300.0f); // (a deep roar)
	double c[2];
	double F[2] = {3.5e7, 2e5};
	for (int k = 0; k < 2; k++) {
		Rig r;
		auto s = RocketSynth::Make (r.e, 2);
		RocketSynth::Params p;
		p.level = 1.0f, p.peak = RocketSynth::PeakFor (F[k]), p.crackle = 0.0f;
		s->Set (p);
		r.Attach (*s);
		c[k] = Centroid (r.Take (1.0), r.e.SampleRate ());
	}
	INFO ("centroids " << c[0] << " " << c[1]);
	CHECK (c[0] < c[1] * 0.5);
}

TEST_CASE ("Rocket: Doppler shifts it up", "[audio][synth]")
{
	double c[2];
	float dop[2] = {1.0f, 2.0f};
	for (int k = 0; k < 2; k++) {
		Rig r;
		auto s = RocketSynth::Make (r.e, 3);
		RocketSynth::Params p;
		p.level = 1.0f, p.peak = 300.0f, p.crackle = 0.0f;
		s->Set (p);
		s->SetDoppler (dop[k]);
		r.Attach (*s);
		c[k] = Centroid (r.Take (1.0), r.e.SampleRate ());
	}
	CHECK (c[1] > 1.5 * c[0]);
}

TEST_CASE ("RCS: level when firing, a bang at the start", "[audio][synth]")
{
	Rig r;
	auto s = RcsSynth::Make (r.e, 4);
	s->Set (0.0f, 800.0f);
	r.Attach (*s);
	CHECK (Rms (r.Take (0.2)) < 1e-6);
	s->Set (1.0f, 800.0f);
	auto on = r.Take (0.05);
	auto run = r.Take (1.0);
	INFO ("rcs RMS firing " << Rms (run));
	CHECK_THAT (Db (Rms (run)), WithinAbs (Db (0.3), 4.0));   // (RCS_RMS)
	CHECK (Rms (on) > 1.5 * Rms (run));
}

namespace {
	/// RMS of x in a band (a 4th-order Butterworth made of two biquads)
	double BandRms (const std::vector<float> &x, float lo, float hi, float rate)
	{
		dsp::Biquad a, b, c, d;
		if (lo > 0) a.HighPass (lo, 0.707f, rate), b.HighPass (lo, 0.707f, rate);
		if (hi > 0) c.LowPass (hi, 0.707f, rate), d.LowPass (hi, 0.707f, rate);
		double s = 0.0;
		for (float v : x) {
			float y = v;
			if (lo > 0) y = b.Process (a.Process (y));
			if (hi > 0) y = d.Process (c.Process (y));
			s += (double)y * y;
		}
		return std::sqrt (s / std::max<size_t> (1, x.size ()));
	}
}

TEST_CASE ("RCS: the bang is deep and punchy, not a click", "[audio][synth]")
{
	Rig r;
	auto s = RcsSynth::Make (r.e, 9);
	s->Set (0.0f, 900.0f);
	r.Attach (*s);
	r.Take (0.3);
	// a short pulse (50 ms)
	s->Set (1.0f, 900.0f);
	auto on = r.Take (0.05);
	s->Set (0.0f, 900.0f);
	auto after = r.Take (0.35);
	on.insert (on.end (), after.begin (), after.begin () + 2400); // (the first 100 ms)
	std::vector<float> tail (after.begin () + 7200, after.begin () + 14400); // (150-300 ms after the start... of the release)
	double low = BandRms (on, 0, 120, 48000), high = BandRms (on, 500, 0, 48000);
	INFO ("onset: below 120 Hz " << Db (low) << " dB, above 500 Hz " << Db (high) << " dB; tail " << Db (Rms (tail) / Rms (on)) << " dB");
	CHECK (low > 3.0 * high);
	// punchy: over within a few tenths of a second
	CHECK (Rms (tail) < 0.1 * Rms (on));
}

TEST_CASE ("Rocket: a bass, and the turbopumps' whine", "[audio][synth]")
{
	Rig r;
	auto s = RocketSynth::Make (r.e, 11);
	RocketSynth::Params p;
	p.level = 1.0f, p.peak = RocketSynth::PeakFor (1.6e5), p.crackle = 0.3f;
	p.rumble = RocketSynth::RumbleFor (1.6e5), p.whine = 0.15f, p.pump = RocketSynth::PumpFor (1.6e5);
	s->Set (p);
	r.Attach (*s);
	r.Take (2.0); // (the pumps spool up)
	auto x = r.Take (2.0);
	// the bass: below 100 Hz, a good part of it
	double bass = BandRms (x, 25, 100, 48000), all = Rms (x);
	INFO ("below 100 Hz " << Db (bass / all) << " dB of the whole");
	CHECK (bass > 0.4 * all);
	// the whine: a tone standing out of the roar around the pumps' frequency,
	// but faint (well below the whole)
	double f = p.pump;
	// (power in 1 Hz bins over 2 s: within +-10 Hz of the whine, against as
	// wide a stretch of its surroundings, 15-25 % above it)
	auto bin = [&](double fq) {
		double re = 0.0, im = 0.0, w = 2.0 * M_PI * fq / 48000.0;
		for (size_t i = 0; i < x.size (); i++) re += x[i] * std::cos (w * i), im += x[i] * std::sin (w * i);
		return (re * re + im * im) / ((double)x.size () * x.size ());
	};
	double tone = 0.0, side = 0.0;
	for (int k = -10; k <= 10; k++) tone += bin (f + k);
	for (int k = 0; k < 21; k++) side += bin (f * 1.15 + k * (0.1 * f / 20.0));
	INFO ("whine at " << f << " Hz: " << 10.0 * std::log10 (tone / side) << " dB over its surroundings, " << 10.0 * std::log10 (2.0 * tone) - Db (all) << " dB of the whole");
	CHECK (tone > 10.0 * side);                       // (stands out: 10 dB)
	CHECK (std::sqrt (2.0 * tone) < 0.2 * all);       // (but faint: 14 dB under the whole)
	// after shutdown the pumps wind down: the whine lingers, lower
	p.level = 0.0f;
	s->Set (p);
	r.Take (0.6);
	auto down = r.Take (0.3);
	double lo = BandRms (down, (float)(f * 0.3), (float)(f * 0.85), 48000), at = BandRms (down, (float)(f * 0.97), (float)(f * 1.03), 48000);
	INFO ("winding down: below its full speed " << Db (lo) << " dB, at it " << Db (at) << " dB");
	CHECK (lo > 2.0 * at);
	CHECK (lo > 1e-4);
}

TEST_CASE ("Rocket: crackle without spikes", "[audio][synth]")
{
	// crackle adds texture, not spikes only a limiter can catch: the peaks
	// against the RMS (the crest) at most 2 dB above the roar's own
	double crest[2];
	float mix[2] = {0.0f, 0.3f};
	for (int k = 0; k < 2; k++) {
		Rig r;
		auto s = RocketSynth::Make (r.e, 10);
		RocketSynth::Params p;
		p.level = 1.0f, p.peak = RocketSynth::PeakFor (1.6e5), p.crackle = mix[k];
		s->Set (p);
		r.Attach (*s);
		r.Take (0.5);
		auto x = r.Take (5.0);
		float peak = 0.0f;
		for (float v : x) peak = std::max (peak, std::fabs (v));
		crest[k] = Db (peak / Rms (x));
	}
	INFO ("crest without crackle " << crest[0] << " dB, with " << crest[1] << " dB");
	CHECK (crest[1] - crest[0] < 2.0);
}

TEST_CASE ("Wind: level, brighter faster", "[audio][synth]")
{
	double c[2], rms = 0.0;
	float u[2] = {80.0f, 600.0f};
	for (int k = 0; k < 2; k++) {
		Rig r;
		auto s = WindSynth::Make (r.e, 5);
		WindSynth::Params p;
		p.speed = u[k];
		s->Set (p);
		r.Attach (*s);
		auto x = r.Take (1.5);
		c[k] = Centroid (x, r.e.SampleRate ());
		if (k == 1) rms = Rms (x);
	}
	INFO ("wind RMS " << rms << ", centroids " << c[0] << " " << c[1]);
	CHECK_THAT (Db (rms), WithinAbs (Db (0.3), 4.0));   // (WIND_RMS)
	CHECK (c[1] > 1.5 * c[0]);
	// buffeting: at high angles of attack and around Mach 1
	CHECK (WindSynth::BuffetFor (0.05, 0.5) < 0.05f);
	CHECK (WindSynth::BuffetFor (0.5, 0.5) > 0.9f);
	CHECK (WindSynth::BuffetFor (0.05, 1.0) > 0.6f);
}

TEST_CASE ("Sonic boom: strength and length", "[audio][synth]")
{
	// a fighter-sized body (18 m) at Mach 2, 1 km up: hundreds of pascals
	// (a fighter low and supersonic: 15-20 psf), tens of milliseconds
	double dp, T;
	BoomSynth::Strength (18.0, 2.0, 1000.0, 1e5, 1e5, 340.0, dp, T);
	INFO ("dp " << dp << " Pa, T " << T << " s");
	CHECK (dp > 500.0);
	CHECK (dp < 1200.0);
	CHECK (T > 0.04);
	CHECK (T < 0.15);
	// Concorde (62 m) at Mach 2, 17 km (p 9 kPa there): about 93 Pa (1.94
	// psf) and 0.3 s measured
	BoomSynth::Strength (62.0, 2.0, 17000.0, 9000.0, 1e5, 295.0, dp, T);
	INFO ("Concorde dp " << dp << " Pa, T " << T << " s");
	CHECK (dp > 60.0);
	CHECK (dp < 130.0);
	CHECK (T > 0.25);
	CHECK (T < 0.45);

	// the N-wave: a jump up at once, through zero half way, a jump back
	Rig r;
	auto s = BoomSynth::Make (r.e, 0.2f, 6);
	r.Attach (*s);
	auto x = r.Take (0.5);
	float peak = 0.0f;
	for (size_t i = 0; i < 960; i++) peak = std::max (peak, x[i]);
	CHECK (peak > 0.8f);
	CHECK (x[(size_t)(0.1 * 48000)] < 0.3f);
	CHECK (x[(size_t)(0.15 * 48000)] < -0.2f);
}

TEST_CASE ("Rocket noise: acoustic power", "[audio][synth]")
{
	// Saturn V: 35 MN, 2600 m/s: about 2e8 W, 203-204 dB re 1 pW
	double W = VesselSounds::JetPower (3.5e7, 2600.0);
	CHECK_THAT (10.0 * std::log10 (W / 1e-12), WithinAbs (203.6, 1.0));
	// at 1 km, free field: Lp = Lw - 20 log r - 11 = 132.6 dB SPL
	double p = VesselSounds::PressureAt (W, 1000.0);
	CHECK_THAT (Db (p / 20e-6), WithinAbs (10.0 * std::log10 (W / 1e-12) - 60.0 - 11.0, 0.3));
}

TEST_CASE ("Contacts: rolling, touchdown, squeal, docking", "[audio][synth]")
{
	// (the recordings: Sound/Contacts, under the build root, where this runs)
	{	// rolling at 40 m/s under the full load: the calibration (CONTACT_RMS)
		Rig r;
		auto s = ContactSynth::Make (r.e, 20);
		REQUIRE (s->HasSamples ());
		ContactSynth::Params p;
		p.rollSpeed = 40.0f, p.rollLoad = 1.0f;
		s->Set (p);
		r.Attach (*s);
		r.Take (0.5);
		auto x = r.Take (2.0);
		INFO ("rolling RMS " << Rms (x));
		CHECK_THAT (Db (Rms (x)), WithinAbs (Db (0.18), 2.0)); // (gentle: 4 dB under the calibration)
		// a low rumble: nearly all of it below 200 Hz, no hiss
		INFO ("below 200 Hz " << Db (BandRms (x, 0, 200, 48000) / Rms (x)) << " dB, above 1 kHz " << Db (BandRms (x, 1000, 0, 48000) / Rms (x)) << " dB");
		INFO ("bands 0-60 " << Db (BandRms (x, 0, 60, 48000)) << " 60-120 " << Db (BandRms (x, 60, 120, 48000)) << " 120-250 " << Db (BandRms (x, 120, 250, 48000)));
		INFO ("bands 250-500 " << Db (BandRms (x, 250, 500, 48000)) << " 500-1k " << Db (BandRms (x, 500, 1000, 48000)));
		CHECK (BandRms (x, 0, 200, 48000) > 0.9 * Rms (x));
		CHECK (BandRms (x, 1000, 0, 48000) < 0.03 * Rms (x));
		// textured (impacts), not a steady hum: its 20 ms envelope varies
		std::vector<double> env;
		for (size_t i = 0; i + 960 <= x.size (); i += 960) env.push_back (Rms (x, i, i + 960));
		double mean = 0.0, var = 0.0;
		for (double e : env) mean += e;
		mean /= env.size ();
		for (double e : env) var += (e - mean) * (e - mean);
		double cv = std::sqrt (var / env.size ()) / mean;
		INFO ("envelope variation " << cv);
		CHECK (cv > 0.12);
	}
	{	// a touchdown: the strut's thump (low) and the tyres' chirp (a squeal
		// near 1 kHz, falling), the chirp louder than the thump, both over
		// within a second
		Rig r;
		auto s = ContactSynth::Make (r.e, 21);
		r.Attach (*s);
		r.Take (0.1);
		s->Strike (ContactSynth::TOUCHDOWN, 1.0f);
		auto x = r.Take (1.0);
		std::vector<float> head (x.begin (), x.begin () + 7200), chirp (x.begin () + 2400, x.begin () + 19200), tail (x.begin () + 43200, x.end ());
		INFO ("touchdown: peak " << Db (Peak (x)) << ", head below 120 Hz " << Db (BandRms (head, 0, 120, 48000)) << " dB of " << Db (Rms (head)));
		INFO ("chirp 600-1400 Hz " << Db (BandRms (chirp, 600, 1400, 48000)) << " dB of " << Db (Rms (chirp)) << "; tail " << Db (Rms (tail)));
		CHECK (BandRms (head, 0, 120, 48000) > 0.3 * Rms (head));        // the thump
		CHECK (BandRms (chirp, 600, 1400, 48000) > 0.4 * Rms (chirp));   // the chirp
		CHECK (BandRms (chirp, 600, 1400, 48000) > BandRms (chirp, 0, 120, 48000)); // (clearly audible over the thump)
		CHECK (Rms (tail) < 0.05 * Rms (head));
	}
	{	// a squeal: tonal, near 1.1 kHz (the recording re-pitched), wavering
		Rig r;
		auto s = ContactSynth::Make (r.e, 22);
		ContactSynth::Params p;
		p.squeal = 1.0f;
		s->Set (p);
		r.Attach (*s);
		r.Take (0.3);
		auto x = r.Take (1.0);
		INFO ("squeal: RMS " << Db (Rms (x)) << "; 900-1400 Hz " << Db (BandRms (x, 900, 1400, 48000) / Rms (x)) << " dB of the whole");
		INFO ("1800-2800 " << Db (BandRms (x, 1800, 2800, 48000) / Rms (x)) << ", below 500 " << Db (BandRms (x, 0, 500, 48000) / Rms (x)) << ", above 4k " << Db (BandRms (x, 4000, 0, 48000) / Rms (x)));
		CHECK (BandRms (x, 900, 1400, 48000) > 0.35 * Rms (x)); // (its fundamental)
		CHECK (BandRms (x, 900, 3000, 48000) > 0.6 * Rms (x));  // (and its overtones)
		CHECK (BandRms (x, 0, 500, 48000) < 0.3 * Rms (x));
		CHECK_THAT (Db (Rms (x)), WithinAbs (Db (0.3), 3.0)); // (a full skid: about the calibration)
	}
	{	// docking: the clunk (heavy, dull), then two latches (hard mid clacks)
		// 0.45 and 0.62 s later - three onsets within 0.8 s; nothing ringing
		// like a bell once the structure has settled (1.3 s on)
		Rig r;
		auto s = ContactSynth::Make (r.e, 23);
		r.Attach (*s);
		r.Take (0.1);
		s->Strike (ContactSynth::DOCK, 0.8f);
		auto x = r.Take (1.3); // (the clunk's rattle, the structure settling, lasts over a second)
		auto win = [&](double t0, double t1, double lo, double hi) { std::vector<float> w (x.begin () + (size_t)(t0 * 48000), x.begin () + (size_t)(t1 * 48000)); return BandRms (w, lo, hi, 48000); };
		double clunkLo = win (0.0, 0.1, 0, 200), clunkMid = win (0.0, 0.1, 200, 2000), gap = win (0.35, 0.44, 200, 2000), latch1 = win (0.45, 0.5, 200, 2000), latch2 = win (0.62, 0.67, 200, 2000);
		INFO ("clunk: peak " << Db (Peak (std::vector<float> (x.begin (), x.begin () + 4800))) << ", low " << Db (clunkLo) << " mid " << Db (clunkMid));
		INFO ("gap (mid) " << Db (gap) << ", latches " << Db (latch1) << " " << Db (latch2));
		CHECK (clunkLo > clunkMid);        // dull
		CHECK (latch1 > 2.0 * gap);        // the latches: clacks
		CHECK (latch2 > 2.0 * gap);
		auto late = r.Take (0.5);
		INFO ("ringing after: " << Db (Rms (late)) << " dB vs the clunk " << Db (Rms (std::vector<float> (x.begin (), x.begin () + 4800))));
		CHECK (Rms (late) < 0.02 * Rms (std::vector<float> (x.begin (), x.begin () + 4800)));
	}
}

TEST_CASE ("Blast: Kinney & Graham's overpressure and duration; the Friedlander wave", "[audio][synth]")
{
	// (published values of the fits: Z = 1 m/kg^1/3 about 10 bar; Z = 10 about
	// 0.1 bar; far out, 1/Z; the positive phase about 4.2 ms/kg^1/3 far out)
	CHECK_THAT (BlastSynth::Overpressure (1.0) / 101325.0, WithinRel (9.96, 0.03));
	CHECK_THAT (BlastSynth::Overpressure (10.0) / 101325.0, WithinRel (0.0985, 0.03));
	CHECK_THAT (BlastSynth::Overpressure (300.0) * 300.0, WithinRel (BlastSynth::Overpressure (600.0) * 600.0, 0.02));
	CHECK_THAT (BlastSynth::Duration (100.0, 1.0), WithinRel (4.2e-3, 0.05));
	CHECK_THAT (BlastSynth::Duration (100.0, 1000.0), WithinRel (42e-3, 0.05)); // (times scale with W^1/3)
	CHECK_THAT (BlastSynth::TntEquivalent (1000.0, false), WithinRel (100.0, 1e-9));
	CHECK_THAT (BlastSynth::TntEquivalent (1000.0, true), WithinRel (180.0, 1e-9));
	{	// Sachs: sea level as the fits; in Mars' air (600 Pa, c 240 m/s) far
		// out, (p/P0)^4/3 of it - about 60 dB down - and longer (c0/c (P0/p)^1/3)
		double dP0, td0, dPm, tdm;
		BlastSynth::Sachs (5000.0, 100.0, 101325.0, 340.3, dP0, td0);
		CHECK_THAT (dP0, WithinRel (BlastSynth::Overpressure (5000.0 / std::cbrt (100.0)), 1e-6));
		BlastSynth::Sachs (5000.0, 100.0, 600.0, 240.0, dPm, tdm);
		INFO ("Mars: " << 20.0 * std::log10 (dPm / dP0) << " dB, td x" << tdm / td0);
		CHECK_THAT (20.0 * std::log10 (dPm / dP0), WithinAbs (80.0 / 3.0 * std::log10 (600.0 / 101325.0), 3.0));
		CHECK (tdm > 5.0 * td0);
	}

	Rig r;
	auto s = BlastSynth::Make (r.e, 31);
	s->Set (1000.0, 0.03);
	r.Attach (*s);
	auto quiet = r.Take (0.2);
	CHECK (Rms (quiet) < 1e-6); // (nothing until it arrives)
	s->Fire ();
	auto x = r.Take (6.0);
	// the positive phase: up at once to about 1, falling through zero by td
	// (30 ms); then negative
	float peak = *std::max_element (x.begin (), x.begin () + 4800);
	size_t ipk = std::max_element (x.begin (), x.begin () + 4800) - x.begin ();
	INFO ("peak " << peak << " at " << ipk / 48.0 << " ms");
	CHECK (peak > 0.7f);
	CHECK (ipk < 48 * 3); // (within 3 ms: a shock)
	double neg = 0.0;
	for (size_t i = 48 * 35; i < 48 * 80; i++) neg = std::min (neg, (double)x[i]);
	CHECK (neg < -0.05);
	// then the rumble dying away: the last second far below the blast
	std::vector<float> last (x.end () - 48000, x.end ());
	CHECK (Rms (last) < 0.01 * peak);
}

TEST_CASE ("Fire: the roar's level and spectrum", "[audio][synth]")
{
	CHECK (FireSynth::PeakFor (1e9) < FireSynth::PeakFor (1e6)); // (bigger fires are deeper)
	CHECK_THAT (FireSynth::PeakFor (1e6), WithinRel (400.0, 0.01));
	Rig r;
	auto s = FireSynth::Make (r.e, 32);
	const double Q = 2e9, fpk = FireSynth::PeakFor (Q);
	s->Set (fpk, FireSynth::DiameterFor (Q), 1.0);
	r.Attach (*s);
	r.Take (2.0);
	auto x = r.Take (4.0);
	INFO ("fire RMS " << Rms (x) << ", peak " << fpk << " Hz: below " << 2 * fpk << " Hz " << Db (BandRms (x, 0, (float)(2 * fpk), 48000) / Rms (x)) << " dB, above 1 kHz " << Db (BandRms (x, 1000, 0, 48000) / Rms (x)) << " dB");
	CHECK_THAT (Db (Rms (x)), WithinAbs (Db (FireSynth::FIRE_RMS), 3.0));
	CHECK (BandRms (x, 0, (float)(2 * fpk), 48000) > 0.8 * Rms (x));
	CHECK (BandRms (x, 1000, 0, 48000) < 0.1 * Rms (x));
}

TEST_CASE ("Splash: an entry's slam and bubbles, dying away", "[audio][synth]")
{
	CHECK_THAT (SplashSynth::Minnaert (0.001), WithinRel (3260.0, 1e-6)); // (a 1 mm bubble: 3.3 kHz)
	Rig r;
	auto s = SplashSynth::Make (r.e, 41);
	r.Attach (*s);
	r.Take (0.1);
	s->Entry (1.0f, 4.0f, 9.0f); // (a capsule, 4 m, at 9 m/s)
	auto x = r.Take (6.0);
	std::vector<float> first (x.begin (), x.begin () + 48000), last (x.end () - 48000, x.end ());
	INFO ("first second RMS " << Rms (first) << "; below 100 Hz " << Db (BandRms (first, 0, 100, 48000) / Rms (first)) << " dB, 100-500 Hz " << Db (BandRms (first, 100, 500, 48000) / Rms (first)) << " dB, 500-5000 " << Db (BandRms (first, 500, 5000, 48000) / Rms (first))
		<< " dB, above 8k " << Db (BandRms (first, 8000, 0, 48000) / Rms (first)) << " dB; last second " << Db (Rms (last) / Rms (first)) << " dB");
	CHECK_THAT (Db (Rms (first)), WithinAbs (Db (SplashSynth::SPLASH_RMS), 4.0));
	// bubbles over the whole range: the cavity's low, the spray's high
	CHECK (BandRms (first, 100, 500, 48000) > 0.1 * Rms (first));
	CHECK (BandRms (first, 500, 5000, 48000) > 0.3 * Rms (first));
	CHECK (BandRms (first, 8000, 0, 48000) < 0.3 * Rms (first));
	// over within seconds
	CHECK (Rms (last) < 0.03 * Rms (first));
}

TEST_CASE ("Thunder: Few's channel model", "[audio][synth]")
{
	// Earth: 2e5 J/m at 1 bar - a relaxation radius of about a quarter metre
	CHECK_THAT (ThunderSynth::RelaxationRadius (2e5, 101325.0), WithinRel (0.79, 0.02));
	// a channel 5 km long, from 1 km above the listener, going sideways
	const double start[3] = {-2500.0, 1000.0, 0.0}, dir[3] = {1.0, 0.0, 0.0}, L[3] = {0.0, 0.0, 0.0};
	auto ch = ThunderSynth::Make (start, dir, 5000.0, 20.0, 7);
	REQUIRE (ch.x.size () > 200);
	double dmin = 0.0;
	auto x = ThunderSynth::Render (ch, L, {0.0, 0.08, 0.2}, 2e5, 101325.0, 340.0, 48000, dmin);
	const double dur = x.size () / 48000.0;
	float peak = 0.0f;
	for (float v : x) peak = std::max (peak, std::fabs (v));
	INFO ("nearest " << dmin << " m, " << dur << " s, peak " << peak << " Pa (" << 20.0 * std::log10 (peak / 2e-5) << " dB); below 200 Hz "
		<< Db (BandRms (x, 0, 200, 48000) / Rms (x)) << " dB, above 1 kHz " << Db (BandRms (x, 1000, 0, 48000) / Rms (x)) << " dB");
	CHECK (dmin > 900.0);
	CHECK (dmin < 1300.0);
	// rolling for seconds: the far end's 2.5 km further, and more
	CHECK (dur > 4.0);
	CHECK (dur < 15.0);
	// loud: thunder a kilometre off peaks at some tens of pascals (Holmes et
	// al. 1971: 0.5-10 Pa at 1-10 km)
	CHECK (peak > 5.0f);
	CHECK (peak < 80.0f);
	// low: its energy mostly below 200 Hz
	CHECK (BandRms (x, 0, 200, 48000) > 0.6 * Rms (x));
	CHECK (BandRms (x, 1000, 0, 48000) < 0.2 * Rms (x));
}

TEST_CASE ("Creak: a structure's stick-slip, normalized", "[audio][synth]")
{
	for (int k = 0; k < 2; k++) {
		Rig r;
		auto s = CreakSynth::Make (r.e, 50 + k);
		r.Attach (*s);
		r.Take (0.1);
		float pk = 0.0f;
		double low = 0.0, all = 0.0;
		for (int i = 0; i < 8; i++) {
			s->Strike ((CreakSynth::Kind)k, 1.0f);
			auto x = r.Take (2.0);
			for (float v : x) pk = std::max (pk, std::fabs (v));
			low += BandRms (x, 0, k ? 350.0f : 250.0f, 48000), all += Rms (x);
		}
		INFO ((k ? "groan" : "creak") << ": peak " << pk << ", low share " << low / all);
		// (full strength: about full scale - the emitter's level makes it CREAK_P)
		CHECK (pk > 0.4f);
		CHECK (pk < 2.0f);
	}
}

TEST_CASE ("Ambience: the wind's and the sea's pressure, as asked", "[audio][synth]")
{
	// Monahan's whitecaps: 0.29 % at 7 m/s; the sea's noise 0.02 Pa there
	CHECK_THAT (AmbienceSynth::Whitecaps (7.0), WithinRel (0.00290, 0.03));
	CHECK_THAT (AmbienceSynth::SeaPressure (7.0), WithinRel (0.02, 1e-9));
	for (int k = 0; k < 2; k++) {
		Rig r;
		auto s = AmbienceSynth::Make (r.e, 60 + k);
		if (k == 0) s->Set (1.0, 7.0, 0.0, 8.0); else s->Set (0.0, 7.0, 1.0, 8.0);
		r.Attach (*s);
		r.Take (3.0);
		auto x = r.Take (16.0); // (two waves' periods and more: the swell's mean)
		INFO ((k ? "sea" : "wind") << ": RMS " << Rms (x) << ", below 250 Hz " << Db (BandRms (x, 0, 250, 48000) / Rms (x)) << " dB, above 1 kHz " << Db (BandRms (x, 1000, 0, 48000) / Rms (x)) << " dB");
		CHECK_THAT (Db (Rms (x)), WithinAbs (0.0, 2.0));
		if (k == 0) CHECK (BandRms (x, 0, 250, 48000) > 0.8 * Rms (x)); // (the wind: low)
		else CHECK (BandRms (x, 100, 6000, 48000) > 0.7 * Rms (x));    // (the sea: wash and fizz)
	}
}

namespace {
/// A composer's output over `seconds` in a mood (left, right)
void Compose (music::Composer &c, music::Mood m, double seconds, std::vector<float> &l, std::vector<float> &r, float intensity = 0.5f)
{
	c.SetMood (m);
	c.SetIntensity (intensity);
	const size_t n = (size_t)(seconds * 48000.0);
	l.assign (n, 0.0f), r.assign (n, 0.0f);
	for (size_t i = 0; i < n; i += 512) c.Render (l.data () + i, r.data () + i, (unsigned)std::min<size_t> (512, n - i));
}
}

TEST_CASE ("Music: each mood at its level, moving, not clipping", "[audio][synth][music]")
{
	using namespace music;
	for (int m = MENU; m < NMOOD; m++) {
		Composer c (48000.0f, 11 + m);
		std::vector<float> l, r;
		Compose (c, (Mood)m, 10.0, l, r);   // (in: the level's glide)
		Compose (c, (Mood)m, 60.0, l, r);
		const double rms = std::sqrt (0.5 * (Rms (l) * Rms (l) + Rms (r) * Rms (r)));
		INFO (Name ((Mood)m) << ": RMS " << Db (rms) << " dBFS, peak " << Db (std::max (Peak (l), Peak (r))) << " dBFS, chords " << c.Chords () << ", bells " << c.Bells () << ", pulses " << c.Pulses () << ", sounding " << c.Sounding ());
		// (at its level, within a few dB of the composer's: the mood's own)
		CHECK (Db (rms) < Db (Composer::RMS) + 4.0);
		CHECK (Db (rms) > Db (Composer::RMS) - 12.0);
		CHECK (Db (std::max (Peak (l), Peak (r))) < -3.0);
		CHECK (c.Chords () >= 2);       // (a minute: chords change)
		// stereo: the sides differ
		double d = 0.0;
		for (size_t i = 0; i < l.size (); i++) d += (double)(l[i] - r[i]) * (l[i] - r[i]);
		CHECK (std::sqrt (d / l.size ()) > 0.1 * rms);
	}
}

TEST_CASE ("Music: moods' character - pulses, bells, a pedal", "[audio][synth][music]")
{
	using namespace music;
	std::vector<float> l, r;
	{
		Composer c (48000.0f, 3);
		Compose (c, ASCENT, 30.0, l, r, 1.0f);
		CHECK (c.Pulses () > 60);              // (some 3 a second)
		CHECK (c.Chord ()[0] % 12 == c.Tonic ()); // (the pedal: the tonic)
	}
	{
		Composer c (48000.0f, 4);
		Compose (c, ORBIT, 120.0, l, r);
		CHECK (c.Pulses () == 0);
		CHECK (c.Bells () >= 2);
		INFO ("orbit's chord " << c.Chord ().size ());
		for (int n : c.Chord ()) { CHECK (n >= 31); CHECK (n <= 90); }
	}
}

TEST_CASE ("Music: silence fades it out quickly, and it comes back", "[audio][synth][music]")
{
	using namespace music;
	Composer c (48000.0f, 5);
	std::vector<float> l, r;
	Compose (c, ORBIT, 20.0, l, r);
	const double before = Rms (l, l.size () - 48000);
	Compose (c, SILENT, 6.0, l, r);
	INFO ("before " << Db (before) << " dBFS, after 5 s of silence " << Db (Rms (l, l.size () - 48000) + 1e-12));
	CHECK (Db (Rms (l, l.size () - 48000) + 1e-12) < Db (before) - 40.0);
	Compose (c, SILENT, 30.0, l, r);
	CHECK (c.Sounding () == 0);
	Compose (c, ORBIT, 15.0, l, r);
	CHECK (Db (Rms (l, l.size () - 48000)) > Db (before) - 10.0);
}

TEST_CASE ("Music: cheap enough to run beside everything", "[audio][synth][music]")
{
	using namespace music;
	Composer c (48000.0f, 6);
	std::vector<float> l, r;
	Compose (c, MENU, 5.0, l, r);
	auto t0 = std::chrono::steady_clock::now ();
	Compose (c, MENU, 30.0, l, r);
	const double s = std::chrono::duration<double> (std::chrono::steady_clock::now () - t0).count ();
	INFO ("30 s rendered in " << s << " s");
	CHECK (s < 30.0 * 0.05); // (under 5 % of a core)
}

// ORBITER_MUSIC_DEMO=<dir>: each mood's music into <dir>/music_<mood>.wav (listening)
TEST_CASE ("Music: demos", "[audio][synth][music]")
{
	const char *dir = getenv ("ORBITER_MUSIC_DEMO");
	if (!dir || !*dir) return;
	const double secs = getenv ("ORBITER_MUSIC_SECONDS") ? atof (getenv ("ORBITER_MUSIC_SECONDS")) : 90.0;
	using namespace music;
	for (int m = MENU; m < NMOOD; m++) {
		Composer c (48000.0f, 100 + m);
		std::vector<float> l, r;
		WavWriter w;
		REQUIRE (w.Open (std::string (dir) + "/music_" + Name ((Mood)m) + ".wav", 48000, 2));
		for (double t = 0.0; t < secs; t += 1.0) {
			Compose (c, (Mood)m, 1.0, l, r, (float)std::min (1.0, t / secs));
			std::vector<float> f (l.size () * 2);
			for (size_t i = 0; i < l.size (); i++) f[2 * i] = l[i], f[2 * i + 1] = r[i];
			w.Write (f.data (), l.size ());
		}
		w.Close ();
	}
	// a flight: each mood in turn (the changes between them)
	{
		static const Mood path[] = { MENU, GROUND, ASCENT, ORBIT, APPROACH, ORBIT, DEEP, REENTRY, GROUND };
		Composer c (48000.0f, 200);
		std::vector<float> l, r;
		WavWriter w;
		REQUIRE (w.Open (std::string (dir) + "/music_flight.wav", 48000, 2));
		for (Mood m : path)
			for (double t = 0.0; t < secs / 2.0; t += 1.0) {
				Compose (c, m, 1.0, l, r, (float)std::min (1.0, t / (secs / 2.0)));
				std::vector<float> f (l.size () * 2);
				for (size_t i = 0; i < l.size (); i++) f[2 * i] = l[i], f[2 * i + 1] = r[i];
				w.Write (f.data (), l.size ());
			}
		w.Close ();
	}
}

namespace {
double Pw (const float *x) { double s = 0.0; for (int i = 0; i < Hrtf::TAPS; i++) s += (double)x[i] * x[i]; return s; }
/// A sine (tests)
class ToneSynth : public SynthSource {
public:
	static std::unique_ptr<ToneSynth> Make (Engine &e, float hz) { std::unique_ptr<ToneSynth> s (new ToneSynth ()); s->f = hz; if (!s->Init (e)) return nullptr; return s; }
protected:
	void Render (float *out, unsigned n, float) override { for (unsigned i = 0; i < n; i++) { out[i] = 0.5f * std::sin (ph); ph += 6.2831853f * f / rate; if (ph > 6.2831853f) ph -= 6.2831853f; } }
private:
	float f = 1000.0f, ph = 0.0f;
};
}

TEST_CASE ("HRTF: KEMAR's ears - the level and delay between them, ahead and behind", "[audio][hrtf]")
{
	Hrtf h;
	REQUIRE (h.Load ("Sound/HRTF/kemar.hrtf", 48000));
	float l[Hrtf::TAPS], r[Hrtf::TAPS], dl, dr;
	// from the right: the right ear louder (the head's shadow), first (some 0.65 ms)
	h.Get (1, 0, 0, l, r, dl, dr);
	INFO ("right: ILD " << 10 * std::log10 (Pw (r) / Pw (l)) << " dB, ITD " << (dl - dr) / 48.0 << " ms");
	CHECK (10 * std::log10 (Pw (r) / Pw (l)) > 10.0);
	CHECK ((dl - dr) / 48000.0 > 550e-6);
	CHECK ((dl - dr) / 48000.0 < 800e-6);
	// from the left: the mirror
	float l2[Hrtf::TAPS], r2[Hrtf::TAPS], dl2, dr2;
	h.Get (-1, 0, 0, l2, r2, dl2, dr2);
	CHECK_THAT (Pw (l2), WithinRel (Pw (r), 1e-4));
	CHECK_THAT (dr2 - dl2, WithinAbs (dl - dr, 1e-3));
	// ahead: both alike
	h.Get (0, 0, 1, l, r, dl, dr);
	CHECK_THAT (Pw (l), WithinRel (Pw (r), 0.02));
	CHECK (std::fabs (dl - dr) < 0.5f);
	// behind: another colour (the pinnae) - the spectra apart by some dB
	h.Get (0, 0, -1, l2, r2, dl2, dr2);
	double most = 0.0;
	for (int k = 4; k < 32; k++) { // (3 - 24 kHz in 0.75 kHz bins)
		double a[2] = {0, 0}, b[2] = {0, 0};
		for (int i = 0; i < Hrtf::TAPS; i++) {
			const double w = 2.0 * M_PI * k * i / Hrtf::TAPS;
			a[0] += l[i] * std::cos (w), a[1] -= l[i] * std::sin (w);
			b[0] += l2[i] * std::cos (w), b[1] -= l2[i] * std::sin (w);
		}
		most = std::max (most, std::fabs (10 * std::log10 ((a[0] * a[0] + a[1] * a[1]) / (b[0] * b[0] + b[1] * b[1] + 1e-12))));
	}
	INFO ("ahead and behind apart by up to " << most << " dB");
	CHECK (most > 6.0);
	// the level over the sphere: both ears' power about 1 (panning's)
	double sum = 0.0;
	int n = 0;
	for (int i = 0; i < 2000; i++) {
		const double z = 1.0 - 2.0 * (i + 0.5) / 2000, rr = std::sqrt (1 - z * z), ph = i * 2.39996323;
		if (std::asin (z) < -40.0 * M_PI / 180.0) continue; // (none measured below)
		h.Get ((float)(rr * std::cos (ph)), (float)z, (float)(rr * std::sin (ph)), l, r, dl, dr);
		sum += Pw (l) + Pw (r), n++;
	}
	INFO ("mean power " << 10 * std::log10 (sum / n) << " dB");
	CHECK (std::fabs (10 * std::log10 (sum / n)) < 1.5);
	// at 44.1 kHz: the same delay in seconds
	Hrtf h44;
	REQUIRE (h44.Load ("Sound/HRTF/kemar.hrtf", 44100));
	h44.Get (1, 0, 0, l, r, dl2, dr2);
	h.Get (1, 0, 0, l, r, dl, dr);
	CHECK_THAT ((dl2 - dr2) / 44100.0, WithinAbs ((dl - dr) / 48000.0, 2e-6));
}

TEST_CASE ("HRTF: through a path node - binaural, turning about the head without clicks", "[audio][hrtf]")
{
	Rig rig;
	Hrtf h;
	REQUIRE (h.Load ("Sound/HRTF/kemar.hrtf", rig.e.SampleRate ()));
	PathNode::SetHrtf (&h);
	auto s = ToneSynth::Make (rig.e, 3000.0f);
	rig.Attach (*s);
	auto take = [&] (double sec, std::vector<float> &L, std::vector<float> &R) {
		const uint64_t n = (uint64_t)(sec * rig.e.SampleRate ());
		std::vector<float> b (2 * n);
		rig.e.Render (b.data (), n);
		L.resize (n), R.resize (n);
		for (uint64_t i = 0; i < n; i++) L[i] = b[2 * i], R[i] = b[2 * i + 1];
	};
	PathNode::Target t;
	t.gain = 1.0f;
	t.dir[0] = 1.0f;
	rig.node.Set (PathNode::AIR, t);
	std::vector<float> L, R;
	take (0.2, L, R);
	take (0.5, L, R);
	INFO ("from the right at 3 kHz: L " << Db (Rms (L)) << " dB, R " << Db (Rms (R)) << " dB");
	CHECK (Db (Rms (R)) - Db (Rms (L)) > 6.0); // (a real head at 3 kHz: some 8-10 dB)
	// about the head, twice a second, 10 ms steps: smooth (no jumps beyond
	// the tone's own steps: 2 pi 3000/48000 of its peak)
	double maxStep = 0.0, peak = 0.0;
	for (int i = 0; i < 200; i++) {
		const double a = 2.0 * M_PI * i / 50.0;
		t.dir[0] = (float)std::sin (a), t.dir[1] = 0.3f * (float)std::sin (0.37 * a), t.dir[2] = (float)std::cos (a);
		rig.node.Set (PathNode::AIR, t);
		take (0.01, L, R);
		for (size_t k = 1; k < L.size (); k++) maxStep = std::max ({ maxStep, (double)std::fabs (L[k] - L[k - 1]), (double)std::fabs (R[k] - R[k - 1]) });
		peak = std::max ({ peak, Peak (L), Peak (R) });
	}
	INFO ("turning: peak " << peak << ", the largest step " << maxStep << " (" << maxStep / peak << " of the peak)");
	CHECK (maxStep < 0.5 * peak);
	PathNode::SetHrtf (nullptr);
}

TEST_CASE ("Loudness: BS.1770 - its sine, files as ffmpeg's meter reads them", "[audio][loudness]")
{
	for (unsigned rate : { 48000u, 44100u }) {
		std::vector<float> x (rate * 5);
		for (size_t i = 0; i < x.size (); i++) x[i] = (float)std::sin (2.0 * M_PI * 1000.0 * i / rate);
		// (BS.1770: a full-scale 1 kHz sine in one channel reads -3.01)
		CHECK_THAT (Loudness (x.data (), x.size (), 1, rate), WithinAbs (-3.01, 0.05));
		for (float &v : x) v *= 0.1f;
		CHECK_THAT (Loudness (x.data (), x.size (), 1, rate), WithinAbs (-23.01, 0.05));
	}
	// silence: none; gated: a pause doesn't make it quieter
	std::vector<float> z (48000 * 3, 0.0f);
	CHECK (!std::isfinite (Loudness (z.data (), z.size (), 1, 48000)));
	std::vector<float> y (48000 * 6, 0.0f);
	for (size_t i = 0; i < 48000 * 3; i++) y[i] = 0.1f * (float)std::sin (2.0 * M_PI * 1000.0 * i / 48000);
	CHECK_THAT (Loudness (y.data (), y.size (), 1, 48000), WithinAbs (-23.01, 0.3)); // (the blocks across its end: a little less)
	// files (ffmpeg's ebur128: -17.6, -11.6, -21.9 LUFS)
	CHECK_THAT (FileLoudness ("Sound/Voice/Ignition.flac"), WithinAbs (-17.6, 0.3));
	CHECK_THAT (FileLoudness ("XRSound/Default/Autopilot On.wav"), WithinAbs (-11.6, 0.3));
	CHECK_THAT (FileLoudness ("Sound/Voice/Ground/Undocked.flac"), WithinAbs (-21.9, 0.3));
	CHECK_THAT (LoudnessGain ("XRSound/Default/Autopilot On.wav", -18.0), WithinRel (std::pow (10.0, (-18.0 + 11.6) / 20.0), 0.05));
}

// ORBITER_MUSIC_DEMO=<dir>: an engine's roar about the head - around it
// (ahead, right, behind, left), then over it - binaural, then panned
// (<dir>/hrtf_circle.wav; headphones)
TEST_CASE ("HRTF: demo", "[audio][hrtf]")
{
	const char *dir = getenv ("ORBITER_MUSIC_DEMO");
	if (!dir || !*dir) return;
	Hrtf h;
	WavWriter w;
	REQUIRE (w.Open (std::string (dir) + "/hrtf_circle.wav", 48000, 2));
	for (int pass = 0; pass < 2; pass++) {
		Rig rig;
		REQUIRE (h.Load ("Sound/HRTF/kemar.hrtf", rig.e.SampleRate ()));
		PathNode::SetHrtf (pass == 0 ? &h : nullptr);
		auto s = RocketSynth::Make (rig.e, 7);
		RocketSynth::Params p;
		p.level = 0.6f, p.peak = 900.0f, p.crackle = 0.2f;
		s->Set (p);
		rig.Attach (*s);
		std::vector<float> b (2 * 480);
		for (int i = 0; i < 1400; i++) { // (14 s in 10 ms steps)
			const double t = i * 0.01;
			double az = 0.0, el = 0.0;
			if (t < 8.0) az = 2.0 * M_PI * t / 8.0;               // around
			else if (t < 12.0) el = M_PI * (t - 8.0) / 4.0;       // ahead, up, over, behind
			else el = M_PI;
			PathNode::Target tg;
			tg.gain = 0.5f;
			tg.dir[0] = (float)(std::sin (az) * std::cos (el)), tg.dir[1] = (float)std::sin (el), tg.dir[2] = (float)(std::cos (az) * std::cos (el));
			tg.pan = (float)std::clamp (0.7 * tg.dir[0], -1.0, 1.0);
			rig.node.Set (PathNode::AIR, tg);
			rig.e.Render (b.data (), 480);
			w.Write (b.data (), 480);
		}
		std::vector<float> gap (2 * 48000, 0.0f);
		w.Write (gap.data (), 48000);
	}
	PathNode::SetHrtf (nullptr);
	w.Close ();
}
