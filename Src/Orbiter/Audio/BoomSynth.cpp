// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "BoomSynth.h"
#include <algorithm>
#include <cmath>

namespace audio {

std::unique_ptr<BoomSynth> BoomSynth::Make (Engine &e, float duration, uint32_t seed)
{
	std::unique_ptr<BoomSynth> s (new BoomSynth (std::clamp (duration, 0.02f, 0.6f), seed));
	if (!s->Init (e)) return nullptr;
	return s;
}

void BoomSynth::Strength (double l, double M, double h, double pv, double pg, double av, double &dp, double &T)
{
	const double Kr = 2.0, Ks = 0.08;
	double m2 = std::max (M * M - 1.0, 1e-3);
	h = std::max (h, 10.0), l = std::max (l, 1.0);
	dp = Kr * std::sqrt (std::max (pv, 0.0) * std::max (pg, 0.0)) * std::pow (m2, 0.125) * std::pow (h, -0.75) * std::pow (l, 0.75) * Ks;
	// (the duration's factor fitted to Concorde's measured 0.3 s)
	const double Kt = 0.3;
	T = Kt * 3.42 / std::max (av, 1.0) * M / std::pow (m2, 0.375) * std::pow (h, 0.25) * std::pow (l, 0.75) * std::sqrt (Ks);
}

void BoomSynth::Render (float *out, unsigned n, float dop)
{
	if (!setup) {
		rumbleLp.LowPass (60.0f, 0.7f, rate);
		edge.SetCutoff (250.0f, rate); // (rise times of a few milliseconds)
		setup = true;
	}
	const unsigned nN = (unsigned)(dur * rate);
	const float tailTau = 0.45f;
	for (unsigned i = 0; i < n; i++, pos++) {
		float t = (float)pos / rate;
		// the N-wave: up, a linear fall through zero, back
		float nw = pos < nN ? 1.0f - 2.0f * (float)pos / (float)nN : 0.0f;
		float s = edge.Lp (nw);
		// the rumble: echoes, building up in the first 0.1 s, dying away
		float env = (1.0f - std::exp (-t / 0.05f)) * std::exp (-t / tailTau);
		s += 0.8f * env * rumbleLp.Process (pink.Next (rng.White ()));
		out[i] = s;
	}
}

} // namespace audio
