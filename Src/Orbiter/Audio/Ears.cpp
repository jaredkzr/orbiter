// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Ears.h"
#include "miniaudio.h"
#include <algorithm>
#include <atomic>
#include <cmath>

namespace audio {

struct Ears::Impl {
	ma_node_base base; // (first: the graph calls back with its address)
	ma_node_vtable vtable;
	Settings set;
	float rate = 48000.0f;
	float env = 0.0f;       // detector: mean square
	float gainDb = 0.0f;    // the smoothed gain
	std::atomic<float> reduction{0.0f};
	bool init = false;
};

Ears::Ears () = default;

Ears::~Ears ()
{
	Uninit ();
}

float Ears::GainDb (float l, const Settings &s)
{
	if (l <= s.threshold) return 0.0f;
	return (s.threshold + (l - s.threshold) / s.ratio) - l;
}

bool Ears::Init (ma_engine *engine, const Settings &s)
{
	Uninit ();
	impl = std::make_unique<Impl>();
	impl->set = s;
	impl->rate = (float)ma_engine_get_sample_rate (engine);
	impl->vtable = {};
	impl->vtable.onProcess = [](ma_node *node, const float **in, ma_uint32 *nIn, float **out, ma_uint32 *nOut) {
		Impl *m = static_cast<Impl*>(node);
		const unsigned n = *nOut;
		*nIn = n;
		const float *x = in[0];
		float *y = out[0];
		const Settings &s = m->set;
		// the detector: a mean square over 50 ms
		const float kEnv = 1.0f - std::exp (-1.0f / (0.05f * m->rate));
		const float kAtt = 1.0f - std::exp (-1.0f / (s.attack * m->rate));
		const float kRel = 1.0f - std::exp (-1.0f / (s.release * m->rate));
		const float knee = 0.6f * s.ceiling, span = s.ceiling - knee;
		float env = m->env, g = m->gainDb;
		for (unsigned i = 0; i < n; i++) {
			float l = x[2*i], r = x[2*i+1];
			float ms = 0.5f * (l * l + r * r);
			env += kEnv * (ms - env);
			// (an RMS level of a sine: +3 dB for its peak)
			float lev = 10.0f * std::log10 (env + 1e-20f) + 3.0f;
			float tg = GainDb (lev, s);
			g += (tg < g ? kAtt : kRel) * (tg - g);
			float k = std::pow (10.0f, g * 0.05f);
			for (int c = 0; c < 2; c++) {
				float v = x[2*i+c] * k;
				// a soft limit on the peaks
				float a = std::fabs (v);
				if (a > knee) {
					float over = (a - knee) / span;
					a = knee + span * over / std::sqrt (1.0f + over * over);
					v = v < 0.0f ? -a : a;
				}
				y[2*i+c] = v;
			}
		}
		m->env = env, m->gainDb = g;
		m->reduction.store (g, std::memory_order_relaxed);
	};
	impl->vtable.inputBusCount = 1;
	impl->vtable.outputBusCount = 1;
	impl->vtable.flags = MA_NODE_FLAG_CONTINUOUS_PROCESSING;
	ma_uint32 ch[1] = { 2 };
	ma_node_config cfg = ma_node_config_init ();
	cfg.vtable = &impl->vtable;
	cfg.pInputChannels = ch;
	cfg.pOutputChannels = ch;
	if (ma_node_init (ma_engine_get_node_graph (engine), &cfg, NULL, &impl->base) != MA_SUCCESS) {
		impl.reset ();
		return false;
	}
	impl->init = true;
	ma_node_attach_output_bus (&impl->base, 0, ma_engine_get_endpoint (engine), 0);
	return true;
}

void Ears::Uninit ()
{
	if (impl && impl->init) ma_node_uninit (&impl->base, NULL);
	impl.reset ();
}

void *Ears::Node ()
{
	return impl && impl->init ? &impl->base : nullptr;
}

float Ears::Reduction () const
{
	return impl ? impl->reduction.load (std::memory_order_relaxed) : 0.0f;
}

} // namespace audio
