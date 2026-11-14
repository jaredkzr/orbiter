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
	constexpr float CRACKLE_RATE = 600.0f;
	/// the biggest pop, against a typical one (the heavy tail capped: no spikes
	/// that only the limiter can catch)
	constexpr float CRACKLE_CAP = 2.5f;
}

SourcePtr<RocketSynth> RocketSynth::Make (Engine &e, uint32_t seed)
{
	SourcePtr<RocketSynth> s (new RocketSynth (seed));
	if (!s->Init (e)) return nullptr;
	return s;
}

void RocketSynth::Set (const Params &p)
{
	pLevel.store (std::clamp (p.level, 0.0f, 1.0f), std::memory_order_relaxed);
	pPeak.store (std::clamp (p.peak, 10.0f, 4000.0f), std::memory_order_relaxed);
	pCrackle.store (std::clamp (p.crackle, 0.0f, 1.0f), std::memory_order_relaxed);
	pRumble.store (std::clamp (p.rumble, 0.0f, 1.0f), std::memory_order_relaxed);
	pWhine.store (std::clamp (p.whine, 0.0f, 1.0f), std::memory_order_relaxed);
	pPump.store (std::clamp (p.pump, 200.0f, 6000.0f), std::memory_order_relaxed);
}

float RocketSynth::RumbleFor (double maxThrust)
{
	// a DG engine (160 kN): 0.6; a launcher stage (35 MN): 1
	return (float)std::clamp (0.6 + 0.17 * std::log10 (std::max (maxThrust, 1.0) / 1.6e5), 0.4, 1.0);
}

float RocketSynth::PumpFor (double maxThrust)
{
	// about 1.6 kHz for a 100 kN engine, lower for bigger ones
	return (float)std::clamp (1600.0 * std::pow (std::max (maxThrust, 1.0) / 1e5, -0.12), 500.0, 3000.0);
}

float RocketSynth::PeakFor (double maxThrust)
{
	// a launcher stage (35 MN, D ~ 10 m) peaks low, about 12 Hz; a 100 kN
	// engine (D ~ 0.5 m) near 200 Hz: f ~ thrust^-1/2 (tuned by ear a little
	// below the Strouhal estimate: a deep roar)
	double f = 12.0 * std::sqrt (3.5e7 / std::max (maxThrust, 1.0));
	return (float)std::clamp (f, 10.0, 900.0);
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
	outLevel.store (lv, std::memory_order_relaxed);
	peak = peakS.Step (peak, dsp::Smooth::K (0.3f, n, rate)) * dop;
	// (higher throttle: a faster jet, a higher peak)
	float pk = peak * (0.8f + 0.3f * lv);
	roarBp.BandPass (pk, 0.6f, rate);
	roarLp.LowPass (pk * 3.5f, 0.5f, rate);
	hiss.HighPass (pk * 10.0f, 0.5f, rate);
	crackHp.HighPass (std::max (300.0f, 2.0f * pk) * dop, 0.7f, rate);
	crackShape.SetCutoff (2500.0f * dop, rate); // (rounder pops)
	rumbleLp.LowPass (std::min (80.0f, 0.5f * pk + 20.0f) * dop, 0.9f, rate);
	rumbleHp.HighPass (25.0f * dop, 0.7f, rate);
	pumpWobble.SetCutoff (3.0f, rate);
	const float whineMix = pWhine.load (std::memory_order_relaxed);
	const float pumpF = pPump.load (std::memory_order_relaxed);
	// the pumps: spool up with the throttle (0.7 s), wind down slowly (1.5 s)
	const float pumpTarget = lit ? 0.55f + 0.45f * std::sqrt (std::max (lv, 0.0f)) : 0.0f;
	pumpSpeed += (pumpTarget > pumpSpeed ? dsp::Smooth::K (0.7f, n, rate) : dsp::Smooth::K (1.5f, n, rate)) * (pumpTarget - pumpSpeed);
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
		float roar = 2.4f * roarBp.Process (p) + 1.0f * roarLp.Process (p) + 0.04f * hiss.Process (pinkHi.Next (rng.White ()));
		float s = amp * fl * roar;
		// crackle: one-sided pops at random times, heavy-tailed
		float c = 0.0f;
		if (crackRate > 1.0f) {
			crackRun -= 1.0f;
			if (crackRun <= 0.0f) {
				float u = std::max (rng.Uniform (), 1e-6f);
				crackRun += -std::log (u) * rate / crackRate;
				float g = rng.Gauss ();
				c = std::min (std::exp (0.7f * g), CRACKLE_CAP) * 0.6f;
			}
		}
		float cs = crackHp.Process (crackShape.Lp (c));
		s += crackMix * (amp + 0.6f * burst) * cs * 2.0f;
		// rumble: the sub-bass
		if (rumbleMix > 0.0f) s += rumbleMix * amp * 5.0f * rumbleHp.Process (rumbleLp.Process (p));
		// the turbopumps' whine (a slow wobble of 0.3 % in pitch)
		if (whineMix > 0.0f && pumpSpeed > 0.01f) {
			float wob = 1.0f + 0.4f * pumpWobble.Lp (rng.White ()); // (sigma about 0.3 %)
			float f1 = pumpF * pumpSpeed * wob * dop;
			pumpPh1 += TWO_PI * f1 / rate;
			pumpPh2 += TWO_PI * 2.03f * f1 / rate;
			if (pumpPh1 > TWO_PI) pumpPh1 -= TWO_PI;
			if (pumpPh2 > TWO_PI) pumpPh2 -= TWO_PI;
			s += whineMix * 0.5f * pumpSpeed * pumpSpeed * (std::sin (pumpPh1) + 0.35f * std::sin (pumpPh2));
		}
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
