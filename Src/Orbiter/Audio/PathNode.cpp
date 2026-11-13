// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "PathNode.h"
#include "Hrtf.h"
#include "miniaudio.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace audio {

namespace {
	constexpr float PI_F = 3.14159265358979f;
	/// The low-pass cutoff eases with this time constant [s]
	constexpr float CUTOFF_TAU = 0.03f;
	/// Blocks up to this many frames without allocating on the audio thread
	constexpr unsigned MAX_BLOCK = 16384;
	constexpr int TAPS = Hrtf::TAPS;
	/// A new direction's filters when it moves this far (cosine: about 1 degree)
	constexpr float HRTF_MOVE = 0.99985f;

	std::atomic<const Hrtf*> g_hrtf{nullptr};

	/// out[i] = sum over k of h[k] x[i - d - k] (x: the block's start, the
	/// past before it); added along each tap (vectorizes)
	void Convolve (const float *x, unsigned n, const float *h, int d, float *out)
	{
		std::fill (out, out + n, 0.0f);
		for (int k = 0; k < TAPS; k++) {
			const float hk = h[k];
			if (hk == 0.0f) continue;
			const float *src = x - d - k;
			for (unsigned i = 0; i < n; i++) out[i] += hk * src[i];
		}
	}
}

void PathNode::SetHrtf (const Hrtf *h)
{
	g_hrtf.store (h, std::memory_order_release);
}

