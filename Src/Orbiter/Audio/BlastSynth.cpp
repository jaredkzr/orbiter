// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "BlastSynth.h"
#include <algorithm>
#include <cmath>

namespace audio {

namespace {
	constexpr double P0 = 101325.0;
	/// The Friedlander wave's decay (weak, distant blasts: about 1)
	constexpr float FRIEDLANDER_B = 1.2f;
}

std::unique_ptr<BlastSynth> BlastSynth::Make (Engine &e, uint32_t seed)
{
	std::unique_ptr<BlastSynth> s (new BlastSynth (seed));
	if (!s->Init (e)) return nullptr;
	return s;
}

double BlastSynth::Overpressure (double Z)
{
	Z = std::max (Z, 0.05);
	const double a = 1.0 + (Z / 4.5) * (Z / 4.5);
	const double b = std::sqrt (1.0 + (Z / 0.048) * (Z / 0.048)) * std::sqrt (1.0 + (Z / 0.32) * (Z / 0.32)) * std::sqrt (1.0 + (Z / 1.35) * (Z / 1.35));
	return P0 * 808.0 * a / b;
}

double BlastSynth::Duration (double Z, double W)
{
	Z = std::max (Z, 0.05);
	const double num = 980.0 * (1.0 + std::pow (Z / 0.54, 10.0));
	const double den = (1.0 + std::pow (Z / 0.02, 3.0)) * (1.0 + std::pow (Z / 0.74, 6.0)) * std::sqrt (1.0 + (Z / 6.9) * (Z / 6.9));
	return 1e-3 * num / den * std::cbrt (std::max (W, 1e-3)); // [ms/kg^1/3] scaled by W^1/3
}

double BlastSynth::TntEquivalent (double propellant, bool surface)
{
	return std::max (propellant, 0.0) * YIELD * (surface ? SURFACE : 1.0);
}

void BlastSynth::Sachs (double R, double W, double p, double c, double &dP, double &tdv)
{
	const double pr = std::max (p, 1e-3) / P0;
	const double sR = std::cbrt (1.0 / pr);              // (P0/p)^1/3
	const double Z = R * sR / std::cbrt (std::max (W, 1e-3));
	dP = Overpressure (Z) * pr;
	tdv = Duration (Z, W) * sR * 340.3 / std::max (c, 1.0);
}

void BlastSynth::Set (double R, double tdv)
{
	pR.store ((float)std::max (R, 1.0), std::memory_order_relaxed);
	pTd.store ((float)std::clamp (tdv, 0.002, 1.0), std::memory_order_relaxed);
}

float BlastSynth::Friedlander (float t, float td)
{
	if (t < 0.0f) return 0.0f;
	return (1.0f - t / td) * std::exp (-FRIEDLANDER_B * t / td);
}

void BlastSynth::Render (float *out, unsigned n, float dop)
{
	if (!started) {
		if (!fire.load (std::memory_order_acquire)) { std::fill (out, out + n, 0.0f); return; }
		// the blast arrives: its shape for the distance it came
		started = true;
		td = pTd.load (std::memory_order_relaxed);
		const float R = pR.load (std::memory_order_relaxed);
		// the reflections: from the ground and the terrain about, a few
		// tenths as strong, spread over a time growing with the distance
		// (the terrain within reach of a path not much longer); then their
		// rumble, a T60 of a few seconds
		const float spread = std::clamp (0.15f + R / 3000.0f, 0.15f, 2.5f);
		for (int k = 0; k < NECHO; k++) {
			echoT[k] = 0.01f + spread * (0.1f + 0.9f * rng.Uniform ()) * (k + 1) / NECHO;
			echoA[k] = (0.35f - 0.04f * k) * (0.6f + 0.4f * rng.Uniform ()) * (rng.Uniform () < 0.2f ? -0.5f : 1.0f);
		}
		tailT60 = std::clamp (1.5f + R / 2000.0f, 1.5f, 6.0f);
		len = echoT[NECHO - 1] + 6.0f * td + tailT60 + 0.5f;
		rumbleLp.LowPass (90.0f, 0.7f, rate);
		rumbleLp2.LowPass (90.0f, 0.7f, rate);
		// (the shock's rise: instant at the source, a millisecond or so after
		// the air's turbulence over a distance)
		rise.SetCutoff (std::clamp (6000.0f / (1.0f + R / 500.0f), 300.0f, 6000.0f), rate);
		t = 0.0f;
	}
	const float dt = dop / rate;
	const float tailK = 6.9f / tailT60; // (60 dB over T60)
	for (unsigned i = 0; i < n; i++) {
		if (t > len) { out[i] = 0.0f; continue; }
		float s = Friedlander (t, td);
		for (int k = 0; k < NECHO; k++) s += echoA[k] * Friedlander (t - echoT[k], td * 1.3f);
		s = rise.Lp (s);
		// the rumble: the reflections blurred, low, building over a tenth of a
		// second and dying away
		const float env = (1.0f - std::exp (-t / 0.08f)) * std::exp (-tailK * t);
		s += 0.25f * env * rumbleLp2.Process (rumbleLp.Process (pink.Next (rng.White ()))) * 4.0f;
		out[i] = s;
		t += dt;
	}
	if (t > len) done.store (true, std::memory_order_release);
}

} // namespace audio
