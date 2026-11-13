// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Synth.h"
#include "miniaudio.h"
#include <algorithm>

namespace audio {

struct SynthSource::Impl {
	ma_node_base base; // (first: the graph calls back with its address)
	SynthSource *owner;
	ma_node_vtable vtable;
	bool init = false;
};

SynthSource::SynthSource () = default;

SynthSource::~SynthSource ()
{
	Release ();
}

void SynthSource::Release ()
{
	if (impl && impl->init) ma_node_uninit (&impl->base, NULL), impl->init = false;
}

bool SynthSource::Init (Engine &e)
{
	if (!e.Active ()) return false;
	rate = (float)e.SampleRate ();
	impl = std::make_unique<Impl>();
	impl->owner = this;
	impl->vtable = {};
	impl->vtable.onProcess = [](ma_node *node, const float **, ma_uint32 *, float **out, ma_uint32 *nOut) {
		SynthSource *s = static_cast<Impl*>(node)->owner;
		s->Render (out[0], *nOut, s->doppler.load (std::memory_order_relaxed));
	};
	impl->vtable.inputBusCount = 0;
	impl->vtable.outputBusCount = 1;
	ma_uint32 outCh[1] = { channels };
	ma_node_config cfg = ma_node_config_init ();
	cfg.vtable = &impl->vtable;
	cfg.pOutputChannels = outCh;
	cfg.initialState = ma_node_state_stopped;
	if (ma_node_init (ma_engine_get_node_graph (e.Ma ()), &cfg, NULL, &impl->base) != MA_SUCCESS) {
		impl.reset ();
		return false;
	}
	impl->init = true;
	return true;
}

bool SynthSource::Attach (void *node)
{
	return impl && impl->init && ma_node_attach_output_bus (&impl->base, 0, node, 0) == MA_SUCCESS;
}

void SynthSource::Start ()
{
	if (impl && impl->init) ma_node_set_state (&impl->base, ma_node_state_started);
}

void SynthSource::Stop ()
{
	if (impl && impl->init) ma_node_set_state (&impl->base, ma_node_state_stopped);
}

namespace dsp {

float Rng::Gauss ()
{
	float u1 = std::max (Uniform (), 1e-7f), u2 = Uniform ();
	return std::sqrt (-2.0f * std::log (u1)) * std::cos (6.2831853f * u2);
}

namespace {
	struct Rbj { float w0, cw, sw, alpha; };
	Rbj Prep (float fc, float q, float rate)
	{
		fc = std::clamp (fc, 5.0f, 0.45f * rate);
		Rbj r;
		r.w0 = 6.2831853f * fc / rate;
		r.cw = std::cos (r.w0), r.sw = std::sin (r.w0);
		r.alpha = r.sw / (2.0f * std::max (q, 0.05f));
		return r;
	}
}

void Biquad::LowPass (float fc, float q, float rate)
{
	Rbj r = Prep (fc, q, rate);
	float a0 = 1.0f + r.alpha;
	b0 = (1.0f - r.cw) * 0.5f / a0, b1 = (1.0f - r.cw) / a0, b2 = b0;
	a1 = -2.0f * r.cw / a0, a2 = (1.0f - r.alpha) / a0;
}

void Biquad::HighPass (float fc, float q, float rate)
{
	Rbj r = Prep (fc, q, rate);
	float a0 = 1.0f + r.alpha;
	b0 = (1.0f + r.cw) * 0.5f / a0, b1 = -(1.0f + r.cw) / a0, b2 = b0;
	a1 = -2.0f * r.cw / a0, a2 = (1.0f - r.alpha) / a0;
}

void Biquad::BandPass (float fc, float q, float rate)
{
	Rbj r = Prep (fc, q, rate);
	float a0 = 1.0f + r.alpha;
	b0 = r.alpha / a0, b1 = 0.0f, b2 = -r.alpha / a0;
	a1 = -2.0f * r.cw / a0, a2 = (1.0f - r.alpha) / a0;
}

} // namespace dsp
} // namespace audio
