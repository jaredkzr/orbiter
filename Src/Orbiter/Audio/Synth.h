// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Sound made as it plays: a node of the audio graph without inputs and one
// mono output (or more channels: the music), rendering block by block from
// parameters the simulation sets (atomics). The Doppler factor scales its
// frequencies (and the rates of its events). Building blocks: noise,
// filters, smoothing.
// =======================================================================

#ifndef __AUDIOSYNTH_H
#define __AUDIOSYNTH_H

#include "World.h"
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>

namespace audio {

class SynthSource : public Source {
public:
	SynthSource ();
	~SynthSource () override;

	/// Create the node in the engine's graph (call from the derived
	/// class's factory, after construction)
	bool Init (Engine &e);

	bool Attach (void *node) override;
	void Start () override;
	void Stop () override;
	void SetDoppler (float f) override { doppler.store (f, std::memory_order_relaxed); }

protected:
	/// Render n samples (audio thread); dop: the Doppler factor (scale
	/// frequencies and event rates with it)
	virtual void Render (float *out, unsigned n, float dop) = 0;
	float rate = 48000.0f;
	/// Its output's channels (set before Init; more than one: interleaved
	/// frames, attached to a node of as many - the music, on its bus)
	unsigned channels = 1;
	/// Take the node out of the graph (waits for the audio thread to leave
	/// it): from a derived class's destructor, before its members go
	void Release ();

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
	std::atomic<float> doppler{1.0f};
	friend struct Impl;
};

namespace dsp {

/// A fast random number generator (xorshift)
struct Rng {
	uint32_t s;
	explicit Rng (uint32_t seed = 0x9e3779b9u): s (seed ? seed : 1u) {}
	inline uint32_t Next () { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
	/// uniform in [-1, 1)
	inline float White () { return (float)(int32_t)Next () * (1.0f / 2147483648.0f); }
	/// uniform in [0, 1)
	inline float Uniform () { return (float)(Next () >> 8) * (1.0f / 16777216.0f); }
	/// standard normal (Box-Muller, one of the pair)
	float Gauss ();
};

/// Pink noise (-3 dB/octave; Paul Kellet's filter), about unit RMS
struct Pink {
	float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
	inline float Next (float w) {
		b0 = 0.99886f * b0 + w * 0.0555179f;
		b1 = 0.99332f * b1 + w * 0.0750759f;
		b2 = 0.96900f * b2 + w * 0.1538520f;
		b3 = 0.86650f * b3 + w * 0.3104856f;
		b4 = 0.55000f * b4 + w * 0.5329522f;
		b5 = -0.7616f * b5 - w * 0.0168980f;
		float p = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362f;
		b6 = w * 0.115926f;
		return p * 0.25f;
	}
};

/// One-pole low-pass
struct OnePole {
	float y = 0.0f, a = 1.0f;
	void SetCutoff (float fc, float rate) { a = 1.0f - std::exp (-2.0f * 3.14159265f * std::fmin (fc, 0.45f * rate) / rate); }
	inline float Lp (float x) { return y += a * (x - y); }
	inline float Hp (float x) { return x - Lp (x); }
};

/// A biquad (RBJ cookbook), transposed direct form II
struct Biquad {
	float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
	void LowPass (float fc, float q, float rate);
	void HighPass (float fc, float q, float rate);
	void BandPass (float fc, float q, float rate);   // (0 dB peak)
	inline float Process (float x) {
		float y = b0 * x + z1;
		z1 = b1 * x - a1 * y + z2;
		z2 = b2 * x - a2 * y;
		return y;
	}
};

/// Exponential smoothing towards a target with a time constant
struct Smooth {
	float v = 0.0f;
	inline float Step (float target, float k) { return v += k * (target - v); }
	/// the factor for a time constant tau [s] over n samples
	static float K (float tau, unsigned n, float rate) { return tau <= 0.0f ? 1.0f : 1.0f - std::exp (-(float)n / (tau * rate)); }
};

} // namespace dsp
} // namespace audio

#endif // !__AUDIOSYNTH_H
