// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "AmbienceSynth.h"
#include <algorithm>
#include <cmath>

namespace audio {

namespace {
	constexpr float TWO_PI = 6.2831853f;
	/// Gains normalizing each part to an RMS of 1 (unit tests check them)
	constexpr float WIND_NORM = 2.56f, SEA_NORM = 6.21f;
}

std::unique_ptr<AmbienceSynth> AmbienceSynth::Make (Engine &e, uint32_t seed)
{
	std::unique_ptr<AmbienceSynth> s (new AmbienceSynth (seed));
	if (!s->Init (e)) return nullptr;
	return s;
}

void AmbienceSynth::Set (double windP, double windSpeed, double seaP, double period)
{
	pWind.store ((float)std::max (windP, 0.0), std::memory_order_relaxed);
	pSpeed.store ((float)std::max (windSpeed, 0.0), std::memory_order_relaxed);
	pSea.store ((float)std::max (seaP, 0.0), std::memory_order_relaxed);
	pPeriod.store ((float)std::clamp (period, 2.0, 20.0), std::memory_order_relaxed);
}

void AmbienceSynth::Render (float *out, unsigned n, float dop)
{
	const float wind = windS.Step (pWind.load (std::memory_order_relaxed), dsp::Smooth::K (1.0f, n, rate));
	const float sea = seaS.Step (pSea.load (std::memory_order_relaxed), dsp::Smooth::K (1.0f, n, rate));
	const float U = pSpeed.load (std::memory_order_relaxed);
	const float T = pPeriod.load (std::memory_order_relaxed);
	if (wind < 1e-7f && sea < 1e-7f) { std::fill (out, out + n, 0.0f); return; }
	// the wind: its rumble below some 100-250 Hz (faster: higher), a
	// whoosh above it, quieter; gusts over seconds
	const float fc = std::clamp (60.0f + 15.0f * U, 60.0f, 250.0f);
	windLp.LowPass (fc, 0.7f, rate);
	windLp2.LowPass (fc, 0.7f, rate);
	whooshBp.BandPass (std::clamp (250.0f + 40.0f * U, 250.0f, 900.0f), 0.8f, rate);
	gust.SetCutoff (0.25f, rate);
	gust2.SetCutoff (0.05f, rate);
	// the sea: the wash (100-1000 Hz) and the foam's fizz (1-6 kHz)
	washHp.HighPass (100.0f, 0.7f, rate);
	washLp.LowPass (1000.0f, 0.7f, rate);
	foamHp.HighPass (1000.0f, 0.7f, rate);
	foamLp.LowPass (6000.0f, 0.7f, rate);
	for (unsigned i = 0; i < n; i++) {
		const float w = rng.White ();
		float s = 0.0f;
		if (wind > 1e-7f) {
			const float p = pink.Next (w);
			// (turbulence: some 20 % about its mean, slow and slower)
			const float g = std::max (0.2f, 1.0f + std::clamp (120.0f * gust.Lp (rng.White ()) + 250.0f * gust2.Lp (rng.White ()), -0.8f, 1.2f));
			s += wind * g * WIND_NORM * (windLp2.Process (windLp.Process (p)) + 0.25f * whooshBp.Process (p));
		}
		if (sea > 1e-7f) {
			// each wave's wash swelling and ebbing over its period
			wavePh += 1.0f / (T * rate);
			if (wavePh >= 1.0f) wavePh -= 1.0f;
			const float swell = 0.35f + 0.65f * std::pow (0.5f + 0.5f * std::sin (TWO_PI * wavePh), 2.0f);
			const float q = pink2.Next (rng.White ());
			s += sea * SEA_NORM * swell * (washLp.Process (washHp.Process (q)) + 0.5f * foamLp.Process (foamHp.Process (w)));
		}
		out[i] = s;
	}
}

} // namespace audio
