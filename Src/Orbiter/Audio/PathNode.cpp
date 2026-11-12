// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "PathNode.h"
#include "miniaudio.h"
#include <algorithm>
#include <cmath>

namespace audio {

namespace {
	constexpr float PI_F = 3.14159265358979f;
	/// The low-pass cutoff eases with this time constant [s]
	constexpr float CUTOFF_TAU = 0.03f;
}

struct PathNode::Impl {
	ma_node_base base; // (first: the graph calls back with its address)
	PathNode *owner;
	ma_node_vtable vtable;
};

PathNode::PathNode () = default;

PathNode::~PathNode ()
{
	Uninit ();
}

void *PathNode::Node ()
{
	return valid ? &impl->base : nullptr;
}

bool PathNode::Init (ma_engine *engine)
{
	Uninit ();
	if (!engine) return false;
	impl = std::make_unique<Impl>();
	impl->owner = this;
	impl->vtable = {};
	impl->vtable.onProcess = [](ma_node *node, const float **in, ma_uint32 *nIn, float **out, ma_uint32 *nOut) {
		Process (node, in, nIn, out, nOut);
	};
	impl->vtable.inputBusCount = 1;
	impl->vtable.outputBusCount = NWAY;
	impl->vtable.flags = MA_NODE_FLAG_CONTINUOUS_PROCESSING; // (the low-pass rings out)

	ma_uint32 inCh[1] = { 1 };
	ma_uint32 outCh[NWAY] = { 2, 2 };
	ma_node_config cfg = ma_node_config_init ();
	cfg.vtable = &impl->vtable;
	cfg.pInputChannels = inCh;
	cfg.pOutputChannels = outCh;
	rate = ma_engine_get_sample_rate (engine);
	if (ma_node_init (ma_engine_get_node_graph (engine), &cfg, NULL, &impl->base) != MA_SUCCESS) {
		impl.reset ();
		return false;
	}
	valid = true;
	return true;
}

void PathNode::Uninit ()
{
	if (!valid) return;
	ma_node_uninit (&impl->base, NULL);
	impl.reset ();
	valid = false;
}

void PathNode::Set (Way w, const Target &t)
{
	way[w].gain.store (std::max (0.0f, t.gain), std::memory_order_relaxed);
	way[w].cutoff.store (std::clamp (t.cutoff, 10.0f, 20000.0f), std::memory_order_relaxed);
	way[w].pan.store (std::clamp (t.pan, -1.0f, 1.0f), std::memory_order_relaxed);
}

void PathNode::Cut ()
{
	for (int w = 0; w < NWAY; w++) way[w].gain.store (0.0f, std::memory_order_relaxed);
	cut.store (true, std::memory_order_release);
}

void PathNode::Process (void *node, const float **in, unsigned *nIn, float **out, unsigned *nOut)
{
	PathNode *self = static_cast<Impl*>(node)->owner;
	const unsigned n = *nOut;
	const float *x = in[0];
	*nIn = n;
	const bool hardCut = self->cut.exchange (false, std::memory_order_acquire);
	const float rate = (float)self->rate;
	// (the cutoff's easing over this block)
	const float k = 1.0f - std::exp (-(float)n / (CUTOFF_TAU * rate));

	for (int w = 0; w < NWAY; w++) {
		WayState &s = self->way[w];
		float *o = out[w];
		float g1 = s.gain.load (std::memory_order_relaxed);
		float p1 = s.pan.load (std::memory_order_relaxed);
		float fc1 = s.cutoff.load (std::memory_order_relaxed);
		if (hardCut) s.g = 0.0f, s.y = 0.0f;
		float g0 = s.g, p0 = s.p;
		// (the cutoff eases in the log: an octave is an octave)
		s.fc = std::exp (std::log (s.fc) + (std::log (fc1) - std::log (s.fc)) * k);
		float a = 1.0f - std::exp (-2.0f * PI_F * s.fc / rate);
		// equal-power panning, at the block's ends
		float l0 = std::cos ((p0 + 1.0f) * 0.25f * PI_F), r0 = std::sin ((p0 + 1.0f) * 0.25f * PI_F);
		float l1 = std::cos ((p1 + 1.0f) * 0.25f * PI_F), r1 = std::sin ((p1 + 1.0f) * 0.25f * PI_F);
		if (g0 == 0.0f && g1 == 0.0f) {
			// silent: only the filter follows the input
			float y = s.y;
			for (unsigned i = 0; i < n; i++) y += a * (x[i] - y);
			s.y = std::fabs (y) < 1e-20f ? 0.0f : y;
			std::fill (o, o + 2 * n, 0.0f);
		} else {
			float y = s.y, inv = 1.0f / (float)n;
			float dl = (g1 * l1 - g0 * l0) * inv, dr = (g1 * r1 - g0 * r0) * inv;
			float gl = g0 * l0, gr = g0 * r0;
			for (unsigned i = 0; i < n; i++) {
				y += a * (x[i] - y);
				gl += dl, gr += dr;
				o[2*i] = y * gl;
				o[2*i+1] = y * gr;
			}
			s.y = std::fabs (y) < 1e-20f ? 0.0f : y;
		}
		s.g = g1, s.p = p1;
		s.cur.gain.store (g1, std::memory_order_relaxed);
	}
}

} // namespace audio
