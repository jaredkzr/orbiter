// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "CreakSynth.h"
#include <algorithm>
#include <cmath>

namespace audio {

std::unique_ptr<CreakSynth> CreakSynth::Make (Engine &e, uint32_t seed)
{
	std::unique_ptr<CreakSynth> s (new CreakSynth (seed));
	if (!s->Init (e)) return nullptr;
	return s;
}

void CreakSynth::Strike (Kind k, float strength)
{
	if (k < 0 || k > 1) return;
	pending[k].strength.store (std::clamp (strength, 0.0f, 1.5f), std::memory_order_relaxed);   // (over 1: a structure near tearing)
	pending[k].count.fetch_add (1, std::memory_order_release);
}

void CreakSynth::Start (Kind k, float s)
{
	Voice *v = nullptr;
	for (auto &x : voices) if (!x.on) { v = &x; break; }
	if (!v) return; // (enough creaking at once)
	*v = Voice ();
	v->on = true, v->s = s;
	if (k == CREAK) {
		// a panel or a joint: 0.1-0.4 s, slipping 40-160 times a second,
		// panels' modes 300-900 Hz
		v->len = 0.1f + 0.3f * rng.Uniform ();
		v->rate0 = 40.0f + 80.0f * rng.Uniform ();
		v->rate1 = v->rate0 * (0.7f + 0.8f * rng.Uniform ());
		v->modeA.BandPass (300.0f + 600.0f * rng.Uniform (), 10.0f, rate);
		v->modeB.BandPass (700.0f + 900.0f * rng.Uniform (), 14.0f, rate);
		v->ga = 12.0f, v->gb = 4.8f; // (normalized: a full-strength creak peaks near 1)
	} else {
		// a frame, a longeron: 0.5-1.5 s, slower (15-40 a second), lower
		// (100-250 Hz)
		v->len = 0.5f + 1.0f * rng.Uniform ();
		v->rate0 = 15.0f + 25.0f * rng.Uniform ();
		v->rate1 = v->rate0 * (0.8f + 0.5f * rng.Uniform ());
		v->modeA.BandPass (100.0f + 150.0f * rng.Uniform (), 8.0f, rate);
		v->modeB.BandPass (250.0f + 200.0f * rng.Uniform (), 10.0f, rate);
		v->ga = 31.0f, v->gb = 7.2f; // (normalized: a full-strength groan peaks near 1)
	}
}

void CreakSynth::Render (float *out, unsigned n, float dop)
{
	for (int k = 0; k < 2; k++) {
		int c = pending[k].count.load (std::memory_order_acquire);
		if (c == played[k]) continue;
		played[k] = c;
		Start ((Kind)k, pending[k].strength.load (std::memory_order_relaxed));
	}
	const float dt = 1.0f / rate;
	for (unsigned i = 0; i < n; i++) {
		float y = 0.0f;
		for (auto &v : voices) {
			if (!v.on) continue;
			// the slips: an impulse each, a little irregular, their rate gliding
			float x = 0.0f;
			v.next -= dt;
			if (v.next <= 0.0f) {
				const float u = v.t / v.len;
				const float r = v.rate0 + (v.rate1 - v.rate0) * u;
				v.next += (1.0f + 0.15f * rng.White ()) / std::max (r, 5.0f);
				x = 0.5f + 0.5f * rng.Uniform ();
			}
			// (its swell and fade: sticking in, letting go)
			const float u = v.t / v.len;
			const float env = std::max (0.0f, std::min (1.0f, u / 0.15f) * std::min (1.0f, (1.0f - u) / 0.25f));
			y += v.s * env * (v.ga * v.modeA.Process (x) + v.gb * v.modeB.Process (x)) * 6.0f;
			v.t += dt;
			if (v.t >= v.len + 0.1f) v.on = false;
		}
		out[i] = y;
	}
}

} // namespace audio
