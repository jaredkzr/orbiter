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
	// off: silent after the chuff
	p.level = 0.0f;
	s->Set (p);
	r.Take (1.5);
	CHECK (Rms (r.Take (0.5)) < 1e-3);
}

TEST_CASE ("Rocket: bigger engines lower", "[audio][synth]")
{
	CHECK (RocketSynth::PeakFor (3.5e7) < 25.0f);
	CHECK (RocketSynth::PeakFor (1e5) > 300.0f);
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
