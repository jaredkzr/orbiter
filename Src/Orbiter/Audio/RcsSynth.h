// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Attitude thrusters: a cluster of small engines firing in pulses. Each
// pulse starts with a bang - a deep, short thump falling in pitch and a low
// whump, carried by the structure (astronauts likened them to cannon shots),
// a low snap of the valve at its start - and roars low while it fires; it
// ends with a softer thud.
// =======================================================================

#ifndef __RCSSYNTH_H
#define __RCSSYNTH_H

#include "Synth.h"

namespace audio {

class RcsSynth : public SynthSource {
public:
	static std::unique_ptr<RcsSynth> Make (Engine &e, uint32_t seed);
	/// The cluster's level (0..1: the strongest of its thrusters), and its
	/// size (the roar's peak, Hz)
	void Set (float level, float peak);

protected:
	void Render (float *out, unsigned n, float dop) override;

private:
	explicit RcsSynth (uint32_t seed): rng (seed) {}
	std::atomic<float> pLevel{0.0f}, pPeak{800.0f};
	dsp::Rng rng;
	dsp::Pink pink;
	dsp::Biquad roarBp, hissHp, clickBp, whumpLp;
	dsp::Smooth level;
	float bang = 0.0f, bangT = 0.0f, bangPh = 0.0f, click = 0.0f, whump = 0.0f;
	bool on = false;
	float offTime = 1.0f; // [s] since the last pulse ended
};

} // namespace audio

#endif // !__RCSSYNTH_H