const Hrtf *PathNode::GetHrtf ()
{
	return g_hrtf.load (std::memory_order_acquire);
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

bool PathNode::Init (ma_engine *engine, float dcBlock)
{
	dcHz = dcBlock;
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
	dcBuf.assign (MAX_BLOCK, 0.0f); // (no allocation on the audio thread, for blocks up to this)
	// (the filters' reach: their taps and the longest delay)
	pastLen = TAPS + (unsigned)std::ceil (Hrtf::MAX_DELAY * rate) + 4;
	for (int w = 0; w < NWAY; w++) way[w].past.assign (pastLen + MAX_BLOCK, 0.0f), way[w].binaural = false;
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
	for (int i = 0; i < 3; i++) way[w].dir[i].store (t.dir[i], std::memory_order_relaxed);
}

void PathNode::SetRunning (bool on)
{
	if (valid) ma_node_set_state (&impl->base, on ? ma_node_state_started : ma_node_state_stopped);
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
	// the input without its DC and infrasound (synthesized noise drifts there)
	float *xb = self->dcBuf.size () >= n ? self->dcBuf.data () : (self->dcBuf.resize (n), self->dcBuf.data ());
	if (self->dcHz > 0.0f) {
		const float a = 1.0f - std::exp (-2.0f * PI_F * self->dcHz / rate);
		float lp = self->dcLp;
		for (unsigned i = 0; i < n; i++) lp += a * (x[i] - lp), xb[i] = x[i] - lp;
		self->dcLp = std::fabs (lp) < 1e-20f ? 0.0f : lp;
		x = xb;
	}
	// (the cutoff's easing over this block)
	const float k = 1.0f - std::exp (-(float)n / (CUTOFF_TAU * rate));

	for (int w = 0; w < NWAY; w++) {
		WayState &s = self->way[w];
		float *o = out[w];
		float g1 = s.gain.load (std::memory_order_relaxed);
		float p1 = s.pan.load (std::memory_order_relaxed);
		float fc1 = s.cutoff.load (std::memory_order_relaxed);
		if (hardCut) s.g = 0.0f, s.y = s.y2 = 0.0f;
		float g0 = s.g, p0 = s.p;
		// (the cutoff eases in the log: an octave is an octave)
		s.fc = std::exp (std::log (s.fc) + (std::log (fc1) - std::log (s.fc)) * k);
		// (two one-poles, each at 1.554 fc: -3 dB at fc, 12 dB/octave above)
		float a = 1.0f - std::exp (-2.0f * PI_F * std::min (1.554f * s.fc, 0.45f * rate) / rate);
		// equal-power panning, at the block's ends
		float l0 = std::cos ((p0 + 1.0f) * 0.25f * PI_F), r0 = std::sin ((p0 + 1.0f) * 0.25f * PI_F);
		float l1 = std::cos ((p1 + 1.0f) * 0.25f * PI_F), r1 = std::sin ((p1 + 1.0f) * 0.25f * PI_F);
		// binaural: the HRTFs loaded, a direction given (and within reach)
		const Hrtf *hrtf = g_hrtf.load (std::memory_order_acquire);
		float d[3] = { s.dir[0].load (std::memory_order_relaxed), s.dir[1].load (std::memory_order_relaxed), s.dir[2].load (std::memory_order_relaxed) };
		const float dn = std::sqrt (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		const bool bin = hrtf && hrtf->Loaded () && hrtf->Rate () == self->rate && dn > 1e-6f
			&& hrtf->MaxDelay () + TAPS + 1 <= (int)self->pastLen && n <= MAX_BLOCK;
		if (g0 == 0.0f && g1 == 0.0f) {
			// silent: only the filter follows the input
			float y = s.y, y2 = s.y2;
			for (unsigned i = 0; i < n; i++) y += a * (x[i] - y), y2 += a * (y - y2);
			s.y = std::fabs (y) < 1e-20f ? 0.0f : y;
			s.y2 = std::fabs (y2) < 1e-20f ? 0.0f : y2;
			std::fill (o, o + 2 * n, 0.0f);
			s.binaural = false; // (afresh when heard again)
		} else if (bin) {
			// the filtered signal after its past
			const unsigned P = self->pastLen;
			float *e = s.past.data ();
			if (!s.binaural || hardCut) std::fill (e, e + P, 0.0f);
			float y = s.y, y2 = s.y2;
			for (unsigned i = 0; i < n; i++) {
				y += a * (x[i] - y), y2 += a * (y - y2);
				e[P + i] = y2;
			}
			s.y = std::fabs (y) < 1e-20f ? 0.0f : y;
			s.y2 = std::fabs (y2) < 1e-20f ? 0.0f : y2;
			// new filters if it moved (or is new): the last block's crossfaded out
			for (int i = 0; i < 3; i++) d[i] /= dn;
			const bool fresh = !s.binaural;
			const bool moved = fresh || d[0] * s.u[0] + d[1] * s.u[1] + d[2] * s.u[2] < HRTF_MOVE;
			static thread_local std::vector<float> scratch (4 * MAX_BLOCK);
			float *L1 = scratch.data (), *R1 = L1 + MAX_BLOCK, *L0 = R1 + MAX_BLOCK, *R0 = L0 + MAX_BLOCK;
			float ol[TAPS], orr[TAPS];
			int odl = s.dl, odr = s.dr;
			if (moved) {
				memcpy (ol, s.hl, sizeof (ol)), memcpy (orr, s.hr, sizeof (orr));
				float hl[TAPS], hr[TAPS], fdl, fdr;
				hrtf->Get (d[0], d[1], d[2], hl, hr, fdl, fdr);
				// (each delay's fraction folded into its filter: a linear step)
				auto fold = [] (const float *h, float dly, float *out, int &whole) {
					whole = (int)std::floor (dly);
					const float f = dly - whole;
					out[0] = (1.0f - f) * h[0];
					for (int k = 1; k < TAPS; k++) out[k] = (1.0f - f) * h[k] + f * h[k - 1];
				};
				fold (hl, fdl, s.hl, s.dl);
				fold (hr, fdr, s.hr, s.dr);
				memcpy (s.u, d, sizeof (s.u));
			}
			const float *xs = e + P;
			Convolve (xs, n, s.hl, s.dl, L1);
			Convolve (xs, n, s.hr, s.dr, R1);
			const float inv = 1.0f / (float)n;
			if (moved && !fresh) {
				Convolve (xs, n, ol, odl, L0);
				Convolve (xs, n, orr, odr, R0);
				for (unsigned i = 0; i < n; i++) {
					const float w = (i + 1) * inv, g = g0 + (g1 - g0) * w;
					o[2*i] = g * (L0[i] + w * (L1[i] - L0[i]));
					o[2*i+1] = g * (R0[i] + w * (R1[i] - R0[i]));
				}
			} else {
				for (unsigned i = 0; i < n; i++) {
					const float g = g0 + (g1 - g0) * (i + 1) * inv;
					o[2*i] = g * L1[i], o[2*i+1] = g * R1[i];
				}
			}
			// (the past for the next block)
			memmove (e, e + n, P * sizeof (float));
			s.binaural = true;
		} else {
			float y = s.y, y2 = s.y2, inv = 1.0f / (float)n;
			float dl = (g1 * l1 - g0 * l0) * inv, dr = (g1 * r1 - g0 * r0) * inv;
			float gl = g0 * l0, gr = g0 * r0;
			for (unsigned i = 0; i < n; i++) {
				y += a * (x[i] - y);
				y2 += a * (y - y2);
				gl += dl, gr += dr;
				o[2*i] = y2 * gl;
				o[2*i+1] = y2 * gr;
			}
			s.y = std::fabs (y) < 1e-20f ? 0.0f : y;
			s.y2 = std::fabs (y2) < 1e-20f ? 0.0f : y2;
			s.binaural = false;
		}
		s.g = g1, s.p = p1;
		s.cur.gain.store (g1, std::memory_order_relaxed);
	}
}

} // namespace audio
