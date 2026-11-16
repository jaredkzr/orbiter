// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Ears.h"
#include "miniaudio.h"
#include <algorithm>
#include <atomic>
#include <cmath>

namespace audio {

namespace {
	/// The peak limiter's look-ahead [samples]: 2 ms at 48 kHz
	constexpr int LOOK = 96;
	/// The listening level's recovery: its scene level falling back [dB/s]
	constexpr float ADAPT_FALL = 4.0f;
}

struct Ears::Impl {
	ma_node_base base; // (first: the graph calls back with its address)
	ma_node_vtable vtable;
	Settings set;
	float rate = 48000.0f;
	float env = 0.0f;       // detector: mean square
	float gainDb = 0.0f;    // the smoothed gain
	std::atomic<float> reduction{0.0f}, adaptGain{0.0f};
	std::atomic<bool> adaptOn{false};
	// (live: the compression's settings, set from the simulation thread)
	std::atomic<float> thr{-20.0f}, ratio{3.0f}, aMax{45.0f}, aTarget{-24.0f};
	float msFast = 0.0f, held = -200.0f, adaptDb = 0.0f;
	bool init = false;
	// the peak limiter: the gain each sample needs (its window's minimum over
	// the look-ahead), averaged over the look-ahead (so the gain is down,
	// smoothly, by the time the peak plays), the sound delayed to match
	float need[LOOK] = {}, mins[LOOK] = {}, delay[LOOK][2] = {};
	float minSum = (float)LOOK, limG = 1.0f;
	int pos = 0;
	Impl () { for (int i = 0; i < LOOK; i++) need[i] = mins[i] = 1.0f; }
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
	impl->adaptOn.store (s.adapt);
	impl->adaptDb = s.adapt ? s.adaptMax : 0.0f; // (starting adapted to quiet)
	impl->thr.store (s.threshold), impl->ratio.store (s.ratio), impl->aMax.store (s.adaptMax), impl->aTarget.store (s.adaptTarget);
	impl->rate = (float)ma_engine_get_sample_rate (engine);
	impl->vtable = {};
	impl->vtable.onProcess = [](ma_node *node, const float **in, ma_uint32 *nIn, float **out, ma_uint32 *nOut) {
		Impl *m = static_cast<Impl*>(node);
		const unsigned n = *nOut;
		*nIn = n;
		const float *x = in[0];
		float *y = out[0];
		Settings s = m->set;
		s.threshold = m->thr.load (std::memory_order_relaxed), s.ratio = m->ratio.load (std::memory_order_relaxed);
		s.adaptMax = m->aMax.load (std::memory_order_relaxed), s.adaptTarget = m->aTarget.load (std::memory_order_relaxed);
		// the detector: a mean square over 50 ms
		const float kEnv = 1.0f - std::exp (-1.0f / (0.05f * m->rate));
		const float kAtt = 1.0f - std::exp (-1.0f / (s.attack * m->rate));
		const float kRel = 1.0f - std::exp (-1.0f / (s.release * m->rate));
		const float kFast = 1.0f - std::exp (-1.0f / (0.1f * m->rate)), fall = ADAPT_FALL / m->rate;
		const float kDown = 1.0f - std::exp (-1.0f / (s.adaptDown * m->rate)), kUp = 1.0f - std::exp (-1.0f / (s.adaptUp * m->rate));
		const bool adaptOn = m->adaptOn.load (std::memory_order_relaxed);
		// the limiter recovers in 60 ms; a last soft clip only above 95 % of the ceiling
		const float kLimRel = 1.0f - std::exp (-1.0f / (0.06f * m->rate));
		const float knee = 0.95f * s.ceiling, span = s.ceiling - knee;
		float env = m->env, g = m->gainDb;
		for (unsigned i = 0; i < n; i++) {
			float l = x[2*i], r = x[2*i+1];
			if (adaptOn) {
				// the scene's level: its mean square over 0.1 s, held - rising
				// with it at once, falling back at ADAPT_FALL dB/s (the ears
				// recovering: after something loud, seconds as it was loud); the
				// gain to bring it to the target, smoothed
				const float m0 = 0.5f * (l * l + r * r);
				m->msFast += kFast * (m0 - m->msFast);
				const float lev = 10.0f * std::log10 (m->msFast + 1e-20f);
				m->held = std::max (lev, m->held - fall);
				const float want = std::clamp (s.adaptTarget - m->held, 0.0f, s.adaptMax);
				m->adaptDb += (want < m->adaptDb ? kDown : kUp) * (want - m->adaptDb);
				const float a = std::pow (10.0f, m->adaptDb * 0.05f);
				l *= a, r *= a;
			}
			float ms = 0.5f * (l * l + r * r);
			env += kEnv * (ms - env);
			// (an RMS level of a sine: +3 dB for its peak)
			float lev = 10.0f * std::log10 (env + 1e-20f) + 3.0f;
			float tg = GainDb (lev, s);
			g += (tg < g ? kAtt : kRel) * (tg - g);
			float k = std::pow (10.0f, g * 0.05f);
			float cl = l * k, cr = r * k;
			// the peak limiter
			float a = std::max (std::fabs (cl), std::fabs (cr));
			int p = m->pos;
			const float target = 0.92f * s.ceiling; // (headroom below the last soft clip)
			m->need[p] = a > target ? target / a : 1.0f;
			float mn = 1.0f;
			for (int j = 0; j < LOOK; j++) mn = std::min (mn, m->need[j]);
			m->minSum += mn - m->mins[p];
			m->mins[p] = mn;
			float box = m->minSum * (1.0f / LOOK);
			m->limG = box < m->limG ? box : m->limG + kLimRel * (box - m->limG);
			// out: the sample from LOOK - 1 samples ago, at the limiter's gain
			int q = (p + 1) % LOOK;
			float ol = m->delay[q][0] * m->limG, orr = m->delay[q][1] * m->limG;
			m->delay[p][0] = cl, m->delay[p][1] = cr;
			m->pos = q;
			for (int c = 0; c < 2; c++) {
				float v = c ? orr : ol;
				float av = std::fabs (v);
				if (av > knee) {
					float over = (av - knee) / span;
					av = knee + span * over / std::sqrt (1.0f + over * over);
					v = v < 0.0f ? -av : av;
				}
				y[2*i+c] = v;
			}
		}
		// (the running sum drifts with rounding: recomputed now and then)
		if ((m->pos & 63) == 0) { float sum = 0.0f; for (int j = 0; j < LOOK; j++) sum += m->mins[j]; m->minSum = sum; }
		m->env = env, m->gainDb = g;
		m->reduction.store (g, std::memory_order_relaxed);
		m->adaptGain.store (adaptOn ? m->adaptDb : 0.0f, std::memory_order_relaxed);
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

float Ears::AdaptGain () const
{
	return impl ? impl->adaptGain.load (std::memory_order_relaxed) : 0.0f;
}

Ears::Settings Ears::ForCompression (int level)
{
	Settings s;
	switch (level) {
	case COMP_OFF:    s.ratio = 1.0f, s.adaptMax = 0.0f; break;                                 // (the limiter only)
	case COMP_LIGHT:  s.threshold = -15.0f, s.ratio = 2.0f, s.adaptMax = 25.0f, s.adaptTarget = -28.0f; break;
	case COMP_STRONG: s.threshold = -30.0f, s.ratio = 6.0f, s.adaptMax = 55.0f, s.adaptTarget = -20.0f; break;
	default: break;                                                                              // (medium: the defaults)
	}
	return s;
}

void Ears::SetCompression (int level)
{
	if (!impl) return;
	const Settings s = ForCompression (level);
	impl->thr.store (s.threshold, std::memory_order_relaxed), impl->ratio.store (s.ratio, std::memory_order_relaxed);
	impl->aMax.store (s.adaptMax, std::memory_order_relaxed), impl->aTarget.store (s.adaptTarget, std::memory_order_relaxed);
}

void Ears::SetAdapt (bool on)
{
	if (impl) impl->adaptOn.store (on, std::memory_order_relaxed);
}

float Ears::Reduction () const
{
	return impl ? impl->reduction.load (std::memory_order_relaxed) : 0.0f;
}

} // namespace audio
