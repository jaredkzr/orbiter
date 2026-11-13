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
#include "Audio/VesselSounds.h"
#include "miniaudio.h"
#include <cmath>
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
