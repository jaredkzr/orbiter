// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "RcsSynth.h"
#include <algorithm>
#include <cmath>

namespace audio {

namespace {
	/// a pulse starts above ON, ends below OFF (and stays off a while: an
	/// autopilot's proportional control hovering about the threshold is
	/// one pulse, not a drum roll)
	constexpr float ON = 0.06f, OFF = 0.02f, HOLD = 0.15f;
}

std::unique_ptr<RcsSynth> RcsSynth::Make (Engine &e, uint32_t seed)
{
	std::unique_ptr<RcsSynth> s (new RcsSynth (seed));
	if (!s->Init (e)) return nullptr;
	return s;
}

void RcsSynth::Set (float l, float peak)
{
	pLevel.store (std::clamp (l, 0.0f, 1.0f), std::memory_order_relaxed);
	pPeak.store (std::clamp (peak, 50.0f, 4000.0f), std::memory_order_relaxed);
}

void RcsSynth::Render (float *out, unsigned n, float dop)
{
	const float target = pLevel.load (std::memory_order_relaxed);
	const float pk = pPeak.load (std::memory_order_relaxed) * dop;
	// (valves open and close fast: 10 ms)
	float lv = level.Step (target, dsp::Smooth::K (0.01f, n, rate));
	roarBp.BandPass (pk, 0.7f, rate);
	hissHp.HighPass (std::max (2500.0f, 3.0f * pk), 0.6f, rate);
	clickBp.BandPass (2500.0f * dop, 1.5f, rate);

	offTime += (float)n / rate;
	if (!on && target > ON && offTime >= HOLD) {
		on = true;
		float k = std::clamp (target, 0.3f, 1.0f); // (a valve opening wide bangs harder)
		bang = k; bangPh = 0.0f; click = k;
	} else if (on && target < OFF) { on = false; click = 0.4f; offTime = 0.0f; }
	if (on) offTime = 0.0f;

	const float bangF = 55.0f * dop;
	const float bangDecay = std::exp (-1.0f / (0.06f * rate));
	const float clickDecay = std::exp (-1.0f / (0.004f * rate));
	const float amp = std::sqrt (std::max (lv, 0.0f));
	for (unsigned i = 0; i < n; i++) {
		float w = rng.White ();
		float p = pink.Next (w);
		float s = amp * (1.6f * roarBp.Process (p) + 0.5f * hissHp.Process (w));
		if (bang > 1e-4f) {
			bangPh += 6.2831853f * bangF / rate;
			s += 1.2f * bang * std::sin (bangPh);
			bang *= bangDecay;
		}
		if (click > 1e-4f) {
			s += 2.0f * click * clickBp.Process (w);
			click *= clickDecay;
		}
		out[i] = 0.84f * s; // (0.3 RMS firing)
	}
	if (bang < 1e-4f) bang = 0.0f;
	if (click < 1e-4f) click = 0.0f;
}

} // namespace audio
