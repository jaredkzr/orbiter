// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "RocketSynth.h"
#include <algorithm>
#include <cmath>

namespace audio {

namespace {
	constexpr float TWO_PI = 6.2831853f;
	/// the throttle spools with this time constant [s]
	constexpr float SPOOL_TAU = 0.12f;
	/// below this level the engine is out
	constexpr float LIT = 0.01f;
	/// crackle: pops per second at full throttle
	constexpr float CRACKLE_RATE = 900.0f;
}

std::unique_ptr<RocketSynth> RocketSynth::Make (Engine &e, uint32_t seed)
{
	std::unique_ptr<RocketSynth> s (new RocketSynth (seed));
	if (!s->Init (e)) return nullptr;
	return s;
}

void RocketSynth::Set (const Params &p)
{
	pLevel.store (std::clamp (p.level, 0.0f, 1.0f), std::memory_order_relaxed);
	pPeak.store (std::clamp (p.peak, 10.0f, 4000.0f), std::memory_order_relaxed);
	pCrackle.store (std::clamp (p.crackle, 0.0f, 1.0f), std::memory_order_relaxed);
	pRumble.store (std::clamp (p.rumble, 0.0f, 1.0f), std::memory_order_relaxed);
}

float RocketSynth::PeakFor (double maxThrust)
{
	// a launcher stage (35 MN, D ~ 10 m) peaks at about 20 Hz; a 100 kN
	// engine (D ~ 0.5 m) at a few hundred: f ~ thrust^-1/2
	double f = 20.0 * std::sqrt (3.5e7 / std::max (maxThrust, 1.0));
	return (float)std::clamp (f, 15.0, 1500.0);
}

void RocketSynth::Render (float *out, unsigned n, float dop)
{
	const float target = pLevel.load (std::memory_order_relaxed);
	const float crackMix = pCrackle.load (std::memory_order_relaxed);
	const float rumbleMix = pRumble.load (std::memory_order_relaxed);
	float peak = pPeak.load (std::memory_order_relaxed);
	if (first) { level.v = target; peakS.v = peak; first = false; lit = target > LIT; }

	// the block's settings (the Doppler factor shifts every frequency)
	float lv = level.Step (target, dsp::Smooth::K (SPOOL_TAU, n, rate));
	peak = peakS.Step (peak, dsp::Smooth::K (0.3f, n, rate)) * dop;
	// (higher throttle: a faster jet, a higher peak)
	float pk = peak * (0.8f + 0.3f * lv);
	roarBp.BandPass (pk, 0.6f, rate);
	roarLp.LowPass (pk * 6.0f, 0.5f, rate);
	hiss.HighPass (pk * 8.0f, 0.5f, rate);
	crackHp.HighPass (std::max (900.0f, 4.0f * pk) * dop, 0.7f, rate);
	crackShape.SetCutoff (6000.0f * dop, rate);
	rumbleLp.LowPass (std::min (40.0f, pk) * dop, 0.7f, rate);
	chuffBp.BandPass (pk * 2.0f, 0.8f, rate);
	flutter.SetCutoff (4.0f * dop, rate);
	// (white noise through the one-pole: sigma 0.577 sqrt(a/2); scaled to a
	// flicker of 15 % RMS)
	flutterK = 0.15f / (0.577f * std::sqrt (std::max (flutter.a, 1e-9f) * 0.5f));

	// ignition and shutdown
	if (!lit && lv > LIT) { lit = true; thump = 1.0f; thumpPh = 0.0f; burst = 1.0f; }
	else if (lit && lv < LIT * 0.5f) { lit = false; chuff = 1.0f; }

	// loudness with throttle: acoustic power about with thrust (amplitude ^0.7)
	const float amp = lv > 0.0f ? std::pow (lv, 0.7f) : 0.0f;
	const float crackRate = CRACKLE_RATE * dop * std::max (lv, burst);
	const float thumpF = std::clamp (pk * 0.4f, 25.0f, 120.0f);
	const float thumpDecay = std::exp (-1.0f / (0.18f * rate));
	const float burstDecay = std::exp (-1.0f / (0.25f * rate));
	const float chuffDecay = std::exp (-1.0f / (0.15f * rate));

	for (unsigned i = 0; i < n; i++) {
		float w = rng.White ();
		float p = pink.Next (w);
		// the turbulence's flicker: 15 % RMS in amplitude
		float fl = std::max (0.2f, 1.0f + flutterK * flutter.Lp (rng.White ()));
		// roar: the hump around the peak, its skirt, a little hiss above
		float roar = 2.2f * roarBp.Process (p) + 0.9f * roarLp.Process (p) + 0.12f * hiss.Process (pinkHi.Next (rng.White ()));
		float s = amp * fl * roar;
		// crackle: one-sided pops at random times, heavy-tailed
		float c = 0.0f;
		if (crackRate > 1.0f) {
			crackRun -= 1.0f;
			if (crackRun <= 0.0f) {
				float u = std::max (rng.Uniform (), 1e-6f);
				crackRun += -std::log (u) * rate / crackRate;
				float g = rng.Gauss ();
				c = std::exp (0.9f * g) * 0.6f;
			}
		}
		float cs = crackHp.Process (crackShape.Lp (c));
		s += crackMix * (amp + 0.6f * burst) * cs * 3.0f;
		// rumble
		if (rumbleMix > 0.0f) s += rumbleMix * amp * 3.0f * rumbleLp.Process (p);
		// ignition thump (a decaying low sine) and burst; shutdown chuff
		if (thump > 1e-4f) {
			thumpPh += TWO_PI * thumpF * dop / rate;
			s += 2.0f * thump * std::sin (thumpPh);
			thump *= thumpDecay;
		}
		burst *= burstDecay;
		if (chuff > 1e-4f) {
			s += 1.2f * chuff * chuffBp.Process (p);
			chuff *= chuffDecay;
		}
		out[i] = 0.35f * s;
	}
	if (thump < 1e-4f) thump = 0.0f;
	if (burst < 1e-4f) burst = 0.0f;
	if (chuff < 1e-4f) chuff = 0.0f;
}

} // namespace audio
