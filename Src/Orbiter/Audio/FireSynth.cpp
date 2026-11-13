// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "FireSynth.h"
#include <algorithm>
#include <cmath>

namespace audio {

namespace {
	constexpr float TWO_PI = 6.2831853f;
}

std::unique_ptr<FireSynth> FireSynth::Make (Engine &e, uint32_t seed)
{
	std::unique_ptr<FireSynth> s (new FireSynth (seed));
	if (!s->Init (e)) return nullptr;
	return s;
}

double FireSynth::PeakFor (double Q)
{
	// (combustion noise's peak falls slowly with the fire's size: about 400
	// Hz for a megawatt, near 100 Hz for gigawatts)
	return std::clamp (400.0 * std::pow (std::max (Q, 1.0) / 1e6, -0.18), 25.0, 600.0);
}

double FireSynth::DiameterFor (double Q)
{
	return std::clamp (std::sqrt (4.0 * std::max (Q, 1.0) / (3.14159265 * 2e6)), 0.3, 300.0);
}

void FireSynth::Set (double peak, double diameter, double level)
{
	pPeak.store ((float)peak, std::memory_order_relaxed);
	pDiam.store ((float)diameter, std::memory_order_relaxed);
	pLevel.store ((float)std::clamp (level, 0.0, 1.0), std::memory_order_relaxed);
}

void FireSynth::Render (float *out, unsigned n, float dop)
{
	const float fpk = pPeak.load (std::memory_order_relaxed) * dop;
	const float D = pDiam.load (std::memory_order_relaxed);
	const float level = levelS.Step (pLevel.load (std::memory_order_relaxed), dsp::Smooth::K (0.5f, n, rate));
	// the hump: pink noise between half and twice the peak (two octaves),
	// rolling off either side; a low rumble under it
	hp.HighPass (0.5f * fpk, 0.6f, rate);
	lp.LowPass (2.0f * fpk, 0.6f, rate);
	lp2.LowPass (2.0f * fpk, 0.6f, rate);
	rumble.LowPass (0.4f * fpk, 0.7f, rate);
	flicker.SetCutoff (3.0f, rate);
	const float puff = 1.5f / std::sqrt (std::max (D, 0.3f)) * dop;
	for (unsigned i = 0; i < n; i++) {
		const float p = pink.Next (rng.White ());
		// the throb of its puffing, the flicker of its turbulence
		puffPh += puff / rate;
		if (puffPh >= 1.0f) puffPh -= 1.0f;
		const float am = 1.0f + 0.3f * std::sin (TWO_PI * puffPh) + std::clamp (30.0f * flicker.Lp (rng.White ()), -0.4f, 0.4f);
		const float s = 2.2f * lp2.Process (lp.Process (hp.Process (p))) + 1.2f * rumble.Process (p);
		out[i] = level * am * s;
	}
}

} // namespace audio
