// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "WindSynth.h"
#include <algorithm>
#include <cmath>

namespace audio {

SourcePtr<WindSynth> WindSynth::Make (Engine &e, uint32_t seed)
{
	SourcePtr<WindSynth> s (new WindSynth (seed));
	if (!s->Init (e)) return nullptr;
	return s;
}

void WindSynth::Set (const Params &p)
{
	pSpeed.store (std::max (0.0f, p.speed), std::memory_order_relaxed);
	pBuffet.store (std::clamp (p.buffet, 0.0f, 1.0f), std::memory_order_relaxed);
}

float WindSynth::BuffetFor (double aoa, double mach)
{
	double a = std::fabs (aoa);
	double stall = std::clamp ((a - 0.2) / 0.25, 0.0, 1.0);          // from 12 deg, full at 26 deg
	double trans = std::exp (-0.5 * std::pow ((mach - 1.0) / 0.12, 2.0)); // around Mach 1
	return (float)std::clamp (stall + 0.7 * trans, 0.0, 1.0);
}

void WindSynth::Render (float *out, unsigned n, float dop)
{
	const float tSpeed = pSpeed.load (std::memory_order_relaxed);
	const float tBuff = pBuffet.load (std::memory_order_relaxed);
	if (first) { speed.v = tSpeed; buffet.v = tBuff; first = false; }
	float u = speed.Step (tSpeed, dsp::Smooth::K (0.2f, n, rate));
	float bf = buffet.Step (tBuff, dsp::Smooth::K (0.3f, n, rate));

	// the rush's centre goes with speed: a few hundred hertz at 100 m/s,
	// kilohertz at a thousand
	float fc = std::clamp (150.0f + 2.5f * u, 120.0f, 5000.0f) * dop;
	rushBp.BandPass (fc, 0.45f, rate);
	rushLp.LowPass (fc * 0.35f, 0.5f, rate);
	buffLp.LowPass (70.0f * dop, 0.8f, rate);
	buffHp.HighPass (12.0f * dop, 0.7f, rate);
	gust.SetCutoff (0.8f, rate);
	buffEnv.SetCutoff (6.0f, rate);
	gustK = 0.25f / (0.577f * std::sqrt (std::max (gust.a, 1e-9f) * 0.5f));
	buffK = 0.8f / (0.577f * std::sqrt (std::max (buffEnv.a, 1e-9f) * 0.5f));

	for (unsigned i = 0; i < n; i++) {
		float w = rng.White ();
		float p = pink.Next (w);
		float g = std::max (0.3f, 1.0f + gustK * gust.Lp (rng.White ()));
		float rush = g * (1.6f * rushBp.Process (p) + 1.0f * rushLp.Process (p));
		// buffeting: low noise in irregular bursts
		float env = std::max (0.0f, buffK * buffEnv.Lp (rng.White ()));
		float b = bf * env * 4.0f * buffHp.Process (buffLp.Process (p));
		out[i] = 0.65f * (rush + b); // (0.3 RMS)
	}
}

} // namespace audio
