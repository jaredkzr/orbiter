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
	outLevel.store (lv, std::memory_order_relaxed);
	// (the roar while firing: low, about 0.4 of the size's peak)
	roarBp.BandPass (0.4f * pk, 0.7f, rate);
	hissHp.HighPass (std::max (2500.0f, 3.0f * pk), 0.6f, rate);
	clickBp.BandPass (350.0f * dop, 0.8f, rate);
	whumpLp.LowPass (160.0f * dop, 0.7f, rate);

	offTime += (float)n / rate;
	if (!on && target > ON && offTime >= HOLD) {
		on = true;
		float k = std::clamp (target, 0.3f, 1.0f); // (a valve opening wide bangs harder)
		bang = k; bangT = 0.0f; bangPh = 0.0f; click = 0.7f * k; whump = k;
	} else if (on && target < OFF) { on = false; click = 0.15f; whump = 0.25f; offTime = 0.0f; }
	if (on) offTime = 0.0f;

	// the bang: a thump falling in pitch (110 Hz to 35 Hz in about 25 ms, as
	// a kick drum), a short low whump of noise, a low snap at its start;
	// punchy: all of it over in a tenth of a second
	const float bangDecay = std::exp (-1.0f / (0.07f * rate));
	const float whumpDecay = std::exp (-1.0f / (0.035f * rate));
	const float clickDecay = std::exp (-1.0f / (0.004f * rate));
	const float amp = std::sqrt (std::max (lv, 0.0f));
	for (unsigned i = 0; i < n; i++) {
		float w = rng.White ();
		float p = pink.Next (w);
		float s = amp * (1.3f * roarBp.Process (p) + 0.12f * hissHp.Process (w));
		if (bang > 1e-4f) {
			float f = (35.0f + 75.0f * std::exp (-bangT / 0.009f)) * dop;
			bangPh += 6.2831853f * f / rate;
			bangT += 1.0f / rate;
			s += 3.2f * bang * std::sin (bangPh);
			bang *= bangDecay;
		}
		if (whump > 1e-4f) {
			s += 3.5f * whump * whumpLp.Process (p);
			whump *= whumpDecay;
		} else whumpLp.Process (0.0f);
		if (click > 1e-4f) {
			s += 1.2f * click * clickBp.Process (w);
			click *= clickDecay;
		}
		out[i] = 1.0f * s; // (0.3 RMS firing)
	}
	if (bang < 1e-4f) bang = 0.0f;
	if (whump < 1e-4f) whump = 0.0f;
	if (click < 1e-4f) click = 0.0f;
}

} // namespace audio
